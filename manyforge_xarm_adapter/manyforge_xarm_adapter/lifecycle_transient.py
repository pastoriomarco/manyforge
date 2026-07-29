"""Bounded joint/state sampling around attended lifecycle commands."""

from __future__ import annotations

from dataclasses import dataclass
import math
import statistics
import threading
import time
from typing import Any

import rclpy
from rclpy.context import Context
from rclpy.executors import MultiThreadedExecutor
from rclpy.node import Node
from rclpy.qos import DurabilityPolicy
from rclpy.qos import QoSProfile
from rclpy.qos import ReliabilityPolicy
from sensor_msgs.msg import JointState
from xarm_msgs.msg import SupervisedLifecycleState


MAX_TRANSIENT_SAMPLES = 8192


@dataclass(frozen=True)
class TransientJointSample:
    """One ordered joint sample with its contemporaneous owner facts."""

    received_at_steady_ns: int
    positions: tuple[float, ...]
    state_generation: int
    position_valid: bool
    brake_mask: int
    servo_enable_mask: int


def summarize_transient(
    *,
    samples: list[TransientJointSample],
    command_started_ns: int,
    command_finished_ns: int,
    joint_count: int,
    capture_window_ms: int,
    max_deviation_rad: float | None,
) -> dict[str, object]:
    """Reduce raw transient samples without treating invalid zeroes as pose."""
    expected_mask = (1 << joint_count) - 1
    post_samples = [
        sample
        for sample in samples
        if sample.received_at_steady_ns >= command_started_ns
    ]
    qualified = [
        sample
        for sample in post_samples
        if (
            sample.position_valid
            and sample.brake_mask & expected_mask == expected_mask
            and sample.servo_enable_mask & expected_mask == expected_mask
        )
    ]
    max_deviation = None
    if qualified:
        anchor = qualified[0].positions
        max_deviation = max(
            abs(value - anchor[index])
            for sample in qualified
            for index, value in enumerate(sample.positions)
        )
    interarrival_ms = [
        (
            samples[index].received_at_steady_ns
            - samples[index - 1].received_at_steady_ns
        )
        / 1_000_000.0
        for index in range(1, len(samples))
        if (
            samples[index].received_at_steady_ns
            > samples[index - 1].received_at_steady_ns
        )
    ]
    capture_complete = len(post_samples) >= 2 and len(qualified) >= 2
    within_bound = (
        max_deviation is not None
        and max_deviation_rad is not None
        and max_deviation <= max_deviation_rad
    )
    if not capture_complete:
        assessment = 'capture_incomplete'
    elif max_deviation_rad is None:
        assessment = 'captured_requires_bound_adjudication'
    elif within_bound:
        assessment = 'within_explicit_bound'
    else:
        assessment = 'exceeds_explicit_bound'
    first_qualified_delay_ms = (
        (
            qualified[0].received_at_steady_ns
            - command_started_ns
        )
        / 1_000_000.0
        if qualified
        else None
    )
    return {
        'capture_window_ms': capture_window_ms,
        'sample_count': len(samples),
        'post_command_sample_count': len(post_samples),
        'qualified_post_command_sample_count': len(qualified),
        'command_duration_ms': (
            command_finished_ns - command_started_ns
        )
        / 1_000_000.0,
        'first_qualified_delay_ms': first_qualified_delay_ms,
        'median_sample_interarrival_ms': (
            statistics.median(interarrival_ms)
            if interarrival_ms
            else None
        ),
        'first_post_command_positions_rad': (
            list(post_samples[0].positions) if post_samples else None
        ),
        'first_qualified_positions_rad': (
            list(qualified[0].positions) if qualified else None
        ),
        'last_qualified_positions_rad': (
            list(qualified[-1].positions) if qualified else None
        ),
        'max_qualified_pose_deviation_rad': max_deviation,
        'max_pose_deviation_limit_rad': max_deviation_rad,
        'capture_complete': capture_complete,
        'within_explicit_bound': (
            within_bound if max_deviation_rad is not None else None
        ),
        'assessment': assessment,
    }


