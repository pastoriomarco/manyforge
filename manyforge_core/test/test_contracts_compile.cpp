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
#include <manyforge_core/types/execution_status.hpp>
#include <manyforge_core/types/execution_result.hpp>

// Time headers
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>

// Port headers
#include <manyforge_core/ports/iplanner_backend.hpp>
#include <manyforge_core/ports/iplanning_context.hpp>
#include <manyforge_core/ports/iexecution_backend.hpp>

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
  timeouts.planning_timeout_s = 10.0;
  EXPECT_EQ(timeouts.planning_timeout_s, 10.0);

  // Robot state DTO
  manyforge::core::RobotStateDTO robot_state;
  EXPECT_TRUE(robot_state.joint_names.empty());

  // Trajectory DTO
  manyforge::core::TrajectoryDTO traj;
  EXPECT_TRUE(traj.empty());

  // Plan request DTO
  manyforge::core::PlanRequestDTO plan_request;
  EXPECT_TRUE(plan_request.robot_id.empty());

  // Plan result DTO
  manyforge::core::PlanResultDTO plan_result;
  EXPECT_FALSE(plan_result.trajectory.has_value());
  EXPECT_FALSE(plan_result.error.has_value());

  // Execute request DTO
  manyforge::core::ExecuteRequestDTO execute_request;
  EXPECT_TRUE(execute_request.robot_id.empty());

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

  // Scene edit DTOs
  manyforge::core::AddObjectRequestDTO add_object_request;
  EXPECT_TRUE(add_object_request.object_id.empty());

  manyforge::core::RemoveObjectRequestDTO remove_object_request;
  EXPECT_TRUE(remove_object_request.object_id.empty());

  manyforge::core::UpdateObjectPoseRequestDTO update_object_pose_request;
  EXPECT_TRUE(update_object_pose_request.object_id.empty());

  manyforge::core::AttachObjectRequestDTO attach_object_request;
  EXPECT_TRUE(attach_object_request.object_id.empty());

  manyforge::core::DetachObjectRequestDTO detach_object_request;
  EXPECT_TRUE(detach_object_request.object_id.empty());

  manyforge::core::ObjectStateDTO object_state;
  EXPECT_FALSE(object_state.exists);
  EXPECT_FALSE(object_state.attached);

  manyforge::core::SceneEditResultDTO scene_edit_result;
  EXPECT_FALSE(scene_edit_result.success);
  EXPECT_FALSE(scene_edit_result.error.has_value());

  // Execution result DTOs
  manyforge::core::StopResultDTO stop_result;
  EXPECT_FALSE(stop_result.accepted);
  EXPECT_FALSE(stop_result.error.has_value());

  manyforge::core::PauseResultDTO pause_result;
  EXPECT_FALSE(pause_result.accepted);
  EXPECT_FALSE(pause_result.error.has_value());

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

// Test MotionSpecDTO variant with all 5 goal types
TEST(ContractsCompile, MotionSpecVariant) {
  using namespace manyforge::core;

  // Joint goal
  JointGoalDTO joint_goal;
  joint_goal.joint_names.push_back("joint_1");
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

  // Relative move
  RelativeMoveDTO relative_move;
  relative_move.target_reference.type = ReferenceDTO::Type::kFrame;
  relative_move.target_reference.id = "world";
  MotionSpecDTO m4 = relative_move;
  EXPECT_TRUE(std::holds_alternative<RelativeMoveDTO>(m4));

  ReferenceDTO pose_ref;
  pose_ref.type = ReferenceDTO::Type::kPose;
  pose_ref.pose = Pose3DTO{};
  pose_ref.pose->position_m = {0.1, 0.2, 0.3};
  pose_ref.pose->orientation = {0.0, 0.0, 0.0, 1.0};
  relative_move.target_reference = pose_ref;
  relative_move.offset_reference = pose_ref;
  MotionSpecDTO m4_pose = relative_move;
  EXPECT_TRUE(std::holds_alternative<RelativeMoveDTO>(m4_pose));

  // Cartesian path
  CartesianPathDTO cartesian_path;
  cartesian_path.max_translation_step_m = 0.01;
  cartesian_path.max_rotation_step_rad = 0.05;
  MotionSpecDTO m5 = cartesian_path;
  EXPECT_TRUE(std::holds_alternative<CartesianPathDTO>(m5));
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
  EXPECT_STREQ(kErrorObjectAlreadyExists, "object_already_exists");
  EXPECT_STREQ(kErrorExecutionRejected, "execution_rejected");
  EXPECT_STREQ(kErrorExecutionAborted, "execution_aborted");
  EXPECT_STREQ(kErrorStopFailed, "stop_failed");
  EXPECT_STREQ(kErrorInternalError, "internal_error");
  EXPECT_STREQ(kErrorInvariantViolation, "invariant_violation");
  EXPECT_STREQ(to_string(ErrorCode::kInvalidRequest), kErrorInvalidRequest);
  EXPECT_STREQ(to_string(ErrorCode::kPlanningFailed), kErrorPlanningFailed);
}

