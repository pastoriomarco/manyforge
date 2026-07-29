"""Tests for deterministic no-trajectory controller/gate evidence."""

from __future__ import annotations

import copy
from datetime import datetime
from datetime import timezone
import json
from pathlib import Path
from types import SimpleNamespace
from typing import Any

from manyforge_xarm_adapter.controller_gate_evidence import (
    capture_controller_gate_evidence,
)
from manyforge_xarm_adapter.controller_gate_evidence import (
    CONFIRMATION,
)
from manyforge_xarm_adapter.supervised_runtime_binding import (
    RUNTIME_BINDING_SCHEMA,
)
from sensor_msgs.msg import JointState
from xarm_msgs.msg import SupervisedLifecycleState
from xarm_msgs.srv import SetSupervisedCommandGate


def _runtime_binding(path: Path) -> Path:
    payload = {
        'schema': RUNTIME_BINDING_SCHEMA,
        'generation_id': 'a' * 64,
        'runtime': {
            'robot_instance_id': 'lite6-evidence',
            'transport_owner_id': 'lite6-evidence:sole-owner',
            'authority_profile': 'local_operator',
        },
        'controller_binding': {
            'controller_manager': '/controller_manager',
            'controller_manager_activity_topic': (
                '/controller_manager/activity'
            ),
            'trajectory_controller_name': '/lite6_traj_controller',
            'supervised_state_topic': (
                '/ufactory/supervised_lifecycle/state'
            ),
            'supervised_command_gate_service': (
                '/ufactory/supervised_lifecycle/set_command_gate'
            ),
            'joint_state_topic': '/ufactory/joint_states',
            'arm_joint_names': [f'joint{index}' for index in range(1, 7)],
        },
        'source_binding': {
            'manifest_sha256': 'b' * 64,
        },
    }
    path.write_text(json.dumps(payload), encoding='utf-8')
    return path


def _state() -> SupervisedLifecycleState:
    value = SupervisedLifecycleState()
    value.owner_id = 'lite6-evidence:sole-owner'
    value.session_id = 'owner-session-1'
    value.owner_publication_sequence = 1
    value.owner_published_steady_time_ns = 1
    value.generation = 5
    value.transport_connected = True
    value.report_connected = True
    value.identity_verified = True
    value.position_valid = True
    value.hardware_component_active = True
    value.joint_write_attempt_count = 10
    value.lifecycle_command_attempt_count = 4
    value.last_joint_write_return_code = 0
    return value


class _FakeCapture:
    """Deterministic seam matching the ROS capture's observable behavior."""

    def __init__(
        self,
        _: dict[str, Any],
        *,
        deviation_rad: float = 0.0002,
    ) -> None:
        self.state = _state()
        self.active = True
        self.deviation_rad = deviation_rad
        self.samples = [
            _joint_sample(0.0),
        ]
        self.recorded_events: list[dict[str, object]] = []
        self.closed = False
        self.wait_count = 0

    def close(self) -> None:
        self.closed = True

    def wait_for_services(self, _: int) -> None:
        return

    def initial_ready(self, _: int) -> None:
        return

    def switch_controller(self, active: bool, _: int) -> None:
        self.active = active
        self.recorded_events.append(
            {'kind': 'switch_controller', 'active': active}
        )

    def open_gate(
        self,
        request_id: str,
        expected_generation: int,
        _: int,
    ) -> SetSupervisedCommandGate.Response:
        assert expected_generation == self.state.generation
        self.state.command_gate_open = True
        response = SetSupervisedCommandGate.Response()
        response.accepted = True
        response.reason = 'command_gate_open'
        response.observed_generation = expected_generation
        response.valid_until_steady_time_ns = 100_000_000
        self.recorded_events.append(
            {'kind': 'gate_open', 'request_id': request_id}
        )
        return response

    def wait_for(self, predicate: Any, _: int, reason: str) -> None:
        self.wait_count += 1
        if self.wait_count == 2:
            self.state.command_gate_open = False
            self.state.joint_write_attempt_count += 15
            self.samples.append(_joint_sample(self.deviation_rad))
        if not predicate():
            raise TimeoutError(reason)

    def close_gate(self, request_id: str, _: int) -> None:
        self.state.command_gate_open = False
        self.recorded_events.append(
            {'kind': 'gate_close', 'request_id': request_id}
        )

    def state_copy(self) -> SupervisedLifecycleState:
        return copy.deepcopy(self.state)

    def controller_active(self) -> bool:
        return self.active

    def joint_samples_from(self, start_index: int) -> list[Any]:
        return self.samples[start_index:]

    def joint_sample_count(self) -> int:
        return len(self.samples)

    def events(self) -> list[dict[str, object]]:
        return self.recorded_events


