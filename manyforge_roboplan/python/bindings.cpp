#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>

#include <cstdint>
#include <cmath>
#include <optional>
#include <stdexcept>

#include <Eigen/Dense>

#include <manyforge_roboplan/native_kernel_factory.hpp>
#ifdef MANYFORGE_ROS2_CONTROL_ENABLED
#include <manyforge_roboplan/ros2_control_execution_backend.hpp>
#endif

namespace py = pybind11;
using namespace manyforge::roboplan_backend;
using namespace manyforge::core;

namespace {

py::dict error_details_to_python(
    const std::map<std::string, std::string>& details) {
  py::dict d;
  for (const auto& [key, value] : details) {
    d[py::cast(key)] = value;
  }
  return d;
}

// Helper: convert Result<uint64_t> to Python dict
py::dict result_to_python(const Result<uint64_t>& result) {
  py::dict d;
  d["ok"] = result.ok();
  if (result.ok()) {
    d["execution_id"] = result.value();
  } else {
    d["error"] = result.error().message;
  }
  return d;
}

// Helper: convert ExecutionStatus to Python dict
py::dict status_to_python(const ExecutionStatus& status) {
  py::dict d;
  d["state"] = static_cast<int>(status.state);
  d["state_name"] = to_string(status.state);
  d["execution_id"] = status.execution_id;
  d["progress"] = status.progress;
  d["active_move_id"] = status.active_move_id;
  d["last_error"] = to_string(status.last_error);
  return d;
}

// Helper: convert ExecutionSnapshot to Python dict
py::dict snapshot_to_python(const ExecutionSnapshot& snapshot) {
  py::dict d;
  d["timestamp_ns"] = snapshot.timestamp_ns;
  d["sequence_number"] = snapshot.sequence_number;
  d["execution_id"] = snapshot.execution_id;
  d["move_id"] = snapshot.move_id;
  d["state"] = static_cast<int>(snapshot.state);
  d["state_name"] = to_string(snapshot.state);
  d["progress"] = snapshot.progress;
  d["joint_positions_rad"] = snapshot.joint_positions_rad;
  if (snapshot.error.has_value()) {
    d["error"] = snapshot.error->message;
    d["error_code"] = snapshot.error->code;
    d["error_details"] = error_details_to_python(snapshot.error->details);
  } else {
    d["error"] = py::none();
    d["error_code"] = py::none();
    d["error_details"] = py::dict();
  }
  return d;
}

// Helper: convert Pose3DTO to Python dict
py::dict pose_to_python(const Pose3DTO& pose) {
  py::dict d;
  d["position_m"] = py::make_tuple(
      pose.position_m.x_m, pose.position_m.y_m, pose.position_m.z_m);
  d["orientation_xyzw"] = py::make_tuple(
      pose.orientation.x, pose.orientation.y,
      pose.orientation.z, pose.orientation.w);
  return d;
}

Pose3DTO multiply_poses_for_direct_plan(const Pose3DTO& a, const Pose3DTO& b) {
  auto pose_to_matrix = [](const Pose3DTO& pose) {
    Eigen::Matrix4d matrix = Eigen::Matrix4d::Identity();
    const double x = pose.orientation.x;
    const double y = pose.orientation.y;
    const double z = pose.orientation.z;
    const double w = pose.orientation.w;
    matrix(0, 0) = 1 - 2 * y * y - 2 * z * z;
    matrix(0, 1) = 2 * x * y - 2 * z * w;
    matrix(0, 2) = 2 * x * z + 2 * y * w;
    matrix(1, 0) = 2 * x * y + 2 * z * w;
    matrix(1, 1) = 1 - 2 * x * x - 2 * z * z;
    matrix(1, 2) = 2 * y * z - 2 * x * w;
    matrix(2, 0) = 2 * x * z - 2 * y * w;
    matrix(2, 1) = 2 * y * z + 2 * x * w;
    matrix(2, 2) = 1 - 2 * x * x - 2 * y * y;
    matrix(0, 3) = pose.position_m.x_m;
    matrix(1, 3) = pose.position_m.y_m;
    matrix(2, 3) = pose.position_m.z_m;
    return matrix;
  };

  auto matrix_to_pose = [](const Eigen::Matrix4d& matrix) {
    Pose3DTO pose;
    pose.position_m = {matrix(0, 3), matrix(1, 3), matrix(2, 3)};

    const Eigen::Matrix3d rotation = matrix.block<3, 3>(0, 0);
    Eigen::Quaterniond quaternion(rotation);
    quaternion.normalize();
    pose.orientation = {
        quaternion.x(), quaternion.y(), quaternion.z(), quaternion.w()};
    return pose;
  };

  return matrix_to_pose(pose_to_matrix(a) * pose_to_matrix(b));
}

Pose3DTO pose_with_position_and_orientation_for_direct_plan(
    const Vector3DTO& position,
    const QuaternionDTO& orientation) {
  Pose3DTO pose;
  pose.position_m = position;
  pose.orientation = orientation;
  return pose;
}

std::optional<ErrorDTO> resolve_motion_spec_for_direct_plan(
    NativeKernelBundle& bundle,
    const ExecuteRequestDTO& request,
    MotionSpecDTO& resolved_motion) {
  if (const auto* relative_move = std::get_if<RelativeMoveDTO>(&request.motion)) {
    auto target_pose = bundle.context->resolve_reference_pose(
        relative_move->target_reference, request.base_frame);
    if (!target_pose.ok()) {
      return target_pose.error();
    }

    Pose3DTO offset_basis_pose = target_pose.value();
    if (relative_move->offset_reference.has_value()) {
      auto offset_reference_pose = bundle.context->resolve_reference_pose(
          *relative_move->offset_reference, request.base_frame);
      if (!offset_reference_pose.ok()) {
        return offset_reference_pose.error();
      }
      offset_basis_pose = offset_reference_pose.value();
    }

    PoseGoalDTO pose_goal;
    pose_goal.goal = multiply_poses_for_direct_plan(
        pose_with_position_and_orientation_for_direct_plan(
            target_pose.value().position_m,
            offset_basis_pose.orientation),
        relative_move->offset);
    resolved_motion = MotionSpecDTO{pose_goal};
    return std::nullopt;
  }

  resolved_motion = request.motion;
  return std::nullopt;
}

Pose3DTO pose_from_python(const py::dict& pose_dict) {
  Pose3DTO pose{};
  pose.position_m = {0.0, 0.0, 0.0};
  pose.orientation = {0.0, 0.0, 0.0, 1.0};
  if (pose_dict.contains("position_m")) {
    auto pos = pose_dict["position_m"].cast<std::vector<double>>();
    if (pos.size() != 3) {
      throw std::invalid_argument("position_m must have exactly 3 elements");
    }
    pose.position_m = {pos[0], pos[1], pos[2]};
  }
  if (pose_dict.contains("orientation_xyzw")) {
    auto orient = pose_dict["orientation_xyzw"].cast<std::vector<double>>();
    if (orient.size() != 4) {
      throw std::invalid_argument("orientation_xyzw must have exactly 4 elements");
    }
    pose.orientation = {orient[0], orient[1], orient[2], orient[3]};
  }
  return pose;
}

std::vector<SupportContactDTO> support_contacts_from_python(const py::dict& request) {
  std::vector<SupportContactDTO> contacts;
  if (!request.contains("support_contacts") || request["support_contacts"].is_none()) {
    return contacts;
  }
  if (!py::isinstance<py::list>(request["support_contacts"])) {
    throw std::invalid_argument("support_contacts must be a list");
  }
  for (const auto item : request["support_contacts"].cast<py::list>()) {
    if (!py::isinstance<py::dict>(item)) {
      throw std::invalid_argument("support_contacts entries must be dictionaries");
    }
    const py::dict contact_dict = py::reinterpret_borrow<py::dict>(item);
    if (!contact_dict.contains("support_object_id")) {
      throw std::invalid_argument(
          "support_contacts entries require support_object_id");
    }
    SupportContactDTO contact;
    contact.support_object_id =
        contact_dict["support_object_id"].cast<std::string>();
    if (contact.support_object_id.empty()) {
      throw std::invalid_argument("support_object_id must not be empty");
    }
    if (contact_dict.contains("max_start_penetration_m")) {
      contact.max_start_penetration_m =
          contact_dict["max_start_penetration_m"].cast<double>();
      if (contact.max_start_penetration_m < 0.0) {
        throw std::invalid_argument("max_start_penetration_m must be >= 0");
      }
    }
    if (contact_dict.contains("require_separation_by_end")) {
      contact.require_separation_by_end =
          contact_dict["require_separation_by_end"].cast<bool>();
      if (!contact.require_separation_by_end) {
        throw std::invalid_argument(
            "require_separation_by_end=false is not supported");
      }
    }
    contacts.push_back(std::move(contact));
  }
  return contacts;
}

// Helper: convert RobotStateDTO to Python dict
py::dict robot_state_to_python(const RobotStateDTO& state) {
  py::dict d;
  d["joint_names"] = state.joint_names;
  d["positions_rad"] = state.positions_rad;
  d["velocities_rad_s"] = state.velocities_rad_s;
  return d;
}

py::dict trajectory_to_python(const TrajectoryDTO& trajectory) {
  py::dict d;
  d["joint_names"] = trajectory.joint_names;
  py::list waypoints;
  for (const auto& wp : trajectory.waypoints) {
    py::dict wp_dict;
    wp_dict["time_from_start_s"] = wp.time_from_start_s;
    wp_dict["positions_rad"] = wp.positions_rad;
    wp_dict["velocities_rad_s"] = wp.velocities_rad_s;
    wp_dict["accelerations_rad_s2"] = wp.accelerations_rad_s2;
    waypoints.append(wp_dict);
  }
  d["waypoints"] = waypoints;
  return d;
}

// Helper: convert ObjectStateDTO to Python dict
py::dict object_state_to_python(const ObjectStateDTO& state) {
  py::dict d;
  d["object_id"] = state.object_id;
  d["exists"] = state.exists;
  d["attached"] = state.attached;
  if (state.frame_id.has_value()) {
    d["frame_id"] = *state.frame_id;
  } else {
    d["frame_id"] = py::none();
  }
  return d;
}

py::dict scene_trace_event_to_python(
    const RoboPlanPlanningContext::SceneTraceEvent& event) {
  py::dict d;
  d["timestamp_ns"] = event.timestamp_ns;
  d["source"] = "backend";
  d["event"] = event.event;
  d["object_id"] = event.object_id;
  d["success"] = event.success;
  if (event.frame_id.has_value()) {
    d["frame_id"] = *event.frame_id;
  } else {
    d["frame_id"] = py::none();
  }
  d["attached"] = event.attached;
  if (event.pose_in_frame.has_value()) {
    d["pose_in_frame"] = pose_to_python(*event.pose_in_frame);
  } else {
    d["pose_in_frame"] = py::none();
  }
  if (event.pose_in_universe.has_value()) {
    d["pose_in_universe"] = pose_to_python(*event.pose_in_universe);
  } else {
    d["pose_in_universe"] = py::none();
  }
  if (event.error.has_value()) {
    d["error"] = *event.error;
  } else {
    d["error"] = py::none();
  }
  return d;
}

// Helper: KernelEventType to string
const char* kernel_event_type_to_string(KernelEventType type) {
  switch (type) {
    case KernelEventType::kStateTransition: return "state_transition";
    case KernelEventType::kPlanStarted: return "plan_started";
    case KernelEventType::kPlanCompleted: return "plan_completed";
    case KernelEventType::kCacheHit: return "cache_hit";
    case KernelEventType::kCacheMiss: return "cache_miss";
    case KernelEventType::kCacheInvalidated: return "cache_invalidated";
    case KernelEventType::kValidationStarted: return "validation_started";
    case KernelEventType::kValidationPassed: return "validation_passed";
    case KernelEventType::kValidationFailed: return "validation_failed";
    case KernelEventType::kExecuteStarted: return "execute_started";
    case KernelEventType::kExecuteCompleted: return "execute_completed";
    case KernelEventType::kStopRequested: return "stop_requested";
    case KernelEventType::kStopIssued: return "stop_issued";
    case KernelEventType::kPauseRequested: return "pause_requested";
    case KernelEventType::kResumeRequested: return "resume_requested";
    case KernelEventType::kCompletedAcknowledged: return "acknowledge_completed";
    case KernelEventType::kErrorRaised: return "error_raised";
    case KernelEventType::kCollisionDetected: return "collision_detected";
    default: return "unknown";
  }
}

// Helper: convert KernelEvent to Python dict
py::dict kernel_event_to_python(const KernelEvent& event) {
  py::dict d;
  d["type"] = kernel_event_type_to_string(event.type);
  d["execution_id"] = event.execution_id;
  d["timestamp_ns"] = event.timestamp_ns;
  if (event.type == KernelEventType::kStateTransition) {
    d["from_state"] = to_string(event.from_state);
    d["to_state"] = to_string(event.to_state);
  }
  if (event.error.has_value()) {
    py::dict err;
    err["code"] = event.error->code;
    err["message"] = event.error->message;
    d["error"] = err;
  }
  if (!event.details.empty()) {
    py::dict details;
    for (const auto& [key, value] : event.details) {
      details[py::cast(key)] = value;
    }
    d["details"] = details;
  }
  return d;
}

// Helper: convert stop/pause/resume/reset results
py::dict command_result(bool accepted, const std::optional<ErrorDTO>& error) {
  py::dict d;
  d["accepted"] = accepted;
  if (error.has_value()) {
    d["error"] = error->message;
  }
  return d;
}

// Helper: convert MetricsSnapshot to Python dict
py::dict metrics_to_python(const MetricsSnapshot& metrics) {
  py::dict d;
  d["cache_hits"] = metrics.cache_hits;
  d["cache_misses"] = metrics.cache_misses;
  d["cache_invalidations"] = metrics.cache_invalidations;
  d["plans_succeeded"] = metrics.plans_succeeded;
  d["plans_failed"] = metrics.plans_failed;
  d["validations_passed"] = metrics.validations_passed;
  d["validations_failed"] = metrics.validations_failed;
  d["executions_completed"] = metrics.executions_completed;
  d["executions_failed"] = metrics.executions_failed;
  d["stops_issued"] = metrics.stops_issued;
  d["collision_detections"] = metrics.collision_detections;
  d["snapshots_published"] = metrics.snapshots_published;
  d["observer_lag_max_ns"] = metrics.observer_lag_max_ns;
  return d;
}

py::dict shape_to_python(const CollisionShapeDTO& shape) {
  py::dict d;
  if (const auto* box = std::get_if<BoxShapeDTO>(&shape)) {
    d["type"] = "box";
    d["box_dimensions_m"] = py::make_tuple(
        box->dimensions_m.x_m, box->dimensions_m.y_m, box->dimensions_m.z_m);
    return d;
  }
  if (const auto* sphere = std::get_if<SphereShapeDTO>(&shape)) {
    d["type"] = "sphere";
    d["sphere_radius_m"] = sphere->radius_m;
    return d;
  }
  if (const auto* cylinder = std::get_if<CylinderShapeDTO>(&shape)) {
    d["type"] = "cylinder";
    d["cylinder_radius_m"] = cylinder->radius_m;
    d["cylinder_height_m"] = cylinder->height_m;
    return d;
  }
  if (const auto* mesh = std::get_if<MeshShapeDTO>(&shape)) {
    d["type"] = "mesh";
    d["mesh_resource_uri"] = mesh->resource_uri;
    d["mesh_scale"] = py::make_tuple(mesh->scale.x_m, mesh->scale.y_m, mesh->scale.z_m);
    return d;
  }
  d["type"] = "unknown";
  return d;
}

py::dict scene_object_snapshot_to_python(
    const RoboPlanPlanningContext::SceneObjectSnapshot& object) {
  py::dict d;
  d["object_id"] = object.object_id;
  d["shape"] = shape_to_python(object.shape);
  d["pose_in_frame"] = pose_to_python(object.pose_in_frame);
  d["pose_in_universe"] = pose_to_python(object.pose_in_universe);
  d["frame_id"] = object.frame_id;
  d["attached"] = object.attached;
  return d;
}

double finite_scale_from_python(const py::handle& value, const char* field_name) {
  const double result = value.cast<double>();
  if (!std::isfinite(result)) {
    throw std::invalid_argument(std::string(field_name) + " must be finite");
  }
  return result;
}

ExecutionKernelConfig kernel_config_from_python(const py::object& config_obj) {
  ExecutionKernelConfig config;
  if (config_obj.is_none()) {
    return config;
  }

  py::dict config_dict = config_obj.cast<py::dict>();
  if (config_dict.contains("collision_monitor_period_ns")) {
    const double period_ns =
        finite_scale_from_python(config_dict["collision_monitor_period_ns"],
                                 "collision_monitor_period_ns");
    if (period_ns <= 0.0) {
      throw std::invalid_argument(
          "collision_monitor_period_ns must be > 0");
    }
    config.collision_monitor_period_ns =
        static_cast<int64_t>(period_ns);
  }
  return config;
}

#ifdef MANYFORGE_ROS2_CONTROL_ENABLED
Ros2ControlExecutionBackendConfig ros2_control_config_from_python(
    const py::object& config_obj) {
  if (config_obj.is_none()) {
    throw std::invalid_argument("ros2_control config must not be None");
  }
  py::dict config_dict = config_obj.cast<py::dict>();
  Ros2ControlExecutionBackendConfig config;
  if (config_dict.contains("node_name")) {
    config.node_name = config_dict["node_name"].cast<std::string>();
  }
  if (config_dict.contains("action_name")) {
    config.action_name = config_dict["action_name"].cast<std::string>();
  }
  if (config_dict.contains("arm_action")) {
    config.action_name = config_dict["arm_action"].cast<std::string>();
  }
  if (config_dict.contains("joint_state_topic")) {
    config.joint_state_topic = config_dict["joint_state_topic"].cast<std::string>();
  }
  if (config_dict.contains("controller_state_topic")) {
    config.controller_state_topic =
        config_dict["controller_state_topic"].cast<std::string>();
  }
  if (config_dict.contains("required_joints")) {
    config.required_joints =
        config_dict["required_joints"].cast<std::vector<std::string>>();
  }
  auto read_seconds = [&](const char* s_key, const char* ms_key, double current) {
    if (config_dict.contains(s_key)) {
      return finite_scale_from_python(config_dict[s_key], s_key);
    }
    if (config_dict.contains(ms_key)) {
      return finite_scale_from_python(config_dict[ms_key], ms_key) / 1000.0;
    }
    return current;
  };
  config.joint_state_max_age_s =
      read_seconds("joint_state_max_age_s", "joint_state_max_age_ms",
                   config.joint_state_max_age_s);
  config.wait_for_state_timeout_s =
      read_seconds("wait_for_state_timeout_s", "wait_for_state_timeout_ms",
                   config.wait_for_state_timeout_s);
  config.wait_for_action_server_timeout_s =
      read_seconds("wait_for_action_server_timeout_s",
                   "wait_for_action_server_timeout_ms",
                   config.wait_for_action_server_timeout_s);
  config.tracking_error_timeout_s =
      read_seconds("tracking_error_timeout_s", "tracking_error_timeout_ms",
                   config.tracking_error_timeout_s);
  config.monitor_period_s =
      read_seconds("monitor_period_s", "monitor_period_ms", config.monitor_period_s);
  config.stop_completion_timeout_s =
      read_seconds("stop_completion_timeout_s", "stop_completion_timeout_ms",
                   config.stop_completion_timeout_s);
  config.sim_reset_motion_duration_s =
      read_seconds("sim_reset_motion_duration_s",
                   "sim_reset_motion_duration_ms",
                   config.sim_reset_motion_duration_s);

  if (config_dict.contains("start_tolerance_rad")) {
    config.start_tolerance_rad =
        finite_scale_from_python(config_dict["start_tolerance_rad"],
                                 "start_tolerance_rad");
  }
  if (config_dict.contains("final_tolerance_rad")) {
    config.final_tolerance_rad =
        finite_scale_from_python(config_dict["final_tolerance_rad"],
                                 "final_tolerance_rad");
  }
  if (config_dict.contains("tracking_error_tolerance_rad")) {
    config.tracking_error_tolerance_rad =
        finite_scale_from_python(config_dict["tracking_error_tolerance_rad"],
                                 "tracking_error_tolerance_rad");
  }
  if (config_dict.contains("require_fresh_state_before_planning")) {
    config.require_fresh_state_before_planning =
        config_dict["require_fresh_state_before_planning"].cast<bool>();
  }
  if (config_dict.contains("disable_controller_path_tolerances")) {
    config.disable_controller_path_tolerances =
        config_dict["disable_controller_path_tolerances"].cast<bool>();
  }
  if (config_dict.contains("execution_state_source_mode")) {
    config.execution_state_source_mode =
        config_dict["execution_state_source_mode"].cast<std::string>();
  }
  if (config_dict.contains("initial_joint_positions_rad")) {
    config.initial_joint_positions_rad =
        config_dict["initial_joint_positions_rad"].cast<std::vector<double>>();
  }
  if (config_dict.contains("command_gate_topic")) {
    config.command_gate_topic =
        config_dict["command_gate_topic"].cast<std::string>();
  }
  return config;
}

Ros2ControlExecutionBackend& require_ros2_control_execution(
    NativeKernelBundle& self) {
  auto* backend =
      dynamic_cast<Ros2ControlExecutionBackend*>(self.execution.get());
  if (backend == nullptr) {
    throw std::runtime_error(
        "Native kernel execution backend is not ros2_control");
  }
  return *backend;
}
#endif

}  // namespace

