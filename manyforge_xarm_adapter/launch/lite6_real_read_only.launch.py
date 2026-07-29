#!/usr/bin/env python3
"""Start the experimental Lite6 observation surface without command ownership."""

from __future__ import annotations

import ipaddress
from pathlib import Path

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.actions import OpaqueFunction
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node
from manyforge_xarm_adapter.binding import compile_lite6_binding
from manyforge_xarm_adapter.binding import Lite6BindingInputs


def _boolean(value: str, label: str) -> bool:
    normalized = value.strip().lower()
    if normalized in {'1', 'true', 'yes', 'on'}:
        return True
    if normalized in {'0', 'false', 'no', 'off'}:
        return False
    raise ValueError(f'{label} must be true or false')


def _launch_setup(context, *args, **kwargs):
    del args, kwargs
    connect = _boolean(
        LaunchConfiguration('connect_to_robot').perform(context),
        'connect_to_robot',
    )
    robot_ip = LaunchConfiguration('robot_ip').perform(context).strip()
    report_type = LaunchConfiguration('report_type').perform(context).strip()
    if report_type != 'rich':
        raise ValueError("report_type must be 'rich' for the XC-6 observer")
    if connect:
        if not robot_ip:
            raise ValueError('robot_ip is required when connect_to_robot=true')
        try:
            parsed_address = ipaddress.ip_address(robot_ip)
        except ValueError as exc:
            raise ValueError('robot_ip must be a literal IPv4 address') from exc
        if parsed_address.version != 4:
            raise ValueError('robot_ip must be an IPv4 address')

    model_path = Path(
        LaunchConfiguration('model_urdf_path').perform(context)
    ).resolve()
    if not model_path.is_file():
        raise FileNotFoundError(f'generated Lite6 model is missing: {model_path}')
    robot_description = model_path.read_text(encoding='utf-8')

    inputs = Lite6BindingInputs(
        ros_namespace=LaunchConfiguration('ros_namespace').perform(context),
        prefix=LaunchConfiguration('prefix').perform(context),
        hardware_namespace=LaunchConfiguration('hardware_namespace').perform(
            context
        ),
        controller_name=LaunchConfiguration('controller_name').perform(context),
    )
    binding = compile_lite6_binding(inputs)
    namespace = binding.ros_namespace
    driver_parameters = {
        'robot_ip': robot_ip,
        'report_type': report_type,
        'dof': 6,
        'robot_type': 'lite',
        'prefix': binding.prefix,
        'hw_ns': binding.hardware_namespace,
        'add_gripper': False,
        'add_bio_gripper': False,
        'add_vacuum_gripper': False,
        'joint_states_rate': 25,
        'read_only': True,
        'services.debug': False,
        'expected_robot_device_type': 9,
    }

    nodes = [
        Node(
            package='robot_state_publisher',
            executable='robot_state_publisher',
            name='lite6_robot_state_publisher',
            namespace=namespace,
            parameters=[{'robot_description': robot_description}],
            remappings=[
                ('joint_states', binding.validated_joint_state_topic)
            ],
            output='screen',
        ),
        Node(
            package='manyforge_xarm_adapter',
            executable='manyforge-xarm-read-only-observer',
            name='manyforge_xarm_read_only_observer',
            namespace=namespace,
            parameters=[
                {
                    'robot_state_topic': binding.robot_state_topic,
                    'joint_state_topic': binding.joint_state_topic,
                    'validated_joint_state_topic': (
                        binding.validated_joint_state_topic
                    ),
                    'expected_joint_names': list(binding.arm_joint_names),
                    'diagnostics_topic': binding.diagnostics_topic,
                    'stale_after_s': 0.75,
                    'source_agreement_tolerance_rad': 0.002,
                }
            ],
            output='screen',
        ),
    ]
    if connect:
        nodes.insert(
            0,
            Node(
                package='xarm_api',
                executable='xarm_driver_node',
                name='ufactory_driver',
                namespace=namespace,
                parameters=[driver_parameters],
                output='screen',
                emulate_tty=True,
            ),
        )
    return nodes


def generate_launch_description():
    """Build a launch whose default cannot contact or command the robot."""
    return LaunchDescription(
        [
            DeclareLaunchArgument(
                'connect_to_robot',
                default_value='false',
                description='Explicitly connect the enforced read-only xArm driver.',
            ),
            DeclareLaunchArgument(
                'robot_ip',
                default_value='',
                description='Runtime-only Lite6 IPv4 address; never retained.',
            ),
            DeclareLaunchArgument('report_type', default_value='rich'),
            DeclareLaunchArgument('ros_namespace', default_value=''),
            DeclareLaunchArgument('prefix', default_value=''),
            DeclareLaunchArgument('hardware_namespace', default_value='ufactory'),
            DeclareLaunchArgument(
                'controller_name',
                default_value='lite6_traj_controller',
            ),
            DeclareLaunchArgument(
                'model_urdf_path',
                default_value=(
                    '/workspace/.manyforge-var/real-lite6/model/'
                    'lite6_fixed_pneumatic.model.urdf'
                ),
            ),
            OpaqueFunction(function=_launch_setup),
        ]
    )
