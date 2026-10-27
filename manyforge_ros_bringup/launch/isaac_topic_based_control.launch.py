"""Attach the retained ros2_control path to an already-open Isaac scene.

The launch owns no scene loading, simulation control, motion, or component
lifecycle policy. It starts controller infrastructure inactive; the existing
CCLC path activates it only after fresh backend readiness is observed.
"""

from __future__ import annotations

import math
from pathlib import Path

from launch import LaunchDescription
from launch.actions import (
    DeclareLaunchArgument,
    EmitEvent,
    OpaqueFunction,
    TimerAction,
)
from launch.events import Shutdown
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare

from manyforge_ros_bringup.launch_signals import keep_sigint_out_of_tasks


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
                "robot_description_file",
                description="Host-generated complete robot description file.",
            ),
            DeclareLaunchArgument(
                "controllers_file",
                description="Host-generated controller parameters file.",
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
                "gripper_state_freshness_timeout_s",
                default_value="",
                description=(
                    "Host-passed feedback staleness threshold [s]: the "
                    "budget of the component's gripper_feedback stream; "
                    "required when the gripper action adapter starts."
                ),
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
            DeclareLaunchArgument("frame_prefix", default_value=""),
            DeclareLaunchArgument("base_xyz", default_value="0 0 0"),
            DeclareLaunchArgument("base_rpy", default_value="0 0 0"),
            OpaqueFunction(function=_launch_setup),
        ]
    )


def _launch_setup(context, *_, **__) -> list[object]:
    keep_sigint_out_of_tasks(context)  # Q116: before any Node action exists.
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
    robot_description_path = Path(
        LaunchConfiguration("robot_description_file").perform(context)
    )
    controllers_path = Path(
        LaunchConfiguration("controllers_file").perform(context)
    )
    # The host owns generation and validation. Read supplied artifacts before
    # constructing any runtime action; never select a stored profile fallback.
    robot_description = _read_artifact(
        robot_description_path, name="robot_description_file"
    )
    _read_artifact(controllers_path, name="controllers_file")
    frame_prefix = LaunchConfiguration("frame_prefix").perform(context)

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

    robot_state_publisher = Node(
        package="robot_state_publisher",
        executable="robot_state_publisher",
        namespace=runtime_namespace,
        output="screen",
        ros_arguments=["--disable-external-lib-logs"],
        parameters=[{
            "robot_description": robot_description, "frame_prefix": frame_prefix,
        }],
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
        ros_arguments=["--disable-external-lib-logs"],
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
            frame_prefix + LaunchConfiguration("base_child_frame").perform(context),
        ],
    )
    controller_manager = Node(
        package="controller_manager",
        executable="ros2_control_node",
        name=controller_manager_name,
        namespace=runtime_namespace,
        output="screen",
        ros_arguments=["--disable-external-lib-logs"],
        parameters=[
            {"robot_description": robot_description},
            controller_manager_resources,
            str(controllers_path),
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
        ros_arguments=["--disable-external-lib-logs"],
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
        ros_arguments=["--disable-external-lib-logs"],
        arguments=[
            arm_controller_name,
            "--inactive",
            "--param-file",
            str(controllers_path),
            "--controller-manager",
            controller_manager_path,
            "--controller-manager-timeout",
            "30",
        ],
    )

    control_actions: list[object] = [
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
                ros_arguments=["--disable-external-lib-logs"],
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
                        "state_freshness_timeout_s": _bounded_float(
                            LaunchConfiguration(
                                "gripper_state_freshness_timeout_s"
                            ).perform(context),
                            name="gripper_state_freshness_timeout_s",
                            minimum=0.01,
                            maximum=60.0,
                        ),
                    },
                ],
                on_exit=_shutdown("Isaac gripper transport adapter exited"),
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
                ros_arguments=["--disable-external-lib-logs"],
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

    # Start the ROS stack against the scene the user already opened. The
    # existing host/CCLC owner activates JTC only after real Isaac readback.
    control_actions.extend(
        [
            TimerAction(period=0.5, actions=[controller_manager]),
            TimerAction(period=2.5, actions=[joint_state_broadcaster]),
            TimerAction(period=3.5, actions=[arm_controller]),
        ]
    )
    return control_actions


def _shutdown(reason: str) -> EmitEvent:
    return EmitEvent(event=Shutdown(reason=reason))


def _read_artifact(path: Path, *, name: str) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise ValueError(f"{name} must be a readable UTF-8 file: {path}") from error


def _profile_configuration(value: str) -> dict[str, str]:
    normalized = value.strip().lower()
    profiles = {
        "ur10e": {
            "name": "ur10e",
            "raw_gripper_topic": "/isaac_gripper_state",
        },
        "lite6": {
            "name": "lite6",
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
