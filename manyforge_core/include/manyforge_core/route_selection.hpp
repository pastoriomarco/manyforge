#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/active_control_configuration.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/controller_lifecycle.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace manyforge::core {

inline constexpr std::size_t kMaxRouteSelectionComponents = 4096U;
inline constexpr std::size_t kMaxRouteSelectionAffectedBindings = 4096U;
inline constexpr std::size_t kMaxRouteSelectionAffectedControllers = 4096U;
inline constexpr std::size_t kMaxRouteSelectionTransitionResults = 512U;
inline constexpr std::size_t kMaxRouteSelectionIdentityBytes = 256U;
inline constexpr std::size_t kMaxRouteSelectionSourceLabelBytes = 256U;
inline constexpr std::size_t kMaxRouteSelectionCorrelationBytes = 256U;
inline constexpr std::size_t kMaxRouteSelectionReasonBytes = 1024U;
inline constexpr std::int64_t kMaxRouteSelectionTimeoutNs = 3'600'000'000'000LL;

/// Exact current component fact required by one complete-domain route request.
struct RouteSelectionComponentExpectationDTO final {
  ControllableComponentId controllable_component_id;
  ComponentLifecycleGeneration expected_component_lifecycle_generation;
  ComponentLifecycleStateDTO expected_component_lifecycle_state;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const RouteSelectionComponentExpectationDTO&,
                         const RouteSelectionComponentExpectationDTO&) = default;
};

[[nodiscard]] bool CanonicalRouteSelectionComponentExpectationLess(
    const RouteSelectionComponentExpectationDTO& lhs,
    const RouteSelectionComponentExpectationDTO& rhs) noexcept;

/// Public additive request for one complete desired control-domain configuration.
struct RouteSelectionIntentDTO final {
  PublicRequestContextDTO request_context;
  RetainedBundleId expected_retained_bundle_id;
  GraphRevision expected_graph_revision;
  DesiredControlConfigurationDTO desired_configuration;
  ExpectedActiveControlConfigurationDTO expected_active_control_configuration;
  std::vector<RouteSelectionComponentExpectationDTO> component_expectations;
  RequestAttributionDTO attribution{};
  /// Absent only in legacy retained records. Every new admission requires it.
  std::optional<AdmissionGeneration> expected_admission_generation{};
  std::optional<ProgramNodeAdmissionEvidenceDTO> program_node_evidence{};

  [[nodiscard]] bool locally_valid() const noexcept;
  [[nodiscard]] bool replay_equal(const RouteSelectionIntentDTO& other) const noexcept;

  friend bool operator==(const RouteSelectionIntentDTO&, const RouteSelectionIntentDTO&) = default;
};

enum class RouteConfigurationFenceBusyDispositionDTO : std::uint8_t {
  kRejectIfAffectedBusy = 0,
};

enum class RouteConfigurationFenceStateDTO : std::uint8_t {
  kHeld = 0,
  kReleasePending,
  kReleased,
};

enum class RouteConfigurationFenceAcquireAttemptDispositionDTO : std::uint8_t {
  kAcceptedExact = 0,
  kNotSeen,
  kConflicting,
  kUncertain,
};

enum class RouteConfigurationFenceAcquireEffectDispositionDTO : std::uint8_t {
  kAccepted = 0,
  kProvenNotSeen,
};

enum class RouteConfigurationFenceReleaseAttemptDispositionDTO : std::uint8_t {
  kReleasedExact = 0,
  kNotReleasedHeld,
  kConflicting,
  kUncertain,
};

enum class RouteConfigurationFenceReleaseEffectDispositionDTO : std::uint8_t {
  kReleased = 0,
  kProvenNotReleasedHeld,
};

[[nodiscard]] constexpr std::string_view to_string(
    RouteConfigurationFenceBusyDispositionDTO value) noexcept {
  switch (value) {
    case RouteConfigurationFenceBusyDispositionDTO::kRejectIfAffectedBusy:
      return "reject_if_affected_busy";
  }
  return {};
}

[[nodiscard]] constexpr std::string_view to_string(RouteConfigurationFenceStateDTO value) noexcept {
  switch (value) {
    case RouteConfigurationFenceStateDTO::kHeld:
      return "held";
    case RouteConfigurationFenceStateDTO::kReleasePending:
      return "release_pending";
    case RouteConfigurationFenceStateDTO::kReleased:
      return "released";
  }
  return {};
}

