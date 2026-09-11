#pragma once

#include <cstddef>
#include <manyforge_core/types/active_bundle_publication.hpp>
#include <manyforge_core/types/managed_runtime_withdrawal.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <manyforge_core/types/retained_product.hpp>
#include <optional>
#include <string_view>
#include <vector>

namespace manyforge::core {

enum class ProjectionTransitionDisposition {
  kRetained,
  kAdded,
  kRemoved,
};

[[nodiscard]] constexpr std::string_view to_string(
    ProjectionTransitionDisposition disposition) noexcept {
  switch (disposition) {
    case ProjectionTransitionDisposition::kRetained:
      return "retained";
    case ProjectionTransitionDisposition::kAdded:
      return "added";
    case ProjectionTransitionDisposition::kRemoved:
      return "removed";
  }
  return {};
}

/// `expected_local_active == std::nullopt` is the closed `absent`
/// alternative; a value is the `exact_revision` alternative.
struct ProjectionTransitionRowDTO final {
  ProjectionNativeRuntimeBindingDTO native_runtime_binding;
  ProjectionTransitionDisposition disposition;
  std::optional<ProjectionRevisionDTO> expected_local_active;
  std::optional<ManagedRuntimeWithdrawalDTO> managed_withdrawal{};

  [[nodiscard]] bool valid() const noexcept {
    if (managed_withdrawal &&
        (disposition != ProjectionTransitionDisposition::kRemoved || !managed_withdrawal->valid() ||
         managed_withdrawal->runtime_unit_id != native_runtime_binding.runtime_unit_id ||
         managed_withdrawal->runtime_epoch != native_runtime_binding.runtime_epoch))
      return false;
    switch (disposition) {
      case ProjectionTransitionDisposition::kRetained:
      case ProjectionTransitionDisposition::kRemoved:
        return native_runtime_binding.valid() && expected_local_active.has_value() &&
               expected_local_active->valid();
      case ProjectionTransitionDisposition::kAdded:
        return native_runtime_binding.valid() && !expected_local_active.has_value();
      default:
        return false;
    }
  }

  [[nodiscard]] bool matches_predecessor(const ProjectionRevisionDTO& predecessor) const noexcept {
    if (!valid() || !predecessor.valid()) {
      return false;
    }
    switch (disposition) {
      case ProjectionTransitionDisposition::kRetained:
      case ProjectionTransitionDisposition::kRemoved:
        return *expected_local_active == predecessor;
      case ProjectionTransitionDisposition::kAdded:
        return true;
    }
    return false;
  }

  friend bool operator==(const ProjectionTransitionRowDTO&,
                         const ProjectionTransitionRowDTO&) = default;
};

[[nodiscard]] inline bool CanonicalProjectionTransitionRows(
    const std::vector<ProjectionTransitionRowDTO>& rows) noexcept {
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    if (!rows[index].valid()) {
      return false;
    }
    if (index != 0U &&
        !CanonicalProjectionNativeBindingLess(rows[index - 1U].native_runtime_binding,
                                              rows[index].native_runtime_binding)) {
      return false;
    }
  }
  return true;
}

enum class ProjectionTransitionRowEvidenceState {
  kPending,
  kStagedNoActiveEffect,
  kCandidateActive,
  kRemovedRetired,
  kCandidateStageDiscarded,
  kPredecessorUnchanged,
  kUncertain,
};

[[nodiscard]] constexpr std::string_view to_string(
    ProjectionTransitionRowEvidenceState state) noexcept {
  switch (state) {
    case ProjectionTransitionRowEvidenceState::kPending:
      return "pending";
    case ProjectionTransitionRowEvidenceState::kStagedNoActiveEffect:
      return "staged_no_active_effect";
    case ProjectionTransitionRowEvidenceState::kCandidateActive:
      return "candidate_active";
    case ProjectionTransitionRowEvidenceState::kRemovedRetired:
      return "removed_retired";
    case ProjectionTransitionRowEvidenceState::kCandidateStageDiscarded:
      return "candidate_stage_discarded";
    case ProjectionTransitionRowEvidenceState::kPredecessorUnchanged:
      return "predecessor_unchanged";
    case ProjectionTransitionRowEvidenceState::kUncertain:
      return "uncertain";
  }
  return {};
}

