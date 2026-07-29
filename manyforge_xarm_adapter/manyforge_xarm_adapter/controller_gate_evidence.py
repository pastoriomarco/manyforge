"""Capture attended, no-trajectory Lite6 controller/gate evidence."""

from __future__ import annotations

import argparse
from collections.abc import Callable
from dataclasses import dataclass
from datetime import datetime
from datetime import timezone
import hashlib
import json
import math
import os
from pathlib import Path
import threading
import time
from typing import Any

from controller_manager_msgs.msg import ControllerManagerActivity
from controller_manager_msgs.srv import SwitchController
from lifecycle_msgs.msg import State
import rclpy
from rclpy.callback_groups import MutuallyExclusiveCallbackGroup
from rclpy.context import Context
from rclpy.executors import MultiThreadedExecutor
from rclpy.node import Node
from rclpy.qos import DurabilityPolicy
from rclpy.qos import QoSProfile
from rclpy.qos import ReliabilityPolicy
from sensor_msgs.msg import JointState
from xarm_msgs.msg import SupervisedLifecycleState
from xarm_msgs.srv import SetSupervisedCommandGate

from .supervised_runtime_binding import RUNTIME_BINDING_SCHEMA


EVIDENCE_SCHEMA = (
    'manyforge.xarm_adapter.controller_gate_evidence.v1'
)
CONFIRMATION = 'QUALIFY_NO_TRAJECTORY_CONTROLLER_GATE'
GATE_LEASE_NS = 100_000_000
MAX_SAMPLE_COUNT = 4096


def _steady_ns() -> int:
    return time.monotonic_ns()


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


def _binding(path: Path) -> dict[str, Any]:
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
    if not isinstance(runtime, dict) or not isinstance(controller, dict):
        raise ValueError('runtime binding is missing runtime/controller data')
    if runtime.get('authority_profile') != 'local_operator':
        raise ValueError(
            'controller/gate evidence requires local_operator authority'
        )
    required_fields = (
        'controller_manager',
        'controller_manager_activity_topic',
        'trajectory_controller_name',
        'supervised_state_topic',
        'supervised_command_gate_service',
        'joint_state_topic',
        'arm_joint_names',
    )
    missing = [
        field for field in required_fields if not controller.get(field)
    ]
    if missing:
        raise ValueError(f'controller binding is missing {missing}')
    joints = controller['arm_joint_names']
    if (
        not isinstance(joints, list)
        or not joints
        or not all(isinstance(joint, str) and joint for joint in joints)
        or len(set(joints)) != len(joints)
    ):
        raise ValueError('controller binding arm_joint_names are invalid')
    return payload


def _controller_leaf(binding: dict[str, Any]) -> str:
    return str(
        binding['controller_binding']['trajectory_controller_name']
    ).rsplit('/', 1)[-1]


@dataclass(frozen=True)
class _JointSample:
    received_at_ns: int
    positions: tuple[float, ...]


