#pragma once

#include <memory>
#include <string>
#include <vector>

#include <manyforge_core/execution_kernel.hpp>
#include <manyforge_roboplan/native_kernel_factory.hpp>
#include <manyforge_ros2_control_execution/ros2_control_capability.hpp>

namespace manyforge::roboplan_backend {

/// Compose a RoboPlan planning kernel with the planner-neutral ros2_control
/// execution backend.
///
/// This factory remains RoboPlan-owned: it constructs the RoboPlan scene,
/// planner/context/collision engines, the RoboPlan execution-state sink, and
/// the ros2_control execution backend. The backend class itself must not
/// depend on RoboPlan scene types.
std::shared_ptr<NativeKernelBundle> create_ros2_control_kernel(
    const std::string& model_name,
    const std::string& group_name,
    const std::string& urdf_path,
    const std::string& srdf_path,
    const std::vector<std::string>& package_paths,
    const ::manyforge::ros2_control_execution::
        Ros2ControlExecutionBackendConfig& ros_config,
    const core::ExecutionKernelConfig& kernel_config = {},
    const std::string& logical_robot_id = {});

}  // namespace manyforge::roboplan_backend
