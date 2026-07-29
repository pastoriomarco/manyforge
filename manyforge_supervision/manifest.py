"""Strict deployment-manifest and heartbeat translation helpers."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Mapping

from .component_health import (
    ComponentHealthState,
    ComponentHeartbeat,
    ComponentPolicy,
    ComponentRole,
    SupervisionPolicy,
)

SUPERVISION_SCHEMA = 'manyforge.cell_supervision.v1'
CELL_HEARTBEAT_SCHEMA = 'manyforge.cell_supervisor.heartbeat.v1'
_MAX_HEARTBEAT_BYTES = 64 * 1024
_SUPERVISION_KEYS = frozenset(
    {
        'schema',
        'cell_id',
        'robot_ids',
        'authority_profile',
        'supervisor_heartbeat_path',
        'cell_supervisor_component_epoch',
        'components',
    }
)
_COMPONENT_KEYS = frozenset(
    {
        'role',
        'id',
        'maximum_heartbeat_age_ms',
        'maximum_critical_progress_age_ms',
        'dependencies',
        'restart_domain',
        'required_for_dispatch',
        'expected_runtime_generation',
        'expected_binding_generation',
        'expected_configuration_generation',
    }
)
_CELL_HEARTBEAT_KEYS = frozenset(
    {
        'schema',
        'componentRole',
        'componentId',
        'robotId',
        'cellId',
        'componentEpoch',
        'sequence',
        'healthState',
        'criticalProgressSequence',
        'dependencyEpochs',
        'runtimeGeneration',
        'bindingGeneration',
        'configurationGeneration',
        'desiredState',
        'reasonCodes',
        'requestedContainment',
        'observedContainers',
        'uncleanTerminations',
        'observedAtSteadyTimeNs',
        'observedAtUtc',
    }
)


def _mapping(value: object, label: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise ValueError(f'{label} must be a mapping')
    return value


def _text(value: object, label: str, *, optional: bool = False) -> str:
    if optional and value in (None, ''):
        return ''
    if (
        not isinstance(value, str)
        or not value
        or len(value) > 256
        or any(ord(character) < 0x21 or ord(character) > 0x7E for character in value)
    ):
        raise ValueError(f'{label} must be a bounded printable identifier')
    return value


def _positive_integer(value: object, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise ValueError(f'{label} must be a positive integer')
    return value


def _boolean(value: object, label: str) -> bool:
    if not isinstance(value, bool):
        raise ValueError(f'{label} must be a boolean')
    return value


def _reject_unknown_keys(
    value: Mapping[str, Any],
    allowed: frozenset[str],
    label: str,
) -> None:
    unknown = sorted(set(value) - allowed)
    if unknown:
        raise ValueError(f"{label} contains unknown keys: {', '.join(unknown)}")


def supervision_policy_from_mapping(
    value: Mapping[str, Any],
) -> SupervisionPolicy:
    _reject_unknown_keys(value, _SUPERVISION_KEYS, 'cell supervision')
    if value.get('schema') != SUPERVISION_SCHEMA:
        raise ValueError('cell supervision schema is unsupported')
    raw_robots = value.get('robot_ids')
    raw_components = value.get('components')
    if not isinstance(raw_robots, list) or not isinstance(raw_components, list):
        raise ValueError('cell supervision lists are malformed')
    components: list[ComponentPolicy] = []
    for index, raw in enumerate(raw_components):
        component = _mapping(raw, f'components[{index}]')
        _reject_unknown_keys(
            component,
            _COMPONENT_KEYS,
            f'components[{index}]',
        )
        dependencies = component.get('dependencies')
        if not isinstance(dependencies, list):
            raise ValueError(
                f'components[{index}].dependencies must be a list'
            )
        try:
            role = ComponentRole(
                _text(component.get('role'), f'components[{index}].role')
            )
        except ValueError as exc:
            raise ValueError(
                f'components[{index}].role is unsupported'
            ) from exc
        components.append(
            ComponentPolicy(
                component_role=role,
                component_id=_text(
                    component.get('id'),
                    f'components[{index}].id',
                ),
                maximum_heartbeat_age_ns=_positive_integer(
                    component.get('maximum_heartbeat_age_ms'),
                    f'components[{index}].maximum_heartbeat_age_ms',
                )
                * 1_000_000,
                maximum_critical_progress_age_ns=_positive_integer(
                    component.get('maximum_critical_progress_age_ms'),
                    (
                        f'components[{index}].'
                        'maximum_critical_progress_age_ms'
                    ),
                )
                * 1_000_000,
                dependency_ids=tuple(
                    _text(
                        dependency,
                        f'components[{index}].dependencies',
                    )
                    for dependency in dependencies
                ),
                restart_domain=_text(
                    component.get('restart_domain'),
                    f'components[{index}].restart_domain',
                    optional=True,
                ),
                required_for_dispatch=_boolean(
                    component.get('required_for_dispatch', True),
                    f'components[{index}].required_for_dispatch',
                ),
                expected_runtime_generation=_text(
                    component.get('expected_runtime_generation'),
                    f'components[{index}].expected_runtime_generation',
                    optional=True,
                ),
                expected_binding_generation=_text(
                    component.get('expected_binding_generation'),
                    f'components[{index}].expected_binding_generation',
                    optional=True,
                ),
                expected_configuration_generation=_text(
                    component.get('expected_configuration_generation'),
                    f'components[{index}].expected_configuration_generation',
                    optional=True,
                ),
            )
        )
    return SupervisionPolicy(
        cell_id=_text(value.get('cell_id'), 'cell_id'),
        robot_ids=tuple(_text(robot, 'robot_ids') for robot in raw_robots),
        authority_profile=_text(
            value.get('authority_profile'),
            'authority_profile',
        ),
        components=tuple(components),
    )


def read_cell_supervisor_heartbeat(
    path: Path,
    *,
    expected_component_id: str,
    expected_component_epoch: str,
) -> ComponentHeartbeat:
    try:
        if path.stat().st_size > _MAX_HEARTBEAT_BYTES:
            raise ValueError('cell_supervisor_heartbeat_oversized')
        payload = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError('cell_supervisor_heartbeat_unavailable') from exc
    value = _mapping(payload, 'cell supervisor heartbeat')
    _reject_unknown_keys(
        value,
        _CELL_HEARTBEAT_KEYS,
        'cell supervisor heartbeat',
    )
    if value.get('schema') != CELL_HEARTBEAT_SCHEMA:
        raise ValueError('cell_supervisor_heartbeat_schema_mismatch')
    if (
        value.get('componentId') != expected_component_id
        or value.get('componentEpoch') != expected_component_epoch
    ):
        raise ValueError('cell_supervisor_heartbeat_identity_mismatch')
    raw_dependencies = _mapping(
        value.get('dependencyEpochs'),
        'dependencyEpochs',
    )
    try:
        role = ComponentRole(_text(value.get('componentRole'), 'componentRole'))
        state = ComponentHealthState(
            _text(value.get('healthState'), 'healthState')
        )
    except ValueError as exc:
        raise ValueError('cell_supervisor_heartbeat_state_invalid') from exc
    if role != ComponentRole.CELL_SUPERVISOR:
        raise ValueError('cell_supervisor_heartbeat_role_mismatch')
    return ComponentHeartbeat(
        component_role=role,
        component_id=_text(value.get('componentId'), 'componentId'),
        robot_id=_text(value.get('robotId'), 'robotId'),
        cell_id=_text(value.get('cellId'), 'cellId'),
        component_epoch=_text(value.get('componentEpoch'), 'componentEpoch'),
        sequence=_positive_integer(value.get('sequence'), 'sequence'),
        health_state=state,
        critical_progress_sequence=_positive_integer(
            value.get('criticalProgressSequence'),
            'criticalProgressSequence',
        ),
        dependency_epochs={
            _text(key, 'dependencyEpochs key'): _text(
                item,
                'dependencyEpochs value',
            )
            for key, item in raw_dependencies.items()
        },
        runtime_generation=_text(
            value.get('runtimeGeneration'),
            'runtimeGeneration',
            optional=True,
        ),
        binding_generation=_text(
            value.get('bindingGeneration'),
            'bindingGeneration',
            optional=True,
        ),
        configuration_generation=_text(
            value.get('configurationGeneration'),
            'configurationGeneration',
            optional=True,
        ),
    )