/// Side-effect-free Kernel admission sample for one complete route scope.
struct RouteConfigurationFenceAdmissionQueryDTO final {
  RouteSelectionTransactionId route_selection_transaction_id;
  RouteSelectionTransactionGeneration route_selection_transaction_generation;
  ConfigurationFenceScopeDTO scope;
  RetainedBundleId expected_retained_bundle_id;
  GraphRevision expected_graph_revision;
  ExpectedActiveControlConfigurationDTO expected_active_control_configuration;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const RouteConfigurationFenceAdmissionQueryDTO&,
                         const RouteConfigurationFenceAdmissionQueryDTO&) = default;
};

struct RouteConfigurationFenceAdmissionSnapshotDTO final {
  RouteConfigurationFenceAdmissionQueryDTO query;
  ExecutionHostEpochDTO execution_host_epoch;
  ProjectionRevisionDTO active_projection_revision;
  ActiveBundlePublicationGeneration active_publication_generation;
  ContentDigest active_public_pointer_digest;
  std::uint64_t operation_resource_generation{};
  ConfigurationFenceGeneration configuration_fence_generation{0U};
  ExpectedActiveControlConfigurationDTO current_active_control_configuration;
  std::uint64_t affected_queued_operation_count{};
  std::uint64_t affected_active_operation_count{};
  ContentDigest snapshot_digest;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const RouteConfigurationFenceAdmissionSnapshotDTO&,
                         const RouteConfigurationFenceAdmissionSnapshotDTO&) = default;
};

/// Exact prepared CCLC-to-Kernel route-fence Acquire request.
struct RouteConfigurationFenceIntentDTO final {
  PublicRequestContextDTO owner_request_context;
  RequestId initiating_public_request_id;
  RouteSelectionTransactionId route_selection_transaction_id;
  RouteSelectionTransactionGeneration route_selection_transaction_generation;
  ConfigurationFenceScopeDTO scope;
  ContentDigest admission_snapshot_digest;
  RouteConfigurationFenceBusyDispositionDTO busy_disposition;
  RetainedBundleId expected_retained_bundle_id;
  GraphRevision expected_graph_revision;
  std::uint64_t expected_operation_resource_generation{};
  ConfigurationFenceGeneration expected_fence_generation{0U};
  ExpectedActiveControlConfigurationDTO expected_active_control_configuration;
  ActiveControlConfigurationId candidate_active_control_configuration_id;
  ActiveControlConfigurationGeneration candidate_active_control_configuration_generation{0U};
  std::string reason;

  [[nodiscard]] bool locally_valid() const noexcept;
  [[nodiscard]] bool matches(
      const RouteConfigurationFenceAdmissionSnapshotDTO& snapshot) const noexcept;

  friend bool operator==(const RouteConfigurationFenceIntentDTO&,
                         const RouteConfigurationFenceIntentDTO&) = default;
};

struct RouteConfigurationFenceAcceptedDTO final {
  RequestId request_id;
  RequestReplayStatusDTO replay_status;
  RouteSelectionTransactionId route_selection_transaction_id;
  RouteSelectionTransactionGeneration route_selection_transaction_generation;
  ConfigurationFenceId configuration_fence_id;
  ConfigurationFenceGeneration configuration_fence_generation{0U};
  ConfigurationFenceScopeDTO captured_scope;
  ActiveControlConfigurationId candidate_active_control_configuration_id;
  ActiveControlConfigurationGeneration candidate_active_control_configuration_generation{0U};
  std::int64_t accepted_at_ns{};
  ContentDigest acceptance_digest;

  [[nodiscard]] bool locally_valid() const noexcept;
  [[nodiscard]] bool matches(const RouteConfigurationFenceIntentDTO& intent) const noexcept;

  friend bool operator==(const RouteConfigurationFenceAcceptedDTO&,
                         const RouteConfigurationFenceAcceptedDTO&) = default;
};

struct RouteConfigurationFenceStatusQueryDTO final {
  RouteSelectionTransactionId route_selection_transaction_id;
  RouteSelectionTransactionGeneration route_selection_transaction_generation;
  ConfigurationFenceId configuration_fence_id;
  ConfigurationFenceGeneration configuration_fence_generation{0U};
  ConfigurationFenceScopeDTO expected_scope;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const RouteConfigurationFenceStatusQueryDTO&,
                         const RouteConfigurationFenceStatusQueryDTO&) = default;
};

