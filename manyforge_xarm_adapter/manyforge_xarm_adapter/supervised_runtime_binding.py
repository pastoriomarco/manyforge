"""Resolve and verify one exact supervised Lite6 runtime artifact."""

from __future__ import annotations

import argparse
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
import tempfile
from typing import Any
import xml.etree.ElementTree as ET

from .binding import ResolvedLite6Binding
from .binding_manifest import BINDING_SCHEMA
from .controller_binding import compile_production_controller_binding
from .controller_binding_validation import validate_controller_binding
from .model_generation import ARTIFACT_NAMES
from .model_generation import model_only_urdf
from .model_generation import ROBOT_IP_TOKEN
from .model_generation import TRANSPORT_OWNER_ID_TOKEN


RUNTIME_BINDING_SCHEMA = (
    'manyforge.xarm_adapter.supervised_runtime_binding.v0.experimental'
)
EXECUTION_ADMISSION_SCHEMA = (
    'manyforge.robot_lifecycle.execution_admission.v0.experimental'
)
_AUTHORITY_PROFILES = frozenset({'local_operator', 'plc_roxsie'})
_IDENTIFIER = re.compile(r'[A-Za-z][A-Za-z0-9_.:-]{0,127}\Z')
_URDF_TOKEN_VALUES = {
    'robot_ip': ROBOT_IP_TOKEN,
    'transport_owner_id': TRANSPORT_OWNER_ID_TOKEN,
}
_AUTHORITY_HEALTH_POLICY = {
    'maximum_authority_lease_ms': 300_000,
    'authority_lease_duration_ms': 5_000,
    'authority_heartbeat_period_ms': 1_000,
    'authority_health_max_age_ms': 2_000,
}


def _sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _sha256_text(value: str) -> str:
    return _sha256_bytes(value.encode('utf-8'))


def _json_mapping(path: Path) -> dict[str, Any]:
    payload = json.loads(path.read_text(encoding='utf-8'))
    if not isinstance(payload, dict):
        raise ValueError(f'{path} must contain a JSON object')
    return payload


def _resolved_binding(payload: dict[str, Any]) -> ResolvedLite6Binding:
    values = payload.get('binding')
    if not isinstance(values, dict):
        raise ValueError('binding manifest has no resolved binding')
    values = dict(values)
    arm_joint_names = values.get('arm_joint_names')
    if not isinstance(arm_joint_names, list):
        raise ValueError('binding arm_joint_names must be a list')
    values['arm_joint_names'] = tuple(arm_joint_names)
    try:
        return ResolvedLite6Binding(**values)
    except TypeError as exc:
        raise ValueError('binding manifest resolved binding is malformed') from exc


def _validated_ipv4(value: str) -> str:
    try:
        address = ipaddress.IPv4Address(value)
    except ipaddress.AddressValueError as exc:
        raise ValueError('robot_ip must be one literal IPv4 address') from exc
    if (
        address.is_unspecified
        or int(address) >> 24 == 0
        or address.is_loopback
        or address.is_multicast
        or int(address) == (1 << 32) - 1
    ):
        raise ValueError('robot_ip must be a usable unicast IPv4 address')
    return str(address)


def _validated_identifier(value: str, label: str) -> str:
    if not _IDENTIFIER.fullmatch(value) or '__MANYFORGE_' in value:
        raise ValueError(
            f'{label} must be a bounded portable runtime identifier'
        )
    return value


def _hardware_parameters(runtime_urdf: str) -> dict[str, str]:
    root = ET.fromstring(runtime_urdf)
    controls = root.findall('./ros2_control')
    if len(controls) != 1:
        raise ValueError('runtime URDF must contain one ros2_control block')
    hardware = controls[0].find('./hardware')
    if hardware is None:
        raise ValueError('runtime URDF has no ros2_control hardware block')
    result: dict[str, str] = {}
    for parameter in hardware.findall('./param'):
        name = parameter.get('name', '')
        if not name or name in result:
            raise ValueError('runtime hardware parameters must be unique')
        result[name] = parameter.text or ''
    return result


