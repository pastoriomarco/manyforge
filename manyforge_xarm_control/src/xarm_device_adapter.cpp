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

#include <manyforge_xarm_control/xarm_device_adapter.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <manyforge_xarm_control/lifecycle_profile.hpp>
#include <manyforge_xarm_control/xarm_brand_lifecycle_port.hpp>
#include <manyforge_xarm_control/xarm_driver_services.hpp>
#include <manyforge_xarm_control/xarm_robot_states_transport.hpp>
#include <manyforge_xarm_control/xarm_tool_context.hpp>

// V-A (merge 7): moved unchanged from manyforge_roboplan's
// `retained_vendor_device_status.cpp`; RoboPlan reaches it only through the
// xArm family's registry row.
namespace manyforge::xarm_control {

using roboplan_backend::IRetainedDeviceStatusAdapter;
using roboplan_backend::RetainedDeviceStatus;
using roboplan_backend::RetainedRos2ControlSwitchOutcome;
using roboplan_backend::RetainedRos2ControlSwitchResult;
using roboplan_backend::VendorAdapterSelection;
using roboplan_backend::VendorCallResult;
using roboplan_backend::VendorJointSampleUse;
using roboplan_backend::refuse_if_expired_at_send;

namespace {

[[nodiscard]] RetainedDeviceStatus
device_status(xarm_control::XArmDeviceStatus status) {
  return {status.cause, std::move(status.facts)};
}

[[nodiscard]] core::ErrorDTO vendor_error(std::string reason,
                                          std::string message) {
  return {std::string(core::to_string(core::ErrorCode::kExecutionRejected)),
          std::move(message),
          {{"reason", std::move(reason)}}};
}

// M6 XADAPT: vendor-neutral, user-facing: the robot controller did not
// answer in time; `reason` names the vendor call.
[[nodiscard]] core::ErrorDTO unresponsive(std::string reason) {
  return core::ErrorDTO{"robot_controller_unresponsive",
                        "the robot controller did not answer in time",
                        {{"reason", std::move(reason)}}};
}

// M6b (SX-727, the D8 rule): one call's allowed time, min(the call cap, the
// step's remaining time); none left: nothing (the call is not sent).
[[nodiscard]] std::optional<std::chrono::nanoseconds> allowed_call_time(
    const std::optional<std::chrono::steady_clock::time_point> &step_deadline) {
  if (!step_deadline) return xarm_control::kXArmServiceResponseTimeout;
  const auto now = std::chrono::steady_clock::now();
  if (now >= *step_deadline) return std::nullopt;
  return std::min<std::chrono::nanoseconds>(
      xarm_control::kXArmServiceResponseTimeout,
      std::chrono::duration_cast<std::chrono::nanoseconds>(*step_deadline - now));
}

[[nodiscard]] RetainedRos2ControlSwitchResult
vendor_result(const RetainedRos2ControlSwitchOutcome outcome,
              std::string reason, std::string message) {
  return {outcome, vendor_error(std::move(reason), std::move(message))};
}

// V-A (A3, moved from the generic handle): after a successful hardware
// activation (and after the adapter's own re-arm) the xArm plugin's first
// write() cycles deactivate every controller and later reactivate them, each
// through blocking controller-manager calls from its RT loop (list and
// switch, 1 s client timeout each; its switch requests a 2 s manager-side
// timeout). The handle waits at most this long for that transient to end.
constexpr std::chrono::nanoseconds kXArmActivationTransientBound = std::chrono::seconds(5);

// V-A (A2): an xArm call's result for the handle, typed. The error stays
// what the user sees; `effect` and the unresponsive code are the vendor
// package's own protocol.
[[nodiscard]] VendorCallResult vendor_call_result(const core::Result<bool> &result) {
  if (result.ok()) return {result.value()};
  const auto &error = result.error();
  const auto effect = error.details.find("effect");
  return {false, error, effect != error.details.end() &&
              (effect->second == "possible" || effect->second == "observed"),
          error.code == xarm_control::kRobotControllerUnresponsiveCode};
}

struct XArmClearFacts final {
  int error{0};
  int warning{0};
  int state{0};

