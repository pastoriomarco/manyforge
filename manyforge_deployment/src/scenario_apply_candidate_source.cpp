#include "manyforge_deployment/scenario_apply_candidate_source.hpp"

#include "content_identity_internal.hpp"
#include "posix_durability_internal.hpp"

#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/scenario_catalog.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <optional>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace manyforge::deployment {
namespace {

namespace core = manyforge::core;

const detail::ContentIdentitySpec kScenarioCatalogIdentitySpec{
    .identity_domain = "scenario-catalog-revision",
    .api_version = std::string(kScenarioCatalogApiVersion),
    .own_field = "scenario_catalog_revision",
};

[[noreturn]] void Fail(const std::string &detail) {
  throw ScenarioApplyCandidateError(detail);
}

void Require(const bool condition, const std::string &detail) {
  if (!condition) {
    Fail(detail);
  }
}

[[nodiscard]] bool IsIdentifier(const std::string_view value) noexcept {
  if (value.empty() || value.size() > 128U || value.front() < 'a' ||
      value.front() > 'z') {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [](const char item) {
    return (item >= 'a' && item <= 'z') || (item >= '0' && item <= '9') ||
           item == '_' || item == '.' || item == '-';
  });
}

[[nodiscard]] std::size_t
Utf8CodepointCount(const std::string_view value) noexcept {
  return static_cast<std::size_t>(
      std::count_if(value.begin(), value.end(), [](const unsigned char item) {
        return (item & 0xc0U) != 0x80U;
      }));
}

[[nodiscard]] const CanonicalValue &Member(const CanonicalValue &value,
                                           const std::string_view name,
                                           const std::string_view path) {
  const auto *result = value.find_object_member(name);
  Require(result != nullptr,
          std::string(path) + " is missing " + std::string(name));
  return *result;
}

[[nodiscard]] const std::string &Text(const CanonicalValue &value,
                                      const std::string_view path) {
  const auto *result = std::get_if<std::string>(&value.storage());
  Require(result != nullptr, std::string(path) + " must be text");
  return *result;
}

[[nodiscard]] const CanonicalValue::Array &Array(const CanonicalValue &value,
                                                 const std::string_view path) {
  const auto *result = std::get_if<CanonicalValue::Array>(&value.storage());
  Require(result != nullptr, std::string(path) + " must be an array");
  return *result;
}

[[nodiscard]] const CanonicalValue::Object &
Object(const CanonicalValue &value, const std::string_view path) {
  const auto *result = std::get_if<CanonicalValue::Object>(&value.storage());
  Require(result != nullptr, std::string(path) + " must be an object");
  return *result;
}

void ExactMembers(const CanonicalValue &value,
                  const std::set<std::string, std::less<>> &required,
                  const std::set<std::string, std::less<>> &optional,
                  const std::string_view path) {
  const auto &object = Object(value, path);
  for (const auto &member : required) {
    static_cast<void>(Member(value, member, path));
  }
  for (const auto &[name, ignored] : object) {
    static_cast<void>(ignored);
    Require(required.contains(name) || optional.contains(name),
            std::string(path) + " has unknown field " + name);
  }
}

[[nodiscard]] ScenarioDeploymentCompatibilityDTO
DeploymentCompatibility(const CanonicalValue &row,
                        const std::string_view path) {
  const auto compatibility_path =
      std::string(path) + ".deployment_compatibility";
  const auto &compatibility = Member(row, "deployment_compatibility", path);
  ExactMembers(compatibility, {"lane", "provides"}, {}, compatibility_path);
  ScenarioDeploymentCompatibilityDTO result{
      .lane = Text(Member(compatibility, "lane", compatibility_path),
                   compatibility_path + ".lane"),
      .provides = {},
  };
  Require(result.lane == "manyforge-only" || result.lane == "isaac-sim" ||
              result.lane == "real-robot",
          "scenario Apply deployment lane is unsupported");
  const auto &provides =
      Array(Member(compatibility, "provides", compatibility_path),
            compatibility_path + ".provides");
  Require(provides.size() <= kMaxScenarioDeploymentProvides,
          "scenario Apply deployment provides exceed their bound");
  result.provides.reserve(provides.size());
  for (std::size_t index = 0U; index < provides.size(); ++index) {
    const auto &item =
        Text(provides[index], compatibility_path + ".provides[]");
    Require(IsIdentifier(item),
            "scenario Apply deployment provides contain an invalid ID");
    result.provides.push_back(item);
  }
  Require(
      std::is_sorted(result.provides.begin(), result.provides.end()) &&
          std::adjacent_find(result.provides.begin(), result.provides.end()) ==
              result.provides.end(),
      "scenario Apply deployment provides are not canonical");
  return result;
}

[[nodiscard]] std::filesystem::path ExistingRoot(std::filesystem::path root) {
  const detail::posix_durability::ErrorPolicy errors{
      [](const std::string_view message) {
        return std::make_exception_ptr(
            ScenarioApplyCandidateError(std::string(message)));
      }};
  constexpr detail::posix_durability::TrustedRootMessages messages{
      "scenario Apply package root is empty",
      "scenario Apply package root cannot be resolved",
      "scenario Apply package root must be a plain directory",
  };
  try {
    return detail::posix_durability::normalize_existing_plain_root(
        std::move(root), errors, messages);
  } catch (const ScenarioApplyCandidateError &) {
    throw;
  } catch (const std::exception &error) {
    throw ScenarioApplyCandidateError(
        "scenario Apply package root is unavailable: " +
        std::string(error.what()));
  }
}

class FileDescriptor final {
public:
  explicit FileDescriptor(const int value) noexcept : value_(value) {}
  ~FileDescriptor() {
    if (value_ >= 0) {
      static_cast<void>(::close(value_));
    }
  }
  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;

