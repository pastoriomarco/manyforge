#include <algorithm>
#include <manyforge_core/resource_slot_table.hpp>
#include <manyforge_core/runtime_participant_registry.hpp>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace manyforge::core {
namespace {

[[nodiscard]] Result<RuntimeParticipantBinding> reject(std::string message, std::string reason) {
  return Result<RuntimeParticipantBinding>::Err(
      ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)),
               std::move(message),
               {{"reason", std::move(reason)}}});
}

[[nodiscard]] bool same_registration_key(const RuntimeParticipantBindingDTO& lhs,
                                         const RuntimeParticipantBindingDTO& rhs) noexcept {
  return lhs.program_binding_id == rhs.program_binding_id &&
         lhs.component_role_id == rhs.component_role_id &&
         lhs.target_role_id == rhs.target_role_id && lhs.capability_id == rhs.capability_id &&
         (!lhs.io_output_binding || !rhs.io_output_binding ||
          lhs.io_output_binding->resource_role_id == rhs.io_output_binding->resource_role_id);
}

[[nodiscard]] const ProgramFrameRoleBindingDTO* find_frame(
    const RuntimeParticipantBindingDTO& binding, const FrameRoleId& role) noexcept {
  const auto found = std::find_if(
      binding.frame_bindings.begin(), binding.frame_bindings.end(),
      [&](const ProgramFrameRoleBindingDTO& candidate) { return candidate.frame_role_id == role; });
  return found == binding.frame_bindings.end() ? nullptr : &*found;
}

[[nodiscard]] Result<RuntimeParticipantBinding> validate_payload_binding(
    const OperationParticipantIntentDTO& participant, const RuntimeParticipantBinding& binding) {
  const auto& runtime = binding.value();
  return std::visit(
      [&](const auto& payload) -> Result<RuntimeParticipantBinding> {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, MotionParticipantIntentDTO>) {
          if (!runtime.control_path.has_value() || runtime.model_groups.empty() ||
              runtime.resources.empty() ||
              find_frame(runtime, payload.base_frame_role_id) == nullptr ||
              find_frame(runtime, payload.tcp_frame_role_id) == nullptr) {
            return reject("motion participant lacks an exact runtime mapping",
                          "runtime_motion_mapping_incomplete");
          }
          if (std::any_of(runtime.resources.begin(), runtime.resources.end(),
                          [](const RuntimeResourceBindingDTO& resource) {
                            return resource.coordinate_kind != CoordinateKind::kAngular;
                          })) {
            return reject("motion participant selects a non-angular trajectory resource",
                          "runtime_motion_resource_not_angular");
          }
          return Result<RuntimeParticipantBinding>::Ok(binding);
        } else {
          if (const auto* output = std::get_if<IoSignalWriteCommandValueDTO>(&payload.command_value)) {
            if (!runtime.io_output_binding || runtime.control_path || !runtime.command_schema ||
                runtime.command_schema->command_schema_id != payload.command_schema_id ||
                runtime.command_schema->command_schema_revision != payload.command_schema_revision ||
                runtime.io_output_binding->resource_role_id != output->resource_role_id) {
              return reject("output command lacks its exact sealed provider channel and schema",
                            "runtime_io_output_mapping_incomplete");
            }
            return Result<RuntimeParticipantBinding>::Ok(binding);
          }
          if (runtime.io_output_binding || !runtime.control_path.has_value() || !runtime.command_schema.has_value() ||
              runtime.command_schema->command_schema_id != payload.command_schema_id ||
              runtime.command_schema->command_schema_revision != payload.command_schema_revision ||
              runtime.resources.empty()) {
            return reject("capability command lacks its exact runtime schema or path",
                          "runtime_command_mapping_incomplete");
          }
          const auto& command = std::get<GripperPositionCommandValueDTO>(payload.command_value);
          const auto kind = coordinate_kind(command.position);
          if (std::any_of(runtime.resources.begin(), runtime.resources.end(),
                          [&](const RuntimeResourceBindingDTO& resource) {
                            return resource.coordinate_kind != kind;
                          })) {
            return reject("capability command coordinate unit differs from its resources",
                          "runtime_command_resource_unit_mismatch");
          }
          return Result<RuntimeParticipantBinding>::Ok(binding);
        }
      },
      participant.payload);
}

}  // namespace

