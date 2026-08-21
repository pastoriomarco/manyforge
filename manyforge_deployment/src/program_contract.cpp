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
constexpr std::size_t kMaximumProgramAllowedSkills = 256U;
constexpr std::size_t kMaximumProgramSkillIdBytes = 128U;

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

[[nodiscard]] core::ProgramModificationPolicyValue
parse_modification_value(const CanonicalValue& value, std::string_view path) {
  const auto& spelling = require_string(value, path);
  if (spelling == "forbidden") {
    return core::ProgramModificationPolicyValue::kForbidden;
  }
  if (spelling == "human_approval") {
    return core::ProgramModificationPolicyValue::kHumanApproval;
  }
  if (spelling == "llm_allowed") {
    return core::ProgramModificationPolicyValue::kLlmAllowed;
  }
  fail(std::string(path) + ": invalid modification-policy value");
}

[[nodiscard]] core::ProgramInterventionDelegate parse_intervention_delegate(
    const CanonicalValue& value, std::string_view path) {
  const auto& spelling = require_string(value, path);
  if (spelling == "forbidden") {
    return core::ProgramInterventionDelegate::kForbidden;
  }
  if (spelling == "human_only") {
    return core::ProgramInterventionDelegate::kHumanOnly;
  }
  if (spelling == "llm_allowed") {
    return core::ProgramInterventionDelegate::kLlmAllowed;
  }
  if (spelling == "program_only") {
    return core::ProgramInterventionDelegate::kProgramOnly;
  }
  fail(std::string(path) + ": invalid intervention-delegate value");
}

[[nodiscard]] core::ProgramRecoveryAction parse_recovery_action(
    const CanonicalValue& value, std::string_view path) {
  const auto& spelling = require_string(value, path);
  if (spelling == "intervention") {
    return core::ProgramRecoveryAction::kIntervention;
  }
  if (spelling == "runtime_override") {
    return core::ProgramRecoveryAction::kRuntimeOverride;
  }
  fail(std::string(path) + ": invalid recovery action");
}

[[nodiscard]] core::ProgramRecoveryPersistenceCeiling
parse_persistence_ceiling(const CanonicalValue& value, std::string_view path) {
  const auto& spelling = require_string(value, path);
  if (spelling == "session_only") {
    return core::ProgramRecoveryPersistenceCeiling::kSessionOnly;
  }
  if (spelling == "runtime_override") {
    return core::ProgramRecoveryPersistenceCeiling::kRuntimeOverride;
  }
  if (spelling == "draft_patch") {
    return core::ProgramRecoveryPersistenceCeiling::kDraftPatch;
  }
  fail(std::string(path) + ": invalid persistence ceiling");
}

[[nodiscard]] core::ProgramOperatorStopPolicy parse_stop_policy(
    const CanonicalValue& value, std::string_view path) {
  const auto& spelling = require_string(value, path);
  if (spelling == "inherit") {
    return core::ProgramOperatorStopPolicy::kInherit;
  }
  if (spelling == "hold") {
    return core::ProgramOperatorStopPolicy::kHold;
  }
  if (spelling == "continue") {
    return core::ProgramOperatorStopPolicy::kContinue;
  }
  fail(std::string(path) + ": invalid operator Stop policy");
}

[[nodiscard]] std::uint8_t require_retry_count(const CanonicalValue& value,
                                                std::string_view path) {
  const double number = require_number(value, path);
  if (number < 0.0 || number > 255.0 || std::trunc(number) != number) {
    fail(std::string(path) + ": expected integer in 0..255");
  }
  return static_cast<std::uint8_t>(number);
}

[[nodiscard]] bool policy_is_default(const core::ProgramNodePolicyDTO& policy) {
  return policy.modification_policy ==
             core::ProgramNodeModificationPolicyDTO{} &&
         policy.recovery_policy == core::ProgramNodeRecoveryPolicyDTO{} &&
         policy.operator_stop_policy ==
             core::ProgramOperatorStopPolicy::kInherit;
}

