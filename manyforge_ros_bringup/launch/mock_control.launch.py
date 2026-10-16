"""Run retained mock ros2_control artifacts under Manager ownership.

The deployment generator owns joint order, startup pose and controller values.
This launch starts observation and loads a configured arm controller inactive; the
existing component lifecycle owner activates it and the optional authored gripper
controller. Both load inactive. This launch owns
no motion policy.
"""

from __future__ import annotations

from pathlib import Path
import xml.etree.ElementTree as ET

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, EmitEvent, OpaqueFunction
from launch.events import Shutdown
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def _artifact(context, name: str) -> Path:
    path = Path(LaunchConfiguration(name).perform(context))
    if not path.is_absolute() or not path.is_file():
        raise ValueError(f"{name} must identify the retained absolute file")
    return path


def launch_setup(context, *args, **kwargs):
    description_path = _artifact(context, "robot_description_file")
    controllers_path = _artifact(context, "controllers_file")
    description = description_path.read_text(encoding="utf-8")
    controls = ET.fromstring(description).findall("./ros2_control")
    if not controls or any(
        control.findtext("./hardware/plugin") != "mock_components/GenericSystem"
        for control in controls
    ):
        raise ValueError("mock bringup requires GenericSystem hardware")

    namespace = LaunchConfiguration("runtime_namespace").perform(context)
    manager_name = LaunchConfiguration("controller_manager_name").perform(context)
    arm_name = LaunchConfiguration("arm_controller_name").perform(context)
    reset_position_name = LaunchConfiguration("reset_position_controller_name").perform(context)
    gripper_name = LaunchConfiguration("gripper_controller_name").perform(context)
    frame_prefix = LaunchConfiguration("frame_prefix").perform(context)
    manager_path = f"{namespace.rstrip('/')}/{manager_name}"
    nodes = [
        Node(
            package="robot_state_publisher",
            executable="robot_state_publisher",
            namespace=namespace,
            parameters=[{
                "robot_description": description, "use_sim_time": False,
                "frame_prefix": frame_prefix,
            }],
            output="screen",
            ros_arguments=["--disable-external-lib-logs"],
            on_exit=EmitEvent(event=Shutdown(reason="Mock robot_state_publisher exited")),
        ),
        Node(
            package="controller_manager",
            executable="ros2_control_node",
            name=manager_name,
            namespace=namespace,
            parameters=[str(controllers_path), {"use_sim_time": False}],
            remappings=[("~/robot_description", "robot_description")],
            output="screen",
            ros_arguments=["--disable-external-lib-logs"],
            on_exit=EmitEvent(event=Shutdown(reason="Mock controller_manager exited")),
        ),
        Node(
            package="controller_manager",
            executable="spawner",
            arguments=["joint_state_broadcaster", "--controller-manager", manager_path],
            output="screen",
            ros_arguments=["--disable-external-lib-logs"],
        ),
    ]
    if arm_name:
        nodes.append(
            Node(
                package="controller_manager",
                executable="spawner",
                arguments=[arm_name, "--inactive", "--controller-manager", manager_path, "--param-file", str(controllers_path)],
                output="screen",
                ros_arguments=["--disable-external-lib-logs"],
            )
        )
    if reset_position_name:
        nodes.append(
            Node(
                package="controller_manager",
                executable="spawner",
                arguments=[reset_position_name, "--inactive", "--controller-manager", manager_path, "--param-file", str(controllers_path)],
                output="screen",
                ros_arguments=["--disable-external-lib-logs"],
            )
        )
    if gripper_name:
        nodes.append(
            Node(
                package="controller_manager",
                executable="spawner",
                arguments=[gripper_name, "--inactive", "--controller-manager", manager_path, "--param-file", str(controllers_path)],
                output="screen",
                ros_arguments=["--disable-external-lib-logs"],
            )
        )
    return nodes


def generate_launch_description() -> LaunchDescription:
    return LaunchDescription(
        [
            DeclareLaunchArgument("robot_description_file"),
            DeclareLaunchArgument("controllers_file"),
            DeclareLaunchArgument("runtime_namespace"),
            DeclareLaunchArgument("controller_manager_name", default_value="controller_manager"),
            DeclareLaunchArgument("arm_controller_name", default_value=""),
            DeclareLaunchArgument("reset_position_controller_name", default_value=""),
            DeclareLaunchArgument("gripper_controller_name", default_value=""),
            DeclareLaunchArgument("frame_prefix", default_value=""),
            OpaqueFunction(function=launch_setup),
        ]
    )
