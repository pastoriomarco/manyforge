#include "manyforge_component_manager/manager_scene_stage_coordinator.hpp"

#include "native_projection_transition_internal.hpp"

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

[[nodiscard]] bool
SameDeploymentRevision(const core::ProjectionRevisionDTO &left,
                       const core::ProjectionRevisionDTO &right) noexcept {
  return left.retained_bundle_id == right.retained_bundle_id &&
         left.graph_revision == right.graph_revision;
}

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
      !SameDeploymentRevision(
          receipt.request.candidate_projection_revision,
          stage_intent.expected_active_projection_revision) ||
      receipt.request.candidate_retained_bundle_id !=
          stage_intent.expected_active_projection_revision.retained_bundle_id ||
      receipt.request.candidate_graph_revision !=
          stage_intent.expected_active_projection_revision.graph_revision) {
    Fail("scene stage predecessor publication differs from its durable "
         "intent");
  }
  return receipt;
}

[[nodiscard]] std::vector<core::ProjectionHealthDTO>
ObservePredecessorNativeHealth(
    const ActiveBundlePublicationReceiptDTO &current_publication,
    const core::ProjectionRevisionDTO &expected_predecessor,
    core::IManagerSceneProjectionHandoffPort &scene_owner,
    core::IManagerSceneProjectionBootstrapPort *bootstrap_scene_owner) {
  try {
    if (const auto *bootstrap =
            std::get_if<core::BootstrapPublicationTransactionDTO>(
                &current_publication.request.transaction_ref)) {
      if (bootstrap_scene_owner == nullptr) {
        Fail("native predecessor bootstrap status is unavailable");
      }
      const auto status = bootstrap_scene_owner->ProjectionBootstrapStatus(
          {bootstrap->bootstrap_id, bootstrap->bootstrap_generation});
      if (!status.presence_valid() ||
          status.state != core::ProjectionBootstrapStatusState::kFinalized ||
          status.canonical_empty_target !=
              std::optional<core::ProjectionRevisionDTO>{
                  expected_predecessor} ||
          status.publication_receipt !=
              std::optional<ActiveBundlePublicationReceiptDTO>{
                  current_publication} ||
          !status.finalized_aggregate_health.has_value() ||
          !status.finalized_aggregate_health->healthy_for_target(
              expected_predecessor)) {
        Fail("current bootstrap does not provide exact healthy native "
             "predecessor evidence");
      }
      return status.finalized_aggregate_health->native_projection_health;
    }
    const auto *named = std::get_if<core::NamedApplyPublicationTransactionDTO>(
        &current_publication.request.transaction_ref);
    if (named == nullptr) {
      Fail("current publication has an unknown transaction source");
    }
    const auto health = scene_owner.ActiveProjectionHealth();
    if (!health.matches_named_apply(
            named->apply_id, named->apply_generation,
            named->projection_stage_generation,
            current_publication.request.candidate_projection_revision))
      Fail("current named Apply native predecessor identity differs from the "
           "active publication");
    // These rows carry Scene's exact active membership even when runtime
    // convergence has already replaced a predecessor child. Do not require
    // aggregate live health here: DeriveNativeProjectionTransitionRows owns
    // the per-binding decision and accepts an unavailable row only when the
    // exact, already durable Manager withdrawal receipt names that runtime
    // epoch. Provenance and canonical membership remain fail-closed above and
    // in that derivation.
    return health.native_projection_health;
  } catch (const ManagerSceneStageCoordinatorError &) {
    throw;
  } catch (const std::exception &error) {
    Fail("native predecessor status observation failed: " +
         std::string(error.what()));
  } catch (...) {
    Fail("native predecessor status observation failed");
  }
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
    core::IManagerSceneProjectionHandoffPort &scene_owner,
    core::IManagerSceneProjectionBootstrapPort *bootstrap_scene_owner,
    IRuntimeUnitProcessPort *runtime_process_port)
    : apply_journal_(apply_journal),
      retained_bundle_source_(retained_bundle_source),
      publication_reader_(publication_reader), scene_owner_(scene_owner),
      bootstrap_scene_owner_(bootstrap_scene_owner),
      runtime_process_port_(runtime_process_port) {}

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

  const auto append_intent =
      [&](const PersistedManagerApplyRuntimeStagedReady &ready) {
        std::vector<core::ProjectionHealthDTO> predecessor_health;
        if (detail::HasNativeProjectionRequirements(predecessor->value())) {
          const auto current = VerifyCurrentPredecessor(
              publication_reader_,
              ManagerApplySceneStageIntentDTO{
                  apply_id,
                  apply_generation,
                  ready.staged_ready_digest,
                  ready.staged_ready.predecessor_publication_generation,
                  ready.staged_ready.predecessor_public_pointer_digest,
                  apply->intent.expected_active_projection_revision,
                  apply->intent.candidate_retained_bundle_id,
                  apply->intent.candidate_graph_revision,
                  {},
              });
          predecessor_health = ObservePredecessorNativeHealth(
              current, apply->intent.expected_active_projection_revision,
              scene_owner_, bootstrap_scene_owner_);
        }
        auto transition_rows = detail::DeriveNativeProjectionTransitionRows(
            predecessor->value(), candidate->value(),
            apply->intent.expected_active_projection_revision,
            predecessor_health, ready.staged_ready);
        ManagerApplySceneStageIntentDTO intent{
            apply_id,
            apply_generation,
            ready.staged_ready_digest,
            ready.staged_ready.predecessor_publication_generation,
            ready.staged_ready.predecessor_public_pointer_digest,
            apply->intent.expected_active_projection_revision,
            apply->intent.candidate_retained_bundle_id,
            apply->intent.candidate_graph_revision,
            std::move(transition_rows),
        };
        return apply_journal_.EnsureSceneStageIntent(intent);
      };

  auto durable_intent = [&] {
    const auto existing =
        apply_journal_.ReadSceneStageIntent(apply_id, apply_generation);
    return existing.has_value()
               ? ManagerApplySceneStageIntentWriteResult{*existing, false}
               : append_intent(*staged_ready);
  }();

  const auto make_request = [&] {
    return ProjectionApplyStageDTO{
        apply_id,
        apply_generation,
        durable_intent.record.scene_stage_intent
            .expected_active_projection_revision,
        predecessor->value(),
        candidate->value(),
        durable_intent.record.scene_stage_intent.native_transition_rows,
    };
  };
  const auto request = make_request();
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

  const auto cleanup_failed_candidate = [&] {
    // Stop only an exact Manager-owned process launched solely for this
    // candidate. A retained-exact process may still be the healthy
    // predecessor and remains authoritative.
    const auto runtime_intent =
        apply_journal_.ReadRuntimeConvergenceIntent(apply_id, apply_generation);
    const auto current_ready =
        apply_journal_.ReadLatestRuntimeStagedReady(apply_id, apply_generation);
    if (!runtime_intent.has_value() || !current_ready.has_value() ||
        current_ready->staged_ready_digest !=
            durable_intent.record.scene_stage_intent
                .runtime_staged_ready_digest) {
      Fail("failed candidate cleanup lacks its exact retained runtime "
           "evidence");
    }
    const auto has_added_manager_process = std::any_of(
        runtime_intent->runtime_convergence_intent.runtime_units.begin(),
        runtime_intent->runtime_convergence_intent.runtime_units.end(),
        [](const auto &plan) {
          return plan.lifecycle_control ==
                     core::RuntimeLifecycleControl::kManagerManaged &&
                 plan.candidate_disposition ==
                     RuntimeUnitCandidateDisposition::kAdded;
        });
    if (has_added_manager_process && runtime_process_port_ == nullptr) {
      Fail("scene rejected a Manager-owned candidate, but exact process "
           "cleanup is unavailable");
    }
    const auto &ready_rows = current_ready->staged_ready.runtime_units;
    for (const auto &plan :
         runtime_intent->runtime_convergence_intent.runtime_units) {
      if (plan.lifecycle_control !=
              core::RuntimeLifecycleControl::kManagerManaged ||
          plan.candidate_disposition !=
              RuntimeUnitCandidateDisposition::kAdded) {
        continue;
      }
      const auto evidence = std::lower_bound(
          ready_rows.begin(), ready_rows.end(), plan.runtime_unit_id,
          [](const auto &row, const auto &runtime_unit_id) {
            return row.runtime_unit_id < runtime_unit_id;
          });
      if (evidence == ready_rows.end() ||
          evidence->runtime_unit_id != plan.runtime_unit_id) {
        Fail("failed candidate cleanup cannot identify its exact runtime "
             "unit");
      }
      RuntimeUnitConvergenceQueryDTO cleanup_query{
          apply_id,
          apply_generation,
          runtime_intent->runtime_convergence_intent
              .candidate_retained_bundle_id,
          runtime_intent->runtime_convergence_intent.candidate_graph_revision,
          runtime_intent->runtime_convergence_intent_digest,
          plan,
      };
      try {
        const auto before =
            runtime_process_port_->ObserveProcess(cleanup_query);
        if (before.runtime_unit_id() != plan.runtime_unit_id.value() ||
            (before.state() == ProcessPresenceState::kPresent &&
             before.process_incarnation() != evidence->runtime_epoch.value())) {
          Fail("failed candidate cleanup observed a foreign or stale process "
               "instance");
        }
        if (before.state() == ProcessPresenceState::kPresent) {
          const auto stopped =
              runtime_process_port_->EnsureStopped(cleanup_query);
          if (stopped.runtime_unit_id() != plan.runtime_unit_id.value() ||
              stopped.state() != ProcessPresenceState::kAbsent) {
            Fail("Manager stop did not return exact process absence");
          }
        }
        const auto confirmed =
            runtime_process_port_->ObserveProcess(cleanup_query);
        if (confirmed.runtime_unit_id() != plan.runtime_unit_id.value() ||
            confirmed.state() != ProcessPresenceState::kAbsent) {
          Fail("Manager could not confirm failed candidate process "
               "termination");
        }
      } catch (const ManagerSceneStageCoordinatorError &) {
        throw;
      } catch (const std::exception &error) {
        Fail("failed candidate process cleanup failed: " +
             std::string(error.what()));
      } catch (...) {
        Fail("failed candidate process cleanup failed");
      }
    }
  };

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
  if (status.state == ProjectionApplyStatusState::kDiscarded) {
    cleanup_failed_candidate();
    Fail("scene owner definitively discarded the exact durable stage intent");
  }
  if (status.state != ProjectionApplyStatusState::kAbsent &&
      status.state != ProjectionApplyStatusState::kQuiescing) {
    throw ManagerSceneStageEffectUncertainError(
        "scene owner is not absent or exactly staged for the durable Apply",
        recovery_query, status);
  }

  // Authoritative absence makes an exact same-intent replay safe; quiescing
  // proves that Scene already captured the exact intent and needs the same
  // owner-level replay to settle/discard its recorded participants. Scene
  // recovers exact participant statuses and never submits a duplicate native
  // Stage job for an already captured intent.

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
  if (result.presence_valid() &&
      result.state == ProjectionApplyStageResultState::kAccepted &&
      result.apply_id == request.apply_id &&
      result.apply_generation == request.apply_generation &&
      result.expected_active_projection_revision ==
          request.expected_active_projection_revision) {
    throw ManagerSceneStagePending("scene Stage was accepted");
  }
  if (!ExactStagedResult(result, request)) {
    if (result.presence_valid() &&
        result.state == ProjectionApplyStageResultState::kRejected &&
        result.apply_id == request.apply_id &&
        result.apply_generation == request.apply_generation &&
        result.expected_active_projection_revision ==
            request.expected_active_projection_revision) {
      // A participant rejection is not proof that Scene has finished
      // discarding every sibling candidate. Stop the Manager-owned process
      // only after Scene reports durable terminal discard (or authoritative
      // absence when it captured no transition at all).
      const auto rejected_status = QueryStatus(scene_owner_, recovery_query);
      if (rejected_status.state == ProjectionApplyStatusState::kQuiescing) {
        throw ManagerSceneStagePending(
            "scene rejected candidate staging and is settling exact sibling "
            "cleanup");
      }
      if (rejected_status.state == ProjectionApplyStatusState::kDiscarded ||
          rejected_status.state == ProjectionApplyStatusState::kAbsent) {
        cleanup_failed_candidate();
        Fail("scene owner rejected the exact durable stage request" +
             (result.error.has_value()
                  ? " [" + result.error->code + "]: " + result.error->message
                  : std::string{}));
      }
      throw ManagerSceneStageEffectUncertainError(
          "scene rejected candidate staging without terminal cleanup "
          "evidence",
          recovery_query, rejected_status, result);
    }
    throw ManagerSceneStageEffectUncertainError(
        "scene owner returned uncertain, malformed, or foreign Stage "
        "evidence" +
            (result.error.has_value()
                 ? " [" + result.error->code + "]: " + result.error->message
                 : std::string{}),
        recovery_query, std::nullopt, result);
  }

  return persist_staged(result, ManagerSceneStagePath::kStagedFromNewCall);
}

} // namespace manyforge::component_manager
