#include <gtest/gtest.h>

#include "roboplan_worker_protocol.hpp"

#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::roboplan_backend::worker_protocol {
namespace {

using namespace manyforge::core;

[[nodiscard]] RoboPlanCandidateBindingDTO binding() {
  const ControllableComponentId component("component-a");
  const ModelInstanceId model("model-a");
  const ModelRevision revision("model-revision-a");
  return RoboPlanCandidateBindingDTO{
      component,
      CommandTargetId("arm-target"),
      model,
      revision,
      "compiled-arm",
      {ResourceId("joint-1"), ResourceId("joint-2")},
      {"compiled-joint-1", "compiled-joint-2"},
      {ResolvedCellFrameDTO{CellId("cell-a"), "cell-frame"},
       ResolvedComponentModelFrameDTO{
           component, model, FrameRoleId("tcp"), "compiled-tcp",
           revision}}};
}

[[nodiscard]] ControlledStateDTO state() {
  const auto selected = binding();
  return ControlledStateDTO{
      .controllable_component_id = selected.controllable_component_id,
      .command_target_id = selected.command_target_id,
      .control_domain_id = ControlDomainId("domain-a"),
      .control_domain_revision = ControlDomainRevision("domain-revision-a"),
      .control_binding_id = ControlBindingId("binding-a"),
      .control_binding_revision =
          ControlBindingRevision("binding-revision-a"),
      .controller_topology_id = ControllerTopologyId("topology-a"),
      .topology_revision = TopologyRevision("topology-revision-a"),
      .control_route_id = ControlRouteId("route-a"),
      .active_control_configuration_id =
          ActiveControlConfigurationId("configuration-a"),
      .active_control_configuration_generation =
          ActiveControlConfigurationGeneration(9U),
      .resources = selected.resources,
      .joint_names = selected.compiled_joint_names,
      .positions = {AngularPositionDTO{0.25},
                    LinearPositionDTO{0.012}},
      .velocities =
          std::vector<CoordinateVelocityDTO>{
              AngularVelocityDTO{0.5}, LinearVelocityDTO{0.02}}};
}

[[nodiscard]] RoboPlanCandidateRequestDTO candidate_request() {
  const auto selected = binding();
  CartesianPathDTO path;
  path.waypoints = {{{0.1, 0.2, 0.3}, {0.0, 0.0, 0.0, 1.0}},
                    {{0.2, 0.3, 0.4}, {0.0, 0.0, 0.5, 0.5}}};
  path.max_translation_step_m = 0.004;
  path.max_rotation_step_rad = 0.03;
  path.max_angular_speed_rad_s = 0.4;
  path.max_linear_acceleration_mps2 = 0.3;
  path.max_angular_acceleration_rad_s2 = 1.5;
  path.max_blend_deviation_rad = 0.02;
  path.fallback_policy =
      CartesianFallbackPolicyDTO::kAllowJointSpaceFallback;
  return RoboPlanCandidateRequestDTO{
      .controllable_component_id = selected.controllable_component_id,
      .command_target_id = selected.command_target_id,
      .model_instance_id = selected.model_instance_id,
      .model_revision = selected.model_revision,
      .resources = selected.resources,
      .base_frame = selected.permitted_frames[0],
      .tcp_frame = selected.permitted_frames[1],
      .start_state = state(),
      .motion = std::move(path),
      .tolerances = TolerancesDTO{1.0e-4, 2.0e-3, 3.0e-3},
      .speed_profile = SpeedProfileDTO{0.7, 0.6, 0.15},
      .planning_profile = PlanningProfileDTO::kQualityBounded,
      .dynamics_limits =
          {ResolvedResourceDynamicsLimitDTO{
               selected.resources[0], selected.compiled_joint_names[0],
               JointDynamicsLimitDTO{1.2, 2.3, std::nullopt}},
           ResolvedResourceDynamicsLimitDTO{
               selected.resources[1], selected.compiled_joint_names[1],
               JointDynamicsLimitDTO{1.3, 2.4, std::nullopt}}},
      .planning_timeout_s = 4.5,
      .planning_attempt_index = 7U,
      .action_span =
          ActionSpanDTO{ActionSource::kProgramNode, "span-a", "run-a",
                        "node-a", "attempt-a", "parent-a"}};
}

TEST(RoboPlanWorkerProtocolTest,
     InitializationRoundTripsResolvedModelAndCandidateBinding) {
  WorkerInitializationDTO input{
      RoboPlanWorkerModelDTO{
          "scene-a",
          "/retained/model.urdf",
          "/retained/model.srdf",
          {"/retained", "/resources"},
          {RoboPlanJointLimitInput{
              "joint-a", std::vector<double>{-1.0},
              std::vector<double>{1.0}, std::vector<double>{2.0},
              std::vector<double>{3.0}}},
          {RoboPlanWorkerGeometryDTO{
               "box-a", "cell-frame", BoxShapeDTO{{1.0, 2.0, 3.0}},
               {{0.1, 0.2, 0.3}, {0.0, 0.0, 0.0, 1.0}}, std::nullopt, std::nullopt},
           RoboPlanWorkerGeometryDTO{
               "sphere-a", "cell-frame", SphereShapeDTO{0.25},
               {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}}, std::nullopt,
               std::nullopt},
           RoboPlanWorkerGeometryDTO{
               "cylinder-a", "cell-frame", CylinderShapeDTO{0.2, 0.7},
               {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}}, std::nullopt,
               std::nullopt},
           RoboPlanWorkerGeometryDTO{
               "mesh-a", "cell-frame",
               MeshShapeDTO{"asset://mesh-a", {1.0, 1.5, 2.0}},
               {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}},
               "/resources/mesh-a.stl", std::nullopt},
           RoboPlanWorkerGeometryDTO{
               "scene-mesh-a", "cell-frame",
               MeshShapeDTO{"examples/x/meshes/a.obj", {1.0, 1.5, 1.15}},
               {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}}, std::nullopt,
               std::string("sha256:"
                           "2222222222222222222222222222222222222222222222222"
                           "222222222222222")}},
          {{"body-a", "body-b"}, {"body-c", "body-d"}}},
      binding(), RoboPlanCandidateComputerConfig{0x1234U, 4096U, 0.02, 0.0075}};

  const auto decoded = decode_initialization(encode_initialization(input));

  EXPECT_EQ(decoded.model.native_scene_name, input.model.native_scene_name);
  EXPECT_EQ(decoded.model.urdf_path, input.model.urdf_path);
  EXPECT_EQ(decoded.model.srdf_path, input.model.srdf_path);
  EXPECT_EQ(decoded.model.package_paths, input.model.package_paths);
  EXPECT_EQ(decoded.model.joint_limits, input.model.joint_limits);
  EXPECT_EQ(decoded.model.allowed_collision_pairs,
            input.model.allowed_collision_pairs);
  ASSERT_EQ(decoded.model.geometries.size(), 5U);
  EXPECT_EQ(std::get<BoxShapeDTO>(decoded.model.geometries[0].shape)
                .dimensions_m,
            Vector3DTO(1.0, 2.0, 3.0));
  EXPECT_DOUBLE_EQ(
      std::get<SphereShapeDTO>(decoded.model.geometries[1].shape).radius_m,
      0.25);
  EXPECT_DOUBLE_EQ(
      std::get<CylinderShapeDTO>(decoded.model.geometries[2].shape).height_m,
      0.7);
  EXPECT_EQ(std::get<MeshShapeDTO>(decoded.model.geometries[3].shape)
                .resource_uri,
            "asset://mesh-a");
  EXPECT_EQ(decoded.model.geometries[3].resolved_mesh_path,
            std::filesystem::path("/resources/mesh-a.stl"));
  EXPECT_FALSE(decoded.model.geometries[3].scene_mesh_content_digest);
  EXPECT_FALSE(decoded.model.geometries[4].resolved_mesh_path);
  EXPECT_EQ(decoded.model.geometries[4].scene_mesh_content_digest,
            input.model.geometries[4].scene_mesh_content_digest);
  EXPECT_EQ(decoded.binding, input.binding);
  EXPECT_EQ(decoded.candidate_config.rng_seed,
            input.candidate_config.rng_seed);
  EXPECT_EQ(decoded.candidate_config.maximum_trajectory_samples,
            input.candidate_config.maximum_trajectory_samples);
  EXPECT_DOUBLE_EQ(decoded.candidate_config.trajectory_sample_period_s,
                   input.candidate_config.trajectory_sample_period_s);
  // The Deployment's collision-check step reaches the worker's planner.
  EXPECT_DOUBLE_EQ(decoded.candidate_config.collision_check_step_rad, 0.0075);
}

