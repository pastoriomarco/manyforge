#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <manyforge_core/ports/icclc_journal.hpp>
#include <manyforge_core/ports/icclc_lifecycle_target_store.hpp>
#include <manyforge_core/ports/ioperation_journal.hpp>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace manyforge::core {

inline constexpr std::string_view kCCLCJournalSchemaId = "manyforge.internal.cclc.journal.v5";
inline constexpr std::int64_t kCCLCJournalSchemaVersion = 21;
inline constexpr std::int64_t kCCLCJournalSqliteApplicationId = 0x4D464C4A;
inline constexpr std::string_view kCCLCJournalFilename = "cclc-journal.sqlite3";
inline constexpr std::size_t kDefaultCCLCExactResultHistory = 128U;
inline constexpr std::size_t kMaximumCCLCExactResultHistory = 128U;
inline constexpr std::size_t kDefaultCCLCTerminalResultBytes = 512U * 1024U * 1024U;
inline constexpr std::size_t kMaximumCCLCTerminalResultBytes = 512U * 1024U * 1024U;

/// One component of a native Apply: the state its route gate must have and
/// the lifecycle generation CCLC assigns to it.
struct NativeApplyComponentFactDTO final {
  ControllableComponentId component_id;
  ComponentLifecycleStateDTO state{ComponentLifecycleStateDTO::kDeclared};
  ComponentLifecycleGeneration generation{0U};
  friend bool operator==(const NativeApplyComponentFactDTO&, const NativeApplyComponentFactDTO&) = default;
};

struct NativeApplyRouteGenerationDTO final {
  ControlDomainId control_domain_id;
  ActiveControlConfigurationGeneration generation{0U};
  friend bool operator==(const NativeApplyRouteGenerationDTO&, const NativeApplyRouteGenerationDTO&) = default;
};

/// Stable original delegated acceptance identity, never continuation authority.
struct ApplyComponentBringUpAcceptedReceiptDTO final {
  PublicRequestContextDTO request_context;
  ControllableComponentId component_id;
  ComponentLifecycleAcceptedDTO acceptance;
  ContentDigest original_request_payload_digest;
  friend bool operator==(const ApplyComponentBringUpAcceptedReceiptDTO&,
                         const ApplyComponentBringUpAcceptedReceiptDTO&) = default;
};

struct NativeApplyComponentBringUpDTO final {
  ApplyComponentBringUpReferenceDTO parent;
  std::vector<ApplyComponentBringUpAcceptedReceiptDTO> children;
  friend bool operator==(const NativeApplyComponentBringUpDTO&,
                         const NativeApplyComponentBringUpDTO&) = default;
};

/// Parent-Apply delegation to CCLC, recorded before any route is published.
/// The caller must hold and verify the exact parent Apply fence.
struct NativeApplyPlanRequestDTO final {
  NamedApplyPublicationTransactionDTO apply;
  RetainedBundleId candidate_retained_bundle_id;
  GraphRevision candidate_graph_revision;
  std::vector<ControllableComponentId> components;
  /// Every native control domain with the generation compiled into its
  /// runtime bindings (the floor for its planned generation).
  std::vector<NativeApplyRouteGenerationDTO> compiled_routes;
  /// Components of `components` whose runtime the Apply keeps (the Manager
  /// delta's kRetain rows). Each keeps its exact current lifecycle fact.
  std::vector<ControllableComponentId> retained_components;
  /// Predecessor components the Apply removes; never in `components`.
  std::vector<ControllableComponentId> removed_components;
  /// Live-readback current routes of retained domains (a subset of
  /// `compiled_routes`). Each must equal the journal's current fact exactly.
  std::vector<ActiveControlConfigurationDTO> retained_routes;
  /// Predecessor domains the Apply removes; never in `compiled_routes`.
  std::vector<ControlDomainId> removed_domains;
  /// The state of each changed component without an explicit Disable: kActive
  /// when the host enables it during Apply (native), kDeclared when the host
  /// only observes an externally managed controller (ROS 2 control) and the
  /// component reaches readiness later through ordinary CCLC Prepare/Enable.
  ComponentLifecycleStateDTO changed_component_state{ComponentLifecycleStateDTO::kActive};
  std::optional<NativeApplyComponentBringUpDTO> component_bring_up{};
};

