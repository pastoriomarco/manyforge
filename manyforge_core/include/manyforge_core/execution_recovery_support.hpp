#pragma once

#include <cstdint>
#include <manyforge_core/observed_robot_state_publication.hpp>
#include <manyforge_core/types/recovery.hpp>
#include <manyforge_core/types/stop_certificate.hpp>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace manyforge::core {

std::optional<ObservedStateEvidenceDTO> capture_observed_state_evidence(
    const ObservedRobotStatePublication* publication, std::int64_t now_ns,
    double at_rest_velocity_threshold_rad_s);

void apply_observed_state_to_stop_certificate(const ObservedRobotStatePublication* publication,
                                              std::int64_t observed_at_ns,
                                              double at_rest_velocity_threshold_rad_s,
                                              KernelStopCertificateDTO& certificate);

/// Deterministic store of definitive stop certificates
/// (spec/230 §3.7.1). NOT thread-safe: the owner guards it with a leaf
/// mutex. Semantics:
///  - per stop-owner kind, the AUTHORITATIVE certificate is selected by
///    strictly monotonic (stop_owner_generation, certificate_revision):
///    a late completion from an older generation can never displace or
///    become authoritative for a newer one;
///  - admission lookups are by exact (owner kind, execution, latch,
///    generation) match against the authoritative selection only.
class RecoveryCertificateStore {
 public:
  /// Returns the assigned strictly increasing certificate revision.
  std::uint64_t publish(KernelStopCertificateDTO certificate);

  /// Publish monotonic progress for one exact owner session. A partial/failed
  /// record may be upgraded to succeeded; equivalent duplicates and any
  /// downgrade after success are ignored. Returns the assigned revision when
  /// published, or nullopt when coalesced.
  std::optional<std::uint64_t> publish_owner_session_progress(KernelStopCertificateDTO certificate);

  /// Authoritative certificate for an owner kind, if any.
  [[nodiscard]] std::optional<KernelStopCertificateDTO> authoritative(StopOwnerKind kind) const;

  /// Exact-match authoritative lookup used by admission. Returns nullopt
  /// unless the authoritative certificate for `kind` matches every key.
  [[nodiscard]] std::optional<KernelStopCertificateDTO> find_authoritative(
      StopOwnerKind kind, std::uint64_t execution_id, const std::string& latch_id,
      std::uint64_t stop_owner_generation) const;

  [[nodiscard]] std::uint64_t last_revision() const { return next_revision_; }

 private:
  static constexpr std::size_t kOwnerKindCount = 3;
  static std::size_t owner_index(StopOwnerKind kind);

  std::uint64_t next_revision_ = 0;
  std::optional<KernelStopCertificateDTO> authoritative_[kOwnerKindCount];
};

/// Coherent execution-to-generation binding for one stop-owner kind. NOT
/// thread-safe: the kernel guards it with its dedicated identity leaf mutex.
/// The global counter never regresses, while `generation_for_execution`
/// prevents a newer execution from accidentally consuming an older stop fact.
class StopOwnerIdentityTracker {
 public:
  std::uint64_t begin(std::uint64_t execution_id);
  std::uint64_t ensure(std::uint64_t execution_id);
  [[nodiscard]] std::uint64_t generation_for_execution(std::uint64_t execution_id) const;

 private:
  std::uint64_t generation_counter_ = 0;
  std::uint64_t selected_generation_ = 0;
  std::uint64_t selected_execution_id_ = 0;
};

/// Participant-local recovery evidence for one aggregate addressed Stop.
/// Unlike KernelStopCertificateDTO this is only a reference to the addressed
/// owner's certificate identity and is never published as another certificate.
struct AddressedMotionStopRecoveryEvidenceDTO final {
  std::uint64_t execution_id{};
  std::uint64_t stop_owner_generation{};
  StopCertificateRefDTO aggregate_stop_certificate;
  bool backend_stop_confirmed{};

  friend bool operator==(const AddressedMotionStopRecoveryEvidenceDTO&,
                         const AddressedMotionStopRecoveryEvidenceDTO&) = default;
};

/// One coherent capture of the authority facts an assessment is built from.
/// Assembled under lifecycle ordering by the kernel; never composed from
/// separately sampled leaf state of different generations.
struct RecoveryAuthorityCapture {
  std::uint64_t control_command_epoch = 0;
  std::uint64_t execution_id = 0;
  ExecutionState execution_state = ExecutionState::kIdle;
  std::optional<InterruptLatch> active_latch;
  bool fault_preempt_stop_active = false;
  bool fault_preempt_worker_active = false;
  bool operator_stop_active = false;
  bool motion_trip_stop_active = false;
  bool stronger_authority_active = false;
  bool queued_latch_present = false;
  std::uint64_t fault_preempt_owner_generation = 0;
  std::uint64_t operator_stop_owner_generation = 0;
  std::optional<KernelStopCertificateDTO> fault_preempt_certificate;
  std::optional<KernelStopCertificateDTO> operator_stop_certificate;
  std::optional<AddressedMotionStopRecoveryEvidenceDTO> addressed_motion_stop_evidence;
  bool stop_fault_intervention_classified = false;
  /// Classified retryable pre-dispatch validation failure, or a Failed
  /// prepared execution proven never dispatched (no physical effect).
  bool failed_validation_retryable = false;
  std::string failed_validation_reason;
  /// Exact immutable failure facts, distinct from broader manual recovery.
  std::string failed_validation_code;
  ParticipantEffectKnowledge failed_execution_effect{ParticipantEffectKnowledge::kEffectUncertain};
  bool failed_preparation_never_dispatched{false};
  /// Set only by the authoritative typed collision decision before it is
  /// converted into an ErrorDTO; validation_collision alone is insufficient.
  bool failed_validation_conclusive_collision{false};
  ParticipantEffectKnowledge terminal_effect{ParticipantEffectKnowledge::kEffectUncertain};
  /// The terminal publisher and all subordinate worker borrows have finished.
  bool terminal_cleanup_complete{false};
  /// Observation evidence; absent until RT-8C wires the dedicated
  /// publication (never lifecycle ExecutionSnapshot).
  std::optional<ObservedStateEvidenceDTO> observed_state;
  bool observed_state_qualified = false;
  std::int64_t captured_at_ns = 0;
};

