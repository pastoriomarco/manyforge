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
#include <vector>

#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/lifecycle_cause.hpp>
#include <manyforge_xarm_control/contract.hpp>

namespace manyforge::xarm_control {

/// Diagnostic vendor-code facts copied verbatim from the latest fresh vendor
/// report for incident records (addendum 2 `vendor_codes`). They are never
/// catalog predicate inputs. Rendering: a token is the code itself (`C2`), an
/// integer is `<last id segment>=<value>` (`mode=0`, `state=4`). Facts whose
/// value is not established by the report are omitted.
inline constexpr std::string_view kVendorCodeErrorFactId =
    "manyforge.vendor_code.error";
inline constexpr std::string_view kVendorCodeModeFactId =
    "manyforge.vendor_code.mode";
inline constexpr std::string_view kVendorCodePowerCycleFactId =
    "manyforge.vendor_code.power_cycle";
inline constexpr std::string_view kVendorCodeStateFactId =
    "manyforge.vendor_code.state";
inline constexpr std::string_view kVendorCodeWarningFactId =
    "manyforge.vendor_code.warning";
/// m5.5: the typed reason of a vendor condition ManyForge did not request or
/// could not clear (token): `xarm_error_persists_after_clear`,
/// `xarm_manual_mode` (mode 2), `xarm_mode_external` (another mode ManyForge
/// never selects), `xarm_paused` (state 3), `xarm_stopped` (state 4),
/// `xarm_mode_changed` (state 5), `xarm_stop_requested` (state 6, the
/// UFACTORY Studio stop) or `xarm_moved_externally`.
inline constexpr std::string_view kVendorCodeConditionFactId =
    "manyforge.vendor_code.condition";
/// Token value of `kVendorCodePowerCycleFactId` for servo faults C10-C17.
inline constexpr std::string_view kPowerCycleRequiredToken =
    "power_cycle_required";

/// R7 D3: `robot_states` arrives at 5 Hz (`report_type: rich`); a report older
/// than two lost periods is stale, and the status is then unavailable.
inline constexpr std::int64_t kXArmStatusFreshnessNs = 500'000'000;

/// R7 §3.6: the Enable gate's largest difference between a measured joint and
/// the vendor-reported angle of the same axis while stopped. It catches a
/// zero hold target against the real pose; per robot data, checked live.
inline constexpr double kXArmJointAgreementToleranceRad = 0.02;

/// M6 XADAPT D11: an answered own `clean_error` is verified by on-demand
/// `get_err_warn_code` reads at these offsets after the answer (3 reads,
/// 150 ms apart, 0.3 s in all; the controller answers the clear once it has
/// processed it, the retries cover a lag; to verify live). The first read
/// without the error proves it cleared; the error still read at the last
/// one proves it persists (the emergency stop or its input still engaged).
/// A later clear by itself is not that step's concern.
inline constexpr std::array<std::int64_t, 3> kXArmClearVerifyReadsAtNs{
    0LL, 150'000'000LL, 300'000'000LL};

/// M6 ADAPTERM: after `set_mode` and `set_state` returned, the robot reports
/// joint-servo mode and the ready state within this bound (5 Hz reports).
inline constexpr std::int64_t kXArmReengageReadyNs = 3'000'000'000LL;

/// M6 ADAPTERM: what an admitted ManyForge motion needs first after an
/// error-free move from the robot's own interface (UFACTORY Studio).
enum class XArmMotionReengage : std::uint8_t {
  /// No such move is latched: nothing to do.
  kNotNeeded,
  /// At rest, error-free, every axis enabled, armed: re-engage, then move.
  kNeeded,
  /// Latched, but not re-engaged here (`reason`): still moving, an error
  /// (an emergency stop is never re-enabled automatically), axes disabled,
  /// no fresh report or not armed.
  kRefused,
};

struct XArmMotionReengageDecision final {
  XArmMotionReengage kind{XArmMotionReengage::kNotNeeded};
  std::string reason;
};

/// m5.5 policy `external_move_rearm`: what an error-free move or mode/state
/// change from the robot's own interface (UFACTORY Studio) does while
/// ManyForge has the robot armed.
enum class XArmExternalMoveRearm : std::uint8_t {
  /// Reported as `device.moved_externally` (no intervention cause): on an
  /// idle cell the CCLC target driver re-arms the robot once it is at rest;
  /// during ManyForge work it is the `external_control` incident.
  kAutomatic,
  /// Always the `external_control` incident; the operator's Recover re-arms.
  kOperator,
};

/// Adapter data of one xArm-family robot (xArm 5/6/7, Lite 6, UF850).
struct XArmBrandLifecyclePortConfig {
  /// Message-age freshness of `robot_states`.
  std::int64_t status_freshness_ns = kXArmStatusFreshnessNs;
  /// Joint count; `mt_able` with every one of these bits set is all axes enabled.
  int axis_count = 6;
  /// The Enable gate's measured-versus-reported joint tolerance.
  double joint_agreement_tolerance_rad = kXArmJointAgreementToleranceRad;
  XArmExternalMoveRearm external_move_rearm = XArmExternalMoveRearm::kAutomatic;
};

/// The normalized status of one xArm: the common cause class plus the
/// `device.*` and `manyforge.vendor_code.*` facts (sorted by id). The cause is
/// `availability` while the latest report is stale or missing; the device
/// facts are then unknown and no vendor code is reported.
struct XArmDeviceStatus final {
  core::LifecycleBlockingCauseClassDTO cause{
      core::LifecycleBlockingCauseClassDTO::kAvailability};
  std::vector<core::LifecycleFactDTO> facts;

