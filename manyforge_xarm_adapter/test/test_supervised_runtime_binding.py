from __future__ import annotations

import json
from pathlib import Path
import xml.etree.ElementTree as ET

from manyforge_xarm_adapter.binding import compile_lite6_binding
from manyforge_xarm_adapter.binding import Lite6BindingInputs
from manyforge_xarm_adapter.binding_manifest import BINDING_SCHEMA
from manyforge_xarm_adapter.controller_binding import (
    compile_production_controller_binding,
)
from manyforge_xarm_adapter.model_generation import model_only_urdf
from manyforge_xarm_adapter.model_generation import supervised_runtime_urdf
from manyforge_xarm_adapter.profile import load_profile
from manyforge_xarm_adapter.supervised_runtime_binding import (
    resolve_supervised_runtime_binding,
)
from manyforge_xarm_adapter.supervised_runtime_binding import (
    RUNTIME_BINDING_SCHEMA,
)
import pytest
import yaml


_PROFILE = Path(__file__).resolve().parents[1] / 'config' / (
    'lite6_fixed_pneumatic.profile.yaml'
)


def _vendor_runtime_urdf() -> str:
    joints = '\n'.join(
        f"""\
    <joint name="joint{index}">
      <command_interface name="position"/>
      <command_interface name="velocity"/>
      <state_interface name="position"/>
      <state_interface name="velocity"/>
    </joint>"""
        for index in range(1, 7)
    )
    return f"""\
<robot name="lite6">
  <link name="world"/>
  <ros2_control name="vendor" type="system">
    <hardware>
      <plugin>uf_robot_hardware/UFRobotSystemHardware</plugin>
    </hardware>
{joints}
  </ros2_control>
</robot>
"""


def _controllers_yaml() -> str:
    joints = [f'joint{index}' for index in range(1, 7)]
    return yaml.safe_dump(
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
                    'command_interfaces': ['position', 'velocity'],
                    'state_interfaces': ['position', 'velocity'],
                    'joints': joints,
                    'state_publish_rate': 25.0,
                    'action_monitor_rate': 10.0,
                    'constraints': {
                        'decelerate_on_cancel': True,
                        **{
                            joint: {
                                'max_deceleration_on_cancel': 2.0,
                            }
                            for joint in joints
                        },
                    },
                }
            },
        },
        sort_keys=False,
    )


def _binding_fixture(tmp_path: Path) -> tuple[Path, Path, str]:
    profile = load_profile(_PROFILE)
    resolved = compile_lite6_binding(Lite6BindingInputs())
    template = supervised_runtime_urdf(
        _vendor_runtime_urdf(),
        profile,
        test_only_fake=False,
    )
    controllers = _controllers_yaml()
    compiled = compile_production_controller_binding(
        resolved=resolved,
        supervised_runtime_urdf=template,
        ros2_controllers_yaml=controllers,
    )
    model_root = tmp_path / 'model'
    model_root.mkdir()
    (model_root / 'lite6_fixed_pneumatic.supervised.runtime.urdf').write_text(
        template,
        encoding='utf-8',
    )
    (model_root / 'ros2_controllers.yaml').write_text(
        controllers,
        encoding='utf-8',
    )
    manifest_path = tmp_path / 'binding.json'
    manifest_path.write_text(
        json.dumps(
            {
                'schema': BINDING_SCHEMA,
                'generation_id': 'a' * 64,
                'binding': resolved.to_dict(),
                'controller_binding': compiled.to_dict(),
            }
        ),
        encoding='utf-8',
    )
    return manifest_path, model_root, template


