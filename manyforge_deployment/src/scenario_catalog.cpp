#include "manyforge_deployment/scenario_catalog.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "content_identity_internal.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "manyforge_deployment/deployment_artifact_codec.hpp"
#include "manyforge_deployment/program_binding.hpp"
#include "manyforge_deployment/program_contract.hpp"
#include "manyforge_deployment/retained_bundle.hpp"
#include "manyforge_deployment/runtime_profile_codec.hpp"
#include "manyforge_deployment/verified_retained_bundle.hpp"
#include "posix_durability_internal.hpp"

namespace manyforge::deployment {

namespace detail {

class ScenarioCatalogConstructionAuthority final {
public:
  [[nodiscard]] static std::shared_ptr<const ScenarioCatalog>
  seal(std::vector<ScenarioCatalogRowDTO> rows);
  [[nodiscard]] static std::shared_ptr<const ScenarioCatalog>
  reconstruct(core::ScenarioCatalogRevision revision,
              std::vector<ScenarioCatalogRowDTO> rows, CanonicalValue root,
              std::string canonical_bytes);
};

} // namespace detail

namespace {

namespace durability = detail::posix_durability;

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";
const detail::ContentIdentitySpec kScenarioCatalogIdentitySpec{
    .identity_domain = "scenario-catalog-revision",
    .api_version = std::string(kScenarioCatalogApiVersion),
    .own_field = "scenario_catalog_revision",
};

[[noreturn]] void fail(std::string_view message) {
  throw ScenarioCatalogError(std::string(message));
}

[[nodiscard]] std::exception_ptr make_scenario_error(std::string_view message) {
  return std::make_exception_ptr(ScenarioCatalogError(std::string(message)));
}

const durability::ErrorPolicy kErrors{make_scenario_error};
const durability::TrustedRootMessages kRootMessages{
    "scenario-catalog repository root is empty",
    "scenario-catalog repository root cannot be resolved",
    "scenario-catalog repository root must be an existing plain directory",
};
const durability::LockFileMessages kLockMessages{
    "scenario-catalog lock cannot be opened",
    "scenario-catalog lock cannot be inspected",
    "scenario-catalog lock must be a plain file",
    "scenario-catalog lock cannot be acquired",
    "scenario-catalog repository cannot be reused by a forked process",
};
const durability::DirectorySyncMessages kDirectorySyncMessages{
    "scenario-catalog directory cannot be opened",
    "scenario-catalog directory cannot be synchronized",
};
const durability::ReadMessages kReadMessages{
    "scenario artifact cannot be opened",
    "scenario artifact cannot be inspected",
    "scenario artifact exceeds its fixed byte bound",
    "scenario artifact changed while it was read",
    "scenario artifact grew while it was read",
};

[[nodiscard]] const CanonicalValue::Object &
require_object(const CanonicalValue &value, std::string_view path) {
  const auto *object = std::get_if<CanonicalValue::Object>(&value.storage());
  if (object == nullptr) {
    fail(std::string(path) + " must be a mapping");
  }
  return *object;
}

[[nodiscard]] const CanonicalValue::Array &
require_array(const CanonicalValue &value, std::string_view path) {
  const auto *array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    fail(std::string(path) + " must be an array");
  }
  return *array;
}

[[nodiscard]] const std::string &require_string(const CanonicalValue &value,
                                                std::string_view path) {
  const auto *text = std::get_if<std::string>(&value.storage());
  if (text == nullptr) {
    fail(std::string(path) + " must be text");
  }
  return *text;
}

[[nodiscard]] const CanonicalValue &require_member(const CanonicalValue &value,
                                                   std::string_view member,
                                                   std::string_view path) {
  const CanonicalValue *result = value.find_object_member(member);
  if (result == nullptr) {
    fail(std::string(path) + " is missing " + std::string(member));
  }
  return *result;
}

void require_exact_members(const CanonicalValue &value,
                           std::initializer_list<std::string_view> required,
                           std::initializer_list<std::string_view> optional,
                           std::string_view path) {
  const auto &object = require_object(value, path);
  std::set<std::string, std::less<>> allowed;
  for (const auto member : required) {
    allowed.emplace(member);
    (void)require_member(value, member, path);
  }
  for (const auto member : optional) {
    allowed.emplace(member);
  }
  for (const auto &[name, ignored] : object) {
    static_cast<void>(ignored);
    if (!allowed.contains(name)) {
      fail(std::string(path) + " has unknown field " + name);
    }
  }
}

[[nodiscard]] bool valid_identifier(std::string_view value) noexcept {
  if (value.empty() || value.size() > 128U || value.front() < 'a' ||
      value.front() > 'z') {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [](const char item) {
    return (item >= 'a' && item <= 'z') || (item >= '0' && item <= '9') ||
           item == '_' || item == '.' || item == '-';
  });
}

[[nodiscard]] std::string require_identifier(const CanonicalValue &value,
                                             std::string_view path) {
  const std::string &text = require_string(value, path);
  if (!valid_identifier(text)) {
    fail(std::string(path) + " must be a bounded lowercase identifier");
  }
  return text;
}

[[nodiscard]] std::size_t
utf8_codepoint_count(std::string_view value) noexcept {
  return static_cast<std::size_t>(
      std::count_if(value.begin(), value.end(), [](const unsigned char item) {
        return (item & 0xc0U) != 0x80U;
      }));
}

[[nodiscard]] std::string require_presentation_text(const CanonicalValue &value,
                                                    std::string_view path) {
  const std::string &text = require_string(value, path);
  if (text.empty() ||
      utf8_codepoint_count(text) > kMaxScenarioPresentationTextCharacters) {
    fail(std::string(path) + " violates its presentation-text bound");
  }
  return text;
}

[[nodiscard]] std::string require_digest(const CanonicalValue &value,
                                         std::string_view path) {
  const std::string &text = require_string(value, path);
  if (!is_sha256_digest(text)) {
    fail(std::string(path) + " must be a lowercase SHA-256 digest");
  }
  return text;
}

[[nodiscard]] ScenarioAssistantMode
parse_assistant_mode(std::string_view value, std::string_view path) {
  if (value == "required") {
    return ScenarioAssistantMode::kRequired;
  }
  if (value == "optional") {
    return ScenarioAssistantMode::kOptional;
  }
  if (value == "forbidden") {
    return ScenarioAssistantMode::kForbidden;
  }
  fail(std::string(path) + " is not a registered assistant mode");
}

[[nodiscard]] ScenarioDeploymentCompatibilityDTO
parse_deployment_compatibility(const CanonicalValue &value,
                               std::string_view path) {
  require_exact_members(value, {"lane", "provides"}, {}, path);
  ScenarioDeploymentCompatibilityDTO result{
      .lane = require_identifier(require_member(value, "lane", path),
                                 std::string(path) + ".lane"),
      .provides = {},
  };
  if (result.lane != "manyforge-only" && result.lane != "isaac-sim" &&
      result.lane != "real-robot") {
    fail(std::string(path) + ".lane is unsupported");
  }
  const auto &provides = require_array(require_member(value, "provides", path),
                                       std::string(path) + ".provides");
  if (provides.size() > kMaxScenarioDeploymentProvides) {
    fail(std::string(path) + ".provides exceeds its fixed bound");
  }
  result.provides.reserve(provides.size());
  for (std::size_t index = 0U; index < provides.size(); ++index) {
    result.provides.push_back(
        require_identifier(provides[index], std::string(path) + ".provides[" +
                                                std::to_string(index) + "]"));
  }
  if (!std::is_sorted(result.provides.begin(), result.provides.end()) ||
      std::adjacent_find(result.provides.begin(), result.provides.end()) !=
          result.provides.end()) {
    fail(std::string(path) +
         ".provides must be duplicate-free and canonical-sorted");
  }
  return result;
}

[[nodiscard]] CanonicalValue deployment_compatibility_projection(
    const ScenarioDeploymentCompatibilityDTO &compatibility) {
  CanonicalValue::Array provides;
  provides.reserve(compatibility.provides.size());
  for (const auto &item : compatibility.provides) {
    provides.emplace_back(item);
  }
  return CanonicalValue(CanonicalValue::Object{
      {"lane", CanonicalValue(compatibility.lane)},
      {"provides", CanonicalValue(std::move(provides))},
  });
}

[[nodiscard]] ScenarioCatalogRowDTO parse_row(const CanonicalValue &value,
                                              std::string_view path) {
  require_exact_members(value,
                        {"scenario_id", "deployment_artifact_id",
                         "deployment_revision", "retained_bundle_id",
                         "program_binding_id", "deployment_compatibility",
                         "assistant_mode", "typed_parameter_overrides"},
                        {"description", "aliases"}, path);
  ScenarioCatalogRowDTO row{
      .scenario_id =
          require_identifier(require_member(value, "scenario_id", path),
                             std::string(path) + ".scenario_id"),
      .description = std::nullopt,
      .aliases = {},
      .deployment_artifact_id = core::DeploymentArtifactId(require_identifier(
          require_member(value, "deployment_artifact_id", path),
          std::string(path) + ".deployment_artifact_id")),
      .deployment_revision = core::DeploymentRevision(
          require_digest(require_member(value, "deployment_revision", path),
                         std::string(path) + ".deployment_revision")),
      .retained_bundle_id = core::RetainedBundleId(
          require_digest(require_member(value, "retained_bundle_id", path),
                         std::string(path) + ".retained_bundle_id")),
      .program_binding_id = core::ProgramBindingId(
          require_digest(require_member(value, "program_binding_id", path),
                         std::string(path) + ".program_binding_id")),
      .deployment_compatibility = parse_deployment_compatibility(
          require_member(value, "deployment_compatibility", path),
          std::string(path) + ".deployment_compatibility"),
      .assistant_mode = parse_assistant_mode(
          require_string(require_member(value, "assistant_mode", path),
                         std::string(path) + ".assistant_mode"),
          std::string(path) + ".assistant_mode"),
      .typed_parameter_overrides =
          require_member(value, "typed_parameter_overrides", path),
  };
  (void)require_object(row.typed_parameter_overrides,
                       std::string(path) + ".typed_parameter_overrides");
  if (const CanonicalValue *description =
          value.find_object_member("description");
      description != nullptr) {
    row.description = require_presentation_text(
        *description, std::string(path) + ".description");
  }
  if (const CanonicalValue *aliases = value.find_object_member("aliases");
      aliases != nullptr) {
    const auto &values =
        require_array(*aliases, std::string(path) + ".aliases");
    if (values.size() > kMaxScenarioAliasesPerRow) {
      fail(std::string(path) + ".aliases exceeds its fixed row bound");
    }
    row.aliases.reserve(values.size());
    for (std::size_t index = 0U; index < values.size(); ++index) {
      row.aliases.push_back(
          require_identifier(values[index], std::string(path) + ".aliases[" +
                                                std::to_string(index) + "]"));
    }
  }
  return row;
}

void validate_rows(const std::vector<ScenarioCatalogRowDTO> &rows) {
  if (rows.size() > kMaxScenarioCatalogRows) {
    fail("scenario catalog exceeds its fixed row bound");
  }
  std::set<std::string, std::less<>> identifiers;
  std::string previous;
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const auto &row = rows[index];
    const std::string path = "$.scenarios[" + std::to_string(index) + "]";
    if (!valid_identifier(row.scenario_id)) {
      fail(path + ".scenario_id is invalid");
    }
    if (index != 0U && previous >= row.scenario_id) {
      fail("$.scenarios must be unique and canonical-sorted by scenario_id");
    }
    previous = row.scenario_id;
    if (!identifiers.emplace(row.scenario_id).second) {
      fail("scenario catalog repeats an identifier");
    }
    if (row.description.has_value() &&
        (row.description->empty() ||
         utf8_codepoint_count(*row.description) >
             kMaxScenarioPresentationTextCharacters)) {
      fail(path + ".description violates its presentation-text bound");
    }
    if (row.aliases.size() > kMaxScenarioAliasesPerRow ||
        !std::is_sorted(row.aliases.begin(), row.aliases.end()) ||
        std::adjacent_find(row.aliases.begin(), row.aliases.end()) !=
            row.aliases.end()) {
      fail(path + ".aliases must be bounded, unique, and canonical-sorted");
    }
    for (const auto &alias : row.aliases) {
      if (!valid_identifier(alias) || !identifiers.emplace(alias).second) {
        fail("scenario catalog has an invalid or ambiguous alias");
      }
    }
    if (!is_sha256_digest(row.deployment_revision.value()) ||
        !is_sha256_digest(row.retained_bundle_id.value()) ||
        !is_sha256_digest(row.program_binding_id.value())) {
      fail(path + " contains an invalid strong digest");
    }
    (void)parse_deployment_compatibility(
        deployment_compatibility_projection(row.deployment_compatibility),
        path + ".deployment_compatibility");
    (void)require_object(row.typed_parameter_overrides,
                         path + ".typed_parameter_overrides");
  }
}

