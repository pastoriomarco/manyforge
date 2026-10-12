#include <gtest/gtest.h>

#include <manyforge_roboplan/isaac_arm_guard.hpp>

#include <optional>
#include <stdexcept>

namespace manyforge::roboplan_backend {
namespace {

constexpr core::Duration kMaxAgeNs = 250'000'000;

[[nodiscard]] IsaacArmGuardFacts facts(std::uint64_t epoch, core::TimePoint stamp,
                                       double velocity = 0.0,
                                       bool goal_executing = false,
                                       bool active = true) {
  IsaacArmGuardFacts result;
  result.readiness.controller_active = active;
  result.readiness.controller_bound = active;
  result.readiness.hardware_active = true;
  result.readiness.binding_epoch = core::ExecutionBindingEpoch(epoch);
  core::ExecutionControllerSampleDTO sample;
  sample.source_stamp_ns = stamp;
  sample.binding_epoch = core::ExecutionBindingEpoch(epoch);
  sample.reference_velocities_rad_s = {0.0, velocity};
  result.sample = sample;
  result.goal_executing = goal_executing;
  return result;
}

[[nodiscard]] IsaacGuardStatus status(IsaacGuardState state,
                                      std::optional<core::TimePoint> after = {},
                                      std::optional<std::string> reason = {}) {
  return IsaacGuardStatus{state, std::move(reason), after};
}

/// Startup through confirmation; returns the confirmed T.
core::TimePoint open_at_startup(IsaacArmGuard &guard, core::TimePoint stamp) {
  EXPECT_TRUE(guard.OnJointStateReceived(1).empty());
  const auto sent = guard.Step(2, facts(1, stamp));
  EXPECT_EQ(sent.authorize_after_ns, std::optional<core::TimePoint>(stamp));
  EXPECT_TRUE(guard.OnGuardStatus(status(IsaacGuardState::kOpen, stamp)).empty());
  EXPECT_TRUE(guard.MotionAdmitted());
  return stamp;
}

TEST(IsaacArmGuard, RequiresAnExplicitJointStateAge) {
  EXPECT_THROW(IsaacArmGuard(IsaacArmGuardConfig{}), std::invalid_argument);
}

TEST(IsaacArmGuard, StartupActivatesAfterFeedbackAndAuthorizesFromTheHold) {
  IsaacArmGuard guard({kMaxAgeNs});
  // No activation and no authorization before real Isaac joint states.
  EXPECT_FALSE(guard.JtcActivationAllowed());
  EXPECT_TRUE(guard.Step(1, facts(1, 100)).empty());
  EXPECT_FALSE(guard.MotionAdmitted());

  EXPECT_TRUE(guard.OnJointStateReceived(5).empty());
  EXPECT_TRUE(guard.JtcActivationAllowed());
  // JTC not active yet: nothing to authorize.
  EXPECT_TRUE(guard.Step(6, facts(1, 110, 0.0, false, false)).empty());
  // Moving reference is not a hold.
  EXPECT_TRUE(guard.Step(7, facts(1, 120, 0.3)).empty());
  // A sample from another binding epoch is not this binding's hold.
  auto foreign = facts(2, 125);
  foreign.sample->binding_epoch = core::ExecutionBindingEpoch(1U);
  EXPECT_TRUE(guard.Step(8, foreign).empty());

  const auto sent = guard.Step(9, facts(2, 130));
  EXPECT_EQ(sent.authorize_after_ns, std::optional<core::TimePoint>(130));
  EXPECT_EQ(guard.PendingAuthorization(), std::optional<core::TimePoint>(130));
  // Startup homing waits for the guard's confirmation.
  EXPECT_FALSE(guard.MotionAdmitted());
}

TEST(IsaacArmGuard, WaitsForTheExactConfirmationWithOneOutstandingAuthorization) {
  IsaacArmGuard guard({kMaxAgeNs});
  static_cast<void>(guard.OnJointStateReceived(1));
  ASSERT_EQ(guard.Step(2, facts(1, 200)).authorize_after_ns,
            std::optional<core::TimePoint>(200));
  // One outstanding authorization: a newer hold sends nothing more.
  EXPECT_TRUE(guard.Step(3, facts(1, 210)).empty());
  // Transient-local closed status and a stale open status do not confirm.
  EXPECT_TRUE(guard.OnGuardStatus(status(IsaacGuardState::kClosed)).empty());
  EXPECT_TRUE(guard.OnGuardStatus(status(IsaacGuardState::kOpen, 150)).empty());
  EXPECT_TRUE(guard.OnGuardStatus(status(IsaacGuardState::kOpen)).empty());
  EXPECT_FALSE(guard.MotionAdmitted());
  EXPECT_TRUE(guard.OnGuardStatus(status(IsaacGuardState::kOpen, 200)).empty());
  EXPECT_TRUE(guard.MotionAdmitted());
  EXPECT_FALSE(guard.PendingAuthorization().has_value());
}

TEST(IsaacArmGuard, TripDeactivatesReportsAndRetiresThePendingAuthorization) {
  IsaacArmGuard guard({kMaxAgeNs});
  static_cast<void>(guard.OnJointStateReceived(1));
  ASSERT_TRUE(guard.Step(2, facts(1, 300)).authorize_after_ns.has_value());
  const auto tripped = guard.OnGuardStatus(
      status(IsaacGuardState::kTripped, std::nullopt, "no fresh frame"));
  EXPECT_TRUE(tripped.deactivate_jtc);
  EXPECT_EQ(tripped.fault_reason, std::optional<std::string>("no fresh frame"));
  EXPECT_TRUE(guard.Tripped());
  EXPECT_FALSE(guard.PendingAuthorization().has_value());
  // Late open status for the retired T neither clears nor admits.
  EXPECT_TRUE(guard.OnGuardStatus(status(IsaacGuardState::kOpen, 300)).empty());
  EXPECT_TRUE(guard.Tripped());
  EXPECT_FALSE(guard.MotionAdmitted());
  // No authorization while tripped, even from a hold.
  EXPECT_TRUE(guard.Step(3, facts(1, 310)).empty());
}

TEST(IsaacArmGuard, UnexpectedAuthorizationAfterATripKeepsTheIncident) {
  IsaacArmGuard guard({kMaxAgeNs});
  const auto confirmed = open_at_startup(guard, 400);
  const auto tripped = guard.OnGuardStatus(status(IsaacGuardState::kTripped));
  EXPECT_TRUE(tripped.deactivate_jtc);
  EXPECT_EQ(tripped.fault_reason,
            std::optional<std::string>("isaac_guard_tripped"));
  EXPECT_TRUE(guard.OnGuardStatus(status(IsaacGuardState::kOpen, 999)).empty());
  EXPECT_TRUE(
      guard.OnGuardStatus(status(IsaacGuardState::kOpen, confirmed)).empty());
  EXPECT_TRUE(guard.Tripped());
  EXPECT_FALSE(guard.MotionAdmitted());
}

TEST(IsaacArmGuard, ResetAfterTripReactivatesAndAuthorizesFromTheNewHold) {
  IsaacArmGuard guard({kMaxAgeNs});
  static_cast<void>(guard.OnJointStateReceived(1));
  ASSERT_TRUE(guard.Step(2, facts(3, 500)).authorize_after_ns.has_value());
  static_cast<void>(guard.OnGuardStatus(status(IsaacGuardState::kOpen, 500)));
  static_cast<void>(guard.Step(3, facts(3, 510, 0.2, true)));
  ASSERT_TRUE(guard.OnGuardStatus(status(IsaacGuardState::kTripped))
                  .deactivate_jtc);

  // The trip's switch-off: the epoch-3 instance reports its controller
  // inactive; nothing is authorized from it.
  EXPECT_TRUE(guard.Step(4, facts(3, 515, 0.0, false, false)).empty());

  const auto reset = guard.OnResetRequested();
  EXPECT_TRUE(reset.activate_jtc);
  EXPECT_FALSE(reset.deactivate_jtc);
  EXPECT_FALSE(guard.Tripped());
  EXPECT_FALSE(guard.MotionAdmitted());
  // A hold still on the binding that tripped is not the reactivated hold.
  EXPECT_TRUE(guard.Step(5, facts(3, 520)).empty());
  // The host retires the epoch-3 adapter and constructs the epoch-4 one
  // before the switch: inactive until the activation lands.
  EXPECT_TRUE(guard.Step(6, facts(4, 530, 0.0, false, false)).empty());
  // Reactivated on epoch 4: its own hold is authorized.
  const auto sent = guard.Step(7, facts(4, 540));
  EXPECT_EQ(sent.authorize_after_ns, std::optional<core::TimePoint>(540));
  EXPECT_FALSE(guard.MotionAdmitted());
  static_cast<void>(guard.OnGuardStatus(status(IsaacGuardState::kOpen, 540)));
  EXPECT_TRUE(guard.MotionAdmitted());
}

TEST(IsaacArmGuard, NormalResetWithTheGuardOpenDoesNothing) {
  IsaacArmGuard guard({kMaxAgeNs});
  static_cast<void>(open_at_startup(guard, 600));
  EXPECT_TRUE(guard.OnResetRequested().empty());
  EXPECT_TRUE(guard.MotionAdmitted());
  EXPECT_TRUE(guard.Step(3, facts(1, 610)).empty());
}

TEST(IsaacArmGuard, GuardFoundClosedWhileAGoalExecutesIsATrip) {
  IsaacArmGuard guard({kMaxAgeNs});
  static_cast<void>(open_at_startup(guard, 700));
  static_cast<void>(guard.Step(3, facts(1, 710, 0.4, true)));
  const auto closed = guard.OnGuardStatus(status(IsaacGuardState::kClosed));
  EXPECT_TRUE(closed.deactivate_jtc);
  EXPECT_EQ(closed.fault_reason,
            std::optional<std::string>("isaac_guard_closed_while_executing"));
  EXPECT_TRUE(guard.Tripped());
}

TEST(IsaacArmGuard, ReconnectWhileHoldingReauthorizesWithAFreshTime) {
  IsaacArmGuard guard({kMaxAgeNs});
  static_cast<void>(open_at_startup(guard, 800));
  EXPECT_TRUE(guard.OnGuardReconnected().empty());
  EXPECT_FALSE(guard.MotionAdmitted());
  // The same hold stamp is never reused: wait for a newer sample.
  EXPECT_TRUE(guard.Step(3, facts(1, 800)).empty());
  const auto sent = guard.Step(4, facts(1, 820));
  EXPECT_EQ(sent.authorize_after_ns, std::optional<core::TimePoint>(820));
  // The previous confirmation does not count for the new authorization.
  EXPECT_TRUE(guard.OnGuardStatus(status(IsaacGuardState::kOpen, 800)).empty());
  EXPECT_FALSE(guard.MotionAdmitted());
  static_cast<void>(guard.OnGuardStatus(status(IsaacGuardState::kOpen, 820)));
  EXPECT_TRUE(guard.MotionAdmitted());
}

TEST(IsaacArmGuard, ReconnectWhileAGoalExecutesWaitsForTheHold) {
  IsaacArmGuard guard({kMaxAgeNs});
  static_cast<void>(open_at_startup(guard, 900));
  static_cast<void>(guard.Step(3, facts(1, 910, 0.5, true)));
  EXPECT_TRUE(guard.OnGuardReconnected().empty());
  EXPECT_FALSE(guard.MotionAdmitted());
  // No authorization while the goal executes.
  EXPECT_TRUE(guard.Step(4, facts(1, 920, 0.5, true)).empty());
  // Goal ended but JTC still decelerating: not a hold yet.
  EXPECT_TRUE(guard.Step(5, facts(1, 930, 0.1, false)).empty());
  EXPECT_EQ(guard.Step(6, facts(1, 940)).authorize_after_ns,
            std::optional<core::TimePoint>(940));
}

TEST(IsaacArmGuard, StatusWithoutTheConfirmedTimeRequiresReauthorization) {
  IsaacArmGuard guard({kMaxAgeNs});
  static_cast<void>(open_at_startup(guard, 1000));
  // Guard restarted while idle: closed, not a trip; new motion waits.
  const auto closed = guard.OnGuardStatus(status(IsaacGuardState::kClosed));
  EXPECT_TRUE(closed.empty());
  EXPECT_FALSE(guard.Tripped());
  EXPECT_FALSE(guard.MotionAdmitted());
  EXPECT_EQ(guard.Step(3, facts(1, 1010)).authorize_after_ns,
            std::optional<core::TimePoint>(1010));
}

TEST(IsaacArmGuard, StalenessIsReportedOnceAndSamplesStayAccepted) {
  IsaacArmGuard guard({kMaxAgeNs});
  static_cast<void>(open_at_startup(guard, 1100));
  EXPECT_TRUE(guard.Step(1 + kMaxAgeNs, facts(1, 1110)).empty());
  const auto stale = guard.Step(2 + kMaxAgeNs, facts(1, 1120));
  EXPECT_EQ(stale.feedback_stale, std::optional<bool>(true));
  EXPECT_TRUE(guard.Step(3 + kMaxAgeNs, facts(1, 1130)).empty());
  // Staleness is observed and reported, not a motion gate (item 33).
  EXPECT_TRUE(guard.MotionAdmitted());
  const auto fresh = guard.OnJointStateReceived(4 + kMaxAgeNs);
  EXPECT_EQ(fresh.feedback_stale, std::optional<bool>(false));
  EXPECT_TRUE(guard.Step(5 + kMaxAgeNs, facts(1, 1140)).empty());
}

}  // namespace
}  // namespace manyforge::roboplan_backend