  friend bool operator==(const XArmDeviceStatus &,
                         const XArmDeviceStatus &) = default;
};

/// Bounded read seam of the vendor status; the production implementation is
/// the `robot_states` subscription, tests provide a deterministic one.
class IXArmBrandLifecycleTransport {
public:
  virtual ~IXArmBrandLifecycleTransport() = default;

  /// Copy of the latest received report; no wait and no effect.
  [[nodiscard]] virtual XArmObservedState observe() const = 0;
};

/// ROS-independent xArm vendor adapter over the unmodified driver's status
/// (R7 packages 3 and 4). It classifies the vendor report into the common
/// cause classes, reports the device facts and vendor codes, tracks
/// unrequested mode/state changes and checks the Enable gate. The ros2_control
/// motion domain merges its status; CCLC keeps sequencing, incidents and
/// retained causes.
class XArmBrandLifecyclePort final {
public:
  explicit XArmBrandLifecyclePort(
      std::shared_ptr<const IXArmBrandLifecycleTransport> transport,
      XArmBrandLifecyclePortConfig config = {});

  XArmBrandLifecyclePort(const XArmBrandLifecyclePort &) = delete;
  XArmBrandLifecyclePort &operator=(const XArmBrandLifecyclePort &) = delete;

  /// Classifies the latest report and commits the vendor history (the
  /// lifecycle Observe checkpoint).
  [[nodiscard]] XArmDeviceStatus Observe(std::int64_t now_ns);

  /// The same classification on a copy of the vendor history (pure health).
  [[nodiscard]] XArmDeviceStatus ReadHealth(std::int64_t now_ns) const;

  /// An own vendor operation explains the mode/state change it causes while
  /// in flight. Dispatched only after the operator acknowledged an
  /// intervention, it also re-baselines unrequested-change detection from the
  /// latest fresh report.
  void BeginOwnPrimitive(XArmLifecycleCommand command);
  /// Ends the in-flight operation: with a possible effect it keeps explaining
  /// for a bounded settle window, otherwise it explains nothing further.
  void SettleOwnPrimitive(XArmLifecycleCommand command, bool effect_possible,
                          std::int64_t now_ns);

  /// M6: whether a sample of the vendor driver's own joint states
  /// (`<driver ns>/joint_states`, published from every hardware read in any
  /// mode) may stand in for the joint state broadcaster's: only while the
  /// robot reports the move from its own interface (`device.moved_externally`,
  /// which the broadcaster does not survive) or ManyForge's own re-arm is in
  /// progress (until the handle's rebind), on a fresh report without an
  /// error. Never for Prepare or the Enable gate. `positions` all exactly zero
  /// is the controller's emergency-stop signature (R7 §3.5) and is refused.
  [[nodiscard]] bool VendorJointSampleUsable(std::int64_t now_ns,
                                             const std::vector<double> &positions) const;

  /// m5.5: the hardware interface's observed activity (the controller
  /// manager's list). Outside ManyForge's own activation and its settle
  /// window it is the armed state: a sent-but-uncertain deactivation stays
  /// armed until the hardware is observed inactive.
  void ObserveHardwareActive(bool hardware_active);

