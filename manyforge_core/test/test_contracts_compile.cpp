// Compile-time test: verify all contract headers compile together
// and are self-contained (each header includes its own dependencies).
#include <gtest/gtest.h>

// Type headers
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/geometry.hpp>
#include <manyforge_core/types/tolerances.hpp>
#include <manyforge_core/types/speed_profile.hpp>
#include <manyforge_core/types/timeouts.hpp>
#include <manyforge_core/types/robot_state.hpp>
#include <manyforge_core/types/motion_spec.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>
#include <manyforge_core/types/plan_request.hpp>
#include <manyforge_core/types/execute_request.hpp>
#include <manyforge_core/types/validation.hpp>
#include <manyforge_core/types/scene_edit.hpp>
#include <manyforge_core/types/logical_scene_snapshot.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <manyforge_core/types/execution_status.hpp>
#include <manyforge_core/types/execution_result.hpp>

// Time headers
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>

// Port headers
#include <manyforge_core/ports/iplanner_backend.hpp>
#include <manyforge_core/ports/ireference_pose_resolver.hpp>
#include <manyforge_core/ports/irobot_kinematics.hpp>
#include <manyforge_core/ports/itrajectory_validator.hpp>
#include <manyforge_core/ports/iexecution_backend.hpp>
#include <manyforge_core/ports/iexecution_state_sink.hpp>
#include <manyforge_core/testing/addressed_request_fixtures.hpp>

#include <type_traits>

// Test that Result works
TEST(ContractsCompile, ResultOk) {
  auto r = manyforge::core::Result<int>::Ok(42);
  EXPECT_TRUE(r.ok());
  EXPECT_EQ(r.value(), 42);
}

TEST(ContractsCompile, ResultErr) {
  auto r = manyforge::core::Result<int>::Err(manyforge::core::ErrorDTO{"test_code", "test message", {}});
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, "test_code");
}

TEST(ContractsCompile, ResultVoid) {
  auto ok = manyforge::core::Result<void>::Ok();
  EXPECT_TRUE(ok.ok());

  auto err = manyforge::core::Result<void>::Err(manyforge::core::ErrorDTO{"test_code", "test message", {}});
  EXPECT_FALSE(err.ok());
  EXPECT_EQ(err.error().code, "test_code");
}

// Test default construction of all DTOs
TEST(ContractsCompile, DefaultDTOs) {
  // Geometry DTOs - just verify they construct without error
  manyforge::core::Vector3DTO vec;
  vec.x_m = 1.0;
  EXPECT_EQ(vec.x_m, 1.0);

  manyforge::core::QuaternionDTO quat;
  quat.w = 1.0;
  EXPECT_EQ(quat.w, 1.0);

  manyforge::core::Pose3DTO pose;
  pose.position_m.x_m = 1.0;
  EXPECT_EQ(pose.position_m.x_m, 1.0);

  // Tolerances DTO
  manyforge::core::TolerancesDTO tolerances;
  tolerances.joint_position_tol_rad = 0.01;
  EXPECT_EQ(tolerances.joint_position_tol_rad, 0.01);

  // Speed profile DTO
  manyforge::core::SpeedProfileDTO speed_profile;
  speed_profile.velocity_scale = 1.0;
  EXPECT_EQ(speed_profile.velocity_scale, 1.0);

  // Timeouts DTO
  manyforge::core::TimeoutsDTO timeouts;
  EXPECT_EQ(timeouts.planning_timeout_s, 5.0);
  EXPECT_EQ(timeouts.execution_timeout_s, 10.0);
  timeouts.planning_timeout_s = 10.0;
  EXPECT_EQ(timeouts.planning_timeout_s, 10.0);

  // Robot state DTO
  manyforge::core::RobotStateDTO robot_state;
  EXPECT_TRUE(robot_state.joint_names.empty());

  // Trajectory DTO
  manyforge::core::TrajectoryDTO traj;
  EXPECT_TRUE(traj.empty());

  // Authority-bearing requests cannot be default-constructed without an
  // exact resolved binding.
  static_assert(!std::is_default_constructible_v<
                manyforge::core::PlanRequestDTO>);
  static_assert(!std::is_default_constructible_v<
                manyforge::core::ExecuteRequestDTO>);

  // Plan result DTO
  manyforge::core::PlanResultDTO plan_result;
  EXPECT_FALSE(plan_result.trajectory.has_value());
  EXPECT_FALSE(plan_result.error.has_value());

  const auto execute_request =
      manyforge::core::testing::make_test_execute_request();
  EXPECT_EQ(execute_request.binding.controllable_component_id.value(),
            "component:test");

  // Validation DTOs
  manyforge::core::ValidationOptionsDTO validation_options;
  EXPECT_EQ(validation_options.max_tcp_linear_speed_mps, 0.0);

  manyforge::core::ValidationResultDTO validation_result;
  EXPECT_FALSE(validation_result.valid);
  EXPECT_FALSE(validation_result.error.has_value());

  manyforge::core::CollisionCheckRequestDTO collision_check_request;
  EXPECT_TRUE(collision_check_request.robot_state.joint_names.empty());

  manyforge::core::CollisionCheckResultDTO collision_check_result;
  EXPECT_FALSE(collision_check_result.in_collision);
  EXPECT_FALSE(collision_check_result.contact_body_a.has_value());
  EXPECT_FALSE(collision_check_result.contact_body_b.has_value());

  // FK DTOs
  manyforge::core::FkRequestDTO fk_request;
  EXPECT_TRUE(fk_request.target_frame.empty());

  // Joint limits DTO
  manyforge::core::JointLimitsDTO joint_limits;
  EXPECT_TRUE(joint_limits.joint_names.empty());

  // Composite scene/projection identity DTOs remain explicitly invalid until
  // an owner supplies their complete strong identities.
  manyforge::core::SceneRevision scene_revision;
  EXPECT_FALSE(scene_revision.valid());
  manyforge::core::ProjectionRevisionDTO projection_revision;
  EXPECT_FALSE(projection_revision.valid());
  manyforge::core::ProjectionLeaseBindingDTO projection_binding;
  EXPECT_FALSE(projection_binding.valid());

  // Execution result DTOs
  manyforge::core::StopResultDTO stop_result;
  EXPECT_FALSE(stop_result.accepted);
  EXPECT_FALSE(stop_result.error.has_value());

  manyforge::core::ResetResultDTO reset_result;
  EXPECT_FALSE(reset_result.accepted);
  EXPECT_FALSE(reset_result.error.has_value());
}

