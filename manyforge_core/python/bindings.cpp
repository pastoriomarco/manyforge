#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/operators.h>
#include <Python.h>

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

namespace py = pybind11;

// ============================================================================
// Module definition
// ============================================================================

PYBIND11_MODULE(manyforge_core_py, m) {
    m.doc() = "Python bindings for manyforge_core DTOs and enums";

    // ========================================================================
    // Enums
    // ========================================================================

    py::enum_<manyforge::core::ExecutionState>(m, "ExecutionState")
        .value("kIdle", manyforge::core::ExecutionState::kIdle)
        .value("kPlanning", manyforge::core::ExecutionState::kPlanning)
        .value("kValidating", manyforge::core::ExecutionState::kValidating)
        .value("kExecuting", manyforge::core::ExecutionState::kExecuting)
        .value("kPaused", manyforge::core::ExecutionState::kPaused)
        .value("kStopping", manyforge::core::ExecutionState::kStopping)
        .value("kStopped", manyforge::core::ExecutionState::kStopped)
        .value("kCompleted", manyforge::core::ExecutionState::kCompleted)
        .value("kFailed", manyforge::core::ExecutionState::kFailed)
        .export_values();

    py::enum_<manyforge::core::CacheMode>(m, "CacheMode")
        .value("kDefault", manyforge::core::CacheMode::kDefault)
        .value("kDisable", manyforge::core::CacheMode::kDisable)
        .export_values();

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
        .export_values();

    py::class_<manyforge::core::ReferenceDTO>(m, "ReferenceDTO")
        .def(py::init<>())
        .def(py::init<manyforge::core::ReferenceDTO::Type, const std::string&>(),
             py::arg("type"), py::arg("id"))
        .def_readwrite("type", &manyforge::core::ReferenceDTO::type)
        .def_readwrite("id", &manyforge::core::ReferenceDTO::id)
        .def("__repr__", [](const manyforge::core::ReferenceDTO& r) {
            return py::str("ReferenceDTO(type={}, id='{}')").format(py::cast(r.type), r.id);
        });

    py::class_<manyforge::core::RelativeMoveDTO>(m, "RelativeMoveDTO")
        .def(py::init<>())
        .def(py::init<manyforge::core::ReferenceDTO, manyforge::core::Pose3DTO>(),
             py::arg("reference"), py::arg("offset"))
        .def_readwrite("reference", &manyforge::core::RelativeMoveDTO::reference)
        .def_readwrite("offset", &manyforge::core::RelativeMoveDTO::offset)
        .def("__repr__", [](const manyforge::core::RelativeMoveDTO& r) {
            return py::str("RelativeMoveDTO(reference={}, offset={})")
                .format(py::cast(r.reference), py::cast(r.offset));
        });

    py::class_<manyforge::core::CartesianPathDTO>(m, "CartesianPathDTO")
        .def(py::init<>())
        .def(py::init<std::vector<manyforge::core::Pose3DTO>, double, double>(),
             py::arg("waypoints"),
             py::arg("max_translation_step_m"),
             py::arg("max_rotation_step_rad"))
        .def_readwrite("waypoints", &manyforge::core::CartesianPathDTO::waypoints)
        .def_readwrite("max_translation_step_m", &manyforge::core::CartesianPathDTO::max_translation_step_m)
        .def_readwrite("max_rotation_step_rad", &manyforge::core::CartesianPathDTO::max_rotation_step_rad)
        .def("__repr__", [](const manyforge::core::CartesianPathDTO& c) {
            return py::str("CartesianPathDTO(waypoints={}, max_translation_step_m={}, max_rotation_step_rad={})")
                .format(py::cast(c.waypoints), c.max_translation_step_m, c.max_rotation_step_rad);
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
        .def_readwrite("timeouts", &manyforge::core::ExecuteRequestDTO::timeouts)
        .def_readwrite("cache_mode", &manyforge::core::ExecuteRequestDTO::cache_mode)
        .def_readwrite("motion", &manyforge::core::ExecuteRequestDTO::motion)
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
        .def("__repr__", [](const manyforge::core::PlanRequestDTO& p) {
            return py::str("PlanRequestDTO(robot_id='{}', group_id='{}')")
                .format(p.robot_id, p.group_id);
        });

    py::class_<manyforge::core::PlanResultDTO>(m, "PlanResultDTO")
        .def(py::init<>())
        .def_readwrite("trajectory", &manyforge::core::PlanResultDTO::trajectory)
        .def_readwrite("error", &manyforge::core::PlanResultDTO::error)
        .def("__repr__", [](const manyforge::core::PlanResultDTO& p) {
            return py::str("PlanResultDTO(trajectory={})")
                .format(p.trajectory.has_value() ? "present" : "absent");
        });

    // ========================================================================
    // Validation DTOs
    // ========================================================================

    py::class_<manyforge::core::ValidationOptionsDTO>(m, "ValidationOptionsDTO")
        .def(py::init<>())
        .def(py::init<double, const std::string&, const std::string&>(),
             py::arg("max_tcp_linear_speed_mps") = 0.0,
             py::arg("tcp_frame"), py::arg("base_frame"))
        .def_readwrite("max_tcp_linear_speed_mps", &manyforge::core::ValidationOptionsDTO::max_tcp_linear_speed_mps)
        .def_readwrite("tcp_frame", &manyforge::core::ValidationOptionsDTO::tcp_frame)
        .def_readwrite("base_frame", &manyforge::core::ValidationOptionsDTO::base_frame)
        .def("__repr__", [](const manyforge::core::ValidationOptionsDTO& v) {
            return py::str("ValidationOptionsDTO(max_tcp_linear_speed_mps={}, tcp_frame='{}', base_frame='{}')")
                .format(v.max_tcp_linear_speed_mps, v.tcp_frame, v.base_frame);
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
        .def("__repr__", [](const manyforge::core::UpdateObjectPoseRequestDTO& u) {
            return py::str("UpdateObjectPoseRequestDTO(object_id='{}')").format(u.object_id);
        });

    py::class_<manyforge::core::SceneEditResultDTO>(m, "SceneEditResultDTO")
        .def(py::init<>())
        .def_readwrite("success", &manyforge::core::SceneEditResultDTO::success)
        .def_readwrite("error", &manyforge::core::SceneEditResultDTO::error)
        .def("__repr__", [](const manyforge::core::SceneEditResultDTO& s) {
            return py::str("SceneEditResultDTO(success={})").format(s.success);
        });

    // ========================================================================
    // Cache Config
    // Port interfaces, kernel, cache, and state machine are C++-only.
    // Python interacts with the kernel via NativeKernelClient (manyforge_roboplan_py).
}
