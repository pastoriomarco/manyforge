#pragma once

// V-A (merge 7): ManyForge's generic vendor device adapter interface, shared
// by the Real-lane runtime handle (manyforge_roboplan) and every vendor
// adapter library (manyforge_xarm_control). Header-only, Core types and the
// standard library only: a vendor library includes it without linking
// RoboPlan, and RoboPlan reaches a vendor only through
// `VendorAdapterFamily` rows (`retained_vendor_device_status.hpp`).

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/lifecycle_cause.hpp>
#include <manyforge_core/types/provider_manifest.hpp>
#include <manyforge_core/types/tool_context.hpp>

namespace rclcpp {
class Node;
} // namespace rclcpp

namespace manyforge::roboplan_backend {

/// Outcome of one CCLC controller switch primitive.
enum class RetainedRos2ControlSwitchOutcome {
  /// Nothing was sent (refused before effect); see the error.
  kNotSent,
  /// The controller manager answered `ok`.
  kSucceeded,
  /// The controller manager answered not `ok`.
  kFailed,
  /// Sent, and a fresh report proved it changed nothing (m5.5: a vendor
  /// clear whose error persists; M6 XADAPT: a refused Real arm step whose
  /// next vendor report and controller activity are unchanged). A known
  /// failure with no effect.
  kFailedNoEffect,
  /// M6 XADAPT: a refused Real arm step after which the queried status
  /// changed, or a step whose component did not answer in time (the
  /// unresponsive fault it raises is the observed change). A known failure
  /// with an observed (partial) effect.
  kFailedEffectObserved,
  /// Sent, but cancelled before an answer arrived.
  kUncertain,
};

struct RetainedRos2ControlSwitchResult final {
  RetainedRos2ControlSwitchOutcome outcome{
      RetainedRos2ControlSwitchOutcome::kNotSent};
  std::optional<core::ErrorDTO> error{};
  /// V-A (A2): set only by a vendor adapter's `Dispatch`: its own comparison
  /// decided this outcome. An uncertain one then stands as it is, with no
  /// status query after the step (DX-2967/2969: the generic query cannot
  /// complete the adapter's comparison); a definite one is still checked
  /// against the queried status (SX-664).
  bool adapter_decided{false};
};

/// M6 XADAPT (SX-695/699): the recheck right at a send, after the caller's
/// own-primitive bookkeeping. With no time left before `deadline` it calls
/// `release` (the bookkeeping, as a send that never happened) and returns the
/// refusal `component_lifecycle_deadline_expired`: nothing sent, no fault.
[[nodiscard]] inline std::optional<RetainedRos2ControlSwitchResult>
refuse_if_expired_at_send(const std::optional<std::chrono::steady_clock::time_point> &deadline,
                          const std::function<void()> &release) {
  if (!deadline || std::chrono::steady_clock::now() < *deadline) return std::nullopt;
  if (release) release();
  return RetainedRos2ControlSwitchResult{
      RetainedRos2ControlSwitchOutcome::kNotSent,
      core::ErrorDTO{"component_lifecycle_deadline_expired",
                     "the lifecycle deadline expired before dispatch",
                     {{"reason", "deadline_exhausted"}}}};
}

/// Active TCP (M7): the explicit forms of one tool-context call (see
/// `IRetainedDeviceStatusAdapter::SendToolContext`). Poses and loads are in
/// the robot's flange frame, SI units.
struct VendorToolOffsetKeep final {
  friend bool operator==(const VendorToolOffsetKeep &, const VendorToolOffsetKeep &) = default;
};
struct VendorToolOffsetSet final {
  core::Pose3DTO tcp_pose_in_flange;
  friend bool operator==(const VendorToolOffsetSet &, const VendorToolOffsetSet &) = default;
};
struct VendorToolOffsetZero final {
  friend bool operator==(const VendorToolOffsetZero &, const VendorToolOffsetZero &) = default;
};
using VendorToolOffsetCommand =
    std::variant<VendorToolOffsetKeep, VendorToolOffsetSet, VendorToolOffsetZero>;
struct VendorToolLoadKeep final {
  friend bool operator==(const VendorToolLoadKeep &, const VendorToolLoadKeep &) = default;
};
struct VendorToolLoadSet final {
  double mass_kg{};
  std::array<double, 3> center_of_mass_m{};
  friend bool operator==(const VendorToolLoadSet &, const VendorToolLoadSet &) = default;
};
using VendorToolLoadCommand = std::variant<VendorToolLoadKeep, VendorToolLoadSet>;
struct VendorToolContextCommand final {
  VendorToolOffsetCommand offset{VendorToolOffsetKeep{}};
  VendorToolLoadCommand load{VendorToolLoadKeep{}};
  friend bool operator==(const VendorToolContextCommand &,
                         const VendorToolContextCommand &) = default;
};

/// R7 packages 3 and 4: the vendor adapter that observes a Real component's
/// own unmodified driver status and serves its vendor primitives, Enable
/// gate and tool context. Absent for Mock and Isaac, which ignore the tool
/// context.
struct VendorAdapterSelection final {
  /// The adapter family: V-B4, the Deployment row's `adapter_family`
  /// (e.g. "xarm" for every xArm-family robot), never implied by the profile.
  std::string vendor;
  /// The absolute namespace of the component's vendor driver.
  std::string driver_namespace;
  /// The robot's joint count.
  int axis_count{};
  /// R7 §3.6: the Enable gate's largest measured-versus-reported joint
  /// difference while stopped (per robot data).
  double joint_agreement_tolerance_rad{};