def _resolve(
    tmp_path: Path,
    *,
    robot_ip: str = '192.168.1.31',
    robot_instance_id: str = 'lite6-a',
    transport_owner_id: str = 'lite6-a:sole-owner',
    lifecycle_provider_instance_id: str = 'lite6-a:provider:session-1',
    authority_profile: str = 'local_operator',
):
    binding, model_root, template = _binding_fixture(tmp_path)
    output_urdf = tmp_path / 'runtime' / 'lite6-a.urdf'
    output_manifest = tmp_path / 'runtime' / 'lite6-a.json'
    result = resolve_supervised_runtime_binding(
        binding_manifest_path=binding,
        model_root=model_root,
        robot_ip=robot_ip,
        robot_instance_id=robot_instance_id,
        transport_owner_id=transport_owner_id,
        lifecycle_provider_instance_id=lifecycle_provider_instance_id,
        authority_profile=authority_profile,
        cell_id='lite6-cell',
        cell_supervisor_component_epoch='cell-supervisor-epoch-1',
        cell_supervisor_heartbeat_path=(
            tmp_path / 'cell-supervisor-heartbeat.json'
        ),
        output_urdf_path=output_urdf,
        output_manifest_path=output_manifest,
    )
    return result, output_urdf, output_manifest, template


def test_resolves_only_runtime_identity_and_preserves_binding(
    tmp_path: Path,
) -> None:
    result, output_urdf, output_manifest, template = _resolve(tmp_path)
    runtime = output_urdf.read_text(encoding='utf-8')
    root = ET.fromstring(runtime)
    parameters = {
        item.get('name'): item.text
        for item in root.findall('./ros2_control/hardware/param')
    }

    assert result['schema'] == RUNTIME_BINDING_SCHEMA
    assert result['runtime']['robot_ip'] == '192.168.1.31'
    assert result['runtime']['address_retained'] is True
    assert result['command_surface'] == {
        'transport_owner': 'xarm_ros2_ros2_control',
        'manyforge_transport': 'restricted_ros_only',
        'on_init_transport': 'closed',
        'on_configure_transport': 'observation_connect',
        'motion_admission_initial': 'closed',
        'automatic_startup_sequence': False,
        'legacy_command_endpoints': 'suppressed',
        'supervised_state_topic': (
            '/ufactory/supervised_lifecycle/state'
        ),
        'supervised_execute_service': (
            '/ufactory/supervised_lifecycle/execute'
        ),
        'supervised_command_gate_service': (
            '/ufactory/supervised_lifecycle/set_command_gate'
        ),
        'supervised_shutdown_controller_service': (
            '/ufactory/supervised_lifecycle/shutdown_controller'
        ),
    }
    assert 'backend' not in parameters
    assert 'robot_id' not in parameters
    assert parameters['transport_owner_id'] == 'lite6-a:sole-owner'
    assert 'authority_profile' not in parameters
    assert parameters['robot_ip'] == '192.168.1.31'
    assert result['runtime']['robot_instance_id'] == 'lite6-a'
    assert result['runtime']['authority_profile'] == 'local_operator'
    assert result['runtime']['lifecycle_provider_instance_id'] == (
        'lite6-a:provider:session-1'
    )
    assert result['execution_admission'] == {
        'schema': (
            'manyforge.robot_lifecycle.'
            'execution_admission.v0.experimental'
        ),
        'topic': '/manyforge_execution_admission',
        'runtime_generation_id': result['generation_id'],
        'cell_supervisor_component_epoch': (
            'cell-supervisor-epoch-1'
        ),
        'provider_instance_id': 'lite6-a:provider:session-1',
        'supervisor_instance_id': 'lite6-a:lifecycle-supervisor',
        'refresh_period_ms': 50,
        'maximum_lease_ms': 250,
    }
    assert result['robot_lifecycle_provider'][
        'authority_lease_duration_ms'
    ] == 5_000
    assert result['robot_lifecycle_provider'][
        'authority_heartbeat_period_ms'
    ] == 1_000
    assert result['robot_lifecycle_provider'][
        'authority_health_max_age_ms'
    ] == 2_000
    assert result['robot_lifecycle_provider'][
        'material_adapter_status_keys'
    ] == [
        'vendor_mode',
        'vendor_state',
        'brake_mask',
        'servo_enable_mask',
        'error_code',
        'warning_code',
    ]
    execution_binding = result['ros2_control_execution_binding']
    assert execution_binding['generation_id'] == (
        result['controller_binding']['generation_id']
    )
    assert execution_binding['trajectory_controller_manager_name'] == (
        'lite6_traj_controller'
    )
    assert execution_binding['follow_joint_trajectory_action'] == (
        '/lite6_traj_controller/follow_joint_trajectory'
    )
    assert execution_binding['required_joints'] == [
        f'joint{index}' for index in range(1, 7)
    ]
    execution_config = result['manyforge_execution_config']
    assert execution_config['controller_binding'] == execution_binding
    assert execution_config['execution_admission'] == (
        result['execution_admission']
    )
    assert execution_config['action_name'] == (
        '/lite6_traj_controller/follow_joint_trajectory'
    )
    assert execution_config['execution_state_source_mode'] == (
        'observed_robot'
    )
    assert execution_config['require_fresh_state_before_planning'] is True
    assert (
        execution_config['require_execution_admission_snapshot']
        is True
    )
    assert execution_config['enable_jtc_speed_scaling_input'] is False
    assert execution_config['decelerate_on_cancel'] is True
    assert execution_config['velocity_state_interface_joints'] == [
        f'joint{index}' for index in range(1, 7)
    ]
    assert execution_config['max_deceleration_on_cancel_rad_s2'] == {
        f'joint{index}': 2.0 for index in range(1, 7)
    }
    assert '__MANYFORGE_' not in runtime
    assert model_only_urdf(runtime) == model_only_urdf(template)
    assert json.loads(output_manifest.read_text(encoding='utf-8')) == result


