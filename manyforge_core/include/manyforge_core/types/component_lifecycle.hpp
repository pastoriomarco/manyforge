#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/ports/iexecution_port.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/active_bundle_publication.hpp>
#include <manyforge_core/types/active_control_configuration.hpp>
#include <manyforge_core/types/cell_admission.hpp>
#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/stop_certificate.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

inline constexpr std::size_t kMaxLifecycleExpectedDomains = 256U;
inline constexpr std::size_t kMaxLifecycleResolvedDomains = 256U;
inline constexpr std::size_t kMaxLifecycleDisableDomainUnion = 512U;
inline constexpr std::size_t kMaxLifecycleExpectedBindings = 4096U;
inline constexpr std::size_t kMaxLifecycleResolvedBindings = 4096U;
inline constexpr std::size_t kMaxLifecycleStatusBindingUnion = 8192U;
inline constexpr std::size_t kMaxLifecycleFacts = 256U;
inline constexpr std::size_t kMaxLifecycleAttemptsPerDomain = 64U;
inline constexpr std::size_t kMaxLifecycleAttemptsPerStatus = 4096U;
inline constexpr std::size_t kMaxLifecycleCurrentResources = 4096U;
inline constexpr std::size_t kMaxLifecycleCancelledOperations = 4096U;
inline constexpr std::size_t kMaxLifecycleIdentityBytes = 128U;
inline constexpr std::size_t kMaxLifecycleRequestLabelBytes = 256U;
inline constexpr std::size_t kMaxLifecycleErrorCodeBytes = 128U;
inline constexpr std::size_t kMaxLifecycleErrorMessageBytes = 1024U;
inline constexpr std::size_t kMaxLifecycleErrorDetails = 8U;
inline constexpr std::size_t kMaxLifecycleErrorDetailKeyBytes = 64U;
inline constexpr std::size_t kMaxLifecycleErrorDetailValueBytes = 256U;
inline constexpr std::size_t kMaxLifecycleKinematicSourceProjections = 4096U;
inline constexpr std::size_t kMaxLifecycleKinematicContributingBindings = 8192U;
inline constexpr std::size_t kMaxLifecycleReasonBytes = 1024U;
inline constexpr std::size_t kMaxLifecycleFactTokenBytes = 128U;
inline constexpr std::size_t kMaxLifecycleCatalogPlans = 6U;
inline constexpr std::size_t kMaxLifecycleTransitionsPerPlan = 64U;
inline constexpr std::size_t kMaxLifecycleTransitionsPerCatalog = 256U;
inline constexpr std::size_t kMaxLifecyclePredicateFacts = 256U;
inline constexpr std::size_t kMaxLifecycleProviderOccurrences = 256U;
inline constexpr std::int64_t kMaxLifecycleTransactionTimeoutNs = 3'600'000'000'000LL;

enum class ComponentLifecycleIntentKindDTO : std::uint8_t {
  kPrepare = 0,
  kEnable,
  kDisable,
  kRecover,
};

enum class ComponentLifecycleStateDTO : std::uint8_t {
  kUndeclared = 0,
  kDeclared,
  kPrepared,
  kActive,
  kDegraded,
  kFaulted,
  kDisabling,
  kDisabled,
};

enum class ComponentLifecycleRequestedStatePresenceDTO : std::uint8_t {
  kUnresolved = 0,
  kResolved,
};

enum class ExpectedActiveControlConfigurationPresenceDTO : std::uint8_t {
  kAbsent = 0,
  kPresent,
};

enum class LifecycleFactValueKindDTO : std::uint8_t {
  kUnknown = 0,
  kBoolean,
  kInteger,
  kToken,
};

enum class LifecyclePrimitiveDispatchStateDTO : std::uint8_t {
  kNotDispatched = 0,
  kDispatchedKnown,
  kDispatchedUncertain,
};

enum class LifecyclePrimitiveKnownResultDTO : std::uint8_t {
  kSucceeded = 0,
  kFailed,
};

enum class LifecyclePrimitiveEffectDTO : std::uint8_t {
  kNone = 0,
  kPossible,
  kObserved,
};

