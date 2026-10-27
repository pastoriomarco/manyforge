"""Run retained mock ros2_control artifacts under Manager ownership.

This is the shared ros2_control component launch of Mock and Real components
(CP8 step 2a); the file name stays until CP8 step 3. The generated artifacts own
the differences as data: the hardware plugin the description must name, an
optional driver parameters file and remappings for `ros2_control_node`.

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

from manyforge_ros_bringup.launch_signals import keep_sigint_out_of_tasks


def _artifact(context, name: str) -> Path:
    path = Path(LaunchConfiguration(name).perform(context))
    if not path.is_absolute() or not path.is_file():
        raise ValueError(f"{name} must identify the retained absolute file")
    return path


def _remappings(text: str) -> list[tuple[str, str]]:
    """`from=to` pairs joined by `;`, each name absolute and remapped once."""
    pairs = []
    for entry in text.split(";") if text else []:
        source, separator, target = entry.partition("=")
        if not separator or not source.startswith("/") or not target.startswith("/") \
                or "=" in target:
            raise ValueError("hardware_remappings must be ';'-joined absolute from=to names")
        pairs.append((source, target))
    if len({source for source, _ in pairs}) != len(pairs):
        raise ValueError("hardware_remappings repeats a name")
    return pairs


def launch_setup(context, *args, **kwargs):
    keep_sigint_out_of_tasks(context)  # Q116: before any Node action exists.
    description_path = _artifact(context, "robot_description_file")
    controllers_path = _artifact(context, "controllers_file")
    description = description_path.read_text(encoding="utf-8")
    controls = ET.fromstring(description).findall("./ros2_control")
    plugin = LaunchConfiguration("hardware_plugin").perform(context)
    if not controls or any(
        control.findtext("./hardware/plugin") != plugin for control in controls
    ):
        raise ValueError(f"the robot description must use only {plugin} hardware")
    driver_parameters = (
        [str(_artifact(context, "driver_parameters_file"))]
        if LaunchConfiguration("driver_parameters_file").perform(context) else []
    )
    remappings = _remappings(LaunchConfiguration("hardware_remappings").perform(context))

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
            namespace=namespace,
            parameters=[str(controllers_path), *driver_parameters, {"use_sim_time": False}],
            remappings=[("~/robot_description", "robot_description"), *remappings],
            output="screen",
            # Name only the manager node: a process-wide name would also rename
            # a hardware plugin's own nodes (the xArm `ufactory_driver`).
            ros_arguments=[
                "--disable-external-lib-logs", "-r", f"controller_manager:__node:={manager_name}",
            ],
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
            DeclareLaunchArgument(
                "hardware_plugin", default_value="mock_components/GenericSystem"
            ),
            DeclareLaunchArgument("driver_parameters_file", default_value=""),
            DeclareLaunchArgument("hardware_remappings", default_value=""),
            OpaqueFunction(function=launch_setup),
        ]
    )
