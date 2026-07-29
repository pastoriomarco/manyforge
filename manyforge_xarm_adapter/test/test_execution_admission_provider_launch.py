from __future__ import annotations

import json
from pathlib import Path

from manyforge_xarm_adapter.execution_admission_provider_launch import (
    admission_provider_command,
)
from manyforge_xarm_adapter.supervised_runtime_binding import (
    EXECUTION_ADMISSION_SCHEMA,
)
from manyforge_xarm_adapter.supervised_runtime_binding import (
    RUNTIME_BINDING_SCHEMA,
)
import pytest


def _manifest(path: Path) -> Path:
    payload = {
        'schema': RUNTIME_BINDING_SCHEMA,
        'generation_id': 'runtime-generation-1',
        'runtime': {
            'robot_instance_id': 'lite6-a',
            'transport_owner_id': 'lite6-a:sole-owner',
            'authority_profile': 'local_operator',
            'cell_id': 'lite6-cell',
        },
        'controller_binding': {
            'generation_id': 'a' * 64,
            'controller_manager': '/controller_manager',
            'controller_manager_activity_topic': (
                '/controller_manager/activity'
            ),
            'hardware_component_name': 'lite6_system',
            'joint_state_broadcaster_manager_name': (
                'joint_state_broadcaster'
            ),
            'trajectory_controller_manager_name': (
                'lite6_traj_controller'
            ),
        },
        'command_surface': {
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
        },
        'execution_admission': {
            'schema': EXECUTION_ADMISSION_SCHEMA,
            'topic': '/manyforge_execution_admission',
            'provider_instance_id': 'lite6-a:provider:session-1',
            'supervisor_instance_id': 'lite6-a:lifecycle-supervisor',
            'refresh_period_ms': 50,
            'maximum_lease_ms': 250,
        },
        'robot_lifecycle_provider': {
            'schema': 'manyforge.robot_lifecycle.ros_provider.v1',
            'robot_id': 'lite6-a',
            'provider_instance_id': 'lite6-a:provider:session-1',
            'status_topic': '/manyforge/robot_lifecycle/status',
            'open_session_service': '/manyforge/robot_lifecycle/open_session',
            'close_session_service': (
                '/manyforge/robot_lifecycle/close_session'
            ),
            'submit_command_service': (
                '/manyforge/robot_lifecycle/submit_command'
            ),
            'reconcile_command_service': (
                '/manyforge/robot_lifecycle/reconcile'
            ),
            'caller_assurance': 'unverified',
            'maximum_authority_lease_ms': 300_000,
        },
        'cell_supervision': {
            'schema': 'manyforge.cell_supervision.v1',
            'cell_id': 'lite6-cell',
            'authority_profile': 'local_operator',
            'cell_supervisor_component_epoch': (
                'cell-supervisor-epoch-1'
            ),
            'components': [
                {
                    'role': 'transport_owner',
                    'id': 'lite6-a:sole-owner',
                    'maximum_heartbeat_age_ms': 500,
                    'maximum_critical_progress_age_ms': 1_000,
                }
            ],
        },
    }
    path.write_text(json.dumps(payload), encoding='utf-8')
    return path


def test_compiles_exact_provider_command(tmp_path: Path) -> None:
    command = admission_provider_command(_manifest(tmp_path / 'runtime.json'))

    assert command[:5] == [
        'ros2',
        'run',
        'manyforge_xarm_control',
        'lite6_execution_admission_provider',
        '--ros-args',
    ]
    remaps = [
        command[index + 1]
        for index, item in enumerate(command)
        if item == '-r'
    ]
    assert remaps == [
        '__node:=manyforge_lite6_a_'
        '72542a36_execution_admission_provider'
    ]
    parameters = {
        command[index + 1].split(':=', 1)[0]: (
            command[index + 1].split(':=', 1)[1]
        )
        for index, item in enumerate(command)
        if item == '-p'
    }
    assert parameters['robot_id'] == 'lite6-a'
    assert parameters['provider_instance_id'] == (
        'lite6-a:provider:session-1'
    )
    assert parameters['binding_contract_generation_id'] == 'a' * 64
    assert parameters['runtime_generation_id'] == 'runtime-generation-1'
    assert parameters['cell_supervisor_component_epoch'] == (
        'cell-supervisor-epoch-1'
    )
    assert parameters['controller_activity_topic'] == (
        '/controller_manager/activity'
    )
    assert parameters['controller_manager'] == '/controller_manager'
    assert parameters['hardware_component_name'] == 'lite6_system'
    assert parameters['joint_state_broadcaster_name'] == (
        'joint_state_broadcaster'
    )
    assert parameters['trajectory_controller_name'] == (
        'lite6_traj_controller'
    )
    assert parameters['lifecycle_status_topic'] == (
        '/manyforge/robot_lifecycle/status'
    )
    assert parameters['refresh_period_ms'] == '50'
    assert parameters['maximum_lease_ms'] == '250'
    assert parameters['observation_max_age_ms'] == '1000'
    assert parameters['owner_progress_max_age_ms'] == '500'