/// Deployment/observation policy inputs for assessment construction.
struct RecoveryReadinessPolicy {
  /// True when the deployment claims measured stop evidence, making fresh
  /// observed-state/at-rest proof mandatory for retry admission.
  bool require_measured_observation = false;
  /// Asymmetric restoration: unsafe->safe requires this continuous-safe
  /// dwell plus consecutive qualifying samples; safe->unsafe is immediate.
  std::int64_t ready_dwell_ns = 0;
  std::uint32_t ready_min_samples = 1;
};

/// Asymmetric observed-readiness qualifier (spec/230 §3.7.1): any
/// unsafe/stale/incomplete sample revokes qualification immediately;
/// restoration requires a continuous-safe dwell plus consecutive qualifying
/// samples. NOT thread-safe; the owner serializes access.
class ObservedReadinessTracker {
 public:
  /// Feed one sample; returns true when the sample chain is currently
  /// qualified under `policy` evaluated at `now_ns`.
  bool update(const ObservedStateEvidenceDTO& sample, const std::string& expected_joint_order_token,
              const RecoveryReadinessPolicy& policy, std::int64_t now_ns);

  /// Time-only re-evaluation (freshness expiry without new samples).
  bool evaluate(const RecoveryReadinessPolicy& policy, std::int64_t now_ns) const;

  void reset();

  [[nodiscard]] std::optional<ObservedStateEvidenceDTO> last_sample() const { return last_sample_; }

 private:
  [[nodiscard]] bool sample_safe(const ObservedStateEvidenceDTO& sample,
                                 const std::string& expected_token,
                                 const RecoveryReadinessPolicy& policy, std::int64_t now_ns) const;

  std::optional<ObservedStateEvidenceDTO> last_sample_;
  std::uint64_t last_counted_sequence_ = 0;
  std::int64_t continuous_safe_since_ns_ = -1;
  std::uint32_t consecutive_safe_samples_ = 0;
};

/// Pure planning-failure classification shared by original failure evidence
/// and recovery assessment. Callers supply their exact captured facts.
MotionRetryEligibilityDTO planning_retry_eligibility(const RecoveryAuthorityCapture& capture);

/// Pure construction of the assessment from one coherent capture. Performs
/// no locking, no I/O, no backend/planner/engine calls.
KernelRecoveryAssessmentDTO build_recovery_assessment(const RecoveryAuthorityCapture& capture,
                                                      const RecoveryReadinessPolicy& policy,
                                                      std::uint64_t previous_revision);

/// A certified ordinary Stop whose execute worker's terminal cleanup is still
/// pending: settled under its certificate, but neither retry nor close is
/// decided yet (status waiting, `terminal_cleanup_pending`).
[[nodiscard]] bool stopped_awaiting_terminal_cleanup(
    const KernelRecoveryAssessmentDTO& assessment) noexcept;

/// True when two assessments differ in identity, authority, actionability,
/// status, allowed actions, or reason — i.e. when publication must advance
/// the revision. Equal admissibility facts do not churn.
bool recovery_assessment_semantically_differs(const KernelRecoveryAssessmentDTO& previous,
                                              const KernelRecoveryAssessmentDTO& next);

/// Pure assessment + durable-record projection. The kernel remains the owner
/// of coherent capture, publication locks, scheduling, and event emission;
/// this helper owns the duplicated semantic comparison and field mapping.
struct RecoveryAssessmentPublication {
  KernelRecoveryAssessmentDTO candidate;
  bool changed = false;
  std::map<std::string, std::string> event_details;
};

RecoveryAssessmentPublication prepare_recovery_assessment_publication(
    const KernelRecoveryAssessmentDTO& previous, const RecoveryAuthorityCapture& capture,
    const RecoveryReadinessPolicy& policy, std::string_view trigger);

/// Structured admission decision for ApplyRecoveryAction. `admitted=false`
/// carries the stable rejection reason; the caller guarantees no side
/// effects were taken.
struct RecoveryAdmissionDecision {
  bool admitted = false;
  std::string_view rejection_reason;
};

/// Pure admission predicate: request vs the freshly assembled capture and
/// the assessment the caller just rebuilt from it. Compares pre-existing
/// ids/revisions only; allocates nothing on the admit path.
RecoveryAdmissionDecision evaluate_recovery_admission(
    const KernelRecoveryActionRequestDTO& request, const RecoveryAuthorityCapture& capture,
    const KernelRecoveryAssessmentDTO& assessment);

/// Rejection error construction (allocates; call after the authority lock
/// is released or on the already-rejected path).
ErrorDTO recovery_action_rejection_error(const KernelRecoveryActionRequestDTO& request,
                                         const KernelRecoveryAssessmentDTO& assessment,
                                         std::string_view rejection_reason);

}  // namespace manyforge::core