TEST(RoboPlanWorkerProtocolTest,
     CandidateAndIkRequestsRoundTripEveryClosedField) {
  const auto input = candidate_request();
  const auto decoded =
      decode_candidate_request(encode_candidate_request(input));

  EXPECT_EQ(decoded.controllable_component_id,
            input.controllable_component_id);
  EXPECT_EQ(decoded.command_target_id, input.command_target_id);
  EXPECT_EQ(decoded.model_instance_id, input.model_instance_id);
  EXPECT_EQ(decoded.model_revision, input.model_revision);
  EXPECT_EQ(decoded.resources, input.resources);
  EXPECT_EQ(decoded.base_frame, input.base_frame);
  EXPECT_EQ(decoded.tcp_frame, input.tcp_frame);
  EXPECT_EQ(decoded.start_state, input.start_state);
  EXPECT_EQ(decoded.motion, input.motion);
  EXPECT_DOUBLE_EQ(
      std::get<CartesianPathDTO>(decoded.motion).max_blend_deviation_rad,
      0.02);
  EXPECT_EQ(decoded.tolerances, input.tolerances);
  EXPECT_EQ(decoded.speed_profile, input.speed_profile);
  EXPECT_EQ(decoded.planning_profile, input.planning_profile);
  EXPECT_EQ(decoded.dynamics_limits, input.dynamics_limits);
  EXPECT_DOUBLE_EQ(decoded.planning_timeout_s, input.planning_timeout_s);
  EXPECT_EQ(decoded.planning_attempt_index, input.planning_attempt_index);
  EXPECT_EQ(decoded.action_span, input.action_span);

  const WorkerIkRequestDTO ik{
      {{0.4, 0.5, 0.6}, {0.1, 0.2, 0.3, 0.9}},
      binding().permitted_frames[0], binding().permitted_frames[1],
      state(), 2.75};
  const auto decoded_ik = decode_ik_request(encode_ik_request(ik));
  EXPECT_EQ(decoded_ik.goal, ik.goal);
  EXPECT_EQ(decoded_ik.base_frame, ik.base_frame);
  EXPECT_EQ(decoded_ik.tcp_frame, ik.tcp_frame);
  EXPECT_EQ(decoded_ik.seed_state, ik.seed_state);
  EXPECT_DOUBLE_EQ(decoded_ik.timeout_s, ik.timeout_s);
}

