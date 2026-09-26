#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <manyforge_core/ports/icclc_journal.hpp>
#include <manyforge_core/ports/ioperation_journal.hpp>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace manyforge::core {

inline constexpr std::string_view kCCLCJournalSchemaId = "manyforge.internal.cclc.journal.v5";
inline constexpr std::int64_t kCCLCJournalSchemaVersion = 12;
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
};

struct NativeApplyRouteGenerationDTO final {
  ControlDomainId control_domain_id;
  ActiveControlConfigurationGeneration generation{0U};
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
};

/// CCLC's recorded decision for one native Apply: which components keep an
/// explicit Disable (their gate stays closed) and the generations the
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
};

/// Private promotion of the planned facts after the matching Manager Apply
/// has published its candidate, but before its fence is released. Accepted
/// only when it matches the recorded plan exactly. This is not a public
/// lifecycle request or permission to perform effects.
struct NativeApplyFactAdoptionDTO final {
  ActiveBundlePublicationReceiptDTO publication;
  std::vector<NativeApplyComponentFactDTO> components;
  std::vector<ActiveControlConfigurationDTO> current_routes;
};

/// SQLite-backed CCLC lifecycle journal.
///
/// The adapter is deliberately separate from the pure Core target. It uses a
/// closed binary DTO codec and a defensive fixed-schema database; it does not
/// acquire lifecycle semantics or call any external primitive.
class SqliteCCLCJournal final : public ICCLCJournal {
 public:
  SqliteCCLCJournal(std::filesystem::path database_path, CellId cell_id,
                    std::size_t exact_result_history_capacity = kDefaultCCLCExactResultHistory,
                    std::size_t terminal_result_bytes_capacity = kDefaultCCLCTerminalResultBytes);
  ~SqliteCCLCJournal() override;

  SqliteCCLCJournal(const SqliteCCLCJournal&) = delete;
  SqliteCCLCJournal& operator=(const SqliteCCLCJournal&) = delete;

  [[nodiscard]] Result<std::optional<NamedApplyPublicationTransactionDTO>>
  ReadUnsettledNativeApplyPlan() override;

  [[nodiscard]] Result<CCLCJournalRecoveryDTO> BeginOwnerEpoch(
      const RequestOwnerEpoch& owner_epoch, std::int64_t started_at_ns,
      std::size_t request_membership_capacity,
      const std::optional<RequestOwnerEpoch>& quiescent_predecessor = std::nullopt) override;

  [[nodiscard]] Result<std::optional<CCLCLifecycleRequestResultDTO>> InspectLifecycleRequest(
      const RequestOwnerEpoch& active_owner_epoch,
      const ComponentLifecycleIntentDTO& intent) override;

  [[nodiscard]] Result<std::optional<CCLCLifecycleRequestResultDTO>> ReadLifecycleRequestResult(
      const ComponentLifecycleIntentDTO& intent) override;

  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> RecordLifecycleRejection(
      const RequestOwnerEpoch&, const ComponentLifecycleIntentDTO&, const ErrorDTO&,
      const std::optional<CellAdmissionClaimIntentDTO>&) override;

  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> AcceptOrReplayLifecycle(
      const RequestOwnerEpoch& active_owner_epoch, const ComponentLifecycleIntentDTO& intent,
      const ComponentLifecycleAcceptedDTO& candidate_acceptance,
      const ComponentLifecycleStatusDTO& candidate_initial_status) override;
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
  [[nodiscard]] Result<std::uint64_t>
  RecordEffectCompletedAndLifecycleClaimRelease(
      const RequestOwnerEpoch&, const CCLCEffectAttemptDTO&,
      const ComponentLifecycleStatusDTO&,
      const CellAdmissionClaimReleaseDTO&) override;

  [[nodiscard]] Result<std::uint64_t> RecordLifecycleStatus(
      const RequestOwnerEpoch& active_owner_epoch,
      const ComponentLifecycleStatusDTO& status) override;

  [[nodiscard]] Result<std::optional<ComponentLifecycleStatusDTO>> LoadRetainedLifecycleStatus(
      const LifecycleTransactionId& lifecycle_transaction_id) override;

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
  [[nodiscard]] Result<void> AdoptNativeApplyFacts(
      const NativeApplyFactAdoptionDTO& adoption);

  [[nodiscard]] Result<bool> HasNativeApplyFacts(
      const ActiveBundlePublicationReceiptDTO& publication);

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