enum class LifecyclePostconditionResultDTO : std::uint8_t {
  kSatisfied = 0,
  kNotSatisfied,
  kInconclusive,
};

enum class ComponentLifecycleDomainActionStateDTO : std::uint8_t {
  kReserved = 0,
  kNotStarted,
  kExecuting,
  kVerifying,
  kReconciling,
  kSucceeded,
  kFailed,
  kOutcomeUnknown,
};

enum class ComponentLifecycleDomainEffectDTO : std::uint8_t {
  kNone = 0,
  kKnownNoEffect,
  kEffectObserved,
  kPartialEffectObserved,
  kEffectUncertain,
};

enum class ComponentLifecycleTransactionPhaseDTO : std::uint8_t {
  kAccepted = 0,
  kAcquiringDomains,
  kContaining,
  kExecuting,
  kVerifying,
  kOutcomeUnknown,
  kReconciling,
  kSucceeded,
  kFailed,
};

enum class ComponentLifecycleTerminalResultDTO : std::uint8_t {
  kSucceeded = 0,
  kFailedKnownNoEffect,
  kFailedPartialEffect,
};

[[nodiscard]] constexpr std::string_view to_string(ComponentLifecycleIntentKindDTO value) noexcept {
  switch (value) {
    case ComponentLifecycleIntentKindDTO::kPrepare:
      return "prepare";
    case ComponentLifecycleIntentKindDTO::kEnable:
      return "enable";
    case ComponentLifecycleIntentKindDTO::kDisable:
      return "disable";
    case ComponentLifecycleIntentKindDTO::kRecover:
      return "recover";
  }
  return {};
}

[[nodiscard]] constexpr std::string_view to_string(ComponentLifecycleStateDTO value) noexcept {
  switch (value) {
    case ComponentLifecycleStateDTO::kUndeclared:
      return "undeclared";
    case ComponentLifecycleStateDTO::kDeclared:
      return "declared";
    case ComponentLifecycleStateDTO::kPrepared:
      return "prepared";
    case ComponentLifecycleStateDTO::kActive:
      return "active";
    case ComponentLifecycleStateDTO::kDegraded:
      return "degraded";
    case ComponentLifecycleStateDTO::kFaulted:
      return "faulted";
    case ComponentLifecycleStateDTO::kDisabling:
      return "disabling";
    case ComponentLifecycleStateDTO::kDisabled:
      return "disabled";
  }
  return {};
}

struct ComponentLifecycleRequestedStateDTO final {
  ComponentLifecycleRequestedStatePresenceDTO presence;
  std::optional<ComponentLifecycleStateDTO> state;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ComponentLifecycleRequestedStateDTO&,
                         const ComponentLifecycleRequestedStateDTO&) = default;
};

struct ExpectedActiveControlConfigurationDTO final {
  ExpectedActiveControlConfigurationPresenceDTO presence;
  std::optional<ActiveControlConfigurationId> active_control_configuration_id;
  std::optional<ActiveControlConfigurationGeneration> active_control_configuration_generation;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ExpectedActiveControlConfigurationDTO&,
                         const ExpectedActiveControlConfigurationDTO&) = default;
};

struct ExpectedControlBindingRevisionDTO final {
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  ControlRouteId control_route_id;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ExpectedControlBindingRevisionDTO&,
                         const ExpectedControlBindingRevisionDTO&) = default;
};

struct ExpectedControlDomainRevisionDTO final {
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ControllerTopologyId controller_topology_id;
  TopologyRevision topology_revision;
  ExpectedActiveControlConfigurationDTO expected_active_control_configuration;
  std::vector<ExpectedControlBindingRevisionDTO> expected_control_bindings;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ExpectedControlDomainRevisionDTO&,
                         const ExpectedControlDomainRevisionDTO&) = default;
};

/// Actual predecessor qualified by the original CCLC owner from a verified
/// snapshot. It does not create or replace a durable journal watermark.
struct ApplyComponentQualifiedPredecessorDTO final {
  ComponentLifecycleGeneration generation;
  ContentDigest snapshot_digest;