[[nodiscard]] CanonicalValue row_projection(const ScenarioCatalogRowDTO &row) {
  CanonicalValue::Object value{
      {"scenario_id", CanonicalValue(row.scenario_id)},
      {"deployment_artifact_id",
       CanonicalValue(row.deployment_artifact_id.value())},
      {"deployment_revision", CanonicalValue(row.deployment_revision.value())},
      {"retained_bundle_id", CanonicalValue(row.retained_bundle_id.value())},
      {"program_binding_id", CanonicalValue(row.program_binding_id.value())},
      {"deployment_compatibility",
       deployment_compatibility_projection(row.deployment_compatibility)},
      {"assistant_mode",
       CanonicalValue(std::string(to_string(row.assistant_mode)))},
      {"typed_parameter_overrides", row.typed_parameter_overrides},
  };
  if (row.description.has_value()) {
    value.emplace_back("description", CanonicalValue(*row.description));
  }
  if (!row.aliases.empty()) {
    CanonicalValue::Array aliases;
    aliases.reserve(row.aliases.size());
    for (const auto &alias : row.aliases) {
      aliases.emplace_back(alias);
    }
    value.emplace_back("aliases", CanonicalValue(std::move(aliases)));
  }
  return CanonicalValue(std::move(value));
}

[[nodiscard]] CanonicalValue
catalog_projection(const core::ScenarioCatalogRevision &revision,
                   const std::vector<ScenarioCatalogRowDTO> &rows) {
  CanonicalValue::Array scenarios;
  scenarios.reserve(rows.size());
  for (const auto &row : rows) {
    scenarios.push_back(row_projection(row));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kScenarioCatalogApiVersion))},
      {"scenario_catalog_revision", CanonicalValue(revision.value())},
      {"scenarios", CanonicalValue(std::move(scenarios))},
  });
}

