#include <manyforge_roboplan/pointcloud_source_adapter.hpp>

#include <cmath>
#include <exception>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

using content_identity::CanonicalValue;

const CanonicalValue &setting(const CanonicalValue &row, const std::string_view name) {
  const auto *value = row.find_object_member(name);
  if (value == nullptr)
    throw std::invalid_argument("the point-cloud source has no " + std::string(name));
  return *value;
}

std::string text_setting(const CanonicalValue &row, const std::string_view name) {
  const auto &value = setting(row, name);
  if (!value.is_string() || value.as_string().empty() ||
      value.as_string().find_first_of(" \t\r\n") != std::string::npos)
    throw std::invalid_argument("the point-cloud source's " + std::string(name) +
                                " must be a name without spaces");
  return value.as_string();
}

double number_setting(const CanonicalValue &row, const std::string_view name, const double minimum,
                      const double maximum) {
  const auto &value = setting(row, name);
  if (!value.is_number() || !std::isfinite(value.as_number()) || value.as_number() < minimum ||
      value.as_number() > maximum) {
    std::ostringstream message;
    message << "the point-cloud source's " << name << " must be in [" << minimum << ", "
            << maximum << "]";
    throw std::invalid_argument(message.str());
  }
  return value.as_number();
}

}  // namespace

