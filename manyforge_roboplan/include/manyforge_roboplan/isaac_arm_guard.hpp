#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <manyforge_core/ports/iexecution_port.hpp>
#include <manyforge_core/time/iclock.hpp>

namespace manyforge::roboplan_backend {

/// State reported by the Isaac-side command guard on
/// `<joint_commands_topic>/guard_status` (values `state`, `reason`,
/// `authorized_after.sec`/`.nanosec`). The ROS binding converts the
/// `DiagnosticStatus` `values` into this; an absent authorization stays
/// absent. Decisions never use the status `level` byte.
enum class IsaacGuardState {
  kClosed,
  kOpen,
  kTripped,
};

struct IsaacGuardStatus final {
  IsaacGuardState state{IsaacGuardState::kClosed};
  std::optional<std::string> reason{};
  /// `authorized_after` in nanoseconds of the controller's stamp clock.
  std::optional<core::TimePoint> authorized_after_ns{};

  friend bool operator==(const IsaacGuardStatus &,
                         const IsaacGuardStatus &) = default;
};

/// Facts the owner reads on each step from the arm's execution port and the
/// host's own goal bookkeeping.
struct IsaacArmGuardFacts final {
  core::ExecutionReadinessDTO readiness{};
  std::optional<core::ExecutionControllerSampleDTO> sample{};
  /// A goal of this component was sent and is not yet terminal.
  bool goal_executing{false};
};

/// Effects the host performs, in this order, through the existing owners:
/// the controller lifecycle through CCLC, the authorization publisher, the
/// existing fault path (InterventionRequired, item 44) and the existing
/// observability path. The owner itself performs no effect.
struct IsaacArmGuardActions final {
  bool deactivate_jtc{false};
  bool activate_jtc{false};
  /// Publish one `builtin_interfaces/Time` authorization with this T.
  std::optional<core::TimePoint> authorize_after_ns{};
  /// A trip (or a guard found closed while a goal executes): the operation
  /// ends InterventionRequired; Reset is the only way back.
  std::optional<std::string> fault_reason{};
  /// `/isaac_joint_states` receipt age crossed the configured maximum
  /// (true: stale, false: fresh again). Reported only; samples are accepted
  /// as `main` (item 33).
  std::optional<bool> feedback_stale{};

  [[nodiscard]] bool empty() const noexcept {
    return !deactivate_jtc && !activate_jtc && !authorize_after_ns &&
           !fault_reason && !feedback_stale;
  }

  friend bool operator==(const IsaacArmGuardActions &,
                         const IsaacArmGuardActions &) = default;
};

struct IsaacArmGuardConfig final {
  /// Largest `/isaac_joint_states` receipt age (local monotonic time) before
  /// staleness is reported. Supplied by the deployment; no default.
  core::Duration joint_state_max_age_ns{0};
};

/// Host-side sequencing of one Isaac arm and its command guard (design §4.3,
/// Q13, Q41, Q43). One instance per Isaac arm; observations are processed in
/// arrival order by the single caller that owns it (not thread-safe).
///
/// - JTC activation is allowed only after real Isaac joint states arrived.
/// - Once JTC is active and holding (active, bound, hardware active, a hold
///   sample on the current binding epoch: zero reference velocity, no goal
///   executing), the owner authorizes with T taken from that sample's own
///   controller stamp. T is never reused: it must exceed every T sent before.
/// - One authorization is outstanding at a time. No motion is admitted
///   (startup homing included) until the guard reports open with
///   `authorized_after == T`.
/// - A trip, or a guard found closed while a goal executes, deactivates JTC,
///   reports the fault and retires the pending T. Later open statuses neither
///   clear the incident nor admit motion. Operator Reset reactivates JTC and
///   authorizes again from the new binding's hold. Reset with the guard open
///   does nothing here (Q27).
/// - A guard reconnect, or a status that no longer shows the confirmed T,
///   requires a fresh authorization under the same conditions; new motion
///   waits for it.
class IsaacArmGuard final {
public:
  explicit IsaacArmGuard(IsaacArmGuardConfig config);

  /// One `/isaac_joint_states` message received at this local monotonic time.
  [[nodiscard]] IsaacArmGuardActions
  OnJointStateReceived(core::TimePoint local_receipt_ns);

  [[nodiscard]] IsaacArmGuardActions
  OnGuardStatus(const IsaacGuardStatus &status);

  /// The guard's status/authorization endpoints were (re)matched.
  [[nodiscard]] IsaacArmGuardActions OnGuardReconnected();

  /// Operator Reset of this component.
  [[nodiscard]] IsaacArmGuardActions OnResetRequested();

  /// Periodic step with the latest port facts and local monotonic time.
  [[nodiscard]] IsaacArmGuardActions Step(core::TimePoint now_ns,
                                          const IsaacArmGuardFacts &facts);

  /// CCLC may activate the arm JTC: real Isaac joint states were received.
  [[nodiscard]] bool JtcActivationAllowed() const noexcept;

  /// Gate for every motion path of this component (goals, replacements,
  /// startup motion).
  [[nodiscard]] bool MotionAdmitted() const noexcept;

  /// The component is in a guard incident awaiting Reset.
  [[nodiscard]] bool Tripped() const noexcept;

  /// The authorization sent and not yet confirmed, if any.
  [[nodiscard]] std::optional<core::TimePoint> PendingAuthorization() const
      noexcept;

private:
  enum class Phase {
    kAwaitingFeedback,
    kAwaitingHold,
    kAwaitingConfirmation,
    kOpen,
    kTripped,
  };

  [[nodiscard]] IsaacArmGuardActions Trip(std::string reason);
  void RequireReauthorization() noexcept;

  IsaacArmGuardConfig config_;
  Phase phase_{Phase::kAwaitingFeedback};
  std::optional<core::TimePoint> last_joint_state_receipt_ns_{};
  bool feedback_reported_stale_{false};
  std::optional<core::TimePoint> pending_authorization_ns_{};
  std::optional<core::TimePoint> confirmed_authorization_ns_{};
  std::optional<core::TimePoint> last_sent_authorization_ns_{};
  /// After a Reset from a trip: the hold must come from a binding newer than
  /// the one that tripped (JTC reactivated).
  std::optional<core::ExecutionBindingEpoch> tripped_binding_epoch_{};
  bool goal_executing_{false};
  core::ExecutionBindingEpoch latest_binding_epoch_{0U};
};

} // namespace manyforge::roboplan_backend