[[nodiscard]] core::ProgramNodePolicyDTO parse_node_policy(
    const CanonicalValue::Object& node, std::string_view path) {
  core::ProgramNodePolicyDTO result{
      core::ProgramNodeInstanceId(require_string(
          require_member(node, "instance_id", path),
          child_path(path, "instance_id"))),
      {},
      {},
      core::ProgramOperatorStopPolicy::kInherit};
  require_program_local_identity(result.instance_id.value(),
                                 child_path(path, "instance_id"));

  if (const auto* modification = find_member(node, "modification_policy");
      modification != nullptr) {
    const auto& value = require_object(
        *modification, child_path(path, "modification_policy"));
    require_exact_fields(value,
                         {"runtime_override", "persistent_edit",
                          "intervention_delegate"},
                         child_path(path, "modification_policy"));
    result.modification_policy.runtime_override = parse_modification_value(
        require_member(value, "runtime_override",
                       child_path(path, "modification_policy")),
        child_path(child_path(path, "modification_policy"),
                   "runtime_override"));
    result.modification_policy.persistent_edit = parse_modification_value(
        require_member(value, "persistent_edit",
                       child_path(path, "modification_policy")),
        child_path(child_path(path, "modification_policy"),
                   "persistent_edit"));
    result.modification_policy.intervention_delegate =
        parse_intervention_delegate(
            require_member(value, "intervention_delegate",
                           child_path(path, "modification_policy")),
            child_path(child_path(path, "modification_policy"),
                       "intervention_delegate"));
  }

  if (const auto* recovery = find_member(node, "recovery_policy");
      recovery != nullptr) {
    const auto& value =
        require_object(*recovery, child_path(path, "recovery_policy"));
    require_exact_fields(
        value,
        {"allow_intervention", "allowed_recovery_actions", "max_retries",
         "allowed_skills", "persistence_ceiling"},
        child_path(path, "recovery_policy"));
    const auto& allow = require_member(
        value, "allow_intervention", child_path(path, "recovery_policy"));
    if (!allow.is_bool()) {
      fail(child_path(child_path(path, "recovery_policy"),
                      "allow_intervention") +
           ": expected boolean");
    }
    result.recovery_policy.allow_intervention = allow.as_bool();

    const std::string actions_path =
        child_path(child_path(path, "recovery_policy"),
                   "allowed_recovery_actions");
    const auto& actions = require_array(
        require_member(value, "allowed_recovery_actions",
                       child_path(path, "recovery_policy")),
        actions_path);
    if (actions.size() > 2U) {
      fail(actions_path + ": exceeds the closed action bound");
    }
    for (std::size_t index = 0U; index < actions.size(); ++index) {
      result.recovery_policy.allowed_recovery_actions.push_back(
          parse_recovery_action(actions[index], index_path(actions_path, index)));
    }
    if (!std::is_sorted(result.recovery_policy.allowed_recovery_actions.begin(),
                        result.recovery_policy.allowed_recovery_actions.end()) ||
        std::adjacent_find(
            result.recovery_policy.allowed_recovery_actions.begin(),
            result.recovery_policy.allowed_recovery_actions.end()) !=
            result.recovery_policy.allowed_recovery_actions.end()) {
      fail(actions_path + ": must be duplicate-free and canonical-sorted");
    }

    result.recovery_policy.max_retries = require_retry_count(
        require_member(value, "max_retries",
                       child_path(path, "recovery_policy")),
        child_path(child_path(path, "recovery_policy"), "max_retries"));

    const std::string skills_path =
        child_path(child_path(path, "recovery_policy"), "allowed_skills");
    const auto& skills = require_array(
        require_member(value, "allowed_skills",
                       child_path(path, "recovery_policy")),
        skills_path);
    if (skills.size() > kMaximumProgramAllowedSkills) {
      fail(skills_path + ": exceeds the 256-skill bound");
    }
    for (std::size_t index = 0U; index < skills.size(); ++index) {
      const std::string item_path = index_path(skills_path, index);
      const auto& skill = require_string(skills[index], item_path);
      if (skill.size() > kMaximumProgramSkillIdBytes) {
        fail(item_path + ": exceeds the 128-byte skill ID bound");
      }
      require_program_local_identity(skill, item_path);
      result.recovery_policy.allowed_skills.push_back(skill);
    }
    if (!std::is_sorted(result.recovery_policy.allowed_skills.begin(),
                        result.recovery_policy.allowed_skills.end()) ||
        std::adjacent_find(result.recovery_policy.allowed_skills.begin(),
                           result.recovery_policy.allowed_skills.end()) !=
            result.recovery_policy.allowed_skills.end()) {
      fail(skills_path + ": must be duplicate-free and canonical-sorted");
    }
    result.recovery_policy.persistence_ceiling = parse_persistence_ceiling(
        require_member(value, "persistence_ceiling",
                       child_path(path, "recovery_policy")),
        child_path(child_path(path, "recovery_policy"),
                   "persistence_ceiling"));
  }

  if (const auto* stop = find_member(node, "operator_stop_policy");
      stop != nullptr) {
    result.operator_stop_policy =
        parse_stop_policy(*stop, child_path(path, "operator_stop_policy"));
  }

  const auto& actions = result.recovery_policy.allowed_recovery_actions;
  const bool intervention =
      std::find(actions.begin(), actions.end(),
                core::ProgramRecoveryAction::kIntervention) != actions.end();
  if (result.recovery_policy.allow_intervention != intervention) {
    fail(std::string(path) +
         ".recovery_policy.allow_intervention must match intervention");
  }
  if (!result.recovery_policy.allowed_skills.empty() && !intervention) {
    fail(std::string(path) +
         ".recovery_policy.allowed_skills requires intervention");
  }
  const bool runtime_override =
      std::find(actions.begin(), actions.end(),
                core::ProgramRecoveryAction::kRuntimeOverride) !=
      actions.end();
  if (runtime_override &&
      result.modification_policy.runtime_override ==
          core::ProgramModificationPolicyValue::kForbidden) {
    fail(std::string(path) +
         ".recovery_policy.runtime_override requires modification permission");
  }
  if (result.recovery_policy.persistence_ceiling ==
          core::ProgramRecoveryPersistenceCeiling::kRuntimeOverride &&
      result.modification_policy.runtime_override ==
          core::ProgramModificationPolicyValue::kForbidden) {
    fail(std::string(path) +
         ".recovery_policy.runtime_override persistence requires permission");
  }
  if (result.recovery_policy.persistence_ceiling ==
          core::ProgramRecoveryPersistenceCeiling::kDraftPatch &&
      result.modification_policy.persistent_edit ==
          core::ProgramModificationPolicyValue::kForbidden) {
    fail(std::string(path) +
         ".recovery_policy.draft_patch persistence requires permission");
  }
  if (!result.recovery_policy.allowed_recovery_actions.empty() ||
      result.recovery_policy.allow_intervention ||
      result.recovery_policy.max_retries != 0U ||
      !result.recovery_policy.allowed_skills.empty() ||
      result.recovery_policy.persistence_ceiling !=
          core::ProgramRecoveryPersistenceCeiling::kSessionOnly) {
    if (actions.empty()) {
      fail(std::string(path) +
           ".recovery_policy non-default values require an action");
    }
  }
  if (!result.recovery_policy.allowed_skills.empty()) {
    fail(std::string(path) +
         ".recovery_policy.allowed_skills requires an exact "
         "revision-qualified retained skill referent; ID-only assistant "
         "registry rows are insufficient");
  }
  return result;
}

