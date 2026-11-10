// Copyright 2026 Marco Pastorio
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <memory>
#include <utility>
#include <rclcpp/context.hpp>

namespace manyforge::roboplan_backend::host_detail {

/// One exact context's validity, sampled at construction and shutdown.
class CachedRosContext final {
 public:
  explicit CachedRosContext(std::shared_ptr<rclcpp::Context> context)
      : context_(std::move(context)), valid_(std::make_shared<std::atomic<bool>>(true)) {
    const std::weak_ptr<std::atomic<bool>> weak = valid_;
    callback_ = context_->add_on_shutdown_callback([weak] {
      if (const auto flag = weak.lock()) flag->store(false, std::memory_order_release);
    });
    if (!context_->is_valid()) valid_->store(false, std::memory_order_release);
  }
  ~CachedRosContext() {
    try { (void)context_->remove_on_shutdown_callback(callback_); } catch (...) {}
  }
  bool valid() const noexcept { return valid_->load(std::memory_order_acquire); }
 private:
  std::shared_ptr<rclcpp::Context> context_;
  std::shared_ptr<std::atomic<bool>> valid_;
  rclcpp::OnShutdownCallbackHandle callback_;
};

}  // namespace manyforge::roboplan_backend::host_detail
