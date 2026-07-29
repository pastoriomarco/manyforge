"""Strict ROS ingress for robot-neutral execution-admission snapshots."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import re
import threading
import time
from typing import Any


EXECUTION_ADMISSION_SCHEMA = (
    'manyforge.robot_lifecycle.execution_admission.v0.experimental'
)
_MAXIMUM_INT64 = (1 << 63) - 1


@dataclass(frozen=True)
class ExecutionAdmissionIngressConfig:
    topic: str
    provider_instance_id: str
    robot_id: str
    binding_contract_generation_id: str
    runtime_generation_id: str
    cell_supervisor_component_epoch: str
    maximum_lease_ns: int = 250_000_000

    def __post_init__(self) -> None:
        for label, value in (
            ('topic', self.topic),
            ('provider_instance_id', self.provider_instance_id),
            ('robot_id', self.robot_id),
            (
                'binding_contract_generation_id',
                self.binding_contract_generation_id,
            ),
            ('runtime_generation_id', self.runtime_generation_id),
            (
                'cell_supervisor_component_epoch',
                self.cell_supervisor_component_epoch,
            ),
        ):
            if (
                not isinstance(value, str)
                or not value
                or len(value) > 256
                or any(
                    ord(character) < 0x21 or ord(character) > 0x7e
                    for character in value
                )
            ):
                raise ValueError(
                    f'{label} must be a bounded printable identifier'
                )
        if (
            isinstance(self.maximum_lease_ns, bool)
            or not isinstance(self.maximum_lease_ns, int)
            or self.maximum_lease_ns <= 0
        ):
            raise ValueError('maximum_lease_ns must be a positive integer')


def _ingress_node_name(config: ExecutionAdmissionIngressConfig) -> str:
    portable_robot = re.sub(r'[^A-Za-z0-9_]', '_', config.robot_id)
    portable_robot = portable_robot.strip('_') or 'robot'
    identity = '\0'.join(
        (
            config.robot_id,
            config.provider_instance_id,
            config.binding_contract_generation_id,
        )
    )
    digest = hashlib.sha256(identity.encode('utf-8')).hexdigest()[:8]
    return (
        f'manyforge_{portable_robot[:80]}_{digest}_'
        'execution_admission_ingress'
    )


class ExecutionAdmissionIngress:
    """Validate, fence, and translate one provider's admission stream."""

    def __init__(self, config: ExecutionAdmissionIngressConfig) -> None:
        if not isinstance(config, ExecutionAdmissionIngressConfig):
            raise TypeError(
                'config must be an ExecutionAdmissionIngressConfig'
            )
        self.config = config
        self._last_publication_generation = 0
        self._last_lifecycle_generation = 0
        self._last_source_snapshot_generation = 0
        self._last_payload_fingerprint: tuple[Any, ...] | None = None
        self._last_semantic_fingerprint: tuple[Any, ...] | None = None
        self._provider_observed = False
        self._provider_component_epoch = ''
        self._terminal_reason = ''
        self.accepted_total = 0
        self.duplicate_total = 0
        self.rejected_total = 0

    @property
    def terminal_reason(self) -> str:
        return self._terminal_reason

    def _reject(self, reason: str, *, terminal: bool = False) -> None:
        self.rejected_total += 1
        if terminal and not self._terminal_reason:
            self._terminal_reason = reason

    def mark_sink_rejection(self) -> None:
        self._reject('execution_admission_sink_rejected', terminal=True)

    def accept(
        self,
        message: Any,
        received_steady_time_ns: int,
    ) -> dict[str, Any] | None:
        """Return a native-backend snapshot, or ``None`` on refusal/duplicate."""
        if self._terminal_reason:
            self._reject('execution_admission_ingress_terminal')
            return None
        if (
            isinstance(received_steady_time_ns, bool)
            or not isinstance(received_steady_time_ns, int)
            or received_steady_time_ns <= 0
        ):
            self._reject('execution_admission_invalid_receipt_time')
            return None

        try:
            schema = message.schema
            provider_instance_id = message.provider_instance_id
            robot_id = message.robot_id
            binding_generation = message.binding_contract_generation_id
            runtime_generation = message.runtime_generation_id
            provider_component_epoch = message.provider_component_epoch
            transport_owner_component_epoch = (
                message.transport_owner_component_epoch
            )
            authority_component_epoch = message.authority_component_epoch
            cell_supervisor_component_epoch = (
                message.cell_supervisor_component_epoch
            )
            publication_generation = message.publication_generation
            lifecycle_generation = message.lifecycle_generation
            source_snapshot_generation = message.source_snapshot_generation
            projected_at_ns = message.projected_at_steady_time_ns
            lease_duration_ns = message.lease_duration_ns
            controller_ready = message.controller_ready
            motion_admitted = message.motion_admitted
            reason_codes = tuple(message.reason_codes)
        except (AttributeError, TypeError):
            self._reject('execution_admission_malformed')
            return None
        if (
            any(
                not isinstance(value, str)
                for value in (
                    schema,
                    provider_instance_id,
                    robot_id,
                    binding_generation,
                    runtime_generation,
                    provider_component_epoch,
                    transport_owner_component_epoch,
                    authority_component_epoch,
                    cell_supervisor_component_epoch,
                    *reason_codes,
                )
            )
            or any(
                isinstance(value, bool) or not isinstance(value, int)
                for value in (
                    publication_generation,
                    lifecycle_generation,
                    source_snapshot_generation,
                    projected_at_ns,
                    lease_duration_ns,
                )
            )
            or not isinstance(controller_ready, bool)
            or not isinstance(motion_admitted, bool)
        ):
            self._reject('execution_admission_malformed')
            return None

        if schema != EXECUTION_ADMISSION_SCHEMA:
            self._reject('execution_admission_schema_mismatch')
            return None
        if provider_instance_id != self.config.provider_instance_id:
            self._reject(
                'execution_admission_provider_mismatch',
                terminal=self._provider_observed,
            )
            return None
        self._provider_observed = True
        if robot_id != self.config.robot_id:
            self._reject('execution_admission_robot_mismatch', terminal=True)
            return None
        if binding_generation != self.config.binding_contract_generation_id:
            self._reject(
                'execution_admission_binding_mismatch',
                terminal=True,
            )
            return None
        if runtime_generation != self.config.runtime_generation_id:
            self._reject(
                'execution_admission_runtime_generation_mismatch',
                terminal=True,
            )
            return None
        if (
            cell_supervisor_component_epoch
            != self.config.cell_supervisor_component_epoch
        ):
            self._reject(
                'execution_admission_cell_supervisor_epoch_mismatch',
                terminal=True,
            )
            return None
        if self._provider_component_epoch:
            if provider_component_epoch != self._provider_component_epoch:
                self._reject(
                    'execution_admission_provider_epoch_changed',
                    terminal=True,
                )
                return None
        else:
            self._provider_component_epoch = provider_component_epoch
        if (
            publication_generation <= 0
            or lifecycle_generation <= 0
            or source_snapshot_generation <= 0
            or projected_at_ns <= 0
            or projected_at_ns > received_steady_time_ns
            or lease_duration_ns <= 0
            or not provider_component_epoch
            or (
                motion_admitted
                and (
                    not transport_owner_component_epoch
                    or not authority_component_epoch
                )
            )
            or any(not reason for reason in reason_codes)
            or tuple(sorted(reason_codes)) != reason_codes
            or len(set(reason_codes)) != len(reason_codes)
            or (motion_admitted and (not controller_ready or reason_codes))
        ):
            self._reject('execution_admission_invalid_payload')
            return None

        payload_fingerprint = (
            schema,
            provider_instance_id,
            robot_id,
            binding_generation,
            runtime_generation,
            provider_component_epoch,
            transport_owner_component_epoch,
            authority_component_epoch,
            cell_supervisor_component_epoch,
            publication_generation,
            lifecycle_generation,
            source_snapshot_generation,
            projected_at_ns,
            lease_duration_ns,
            controller_ready,
            motion_admitted,
            reason_codes,
        )
        semantic_fingerprint = (
            controller_ready,
            motion_admitted,
            reason_codes,
            runtime_generation,
            provider_component_epoch,
            transport_owner_component_epoch,
            authority_component_epoch,
            cell_supervisor_component_epoch,
        )
        if publication_generation == self._last_publication_generation:
            if payload_fingerprint == self._last_payload_fingerprint:
                self.duplicate_total += 1
                return None
            self._reject(
                'execution_admission_generation_conflict',
                terminal=True,
            )
            return None
        if (
            publication_generation < self._last_publication_generation
            or lifecycle_generation < self._last_lifecycle_generation
            or source_snapshot_generation
            < self._last_source_snapshot_generation
        ):
            self._reject(
                'execution_admission_generation_regression',
                terminal=True,
            )
            return None
        if (
            lifecycle_generation == self._last_lifecycle_generation
            and self._last_semantic_fingerprint is not None
            and semantic_fingerprint != self._last_semantic_fingerprint
        ):
            self._reject(
                'execution_admission_hidden_semantic_change',
                terminal=True,
            )
            return None

        effective_lease_ns = min(
            lease_duration_ns,
            self.config.maximum_lease_ns,
        )
        if projected_at_ns > _MAXIMUM_INT64 - effective_lease_ns:
            self._reject('execution_admission_invalid_payload')
            return None
        source_deadline_ns = projected_at_ns + effective_lease_ns
        if source_deadline_ns <= received_steady_time_ns:
            self._reject('execution_admission_stale_on_arrival')
            return None

        self._last_publication_generation = publication_generation
        self._last_lifecycle_generation = lifecycle_generation
        self._last_source_snapshot_generation = source_snapshot_generation
        self._last_payload_fingerprint = payload_fingerprint
        self._last_semantic_fingerprint = semantic_fingerprint
        self.accepted_total += 1
        return {
            'generation': publication_generation,
            'lifecycle_generation': lifecycle_generation,
            'robot_id': robot_id,
            'binding_contract_generation_id': binding_generation,
            'runtime_generation_id': runtime_generation,
            'provider_component_epoch': provider_component_epoch,
            'transport_owner_component_epoch': (
                transport_owner_component_epoch
            ),
            'authority_component_epoch': authority_component_epoch,
            'cell_supervisor_component_epoch': (
                cell_supervisor_component_epoch
            ),
            'observed_timestamp_ns': projected_at_ns,
            'received_timestamp_ns': received_steady_time_ns,
            'fresh_until_ns': source_deadline_ns,
            'controller_ready': controller_ready,
            'motion_admitted': motion_admitted,
            'reason_codes': list(reason_codes),
        }