[[nodiscard]] std::vector<ScenarioCatalogRowDTO>
parse_catalog_rows(const CanonicalValue &root) {
  require_exact_members(
      root, {"api_version", "scenario_catalog_revision", "scenarios"}, {}, "$");
  if (require_string(require_member(root, "api_version", "$"),
                     "$.api_version") != kScenarioCatalogApiVersion) {
    fail("$.api_version is not manyforge.scenario_catalog.v1");
  }
  (void)require_digest(require_member(root, "scenario_catalog_revision", "$"),
                       "$.scenario_catalog_revision");
  const auto &values =
      require_array(require_member(root, "scenarios", "$"), "$.scenarios");
  if (values.size() > kMaxScenarioCatalogRows) {
    fail("$.scenarios exceeds its fixed row bound");
  }
  std::vector<ScenarioCatalogRowDTO> rows;
  rows.reserve(values.size());
  for (std::size_t index = 0U; index < values.size(); ++index) {
    rows.push_back(
        parse_row(values[index], "$.scenarios[" + std::to_string(index) + "]"));
  }
  validate_rows(rows);
  return rows;
}

[[nodiscard]] std::shared_ptr<const ScenarioCatalog>
verified_catalog(std::string_view bytes, bool require_canonical_bytes) {
  if (bytes.size() > kMaxArtifactEncodedBytes) {
    fail("scenario catalog exceeds the fixed artifact byte bound");
  }
  const CanonicalValue root = parse_strict_json_model_yaml(bytes).root();
  const std::string canonical = canonical_json(root);
  if (require_canonical_bytes && canonical != bytes) {
    fail("scenario-catalog repository bytes are not RFC 8785 canonical");
  }
  std::vector<ScenarioCatalogRowDTO> rows = parse_catalog_rows(root);
  if (!detail::verify_content_identity(root, kScenarioCatalogIdentitySpec)) {
    fail("scenario_catalog_revision does not match the complete catalog");
  }
  return detail::ScenarioCatalogConstructionAuthority::reconstruct(
      core::ScenarioCatalogRevision(
          require_string(require_member(root, "scenario_catalog_revision", "$"),
                         "$.scenario_catalog_revision")),
      std::move(rows), root, canonical);
}