/// CCLC's recorded decision for one native Apply: which components keep an
/// explicit Disable (their gate stays closed), whether the others start
/// Active or only Declared, and the generations the
/// candidate routes and component facts use. Generations continue from the
/// journal's watermarks and every earlier plan, so an A->B->A sequence never
/// reuses or falls behind a generation.
///
/// Retained components and routes carry their exact predecessor facts: no
/// generation advances, nothing is reserved. Only changed ones advance.
/// Removed components/domains lose their current facts at adoption while
/// their watermarks stay, so a later re-add always gets a newer generation.
struct NativeApplyPlanDTO final {
  NamedApplyPublicationTransactionDTO apply;
  RetainedBundleId candidate_retained_bundle_id;
  GraphRevision candidate_graph_revision;
  std::vector<NativeApplyComponentFactDTO> components;
  std::vector<NativeApplyRouteGenerationDTO> routes;
  std::vector<ControllableComponentId> retained_components;
  std::vector<ControlDomainId> retained_domains;
  std::vector<ControllableComponentId> removed_components;
  std::vector<ControlDomainId> removed_domains;
  std::optional<NativeApplyComponentBringUpDTO> component_bring_up{};
  friend bool operator==(const NativeApplyPlanDTO&, const NativeApplyPlanDTO&) = default;
};

/// Observation-only classifier for the new pre-Finalize preparation plan.
/// The owner must join the exact retained terminal and actual held parent
/// claim. It never authorizes effects, adoption, release or command readiness.
using NativeApplyParentPreparationFailureSource =
    std::function<Result<bool>(const NativeApplyPlanDTO&)>;

/// Private promotion of the planned facts after the matching Manager Apply
/// has published its candidate, but before its fence is released. Accepted
/// only when it matches the recorded plan exactly. This is not a public
/// lifecycle request or permission to perform effects.
struct NativeApplyFactAdoptionDTO final {
  ActiveBundlePublicationReceiptDTO publication;
  std::vector<NativeApplyComponentFactDTO> components;
  std::vector<ActiveControlConfigurationDTO> current_routes;
  std::optional<NativeApplyComponentBringUpDTO> component_bring_up{};
};

/// Per-write informational notice; no new durable counter or incident identity.
struct ComponentIncidentPruneNotice final {
  std::uint64_t count{};
  std::uint64_t oldest_sequence{};
  std::uint64_t newest_sequence{};
};

/// SQLite-backed CCLC lifecycle journal.
///
/// The adapter is deliberately separate from the pure Core target. It uses a
/// closed binary DTO codec and a defensive fixed-schema database; it does not
/// acquire lifecycle semantics or call any external primitive.
class SqliteCCLCJournal final : public ICCLCJournal, public ICCLCLifecycleTargetStore {
 public:
  SqliteCCLCJournal(
      std::filesystem::path database_path, CellId cell_id,
      std::size_t exact_result_history_capacity = kDefaultCCLCExactResultHistory,
      std::size_t terminal_result_bytes_capacity = kDefaultCCLCTerminalResultBytes,
      std::function<void(const ComponentIncidentPruneNotice&)> on_incidents_pruned = {},
      NativeApplyParentPreparationFailureSource preparation_failure_source = {});
  ~SqliteCCLCJournal() override;

  SqliteCCLCJournal(const SqliteCCLCJournal&) = delete;
  SqliteCCLCJournal& operator=(const SqliteCCLCJournal&) = delete;

  [[nodiscard]] Result<std::optional<NamedApplyPublicationTransactionDTO>>
  ReadUnsettledNativeApplyPlan() override;

  /// Receiver startup, even when it requires an exact settled predecessor.
  [[nodiscard]] Result<CCLCJournalRecoveryDTO> BeginOwnerEpoch(
      const RequestOwnerEpoch& owner_epoch, std::int64_t started_at_ns,
      std::size_t request_membership_capacity,
      const std::optional<RequestOwnerEpoch>& quiescent_predecessor = std::nullopt) override;

  [[nodiscard]] Result<CCLCJournalRecoveryDTO> BeginOwnerEpoch(
      const RequestOwnerEpoch& owner_epoch, std::int64_t started_at_ns,
      std::size_t request_membership_capacity,
      const std::optional<RequestOwnerEpoch>& quiescent_predecessor,
      CCLCOwnerEpochTransitionPurpose purpose) override;