def _joint_sample(position: float) -> Any:
    # Use the module's structural sample contract without depending on its
    # private type.
    return SimpleNamespace(
        received_at_ns=1,
        positions=(position,) * 6,
    )


def test_controller_gate_capture_passes_exact_no_trajectory_sequence(
    tmp_path: Path,
) -> None:
    holder: list[_FakeCapture] = []

    def factory(binding: dict[str, Any]) -> _FakeCapture:
        capture = _FakeCapture(binding)
        holder.append(capture)
        return capture

    output = tmp_path / 'evidence'
    result = capture_controller_gate_evidence(
        runtime_binding_path=_runtime_binding(
            tmp_path / 'runtime-binding.json'
        ),
        output_root=output,
        request_id='controller-gate-1',
        confirmation=CONFIRMATION,
        max_pose_deviation_rad=0.001,
        captured_at=datetime(2026, 7, 25, tzinfo=timezone.utc),
        capture_factory=factory,
    )

    assert result['result'] == 'pass'
    assert result['controller_rearmed'] is True
    assert result['gate_open_observed'] is True
    assert result['gate_expiry_observed'] is True
    assert result['max_pose_deviation_rad'] == 0.0002
    assert result['initial_state']['joint_write_attempt_count'] == 10
    assert result['final_state']['joint_write_attempt_count'] == 25
    assert result['initial_state']['lifecycle_command_attempt_count'] == 4
    assert result['final_state']['lifecycle_command_attempt_count'] == 4
    assert holder[0].closed is True
    assert [event['active'] for event in result['events'][:2]] == [
        False,
        True,
    ]
    assert (output / 'evidence.json').is_file()
    assert (output / 'events.jsonl').is_file()


def test_controller_gate_capture_fails_pose_limit_but_still_closes(
    tmp_path: Path,
) -> None:
    holder: list[_FakeCapture] = []

    def factory(binding: dict[str, Any]) -> _FakeCapture:
        capture = _FakeCapture(binding, deviation_rad=0.02)
        holder.append(capture)
        return capture

    result = capture_controller_gate_evidence(
        runtime_binding_path=_runtime_binding(
            tmp_path / 'runtime-binding.json'
        ),
        output_root=tmp_path / 'evidence',
        request_id='controller-gate-2',
        confirmation=CONFIRMATION,
        max_pose_deviation_rad=0.001,
        captured_at=datetime(2026, 7, 25, tzinfo=timezone.utc),
        capture_factory=factory,
    )

    assert result['result'] == 'fail'
    assert result['failure_reason'] == (
        'controller_gate_postcondition_failed'
    )
    assert result['max_pose_deviation_rad'] == 0.02
    assert holder[0].closed is True


def test_controller_gate_capture_requires_exact_confirmation(
    tmp_path: Path,
) -> None:
    try:
        capture_controller_gate_evidence(
            runtime_binding_path=_runtime_binding(
                tmp_path / 'runtime-binding.json'
            ),
            output_root=tmp_path / 'evidence',
            request_id='controller-gate-3',
            confirmation='yes',
            max_pose_deviation_rad=0.001,
        )
    except ValueError as error:
        assert CONFIRMATION in str(error)
    else:
        raise AssertionError('incorrect confirmation was accepted')


def test_joint_state_message_dependency_is_installed() -> None:
    # Keep the runtime dependency exercised in the package test collection.
    assert isinstance(JointState(), JointState)
