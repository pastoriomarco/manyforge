#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/stop_state.hpp>
#include <manyforge_core/types/cell_admission.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/io_rule_state.hpp>
#include <manyforge_core/types/operation_runtime.hpp>
#include <manyforge_core/types/prepared_operation_diagnostics.hpp>
#include <manyforge_core/types/request_key.hpp>
#include <manyforge_core/types/runtime_scale.hpp>
#include <manyforge_core/types/tcp_entry.hpp>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace manyforge::core {

/// `operation_accepted`: the values the acceptance section made.
struct JournalOperationAccepted final {
  OperationIntentDTO intent;
  OperationAcceptedDTO accepted;
  OperationStateDTO state;
};

/// `control_accepted`.
struct JournalControlAccepted final {
  OperationControlRequestDTO request;
  OperationControlAcceptedDTO accepted;
  OperationStateDTO state;
};

/// `state_observed` or `operation_prepared` (the reason decides, as in the row).
struct JournalOperationState final {
  OperationStateDTO state;
  std::optional<RuntimeScaleStatusDTO> scale_status;
  const char* transition_reason{};
  std::vector<PreparedMotionDiagnosticsDTO> prepared_motion_diagnostics;
};

/// `runtime_scale_accepted`.
struct JournalRuntimeScaleAccepted final {
  RuntimeScaleRequestDTO request;
  RuntimeScaleAcceptedDTO accepted;
  OperationStateDTO state;
  RuntimeScaleStatusDTO scale_status;
};

/// `stop_event_accepted` or `stop_event_completed`. Posted as a reference into
/// the Stop event ring; the writer copies the ring record into `record` just
/// before the write.
struct JournalStopEventRow final {
  StopEventId event_id{0U};
  std::uint32_t ring_slot{};
  bool completed{};
  std::shared_ptr<const StopEventRecord> record{};
};

/// `cell_claim_acquired`: one cell-admission claim taking effect.
struct JournalCellClaimAcquired final {
  CellAdmissionClaimDTO claim;
};

/// `cell_claim_released`: one held claim released.
struct JournalCellClaimReleased final {
  CellAdmissionClaimDTO claim;
  CellAdmissionClaimReleaseDTO release;
  std::int64_t released_at_ns{};
};

/// `active_tcp_changed`: one confirmed Active TCP change, posted by the robot
/// owner after its own lock is released (`post_active_tcp_change`).
using JournalRecordPayload =
    std::variant<JournalOperationAccepted, JournalControlAccepted, JournalOperationState,
                 JournalRuntimeScaleAccepted, JournalStopEventRow, IoRuleStateChangeDTO, JournalCellClaimAcquired,
                 JournalCellClaimReleased, ActiveTcpChangeDTO>;

/// One record posted to the journal writer in the section that made the
/// action. Queue order is action order.
struct JournalRecord final {
  JournalRecordPayload payload;
  /// The write kind named by the journal write fault if this record fails.
  const char* write_kind{};
  std::optional<RequestKey> fault_request_key{};
  /// Stop-lane record (Stop rows, Stop captures): a failure is logged with
  /// its row and does not set the journal write fault.
  bool stop_lane{};
  /// Not written: a later state of the same operation superseded it in the
  /// queue, or its Stop ring slot was overwritten before the copy.
  bool skipped{};
};

/// One record of a batch that was not written.
struct JournalRecordFailure final {
  std::size_t index{};
  ErrorDTO error;
};

struct JournalBatchResult final {
  std::vector<JournalRecordFailure> failures;
};

}  // namespace manyforge::core
