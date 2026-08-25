"""Bring up the retained Isaac ros2_control path behind an exact world barrier.

The launch owns no motion or lifecycle policy. It first proves that Isaac's
``simulation_interfaces`` owner loaded the selected world and left it stopped.
Only then does it expose the retained JTC transport and the two thin mechanical
adapters around the C++ execution backend's single command-admission fact.
"""

from __future__ import annotations

import math
from pathlib import Path

from launch import LaunchDescription
from launch.actions import (
    DeclareLaunchArgument,
    EmitEvent,
    OpaqueFunction,
    RegisterEventHandler,
    TimerAction,
)
from launch.event_handlers import OnProcessExit
from launch.events import Shutdown
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare


DEFAULT_WORLD_URI = (
    "file:/home/tndlux/workspaces/isaac_ros-dev/src/"
    "isaac_sim_custom_examples/Scenario/IsaacManipulator/UniversalRobots/"
    "isaac_manipulator_scene_light.usda"
)
DEFAULT_WORLD_NAME = "isaac_manipulator_scene_light"
DEFAULT_RUNTIME_NAMESPACE = "/manyforge/isaac_ur10e_light_robot"


def generate_launch_description() -> LaunchDescription:
    return LaunchDescription(
        [
            DeclareLaunchArgument(
                "isaac_profile",
                default_value="ur10e",
                description="Retained Isaac transport profile: ur10e or lite6.",
            ),
            DeclareLaunchArgument(
                "runtime_namespace",
                default_value=DEFAULT_RUNTIME_NAMESPACE,
                description=(
                    "Exact /manyforge/<controllable_component_id> namespace "
                    "shared by this runtime and its mechanical adapters."
                ),
            ),
            DeclareLaunchArgument(
                "world_uri",
                default_value=DEFAULT_WORLD_URI,
                description="Exact local file URI resolved by the Isaac host.",
            ),
            DeclareLaunchArgument(
                "expected_world_name",
                default_value=DEFAULT_WORLD_NAME,
                description="Exact GetCurrentWorld name required after LoadWorld.",
            ),
            DeclareLaunchArgument("scene_startup_timeout_s", default_value="180.0"),
            DeclareLaunchArgument("scene_service_timeout_s", default_value="120.0"),
            DeclareLaunchArgument(
                "pose_observer_site_config",
                default_value="",
                description=(
                    "Absolute path to the exact retained-cell pose-observer "
                    "site config. Empty disables observation."
                ),
            ),
            DeclareLaunchArgument(
                "manager_run_root",
                default_value="",
                description=(
                    "Absolute Manager run root. Required exactly when the "
                    "pose observer is enabled."
                ),
            ),
            DeclareLaunchArgument(
                "pose_observer_manager_timeout_s", default_value="2.0"
            ),
            DeclareLaunchArgument(
                "pose_observer_simulation_service_timeout_s",
                default_value="2.0",
            ),
            DeclareLaunchArgument(
                "pose_observer_retry_log_period_s", default_value="5.0"
            ),
            DeclareLaunchArgument(
                "pointcloud_topic",
                default_value="",
                description=(
                    "Absolute cell-frame PointCloud2 topic. Empty disables the "
                    "demand-driven browser observation bridge."
                ),
            ),
            DeclareLaunchArgument(
                "pointcloud_source_cell_frame_id", default_value="universe"
            ),
            DeclareLaunchArgument("pointcloud_cell_id", default_value=""),
            DeclareLaunchArgument(
                "pointcloud_composer_base_url",
                default_value="http://127.0.0.1:8000",
            ),
            DeclareLaunchArgument(
                "pointcloud_owner_id",
                default_value="manyforge.isaac.pointcloud",
            ),
            DeclareLaunchArgument("pointcloud_max_points", default_value="200000"),
            DeclareLaunchArgument(
                "pointcloud_minimum_period_s", default_value="0.1"
            ),
            DeclareLaunchArgument(
                "pointcloud_demand_poll_period_s", default_value="0.5"
            ),
            DeclareLaunchArgument("pointcloud_http_timeout_s", default_value="1.0"),
            DeclareLaunchArgument(
                "additional_required_topics",
                default_value="",
                description=(
                    "Comma-separated absolute topic names that must join the "
                    "profile's simulator-owned Isaac graph before bringup "
                    "continues. Topics owned by downstream providers must be "
                    "validated after those providers start."
                ),
            ),
            DeclareLaunchArgument(
                "joint_commands_topic", default_value="/isaac_joint_commands"
            ),
            DeclareLaunchArgument(
                "joint_states_topic", default_value="/isaac_joint_states"
            ),
            DeclareLaunchArgument(
                "controller_manager_name", default_value="controller_manager"
            ),
            DeclareLaunchArgument(
                "controller_manager_lock_memory", default_value="false"
            ),
            DeclareLaunchArgument(
                "controller_manager_thread_priority", default_value="50"
            ),
            DeclareLaunchArgument(
                "controller_manager_cpu_affinity", default_value=""
            ),
            DeclareLaunchArgument(
                "arm_controller_name", default_value="isaac_arm_controller"
            ),
            DeclareLaunchArgument(
                "trigger_joint_command_threshold", default_value="-1.0"
            ),
            DeclareLaunchArgument(
                "command_gate_enable_topic",
                default_value="manyforge/isaac_arm_command_enable",
            ),
            DeclareLaunchArgument(
                "command_gate_owner_node",
                default_value="manyforge_ros2_control_execution_backend",
            ),
            DeclareLaunchArgument(
                "command_gate_owner_namespace",
                default_value="",
                description=(
                    "Optional exact owner namespace assertion. Empty selects "
                    "runtime_namespace; any crossed value is rejected."
                ),
            ),
            DeclareLaunchArgument(
                "stop_joint_commands_topic", default_value="/stop_joint_commands"
            ),
            DeclareLaunchArgument(
                "command_gate_publish_rate_hz", default_value="20.0"
            ),
            DeclareLaunchArgument("sum_wrapped_joint_states", default_value="false"),
            DeclareLaunchArgument("shoulder_pan_initial", default_value="2.63422544"),
            DeclareLaunchArgument(
                "shoulder_lift_initial", default_value="-0.875980752"
            ),
            DeclareLaunchArgument("elbow_initial", default_value="1.551074106"),
            DeclareLaunchArgument("wrist_1_initial", default_value="-2.29231544"),
            DeclareLaunchArgument("wrist_2_initial", default_value="-1.560498884"),
            DeclareLaunchArgument("wrist_3_initial", default_value="-2.038544566"),
            DeclareLaunchArgument("joint1_initial", default_value="0.0"),
            DeclareLaunchArgument("joint2_initial", default_value="0.0"),
            DeclareLaunchArgument("joint3_initial", default_value="1.57"),
            DeclareLaunchArgument("joint4_initial", default_value="0.0"),
            DeclareLaunchArgument("joint5_initial", default_value="0.0"),
            DeclareLaunchArgument("joint6_initial", default_value="0.0"),
            DeclareLaunchArgument(
                "gripper_action_name",
                default_value="robotiq_gripper_controller/gripper_cmd",
            ),
            DeclareLaunchArgument(
                "gripper_command_topic", default_value="/isaac_gripper_state"
            ),
            DeclareLaunchArgument(
                "gripper_state_topic", default_value="/isaac_joint_states"
            ),
            DeclareLaunchArgument(
                "gripper_readiness_topic",
                default_value="manyforge/isaac_gripper_bridge/ready",
            ),
            DeclareLaunchArgument(
                "start_gripper_action_bridge",
                default_value="auto",
                description=(
                    "auto starts the thin UR/Robotiq action adapter; Lite6 uses "
                    "the retained C++ direct-effort transport."
                ),
            ),
            DeclareLaunchArgument("base_parent_frame", default_value="universe"),
            DeclareLaunchArgument("base_child_frame", default_value="world"),
            DeclareLaunchArgument("base_xyz", default_value="0 0 0"),
            DeclareLaunchArgument("base_rpy", default_value="0 0 0"),
            OpaqueFunction(function=_launch_setup),
        ]
    )


