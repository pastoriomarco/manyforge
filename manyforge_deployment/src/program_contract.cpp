#include "manyforge_deployment/program_contract.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cmath>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "content_identity_internal.hpp"
#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/content_identity.hpp"

namespace manyforge::deployment {
namespace {

constexpr std::size_t kMaximumIdentityBytes = 128U;
constexpr std::size_t kMaximumProgramTreeDepth = 256U;
constexpr std::size_t kMaximumProgramTreeNodes = 100000U;
constexpr std::size_t kMaximumProgramChildren = 10000U;

[[noreturn]] void fail(std::string message) {
  throw ProgramContractError(std::move(message));
}

[[nodiscard]] const CanonicalValue::Object& require_object(
    const CanonicalValue& value, std::string_view path) {
  const auto* object = std::get_if<CanonicalValue::Object>(&value.storage());
  if (object == nullptr) {
    fail(std::string(path) + ": expected object");
  }
  return *object;
}

[[nodiscard]] const CanonicalValue::Array& require_array(
    const CanonicalValue& value, std::string_view path) {
  const auto* array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    fail(std::string(path) + ": expected array");
  }
  return *array;
}

[[nodiscard]] const std::string& require_string(
    const CanonicalValue& value, std::string_view path) {
  const auto* text = std::get_if<std::string>(&value.storage());
  if (text == nullptr) {
    fail(std::string(path) + ": expected string");
  }
  return *text;
}

[[nodiscard]] double require_number(const CanonicalValue& value,
                                    std::string_view path) {
  const auto* number = std::get_if<double>(&value.storage());
  if (number == nullptr || !std::isfinite(*number)) {
    fail(std::string(path) + ": expected finite number");
  }
  return *number;
}

[[nodiscard]] const CanonicalValue& require_member(
    const CanonicalValue::Object& object, std::string_view name,
    std::string_view path) {
  const auto found = std::find_if(
      object.begin(), object.end(),
      [name](const auto& member) { return member.first == name; });
  if (found == object.end()) {
    fail(std::string(path) + ": missing required field '" +
         std::string(name) + "'");
  }
  return found->second;
}

[[nodiscard]] const CanonicalValue* find_member(
    const CanonicalValue::Object& object, std::string_view name) noexcept {
  const auto found = std::find_if(
      object.begin(), object.end(),
      [name](const auto& member) { return member.first == name; });
  return found == object.end() ? nullptr : &found->second;
}

void require_fields(const CanonicalValue::Object& object,
                    std::initializer_list<std::string_view> required,
                    std::initializer_list<std::string_view> optional,
                    std::string_view path) {
  for (const auto& [name, unused] : object) {
    (void)unused;
    if (std::find(required.begin(), required.end(), name) == required.end() &&
        std::find(optional.begin(), optional.end(), name) == optional.end()) {
      fail(std::string(path) + ": unknown field '" + name + "'");
    }
  }
  for (const std::string_view name : required) {
    (void)require_member(object, name, path);
  }
}

void require_exact_fields(const CanonicalValue::Object& object,
                          std::initializer_list<std::string_view> expected,
                          std::string_view path) {
  require_fields(object, expected, {}, path);
}

[[nodiscard]] std::string child_path(std::string_view parent,
                                     std::string_view child) {
  return std::string(parent) + "." + std::string(child);
}

[[nodiscard]] std::string index_path(std::string_view parent,
                                     std::size_t index) {
  return std::string(parent) + "[" + std::to_string(index) + "]";
}

[[nodiscard]] bool is_ascii_alphanumeric(unsigned char value) noexcept {
  return (value >= 'a' && value <= 'z') ||
         (value >= 'A' && value <= 'Z') ||
         (value >= '0' && value <= '9');
}

void require_identity(std::string_view value, std::string_view path) {
  if (value.empty() || value.size() > kMaximumIdentityBytes ||
      !is_ascii_alphanumeric(static_cast<unsigned char>(value.front())) ||
      !std::all_of(value.begin() + 1, value.end(), [](unsigned char byte) {
        return is_ascii_alphanumeric(byte) || byte == '.' || byte == '_' ||
               byte == '-';
      })) {
    fail(std::string(path) + ": invalid bounded portable identity");
  }
}

void require_program_local_identity(std::string_view value,
                                    std::string_view path) {
  require_identity(value, path);
  if (value.front() < 'a' || value.front() > 'z' ||
      !std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return (byte >= 'a' && byte <= 'z') ||
               (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
               byte == '-';
      })) {
    fail(std::string(path) + ": expected lower-case Program-local ID");
  }
}

