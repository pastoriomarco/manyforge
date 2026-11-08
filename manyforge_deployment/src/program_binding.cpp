#include "program_contact_envelope_codec.hpp"
#include "manyforge_deployment/program_binding.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "content_identity_internal.hpp"
#include "manyforge_core/types/command_schema.hpp"
#include "manyforge_core/types/collision_world_snapshot.hpp"
#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/cell_kinematic_index.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "manyforge_deployment/program_reference_facts.hpp"
#include "program_binding_internal.hpp"
#include "program_reference_probe_internal.hpp"
#include "ros2_control_execution_runtime_unit_internal.hpp"
#include <manyforge_core/io_signal_descriptor.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>

namespace manyforge::deployment {
namespace {

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";
constexpr std::size_t kMaximumIdentityBytes = 128U;
constexpr std::size_t kMaximumCompiledNameBytes = 4096U;

[[noreturn]] void fail(std::string message) {
  throw ProgramBindingError(std::move(message));
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
  const auto* string = std::get_if<std::string>(&value.storage());
  if (string == nullptr) {
    fail(std::string(path) + ": expected string");
  }
  return *string;
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

void require_exact_fields(const CanonicalValue::Object& object,
                          std::initializer_list<std::string_view> expected,
                          std::string_view path) {
  if (object.size() != expected.size()) {
    fail(std::string(path) + ": unexpected field count");
  }
  for (const auto& [name, unused] : object) {
    (void)unused;
    if (std::find(expected.begin(), expected.end(), name) == expected.end()) {
      fail(std::string(path) + ": unknown field '" + name + "'");
    }
  }
  for (const std::string_view name : expected) {
    (void)require_member(object, name, path);
  }
}

void require_fields(const CanonicalValue::Object &object,
                    std::initializer_list<std::string_view> required,
                    std::initializer_list<std::string_view> optional,
                    std::string_view path) {
  for (const auto name : required)
    (void)require_member(object, name, path);
  for (const auto &[name, unused] : object) {
    (void)unused;
    if (std::find(required.begin(), required.end(), name) == required.end() &&
        std::find(optional.begin(), optional.end(), name) == optional.end())
      fail(std::string(path) + ": unknown field '" + name + "'");
  }
}

const CanonicalValue *find_member(const CanonicalValue::Object &object,
                                  std::string_view name) {
  const auto found =
      std::find_if(object.begin(), object.end(),
                   [&](const auto &row) { return row.first == name; });
  return found == object.end() ? nullptr : &found->second;
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

void require_digest(std::string_view value, std::string_view path) {
  if (!is_sha256_digest(value)) {
    fail(std::string(path) + ": expected lowercase sha256 digest");
  }
}

void require_text(std::string_view value, std::string_view path,
                  std::size_t maximum = kMaximumCompiledNameBytes) {
  if (value.empty() || value.size() > maximum) {
    fail(std::string(path) + ": expected bounded non-empty text");
  }
}

[[nodiscard]] bool is_default_node_policy(
    const core::ProgramNodePolicyDTO& policy) {
  return policy.recovery_policy == core::ProgramNodeRecoveryPolicyDTO{} &&
         policy.operator_stop_policy ==
             core::ProgramOperatorStopPolicy::kInherit;
}

void validate_node_policy(const core::ProgramNodePolicyDTO& policy,
                          std::string_view path) {
  require_identity(policy.instance_id.value(),
                   child_path(path, "instance_id"));
  if (policy.instance_id.value().front() < 'a' ||
      policy.instance_id.value().front() > 'z' ||
      !std::all_of(policy.instance_id.value().begin(),
                   policy.instance_id.value().end(), [](unsigned char byte) {
                     return (byte >= 'a' && byte <= 'z') ||
                            (byte >= '0' && byte <= '9') || byte == '.' ||
                            byte == '_' || byte == '-';
                   })) {
    fail(std::string(path) + ".instance_id: expected lower-case Program ID");
  }
  if (policy.recovery_policy.max_retries > core::kMaxProgramRecoveryRetries) {
    fail(std::string(path) + ": recovery retry budget is out of range");
  }
  if (core::to_string(policy.recovery_policy.intervention_stop_scope).empty()) {
    fail(std::string(path) + ": invalid intervention Stop scope");
  }
  if (policy.operator_stop_policy != core::ProgramOperatorStopPolicy::kInherit &&
      policy.operator_stop_policy != core::ProgramOperatorStopPolicy::kHold &&
      policy.operator_stop_policy != core::ProgramOperatorStopPolicy::kContinue) {
    fail(std::string(path) + ": invalid operator Stop policy");
  }
  if (is_default_node_policy(policy)) {
    fail(std::string(path) + ": default node-policy rows must be omitted");
  }
}

[[nodiscard]] core::ProgramOperatorStopPolicy parse_operator_stop_policy(
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

[[nodiscard]] std::uint8_t parse_policy_retries(const CanonicalValue& value,
                                                std::string_view path) {
  const auto* number = std::get_if<double>(&value.storage());
  if (number == nullptr || !std::isfinite(*number) || *number < 0.0 ||
      *number > static_cast<double>(core::kMaxProgramRecoveryRetries) ||
      std::trunc(*number) != *number) {
    fail(std::string(path) + ": expected integer in 0..255 (0 is Unlimited)");
  }
  return static_cast<std::uint8_t>(*number);
}

[[nodiscard]] core::ProgramNodePolicyDTO parse_program_node_policy(
    const CanonicalValue& value, std::string_view path) {
  const auto& root = require_object(value, path);
  require_exact_fields(
      root, {"instance_id", "recovery_policy", "operator_stop_policy"}, path);
  const auto recovery_path = child_path(path, "recovery_policy");
  const auto& recovery = require_object(
      require_member(root, "recovery_policy", path), recovery_path);
  const auto scope =
      std::find_if(recovery.begin(), recovery.end(), [](const auto &field) {
        return field.first == "intervention_stop_scope";
      });
  auto stop_scope = core::ProgramInterventionStopScope::kAll;
  if (scope == recovery.end()) {
    require_exact_fields(recovery, {"allow_intervention", "max_retries"},
                         recovery_path);
  } else {
    require_exact_fields(
        recovery,
        {"allow_intervention", "max_retries", "intervention_stop_scope"},
        recovery_path);
    const auto scope_path =
        child_path(recovery_path, "intervention_stop_scope");
    const auto &spelling = require_string(scope->second, scope_path);
    if (spelling == "own_groups") {
      stop_scope = core::ProgramInterventionStopScope::kOwnGroups;
    } else if (spelling != "all") {
      fail(scope_path + ": expected all or own_groups");
    }
  }
  const auto& allow =
      require_member(recovery, "allow_intervention", recovery_path);
  if (!allow.is_bool()) {
    fail(std::string(path) +
         ".recovery_policy.allow_intervention: expected boolean");
  }
  const auto result = core::ProgramNodePolicyDTO{
      core::ProgramNodeInstanceId(require_string(
          require_member(root, "instance_id", path),
          child_path(path, "instance_id"))),
      core::ProgramNodeRecoveryPolicyDTO{
          allow.as_bool(),
          parse_policy_retries(
              require_member(recovery, "max_retries", recovery_path),
              child_path(recovery_path, "max_retries")),
          stop_scope},
      parse_operator_stop_policy(
          require_member(root, "operator_stop_policy", path),
          child_path(path, "operator_stop_policy"))};
  validate_node_policy(result, path);
  return result;
}

template <typename StrongType>
[[nodiscard]] StrongType parse_identity(const CanonicalValue& value,
                                        std::string_view path) {
  const std::string& spelling = require_string(value, path);
  require_identity(spelling, path);
  return StrongType(spelling);
}

template <typename StrongType>
[[nodiscard]] StrongType parse_digest(const CanonicalValue& value,
                                      std::string_view path) {
  const std::string& spelling = require_string(value, path);
  require_digest(spelling, path);
  return StrongType(spelling);
}

void require_signal_name(std::string_view name, std::string_view path) {
  if (name.empty() || name.size() > 512U || name.find('\0') != std::string_view::npos)
    fail(std::string(path) + ": expected a non-empty signal name of at most 512 bytes without NUL");
}

[[nodiscard]] core::ProgramTargetReferenceDTO parse_target_reference(
    const CanonicalValue& value, std::string_view path) {
  const auto& row = require_object(value, path);
  require_exact_fields(row, {"component_id", "command_target_id"}, path);
  return {parse_identity<core::ControllableComponentId>(
              require_member(row, "component_id", path), child_path(path, "component_id")),
          parse_identity<core::CommandTargetId>(
              require_member(row, "command_target_id", path), child_path(path, "command_target_id"))};
}

[[nodiscard]] core::ProgramFrameReferenceDTO parse_frame_reference(
    const CanonicalValue& value, std::string_view path) {
  const auto& row = require_object(value, path);
  require_exact_fields(row, {"component_id", "frame_id"}, path);
  return {parse_identity<core::ControllableComponentId>(
              require_member(row, "component_id", path), child_path(path, "component_id")),
          parse_identity<core::FrameRoleId>(
              require_member(row, "frame_id", path), child_path(path, "frame_id"))};
}

[[nodiscard]] core::ProgramCollisionBodyReferenceDTO parse_body_reference(
    const CanonicalValue& value, std::string_view path) {
  const auto& row = require_object(value, path);
  require_exact_fields(row, {"component_id", "collision_body_id"}, path);
  return {parse_identity<core::ControllableComponentId>(
              require_member(row, "component_id", path), child_path(path, "component_id")),
          parse_identity<core::CollisionBodyRoleId>(
              require_member(row, "collision_body_id", path), child_path(path, "collision_body_id"))};
}

[[nodiscard]] core::ProgramSignalReferenceDTO parse_signal_reference(
    const CanonicalValue& value, std::string_view path) {
  const auto& row = require_object(value, path);
  require_exact_fields(row, {"component_id", "signal_name"}, path);
  auto name = require_string(require_member(row, "signal_name", path), child_path(path, "signal_name"));
  require_signal_name(name, child_path(path, "signal_name"));
  return {parse_identity<core::ControllableComponentId>(
              require_member(row, "component_id", path), child_path(path, "component_id")),
          std::move(name)};
}

[[nodiscard]] core::TcpEntryReferenceDTO parse_tcp_reference(
    const CanonicalValue& value, std::string_view path) {
  const auto& row = require_object(value, path);
  require_exact_fields(row, {"target", "frame"}, path);
  return {parse_target_reference(require_member(row, "target", path),
                                 child_path(path, "target")),
          parse_frame_reference(require_member(row, "frame", path),
                                child_path(path, "frame"))};
}

void validate_target_reference(const core::ProgramTargetReferenceDTO& ref,
                               std::string_view path) {
  require_identity(ref.component_id.value(), child_path(path, "component_id"));
  require_identity(ref.command_target_id.value(), child_path(path, "command_target_id"));
}

void validate_frame_reference(const core::ProgramFrameReferenceDTO& ref,
                              std::string_view path) {
  require_identity(ref.component_id.value(), child_path(path, "component_id"));
  require_identity(ref.frame_id.value(), child_path(path, "frame_id"));
}

void validate_body_reference(const core::ProgramCollisionBodyReferenceDTO& ref,
                             std::string_view path) {
  require_identity(ref.component_id.value(), child_path(path, "component_id"));
  require_identity(ref.collision_body_id.value(), child_path(path, "collision_body_id"));
}

void validate_signal_reference(const core::ProgramSignalReferenceDTO& ref,
                               std::string_view path) {
  require_identity(ref.component_id.value(), child_path(path, "component_id"));
  require_signal_name(ref.signal_name, child_path(path, "signal_name"));
}

[[nodiscard]] CanonicalValue target_reference_projection(
    const core::ProgramTargetReferenceDTO& ref) {
  return CanonicalValue(CanonicalValue::Object{
      {"component_id", CanonicalValue(ref.component_id.value())},
      {"command_target_id", CanonicalValue(ref.command_target_id.value())}});
}

[[nodiscard]] CanonicalValue frame_reference_projection(
    const core::ProgramFrameReferenceDTO& ref) {
  return CanonicalValue(CanonicalValue::Object{
      {"component_id", CanonicalValue(ref.component_id.value())},
      {"frame_id", CanonicalValue(ref.frame_id.value())}});
}

[[nodiscard]] CanonicalValue body_reference_projection(
    const core::ProgramCollisionBodyReferenceDTO& ref) {
  return CanonicalValue(CanonicalValue::Object{
      {"component_id", CanonicalValue(ref.component_id.value())},
      {"collision_body_id", CanonicalValue(ref.collision_body_id.value())}});
}

[[nodiscard]] CanonicalValue signal_reference_projection(
    const core::ProgramSignalReferenceDTO& ref) {
  return CanonicalValue(CanonicalValue::Object{
      {"component_id", CanonicalValue(ref.component_id.value())},
      {"signal_name", CanonicalValue(ref.signal_name)}});
}

[[nodiscard]] CanonicalValue tcp_reference_projection(
    const core::TcpEntryReferenceDTO& ref) {
  return CanonicalValue(CanonicalValue::Object{
      {"target", target_reference_projection(ref.target)},
      {"frame", frame_reference_projection(ref.frame)}});
}

template <typename Value, typename Key>
void sort_unique(std::vector<Value>& values, Key key, std::string_view path) {
  std::sort(values.begin(), values.end(), [&](const Value& left,
                                               const Value& right) {
    return key(left) < key(right);
  });
  if (std::adjacent_find(values.begin(), values.end(),
                         [&](const Value& left, const Value& right) {
                           return key(left) == key(right);
                         }) != values.end()) {
    fail(std::string(path) + ": duplicate canonical key");
  }
}

[[nodiscard]] auto source_record_key(
    const core::RetainedSourceRecordDTO& record) {
  return std::visit(
      [](const auto& typed) {
        using Record = std::remove_cvref_t<decltype(typed)>;
        return std::tuple(std::string(core::to_string(Record::kRecordKind)),
                          typed.record_id.value(), typed.revision.value());
      },
      record);
}

void validate_source_record(const core::RetainedSourceRecordDTO& record,
                            std::string_view path) {
  std::visit(
      [path](const auto& typed) {
        require_identity(typed.record_id.value(),
                         child_path(path, "record_id"));
        require_digest(typed.revision.value(), child_path(path, "revision"));
      },
      record);
}

void normalize_and_validate(core::ProgramBindingDTO& value) {
  if (value.api_version != core::kProgramBindingApiVersion) {
    fail("program_binding.api_version: expected manyforge.program_binding.v3");
  }
  require_digest(value.program_binding_id.value(),
                 "program_binding.program_binding_id");
  require_identity(value.program_id.value(), "program_binding.program_id");
  require_digest(value.program_revision.value(),
                 "program_binding.program_revision");
  require_digest(value.retained_bundle_id.value(),
                 "program_binding.retained_bundle_id");
  require_digest(value.graph_revision.value(),
                 "program_binding.graph_revision");
  require_identity(value.node_environment.catalog_id,
                   "program_binding.node_environment.catalog_id");
  require_text(value.node_environment.content_version,
               "program_binding.node_environment.content_version",
               kMaximumIdentityBytes);
  require_digest(value.node_environment.catalog_digest.value(),
                 "program_binding.node_environment.catalog_digest");
  require_digest(value.node_environment.environment_digest.value(),
                 "program_binding.node_environment.environment_digest");

  if (value.component_bindings.size() > core::kMaxProgramRoleBindings ||
      value.target_bindings.size() > core::kMaxProgramRoleBindings) {
    fail("program binding role tables violate fixed bounds");
  }

  for (auto& component : value.component_bindings) {
    require_identity(component.component_id.value(), "component_bindings.component_id");
    require_identity(component.component_type_id.value(),
                     "component_bindings.component_type_id");
    require_digest(component.component_type_revision.value(),
                   "component_bindings.component_type_revision");
    if (component.model_bindings.size() >
        core::kMaxProgramNestedRoleBindings) {
      fail("component model-role binding table exceeds fixed bound");
    }
    for (const auto& model : component.model_bindings) {
      require_identity(model.model_role_id.value(),
                       "model_bindings.model_role_id");
      require_identity(model.model_instance_id.value(),
                       "model_bindings.model_instance_id");
      require_digest(model.model_revision.value(),
                     "model_bindings.model_revision");
    }
    sort_unique(component.model_bindings,
                [](const auto& item) { return item.model_role_id; },
                "model_bindings");
  }
  sort_unique(value.component_bindings,
              [](const auto& item) { return item.component_id; },
              "component_bindings");

  for (auto& target : value.target_bindings) {
    validate_target_reference(target.target, "target_bindings.target");
    if (target.default_tcp) {
      validate_target_reference(target.default_tcp->target, "default_tcp.target");
      validate_frame_reference(target.default_tcp->frame, "default_tcp.frame");
    }
    if (target.frame_references.size() > core::kMaxProgramNestedRoleBindings ||
        target.collision_body_references.size() > core::kMaxProgramNestedRoleBindings ||
        target.signal_references.size() > core::kMaxProgramNestedRoleBindings ||
        target.resource_role_bindings.size() > core::kMaxProgramNestedRoleBindings ||
        target.capability_bindings.size() > core::kMaxProgramCapabilityBindings ||
        (target.robot_tcps && target.robot_tcps->size() > core::kMaxProgramNestedRoleBindings))
      fail("target nested binding table exceeds fixed bound");
    for (const auto& ref : target.frame_references) validate_frame_reference(ref, "frame_references");
    for (const auto& ref : target.collision_body_references) validate_body_reference(ref, "collision_body_references");
    for (const auto& ref : target.signal_references) validate_signal_reference(ref, "signal_references");
    sort_unique(target.frame_references, [](const auto& ref) { return ref; }, "frame_references");
    sort_unique(target.collision_body_references, [](const auto& ref) { return ref; }, "collision_body_references");
    sort_unique(target.signal_references, [](const auto& ref) { return ref; }, "signal_references");
    if (target.robot_tcps) {
      for (auto& tcp : *target.robot_tcps) {
        validate_target_reference(tcp.entry.target, "robot_tcps.entry.target");
        validate_frame_reference(tcp.entry.frame, "robot_tcps.entry.frame");
        validate_frame_reference(tcp.frame.reference, "robot_tcps.frame.reference");
        require_identity(tcp.frame.model_instance_id.value(), "robot_tcps.frame.model_instance_id");
        require_digest(tcp.frame.model_revision.value(), "robot_tcps.frame.model_revision");
        require_text(tcp.frame.compiled_frame_name, "robot_tcps.frame.compiled_frame_name");
        if (tcp.entry.frame != tcp.frame.reference || tcp.contact_body_role_ids.size() > core::kMaxProgramNestedRoleBindings)
          fail("robot TCP entry/frame correspondence is malformed");
        for (const auto& role : tcp.contact_body_role_ids) require_identity(role.value(), "robot_tcps.contact_body_role_ids");
        sort_unique(tcp.contact_body_role_ids, [](const auto& role) { return role; }, "robot_tcps.contact_body_role_ids");
      }
      sort_unique(*target.robot_tcps, [](const auto& row) { return std::tie(row.entry.target, row.entry.frame); }, "robot_tcps");
    }
    if (target.pneumatic_io) {
      auto& route = *target.pneumatic_io;
      if (route.outputs.empty() || route.outputs.size() > 2U || route.inputs.size() > 4U)
        fail("pneumatic I/O binding has invalid row counts");
      for (const auto& row : route.outputs) validate_signal_reference(row.signal, "pneumatic output signal");
      sort_unique(route.outputs, [](const auto& row) { return row.signal; }, "pneumatic output bindings");
      for (const auto& row : route.inputs) {
        if (row.input != "open" && row.input != "closed" && row.input != "pressure" && row.input != "workpiece_grasped")
          fail("pneumatic input binding has an unknown input");
        validate_signal_reference(row.signal, "pneumatic input signal");
      }
      sort_unique(route.inputs, [](const auto& row) { return row.input; }, "pneumatic input bindings");
    }

    for (const auto& resource : target.resource_role_bindings) {
      require_identity(resource.resource_role_id.value(),
                       "resource_role_bindings.resource_role_id");
      if (resource.resource_ids.empty() ||
          resource.resource_ids.size() !=
              resource.compiled_resource_names.size() ||
          resource.resource_ids.size() >
              core::kMaxProgramNestedRoleBindings) {
        fail("resource role binding correspondence is empty or malformed");
      }
      std::set<core::ResourceId> resource_ids;
      std::set<std::string> compiled_names;
      for (std::size_t index = 0U; index < resource.resource_ids.size();
           ++index) {
        require_identity(resource.resource_ids[index].value(),
                         "resource_role_bindings.resource_ids");
        require_text(resource.compiled_resource_names[index],
                     "resource_role_bindings.compiled_resource_names");
        if (!resource_ids.insert(resource.resource_ids[index]).second ||
            !compiled_names.insert(resource.compiled_resource_names[index])
                 .second) {
          fail("resource role binding contains duplicate correspondence");
        }
      }
    }
    sort_unique(target.resource_role_bindings,
                [](const auto& item) { return item.resource_role_id; },
                "resource_role_bindings");

    for (const auto& capability : target.capability_bindings) {
      require_identity(capability.capability_id.value(),
                       "capability_bindings.capability_id");
      require_digest(capability.capability_definition_revision.value(),
                     "capability_bindings.capability_definition_revision");
      if (!capability.control_binding.has_value() &&
          !capability.capability_provider_binding.has_value()) {
        fail("capability binding has no control or provider resolution");
      }
      if (capability.control_binding.has_value()) {
        require_identity(
            capability.control_binding->control_binding_id.value(),
            "capability_bindings.control_binding.control_binding_id");
        require_digest(
            capability.control_binding->control_binding_revision.value(),
            "capability_bindings.control_binding.control_binding_revision");
      }
      if (capability.capability_provider_binding.has_value()) {
        require_identity(
            capability.capability_provider_binding
                ->capability_provider_binding_id.value(),
            "capability_bindings.capability_provider_binding.id");
        require_digest(
            capability.capability_provider_binding
                ->capability_provider_binding_revision.value(),
            "capability_bindings.capability_provider_binding.revision");
      }
      if (capability.command_schema.has_value()) {
        require_identity(capability.command_schema->command_schema_id.value(),
                         "capability_bindings.command_schema.id");
        require_digest(
            capability.command_schema->command_schema_revision.value(),
            "capability_bindings.command_schema.revision");
      }
    }
    sort_unique(target.capability_bindings,
                [](const auto& item) { return item.capability_id; },
                "capability_bindings");
  }
  sort_unique(value.target_bindings,
              [](const auto& item) { return item.target; },
              "target_bindings");

  if (value.frame_bindings.size() > core::kMaxProgramRoleBindings ||
      value.collision_body_bindings.size() > core::kMaxProgramRoleBindings ||
      value.signal_bindings.size() > core::kMaxProgramRoleBindings)
    fail("program binding reference tables exceed fixed bounds");
  for (auto& frame : value.frame_bindings) {
    validate_frame_reference(frame.reference, "frame_bindings.reference");
    require_identity(frame.model_instance_id.value(), "frame_bindings.model_instance_id");
    require_digest(frame.model_revision.value(), "frame_bindings.model_revision");
    require_text(frame.compiled_frame_name, "frame_bindings.compiled_frame_name");
  }
  sort_unique(value.frame_bindings, [](const auto& row) { return row.reference; }, "frame_bindings");
  for (const auto& body : value.collision_body_bindings) {
    validate_body_reference(body.reference, "collision_body_bindings.reference");
    require_identity(body.model_instance_id.value(), "collision_body_bindings.model_instance_id");
    require_digest(body.model_revision.value(), "collision_body_bindings.model_revision");
    if (body.compiled_collision_body_names.empty() || body.compiled_collision_body_names.size() > core::kMaxProgramNestedRoleBindings) fail("collision-body correspondence is empty or too large");
    std::set<std::string> names;
    for (const auto& name : body.compiled_collision_body_names) {
      require_text(name, "collision_body_bindings.compiled_collision_body_names");
      if (!names.insert(name).second) fail("collision-body binding contains duplicate names");
    }
  }
  sort_unique(value.collision_body_bindings, [](const auto& row) { return row.reference; }, "collision_body_bindings");
  for (const auto& signal : value.signal_bindings) {
    validate_signal_reference(signal.reference, "signal_bindings.reference");
    validate_target_reference(signal.target, "signal_bindings.target");
    require_identity(signal.runtime_unit_id.value(), "signal_bindings.runtime_unit_id");
    require_digest(signal.signal_descriptor_digest.value(), "signal_bindings.signal_descriptor_digest");
    if (signal.reference.component_id != signal.target.component_id ||
        signal.value_type != core::IoSignalValueType::kBoolean ||
        (signal.direction != core::IoSignalDirection::kInput && signal.direction != core::IoSignalDirection::kOutput) ||
        (signal.transition_capability != core::IoTransitionCapability::kSampledOnly && signal.transition_capability != core::IoTransitionCapability::kCaptured))
      fail("signal binding has an unsupported value contract");
  }
  sort_unique(value.signal_bindings, [](const auto& row) { return row.reference; }, "signal_bindings");
  for (auto& target : value.target_bindings) {
    const auto component = std::lower_bound(value.component_bindings.begin(), value.component_bindings.end(), target.target.component_id,
        [](const auto& row, const auto& key) { return row.component_id < key; });
    if (component == value.component_bindings.end() || component->component_id != target.target.component_id)
      fail("target has no exact component binding");
    for (const auto& ref : target.frame_references) {
      const auto row = std::lower_bound(value.frame_bindings.begin(), value.frame_bindings.end(), ref, [](const auto& item, const auto& key) { return item.reference < key; });
      if (row == value.frame_bindings.end() || row->reference != ref) fail("target frame reference is absent");
    }
    if (target.frame_associations.size() != target.frame_references.size())
      fail("target frame associations do not cover the exact frame references");
    for (auto& association : target.frame_associations) {
      validate_frame_reference(association.frame, "frame_associations.frame");
      if (association.contact_body_role_ids.size() > core::kMaxProgramNestedRoleBindings)
        fail("frame contact-body role binding exceeds fixed bound");
      for (const auto& role : association.contact_body_role_ids)
        require_identity(role.value(), "frame_associations.contact_body_role_ids");
      sort_unique(association.contact_body_role_ids, [](const auto& role) { return role; },
                  "frame_associations.contact_body_role_ids");
    }
    sort_unique(target.frame_associations, [](const auto& association) { return association.frame; },
                "frame_associations");
    for (std::size_t index = 0; index < target.frame_references.size(); ++index) {
      const auto& association = target.frame_associations[index];
      if (association.frame != target.frame_references[index])
        fail("target frame association selects another frame");
      const auto frame = std::lower_bound(value.frame_bindings.begin(), value.frame_bindings.end(), association.frame,
          [](const auto& row, const auto& key) { return row.reference < key; });
      for (const auto& role : association.contact_body_role_ids) {
        const core::ProgramCollisionBodyReferenceDTO ref{association.frame.component_id, role};
        const auto body = std::lower_bound(value.collision_body_bindings.begin(), value.collision_body_bindings.end(), ref,
            [](const auto& row, const auto& key) { return row.reference < key; });
        if (body == value.collision_body_bindings.end() || body->reference != ref ||
            body->model_instance_id != frame->model_instance_id || body->model_revision != frame->model_revision)
          fail("target frame contact body does not resolve to the same retained model");
      }
    }
    for (const auto& ref : target.collision_body_references) {
      const auto row = std::lower_bound(value.collision_body_bindings.begin(), value.collision_body_bindings.end(), ref, [](const auto& item, const auto& key) { return item.reference < key; });
      if (row == value.collision_body_bindings.end() || row->reference != ref) fail("target body reference is absent");
    }
    for (const auto& ref : target.signal_references) {
      const auto row = std::lower_bound(value.signal_bindings.begin(), value.signal_bindings.end(), ref, [](const auto& item, const auto& key) { return item.reference < key; });
      if (row == value.signal_bindings.end() || row->reference != ref || row->target != target.target) fail("target signal reference is absent or selects another target");
    }
    if (target.pneumatic_io) {
      if (std::none_of(target.capability_bindings.begin(), target.capability_bindings.end(), [](const auto& row) { return row.capability_id.value() == "tool.gripper.command"; })) fail("pneumatic I/O mapping requires a gripper command target");
      const auto check = [&](const auto& route, core::IoSignalDirection direction) {
        const auto signal = std::lower_bound(value.signal_bindings.begin(), value.signal_bindings.end(), route.signal, [](const auto& row, const auto& key) { return row.reference < key; });
        if (signal == value.signal_bindings.end() || signal->reference != route.signal || signal->direction != direction || signal->value_type != core::IoSignalValueType::kBoolean) fail("pneumatic signal is absent or has wrong direction");
        const auto selected = std::lower_bound(value.target_bindings.begin(), value.target_bindings.end(), signal->target, [](const auto& row, const auto& key) { return row.target < key; });
        if (selected == value.target_bindings.end() || selected->target != signal->target || !std::binary_search(selected->signal_references.begin(), selected->signal_references.end(), route.signal)) fail("pneumatic signal target is absent");
        const auto capability = direction == core::IoSignalDirection::kOutput ? "io.gpio.set_output" : "io.gpio.get_input";
        if (std::none_of(selected->capability_bindings.begin(), selected->capability_bindings.end(), [&](const auto& row) { return row.capability_id.value() == capability; })) fail("pneumatic target has no required capability");
      };
      for (const auto& row : target.pneumatic_io->outputs) check(row, core::IoSignalDirection::kOutput);
      for (const auto& row : target.pneumatic_io->inputs) check(row, core::IoSignalDirection::kInput);
    }
  }

  if (value.node_policies.size() > core::kMaxProgramNodePolicies) {
    fail("program binding node-policy table exceeds Program tree-node bound");
  }
  for (const auto& policy : value.node_policies) {
    validate_node_policy(policy, "program_binding.node_policies");
  }
  sort_unique(value.node_policies,
              [](const auto& policy) { return policy.instance_id; },
              "program_binding.node_policies");

  if (value.contact_grasp_bindings.size() >
      core::kMaxProgramRoleBindings) {
    fail("contact-grasp binding table exceeds fixed bound");
  }
  for (auto& policy : value.contact_grasp_bindings) {
    require_identity(policy.contact_policy_id.value(),
                     "contact_grasp_bindings.contact_policy_id");
    require_digest(policy.contact_policy_revision.value(),
                   "contact_grasp_bindings.contact_policy_revision");
    require_identity(policy.runtime_object_id.value(),
                     "contact_grasp_bindings.runtime_object_id");
    if ((policy.bodies_from_motion_endpoint != policy.allowed_body_bindings.empty()) ||
        policy.allowed_body_bindings.size() >
            core::kMaxProgramNestedRoleBindings ||
        !core::valid_program_contact_envelope(policy.envelope)) {
      fail("contact-grasp binding envelope is empty, oversized, or invalid");
    }
    for (const auto& body : policy.allowed_body_bindings) {
      validate_body_reference(body.body, "contact_grasp_body_bindings.body");
      if (body.model_role_id.has_value()) {
        require_identity(body.model_role_id->value(),
                         "contact_grasp_body_bindings.model_role_id");
      }
      require_identity(body.model_instance_id.value(),
                       "contact_grasp_body_bindings.model_instance_id");
      require_digest(body.model_revision.value(),
                     "contact_grasp_body_bindings.model_revision");
      if (body.collision_body_ids.empty() ||
          body.collision_body_ids.size() >
              core::kMaxProgramNestedRoleBindings) {
        fail("contact-grasp body binding has an invalid body-id count");
      }
      for (std::size_t index = 0U; index < body.collision_body_ids.size();
           ++index) {
        require_text(body.collision_body_ids[index],
                     "contact_grasp_body_bindings.collision_body_ids");
        if (body.collision_body_ids[index] !=
            core::make_component_collision_body_id(
                body.body.component_id, body.model_instance_id,
                body.body.collision_body_id, index)) {
          fail("contact-grasp body id differs from its exact resolved scope");
        }
      }

      const auto collision = std::lower_bound(value.collision_body_bindings.begin(), value.collision_body_bindings.end(), body.body,
          [](const auto& row, const auto& key) { return row.reference < key; });
      if (collision == value.collision_body_bindings.end() || collision->reference != body.body || collision->model_instance_id != body.model_instance_id || collision->model_revision != body.model_revision || collision->compiled_collision_body_names.size() != body.collision_body_ids.size())
        fail("contact-grasp body differs from its collision-body binding");
      if (body.model_role_id) {
        const auto component = std::lower_bound(value.component_bindings.begin(), value.component_bindings.end(), body.body.component_id,
            [](const auto& row, const auto& key) { return row.component_id < key; });
        if (component == value.component_bindings.end() || component->component_id != body.body.component_id ||
            std::count_if(component->model_bindings.begin(), component->model_bindings.end(), [&](const auto& model) { return model.model_role_id == *body.model_role_id && model.model_instance_id == body.model_instance_id && model.model_revision == body.model_revision; }) != 1)
          fail("contact-grasp model role differs from the component binding");
      }
    }
    sort_unique(
        policy.allowed_body_bindings,
        [](const auto& body) {
          return std::tie(body.body, body.model_role_id);
        },
        "contact_grasp_bindings.allowed_body_bindings");
  }
  sort_unique(value.contact_grasp_bindings,
              [](const auto& policy) { return policy.contact_policy_id; },
              "contact_grasp_bindings");

  require_digest(
      value.provenance.binding_compiler_contract_digest.value(),
      "program_binding.provenance.binding_compiler_contract_digest");
  if (value.provenance.source_records.size() >
          core::kMaxProgramBindingProvenanceRecords ||
      value.provenance.resolved_output_records.size() >
          core::kMaxProgramBindingProvenanceRecords) {
    fail("program binding provenance exceeds fixed record bounds");
  }
  for (const auto& record : value.provenance.source_records) {
    validate_source_record(record, "program_binding.provenance.source_records");
  }
  for (const auto& record : value.provenance.resolved_output_records) {
    validate_source_record(
        record, "program_binding.provenance.resolved_output_records");
  }
  sort_unique(value.provenance.source_records, source_record_key,
              "program_binding.provenance.source_records");
  sort_unique(value.provenance.resolved_output_records, source_record_key,
              "program_binding.provenance.resolved_output_records");
}

[[nodiscard]] CanonicalValue string_array(
    std::span<const std::string> values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto& value : values) {
    result.emplace_back(value);
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue node_policy_projection(
    const core::ProgramNodePolicyDTO& policy) {
  CanonicalValue::Object recovery{
      {"allow_intervention",
       CanonicalValue(policy.recovery_policy.allow_intervention)},
      {"max_retries",
       CanonicalValue(static_cast<double>(policy.recovery_policy.max_retries))},
  };
  if (policy.recovery_policy.intervention_stop_scope !=
      core::ProgramInterventionStopScope::kAll) {
    recovery.emplace_back(
        "intervention_stop_scope",
        CanonicalValue(std::string(
            core::to_string(policy.recovery_policy.intervention_stop_scope))));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"instance_id", CanonicalValue(policy.instance_id.value())},
      {"operator_stop_policy", CanonicalValue(std::string(core::to_string(
                                   policy.operator_stop_policy)))},
      {"recovery_policy", CanonicalValue(std::move(recovery))},
  });
}

template <typename StrongType>
[[nodiscard]] CanonicalValue strong_array(
    std::span<const StrongType> values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto& value : values) {
    result.emplace_back(value.value());
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue source_records_projection(
    std::span<const core::RetainedSourceRecordDTO> records) {
  CanonicalValue::Array result;
  result.reserve(records.size());
  for (const auto& record : records) {
    result.push_back(std::visit(
        [](const auto& typed) {
          using Record = std::remove_cvref_t<decltype(typed)>;
          return CanonicalValue(CanonicalValue::Object{
              {"record_kind",
               CanonicalValue(
                   std::string(core::to_string(Record::kRecordKind)))},
              {"record_id", CanonicalValue(typed.record_id.value())},
              {"revision", CanonicalValue(typed.revision.value())},
          });
        },
        record));
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue optional_control_binding_projection(
    const std::optional<core::ProgramControlBindingRefDTO>& value) {
  if (!value.has_value()) {
    return CanonicalValue(nullptr);
  }
  return CanonicalValue(CanonicalValue::Object{
      {"control_binding_id",
       CanonicalValue(value->control_binding_id.value())},
      {"control_binding_revision",
       CanonicalValue(value->control_binding_revision.value())},
  });
}

[[nodiscard]] CanonicalValue optional_provider_binding_projection(
    const std::optional<core::ProgramCapabilityProviderBindingRefDTO>& value) {
  if (!value.has_value()) {
    return CanonicalValue(nullptr);
  }
  return CanonicalValue(CanonicalValue::Object{
      {"capability_provider_binding_id",
       CanonicalValue(value->capability_provider_binding_id.value())},
      {"capability_provider_binding_revision",
       CanonicalValue(value->capability_provider_binding_revision.value())},
  });
}

[[nodiscard]] CanonicalValue optional_command_schema_projection(
    const std::optional<core::ProgramCommandSchemaRefDTO>& value) {
  if (!value.has_value()) {
    return CanonicalValue(nullptr);
  }
  return CanonicalValue(CanonicalValue::Object{
      {"command_schema_id", CanonicalValue(value->command_schema_id.value())},
      {"command_schema_revision",
       CanonicalValue(value->command_schema_revision.value())},
  });
}

[[nodiscard]] core::RetainedSourceRecordDTO parse_source_record(
    const CanonicalValue& value, std::string_view path) {
  const auto& object = require_object(value, path);
  require_exact_fields(object, {"record_kind", "record_id", "revision"},
                       path);
  const std::string& kind = require_string(
      require_member(object, "record_kind", path),
      child_path(path, "record_kind"));
  const std::string& id = require_string(
      require_member(object, "record_id", path),
      child_path(path, "record_id"));
  const std::string& revision = require_string(
      require_member(object, "revision", path), child_path(path, "revision"));
  require_identity(id, child_path(path, "record_id"));
  require_digest(revision, child_path(path, "revision"));

#define MANYFORGE_PARSE_SOURCE_RECORD(enum_name, spelling, id_type, revision_type) \
  if (kind == spelling) {                                                     \
    return core::TypedRetainedSourceRecordDTO<                                \
        core::CatalogRecordKind::enum_name, core::id_type,                    \
        core::revision_type>{core::id_type(id), core::revision_type(revision)}; \
  }
  MANYFORGE_PARSE_SOURCE_RECORD(kModelAsset, "model_asset", ModelAssetId,
                                ModelRevision)
  MANYFORGE_PARSE_SOURCE_RECORD(kRuntimeProfile, "runtime_profile",
                                RuntimeProfileId, RuntimeProfileRevision)
  MANYFORGE_PARSE_SOURCE_RECORD(kComponentType, "component_type",
                                ComponentTypeId, ComponentTypeRevision)
  MANYFORGE_PARSE_SOURCE_RECORD(kControlInterfaceContract,
                                "control_interface_contract",
                                ControlInterfaceContractId,
                                ControlInterfaceRevision)
  MANYFORGE_PARSE_SOURCE_RECORD(kControllerTopology, "controller_topology",
                                ControllerTopologyId, TopologyRevision)
  MANYFORGE_PARSE_SOURCE_RECORD(kProviderManifest, "provider_manifest",
                                ProviderManifestId, ProviderManifestRevision)
  MANYFORGE_PARSE_SOURCE_RECORD(kCommandSchema, "command_schema",
                                CommandSchemaId, CommandSchemaRevision)
  MANYFORGE_PARSE_SOURCE_RECORD(kCapabilityDefinition,
                                "capability_definition", CapabilityId,
                                CapabilityDefinitionRevision)
  MANYFORGE_PARSE_SOURCE_RECORD(kContactPolicy, "contact_policy",
                                ContactPolicyId, ContactPolicyRevision)
  MANYFORGE_PARSE_SOURCE_RECORD(kStopEvidencePolicy, "stop_evidence_policy",
                                StopEvidencePolicyId,
                                StopEvidencePolicyRevision)
#undef MANYFORGE_PARSE_SOURCE_RECORD
  fail(std::string(path) + ": unknown catalog record kind");
}

[[nodiscard]] std::vector<core::RetainedSourceRecordDTO>
parse_source_records(const CanonicalValue& value, std::string_view path) {
  const auto& array = require_array(value, path);
  if (array.size() > core::kMaxProgramBindingProvenanceRecords) {
    fail(std::string(path) + ": record table exceeds fixed bound");
  }
  std::vector<core::RetainedSourceRecordDTO> result;
  result.reserve(array.size());
  for (std::size_t index = 0U; index < array.size(); ++index) {
    result.push_back(parse_source_record(array[index], index_path(path, index)));
  }
  return result;
}

[[nodiscard]] std::vector<std::string> parse_string_array(
    const CanonicalValue& value, std::string_view path, bool nonempty) {
  const auto& array = require_array(value, path);
  if ((nonempty && array.empty()) ||
      array.size() > core::kMaxProgramNestedRoleBindings) {
    fail(std::string(path) + ": correspondence violates fixed bounds");
  }
  std::vector<std::string> result;
  result.reserve(array.size());
  for (std::size_t index = 0U; index < array.size(); ++index) {
    const std::string& item = require_string(array[index], index_path(path, index));
    require_text(item, index_path(path, index));
    result.push_back(item);
  }
  return result;
}

template <typename StrongType>
[[nodiscard]] std::vector<StrongType> parse_identity_array(
    const CanonicalValue& value, std::string_view path, bool nonempty) {
  const auto& array = require_array(value, path);
  if ((nonempty && array.empty()) ||
      array.size() > core::kMaxProgramNestedRoleBindings) {
    fail(std::string(path) + ": correspondence violates fixed bounds");
  }
  std::vector<StrongType> result;
  result.reserve(array.size());
  for (std::size_t index = 0U; index < array.size(); ++index) {
    result.push_back(
        parse_identity<StrongType>(array[index], index_path(path, index)));
  }
  return result;
}

template <typename Parse>
[[nodiscard]] auto parse_reference_array(const CanonicalValue& value,
                                         std::string_view path, Parse parse) {
  const auto& rows = require_array(value, path);
  if (rows.size() > core::kMaxProgramNestedRoleBindings)
    fail(std::string(path) + ": exceeds fixed bound");
  using T = decltype(parse(rows.front(), path));
  std::vector<T> result;
  result.reserve(rows.size());
  for (std::size_t i = 0; i < rows.size(); ++i)
    result.push_back(parse(rows[i], index_path(path, i)));
  return result;
}

[[nodiscard]] core::ProgramFrameBindingDTO parse_frame_binding(
    const CanonicalValue& value, std::string_view path) {
  const auto& row = require_object(value, path);
  require_exact_fields(row, {"reference", "model_instance_id", "model_revision",
                             "compiled_frame_name"}, path);
  return {parse_frame_reference(require_member(row, "reference", path),
                                child_path(path, "reference")),
          parse_identity<core::ModelInstanceId>(require_member(row, "model_instance_id", path),
                                               child_path(path, "model_instance_id")),
          parse_digest<core::ModelRevision>(require_member(row, "model_revision", path),
                                            child_path(path, "model_revision")),
          require_string(require_member(row, "compiled_frame_name", path),
                         child_path(path, "compiled_frame_name"))};
}

[[nodiscard]] core::ProgramBindingDTO parse_program_binding(
    const CanonicalValue& value) {
  const auto& root = require_object(value, "program_binding");
  require_exact_fields(
      root,
      {"api_version", "program_binding_id", "program_id",
       "program_revision", "retained_bundle_id", "graph_revision",
       "node_environment", "component_bindings",
       "target_bindings", "frame_bindings", "collision_body_bindings",
       "signal_bindings", "node_policies", "contact_grasp_bindings",
       "provenance"},
      "program_binding");

  const auto& node_environment = require_object(
      require_member(root, "node_environment", "program_binding"),
      "program_binding.node_environment");
  require_exact_fields(node_environment,
                       {"catalog_id", "content_version", "catalog_digest",
                        "environment_digest"},
                       "program_binding.node_environment");

  const auto& component_values = require_array(
      require_member(root, "component_bindings", "program_binding"),
      "program_binding.component_bindings");
  if (component_values.size() > core::kMaxProgramRoleBindings) {
    fail("program_binding.component_bindings: violates fixed bounds");
  }
  std::vector<core::ProgramComponentBindingDTO> components;
  components.reserve(component_values.size());
  for (std::size_t index = 0U; index < component_values.size(); ++index) {
    const std::string path = index_path(
        "program_binding.component_bindings", index);
    const auto& component = require_object(component_values[index], path);
    require_exact_fields(
        component,
        {"component_id", "component_type_id", "component_type_revision",
         "model_bindings"},
        path);
    const auto& model_values = require_array(
        require_member(component, "model_bindings", path),
        child_path(path, "model_bindings"));
    if (model_values.size() > core::kMaxProgramNestedRoleBindings) {
      fail(path + ": model role table exceeds fixed bound");
    }
    std::vector<core::ProgramModelRoleBindingDTO> models;
    models.reserve(model_values.size());
    for (std::size_t model_index = 0U; model_index < model_values.size();
         ++model_index) {
      const std::string model_path =
          index_path(child_path(path, "model_bindings"), model_index);
      const auto& model = require_object(model_values[model_index], model_path);
      require_exact_fields(
          model, {"model_role_id", "model_instance_id", "model_revision"},
          model_path);
      models.push_back(core::ProgramModelRoleBindingDTO{
          parse_identity<core::ModelRoleId>(
              require_member(model, "model_role_id", model_path),
              child_path(model_path, "model_role_id")),
          parse_identity<core::ModelInstanceId>(
              require_member(model, "model_instance_id", model_path),
              child_path(model_path, "model_instance_id")),
          parse_digest<core::ModelRevision>(
              require_member(model, "model_revision", model_path),
              child_path(model_path, "model_revision")),
      });
    }
    components.push_back(core::ProgramComponentBindingDTO{
        parse_identity<core::ControllableComponentId>(
            require_member(component, "component_id", path),
            child_path(path, "component_id")),
        parse_identity<core::ComponentTypeId>(
            require_member(component, "component_type_id", path),
            child_path(path, "component_type_id")),
        parse_digest<core::ComponentTypeRevision>(
            require_member(component, "component_type_revision", path),
            child_path(path, "component_type_revision")),
        std::move(models),
    });
  }

  const auto& target_values = require_array(
      require_member(root, "target_bindings", "program_binding"),
      "program_binding.target_bindings");
  if (target_values.size() > core::kMaxProgramRoleBindings) {
    fail("program_binding.target_bindings: exceeds fixed bound");
  }
  std::vector<core::ProgramTargetBindingDTO> targets;
  targets.reserve(target_values.size());
  for (std::size_t index = 0U; index < target_values.size(); ++index) {
    const std::string path =
        index_path("program_binding.target_bindings", index);
    const auto& target = require_object(target_values[index], path);
    require_fields(target,
                   {"target", "frame_references", "frame_associations", "collision_body_references",
                    "signal_references", "resource_role_bindings", "capability_bindings"},
                   {"pneumatic_io", "robot_tcps", "default_tcp"}, path);
    auto frames = parse_reference_array(require_member(target, "frame_references", path),
                                        child_path(path, "frame_references"), parse_frame_reference);
    auto collision_bodies = parse_reference_array(require_member(target, "collision_body_references", path),
                                        child_path(path, "collision_body_references"), parse_body_reference);
    auto signals = parse_reference_array(require_member(target, "signal_references", path),
                                        child_path(path, "signal_references"), parse_signal_reference);
    const auto associations_path = child_path(path, "frame_associations");
    const auto& association_values = require_array(
        require_member(target, "frame_associations", path), associations_path);
    if (association_values.size() > core::kMaxProgramNestedRoleBindings)
      fail(associations_path + ": exceeds fixed bound");
    std::vector<core::ProgramTargetFrameAssociationDTO> associations;
    for (std::size_t i = 0; i < association_values.size(); ++i) {
      const auto association_path = index_path(associations_path, i);
      const auto& row = require_object(association_values[i], association_path);
      require_exact_fields(row, {"frame", "contact_body_role_ids"}, association_path);
      associations.push_back({parse_frame_reference(require_member(row, "frame", association_path),
                                                     child_path(association_path, "frame")),
          parse_identity_array<core::CollisionBodyRoleId>(require_member(row, "contact_body_role_ids", association_path),
                                                           child_path(association_path, "contact_body_role_ids"), false)});
    }

    std::vector<core::ProgramResourceRoleBindingDTO> resources;
    const std::string resources_path =
        child_path(path, "resource_role_bindings");
    const auto& resource_values = require_array(
        require_member(target, "resource_role_bindings", path), resources_path);
    if (resource_values.size() > core::kMaxProgramNestedRoleBindings) {
      fail(resources_path + ": exceeds fixed bound");
    }
    resources.reserve(resource_values.size());
    for (std::size_t resource_index = 0U;
         resource_index < resource_values.size(); ++resource_index) {
      const std::string resource_path =
          index_path(resources_path, resource_index);
      const auto& resource =
          require_object(resource_values[resource_index], resource_path);
      require_exact_fields(resource,
                           {"resource_role_id", "resource_ids",
                            "compiled_resource_names"},
                           resource_path);
      resources.push_back(core::ProgramResourceRoleBindingDTO{
          parse_identity<core::ResourceRoleId>(
              require_member(resource, "resource_role_id", resource_path),
              child_path(resource_path, "resource_role_id")),
          parse_identity_array<core::ResourceId>(
              require_member(resource, "resource_ids", resource_path),
              child_path(resource_path, "resource_ids"), true),
          parse_string_array(
              require_member(resource, "compiled_resource_names", resource_path),
              child_path(resource_path, "compiled_resource_names"), true),
      });
    }

    std::vector<core::ProgramCapabilityBindingDTO> capabilities;
    const std::string capabilities_path =
        child_path(path, "capability_bindings");
    const auto& capability_values = require_array(
        require_member(target, "capability_bindings", path), capabilities_path);
    if (capability_values.size() > core::kMaxProgramCapabilityBindings) {
      fail(capabilities_path + ": exceeds fixed bound");
    }
    capabilities.reserve(capability_values.size());
    for (std::size_t capability_index = 0U;
         capability_index < capability_values.size(); ++capability_index) {
      const std::string capability_path =
          index_path(capabilities_path, capability_index);
      const auto& capability =
          require_object(capability_values[capability_index], capability_path);
      require_exact_fields(
          capability,
          {"capability_id", "capability_definition_revision",
           "control_binding", "capability_provider_binding", "command_schema"},
          capability_path);

      std::optional<core::ProgramControlBindingRefDTO> control_binding;
      const auto& control_value =
          require_member(capability, "control_binding", capability_path);
      if (!std::holds_alternative<std::nullptr_t>(control_value.storage())) {
        const auto& control = require_object(
            control_value, child_path(capability_path, "control_binding"));
        require_exact_fields(control,
                             {"control_binding_id",
                              "control_binding_revision"},
                             child_path(capability_path, "control_binding"));
        control_binding = core::ProgramControlBindingRefDTO{
            parse_identity<core::ControlBindingId>(
                require_member(control, "control_binding_id", capability_path),
                child_path(capability_path, "control_binding_id")),
            parse_digest<core::ControlBindingRevision>(
                require_member(control, "control_binding_revision",
                               capability_path),
                child_path(capability_path, "control_binding_revision")),
        };
      }

      std::optional<core::ProgramCapabilityProviderBindingRefDTO>
          provider_binding;
      const auto& provider_value = require_member(
          capability, "capability_provider_binding", capability_path);
      if (!std::holds_alternative<std::nullptr_t>(provider_value.storage())) {
        const auto& provider = require_object(
            provider_value,
            child_path(capability_path, "capability_provider_binding"));
        require_exact_fields(
            provider,
            {"capability_provider_binding_id",
             "capability_provider_binding_revision"},
            child_path(capability_path, "capability_provider_binding"));
        provider_binding = core::ProgramCapabilityProviderBindingRefDTO{
            parse_identity<core::CapabilityProviderBindingId>(
                require_member(provider, "capability_provider_binding_id",
                               capability_path),
                child_path(capability_path,
                           "capability_provider_binding_id")),
            parse_digest<core::CapabilityProviderBindingRevision>(
                require_member(provider,
                               "capability_provider_binding_revision",
                               capability_path),
                child_path(capability_path,
                           "capability_provider_binding_revision")),
        };
      }

      std::optional<core::ProgramCommandSchemaRefDTO> command_schema;
      const auto& command_schema_value =
          require_member(capability, "command_schema", capability_path);
      if (!std::holds_alternative<std::nullptr_t>(
              command_schema_value.storage())) {
        const auto& schema = require_object(
            command_schema_value,
            child_path(capability_path, "command_schema"));
        require_exact_fields(
            schema, {"command_schema_id", "command_schema_revision"},
            child_path(capability_path, "command_schema"));
        command_schema = core::ProgramCommandSchemaRefDTO{
            parse_identity<core::CommandSchemaId>(
                require_member(schema, "command_schema_id", capability_path),
                child_path(capability_path, "command_schema_id")),
            parse_digest<core::CommandSchemaRevision>(
                require_member(schema, "command_schema_revision",
                               capability_path),
                child_path(capability_path, "command_schema_revision")),
        };
      }

      capabilities.push_back(core::ProgramCapabilityBindingDTO{
          parse_identity<core::CapabilityId>(
              require_member(capability, "capability_id", capability_path),
              child_path(capability_path, "capability_id")),
          parse_digest<core::CapabilityDefinitionRevision>(
              require_member(capability, "capability_definition_revision",
                             capability_path),
              child_path(capability_path,
                         "capability_definition_revision")),
          std::move(control_binding), std::move(provider_binding),
          std::move(command_schema),
      });
    }

    std::optional<core::ProgramPneumaticIoBindingDTO> pneumatic_io;
    if (const auto *supplied = find_member(target, "pneumatic_io")) {
      const auto &route = require_object(*supplied, child_path(path, "pneumatic_io"));
      require_exact_fields(route, {"outputs", "inputs"}, child_path(path, "pneumatic_io"));
      core::ProgramPneumaticIoBindingDTO decoded;
      for (const auto &item : require_array(require_member(route, "outputs", path), path)) {
        const auto &row = require_object(item, path);
        require_exact_fields(row, {"signal", "open_value", "close_value"}, path);
        const auto &open = require_member(row, "open_value", path);
        const auto &close = require_member(row, "close_value", path);
        if (!open.is_bool() || !close.is_bool()) fail(path + ": pneumatic output values must be boolean");
        decoded.outputs.push_back({
            parse_signal_reference(require_member(row, "signal", path), child_path(path, "signal")),
            open.as_bool(), close.as_bool()});
      }
      for (const auto &item : require_array(require_member(route, "inputs", path), path)) {
        const auto &row = require_object(item, path);
        require_exact_fields(row, {"input", "signal"}, path);
        decoded.inputs.push_back({
            require_string(require_member(row, "input", path), path),
            parse_signal_reference(require_member(row, "signal", path), child_path(path, "signal"))});
      }
      pneumatic_io = std::move(decoded);
    }

    std::optional<std::vector<core::ProgramRobotTcpBindingDTO>> robot_tcps;
    if (const auto *supplied = find_member(target, "robot_tcps")) {
      robot_tcps.emplace();
      const auto tcps_path = child_path(path, "robot_tcps");
      const auto &values = require_array(*supplied, tcps_path);
      if (values.size() > core::kMaxProgramNestedRoleBindings)
        fail(tcps_path + ": exceeds fixed bound");
      robot_tcps->reserve(values.size());
      for (std::size_t tcp_index = 0U; tcp_index < values.size(); ++tcp_index) {
        const auto tcp_path = index_path(tcps_path, tcp_index);
        const auto &tcp = require_object(values[tcp_index], tcp_path);
        require_exact_fields(tcp,
                             {"entry", "frame", "contact_body_role_ids", "end_effector",
                              "component_default"},
                             tcp_path);
        const auto decoded_entry = parse_tcp_reference(
            require_member(tcp, "entry", tcp_path), child_path(tcp_path, "entry"));
        const auto decoded_frame = parse_frame_binding(
            require_member(tcp, "frame", tcp_path), child_path(tcp_path, "frame"));
        const auto &end_effector =
            require_member(tcp, "end_effector", tcp_path);
        const auto &component_default =
            require_member(tcp, "component_default", tcp_path);
        if (!end_effector.is_bool() || !component_default.is_bool())
          fail(tcp_path + ": TCP marks must be boolean");
        robot_tcps->push_back(core::ProgramRobotTcpBindingDTO{
            decoded_entry, decoded_frame,
            parse_identity_array<core::CollisionBodyRoleId>(
                require_member(tcp, "contact_body_role_ids", tcp_path),
                child_path(tcp_path, "contact_body_role_ids"), false), end_effector.as_bool(),
            component_default.as_bool()});
      }
    }

    std::optional<core::TcpEntryReferenceDTO> default_tcp;
    if (const auto* supplied = find_member(target, "default_tcp"))
      default_tcp = parse_tcp_reference(*supplied, child_path(path, "default_tcp"));
    targets.push_back(core::ProgramTargetBindingDTO{
        parse_target_reference(require_member(target, "target", path), child_path(path, "target")),
        std::move(frames), std::move(collision_bodies), std::move(signals),
        std::move(resources), std::move(capabilities), std::move(pneumatic_io),
        std::move(robot_tcps), std::move(default_tcp), std::move(associations)});
  }

  const auto& node_policy_values = require_array(
      require_member(root, "node_policies", "program_binding"),
      "program_binding.node_policies");
  if (node_policy_values.size() > core::kMaxProgramNodePolicies) {
    fail("program_binding.node_policies: exceeds Program tree-node bound");
  }
  std::vector<core::ProgramNodePolicyDTO> node_policies;
  node_policies.reserve(node_policy_values.size());
  for (std::size_t index = 0U; index < node_policy_values.size(); ++index) {
    const auto path = index_path("program_binding.node_policies", index);
    node_policies.push_back(
        parse_program_node_policy(node_policy_values[index], path));
  }

  const auto& contact_values = require_array(
      require_member(root, "contact_grasp_bindings", "program_binding"),
      "program_binding.contact_grasp_bindings");
  if (contact_values.size() > core::kMaxProgramRoleBindings) {
    fail("program_binding.contact_grasp_bindings: exceeds fixed bound");
  }
  std::vector<core::ProgramContactGraspBindingDTO> contact_bindings;
  contact_bindings.reserve(contact_values.size());
  for (std::size_t policy_index = 0U;
       policy_index < contact_values.size(); ++policy_index) {
    const std::string policy_path =
        index_path("program_binding.contact_grasp_bindings", policy_index);
    const auto& policy = require_object(contact_values[policy_index],
                                        policy_path);
    const auto* mode = find_member(policy, "mode");
    const bool release = mode != nullptr;
    if (release) {
      if (require_string(*mode, child_path(policy_path, "mode")) != "contact_release")
        fail(policy_path + ": unsupported contact mode");
      require_fields(policy,
          {"contact_policy_id", "contact_policy_revision", "runtime_object_id", "allowed_body_bindings",
           "mode", "release_evidence_mode", "enforce_max_start_penetration", "max_start_penetration_m",
           "min_release_progress_m", "release_tolerance_m", "require_strictly_decreasing_penetration",
           "require_separation_by_end", "allow_recontact"}, {"bodies_from_motion_endpoint"}, policy_path);
    } else {
      require_fields(policy, {"contact_policy_id", "contact_policy_revision", "runtime_object_id",
          "allowed_body_bindings", "max_penetration_m", "terminal_window_start_fraction"},
          {"bodies_from_motion_endpoint"}, policy_path);
    }
    bool bodies_from_motion_endpoint = false;
    if (const auto* supplied = find_member(policy, "bodies_from_motion_endpoint")) {
      const auto* selected = std::get_if<bool>(&supplied->storage());
      if (selected == nullptr || !*selected) {
        fail(policy_path + ": bodies_from_motion_endpoint must be true or omitted");
      }
      bodies_from_motion_endpoint = true;
    }
    const std::string bodies_path =
        child_path(policy_path, "allowed_body_bindings");
    const auto& body_values = require_array(
        require_member(policy, "allowed_body_bindings", policy_path),
        bodies_path);
    if ((bodies_from_motion_endpoint != body_values.empty()) ||
        body_values.size() > core::kMaxProgramNestedRoleBindings) {
      fail(bodies_path + ": endpoint/explicit body-selector shape or fixed bound is invalid");
    }
    std::vector<core::ProgramContactGraspBodyBindingDTO> bodies;
    bodies.reserve(body_values.size());
    for (std::size_t body_index = 0U; body_index < body_values.size();
         ++body_index) {
      const std::string body_path = index_path(bodies_path, body_index);
      const auto& body = require_object(body_values[body_index], body_path);
      require_exact_fields(
          body,
          {"body", "model_role_id", "model_instance_id",
           "model_revision", "collision_body_ids"},
          body_path);
      std::optional<core::ModelRoleId> model_role;
      const auto& model_role_value =
          require_member(body, "model_role_id", body_path);
      if (!std::holds_alternative<std::nullptr_t>(
              model_role_value.storage())) {
        model_role = parse_identity<core::ModelRoleId>(
            model_role_value, child_path(body_path, "model_role_id"));
      }
      const auto body_id_values = parse_string_array(
          require_member(body, "collision_body_ids", body_path),
          child_path(body_path, "collision_body_ids"), true);
      bodies.push_back(core::ProgramContactGraspBodyBindingDTO{
          parse_body_reference(require_member(body, "body", body_path), child_path(body_path, "body")),
          std::move(model_role),
          parse_identity<core::ModelInstanceId>(
              require_member(body, "model_instance_id", body_path),
              child_path(body_path, "model_instance_id")),
          parse_digest<core::ModelRevision>(
              require_member(body, "model_revision", body_path),
              child_path(body_path, "model_revision")),
          body_id_values});
    }
    contact_bindings.push_back(core::ProgramContactGraspBindingDTO{
        parse_identity<core::ContactPolicyId>(
            require_member(policy, "contact_policy_id", policy_path),
            child_path(policy_path, "contact_policy_id")),
        parse_digest<core::ContactPolicyRevision>(
            require_member(policy, "contact_policy_revision", policy_path),
            child_path(policy_path, "contact_policy_revision")),
        parse_identity<core::RuntimeObjectId>(
            require_member(policy, "runtime_object_id", policy_path),
            child_path(policy_path, "runtime_object_id")),
        std::move(bodies),
        [&]() -> core::ContactZoneEnvelopeDTO {
          if (release) return detail::parse_program_contact_envelope(policy, true, true);
          return core::ContactGraspEnvelopeDTO{
              require_number(require_member(policy, "max_penetration_m", policy_path),
                             child_path(policy_path, "max_penetration_m")),
              require_number(
                  require_member(policy, "terminal_window_start_fraction", policy_path),
                  child_path(policy_path, "terminal_window_start_fraction"))};
        }(),
        bodies_from_motion_endpoint});
  }

  const auto& provenance = require_object(
      require_member(root, "provenance", "program_binding"),
      "program_binding.provenance");
  require_exact_fields(
      provenance,
      {"binding_compiler_contract_digest", "source_records", "resolved_output_records"},
      "program_binding.provenance");

  const auto& frame_values = require_array(require_member(root, "frame_bindings", "program_binding"), "program_binding.frame_bindings");
  if (frame_values.size() > core::kMaxProgramRoleBindings) fail("frame bindings exceed fixed bound");
  std::vector<core::ProgramFrameBindingDTO> root_frames;
  for (std::size_t i = 0; i < frame_values.size(); ++i)
    root_frames.push_back(parse_frame_binding(frame_values[i], index_path("program_binding.frame_bindings", i)));
  const auto& body_values = require_array(require_member(root, "collision_body_bindings", "program_binding"), "program_binding.collision_body_bindings");
  if (body_values.size() > core::kMaxProgramRoleBindings) fail("body bindings exceed fixed bound");
  std::vector<core::ProgramCollisionBodyBindingDTO> root_bodies;
  for (std::size_t i = 0; i < body_values.size(); ++i) {
    const auto path = index_path("program_binding.collision_body_bindings", i);
    const auto& row = require_object(body_values[i], path);
    require_exact_fields(row, {"reference", "model_instance_id", "model_revision", "compiled_collision_body_names"}, path);
    root_bodies.push_back({parse_body_reference(require_member(row, "reference", path), child_path(path, "reference")),
        parse_identity<core::ModelInstanceId>(require_member(row, "model_instance_id", path), child_path(path, "model_instance_id")),
        parse_digest<core::ModelRevision>(require_member(row, "model_revision", path), child_path(path, "model_revision")),
        parse_string_array(require_member(row, "compiled_collision_body_names", path), child_path(path, "compiled_collision_body_names"), true)});
  }
  const auto& signal_values = require_array(require_member(root, "signal_bindings", "program_binding"), "program_binding.signal_bindings");
  if (signal_values.size() > core::kMaxProgramRoleBindings) fail("signal bindings exceed fixed bound");
  std::vector<core::ProgramSignalBindingDTO> root_signals;
  for (std::size_t i = 0; i < signal_values.size(); ++i) {
    const auto path = index_path("program_binding.signal_bindings", i);
    const auto& row = require_object(signal_values[i], path);
    require_exact_fields(row, {"reference", "target", "runtime_unit_id", "value_type", "direction", "transition_capability", "signal_descriptor_digest"}, path);
    const auto& type = require_string(require_member(row, "value_type", path), path);
    const auto& direction = require_string(require_member(row, "direction", path), path);
    const auto& transition = require_string(require_member(row, "transition_capability", path), path);
    if (type != "bool" || (direction != "input" && direction != "output") || (transition != "sampled_only" && transition != "captured")) fail(path + ": unsupported signal binding contract");
    root_signals.push_back({parse_signal_reference(require_member(row, "reference", path), child_path(path, "reference")),
        parse_target_reference(require_member(row, "target", path), child_path(path, "target")),
        parse_identity<core::RuntimeUnitId>(require_member(row, "runtime_unit_id", path), child_path(path, "runtime_unit_id")),
        core::IoSignalValueType::kBoolean, direction == "input" ? core::IoSignalDirection::kInput : core::IoSignalDirection::kOutput,
        transition == "sampled_only" ? core::IoTransitionCapability::kSampledOnly : core::IoTransitionCapability::kCaptured,
        parse_digest<core::ContentDigest>(require_member(row, "signal_descriptor_digest", path), child_path(path, "signal_descriptor_digest"))});
  }

  core::ProgramBindingDTO result{
      require_string(require_member(root, "api_version", "program_binding"),
                     "program_binding.api_version"),
      parse_digest<core::ProgramBindingId>(
          require_member(root, "program_binding_id", "program_binding"),
          "program_binding.program_binding_id"),
      parse_identity<core::ProgramId>(
          require_member(root, "program_id", "program_binding"),
          "program_binding.program_id"),
      parse_digest<core::ProgramRevision>(
          require_member(root, "program_revision", "program_binding"),
          "program_binding.program_revision"),
      parse_digest<core::RetainedBundleId>(
          require_member(root, "retained_bundle_id", "program_binding"),
          "program_binding.retained_bundle_id"),
      parse_digest<core::GraphRevision>(
          require_member(root, "graph_revision", "program_binding"),
          "program_binding.graph_revision"),
      core::RetainedNodeEnvironmentDTO{
          require_string(require_member(node_environment, "catalog_id",
                                        "program_binding.node_environment"),
                         "program_binding.node_environment.catalog_id"),
          require_string(require_member(node_environment, "content_version",
                                        "program_binding.node_environment"),
                         "program_binding.node_environment.content_version"),
          parse_digest<core::ContentDigest>(
              require_member(node_environment, "catalog_digest",
                             "program_binding.node_environment"),
              "program_binding.node_environment.catalog_digest"),
          parse_digest<core::ContentDigest>(
              require_member(node_environment, "environment_digest",
                             "program_binding.node_environment"),
              "program_binding.node_environment.environment_digest"),
      },
      std::move(components), std::move(targets), std::move(root_frames),
      std::move(root_signals), std::move(root_bodies),
      core::ProgramBindingProvenanceDTO{
          parse_digest<core::ContentDigest>(
              require_member(provenance, "binding_compiler_contract_digest",
                             "program_binding.provenance"),
              "program_binding.provenance.binding_compiler_contract_digest"),
          parse_source_records(
              require_member(provenance, "source_records",
                             "program_binding.provenance"),
              "program_binding.provenance.source_records"),
          parse_source_records(
              require_member(provenance, "resolved_output_records",
                             "program_binding.provenance"),
              "program_binding.provenance.resolved_output_records"),
      },
      std::move(contact_bindings),
      std::move(node_policies),
  };
  core::ProgramBindingDTO normalized = result;
  normalize_and_validate(normalized);
  if (normalized != result) {
    fail("program binding arrays are not in canonical order");
  }
  return result;
}

template <typename Value, typename Predicate>
[[nodiscard]] const Value* find_exactly_one(std::span<const Value> values,
                                            Predicate predicate) {
  const Value* found = nullptr;
  for (const auto& value : values) {
    if (!predicate(value)) {
      continue;
    }
    if (found != nullptr) {
      return nullptr;
    }
    found = &value;
  }
  return found;
}

[[nodiscard]] const core::CatalogRecordRefDTO* find_catalog_record(
    const core::RetainedBundleDTO& bundle, core::CatalogRecordKind kind,
    std::string_view id, std::string_view revision) {
  return find_exactly_one<core::CatalogRecordRefDTO>(
      bundle.catalogs.records, [&](const auto& record) {
        return core::catalog_record_kind(record) == kind &&
               core::catalog_record_id_value(record) == id &&
               core::catalog_record_revision_value(record) == revision;
      });
}

void verify_source_records_against_bundle(
    std::span<const core::RetainedSourceRecordDTO> records,
    const core::RetainedBundleDTO& bundle, std::string_view path) {
  for (const auto& record : records) {
    const bool exists = std::visit(
        [&](const auto& typed) {
          using Record = std::remove_cvref_t<decltype(typed)>;
          return find_catalog_record(bundle, Record::kRecordKind,
                                     typed.record_id.value(),
                                     typed.revision.value()) != nullptr;
        },
        record);
    if (!exists) {
      fail(std::string(path) +
           ": provenance row is absent from the exact retained catalog");
    }
  }
}

[[nodiscard]] const core::ModelInstanceDTO* find_model_instance(
    const core::CellGraphDTO& graph,
    const core::ModelInstanceId& model_instance_id) {
  return find_exactly_one<core::ModelInstanceDTO>(
      graph.model_instances, [&](const auto& model) {
        return model.model_instance_id == model_instance_id;
      });
}

[[nodiscard]] const core::ControllableComponentDTO* find_component(
    const core::CellGraphDTO& graph,
    const core::ControllableComponentId& component_id) {
  return find_exactly_one<core::ControllableComponentDTO>(
      graph.controllable_components, [&](const auto& component) {
        return component.controllable_component_id == component_id;
      });
}

[[nodiscard]] const core::ControlBindingDTO* find_control_binding(
    const core::CellGraphDTO& graph,
    const core::ControlBindingId& control_binding_id) {
  return find_exactly_one<core::ControlBindingDTO>(
      graph.control_bindings, [&](const auto& binding) {
        return binding.control_binding_id == control_binding_id;
      });
}

void verify_provider_target(
    const core::CapabilityProviderBindingDTO& provider,
    const core::ProgramTargetBindingDTO& target,
    const core::CellGraphDTO& graph) {
  const bool matches = std::visit(
      [&](const auto& provider_target) {
        using Target = std::remove_cvref_t<decltype(provider_target)>;
        if constexpr (std::is_same_v<
                          Target, core::CellGlobalCapabilityProviderTargetDTO>) {
          return provider.scope == core::CapabilityScope::kCellGlobal &&
                 provider_target.cell_id == graph.cell_id;
        } else if constexpr (std::is_same_v<Target, core::ComponentTargetCapabilityProviderTargetDTO>) {
          return provider_target.controllable_component_id == target.target.component_id &&
                 provider_target.command_target_id == target.target.command_target_id &&
                 ValidateNonCoordinateProviderTarget(graph, provider).ok();
        } else if constexpr (std::is_same_v<
                                 Target,
                                 core::TargetBindingCapabilityProviderTargetDTO>) {
          const auto* component =
              find_component(graph, target.target.component_id);
          const auto* control =
              find_control_binding(graph, provider_target.control_binding_id);
          return provider.scope == core::CapabilityScope::kTargetBinding &&
                 component != nullptr && control != nullptr &&
                 provider_target.controllable_component_id ==
                     target.target.component_id &&
                 provider_target.component_type_revision ==
                     component->component_type_revision &&
                 provider_target.command_target_id == target.target.command_target_id &&
                 provider_target.control_binding_revision ==
                     control->control_binding_revision &&
                 control->controllable_component_id ==
                     target.target.component_id &&
                 control->command_target_id == target.target.command_target_id;
        } else {
          const bool target_uses_domain = std::any_of(
              graph.control_bindings.begin(), graph.control_bindings.end(),
              [&](const auto& candidate) {
                return candidate.controllable_component_id ==
                           target.target.component_id &&
                       candidate.command_target_id ==
                           target.target.command_target_id &&
                       candidate.control_domain_id ==
                           provider_target.control_domain_id;
              });
          const auto* domain = find_exactly_one<core::ControlDomainDTO>(
              graph.control_domains, [&](const auto& candidate) {
                return candidate.control_domain_id ==
                       provider_target.control_domain_id;
              });
          if (domain && provider_target.provider()) {
            const auto* driver = domain->provider();
            const auto configured = ResolveRetainedIoSignalCatalog(graph);
            return driver && configured.ok() &&
                provider.scope == core::CapabilityScope::kControlDomain &&
                provider_target.control_domain_revision == domain->control_domain_revision &&
                driver->controllable_component_id == target.target.component_id &&
                driver->provider_instance_id == provider.provider_instance_id &&
                driver->provider_manifest_id == provider.provider_manifest_id &&
                driver->provider_manifest_revision == provider.provider_manifest_revision &&
                std::any_of(configured.value().begin(), configured.value().end(), [&](const auto& row) {
                  return row.runtime_unit_id == driver->provider_runtime_unit_id &&
                         row.descriptor.signal_id.controllable_component_id == target.target.component_id &&
                         row.command_target_id == target.target.command_target_id;
                });
          }
          return provider.scope == core::CapabilityScope::kControlDomain &&
                 target_uses_domain && domain != nullptr &&
                 !domain->provider() && !provider_target.provider() &&
                 provider_target.control_domain_revision ==
                     domain->control_domain_revision &&
                 provider_target.controller().controller_topology_id ==
                     domain->controller().controller_topology_id &&
                 provider_target.controller().topology_revision == domain->controller().topology_revision;
        }
      },
      provider.target);
  if (!matches) {
    fail("capability provider binding has the wrong retained scope target");
  }
}

void verify_binding_context(const core::ProgramBindingDTO& binding,
                            const RetainedBundle& retained_bundle,
                            const VerifiedRetainedModelMap& retained_model_map,
                            const ICatalogArtifactSource& catalog_source) {
  const auto& bundle = retained_bundle.value();
  const auto& graph = bundle.cell_graph;
  if (binding.retained_bundle_id != retained_bundle.retained_bundle_id() ||
      binding.graph_revision != graph.graph_revision ||
      binding.node_environment != bundle.catalogs.node_environment ||
      retained_model_map.query().retained_bundle_id !=
          retained_bundle.retained_bundle_id()) {
    fail("program binding selects another bundle, graph, node environment, or model map");
  }
  if (!verify_retained_bundle_identity(retained_bundle)) {
    fail("selected retained bundle no longer passes its intrinsic identity proof");
  }
  const auto io_catalog = ResolveCompiledIoSignalCatalog(graph);
  if (!io_catalog.ok())
    fail(io_catalog.error().details.at("reason"));

  for (const auto& component_binding : binding.component_bindings) {
    const auto* component =
        find_component(graph, component_binding.component_id);
    if (component == nullptr ||
        component->component_type_id != component_binding.component_type_id ||
        component->component_type_revision !=
            component_binding.component_type_revision) {
      fail("component role binding differs from the exact retained component");
    }
    for (const auto& model_binding : component_binding.model_bindings) {
      const auto* graph_role = find_exactly_one<core::ModelRoleBindingDTO>(
          component->model_role_bindings, [&](const auto& role) {
            return role.model_role_id == model_binding.model_role_id;
          });
      const auto* model =
          find_model_instance(graph, model_binding.model_instance_id);
      if (graph_role == nullptr || model == nullptr ||
          graph_role->model_instance_id != model_binding.model_instance_id ||
          model->model_revision != model_binding.model_revision) {
        fail("component model-role binding differs from the retained graph");
      }
    }
  }

  for (const auto& frame_binding : binding.frame_bindings) {
    const auto matches = find_program_frame_matches(frame_binding.reference, retained_model_map);
    if (matches.size() != 1U || matches.front() != frame_binding)
      fail("frame binding differs from the exact retained model map");
  }
  for (const auto& body : binding.collision_body_bindings) {
    const auto matches = find_program_collision_body_matches(body.reference, retained_model_map);
    if (matches.size() != 1U || matches.front() != body)
      fail("body binding differs from the exact retained model map");
  }
  for (const auto& signal : binding.signal_bindings) {
    const auto matches = FindProgramSignalMatches(io_catalog.value(), signal.reference);
    if (matches.size() != 1U)
      fail("signal reference is missing or ambiguous in retained catalog");
    const auto* resolved = matches.front().retained;
    const auto& descriptor = matches.front().descriptor;
    const auto digest = core::ComputeIoSignalDescriptorDigest(descriptor);
    if (!digest.ok() || digest.value() != signal.signal_descriptor_digest ||
        signal.target.component_id != resolved->descriptor.signal_id.controllable_component_id || signal.target.command_target_id != resolved->command_target_id ||
        signal.runtime_unit_id != resolved->runtime_unit_id || descriptor.signal_id.name != signal.reference.signal_name ||
        descriptor.value_type != signal.value_type || descriptor.direction != signal.direction || descriptor.transition_capability != signal.transition_capability)
      fail("signal binding differs from the exact retained descriptor");
  }
  for (const auto& target_binding : binding.target_bindings) {
    const auto* component = find_component(graph, target_binding.target.component_id);
    const auto* target = retained_model_map.find_target(target_binding.target.component_id, target_binding.target.command_target_id);
    if (!component || !target) fail("target binding has no exact retained component/target resolution");
    const auto component_type = qualified_program_component_type(
        target_binding.target.component_id, retained_bundle, catalog_source);
    const auto* target_type = find_exactly_one<core::ComponentTypeCommandTargetDTO>(
        component_type.command_targets, [&](const auto& value) {
          return value.command_target_id == target_binding.target.command_target_id;
        });
    if (!target_type) fail("target binding has no exact qualified component-type target");
    const auto expected_associations = reference_probe::resolve_target_frame_associations(
        target_binding.target, target_binding.frame_references, *target_type, *target,
        retained_model_map, binding.frame_bindings);
    if (expected_associations != target_binding.frame_associations)
      fail("target frame contact associations differ from the exact retained catalog");
    const auto* retained_default = retained_model_map.kinematic_index().default_tcp_of(target_binding.target.component_id);
    std::optional<core::TcpEntryReferenceDTO> expected_default;
    if (retained_default) {
      const auto* default_target = retained_model_map.find_target(target_binding.target.component_id,
                                                                  retained_default->command_target_id);
      const auto* default_row = default_target ? find_target_frame_role(*default_target, retained_default->frame_role_id) : nullptr;
      const auto* default_frame = default_row ? std::get_if<CompositionComponentModelFrame>(&default_row->frame) : nullptr;
      if (!default_frame || !retained_model_map.kinematic_index().carries(target_binding.target.component_id, default_frame->controllable_component_id))
        fail("default TCP has no exact mounted retained frame resolution");
      const core::ProgramFrameReferenceDTO reference{target_binding.target.component_id,
                                                      retained_default->frame_role_id};
      const auto matches = find_program_frame_matches(reference, retained_model_map);
      if (matches.size() != 1U || matches.front().model_instance_id != default_frame->model_instance_id ||
          matches.front().model_revision != default_frame->model_revision ||
          matches.front().compiled_frame_name != default_frame->compiled_frame_name)
        fail("default TCP alias has no unique qualified retained frame");
      expected_default = core::TcpEntryReferenceDTO{
          {target_binding.target.component_id, retained_default->command_target_id},
          reference};
    }
    if (target_binding.default_tcp != expected_default)
      fail("target default TCP differs from the exact retained default");
    for (const auto& ref : target_binding.frame_references) {
      const auto row = std::lower_bound(binding.frame_bindings.begin(), binding.frame_bindings.end(), ref, [](const auto& item, const auto& key) { return item.reference < key; });
      if (row == binding.frame_bindings.end() || row->reference != ref ||
          !retained_model_map.kinematic_index().carries(target_binding.target.component_id, ref.component_id) ||
          !std::any_of(target->frames.begin(), target->frames.end(), [&](const auto& candidate) {
            const auto* resolved = std::get_if<CompositionComponentModelFrame>(&candidate.frame);
            return resolved && resolved->controllable_component_id == ref.component_id &&
                   resolved->model_instance_id == row->model_instance_id &&
                   resolved->model_revision == row->model_revision &&
                   resolved->compiled_frame_name == row->compiled_frame_name;
          }))
        fail("target frame reference differs from retained target model map");
    }
    for (const auto& ref : target_binding.signal_references) {
      const auto matches = FindProgramSignalMatches(io_catalog.value(), ref);
      if (matches.size() != 1U)
        fail("target signal reference is missing or ambiguous");
      const auto role = matches.front().retained->resource_role_id;
      if (std::any_of(target_binding.resource_role_bindings.begin(),
                      target_binding.resource_role_bindings.end(),
                      [&](const auto& resource) { return resource.resource_role_id == role; }))
        fail("resource role resolves through both coordinate and signal bindings");
    }
    const bool motion = std::any_of(
        target_binding.capability_bindings.begin(), target_binding.capability_bindings.end(),
        [](const auto& row) { return row.capability_id.value() == "execution.trajectory.execute"; });
    if (motion) {
      const auto catalog = reference_probe::load_qualified_catalog_closure(bundle, catalog_source);
      const auto expected = reference_probe::compile_robot_tcps(
          bundle, retained_model_map, target_binding.target.component_id,
          target_binding.target.command_target_id, catalog.capabilities, catalog_source);
      if (!target_binding.robot_tcps || *target_binding.robot_tcps != expected)
        fail("robot TCP inventory differs from the exact retained catalog and model map");
    } else if (target_binding.robot_tcps) {
      fail("non-motion target has an unexpected robot TCP inventory");
    }

    const bool is_gripper = std::any_of(
        target_binding.capability_bindings.begin(), target_binding.capability_bindings.end(),
        [](const auto &row) { return row.capability_id.value() == "tool.gripper.command"; });
    if (is_gripper || target_binding.pneumatic_io) {
      using PneumaticIo = ros2_control_execution_runtime_contract::Configuration::PneumaticIo;
      std::optional<PneumaticIo> expected;
      std::size_t matches = 0U;
      for (const auto &unit : graph.runtime_units) {
        if (unit.runtime_profile_id.value() != ros2_control_execution_runtime_contract::kRuntimeProfileId)
          continue;
        const auto rows = unit.configuration.find("gripper_target_configurations");
        if (rows == unit.configuration.end()) continue;
        const auto *array = std::get_if<core::JsonValueDTO::Array>(&rows->second.value);
        if (!array) fail("retained gripper configuration is not an array");
        for (const auto &row : detail::decode_ros2_control_execution_gripper_target_configurations(*array)) {
          if (row.controllable_component_id == target_binding.target.component_id &&
              row.command_target_id == target_binding.target.command_target_id) {
            ++matches;
            expected = row.pneumatic_io;
          }
        }
      }
      if (matches > 1U || expected.has_value() != target_binding.pneumatic_io.has_value())
        fail("pneumatic I/O binding differs from its retained gripper configuration");
      if (expected) {
        const auto &actual = *target_binding.pneumatic_io;
        const auto signal_id = [&](const auto &row) -> core::IoSignalId {
          return {row.signal.component_id, row.signal.signal_name};
        };
        if (actual.outputs.size() != expected->outputs.size() || actual.inputs.size() != expected->inputs.size())
          fail("pneumatic I/O binding has a different retained mapping");
        std::set<core::IoSignalId> seen_outputs;
        for (const auto &row : actual.outputs) {
          const auto id = signal_id(row);
          if (!seen_outputs.insert(id).second)
            fail("pneumatic output repeats a retained physical signal");
          const auto found = std::find_if(expected->outputs.begin(), expected->outputs.end(),
              [&](const auto &item) { return item.signal == id; });
          if (found == expected->outputs.end() || found->open_value != row.open_value ||
              found->close_value != row.close_value)
            fail("pneumatic output differs from its retained mapping");
        }
        for (const auto &row : actual.inputs) {
          const auto found = expected->inputs.find(row.input);
          if (found == expected->inputs.end() || found->second != signal_id(row))
            fail("pneumatic input differs from its retained mapping");
        }
      }
    }

    for (const auto& resource_binding : target_binding.resource_role_bindings) {
      std::optional<std::pair<std::vector<core::ResourceId>,
                              std::vector<std::string>>>
          expected;
      for (const auto& coordinate_map : retained_model_map.control_bindings()) {
        if (coordinate_map.controllable_component_id !=
                target_binding.target.component_id ||
            coordinate_map.command_target_id !=
                target_binding.target.command_target_id) {
          continue;
        }
        std::vector<core::ResourceId> candidate_ids;
        std::vector<std::string> candidate_names;
        for (const auto& row : coordinate_map.resources) {
          if (row.resource_role_id != resource_binding.resource_role_id) {
            continue;
          }
          candidate_ids.push_back(row.resource_id);
          candidate_names.push_back(
              row.coordinates.empty()
                  ? row.resource_id.value()
                  : row.coordinates.front().compiled_joint_name);
        }
        if (candidate_ids.empty()) {
          continue;
        }
        auto candidate = std::pair{std::move(candidate_ids),
                                   std::move(candidate_names)};
        if (expected.has_value() && *expected != candidate) {
          fail("resource-role binding has conflicting retained control maps");
        }
        expected = std::move(candidate);
      }
      if (!expected.has_value()) {
        fail("resource-role binding has no retained control map");
      }
      if (expected->first != resource_binding.resource_ids ||
          expected->second != resource_binding.compiled_resource_names) {
        fail("resource-role binding differs from the retained coordinate map");
      }
    }

    for (const auto& ref : target_binding.collision_body_references) {
      const auto body = std::lower_bound(binding.collision_body_bindings.begin(), binding.collision_body_bindings.end(), ref, [](const auto& item, const auto& key) { return item.reference < key; });
      const bool has_collision = body != binding.collision_body_bindings.end() &&
          std::any_of(target->collision_bodies.begin(), target->collision_bodies.end(), [&](const auto& row) {
            return row.model_instance_id == body->model_instance_id &&
                   !body->compiled_collision_body_names.empty() &&
                   std::all_of(body->compiled_collision_body_names.begin(), body->compiled_collision_body_names.end(),
                       [&](const auto& name) {
                         return std::find(row.compiled_link_names.begin(), row.compiled_link_names.end(), name) !=
                                row.compiled_link_names.end();
                       });
          });
      if (body == binding.collision_body_bindings.end() || body->reference != ref || !has_collision ||
          !retained_model_map.kinematic_index().carries(target_binding.target.component_id, ref.component_id))
        fail("target body reference differs from retained target model map");
    }

    for (const auto& capability : target_binding.capability_bindings) {
      if (find_catalog_record(
              bundle, core::CatalogRecordKind::kCapabilityDefinition,
              capability.capability_id.value(),
              capability.capability_definition_revision.value()) == nullptr) {
        fail("capability binding selects an unretained capability definition");
      }
      if (capability.control_binding.has_value()) {
        const auto* control = find_control_binding(
            graph, capability.control_binding->control_binding_id);
        if (control == nullptr ||
            control->control_binding_revision !=
                capability.control_binding->control_binding_revision ||
            control->controllable_component_id !=
                target_binding.target.component_id ||
            control->command_target_id != target_binding.target.command_target_id ||
            control->capability_id != capability.capability_id ||
            control->capability_definition_revision !=
                capability.capability_definition_revision) {
          fail("capability control binding differs from the retained graph");
        }
      }
      if (capability.capability_provider_binding.has_value()) {
        const auto* provider =
            find_exactly_one<core::CapabilityProviderBindingDTO>(
                bundle.capability_provider_bindings,
                [&](const auto& candidate) {
                  return candidate.capability_provider_binding_id ==
                         capability.capability_provider_binding
                             ->capability_provider_binding_id;
                });
        if (provider == nullptr ||
            provider->capability_provider_binding_revision !=
                capability.capability_provider_binding
                    ->capability_provider_binding_revision ||
            provider->capability_id != capability.capability_id ||
            provider->capability_definition_revision !=
                capability.capability_definition_revision) {
          fail("capability provider binding differs from the retained bundle");
        }
        verify_provider_target(*provider, target_binding, graph);
      }
      if (capability.command_schema.has_value()) {
        if (find_catalog_record(
                bundle, core::CatalogRecordKind::kCommandSchema,
                capability.command_schema->command_schema_id.value(),
                capability.command_schema->command_schema_revision.value()) ==
            nullptr) {
          fail("capability command schema is absent from the retained catalog");
        }
      }
      const bool is_gripper_position_command =
          capability.capability_id.value() == "tool.gripper.command";
      if (is_gripper_position_command != capability.command_schema.has_value()) {
        fail("capability command-schema presence differs from the qualified "
             "command family");
      }
      if (is_gripper_position_command &&
          (capability.command_schema->command_schema_id.value() !=
               core::kGripperPositionCommandSchemaId ||
           capability.command_schema->command_schema_revision.value() !=
               core::kGripperPositionCommandSchemaRevision)) {
        fail("gripper capability selects another command-schema family");
      }
    }
  }

  verify_source_records_against_bundle(binding.provenance.source_records,
                                       bundle, "source_records");
  verify_source_records_against_bundle(
      binding.provenance.resolved_output_records, bundle,
      "resolved_output_records");
}

}  // namespace

namespace detail {

CanonicalValue program_binding_projection(
    const core::ProgramBindingDTO& value) {
  CanonicalValue::Array components;
  components.reserve(value.component_bindings.size());
  for (const auto& component : value.component_bindings) {
    CanonicalValue::Array models;
    models.reserve(component.model_bindings.size());
    for (const auto& model : component.model_bindings) {
      models.emplace_back(CanonicalValue::Object{
          {"model_role_id", CanonicalValue(model.model_role_id.value())},
          {"model_instance_id",
           CanonicalValue(model.model_instance_id.value())},
          {"model_revision", CanonicalValue(model.model_revision.value())},
      });
    }
    components.emplace_back(CanonicalValue::Object{
        {"component_id", CanonicalValue(component.component_id.value())},
        {"component_type_id",
         CanonicalValue(component.component_type_id.value())},
        {"component_type_revision",
         CanonicalValue(component.component_type_revision.value())},
        {"model_bindings", CanonicalValue(std::move(models))},
    });
  }

  CanonicalValue::Array targets;
  targets.reserve(value.target_bindings.size());
  for (const auto& target : value.target_bindings) {
    CanonicalValue::Array frames;
    for (const auto& ref : target.frame_references)
      frames.emplace_back(frame_reference_projection(ref));
    CanonicalValue::Array associations;
    for (const auto& association : target.frame_associations)
      associations.emplace_back(CanonicalValue::Object{
          {"frame", frame_reference_projection(association.frame)},
          {"contact_body_role_ids", strong_array<core::CollisionBodyRoleId>(association.contact_body_role_ids)}});
    CanonicalValue::Array collision_bodies;
    for (const auto& ref : target.collision_body_references)
      collision_bodies.emplace_back(body_reference_projection(ref));
    CanonicalValue::Array signals;
    for (const auto& ref : target.signal_references)
      signals.emplace_back(signal_reference_projection(ref));
    CanonicalValue::Array resources;
    resources.reserve(target.resource_role_bindings.size());
    for (const auto& resource : target.resource_role_bindings) {
      resources.emplace_back(CanonicalValue::Object{
          {"resource_role_id",
           CanonicalValue(resource.resource_role_id.value())},
          {"resource_ids", strong_array<core::ResourceId>(resource.resource_ids)},
          {"compiled_resource_names",
           string_array(resource.compiled_resource_names)},
      });
    }
    CanonicalValue::Array capabilities;
    capabilities.reserve(target.capability_bindings.size());
    for (const auto& capability : target.capability_bindings) {
      capabilities.emplace_back(CanonicalValue::Object{
          {"capability_id", CanonicalValue(capability.capability_id.value())},
          {"capability_definition_revision",
           CanonicalValue(
               capability.capability_definition_revision.value())},
          {"control_binding",
           optional_control_binding_projection(capability.control_binding)},
          {"capability_provider_binding",
           optional_provider_binding_projection(
               capability.capability_provider_binding)},
          {"command_schema",
           optional_command_schema_projection(capability.command_schema)},
      });
    }
    CanonicalValue::Object target_projection{
        {"target", target_reference_projection(target.target)},
        {"frame_references", CanonicalValue(std::move(frames))},
        {"frame_associations", CanonicalValue(std::move(associations))},
        {"collision_body_references", CanonicalValue(std::move(collision_bodies))},
        {"signal_references", CanonicalValue(std::move(signals))},
        {"resource_role_bindings", CanonicalValue(std::move(resources))},
        {"capability_bindings", CanonicalValue(std::move(capabilities))},
    };
    if (target.pneumatic_io) {
      CanonicalValue::Array outputs;
      for (const auto &row : target.pneumatic_io->outputs)
        outputs.emplace_back(CanonicalValue::Object{
            {"signal", signal_reference_projection(row.signal)},
            {"open_value", CanonicalValue(row.open_value)},
            {"close_value", CanonicalValue(row.close_value)}});
      CanonicalValue::Array inputs;
      for (const auto &row : target.pneumatic_io->inputs)
        inputs.emplace_back(CanonicalValue::Object{
            {"input", CanonicalValue(row.input)},
            {"signal", signal_reference_projection(row.signal)}});
      target_projection.emplace_back("pneumatic_io", CanonicalValue(CanonicalValue::Object{
          {"outputs", CanonicalValue(std::move(outputs))},
          {"inputs", CanonicalValue(std::move(inputs))}}));
    }
    if (target.robot_tcps) {
      CanonicalValue::Array tcps;
      tcps.reserve(target.robot_tcps->size());
      for (const auto& tcp : *target.robot_tcps) {
        CanonicalValue::Object frame{
            {"reference", frame_reference_projection(tcp.frame.reference)},
            {"model_instance_id",
             CanonicalValue(tcp.frame.model_instance_id.value())},
            {"model_revision", CanonicalValue(tcp.frame.model_revision.value())},
            {"compiled_frame_name",
             CanonicalValue(tcp.frame.compiled_frame_name)},
        };
        tcps.emplace_back(CanonicalValue::Object{
            {"entry", tcp_reference_projection(tcp.entry)},
            {"frame", CanonicalValue(std::move(frame))},
            {"contact_body_role_ids", strong_array<core::CollisionBodyRoleId>(tcp.contact_body_role_ids)},
            {"end_effector", CanonicalValue(tcp.end_effector)},
            {"component_default", CanonicalValue(tcp.component_default)},
        });
      }
      target_projection.emplace_back("robot_tcps",
                                     CanonicalValue(std::move(tcps)));
    }
    if (target.default_tcp.has_value())
      target_projection.emplace_back("default_tcp",
                                     tcp_reference_projection(*target.default_tcp));
    targets.emplace_back(std::move(target_projection));
  }

  CanonicalValue::Array frame_bindings;
  for (const auto& frame : value.frame_bindings) {
    CanonicalValue::Object row{
        {"reference", frame_reference_projection(frame.reference)},
        {"model_instance_id", CanonicalValue(frame.model_instance_id.value())},
        {"model_revision", CanonicalValue(frame.model_revision.value())},
        {"compiled_frame_name", CanonicalValue(frame.compiled_frame_name)}};
    frame_bindings.emplace_back(std::move(row));
  }
  CanonicalValue::Array body_bindings;
  for (const auto& body : value.collision_body_bindings)
    body_bindings.emplace_back(CanonicalValue::Object{
        {"reference", body_reference_projection(body.reference)},
        {"model_instance_id", CanonicalValue(body.model_instance_id.value())},
        {"model_revision", CanonicalValue(body.model_revision.value())},
        {"compiled_collision_body_names", string_array(body.compiled_collision_body_names)}});
  CanonicalValue::Array signal_bindings;
  for (const auto& signal : value.signal_bindings)
    signal_bindings.emplace_back(CanonicalValue::Object{
        {"reference", signal_reference_projection(signal.reference)},
        {"target", target_reference_projection(signal.target)},
        {"runtime_unit_id", CanonicalValue(signal.runtime_unit_id.value())},
        {"value_type", CanonicalValue("bool")},
        {"direction", CanonicalValue(signal.direction == core::IoSignalDirection::kInput ? "input" : "output")},
        {"transition_capability", CanonicalValue(signal.transition_capability == core::IoTransitionCapability::kSampledOnly ? "sampled_only" : "captured")},
        {"signal_descriptor_digest", CanonicalValue(signal.signal_descriptor_digest.value())}});

  CanonicalValue::Array node_policies;
  node_policies.reserve(value.node_policies.size());
  for (const auto& policy : value.node_policies) {
    node_policies.emplace_back(node_policy_projection(policy));
  }

  CanonicalValue::Array contact_grasps;
  contact_grasps.reserve(value.contact_grasp_bindings.size());
  for (const auto& policy : value.contact_grasp_bindings) {
    CanonicalValue::Array bodies;
    bodies.reserve(policy.allowed_body_bindings.size());
    for (const auto& body : policy.allowed_body_bindings) {
      bodies.emplace_back(CanonicalValue::Object{
          {"body", body_reference_projection(body.body)},
          {"model_role_id",
           body.model_role_id.has_value()
               ? CanonicalValue(body.model_role_id->value())
               : CanonicalValue(nullptr)},
          {"model_instance_id",
           CanonicalValue(body.model_instance_id.value())},
          {"model_revision", CanonicalValue(body.model_revision.value())},
          {"collision_body_ids",
           string_array(body.collision_body_ids)},
      });
    }
    CanonicalValue::Object contact{
        {"contact_policy_id",
         CanonicalValue(policy.contact_policy_id.value())},
        {"contact_policy_revision",
         CanonicalValue(policy.contact_policy_revision.value())},
        {"runtime_object_id",
         CanonicalValue(policy.runtime_object_id.value())},
        {"allowed_body_bindings", CanonicalValue(std::move(bodies))},

    };
    detail::append_program_contact_envelope(contact, policy.envelope);
    if (policy.bodies_from_motion_endpoint) {
      contact.emplace_back("bodies_from_motion_endpoint", CanonicalValue(true));
    }
    contact_grasps.emplace_back(std::move(contact));
  }

  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue(value.api_version)},
      {"program_binding_id",
       CanonicalValue(value.program_binding_id.value())},
      {"program_id", CanonicalValue(value.program_id.value())},
      {"program_revision", CanonicalValue(value.program_revision.value())},
      {"retained_bundle_id",
       CanonicalValue(value.retained_bundle_id.value())},
      {"graph_revision", CanonicalValue(value.graph_revision.value())},
      {"node_environment",
       CanonicalValue(CanonicalValue::Object{
           {"catalog_id",
            CanonicalValue(value.node_environment.catalog_id)},
           {"content_version",
            CanonicalValue(value.node_environment.content_version)},
           {"catalog_digest",
            CanonicalValue(value.node_environment.catalog_digest.value())},
           {"environment_digest",
            CanonicalValue(value.node_environment.environment_digest.value())},
       })},
      {"component_bindings", CanonicalValue(std::move(components))},
      {"target_bindings", CanonicalValue(std::move(targets))},
      {"frame_bindings", CanonicalValue(std::move(frame_bindings))},
      {"collision_body_bindings", CanonicalValue(std::move(body_bindings))},
      {"signal_bindings", CanonicalValue(std::move(signal_bindings))},
      {"node_policies", CanonicalValue(std::move(node_policies))},
      {"contact_grasp_bindings",
       CanonicalValue(std::move(contact_grasps))},
      {"provenance",
       CanonicalValue(CanonicalValue::Object{
           {"binding_compiler_contract_digest",
            CanonicalValue(
                value.provenance.binding_compiler_contract_digest.value())},
           {"source_records",
            source_records_projection(value.provenance.source_records)},
           {"resolved_output_records",
            source_records_projection(
                value.provenance.resolved_output_records)},
       })},
  });
}

std::shared_ptr<const ProgramBinding>
ProgramBindingConstructionAuthority::seal(core::ProgramBindingDTO value) {
  value.program_binding_id = core::ProgramBindingId(std::string(kZeroDigest));
  normalize_and_validate(value);
  CanonicalValue root = program_binding_projection(value);
  const detail::ContentIdentitySpec identity_spec{
      .identity_domain = "program-binding-id",
      .api_version = std::string(core::kProgramBindingApiVersion),
      .own_field = "program_binding_id",
  };
  value.program_binding_id = core::ProgramBindingId(
      detail::compute_content_identity(root, identity_spec));
  root = program_binding_projection(value);
  if (!detail::verify_content_identity(root, identity_spec)) {
    fail("program binding construction produced an invalid content identity");
  }
  const std::string bytes = canonical_json(root);
  const core::ProgramBindingId id = value.program_binding_id;
  return std::shared_ptr<const ProgramBinding>(new ProgramBinding(
      id, std::move(value), std::move(root), bytes));
}

std::shared_ptr<const ProgramBinding>
ProgramBindingConstructionAuthority::reload_intrinsic(
    std::string_view canonical_bytes) {
  if (canonical_bytes.size() > kMaxArtifactEncodedBytes) {
    fail("program binding canonical envelope exceeds fixed byte bound");
  }
  const auto parsed = parse_strict_json_model_yaml(canonical_bytes);
  CanonicalValue root = parsed.root();
  if (canonical_json(root) != canonical_bytes) {
    fail("program binding repository bytes are not RFC 8785 canonical");
  }
  core::ProgramBindingDTO value = parse_program_binding(root);
  const detail::ContentIdentitySpec identity_spec{
      .identity_domain = "program-binding-id",
      .api_version = std::string(core::kProgramBindingApiVersion),
      .own_field = "program_binding_id",
  };
  if (!detail::verify_content_identity(root, identity_spec)) {
    fail("program_binding_id does not match the complete canonical envelope");
  }
  const core::ProgramBindingId id = value.program_binding_id;
  return std::shared_ptr<const ProgramBinding>(new ProgramBinding(
      id, std::move(value), std::move(root), std::string(canonical_bytes)));
}

}  // namespace detail

std::shared_ptr<const ProgramBinding> reload_verified_program_binding(
    std::string_view canonical_bytes, const RetainedBundle& retained_bundle,
    const VerifiedRetainedModelMap& retained_model_map,
    const ICatalogArtifactSource& catalog_source) {
  try {
    auto binding =
        detail::ProgramBindingConstructionAuthority::reload_intrinsic(
            canonical_bytes);
    verify_binding_context(binding->value(), retained_bundle,
                           retained_model_map, catalog_source);
    return binding;
  } catch (const ProgramBindingError&) {
    throw;
  } catch (const std::exception& error) {
    fail(std::string("program binding verification failed: ") + error.what());
  }
}

const CompositionTargetFrameResolution* find_target_frame_role(
    const CompositionTargetResolution& target, const core::FrameRoleId& role) {
  return find_exactly_one<CompositionTargetFrameResolution>(
      target.frames, [&](const auto& frame) { return frame.frame_role_id == role; });
}

bool verify_program_binding_identity(const ProgramBinding& binding) noexcept {
  try {
    const auto reloaded =
        detail::ProgramBindingConstructionAuthority::reload_intrinsic(
            binding.canonical_bytes());
    return reloaded->program_binding_id() == binding.program_binding_id() &&
           reloaded->value() == binding.value() &&
           reloaded->normalized_root() == binding.normalized_root();
  } catch (const std::exception&) {
    return false;
  }
}

}  // namespace manyforge::deployment