  /// Observation only: exact active owner and the existing five-table settled predicate.
  /// The successor owner transition still performs its atomic predecessor recheck.
  [[nodiscard]] Result<void> ReadQuiescentOwner(const RequestOwnerEpoch& expected) override;

  [[nodiscard]] Result<std::optional<CCLCLifecycleRequestResultDTO>> InspectLifecycleRequest(
      const RequestOwnerEpoch& active_owner_epoch,
      const ComponentLifecycleIntentDTO& intent) override;

  [[nodiscard]] Result<std::optional<CCLCLifecycleRequestResultDTO>> ReadLifecycleRequestResult(
      const ComponentLifecycleIntentDTO& intent) override;

  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> RecordLifecycleRejection(
      const RequestOwnerEpoch&, const ComponentLifecycleIntentDTO&, const ErrorDTO&,
      const std::optional<CellAdmissionClaimIntentDTO>&) override;

  /// An accepted-new operator Prepare/Enable/Disable (any source label other
  /// than `kLifecycleTargetSourceLabel`) of a component with a present target
  /// also records its operator target in the same commit; a failure of
  /// either leaves neither (ADR-0026 §7, spec 210). An accepted-new operator
  /// Recover (no open incident) clears the component's retained
  /// intervention-requiring causes in its acceptance commit (spec 220).
  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> AcceptOrReplayLifecycle(
      const RequestOwnerEpoch& active_owner_epoch, const ComponentLifecycleIntentDTO& intent,
      const ComponentLifecycleAcceptedDTO& candidate_acceptance,
      const ComponentLifecycleStatusDTO& candidate_initial_status) override;
  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> AcceptOrReplayTargetLifecycle(
      const RequestOwnerEpoch&, const ComponentLifecycleIntentDTO&,
      const ComponentLifecycleAcceptedDTO&, const ComponentLifecycleStatusDTO&,
      const ComponentLifecycleTargetExpectationDTO&, const CellAdmissionClaimDTO&) override;
  [[nodiscard]] Result<std::optional<CCLCLifecycleRequestResultDTO>>
  InspectSelectedComponentBringUpRequest(const RequestOwnerEpoch&,
                                         const ComponentLifecycleIntentDTO&,
                                         const ContentDigest&) override;
  [[nodiscard]] Result<std::optional<CCLCLifecycleRequestResultDTO>>
  ReadSelectedComponentBringUpRequestResult(const ComponentLifecycleIntentDTO&,
                                            const ContentDigest&) override;
  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> AcceptOrReplaySelectedComponentBringUp(
      const RequestOwnerEpoch&, const ComponentLifecycleIntentDTO&,
      const ComponentLifecycleAcceptedDTO&, const ComponentLifecycleStatusDTO&,
      const ContentDigest&) override;
  [[nodiscard]] Result<void> RecordSelectedCellClaimAttempt(const RequestOwnerEpoch&,
                                                            const CellAdmissionClaimIntentDTO&,
                                                            const ContentDigest&) override;
  [[nodiscard]] Result<std::optional<CCLCLifecycleRequestResultDTO>>
  InspectApplyComponentBringUpRequest(const RequestOwnerEpoch&,
      const ComponentLifecycleIntentDTO&, const ContentDigest&) override;
  [[nodiscard]] Result<ApplyComponentGenerationSelectionDTO> ReadApplyComponentGenerationSelection(
      const RequestOwnerEpoch&, const ControllableComponentId&,
      const std::optional<ApplyComponentQualifiedPredecessorDTO>& = std::nullopt) override;
  [[nodiscard]] Result<std::optional<CCLCLifecycleRequestResultDTO>>
  ReadApplyComponentBringUpRequestResult(const ComponentLifecycleIntentDTO&,
      const ContentDigest&) override;
  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> AcceptOrReplayApplyComponentBringUp(
      const RequestOwnerEpoch&, const ComponentLifecycleIntentDTO&,
      const ComponentLifecycleAcceptedDTO&, const ComponentLifecycleStatusDTO&,
      const ContentDigest&) override;
  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> RecordSelectedComponentBringUpRejection(
      const RequestOwnerEpoch&, const ComponentLifecycleIntentDTO&, const ContentDigest&,
      const ErrorDTO&, const std::optional<CellAdmissionClaimIntentDTO>&) override;
  [[nodiscard]] Result<void> RecordCellClaimAttempt(const RequestOwnerEpoch&,
                                                    const CellAdmissionClaimIntentDTO&) override;
  [[nodiscard]] Result<void> RecordCellClaimAcquired(const RequestOwnerEpoch&,
                                                     const CellAdmissionClaimDTO&) override;
  [[nodiscard]] Result<std::uint64_t> RecordLifecycleTerminalAndClaimRelease(
      const RequestOwnerEpoch&, const ComponentLifecycleStatusDTO&,
      const CellAdmissionClaimReleaseDTO&) override;
  [[nodiscard]] Result<void> RecordCellClaimReleaseReceipt(
      const RequestOwnerEpoch&, const CellAdmissionClaimReleaseDTO&,
      const CellAdmissionRecoveryReceiptDTO&) override;
  [[nodiscard]] Result<void> RecordUnacceptedCellClaimRelease(
      const RequestOwnerEpoch&, const CellAdmissionClaimReleaseDTO&) override;
  [[nodiscard]] Result<void> RecordCellClaimNotAcquired(
      const RequestOwnerEpoch&, const CellAdmissionClaimIntentDTO&) override;

