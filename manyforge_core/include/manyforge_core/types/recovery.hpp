#pragma once

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/execution_status.hpp>
#include <manyforge_core/types/interrupt.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace manyforge::core {

/// Backend-neutral, ROS-free recovery authority records (spec/210 §3.12,
/// spec/230 §3.7.1). All types here are immutable value objects: once
/// published they never change; a fact change publishes a new revision.
/// The kernel owns every field; no other layer may synthesize these.

/// Which stop authority produced a certificate.
enum class StopOwnerKind {
  kFaultPreemptWorker,
  kOperatorStopWorker,
  kMotionTripStop,
};

/// Definitive outcome a stop certificate attests.
enum class StopCertificateOutcome {
  kSucceeded,
  kPartial,
  kBackendPreemptFailed,
};

/// Evidence class behind `observed_state_at_rest` on a certificate.
/// `kMeasuredVelocity` means the stop owner's settle wait measured joint
/// velocities below the qualified threshold. `kQualifiedCommandedState` is
/// reserved for explicitly qualified preview-lane authority. Backend stop
/// acknowledgement alone is not at-rest evidence; such certificates use
/// `kUnavailable`.
enum class AtRestEvidence {
  kUnavailable,
  kQualifiedCommandedState,
  kMeasuredVelocity,
};

/// Recovery classification of the current kernel situation.
enum class RecoveryClass {
  kNone,
  kRuntimeCollision,
  kStopFaultIntervention,
  kFailedValidation,
  kOrdinaryOperatorStop,
  kUnknownFault,
};

/// Shared recovery action identifiers. The first four are valid non-resetting
/// kernel commands. `reset_cycle` is offer-only: assessments may advertise it
/// and untrusted wire input may contain it, but ApplyRecoveryAction always
/// rejects it because the existing cycle Reset route owns execution
/// (spec/230 §3.7.1).
enum class RecoveryAction {
  kRetryMotionFromCurrentState,
  kAcknowledgeAndContinue,
  kRetryAfterSceneRepair,
  kReplanFromStoppedState,
  /// Offer-only action. ApplyRecoveryAction always rejects this value because
  /// the existing cycle Reset command is its sole execution path.
  kResetCycle,
};

/// Assessment status published to consumers.
enum class RecoveryAssessmentStatus {
  kUnavailable,     ///< No recovery situation or evidence is available.
  kWaiting,         ///< Recovery exists but is not yet admissible.
  kActionRequired,  ///< Explicit operator choice is required.
  kRetryReady,      ///< A non-resetting action is currently admissible.
  kResetRequired,   ///< Only the cycle Reset route can clear this state.
};

/// Immutable record that a stop authority reached a definitive result.
/// Keyed by (execution_id, latch_id, stop_owner kind+generation); a late
/// completion from an older owner generation can never overwrite or become
/// authoritative for a newer generation (spec/230 §3.7.1).
struct KernelStopCertificateDTO {
  std::string schema_version = "manyforge.kernel_stop_certificate.v1";
  std::string certificate_id;
  /// Strictly increasing publication counter across all certificates.
  std::uint64_t certificate_revision = 0;
  StopCertificateOutcome completion_result =
      StopCertificateOutcome::kBackendPreemptFailed;
  StopOwnerKind stop_owner = StopOwnerKind::kOperatorStopWorker;
  std::uint64_t stop_owner_generation = 0;
  std::uint64_t execution_id = 0;
  /// Interrupt latch this stop belongs to; empty for ordinary operator /
  /// motion-trip stops that carry no latch.
  std::string latch_id;
  std::uint64_t control_command_epoch = 0;
  std::string requested_stop_mode;
  std::string executed_stop_mode;
  std::string command_source;
  /// Meaningful for fault-preempt certificates; `kNoneRequired` otherwise.
  InterruptPreemptionResult preemption_result =
      InterruptPreemptionResult::kNoneRequired;
  bool backend_stop_confirmed = false;
  std::uint64_t observed_state_sequence = 0;
  std::string observed_state_source_id;
  std::int64_t observed_state_source_timestamp_ns = 0;
  std::int64_t observed_state_received_timestamp_ns = 0;
  bool observed_state_fresh = false;
  bool observed_state_at_rest = false;
  AtRestEvidence at_rest_evidence = AtRestEvidence::kUnavailable;
  /// Kernel clock time the definitive result was reached.
  std::int64_t completed_at_ns = 0;
  std::optional<ErrorDTO> terminal_error;
};