  [[nodiscard]] bool locally_valid() const noexcept;
  friend bool operator==(const ApplyComponentQualifiedPredecessorDTO&,
                         const ApplyComponentQualifiedPredecessorDTO&) = default;
};

/// Owner-read Q97 generation prediction. This value grants no authority;
/// delegated acceptance rechecks every field against the durable journal.
struct ApplyComponentGenerationSelectionDTO final {
  std::optional<ComponentLifecycleGeneration> predecessor_generation;
  ComponentLifecycleGeneration candidate_generation;
  std::uint64_t prior_reservation_generation{};
  std::optional<ApplyComponentQualifiedPredecessorDTO> owner_qualified_predecessor{};

  [[nodiscard]] bool locally_valid() const noexcept;
  friend bool operator==(const ApplyComponentGenerationSelectionDTO&,
                         const ApplyComponentGenerationSelectionDTO&) = default;
};

struct ComponentLifecycleIntentDTO final {
  PublicRequestContextDTO request_context;
  ControllableComponentId controllable_component_id;
  ComponentLifecycleIntentKindDTO intent;
  GraphRevision expected_graph_revision;
  ComponentLifecycleGeneration expected_component_lifecycle_generation;
  std::vector<ExpectedControlDomainRevisionDTO> expected_control_domains;
  std::optional<std::string> reason;
  RequestAttributionDTO attribution{};
  AdmissionGeneration expected_admission_generation{1U};
  std::optional<ProgramNodeAdmissionEvidenceDTO> program_node_evidence{};
  std::optional<ApplyComponentGenerationSelectionDTO> apply_generation_selection{};

  [[nodiscard]] bool locally_valid() const noexcept;
  [[nodiscard]] bool replay_equal(const ComponentLifecycleIntentDTO& other) const noexcept;

  friend bool operator==(const ComponentLifecycleIntentDTO&,
                         const ComponentLifecycleIntentDTO&) = default;
};

struct ComponentLifecycleAcceptedDTO final {
  RequestId request_id;
  RequestReplayStatusDTO replay_status;
  LifecycleTransactionId lifecycle_transaction_id;
  LifecycleTransactionGeneration lifecycle_transaction_generation;
  ComponentLifecycleGeneration component_lifecycle_generation;
  std::int64_t accepted_at_ns;
  std::int64_t timeout_duration_ns;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ComponentLifecycleAcceptedDTO&,
                         const ComponentLifecycleAcceptedDTO&) = default;
};

struct LifecycleFactValueDTO final {
  LifecycleFactValueKindDTO kind;
  std::optional<bool> boolean_value;
  std::optional<std::int64_t> integer_value;
  std::optional<std::string> token_value;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const LifecycleFactValueDTO&, const LifecycleFactValueDTO&) = default;
};

struct LifecycleFactDTO final {
  LifecycleFactId lifecycle_fact_id;
  LifecycleFactValueDTO value;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const LifecycleFactDTO&, const LifecycleFactDTO&) = default;
};

/// Pure projection of one exact provider's existing normalized health facts.
/// This value grants no admission and carries no execution transaction,
/// synthetic observation generation, source timestamp, or mutable state owner.
struct LifecycleDomainHealthDTO final {
  ComponentLifecycleStateDTO observed_state;
  std::vector<LifecycleFactDTO> facts;
  std::optional<ErrorDTO> diagnostic;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const LifecycleDomainHealthDTO&,
                         const LifecycleDomainHealthDTO&) = default;
};

/// Product-owned projection over the same maintained source as lifecycle
/// observation. Implementations must not advance observation/bookkeeping,
/// acquire authority, perform lifecycle effects, or manufacture a sample.
using LifecycleDomainHealthProjection =
    std::function<Result<LifecycleDomainHealthDTO>(std::int64_t now_ns)>;

struct LifecycleFactPredicateDTO final {
  LifecycleFactId lifecycle_fact_id;
  LifecycleFactValueDTO expected_value;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const LifecycleFactPredicateDTO&,
                         const LifecycleFactPredicateDTO&) = default;
};

