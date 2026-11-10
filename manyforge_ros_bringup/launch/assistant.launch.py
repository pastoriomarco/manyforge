"""Start the host of one ManyForge ``assistant`` component (the chat bridge).

ros2 launch manyforge_ros_bringup assistant.launch.py namespace:=<ns>
    backend:=<backend> health_period_s:=<s>

The execution host's ``assistant`` adapter starts this at the component's
Prepare with its effective settings and supervises it; the host and its bridge
read the Headless and Composer URLs (and the model token variables) from the
environment they inherit.
"""

from __future__ import annotations

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node

from manyforge_ros_bringup.launch_signals import keep_sigint_out_of_tasks

ARGUMENTS = ("namespace", "backend", "health_period_s")


def node_kwargs(namespace: str, backend: str, health_period_s: str) -> dict:
    if not namespace.startswith("/"):
        raise ValueError("namespace must be an absolute ROS name")
    if not backend:
        raise ValueError("backend must not be empty")
    return {
        "package": "manyforge_ros_bringup",
        "executable": "assistant_host",
        "name": "assistant_host",
        "namespace": namespace,
        "output": "screen",
        "ros_arguments": ["--disable-external-lib-logs"],
        "parameters": [{"backend": backend, "health_period_s": float(health_period_s)}],
    }


def launch_setup(context, *args, **kwargs):
    keep_sigint_out_of_tasks(context)  # Before any Node action exists.
    values = [LaunchConfiguration(name).perform(context) for name in ARGUMENTS]
    return [Node(**node_kwargs(*values))]


def generate_launch_description() -> LaunchDescription:
    return LaunchDescription([
        *(DeclareLaunchArgument(name) for name in ARGUMENTS),
        OpaqueFunction(function=launch_setup),
    ])