template <typename StrongType>
[[nodiscard]] StrongType parse_program_identity(const CanonicalValue& value,
                                                std::string_view path) {
  const std::string& spelling = require_string(value, path);
  require_program_local_identity(spelling, path);
  return StrongType(spelling);
}

template <typename StrongType>
[[nodiscard]] std::vector<StrongType> parse_identity_set(
    const CanonicalValue* value, std::string_view path) {
  if (value == nullptr) {
    return {};
  }
  const auto& array = require_array(*value, path);
  if (array.size() > core::kMaxProgramRoleRequirements) {
    fail(std::string(path) + ": exceeds fixed Program role bound");
  }
  std::vector<StrongType> result;
  result.reserve(array.size());
  for (std::size_t index = 0U; index < array.size(); ++index) {
    result.push_back(parse_program_identity<StrongType>(
        array[index], index_path(path, index)));
  }
  if (!std::is_sorted(result.begin(), result.end()) ||
      std::adjacent_find(result.begin(), result.end()) != result.end()) {
    fail(std::string(path) + ": must be duplicate-free and canonical-sorted");
  }
  return result;
}

[[nodiscard]] std::vector<std::string> parse_text_set(
    const CanonicalValue* value, std::string_view path) {
  if (value == nullptr) {
    return {};
  }
  const auto& array = require_array(*value, path);
  if (array.size() > core::kMaxProgramRoleRequirements) {
    fail(std::string(path) + ": exceeds fixed Program role bound");
  }
  std::vector<std::string> result;
  result.reserve(array.size());
  for (std::size_t index = 0U; index < array.size(); ++index) {
    const std::string& item = require_string(array[index], index_path(path, index));
    require_program_local_identity(item, index_path(path, index));
    result.push_back(item);
  }
  if (!std::is_sorted(result.begin(), result.end()) ||
      std::adjacent_find(result.begin(), result.end()) != result.end()) {
    fail(std::string(path) + ": must be duplicate-free and canonical-sorted");
  }
  return result;
}

[[nodiscard]] const core::CapabilityDefinitionDescriptor*
require_capability_definition(const core::RetainedBundleDTO& bundle,
                              const core::CapabilityId& capability_id) {
  const auto* descriptor =
      core::find_builtin_capability_definition(capability_id.value());
  if (descriptor == nullptr) {
    fail("Program target requires an unknown capability: " +
         capability_id.value());
  }
  std::size_t exact_matches = 0U;
  std::size_t conflicting_matches = 0U;
  for (const auto& record : bundle.catalogs.records) {
    if (core::catalog_record_kind(record) !=
            core::CatalogRecordKind::kCapabilityDefinition ||
        core::catalog_record_id_value(record) != capability_id.value()) {
      continue;
    }
    if (core::catalog_record_revision_value(record) ==
        descriptor->capability_definition_revision) {
      ++exact_matches;
    } else {
      ++conflicting_matches;
    }
  }
  if (exact_matches != 1U || conflicting_matches != 0U) {
    fail("retained catalog does not contain exactly one selected capability "
         "definition revision");
  }
  return descriptor;
}

[[nodiscard]] core::ProgramCapabilityDisposition parse_disposition(
    const CanonicalValue& value, std::string_view path) {
  const std::string& spelling = require_string(value, path);
  if (spelling == "required") {
    return core::ProgramCapabilityDisposition::kRequired;
  }
  if (spelling == "optional") {
    return core::ProgramCapabilityDisposition::kOptional;
  }
  fail(std::string(path) + ": expected required or optional");
}

void verify_value_source(const CanonicalValue& value, std::string_view path) {
  const auto& source = require_object(value, path);
  const std::string& kind =
      require_string(require_member(source, "kind", path), child_path(path, "kind"));
  if (kind == "program_parameter" || kind == "blackboard_key") {
    require_exact_fields(source, {"kind", "reference"}, path);
    require_program_local_identity(
        require_string(require_member(source, "reference", path), child_path(path, "reference")),
        child_path(path, "reference"));
    return;
  }
  if (kind == "blackboard_field") {
    require_exact_fields(source, {"kind", "reference", "field"}, path);
    require_program_local_identity(
        require_string(require_member(source, "reference", path), child_path(path, "reference")),
        child_path(path, "reference"));
    require_program_local_identity(
        require_string(require_member(source, "field", path), child_path(path, "field")),
        child_path(path, "field"));
    return;
  }
  fail(std::string(path) + ": unsupported Program value-source kind");
}