  friend bool operator==(const VendorAdapterSelection &,
                         const VendorAdapterSelection &) = default;
};

/// R7 package 3 (vendor-adapter principle): the status a Real component's
/// vendor adapter normalizes from its own unmodified driver, as the common
/// cause class plus `device.*` and `manyforge.vendor_code.*` facts (sorted by
/// id). The adapter owns the vendor message, rate, freshness rule and code
/// table; stale or missing status is `availability`.
struct RetainedDeviceStatus final {
  core::LifecycleBlockingCauseClassDTO cause{
      core::LifecycleBlockingCauseClassDTO::kAvailability};
  std::vector<core::LifecycleFactDTO> facts;

  friend bool operator==(const RetainedDeviceStatus &,
                         const RetainedDeviceStatus &) = default;
};

/// V-A (A4): what a vendor joint sample (well-formed, see
/// `vendor_joint_display_sample`) is good for, by the vendor adapter's check.
enum class VendorJointSampleUse : std::uint8_t {
  /// Not a pose (xArm: every joint exactly zero, the controller's e-stop
  /// signature with the hardware active): the display keeps its last valid
  /// pose.
  kIgnore,
  /// The display only.
  kDisplay,
  /// The display, and it refreshes the measured stream's liveness and leads
  /// previews (a move from the robot's own interface).
  kLeads,
};

/// V-A (A2): the result of a vendor adapter call outside its catalog
/// primitives (the tool context, the re-engage), typed for the generic
/// handle: it decides only on these fields, never on the error's strings.
/// The error is what the user sees (code, message and details).
struct VendorCallResult final {
  /// Without an error: the adapter re-armed the robot (see the call).
  bool rearmed{false};
  std::optional<core::ErrorDTO> error{};
  /// With an error: a sent vendor call may still take effect.
  bool effect_possible{false};
  /// With an error: the robot controller did not answer in time (the
  /// handle reports the component fault, as for a lifecycle step).
  bool unresponsive{false};

