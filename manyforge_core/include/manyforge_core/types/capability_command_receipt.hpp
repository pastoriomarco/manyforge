#pragma once

#include <cstdint>
#include <atomic>
#include <limits>
#include <functional>
#include <memory>
#include <mutex>
#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <optional>
#include <string>

namespace manyforge::core {

// Process-local handoff retained by the exact prepared session and cancel item.
// Access fields only under mutex; invoke handoff only after releasing it.
enum class GripperLateCancelState { kIdle, kPending, kFailed, kSettled, kHandedOff };
struct GripperLateCancelSlot {
  std::shared_ptr<std::atomic<std::uint64_t>> failure_epoch{};
  std::atomic<bool> cancel_allowed{true};
  std::atomic<bool> cancel_queued{false};
  std::atomic<bool> shutdown_handoff_queued{false};
  std::atomic<bool> handoff_running{false};
  // Steady-clock nanoseconds; max means no lifecycle cutoff is installed.
  std::atomic<std::int64_t> lifecycle_deadline_ns{std::numeric_limits<std::int64_t>::max()};
  std::function<void()> state_changed{};
  std::mutex mutex;
  GripperLateCancelState state{GripperLateCancelState::kIdle};
  std::uint64_t preparation_id{};
  std::uint64_t command_id{};
  std::uint64_t generation{};
  std::string gripper_id;
  std::optional<CommandTargetId> target{};
  bool receipt_installed{};
  std::function<Result<void>(std::uint64_t, std::uint64_t, std::uint64_t,
                             const CommandTargetId&, const std::string&)> handoff{};
};

/// Receipt of the subordinate command-session submission. An accepted receipt
/// does not imply provider acknowledgement or physical output/readback.
struct CapabilityCommandDispatchReceiptDTO {
  bool accepted = false;
  std::uint64_t command_id = 0;
  std::uint64_t generation = 0;
  std::optional<ErrorDTO> error;
  std::shared_ptr<GripperLateCancelSlot> late_cancel_slot{};
};

}  // namespace manyforge::core
