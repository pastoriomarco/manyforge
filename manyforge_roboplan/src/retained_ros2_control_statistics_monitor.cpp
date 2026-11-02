#include <manyforge_roboplan/retained_ros2_control_statistics_monitor.hpp>

#include <cmath>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace manyforge::roboplan_backend {

RetainedControllerStatisticsMonitor::RetainedControllerStatisticsMonitor(
    RetainedControllerStatisticsConfig config, const core::IClock &clock,
    Warning warning)
    : config_(std::move(config)), clock_(clock), warning_(std::move(warning)),
      min_name_(config_.controller_name + ".stats/periodicity/min"),
      sample_count_name_(config_.controller_name +
                         ".stats/periodicity/sample_count") {
  if (config_.controller_manager.empty() || config_.controller_name.empty() ||
      !std::isfinite(config_.controller_update_rate_hz) ||
      config_.controller_update_rate_hz <= 0.0 ||
      config_.warning_throttle_ns <= 0)
    throw std::invalid_argument(
        "controller statistics monitor requires the controller manager, the "
        "controller and its update rate");
  snapshot_.controller_name = config_.controller_name;
  snapshot_.warning_margin_period_s = 2.0 / config_.controller_update_rate_hz;
}

void RetainedControllerStatisticsMonitor::Observe(
    const pal_statistics_msgs::msg::Statistics &message,
    const core::TimePoint receipt_ns) {
  std::optional<std::string> warning;
  {
    std::lock_guard lock(mutex_);
    snapshot_.observed = true;
    ++snapshot_.observed_statistics_updates;
    last_receipt_ns_ = receipt_ns;
    for (const auto &statistic : message.statistics) {
      if (statistic.name == min_name_) {
        if (std::isfinite(statistic.value) && statistic.value > 0.0) {
          snapshot_.since_activation_min_hz = statistic.value;
          snapshot_.since_activation_worst_period_s = 1.0 / statistic.value;
        } else {
          // No cycle observed since activation (the collector reports NaN).
          snapshot_.since_activation_min_hz.reset();
          snapshot_.since_activation_worst_period_s.reset();
        }
      } else if (statistic.name == sample_count_name_ &&
                 std::isfinite(statistic.value)) {
        snapshot_.since_activation_sample_count = statistic.value;
      }
    }
    if (snapshot_.since_activation_worst_period_s.has_value() &&
        *snapshot_.since_activation_worst_period_s >
            snapshot_.warning_margin_period_s) {
      ++snapshot_.observed_updates_beyond_margin;
      if (!last_warning_ns_.has_value() ||
          receipt_ns - *last_warning_ns_ >= config_.warning_throttle_ns) {
        last_warning_ns_ = receipt_ns;
        std::ostringstream text;
        text << "observed controller statistics: '" << config_.controller_name
             << "' worst period since activation "
             << *snapshot_.since_activation_worst_period_s
             << " s exceeds the warning margin "
             << snapshot_.warning_margin_period_s
             << " s (two controller periods); observed statistics updates "
             << snapshot_.observed_statistics_updates
             << ", of them beyond the margin "
             << snapshot_.observed_updates_beyond_margin
             << " (observed updates, not late-cycle counts)";
        warning = text.str();
      }
    }
  }
  if (warning.has_value() && warning_)
    warning_(*warning);
}

RetainedControllerStatisticsSnapshot
RetainedControllerStatisticsMonitor::Read() const {
  const auto now = clock_.now();
  std::lock_guard lock(mutex_);
  auto snapshot = snapshot_;
  if (last_receipt_ns_.has_value())
    snapshot.receipt_age_ns = now > *last_receipt_ns_ ? now - *last_receipt_ns_ : 0;
  return snapshot;
}

rclcpp::Subscription<pal_statistics_msgs::msg::Statistics>::SharedPtr
RetainedControllerStatisticsMonitor::Subscribe(
    const rclcpp::Node::SharedPtr &node,
    const std::shared_ptr<RetainedControllerStatisticsMonitor> &monitor) {
  if (!node || !monitor)
    throw std::invalid_argument("controller statistics subscription requires a node");
  const std::weak_ptr<RetainedControllerStatisticsMonitor> weak = monitor;
  return node->create_subscription<pal_statistics_msgs::msg::Statistics>(
      monitor->config().controller_manager + "/statistics/full",
      rclcpp::QoS(10).best_effort(),
      [weak](const pal_statistics_msgs::msg::Statistics::ConstSharedPtr message) {
        if (const auto self = weak.lock())
          self->Observe(*message, self->clock_.now());
      });
}

} // namespace manyforge::roboplan_backend
