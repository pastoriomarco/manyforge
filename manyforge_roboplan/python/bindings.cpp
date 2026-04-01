#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>

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
        req.speed_profile.velocity_scale = request.attr("get")("velocity_scale", 1.0).cast<double>();
        req.speed_profile.acceleration_scale = request.attr("get")("acceleration_scale", 1.0).cast<double>();

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
            auto dims = request["box_dimensions_m"].cast<std::vector<double>>();
            req.shape = BoxShapeDTO{{dims[0], dims[1], dims[2]}};
          } else if (shape_type == "sphere") {
            double radius = request["sphere_radius_m"].cast<double>();
            req.shape = SphereShapeDTO{radius};
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
        }

        return result;
      })
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
      });

  // Factory function
  m.def("create_native_kernel",
        py::overload_cast<const std::string&, const std::string&>(
            &create_native_kernel),
        py::arg("model_name") = "ur5_gripper",
        py::arg("group_name") = "arm",
        "Create a native C++ kernel with real RoboPlan backends.");
}