def _launch_setup(context, *_, **__) -> list[object]:
    package_share = Path(
        FindPackageShare("manyforge_ros_bringup").find("manyforge_ros_bringup")
    )
    profile = _profile_configuration(
        LaunchConfiguration("isaac_profile").perform(context)
    )
    runtime_namespace = _instance_namespace(
        LaunchConfiguration("runtime_namespace").perform(context),
        name="runtime_namespace",
    )
    joint_commands_topic = _absolute_ros_name(
        LaunchConfiguration("joint_commands_topic").perform(context),
        name="joint_commands_topic",
    )
    joint_states_topic = _absolute_ros_name(
        LaunchConfiguration("joint_states_topic").perform(context),
        name="joint_states_topic",
    )
    additional_topics = _parse_topic_csv(
        LaunchConfiguration("additional_required_topics").perform(context),
        name="additional_required_topics",
    )
    pose_observer_site_config = _optional_absolute_path(
        LaunchConfiguration("pose_observer_site_config").perform(context),
        name="pose_observer_site_config",
    )
    manager_run_root = _optional_absolute_path(
        LaunchConfiguration("manager_run_root").perform(context),
        name="manager_run_root",
    )
    if bool(pose_observer_site_config) != bool(manager_run_root):
        raise ValueError(
            "pose_observer_site_config and manager_run_root must be "
            "configured together"
        )
    required_topics = sorted(
        {
            "/clock",
            joint_commands_topic,
            joint_states_topic,
            profile["raw_gripper_topic"],
            *additional_topics,
        }
    )

    urdf_template = (
        package_share / "urdf" / profile["urdf_file"]
    ).read_text(encoding="utf-8")
    robot_description = urdf_template.format(
        joint_commands_topic=joint_commands_topic,
        joint_states_topic=joint_states_topic,
        trigger_joint_command_threshold=_bounded_float_text(
            LaunchConfiguration("trigger_joint_command_threshold").perform(context),
            name="trigger_joint_command_threshold",
            minimum=-1.0,
            maximum=1.0e9,
        ),
        sum_wrapped_joint_states=str(
            _parse_bool_launch_value(
                LaunchConfiguration("sum_wrapped_joint_states").perform(context),
                name="sum_wrapped_joint_states",
            )
        ).lower(),
        shoulder_pan_initial=_finite_float_text(
            LaunchConfiguration("shoulder_pan_initial").perform(context),
            name="shoulder_pan_initial",
        ),
        shoulder_lift_initial=_finite_float_text(
            LaunchConfiguration("shoulder_lift_initial").perform(context),
            name="shoulder_lift_initial",
        ),
        elbow_initial=_finite_float_text(
            LaunchConfiguration("elbow_initial").perform(context),
            name="elbow_initial",
        ),
        wrist_1_initial=_finite_float_text(
            LaunchConfiguration("wrist_1_initial").perform(context),
            name="wrist_1_initial",
        ),
        wrist_2_initial=_finite_float_text(
            LaunchConfiguration("wrist_2_initial").perform(context),
            name="wrist_2_initial",
        ),
        wrist_3_initial=_finite_float_text(
            LaunchConfiguration("wrist_3_initial").perform(context),
            name="wrist_3_initial",
        ),
        joint1_initial=_finite_float_text(
            LaunchConfiguration("joint1_initial").perform(context),
            name="joint1_initial",
        ),
        joint2_initial=_finite_float_text(
            LaunchConfiguration("joint2_initial").perform(context),
            name="joint2_initial",
        ),
        joint3_initial=_finite_float_text(
            LaunchConfiguration("joint3_initial").perform(context),
            name="joint3_initial",
        ),
        joint4_initial=_finite_float_text(
            LaunchConfiguration("joint4_initial").perform(context),
            name="joint4_initial",
        ),
        joint5_initial=_finite_float_text(
            LaunchConfiguration("joint5_initial").perform(context),
            name="joint5_initial",
        ),
        joint6_initial=_finite_float_text(
            LaunchConfiguration("joint6_initial").perform(context),
            name="joint6_initial",
        ),
    )

    controller_manager_name = _relative_ros_name(
        LaunchConfiguration("controller_manager_name").perform(context),
        name="controller_manager_name",
    )
    arm_controller_name = _relative_ros_name(
        LaunchConfiguration("arm_controller_name").perform(context),
        name="arm_controller_name",
    )
    controller_manager_resources: dict[str, object] = {
        "lock_memory": _parse_bool_launch_value(
            LaunchConfiguration("controller_manager_lock_memory").perform(context),
            name="controller_manager_lock_memory",
        ),
        "thread_priority": _parse_bounded_int_launch_value(
            LaunchConfiguration("controller_manager_thread_priority").perform(
                context
            ),
            name="controller_manager_thread_priority",
            minimum=1,
            maximum=99,
        ),
    }
    affinity = _parse_cpu_affinity_launch_value(
        LaunchConfiguration("controller_manager_cpu_affinity").perform(context),
        name="controller_manager_cpu_affinity",
    )
    if affinity:
        controller_manager_resources["cpu_affinity"] = affinity

    owner_node = _relative_ros_name(
        LaunchConfiguration("command_gate_owner_node").perform(context),
        name="command_gate_owner_node",
    )
    requested_owner_namespace = LaunchConfiguration(
        "command_gate_owner_namespace"
    ).perform(context).strip()
    owner_namespace = (
        runtime_namespace
        if not requested_owner_namespace
        else _instance_namespace(
            requested_owner_namespace,
            name="command_gate_owner_namespace",
        )
    )
    if owner_namespace != runtime_namespace:
        raise ValueError(
            "command_gate_owner_namespace must equal runtime_namespace"
        )
    enable_topic = _scoped_ros_name(
        LaunchConfiguration("command_gate_enable_topic").perform(context),
        runtime_namespace=runtime_namespace,
        name="command_gate_enable_topic",
    )
    stop_topic = _absolute_ros_name(
        LaunchConfiguration("stop_joint_commands_topic").perform(context),
        name="stop_joint_commands_topic",
    )

    scene_barrier = Node(
        package="manyforge_ros_bringup",
        executable="isaac_scene_load_barrier",
        name="manyforge_isaac_scene_load_barrier",
        namespace=runtime_namespace,
        output="screen",
        parameters=[
            {
                "world_uri": LaunchConfiguration("world_uri").perform(context),
                "expected_world_name": LaunchConfiguration(
                    "expected_world_name"
                ).perform(context),
                "startup_timeout_s": _bounded_float(
                    LaunchConfiguration("scene_startup_timeout_s").perform(context),
                    name="scene_startup_timeout_s",
                    minimum=0.1,
                    maximum=600.0,
                ),
                "service_timeout_s": _bounded_float(
                    LaunchConfiguration("scene_service_timeout_s").perform(context),
                    name="scene_service_timeout_s",
                    minimum=0.1,
                    maximum=120.0,
                ),
                "required_topics": required_topics,
            }
        ],
    )
    topic_command_gate = Node(
        package="manyforge_ros_bringup",
        executable="isaac_topic_command_gate",
        name="manyforge_isaac_topic_command_gate",
        namespace=runtime_namespace,
        output="screen",
        parameters=[
            {
                "enable_topic": enable_topic,
                "stop_topic": stop_topic,
                "expected_owner_node": owner_node,
                "expected_owner_namespace": owner_namespace,
                "publish_rate_hz": _bounded_float(
                    LaunchConfiguration("command_gate_publish_rate_hz").perform(
                        context
                    ),
                    name="command_gate_publish_rate_hz",
                    minimum=1.0,
                    maximum=200.0,
                ),
            }
        ],
        on_exit=_shutdown("Isaac command-gate adapter exited"),
    )
    robot_state_publisher = Node(
        package="robot_state_publisher",
        executable="robot_state_publisher",
        namespace=runtime_namespace,
        output="screen",
        parameters=[{"robot_description": robot_description}],
    )
    base_xyz = _parse_vector3(
        LaunchConfiguration("base_xyz").perform(context), name="base_xyz"
    )
    base_rpy = _parse_vector3(
        LaunchConfiguration("base_rpy").perform(context), name="base_rpy"
    )
    static_base_tf = Node(
        package="tf2_ros",
        executable="static_transform_publisher",
        name="manyforge_static_base_tf",
        namespace=runtime_namespace,
        output="screen",
        arguments=[
            "--x",
            base_xyz[0],
            "--y",
            base_xyz[1],
            "--z",
            base_xyz[2],
            "--roll",
            base_rpy[0],
            "--pitch",
            base_rpy[1],
            "--yaw",
            base_rpy[2],
            "--frame-id",
            LaunchConfiguration("base_parent_frame"),
            "--child-frame-id",
            LaunchConfiguration("base_child_frame"),
        ],
    )
    controller_manager = Node(
        package="controller_manager",
        executable="ros2_control_node",
        name=controller_manager_name,
        namespace=runtime_namespace,
        output="screen",
        parameters=[
            {"robot_description": robot_description},
            controller_manager_resources,
            str(package_share / "config" / profile["controller_config_file"]),
        ],
        on_exit=_shutdown("Isaac controller_manager exited"),
    )
    controller_manager_path = _join_namespace(
        runtime_namespace, controller_manager_name
    )
    joint_state_broadcaster = Node(
        package="controller_manager",
        executable="spawner",
        namespace=runtime_namespace,
        output="screen",
        arguments=[
            "joint_state_broadcaster",
            "--controller-manager",
            controller_manager_path,
            "--controller-manager-timeout",
            "30",
        ],
    )
    arm_controller = Node(
        package="controller_manager",
        executable="spawner",
        namespace=runtime_namespace,
        output="screen",
        arguments=[
            arm_controller_name,
            "--controller-manager",
            controller_manager_path,
            "--controller-manager-timeout",
            "30",
        ],
    )

    control_actions: list[object] = [
        topic_command_gate,
        robot_state_publisher,
        static_base_tf,
    ]
    start_gripper = _resolve_start_gripper_action_bridge(
        LaunchConfiguration("start_gripper_action_bridge").perform(context),
        isaac_profile=profile["name"],
    )
    if start_gripper:
        control_actions.append(
            Node(
                package="manyforge_ros_bringup",
                executable="isaac_gripper_action_bridge",
                name="manyforge_isaac_gripper_action_bridge",
                namespace=runtime_namespace,
                output="screen",
                parameters=[
                    str(
                        package_share
                        / "config"
                        / "isaac_robotiq_2f_140_gripper_bridge.yaml"
                    ),
                    {
                        "action_name": _scoped_ros_name(
                            LaunchConfiguration("gripper_action_name").perform(
                                context
                            ),
                            runtime_namespace=runtime_namespace,
                            name="gripper_action_name",
                        ),
                        "command_topic": _absolute_ros_name(
                            LaunchConfiguration("gripper_command_topic").perform(
                                context
                            ),
                            name="gripper_command_topic",
                        ),
                        "state_topic": _absolute_ros_name(
                            LaunchConfiguration("gripper_state_topic").perform(
                                context
                            ),
                            name="gripper_state_topic",
                        ),
                        "readiness_topic": _scoped_ros_name(
                            LaunchConfiguration("gripper_readiness_topic").perform(
                                context
                            ),
                            runtime_namespace=runtime_namespace,
                            name="gripper_readiness_topic",
                        ),
                        "command_enable_topic": enable_topic,
                        "expected_owner_node": owner_node,
                        "expected_owner_namespace": owner_namespace,
                    },
                ],
                on_exit=_shutdown("Isaac gripper transport adapter exited"),
            )
        )
    if pose_observer_site_config:
        control_actions.append(
            Node(
                package="manyforge_ros_bringup",
                executable="pose_source_snapshot_publisher",
                name="manyforge_isaac_pose_observer",
                namespace=runtime_namespace,
                output="screen",
                parameters=[
                    {
                        "site_config_path": pose_observer_site_config,
                        "manager_run_root": manager_run_root,
                        "manager_timeout_s": _bounded_float(
                            LaunchConfiguration(
                                "pose_observer_manager_timeout_s"
                            ).perform(context),
                            name="pose_observer_manager_timeout_s",
                            minimum=0.05,
                            maximum=5.0,
                        ),
                        "simulation_service_timeout_s": _bounded_float(
                            LaunchConfiguration(
                                "pose_observer_simulation_service_timeout_s"
                            ).perform(context),
                            name="pose_observer_simulation_service_timeout_s",
                            minimum=0.05,
                            maximum=30.0,
                        ),
                        "retry_log_period_s": _bounded_float(
                            LaunchConfiguration(
                                "pose_observer_retry_log_period_s"
                            ).perform(context),
                            name="pose_observer_retry_log_period_s",
                            minimum=0.1,
                            maximum=300.0,
                        ),
                    }
                ],
                on_exit=_shutdown("Isaac pose-observer adapter exited"),
            )
        )
    pointcloud_topic_raw = LaunchConfiguration("pointcloud_topic").perform(
        context
    ).strip()
    if pointcloud_topic_raw:
        pointcloud_topic = _absolute_ros_name(
            pointcloud_topic_raw,
            name="pointcloud_topic",
        )
        pointcloud_cell_id = _bounded_observer_identity(
            LaunchConfiguration("pointcloud_cell_id").perform(context),
            name="pointcloud_cell_id",
            maximum_bytes=256,
        )
        control_actions.append(
            Node(
                package="manyforge_ros_bringup",
                executable="isaac_pointcloud_observer_bridge",
                name="manyforge_isaac_pointcloud_observer_bridge",
                namespace=runtime_namespace,
                output="screen",
                parameters=[
                    {
                        "pointcloud_topic": pointcloud_topic,
                        "source_cell_frame_id": _bounded_observer_identity(
                            LaunchConfiguration(
                                "pointcloud_source_cell_frame_id"
                            ).perform(context),
                            name="pointcloud_source_cell_frame_id",
                            maximum_bytes=256,
                        ),
                        "cell_id": pointcloud_cell_id,
                        "composer_base_url": LaunchConfiguration(
                            "pointcloud_composer_base_url"
                        ).perform(context),
                        "owner_id": _bounded_observer_identity(
                            LaunchConfiguration("pointcloud_owner_id").perform(
                                context
                            ),
                            name="pointcloud_owner_id",
                            maximum_bytes=128,
                        ),
                        "max_points": _parse_bounded_int_launch_value(
                            LaunchConfiguration("pointcloud_max_points").perform(
                                context
                            ),
                            name="pointcloud_max_points",
                            minimum=1,
                            maximum=200_000,
                        ),
                        "minimum_period_s": _bounded_float(
                            LaunchConfiguration(
                                "pointcloud_minimum_period_s"
                            ).perform(context),
                            name="pointcloud_minimum_period_s",
                            minimum=0.0,
                            maximum=10.0,
                        ),
                        "demand_poll_period_s": _bounded_float(
                            LaunchConfiguration(
                                "pointcloud_demand_poll_period_s"
                            ).perform(context),
                            name="pointcloud_demand_poll_period_s",
                            minimum=0.1,
                            maximum=10.0,
                        ),
                        "http_timeout_s": _bounded_float(
                            LaunchConfiguration(
                                "pointcloud_http_timeout_s"
                            ).perform(context),
                            name="pointcloud_http_timeout_s",
                            minimum=0.05,
                            maximum=5.0,
                        ),
                    }
                ],
                on_exit=_shutdown("Isaac point-cloud observer adapter exited"),
            )
        )

    # The gate publishes its fail-closed state before hardware activation. The
    # repeated transient-local Stop sample then reaches the topic-based system
    # during its configure/activate sequence.
    control_actions.extend(
        [
            TimerAction(period=0.5, actions=[controller_manager]),
            TimerAction(period=2.5, actions=[joint_state_broadcaster]),
            TimerAction(period=3.5, actions=[arm_controller]),
        ]
    )
    return [
        scene_barrier,
        RegisterEventHandler(
            OnProcessExit(
                target_action=scene_barrier,
                on_exit=lambda event, _context: _after_scene_barrier(
                    event.returncode,
                    control_actions,
                ),
            )
        ),
    ]