struct LifecyclePredicateDTO final {
  std::vector<ComponentLifecycleStateDTO> allowed_states;
  std::vector<LifecycleFactPredicateDTO> fact_predicates;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const LifecyclePredicateDTO&, const LifecyclePredicateDTO&) = default;
};

struct BrandLifecycleObservationDTO;

/// Deterministically evaluate one closed lifecycle predicate against one exact
/// normalized observation. Invalid inputs fail closed as inconclusive; this
/// operation performs no I/O or clock reads.
[[nodiscard]] LifecyclePostconditionResultDTO EvaluateLifecyclePredicate(
    const LifecyclePredicateDTO& predicate,
    const BrandLifecycleObservationDTO& observation) noexcept;

[[nodiscard]] LifecyclePostconditionResultDTO EvaluateLifecyclePredicate(
    const LifecyclePredicateDTO& predicate,
    const LifecycleDomainHealthDTO& health) noexcept;

struct LifecyclePrimitiveTransitionDTO final {
  LifecyclePrimitiveId lifecycle_primitive_id;
  LifecyclePredicateDTO eligibility_predicate;
  LifecyclePredicateDTO postcondition_predicate;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const LifecyclePrimitiveTransitionDTO&,
                         const LifecyclePrimitiveTransitionDTO&) = default;
};

struct LifecycleIntentPlanDTO final {
  ComponentLifecycleIntentKindDTO intent;
  ComponentLifecycleStateDTO requested_state;
  std::optional<LifecyclePredicateDTO> recover_target_eligibility;
  LifecyclePredicateDTO requested_postcondition;
  std::int64_t timeout_duration_ns;
  std::vector<LifecyclePrimitiveTransitionDTO> transitions;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const LifecycleIntentPlanDTO&, const LifecycleIntentPlanDTO&) = default;
};

struct BrandLifecyclePlanCatalogDTO final {
  ProviderManifestId provider_manifest_id;
  ProviderManifestRevision provider_manifest_revision;
  ContentDigest catalog_digest;
  std::vector<LifecycleIntentPlanDTO> plans;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const BrandLifecyclePlanCatalogDTO&,
                         const BrandLifecyclePlanCatalogDTO&) = default;
};

struct BrandLifecycleObservationQueryDTO final {
  LifecycleTransactionId lifecycle_transaction_id;
  LifecycleTransactionGeneration lifecycle_transaction_generation;
  ControllableComponentId controllable_component_id;
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ProviderInstanceId provider_instance_id;
  ProviderManifestId provider_manifest_id;
  ProviderManifestRevision provider_manifest_revision;
  RuntimeUnitId provider_runtime_unit_id;
  RuntimeEpoch provider_runtime_epoch;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const BrandLifecycleObservationQueryDTO&,
                         const BrandLifecycleObservationQueryDTO&) = default;
};

struct BrandLifecycleObservationDTO final {
  LifecycleTransactionId lifecycle_transaction_id;
  LifecycleTransactionGeneration lifecycle_transaction_generation;
  ControllableComponentId controllable_component_id;
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ProviderInstanceId provider_instance_id;
  ProviderManifestId provider_manifest_id;
  ProviderManifestRevision provider_manifest_revision;
  RuntimeUnitId provider_runtime_unit_id;
  RuntimeEpoch provider_runtime_epoch;
  std::uint64_t source_sequence;
  std::uint64_t observation_generation;
  std::int64_t source_timestamp_ns;
  std::int64_t received_timestamp_ns;
  std::int64_t fresh_until_ns;
  ComponentLifecycleStateDTO observed_state;
  std::vector<LifecycleFactDTO> facts;
  // Bounded diagnostics from the same observation. They never determine
  // lifecycle eligibility, fault classification, or containment.
  std::optional<ErrorDTO> diagnostic{};
  std::shared_ptr<const content_identity::CanonicalValue> diagnostic_snapshot{};

  [[nodiscard]] bool locally_valid() const noexcept;
  [[nodiscard]] bool matches(const BrandLifecycleObservationQueryDTO& query) const noexcept;