TEST(RoboPlanWorkerProtocolTest, CandidateRequestCarriesPerPairClearanceMargins) {
  auto input = candidate_request();
  input.collision_pair_margins = {
      RoboPlanPairMarginDTO{"link4", "scene-wall-shape-0", 0.003},
      RoboPlanPairMarginDTO{"link6", "scene-wall-shape-0", 0.0015}};
  const auto bytes = encode_candidate_request(input);
  const auto decoded = decode_candidate_request(bytes);
  EXPECT_EQ(decoded.collision_pair_margins, input.collision_pair_margins);
  EXPECT_EQ(encode_candidate_request(decoded), bytes);
  EXPECT_TRUE(decode_candidate_request(encode_candidate_request(candidate_request()))
                  .collision_pair_margins.empty());

  for (const auto &invalid :
       {RoboPlanPairMarginDTO{"link6", "link4", 0.003},
        RoboPlanPairMarginDTO{"link4", "link4", 0.003},
        RoboPlanPairMarginDTO{"", "link4", 0.003},
        RoboPlanPairMarginDTO{"link4", "link6", -0.001},
        RoboPlanPairMarginDTO{"link4", "link6",
                              std::numeric_limits<double>::quiet_NaN()}}) {
    auto malformed = candidate_request();
    malformed.collision_pair_margins = {invalid};
    EXPECT_THROW(static_cast<void>(decode_candidate_request(
                     encode_candidate_request(malformed))),
                 std::invalid_argument);
  }
}