struct ProjectionTransitionRowEvidenceDTO final {
  ProjectionTransitionRowDTO transition_row;
  ProjectionTransitionRowEvidenceState state;
  std::optional<ProjectionRevisionDTO> acknowledged_staged_candidate_revision;
  std::optional<ProjectionRevisionDTO> observed_local_active;
  std::optional<ProjectionHealthDTO> resulting_health;
  std::optional<ErrorDTO> error;

  [[nodiscard]] bool valid() const noexcept {
    switch (state) {
      case ProjectionTransitionRowEvidenceState::kPending:
      case ProjectionTransitionRowEvidenceState::kStagedNoActiveEffect:
      case ProjectionTransitionRowEvidenceState::kCandidateActive:
      case ProjectionTransitionRowEvidenceState::kRemovedRetired:
      case ProjectionTransitionRowEvidenceState::kCandidateStageDiscarded:
      case ProjectionTransitionRowEvidenceState::kPredecessorUnchanged:
      case ProjectionTransitionRowEvidenceState::kUncertain:
        break;
      default:
        return false;
    }
    if (transition_row.managed_withdrawal) {
      return transition_row.valid() && !resulting_health &&
             !acknowledged_staged_candidate_revision && !observed_local_active &&
             (state == ProjectionTransitionRowEvidenceState::kRemovedRetired ||
              (state == ProjectionTransitionRowEvidenceState::kUncertain && error));
    }
    return transition_row.valid() && resulting_health &&
           (!acknowledged_staged_candidate_revision.has_value() ||
            acknowledged_staged_candidate_revision->valid()) &&
           (!observed_local_active.has_value() || observed_local_active->valid()) &&
           resulting_health->valid() &&
           resulting_health->native_runtime_binding == transition_row.native_runtime_binding;
  }
};

enum class ProjectionTransitionEvidenceState {
  kStaged,
  kActivatedAndRetired,
  kAborted,
  kFinalized,
  kUncertain,
};

[[nodiscard]] constexpr std::string_view to_string(
    ProjectionTransitionEvidenceState state) noexcept {
  switch (state) {
    case ProjectionTransitionEvidenceState::kStaged:
      return "staged";
    case ProjectionTransitionEvidenceState::kActivatedAndRetired:
      return "activated_and_retired";
    case ProjectionTransitionEvidenceState::kAborted:
      return "aborted";
    case ProjectionTransitionEvidenceState::kFinalized:
      return "finalized";
    case ProjectionTransitionEvidenceState::kUncertain:
      return "uncertain";
  }
  return {};
}

struct ProjectionTransitionEvidenceDTO final {
  ApplyId apply_id;
  ApplyGeneration apply_generation;
  ProjectionStageGeneration projection_stage_generation;
  ProjectionRevisionDTO predecessor_projection_revision;
  ProjectionRevisionDTO candidate_projection_revision;
  ProjectionTransitionEvidenceState state;
  std::vector<ProjectionTransitionRowEvidenceDTO> transition_rows;
  std::optional<ErrorDTO> error;

  /// Structural validation used internally before phase semantics.
  [[nodiscard]] bool structurally_valid() const noexcept {
    if (apply_id.empty() || !apply_generation.nonzero() || !projection_stage_generation.nonzero() ||
        !predecessor_projection_revision.valid() || !candidate_projection_revision.valid()) {
      return false;
    }
    switch (state) {
      case ProjectionTransitionEvidenceState::kStaged:
      case ProjectionTransitionEvidenceState::kActivatedAndRetired:
      case ProjectionTransitionEvidenceState::kAborted:
      case ProjectionTransitionEvidenceState::kFinalized:
      case ProjectionTransitionEvidenceState::kUncertain:
        break;
      default:
        return false;
    }
    bool has_uncertainty_error = error.has_value();
    for (std::size_t index = 0U; index < transition_rows.size(); ++index) {
      const auto& row = transition_rows[index];
      if (!row.valid() ||
          !row.transition_row.matches_predecessor(predecessor_projection_revision)) {
        return false;
      }
      if (index != 0U && !CanonicalProjectionNativeBindingLess(
                             transition_rows[index - 1U].transition_row.native_runtime_binding,
                             row.transition_row.native_runtime_binding)) {
        return false;
      }
      has_uncertainty_error = has_uncertainty_error || row.error.has_value() ||
                              (row.resulting_health && row.resulting_health->error.has_value());
    }
    return state != ProjectionTransitionEvidenceState::kUncertain || has_uncertainty_error;
  }