void verify_tree(const CanonicalValue& value, std::string_view path,
                 std::size_t depth, std::size_t& count,
                 std::set<std::string>& instance_ids) {
  if (depth > kMaximumProgramTreeDepth || ++count > kMaximumProgramTreeNodes) {
    fail(std::string(path) + ": Program tree exceeds fixed traversal bounds");
  }
  const auto& node = require_object(value, path);
  require_fields(node, {"node_id", "instance_id", "parameters", "children"},
                 {"metadata", "value_sources"}, path);
  const std::string& node_id =
      require_string(require_member(node, "node_id", path), child_path(path, "node_id"));
  const std::string& instance_id =
      require_string(require_member(node, "instance_id", path), child_path(path, "instance_id"));
  require_program_local_identity(node_id, child_path(path, "node_id"));
  require_program_local_identity(instance_id, child_path(path, "instance_id"));
  if (!instance_ids.insert(instance_id).second) {
    fail(std::string(path) + ": duplicate tree instance_id");
  }
  const auto& parameters =
      require_object(require_member(node, "parameters", path), child_path(path, "parameters"));
  if (parameters.size() > core::kMaxProgramRoleRequirements) {
    fail(std::string(path) + ".parameters: exceeds fixed bound");
  }
  if (const auto* value_sources = find_member(node, "value_sources"); value_sources != nullptr) {
    const auto& sources = require_object(*value_sources, child_path(path, "value_sources"));
    if (sources.empty() || sources.size() > core::kMaxProgramRoleRequirements) {
      fail(std::string(path) + ".value_sources: violates fixed non-empty bound");
    }
    for (const auto& [parameter_id, source] : sources) {
      require_program_local_identity(parameter_id, child_path(path, "value_sources") + " key");
      if (find_member(parameters, parameter_id) != nullptr) {
        fail(std::string(path) + ": parameter '" + parameter_id +
             "' is supplied by both parameters and value_sources");
      }
      verify_value_source(source, child_path(child_path(path, "value_sources"), parameter_id));
    }
  }
  if (const auto* metadata = find_member(node, "metadata"); metadata != nullptr) {
    (void)require_object(*metadata, child_path(path, "metadata"));
  }
  const auto& children = require_array(require_member(node, "children", path),
                                       child_path(path, "children"));
  if (children.size() > kMaximumProgramChildren) {
    fail(std::string(path) + ": child count exceeds fixed bound");
  }
  for (std::size_t index = 0U; index < children.size(); ++index) {
    verify_tree(children[index], index_path(child_path(path, "children"), index),
                depth + 1U, count, instance_ids);
  }
}

void verify_optional_top_level_shapes(const CanonicalValue::Object& root) {
  if (const auto* compatibility = find_member(root, "compatibility");
      compatibility != nullptr) {
    const auto& object = require_object(*compatibility, "program.compatibility");
    require_fields(object, {}, {"lanes", "assistant", "requires", "tags"},
                   "program.compatibility");
    if (const auto* lanes = find_member(object, "lanes"); lanes != nullptr) {
      const auto values = parse_text_set(lanes, "program.compatibility.lanes");
      for (const auto& lane : values) {
        if (lane != "manyforge-only" && lane != "isaac-sim" &&
            lane != "real-robot") {
          fail("program.compatibility.lanes: unknown lane");
        }
      }
    }
    if (const auto* assistant = find_member(object, "assistant");
        assistant != nullptr) {
      const std::string& spelling =
          require_string(*assistant, "program.compatibility.assistant");
      if (spelling != "required" && spelling != "optional" &&
          spelling != "forbidden") {
        fail("program.compatibility.assistant: invalid disposition");
      }
    }
    (void)parse_text_set(find_member(object, "requires"),
                         "program.compatibility.requires");
    (void)parse_text_set(find_member(object, "tags"),
                         "program.compatibility.tags");
  }
  if (const auto* viewer = find_member(root, "viewer"); viewer != nullptr) {
    (void)require_object(*viewer, "program.viewer");
  }
  for (const std::string_view field : {"scene", "parameters",
                                       "contact_policies"}) {
    if (const auto* value = find_member(root, field); value != nullptr) {
      (void)require_array(*value, child_path("program", field));
    }
  }
  if (const auto* blackboard = find_member(root, "blackboard");
      blackboard != nullptr) {
    (void)require_object(*blackboard, "program.blackboard");
  }
}

