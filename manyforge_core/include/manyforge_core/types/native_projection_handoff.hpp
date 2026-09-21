#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/projection_apply.hpp>
#include <manyforge_core/types/projection_bootstrap.hpp>
#include <optional>
#include <vector>

namespace manyforge::core {

/// Complete current native runtime set observed by Component Manager for the
/// already-selected structural deployment.
///
/// The scene owner independently validates the provider/runtime requirements
/// against the active retained bundle. This value carries no process address,
/// PID, discovery result, or caller-selected executable.
struct ActiveProjectionRuntimeSetDTO final {
  ProjectionRevisionDTO active_projection_revision;
  std::vector<ProjectionNativeRuntimeBindingDTO> native_runtime_bindings;

  [[nodiscard]] bool valid() const noexcept {
    if (!active_projection_revision.valid() ||
        !std::is_sorted(native_runtime_bindings.begin(), native_runtime_bindings.end(),
                        CanonicalProjectionNativeBindingLess)) {
      return false;
    }
    for (std::size_t index = 0U; index < native_runtime_bindings.size(); ++index) {
      const auto& binding = native_runtime_bindings[index];
      if (!binding.valid()) {
        return false;
      }
      if (index != 0U) {
        const auto& prior = native_runtime_bindings[index - 1U];
        if (prior.provider_binding_id == binding.provider_binding_id &&
            prior.provider_binding_revision == binding.provider_binding_revision) {
          return false;
        }
      }
    }
    return true;
  }

  friend bool operator==(const ActiveProjectionRuntimeSetDTO&,
                         const ActiveProjectionRuntimeSetDTO&) = default;
};

enum class ActiveProjectionRuntimeRecoveryState {
  kProgressing,
  kCurrent,
  kRecovered,
};

/// Durable result of reconciling a Manager-observed runtime set with the
/// scene owner's current active native projections.
struct ActiveProjectionRuntimeRecoveryResultDTO final {
  ActiveProjectionRuntimeSetDTO runtime_set;
  ActiveProjectionRuntimeRecoveryState state;
  std::optional<NativeProjectionRecoveryGeneration> recovery_generation;

  [[nodiscard]] bool valid() const noexcept {
    if (!runtime_set.valid() ||
        (recovery_generation.has_value() && !recovery_generation->nonzero())) {
      return false;
    }
    switch (state) {
      case ActiveProjectionRuntimeRecoveryState::kProgressing:
        return recovery_generation.has_value();
      case ActiveProjectionRuntimeRecoveryState::kCurrent:
        return true;
      case ActiveProjectionRuntimeRecoveryState::kRecovered:
        return recovery_generation.has_value();
    }
    return false;
  }
};

/// Exact input for one native participant during initial bootstrap.
struct NativeProjectionBootstrapRebuildDTO final {
  BootstrapId bootstrap_id;
  BootstrapGeneration bootstrap_generation;
  ProjectionNativeRuntimeBindingDTO native_runtime_binding;
  ProjectionRevisionDTO canonical_empty_target;
  RetainedProductDTO planning_collision_model;
  LogicalSceneSnapshotRoot initialized_logical_scene;

  [[nodiscard]] bool valid() const noexcept {
    return !bootstrap_id.empty() && bootstrap_generation.nonzero() &&
           native_runtime_binding.valid() && canonical_empty_target.valid() &&
           planning_collision_model.product_kind == RetainedProductKind::kPlanningCollisionModel &&
           !planning_collision_model.product_id.empty() &&
           !planning_collision_model.product_contract_id.empty() &&
           !planning_collision_model.product_contract_revision.empty() &&
           initialized_logical_scene.has_exact_identity(
               {canonical_empty_target.scene_revision,
                initialized_logical_scene.shared()->cell_id});
  }
};

struct NativeProjectionBootstrapStatusQueryDTO final {
  BootstrapId bootstrap_id;
  BootstrapGeneration bootstrap_generation;
  ProjectionNativeRuntimeBindingDTO native_runtime_binding;
  ProjectionRevisionDTO canonical_empty_target;

