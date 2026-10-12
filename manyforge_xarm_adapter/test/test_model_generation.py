from __future__ import annotations

import json
from pathlib import Path
import xml.etree.ElementTree as ET

from manyforge_xarm_adapter.controller_binding import (
    SUPERVISED_FAKE_HARDWARE_PLUGIN,
)
from manyforge_xarm_adapter.model_generation import _load_source_manifest
from manyforge_xarm_adapter.model_generation import (
    apply_manyforge_jtc_qualification,
)
from manyforge_xarm_adapter.model_generation import (
    MANYFORGE_CANCEL_DECELERATION_RAD_S2,
)
from manyforge_xarm_adapter.model_generation import model_only_urdf
from manyforge_xarm_adapter.model_generation import SOURCE_SCHEMA
from manyforge_xarm_adapter.model_generation import supervised_runtime_urdf
from manyforge_xarm_adapter.profile import load_profile
import pytest


def test_model_only_urdf_removes_execution_blocks_only() -> None:
    runtime = """\
<robot name="fixture">
  <link name="world"/>
  <link name="tool"/>
  <joint name="tool_fix" type="fixed">
    <parent link="world"/>
    <child link="tool"/>
  </joint>
  <ros2_control name="system" type="system">
    <hardware><plugin>vendor/System</plugin></hardware>
  </ros2_control>
  <transmission name="joint_transmission"/>
  <gazebo><plugin name="runtime"/></gazebo>
</robot>
"""

    model = model_only_urdf(runtime)
    root = ET.fromstring(model)

    assert root.find("./link[@name='world']") is not None
    assert root.find("./link[@name='tool']") is not None
    assert root.find("./joint[@name='tool_fix']") is not None
    assert root.find('./ros2_control') is None
    assert root.find('./transmission') is None
    assert root.find('./gazebo') is None


def test_supervised_runtime_changes_only_execution_owner() -> None:
    profile_path = Path(__file__).resolve().parents[1] / 'config' / (
        'lite6_fixed_pneumatic.profile.yaml'
    )
    profile = load_profile(profile_path)
    links = ''.join(
        f'  <link name="link{index}"/>\n' for index in range(1, 7)
    )
    joints = ''.join(
        f"""\
  <joint name="joint{index}" type="revolute">
    <parent link="world"/>
    <child link="link{index}"/>
  </joint>
"""
        for index in range(1, 7)
    )
    control_joints = ''.join(
        f"""\
    <joint name="joint{index}">
      <command_interface name="position"/>
      <command_interface name="velocity"/>
      <state_interface name="position"/>
      <state_interface name="velocity"/>
    </joint>
"""
        for index in range(1, 7)
    )
    runtime = f"""\
<robot name="fixture">
  <link name="world"/>
{links}
{joints}
  <ros2_control name="vendor" type="system">
    <hardware>
      <plugin>uf_robot_hardware/UFRobotSystemHardware</plugin>
    </hardware>
{control_joints}
  </ros2_control>
</robot>
"""

    production = supervised_runtime_urdf(
        runtime,
        profile,
        test_only_fake=False,
    )
    qualification = supervised_runtime_urdf(
        runtime,
        profile,
        test_only_fake=True,
    )

    assert model_only_urdf(production) == model_only_urdf(runtime)
    assert model_only_urdf(qualification) == model_only_urdf(runtime)
    assert '__MANYFORGE_RUNTIME_ROBOT_IP__' in production
    assert 'expected_robot_sn' not in production
    assert SUPERVISED_FAKE_HARDWARE_PLUGIN not in production
    assert SUPERVISED_FAKE_HARDWARE_PLUGIN in qualification
    assert 'authority_profile' not in qualification
    assert 'authority_session' not in qualification
    qualification_hardware = ET.fromstring(qualification).find(
        './ros2_control/hardware'
    )
    assert qualification_hardware is not None
    assert {
        parameter.get('name'): parameter.text
        for parameter in qualification_hardware.findall('./param')
    } == {
        'mock_sensor_commands': 'false',
        'calculate_dynamics': 'false',
    }


def test_source_manifest_rejects_non_exact_tree_identity(
    tmp_path: Path,
) -> None:
    manifest = tmp_path / 'source.json'
    manifest.write_text(
        json.dumps(
            {
                'schema': SOURCE_SCHEMA,
                'repository': 'https://github.com/pastoriomarco/xarm_ros2',
                'commit': 'a' * 40,
                'tree': 'not-a-tree',
                'content_sha256': 'b' * 64,
                'dirty': True,
                'xc4_base_commit': 'c' * 40,
                'xc4_base_tree': 'd' * 40,
                'qualification': 'local_experimental',
            }
        ),
        encoding='utf-8',
    )

    with pytest.raises(ValueError, match='tree must be 40 lowercase hex digits'):
        _load_source_manifest(manifest)


def test_source_manifest_rejects_dirty_xarm_input(tmp_path: Path) -> None:
    manifest = tmp_path / 'source.json'
    manifest.write_text(
        json.dumps(
            {
                'schema': SOURCE_SCHEMA,
                'repository': 'https://github.com/pastoriomarco/xarm_ros2',
                'commit': 'a' * 40,
                'tree': 'b' * 40,
                'content_sha256': 'c' * 64,
                'dirty': True,
                'xc4_base_commit': 'd' * 40,
                'xc4_base_tree': 'e' * 40,
                'qualification': 'local_experimental',
            }
        ),
        encoding='utf-8',
    )

    with pytest.raises(ValueError, match='clean xArm worktree'):
        _load_source_manifest(manifest)


def test_manyforge_jtc_qualification_preserves_vendor_constraints() -> None:
    profile_path = Path(__file__).resolve().parents[1] / 'config' / (
        'lite6_fixed_pneumatic.profile.yaml'
    )
    profile = load_profile(profile_path)
    joints = [f'joint{index}' for index in range(1, 7)]
    source = {
        'lite6_traj_controller': {
            'ros__parameters': {
                'joints': joints,
                'command_interfaces': ['position', 'velocity'],
                'state_interfaces': ['position', 'velocity'],
                'constraints': {
                    'goal_time': 0.5,
                    **{
                        joint: {
                            'goal': 0.01,
                            'trajectory': 1.0,
                        }
                        for joint in joints
                    },
                },
            },
        },
    }

    result = apply_manyforge_jtc_qualification(source, profile)
    constraints = result['lite6_traj_controller']['ros__parameters'][
        'constraints'
    ]

    assert result is not source
    assert result['lite6_traj_controller']['ros__parameters'][
        'command_interfaces'
    ] == ['position']
    assert result['lite6_traj_controller']['ros__parameters'][
        'state_interfaces'
    ] == ['position', 'velocity']
    assert source['lite6_traj_controller']['ros__parameters'][
        'command_interfaces'
    ] == ['position', 'velocity']
    assert 'decelerate_on_cancel' not in source[
        'lite6_traj_controller'
    ]['ros__parameters']['constraints']
    assert constraints['goal_time'] == 0.5
    assert constraints['decelerate_on_cancel'] is True
    for joint in joints:
        assert constraints[joint] == {
            'goal': 0.01,
            'trajectory': 1.0,
            'max_deceleration_on_cancel': (
                MANYFORGE_CANCEL_DECELERATION_RAD_S2
            ),
        }