  [[nodiscard]] bool valid() const noexcept { return phase_consistent(state); }

  [[nodiscard]] bool matches_apply(const ApplyId& expected_apply_id,
                                   ApplyGeneration expected_apply_generation,
                                   ProjectionStageGeneration expected_stage_generation,
                                   const ProjectionRevisionDTO& expected_predecessor,
                                   const ProjectionRevisionDTO& expected_candidate) const noexcept {
    return valid() && apply_id == expected_apply_id &&
           apply_generation == expected_apply_generation &&
           projection_stage_generation == expected_stage_generation &&
           predecessor_projection_revision == expected_predecessor &&
           candidate_projection_revision == expected_candidate;
  }

  /// Checks the locally enforceable row shape for one closed durable phase.
  ///
  /// This deliberately does not prove that the row array is the contextual
  /// predecessor-union-candidate set.
  [[nodiscard]] bool phase_consistent(
      ProjectionTransitionEvidenceState expected_state) const noexcept {
    if (!structurally_valid() || state != expected_state) {
      return false;
    }
    switch (expected_state) {
      case ProjectionTransitionEvidenceState::kStaged:
      case ProjectionTransitionEvidenceState::kActivatedAndRetired:
      case ProjectionTransitionEvidenceState::kAborted:
      case ProjectionTransitionEvidenceState::kFinalized:
        if (error.has_value()) {
          return false;
        }
        break;
      case ProjectionTransitionEvidenceState::kUncertain:
        break;
      default:
        return false;
    }

    bool has_uncertainty_error = error.has_value();
    const ProjectionStagedCandidateDTO expected_staged_candidate{
        apply_id,
        apply_generation,
        projection_stage_generation,
        candidate_projection_revision,
    };
    for (const auto& row : transition_rows) {
      if (row.transition_row.managed_withdrawal) {
        const auto& receipt = *row.transition_row.managed_withdrawal;
        if (receipt.apply_id != apply_id || receipt.apply_generation != apply_generation ||
            expected_state == ProjectionTransitionEvidenceState::kAborted ||
            (expected_state != ProjectionTransitionEvidenceState::kUncertain &&
             (row.error || row.state != ProjectionTransitionRowEvidenceState::kRemovedRetired)))
          return false;
        has_uncertainty_error = has_uncertainty_error || row.error.has_value();
        continue;
      }
      if (row.acknowledged_staged_candidate_revision.has_value() &&
          *row.acknowledged_staged_candidate_revision != candidate_projection_revision) {
        return false;
      }
      if (row.resulting_health->active_projection_revision != row.observed_local_active ||
          (row.resulting_health->staged_candidate.has_value() &&
           *row.resulting_health->staged_candidate != expected_staged_candidate)) {
        return false;
      }
      has_uncertainty_error = has_uncertainty_error || row.error.has_value() ||
                              (row.resulting_health && row.resulting_health->error.has_value());
      if (expected_state == ProjectionTransitionEvidenceState::kUncertain) {
        continue;
      }
      if (row.error.has_value() ||
          (row.resulting_health && row.resulting_health->error.has_value())) {
        return false;
      }
      if ((row.observed_local_active.has_value() &&
           row.resulting_health->state != ProjectionHealthState::kHealthy) ||
          (!row.observed_local_active.has_value() &&
           row.resulting_health->state != ProjectionHealthState::kHealthy &&
           row.resulting_health->state != ProjectionHealthState::kUninitialized)) {
        return false;
      }
      const bool candidate_row =
          row.transition_row.disposition != ProjectionTransitionDisposition::kRemoved;
      if (expected_state == ProjectionTransitionEvidenceState::kStaged) {
        if (candidate_row) {
          if (row.state != ProjectionTransitionRowEvidenceState::kStagedNoActiveEffect ||
              !row.acknowledged_staged_candidate_revision.has_value() ||
              row.observed_local_active != row.transition_row.expected_local_active ||
              row.resulting_health->staged_candidate !=
                  std::optional<ProjectionStagedCandidateDTO>{expected_staged_candidate}) {
            return false;
          }
        } else if (row.state != ProjectionTransitionRowEvidenceState::kPredecessorUnchanged ||
                   row.acknowledged_staged_candidate_revision.has_value() ||
                   row.observed_local_active != row.transition_row.expected_local_active ||
                   row.resulting_health->staged_candidate.has_value()) {
          return false;
        }
        continue;
      }
      if (expected_state == ProjectionTransitionEvidenceState::kAborted) {
        if ((candidate_row &&
             row.state != ProjectionTransitionRowEvidenceState::kCandidateStageDiscarded) ||
            (!candidate_row &&
             row.state != ProjectionTransitionRowEvidenceState::kPredecessorUnchanged) ||
            (!candidate_row && row.acknowledged_staged_candidate_revision.has_value()) ||
            row.observed_local_active != row.transition_row.expected_local_active ||
            row.resulting_health->staged_candidate.has_value()) {
          return false;
        }
        continue;
      }
      if (candidate_row) {
        if (row.state != ProjectionTransitionRowEvidenceState::kCandidateActive ||
            !row.acknowledged_staged_candidate_revision.has_value() ||
            row.observed_local_active !=
                std::optional<ProjectionRevisionDTO>{candidate_projection_revision} ||
            row.resulting_health->staged_candidate.has_value() ||
            !row.resulting_health->healthy_for_target(candidate_projection_revision)) {
          return false;
        }
      } else if (row.state != ProjectionTransitionRowEvidenceState::kRemovedRetired ||
                 row.acknowledged_staged_candidate_revision.has_value() ||
                 row.observed_local_active.has_value() ||
                 row.resulting_health->staged_candidate.has_value()) {
        return false;
      }
    }
    return expected_state != ProjectionTransitionEvidenceState::kUncertain || has_uncertainty_error;
  }