[[nodiscard]] std::optional<std::string>
read_bounded_file(const std::filesystem::path &path,
                  bool missing_is_empty = false) {
  return durability::read_exact_regular_file(
      path, kMaxArtifactEncodedBytes, missing_is_empty, kErrors, kReadMessages);
}

[[nodiscard]] std::filesystem::path
canonical_source_path(const std::filesystem::path &trusted_root,
                      const std::filesystem::path &candidate,
                      std::string_view label) {
  std::error_code error;
  const auto supplied_status =
      std::filesystem::symlink_status(candidate, error);
  if (error || std::filesystem::is_symlink(supplied_status)) {
    fail(std::string(label) + " must not be a symbolic link");
  }
  const std::filesystem::path canonical =
      std::filesystem::canonical(candidate, error);
  if (error) {
    fail(std::string(label) + " cannot be resolved");
  }
  const std::filesystem::path relative =
      canonical.lexically_relative(trusted_root);
  if (relative.empty() || relative.is_absolute() ||
      std::any_of(relative.begin(), relative.end(),
                  [](const auto &part) { return part == ".."; })) {
    fail(std::string(label) + " escapes the scenario repository root");
  }
  const auto status = std::filesystem::symlink_status(canonical, error);
  if (error || !std::filesystem::is_regular_file(status) ||
      std::filesystem::is_symlink(status)) {
    fail(std::string(label) + " must be one existing plain file");
  }
  return canonical;
}

[[nodiscard]] std::string
read_required_source(const std::filesystem::path &trusted_root,
                     const std::filesystem::path &candidate,
                     std::string_view label) {
  const auto path = canonical_source_path(trusted_root, candidate, label);
  const auto bytes = read_bounded_file(path);
  if (!bytes.has_value()) {
    fail(std::string(label) + " is unavailable");
  }
  return *bytes;
}

[[nodiscard]] std::filesystem::path
expected_digest_path(const std::filesystem::path &root,
                     std::string_view collection, std::string_view digest) {
  if (!is_sha256_digest(digest)) {
    fail("repository identity is not a SHA-256 digest");
  }
  return root / collection / "sha256" /
         (std::string(digest.substr(7U)) + ".json");
}

void require_exact_path(const std::filesystem::path &root,
                        const std::filesystem::path &supplied,
                        const std::filesystem::path &expected,
                        std::string_view label) {
  if (canonical_source_path(root, supplied, label) !=
      canonical_source_path(root, expected, label)) {
    fail(std::string(label) + " is not the exact repository coordinate");
  }
}

[[nodiscard]] const ScenarioCatalogRowDTO &
require_scenario_row(const ScenarioCatalog &catalog,
                     std::string_view scenario_id) {
  const auto found = std::lower_bound(
      catalog.rows().begin(), catalog.rows().end(), scenario_id,
      [](const ScenarioCatalogRowDTO &row, std::string_view id) {
        return row.scenario_id < id;
      });
  if (found == catalog.rows().end() || found->scenario_id != scenario_id) {
    fail("scenario catalog does not contain the requested scenario");
  }
  return *found;
}

[[nodiscard]] const CanonicalValue *
optional_object_member(const CanonicalValue &root, std::string_view member,
                       std::string_view path) {
  const CanonicalValue *value = root.find_object_member(member);
  if (value != nullptr) {
    (void)require_object(*value, std::string(path) + "." + std::string(member));
  }
  return value;
}

[[nodiscard]] std::set<std::string, std::less<>>
string_set_member(const CanonicalValue &root, std::string_view member,
                  std::string_view path) {
  std::set<std::string, std::less<>> result;
  const CanonicalValue *value = root.find_object_member(member);
  if (value == nullptr) {
    return result;
  }
  const auto &array =
      require_array(*value, std::string(path) + "." + std::string(member));
  for (std::size_t index = 0U; index < array.size(); ++index) {
    result.emplace(require_identifier(
        array[index], std::string(path) + "." + std::string(member) + "[" +
                          std::to_string(index) + "]"));
  }
  return result;
}

[[nodiscard]] double require_number(const CanonicalValue &value,
                                    std::string_view path) {
  const auto *number = std::get_if<double>(&value.storage());
  if (number == nullptr || !std::isfinite(*number)) {
    fail(std::string(path) + " must be a finite number");
  }
  return *number;
}