  [[nodiscard]] int get() const noexcept { return value_; }

private:
  int value_;
};

[[nodiscard]] bool SameSnapshot(const struct stat &left,
                                const struct stat &right) noexcept {
  return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
         left.st_size == right.st_size &&
         left.st_mtim.tv_sec == right.st_mtim.tv_sec &&
         left.st_mtim.tv_nsec == right.st_mtim.tv_nsec &&
         left.st_ctim.tv_sec == right.st_ctim.tv_sec &&
         left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
}

[[nodiscard]] std::string ReadExactArtifact(const std::filesystem::path &path,
                                            const std::string_view label) {
  const FileDescriptor descriptor(
      ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  Require(descriptor.get() >= 0, std::string(label) + " cannot be opened: " +
                                     std::string(std::strerror(errno)));
  struct stat before {};
  Require(::fstat(descriptor.get(), &before) == 0 && S_ISREG(before.st_mode) &&
              before.st_size >= 0 &&
              static_cast<std::uint64_t>(before.st_size) <=
                  kMaxArtifactEncodedBytes,
          std::string(label) + " is not one bounded regular file");
  std::string bytes(static_cast<std::size_t>(before.st_size), '\0');
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const auto count =
        ::read(descriptor.get(), bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    Require(count > 0, std::string(label) + " changed during its read");
    offset += static_cast<std::size_t>(count);
  }
  char extra{};
  ssize_t extra_count = -1;
  do {
    extra_count = ::read(descriptor.get(), &extra, 1U);
  } while (extra_count < 0 && errno == EINTR);
  Require(extra_count == 0, std::string(label) + " grew during its read");
  struct stat after {};
  Require(::fstat(descriptor.get(), &after) == 0 && SameSnapshot(before, after),
          std::string(label) + " changed during its read");
  return bytes;
}

[[nodiscard]] std::filesystem::path
DigestPath(const std::filesystem::path &root, const std::string_view collection,
           const std::string_view digest) {
  Require(is_sha256_digest(digest),
          "scenario Program selection contains an invalid digest");
  return root / collection / "sha256" /
         (std::string(digest.substr(7U)) + ".json");
}

struct VerifiedScenarioCatalog final {
  CanonicalValue root;
  core::ScenarioCatalogRevision revision;
};

[[nodiscard]] const CanonicalValue &
ExactScenario(const CanonicalValue &catalog,
              const std::string_view scenario_id) {
  Require(IsIdentifier(scenario_id), "scenario ID is invalid");
  const auto &rows = Array(Member(catalog, "scenarios", "$"), "$.scenarios");
  const auto found = std::lower_bound(
      rows.begin(), rows.end(), scenario_id,
      [](const CanonicalValue &row, const std::string_view selected) {
        return Text(Member(row, "scenario_id", "$.scenarios[]"),
                    "$.scenarios[].scenario_id") < selected;
      });
  Require(found != rows.end() &&
              Text(Member(*found, "scenario_id", "$.scenarios[]"),
                   "$.scenarios[].scenario_id") == scenario_id,
          "scenario catalog has no exact canonical scenario ID");
  return *found;
}

[[nodiscard]] ScenarioAssistantMode
AssistantMode(const std::string_view value) {
  if (value == "required") {
    return ScenarioAssistantMode::kRequired;
  }
  if (value == "optional") {
    return ScenarioAssistantMode::kOptional;
  }
  Require(value == "forbidden", "scenario assistant mode is invalid");
  return ScenarioAssistantMode::kForbidden;
}

[[nodiscard]] VerifiedScenarioCatalog
VerifyCatalog(const std::string_view bytes) {
  Require(!bytes.empty() && bytes.size() <= kMaxArtifactEncodedBytes,
          "scenario Apply catalog is empty or oversized");
  auto root = parse_strict_json_model_yaml(bytes).root();
  Require(canonical_json(root) == bytes,
          "scenario Apply catalog is not canonical JSON");
  ExactMembers(root, {"api_version", "scenario_catalog_revision", "scenarios"},
               {}, "$");
  Require(Text(Member(root, "api_version", "$"), "$.api_version") ==
              kScenarioCatalogApiVersion,
          "scenario Apply catalog API version is unsupported");
  const auto &revision = Text(Member(root, "scenario_catalog_revision", "$"),
                              "$.scenario_catalog_revision");
  Require(is_sha256_digest(revision) && detail::verify_content_identity(
                                            root, kScenarioCatalogIdentitySpec),
          "scenario Apply catalog content identity is invalid");

  const auto &rows = Array(Member(root, "scenarios", "$"), "$.scenarios");
  Require(rows.size() <= kMaxScenarioCatalogRows,
          "scenario Apply catalog exceeds its row bound");
  std::set<std::string, std::less<>> global_identifiers;
  std::string previous_scenario;
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const auto path = "$.scenarios[" + std::to_string(index) + "]";
    const auto &row = rows[index];
    ExactMembers(row,
                 {"scenario_id", "deployment_artifact_id",
                  "deployment_revision", "retained_bundle_id",
                  "program_binding_id", "deployment_compatibility",
                  "assistant_mode", "typed_parameter_overrides"},
                 {"description", "aliases"}, path);
    const auto &scenario_id =
        Text(Member(row, "scenario_id", path), path + ".scenario_id");
    Require(IsIdentifier(scenario_id) &&
                (index == 0U || previous_scenario < scenario_id) &&
                global_identifiers.emplace(scenario_id).second,
            "scenario Apply catalog IDs are invalid or not canonical");
    previous_scenario = scenario_id;
    const auto &artifact_id = Text(Member(row, "deployment_artifact_id", path),
                                   path + ".deployment_artifact_id");
    const auto &deployment_revision =
        Text(Member(row, "deployment_revision", path),
             path + ".deployment_revision");
    const auto &retained_bundle = Text(Member(row, "retained_bundle_id", path),
                                       path + ".retained_bundle_id");
    const auto &program_binding = Text(Member(row, "program_binding_id", path),
                                       path + ".program_binding_id");
    const auto &assistant_mode =
        Text(Member(row, "assistant_mode", path), path + ".assistant_mode");
    Require(IsIdentifier(artifact_id) &&
                is_sha256_digest(deployment_revision) &&
                is_sha256_digest(retained_bundle) &&
                is_sha256_digest(program_binding) &&
                (assistant_mode == "required" || assistant_mode == "optional" ||
                 assistant_mode == "forbidden"),
            "scenario Apply catalog row has an invalid closed identity");
    static_cast<void>(DeploymentCompatibility(row, path));
    static_cast<void>(Object(Member(row, "typed_parameter_overrides", path),
                             path + ".typed_parameter_overrides"));
    if (const auto *description = row.find_object_member("description");
        description != nullptr) {
      const auto &value = Text(*description, path + ".description");
      Require(!value.empty() && Utf8CodepointCount(value) <=
                                    kMaxScenarioPresentationTextCharacters,
              "scenario Apply catalog description exceeds its bound");
    }
    if (const auto *aliases = row.find_object_member("aliases");
        aliases != nullptr) {
      const auto &values = Array(*aliases, path + ".aliases");
      Require(values.size() <= kMaxScenarioAliasesPerRow,
              "scenario Apply catalog aliases exceed their row bound");
      std::string previous_alias;
      for (std::size_t alias_index = 0U; alias_index < values.size();
           ++alias_index) {
        const auto &alias = Text(values[alias_index], path + ".aliases[]");
        Require(IsIdentifier(alias) &&
                    (alias_index == 0U || previous_alias < alias) &&
                    global_identifiers.emplace(alias).second,
                "scenario Apply catalog aliases are ambiguous or not "
                "canonical");
        previous_alias = alias;
      }
    }
  }
  core::ScenarioCatalogRevision verified_revision(revision);
  return {std::move(root), std::move(verified_revision)};
}

[[nodiscard]] ScenarioApplyCandidateSelection
ResolveCatalog(const std::string_view bytes,
               const ScenarioApplyCandidateQuery &query) {
  auto catalog = VerifyCatalog(bytes);
  std::optional<core::RetainedBundleId> selected_bundle;
  const auto &rows =
      Array(Member(catalog.root, "scenarios", "$"), "$.scenarios");
  for (const auto &row : rows) {
    const auto &artifact_id =
        Text(Member(row, "deployment_artifact_id", "$.scenarios[]"),
             "$.scenarios[].deployment_artifact_id");
    const auto &deployment_revision =
        Text(Member(row, "deployment_revision", "$.scenarios[]"),
             "$.scenarios[].deployment_revision");
    if (artifact_id == query.deployment_artifact_id.value() &&
        deployment_revision == query.deployment_revision.value()) {
      const core::RetainedBundleId candidate(
          Text(Member(row, "retained_bundle_id", "$.scenarios[]"),
               "$.scenarios[].retained_bundle_id"));
      Require(!selected_bundle.has_value() || *selected_bundle == candidate,
              "scenario Apply catalog maps one deployment revision to "
              "multiple retained bundles");
      selected_bundle = candidate;
    }
  }
  Require(selected_bundle.has_value(),
          "scenario Apply catalog has no exact requested deployment "
          "revision");
  return {
      query,
      std::move(catalog.revision),
      std::move(*selected_bundle),
  };
}

[[nodiscard]] std::optional<ScenarioApplyCandidateSelection>
TryResolveCatalog(const std::string_view bytes,
                  const ScenarioApplyCandidateQuery &query) {
  auto catalog = VerifyCatalog(bytes);
  std::optional<core::RetainedBundleId> selected_bundle;
  const auto &rows =
      Array(Member(catalog.root, "scenarios", "$"), "$.scenarios");
  for (const auto &row : rows) {
    const auto &artifact_id =
        Text(Member(row, "deployment_artifact_id", "$.scenarios[]"),
             "$.scenarios[].deployment_artifact_id");
    const auto &deployment_revision =
        Text(Member(row, "deployment_revision", "$.scenarios[]"),
             "$.scenarios[].deployment_revision");
    if (artifact_id == query.deployment_artifact_id.value() &&
        deployment_revision == query.deployment_revision.value()) {
      const core::RetainedBundleId candidate(
          Text(Member(row, "retained_bundle_id", "$.scenarios[]"),
               "$.scenarios[].retained_bundle_id"));
      Require(!selected_bundle.has_value() || *selected_bundle == candidate,
              "scenario Apply catalog maps one deployment revision to "
              "multiple retained bundles");
      selected_bundle = candidate;
    }
  }
  if (!selected_bundle.has_value()) {
    return std::nullopt;
  }
  return ScenarioApplyCandidateSelection{
      query,
      std::move(catalog.revision),
      std::move(*selected_bundle),
  };
}

} // namespace

