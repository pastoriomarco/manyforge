#include <manyforge_core/io_rule_stop_response.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/prepared_motion_evidence_codec.hpp>
#include <manyforge_core/prepared_operation_participant_codec.hpp>
#include <manyforge_core/prepared_participant_stop_plan_codec.hpp>
#include <manyforge_core/prepared_planning_codec.hpp>
#include <manyforge_core/program_frame_identity.hpp>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

namespace manyforge::core {
namespace {

using content_identity::CanonicalValue;
constexpr std::size_t kMaximumRows = 4096U;
constexpr std::size_t kMaximumScopes = 64U;
constexpr std::size_t kMaximumIdentityBytes = 128U;
constexpr double kMaximumSafeInteger = 9'007'199'254'740'991.0;

class Invalid final : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

[[noreturn]] void fail(std::string reason) {
  throw Invalid(std::move(reason));
}

template <class Value>
[[nodiscard]] Result<Value> rejected(std::string reason) {
  return Result<Value>::Err(
      ErrorDTO{"prepared_operation_participant_invalid",
               "prepared operation participant is not an exact closed Prepare product",
               {{"reason", std::move(reason)}}});
}

[[nodiscard]] bool portable(std::string_view value) noexcept {
  if (value.empty() || value.size() > kMaximumIdentityBytes) return false;
  const auto alphanumeric = [](unsigned char character) {
    return (character >= '0' && character <= '9') || (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z');
  };
  if (!alphanumeric(static_cast<unsigned char>(value.front()))) return false;
  return std::all_of(value.begin() + 1, value.end(), [&](char character) {
    return alphanumeric(static_cast<unsigned char>(character)) || character == '.' ||
           character == '_' || character == '-';
  });
}

void require_portable(std::string_view value, std::string reason) {
  if (!portable(value)) fail(std::move(reason));
}

[[nodiscard]] CanonicalValue embedded(const Result<std::string>& encoded,
                                      std::string_view failure_reason) {
  if (!encoded.ok()) {
    auto reason = std::string(failure_reason);
    const auto nested = encoded.error().details.find("reason");
    if (nested != encoded.error().details.end() && !nested->second.empty()) {
      reason.append(".").append(nested->second);
    }
    fail(std::move(reason));
  }
  return content_identity::parse_canonical_json(encoded.value());
}

template <class Id>
[[nodiscard]] CanonicalValue id_array(const std::vector<Id>& values) {
  CanonicalValue::Array rows;
  rows.reserve(values.size());
  for (const auto& value : values) rows.emplace_back(value.value());
  return CanonicalValue(std::move(rows));
}

[[nodiscard]] CanonicalValue model_bindings_value(
    const std::vector<ProgramModelRoleBindingDTO>& values) {
  CanonicalValue::Array rows;
  for (const auto& value : values) {
    rows.emplace_back(CanonicalValue::Object{
        {"model_role_id", CanonicalValue(value.model_role_id.value())},
        {"model_instance_id", CanonicalValue(value.model_instance_id.value())},
        {"model_revision", CanonicalValue(value.model_revision.value())}});
  }
  return CanonicalValue(std::move(rows));
}

[[nodiscard]] CanonicalValue frame_bindings_value(
    const std::vector<ProgramFrameBindingDTO>& values) {
  CanonicalValue::Array rows;
  for (const auto& value : values) {
    CanonicalValue::Object fields{
        {"reference", CanonicalValue(CanonicalValue::Object{
            {"component_id", CanonicalValue(value.reference.component_id.value())},
            {"frame_id", CanonicalValue(value.reference.frame_id.value())}})},
        {"model_instance_id", CanonicalValue(value.model_instance_id.value())},
        {"model_revision", CanonicalValue(value.model_revision.value())},
        {"compiled_frame_name", CanonicalValue(value.compiled_frame_name)}};
    rows.emplace_back(std::move(fields));
  }
  return CanonicalValue(std::move(rows));
}

[[nodiscard]] CanonicalValue motion_endpoint_value(const RuntimeMotionEndpointBindingDTO& value) {
  CanonicalValue::Array bodies;
  for (const auto& body : value.contact_body_bindings) {
    CanonicalValue::Array ids;
    for (const auto& id : body.collision_body_ids) ids.emplace_back(id);
    bodies.emplace_back(CanonicalValue::Object{
        {"body", CanonicalValue(CanonicalValue::Object{
            {"component_id", CanonicalValue(body.body.component_id.value())},
            {"collision_body_id", CanonicalValue(body.body.collision_body_id.value())}})},
        {"model_role_id", body.model_role_id ? CanonicalValue(body.model_role_id->value())
                                             : CanonicalValue(nullptr)},
        {"model_instance_id", CanonicalValue(body.model_instance_id.value())},
        {"model_revision", CanonicalValue(body.model_revision.value())},
        {"collision_body_ids", CanonicalValue(std::move(ids))}});
  }
  return CanonicalValue(
      CanonicalValue::Object{{"target", CanonicalValue(CanonicalValue::Object{
                                 {"component_id", CanonicalValue(value.target.component_id.value())},
                                 {"command_target_id", CanonicalValue(value.target.command_target_id.value())}})},
                             {"frame", frame_bindings_value({value.frame}).as_array().front()},
                             {"contact_body_bindings", CanonicalValue(std::move(bodies))},
                             {"contact_body_role_ids", id_array(value.contact_body_role_ids)}});
}

[[nodiscard]] CanonicalValue provider_value(const PreparedProviderRuntimeOccurrenceDTO& value) {
  return CanonicalValue(CanonicalValue::Object{
      {"provider_instance_id", CanonicalValue(value.provider_instance_id.value())},
      {"provider_manifest_id", CanonicalValue(value.provider_manifest_id.value())},
      {"provider_manifest_revision", CanonicalValue(value.provider_manifest_revision.value())},
      {"runtime_unit_id", CanonicalValue(value.runtime_unit_id.value())},
      {"runtime_epoch", CanonicalValue(value.runtime_epoch.value())}});
}

[[nodiscard]] CanonicalValue capability_provider_value(
    const PreparedCapabilityProviderRuntimeOccurrenceDTO& value) {
  return CanonicalValue(
      CanonicalValue::Object{{"capability_provider_binding_id",
                              CanonicalValue(value.capability_provider_binding_id.value())},
                             {"capability_provider_binding_revision",
                              CanonicalValue(value.capability_provider_binding_revision.value())},
                             {"provider", provider_value(value.provider)}});
}

[[nodiscard]] CanonicalValue interfaces_value(
    const std::vector<ControllerInterfaceInstanceDTO>& values) {
  CanonicalValue::Array rows;
  for (const auto& value : values) {
    rows.emplace_back(
        CanonicalValue::Object{{"resource_id", CanonicalValue(value.resource_id.value())},
                               {"interface_name", CanonicalValue(value.interface_name)}});
  }
  return CanonicalValue(std::move(rows));
}

[[nodiscard]] CanonicalValue endpoint_value(const ControllerLifecycleExpectedEndpointDTO& value) {
  return CanonicalValue(CanonicalValue::Object{
      {"control_binding_id", CanonicalValue(value.control_binding_id.value())},
      {"control_binding_revision", CanonicalValue(value.control_binding_revision.value())},
      {"control_route_id", CanonicalValue(value.control_route_id.value())},
      {"endpoint_role_id", CanonicalValue(value.endpoint_role_id.value())},
      {"logical_endpoint_symbol", CanonicalValue(value.logical_endpoint_symbol)},
      {"is_command_endpoint", CanonicalValue(value.is_command_endpoint)},
      {"is_state_endpoint", CanonicalValue(value.is_state_endpoint)}});
}

[[nodiscard]] CanonicalValue endpoint_contract_value(
    const PreparedParticipantEndpointContractDTO& value) {
  return std::visit(
      [](const auto& selected) -> CanonicalValue {
        using Selected = std::decay_t<decltype(selected)>;
        if constexpr (std::is_same_v<Selected, PreparedParticipantFocusedPortDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"focused_port",
               CanonicalValue(CanonicalValue::Object{
                   {"runtime_unit_id", CanonicalValue(selected.runtime_unit_id.value())}})}});
        } else {
          CanonicalValue::Array rows;
          for (const auto& endpoint : selected.endpoints) rows.push_back(endpoint_value(endpoint));
          return CanonicalValue(CanonicalValue::Object{
              {"selected_route", CanonicalValue(CanonicalValue::Object{
                                     {"endpoints", CanonicalValue(std::move(rows))}})}});
        }
      },
      value);
}

