from __future__ import annotations

from datetime import datetime
from datetime import timezone
import json
from pathlib import Path
from typing import Sequence

from manyforge_xarm_adapter.supervised_command_evidence import (
    capture_supervised_command_evidence,
)
from manyforge_xarm_adapter.supervised_command_evidence import (
    COMMAND_RESULT_SCHEMA,
)
from manyforge_xarm_adapter.supervised_command_evidence import CommandOutcome
from manyforge_xarm_adapter.supervised_runtime_binding import (
    RUNTIME_BINDING_SCHEMA,
)
import pytest


def _runtime_binding(
    path: Path,
    *,
    authority_profile: str = 'local_operator',
) -> Path:
    payload = {
        'schema': RUNTIME_BINDING_SCHEMA,
        'generation_id': 'a' * 64,
        'runtime': {
            'robot_instance_id': 'lite6-a',
            'transport_owner_id': 'lite6-a:sole-owner',
            'authority_profile': authority_profile,
        },
        'controller_binding': {
            'controller_manager': '/controller_manager',
            'hardware_component_name': 'lite6_system',
            'joint_state_broadcaster_manager_name': (
                'joint_state_broadcaster'
            ),
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
            'controller_manager_activity_topic': (
                '/controller_manager/activity'
            ),
            'trajectory_controller_name': '/lite6_traj_controller',
            'joint_state_topic': '/ufactory/joint_states',
            'arm_joint_names': [
                f'joint{index}' for index in range(1, 7)
            ],
        },
        'source_binding': {
            'manifest_sha256': 'b' * 64,
        },
        'robot_lifecycle_provider': {
            'schema': 'manyforge.robot_lifecycle.ros_provider.v1',
            'robot_id': 'lite6-a',
            'provider_instance_id': 'lite6-a:provider:test',
            'caller_assurance': 'unverified',
        },
    }
    path.write_text(json.dumps(payload), encoding='utf-8')
    return path


def _command_payload(
    *,
    action_id: str = 'robot.stop_control',
    request_id: str = 'request-1',
) -> dict[str, object]:
    return {
        'schema': COMMAND_RESULT_SCHEMA,
        'result': 'pass',
        'failure_reason': '',
        'action_id': action_id,
        'request_id': request_id,
        'authority_session_id': 'session-1',
        'command_source': 'test.local_operator',
        'caller_context_id': 'test-context-1',
        'caller_assurance': 'unverified',
        'confirmation_evidence': f'CONFIRM:{action_id}',
        'primitive_audit': [
            {
                'command': 'set_stopped_state',
                'return_code': 0,
                'permitted': True,
                'attempted': True,
            },
        ],
    }


class FakeRunner:
    def __init__(
        self,
        payload: dict[str, object] | None = None,
        *,
        timed_out: bool = False,
    ) -> None:
        self.payload = payload or _command_payload()
        self.timed_out = timed_out
        self.calls: list[tuple[tuple[str, ...], float]] = []

    def __call__(
        self,
        argv: Sequence[str],
        timeout_s: float,
    ) -> CommandOutcome:
        self.calls.append((tuple(argv), timeout_s))
        if self.timed_out:
            return CommandOutcome(124, b'', b'', True, timeout_s * 1000)
        return CommandOutcome(
            0,
            json.dumps(self.payload).encode(),
            b'',
            False,
            12.5,
        )


class FakeTransientSampler:
    def __init__(
        self,
        _: dict[str, object],
        *,
        capture_complete: bool = True,
        assessment: str = 'captured_requires_bound_adjudication',
    ) -> None:
        self.capture_complete = capture_complete
        self.assessment = assessment
        self.waited = False
        self.closed = False

    def wait_until_ready(self, timeout_ms: int) -> None:
        assert timeout_ms == 1000
        self.waited = True

    def finish(self, **kwargs: object) -> dict[str, object]:
        assert kwargs['capture_window_ms'] == 3000
        return {
            'capture_complete': self.capture_complete,
            'assessment': self.assessment,
            'max_qualified_pose_deviation_rad': 0.0003,
        }

    def close(self) -> None:
        self.closed = True


def test_capture_binds_runtime_action_and_exact_confirmation(
    tmp_path: Path,
) -> None:
    runner = FakeRunner()
    output = tmp_path / 'evidence'

    summary = capture_supervised_command_evidence(
        runtime_binding_path=_runtime_binding(
            tmp_path / 'runtime-binding.json'
        ),
        output_root=output,
        action_id='robot.stop_control',
        request_id='request-1',
        authority_session_id='session-1',
        command_source='test.local_operator',
        caller_context_id='test-context-1',
        runner=runner,
        captured_at=datetime(2026, 7, 25, tzinfo=timezone.utc),
    )

    assert summary['result'] == 'pass'
    assert summary['event']['passed'] is True
    assert len(runner.calls) == 1
    argv = runner.calls[0][0]
    assert argv[argv.index('--action-id') + 1] == 'robot.stop_control'
    assert argv[argv.index('--confirm-action') + 1] == (
        'robot.stop_control'
    )
    assert argv[argv.index('--confirmation-evidence') + 1] == (
        'CONFIRM:robot.stop_control'
    )
    assert argv[argv.index('--trajectory-controller-name') + 1] == (
        'lite6_traj_controller'
    )
    assert argv[argv.index('--controller-manager') + 1] == (
        '/controller_manager'
    )
    assert argv[argv.index('--command-source') + 1] == (
        'test.local_operator'
    )
    assert argv[argv.index('--caller-context-id') + 1] == (
        'test-context-1'
    )
    assert json.loads((output / 'evidence.json').read_text())[
        'result'
    ] == 'pass'
    assert len((output / 'events.jsonl').read_text().splitlines()) == 1