  [[nodiscard]] Result<std::uint64_t> RecordEffectPrepared(
      const RequestOwnerEpoch& active_owner_epoch, const CCLCEffectAttemptDTO& prepared_attempt,
      const ComponentLifecycleStatusDTO& successor_status) override;

  [[nodiscard]] Result<std::uint64_t> RecordEffectCompleted(
      const RequestOwnerEpoch& active_owner_epoch, const CCLCEffectAttemptDTO& completed_attempt,
      const ComponentLifecycleStatusDTO& successor_status) override;

  [[nodiscard]] Result<std::uint64_t> RecordLifecycleStatus(
      const RequestOwnerEpoch& active_owner_epoch,
      const ComponentLifecycleStatusDTO& status) override;

  [[nodiscard]] Result<std::optional<ComponentLifecycleStatusDTO>> LoadRetainedLifecycleStatus(
      const LifecycleTransactionId& lifecycle_transaction_id) override;

  [[nodiscard]] Result<std::vector<ComponentLifecycleStatusDTO>>
  LoadRetainedComponentLifecycleHistory(const ControllableComponentId&) override;

  [[nodiscard]] Result<ComponentLifecycleCurrentFactReadbackDTO> LoadCurrentComponentLifecycleFact(
      const ControllableComponentId& controllable_component_id) override;

  [[nodiscard]] Result<CCLCCurrentFactsReadbackDTO> LoadCurrentSnapshotFacts(
      const std::vector<ControllableComponentId>& canonical_controllable_component_ids,
      const std::vector<ControlDomainId>& canonical_control_domain_ids) override;

  [[nodiscard]] Result<std::optional<CCLCRouteSelectionRequestResultDTO>>
  InspectRouteSelectionRequest(const RequestOwnerEpoch& active_owner_epoch,
                               const RouteSelectionIntentDTO& intent) override;

  [[nodiscard]] Result<std::optional<CCLCRouteSelectionRequestResultDTO>>
  ReadRouteSelectionRequestResult(const RouteSelectionIntentDTO& intent) override;

  [[nodiscard]] Result<CCLCRouteSelectionRequestResultDTO> RecordRouteSelectionRejection(
      const RequestOwnerEpoch& owner, const RouteSelectionIntentDTO& intent, const ErrorDTO& error,
      const std::optional<CellAdmissionClaimIntentDTO>& unacquired) override;

  [[nodiscard]] Result<CCLCRouteSelectionRequestResultDTO> AcceptOrReplayRouteSelection(
      const RequestOwnerEpoch& active_owner_epoch, const RouteSelectionIntentDTO& intent,
      const RouteSelectionAcceptedDTO& candidate_acceptance,
      const RouteSelectionStatusDTO& candidate_initial_status) override;

  [[nodiscard]] Result<std::uint64_t> RecordRouteEffectPrepared(
      const RequestOwnerEpoch& active_owner_epoch,
      const CCLCRouteEffectAttemptDTO& prepared_attempt,
      const RouteSelectionStatusDTO& successor_status) override;