bool ScenarioApplyCandidateQuery::valid() const noexcept {
  return !deployment_artifact_id.empty() &&
         is_sha256_digest(deployment_revision.value());
}

bool ScenarioApplyCandidateSelection::valid() const noexcept {
  return query.valid() && !scenario_catalog_revision.empty() &&
         !retained_bundle_id.empty();
}

bool ScenarioProgramSelection::valid() const noexcept {
  const bool assistant_absent = std::holds_alternative<std::nullptr_t>(
      normalized_assistant_projection.storage());
  const bool compatibility_valid =
      (deployment_compatibility.lane == "manyforge-only" ||
       deployment_compatibility.lane == "isaac-sim" ||
       deployment_compatibility.lane == "real-robot") &&
      deployment_compatibility.provides.size() <=
          kMaxScenarioDeploymentProvides &&
      std::all_of(deployment_compatibility.provides.begin(),
                  deployment_compatibility.provides.end(), IsIdentifier) &&
      std::is_sorted(deployment_compatibility.provides.begin(),
                     deployment_compatibility.provides.end()) &&
      std::adjacent_find(deployment_compatibility.provides.begin(),
                         deployment_compatibility.provides.end()) ==
          deployment_compatibility.provides.end();
  return IsIdentifier(scenario_id) &&
         is_sha256_digest(scenario_catalog_revision.value()) &&
         IsIdentifier(deployment_artifact_id.value()) &&
         is_sha256_digest(deployment_revision.value()) &&
         is_sha256_digest(retained_bundle_id.value()) &&
         is_sha256_digest(graph_revision.value()) &&
         is_sha256_digest(program_binding_id.value()) &&
         IsIdentifier(program_id.value()) &&
         is_sha256_digest(program_revision.value()) &&
         is_sha256_digest(node_environment_digest.value()) &&
         compatibility_valid && typed_parameter_overrides.is_object() &&
         (assistant_absent ||
          normalized_assistant_projection.is_object()) &&
         (assistant_mode != ScenarioAssistantMode::kRequired ||
         normalized_assistant_projection.is_object()) &&
         (assistant_mode != ScenarioAssistantMode::kForbidden ||
          assistant_absent) &&
         normalized_program_root.is_object() &&
         normalized_program_binding_root.is_object();
}