  [[nodiscard]] bool valid() const noexcept {
    return !bootstrap_id.empty() && bootstrap_generation.nonzero() &&
           native_runtime_binding.valid() && canonical_empty_target.valid();
  }
};

/// Exact no-active-effect candidate projection stage for one retained/added
/// native participant.
struct NativeProjectionStageDTO final {
  ApplyId apply_id;
  ApplyGeneration apply_generation;
  ProjectionStageGeneration projection_stage_generation;
  ProjectionTransitionRowDTO transition_row;
  ProjectionRevisionDTO expected_predecessor_projection_revision;
  ProjectionRevisionDTO candidate_projection_revision;
  RetainedProductDTO planning_collision_model;
  LogicalSceneSnapshotRoot frozen_logical_scene;

  [[nodiscard]] bool valid() const noexcept {
    return !apply_id.empty() && apply_generation.nonzero() &&
           projection_stage_generation.nonzero() && transition_row.valid() &&
           !transition_row.managed_withdrawal &&
           transition_row.disposition != ProjectionTransitionDisposition::kRemoved &&
           transition_row.matches_predecessor(expected_predecessor_projection_revision) &&
           candidate_projection_revision.valid() &&
           planning_collision_model.product_kind == RetainedProductKind::kPlanningCollisionModel &&
           !planning_collision_model.product_id.empty() &&
           !planning_collision_model.product_contract_id.empty() &&
           !planning_collision_model.product_contract_revision.empty() &&
           frozen_logical_scene.has_exact_identity({candidate_projection_revision.scene_revision,
                                                    frozen_logical_scene.shared()->cell_id});
  }

  friend bool operator==(const NativeProjectionStageDTO& lhs, const NativeProjectionStageDTO& rhs) {
    return lhs.apply_id == rhs.apply_id && lhs.apply_generation == rhs.apply_generation &&
           lhs.projection_stage_generation == rhs.projection_stage_generation &&
           lhs.transition_row == rhs.transition_row &&
           lhs.expected_predecessor_projection_revision ==
               rhs.expected_predecessor_projection_revision &&
           lhs.candidate_projection_revision == rhs.candidate_projection_revision &&
           lhs.planning_collision_model == rhs.planning_collision_model &&
           *lhs.frozen_logical_scene.shared() == *rhs.frozen_logical_scene.shared();
  }
};

enum class NativeProjectionStageResultState {
  kAccepted,
  kStaged,
  kRejected,
  kFailedProjectionUncertain,
};

struct NativeProjectionStageResultDTO final {
  ApplyId apply_id;
  ApplyGeneration apply_generation;
  ProjectionStageGeneration projection_stage_generation;
  ProjectionTransitionRowDTO transition_row;
  ProjectionRevisionDTO expected_predecessor_projection_revision;
  ProjectionRevisionDTO candidate_projection_revision;
  NativeProjectionStageResultState state;
  std::optional<ProjectionRevisionDTO> acknowledged_staged_candidate_revision;
  std::optional<ProjectionRevisionDTO> observed_local_active;
  std::optional<std::uint64_t> native_projection_generation;
  std::optional<ContentDigest> projection_content_digest;
  ProjectionHealthDTO resulting_health;
  std::optional<ErrorDTO> error;