[[nodiscard]] bool declared_scene_resource_runtime_object(
    const CanonicalValue::Object& root, std::string_view runtime_object_id) {
  const auto* blackboard_value = find_member(root, "blackboard");
  if (blackboard_value == nullptr) {
    return false;
  }
  const auto& blackboard = require_object(*blackboard_value,
                                          "program.blackboard");
  const auto* keys_value = find_member(blackboard, "keys");
  if (keys_value == nullptr) {
    return false;
  }
  const auto& keys = require_array(*keys_value, "program.blackboard.keys");
  return std::count_if(keys.begin(), keys.end(), [&](const auto& value) {
           const auto& key = require_object(value, "program.blackboard.keys[]");
           const auto* name = find_member(key, "key");
           const auto* type = find_member(key, "type");
           return name != nullptr && type != nullptr &&
                  require_string(*name, "program.blackboard.keys[].key") ==
                      runtime_object_id &&
                  require_string(*type, "program.blackboard.keys[].type") ==
                      "scene_resource";
         }) == 1;
}

struct ParsedContactPair final {
  core::ProgramContactGraspBodySelectorDTO body_selector;
  core::RuntimeObjectId runtime_object_id;
};

[[nodiscard]] ParsedContactPair parse_contact_grasp_pair(
    const CanonicalValue& value, std::string_view path) {
  const auto& pair = require_object(value, path);
  require_fields(pair, {"body_a", "body_b"}, {}, path);

  std::optional<core::ProgramContactGraspBodySelectorDTO> body_selector;
  std::optional<core::RuntimeObjectId> runtime_object_id;
  const auto parse_side = [&](const CanonicalValue& side_value,
                              std::string side_path) {
    const auto& side = require_object(side_value, side_path);
    const std::string& kind = require_string(
        require_member(side, "kind", side_path), child_path(side_path, "kind"));
    if (kind == "component_model_body") {
      require_fields(side,
                     {"kind", "component_role_id",
                      "collision_body_role_id"},
                     {"model_role_id"}, side_path);
      if (body_selector.has_value()) {
        fail(std::string(path) +
             ": contact_grasp pair has two component-model sides");
      }
      std::optional<core::ModelRoleId> model_role;
      if (const auto* model = find_member(side, "model_role_id");
          model != nullptr) {
        model_role = parse_program_identity<core::ModelRoleId>(
            *model, child_path(side_path, "model_role_id"));
      }
      body_selector = core::ProgramContactGraspBodySelectorDTO{
          parse_program_identity<core::ComponentRoleId>(
              require_member(side, "component_role_id", side_path),
              child_path(side_path, "component_role_id")),
          std::move(model_role),
          parse_program_identity<core::CollisionBodyRoleId>(
              require_member(side, "collision_body_role_id", side_path),
              child_path(side_path, "collision_body_role_id"))};
      return;
    }
    if (kind == "runtime_object") {
      require_fields(side, {"kind", "runtime_object_id"}, {}, side_path);
      if (runtime_object_id.has_value()) {
        fail(std::string(path) +
             ": contact_grasp pair has two runtime-object sides");
      }
      runtime_object_id = parse_program_identity<core::RuntimeObjectId>(
          require_member(side, "runtime_object_id", side_path),
          child_path(side_path, "runtime_object_id"));
      return;
    }
    fail(side_path + ": contact_grasp requires component_model_body or "
                       "runtime_object");
  };

  parse_side(require_member(pair, "body_a", path), child_path(path, "body_a"));
  parse_side(require_member(pair, "body_b", path), child_path(path, "body_b"));
  if (!body_selector.has_value() || !runtime_object_id.has_value()) {
    fail(std::string(path) +
         ": contact_grasp requires exactly one component body and one "
         "runtime object");
  }
  return {std::move(*body_selector), std::move(*runtime_object_id)};
}