FilesystemScenarioApplyCandidateSource::FilesystemScenarioApplyCandidateSource(
    std::filesystem::path package_root, std::string catalog_name,
    std::optional<std::filesystem::path> candidate_package_root)
    : package_root_(ExistingRoot(std::move(package_root))),
      candidate_package_root_(
          candidate_package_root.has_value()
              ? std::optional(ExistingRoot(std::move(*candidate_package_root)))
              : std::nullopt),
      catalog_name_(std::move(catalog_name)) {
  Require(IsIdentifier(catalog_name_),
          "scenario Apply catalog name is invalid");
}

ScenarioApplyCandidateSelection
FilesystemScenarioApplyCandidateSource::ResolveExact(
    const ScenarioApplyCandidateQuery &query) const {
  Require(query.valid(), "scenario Apply candidate query is invalid");
  if (const auto candidate = candidate_catalog_path();
      candidate.has_value()) {
    auto result = TryResolveCatalog(
        ReadExactArtifact(*candidate, "candidate scenario Apply catalog"),
        query);
    if (result.has_value()) {
      Require(result->valid(),
              "scenario Apply candidate selection is invalid");
      return *result;
    }
  }
  auto result = ResolveCatalog(ReadExactArtifact(
                                   catalog_path(), "scenario Apply catalog"),
                               query);
  Require(result.valid(), "scenario Apply candidate selection is invalid");
  return result;
}

