#include <gtest/gtest.h>

#include <manyforge_core/testing/addressed_request_fixtures.hpp>
#include <manyforge_roboplan/roboplan_planner_backend.hpp>

#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {
namespace {

using namespace manyforge::core;

[[nodiscard]] RoboPlanCandidateBindingDTO candidate_binding() {
  const auto resolved = testing::make_test_execution_binding();
  return RoboPlanCandidateBindingDTO{
      resolved.controllable_component_id,
      resolved.command_target_id,
      resolved.model_group_binding.model_instance_id,
      resolved.model_group_binding.model_revision,
      resolved.model_group_binding.compiled_group_name,
      resolved.resources,
      resolved.model_group_binding.compiled_joint_names,
      {resolved.base_frame, resolved.tcp_frame}};
}

[[nodiscard]] ResolvedJointGoalDTO joint_goal(
    const RoboPlanCandidateBindingDTO& binding) {
  return ResolvedJointGoalDTO{binding.resources,
                              binding.compiled_joint_names,
                              {0.25, -0.5}};
}

[[nodiscard]] TrajectoryDTO trajectory(
    const RoboPlanCandidateBindingDTO& binding) {
  return TrajectoryDTO{
      binding.resources,
      binding.compiled_joint_names,
      {TrajectoryWaypointDTO{0.0, {0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}},
       TrajectoryWaypointDTO{1.0, {0.25, -0.5}, {0.0, 0.0},
                             {0.0, 0.0}}}};
}

class RecordingCandidateClient final : public IRoboPlanCandidateClient {
 public:
  explicit RecordingCandidateClient(RoboPlanCandidateBindingDTO binding)
      : binding_(std::move(binding)), next_trajectory_(trajectory(binding_)) {}

  [[nodiscard]] const RoboPlanCandidateBindingDTO& binding()
      const noexcept override {
    return binding_;
  }

  [[nodiscard]] Result<TrajectoryDTO> compute(
      const RoboPlanCandidateRequestDTO& request) override {
    ++compute_calls;
    last_request = request;
    if (compute_error.has_value()) {
      return Result<TrajectoryDTO>::Err(*compute_error);
    }
    return Result<TrajectoryDTO>::Ok(next_trajectory_);
  }

  [[nodiscard]] Result<std::vector<double>> solve_ik(
      const Pose3DTO& goal, const ResolvedFrameDTO& base_frame,
      const ResolvedFrameDTO& tcp_frame,
      const ControlledStateDTO& seed_state,
      double timeout_s) override {
    ++ik_calls;
    last_goal = goal;
    last_base_frame = base_frame;
    last_tcp_frame = tcp_frame;
    last_seed = seed_state;
    last_timeout_s = timeout_s;
    if (ik_error.has_value()) {
      return Result<std::vector<double>>::Err(*ik_error);
    }
    return Result<std::vector<double>>::Ok(ik_solution);
  }

  void request_cancel() noexcept override { ++cancel_calls; }

  RoboPlanCandidateBindingDTO binding_;
  TrajectoryDTO next_trajectory_;
  std::vector<double> ik_solution{0.1, -0.2};
  std::optional<ErrorDTO> compute_error;
  std::optional<ErrorDTO> ik_error;
  std::optional<RoboPlanCandidateRequestDTO> last_request;
  std::optional<Pose3DTO> last_goal;
  std::optional<ResolvedFrameDTO> last_base_frame;
  std::optional<ResolvedFrameDTO> last_tcp_frame;
  std::optional<ControlledStateDTO> last_seed;
  double last_timeout_s{};
  int compute_calls{};
  int ik_calls{};
  int cancel_calls{};
};

struct BackendFixture final {
  RoboPlanCandidateBindingDTO binding{candidate_binding()};
  RecordingCandidateClient* client{};
  std::unique_ptr<RoboPlanPlannerBackend> backend;

