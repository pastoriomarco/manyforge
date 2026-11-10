"""Start the reachability probe of one ManyForge ``ai_model`` component.

ros2 launch manyforge_ros_bringup ai_model.launch.py site_config_path:=<file>

The execution host writes the site document from the Deployment and supervises
this launch; the probe reads only that document (and the token variable it
names, inherited from the host environment).
"""

from __future__ import annotations

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node

from manyforge_ros_bringup.launch_signals import keep_sigint_out_of_tasks


def node_kwargs(site_config_path: str) -> dict:
    if not site_config_path.startswith("/"):
        raise ValueError("site_config_path must be an absolute path")
    return {
        "package": "manyforge_ros_bringup",
        "executable": "ai_model_probe",
        "output": "screen",
        "ros_arguments": ["--disable-external-lib-logs"],
        "parameters": [{"site_config_path": site_config_path}],
    }


def launch_setup(context, *args, **kwargs):
    keep_sigint_out_of_tasks(context)  # Before any Node action exists.
    return [
        Node(**node_kwargs(LaunchConfiguration("site_config_path").perform(context)))
    ]


def generate_launch_description() -> LaunchDescription:
    return LaunchDescription([
        DeclareLaunchArgument(
            "site_config_path", description="probe site document (absolute)"
        ),
        OpaqueFunction(function=launch_setup),
    ])