CanonicalValue
FilesystemScenarioApplyCandidateSource::LoadVerifiedCatalog() const {
  return VerifyCatalog(ReadExactArtifact(catalog_path(), "scenario catalog"))
      .root;
}

ScenarioProgramSelection
FilesystemScenarioApplyCandidateSource::LoadExactProgramSelection(
    const std::string_view scenario_id) const {
  return LoadExactProgramSelectionFrom(scenario_id, catalog_path());
}

ScenarioProgramSelection
FilesystemScenarioApplyCandidateSource::LoadExactProgramSelectionForCatalog(
    const std::string_view scenario_id,
    const core::ScenarioCatalogRevision &scenario_catalog_revision) const {
  return LoadExactProgramSelectionFrom(
      scenario_id, catalog_path_for_revision(scenario_catalog_revision));
}

std::optional<ScenarioProgramSelection>
FilesystemScenarioApplyCandidateSource::
    TryLoadExactProgramSelectionForActive(
        const std::string_view scenario_id,
        const core::RetainedBundleId &retained_bundle_id,
        const core::GraphRevision &graph_revision) const {
  Require(is_sha256_digest(retained_bundle_id.value()) &&
              is_sha256_digest(graph_revision.value()),
          "active Scenario selection identity is invalid");
  if (const auto candidate = candidate_catalog_path();
      candidate.has_value()) {
    auto selection = LoadExactProgramSelectionFrom(scenario_id, *candidate);
    if (selection.retained_bundle_id == retained_bundle_id &&
        selection.graph_revision == graph_revision) {
      return selection;
    }
  }
  auto selection = LoadExactProgramSelectionFrom(scenario_id, catalog_path());
  if (selection.retained_bundle_id == retained_bundle_id &&
      selection.graph_revision == graph_revision) {
    return selection;
  }
  return std::nullopt;
}

