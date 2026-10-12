#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <manyforge_core/time/iclock.hpp>
#include <pal_statistics_msgs/msg/statistics.hpp>
#include <rclcpp/rclcpp.hpp>

namespace manyforge::roboplan_backend {

/// Q54 (monitor only, DX-136): the controller manager's per-controller
/// statistics for one trajectory controller, observed by the host. The
/// controller manager resets these statistics immediately before the
/// controller's activation, so its values are the extrema since activation,
/// delivered as delayed telemetry. They are reported as observed statistics
/// updates, never as per-event late-cycle counts. They are not execution
/// timing facts: the port's `ExecutionTimingDTO` period fields stay absent
/// and nothing here stops motion.
struct RetainedControllerStatisticsConfig final {
  /// Absolute controller-manager name; its statistics topic is
  /// `<controller_manager>/statistics/full`.
  std::string controller_manager;
  std::string controller_name;
  /// The controller's nominal update rate; Q55 warning margin: a worst
  /// observed period beyond two controller periods.
  double controller_update_rate_hz{0.0};
  /// Warning throttle (as the controller manager's own overrun warnings).
  core::Duration warning_throttle_ns{1'000'000'000};
};

struct RetainedControllerStatisticsSnapshot final {
  std::string controller_name;
  /// At least one statistics update was received.
  bool observed{false};
  /// `<controller>.stats/periodicity/min`: the lowest observed update
  /// frequency since activation (Hz), and its inverse, the worst period.
  std::optional<double> since_activation_min_hz;
  std::optional<double> since_activation_worst_period_s;
  /// `<controller>.stats/periodicity/sample_count` as last reported.
  std::optional<double> since_activation_sample_count;
  /// Local time since the last statistics update was received.
  std::optional<core::Duration> receipt_age_ns;
  /// Statistics updates received (observed updates, not controller cycles).
  std::uint64_t observed_statistics_updates{0U};
  /// Observed updates whose worst period was beyond the warning margin.
  std::uint64_t observed_updates_beyond_margin{0U};
  /// Two controller periods (Q55).
  double warning_margin_period_s{0.0};
};

class RetainedControllerStatisticsMonitor final {
public:
  using Warning = std::function<void(const std::string &)>;

  /// Throws on an unusable configuration (no names or update rate).
  RetainedControllerStatisticsMonitor(RetainedControllerStatisticsConfig config,
                                      const core::IClock &clock, Warning warning);

  /// One statistics update, received at local time `receipt_ns`.
  void Observe(const pal_statistics_msgs::msg::Statistics &message,
               core::TimePoint receipt_ns);

  [[nodiscard]] RetainedControllerStatisticsSnapshot Read() const;

  /// The one subscription on `node` (best effort, so it matches any
  /// publisher reliability); the monitor is held weakly.
  [[nodiscard]] static rclcpp::Subscription<pal_statistics_msgs::msg::Statistics>::SharedPtr
  Subscribe(const rclcpp::Node::SharedPtr &node,
            const std::shared_ptr<RetainedControllerStatisticsMonitor> &monitor);

  [[nodiscard]] const RetainedControllerStatisticsConfig &config() const noexcept {
    return config_;
  }

private:
  const RetainedControllerStatisticsConfig config_;
  const core::IClock &clock_;
  const Warning warning_;
  const std::string min_name_;
  const std::string sample_count_name_;
  mutable std::mutex mutex_;
  RetainedControllerStatisticsSnapshot snapshot_;
  std::optional<core::TimePoint> last_receipt_ns_;
  std::optional<core::TimePoint> last_warning_ns_;
};

} // namespace manyforge::roboplan_backend
