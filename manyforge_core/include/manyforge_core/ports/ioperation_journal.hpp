#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/journal_record.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/stop_state.hpp>
#include <manyforge_core/types/application_identity.hpp>
#include <manyforge_core/types/cell_admission.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/io_rule_state.hpp>
#include <manyforge_core/types/operation_runtime.hpp>
#include <manyforge_core/types/prepared_operation_diagnostics.hpp>
#include <manyforge_core/types/tcp_entry.hpp>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace manyforge::core {

/// Record of the resource-addressed Execution Kernel's actions.
///
/// Every action takes effect in memory first; the Kernel's journal writer
/// thread hands the records here afterwards, in action order. Nothing reads
/// the journal back to learn current state.
class IOperationJournal {
 public:
  virtual ~IOperationJournal() = default;

  /// Opens this start's journal: stores the application-start identity once
  /// in its metadata (build spec 6a). The journal is fresh at every
  /// application start; nothing of an earlier start is read.
  [[nodiscard]] virtual Result<void> BeginStart(ApplicationStartId application_start_id,
                                                std::int64_t started_at_ns,
                                                std::size_t request_capacity) = 0;

  /// Writes `records` in their order (records marked `skipped` are not
  /// written). Called only by the Kernel's journal writer thread.
  /// `records_dropped` counts records the writer's queue could not take since
  /// the previous batch. A record that fails is reported and is not written;
  /// the others are.
  [[nodiscard]] virtual JournalBatchResult WriteBatch(std::span<const JournalRecord> records,
                                                      std::uint64_t records_dropped) = 0;
};

}  // namespace manyforge::core
