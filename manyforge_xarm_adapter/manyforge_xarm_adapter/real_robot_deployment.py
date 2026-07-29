"""Resolve a portable Lite6 cell/template into one retained runtime record."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile
from typing import Any, Mapping

import yaml

from .model_generation import ARTIFACT_NAMES
from .supervised_runtime_binding import RUNTIME_BINDING_SCHEMA

CELL_SCHEMA = 'manyforge.real_robot_cell.v1'
RUNTIME_RECORD_SCHEMA = 'manyforge.real_robot_runtime_record.v1'
RESOLUTION_SCHEMA = 'manyforge.real_robot_resolution.v1'
RESOLVER_ID = 'xarm.lite6_supervised.v1'
_TOKEN_PREFIX = '__MANYFORGE_'
_IDENTIFIER = re.compile(r'[A-Za-z][A-Za-z0-9_.:-]{0,127}\Z')
_REQUIRED_CELL_CONFIG = frozenset(
    {
        'robot_instance_id',
        'transport_owner_id',
        'authority_profile',
        'robot_address_source_env',
        'xarm_package_root',
    }
)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def _mapping(path: Path) -> dict[str, Any]:
    try:
        value = yaml.safe_load(path.read_text(encoding='utf-8'))
    except (OSError, yaml.YAMLError) as exc:
        raise ValueError(f'{path} is not readable YAML: {exc}') from exc
    if not isinstance(value, dict):
        raise ValueError(f'{path} must contain a mapping')
    return value


def _json_mapping(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f'{path} is not readable JSON: {exc}') from exc
    if not isinstance(value, dict):
        raise ValueError(f'{path} must contain a JSON object')
    return value


def _atomic_text(path: Path, value: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(
        prefix=f'.{path.name}.',
        dir=path.parent,
        text=True,
    )
    try:
        with os.fdopen(descriptor, 'w', encoding='utf-8') as handle:
            handle.write(value)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    except BaseException:
        Path(temporary).unlink(missing_ok=True)
        raise


def _required_text(
    mapping: Mapping[str, Any],
    key: str,
    *,
    identifier: bool = False,
) -> str:
    value = mapping.get(key)
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f'{key} must be a non-empty string')
    value = value.strip()
    if identifier and not _IDENTIFIER.fullmatch(value):
        raise ValueError(f'{key} must be a portable identifier')
    return value


def _resolved_cell_path(
    cell_path: Path,
    value: object,
    *,
    key: str,
) -> Path:
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f'scenario.{key} must be a relative path')
    relative = Path(value)
    if relative.is_absolute():
        raise ValueError(f'scenario.{key} must be relative')
    try:
        result = (cell_path.parent / relative).resolve(strict=True)
        result.relative_to(cell_path.parent.resolve())
    except (OSError, ValueError) as exc:
        raise ValueError(
            f'scenario.{key} must resolve inside the cell directory'
        ) from exc
    if not result.is_file():
        raise ValueError(f'scenario.{key} must resolve to a file')
    return result


def _cell_config(
    cell_path: Path,
    *,
    template_path: Path,
    program_path: Path,
) -> dict[str, str]:
    cell = _mapping(cell_path)
    if cell.get('schema') != CELL_SCHEMA:
        raise ValueError(f'cell schema must be {CELL_SCHEMA}')
    adapter = cell.get('adapter')
    if not isinstance(adapter, dict):
        raise ValueError('cell adapter must be a mapping')
    if adapter.get('resolver_id') != RESOLVER_ID:
        raise ValueError(f'cell resolver must be {RESOLVER_ID}')
    config = adapter.get('config')
    if not isinstance(config, dict) or set(config) != _REQUIRED_CELL_CONFIG:
        raise ValueError(
            'cell adapter.config must contain exactly '
            + ', '.join(sorted(_REQUIRED_CELL_CONFIG))
        )
    scenario = cell.get('scenario')
    if not isinstance(scenario, dict):
        raise ValueError('cell scenario must be a mapping')
    if (
        _resolved_cell_path(
            cell_path,
            scenario.get('deployment_template'),
            key='deployment_template',
        )
        != template_path.resolve()
    ):
        raise ValueError('selected deployment template does not match the cell')
    if (
        _resolved_cell_path(
            cell_path,
            scenario.get('program'),
            key='program',
        )
        != program_path.resolve()
    ):
        raise ValueError('selected program does not match the cell')
    result = {
        key: _required_text(
            config,
            key,
            identifier=key
            in {
                'robot_instance_id',
                'transport_owner_id',
                'authority_profile',
                'robot_address_source_env',
            },
        )
        for key in _REQUIRED_CELL_CONFIG
    }
    if result['authority_profile'] not in {
        'local_operator',
        'plc_roxsie',
    }:
        raise ValueError('cell authority_profile is unsupported')
    package_root = Path(result['xarm_package_root'])
    if not package_root.is_absolute():
        raise ValueError('xarm_package_root must be absolute')
    return result


def _artifact_record(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise ValueError(f'required runtime artifact is missing: {path}')
    return {
        'path': str(path),
        'sha256': _sha256(path),
        'sizeBytes': path.stat().st_size,
    }


def resolve_real_robot_deployment(
    *,
    cell_path: Path,
    template_path: Path,
    program_path: Path,
    model_root: Path,
    runtime_binding_path: Path,
    output_deployment_path: Path,
    output_record_path: Path,
) -> dict[str, Any]:
    """Resolve one exact process generation without mutating source artifacts."""
    cell_path = cell_path.resolve(strict=True)
    template_path = template_path.resolve(strict=True)
    program_path = program_path.resolve(strict=True)
    model_root = model_root.resolve(strict=True)
    runtime_binding_path = runtime_binding_path.resolve(strict=True)
    output_deployment_path = output_deployment_path.resolve()
    output_record_path = output_record_path.resolve()

    config = _cell_config(
        cell_path,
        template_path=template_path,
        program_path=program_path,
    )
    template = _mapping(template_path)
    binding = _json_mapping(runtime_binding_path)
    if binding.get('schema') != RUNTIME_BINDING_SCHEMA:
        raise ValueError(
            f'runtime binding schema must be {RUNTIME_BINDING_SCHEMA}'
        )
    runtime = binding.get('runtime')
    execution_config = binding.get('manyforge_execution_config')
    cell_supervision = binding.get('cell_supervision')
    if not isinstance(runtime, dict) or not isinstance(execution_config, dict):
        raise ValueError('runtime binding is missing runtime/execution config')
    if (
        not isinstance(cell_supervision, dict)
        or cell_supervision.get('schema')
        != 'manyforge.cell_supervision.v1'
        or execution_config.get('cell_supervision') != cell_supervision
        or cell_supervision.get('cell_id')
        != _required_text(_mapping(cell_path), 'cell_id')
    ):
        raise ValueError(
            'runtime binding cell supervision does not match the cell'
        )
    expected_runtime = {
        'robot_instance_id': config['robot_instance_id'],
        'transport_owner_id': config['transport_owner_id'],
        'authority_profile': config['authority_profile'],
    }
    for key, expected in expected_runtime.items():
        if runtime.get(key) != expected:
            raise ValueError(f'runtime binding does not match cell {key}')

    model_urdf = model_root / ARTIFACT_NAMES['model_urdf']
    model_srdf = model_root / ARTIFACT_NAMES['srdf']
    controllers = model_root / ARTIFACT_NAMES['ros2_controllers']
    model_manifest = model_root / 'MODEL_MANIFEST.json'
    runtime_urdf = Path(_required_text(runtime, 'urdf_path')).resolve(
        strict=True
    )
    if _sha256(runtime_urdf) != runtime.get('urdf_sha256'):
        raise ValueError('runtime URDF digest does not match runtime binding')

    robot_config = template.get('robot_config')
    adapters = template.get('adapters')
    if not isinstance(robot_config, dict) or not isinstance(adapters, dict):
        raise ValueError('deployment template lacks robot_config/adapters')
    execution = adapters.get('execution')
    if not isinstance(execution, dict):
        raise ValueError('deployment template lacks execution adapter')
    policy_config = execution.get('config')
    if not isinstance(policy_config, dict):
        raise ValueError('deployment execution config must be a mapping')
    policy_config = dict(policy_config)
    if (
        policy_config.pop('resolution_marker', None)
        != '__MANYFORGE_SUPERVISED_RUNTIME_BINDING__'
    ):
        raise ValueError('deployment template resolution marker is invalid')
    overlap = sorted(set(policy_config) & set(execution_config))
    if overlap:
        raise ValueError(
            'deployment policy attempts to override immutable runtime binding: '
            + ', '.join(overlap)
        )

    robot_config['urdf_path'] = str(model_urdf)
    robot_config['srdf_path'] = str(model_srdf)
    robot_config['package_paths'] = [config['xarm_package_root']]
    resolved_execution_config = dict(execution_config)
    resolved_execution_config.update(policy_config)
    resolved_execution_config['real_robot_runtime_record'] = {
        'schema': RESOLUTION_SCHEMA,
        'path': str(output_record_path),
        'bindingGenerationId': _required_text(binding, 'generation_id'),
        'providerInstanceId': _required_text(
            binding['robot_lifecycle_provider'],
            'provider_instance_id',
        ),
    }
    execution['config'] = resolved_execution_config
    serialized_deployment = yaml.safe_dump(template, sort_keys=False)
    if _TOKEN_PREFIX in serialized_deployment:
        raise ValueError('resolved deployment retains a template token')
    _atomic_text(output_deployment_path, serialized_deployment)

    artifacts = {
        'cell': _artifact_record(cell_path),
        'deploymentTemplate': _artifact_record(template_path),
        'program': _artifact_record(program_path),
        'modelManifest': _artifact_record(model_manifest),
        'modelUrdf': _artifact_record(model_urdf),
        'modelSrdf': _artifact_record(model_srdf),
        'controllers': _artifact_record(controllers),
        'runtimeUrdf': _artifact_record(runtime_urdf),
        'runtimeBinding': _artifact_record(runtime_binding_path),
        'resolvedDeployment': _artifact_record(output_deployment_path),
    }
    record = {
        'schema': RUNTIME_RECORD_SCHEMA,
        'generatedAt': datetime.now(timezone.utc).isoformat(),
        'resolver': {
            'id': RESOLVER_ID,
            'resolutionSchema': RESOLUTION_SCHEMA,
        },
        'cellId': _required_text(_mapping(cell_path), 'cell_id'),
        'runtimeGenerationId': _required_text(binding, 'generation_id'),
        'bindingGenerationId': _required_text(
            binding['controller_binding'],
            'generation_id',
        ),
        'cellSupervision': {
            'schema': cell_supervision['schema'],
            'cellId': cell_supervision['cell_id'],
            'supervisorComponentEpoch': cell_supervision[
                'cell_supervisor_component_epoch'
            ],
            'supervisorHeartbeatPath': cell_supervision[
                'supervisor_heartbeat_path'
            ],
            'startupScopedEvidenceOnly': True,
            'components': cell_supervision['components'],
        },
        'robot': {
            'id': runtime['robot_instance_id'],
            'address': runtime['robot_ip'],
            'transportOwnerId': runtime['transport_owner_id'],
            'authorityProfile': runtime['authority_profile'],
            'lifecycleProviderInstanceId': runtime[
                'lifecycle_provider_instance_id'
            ],
            'authorityHealthPolicy': {
                'leaseDurationMs': binding['robot_lifecycle_provider'][
                    'authority_lease_duration_ms'
                ],
                'heartbeatPeriodMs': binding['robot_lifecycle_provider'][
                    'authority_heartbeat_period_ms'
                ],
                'criticalProgressMaxAgeMs': binding[
                    'robot_lifecycle_provider'
                ]['authority_health_max_age_ms'],
                'providerMaximumLeaseMs': binding[
                    'robot_lifecycle_provider'
                ]['maximum_authority_lease_ms'],
            },
        },
        'artifacts': artifacts,
    }
    _atomic_text(
        output_record_path,
        json.dumps(record, indent=2, sort_keys=True) + '\n',
    )
    return record


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cell', type=Path, required=True)
    parser.add_argument('--deployment-template', type=Path, required=True)
    parser.add_argument('--program', type=Path, required=True)
    parser.add_argument('--model-root', type=Path, required=True)
    parser.add_argument('--runtime-binding', type=Path, required=True)
    parser.add_argument('--output-deployment', type=Path, required=True)
    parser.add_argument('--output-record', type=Path, required=True)
    args = parser.parse_args()
    record = resolve_real_robot_deployment(
        cell_path=args.cell,
        template_path=args.deployment_template,
        program_path=args.program,
        model_root=args.model_root,
        runtime_binding_path=args.runtime_binding,
        output_deployment_path=args.output_deployment,
        output_record_path=args.output_record,
    )
    print(record['runtimeGenerationId'])
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
