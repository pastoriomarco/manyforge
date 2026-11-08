#include "program_contact_envelope_codec.hpp"
#include "manyforge_deployment/program_contract.hpp"
#include "manyforge_deployment/program_reference_validator.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <manyforge_core/active_tcp_blackboard_key.hpp>
#include "content_identity_internal.hpp"
#include "embedded_build_evidence.hpp"
#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "manyforge_deployment/expression.hpp"
#include "manyforge_deployment/io_signal_catalog.hpp"
#include "manyforge_deployment/program_value_path.hpp"

namespace manyforge::deployment {
namespace {

constexpr std::size_t kMaximumIdentityBytes = 128U;
constexpr std::size_t kMaximumProgramTreeDepth = 256U;
constexpr std::size_t kMaximumProgramTreeNodes = 100000U;
constexpr std::size_t kMaximumProgramChildren = 10000U;

[[noreturn]] void fail(std::string message) {
  throw ProgramContractError(std::move(message));
}

[[nodiscard]] const CanonicalValue::Object &
require_object(const CanonicalValue &value, std::string_view path) {
  const auto *object = std::get_if<CanonicalValue::Object>(&value.storage());
  if (object == nullptr) {
    fail(std::string(path) + ": expected object");
  }
  return *object;
}

[[nodiscard]] const CanonicalValue::Array &
require_array(const CanonicalValue &value, std::string_view path) {
  const auto *array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    fail(std::string(path) + ": expected array");
  }
  return *array;
}

[[nodiscard]] const std::string &require_string(const CanonicalValue &value,
                                                std::string_view path) {
  const auto *text = std::get_if<std::string>(&value.storage());
  if (text == nullptr) {
    fail(std::string(path) + ": expected string");
  }
  return *text;
}

[[nodiscard]] double require_number(const CanonicalValue &value,
                                    std::string_view path) {
  const auto *number = std::get_if<double>(&value.storage());
  if (number == nullptr || !std::isfinite(*number)) {
    fail(std::string(path) + ": expected finite number");
  }
  return *number;
}

[[nodiscard]] const CanonicalValue &
require_member(const CanonicalValue::Object &object, std::string_view name,
               std::string_view path) {
  const auto found =
      std::find_if(object.begin(), object.end(),
                   [name](const auto &member) { return member.first == name; });
  if (found == object.end()) {
    fail(std::string(path) + ": missing required field '" + std::string(name) +
         "'");
  }
  return found->second;
}

[[nodiscard]] const CanonicalValue *
find_member(const CanonicalValue::Object &object,
            std::string_view name) noexcept {
  const auto found =
      std::find_if(object.begin(), object.end(),
                   [name](const auto &member) { return member.first == name; });
  return found == object.end() ? nullptr : &found->second;
}

void require_fields(const CanonicalValue::Object &object,
                    std::initializer_list<std::string_view> required,
                    std::initializer_list<std::string_view> optional,
                    std::string_view path) {
  for (const auto &[name, unused] : object) {
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

void require_exact_fields(const CanonicalValue::Object &object,
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
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
         (value >= '0' && value <= '9');
}

void require_identity(std::string_view value, std::string_view path,
                      std::size_t maximum_bytes = kMaximumIdentityBytes) {
  if (value.empty() || value.size() > maximum_bytes ||
      !is_ascii_alphanumeric(static_cast<unsigned char>(value.front())) ||
      !std::all_of(value.begin() + 1, value.end(), [](unsigned char byte) {
        return is_ascii_alphanumeric(byte) || byte == '.' || byte == '_' ||
               byte == '-';
      })) {
    fail(std::string(path) + ": invalid bounded portable identity");
  }
}

void require_program_local_identity(std::string_view value,
                                    std::string_view path,
                                    std::size_t maximum_bytes = kMaximumIdentityBytes) {
  require_identity(value, path, maximum_bytes);
  if (value.front() < 'a' || value.front() > 'z' ||
      !std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
               byte == '.' || byte == '_' || byte == '-';
      })) {
    fail(std::string(path) + ": expected lower-case Program-local ID");
  }
}

template <typename StrongType>
[[nodiscard]] StrongType parse_program_identity(const CanonicalValue &value,
                                                std::string_view path) {
  const std::string &spelling = require_string(value, path);
  require_program_local_identity(spelling, path);
  return StrongType(spelling);
}

template <typename StrongType>
[[nodiscard]] StrongType parse_direct_reference_identity(
    const CanonicalValue &value, std::string_view path) {
  const auto &spelling = require_string(value, path);
  if constexpr (std::is_same_v<StrongType, core::ControllableComponentId>) {
    require_program_local_identity(spelling, path);
  } else {
    const auto alphanumeric = [](unsigned char ch) {
      return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
             (ch >= '0' && ch <= '9');
    };
    if (spelling.empty() || spelling.size() > 128U ||
        !alphanumeric(static_cast<unsigned char>(spelling.front())) ||
        !std::all_of(spelling.begin(), spelling.end(), [&](unsigned char ch) {
          return alphanumeric(ch) || ch == '_' || ch == '.' || ch == '-';
        }))
      fail(std::string(path) + ": invalid component feature identity");
  }
  return StrongType(spelling);
}

template <typename StrongType>
[[nodiscard]] std::vector<StrongType>
parse_identity_set(const CanonicalValue *value, std::string_view path) {
  if (value == nullptr) {
    return {};
  }
  const auto &array = require_array(*value, path);
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

[[nodiscard]] std::vector<std::string>
parse_text_set(const CanonicalValue *value, std::string_view path) {
  if (value == nullptr) {
    return {};
  }
  const auto &array = require_array(*value, path);
  if (array.size() > core::kMaxProgramRoleRequirements) {
    fail(std::string(path) + ": exceeds fixed Program role bound");
  }
  std::vector<std::string> result;
  result.reserve(array.size());
  for (std::size_t index = 0U; index < array.size(); ++index) {
    const std::string &item =
        require_string(array[index], index_path(path, index));
    require_program_local_identity(item, index_path(path, index));
    result.push_back(item);
  }
  if (!std::is_sorted(result.begin(), result.end()) ||
      std::adjacent_find(result.begin(), result.end()) != result.end()) {
    fail(std::string(path) + ": must be duplicate-free and canonical-sorted");
  }
  return result;
}

[[nodiscard]] const core::CapabilityDefinitionDescriptor *
require_capability_definition(const core::RetainedBundleDTO &bundle,
                              const core::CapabilityId &capability_id) {
  const auto *descriptor =
      core::find_builtin_capability_definition(capability_id.value());
  if (descriptor == nullptr) {
    fail("Program target requires an unknown capability: " +
         capability_id.value());
  }
  std::size_t exact_matches = 0U;
  std::size_t conflicting_matches = 0U;
  for (const auto &record : bundle.catalogs.records) {
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
  // The requirement's revision is the builtin definition. Whether the
  // candidate cell retains that definition is a binding fact: an absent
  // record makes the Program non-executable against this cell, while
  // a duplicate or different revision remains a corrupt/incompatible catalog.
  if (exact_matches > 1U || conflicting_matches != 0U) {
    fail("retained catalog does not contain exactly one selected capability "
         "definition revision");
  }
  return descriptor;
}

void verify_value_source(const CanonicalValue &value, std::string_view path) {
  const auto &source = require_object(value, path);
  const std::string &kind = require_string(require_member(source, "kind", path),
                                           child_path(path, "kind"));
  if (kind == "program_parameter" || kind == "blackboard_key") {
    require_exact_fields(source, {"kind", "reference"}, path);
    const auto &reference = require_string(require_member(source, "reference", path),
                                           child_path(path, "reference"));
    if (!(kind == "blackboard_key" && core::ParseActiveTcpBlackboardKey(reference))) {
      require_program_local_identity(reference, child_path(path, "reference"),
          kind == "blackboard_key" && reference.starts_with("node_outcome.")
              ? 256U : kMaximumIdentityBytes);
    }
    return;
  }
  if (kind == "expression") {
    require_exact_fields(source, {"kind", "expression"}, path);
    (void)require_string(require_member(source, "expression", path),
                         child_path(path, "expression"));
    return;
  }
  if (kind == "blackboard_field") {
    require_exact_fields(source, {"kind", "reference", "field"}, path);
    require_program_local_identity(
        require_string(require_member(source, "reference", path),
                       child_path(path, "reference")),
        child_path(path, "reference"));
    require_program_local_identity(
        require_string(require_member(source, "field", path),
                       child_path(path, "field")),
        child_path(path, "field"));
    return;
  }
  fail(std::string(path) + ": unsupported Program value-source kind");
}

[[nodiscard]] core::ProgramOperatorStopPolicy
parse_stop_policy(const CanonicalValue &value, std::string_view path) {
  const auto &spelling = require_string(value, path);
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

[[nodiscard]] std::uint8_t require_retry_count(const CanonicalValue &value,
                                               std::string_view path) {
  const double number = require_number(value, path);
  if (number < 0.0 ||
      number > static_cast<double>(core::kMaxProgramRecoveryRetries) ||
      std::trunc(number) != number) {
    fail(std::string(path) + ": expected integer in 0..255 (0 is Unlimited)");
  }
  return static_cast<std::uint8_t>(number);
}

[[nodiscard]] bool policy_is_default(const core::ProgramNodePolicyDTO &policy) {
  return policy.recovery_policy == core::ProgramNodeRecoveryPolicyDTO{} &&
         policy.operator_stop_policy ==
             core::ProgramOperatorStopPolicy::kInherit;
}

[[nodiscard]] core::ProgramNodePolicyDTO
parse_node_policy(const CanonicalValue::Object &node, std::string_view path) {
  core::ProgramNodePolicyDTO result{
      core::ProgramNodeInstanceId(
          require_string(require_member(node, "instance_id", path),
                         child_path(path, "instance_id"))),
      {},
      core::ProgramOperatorStopPolicy::kInherit};
  require_program_local_identity(result.instance_id.value(),
                                 child_path(path, "instance_id"));

  if (const auto *recovery = find_member(node, "recovery_policy");
      recovery != nullptr) {
    const auto recovery_path = child_path(path, "recovery_policy");
    const auto &value = require_object(*recovery, recovery_path);
    require_fields(value, {"allow_intervention", "max_retries"},
                   {"intervention_stop_scope"}, recovery_path);
    const auto &allow =
        require_member(value, "allow_intervention", recovery_path);
    if (!allow.is_bool()) {
      fail(child_path(recovery_path, "allow_intervention") +
           ": expected boolean");
    }
    result.recovery_policy.allow_intervention = allow.as_bool();
    result.recovery_policy.max_retries =
        require_retry_count(require_member(value, "max_retries", recovery_path),
                            child_path(recovery_path, "max_retries"));
    if (const auto *scope = find_member(value, "intervention_stop_scope")) {
      const auto scope_path =
          child_path(recovery_path, "intervention_stop_scope");
      const auto &spelling = require_string(*scope, scope_path);
      if (spelling == "own_groups") {
        result.recovery_policy.intervention_stop_scope =
            core::ProgramInterventionStopScope::kOwnGroups;
      } else if (spelling != "all") {
        fail(scope_path + ": expected all or own_groups");
      }
    }
  }

  if (const auto *stop = find_member(node, "operator_stop_policy");
      stop != nullptr) {
    result.operator_stop_policy =
        parse_stop_policy(*stop, child_path(path, "operator_stop_policy"));
  }
  return result;
}

[[nodiscard]] CanonicalValue
node_policy_projection(const core::ProgramNodePolicyDTO &policy) {
  CanonicalValue::Object result;
  if (policy.recovery_policy != core::ProgramNodeRecoveryPolicyDTO{}) {
    CanonicalValue::Object recovery{
        {"allow_intervention",
         CanonicalValue(policy.recovery_policy.allow_intervention)},
        {"max_retries", CanonicalValue(static_cast<double>(
                            policy.recovery_policy.max_retries))},
    };
    if (policy.recovery_policy.intervention_stop_scope !=
        core::ProgramInterventionStopScope::kAll) {
      recovery.emplace_back(
          "intervention_stop_scope",
          CanonicalValue(std::string(core::to_string(
              policy.recovery_policy.intervention_stop_scope))));
    }
    result.emplace_back("recovery_policy", CanonicalValue(std::move(recovery)));
  }
  if (policy.operator_stop_policy !=
      core::ProgramOperatorStopPolicy::kInherit) {
    result.emplace_back("operator_stop_policy",
                        CanonicalValue(std::string(
                            core::to_string(policy.operator_stop_policy))));
  }
  return CanonicalValue(std::move(result));
}

// nullopt inherits; an empty vector means explicit `all` (no named groups).
[[nodiscard]] std::optional<std::vector<std::string>>
execution_groups(const CanonicalValue::Object &node, std::string_view path) {
  const auto *value = find_member(node, "execution_groups");
  if (value == nullptr) {
    return std::nullopt;
  }
  const auto group_path = child_path(path, "execution_groups");
  if (const auto *text = std::get_if<std::string>(&value->storage())) {
    if (*text == "inherit")
      return std::nullopt;
    if (*text == "all")
      return std::vector<std::string>{};
    fail(group_path + ": expected inherit, all, or a nonempty named set");
  }
  const auto &values = require_array(*value, group_path);
  if (values.empty() || values.size() > core::kMaxProgramExecutionGroups) {
    fail(group_path + ": named set violates fixed nonempty bound");
  }
  std::vector<std::string> groups;
  groups.reserve(values.size());
  for (const auto &item : values) {
    const auto &name = require_string(item, group_path);
    require_program_local_identity(name, group_path);
    if (name == "all" || name == "inherit") {
      fail(group_path + ": reserved selector cannot be a named membership");
    }
    groups.push_back(name);
  }
  std::sort(groups.begin(), groups.end());
  if (std::adjacent_find(groups.begin(), groups.end()) != groups.end()) {
    fail(group_path + ": duplicate named membership");
  }
  return groups;
}

[[nodiscard]] core::ProgramExecutionPolicy execution_policy(
    std::string_view value, std::string_view path) {
  if (value == "independent")
    return core::ProgramExecutionPolicy::kIndependent;
  if (value == "common_start")
    return core::ProgramExecutionPolicy::kCommonStart;
  if (value == "common_start_and_finish")
    return core::ProgramExecutionPolicy::kCommonStartAndFinish;
  if (value == "sync_offset_traj")
    return core::ProgramExecutionPolicy::kSyncOffsetTrajectory;
  fail(std::string(path) + ": unsupported execution policy");
}

[[nodiscard]] std::map<std::string, core::ProgramExecutionPolicy>
execution_group_policies(const CanonicalValue::Object &root,
                         std::string_view path) {
  const auto *value = find_member(root, "execution_groups");
  if (value == nullptr)
    return {};
  const auto group_path = child_path(path, "execution_groups");
  const auto &definitions = require_object(*value, group_path);
  if (definitions.size() > core::kMaxProgramExecutionGroups)
    fail(group_path + ": exceeds the execution-group bound");
  std::map<std::string, core::ProgramExecutionPolicy> result;
  std::string previous;
  for (const auto &[name, definition] : definitions) {
    require_program_local_identity(name, group_path);
    if (name == "all" || name == "inherit")
      fail(group_path + ": reserved selector cannot name a group");
    if (!previous.empty() && previous >= name)
      fail(group_path + ": keys must be duplicate-free and canonical-sorted");
    const auto definition_path = child_path(group_path, name);
    const auto &fields = require_object(definition, definition_path);
    require_fields(fields, {"execution_policy"}, {}, definition_path);
    result.emplace(
        name,
        execution_policy(require_string(
                             require_member(fields, "execution_policy",
                                            definition_path),
                             child_path(definition_path, "execution_policy")),
                         child_path(definition_path, "execution_policy")));
    previous = name;
  }
  return result;
}

[[nodiscard]] CanonicalValue normalize_program_tree(const CanonicalValue &value,
                                                    std::string_view path,
                                                    std::size_t depth,
                                                    std::size_t &count) {
  if (depth > kMaximumProgramTreeDepth || ++count > kMaximumProgramTreeNodes) {
    fail(std::string(path) + ": Program tree exceeds fixed traversal bounds");
  }
  const auto &node = require_object(value, path);
  require_fields(node, {"node_id", "instance_id", "parameters", "children"},
                 {"metadata", "value_sources", "recovery_policy",
                  "operator_stop_policy", "execution_groups"},
                 path);
  const auto &instance =
      require_string(require_member(node, "instance_id", path),
                     child_path(path, "instance_id"));
  require_program_local_identity(instance, child_path(path, "instance_id"));
  const auto policy = parse_node_policy(node, path);
  const auto groups = execution_groups(node, path);
  CanonicalValue::Object normalized;
  normalized.reserve(node.size());
  for (const auto &[name, child] : node) {
    if (name == "recovery_policy" || name == "operator_stop_policy" ||
        name == "execution_groups") {
      continue;
    }
    if (name == "children") {
      const auto &children = require_array(child, child_path(path, name));
      if (children.size() > kMaximumProgramChildren) {
        fail(std::string(path) + ": child count exceeds fixed bound");
      }
      CanonicalValue::Array normalized_children;
      normalized_children.reserve(children.size());
      for (std::size_t index = 0U; index < children.size(); ++index) {
        normalized_children.push_back(normalize_program_tree(
            children[index], index_path(child_path(path, "children"), index),
            depth + 1U, count));
      }
      normalized.emplace_back(name,
                              CanonicalValue(std::move(normalized_children)));
    } else {
      normalized.emplace_back(name, child);
    }
  }
  if (!policy_is_default(policy)) {
    const auto projected = node_policy_projection(policy);
    for (const auto &[name, child] : projected.as_object()) {
      normalized.emplace_back(name, child);
    }
  }
  if (groups.has_value()) {
    if (groups->empty()) {
      normalized.emplace_back("execution_groups", CanonicalValue("all"));
    } else {
      CanonicalValue::Array names;
      for (const auto &name : *groups)
        names.emplace_back(name);
      normalized.emplace_back("execution_groups",
                              CanonicalValue(std::move(names)));
    }
  }
  return CanonicalValue(std::move(normalized));
}

void verify_tree(const CanonicalValue &value, std::string_view path,
                 std::size_t depth, std::size_t &count,
                 std::set<std::string> &instance_ids,
                 std::vector<core::ProgramNodePolicyDTO> &node_policies) {
  if (depth > kMaximumProgramTreeDepth || ++count > kMaximumProgramTreeNodes) {
    fail(std::string(path) + ": Program tree exceeds fixed traversal bounds");
  }
  const auto &node = require_object(value, path);
  require_fields(node, {"node_id", "instance_id", "parameters", "children"},
                 {"metadata", "value_sources", "recovery_policy",
                  "operator_stop_policy", "execution_groups"},
                 path);
  const std::string &node_id = require_string(
      require_member(node, "node_id", path), child_path(path, "node_id"));
  const std::string &instance_id =
      require_string(require_member(node, "instance_id", path),
                     child_path(path, "instance_id"));
  require_program_local_identity(node_id, child_path(path, "node_id"));
  require_program_local_identity(instance_id, child_path(path, "instance_id"));
  if (!instance_ids.insert(instance_id).second) {
    fail(std::string(path) + ": duplicate tree instance_id");
  }
  const auto policy = parse_node_policy(node, path);
  if (!policy_is_default(policy)) {
    node_policies.push_back(policy);
  }
  const auto &parameters = require_object(
      require_member(node, "parameters", path), child_path(path, "parameters"));
  if (parameters.size() > core::kMaxProgramRoleRequirements) {
    fail(std::string(path) + ".parameters: exceeds fixed bound");
  }
  if (const auto *value_sources = find_member(node, "value_sources");
      value_sources != nullptr) {
    const auto &sources =
        require_object(*value_sources, child_path(path, "value_sources"));
    if (sources.empty() || sources.size() > core::kMaxProgramRoleRequirements) {
      fail(std::string(path) +
           ".value_sources: violates fixed non-empty bound");
    }
    // Keys are value paths; a literal may stay beside its source (it is
    // restored when the source is removed). Entries never overlap.
    std::set<ValuePath> paths;
    for (const auto &[key, source] : sources) {
      try {
        paths.insert(parse_value_path(key));
      } catch (const ValuePathError &error) {
        throw ProgramContractError(
            child_path(path, "value_sources") + " key '" + key +
                "': " + error.code() + ": " + error.what(),
            error.code());
      }
      verify_value_source(source,
                          child_path(child_path(path, "value_sources"), key));
    }
    for (const auto &value_path : paths) {
      for (std::size_t length = 1U; length < value_path.size(); ++length) {
        const ValuePath outer(value_path.begin(), value_path.begin() + length);
        if (paths.contains(outer)) {
          fail(child_path(child_path(path, "value_sources"),
                          format_value_path(value_path)) +
               ": overlaps the value_sources entry '" +
               format_value_path(outer) +
               "'; a value bound as a whole cannot also have entries for its "
               "own fields");
        }
      }
    }
  }
  if (const auto *metadata = find_member(node, "metadata");
      metadata != nullptr) {
    (void)require_object(*metadata, child_path(path, "metadata"));
  }
  const auto &children = require_array(require_member(node, "children", path),
                                       child_path(path, "children"));
  if (children.size() > kMaximumProgramChildren) {
    fail(std::string(path) + ": child count exceeds fixed bound");
  }
  for (std::size_t index = 0U; index < children.size(); ++index) {
    verify_tree(children[index],
                index_path(child_path(path, "children"), index), depth + 1U,
                count, instance_ids, node_policies);
  }
}

void verify_optional_top_level_shapes(const CanonicalValue::Object &root) {
  if (const auto *compatibility = find_member(root, "compatibility");
      compatibility != nullptr) {
    const auto &object =
        require_object(*compatibility, "program.compatibility");
    require_fields(object, {}, {"lanes", "assistant", "requires", "tags"},
                   "program.compatibility");
    if (const auto *lanes = find_member(object, "lanes"); lanes != nullptr) {
      const auto values = parse_text_set(lanes, "program.compatibility.lanes");
      for (const auto &lane : values) {
        if (lane != "manyforge-only" && lane != "isaac-sim" &&
            lane != "real-robot") {
          fail("program.compatibility.lanes: unknown lane");
        }
      }
    }
    if (const auto *assistant = find_member(object, "assistant");
        assistant != nullptr) {
      const std::string &spelling =
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
  if (const auto *viewer = find_member(root, "viewer"); viewer != nullptr) {
    (void)require_object(*viewer, "program.viewer");
  }
  for (const std::string_view field :
       {"scene", "parameters", "contact_policies"}) {
    if (const auto *value = find_member(root, field); value != nullptr) {
      (void)require_array(*value, child_path("program", field));
    }
  }
  if (const auto *blackboard = find_member(root, "blackboard");
      blackboard != nullptr) {
    (void)require_object(*blackboard, "program.blackboard");
  }
}

[[nodiscard]] bool
declared_scene_resource_runtime_object(const CanonicalValue::Object &root,
                                       std::string_view runtime_object_id) {
  const auto *blackboard_value = find_member(root, "blackboard");
  if (blackboard_value == nullptr) {
    return false;
  }
  const auto &blackboard =
      require_object(*blackboard_value, "program.blackboard");
  const auto *keys_value = find_member(blackboard, "keys");
  if (keys_value == nullptr) {
    return false;
  }
  const auto &keys = require_array(*keys_value, "program.blackboard.keys");
  return std::count_if(keys.begin(), keys.end(), [&](const auto &value) {
           const auto &key = require_object(value, "program.blackboard.keys[]");
           const auto *name = find_member(key, "key");
           const auto *type = find_member(key, "type");
           return name != nullptr && type != nullptr &&
                  require_string(*name, "program.blackboard.keys[].key") ==
                      runtime_object_id &&
                  require_string(*type, "program.blackboard.keys[].type") ==
                      "scene_resource";
         }) == 1;
}

struct ParsedContactPair final {
  std::optional<core::ProgramContactGraspBodySelectorDTO> body_selector;
  core::RuntimeObjectId runtime_object_id;
  bool bodies_from_motion_endpoint{};
};

[[nodiscard]] ParsedContactPair
parse_contact_grasp_pair(const CanonicalValue &value, std::string_view path) {
  const auto &pair = require_object(value, path);
  require_fields(pair, {"body_a", "body_b"}, {}, path);

  std::optional<core::ProgramContactGraspBodySelectorDTO> body_selector;
  std::optional<core::RuntimeObjectId> runtime_object_id;
  bool motion_endpoint = false;
  const auto parse_side = [&](const CanonicalValue &side_value,
                              std::string side_path) {
    const auto &side = require_object(side_value, side_path);
    const std::string &kind = require_string(
        require_member(side, "kind", side_path), child_path(side_path, "kind"));
    if (kind == "component_model_body") {
      require_fields(side, {"kind", "body"}, {}, side_path);
      if (body_selector.has_value() || motion_endpoint) {
        fail(std::string(path) +
             ": contact_grasp pair has two component-model sides");
      }
      const auto &body = require_object(require_member(side, "body", side_path),
                                        child_path(side_path, "body"));
      require_exact_fields(body, {"component_id", "collision_body_id"}, side_path);
      body_selector = core::ProgramContactGraspBodySelectorDTO{{
          parse_direct_reference_identity<core::ControllableComponentId>(
              require_member(body, "component_id", side_path), side_path),
          parse_direct_reference_identity<core::CollisionBodyRoleId>(
              require_member(body, "collision_body_id", side_path), side_path)}};
      return;
    }
    if (kind == "motion_endpoint") {
      require_fields(side, {"kind"}, {}, side_path);
      if (body_selector.has_value() || motion_endpoint) {
        fail(std::string(path) + ": contact_grasp pair has two robot sides");
      }
      motion_endpoint = true;
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
    fail(side_path +
         ": contact_grasp requires component_model_body, motion_endpoint or "
         "runtime_object");
  };

  parse_side(require_member(pair, "body_a", path), child_path(path, "body_a"));
  parse_side(require_member(pair, "body_b", path), child_path(path, "body_b"));
  if ((!body_selector.has_value() && !motion_endpoint) ||
      !runtime_object_id.has_value()) {
    fail(std::string(path) +
         ": contact_grasp requires exactly one component body and one "
         "runtime object");
  }
  return {std::move(body_selector), std::move(*runtime_object_id),
          motion_endpoint};
}

[[nodiscard]] std::vector<core::ProgramContactGraspTemplateDTO>
parse_contact_grasp_templates(const CanonicalValue::Object &root) {
  const auto *policies_value = find_member(root, "contact_policies");
  if (policies_value == nullptr) {
    return {};
  }
  const auto &policies =
      require_array(*policies_value, "program.contact_policies");
  if (policies.size() > core::kMaxProgramContractRoles) {
    fail("program.contact_policies: exceeds fixed bound");
  }
  std::vector<core::ProgramContactGraspTemplateDTO> result;
  result.reserve(policies.size());
  for (std::size_t index = 0U; index < policies.size(); ++index) {
    const std::string path = index_path("program.contact_policies", index);
    const auto &policy = require_object(policies[index], path);
    const auto *mode_value = find_member(policy, "mode");
    if (mode_value == nullptr ||
        (require_string(*mode_value, child_path(path, "mode")) != "contact_grasp" &&
         require_string(*mode_value, child_path(path, "mode")) != "contact_release")) {
      // This adds one binding projection for contact-grasp. Other policy modes
      // retain their existing Program validation and runtime ownership.
      continue;
    }
    const bool release = require_string(*mode_value, child_path(path, "mode")) == "contact_release";
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
      fail(path + ": only geometry-only contact_grasp policies are accepted");
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
    const auto policy_revision = core::ContactPolicyRevision(require_string(
        require_member(policy, "version", path), child_path(path, "version")));
    if (!is_sha256_digest(policy_revision.value()) ||
        !detail::verify_content_identity(
            policies[index],
            detail::ContentIdentitySpec{
                .identity_domain = "catalog-record-revision/contact_policy",
                .api_version = "manyforge.contact_policy.v1",
                .own_field = "version"})) {
      fail(path + ": ContactPolicyRevision does not match the policy value");
    }

    const auto &zones = require_array(require_member(policy, "zones", path),
                                      child_path(path, "zones"));
    if (zones.size() != 1U) {
      fail(path + ": contact_grasp currently requires exactly one zone");
    }
    const std::string zone_path = index_path(child_path(path, "zones"), 0U);
    const auto &zone = require_object(zones.front(), zone_path);
    if (release) {
      require_fields(zone, {"zone_id", "body_pairs", "violation_response", "release_evidence_mode"},
          {"notes", "enforce_max_start_penetration", "max_start_penetration_m",
           "min_release_progress_m", "release_tolerance_m",
           "require_strictly_decreasing_penetration", "require_separation_by_end", "allow_recontact"}, zone_path);
    } else {
      require_fields(zone, {"zone_id", "body_pairs", "violation_response",
                           "max_penetration_m", "terminal_window_start_fraction"}, {"notes"}, zone_path);
    }
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
    const auto envelope = [&]() -> core::ContactZoneEnvelopeDTO {
      if (release) return detail::parse_program_contact_envelope(zone, true, false);
      const double max_penetration =
          require_number(require_member(zone, "max_penetration_m", zone_path),
                         child_path(zone_path, "max_penetration_m"));
      const double terminal_start = require_number(
          require_member(zone, "terminal_window_start_fraction", zone_path),
          child_path(zone_path, "terminal_window_start_fraction"));
      if (max_penetration <= 0.0 || terminal_start < 0.0 || terminal_start > 1.0) {
        fail(zone_path + ": contact_grasp bounds are outside their valid range");
      }
      return core::ContactGraspEnvelopeDTO{max_penetration, terminal_start};
    }();

    const auto &pairs =
        require_array(require_member(zone, "body_pairs", zone_path),
                      child_path(zone_path, "body_pairs"));
    if (pairs.empty() || pairs.size() > core::kMaxProgramRoleRequirements) {
      fail(zone_path + ": contact_grasp body-pair count violates bounds");
    }
    std::vector<core::ProgramContactGraspBodySelectorDTO> selectors;
    selectors.reserve(pairs.size());
    std::optional<core::RuntimeObjectId> runtime_object_id;
    bool bodies_from_motion_endpoint = false;
    for (std::size_t pair_index = 0U; pair_index < pairs.size(); ++pair_index) {
      auto parsed = parse_contact_grasp_pair(
          pairs[pair_index],
          index_path(child_path(zone_path, "body_pairs"), pair_index));
      if (runtime_object_id.has_value() &&
          *runtime_object_id != parsed.runtime_object_id) {
        fail(zone_path +
             ": every contact_grasp pair must bind the same runtime object");
      }
      runtime_object_id = parsed.runtime_object_id;
      if (parsed.bodies_from_motion_endpoint) {
        if (pairs.size() != 1U) {
          fail(zone_path +
               ": motion_endpoint cannot mix with other body selectors");
        }
        bodies_from_motion_endpoint = true;
      } else {
        selectors.push_back(std::move(*parsed.body_selector));
      }
    }
    const auto selector_key = [](const auto &selector) {
      return std::tuple(selector.body.component_id.value(),
                        selector.body.collision_body_id.value());
    };
    if (!std::is_sorted(selectors.begin(), selectors.end(),
                        [&](const auto &left, const auto &right) {
                          return selector_key(left) < selector_key(right);
                        }) ||
        std::adjacent_find(selectors.begin(), selectors.end(),
                           [&](const auto &left, const auto &right) {
                             return selector_key(left) == selector_key(right);
                           }) != selectors.end()) {
      fail(zone_path +
           ": contact_grasp body pairs must be unique and canonical-sorted");
    }
    if (!runtime_object_id.has_value() ||
        !declared_scene_resource_runtime_object(root,
                                                runtime_object_id->value())) {
      fail(path + ": contact_grasp runtime object is not one declared "
                  "scene_resource blackboard key");
    }
    result.push_back(core::ProgramContactGraspTemplateDTO{
        policy_id, policy_revision, *runtime_object_id, std::move(selectors),
        envelope, bodies_from_motion_endpoint});
  }
  if (!std::is_sorted(result.begin(), result.end(),
                      [](const auto &left, const auto &right) {
                        return left.contact_policy_id < right.contact_policy_id;
                      }) ||
      std::adjacent_find(result.begin(), result.end(),
                         [](const auto &left, const auto &right) {
                           return left.contact_policy_id ==
                                  right.contact_policy_id;
                         }) != result.end()) {
    fail("program.contact_policies: must be unique and canonical-sorted");
  }
  return result;
}

struct QualifiedExecutionCatalog final {
  std::string catalog_id;
  std::string content_version;
  std::string digest;
  CanonicalValue schema_types;
  std::map<std::string, CanonicalValue, std::less<>> entries;
};

// The existing build gate validates the complete catalog. Read its embedded
// bytes once, never files/environment projections or an editable reverse map
// during Run/Stop. The retained catalog identity still has to match exactly.
[[nodiscard]] const QualifiedExecutionCatalog &execution_catalog() {
  static const auto catalog = [] {
    const auto root =
        parse_strict_json_model_yaml(detail::build_evidence::kNodeCatalog)
            .root();
    const auto &object = require_object(root, "execution catalog");
    QualifiedExecutionCatalog result{
        require_string(require_member(object, "catalog_id", "catalog"),
                       "catalog_id"),
        require_string(require_member(object, "content_version", "catalog"),
                       "content_version"),
        require_string(require_member(object, "catalog_digest", "catalog"),
                       "catalog_digest"),
        require_member(object, "schema_types", "catalog"),
        {}};
    // The identity is the digest computed from the content; a stale stored
    // digest is not a refusal (the Python loader reports it).
    auto payload = object;
    std::erase_if(payload, [](const auto &item) {
      return item.first == "catalog_digest";
    });
    result.digest = sha256_digest(canonical_json(CanonicalValue(std::move(payload))));
    for (const auto &entry : require_array(
             require_member(object, "entries", "catalog"), "catalog.entries")) {
      const auto &fields = require_object(entry, "catalog.entry");
      const auto &id = require_string(
          require_member(fields, "id", "catalog.entry"), "node_id");
      if (!result.entries.emplace(id, entry).second) {
        fail("embedded execution catalog contains duplicate nodes");
      }
    }
    return result;
  }();
  return catalog;
}

[[nodiscard]] const CanonicalValue *literal_node_parameter(
    const CanonicalValue::Object &node, const CanonicalValue::Object &entry,
    const std::string &id, std::string_view path, bool condition = false) {
  const auto &parameters =
      require_object(require_member(node, "parameters", path), path);
  const auto *sources_value = find_member(node, "value_sources");
  const auto *sources = sources_value == nullptr
                            ? nullptr
                            : &require_object(*sources_value, path);
  const auto &definitions =
      require_array(require_member(entry, "parameters", path), path);
  // Scope attribution must not depend on a later blackboard value or edit.
  if (sources != nullptr && find_member(*sources, id) != nullptr) {
    fail(std::string(path) +
         ": effect scope/condition requires a literal parameter: " + id);
  }
  for (const auto &definition : definitions) {
    const auto &fields = require_object(definition, "catalog.parameter");
    if (require_string(require_member(fields, "id", path), path) == id) {
      const auto *value = find_member(parameters, id);
      if (value == nullptr)
        value = find_member(fields, "default");
      if (!condition)
        return value;
      const auto *schema = find_member(fields, "schema");
      if (schema == nullptr) {
        const auto &ref =
            require_string(require_member(fields, "schema_ref", path), path);
        constexpr std::string_view prefix = "#/schema_types/";
        if (!ref.starts_with(prefix))
          fail(std::string(path) + ": invalid condition schema reference");
        schema = execution_catalog().schema_types.find_object_member(
            ref.substr(prefix.size()));
      }
      if (schema == nullptr)
        fail(std::string(path) + ": absent condition schema");
      const auto &shape = require_object(*schema, path);
      // Qualified catalog conditions are enum selections. New condition
      // types need explicit qualification, never truthy/coercing fallback.
      if (require_string(require_member(shape, "type", path), path) != "enum") {
        fail(std::string(path) + ": condition schema is not a qualified enum");
      }
      const auto &values =
          require_array(require_member(shape, "values", path), path);
      if (value == nullptr || !value->is_string() ||
          std::find(values.begin(), values.end(), *value) == values.end()) {
        fail(std::string(path) + ": invalid catalog condition value: " + id);
      }
      return value;
    }
  }
  fail(std::string(path) +
       ": catalog effect selector names no parameter: " + id);
}

[[nodiscard]] std::vector<core::ProgramNodeTargetEffectDTO> node_target_effects(
    const CanonicalValue::Object &node, const CanonicalValue::Object &entry,
    const std::vector<RetainedIoSignalConfiguration> &signals,
    std::string_view path) {
  const auto effective = [&](const std::string &id,
                             bool condition = false) -> const CanonicalValue * {
    return literal_node_parameter(node, entry, id, path, condition);
  };
  std::vector<core::ProgramNodeTargetEffectDTO> effects;
  for (const auto &requirement : require_array(
           require_member(entry, "required_capabilities", path), path)) {
    const auto &fields =
        require_object(requirement, "catalog.required_capability");
    if (require_string(require_member(fields, "scope", path), path) !=
        "target_binding") {
      continue;
    }
    if (const auto *condition = find_member(fields, "when")) {
      const auto &when = require_object(*condition, "catalog.when");
      const auto *value = effective(
          require_string(require_member(when, "parameter", path), path), true);
      if (value == nullptr || *value != require_member(when, "equals", path))
        continue;
    }
    const auto *selection = effective(
        require_string(require_member(fields, "target_parameter", path), path));
    if (selection == nullptr)
      fail(std::string(path) + ": missing literal target reference for node effect");
    const auto &selected = require_object(*selection, path);
    const auto component = parse_direct_reference_identity<core::ControllableComponentId>(
        require_member(selected, "component_id", path), path);
    std::optional<core::CommandTargetId> target_id;
    if (const auto *target = find_member(selected, "command_target_id")) {
      require_exact_fields(selected, {"component_id", "command_target_id"}, path);
      target_id = parse_direct_reference_identity<core::CommandTargetId>(*target, path);
    } else {
      require_exact_fields(selected, {"component_id", "signal_name"}, path);
      const auto &name = require_string(require_member(selected, "signal_name", path), path);
      const auto matches = FindProgramSignalMatches(signals, {component, name});
      if (matches.size() != 1U)
        fail(std::string(path) + ": signal reference is missing or ambiguous");
      target_id = matches.front().retained->command_target_id;
    }
    const core::CapabilityId capability(
        require_string(require_member(fields, "capability_id", path), path));
    const auto *effectful_value = find_member(fields, "effectful");
    if (effectful_value != nullptr && !effectful_value->is_bool())
      fail(child_path(path, "effectful") + ": expected boolean");
    const bool effectful =
        effectful_value != nullptr && effectful_value->as_bool();
    effects.push_back({{component, *target_id}, capability, effectful});
  }
  const auto less = [](const auto &a, const auto &b) {
    return std::tie(a.target, a.capability_id) <
           std::tie(b.target, b.capability_id);
  };
  std::sort(effects.begin(), effects.end(), less);
  effects.erase(std::unique(effects.begin(), effects.end()), effects.end());
  return effects;
}

[[nodiscard]] const CanonicalValue::Object *
find_declaration(const CanonicalValue *rows, std::string_view id_field,
                 std::string_view id, std::string_view path) {
  if (rows == nullptr) {
    return nullptr;
  }
  for (const auto &row : require_array(*rows, path)) {
    const auto &fields = require_object(row, path);
    const auto *name = find_member(fields, id_field);
    if (name != nullptr && require_string(*name, path) == id) {
      return &fields;
    }
  }
  return nullptr;
}

// ------------------------------------------------------------ pose values
// A stored_pose literal (a key's initial value or a key-typed node parameter)
// and a key's allowed_frames are checked as the Python Program validator
// checks them (program.py _validate_declared_type_value and
// _validate_allowed_frames), with its wording
// (program_pose_value_conformance.v1.yaml runs both).

struct PoseFrames final {
  std::set<std::string> scene_resource_ids;
};

[[nodiscard]] std::set<std::string>
declared_scene_resource_ids(const CanonicalValue::Object &root) {
  std::set<std::string> ids;
  if (const auto *scene = find_member(root, "scene")) {
    for (const auto &row : require_array(*scene, "program.scene")) {
      const auto &fields = require_object(row, "program.scene[]");
      ids.insert(require_string(
          require_member(fields, "scene_resource_id", "program.scene[]"),
          "program.scene[].scene_resource_id"));
    }
  }
  return ids;
}

void require_closed_pose_value(const CanonicalValue::Object &object,
                               const std::set<std::string_view> &required,
                               const std::string &path) {
  const auto python_list = [](const std::set<std::string_view> &names) {
    std::string text = "[";
    for (const auto name : names) {
      text += (text.size() > 1U ? ", '" : "'") + std::string(name) + "'";
    }
    return text + "]";
  };
  std::set<std::string_view> missing = required;
  std::set<std::string_view> unknown;
  for (const auto &[name, unused] : object) {
    (void)unused;
    if (missing.erase(name) == 0U && !required.contains(name)) {
      unknown.insert(name);
    }
  }
  if (!missing.empty()) {
    fail(path + ": is missing required fields " + python_list(missing));
  }
  if (!unknown.empty()) {
    fail(path + ": contains unknown fields " + python_list(unknown));
  }
}

[[nodiscard]] const CanonicalValue::Object &
require_pose_mapping(const CanonicalValue &value, const std::string &path) {
  if (!value.is_object()) {
    fail(path + ": must be a mapping");
  }
  return value.as_object();
}

[[nodiscard]] std::vector<double> pose_numbers(const CanonicalValue &value,
                                               std::size_t count,
                                               std::string_view count_text,
                                               const std::string &path) {
  if (!value.is_array()) {
    fail(path + ": must be a list");
  }
  if (value.as_array().size() != count) {
    fail(path + ": must contain " + std::string(count_text) + " numbers");
  }
  std::vector<double> numbers;
  for (std::size_t index = 0U; index < count; ++index) {
    const auto &item = value.as_array()[index];
    if (!item.is_number() || !std::isfinite(item.as_number())) {
      fail(index_path(path, index) + ": must be a finite number");
    }
    numbers.push_back(item.as_number());
  }
  return numbers;
}

// Returns the canonical order of the frame: kind, then component or scene
// resource, then component-local frame identity.
std::tuple<std::string, std::string, std::string>
verify_pose_frame_ref(const CanonicalValue &value, const PoseFrames &frames,
                      const std::string &path) {
  const auto &frame = require_pose_mapping(value, path);
  const auto *kind_value = find_member(frame, "kind");
  const std::string kind = kind_value != nullptr && kind_value->is_string()
                               ? kind_value->as_string()
                               : std::string{};
  if (kind == "cell_frame") {
    require_closed_pose_value(frame, {"kind"}, path);
    return {kind, {}, {}};
  }
  if (kind == "scene_resource") {
    require_closed_pose_value(frame, {"kind", "scene_resource_id"}, path);
    const auto &resource = require_string(
        require_member(frame, "scene_resource_id", path),
        child_path(path, "scene_resource_id"));
    if (!frames.scene_resource_ids.contains(resource)) {
      fail(child_path(path, "scene_resource_id") +
           ": does not name a declared scene resource");
    }
    return {kind, resource, {}};
  }
  if (kind != "target_frame") {
    fail(child_path(path, "kind") +
         ": must be cell_frame, target_frame, or scene_resource");
  }
  require_closed_pose_value(frame, {"kind", "frame"}, path);
  const auto frame_path = child_path(path, "frame");
  const auto &ref = require_pose_mapping(require_member(frame, "frame", path),
                                          frame_path);
  require_closed_pose_value(ref, {"component_id", "frame_id"}, child_path(path, "frame"));
  const auto component = parse_direct_reference_identity<core::ControllableComponentId>(
      require_member(ref, "component_id", path), child_path(path, "frame.component_id"));
  const auto id = parse_direct_reference_identity<core::FrameRoleId>(
      require_member(ref, "frame_id", path), child_path(path, "frame.frame_id"));
  return {kind, component.value(), id.value()};
}

void verify_stored_pose(const CanonicalValue &value, const PoseFrames &frames,
                        const std::string &path) {
  const auto &pose = require_pose_mapping(value, path);
  require_closed_pose_value(pose, {"frame_ref", "position_m", "orientation"},
                            path);
  (void)pose_numbers(require_member(pose, "position_m", path), 3U, "three",
                     child_path(path, "position_m"));
  const auto orientation_path = child_path(path, "orientation");
  const auto &orientation = require_pose_mapping(
      require_member(pose, "orientation", path), orientation_path);
  if (orientation.size() != 1U || (orientation.front().first != "xyzw" &&
                                   orientation.front().first != "rpy_deg")) {
    fail(orientation_path + ": must contain exactly one of xyzw or rpy_deg");
  }
  const auto &[name, components] = orientation.front();
  const bool quaternion = name == "xyzw";
  const auto numbers =
      pose_numbers(components, quaternion ? 4U : 3U, quaternion ? "4" : "3",
                   child_path(orientation_path, name));
  if (quaternion) {
    double squared_norm = 0.0;
    for (const double item : numbers) {
      squared_norm += item * item;
    }
    if (std::fabs(squared_norm - 1.0) > 1e-6) {
      fail(child_path(orientation_path, "xyzw") +
           ": must already be a unit quaternion");
    }
  }
  (void)verify_pose_frame_ref(require_member(pose, "frame_ref", path), frames,
                              child_path(path, "frame_ref"));
}

// A stored_pose key may narrow its frames with `allowed_frames`; its initial
// value is a complete pose in one of them.
void verify_blackboard_pose_keys(const CanonicalValue::Object &root,
                                 const PoseFrames &frames) {
  const auto *blackboard = find_member(root, "blackboard");
  const auto *keys =
      blackboard == nullptr
          ? nullptr
          : find_member(require_object(*blackboard, "program.blackboard"),
                        "keys");
  if (keys == nullptr) {
    return;
  }
  const auto &rows = require_array(*keys, "program.blackboard.keys");
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const auto path = index_path("program.blackboard.keys", index);
    const auto &fields = require_object(rows[index], path);
    const bool pose =
        require_string(require_member(fields, "type", path),
                       child_path(path, "type")) == "stored_pose";
    const auto *allowed = find_member(fields, "allowed_frames");
    if (allowed != nullptr) {
      const auto allowed_path = child_path(path, "allowed_frames");
      if (!pose) {
        fail(allowed_path + ": is valid only for a stored_pose key");
      }
      if (!allowed->is_array()) {
        fail(allowed_path + ": must be a list");
      }
      const auto &allowed_rows = allowed->as_array();
      if (allowed_rows.empty()) {
        fail(allowed_path +
             ": must not be empty; omit it to allow every declared frame");
      }
      std::vector<std::tuple<std::string, std::string, std::string>> order;
      for (std::size_t item = 0U; item < allowed_rows.size(); ++item) {
        order.push_back(verify_pose_frame_ref(allowed_rows[item], frames,
                                              index_path(allowed_path, item)));
      }
      if (std::adjacent_find(order.begin(), order.end(),
                             std::greater_equal<>()) != order.end()) {
        fail(allowed_path +
             ": must be duplicate-free and canonical-sorted by kind, target "
             "role or scene resource, and frame role");
      }
    }
    const auto *initial = find_member(fields, "initial");
    if (!pose || initial == nullptr) {
      continue;
    }
    const auto initial_path = child_path(path, "initial");
    verify_stored_pose(*initial, frames, initial_path);
    if (allowed != nullptr &&
        std::none_of(allowed->as_array().begin(), allowed->as_array().end(),
                     [&](const auto &frame) {
                       return frame == require_member(initial->as_object(),
                                                      "frame_ref",
                                                      initial_path);
                     })) {
      fail(child_path(initial_path, "frame_ref") +
           ": is not one of the key's allowed_frames");
    }
  }
}

[[nodiscard]] bool literal_has_key_type(const CanonicalValue &value,
                                        std::string_view type) {
  if (type == "bool") {
    return value.is_bool();
  }
  if (type == "string") {
    return value.is_string();
  }
  if (type == "float64") {
    return value.is_number();
  }
  if (type == "int64") {
    return value.is_number() &&
           std::trunc(value.as_number()) == value.as_number() &&
           std::fabs(value.as_number()) <= 9007199254740991.0;
  }
  if (type == "stored_pose") {
    return value.is_object(); // its members are checked by verify_stored_pose
  }
  return false;
}

[[nodiscard]] bool reserved_system_namespace(std::string_view name) {
  return name == "node_outcome" || name.starts_with("node_outcome.") ||
         name == "interrupt_state" || name.starts_with("interrupt_state.") ||
         name == "active_tcp" || name.starts_with("active_tcp.") ||
         name.starts_with("active_tcp/");
}

// System declarations are a validation view only: the authored root and its
// ProgramRevision remain unchanged. Runtime owners retain their value lifetime.
[[nodiscard]] CanonicalValue
with_node_outcome_declarations(const CanonicalValue::Object &root,
                               const RetainedBundle &retained_bundle) {
  CanonicalValue::Object blackboard;
  if (const auto *value = find_member(root, "blackboard")) {
    blackboard = require_object(*value, "program.blackboard");
  }
  if (const auto *namespaces = find_member(blackboard, "namespaces")) {
    for (const auto &value :
         require_array(*namespaces, "program.blackboard.namespaces")) {
      const auto &row =
          require_object(value, "program.blackboard.namespaces[]");
      const auto &name = require_string(
          require_member(row, "namespace_id", "namespace"), "namespace_id");
      if (reserved_system_namespace(name))
        fail("reserved_system_namespace: program.blackboard.namespaces: " +
             name + " is reserved for system-owned values");
    }
  }
  CanonicalValue::Array keys;
  if (const auto *value = find_member(blackboard, "keys")) {
    keys = require_array(*value, "program.blackboard.keys");
  }
  for (const auto &value : keys) {
    const auto &row = require_object(value, "program.blackboard.keys[]");
    const auto &name = require_string(require_member(row, "key", "key"), "key");
    if (reserved_system_namespace(name))
      fail("reserved_system_namespace: program.blackboard.keys: " + name +
           " is system-owned and cannot be authored");
  }
  std::set<std::string> active_tcp_keys;
  // verify_tree has already bounded and validated this tree and its IDs.
  const auto visit = [&](auto &&self, const CanonicalValue &value) -> void {
    const auto &node = require_object(value, "program.tree");
    const auto &id =
        require_string(require_member(node, "node_id", "tree"), "node_id");
    if (id == "motion.move_to_joint_target" || id == "motion.move_to_pose" ||
        id == "motion.follow_cartesian_path" || id == "motion.move_until_signal" ||
        id == "motion.probe_search") {
      const auto &instance = require_string(
          require_member(node, "instance_id", "tree"), "instance_id");
      keys.emplace_back(CanonicalValue::Object{
          {"key", CanonicalValue("node_outcome." + instance)},
          {"type", CanonicalValue("string")},
          {"access", CanonicalValue("read")},
          {"persistence", CanonicalValue("run")}});
    }
    if (id == "motion.move_to_joint_target" || id == "motion.move_to_pose" ||
        id == "motion.follow_cartesian_path" || id == "motion.move_until_signal" ||
        id == "motion.probe_search" || id == "planning.plan_pick_sequence" ||
        id == "planning.plan_place_sequence" || id == "motion.set_tcp") {
      const auto &parameters = require_object(require_member(node, "parameters", "tree"), "parameters");
      if (const auto *target = find_member(parameters, "target")) {
        const auto &reference = require_object(*target, "target");
        require_exact_fields(reference, {"component_id", "command_target_id"}, "target");
        const core::ProgramTargetReferenceDTO selected{
            parse_direct_reference_identity<core::ControllableComponentId>(
                require_member(reference, "component_id", "target"), "target.component_id"),
            parse_direct_reference_identity<core::CommandTargetId>(
                require_member(reference, "command_target_id", "target"), "target.command_target_id")};
        const auto key = core::ActiveTcpBlackboardKey(selected);
        if (!key) fail("invalid Active TCP target reference");
        if (active_tcp_keys.insert(*key).second)
          keys.emplace_back(CanonicalValue::Object{
              {"key", CanonicalValue(*key)}, {"type", CanonicalValue("tcp_entry")},
              {"access", CanonicalValue("read")}, {"persistence", CanonicalValue("run")}});
      }
    }
    for (const auto &child :
         require_array(require_member(node, "children", "tree"), "children")) {
      self(self, child);
    }
  };
  visit(visit, require_member(root, "tree", "program"));
  // These IDs have crossed the retained-bundle revision/codec boundary. The
  // final bound Apply check still qualifies Program-dependent keys/groups;
  // this validation view grants no source, installed key or runtime rule.
  if (const auto *configuration =
          retained_bundle.normalized_root().find_object_member(
              "interrupt_policy_configuration"))
    if (const auto *rules =
            configuration->find_object_member("interrupt_rules"))
      for (const auto &rule : rules->as_array())
        keys.emplace_back(CanonicalValue::Object{
            {"key",
             CanonicalValue("interrupt_state." +
                            rule.find_object_member("rule_id")->as_string())},
            {"type", CanonicalValue("string")},
            {"access", CanonicalValue("read")},
            {"persistence", CanonicalValue("run")}});
  std::sort(keys.begin(), keys.end(), [](const auto &left, const auto &right) {
    return require_string(require_member(left.as_object(), "key", "key"),
                          "key") <
           require_string(require_member(right.as_object(), "key", "key"),
                          "key");
  });
  const auto set = [](auto &object, std::string name, CanonicalValue value) {
    const auto row =
        std::find_if(object.begin(), object.end(),
                     [&](const auto &member) { return member.first == name; });
    if (row == object.end())
      object.emplace_back(std::move(name), std::move(value));
    else
      row->second = std::move(value);
  };
  set(blackboard, "keys", CanonicalValue(std::move(keys)));
  auto view = root;
  set(view, "blackboard", CanonicalValue(std::move(blackboard)));
  return CanonicalValue(std::move(view));
}

// A catalog `key_value` parameter is typed by the blackboard key named by its
// `value_type_from_key` parameter. The key must be one literal declared key of
// an admitted type, and a literal must carry exactly that type (value sources
// are typed by the key in verify_node_value_sources). A pose literal is a
// complete stored_pose in a declared frame, as in Python.
void verify_key_typed_parameters(const CanonicalValue::Object &node,
                                 const CanonicalValue::Object &entry,
                                 const CanonicalValue::Object &root,
                                 const PoseFrames &frames,
                                 std::string_view path) {
  const auto &parameters =
      require_object(require_member(node, "parameters", path), path);
  const auto *sources_value = find_member(node, "value_sources");
  const auto *sources = sources_value == nullptr
                            ? nullptr
                            : &require_object(*sources_value, path);
  const auto *blackboard = find_member(root, "blackboard");
  const auto *keys =
      blackboard == nullptr
          ? nullptr
          : find_member(require_object(*blackboard, "program.blackboard"),
                        "keys");
  const auto &definitions =
      require_array(require_member(entry, "parameters", path), path);
  const auto declares = [&](std::string_view id) {
    return std::any_of(definitions.begin(), definitions.end(),
                       [&](const auto &definition) {
                         return require_string(
                                    require_member(require_object(definition,
                                                                  path),
                                                   "id", path),
                                    path) == id;
                       });
  };
  for (const auto &definition : definitions) {
    const auto &fields = require_object(definition, "catalog.parameter");
    const auto *schema = find_member(fields, "schema");
    const auto *from_key =
        schema == nullptr ? nullptr
                          : find_member(require_object(*schema, path),
                                        "value_type_from_key");
    if (from_key == nullptr) {
      continue;
    }
    const auto &shape = require_object(*schema, path);
    const auto &id = require_string(require_member(fields, "id", path), path);
    const auto &key_parameter = require_string(*from_key, path);
    const auto &value_types =
        require_array(require_member(shape, "value_types", path), path);
    if (require_string(require_member(shape, "type", path), path) !=
            "key_value" ||
        value_types.empty() || !declares(key_parameter)) {
      fail(std::string(path) +
           ": inconsistent catalog key-typed declaration: " + id);
    }
    if (sources != nullptr && find_member(*sources, key_parameter) != nullptr) {
      fail(std::string(path) +
           ": key-typed parameter requires a literal key parameter: " +
           key_parameter);
    }
    const auto *key_name = find_member(parameters, key_parameter);
    const auto *key =
        key_name == nullptr
            ? nullptr
            : find_declaration(keys, "key", require_string(*key_name, path),
                               "program.blackboard.keys");
    if (key == nullptr) {
      fail(std::string(path) +
           ": key-typed parameter names no declared blackboard key: " + id);
    }
    const auto &key_type =
        require_string(require_member(*key, "type", path), path);
    if (std::find(value_types.begin(), value_types.end(),
                  CanonicalValue(key_type)) == value_types.end()) {
      fail(std::string(path) + ": blackboard key type '" + key_type +
           "' is not admitted by the node: " + id);
    }
    if (const auto *literal = find_member(parameters, id)) {
      if (!literal_has_key_type(*literal, key_type)) {
        fail(std::string(path) + ": literal does not match key type '" +
             key_type + "': " + id);
      }
      if (key_type == "stored_pose") {
        verify_stored_pose(*literal, frames,
                           child_path(child_path(path, "parameters"), id));
      }
    }
  }
}

// ------------------------------------------------------------ expressions
// Computed keys and field expressions are checked by the one expression
// engine against the Program's parameters and readable keys, exactly as the
// Python Program validator does (EXPRESSION_LANGUAGE_REFERENCE.md).

[[nodiscard]] std::optional<expression::ValueType>
expression_type(std::string_view declared) {
  if (declared == "bool") {
    return expression::ValueType{expression::ValueKind::kBool, {}};
  }
  if (declared == "int64") {
    return expression::ValueType{expression::ValueKind::kInt64, {}};
  }
  if (declared == "float64") {
    return expression::ValueType{expression::ValueKind::kFloat64, {}};
  }
  if (declared == "string" || declared == "scene_resource") {
    return expression::ValueType{expression::ValueKind::kString, {}};
  }
  if (declared == "stored_pose") {
    return expression::ValueType{expression::ValueKind::kPose, {}};
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<expression::ValueKind>
expression_destination(std::string_view declared) {
  if (declared == "bool") {
    return expression::ValueKind::kBool;
  }
  if (declared == "int64") {
    return expression::ValueKind::kInt64;
  }
  if (declared == "float64") {
    return expression::ValueKind::kFloat64;
  }
  if (declared == "string") {
    return expression::ValueKind::kString;
  }
  return std::nullopt;
}

/// Check one expression; returns the keys it reads.
std::vector<std::string>
check_program_expression(std::string_view text,
                         const expression::TypeEnvironment &environment,
                         expression::ValueKind destination,
                         std::string_view path) {
  const auto describe = [&](const expression::Diagnostic &diagnostic) {
    return std::string(path) + ": " + diagnostic.code + " at character " +
           std::to_string(diagnostic.position) + ": " + diagnostic.message;
  };
  auto parsed = expression::parse(text);
  if (const auto *diagnostic = std::get_if<expression::Diagnostic>(&parsed)) {
    fail(describe(*diagnostic));
  }
  auto checked = expression::check(
      std::get<expression::ParsedExpression>(parsed), environment, destination);
  if (const auto *diagnostics =
          std::get_if<std::vector<expression::Diagnostic>>(&checked)) {
    fail(describe(diagnostics->front()));
  }
  std::vector<std::string> keys;
  for (const auto &[reference, type] :
       std::get<expression::CheckedExpression>(checked).references()) {
    (void)type;
    if (reference.source == expression::Source::kKey) {
      keys.push_back(reference.name);
    }
  }
  return keys;
}

struct ProgramExpressionContext final {
  expression::TypeEnvironment environment;
  std::map<std::string, std::string, std::less<>> key_types;
};

/// Build the expression environment and check every computed key, refusing
/// computed keys that read each other in a cycle.
[[nodiscard]] ProgramExpressionContext
verify_program_expressions(const CanonicalValue::Object &root) {
  ProgramExpressionContext context;
  if (const auto *rows = find_member(root, "parameters")) {
    for (const auto &row : require_array(*rows, "program.parameters")) {
      const auto &fields = require_object(row, "program.parameters[]");
      // Program parameters are fixed authored values with no override path,
      // so the overridability field was removed; refuse it by name.
      if (find_member(fields, "runtime_mutability") != nullptr) {
        fail("program.parameters[].runtime_mutability was removed: Program "
             "parameters always take their Program default; remove the field");
      }
      if (const auto *name = find_member(fields, "parameter_id");
          name && name->is_string() &&
          reserved_system_namespace(name->as_string()))
        fail("reserved_system_namespace: program.parameters: " +
             name->as_string() + " is reserved for system-owned values");
      const auto type = expression_type(require_string(
          require_member(fields, "type", "program.parameters[]"),
          "program.parameters[].type"));
      if (type.has_value()) {
        context.environment.parameters.emplace(
            require_string(
                require_member(fields, "parameter_id", "program.parameters[]"),
                "program.parameters[].parameter_id"),
            *type);
      }
    }
  }
  const auto *blackboard = find_member(root, "blackboard");
  const auto *keys =
      blackboard == nullptr
          ? nullptr
          : find_member(require_object(*blackboard, "program.blackboard"),
                        "keys");
  if (keys == nullptr) {
    return context;
  }
  const auto &rows = require_array(*keys, "program.blackboard.keys");
  for (const auto &row : rows) {
    const auto &fields = require_object(row, "program.blackboard.keys[]");
    const auto &name = require_string(
        require_member(fields, "key", "program.blackboard.keys[]"),
        "program.blackboard.keys[].key");
    const auto &type = require_string(
        require_member(fields, "type", "program.blackboard.keys[]"),
        "program.blackboard.keys[].type");
    context.key_types.emplace(name, type);
    const auto &access = require_string(
        require_member(fields, "access", "program.blackboard.keys[]"),
        "program.blackboard.keys[].access");
    if (access != "read" && access != "read_write") {
      continue;
    }
    if (const auto *record = find_member(fields, "fields")) {
      expression::ValueType value{expression::ValueKind::kRecord, {}};
      for (const auto &field :
           require_array(*record, "program.blackboard.keys[].fields")) {
        const auto &declared = require_object(field, "fields[]");
        const auto field_type = expression_type(
            require_string(require_member(declared, "type", "fields[]"),
                           "fields[].type"));
        if (field_type.has_value()) {
          value.fields.push_back(
              {require_string(require_member(declared, "field", "fields[]"),
                              "fields[].field"),
               *field_type});
        }
      }
      if (!value.fields.empty()) {
        context.environment.keys.emplace(name, std::move(value));
      }
    } else if (const auto engine = expression_type(type)) {
      context.environment.keys.emplace(name, *engine);
    }
  }
  std::map<std::string, std::vector<std::string>> reads;
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const auto &fields = require_object(rows[index], "program.blackboard.keys[]");
    const auto *text = find_member(fields, "expression");
    if (text == nullptr) {
      continue;
    }
    const auto path = index_path("program.blackboard.keys", index);
    const auto &name = require_string(require_member(fields, "key", path), path);
    if (find_member(fields, "initial") != nullptr) {
      fail(path + ".initial: a computed key has no initial value");
    }
    if (require_string(require_member(fields, "persistence", path), path) !=
        "run") {
      fail(path + ".persistence: a computed key cannot be persistent");
    }
    if (require_string(require_member(fields, "access", path), path) !=
        "read") {
      fail(path + ".access: a computed key is read-only; declare access read");
    }
    const auto destination = expression_destination(
        require_string(require_member(fields, "type", path), path));
    if (!destination.has_value() || find_member(fields, "fields") != nullptr) {
      fail(path + ".type: a computed key must be bool, int64, float64 or "
                  "string");
    }
    reads.emplace(name, check_program_expression(
                            require_string(*text, child_path(path, "expression")),
                            context.environment, *destination,
                            child_path(path, "expression")));
  }
  // Depth-first search over computed keys only; a key on the active stack
  // closes a cycle.
  std::map<std::string, int> state;
  std::vector<std::string> stack;
  const auto visit = [&](auto &&self, const std::string &name) -> void {
    const auto marked = state.find(name);
    if (marked != state.end() && marked->second == 2) {
      return;
    }
    if (marked != state.end() && marked->second == 1) {
      std::string cycle;
      for (auto item = std::find(stack.begin(), stack.end(), name);
           item != stack.end(); ++item) {
        cycle += *item + " -> ";
      }
      fail("program.blackboard.keys: computed keys read each other in a "
           "cycle: " +
           cycle + name);
    }
    state[name] = 1;
    stack.push_back(name);
    for (const auto &dependency : reads.at(name)) {
      if (reads.contains(dependency)) {
        self(self, dependency);
      }
    }
    stack.pop_back();
    state[name] = 2;
  };
  for (const auto &[name, unused] : reads) {
    (void)unused;
    visit(visit, name);
  }
  return context;
}

[[nodiscard]] const CanonicalValue::Object &
resolve_catalog_schema(const CanonicalValue::Object &holder,
                       std::string_view path) {
  const CanonicalValue::Object *current = &holder;
  if (const auto *inline_schema = find_member(holder, "schema")) {
    current = &require_object(*inline_schema, path);
  }
  for (std::size_t hops = 0U; hops < 64U; ++hops) {
    const auto *reference = find_member(*current, "schema_ref");
    if (reference == nullptr) {
      return *current;
    }
    const auto &text = require_string(*reference, path);
    constexpr std::string_view prefix = "#/schema_types/";
    if (!text.starts_with(prefix)) {
      fail(std::string(path) + ": invalid catalog schema reference");
    }
    const auto name = text.substr(prefix.size());
    const auto *target =
        execution_catalog().schema_types.find_object_member(name);
    if (target == nullptr) {
      fail(std::string(path) + ": absent catalog schema reference");
    }
    current = &require_object(*target, path);
  }
  fail(std::string(path) + ": catalog schema reference chain is too long");
}

struct CapturedInputSignalUse final {
  std::set<core::ProgramSignalReferenceDTO> signals;
  bool all_uses_are_signal_reads{true};
};

struct CapturedDirectReferences final {
  std::set<core::ProgramTargetReferenceDTO> targets;
  std::set<core::ProgramSignalReferenceDTO> signals;
  std::set<core::ProgramFrameReferenceDTO> frames;
  std::set<core::ProgramCollisionBodyReferenceDTO> bodies;
  std::map<core::CapabilityId, CapturedInputSignalUse> input_signal_uses;
  bool default_tcp_required{};
};

using TargetAssociations =
    std::map<core::ProgramTargetReferenceDTO, CapturedDirectReferences>;

void capture_direct_reference(const CanonicalValue &value, std::string_view kind,
                              CapturedDirectReferences &result,
                              std::string_view path) {
  const auto &object = require_object(value, path);
  const auto component = parse_direct_reference_identity<core::ControllableComponentId>(
      require_member(object, "component_id", path), path);
  if (kind == "target_reference") {
    require_exact_fields(object, {"component_id", "command_target_id"}, path);
    result.targets.insert({component, parse_direct_reference_identity<core::CommandTargetId>(
                                          require_member(object, "command_target_id", path), path)});
  } else if (kind == "signal_reference") {
    require_exact_fields(object, {"component_id", "signal_name"}, path);
    const auto &signal = require_string(require_member(object, "signal_name", path), path);
    if (signal.empty() || signal.size() > 512U || signal.find('\0') != std::string::npos)
      fail(std::string(path) + ": invalid signal name");
    result.signals.insert({component, signal});
  } else if (kind == "frame_reference") {
    require_exact_fields(object, {"component_id", "frame_id"}, path);
    result.frames.insert({component, parse_direct_reference_identity<core::FrameRoleId>(
                                         require_member(object, "frame_id", path), path)});
  } else if (kind == "collision_body_reference") {
    require_exact_fields(object, {"component_id", "collision_body_id"}, path);
    result.bodies.insert({component, parse_direct_reference_identity<core::CollisionBodyRoleId>(
                                         require_member(object, "collision_body_id", path), path)});
  } else {
    fail(std::string(path) + ": unknown direct reference type");
  }
}

void capture_condition_references(const CanonicalValue &condition,
                                  CapturedDirectReferences &result,
                                  std::string_view path, std::size_t depth) {
  if (depth > 8U) fail(std::string(path) + ": condition depth exceeded");
  const auto &object = require_object(condition, path);
  if (object.size() != 1U) fail(std::string(path) + ": invalid condition");
  const auto &[op, value] = object.front();
  if (op == "all" || op == "any") {
    for (const auto &item : require_array(value, path))
      capture_condition_references(item, result, path, depth + 1U);
  } else if (op == "not") {
    capture_condition_references(value, result, path, depth + 1U);
  } else if (op == "compare") {
    const auto &compare = require_object(value, path);
    const auto &operand = require_object(require_member(compare, "operand", path), path);
    if (const auto *signal = find_member(operand, "signal"))
      capture_direct_reference(*signal, "signal_reference", result, path);
  } else {
    fail(std::string(path) + ": invalid condition operator");
  }
}

void capture_schema_references(const CanonicalValue &value,
                               const CanonicalValue::Object &holder,
                               CapturedDirectReferences &result,
                               std::string_view path, std::size_t depth) {
  if (depth > 256U) fail(std::string(path) + ": schema depth exceeded");
  const auto &shape = resolve_catalog_schema(holder, path);
  if (const auto *semantic = find_member(shape, "semantic_type")) {
    const auto &kind = require_string(*semantic, path);
    if (kind == "target_reference" || kind == "signal_reference" ||
        kind == "frame_reference" || kind == "collision_body_reference") {
      capture_direct_reference(value, kind, result, path);
      return;
    }
  }
  if (const auto *type = find_member(shape, "type");
      type && type->is_string() && type->as_string() == "component_interrupt_condition") {
    capture_condition_references(value, result, path, 1U);
    return;
  }
  if (value.is_object()) {
    const auto *properties = find_member(shape, "properties");
    if (!properties) return;
    const auto &fields = require_object(*properties, path);
    for (const auto &[name, child] : value.as_object()) {
      if (const auto *child_schema = find_member(fields, name))
        capture_schema_references(child, require_object(*child_schema, path),
                                  result, path, depth + 1U);
    }
  } else if (value.is_array()) {
    const auto *items = find_member(shape, "items");
    if (!items) return;
    for (const auto &child : value.as_array())
      capture_schema_references(child, require_object(*items, path),
                                result, path, depth + 1U);
  }
}

[[nodiscard]] CapturedDirectReferences capture_node_references(
    const CanonicalValue::Object &node, const CanonicalValue::Object &entry,
    const CanonicalValue::Object &root, std::string_view path) {
  CapturedDirectReferences result;
  const auto &parameters = require_object(require_member(node, "parameters", path), path);
  for (const auto &value : require_array(require_member(entry, "parameters", path), path)) {
    const auto &definition = require_object(value, path);
    const auto &id = require_string(require_member(definition, "id", path), path);
    const auto *selected = find_member(parameters, id);
    if (!selected) selected = find_member(definition, "default");
    if (selected) {
      capture_schema_references(*selected, definition, result, path, 0U);
      // A key-typed value has no fixed semantic_type in the catalog. Its
      // literal key determines whether the value is a stored pose.
      const auto &shape = resolve_catalog_schema(definition, path);
      const auto *from_key = find_member(shape, "value_type_from_key");
      const auto *key_name = from_key && from_key->is_string()
                                 ? find_member(parameters, from_key->as_string())
                                 : nullptr;
      const auto *blackboard = find_member(root, "blackboard");
      const auto *keys = blackboard && blackboard->is_object()
                             ? find_member(blackboard->as_object(), "keys")
                             : nullptr;
      const auto *key = key_name && key_name->is_string()
                            ? find_declaration(keys, "key", key_name->as_string(), path)
                            : nullptr;
      const auto *key_type = key ? find_member(*key, "type") : nullptr;
      if (key_type && key_type->is_string() && key_type->as_string() == "stored_pose") {
        const auto &pose = require_object(*selected, path);
        const auto &frame = require_object(require_member(pose, "frame_ref", path), path);
        const auto &kind = require_string(require_member(frame, "kind", path), path);
        if (kind == "target_frame" || kind == "component_frame")
          capture_direct_reference(require_member(frame, "frame", path),
                                   "frame_reference", result, path);
      }
    }
  }
  return result;
}

void capture_node_associations(
    const CanonicalValue::Object &node, const CapturedDirectReferences &references,
    const std::vector<RetainedIoSignalConfiguration> &signals,
    CapturedDirectReferences &all_references,
    TargetAssociations &associations, std::string_view path) {
  const auto &parameters = require_object(require_member(node, "parameters", path), path);
  const auto target_for = [&](std::string_view parameter)
      -> std::optional<core::ProgramTargetReferenceDTO> {
    const auto *value = find_member(parameters, parameter);
    if (!value) return std::nullopt;
    CapturedDirectReferences selected;
    capture_direct_reference(*value, "target_reference", selected, path);
    return *selected.targets.begin();
  };
  const auto main_target = target_for("target");
  const auto tool_target = target_for("tool_target");
  const auto append_one = [&](const std::optional<core::ProgramTargetReferenceDTO> &target,
                              std::string_view parameter, std::string_view kind) {
    const auto *value = find_member(parameters, parameter);
    if (!target || !value) return;
    capture_direct_reference(*value, kind, associations[*target], path);
  };
  append_one(main_target, "base_frame", "frame_reference");
  append_one(tool_target ? tool_target : main_target, "carrier_frame_ref", "frame_reference");
  if (const auto *bodies = find_member(parameters, "touch_bodies")) {
    const auto target = tool_target ? tool_target : main_target;
    if (!target) fail(std::string(path) + ": touch bodies have no selected target");
    for (const auto &body : require_array(*bodies, path))
      capture_direct_reference(body, "collision_body_reference", associations[*target], path);
  }
  for (const auto parameter : {"tcp_endpoint", "tcp"}) {
    const auto *tcp = find_member(parameters, parameter);
    if (!tcp) continue;
    const auto &selector = require_object(*tcp, path);
    const auto *kind = find_member(selector, "kind");
    if (kind && kind->is_string() && kind->as_string() == "default_of_target") {
      CapturedDirectReferences target;
      capture_direct_reference(require_member(selector, "target", path),
                               "target_reference", target, path);
      associations[*target.targets.begin()].default_tcp_required = true;
      continue;
    }
    if (kind && (!kind->is_string() || kind->as_string() != "entry")) continue;
    if (const auto *target_value = find_member(selector, "target")) {
      CapturedDirectReferences target;
      capture_direct_reference(*target_value, "target_reference", target, path);
      capture_direct_reference(require_member(selector, "frame", path), "frame_reference",
                               associations[*target.targets.begin()], path);
    }
  }
  for (const auto &reference : references.signals) {
    const auto matches = FindProgramSignalMatches(signals, reference);
    if (matches.size() != 1U)
      fail(std::string(path) + ": signal reference is missing or ambiguous");
    auto &associated = associations[{reference.component_id,
                                      matches.front().retained->command_target_id}];
    associated.signals.insert(reference);
    if (matches.front().changed_outside) {
      const core::ProgramSignalReferenceDTO base{
          reference.component_id, matches.front().retained->descriptor.signal_id.name};
      associated.signals.insert(base);
      all_references.signals.insert(base);
    }
  }
}

void capture_non_node_references(const CanonicalValue::Object &root,
                                 CapturedDirectReferences &result,
                                 TargetAssociations &associations) {
  const auto capture_frame_ref = [&](const CanonicalValue &value,
                                     std::string_view path) {
    const auto &frame = require_object(value, path);
    const auto &kind = require_string(require_member(frame, "kind", path), path);
    if (kind == "target_frame" || kind == "component_frame")
      capture_direct_reference(require_member(frame, "frame", path),
                               "frame_reference", result, path);
  };
  const auto capture_typed = [&](const CanonicalValue::Object &row,
                                 std::string_view field, std::string_view path) {
    const auto *type = find_member(row, "type");
    const auto *value = find_member(row, field);
    if (!type || !type->is_string() || !value) return;
    if (type->as_string() == "stored_pose") {
      const auto &pose = require_object(*value, path);
      capture_frame_ref(require_member(pose, "frame_ref", path), path);
    } else if (type->as_string() == "tcp_entry") {
      const auto &entry = require_object(*value, path);
      require_exact_fields(entry, {"target", "frame"}, path);
      CapturedDirectReferences target;
      capture_direct_reference(require_member(entry, "target", path),
                               "target_reference", target, path);
      result.targets.insert(target.targets.begin(), target.targets.end());
      capture_direct_reference(require_member(entry, "frame", path),
                               "frame_reference", result, path);
      capture_direct_reference(require_member(entry, "frame", path),
                               "frame_reference", associations[*target.targets.begin()], path);
    }
  };
  if (const auto *scene = find_member(root, "scene")) {
    for (const auto &value : require_array(*scene, "program.scene")) {
      const auto &row = require_object(value, "program.scene[]");
      const auto &pose = require_object(require_member(row, "pose", "scene"), "scene.pose");
      capture_frame_ref(require_member(pose, "frame_ref", "scene.pose"), "scene.pose.frame_ref");
    }
  }
  if (const auto *policies = find_member(root, "contact_policies")) {
    for (const auto &value : require_array(*policies, "program.contact_policies")) {
      const auto &policy = require_object(value, "program.contact_policies[]");
      const auto &zones = require_array(require_member(policy, "zones", "contact policy"),
                                        "contact policy.zones");
      for (const auto &zone_value : zones) {
        const auto &zone = require_object(zone_value, "contact policy zone");
        const auto &pairs = require_array(require_member(zone, "body_pairs", "zone"), "zone.body_pairs");
        for (const auto &pair_value : pairs) {
          const auto &pair = require_object(pair_value, "zone.body_pairs[]");
          for (const auto side : {"body_a", "body_b"}) {
            const auto &selector = require_object(require_member(pair, side, "body pair"), side);
            if (require_string(require_member(selector, "kind", side), side) == "component_model_body")
              capture_direct_reference(require_member(selector, "body", side),
                                       "collision_body_reference", result, side);
          }
        }
      }
    }
  }
  if (const auto *parameters = find_member(root, "parameters")) {
    for (const auto &value : require_array(*parameters, "program.parameters"))
      capture_typed(require_object(value, "program.parameters[]"), "default", "program.parameters[].default");
  }
  if (const auto *blackboard = find_member(root, "blackboard")) {
    const auto &object = require_object(*blackboard, "program.blackboard");
    if (const auto *keys = find_member(object, "keys")) {
      for (const auto &value : require_array(*keys, "program.blackboard.keys")) {
        const auto &row = require_object(value, "program.blackboard.keys[]");
        capture_typed(row, "initial", "program.blackboard.keys[].initial");
        if (const auto *frames = find_member(row, "allowed_frames"))
          for (const auto &frame : require_array(*frames, "blackboard.allowed_frames"))
            capture_frame_ref(frame, "blackboard.allowed_frames[]");
      }
    }
  }
}

/// Where a path-keyed value source writes: the catalog schema at the path,
/// or the key type of a whole key-typed value (`schema` is then null).
struct SourceDestination final {
  const CanonicalValue::Object *schema{};
  std::string key;
  std::string key_type;
};

/// A path into a value must name a value of the literal, or of the catalog
/// default when there is no literal; a list index must be within the list.
/// Only the first `depth` segments are checked: a source may supply an absent
/// map entry, whose parent map must exist.
void require_value_at(const ValuePath &value_path, std::size_t depth,
                      const CanonicalValue::Object &definition,
                      const CanonicalValue::Object &parameters,
                      const std::string &path) {
  const CanonicalValue *current =
      find_member(parameters, std::get<std::string>(value_path.front()));
  if (current == nullptr) {
    current = find_member(definition, "default");
  }
  for (std::size_t item = 1U; current != nullptr && item < depth; ++item) {
    if (const auto *index = std::get_if<std::size_t>(&value_path[item])) {
      current = current->is_array() && *index < current->as_array().size()
                    ? &current->as_array()[*index]
                    : nullptr;
    } else {
      current = current->is_object()
                    ? find_member(current->as_object(),
                                  std::get<std::string>(value_path[item]))
                    : nullptr;
    }
  }
  if (current == nullptr) {
    fail(path + ": names no value in the node's parameters");
  }
}

/// Whether the path ends at an entry of a map (an object's additional
/// properties), which a source may supply without a literal entry. A path the
/// schema does not admit is not a map entry.
[[nodiscard]] bool names_map_entry(const ValuePath &value_path,
                                   const CanonicalValue::Object &root_schema,
                                   const std::string &path) {
  const CanonicalValue::Object *schema = &root_schema;
  for (std::size_t item = 1U; item + 1U < value_path.size(); ++item) {
    const auto *type = find_member(*schema, "type");
    const std::string kind =
        type == nullptr ? std::string() : require_string(*type, path);
    const CanonicalValue *child = nullptr;
    if (std::holds_alternative<std::size_t>(value_path[item])) {
      child = kind == "array" ? find_member(*schema, "items") : nullptr;
    } else if (kind == "object") {
      if (const auto *properties = find_member(*schema, "properties")) {
        child = find_member(require_object(*properties, path),
                            std::get<std::string>(value_path[item]));
      }
      if (child == nullptr) {
        child = find_member(*schema, "additional_properties");
      }
    }
    if (child == nullptr || !child->is_object()) {
      return false;
    }
    schema = &resolve_catalog_schema(child->as_object(), path);
  }
  const auto *last = std::get_if<std::string>(&value_path.back());
  const auto *type = find_member(*schema, "type");
  const auto *extra = find_member(*schema, "additional_properties");
  const auto *properties = find_member(*schema, "properties");
  return last != nullptr && type != nullptr &&
         require_string(*type, path) == "object" && extra != nullptr &&
         extra->is_object() &&
         (properties == nullptr ||
          find_member(require_object(*properties, path), *last) == nullptr);
}

[[nodiscard]] SourceDestination
value_source_destination(const ValuePath &value_path,
                         const CanonicalValue::Object &definition,
                         const CanonicalValue::Object &parameters,
                         const ProgramExpressionContext &context,
                         const std::string &path) {
  const CanonicalValue::Object *schema =
      &resolve_catalog_schema(definition, path);
  if (const auto *from_key = find_member(*schema, "value_type_from_key")) {
    const auto *key_name =
        find_member(parameters, require_string(*from_key, path));
    const auto key =
        key_name == nullptr
            ? context.key_types.end()
            : context.key_types.find(require_string(*key_name, path));
    if (key == context.key_types.end()) {
      fail(path + ": key-typed value source names no declared blackboard key");
    }
    if (value_path.size() == 1U) {
      return {nullptr, key->first, key->second};
    }
    if (key->second != "stored_pose") {
      fail(path + ": " + key->second + " values have no fields");
    }
    const auto *pose = execution_catalog().schema_types.find_object_member(
        "stored_pose");
    if (pose == nullptr) {
      fail(path + ": absent catalog stored_pose schema");
    }
    schema = &require_object(*pose, path);
  }
  if (value_path.size() > 1U) {
    require_value_at(value_path,
                     value_path.size() -
                         (names_map_entry(value_path, *schema, path) ? 1U : 0U),
                     definition, parameters, path);
  }
  std::string previous = std::get<std::string>(value_path.front());
  for (std::size_t item = 1U; item < value_path.size(); ++item) {
    const auto *type = find_member(*schema, "type");
    const std::string kind =
        type == nullptr ? std::string() : require_string(*type, path);
    const CanonicalValue *child = nullptr;
    if (std::holds_alternative<std::size_t>(value_path[item])) {
      if (kind != "array" || find_member(*schema, "items") == nullptr) {
        fail(path + ": indexes a value that is not a list");
      }
      child = find_member(*schema, "items");
      previous.clear();
    } else {
      const auto &name = std::get<std::string>(value_path[item]);
      if (previous == "orientation" && name == "xyzw") {
        fail(path + ": quaternion components depend on each other; an xyzw "
                    "orientation is typed or bound whole");
      }
      if (kind == "object") {
        if (const auto *properties = find_member(*schema, "properties")) {
          child = find_member(require_object(*properties, path), name);
        }
        if (child == nullptr) {
          const auto *extra = find_member(*schema, "additional_properties");
          child = extra != nullptr && extra->is_object() ? extra : nullptr;
        }
      }
      if (child == nullptr) {
        fail(path + ": names no field '" + name + "'");
      }
      previous = name;
    }
    schema = &resolve_catalog_schema(require_object(*child, path), path);
  }
  return {schema, {}, {}};
}

[[nodiscard]] std::string schema_kind(const CanonicalValue::Object &schema,
                                      const std::string &path) {
  const auto *type = find_member(schema, "type");
  return type == nullptr ? std::string() : require_string(*type, path);
}

[[nodiscard]] const std::string *
schema_semantic(const CanonicalValue::Object &schema, const std::string &path) {
  const auto *semantic = find_member(schema, "semantic_type");
  return semantic == nullptr ? nullptr : &require_string(*semantic, path);
}

/// Whether `semantic` occurs at or anywhere below `schema`.
[[nodiscard]] bool
schema_contains_semantic(const CanonicalValue::Object &schema,
                         std::string_view semantic, const std::string &path,
                         std::set<const CanonicalValue::Object *> &visited) {
  if (!visited.insert(&schema).second) {
    return false;
  }
  if (const auto *own = schema_semantic(schema, path);
      own != nullptr && *own == semantic) {
    return true;
  }
  const auto below = [&](const CanonicalValue *child) {
    return child != nullptr && child->is_object() &&
           schema_contains_semantic(
               resolve_catalog_schema(child->as_object(), path), semantic,
               path, visited);
  };
  if (const auto *properties = find_member(schema, "properties")) {
    for (const auto &[name, child] : require_object(*properties, path)) {
      (void)name;
      if (below(&child)) {
        return true;
      }
    }
  }
  return below(find_member(schema, "items")) ||
         below(find_member(schema, "additional_properties"));
}

[[nodiscard]] bool has_properties(const CanonicalValue::Object &schema,
                                  std::initializer_list<std::string_view> names,
                                  const std::string &path) {
  const auto *properties = find_member(schema, "properties");
  if (properties == nullptr) {
    return false;
  }
  const auto &fields = require_object(*properties, path);
  return std::all_of(names.begin(), names.end(), [&](std::string_view name) {
    return find_member(fields, name) != nullptr;
  });
}

/// The declared type a binding reads: a Program parameter, a readable key
/// or a declared field of one.
[[nodiscard]] std::string source_declared_type(const CanonicalValue::Object &source,
                                               const CanonicalValue::Object &root,
                                               const std::string &path) {
  const auto &kind = require_string(require_member(source, "kind", path), path);
  const auto &reference =
      require_string(require_member(source, "reference", path), path);
  const CanonicalValue::Object *row = nullptr;
  if (kind == "program_parameter") {
    row = find_declaration(find_member(root, "parameters"), "parameter_id",
                           reference, "program.parameters");
    if (row == nullptr) {
      fail(path + ".reference: names no Program parameter");
    }
    return require_string(require_member(*row, "type", path), path);
  }
  const auto *blackboard = find_member(root, "blackboard");
  row = find_declaration(
      blackboard == nullptr
          ? nullptr
          : find_member(require_object(*blackboard, "program.blackboard"),
                        "keys"),
      "key", reference, "program.blackboard.keys");
  if (row == nullptr) {
    fail(path + ".reference: names no blackboard key");
  }
  const auto &access = require_string(require_member(*row, "access", path), path);
  if (access != "read" && access != "read_write") {
    fail(path + ".reference: does not permit reads");
  }
  if (kind == "blackboard_field") {
    row = find_declaration(
        find_member(*row, "fields"), "field",
        require_string(require_member(source, "field", path), path),
        "program.blackboard.keys[].fields");
    if (row == nullptr) {
      fail(path + ".field: names no declared field");
    }
  }
  return require_string(require_member(*row, "type", path), path);
}

[[nodiscard]] bool source_type_compatible(std::string_view source_type,
                                          const CanonicalValue::Object &destination,
                                          const std::string &path) {
  const auto *semantic = schema_semantic(destination, path);
  if (semantic != nullptr && *semantic == "scene_resource") {
    return source_type == "scene_resource";
  }
  const auto kind = schema_kind(destination, path);
  if (kind == "key_value") {
    const auto &types =
        require_array(require_member(destination, "value_types", path), path);
    return std::find(types.begin(), types.end(),
                     CanonicalValue(std::string(source_type))) != types.end();
  }
  if (kind == "float") {
    return source_type == "float64";
  }
  if (kind == "int") {
    return source_type == "int64";
  }
  if (kind == "bool") {
    return source_type == "bool";
  }
  if (kind == "string" || kind == "enum") {
    return source_type == "string";
  }
  if (kind == "array") {
    const auto *items = find_member(destination, "items");
    if (items == nullptr) {
      return false;
    }
    const auto &item =
        resolve_catalog_schema(require_object(*items, path), path);
    if (source_type == "string_list") {
      return schema_kind(item, path) == "string";
    }
    if (source_type == "cartesian_path") {
      return schema_kind(item, path) == "object" &&
             has_properties(item, {"position_m", "orientation"}, path);
    }
    return false;
  }
  if (kind == "object") {
    if (source_type == "stored_pose") {
      return has_properties(destination, {"position_m", "orientation"}, path);
    }
    if (source_type == "coordinate_position") {
      return has_properties(destination, {"kind", "value_m", "value_rad"},
                            path);
    }
  }
  return false;
}

/// The parameters whose values select the node's binding effects.
[[nodiscard]] std::set<std::string>
selector_parameters(const CanonicalValue::Object &entry, std::string_view path) {
  std::set<std::string> selectors;
  for (const auto &requirement : require_array(
           require_member(entry, "required_capabilities", path), path)) {
    const auto &fields =
        require_object(requirement, "catalog.required_capability");
    for (const auto *name :
         {find_member(fields, "target_parameter"),
          find_member(fields, "component_parameter")}) {
      if (name != nullptr) {
        selectors.insert(require_string(*name, path));
      }
    }
    if (const auto *when = find_member(fields, "when")) {
      selectors.insert(require_string(
          require_member(require_object(*when, path), "parameter", path),
          path));
    }
  }
  return selectors;
}

// The same refusals as the Python Program validator's path-keyed value-source
// checks (program_value_source_conformance.v1.yaml runs both).
void verify_node_value_sources(const CanonicalValue::Object &node,
                               const CanonicalValue::Object &entry,
                               const CanonicalValue::Object &root,
                               const ProgramExpressionContext &context,
                               std::string_view path) {
  static const std::set<std::string, std::less<>> kForbiddenParameters{
      "automatic_retries", "automatic_retry_delay_s", "preemption_priority",
      "preempted_result",  "contact_policy_id",       "model_endpoint_alias",
      "mock_simulation",
      "prompt_template_id"};
  static const std::set<std::string, std::less<>> kForbiddenSemantics{
      "blackboard_key", "collision_body_reference", "frame_reference",
      "signal_reference", "source_joint", "target_reference"};
  const auto *sources_value = find_member(node, "value_sources");
  if (sources_value == nullptr) {
    return;
  }
  const auto &parameters =
      require_object(require_member(node, "parameters", path), path);
  const auto &definitions =
      require_array(require_member(entry, "parameters", path), path);
  const auto selectors = selector_parameters(entry, path);
  const auto &sources = require_object(*sources_value, path);
  const auto definition_of = [&](const std::string &id) {
    const CanonicalValue::Object *definition = nullptr;
    for (const auto &candidate : definitions) {
      const auto &fields = require_object(candidate, "catalog.parameter");
      if (require_string(require_member(fields, "id", path), path) == id) {
        definition = &fields;
      }
    }
    return definition;
  };
  for (const auto &[key, unused] : sources) {
    (void)unused;
    const auto name = std::get<std::string>(parse_value_path(key).front());
    if (definition_of(name) == nullptr) {
      fail(std::string(path) + ": supplies unknown catalog parameter '" +
           name + "'");
    }
  }
  for (const auto &[key, unused] : sources) {
    (void)unused;
    const auto name = std::get<std::string>(parse_value_path(key).front());
    if (kForbiddenParameters.contains(name) || selectors.contains(name)) {
      fail(child_path(child_path(path, "value_sources"), key) +
           ": cannot dynamically select runtime/binding semantics");
    }
  }
  for (const auto &[key, source_value] : sources) {
    const auto entry_path = child_path(child_path(path, "value_sources"), key);
    const auto &source = require_object(source_value, entry_path);
    const auto value_path = parse_value_path(key);
    // Set TCP accepts a whole tcp_entry from a blackboard key, matching the
    // Program loader. Other sources cannot dynamically choose a TCP selector.
    if (require_string(require_member(node, "node_id", path), path) == "motion.set_tcp" &&
        value_path.size() == 1U && std::get<std::string>(value_path.front()) == "tcp") {
      const auto source_type = source_declared_type(source, root, entry_path);
      if (require_string(require_member(source, "kind", entry_path), entry_path) != "blackboard_key" ||
          source_type != "tcp_entry") {
        fail(entry_path + ": Set TCP requires a tcp_entry blackboard key");
      }
      continue;
    }
    const auto destination = value_source_destination(
        value_path,
        *definition_of(std::get<std::string>(value_path.front())), parameters,
        context, entry_path);
    if (destination.schema != nullptr) {
      const auto *semantic = schema_semantic(*destination.schema, entry_path);
      std::set<const CanonicalValue::Object *> visited;
      if ((semantic != nullptr && kForbiddenSemantics.contains(*semantic)) ||
          schema_contains_semantic(*destination.schema, "source_joint",
                                   entry_path, visited)) {
        fail(entry_path +
             ": cannot dynamically select runtime/binding semantics");
      }
      const auto kind = schema_kind(*destination.schema, entry_path);
      if (value_path.size() > 1U &&
          (kind == "string" || kind == "enum" || semantic != nullptr)) {
        fail(entry_path +
             ": nested text, choice and identifier fields stay literal");
      }
    }
    const auto &kind = require_string(
        require_member(source, "kind", entry_path), entry_path);
    if (kind == "expression") {
      std::optional<expression::ValueKind> computes;
      if (destination.schema == nullptr) {
        computes = expression_destination(destination.key_type);
      } else if (schema_semantic(*destination.schema, entry_path) == nullptr) {
        const auto type = schema_kind(*destination.schema, entry_path);
        if (type == "float") {
          computes = expression::ValueKind::kFloat64;
        } else if (type == "int") {
          computes = expression::ValueKind::kInt64;
        } else if (type == "bool") {
          computes = expression::ValueKind::kBool;
        } else if (type == "string") {
          computes = expression::ValueKind::kString;
        }
      }
      if (!computes.has_value()) {
        fail(entry_path + ": an expression computes only a bool, int64, "
                          "float64 or string value (not a choice, identifier "
                          "or whole value)");
      }
      (void)check_program_expression(
          require_string(require_member(source, "expression", entry_path),
                         entry_path),
          context.environment, *computes, child_path(entry_path, "expression"));
      continue;
    }
    const auto source_type = source_declared_type(source, root, entry_path);
    if (destination.schema == nullptr) {
      if (source_type != destination.key_type) {
        fail(entry_path + ": source type '" + source_type +
             "' does not match the '" + destination.key_type +
             "' type of key '" + destination.key + "'");
      }
      continue;
    }
    if (!source_type_compatible(source_type, *destination.schema, entry_path)) {
      fail(entry_path + ": source type '" + source_type +
           "' is incompatible with the destination schema");
    }
  }
}

void validate_automatic_motion_retry_policy(
    const CanonicalValue::Object &node, const CanonicalValue::Object &entry,
    const std::string &node_id, std::string_view path) {
  if (node_id != "motion.move_to_joint_target" &&
      node_id != "motion.move_to_pose" &&
      node_id != "motion.follow_cartesian_path") {
    return;
  }
  const auto *value =
      literal_node_parameter(node, entry, "automatic_retries", path);
  if (value == nullptr)
    fail(child_path(path, "automatic_retries") +
         ": missing literal or catalog default");
  const double count =
      require_number(*value, child_path(path, "automatic_retries"));
  if (count < 0.0 || count > 10.0 || std::trunc(count) != count) {
    fail(child_path(path, "automatic_retries") + ": expected integer in 0..10");
  }
}

[[nodiscard]] std::uint8_t
capture_preemption_priority(const CanonicalValue::Object &node,
                            const CanonicalValue::Object &entry,
                            const std::string &node_id, std::string_view path) {
  if (node_id != "motion.move_to_joint_target" &&
      node_id != "motion.move_to_pose" &&
      node_id != "motion.follow_cartesian_path") {
    return 0U;
  }
  const auto priority_path = child_path(path, "preemption_priority");
  const auto *value =
      literal_node_parameter(node, entry, "preemption_priority", path);
  if (value == nullptr) {
    fail(priority_path + ": missing literal or catalog default");
  }
  const auto priority = require_number(*value, priority_path);
  if (priority < 0.0 || priority > 100.0 || std::trunc(priority) != priority) {
    fail(priority_path + ": expected integer in 0..100");
  }
  return static_cast<std::uint8_t>(priority);
}

void verify_node_outcome_writes(const CanonicalValue::Object &node,
                                const CanonicalValue::Object &entry,
                                std::string_view path) {
  const auto *blackboard = find_member(entry, "blackboard");
  const auto *writes =
      blackboard == nullptr
          ? nullptr
          : find_member(require_object(*blackboard, "catalog.blackboard"),
                        "writes");
  if (writes == nullptr)
    return;
  const auto &parameters = require_object(require_member(node, "parameters", path), path);
  for (const auto &value :
       require_array(*writes, "catalog.blackboard.writes")) {
    const auto &binding = require_object(value, "catalog.blackboard.writes[]");
    const auto &parameter =
        require_string(require_member(binding, "key_parameter", path), path);
    const auto *key = find_member(parameters, parameter);
    if (key == nullptr) {
      if (const auto *definition = find_declaration(find_member(entry, "parameters"), "id",
                                                     parameter, "catalog.parameters")) {
        key = find_member(*definition, "default");
      }
    }
    if (key != nullptr &&
        require_string(*key, path).starts_with("node_outcome.")) {
      fail(std::string(path) + ": system-owned node outcome keys cannot be "
                               "written by catalog bindings");
    }
  }
}

void verify_catalog_node_semantics(
    const CanonicalValue &tree, const CanonicalValue::Object &root,
    const core::RetainedNodeEnvironmentDTO &environment,
    const ProgramExpressionContext &expressions, const PoseFrames &frames) {
  const auto catalog = qualified_program_catalog(environment);
  const auto visit = [&](auto &&self, const CanonicalValue &value) -> void {
    const auto &node = require_object(value, "program.tree");
    const auto &instance = require_string(require_member(node, "instance_id", "tree"), "instance_id");
    const auto path = "program.tree." + instance;
    const auto &node_id = require_string(require_member(node, "node_id", path), path);
    const auto definition = catalog.entries.find(node_id);
    if (definition == catalog.entries.end())
      fail(path + ": node is absent from the qualified catalog");
    const auto &entry = require_object(definition->second, "catalog.entry");
    (void)capture_node_references(node, entry, root, path);
    verify_node_outcome_writes(node, entry, path);
    verify_key_typed_parameters(node, entry, root, frames, path);
    verify_node_value_sources(node, entry, root, expressions, path);
    if (node_id == "scene.attach_carried_object") {
      const auto &parameters = require_object(require_member(node, "parameters", path), path);
      bool literal_frame = find_member(parameters, "carrier_frame_ref") != nullptr;
      bool active_frame = find_member(parameters, "carrier_frame") != nullptr;
      if (const auto *sources = find_member(node, "value_sources")) {
        for (const auto &[key, unused] : require_object(*sources, path)) {
          (void)unused;
          const auto parts = parse_value_path(key);
          const auto &name = std::get<std::string>(parts.front());
          literal_frame = literal_frame || name == "carrier_frame_ref";
          active_frame = active_frame || name == "carrier_frame";
        }
      }
      if (literal_frame == active_frame)
        fail(path + ": requires exactly one carrier frame selector");
    }
    validate_automatic_motion_retry_policy(node, entry, node_id, path);
    for (const auto &child : require_array(require_member(node, "children", path), path))
      self(self, child);
  };
  visit(visit, tree);
}

[[nodiscard]] core::ProgramExecutionTableDTO capture_execution_table(
    const CanonicalValue &tree, const CanonicalValue::Object &root,
    const std::set<std::string> &instances,
    const core::RetainedNodeEnvironmentDTO &environment,
    const std::vector<RetainedIoSignalConfiguration> &signals,
    CapturedDirectReferences &references,
    TargetAssociations &associations,
    const std::map<std::string, core::ProgramExecutionPolicy> &group_policies) {
  const auto catalog = qualified_program_catalog(environment);
  core::ProgramExecutionTableDTO table;
  table.nodes.reserve(instances.size());
  for (const auto &instance : instances) {
    table.nodes.push_back({core::ProgramNodeInstanceId(instance), {}, {}});
  }
  std::map<std::string, std::vector<std::uint32_t>> selections;
  struct NodeShape final {
    std::string node_id;
    std::string parent_instance_id;
    std::vector<std::string> direct_groups;
    bool parallel_success_on_all{};
  };
  std::map<std::string, NodeShape> shapes;
  std::size_t membership_count = 0U;
  std::size_t effect_count = 0U;
  const auto visit =
      [&](auto &&self, const CanonicalValue &value,
          const std::vector<std::string> &inherited,
          const std::string &parent_instance_id,
          const core::ProgramOperatorStopPolicy inherited_stop_policy,
          const std::optional<std::uint32_t> parent_node_index) -> void {
    const auto &node = require_object(value, "program.tree");
    const auto &instance = require_string(
        require_member(node, "instance_id", "tree"), "instance_id");
    const auto path = "program.tree." + instance;
    const auto authored = execution_groups(node, path);
    const auto &groups = authored.has_value() ? *authored : inherited;
    if (groups.size() >
        core::kMaxProgramExecutionMemberships - membership_count) {
      fail(path + ": resolved execution memberships exceed fixed bound");
    }
    membership_count += groups.size();
    auto selected = std::lower_bound(
        table.nodes.begin(), table.nodes.end(),
        core::ProgramNodeInstanceId(instance),
        [](const auto &item, const auto &id) { return item.instance_id < id; });
    const auto index =
        static_cast<std::uint32_t>(selected - table.nodes.begin());
    for (const auto &group : groups) {
      auto [entry, inserted] = selections.try_emplace(group);
      if (inserted && selections.size() > core::kMaxProgramExecutionGroups) {
        fail(path + ": execution group names exceed fixed bound");
      }
      entry->second.push_back(index);
    }
    const auto &node_id =
        require_string(require_member(node, "node_id", path), path);
    bool parallel_success_on_all = false;
    if (node_id == "parallel") {
      const auto &parameters =
          require_object(require_member(node, "parameters", path), path);
      const auto *policy = find_member(parameters, "success_policy");
      parallel_success_on_all =
          policy == nullptr || require_string(*policy, path) == "all";
    }
    shapes.emplace(
        instance,
        NodeShape{node_id, parent_instance_id,
                  authored.has_value() ? *authored : std::vector<std::string>{},
                  parallel_success_on_all});
    const auto definition = catalog.entries.find(node_id);
    if (definition == catalog.entries.end())
      fail(path + ": node is absent from the qualified catalog");
    const auto &entry = require_object(definition->second, "catalog.entry");
    const auto node_references = capture_node_references(node, entry, root, path);
    selected->signal_references.assign(node_references.signals.begin(),
                                       node_references.signals.end());
    references.targets.insert(node_references.targets.begin(), node_references.targets.end());
    references.signals.insert(node_references.signals.begin(), node_references.signals.end());
    references.frames.insert(node_references.frames.begin(), node_references.frames.end());
    references.bodies.insert(node_references.bodies.begin(), node_references.bodies.end());
    capture_node_associations(node, node_references, signals, references, associations, path);
    const auto &kind =
        require_string(require_member(entry, "kind", path), path);
    const auto *authored_stop_policy =
        find_member(node, "operator_stop_policy");
    auto stop_policy = authored_stop_policy == nullptr
                           ? core::ProgramOperatorStopPolicy::kInherit
                           : parse_stop_policy(*authored_stop_policy, path);
    if (stop_policy == core::ProgramOperatorStopPolicy::kInherit) {
      stop_policy = !parent_node_index.has_value()
                        ? core::ProgramOperatorStopPolicy::kContinue
                        : ((kind == "composite" || kind == "decorator")
                               ? inherited_stop_policy
                               : core::ProgramOperatorStopPolicy::kHold);
    }
    // Match Behavior's node-local resolution. A Hold ancestor blocks only
    // when its whole subtree is in the current authoritative held selection.
    selected->effective_operator_stop_policy = stop_policy;
    selected->parent_node_index = parent_node_index;
    const auto &execution =
        require_object(require_member(entry, "execution", path), path);
    selected->owner_api =
        require_string(require_member(execution, "owner_api", path), path);
    selected->preemption_priority = capture_preemption_priority(node, entry, node_id, path);
    selected->target_effects = node_target_effects(node, entry, signals, path);
    for (const auto &effect : selected->target_effects) {
      const auto &capability = effect.capability_id.value();
      if (capability != "io.gpio.get_input" && capability != "io.gpio.wait_for_input")
        continue;
      auto &use = associations[effect.target].input_signal_uses[effect.capability_id];
      const bool input_node =
          ((node_id == "io.get_input" || node_id == "io.check_input") &&
           capability == "io.gpio.get_input") ||
          (node_id == "io.wait_for_input" && capability == "io.gpio.wait_for_input");
      const auto &parameters = require_object(require_member(node, "parameters", path), path);
      const auto *value = find_member(parameters, "signal");
      if (!input_node || !value) {
        use.all_uses_are_signal_reads = false;
        continue;
      }
      CapturedDirectReferences selected_signal;
      capture_direct_reference(*value, "signal_reference", selected_signal, path);
      const auto &reference = *selected_signal.signals.begin();
      const auto matches = FindProgramSignalMatches(signals, reference);
      if (matches.size() != 1U || reference.component_id != effect.target.component_id ||
          matches.front().retained->command_target_id != effect.target.command_target_id) {
        use.all_uses_are_signal_reads = false;
        continue;
      }
      use.signals.insert(reference);
    }
    if (selected->target_effects.size() >
        core::kMaxProgramTargetEffects - effect_count) {
      fail(path + ": resolved target effects exceed fixed bound");
    }
    effect_count += selected->target_effects.size();
    for (const auto &child :
         require_array(require_member(node, "children", path), path)) {
      self(self, child, groups, instance, stop_policy, index);
    }
  };
  visit(visit, tree, {}, {}, core::ProgramOperatorStopPolicy::kContinue,
        std::nullopt);
  std::set<std::uint32_t> coordinated_nodes;
  for (auto &[name, indices] : selections) {
    std::sort(indices.begin(), indices.end());
    const auto declared = group_policies.find(name);
    if (declared == group_policies.end())
      fail("execution group is used but not declared: " + name);
    const auto policy = declared->second;
    if (policy != core::ProgramExecutionPolicy::kIndependent) {
      if (indices.size() < 2U)
        fail("coordinated execution group must contain at least two nodes: " +
             name);
      std::string parent;
      for (const auto index : indices) {
        const auto &instance = table.nodes[index].instance_id.value();
        const auto shape = shapes.find(instance);
        if (shape == shapes.end() ||
            (shape->second.node_id != "motion.move_to_joint_target" &&
             shape->second.node_id != "motion.move_to_pose" &&
             shape->second.node_id != "motion.follow_cartesian_path") ||
            std::find(shape->second.direct_groups.begin(),
                      shape->second.direct_groups.end(),
                      name) == shape->second.direct_groups.end()) {
          fail("coordinated execution group contains a non-motion or "
               "inherited member: " +
               name);
        }
        if (parent.empty())
          parent = shape->second.parent_instance_id;
        else if (parent != shape->second.parent_instance_id)
          fail("coordinated execution group members are not Parallel "
               "siblings: " +
               name);
        if (!coordinated_nodes.insert(index).second)
          fail("a motion node belongs to more than one coordinated execution "
               "group");
      }
      const auto parent_shape = shapes.find(parent);
      if (parent.empty() || parent_shape == shapes.end() ||
          parent_shape->second.node_id != "parallel" ||
          !parent_shape->second.parallel_success_on_all)
        fail("coordinated execution group requires one Success-on-all "
             "Parallel: " +
             name);
    }
    table.named_selections.push_back({name, std::move(indices), policy});
  }
  for (const auto &[name, _policy] : group_policies) {
    if (!selections.contains(name))
      fail("execution group is declared but not used: " + name);
  }
  return table;
}

[[nodiscard]] core::ProgramBindingContractDTO
extract_contract(const CanonicalValue &root_value,
                 const RetainedBundle &retained_bundle,
                 const std::optional<VerifiedRetainedModelMap> *model_map,
                 const ICatalogArtifactSource *catalog_source,
                 core::ProgramExecutionTableDTO &execution_table,
                 std::vector<core::ProgramBlackboardKeyDeclarationDTO>
                     &blackboard_declarations) {
  const auto &root = require_object(root_value, "program");
  require_fields(
      root,
      {"api_version", "program_id", "program_revision", "program_profile",
       "name", "catalog", "tree"},
      {"description", "compatibility", "viewer", "scene", "parameters",
       "blackboard", "contact_policies", "execution_groups"},
      "program");
  if (require_string(require_member(root, "api_version", "program"),
                     "program.api_version") != core::kProgramApiVersion) {
    fail("program.api_version: expected manyforge.program.v3");
  }
  if (require_string(require_member(root, "program_profile", "program"),
                     "program.program_profile") !=
      core::kComposableCellProgramProfile) {
    fail("program.program_profile: expected composable-cell-v1");
  }
  const std::string &name =
      require_string(require_member(root, "name", "program"), "program.name");
  if (name.empty()) {
    fail("program.name: expected non-empty text");
  }
  if (const auto *description = find_member(root, "description");
      description != nullptr) {
    (void)require_string(*description, "program.description");
  }

  const auto &bundle = retained_bundle.value();
  const auto &catalog = require_object(
      require_member(root, "catalog", "program"), "program.catalog");
  require_exact_fields(catalog, {"api_version", "content_version"},
                       "program.catalog");
  if (require_string(require_member(catalog, "api_version", "program.catalog"),
                     "program.catalog.api_version") !=
          "manyforge.node_catalog.v3" ||
      require_string(
          require_member(catalog, "content_version", "program.catalog"),
          "program.catalog.content_version") !=
          bundle.catalogs.node_environment.content_version) {
    fail("Program catalog selector differs from the retained node environment");
  }

  verify_optional_top_level_shapes(root);
  const PoseFrames frames{declared_scene_resource_ids(root)};
  verify_blackboard_pose_keys(root, frames);
  std::size_t tree_count = 0U;
  std::set<std::string> instance_ids;
  std::vector<core::ProgramNodePolicyDTO> node_policies;
  verify_tree(require_member(root, "tree", "program"), "program.tree", 0U,
              tree_count, instance_ids, node_policies);
  const auto validation_view =
      with_node_outcome_declarations(root, retained_bundle);
  const auto &validation_root = validation_view.as_object();
  const auto expressions = verify_program_expressions(validation_root);
  verify_catalog_node_semantics(require_member(root, "tree", "program"),
                                validation_root, bundle.catalogs.node_environment,
                                expressions, frames);
  blackboard_declarations.clear();
  if (const auto *blackboard = find_member(validation_root, "blackboard")) {
    if (const auto *keys = find_member(blackboard->as_object(), "keys")) {
      for (const auto &key : keys->as_array()) {
        blackboard_declarations.push_back(
            {require_string(
                 require_member(key.as_object(), "key", "blackboard key"),
                 "key"),
             require_string(
                 require_member(key.as_object(), "type", "blackboard key"),
                 "type"),
             find_member(key.as_object(), "initial") != nullptr});
      }
    }
  }
  if (!model_map || !catalog_source)
    throw ProgramReferenceValidationIncomplete("Program verification requires exact reference-validation inputs");
  auto diagnostics = validate_program_references(root_value, false, retained_bundle,
                                                  *model_map, *catalog_source);
  if (!diagnostics.empty())
    throw ProgramReferenceValidationRefusal(std::move(diagnostics));
  const auto signal_catalog = ResolveCompiledIoSignalCatalog(bundle.cell_graph);
  if (!signal_catalog.ok())
    fail("Program execution capture cannot resolve retained I/O catalog: " +
         signal_catalog.error().message);
  CapturedDirectReferences references;
  TargetAssociations associations;
  execution_table = capture_execution_table(
      require_member(root, "tree", "program"), validation_root, instance_ids,
      bundle.catalogs.node_environment, signal_catalog.value(), references, associations,
      execution_group_policies(root, "program"));
  std::sort(node_policies.begin(), node_policies.end(),
            [](const auto &left, const auto &right) {
              return left.instance_id < right.instance_id;
            });

  const std::string &revision =
      require_string(require_member(root, "program_revision", "program"),
                     "program.program_revision");
  if (!is_sha256_digest(revision)) {
    fail("program.program_revision: expected lowercase sha256 digest");
  }
  auto contact_grasp_templates = parse_contact_grasp_templates(root);
  capture_non_node_references(root, references, associations);
  for (const auto &contact : contact_grasp_templates)
    for (const auto &selector : contact.allowed_body_selectors)
      references.bodies.insert(selector.body);
  std::map<core::ProgramTargetReferenceDTO,
           std::map<core::CapabilityId, core::ProgramCapabilityRequirementDTO>>
      target_capabilities;
  for (const auto &target : references.targets)
    target_capabilities.try_emplace(target);
  for (const auto &[target, unused] : associations) {
    (void)unused;
    target_capabilities.try_emplace(target);
  }
  for (const auto &node : execution_table.nodes) {
    for (const auto &effect : node.target_effects) {
      const auto *definition = require_capability_definition(bundle, effect.capability_id);
      target_capabilities[effect.target].try_emplace(
          effect.capability_id,
          core::ProgramCapabilityRequirementDTO{
              effect.capability_id,
              core::CapabilityDefinitionRevision(std::string(definition->capability_definition_revision)),
              core::ProgramCapabilityDisposition::kRequired});
    }
  }
  std::vector<core::ProgramTargetRequirementDTO> targets;
  for (auto &[target, capabilities] : target_capabilities) {
    std::vector<core::ProgramCapabilityRequirementDTO> rows;
    rows.reserve(capabilities.size());
    for (auto &[id, capability] : capabilities) {
      (void)id;
      rows.push_back(std::move(capability));
    }
    const auto found = associations.find(target);
    const CapturedDirectReferences empty;
    const auto &associated = found == associations.end() ? empty : found->second;
    std::vector<core::ProgramInputSignalUseDTO> input_signal_uses;
    for (const auto &[capability, use] : associated.input_signal_uses)
      input_signal_uses.push_back({capability, {use.signals.begin(), use.signals.end()},
                                   use.all_uses_are_signal_reads});
    targets.push_back({target, std::move(rows),
                       {associated.frames.begin(), associated.frames.end()},
                       {associated.bodies.begin(), associated.bodies.end()},
                       {associated.signals.begin(), associated.signals.end()},
                       std::move(input_signal_uses), associated.default_tcp_required});
  }
  return core::ProgramBindingContractDTO{
      parse_program_identity<core::ProgramId>(
          require_member(root, "program_id", "program"), "program.program_id"),
      core::ProgramRevision(revision),
      bundle.catalogs.node_environment,
      std::move(targets),
      std::vector<core::ProgramFrameReferenceDTO>(references.frames.begin(), references.frames.end()),
      std::vector<core::ProgramSignalReferenceDTO>(references.signals.begin(), references.signals.end()),
      std::vector<core::ProgramCollisionBodyReferenceDTO>(references.bodies.begin(), references.bodies.end()),
      std::move(contact_grasp_templates),
      std::move(node_policies),
  };
}

} // namespace

QualifiedProgramCatalogView qualified_program_catalog(
    const core::RetainedNodeEnvironmentDTO &environment) {
  const auto &catalog = execution_catalog();
  if (catalog.catalog_id != environment.catalog_id ||
      catalog.content_version != environment.content_version ||
      core::ContentDigest(catalog.digest) != environment.catalog_digest) {
    fail("Program execution capture requires the exact qualified retained node "
         "catalog");
  }
  return {catalog.schema_types, catalog.entries};
}

namespace detail {

class ProgramBindingContractConstructionAuthority final {
public:
  [[nodiscard]] static std::shared_ptr<const ProgramBindingContract>
  construct(core::ProgramBindingContractDTO value, CanonicalValue root,
            std::string bytes, core::ProgramExecutionTableDTO execution_table,
            std::vector<core::ProgramBlackboardKeyDeclarationDTO>
                blackboard_declarations) {
    return std::shared_ptr<const ProgramBindingContract>(
        new ProgramBindingContract(std::move(value), std::move(root),
                                   std::move(bytes), std::move(execution_table),
                                   std::move(blackboard_declarations)));
  }
};

} // namespace detail

void require_supported_program_format(const CanonicalValue &source_root) {
  const auto &source = require_object(source_root, "program");
  const auto &version = require_string(require_member(source, "api_version", "program"),
                                       "program.api_version");
  if (version != core::kProgramApiVersion) {
    throw ProgramContractError(
        "program.api_version: unsupported Program format",
        ProgramContractError::UnsupportedFormat{version, std::string(core::kProgramApiVersion)});
  }
}

CanonicalValue normalize_program_authoring(const CanonicalValue &source_root) {
  require_supported_program_format(source_root);
  const auto &source = source_root.as_object();
  (void)require_member(source, "tree", "program");
  auto members = source;
  std::size_t count = 0U;
  for (auto &[name, value] : members) {
    if (name == "tree")
      value = normalize_program_tree(value, "program.tree", 0U, count);
  }
  return CanonicalValue(std::move(members));
}

static std::shared_ptr<const ProgramBindingContract>
verify_program_binding_contract_impl(std::string_view canonical_program_bytes,
                                     const RetainedBundle &retained_bundle,
                                     const std::optional<VerifiedRetainedModelMap> *model_map,
                                     const ICatalogArtifactSource *catalog_source) {
  try {
    if (canonical_program_bytes.size() > kMaxArtifactEncodedBytes) {
      fail("canonical Program exceeds fixed artifact byte bound");
    }
    const auto parsed = parse_strict_json_model_yaml(canonical_program_bytes);
    const CanonicalValue source_root = parsed.root();
    require_supported_program_format(source_root);
    if (!verify_retained_bundle_identity(retained_bundle)) {
      fail("Program binding contract selected an invalid retained bundle");
    }
    if (canonical_json(source_root) != canonical_program_bytes) {
      fail("Program binding input is not schema-normalized RFC 8785 JSON");
    }
    CanonicalValue root = normalize_program_authoring(source_root);
    if (!detail::verify_content_identity(
            root, detail::ContentIdentitySpec{
                      .identity_domain = "program-revision",
                      .api_version = std::string(core::kProgramApiVersion),
                      .own_field = "program_revision",
                  })) {
      fail("ProgramRevision does not match the complete canonical Program");
    }
    core::ProgramExecutionTableDTO execution_table;
    std::vector<core::ProgramBlackboardKeyDeclarationDTO>
        blackboard_declarations;
    core::ProgramBindingContractDTO value = extract_contract(
        root, retained_bundle, model_map, catalog_source, execution_table, blackboard_declarations);
    const std::string normalized_bytes = canonical_json(root);
    return detail::ProgramBindingContractConstructionAuthority::construct(
        std::move(value), std::move(root), normalized_bytes,
        std::move(execution_table), std::move(blackboard_declarations));
  } catch (const ProgramReferenceValidationRefusal &) {
    throw;
  } catch (const ProgramReferenceValidationIncomplete &) {
    throw;
  } catch (const ProgramContractError &) {
    throw;
  } catch (const std::exception &error) {
    fail(std::string("Program binding-contract verification failed: ") +
         error.what());
  }
}

std::shared_ptr<const ProgramBindingContract>
verify_program_binding_contract(std::string_view canonical_program_bytes,
                                const RetainedBundle &retained_bundle) {
  return verify_program_binding_contract_impl(canonical_program_bytes, retained_bundle, nullptr, nullptr);
}

std::shared_ptr<const ProgramBindingContract>
verify_program_binding_contract(std::string_view canonical_program_bytes,
                                const RetainedBundle &retained_bundle,
                                const std::optional<VerifiedRetainedModelMap> &model_map,
                                const ICatalogArtifactSource &catalog_source) {
  return verify_program_binding_contract_impl(canonical_program_bytes, retained_bundle, &model_map, &catalog_source);
}

} // namespace manyforge::deployment