// Test enum to_string functions
TEST(ContractsCompile, EnumToString) {
  EXPECT_STREQ(manyforge::core::to_string(manyforge::core::ExecutionState::kValidating), "Validating");
  EXPECT_STREQ(manyforge::core::to_string(manyforge::core::ExecutionState::kExecuting), "Executing");
}

// Test geometry types
TEST(ContractsCompile, GeometryTypes) {
  manyforge::core::Vector3DTO vec;
  vec.x_m = 1.0;
  vec.y_m = 2.0;
  vec.z_m = 3.0;

  manyforge::core::QuaternionDTO quat;
  quat.x = 0.0;
  quat.y = 0.0;
  quat.z = 0.0;
  quat.w = 1.0;

  manyforge::core::Pose3DTO pose;
  pose.position_m = vec;
  pose.orientation = quat;
}

// Test MotionSpecDTO variants and orthogonal target-policy fields.
TEST(ContractsCompile, MotionSpecVariant) {
  using namespace manyforge::core;

  // Joint goal
  JointGoalDTO joint_goal;
  joint_goal.joint_names.emplace_back("joint_1");
  joint_goal.positions_rad.push_back(1.0);
  MotionSpecDTO m1 = joint_goal;
  EXPECT_TRUE(std::holds_alternative<JointGoalDTO>(m1));

  // Pose goal
  PoseGoalDTO pose_goal{};
  MotionSpecDTO m2 = pose_goal;
  EXPECT_TRUE(std::holds_alternative<PoseGoalDTO>(m2));

  // Named target
  NamedTargetDTO named_target;
  named_target.name = "home";
  MotionSpecDTO m3 = named_target;
  EXPECT_TRUE(std::holds_alternative<NamedTargetDTO>(m3));

  ReferenceDTO pose_ref;
  pose_ref.type = ReferenceDTO::Type::kPose;
  pose_ref.pose = Pose3DTO{};
  pose_ref.pose->position_m = {0.1, 0.2, 0.3};
  pose_ref.pose->orientation = {0.0, 0.0, 0.0, 1.0};
  auto relative_pose_request =
      manyforge::core::testing::make_test_execute_request();
  relative_pose_request.motion = pose_goal;
  relative_pose_request.target_mode = MotionTargetModeDTO::kRelative;
  relative_pose_request.target_reference = pose_ref;
  relative_pose_request.offset_reference = pose_ref;
  relative_pose_request.planner.search = PlannerSearchDTO::kRrt;
  EXPECT_TRUE(std::holds_alternative<PoseGoalDTO>(
      relative_pose_request.motion));
  EXPECT_EQ(relative_pose_request.target_mode,
            MotionTargetModeDTO::kRelative);
  EXPECT_EQ(relative_pose_request.planner.search, PlannerSearchDTO::kRrt);

  // Cartesian path
  CartesianPathDTO cartesian_path;
  cartesian_path.max_translation_step_m = 0.01;
  cartesian_path.max_rotation_step_rad = 0.05;
  MotionSpecDTO m4 = cartesian_path;
  EXPECT_TRUE(std::holds_alternative<CartesianPathDTO>(m4));
}

