#include <manyforge_roboplan/ros2_control_component_state.hpp>

#include <gtest/gtest.h>

namespace manyforge::roboplan_backend {
namespace {

using Lifecycle = core::ComponentLifecycle;
using Activity = core::ComponentActivity;

[[nodiscard]] Ros2ControlObservation Prepared() {
  Ros2ControlObservation observation;
  observation.controller_manager_present = true;
  observation.observed = true;
  observation.controller_loaded = true;
  observation.hardware_active = true;
  return observation;
}

TEST(Ros2ControlComponentState, LifecycleIsWhatTheControllerManagerReports) {
  EXPECT_EQ(ros2_control_component_state({}, 7).lifecycle, Lifecycle::kOffline);
  EXPECT_EQ(ros2_control_component_state({}, 7).reason->code, "controller_manager_unavailable");
  auto observation = Prepared();
  const auto prepared = ros2_control_component_state(observation, 7);
  EXPECT_EQ(prepared.lifecycle, Lifecycle::kPrepared);
  EXPECT_EQ(prepared.observed_at_ns, 7);
  EXPECT_FALSE(prepared.fault.has_value());
  observation.controller_active = true;
  EXPECT_EQ(ros2_control_component_state(observation, 7).lifecycle, Lifecycle::kEnabled);
  observation.hardware_active = false;
  EXPECT_EQ(ros2_control_component_state(observation, 7).lifecycle, Lifecycle::kNotPrepared);
}

TEST(Ros2ControlComponentState, AnExitedLaunchIsAFaultWithRecover) {
  auto observation = Prepared();
  observation.process_fault = "controller launch exited with status 1";
  const auto state = ros2_control_component_state(observation, 3);
  EXPECT_EQ(state.lifecycle, Lifecycle::kFault);
  ASSERT_TRUE(state.fault.has_value());
  EXPECT_EQ(state.fault->code, core::FaultCode::kProcessExited);
  EXPECT_EQ(state.fault->operator_action, core::FaultOperatorAction::kRecover);
  EXPECT_EQ(state.fault->since_ns, 3);
}

TEST(Ros2ControlComponentState, AStaleOwnStreamIsAFaultExceptInASimulator) {
  auto observation = Prepared();
  observation.controller_active = true;
  observation.streams = {{"measured_joints", core::StreamFreshness::kStale, 500}};
  const auto robot = ros2_control_component_state(observation, 1);
  EXPECT_EQ(robot.lifecycle, Lifecycle::kFault);
  EXPECT_EQ(robot.fault->code, core::FaultCode::kStreamStale);
  observation.simulated = true;
  const auto simulated = ros2_control_component_state(observation, 1);
  EXPECT_FALSE(simulated.fault.has_value());
  EXPECT_EQ(simulated.streams.front().freshness, core::StreamFreshness::kStale);
  EXPECT_EQ(simulated.reason->code, "simulation_not_playing");
}

// SIM-1: a simulation that isn't playing keeps the lifecycle the controller
// reports (an enabled robot stays enabled) with the reason; playing again
// drops the reason.
TEST(Ros2ControlComponentState, ASimulatorNotPlayingKeepsTheControllersLifecycleWithTheReason) {
  auto observation = Prepared();
  observation.simulated = true;
  observation.streams = {{"measured_joints", core::StreamFreshness::kStale, 500}};
  const auto prepared = ros2_control_component_state(observation, 1);
  EXPECT_EQ(prepared.lifecycle, Lifecycle::kPrepared);
  EXPECT_EQ(prepared.reason->code, "simulation_not_playing");
  observation.controller_active = true;
  const auto enabled = ros2_control_component_state(observation, 1);
  EXPECT_EQ(enabled.lifecycle, Lifecycle::kEnabled);
  EXPECT_EQ(enabled.reason->code, "simulation_not_playing");
  Ros2ControlObservation gripper;
  gripper.simulated = true;
  gripper.local_owner_ready = true;
  gripper.local_owner_enabled = true;
  gripper.streams = {{"gripper_feedback", core::StreamFreshness::kStale, 500}};
  const auto tool = ros2_control_component_state(gripper, 1);
  EXPECT_EQ(tool.lifecycle, Lifecycle::kEnabled);
  EXPECT_EQ(tool.reason->code, "simulation_not_playing");
  observation.streams.front().freshness = core::StreamFreshness::kFresh;
  const auto playing = ros2_control_component_state(observation, 1);
  EXPECT_EQ(playing.lifecycle, Lifecycle::kEnabled);
  EXPECT_FALSE(playing.reason.has_value());
}

TEST(Ros2ControlComponentState, ActivityTrustsTheControllersOwnReport) {
  auto observation = Prepared();
  observation.controller_active = true;
  EXPECT_EQ(ros2_control_component_state(observation, 1).activity, Activity::kIdle);
  observation.goal_running = true;
  EXPECT_EQ(ros2_control_component_state(observation, 1).activity, Activity::kBusy);
  observation.goal_running = false;
  observation.decelerating = true;  // a cancelled goal whose reference still moves
  EXPECT_EQ(ros2_control_component_state(observation, 1).activity, Activity::kBusy);
  observation.decelerating = false;
  observation.reset_in_progress = true;
  EXPECT_EQ(ros2_control_component_state(observation, 1).activity, Activity::kBusy);
  // A goal whose result cannot come back.
  Ros2ControlObservation lost;
  lost.goal_running = true;
  EXPECT_EQ(ros2_control_component_state(lost, 1).activity, Activity::kUnknown);
}

// An open goal (the Stop's braking goal included) whose result hasn't come
// back by its expected end plus the lane's existing bound
// (`hard_stop_completion_timeout_s`) is `unknown`, never `busy` forever.
TEST(Ros2ControlComponentState, AGoalWithoutItsResultPastItsEndAndTheLaneBoundIsUnknown) {
  auto observation = Prepared();
  observation.controller_active = true;
  observation.goal_running = true;
  observation.goal_result_bound_ns = 2'000'000'000;
  observation.goal_past_end_ns = -500'000'000;  // still before its end
  EXPECT_EQ(ros2_control_component_state(observation, 1).activity, Activity::kBusy);
  observation.goal_past_end_ns = 2'000'000'000;
  EXPECT_EQ(ros2_control_component_state(observation, 1).activity, Activity::kBusy);
  observation.goal_past_end_ns = 2'000'000'001;
  EXPECT_EQ(ros2_control_component_state(observation, 1).activity, Activity::kUnknown);
  // Its result came back: the controller's report decides again.
  observation.goal_running = false;
  EXPECT_EQ(ros2_control_component_state(observation, 1).activity, Activity::kIdle);
  // Without a bound (a tool), an open goal stays the controller's report.
  observation.goal_running = true;
  observation.goal_result_bound_ns.reset();
  EXPECT_EQ(ros2_control_component_state(observation, 1).activity, Activity::kBusy);
}

TEST(Ros2ControlComponentState, ALocalGripperOwnerReportsItsReadiness) {
  Ros2ControlObservation observation;
  observation.local_owner_ready = false;
  EXPECT_EQ(ros2_control_component_state(observation, 1).lifecycle, Lifecycle::kNotPrepared);
  observation.local_owner_ready = true;
  EXPECT_EQ(ros2_control_component_state(observation, 1).lifecycle, Lifecycle::kPrepared);
  observation.local_owner_enabled = true;
  EXPECT_EQ(ros2_control_component_state(observation, 1).lifecycle, Lifecycle::kEnabled);
}

} // namespace
} // namespace manyforge::roboplan_backend
