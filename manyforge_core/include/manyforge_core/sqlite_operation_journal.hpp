#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <manyforge_core/ports/ioperation_journal.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

inline constexpr std::string_view kOperationJournalSchemaId =
    "manyforge.internal.execution.operation-journal.n4.v2";
inline constexpr std::int64_t kOperationJournalSchemaVersion = 2;
inline constexpr std::int64_t kOperationJournalSqliteApplicationId = 0x4D46454A;
inline constexpr std::string_view kOperationJournalFilename = "operation-journal.sqlite3";
inline constexpr std::size_t kMaximumOperationJournalHistory = 4096U;
inline constexpr std::size_t kMaximumOperationJournalEventPage = 32U;

/// One immutable committed operation record projected to a bounded observer.
/// The canonical payload remains opaque so this persistence adapter does not
/// acquire a deployment-codec dependency.
struct OperationJournalEventDTO final {
  std::uint64_t sequence;
  RequestOwnerEpoch owner_epoch;
  std::string record_kind;
  std::optional<RequestId> request_id;
  OperationId operation_id;
  OperationRevision operation_revision;
  std::int64_t occurred_at_ns;
  std::string occurred_at_wall;
  bool effect_pending;
  std::string payload_json;
  std::string payload_digest;
};

struct OperationJournalEventPageDTO final {
  RequestOwnerEpoch owner_epoch;
  std::vector<OperationJournalEventDTO> events;
  std::uint64_t next_after_sequence;
  bool has_more;
};

/// Planner- and transport-neutral durable store for the Kernel operation
/// journal. It is a separate target from manyforge_core so DTO-only and pure
/// algorithm consumers do not acquire a SQLite dependency.
class SqliteOperationJournal final : public IOperationJournal {
 public:
  SqliteOperationJournal(std::filesystem::path database_path, CellId cell_id,
                         std::size_t history_capacity = kMaximumOperationJournalHistory,
                         std::function<std::string()> wall_time_text = {});
  ~SqliteOperationJournal() override;

  SqliteOperationJournal(const SqliteOperationJournal&) = delete;
  SqliteOperationJournal& operator=(const SqliteOperationJournal&) = delete;

  [[nodiscard]] Result<OperationJournalRecoveryDTO> BeginOwnerEpoch(
      const RequestOwnerEpoch& owner_epoch, std::int64_t started_at_ns,
      std::size_t request_capacity, const std::vector<ResourceId>& resource_inventory) override;

  [[nodiscard]] Result<std::uint64_t> RecordOperationAccepted(
      const RequestOwnerEpoch& owner_epoch, const OperationIntentDTO& intent,
      const OperationAcceptedDTO& accepted, const OperationStateDTO& state) override;

  [[nodiscard]] Result<std::uint64_t> RecordControlAccepted(
      const RequestOwnerEpoch& owner_epoch, const OperationControlRequestDTO& request,
      const OperationControlAcceptedDTO& accepted, const OperationStateDTO& state) override;

  [[nodiscard]] Result<std::uint64_t> RecordStopAccepted(const RequestOwnerEpoch& owner_epoch,
                                                         const StopIntentDTO& intent,
                                                         const StopAcceptedDTO& accepted,
                                                         const OperationStateDTO& state) override;

  [[nodiscard]] Result<std::uint64_t> RecordRuntimeScaleAccepted(
      const RequestOwnerEpoch& owner_epoch, const RuntimeScaleRequestDTO& request,
      const RuntimeScaleAcceptedDTO& accepted, const OperationStateDTO& state,
      const RuntimeScaleStatusDTO& scale_status) override;

  [[nodiscard]] Result<std::uint64_t> RecordOperationState(
      const RequestOwnerEpoch& owner_epoch, const OperationStateDTO& state,
      const RuntimeScaleStatusDTO* scale_status, const char* transition_reason) override;

  /// Read-only, replay-safe projection. Reading never acknowledges, deletes,
  /// or changes operation ownership.
  [[nodiscard]] Result<OperationJournalEventPageDTO> ReadEvents(
      const RequestOwnerEpoch& owner_epoch, std::uint64_t after_sequence,
      std::size_t limit = kMaximumOperationJournalEventPage);

  void Close();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace manyforge::core
