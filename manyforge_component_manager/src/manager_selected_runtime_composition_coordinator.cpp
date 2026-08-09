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

[[nodiscard]] std::shared_ptr<const deployment::RetainedBundle>
LoadBundle(const deployment::IVerifiedRetainedBundleSource &source,
           const core::RetainedBundleId &bundle_id) {
  auto bundle = source.get_verified(bundle_id);
  Require(bundle != nullptr &&
              deployment::verify_retained_bundle_identity(*bundle) &&
              bundle->retained_bundle_id() == bundle_id,
          "selected runtime composition cannot verify its retained bundle");
  return bundle;
}

} // namespace

ManagerSelectedRuntimeCompositionCoordinator::
    ManagerSelectedRuntimeCompositionCoordinator(
        SqliteManagerApplyJournal &apply_journal,
        const deployment::IVerifiedRetainedBundleSource &retained_bundle_source,
        IManagerSelectedRuntimeCompositionPort &runtime_composition,
        core::RuntimeEpoch manager_runtime_epoch)
    : apply_journal_(apply_journal),
      retained_bundle_source_(retained_bundle_source),
      runtime_composition_(runtime_composition),
      manager_runtime_epoch_(std::move(manager_runtime_epoch)) {
  Require(!manager_runtime_epoch_.empty(),
          "selected runtime-composition Manager epoch is empty");
}

ManagerSelectedRuntimeCompositionCoordinatorResult
ManagerSelectedRuntimeCompositionCoordinator::InstallOrRecover(
    const core::ApplyId &apply_id,
    const core::ApplyGeneration apply_generation) {
  std::lock_guard lock(operation_mutex_);
  const auto apply = apply_journal_.ReadIntent(apply_id, apply_generation);
  const auto attempt =
      apply_journal_.ReadFenceAttempt(apply_id, apply_generation);
  const auto acceptance =
      apply_journal_.ReadFenceAcceptance(apply_id, apply_generation);
  const auto runtime =
      apply_journal_.ReadLatestRuntimeStagedReady(apply_id, apply_generation);
  const auto stage =
      apply_journal_.ReadSceneStageIntent(apply_id, apply_generation);
  const auto activated =
      apply_journal_.ReadSceneActivated(apply_id, apply_generation);
  Require(apply.has_value() && attempt.has_value() && acceptance.has_value() &&
              runtime.has_value() && stage.has_value() && activated.has_value(),
          "selected runtime composition requires complete durable Apply, "
          "fence, runtime, stage, and activation evidence");
  Require(runtime->staged_ready.runtime_units.size() == 1U,
          "N7 selected runtime composition requires the sole staged native "
          "execution host");

  const auto &activation = activated->scene_activated.activation_result;
  const auto &accepted_fence = acceptance->accepted_fence;
  const auto &fence_intent = attempt->fence_intent;
  const auto &runtime_unit = runtime->staged_ready.runtime_units.front();
  Require(
      activated->scene_activated.valid() && activation.presence_valid() &&
          activation.state ==
              core::ProjectionApplyActivationResultState::kActivated &&
          activation.apply_id == apply_id &&
          activation.apply_generation == apply_generation &&
          activation.expected_active_projection_revision ==
              stage->scene_stage_intent.expected_active_projection_revision &&
          activation.projection_stage_generation.nonzero() &&
          activation.candidate_projection_revision.retained_bundle_id ==
              stage->scene_stage_intent.candidate_retained_bundle_id &&
          activation.candidate_projection_revision.graph_revision ==
              stage->scene_stage_intent.candidate_graph_revision,
      "selected runtime composition has invalid activation evidence");
  Require(
      accepted_fence.matches(fence_intent) &&
          fence_intent.owner_transaction.apply_id == apply_id &&
          fence_intent.owner_transaction.apply_generation == apply_generation &&
          runtime->staged_ready.predecessor_publication_generation ==
              stage->scene_stage_intent.predecessor_publication_generation &&
          runtime->staged_ready.predecessor_public_pointer_digest ==
              stage->scene_stage_intent.predecessor_public_pointer_digest,
      "selected runtime composition fence or predecessor evidence "
      "conflicts");

  auto candidate =
      LoadBundle(retained_bundle_source_,
                 activation.candidate_projection_revision.retained_bundle_id);
  Require(candidate->value().cell_graph.cell_id == apply_journal_.cell_id() &&
              candidate->value().cell_graph.graph_revision ==
                  activation.candidate_projection_revision.graph_revision &&
              candidate->retained_bundle_id() ==
                  apply->intent.candidate_retained_bundle_id,
          "selected runtime composition candidate differs from the durable "
          "Apply");
  const auto canonical_bytes = candidate->canonical_bytes();
  Require(!canonical_bytes.empty() &&
              canonical_bytes.size() <=
                  static_cast<std::size_t>(
                      std::numeric_limits<std::uint64_t>::max()),
          "selected runtime composition bundle byte extent is invalid");
  const ManagerApplyPrivateMaterializationDTO materialization{
      activated->scene_activated_digest,
      apply_id,
      apply_generation,
      activation.projection_stage_generation,
      activation.candidate_projection_revision,
      core::ContentDigest(deployment::sha256_digest(canonical_bytes)),
      static_cast<std::uint64_t>(canonical_bytes.size()),
  };
  Require(materialization.valid(),
          "selected runtime composition materialization is invalid");
  const ManagerSelectedRuntimeCompositionRequestDTO request{
      materialization.scene_activated_digest,
      materialization.apply_id,
      materialization.apply_generation,
      materialization.projection_stage_generation,
      materialization.candidate_projection_revision,
      stage->scene_stage_intent.predecessor_publication_generation,
      stage->scene_stage_intent.predecessor_public_pointer_digest,
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
