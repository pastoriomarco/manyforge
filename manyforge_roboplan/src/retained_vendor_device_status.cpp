#include <manyforge_roboplan/retained_vendor_device_status.hpp>

#include <cmath>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include <manyforge_xarm_control/lifecycle_profile.hpp>
#include <manyforge_xarm_control/xarm_brand_lifecycle_port.hpp>
#include <manyforge_xarm_control/xarm_driver_services.hpp>
#include <manyforge_xarm_control/xarm_robot_states_transport.hpp>

namespace manyforge::roboplan_backend {
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

[[nodiscard]] RetainedRos2ControlSwitchResult
vendor_result(const RetainedRos2ControlSwitchOutcome outcome,
              std::string reason, std::string message) {
  return {outcome, vendor_error(std::move(reason), std::move(message))};
}

/// The xArm adapter (manyforge_xarm_control) over the driver's `robot_states`.
class XArmDeviceStatusAdapter final : public IRetainedDeviceStatusAdapter {
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
    std::lock_guard lock(mutex_);
    if (port_) throw std::logic_error("the xArm adapter starts once");
    port_ = std::move(port);
    services_ = std::move(services);
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

  // R7 D10: the operator's Recover clears the vendor error, then the warning;
  // one call each, never retried. A timeout or a non-zero return is an
  // unknown outcome that the next report reconciles.
  [[nodiscard]] RetainedRos2ControlSwitchResult
  Dispatch(const core::LifecyclePrimitiveId &primitive,
           const std::stop_token cancellation) override {
    using xarm_control::XArmLifecycleCommand;
    const auto &id = primitive.value();
    std::optional<XArmLifecycleCommand> vendor_command;
    if (id == xarm_control::kXArmClearErrorPrimitiveId)
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
    port->BeginOwnPrimitive(*vendor_command);
    const auto call = services->Call(
        *vendor_command, xarm_control::kXArmServiceResponseTimeout,
        cancellation);
    const auto settled_at = clock_now();
    port->SettleOwnPrimitive(
        *vendor_command,
        call.outcome != xarm_control::XArmServiceCallOutcome::kNotSent,
        settled_at);
    switch (call.outcome) {
    case xarm_control::XArmServiceCallOutcome::kSucceeded:
      if (*vendor_command == XArmLifecycleCommand::kClearError)
        return VerifyClear(*port, settled_at, cancellation);
      return {RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
    case xarm_control::XArmServiceCallOutcome::kNotSent:
      return vendor_result(RetainedRos2ControlSwitchOutcome::kNotSent,
                           call.reason, "the xArm service call was not sent");
    case xarm_control::XArmServiceCallOutcome::kUnknown:
      break;
    }
    return vendor_result(RetainedRos2ControlSwitchOutcome::kUncertain,
                         call.reason,
                         "the xArm service call's effect is unknown");
  }

  // D11: `set_tcp_load` once, never retried; a timeout or a vendor failure
  // refuses the Enable (the switch is not sent).
  [[nodiscard]] RetainedRos2ControlSwitchResult
  ApplyToolPayload(const std::stop_token cancellation) override {
    if (!selection_.tool_payload)
      return {RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
    std::shared_ptr<const xarm_control::XArmDriverServices> services;
    {
      std::lock_guard lock(mutex_);
      services = services_;
    }
    if (!services)
      return vendor_result(RetainedRos2ControlSwitchOutcome::kNotSent,
                           "xarm_adapter_not_started",
                           "the xArm adapter is not started");
    const auto call = services->SetTcpLoad(
        selection_.tool_payload->mass_kg,
        selection_.tool_payload->center_of_mass_m,
        xarm_control::kXArmServiceResponseTimeout, cancellation);
    if (call.outcome == xarm_control::XArmServiceCallOutcome::kSucceeded)
      return {RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
    return vendor_result(RetainedRos2ControlSwitchOutcome::kNotSent,
                         call.reason,
                         "the robot did not confirm the tool payload");
  }

private:
  // m5.5: an own `clean_error` succeeds only once a report shows the error
  // cleared. Still present `kXArmClearVerifyNs` after the call (the e-stop
  // or its input is still open), the step fails typed with the vendor code:
  // a known failure, never an incident or a latch.
  [[nodiscard]] RetainedRos2ControlSwitchResult
  VerifyClear(const xarm_control::XArmBrandLifecyclePort &port,
              const core::TimePoint settled_at,
              const std::stop_token cancellation) const {
    using xarm_control::XArmClearVerification;
    while (!cancellation.stop_requested()) {
      switch (port.VerifyOwnClear(settled_at)) {
      case XArmClearVerification::kCleared:
        return {RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
      case XArmClearVerification::kPersists: {
        auto error = vendor_error(
            "xarm_error_persists_after_clear",
            "the robot still reports its error after it was cleared (is the "
            "emergency stop or its input still engaged?)");
        if (const auto code = port.LatestErrorCode(); code > 0)
          error.details.emplace("vendor_code", "C" + std::to_string(code));
        return {RetainedRos2ControlSwitchOutcome::kFailed, std::move(error)};
      }
      case XArmClearVerification::kPending:
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return vendor_result(RetainedRos2ControlSwitchOutcome::kUncertain,
                         "xarm_clear_verification_cancelled",
                         "the clear was sent; its verification was cancelled");
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
  core::IClock *clock_{};
};

} // namespace

std::shared_ptr<IRetainedDeviceStatusAdapter>
make_retained_vendor_device_status_adapter(
    const VendorAdapterSelection &selection) {
  if (selection.vendor == "xarm")
    return std::make_shared<XArmDeviceStatusAdapter>(selection);
  throw std::invalid_argument("no vendor adapter for '" + selection.vendor + "'");
}

} // namespace manyforge::roboplan_backend