def _after_scene_barrier(returncode: object, actions: list[object]) -> list[object]:
    if type(returncode) is int and returncode == 0:
        return actions
    raise RuntimeError(
        f"Isaac scene-load barrier failed with return code {returncode!r}"
    )


def _shutdown(reason: str) -> EmitEvent:
    return EmitEvent(event=Shutdown(reason=reason))


def _profile_configuration(value: str) -> dict[str, str]:
    normalized = value.strip().lower()
    profiles = {
        "ur10e": {
            "name": "ur10e",
            "urdf_file": "isaac_ur10e_topic_based.ros2_control.urdf",
            "controller_config_file": "isaac_ur10e_topic_based_controllers.yaml",
            "raw_gripper_topic": "/isaac_gripper_state",
        },
        "lite6": {
            "name": "lite6",
            "urdf_file": "isaac_lite6_topic_based.ros2_control.urdf",
            "controller_config_file": "isaac_lite6_topic_based_controllers.yaml",
            "raw_gripper_topic": "/isaac_joint_commands_gripper",
        },
    }
    if normalized not in profiles:
        raise ValueError(
            f"unsupported isaac_profile={value!r}; expected ur10e or lite6"
        )
    return dict(profiles[normalized])


def _resolve_start_gripper_action_bridge(value: str, *, isaac_profile: str) -> bool:
    normalized = value.strip().lower()
    if normalized == "auto":
        return isaac_profile == "ur10e"
    requested = _parse_bool_launch_value(
        normalized, name="start_gripper_action_bridge"
    )
    if requested and isaac_profile != "ur10e":
        raise ValueError(
            "Lite6 uses the retained C++ direct-effort gripper transport; "
            "the UR action bridge cannot be enabled for lite6"
        )
    return requested