void validate_override_value(
    const CanonicalValue &override_value, const CanonicalValue &declaration,
    const std::set<std::string, std::less<>> &scene_resource_ids,
    std::string_view path) {
  const std::string &type =
      require_string(require_member(declaration, "type", path),
                     std::string(path) + ".declaration.type");
  if (type == "bool") {
    if (std::get_if<bool>(&override_value.storage()) == nullptr) {
      fail(std::string(path) + " does not match bool");
    }
  } else if (type == "float64") {
    (void)require_number(override_value, path);
  } else if (type == "int64") {
    const double number = require_number(override_value, path);
    if (std::trunc(number) != number || std::abs(number) > 9007199254740991.0) {
      fail(std::string(path) + " does not match int64");
    }
  } else if (type == "scene_resource" || type == "string") {
    const std::string &text = require_string(override_value, path);
    if (text.empty() ||
        (type == "scene_resource" && !scene_resource_ids.contains(text))) {
      fail(std::string(path) + " does not match " + type);
    }
  } else if (type == "string_list") {
    const auto &values = require_array(override_value, path);
    for (const auto &value : values) {
      (void)require_string(value, path);
    }
  } else if (type == "model_response" || type == "pick_planning_result" ||
             type == "scene_selection_context" ||
             type == "scene_selection_result" || type == "stored_pose") {
    (void)require_object(override_value, path);
  } else {
    fail(std::string(path) + " has an unsupported Program parameter type");
  }

  const CanonicalValue *constraints =
      declaration.find_object_member("constraints");
  if (constraints == nullptr) {
    return;
  }
  (void)require_object(*constraints, std::string(path) + ".constraints");
  if (type == "float64" || type == "int64") {
    const double number = require_number(override_value, path);
    for (const auto &[name, value] : require_object(*constraints, path)) {
      const double bound = require_number(value, path);
      const bool valid = (name == "min_inclusive" && number >= bound) ||
                         (name == "min_exclusive" && number > bound) ||
                         (name == "max_inclusive" && number <= bound) ||
                         (name == "max_exclusive" && number < bound);
      if (!valid) {
        fail(std::string(path) + " violates " + name);
      }
    }
  } else if (type == "string" || type == "string_list") {
    const std::size_t length =
        type == "string"
            ? utf8_codepoint_count(require_string(override_value, path))
            : require_array(override_value, path).size();
    for (const auto &[name, value] : require_object(*constraints, path)) {
      const double bound = require_number(value, path);
      const bool valid =
          (name == "min_length" && static_cast<double>(length) >= bound) ||
          (name == "max_length" && static_cast<double>(length) <= bound);
      if (!valid) {
        fail(std::string(path) + " violates " + name);
      }
    }
  }
}

void validate_typed_overrides(const CanonicalValue &program_root,
                              const CanonicalValue &overrides) {
  const auto &override_object =
      require_object(overrides, "typed_parameter_overrides");
  std::set<std::string, std::less<>> scene_resource_ids;
  if (const CanonicalValue *scene = program_root.find_object_member("scene");
      scene != nullptr) {
    for (const auto &resource : require_array(*scene, "Program.scene")) {
      scene_resource_ids.emplace(require_identifier(
          require_member(resource, "scene_resource_id", "Program.scene[]"),
          "Program.scene[].scene_resource_id"));
    }
  }

  std::map<std::string, const CanonicalValue *, std::less<>> declarations;
  if (const CanonicalValue *parameters =
          program_root.find_object_member("parameters");
      parameters != nullptr) {
    for (const auto &declaration :
         require_array(*parameters, "Program.parameters")) {
      const std::string id = require_identifier(
          require_member(declaration, "parameter_id", "Program.parameters[]"),
          "Program.parameters[].parameter_id");
      declarations.emplace(id, &declaration);
    }
  }
  for (const auto &[id, value] : override_object) {
    const auto found = declarations.find(id);
    if (found == declarations.end()) {
      fail("typed_parameter_overrides selects an undeclared Program parameter");
    }
    const std::string &mutability =
        require_string(require_member(*found->second, "runtime_mutability",
                                      "Program parameter"),
                       "Program parameter.runtime_mutability");
    if (mutability == "immutable") {
      fail("typed_parameter_overrides selects an immutable Program parameter");
    }
    validate_override_value(value, *found->second, scene_resource_ids,
                            "typed_parameter_overrides." + id);
  }
}

[[nodiscard]] std::vector<QualifiedRuntimeProfile>
load_qualified_runtime_profiles(const core::RetainedBundleDTO &bundle,
                                const std::filesystem::path &root) {
  FilesystemCatalogArtifactStore catalog_store(root);
  std::vector<QualifiedRuntimeProfile> profiles;
  for (const auto &record : bundle.catalogs.records) {
    const auto *profile =
        std::get_if<core::RuntimeProfileCatalogRecordRefDTO>(&record);
    if (profile == nullptr) {
      continue;
    }
    const auto bytes = catalog_store.get_verified(
        profile->content_addressed_store_key, profile->content_digest);
    if (!bytes.has_value()) {
      fail("retained runtime-profile catalog artifact is unavailable");
    }
    profiles.push_back(resolve_and_qualify_runtime_profile(
        *profile, profile->content_addressed_store_key, *bytes));
  }
  if (profiles.empty()) {
    fail("retained bundle has no qualified runtime profile");
  }
  return profiles;
}