  BackendFixture() {
    auto owned = std::make_unique<RecordingCandidateClient>(binding);
    client = owned.get();
    backend = std::make_unique<RoboPlanPlannerBackend>(
        testing::make_test_projection_binding(), binding, std::move(owned),
        RoboPlanPlannerBackendConfig{0.125});
  }
};

TEST(RoboPlanPlannerBackendTest,
     MapsTheExactAddressedRequestAndReturnsItsProjectionRevision) {
  BackendFixture fixture;
  auto request = testing::make_test_plan_request(
      "move:test", ResolvedMotionSpecDTO{joint_goal(fixture.binding)});
  request.planning_attempt_index = 7U;

  const auto result = fixture.backend->plan(request);

  ASSERT_TRUE(result.trajectory.has_value());
  EXPECT_FALSE(result.error.has_value());
  EXPECT_EQ(result.projection_revision,
            request.projection_binding.projection_revision);
  EXPECT_EQ(result.trajectory->resources, fixture.binding.resources);
  ASSERT_TRUE(fixture.client->last_request.has_value());
  EXPECT_EQ(fixture.client->last_request->controllable_component_id,
            request.controllable_component_id);
  EXPECT_EQ(fixture.client->last_request->command_target_id,
            request.command_target_id);
  EXPECT_EQ(fixture.client->last_request->model_instance_id,
            fixture.binding.model_instance_id);
  EXPECT_EQ(fixture.client->last_request->resources, request.resources);
  EXPECT_EQ(fixture.client->last_request->start_state,
            request.start_state);
  EXPECT_EQ(fixture.client->last_request->motion, request.motion);
  EXPECT_EQ(fixture.client->last_request->planning_attempt_index, 7U);
}

TEST(RoboPlanPlannerBackendTest,
     RejectsProjectionParticipantAndControlStateCrossingBeforeTransport) {
  BackendFixture fixture;
  auto request = testing::make_test_plan_request(
      "move:test", ResolvedMotionSpecDTO{joint_goal(fixture.binding)});

  request.projection_binding.projection_lease_id =
      ProjectionLeaseId("another-lease");
  auto result = fixture.backend->plan(request);
  ASSERT_TRUE(result.trajectory.has_value());
  EXPECT_FALSE(result.error.has_value());
  EXPECT_EQ(fixture.client->compute_calls, 1);

  request = testing::make_test_plan_request(
      "move:test", ResolvedMotionSpecDTO{joint_goal(fixture.binding)});
  request.projection_binding.projection_revision.graph_revision =
      GraphRevision("another-graph-revision");
  result = fixture.backend->plan(request);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->details.at("reason"),
            "native_planner_projection_binding_mismatch");
  EXPECT_EQ(fixture.client->compute_calls, 1);

  request = testing::make_test_plan_request(
      "move:test", ResolvedMotionSpecDTO{joint_goal(fixture.binding)});
  request.model_group_binding.compiled_group_name = "caller-group";
  result = fixture.backend->plan(request);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->details.at("reason"),
            "native_planner_participant_binding_mismatch");
  EXPECT_EQ(fixture.client->compute_calls, 1);

  request = testing::make_test_plan_request(
      "move:test", ResolvedMotionSpecDTO{joint_goal(fixture.binding)});
  request.start_state.control_route_id = ControlRouteId("stale-route");
  result = fixture.backend->plan(request);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->details.at("reason"),
            "native_planner_start_state_binding_mismatch");
  EXPECT_EQ(fixture.client->compute_calls, 1);
}

TEST(RoboPlanPlannerBackendTest,
     RejectsMalformedWorkerTrajectoryWithoutPublishingProjectionSuccess) {
  BackendFixture fixture;
  auto request = testing::make_test_plan_request(
      "move:test", ResolvedMotionSpecDTO{joint_goal(fixture.binding)});
  fixture.client->next_trajectory_.joint_names.front() = "foreign-joint";

  const auto result = fixture.backend->plan(request);

  ASSERT_TRUE(result.error.has_value());
  EXPECT_FALSE(result.trajectory.has_value());
  EXPECT_FALSE(result.projection_revision.valid());
  EXPECT_EQ(result.error->code, kErrorInvariantViolation);
  EXPECT_EQ(result.error->details.at("reason"),
            "native_planner_result_invalid");
}