@pytest.mark.parametrize(
    ('field', 'value', 'message'),
    [
        ('robot_ip', 'not-an-address', 'literal IPv4'),
        ('robot_ip', '0.0.0.0', 'usable unicast'),
        ('robot_ip', '0.1.2.3', 'usable unicast'),
        ('robot_ip', '127.0.0.1', 'usable unicast'),
        ('robot_ip', '224.0.0.1', 'usable unicast'),
        ('robot_ip', '255.255.255.255', 'usable unicast'),
        ('robot_instance_id', '../lite6', 'portable runtime identifier'),
        ('transport_owner_id', '', 'portable runtime identifier'),
        (
            'lifecycle_provider_instance_id',
            '',
            'portable runtime identifier',
        ),
        ('authority_profile', 'implicit_admin', 'authority_profile'),
    ],
)
def test_rejects_unqualified_runtime_values(
    tmp_path: Path,
    field: str,
    value: str,
    message: str,
) -> None:
    arguments = {field: value}

    with pytest.raises(ValueError, match=message):
        _resolve(tmp_path, **arguments)


def test_rejects_template_drift_before_resolution(tmp_path: Path) -> None:
    binding, model_root, _ = _binding_fixture(tmp_path)
    runtime_path = (
        model_root / 'lite6_fixed_pneumatic.supervised.runtime.urdf'
    )
    runtime_path.write_text(
        runtime_path.read_text(encoding='utf-8').replace(
            '<param name="access_mode">supervised_lifecycle</param>',
            '<param name="access_mode">legacy_control</param>',
        ),
        encoding='utf-8',
    )

    with pytest.raises(ValueError, match='supervised_lifecycle'):
        resolve_supervised_runtime_binding(
            binding_manifest_path=binding,
            model_root=model_root,
            robot_ip='192.168.1.31',
            robot_instance_id='lite6-a',
            transport_owner_id='lite6-a:sole-owner',
            lifecycle_provider_instance_id='lite6-a:provider:session-1',
            authority_profile='local_operator',
            cell_id='lite6-cell',
            cell_supervisor_component_epoch='cell-supervisor-epoch-1',
            cell_supervisor_heartbeat_path=(
                tmp_path / 'cell-supervisor-heartbeat.json'
            ),
            output_urdf_path=tmp_path / 'runtime.urdf',
            output_manifest_path=tmp_path / 'runtime.json',
        )