def _structural_controller_binding(
    payload: dict[str, object],
) -> dict[str, object]:
    result = dict(payload)
    result.pop('generation_id', None)
    result.pop('runtime_urdf_sha256', None)
    return result


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


def resolve_supervised_runtime_binding(
    *,
    binding_manifest_path: Path,
    model_root: Path,
    robot_ip: str,
    robot_instance_id: str,
    transport_owner_id: str,
    lifecycle_provider_instance_id: str,
    authority_profile: str,
    cell_id: str,
    cell_supervisor_component_epoch: str,
    cell_supervisor_heartbeat_path: Path,
    output_urdf_path: Path,
    output_manifest_path: Path,
) -> dict[str, Any]:
    """Resolve runtime-only values and preserve every generated binding fact."""
    manifest = _json_mapping(binding_manifest_path)
    if manifest.get('schema') != BINDING_SCHEMA:
        raise ValueError(
            f'{binding_manifest_path} is not a {BINDING_SCHEMA} document'
        )
    validate_controller_binding(
        binding_manifest_path=binding_manifest_path,
        model_root=model_root,
    )
    resolved = _resolved_binding(manifest)
    template_path = (
        model_root / ARTIFACT_NAMES['supervised_runtime_urdf_template']
    )
    controllers_path = model_root / ARTIFACT_NAMES['ros2_controllers']
    template = template_path.read_text(encoding='utf-8')
    controllers = controllers_path.read_text(encoding='utf-8')

    runtime_values = {
        'robot_ip': _validated_ipv4(robot_ip),
        'robot_id': _validated_identifier(
            robot_instance_id,
            'robot_instance_id',
        ),
        'transport_owner_id': _validated_identifier(
            transport_owner_id,
            'transport_owner_id',
        ),
        'lifecycle_provider_instance_id': _validated_identifier(
            lifecycle_provider_instance_id,
            'lifecycle_provider_instance_id',
        ),
        'authority_profile': authority_profile,
        'cell_id': _validated_identifier(cell_id, 'cell_id'),
        'cell_supervisor_component_epoch': _validated_identifier(
            cell_supervisor_component_epoch,
            'cell_supervisor_component_epoch',
        ),
    }
    if authority_profile not in _AUTHORITY_PROFILES:
        raise ValueError(
            'authority_profile must be local_operator or plc_roxsie'
        )

    runtime = template
    for parameter, token in _URDF_TOKEN_VALUES.items():
        if runtime.count(token) != 1:
            raise ValueError(
                f'runtime template must contain exactly one {token}'
            )
        runtime = runtime.replace(token, runtime_values[parameter], 1)
    if '__MANYFORGE_' in runtime:
        raise ValueError('resolved runtime URDF retains a runtime token')
    if model_only_urdf(runtime) != model_only_urdf(template):
        raise ValueError('runtime resolution changed model semantics')

    parameters = _hardware_parameters(runtime)
    for name in _URDF_TOKEN_VALUES:
        expected = runtime_values[name]
        if parameters.get(name) != expected:
            raise ValueError(f'resolved runtime parameter mismatch: {name}')
    if 'backend' in parameters:
        raise ValueError('production runtime must not select a ManyForge backend')
    if parameters.get('access_mode') != 'supervised_lifecycle':
        raise ValueError(
            'resolved runtime access mode must remain supervised_lifecycle'
        )

    compiled = compile_production_controller_binding(
        resolved=resolved,
        supervised_runtime_urdf=runtime,
        ros2_controllers_yaml=controllers,
    )
    recorded = manifest.get('controller_binding')
    if not isinstance(recorded, dict):
        raise ValueError('binding manifest has no controller binding')
    if _structural_controller_binding(recorded) != (
        _structural_controller_binding(compiled.to_dict())
    ):
        raise ValueError(
            'runtime resolution changed the generated controller binding'
        )

    binding_manifest_sha256 = _sha256_bytes(
        binding_manifest_path.read_bytes()
    )
    identity = {
        'binding_manifest_sha256': binding_manifest_sha256,
        'binding_generation_id': manifest.get('generation_id'),
        'template_urdf_sha256': _sha256_text(template),
        'runtime_urdf_sha256': _sha256_text(runtime),
        'controllers_yaml_sha256': _sha256_text(controllers),
        'robot_ip': runtime_values['robot_ip'],
        'robot_instance_id': runtime_values['robot_id'],
        'transport_owner_id': runtime_values['transport_owner_id'],
        'lifecycle_provider_instance_id': (
            runtime_values['lifecycle_provider_instance_id']
        ),
        'authority_profile': runtime_values['authority_profile'],
        'authority_health_policy': _AUTHORITY_HEALTH_POLICY,
    }
    generation_id = _sha256_bytes(
        json.dumps(
            identity,
            sort_keys=True,
            separators=(',', ':'),
        ).encode('utf-8')
    )
    namespace = compiled.ros_namespace.strip('/')
    execution_admission_topic = (
        f'/{namespace}/manyforge_execution_admission'
        if namespace
        else '/manyforge_execution_admission'
    )
    lifecycle_root = (
        f'/{namespace}/manyforge/robot_lifecycle'
        if namespace
        else '/manyforge/robot_lifecycle'
    )
    execution_binding = compiled.to_ros2_control_execution_binding()
    execution_admission = {
        'schema': EXECUTION_ADMISSION_SCHEMA,
        'topic': execution_admission_topic,
        'runtime_generation_id': generation_id,
        'cell_supervisor_component_epoch': (
            runtime_values['cell_supervisor_component_epoch']
        ),
        'provider_instance_id': (
            runtime_values['lifecycle_provider_instance_id']
        ),
        'supervisor_instance_id': (
            f"{runtime_values['robot_id']}:lifecycle-supervisor"
        ),
        'refresh_period_ms': 50,
        'maximum_lease_ms': 250,
    }
    lifecycle_provider = {
        'schema': 'manyforge.robot_lifecycle.ros_provider.v1',
        'robot_id': runtime_values['robot_id'],
        'runtime_generation_id': generation_id,
        'cell_supervisor_component_epoch': (
            runtime_values['cell_supervisor_component_epoch']
        ),
        'provider_instance_id': (
            runtime_values['lifecycle_provider_instance_id']
        ),
        'status_topic': f'{lifecycle_root}/status',
        'open_session_service': f'{lifecycle_root}/open_session',
        'close_session_service': f'{lifecycle_root}/close_session',
        'submit_command_service': f'{lifecycle_root}/submit_command',
        'reconcile_command_service': f'{lifecycle_root}/reconcile',
        # The xArm adapter owns this vendor vocabulary. Generic Composer uses
        # only the declared bounded subset as material history facets.
        'material_adapter_status_keys': [
            'vendor_mode',
            'vendor_state',
            'brake_mask',
            'servo_enable_mask',
            'error_code',
            'warning_code',
        ],
        'caller_assurance': 'unverified',
        # Failure-detection policy, not an operator/browser inactivity timer.
        # Physical release still requires measured jitter/loss qualification.
        **_AUTHORITY_HEALTH_POLICY,
    }
    cell_supervisor_id = f"{runtime_values['cell_id']}:cell-supervisor"
    execution_backend_id = (
        f"{runtime_values['robot_id']}:execution-backend"
    )
    authority_component_id = (
        f"{runtime_values['robot_id']}:control-authority:"
        f"{runtime_values['authority_profile']}"
    )
    cell_supervision = {
        'schema': 'manyforge.cell_supervision.v1',
        'cell_id': runtime_values['cell_id'],
        'robot_ids': [runtime_values['robot_id']],
        'authority_profile': runtime_values['authority_profile'],
        'supervisor_heartbeat_path': str(
            cell_supervisor_heartbeat_path.resolve()
        ),
        'cell_supervisor_component_epoch': (
            runtime_values['cell_supervisor_component_epoch']
        ),
        'components': [
            {
                'role': 'cell_supervisor',
                'id': cell_supervisor_id,
                'maximum_heartbeat_age_ms': 1_000,
                'maximum_critical_progress_age_ms': 2_000,
                'dependencies': [],
                'restart_domain': 'cell-control',
            },
            {
                'role': 'transport_owner',
                'id': runtime_values['transport_owner_id'],
                'maximum_heartbeat_age_ms': 500,
                'maximum_critical_progress_age_ms': 1_000,
                'dependencies': [cell_supervisor_id],
                'restart_domain': 'robot-control',
                'expected_runtime_generation': generation_id,
                'expected_binding_generation': compiled.generation_id,
            },
            {
                'role': 'lifecycle_provider',
                'id': runtime_values[
                    'lifecycle_provider_instance_id'
                ],
                'maximum_heartbeat_age_ms': 500,
                'maximum_critical_progress_age_ms': 1_000,
                'dependencies': [
                    cell_supervisor_id,
                    runtime_values['transport_owner_id'],
                ],
                'restart_domain': 'robot-control',
                'expected_runtime_generation': generation_id,
                'expected_binding_generation': compiled.generation_id,
            },
            {
                'role': 'execution_backend',
                'id': execution_backend_id,
                'maximum_heartbeat_age_ms': 2_000,
                'maximum_critical_progress_age_ms': 2_000,
                'dependencies': [
                    cell_supervisor_id,
                    runtime_values['transport_owner_id'],
                    runtime_values['lifecycle_provider_instance_id'],
                ],
                'restart_domain': 'execution',
                'expected_runtime_generation': generation_id,
                'expected_binding_generation': compiled.generation_id,
            },
            {
                'role': 'control_authority',
                'id': authority_component_id,
                'maximum_heartbeat_age_ms': 2_000,
                'maximum_critical_progress_age_ms': 2_000,
                'dependencies': [
                    cell_supervisor_id,
                    runtime_values['lifecycle_provider_instance_id'],
                    execution_backend_id,
                ],
                'restart_domain': 'authority',
                'expected_runtime_generation': generation_id,
                'expected_binding_generation': compiled.generation_id,
            },
        ],
    }
    result = {
        'schema': RUNTIME_BINDING_SCHEMA,
        'generation_id': generation_id,
        'source_binding': {
            'generation_id': manifest.get('generation_id'),
            'manifest_sha256': binding_manifest_sha256,
            'controller_binding_generation_id': recorded.get(
                'generation_id'
            ),
        },
        'runtime': {
            'urdf_path': str(output_urdf_path),
            'urdf_sha256': identity['runtime_urdf_sha256'],
            'template_urdf_sha256': identity['template_urdf_sha256'],
            'controllers_yaml_sha256': (
                identity['controllers_yaml_sha256']
            ),
            'robot_ip': runtime_values['robot_ip'],
            'robot_instance_id': runtime_values['robot_id'],
            'transport_owner_id': runtime_values['transport_owner_id'],
            'lifecycle_provider_instance_id': (
                runtime_values['lifecycle_provider_instance_id']
            ),
            'authority_profile': runtime_values['authority_profile'],
            'cell_id': runtime_values['cell_id'],
            'address_retained': True,
        },
        'controller_binding': compiled.to_dict(),
        'ros2_control_execution_binding': execution_binding,
        'execution_admission': execution_admission,
        'robot_lifecycle_provider': lifecycle_provider,
        'manyforge_execution_config': {
            'action_name': compiled.follow_joint_trajectory_action,
            'arm_action': compiled.follow_joint_trajectory_action,
            'execution_state_authority_profile': 'observed_robot',
            'execution_state_source_mode': 'observed_robot',
            'observed_state_authority_profile_id': (
                'physical_robot_measured_v1'
            ),
            'observed_state_source_precedence': [
                'controller_state',
                'joint_state',
                'action_feedback',
            ],
            'require_fresh_state_before_planning': True,
            'disable_controller_path_tolerances': False,
            'reset_to_initial_on_load': False,
            'joint_state_topic': compiled.joint_state_topic,
            'controller_state_topic': compiled.controller_state_topic,
            'required_joints': list(compiled.arm_joint_names),
            'joint_state_max_age_ms': 250,
            'wait_for_state_timeout_ms': 1000,
            'wait_for_action_server_timeout_ms': 2000,
            'stop_completion_timeout_ms': 1500,
            'controller_binding_refresh_timeout_ms': 250,
            'enable_jtc_speed_scaling_input': False,
            'decelerate_on_cancel': compiled.decelerate_on_cancel,
            'velocity_state_interface_joints': (
                list(compiled.arm_joint_names)
            ),
            'max_deceleration_on_cancel_rad_s2': dict(
                compiled.max_deceleration_on_cancel_rad_s2
            ),
            'controller_binding': execution_binding,
            'require_execution_admission_snapshot': True,
            'execution_admission': execution_admission,
            'robot_lifecycle_provider': lifecycle_provider,
            'cell_supervision': cell_supervision,
        },
        'command_surface': {
            'transport_owner': 'xarm_ros2_ros2_control',
            'manyforge_transport': 'restricted_ros_only',
            'on_init_transport': 'closed',
            'on_configure_transport': 'observation_connect',
            'motion_admission_initial': 'closed',
            'automatic_startup_sequence': False,
            'legacy_command_endpoints': 'suppressed',
            'supervised_state_topic': (
                compiled.supervised_state_topic
            ),
            'supervised_execute_service': (
                compiled.supervised_execute_service
            ),
            'supervised_command_gate_service': (
                compiled.supervised_command_gate_service
            ),
            'supervised_shutdown_controller_service': (
                compiled.supervised_shutdown_controller_service
            ),
        },
        'cell_supervision': cell_supervision,
    }
    serialized_manifest = (
        json.dumps(result, indent=2, sort_keys=True) + '\n'
    )
    _atomic_text(output_urdf_path, runtime)
    _atomic_text(output_manifest_path, serialized_manifest)
    return result


