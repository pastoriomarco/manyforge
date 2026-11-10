// Copyright 2026 Marco Pastorio
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <rclcpp/rclcpp.hpp>
#include <rclcpp/waitable.hpp>

#include <functional>
#include <atomic>
#include <memory>
#include <utility>
#include <vector>

namespace manyforge::ros2_control_execution {

/// Owner-executor observation of node graph changes. The graph event is set
/// by rclcpp's GraphListener; no graph guard is added to a second wait set.
/// Construct and destroy on the node's owner. The supplied callback must not
/// wait and must capture exact entity lifetime pins or weak references.
class GraphEventObserver final {
  class EventWaitable final : public rclcpp::Waitable {
   public:
    EventWaitable(rclcpp::Event::SharedPtr event, std::function<void()> refresh)
        : event_(std::move(event)), refresh_(std::move(refresh)) {}
    void add_to_wait_set(rcl_wait_set_t &) override {}
    bool is_ready(const rcl_wait_set_t &) override {
      const bool requested = requested_.exchange(false, std::memory_order_acq_rel);
      const bool graph_changed = event_->check_and_clear();
      return requested || graph_changed;
    }
    std::shared_ptr<void> take_data() override { return {}; }
    std::shared_ptr<void> take_data_by_entity_id(size_t) override { return {}; }
    void execute(const std::shared_ptr<void> &) override { refresh_(); }
    void set_on_ready_callback(std::function<void(size_t, int)>) override {}
    void clear_on_ready_callback() override {}
    std::vector<std::shared_ptr<rclcpp::TimerBase>> get_timers() const override { return {}; }
    void request() noexcept { requested_.store(true, std::memory_order_release); }
   private:
    rclcpp::Event::SharedPtr event_;
    std::function<void()> refresh_;
    std::atomic<bool> requested_{false};
  };

 public:
  GraphEventObserver(rclcpp::Node::SharedPtr node, rclcpp::CallbackGroup::SharedPtr group,
                     std::function<void()> refresh)
      : node_(std::move(node)), group_(std::move(group)),
        waitable_(std::make_shared<EventWaitable>(node_->get_graph_event(), std::move(refresh))) {
    node_->get_node_waitables_interface()->add_waitable(waitable_, group_);
  }
  ~GraphEventObserver() {
    node_->get_node_waitables_interface()->remove_waitable(waitable_, group_);
  }
  GraphEventObserver(const GraphEventObserver &) = delete;
  GraphEventObserver &operator=(const GraphEventObserver &) = delete;
  /// Ask the owner to refresh without waiting. Call without application locks.
  bool request_refresh() noexcept {
    waitable_->request();
    try {
      node_->get_node_base_interface()->trigger_notify_guard_condition();
      return true;
    } catch (...) {
      return false;
    }
  }

 private:
  rclcpp::Node::SharedPtr node_;
  rclcpp::CallbackGroup::SharedPtr group_;
  std::shared_ptr<EventWaitable> waitable_;
};

}  // namespace manyforge::ros2_control_execution