[[nodiscard]] std::vector<core::ProgramContactGraspTemplateDTO>
parse_contact_grasp_templates(
    const CanonicalValue::Object& root,
    const std::vector<core::ProgramTargetRoleRequirementDTO>& targets) {
  const auto* policies_value = find_member(root, "contact_policies");
  if (policies_value == nullptr) {
    return {};
  }
  const auto& policies = require_array(*policies_value,
                                       "program.contact_policies");
  if (policies.size() > core::kMaxProgramContractRoles) {
    fail("program.contact_policies: exceeds fixed bound");
  }
  std::vector<core::ProgramContactGraspTemplateDTO> result;
  result.reserve(policies.size());
  for (std::size_t index = 0U; index < policies.size(); ++index) {
    const std::string path = index_path("program.contact_policies", index);
    const auto& policy = require_object(policies[index], path);
    const auto* mode_value = find_member(policy, "mode");
    if (mode_value == nullptr ||
        require_string(*mode_value, child_path(path, "mode")) !=
            "contact_grasp") {
      // N5d adds one binding projection for contact-grasp. Other policy modes
      // retain their existing Program validation and runtime ownership.
      continue;
    }
    require_fields(policy,
                   {"api_version", "policy_id", "version", "mode",
                    "sensing_mode", "default_unmatched_response", "zones"},
                   {"description"}, path);
    if (require_string(require_member(policy, "api_version", path),
                       child_path(path, "api_version")) !=
        "manyforge.contact_policy.v1") {
      fail(path + ": unsupported contact-policy api_version");
    }
    if (require_string(require_member(policy, "sensing_mode", path),
                       child_path(path, "sensing_mode")) != "geometry_only") {
      fail(path +
           ": I6/N5d accepts only geometry-only contact_grasp policies");
    }
    if (require_string(
            require_member(policy, "default_unmatched_response", path),
            child_path(path, "default_unmatched_response")) != "hard_stop") {
      fail(path +
           ": contact_grasp unmatched contact must use hard_stop in this "
           "qualified profile");
    }
    const auto policy_id = parse_program_identity<core::ContactPolicyId>(
        require_member(policy, "policy_id", path),
        child_path(path, "policy_id"));
    const auto policy_revision =
        core::ContactPolicyRevision(require_string(
            require_member(policy, "version", path),
            child_path(path, "version")));
    if (!is_sha256_digest(policy_revision.value()) ||
        !detail::verify_content_identity(
            policies[index],
            detail::ContentIdentitySpec{
                .identity_domain =
                    "catalog-record-revision/contact_policy",
                .api_version = "manyforge.contact_policy.v1",
                .own_field = "version"})) {
      fail(path + ": ContactPolicyRevision does not match the policy value");
    }

    const auto& zones = require_array(require_member(policy, "zones", path),
                                      child_path(path, "zones"));
    if (zones.size() != 1U) {
      fail(path + ": contact_grasp currently requires exactly one zone");
    }
    const std::string zone_path = index_path(child_path(path, "zones"), 0U);
    const auto& zone = require_object(zones.front(), zone_path);
    require_fields(zone,
                   {"zone_id", "body_pairs", "violation_response",
                    "max_penetration_m",
                    "terminal_window_start_fraction"},
                   {"notes"}, zone_path);
    require_program_local_identity(
        require_string(require_member(zone, "zone_id", zone_path),
                       child_path(zone_path, "zone_id")),
        child_path(zone_path, "zone_id"));
    if (require_string(require_member(zone, "violation_response", zone_path),
                       child_path(zone_path, "violation_response")) !=
        "hard_stop") {
      fail(zone_path +
           ": contact_grasp violation must use hard_stop in this qualified "
           "profile");
    }
    const double max_penetration = require_number(
        require_member(zone, "max_penetration_m", zone_path),
        child_path(zone_path, "max_penetration_m"));
    const double terminal_start = require_number(
        require_member(zone, "terminal_window_start_fraction", zone_path),
        child_path(zone_path, "terminal_window_start_fraction"));
    if (max_penetration <= 0.0 || terminal_start < 0.0 ||
        terminal_start > 1.0) {
      fail(zone_path + ": contact_grasp bounds are outside their valid range");
    }

    const auto& pairs = require_array(
        require_member(zone, "body_pairs", zone_path),
        child_path(zone_path, "body_pairs"));
    if (pairs.empty() ||
        pairs.size() > core::kMaxProgramRoleRequirements) {
      fail(zone_path + ": contact_grasp body-pair count violates bounds");
    }
    std::vector<core::ProgramContactGraspBodySelectorDTO> selectors;
    selectors.reserve(pairs.size());
    std::optional<core::RuntimeObjectId> runtime_object_id;
    for (std::size_t pair_index = 0U; pair_index < pairs.size();
         ++pair_index) {
      auto parsed = parse_contact_grasp_pair(
          pairs[pair_index],
          index_path(child_path(zone_path, "body_pairs"), pair_index));
      if (runtime_object_id.has_value() &&
          *runtime_object_id != parsed.runtime_object_id) {
        fail(zone_path +
             ": every contact_grasp pair must bind the same runtime object");
      }
      runtime_object_id = parsed.runtime_object_id;
      selectors.push_back(std::move(parsed.body_selector));
    }
    const auto selector_key = [](const auto& selector) {
      return std::tuple(
          selector.component_role_id.value(),
          selector.model_role_id.has_value()
              ? selector.model_role_id->value()
              : std::string{},
          selector.collision_body_role_id.value());
    };
    if (!std::is_sorted(selectors.begin(), selectors.end(),
                        [&](const auto& left, const auto& right) {
                          return selector_key(left) < selector_key(right);
                        }) ||
        std::adjacent_find(selectors.begin(), selectors.end(),
                           [&](const auto& left, const auto& right) {
                             return selector_key(left) == selector_key(right);
                           }) != selectors.end()) {
      fail(zone_path +
           ": contact_grasp body pairs must be unique and canonical-sorted");
    }
    for (const auto& selector : selectors) {
      const auto matches = std::count_if(
          targets.begin(), targets.end(), [&](const auto& target) {
            return target.component_role_id == selector.component_role_id &&
                   std::find(target.required_collision_body_roles.begin(),
                             target.required_collision_body_roles.end(),
                             selector.collision_body_role_id) !=
                       target.required_collision_body_roles.end() &&
                   (!selector.model_role_id.has_value() ||
                    std::find(target.required_model_roles.begin(),
                              target.required_model_roles.end(),
                              *selector.model_role_id) !=
                        target.required_model_roles.end());
          });
      if (matches != 1) {
        fail(path +
             ": contact_grasp body selector does not identify exactly one "
             "Program target role");
      }
    }
    if (!runtime_object_id.has_value() ||
        !declared_scene_resource_runtime_object(
            root, runtime_object_id->value())) {
      fail(path +
           ": contact_grasp runtime object is not one declared "
           "scene_resource blackboard key");
    }
    result.push_back(core::ProgramContactGraspTemplateDTO{
        policy_id, policy_revision, *runtime_object_id, std::move(selectors),
        max_penetration, terminal_start});
  }
  if (!std::is_sorted(result.begin(), result.end(), [](const auto& left,
                                                       const auto& right) {
        return left.contact_policy_id < right.contact_policy_id;
      }) ||
      std::adjacent_find(result.begin(), result.end(),
                         [](const auto& left, const auto& right) {
                           return left.contact_policy_id ==
                                  right.contact_policy_id;
                         }) != result.end()) {
    fail("program.contact_policies: must be unique and canonical-sorted");
  }
  return result;
}