RuntimeParticipantRegistry::RuntimeParticipantRegistry(
    std::vector<RuntimeParticipantBinding> bindings)
    : bindings_(std::move(bindings)) {
  if (bindings_.size() > kMaxOperationParticipants) {
    throw std::invalid_argument("runtime participant registry exceeds its fixed bound");
  }
  for (std::size_t index = 0; index < bindings_.size(); ++index) {
    const auto& value = bindings_[index].value();
    for (std::size_t other = index + 1U; other < bindings_.size(); ++other) {
      if (same_registration_key(value, bindings_[other].value())) {
        throw std::invalid_argument("runtime participant registry contains an ambiguous exact key");
      }
    }
    for (const auto& resource : value.resources) {
      resource_inventory_.push_back(resource.resource_id);
    }
    if (value.io_output_binding)
      resource_inventory_.push_back(*value.io_output_binding->signal.output_resource);
  }
  std::sort(resource_inventory_.begin(), resource_inventory_.end());
  resource_inventory_.erase(std::unique(resource_inventory_.begin(), resource_inventory_.end()),
                            resource_inventory_.end());
  if (resource_inventory_.size() > kMaxKernelResourceSlots) {
    throw std::invalid_argument(
        "runtime participant registry exceeds its bounded resource inventory");
  }
}

Result<RuntimeParticipantBinding> RuntimeParticipantRegistry::resolve(
    const OperationIntentDTO& operation, std::size_t participant_index) const {
  auto operation_validation = validate_operation_intent(operation);
  if (!operation_validation.ok()) {
    return Result<RuntimeParticipantBinding>::Err(operation_validation.error());
  }
  if (participant_index >= operation.participants.size()) {
    return reject("operation participant index is outside the request",
                  "operation_participant_index_invalid");
  }
  const auto& participant = operation.participants[participant_index];
  const RuntimeParticipantBinding* match = nullptr;
  for (const auto& binding : bindings_) {
    const auto& value = binding.value();
    if (value.program_id != operation.program_id ||
        value.program_revision != operation.program_revision ||
        value.program_binding_id != operation.program_binding_id ||
        value.retained_bundle_id != operation.expected_retained_bundle_id ||
        value.graph_revision != operation.expected_graph_revision ||
        value.component_role_id != participant.component_role_id ||
        value.target_role_id != participant.target_role_id ||
        value.capability_id != participant.required_capability_id) {
      continue;
    }
    if (const auto* command = std::get_if<CapabilityCommandIntentDTO>(&participant.payload)) {
      if (const auto* output = std::get_if<IoSignalWriteCommandValueDTO>(&command->command_value)) {
        if (!value.io_output_binding ||
            value.io_output_binding->resource_role_id != output->resource_role_id) continue;
      }
    }
    if (match != nullptr) {
      return reject("operation participant resolution is ambiguous",
                    "runtime_participant_ambiguous");
    }
    match = &binding;
  }
  if (match == nullptr) {
    return reject("operation participant has no exact active runtime binding",
                  "runtime_participant_unavailable");
  }
  return validate_resolved(operation, participant_index, *match);
}

Result<RuntimeParticipantBinding> RuntimeParticipantRegistry::validate_resolved(
    const OperationIntentDTO& operation, const std::size_t participant_index,
    const RuntimeParticipantBinding& binding) {
  auto operation_validation = validate_operation_intent(operation);
  if (!operation_validation.ok()) {
    return Result<RuntimeParticipantBinding>::Err(operation_validation.error());
  }
  if (participant_index >= operation.participants.size()) {
    return reject("operation participant index is outside the request",
                  "operation_participant_index_invalid");
  }
  const auto& participant = operation.participants[participant_index];
  const auto& value = binding.value();
  if (value.program_id != operation.program_id ||
      value.program_revision != operation.program_revision ||
      value.program_binding_id != operation.program_binding_id ||
      value.retained_bundle_id != operation.expected_retained_bundle_id ||
      value.graph_revision != operation.expected_graph_revision ||
      value.component_role_id != participant.component_role_id ||
      value.target_role_id != participant.target_role_id ||
      value.capability_id != participant.required_capability_id) {
    return reject("resolved participant differs from the exact operation address",
                  "runtime_participant_address_mismatch");
  }
  return validate_payload_binding(participant, binding);
}

}  // namespace manyforge::core
