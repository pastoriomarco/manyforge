"""Factory for Composer's C++ ros2_control execution runtime.

This module is intentionally only Python-side configuration glue.  The ROS
action client, observer/source arbitration, freshness gates, tracking
divergence checks, and gripper JointState publisher live in C++ inside
``manyforge_roboplan_py`` when it is built with
``MANYFORGE_ENABLE_ROS2_CONTROL=ON``.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from manyforge_behavior.deployment import Deployment
from manyforge_behavior.model_paths import (
    default_package_paths,
    resolve_robot_model,
)


@dataclass
class Ros2ControlRuntimeBundle:
    """Owns the native bundle selected for Composer's runtime."""

    native_bundle: Any
    group_id: str

    def shutdown(self) -> None:
        if hasattr(self.native_bundle, "reset"):
            self.native_bundle.reset()

    def set_joint_positions(self, positions: list[float]) -> None:
        self.native_bundle.set_group_joint_positions(self.group_id, positions)


def create_ros2_control_kernel(
    *,
    deployment: Deployment,
    collision_monitor_period_ns: int | None = None,
    runtime_collision_query_timeout_ns: int | None = None,
    runtime_dynamic_body_max_age_cap_ns: int | None = None,
):
    """Create a KernelClient that plans with RoboPlan and executes via C++ ROS."""
    robot_config = deployment.robot_config
    if robot_config is None:
        raise RuntimeError("ROS 2 controller execution requires robot_config")
    execution = deployment.adapters.get("execution")
    if execution is None:
        raise RuntimeError("ROS 2 controller execution requires an execution adapter")
    config = dict(execution.config or {})

    import manyforge_roboplan_py
    from native_kernel_client import NativeKernelClient

    if not hasattr(manyforge_roboplan_py, "create_ros2_control_kernel_from_paths"):
        raise RuntimeError(
            "manyforge_roboplan_py was not built with C++ ros2_control support. "
            "Rebuild with -DMANYFORGE_ENABLE_ROS2_CONTROL=ON."
        )

    resolved_model = resolve_robot_model(
        urdf_path=robot_config.urdf_path,
        srdf_path=robot_config.srdf_path,
        package_paths=robot_config.package_paths,
        deployment_path=deployment.source_path,
        extra_package_paths=default_package_paths(),
    )
    kernel_config: dict[str, float | int] = {}
    if collision_monitor_period_ns is not None:
        kernel_config["collision_monitor_period_ns"] = collision_monitor_period_ns
    if runtime_collision_query_timeout_ns is not None:
        kernel_config["runtime_collision_query_timeout_ns"] = (
            runtime_collision_query_timeout_ns
        )
    if runtime_dynamic_body_max_age_cap_ns is not None:
        kernel_config["runtime_dynamic_body_max_age_cap_ns"] = (
            runtime_dynamic_body_max_age_cap_ns
        )
    for key in (
        "single_shot_ik_timeout_s",
        "single_shot_ik_timeout_ms",
        "default_fault_stop_profile",
        "operator_stop_profile",
    ):
        if key in config:
            kernel_config[key] = config[key]
    kernel_config_arg = kernel_config or None

    required_joints = tuple(str(joint) for joint in config.get("required_joints", []))
    if not required_joints:
        raise RuntimeError("execution.config.required_joints must not be empty")
    action_name = str(config.get("arm_action") or config.get("action_name") or "")
    if not action_name:
        raise RuntimeError("execution.config.arm_action/action_name is required")

    ros_config = dict(config)
    ros_config["action_name"] = action_name
    ros_config["required_joints"] = list(required_joints)
    if collision_monitor_period_ns is not None and "monitor_period_s" not in ros_config:
        ros_config["monitor_period_s"] = float(collision_monitor_period_ns) / 1e9

    native_bundle = manyforge_roboplan_py.create_ros2_control_kernel_from_paths(
        resolved_model.model_name,
        robot_config.group_id or "arm",
        resolved_model.urdf_path,
        resolved_model.srdf_path,
        resolved_model.package_paths,
        ros_config,
        kernel_config_arg,
    )

    _configure_ros_grippers(native_bundle, deployment, config)

    bundle = Ros2ControlRuntimeBundle(
        native_bundle=native_bundle,
        group_id=robot_config.group_id or "arm",
    )
    return NativeKernelClient(native_bundle), bundle


def _configure_ros_grippers(native_bundle: Any, deployment: Deployment, config: dict) -> None:
    if not hasattr(native_bundle, "configure_ros_gripper_action"):
        return
    gripper_config = dict(config.get("gripper") or {})
    for gripper_id, gripper in deployment.grippers.items():
        if gripper.command_mode != "ros_gripper_command":
            continue
        action_name = str(
            gripper_config.get("action_name")
            or gripper_config.get(f"{gripper_id}_action_name")
            or ""
        )
        if not action_name:
            raise RuntimeError(
                "execution.config.gripper.action_name is required for "
                f"gripper {gripper_id!r} command_mode=ros_gripper_command"
            )
        native_bundle.configure_ros_gripper_action(
            gripper_id,
            {
                "action_name": action_name,
                "group_id": str(gripper.group_id),
                "joint_name": str(gripper.joint_name),
                "wait_for_action_server_timeout_s": float(
                    gripper_config.get("wait_for_action_server_timeout_s", 2.0)
                ),
                "result_timeout_s": float(
                    gripper_config.get("result_timeout_s", 5.0)
                ),
            },
        )