def _absolute_ros_name(value: str, *, name: str) -> str:
    normalized = value.strip()
    if (
        not normalized.startswith("/")
        or normalized == "/"
        or "//" in normalized
        or any(character.isspace() for character in normalized)
        or len(normalized.encode("utf-8")) > 1024
    ):
        raise ValueError(f"{name} must be one bounded absolute ROS name")
    return normalized


def _relative_ros_name(value: str, *, name: str) -> str:
    normalized = value.strip()
    if (
        not normalized
        or normalized.startswith("/")
        or "/" in normalized
        or any(character.isspace() for character in normalized)
        or not normalized.isascii()
        or (not normalized[0].isalpha() and normalized[0] != "_")
        or any(
            not character.isalnum() and character != "_"
            for character in normalized[1:]
        )
        or len(normalized.encode("utf-8")) > 256
    ):
        raise ValueError(f"{name} must be one bounded relative ROS node name")
    return normalized


def _absolute_namespace(value: str, *, name: str) -> str:
    normalized = value.strip().rstrip("/") or "/"
    if normalized == "/":
        return normalized
    return _absolute_ros_name(normalized, name=name)


def _instance_namespace(value: str, *, name: str) -> str:
    normalized = _absolute_namespace(value, name=name)
    prefix = "/manyforge/"
    if not normalized.startswith(prefix):
        raise ValueError(
            f"{name} must be /manyforge/<controllable_component_id>"
        )
    component_id = normalized[len(prefix) :]
    if "/" in component_id:
        raise ValueError(
            f"{name} must contain exactly one controllable-component token"
        )
    _relative_ros_name(component_id, name=name)
    return normalized