[[nodiscard]] ScenarioDeploymentCompatibilityDTO deployment_compatibility_from(
    const RevisionVerifiedDeploymentArtifact &deployment) {
  const CanonicalValue *compatibility = optional_object_member(
      deployment.normalized_authored_root(), "compatibility", "deployment");
  if (compatibility == nullptr) {
    fail("scenario deployment has no compatibility projection");
  }
  return parse_deployment_compatibility(
      CanonicalValue(CanonicalValue::Object{
          {"lane",
           require_member(*compatibility, "lane", "deployment.compatibility")},
          {"provides", require_member(*compatibility, "provides",
                                      "deployment.compatibility")},
      }),
      "deployment.compatibility");
}

void validate_program_admission(
    const CanonicalValue &program_root, const core::RetainedBundleDTO &bundle,
    const ScenarioDeploymentCompatibilityDTO &deployment_compatibility,
    ScenarioAssistantMode scenario_mode, const CanonicalValue &overrides) {
  if (require_string(require_member(program_root, "program_profile", "Program"),
                     "Program.program_profile") !=
      core::kComposableCellProgramProfile) {
    fail("scenario Program does not use composable-cell-v1");
  }
  const CanonicalValue &catalog =
      require_member(program_root, "catalog", "Program");
  const auto &environment = bundle.catalogs.node_environment;
  if (require_string(require_member(catalog, "api_version", "Program.catalog"),
                     "Program.catalog.api_version") !=
          "manyforge.node_catalog.v2" ||
      require_string(
          require_member(catalog, "content_version", "Program.catalog"),
          "Program.catalog.content_version") != environment.content_version) {
    fail("scenario Program selects another retained node catalog");
  }

  const CanonicalValue *program_compatibility =
      optional_object_member(program_root, "compatibility", "Program");
  std::set<std::string, std::less<>> lanes;
  std::set<std::string, std::less<>> requirements;
  std::optional<std::string> program_assistant;
  if (program_compatibility != nullptr) {
    lanes = string_set_member(*program_compatibility, "lanes",
                              "Program.compatibility");
    requirements = string_set_member(*program_compatibility, "requires",
                                     "Program.compatibility");
    if (const CanonicalValue *assistant =
            program_compatibility->find_object_member("assistant");
        assistant != nullptr) {
      program_assistant =
          require_string(*assistant, "Program.compatibility.assistant");
    }
  }
  if (!lanes.empty() && !lanes.contains(deployment_compatibility.lane)) {
    fail("scenario deployment lane is incompatible with the Program");
  }
  const std::set<std::string, std::less<>> provides(
      deployment_compatibility.provides.begin(),
      deployment_compatibility.provides.end());
  if (!std::includes(provides.begin(), provides.end(), requirements.begin(),
                     requirements.end())) {
    fail("scenario deployment does not provide every Program requirement");
  }

  const bool assistant_enabled = bundle.assistant.has_value();
  switch (scenario_mode) {
  case ScenarioAssistantMode::kRequired:
    if (!assistant_enabled || program_assistant == "forbidden") {
      fail("required scenario assistant mode is not admissible");
    }
    break;
  case ScenarioAssistantMode::kForbidden:
    if (assistant_enabled || program_assistant == "required") {
      fail("forbidden scenario assistant mode is not admissible");
    }
    break;
  case ScenarioAssistantMode::kOptional:
    if (program_assistant != "optional") {
      fail("optional scenario assistant mode requires an optional Program");
    }
    break;
  }
  validate_typed_overrides(program_root, overrides);
}

void write_atomically(const std::filesystem::path &path,
                      std::string_view bytes) {
  std::string pattern = path.string() + ".tmp.XXXXXX";
  std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
  mutable_pattern.push_back('\0');
  const int descriptor = ::mkstemp(mutable_pattern.data());
  if (descriptor < 0) {
    fail("scenario-catalog temporary file cannot be created");
  }
  const std::filesystem::path temporary(mutable_pattern.data());
  bool open = true;
  try {
    if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0) {
      fail("scenario-catalog temporary file cannot be made close-on-exec");
    }
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
      const ssize_t written =
          ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        fail("scenario-catalog temporary file cannot be written");
      }
      if (written == 0) {
        fail("scenario-catalog temporary write made no progress");
      }
      offset += static_cast<std::size_t>(written);
    }
    if (::fsync(descriptor) != 0) {
      fail("scenario-catalog temporary file cannot be synchronized");
    }
    if (::close(descriptor) != 0) {
      open = false;
      fail("scenario-catalog temporary file cannot be closed");
    }
    open = false;
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
      fail("scenario catalog cannot be atomically published");
    }
    durability::sync_directory(path.parent_path(), kDirectorySyncMessages);
  } catch (...) {
    if (open) {
      (void)::close(descriptor);
    }
    std::error_code ignored;
    (void)std::filesystem::remove(temporary, ignored);
    throw;
  }
}

} // namespace

namespace detail {

std::shared_ptr<const ScenarioCatalog>
ScenarioCatalogConstructionAuthority::seal(
    std::vector<ScenarioCatalogRowDTO> rows) {
  validate_rows(rows);
  core::ScenarioCatalogRevision revision{std::string(kZeroDigest)};
  CanonicalValue root = catalog_projection(revision, rows);
  revision = core::ScenarioCatalogRevision(
      compute_content_identity(root, kScenarioCatalogIdentitySpec));
  root = catalog_projection(revision, rows);
  if (!verify_content_identity(root, kScenarioCatalogIdentitySpec)) {
    fail("scenario catalog construction produced an invalid identity");
  }
  return std::shared_ptr<const ScenarioCatalog>(new ScenarioCatalog(
      revision, std::move(rows), root, canonical_json(root)));
}

std::shared_ptr<const ScenarioCatalog>
ScenarioCatalogConstructionAuthority::reconstruct(
    core::ScenarioCatalogRevision revision,
    std::vector<ScenarioCatalogRowDTO> rows, CanonicalValue root,
    std::string canonical_bytes) {
  return std::shared_ptr<const ScenarioCatalog>(
      new ScenarioCatalog(std::move(revision), std::move(rows), std::move(root),
                          std::move(canonical_bytes)));
}

} // namespace detail