  friend bool operator==(const BrandLifecycleObservationDTO&,
                         const BrandLifecycleObservationDTO&) = default;
};

struct BrandLifecyclePrimitiveIntentDTO final {
  LifecycleTransactionId lifecycle_transaction_id;
  LifecycleTransactionGeneration lifecycle_transaction_generation;
  ControllableComponentId controllable_component_id;
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ProviderInstanceId provider_instance_id;
  ProviderManifestId provider_manifest_id;
  ProviderManifestRevision provider_manifest_revision;
  RuntimeUnitId provider_runtime_unit_id;
  RuntimeEpoch provider_runtime_epoch;
  LifecyclePrimitiveId lifecycle_primitive_id;
  LifecyclePrimitiveAttemptId lifecycle_primitive_attempt_id;
  std::uint64_t expected_observation_generation;
  std::int64_t remaining_duration_ns;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const BrandLifecyclePrimitiveIntentDTO&,
                         const BrandLifecyclePrimitiveIntentDTO&) = default;
};

struct BrandLifecyclePrimitiveResultDTO final {
  LifecycleTransactionId lifecycle_transaction_id;
  LifecycleTransactionGeneration lifecycle_transaction_generation;
  ControllableComponentId controllable_component_id;
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ProviderInstanceId provider_instance_id;
  ProviderManifestId provider_manifest_id;
  ProviderManifestRevision provider_manifest_revision;
  RuntimeUnitId provider_runtime_unit_id;
  RuntimeEpoch provider_runtime_epoch;
  LifecyclePrimitiveId lifecycle_primitive_id;
  LifecyclePrimitiveAttemptId lifecycle_primitive_attempt_id;
  std::uint64_t expected_observation_generation;
  LifecyclePrimitiveDispatchStateDTO dispatch_state;
  LifecyclePrimitiveEffectDTO effect;
  std::optional<LifecyclePrimitiveKnownResultDTO> known_result;
  std::optional<ErrorDTO> error;

  [[nodiscard]] bool locally_valid() const noexcept;
  [[nodiscard]] bool matches(const BrandLifecyclePrimitiveIntentDTO& intent) const noexcept;

  friend bool operator==(const BrandLifecyclePrimitiveResultDTO&,
                         const BrandLifecyclePrimitiveResultDTO&) = default;
};

struct LifecycleReconciliationEvidenceDTO final {
  ProviderInstanceId provider_instance_id;
  ProviderManifestId provider_manifest_id;
  ProviderManifestRevision provider_manifest_revision;
  RuntimeUnitId provider_runtime_unit_id;
  RuntimeEpoch provider_runtime_epoch;
  std::uint64_t source_sequence;
  std::uint64_t observation_generation;
  ComponentLifecycleStateDTO observed_state;
  LifecyclePostconditionResultDTO postcondition_result;
  std::int64_t observed_at_ns;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const LifecycleReconciliationEvidenceDTO&,
                         const LifecycleReconciliationEvidenceDTO&) = default;
};

struct ResolvedLifecycleControlDomainScopeDTO final {
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ControllerTopologyId controller_topology_id;
  TopologyRevision topology_revision;
  std::optional<ActiveControlConfigurationDTO> active_control_configuration;
  std::vector<ExpectedControlBindingRevisionDTO> current_control_bindings;
  std::vector<ResourceId> current_resources;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ResolvedLifecycleControlDomainScopeDTO&,
                         const ResolvedLifecycleControlDomainScopeDTO&) = default;
};

struct ResolvedLifecycleProviderOccurrenceDTO final {
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ProviderInstanceId provider_instance_id;
  ProviderManifestId provider_manifest_id;
  ProviderManifestRevision provider_manifest_revision;
  RuntimeUnitId provider_runtime_unit_id;
  RuntimeEpoch provider_runtime_epoch;
  ContentDigest lifecycle_plan_catalog_digest;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ResolvedLifecycleProviderOccurrenceDTO&,
                         const ResolvedLifecycleProviderOccurrenceDTO&) = default;
};