def _relative_ros_path(value: str, *, name: str) -> str:
    normalized = value.strip().strip("/")
    if (
        not normalized
        or value.strip().startswith("/")
        or value.strip().endswith("/")
        or "//" in normalized
        or len(normalized.encode("utf-8")) > 1024
    ):
        raise ValueError(f"{name} must be one bounded relative ROS name")
    for token in normalized.split("/"):
        _relative_ros_name(token, name=name)
    return normalized


def _join_namespace(namespace: str, relative_name: str) -> str:
    return f"{namespace}/{relative_name}"


def _scoped_ros_name(
    value: str, *, runtime_namespace: str, name: str
) -> str:
    return _join_namespace(
        runtime_namespace,
        _relative_ros_path(value, name=name),
    )


def _parse_topic_csv(value: str, *, name: str) -> list[str]:
    if not value.strip():
        return []
    raw_topics = value.split(",")
    if len(raw_topics) > 64:
        raise ValueError(f"{name} contains more than 64 topics")
    topics = [_absolute_ros_name(topic, name=name) for topic in raw_topics]
    if len(set(topics)) != len(topics):
        raise ValueError(f"{name} contains duplicate topics")
    return topics


def _optional_absolute_path(value: str, *, name: str) -> str:
    normalized = value.strip()
    if not normalized:
        return ""
    if (
        not normalized.startswith("/")
        or "\x00" in normalized
        or len(normalized.encode("utf-8")) > 4096
    ):
        raise ValueError(f"{name} must be one bounded absolute path")
    return normalized