  /// The latest report's error code (-1 before any report), for a typed
  /// failure's vendor code.
  [[nodiscard]] int LatestErrorCode() const;
  /// m5.5, pure: the device facts and vendor codes `Observe` publishes for
  /// the latest report, or none when it is not fresh at `now_ns` (M6
  /// XADAPT: or not received after `received_after_ns`, when given).
  [[nodiscard]] std::optional<std::vector<core::LifecycleFactDTO>>
  LatestFreshFacts(std::int64_t now_ns,
                   std::optional<std::int64_t> received_after_ns = std::nullopt) const;
  /// Servo faults C10-C17 need a power cycle; clearing the error must not
  /// precede it (R7 verification list).
  [[nodiscard]] bool ClearErrorPermitted() const;

  /// M6 ADAPTERM, pure: whether an admitted motion must re-engage the robot
  /// first (the `device.moved_externally` latch), and whether it may.
  [[nodiscard]] XArmMotionReengageDecision
  MotionReengageDecision(std::int64_t now_ns) const;
  /// M6 ADAPTERM, pure: a fresh report received after `since_ns` shows
  /// joint-servo mode and the ready state (never the moving state), no error
  /// and every axis enabled.
  [[nodiscard]] bool ReportsServoReady(std::int64_t now_ns,
                                       std::int64_t since_ns,
                                       std::optional<std::array<double, 6>> *offset = nullptr) const;
  /// M6 ADAPTERM (item 44): the controller's reported TCP offset (mm, rad)
  /// from the latest fresh report; absent without one.
  [[nodiscard]] std::optional<std::array<double, 6>>
  ReportedTcpOffset(std::int64_t now_ns) const;
  /// M6 ADAPTERM: ManyForge's own re-arm (the re-engage, or a TCP offset
  /// send and its re-arm) is in progress: reported as
  /// `device.rearm_in_progress` from its first vendor step until the handle
  /// has bound the reactivated controller (`EndRearm`), so that the
  /// controllers the vendor plugin switches off and on are never a loss.
  void BeginRearm();
  void EndRearm();
  [[nodiscard]] bool RearmInProgress() const;

  /// M6 ADAPTERM, pure: the Enable gate (`CheckActivationGate`) and a latest
  /// report that is not the moving state: the arm is at rest by its own
  /// report (`xarm_in_motion` otherwise).
  [[nodiscard]] core::Result<void>
  CheckReengagedAtRest(std::int64_t now_ns,
                       const std::vector<double> &measured_positions_rad) const;

  /// The vendor part of the Enable gate (R7 §3.6), pure: a fresh report with
  /// no error that accepts commands, and every measured joint (axis order,
  /// already fresh and finite) within the tolerance of the reported angle of
  /// the same axis. A refusal is `execution_rejected` with a `reason`.
  [[nodiscard]] core::Result<void>
  CheckActivationGate(std::int64_t now_ns,
                      const std::vector<double> &measured_positions_rad) const;

private:
  /// One vendor mode/state pair; -1 is unknown.
  struct VendorControlPair final {
    int mode = -1;
    int state = -1;
  };

  /// The own primitive whose expected vendor effect explains a mode/state
  /// change while it is in flight or for a bounded settle window after it
  /// returned, until a report shows its final expected pair.
  struct OwnPrimitiveExpectation final {
    XArmLifecycleCommand command;
    VendorControlPair base;
    std::optional<std::int64_t> completed_at_ns;
  };

  struct VendorTrackingState final {
    std::optional<VendorControlPair> explained_pair;
    std::optional<VendorControlPair> last_reported_pair;
    std::optional<OwnPrimitiveExpectation> own_expectation;
    bool external_control_detected = false;
    // Vendor error code of the latest fresh report; -1 when unknown.
    int last_reported_error_code = -1;
    // m5.5: whether the robot is armed (its hardware interface active). Set
    // by ManyForge's own activation; otherwise reconstructed from the
    // observed hardware state (`ObserveHardwareActive`), never from an own
    // deactivation whose effect may be uncertain. Unknown (a new adapter
    // before the first hardware observation) is treated as armed. While
    // unarmed, a vendor error or an external change is facts only: Prepare
    // clears it.
    std::optional<bool> armed;
    // m5.5: an own `clean_error` returned at this time; a report this long
    // after it that still shows the error is `error_persists_after_clear`.
    std::optional<std::int64_t> clear_settled_at_ns;
    bool error_persists_after_clear = false;
    // m5.5: an error-free external change while armed (automatic policy),
    // until ManyForge's own arming completes.
    bool moved_externally = false;
    // M6 ADAPTERM: ManyForge's own re-arm in progress (`BeginRearm`).
    bool rearm_pending = false;
  };

