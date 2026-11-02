"""Start the point-cloud bridge of one ManyForge point-cloud source component.

ros2 launch manyforge_ros_bringup pointcloud_source.launch.py namespace:=<ns>
    topic:=<topic> source_frame_id:=<frame> cell_id:=<cell>
    max_points:=<n> minimum_period_s:=<s>

The execution host's `pointcloud` adapter starts this at the component's
Prepare with its effective settings and supervises it; the bridge reads the
Composer base URL and owner id from the environment it inherits.
"""

from __future__ import annotations

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node

from manyforge_ros_bringup.launch_signals import keep_sigint_out_of_tasks

ARGUMENTS = (
    "namespace",
    "topic",
    "source_frame_id",
    "cell_id",
    "max_points",
    "minimum_period_s",
)


def node_kwargs(
    namespace: str,
    topic: str,
    source_frame_id: str,
    cell_id: str,
    max_points: str,
    minimum_period_s: str,
) -> dict:
    for name, value in (("namespace", namespace), ("topic", topic)):
        if not value.startswith("/"):
            raise ValueError(f"{name} must be an absolute ROS name")
    for name, value in (("source_frame_id", source_frame_id), ("cell_id", cell_id)):
        if not value:
            raise ValueError(f"{name} must not be empty")
    return {
        "package": "manyforge_ros_bringup",
        "executable": "pointcloud_observer_bridge",
        "name": "pointcloud_observer_bridge",
        "namespace": namespace,
        "output": "screen",
        "ros_arguments": ["--disable-external-lib-logs"],
        "parameters": [
            {
                "topic": topic,
                "source_frame_id": source_frame_id,
                "cell_id": cell_id,
                "max_points": int(max_points),
                "minimum_period_s": float(minimum_period_s),
            }
        ],
    }


def launch_setup(context, *args, **kwargs):
    keep_sigint_out_of_tasks(context)  # Q116: before any Node action exists.
    values = [LaunchConfiguration(name).perform(context) for name in ARGUMENTS]
    return [Node(**node_kwargs(*values))]


def generate_launch_description() -> LaunchDescription:
    return LaunchDescription([
        *(DeclareLaunchArgument(name) for name in ARGUMENTS),
        OpaqueFunction(function=launch_setup),
    ])