TEST(RoboPlanWorkerProtocolTest,
     MovingReplacementRoundTripsWithoutChangingOrdinaryPayloadPrefix) {
  auto input = candidate_request();
  const auto ordinary_bytes = encode_candidate_request(input);
  EXPECT_FALSE(decode_candidate_request(ordinary_bytes).moving_replacement);
  auto suffix_seed = state();
  suffix_seed.positions = {AngularPositionDTO{0.8}, LinearPositionDTO{0.03}};
  suffix_seed.velocities.reset();
  input.moving_replacement = MovingReplacementInputDTO{
      ExecutionGoalKey{ControllerGoalId("source-goal-a"),
                       ControllerGoalGeneration(17U)},
      ExecutionBindingEpoch(23U),
      2.75,
      TrajectoryDTO{binding().resources,
                    binding().compiled_joint_names,
                    {{0.0, {0.25, 0.012}, {0.5, 0.02}, {0.2, -0.03}},
                     {0.375, {0.45, 0.02}, {0.4, 0.015}, {-0.1, 0.02}}}},
      3.125,
      suffix_seed,
      ResolvedCellFrameDTO{CellId("cell-a"), "source-base"},
      ResolvedComponentModelFrameDTO{
          binding().controllable_component_id, binding().model_instance_id,
          FrameRoleId("source-tcp"), "source-tcp", binding().model_revision},
      SpeedProfileDTO{0.9, 0.8, 0.25},
      {ResolvedResourceDynamicsLimitDTO{binding().resources[0],
                                        binding().compiled_joint_names[0],
                                        JointDynamicsLimitDTO{2.0, 3.0, 4.0}}},
      1.75};

  const auto moving_bytes = encode_candidate_request(input);
  EXPECT_EQ(moving_bytes.substr(0U, ordinary_bytes.size()), ordinary_bytes);
  const auto decoded = decode_candidate_request(moving_bytes);
  ASSERT_TRUE(decoded.moving_replacement);
  const auto &actual = *decoded.moving_replacement;
  const auto &expected = *input.moving_replacement;
  EXPECT_EQ(actual.source_goal, expected.source_goal);
  EXPECT_EQ(actual.source_binding_epoch, expected.source_binding_epoch);
  EXPECT_DOUBLE_EQ(actual.source_cursor_s, expected.source_cursor_s);
  EXPECT_EQ(encode_trajectory_result(
                Result<TrajectoryDTO>::Ok(actual.source_prefix_trajectory)),
            encode_trajectory_result(
                Result<TrajectoryDTO>::Ok(expected.source_prefix_trajectory)));
  EXPECT_DOUBLE_EQ(actual.divergence_cursor_s, expected.divergence_cursor_s);
  EXPECT_EQ(actual.suffix_start_state, expected.suffix_start_state);
  EXPECT_EQ(decoded.start_state, input.start_state);
  EXPECT_NE(actual.suffix_start_state, decoded.start_state);
  EXPECT_EQ(actual.source_base_frame, expected.source_base_frame);
  EXPECT_EQ(actual.source_tcp_frame, expected.source_tcp_frame);
  EXPECT_EQ(actual.source_speed_profile, expected.source_speed_profile);
  EXPECT_EQ(actual.source_per_resource_dynamics_limits,
            expected.source_per_resource_dynamics_limits);
  EXPECT_DOUBLE_EQ(actual.remaining_preparation_budget_s,
                   expected.remaining_preparation_budget_s);
  EXPECT_FALSE(actual.remaining_planning_budget_s);
  EXPECT_EQ(encode_candidate_request(decoded), moving_bytes);
  input.moving_replacement->remaining_planning_budget_s = 0.125;
  const auto capped_bytes = encode_candidate_request(input);
  EXPECT_EQ(capped_bytes.substr(0U, moving_bytes.size()), moving_bytes);
  std::optional<std::int64_t> deadline = 99;
  const auto capped = decode_candidate_request(capped_bytes, &deadline);
  EXPECT_FALSE(deadline);
  ASSERT_TRUE(capped.moving_replacement->remaining_planning_budget_s);
  EXPECT_DOUBLE_EQ(*capped.moving_replacement->remaining_planning_budget_s, 0.125);
  EXPECT_EQ(encode_candidate_request(capped), capped_bytes);
  const auto timed_bytes = encode_candidate_request(input, 9'876'543'210);
  const auto timed = decode_candidate_request(timed_bytes, &deadline);
  ASSERT_TRUE(deadline);
  EXPECT_EQ(*deadline, 9'876'543'210);
  EXPECT_DOUBLE_EQ(*timed.moving_replacement->remaining_planning_budget_s, 0.125);
  EXPECT_EQ(encode_candidate_request(timed, deadline), timed_bytes);
  auto truncated = timed_bytes;
  truncated.pop_back();
  EXPECT_THROW(static_cast<void>(decode_candidate_request(truncated)), std::invalid_argument);
  auto absent_extension = capped_bytes;
  absent_extension[moving_bytes.size()] = '\0';
  EXPECT_THROW(static_cast<void>(decode_candidate_request(absent_extension)), std::invalid_argument);
  EXPECT_THROW(static_cast<void>(encode_candidate_request(input, -1)), std::invalid_argument);
  input.moving_replacement->remaining_planning_budget_s.reset();
  EXPECT_EQ(encode_candidate_request(input), moving_bytes);
  EXPECT_THROW(static_cast<void>(encode_candidate_request(input, 1)), std::invalid_argument);
  input.moving_replacement.reset();
  EXPECT_EQ(encode_candidate_request(input), ordinary_bytes);

  auto malformed = moving_bytes;
  malformed[ordinary_bytes.size()] = static_cast<char>(2);
  EXPECT_THROW(static_cast<void>(decode_candidate_request(malformed)),
               std::invalid_argument);
  malformed = moving_bytes;
  malformed.pop_back();
  EXPECT_THROW(static_cast<void>(decode_candidate_request(malformed)),
               std::invalid_argument);
  malformed = moving_bytes;
  malformed.push_back('\0');
  EXPECT_THROW(static_cast<void>(decode_candidate_request(malformed)),
               std::invalid_argument);
}

TEST(RoboPlanWorkerProtocolTest, SpeedCheckFrameSetsRoundTrip) {
  auto input = candidate_request();
  const ResolvedFrameDTO flange = ResolvedComponentModelFrameDTO{
      binding().controllable_component_id, binding().model_instance_id,
      FrameRoleId("flange"), "compiled-flange", binding().model_revision};
  input.speed_check_frames = {input.tcp_frame, flange};
  const auto ordinary_bytes = encode_candidate_request(input);
  const auto decoded = decode_candidate_request(ordinary_bytes);
  EXPECT_EQ(decoded.speed_check_frames, input.speed_check_frames);
  EXPECT_FALSE(decoded.moving_replacement);
  EXPECT_EQ(encode_candidate_request(decoded), ordinary_bytes);

  auto suffix_seed = state();
  suffix_seed.velocities.reset();
  input.moving_replacement = MovingReplacementInputDTO{
      ExecutionGoalKey{ControllerGoalId("source-goal-a"),
                       ControllerGoalGeneration(3U)},
      ExecutionBindingEpoch(4U),
      0.5,
      TrajectoryDTO{binding().resources,
                    binding().compiled_joint_names,
                    {{0.0, {0.25, 0.012}, {0.5, 0.02}, {0.2, -0.03}},
                     {0.375, {0.45, 0.02}, {0.4, 0.015}, {-0.1, 0.02}}}},
      0.875,
      suffix_seed,
      input.base_frame,
      input.tcp_frame,
      SpeedProfileDTO{0.9, 0.8, 0.25},
      {},
      1.75};
  // The retained prefix keeps its own (here one-point) set.
  input.moving_replacement->source_speed_check_frames = {input.tcp_frame};
  const auto moving_bytes = encode_candidate_request(input);
  EXPECT_EQ(moving_bytes.substr(0U, ordinary_bytes.size()), ordinary_bytes);
  const auto moving = decode_candidate_request(moving_bytes);
  ASSERT_TRUE(moving.moving_replacement);
  EXPECT_EQ(moving.speed_check_frames, input.speed_check_frames);
  EXPECT_EQ(moving.moving_replacement->source_speed_check_frames,
            std::vector<ResolvedFrameDTO>{input.tcp_frame});
  EXPECT_EQ(encode_candidate_request(moving), moving_bytes);
}

TEST(RoboPlanWorkerProtocolTest,
     ResultPayloadsPreservePortableSuccessAndClosedErrors) {
  TrajectoryDTO trajectory{
      binding().resources,
      binding().compiled_joint_names,
      {TrajectoryWaypointDTO{0.0, {0.0, 0.1}, {0.0, 0.0}, {0.0, 0.0}},
       TrajectoryWaypointDTO{1.0, {0.2, 0.3}, {0.0, 0.0}, {0.0, 0.0}}},
      TrajectoryRuntimeScaleBoundsDTO{2.0, 8.0, 1.5, 2.5}};
  trajectory.cartesian_waypoint_times_s = {0.0, 0.5, 1.0};
  trajectory.cartesian_fidelity_metrics = CartesianFidelityMetricsDTO{
      0.005, 0.01, 0.001, 0.002, 0.0005, 0.001,
      0.0015, 0.003, 0.02, 0.01, 2U};
  const auto decoded_success = decode_trajectory_result(
      encode_trajectory_result(Result<TrajectoryDTO>::Ok(trajectory)));
  ASSERT_TRUE(decoded_success.ok());
  EXPECT_EQ(decoded_success.value().resources, trajectory.resources);
  EXPECT_EQ(decoded_success.value().joint_names, trajectory.joint_names);
  EXPECT_EQ(decoded_success.value().cartesian_waypoint_times_s,
            trajectory.cartesian_waypoint_times_s);
  EXPECT_EQ(decoded_success.value().cartesian_fidelity_metrics,
            trajectory.cartesian_fidelity_metrics);
  ASSERT_EQ(decoded_success.value().waypoints.size(), 2U);
  ASSERT_TRUE(decoded_success.value().runtime_scale_bounds);
  EXPECT_EQ(decoded_success.value().runtime_scale_bounds->max_rate_per_s, 2.0);
  EXPECT_EQ(decoded_success.value().runtime_scale_bounds->max_stop_path_rate, 1.5);
  EXPECT_EQ(decoded_success.value().runtime_scale_bounds->nominal_time_multiplier, 2.5);
  trajectory.runtime_scale_bounds->nominal_time_multiplier.reset();
  const auto legacy = decode_trajectory_result(
      encode_trajectory_result(Result<TrajectoryDTO>::Ok(trajectory)));
  ASSERT_TRUE(legacy.ok());
  EXPECT_FALSE(legacy.value().runtime_scale_bounds->nominal_time_multiplier);
  EXPECT_EQ(
      decoded_success.value().runtime_scale_bounds->max_rate_change_per_s2,
      8.0);
  EXPECT_EQ(decoded_success.value().waypoints[1].positions_rad,
            trajectory.waypoints[1].positions_rad);

  const ErrorDTO error{"planning_failed", "bounded failure",
                       {{"reason", "test-reason"}, {"stage", "planning"}}};
  const auto decoded_error = decode_trajectory_result(
      encode_trajectory_result(Result<TrajectoryDTO>::Err(error)));
  ASSERT_FALSE(decoded_error.ok());
  EXPECT_EQ(decoded_error.error().code, error.code);
  EXPECT_EQ(decoded_error.error().message, error.message);
  EXPECT_EQ(decoded_error.error().details, error.details);

  const auto decoded_ik = decode_ik_result(
      encode_ik_result(Result<std::vector<double>>::Ok(
          std::vector<double>{0.1, 0.2})));
  ASSERT_TRUE(decoded_ik.ok());
  EXPECT_EQ(decoded_ik.value(), (std::vector<double>{0.1, 0.2}));
  EXPECT_EQ(decode_protocol_error(encode_protocol_error("closed detail")),
            "closed detail");
}

TEST(RoboPlanWorkerProtocolTest,
     TruncatedTrailingAndInvalidTaggedPayloadsFailClosed) {
  auto encoded = encode_candidate_request(candidate_request());
  encoded.resize(encoded.size() / 2U);
  EXPECT_THROW(static_cast<void>(decode_candidate_request(encoded)),
               std::invalid_argument);

  encoded = encode_candidate_request(candidate_request());
  encoded.push_back('\0');
  EXPECT_THROW(static_cast<void>(decode_candidate_request(encoded)),
               std::invalid_argument);

  const std::string invalid_boolean(1U, static_cast<char>(2));
  EXPECT_THROW(static_cast<void>(decode_trajectory_result(invalid_boolean)),
               std::invalid_argument);
}

}  // namespace
}  // namespace manyforge::roboplan_backend::worker_protocol