  /// Compares the locally supplied retained+added binding projection with a
  /// candidate-only aggregate. Persisted required-set completeness remains a
  /// contextual scene-owner check.
  [[nodiscard]] bool candidate_bindings_match(
      const ProjectionAggregateHealthDTO& aggregate) const noexcept {
    if (!structurally_valid() || !aggregate.valid()) {
      return false;
    }
    std::size_t candidate_count = 0U;
    for (const auto& row : transition_rows) {
      if (row.transition_row.disposition != ProjectionTransitionDisposition::kRemoved) {
        ++candidate_count;
      }
    }
    if (candidate_count != aggregate.native_projection_health.size()) {
      return false;
    }
    std::size_t aggregate_index = 0U;
    for (const auto& row : transition_rows) {
      if (row.transition_row.disposition == ProjectionTransitionDisposition::kRemoved) {
        continue;
      }
      if (row.transition_row.native_runtime_binding !=
          aggregate.native_projection_health[aggregate_index].native_runtime_binding) {
        return false;
      }
      ++aggregate_index;
    }
    return true;
  }

  /// Matches both exact Apply identity and one closed durable phase.
  ///
  /// Exact predecessor-union-candidate row membership remains contextual to
  /// the scene owner; the local phase value itself may not contradict the
  /// phase claimed by a result or status.
  [[nodiscard]] bool matches_apply_phase(
      const ApplyId& expected_apply_id, ApplyGeneration expected_apply_generation,
      ProjectionStageGeneration expected_stage_generation,
      const ProjectionRevisionDTO& expected_predecessor,
      const ProjectionRevisionDTO& expected_candidate,
      ProjectionTransitionEvidenceState expected_state) const noexcept {
    switch (expected_state) {
      case ProjectionTransitionEvidenceState::kStaged:
      case ProjectionTransitionEvidenceState::kActivatedAndRetired:
      case ProjectionTransitionEvidenceState::kAborted:
      case ProjectionTransitionEvidenceState::kFinalized:
      case ProjectionTransitionEvidenceState::kUncertain:
        return phase_consistent(expected_state) &&
               matches_apply(expected_apply_id, expected_apply_generation,
                             expected_stage_generation, expected_predecessor, expected_candidate);
    }
    return false;
  }
};

/// Complete immutable stage input. A raw aggregate is not proof that either
/// retained bundle is verified; the Manager trust boundary must obtain both
/// from the sealed retained-bundle repository before constructing this value.
struct ProjectionApplyStageDTO final {
  ApplyId apply_id;
  ApplyGeneration apply_generation;
  ProjectionRevisionDTO expected_active_projection_revision;
  RetainedBundleDTO predecessor_retained_bundle;
  RetainedBundleDTO candidate_retained_bundle;
  std::vector<ProjectionTransitionRowDTO> native_transition_rows;