[[nodiscard]] core::ProgramBindingContractDTO extract_contract(
    const CanonicalValue& root_value, const RetainedBundle& retained_bundle) {
  const auto& root = require_object(root_value, "program");
  require_fields(
      root,
      {"api_version", "program_id", "program_revision", "program_profile",
       "name", "catalog", "component_roles", "target_roles", "tree"},
      {"description", "compatibility", "viewer", "scene", "parameters",
       "blackboard", "contact_policies"},
      "program");
  if (require_string(require_member(root, "api_version", "program"),
                     "program.api_version") != core::kProgramApiVersion) {
    fail("program.api_version: expected manyforge.program.v1");
  }
  if (require_string(require_member(root, "program_profile", "program"),
                     "program.program_profile") !=
      core::kComposableCellProgramProfile) {
    fail("program.program_profile: expected composable-cell-v1");
  }
  const std::string& name = require_string(
      require_member(root, "name", "program"), "program.name");
  if (name.empty()) {
    fail("program.name: expected non-empty text");
  }
  if (const auto* description = find_member(root, "description");
      description != nullptr) {
    (void)require_string(*description, "program.description");
  }

  const auto& bundle = retained_bundle.value();
  const auto& catalog = require_object(require_member(root, "catalog", "program"),
                                       "program.catalog");
  require_exact_fields(catalog, {"api_version", "content_version"},
                       "program.catalog");
  if (require_string(require_member(catalog, "api_version", "program.catalog"),
                     "program.catalog.api_version") !=
          "manyforge.node_catalog.v2" ||
      require_string(
          require_member(catalog, "content_version", "program.catalog"),
          "program.catalog.content_version") !=
          bundle.catalogs.node_environment.content_version) {
    fail("Program catalog selector differs from the retained node environment");
  }

  const auto& component_roles = require_object(
      require_member(root, "component_roles", "program"),
      "program.component_roles");
  if (component_roles.empty() ||
      component_roles.size() > core::kMaxProgramContractRoles) {
    fail("program.component_roles: violates fixed non-empty role bound");
  }
  std::vector<core::ProgramComponentRoleRequirementDTO> components;
  components.reserve(component_roles.size());
  for (const auto& [role_name, role_value] : component_roles) {
    require_program_local_identity(role_name, "program.component_roles key");
    const std::string path = "program.component_roles." + role_name;
    const auto& role = require_object(role_value, path);
    require_fields(role, {},
                   {"required_component_type_tags", "required_model_roles",
                    "required_attachment_interfaces"},
                   path);
    components.push_back(core::ProgramComponentRoleRequirementDTO{
        core::ComponentRoleId(role_name),
        parse_text_set(find_member(role, "required_component_type_tags"),
                       child_path(path, "required_component_type_tags")),
        parse_identity_set<core::ModelRoleId>(
            find_member(role, "required_model_roles"),
            child_path(path, "required_model_roles")),
        parse_identity_set<core::AttachmentInterfaceId>(
            find_member(role, "required_attachment_interfaces"),
            child_path(path, "required_attachment_interfaces")),
    });
  }

  const auto& target_roles = require_object(
      require_member(root, "target_roles", "program"),
      "program.target_roles");
  if (target_roles.size() > core::kMaxProgramContractRoles) {
    fail("program.target_roles: exceeds fixed role bound");
  }
  std::vector<core::ProgramTargetRoleRequirementDTO> targets;
  targets.reserve(target_roles.size());
  for (const auto& [role_name, role_value] : target_roles) {
    require_program_local_identity(role_name, "program.target_roles key");
    const std::string path = "program.target_roles." + role_name;
    const auto& role = require_object(role_value, path);
    require_fields(
        role, {"component_role", "required_capabilities"},
        {"required_frame_roles", "required_resource_roles",
         "required_collision_body_roles", "required_model_roles"},
        path);
    const core::ComponentRoleId component_role =
        parse_program_identity<core::ComponentRoleId>(
            require_member(role, "component_role", path),
            child_path(path, "component_role"));
    if (std::none_of(components.begin(), components.end(),
                     [&](const auto& component) {
                       return component.component_role_id == component_role;
                     })) {
      fail(path + ": references an undeclared component role");
    }

    const auto& capability_values = require_array(
        require_member(role, "required_capabilities", path),
        child_path(path, "required_capabilities"));
    if (capability_values.empty() ||
        capability_values.size() > core::kMaxProgramRoleRequirements) {
      fail(path + ": required_capabilities violates fixed non-empty bound");
    }
    std::vector<core::ProgramCapabilityRequirementDTO> capabilities;
    capabilities.reserve(capability_values.size());
    for (std::size_t index = 0U; index < capability_values.size(); ++index) {
      const std::string capability_path =
          index_path(child_path(path, "required_capabilities"), index);
      const auto& capability =
          require_object(capability_values[index], capability_path);
      require_exact_fields(capability,
                           {"capability_id", "parameters", "disposition"},
                           capability_path);
      const core::CapabilityId capability_id =
          parse_program_identity<core::CapabilityId>(
              require_member(capability, "capability_id", capability_path),
              child_path(capability_path, "capability_id"));
      (void)require_object(
          require_member(capability, "parameters", capability_path),
          child_path(capability_path, "parameters"));
      const auto* definition =
          require_capability_definition(bundle, capability_id);
      capabilities.push_back(core::ProgramCapabilityRequirementDTO{
          capability_id,
          core::CapabilityDefinitionRevision(
              std::string(definition->capability_definition_revision)),
          parse_disposition(
              require_member(capability, "disposition", capability_path),
              child_path(capability_path, "disposition")),
      });
    }
    if (!std::is_sorted(
            capabilities.begin(), capabilities.end(),
            [](const auto& left, const auto& right) {
              return left.capability_id < right.capability_id;
            }) ||
        std::adjacent_find(
            capabilities.begin(), capabilities.end(),
            [](const auto& left, const auto& right) {
              return left.capability_id == right.capability_id;
            }) != capabilities.end()) {
      fail(path +
           ": required_capabilities must be unique and canonical-sorted");
    }

    targets.push_back(core::ProgramTargetRoleRequirementDTO{
        core::TargetRoleId(role_name), component_role,
        std::move(capabilities),
        parse_identity_set<core::FrameRoleId>(
            find_member(role, "required_frame_roles"),
            child_path(path, "required_frame_roles")),
        parse_identity_set<core::ResourceRoleId>(
            find_member(role, "required_resource_roles"),
            child_path(path, "required_resource_roles")),
        parse_identity_set<core::CollisionBodyRoleId>(
            find_member(role, "required_collision_body_roles"),
            child_path(path, "required_collision_body_roles")),
        parse_identity_set<core::ModelRoleId>(
            find_member(role, "required_model_roles"),
            child_path(path, "required_model_roles")),
    });
  }

  verify_optional_top_level_shapes(root);
  std::size_t tree_count = 0U;
  std::set<std::string> instance_ids;
  verify_tree(require_member(root, "tree", "program"), "program.tree", 0U,
              tree_count, instance_ids);

  const std::string& revision = require_string(
      require_member(root, "program_revision", "program"),
      "program.program_revision");
  if (!is_sha256_digest(revision)) {
    fail("program.program_revision: expected lowercase sha256 digest");
  }
  auto contact_grasp_templates = parse_contact_grasp_templates(root, targets);
  return core::ProgramBindingContractDTO{
      parse_program_identity<core::ProgramId>(
          require_member(root, "program_id", "program"),
          "program.program_id"),
      core::ProgramRevision(revision),
      bundle.catalogs.node_environment,
      std::move(components),
      std::move(targets),
      std::move(contact_grasp_templates),
  };
}

}  // namespace

