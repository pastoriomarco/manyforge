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
  EXPECT_FALSE(fixture.client->last_request->moving_replacement);
}

TEST(RoboPlanPlannerBackendTest,
     MapsMovingSourceWithoutReplacingCapturedStartAndChargesSetupBudget) {
  BackendFixture fixture;
  auto request = testing::make_test_plan_request(
      "move:moving", ResolvedMotionSpecDTO{joint_goal(fixture.binding)});
  auto suffix_seed = request.start_state;
  suffix_seed.positions[0] = AngularPositionDTO{0.5};
  request.moving_replacement = MovingReplacementInputDTO{
      ExecutionGoalKey{ControllerGoalId("source-goal"),
                       ControllerGoalGeneration(13U)},
      ExecutionBindingEpoch(7U),
      1.0,
      trajectory(fixture.binding),
      2.0,
      suffix_seed,
      request.base_frame,
      request.tcp_frame,
      SpeedProfileDTO{0.9, 0.8, 0.3},
      request.per_resource_dynamics_limits,
      3.0};

  const auto result = fixture.backend->plan(request);

  ASSERT_TRUE(result.trajectory);
  ASSERT_TRUE(fixture.client->last_request);
  ASSERT_TRUE(fixture.client->last_request->moving_replacement);
  const auto &actual = *fixture.client->last_request->moving_replacement;
  const auto &expected = *request.moving_replacement;
  EXPECT_EQ(fixture.client->last_request->start_state, request.start_state);
  EXPECT_EQ(actual.source_goal, expected.source_goal);
  EXPECT_EQ(actual.source_binding_epoch, expected.source_binding_epoch);
  EXPECT_EQ(actual.source_cursor_s, expected.source_cursor_s);
  EXPECT_EQ(actual.divergence_cursor_s, expected.divergence_cursor_s);
  EXPECT_EQ(actual.source_prefix_trajectory.resources,
            expected.source_prefix_trajectory.resources);
  EXPECT_EQ(actual.source_prefix_trajectory.joint_names,
            expected.source_prefix_trajectory.joint_names);
  ASSERT_EQ(actual.source_prefix_trajectory.waypoints.size(), 2U);
  EXPECT_EQ(actual.source_prefix_trajectory.waypoints[1].positions_rad,
            expected.source_prefix_trajectory.waypoints[1].positions_rad);
  EXPECT_EQ(actual.suffix_start_state, expected.suffix_start_state);
  EXPECT_EQ(actual.source_base_frame, expected.source_base_frame);
  EXPECT_EQ(actual.source_tcp_frame, expected.source_tcp_frame);
  EXPECT_EQ(actual.source_speed_profile, expected.source_speed_profile);
  EXPECT_EQ(actual.source_per_resource_dynamics_limits,
            expected.source_per_resource_dynamics_limits);
  EXPECT_GT(actual.remaining_preparation_budget_s, 0.0);
  EXPECT_LT(actual.remaining_preparation_budget_s, 3.0);
  EXPECT_FALSE(actual.remaining_planning_budget_s);

  request.moving_replacement->remaining_planning_budget_s = 0.5;
  ASSERT_TRUE(fixture.backend->plan(request).trajectory);
  const auto& capped = *fixture.client->last_request->moving_replacement;
  ASSERT_TRUE(capped.remaining_planning_budget_s);
  EXPECT_GT(*capped.remaining_planning_budget_s, 0.0);
  EXPECT_LT(*capped.remaining_planning_budget_s, 0.5);
  // One setup interval is charged to both budgets, rather than starting P here.
  EXPECT_NEAR(3.0 - capped.remaining_preparation_budget_s,
              0.5 - *capped.remaining_planning_budget_s, 1e-12);
}

TEST(RoboPlanPlannerBackendTest,
     ProductRebindKeepsTheWorkerAndRejectsAStalePredecessor) {
  BackendFixture fixture;
  const auto predecessor = fixture.backend->projection_binding();
  auto candidate = predecessor;
  candidate.projection_revision.graph_revision = GraphRevision("graph.next");
  candidate.projection_lease_id = ProjectionLeaseId("lease.next");
  ASSERT_TRUE(candidate.valid());

  const auto stale = fixture.backend->RebindProjection(candidate, predecessor);
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().details.at("reason"),
            "native_planner_product_rebind_stale_predecessor");
  ASSERT_TRUE(fixture.backend->RebindProjection(predecessor, candidate).ok());
  EXPECT_EQ(fixture.backend->projection_binding(), candidate);

  auto request = testing::make_test_plan_request(
      "move:next-product", ResolvedMotionSpecDTO{joint_goal(fixture.binding)});
  request.projection_binding = predecessor;
  const auto stale_request_result = fixture.backend->plan(request);
  ASSERT_TRUE(stale_request_result.error.has_value());
  EXPECT_EQ(stale_request_result.error->details.at("reason"),
            "native_planner_projection_binding_mismatch");
  EXPECT_EQ(fixture.client->compute_calls, 0);

  request.projection_binding = candidate;
  const auto result = fixture.backend->plan(request);
  ASSERT_TRUE(result.trajectory.has_value());
  EXPECT_EQ(fixture.client->compute_calls, 1);
}

TEST(RoboPlanPlannerBackendTest, AcceptsAnExactProviderGenerationWithoutInventingARuntimeUnit) {
  BackendFixture fixture;
  auto request = testing::make_test_plan_request(
      "move:embedded-provider", ResolvedMotionSpecDTO{joint_goal(fixture.binding)});
  request.runtime_unit_bindings.clear();

  const auto result = fixture.backend->plan(request);

  ASSERT_TRUE(result.trajectory.has_value());
  EXPECT_FALSE(result.error.has_value());
  EXPECT_EQ(fixture.client->compute_calls, 1);
  EXPECT_FALSE(request.provider_runtime_epoch.empty());
  EXPECT_FALSE(request.planner_capability_binding.provider_runtime_epoch.empty());
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
