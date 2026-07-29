from __future__ import annotations

from datetime import datetime
from datetime import timezone
import json
from pathlib import Path
import threading
import time

from controller_manager_msgs.msg import ControllerManagerActivity
from controller_manager_msgs.msg import NamedLifecycleState
from lifecycle_msgs.msg import State
from manyforge_xarm_adapter.supervised_command_evidence import (
    capture_supervised_command_evidence,
)
from manyforge_xarm_adapter.supervised_runtime_binding import (
    RUNTIME_BINDING_SCHEMA,
)
import rclpy
from rclpy.context import Context
from rclpy.executors import MultiThreadedExecutor
from rclpy.node import Node
from rclpy.qos import DurabilityPolicy
from rclpy.qos import QoSProfile
from rclpy.qos import ReliabilityPolicy
from xarm_msgs.msg import SupervisedLifecycleState
from xarm_msgs.srv import ExecuteSupervisedLifecycleCommand
from xarm_msgs.srv import SetSupervisedCommandGate


class FakeSupervisedOwner:
    """Restricted ROS owner used to qualify the installed evidence CLI."""

    def __init__(self) -> None:
        self.context = Context()
        rclpy.init(context=self.context)
        self.node = Node(
            'fake_supervised_command_evidence_owner',
            context=self.context,
        )
        qos = QoSProfile(
            depth=1,
            durability=DurabilityPolicy.TRANSIENT_LOCAL,
            reliability=ReliabilityPolicy.RELIABLE,
        )
        self.state_publisher = self.node.create_publisher(
            SupervisedLifecycleState,
            '/evidence/ufactory/supervised_lifecycle/state',
            qos,
        )
        self.activity_publisher = self.node.create_publisher(
            ControllerManagerActivity,
            '/evidence/controller_manager/activity',
            qos,
        )
        self.execute_service = self.node.create_service(
            ExecuteSupervisedLifecycleCommand,
            '/evidence/ufactory/supervised_lifecycle/execute',
            self._execute,
        )
        self.gate_service = self.node.create_service(
            SetSupervisedCommandGate,
            '/evidence/ufactory/supervised_lifecycle/set_command_gate',
            self._set_gate,
        )
        self.generation = 1
        self.owner_publication_sequence = 0
        self.robot_state = 2
        self.enabled = True
        self.command_requests: list[tuple[int, str, int]] = []
        self.replay: dict[
            str,
            tuple[int, int, bool, bool, int, str, int],
        ] = {}
        self.gate_requests: list[tuple[bool, int]] = []
        self.timer = self.node.create_timer(0.05, self.publish)
        self.executor = MultiThreadedExecutor(
            num_threads=2,
            context=self.context,
        )
        self.executor.add_node(self.node)
        self.thread = threading.Thread(target=self.executor.spin)
        self.thread.start()
        self.publish()

    def publish(self) -> None:
        now_ns = time.monotonic_ns()
        self.owner_publication_sequence += 1
        state = SupervisedLifecycleState()
        state.owner_id = 'lite6-evidence:sole-owner'
        state.session_id = 'fake-owner-session-1'
        state.owner_publication_sequence = (
            self.owner_publication_sequence
        )
        state.owner_published_steady_time_ns = now_ns
        state.generation = self.generation
        state.source_steady_time_ns = now_ns
        state.fresh_until_steady_time_ns = now_ns + 1_000_000_000
        state.transport_connected = True
        state.report_connected = True
        state.identity_verified = True
        state.report_received = True
        state.position_valid = True
        state.hardware_component_active = True
        state.identity_axis = 6
        state.identity_device_type = 9
        state.state = self.robot_state
        state.mode = 1
        state.brake_mask = 63 if self.enabled else 0
        state.servo_enable_mask = 63 if self.enabled else 0
        state.error_code = 0
        state.warning_code = 0
        state.report_sample_count = self.generation
        state.joint_read_attempt_count = self.generation
        state.joint_read_success_count = self.generation
        self.state_publisher.publish(state)

        activity = ControllerManagerActivity()
        controller = NamedLifecycleState()
        controller.name = 'lite6_evidence_controller'
        controller.state.id = State.PRIMARY_STATE_INACTIVE
        controller.state.label = 'inactive'
        activity.controllers.append(controller)
        self.activity_publisher.publish(activity)

    def _execute(
        self,
        request: ExecuteSupervisedLifecycleCommand.Request,
        response: ExecuteSupervisedLifecycleCommand.Response,
    ) -> ExecuteSupervisedLifecycleCommand.Response:
        self.command_requests.append(
            (request.command, request.request_id, request.expected_generation)
        )
        replay = self.replay.get(request.request_id)
        if replay is not None:
            (
                replay_command,
                replay_generation,
                response.permitted,
                response.attempted,
                response.return_code,
                response.reason,
                response.observed_generation,
            ) = replay
            if (
                replay_command != request.command
                or replay_generation != request.expected_generation
            ):
                response.permitted = False
                response.attempted = False
                response.return_code = 0
                response.reason = 'request_id_conflict'
                response.observed_generation = self.generation
            return response
        permitted = (
            request.expected_generation == self.generation
            and request.command
            in (
                ExecuteSupervisedLifecycleCommand.Request.SET_STOPPED_STATE,
                ExecuteSupervisedLifecycleCommand.Request.DISABLE_MOTION,
            )
            and (
                request.command
                != ExecuteSupervisedLifecycleCommand.Request.DISABLE_MOTION
                or self.robot_state == 4
            )
        )
        response.permitted = permitted
        response.attempted = permitted
        response.return_code = 0 if permitted else -1
        response.reason = (
            'command_succeeded'
            if permitted
            else 'fake_owner_request_rejected'
        )
        response.observed_generation = self.generation
        if permitted:
            self.generation += 1
            if (
                request.command
                == ExecuteSupervisedLifecycleCommand.Request.SET_STOPPED_STATE
            ):
                self.robot_state = 4
            else:
                self.enabled = False
            self.publish()
        self.replay[request.request_id] = (
            request.command,
            request.expected_generation,
            response.permitted,
            response.attempted,
            response.return_code,
            response.reason,
            response.observed_generation,
        )
        return response

    def _set_gate(
        self,
        request: SetSupervisedCommandGate.Request,
        response: SetSupervisedCommandGate.Response,
    ) -> SetSupervisedCommandGate.Response:
        self.gate_requests.append(
            (request.open, request.expected_generation)
        )
        response.accepted = request.expected_generation == self.generation
        response.reason = (
            'gate_updated'
            if response.accepted
            else 'observation_generation_mismatch'
        )
        response.observed_generation = self.generation
        response.valid_until_steady_time_ns = (
            time.monotonic_ns() + request.lease_duration_ns
            if request.open and response.accepted
            else 0
        )
        return response

    def close(self) -> None:
        self.executor.shutdown(timeout_sec=5.0)
        self.thread.join(timeout=5.0)
        assert not self.thread.is_alive()
        self.node.destroy_node()
        self.context.shutdown()


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
            'controller_manager': '/evidence/controller_manager',
            'hardware_component_name': 'lite6_evidence_system',
            'joint_state_broadcaster_manager_name': (
                'lite6_evidence_joint_state_broadcaster'
            ),
            'supervised_state_topic': (
                '/evidence/ufactory/supervised_lifecycle/state'
            ),
            'supervised_execute_service': (
                '/evidence/ufactory/supervised_lifecycle/execute'
            ),
            'supervised_command_gate_service': (
                '/evidence/ufactory/supervised_lifecycle/set_command_gate'
            ),
            'supervised_shutdown_controller_service': (
                '/evidence/ufactory/supervised_lifecycle/'
                'shutdown_controller'
            ),
            'controller_manager_activity_topic': (
                '/evidence/controller_manager/activity'
            ),
            'trajectory_controller_name': (
                '/lite6_evidence_controller'
            ),
        },
        'source_binding': {
            'manifest_sha256': 'b' * 64,
        },
        'robot_lifecycle_provider': {
            'schema': 'manyforge.robot_lifecycle.ros_provider.v1',
            'robot_id': 'lite6-evidence',
            'provider_instance_id': 'lite6-evidence:provider:test',
            'caller_assurance': 'unverified',
        },
    }
    path.write_text(json.dumps(payload), encoding='utf-8')
    return path


