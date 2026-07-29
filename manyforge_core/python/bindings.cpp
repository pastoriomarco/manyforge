#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/operators.h>
#include <Python.h>

#include "binding_registrations.hpp"

// Only DTO/enum types are bound — port interfaces and kernel are C++-only.
// Python interacts with the kernel via NativeKernelClient (manyforge_roboplan_py).
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/execution_status.hpp>
#include <manyforge_core/types/execution_result.hpp>
#include <manyforge_core/types/execute_request.hpp>
#include <manyforge_core/types/geometry.hpp>
#include <manyforge_core/types/robot_state.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>
#include <manyforge_core/types/motion_spec.hpp>
#include <manyforge_core/types/plan_request.hpp>
#include <manyforge_core/types/validation.hpp>
#include <manyforge_core/types/scene_edit.hpp>
#include <manyforge_core/types/collision_event.hpp>
#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/collision_world_snapshot.hpp>
#include <manyforge_core/collision/collision_world_snapshot_builder.hpp>

namespace py = pybind11;

// ============================================================================
// Module definition
// ============================================================================

PYBIND11_MODULE(manyforge_core_py, m) {
    m.doc() = "Python bindings for manyforge_core DTOs and enums";

    manyforge::core::python::register_strong_identity_bindings(m);
    manyforge::core::python::register_catalog_record_bindings(m);
    manyforge::core::python::register_cell_graph_bindings(m);
    manyforge::core::python::register_deployment_candidate_bindings(m);

    // ========================================================================
    // Enums
    // ========================================================================

    py::enum_<manyforge::core::ExecutionState>(m, "ExecutionState")
        .value("kIdle", manyforge::core::ExecutionState::kIdle)
        .value("kPlanning", manyforge::core::ExecutionState::kPlanning)
        .value("kValidating", manyforge::core::ExecutionState::kValidating)
        .value("kExecuting", manyforge::core::ExecutionState::kExecuting)
        .value("kInterrupted", manyforge::core::ExecutionState::kInterrupted)
        .value("kPaused", manyforge::core::ExecutionState::kPaused)
        .value("kStopping", manyforge::core::ExecutionState::kStopping)
        .value("kStopped", manyforge::core::ExecutionState::kStopped)
        .value("kCompleted", manyforge::core::ExecutionState::kCompleted)
        .value("kFailed", manyforge::core::ExecutionState::kFailed)
        .value("kInterventionRequired",
               manyforge::core::ExecutionState::kInterventionRequired)
        .export_values();

    py::enum_<manyforge::core::CacheMode>(m, "CacheMode")
        .value("kDefault", manyforge::core::CacheMode::kDefault)
        .value("kDisable", manyforge::core::CacheMode::kDisable)
        .export_values();

    py::enum_<manyforge::core::MotionTargetModeDTO>(m, "MotionTargetMode")
        .value("kAbsolute", manyforge::core::MotionTargetModeDTO::kAbsolute)
        .value("kRelative", manyforge::core::MotionTargetModeDTO::kRelative);

    py::enum_<manyforge::core::PlanningProfileDTO>(m, "PlanningProfile")
        .value("kFirstFeasible",
               manyforge::core::PlanningProfileDTO::kFirstFeasible)
        .value("kBalanced", manyforge::core::PlanningProfileDTO::kBalanced)
        .value("kQualityBounded",
               manyforge::core::PlanningProfileDTO::kQualityBounded);

    py::enum_<manyforge::core::ErrorCode>(m, "ErrorCode")
        .value("kSuccess", manyforge::core::ErrorCode::kSuccess)
        .value("kInvalidRequest", manyforge::core::ErrorCode::kInvalidRequest)
        .value("kMissingReference", manyforge::core::ErrorCode::kMissingReference)
        .value("kAmbiguousReference", manyforge::core::ErrorCode::kAmbiguousReference)
        .value("kUnsupportedCapability", manyforge::core::ErrorCode::kUnsupportedCapability)
        .value("kPlanningFailed", manyforge::core::ErrorCode::kPlanningFailed)
        .value("kPlanningTimeout", manyforge::core::ErrorCode::kPlanningTimeout)
        .value("kIkFailed", manyforge::core::ErrorCode::kIkFailed)
        .value("kCartesianIncomplete", manyforge::core::ErrorCode::kCartesianIncomplete)
        .value("kStartStateMismatch", manyforge::core::ErrorCode::kStartStateMismatch)
        .value("kValidationCollision", manyforge::core::ErrorCode::kValidationCollision)
        .value("kInvalidContactPolicy",
               manyforge::core::ErrorCode::kInvalidContactPolicy)
        .value("kContactPolicyViolation",
               manyforge::core::ErrorCode::kContactPolicyViolation)
        .value("kValidationLimitsViolation", manyforge::core::ErrorCode::kValidationLimitsViolation)
        .value("kObjectAlreadyExists", manyforge::core::ErrorCode::kObjectAlreadyExists)
        .value("kObjectNotFound", manyforge::core::ErrorCode::kObjectNotFound)
        .value("kExecutionRejected", manyforge::core::ErrorCode::kExecutionRejected)
        .value("kExecutionAborted", manyforge::core::ErrorCode::kExecutionAborted)
        .value("kExecutionTimeout", manyforge::core::ErrorCode::kExecutionTimeout)
        .value("kStopFailed", manyforge::core::ErrorCode::kStopFailed)
        .value("kPauseFailed", manyforge::core::ErrorCode::kPauseFailed)
        .value("kResumeFailed", manyforge::core::ErrorCode::kResumeFailed)
        .value("kResetFailed", manyforge::core::ErrorCode::kResetFailed)
        .value("kInternalError", manyforge::core::ErrorCode::kInternalError)
        .value("kInvariantViolation", manyforge::core::ErrorCode::kInvariantViolation)
        .value("kInvalidStateTransition", manyforge::core::ErrorCode::kInvalidStateTransition)
        .value("kCacheMiss", manyforge::core::ErrorCode::kCacheMiss)
        .value("kCollisionDetected", manyforge::core::ErrorCode::kCollisionDetected)
        .export_values();

    // ========================================================================
    // Geometry DTOs
    // ========================================================================

    py::class_<manyforge::core::Vector3DTO>(m, "Vector3DTO")
        .def(py::init<>())
        .def(py::init<double, double, double>(),
             py::arg("x_m"), py::arg("y_m"), py::arg("z_m"))
        .def_readwrite("x_m", &manyforge::core::Vector3DTO::x_m)
        .def_readwrite("y_m", &manyforge::core::Vector3DTO::y_m)
        .def_readwrite("z_m", &manyforge::core::Vector3DTO::z_m)
        .def("__repr__", [](const manyforge::core::Vector3DTO& v) {
            return py::str("Vector3DTO(x_m={}, y_m={}, z_m={})")
                .format(v.x_m, v.y_m, v.z_m);
        });

    py::class_<manyforge::core::QuaternionDTO>(m, "QuaternionDTO")
        .def(py::init<>())
        .def(py::init<double, double, double, double>(),
             py::arg("x"), py::arg("y"), py::arg("z"), py::arg("w"))
        .def_readwrite("x", &manyforge::core::QuaternionDTO::x)
        .def_readwrite("y", &manyforge::core::QuaternionDTO::y)
        .def_readwrite("z", &manyforge::core::QuaternionDTO::z)
        .def_readwrite("w", &manyforge::core::QuaternionDTO::w)
        .def("__repr__", [](const manyforge::core::QuaternionDTO& q) {
            return py::str("QuaternionDTO(x={}, y={}, z={}, w={})")
                .format(q.x, q.y, q.z, q.w);
        });

    py::class_<manyforge::core::Pose3DTO>(m, "Pose3DTO")
        .def(py::init<>())
        .def(py::init<manyforge::core::Vector3DTO, manyforge::core::QuaternionDTO>(),
             py::arg("position_m"), py::arg("orientation"))
        .def_readwrite("position_m", &manyforge::core::Pose3DTO::position_m)
        .def_readwrite("orientation", &manyforge::core::Pose3DTO::orientation)
        .def("__repr__", [](const manyforge::core::Pose3DTO& p) {
            return py::str("Pose3DTO(position_m={}, orientation={})")
                .format(py::cast(p.position_m), py::cast(p.orientation));
        });

    // ========================================================================
    // Basic DTOs
    // ========================================================================

    py::class_<manyforge::core::TolerancesDTO>(m, "TolerancesDTO")
        .def(py::init<>())
        .def(py::init<double, double, double>(),
             py::arg("joint_position_tol_rad"),
             py::arg("translation_tol_m"),
             py::arg("rotation_tol_rad"))
        .def_readwrite("joint_position_tol_rad", &manyforge::core::TolerancesDTO::joint_position_tol_rad)
        .def_readwrite("translation_tol_m", &manyforge::core::TolerancesDTO::translation_tol_m)
        .def_readwrite("rotation_tol_rad", &manyforge::core::TolerancesDTO::rotation_tol_rad)
        .def("__repr__", [](const manyforge::core::TolerancesDTO& t) {
            return py::str("TolerancesDTO(joint_position_tol_rad={}, translation_tol_m={}, rotation_tol_rad={})")
                .format(t.joint_position_tol_rad, t.translation_tol_m, t.rotation_tol_rad);
        });

    py::class_<manyforge::core::SpeedProfileDTO>(m, "SpeedProfileDTO")
        .def(py::init<>())
        .def(py::init<double, double, std::optional<double>>(),
             py::arg("velocity_scale"),
             py::arg("acceleration_scale"),
             py::arg("max_tcp_linear_speed_mps") = std::nullopt)
        .def_readwrite("velocity_scale", &manyforge::core::SpeedProfileDTO::velocity_scale)
        .def_readwrite("acceleration_scale", &manyforge::core::SpeedProfileDTO::acceleration_scale)
        .def_readwrite("max_tcp_linear_speed_mps", &manyforge::core::SpeedProfileDTO::max_tcp_linear_speed_mps)
        .def("__repr__", [](const manyforge::core::SpeedProfileDTO& s) {
            return py::str("SpeedProfileDTO(velocity_scale={}, acceleration_scale={}, max_tcp_linear_speed_mps={})")
                .format(s.velocity_scale, s.acceleration_scale,
                       s.max_tcp_linear_speed_mps.has_value() ? py::cast(*s.max_tcp_linear_speed_mps) : py::none());
        });

    py::class_<manyforge::core::JointDynamicsLimitDTO>(m, "JointDynamicsLimitDTO")
        .def(py::init<>())
        .def(py::init<std::optional<double>, std::optional<double>, std::optional<double>>(),
             py::arg("max_velocity_rad_s") = std::nullopt,
             py::arg("max_acceleration_rad_s2") = std::nullopt,
             py::arg("max_jerk_rad_s3") = std::nullopt)
        .def_readwrite("max_velocity_rad_s",
                       &manyforge::core::JointDynamicsLimitDTO::max_velocity_rad_s)
        .def_readwrite("max_acceleration_rad_s2",
                       &manyforge::core::JointDynamicsLimitDTO::max_acceleration_rad_s2)
        .def_readwrite("max_jerk_rad_s3",
                       &manyforge::core::JointDynamicsLimitDTO::max_jerk_rad_s3)
        .def("__repr__", [](const manyforge::core::JointDynamicsLimitDTO& l) {
            return py::str("JointDynamicsLimitDTO(max_velocity_rad_s={}, "
                           "max_acceleration_rad_s2={}, max_jerk_rad_s3={})")
                .format(l.max_velocity_rad_s.has_value() ? py::cast(*l.max_velocity_rad_s) : py::none(),
                        l.max_acceleration_rad_s2.has_value() ? py::cast(*l.max_acceleration_rad_s2) : py::none(),
                        l.max_jerk_rad_s3.has_value() ? py::cast(*l.max_jerk_rad_s3) : py::none());
        });

    py::class_<manyforge::core::TimeoutsDTO>(m, "TimeoutsDTO")
        .def(py::init<>())
        .def(py::init<double, double>(),
             py::arg("planning_timeout_s"),
             py::arg("execution_timeout_s"))
        .def_readwrite("planning_timeout_s", &manyforge::core::TimeoutsDTO::planning_timeout_s)
        .def_readwrite("execution_timeout_s", &manyforge::core::TimeoutsDTO::execution_timeout_s)
        .def("__repr__", [](const manyforge::core::TimeoutsDTO& t) {
            return py::str("TimeoutsDTO(planning_timeout_s={}, execution_timeout_s={})")
                .format(t.planning_timeout_s, t.execution_timeout_s);
        });

    py::class_<manyforge::core::ErrorDTO>(m, "ErrorDTO")
        .def(py::init<>())
        .def(py::init<const std::string&, const std::string&, std::map<std::string, std::string>>(),
             py::arg("code"), py::arg("message"), py::arg("details") = std::map<std::string, std::string>{})
        .def_readwrite("code", &manyforge::core::ErrorDTO::code)
        .def_readwrite("message", &manyforge::core::ErrorDTO::message)
        .def_readwrite("details", &manyforge::core::ErrorDTO::details)
        .def("__repr__", [](const manyforge::core::ErrorDTO& e) {
            return py::str("ErrorDTO(code='{}', message='{}')").format(e.code, e.message);
        });

    // ========================================================================
    // Motion Spec DTOs
    // ========================================================================

    py::class_<manyforge::core::JointGoalDTO>(m, "JointGoalDTO")
        .def(py::init<>())
        .def(py::init<std::vector<std::string>, std::vector<double>>(),
             py::arg("joint_names"), py::arg("positions_rad"))
        .def_readwrite("joint_names", &manyforge::core::JointGoalDTO::joint_names)
        .def_readwrite("positions_rad", &manyforge::core::JointGoalDTO::positions_rad)
        .def("__repr__", [](const manyforge::core::JointGoalDTO& j) {
            return py::str("JointGoalDTO(joint_names={}, positions_rad={})")
                .format(py::cast(j.joint_names), py::cast(j.positions_rad));
        });

    py::class_<manyforge::core::PoseGoalDTO>(m, "PoseGoalDTO")
        .def(py::init<>())
        .def(py::init<manyforge::core::Pose3DTO>(), py::arg("goal"))
        .def_readwrite("goal", &manyforge::core::PoseGoalDTO::goal)
        .def("__repr__", [](const manyforge::core::PoseGoalDTO& p) {
            return py::str("PoseGoalDTO(goal={})").format(py::cast(p.goal));
        });

    py::class_<manyforge::core::NamedTargetDTO>(m, "NamedTargetDTO")
        .def(py::init<>())
        .def(py::init<const std::string&>(), py::arg("name"))
        .def_readwrite("name", &manyforge::core::NamedTargetDTO::name)
        .def("__repr__", [](const manyforge::core::NamedTargetDTO& n) {
            return py::str("NamedTargetDTO(name='{}')").format(n.name);
        });

    py::enum_<manyforge::core::ReferenceDTO::Type>(m, "ReferenceType")
        .value("kFrame", manyforge::core::ReferenceDTO::Type::kFrame)
        .value("kObject", manyforge::core::ReferenceDTO::Type::kObject)
        .value("kRobotLink", manyforge::core::ReferenceDTO::Type::kRobotLink)
        .value("kPose", manyforge::core::ReferenceDTO::Type::kPose)
        .export_values();

    py::class_<manyforge::core::ReferenceDTO>(m, "ReferenceDTO")
        .def(py::init<>())
        .def(py::init<manyforge::core::ReferenceDTO::Type, const std::string&>(),
             py::arg("type"), py::arg("id"))
        .def_readwrite("type", &manyforge::core::ReferenceDTO::type)
        .def_readwrite("id", &manyforge::core::ReferenceDTO::id)
        .def_readwrite("pose", &manyforge::core::ReferenceDTO::pose)
        .def("__repr__", [](const manyforge::core::ReferenceDTO& r) {
            return py::str("ReferenceDTO(type={}, id='{}', pose={})")
                .format(py::cast(r.type), r.id, py::cast(r.pose));
        });

    py::enum_<manyforge::core::CartesianSpeedModeDTO>(m, "CartesianSpeedMode")
        .value("kBounded", manyforge::core::CartesianSpeedModeDTO::kBounded);

    py::enum_<manyforge::core::CartesianFallbackPolicyDTO>(m, "CartesianFallbackPolicy")
        .value("kFailClosed", manyforge::core::CartesianFallbackPolicyDTO::kFailClosed)
        .value("kAllowJointSpaceFallback",
               manyforge::core::CartesianFallbackPolicyDTO::kAllowJointSpaceFallback);

    py::class_<manyforge::core::CartesianPathDTO>(m, "CartesianPathDTO")
        .def(py::init<>())
        .def(py::init([](std::vector<manyforge::core::Pose3DTO> waypoints,
                         double max_translation_step_m,
                         double max_rotation_step_rad,
                         manyforge::core::CartesianSpeedModeDTO speed_mode,
                         double max_linear_speed_mps,
                         double max_angular_speed_rad_s,
                         double max_linear_acceleration_mps2,
                         double max_angular_acceleration_rad_s2,
                         manyforge::core::CartesianFallbackPolicyDTO fallback_policy) {
            manyforge::core::CartesianPathDTO path;
            path.waypoints = std::move(waypoints);
            path.max_translation_step_m = max_translation_step_m;
            path.max_rotation_step_rad = max_rotation_step_rad;
            path.speed_mode = speed_mode;
            path.max_linear_speed_mps = max_linear_speed_mps;
            path.max_angular_speed_rad_s = max_angular_speed_rad_s;
            path.max_linear_acceleration_mps2 = max_linear_acceleration_mps2;
            path.max_angular_acceleration_rad_s2 = max_angular_acceleration_rad_s2;
            path.fallback_policy = fallback_policy;
            return path;
        }),
             py::arg("waypoints"),
             py::arg("max_translation_step_m") = 0.005,
             py::arg("max_rotation_step_rad") = 0.05,
             py::arg("speed_mode") = manyforge::core::CartesianSpeedModeDTO::kBounded,
             py::arg("max_linear_speed_mps") = 0.1,
             py::arg("max_angular_speed_rad_s") = 0.5,
             py::arg("max_linear_acceleration_mps2") = 0.5,
             py::arg("max_angular_acceleration_rad_s2") = 2.5,
             py::arg("fallback_policy") =
                 manyforge::core::CartesianFallbackPolicyDTO::kFailClosed)
        .def_readwrite("waypoints", &manyforge::core::CartesianPathDTO::waypoints)
        .def_readwrite("max_translation_step_m", &manyforge::core::CartesianPathDTO::max_translation_step_m)
        .def_readwrite("max_rotation_step_rad", &manyforge::core::CartesianPathDTO::max_rotation_step_rad)
        .def_readwrite("speed_mode", &manyforge::core::CartesianPathDTO::speed_mode)
        .def_readwrite("max_linear_speed_mps", &manyforge::core::CartesianPathDTO::max_linear_speed_mps)
        .def_readwrite("max_angular_speed_rad_s", &manyforge::core::CartesianPathDTO::max_angular_speed_rad_s)
        .def_readwrite("max_linear_acceleration_mps2", &manyforge::core::CartesianPathDTO::max_linear_acceleration_mps2)
        .def_readwrite("max_angular_acceleration_rad_s2", &manyforge::core::CartesianPathDTO::max_angular_acceleration_rad_s2)
        .def_readwrite("fallback_policy", &manyforge::core::CartesianPathDTO::fallback_policy)
        .def("__repr__", [](const manyforge::core::CartesianPathDTO& c) {
            return py::str("CartesianPathDTO(waypoints={}, max_translation_step_m={}, max_rotation_step_rad={}, speed_mode={})")
                .format(py::cast(c.waypoints), c.max_translation_step_m,
                        c.max_rotation_step_rad, py::cast(c.speed_mode));
        });

    // ========================================================================
    // Robot State DTOs
    // ========================================================================

    py::class_<manyforge::core::RobotStateDTO>(m, "RobotStateDTO")
        .def(py::init<>())
        .def(py::init<std::vector<std::string>, std::vector<double>, std::vector<double>>(),
             py::arg("joint_names"),
             py::arg("positions_rad"),
             py::arg("velocities_rad_s") = std::vector<double>{})
        .def_readwrite("joint_names", &manyforge::core::RobotStateDTO::joint_names)
        .def_readwrite("positions_rad", &manyforge::core::RobotStateDTO::positions_rad)
        .def_readwrite("velocities_rad_s", &manyforge::core::RobotStateDTO::velocities_rad_s)
        .def("__repr__", [](const manyforge::core::RobotStateDTO& r) {
            return py::str("RobotStateDTO(joint_names={}, positions_rad={})")
                .format(py::cast(r.joint_names), py::cast(r.positions_rad));
        });

    // ========================================================================
    // Trajectory DTOs
    // ========================================================================

    py::class_<manyforge::core::TrajectoryWaypointDTO>(m, "TrajectoryWaypointDTO")
        .def(py::init<>())
        .def(py::init<double, std::vector<double>, std::vector<double>, std::vector<double>>(),
             py::arg("time_from_start_s"),
             py::arg("positions_rad"),
             py::arg("velocities_rad_s") = std::vector<double>{},
             py::arg("accelerations_rad_s2") = std::vector<double>{})
        .def_readwrite("time_from_start_s", &manyforge::core::TrajectoryWaypointDTO::time_from_start_s)
        .def_readwrite("positions_rad", &manyforge::core::TrajectoryWaypointDTO::positions_rad)
        .def_readwrite("velocities_rad_s", &manyforge::core::TrajectoryWaypointDTO::velocities_rad_s)
        .def_readwrite("accelerations_rad_s2", &manyforge::core::TrajectoryWaypointDTO::accelerations_rad_s2)
        .def("__repr__", [](const manyforge::core::TrajectoryWaypointDTO& w) {
            return py::str("TrajectoryWaypointDTO(time_from_start_s={}, positions_rad={})")
                .format(w.time_from_start_s, py::cast(w.positions_rad));
        });

    py::class_<manyforge::core::TrajectoryDTO>(m, "TrajectoryDTO")
        .def(py::init<>())
        .def(py::init<std::vector<std::string>, std::vector<manyforge::core::TrajectoryWaypointDTO>>(),
             py::arg("joint_names"), py::arg("waypoints"))
        .def_readwrite("joint_names", &manyforge::core::TrajectoryDTO::joint_names)
        .def_readwrite("waypoints", &manyforge::core::TrajectoryDTO::waypoints)
        .def("empty", &manyforge::core::TrajectoryDTO::empty)
        .def("__repr__", [](const manyforge::core::TrajectoryDTO& t) {
            return py::str("TrajectoryDTO(joint_names={}, waypoints={})")
                .format(py::cast(t.joint_names), t.waypoints.size());
        });

    // ========================================================================
    // Execution Status and Request DTOs
    // ========================================================================

    py::class_<manyforge::core::ExecutionStatus>(m, "ExecutionStatus")
        .def(py::init<>())
        .def_readwrite("state", &manyforge::core::ExecutionStatus::state)
        .def_readwrite("last_error", &manyforge::core::ExecutionStatus::last_error)
        .def_readwrite("progress", &manyforge::core::ExecutionStatus::progress)
        .def_readwrite("last_transition_ns", &manyforge::core::ExecutionStatus::last_transition_ns)
        .def_readwrite("execution_id", &manyforge::core::ExecutionStatus::execution_id)
        .def_readwrite("active_move_id", &manyforge::core::ExecutionStatus::active_move_id)
        .def("__repr__", [](const manyforge::core::ExecutionStatus& s) {
            return py::str("ExecutionStatus(state={}, execution_id={}, progress={}, active_move_id='{}')")
                .format(py::cast(s.state), s.execution_id, s.progress, s.active_move_id);
        });

    py::class_<manyforge::core::ExecuteRequestDTO>(m, "ExecuteRequestDTO")
        .def(py::init<>())
        .def(py::init<const std::string&, const std::string&, const std::string&,
                      const std::string&, const std::string&,
                      manyforge::core::TolerancesDTO, manyforge::core::SpeedProfileDTO,
                      manyforge::core::TimeoutsDTO, manyforge::core::CacheMode,
                      manyforge::core::MotionSpecDTO>(),
             py::arg("robot_id"), py::arg("group_id"), py::arg("move_id"),
             py::arg("base_frame"), py::arg("tcp_frame"),
             py::arg("tolerances"), py::arg("speed_profile"),
             py::arg("timeouts"), py::arg("cache_mode"), py::arg("motion"))
        .def_readwrite("robot_id", &manyforge::core::ExecuteRequestDTO::robot_id)
        .def_readwrite("group_id", &manyforge::core::ExecuteRequestDTO::group_id)
        .def_readwrite("move_id", &manyforge::core::ExecuteRequestDTO::move_id)
        .def_readwrite("base_frame", &manyforge::core::ExecuteRequestDTO::base_frame)
        .def_readwrite("tcp_frame", &manyforge::core::ExecuteRequestDTO::tcp_frame)
        .def_readwrite("tolerances", &manyforge::core::ExecuteRequestDTO::tolerances)
        .def_readwrite("speed_profile", &manyforge::core::ExecuteRequestDTO::speed_profile)
        .def_readwrite("per_joint_dynamics_limits", &manyforge::core::ExecuteRequestDTO::per_joint_dynamics_limits)
        .def_readwrite("timeouts", &manyforge::core::ExecuteRequestDTO::timeouts)
        .def_readwrite("cache_mode", &manyforge::core::ExecuteRequestDTO::cache_mode)
        .def_readwrite("motion", &manyforge::core::ExecuteRequestDTO::motion)
        .def_readwrite("planning_profile",
                       &manyforge::core::ExecuteRequestDTO::planning_profile)
        .def_readwrite("target_mode",
                       &manyforge::core::ExecuteRequestDTO::target_mode)
        .def_readwrite("target_reference",
                       &manyforge::core::ExecuteRequestDTO::target_reference)
        .def_readwrite("offset_reference",
                       &manyforge::core::ExecuteRequestDTO::offset_reference)
        .def("__repr__", [](const manyforge::core::ExecuteRequestDTO& r) {
            return py::str("ExecuteRequestDTO(robot_id='{}', group_id='{}', move_id='{}')")
                .format(r.robot_id, r.group_id, r.move_id);
        });

    // ========================================================================
    // Execution Result DTOs
    // ========================================================================

    py::class_<manyforge::core::ExecutionHandle>(m, "ExecutionHandle")
        .def(py::init<>())
        .def("__repr__", [](const manyforge::core::ExecutionHandle&) {
            return "ExecutionHandle()";
        });

    py::class_<manyforge::core::StopResultDTO>(m, "StopResultDTO")
        .def(py::init<>())
        .def_readwrite("accepted", &manyforge::core::StopResultDTO::accepted)
        .def_readwrite("error", &manyforge::core::StopResultDTO::error)
        .def("__repr__", [](const manyforge::core::StopResultDTO& s) {
            return py::str("StopResultDTO(accepted={})").format(s.accepted);
        });

    py::class_<manyforge::core::PauseResultDTO>(m, "PauseResultDTO")
        .def(py::init<>())
        .def_readwrite("accepted", &manyforge::core::PauseResultDTO::accepted)
        .def_readwrite("error", &manyforge::core::PauseResultDTO::error)
        .def("__repr__", [](const manyforge::core::PauseResultDTO& p) {
            return py::str("PauseResultDTO(accepted={})").format(p.accepted);
        });

    py::class_<manyforge::core::ResumeResultDTO>(m, "ResumeResultDTO")
        .def(py::init<>())
        .def_readwrite("accepted", &manyforge::core::ResumeResultDTO::accepted)
        .def_readwrite("error", &manyforge::core::ResumeResultDTO::error)
        .def("__repr__", [](const manyforge::core::ResumeResultDTO& r) {
            return py::str("ResumeResultDTO(accepted={})").format(r.accepted);
        });

    py::class_<manyforge::core::ResetResultDTO>(m, "ResetResultDTO")
        .def(py::init<>())
        .def_readwrite("accepted", &manyforge::core::ResetResultDTO::accepted)
        .def_readwrite("error", &manyforge::core::ResetResultDTO::error)
        .def("__repr__", [](const manyforge::core::ResetResultDTO& r) {
            return py::str("ResetResultDTO(accepted={})").format(r.accepted);
        });

    // ========================================================================
    // Planning DTOs
    // ========================================================================

    py::class_<manyforge::core::PlanRequestDTO>(m, "PlanRequestDTO")
        .def(py::init<>())
        .def(py::init<const std::string&, const std::string&, const std::string&,
                      const std::string&, const std::string&,
                      manyforge::core::RobotStateDTO, manyforge::core::MotionSpecDTO,
                      manyforge::core::TolerancesDTO, manyforge::core::SpeedProfileDTO,
                      double>(),
             py::arg("robot_id"), py::arg("group_id"), py::arg("move_id"),
             py::arg("base_frame"), py::arg("tcp_frame"),
             py::arg("start_state"), py::arg("motion"),
             py::arg("tolerances"), py::arg("speed_profile"),
             py::arg("planning_timeout_s"))
        .def_readwrite("robot_id", &manyforge::core::PlanRequestDTO::robot_id)
        .def_readwrite("group_id", &manyforge::core::PlanRequestDTO::group_id)
        .def_readwrite("move_id", &manyforge::core::PlanRequestDTO::move_id)
        .def_readwrite("base_frame", &manyforge::core::PlanRequestDTO::base_frame)
        .def_readwrite("tcp_frame", &manyforge::core::PlanRequestDTO::tcp_frame)
        .def_readwrite("start_state", &manyforge::core::PlanRequestDTO::start_state)
        .def_readwrite("motion", &manyforge::core::PlanRequestDTO::motion)
        .def_readwrite("tolerances", &manyforge::core::PlanRequestDTO::tolerances)
        .def_readwrite("speed_profile", &manyforge::core::PlanRequestDTO::speed_profile)
        .def_readwrite("planning_timeout_s", &manyforge::core::PlanRequestDTO::planning_timeout_s)
        .def_readwrite("planning_profile",
                       &manyforge::core::PlanRequestDTO::planning_profile)
        .def_readwrite("per_joint_dynamics_limits", &manyforge::core::PlanRequestDTO::per_joint_dynamics_limits)
        .def_readwrite("scene_binding", &manyforge::core::PlanRequestDTO::scene_binding)
        .def("__repr__", [](const manyforge::core::PlanRequestDTO& p) {
            return py::str("PlanRequestDTO(robot_id='{}', group_id='{}')")
                .format(p.robot_id, p.group_id);
        });

    py::class_<manyforge::core::PlanResultDTO>(m, "PlanResultDTO")
        .def(py::init<>())
        .def_readwrite("trajectory", &manyforge::core::PlanResultDTO::trajectory)
        .def_readwrite("error", &manyforge::core::PlanResultDTO::error)
        .def_readwrite("scene_revision", &manyforge::core::PlanResultDTO::scene_revision)
        .def("__repr__", [](const manyforge::core::PlanResultDTO& p) {
            return py::str("PlanResultDTO(trajectory={})")
                .format(p.trajectory.has_value() ? "present" : "absent");
        });

    // ========================================================================
    // Validation DTOs
    // ========================================================================

    py::class_<manyforge::core::ValidationOptionsDTO>(m, "ValidationOptionsDTO")
        .def(py::init<>())
        .def(py::init<double, const std::string&, const std::string&, bool>(),
             py::arg("max_tcp_linear_speed_mps") = 0.0,
             py::arg("tcp_frame"), py::arg("base_frame"),
             py::arg("run_collision_checks") = true)
        .def_readwrite("max_tcp_linear_speed_mps", &manyforge::core::ValidationOptionsDTO::max_tcp_linear_speed_mps)
        .def_readwrite("tcp_frame", &manyforge::core::ValidationOptionsDTO::tcp_frame)
        .def_readwrite("base_frame", &manyforge::core::ValidationOptionsDTO::base_frame)
        .def_readwrite("run_collision_checks", &manyforge::core::ValidationOptionsDTO::run_collision_checks)
        .def("__repr__", [](const manyforge::core::ValidationOptionsDTO& v) {
            return py::str("ValidationOptionsDTO(max_tcp_linear_speed_mps={}, tcp_frame='{}', base_frame='{}', run_collision_checks={})")
                .format(v.max_tcp_linear_speed_mps, v.tcp_frame, v.base_frame, v.run_collision_checks);
        });

    py::class_<manyforge::core::ValidationResultDTO>(m, "ValidationResultDTO")
        .def(py::init<>())
        .def_readwrite("valid", &manyforge::core::ValidationResultDTO::valid)
        .def_readwrite("error", &manyforge::core::ValidationResultDTO::error)
        .def("__repr__", [](const manyforge::core::ValidationResultDTO& v) {
            return py::str("ValidationResultDTO(valid={})").format(v.valid);
        });

    py::class_<manyforge::core::CollisionCheckRequestDTO>(m, "CollisionCheckRequestDTO")
        .def(py::init<>())
        .def(py::init<manyforge::core::RobotStateDTO>(), py::arg("robot_state"))
        .def_readwrite("robot_state", &manyforge::core::CollisionCheckRequestDTO::robot_state)
        .def("__repr__", [](const manyforge::core::CollisionCheckRequestDTO&) {
            return "CollisionCheckRequestDTO(robot_state=...)";
        });

    py::class_<manyforge::core::CollisionCheckResultDTO>(m, "CollisionCheckResultDTO")
        .def(py::init<>())
        .def_readwrite("in_collision", &manyforge::core::CollisionCheckResultDTO::in_collision)
        .def_readwrite("contact_body_a", &manyforge::core::CollisionCheckResultDTO::contact_body_a)
        .def_readwrite("contact_body_b", &manyforge::core::CollisionCheckResultDTO::contact_body_b)
        .def_readwrite("penetration_depth_m", &manyforge::core::CollisionCheckResultDTO::penetration_depth_m)
        .def_readwrite("estimated_force_n", &manyforge::core::CollisionCheckResultDTO::estimated_force_n)
        .def("__repr__", [](const manyforge::core::CollisionCheckResultDTO& c) {
            return py::str("CollisionCheckResultDTO(in_collision={})").format(c.in_collision);
        });

    py::class_<manyforge::core::FkRequestDTO>(m, "FkRequestDTO")
        .def(py::init<>())
        .def(py::init<manyforge::core::RobotStateDTO, const std::string&, const std::string&>(),
             py::arg("robot_state"), py::arg("target_frame"), py::arg("base_frame"))
        .def_readwrite("robot_state", &manyforge::core::FkRequestDTO::robot_state)
        .def_readwrite("target_frame", &manyforge::core::FkRequestDTO::target_frame)
        .def_readwrite("base_frame", &manyforge::core::FkRequestDTO::base_frame)
        .def("__repr__", [](const manyforge::core::FkRequestDTO& f) {
            return py::str("FkRequestDTO(target_frame='{}', base_frame='{}')")
                .format(f.target_frame, f.base_frame);
        });

    py::class_<manyforge::core::JointLimitsDTO>(m, "JointLimitsDTO")
        .def(py::init<>())
        .def(py::init<std::vector<std::string>, std::vector<double>, std::vector<double>,
                      std::vector<double>, std::vector<double>>(),
             py::arg("joint_names"),
             py::arg("position_lower_rad"),
             py::arg("position_upper_rad"),
             py::arg("velocity_max_rad_s"),
             py::arg("acceleration_max_rad_s2") = std::vector<double>{})
        .def_readwrite("joint_names", &manyforge::core::JointLimitsDTO::joint_names)
        .def_readwrite("position_lower_rad", &manyforge::core::JointLimitsDTO::position_lower_rad)
        .def_readwrite("position_upper_rad", &manyforge::core::JointLimitsDTO::position_upper_rad)
        .def_readwrite("velocity_max_rad_s", &manyforge::core::JointLimitsDTO::velocity_max_rad_s)
        .def_readwrite("acceleration_max_rad_s2", &manyforge::core::JointLimitsDTO::acceleration_max_rad_s2)
        .def("__repr__", [](const manyforge::core::JointLimitsDTO& j) {
            return py::str("JointLimitsDTO(joint_names={})").format(py::cast(j.joint_names));
        });

    // ========================================================================
    // Scene Edit DTOs
    // ========================================================================

    py::class_<manyforge::core::BoxShapeDTO>(m, "BoxShapeDTO")
        .def(py::init<>())
        .def(py::init<manyforge::core::Vector3DTO>(), py::arg("dimensions_m"))
        .def_readwrite("dimensions_m", &manyforge::core::BoxShapeDTO::dimensions_m)
        .def("__repr__", [](const manyforge::core::BoxShapeDTO& b) {
            return py::str("BoxShapeDTO(dimensions_m={})").format(py::cast(b.dimensions_m));
        });

    py::class_<manyforge::core::SphereShapeDTO>(m, "SphereShapeDTO")
        .def(py::init<>())
        .def(py::init<double>(), py::arg("radius_m"))
        .def_readwrite("radius_m", &manyforge::core::SphereShapeDTO::radius_m)
        .def("__repr__", [](const manyforge::core::SphereShapeDTO& s) {
            return py::str("SphereShapeDTO(radius_m={})").format(s.radius_m);
        });

    py::class_<manyforge::core::CylinderShapeDTO>(m, "CylinderShapeDTO")
        .def(py::init<>())
        .def(py::init<double, double>(), py::arg("radius_m"), py::arg("height_m"))
        .def_readwrite("radius_m", &manyforge::core::CylinderShapeDTO::radius_m)
        .def_readwrite("height_m", &manyforge::core::CylinderShapeDTO::height_m)
        .def("__repr__", [](const manyforge::core::CylinderShapeDTO& c) {
            return py::str("CylinderShapeDTO(radius_m={}, height_m={})")
                .format(c.radius_m, c.height_m);
        });

    py::class_<manyforge::core::MeshShapeDTO>(m, "MeshShapeDTO")
        .def(py::init<>())
        .def(py::init<const std::string&, manyforge::core::Vector3DTO>(),
             py::arg("resource_uri"), py::arg("scale"))
        .def_readwrite("resource_uri", &manyforge::core::MeshShapeDTO::resource_uri)
        .def_readwrite("scale", &manyforge::core::MeshShapeDTO::scale)
        .def("__repr__", [](const manyforge::core::MeshShapeDTO& m) {
            return py::str("MeshShapeDTO(resource_uri='{}')").format(m.resource_uri);
        });

    py::class_<manyforge::core::AddObjectRequestDTO>(m, "AddObjectRequestDTO")
        .def(py::init<>())
        .def(py::init<const std::string&, manyforge::core::CollisionShapeDTO,
                      manyforge::core::Pose3DTO, const std::string&>(),
             py::arg("object_id"), py::arg("shape"), py::arg("pose"), py::arg("frame_id"))
        .def_readwrite("object_id", &manyforge::core::AddObjectRequestDTO::object_id)
        .def_readwrite("shape", &manyforge::core::AddObjectRequestDTO::shape)
        .def_readwrite("pose", &manyforge::core::AddObjectRequestDTO::pose)
        .def_readwrite("frame_id", &manyforge::core::AddObjectRequestDTO::frame_id)
        .def("__repr__", [](const manyforge::core::AddObjectRequestDTO& a) {
            return py::str("AddObjectRequestDTO(object_id='{}')").format(a.object_id);
        });

    py::class_<manyforge::core::RemoveObjectRequestDTO>(m, "RemoveObjectRequestDTO")
        .def(py::init<>())
        .def(py::init<const std::string&>(), py::arg("object_id"))
        .def_readwrite("object_id", &manyforge::core::RemoveObjectRequestDTO::object_id)
        .def("__repr__", [](const manyforge::core::RemoveObjectRequestDTO& r) {
            return py::str("RemoveObjectRequestDTO(object_id='{}')").format(r.object_id);
        });

    py::class_<manyforge::core::UpdateObjectPoseRequestDTO>(m, "UpdateObjectPoseRequestDTO")
        .def(py::init<>())
        .def(py::init<const std::string&, manyforge::core::Pose3DTO, const std::string&>(),
             py::arg("object_id"), py::arg("pose"), py::arg("frame_id"))
        .def_readwrite("object_id", &manyforge::core::UpdateObjectPoseRequestDTO::object_id)
        .def_readwrite("pose", &manyforge::core::UpdateObjectPoseRequestDTO::pose)
        .def_readwrite("frame_id", &manyforge::core::UpdateObjectPoseRequestDTO::frame_id)
        .def_readwrite("source_id", &manyforge::core::UpdateObjectPoseRequestDTO::source_id)
        .def_readwrite("source_timestamp_ns",
                       &manyforge::core::UpdateObjectPoseRequestDTO::source_timestamp_ns)
        .def_readwrite(
            "source_freshness_budget_ns",
            &manyforge::core::UpdateObjectPoseRequestDTO::source_freshness_budget_ns)
        .def_readwrite("scene_admission_role",
                       &manyforge::core::UpdateObjectPoseRequestDTO::scene_admission_role)
        .def_readwrite("scene_admission_lease_max_age_ns",
                       &manyforge::core::UpdateObjectPoseRequestDTO::scene_admission_lease_max_age_ns)
        .def_readwrite(
            "scene_admission_target_drift_tolerance_m",
            &manyforge::core::UpdateObjectPoseRequestDTO::scene_admission_target_drift_tolerance_m)
        .def_readwrite(
            "scene_admission_target_angular_tolerance_rad",
            &manyforge::core::UpdateObjectPoseRequestDTO::scene_admission_target_angular_tolerance_rad)
        .def_readwrite(
            "scene_admission_obstacle_drift_tolerance_m",
            &manyforge::core::UpdateObjectPoseRequestDTO::scene_admission_obstacle_drift_tolerance_m)
        .def_readwrite(
            "scene_admission_obstacle_angular_tolerance_rad",
            &manyforge::core::UpdateObjectPoseRequestDTO::scene_admission_obstacle_angular_tolerance_rad)
        .def_readwrite(
            "scene_admission_allow_runtime_dynamic_obstacle",
            &manyforge::core::UpdateObjectPoseRequestDTO::scene_admission_allow_runtime_dynamic_obstacle)
        .def_readwrite("expected_attached",
                       &manyforge::core::UpdateObjectPoseRequestDTO::expected_attached)
        .def("__repr__", [](const manyforge::core::UpdateObjectPoseRequestDTO& u) {
            return py::str("UpdateObjectPoseRequestDTO(object_id='{}')").format(u.object_id);
        });

    py::class_<manyforge::core::ObjectStateDTO>(m, "ObjectStateDTO")
        .def(py::init<>())
        .def(py::init<const std::string&, bool, bool, std::optional<std::string>>(),
             py::arg("object_id"),
             py::arg("exists"),
             py::arg("attached"),
             py::arg("frame_id") = std::nullopt)
        .def_readwrite("object_id", &manyforge::core::ObjectStateDTO::object_id)
        .def_readwrite("exists", &manyforge::core::ObjectStateDTO::exists)
        .def_readwrite("attached", &manyforge::core::ObjectStateDTO::attached)
        .def_readwrite("frame_id", &manyforge::core::ObjectStateDTO::frame_id)
        .def("__repr__", [](const manyforge::core::ObjectStateDTO& o) {
            return py::str("ObjectStateDTO(object_id='{}', exists={}, attached={}, frame_id={})")
                .format(o.object_id, o.exists, o.attached,
                        o.frame_id.has_value() ? py::cast(*o.frame_id) : py::none());
        });

    py::class_<manyforge::core::SceneEditResultDTO>(m, "SceneEditResultDTO")
        .def(py::init<>())
        .def_readwrite("success", &manyforge::core::SceneEditResultDTO::success)
        .def_readwrite("error", &manyforge::core::SceneEditResultDTO::error)
        .def_readwrite("update_status",
                       &manyforge::core::SceneEditResultDTO::update_status)
        .def_readwrite(
            "collision_world_revision_changed",
            &manyforge::core::SceneEditResultDTO::collision_world_revision_changed)
        .def_readwrite(
            "collision_world_freshness_changed",
            &manyforge::core::SceneEditResultDTO::collision_world_freshness_changed)
        .def_readwrite("rollback_proven",
                       &manyforge::core::SceneEditResultDTO::rollback_proven)
        .def("__repr__", [](const manyforge::core::SceneEditResultDTO& s) {
            return py::str("SceneEditResultDTO(success={})").format(s.success);
        });

    // ========================================================================
    // Normalized Collision DTOs (spec §3.8.1)
    //
    // During the historical collision-ownership rollout, these DTOs rode
    // alongside the existing CollisionCheckRequestDTO /
    // CollisionCheckResultDTO through Chunks 1-4 and became the kernel's
    // source of truth in Chunk 5.
    // ========================================================================

    py::enum_<manyforge::core::CollisionOwnerKind>(m, "CollisionOwnerKind")
        .value("kRobotLink", manyforge::core::CollisionOwnerKind::kRobotLink)
        .value("kAttachedObject", manyforge::core::CollisionOwnerKind::kAttachedObject)
        .value("kDetachedObject", manyforge::core::CollisionOwnerKind::kDetachedObject)
        .value("kFixture", manyforge::core::CollisionOwnerKind::kFixture);

    py::enum_<manyforge::core::CollisionGeometryKind>(m, "CollisionGeometryKind")
        .value("kBox", manyforge::core::CollisionGeometryKind::kBox)
        .value("kSphere", manyforge::core::CollisionGeometryKind::kSphere)
        .value("kCylinder", manyforge::core::CollisionGeometryKind::kCylinder)
        .value("kCapsule", manyforge::core::CollisionGeometryKind::kCapsule)
        .value("kConvexMesh", manyforge::core::CollisionGeometryKind::kConvexMesh)
        .value("kTriangleMesh", manyforge::core::CollisionGeometryKind::kTriangleMesh)
        .value("kCompound", manyforge::core::CollisionGeometryKind::kCompound);

    py::enum_<manyforge::core::CollisionSurface>(m, "CollisionSurface")
        .value("kPreExecutionValidation",
               manyforge::core::CollisionSurface::kPreExecutionValidation)
        .value("kPredictedRuntimeMonitor",
               manyforge::core::CollisionSurface::kPredictedRuntimeMonitor)
        .value("kObservedRuntimeMonitor",
               manyforge::core::CollisionSurface::kObservedRuntimeMonitor)
        .value("kObservedRuntimeLookahead",
               manyforge::core::CollisionSurface::kObservedRuntimeLookahead)
        .value("kDiagnosticCompare", manyforge::core::CollisionSurface::kDiagnosticCompare)
        .value("kPlannerHint", manyforge::core::CollisionSurface::kPlannerHint);

    py::enum_<manyforge::core::CollisionSamplingMode>(m, "CollisionSamplingMode")
        .value("kWaypointOnly", manyforge::core::CollisionSamplingMode::kWaypointOnly)
        .value("kInterpolatedDiscrete",
               manyforge::core::CollisionSamplingMode::kInterpolatedDiscrete)
        .value("kCcd", manyforge::core::CollisionSamplingMode::kCcd)
        .value("kEngineDefault", manyforge::core::CollisionSamplingMode::kEngineDefault);

    py::enum_<manyforge::core::CollisionDistanceRequestMode>(
        m, "CollisionDistanceRequestMode")
        .value("kNone",
               manyforge::core::CollisionDistanceRequestMode::kNone)
        .value("kSelectedCollidingPairs",
               manyforge::core::CollisionDistanceRequestMode::
                   kSelectedCollidingPairs)
        .value("kAllCollidingPairs",
               manyforge::core::CollisionDistanceRequestMode::
                   kAllCollidingPairs)
        .value("kAllActivePairs",
               manyforge::core::CollisionDistanceRequestMode::
                   kAllActivePairs);

    py::enum_<manyforge::core::CollisionEngineStatus>(m, "CollisionEngineStatus")
        .value("kOk", manyforge::core::CollisionEngineStatus::kOk)
        .value("kTimeout", manyforge::core::CollisionEngineStatus::kTimeout)
        .value("kUnsupportedGeometry",
               manyforge::core::CollisionEngineStatus::kUnsupportedGeometry)
        .value("kDiagnosticIncomplete",
               manyforge::core::CollisionEngineStatus::kDiagnosticIncomplete)
        .value("kCancelled", manyforge::core::CollisionEngineStatus::kCancelled)
        .value("kEngineError", manyforge::core::CollisionEngineStatus::kEngineError);

    py::enum_<manyforge::core::CollisionDecisionResult>(m, "CollisionDecisionResult")
        .value("kPass", manyforge::core::CollisionDecisionResult::kPass)
        .value("kFail", manyforge::core::CollisionDecisionResult::kFail)
        .value("kWarn", manyforge::core::CollisionDecisionResult::kWarn)
        .value("kStopRequested",
               manyforge::core::CollisionDecisionResult::kStopRequested)
        .value("kInconclusiveFail",
               manyforge::core::CollisionDecisionResult::kInconclusiveFail);

    py::enum_<manyforge::core::CollisionDecisionSeverity>(m, "CollisionDecisionSeverity")
        .value("kNone", manyforge::core::CollisionDecisionSeverity::kNone)
        .value("kWarn", manyforge::core::CollisionDecisionSeverity::kWarn)
        .value("kBlock", manyforge::core::CollisionDecisionSeverity::kBlock);

    py::enum_<manyforge::core::CollisionDecisionAction>(m, "CollisionDecisionAction")
        .value("kDispatch", manyforge::core::CollisionDecisionAction::kDispatch)
        .value("kReject", manyforge::core::CollisionDecisionAction::kReject)
        .value("kWarnOnly", manyforge::core::CollisionDecisionAction::kWarnOnly)
        .value("kInconclusive",
               manyforge::core::CollisionDecisionAction::kInconclusive);

    py::class_<manyforge::core::CollisionWorldRevision>(m, "CollisionWorldRevision")
        .def(py::init<>())
        .def(py::init<std::uint64_t, const std::string&>(),
             py::arg("revision_id"), py::arg("content_hash"))
        .def_readwrite("revision_id",
                       &manyforge::core::CollisionWorldRevision::revision_id)
        .def_readwrite("content_hash",
                       &manyforge::core::CollisionWorldRevision::content_hash)
        .def("__repr__", [](const manyforge::core::CollisionWorldRevision& r) {
            return py::str("CollisionWorldRevision(revision_id={}, content_hash='{}')")
                .format(r.revision_id, r.content_hash);
        });

    py::class_<manyforge::core::SceneRevisionBindingDTO>(m, "SceneRevisionBindingDTO")
        .def(py::init<>())
        .def(py::init<manyforge::core::SceneRevisionDTO, const std::string&>(),
             py::arg("revision"), py::arg("projection_lease_id"))
        .def_readwrite("revision",
                       &manyforge::core::SceneRevisionBindingDTO::revision)
        .def_readwrite(
            "projection_lease_id",
            &manyforge::core::SceneRevisionBindingDTO::projection_lease_id)
        .def("valid", &manyforge::core::SceneRevisionBindingDTO::valid);

    py::enum_<manyforge::core::CollisionWorldMutationSource>(m, "CollisionWorldMutationSource")
        .value("kProgramLoad",
               manyforge::core::CollisionWorldMutationSource::kProgramLoad)
        .value("kSceneEdit",
               manyforge::core::CollisionWorldMutationSource::kSceneEdit)
        .value("kAttach", manyforge::core::CollisionWorldMutationSource::kAttach)
        .value("kDetach", manyforge::core::CollisionWorldMutationSource::kDetach)
        .value("kPoseSourceApply",
               manyforge::core::CollisionWorldMutationSource::kPoseSourceApply)
        .value("kDeploymentLoad",
               manyforge::core::CollisionWorldMutationSource::kDeploymentLoad)
        .value("kPolicyUpdate",
               manyforge::core::CollisionWorldMutationSource::kPolicyUpdate)
        .value("kProjectionRebuild",
               manyforge::core::CollisionWorldMutationSource::kProjectionRebuild);

    py::class_<manyforge::core::CollisionWorldLedgerEntry>(m, "CollisionWorldLedgerEntry")
        .def(py::init<>())
        .def_readwrite("revision",
                       &manyforge::core::CollisionWorldLedgerEntry::revision)
        .def_readwrite("previous_revision_id",
                       &manyforge::core::CollisionWorldLedgerEntry::previous_revision_id)
        .def_readwrite("mutation_source",
                       &manyforge::core::CollisionWorldLedgerEntry::mutation_source)
        .def_readwrite("affected_body_ids",
                       &manyforge::core::CollisionWorldLedgerEntry::affected_body_ids)
        .def_readwrite("timestamp_ns",
                       &manyforge::core::CollisionWorldLedgerEntry::timestamp_ns)
        .def_readwrite("authority_source",
                       &manyforge::core::CollisionWorldLedgerEntry::authority_source)
        .def_readwrite("session_id",
                       &manyforge::core::CollisionWorldLedgerEntry::session_id)
        .def("__repr__", [](const manyforge::core::CollisionWorldLedgerEntry& e) {
            return py::str(
                       "CollisionWorldLedgerEntry(revision_id={}, source='{}')")
                .format(e.revision.revision_id, e.authority_source);
        });

    py::class_<manyforge::core::CollisionPairId>(m, "CollisionPairId")
        .def(py::init<>())
        .def(py::init<const std::string&, const std::string&>(),
             py::arg("body_a"), py::arg("body_b"))
        .def_readwrite("body_a", &manyforge::core::CollisionPairId::body_a)
        .def_readwrite("body_b", &manyforge::core::CollisionPairId::body_b)
        .def("__repr__", [](const manyforge::core::CollisionPairId& p) {
            return py::str("CollisionPairId(body_a='{}', body_b='{}')")
                .format(p.body_a, p.body_b);
        });

    py::enum_<manyforge::core::CollisionPairClassification>(m,
                                                            "CollisionPairClassification")
        .value("kBlockingCollision",
               manyforge::core::CollisionPairClassification::kBlockingCollision)
        .value("kAllowedStaticStatic",
               manyforge::core::CollisionPairClassification::kAllowedStaticStatic)
        .value("kSupportContactStart",
               manyforge::core::CollisionPairClassification::kSupportContactStart)
        .value("kSupportContactContinued",
               manyforge::core::CollisionPairClassification::kSupportContactContinued)
        .value("kSupportContactSeparated",
               manyforge::core::CollisionPairClassification::kSupportContactSeparated)
        .value("kSupportContactRecontactViolation",
               manyforge::core::CollisionPairClassification::
                   kSupportContactRecontactViolation)
        .value("kAllowedTouchLink",
               manyforge::core::CollisionPairClassification::kAllowedTouchLink)
        .value("kAllowedContactGrasp",
               manyforge::core::CollisionPairClassification::kAllowedContactGrasp)
        .value("kDiagnosticIncomplete",
               manyforge::core::CollisionPairClassification::kDiagnosticIncomplete);

    py::class_<manyforge::core::CollisionPairResult>(m, "CollisionPairResult")
        .def(py::init<>())
        .def_readwrite("pair", &manyforge::core::CollisionPairResult::pair)
        .def_readwrite("classification",
                       &manyforge::core::CollisionPairResult::classification)
        .def_readwrite("is_first_failure",
                       &manyforge::core::CollisionPairResult::is_first_failure)
        .def("__repr__", [](const manyforge::core::CollisionPairResult& r) {
            return py::str("CollisionPairResult(is_first_failure={})")
                .format(r.is_first_failure);
        });

    py::class_<manyforge::core::CollisionPairClearance>(m,
                                                        "CollisionPairClearance")
        .def(py::init<>())
        .def_readwrite("pair",
                       &manyforge::core::CollisionPairClearance::pair)
        .def_readwrite(
            "signed_clearance_m",
            &manyforge::core::CollisionPairClearance::signed_clearance_m)
        .def("__repr__", [](const manyforge::core::CollisionPairClearance& r) {
            return py::str("CollisionPairClearance(signed_clearance_m={})")
                .format(r.signed_clearance_m);
        });

    py::enum_<manyforge::core::CollisionContactPolicyMode>(
        m, "CollisionContactPolicyMode")
        .value("kContactRelease",
               manyforge::core::CollisionContactPolicyMode::kContactRelease)
        .value("kBoundedPenetration",
               manyforge::core::CollisionContactPolicyMode::
                   kBoundedPenetration)
        .value("kContactProcess",
               manyforge::core::CollisionContactPolicyMode::kContactProcess);

    py::enum_<manyforge::core::ContactReleaseEvidenceMode>(
        m, "ContactReleaseEvidenceMode")
        .value("kSignedPenetration",
               manyforge::core::ContactReleaseEvidenceMode::
                   kSignedPenetration)
        .value("kBinaryContact",
               manyforge::core::ContactReleaseEvidenceMode::kBinaryContact);

    py::class_<manyforge::core::SupportContactPolicyEntry>(m,
                                                           "SupportContactPolicyEntry")
        .def(py::init<>())
        .def_readwrite("pair",
                       &manyforge::core::SupportContactPolicyEntry::pair)
        .def_readwrite("mode",
                       &manyforge::core::SupportContactPolicyEntry::mode)
        .def_readwrite(
            "release_evidence_mode",
            &manyforge::core::SupportContactPolicyEntry::release_evidence_mode)
        .def_readwrite(
            "enforce_max_start_penetration",
            &manyforge::core::SupportContactPolicyEntry::
                enforce_max_start_penetration)
        .def_readwrite(
            "max_start_penetration_m",
            &manyforge::core::SupportContactPolicyEntry::max_start_penetration_m)
        .def_readwrite(
            "release_tolerance_m",
            &manyforge::core::SupportContactPolicyEntry::release_tolerance_m)
        .def_readwrite(
            "min_release_progress_m",
            &manyforge::core::SupportContactPolicyEntry::min_release_progress_m)
        .def_readwrite(
            "max_contact_penetration_m",
            &manyforge::core::SupportContactPolicyEntry::
                max_contact_penetration_m)
        .def_readwrite(
            "require_separation_by_end",
            &manyforge::core::SupportContactPolicyEntry::require_separation_by_end)
        .def_readwrite(
            "require_strictly_decreasing_penetration",
            &manyforge::core::SupportContactPolicyEntry::
                require_strictly_decreasing_penetration)
        .def_readwrite("allow_recontact",
                       &manyforge::core::SupportContactPolicyEntry::allow_recontact);
    py::class_<manyforge::core::ContactGraspPolicyEntry>(m,
                                                         "ContactGraspPolicyEntry")
        .def(py::init<>())
        .def_readwrite("policy_id",
                       &manyforge::core::ContactGraspPolicyEntry::policy_id)
        .def_readwrite("selected_object_id",
                       &manyforge::core::ContactGraspPolicyEntry::selected_object_id)
        .def_readwrite("allowed_robot_links",
                       &manyforge::core::ContactGraspPolicyEntry::allowed_robot_links)
        .def_readwrite("max_penetration_m",
                       &manyforge::core::ContactGraspPolicyEntry::max_penetration_m)
        .def_readwrite("terminal_window_start_fraction",
                       &manyforge::core::ContactGraspPolicyEntry::
                           terminal_window_start_fraction);

    py::class_<manyforge::core::CollisionPolicyRevision>(m,
                                                         "CollisionPolicyRevision")
        .def(py::init<>())
        .def(py::init<std::uint64_t, const std::string&>(),
             py::arg("revision_id"), py::arg("content_hash"))
        .def_readwrite("revision_id",
                       &manyforge::core::CollisionPolicyRevision::revision_id)
        .def_readwrite("content_hash",
                       &manyforge::core::CollisionPolicyRevision::content_hash);

    py::class_<manyforge::core::CollisionPolicySnapshot>(m,
                                                         "CollisionPolicySnapshot")
        .def(py::init<>())
        .def_readwrite("policy_id",
                       &manyforge::core::CollisionPolicySnapshot::policy_id)
        .def_readwrite("revision",
                       &manyforge::core::CollisionPolicySnapshot::revision)
        .def_readwrite(
            "allowed_static_static_pairs",
            &manyforge::core::CollisionPolicySnapshot::allowed_static_static_pairs)
        .def_readwrite(
            "allowed_touch_link_pairs",
            &manyforge::core::CollisionPolicySnapshot::allowed_touch_link_pairs)
        .def_readwrite(
            "support_contacts",
            &manyforge::core::CollisionPolicySnapshot::support_contacts)
        .def_readwrite(
            "contact_grasps",
            &manyforge::core::CollisionPolicySnapshot::contact_grasps)
        .def_readwrite(
            "require_full_diagnostics",
            &manyforge::core::CollisionPolicySnapshot::require_full_diagnostics);

    py::class_<manyforge::core::CollisionGeometryDescriptor>(m, "CollisionGeometryDescriptor")
        .def(py::init<>())
        .def_readwrite("body_id",
                       &manyforge::core::CollisionGeometryDescriptor::body_id)
        .def_readwrite("owner_kind",
                       &manyforge::core::CollisionGeometryDescriptor::owner_kind)
        .def_readwrite("geometry_kind",
                       &manyforge::core::CollisionGeometryDescriptor::geometry_kind)
        .def_readwrite("local_pose",
                       &manyforge::core::CollisionGeometryDescriptor::local_pose)
        .def_readwrite(
            "box_dimensions_m",
            &manyforge::core::CollisionGeometryDescriptor::box_dimensions_m)
        .def_readwrite(
            "sphere_radius_m",
            &manyforge::core::CollisionGeometryDescriptor::sphere_radius_m)
        .def_readwrite(
            "cylinder_radius_m",
            &manyforge::core::CollisionGeometryDescriptor::cylinder_radius_m)
        .def_readwrite(
            "cylinder_height_m",
            &manyforge::core::CollisionGeometryDescriptor::cylinder_height_m)
        .def_readwrite(
            "capsule_radius_m",
            &manyforge::core::CollisionGeometryDescriptor::capsule_radius_m)
        .def_readwrite(
            "capsule_height_m",
            &manyforge::core::CollisionGeometryDescriptor::capsule_height_m)
        .def_readwrite("mesh_scale",
                       &manyforge::core::CollisionGeometryDescriptor::mesh_scale)
        .def_readwrite("source_uri",
                       &manyforge::core::CollisionGeometryDescriptor::source_uri)
        .def_readwrite("source_hash",
                       &manyforge::core::CollisionGeometryDescriptor::source_hash)
        .def_readwrite("quality_labels",
                       &manyforge::core::CollisionGeometryDescriptor::quality_labels)
        .def_readwrite("triangle_count",
                       &manyforge::core::CollisionGeometryDescriptor::triangle_count)
        .def_readwrite("convex_part_count",
                       &manyforge::core::CollisionGeometryDescriptor::convex_part_count)
        .def_readwrite("memory_estimate_bytes",
                       &manyforge::core::CollisionGeometryDescriptor::memory_estimate_bytes)
        .def("__repr__", [](const manyforge::core::CollisionGeometryDescriptor& g) {
            return py::str("CollisionGeometryDescriptor(body_id='{}')")
                .format(g.body_id);
        });

    py::class_<manyforge::core::CollisionWorldBody>(m, "CollisionWorldBody")
        .def(py::init<>())
        .def_readwrite("body_id",
                       &manyforge::core::CollisionWorldBody::body_id)
        .def_readwrite("owner_kind",
                       &manyforge::core::CollisionWorldBody::owner_kind)
        .def_readwrite("parent_link",
                       &manyforge::core::CollisionWorldBody::parent_link)
        .def_readwrite("frame_id",
                       &manyforge::core::CollisionWorldBody::frame_id)
        .def_readwrite("pose", &manyforge::core::CollisionWorldBody::pose)
        .def_readwrite("source_id",
                       &manyforge::core::CollisionWorldBody::source_id)
        .def_readwrite(
            "source_timestamp_ns",
            &manyforge::core::CollisionWorldBody::source_timestamp_ns)
        .def_readwrite(
            "source_freshness_budget_ns",
            &manyforge::core::CollisionWorldBody::source_freshness_budget_ns)
        .def_readwrite("geometries",
                       &manyforge::core::CollisionWorldBody::geometries)
        .def_readwrite("body_content_hash",
                       &manyforge::core::CollisionWorldBody::body_content_hash)
        .def("__repr__", [](const manyforge::core::CollisionWorldBody& b) {
            return py::str("CollisionWorldBody(body_id='{}')")
                .format(b.body_id);
        });

    py::class_<manyforge::core::CollisionRobotModelFrame>(
        m, "CollisionRobotModelFrame")
        .def(py::init<>())
        .def_readwrite("robot_id",
                       &manyforge::core::CollisionRobotModelFrame::robot_id)
        .def_readwrite(
            "model_frame_id",
            &manyforge::core::CollisionRobotModelFrame::model_frame_id)
        .def_readwrite("world_pose",
                       &manyforge::core::CollisionRobotModelFrame::world_pose)
        .def("__repr__", [](const manyforge::core::CollisionRobotModelFrame& f) {
            return py::str(
                       "CollisionRobotModelFrame(robot_id='{}', "
                       "model_frame_id='{}')")
                .format(f.robot_id, f.model_frame_id);
        });

    py::class_<manyforge::core::CollisionWorldSnapshot,
               std::shared_ptr<manyforge::core::CollisionWorldSnapshot>>(
        m, "CollisionWorldSnapshot")
        .def(py::init<>())
        .def_readwrite(
            "snapshot_format_version",
            &manyforge::core::CollisionWorldSnapshot::snapshot_format_version)
        .def_readwrite("publisher_id",
                       &manyforge::core::CollisionWorldSnapshot::publisher_id)
        .def_readwrite("revision",
                       &manyforge::core::CollisionWorldSnapshot::revision)
        .def_readwrite("emitted_at_ns",
                       &manyforge::core::CollisionWorldSnapshot::emitted_at_ns)
        .def_readwrite(
            "robot_model_frames",
            &manyforge::core::CollisionWorldSnapshot::robot_model_frames)
        .def_readwrite("bodies",
                       &manyforge::core::CollisionWorldSnapshot::bodies)
        .def_readwrite("body_count",
                       &manyforge::core::CollisionWorldSnapshot::body_count)
        .def_readwrite(
            "geometry_count",
            &manyforge::core::CollisionWorldSnapshot::geometry_count)
        .def_readwrite(
            "robot_link_count",
            &manyforge::core::CollisionWorldSnapshot::robot_link_count)
        .def_readwrite(
            "attached_object_count",
            &manyforge::core::CollisionWorldSnapshot::attached_object_count)
        .def_readwrite(
            "detached_object_count",
            &manyforge::core::CollisionWorldSnapshot::detached_object_count)
        .def_readwrite("fixture_count",
                       &manyforge::core::CollisionWorldSnapshot::fixture_count)
        .def_readwrite(
            "contains_mesh_geometry",
            &manyforge::core::CollisionWorldSnapshot::contains_mesh_geometry)
        .def_readwrite(
            "all_bodies_have_geometry",
            &manyforge::core::CollisionWorldSnapshot::all_bodies_have_geometry)
        .def("__repr__", [](const manyforge::core::CollisionWorldSnapshot& s) {
            return py::str(
                       "CollisionWorldSnapshot(publisher_id='{}', "
                       "revision_id={}, body_count={}, geometry_count={})")
                .format(s.publisher_id, s.revision.revision_id,
                        s.body_count, s.geometry_count);
        });

    // Builder. `build()` raises ValueError on invariant violation; that is
    // the idiomatic Python surface for a fail-closed assembler. The
    // setters return self so callers can chain the same way they do in C++.
    py::class_<manyforge::core::CollisionWorldSnapshotBuilder>(
        m, "CollisionWorldSnapshotBuilder")
        .def(py::init<>())
        .def("set_publisher_id",
             &manyforge::core::CollisionWorldSnapshotBuilder::set_publisher_id,
             py::return_value_policy::reference_internal)
        .def(
            "set_snapshot_format_version",
            &manyforge::core::CollisionWorldSnapshotBuilder::
                set_snapshot_format_version,
            py::return_value_policy::reference_internal)
        .def("set_revision_id",
             &manyforge::core::CollisionWorldSnapshotBuilder::set_revision_id,
             py::return_value_policy::reference_internal)
        .def(
            "set_emitted_at_ns",
            &manyforge::core::CollisionWorldSnapshotBuilder::set_emitted_at_ns,
            py::return_value_policy::reference_internal)
        .def(
            "add_robot_model_frame",
            &manyforge::core::CollisionWorldSnapshotBuilder::
                add_robot_model_frame,
            py::return_value_policy::reference_internal)
        .def("add_body",
             &manyforge::core::CollisionWorldSnapshotBuilder::add_body,
             py::return_value_policy::reference_internal)
        .def("build",
             [](manyforge::core::CollisionWorldSnapshotBuilder& self) {
                 auto result = self.build();
                 if (!result.ok()) {
                     throw py::value_error(result.error().message);
                 }
                 return result.value();
             });

    py::class_<manyforge::core::CollisionQuery>(m, "CollisionQuery")
        .def(py::init<>())
        .def_readwrite("query_id", &manyforge::core::CollisionQuery::query_id)
        .def_readwrite("validation_id",
                       &manyforge::core::CollisionQuery::validation_id)
        .def_readwrite("surface", &manyforge::core::CollisionQuery::surface)
        .def_readwrite("collision_world_revision",
                       &manyforge::core::CollisionQuery::collision_world_revision)
        .def_readwrite("policy_revision",
                       &manyforge::core::CollisionQuery::policy_revision)
        .def_readwrite("robot_state",
                       &manyforge::core::CollisionQuery::robot_state)
        .def_readwrite("waypoint_index",
                       &manyforge::core::CollisionQuery::waypoint_index)
        .def_readwrite("state_time_ns",
                       &manyforge::core::CollisionQuery::state_time_ns)
        .def_readwrite("sampling_mode",
                       &manyforge::core::CollisionQuery::sampling_mode)
        .def_readwrite("timeout_budget_ns",
                       &manyforge::core::CollisionQuery::timeout_budget_ns)
        .def_readwrite("required_capabilities",
                       &manyforge::core::CollisionQuery::required_capabilities)
        .def_readwrite(
            "distance_request_mode",
            &manyforge::core::CollisionQuery::distance_request_mode)
        .def_readwrite(
            "selected_distance_pairs",
            &manyforge::core::CollisionQuery::selected_distance_pairs)
        .def_readwrite("max_contacts",
                       &manyforge::core::CollisionQuery::max_contacts)
        .def("__repr__", [](const manyforge::core::CollisionQuery& q) {
            return py::str("CollisionQuery(query_id='{}')").format(q.query_id);
        });

    py::class_<manyforge::core::CollisionResult>(m, "CollisionResult")
        .def(py::init<>())
        .def_readwrite("in_collision",
                       &manyforge::core::CollisionResult::in_collision)
        .def_readwrite("contact_body_a",
                       &manyforge::core::CollisionResult::contact_body_a)
        .def_readwrite("contact_body_b",
                       &manyforge::core::CollisionResult::contact_body_b)
        .def_readwrite("normalized_pairs",
                       &manyforge::core::CollisionResult::normalized_pairs)
        .def_readwrite("query_id", &manyforge::core::CollisionResult::query_id)
        .def_readwrite("collision_world_revision",
                       &manyforge::core::CollisionResult::collision_world_revision)
        .def_readwrite("policy_revision",
                       &manyforge::core::CollisionResult::policy_revision)
        .def_readwrite("surface", &manyforge::core::CollisionResult::surface)
        .def_readwrite("state_time_ns",
                       &manyforge::core::CollisionResult::state_time_ns)
        .def_readwrite("engine_status",
                       &manyforge::core::CollisionResult::engine_status)
        .def_readwrite("decision", &manyforge::core::CollisionResult::decision)
        .def_readwrite("engine_id", &manyforge::core::CollisionResult::engine_id)
        .def_readwrite("engine_version",
                       &manyforge::core::CollisionResult::engine_version)
        .def_readwrite("query_duration_ns",
                       &manyforge::core::CollisionResult::query_duration_ns)
        .def_readwrite("sampling_mode",
                       &manyforge::core::CollisionResult::sampling_mode)
        .def_readwrite("min_clearance_m",
                       &manyforge::core::CollisionResult::min_clearance_m)
        .def_readwrite("min_clearance_pair",
                       &manyforge::core::CollisionResult::min_clearance_pair)
        .def_readwrite("pair_clearances",
                       &manyforge::core::CollisionResult::pair_clearances)
        .def_readwrite(
            "normalized_pairs_classified",
            &manyforge::core::CollisionResult::normalized_pairs_classified)
        .def_readwrite("policy_revision_used",
                       &manyforge::core::CollisionResult::policy_revision_used)
        .def_readwrite("raw_diagnostics",
                       &manyforge::core::CollisionResult::raw_diagnostics)
        .def_readwrite(
            "distance_request_mode",
            &manyforge::core::CollisionResult::distance_request_mode)
        .def_readwrite(
            "evaluated_distance_pair_count",
            &manyforge::core::CollisionResult::evaluated_distance_pair_count)
        .def("__repr__", [](const manyforge::core::CollisionResult& r) {
            return py::str("CollisionResult(in_collision={}, pairs={})")
                .format(r.in_collision, r.normalized_pairs.size());
        });

    py::class_<manyforge::core::CollisionDecision>(m, "CollisionDecision")
        .def(py::init<>())
        .def_readwrite("accepted", &manyforge::core::CollisionDecision::accepted)
        .def_readwrite("first_failing_waypoint",
                       &manyforge::core::CollisionDecision::first_failing_waypoint)
        .def_readwrite("first_failing_time_ns",
                       &manyforge::core::CollisionDecision::first_failing_time_ns)
        .def_readwrite("severity",
                       &manyforge::core::CollisionDecision::severity)
        .def_readwrite("action", &manyforge::core::CollisionDecision::action)
        .def_readwrite("collision_world_revision",
                       &manyforge::core::CollisionDecision::collision_world_revision)
        .def_readwrite("failure_archive_id",
                       &manyforge::core::CollisionDecision::failure_archive_id)
        .def("__repr__", [](const manyforge::core::CollisionDecision& d) {
            return py::str("CollisionDecision(accepted={})").format(d.accepted);
        });

    py::enum_<manyforge::core::CollisionFailureKind>(m, "CollisionFailureKind")
        .value("kBlockingCollision",
               manyforge::core::CollisionFailureKind::kBlockingCollision)
        .value("kSupportContactViolation",
               manyforge::core::CollisionFailureKind::kSupportContactViolation)
        .value("kDiagnosticIncomplete",
               manyforge::core::CollisionFailureKind::kDiagnosticIncomplete)
        .value("kEngineError",
               manyforge::core::CollisionFailureKind::kEngineError)
        .value("kCancelled",
               manyforge::core::CollisionFailureKind::kCancelled)
        .value("kSceneChangedDuringValidation",
               manyforge::core::CollisionFailureKind::
                   kSceneChangedDuringValidation);

    py::class_<manyforge::core::CollisionFailureArchiveEntry>(
        m, "CollisionFailureArchiveEntry")
        .def(py::init<>())
        .def_readwrite("entry_id",
                       &manyforge::core::CollisionFailureArchiveEntry::entry_id)
        .def_readwrite("emitted_at_ns",
                       &manyforge::core::CollisionFailureArchiveEntry::
                           emitted_at_ns)
        .def_readwrite("engine_id",
                       &manyforge::core::CollisionFailureArchiveEntry::engine_id)
        .def_readwrite(
            "engine_version",
            &manyforge::core::CollisionFailureArchiveEntry::engine_version)
        .def_readwrite("collision_world_revision",
                       &manyforge::core::CollisionFailureArchiveEntry::
                           collision_world_revision)
        .def_readwrite("policy_revision",
                       &manyforge::core::CollisionFailureArchiveEntry::
                           policy_revision)
        .def_readwrite("failure_kind",
                       &manyforge::core::CollisionFailureArchiveEntry::
                           failure_kind)
        .def_readwrite("decision_action",
                       &manyforge::core::CollisionFailureArchiveEntry::
                           decision_action)
        .def_readwrite("decision_severity",
                       &manyforge::core::CollisionFailureArchiveEntry::
                           decision_severity)
        .def_readwrite("normalized_pairs",
                       &manyforge::core::CollisionFailureArchiveEntry::
                           normalized_pairs)
        .def_readwrite("first_failing_waypoint",
                       &manyforge::core::CollisionFailureArchiveEntry::
                           first_failing_waypoint)
        .def_readwrite("first_failing_time_ns",
                       &manyforge::core::CollisionFailureArchiveEntry::
                           first_failing_time_ns)
        .def_readwrite("trajectory_waypoint_count",
                       &manyforge::core::CollisionFailureArchiveEntry::
                           trajectory_waypoint_count)
        .def_readwrite("query_duration_ns",
                       &manyforge::core::CollisionFailureArchiveEntry::
                           query_duration_ns)
        .def_readwrite("worker_count_actually_used",
                       &manyforge::core::CollisionFailureArchiveEntry::
                           worker_count_actually_used)
        .def_readwrite("fallback_reason",
                       &manyforge::core::CollisionFailureArchiveEntry::
                           fallback_reason)
        .def_readwrite("raw_diagnostics",
                       &manyforge::core::CollisionFailureArchiveEntry::
                           raw_diagnostics)
        .def_readwrite("correlation_id",
                       &manyforge::core::CollisionFailureArchiveEntry::
                           correlation_id)
        .def("__repr__", [](const manyforge::core::CollisionFailureArchiveEntry& e) {
            return py::str("CollisionFailureArchiveEntry(entry_id='{}')")
                .format(e.entry_id);
        });

    // ========================================================================
    // Cache Config
    // Port interfaces, kernel, cache, and state machine are C++-only.
    // Python interacts with the kernel via NativeKernelClient (manyforge_roboplan_py).
}
