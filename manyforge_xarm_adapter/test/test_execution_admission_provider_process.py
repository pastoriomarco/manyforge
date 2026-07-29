from __future__ import annotations

import json
import os
from pathlib import Path
import signal
import subprocess
import threading
import time
import uuid

from controller_manager_msgs.msg import ControllerManagerActivity
from controller_manager_msgs.msg import NamedLifecycleState
from lifecycle_msgs.msg import State
from manyforge_robot_lifecycle_msgs.msg import ExecutionAdmissionSnapshot
from manyforge_xarm_adapter.execution_admission_provider_launch import (
    admission_provider_command,
)
from manyforge_xarm_adapter.supervised_runtime_binding import (
    EXECUTION_ADMISSION_SCHEMA,
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
from xarm_msgs.srv import ShutdownSupervisedController


class FakeAdmissionOwner:
    """ROS-only fake for the installed admission-provider process."""

    def __init__(self, prefix: str) -> None:
        self.prefix = prefix
        self.owner_id = f'{prefix}:sole-owner'
        self.context = Context()
        rclpy.init(context=self.context)
        self.node = Node(
            f'fake_admission_owner_{uuid.uuid4().hex}',
            context=self.context,
        )
        qos = QoSProfile(
            depth=1,
            durability=DurabilityPolicy.TRANSIENT_LOCAL,
            reliability=ReliabilityPolicy.RELIABLE,
        )
        self.state_publisher = self.node.create_publisher(
            SupervisedLifecycleState,
            f'/{prefix}/supervised_state',
            qos,
        )
        self.activity_publisher = self.node.create_publisher(
            ControllerManagerActivity,
            f'/{prefix}/controller_manager/activity',
            qos,
        )
        self.admission_messages: list[ExecutionAdmissionSnapshot] = []
        self._messages_lock = threading.Lock()
        self.admission_subscription = self.node.create_subscription(
            ExecutionAdmissionSnapshot,
            f'/{prefix}/execution_admission',
            self._capture_admission,
            qos,
        )
        self.execute_requests = 0
        self.shutdown_requests = 0
        self.gate_requests: list[bool] = []
        self.execute_service = self.node.create_service(
            ExecuteSupervisedLifecycleCommand,
            f'/{prefix}/supervised_execute',
            self._execute,
        )
        self.gate_service = self.node.create_service(
            SetSupervisedCommandGate,
            f'/{prefix}/supervised_gate',
            self._gate,
        )
        self.shutdown_service = self.node.create_service(
            ShutdownSupervisedController,
            f'/{prefix}/supervised_shutdown',
            self._shutdown,
        )
        self.generation = 1
        self.owner_publication_sequence = 0
        self.publish_timer = self.node.create_timer(0.025, self.publish)
        self.executor = MultiThreadedExecutor(
            num_threads=2,
            context=self.context,
        )
        self.executor.add_node(self.node)
        self.thread = threading.Thread(
            target=self.executor.spin,
            name=f'fake-admission-owner-{prefix}',
        )
        self.thread.start()
        self.publish()

    def _capture_admission(
        self,
        message: ExecutionAdmissionSnapshot,
    ) -> None:
        with self._messages_lock:
            self.admission_messages.append(message)

    def messages(self) -> list[ExecutionAdmissionSnapshot]:
        with self._messages_lock:
            return list(self.admission_messages)

    def publish(self) -> None:
        now_ns = time.monotonic_ns()
        self.owner_publication_sequence += 1
        state = SupervisedLifecycleState()
        state.owner_id = self.owner_id
        state.session_id = f'{self.prefix}:owner-session-1'
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
        state.position_ever_initialized = True
        state.stationary = True
        state.hardware_component_active = True
        state.identity_axis = 6
        state.identity_device_type = 9
        state.state = 2
        state.mode = 1
        state.brake_mask = 63
        state.servo_enable_mask = 63
        state.error_code = 0
        state.warning_code = 0
        state.report_sample_count = self.generation
        state.joint_read_attempt_count = self.generation
        state.joint_read_success_count = self.generation
        self.state_publisher.publish(state)

        activity = ControllerManagerActivity()
        controller = NamedLifecycleState()
        controller.name = f'{self.prefix}_trajectory_controller'
        controller.state.id = State.PRIMARY_STATE_ACTIVE
        controller.state.label = 'active'
        activity.controllers.append(controller)
        self.activity_publisher.publish(activity)
        self.generation += 1

    def stop_state_stream(self) -> None:
        self.publish_timer.cancel()

    def _execute(
        self,
        request: ExecuteSupervisedLifecycleCommand.Request,
        response: ExecuteSupervisedLifecycleCommand.Response,
    ) -> ExecuteSupervisedLifecycleCommand.Response:
        del request
        self.execute_requests += 1
        response.reason = 'unexpected_lifecycle_command'
        return response

    def _gate(
        self,
        request: SetSupervisedCommandGate.Request,
        response: SetSupervisedCommandGate.Response,
    ) -> SetSupervisedCommandGate.Response:
        self.gate_requests.append(request.open)
        response.accepted = not request.open
        response.reason = 'gate_closed' if response.accepted else 'refused'
        response.observed_generation = self.generation
        response.valid_until_steady_time_ns = 0
        return response

    def _shutdown(
        self,
        request: ShutdownSupervisedController.Request,
        response: ShutdownSupervisedController.Response,
    ) -> ShutdownSupervisedController.Response:
        del request
        self.shutdown_requests += 1
        response.reason = 'unexpected_controller_shutdown'
        return response

    def close(self) -> None:
        self.executor.shutdown(timeout_sec=5.0)
        self.thread.join(timeout=5.0)
        assert not self.thread.is_alive()
        self.node.destroy_node()
        self.context.shutdown()


def _runtime_binding(path: Path, prefix: str) -> Path:
    payload = {
        'schema': RUNTIME_BINDING_SCHEMA,
        'generation_id': f'{prefix}:runtime-generation-1',
        'runtime': {
            'robot_instance_id': prefix,
            'transport_owner_id': f'{prefix}:sole-owner',
            'authority_profile': 'local_operator',
            'cell_id': f'{prefix}:cell',
        },
        'controller_binding': {
            'generation_id': f'{prefix}:binding-generation-1',
            'controller_manager': f'/{prefix}/controller_manager',
            'controller_manager_activity_topic': (
                f'/{prefix}/controller_manager/activity'
            ),
            'hardware_component_name': f'{prefix}_system',
            'joint_state_broadcaster_manager_name': (
                f'{prefix}_joint_state_broadcaster'
            ),
            'trajectory_controller_manager_name': (
                f'{prefix}_trajectory_controller'
            ),
        },
        'command_surface': {
            'supervised_state_topic': f'/{prefix}/supervised_state',
            'supervised_execute_service': f'/{prefix}/supervised_execute',
            'supervised_command_gate_service': f'/{prefix}/supervised_gate',
            'supervised_shutdown_controller_service': (
                f'/{prefix}/supervised_shutdown'
            ),
        },
        'execution_admission': {
            'schema': EXECUTION_ADMISSION_SCHEMA,
            'topic': f'/{prefix}/execution_admission',
            'provider_instance_id': f'{prefix}:provider-session-1',
            'supervisor_instance_id': f'{prefix}:supervisor-session-1',
            'refresh_period_ms': 25,
            'maximum_lease_ms': 150,
        },
        'robot_lifecycle_provider': {
            'schema': 'manyforge.robot_lifecycle.ros_provider.v1',
            'robot_id': prefix,
            'provider_instance_id': f'{prefix}:provider-session-1',
            'status_topic': f'/{prefix}/robot_lifecycle/status',
            'open_session_service': f'/{prefix}/robot_lifecycle/open',
            'close_session_service': f'/{prefix}/robot_lifecycle/close',
            'submit_command_service': f'/{prefix}/robot_lifecycle/submit',
            'reconcile_command_service': (
                f'/{prefix}/robot_lifecycle/reconcile'
            ),
            'caller_assurance': 'unverified',
            'maximum_authority_lease_ms': 300_000,
        },
        'cell_supervision': {
            'schema': 'manyforge.cell_supervision.v1',
            'cell_id': f'{prefix}:cell',
            'authority_profile': 'local_operator',
            'cell_supervisor_component_epoch': (
                f'{prefix}:cell-supervisor-epoch-1'
            ),
            'components': [
                {
                    'role': 'transport_owner',
                    'id': f'{prefix}:sole-owner',
                    'maximum_heartbeat_age_ms': 500,
                    'maximum_critical_progress_age_ms': 1_000,
                }
            ],
        },
    }
    path.write_text(json.dumps(payload), encoding='utf-8')
    return path


def _wait_for(
    predicate,
    *,
    timeout_s: float = 5.0,
) -> bool:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.01)
    return predicate()


