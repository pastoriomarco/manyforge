"""Attach an Isaac tool component (a gripper) to an already-open Isaac scene.

The tool's joints are part of the robot's Isaac articulation and its state is
on the shared joint-states topic, so the tool has no controller manager. This
launch publishes the tool's description in the tool's own namespace and, for
an action transport, runs the thin ParallelGripperCommand bridge there. The retained
C++ backend owns command meaning, lifecycle admission, timeouts and Stop. The
host passes every value; this launch only reads them.
"""

from __future__ import annotations

from pathlib import Path

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, EmitEvent, OpaqueFunction
from launch.events import Shutdown
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node

from manyforge_ros_bringup.launch_signals import keep_sigint_out_of_tasks


def generate_launch_description() -> LaunchDescription:
    return LaunchDescription(
        [
            DeclareLaunchArgument(
                "robot_description_file",
                description="Host-written description of the tool's model.",
            ),
            DeclareLaunchArgument(
                "runtime_namespace",
                description="Exact /manyforge/<tool controllable_component_id>.",
            ),
            DeclareLaunchArgument("frame_prefix", default_value=""),
            DeclareLaunchArgument(
                "gripper_bridge_parameters",
                default_value="",
                description=(
                    "The absolute bridge parameters file (the Isaac lane's "
                    "gripper_bridges/); empty (a direct transport) starts no "
                    "bridge."
                ),
            ),
            DeclareLaunchArgument("gripper_action_name", default_value=""),
            DeclareLaunchArgument("gripper_command_topic", default_value=""),
            DeclareLaunchArgument("gripper_state_topic", default_value=""),
            DeclareLaunchArgument(
                "gripper_readiness_topic",
                default_value="manyforge/isaac_gripper_bridge/ready",
            ),
            DeclareLaunchArgument(
                "gripper_state_freshness_timeout_s",
                default_value="",
                description=(
                    "The budget [s] of the tool's gripper_feedback stream; "
                    "required with a bridge."
                ),
            ),
            OpaqueFunction(function=_launch_setup),
        ]
    )


def _launch_setup(context, *_, **__) -> list[object]:
    keep_sigint_out_of_tasks(context)  # Before any Node action exists.

    def value(name: str) -> str:
        return LaunchConfiguration(name).perform(context).strip()

    namespace = value("runtime_namespace")
    path = Path(value("robot_description_file"))
    try:
        description = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise ValueError(
            f"robot_description_file must be a readable UTF-8 file: {path}"
        ) from error
    actions: list[object] = [
        Node(
            package="robot_state_publisher",
            executable="robot_state_publisher",
            namespace=namespace,
            output="screen",
            ros_arguments=["--disable-external-lib-logs"],
            parameters=[
                {"robot_description": description, "frame_prefix": value("frame_prefix")}
            ],
        )
    ]
    bridge_parameters = value("gripper_bridge_parameters")
    if bridge_parameters:
        bridge_path = Path(bridge_parameters)
        if not bridge_path.is_absolute() or not bridge_path.is_file():
            raise ValueError(
                f"gripper_bridge_parameters must be an absolute file: {bridge_path}"
            )
        actions.append(
            Node(
                package="manyforge_ros_bringup",
                executable="isaac_gripper_action_bridge",
                name="manyforge_isaac_gripper_action_bridge",
                namespace=namespace,
                output="screen",
                ros_arguments=["--disable-external-lib-logs"],
                parameters=[
                    str(bridge_path),
                    {
                        "action_name": f"{namespace}/{value('gripper_action_name')}",
                        "command_topic": value("gripper_command_topic"),
                        "state_topic": value("gripper_state_topic"),
                        "readiness_topic": (
                            f"{namespace}/{value('gripper_readiness_topic')}"
                        ),
                        "state_freshness_timeout_s": float(
                            value("gripper_state_freshness_timeout_s")
                        ),
                    },
                ],
                on_exit=EmitEvent(
                    event=Shutdown(reason="Isaac gripper transport adapter exited")
                ),
            )
        )
    return actions