  bool operator==(const XArmClearFacts &) const = default;
};

/// The xArm adapter (manyforge_xarm_control) over the driver's `robot_states`.
class XArmDeviceStatusAdapter final
    : public IRetainedDeviceStatusAdapter,
      public std::enable_shared_from_this<XArmDeviceStatusAdapter> {
public:
  explicit XArmDeviceStatusAdapter(VendorAdapterSelection selection)
      : selection_(std::move(selection)) {
    // Validates the adapter data before any ROS effect.
    static_cast<void>(
        xarm_control::xarm_robot_states_topic(selection_.driver_namespace));
    if (selection_.axis_count < 1 || selection_.axis_count > 7)
      throw std::invalid_argument("an xArm-family robot has 1 to 7 axes");
    if (!std::isfinite(selection_.joint_agreement_tolerance_rad) ||
        selection_.joint_agreement_tolerance_rad <= 0.0)
      throw std::invalid_argument(
          "an xArm-family robot needs a positive joint agreement tolerance");
  }

  void Start(rclcpp::Node &node, core::IClock &clock) override {
    auto transport = std::make_shared<xarm_control::XArmRobotStatesTransport>(
        node, selection_.driver_namespace, clock);
    auto port = std::make_shared<xarm_control::XArmBrandLifecyclePort>(
        std::move(transport),
        xarm_control::XArmBrandLifecyclePortConfig{
            xarm_control::kXArmStatusFreshnessNs, selection_.axis_count,
            selection_.joint_agreement_tolerance_rad});
    auto services = std::make_shared<xarm_control::XArmDriverServices>(
        node, selection_.driver_namespace);
    // TOOLCTX: each call bounded by the call cap, never retried; no cancellation
    // before a motion (the robot is at rest and nothing waits on it). M6b
    // (SX-727): Enable's offset send is part of its lifecycle step (its
    // remaining time and cancellation, `XArmToolContextBound`).
    auto tool_context = std::make_shared<xarm_control::XArmToolContextSender>(
        // M6 ADAPTERM: the offset leaves the ready state; its re-arm
        // (`set_state(0)`, ready) is part of the send, as own arming.
        [services, port, &clock](const core::Pose3DTO &tcp,
                                 const xarm_control::XArmToolContextBound &bound) {
          // No time left in the step: nothing is sent, no fault.
          if (auto refused = refuse_if_expired_at_send(bound.step_deadline, {}))
            return xarm_control::XArmTcpOffsetSent{false, refused->error};
          // Each call allowed min(the call cap, the step's remaining time),
          // recomputed at its send; none left before one: not sent.
          const auto bounded = [&](const auto &send) -> xarm_control::XArmReengageCallResult {
            const auto allowed = allowed_call_time(bound.step_deadline);
            if (!allowed)
              return {false, false, "component_lifecycle_deadline_expired", std::nullopt};
            const auto result = send(*allowed);
            return {result.outcome != xarm_control::XArmServiceCallOutcome::kNotSent,
                    result.outcome == xarm_control::XArmServiceCallOutcome::kSucceeded,
                    result.reason, result.vendor_return};
          };
          const auto sent = xarm_control::SendTcpOffsetAtRest(
              *port,
              {[&] {
                 return bounded([&](const std::chrono::nanoseconds allowed) {
                   return services->SetTcpOffset(tcp, allowed, bound.cancellation);
                 });
               },
               [&](const int state) {
                 return bounded([&](const std::chrono::nanoseconds allowed) {
                   return services->SetState(state, allowed, bound.cancellation);
                 });
               },
               [&clock] { return clock.now(); },
               [] { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }},
              bound.cancellation, bound.step_deadline);
          return xarm_control::XArmTcpOffsetSent{sent.reengaged, sent.error, sent.ready_offset};
        },
        [services](const double mass_kg,
                   const std::array<double, 3> &center_of_mass_m,
                   const xarm_control::XArmToolContextBound &bound) {
          const auto allowed = allowed_call_time(bound.step_deadline);
          if (!allowed)
            return xarm_control::XArmServiceCallResult{
                xarm_control::XArmServiceCallOutcome::kNotSent,
                "component_lifecycle_deadline_expired", std::nullopt};
          return services->SetTcpLoad(mass_kg, center_of_mass_m, *allowed,
                                      bound.cancellation);
        },
        // Enable compares the fresh reported offset before deciding whether to send.
        [port, &clock] { return port->ReportedTcpOffset(clock.now()); });
    std::lock_guard lock(mutex_);
    if (port_) throw std::logic_error("the xArm adapter starts once");
    port_ = std::move(port);
    services_ = std::move(services);
    tool_context_ = std::move(tool_context);
    clock_ = &clock;
  }

  [[nodiscard]] RetainedDeviceStatus Observe(const core::TimePoint now_ns) override {
    const auto port = started();
    return port ? device_status(port->Observe(now_ns)) : not_started();
  }

  [[nodiscard]] RetainedDeviceStatus
  ReadHealth(const core::TimePoint now_ns) const override {
    const auto port = started();
    return port ? device_status(port->ReadHealth(now_ns)) : not_started();
  }

  void BeginHardwareTransition(const bool activate) override {
    if (const auto port = started())
      port->BeginOwnPrimitive(command(activate));
  }

  [[nodiscard]] std::optional<std::string> VendorJointStateTopic() const override {
    return selection_.driver_namespace + "/joint_states";
  }

  [[nodiscard]] VendorJointSampleGate VendorJointSampleGateFn() const override {
    // Staged before Start: the port is read per sample, through a weak
    // reference (the subscription never keeps this adapter alive).
    return [weak = weak_from_this()](const core::TimePoint now_ns,
                                     const std::vector<double> &positions) {
      // V-A (A4, moved from the generic handle): every bound joint exactly
      // zero is the xArm controller's e-stop signature with the hardware
      // active, not a pose.
      if (std::all_of(positions.begin(), positions.end(),
                      [](const double value) { return value == 0.0; }))
        return VendorJointSampleUse::kIgnore;
      const auto adapter = weak.lock();
      const auto port = adapter ? adapter->started() : nullptr;
      return port && port->VendorJointSampleUsable(now_ns, positions)
                 ? VendorJointSampleUse::kLeads
                 : VendorJointSampleUse::kDisplay;
    };
  }

  [[nodiscard]] std::chrono::nanoseconds ActivationTransientBound() const override {
    return kXArmActivationTransientBound;
  }

  // V-A (A5, moved from the generic handle), D1: the plugin's activation
  // clears the controller error, so it never runs over an error the operator
  // has not acknowledged, nor on stale status.
  [[nodiscard]] core::Result<void> CheckArmable(const core::TimePoint now_ns) const override {
    const auto status = ReadHealth(now_ns);
    const auto error = std::find_if(
        status.facts.begin(), status.facts.end(), [](const auto &fact) {
          return fact.lifecycle_fact_id.value() == core::kDeviceErrorPresentFactId;
        });
    if (status.cause != core::LifecycleBlockingCauseClassDTO::kNone ||
        error == status.facts.end() || error->value.boolean_value != false)
      return core::Result<void>::Err(vendor_error(
          "ros2_control_device_not_armable",
          "the robot reports an error or no fresh status; arming is refused"));
    return core::Result<void>::Ok();
  }

  void ObserveHardwareActive(const bool hardware_active) override {
    if (const auto port = started())
      port->ObserveHardwareActive(hardware_active);
  }

  void SettleHardwareTransition(const bool activate, const bool effect_possible,
                                const core::TimePoint now_ns) override {
    if (const auto port = started())
      port->SettleOwnPrimitive(command(activate), effect_possible, now_ns);
  }

  [[nodiscard]] core::Result<void>
  CheckActivation(const core::TimePoint now_ns,
                  const std::vector<double> &measured_positions) const override {
    const auto port = started();
    if (!port)
      return core::Result<void>::Err(vendor_error(
          "xarm_status_unavailable", "the xArm adapter is not started"));
    return port->CheckActivationGate(now_ns, measured_positions);
  }

  // R7 D10: the operator's Recover clears the vendor error (first, M6
  // XADAPT), then the warning; one call each, never retried. A timeout or a
  // non-zero return is an unknown outcome that the next report reconciles,
  // except an answered `clean_error`, which the reports verify.
  [[nodiscard]] RetainedRos2ControlSwitchResult
  Dispatch(const core::LifecyclePrimitiveId &primitive,
           const std::optional<std::chrono::steady_clock::time_point> step_deadline,
           const std::stop_token cancellation) override {
    using xarm_control::XArmLifecycleCommand;
    const auto &id = primitive.value();
    std::optional<XArmLifecycleCommand> vendor_command;
    if (id == core::kDeviceClearErrorPrimitiveId)
      vendor_command = XArmLifecycleCommand::kClearError;
    else if (id == xarm_control::kXArmClearWarningPrimitiveId)
      vendor_command = XArmLifecycleCommand::kClearWarning;
    if (!vendor_command)
      return vendor_result(RetainedRos2ControlSwitchOutcome::kNotSent,
                           "ros2_control_lifecycle_primitive_unknown",
                           "the xArm catalog has no such primitive");
    std::shared_ptr<xarm_control::XArmBrandLifecyclePort> port;
    std::shared_ptr<const xarm_control::XArmDriverServices> services;
    {
      std::lock_guard lock(mutex_);
      port = port_;
      services = services_;
    }
    if (!port || !services)
      return vendor_result(RetainedRos2ControlSwitchOutcome::kNotSent,
                           "xarm_adapter_not_started",
                           "the xArm adapter is not started");
    if (*vendor_command == XArmLifecycleCommand::kClearError &&
        !port->ClearErrorPermitted())
      return vendor_result(RetainedRos2ControlSwitchOutcome::kNotSent,
                           "xarm_power_cycle_required",
                           "a servo fault needs a power cycle before its "
                           "error is cleared");
    std::optional<XArmClearFacts> before;
    if (*vendor_command == XArmLifecycleCommand::kClearError) {
      before.emplace();
      if (auto refused = ReadClearErrorWarning(*services, step_deadline, cancellation,
                                               false, *before))
        return *refused;
      if (auto refused = ReadClearState(*services, step_deadline, cancellation,
                                        false, *before))
        return *refused;
    }
    port->BeginOwnPrimitive(*vendor_command);
    // M6 XADAPT (D8, SX-695/699): allowed min(the call cap, the step's
    // remaining time), recomputed right at the send; none left, nothing is
    // sent and the own-primitive bookkeeping is released. No answer within
    // it is the connection lost.
    if (auto refused = refuse_if_expired_at_send(step_deadline, [&] {
          port->SettleOwnPrimitive(*vendor_command, false, clock_now());
        }))
      return *refused;
    const auto now = std::chrono::steady_clock::now();
    const auto timeout =
        step_deadline && *step_deadline < now + xarm_control::kXArmServiceResponseTimeout
            ? std::chrono::duration_cast<std::chrono::nanoseconds>(*step_deadline - now)
            : xarm_control::kXArmServiceResponseTimeout;
    const auto call = services->Call(*vendor_command, timeout, cancellation);
    const auto settled_at = clock_now();
    port->SettleOwnPrimitive(
        *vendor_command,
        call.outcome != xarm_control::XArmServiceCallOutcome::kNotSent,
        settled_at);
    // M6 XADAPT: an answered `clean_error` is verified on on-demand reads, also
    // with API code 1 or 2 ("controller has uncleared errors / warnings",
    // xarm_api_code.md): that return reports the error still present, not
    // a failed call.
    const bool answered_clear =
        *vendor_command == XArmLifecycleCommand::kClearError &&
        (call.outcome == xarm_control::XArmServiceCallOutcome::kSucceeded ||
         (call.vendor_return == 1 || call.vendor_return == 2));
    if (answered_clear)
      return VerifyClear(*services, *before, step_deadline, cancellation);
    switch (call.outcome) {
    case xarm_control::XArmServiceCallOutcome::kSucceeded:
      return {RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
    case xarm_control::XArmServiceCallOutcome::kNotSent:
      return vendor_result(RetainedRos2ControlSwitchOutcome::kNotSent,
                           call.reason, "the xArm service call was not sent");
    case xarm_control::XArmServiceCallOutcome::kUnknown:
      break;
    }
    // M6 XADAPT (D8): no answer within the time it was allowed: the robot
    // controller is unresponsive (vendor-neutral code).
    if (xarm_control::XArmCallUnanswered(call.reason))
      return {RetainedRos2ControlSwitchOutcome::kUncertain, unresponsive(call.reason)};
    return vendor_result(RetainedRos2ControlSwitchOutcome::kUncertain,
                         call.reason,
                         "the xArm service call's effect is unknown");
  }

  // M6 XADAPT: the latest report's vendor facts, or the first one received
  // after `received_after`, within one freshness window (5 Hz reports).
  [[nodiscard]] std::optional<std::vector<core::LifecycleFactDTO>>
  ReadReportFacts(const std::optional<core::TimePoint> received_after,
                  const std::stop_token cancellation) const override {
    const auto port = started();
    if (!port) return std::nullopt;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::nanoseconds(xarm_control::kXArmStatusFreshnessNs);
    for (;;) {
      auto facts = port->LatestFreshFacts(clock_now(), received_after);
      if (facts || !received_after || cancellation.stop_requested() ||
          std::chrono::steady_clock::now() >= deadline)
        return facts;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }

  // Event-driven tool context: each requested vendor call is sent once.
  [[nodiscard]] VendorCallResult
  SendToolContext(const roboplan_backend::VendorToolContextCommand &command, const bool at_enable,
                   const std::optional<std::chrono::steady_clock::time_point> step_deadline,
                   const std::stop_token cancellation) override {
    std::shared_ptr<xarm_control::XArmToolContextSender> sender;
    {
      std::lock_guard lock(mutex_);
      sender = tool_context_;
    }
    if (!sender)
      return {false, vendor_error("xarm_adapter_not_started", "the xArm adapter is not started")};
    std::optional<core::Pose3DTO> offset;
    if (const auto *set = std::get_if<roboplan_backend::VendorToolOffsetSet>(&command.offset))
      offset = set->tcp_pose_in_flange;
    else if (std::holds_alternative<roboplan_backend::VendorToolOffsetZero>(command.offset))
      offset = core::Pose3DTO{};
    std::optional<core::ToolContextPointMassDTO> payload;
    if (const auto *set = std::get_if<roboplan_backend::VendorToolLoadSet>(&command.load))
      payload = core::ToolContextPointMassDTO{set->mass_kg, set->center_of_mass_m};
    return vendor_call_result(sender->Apply(offset, payload, at_enable,
                                            {step_deadline, cancellation}));
  }

  // M6 ADAPTERM: the re-engage sequence (`xarm_control::ReengageAtRest`)
  // over the driver's `set_mode` / `set_state`, each bounded by the call cap. The
  // vendor plugin reactivates its controllers itself once the robot is ready
  // again; the trajectory controller holds the current pose (the plugin's
  // command follows the measured pose while the robot is not ready). An
  // error says whether a sent call may still take effect (`effect`).
  [[nodiscard]] VendorCallResult
  ReengageAtRest(const std::stop_token cancellation) override {
    std::shared_ptr<xarm_control::XArmBrandLifecyclePort> port;
    std::shared_ptr<const xarm_control::XArmDriverServices> services;
    {
      std::lock_guard lock(mutex_);
      port = port_;
      services = services_;
    }
    // Not started: no call was sent, but (as before V-A) a re-engage error
    // without `effect: not_started` leaves the effect possible.
    if (!port || !services)
      return {false, vendor_error("xarm_adapter_not_started", "the xArm adapter is not started"),
              true};
    const auto call = [](const xarm_control::XArmServiceCallResult &result) {
      return xarm_control::XArmReengageCallResult{
          result.outcome != xarm_control::XArmServiceCallOutcome::kNotSent,
          result.outcome == xarm_control::XArmServiceCallOutcome::kSucceeded,
          result.reason, result.vendor_return};
    };
    const auto result = xarm_control::ReengageAtRest(
        *port,
        {[&](const int mode) {
           return call(services->SetMode(
               mode, xarm_control::kXArmServiceResponseTimeout, cancellation));
         },
         [&](const int state) {
           return call(services->SetState(
               state, xarm_control::kXArmServiceResponseTimeout, cancellation));
         },
         [this] { return clock_now(); },
         [] { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }},
        cancellation);
    if (result.error)
      return {false, *result.error, result.effect_possible,
              result.error->code == xarm_control::kRobotControllerUnresponsiveCode};
    return {result.reengaged};
  }

  void EndRearm() override {
    if (const auto port = started()) port->EndRearm();
  }

  [[nodiscard]] core::Result<void>
  CheckReengagedAtRest(const core::TimePoint now_ns,
                       const std::vector<double> &measured_positions) const override {
    const auto port = started();
    if (!port)
      return core::Result<void>::Err(vendor_error(
          "xarm_status_unavailable", "the xArm adapter is not started"));
    return port->CheckReengagedAtRest(now_ns, measured_positions);
  }

private:
  [[nodiscard]] static std::optional<RetainedRos2ControlSwitchResult>
  ClearReadFailure(const xarm_control::XArmServiceCallResult &call,
                   const bool has_payload, const bool clear_sent,
                   const std::string_view phase, const std::string_view read_kind,
                   const std::string_view fallback) {
    const auto outcome = clear_sent ? RetainedRos2ControlSwitchOutcome::kUncertain
                                    : RetainedRos2ControlSwitchOutcome::kNotSent;
    const auto finish = [clear_sent](RetainedRos2ControlSwitchResult result) {
      if (result.error)
        result.error->details.emplace("clear_error_sent", clear_sent ? "true" : "false");
      return std::optional<RetainedRos2ControlSwitchResult>(std::move(result));
    };
    if (xarm_control::XArmCallUnanswered(call.reason))
      return finish({RetainedRos2ControlSwitchOutcome::kUncertain,
                     unresponsive(call.reason)});
    if (call.reason == "xarm_service_cancelled")
      return finish(vendor_result(
          outcome, "xarm_service_cancelled",
          std::string("the ") + std::string(phase) + "-clear " +
              std::string(read_kind) + " read was cancelled; clean_error " +
              (clear_sent ? "was sent" : "was not sent")));
    if (call.outcome != xarm_control::XArmServiceCallOutcome::kSucceeded ||
        !has_payload)
      return finish(vendor_result(
          outcome, call.reason.empty() ? std::string(fallback) : call.reason,
          std::string("the ") + std::string(phase) + "-clear " +
              std::string(read_kind) + " read failed; clean_error " +
              (clear_sent ? "was sent" : "was not sent")));
    return std::nullopt;
  }

  [[nodiscard]] static std::optional<RetainedRos2ControlSwitchResult>
  ClearReadDeadline(const bool clear_sent, const std::string_view phase,
                    const std::string_view what) {
    core::ErrorDTO error{
        "component_lifecycle_deadline_expired",
        std::string("no time was left for the ") + std::string(phase) +
            "-clear " + std::string(what),
        {{"reason", "deadline_exhausted"},
         {"clear_error_sent", clear_sent ? "true" : "false"}}};
    return RetainedRos2ControlSwitchResult{
        clear_sent ? RetainedRos2ControlSwitchOutcome::kUncertain
                   : RetainedRos2ControlSwitchOutcome::kNotSent,
        std::move(error)};
  }

  // DX-2987: read error and warning on demand. The clear baseline also reads
  // state; scheduled post-clear polls deliberately do not.
  [[nodiscard]] std::optional<RetainedRos2ControlSwitchResult>
  ReadClearErrorWarning(const xarm_control::XArmDriverServices &services,
                        const std::optional<std::chrono::steady_clock::time_point>
                            step_deadline,
                        const std::stop_token cancellation, const bool clear_sent,
                        XArmClearFacts &facts) const {
    const auto phase = clear_sent ? "after" : "before";
    const auto timeout = allowed_call_time(step_deadline);
    if (!timeout) return ClearReadDeadline(clear_sent, phase, "controller facts");
    const auto read = services.GetErrWarnCode(*timeout, cancellation);
    if (auto failed = ClearReadFailure(
            read.call, read.error.has_value() && read.warning.has_value(), clear_sent,
            phase, "error/warning",
            clear_sent ? "xarm_clear_verification_unread"
                       : "xarm_clear_before_read_failed"))
      return failed;
    facts.error = *read.error;
    facts.warning = *read.warning;
    return std::nullopt;
  }

  [[nodiscard]] std::optional<RetainedRos2ControlSwitchResult>
  ReadClearState(const xarm_control::XArmDriverServices &services,
                 const std::optional<std::chrono::steady_clock::time_point> step_deadline,
                 const std::stop_token cancellation, const bool clear_sent,
                 XArmClearFacts &facts) const {
    const auto phase = clear_sent ? "after" : "before";
    const auto timeout = allowed_call_time(step_deadline);
    if (!timeout) return ClearReadDeadline(clear_sent, phase, "controller state");
    const auto state = services.GetState(*timeout, cancellation);
    if (auto failed = ClearReadFailure(
            state.call, state.state.has_value(), clear_sent, phase, "state",
            clear_sent ? "xarm_clear_verification_unread"
                       : "xarm_clear_before_read_failed"))
      return failed;
    facts.state = *state.state;
    return std::nullopt;
  }

  // M6 XADAPT D12 / DX-2987: an answered own `clean_error` is verified by
  // on-demand error/warning reads at `kXArmClearVerifyReadsAtNs`. The first
  // error read of zero proves success immediately. If an error remains when
  // polling ends, read state once and compare all three facts. No report
  // snapshot or mode/axis-enable fact participates in this comparison.
  [[nodiscard]] RetainedRos2ControlSwitchResult
  VerifyClear(const xarm_control::XArmDriverServices &services,
              const XArmClearFacts &before,
              const std::optional<std::chrono::steady_clock::time_point> step_deadline,
              const std::stop_token cancellation) const {
    const auto persisted = [&](const XArmClearFacts &after) {
      // C1-C3 are the emergency stop button, the control box's emergency
      // input and the three-state switch's stop (xarm_api_code.md); C2
      // stays latched until cleared, so only the clear tells them apart.
      auto error = vendor_error(
          "xarm_error_persists_after_clear",
          after.error >= 1 && after.error <= 3
              ? "the emergency stop is still engaged (C" + std::to_string(after.error) +
                    "): release it and try again"
              : "the robot still reports its error after it was cleared");
      error.details.emplace("vendor_code", "C" + std::to_string(after.error));
      const auto outcome = after == before
                               ? RetainedRos2ControlSwitchOutcome::kFailedNoEffect
                               : RetainedRos2ControlSwitchOutcome::kFailedEffectObserved;
      // V-A (A2): decided by this adapter's own comparison.
      return RetainedRos2ControlSwitchResult{outcome, std::move(error), true};
    };
    const auto answered_at = std::chrono::steady_clock::now();
    std::optional<XArmClearFacts> last_facts;
    for (const auto offset : xarm_control::kXArmClearVerifyReadsAtNs) {
      if (cancellation.stop_requested()) break;
      if (step_deadline && std::chrono::steady_clock::now() >= *step_deadline)
        break;
      const auto at = answered_at + std::chrono::nanoseconds(offset);
      while (std::chrono::steady_clock::now() < at) {
        if (cancellation.stop_requested()) break;
        if (step_deadline && std::chrono::steady_clock::now() >= *step_deadline)
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      if (cancellation.stop_requested()) break;
      // Do not start another scheduled service read once the step has expired.
      // The last error/warning answer remains available, but its required
      // after-state may no longer fit the step budget.
      if (step_deadline && std::chrono::steady_clock::now() >= *step_deadline)
        break;
      XArmClearFacts after;
      if (auto failed = ReadClearErrorWarning(services, step_deadline, cancellation,
                                              true, after))
        return *failed;
      if (after.error == 0)
        return {RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
      last_facts = after;
    }
    if (cancellation.stop_requested())
      return vendor_result(RetainedRos2ControlSwitchOutcome::kUncertain,
                           "xarm_clear_verification_cancelled",
                           "the clear was sent; its verification was cancelled");
    if (last_facts) {
      if (auto failed = ReadClearState(services, step_deadline, cancellation,
                                       true, *last_facts))
        return *failed;
      return persisted(*last_facts);
    }
    // The clear was sent, but no time was left to read its result.
    return {RetainedRos2ControlSwitchOutcome::kUncertain,
            core::ErrorDTO{"component_lifecycle_deadline_expired",
                           "the clear was sent; no time was left to verify it",
                           {{"reason", "deadline_exhausted"}}}};
  }

  [[nodiscard]] std::shared_ptr<xarm_control::XArmBrandLifecyclePort>
  started() const {
    std::lock_guard lock(mutex_);
    return port_;
  }

  [[nodiscard]] core::TimePoint clock_now() const {
    std::lock_guard lock(mutex_);
    return clock_ != nullptr ? clock_->now() : 0;
  }

  [[nodiscard]] static xarm_control::XArmLifecycleCommand
  command(const bool activate) {
    return activate ? xarm_control::XArmLifecycleCommand::kArm
                    : xarm_control::XArmLifecycleCommand::kDisarm;
  }

  [[nodiscard]] static RetainedDeviceStatus not_started() {
    // The same unknown device facts as a stale report.
    struct NoReport final : xarm_control::IXArmBrandLifecycleTransport {
      [[nodiscard]] xarm_control::XArmObservedState observe() const override {
        return {};
      }
    };
    return device_status(
        xarm_control::XArmBrandLifecyclePort(std::make_shared<NoReport>())
            .ReadHealth(0));
  }

  const VendorAdapterSelection selection_;
  mutable std::mutex mutex_;
  std::shared_ptr<xarm_control::XArmBrandLifecyclePort> port_;
  std::shared_ptr<const xarm_control::XArmDriverServices> services_;
  std::shared_ptr<xarm_control::XArmToolContextSender> tool_context_;
  core::IClock *clock_{};
};

} // namespace

std::shared_ptr<IRetainedDeviceStatusAdapter>
make_xarm_device_adapter(const VendorAdapterSelection &selection) {
  return std::make_shared<XArmDeviceStatusAdapter>(selection);
}

roboplan_backend::VendorAdapterFamily xarm_vendor_adapter_family() {
  return {kXArmVendorAdapterFamily, &make_xarm_device_adapter,
          kXArmRos2ControlLifecycleProviderManifestId,
          &make_xarm_ros2_control_lifecycle_catalog,
          kXArmRos2ControlGripperLifecycleProviderManifestId};
}

} // namespace manyforge::xarm_control