def test_installed_provider_publishes_closed_and_expires_stale_owner(
    tmp_path: Path,
) -> None:
    prefix = f'admission_{uuid.uuid4().hex[:12]}'
    owner = FakeAdmissionOwner(prefix)
    command = admission_provider_command(
        _runtime_binding(tmp_path / 'runtime-binding.json', prefix)
    )
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        start_new_session=True,
        text=True,
    )
    output = ''
    try:
        assert _wait_for(
            lambda: any(
                message.controller_ready
                and not message.motion_admitted
                for message in owner.messages()
            )
        ), 'provider never published ready-but-unadmitted state'
        ready_messages = [
            message
            for message in owner.messages()
            if message.controller_ready
        ]
        assert ready_messages
        assert all(
            message.schema == EXECUTION_ADMISSION_SCHEMA
            and message.provider_instance_id
            == f'{prefix}:provider-session-1'
            and message.robot_id == prefix
            and message.binding_contract_generation_id
            == f'{prefix}:binding-generation-1'
            and message.runtime_generation_id
            == f'{prefix}:runtime-generation-1'
            and message.provider_component_epoch
            and message.transport_owner_component_epoch
            == f'{prefix}:owner-session-1'
            and message.cell_supervisor_component_epoch
            == f'{prefix}:cell-supervisor-epoch-1'
            and not message.motion_admitted
            and 'authority_session_stale_or_missing'
            in message.reason_codes
            for message in ready_messages
        )
        assert all(
            tuple(message.reason_codes)
            == tuple(sorted(message.reason_codes))
            for message in ready_messages
        )
        assert owner.execute_requests == 0
        assert owner.shutdown_requests == 0
        assert owner.gate_requests
        assert not any(owner.gate_requests)

        last_ready_publication = max(
            message.publication_generation
            for message in ready_messages
        )
        owner.stop_state_stream()
        assert _wait_for(
            lambda: any(
                message.publication_generation > last_ready_publication
                and not message.controller_ready
                and 'observation_stale_or_missing' in message.reason_codes
                for message in owner.messages()
            )
        ), 'provider did not close readiness after source expiry'
        assert owner.execute_requests == 0
        assert owner.shutdown_requests == 0
        assert not any(owner.gate_requests)
    finally:
        try:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                output, _ = process.communicate(timeout=5.0)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                output, _ = process.communicate(timeout=5.0)
        finally:
            owner.close()
    assert process.returncode in (0, -15), output
