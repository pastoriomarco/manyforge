"""Capture one attended Lite6 lifecycle command with bounded evidence."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime
from datetime import timezone
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import time
from typing import Any
from typing import Callable
from typing import Sequence

from ament_index_python.packages import get_package_prefix

from .lifecycle_transient import LifecycleTransientSampler
from .supervised_runtime_binding import RUNTIME_BINDING_SCHEMA


EVIDENCE_SCHEMA = (
    'manyforge.xarm_adapter.supervised_command_evidence.v1'
)
COMMAND_RESULT_SCHEMA = (
    'manyforge.xarm_control.lifecycle_command_evidence.v2'
)
MAX_OUTPUT_BYTES = 1024 * 1024


@dataclass(frozen=True)
class CommandOutcome:
    """Bounded subprocess result."""

    return_code: int
    stdout: bytes
    stderr: bytes
    timed_out: bool
    duration_ms: float


Runner = Callable[[Sequence[str], float], CommandOutcome]
TransientSamplerFactory = Callable[
    [dict[str, Any]],
    LifecycleTransientSampler,
]
_TRANSIENT_ACTIONS = frozenset({'robot.enable', 'robot.start_control'})


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


def _subprocess_runner(
    argv: Sequence[str],
    timeout_s: float,
) -> CommandOutcome:
    started = time.monotonic()
    try:
        completed = subprocess.run(
            list(argv),
            check=False,
            capture_output=True,
            timeout=timeout_s,
        )
        return CommandOutcome(
            return_code=completed.returncode,
            stdout=completed.stdout,
            stderr=completed.stderr,
            timed_out=False,
            duration_ms=(time.monotonic() - started) * 1000.0,
        )
    except subprocess.TimeoutExpired as error:
        stdout = error.stdout if isinstance(error.stdout, bytes) else b''
        stderr = error.stderr if isinstance(error.stderr, bytes) else b''
        return CommandOutcome(
            return_code=124,
            stdout=stdout,
            stderr=stderr,
            timed_out=True,
            duration_ms=(time.monotonic() - started) * 1000.0,
        )


def _runtime_binding(path: Path) -> dict[str, Any]:
    payload = json.loads(path.read_text(encoding='utf-8'))
    if (
        not isinstance(payload, dict)
        or payload.get('schema') != RUNTIME_BINDING_SCHEMA
    ):
        raise ValueError(
            f'{path} is not a {RUNTIME_BINDING_SCHEMA} document'
        )
    runtime = payload.get('runtime')
    controller = payload.get('controller_binding')
    lifecycle = payload.get('robot_lifecycle_provider')
    if (
        not isinstance(runtime, dict)
        or not isinstance(controller, dict)
        or not isinstance(lifecycle, dict)
    ):
        raise ValueError(
            'runtime binding is missing runtime/controller/lifecycle data'
        )
    if runtime.get('authority_profile') != 'local_operator':
        raise ValueError(
            'attended command evidence requires local_operator authority'
        )
    if (
        lifecycle.get('schema')
        != 'manyforge.robot_lifecycle.ros_provider.v1'
        or lifecycle.get('caller_assurance') != 'unverified'
    ):
        raise ValueError(
            'attended command evidence requires the explicit unverified '
            'commissioning caller contract'
        )
    if lifecycle.get('robot_id') != runtime.get('robot_instance_id'):
        raise ValueError('runtime and lifecycle robot identities disagree')
    required_controller_fields = (
        'controller_manager',
        'hardware_component_name',
        'joint_state_broadcaster_manager_name',
        'supervised_state_topic',
        'supervised_execute_service',
        'supervised_command_gate_service',
        'supervised_shutdown_controller_service',
        'controller_manager_activity_topic',
        'trajectory_controller_name',
    )
    for field in required_controller_fields:
        value = controller.get(field)
        if (
            not isinstance(value, str)
            or not value
            or '\x00' in value
            or '\n' in value
        ):
            raise ValueError(
                f'controller_binding.{field} must be a non-empty string'
            )
    return payload


def _command_argv(
    *,
    binding: dict[str, Any],
    action_id: str,
    request_id: str,
    authority_session_id: str,
    command_source: str,
    caller_context_id: str,
    timeout_ms: int,
    sample_period_ms: int,
) -> tuple[str, ...]:
    runtime = binding['runtime']
    controller = binding['controller_binding']
    controller_name = str(controller['trajectory_controller_name']).rsplit(
        '/', 1
    )[-1]
    executable = (
        Path(get_package_prefix('manyforge_xarm_control'))
        / 'lib'
        / 'manyforge_xarm_control'
        / 'lite6_lifecycle_command_evidence'
    )
    if not executable.is_file() or not os.access(executable, os.X_OK):
        raise ValueError(
            'installed lifecycle command evidence executable is unavailable'
        )
    return (
        str(executable),
        '--robot-id',
        str(runtime['robot_instance_id']),
        '--supervisor-instance-id',
        f"{runtime['robot_instance_id']}:attended-evidence",
        '--owner-id',
        str(runtime['transport_owner_id']),
        '--state-topic',
        str(controller['supervised_state_topic']),
        '--execute-service',
        str(controller['supervised_execute_service']),
        '--command-gate-service',
        str(controller['supervised_command_gate_service']),
        '--shutdown-controller-service',
        str(controller['supervised_shutdown_controller_service']),
        '--controller-manager',
        str(controller['controller_manager']),
        '--hardware-component-name',
        str(controller['hardware_component_name']),
        '--joint-state-broadcaster-name',
        str(controller['joint_state_broadcaster_manager_name']),
        '--controller-activity-topic',
        str(controller['controller_manager_activity_topic']),
        '--trajectory-controller-name',
        controller_name,
        '--action-id',
        action_id,
        '--confirm-action',
        action_id,
        '--request-id',
        request_id,
        '--authority-session-id',
        authority_session_id,
        '--command-source',
        command_source,
        '--caller-context-id',
        caller_context_id,
        '--confirmation-evidence',
        f'CONFIRM:{action_id}',
        '--timeout-ms',
        str(timeout_ms),
        '--sample-period-ms',
        str(sample_period_ms),
    )


def _parse_command_result(stdout: bytes) -> dict[str, Any]:
    try:
        payload = json.loads(stdout.decode('utf-8'))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError(
            'lifecycle command did not emit one exact JSON document'
        ) from error
    if (
        not isinstance(payload, dict)
        or payload.get('schema') != COMMAND_RESULT_SCHEMA
    ):
        raise ValueError(
            f'lifecycle command output is not {COMMAND_RESULT_SCHEMA}'
        )
    return payload


def capture_supervised_command_evidence(
    *,
    runtime_binding_path: Path,
    output_root: Path,
    action_id: str,
    request_id: str,
    authority_session_id: str,
    command_source: str,
    caller_context_id: str,
    timeout_ms: int = 8000,
    sample_period_ms: int = 20,
    transient_capture_ms: int = 3000,
    max_transient_deviation_rad: float | None = None,
    runner: Runner = _subprocess_runner,
    transient_sampler_factory: TransientSamplerFactory = (
        LifecycleTransientSampler
    ),
    captured_at: datetime | None = None,
) -> dict[str, Any]:
    """Capture exactly one gateway-routed command and its postcondition."""
    if timeout_ms < 1000 or timeout_ms > 30000:
        raise ValueError('timeout_ms must be between 1000 and 30000')
    if (
        sample_period_ms < 5
        or sample_period_ms > 1000
        or sample_period_ms >= timeout_ms
    ):
        raise ValueError('sample_period_ms is outside its safe bounds')
    if transient_capture_ms < 100 or transient_capture_ms > 10000:
        raise ValueError('transient_capture_ms must be between 100 and 10000')
    if (
        max_transient_deviation_rad is not None
        and (
            not isinstance(max_transient_deviation_rad, (int, float))
            or not math.isfinite(max_transient_deviation_rad)
            or max_transient_deviation_rad <= 0.0
        )
    ):
        raise ValueError(
            'max_transient_deviation_rad must be finite and positive'
        )
    for label, value in (
        ('action_id', action_id),
        ('request_id', request_id),
        ('authority_session_id', authority_session_id),
        ('command_source', command_source),
        ('caller_context_id', caller_context_id),
    ):
        if not value or len(value) > 128:
            raise ValueError(f'{label} must be a bounded identifier')

    binding = _runtime_binding(runtime_binding_path)
    instant = captured_at or datetime.now(timezone.utc)
    if instant.tzinfo is None:
        raise ValueError('captured_at must be timezone-aware')
    output_root.mkdir(parents=True, exist_ok=False)
    raw_root = output_root / 'raw'
    raw_root.mkdir()

    argv = _command_argv(
        binding=binding,
        action_id=action_id,
        request_id=request_id,
        authority_session_id=authority_session_id,
        command_source=command_source,
        caller_context_id=caller_context_id,
        timeout_ms=timeout_ms,
        sample_period_ms=sample_period_ms,
    )
    process_timeout_s = timeout_ms / 1000.0 + 5.0
    transient: dict[str, object] | None = None
    transient_sampler: LifecycleTransientSampler | None = None
    transient_error = ''
    command_started_ns = time.monotonic_ns()
    if action_id in _TRANSIENT_ACTIONS:
        try:
            transient_sampler = transient_sampler_factory(binding)
            transient_sampler.wait_until_ready(1000)
            command_started_ns = time.monotonic_ns()
        except (RuntimeError, TimeoutError, ValueError) as error:
            transient_error = str(error)
    if transient_error:
        outcome = CommandOutcome(
            return_code=125,
            stdout=b'',
            stderr=transient_error.encode('utf-8', errors='replace'),
            timed_out=False,
            duration_ms=0.0,
        )
        command_finished_ns = time.monotonic_ns()
        if transient_sampler is not None:
            try:
                transient_sampler.close()
            except RuntimeError as error:
                transient_error = (
                    f'{transient_error};'
                    f'lifecycle_transient_close_failed:{error}'
                )
            transient_sampler = None
    else:
        outcome = runner(argv, process_timeout_s)
        command_finished_ns = time.monotonic_ns()
    if transient_sampler is not None:
        try:
            transient = transient_sampler.finish(
                command_started_ns=command_started_ns,
                command_finished_ns=command_finished_ns,
                capture_window_ms=transient_capture_ms,
                max_deviation_rad=max_transient_deviation_rad,
            )
        except (RuntimeError, TimeoutError, ValueError) as error:
            transient_error = str(error)
        finally:
            try:
                transient_sampler.close()
            except RuntimeError as error:
                transient_error = (
                    transient_error
                    or f'lifecycle_transient_close_failed:{error}'
                )
    transient_failure_reason = ''
    if transient is not None:
        if transient.get('capture_complete') is not True:
            transient_failure_reason = 'lifecycle_transient_incomplete'
        elif transient.get('assessment') == 'exceeds_explicit_bound':
            transient_failure_reason = (
                'lifecycle_transient_exceeds_explicit_bound'
            )
    combined = outcome.stdout + (
        b'\n--- stderr ---\n' + outcome.stderr if outcome.stderr else b''
    )
    truncated = len(combined) > MAX_OUTPUT_BYTES
    retained = combined[:MAX_OUTPUT_BYTES]
    raw_path = raw_root / '01-lifecycle-command.txt'
    raw_path.write_bytes(retained)

    command_result: dict[str, Any] | None = None
    parse_error = ''
    if truncated:
        parse_error = 'lifecycle command output exceeded the evidence bound'
    else:
        try:
            command_result = _parse_command_result(outcome.stdout)
        except ValueError as error:
            parse_error = str(error)
    inner_matches = (
        command_result is not None
        and command_result.get('action_id') == action_id
        and command_result.get('request_id') == request_id
        and command_result.get('authority_session_id')
        == authority_session_id
        and command_result.get('command_source') == command_source
        and command_result.get('caller_context_id') == caller_context_id
        and command_result.get('caller_assurance') == 'unverified'
        and command_result.get('confirmation_evidence')
        == f'CONFIRM:{action_id}'
    )
    passed = (
        outcome.return_code == 0
        and not outcome.timed_out
        and not truncated
        and command_result is not None
        and command_result.get('result') == 'pass'
        and inner_matches
        and not transient_error
        and not transient_failure_reason
        and (
            transient is None
            or (
                transient.get('capture_complete') is True
                and transient.get('assessment')
                != 'exceeds_explicit_bound'
            )
        )
    )
    event = {
        'schema': EVIDENCE_SCHEMA,
        'sequence': 1,
        'kind': 'attended_lifecycle_command',
        'action_id': action_id,
        'request_id': request_id,
        'authority_session_id': authority_session_id,
        'command_source': command_source,
        'caller_context_id': caller_context_id,
        'caller_assurance': 'unverified',
        'argv': list(argv),
        'timeout_is_success': False,
        'timeout_s': process_timeout_s,
        'return_code': outcome.return_code,
        'timed_out': outcome.timed_out,
        'duration_ms': outcome.duration_ms,
        'output_path': str(raw_path.relative_to(output_root)),
        'output_sha256': _sha256_bytes(retained),
        'output_size_bytes': len(retained),
        'output_truncated': truncated,
        'command_result_parsed': command_result is not None,
        'command_result_matches_request': inner_matches,
        'command_result': command_result,
        'lifecycle_transient': transient,
        'lifecycle_transient_error': transient_error,
        'lifecycle_transient_failure_reason': (
            transient_failure_reason
        ),
        'failure_reason': (
            ''
            if passed
            else (
                'subprocess_timeout'
                if outcome.timed_out
                else parse_error
            )
            or transient_error
            or transient_failure_reason
            or (
                str(command_result.get('failure_reason', 'command_failed'))
                if command_result is not None
                else 'command_failed'
            )
        ),
        'passed': passed,
    }
    runtime_binding_bytes = runtime_binding_path.read_bytes()
    summary = {
        'schema': EVIDENCE_SCHEMA,
        'captured_at_utc': instant.astimezone(timezone.utc).isoformat(),
        'result': 'pass' if passed else 'fail',
        'scope': (
            'one_attended_action_via_local_gateway_and_restricted_ros'
        ),
        'runtime_binding_sha256': _sha256_bytes(runtime_binding_bytes),
        'runtime_binding_generation_id': binding['generation_id'],
        'source_binding_manifest_sha256': binding['source_binding'][
            'manifest_sha256'
        ],
        'action_id': action_id,
        'request_id': request_id,
        'authority_session_id': authority_session_id,
        'command_source': command_source,
        'caller_context_id': caller_context_id,
        'caller_assurance': 'unverified',
        'event': event,
    }
    _atomic_json(output_root / 'evidence.json', summary)
    with (output_root / 'events.jsonl').open(
        'x', encoding='utf-8'
    ) as handle:
        handle.write(json.dumps(event, sort_keys=True) + '\n')
        handle.flush()
        os.fsync(handle.fileno())
    return summary


def main() -> int:
    """CLI entry point."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime-binding', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--action-id', required=True)
    parser.add_argument('--confirm-action', required=True)
    parser.add_argument('--request-id', required=True)
    parser.add_argument('--authority-session-id', required=True)
    parser.add_argument(
        '--command-source',
        default='composer.local_operator',
    )
    parser.add_argument(
        '--caller-context-id',
        default='local-attended-qualification',
    )
    parser.add_argument('--timeout-ms', type=int, default=8000)
    parser.add_argument('--sample-period-ms', type=int, default=20)
    parser.add_argument('--transient-capture-ms', type=int, default=3000)
    parser.add_argument('--max-transient-deviation-rad', type=float)
    arguments = parser.parse_args()
    if arguments.confirm_action != arguments.action_id:
        parser.error('--confirm-action must exactly equal --action-id')
    summary = capture_supervised_command_evidence(
        runtime_binding_path=arguments.runtime_binding.resolve(),
        output_root=arguments.output.resolve(),
        action_id=arguments.action_id,
        request_id=arguments.request_id,
        authority_session_id=arguments.authority_session_id,
        command_source=arguments.command_source,
        caller_context_id=arguments.caller_context_id,
        timeout_ms=arguments.timeout_ms,
        sample_period_ms=arguments.sample_period_ms,
        transient_capture_ms=arguments.transient_capture_ms,
        max_transient_deviation_rad=(
            arguments.max_transient_deviation_rad
        ),
    )
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0 if summary['result'] == 'pass' else 1


if __name__ == '__main__':
    raise SystemExit(main())
