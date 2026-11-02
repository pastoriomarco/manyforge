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

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <manyforge_core/ports/icomponent_adapter_port.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_xarm_control/contract.hpp>
#include <manyforge_xarm_control/xarm_arm_adapter.hpp>
#include <manyforge_xarm_control/xarm_fault.hpp>
#include <manyforge_xarm_control/xarm_io_component.hpp>

namespace manyforge::xarm_control {

/// Offline reasons of the power actions (CONTRACTS §1 `reason`).
inline constexpr std::string_view kShutdownByOperatorReason = "shutdown_by_operator";
inline constexpr std::string_view kRebootingReason = "component_rebooting";

/// The member roles of an xArm machine (CONTRACTS §16).
inline constexpr std::string_view kXArmArmRole = "arm";
inline constexpr std::string_view kXArmControllerIoRole = "controller_io";
inline constexpr std::string_view kXArmToolIoRole = "tool_io";

/// Receives each I/O member's updates of one stream sample (the host
/// publishes them to the core as one acquisition of that component).
using XArmIoUpdateSink = std::function<void(const std::string &component_id,
                                            const std::vector<core::IoSignalUpdateDTO> &updates,
                                            std::int64_t received_at_ns)>;

/// What the owner adapter runs on, all owned by the execution host.
struct XArmOwnerAdapterPorts final {
  /// The driver's `robot_states`.
  std::shared_ptr<const IXArmStatusSource> status;
  std::shared_ptr<const IXArmDriver> driver;
  /// The ros2_control lane adapter of the machine's connection: read for its
  /// process state (offline, process exited) only.
  std::shared_ptr<core::IComponentAdapter> lane;
  /// Restarts the machine's process (the driver doesn't reconnect by itself
  /// after a shutdown or reboot of the controller).
  std::function<core::Result<void>(std::stop_token)> restart_driver;
  /// Whether the controller accepts a connection on its command port now:
  /// one probe, bounded by `reboot_probe_interval`.
  std::function<bool()> controller_reachable;
  /// Waits this long (production: sleeps).
  std::function<void(std::chrono::nanoseconds)> wait;
  core::IClock *clock{};
  /// Where the I/O members' stream updates go.
  XArmIoUpdateSink io_sink;
  /// Reboot: the longest wait for the controller to go down and answer again
  /// (`vendor/manyforge_settings.yaml`, `reboot.controller_wait_s`).
  std::chrono::nanoseconds reboot_wait{};
  /// Reboot: the interval between probes (`reboot.probe_interval_ms`).
  std::chrono::nanoseconds reboot_probe_interval{};
  /// Prepare's bound, from its start, for the first report after a driver
  /// restart: the component's lifecycle command timeout the host passes
  /// (`lifecycle_command_timeout_ns`).
  std::chrono::nanoseconds lifecycle_command_timeout{};
  /// Whether the machine's launch is still starting (its process runs and its
  /// controller spawners have not all finished, within its launch bound).
  /// Prepare then waits for the driver's first report instead of restarting
  /// it (CONTRACTS §6: a driver restart is for a reboot or shutdown). Empty:
  /// never settling.
  std::function<bool()> launch_settling;
};

/// After an answered `clean_error`, `get_err_warn_code` is read at these
/// offsets (M7: three reads 150 ms apart); the first read without the error
/// shows it cleared, the error still read at the last one shows it stays
/// (an e-stop still pressed).
inline constexpr std::array<std::chrono::nanoseconds, 3> kXArmClearReadsAt{
    std::chrono::milliseconds(0), std::chrono::milliseconds(150),
    std::chrono::milliseconds(300)};

/// After the clear is read back, `robot_states` is looked at this often
/// until it shows the error gone too (within the lifecycle command timeout).
inline constexpr std::int64_t kXArmClearedReportPollNs = 50'000'000;

/// One member registered with its machine at its factory call.
struct XArmMember final {
  std::string role;
  std::string component_id;
  /// The member's adapter; expired once the member left the cell.
  std::weak_ptr<core::IComponentAdapter> adapter;
  /// An I/O member's stream half; empty for the arm.
  std::weak_ptr<XArmIoComponent> io;
  /// The arm member's own adapter (its external control); empty otherwise.
  std::weak_ptr<XArmArmAdapter> arm;
};

/// The owner of an xArm machine (CONTRACTS §16; xArm 5/6/7, Lite 6, UF850):
/// the connection, the driver process, the controller's report and error
/// register, and the power actions. Its members (arm, controller I/O, tool
/// I/O) register with it and read the controller through it.
///
/// - `prepare`: when it isn't connected (no fresh report) or was shut down
///   or rebooted by the operator, restart the machine's process and wait for
///   its first report (at most `lifecycle_command_timeout` from the start);
///   then `clean_error`, read back three times 150 ms apart (M7), then the
///   wait (same bound) for a `robot_states` report without the error (the
///   state is derived from it, so a member's Prepare right after sees the
///   owner prepared), and, with a warning left, `clean_warn`; never `motion_enable` (XPW-1: the motors stay
///   off until the arm's Enable). An error that stays (an e-stop still
///   pressed) fails the action; a servo error C10-C17 is not cleared (power
///   cycle). An arm under external control is released (its Enable re-arms
///   it).
/// - `recover`: the same (the reconnect when the driver connection is lost).
/// - `power_off` (the core disabled the members first, CONTRACTS §2, §16):
///   `system_control(1)`: `offline` with reason `shutdown_by_operator` (not
///   a fault) until a later `prepare`.
/// - `reboot` (members disabled first by the core): `system_control(2)`, the
///   wait for the controller, then `prepare`; `offline` with reason
///   `rebooting` meanwhile.
/// - No `enable`/`disable` of its own.
///
/// State: the lane's process state; a stale `robot_states` is
/// `fault_communication`; a controller error is its catalogue fault
/// (`xarm_fault`); the GPIO module's state or error code (rich report) is
/// `fault_communication`; external control of the arm (M7's rule,
/// `XArmArmAdapter::ExternalControl`) is `fault_external_control`; each with
/// the members it `concerns` (`xarm_fault_scope`: C33 and the GPIO module the
/// controller I/O, C19 the tool I/O, the arm's own errors and external
/// control the arm, anything else every member). `details` (display only):
/// the report's mode, state, error, warning and motors enabled. Trust the
/// controller: no retries and no verification loops; each vendor call is
/// sent once.
class XArmOwnerAdapter final : public core::IComponentAdapter {
public:
  explicit XArmOwnerAdapter(XArmOwnerAdapterPorts ports);

