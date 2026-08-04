#include "manyforge_component_manager/manager_scene_activation_coordinator.hpp"

#include <exception>
#include <utility>

namespace manyforge::component_manager {
namespace {

using core::ApplyGeneration;
using core::ApplyId;
using core::ProjectionApplyActivationDTO;
using core::ProjectionApplyActivationResultDTO;
using core::ProjectionApplyActivationResultState;
using core::ProjectionApplyStatusDTO;
using core::ProjectionApplyStatusQueryDTO;
using core::ProjectionApplyStatusState;

[[noreturn]] void Fail(const std::string &detail) {
  throw ManagerSceneActivationCoordinatorError(detail);
}

[[nodiscard]] bool
ExactActivatedResult(const ProjectionApplyActivationResultDTO &result,
                     const ManagerApplyActivationSelectionDTO &selection,
                     const ManagerApplySceneStagedDTO &staged) {
  if (result.state != ProjectionApplyActivationResultState::kActivated ||
      !result.presence_valid() || result.apply_id != selection.apply_id ||
      result.apply_generation != selection.apply_generation ||
      result.expected_active_projection_revision !=
          selection.predecessor_projection_revision ||
      result.projection_stage_generation !=
          selection.projection_stage_generation ||
      result.candidate_projection_revision !=
          selection.candidate_projection_revision ||
      result.acknowledged_candidate_projection_revision !=
          std::optional<core::ProjectionRevisionDTO>{
              selection.candidate_projection_revision} ||
      !staged.staged_result.transition_evidence.has_value() ||
      result.transition_evidence.transition_rows.size() !=
          staged.staged_result.transition_evidence->transition_rows.size()) {
    return false;
  }
  for (std::size_t index = 0U;
       index < result.transition_evidence.transition_rows.size(); ++index) {
    if (result.transition_evidence.transition_rows[index].transition_row !=
        staged.staged_result.transition_evidence->transition_rows[index]
            .transition_row) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] ProjectionApplyStatusDTO
QueryStatus(const core::IManagerSceneProjectionHandoffPort &scene_owner,
            const ProjectionApplyStatusQueryDTO &query) {
  try {
    auto status = scene_owner.ProjectionApplyStatus(query);
    if (!status.presence_valid() || status.apply_id != query.apply_id ||
        status.apply_generation != query.apply_generation) {
      throw ManagerSceneActivationEffectUncertainError(
          "scene owner returned malformed or foreign activation status", query,
          status);
    }
    return status;
  } catch (const ManagerSceneActivationEffectUncertainError &) {
    throw;
  } catch (const std::exception &error) {
    throw ManagerSceneActivationEffectUncertainError(
        "scene activation status query failed: " + std::string(error.what()),
        query);
  } catch (...) {
    throw ManagerSceneActivationEffectUncertainError(
        "scene activation status query failed", query);
  }
}

[[nodiscard]] ProjectionApplyActivationResultDTO
ActivatedResultFromStatus(const ProjectionApplyStatusDTO &status,
                          const ManagerApplyActivationSelectionDTO &selection,
                          const ManagerApplySceneStagedDTO &staged) {
  if (status.state != ProjectionApplyStatusState::kActivated ||
      !status.predecessor_projection_revision.has_value() ||
      !status.projection_stage_generation.has_value() ||
      !status.candidate_projection_revision.has_value() ||
      !status.transition_evidence.has_value()) {
    throw ManagerSceneActivationEffectUncertainError(
        "scene owner does not expose exact activated recovery evidence",
        {selection.apply_id, selection.apply_generation}, status);
  }
  ProjectionApplyActivationResultDTO result{
      status.apply_id,
      status.apply_generation,
      *status.predecessor_projection_revision,
      *status.projection_stage_generation,
      *status.candidate_projection_revision,
      ProjectionApplyActivationResultState::kActivated,
      status.candidate_projection_revision,
      *status.transition_evidence,
      std::nullopt,
  };
  if (!ExactActivatedResult(result, selection, staged)) {
    throw ManagerSceneActivationEffectUncertainError(
        "scene owner activated status differs from durable selection",
        {selection.apply_id, selection.apply_generation}, status);
  }
  return result;
}

} // namespace

ManagerSceneActivationEffectUncertainError::
    ManagerSceneActivationEffectUncertainError(
        std::string detail, ProjectionApplyStatusQueryDTO recovery_query,
        std::optional<ProjectionApplyStatusDTO> observed_status,
        std::optional<ProjectionApplyActivationResultDTO>
            observed_activation_result)
    : ManagerSceneActivationCoordinatorError(std::move(detail)),
      recovery_query_(std::move(recovery_query)),
      observed_status_(std::move(observed_status)),
      observed_activation_result_(std::move(observed_activation_result)) {}

const ProjectionApplyStatusQueryDTO &
ManagerSceneActivationEffectUncertainError::recovery_query() const noexcept {
  return recovery_query_;
}

const std::optional<ProjectionApplyStatusDTO> &
ManagerSceneActivationEffectUncertainError::observed_status() const noexcept {
  return observed_status_;
}

const std::optional<ProjectionApplyActivationResultDTO> &
ManagerSceneActivationEffectUncertainError::observed_activation_result()
    const noexcept {
  return observed_activation_result_;
}

ManagerSceneActivationCoordinator::ManagerSceneActivationCoordinator(
    SqliteManagerApplyJournal &apply_journal,
    core::IManagerSceneProjectionHandoffPort &scene_owner)
    : apply_journal_(apply_journal), scene_owner_(scene_owner) {}

ManagerSceneActivationResult
ManagerSceneActivationCoordinator::ActivateOrRecover(
    const ApplyId &apply_id, const ApplyGeneration apply_generation) {
  std::lock_guard operation_lock(operation_mutex_);
  const auto staged =
      apply_journal_.ReadSceneStaged(apply_id, apply_generation);
  if (!staged.has_value()) {
    Fail("scene activation requires exact durable staged evidence");
  }
  const auto &stage_result = staged->scene_staged.staged_result;
  if (!stage_result.projection_stage_generation.has_value() ||
      !stage_result.candidate_projection_revision.has_value()) {
    Fail("durable staged evidence lacks frozen activation identities");
  }
  const ManagerApplyActivationSelectionDTO selection{
      staged->scene_staged_digest,
      apply_id,
      apply_generation,
      stage_result.expected_active_projection_revision,
      *stage_result.projection_stage_generation,
      *stage_result.candidate_projection_revision,
  };
  const auto durable_selection =
      apply_journal_.EnsureActivationSelection(selection);
  if (const auto durable =
          apply_journal_.ReadSceneActivated(apply_id, apply_generation);
      durable.has_value()) {
    return {
        durable_selection,
        ManagerApplySceneActivatedWriteResult{*durable, false},
        ManagerSceneActivationPath::kDurableActivatedReplay,
    };
  }

  const ProjectionApplyStatusQueryDTO recovery_query{apply_id,
                                                     apply_generation};
  const auto persist = [&](const ProjectionApplyActivationResultDTO &result,
                           const ManagerSceneActivationPath path) {
    if (!ExactActivatedResult(result, selection, staged->scene_staged)) {
      throw ManagerSceneActivationEffectUncertainError(
          "scene activation result differs from durable selection",
          recovery_query, std::nullopt, result);
    }
    try {
      auto durable = apply_journal_.EnsureSceneActivated(
          {durable_selection.record.activation_selection_digest, result});
      return ManagerSceneActivationResult{durable_selection, durable, path};
    } catch (const std::exception &error) {
      throw ManagerSceneActivationEffectUncertainError(
          "scene activation evidence could not be made durable: " +
              std::string(error.what()),
          recovery_query, std::nullopt, result);
    } catch (...) {
      throw ManagerSceneActivationEffectUncertainError(
          "scene activation evidence could not be made durable", recovery_query,
          std::nullopt, result);
    }
  };

  const auto status = QueryStatus(scene_owner_, recovery_query);
  if (status.state == ProjectionApplyStatusState::kActivated) {
    return persist(
        ActivatedResultFromStatus(status, selection, staged->scene_staged),
        ManagerSceneActivationPath::kRecoveredOwnerStatus);
  }
  if (status.state != ProjectionApplyStatusState::kStaged) {
    throw ManagerSceneActivationEffectUncertainError(
        "selected scene transition is not exactly staged or activated",
        recovery_query, status);
  }

  const ProjectionApplyActivationDTO request{
      apply_id,
      apply_generation,
      selection.predecessor_projection_revision,
      selection.projection_stage_generation,
      selection.candidate_projection_revision,
  };
  ProjectionApplyActivationResultDTO result = [&] {
    try {
      return scene_owner_.ActivateStagedProjection(request);
    } catch (const std::exception &error) {
      throw ManagerSceneActivationEffectUncertainError(
          "scene activation outcome is uncertain: " + std::string(error.what()),
          recovery_query);
    } catch (...) {
      throw ManagerSceneActivationEffectUncertainError(
          "scene activation outcome is uncertain", recovery_query);
    }
  }();
  return persist(result, ManagerSceneActivationPath::kActivatedFromNewCall);
}

} // namespace manyforge::component_manager