// Test CacheMode enum values
TEST(ContractsCompile, CacheModeEnum) {
  using namespace manyforge::core;

  CacheMode mode1 = CacheMode::kDefault;
  CacheMode mode2 = CacheMode::kDisable;
  EXPECT_NE(static_cast<int>(mode1), static_cast<int>(mode2));
}

// Test all 9 ExecutionState enum values and to_string
TEST(ContractsCompile, AllExecutionStates) {
  using namespace manyforge::core;

  EXPECT_STREQ(to_string(ExecutionState::kIdle), "Idle");
  EXPECT_STREQ(to_string(ExecutionState::kPlanning), "Planning");
  EXPECT_STREQ(to_string(ExecutionState::kValidating), "Validating");
  EXPECT_STREQ(to_string(ExecutionState::kExecuting), "Executing");
  EXPECT_STREQ(to_string(ExecutionState::kPaused), "Paused");
  EXPECT_STREQ(to_string(ExecutionState::kStopping), "Stopping");
  EXPECT_STREQ(to_string(ExecutionState::kStopped), "Stopped");
  EXPECT_STREQ(to_string(ExecutionState::kCompleted), "Completed");
  EXPECT_STREQ(to_string(ExecutionState::kFailed), "Failed");
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

  ExecuteRequestDTO req;
  req.robot_id = "robot1";
  req.group_id = "arm";
  req.move_id = "move_001";
  req.base_frame = "world";
  req.tcp_frame = "tool0";
  req.tolerances.joint_position_tol_rad = 0.01;
  req.speed_profile.velocity_scale = 0.5;
  req.speed_profile.acceleration_scale = 0.5;
  req.timeouts.planning_timeout_s = 10.0;
  req.timeouts.execution_timeout_s = 30.0;
  req.cache_mode = CacheMode::kDefault;
  req.motion = JointGoalDTO{{"j1", "j2"}, {0.0, 1.57}};

  EXPECT_EQ(req.robot_id, "robot1");
  EXPECT_EQ(req.cache_mode, CacheMode::kDefault);
  EXPECT_TRUE(std::holds_alternative<JointGoalDTO>(req.motion));
}

// Test that port interfaces can be declared (not instantiated - they're abstract)
TEST(ContractsCompile, PortInterfaces) {
  // Just verify the types exist and are accessible
  using PlannerBackendType = manyforge::core::IPlannerBackend;
  using PlanningContextType = manyforge::core::IPlanningContext;
  using ExecutionBackendType = manyforge::core::IExecutionBackend;
  
  // Verify they're pointers (abstract classes)
  PlannerBackendType* planner = nullptr;
  PlanningContextType* context = nullptr;
  ExecutionBackendType* execution = nullptr;
  
  EXPECT_EQ(planner, nullptr);
  EXPECT_EQ(context, nullptr);
  EXPECT_EQ(execution, nullptr);
}