PYBIND11_MODULE(manyforge_roboplan_py, m) {
  m.doc() = "Native C++ kernel with RoboPlan backends (no Python trampolines)";

  // Expose NativeKernelBundle as opaque holder
  py::class_<NativeKernelBundle, std::shared_ptr<NativeKernelBundle>>(m, "NativeKernelBundle")
      .def("execute", [](NativeKernelBundle& self, py::dict request) -> py::dict {
        // Build ExecuteRequestDTO from Python dict
        ExecuteRequestDTO req;
        req.robot_id = request["robot_id"].cast<std::string>();
        req.group_id = request["group_id"].cast<std::string>();
        req.move_id = request["move_id"].cast<std::string>();
        req.base_frame = request["base_frame"].cast<std::string>();
        req.tcp_frame = request["tcp_frame"].cast<std::string>();

        // Tolerances
        req.tolerances.joint_position_tol_rad = request.attr("get")("joint_position_tol_rad", 0.05).cast<double>();
        req.tolerances.translation_tol_m = request.attr("get")("translation_tol_m", 0.005).cast<double>();
        req.tolerances.rotation_tol_rad = request.attr("get")("rotation_tol_rad", 0.05).cast<double>();

        // Speed profile
        req.speed_profile.velocity_scale = finite_scale_from_python(
            request.attr("get")("velocity_scale", 1.0),
            "velocity_scale");
        req.speed_profile.acceleration_scale = finite_scale_from_python(
            request.attr("get")("acceleration_scale", 1.0),
            "acceleration_scale");
        if (request.contains("max_tcp_linear_speed_mps")) {
          auto val = request["max_tcp_linear_speed_mps"];
          if (!val.is_none()) {
            double tcp_speed = val.cast<double>();
            if (tcp_speed > 0.0) {
              req.speed_profile.max_tcp_linear_speed_mps = tcp_speed;
            }
          }
        }

        // Timeouts
        req.timeouts.planning_timeout_s = request.attr("get")("planning_timeout_s", 5.0).cast<double>();
        req.timeouts.execution_timeout_s = request.attr("get")("execution_timeout_s", 10.0).cast<double>();

        // Cache mode
        std::string cache_mode_str = request.attr("get")("cache_mode", "default").cast<std::string>();
        req.cache_mode = (cache_mode_str == "disable") ? CacheMode::kDisable : CacheMode::kDefault;

        // Motion type
        std::string motion_type = request["motion_type"].cast<std::string>();
        if (motion_type == "joint_goal") {
          JointGoalDTO goal;
          goal.joint_names = request["joint_goal_names"].cast<std::vector<std::string>>();
          goal.positions_rad = request["joint_goal_positions_rad"].cast<std::vector<double>>();
          req.motion = goal;
        } else if (motion_type == "pose_goal") {
          PoseGoalDTO goal;
          auto pose_dict = request["pose_goal"];
          auto pos = pose_dict["position_m"].cast<std::vector<double>>();
          auto orient = pose_dict["orientation_xyzw"].cast<std::vector<double>>();
          goal.goal.position_m = {pos[0], pos[1], pos[2]};
          goal.goal.orientation = {orient[0], orient[1], orient[2], orient[3]};
          req.motion = goal;
        } else if (motion_type == "relative_move") {
          RelativeMoveDTO goal;
          auto rel_dict = request["relative_move"];
          auto parse_reference_dict = [](const py::dict& ref_dict,
                                         const std::string& field_name) {
            ReferenceDTO reference;
            const std::string ref_type = ref_dict["type"].cast<std::string>();
            if (ref_type == "frame") {
              reference.type = ReferenceDTO::Type::kFrame;
            } else if (ref_type == "object") {
              reference.type = ReferenceDTO::Type::kObject;
            } else if (ref_type == "robot_link") {
              reference.type = ReferenceDTO::Type::kRobotLink;
            } else if (ref_type == "pose") {
              reference.type = ReferenceDTO::Type::kPose;
            } else {
              throw std::runtime_error("Unsupported reference type: " + ref_type);
            }
            if (ref_dict.contains("id")) {
              reference.id = ref_dict["id"].cast<std::string>();
            }
            if (reference.type == ReferenceDTO::Type::kPose) {
              if (!ref_dict.contains("pose")) {
                throw std::runtime_error(
                    "Pose reference requires " + field_name + ".pose");
              }
              auto pose_dict = ref_dict["pose"];
              auto pos = pose_dict["position_m"].cast<std::vector<double>>();
              auto orient = pose_dict["orientation_xyzw"].cast<std::vector<double>>();
              reference.pose = Pose3DTO{};
              reference.pose->position_m = {pos[0], pos[1], pos[2]};
              reference.pose->orientation = {orient[0], orient[1], orient[2], orient[3]};
            }
            return reference;
          };

          goal.target_reference = parse_reference_dict(
              rel_dict["target_reference"].cast<py::dict>(), "target_reference");
          if (rel_dict.contains("offset_reference")) {
            goal.offset_reference = parse_reference_dict(
                rel_dict["offset_reference"].cast<py::dict>(), "offset_reference");
          }

          auto offset_dict = rel_dict["offset"];
          auto pos = offset_dict["position_m"].cast<std::vector<double>>();
          auto orient = offset_dict["orientation_xyzw"].cast<std::vector<double>>();
          goal.offset.position_m = {pos[0], pos[1], pos[2]};
          goal.offset.orientation = {orient[0], orient[1], orient[2], orient[3]};
          req.motion = goal;
        }

        auto result = [&]() {
          py::gil_scoped_release release;
          return self.kernel->execute(req);
        }();
        return result_to_python(result);
      })
      .def("preview_motion", [](NativeKernelBundle& self,
                                 py::dict request,
                                 py::object start_state_obj) -> py::dict {
        ExecuteRequestDTO req;
        req.robot_id = request["robot_id"].cast<std::string>();
        req.group_id = request["group_id"].cast<std::string>();
        req.move_id = request["move_id"].cast<std::string>();
        req.base_frame = request["base_frame"].cast<std::string>();
        req.tcp_frame = request["tcp_frame"].cast<std::string>();

        req.tolerances.joint_position_tol_rad = request.attr("get")("joint_position_tol_rad", 0.05).cast<double>();
        req.tolerances.translation_tol_m = request.attr("get")("translation_tol_m", 0.005).cast<double>();
        req.tolerances.rotation_tol_rad = request.attr("get")("rotation_tol_rad", 0.05).cast<double>();

        req.speed_profile.velocity_scale = finite_scale_from_python(
            request.attr("get")("velocity_scale", 1.0),
            "velocity_scale");
        req.speed_profile.acceleration_scale = finite_scale_from_python(
            request.attr("get")("acceleration_scale", 1.0),
            "acceleration_scale");
        if (request.contains("max_tcp_linear_speed_mps")) {
          auto val = request["max_tcp_linear_speed_mps"];
          if (!val.is_none()) {
            double tcp_speed = val.cast<double>();
            if (tcp_speed > 0.0) {
              req.speed_profile.max_tcp_linear_speed_mps = tcp_speed;
            }
          }
        }

        req.timeouts.planning_timeout_s = request.attr("get")("planning_timeout_s", 5.0).cast<double>();
        req.timeouts.execution_timeout_s = request.attr("get")("execution_timeout_s", 10.0).cast<double>();

        std::string cache_mode_str = request.attr("get")("cache_mode", "default").cast<std::string>();
        req.cache_mode = (cache_mode_str == "disable") ? CacheMode::kDisable : CacheMode::kDefault;

        std::string motion_type = request["motion_type"].cast<std::string>();
        if (motion_type == "joint_goal") {
          JointGoalDTO goal;
          goal.joint_names = request["joint_goal_names"].cast<std::vector<std::string>>();
          goal.positions_rad = request["joint_goal_positions_rad"].cast<std::vector<double>>();
          req.motion = goal;
        } else if (motion_type == "pose_goal") {
          PoseGoalDTO goal;
          auto pose_dict = request["pose_goal"];
          auto pos = pose_dict["position_m"].cast<std::vector<double>>();
          auto orient = pose_dict["orientation_xyzw"].cast<std::vector<double>>();
          goal.goal.position_m = {pos[0], pos[1], pos[2]};
          goal.goal.orientation = {orient[0], orient[1], orient[2], orient[3]};
          req.motion = goal;
        } else if (motion_type == "relative_move") {
          RelativeMoveDTO goal;
          auto rel_dict = request["relative_move"];
          auto parse_reference_dict = [](const py::dict& ref_dict,
                                         const std::string& field_name) {
            ReferenceDTO reference;
            const std::string ref_type = ref_dict["type"].cast<std::string>();
            if (ref_type == "frame") {
              reference.type = ReferenceDTO::Type::kFrame;
            } else if (ref_type == "object") {
              reference.type = ReferenceDTO::Type::kObject;
            } else if (ref_type == "robot_link") {
              reference.type = ReferenceDTO::Type::kRobotLink;
            } else if (ref_type == "pose") {
              reference.type = ReferenceDTO::Type::kPose;
            } else {
              throw std::runtime_error("Unsupported reference type: " + ref_type);
            }
            if (ref_dict.contains("id")) {
              reference.id = ref_dict["id"].cast<std::string>();
            }
            if (reference.type == ReferenceDTO::Type::kPose) {
              if (!ref_dict.contains("pose")) {
                throw std::runtime_error(
                    "Pose reference requires " + field_name + ".pose");
              }
              auto pose_dict = ref_dict["pose"];
              auto pos = pose_dict["position_m"].cast<std::vector<double>>();
              auto orient = pose_dict["orientation_xyzw"].cast<std::vector<double>>();
              reference.pose = Pose3DTO{};
              reference.pose->position_m = {pos[0], pos[1], pos[2]};
              reference.pose->orientation = {orient[0], orient[1], orient[2], orient[3]};
            }
            return reference;
          };

          goal.target_reference = parse_reference_dict(
              rel_dict["target_reference"].cast<py::dict>(), "target_reference");
          if (rel_dict.contains("offset_reference")) {
            goal.offset_reference = parse_reference_dict(
                rel_dict["offset_reference"].cast<py::dict>(), "offset_reference");
          }

          auto offset_dict = rel_dict["offset"];
          auto pos = offset_dict["position_m"].cast<std::vector<double>>();
          auto orient = offset_dict["orientation_xyzw"].cast<std::vector<double>>();
          goal.offset.position_m = {pos[0], pos[1], pos[2]};
          goal.offset.orientation = {orient[0], orient[1], orient[2], orient[3]};
          req.motion = goal;
        }

        RobotStateDTO start_state;
        if (start_state_obj.is_none()) {
          start_state = [&]() {
            py::gil_scoped_release release;
            return self.context->get_current_state();
          }();
        } else {
          py::dict start_dict = start_state_obj.cast<py::dict>();
          start_state.joint_names =
              start_dict["joint_names"].cast<std::vector<std::string>>();
          start_state.positions_rad =
              start_dict["positions_rad"].cast<std::vector<double>>();
          if (start_dict.contains("velocities_rad_s")) {
            start_state.velocities_rad_s =
                start_dict["velocities_rad_s"].cast<std::vector<double>>();
          }
        }

        PlanRequestDTO plan_request;
        plan_request.robot_id = req.robot_id;
        plan_request.group_id = req.group_id;
        plan_request.move_id = req.move_id;
        plan_request.base_frame = req.base_frame;
        plan_request.tcp_frame = req.tcp_frame;
        plan_request.start_state = start_state;
        MotionSpecDTO resolved_motion;
        if (auto error = resolve_motion_spec_for_direct_plan(
                self, req, resolved_motion)) {
          py::dict result;
          result["ok"] = false;
          result["error"] = error->message;
          result["error_details"] = error_details_to_python(error->details);
          return result;
        }
        plan_request.motion = std::move(resolved_motion);
        plan_request.tolerances = req.tolerances;
        plan_request.speed_profile = req.speed_profile;
        plan_request.planning_timeout_s = req.timeouts.planning_timeout_s;

        auto plan_result = [&]() {
          py::gil_scoped_release release;
          return self.planner->plan(plan_request);
        }();
        py::dict result;
        if (!plan_result.trajectory.has_value()) {
          result["ok"] = false;
          if (plan_result.error.has_value()) {
            result["error"] = plan_result.error->message;
            result["error_details"] =
                error_details_to_python(plan_result.error->details);
          } else {
            result["error"] = "Planning preview failed";
          }
          return result;
        }

        ValidationOptionsDTO validation_options;
        if (req.speed_profile.max_tcp_linear_speed_mps.has_value()) {
          validation_options.max_tcp_linear_speed_mps =
              *req.speed_profile.max_tcp_linear_speed_mps;
          validation_options.tcp_frame = req.tcp_frame;
          validation_options.base_frame = req.base_frame;
        }
        auto validation_result = [&]() {
          py::gil_scoped_release release;
          return self.context->validate_trajectory(
              *plan_result.trajectory, validation_options);
        }();
        if (!validation_result.valid) {
          result["ok"] = false;
          if (validation_result.error.has_value()) {
            result["error"] = validation_result.error->message;
            result["error_details"] =
                error_details_to_python(validation_result.error->details);
          } else {
            result["error"] = "Trajectory validation failed";
          }
          return result;
        }

        const auto& trajectory = *plan_result.trajectory;
        RobotStateDTO final_state;
        final_state.joint_names = trajectory.joint_names;
        if (!trajectory.waypoints.empty()) {
          const auto& final_wp = trajectory.waypoints.back();
          final_state.positions_rad = final_wp.positions_rad;
          final_state.velocities_rad_s = final_wp.velocities_rad_s;
        } else {
          final_state = start_state;
        }

        FkRequestDTO fk_request;
        fk_request.robot_state = final_state;
        fk_request.target_frame = req.tcp_frame;
        fk_request.base_frame = req.base_frame;
        auto final_tcp_pose = [&]() {
          py::gil_scoped_release release;
          return self.context->forward_kinematics(fk_request);
        }();

        result["ok"] = true;
        result["trajectory_duration_s"] =
            trajectory.waypoints.empty() ? 0.0 : trajectory.waypoints.back().time_from_start_s;
        result["trajectory_waypoint_count"] = trajectory.waypoints.size();
        result["final_state"] = robot_state_to_python(final_state);
        result["final_tcp_pose"] = pose_to_python(final_tcp_pose);
        return result;
      }, py::arg("request"), py::arg("start_state") = py::none())
      .def("plan_motion", [](NativeKernelBundle& self,
                              py::dict request,
                              py::object start_state_obj) -> py::dict {
        ExecuteRequestDTO req;
        req.robot_id = request["robot_id"].cast<std::string>();
        req.group_id = request["group_id"].cast<std::string>();
        req.move_id = request["move_id"].cast<std::string>();
        req.base_frame = request["base_frame"].cast<std::string>();
        req.tcp_frame = request["tcp_frame"].cast<std::string>();

        req.tolerances.joint_position_tol_rad = request.attr("get")("joint_position_tol_rad", 0.05).cast<double>();
        req.tolerances.translation_tol_m = request.attr("get")("translation_tol_m", 0.005).cast<double>();
        req.tolerances.rotation_tol_rad = request.attr("get")("rotation_tol_rad", 0.05).cast<double>();

        req.speed_profile.velocity_scale = finite_scale_from_python(
            request.attr("get")("velocity_scale", 1.0),
            "velocity_scale");
        req.speed_profile.acceleration_scale = finite_scale_from_python(
            request.attr("get")("acceleration_scale", 1.0),
            "acceleration_scale");
        if (request.contains("max_tcp_linear_speed_mps")) {
          auto val = request["max_tcp_linear_speed_mps"];
          if (!val.is_none()) {
            double tcp_speed = val.cast<double>();
            if (tcp_speed > 0.0) {
              req.speed_profile.max_tcp_linear_speed_mps = tcp_speed;
            }
          }
        }

        req.timeouts.planning_timeout_s = request.attr("get")("planning_timeout_s", 5.0).cast<double>();
        req.timeouts.execution_timeout_s = request.attr("get")("execution_timeout_s", 10.0).cast<double>();

        std::string cache_mode_str = request.attr("get")("cache_mode", "default").cast<std::string>();
        req.cache_mode = (cache_mode_str == "disable") ? CacheMode::kDisable : CacheMode::kDefault;

        std::string motion_type = request["motion_type"].cast<std::string>();
        if (motion_type == "joint_goal") {
          JointGoalDTO goal;
          goal.joint_names = request["joint_goal_names"].cast<std::vector<std::string>>();
          goal.positions_rad = request["joint_goal_positions_rad"].cast<std::vector<double>>();
          req.motion = goal;
        } else if (motion_type == "pose_goal") {
          PoseGoalDTO goal;
          auto pose_dict = request["pose_goal"];
          auto pos = pose_dict["position_m"].cast<std::vector<double>>();
          auto orient = pose_dict["orientation_xyzw"].cast<std::vector<double>>();
          goal.goal.position_m = {pos[0], pos[1], pos[2]};
          goal.goal.orientation = {orient[0], orient[1], orient[2], orient[3]};
          req.motion = goal;
        } else if (motion_type == "relative_move") {
          RelativeMoveDTO goal;
          auto rel_dict = request["relative_move"];
          auto parse_reference_dict = [](const py::dict& ref_dict,
                                         const std::string& field_name) {
            ReferenceDTO reference;
            const std::string ref_type = ref_dict["type"].cast<std::string>();
            if (ref_type == "frame") {
              reference.type = ReferenceDTO::Type::kFrame;
            } else if (ref_type == "object") {
              reference.type = ReferenceDTO::Type::kObject;
            } else if (ref_type == "robot_link") {
              reference.type = ReferenceDTO::Type::kRobotLink;
            } else if (ref_type == "pose") {
              reference.type = ReferenceDTO::Type::kPose;
            } else {
              throw std::runtime_error("Unsupported reference type: " + ref_type);
            }
            if (ref_dict.contains("id")) {
              reference.id = ref_dict["id"].cast<std::string>();
            }
            if (reference.type == ReferenceDTO::Type::kPose) {
              if (!ref_dict.contains("pose")) {
                throw std::runtime_error(
                    "Pose reference requires " + field_name + ".pose");
              }
              auto pose_dict = ref_dict["pose"];
              auto pos = pose_dict["position_m"].cast<std::vector<double>>();
              auto orient = pose_dict["orientation_xyzw"].cast<std::vector<double>>();
              reference.pose = Pose3DTO{};
              reference.pose->position_m = {pos[0], pos[1], pos[2]};
              reference.pose->orientation = {orient[0], orient[1], orient[2], orient[3]};
            }
            return reference;
          };

          goal.target_reference = parse_reference_dict(
              rel_dict["target_reference"].cast<py::dict>(), "target_reference");
          if (rel_dict.contains("offset_reference")) {
            goal.offset_reference = parse_reference_dict(
                rel_dict["offset_reference"].cast<py::dict>(), "offset_reference");
          }

          auto offset_dict = rel_dict["offset"];
          auto pos = offset_dict["position_m"].cast<std::vector<double>>();
          auto orient = offset_dict["orientation_xyzw"].cast<std::vector<double>>();
          goal.offset.position_m = {pos[0], pos[1], pos[2]};
          goal.offset.orientation = {orient[0], orient[1], orient[2], orient[3]};
          req.motion = goal;
        }

        RobotStateDTO start_state;
        if (start_state_obj.is_none()) {
          start_state = [&]() {
            py::gil_scoped_release release;
            return self.context->get_current_state();
          }();
        } else {
          py::dict start_dict = start_state_obj.cast<py::dict>();
          start_state.joint_names =
              start_dict["joint_names"].cast<std::vector<std::string>>();
          start_state.positions_rad =
              start_dict["positions_rad"].cast<std::vector<double>>();
          if (start_dict.contains("velocities_rad_s")) {
            start_state.velocities_rad_s =
                start_dict["velocities_rad_s"].cast<std::vector<double>>();
          }
        }

        PlanRequestDTO plan_request;
        plan_request.robot_id = req.robot_id;
        plan_request.group_id = req.group_id;
        plan_request.move_id = req.move_id;
        plan_request.base_frame = req.base_frame;
        plan_request.tcp_frame = req.tcp_frame;
        plan_request.start_state = start_state;
        MotionSpecDTO resolved_motion;
        if (auto error = resolve_motion_spec_for_direct_plan(
                self, req, resolved_motion)) {
          py::dict result;
          result["ok"] = false;
          result["error"] = error->message;
          result["error_details"] = error_details_to_python(error->details);
          return result;
        }
        plan_request.motion = std::move(resolved_motion);
        plan_request.tolerances = req.tolerances;
        plan_request.speed_profile = req.speed_profile;
        plan_request.planning_timeout_s = req.timeouts.planning_timeout_s;

        auto plan_result = [&]() {
          py::gil_scoped_release release;
          return self.planner->plan(plan_request);
        }();
        py::dict result;
        if (!plan_result.trajectory.has_value()) {
          result["ok"] = false;
          if (plan_result.error.has_value()) {
            result["error"] = plan_result.error->message;
            result["error_details"] =
                error_details_to_python(plan_result.error->details);
          } else {
            result["error"] = "Planning failed";
          }
          return result;
        }

        ValidationOptionsDTO validation_options;
        if (req.speed_profile.max_tcp_linear_speed_mps.has_value()) {
          validation_options.max_tcp_linear_speed_mps =
              *req.speed_profile.max_tcp_linear_speed_mps;
          validation_options.tcp_frame = req.tcp_frame;
          validation_options.base_frame = req.base_frame;
        }
        auto validation_result = [&]() {
          py::gil_scoped_release release;
          return self.context->validate_trajectory(
              *plan_result.trajectory, validation_options);
        }();
        if (!validation_result.valid) {
          result["ok"] = false;
          if (validation_result.error.has_value()) {
            result["error"] = validation_result.error->message;
            result["error_details"] =
                error_details_to_python(validation_result.error->details);
          } else {
            result["error"] = "Trajectory validation failed";
          }
          return result;
        }

        const auto& trajectory = *plan_result.trajectory;
        RobotStateDTO final_state;
        final_state.joint_names = trajectory.joint_names;
        if (!trajectory.waypoints.empty()) {
          const auto& final_wp = trajectory.waypoints.back();
          final_state.positions_rad = final_wp.positions_rad;
          final_state.velocities_rad_s = final_wp.velocities_rad_s;
        } else {
          final_state = start_state;
        }

        FkRequestDTO fk_request;
        fk_request.robot_state = final_state;
        fk_request.target_frame = req.tcp_frame;
        fk_request.base_frame = req.base_frame;
        auto final_tcp_pose = [&]() {
          py::gil_scoped_release release;
          return self.context->forward_kinematics(fk_request);
        }();

        result["ok"] = true;
        result["trajectory_duration_s"] =
            trajectory.waypoints.empty() ? 0.0 : trajectory.waypoints.back().time_from_start_s;
        result["trajectory_waypoint_count"] = trajectory.waypoints.size();
        result["final_state"] = robot_state_to_python(final_state);
        result["final_tcp_pose"] = pose_to_python(final_tcp_pose);
        result["trajectory"] = trajectory_to_python(trajectory);
        return result;
      }, py::arg("request"), py::arg("start_state") = py::none(),
      "Plan and validate a motion, returning the time-parameterized trajectory.")
      .def("stop", [](NativeKernelBundle& self) -> py::dict {
        auto r = [&]() {
          py::gil_scoped_release release;
          return self.kernel->stop();
        }();
        return command_result(r.accepted, r.error);
      })
      .def("pause", [](NativeKernelBundle& self) -> py::dict {
        auto r = [&]() {
          py::gil_scoped_release release;
          return self.kernel->pause();
        }();
        return command_result(r.accepted, r.error);
      })
      .def("resume", [](NativeKernelBundle& self) -> py::dict {
        auto r = [&]() {
          py::gil_scoped_release release;
          return self.kernel->resume();
        }();
        return command_result(r.accepted, r.error);
      })
      .def("reset", [](NativeKernelBundle& self) -> py::dict {
        auto r = [&]() {
          py::gil_scoped_release release;
          return self.kernel->reset();
        }();
#ifdef MANYFORGE_ROS2_CONTROL_ENABLED
        // The kernel rejects reset while already idle, but ROS-side auxiliary
        // channels such as JointState gripper commands can still hold adapter
        // state when used directly from the native bundle. Keep the low-level
        // binding idempotent so reset reliably clears the deployment adapter.
        if (!r.accepted) {
          try {
            auto backend_result = [&]() {
              py::gil_scoped_release release;
              return require_ros2_control_execution(self).reset();
            }();
            if (backend_result.accepted) {
              return command_result(backend_result.accepted,
                                    backend_result.error);
            }
          } catch (const std::runtime_error&) {
            // Non-ROS bundles keep the kernel result below.
          }
        }
#endif
        return command_result(r.accepted, r.error);
      })
      .def("acknowledge_completed", [](NativeKernelBundle& self,
                                        uint64_t execution_id) -> py::dict {
        auto r = self.kernel->acknowledge_completed(execution_id);
        return command_result(r.accepted, r.error);
      })
      .def("get_status", [](NativeKernelBundle& self) -> py::dict {
        return status_to_python(self.kernel->get_status());
      })
      .def("get_execution_snapshot", [](NativeKernelBundle& self) -> py::dict {
        return snapshot_to_python(self.kernel->get_execution_snapshot());
      })
      .def("get_metrics", [](NativeKernelBundle& self) -> py::dict {
        return metrics_to_python(self.kernel->get_metrics());
      })
#ifdef MANYFORGE_ROS2_CONTROL_ENABLED
      .def("configure_ros_gripper_action",
          [](NativeKernelBundle& self, const std::string& gripper_id,
             py::dict config_dict) {
            Ros2GripperActionConfig config;
            if (config_dict.contains("action_name")) {
              config.action_name =
                  config_dict["action_name"].cast<std::string>();
            }
            if (config_dict.contains("wait_for_action_server_timeout_s")) {
              config.wait_for_action_server_timeout_s =
                  finite_scale_from_python(
                      config_dict["wait_for_action_server_timeout_s"],
                      "wait_for_action_server_timeout_s");
            }
            if (config_dict.contains("result_timeout_s")) {
              config.result_timeout_s =
                  finite_scale_from_python(config_dict["result_timeout_s"],
                                           "result_timeout_s");
            }
            require_ros2_control_execution(self).configure_gripper_action(
                gripper_id, std::move(config));
          },
          py::arg("gripper_id"), py::arg("config"))
      .def("command_ros_gripper",
          [](NativeKernelBundle& self, const std::string& gripper_id,
             double target_position, double duration_s,
             double max_effort) -> py::dict {
            const auto result = require_ros2_control_execution(self).command_gripper(
                gripper_id, target_position, duration_s, max_effort);
            py::dict d;
            d["accepted"] = result.accepted;
            if (!result.error.empty()) {
              d["error"] = result.error;
            }
            return d;
          },
          py::arg("gripper_id"), py::arg("target_position"),
          py::arg("duration_s"), py::arg("max_effort") = 0.0)
      .def("update_ros_grippers", [](NativeKernelBundle& self) {
        require_ros2_control_execution(self).update_gripper_commands();
      })
      .def("get_ros_gripper_status",
          [](NativeKernelBundle& self, const std::string& gripper_id) -> py::dict {
            const auto status =
                require_ros2_control_execution(self).get_gripper_status(gripper_id);
            py::dict d;
            d["state"] = status.state;
            d["progress"] = status.progress;
            if (status.current_position.has_value()) {
              d["current_position"] = *status.current_position;
            } else {
              d["current_position"] = py::none();
            }
            if (status.target_position.has_value()) {
              d["target_position"] = *status.target_position;
            } else {
              d["target_position"] = py::none();
            }
            if (!status.error.empty()) {
              d["error"] = status.error;
            }
            return d;
          },
          py::arg("gripper_id"))
#endif
      .def("get_scene_object_ids", [](NativeKernelBundle& self) {
        return self.context->get_scene_object_ids();
      })
      .def("get_joint_names", [](NativeKernelBundle& self) {
        return self.context->get_joint_names();
      })
      .def("get_scene_objects", [](NativeKernelBundle& self) -> py::list {
        py::list result;
        for (const auto& object : self.context->get_scene_objects()) {
          result.append(scene_object_snapshot_to_python(object));
        }
        return result;
      })
      .def("invalidate_trajectories", [](NativeKernelBundle& self) {
        self.cache->invalidate_all();
      })
      .def("set_robot_base_pose", [](NativeKernelBundle& self, py::dict pose) {
        self.context->set_robot_base_pose(pose_from_python(pose));
        self.cache->invalidate_all();
      }, py::arg("pose"))
      .def("scene_edit", [](NativeKernelBundle& self, py::dict request) -> py::dict {
        py::dict result;
        std::string op = request["operation"].cast<std::string>();

        if (op == "add" || op == "upsert") {
          AddObjectRequestDTO req;
          req.object_id = request["object_id"].cast<std::string>();
          req.frame_id = request.attr("get")("frame_id", "universe").cast<std::string>();
          req.support_contacts = support_contacts_from_python(request);

          // Pose
          if (request.contains("pose_position_m")) {
            auto pos = request["pose_position_m"].cast<std::vector<double>>();
            req.pose.position_m = {pos[0], pos[1], pos[2]};
          }
          if (request.contains("pose_orientation_xyzw")) {
            auto orient = request["pose_orientation_xyzw"].cast<std::vector<double>>();
            req.pose.orientation = {orient[0], orient[1], orient[2], orient[3]};
          } else {
            req.pose.orientation = {0, 0, 0, 1};
          }

          // Shape
          std::string shape_type = request.attr("get")("shape_type", "box").cast<std::string>();
          if (shape_type == "box") {
            if (!request.contains("box_dimensions_m")) {
              result["success"] = false;
              result["error"] = "add with shape_type=box requires box_dimensions_m";
              return result;
            }
            auto dims = request["box_dimensions_m"].cast<std::vector<double>>();
            if (dims.size() != 3) {
              result["success"] = false;
              result["error"] = "box_dimensions_m must have exactly 3 elements";
              return result;
            }
            req.shape = BoxShapeDTO{{dims[0], dims[1], dims[2]}};
          } else if (shape_type == "sphere") {
            if (!request.contains("sphere_radius_m")) {
              result["success"] = false;
              result["error"] = "add with shape_type=sphere requires sphere_radius_m";
              return result;
            }
            double radius = request["sphere_radius_m"].cast<double>();
            req.shape = SphereShapeDTO{radius};
          } else if (shape_type == "mesh") {
            if (!request.contains("mesh_resource_uri")) {
              result["success"] = false;
              result["error"] = "add with shape_type=mesh requires mesh_resource_uri";
              return result;
            }
            MeshShapeDTO mesh;
            mesh.resource_uri = request["mesh_resource_uri"].cast<std::string>();
            mesh.scale = {1.0, 1.0, 1.0};
            if (request.contains("mesh_scale")) {
              auto scale = request["mesh_scale"].cast<std::vector<double>>();
              if (scale.size() != 3) {
                result["success"] = false;
                result["error"] = "mesh_scale must have exactly 3 elements";
                return result;
              }
              mesh.scale = {scale[0], scale[1], scale[2]};
            }
            req.shape = mesh;
          } else {
            result["success"] = false;
            result["error"] = "Unsupported shape_type for native backend: " + shape_type;
            return result;
          }

          if (op == "upsert") {
            auto state = [&]() {
              py::gil_scoped_release release;
              return self.context->get_object_state(req.object_id);
            }();
            if (state.exists && state.attached) {
              result["success"] = false;
              result["error"] = "Cannot upsert attached object";
              return result;
            }
            if (state.exists) {
              RemoveObjectRequestDTO remove_req;
              remove_req.object_id = req.object_id;
              auto remove_result = [&]() {
                py::gil_scoped_release release;
                return self.context->remove_object(remove_req);
              }();
              if (!remove_result.success) {
                result["success"] = false;
                if (remove_result.error.has_value()) {
                  result["error"] = remove_result.error->message;
                } else {
                  result["error"] = "Failed to remove existing object before upsert";
                }
                return result;
              }
            }
          }

          auto r = [&]() {
            py::gil_scoped_release release;
            return self.context->add_object(req);
          }();
          result["success"] = r.success;
          if (r.error.has_value()) result["error"] = r.error->message;

        } else if (op == "remove") {
          RemoveObjectRequestDTO req;
          req.object_id = request["object_id"].cast<std::string>();
          auto r = [&]() {
            py::gil_scoped_release release;
            return self.context->remove_object(req);
          }();
          result["success"] = r.success;
          if (r.error.has_value()) result["error"] = r.error->message;

        } else if (op == "update_pose") {
          UpdateObjectPoseRequestDTO req;
          req.object_id = request["object_id"].cast<std::string>();
          req.frame_id = request.attr("get")("frame_id", "universe").cast<std::string>();
          if (request.contains("pose_position_m")) {
            auto pos = request["pose_position_m"].cast<std::vector<double>>();
            req.pose.position_m = {pos[0], pos[1], pos[2]};
          }
          if (request.contains("pose_orientation_xyzw")) {
            auto orient = request["pose_orientation_xyzw"].cast<std::vector<double>>();
            req.pose.orientation = {orient[0], orient[1], orient[2], orient[3]};
          } else {
            req.pose.orientation = {0, 0, 0, 1};
          }
          auto r = [&]() {
            py::gil_scoped_release release;
            return self.context->update_object_pose(req);
          }();
          result["success"] = r.success;
          if (r.error.has_value()) result["error"] = r.error->message;
        } else if (op == "attach") {
          AttachObjectRequestDTO req;
          req.object_id = request["object_id"].cast<std::string>();
          req.frame_id = request.attr("get")("frame_id", "").cast<std::string>();
          if (request.contains("touch_links") &&
              !request["touch_links"].is_none()) {
            req.touch_links = request["touch_links"].cast<std::vector<std::string>>();
          }
          auto r = [&]() {
            py::gil_scoped_release release;
            return self.context->attach_object(req);
          }();
          result["success"] = r.success;
          if (r.error.has_value()) result["error"] = r.error->message;
        } else if (op == "detach") {
          DetachObjectRequestDTO req;
          req.object_id = request["object_id"].cast<std::string>();
          req.frame_id = request.attr("get")("frame_id", "universe").cast<std::string>();
          auto r = [&]() {
            py::gil_scoped_release release;
            return self.context->detach_object(req);
          }();
          result["success"] = r.success;
          if (r.error.has_value()) result["error"] = r.error->message;
        } else {
          result["success"] = false;
          result["error"] = "Unsupported scene edit operation: " + op;
        }

        return result;
      })
      .def("set_joint_positions", [](NativeKernelBundle& self,
                                      const std::vector<double>& positions) {
        Eigen::VectorXd q(positions.size());
        for (size_t i = 0; i < positions.size(); ++i) {
          q[i] = positions[i];
        }
        if (self.scene && q.size() != self.scene->getCurrentJointPositions().size()) {
          q = self.scene->toFullJointPositions(self.group_name, q);
        }
        self.scene->setJointPositions(q);
        if (self.kernel) {
          self.kernel->sync_snapshot_from_context();
        }
      }, py::arg("positions"),
      "Set the robot's current joint positions in the planning scene.")
      .def("get_group_joint_names", [](NativeKernelBundle& self,
                                        const std::string& group_name) {
        const auto maybe_group_info = self.scene->getJointGroupInfo(group_name);
        if (!maybe_group_info) {
          throw std::runtime_error(
              "Failed to get joint names for group '" + group_name + "': " +
              maybe_group_info.error());
        }
        return maybe_group_info->joint_names;
      }, py::arg("group_name"),
      "Get ordered joint names for a specific SRDF group.")
      .def("get_group_joint_positions", [](NativeKernelBundle& self,
                                           const std::string& group_name) {
        const auto maybe_group_info = self.scene->getJointGroupInfo(group_name);
        if (!maybe_group_info) {
          throw std::runtime_error(
              "Failed to get joint positions for group '" + group_name + "': " +
              maybe_group_info.error());
        }
        const auto& group_info = *maybe_group_info;
        const auto q = self.scene->getCurrentJointPositions();
        std::vector<double> result;
        result.reserve(group_info.q_indices.size());
        for (int i = 0; i < group_info.q_indices.size(); ++i) {
          result.push_back(q[group_info.q_indices[i]]);
        }
        return result;
      }, py::arg("group_name"),
      "Get current joint positions for a specific SRDF group.")
      .def("set_group_joint_positions", [](NativeKernelBundle& self,
                                           const std::string& group_name,
                                           const std::vector<double>& positions) {
        Eigen::VectorXd q(positions.size());
        for (size_t i = 0; i < positions.size(); ++i) {
          q[i] = positions[i];
        }
        if (self.scene && q.size() != self.scene->getCurrentJointPositions().size()) {
          q = self.scene->toFullJointPositions(group_name, q);
        }
        self.scene->setJointPositions(q);
        if (self.kernel) {
          self.kernel->sync_snapshot_from_context();
        }
      }, py::arg("group_name"), py::arg("positions"),
      "Set joint positions for a specific SRDF group in the planning scene.")
      .def("collision_check_positions", [](NativeKernelBundle& self,
                                            const std::vector<double>& positions) {
        RobotStateDTO state;
        state.joint_names = self.context->get_joint_names();
        state.positions_rad = positions;
        CollisionCheckRequestDTO request;
        request.robot_state = state;
        const auto result = [&]() {
          py::gil_scoped_release release;
          return self.context->collision_check(request);
        }();
        py::dict d;
        d["in_collision"] = result.in_collision;
        if (result.contact_body_a.has_value()) {
          d["contact_body_a"] = *result.contact_body_a;
        } else {
          d["contact_body_a"] = py::none();
        }
        if (result.contact_body_b.has_value()) {
          d["contact_body_b"] = *result.contact_body_b;
        } else {
          d["contact_body_b"] = py::none();
        }
        return d;
      }, py::arg("positions"),
      "Check collisions for active-group joint positions.")
      .def("get_cached_trajectory", [](NativeKernelBundle& self,
                                       const std::string& move_id) -> py::object {
        auto cached = self.cache->lookup(move_id);
        if (!cached.has_value() || cached->waypoints.empty()) {
          return py::none();
        }
        py::dict result;
        result["joint_names"] = cached->joint_names;
        py::list waypoints;
        for (const auto& wp : cached->waypoints) {
          py::dict wp_dict;
          wp_dict["time_from_start_s"] = wp.time_from_start_s;
          wp_dict["positions_rad"] = wp.positions_rad;
          waypoints.append(wp_dict);
        }
        result["waypoints"] = waypoints;
        return result;
      }, py::arg("move_id"), "Get cached trajectory waypoints for a move_id.")
      .def("get_link_pose", [](NativeKernelBundle& self,
                                const std::string& target_frame,
                                const std::string& base_frame) -> py::object {
        try {
          FkRequestDTO req;
          req.target_frame = target_frame;
          req.base_frame = base_frame;

          // Get current state for FK
          req.robot_state = [&]() {
            py::gil_scoped_release release;
            return self.context->get_current_state();
          }();

          auto pose = [&]() {
            py::gil_scoped_release release;
            return self.context->forward_kinematics(req);
          }();

          py::dict result;
          result["position_m"] = py::make_tuple(
              pose.position_m.x_m, pose.position_m.y_m, pose.position_m.z_m);
          result["orientation_xyzw"] = py::make_tuple(
              pose.orientation.x, pose.orientation.y,
              pose.orientation.z, pose.orientation.w);
          return result;
        } catch (...) {
          return py::none();
        }
      })
      .def("get_object_pose", [](NativeKernelBundle& self,
                                  const std::string& object_id,
                                  const std::string& base_frame) -> py::object {
        try {
          auto pose = [&]() {
            py::gil_scoped_release release;
            return self.context->get_object_pose(object_id, base_frame);
          }();
          return pose_to_python(pose);
        } catch (...) {
          return py::none();
        }
      })
      .def("get_object_state", [](NativeKernelBundle& self,
                                   const std::string& object_id) -> py::dict {
        return object_state_to_python(self.context->get_object_state(object_id));
      })
      .def("get_scene_trace", [](NativeKernelBundle& self, int limit) -> py::list {
        py::list result;
        size_t bounded_limit = limit < 0 ? 0 : static_cast<size_t>(limit);
        for (const auto& event : self.context->get_scene_trace(bounded_limit)) {
          result.append(scene_trace_event_to_python(event));
        }
        return result;
      }, py::arg("limit") = -1)
      .def("clear_scene_trace", [](NativeKernelBundle& self) {
        self.context->clear_scene_trace();
      })
      .def("drain_kernel_events", [](NativeKernelBundle& self) -> py::list {
        py::list result;
        for (const auto& event : self.event_collector.drain()) {
          result.append(kernel_event_to_python(event));
        }
        return result;
      });

  // Factory function
  m.def("create_native_kernel",
        [](const std::string& model_name, const std::string& group_name,
           py::object config) {
          return create_native_kernel(
              model_name, group_name, kernel_config_from_python(config));
        },
        py::arg("model_name") = "ur5_gripper",
        py::arg("group_name") = "arm",
        py::arg("config") = py::none(),
        "Create a native C++ kernel with real RoboPlan backends.");

  m.def("create_native_kernel_from_paths",
        [](const std::string& model_name, const std::string& group_name,
           const std::string& urdf_path, const std::string& srdf_path,
           const std::vector<std::string>& package_paths, py::object config) {
          return create_native_kernel(
              model_name, group_name, urdf_path, srdf_path, package_paths,
              kernel_config_from_python(config));
        },
        py::arg("model_name"),
        py::arg("group_name"),
        py::arg("urdf_path"),
        py::arg("srdf_path"),
        py::arg("package_paths") = std::vector<std::string>{},
        py::arg("config") = py::none(),
        "Create a native C++ kernel from explicit URDF/SRDF/package bundle paths.");

#ifdef MANYFORGE_ROS2_CONTROL_ENABLED
  m.def("create_ros2_control_kernel_from_paths",
        [](const std::string& model_name, const std::string& group_name,
           const std::string& urdf_path, const std::string& srdf_path,
           const std::vector<std::string>& package_paths, py::object ros_config,
           py::object kernel_config) {
          return create_ros2_control_kernel(
              model_name, group_name, urdf_path, srdf_path, package_paths,
              ros2_control_config_from_python(ros_config),
              kernel_config_from_python(kernel_config));
        },
        py::arg("model_name"),
        py::arg("group_name"),
        py::arg("urdf_path"),
        py::arg("srdf_path"),
        py::arg("package_paths") = std::vector<std::string>{},
        py::arg("ros_config") = py::none(),
        py::arg("kernel_config") = py::none(),
        "Create a native C++ kernel with RoboPlan planning and ros2_control execution.");
#endif
}
