#pragma once

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <manyforge_component_manager/selected_runtime_composition.hpp>
#include <manyforge_core/resource_addressed_execution_kernel.hpp>
#include <manyforge_roboplan/retained_ros2_control_runtime_handle.hpp>

namespace manyforge::roboplan_backend::detail {

// Interim Apply guard until selected component bring-up supplies Q86 recovery.
// A known exited Retain cannot restart its launch or clear the existing incident.
inline void RefuseExitedRos2Retain(
    const core::ControllableComponentId &component_id,
    const std::optional<std::string> &exit_reason) {
  if (exit_reason) {
    throw std::invalid_argument("ROS Apply cannot retain component " +
                                component_id.value() + ": " + *exit_reason +
                                "; bring-up required");
  }
}

// Read actual factory-bound adapter and owned participant registrations.
// Readiness is a separate lifecycle observation, never invented by this
// ownership receipt.
inline component_manager::ManagerRuntimeComponentReceiptDTO
ReadRos2ComponentReceipt(
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request,
    const core::ControllableComponentId &component_id,
    const core::ContentDigest &installed_runtime_identity,
    const RetainedRos2ControlRuntimeHandle &handle,
    const core::IExecutionBackend *execution,
    const core::IGripperCommandBackend *gripper,
    const core::RuntimeParticipantBinding &motion,
    const std::optional<core::RuntimeParticipantBinding> &tool,
    const std::vector<core::RuntimeParticipantExecutionRegistration>
        &registrations,
    const core::IResolvedParticipantExecutionEngine *motion_engine,
    const core::IResolvedParticipantExecutionEngine *gripper_engine,
    const RetainedRos2ControlRuntimeHandle *predecessor_handle,
    const std::optional<std::string> &retained_process_exit = std::nullopt) {
  const auto require = [](const bool condition, const char *message) {
    if (!condition)
      throw std::invalid_argument(message);
  };
  const auto &rows = request.component_delta.components;
  const auto row =
      std::find_if(rows.begin(), rows.end(), [&](const auto &entry) {
        return entry.controllable_component_id == component_id;
      });
  require(request.valid() && row != rows.end() &&
              row->disposition !=
                  deployment::ApplyComponentDisposition::kRemove,
          "ROS installed receipt requires an exact candidate component");
  // Q118: the authored delta is immutable; retention reports the actual owner.
  const bool retained = predecessor_handle == &handle;
  if (retained)
    RefuseExitedRos2Retain(component_id, retained_process_exit);

  require(row->candidate_runtime_identity == installed_runtime_identity &&
              execution != nullptr &&
              handle.OwnsBackend(execution, motion, tool) &&
              (tool ? handle.OwnsGripper(gripper, *tool) : gripper == nullptr),
          "ROS installed receipt has no exact live factory-bound adapters");
  require(motion_engine != nullptr &&
              (!tool ||
               (gripper_engine != nullptr && motion_engine != gripper_engine)),
          "ROS installed receipt has no distinct owned participant engines");
  std::vector<const core::RuntimeParticipantBinding *> physical_bindings{
      &motion};
  if (tool)
    physical_bindings.push_back(&*tool);
  for (const auto *physical : physical_bindings) {
    const auto &binding = physical->value();
    require(binding.controllable_component_id == component_id &&
                binding.retained_bundle_id ==
                    request.candidate_projection_revision.retained_bundle_id &&
                binding.graph_revision ==
                    request.candidate_projection_revision.graph_revision &&
                binding.runtime_unit_epochs ==
                    std::vector<core::RuntimeUnitEpochBindingDTO>{
                        {request.runtime_unit_id, request.runtime_epoch}},
            "ROS installed participant names another product or runtime epoch");
    require(binding.control_path.has_value() &&
                binding.control_path->provider_runtime_unit_id ==
                    request.runtime_unit_id &&
                binding.control_path->provider_runtime_epoch ==
                    request.runtime_epoch,
            "ROS installed participant has another control owner");
    for (const auto &resource : binding.resources) {
      require(retained ||
                  std::binary_search(
                      request.configuration_fence_scope.resources.begin(),
                      request.configuration_fence_scope.resources.end(),
                      resource.resource_id),
              "ROS installed participant resource is outside its Apply fence");
    }
    const auto expected_engine =
        physical == &motion ? motion_engine : gripper_engine;
    require(
        std::count_if(
            registrations.begin(), registrations.end(),
            [&](const auto &registration) {
              return registration.binding.value() == binding &&
                     registration.engine == expected_engine &&
                     registration.stop_execution_port ==
                         static_cast<const core::IControlDomainExecutionPort *>(
                             execution);
            }) == 1,
        "ROS installed participant has no unique owned execution/Stop "
        "registration");
  }
  for (const auto &registration : registrations) {
    const auto &binding = registration.binding.value();
    if (binding.controllable_component_id != component_id)
      continue;
    const bool is_motion =
        binding.command_target_id == motion.value().command_target_id;
    require(
        binding.controllable_component_id == component_id &&
            binding.retained_bundle_id ==
                request.candidate_projection_revision.retained_bundle_id &&
            binding.graph_revision ==
                request.candidate_projection_revision.graph_revision &&
            binding.runtime_unit_epochs == motion.value().runtime_unit_epochs &&
            (is_motion || (tool && binding.command_target_id ==
                                       tool->value().command_target_id)) &&
            registration.engine ==
                (is_motion ? motion_engine : gripper_engine) &&
            registration.stop_execution_port ==
                static_cast<const core::IControlDomainExecutionPort *>(
                    execution),
        "ROS installed registration crosses its owned component or adapter");
  }
  const bool product_changed =
      request.predecessor_projection_revision.retained_bundle_id !=
          request.candidate_projection_revision.retained_bundle_id ||
      request.predecessor_projection_revision.graph_revision !=
          request.candidate_projection_revision.graph_revision;
  component_manager::ManagerRuntimeComponentReceiptDTO receipt{
      component_id,
      row->disposition,
      installed_runtime_identity,
      motion.value().runtime_unit_epochs,
      retained,
      retained && product_changed};
  require(receipt.valid(), "ROS installed receipt is invalid");
  return receipt;
}

} // namespace manyforge::roboplan_backend::detail
