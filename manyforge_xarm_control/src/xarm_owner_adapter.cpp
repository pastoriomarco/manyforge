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

#include <manyforge_xarm_control/xarm_owner_adapter.hpp>

#include <algorithm>
#include <bit>
#include <stdexcept>
#include <utility>

namespace manyforge::xarm_control {

using core::ActionOutcome;
using core::ActionResult;
using core::ComponentAction;
using core::ComponentLifecycle;

namespace {

[[nodiscard]] ActionResult result(const ComponentAction action, const ActionOutcome outcome,
                                  std::optional<core::ActionError> error = std::nullopt) {
  ActionResult made;
  made.action = action;
  made.outcome = outcome;
  made.error = std::move(error);
  return made;
}

[[nodiscard]] ActionResult failed(const ComponentAction action, std::string code,
                                  std::string message,
                                  std::optional<std::string> vendor_code = std::nullopt,
                                  std::optional<std::string> vendor_text = std::nullopt) {
  return result(action, ActionOutcome::kFailed,
                core::ActionError{std::move(code), std::move(message), std::move(vendor_code),
                                  std::move(vendor_text)});
}

/// One sent vendor call that wasn't answered `ret=0`, typed: no answer or a
/// Stop during it is `unknown` (never resent); refused before sending or
/// answered otherwise is `failed`.
[[nodiscard]] ActionResult call_failure(const ComponentAction action,
                                        const XArmServiceCallResult &call,
                                        const std::string_view service) {
  const auto vendor = call.vendor_return
                          ? std::optional<std::string>("ret=" + std::to_string(*call.vendor_return))
                          : std::nullopt;
  if (call.outcome == XArmServiceCallOutcome::kNotSent) {
    if (call.reason == "xarm_service_cancelled")
      return failed(action, "component_action_interrupted_by_stop",
                    "a Stop interrupted the action before " + std::string(service));
    return failed(action, "robot_controller_call_failed",
                  "the robot controller's " + std::string(service) + " is not available (" +
                      call.reason + ")");
  }
  if (XArmCallUnanswered(call.reason))
    return result(action, ActionOutcome::kUnknown,
                  core::ActionError{std::string(kRobotControllerUnresponsiveCode),
                                    "the robot controller did not answer " + std::string(service),
                                    std::nullopt, std::nullopt});
  if (call.reason == "xarm_service_cancelled")
    return result(action, ActionOutcome::kUnknown,
                  core::ActionError{"component_action_interrupted_by_stop",
                                    "a Stop interrupted the action during " + std::string(service),
                                    std::nullopt, std::nullopt});
  return failed(action, "robot_controller_call_failed",
                "the robot controller refused " + std::string(service), vendor);
}

} // namespace

XArmOwnerAdapter::XArmOwnerAdapter(XArmOwnerAdapterPorts ports) : ports_(std::move(ports)) {
  if (!ports_.status || !ports_.driver || !ports_.lane || !ports_.restart_driver ||
      !ports_.controller_reachable || !ports_.wait || ports_.clock == nullptr)
    throw std::invalid_argument("the xArm owner adapter needs every port");
}

std::int64_t XArmOwnerAdapter::Now() const { return ports_.clock->now(); }

bool XArmOwnerAdapter::Fresh(const XArmObservedState &observed, const std::int64_t now_ns) const {
  return observed.received_timestamp_ns &&
         now_ns - *observed.received_timestamp_ns <= kXArmStatusFreshnessNs;
}

void XArmOwnerAdapter::AddMember(XArmMember member) {
  std::lock_guard lock(mutex_);
  // A member rebuilt by a later Apply replaces its earlier registration.
  const auto current = std::find_if(members_.begin(), members_.end(), [&](const auto &row) {
    return row.component_id == member.component_id;
  });
  if (current != members_.end())
    *current = std::move(member);
  else
    members_.push_back(std::move(member));
}

std::vector<XArmMember> XArmOwnerAdapter::Members() const {
  std::lock_guard lock(mutex_);
  return members_;
}

std::vector<std::string> XArmOwnerAdapter::Concerns(const XArmFaultScope scope) const {
  const auto role = scope == XArmFaultScope::kArm            ? kXArmArmRole
                    : scope == XArmFaultScope::kControllerIo ? kXArmControllerIoRole
                    : scope == XArmFaultScope::kToolIo       ? kXArmToolIoRole
                                                             : std::string_view{};
  std::vector<std::string> ids;
  for (const auto &member : Members())
    if (!member.adapter.expired() && (scope == XArmFaultScope::kMachine || member.role == role))
      ids.push_back(member.component_id);
  return ids;
}

bool XArmOwnerAdapter::Usable(const std::int64_t now_ns) const {
  const auto observed = ports_.status->observe();
  std::lock_guard lock(mutex_);
  return prepared_ && !reason_ && Fresh(observed, now_ns);
}

void XArmOwnerAdapter::OnIoStates(const XArmIoStatesSample &sample) {
  for (const auto &member : Members()) {
    const auto io = member.io.lock();
    if (!io) continue;
    const auto updates = io->OnIoStates(sample);
    if (ports_.io_sink) ports_.io_sink(member.component_id, updates, sample.received_at_ns);
  }
}

void XArmOwnerAdapter::OnGpioModuleStatus(const XArmGpioModuleStatus &status) {
  {
    std::lock_guard lock(mutex_);
    gpio_ = status;
  }
  for (const auto &member : Members())
    if (const auto io = member.io.lock()) io->OnGpioModuleStatus(status);
}

core::ComponentState XArmOwnerAdapter::State(const std::int64_t now_ns) const {
  core::ComponentState state;
  state.observed_at_ns = now_ns;
  state.activity = core::ComponentActivity::kIdle;
  const auto observed = ports_.status->observe();
  const bool fresh = Fresh(observed, now_ns);
  core::ComponentStream stream{"robot_states", core::StreamFreshness::kNone, std::nullopt};
  if (observed.received_timestamp_ns) {
    stream.freshness = fresh ? core::StreamFreshness::kFresh : core::StreamFreshness::kStale;
    stream.age_ns = now_ns - *observed.received_timestamp_ns;
  }
  state.streams.push_back(std::move(stream));

  std::optional<core::ComponentStateReason> reason;
  std::optional<XArmGpioModuleStatus> gpio;
  bool prepared = false;
  {
    std::lock_guard lock(mutex_);
    reason = reason_;
    gpio = gpio_;
    prepared = prepared_;
  }
  if (fresh) {
    // AUD-2: the controller's report as M7's vendor status, display only.
    const auto motors = observed.motion_enable_mask < 0
                            ? std::string("unknown")
                            : std::to_string(std::popcount(
                                  static_cast<unsigned>(observed.motion_enable_mask)));
    state.details = {
        {"mode", std::to_string(observed.mode)},
        {"state", std::to_string(observed.state)},
        {"error", observed.error_code > 0 ? xarm_vendor_code(observed.error_code) + ": " +
                                                std::string(xarm_error_text(observed.error_code))
                                          : "none"},
        {"warning", observed.warning_code > 0 ? std::to_string(observed.warning_code) : "none"},
        {"motors_enabled", motors}};
  }
  const auto lane = ports_.lane->State(now_ns);
  if (reason || lane.lifecycle == ComponentLifecycle::kOffline) {
    // Shut down or rebooting by the operator, or the process isn't running:
    // offline, not a fault.
    state.lifecycle = ComponentLifecycle::kOffline;
    state.reason = reason ? reason : lane.reason;
    std::lock_guard lock(mutex_);
    fault_since_.reset();
    return state;
  }

  std::optional<core::Fault> fault;
  if (lane.fault && lane.fault->code == core::FaultCode::kProcessExited) {
    fault = lane.fault;
    fault->concerns = Concerns(XArmFaultScope::kMachine);
  } else if (!fresh) {
    fault = core::Fault{core::FaultCode::kCommunication,
                        "the robot controller's status report is missing or out of date",
                        true,
                        core::FaultOperatorAction::kRecover,
                        std::nullopt,
                        std::nullopt,
                        Concerns(XArmFaultScope::kMachine),
                        0};
  } else if (observed.error_code > 0) {
    fault = xarm_fault(observed.error_code, 0);
    fault->concerns = Concerns(xarm_fault_scope(observed.error_code));
  } else if (gpio && (gpio->state != 0 || gpio->code != 0)) {
    // The controller I/O's own error, not the machine's connection.
    const auto vendor =
        "gpio state=" + std::to_string(gpio->state) + " code=" + std::to_string(gpio->code);
    fault = core::Fault{core::FaultCode::kCommunication,
                        "the controller's GPIO module reports an error (" + vendor + ")",
                        true,
                        core::FaultOperatorAction::kRecover,
                        vendor,
                        gpio->state == 6 ? "communication failure" : "GPIO module error",
                        Concerns(XArmFaultScope::kControllerIo),
                        0};
  } else {
    for (const auto &member : Members()) {
      const auto arm = member.arm.lock();
      if (!arm || !arm->ExternalControl(observed, now_ns)) continue;
      fault = xarm_external_control_fault(observed.mode, observed.state, 0);
      fault->concerns = {member.component_id};
      break;
    }
  }
  std::lock_guard lock(mutex_);
  if (!fault) {
    fault_since_.reset();
    state.lifecycle = prepared ? ComponentLifecycle::kPrepared : ComponentLifecycle::kNotPrepared;
    return state;
  }
  // `since` is when this fault first appeared, not the latest report.
  const auto key =
      std::string(core::to_string(fault->code)) + "/" + fault->vendor_code.value_or("");
  if (!fault_since_ || fault_since_->first != key) fault_since_.emplace(key, now_ns);
  fault->since_ns = fault_since_->second;
  state.lifecycle = ComponentLifecycle::kFault;
  state.fault = std::move(fault);
  return state;
}

std::vector<core::Capability> XArmOwnerAdapter::capabilities() const {
  return {core::Capability::kPowerOff, core::Capability::kReboot};
}

ActionResult XArmOwnerAdapter::Run(const ComponentAction action, const std::stop_token stop) {
  switch (action) {
  case ComponentAction::kPrepare:
  case ComponentAction::kRecover: return Clear(action, stop);
  case ComponentAction::kPowerOff:
  case ComponentAction::kReboot: return PowerAction(action, stop);
  case ComponentAction::kEnable:
  case ComponentAction::kDisable:
  case ComponentAction::kResetPosition:
  case ComponentAction::kSimReset: break;
  }
  return failed(action, "component_action_unsupported",
                "the xArm machine doesn't support this action; its members do");
}

std::optional<ActionResult> XArmOwnerAdapter::ClearError(const ComponentAction action,
                                                         const std::stop_token stop,
                                                         int &warning) {
  // A servo error needs a power cycle before its error is cleared.
  const auto observed = ports_.status->observe();
  if (Fresh(observed, Now()) && xarm_drive_error(observed.error_code))
    return failed(action, "fault_drive",
                  "a servo error needs a power cycle of the robot before it is cleared",
                  xarm_vendor_code(observed.error_code),
                  std::string(xarm_error_text(observed.error_code)));
  const auto call = ports_.driver->CleanError(kXArmServiceResponseTimeout, stop);
  // `ret` 1 or 2: answered, an error or warning was still flagged in the
  // reply; the controller's error code says whether it remains.
  const bool answered =
      call.outcome == XArmServiceCallOutcome::kSucceeded ||
      (call.vendor_return && (*call.vendor_return == 1 || *call.vendor_return == 2));
  if (!answered) return call_failure(action, call, "clean_error");
  // As M7: read back at 0, 150 and 300 ms after the answer; the controller
  // may take a moment to process the clear.
  int error = 0;
  auto at = std::chrono::nanoseconds::zero();
  for (const auto read_at : kXArmClearReadsAt) {
    ports_.wait(read_at - at);
    at = read_at;
    const auto read = ports_.driver->GetErrWarnCode(kXArmServiceResponseTimeout, stop);
    if (!read.error) return call_failure(action, read.call, "get_err_warn_code");
    error = *read.error;
    warning = read.warning.value_or(0);
    if (error == 0) return std::nullopt;
  }
  return failed(action, "robot_error_not_cleared",
                "Couldn't clear the robot error. If the e-stop is pressed, release it, then "
                "Recover the machine and Enable the arm.",
                xarm_vendor_code(error), std::string(xarm_error_text(error)));
}

std::optional<ActionResult> XArmOwnerAdapter::AwaitClearedReport(const ComponentAction action,
                                                                 const std::stop_token stop,
                                                                 const std::int64_t deadline_ns) {
  // Live 2026-11-02 (Real Lite 6): the read showed the error cleared while
  // `robot_states` (5 Hz) still carried C2, so the owner's state was still
  // the fault when its members' Prepare followed. The action ends once the
  // owner's own state shows no fault (the report's error, its freshness, the
  // GPIO module's error, external control).
  const auto cleared = [&] { return State(Now()).lifecycle != ComponentLifecycle::kFault; };
  while (!cleared() && !stop.stop_requested() && Now() < deadline_ns)
    ports_.wait(std::chrono::nanoseconds(kXArmClearedReportPollNs));
  if (cleared()) return std::nullopt;
  if (stop.stop_requested())
    return failed(action, "component_action_interrupted_by_stop",
                  "a Stop interrupted the wait for the robot controller's report");
  const auto observed = ports_.status->observe();
  if (!Fresh(observed, Now()))
    return failed(action, "xarm_status_unavailable",
                  "the robot controller sent no status report after the error was cleared");
  if (observed.error_code > 0)
    return failed(action, "robot_error_not_cleared",
                  "Couldn't clear the robot error. If the e-stop is pressed, release it, then "
                  "Recover the machine and Enable the arm.",
                  xarm_vendor_code(observed.error_code),
                  std::string(xarm_error_text(observed.error_code)));
  const auto state = State(Now());
  if (!state.fault) return std::nullopt;
  return failed(action, std::string(core::to_string(state.fault->code)), state.fault->message,
                state.fault->vendor_code, state.fault->vendor_text);
}

std::optional<ActionResult> XArmOwnerAdapter::Connect(const ComponentAction action,
                                                      const std::stop_token stop) {
  bool offline = false;
  {
    std::lock_guard lock(mutex_);
    offline = reason_.has_value();
  }
  auto since = Now();
  if (!offline && Fresh(ports_.status->observe(), since)) return std::nullopt;
  const auto deadline = since + ports_.lifecycle_command_timeout.count();
  // A just-launched driver has not reported yet: its first report is awaited
  // while its launch starts, within the same bound (no restart).
  if (!offline && ports_.launch_settling) {
    while (!stop.stop_requested() && Now() < deadline && ports_.launch_settling() &&
           !Fresh(ports_.status->observe(), Now()))
      ports_.wait(std::chrono::nanoseconds(kXArmReportPeriodNs));
    if (Fresh(ports_.status->observe(), Now())) return std::nullopt;
    if (stop.stop_requested())
      return failed(action, "component_action_interrupted_by_stop",
                    "a Stop interrupted the wait for the driver's first report");
    if (Now() >= deadline)
      return failed(action, "xarm_status_unavailable",
                    "the robot controller sent no status report while its driver started");
    since = Now();
  }
  // Not connected, or after a shutdown or reboot (the driver doesn't
  // reconnect by itself): restart the machine's process, then its first
  // report connects it.
  if (auto restarted = ports_.restart_driver(stop); !restarted.ok())
    return failed(action, restarted.error().code, restarted.error().message);
  {
    std::lock_guard lock(mutex_);
    reason_.reset();
  }
  const auto reported = [&] {
    const auto observed = ports_.status->observe();
    return observed.received_timestamp_ns && *observed.received_timestamp_ns > since;
  };
  while (!reported() && !stop.stop_requested() && Now() < deadline)
    ports_.wait(std::chrono::nanoseconds(kXArmReportPeriodNs));
  if (!reported())
    return failed(action, "xarm_status_unavailable",
                  "the robot controller sent no status report after the driver restart");
  return std::nullopt;
}

ActionResult XArmOwnerAdapter::Clear(const ComponentAction action, const std::stop_token stop) {
  const auto deadline = Now() + ports_.lifecycle_command_timeout.count();
  if (auto refused = Connect(action, stop)) return *refused;
  int warning = 0;
  if (auto refused = ClearError(action, stop, warning)) return *refused;
  // A warning left after the clear is cleared too (XPW-1: the arm's
  // hardware, whose activation also clears it, stays off until Enable).
  if (warning > 0) {
    const auto call = ports_.driver->CleanWarn(kXArmServiceResponseTimeout, stop);
    if (call.outcome != XArmServiceCallOutcome::kSucceeded)
      return call_failure(action, call, "clean_warn");
  }
  // An arm taken over from outside is released: its Enable re-arms it.
  for (const auto &member : Members())
    if (const auto arm = member.arm.lock()) arm->ReleaseExternalControl(Now());
  if (auto refused = AwaitClearedReport(action, stop, deadline)) return *refused;
  std::lock_guard lock(mutex_);
  if (action == ComponentAction::kPrepare) prepared_ = true;
  return result(action, ActionOutcome::kSucceeded);
}

ActionResult XArmOwnerAdapter::PowerAction(const ComponentAction action,
                                           const std::stop_token stop) {
  // The core disabled the members first (CONTRACTS §2, §16).
  const bool reboot = action == ComponentAction::kReboot;
  const auto call = ports_.driver->SystemControl(reboot ? kXArmSystemReboot : kXArmSystemShutdown,
                                                 kXArmServiceResponseTimeout, stop);
  if (call.outcome != XArmServiceCallOutcome::kSucceeded)
    return call_failure(action, call, "system_control");
  {
    std::lock_guard lock(mutex_);
    prepared_ = false;
    reason_ = reboot ? core::ComponentStateReason{std::string(kRebootingReason), "rebooting"}
                     : core::ComponentStateReason{std::string(kShutdownByOperatorReason),
                                                  "shut down by the operator"};
  }
  if (!reboot) return result(action, ActionOutcome::kSucceeded);
  // The controller goes down, then answers again.
  const auto deadline = Now() + ports_.reboot_wait.count();
  for (const bool want_reachable : {false, true}) {
    while (ports_.controller_reachable() != want_reachable) {
      if (stop.stop_requested())
        return failed(action, "component_action_interrupted_by_stop",
                      "a Stop interrupted the wait for the rebooting robot controller");
      if (Now() >= deadline)
        return failed(action, "component_reboot_timeout",
                      "the robot controller did not come back from its reboot in time");
      ports_.wait(ports_.reboot_probe_interval);
    }
  }
  auto prepared = Clear(ComponentAction::kPrepare, stop);
  prepared.action = action;
  return prepared;
}

} // namespace manyforge::xarm_control