class RosExecutionAdmissionSubscription:
    """Own one isolated rclpy subscriber and publish into the native bundle."""

    def __init__(
        self,
        *,
        native_bundle: Any,
        config: ExecutionAdmissionIngressConfig,
    ) -> None:
        import rclpy
        from manyforge_robot_lifecycle_msgs.msg import (
            ExecutionAdmissionSnapshot,
        )
        from rclpy.executors import SingleThreadedExecutor
        from rclpy.qos import DurabilityPolicy
        from rclpy.qos import HistoryPolicy
        from rclpy.qos import QoSProfile
        from rclpy.qos import ReliabilityPolicy

        self._native_bundle = native_bundle
        self.ingress = ExecutionAdmissionIngress(config)
        context = rclpy.Context()
        node = None
        executor = None
        thread = None
        try:
            rclpy.init(context=context)
            node = rclpy.create_node(
                _ingress_node_name(config),
                context=context,
                enable_rosout=False,
                start_parameter_services=False,
            )
            qos = QoSProfile(
                history=HistoryPolicy.KEEP_LAST,
                depth=1,
                reliability=ReliabilityPolicy.RELIABLE,
                durability=DurabilityPolicy.TRANSIENT_LOCAL,
            )
            subscription = node.create_subscription(
                ExecutionAdmissionSnapshot,
                config.topic,
                self._on_message,
                qos,
            )
            executor = SingleThreadedExecutor(context=context)
            executor.add_node(node)
            thread = threading.Thread(
                target=executor.spin,
                name='manyforge-execution-admission-ingress',
                daemon=False,
            )
            thread.start()
        except BaseException:
            if executor is not None:
                executor.shutdown(timeout_sec=2.0)
            if thread is not None and thread.is_alive():
                thread.join(timeout=2.0)
            if executor is not None and node is not None:
                executor.remove_node(node)
            if node is not None:
                node.destroy_node()
            if context.ok():
                context.shutdown()
            raise
        self._context = context
        self._node = node
        self._subscription = subscription
        self._executor = executor
        self._thread = thread
        self._closed = False
        self._close_lock = threading.Lock()

    def _on_message(self, message: Any) -> None:
        snapshot = self.ingress.accept(message, time.monotonic_ns())
        if snapshot is None:
            return
        if not self._native_bundle.publish_ros2_control_execution_admission_snapshot(
            snapshot
        ):
            self.ingress.mark_sink_rejection()

    def shutdown(self) -> None:
        with self._close_lock:
            if self._closed:
                return
            self._closed = True
            cleanup_errors: list[BaseException] = []
            try:
                self._executor.shutdown(timeout_sec=2.0)
            except BaseException as error:
                cleanup_errors.append(error)
            if self._thread.is_alive() and self._context.ok():
                try:
                    self._context.shutdown()
                except BaseException as error:
                    cleanup_errors.append(error)
            self._thread.join(timeout=2.0)
            if self._thread.is_alive():
                cleanup_errors.append(
                    RuntimeError(
                        'execution admission subscriber did not stop'
                    )
                )
            try:
                self._executor.remove_node(self._node)
            except BaseException as error:
                cleanup_errors.append(error)
            try:
                self._node.destroy_node()
            except BaseException as error:
                cleanup_errors.append(error)
            if self._context.ok():
                try:
                    self._context.shutdown()
                except BaseException as error:
                    cleanup_errors.append(error)
            if cleanup_errors:
                raise RuntimeError(
                    'execution admission subscriber cleanup failed: '
                    + '; '.join(str(error) for error in cleanup_errors)
                ) from cleanup_errors[0]
