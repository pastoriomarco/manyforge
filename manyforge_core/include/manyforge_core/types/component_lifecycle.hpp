#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/active_bundle_publication.hpp>
#include <manyforge_core/types/active_control_configuration.hpp>
#include <manyforge_core/types/cell_admission.hpp>
#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/stop_certificate.hpp>
#include <manyforge_core/types/strong_id.hpp>
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

enum class DisableHandoffStateDTO : std::uint8_t {
  kStopping = 0,
  kReady,
  kFailed,
};

enum class DisableHandoffAttemptDispositionDTO : std::uint8_t {
  kAcceptedExact = 0,
  kNotSeen,
  kConflicting,
  kUncertain,
};

enum class DisableHandoffScopeRequestKindDTO : std::uint8_t {
  kResolvedComponentScope = 0,
  kFullStopRequired,
};

enum class DisableHandoffScopeResolutionDTO : std::uint8_t {
  kExact = 0,
  kStaleButUnambiguouslyResolved,
  kPromotedToFull,
};

enum class DisableHandoffEffectDispositionDTO : std::uint8_t {
  kAccepted = 0,
  kProvenNotSeen,
};

enum class DisableFenceReleaseAttemptDispositionDTO : std::uint8_t {
  kReleasedExact = 0,
  kNotReleasedHeld,
  kConflicting,
  kUncertain,
};

enum class DisableFenceReleaseEffectDispositionDTO : std::uint8_t {
  kReleased = 0,
  kProvenNotReleasedHeld,
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

/// Kernel-minted lifecycle-owned view of one configuration fence.
struct ConfigurationFenceHandleDTO final {
  ConfigurationFenceId configuration_fence_id;
  ConfigurationFenceGeneration configuration_fence_generation;
  LifecycleTransactionId lifecycle_transaction_id;
  LifecycleTransactionGeneration lifecycle_transaction_generation;
  ControllableComponentId controllable_component_id;
  ConfigurationFenceScopeDTO scope;
  std::string reason;
  ConfigurationFenceState state;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ConfigurationFenceHandleDTO&,
                         const ConfigurationFenceHandleDTO&) = default;
};

struct CurrentDomainActiveControlConfigurationDTO final {
  ControlDomainId control_domain_id;
  ExpectedActiveControlConfigurationDTO active_control_configuration;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const CurrentDomainActiveControlConfigurationDTO&,
                         const CurrentDomainActiveControlConfigurationDTO&) = default;
};

struct DisableHandoffComponentScopeExpectationDTO final {
  ContentDigest component_snapshot_digest;
  ActiveBundlePublicationGeneration active_publication_generation;
  ContentDigest active_public_pointer_digest;
  RetainedBundleId retained_bundle_id;
  GraphRevision graph_revision;
  ComponentLifecycleGeneration component_lifecycle_generation;
  ConfigurationFenceScopeDTO component_scope;
  std::vector<CurrentDomainActiveControlConfigurationDTO> current_active_control_configurations;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const DisableHandoffComponentScopeExpectationDTO&,
                         const DisableHandoffComponentScopeExpectationDTO&) = default;
};

struct DisableHandoffAdmissionQueryDTO final {
  ControllableComponentId controllable_component_id;
  DisableHandoffScopeRequestKindDTO request_kind;
  std::optional<DisableHandoffComponentScopeExpectationDTO> component_expectation;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const DisableHandoffAdmissionQueryDTO&,
                         const DisableHandoffAdmissionQueryDTO&) = default;
};

struct DisableHandoffAdmissionSnapshotDTO final {
  DisableHandoffAdmissionQueryDTO query;
  RequestOwnerEpoch request_owner_epoch;
  DisableHandoffScopeResolutionDTO scope_resolution;
  ConfigurationFenceScopeDTO current_scope;
  ActiveBundlePublicationGeneration active_publication_generation;
  ContentDigest active_public_pointer_digest;
  RetainedBundleId retained_bundle_id;
  GraphRevision graph_revision;
  std::uint64_t operation_resource_generation;
  ConfigurationFenceGeneration configuration_fence_generation;
  std::vector<CurrentDomainActiveControlConfigurationDTO> current_active_control_configurations;
  ContentDigest snapshot_digest;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const DisableHandoffAdmissionSnapshotDTO&,
                         const DisableHandoffAdmissionSnapshotDTO&) = default;
};

