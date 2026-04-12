#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>

#include <cstdint>
#include <cmath>
#include <stdexcept>

#include <Eigen/Dense>

#include <manyforge_roboplan/native_kernel_factory.hpp>

namespace py = pybind11;
using namespace manyforge::roboplan_backend;
using namespace manyforge::core;

namespace {

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
  } else {
    d["error"] = py::none();
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

        auto result = self.kernel->execute(req);
        return result_to_python(result);
      })
      .def("stop", [](NativeKernelBundle& self) -> py::dict {
        auto r = self.kernel->stop();
        return command_result(r.accepted, r.error);
      })
      .def("pause", [](NativeKernelBundle& self) -> py::dict {
        auto r = self.kernel->pause();
        return command_result(r.accepted, r.error);
      })
      .def("resume", [](NativeKernelBundle& self) -> py::dict {
        auto r = self.kernel->resume();
        return command_result(r.accepted, r.error);
      })
      .def("reset", [](NativeKernelBundle& self) -> py::dict {
        auto r = self.kernel->reset();
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
      .def("scene_edit", [](NativeKernelBundle& self, py::dict request) -> py::dict {
        py::dict result;
        std::string op = request["operation"].cast<std::string>();

        if (op == "add") {
          AddObjectRequestDTO req;
          req.object_id = request["object_id"].cast<std::string>();
          req.frame_id = request.attr("get")("frame_id", "universe").cast<std::string>();

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
          } else {
            result["success"] = false;
            result["error"] = "Unsupported shape_type for native backend: " + shape_type;
            return result;
          }

          auto r = self.context->add_object(req);
          result["success"] = r.success;
          if (r.error.has_value()) result["error"] = r.error->message;

        } else if (op == "remove") {
          RemoveObjectRequestDTO req;
          req.object_id = request["object_id"].cast<std::string>();
          auto r = self.context->remove_object(req);
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
          auto r = self.context->update_object_pose(req);
          result["success"] = r.success;
          if (r.error.has_value()) result["error"] = r.error->message;
        } else if (op == "attach") {
          AttachObjectRequestDTO req;
          req.object_id = request["object_id"].cast<std::string>();
          req.frame_id = request.attr("get")("frame_id", "").cast<std::string>();
          auto r = self.context->attach_object(req);
          result["success"] = r.success;
          if (r.error.has_value()) result["error"] = r.error->message;
        } else if (op == "detach") {
          DetachObjectRequestDTO req;
          req.object_id = request["object_id"].cast<std::string>();
          req.frame_id = request.attr("get")("frame_id", "universe").cast<std::string>();
          auto r = self.context->detach_object(req);
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
        self.scene->setJointPositions(q);
        if (self.kernel) {
          self.kernel->sync_snapshot_from_context();
        }
      }, py::arg("positions"),
      "Set the robot's current joint positions in the planning scene.")
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
          req.robot_state = self.context->get_current_state();

          auto pose = self.context->forward_kinematics(req);

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
          return pose_to_python(self.context->get_object_pose(object_id, base_frame));
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
}