std::shared_ptr<const ScenarioCatalog>
build_scenario_catalog(std::vector<ScenarioCatalogRowDTO> rows) {
  return detail::ScenarioCatalogConstructionAuthority::seal(std::move(rows));
}

std::shared_ptr<const ScenarioCatalog>
verify_scenario_catalog_artifact(std::string_view encoded_bytes) {
  return verified_catalog(encoded_bytes, false);
}

std::shared_ptr<const ScenarioCatalog>
reload_verified_scenario_catalog(std::string_view canonical_bytes) {
  return verified_catalog(canonical_bytes, true);
}

ScenarioDeploymentCompatibilityDTO scenario_deployment_compatibility(
    const RevisionVerifiedDeploymentArtifact &deployment) {
  try {
    return deployment_compatibility_from(deployment);
  } catch (const ScenarioCatalogError &) {
    throw;
  } catch (const std::exception &error) {
    fail(
        std::string("scenario deployment compatibility verification failed: ") +
        error.what());
  }
}

void verify_scenario_program_admission(
    const CanonicalValue &normalized_program_root,
    const core::RetainedBundleDTO &retained_bundle,
    const ScenarioDeploymentCompatibilityDTO &deployment_compatibility,
    const ScenarioAssistantMode assistant_mode,
    const CanonicalValue &typed_parameter_overrides) {
  try {
    (void)parse_deployment_compatibility(
        deployment_compatibility_projection(deployment_compatibility),
        "scenario.deployment_compatibility");
    validate_program_admission(normalized_program_root, retained_bundle,
                               deployment_compatibility, assistant_mode,
                               typed_parameter_overrides);
  } catch (const ScenarioCatalogError &) {
    throw;
  } catch (const std::exception &error) {
    fail(std::string("scenario Program admission verification failed: ") +
         error.what());
  }
}

struct FilesystemScenarioCatalogRepository::Storage final {
  [[nodiscard]] static std::string validated_name(std::string value) {
    if (!valid_identifier(value)) {
      fail("scenario-catalog repository name is invalid");
    }
    return value;
  }

  Storage(std::filesystem::path trusted_root, std::string catalog_name)
      : root(durability::normalize_existing_plain_root(trusted_root, kErrors,
                                                       kRootMessages)),
        name(validated_name(std::move(catalog_name))),
        path(root / (name + ".scenario-catalog.yaml")),
        lock(root / ("." + name + ".scenario-catalog.lock"), kErrors,
             kLockMessages, kDirectorySyncMessages) {}

  std::filesystem::path root;
  std::string name;
  std::filesystem::path path;
  durability::ProcessAffineLockFile lock;
};

FilesystemScenarioCatalogRepository::FilesystemScenarioCatalogRepository(
    std::filesystem::path trusted_root, std::string catalog_name)
    : storage_(std::make_unique<Storage>(std::move(trusted_root),
                                         std::move(catalog_name))) {}

FilesystemScenarioCatalogRepository::~FilesystemScenarioCatalogRepository() =
    default;

ScenarioCatalogPublishResult
FilesystemScenarioCatalogRepository::compare_and_set(
    const std::optional<core::ScenarioCatalogRevision> &expected_revision,
    std::shared_ptr<const ScenarioCatalog> candidate) {
  storage_->lock.require_owner_process();
  if (candidate == nullptr) {
    fail("scenario-catalog repository rejects a null candidate");
  }
  const auto reloaded =
      reload_verified_scenario_catalog(candidate->canonical_bytes());
  if (reloaded->revision() != candidate->revision() ||
      reloaded->rows() != candidate->rows()) {
    fail("scenario-catalog repository rejects divergent typed and canonical "
         "values");
  }

  const auto lock = storage_->lock.acquire(LOCK_EX);
  const auto current_bytes = read_bounded_file(storage_->path, true);
  std::shared_ptr<const ScenarioCatalog> current;
  if (current_bytes.has_value()) {
    current = reload_verified_scenario_catalog(*current_bytes);
  }
  if ((current == nullptr) != !expected_revision.has_value() ||
      (current != nullptr && current->revision() != *expected_revision)) {
    return ScenarioCatalogPublishResult::kRevisionConflict;
  }
  if (current != nullptr && current->revision() == candidate->revision()) {
    return ScenarioCatalogPublishResult::kAlreadyCurrent;
  }
  write_atomically(storage_->path, candidate->canonical_bytes());
  return ScenarioCatalogPublishResult::kPublished;
}

std::shared_ptr<const ScenarioCatalog>
FilesystemScenarioCatalogRepository::get_verified() const {
  storage_->lock.require_owner_process();
  const auto lock = storage_->lock.acquire(LOCK_SH);
  const auto bytes = read_bounded_file(storage_->path, true);
  return bytes.has_value() ? reload_verified_scenario_catalog(*bytes) : nullptr;
}

const std::filesystem::path &
FilesystemScenarioCatalogRepository::catalog_path() const noexcept {
  return storage_->path;
}

