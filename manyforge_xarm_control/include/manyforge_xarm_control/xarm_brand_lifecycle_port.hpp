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

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
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

/// m5.5: an own `clean_error` is verified on the reports after it returned:
/// a report without the error proves it cleared; one this long after it that
/// still shows the error proves it persists (the emergency stop or its input
/// is still open). The own-primitive settle window, 5 s.
inline constexpr std::int64_t kXArmClearVerifyNs = 5'000'000'000LL;

/// m5.5: the verification of an own `clean_error` (`VerifyOwnClear`).
enum class XArmClearVerification : std::uint8_t { kPending, kCleared, kPersists };

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

  /// m5.5: the hardware interface's observed activity (the controller
  /// manager's list). Outside ManyForge's own activation and its settle
  /// window it is the armed state: a sent-but-uncertain deactivation stays
  /// armed until the hardware is observed inactive.
  void ObserveHardwareActive(bool hardware_active);

  /// m5.5, pure: the latest report after an own `clean_error` returned at
  /// `settled_at_ns` shows the error cleared, still present
  /// `kXArmClearVerifyNs` later, or neither yet (no such report).
  [[nodiscard]] XArmClearVerification VerifyOwnClear(std::int64_t settled_at_ns) const;
  /// The latest report's error code (-1 before any report), for a typed
  /// failure's vendor code.
  [[nodiscard]] int LatestErrorCode() const;

  /// Servo faults C10-C17 need a power cycle; clearing the error must not
  /// precede it (R7 verification list).
  [[nodiscard]] bool ClearErrorPermitted() const;

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

} // namespace manyforge::xarm_control
