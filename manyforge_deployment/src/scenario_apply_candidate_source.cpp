#include "manyforge_deployment/scenario_apply_candidate_source.hpp"

#include "content_identity_internal.hpp"

#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/scenario_catalog.hpp>

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
  return static_cast<std::size_t>(std::count_if(
      value.begin(), value.end(), [](const unsigned char item) {
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

[[nodiscard]] std::filesystem::path ExistingRoot(std::filesystem::path root) {
  Require(!root.empty(), "scenario Apply package root is empty");
  std::error_code error;
  const auto supplied = std::filesystem::symlink_status(root, error);
  Require(!error && !std::filesystem::is_symlink(supplied) &&
              std::filesystem::is_directory(supplied),
          "scenario Apply package root must be a plain directory");
  root = std::filesystem::canonical(root, error);
  Require(!error && root.is_absolute(),
          "scenario Apply package root cannot be resolved");
  return root;
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

[[nodiscard]] std::string ReadExactCatalog(const std::filesystem::path &path) {
  const FileDescriptor descriptor(
      ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  Require(descriptor.get() >= 0, "scenario Apply catalog cannot be opened: " +
                                     std::string(std::strerror(errno)));
  struct stat before {};
  Require(::fstat(descriptor.get(), &before) == 0 && S_ISREG(before.st_mode) &&
              before.st_size >= 0 &&
              static_cast<std::uint64_t>(before.st_size) <=
                  kMaxArtifactEncodedBytes,
          "scenario Apply catalog is not one bounded regular file");
  std::string bytes(static_cast<std::size_t>(before.st_size), '\0');
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const auto count =
        ::read(descriptor.get(), bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    Require(count > 0, "scenario Apply catalog changed during its read");
    offset += static_cast<std::size_t>(count);
  }
  char extra{};
  ssize_t extra_count = -1;
  do {
    extra_count = ::read(descriptor.get(), &extra, 1U);
  } while (extra_count < 0 && errno == EINTR);
  Require(extra_count == 0,
          "scenario Apply catalog grew during its read");
  struct stat after {};
  Require(::fstat(descriptor.get(), &after) == 0 && SameSnapshot(before, after),
          "scenario Apply catalog changed during its read");
  return bytes;
}

[[nodiscard]] ScenarioApplyCandidateSelection
ResolveCatalog(const std::string_view bytes,
               const ScenarioApplyCandidateQuery &query) {
  Require(!bytes.empty() && bytes.size() <= kMaxArtifactEncodedBytes,
          "scenario Apply catalog is empty or oversized");
  const auto root = parse_strict_json_model_yaml(bytes).root();
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
  std::optional<core::RetainedBundleId> selected_bundle;
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const auto path = "$.scenarios[" + std::to_string(index) + "]";
    const auto &row = rows[index];
    ExactMembers(row,
                 {"scenario_id", "deployment_artifact_id",
                  "deployment_revision", "retained_bundle_id",
                  "program_binding_id", "assistant_mode",
                  "typed_parameter_overrides"},
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
    static_cast<void>(Object(Member(row, "typed_parameter_overrides", path),
                             path + ".typed_parameter_overrides"));
    if (const auto *description = row.find_object_member("description");
        description != nullptr) {
      const auto &value = Text(*description, path + ".description");
      Require(!value.empty() &&
                  Utf8CodepointCount(value) <=
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
    if (artifact_id == query.deployment_artifact_id.value() &&
        deployment_revision == query.deployment_revision.value()) {
      const core::RetainedBundleId candidate(retained_bundle);
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
      core::ScenarioCatalogRevision(revision),
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

FilesystemScenarioApplyCandidateSource::FilesystemScenarioApplyCandidateSource(
    std::filesystem::path package_root, std::string catalog_name)
    : package_root_(ExistingRoot(std::move(package_root))) {
  Require(IsIdentifier(catalog_name), "scenario Apply catalog name is invalid");
  catalog_path_ =
      package_root_ / (std::move(catalog_name) + ".scenario-catalog.yaml");
}

ScenarioApplyCandidateSelection
FilesystemScenarioApplyCandidateSource::ResolveExact(
    const ScenarioApplyCandidateQuery &query) const {
  Require(query.valid(), "scenario Apply candidate query is invalid");
  auto result = ResolveCatalog(ReadExactCatalog(catalog_path_), query);
  Require(result.valid(), "scenario Apply candidate selection is invalid");
  return result;
}

const std::filesystem::path &
FilesystemScenarioApplyCandidateSource::catalog_path() const noexcept {
  return catalog_path_;
}

} // namespace manyforge::deployment