  [[nodiscard]] Result<std::uint64_t> RecordRouteEffectCompleted(
      const RequestOwnerEpoch& active_owner_epoch,
      const CCLCRouteEffectAttemptDTO& completed_attempt,
      const RouteSelectionStatusDTO& successor_status) override;

  [[nodiscard]] Result<std::uint64_t> RecordRouteEffectCompletedAndClaimRelease(
      const RequestOwnerEpoch& owner, const CCLCRouteEffectAttemptDTO& completed,
      const RouteSelectionStatusDTO& status, const CellAdmissionClaimReleaseDTO& release) override;

  [[nodiscard]] Result<std::uint64_t> RecordRouteTerminalAndClaimRelease(
      const RequestOwnerEpoch& owner, const RouteSelectionStatusDTO& status,
      const CellAdmissionClaimReleaseDTO& release) override;

  [[nodiscard]] Result<std::uint64_t> RecordRouteSelectionStatus(
      const RequestOwnerEpoch& active_owner_epoch, const RouteSelectionStatusDTO& status) override;

  [[nodiscard]] Result<std::optional<RouteSelectionStatusDTO>> LoadRetainedRouteSelectionStatus(
      const RouteSelectionTransactionId& route_selection_transaction_id) override;

  [[nodiscard]] Result<std::optional<ActiveControlConfigurationDTO>>
  LoadCurrentActiveControlConfiguration(const ControlDomainId& control_domain_id) override;

  /// Record (or, for the same Apply and candidate, return) the plan before
  /// any candidate route is published. Another candidate for the same Apply
  /// or unsettled CCLC work fails closed. Retains only the latest plan.
  [[nodiscard]] Result<NativeApplyPlanDTO> PlanNativeApply(
      const NativeApplyPlanRequestDTO& request);

  [[nodiscard]] Result<std::optional<NativeApplyPlanDTO>> LoadNativeApplyPlan(
      const NamedApplyPublicationTransactionDTO& apply);

  /// Components whose latest lifecycle transaction is not terminal. Restart
  /// reconstructs command eligibility only for components absent from this
  /// set: interrupted work stays command-closed until CCLC settles it.
  [[nodiscard]] Result<std::vector<ControllableComponentId>>
  LoadComponentsWithUnsettledLifecycle();

  /// The caller must have verified the exact held parent Apply fence and each
  /// runtime route readback. The facts must equal the recorded plan.
  /// Repetition of the same promotion is read-only; another candidate or
  /// unsettled CCLC transaction fails closed.
  /// The same transaction sets each changed component's requested target:
  /// `disabled` when the plan keeps an explicit Disable, otherwise the startup
  /// target `active`; retained components keep theirs and components leaving
  /// the adopted set have theirs cancelled.
  [[nodiscard]] Result<void> AdoptNativeApplyFacts(
      const NativeApplyFactAdoptionDTO& adoption);

  [[nodiscard]] Result<bool> HasNativeApplyFacts(
      const ActiveBundlePublicationReceiptDTO& publication);

  /// Requested lifecycle targets (session-scoped). Adoption of an Apply's
  /// facts initializes or cancels them atomically; see AdoptNativeApplyFacts.
  [[nodiscard]] Result<ComponentLifecycleTargetDTO> SetLifecycleTarget(
      const ControllableComponentId& component, LifecycleTargetStateDTO requested_state,
      LifecycleTargetOriginDTO origin) override;
  [[nodiscard]] Result<std::vector<ComponentLifecycleTargetDTO>> LoadLifecycleTargets() override;
  [[nodiscard]] Result<std::optional<ComponentLifecycleTargetRetentionDTO>>
  ReadLifecycleTargetRetention(const ControllableComponentId&) override;
  [[nodiscard]] Result<std::vector<ComponentLifecycleTargetDTO>>
  LoadLifecycleTargetsForClose(const RequestOwnerEpoch&) override;
  [[nodiscard]] Result<std::optional<CCLCLifecycleTargetRetirementResultDTO>>
  ReadLifecycleTargetRetirementResult(const CCLCLifecycleTargetRetirementIntentDTO&) override;
  [[nodiscard]] Result<CCLCLifecycleTargetRetirementResultDTO>
  RetireLifecycleTargetsForClose(const RequestOwnerEpoch&,
      const CCLCLifecycleTargetRetirementIntentDTO&, const ICellCloseAdmissionLease&) override;