class LifecycleTransientSampler:
    """ROS-only transient sampler; it never creates a command endpoint."""

    def __init__(self, binding: dict[str, Any]) -> None:
        controller = binding['controller_binding']
        self._joint_names = tuple(controller.get('arm_joint_names', ()))
        if not self._joint_names:
            raise ValueError(
                'transient capture requires controller arm_joint_names'
            )
        self._expected_owner = str(
            binding['runtime']['transport_owner_id']
        )
        self._lock = threading.RLock()
        self._condition = threading.Condition(self._lock)
        self._latest_state: SupervisedLifecycleState | None = None
        self._samples: list[TransientJointSample] = []

        self._context = Context()
        rclpy.init(context=self._context)
        self._node = Node(
            'manyforge_lite6_lifecycle_transient_evidence',
            context=self._context,
        )
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
        self._joint_subscription = self._node.create_subscription(
            JointState,
            str(controller['joint_state_topic']),
            self._on_joint_state,
            10,
        )
        self._executor = MultiThreadedExecutor(
            num_threads=2,
            context=self._context,
        )
        self._executor.add_node(self._node)
        self._thread = threading.Thread(
            target=self._executor.spin,
            name='lite6-lifecycle-transient-evidence-ros',
        )
        self._thread.start()

    def _on_state(self, message: SupervisedLifecycleState) -> None:
        if message.owner_id != self._expected_owner:
            return
        with self._condition:
            self._latest_state = message
            self._condition.notify_all()

    def _on_joint_state(self, message: JointState) -> None:
        if len(message.name) != len(message.position):
            return
        values = dict(zip(message.name, message.position))
        if not all(joint in values for joint in self._joint_names):
            return
        positions = tuple(
            float(values[joint]) for joint in self._joint_names
        )
        if not all(math.isfinite(value) for value in positions):
            return
        with self._condition:
            if (
                self._latest_state is None
                or len(self._samples) >= MAX_TRANSIENT_SAMPLES
            ):
                return
            state = self._latest_state
            self._samples.append(
                TransientJointSample(
                    received_at_steady_ns=time.monotonic_ns(),
                    positions=positions,
                    state_generation=state.generation,
                    position_valid=state.position_valid,
                    brake_mask=state.brake_mask,
                    servo_enable_mask=state.servo_enable_mask,
                )
            )
            self._condition.notify_all()

    def wait_until_ready(self, timeout_ms: int) -> None:
        deadline_ns = time.monotonic_ns() + timeout_ms * 1_000_000
        with self._condition:
            while self._latest_state is None or not self._samples:
                remaining_ns = deadline_ns - time.monotonic_ns()
                if remaining_ns <= 0:
                    raise TimeoutError(
                        'lifecycle_transient_initial_sample_unavailable'
                    )
                self._condition.wait(remaining_ns / 1_000_000_000)

    def finish(
        self,
        *,
        command_started_ns: int,
        command_finished_ns: int,
        capture_window_ms: int,
        max_deviation_rad: float | None,
    ) -> dict[str, object]:
        deadline_ns = (
            command_finished_ns + capture_window_ms * 1_000_000
        )
        with self._condition:
            while time.monotonic_ns() < deadline_ns:
                remaining_ns = deadline_ns - time.monotonic_ns()
                self._condition.wait(remaining_ns / 1_000_000_000)
            samples = list(self._samples)
        return summarize_transient(
            samples=samples,
            command_started_ns=command_started_ns,
            command_finished_ns=command_finished_ns,
            joint_count=len(self._joint_names),
            capture_window_ms=capture_window_ms,
            max_deviation_rad=max_deviation_rad,
        )

    def close(self) -> None:
        self._executor.shutdown(timeout_sec=5.0)
        self._thread.join(timeout=5.0)
        if self._thread.is_alive():
            raise RuntimeError('transient evidence executor did not stop')
        self._node.destroy_node()
        self._context.shutdown()