// Test CollisionShapeDTO variant with all 4 shapes
TEST(ContractsCompile, CollisionShapeVariant) {
  using namespace manyforge::core;

  // Box
  CollisionShapeDTO s1 = BoxShapeDTO{{1.0, 1.0, 1.0}};
  EXPECT_TRUE(std::holds_alternative<BoxShapeDTO>(s1));

  // Sphere
  CollisionShapeDTO s2 = SphereShapeDTO{0.5};
  EXPECT_TRUE(std::holds_alternative<SphereShapeDTO>(s2));

  // Cylinder
  CollisionShapeDTO s3 = CylinderShapeDTO{0.1, 0.5};
  EXPECT_TRUE(std::holds_alternative<CylinderShapeDTO>(s3));

  // Mesh
  MeshShapeDTO mesh;
  mesh.resource_uri = "package://mesh.stl";
  mesh.scale = {1.0, 1.0, 1.0};
  CollisionShapeDTO s4 = mesh;
  EXPECT_TRUE(std::holds_alternative<MeshShapeDTO>(s4));
}

// Test error code string constants exist and are non-empty
TEST(ContractsCompile, ErrorCodeConstants) {
  using namespace manyforge::core;

  // Verify a representative set of error constants
  EXPECT_STREQ(kErrorInvalidRequest, "invalid_request");
  EXPECT_STREQ(kErrorPlanningFailed, "planning_failed");
  EXPECT_STREQ(kErrorPlanningTimeout, "planning_timeout");
  EXPECT_STREQ(kErrorStartStateMismatch, "start_state_mismatch");
  EXPECT_STREQ(kErrorValidationCollision, "validation_collision");
  EXPECT_STREQ(kErrorInvalidContactPolicy, "invalid_contact_policy");
  EXPECT_STREQ(kErrorContactPolicyViolation, "contact_policy_violation");
  EXPECT_STREQ(kErrorObjectAlreadyExists, "object_already_exists");
  EXPECT_STREQ(kErrorExecutionRejected, "execution_rejected");
  EXPECT_STREQ(kErrorExecutionAborted, "execution_aborted");
  EXPECT_STREQ(kErrorStopFailed, "stop_failed");
  EXPECT_STREQ(kErrorInternalError, "internal_error");
  EXPECT_STREQ(kErrorInvariantViolation, "invariant_violation");
  EXPECT_STREQ(to_string(ErrorCode::kInvalidRequest), kErrorInvalidRequest);
  EXPECT_STREQ(to_string(ErrorCode::kPlanningFailed), kErrorPlanningFailed);
  EXPECT_STREQ(to_string(ErrorCode::kInvalidContactPolicy),
               kErrorInvalidContactPolicy);
  EXPECT_STREQ(to_string(ErrorCode::kContactPolicyViolation),
               kErrorContactPolicyViolation);
}

// Test CacheMode enum values
TEST(ContractsCompile, CacheModeEnum) {
  using namespace manyforge::core;

  CacheMode mode1 = CacheMode::kDefault;
  CacheMode mode2 = CacheMode::kDisable;
  EXPECT_NE(static_cast<int>(mode1), static_cast<int>(mode2));
}

// Test all ExecutionState enum values and to_string
TEST(ContractsCompile, AllExecutionStates) {
  using namespace manyforge::core;

  EXPECT_STREQ(to_string(ExecutionState::kIdle), "Idle");
  EXPECT_STREQ(to_string(ExecutionState::kPlanning), "Planning");
  EXPECT_STREQ(to_string(ExecutionState::kValidating), "Validating");
  EXPECT_STREQ(to_string(ExecutionState::kExecuting), "Executing");
  EXPECT_STREQ(to_string(ExecutionState::kInterrupted), "Interrupted");
  EXPECT_STREQ(to_string(ExecutionState::kStopping), "Stopping");
  EXPECT_STREQ(to_string(ExecutionState::kStopped), "Stopped");
  EXPECT_STREQ(to_string(ExecutionState::kCompleted), "Completed");
  EXPECT_STREQ(to_string(ExecutionState::kFailed), "Failed");
  EXPECT_STREQ(to_string(ExecutionState::kInterventionRequired),
               "InterventionRequired");
}

