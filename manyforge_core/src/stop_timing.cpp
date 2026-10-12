#include <algorithm>
#include <cmath>
#include <manyforge_core/stop_timing.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>
#include <limits>
#include <map>
#include <string>

namespace manyforge::core {
namespace {

ErrorDTO invalid(std::string reason, std::string message,
                 std::map<std::string, std::string> details = {}) {
  details.emplace("reason", std::move(reason));
  return {"invalid_request", std::move(message), std::move(details)};
}

}  // namespace

bool max_stop_time_valid(const std::optional<double>& max_stop_time_s) noexcept {
  return !max_stop_time_s.has_value() ||
         (std::isfinite(*max_stop_time_s) && *max_stop_time_s > 0.0 &&
          *max_stop_time_s * 1e9 < static_cast<double>(std::numeric_limits<Duration>::max()));
}

Duration controlled_stop_deadline_ns(const MotionInterruptStopProfileDTO& profile,
                                     const Duration completion_tolerance_ns) {
  if (profile.max_stop_time_s.has_value() && max_stop_time_valid(profile.max_stop_time_s)) {
    return static_cast<Duration>(*profile.max_stop_time_s * 1e9);  // as `main`
  }
  const auto deceleration_ns = static_cast<Duration>(
      motion_interrupt_deceleration_time_or_default(profile) * 1e9);
  return deceleration_ns + std::max<Duration>(0, completion_tolerance_ns);
}

Result<std::int64_t> authored_stop_duration_ns(const double seconds) {
  if (!std::isfinite(seconds) || seconds <= 0 || seconds > kMaxControlledStopDecelerationTimeS)
    return Result<std::int64_t>::Err(
        invalid("stop_profile_duration_out_of_range",
                "Controlled Stop duration is outside its qualified range"));
  const auto duration = std::llround(seconds * 1'000'000'000.0);
  if (duration <= 0)
    return Result<std::int64_t>::Err(
        invalid("stop_profile_duration_out_of_range",
                "Controlled Stop duration must round to at least one nanosecond"));
  return Result<std::int64_t>::Ok(duration);
}

}  // namespace manyforge::core