  [[nodiscard]] bool valid() const noexcept {
    if (apply_id.empty() || !apply_generation.nonzero() ||
        !expected_active_projection_revision.valid() ||
        predecessor_retained_bundle.retained_bundle_id !=
            expected_active_projection_revision.retained_bundle_id ||
        predecessor_retained_bundle.cell_graph.graph_revision !=
            expected_active_projection_revision.graph_revision ||
        candidate_retained_bundle.retained_bundle_id.empty() ||
        candidate_retained_bundle.cell_graph.graph_revision.empty() ||
        predecessor_retained_bundle.cell_graph.cell_id.empty() ||
        predecessor_retained_bundle.cell_graph.cell_id !=
            candidate_retained_bundle.cell_graph.cell_id) {
      return false;
    }
    if (!CanonicalProjectionTransitionRows(native_transition_rows)) {
      return false;
    }
    for (const auto& row : native_transition_rows) {
      if (!row.matches_predecessor(expected_active_projection_revision) ||
          (row.managed_withdrawal &&
           (row.managed_withdrawal->apply_id != apply_id ||
            row.managed_withdrawal->apply_generation != apply_generation))) {
        return false;
      }
    }
    return true;
  }
};

enum class ProjectionApplyStageResultState {
  kStaged,
  kRejected,
  kFailedProjectionUncertain,
};

struct ProjectionApplyStageResultDTO final {
  ApplyId apply_id;
  ApplyGeneration apply_generation;
  ProjectionRevisionDTO expected_active_projection_revision;
  std::optional<ProjectionRevisionDTO> candidate_projection_revision;
  std::optional<ProjectionStageGeneration> projection_stage_generation;
  std::optional<ProjectionTransitionEvidenceDTO> transition_evidence;
  ProjectionApplyStageResultState state;
  std::optional<ErrorDTO> error;

  /// Checks only the closed phase-dependent presence contract. Semantic
  /// health/row validation remains scene-owner policy.
  [[nodiscard]] bool presence_valid() const noexcept {
    if (apply_id.empty() || !apply_generation.nonzero() ||
        !expected_active_projection_revision.valid()) {
      return false;
    }
    const bool has_stage = projection_stage_generation.has_value();
    const bool has_candidate = candidate_projection_revision.has_value();
    const bool has_evidence = transition_evidence.has_value();
    if (has_candidate != has_evidence) {
      return false;
    }
    if (has_candidate && !has_stage) {
      return false;
    }
    if (has_stage && !projection_stage_generation->nonzero()) {
      return false;
    }
    if (has_candidate && !candidate_projection_revision->valid()) {
      return false;
    }
    if (has_evidence && !transition_evidence->matches_apply(
                            apply_id, apply_generation, *projection_stage_generation,
                            expected_active_projection_revision, *candidate_projection_revision)) {
      return false;
    }
    switch (state) {
      case ProjectionApplyStageResultState::kStaged:
        return has_stage && has_candidate &&
               transition_evidence->phase_consistent(ProjectionTransitionEvidenceState::kStaged) &&
               !error.has_value();
      case ProjectionApplyStageResultState::kRejected:
        return error.has_value() &&
               (!has_evidence ||
                transition_evidence->phase_consistent(ProjectionTransitionEvidenceState::kAborted));
      case ProjectionApplyStageResultState::kFailedProjectionUncertain:
        return error.has_value() &&
               (!has_evidence || transition_evidence->phase_consistent(
                                     ProjectionTransitionEvidenceState::kUncertain));
    }
    return false;
  }
};

struct ProjectionApplyActivationDTO final {
  ApplyId apply_id;
  ApplyGeneration apply_generation;
  ProjectionRevisionDTO expected_active_projection_revision;
  ProjectionStageGeneration projection_stage_generation;
  ProjectionRevisionDTO candidate_projection_revision;

  [[nodiscard]] bool valid() const noexcept {
    return !apply_id.empty() && apply_generation.nonzero() &&
           expected_active_projection_revision.valid() && projection_stage_generation.nonzero() &&
           candidate_projection_revision.valid();
  }
};

enum class ProjectionApplyActivationResultState {
  kActivated,
  kStale,
  kFailedProjectionUncertain,
};

