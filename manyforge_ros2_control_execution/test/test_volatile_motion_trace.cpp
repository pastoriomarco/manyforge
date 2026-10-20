#include <gtest/gtest.h>
#include <limits>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_ros2_control_execution/volatile_motion_trace.hpp>

namespace manyforge::ros2_control_execution {
namespace {
using namespace core;
using V = content_identity::CanonicalValue;
ExecutionGoalKey Key(const char *name, std::uint64_t generation = 1U) {
  return {ControllerGoalId(name), ControllerGoalGeneration(generation)};
}
VolatileMotionTrace::Goal Goal(double position = 1.0) {
  VolatileMotionTrace::Goal goal;
  goal.trajectory.joint_names = {"j1"};
  trajectory_msgs::msg::JointTrajectoryPoint point;
  point.time_from_start.sec = -1;
  point.time_from_start.nanosec = 500000000U;
  point.positions = {position};
  point.velocities = {0.25};
  point.accelerations = {-0.5};
  goal.trajectory.points.push_back(std::move(point));
  return goal;
}
const V &Member(const V &value, const char *name) {
  const auto *member = value.find_object_member(name);
  if (!member)
    throw std::logic_error(name);
  return *member;
}
TEST(VolatileMotionTraceTest,
     EarlyFeedbackSurvivesPayloadAttachmentAndSignedPointsRemainWhole) {
  VolatileMotionTrace trace;
  VolatileMotionTrace::Garbage garbage;
  const auto key = Key("goal.one");
  auto token =
      trace.Begin("/arm/fjt", ExecutionBindingEpoch(7U), key, {}, 10, garbage);
  ASSERT_TRUE(token);
  ExecutionGoalObservationDTO facts;
  facts.key = key;
  facts.binding_epoch = ExecutionBindingEpoch(7U);
  facts.mechanism_goal_id = MechanismGoalId("uuid.actual");
  facts.accepted_stamp_ns = 11;
  facts.latest_feedback = ExecutionGoalFeedbackDTO{};
  facts.latest_feedback->local_receipt_stamp_ns = 12;
  trace.Observe(token, facts); // Callback before the SDK send returns.
  auto goal = Goal();
  trace.Attach(token, std::move(goal), 13, ExecutionPortDispatch::kDispatched,
               garbage);
  const auto result = trace.Snapshot(RequestOwnerEpoch("owner.actual"),
                                     VolatileMotionTrace::kMaximumTraceBytes);
  ASSERT_TRUE(result.ok()) << result.error().message;
  const auto &row = Member(result.value(), "goals").as_array().front();
  EXPECT_EQ(Member(row, "mechanism_goal_id").as_string(), "uuid.actual");
  EXPECT_EQ(Member(row, "accepted_at_ns").as_number(), 11.0);
  EXPECT_EQ(Member(row, "first_own_feedback_at_ns").as_number(), 12.0);
  EXPECT_EQ(Member(row, "installation").as_string(), "unknown");
  const auto &point =
      Member(Member(row, "trajectory"), "points").as_array().front();
  EXPECT_EQ(Member(point, "time_sec").as_number(), -1.0);
  EXPECT_EQ(Member(point, "time_nanosec").as_number(), 500000000.0);
  EXPECT_EQ(Member(point, "p").as_array().front().as_number(), 1.0);
  const auto encoded = content_identity::canonical_json(result.value());
  EXPECT_EQ(
      Member(Member(result.value(), "coverage"), "exported_bytes").as_number(),
      static_cast<double>(encoded.size()));
  EXPECT_TRUE(goal.trajectory.points.empty());
}
TEST(VolatileMotionTraceTest,
     OnlyExactStopGenerationCanInheritOperationContext) {
  VolatileMotionTrace trace;
  VolatileMotionTrace::Garbage garbage;
  auto parent =
      std::make_shared<VolatileMotionTrace::Token>(VolatileMotionTrace::Token{
          "/arm/fjt", ExecutionBindingEpoch(7U), Key("goal.one"),
          VolatileMotionTrace::Context{"op.actual", "participant.actual",
                                       "run.actual", "binding.actual",
                                       "robot.actual", 3U}});
  auto stop = trace.Begin("/arm/fjt", ExecutionBindingEpoch(7U),
                          Key("goal.one", 2U), parent, 10, garbage);
  auto newcomer = trace.Begin("/arm/fjt", ExecutionBindingEpoch(7U),
                              Key("goal.new"), parent, 11, garbage);
  auto epoch = trace.Begin("/arm/fjt", ExecutionBindingEpoch(8U),
                           Key("goal.one", 2U), parent, 12, garbage);
  ASSERT_TRUE(stop);
  ASSERT_TRUE(newcomer);
  ASSERT_TRUE(epoch);
  ASSERT_TRUE(stop->context);
  EXPECT_EQ(stop->context->operation_id, "op.actual");
  EXPECT_FALSE(newcomer->context);
  EXPECT_FALSE(epoch->context);
}
TEST(VolatileMotionTraceTest,
     UnsignedIdentityBoundariesRemainExactAndDistinct) {
  constexpr std::uint64_t safe = 9007199254740991ULL;
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  VolatileMotionTrace trace;
  VolatileMotionTrace::Garbage garbage;
  auto parent =
      std::make_shared<VolatileMotionTrace::Token>(VolatileMotionTrace::Token{
          "/arm/fjt", ExecutionBindingEpoch(safe + 2U), Key("goal.exact", safe),
          VolatileMotionTrace::Context{"op.actual", "participant.actual",
                                       "run.actual", "binding.actual",
                                       "robot.actual", maximum}});
  auto first = trace.Begin("/arm/fjt", ExecutionBindingEpoch(safe + 2U),
                           Key("goal.exact", safe + 1U), parent, 10, garbage);
  auto second = trace.Begin("/arm/fjt", ExecutionBindingEpoch(safe + 2U),
                            Key("goal.exact", safe + 2U), first, 11, garbage);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  trace.Attach(first, Goal(), 12, ExecutionPortDispatch::kDispatched, garbage);
  trace.Attach(second, Goal(), 13, ExecutionPortDispatch::kDispatched, garbage);
  const auto result = trace.Snapshot(RequestOwnerEpoch("owner"),
                                     VolatileMotionTrace::kMaximumTraceBytes);
  ASSERT_TRUE(result.ok());
  const auto &rows = Member(result.value(), "goals").as_array();
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(Member(rows[0], "controller_goal_generation").as_string(),
            "9007199254740993");
  EXPECT_EQ(Member(rows[1], "controller_goal_generation").as_string(),
            "9007199254740992");
  EXPECT_EQ(Member(rows[0], "binding_epoch").as_string(), "9007199254740993");
  EXPECT_EQ(Member(rows[0], "operation_revision").as_string(),
            "18446744073709551615");
  EXPECT_EQ(Member(Member(rows[0], "predecessor"), "controller_goal_generation")
                .as_string(),
            "9007199254740992");
  EXPECT_EQ(Member(Member(rows[1], "predecessor"), "controller_goal_generation")
                .as_number(),
            static_cast<double>(safe));
}
TEST(VolatileMotionTraceTest,
     EvictionDetachesPayloadAndCallbackTokenDoesNotPinIt) {
  VolatileMotionTrace trace;
  VolatileMotionTrace::Garbage garbage;
  auto first = trace.Begin("/arm/fjt", ExecutionBindingEpoch(1U), Key("first"),
                           {}, 1, garbage);
  trace.Attach(first, Goal(), 2, ExecutionPortDispatch::kDispatched, garbage);
  for (std::size_t i = 0; i < VolatileMotionTrace::kGoalLimit; ++i) {
    auto token = trace.Begin("/arm/fjt", ExecutionBindingEpoch(1U),
                             {ControllerGoalId("goal." + std::to_string(i)),
                              ControllerGoalGeneration(1U)},
                             {}, 3, garbage);
    trace.Attach(token, Goal(), 4, ExecutionPortDispatch::kDispatched, garbage);
  }
  ASSERT_EQ(garbage.size(), 1U);
  std::weak_ptr<const VolatileMotionTrace::Goal> retired = garbage.front();
  garbage.clear();
  EXPECT_TRUE(retired.expired());
  EXPECT_TRUE(first);
  const auto result = trace.Snapshot(RequestOwnerEpoch("owner"),
                                     VolatileMotionTrace::kMaximumTraceBytes);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(Member(result.value(), "goals").as_array().size(), 16U);
  EXPECT_EQ(
      Member(Member(result.value(), "coverage"), "evicted_goals").as_number(),
      1.0);
}
TEST(VolatileMotionTraceTest,
     TinyAllowanceRefusesAndLargerCutoffOmitsWholeGoal) {
  VolatileMotionTrace trace;
  VolatileMotionTrace::Garbage garbage;
  auto token = trace.Begin("/arm/fjt", ExecutionBindingEpoch(1U), Key("large"),
                           {}, 1, garbage);
  auto goal = Goal();
  goal.trajectory.points.resize(200U, goal.trajectory.points.front());
  trace.Attach(token, std::move(goal), 2, ExecutionPortDispatch::kDispatched,
               garbage);
  EXPECT_FALSE(trace.Snapshot(RequestOwnerEpoch("owner"), 1U).ok());
  const auto result = trace.Snapshot(RequestOwnerEpoch("owner"), 2000U);
  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(Member(result.value(), "goals").as_array().empty());
  EXPECT_TRUE(
      Member(Member(result.value(), "coverage"), "truncated").as_bool());
  EXPECT_EQ(Member(Member(result.value(), "coverage"), "omitted_goals")
                .as_array()
                .size(),
            1U);
  EXPECT_LE(content_identity::canonical_json(result.value()).size(), 2000U);
}
TEST(VolatileMotionTraceTest,
     LifetimeChargeIsBoundedUntilDetachedPinsAreReleased) {
  VolatileMotionTrace trace;
  VolatileMotionTrace::Garbage garbage;
  std::vector<VolatileMotionTrace::TokenPtr> pins;
  bool exhausted = false;
  for (int i = 0; i < 160; ++i) {
    auto token = trace.Begin("/arm/fjt", ExecutionBindingEpoch(1U),
                             {ControllerGoalId("pinned." + std::to_string(i)),
                              ControllerGoalGeneration(1U)},
                             {}, 1, garbage);
    if (!token) {
      exhausted = true;
      break;
    }
    trace.Attach(token, Goal(), 2, ExecutionPortDispatch::kDispatched, garbage);
    pins.push_back(std::move(token));
  }
  ASSERT_TRUE(exhausted);
  EXPECT_FALSE(trace.Begin("/arm/fjt", ExecutionBindingEpoch(1U),
                           Key("blocked"), {}, 3, garbage));
  pins.clear();
  garbage.clear();
  auto resumed = trace.Begin("/arm/fjt", ExecutionBindingEpoch(1U),
                             Key("after_release"), {}, 4, garbage);
  ASSERT_TRUE(resumed);
  trace.Attach(resumed, Goal(), 5, ExecutionPortDispatch::kDispatched, garbage);
  const auto result = trace.Snapshot(RequestOwnerEpoch("owner"),
                                     VolatileMotionTrace::kMaximumTraceBytes);
  ASSERT_TRUE(result.ok());
  EXPECT_GE(Member(Member(result.value(), "coverage"), "omitted_captures")
                .as_number(),
            2.0);
  EXPECT_EQ(Member(Member(result.value(), "goals").as_array().front(),
                   "controller_goal_id")
                .as_string(),
            "after_release");
}
TEST(VolatileMotionTraceTest,
     ResidentByteBoundEvictsAndOversizedCaptureHasNoPartialPoints) {
  VolatileMotionTrace trace;
  VolatileMotionTrace::Garbage garbage;
  for (int i = 0; i < 8; ++i) {
    auto token = trace.Begin("/arm/fjt", ExecutionBindingEpoch(1U),
                             {ControllerGoalId("large." + std::to_string(i)),
                              ControllerGoalGeneration(1U)},
                             {}, 1, garbage);
    auto goal = Goal();
    goal.trajectory.points.resize(5000U, goal.trajectory.points.front());
    trace.Attach(token, std::move(goal), 2, ExecutionPortDispatch::kDispatched,
                 garbage);
    garbage.clear();
  }
  const auto bounded = trace.Snapshot(RequestOwnerEpoch("owner"), 2000U);
  ASSERT_TRUE(bounded.ok());
  EXPECT_GT(
      Member(Member(bounded.value(), "coverage"), "evicted_goals").as_number(),
      0.0);

  VolatileMotionTrace oversize;
  auto token = oversize.Begin("/arm/fjt", ExecutionBindingEpoch(1U),
                              Key("oversize"), {}, 1, garbage);
  auto goal = Goal();
  goal.trajectory.points.front().positions.resize(
      VolatileMotionTrace::kGoalBytes / sizeof(double) + 1U);
  oversize.Attach(token, std::move(goal), 2, ExecutionPortDispatch::kDispatched,
                  garbage);
  const auto result = oversize.Snapshot(RequestOwnerEpoch("owner"), 2000U);
  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(Member(result.value(), "goals").as_array().empty());
  const auto &omissions =
      Member(Member(result.value(), "coverage"), "omitted_goals").as_array();
  ASSERT_EQ(omissions.size(), 1U);
  EXPECT_EQ(Member(omissions.front(), "reason").as_string(),
            "payload_not_retained");
}
TEST(VolatileMotionTraceTest,
     SamplesHaveIndependentBoundsAndScalePublicationIsDistinct) {
  VolatileMotionTrace trace;
  ExecutionControllerSampleDTO sample;
  sample.binding_epoch = ExecutionBindingEpoch(1U);
  sample.applied_factor = 0.5;
  sample.reference_positions_rad = {1.0};
  sample.command_positions_rad = {1.0};
  sample.measured_positions_rad = {0.99};
  for (std::size_t i = 0; i < VolatileMotionTrace::kSampleLimit + 3U; ++i) {
    sample.local_receipt_stamp_ns = static_cast<TimePoint>(i);
    trace.Sample("/arm/fjt", sample);
  }
  trace.PublishedScale("/arm/fjt", ExecutionBindingEpoch(1U), 2000, 0.25);
  const auto result = trace.Snapshot(RequestOwnerEpoch("owner"),
                                     VolatileMotionTrace::kMaximumTraceBytes);
  ASSERT_TRUE(result.ok());
  EXPECT_LE(Member(result.value(), "controller_samples").as_array().size(),
            VolatileMotionTrace::kSampleLimit);
  EXPECT_GE(
      Member(Member(result.value(), "coverage"), "dropped_samples").as_number(),
      3.0);
  const auto &events = Member(result.value(), "scale_events").as_array();
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(Member(events.front(), "kind").as_string(), "published");
  EXPECT_EQ(Member(events.back(), "kind").as_string(), "applied_observed");
}
} // namespace
} // namespace manyforge::ros2_control_execution