  [[nodiscard]] Result<std::optional<ComponentLifecycleTargetDTO>> LoadLifecycleTarget(
      const ControllableComponentId& component) override;
  [[nodiscard]] Result<ComponentLifecycleTargetFactsDTO> LoadLifecycleTargetFacts(
      const ControllableComponentId& component) override;
  [[nodiscard]] Result<bool> RecordInterventionCause(
      const ControllableComponentId& component, const ControlDomainId& domain,
      std::optional<LifecycleBlockingCauseClassDTO> cause_class, std::int64_t observed_at_ns,
      std::uint64_t recovery_generation) override;
  [[nodiscard]] Result<std::vector<ComponentLifecycleInterventionCauseDTO>>
  LoadInterventionCauses(const ControllableComponentId& component);
  [[nodiscard]] Result<bool> RecordLifecycleTargetReached(
      const ControllableComponentId& component, bool reached,
      std::uint64_t recovery_generation) override;
  /// True only for a component that an unadopted native Apply plan installs,
  /// replaces or removes; a retained-unchanged component is false.
  [[nodiscard]] Result<bool> RecordLifecycleTargetReached(
      const ComponentLifecycleTargetExpectationDTO&, bool, std::uint64_t) override;
  [[nodiscard]] Result<std::vector<ComponentLifecycleTargetDTO>>
  RetireLifecycleTargetsForClose(const RequestOwnerEpoch&) override;
  [[nodiscard]] Result<bool> ComponentInUnadoptedApplyPlan(
      const ControllableComponentId& component) override;
  /// Component incidents and their bounded durable history events
  /// (addendum 2, 2026-10-08); session-scoped like the targets.
  [[nodiscard]] Result<ComponentIncidentRecordDTO> OpenComponentIncident(
      const ComponentIncidentRecordDTO& record, std::uint64_t recovery_generation) override;
  [[nodiscard]] Result<std::optional<ComponentIncidentRecordDTO>> LoadOpenComponentIncident(
      const ControllableComponentId& component) override;
  [[nodiscard]] Result<bool> HasOpenIncident() override;
  [[nodiscard]] Result<bool> RecordComponentIncidentContainment(
      const std::string& incident_id, ComponentIncidentContainmentStateDTO state,
      const std::string& record_json, std::int64_t occurred_at_ns) override;
  [[nodiscard]] Result<std::optional<CCLCLifecycleRequestResultDTO>> RecoverComponentIncident(
      const RequestOwnerEpoch& active_owner_epoch, const ComponentLifecycleIntentDTO& intent,
      const std::string& acknowledged_by, std::int64_t acknowledged_at_ns) override;
  [[nodiscard]] Result<bool> UpdateOpenComponentIncident(
      const std::string& incident_id, LifecycleBlockingCauseClassDTO cause_class,
      const std::vector<std::string>& vendor_codes, const std::string& record_json) override;
  [[nodiscard]] Result<bool> AppendComponentIncidentDiagnostic(
      const std::string& incident_id, std::int64_t occurred_at_ns, const ErrorDTO& diagnostic,
      std::shared_ptr<const content_identity::CanonicalValue> snapshot = {}) override;
  [[nodiscard]] Result<std::vector<ComponentIncidentEventDTO>> LoadComponentIncidentEvents(
      std::uint64_t after_sequence, std::size_t limit) override;

  void Close();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

/// Verify one exact lifecycle-owner settlement without initializing schema,
/// acquiring a write transaction, checkpointing, or mutating the journal.
[[nodiscard]] Result<void> ValidateCCLCCellAdmissionSettlementReadOnly(
    const std::filesystem::path& database_path, const CellId& cell_id,
    const CellAdmissionClaimDTO& claim, const CellAdmissionClaimReleaseDTO& release);

/// Authorize only an exact active CCLC owner epoch and quiescent durable
/// lifecycle claim settlements. The reader never performs recovery or repair.
[[nodiscard]] Result<CellAdmissionReconcileValidationDTO>
ValidateCCLCCellAdmissionReconciliationReadOnly(
    const std::filesystem::path& database_path, const CellId& cell_id,
    const std::vector<CellAdmissionClaimDTO>& held_claims,
    const std::vector<CellAdmissionCurrentOwnerEpochDTO>& current_owner_epochs,
    const CellAdmissionReconcileDTO& reconcile);

}  // namespace manyforge::core
