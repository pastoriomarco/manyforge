from __future__ import annotations

import time

from diagnostic_msgs.msg import DiagnosticArray
from diagnostic_msgs.msg import DiagnosticStatus
from manyforge_xarm_adapter.read_only_observer import ReadOnlyObserver
import rclpy
from rclpy.executors import SingleThreadedExecutor
from rclpy.node import Node
from sensor_msgs.msg import JointState
from xarm_msgs.msg import RobotMsg


def _values(message: DiagnosticArray) -> dict[str, str]:
    assert len(message.status) == 1
    return {item.key: item.value for item in message.status[0].values}


def test_fake_driver_state_is_observed_without_opening_motion() -> None:
    rclpy.init()
    observer = ReadOnlyObserver()
    fixture = Node('manyforge_xarm_fake_read_only_driver')
    executor = SingleThreadedExecutor()
    executor.add_node(observer)
    executor.add_node(fixture)
    received: list[DiagnosticArray] = []
    validated_joints: list[JointState] = []
    diagnostic_subscription = fixture.create_subscription(
        DiagnosticArray,
        '/manyforge/xarm/diagnostics',
        received.append,
        10,
    )
    validated_subscription = fixture.create_subscription(
        JointState,
        '/manyforge/xarm/validated_joint_states',
        validated_joints.append,
        10,
    )
    robot_publisher = fixture.create_publisher(
        RobotMsg,
        '/ufactory/robot_states',
        10,
    )
    joint_publisher = fixture.create_publisher(
        JointState,
        '/ufactory/joint_states',
        10,
    )

    try:
        robot = RobotMsg(state=4, mode=0, err=2, warn=0)
        robot.angle = [0.0] * 6
        joints = JointState()
        joints.name = [f'joint{index}' for index in range(1, 7)]
        joints.position = [0.0] * 6

        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            robot_publisher.publish(robot)
            joint_publisher.publish(joints)
            executor.spin_once(timeout_sec=0.02)
            if (
                received
                and received[-1].status[0].message == 'observing_fault'
            ):
                break

        assert received
        status = received[-1].status[0]
        assert status.level == DiagnosticStatus.ERROR
        assert status.message == 'observing_fault'
        assert _values(received[-1])['access_mode'] == 'read_only'
        assert _values(received[-1])['motion_admission'] == 'closed'
        assert _values(received[-1])['command_endpoints'] == 'suppressed'
        assert _values(received[-1])['joint_position_initialized'] == 'false'
        assert _values(received[-1])['joint_position_valid'] == 'false'
        assert (
            _values(received[-1])['joint_position_reason']
            == 'awaiting_drive_initialization'
        )
        assert not validated_joints

        robot.state = 2
        robot.err = 0
        robot.mt_brake = 63
        robot.mt_able = 63
        deadline = time.monotonic() + 1.0
        while time.monotonic() < deadline:
            robot_publisher.publish(robot)
            joint_publisher.publish(joints)
            executor.spin_once(timeout_sec=0.02)
            if (
                received[-1].status[0].message == 'observing'
                and validated_joints
            ):
                break

        assert received[-1].status[0].level == DiagnosticStatus.OK
        assert received[-1].status[0].message == 'observing'
        assert _values(received[-1])['joint_position_initialized'] == 'true'
        assert _values(received[-1])['joint_position_valid'] == 'true'
        assert _values(received[-1])['robot_joint_agreement'] == 'agreeing'
        assert validated_joints

        robot.state = 4
        robot.err = 2
        robot.mt_brake = 0
        robot.mt_able = 0
        joints.position[3] = -0.007
        deadline = time.monotonic() + 1.0
        while time.monotonic() < deadline:
            robot_publisher.publish(robot)
            joint_publisher.publish(joints)
            executor.spin_once(timeout_sec=0.02)
            if (
                received[-1].status[0].message == 'observing_fault'
                and _values(received[-1])['robot_joint_agreement']
                == 'divergent'
            ):
                break

        assert received[-1].status[0].level == DiagnosticStatus.ERROR
        assert received[-1].status[0].message == 'observing_fault'
        assert _values(received[-1])['robot_error'] == '2'
        assert _values(received[-1])['joint_position_initialized'] == 'true'
        assert _values(received[-1])['joint_position_valid'] == 'true'
        assert _values(received[-1])['robot_joint_agreement'] == 'divergent'
        assert validated_joints[-1].position[3] == -0.007
    finally:
        fixture.destroy_subscription(validated_subscription)
        fixture.destroy_subscription(diagnostic_subscription)
        fixture.destroy_node()
        observer.destroy_node()
        executor.shutdown()
        rclpy.shutdown()