  [[nodiscard]] core::ComponentState State(std::int64_t now_ns) const override;
  [[nodiscard]] core::ActionResult Run(core::ComponentAction action,
                                       std::stop_token stop) override;
  [[nodiscard]] std::vector<core::Capability> capabilities() const override;

  /// A member registers at its factory call; a member rebuilt later
  /// replaces its earlier registration (same `component_id`).
  void AddMember(XArmMember member);
  /// Usable for its members: prepared, connected (a fresh report) and not
  /// deliberately offline.
  [[nodiscard]] bool Usable(std::int64_t now_ns) const;

  /// One `xarm_io_states` sample, handed to every I/O member.
  void OnIoStates(const XArmIoStatesSample &sample);
  /// One rich report's GPIO module status: the owner's health, the I/O
  /// members' pin configuration.
  void OnGpioModuleStatus(const XArmGpioModuleStatus &status);

  [[nodiscard]] const std::shared_ptr<const IXArmStatusSource> &status() const noexcept {
    return ports_.status;
  }
  [[nodiscard]] const std::shared_ptr<const IXArmDriver> &driver() const noexcept {
    return ports_.driver;
  }

private:
  /// Prepare and Recover: reconnect when needed, clear the controller error
  /// and its warning, release external control.
  [[nodiscard]] core::ActionResult Clear(core::ComponentAction action, std::stop_token stop);
  [[nodiscard]] core::ActionResult PowerAction(core::ComponentAction action, std::stop_token stop);
  /// Not connected or deliberately offline: restart the machine's process
  /// and wait for its first report; none on success.
  [[nodiscard]] std::optional<core::ActionResult> Connect(core::ComponentAction action,
                                                          std::stop_token stop);
  /// `clean_error` and its reads until one shows the error gone; none on
  /// success, with the warning code read last in `warning`.
  [[nodiscard]] std::optional<core::ActionResult> ClearError(core::ComponentAction action,
                                                             std::stop_token stop,
                                                             int &warning);
  /// After the clear and the release of external control: waits until the
  /// owner's own state shows no fault (report error, staleness, GPIO module
  /// error, external control), until `deadline_ns`; none once it does, else
  /// the remaining fault, typed.
  [[nodiscard]] std::optional<core::ActionResult>
  AwaitClearedReport(core::ComponentAction action, std::stop_token stop, std::int64_t deadline_ns);
  [[nodiscard]] std::vector<XArmMember> Members() const;
  /// The ids of the members a fault concerns.
  [[nodiscard]] std::vector<std::string> Concerns(XArmFaultScope scope) const;
  [[nodiscard]] bool Fresh(const XArmObservedState &observed, std::int64_t now_ns) const;
  [[nodiscard]] std::int64_t Now() const;

  const XArmOwnerAdapterPorts ports_;
  mutable std::mutex mutex_;
  std::vector<XArmMember> members_;
  /// A successful prepare; cleared by the power actions.
  bool prepared_{false};
  std::optional<core::ComponentStateReason> reason_;
  std::optional<XArmGpioModuleStatus> gpio_;
  /// When the fault currently reported first appeared (code and vendor code).
  mutable std::optional<std::pair<std::string, std::int64_t>> fault_since_;
};

} // namespace manyforge::xarm_control
