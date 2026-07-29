"""Factory for Composer's C++ ros2_control execution runtime.

This module is intentionally only Python-side configuration glue.  The ROS
action client, observer/source arbitration, freshness gates, tracking
divergence checks, and gripper JointState publisher live in C++ inside
``manyforge_roboplan_py`` when it is built with
``MANYFORGE_ENABLE_ROS2_CONTROL=ON``.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Any

from manyforge_behavior.deployment import Deployment
from manyforge_behavior.model_paths import (
    default_package_paths,
    resolve_robot_model,
)
from manyforge_behavior.robot_model_runtime import (
    attach_robot_model_metadata,
    prepare_roboplan_runtime_model_view,
)


@dataclass
class Ros2ControlRuntimeBundle:
    """Owns the native bundle selected for Composer's runtime."""

    native_bundle: Any
    group_id: str
    execution_admission_subscription: Any | None = None

    def shutdown(self) -> None:
        try:
            if self.execution_admission_subscription is not None:
                self.execution_admission_subscription.shutdown()
        finally:
            self.execution_admission_subscription = None
            if hasattr(self.native_bundle, "reset"):
                self.native_bundle.reset()

    def set_joint_positions(self, positions: list[float]) -> None:
        self.native_bundle.set_group_joint_positions(self.group_id, positions)


def _kernel_stop_profile(
    execution_config: dict[str, Any],
    profile_key: str,
) -> Any:
    """Give the Kernel the same derived stop deadline as the ROS backend.

    ``stop_completion_timeout_{s,ms}`` is the lane's controller-confirmation
    margin.  The ROS backend adds it to the requested deceleration window when
    a profile omits ``max_stop_time_s``.  Materialize that same total in the
    profile sent to the Kernel so its outer watchdog cannot expire while the
    backend is still inside its declared confirmation window.
    """

    profile = execution_config.get(profile_key)
    if not isinstance(profile, dict):
        return profile
    resolved = dict(profile)
    if resolved.get("max_stop_time_s") is not None:
        return resolved
    if resolved.get("deceleration_time_s") is None:
        return resolved

    tolerance_value: Any | None = None
    tolerance_scale = 1.0
    if "stop_completion_timeout_s" in execution_config:
        tolerance_value = execution_config["stop_completion_timeout_s"]
    elif "stop_completion_timeout_ms" in execution_config:
        tolerance_value = execution_config["stop_completion_timeout_ms"]
        tolerance_scale = 0.001
    if tolerance_value is None:
        return resolved
    if (
        isinstance(tolerance_value, bool)
        or not isinstance(tolerance_value, (int, float))
        or not math.isfinite(float(tolerance_value))
        or float(tolerance_value) < 0.0
    ):
        raise RuntimeError(
            "execution.config.stop_completion_timeout must be a finite "
            "non-negative duration"
        )

    deceleration_value = resolved["deceleration_time_s"]
    if (
        isinstance(deceleration_value, bool)
        or not isinstance(deceleration_value, (int, float))
        or not math.isfinite(float(deceleration_value))
        or float(deceleration_value) <= 0.0
    ):
        # Leave validation and its profile-specific diagnostic to the owning
        # deployment/C++ DTO parser rather than inventing a different default.
        return resolved

    resolved["max_stop_time_s"] = (
        float(deceleration_value) + float(tolerance_value) * tolerance_scale
    )
    return resolved


