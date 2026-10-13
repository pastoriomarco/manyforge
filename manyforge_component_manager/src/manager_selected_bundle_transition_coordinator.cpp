#include "manyforge_component_manager/manager_selected_bundle_transition_coordinator.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <manyforge_deployment/content_identity.hpp>
#include <utility>

namespace manyforge::component_manager {
namespace {

namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

[[noreturn]] void Fail(const std::string &detail) {
  throw ManagerSelectedBundleTransitionCoordinatorError(detail);
}

[[nodiscard]] bool ExactPublication(
    const core::ActiveBundlePublicationReceiptDTO &receipt,
    const core::ActiveBundlePublicationRequestDTO &request,
    const core::ActiveBundlePublicationGeneration generation) {
  return receipt.valid() && receipt.request == request &&
         receipt.publication_generation == generation;
}

[[nodiscard]] bool ExactPredecessorPublication(
    const core::ActiveBundlePublicationReceiptDTO &receipt,
    const ManagerApplyIntentDTO &intent,
    const PersistedManagerApplyRuntimeStagedReady &ready) {
  return receipt.valid() &&
         receipt.publication_generation ==
             ready.staged_ready.predecessor_publication_generation &&
         receipt.durable_public_pointer_content_digest ==
             ready.staged_ready.predecessor_public_pointer_digest &&
         core::SameActiveDeployment(
             receipt.request.candidate_projection_revision,
             intent.expected_active_projection_revision);
}

} // namespace

ManagerSelectedBundleTransitionEffectUncertainError::
    ManagerSelectedBundleTransitionEffectUncertainError(
        std::string detail,
        core::ActiveBundlePublicationRequestDTO publication_request,
        core::ProjectionMaintenanceInitializationQueryDTO scene_status_query,
        std::optional<core::ActiveBundlePublicationReceiptDTO>
            observed_publication,
        std::optional<core::ProjectionMaintenanceInitializationStatusDTO>
            observed_scene_status)
    : ManagerSelectedBundleTransitionCoordinatorError(std::move(detail)),
      publication_request_(std::move(publication_request)),
      scene_status_query_(std::move(scene_status_query)),
      observed_publication_(std::move(observed_publication)),
      observed_scene_status_(std::move(observed_scene_status)) {}

const core::ActiveBundlePublicationRequestDTO &
ManagerSelectedBundleTransitionEffectUncertainError::publication_request()
    const noexcept {
  return publication_request_;
}

const core::ProjectionMaintenanceInitializationQueryDTO &
ManagerSelectedBundleTransitionEffectUncertainError::scene_status_query()
    const noexcept {
  return scene_status_query_;
}

const std::optional<core::ActiveBundlePublicationReceiptDTO> &
ManagerSelectedBundleTransitionEffectUncertainError::observed_publication()
    const noexcept {
  return observed_publication_;
}

const std::optional<core::ProjectionMaintenanceInitializationStatusDTO> &
ManagerSelectedBundleTransitionEffectUncertainError::observed_scene_status()
    const noexcept {
  return observed_scene_status_;
}

ManagerSelectedBundleTransitionCoordinator::
    ManagerSelectedBundleTransitionCoordinator(
        SqliteManagerApplyJournal &apply_journal,
        core::IActiveBundlePublicationStore &publication_store,
        core::IManagerSceneProjectionMaintenancePort &scene_owner)
    : apply_journal_(apply_journal), publication_store_(publication_store),
      scene_owner_(scene_owner) {}

ManagerSelectedBundleTransitionResult
ManagerSelectedBundleTransitionCoordinator::AdvanceOrRecover(
    const core::ApplyId &apply_id,
    const core::ApplyGeneration apply_generation,
    const deployment::RetainedBundle &predecessor,
    const deployment::RetainedBundle &candidate) {
  return Advance(apply_id, apply_generation, predecessor, candidate, true);
}

ManagerSelectedBundleTransitionResult
ManagerSelectedBundleTransitionCoordinator::PublishOriginalConfiguration(
    const core::ApplyId &apply_id, const core::ApplyGeneration apply_generation,
    const deployment::RetainedBundle &predecessor,
    const deployment::RetainedBundle &candidate) {
  return Advance(apply_id, apply_generation, predecessor, candidate, false);
}

ManagerSelectedBundleTransitionResult
ManagerSelectedBundleTransitionCoordinator::Advance(
    const core::ApplyId &apply_id, const core::ApplyGeneration apply_generation,
    const deployment::RetainedBundle &predecessor,
    const deployment::RetainedBundle &candidate, const bool complete_scene) {
  std::lock_guard lock(operation_mutex_);
  const auto apply = apply_journal_.ReadIntent(apply_id, apply_generation);
  const auto ready =
      apply_journal_.ReadLatestRuntimeStagedReady(apply_id, apply_generation);
  const auto initialized = scene_owner_.ReadMaintenanceInitialization(
      {apply_id, apply_generation});
  if (!apply.has_value() || !ready.has_value() || !initialized.has_value() ||
      !initialized->valid() ||
      (initialized->state !=
           core::ProjectionMaintenanceInitializationState::kInitialized &&
       initialized->state !=
           core::ProjectionMaintenanceInitializationState::kCompleted) ||
      !initialized->initial_scene_installed) {
    Fail("selected-bundle publication requires the final initial Scene");
  }
  if (initialized->predecessor_projection_revision !=
          apply->intent.expected_active_projection_revision ||
      initialized->target_projection_revision.retained_bundle_id !=
          apply->intent.candidate_retained_bundle_id ||
      initialized->target_projection_revision.graph_revision !=
          apply->intent.candidate_graph_revision) {
    Fail("Scene initialization identity differs from the durable Apply");
  }

  if (!deployment::verify_retained_bundle_identity(predecessor) ||
      !deployment::verify_retained_bundle_identity(candidate) ||
      predecessor.retained_bundle_id() !=
          apply->intent.expected_active_projection_revision.retained_bundle_id ||
      candidate.retained_bundle_id() !=
          apply->intent.candidate_retained_bundle_id ||
      predecessor.value().cell_graph.cell_id != apply->intent.cell_id ||
      candidate.value().cell_graph.cell_id != apply->intent.cell_id ||
      predecessor.value().cell_graph.graph_revision !=
          apply->intent.expected_active_projection_revision.graph_revision ||
      candidate.value().cell_graph.graph_revision !=
          apply->intent.candidate_graph_revision) {
    Fail("selected retained bundles differ from the durable Apply");
  }

  const auto canonical_bytes = candidate.canonical_bytes();
  if (canonical_bytes.empty() ||
      canonical_bytes.size() >
          static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max())) {
    Fail("selected retained bundle has an invalid canonical byte extent");
  }
  const ManagerApplyPrivateMaterializationDTO expected_materialization{
      initialized->authoritative_snapshot_identity.revision.content_hash,
      apply_id,
      apply_generation,
      initialized->target_projection_revision,
      core::ContentDigest(deployment::sha256_digest(canonical_bytes)),
      static_cast<std::uint64_t>(canonical_bytes.size()),
  };
  const auto durable_materialization =
      apply_journal_.ReadPrivateMaterialization(apply_id, apply_generation);
  if (!durable_materialization.has_value() ||
      durable_materialization->private_materialization !=
          expected_materialization) {
    Fail("selected runtime composition is not durably materialized");
  }
  ManagerApplyPrivateMaterializationWriteResult materialization{
      *durable_materialization, false};
  const core::ActiveBundlePublicationRequestDTO publication_request{
      core::NamedApplyPublicationTransactionDTO{
          apply_id, apply_generation},
      initialized->target_projection_revision,
      initialized->target_projection_revision.retained_bundle_id,
      initialized->target_projection_revision.graph_revision,
      ready->staged_ready.predecessor_publication_generation,
      materialization.record.private_materialization_content_digest,
  };
  if (!publication_request.valid()) {
    Fail("selected-bundle publication request is invalid");
  }
  const auto previous_generation =
      ready->staged_ready.predecessor_publication_generation.value();
  if (previous_generation == std::numeric_limits<std::uint64_t>::max()) {
    Fail("active-bundle publication generation is exhausted");
  }
  const core::ActiveBundlePublicationGeneration expected_generation(
      previous_generation + 1U);
  const core::ProjectionMaintenanceInitializationQueryDTO scene_query{
      apply_id, apply_generation};
  const auto read_head = [&](core::ActiveBundlePublicationGeneration value) {
    try {
      return publication_store_.ReadActiveBundlePublication(value);
    } catch (const std::exception &error) {
      throw ManagerSelectedBundleTransitionEffectUncertainError(
          "active-bundle head read failed: " + std::string(error.what()),
          publication_request, scene_query);
    }
  };

