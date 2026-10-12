#pragma once

#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>
#include <optional>

namespace manyforge::core {

/// `main`'s `complete_move_on_signal_timeout_ns` default (500 ms): the
/// completion tolerance added to a controlled Stop's deceleration time.
inline constexpr Duration kCompleteMoveOnSignalTimeoutNs = 500'000'000;

/// Q28/Q51 (as `main`'s `effective_controlled_stop_deadline_ns`): the one
/// deadline of a robot's controlled Stop, measured from the Stop request and
/// computed at Stop time from its own profile: `max_stop_time_s` when set,
/// else the deceleration time (or its default) plus the completion
/// tolerance. There is no shared cell-wide Stop budget.
[[nodiscard]] Duration controlled_stop_deadline_ns(
    const MotionInterruptStopProfileDTO& profile,
    Duration completion_tolerance_ns = kCompleteMoveOnSignalTimeoutNs);

/// `main`'s profile check of `max_stop_time_s`: absent, or finite, positive
/// and representable in nanoseconds.
[[nodiscard]] bool max_stop_time_valid(const std::optional<double>& max_stop_time_s) noexcept;

/// Authored full Soft/Spring duration, rounded to the nearest nanosecond
/// (half up). Draft, Apply and Prepare share this conversion so decimal input
/// does not spuriously add a nanosecond at an otherwise exact boundary.
[[nodiscard]] Result<std::int64_t> authored_stop_duration_ns(double seconds);

}  // namespace manyforge::core
