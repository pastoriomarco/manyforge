#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <manyforge_core/ports/icclc_journal.hpp>
#include <memory>
#include <string_view>

namespace manyforge::core {

inline constexpr std::string_view kCCLCJournalSchemaId = "manyforge.internal.cclc.journal.v5";
inline constexpr std::int64_t kCCLCJournalSchemaVersion = 5;
inline constexpr std::int64_t kCCLCJournalSqliteApplicationId = 0x4D464C4A;
inline constexpr std::string_view kCCLCJournalFilename = "cclc-journal.sqlite3";
inline constexpr std::size_t kDefaultCCLCExactResultHistory = 128U;
inline constexpr std::size_t kMaximumCCLCExactResultHistory = 128U;
inline constexpr std::size_t kDefaultCCLCTerminalResultBytes = 512U * 1024U * 1024U;
inline constexpr std::size_t kMaximumCCLCTerminalResultBytes = 512U * 1024U * 1024U;

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

  [[nodiscard]] Result<CCLCJournalRecoveryDTO> BeginOwnerEpoch(
      const RequestOwnerEpoch& owner_epoch, std::int64_t started_at_ns,
      std::size_t request_membership_capacity) override;

  [[nodiscard]] Result<std::optional<CCLCLifecycleRequestResultDTO>> InspectLifecycleRequest(
      const RequestOwnerEpoch& active_owner_epoch,
      const ComponentLifecycleIntentDTO& intent) override;

  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> AcceptOrReplayLifecycle(
      const RequestOwnerEpoch& active_owner_epoch, const ComponentLifecycleIntentDTO& intent,
      const ComponentLifecycleAcceptedDTO& candidate_acceptance,
      const ComponentLifecycleStatusDTO& candidate_initial_status) override;

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

  [[nodiscard]] Result<ComponentLifecycleCurrentFactReadbackDTO> LoadCurrentComponentLifecycleFact(
      const ControllableComponentId& controllable_component_id) override;

  [[nodiscard]] Result<CCLCCurrentFactsReadbackDTO> LoadCurrentSnapshotFacts(
      const std::vector<ControllableComponentId>& canonical_controllable_component_ids,
      const std::vector<ControlDomainId>& canonical_control_domain_ids) override;

  [[nodiscard]] Result<std::optional<CCLCRouteSelectionRequestResultDTO>>
  InspectRouteSelectionRequest(const RequestOwnerEpoch& active_owner_epoch,
                               const RouteSelectionIntentDTO& intent) override;

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

  [[nodiscard]] Result<std::uint64_t> RecordRouteSelectionStatus(
      const RequestOwnerEpoch& active_owner_epoch, const RouteSelectionStatusDTO& status) override;

  [[nodiscard]] Result<std::optional<RouteSelectionStatusDTO>> LoadRetainedRouteSelectionStatus(
      const RouteSelectionTransactionId& route_selection_transaction_id) override;

  [[nodiscard]] Result<std::optional<ActiveControlConfigurationDTO>>
  LoadCurrentActiveControlConfiguration(const ControlDomainId& control_domain_id) override;

  void Close();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace manyforge::core
