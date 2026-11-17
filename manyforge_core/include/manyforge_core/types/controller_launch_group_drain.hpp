#pragma once

#include <chrono>
#include <string>

namespace manyforge::core {

/// The host's stop grace of a component's controller launch group: the
/// launch leader gets SIGINT, and the group SIGKILL once this has passed
/// (ControllerLaunchSupervisor). Retiring a hung component therefore takes
/// this, then the kill and reap.
inline constexpr std::chrono::milliseconds kControllerLaunchStopGrace{10'000};

enum class ControllerLaunchCleanupState {
  kNotRequested,
  kPending,
  kUnknown,
  kDrained
};

/// Process-group proof from the original live host, never physical robot state.
/// The no-live-member scan precedes release of the exact zombie leader pin.
struct ControllerLaunchGroupDrainReceipt final {
  int leader_pid;
  std::string controllable_component_id;
  std::string launch_identity;
  std::string observed_utc;

  friend bool operator==(const ControllerLaunchGroupDrainReceipt &,
                         const ControllerLaunchGroupDrainReceipt &) = default;
};

} // namespace manyforge::core