// Test PlanResultDTO XOR invariant: exactly one of trajectory or error
TEST(ContractsCompile, PlanResultXOR) {
  using namespace manyforge::core;

  // Success case: trajectory present, error absent
  PlanResultDTO success;
  success.trajectory = TrajectoryDTO{};
  EXPECT_TRUE(success.trajectory.has_value());
  EXPECT_FALSE(success.error.has_value());

  // Failure case: error present, trajectory absent
  PlanResultDTO failure;
  failure.error = ErrorDTO{kErrorPlanningFailed, "No solution found", {}};
  EXPECT_FALSE(failure.trajectory.has_value());
  EXPECT_TRUE(failure.error.has_value());
  EXPECT_STREQ(failure.error->code.c_str(), kErrorPlanningFailed);
}

// Test ErrorDTO details map
TEST(ContractsCompile, ErrorDTODetails) {
  using namespace manyforge::core;

  ErrorDTO err;
  err.code = kErrorMissingReference;
  err.message = "Named target not found";
  err.details["reference_type"] = "named_target";
  err.details["reference_id"] = "home";

  EXPECT_EQ(err.details.size(), 2u);
  EXPECT_EQ(err.details.at("reference_type"), "named_target");
  EXPECT_EQ(err.details.at("reference_id"), "home");
}

// Test ExecuteRequestDTO composition with all embedded types
TEST(ContractsCompile, ExecuteRequestComposition) {
  using namespace manyforge::core;

  auto req = manyforge::core::testing::make_test_execute_request(
      {"j1", "j2"});
  req.binding.controllable_component_id =
      ControllableComponentId("robot1");
  req.move_id = "move_001";
  std::get<ResolvedComponentModelFrameDTO>(req.binding.base_frame)
      .compiled_frame_name = "world";
  std::get<ResolvedComponentModelFrameDTO>(req.binding.tcp_frame)
      .compiled_frame_name = "tool0";
  req.tolerances.joint_position_tol_rad = 0.01;
  req.speed_profile.velocity_scale = 0.5;
  req.speed_profile.acceleration_scale = 0.5;
  req.timeouts.planning_timeout_s = 10.0;
  req.timeouts.execution_timeout_s = 30.0;
  req.cache_mode = CacheMode::kDefault;
  req.motion = ResolvedJointGoalDTO{{ResourceId("r1"), ResourceId("r2")},
                                    {"j1", "j2"}, {0.0, 1.57}};

  EXPECT_EQ(req.binding.controllable_component_id.value(), "robot1");
  EXPECT_EQ(req.cache_mode, CacheMode::kDefault);
  EXPECT_TRUE(std::holds_alternative<ResolvedJointGoalDTO>(req.motion));
}

// Test that port interfaces can be declared (not instantiated - they're abstract)
TEST(ContractsCompile, PortInterfaces) {
  // Just verify the types exist and are accessible
  using PlannerBackendType = manyforge::core::IPlannerBackend;
  using ReferenceResolverType = manyforge::core::IReferencePoseResolver;
  using RobotKinematicsType = manyforge::core::IRobotKinematics;
  using TrajectoryValidatorType = manyforge::core::ITrajectoryValidator;
  using ExecutionBackendType = manyforge::core::IExecutionBackend;
  using ExecutionStateSinkType = manyforge::core::IExecutionStateSink;
  
  // Verify they're pointers (abstract classes)
  PlannerBackendType* planner = nullptr;
  ReferenceResolverType* reference_resolver = nullptr;
  RobotKinematicsType* kinematics = nullptr;
  TrajectoryValidatorType* trajectory_validator = nullptr;
  ExecutionBackendType* execution = nullptr;
  ExecutionStateSinkType* execution_state_sink = nullptr;
  
  EXPECT_EQ(planner, nullptr);
  EXPECT_EQ(reference_resolver, nullptr);
  EXPECT_EQ(kinematics, nullptr);
  EXPECT_EQ(trajectory_validator, nullptr);
  EXPECT_EQ(execution, nullptr);
  EXPECT_EQ(execution_state_sink, nullptr);
}