[[nodiscard]] CanonicalValue node_policy_projection(
    const core::ProgramNodePolicyDTO& policy) {
  CanonicalValue::Object result;
  if (policy.modification_policy !=
      core::ProgramNodeModificationPolicyDTO{}) {
    result.emplace_back(
        "modification_policy",
        CanonicalValue(CanonicalValue::Object{
            {"intervention_delegate", CanonicalValue(std::string(
                                           core::to_string(policy
                                               .modification_policy
                                               .intervention_delegate)))},
            {"persistent_edit", CanonicalValue(std::string(
                                     core::to_string(policy.modification_policy
                                                         .persistent_edit)))},
            {"runtime_override", CanonicalValue(std::string(
                                      core::to_string(policy.modification_policy
                                                          .runtime_override)))},
        }));
  }
  if (policy.recovery_policy != core::ProgramNodeRecoveryPolicyDTO{}) {
    CanonicalValue::Array actions;
    for (const auto action :
         policy.recovery_policy.allowed_recovery_actions) {
      actions.emplace_back(std::string(core::to_string(action)));
    }
    CanonicalValue::Array skills;
    for (const auto& skill : policy.recovery_policy.allowed_skills) {
      skills.emplace_back(skill);
    }
    result.emplace_back(
        "recovery_policy",
        CanonicalValue(CanonicalValue::Object{
            {"allow_intervention",
             CanonicalValue(policy.recovery_policy.allow_intervention)},
            {"allowed_recovery_actions", CanonicalValue(std::move(actions))},
            {"allowed_skills", CanonicalValue(std::move(skills))},
            {"max_retries", CanonicalValue(static_cast<double>(
                                policy.recovery_policy.max_retries))},
            {"persistence_ceiling",
             CanonicalValue(std::string(core::to_string(
                 policy.recovery_policy.persistence_ceiling)))},
        }));
  }
  if (policy.operator_stop_policy !=
      core::ProgramOperatorStopPolicy::kInherit) {
    result.emplace_back(
        "operator_stop_policy",
        CanonicalValue(std::string(
            core::to_string(policy.operator_stop_policy))));
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue normalize_program_tree(
    const CanonicalValue& value, std::string_view path) {
  const auto& node = require_object(value, path);
  require_fields(node, {"node_id", "instance_id", "parameters", "children"},
                 {"metadata", "value_sources", "modification_policy",
                  "recovery_policy", "operator_stop_policy"},
                 path);
  const auto& instance = require_string(
      require_member(node, "instance_id", path),
      child_path(path, "instance_id"));
  require_program_local_identity(instance, child_path(path, "instance_id"));
  const auto policy = parse_node_policy(node, path);
  CanonicalValue::Object normalized;
  normalized.reserve(node.size());
  for (const auto& [name, child] : node) {
    if (name == "modification_policy" || name == "recovery_policy" ||
        name == "operator_stop_policy") {
      continue;
    }
    if (name == "children") {
      const auto& children = require_array(child, child_path(path, name));
      if (children.size() > kMaximumProgramChildren) {
        fail(std::string(path) + ": child count exceeds fixed bound");
      }
      CanonicalValue::Array normalized_children;
      normalized_children.reserve(children.size());
      for (std::size_t index = 0U; index < children.size(); ++index) {
        normalized_children.push_back(normalize_program_tree(
            children[index], index_path(child_path(path, "children"), index)));
      }
      normalized.emplace_back(name, CanonicalValue(std::move(normalized_children)));
    } else {
      normalized.emplace_back(name, child);
    }
  }
  if (!policy_is_default(policy)) {
    const auto projected = node_policy_projection(policy);
    for (const auto& [name, child] : projected.as_object()) {
      normalized.emplace_back(name, child);
    }
  }
  return CanonicalValue(std::move(normalized));
}

void verify_tree(const CanonicalValue& value, std::string_view path,
                 std::size_t depth, std::size_t& count,
                 std::set<std::string>& instance_ids,
                 std::vector<core::ProgramNodePolicyDTO>& node_policies) {
  if (depth > kMaximumProgramTreeDepth || ++count > kMaximumProgramTreeNodes) {
    fail(std::string(path) + ": Program tree exceeds fixed traversal bounds");
  }
  const auto& node = require_object(value, path);
  require_fields(node, {"node_id", "instance_id", "parameters", "children"},
                 {"metadata", "value_sources", "modification_policy",
                  "recovery_policy", "operator_stop_policy"},
                 path);
  const std::string& node_id =
      require_string(require_member(node, "node_id", path), child_path(path, "node_id"));
  const std::string& instance_id =
      require_string(require_member(node, "instance_id", path), child_path(path, "instance_id"));
  require_program_local_identity(node_id, child_path(path, "node_id"));
  require_program_local_identity(instance_id, child_path(path, "instance_id"));
  if (!instance_ids.insert(instance_id).second) {
    fail(std::string(path) + ": duplicate tree instance_id");
  }
  const auto policy = parse_node_policy(node, path);
  if (!policy_is_default(policy)) {
    node_policies.push_back(policy);
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
                depth + 1U, count, instance_ids, node_policies);
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
  std::vector<core::ProgramNodePolicyDTO> node_policies;
  verify_tree(require_member(root, "tree", "program"), "program.tree", 0U,
              tree_count, instance_ids, node_policies);
  std::sort(node_policies.begin(), node_policies.end(),
            [](const auto& left, const auto& right) {
              return left.instance_id < right.instance_id;
            });

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
      std::move(node_policies),
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
    const CanonicalValue source_root = parsed.root();
    if (canonical_json(source_root) != canonical_program_bytes) {
      fail("Program binding input is not schema-normalized RFC 8785 JSON");
    }
    CanonicalValue root = source_root;
    if (root.find_object_member("tree") != nullptr) {
      CanonicalValue::Object members;
      const auto& source_members = require_object(root, "program");
      members.reserve(source_members.size());
      for (const auto& [name, value] : source_members) {
        if (name == "tree") {
          members.emplace_back(name, normalize_program_tree(value, "program.tree"));
        } else {
          members.emplace_back(name, value);
        }
      }
      root = CanonicalValue(std::move(members));
    }
    core::ProgramBindingContractDTO value =
        extract_contract(root, retained_bundle);
    const std::string normalized_bytes = canonical_json(root);
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
        std::move(value), std::move(root), normalized_bytes);
  } catch (const ProgramContractError&) {
    throw;
  } catch (const std::exception& error) {
    fail(std::string("Program binding-contract verification failed: ") +
         error.what());
  }
}

}  // namespace manyforge::deployment