class _RosCapture:
    """Own the ROS observation and service boundary for one capture."""

    def __init__(self, binding: dict[str, Any]) -> None:
        runtime = binding['runtime']
        controller = binding['controller_binding']
        self._owner_id = str(runtime['transport_owner_id'])
        self._controller_name = _controller_leaf(binding)
        self._joint_names = tuple(controller['arm_joint_names'])
        self._lock = threading.RLock()
        self._condition = threading.Condition(self._lock)
        self._state: SupervisedLifecycleState | None = None
        self._controller_active: bool | None = None
        self._joint_samples: list[_JointSample] = []
        self._events: list[dict[str, object]] = []
        self._sequence = 0

        self._context = Context()
        rclpy.init(context=self._context)
        self._node = Node(
            'manyforge_lite6_controller_gate_evidence',
            context=self._context,
        )
        service_group = MutuallyExclusiveCallbackGroup()
        state_qos = QoSProfile(
            depth=1,
            durability=DurabilityPolicy.TRANSIENT_LOCAL,
            reliability=ReliabilityPolicy.RELIABLE,
        )
        self._state_subscription = self._node.create_subscription(
            SupervisedLifecycleState,
            str(controller['supervised_state_topic']),
            self._on_state,
            state_qos,
        )
        self._activity_subscription = self._node.create_subscription(
            ControllerManagerActivity,
            str(controller['controller_manager_activity_topic']),
            self._on_activity,
            state_qos,
        )
        self._joint_subscription = self._node.create_subscription(
            JointState,
            str(controller['joint_state_topic']),
            self._on_joint_state,
            10,
        )
        manager = str(controller['controller_manager']).rstrip('/')
        self._switch_client = self._node.create_client(
            SwitchController,
            f'{manager}/switch_controller',
            callback_group=service_group,
        )
        self._gate_client = self._node.create_client(
            SetSupervisedCommandGate,
            str(controller['supervised_command_gate_service']),
            callback_group=service_group,
        )
        self._executor = MultiThreadedExecutor(
            num_threads=3,
            context=self._context,
        )
        self._executor.add_node(self._node)
        self._thread = threading.Thread(
            target=self._executor.spin,
            name='lite6-controller-gate-evidence-ros',
        )
        self._thread.start()

    def close(self) -> None:
        self._executor.shutdown(timeout_sec=5.0)
        self._thread.join(timeout=5.0)
        if self._thread.is_alive():
            raise RuntimeError('ROS evidence executor did not stop')
        self._node.destroy_node()
        self._context.shutdown()

    def _append_event(self, kind: str, **fields: object) -> None:
        self._sequence += 1
        self._events.append(
            {
                'sequence': self._sequence,
                'occurred_at_steady_ns': _steady_ns(),
                'kind': kind,
                **fields,
            }
        )

    def _on_state(self, message: SupervisedLifecycleState) -> None:
        with self._condition:
            if message.owner_id != self._owner_id:
                self._append_event(
                    'foreign_owner_state_ignored',
                    owner_id=message.owner_id,
                )
                return
            previous = self._state
            self._state = message
            if (
                previous is None
                or previous.generation != message.generation
                or previous.command_gate_open != message.command_gate_open
                or previous.joint_write_attempt_count
                != message.joint_write_attempt_count
            ):
                self._append_event(
                    'supervised_state',
                    generation=message.generation,
                    command_gate_open=message.command_gate_open,
                    command_gate_valid_until_steady_time_ns=(
                        message.command_gate_valid_until_steady_time_ns
                    ),
                    hardware_component_active=(
                        message.hardware_component_active
                    ),
                    joint_write_attempt_count=(
                        message.joint_write_attempt_count
                    ),
                    lifecycle_command_attempt_count=(
                        message.lifecycle_command_attempt_count
                    ),
                    last_joint_write_return_code=(
                        message.last_joint_write_return_code
                    ),
                    position_valid=message.position_valid,
                )
            self._condition.notify_all()

    def _on_activity(self, message: ControllerManagerActivity) -> None:
        active: bool | None = None
        for controller in message.controllers:
            if controller.name == self._controller_name:
                active = (
                    controller.state.id
                    == State.PRIMARY_STATE_ACTIVE
                )
                break
        with self._condition:
            if active != self._controller_active:
                self._controller_active = active
                self._append_event(
                    'controller_activity',
                    controller_name=self._controller_name,
                    active=active,
                )
            self._condition.notify_all()

    def _on_joint_state(self, message: JointState) -> None:
        if len(message.name) != len(message.position):
            return
        positions_by_name = dict(zip(message.name, message.position))
        if not all(joint in positions_by_name for joint in self._joint_names):
            return
        positions = tuple(
            float(positions_by_name[joint]) for joint in self._joint_names
        )
        if not all(math.isfinite(value) for value in positions):
            return
        with self._condition:
            if len(self._joint_samples) < MAX_SAMPLE_COUNT:
                self._joint_samples.append(
                    _JointSample(_steady_ns(), positions)
                )
            self._condition.notify_all()

    def wait_for(
        self,
        predicate: Callable[[], bool],
        deadline_ns: int,
        reason: str,
    ) -> None:
        with self._condition:
            while not predicate():
                remaining_ns = deadline_ns - _steady_ns()
                if remaining_ns <= 0:
                    raise TimeoutError(reason)
                self._condition.wait(remaining_ns / 1_000_000_000)

    def wait_for_services(self, deadline_ns: int) -> None:
        for client, label in (
            (self._switch_client, 'switch_controller'),
            (self._gate_client, 'set_command_gate'),
        ):
            remaining_s = (deadline_ns - _steady_ns()) / 1_000_000_000
            if remaining_s <= 0 or not client.wait_for_service(
                timeout_sec=remaining_s
            ):
                raise TimeoutError(f'{label}_service_unavailable')

    def initial_ready(self, deadline_ns: int) -> None:
        def predicate() -> bool:
            return (
                self._state is not None
                and self._state.generation > 0
                and self._state.transport_connected
                and self._state.report_connected
                and self._state.identity_verified
                and self._state.position_valid
                and self._state.hardware_component_active
                and not self._state.process_restart_required
                and self._controller_active is True
                and bool(self._joint_samples)
            )

        self.wait_for(
            predicate,
            deadline_ns,
            'initial_controller_gate_observation_unavailable',
        )

    def _await_future(
        self,
        future: Any,
        deadline_ns: int,
        reason: str,
    ) -> Any:
        done = threading.Event()
        future.add_done_callback(lambda _: done.set())
        remaining_s = (deadline_ns - _steady_ns()) / 1_000_000_000
        if remaining_s <= 0 or not done.wait(remaining_s):
            raise TimeoutError(reason)
        exception = future.exception()
        if exception is not None:
            raise RuntimeError(f'{reason}: {exception}')
        return future.result()

    def switch_controller(self, active: bool, deadline_ns: int) -> None:
        request = SwitchController.Request()
        if active:
            request.activate_controllers = [self._controller_name]
        else:
            request.deactivate_controllers = [self._controller_name]
        request.strictness = SwitchController.Request.STRICT
        request.activate_asap = True
        request.timeout.sec = 2
        response = self._await_future(
            self._switch_client.call_async(request),
            deadline_ns,
            'switch_controller_timeout',
        )
        self._append_event(
            'switch_controller_response',
            requested_active=active,
            accepted=bool(response.ok),
        )
        if not response.ok:
            raise RuntimeError('switch_controller_rejected')
        self.wait_for(
            lambda: self._controller_active is active,
            deadline_ns,
            'controller_activity_transition_timeout',
        )

    def open_gate(
        self,
        request_id: str,
        expected_generation: int,
        deadline_ns: int,
    ) -> SetSupervisedCommandGate.Response:
        request = SetSupervisedCommandGate.Request()
        request.request_id = request_id
        request.expected_generation = expected_generation
        request.open = True
        request.lease_duration_ns = GATE_LEASE_NS
        response = self._await_future(
            self._gate_client.call_async(request),
            deadline_ns,
            'set_command_gate_timeout',
        )
        self._append_event(
            'command_gate_response',
            request_id=request_id,
            accepted=bool(response.accepted),
            reason=response.reason,
            observed_generation=response.observed_generation,
            valid_until_steady_time_ns=(
                response.valid_until_steady_time_ns
            ),
        )
        if not response.accepted:
            raise RuntimeError(
                f'command_gate_rejected:{response.reason}'
            )
        return response

    def close_gate(self, request_id: str, deadline_ns: int) -> None:
        request = SetSupervisedCommandGate.Request()
        request.request_id = request_id
        request.open = False
        response = self._await_future(
            self._gate_client.call_async(request),
            deadline_ns,
            'close_command_gate_timeout',
        )
        self._append_event(
            'command_gate_close_response',
            accepted=bool(response.accepted),
            reason=response.reason,
        )
        if not response.accepted:
            raise RuntimeError(
                f'command_gate_close_rejected:{response.reason}'
            )

    def state_copy(self) -> SupervisedLifecycleState:
        with self._lock:
            if self._state is None:
                raise RuntimeError('supervised_state_unavailable')
            return self._state

    def controller_active(self) -> bool | None:
        with self._lock:
            return self._controller_active

    def joint_samples_from(self, start_index: int) -> list[_JointSample]:
        with self._lock:
            return list(self._joint_samples[start_index:])

    def joint_sample_count(self) -> int:
        with self._lock:
            return len(self._joint_samples)

    def events(self) -> list[dict[str, object]]:
        with self._lock:
            return list(self._events)