/// Minimal observed-state evidence consumed by recovery assessment. RT-8C's
/// dedicated `ObservedRobotStateSnapshotDTO` publication is the production
/// source; until it exists no provider is wired and every observation-
/// dependent assessment publishes unavailable and rejects fail-closed.
/// Lifecycle `ExecutionSnapshotDTO` freshness MUST NOT be used here
/// (spec/230 §3.7.1).
struct ObservedStateEvidenceDTO {
  std::uint64_t sequence = 0;
  std::int64_t sampled_at_ns = 0;
  std::int64_t freshness_budget_ns = 0;
  bool complete = false;
  bool at_rest = false;
  /// Stable token identifying joint ordering/identity of the source; a
  /// mismatch against the deployment token invalidates the evidence.
  std::string joint_order_token;
  std::string source_identity;
  /// Stable deployment profile carried by the selected publication lane.
  /// Recovery policy evaluates this fact explicitly; source names are never
  /// interpreted as authority.
  std::string authority_profile;
  bool measured_authority = false;
};

/// The only kernel statement of whether recovery is currently admissible
/// (spec/230 §3.7.1). Built from one coherent authority capture under
/// lifecycle ordering; published as an immutable copy under a leaf lock.
struct KernelRecoveryAssessmentDTO {
  std::string schema_version = "manyforge.kernel_recovery_assessment.v1";
  /// Strictly increasing whenever identity, authority, actionability,
  /// status, allowed actions, or reason changes. Equally admissible
  /// observation samples do not churn this revision.
  std::uint64_t assessment_revision = 0;
  std::int64_t assessed_at_ns = 0;
  std::uint64_t control_command_epoch = 0;
  std::uint64_t execution_id = 0;
  ExecutionState execution_state = ExecutionState::kIdle;
  RecoveryClass recovery_class = RecoveryClass::kNone;
  RecoveryAssessmentStatus status = RecoveryAssessmentStatus::kUnavailable;
  std::string latch_id;
  std::string source_id;
  std::string policy_id;
  InterruptPreemptionResult preemption_result =
      InterruptPreemptionResult::kNoneRequired;
  bool stop_owner_active = false;
  std::uint64_t stop_owner_generation = 0;
  bool backend_stop_confirmed = false;
  std::string stop_certificate_id;
  /// Observation facts. `observed_state_available=false` until RT-8C wires
  /// the dedicated publication; freshness/at-rest are then fail-closed.
  bool observed_state_available = false;
  bool observed_state_fresh = false;
  bool observed_state_at_rest = false;
  std::uint64_t observed_state_sequence = 0;
  std::string observed_state_source_id;
  std::string observed_state_authority_profile;
  bool observed_state_measured = false;
  std::int64_t observed_state_valid_until_ns = 0;
  bool stronger_authority_active = false;
  bool queued_latch_present = false;
  bool retry_admissible = false;
  std::vector<RecoveryAction> allowed_actions;
  /// Stable machine-readable reason for the current status.
  std::string reason_code;
  /// Bounded references to native events / certificate ids backing this
  /// assessment (spec/500 correlation spine).
  std::vector<std::string> evidence_refs;
};

/// Request for the single generation-bound kernel recovery command.
struct KernelRecoveryActionRequestDTO {
  std::string request_id;
  RecoveryAction action = RecoveryAction::kRetryMotionFromCurrentState;
  std::string principal_id;
  std::uint64_t expected_assessment_revision = 0;
  std::uint64_t expected_control_command_epoch = 0;
  std::uint64_t expected_execution_id = 0;
  std::string expected_latch_id;
  std::string expected_stop_certificate_id;
  std::uint64_t expected_stop_owner_generation = 0;
  std::string reason;
};

/// Result of `ApplyRecoveryAction`. On rejection the kernel state, latch,
/// certificate, stop owner, backend command, and control epoch are unchanged
/// and `resulting_assessment` carries the newest assessment.
struct KernelRecoveryActionResultDTO {
  std::string request_id;
  RecoveryAction action = RecoveryAction::kRetryMotionFromCurrentState;
  bool accepted = false;
  KernelRecoveryAssessmentDTO resulting_assessment;
  std::optional<ErrorDTO> error;
};

const char* stop_owner_kind_name(StopOwnerKind kind);
const char* stop_certificate_outcome_name(StopCertificateOutcome outcome);
const char* at_rest_evidence_name(AtRestEvidence evidence);
const char* recovery_class_name(RecoveryClass recovery_class);
const char* recovery_action_name(RecoveryAction action);
const char* recovery_assessment_status_name(RecoveryAssessmentStatus status);

}  // namespace manyforge::core