  auto path = ManagerSelectedBundleTransitionPath::kPublishedFromNewCall;
  ManagerApplyPublicationReceiptWriteResult durable_publication = [&] {
    if (const auto existing =
            apply_journal_.ReadPublicationReceipt(apply_id, apply_generation);
        existing.has_value()) {
      if (!ExactPublication(existing->publication.publication_receipt,
                            publication_request, expected_generation)) {
        Fail("durable publication differs from the selected transition");
      }
      return ManagerApplyPublicationReceiptWriteResult{*existing, false};
    }
    const auto possible = read_head(expected_generation);
    if (possible.ok()) {
      if (!ExactPublication(possible.value(), publication_request,
                            expected_generation)) {
        throw ManagerSelectedBundleTransitionEffectUncertainError(
            "current publication belongs to a different transaction",
            publication_request, scene_query, possible.value());
      }
      path = ManagerSelectedBundleTransitionPath::kRecoveredPublicationHead;
      return apply_journal_.EnsurePublicationReceipt(
          {materialization.record.private_materialization_content_digest,
           possible.value()});
    }
    const auto predecessor_head = read_head(
        ready->staged_ready.predecessor_publication_generation);
    if (!predecessor_head.ok() ||
        !ExactPredecessorPublication(predecessor_head.value(), apply->intent,
                                    *ready)) {
      throw ManagerSelectedBundleTransitionEffectUncertainError(
          "neither predecessor nor candidate is the durable publication head",
          publication_request, scene_query,
          predecessor_head.ok()
              ? std::optional<core::ActiveBundlePublicationReceiptDTO>{
                    predecessor_head.value()}
              : std::nullopt);
    }
    const auto receipt = [&]() -> core::ActiveBundlePublicationReceiptDTO {
      try {
        return publication_store_.PublishActiveBundle(publication_request);
      } catch (const std::exception &error) {
        const auto recovered = read_head(expected_generation);
        if (!recovered.ok() ||
            !ExactPublication(recovered.value(), publication_request,
                              expected_generation)) {
          throw ManagerSelectedBundleTransitionEffectUncertainError(
              "active-bundle publication outcome is uncertain: " +
                  std::string(error.what()),
              publication_request, scene_query,
              recovered.ok()
                  ? std::optional<core::ActiveBundlePublicationReceiptDTO>{
                        recovered.value()}
                  : std::nullopt);
        }
        path = ManagerSelectedBundleTransitionPath::kRecoveredPublicationHead;
        return recovered.value();
      }
    }();
    if (!ExactPublication(receipt, publication_request, expected_generation)) {
      throw ManagerSelectedBundleTransitionEffectUncertainError(
          "publication store returned foreign evidence", publication_request,
          scene_query, receipt);
    }
    return apply_journal_.EnsurePublicationReceipt(
        {materialization.record.private_materialization_content_digest,
         receipt});
  }();