/// Kernel-owned current evidence used by status and release compare-and-set.
struct RouteConfigurationFenceReconciliationEvidenceDTO final {
  ExecutionHostEpochDTO execution_host_epoch;
  RouteSelectionTransactionId route_selection_transaction_id;
  RouteSelectionTransactionGeneration route_selection_transaction_generation;
  ConfigurationFenceId configuration_fence_id;
  ConfigurationFenceGeneration configuration_fence_generation{0U};
  ConfigurationFenceScopeDTO captured_scope;
  std::uint64_t operation_resource_generation{};
  RetainedBundleId active_retained_bundle_id;
  GraphRevision active_graph_revision;
  ProjectionRevisionDTO active_projection_revision;
  ActiveBundlePublicationGeneration active_publication_generation;
  ContentDigest active_public_pointer_digest;
  ExpectedActiveControlConfigurationDTO current_active_control_configuration;
  std::uint64_t affected_queued_operation_count{};
  std::uint64_t affected_active_operation_count{};
  ContentDigest evidence_digest;

  [[nodiscard]] bool locally_valid() const noexcept;
  [[nodiscard]] bool matches(const RouteConfigurationFenceStatusQueryDTO& query) const noexcept;

  friend bool operator==(const RouteConfigurationFenceReconciliationEvidenceDTO&,
                         const RouteConfigurationFenceReconciliationEvidenceDTO&) = default;
};

/// CCLC-owned compare-and-set release request. The Kernel evidence is obtained
/// from one fresh held status; carrying the evidence directly avoids a
/// recursive status/receipt DTO graph.
struct RouteConfigurationFenceReleaseRequestDTO final {
  PublicRequestContextDTO owner_request_context;
  RequestId initiating_public_request_id;
  RouteSelectionTransactionId route_selection_transaction_id;
  RouteSelectionTransactionGeneration route_selection_transaction_generation;
  RouteConfigurationFenceAcceptedDTO accepted_fence;
  std::uint64_t release_precondition_status_sequence{};
  ContentDigest release_precondition_status_digest;
  ExpectedActiveControlConfigurationDTO release_active_control_configuration;
  RouteConfigurationFenceReconciliationEvidenceDTO held_evidence;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const RouteConfigurationFenceReleaseRequestDTO&,
                         const RouteConfigurationFenceReleaseRequestDTO&) = default;
};

struct RouteConfigurationFenceReleasedDTO final {
  RouteConfigurationFenceReleaseRequestDTO request;
  RequestReplayStatusDTO replay_status;
  std::int64_t released_at_ns{};
  ConfigurationFenceId configuration_fence_id;
  ConfigurationFenceGeneration configuration_fence_generation{0U};
  ContentDigest released_scope_digest;
  ContentDigest receipt_digest;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const RouteConfigurationFenceReleasedDTO&,
                         const RouteConfigurationFenceReleasedDTO&) = default;
};

struct RouteConfigurationFenceStatusDTO final {
  RouteConfigurationFenceStatusQueryDTO query;
  RouteConfigurationFenceReconciliationEvidenceDTO evidence;
  RouteConfigurationFenceStateDTO state;
  bool admission_fenced{};
  std::optional<RouteConfigurationFenceReleasedDTO> release_receipt;
  std::string required_recovery_direction;
  ContentDigest result_digest;
  std::optional<ErrorDTO> error;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const RouteConfigurationFenceStatusDTO&,
                         const RouteConfigurationFenceStatusDTO&) = default;
};

struct RouteConfigurationFenceAcquireAttemptReconciliationDTO final {
  RouteConfigurationFenceIntentDTO intent;
  RouteConfigurationFenceAcquireAttemptDispositionDTO disposition;
  std::optional<RouteConfigurationFenceAcceptedDTO> acceptance;
  std::optional<RouteConfigurationFenceStatusDTO> current_status;
  std::optional<ErrorDTO> error;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const RouteConfigurationFenceAcquireAttemptReconciliationDTO&,
                         const RouteConfigurationFenceAcquireAttemptReconciliationDTO&) = default;
};

struct RouteConfigurationFenceAcquireEffectResultDTO final {
  RouteConfigurationFenceIntentDTO intent;
  RouteConfigurationFenceAcquireEffectDispositionDTO disposition;
  std::optional<RouteConfigurationFenceAcceptedDTO> acceptance;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const RouteConfigurationFenceAcquireEffectResultDTO&,
                         const RouteConfigurationFenceAcquireEffectResultDTO&) = default;
};

