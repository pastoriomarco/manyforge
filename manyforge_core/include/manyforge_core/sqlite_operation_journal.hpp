#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <manyforge_core/ports/ioperation_journal.hpp>
#include <memory>
#include <optional>
#include <string>
#include <manyforge_core/types/application_identity.hpp>
#include <string_view>
#include <vector>

namespace manyforge::core {

/// v23: `journal_metadata.records_lost` (0 or 1) is 1 once a record of this
/// start was not written (dropped by the full queue, a failed write, or a
/// reused Stop ring slot); no cell-admission or claim generation columns.
/// v22: no `operation_participants`, `operation_resources` or
/// `operation_error_details` tables (the operation state is in the anchored
/// record's payload); the Stop event index is not unique. No cell-admission
/// claim tables: a claim is two `journal_records` rows without operation,
/// `cell_claim_acquired` and `cell_claim_released`, keyed by the payload's
/// `claim_generation`. No unique request-key index.
/// v21 (bulk change 1, one bump): the application-start identity `A` once in
/// the metadata (no owner_epochs table, no owner_epoch columns), 16-byte
/// request keys, integer operation numbers, participant ordinals, no
/// operation disposition, no effect_pending, no request tombstones, no
/// takeover or reconcile records. Stops (stop state model): one
/// `stop_event_accepted` and one `stop_event_completed` row per Stop event
/// (payload `manyforge.stop_event.v1`, keyed by `stop_event_id`), no
/// `stop_projections` table and no Stop recovery projection; the rows of
/// finished events are compacted to the newest 64. Operation snapshots name
/// their Stop by `stop_event_id`. Older journals are refused, never converted.
/// v20: operation intents use direct component references.
/// v19: the motion takeover decision carries its planning cap.
/// v18: lean Stop record without fault latch or surveillance. `stop_projections`
/// is an index of journal_records sequences; each Stop payload is stored once,
/// in journal_records.
inline constexpr std::string_view kOperationJournalSchemaId =
    "manyforge.internal.execution.operation-journal.o1.5.v23";
inline constexpr std::int64_t kOperationJournalSchemaVersion = 23;
inline constexpr std::int64_t kOperationJournalSqliteApplicationId = 0x4D46454A;
inline constexpr std::string_view kOperationJournalFilename = "operation-journal.sqlite3";
inline constexpr std::size_t kMaximumOperationJournalHistory = 4096U;
inline constexpr std::size_t kMaximumOperationJournalEventPage = 32U;

/// One immutable committed operation record projected to a bounded observer.
/// The canonical payload remains opaque so this persistence adapter does not
/// acquire a deployment-codec dependency.
struct OperationJournalEventDTO final {
  std::uint64_t sequence;
  std::string record_kind;
  std::optional<RequestKey> request_key;
  std::optional<OperationNumber> operation_id;
  std::optional<OperationRevision> operation_revision;
  std::int64_t occurred_at_ns;
  std::string occurred_at_wall;
  std::string payload_json;
  std::string payload_digest;
};

struct OperationJournalEventPageDTO final {
  std::vector<OperationJournalEventDTO> events;
  std::uint64_t next_after_sequence;
  bool has_more;
};

/// Planner- and transport-neutral durable store for the Kernel operation
/// journal. It is a separate target from manyforge_core so DTO-only and pure
/// algorithm consumers do not acquire a SQLite dependency.
class SqliteOperationJournal final : public IOperationJournal {
 public:
  /// `wall_time_text` (tests) gives a row's wall time from the action's
  /// steady-clock time; by default it is derived from the system clock.
  /// `critical_section_observer` (tests) is called inside an event read's
  /// snapshot, once its first row has been read. It runs under the reader
  /// lock and must not call back into this journal on its own thread.
  SqliteOperationJournal(std::filesystem::path database_path, CellId cell_id,
                         std::size_t history_capacity = kMaximumOperationJournalHistory,
                         std::function<std::string(std::int64_t)> wall_time_text = {},
                         std::function<void(std::string_view)> critical_section_observer = {});
  ~SqliteOperationJournal() override;

  SqliteOperationJournal(const SqliteOperationJournal&) = delete;
  SqliteOperationJournal& operator=(const SqliteOperationJournal&) = delete;

  [[nodiscard]] Result<void> BeginStart(ApplicationStartId application_start_id,
                                        std::int64_t started_at_ns,
                                        std::size_t request_capacity) override;

  [[nodiscard]] JournalBatchResult WriteBatch(std::span<const JournalRecord> records,
                                              std::uint64_t records_dropped) override;

  /// Read the bounded operational projection consumed by the headless event
  /// pump (operation, control, Stop event, rule-state and Active TCP rows).
  /// Owner lifecycle records remain durable but are not on this page.
  /// Read-only and replay-safe: reading never acknowledges, deletes, or
  /// changes operation ownership.
  [[nodiscard]] Result<OperationJournalEventPageDTO> ReadOperationalEvents(
      std::uint64_t after_sequence,
      std::size_t limit = kMaximumOperationJournalEventPage);

  void Close();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace manyforge::core