inline constexpr std::string_view kDisableHandoffAdmissionSnapshotIdentityDomain =
    "manyforge.kernel-disable-admission-snapshot.v1";
inline constexpr std::string_view kDisableHandoffAdmissionSnapshotApiVersion =
    "manyforge.kernel-disable-admission-snapshot.v1";
inline constexpr std::string_view kDisableHandoffScopeIdentityDomain =
    "kernel-disable-component-scope";

/// Computes the canonical digest of every Disable-admission field except
/// `snapshot_digest`.  The initial configuration-fence predecessor generation
/// is allowed to be zero; accepted fence generations remain nonzero.
[[nodiscard]] Result<ContentDigest> ComputeDisableHandoffAdmissionSnapshotDigest(
    const DisableHandoffAdmissionSnapshotDTO& snapshot);

/// Independently verifies the closed snapshot shape and canonical digest.
[[nodiscard]] Result<void> VerifyDisableHandoffAdmissionSnapshot(
    const DisableHandoffAdmissionSnapshotDTO& snapshot);

/// Canonical identity of the complete actual component/full fence scope used
/// by Disable acceptance, status, and release compare-and-set records.
[[nodiscard]] Result<ContentDigest> ComputeDisableHandoffScopeDigest(
    const ConfigurationFenceScopeDTO& scope);

/// Exact internal CCLC-to-Kernel containment/fence handoff request.
struct DisableHandoffRequestDTO final {
  PublicRequestContextDTO owner_request_context;
  RequestId initiating_public_request_id;
  LifecycleTransactionId lifecycle_transaction_id;
  LifecycleTransactionGeneration lifecycle_transaction_generation;
  ControllableComponentId controllable_component_id;
  DisableHandoffScopeResolutionDTO scope_resolution;
  ConfigurationFenceScopeDTO affected_scope;
  ContentDigest disable_admission_snapshot_digest;
  ActiveBundlePublicationGeneration expected_active_publication_generation;
  ContentDigest expected_active_public_pointer_digest;
  GraphRevision expected_graph_revision;
  RetainedBundleId expected_retained_bundle_id;
  ComponentLifecycleGeneration expected_component_lifecycle_generation;
  std::uint64_t expected_operation_resource_generation;
  ConfigurationFenceGeneration expected_fence_generation;
  std::vector<CurrentDomainActiveControlConfigurationDTO> expected_active_control_configurations;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const DisableHandoffRequestDTO&,
                         const DisableHandoffRequestDTO&) = default;
};

struct DisableHandoffAcceptedDTO final {
  RequestId request_id;
  RequestReplayStatusDTO replay_status;
  LifecycleTransactionId lifecycle_transaction_id;
  LifecycleTransactionGeneration lifecycle_transaction_generation;
  ControllableComponentId controllable_component_id;
  DisableHandoffScopeResolutionDTO scope_resolution;
  ContentDigest component_scope_digest;
  ConfigurationFenceId configuration_fence_id;
  ConfigurationFenceGeneration configuration_fence_generation;
  std::optional<StopId> stop_id;
  std::optional<StopGeneration> stop_generation;

  [[nodiscard]] bool locally_valid() const noexcept;
  [[nodiscard]] bool matches(const DisableHandoffRequestDTO& request) const noexcept;

  friend bool operator==(const DisableHandoffAcceptedDTO&,
                         const DisableHandoffAcceptedDTO&) = default;
};

struct DisableHandoffStatusDTO final {
  LifecycleTransactionId lifecycle_transaction_id;
  LifecycleTransactionGeneration lifecycle_transaction_generation;
  ControllableComponentId controllable_component_id;
  DisableHandoffScopeResolutionDTO scope_resolution;
  ContentDigest component_scope_digest;
  ConfigurationFenceId configuration_fence_id;
  ConfigurationFenceGeneration configuration_fence_generation;
  DisableHandoffStateDTO state;
  ConfigurationFenceScopeDTO scope;
  ConfigurationFenceHandleDTO configuration_fence_handle;
  std::optional<StopCertificateRefDTO> stop_certificate;
  std::vector<OperationId> cancelled_queued_operations;
  std::optional<ErrorDTO> error;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const DisableHandoffStatusDTO&, const DisableHandoffStatusDTO&) = default;
};