TEST(RoboPlanPlannerBackendTest,
     MapsSingleShotIkDefaultsAndForwardsNonBlockingCancellation) {
  BackendFixture fixture;
  const auto resolved = testing::make_test_execution_binding();
  const Pose3DTO goal{{0.1, 0.2, 0.3}, {0.0, 0.0, 0.0, 1.0}};
  IkRequestDTO request{
      resolved.controllable_component_id,
      resolved.command_target_id,
      resolved.model_bindings,
      resolved.resources,
      goal,
      resolved.base_frame,
      resolved.tcp_frame,
      testing::make_test_controlled_state(resolved, {0.0, 0.0}),
      testing::make_test_projection_binding(),
      0.0};

  const auto result = fixture.backend->solve_ik(request);
  fixture.backend->request_cancel();

  ASSERT_TRUE(result.ok);
  EXPECT_EQ(result.resources, fixture.binding.resources);
  EXPECT_EQ(result.joint_names, fixture.binding.compiled_joint_names);
  EXPECT_EQ(result.positions_rad, fixture.client->ik_solution);
  EXPECT_EQ(fixture.client->last_goal, goal);
  EXPECT_EQ(fixture.client->last_base_frame, request.base_frame);
  EXPECT_EQ(fixture.client->last_tcp_frame, request.tcp_frame);
  EXPECT_EQ(fixture.client->last_seed, request.seed_state);
  EXPECT_DOUBLE_EQ(fixture.client->last_timeout_s, 0.125);
  EXPECT_EQ(fixture.client->cancel_calls, 1);
}

TEST(RoboPlanPlannerBackendTest,
     AcceptsAnySealedOperationFrameAndRejectsForeignFramesBeforeTransport) {
  const auto resolved = testing::make_test_execution_binding();
  auto binding = candidate_binding();
  const auto alternate = ResolvedComponentModelFrameDTO{
      resolved.controllable_component_id,
      binding.model_instance_id,
      FrameRoleId("frame-role:inspection"),
      "inspection_frame",
      binding.model_revision};
  binding.permitted_frames.push_back(alternate);
  auto owned_client = std::make_unique<RecordingCandidateClient>(binding);
  auto* client = owned_client.get();
  RoboPlanPlannerBackend backend(testing::make_test_projection_binding(),
                                 binding, std::move(owned_client),
                                 RoboPlanPlannerBackendConfig{0.125});
  const Pose3DTO goal{{0.1, 0.2, 0.3}, {0.0, 0.0, 0.0, 1.0}};
  IkRequestDTO request{
      resolved.controllable_component_id,
      resolved.command_target_id,
      resolved.model_bindings,
      resolved.resources,
      goal,
      alternate,
      resolved.tcp_frame,
      testing::make_test_controlled_state(resolved, {0.0, 0.0}),
      testing::make_test_projection_binding(),
      0.1};

  const auto accepted = backend.solve_ik(request);

  ASSERT_TRUE(accepted.ok);
  EXPECT_EQ(client->ik_calls, 1);
  EXPECT_EQ(client->last_base_frame, request.base_frame);

  request.base_frame = ResolvedComponentModelFrameDTO{
      resolved.controllable_component_id,
      binding.model_instance_id,
      FrameRoleId("frame-role:foreign"),
      "foreign_frame",
      binding.model_revision};
  const auto rejected = backend.solve_ik(request);

  EXPECT_FALSE(rejected.ok);
  ASSERT_TRUE(rejected.error.has_value());
  EXPECT_EQ(rejected.error->details.at("reason"),
            "native_ik_participant_binding_mismatch");
  EXPECT_EQ(client->ik_calls, 1);
}

TEST(RoboPlanPlannerBackendTest,
     ConstructorRejectsTransportForAnotherRetainedParticipant) {
  auto binding = candidate_binding();
  auto other = binding;
  other.command_target_id = CommandTargetId("another-target");
  auto client = std::make_unique<RecordingCandidateClient>(std::move(other));

  EXPECT_THROW(
      (void)RoboPlanPlannerBackend(testing::make_test_projection_binding(),
                                   binding, std::move(client)),
      std::invalid_argument);
}

}  // namespace
}  // namespace manyforge::roboplan_backend