  [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

/// The vendor adapter of a Real component's arm: the handle merges its status
/// into the motion domain's lifecycle observation (R7 package 3) and routes
/// the vendor's own primitives and Enable gate to it (package 4). Vendor
/// facts, timings and quirks are the adapter's answers; the handle holds
/// none.
/// V-A (A3), the re-arm contract: an adapter that re-arms the robot
/// (`rearmed` from `SendToolContext` or `ReengageAtRest`) must leave the
/// controllers reactivated by its vendor driver, as the xArm plugin does
/// once the robot is ready again: the handle waits for them (at most
/// `ActivationTransientBound`) and binds a new epoch with no controller
/// switch.
class IRetainedDeviceStatusAdapter {
public:
  virtual ~IRetainedDeviceStatusAdapter() = default;
  /// Once, from `StartBackend`, before the executor starts: create the
  /// adapter's subscriptions and service clients on the handle's node,
  /// stamping receipt on the handle's clock. Both outlive them.
  virtual void Start(rclcpp::Node &node, core::IClock &clock) = 0;
  /// The lifecycle Observe checkpoint: classify and commit vendor history.
  [[nodiscard]] virtual RetainedDeviceStatus Observe(core::TimePoint now_ns) = 0;
  /// Pure health: the same classification without committing anything.
  [[nodiscard]] virtual RetainedDeviceStatus
  ReadHealth(core::TimePoint now_ns) const = 0;
  /// The handle's own hardware activation (arming, the vendor driver's own
  /// activation) or deactivation is in flight: its vendor mode/state
  /// changes are ManyForge's own, never external control.
  virtual void BeginHardwareTransition(bool activate) = 0;
  /// m5.5: the hardware interface's observed activity (controller-manager
  /// list), read before each Observe/ReadHealth; the vendor adapter
  /// reconstructs whether the robot is armed from it.
  virtual void ObserveHardwareActive(bool hardware_active) {
    static_cast<void>(hardware_active);
  }
  /// M6: the vendor driver's own joint-state topic, which keeps publishing
  /// in every mode (the Real arm's display always follows it), and the gate
  /// that checks each of its well-formed samples (bound joints, controller
  /// order): not a pose, the display only, or also the measured stream's
  /// liveness (only while the robot reports a move from its own interface
  /// without an error). The gate keeps no adapter alive. Both absent for a
  /// vendor without one; a topic without a gate shows every sample.
  [[nodiscard]] virtual std::optional<std::string> VendorJointStateTopic() const {
    return std::nullopt;
  }
  using VendorJointSampleGate = std::function<VendorJointSampleUse(
      core::TimePoint now_ns, const std::vector<double> &positions)>;
  [[nodiscard]] virtual VendorJointSampleGate VendorJointSampleGateFn() const { return {}; }
  /// V-A (A3): the longest transient of the vendor driver after an own
  /// hardware activation or re-arm, which the handle waits out at most:
  /// fresh measured joints after an activation, the reactivated controllers
  /// after a re-arm (xArm: 5 s, the plugin's deactivate-all/reactivate-all
  /// through blocking controller-manager calls from its loop). Default: none.
  [[nodiscard]] virtual std::chrono::nanoseconds ActivationTransientBound() const {
    return std::chrono::nanoseconds::zero();
  }
  /// V-A (A5), pure: the vendor's precondition for the handle's hardware
  /// activation (arming). An error refuses it before effect. xArm: no
  /// error and fresh status (the driver's activation clears the controller
  /// error, so it never runs over an error the operator has not
  /// acknowledged). Default: armable.
  [[nodiscard]] virtual core::Result<void> CheckArmable(core::TimePoint /*now_ns*/) const {
    return core::Result<void>::Ok();
  }
  /// It returned; with a possible effect the explanation lasts a bounded
  /// settle window.
  virtual void SettleHardwareTransition(bool activate, bool effect_possible,
                                        core::TimePoint now_ns) = 0;
  /// The vendor part of the Enable gate (R7 §3.6), pure: the robot is ready
  /// and the measured joints (controller joint order, already fresh and
  /// finite) agree with its own reported angles within the component's
  /// tolerance. An error refuses the activation before effect.
  [[nodiscard]] virtual core::Result<void>
  CheckActivation(core::TimePoint now_ns,
                  const std::vector<double> &measured_positions) const = 0;
  /// M6 XADAPT: the device facts and vendor codes of the latest fresh vendor
  /// report (no derived facts), for a definite outcome of a refused step;
  /// with `received_after`, of the first report received after it, waiting
  /// at most one freshness window (freshness, not a call). None without such
  /// a report and for a vendor without it.
  [[nodiscard]] virtual std::optional<std::vector<core::LifecycleFactDTO>>
  ReadReportFacts(std::optional<core::TimePoint> /*received_after*/,
                  std::stop_token /*cancellation*/) const {
    return std::nullopt;
  }
  /// One vendor primitive of the component's catalog (xArm:
  /// `device.clear_error`, `xarm.clear_warning`): one call, never retried; a
  /// timeout or a vendor failure is an uncertain outcome reconciled by
  /// observation. Other ids are refused before effect.
  /// M6 XADAPT (D8): each call is allowed min(the call cap, `step_deadline`);
  /// no answer within it is the connection lost (`*_unresponsive`).
  /// V-A (A2): `adapter_decided` when its own comparison decided the outcome.
  [[nodiscard]] virtual RetainedRos2ControlSwitchResult
  Dispatch(const core::LifecyclePrimitiveId &primitive,
           std::optional<std::chrono::steady_clock::time_point> step_deadline,
           std::stop_token cancellation) = 0;
  /// Active TCP (M7, T-MQ16, T-MQ16b, design 3): the one tool-context call.
  /// The handle decides what changes; the adapter sends exactly that, each
  /// vendor call once, never retried:
  /// - offset `Keep`: nothing about the TCP; `Set`: the controller's TCP
  ///   offset is X (sent, then the at-rest settle and the post-send check,
  ///   T-C2d; at Enable only when the controller reports another); `Zero`:
  ///   the flange (no TCP, B-run-1; the checkbox off, T-C2e), as `Set` with
  ///   the identity;
  /// - load `Keep`: nothing about the payload; `Set`: the total payload L
  ///   (`set_tcp_load` only: no offset, no re-arm, also while moving).
  /// Calls: `at_enable` the Enable re-sync before the switch {Set|Zero, Set};
  /// a TCP change or reset, robot enabled and at rest {Set|Zero, Keep}; an
  /// attach/detach changing the total {Keep, Set}, at any time.
  /// Result: `rearmed` when an offset was sent (the robot re-armed; the
  /// handle rebinds). An error with neither `effect_possible` nor
  /// `unresponsive`: refused before anything was sent (typed, no fault). An
  /// error with `effect_possible` (sent, not confirmed: a failed re-arm or
  /// rebind, a post-send mismatch `tcp_offset_not_applied`) or
  /// `unresponsive` (no answer) is T-N1: outside a lifecycle step the handle
  /// reports the component fault (D8); at Enable the step is uncertain or
  /// failed as today. At Enable each call is allowed min(the call cap,
  /// `step_deadline`) and follows `cancellation`; otherwise the call cap and
  /// no cancellation. Without a vendor call for it (other vendors): nothing.
  [[nodiscard]] virtual VendorCallResult
  SendToolContext(const VendorToolContextCommand & /*command*/, bool /*at_enable*/,
                  std::optional<std::chrono::steady_clock::time_point> /*step_deadline*/,
                  std::stop_token /*cancellation*/) {
    return {};
  }
  /// M6 ADAPTERM, the same pre-motion hook: an admitted ManyForge motion,
  /// before it is planned, with nothing in flight (the robot at rest). After
  /// an error-free move from the robot's own interface on an idle cell, the
  /// adapter puts the robot back into its command mode and state (each vendor
  /// call once, never retried) and returns `rearmed` once the robot reports
  /// it (see the re-arm contract above). Not rearmed: no such move,
  /// nothing done. An error (still moving, a vendor error, a call not
  /// confirmed) refuses the motion before it moves; its `effect_possible`:
  /// a sent call may still change the robot's mode or state. Other vendors:
  /// not rearmed.
  [[nodiscard]] virtual VendorCallResult
  ReengageAtRest(std::stop_token /*cancellation*/) {
    return {};
  }
  /// M6 ADAPTERM: the handle bound the reactivated controller; ManyForge's
  /// own re-arm (`device.rearm_in_progress`) ends.
  virtual void EndRearm() {}
  /// M6 ADAPTERM: the Enable gate after a re-engage, also requiring the arm
  /// at rest by the vendor's own report (xArm: not the moving state).
  [[nodiscard]] virtual core::Result<void>
  CheckReengagedAtRest(core::TimePoint now_ns,
                       const std::vector<double> &measured_positions) const {
    return CheckActivation(now_ns, measured_positions);
  }
};

/// V-A: one vendor adapter family, as its vendor library supplies it (one
/// registry row, `retained_vendor_device_status.hpp`): the adapter factory
/// and the family's Real lifecycle providers. Apart from the registry's list
/// of linked vendor libraries, the generic Real lane names no vendor id,
/// catalog or adapter class.
struct VendorAdapterFamily final {
  /// The family id, `VendorAdapterSelection::vendor` (e.g. "xarm").
  std::string_view vendor;
  /// The family's adapter for one Real component, not started; throws
  /// `std::invalid_argument` for invalid adapter data.
  std::shared_ptr<IRetainedDeviceStatusAdapter> (*make_adapter)(
      const VendorAdapterSelection &selection);
  /// The lifecycle provider of the family's Real arm and its own catalog,
  /// anchored by the exact provider manifest. V-B4: the Deployment compiler
  /// selects `manyforge.provider.ros2_control.<vendor>` (and `.gripper`
  /// below) for a Real row of this family, so the ids follow that form.
  std::string_view arm_provider_manifest_id;
  core::Result<core::BrandLifecyclePlanCatalogDTO> (*make_arm_catalog)(
      const core::ProviderManifestDTO &manifest);
  /// The lifecycle provider of the family's Real gripper domain, which uses
  /// the shared Real gripper plans (RoboPlan).
  std::string_view gripper_provider_manifest_id;
};

} // namespace manyforge::roboplan_backend
