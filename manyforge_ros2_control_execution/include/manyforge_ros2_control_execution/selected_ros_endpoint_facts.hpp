#pragma once

#include <manyforge_core/types/error_code.hpp>
#include <rclcpp/rclcpp.hpp>

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>

namespace manyforge::ros2_control_execution {

/// The publisher count of one lane-owned, exclusive ROS topic: an
/// action needs exactly one publisher. 0 is not established, 2+ is
/// `duplicate_component_instance`. Refresh (NRT, on the existing observation
/// and admission checkpoints) stores the graph's count; ReadError only reads
/// the stored count and the subscription's matched count. Nothing else on the
/// ROS domain is looked at.
class SelectedRosEndpointFacts final {
public:
  SelectedRosEndpointFacts(const rclcpp::Node::SharedPtr &node, std::string topic)
      : node_(node), topic_(std::move(topic)) {}

  void ObserveMatchedCount(const int count) {
    std::lock_guard lock(mutex_);
    matched_count_ = static_cast<std::size_t>(std::max(count, 0));
  }

  void Refresh() {
    std::size_t count = 0;
    std::optional<std::string> failure;
    if (const auto node = node_.lock()) {
      try {
        count = node->count_publishers(topic_);
      } catch (const std::exception &cause) {
        failure = cause.what();
      }
    }
    std::lock_guard lock(mutex_);
    publisher_count_ = count;
    read_failure_ = std::move(failure);
  }

  [[nodiscard]] std::optional<core::ErrorDTO> ReadError() const {
    std::lock_guard lock(mutex_);
    // The subscription's matched count is a second view of the same count.
    const auto count = std::max(publisher_count_, matched_count_);
    if (count > 1U) {
      return core::ErrorDTO{
          "duplicate_component_instance",
          "The selected ROS topic has more than one publisher.",
          {{"reason", "duplicate_component_instance"}, {"topic", topic_},
           {"endpoint_count", std::to_string(count)}}};
    }
    if (publisher_count_ == 1U)
      return std::nullopt;
    core::ErrorDTO error{"component_instance_graph_unsettled",
                         "The selected ROS publisher role is not established.",
                         {{"reason", "component_instance_graph_unsettled"},
                          {"topic", topic_},
                          {"endpoint_count", std::to_string(publisher_count_)}}};
    if (read_failure_)
      error.details.emplace("cause", *read_failure_);
    return error;
  }

private:
  std::weak_ptr<rclcpp::Node> node_;
  const std::string topic_;
  mutable std::mutex mutex_;
  std::size_t publisher_count_{};
  std::size_t matched_count_{};
  std::optional<std::string> read_failure_;
};

} // namespace manyforge::ros2_control_execution