struct RouteConfigurationFenceReleaseAttemptReconciliationDTO final {
  RouteConfigurationFenceReleaseRequestDTO request;
  RouteConfigurationFenceReleaseAttemptDispositionDTO disposition;
  std::optional<RouteConfigurationFenceReleasedDTO> receipt;
  std::optional<RouteConfigurationFenceStatusDTO> current_status;
  std::optional<ErrorDTO> error;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const RouteConfigurationFenceReleaseAttemptReconciliationDTO&,
                         const RouteConfigurationFenceReleaseAttemptReconciliationDTO&) = default;
};

struct RouteConfigurationFenceReleaseEffectResultDTO final {
  RouteConfigurationFenceReleaseRequestDTO request;
  RouteConfigurationFenceReleaseEffectDispositionDTO disposition;
  std::optional<RouteConfigurationFenceReleasedDTO> receipt;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const RouteConfigurationFenceReleaseEffectResultDTO&,
                         const RouteConfigurationFenceReleaseEffectResultDTO&) = default;
};

enum class RouteSelectionTransactionPhaseDTO : std::uint8_t {
  kAccepted = 0,
  kAcquiringFence,
  kObserving,
  kDeactivating,
  kActivating,
  kVerifying,
  kPublishing,
  kReleasingFence,
  kOutcomeUnknown,
  kReconciling,
  kSucceeded,
  kFailed,
};

enum class RouteSelectionEffectDTO : std::uint8_t {
  kNone = 0,
  kKnownNoEffect,
  kEffectObserved,
  kPartialEffectObserved,
  kEffectUncertain,
};

enum class RouteSelectionTerminalResultDTO : std::uint8_t {
  kSucceeded = 0,
  kFailedKnownNoEffect,
  kFailedPartialEffect,
};

/// Durable semantic acceptance for the complete-domain request.
struct RouteSelectionAcceptedDTO final {
  RequestId request_id;
  RequestReplayStatusDTO replay_status;
  RouteSelectionTransactionId route_selection_transaction_id;
  RouteSelectionTransactionGeneration route_selection_transaction_generation;
  RetainedBundleId accepted_retained_bundle_id;
  GraphRevision accepted_graph_revision;
  DesiredControlConfigurationDTO desired_configuration;
  std::vector<RouteSelectionComponentExpectationDTO> component_expectations;
  std::optional<ActiveControlConfigurationDTO> predecessor_active_control_configuration;
  ConfigurationFenceScopeDTO configuration_fence_scope;
  ControllerLifecycleObservationQueryDTO controller_lifecycle_observation_query;
  ActiveControlConfigurationId candidate_active_control_configuration_id;
  ActiveControlConfigurationGeneration candidate_active_control_configuration_generation{0U};
  std::int64_t accepted_at_ns{};
  std::int64_t timeout_duration_ns{};

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const RouteSelectionAcceptedDTO&,
                         const RouteSelectionAcceptedDTO&) = default;
};

struct RouteSelectionStatusDTO final {
  RouteSelectionAcceptedDTO acceptance;
  RouteSelectionIntentDTO accepted_intent;
  std::uint64_t status_sequence{};
  RequestOwnerEpoch status_owner_epoch;
  RouteSelectionTransactionPhaseDTO phase;
  RouteSelectionEffectDTO effect;
  std::vector<ControlBindingId> affected_control_binding_ids;
  std::vector<ControllerRoleId> affected_controller_role_ids;
  std::optional<ControllerLifecycleObservationDTO> controller_observation;
  std::optional<ControllerLifecycleReconciliationDTO> controller_reconciliation;
  std::optional<ControllerLifecycleTransitionPlanDTO> controller_transition_plan;
  std::vector<ControllerLifecycleTransitionResultDTO> controller_transition_results;
  std::optional<RouteConfigurationFenceAcceptedDTO> accepted_fence;
  std::optional<RouteConfigurationFenceReleasedDTO> released_fence;
  std::optional<ActiveControlConfigurationDTO> resulting_active_control_configuration;
  bool reconciliation_required{};
  std::optional<RouteSelectionTerminalResultDTO> terminal_result;
  std::optional<ErrorDTO> error;
  std::int64_t accepted_at_ns{};
  std::int64_t latest_updated_at_ns{};
  std::int64_t remaining_duration_ns{};

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const RouteSelectionStatusDTO&, const RouteSelectionStatusDTO&) = default;
};

