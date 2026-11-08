"""Start the observer of one ManyForge ``pose_source`` component.

ros2 launch manyforge_ros_bringup pose_source.launch.py \
    site_config_path:=<file> manager_run_root:=<dir> source_epoch_base:=<base>

The execution host writes the site document from the Deployment and supervises
this launch; the observer reads only that document.
"""

from __future__ import annotations

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node

from manyforge_ros_bringup.launch_signals import keep_sigint_out_of_tasks


def node_kwargs(
    site_config_path: str, manager_run_root: str, source_epoch_base: str
) -> dict:
    for name, value in (
        ("site_config_path", site_config_path),
        ("manager_run_root", manager_run_root),
    ):
        if not value.startswith("/"):
            raise ValueError(f"{name} must be an absolute path")
    if not source_epoch_base:
        raise ValueError("source_epoch_base must be set")
    return {
        "package": "manyforge_ros_bringup",
        "executable": "pose_source_snapshot_publisher",
        "output": "screen",
        "ros_arguments": ["--disable-external-lib-logs"],
        "parameters": [
            {
                "site_config_path": site_config_path,
                "manager_run_root": manager_run_root,
                "source_epoch_base": source_epoch_base,
            }
        ],
    }


def launch_setup(context, *args, **kwargs):
    keep_sigint_out_of_tasks(context)  # Before any Node action exists.
    return [
        Node(
            **node_kwargs(
                LaunchConfiguration("site_config_path").perform(context),
                LaunchConfiguration("manager_run_root").perform(context),
                LaunchConfiguration("source_epoch_base").perform(context),
            )
        )
    ]


def generate_launch_description() -> LaunchDescription:
    return LaunchDescription([
        DeclareLaunchArgument(
            "site_config_path", description="observer site document (absolute)"
        ),
        DeclareLaunchArgument(
            "manager_run_root", description="Manager run root (absolute)"
        ),
        DeclareLaunchArgument(
            "source_epoch_base",
            description="per-spawn source epoch base <component>.<token>",
        ),
        OpaqueFunction(function=launch_setup),
    ])