def _bounded_observer_identity(
    value: str,
    *,
    name: str,
    maximum_bytes: int,
) -> str:
    normalized = value.strip()
    allowed = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._:/-"
    if (
        not normalized
        or normalized != value
        or normalized[0] not in allowed[:62]
        or any(character not in allowed for character in normalized)
        or len(normalized.encode("utf-8")) > maximum_bytes
    ):
        raise ValueError(f"{name} must be one bounded observer identity")
    return normalized


def _parse_vector3(value: str, *, name: str) -> list[str]:
    pieces = value.split()
    if len(pieces) != 3:
        raise ValueError(f"{name} must contain exactly three finite numbers")
    return [
        _finite_float_text(piece, name=f"{name}[{index}]")
        for index, piece in enumerate(pieces)
    ]


def _finite_float_text(value: str, *, name: str) -> str:
    parsed = _bounded_float(
        value,
        name=name,
        minimum=-1.0e12,
        maximum=1.0e12,
    )
    return format(parsed, ".17g")


def _bounded_float_text(
    value: str,
    *,
    name: str,
    minimum: float,
    maximum: float,
) -> str:
    return format(
        _bounded_float(value, name=name, minimum=minimum, maximum=maximum),
        ".17g",
    )


def _bounded_float(
    value: str,
    *,
    name: str,
    minimum: float,
    maximum: float,
) -> float:
    try:
        parsed = float(value.strip())
    except ValueError as error:
        raise ValueError(f"{name} must be a number") from error
    if not math.isfinite(parsed) or not minimum <= parsed <= maximum:
        raise ValueError(f"{name} must be finite and in [{minimum}, {maximum}]")
    return parsed


