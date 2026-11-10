#include <manyforge_roboplan/assistant_component_adapter.hpp>

#include <cmath>
#include <exception>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

using content_identity::CanonicalValue;

const CanonicalValue &setting(const CanonicalValue &row, const std::string_view name) {
  const auto *value = row.find_object_member(name);
  if (value == nullptr) throw std::invalid_argument("the assistant has no " + std::string(name));
  return *value;
}

double number_setting(const CanonicalValue &row, const std::string_view name, const double minimum,
                      const double maximum) {
  const auto &value = setting(row, name);
  if (!value.is_number() || !std::isfinite(value.as_number()) || value.as_number() < minimum ||
      value.as_number() > maximum) {
    std::ostringstream message;
    message << "the assistant's " << name << " must be in [" << minimum << ", " << maximum
            << "]";
    throw std::invalid_argument(message.str());
  }
  return value.as_number();
}

}  // namespace

AssistantSettings assistant_settings(const CanonicalValue &row) {
  if (!row.is_object())
    throw std::invalid_argument("the assistant's adapter row is not an object");
  AssistantSettings settings;
  const auto &backend = setting(row, "backend");
  if (!backend.is_string() || backend.as_string().empty() ||
      backend.as_string().find_first_of(" \t\r\n") != std::string::npos)
    throw std::invalid_argument("the assistant's backend must be a name without spaces");
  settings.backend = backend.as_string();
  settings.health_period_s = number_setting(row, "health_period_s", 0.05, 60.0);
  settings.stream_budget_s = number_setting(row, "stream_budget_s", 0.001, 60.0);
  return settings;
}

ComponentProcessCommand assistant_host_command(const AssistantSettings &settings,
                                               const std::string_view ros_namespace) {
  std::ostringstream health_period;
  health_period << settings.health_period_s;
  return {"manyforge_ros_bringup",
          "assistant.launch.py",
          {"namespace:=" + std::string(ros_namespace), "backend:=" + settings.backend,
           "health_period_s:=" + health_period.str()}};
}

core::ComponentState AssistantComponentState(const bool process_started,
                                             const std::optional<std::string> &exit_reason,
                                             const bool serving,
                                             const AssistantHostObservation &observed,
                                             const std::int64_t stream_budget_ns,
                                             const std::int64_t now_ns) {
  core::ComponentState state;
  state.observed_at_ns = now_ns;
  state.activity = core::ComponentActivity::kIdle;
  core::ComponentStream stream{std::string(kAssistantStreamId), core::StreamFreshness::kNone,
                               std::nullopt};
  if (!process_started) {
    state.lifecycle = core::ComponentLifecycle::kNotPrepared;
  } else if (exit_reason) {
    state.lifecycle = core::ComponentLifecycle::kFault;
    state.fault = core::Fault{core::FaultCode::kProcessExited,
                              "The assistant host exited: " + *exit_reason,
                              true,
                              core::FaultOperatorAction::kRecover,
                              std::nullopt,
                              std::nullopt,
                              {},
                              now_ns};
  } else if (serving && !observed.endpoint.empty()) {
    state.lifecycle = core::ComponentLifecycle::kEnabled;
    if (observed.busy) state.activity = core::ComponentActivity::kBusy;
    if (observed.last_health_ns) stream.age_ns = now_ns - *observed.last_health_ns;
    stream.freshness = stream.age_ns && *stream.age_ns <= stream_budget_ns
                           ? core::StreamFreshness::kFresh
                           : core::StreamFreshness::kStale;
    state.endpoints.push_back({std::string(kAssistantRequestRole), observed.endpoint});
  } else {
    state.lifecycle = core::ComponentLifecycle::kPrepared;
  }
  state.streams.push_back(std::move(stream));
  return state;
}