def _max_pose_deviation(samples: list[_JointSample]) -> float:
    if not samples:
        return math.inf
    anchor = samples[0].positions
    return max(
        abs(value - anchor[index])
        for sample in samples
        for index, value in enumerate(sample.positions)
    )


def capture_controller_gate_evidence(
    *,
    runtime_binding_path: Path,
    output_root: Path,
    request_id: str,
    confirmation: str,
    max_pose_deviation_rad: float,
    timeout_s: float = 15.0,
    captured_at: datetime | None = None,
    capture_factory: Callable[[dict[str, Any]], _RosCapture] = _RosCapture,
) -> dict[str, Any]:
    """Run one attended no-trajectory controller/gate qualification."""
    if confirmation != CONFIRMATION:
        raise ValueError(f'confirmation must exactly equal {CONFIRMATION}')
    if not request_id or len(request_id) > 128:
        raise ValueError('request_id must be a bounded identifier')
    if not math.isfinite(max_pose_deviation_rad) or (
        max_pose_deviation_rad <= 0.0
    ):
        raise ValueError('max_pose_deviation_rad must be finite and positive')
    if not math.isfinite(timeout_s) or timeout_s < 5.0 or timeout_s > 60.0:
        raise ValueError('timeout_s must be between 5 and 60 seconds')

    binding = _binding(runtime_binding_path)
    instant = captured_at or datetime.now(timezone.utc)
    if instant.tzinfo is None:
        raise ValueError('captured_at must be timezone-aware')
    output_root.mkdir(parents=True, exist_ok=False)
    deadline_ns = _steady_ns() + int(timeout_s * 1_000_000_000)
    capture = capture_factory(binding)
    failure_reason = ''
    passed = False
    initial_state: SupervisedLifecycleState | None = None
    final_state: SupervisedLifecycleState | None = None
    gate_response: SetSupervisedCommandGate.Response | None = None
    sample_start = 0
    gate_open_observed = False
    gate_expiry_observed = False
    controller_rearmed = False
    try:
        capture.wait_for_services(deadline_ns)
        capture.initial_ready(deadline_ns)
        initial_state = capture.state_copy()
        sample_start = capture.joint_sample_count() - 1

        capture.switch_controller(False, deadline_ns)
        inactive_state = capture.state_copy()
        if inactive_state.command_gate_open:
            raise RuntimeError('gate_open_while_controller_inactive')
        capture.switch_controller(True, deadline_ns)
        controller_rearmed = capture.controller_active() is True
        rearmed_state = capture.state_copy()
        if rearmed_state.command_gate_open:
            raise RuntimeError('controller_rearm_implicitly_opened_gate')
        if (
            rearmed_state.joint_write_attempt_count
            != initial_state.joint_write_attempt_count
        ):
            raise RuntimeError('controller_rearm_implicitly_wrote_command')

        gate_response = capture.open_gate(
            request_id,
            rearmed_state.generation,
            deadline_ns,
        )
        capture.wait_for(
            lambda: capture.state_copy().command_gate_open,
            deadline_ns,
            'command_gate_open_observation_timeout',
        )
        gate_open_observed = True
        capture.wait_for(
            lambda: (
                not capture.state_copy().command_gate_open
                and capture.state_copy().joint_write_attempt_count
                > rearmed_state.joint_write_attempt_count
            ),
            deadline_ns,
            'command_gate_expiry_or_write_progress_timeout',
        )
        gate_expiry_observed = True
        final_state = capture.state_copy()
        samples = capture.joint_samples_from(sample_start)
        max_deviation = _max_pose_deviation(samples)
        passed = (
            controller_rearmed
            and gate_open_observed
            and gate_expiry_observed
            and final_state.joint_write_attempt_count
            > initial_state.joint_write_attempt_count
            and final_state.lifecycle_command_attempt_count
            == initial_state.lifecycle_command_attempt_count
            and final_state.last_joint_write_return_code == 0
            and max_deviation <= max_pose_deviation_rad
            and len(samples) >= 2
        )
        if not passed:
            failure_reason = 'controller_gate_postcondition_failed'
    except (RuntimeError, TimeoutError) as error:
        failure_reason = str(error)
        samples = capture.joint_samples_from(sample_start)
        max_deviation = _max_pose_deviation(samples)
        try:
            final_state = capture.state_copy()
        except RuntimeError:
            final_state = None
    finally:
        try:
            capture.close_gate(
                f'{request_id}:final-close',
                _steady_ns() + 2_000_000_000,
            )
        except (RuntimeError, TimeoutError) as error:
            if passed:
                passed = False
                failure_reason = f'final_gate_close_failed:{error}'
        capture.close()

    def state_record(
        state: SupervisedLifecycleState | None,
    ) -> dict[str, object] | None:
        if state is None:
            return None
        return {
            'generation': state.generation,
            'session_id': state.session_id,
            'command_gate_open': state.command_gate_open,
            'command_gate_valid_until_steady_time_ns': (
                state.command_gate_valid_until_steady_time_ns
            ),
            'hardware_component_active': state.hardware_component_active,
            'position_valid': state.position_valid,
            'joint_write_attempt_count': state.joint_write_attempt_count,
            'lifecycle_command_attempt_count': (
                state.lifecycle_command_attempt_count
            ),
            'last_joint_write_return_code': (
                state.last_joint_write_return_code
            ),
        }

    runtime_bytes = runtime_binding_path.read_bytes()
    summary = {
        'schema': EVIDENCE_SCHEMA,
        'captured_at_utc': instant.astimezone(timezone.utc).isoformat(),
        'result': 'pass' if passed else 'fail',
        'failure_reason': failure_reason,
        'scope': (
            'named_jtc_deactivate_reactivate_and_current_pose_gate;'
            'no_trajectory_published'
        ),
        'request_id': request_id,
        'confirmation': confirmation,
        'runtime_binding_sha256': _sha256_bytes(runtime_bytes),
        'runtime_binding_generation_id': binding['generation_id'],
        'source_binding_manifest_sha256': binding['source_binding'][
            'manifest_sha256'
        ],
        'controller_name': _controller_leaf(binding),
        'gate_lease_ns': GATE_LEASE_NS,
        'controller_rearmed': controller_rearmed,
        'gate_open_observed': gate_open_observed,
        'gate_expiry_observed': gate_expiry_observed,
        'initial_state': state_record(initial_state),
        'final_state': state_record(final_state),
        'gate_response': (
            {
                'accepted': gate_response.accepted,
                'reason': gate_response.reason,
                'observed_generation': gate_response.observed_generation,
                'valid_until_steady_time_ns': (
                    gate_response.valid_until_steady_time_ns
                ),
            }
            if gate_response is not None
            else None
        ),
        'joint_sample_count': len(samples),
        'max_pose_deviation_rad': max_deviation,
        'max_pose_deviation_limit_rad': max_pose_deviation_rad,
        'events': capture.events(),
    }
    _atomic_json(output_root / 'evidence.json', summary)
    with (output_root / 'events.jsonl').open(
        'x', encoding='utf-8'
    ) as handle:
        for event in summary['events']:
            handle.write(json.dumps(event, sort_keys=True) + '\n')
        handle.flush()
        os.fsync(handle.fileno())
    return summary


def main() -> int:
    """CLI entry point."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime-binding', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--request-id', required=True)
    parser.add_argument('--confirm-operation', required=True)
    parser.add_argument(
        '--max-pose-deviation-rad',
        type=float,
        required=True,
    )
    parser.add_argument('--timeout-s', type=float, default=15.0)
    arguments = parser.parse_args()
    summary = capture_controller_gate_evidence(
        runtime_binding_path=arguments.runtime_binding.resolve(),
        output_root=arguments.output.resolve(),
        request_id=arguments.request_id,
        confirmation=arguments.confirm_operation,
        max_pose_deviation_rad=arguments.max_pose_deviation_rad,
        timeout_s=arguments.timeout_s,
    )
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0 if summary['result'] == 'pass' else 1


if __name__ == '__main__':
    raise SystemExit(main())