def create_ros2_control_kernel(
    *,
    deployment: Deployment,
    collision_monitor_period_ns: int | None = None,
    runtime_collision_immediate_query_budget_ns: int | None = None,
    validation_collision_query_timeout_ns: int | None = None,
    validation_collision_wall_timeout_ns: int | None = None,
    runtime_dynamic_body_max_age_cap_ns: int | None = None,
    validation_collision_workers: int | None = None,
    runtime_collision_lookahead_cycle_budget_ns: int | None = None,
    runtime_collision_lookahead_query_budget_ns: int | None = None,
    runtime_collision_lookahead_horizon_ns: int | None = None,
    runtime_collision_lookahead_max_samples: int | None = None,
    runtime_collision_lookahead_workers: int | None = None,
):
    """Create a KernelClient that plans with RoboPlan and executes via C++ ROS."""
    robot_config = deployment.robot_config
    if robot_config is None:
        raise RuntimeError("ROS 2 controller execution requires robot_config")
    execution = deployment.adapters.get("execution")
    if execution is None:
        raise RuntimeError("ROS 2 controller execution requires an execution adapter")
    config = dict(execution.config or {})
    planning = deployment.adapters.get("planning")
    planning_config = dict(planning.config or {}) if planning is not None else {}

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
        # Explicit deployment roots are authoritative. Appending environment
        # defaults changes the RMA fingerprint and can resolve the same URI to
        # a different asset across native and ROS lanes.
        extra_package_paths=(
            [] if robot_config.package_paths else default_package_paths()
        ),
    )
    runtime_model = prepare_roboplan_runtime_model_view(
        resolved_model=resolved_model,
        grippers=deployment.grippers,
    )
    kernel_config: dict[str, Any] = {}
    if collision_monitor_period_ns is not None:
        kernel_config["collision_monitor_period_ns"] = collision_monitor_period_ns
    if runtime_collision_immediate_query_budget_ns is not None:
        kernel_config["runtime_collision_immediate_query_budget_ns"] = (
            runtime_collision_immediate_query_budget_ns
        )
    if validation_collision_query_timeout_ns is not None:
        kernel_config["validation_collision_query_timeout_ns"] = (
            validation_collision_query_timeout_ns
        )
    if validation_collision_wall_timeout_ns is not None:
        kernel_config["validation_collision_wall_timeout_ns"] = (
            validation_collision_wall_timeout_ns
        )
    if runtime_dynamic_body_max_age_cap_ns is not None:
        kernel_config["runtime_dynamic_body_max_age_cap_ns"] = (
            runtime_dynamic_body_max_age_cap_ns
        )
    if validation_collision_workers is not None:
        kernel_config["validation_collision_workers"] = (
            validation_collision_workers
        )
    if runtime_collision_lookahead_cycle_budget_ns is not None:
        kernel_config["runtime_collision_lookahead_cycle_budget_ns"] = (
            runtime_collision_lookahead_cycle_budget_ns
        )
    if runtime_collision_lookahead_query_budget_ns is not None:
        kernel_config["runtime_collision_lookahead_query_budget_ns"] = (
            runtime_collision_lookahead_query_budget_ns
        )
    if runtime_collision_lookahead_horizon_ns is not None:
        kernel_config["runtime_collision_lookahead_horizon_ns"] = (
            runtime_collision_lookahead_horizon_ns
        )
    if runtime_collision_lookahead_max_samples is not None:
        kernel_config["runtime_collision_lookahead_max_samples"] = (
            runtime_collision_lookahead_max_samples
        )
    if runtime_collision_lookahead_workers is not None:
        kernel_config["runtime_collision_lookahead_workers"] = (
            runtime_collision_lookahead_workers
        )
    for key in ("single_shot_ik_timeout_s", "single_shot_ik_timeout_ms"):
        if key in config:
            kernel_config[key] = config[key]
    for key in ("default_fault_stop_profile", "operator_stop_profile"):
        if key in config:
            kernel_config[key] = _kernel_stop_profile(config, key)
    for key in (
        "planner_max_planning_time_s",
        "planner_max_planning_time_ms",
        "rng_seed",
    ):
        if key in planning_config:
            kernel_config[key] = planning_config[key]
        elif key in config:
            kernel_config[key] = config[key]
    baseline_policy = deployment.baseline_collision_policy_snapshot()
    if baseline_policy is not None:
        kernel_config["baseline_collision_policy_snapshot"] = baseline_policy
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
        runtime_model.model_name,
        robot_config.group_id or "arm",
        runtime_model.urdf_path,
        runtime_model.srdf_path,
        runtime_model.package_paths,
        ros_config,
        kernel_config_arg,
        robot_config.robot_id,
        True,
    )

    attach_robot_model_metadata(native_bundle, runtime_model)
    _configure_ros_grippers(native_bundle, deployment, config, runtime_model)
    if not hasattr(native_bundle, "complete_ros2_control_configuration"):
        raise RuntimeError(
            "ros2_control native bundle lacks the required configuration "
            "finalization surface"
        )
    native_bundle.complete_ros2_control_configuration()

    admission_subscription = None
    admission_config = config.get("execution_admission")
    if admission_config is not None:
        if not config.get("require_execution_admission_snapshot", False):
            raise RuntimeError(
                "execution_admission transport requires "
                "require_execution_admission_snapshot=true"
            )
        if not isinstance(admission_config, dict):
            raise RuntimeError(
                "execution.config.execution_admission must be a mapping"
            )
        from manyforge_ros.execution_admission import (
            ExecutionAdmissionIngressConfig,
        )
        from manyforge_ros.execution_admission import (
            RosExecutionAdmissionSubscription,
        )

        maximum_lease_ms = admission_config.get('maximum_lease_ms', 250)
        if (
            isinstance(maximum_lease_ms, bool)
            or not isinstance(maximum_lease_ms, int)
            or maximum_lease_ms <= 0
            or maximum_lease_ms > 10_000
        ):
            raise RuntimeError(
                'execution.config.execution_admission.maximum_lease_ms '
                'must be an integer in 1..10000'
            )
        try:
            admission_subscription = RosExecutionAdmissionSubscription(
                native_bundle=native_bundle,
                config=ExecutionAdmissionIngressConfig(
                    topic=admission_config.get("topic"),
                    provider_instance_id=admission_config.get(
                        "provider_instance_id"
                    ),
                    robot_id=robot_config.robot_id,
                    binding_contract_generation_id=(
                        config.get("controller_binding") or {}
                    ).get(
                        "generation_id"
                    ),
                    runtime_generation_id=admission_config.get(
                        "runtime_generation_id"
                    ),
                    cell_supervisor_component_epoch=admission_config.get(
                        "cell_supervisor_component_epoch"
                    ),
                    maximum_lease_ns=maximum_lease_ms * 1_000_000,
                ),
            )
        except BaseException:
            if hasattr(native_bundle, "reset"):
                native_bundle.reset()
            raise
    bundle = Ros2ControlRuntimeBundle(
        native_bundle=native_bundle,
        group_id=robot_config.group_id or "arm",
        execution_admission_subscription=admission_subscription,
    )
    kernel = NativeKernelClient(
        native_bundle,
        robot_model_authority_metadata=runtime_model.metadata,
        robot_model_authority_temp_dir=runtime_model._temp_dir,
    )
    attach_robot_model_metadata(kernel, runtime_model)
    attach_robot_model_metadata(bundle, runtime_model)
    return kernel, bundle