def main() -> int:
    """CLI entry point."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binding-manifest', type=Path, required=True)
    parser.add_argument('--model-root', type=Path, required=True)
    parser.add_argument('--robot-ip', required=True)
    parser.add_argument('--robot-instance-id', required=True)
    parser.add_argument('--transport-owner-id', required=True)
    parser.add_argument('--lifecycle-provider-instance-id', required=True)
    parser.add_argument(
        '--authority-profile',
        choices=sorted(_AUTHORITY_PROFILES),
        required=True,
    )
    parser.add_argument('--cell-id', required=True)
    parser.add_argument(
        '--cell-supervisor-component-epoch',
        required=True,
    )
    parser.add_argument(
        '--cell-supervisor-heartbeat-path',
        type=Path,
        required=True,
    )
    parser.add_argument('--output-urdf', type=Path, required=True)
    parser.add_argument('--output-manifest', type=Path, required=True)
    args = parser.parse_args()
    result = resolve_supervised_runtime_binding(
        binding_manifest_path=args.binding_manifest.resolve(),
        model_root=args.model_root.resolve(),
        robot_ip=args.robot_ip,
        robot_instance_id=args.robot_instance_id,
        transport_owner_id=args.transport_owner_id,
        lifecycle_provider_instance_id=(
            args.lifecycle_provider_instance_id
        ),
        authority_profile=args.authority_profile,
        cell_id=args.cell_id,
        cell_supervisor_component_epoch=(
            args.cell_supervisor_component_epoch
        ),
        cell_supervisor_heartbeat_path=(
            args.cell_supervisor_heartbeat_path
        ),
        output_urdf_path=args.output_urdf.resolve(),
        output_manifest_path=args.output_manifest.resolve(),
    )
    print(result['generation_id'])
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
