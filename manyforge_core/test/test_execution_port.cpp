#include <gtest/gtest.h>

#include <manyforge_core/ports/iexecution_port.hpp>
#include <manyforge_core/robot_at_target.hpp>
#include <manyforge_core/testing/fake_execution_port.hpp>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>
#include <variant>

namespace manyforge::core {
namespace {

using testing::FakeExecutionPort;

static_assert(std::is_abstract_v<IExecutionPort>);
static_assert(std::has_virtual_destructor_v<IExecutionPort>);

ExecutionGoalKey Key(std::uint64_t generation) {
  return ExecutionGoalKey{ControllerGoalId("op-1.controller-goal.1"),
                          ControllerGoalGeneration(generation)};
}

TrajectoryDTO Trajectory(double end_s) {
  TrajectoryDTO trajectory;
  trajectory.joint_names = {"j1"};
  trajectory.waypoints = {{0.0, {0.0}, {0.0}, {0.0}}, {end_s, {1.0}, {0.0}, {0.0}}};
  return trajectory;
}

TEST(ExecutionPortTest, GoalKeyGenerationsAreDistinctAndOrdered) {
  EXPECT_NE(Key(1U), Key(2U));
  EXPECT_LT(Key(1U), Key(2U));
  EXPECT_EQ(Key(2U), Key(2U));
}

TEST(ExecutionPortTest, DispatchedSubmitIsPendingUntilScripted) {
  FakeExecutionPort port;
  IExecutionPort& iface = port;

  const auto result = iface.Submit(Key(1U), Trajectory(1.0));
  EXPECT_EQ(result.dispatch, ExecutionPortDispatch::kDispatched);
  EXPECT_FALSE(result.refusal.has_value());

  const auto goal = iface.ReadGoal(Key(1U));
  ASSERT_TRUE(goal.has_value());
  EXPECT_EQ(goal->phase, ExecutionGoalPhase::kPending);
  EXPECT_FALSE(goal->result.has_value());
  EXPECT_FALSE(goal->mechanism_goal_id.empty());
  EXPECT_EQ(goal->binding_epoch, iface.ReadReadiness().binding_epoch);
  EXPECT_FALSE(iface.ReadGoal(Key(2U)).has_value());
}

TEST(ExecutionPortTest, RefusedCommandRegistersNoGoal) {
  FakeExecutionPort port;
  port.QueueCommandResult({ExecutionPortDispatch::kNotDispatched,
                           ExecutionPortRefusal::kNotReady,
                           ErrorDTO{"execution_rejected", "controller inactive", {}}});

  const auto result = port.Submit(Key(1U), Trajectory(1.0));
  EXPECT_EQ(result.dispatch, ExecutionPortDispatch::kNotDispatched);
  EXPECT_EQ(result.refusal, ExecutionPortRefusal::kNotReady);
  EXPECT_FALSE(port.ReadGoal(Key(1U)).has_value());
  ASSERT_EQ(port.commands().size(), 1U);
}

TEST(ExecutionPortTest, ScriptedReplaceEndsActiveGoalPreemptedByNextKey) {
  FakeExecutionPort port;
  ASSERT_EQ(port.Submit(Key(1U), Trajectory(2.0)).dispatch, ExecutionPortDispatch::kDispatched);
  port.SetGoalPhase(Key(1U), ExecutionGoalPhase::kExecuting, TimePoint{100});

  ASSERT_EQ(port.Replace(Key(1U), Key(2U), Trajectory(0.5)).dispatch,
            ExecutionPortDispatch::kDispatched);

  // Sending the replacement alone does not end the active goal.
  EXPECT_EQ(port.ReadGoal(Key(1U))->phase, ExecutionGoalPhase::kExecuting);
  EXPECT_EQ(port.ReadGoal(Key(2U))->phase, ExecutionGoalPhase::kPending);

  port.SetGoalPhase(Key(2U), ExecutionGoalPhase::kAccepted, TimePoint{200});
  port.FinishGoal(Key(1U), ExecutionGoalResult::kPreemptedBy, Key(2U));

  const auto active = port.ReadGoal(Key(1U));
  ASSERT_TRUE(active.has_value());
  EXPECT_EQ(active->phase, ExecutionGoalPhase::kTerminal);
  EXPECT_EQ(active->result, ExecutionGoalResult::kPreemptedBy);
  EXPECT_EQ(active->preempted_by, Key(2U));
  EXPECT_EQ(active->accepted_stamp_ns, TimePoint{100});
  EXPECT_NE(active->mechanism_goal_id, port.ReadGoal(Key(2U))->mechanism_goal_id);

  const auto commands = port.commands();
  ASSERT_EQ(commands.size(), 2U);
  const auto* replace = std::get_if<FakeExecutionPort::ReplaceCall>(&commands[1]);
  ASSERT_NE(replace, nullptr);
  EXPECT_EQ(replace->active_key, Key(1U));
  EXPECT_EQ(replace->next_key, Key(2U));
}

TEST(ExecutionPortTest, CancelAcknowledgementIsNotFinish) {
  FakeExecutionPort port;
  ASSERT_EQ(port.Submit(Key(1U), Trajectory(2.0)).dispatch, ExecutionPortDispatch::kDispatched);
  port.SetGoalPhase(Key(1U), ExecutionGoalPhase::kExecuting);

  EXPECT_EQ(port.Cancel(Key(1U)).dispatch, ExecutionPortDispatch::kDispatched);
  port.MarkCancelSent(Key(1U), TimePoint{250});
  port.AcknowledgeCancel(Key(1U), TimePoint{300});
  const auto after_ack = port.ReadGoal(Key(1U));
  ASSERT_TRUE(after_ack.has_value());
  EXPECT_EQ(after_ack->phase, ExecutionGoalPhase::kExecuting);
  EXPECT_FALSE(after_ack->result.has_value());
  EXPECT_EQ(after_ack->cancel_sent_local_stamp, TimePoint{250});
  EXPECT_EQ(after_ack->cancel_ack_local_receipt_stamp, TimePoint{300});
  EXPECT_FALSE(after_ack->terminal_local_receipt_stamp.has_value());

  port.FinishGoal(Key(1U), ExecutionGoalResult::kCanceled, std::nullopt, TimePoint{450});
  const auto terminal = port.ReadGoal(Key(1U));
  ASSERT_TRUE(terminal.has_value());
  EXPECT_EQ(terminal->result, ExecutionGoalResult::kCanceled);
  // The acknowledgement and the terminal result are distinct receipts.
  EXPECT_EQ(terminal->cancel_ack_local_receipt_stamp, TimePoint{300});
  EXPECT_EQ(terminal->terminal_local_receipt_stamp, TimePoint{450});
}

TEST(ExecutionPortTest, CancelOnPendingGoalIsUncertainWhenScripted) {
  FakeExecutionPort port;
  ASSERT_EQ(port.Submit(Key(1U), Trajectory(2.0)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_EQ(port.ReadGoal(Key(1U))->phase, ExecutionGoalPhase::kPending);

  // The mechanism latches the cancel until acceptance.
  port.QueueCommandResult({ExecutionPortDispatch::kEffectUncertain, std::nullopt, std::nullopt});
  EXPECT_EQ(port.Cancel(Key(1U)).dispatch, ExecutionPortDispatch::kEffectUncertain);
  const auto goal = port.ReadGoal(Key(1U));
  ASSERT_TRUE(goal.has_value());
  EXPECT_EQ(goal->phase, ExecutionGoalPhase::kPending);
  EXPECT_FALSE(goal->cancel_ack_local_receipt_stamp.has_value());
  EXPECT_FALSE(goal->result.has_value());
}

TEST(ExecutionPortTest, FeedbackIsAttachedToItsGoal) {
  FakeExecutionPort port;
  ExecutionLaneFactsDTO facts;
  facts.joint_names = {"j1"};
  port.SetLaneFacts(facts);
  ASSERT_EQ(port.Submit(Key(1U), Trajectory(2.0)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_EQ(port.Replace(Key(1U), Key(2U), Trajectory(0.5)).dispatch,
            ExecutionPortDispatch::kDispatched);
  EXPECT_FALSE(port.ReadGoal(Key(1U))->latest_feedback.has_value());

  ExecutionGoalFeedbackDTO feedback;
  feedback.source_stamp_ns = 7000;
  feedback.local_receipt_stamp_ns = 7100;
  feedback.cursor_s = 0.3;
  feedback.desired_positions_rad = {0.5};
  feedback.desired_velocities_rad_s = {0.2};
  feedback.desired_accelerations_rad_s2 = {0.0};
  feedback.actual_positions_rad = {0.49};
  port.SetGoalFeedback(Key(1U), feedback);

  const auto goal = port.ReadGoal(Key(1U));
  ASSERT_TRUE(goal.has_value());
  ASSERT_TRUE(goal->latest_feedback.has_value());
  EXPECT_EQ(*goal->latest_feedback, feedback);
  EXPECT_EQ(goal->latest_feedback->desired_positions_rad.size(),
            port.ReadLaneFacts().joint_names.size());
  // Feedback belongs to its own goal only.
  EXPECT_FALSE(port.ReadGoal(Key(2U))->latest_feedback.has_value());
}

TEST(ExecutionPortTest, RingDropsOldestTerminalRecordBeyondCapacity) {
  static_assert(kExecutionGoalRecordCapacity == 2U);
  FakeExecutionPort port;
  for (std::uint64_t generation = 1U; generation <= kExecutionGoalRecordCapacity; ++generation) {
    ASSERT_EQ(port.Submit(Key(generation), Trajectory(1.0)).dispatch,
              ExecutionPortDispatch::kDispatched);
    port.FinishGoal(Key(generation), ExecutionGoalResult::kSucceeded);
  }
  for (std::uint64_t generation = 1U; generation <= kExecutionGoalRecordCapacity; ++generation) {
    EXPECT_TRUE(port.ReadGoal(Key(generation)).has_value());
  }

  const auto next = Key(kExecutionGoalRecordCapacity + 1U);
  ASSERT_EQ(port.Submit(next, Trajectory(1.0)).dispatch, ExecutionPortDispatch::kDispatched);
  EXPECT_FALSE(port.ReadGoal(Key(1U)).has_value());
  EXPECT_TRUE(port.ReadGoal(Key(2U)).has_value());
  EXPECT_EQ(port.ReadGoal(next)->phase, ExecutionGoalPhase::kPending);
}

TEST(ExecutionPortTest, ThirdUnfinishedGoalIsRefusedNotDropped) {
  FakeExecutionPort port;
  ASSERT_EQ(port.Submit(Key(1U), Trajectory(1.0)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_EQ(port.Replace(Key(1U), Key(2U), Trajectory(1.0)).dispatch,
            ExecutionPortDispatch::kDispatched);
  const auto third = port.Replace(Key(2U), Key(3U), Trajectory(1.0));
  EXPECT_EQ(third.dispatch, ExecutionPortDispatch::kNotDispatched);
  EXPECT_EQ(third.refusal, ExecutionPortRefusal::kNotReady);
  EXPECT_TRUE(port.ReadGoal(Key(1U)).has_value());
  EXPECT_TRUE(port.ReadGoal(Key(2U)).has_value());
  EXPECT_FALSE(port.ReadGoal(Key(3U)).has_value());
}

TEST(ExecutionPortTest, StaleKeyAndUncertainEffectAreReportedAsScripted) {
  FakeExecutionPort port;
  port.QueueCommandResult(
      {ExecutionPortDispatch::kNotDispatched, ExecutionPortRefusal::kStaleKey, std::nullopt});
  port.QueueCommandResult({ExecutionPortDispatch::kEffectUncertain, std::nullopt, std::nullopt});

  EXPECT_EQ(port.Cancel(Key(7U)).refusal, ExecutionPortRefusal::kStaleKey);
  EXPECT_EQ(port.SetScale(0.5, port.ReserveScaleSequence()).dispatch, ExecutionPortDispatch::kEffectUncertain);
  EXPECT_EQ(port.SetScale(0.25, port.ReserveScaleSequence()).dispatch, ExecutionPortDispatch::kDispatched);

  const auto commands = port.commands();
  ASSERT_EQ(commands.size(), 3U);
  ASSERT_TRUE(std::holds_alternative<FakeExecutionPort::SetScaleCall>(commands[2]));
  EXPECT_DOUBLE_EQ(std::get<FakeExecutionPort::SetScaleCall>(commands[2]).value, 0.25);
}

TEST(ExecutionPortTest, ScaleReservationFencesAnOlderUnpublishedValue) {
  FakeExecutionPort port;
  const auto old = port.ReserveScaleSequence();
  const auto current = port.ReserveScaleSequence();
  EXPECT_EQ(port.SetScale(0.25, current).dispatch, ExecutionPortDispatch::kDispatched);
  EXPECT_EQ(port.SetScale(0.75, old).dispatch, ExecutionPortDispatch::kNotDispatched);
  const auto commands = port.commands();
  ASSERT_EQ(commands.size(), 1U);
  EXPECT_DOUBLE_EQ(std::get<FakeExecutionPort::SetScaleCall>(commands.front()).value, 0.25);
}

TEST(ExecutionPortTest, FencedDispatchTokenCannotSubmitAGoal) {
  FakeExecutionPort port;
  auto token = std::make_shared<ExecutionDispatchToken>();
  EXPECT_EQ(token->Fence(), ExecutionDispatchToken::State::kFenced);
  EXPECT_EQ(port.Submit(Key(1U), Trajectory(1.0), token).dispatch,
            ExecutionPortDispatch::kNotDispatched);
  EXPECT_TRUE(port.commands().empty());
}

TEST(ExecutionPortTest, FencedDispatchWaitsForExactNoSendAnswer) {
  ExecutionDispatchToken token;
  EXPECT_EQ(token.Fence(), ExecutionDispatchToken::State::kFenced);
  EXPECT_FALSE(token.WaitUntil(std::chrono::steady_clock::now()).has_value());
  token.Finish(ExecutionPortDispatch::kNotDispatched);
  EXPECT_EQ(token.WaitUntil(std::chrono::steady_clock::now()),
            ExecutionPortDispatch::kNotDispatched);
}

TEST(ExecutionPortTest, SampleCarriesNoGoalAttribution) {
  FakeExecutionPort port;
  ExecutionLaneFactsDTO facts;
  facts.joint_names = {"j1"};
  port.SetLaneFacts(facts);
  EXPECT_FALSE(port.ReadSample().has_value());
  ASSERT_EQ(port.Submit(Key(1U), Trajectory(2.0)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_EQ(port.Replace(Key(1U), Key(2U), Trajectory(0.5)).dispatch,
            ExecutionPortDispatch::kDispatched);

  ExecutionControllerSampleDTO sample;
  sample.source_stamp_ns = 5000;
  sample.local_receipt_stamp_ns = 5100;
  sample.local_receipt_count = ExecutionReceiptCount{3U};
  sample.binding_epoch = ExecutionBindingEpoch{1U};
  sample.cursor_s = 0.02;
  sample.applied_factor = 0.5;
  sample.reference_positions_rad = {0.4};
  sample.reference_velocities_rad_s = {0.1};
  sample.reference_accelerations_rad_s2 = {-1.0};
  sample.command_positions_rad = {0.4};
  sample.measured_positions_rad = {0.39};
  sample.error_positions_rad = {0.01};
  port.SetSample(sample);

  // The sample is controller-wide: which goal it belongs to (old trajectory,
  // replacement or cancel) is decided by the caller from evidence.
  const auto read = port.ReadSample();
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(*read, sample);
  // Vectors follow the binding's controller joint order, not a trajectory's.
  const auto joints = port.ReadLaneFacts().joint_names.size();
  EXPECT_EQ(read->reference_positions_rad.size(), joints);
  EXPECT_EQ(read->measured_positions_rad.size(), joints);
  EXPECT_FALSE(read->command_velocities_rad_s.has_value());
  EXPECT_FALSE(read->measured_velocities_rad_s.has_value());
  EXPECT_EQ(port.ReadGoal(Key(1U))->phase, ExecutionGoalPhase::kPending);
  EXPECT_EQ(port.ReadGoal(Key(2U))->phase, ExecutionGoalPhase::kPending);
}

TEST(ExecutionPortTest, ReadinessTimingAndLaneFactsAreScripted) {
  FakeExecutionPort port;
  port.SetReadiness({true, true, false, ExecutionBindingEpoch{4U}});
  ExecutionTimingDTO timing;
  timing.binding_epoch = ExecutionBindingEpoch{4U};
  timing.period_max_s = 0.012;
  port.SetTiming(timing);
  port.SetLaneFacts({{"j1", "j2"}, true, false, ExecutionCancelSemantics::kDecelerate, 10.0,
                     100.0, 150.0, ExecutionGoalTolerancePolicy::kKept});

  EXPECT_FALSE(port.ReadReadiness().hardware_active);
  EXPECT_EQ(port.ReadReadiness().binding_epoch, ExecutionBindingEpoch{4U});
  EXPECT_EQ(port.ReadTiming(), timing);
  EXPECT_FALSE(port.ReadTiming().period_mean_s.has_value());
  EXPECT_EQ(port.timing_reads(), 2U);
  EXPECT_EQ(port.ReadLaneFacts().goal_tolerance_policy, ExecutionGoalTolerancePolicy::kKept);
  EXPECT_TRUE(port.ReadLaneFacts().supports_scale);
  EXPECT_EQ(port.ReadLaneFacts().joint_names, (std::vector<std::string>{"j1", "j2"}));
  EXPECT_DOUBLE_EQ(port.ReadLaneFacts().controller_update_rate_hz, 150.0);

  ASSERT_EQ(port.Submit(Key(1U), Trajectory(1.0)).dispatch, ExecutionPortDispatch::kDispatched);
  EXPECT_EQ(port.ReadGoal(Key(1U))->binding_epoch, ExecutionBindingEpoch{4U});
}

// Merge 6 arrival: the one "robot at target" rule needs both facts.
TEST(RobotAtTarget, FinishedButOutOfToleranceIsNotArrived) {
  const std::vector<double> target{1.0, 2.0};
  const auto result = robot_at_target(true, std::vector<double>{1.0, 2.03}, target, 0.02);
  EXPECT_FALSE(result.arrived);
  EXPECT_NEAR(result.max_error_rad, 0.03, 1e-12);
  EXPECT_EQ(result.joint_index, 1U);
}

TEST(RobotAtTarget, WithinToleranceButNotFinishedIsNotArrived) {
  const std::vector<double> target{1.0, 2.0};
  const auto result = robot_at_target(false, std::vector<double>{1.0, 2.0}, target, 0.02);
  EXPECT_FALSE(result.arrived);
  EXPECT_DOUBLE_EQ(result.max_error_rad, 0.0);
}

TEST(RobotAtTarget, FinishedAndWithinToleranceIsArrived) {
  const std::vector<double> target{1.0, 2.0};
  EXPECT_TRUE(robot_at_target(true, std::vector<double>{1.005, 1.99}, target, 0.02).arrived);
  // The bound is inclusive, as the former terminal gate.
  EXPECT_TRUE(robot_at_target(true, std::vector<double>{1.5, 2.0}, target, 0.5).arrived);
}

TEST(RobotAtTarget, UnusablePositionsAreNotArrived) {
  const std::vector<double> target{1.0, 2.0};
  EXPECT_FALSE(robot_at_target(true, std::vector<double>{1.0}, target, 0.02).arrived);
  EXPECT_FALSE(robot_at_target(true, std::vector<double>{}, std::vector<double>{}, 0.02).arrived);
  EXPECT_FALSE(
      robot_at_target(true, std::vector<double>{1.0, std::nan("")}, target, 0.02).arrived);
}

}  // namespace
}  // namespace manyforge::core