def test_timeout_is_a_failed_command_evidence_gate(
    tmp_path: Path,
) -> None:
    summary = capture_supervised_command_evidence(
        runtime_binding_path=_runtime_binding(
            tmp_path / 'runtime-binding.json'
        ),
        output_root=tmp_path / 'evidence',
        action_id='robot.stop_control',
        request_id='request-1',
        authority_session_id='session-1',
        command_source='test.local_operator',
        caller_context_id='test-context-1',
        runner=FakeRunner(timed_out=True),
    )

    assert summary['result'] == 'fail'
    assert summary['event']['timed_out'] is True
    assert summary['event']['timeout_is_success'] is False
    assert summary['event']['failure_reason'] == 'subprocess_timeout'


def test_mismatched_inner_command_identity_fails_closed(
    tmp_path: Path,
) -> None:
    payload = _command_payload(action_id='robot.disable')
    summary = capture_supervised_command_evidence(
        runtime_binding_path=_runtime_binding(
            tmp_path / 'runtime-binding.json'
        ),
        output_root=tmp_path / 'evidence',
        action_id='robot.stop_control',
        request_id='request-1',
        authority_session_id='session-1',
        command_source='test.local_operator',
        caller_context_id='test-context-1',
        runner=FakeRunner(payload),
    )

    assert summary['result'] == 'fail'
    assert (
        summary['event']['command_result_matches_request'] is False
    )


def test_mismatched_confirmation_evidence_fails_closed(
    tmp_path: Path,
) -> None:
    payload = _command_payload()
    payload['confirmation_evidence'] = 'CONFIRM:robot.disable'
    summary = capture_supervised_command_evidence(
        runtime_binding_path=_runtime_binding(
            tmp_path / 'runtime-binding.json'
        ),
        output_root=tmp_path / 'evidence',
        action_id='robot.stop_control',
        request_id='request-1',
        authority_session_id='session-1',
        command_source='test.local_operator',
        caller_context_id='test-context-1',
        runner=FakeRunner(payload),
    )

    assert summary['result'] == 'fail'
    assert (
        summary['event']['command_result_matches_request'] is False
    )


def test_external_authority_binding_is_rejected(
    tmp_path: Path,
) -> None:
    with pytest.raises(ValueError, match='local_operator'):
        capture_supervised_command_evidence(
            runtime_binding_path=_runtime_binding(
                tmp_path / 'runtime-binding.json',
                authority_profile='plc_roxsie',
            ),
            output_root=tmp_path / 'evidence',
            action_id='robot.stop_control',
            request_id='request-1',
            authority_session_id='session-1',
            command_source='test.local_operator',
            caller_context_id='test-context-1',
            runner=FakeRunner(),
        )


def test_enable_command_captures_unadjudicated_transient(
    tmp_path: Path,
) -> None:
    samplers: list[FakeTransientSampler] = []

    def factory(binding: dict[str, object]) -> FakeTransientSampler:
        sampler = FakeTransientSampler(binding)
        samplers.append(sampler)
        return sampler

    summary = capture_supervised_command_evidence(
        runtime_binding_path=_runtime_binding(
            tmp_path / 'runtime-binding.json'
        ),
        output_root=tmp_path / 'evidence',
        action_id='robot.enable',
        request_id='request-1',
        authority_session_id='session-1',
        command_source='test.local_operator',
        caller_context_id='test-context-1',
        runner=FakeRunner(
            _command_payload(action_id='robot.enable')
        ),
        transient_sampler_factory=factory,
    )

    assert summary['result'] == 'pass'
    transient = summary['event']['lifecycle_transient']
    assert transient['capture_complete'] is True
    assert transient['assessment'] == (
        'captured_requires_bound_adjudication'
    )
    assert samplers[0].waited is True
    assert samplers[0].closed is True


def test_incomplete_enable_transient_fails_evidence(
    tmp_path: Path,
) -> None:
    def factory(binding: dict[str, object]) -> FakeTransientSampler:
        return FakeTransientSampler(
            binding,
            capture_complete=False,
            assessment='capture_incomplete',
        )

    summary = capture_supervised_command_evidence(
        runtime_binding_path=_runtime_binding(
            tmp_path / 'runtime-binding.json'
        ),
        output_root=tmp_path / 'evidence',
        action_id='robot.enable',
        request_id='request-1',
        authority_session_id='session-1',
        command_source='test.local_operator',
        caller_context_id='test-context-1',
        runner=FakeRunner(
            _command_payload(action_id='robot.enable')
        ),
        transient_sampler_factory=factory,
    )

    assert summary['result'] == 'fail'
    assert summary['event']['lifecycle_transient']['assessment'] == (
        'capture_incomplete'
    )
