#include "manyforge_component_manager/manager_selected_runtime_composition_coordinator.hpp"

#include <limits>
#include <manyforge_deployment/content_identity.hpp>
#include <string>
#include <utility>

namespace manyforge::component_manager {
namespace {

namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

[[noreturn]] void Fail(const std::string &detail) {
  throw ManagerSelectedRuntimeCompositionCoordinatorError(detail);
}

void Require(const bool condition, const std::string &detail) {
  if (!condition) {
    Fail(detail);
  }
}

} // namespace

ManagerSelectedRuntimeCompositionCoordinator::
    ManagerSelectedRuntimeCompositionCoordinator(
        SqliteManagerApplyJournal &apply_journal,
        core::IManagerSceneProjectionMaintenancePort &scene_owner,
        IManagerSelectedRuntimeCompositionPort &runtime_composition,
        core::RuntimeEpoch manager_runtime_epoch)
    : apply_journal_(apply_journal), scene_owner_(scene_owner),
      runtime_composition_(runtime_composition),
      manager_runtime_epoch_(std::move(manager_runtime_epoch)) {
  Require(!manager_runtime_epoch_.empty(),
          "selected runtime-composition Manager epoch is empty");
}

ManagerSelectedRuntimeCompositionCoordinatorResult
ManagerSelectedRuntimeCompositionCoordinator::InstallOrRecover(
    const core::ApplyId &apply_id,
    const core::ApplyGeneration apply_generation,
    const deployment::RetainedBundle &candidate) {
  std::lock_guard lock(operation_mutex_);
  const auto apply = apply_journal_.ReadIntent(apply_id, apply_generation);
  const auto attempt =
      apply_journal_.ReadFenceAttempt(apply_id, apply_generation);
  const auto acceptance =
      apply_journal_.ReadFenceAcceptance(apply_id, apply_generation);
  const auto runtime =
      apply_journal_.ReadLatestRuntimeStagedReady(apply_id, apply_generation);
  const auto initialized = scene_owner_.ReadMaintenanceInitialization(
      {apply_id, apply_generation});
  Require(apply.has_value() && attempt.has_value() && acceptance.has_value() &&
              runtime.has_value() && initialized.has_value(),
          "selected runtime composition requires complete durable Apply, "
          "fence, runtime, and Scene initialization evidence");
  Require(runtime->staged_ready.runtime_units.size() == 1U,
          "N7 selected runtime composition requires the sole staged native "
          "execution host");

  Require(initialized->valid() &&
              (initialized->state ==
                   core::ProjectionMaintenanceInitializationState::kInitialized ||
               initialized->state ==
                   core::ProjectionMaintenanceInitializationState::kCompleted) &&
              initialized->predecessor_projection_revision ==
                  apply->intent.expected_active_projection_revision,
          "selected runtime composition has invalid Scene initialization evidence");
  const auto &accepted_fence = acceptance->accepted_fence;
  const auto &fence_intent = attempt->fence_intent;
  const auto &runtime_unit = runtime->staged_ready.runtime_units.front();
  Require(
      accepted_fence.matches(fence_intent) &&
          fence_intent.owner_transaction.apply_id == apply_id &&
          fence_intent.owner_transaction.apply_generation == apply_generation &&
          runtime->staged_ready.predecessor_publication_generation.nonzero() &&
          !runtime->staged_ready.predecessor_public_pointer_digest.empty(),
      "selected runtime composition fence or predecessor evidence "
      "conflicts");

  Require(deployment::verify_retained_bundle_identity(candidate) &&
              candidate.value().cell_graph.cell_id == apply_journal_.cell_id() &&
              candidate.value().cell_graph.graph_revision ==
                  initialized->target_projection_revision.graph_revision &&
              candidate.retained_bundle_id() ==
                  apply->intent.candidate_retained_bundle_id,
          "selected runtime composition candidate differs from the durable "
          "Apply");
  const auto canonical_bytes = candidate.canonical_bytes();
  Require(!canonical_bytes.empty() &&
              canonical_bytes.size() <=
                  static_cast<std::size_t>(
                      std::numeric_limits<std::uint64_t>::max()),
          "selected runtime composition bundle byte extent is invalid");
  const ManagerApplyPrivateMaterializationDTO materialization{
      initialized->authoritative_snapshot_identity.revision.content_hash,
      apply_id,
      apply_generation,
      initialized->target_projection_revision,
      core::ContentDigest(deployment::sha256_digest(canonical_bytes)),
      static_cast<std::uint64_t>(canonical_bytes.size()),
  };
  Require(materialization.valid(),
          "selected runtime composition materialization is invalid");
  const ManagerSelectedRuntimeCompositionRequestDTO request{
      materialization.authoritative_scene_snapshot_digest,
      materialization.apply_id,
      materialization.apply_generation,
      materialization.candidate_projection_revision,
      runtime->staged_ready.predecessor_publication_generation,
      runtime->staged_ready.predecessor_public_pointer_digest,
      materialization.retained_bundle_canonical_content_digest,
      materialization.retained_bundle_canonical_byte_size,
      runtime_unit.runtime_unit_id,
      runtime_unit.runtime_epoch,
      manager_runtime_epoch_,
      accepted_fence.configuration_fence_id,
      accepted_fence.configuration_fence_generation,
      accepted_fence.captured_scope,
  };
  Require(request.valid(), "selected runtime-composition request is invalid");
  const auto result = runtime_composition_.InstallOrObserve(request);
  Require(result.valid() && result.request == request,
          "managed execution host returned foreign runtime-composition "
          "evidence");
  if (result.state == ManagerSelectedRuntimeCompositionState::kPreparing) {
    return {ManagerSelectedRuntimeCompositionResultState::kPending, result,
            std::nullopt};
  }
  auto durable = apply_journal_.EnsurePrivateMaterialization(materialization);
  return {ManagerSelectedRuntimeCompositionResultState::kInstalled, result,
          std::move(durable)};
}

} // namespace manyforge::component_manager
