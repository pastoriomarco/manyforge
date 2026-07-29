from __future__ import annotations

import time
import uuid

from manyforge_ros.execution_admission import EXECUTION_ADMISSION_SCHEMA
from manyforge_ros.execution_admission import (
    ExecutionAdmissionIngressConfig,
)
from manyforge_ros.execution_admission import (
    RosExecutionAdmissionSubscription,
)
import pytest


rclpy = pytest.importorskip('rclpy')
messages = pytest.importorskip('manyforge_robot_lifecycle_msgs.msg')


class FakeNativeBundle:
    def __init__(self) -> None:
        self.snapshots: list[dict[str, object]] = []

    def publish_ros2_control_execution_admission_snapshot(
        self,
        snapshot: dict[str, object],
    ) -> bool:
        self.snapshots.append(snapshot)
        return True


def test_transient_local_ros_message_reaches_native_sink() -> None:
    ExecutionAdmissionSnapshot = getattr(
        messages,
        'ExecutionAdmissionSnapshot',
        None,
    )
    if ExecutionAdmissionSnapshot is None:
        pytest.skip('manyforge_robot_lifecycle_msgs is not installed')
    from rclpy.qos import DurabilityPolicy
    from rclpy.qos import HistoryPolicy
    from rclpy.qos import QoSProfile
    from rclpy.qos import ReliabilityPolicy
    from rclpy.executors import SingleThreadedExecutor

    topic = f'/manyforge_test/admission_{uuid.uuid4().hex}'
    native = FakeNativeBundle()
    subscription = RosExecutionAdmissionSubscription(
        native_bundle=native,
        config=ExecutionAdmissionIngressConfig(
            topic=topic,
            provider_instance_id='provider-session-1',
            robot_id='lite6-a',
            binding_contract_generation_id='binding-1',
            runtime_generation_id='runtime-1',
            cell_supervisor_component_epoch='supervisor-epoch-1',
            maximum_lease_ns=1_000_000_000,
        ),
    )
    assert subscription._node.get_name().startswith(
        'manyforge_lite6_a_'
    )
    context = rclpy.Context()
    rclpy.init(context=context)
    publisher_node = rclpy.create_node(
        'manyforge_execution_admission_test_publisher',
        context=context,
        enable_rosout=False,
        start_parameter_services=False,
    )
    publisher = publisher_node.create_publisher(
        ExecutionAdmissionSnapshot,
        topic,
        QoSProfile(
            history=HistoryPolicy.KEEP_LAST,
            depth=1,
            reliability=ReliabilityPolicy.RELIABLE,
            durability=DurabilityPolicy.TRANSIENT_LOCAL,
        ),
    )
    executor = SingleThreadedExecutor(context=context)
    executor.add_node(publisher_node)
    try:
        discovery_deadline = time.monotonic() + 3.0
        while (
            publisher.get_subscription_count() == 0
            and time.monotonic() < discovery_deadline
        ):
            executor.spin_once(timeout_sec=0.01)
        assert publisher.get_subscription_count() == 1

        now_ns = time.monotonic_ns()
        message = ExecutionAdmissionSnapshot()
        message.schema = EXECUTION_ADMISSION_SCHEMA
        message.provider_instance_id = 'provider-session-1'
        message.robot_id = 'lite6-a'
        message.binding_contract_generation_id = 'binding-1'
        message.runtime_generation_id = 'runtime-1'
        message.provider_component_epoch = 'provider-epoch-1'
        message.transport_owner_component_epoch = 'owner-epoch-1'
        message.authority_component_epoch = 'authority-epoch-1'
        message.cell_supervisor_component_epoch = 'supervisor-epoch-1'
        message.publication_generation = 1
        message.lifecycle_generation = 1
        message.source_snapshot_generation = 1
        message.projected_at_steady_time_ns = now_ns
        message.lease_duration_ns = 1_000_000_000
        message.controller_ready = False
        message.motion_admitted = False
        message.reason_codes = ['authority_session_stale_or_missing']
        publisher.publish(message)

        delivery_deadline = time.monotonic() + 3.0
        while not native.snapshots and time.monotonic() < delivery_deadline:
            executor.spin_once(timeout_sec=0.01)
        assert len(native.snapshots) == 1
        assert native.snapshots[0]['robot_id'] == 'lite6-a'
        assert native.snapshots[0]['motion_admitted'] is False
        assert subscription.ingress.accepted_total == 1
    finally:
        publisher_node.destroy_publisher(publisher)
        executor.remove_node(publisher_node)
        executor.shutdown(timeout_sec=2.0)
        publisher_node.destroy_node()
        context.shutdown()
        subscription.shutdown()
        subscription.shutdown()
