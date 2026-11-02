#pragma once

#include <manyforge_core/ports/iexecution_port.hpp>
#include <manyforge_core/types/coordinate_state.hpp>
#include <manyforge_roboplan/retained_observed_state_channel.hpp>
#include <manyforge_roboplan/controller_launch_artifacts.hpp>
#include <chrono>
#include <functional>
#include <stop_token>
#include <algorithm>
#include <cmath>
#include <span>

namespace manyforge::roboplan_backend::host_detail {

inline bool ExactMockResetMeasuredTarget(const core::ObservedStateSnapshotDTO& state,
                                        std::span<const double> target,
                                        std::uint64_t after_sequence,
                                        std::int64_t after_source_stamp,
                                        std::int64_t published_at_ns) {
  if (!state.complete || state.positions.size() != target.size() ||
      state.sequence <= after_sequence || state.source_timestamp_ns <= after_source_stamp ||
      state.received_timestamp_ns <= published_at_ns)
    return false;
  for (std::size_t index = 0; index < target.size(); ++index) {
    const auto* position = std::get_if<core::AngularPositionDTO>(&state.positions[index]);
    if (!position || !std::isfinite(target[index]) || !std::isfinite(position->value_rad) ||
        position->value_rad != target[index])
      return false;
  }
  return true;
}

// Isaac (RP-1b): one hardware command sample received after the writer's
// publication, newer than the pre-publication one, carrying exactly the
// target acknowledges the write. Like Mock's loopback it proves the command
// interface holds the target, not that the robot is there.
inline bool ExactResetCommandEcho(const ResetCommandEcho& echo,
                                  std::span<const double> target,
                                  std::uint64_t after_sequence,
                                  std::int64_t published_at_ns) {
  return echo.sequence > after_sequence && echo.received_ns > published_at_ns &&
         std::equal(echo.positions.begin(), echo.positions.end(), target.begin(), target.end());
}

// Wait only on existing source publications. The deadline is the approved
// Reset-only switch/consumption budget; freshness remains the source policy.
template <typename ClockNow = decltype(&std::chrono::steady_clock::now)>
inline core::Result<void> WaitForMockResetObservation(
    RetainedObservationWake& wake, std::stop_token stop_token,
    std::chrono::steady_clock::time_point deadline,
    const std::function<core::Result<bool>()>& observe,
    const std::function<core::Result<void>()>& validate_scope,
    ClockNow now = &std::chrono::steady_clock::now) {
  std::stop_callback cancel_wake(stop_token, [&wake] { wake.notify(); });
  for (;;) {
    const auto generation = wake.generation();
    if (stop_token.stop_requested())
      return core::Result<void>::Err({"program_reset_position_cancelled",
          "Position reset was interrupted", {{"reason", "reset_position_cancelled"}}});
    auto valid = validate_scope();
    if (!valid.ok()) return valid;
    auto observed = observe();
    if (!observed.ok()) return core::Result<void>::Err(observed.error());
    // Receipt freshness cannot extend the operation's fault bound. Even a
    // source read/wake processed late must not turn an expired wait into Ok.
    if (stop_token.stop_requested())
      return core::Result<void>::Err({"program_reset_position_cancelled",
          "Position reset was interrupted", {{"reason", "reset_position_cancelled"}}});
    const auto remaining = deadline - now();
    if (remaining <= std::chrono::steady_clock::duration::zero())
      return core::Result<void>::Err({"program_reset_position_timeout",
          "Position reset did not receive its required acknowledgment",
          {{"reason", "reset_position_acknowledgment_timeout"}}});
    if (observed.value()) return core::Result<void>::Ok();
    static_cast<void>(wake.wait_after(generation,
        std::chrono::duration_cast<std::chrono::nanoseconds>(remaining)));
  }
}

}  // namespace manyforge::roboplan_backend::host_detail
