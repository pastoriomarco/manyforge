"""Launch the admission provider from one resolved Lite6 runtime manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
from typing import Any

from .supervised_runtime_binding import EXECUTION_ADMISSION_SCHEMA
from .supervised_runtime_binding import RUNTIME_BINDING_SCHEMA


_IDENTIFIER = re.compile(r'[A-Za-z0-9][A-Za-z0-9_.:-]{0,255}\Z')


def _mapping(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValueError(f'{label} must be a mapping')
    return value


def _string(mapping: dict[str, Any], key: str, label: str) -> str:
    value = mapping.get(key)
    if (
        not isinstance(value, str)
        or not value
        or '\x00' in value
        or '\n' in value
    ):
        raise ValueError(f'{label}.{key} must be a non-empty string')
    return value


def _identifier(mapping: dict[str, Any], key: str, label: str) -> str:
    value = _string(mapping, key, label)
    if not _IDENTIFIER.fullmatch(value):
        raise ValueError(f'{label}.{key} must be a portable identifier')
    return value


def _positive_integer(
    mapping: dict[str, Any],
    key: str,
    label: str,
) -> int:
    value = mapping.get(key)
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise ValueError(f'{label}.{key} must be a positive integer')
    return value


def _provider_node_name(robot_id: str) -> str:
    portable = re.sub(r'[^A-Za-z0-9_]', '_', robot_id).strip('_')
    digest = hashlib.sha256(robot_id.encode('utf-8')).hexdigest()[:8]
    return (
        f'manyforge_{portable[:80]}_{digest}_'
        'execution_admission_provider'
    )


def _transport_owner_policy(
    supervision: dict[str, Any],
    transport_owner_id: str,
) -> dict[str, Any]:
    components = supervision.get('components')
    if not isinstance(components, list):
        raise ValueError('cell_supervision.components must be a list')
    matches = [
        component
        for component in components
        if (
            isinstance(component, dict)
            and component.get('role') == 'transport_owner'
            and component.get('id') == transport_owner_id
        )
    ]
    if len(matches) != 1:
        raise ValueError(
            'cell_supervision must contain one exact transport-owner policy'
        )
    return matches[0]


def _transport_owner_progress_max_age_ms(
    owner_policy: dict[str, Any],
) -> int:
    return _positive_integer(
        owner_policy,
        'maximum_heartbeat_age_ms',
        'cell_supervision transport_owner',
    )


def _transport_observation_max_age_ms(
    owner_policy: dict[str, Any],
) -> int:
    return _positive_integer(
        owner_policy,
        'maximum_critical_progress_age_ms',
        'cell_supervision transport_owner',
    )


def admission_provider_command(
    runtime_binding_path: Path,
) -> list[str]:
    """Return the exact ROS command for one manifest-bound provider."""
    payload = json.loads(runtime_binding_path.read_text(encoding='utf-8'))
    root = _mapping(payload, 'runtime binding')
    if root.get('schema') != RUNTIME_BINDING_SCHEMA:
        raise ValueError(
            f'runtime binding must use schema {RUNTIME_BINDING_SCHEMA}'
        )
    runtime = _mapping(root.get('runtime'), 'runtime')
    controller = _mapping(root.get('controller_binding'), 'controller_binding')
    command_surface = _mapping(
        root.get('command_surface'),
        'command_surface',
    )
    admission = _mapping(
        root.get('execution_admission'),
        'execution_admission',
    )
    lifecycle = _mapping(
        root.get('robot_lifecycle_provider'),
        'robot_lifecycle_provider',
    )
    supervision = _mapping(
        root.get('cell_supervision'),
        'cell_supervision',
    )
    if supervision.get('schema') != 'manyforge.cell_supervision.v1':
        raise ValueError('cell_supervision uses an unsupported schema')
    if admission.get('schema') != EXECUTION_ADMISSION_SCHEMA:
        raise ValueError(
            'execution_admission uses an unsupported schema'
        )
    if (
        lifecycle.get('schema')
        != 'manyforge.robot_lifecycle.ros_provider.v1'
    ):
        raise ValueError(
            'robot_lifecycle_provider uses an unsupported schema'
        )
    refresh_period_ms = _positive_integer(
        admission,
        'refresh_period_ms',
        'execution_admission',
    )
    maximum_lease_ms = _positive_integer(
        admission,
        'maximum_lease_ms',
        'execution_admission',
    )
    if maximum_lease_ms <= refresh_period_ms or maximum_lease_ms > 10_000:
        raise ValueError(
            'execution admission lease must exceed its refresh period '
            'and be at most 10000 ms'
        )

    robot_id = _identifier(runtime, 'robot_instance_id', 'runtime')
    transport_owner_id = _identifier(
        runtime,
        'transport_owner_id',
        'runtime',
    )
    owner_policy = _transport_owner_policy(
        supervision,
        transport_owner_id,
    )
    owner_progress_max_age_ms = _transport_owner_progress_max_age_ms(
        owner_policy,
    )
    observation_max_age_ms = _transport_observation_max_age_ms(
        owner_policy,
    )
    if (
        owner_progress_max_age_ms <= refresh_period_ms
        or owner_progress_max_age_ms > 10_000
    ):
        raise ValueError(
            'transport-owner heartbeat age must exceed the provider refresh '
            'period and be at most 10000 ms'
        )
    if (
        observation_max_age_ms <= refresh_period_ms
        or observation_max_age_ms > 10_000
    ):
        raise ValueError(
            'transport observation age must exceed the provider refresh '
            'period and be at most 10000 ms'
        )
    lifecycle_robot_id = _identifier(
        lifecycle,
        'robot_id',
        'robot_lifecycle_provider',
    )
    if lifecycle_robot_id != robot_id:
        raise ValueError(
            'robot_lifecycle_provider.robot_id does not match runtime'
        )
    admission_provider_id = _identifier(
        admission,
        'provider_instance_id',
        'execution_admission',
    )
    lifecycle_provider_id = _identifier(
        lifecycle,
        'provider_instance_id',
        'robot_lifecycle_provider',
    )
    if lifecycle_provider_id != admission_provider_id:
        raise ValueError(
            'robot lifecycle and execution-admission provider identities '
            'do not match'
        )
    caller_assurance = _identifier(
        lifecycle,
        'caller_assurance',
        'robot_lifecycle_provider',
    )
    if caller_assurance not in ('unverified', 'authenticated'):
        raise ValueError(
            'robot_lifecycle_provider.caller_assurance is unsupported'
        )
    _positive_integer(
        lifecycle,
        'maximum_authority_lease_ms',
        'robot_lifecycle_provider',
    )
    authority = _identifier(runtime, 'authority_profile', 'runtime')
    if authority not in ('local_operator', 'plc_roxsie'):
        raise ValueError('runtime.authority_profile is unsupported')
    if (
        _identifier(supervision, 'cell_id', 'cell_supervision')
        != _identifier(runtime, 'cell_id', 'runtime')
    ):
        raise ValueError('cell_supervision.cell_id does not match runtime')
    supervision_authority = _identifier(
        supervision,
        'authority_profile',
        'cell_supervision',
    )
    if supervision_authority != authority:
        raise ValueError(
            'cell_supervision.authority_profile does not match runtime'
        )
    parameters: tuple[tuple[str, str], ...] = (
        ('robot_id', robot_id),
        (
            'provider_instance_id',
            admission_provider_id,
        ),
        (
            'supervisor_instance_id',
            _identifier(
                admission,
                'supervisor_instance_id',
                'execution_admission',
            ),
        ),
        (
            'authority_profile',
            authority,
        ),
        (
            'runtime_generation_id',
            _identifier(root, 'generation_id', 'runtime binding'),
        ),
        (
            'cell_supervisor_component_epoch',
            _identifier(
                supervision,
                'cell_supervisor_component_epoch',
                'cell_supervision',
            ),
        ),
        (
            'binding_contract_generation_id',
            _identifier(controller, 'generation_id', 'controller_binding'),
        ),
        (
            'execution_admission_topic',
            _string(admission, 'topic', 'execution_admission'),
        ),
        (
            'lifecycle_status_topic',
            _string(
                lifecycle,
                'status_topic',
                'robot_lifecycle_provider',
            ),
        ),
        (
            'lifecycle_open_session_service',
            _string(
                lifecycle,
                'open_session_service',
                'robot_lifecycle_provider',
            ),
        ),
        (
            'lifecycle_close_session_service',
            _string(
                lifecycle,
                'close_session_service',
                'robot_lifecycle_provider',
            ),
        ),
        (
            'lifecycle_submit_command_service',
            _string(
                lifecycle,
                'submit_command_service',
                'robot_lifecycle_provider',
            ),
        ),
        (
            'lifecycle_reconcile_command_service',
            _string(
                lifecycle,
                'reconcile_command_service',
                'robot_lifecycle_provider',
            ),
        ),
        (
            'transport_owner_id',
            transport_owner_id,
        ),
        (
            'supervised_state_topic',
            _string(
                command_surface,
                'supervised_state_topic',
                'command_surface',
            ),
        ),
        (
            'supervised_execute_service',
            _string(
                command_surface,
                'supervised_execute_service',
                'command_surface',
            ),
        ),
        (
            'supervised_command_gate_service',
            _string(
                command_surface,
                'supervised_command_gate_service',
                'command_surface',
            ),
        ),
        (
            'supervised_shutdown_controller_service',
            _string(
                command_surface,
                'supervised_shutdown_controller_service',
                'command_surface',
            ),
        ),
        (
            'controller_activity_topic',
            _string(
                controller,
                'controller_manager_activity_topic',
                'controller_binding',
            ),
        ),
        (
            'controller_manager',
            _string(
                controller,
                'controller_manager',
                'controller_binding',
            ),
        ),
        (
            'hardware_component_name',
            _identifier(
                controller,
                'hardware_component_name',
                'controller_binding',
            ),
        ),
        (
            'joint_state_broadcaster_name',
            _identifier(
                controller,
                'joint_state_broadcaster_manager_name',
                'controller_binding',
            ),
        ),
        (
            'trajectory_controller_name',
            _identifier(
                controller,
                'trajectory_controller_manager_name',
                'controller_binding',
            ),
        ),
        ('refresh_period_ms', str(refresh_period_ms)),
        ('maximum_lease_ms', str(maximum_lease_ms)),
        ('observation_max_age_ms', str(observation_max_age_ms)),
        (
            'owner_progress_max_age_ms',
            str(owner_progress_max_age_ms),
        ),
    )
    command = [
        'ros2',
        'run',
        'manyforge_xarm_control',
        'lite6_execution_admission_provider',
        '--ros-args',
        '-r',
        f'__node:={_provider_node_name(robot_id)}',
    ]
    for name, value in parameters:
        command.extend(('-p', f'{name}:={value}'))
    return command


def main() -> int:
    """CLI entry point."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime-binding', type=Path, required=True)
    parser.add_argument(
        '--print-command',
        action='store_true',
        help='Validate and print the argv instead of replacing this process.',
    )
    args = parser.parse_args()
    command = admission_provider_command(args.runtime_binding.resolve())
    if args.print_command:
        print(json.dumps(command))
        return 0
    os.execvp(command[0], command)
    return 1


if __name__ == '__main__':
    raise SystemExit(main())
