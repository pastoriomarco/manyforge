"""Capture bounded, command-free Lite6 qualification evidence."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime
from datetime import timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
from typing import Callable
from typing import Sequence

from .binding_manifest import BINDING_SCHEMA


EVIDENCE_SCHEMA = 'manyforge.xarm_adapter.live_evidence.v1'
MAX_PROBE_OUTPUT_BYTES = 1024 * 1024
DEFAULT_PROBE_TIMEOUT_S = 5.0


@dataclass(frozen=True)
class Probe:
    """One read-only ROS graph or state observation."""

    label: str
    argv: tuple[str, ...]
    postcondition: str
    required: bool = True


@dataclass(frozen=True)
class CommandOutcome:
    """Bounded result returned by a probe runner."""

    return_code: int
    stdout: bytes
    stderr: bytes
    timed_out: bool
    duration_ms: float


Runner = Callable[[Sequence[str], float], CommandOutcome]


def _sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _atomic_json(path: Path, payload: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f'.{path.name}.{os.getpid()}.tmp')
    with temporary.open('x', encoding='utf-8') as handle:
        json.dump(payload, handle, indent=2, sort_keys=True)
        handle.write('\n')
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temporary, path)


def _subprocess_runner(argv: Sequence[str], timeout_s: float) -> CommandOutcome:
    started = time.monotonic()
    try:
        result = subprocess.run(
            list(argv),
            check=False,
            capture_output=True,
            timeout=timeout_s,
        )
        return CommandOutcome(
            return_code=result.returncode,
            stdout=result.stdout,
            stderr=result.stderr,
            timed_out=False,
            duration_ms=(time.monotonic() - started) * 1000.0,
        )
    except subprocess.TimeoutExpired as exc:
        stdout = exc.stdout if isinstance(exc.stdout, bytes) else b''
        stderr = exc.stderr if isinstance(exc.stderr, bytes) else b''
        return CommandOutcome(
            return_code=124,
            stdout=stdout,
            stderr=stderr,
            timed_out=True,
            duration_ms=(time.monotonic() - started) * 1000.0,
        )


def _binding(path: Path) -> dict[str, object]:
    payload = json.loads(path.read_text(encoding='utf-8'))
    if not isinstance(payload, dict) or payload.get('schema') != BINDING_SCHEMA:
        raise ValueError(f'{path} is not a {BINDING_SCHEMA} document')
    if not isinstance(payload.get('binding'), dict):
        raise ValueError('binding manifest has no resolved endpoint binding')
    if not isinstance(payload.get('controller_binding'), dict):
        raise ValueError('binding manifest has no production controller binding')
    return payload


def _read_only_probes(manifest: dict[str, object]) -> list[Probe]:
    binding = manifest['binding']
    assert isinstance(binding, dict)
    diagnostics = str(binding['diagnostics_topic'])
    robot_state = str(binding['robot_state_topic'])
    joint_state = str(binding['joint_state_topic'])
    validated = str(binding['validated_joint_state_topic'])
    return [
        Probe(
            'nodes',
            ('ros2', 'node', 'list', '--no-daemon'),
            'read_only_nodes',
        ),
        Probe(
            'actions',
            ('ros2', 'action', 'list'),
            'empty',
        ),
        Probe(
            'services',
            ('ros2', 'service', 'list', '--no-daemon'),
            'no_xarm_command_services',
        ),
        Probe(
            'diagnostics',
            ('ros2', 'topic', 'echo', '--once', diagnostics),
            'read_only_diagnostics',
        ),
        Probe(
            'robot_state',
            ('ros2', 'topic', 'echo', '--once', robot_state),
            'nonempty',
        ),
        Probe(
            'joint_state',
            ('ros2', 'topic', 'echo', '--once', joint_state),
            'nonempty',
        ),
        Probe(
            'validated_joint_state',
            ('ros2', 'topic', 'echo', '--once', validated),
            'nonempty',
            required=False,
        ),
    ]


def _namespaced_node(binding: dict[str, object], name: str) -> str:
    namespace = str(binding.get('ros_namespace', '')).strip('/')
    return '/' + '/'.join(part for part in (namespace, name) if part)


def _supervised_preflight_probes(
    manifest: dict[str, object],
) -> list[Probe]:
    controller = manifest['controller_binding']
    assert isinstance(controller, dict)
    manager = str(controller['controller_manager'])
    activity = str(controller['controller_manager_activity_topic'])
    return [
        Probe(
            'nodes',
            ('ros2', 'node', 'list', '--no-daemon'),
            'nonempty',
        ),
        Probe(
            'hardware_components',
            (
                'ros2',
                'control',
                'list_hardware_components',
                '--controller-manager',
                manager,
            ),
            'active_production_hardware_component',
        ),
        Probe(
            'hardware_interfaces',
            (
                'ros2',
                'control',
                'list_hardware_interfaces',
                '--controller-manager',
                manager,
            ),
            'claimed_production_interfaces',
        ),
        Probe(
            'controllers',
            (
                'ros2',
                'control',
                'list_controllers',
                '--controller-manager',
                manager,
                '--verbose',
            ),
            'active_production_controller',
        ),
        Probe(
            'controller_activity',
            ('ros2', 'topic', 'echo', '--once', activity),
            'active_controller_activity',
        ),
    ]


def _postcondition(
    name: str,
    output: str,
    manifest: dict[str, object],
) -> tuple[bool, str]:
    binding = manifest['binding']
    controller = manifest['controller_binding']
    assert isinstance(binding, dict)
    assert isinstance(controller, dict)
    if name == 'nonempty':
        return (bool(output.strip()), 'output_observed')
    if name == 'empty':
        return (not output.strip(), 'no_endpoint_observed')
    if name == 'read_only_nodes':
        expected = {
            str(binding['driver_node']),
            _namespaced_node(binding, 'lite6_robot_state_publisher'),
            _namespaced_node(
                binding,
                'manyforge_xarm_read_only_observer',
            ),
        }
        actual = {line.strip() for line in output.splitlines() if line.strip()}
        missing = sorted(expected - actual)
        unexpected = sorted(actual - expected)
        return (
            not missing and not unexpected,
            (
                'expected_read_only_graph'
                if not missing and not unexpected
                else f'missing={missing};unexpected={unexpected}'
            ),
        )
    if name == 'no_xarm_command_services':
        forbidden_tokens = (
            'clean_error',
            'clean_warn',
            'motion_enable',
            'set_mode',
            'set_state',
            'set_servo_angle',
            'set_servo_cartesian',
            'set_joint_velocity',
            'set_position',
            'set_tool_position',
            'set_cgpio',
            'gripper',
            'controller_manager',
        )
        found = sorted(
            token for token in forbidden_tokens if token in output.lower()
        )
        allowed_nodes = {
            str(binding['driver_node']),
            _namespaced_node(binding, 'lite6_robot_state_publisher'),
            _namespaced_node(
                binding,
                'manyforge_xarm_read_only_observer',
            ),
        }
        allowed_leaves = {
            'describe_parameters',
            'get_logger_levels',
            'get_parameter_types',
            'get_parameters',
            'get_type_description',
            'list_parameters',
            'set_logger_levels',
            'set_parameters',
            'set_parameters_atomically',
        }
        unexpected = []
        for service in (
            line.strip() for line in output.splitlines() if line.strip()
        ):
            parent, separator, leaf = service.rpartition('/')
            if (
                not separator
                or parent not in allowed_nodes
                or leaf not in allowed_leaves
            ):
                unexpected.append(service)
        unexpected.sort()
        passed = not found and not unexpected
        return (
            passed,
            (
                'only_introspection_services'
                if passed
                else f'forbidden={found};unexpected={unexpected}'
            ),
        )
    if name == 'read_only_diagnostics':
        required = (
            'key: access_mode',
            'value: read_only',
            'key: motion_admission',
            'value: closed',
            'key: command_endpoints',
            'value: suppressed',
        )
        missing = [token for token in required if token not in output]
        return (
            not missing,
            'read_only_fail_closed' if not missing else f'missing={missing}',
        )
    if name == 'active_production_hardware_component':
        expected = str(controller['hardware_component_name'])
        component_start = output.find(expected)
        component_block = (
            output[component_start:] if component_start >= 0 else ''
        )
        next_component = component_block.find(
            'Hardware Component',
            len(expected),
        )
        if next_component >= 0:
            component_block = component_block[:next_component]
        active = (
            component_start >= 0
            and 'active' in component_block.lower()
        )
        return (
            active,
            (
                'active_hardware_component_observed'
                if active
                else f'missing_or_inactive={expected}'
            ),
        )
    if name == 'active_production_controller':
        expected = str(controller['trajectory_controller_name']).rsplit(
            '/', 1
        )[-1]
        expected_type = str(controller['trajectory_controller_type'])
        matched = [
            line.lower()
            for line in output.splitlines()
            if expected in line
        ]
        active = any('active' in line for line in matched)
        exact_type = expected_type in output
        return (
            active and exact_type,
            (
                'active_exact_trajectory_controller_observed'
                if active and exact_type
                else (
                    f'missing_or_inactive={expected};'
                    f'type_observed={exact_type}'
                )
            ),
        )
    if name == 'active_controller_activity':
        expected = str(controller['trajectory_controller_name']).rsplit(
            '/', 1
        )[-1]
        active = expected in output and 'state: active' in output.lower()
        return (
            active,
            (
                'active_controller_activity_observed'
                if active
                else f'missing_or_inactive={expected}'
            ),
        )
    if name == 'claimed_production_interfaces':
        expected_commands = controller['claimed_command_interface_names']
        expected_states = controller['state_interface_names']
        lower_output = output.lower()
        command_heading = lower_output.find('command interfaces')
        state_heading = lower_output.find('state interfaces')
        if (
            command_heading < 0
            or state_heading < 0
            or state_heading <= command_heading
        ):
            return (False, 'command_or_state_interface_section_missing')
        command_lines = [
            line.strip()
            for line in output[command_heading:state_heading].splitlines()
        ]
        state_lines = [
            line.strip()
            for line in output[state_heading:].splitlines()
        ]
        missing_commands = [
            value
            for value in expected_commands
            if not any(
                value in line and '[claimed]' in line.lower()
                for line in command_lines
            )
        ]
        missing_states = [
            value
            for value in expected_states
            if not any(value in line for line in state_lines)
        ]
        passed = not missing_commands and not missing_states
        return (
            passed,
            (
                'all_interfaces_observed_and_commands_claimed'
                if passed
                else (
                    f'unclaimed_or_missing_commands={missing_commands};'
                    f'missing_states={missing_states}'
                )
            ),
        )
    raise ValueError(f'unsupported evidence postcondition: {name}')


def capture_live_evidence(
    *,
    mode: str,
    binding_manifest_path: Path,
    output_root: Path,
    runner: Runner = _subprocess_runner,
    timeout_s: float = DEFAULT_PROBE_TIMEOUT_S,
    captured_at: datetime | None = None,
) -> dict[str, object]:
    """Capture one bounded observation set and return its summary."""
    if timeout_s <= 0:
        raise ValueError('probe timeout must be positive')
    manifest = _binding(binding_manifest_path)
    if mode == 'read_only':
        probes = _read_only_probes(manifest)
    elif mode == 'supervised_preflight':
        probes = _supervised_preflight_probes(manifest)
    else:
        raise ValueError(f'unsupported live evidence mode: {mode}')

    instant = captured_at or datetime.now(timezone.utc)
    if instant.tzinfo is None:
        raise ValueError('captured_at must be timezone-aware')
    output_root.mkdir(parents=True, exist_ok=False)
    raw_root = output_root / 'raw'
    raw_root.mkdir()
    events: list[dict[str, object]] = []
    required_failures = 0
    optional_unavailable = 0
    for sequence, probe in enumerate(probes, start=1):
        outcome = runner(probe.argv, timeout_s)
        combined = outcome.stdout + (
            b'\n--- stderr ---\n' + outcome.stderr if outcome.stderr else b''
        )
        truncated = len(combined) > MAX_PROBE_OUTPUT_BYTES
        retained = combined[:MAX_PROBE_OUTPUT_BYTES]
        raw_path = raw_root / f'{sequence:02d}-{probe.label}.txt'
        raw_path.write_bytes(retained)
        decoded = outcome.stdout.decode('utf-8', errors='replace')
        passed, reason = _postcondition(
            probe.postcondition,
            decoded,
            manifest,
        )
        passed = (
            passed
            and outcome.return_code == 0
            and not outcome.timed_out
            and not truncated
        )
        if not passed:
            if probe.required:
                required_failures += 1
            else:
                optional_unavailable += 1
        events.append(
            {
                'schema': EVIDENCE_SCHEMA,
                'sequence': sequence,
                'kind': 'observation_probe',
                'mode': mode,
                'label': probe.label,
                'argv': list(probe.argv),
                'required': probe.required,
                'timeout_is_success': False,
                'timeout_s': timeout_s,
                'return_code': outcome.return_code,
                'timed_out': outcome.timed_out,
                'duration_ms': outcome.duration_ms,
                'output_path': str(raw_path.relative_to(output_root)),
                'output_sha256': _sha256_bytes(retained),
                'output_size_bytes': len(retained),
                'output_truncated': truncated,
                'postcondition': probe.postcondition,
                'postcondition_passed': passed,
                'postcondition_reason': reason,
            }
        )

    binding_digest = hashlib.sha256(
        binding_manifest_path.read_bytes()
    ).hexdigest()
    result = 'pass' if required_failures == 0 else 'fail'
    summary = {
        'schema': EVIDENCE_SCHEMA,
        'captured_at_utc': instant.astimezone(timezone.utc).isoformat(),
        'mode': mode,
        'binding_manifest_sha256': binding_digest,
        'binding_generation_id': manifest['generation_id'],
        'controller_binding_generation_id': manifest[
            'controller_binding'
        ]['generation_id'],
        'probe_count': len(events),
        'required_failure_count': required_failures,
        'optional_unavailable_count': optional_unavailable,
        'result': result,
        'events': events,
        'scope': (
            'observation_only_no_lifecycle_or_motion_command'
        ),
    }
    _atomic_json(output_root / 'evidence.json', summary)
    with (output_root / 'events.jsonl').open('x', encoding='utf-8') as handle:
        for event in events:
            handle.write(json.dumps(event, sort_keys=True) + '\n')
        handle.flush()
        os.fsync(handle.fileno())
    return summary


def main() -> int:
    """CLI entry point."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        '--mode',
        choices=('read_only', 'supervised_preflight'),
        required=True,
    )
    parser.add_argument('--binding-manifest', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument(
        '--timeout-s',
        type=float,
        default=DEFAULT_PROBE_TIMEOUT_S,
    )
    args = parser.parse_args()
    summary = capture_live_evidence(
        mode=args.mode,
        binding_manifest_path=args.binding_manifest.resolve(),
        output_root=args.output.resolve(),
        timeout_s=args.timeout_s,
    )
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0 if summary['result'] == 'pass' else 1


if __name__ == '__main__':
    raise SystemExit(main())
