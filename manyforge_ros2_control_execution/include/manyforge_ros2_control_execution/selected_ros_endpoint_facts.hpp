#pragma once

#include <manyforge_core/types/error_code.hpp>
#include <rclcpp/rclcpp.hpp>

#include <algorithm>
#include <array>
#include <iomanip>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>

namespace manyforge::ros2_control_execution {

/// Observation of one lane-owned, exclusive ROS publisher role. Refresh is
/// NRT; ReadError only copies maintained facts and checks the SDK graph event.
/// A GID identifies a publisher endpoint, never an OS process/provider epoch.
class SelectedRosEndpointFacts final {
public:
  SelectedRosEndpointFacts(const rclcpp::Node::SharedPtr &node,
                          std::string topic, std::string type)
      : node_(node), topic_(std::move(topic)), type_(std::move(type)),
        graph_event_(node->get_graph_event()) {}

  void ObserveMatchedCount(const int count) {
    std::lock_guard lock(mutex_);
    if (!matched_count_ || *matched_count_ != count) {
      matched_count_ = count;
      ++revision_;
      dirty_ = true;
    }
  }

  /// Called only by existing NRT observation/subscription checkpoints. No
  /// wait, timer, worker, retry or effect is introduced.
  void Refresh() {
    // Multiple existing callbacks may observe the same graph edge. Serialize
    // only this NRT graph capture, never a lifecycle/command owner lock.
    std::lock_guard refresh_lock(refresh_mutex_);
    std::uint64_t revision;
    {
      std::lock_guard lock(mutex_);
      if (!dirty_ && !graph_event_->check())
        return;
      revision = revision_;
    }
    graph_event_->check_and_clear();
    std::optional<core::ErrorDTO> error;
    const auto node = node_.lock();
    if (!node) {
      error = Unsettled("selected_endpoint_node_unavailable");
    } else {
      try {
        const auto endpoints = node->get_publishers_info_by_topic(topic_);
        std::set<std::array<std::uint8_t, RMW_GID_STORAGE_SIZE>> gids;
        std::string first, second;
        bool identifiable = true;
        for (const auto &endpoint : endpoints) {
          if (endpoint.topic_type() != type_) {
            identifiable = false;
            continue;
          }
          const auto &gid = endpoint.endpoint_gid();
          if (std::all_of(gid.begin(), gid.end(),
                          [](const auto byte) { return byte == 0; })) {
            identifiable = false;
            continue;
          }
          if (!gids.insert(gid).second)
            continue;
          std::ostringstream text;
          text << endpoint.node_namespace() << '/' << endpoint.node_name()
               << '@' << std::hex << std::setfill('0');
          for (const auto byte : gid)
            text << std::setw(2) << static_cast<unsigned>(byte);
          if (first.empty()) first = text.str();
          else if (second.empty()) second = text.str();
        }
        if (gids.size() > 1U) {
          error = core::ErrorDTO{
              "duplicate_component_instance",
              "The selected ROS publisher role has conflicting endpoints.",
              {{"reason", "duplicate_component_instance"}, {"topic", topic_},
               {"endpoint_count", std::to_string(gids.size())},
               {"first_endpoint", std::move(first)},
               {"second_endpoint", std::move(second)},
               {"evidence_kind", "ros_publisher_endpoints"}}};
        } else if (!identifiable || gids.empty()) {
          error = Unsettled("selected_endpoint_not_established");
        }
      } catch (const std::exception &cause) {
        error = Unsettled("selected_endpoint_graph_read_failed");
        error->details.emplace("cause", cause.what());
      }
    }
    std::lock_guard lock(mutex_);
    if (revision != revision_ || graph_event_->check()) {
      dirty_ = true;
      // A positive conflicting endpoint observation is kept; an unstable
      // graph must never turn that observation into a clean result.
      if (!error || error->code != "duplicate_component_instance")
        error = Unsettled("selected_endpoint_graph_changed");
    } else {
      dirty_ = false;
    }
    error_ = std::move(error);
  }

  [[nodiscard]] std::optional<core::ErrorDTO> ReadError() const {
    std::lock_guard lock(mutex_);
    if (error_ && error_->code == "duplicate_component_instance")
      return error_;
    if (matched_count_ && *matched_count_ > 1) {
      return core::ErrorDTO{
          "duplicate_component_instance",
          "The selected ROS publisher role has multiple matched endpoints.",
          {{"reason", "duplicate_component_instance"}, {"topic", topic_},
           {"endpoint_count", std::to_string(*matched_count_)},
           {"evidence_kind", "ros_matched_publishers"}}};
    }
    if (dirty_ || graph_event_->check())
      return Unsettled("selected_endpoint_graph_changed");
    return error_;
  }

private:
  [[nodiscard]] core::ErrorDTO Unsettled(std::string cause) const {
    return {"component_instance_graph_unsettled",
            "The selected ROS publisher role is not established.",
            {{"reason", "component_instance_graph_unsettled"},
             {"topic", topic_}, {"graph_reason", std::move(cause)}}};
  }

  std::weak_ptr<rclcpp::Node> node_;
  const std::string topic_;
  const std::string type_;
  rclcpp::Event::SharedPtr graph_event_;
  std::mutex refresh_mutex_;
  mutable std::mutex mutex_;
  std::optional<int> matched_count_;
  std::uint64_t revision_{};
  bool dirty_{true};
  std::optional<core::ErrorDTO> error_;
};

} // namespace manyforge::ros2_control_execution