  void TrackVendorControl(const XArmObservedState &observed,
                          std::int64_t now_ns,
                          VendorTrackingState &tracking) const;
  [[nodiscard]] XArmDeviceStatus Classify(std::int64_t now_ns,
                                          VendorTrackingState &tracking) const;

  std::shared_ptr<const IXArmBrandLifecycleTransport> transport_;
  XArmBrandLifecyclePortConfig config_;
  mutable std::mutex mutex_;
  VendorTrackingState tracking_;
};

/// M6 ADAPTERM: one vendor call of the re-engage: sent (it may have had an
/// effect) and confirmed (`ret == 0`), with the typed reason otherwise.
struct XArmReengageCallResult final {
  bool sent{};
  bool confirmed{};
  std::string reason;
  std::optional<int> vendor_return;
};

/// M6 ADAPTERM: the re-engage's two vendor calls and its clock.
struct XArmReengageCalls final {
  std::function<XArmReengageCallResult(int mode)> set_mode;
  std::function<XArmReengageCallResult(int state)> set_state;
  std::function<std::int64_t()> now;
  /// Between status polls of the ready wait (20 ms in production).
  std::function<void()> pause;
};

/// M6 ADAPTERM: the re-engage's outcome. Not re-engaged and no error:
/// nothing was latched. With an error, `effect_possible` tells whether a
/// sent call may still change the robot's mode or state (a timeout, a late
/// or non-zero reply): the caller must then re-establish the controller
/// binding before the next motion, whatever the latch says by then.
struct XArmReengageResult final {
  bool reengaged{};
  std::optional<core::ErrorDTO> error;
  bool effect_possible{};
  /// Offset from the exact ready report following a TCP offset send.
  std::optional<std::array<double, 6>> ready_offset{};
};

/// M6 ADAPTERM: an admitted motion after an error-free move from the robot's
/// own interface on an idle cell. At rest, as ManyForge's own arming (never
/// external control): `set_mode(1)`, then `set_state(0)`, each once, never
/// retried; then a fresh report showing joint-servo mode and the ready state
/// within `kXArmReengageReadyNs`, which ends the move latch.
[[nodiscard]] XArmReengageResult
ReengageAtRest(XArmBrandLifecyclePort &port, const XArmReengageCalls &calls,
               std::stop_token cancellation);

/// M6 ADAPTERM TOOLCTX: one `set_tcp_offset` and the re-arm it needs.
struct XArmTcpOffsetCalls final {
  std::function<XArmReengageCallResult()> set_tcp_offset;
  std::function<XArmReengageCallResult(int state)> set_state;
  std::function<std::int64_t()> now;
  std::function<void()> pause;
};

/// M6 ADAPTERM TOOLCTX (live 2026-10-28): from ready, `set_tcp_offset`
/// applies but the controller drops to state 5 and stays there; only
/// `set_state(0)` restores ready. As ManyForge's own arming (never external
/// control, `device.rearm_in_progress` until the caller's rebind): the
/// offset, then `set_state(0)`, each once, never retried; then a fresh ready
/// report within `kXArmReengageReadyNs`. Errors: `tool_context_not_applied`
/// (`tool_context_field: tcp_offset`) for the offset, then the re-engage's
/// `xarm_reengage_not_confirmed` / `xarm_reengage_not_ready`; each with
/// `effect`. `reengaged` true: sent and ready again.
/// M6b (SX-727): `step_deadline`, at Enable only (a lifecycle step), also
/// ends the ready wait: min(`kXArmReengageReadyNs`, the step's remaining
/// time). Ended there, the offset was sent: `component_lifecycle_deadline_expired`
/// with reason `xarm_reengage_not_ready` and `effect: possible` (uncertain,
/// no fault). Before a motion: none, the fixed ready wait.
[[nodiscard]] XArmReengageResult
SendTcpOffsetAtRest(XArmBrandLifecyclePort &port, const XArmTcpOffsetCalls &calls,
                    std::stop_token cancellation,
                    std::optional<std::chrono::steady_clock::time_point> step_deadline =
                        std::nullopt);

} // namespace manyforge::xarm_control