struct ComponentLifecycleCurrentSnapshotDTO final {
  ContentDigest snapshot_digest;
  ControllableComponentId controllable_component_id;
  ComponentLifecycleGeneration component_lifecycle_generation;
  ComponentLifecycleStateDTO component_lifecycle_state;
  ActiveBundlePublicationReceiptDTO active_publication_receipt;
  std::vector<ResolvedLifecycleControlDomainScopeDTO> domains;
  std::vector<ResolvedLifecycleProviderOccurrenceDTO> provider_occurrences;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ComponentLifecycleCurrentSnapshotDTO&,
                         const ComponentLifecycleCurrentSnapshotDTO&) = default;
};

/// Owner-local controller occurrence transferred only by selected bring-up.
/// CCLC's component watermark is deliberately not mirrored here. Zero is a
/// real domain floor before its first observation, not a ready observation.
struct SelectedDomainOccurrenceDTO final {
  ControlDomainId control_domain_id;
  std::uint64_t observation_generation{};
  friend bool operator==(const SelectedDomainOccurrenceDTO&,
                         const SelectedDomainOccurrenceDTO&) = default;
};

struct SelectedControllerOccurrenceDTO final {
  std::optional<ExecutionBindingEpoch> arm_binding_epoch;
  std::vector<SelectedDomainOccurrenceDTO> domains;
  [[nodiscard]] bool locally_valid() const noexcept;
  friend bool operator==(const SelectedControllerOccurrenceDTO&,
                         const SelectedControllerOccurrenceDTO&) = default;
};

/// Pure successor construction from the exact owned predecessor, before any
/// retirement. Continuing domains advance; new domains use first generation
/// one; removed domains are absent. It grants no effect or publication right.
[[nodiscard]] Result<SelectedControllerOccurrenceDTO> ComputeSelectedControllerSuccessorOccurrence(
    const SelectedControllerOccurrenceDTO& predecessor,
    const std::vector<ControlDomainId>& candidate_domains, bool candidate_has_arm);

inline constexpr std::string_view kComponentLifecycleCurrentSnapshotIdentityDomain =
    "component-lifecycle-current-snapshot";
inline constexpr std::string_view kComponentLifecycleCurrentSnapshotApiVersion =
    "manyforge.component-lifecycle-current-snapshot.v1";

/// Computes the canonical identity of every semantic snapshot field except
/// `snapshot_digest`.  The input must otherwise be a complete locally valid
/// snapshot; no default, inferred fact, or caller expectation is introduced.
[[nodiscard]] Result<ContentDigest> ComputeComponentLifecycleCurrentSnapshotDigest(
    const ComponentLifecycleCurrentSnapshotDTO& snapshot);

/// Independently verifies both the closed snapshot shape and its canonical
/// digest.  This is the common readback boundary for production composition.
[[nodiscard]] Result<void> VerifyComponentLifecycleCurrentSnapshot(
    const ComponentLifecycleCurrentSnapshotDTO& snapshot);

struct ComponentLifecycleDomainStatusDTO final {
  ControlDomainId control_domain_id;
  std::optional<ExpectedControlDomainRevisionDTO> expected_domain;
  std::optional<ResolvedLifecycleControlDomainScopeDTO> current_scope;
  std::optional<ResolvedLifecycleProviderOccurrenceDTO> resolved_provider_occurrence;
  std::optional<ComponentLifecycleStateDTO> previous_state;
  std::optional<ComponentLifecycleStateDTO> current_state;
  ComponentLifecycleRequestedStateDTO requested_state;
  ComponentLifecycleDomainActionStateDTO action_state;
  ComponentLifecycleDomainEffectDTO effect;
  std::vector<BrandLifecyclePrimitiveResultDTO> primitive_attempts;
  std::optional<LifecycleReconciliationEvidenceDTO> reconciliation_evidence;
  std::optional<ErrorDTO> error;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ComponentLifecycleDomainStatusDTO&,
                         const ComponentLifecycleDomainStatusDTO&) = default;
};

