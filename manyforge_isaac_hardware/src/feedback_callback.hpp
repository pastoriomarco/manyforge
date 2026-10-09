// Copyright 2026 Flexin Group SRL
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef FEEDBACK_CALLBACK_HPP_
#define FEEDBACK_CALLBACK_HPP_

#include <cstdint>

#include "sensor_msgs/msg/joint_state.hpp"
#include "rosgraph_msgs/msg/clock.hpp"
#include "transport.hpp"

namespace manyforge_isaac_hardware::detail
{

// Owned only by the same executor that consumes JointState. This single
// explicit /clock input distinguishes an observed zero from no clock message.
struct SimulationClockInput
{
  bool initialized{false};
  std::int64_t now_ns{0};

  void receive(
    FeedbackHandoff & handoff, const rosgraph_msgs::msg::Clock & message,
    std::int64_t received_ns) noexcept
  {
    const auto & stamp = message.clock;
    if (stamp.sec < 0 || stamp.nanosec >= 1000000000U) {
      handoff.invalidate_source_clock(received_ns);
      return;
    }
    const auto next_ns = static_cast<std::int64_t>(stamp.sec) * 1000000000LL + stamp.nanosec;
    if (initialized && next_ns < now_ns) {
      handoff.invalidate_source_clock(received_ns);
    }
    initialized = true;
    now_ns = next_ns;
  }
};

// Used by the actual ROS callback and deterministic tests. Keep the source
// header intact; the receiver's clock must not renew the physics sample.
inline void receive_joint_state(
  FeedbackHandoff & handoff, const sensor_msgs::msg::JointState & message,
  std::int64_t source_now_ns, std::int64_t received_ns,
  bool source_clock_initialized = true) noexcept
{
  const auto & stamp = message.header.stamp;
  const std::int64_t source_ns = stamp.sec < 0 || stamp.nanosec >= 1000000000U ? -1 :
    static_cast<std::int64_t>(stamp.sec) * 1000000000LL + stamp.nanosec;
  handoff.receive(
    {message.name, message.position, message.velocity, source_ns}, source_now_ns, received_ns,
    source_clock_initialized);
}

}  // namespace manyforge_isaac_hardware::detail

#endif  // FEEDBACK_CALLBACK_HPP_