namespace detail {

class ProgramBindingContractConstructionAuthority final {
 public:
  [[nodiscard]] static std::shared_ptr<const ProgramBindingContract> construct(
      core::ProgramBindingContractDTO value, CanonicalValue root,
      std::string bytes) {
    return std::shared_ptr<const ProgramBindingContract>(
        new ProgramBindingContract(std::move(value), std::move(root),
                                   std::move(bytes)));
  }
};

}  // namespace detail

std::shared_ptr<const ProgramBindingContract>
verify_program_binding_contract(std::string_view canonical_program_bytes,
                                const RetainedBundle& retained_bundle) {
  try {
    if (canonical_program_bytes.size() > kMaxArtifactEncodedBytes) {
      fail("canonical Program exceeds fixed artifact byte bound");
    }
    if (!verify_retained_bundle_identity(retained_bundle)) {
      fail("Program binding contract selected an invalid retained bundle");
    }
    const auto parsed = parse_strict_json_model_yaml(canonical_program_bytes);
    CanonicalValue root = parsed.root();
    if (canonical_json(root) != canonical_program_bytes) {
      fail("Program binding input is not schema-normalized RFC 8785 JSON");
    }
    core::ProgramBindingContractDTO value =
        extract_contract(root, retained_bundle);
    if (!detail::verify_content_identity(
            root,
            detail::ContentIdentitySpec{
                .identity_domain = "program-revision",
                .api_version = std::string(core::kProgramApiVersion),
                .own_field = "program_revision",
            })) {
      fail("ProgramRevision does not match the complete canonical Program");
    }
    return detail::ProgramBindingContractConstructionAuthority::construct(
        std::move(value), std::move(root),
        std::string(canonical_program_bytes));
  } catch (const ProgramContractError&) {
    throw;
  } catch (const std::exception& error) {
    fail(std::string("Program binding-contract verification failed: ") +
         error.what());
  }
}

}  // namespace manyforge::deployment
