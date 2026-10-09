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

#pragma once

#include <algorithm>
#include <chrono>
#include <memory>
#include <rclcpp_action/rclcpp_action.hpp>

namespace manyforge::ros2_control_execution::detail {
inline double clamp_progress(double progress) {
  return std::max(0.0, std::min(1.0, progress));
}

template <typename ActionT>
bool wait_for_action_server_until(
    const std::shared_ptr<rclcpp_action::Client<ActionT>> &client,
    double timeout_s) {
  if (!client) {
    return false;
  }
  const auto budget =
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(std::max(0.0, timeout_s)));
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return client->action_server_is_ready();
    }
    // Graph discovery can wake a wait before every action endpoint is ready.
    // Retry bounded slices until the caller's single overall deadline.
    const auto remaining = deadline - now;
    const auto slice = std::min(
        remaining,
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::milliseconds(100)));
    if (client->wait_for_action_server(
            std::chrono::duration_cast<std::chrono::nanoseconds>(slice))) {
      return true;
    }
  }
}

} // namespace manyforge::ros2_control_execution::detail