  [[nodiscard]] bool presence_valid() const noexcept {
    if (apply_id.empty() || !apply_generation.nonzero() || !projection_stage_generation.nonzero() ||
        !transition_row.valid() || transition_row.managed_withdrawal ||
        transition_row.disposition == ProjectionTransitionDisposition::kRemoved ||
        !transition_row.matches_predecessor(expected_predecessor_projection_revision) ||
        !candidate_projection_revision.valid() || !resulting_health.valid() ||
        resulting_health.native_runtime_binding != transition_row.native_runtime_binding ||
        resulting_health.active_projection_revision != observed_local_active ||
        (native_projection_generation.has_value() && *native_projection_generation == 0U) ||
        (projection_content_digest.has_value() && projection_content_digest->empty())) {
      return false;
    }
    const ProjectionStagedCandidateDTO staged{
        apply_id, apply_generation, projection_stage_generation, candidate_projection_revision};
    switch (state) {
      case NativeProjectionStageResultState::kAccepted:
        return !acknowledged_staged_candidate_revision.has_value() &&
               (observed_local_active == transition_row.expected_local_active ||
                resulting_health.state == ProjectionHealthState::kRebuilding) &&
               !resulting_health.staged_candidate.has_value() &&
               !resulting_health.error.has_value() && !error.has_value();
      case NativeProjectionStageResultState::kStaged:
        return acknowledged_staged_candidate_revision ==
                   std::optional<ProjectionRevisionDTO>{candidate_projection_revision} &&
               observed_local_active == transition_row.expected_local_active &&
               resulting_health.staged_candidate ==
                   std::optional<ProjectionStagedCandidateDTO>{staged} &&
               !resulting_health.error.has_value() && !error.has_value();
      case NativeProjectionStageResultState::kRejected:
        return !acknowledged_staged_candidate_revision.has_value() &&
               observed_local_active == transition_row.expected_local_active && error.has_value();
      case NativeProjectionStageResultState::kFailedProjectionUncertain:
        return !acknowledged_staged_candidate_revision.has_value() && error.has_value() &&
               (resulting_health.state == ProjectionHealthState::kDesynchronized ||
                resulting_health.state == ProjectionHealthState::kUnavailable);
    }
    return false;
  }
};

struct NativeProjectionActivationDTO final {
  ApplyId apply_id;
  ApplyGeneration apply_generation;
  ProjectionStageGeneration projection_stage_generation;
  ProjectionTransitionRowDTO transition_row;
  ProjectionRevisionDTO expected_predecessor_projection_revision;
  ProjectionRevisionDTO candidate_projection_revision;

  [[nodiscard]] bool valid() const noexcept {
    return !apply_id.empty() && apply_generation.nonzero() &&
           projection_stage_generation.nonzero() && transition_row.valid() &&
           !transition_row.managed_withdrawal &&
           transition_row.disposition != ProjectionTransitionDisposition::kRemoved &&
           transition_row.matches_predecessor(expected_predecessor_projection_revision) &&
           candidate_projection_revision.valid();
  }
};

enum class NativeProjectionActivationResultState {
  kAccepted,
  kActivated,
  kStale,
  kFailedProjectionUncertain,
};

struct NativeProjectionActivationResultDTO final {
  NativeProjectionActivationDTO request;
  NativeProjectionActivationResultState state;
  std::optional<ProjectionRevisionDTO> acknowledged_candidate_projection_revision;
  std::optional<std::uint64_t> native_projection_generation;
  std::optional<ContentDigest> projection_content_digest;
  ProjectionHealthDTO resulting_health;
  std::optional<ErrorDTO> error;

  [[nodiscard]] bool presence_valid() const noexcept {
    if (!request.valid() || !resulting_health.valid() ||
        resulting_health.native_runtime_binding != request.transition_row.native_runtime_binding ||
        (native_projection_generation.has_value() && *native_projection_generation == 0U) ||
        (projection_content_digest.has_value() && projection_content_digest->empty())) {
      return false;
    }
    switch (state) {
      case NativeProjectionActivationResultState::kAccepted:
        return !acknowledged_candidate_projection_revision.has_value() &&
               (resulting_health.active_projection_revision ==
                    request.transition_row.expected_local_active ||
                resulting_health.state == ProjectionHealthState::kRebuilding) &&
               !resulting_health.error.has_value() && !error.has_value();
      case NativeProjectionActivationResultState::kActivated:
        return acknowledged_candidate_projection_revision ==
                   std::optional<ProjectionRevisionDTO>{request.candidate_projection_revision} &&
               resulting_health.healthy_for_target(request.candidate_projection_revision) &&
               !error.has_value();
      case NativeProjectionActivationResultState::kStale:
        return !acknowledged_candidate_projection_revision.has_value() && error.has_value();
      case NativeProjectionActivationResultState::kFailedProjectionUncertain:
        return !acknowledged_candidate_projection_revision.has_value() && error.has_value() &&
               (resulting_health.state == ProjectionHealthState::kDesynchronized ||
                resulting_health.state == ProjectionHealthState::kUnavailable);
    }
    return false;
  }
};

struct NativeProjectionRetireDTO final {
  ApplyId apply_id;
  ApplyGeneration apply_generation;
  ProjectionStageGeneration projection_stage_generation;
  ProjectionTransitionRowDTO transition_row;
  ProjectionRevisionDTO expected_predecessor_projection_revision;
  ProjectionRevisionDTO candidate_projection_revision;