struct ProjectionApplyActivationResultDTO final {
  ApplyId apply_id;
  ApplyGeneration apply_generation;
  ProjectionRevisionDTO expected_active_projection_revision;
  ProjectionStageGeneration projection_stage_generation;
  ProjectionRevisionDTO candidate_projection_revision;
  ProjectionApplyActivationResultState state;
  std::optional<ProjectionRevisionDTO> acknowledged_candidate_projection_revision;
  ProjectionTransitionEvidenceDTO transition_evidence;
  std::optional<ErrorDTO> error;

  [[nodiscard]] bool presence_valid() const noexcept {
    if (apply_id.empty() || !apply_generation.nonzero() ||
        !expected_active_projection_revision.valid() || !projection_stage_generation.nonzero() ||
        !candidate_projection_revision.valid()) {
      return false;
    }
    switch (state) {
      case ProjectionApplyActivationResultState::kActivated:
        return transition_evidence.matches_apply_phase(
                   apply_id, apply_generation, projection_stage_generation,
                   expected_active_projection_revision, candidate_projection_revision,
                   ProjectionTransitionEvidenceState::kActivatedAndRetired) &&
               acknowledged_candidate_projection_revision.has_value() &&
               *acknowledged_candidate_projection_revision == candidate_projection_revision &&
               !error.has_value();
      case ProjectionApplyActivationResultState::kStale:
        return transition_evidence.matches_apply_phase(
                   apply_id, apply_generation, projection_stage_generation,
                   expected_active_projection_revision, candidate_projection_revision,
                   ProjectionTransitionEvidenceState::kAborted) &&
               !acknowledged_candidate_projection_revision.has_value() && error.has_value();
      case ProjectionApplyActivationResultState::kFailedProjectionUncertain:
        return transition_evidence.matches_apply_phase(
                   apply_id, apply_generation, projection_stage_generation,
                   expected_active_projection_revision, candidate_projection_revision,
                   ProjectionTransitionEvidenceState::kUncertain) &&
               !acknowledged_candidate_projection_revision.has_value() && error.has_value();
    }
    return false;
  }
};

struct ProjectionApplyDiscardDTO final {
  ApplyId apply_id;
  ApplyGeneration apply_generation;
  ProjectionStageGeneration projection_stage_generation;
  ProjectionRevisionDTO candidate_projection_revision;
  ProjectionRevisionDTO expected_predecessor_projection_revision;

  [[nodiscard]] bool valid() const noexcept {
    return !apply_id.empty() && apply_generation.nonzero() &&
           projection_stage_generation.nonzero() && candidate_projection_revision.valid() &&
           expected_predecessor_projection_revision.valid();
  }
};

enum class ProjectionApplyDiscardResultState {
  kDiscarded,
  kAlreadyAbsent,
  kFailedProjectionUncertain,
};

struct ProjectionApplyDiscardResultDTO final {
  ApplyId apply_id;
  ApplyGeneration apply_generation;
  ProjectionStageGeneration projection_stage_generation;
  ProjectionRevisionDTO candidate_projection_revision;
  ProjectionRevisionDTO expected_predecessor_projection_revision;
  ProjectionApplyDiscardResultState state;
  ProjectionTransitionEvidenceDTO transition_evidence;
  std::optional<ErrorDTO> error;

  [[nodiscard]] bool presence_valid() const noexcept {
    if (apply_id.empty() || !apply_generation.nonzero() || !projection_stage_generation.nonzero() ||
        !candidate_projection_revision.valid() ||
        !expected_predecessor_projection_revision.valid()) {
      return false;
    }
    switch (state) {
      case ProjectionApplyDiscardResultState::kDiscarded:
      case ProjectionApplyDiscardResultState::kAlreadyAbsent:
        return transition_evidence.matches_apply_phase(
                   apply_id, apply_generation, projection_stage_generation,
                   expected_predecessor_projection_revision, candidate_projection_revision,
                   ProjectionTransitionEvidenceState::kAborted) &&
               !error.has_value();
      case ProjectionApplyDiscardResultState::kFailedProjectionUncertain:
        return transition_evidence.matches_apply_phase(
                   apply_id, apply_generation, projection_stage_generation,
                   expected_predecessor_projection_revision, candidate_projection_revision,
                   ProjectionTransitionEvidenceState::kUncertain) &&
               error.has_value();
    }
    return false;
  }
};