ScenarioProgramSelection
FilesystemScenarioApplyCandidateSource::LoadExactProgramSelectionFrom(
    const std::string_view scenario_id,
    const std::filesystem::path &selected_catalog_path) const {
  const auto catalog =
      VerifyCatalog(ReadExactArtifact(selected_catalog_path, "scenario catalog"));
  const auto &row = ExactScenario(catalog.root, scenario_id);
  const auto &row_scenario_id = Text(
      Member(row, "scenario_id", "$.scenarios[]"), "$.scenarios[].scenario_id");
  const core::DeploymentArtifactId deployment_artifact_id(
      Text(Member(row, "deployment_artifact_id", "$.scenarios[]"),
           "$.scenarios[].deployment_artifact_id"));
  const core::DeploymentRevision deployment_revision(
      Text(Member(row, "deployment_revision", "$.scenarios[]"),
           "$.scenarios[].deployment_revision"));
  const core::RetainedBundleId retained_bundle_id(
      Text(Member(row, "retained_bundle_id", "$.scenarios[]"),
           "$.scenarios[].retained_bundle_id"));
  const core::ProgramBindingId program_binding_id(
      Text(Member(row, "program_binding_id", "$.scenarios[]"),
           "$.scenarios[].program_binding_id"));
  const auto deployment_compatibility =
      DeploymentCompatibility(row, "$.scenarios[]");
  const auto assistant_mode =
      AssistantMode(Text(Member(row, "assistant_mode", "$.scenarios[]"),
                         "$.scenarios[].assistant_mode"));
  const auto &typed_parameter_overrides =
      Member(row, "typed_parameter_overrides", "$.scenarios[]");

  const auto selected_package_root = [&]() -> const std::filesystem::path & {
    if (!candidate_package_root_.has_value()) {
      return package_root_;
    }
    const auto candidate_bundle =
        DigestPath(*candidate_package_root_, "retained-bundles",
                   retained_bundle_id.value());
    std::error_code error;
    const bool exists = std::filesystem::exists(candidate_bundle, error);
    Require(!error, "candidate retained-bundle lookup failed");
    return exists ? *candidate_package_root_ : package_root_;
  }();
  const FilesystemVerifiedRetainedBundleSource bundle_source(
      selected_package_root);
  const auto bundle = bundle_source.get_verified(retained_bundle_id);
  Require(bundle != nullptr,
          "scenario retained bundle is absent or unverifiable");
  Require(bundle->value().deployment_artifact_id == deployment_artifact_id &&
              bundle->value().deployment_revision == deployment_revision &&
              bundle->value().compiled_products.symbol_role_maps.size() == 1U,
          "scenario retained bundle differs from its canonical row");

  const auto graph_revision = bundle->value().cell_graph.graph_revision;
  const auto node_environment_digest =
      bundle->value().catalogs.node_environment.environment_digest;
  const FilesystemVerifiedRetainedModelMapSource model_map_source(
      selected_package_root);
  const auto model_map = model_map_source.get_verified(RetainedModelMapQuery{
      retained_bundle_id,
      bundle->value().compiled_products.symbol_role_maps.front().product_id,
  });
  Require(model_map.has_value(),
          "scenario retained model map is absent or unverifiable");

  const auto binding_bytes = ReadExactArtifact(
      DigestPath(selected_package_root, "program-bindings",
                 program_binding_id.value()),
      "scenario Program Binding");
  const auto binding =
      reload_verified_program_binding(binding_bytes, *bundle, *model_map);
  Require(binding->program_binding_id() == program_binding_id &&
              binding->value().retained_bundle_id == retained_bundle_id &&
              binding->value().graph_revision == graph_revision &&
              binding->value().node_environment.environment_digest ==
                  node_environment_digest,
          "scenario Program Binding differs from its retained closure");

  const auto program_bytes =
      ReadExactArtifact(DigestPath(selected_package_root, "programs",
                                   binding->value().program_revision.value()),
                        "scenario Program");
  const auto program = verify_program_binding_contract(program_bytes, *bundle);
  Require(program->value().program_id == binding->value().program_id &&
              program->value().program_revision ==
                  binding->value().program_revision &&
              program->value().node_environment ==
                  binding->value().node_environment,
          "scenario Program differs from its exact Program Binding");

  const bool assistant_available = bundle->value().assistant.has_value();
  Require(assistant_mode != ScenarioAssistantMode::kRequired ||
              assistant_available,
          "scenario requires an unavailable assistant product");
  Require(assistant_mode != ScenarioAssistantMode::kForbidden ||
              !assistant_available,
          "scenario forbids the retained assistant product");

  CanonicalValue assistant_projection(nullptr);
  if (bundle->value().assistant.has_value()) {
    try {
      assistant_projection =
          parse_strict_json_model_yaml(bundle->value().assistant->canonical_json)
              .root();
    } catch (const ArtifactCodecError &error) {
      Fail(std::string("scenario retained assistant projection cannot be "
                       "decoded: ") +
           error.what());
    }
  }

  ScenarioProgramSelection result{
      .scenario_id = row_scenario_id,
      .scenario_catalog_revision = catalog.revision,
      .deployment_artifact_id = deployment_artifact_id,
      .deployment_revision = deployment_revision,
      .retained_bundle_id = retained_bundle_id,
      .graph_revision = graph_revision,
      .program_binding_id = program_binding_id,
      .program_id = binding->value().program_id,
      .program_revision = binding->value().program_revision,
      .node_environment_digest = node_environment_digest,
      .deployment_compatibility = deployment_compatibility,
      .assistant_mode = assistant_mode,
      .typed_parameter_overrides = typed_parameter_overrides,
      .normalized_assistant_projection = std::move(assistant_projection),
      .normalized_program_root = program->normalized_program_root(),
      .normalized_program_binding_root = binding->normalized_root(),
  };
  Require(result.valid(), "scenario Program selection is invalid");
  return result;
}

