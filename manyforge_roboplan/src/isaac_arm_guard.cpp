#include <manyforge_roboplan/isaac_arm_guard.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

// JTC holds when it is active on its current binding with no trajectory
// moving: the hold sample's reference velocity is exactly zero. A cancel
// still decelerating after its goal ended is therefore not a hold.
[[nodiscard]] bool holding(const IsaacArmGuardFacts &facts) {
  const auto &readiness = facts.readiness;
  if (!readiness.controller_active || !readiness.controller_bound ||
      !readiness.hardware_active || facts.goal_executing ||
      !facts.sample.has_value() ||
      facts.sample->binding_epoch != readiness.binding_epoch) {
    return false;
  }
  const auto &velocities = facts.sample->reference_velocities_rad_s;
  return !velocities.empty() &&
         std::all_of(velocities.begin(), velocities.end(),
                     [](const double value) { return value == 0.0; });
}

} // namespace

IsaacArmGuard::IsaacArmGuard(IsaacArmGuardConfig config)
    : config_(config) {
  if (config_.joint_state_max_age_ns <= 0) {
    throw std::invalid_argument(
        "Isaac arm guard requires an explicit joint-state maximum age");
  }
}

IsaacArmGuardActions
IsaacArmGuard::OnJointStateReceived(const core::TimePoint local_receipt_ns) {
  IsaacArmGuardActions actions;
  if (last_joint_state_receipt_ns_.has_value() &&
      local_receipt_ns < *last_joint_state_receipt_ns_) {
    // Local monotonic receipts never go backwards; keep the newest.
    return actions;
  }
  last_joint_state_receipt_ns_ = local_receipt_ns;
  if (phase_ == Phase::kAwaitingFeedback) {
    phase_ = Phase::kAwaitingHold;
  }
  if (feedback_reported_stale_) {
    feedback_reported_stale_ = false;
    actions.feedback_stale = false;
  }
  return actions;
}

IsaacArmGuardActions
IsaacArmGuard::OnGuardStatus(const IsaacGuardStatus &status) {
  if (phase_ == Phase::kTripped) {
    // Retired: no later status clears the incident or admits motion.
    return {};
  }
  if (status.state == IsaacGuardState::kTripped) {
    return Trip(status.reason.value_or("isaac_guard_tripped"));
  }
  if (status.state == IsaacGuardState::kClosed) {
    if (goal_executing_) {
      return Trip("isaac_guard_closed_while_executing");
    }
    if (phase_ == Phase::kOpen) {
      RequireReauthorization();
    }
    return {};
  }
  if (phase_ == Phase::kAwaitingConfirmation &&
      status.authorized_after_ns == pending_authorization_ns_) {
    confirmed_authorization_ns_ = pending_authorization_ns_;
    pending_authorization_ns_.reset();
    phase_ = Phase::kOpen;
  } else if (phase_ == Phase::kOpen &&
             status.authorized_after_ns != confirmed_authorization_ns_) {
    RequireReauthorization();
  }
  return {};
}

IsaacArmGuardActions IsaacArmGuard::OnGuardReconnected() {
  if (phase_ != Phase::kTripped && phase_ != Phase::kAwaitingFeedback) {
    RequireReauthorization();
  }
  return {};
}

IsaacArmGuardActions IsaacArmGuard::OnResetRequested() {
  IsaacArmGuardActions actions;
  if (phase_ != Phase::kTripped) {
    return actions;  // Q27 unchanged: nothing to do here.
  }
  phase_ = Phase::kAwaitingHold;
  actions.activate_jtc = true;
  return actions;
}

IsaacArmGuardActions IsaacArmGuard::Step(const core::TimePoint now_ns,
                                         const IsaacArmGuardFacts &facts) {
  IsaacArmGuardActions actions;
  goal_executing_ = facts.goal_executing;
  latest_binding_epoch_ = facts.readiness.binding_epoch;
  if (last_joint_state_receipt_ns_.has_value() && !feedback_reported_stale_ &&
      now_ns - *last_joint_state_receipt_ns_ > config_.joint_state_max_age_ns) {
    feedback_reported_stale_ = true;
    actions.feedback_stale = true;
  }
  if (phase_ != Phase::kAwaitingHold || !holding(facts) ||
      (tripped_binding_epoch_.has_value() &&
       facts.readiness.binding_epoch == *tripped_binding_epoch_)) {
    return actions;
  }
  const auto stamp = facts.sample->source_stamp_ns;
  if (last_sent_authorization_ns_.has_value() &&
      stamp <= *last_sent_authorization_ns_) {
    return actions;  // Never reuse T; wait for a newer hold sample.
  }
  tripped_binding_epoch_.reset();
  pending_authorization_ns_ = stamp;
  last_sent_authorization_ns_ = stamp;
  phase_ = Phase::kAwaitingConfirmation;
  actions.authorize_after_ns = stamp;
  return actions;
}

bool IsaacArmGuard::JtcActivationAllowed() const noexcept {
  return last_joint_state_receipt_ns_.has_value();
}

bool IsaacArmGuard::MotionAdmitted() const noexcept {
  return phase_ == Phase::kOpen;
}

bool IsaacArmGuard::Tripped() const noexcept {
  return phase_ == Phase::kTripped;
}

std::optional<core::TimePoint>
IsaacArmGuard::PendingAuthorization() const noexcept {
  return pending_authorization_ns_;
}

IsaacArmGuardActions IsaacArmGuard::Trip(std::string reason) {
  phase_ = Phase::kTripped;
  pending_authorization_ns_.reset();
  confirmed_authorization_ns_.reset();
  tripped_binding_epoch_ = latest_binding_epoch_;
  IsaacArmGuardActions actions;
  actions.deactivate_jtc = true;
  actions.fault_reason = std::move(reason);
  return actions;
}

void IsaacArmGuard::RequireReauthorization() noexcept {
  pending_authorization_ns_.reset();
  confirmed_authorization_ns_.reset();
  phase_ = Phase::kAwaitingHold;
}

} // namespace manyforge::roboplan_backend
