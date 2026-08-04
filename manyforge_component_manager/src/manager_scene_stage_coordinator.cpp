#include "manyforge_component_manager/manager_scene_stage_coordinator.hpp"

#include <algorithm>
#include <exception>
#include <memory>
#include <utility>
#include <vector>

namespace manyforge::component_manager {
namespace {

using core::ActiveBundlePublicationReceiptDTO;
using core::ApplyGeneration;
using core::ApplyId;
using core::ProjectionApplyStageDTO;
using core::ProjectionApplyStageResultDTO;
using core::ProjectionApplyStageResultState;
using core::ProjectionApplyStatusDTO;
using core::ProjectionApplyStatusQueryDTO;
using core::ProjectionApplyStatusState;
using core::ProjectionTransitionRowDTO;
using deployment::RetainedBundle;

[[noreturn]] void Fail(const std::string &detail) {
  throw ManagerSceneStageCoordinatorError(detail);
}

[[nodiscard]] std::shared_ptr<const RetainedBundle>
LoadExactBundle(const deployment::IVerifiedRetainedBundleSource &source,
                const core::RetainedBundleId &retained_bundle_id) {
  try {
    auto bundle = source.get_verified(retained_bundle_id);
    if (bundle == nullptr ||
        !deployment::verify_retained_bundle_identity(*bundle) ||
        bundle->retained_bundle_id() != retained_bundle_id) {
      Fail("scene stage cannot reload the exact verified retained bundle");
    }
    return bundle;
  } catch (const ManagerSceneStageCoordinatorError &) {
    throw;
  } catch (const std::exception &error) {
    Fail("scene stage retained-bundle reload failed: " +
         std::string(error.what()));
  } catch (...) {
    Fail("scene stage retained-bundle reload failed");
  }
}

void VerifyBundleBindings(const ManagerApplyIntentDTO &apply_intent,
                          const RetainedBundle &predecessor,
                          const RetainedBundle &candidate) {
  const auto &predecessor_graph = predecessor.value().cell_graph;
  const auto &candidate_graph = candidate.value().cell_graph;
  if (apply_intent.cell_id != predecessor_graph.cell_id ||
      apply_intent.cell_id != candidate_graph.cell_id ||
      apply_intent.expected_active_projection_revision.retained_bundle_id !=
          predecessor.retained_bundle_id() ||
      apply_intent.expected_active_projection_revision.graph_revision !=
          predecessor_graph.graph_revision ||
      apply_intent.candidate_retained_bundle_id !=
          candidate.retained_bundle_id() ||
      apply_intent.candidate_graph_revision != candidate_graph.graph_revision) {
    Fail("scene stage retained bundles differ from the durable Apply intent");
  }
}

[[nodiscard]] ActiveBundlePublicationReceiptDTO
VerifyCurrentPredecessor(const core::IActiveBundlePublicationReader &reader,
                         const ManagerApplySceneStageIntentDTO &stage_intent) {
  core::Result<ActiveBundlePublicationReceiptDTO> read =
      core::Result<ActiveBundlePublicationReceiptDTO>::Err(
          core::ErrorCode::kInternalError);
  try {
    read = reader.ReadActiveBundlePublication(
        stage_intent.predecessor_publication_generation);
  } catch (const std::exception &error) {
    Fail("scene stage predecessor publication read failed: " +
         std::string(error.what()));
  } catch (...) {
    Fail("scene stage predecessor publication read failed");
  }
  if (!read.ok()) {
    Fail("scene stage predecessor publication is no longer current");
  }
  const auto &receipt = read.value();
  if (!receipt.valid() ||
      receipt.publication_generation !=
          stage_intent.predecessor_publication_generation ||
      receipt.durable_public_pointer_content_digest !=
          stage_intent.predecessor_public_pointer_digest ||
      receipt.request.candidate_projection_revision !=
          stage_intent.expected_active_projection_revision ||
      receipt.request.candidate_retained_bundle_id !=
          stage_intent.expected_active_projection_revision.retained_bundle_id ||
      receipt.request.candidate_graph_revision !=
          stage_intent.expected_active_projection_revision.graph_revision) {
    Fail("scene stage predecessor publication differs from its durable "
         "intent");
  }
  return receipt;
}

[[nodiscard]] std::vector<ProjectionTransitionRowDTO> DeriveTransitionRows(
    const RetainedBundle &predecessor, const RetainedBundle &candidate,
    const PersistedManagerApplyRuntimeStagedReady &staged_ready) {
  static_cast<void>(staged_ready);
  // The first admitted I4 named fixture is runtime-only. A nonempty provider
  // set cannot be guessed from generic endpoints: it requires the later exact
  // planner/collision provider-to-runtime derivation and scene adapter. Failing
  // here keeps unsupported lanes offline without accepting caller rows.
  if (!predecessor.value().capability_provider_bindings.empty() ||
      !candidate.value().capability_provider_bindings.empty()) {
    Fail("nonempty native scene transition is not admitted by the current "
         "I4 runtime-only product boundary");
  }
  return {};
}

[[nodiscard]] bool
TransitionRowsMatch(const core::ProjectionTransitionEvidenceDTO &evidence,
                    const std::vector<ProjectionTransitionRowDTO> &expected) {
  if (evidence.transition_rows.size() != expected.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < expected.size(); ++index) {
    if (evidence.transition_rows[index].transition_row != expected[index]) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool
ExactStagedResult(const ProjectionApplyStageResultDTO &result,
                  const ProjectionApplyStageDTO &request) {
  return result.state == ProjectionApplyStageResultState::kStaged &&
         result.presence_valid() && result.apply_id == request.apply_id &&
         result.apply_generation == request.apply_generation &&
         result.expected_active_projection_revision ==
             request.expected_active_projection_revision &&
         result.candidate_projection_revision->retained_bundle_id ==
             request.candidate_retained_bundle.retained_bundle_id &&
         result.candidate_projection_revision->graph_revision ==
             request.candidate_retained_bundle.cell_graph.graph_revision &&
         TransitionRowsMatch(*result.transition_evidence,
                             request.native_transition_rows);
}

[[nodiscard]] ProjectionApplyStageResultDTO
StagedResultFromStatus(const ProjectionApplyStatusDTO &status,
                       const ProjectionApplyStageDTO &request) {
  ProjectionApplyStageResultDTO result{
      status.apply_id,
      status.apply_generation,
      *status.predecessor_projection_revision,
      status.candidate_projection_revision,
      status.projection_stage_generation,
      status.transition_evidence,
      ProjectionApplyStageResultState::kStaged,
      std::nullopt,
  };
  if (!ExactStagedResult(result, request)) {
    throw ManagerSceneStageEffectUncertainError(
        "scene owner staged status differs from the durable request",
        ProjectionApplyStatusQueryDTO{request.apply_id,
                                      request.apply_generation},
        status);
  }
  return result;
}

[[nodiscard]] ProjectionApplyStatusDTO
QueryStatus(const core::IManagerSceneProjectionHandoffPort &scene_owner,
            const ProjectionApplyStatusQueryDTO &query) {
  try {
    auto status = scene_owner.ProjectionApplyStatus(query);
    if (!status.presence_valid() || status.apply_id != query.apply_id ||
        status.apply_generation != query.apply_generation) {
      throw ManagerSceneStageEffectUncertainError(
          "scene owner returned malformed or foreign Apply status", query,
          status);
    }
    return status;
  } catch (const ManagerSceneStageEffectUncertainError &) {
    throw;
  } catch (const std::exception &error) {
    throw ManagerSceneStageEffectUncertainError(
        "scene owner status query failed: " + std::string(error.what()), query);
  } catch (...) {
    throw ManagerSceneStageEffectUncertainError(
        "scene owner status query failed", query);
  }
}

} // namespace

ManagerSceneStageEffectUncertainError::ManagerSceneStageEffectUncertainError(
    std::string detail, ProjectionApplyStatusQueryDTO query,
    std::optional<ProjectionApplyStatusDTO> observed_status,
    std::optional<ProjectionApplyStageResultDTO> observed_stage_result)
    : ManagerSceneStageCoordinatorError(std::move(detail)),
      recovery_query_(std::move(query)),
      observed_status_(std::move(observed_status)),
      observed_stage_result_(std::move(observed_stage_result)) {}

const ProjectionApplyStatusQueryDTO &
ManagerSceneStageEffectUncertainError::recovery_query() const noexcept {
  return recovery_query_;
}

const std::optional<ProjectionApplyStatusDTO> &
ManagerSceneStageEffectUncertainError::observed_status() const noexcept {
  return observed_status_;
}

const std::optional<ProjectionApplyStageResultDTO> &
ManagerSceneStageEffectUncertainError::observed_stage_result() const noexcept {
  return observed_stage_result_;
}

ManagerSceneStageCoordinator::ManagerSceneStageCoordinator(
    SqliteManagerApplyJournal &apply_journal,
    const deployment::IVerifiedRetainedBundleSource &retained_bundle_source,
    const core::IActiveBundlePublicationReader &publication_reader,
    core::IManagerSceneProjectionHandoffPort &scene_owner)
    : apply_journal_(apply_journal),
      retained_bundle_source_(retained_bundle_source),
      publication_reader_(publication_reader), scene_owner_(scene_owner) {}

ManagerSceneStageResult ManagerSceneStageCoordinator::StageOrRecover(
    const ApplyId &apply_id, const ApplyGeneration apply_generation) {
  std::lock_guard operation_lock(operation_mutex_);

  const auto apply = apply_journal_.ReadIntent(apply_id, apply_generation);
  const auto staged_ready =
      apply_journal_.ReadLatestRuntimeStagedReady(apply_id, apply_generation);
  if (!apply.has_value() || !staged_ready.has_value()) {
    Fail("scene stage requires exact durable Apply and runtime staged-ready "
         "records");
  }

  auto predecessor = LoadExactBundle(
      retained_bundle_source_,
      apply->intent.expected_active_projection_revision.retained_bundle_id);
  auto candidate = LoadExactBundle(retained_bundle_source_,
                                   apply->intent.candidate_retained_bundle_id);
  VerifyBundleBindings(apply->intent, *predecessor, *candidate);

  ManagerApplySceneStageIntentWriteResult durable_intent = [&] {
    const auto existing =
        apply_journal_.ReadSceneStageIntent(apply_id, apply_generation);
    if (existing.has_value()) {
      return ManagerApplySceneStageIntentWriteResult{*existing, false};
    }
    ManagerApplySceneStageIntentDTO intent{
        apply_id,
        apply_generation,
        staged_ready->staged_ready_digest,
        staged_ready->staged_ready.predecessor_publication_generation,
        staged_ready->staged_ready.predecessor_public_pointer_digest,
        apply->intent.expected_active_projection_revision,
        apply->intent.candidate_retained_bundle_id,
        apply->intent.candidate_graph_revision,
        DeriveTransitionRows(*predecessor, *candidate, *staged_ready),
    };
    return apply_journal_.EnsureSceneStageIntent(intent);
  }();

  const ProjectionApplyStageDTO request{
      apply_id,
      apply_generation,
      durable_intent.record.scene_stage_intent
          .expected_active_projection_revision,
      predecessor->value(),
      candidate->value(),
      durable_intent.record.scene_stage_intent.native_transition_rows,
  };
  if (!request.valid()) {
    Fail("durable scene-stage records reconstruct an invalid owner request");
  }

  if (const auto durable_staged =
          apply_journal_.ReadSceneStaged(apply_id, apply_generation);
      durable_staged.has_value()) {
    return {
        durable_intent,
        ManagerApplySceneStagedWriteResult{*durable_staged, false},
        ManagerSceneStagePath::kDurableStagedReplay,
    };
  }

  const ProjectionApplyStatusQueryDTO recovery_query{apply_id,
                                                     apply_generation};
  const auto persist_staged =
      [&](const ProjectionApplyStageResultDTO &result,
          const ManagerSceneStagePath path) -> ManagerSceneStageResult {
    try {
      const auto durable_staged = apply_journal_.EnsureSceneStaged({
          durable_intent.record.scene_stage_intent_digest,
          result,
      });
      return {durable_intent, durable_staged, path};
    } catch (const std::exception &error) {
      throw ManagerSceneStageEffectUncertainError(
          "scene staged evidence could not be made durable: " +
              std::string(error.what()),
          recovery_query, std::nullopt, result);
    } catch (...) {
      throw ManagerSceneStageEffectUncertainError(
          "scene staged evidence could not be made durable", recovery_query,
          std::nullopt, result);
    }
  };

  static_cast<void>(VerifyCurrentPredecessor(
      publication_reader_, durable_intent.record.scene_stage_intent));
  const auto status = QueryStatus(scene_owner_, recovery_query);
  if (status.state == ProjectionApplyStatusState::kStaged) {
    const auto result = StagedResultFromStatus(status, request);
    return persist_staged(result, ManagerSceneStagePath::kRecoveredOwnerStatus);
  }
  if (status.state != ProjectionApplyStatusState::kAbsent) {
    throw ManagerSceneStageEffectUncertainError(
        "scene owner is not absent or exactly staged for the durable Apply",
        recovery_query, status);
  }

  static_cast<void>(VerifyCurrentPredecessor(
      publication_reader_, durable_intent.record.scene_stage_intent));
  ProjectionApplyStageResultDTO result = [&] {
    try {
      return scene_owner_.StageCandidateProjection(request);
    } catch (const std::exception &error) {
      throw ManagerSceneStageEffectUncertainError(
          "scene Stage outcome is uncertain: " + std::string(error.what()),
          recovery_query);
    } catch (...) {
      throw ManagerSceneStageEffectUncertainError(
          "scene Stage outcome is uncertain", recovery_query);
    }
  }();
  if (!ExactStagedResult(result, request)) {
    if (result.presence_valid() &&
        result.state == ProjectionApplyStageResultState::kRejected &&
        result.apply_id == request.apply_id &&
        result.apply_generation == request.apply_generation &&
        result.expected_active_projection_revision ==
            request.expected_active_projection_revision) {
      Fail("scene owner rejected the exact durable stage request");
    }
    throw ManagerSceneStageEffectUncertainError(
        "scene owner returned uncertain, malformed, or foreign Stage "
        "evidence",
        recovery_query, std::nullopt, result);
  }

  return persist_staged(result, ManagerSceneStagePath::kStagedFromNewCall);
}

} // namespace manyforge::component_manager