enum class CCLCRouteEffectKindDTO : std::uint8_t {
  kKernelRouteConfigurationFenceAcquire = 0,
  kControllerLifecycleTransition,
  kKernelRouteConfigurationFenceRelease,
};

enum class CCLCRouteEffectAttemptStateDTO : std::uint8_t {
  kPrepared = 0,
  kCompleted,
};

using CCLCRouteEffectRequestDTO =
    std::variant<RouteConfigurationFenceIntentDTO, ControllerLifecycleTransitionIntentDTO,
                 RouteConfigurationFenceReleaseRequestDTO>;
using CCLCRouteEffectResultDTO = std::variant<RouteConfigurationFenceAcquireEffectResultDTO,
                                              ControllerLifecycleTransitionResultDTO,
                                              RouteConfigurationFenceReleaseEffectResultDTO>;

/// Typed route effect row consumed later by the shared CCLC journal adapter.
struct CCLCRouteEffectAttemptDTO final {
  RouteSelectionTransactionId route_selection_transaction_id;
  RouteSelectionTransactionGeneration route_selection_transaction_generation;
  std::uint64_t effect_sequence{};
  CCLCRouteEffectKindDTO effect_kind;
  CCLCRouteEffectAttemptStateDTO state;
  CCLCRouteEffectRequestDTO request;
  std::optional<CCLCRouteEffectResultDTO> result;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const CCLCRouteEffectAttemptDTO&,
                         const CCLCRouteEffectAttemptDTO&) = default;
};

struct CCLCRouteEffectSequenceWatermarkDTO final {
  RouteSelectionTransactionId route_selection_transaction_id;
  RouteSelectionTransactionGeneration route_selection_transaction_generation;
  std::uint64_t highest_effect_sequence{};

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const CCLCRouteEffectSequenceWatermarkDTO&,
                         const CCLCRouteEffectSequenceWatermarkDTO&) = default;
};

struct ControlDomainActiveConfigurationGenerationWatermarkDTO final {
  ControlDomainId control_domain_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation{0U};

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ControlDomainActiveConfigurationGenerationWatermarkDTO&,
                         const ControlDomainActiveConfigurationGenerationWatermarkDTO&) = default;
};

struct CCLCRouteSelectionRequestResultDTO final {
  RequestReplayStatusDTO replay_status;
  std::optional<RouteSelectionAcceptedDTO> acceptance;
  std::optional<RouteSelectionStatusDTO> status;
  std::optional<ErrorDTO> rejection{};

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const CCLCRouteSelectionRequestResultDTO&,
                         const CCLCRouteSelectionRequestResultDTO&) = default;
};

/// Exact already-read facts used by the future CCLC route journal CAS.
struct RouteSelectionAdmissionCandidate final {
  RouteSelectionTransactionId route_selection_transaction_id;
  RouteSelectionTransactionGeneration route_selection_transaction_generation;
  ActiveControlConfigurationId candidate_active_control_configuration_id;
  ActiveControlConfigurationGeneration candidate_active_control_configuration_generation{0U};
  /// Highest candidate generation durably minted for this domain. It may be
  /// greater than the current published configuration after a candidate
  /// fails known-no-effect; candidate generations are never reused.
  ActiveControlConfigurationGeneration durable_active_configuration_generation_watermark{0U};
  std::int64_t accepted_at_ns{};
  std::int64_t timeout_duration_ns{};
  RetainedBundleId current_retained_bundle_id;
  GraphRevision current_graph_revision;
  std::vector<RouteSelectionComponentExpectationDTO> current_component_expectations;
  std::optional<ActiveControlConfigurationDTO> current_active_control_configuration;
  ControllerLifecycleExpectedDomainDTO expected_controller_domain;
  ConfigurationFenceScopeDTO configuration_fence_scope;
  RouteConfigurationFenceAdmissionSnapshotDTO configuration_fence_admission_snapshot;
};

struct PreparedRouteSelectionAdmission final {
  RouteSelectionAcceptedDTO acceptance;
  RouteSelectionStatusDTO initial_status;
  std::vector<ControlDomainId> reserved_control_domains;
  ControllerLifecycleExpectedDomainDTO expected_controller_domain;
};

/// Pure preparation after replay classification and source reads, but before
/// the shared reservation or durable journal CAS.
[[nodiscard]] Result<PreparedRouteSelectionAdmission> PrepareRouteSelectionAdmission(
    const RouteSelectionIntentDTO& intent, const RouteSelectionAdmissionCandidate& candidate);

}  // namespace manyforge::core
