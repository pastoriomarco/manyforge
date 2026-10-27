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

#include <manyforge_core/component_stream_health.hpp>
#include <memory>
#include <rclcpp/node.hpp>

namespace manyforge::roboplan_backend {

/// CP9 host end of the Python `ComponentStreamSink` transport: one
/// subscription to `/manyforge/component_stream_samples`
/// (`diagnostic_msgs/msg/DiagnosticStatus`, reliable, depth 64). Each sample
/// (level OK, value `stamp_ns`, `0` = arrival) is recorded into the stream the
/// host-side adapter declared; each fault event (level ERROR, `message` =
/// code, value `detail`) is reported on it. Malformed messages and undeclared
/// streams are ignored.
///
/// The returned handle keeps the subscription alive; nullptr when `node` or
/// `health` is null. Subscription failures throw as rclcpp does.
[[nodiscard]] std::shared_ptr<void>
InstallComponentStreamSampleBridge(const std::shared_ptr<rclcpp::Node> &node,
                                   std::shared_ptr<core::ComponentStreamHealth> health);

} // namespace manyforge::roboplan_backend