struct ProjectionApplyFinalizeDTO final {
  ApplyId apply_id;
  ApplyGeneration apply_generation;
  ProjectionStageGeneration projection_stage_generation;
  ProjectionRevisionDTO candidate_projection_revision;
  ActiveBundlePublicationReceiptDTO publication_receipt;

  [[nodiscard]] bool valid() const noexcept {
    return !apply_id.empty() && apply_generation.nonzero() &&
           projection_stage_generation.nonzero() && candidate_projection_revision.valid() &&
           publication_receipt.matches_named_apply(apply_id, apply_generation,
                                                   projection_stage_generation,
                                                   candidate_projection_revision);
  }
};

enum class ProjectionApplyFinalizeResultState {
  kFinalized,
  kStale,
  kFailedProjectionUncertain,
};

struct ProjectionApplyFinalizeResultDTO final {
  ApplyId apply_id;
  ApplyGeneration apply_generation;
  ProjectionStageGeneration projection_stage_generation;
  ProjectionRevisionDTO candidate_projection_revision;
  ActiveBundlePublicationReceiptDTO publication_receipt;
  ProjectionApplyFinalizeResultState state;
  std::optional<ProjectionRevisionDTO> acknowledged_candidate_projection_revision;
  ProjectionTransitionEvidenceDTO transition_evidence;
  ProjectionAggregateHealthDTO candidate_aggregate_health;
  std::optional<ErrorDTO> error;

  [[nodiscard]] bool presence_valid() const noexcept {
    if (apply_id.empty() || !apply_generation.nonzero() || !projection_stage_generation.nonzero() ||
        !candidate_projection_revision.valid() ||
        !publication_receipt.matches_named_apply(apply_id, apply_generation,
                                                 projection_stage_generation,
                                                 candidate_projection_revision) ||
        !transition_evidence.matches_apply(apply_id, apply_generation, projection_stage_generation,
                                           transition_evidence.predecessor_projection_revision,
                                           candidate_projection_revision) ||
        !candidate_aggregate_health.matches_named_apply(apply_id, apply_generation,
                                                        projection_stage_generation,
                                                        candidate_projection_revision)) {
      return false;
    }
    switch (state) {
      case ProjectionApplyFinalizeResultState::kFinalized:
        return transition_evidence.phase_consistent(
                   ProjectionTransitionEvidenceState::kFinalized) &&
               transition_evidence.candidate_bindings_match(candidate_aggregate_health) &&
               candidate_aggregate_health.healthy_for_target(candidate_projection_revision) &&
               acknowledged_candidate_projection_revision.has_value() &&
               *acknowledged_candidate_projection_revision == candidate_projection_revision &&
               !error.has_value();
      case ProjectionApplyFinalizeResultState::kStale:
        return transition_evidence.phase_consistent(
                   ProjectionTransitionEvidenceState::kActivatedAndRetired) &&
               !acknowledged_candidate_projection_revision.has_value() && error.has_value();
      case ProjectionApplyFinalizeResultState::kFailedProjectionUncertain:
        return transition_evidence.phase_consistent(
                   ProjectionTransitionEvidenceState::kUncertain) &&
               !acknowledged_candidate_projection_revision.has_value() && error.has_value();
    }
    return false;
  }
};

struct ProjectionApplyStatusQueryDTO final {
  ApplyId apply_id;
  ApplyGeneration apply_generation;

  [[nodiscard]] bool valid() const noexcept {
    return !apply_id.empty() && apply_generation.nonzero();
  }
};

enum class ProjectionApplyStatusState {
  kAbsent,
  kQuiescing,
  kStaged,
  kActivated,
  kPublished,
  kFinalized,
  kDiscarded,
  kFailedProjectionUncertain,
};

struct ProjectionApplyStatusDTO final {
  ApplyId apply_id;
  ApplyGeneration apply_generation;
  std::optional<ProjectionRevisionDTO> predecessor_projection_revision;
  std::optional<ProjectionStageGeneration> projection_stage_generation;
  std::optional<ProjectionRevisionDTO> candidate_projection_revision;
  ProjectionApplyStatusState state;
  std::optional<ProjectionTransitionEvidenceDTO> transition_evidence;
  std::optional<ActiveBundlePublicationReceiptDTO> publication_receipt;
  std::optional<ProjectionAggregateHealthDTO> candidate_aggregate_health;
  std::optional<ErrorDTO> error;