PointCloudSourceSettings pointcloud_source_settings(const CanonicalValue &row) {
  if (!row.is_object())
    throw std::invalid_argument("the point-cloud source's adapter row is not an object");
  PointCloudSourceSettings settings;
  settings.topic = text_setting(row, "topic");
  if (settings.topic.front() != '/')
    throw std::invalid_argument("the point-cloud source's topic must be absolute");
  settings.source_frame_id = text_setting(row, "source_frame_id");
  // The bridge's own bounds (pointcloud_observer_bridge.py).
  const auto max_points = number_setting(row, "max_points", 1.0, 200'000.0);
  if (max_points != std::floor(max_points))
    throw std::invalid_argument("the point-cloud source's max_points must be an integer");
  settings.max_points = static_cast<std::int64_t>(max_points);
  settings.minimum_period_s = number_setting(row, "minimum_period_s", 0.0, 10.0);
  settings.stream_budget_s = number_setting(row, "stream_budget_s", 0.001, 60.0);
  return settings;
}

ComponentProcessCommand pointcloud_bridge_command(const PointCloudSourceSettings &settings,
                                                  const std::string_view ros_namespace,
                                                  const std::string_view cell_id) {
  std::ostringstream minimum_period;
  minimum_period << settings.minimum_period_s;
  return {"manyforge_ros_bringup",
          "pointcloud_source.launch.py",
          {"namespace:=" + std::string(ros_namespace), "topic:=" + settings.topic,
           "source_frame_id:=" + settings.source_frame_id, "cell_id:=" + std::string(cell_id),
           "max_points:=" + std::to_string(settings.max_points),
           "minimum_period_s:=" + minimum_period.str()}};
}

core::ComponentState PointCloudSourceState(const bool process_started,
                                           const std::optional<std::string> &exit_reason,
                                           const bool forwarding,
                                           const PointCloudBridgeObservation &observed,
                                           const std::int64_t stream_budget_ns,
                                           const std::int64_t now_ns) {
  core::ComponentState state;
  state.observed_at_ns = now_ns;
  state.activity = core::ComponentActivity::kIdle;
  core::ComponentStream stream{std::string(kPointCloudStreamId), core::StreamFreshness::kNone,
                               std::nullopt};
  if (!process_started) {
    state.lifecycle = core::ComponentLifecycle::kNotPrepared;
  } else if (exit_reason) {
    state.lifecycle = core::ComponentLifecycle::kFault;
    state.fault = core::Fault{core::FaultCode::kProcessExited,
                              "The point-cloud bridge exited: " + *exit_reason,
                              true,
                              core::FaultOperatorAction::kRecover,
                              std::nullopt,
                              std::nullopt,
                              {},
                              now_ns};
  } else {
    state.lifecycle =
        forwarding ? core::ComponentLifecycle::kEnabled : core::ComponentLifecycle::kPrepared;
    if (observed.watching) {
      if (observed.last_received_ns) stream.age_ns = now_ns - *observed.last_received_ns;
      stream.freshness = stream.age_ns && *stream.age_ns <= stream_budget_ns
                             ? core::StreamFreshness::kFresh
                             : core::StreamFreshness::kStale;
    }
  }
  state.streams.push_back(std::move(stream));
  return state;
}

PointCloudSourceAdapter::PointCloudSourceAdapter(PointCloudSourceSettings settings,
                                                 std::string ros_namespace, std::string cell_id,
                                                 ComponentProcessLauncher start_process,
                                                 std::shared_ptr<IPointCloudBridgeLink> link)
    : settings_(std::move(settings)),
      ros_namespace_(std::move(ros_namespace)),
      cell_id_(std::move(cell_id)),
      start_process_(std::move(start_process)),
      link_(std::move(link)) {
  if (!start_process_ || !link_ || ros_namespace_.empty() || cell_id_.empty())
    throw std::invalid_argument(
        "the point-cloud source needs a process launcher, its bridge, a namespace and a cell");
}

core::ComponentState PointCloudSourceAdapter::State(const std::int64_t now_ns) const {
  const auto observed = link_->Observed();
  const std::lock_guard lock(mutex_);
  return PointCloudSourceState(
      process_ != nullptr, process_ ? process_->exited() : std::nullopt, forwarding_, observed,
      static_cast<std::int64_t>(settings_.stream_budget_s * 1e9), now_ns);
}

core::ActionResult PointCloudSourceAdapter::Run(const core::ComponentAction action,
                                                std::stop_token) {
  core::ActionResult result;
  result.action = action;
  result.outcome = core::ActionOutcome::kSucceeded;
  const auto fail = [&](std::string code, std::string message) {
    result.outcome = core::ActionOutcome::kFailed;
    result.error =
        core::ActionError{std::move(code), std::move(message), std::nullopt, std::nullopt};
  };
  switch (action) {
    case core::ComponentAction::kPrepare:
    case core::ComponentAction::kRecover: {
      std::shared_ptr<IComponentProcess> previous;
      {
        const std::lock_guard lock(mutex_);
        previous = std::move(process_);
        forwarding_ = false;
      }
      link_->SetForwarding(false);
      previous.reset();  // stops the old bridge
      try {
        auto started = start_process_(pointcloud_bridge_command(settings_, ros_namespace_, cell_id_));
        const std::lock_guard lock(mutex_);
        process_ = std::move(started);
      } catch (const std::exception &error) {
        fail("component_process_start_failed",
             std::string("The point-cloud bridge couldn't start: ") + error.what());
      }
      break;
    }
    case core::ComponentAction::kEnable:
    case core::ComponentAction::kDisable: {
      const bool forwarding = action == core::ComponentAction::kEnable;
      std::shared_ptr<IComponentProcess> process;
      {
        const std::lock_guard lock(mutex_);
        process = process_;
        if (forwarding && (!process || process->exited())) {
          fail("component_not_prepared", "Prepare the point-cloud source before enabling it.");
          break;
        }
      }
      // State keeps the last completed forwarding value while middleware runs.
      // The process pin prevents a replaced bridge from being destroyed here.
      try {
        link_->SetForwarding(forwarding);
      } catch (...) {
        // Preserve the old action's state on a publish exception, but never
        // update a newer bridge that replaced the pinned process.
        const std::lock_guard lock(mutex_);
        if (process_ == process) forwarding_ = forwarding;
        throw;
      }
      {
        const std::lock_guard lock(mutex_);
        if (process_ != process) {
          result.outcome = core::ActionOutcome::kUnknown;
          result.error = core::ActionError{
              "component_action_outcome_unknown",
              "The point-cloud bridge changed while forwarding was being set",
              std::nullopt, std::nullopt};
        } else {
          forwarding_ = forwarding;
        }
      }
      break;
    }
    default:
      fail("component_action_unsupported", "A point-cloud source doesn't offer this action.");
      break;
  }
  return result;
}

std::vector<core::Capability> PointCloudSourceAdapter::capabilities() const { return {}; }

}  // namespace manyforge::roboplan_backend