  if (initialized->state ==
      core::ProjectionMaintenanceInitializationState::kCompleted) {
    if (initialized->publication_receipt !=
        std::optional<core::ActiveBundlePublicationReceiptDTO>{
            durable_publication.record.publication.publication_receipt}) {
      Fail("completed Scene state differs from durable publication");
    }
    return {materialization, durable_publication, true,
            ManagerSelectedBundleTransitionPath::kRecoveredCompletedOwnerStatus};
  }
  if (durable_publication.inserted || !complete_scene) {
    return {materialization, durable_publication, false, path};
  }

  const auto completed = [&]() -> core::ProjectionMaintenanceInitializationStatusDTO {
    try {
      return scene_owner_.CompleteMaintenanceInitialization({
          apply_id, apply_generation, initialized->target_projection_revision,
          initialized->authoritative_snapshot_identity,
          durable_publication.record.publication.publication_receipt});
    } catch (const std::exception &error) {
      const auto observed = scene_owner_.ReadMaintenanceInitialization(scene_query);
      throw ManagerSelectedBundleTransitionEffectUncertainError(
          "Scene completion outcome is uncertain: " + std::string(error.what()),
          publication_request, scene_query,
          durable_publication.record.publication.publication_receipt, observed);
    }
  }();
  if (!completed.valid() ||
      completed.state !=
          core::ProjectionMaintenanceInitializationState::kCompleted ||
      completed.publication_receipt !=
          std::optional<core::ActiveBundlePublicationReceiptDTO>{
              durable_publication.record.publication.publication_receipt}) {
    throw ManagerSelectedBundleTransitionEffectUncertainError(
        "Scene returned foreign completion evidence", publication_request,
        scene_query, durable_publication.record.publication.publication_receipt,
        completed);
  }
  return {materialization, durable_publication, true,
          ManagerSelectedBundleTransitionPath::kCompletedFromNewCall};
}

} // namespace manyforge::component_manager
