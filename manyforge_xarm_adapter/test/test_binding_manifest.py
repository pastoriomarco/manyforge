from __future__ import annotations

import hashlib
import json
from pathlib import Path

from manyforge_xarm_adapter.binding_manifest import compile_manifest
from manyforge_xarm_adapter.binding_manifest import MODEL_SCHEMA
from manyforge_xarm_adapter.binding_manifest import SOURCE_SCHEMA
from manyforge_xarm_adapter.controller_binding_validation import (
    validate_controller_binding,
)
from manyforge_xarm_adapter.model_generation import supervised_runtime_urdf
from manyforge_xarm_adapter.profile import load_profile
import yaml


_PROFILE = Path(__file__).resolve().parents[1] / 'config' / (
    'lite6_fixed_pneumatic.profile.yaml'
)


def test_binding_manifest_retains_robot_family_but_no_unit_identity_or_address(
    tmp_path: Path,
) -> None:
    content_sha = 'a' * 64
    source_path = tmp_path / 'source.json'
    source_path.write_text(
        json.dumps(
            {
                'schema': SOURCE_SCHEMA,
                'repository': 'https://github.com/pastoriomarco/xarm_ros2',
                'commit': 'b' * 40,
                'tree': 'c' * 40,
                'content_sha256': content_sha,
                'dirty': False,
                'xc4_base_commit': (
                    '50064477f39ecd69ab006488630c837cdcb8d83e'
                ),
            }
        ),
        encoding='utf-8',
    )
    model_path = tmp_path / 'model.json'
    profile = load_profile(_PROFILE)
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
    runtime = supervised_runtime_urdf(
        f"""\
<robot name="fixture">
  <link name="world"/>
  <ros2_control name="vendor" type="system">
    <hardware>
      <plugin>uf_robot_hardware/UFRobotSystemHardware</plugin>
    </hardware>
{control_joints}
  </ros2_control>
</robot>
""",
        profile,
        test_only_fake=False,
    )
    controllers = yaml.safe_dump(
        {
            'controller_manager': {
                'ros__parameters': {
                    'update_rate': 150,
                    'joint_state_broadcaster': {
                        'type': (
                            'joint_state_broadcaster/'
                            'JointStateBroadcaster'
                        )
                    },
                    'lite6_traj_controller': {
                        'type': (
                            'joint_trajectory_controller/'
                            'JointTrajectoryController'
                        )
                    },
                }
            },
            'lite6_traj_controller': {
                'ros__parameters': {
                    'command_interfaces': ['position'],
                    'state_interfaces': ['position', 'velocity'],
                    'joints': [
                        f'joint{index}' for index in range(1, 7)
                    ],
                    'state_publish_rate': 25.0,
                    'action_monitor_rate': 10.0,
                    'constraints': {
                        'decelerate_on_cancel': True,
                        **{
                            f'joint{index}': {
                                'max_deceleration_on_cancel': 2.0,
                            }
                            for index in range(1, 7)
                        },
                    },
                }
            },
        },
        sort_keys=False,
    )
    runtime_name = 'lite6_fixed_pneumatic.supervised.runtime.urdf'
    controllers_name = 'ros2_controllers.yaml'
    (tmp_path / runtime_name).write_text(runtime, encoding='utf-8')
    (tmp_path / controllers_name).write_text(
        controllers,
        encoding='utf-8',
    )
    files = [
        {
            'role': 'supervised_runtime_urdf_template',
            'path': runtime_name,
            'sha256': hashlib.sha256(runtime.encode()).hexdigest(),
        },
        {
            'role': 'ros2_controllers',
            'path': controllers_name,
            'sha256': hashlib.sha256(controllers.encode()).hexdigest(),
        },
    ]
    model_path.write_text(
        json.dumps(
            {
                'schema': MODEL_SCHEMA,
                'generation_id': 'd' * 64,
                'profile_id': 'lite6_fixed_pneumatic_v1',
                'source': {'content_sha256': content_sha},
                'files': files,
            }
        ),
        encoding='utf-8',
    )

    manifest = compile_manifest(
        profile_path=_PROFILE,
        model_manifest_path=model_path,
        source_manifest_path=source_path,
    )
    serialized = json.dumps(manifest, sort_keys=True)

    assert manifest['network'] == {
        'address_source': 'environment:MANYFORGE_LITE6_ROBOT_IP',
        'address_retained': False,
    }
    assert manifest['robot_family'] == {
        'expected_axis': 6,
        'expected_device_type': 9,
    }
    assert manifest['command_surface']['motion_admission'] == 'closed'
    assert (
        manifest['command_surface']['read_only_driver_command_endpoints']
        == 'suppressed'
    )
    assert manifest['controller_binding']['controller_manager'] == (
        '/controller_manager'
    )
    assert manifest['controller_binding']['hardware_component_name'] == (
        'lite6_system'
    )
    assert manifest['controller_binding']['decelerate_on_cancel'] is True
    binding_path = tmp_path / 'binding.json'
    binding_path.write_text(json.dumps(manifest), encoding='utf-8')
    validation = validate_controller_binding(
        binding_manifest_path=binding_path,
        model_root=tmp_path,
    )
    assert validation['result'] == 'pass'
    assert '192.168.' not in serialized
    assert 'serial' not in serialized.lower()
