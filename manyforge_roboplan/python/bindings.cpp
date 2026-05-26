#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>

#include <cstdint>
#include <cmath>
#include <optional>
#include <set>
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
    auto nonnegative_finite = [](double value, const char* field) {
      if (!std::isfinite(value) || value < 0.0) {
        throw std::invalid_argument(std::string(field) + " must be finite and >= 0");
      }
    };
    if (contact_dict.contains("mode")) {
      const auto mode = contact_dict["mode"].cast<std::string>();
      if (mode == "contact_release") {
        contact.mode = CollisionContactPolicyMode::kContactRelease;
      } else if (mode == "contact_process") {
        contact.mode = CollisionContactPolicyMode::kContactProcess;
      } else {
        throw std::invalid_argument(
            "support_contacts mode must be contact_release or contact_process");
      }
    }
    if (contact_dict.contains("max_start_penetration_m")) {
      contact.max_start_penetration_m =
          contact_dict["max_start_penetration_m"].cast<double>();
      nonnegative_finite(contact.max_start_penetration_m,
                         "max_start_penetration_m");
    }
    if (contact_dict.contains("release_tolerance_m")) {
      contact.release_tolerance_m =
          contact_dict["release_tolerance_m"].cast<double>();
      nonnegative_finite(contact.release_tolerance_m, "release_tolerance_m");
    }
    if (contact_dict.contains("min_release_progress_m")) {
      contact.min_release_progress_m =
          contact_dict["min_release_progress_m"].cast<double>();
      nonnegative_finite(contact.min_release_progress_m,
                         "min_release_progress_m");
    }
    if (contact_dict.contains("min_separation_clearance_m")) {
      contact.min_separation_clearance_m =
          contact_dict["min_separation_clearance_m"].cast<double>();
      nonnegative_finite(contact.min_separation_clearance_m,
                         "min_separation_clearance_m");
    }
    if (contact_dict.contains("require_separation_by_end")) {
      contact.require_separation_by_end =
          contact_dict["require_separation_by_end"].cast<bool>();
    }
    if (contact_dict.contains("require_strictly_decreasing_penetration")) {
      contact.require_strictly_decreasing_penetration =
          contact_dict["require_strictly_decreasing_penetration"].cast<bool>();
    }
    if (contact_dict.contains("allow_recontact")) {
      contact.allow_recontact =
          contact_dict["allow_recontact"].cast<bool>();
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

RobotStateDTO observer_robot_state(NativeKernelBundle& self) {
  if (self.kernel) {
    const auto snapshot = self.kernel->get_execution_snapshot();
    if (!snapshot.joint_positions_rad.empty()) {
      RobotStateDTO state;
      state.positions_rad = snapshot.joint_positions_rad;
      return state;
    }
  }
  return self.context->get_current_state();
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
    case KernelEventType::kInterventionAcknowledged: return "acknowledge_intervention";
    case KernelEventType::kStreamingSessionStarted: return "streaming_session_started";
    case KernelEventType::kStreamingTargetReceived: return "streaming_target_received";
    case KernelEventType::kStreamingTargetDispatched: return "streaming_target_dispatched";
    case KernelEventType::kStreamingTargetRejected: return "streaming_target_rejected";
    case KernelEventType::kStreamingSessionEnded: return "streaming_session_ended";
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
    d["error_code"] = error->code;
    d["error_details"] = error_details_to_python(error->details);
  } else {
    d["error_code"] = py::none();
    d["error_details"] = py::dict();
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
  auto read_seconds = [&](const char* s_key, const char* ms_key,
                          double current) {
    if (config_dict.contains(s_key)) {
      return finite_scale_from_python(config_dict[s_key], s_key);
    }
    if (config_dict.contains(ms_key)) {
      return finite_scale_from_python(config_dict[ms_key], ms_key) / 1000.0;
    }
    return current;
  };
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
  if (config_dict.contains("runtime_collision_query_timeout_ns")) {
    const double timeout_ns =
        finite_scale_from_python(config_dict["runtime_collision_query_timeout_ns"],
                                 "runtime_collision_query_timeout_ns");
    if (timeout_ns <= 0.0) {
      throw std::invalid_argument(
          "runtime_collision_query_timeout_ns must be > 0");
    }
    config.runtime_collision_query_timeout_ns =
        static_cast<int64_t>(timeout_ns);
  }
  if (config_dict.contains("runtime_dynamic_body_max_age_cap_ns")) {
    const double cap_ns = finite_scale_from_python(
        config_dict["runtime_dynamic_body_max_age_cap_ns"],
        "runtime_dynamic_body_max_age_cap_ns");
    // Zero is a sentinel: disable the kernel-side cap while keeping
    // body-local freshness checks active. Negative values are invalid.
    if (cap_ns < 0.0) {
      throw std::invalid_argument(
          "runtime_dynamic_body_max_age_cap_ns must be >= 0");
    }
    config.runtime_dynamic_body_max_age_cap_ns =
        static_cast<int64_t>(cap_ns);
  }
  config.streaming_ik_timeout_s =
      read_seconds("streaming_ik_timeout_s", "streaming_ik_timeout_ms",
                   config.streaming_ik_timeout_s);
  config.native_reset_completion_timeout_s =
      read_seconds("native_reset_completion_timeout_s",
                   "native_reset_completion_timeout_ms",
                   config.native_reset_completion_timeout_s);
  config.native_streaming_stop_completion_timeout_s =
      read_seconds("native_streaming_stop_completion_timeout_s",
                   "native_streaming_stop_completion_timeout_ms",
                   config.native_streaming_stop_completion_timeout_s);
  if (config.streaming_ik_timeout_s <= 0.0) {
    throw std::invalid_argument("streaming_ik_timeout_s must be > 0");
  }
  if (config.native_reset_completion_timeout_s <= 0.0) {
    throw std::invalid_argument(
        "native_reset_completion_timeout_s must be > 0");
  }
  if (config.native_streaming_stop_completion_timeout_s <= 0.0) {
    throw std::invalid_argument(
        "native_streaming_stop_completion_timeout_s must be > 0");
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
  if (config_dict.contains("speed_scaling_topic")) {
    config.speed_scaling_topic =
        config_dict["speed_scaling_topic"].cast<std::string>();
  }
  if (config_dict.contains("enable_jtc_speed_scaling_input")) {
    config.enable_jtc_speed_scaling_input =
        config_dict["enable_jtc_speed_scaling_input"].cast<bool>();
  }
  if (config_dict.contains("decelerate_on_cancel")) {
    config.decelerate_on_cancel =
        config_dict["decelerate_on_cancel"].cast<bool>();
  }
  if (config_dict.contains("velocity_state_interface_joints")) {
    const auto joints =
        config_dict["velocity_state_interface_joints"].cast<std::vector<std::string>>();
    config.velocity_state_interface_joints =
        std::set<std::string>(joints.begin(), joints.end());
  }
  if (config_dict.contains("max_deceleration_on_cancel_rad_s2")) {
    config.max_deceleration_on_cancel_rad_s2 =
        config_dict["max_deceleration_on_cancel_rad_s2"]
            .cast<std::map<std::string, double>>();
  }
  if (config_dict.contains("enable_soft_on_path_path_ramp")) {
    config.enable_soft_on_path_path_ramp =
        config_dict["enable_soft_on_path_path_ramp"].cast<bool>();
  }
  config.soft_on_path_stop_traj_delta_s =
      read_seconds("soft_on_path_stop_traj_delta_s",
                   "soft_on_path_stop_traj_delta_ms",
                   config.soft_on_path_stop_traj_delta_s);
  config.soft_on_path_sample_period_s =
      read_seconds("soft_on_path_sample_period_s",
                   "soft_on_path_sample_period_ms",
                   config.soft_on_path_sample_period_s);
  if (config_dict.contains("soft_on_path_max_initial_path_deviation_rad")) {
    auto val = config_dict["soft_on_path_max_initial_path_deviation_rad"];
    if (!val.is_none()) {
      config.soft_on_path_max_initial_path_deviation_rad =
          finite_scale_from_python(val,
                                   "soft_on_path_max_initial_path_deviation_rad");
    }
  }
  if (config_dict.contains(
          "soft_on_path_max_initial_velocity_deviation_rad_s")) {
    auto val =
        config_dict["soft_on_path_max_initial_velocity_deviation_rad_s"];
    if (!val.is_none()) {
      config.soft_on_path_max_initial_velocity_deviation_rad_s =
          finite_scale_from_python(
              val,
              "soft_on_path_max_initial_velocity_deviation_rad_s");
    }
  }
  if (config_dict.contains("soft_on_path_max_jerk_rad_s3")) {
    config.soft_on_path_max_jerk_rad_s3 =
        config_dict["soft_on_path_max_jerk_rad_s3"]
            .cast<std::map<std::string, double>>();
  }
  if (config_dict.contains("soft_on_path_per_joint_max_jerk_rad_s3")) {
    config.soft_on_path_max_jerk_rad_s3 =
        config_dict["soft_on_path_per_joint_max_jerk_rad_s3"]
            .cast<std::map<std::string, double>>();
  }
  if (config_dict.contains("allow_soft_on_path_model_velocity_fallback")) {
    config.allow_soft_on_path_model_velocity_fallback =
        config_dict["allow_soft_on_path_model_velocity_fallback"].cast<bool>();
  }
  if (config_dict.contains("enable_soft_on_path_experimental")) {
    config.enable_soft_on_path_experimental =
        config_dict["enable_soft_on_path_experimental"].cast<bool>();
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

        // Phase 4.5E: motion-scoped interrupt scopes
        // (spec/210 §3.10 + spec/260 §2.6). Parse each scope's
        // stop_profile + node_category so MoveUntilSignal /
        // ProbeSearchMove can drive the kernel arm path. Without
        // this parsing the scopes would be silently dropped at the
        // Python/native boundary and the move would run unscoped.
        if (request.contains("motion_interrupt_scopes")) {
          auto scopes_obj = request["motion_interrupt_scopes"];
          if (!scopes_obj.is_none()) {
            auto scopes_list = scopes_obj.cast<py::list>();
            for (auto handle : scopes_list) {
              auto sd = handle.cast<py::dict>();
              MotionInterruptScopeDTO scope;
              scope.scope_id = sd["scope_id"].cast<std::string>();
              scope.policy_id = sd["policy_id"].cast<std::string>();
              const std::string activation =
                  sd.attr("get")("activation_mode",
                                  std::string("require_inactive_at_arm"))
                      .cast<std::string>();
              // Phase 4.5E (codex review follow-up): a typoed or
              // stale client value MUST fail before dispatch. Silently
              // coercing "trip_if_already_inactive" → require_inactive
              // would dispatch a different scope than the operator
              // configured.
              if (activation == "require_inactive_at_arm") {
                scope.activation_mode =
                    MotionInterruptActivationMode::kRequireInactiveAtArm;
              } else if (activation == "fresh_edge_after_arm") {
                scope.activation_mode =
                    MotionInterruptActivationMode::kFreshEdgeAfterArm;
              } else if (activation == "trip_if_already_active") {
                scope.activation_mode =
                    MotionInterruptActivationMode::kTripIfAlreadyActive;
              } else if (activation == "maintain_level_with_hysteresis") {
                scope.activation_mode =
                    MotionInterruptActivationMode::kMaintainLevelWithHysteresis;
              } else {
                throw std::invalid_argument(
                    std::string("motion_interrupt_scopes[].activation_mode: "
                                "unknown value '") +
                    activation +
                    "' (expected one of require_inactive_at_arm, "
                    "fresh_edge_after_arm, trip_if_already_active, "
                    "maintain_level_with_hysteresis)");
              }
              if (sd.contains("arm_timeout_ns") &&
                  !sd["arm_timeout_ns"].is_none()) {
                scope.arm_timeout_ns =
                    sd["arm_timeout_ns"].cast<std::int64_t>();
              }
              if (sd.contains("result_binding_key") &&
                  !sd["result_binding_key"].is_none()) {
                scope.result_binding_key =
                    sd["result_binding_key"].cast<std::string>();
              }
              scope.required = sd.attr("get")("required", true).cast<bool>();
              const std::string node_category =
                  sd.attr("get")("node_category", std::string("generic"))
                      .cast<std::string>();
              if (node_category == "generic") {
                scope.node_category = MotionInterruptNodeCategory::kGeneric;
              } else if (node_category == "precision_contact") {
                scope.node_category =
                    MotionInterruptNodeCategory::kPrecisionContact;
              } else {
                throw std::invalid_argument(
                    std::string("motion_interrupt_scopes[].node_category: "
                                "unknown value '") +
                    node_category +
                    "' (expected 'generic' or 'precision_contact')");
              }
              if (sd.contains("stop_profile") &&
                  !sd["stop_profile"].is_none()) {
                auto sp = sd["stop_profile"].cast<py::dict>();
                MotionInterruptStopProfileDTO profile;
                profile.profile_id = sp["profile_id"].cast<std::string>();
                const std::string sm = sp["stop_mode"].cast<std::string>();
                if (sm == "spring_back_hold") {
                  profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
                } else if (sm == "soft_on_path") {
                  profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
                } else if (sm == "hard_deceleration") {
                  profile.stop_mode = MotionInterruptStopMode::kHardDeceleration;
                } else {
                  throw std::invalid_argument(
                      std::string("motion_interrupt_scopes[].stop_profile."
                                  "stop_mode: unknown value '") +
                      sm +
                      "' (expected 'spring_back_hold', 'soft_on_path', or "
                      "'hard_deceleration')");
                }
                if (sp.contains("deceleration_time_s") &&
                    !sp["deceleration_time_s"].is_none()) {
                  profile.deceleration_time_s =
                      sp["deceleration_time_s"].cast<double>();
                }
                if (sp.contains("max_stop_time_s") &&
                    !sp["max_stop_time_s"].is_none()) {
                  profile.max_stop_time_s =
                      sp["max_stop_time_s"].cast<double>();
                }
                if (sp.contains("joint_deceleration_profile_id") &&
                    !sp["joint_deceleration_profile_id"].is_none()) {
                  profile.joint_deceleration_profile_id =
                      sp["joint_deceleration_profile_id"].cast<std::string>();
                }
                if (sp.contains("per_joint_max_deceleration_rad_s2") &&
                    !sp["per_joint_max_deceleration_rad_s2"].is_none()) {
                  profile.per_joint_max_deceleration_rad_s2 =
                      sp["per_joint_max_deceleration_rad_s2"]
                          .cast<std::map<std::string, double>>();
                }
                if (sp.contains("allowed_fallback_modes") &&
                    !sp["allowed_fallback_modes"].is_none()) {
                  for (auto m :
                       sp["allowed_fallback_modes"].cast<py::list>()) {
                    const std::string ms = m.cast<std::string>();
                    if (ms == "spring_back_hold") {
                      profile.allowed_fallback_modes.push_back(
                          MotionInterruptStopMode::kSpringBackHold);
                    } else if (ms == "soft_on_path") {
                      profile.allowed_fallback_modes.push_back(
                          MotionInterruptStopMode::kSoftOnPath);
                    } else if (ms == "hard_deceleration") {
                      profile.allowed_fallback_modes.push_back(
                          MotionInterruptStopMode::kHardDeceleration);
                    } else {
                      throw std::invalid_argument(
                          std::string("motion_interrupt_scopes[].stop_profile."
                                      "allowed_fallback_modes: unknown value '") +
                          ms +
                          "' (expected 'spring_back_hold', 'soft_on_path', "
                          "or 'hard_deceleration')");
                    }
                  }
                }
                profile.allow_natural_completion_on_short_path =
                    sp.attr("get")(
                          "allow_natural_completion_on_short_path", false)
                        .cast<bool>();
                profile.require_backend_confirmation =
                    sp.attr("get")("require_backend_confirmation", true)
                        .cast<bool>();
                scope.stop_profile = profile;
              }
              req.motion_interrupt_scopes.push_back(std::move(scope));
            }
          }
        }

        auto result = [&]() {
          py::gil_scoped_release release;
          return self.kernel->execute(req);
        }();
        return result_to_python(result);
      })
      .def("get_execution_outcome",
           [](NativeKernelBundle& self, std::uint64_t execution_id)
               -> py::object {
             // Phase 4.5E: expose the kernel's terminal motion outcome
             // store so the Python BT nodes can map terminal_cause to
             // py_trees Status. Returns None when no outcome is
             // recorded (e.g., the execution id was never seen or
             // FIFO-evicted).
             auto outcome_opt = [&]() {
               py::gil_scoped_release release;
               return self.kernel->get_execution_outcome(execution_id);
             }();
             if (!outcome_opt) {
               return py::none();
             }
             const auto& outcome = *outcome_opt;
             py::dict d;
             d["execution_id"] = outcome.execution_id;
             d["move_id"] = outcome.move_id;
             const char* cause = "completed_normally";
             switch (outcome.terminal_cause) {
               using enum MotionExecutionTerminalCause;
               case kCompletedNormally: cause = "completed_normally"; break;
               case kSignalTrip:        cause = "signal_trip"; break;
               case kNoSignalTrip:      cause = "no_signal_trip"; break;
               case kStopped:           cause = "stopped"; break;
               case kFailed:            cause = "failed"; break;
               case kInterruptedFault:  cause = "interrupted_fault"; break;
             }
             d["terminal_cause"] = std::string(cause);
             if (outcome.trip) {
               const auto& trip = *outcome.trip;
               py::dict td;
               td["latch_id"] = trip.latch_id;
               td["source_id"] = trip.source_id;
               td["policy_id"] = trip.policy_id;
               td["scope_id"] = trip.scope_id;
               const char* esm = "spring_back_hold";
               switch (trip.executed_stop_mode) {
                 using enum MotionInterruptStopMode;
                 case kSpringBackHold:    esm = "spring_back_hold"; break;
                 case kSoftOnPath:        esm = "soft_on_path"; break;
                 case kHardDeceleration:  esm = "hard_deceleration"; break;
               }
               td["executed_stop_mode"] = std::string(esm);
               const char* scs = "diagnostic_other";
               switch (trip.stop_command_source) {
                 using enum MotionInterruptStopCommandSource;
                 case kFjtPreemptHold:           scs = "fjt_preempt_hold"; break;
                 case kFjtPreemptPathRamp:       scs = "fjt_preempt_path_ramp"; break;
                 case kJtcSpeedScalingRamp:      scs = "jtc_speed_scaling_ramp"; break;
                 case kJtcCancelDecelerateOnCancel:
                   scs = "jtc_cancel_decelerate_on_cancel"; break;
                 case kNativeSimulatedStop:      scs = "native_simulated_stop"; break;
                 case kNativeSimulatedPathRamp:
                   scs = "native_simulated_path_ramp"; break;
                 case kRobotNativeStop:          scs = "robot_native_stop"; break;
                 case kDiagnosticOther:          scs = "diagnostic_other"; break;
               }
               td["stop_command_source"] = std::string(scs);
               td["backend_stop_confirmed"] =
                   trip.stop_snapshot.backend_stop_confirmed;
               // Phase 4.5E (codex review follow-up): full stop
               // snapshot fidelity. Downstream consumers need source +
               // state + captured_at_ns so they can decide whether
               // the certified rest position is acceptable for the
               // node's precision contract.
               const char* sss = "execution_backend_feedback";
               switch (trip.stop_snapshot.source) {
                 using enum ExecutionStopSnapshotSource;
                 case kExecutionBackendFeedback:
                   sss = "execution_backend_feedback"; break;
                 case kKernelExecutionSnapshot:
                   sss = "kernel_execution_snapshot"; break;
                 case kPlanningContextState:
                   sss = "planning_context_state"; break;
               }
               td["stop_snapshot_source"] = std::string(sss);
               td["stop_snapshot_captured_at_ns"] =
                   trip.stop_snapshot.captured_at_ns;
               py::dict rs;
               rs["joint_names"] = trip.stop_snapshot.robot_state.joint_names;
               rs["positions_rad"] =
                   trip.stop_snapshot.robot_state.positions_rad;
               td["stop_snapshot_robot_state"] = rs;
               if (trip.stop_distance_rad) {
                 td["stop_distance_rad"] = *trip.stop_distance_rad;
               }
               if (trip.path_deviation_max_rad) {
                 td["path_deviation_max_rad"] = *trip.path_deviation_max_rad;
               }
               if (trip.initial_path_deviation_rad) {
                 td["initial_path_deviation_rad"] =
                     *trip.initial_path_deviation_rad;
               }
               if (trip.fallback_reason) {
                 td["fallback_reason"] = *trip.fallback_reason;
               }
               if (trip.result_binding_key) {
                 td["result_binding_key"] = *trip.result_binding_key;
               }
               td["signal_to_latch_ns"] = trip.signal_to_latch_ns;
               td["latch_to_backend_stop_call_ns"] =
                   trip.latch_to_backend_stop_call_ns;
               td["latch_to_stop_confirmed_ns"] =
                   trip.latch_to_stop_confirmed_ns;
               // Trip diagnostics: bounded JSON-safe string map; surface
               // every entry so the BT/runtime can read scaffold flags
               // (e.g. soft_on_path_scaffold_phase) without re-querying
               // the backend.
               py::dict trip_diag;
               for (const auto& [k, v] : trip.diagnostics) {
                 trip_diag[py::str(k)] = v;
               }
               td["diagnostics"] = trip_diag;
               // Requested vs executed: the executed_stop_mode above
               // describes physical behavior; expose the original
               // requested mode separately so audit can verify the
               // scope's profile matches what the operator authored.
               const char* req_mode = "spring_back_hold";
               switch (trip.requested_stop_profile.stop_mode) {
                 using enum MotionInterruptStopMode;
                 case kSpringBackHold:    req_mode = "spring_back_hold"; break;
                 case kSoftOnPath:        req_mode = "soft_on_path"; break;
                 case kHardDeceleration:  req_mode = "hard_deceleration"; break;
               }
               td["requested_stop_mode"] = std::string(req_mode);
               td["requested_stop_profile_id"] =
                   trip.requested_stop_profile.profile_id;
               d["trip"] = td;
             }
             if (outcome.error) {
               py::dict ed;
               ed["code"] = outcome.error->code;
               ed["message"] = outcome.error->message;
               py::dict details;
               for (const auto& [k, v] : outcome.error->details) {
                 details[py::str(k)] = v;
               }
               ed["details"] = details;
               d["error"] = ed;
             }
             return d;
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
      .def("acknowledge_intervention",
           [](NativeKernelBundle& self, std::string principal_id,
              std::string reason) -> py::dict {
             auto r = self.kernel->acknowledge_intervention(
                 std::move(principal_id), std::move(reason));
             return command_result(r.accepted, r.error);
           },
           py::arg("principal_id"),
           py::arg("reason"))
      // ---- Streaming-session API (ADR-0016) ----
      .def("begin_streaming_session",
           [](NativeKernelBundle& self, py::dict config_dict) -> py::dict {
             StreamingSessionConfigDTO config;
             auto get_str = [&](const char* key, std::string& dst) {
               if (config_dict.contains(key) &&
                   !config_dict[key].is_none()) {
                 dst = config_dict[key].cast<std::string>();
               }
             };
             auto get_double = [&](const char* key, double& dst) {
               if (config_dict.contains(key) &&
                   !config_dict[key].is_none()) {
                 dst = config_dict[key].cast<double>();
               }
             };
             get_str("group_id", config.group_id);
             if (config_dict.contains("joint_names") &&
                 !config_dict["joint_names"].is_none()) {
               for (const auto& name :
                    config_dict["joint_names"].cast<py::list>()) {
                 config.joint_names.push_back(name.cast<std::string>());
               }
             }
             if (config_dict.contains("input_mode") &&
                 !config_dict["input_mode"].is_none()) {
               const auto mode_str =
                   config_dict["input_mode"].cast<std::string>();
               config.default_input_mode =
                   (mode_str == "cartesian")
                       ? StreamingInputMode::kCartesian
                       : StreamingInputMode::kJoint;
             }
             get_str("cartesian_reference_frame",
                     config.cartesian_reference_frame);
             get_str("tcp_frame", config.tcp_frame);
             if (config_dict.contains("soft_stop_strategy") &&
                 !config_dict["soft_stop_strategy"].is_none()) {
               const auto strat_str =
                   config_dict["soft_stop_strategy"].cast<std::string>();
               config.default_soft_stop_strategy =
                   (strat_str == "velocity_scaling")
                       ? SoftStopStrategy::kVelocityScaling
                       : SoftStopStrategy::kSpringBack;
             }
             get_double("soft_stop_deceleration_time_s",
                        config.soft_stop_deceleration_time_s);
             get_double("anchor_negative_time_s",
                        config.anchor_negative_time_s);
             get_double("target_time_from_start_min_s",
                        config.target_time_from_start_min_s);
             get_double("max_dispatch_rate_hz", config.max_dispatch_rate_hz);
             get_double("target_max_age_s", config.target_max_age_s);
             get_double("velocity_scale", config.velocity_scale);
             get_double("max_tcp_linear_speed_mps",
                        config.max_tcp_linear_speed_mps);
             // Per-joint streaming caps (parallel to joint_names),
             // spec/430 §3.2 streaming-joint-limits + ADR-0016.
             // Sized to match joint_names; zero entries fall back to
             // engine defaults at the backend (0.5 rad/s, 2.0 rad/s²).
             if (config_dict.contains("per_joint_velocity_caps_rad_s")
                 && !config_dict["per_joint_velocity_caps_rad_s"].is_none()) {
               for (const auto& v : config_dict["per_joint_velocity_caps_rad_s"]
                                        .cast<py::list>()) {
                 config.per_joint_velocity_caps_rad_s.push_back(v.cast<double>());
               }
             }
             if (config_dict.contains("per_joint_acceleration_caps_rad_s2")
                 && !config_dict["per_joint_acceleration_caps_rad_s2"].is_none()) {
               for (const auto& v :
                    config_dict["per_joint_acceleration_caps_rad_s2"]
                        .cast<py::list>()) {
                 config.per_joint_acceleration_caps_rad_s2.push_back(
                     v.cast<double>());
               }
             }
             get_str("principal_id", config.principal_id);
             get_str("authority_source", config.authority_source);
             get_str("trigger_reason", config.trigger_reason);

             auto r = [&]() {
               py::gil_scoped_release release;
               return self.kernel->begin_streaming_session(config);
             }();
             py::dict d;
             d["accepted"] = r.accepted;
             d["session_id"] = r.session_id;
             if (r.error.has_value()) {
               d["error"] = r.error->message;
               d["error_code"] = r.error->code;
               d["error_details"] =
                   error_details_to_python(r.error->details);
             } else {
               d["error_code"] = py::none();
               d["error_details"] = py::dict();
             }
             return d;
           })
      .def("push_streaming_target",
           [](NativeKernelBundle& self,
              uint64_t session_id,
              py::dict target_dict) -> py::dict {
             StreamingTargetDTO target;
             if (target_dict.contains("kind") &&
                 !target_dict["kind"].is_none()) {
               const auto kind_str = target_dict["kind"].cast<std::string>();
               target.kind = (kind_str == "cartesian")
                                 ? StreamingInputMode::kCartesian
                                 : StreamingInputMode::kJoint;
             }
             if (target_dict.contains("position_m") &&
                 !target_dict["position_m"].is_none()) {
               const auto pos = target_dict["position_m"].cast<py::list>();
               if (pos.size() == 3) {
                 for (size_t i = 0; i < 3; ++i) {
                   target.position_m[i] = pos[i].cast<double>();
                 }
               }
             }
             if (target_dict.contains("orientation_xyzw") &&
                 !target_dict["orientation_xyzw"].is_none()) {
               const auto q = target_dict["orientation_xyzw"]
                                  .cast<py::list>();
               if (q.size() == 4) {
                 for (size_t i = 0; i < 4; ++i) {
                   target.orientation_xyzw[i] = q[i].cast<double>();
                 }
               }
             }
             if (target_dict.contains("joint_names") &&
                 !target_dict["joint_names"].is_none()) {
               for (const auto& name :
                    target_dict["joint_names"].cast<py::list>()) {
                 target.joint_names.push_back(name.cast<std::string>());
               }
             }
             if (target_dict.contains("joint_positions_rad") &&
                 !target_dict["joint_positions_rad"].is_none()) {
               for (const auto& v :
                    target_dict["joint_positions_rad"].cast<py::list>()) {
                 target.joint_positions_rad.push_back(v.cast<double>());
               }
             }
             if (target_dict.contains("source_timestamp_ns") &&
                 !target_dict["source_timestamp_ns"].is_none()) {
               target.source_timestamp_ns =
                   target_dict["source_timestamp_ns"].cast<int64_t>();
             }
             if (target_dict.contains("source_sequence") &&
                 !target_dict["source_sequence"].is_none()) {
               target.source_sequence =
                   target_dict["source_sequence"].cast<uint64_t>();
             }
             if (target_dict.contains("min_time_from_start_s") &&
                 !target_dict["min_time_from_start_s"].is_none()) {
               target.min_time_from_start_s =
                   target_dict["min_time_from_start_s"].cast<double>();
             }

             auto r = [&]() {
               py::gil_scoped_release release;
               return self.kernel->push_streaming_target(session_id, target);
             }();
             py::dict d;
             d["accepted"] = r.accepted;
             d["backend_sequence"] = r.backend_sequence;
             if (r.error.has_value()) {
               d["error"] = r.error->message;
               d["error_code"] = r.error->code;
               d["error_details"] =
                   error_details_to_python(r.error->details);
             } else {
               d["error_code"] = py::none();
               d["error_details"] = py::dict();
             }
             return d;
           })
      .def("end_streaming_session",
           [](NativeKernelBundle& self,
              uint64_t session_id,
              const std::string& strategy_str) -> py::dict {
             const SoftStopStrategy strategy =
                 (strategy_str == "velocity_scaling")
                     ? SoftStopStrategy::kVelocityScaling
                     : SoftStopStrategy::kSpringBack;
             auto r = [&]() {
               py::gil_scoped_release release;
               return self.kernel->end_streaming_session(session_id, strategy);
             }();
             return command_result(r.accepted, r.error);
           })
      .def("set_runtime_velocity_scale",
           [](NativeKernelBundle& self, double scale) {
             py::gil_scoped_release release;
             self.kernel->set_runtime_velocity_scale(scale);
           })
      .def("set_streaming_velocity_scale",
           [](NativeKernelBundle& self, double scale) {
             py::gil_scoped_release release;
             self.kernel->set_streaming_velocity_scale(scale);
           })
      .def("get_status", [](NativeKernelBundle& self) -> py::dict {
        const auto status = [&]() {
          py::gil_scoped_release release;
          return self.kernel->get_status();
        }();
        return status_to_python(status);
      })
      .def("get_execution_snapshot", [](NativeKernelBundle& self) -> py::dict {
        const auto snapshot = [&]() {
          py::gil_scoped_release release;
          return self.kernel->get_execution_snapshot();
        }();
        return snapshot_to_python(snapshot);
      })
      .def("get_metrics", [](NativeKernelBundle& self) -> py::dict {
        const auto metrics = [&]() {
          py::gil_scoped_release release;
          return self.kernel->get_metrics();
        }();
        return metrics_to_python(metrics);
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
            if (config_dict.contains("group_id")) {
              config.group_id = config_dict["group_id"].cast<std::string>();
            }
            if (config_dict.contains("joint_name")) {
              config.joint_name = config_dict["joint_name"].cast<std::string>();
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
            auto& backend = require_ros2_control_execution(self);
            const auto result = [&]() {
              py::gil_scoped_release release;
              return backend.command_gripper(
                  gripper_id, target_position, duration_s, max_effort);
            }();
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
        auto& backend = require_ros2_control_execution(self);
        py::gil_scoped_release release;
        backend.update_gripper_commands();
      })
      .def("get_ros_gripper_status",
          [](NativeKernelBundle& self, const std::string& gripper_id) -> py::dict {
            auto& backend = require_ros2_control_execution(self);
            const auto status = [&]() {
              py::gil_scoped_release release;
              return backend.get_gripper_status(gripper_id);
            }();
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
        py::gil_scoped_release release;
        return self.context->get_scene_object_ids();
      })
      .def("get_joint_names", [](NativeKernelBundle& self) {
        py::gil_scoped_release release;
        return self.context->get_joint_names();
      })
      .def("get_scene_objects", [](NativeKernelBundle& self) -> py::list {
        const auto objects = [&]() {
          py::gil_scoped_release release;
          return self.context->get_scene_objects();
        }();
        py::list result;
        for (const auto& object : objects) {
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
          } else if (shape_type == "cylinder") {
            if (!request.contains("cylinder_radius_m")) {
              result["success"] = false;
              result["error"] = "add with shape_type=cylinder requires cylinder_radius_m";
              return result;
            }
            if (!request.contains("cylinder_height_m")) {
              result["success"] = false;
              result["error"] = "add with shape_type=cylinder requires cylinder_height_m";
              return result;
            }
            double radius = request["cylinder_radius_m"].cast<double>();
            double height = request["cylinder_height_m"].cast<double>();
            req.shape = CylinderShapeDTO{radius, height};
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
          if (request.contains("source_timestamp_ns") &&
              !request["source_timestamp_ns"].is_none()) {
            req.source_timestamp_ns =
                request["source_timestamp_ns"].cast<int64_t>();
          }
          if (request.contains("source_freshness_budget_ns") &&
              !request["source_freshness_budget_ns"].is_none()) {
            req.source_freshness_budget_ns =
                request["source_freshness_budget_ns"].cast<int64_t>();
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
          const auto poses = [&]() {
            py::gil_scoped_release release;
            const auto state = observer_robot_state(self);
            return self.context->forward_kinematics_batch(
                state, std::vector<std::string>{target_frame}, base_frame);
          }();
          const auto it = poses.find(target_frame);
          if (it == poses.end()) {
            return py::none();
          }
          return py::object(pose_to_python(it->second));
        } catch (...) {
          return py::none();
        }
      })
      .def("get_link_poses", [](NativeKernelBundle& self,
                                 const std::vector<std::string>& target_frames,
                                 const std::string& base_frame) -> py::dict {
        const auto poses = [&]() {
          py::gil_scoped_release release;
          const auto state = observer_robot_state(self);
          return self.context->forward_kinematics_batch(
              state, target_frames, base_frame);
        }();
        py::dict result;
        for (const auto& [target_frame, pose] : poses) {
          result[py::str(target_frame)] = pose_to_python(pose);
        }
        return result;
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
        const auto state = [&]() {
          py::gil_scoped_release release;
          return self.context->get_object_state(object_id);
        }();
        return object_state_to_python(state);
      })
      .def("get_scene_trace", [](NativeKernelBundle& self, int limit) -> py::list {
        const auto events = [&]() {
          py::gil_scoped_release release;
          return self.context->get_scene_trace(
              limit < 0 ? 0 : static_cast<size_t>(limit));
        }();
        py::list result;
        for (const auto& event : events) {
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
      })
      // Compute all link poses at a hypothetical robot configuration,
      // for the Composer Move-To "ghost robot" preview (ADR-0016 §UX).
      // Stateless w.r.t. the live scene: builds a RobotStateDTO from the
      // caller's joint vector (or IK result), then calls FK per link.
      // For kind=="cartesian": runs IK first using the supplied tcp_pose
      // and seed_joint_positions_rad; if IK fails, returns
      // {reachable: False, reason, tcp_pose} so the frontend can render
      // the TCP link only (red gripper-shaped ghost) without the rest
      // of the robot.
      // For kind=="joint": uses joint_positions_rad directly.
      .def("compute_target_link_poses",
           [](NativeKernelBundle& self, py::dict req) -> py::dict {
             py::dict out;
             const std::string kind = req.contains("kind")
                 ? req["kind"].cast<std::string>() : std::string("joint");
             const std::string base_frame = req.contains("base_frame")
                 ? req["base_frame"].cast<std::string>() : std::string("base");
             std::vector<std::string> link_names;
             if (req.contains("link_names") && !req["link_names"].is_none()) {
               for (const auto& n : req["link_names"].cast<py::list>()) {
                 link_names.push_back(n.cast<std::string>());
               }
             }
             RobotStateDTO state;
             if (req.contains("joint_names") && !req["joint_names"].is_none()) {
               for (const auto& n : req["joint_names"].cast<py::list>()) {
                 state.joint_names.push_back(n.cast<std::string>());
               }
             }
             if (req.contains("joint_positions_rad")
                 && !req["joint_positions_rad"].is_none()) {
               for (const auto& v : req["joint_positions_rad"].cast<py::list>()) {
                 state.positions_rad.push_back(v.cast<double>());
               }
             }

             if (kind == "cartesian") {
               IkRequestDTO ik_req;
               const auto pos = req["tcp_position_m"].cast<py::list>();
               const auto quat = req["tcp_orientation_xyzw"].cast<py::list>();
               ik_req.goal_pose.position_m.x_m = pos[0].cast<double>();
               ik_req.goal_pose.position_m.y_m = pos[1].cast<double>();
               ik_req.goal_pose.position_m.z_m = pos[2].cast<double>();
               ik_req.goal_pose.orientation.x = quat[0].cast<double>();
               ik_req.goal_pose.orientation.y = quat[1].cast<double>();
               ik_req.goal_pose.orientation.z = quat[2].cast<double>();
               ik_req.goal_pose.orientation.w = quat[3].cast<double>();
               ik_req.base_frame = base_frame;
               ik_req.tcp_frame = req["tcp_frame"].cast<std::string>();
               if (!state.joint_names.empty()
                   && state.positions_rad.size() == state.joint_names.size()) {
                 ik_req.seed_state = state;
               } else {
                 ik_req.seed_state = self.context->get_current_state();
               }
               auto ik_result = [&]() {
                 py::gil_scoped_release release;
                 return self.planner->solve_ik(ik_req);
               }();
               if (!ik_result.ok) {
                 out["reachable"] = false;
                 out["reason"] = ik_result.error
                     ? ik_result.error->code : std::string("ik_failed");
                 out["tcp_pose"] = py::dict(
                     py::arg("position_m") = py::make_tuple(
                         ik_req.goal_pose.position_m.x_m,
                         ik_req.goal_pose.position_m.y_m,
                         ik_req.goal_pose.position_m.z_m),
                     py::arg("orientation_xyzw") = py::make_tuple(
                         ik_req.goal_pose.orientation.x,
                         ik_req.goal_pose.orientation.y,
                         ik_req.goal_pose.orientation.z,
                         ik_req.goal_pose.orientation.w));
                 return out;
               }
               state.joint_names = ik_result.joint_names;
               state.positions_rad = ik_result.positions_rad;
             }

             // FK for each requested link at the resolved joint state.
             py::dict link_poses;
             for (const auto& link_name : link_names) {
               FkRequestDTO fk_req;
               fk_req.robot_state = state;
               fk_req.target_frame = link_name;
               fk_req.base_frame = base_frame;
               try {
                 auto pose = [&]() {
                   py::gil_scoped_release release;
                   return self.context->forward_kinematics(fk_req);
                 }();
                 py::dict d;
                 d["position_m"] = py::make_tuple(
                     pose.position_m.x_m, pose.position_m.y_m, pose.position_m.z_m);
                 d["orientation_xyzw"] = py::make_tuple(
                     pose.orientation.x, pose.orientation.y,
                     pose.orientation.z, pose.orientation.w);
                 link_poses[py::str(link_name)] = d;
               } catch (...) {
                 // Skip links FK can't resolve — frontend just won't
                 // render them at the target pose.
               }
             }
             out["reachable"] = true;
             out["joint_names"] = py::cast(state.joint_names);
             out["joint_positions_rad"] = py::cast(state.positions_rad);
             out["link_poses"] = link_poses;
             return out;
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
