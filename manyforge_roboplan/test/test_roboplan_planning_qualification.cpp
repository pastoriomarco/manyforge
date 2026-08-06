#include <gtest/gtest.h>

#include <manyforge_roboplan/roboplan_planning_qualification.hpp>
#include <manyforge_core/testing/addressed_request_fixtures.hpp>

#include <chrono>
#include <string>

using namespace manyforge::roboplan_backend;
using namespace manyforge::core;

TEST(RoboPlanPlanningQualificationRecorderTest,
     PreservesExactRequestAndTracksInvocationLifetime) {
  RoboPlanPlanningQualificationRecorder recorder(2);
  auto request = manyforge::core::testing::make_test_plan_request(
      "drop_approach",
      ResolvedJointGoalDTO{{ResourceId("resource:joint_1")},
                           {"joint_1"}, {0.5}},
      {"joint_1"}, {0.25});
  request.projection_binding.projection_lease_id =
      ProjectionLeaseId{"lease-seven"};
  PoseGoalDTO goal;
  goal.goal.position_m = {0.1, 0.2, 0.3};
  request.motion = goal;
  PlanResultDTO result;
  result.projection_revision =
      request.projection_binding.projection_revision;
  RoboPlanPlanningPipelineMetrics metrics;
  metrics.total_duration_ns = 42;

  EXPECT_TRUE(recorder.try_begin_invocation(request));
  EXPECT_TRUE(recorder.wait_until_active(std::chrono::milliseconds(1)));
  EXPECT_EQ(recorder.in_flight(), 1U);
  EXPECT_FALSE(recorder.wait_until_idle(std::chrono::milliseconds(1)));
  recorder.record(request, result, metrics);
  EXPECT_TRUE(recorder.wait_until_idle(std::chrono::milliseconds(1)));

  auto records = recorder.drain();
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(records[0].sequence, 1U);
  EXPECT_EQ(records[0].request.move_id.value(), "drop_approach");
  EXPECT_EQ(
      records[0].request.projection_binding.projection_lease_id.value(),
      "lease-seven");
  EXPECT_EQ(records[0].metrics.total_duration_ns, 42U);
  EXPECT_EQ(recorder.dropped_total(), 0U);
}

TEST(RoboPlanPlanningQualificationRecorderTest,
     BoundedQueueReportsEveryEviction) {
  RoboPlanPlanningQualificationRecorder recorder(1);
  auto request = manyforge::core::testing::make_test_plan_request(
      "move_0",
      ResolvedJointGoalDTO{{ResourceId("resource:joint_1")},
                           {"joint_1"}, {0.5}},
      {"joint_1"}, {0.25});
  PlanResultDTO result;
  RoboPlanPlanningPipelineMetrics metrics;
  for (int index = 0; index < 2; ++index) {
    request.move_id = MoveId("move_" + std::to_string(index));
    EXPECT_TRUE(recorder.try_begin_invocation(request));
    recorder.record(request, result, metrics);
  }

  auto batch = recorder.drain_with_status();
  ASSERT_EQ(batch.records.size(), 1U);
  EXPECT_EQ(batch.records[0].sequence, 2U);
  EXPECT_EQ(batch.records[0].request.move_id.value(), "move_1");
  EXPECT_EQ(batch.dropped_total, 1U);
  EXPECT_EQ(batch.in_flight, 0U);
}

TEST(RoboPlanPlanningQualificationRecorderTest,
     ClosedAdmissionRejectsRetainedObserverSnapshots) {
  RoboPlanPlanningQualificationRecorder recorder(2);
  auto request = manyforge::core::testing::make_test_plan_request(
      "late_move",
      ResolvedJointGoalDTO{{ResourceId("resource:joint_1")},
                           {"joint_1"}, {0.5}},
      {"joint_1"}, {0.25});

  recorder.close_admission();

  EXPECT_FALSE(recorder.wait_until_active(std::chrono::milliseconds(1)));
  EXPECT_FALSE(recorder.try_begin_invocation(request));
  const auto batch = recorder.drain_with_status();
  EXPECT_TRUE(batch.records.empty());
  EXPECT_EQ(batch.dropped_total, 0U);
  EXPECT_EQ(batch.in_flight, 0U);
}
