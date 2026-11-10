"""Start one ManyForge simulated PLC for an I/O device.

ros2 launch manyforge_ros_bringup sim_plc.launch.py device:=<device_id> config_dir:=<dir>

The node reads its own optional ``<config_dir>/<device_id>.yaml``; this launch
only names it and keeps its discovery on the local host.

As an I/O provider it follows the observed-value contract like
a real ROS PLC, with nothing simulator-specific:

- ManyForge outputs are the PLC inputs ``in_<i>`` (SetBool). The PLC has no
  read-back of them, so the component declares ``io_manual_write`` without
  ``io_output_readback``. Their samples are ``source: not_readable``: no value
  is shown, never the last write. A write without a response is ``unknown``
  and is never resent.
- ManyForge inputs are the PLC outputs ``out_<i>`` (Bool topics). They are
  sampled with ``source: device`` and their age.
- ``ready`` is the device's stream. When it goes stale, the I/O component is
  in ``fault_stream_stale``.
"""

from __future__ import annotations

import re

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node

from manyforge_ros_bringup.launch_signals import keep_sigint_out_of_tasks

_DEVICE_RE = re.compile(r"[A-Za-z][A-Za-z0-9_]*\Z")


def node_kwargs(device: str, config_dir: str) -> dict:
    if not _DEVICE_RE.match(device):
        raise ValueError(f"device '{device}' must match [A-Za-z][A-Za-z0-9_]*")
    if not config_dir:
        raise ValueError("config_dir must be set")
    return {
        "package": "manyforge_ros_bringup",
        "executable": "sim_plc",
        "name": f"sim_plc_{device}",
        "namespace": "/manyforge",
        "arguments": ["--device", device, "--config-dir", config_dir],
        "ros_arguments": ["--disable-external-lib-logs"],
        "additional_env": {"ROS_AUTOMATIC_DISCOVERY_RANGE": "LOCALHOST"},
        "output": "screen",
    }


def launch_setup(context, *args, **kwargs):
    keep_sigint_out_of_tasks(context)  # Before any Node action exists.
    device = LaunchConfiguration("device").perform(context)
    config_dir = LaunchConfiguration("config_dir").perform(context)
    return [Node(**node_kwargs(device, config_dir))]


def generate_launch_description() -> LaunchDescription:
    return LaunchDescription([
        DeclareLaunchArgument("device", description="I/O device id, e.g. plc_io"),
        DeclareLaunchArgument(
            "config_dir", description="directory holding the optional <device>.yaml"
        ),
        OpaqueFunction(function=launch_setup),
    ])