std::filesystem::path
FilesystemScenarioApplyCandidateSource::catalog_path() const {
  return package_root_ / (catalog_name_ + ".scenario-catalog.yaml");
}

std::optional<std::filesystem::path>
FilesystemScenarioApplyCandidateSource::candidate_catalog_path() const {
  if (!candidate_package_root_.has_value()) {
    return std::nullopt;
  }
  const auto candidate =
      *candidate_package_root_ / (catalog_name_ + ".scenario-catalog.yaml");
  std::error_code error;
  const bool exists = std::filesystem::exists(candidate, error);
  Require(!error, "candidate scenario-catalog lookup failed");
  if (!exists) {
    return std::nullopt;
  }
  Require(std::filesystem::is_regular_file(candidate, error) && !error &&
              !std::filesystem::is_symlink(candidate, error) && !error,
          "candidate scenario catalog is not one plain file");
  return candidate;
}

std::filesystem::path
FilesystemScenarioApplyCandidateSource::catalog_path_for_revision(
    const core::ScenarioCatalogRevision &scenario_catalog_revision) const {
  Require(is_sha256_digest(scenario_catalog_revision.value()),
          "scenario catalog revision is invalid");
  if (const auto candidate = candidate_catalog_path();
      candidate.has_value()) {
    const auto verified = VerifyCatalog(
        ReadExactArtifact(*candidate, "candidate scenario catalog"));
    if (verified.revision == scenario_catalog_revision) {
      return *candidate;
    }
  }
  const auto base = catalog_path();
  const auto verified =
      VerifyCatalog(ReadExactArtifact(base, "scenario catalog"));
  if (verified.revision == scenario_catalog_revision) {
    return base;
  }
  const auto filename = catalog_name_ + ".scenario-catalog." +
                        scenario_catalog_revision.value().substr(7U) + ".json";
  for (const auto &root :
       {candidate_package_root_,
        std::optional<std::filesystem::path>(package_root_)}) {
    if (!root.has_value()) {
      continue;
    }
    const auto retained = *root / filename;
    std::error_code error;
    const auto status = std::filesystem::symlink_status(retained, error);
    if (status.type() == std::filesystem::file_type::not_found) {
      continue;
    }
    Require(!error && std::filesystem::is_regular_file(status),
            "retained scenario catalog is not one plain file");
    const auto catalog =
        VerifyCatalog(ReadExactArtifact(retained, "retained scenario catalog"));
    Require(catalog.revision == scenario_catalog_revision,
            "retained scenario catalog has another revision");
    return retained;
  }
  Fail("scenario catalog has no exact requested revision");
}

} // namespace manyforge::deployment