def test_successive_installed_clients_keep_transport_ids_unique(
    tmp_path: Path,
) -> None:
    owner = FakeSupervisedOwner()
    try:
        first_output = tmp_path / 'first-evidence'
        first = capture_supervised_command_evidence(
            runtime_binding_path=_runtime_binding(
                tmp_path / 'runtime-binding.json'
            ),
            output_root=first_output,
            action_id='robot.stop_control',
            request_id='process-request-1',
            authority_session_id='process-session-1',
            command_source='test.local_operator',
            caller_context_id='process-context-1',
            timeout_ms=8000,
            captured_at=datetime(2026, 7, 25, tzinfo=timezone.utc),
        )

        second_output = tmp_path / 'second-evidence'
        second = capture_supervised_command_evidence(
            runtime_binding_path=tmp_path / 'runtime-binding.json',
            output_root=second_output,
            action_id='robot.disable',
            request_id='process-request-2',
            authority_session_id='process-session-2',
            command_source='test.local_operator',
            caller_context_id='process-context-1',
            timeout_ms=8000,
            captured_at=datetime(2026, 7, 25, tzinfo=timezone.utc),
        )

        assert first['result'] == 'pass'
        first_result = first['event']['command_result']
        assert first_result['result'] == 'pass'
        assert first_result['command_source'] == 'test.local_operator'
        assert first_result['caller_context_id'] == 'process-context-1'
        assert first_result['caller_assurance'] == 'unverified'
        assert first_result['before']['vendor_control_ready'] == 'true'
        assert first_result['after']['vendor_control_ready'] == 'false'
        assert first_result['after']['robot_enabled'] == 'true'
        assert len(first_result['primitive_audit']) == 1
        first_primitive = first_result['primitive_audit'][0]
        assert first_primitive['command'] == 'set_stopped_state'
        assert first_primitive['result_state'] == 'response_received'
        assert first_primitive['permitted'] is True
        assert first_primitive['attempted'] is True
        assert first_primitive['return_code'] == 0
        assert first_primitive['transport_request_id'] == (
            'mf:process-request-1:step:0'
        )
        assert len(first_result['gateway_audit']) == 3
        assert [
            entry['operation']
            for entry in first_result['gateway_audit']
        ] == ['open_session', 'submit', 'close_session']

        assert second['result'] == 'pass'
        second_result = second['event']['command_result']
        assert second_result['after']['robot_enabled'] == 'false'
        second_primitive = second_result['primitive_audit'][0]
        assert second_primitive['command'] == 'disable_motion'
        assert second_primitive['transport_request_id'] == (
            'mf:process-request-2:step:0'
        )
        assert len(owner.command_requests) == 2
        assert owner.command_requests[0][0] == (
            ExecuteSupervisedLifecycleCommand.Request.SET_STOPPED_STATE
        )
        assert owner.command_requests[1][0] == (
            ExecuteSupervisedLifecycleCommand.Request.DISABLE_MOTION
        )
        assert owner.command_requests[0][1] != owner.command_requests[1][1]
        assert (first_output / 'evidence.json').is_file()
        assert (second_output / 'evidence.json').is_file()
        assert (second_output / 'events.jsonl').is_file()
        assert (
            second_output / 'raw/01-lifecycle-command.txt'
        ).is_file()
    finally:
        owner.close()
