// Copyright 2026 Marco Pastorio
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

#include <manyforge_roboplan/component_stream_sample_bridge.hpp>

#include <charconv>
#include <cstdint>
#include <diagnostic_msgs/msg/diagnostic_status.hpp>
#include <optional>
#include <rclcpp/qos.hpp>
#include <rclcpp/subscription.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

using Status = diagnostic_msgs::msg::DiagnosticStatus;

constexpr char kSamplesTopic[] = "/manyforge/component_stream_samples";
constexpr std::size_t kSamplesDepth = 64U;

const std::string *FindValue(const Status &message, std::string_view key) {
  for (const auto &entry : message.values)
    if (entry.key == key)
      return &entry.value;
  return nullptr;
}

/// Decimal `stamp_ns` >= 0 consumed entirely; nullopt when malformed.
std::optional<std::int64_t> ParseStamp(const std::string &text) {
  std::int64_t value{};
  const char *const end = text.data() + text.size();
  const auto [last, error] = std::from_chars(text.data(), end, value);
  if (text.empty() || error != std::errc{} || last != end || value < 0)
    return std::nullopt;
  return value;
}

void Receive(core::ComponentStreamHealth &health, std::unique_ptr<Status> message) {
  if (message->hardware_id.empty() || message->name.empty())
    return;
  if (message->level == Status::OK) {
    const std::string *const text = FindValue(*message, "stamp_ns");
    if (text == nullptr)
      return;
    const std::optional<std::int64_t> stamp = ParseStamp(*text);
    if (!stamp)
      return;
    const core::ControllableComponentId component(std::move(message->hardware_id));
    (void)health.RecordDeclared(component, message->name,
                                *stamp == 0 ? std::optional<std::int64_t>{} : stamp);
    return;
  }
  if (message->level == Status::ERROR && !message->message.empty()) {
    const std::string *const detail = FindValue(*message, "detail");
    const core::ControllableComponentId component(std::move(message->hardware_id));
    (void)health.ReportDeclaredFault(component, message->name, message->message,
                                     detail == nullptr ? std::string_view{}
                                                       : std::string_view{*detail});
  }
}

} // namespace

std::shared_ptr<void>
InstallComponentStreamSampleBridge(const std::shared_ptr<rclcpp::Node> &node,
                                   std::shared_ptr<core::ComponentStreamHealth> health) {
  if (!node || !health)
    return nullptr;
  return node->create_subscription<Status>(
      kSamplesTopic, rclcpp::QoS(rclcpp::KeepLast(kSamplesDepth)).reliable(),
      [health = std::move(health)](std::unique_ptr<Status> message) {
        Receive(*health, std::move(message));
      });
}

} // namespace manyforge::roboplan_backend
