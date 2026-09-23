#include "manyforge_component_manager/manager_scene_initialization_coordinator.hpp"

#include "native_projection_transition_internal.hpp"

#include <memory>
#include <string>

namespace manyforge::component_manager {
namespace {

[[noreturn]] void Fail(const std::string &message) {
  throw ManagerSceneInitializationError(message);
}

} // namespace

ManagerSceneInitializationCoordinator::ManagerSceneInitializationCoordinator(
    SqliteManagerApplyJournal &apply_journal,
    core::IManagerSceneProjectionMaintenancePort &scene_owner)
    : apply_journal_(apply_journal), scene_owner_(scene_owner) {}

core::ProjectionMaintenanceInitializationStatusDTO
ManagerSceneInitializationCoordinator::Initialize(
    const core::ApplyId &apply_id,
    const core::ApplyGeneration apply_generation,
    const deployment::RetainedBundle &candidate) {
  std::lock_guard lock(operation_mutex_);
  const core::ProjectionMaintenanceInitializationQueryDTO query{apply_id,
                                                                apply_generation};
  if (const auto observed = scene_owner_.ReadMaintenanceInitialization(query);
      observed.has_value()) {
    if (!observed->valid()) {
      Fail("Scene returned malformed maintenance initialization status");
    }
    switch (observed->state) {
    case core::ProjectionMaintenanceInitializationState::kInitializing:
      break;
    case core::ProjectionMaintenanceInitializationState::kInitialized:
    case core::ProjectionMaintenanceInitializationState::kCompleted:
      return *observed;
    case core::ProjectionMaintenanceInitializationState::kFailed:
    case core::ProjectionMaintenanceInitializationState::kInterrupted:
      Fail("Scene maintenance initialization failed: " +
           observed->first_error->code + ": " +
           observed->first_error->message);
    }
  }

  const auto intent = apply_journal_.ReadIntent(apply_id, apply_generation);
  const auto ready =
      apply_journal_.ReadLatestRuntimeStagedReady(apply_id, apply_generation);
  if (!intent.has_value() || !ready.has_value()) {
    Fail("Scene initialization requires durable Apply and runtime readiness");
  }
  if (!deployment::verify_retained_bundle_identity(candidate) ||
      candidate.retained_bundle_id() !=
          intent->intent.candidate_retained_bundle_id ||
      candidate.value().cell_graph.graph_revision !=
          intent->intent.candidate_graph_revision ||
      candidate.value().cell_graph.cell_id != intent->intent.cell_id) {
    Fail("Scene initialization did not receive the exact captured candidate");
  }
  core::ProjectionMaintenanceInitializationRequestDTO request{
      apply_id,
      apply_generation,
      intent->intent.expected_active_projection_revision,
      candidate.value(),
      detail::DeriveCandidateNativeProjectionBindings(candidate.value(),
                                                      ready->staged_ready),
  };
  if (!request.valid()) {
    Fail("Scene initialization request is invalid");
  }
  auto result = scene_owner_.BeginMaintenanceInitialization(request);
  if (!result.valid() || result.apply_id != apply_id ||
      result.apply_generation != apply_generation ||
      result.predecessor_projection_revision !=
          request.expected_active_projection_revision ||
      result.target_projection_revision.retained_bundle_id !=
          request.candidate_retained_bundle.retained_bundle_id ||
      result.target_projection_revision.graph_revision !=
          request.candidate_retained_bundle.cell_graph.graph_revision ||
      result.native_runtime_bindings != request.native_runtime_bindings) {
    Fail("Scene returned foreign maintenance initialization evidence");
  }
  if (result.state ==
      core::ProjectionMaintenanceInitializationState::kInitializing) {
    throw ManagerSceneInitializationPending(
        "Scene maintenance initialization was accepted");
  }
  if (!result.ready_for_publication()) {
    Fail("Scene maintenance initialization did not complete successfully");
  }
  return result;
}

} // namespace manyforge::component_manager