VerifiedScenarioArtifactClosure
verify_scenario_artifact_closure(const ScenarioArtifactClosureQuery &query) {
  try {
    const std::filesystem::path root =
        durability::normalize_existing_plain_root(query.repository_root,
                                                  kErrors, kRootMessages);
    const std::string catalog_bytes = read_required_source(
        root, query.scenario_catalog_path, "scenario catalog");
    const auto catalog = verify_scenario_catalog_artifact(catalog_bytes);
    if (catalog->revision() != query.scenario_catalog_revision) {
      fail("scenario catalog revision differs from the enumerated closure");
    }
    const auto &row = require_scenario_row(*catalog, query.scenario_id);
    const CanonicalValue overrides =
        parse_strict_json_model_yaml(query.typed_parameter_overrides_json)
            .root();
    if (canonical_json(overrides) != query.typed_parameter_overrides_json ||
        !overrides.is_object()) {
      fail("typed_parameter_overrides_json must be one canonical JSON object");
    }
    if (row.deployment_artifact_id != query.deployment_artifact_id ||
        row.deployment_revision != query.deployment_revision ||
        row.retained_bundle_id != query.retained_bundle_id ||
        row.program_binding_id != query.program_binding_id ||
        row.assistant_mode != query.assistant_mode ||
        row.typed_parameter_overrides != overrides) {
      fail("scenario row differs from the enumerated artifact closure");
    }

    require_exact_path(root, query.retained_bundle_path,
                       expected_digest_path(root, "retained-bundles",
                                            query.retained_bundle_id.value()),
                       "retained bundle");
    require_exact_path(root, query.program_binding_path,
                       expected_digest_path(root, "program-bindings",
                                            query.program_binding_id.value()),
                       "Program Binding");

    const FilesystemVerifiedRetainedBundleSource bundle_source(root);
    const auto bundle = bundle_source.get_verified(query.retained_bundle_id);
    if (bundle == nullptr ||
        bundle->value().deployment_artifact_id !=
            query.deployment_artifact_id ||
        bundle->value().deployment_revision != query.deployment_revision ||
        bundle->value().cell_graph.graph_revision != query.graph_revision ||
        bundle->value().catalogs.node_environment.environment_digest !=
            query.node_environment_digest) {
      fail("retained bundle differs from the exact scenario closure");
    }

    const auto profiles =
        load_qualified_runtime_profiles(bundle->value(), root);
    const std::string deployment_bytes =
        read_required_source(root, query.deployment_path, "deployment source");
    const auto deployment = verify_deployment_artifact_revision(
        parse_strict_json_model_yaml(deployment_bytes), profiles);
    if (deployment.deployment_artifact_id() != query.deployment_artifact_id ||
        deployment.deployment_revision() != query.deployment_revision ||
        deployment.graph().graph().graph_revision != query.graph_revision) {
      fail("deployment source differs from the retained scenario closure");
    }
    const auto deployment_compatibility =
        scenario_deployment_compatibility(deployment);
    if (row.deployment_compatibility != deployment_compatibility) {
      fail("scenario deployment compatibility projection differs from its "
           "exact source");
    }
    if (bundle->value().assistant.has_value() !=
        deployment.assistant_settings().enabled()) {
      fail("scenario deployment and retained assistant projection disagree");
    }

    if (bundle->value().compiled_products.symbol_role_maps.size() != 1U) {
      fail("scenario retained bundle must select one exact model map");
    }
    const FilesystemVerifiedRetainedModelMapSource model_map_source(root);
    auto model_map = model_map_source.get_verified(RetainedModelMapQuery{
        query.retained_bundle_id,
        bundle->value().compiled_products.symbol_role_maps.front().product_id});
    if (!model_map.has_value()) {
      fail("scenario retained model map cannot be independently reloaded");
    }
    const std::string binding_bytes = read_required_source(
        root, query.program_binding_path, "Program Binding");
    const auto binding =
        reload_verified_program_binding(binding_bytes, *bundle, *model_map);
    if (binding->program_binding_id() != query.program_binding_id ||
        binding->value().program_id != query.program_id ||
        binding->value().program_revision != query.program_revision ||
        binding->value().graph_revision != query.graph_revision ||
        binding->value().node_environment.environment_digest !=
            query.node_environment_digest) {
      fail("Program Binding differs from the exact scenario closure");
    }

    const std::string program_bytes =
        read_required_source(root, query.program_path, "Program source");
    const CanonicalValue program_root =
        parse_strict_json_model_yaml(program_bytes).root();
    const std::string canonical_program = canonical_json(program_root);
    const auto program_contract =
        verify_program_binding_contract(canonical_program, *bundle);
    if (program_contract->value().program_id != query.program_id ||
        program_contract->value().program_revision != query.program_revision ||
        program_contract->value().node_environment !=
            binding->value().node_environment) {
      fail("Program source differs from the exact Program Binding");
    }

    verify_scenario_program_admission(program_root, bundle->value(),
                                      deployment_compatibility,
                                      query.assistant_mode, overrides);
    return VerifiedScenarioArtifactClosure{
        deployment_compatibility.lane, bundle->value().assistant.has_value()};
  } catch (const ScenarioCatalogError &) {
    throw;
  } catch (const std::exception &error) {
    fail(std::string("scenario artifact closure verification failed: ") +
         error.what());
  }
}

} // namespace manyforge::deployment