/// Durable selected bring-up evidence is observation/reconciliation data.
/// Shape validation never grants qualification, dispatch, or replay authority.
struct SelectedComponentBringUpEvidenceDTO final {
  ContentDigest offer_digest;
  ContentDigest predecessor_snapshot_digest;
  std::string incident_id;
  StopId containment_stop_id;
  StopGeneration containment_stop_generation;
  ProjectionRevisionDTO candidate_projection_revision;
  ContentDigest candidate_retained_bundle_digest;
  std::uint64_t candidate_retained_bundle_bytes;  // Byte count, never bundle payload.
  SelectedControllerOccurrenceDTO predecessor_occurrence;
  SelectedControllerOccurrenceDTO successor_occurrence;
  CellAdmissionClaimDTO selected_claim;
  ConfigurationFenceAcceptedDTO accepted_fence;
  ContentDigest original_request_payload_digest;

  friend bool operator==(const SelectedComponentBringUpEvidenceDTO&,
                         const SelectedComponentBringUpEvidenceDTO&) = default;
};

/// Real contained predecessor facts, absent for ordinary initial activation.
struct ApplyComponentBringUpRecoveryEvidenceDTO final {
  ContentDigest predecessor_snapshot_digest;
  std::string incident_id;
  StopId containment_stop_id;
  StopGeneration containment_stop_generation;

  friend bool operator==(const ApplyComponentBringUpRecoveryEvidenceDTO&,
                         const ApplyComponentBringUpRecoveryEvidenceDTO&) = default;
};

/// Observation of one Q91 component operation under the existing Apply
/// parent's exclusion. It is not an independent child Kernel claim. The
/// full Manager plan stays in Manager; the original request identity is sealed
/// separately. Restoring this value never recreates a continuation.
struct ApplyComponentBringUpEvidenceDTO final {
  ApplyComponentBringUpAuthorityDTO parent_authority;
  ConfigurationFenceScopeDTO selected_scope;
  ContentDigest original_request_payload_digest;
  ProjectionRevisionDTO candidate_projection_revision;
  ContentDigest candidate_retained_bundle_digest;
  std::uint64_t candidate_retained_bundle_bytes{};
  RuntimeUnitEpochBindingDTO original_execution_host;
  std::optional<ApplyComponentBringUpRecoveryEvidenceDTO> recovery;
  std::optional<SelectedControllerOccurrenceDTO> predecessor_occurrence;
  /// Transfer lower bound, not a device observation or proof of readiness.
  std::optional<SelectedControllerOccurrenceDTO> successor_occurrence_floor;

  friend bool operator==(const ApplyComponentBringUpEvidenceDTO&,
                         const ApplyComponentBringUpEvidenceDTO&) = default;
};

struct ComponentLifecycleStatusDTO final {
  ComponentLifecycleAcceptedDTO acceptance;
  ComponentLifecycleIntentDTO accepted_intent;
  std::uint64_t status_sequence;
  RequestOwnerEpoch status_owner_epoch;
  std::optional<ComponentLifecycleStateDTO> previous_state;
  std::optional<ComponentLifecycleStateDTO> current_state;
  ComponentLifecycleRequestedStateDTO requested_state;
  ComponentLifecycleTransactionPhaseDTO phase;
  std::vector<ComponentLifecycleDomainStatusDTO> domains;
  bool reconciliation_required;
  std::optional<ComponentLifecycleTerminalResultDTO> terminal_result;
  std::optional<ErrorDTO> error;
  std::int64_t accepted_at_ns;
  std::int64_t latest_updated_at_ns;
  std::int64_t remaining_duration_ns;
  std::optional<SelectedComponentBringUpEvidenceDTO> selected_bring_up{};
  std::optional<ApplyComponentBringUpEvidenceDTO> apply_bring_up{};

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ComponentLifecycleStatusDTO&,
                         const ComponentLifecycleStatusDTO&) = default;
};

[[nodiscard]] bool ExpectedDomainMatchesResolvedScope(
    const ExpectedControlDomainRevisionDTO& expected,
    const ResolvedLifecycleControlDomainScopeDTO& current) noexcept;

}  // namespace manyforge::core