def _parse_bool_launch_value(value: str, *, name: str) -> bool:
    normalized = value.strip().lower()
    if normalized in {"1", "true", "yes", "on"}:
        return True
    if normalized in {"0", "false", "no", "off"}:
        return False
    raise ValueError(f"{name} must be true or false; got {value!r}")


def _parse_bounded_int_launch_value(
    value: str,
    *,
    name: str,
    minimum: int,
    maximum: int,
) -> int:
    try:
        parsed = int(value.strip())
    except ValueError as exc:
        raise ValueError(f"{name} must be an integer") from exc
    if parsed < minimum or parsed > maximum:
        raise ValueError(f"{name} must be in [{minimum}, {maximum}]")
    return parsed


def _parse_cpu_affinity_launch_value(value: str, *, name: str) -> list[int]:
    maximum_cpu_index = 1023
    normalized = value.strip()
    if not normalized:
        return []
    cpus: set[int] = set()
    for raw_token in normalized.split(","):
        token = raw_token.strip()
        if not token:
            raise ValueError(f"{name} contains an empty CPU token")
        if "-" not in token:
            cpus.add(
                _parse_bounded_int_launch_value(
                    token,
                    name=name,
                    minimum=0,
                    maximum=maximum_cpu_index,
                )
            )
            continue
        pieces = token.split("-")
        if len(pieces) != 2:
            raise ValueError(f"{name} contains an invalid CPU range: {token!r}")
        first = _parse_bounded_int_launch_value(
            pieces[0],
            name=name,
            minimum=0,
            maximum=maximum_cpu_index,
        )
        last = _parse_bounded_int_launch_value(
            pieces[1],
            name=name,
            minimum=0,
            maximum=maximum_cpu_index,
        )
        if last < first:
            raise ValueError(f"{name} contains a descending CPU range: {token!r}")
        cpus.update(range(first, last + 1))
    return sorted(cpus)