  [[nodiscard]] bool presence_valid() const noexcept {
    if (apply_id.empty() || !apply_generation.nonzero()) {
      return false;
    }
    const bool has_stage = projection_stage_generation.has_value();
    const bool has_candidate = candidate_projection_revision.has_value();
    const bool has_evidence = transition_evidence.has_value();
    if (has_candidate != has_evidence || (has_candidate && !has_stage)) {
      return false;
    }
    if (has_stage && !projection_stage_generation->nonzero()) {
      return false;
    }
    if (has_candidate && !candidate_projection_revision->valid()) {
      return false;
    }
    if (publication_receipt.has_value() && !publication_receipt->valid()) {
      return false;
    }
    if ((state != ProjectionApplyStatusState::kAbsent || has_evidence) &&
        (!predecessor_projection_revision.has_value() ||
         !predecessor_projection_revision->valid())) {
      return false;
    }
    if (has_evidence && !transition_evidence->matches_apply(
                            apply_id, apply_generation, *projection_stage_generation,
                            *predecessor_projection_revision, *candidate_projection_revision)) {
      return false;
    }
    if (publication_receipt.has_value() &&
        (!has_candidate || !publication_receipt->matches_named_apply(
                               apply_id, apply_generation, *projection_stage_generation,
                               *candidate_projection_revision))) {
      return false;
    }
    if (candidate_aggregate_health.has_value() &&
        (!has_candidate || !candidate_aggregate_health->matches_named_apply(
                               apply_id, apply_generation, *projection_stage_generation,
                               *candidate_projection_revision))) {
      return false;
    }
    switch (state) {
      case ProjectionApplyStatusState::kAbsent:
        return !predecessor_projection_revision.has_value() && !has_stage && !has_candidate &&
               !publication_receipt.has_value() && !candidate_aggregate_health.has_value() &&
               !error.has_value();
      case ProjectionApplyStatusState::kQuiescing:
        return has_stage && !has_candidate && !publication_receipt.has_value() &&
               !candidate_aggregate_health.has_value() && !error.has_value();
      case ProjectionApplyStatusState::kStaged:
        return has_stage && has_candidate &&
               transition_evidence->phase_consistent(ProjectionTransitionEvidenceState::kStaged) &&
               !publication_receipt.has_value() && !candidate_aggregate_health.has_value() &&
               !error.has_value();
      case ProjectionApplyStatusState::kActivated:
        return has_stage && has_candidate &&
               transition_evidence->phase_consistent(
                   ProjectionTransitionEvidenceState::kActivatedAndRetired) &&
               !publication_receipt.has_value() && !candidate_aggregate_health.has_value() &&
               !error.has_value();
      case ProjectionApplyStatusState::kPublished:
        return has_stage && has_candidate &&
               transition_evidence->phase_consistent(
                   ProjectionTransitionEvidenceState::kActivatedAndRetired) &&
               publication_receipt.has_value() && !candidate_aggregate_health.has_value() &&
               !error.has_value();
      case ProjectionApplyStatusState::kFinalized:
        return has_stage && has_candidate &&
               transition_evidence->phase_consistent(
                   ProjectionTransitionEvidenceState::kFinalized) &&
               publication_receipt.has_value() && candidate_aggregate_health.has_value() &&
               transition_evidence->candidate_bindings_match(*candidate_aggregate_health) &&
               candidate_aggregate_health->healthy_for_target(*candidate_projection_revision) &&
               !error.has_value();
      case ProjectionApplyStatusState::kDiscarded:
        return has_stage && has_candidate &&
               transition_evidence->phase_consistent(ProjectionTransitionEvidenceState::kAborted) &&
               !publication_receipt.has_value() && !candidate_aggregate_health.has_value() &&
               !error.has_value();
      case ProjectionApplyStatusState::kFailedProjectionUncertain:
        return error.has_value() && !candidate_aggregate_health.has_value() &&
               (!publication_receipt.has_value() || has_candidate) &&
               (!has_evidence || transition_evidence->phase_consistent(
                                     ProjectionTransitionEvidenceState::kUncertain));
    }
    return false;
  }
};

}  // namespace manyforge::core