struct DisableHandoffAttemptReconciliationDTO final {
  DisableHandoffRequestDTO request;
  DisableHandoffAttemptDispositionDTO disposition;
  std::optional<DisableHandoffAcceptedDTO> acceptance;
  std::optional<DisableHandoffStatusDTO> status;
  std::optional<ErrorDTO> error;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const DisableHandoffAttemptReconciliationDTO&,
                         const DisableHandoffAttemptReconciliationDTO&) = default;
};

struct DisableHandoffEffectResultDTO final {
  DisableHandoffRequestDTO request;
  DisableHandoffEffectDispositionDTO disposition;
  std::optional<DisableHandoffAcceptedDTO> acceptance;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const DisableHandoffEffectResultDTO&,
                         const DisableHandoffEffectResultDTO&) = default;
};

struct DisableFenceReleaseRequestDTO final {
  PublicRequestContextDTO owner_request_context;
  RequestId initiating_public_request_id;
  LifecycleTransactionId lifecycle_transaction_id;
  LifecycleTransactionGeneration lifecycle_transaction_generation;
  ControllableComponentId controllable_component_id;
  DisableHandoffStatusDTO ready_handoff;
  std::uint64_t release_precondition_status_sequence;
  ContentDigest release_precondition_status_digest;
  DisableHandoffComponentScopeExpectationDTO current_component_expectation;
  DisableHandoffAdmissionSnapshotDTO current_admission_snapshot;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const DisableFenceReleaseRequestDTO&,
                         const DisableFenceReleaseRequestDTO&) = default;
};

struct DisableFenceReleasedDTO final {
  DisableFenceReleaseRequestDTO request;
  RequestReplayStatusDTO replay_status;
  std::int64_t released_at_ns;
  ConfigurationFenceId configuration_fence_id;
  ConfigurationFenceGeneration configuration_fence_generation;
  ContentDigest scope_digest;
  ContentDigest receipt_digest;

  [[nodiscard]] bool locally_valid() const noexcept;
  [[nodiscard]] bool matches(const DisableFenceReleaseRequestDTO& candidate) const noexcept;

  friend bool operator==(const DisableFenceReleasedDTO&, const DisableFenceReleasedDTO&) = default;
};

struct DisableFenceReleaseAttemptReconciliationDTO final {
  DisableFenceReleaseRequestDTO request;
  DisableFenceReleaseAttemptDispositionDTO disposition;
  std::optional<DisableFenceReleasedDTO> release_receipt;
  std::optional<DisableHandoffStatusDTO> current_handoff_status;
  std::optional<ErrorDTO> error;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const DisableFenceReleaseAttemptReconciliationDTO&,
                         const DisableFenceReleaseAttemptReconciliationDTO&) = default;
};

struct DisableFenceReleaseEffectResultDTO final {
  DisableFenceReleaseRequestDTO request;
  DisableFenceReleaseEffectDispositionDTO disposition;
  std::optional<DisableFenceReleasedDTO> release_receipt;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const DisableFenceReleaseEffectResultDTO&,
                         const DisableFenceReleaseEffectResultDTO&) = default;
};

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
  std::optional<ConfigurationFenceHandleDTO> configuration_fence;
  std::optional<DisableHandoffStatusDTO> disable_handoff;
  std::optional<DisableFenceReleasedDTO> disable_fence_released;
  bool reconciliation_required;
  std::optional<ComponentLifecycleTerminalResultDTO> terminal_result;
  std::optional<ErrorDTO> error;
  std::int64_t accepted_at_ns;
  std::int64_t latest_updated_at_ns;
  std::int64_t remaining_duration_ns;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ComponentLifecycleStatusDTO&,
                         const ComponentLifecycleStatusDTO&) = default;
};

[[nodiscard]] bool ExpectedDomainMatchesResolvedScope(
    const ExpectedControlDomainRevisionDTO& expected,
    const ResolvedLifecycleControlDomainScopeDTO& current) noexcept;

[[nodiscard]] bool DisableHandoffPreservesOrWidensScope(
    const DisableHandoffRequestDTO& request, const DisableHandoffStatusDTO& status) noexcept;

}  // namespace manyforge::core