[[nodiscard]] CanonicalValue epochs_value(const std::vector<RuntimeUnitEpochBindingDTO>& values) {
  CanonicalValue::Array rows;
  for (const auto& value : values) {
    rows.emplace_back(
        CanonicalValue::Object{{"runtime_unit_id", CanonicalValue(value.runtime_unit_id.value())},
                               {"runtime_epoch", CanonicalValue(value.runtime_epoch.value())}});
  }
  return CanonicalValue(std::move(rows));
}

[[nodiscard]] CanonicalValue structured_value(const InterruptStructuredValue& value) {
  if (interrupt_structured_value_error(value).has_value()) fail("structured_value_invalid");
  CanonicalValue::Object result;
  for (const auto& [name, scalar] : value) {
    result.emplace_back(name,
                        std::visit([](const auto& item) { return CanonicalValue(item); }, scalar));
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue position_value(const CoordinatePositionDTO& value) {
  return std::visit(
      [](const auto& selected) -> CanonicalValue {
        using Selected = std::decay_t<decltype(selected)>;
        if constexpr (std::is_same_v<Selected, AngularPositionDTO>) {
          return CanonicalValue(
              CanonicalValue::Object{{"coordinate_kind", CanonicalValue("angular_rad")},
                                     {"value_rad", CanonicalValue(selected.value_rad)}});
        } else {
          return CanonicalValue(
              CanonicalValue::Object{{"coordinate_kind", CanonicalValue("linear_m")},
                                     {"value_m", CanonicalValue(selected.value_m)}});
        }
      },
      value);
}

[[nodiscard]] CanonicalValue effort_value(const CoordinateEffortDTO& value) {
  return std::visit(
      [](const auto& selected) -> CanonicalValue {
        using Selected = std::decay_t<decltype(selected)>;
        if constexpr (std::is_same_v<Selected, AngularEffortDTO>) {
          return CanonicalValue(
              CanonicalValue::Object{{"coordinate_kind", CanonicalValue("angular_newton_meter")},
                                     {"value_nm", CanonicalValue(selected.value_nm)}});
        } else {
          return CanonicalValue(
              CanonicalValue::Object{{"coordinate_kind", CanonicalValue("linear_newton")},
                                     {"value_n", CanonicalValue(selected.value_n)}});
        }
      },
      value);
}

[[nodiscard]] CanonicalValue command_value(const GripperPositionCommandValueDTO& command) {
  CanonicalValue::Object fields{{"position", position_value(command.position)}};
  if (command.max_effort) fields.emplace_back("max_effort", effort_value(*command.max_effort));
  return CanonicalValue(
      CanonicalValue::Object{{"gripper_position", CanonicalValue(std::move(fields))}});
}

[[nodiscard]] CanonicalValue capability_payload_value(const PreparedCapabilityCommandDTO& value) {
  CanonicalValue::Array options;
  for (const auto& row : value.validated_options) {
    CanonicalValue::Object option{{"option_id", CanonicalValue(row.option_id.value())}};
    if (const auto* value_s = std::get_if<double>(&row.value)) {
      option.emplace_back("value_s", CanonicalValue(*value_s));
    } else {
      option.emplace_back("value_bool", CanonicalValue(std::get<bool>(row.value)));
    }
    options.emplace_back(std::move(option));
  }
  CanonicalValue::Array resources;
  for (const auto& row : value.resource_bindings) {
    resources.emplace_back(
        CanonicalValue::Object{{"resource_role_id", CanonicalValue(row.resource_role_id.value())},
                               {"resource_id", CanonicalValue(row.resource_id.value())},
                               {"interface_name", CanonicalValue(row.interface_name)}});
  }
  CanonicalValue::Array channels;
  for (const auto& row : value.channel_bindings) {
    channels.emplace_back(
        CanonicalValue::Object{{"channel_role_id", CanonicalValue(row.channel_role_id.value())},
                               {"resource_id", CanonicalValue(row.resource_id.value())},
                               {"interface_name", CanonicalValue(row.interface_name)}});
  }
  CanonicalValue::Array evidence;
  for (const auto requirement : value.stop_evidence_requirements) {
    const auto token = to_string(requirement);
    if (token.empty()) fail("stop_evidence_requirement_invalid");
    evidence.emplace_back(std::string(token));
  }
  if (to_string(value.completion_rule).empty() || to_string(value.confirmation_rule).empty()) {
    fail("command_rule_invalid");
  }
  return CanonicalValue(CanonicalValue::Object{
      {"command_schema_id", CanonicalValue(value.command_schema_id.value())},
      {"command_schema_revision", CanonicalValue(value.command_schema_revision.value())},
      {"command_value", command_value(value.command_value)},
      {"validated_options", CanonicalValue(std::move(options))},
      {"resource_bindings", CanonicalValue(std::move(resources))},
      {"channel_bindings", CanonicalValue(std::move(channels))},
      {"completion_timeout_s", CanonicalValue(value.completion_timeout_s)},
      {"completion_rule", CanonicalValue(std::string(to_string(value.completion_rule)))},
      {"confirmation_rule", CanonicalValue(std::string(to_string(value.confirmation_rule)))},
      {"stop_evidence_requirements", CanonicalValue(std::move(evidence))}});
}

[[nodiscard]] std::string activation_mode_token(MotionInterruptActivationMode value) {
  switch (value) {
    case MotionInterruptActivationMode::kRequireInactiveAtArm:
      return "require_inactive_at_arm";
    case MotionInterruptActivationMode::kFreshEdgeAfterArm:
      return "fresh_edge_after_arm";
    case MotionInterruptActivationMode::kTripIfAlreadyActive:
      return "trip_if_already_active";
    case MotionInterruptActivationMode::kMaintainLevelWithHysteresis:
      return "maintain_level_with_hysteresis";
  }
  fail("interrupt_activation_mode_invalid");
}

[[nodiscard]] std::string node_category_token(MotionInterruptNodeCategory value) {
  switch (value) {
    case MotionInterruptNodeCategory::kGeneric:
      return "generic";
    case MotionInterruptNodeCategory::kPrecisionContact:
      return "precision_contact";
  }
  fail("interrupt_node_category_invalid");
}

[[nodiscard]] CanonicalValue motion_scope_value(
    const PreparedMotionInterruptScopeDTO& value, const ResolvedStopBasePlanDTO& base,
    std::span<const RuntimeResourceBindingDTO> resources,
    std::span<const HardStopResourceLimitDTO> defaults) {
  const auto stop = EncodePreparedParticipantStopPlan(base, value.stop_plan, resources, defaults,
                                                      value.node_category);
  if (!stop.ok()) fail("interrupt_scope_stop_plan_invalid");
  const auto stop_value = content_identity::parse_canonical_json(stop.value());
  const auto* profile = stop_value.find_object_member("participant_stop_profile");
  const bool desired = value.preemption_action == InterruptPreemptionAction::kCompleteMoveOnSignal;
  const bool fault = value.preemption_action == InterruptPreemptionAction::kPreemptTrajectory;
  if ((!desired && !fault) ||
      (desired && (profile == nullptr || !value.stop_profile ||
                   value.stop_plan.participant_stop_profile != value.stop_profile)) ||
      (fault && value.stop_profile))
    fail("interrupt_scope_stop_profile_invalid");
  CanonicalValue::Object fields{
      {"scope_id", CanonicalValue(value.scope_id.value())},
      {"interrupt_policy_id", CanonicalValue(value.interrupt_policy_id.value())},
      {"activation_mode", CanonicalValue(activation_mode_token(value.activation_mode))},
      {"required", CanonicalValue(value.required)},
      {"stop_profile", desired ? *profile : CanonicalValue(nullptr)},
      {"node_category", CanonicalValue(node_category_token(value.node_category))},
      {"metadata", structured_value(value.metadata)},
      {"stop_plan", std::move(stop_value)}};
  if (fault)
    fields.emplace_back("preemption_action", CanonicalValue(std::string("preempt_trajectory")));
  if (value.stop_entry) {
    if (!value.success_through ||
        !VerifyPreparedIoRuleSuccessThrough(value.stop_plan, *value.stop_entry,
                                            *value.success_through)
             .ok() ||
        (fault && *value.success_through != *value.stop_entry))
      fail("interrupt_scope_stop_entry_invalid");
    fields.emplace_back(
        "stop_entry",
        CanonicalValue(CanonicalValue::Object{
            {"resolved_stop_base_plan_id",
             CanonicalValue(value.stop_entry->resolved_stop_base_plan_id.value())},
            {"stop_escalation_stage_id",
             CanonicalValue(value.stop_entry->stop_escalation_stage_id.value())},
            {"stage_index", CanonicalValue(static_cast<double>(value.stop_entry->stage_index))}}));
    fields.emplace_back(
        "success_through",
        CanonicalValue(CanonicalValue::Object{
            {"resolved_stop_base_plan_id",
             CanonicalValue(value.success_through->resolved_stop_base_plan_id.value())},
            {"stop_escalation_stage_id",
             CanonicalValue(value.success_through->stop_escalation_stage_id.value())},
            {"stage_index",
             CanonicalValue(static_cast<double>(value.success_through->stage_index))}}));
  } else if (value.success_through) {
    fail("interrupt_scope_stop_entry_missing");
  }
  if (value.arm_timeout_ns) {
    if (*value.arm_timeout_ns < 0 ||
        static_cast<double>(*value.arm_timeout_ns) > kMaximumSafeInteger)
      fail("interrupt_arm_timeout_invalid");
    fields.emplace_back("arm_timeout_ns",
                        CanonicalValue(static_cast<double>(*value.arm_timeout_ns)));
  }
  return CanonicalValue(std::move(fields));
}

[[nodiscard]] CanonicalValue motion_payload_value(
    const PreparedMotionExecutionDTO& value, const ResolvedStopBasePlanDTO& base,
    std::span<const RuntimeResourceBindingDTO> resources,
    std::span<const HardStopResourceLimitDTO> defaults) {
  const auto* replacement =
      value.plan_request.moving_replacement ? &*value.plan_request.moving_replacement : nullptr;
  CanonicalValue::Array scopes;
  for (const auto& scope : value.interrupt_scopes)
    scopes.push_back(motion_scope_value(scope, base, resources, defaults));
  CanonicalValue::Object fields{
      {"move_id", CanonicalValue(value.move_id.value())},
      {"plan_request", embedded(EncodePreparedPlanRequest(value.plan_request),
                                "prepared_plan_request_encoding_invalid")},
      {"trajectory", embedded(EncodePreparedTrajectory(value.trajectory, replacement),
                              "prepared_trajectory_encoding_invalid")},
      {"start_state", embedded(EncodePreparedControlledState(value.start_state),
                               "prepared_start_state_encoding_invalid")},
      {"planning_evidence",
       embedded(EncodePreparedMotionPlanningEvidence(value.planning_evidence, replacement),
                "prepared_planning_evidence_encoding_invalid")},
      {"validation_evidence",
       embedded(EncodePreparedMotionValidationEvidence(value.validation_evidence),
                "prepared_validation_evidence_encoding_invalid")},
      {"requested_tcp_linear_speed_mps", value.requested_tcp_linear_speed_mps.has_value()
                                             ? CanonicalValue(*value.requested_tcp_linear_speed_mps)
                                             : CanonicalValue(nullptr)},
      {"deployment_maximum_tcp_linear_speed_mps",
       CanonicalValue(value.deployment_maximum_tcp_linear_speed_mps)},
      {"interrupt_scopes", CanonicalValue(std::move(scopes))},
      {"execution_timeout_s", CanonicalValue(value.execution_timeout_s)}};
  if (value.contact_policy) {
    fields.emplace_back(
        "contact_policy",
        CanonicalValue(CanonicalValue::Object{
            {"contact_policy_id", CanonicalValue(value.contact_policy->contact_policy_id.value())},
            {"contact_policy_revision",
             CanonicalValue(value.contact_policy->contact_policy_revision.value())}}));
  }
  return CanonicalValue(std::move(fields));
}

[[nodiscard]] CanonicalValue participant_value(const PreparedOperationParticipantDTO& value,
                                               const ResolvedStopBasePlanDTO& base,
                                               std::span<const RuntimeResourceBindingDTO> resources,
                                               std::span<const HardStopResourceLimitDTO> defaults,
                                               MotionInterruptNodeCategory node_category,
                                               bool include_digest) {
  CanonicalValue::Object fields{
      {"api_version", CanonicalValue(std::string(kPreparedOperationParticipantApiVersion))},
      {"operation_id", CanonicalValue(value.operation_id.value())},
      {"operation_revision", CanonicalValue(static_cast<double>(value.operation_revision.value()))},
      {"participant_id", CanonicalValue(value.participant_id.value())},
      {"program_id", CanonicalValue(value.program_id.value())},
      {"program_revision", CanonicalValue(value.program_revision.value())},
      {"program_binding_id", CanonicalValue(value.program_binding_id.value())},
      {"program_run_id", CanonicalValue(value.program_run_id.value())},
      {"retained_bundle_id", CanonicalValue(value.retained_bundle_id.value())},
      {"graph_revision", CanonicalValue(value.graph_revision.value())},
      {"controllable_component_id", CanonicalValue(value.controllable_component_id.value())},
      {"component_lifecycle_generation",
       CanonicalValue(static_cast<double>(value.component_lifecycle_generation.value()))},
      {"component_type_id", CanonicalValue(value.component_type_id.value())},
      {"component_type_revision", CanonicalValue(value.component_type_revision.value())},
      {"command_target_id", CanonicalValue(value.command_target_id.value())},
      {"capability_id", CanonicalValue(value.capability_id.value())},
      {"capability_definition_revision",
       CanonicalValue(value.capability_definition_revision.value())},
      {"model_bindings", model_bindings_value(value.model_bindings)},
      {"frame_bindings", frame_bindings_value(value.frame_bindings)},
      {"control_domain_id", CanonicalValue(value.binding_identity.control_domain_id.value())},
      {"control_domain_revision", CanonicalValue(value.binding_identity.control_domain_revision.value())},
      {"control_binding_id", CanonicalValue(value.binding_identity.controller.control_binding_id.value())},
      {"control_binding_revision", CanonicalValue(value.binding_identity.controller.control_binding_revision.value())},
      {"control_interface_contract_id",
       CanonicalValue(value.controller().control_interface_contract_id.value())},
      {"control_interface_revision", CanonicalValue(value.controller().control_interface_revision.value())},
      {"controller_topology_id", CanonicalValue(value.controller().controller_topology_id.value())},
      {"topology_revision", CanonicalValue(value.controller().topology_revision.value())},
      {"control_route_id", CanonicalValue(value.controller().control_route_id.value())},
      {"activation_set_id", CanonicalValue(value.controller().activation_set_id.value())},
      {"control_provider", provider_value(value.controller().control_provider)},
      {"resources", id_array(value.resources)},
      {"endpoint_contract", endpoint_contract_value(value.controller().endpoint_contract)},
      {"claimed_command_interfaces", interfaces_value(value.controller().claimed_command_interfaces)},
      {"claimed_state_interfaces", interfaces_value(value.controller().claimed_state_interfaces)},
      {"active_control_configuration_id",
       CanonicalValue(value.controller().active_control_configuration_id.value())},
      {"active_control_configuration_generation",
       CanonicalValue(static_cast<double>(value.controller().active_control_configuration_generation.value()))},
      {"runtime_unit_epochs", epochs_value(value.runtime_unit_epochs)},
      {"stop_evidence_policy_id", CanonicalValue(value.controller().stop_evidence_policy_id.value())},
      {"stop_evidence_policy_revision",
       CanonicalValue(value.controller().stop_evidence_policy_revision.value())},
      {"ordinary_stop_plan",
       embedded(EncodePreparedParticipantStopPlan(base, value.controller().ordinary_stop_plan, resources,
                                                  defaults, node_category),
                "prepared_ordinary_stop_plan_encoding_invalid")}};
  if (value.motion_endpoint)
    fields.emplace_back("motion_endpoint", motion_endpoint_value(*value.motion_endpoint));
  if (value.controller().fault_stop_plan.has_value()) {
    fields.emplace_back("fault_stop_plan", embedded(EncodePreparedParticipantStopPlan(
                                                        base, *value.controller().fault_stop_plan, resources,
                                                        defaults, node_category),
                                                    "prepared_fault_stop_plan_encoding_invalid"));
  }
  if (value.controller().capability_provider)
    fields.emplace_back("capability_provider",
                        capability_provider_value(*value.controller().capability_provider));
  if (std::holds_alternative<PreparedMotionExecutionDTO>(value.payload)) {
    fields.emplace_back(
        "payload", CanonicalValue(CanonicalValue::Object{
                       {"motion_execution",
                        motion_payload_value(std::get<PreparedMotionExecutionDTO>(value.payload),
                                             base, resources, defaults)}}));
  } else {
    fields.emplace_back(
        "payload",
        CanonicalValue(CanonicalValue::Object{
            {"capability_command",
             capability_payload_value(std::get<PreparedCapabilityCommandDTO>(value.payload))}}));
  }
  if (include_digest)
    fields.emplace_back("prepared_contract_digest",
                        CanonicalValue(value.prepared_contract_digest.value()));
  return CanonicalValue(std::move(fields));
}

[[nodiscard]] ContentDigest digest_for(const PreparedOperationParticipantDTO& value,
                                       const ResolvedStopBasePlanDTO& base,
                                       std::span<const RuntimeResourceBindingDTO> resources,
                                       std::span<const HardStopResourceLimitDTO> defaults,
                                       MotionInterruptNodeCategory node_category) {
  return ContentDigest(content_identity::framed_sha256(
      kPreparedOperationParticipantIdentityDomain, kPreparedOperationParticipantApiVersion,
      participant_value(value, base, resources, defaults, node_category, false)));
}

[[nodiscard]] std::vector<ResourceId> binding_resources(
    const RuntimeParticipantBindingDTO& binding) {
  std::vector<ResourceId> result;
  for (const auto& row : binding.resources) result.push_back(row.resource_id);
  std::sort(result.begin(), result.end());
  return result;
}

[[nodiscard]] std::span<const RuntimeResourceBindingDTO> binding_resource_span(
    const RuntimeParticipantBindingDTO& binding) {
  return binding.resources;
}

[[nodiscard]] PreparedProviderRuntimeOccurrenceDTO provider_from(
    const RuntimeControlPathDTO& path) {
  return {path.provider_instance_id, path.provider_manifest_id, path.provider_manifest_revision,
          path.provider_runtime_unit_id, path.provider_runtime_epoch};
}

[[nodiscard]] PreparedCapabilityProviderRuntimeOccurrenceDTO provider_from(
    const RuntimeCapabilityProviderPathDTO& path) {
  return {path.capability_provider_binding_id,
          path.capability_provider_binding_revision,
          {path.provider_instance_id, path.provider_manifest_id, path.provider_manifest_revision,
           path.provider_runtime_unit_id, path.provider_runtime_epoch}};
}

void validate_command_payload(const PreparedCapabilityCommandDTO& command,
                              const RuntimeParticipantBindingDTO& binding) {
  if (!binding.command_schema ||
      command.command_schema_id != binding.command_schema->command_schema_id ||
      command.command_schema_revision != binding.command_schema->command_schema_revision ||
      !std::isfinite(command.completion_timeout_s) || command.completion_timeout_s <= 0.0 ||
      command.stop_evidence_requirements.empty() ||
      command.validated_options.size() > kMaxOperationCommandOptions ||
      command.resource_bindings.size() > kMaximumRows ||
      command.channel_bindings.size() > kMaximumRows) {
    fail("capability_command_contract_invalid");
  }
  const auto schema = builtin_gripper_position_command_schema();
  if (command.command_schema_id != schema.command_schema_id ||
      command.command_schema_revision != schema.command_schema_revision ||
      command.completion_rule != schema.completion_rule ||
      command.confirmation_rule != schema.confirmation_rule ||
      command.stop_evidence_requirements != schema.stop_evidence_requirements) {
    fail("capability_command_schema_invalid");
  }
  if (command.resource_bindings.size() != schema.resource_role_selectors.size() ||
      command.channel_bindings.size() != schema.channel_role_selectors.size() ||
      std::any_of(command.validated_options.begin(), command.validated_options.end(),
                  [&](const auto& row) {
                    return std::none_of(
                        schema.caller_options.begin(), schema.caller_options.end(),
                        [&](const auto& declared) {
                          return declared.option_id == row.option_id &&
                                 std::holds_alternative<bool>(row.value) ==
                                     (declared.value_kind == CommandCallerOptionValueKind::kBool);
                        });
                  }) ||
      std::any_of(
          schema.caller_options.begin(), schema.caller_options.end(), [&](const auto& declared) {
            return declared.required &&
                   std::none_of(
                       command.validated_options.begin(), command.validated_options.end(),
                       [&](const auto& row) { return row.option_id == declared.option_id; });
          })) {
    fail("capability_command_schema_projection_invalid");
  }
  const auto& gripper = command.command_value;
  const auto position_kind = coordinate_kind(gripper.position);
  if (!std::isfinite(std::visit(
          [](const auto& x) {
            if constexpr (requires { x.value_rad; })
              return x.value_rad;
            else
              return x.value_m;
          },
          gripper.position))) {
    fail("capability_command_position_invalid");
  }
  if (binding.resources.empty() ||
      std::any_of(binding.resources.begin(), binding.resources.end(),
                  [&](const auto& row) { return row.coordinate_kind != position_kind; }))
    fail("capability_command_coordinate_kind_invalid");
  if (gripper.max_effort) {
    const double effort = std::visit(
        [](const auto& x) {
          if constexpr (requires { x.value_nm; })
            return x.value_nm;
          else
            return x.value_n;
        },
        *gripper.max_effort);
    if (!std::isfinite(effort) || effort < 0.0 ||
        coordinate_kind(*gripper.max_effort) != position_kind)
      fail("capability_command_effort_invalid");
  }
  if (!std::is_sorted(
          command.validated_options.begin(), command.validated_options.end(),
          [](const auto& lhs, const auto& rhs) { return lhs.option_id < rhs.option_id; }) ||
      std::adjacent_find(command.validated_options.begin(), command.validated_options.end(),
                         [](const auto& lhs, const auto& rhs) {
                           return lhs.option_id == rhs.option_id;
                         }) != command.validated_options.end() ||
      std::any_of(
          command.validated_options.begin(), command.validated_options.end(), [](const auto& row) {
            const auto* value_s = std::get_if<double>(&row.value);
            return row.option_id.empty() ||
                   (value_s != nullptr && (!std::isfinite(*value_s) || *value_s < 0.0));
          }))
    fail("capability_command_options_invalid");
  const auto sorted_resources =
      std::is_sorted(command.resource_bindings.begin(), command.resource_bindings.end(),
                     [](const auto& lhs, const auto& rhs) {
                       return std::tie(lhs.resource_role_id, lhs.resource_id, lhs.interface_name) <
                              std::tie(rhs.resource_role_id, rhs.resource_id, rhs.interface_name);
                     });
  const auto sorted_channels =
      std::is_sorted(command.channel_bindings.begin(), command.channel_bindings.end(),
                     [](const auto& lhs, const auto& rhs) {
                       return std::tie(lhs.channel_role_id, lhs.resource_id, lhs.interface_name) <
                              std::tie(rhs.channel_role_id, rhs.resource_id, rhs.interface_name);
                     });
  if (!sorted_resources || !sorted_channels ||
      std::adjacent_find(command.resource_bindings.begin(), command.resource_bindings.end()) !=
          command.resource_bindings.end() ||
      std::adjacent_find(command.channel_bindings.begin(), command.channel_bindings.end()) !=
          command.channel_bindings.end() ||
      std::any_of(command.resource_bindings.begin(), command.resource_bindings.end(),
                  [&](const auto& row) {
                    return row.resource_role_id.empty() || row.resource_id.empty() ||
                           !portable(row.interface_name) ||
                           std::none_of(binding.resources.begin(), binding.resources.end(),
                                        [&](const auto& source) {
                                          return source.resource_role_id == row.resource_role_id &&
                                                 source.resource_id == row.resource_id;
                                        });
                  }) ||
      std::any_of(
          command.channel_bindings.begin(), command.channel_bindings.end(), [&](const auto& row) {
            return row.channel_role_id.empty() || row.resource_id.empty() ||
                   !portable(row.interface_name) ||
                   std::none_of(
                       binding.resources.begin(), binding.resources.end(),
                       [&](const auto& source) { return source.resource_id == row.resource_id; });
          }))
    fail("capability_command_binding_invalid");
}

void validate_moving_validation_join(const PreparedMotionExecutionDTO& motion) {
  const auto& input = motion.plan_request.moving_replacement;
  const auto& result = motion.validation_evidence.noncollision_result;
  if (input.has_value() != result.moving_replacement.has_value())
    fail("moving_replacement_validation_context_mismatch");
  if (!input) return;
  if (!result.trajectory_metrics || motion.trajectory.waypoints.empty() ||
      !motion.plan_request.speed_profile.max_tcp_linear_speed_mps)
    fail("moving_replacement_metrics_required");
  const auto& whole = *result.trajectory_metrics;
  const auto& prefix = result.moving_replacement->unchanged_prefix;
  const auto& future = result.moving_replacement->changed_future;
  const double seam = input->divergence_cursor_s - input->source_cursor_s;
  const double end = motion.trajectory.waypoints.back().time_from_start_s;
  const auto width = motion.trajectory.joint_names.size();
  const auto complete = [width, &whole](const auto& metrics) {
    const auto finite_nonnegative = [](double value) {
      return std::isfinite(value) && value >= 0.0;
    };
    return metrics.peak_abs_joint_velocity_rad_s.size() == width &&
           metrics.peak_abs_joint_acceleration_rad_s2.size() ==
               whole.peak_abs_joint_acceleration_rad_s2.size() &&
           std::all_of(metrics.peak_abs_joint_velocity_rad_s.begin(),
                       metrics.peak_abs_joint_velocity_rad_s.end(), finite_nonnegative) &&
           std::all_of(metrics.peak_abs_joint_acceleration_rad_s2.begin(),
                       metrics.peak_abs_joint_acceleration_rad_s2.end(), finite_nonnegative) &&
           metrics.peak_tcp_linear_speed_mps &&
           finite_nonnegative(*metrics.peak_tcp_linear_speed_mps);
  };
  if (!std::isfinite(seam) || !(seam > 0.0 && seam < end) || prefix.trajectory_duration_s != seam ||
      future.trajectory_duration_s != end - seam || whole.trajectory_duration_s != end ||
      !complete(prefix) || !complete(future) || !whole.peak_tcp_linear_speed_mps ||
      !input->source_speed_profile.max_tcp_linear_speed_mps ||
      *prefix.peak_tcp_linear_speed_mps >
          *input->source_speed_profile.max_tcp_linear_speed_mps + 1.0e-8 ||
      *future.peak_tcp_linear_speed_mps >
          *motion.plan_request.speed_profile.max_tcp_linear_speed_mps + 1.0e-8 ||
      *whole.peak_tcp_linear_speed_mps !=
          std::max(*prefix.peak_tcp_linear_speed_mps, *future.peak_tcp_linear_speed_mps))
    fail("moving_replacement_metrics_join_invalid");
  for (std::size_t j = 0; j < width; ++j) {
    if (j >= whole.peak_abs_joint_velocity_rad_s.size() ||
        whole.peak_abs_joint_velocity_rad_s[j] != std::max(prefix.peak_abs_joint_velocity_rad_s[j],
                                                           future.peak_abs_joint_velocity_rad_s[j]))
      fail("moving_replacement_metrics_join_invalid");
  }
  for (std::size_t j = 0; j < whole.peak_abs_joint_acceleration_rad_s2.size(); ++j) {
    if (whole.peak_abs_joint_acceleration_rad_s2[j] !=
        std::max(prefix.peak_abs_joint_acceleration_rad_s2[j],
                 future.peak_abs_joint_acceleration_rad_s2[j]))
      fail("moving_replacement_metrics_join_invalid");
  }
}

void validate_motion_payload(const PreparedMotionExecutionDTO& motion,
                             const PreparedOperationParticipantDTO& enclosing,
                             const RuntimeParticipantBindingDTO& binding) {
  validate_moving_validation_join(motion);
  if (binding.motion_endpoint) {
    // The resolved TCP must retain the exact captured frame owner as well as
    // its physical frame key, including for a frame on a mounted tool.
    const auto* tcp = std::get_if<ResolvedComponentModelFrameDTO>(&motion.plan_request.tcp_frame);
    if (tcp == nullptr ||
        tcp->controllable_component_id != binding.motion_endpoint->frame.reference.component_id ||
        !same_program_frame(ProgramFrameBindingDTO{{tcp->controllable_component_id, tcp->frame_role_id}, tcp->model_instance_id,
                                                       tcp->model_revision,
                                                       tcp->compiled_frame_name},
                            binding.motion_endpoint->frame))
      fail("prepared_motion_endpoint_frame_mismatch");
  }
  const auto* cartesian_path = std::get_if<CartesianPathDTO>(&motion.plan_request.motion);
  if (!motion.trajectory.cartesian_waypoint_times_s.empty()) {
    if (cartesian_path == nullptr ||
        cartesian_path->waypoints.size() != motion.trajectory.cartesian_waypoint_times_s.size())
      fail("prepared_cartesian_waypoint_milestones_mismatch");
  }
  const auto metrics_valid = [](const std::vector<double>& values) {
    return std::all_of(values.begin(), values.end(),
                       [](const double item) { return std::isfinite(item) && item >= 0.0; });
  };
  const auto* trajectory_metrics =
      motion.validation_evidence.noncollision_result.trajectory_metrics
          ? &*motion.validation_evidence.noncollision_result.trajectory_metrics
          : nullptr;
  const auto trajectory_width = motion.trajectory.joint_names.size();
  if (!std::isfinite(motion.execution_timeout_s) || motion.execution_timeout_s <= 0.0 ||
      motion.move_id.empty() || motion.plan_request.move_id != motion.move_id ||
      motion.plan_request.operation_id != enclosing.operation_id ||
      motion.plan_request.participant_id != enclosing.participant_id ||
      motion.plan_request.program_id != enclosing.program_id ||
      motion.plan_request.program_revision != enclosing.program_revision ||
      motion.plan_request.program_binding_id != enclosing.program_binding_id ||
      motion.plan_request.program_run_id != enclosing.program_run_id ||
      motion.plan_request.controllable_component_id != enclosing.controllable_component_id ||
      motion.plan_request.component_type_id != enclosing.component_type_id ||
      motion.plan_request.component_type_revision != enclosing.component_type_revision ||
      motion.plan_request.command_target_id != enclosing.command_target_id ||
      motion.plan_request.capability_id != enclosing.capability_id ||
      motion.plan_request.capability_definition_revision !=
          enclosing.capability_definition_revision ||
      motion.plan_request.control_domain_id != enclosing.binding_identity.control_domain_id ||
      motion.plan_request.control_domain_revision != enclosing.binding_identity.control_domain_revision ||
      motion.plan_request.control_binding_id != enclosing.binding_identity.controller.control_binding_id ||
      motion.plan_request.control_binding_revision != enclosing.binding_identity.controller.control_binding_revision ||
      motion.plan_request.control_interface_contract_id !=
          enclosing.controller().control_interface_contract_id ||
      motion.plan_request.control_interface_revision != enclosing.controller().control_interface_revision ||
      motion.plan_request.controller_topology_id != enclosing.controller().controller_topology_id ||
      motion.plan_request.topology_revision != enclosing.controller().topology_revision ||
      motion.plan_request.control_route_id != enclosing.controller().control_route_id ||
      motion.plan_request.activation_set_id != enclosing.controller().activation_set_id ||
      motion.plan_request.active_control_configuration_id !=
          enclosing.controller().active_control_configuration_id ||
      motion.plan_request.active_control_configuration_generation !=
          enclosing.controller().active_control_configuration_generation ||
      motion.plan_request.provider_instance_id != enclosing.controller().control_provider.provider_instance_id ||
      motion.plan_request.provider_manifest_id != enclosing.controller().control_provider.provider_manifest_id ||
      motion.plan_request.provider_manifest_revision !=
          enclosing.controller().control_provider.provider_manifest_revision ||
      motion.plan_request.provider_runtime_epoch != enclosing.controller().control_provider.runtime_epoch ||
      motion.plan_request.resources != enclosing.resources ||
      motion.start_state != motion.plan_request.start_state ||
      !std::isfinite(motion.deployment_maximum_tcp_linear_speed_mps) ||
      motion.deployment_maximum_tcp_linear_speed_mps <= 0.0 ||
      (motion.requested_tcp_linear_speed_mps.has_value() &&
       (!std::isfinite(*motion.requested_tcp_linear_speed_mps) ||
        *motion.requested_tcp_linear_speed_mps <= 0.0)) ||
      !motion.plan_request.speed_profile.max_tcp_linear_speed_mps.has_value() ||
      !std::isfinite(*motion.plan_request.speed_profile.max_tcp_linear_speed_mps) ||
      *motion.plan_request.speed_profile.max_tcp_linear_speed_mps <= 0.0 ||
      *motion.plan_request.speed_profile.max_tcp_linear_speed_mps >
          motion.deployment_maximum_tcp_linear_speed_mps ||
      *motion.plan_request.speed_profile.max_tcp_linear_speed_mps !=
          std::min(motion.requested_tcp_linear_speed_mps.value_or(
                       motion.deployment_maximum_tcp_linear_speed_mps),
                   motion.deployment_maximum_tcp_linear_speed_mps) ||
      trajectory_metrics == nullptr || motion.trajectory.waypoints.empty() ||
      motion.trajectory.cartesian_fidelity_metrics !=
          trajectory_metrics->cartesian_fidelity_metrics ||
      (trajectory_metrics->cartesian_fidelity_metrics.has_value() &&
       (cartesian_path == nullptr ||
        trajectory_metrics->cartesian_fidelity_metrics->requested_translation_tolerance_m !=
            motion.plan_request.tolerances.translation_tol_m ||
        trajectory_metrics->cartesian_fidelity_metrics->requested_rotation_tolerance_rad !=
            motion.plan_request.tolerances.rotation_tol_rad)) ||
      !std::isfinite(trajectory_metrics->trajectory_duration_s) ||
      trajectory_metrics->trajectory_duration_s !=
          motion.trajectory.waypoints.back().time_from_start_s ||
      trajectory_metrics->peak_abs_joint_velocity_rad_s.size() != trajectory_width ||
      !metrics_valid(trajectory_metrics->peak_abs_joint_velocity_rad_s) ||
      (!trajectory_metrics->peak_abs_joint_acceleration_rad_s2.empty() &&
       trajectory_metrics->peak_abs_joint_acceleration_rad_s2.size() != trajectory_width) ||
      !metrics_valid(trajectory_metrics->peak_abs_joint_acceleration_rad_s2) ||
      !trajectory_metrics->peak_tcp_linear_speed_mps.has_value() ||
      !std::isfinite(*trajectory_metrics->peak_tcp_linear_speed_mps) ||
      *trajectory_metrics->peak_tcp_linear_speed_mps < 0.0 ||
      (!motion.plan_request.moving_replacement &&
       *trajectory_metrics->peak_tcp_linear_speed_mps >
           *motion.plan_request.speed_profile.max_tcp_linear_speed_mps + 1.0e-8) ||
      motion.plan_request.retained_collision_world ||
      motion.plan_request.retained_collision_policy ||
      motion.plan_request.retained_clearance_margins) {
    fail("motion_payload_join_invalid");
  }
  std::vector<PlannerRuntimeUnitBindingDTO> expected_runtime_units;
  expected_runtime_units.reserve(enclosing.runtime_unit_epochs.size());
  for (const auto& row : enclosing.runtime_unit_epochs)
    expected_runtime_units.push_back({row.runtime_unit_id, row.runtime_epoch});
  const auto planner_path = std::find_if(
      binding.supporting_capability_provider_paths.begin(),
      binding.supporting_capability_provider_paths.end(), [&](const auto& path) {
        return path.capability_id == motion.plan_request.planner_capability_binding.capability_id &&
               path.capability_definition_revision ==
                   motion.plan_request.planner_capability_binding.capability_definition_revision;
      });
  if (motion.plan_request.runtime_unit_bindings != expected_runtime_units ||
      planner_path == binding.supporting_capability_provider_paths.end() ||
      motion.plan_request.planner_capability_binding.capability_provider_binding_id !=
          planner_path->capability_provider_binding_id ||
      motion.plan_request.planner_capability_binding.capability_provider_binding_revision !=
          planner_path->capability_provider_binding_revision ||
      motion.plan_request.planner_capability_binding.provider_instance_id !=
          planner_path->provider_instance_id ||
      motion.plan_request.planner_capability_binding.provider_manifest_id !=
          planner_path->provider_manifest_id ||
      motion.plan_request.planner_capability_binding.provider_manifest_revision !=
          planner_path->provider_manifest_revision ||
      motion.plan_request.planner_capability_binding.provider_runtime_unit_id !=
          planner_path->provider_runtime_unit_id ||
      motion.plan_request.planner_capability_binding.provider_runtime_epoch !=
          planner_path->provider_runtime_epoch) {
    fail("motion_planner_runtime_join_invalid");
  }
  const auto* replacement =
      motion.plan_request.moving_replacement ? &*motion.plan_request.moving_replacement : nullptr;
  auto trajectory = EncodePreparedTrajectory(motion.trajectory, replacement);
  auto planned = motion.planning_evidence.planning_result.trajectory
                     ? EncodePreparedTrajectory(
                           *motion.planning_evidence.planning_result.trajectory, replacement)
                     : Result<std::string>::Err(ErrorDTO{});
  if (!trajectory.ok() || !planned.ok() || trajectory.value() != planned.value() ||
      motion.planning_evidence.planning_attempt_index !=
          motion.plan_request.planning_attempt_index ||
      motion.planning_evidence.planner_capability_binding !=
          motion.plan_request.planner_capability_binding ||
      motion.planning_evidence.planning_result.projection_revision !=
          motion.plan_request.projection_binding.projection_revision) {
    fail("motion_planning_evidence_join_invalid");
  }
  if (motion.interrupt_scopes.size() > kMaximumScopes) fail("interrupt_scopes_invalid");
  std::set<std::string> scope_ids;
  for (const auto& scope : motion.interrupt_scopes) {
    const bool desired =
        scope.preemption_action == InterruptPreemptionAction::kCompleteMoveOnSignal;
    const bool fault = scope.preemption_action == InterruptPreemptionAction::kPreemptTrajectory;
    if ((scope.stop_entry &&
         !VerifyPreparedIoRuleStopEntry(scope.stop_plan, *scope.stop_entry).ok()) ||
        (scope.stop_entry.has_value() != scope.success_through.has_value()) ||
        (scope.stop_entry && !VerifyPreparedIoRuleSuccessThrough(scope.stop_plan, *scope.stop_entry,
                                                                 *scope.success_through)
                                  .ok()) ||
        (fault && scope.success_through && *scope.success_through != *scope.stop_entry) ||
        (!desired && !fault) || !scope.required || scope.scope_id.value().empty() ||
        scope.interrupt_policy_id.value().empty() ||
        !scope_ids.insert(scope.scope_id.value()).second ||
        (desired && (!scope.stop_profile ||
                     scope.stop_plan.participant_stop_profile_purpose !=
                         ParticipantStopProfilePurposeDTO::kMotionInterruptScope ||
                     scope.stop_plan.motion_interrupt_scope_id != scope.scope_id.value() ||
                     scope.stop_plan.participant_stop_profile != scope.stop_profile)) ||
        (fault &&
         (scope.stop_profile || scope.stop_plan != enclosing.controller().ordinary_stop_plan)) ||
        scope.stop_plan.operation_id != enclosing.operation_id ||
        scope.stop_plan.operation_revision != enclosing.operation_revision ||
        scope.stop_plan.participant_id != enclosing.participant_id ||
        scope.stop_plan.program_id != enclosing.program_id ||
        scope.stop_plan.program_revision != enclosing.program_revision ||
        scope.stop_plan.program_binding_id != enclosing.program_binding_id ||
        scope.stop_plan.program_run_id != enclosing.program_run_id)
      fail("motion_interrupt_scope_invalid");
  }
}

}  // namespace

Result<PreparedOperationParticipantDTO> MakePreparedOperationParticipant(
    PreparedOperationParticipantDTO candidate, const RuntimeParticipantBinding& sealed_binding,
    const RuntimeParticipantStopReadiness& sealed_readiness,
    std::span<const HardStopResourceLimitDTO> retained_default_limits,
    MotionInterruptNodeCategory node_category) {
  try {
    const auto& binding = sealed_binding.value();
    const auto& readiness = sealed_readiness.value();
    if (!binding.control_path) fail("control_path_required");
    const auto& path = *binding.control_path;
    require_portable(candidate.operation_id.value(), "operation_id_invalid");
    if (!candidate.operation_revision.nonzero() ||
        static_cast<double>(candidate.operation_revision.value()) > kMaximumSafeInteger)
      fail("operation_revision_invalid");
    require_portable(candidate.participant_id.value(), "participant_id_invalid");
    require_portable(candidate.program_run_id.value(), "program_run_id_invalid");

    candidate.program_id = binding.program_id;
    candidate.program_revision = binding.program_revision;
    candidate.program_binding_id = binding.program_binding_id;
    candidate.retained_bundle_id = binding.retained_bundle_id;
    candidate.graph_revision = binding.graph_revision;
    candidate.controllable_component_id = binding.controllable_component_id;
    candidate.component_lifecycle_generation = readiness.component_lifecycle_generation;
    candidate.component_type_id = binding.component_type_id;
    candidate.component_type_revision = binding.component_type_revision;
    candidate.command_target_id = binding.command_target_id;
    candidate.capability_id = binding.capability_id;
    candidate.capability_definition_revision = binding.capability_definition_revision;
    candidate.model_bindings = binding.model_bindings;
    candidate.frame_bindings = binding.frame_bindings;
    candidate.motion_endpoint = binding.motion_endpoint;
    auto binding_identity = runtime_participant_binding_identity(binding);
    if (!binding_identity.ok()) return Result<PreparedOperationParticipantDTO>::Err(binding_identity.error());
    candidate.binding_identity = std::move(binding_identity.value());
    candidate.controller().control_interface_contract_id = path.control_interface_contract_id;
    candidate.controller().control_interface_revision = path.control_interface_revision;
    candidate.controller().controller_topology_id = path.controller_topology_id;
    candidate.controller().topology_revision = path.topology_revision;
    candidate.controller().control_route_id = path.control_route_id;
    candidate.controller().activation_set_id = path.activation_set_id;
    candidate.controller().control_provider = provider_from(path);
    candidate.controller().capability_provider =
        binding.capability_provider_path
            ? std::optional(provider_from(*binding.capability_provider_path))
            : std::nullopt;
    candidate.resources = binding_resources(binding);
    candidate.controller().active_control_configuration_id = path.active_control_configuration_id;
    candidate.controller().active_control_configuration_generation =
        path.active_control_configuration_generation;
    candidate.runtime_unit_epochs = binding.runtime_unit_epochs;
    candidate.controller().stop_evidence_policy_id = path.stop_evidence_policy_id;
    candidate.controller().stop_evidence_policy_revision = path.stop_evidence_policy_revision;

    const auto expected_binding = std::find_if(
        readiness.expected_controller_domain.expected_bindings.begin(),
        readiness.expected_controller_domain.expected_bindings.end(), [&](const auto& row) {
          return row.control_binding_id == path.control_binding_id &&
                 row.control_binding_revision == path.control_binding_revision;
        });
    if (expected_binding == readiness.expected_controller_domain.expected_bindings.end())
      fail("expected_interface_contract_missing");
    candidate.controller().claimed_command_interfaces = expected_binding->claimed_command_interfaces;
    candidate.controller().claimed_state_interfaces = expected_binding->claimed_state_interfaces;
    const auto& nominal =
        readiness.resolved_stop_base_plan.escalation_stages.front().command_target;
    if (nominal.endpoint_role_id) {
      PreparedParticipantSelectedRouteDTO selected;
      for (const auto& endpoint : readiness.expected_controller_domain.expected_endpoints) {
        if (endpoint.control_binding_id == path.control_binding_id &&
            endpoint.control_binding_revision == path.control_binding_revision &&
            endpoint.control_route_id == path.control_route_id)
          selected.endpoints.push_back(endpoint);
      }
      if (selected.endpoints.empty()) fail("selected_route_endpoints_missing");
      candidate.controller().endpoint_contract = std::move(selected);
    } else {
      const auto& receiver =
          std::holds_alternative<PreparedCapabilityCommandDTO>(candidate.payload) &&
                  candidate.controller().capability_provider
              ? candidate.controller().capability_provider->provider
              : candidate.controller().control_provider;
      candidate.controller().endpoint_contract = PreparedParticipantFocusedPortDTO{receiver.runtime_unit_id};
    }

    if (candidate.controller().ordinary_stop_plan.participant_stop_profile_purpose !=
            ParticipantStopProfilePurposeDTO::kOrdinary &&
        candidate.controller().ordinary_stop_plan.participant_stop_profile_purpose !=
            ParticipantStopProfilePurposeDTO::kOperator) {
      fail("ordinary_stop_plan_purpose_invalid");
    }
    PreparedParticipantStopPlanContextDTO ordinary_context{
        candidate.operation_id,
        candidate.operation_revision,
        candidate.participant_id,
        candidate.program_id,
        candidate.program_revision,
        candidate.program_binding_id,
        candidate.program_run_id,
        candidate.controller().ordinary_stop_plan.participant_stop_profile,
        candidate.controller().ordinary_stop_plan.participant_stop_profile_purpose,
        std::nullopt,
        node_category};
    auto ordinary =
        MakePreparedParticipantStopPlan(readiness.resolved_stop_base_plan, std::move(ordinary_context),
                                        binding_resource_span(binding), retained_default_limits);
    if (!ordinary.ok()) fail("ordinary_stop_plan_make_failed");
    candidate.controller().ordinary_stop_plan = std::move(ordinary.value());

    if (candidate.controller().fault_stop_plan.has_value()) {
      const auto& fault = *candidate.controller().fault_stop_plan;
      if (!std::holds_alternative<PreparedMotionExecutionDTO>(candidate.payload) ||
          fault.operation_id != candidate.operation_id ||
          fault.operation_revision != candidate.operation_revision ||
          fault.participant_id != candidate.participant_id ||
          fault.program_id != candidate.program_id ||
          fault.program_revision != candidate.program_revision ||
          fault.program_binding_id != candidate.program_binding_id ||
          fault.program_run_id != candidate.program_run_id ||
          fault.participant_stop_profile_purpose != ParticipantStopProfilePurposeDTO::kFault ||
          fault.motion_interrupt_scope_id.has_value() ||
          !fault.participant_stop_profile.has_value() ||
          !VerifyPreparedParticipantStopPlan(readiness.resolved_stop_base_plan, fault,
                                             binding_resource_span(binding),
                                             retained_default_limits, node_category)
               .ok()) {
        fail("fault_stop_plan_invalid");
      }
    }

    if (auto* command = std::get_if<PreparedCapabilityCommandDTO>(&candidate.payload)) {
      validate_command_payload(*command, binding);
    } else {
      validate_motion_payload(std::get<PreparedMotionExecutionDTO>(candidate.payload), candidate,
                              binding);
    }
    candidate.prepared_contract_digest =
        digest_for(candidate, readiness.resolved_stop_base_plan, binding_resource_span(binding),
                   retained_default_limits, node_category);
    const auto bytes = content_identity::canonical_json(participant_value(
        candidate, readiness.resolved_stop_base_plan, binding_resource_span(binding),
        retained_default_limits, node_category, true));
    if (bytes.size() > content_identity::kMaximumCanonicalJsonBytes)
      fail("canonical_byte_bound_exceeded");
    return Result<PreparedOperationParticipantDTO>::Ok(std::move(candidate));
  } catch (const std::exception& error) {
    return rejected<PreparedOperationParticipantDTO>(error.what());
  }
}

Result<void> VerifyPreparedOperationParticipant(
    const PreparedOperationParticipantDTO& value, const RuntimeParticipantBinding& binding,
    const RuntimeParticipantStopReadiness& readiness,
    std::span<const HardStopResourceLimitDTO> retained_default_limits,
    MotionInterruptNodeCategory node_category) {
  auto expected = MakePreparedOperationParticipant(value, binding, readiness,
                                                   retained_default_limits, node_category);
  if (!expected.ok()) return Result<void>::Err(expected.error());
  try {
    const auto& binding_value = binding.value();
    const auto& readiness_value = readiness.value();
    const auto actual = content_identity::canonical_json(participant_value(
        value, readiness_value.resolved_stop_base_plan, binding_resource_span(binding_value),
        retained_default_limits, node_category, true));
    const auto rebuilt = content_identity::canonical_json(participant_value(
        expected.value(), readiness_value.resolved_stop_base_plan,
        binding_resource_span(binding_value), retained_default_limits, node_category, true));
    if (actual != rebuilt) fail("prepared_participant_referent_mismatch");
    return Result<void>::Ok();
  } catch (const std::exception& error) {
    return rejected<void>(error.what());
  }
}

Result<std::string> EncodePreparedOperationParticipant(
    const PreparedOperationParticipantDTO& value, const RuntimeParticipantBinding& binding,
    const RuntimeParticipantStopReadiness& readiness,
    std::span<const HardStopResourceLimitDTO> retained_default_limits,
    MotionInterruptNodeCategory node_category) {
  const auto verified = VerifyPreparedOperationParticipant(
      value, binding, readiness, retained_default_limits, node_category);
  if (!verified.ok()) return Result<std::string>::Err(verified.error());
  try {
    return Result<std::string>::Ok(content_identity::canonical_json(participant_value(
        value, readiness.value().resolved_stop_base_plan, binding_resource_span(binding.value()),
        retained_default_limits, node_category, true)));
  } catch (const std::exception& error) {
    return rejected<std::string>(error.what());
  }
}

}  // namespace manyforge::core
