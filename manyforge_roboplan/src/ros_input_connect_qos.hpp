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
#include <rclcpp/node.hpp>
#include <rclcpp/qos.hpp>
#include <rosidl_runtime_cpp/traits.hpp>

namespace manyforge::roboplan_backend::detail {
/// One connection-time discovery query. Request retained data only when every
/// currently discovered publisher offers it. An empty/volatile set remains
/// compatible with volatile publishers and waits for a real first message.
/// Discovery races/late QoS changes are not continuously renegotiated; actual
/// callback receipt alone is a sample, never graph presence or a heartbeat.
template <class InputMessage>
inline rclcpp::QoS InputConnectQos(rclcpp::Node &node, const std::string &topic,
                                   const rclcpp::QoS &configured) {
  auto qos = configured;
  qos.durability_volatile();
  auto publishers = node.get_publishers_info_by_topic(
      node.get_node_topics_interface()->resolve_topic_name(topic));
  // Other ROS types may share the graph name but cannot match this reader.
  // Only actual endpoints of the selected source type inform its QoS.
  std::erase_if(publishers, [](const auto &info) {
    return info.topic_type() != rosidl_generator_traits::name<InputMessage>();
  });
  if (!publishers.empty() &&
      std::all_of(publishers.begin(), publishers.end(), [](const auto &info) {
        return info.qos_profile().durability() ==
               rclcpp::DurabilityPolicy::TransientLocal;
      }))
    qos.transient_local();
  // A reliable reader cannot match a known best-effort writer. Preserve the
  // configured choice when discovery is empty; no future-endpoint guarantee.
  if (std::any_of(publishers.begin(), publishers.end(), [](const auto &info) {
        return info.qos_profile().reliability() ==
               rclcpp::ReliabilityPolicy::BestEffort;
      }))
    qos.best_effort();
  return qos;
}
} // namespace manyforge::roboplan_backend::detail