  [[nodiscard]] bool valid() const noexcept {
    return !apply_id.empty() && apply_generation.nonzero() &&
           projection_stage_generation.nonzero() && transition_row.valid() &&
           !transition_row.managed_withdrawal &&
           transition_row.disposition == ProjectionTransitionDisposition::kRemoved &&
           transition_row.matches_predecessor(expected_predecessor_projection_revision) &&
           candidate_projection_revision.valid();
  }
};

enum class NativeProjectionRetireResultState {
  kRetired,
  kAlreadyAbsent,
  kStale,
  kFailedProjectionUncertain,
};

struct NativeProjectionRetireResultDTO final {
  NativeProjectionRetireDTO request;
  NativeProjectionRetireResultState state;
  std::optional<ProjectionRevisionDTO> observed_local_active;
  std::optional<std::uint64_t> native_projection_generation;
  std::optional<ContentDigest> projection_content_digest;
  ProjectionHealthDTO resulting_health;
  std::optional<ErrorDTO> error;

  [[nodiscard]] bool presence_valid() const noexcept {
    if (!request.valid() || !resulting_health.valid() ||
        resulting_health.native_runtime_binding != request.transition_row.native_runtime_binding ||
        resulting_health.active_projection_revision != observed_local_active ||
        (native_projection_generation.has_value() && *native_projection_generation == 0U) ||
        (projection_content_digest.has_value() && projection_content_digest->empty())) {
      return false;
    }
    switch (state) {
      case NativeProjectionRetireResultState::kRetired:
      case NativeProjectionRetireResultState::kAlreadyAbsent:
        return !observed_local_active.has_value() &&
               !resulting_health.staged_candidate.has_value() &&
               !resulting_health.error.has_value() && !error.has_value();
      case NativeProjectionRetireResultState::kStale:
        return error.has_value();
      case NativeProjectionRetireResultState::kFailedProjectionUncertain:
        return error.has_value() &&
               (resulting_health.state == ProjectionHealthState::kDesynchronized ||
                resulting_health.state == ProjectionHealthState::kUnavailable);
    }
    return false;
  }
};

struct NativeProjectionRetireStatusQueryDTO final {
  NativeProjectionRetireDTO request;

  [[nodiscard]] bool valid() const noexcept { return request.valid(); }
};

enum class NativeProjectionRetireStatusState {
  kPending,
  kRetired,
  kAlreadyAbsent,
  kStale,
  kFailedProjectionUncertain,
};

struct NativeProjectionRetireStatusDTO final {
  NativeProjectionRetireDTO request;
  NativeProjectionRetireStatusState state;
  std::optional<ProjectionRevisionDTO> observed_local_active;
  std::optional<std::uint64_t> native_projection_generation;
  std::optional<ContentDigest> projection_content_digest;
  ProjectionHealthDTO resulting_health;
  std::optional<ErrorDTO> error;

  [[nodiscard]] bool presence_valid() const noexcept {
    if (!request.valid() || !resulting_health.valid() ||
        resulting_health.native_runtime_binding != request.transition_row.native_runtime_binding ||
        resulting_health.active_projection_revision != observed_local_active) {
      return false;
    }
    switch (state) {
      case NativeProjectionRetireStatusState::kPending:
        return observed_local_active == request.transition_row.expected_local_active &&
               !error.has_value();
      case NativeProjectionRetireStatusState::kRetired:
      case NativeProjectionRetireStatusState::kAlreadyAbsent:
        return !observed_local_active.has_value() && !error.has_value();
      case NativeProjectionRetireStatusState::kStale:
      case NativeProjectionRetireStatusState::kFailedProjectionUncertain:
        return error.has_value();
    }
    return false;
  }
};

struct NativeProjectionDiscardDTO final {
  ApplyId apply_id;
  ApplyGeneration apply_generation;
  ProjectionStageGeneration projection_stage_generation;
  ProjectionTransitionRowDTO transition_row;
  ProjectionRevisionDTO candidate_projection_revision;

