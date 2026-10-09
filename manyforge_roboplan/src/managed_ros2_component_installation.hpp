#pragma once

#include <algorithm>
#include <stdexcept>
#include <vector>

#include <manyforge_component_manager/selected_runtime_composition.hpp>
#include <manyforge_core/resource_addressed_execution_kernel.hpp>
#include <manyforge_roboplan/retained_ros2_control_runtime_handle.hpp>

namespace manyforge::roboplan_backend::detail {

// This provider qualifies only the first aggregate installation. The caller
// independently verifies both bundles and derives their exact component delta.
inline void RequireInitialRos2ComponentInstallation(
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request,
    const core::CellGraphDTO &predecessor,
    const core::CellGraphDTO &candidate) {
  const auto &rows = request.component_delta.components;
  if (request.valid() && predecessor.controllable_components.empty() &&
      candidate.controllable_components.empty() && rows.empty() &&
      predecessor.cell_id == candidate.cell_id &&
      predecessor.graph_revision ==
          request.predecessor_projection_revision.graph_revision &&
      candidate.graph_revision ==
          request.candidate_projection_revision.graph_revision)
    return;
  if (!request.valid() || !predecessor.controllable_components.empty() ||
      candidate.controllable_components.size() != 1U || rows.size() != 1U ||
      rows.front().disposition != deployment::ApplyComponentDisposition::kAdd ||
      rows.front().controllable_component_id !=
          candidate.controllable_components.front().controllable_component_id ||
      predecessor.cell_id != candidate.cell_id ||
      predecessor.graph_revision !=
          request.predecessor_projection_revision.graph_revision ||
      candidate.graph_revision !=
          request.candidate_projection_revision.graph_revision) {
    throw std::invalid_argument(
        "ros2_component_delta_not_qualified: only empty-to-one aggregate "
        "installation is supported; retain, removal and replacement remain "
        "unavailable");
  }
}

// Read actual factory-bound adapter and owned participant registrations.
// Readiness is a separate lifecycle observation, never invented by this
// ownership receipt.
inline component_manager::ManagerRuntimeComponentReceiptDTO
ReadInitialRos2ComponentReceipt(
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request,
    const core::ContentDigest &installed_runtime_identity,
    const RetainedRos2ControlRuntimeHandle &handle,
    const core::IExecutionBackend *execution,
    const core::IGripperCommandBackend *gripper,
    const core::RuntimeParticipantBinding &motion,
    const core::RuntimeParticipantBinding &tool,
    const std::vector<core::RuntimeParticipantExecutionRegistration>
        &registrations,
    const core::IResolvedParticipantExecutionEngine *motion_engine,
    const core::IResolvedParticipantExecutionEngine *gripper_engine) {
  const auto require = [](const bool condition, const char *message) {
    if (!condition)
      throw std::invalid_argument(message);
  };
  const auto &rows = request.component_delta.components;
  require(request.valid() && rows.size() == 1U &&
              rows.front().disposition ==
                  deployment::ApplyComponentDisposition::kAdd,
          "ROS installed receipt requires one exact initial Add");
  const auto &component_id = rows.front().controllable_component_id;
  require(
      rows.front().candidate_runtime_identity == installed_runtime_identity &&
          handle.OwnsBackend(execution, motion, tool) && execution != nullptr &&
          handle.OwnsGripper(gripper, tool),
      "ROS installed receipt has no exact live factory-bound adapters");
  require(motion_engine != nullptr && gripper_engine != nullptr &&
              motion_engine != gripper_engine,
          "ROS installed receipt has no distinct owned participant engines");
  for (const auto *physical : {&motion, &tool}) {
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
      require(std::binary_search(
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
    const bool is_motion =
        binding.command_target_id == motion.value().command_target_id;
    require(
        binding.controllable_component_id == component_id &&
            binding.retained_bundle_id ==
                request.candidate_projection_revision.retained_bundle_id &&
            binding.graph_revision ==
                request.candidate_projection_revision.graph_revision &&
            binding.runtime_unit_epochs == motion.value().runtime_unit_epochs &&
            (is_motion ||
             binding.command_target_id == tool.value().command_target_id) &&
            registration.engine ==
                (is_motion ? motion_engine : gripper_engine) &&
            registration.stop_execution_port ==
                static_cast<const core::IControlDomainExecutionPort *>(
                    execution),
        "ROS installed registration crosses its owned component or adapter");
  }
  component_manager::ManagerRuntimeComponentReceiptDTO receipt{
      component_id,
      deployment::ApplyComponentDisposition::kAdd,
      installed_runtime_identity,
      motion.value().runtime_unit_epochs,
      false,
      false};
  require(component_manager::ComponentReceiptsMatchRequest(request, {receipt}),
          "ROS installed receipt differs from its exact Apply request");
  return receipt;
}

} // namespace manyforge::roboplan_backend::detail