AssistantComponentAdapter::AssistantComponentAdapter(
    AssistantSettings settings, std::string ros_namespace, ComponentProcessLauncher start_process,
    std::shared_ptr<IAssistantHostLink> link, std::shared_ptr<core::IClock> clock,
    const std::chrono::nanoseconds lifecycle_command_timeout, std::function<void()> pause)
    : settings_(std::move(settings)),
      ros_namespace_(std::move(ros_namespace)),
      start_process_(std::move(start_process)),
      link_(std::move(link)),
      clock_(std::move(clock)),
      lifecycle_command_timeout_(lifecycle_command_timeout),
      pause_(std::move(pause)) {
  if (!start_process_ || !link_ || !clock_ || ros_namespace_.empty())
    throw std::invalid_argument(
        "the assistant needs a process launcher, its host, a clock and a namespace");
  if (!pause_) pause_ = [] { std::this_thread::sleep_for(kAssistantPollPeriod); };
}

core::ComponentState AssistantComponentAdapter::State(const std::int64_t now_ns) const {
  const auto observed = link_->Observed();
  std::shared_ptr<IComponentProcess> process;
  bool serving{};
  {
    const std::lock_guard lock(mutex_);
    process = process_;
    serving = serving_;
  }
  return AssistantComponentState(process != nullptr,
                                 process ? process->exited() : std::nullopt, serving, observed,
                                 static_cast<std::int64_t>(settings_.stream_budget_s * 1e9),
                                 now_ns);
}

core::ActionResult AssistantComponentAdapter::Run(const core::ComponentAction action,
                                                  std::stop_token stop) {
  const auto started_at_ns = clock_->now();
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
        serving_ = false;
      }
      link_->SetServing(false);
      previous.reset();  // stops the old host and its bridge
      try {
        auto started = start_process_(assistant_host_command(settings_, ros_namespace_));
        const std::lock_guard lock(mutex_);
        process_ = std::move(started);
      } catch (const std::exception &error) {
        fail("component_process_start_failed",
             std::string("The assistant host couldn't start: ") + error.what());
      }
      break;
    }
    case core::ComponentAction::kEnable:
    case core::ComponentAction::kDisable: {
      const bool serving = action == core::ComponentAction::kEnable;
      std::shared_ptr<IComponentProcess> process;
      {
        const std::lock_guard lock(mutex_);
        process = process_;
      }
      if (serving && (!process || process->exited())) {
        fail("component_not_prepared", "Prepare the assistant before enabling it.");
        break;
      }
      link_->SetServing(serving);
      // Wait for the host's report on this (the action's) thread; State keeps
      // the last completed Enable or Disable meanwhile. Serving is reported by
      // an endpoint together with a `/health` answer that arrived during this
      // action: a latched endpoint of a host that Recover replaced has none.
      const auto reported = [&] {
        const auto observed = link_->Observed();
        if (!serving) return observed.endpoint.empty();
        return !observed.endpoint.empty() && observed.last_health_ns &&
               *observed.last_health_ns >= started_at_ns;
      };
      const auto deadline_ns = started_at_ns + lifecycle_command_timeout_.count();
      const auto what = serving ? std::string("serving") : std::string("stopped");
      for (;;) {
        if (reported()) break;
        if (stop.stop_requested()) {
          fail("component_action_interrupted_by_stop",
               "A Stop interrupted the wait for the assistant bridge to be " + what);
          break;
        }
        if (!process || process->exited()) {
          fail("component_action_result_not_observed",
               "The assistant host exited before its bridge was " + what);
          break;
        }
        if (clock_->now() >= deadline_ns) {
          fail("component_action_result_not_observed",
               "The assistant host didn't report its bridge " + what +
                   " within the lifecycle command time");
          break;
        }
        pause_();
      }
      if (result.outcome != core::ActionOutcome::kSucceeded) break;
      const std::lock_guard lock(mutex_);
      if (process_ != process) {
        result.outcome = core::ActionOutcome::kUnknown;
        result.error = core::ActionError{"component_action_outcome_unknown",
                                         "The assistant host changed while serving was being set",
                                         std::nullopt, std::nullopt};
      } else {
        serving_ = serving;
      }
      break;
    }
    default:
      fail("component_action_unsupported", "An assistant doesn't offer this action.");
      break;
  }
  return result;
}

std::vector<core::Capability> AssistantComponentAdapter::capabilities() const { return {}; }

}  // namespace manyforge::roboplan_backend