  [[nodiscard]] bool valid() const noexcept {
    return !apply_id.empty() && apply_generation.nonzero() &&
           projection_stage_generation.nonzero() && transition_row.valid() &&
           !transition_row.managed_withdrawal &&
           transition_row.disposition != ProjectionTransitionDisposition::kRemoved &&
           candidate_projection_revision.valid();
  }
};

enum class NativeProjectionDiscardResultState {
  kDiscarded,
  kAlreadyAbsent,
  kFailedProjectionUncertain,
};

struct NativeProjectionDiscardResultDTO final {
  NativeProjectionDiscardDTO request;
  NativeProjectionDiscardResultState state;
  std::optional<ProjectionRevisionDTO> observed_local_active;
  std::optional<std::uint64_t> native_projection_generation;
  std::optional<ContentDigest> projection_content_digest;
  ProjectionHealthDTO resulting_health;
  std::optional<ErrorDTO> error;

  [[nodiscard]] bool presence_valid() const noexcept {
    if (!request.valid() || !resulting_health.valid() ||
        resulting_health.native_runtime_binding != request.transition_row.native_runtime_binding ||
        resulting_health.active_projection_revision != observed_local_active) {
      return false;
    }
    switch (state) {
      case NativeProjectionDiscardResultState::kDiscarded:
      case NativeProjectionDiscardResultState::kAlreadyAbsent:
        return observed_local_active == request.transition_row.expected_local_active &&
               !resulting_health.staged_candidate.has_value() && !error.has_value();
      case NativeProjectionDiscardResultState::kFailedProjectionUncertain:
        return error.has_value();
    }
    return false;
  }
};

struct NativeProjectionStatusQueryDTO final {
  ApplyId apply_id;
  ApplyGeneration apply_generation;
  ProjectionStageGeneration projection_stage_generation;
  ProjectionTransitionRowDTO transition_row;

  [[nodiscard]] bool valid() const noexcept {
    return !apply_id.empty() && apply_generation.nonzero() &&
           projection_stage_generation.nonzero() && transition_row.valid() &&
           !transition_row.managed_withdrawal &&
           transition_row.disposition != ProjectionTransitionDisposition::kRemoved;
  }
};

enum class NativeProjectionStatusState {
  kAbsent,
  kProgressing,
  kStaged,
  kActivated,
  kDiscarded,
  kFailedProjectionUncertain,
};

struct NativeProjectionStatusDTO final {
  NativeProjectionStatusQueryDTO query;
  NativeProjectionStatusState state;
  std::optional<ProjectionRevisionDTO> staged_or_active_candidate_revision;
  std::optional<ProjectionRevisionDTO> observed_local_active;
  std::optional<std::uint64_t> native_projection_generation;
  std::optional<ContentDigest> projection_content_digest;
  ProjectionHealthDTO resulting_health;
  std::optional<ErrorDTO> error;

  [[nodiscard]] bool presence_valid() const noexcept {
    if (!query.valid() || !resulting_health.valid() ||
        resulting_health.native_runtime_binding != query.transition_row.native_runtime_binding ||
        resulting_health.active_projection_revision != observed_local_active) {
      return false;
    }
    switch (state) {
      case NativeProjectionStatusState::kAbsent:
        return !staged_or_active_candidate_revision.has_value() && !error.has_value();
      case NativeProjectionStatusState::kProgressing:
        return staged_or_active_candidate_revision.has_value() &&
               staged_or_active_candidate_revision->valid() &&
               !resulting_health.staged_candidate.has_value() &&
               !error.has_value();
      case NativeProjectionStatusState::kStaged:
        return staged_or_active_candidate_revision.has_value() &&
               resulting_health.staged_candidate.has_value() && !error.has_value();
      case NativeProjectionStatusState::kActivated:
        return staged_or_active_candidate_revision.has_value() &&
               observed_local_active == staged_or_active_candidate_revision &&
               !resulting_health.staged_candidate.has_value() && !error.has_value();
      case NativeProjectionStatusState::kDiscarded:
        return !staged_or_active_candidate_revision.has_value() &&
               !resulting_health.staged_candidate.has_value() && !error.has_value();
      case NativeProjectionStatusState::kFailedProjectionUncertain:
        return error.has_value();
    }
    return false;
  }
};

}  // namespace manyforge::core