@pytest.mark.parametrize(
    ('section', 'field'),
    [
        ('execution_admission', 'provider_instance_id'),
        ('controller_binding', 'generation_id'),
        ('runtime', 'transport_owner_id'),
        ('command_surface', 'supervised_state_topic'),
        ('robot_lifecycle_provider', 'status_topic'),
        ('controller_binding', 'hardware_component_name'),
    ],
)
def test_rejects_missing_exact_provider_fact(
    tmp_path: Path,
    section: str,
    field: str,
) -> None:
    path = _manifest(tmp_path / 'runtime.json')
    payload = json.loads(path.read_text(encoding='utf-8'))
    del payload[section][field]
    path.write_text(json.dumps(payload), encoding='utf-8')

    with pytest.raises(ValueError, match=field):
        admission_provider_command(path)


def test_rejects_lease_not_longer_than_refresh(tmp_path: Path) -> None:
    path = _manifest(tmp_path / 'runtime.json')
    payload = json.loads(path.read_text(encoding='utf-8'))
    payload['execution_admission']['maximum_lease_ms'] = 50
    path.write_text(json.dumps(payload), encoding='utf-8')

    with pytest.raises(ValueError, match='lease'):
        admission_provider_command(path)


def test_rejects_owner_progress_age_not_longer_than_refresh(
    tmp_path: Path,
) -> None:
    path = _manifest(tmp_path / 'runtime.json')
    payload = json.loads(path.read_text(encoding='utf-8'))
    payload['cell_supervision']['components'][0][
        'maximum_heartbeat_age_ms'
    ] = 50
    path.write_text(json.dumps(payload), encoding='utf-8')

    with pytest.raises(ValueError, match='heartbeat age'):
        admission_provider_command(path)


def test_rejects_observation_age_not_longer_than_refresh(
    tmp_path: Path,
) -> None:
    path = _manifest(tmp_path / 'runtime.json')
    payload = json.loads(path.read_text(encoding='utf-8'))
    payload['cell_supervision']['components'][0][
        'maximum_critical_progress_age_ms'
    ] = 50
    path.write_text(json.dumps(payload), encoding='utf-8')

    with pytest.raises(ValueError, match='observation age'):
        admission_provider_command(path)


@pytest.mark.parametrize(
    ('section', 'field', 'value'),
    [
        ('runtime', 'authority_profile', 'implicit_admin'),
        ('runtime', 'robot_instance_id', 'lite6 a'),
        ('execution_admission', 'provider_instance_id', '../provider'),
        ('execution_admission', 'maximum_lease_ms', 10_001),
        ('robot_lifecycle_provider', 'caller_assurance', 'implicit_admin'),
        ('robot_lifecycle_provider', 'robot_id', 'other-lite6'),
        (
            'robot_lifecycle_provider',
            'provider_instance_id',
            'other-provider',
        ),
    ],
)
def test_rejects_unsafe_provider_identity_or_timing(
    tmp_path: Path,
    section: str,
    field: str,
    value: object,
) -> None:
    path = _manifest(tmp_path / 'runtime.json')
    payload = json.loads(path.read_text(encoding='utf-8'))
    payload[section][field] = value
    path.write_text(json.dumps(payload), encoding='utf-8')

    with pytest.raises(ValueError):
        admission_provider_command(path)