def _configure_ros_grippers(
    native_bundle: Any,
    deployment: Deployment,
    config: dict,
    runtime_model: Any,
) -> None:
    if not hasattr(native_bundle, "configure_ros_gripper_action"):
        return
    gripper_config = dict(config.get("gripper") or {})
    for gripper_id, gripper in deployment.grippers.items():
        if gripper.command_mode != "ros_gripper_command":
            continue
        transport = dict(getattr(gripper, "transport", {}) or {})
        transport_kind = str(transport.get("kind") or "ros_gripper_command")
        _seed_ros_gripper_scene_position(native_bundle, runtime_model, gripper)
        if transport_kind == "isaac_joint_state_effort":
            open_state_ref = str(getattr(gripper, "open_state_ref", "open") or "open")
            closed_state_ref = str(
                getattr(gripper, "closed_state_ref", "closed") or "closed"
            )
            presets = dict(getattr(gripper, "srdf_presets", {}) or {})
            try:
                open_position = float(presets[open_state_ref])
                closed_position = float(presets[closed_state_ref])
            except KeyError as exc:
                raise RuntimeError(
                    f"gripper {gripper_id!r} transport.kind=isaac_joint_state_effort "
                    f"references missing SRDF state {exc.args[0]!r}"
                ) from exc
            native_bundle.configure_ros_gripper_action(
                gripper_id,
                {
                    "transport_kind": "isaac_joint_state_effort",
                    "command_topic": str(transport.get("command_topic") or ""),
                    "state_topic": str(transport.get("state_topic") or ""),
                    "command_joint_names": list(
                        transport.get("joint_names") or [str(gripper.joint_name)]
                    ),
                    "group_id": str(gripper.group_id),
                    "joint_name": str(gripper.joint_name),
                    "completion_policy": str(
                        getattr(gripper, "completion_policy", "dwell_elapsed")
                    ),
                    "open_position": open_position,
                    "closed_position": closed_position,
                    "open_effort": float(transport.get("open_effort", 0.0)),
                    "close_effort": float(transport.get("close_effort", 0.0)),
                    "publish_nominal_position": bool(
                        transport.get("publish_nominal_position", False)
                    ),
                    "state_freshness_timeout_s": float(
                        transport.get("state_freshness_timeout_s", 1.0)
                    ),
                    "result_timeout_s": float(transport.get("dwell_s", 0.0)),
                },
            )
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
                "transport_kind": "ros_gripper_command",
                "action_name": action_name,
                "group_id": str(gripper.group_id),
                "joint_name": str(gripper.joint_name),
                "completion_policy": str(
                    getattr(gripper, "completion_policy", "position_reached")
                ),
                "wait_for_action_server_timeout_s": float(
                    gripper_config.get("wait_for_action_server_timeout_s", 2.0)
                ),
                "result_timeout_s": float(
                    gripper_config.get("result_timeout_s", 5.0)
                ),
            },
        )


def _seed_ros_gripper_scene_position(
    native_bundle: Any,
    runtime_model: Any,
    gripper: Any,
) -> None:
    """Seed a ROS gripper command joint in the authority-defined compact view."""

    presets = getattr(gripper, "srdf_presets", {}) or {}
    if "open" not in presets:
        return
    get_positions = getattr(
        native_bundle,
        "get_group_commandable_joint_positions",
        None,
    )
    set_positions = getattr(native_bundle, "set_group_joint_positions", None)
    if not callable(get_positions) or not callable(set_positions):
        return

    group_id = str(gripper.group_id)
    joint_name = str(gripper.joint_name)
    commandable_by_group = (
        runtime_model.metadata.get("group_commandable_joint_names", {}) or {}
    )
    commandable_joints = [
        str(joint) for joint in commandable_by_group.get(group_id, [])
    ]
    if not commandable_joints:
        commandable_joints = [joint_name]
    try:
        joint_index = commandable_joints.index(joint_name)
    except ValueError as exc:
        raise RuntimeError(
            f"gripper joint {joint_name!r} is not commandable in group {group_id!r}"
        ) from exc

    positions = list(get_positions(group_id))
    if len(positions) < len(commandable_joints):
        raise RuntimeError(
            f"gripper group {group_id!r} reported {len(positions)} commandable "
            f"positions for {len(commandable_joints)} commandable joints"
        )
    positions[joint_index] = float(presets["open"])
    set_positions(group_id, positions)
