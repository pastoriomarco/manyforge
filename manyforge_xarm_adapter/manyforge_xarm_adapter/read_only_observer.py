"""Publish diagnostics for the read-only xArm observation surface."""

from __future__ import annotations

import math
import signal
import time

from diagnostic_msgs.msg import DiagnosticArray
from diagnostic_msgs.msg import DiagnosticStatus
from diagnostic_msgs.msg import KeyValue
import rclpy
from rclpy.executors import ExternalShutdownException
from rclpy.node import Node
from sensor_msgs.msg import JointState
from xarm_msgs.msg import RobotMsg


class ReadOnlyObserver(Node):
    """Observe driver state without owning any service, action, or command topic."""

    def __init__(self) -> None:
        super().__init__('manyforge_xarm_read_only_observer')
        self.declare_parameter('robot_state_topic', '/ufactory/robot_states')
        self.declare_parameter('joint_state_topic', '/ufactory/joint_states')
        self.declare_parameter(
            'validated_joint_state_topic',
            '/manyforge/xarm/validated_joint_states',
        )
        self.declare_parameter('diagnostics_topic', '/manyforge/xarm/diagnostics')
        self.declare_parameter(
            'expected_joint_names',
            [f'joint{index}' for index in range(1, 7)],
        )
        self.declare_parameter('stale_after_s', 0.75)
        self.declare_parameter('source_agreement_tolerance_rad', 0.002)

        self._robot_received_at: float | None = None
        self._joints_received_at: float | None = None
        self._robot: RobotMsg | None = None
        self._joint_names: tuple[str, ...] = ()
        self._joint_positions: tuple[float, ...] = ()
        self._position_initialized = False
        self._expected_joint_names = tuple(
            str(value)
            for value in self.get_parameter('expected_joint_names').value
        )
        self._stale_after_s = float(self.get_parameter('stale_after_s').value)
        if self._stale_after_s <= 0:
            raise ValueError('stale_after_s must be positive')
        self._source_agreement_tolerance_rad = float(
            self.get_parameter('source_agreement_tolerance_rad').value
        )
        if self._source_agreement_tolerance_rad <= 0:
            raise ValueError(
                'source_agreement_tolerance_rad must be positive'
            )
        self._expected_servo_mask = (1 << len(self._expected_joint_names)) - 1

        robot_topic = str(self.get_parameter('robot_state_topic').value)
        joint_topic = str(self.get_parameter('joint_state_topic').value)
        validated_joint_topic = str(
            self.get_parameter('validated_joint_state_topic').value
        )
        diagnostics_topic = str(self.get_parameter('diagnostics_topic').value)
        self._robot_subscription = self.create_subscription(
            RobotMsg,
            robot_topic,
            self._on_robot,
            10,
        )
        self._joint_subscription = self.create_subscription(
            JointState,
            joint_topic,
            self._on_joints,
            10,
        )
        self._validated_joints = self.create_publisher(
            JointState,
            validated_joint_topic,
            10,
        )
        self._diagnostics = self.create_publisher(
            DiagnosticArray,
            diagnostics_topic,
            10,
        )
        self._timer = self.create_timer(0.2, self._publish_diagnostics)
        self.get_logger().info(
            'xArm observation started with motion admission closed and no '
            'command endpoints'
        )

    def _on_robot(self, message: RobotMsg) -> None:
        self._robot = message
        self._robot_received_at = time.monotonic()
        self._update_position_initialization()

    def _on_joints(self, message: JointState) -> None:
        self._joint_names = tuple(message.name)
        self._joint_positions = tuple(float(value) for value in message.position)
        self._joints_received_at = time.monotonic()
        self._update_position_initialization()
        if self._position_initialized and self._joint_sample_issue() is None:
            self._validated_joints.publish(message)

    def _joint_sample_issue(self) -> str | None:
        if len(self._joint_names) != len(self._joint_positions):
            return 'position_length_mismatch'
        if len(set(self._joint_names)) != len(self._joint_names):
            return 'duplicate_joint_name'
        if set(self._expected_joint_names) - set(self._joint_names):
            return 'mapping_incomplete'
        if not all(math.isfinite(value) for value in self._joint_positions):
            return 'position_nonfinite'
        return None

    def _source_delta_rad(self) -> float | None:
        if self._robot is None or self._joint_sample_issue() is not None:
            return None
        if len(self._robot.angle) < len(self._expected_joint_names):
            return None
        robot_angles = tuple(
            float(self._robot.angle[index])
            for index in range(len(self._expected_joint_names))
        )
        if not all(math.isfinite(value) for value in robot_angles):
            return None
        positions = dict(zip(self._joint_names, self._joint_positions))
        return max(
            abs(positions[name] - robot_angles[index])
            for index, name in enumerate(self._expected_joint_names)
        )

    def _robot_fully_enabled(self) -> bool:
        if self._robot is None or self._robot.err:
            return False
        return (
            self._robot.mt_able & self._expected_servo_mask
        ) == self._expected_servo_mask and (
            self._robot.mt_brake & self._expected_servo_mask
        ) == self._expected_servo_mask

    def _update_position_initialization(self) -> None:
        if self._position_initialized or not self._robot_fully_enabled():
            return
        source_delta = self._source_delta_rad()
        if (
            source_delta is not None
            and source_delta <= self._source_agreement_tolerance_rad
        ):
            self._position_initialized = True

    def _position_state(
        self,
        *,
        robot_age: float,
        joint_age: float,
    ) -> tuple[bool, str]:
        issue = self._joint_sample_issue()
        if issue is not None:
            return False, issue
        if robot_age > self._stale_after_s or joint_age > self._stale_after_s:
            return False, 'stale'
        if self._position_initialized:
            return True, 'valid'
        if not self._robot_fully_enabled():
            return False, 'awaiting_drive_initialization'
        return False, 'awaiting_source_agreement'

    @staticmethod
    def _value(key: str, value: object) -> KeyValue:
        return KeyValue(key=key, value=str(value))

    def _publish_diagnostics(self) -> None:
        now = time.monotonic()
        robot_age = (
            float('inf')
            if self._robot_received_at is None
            else now - self._robot_received_at
        )
        joint_age = (
            float('inf')
            if self._joints_received_at is None
            else now - self._joints_received_at
        )
        missing_joints = sorted(
            set(self._expected_joint_names) - set(self._joint_names)
        )
        position_valid, position_reason = self._position_state(
            robot_age=robot_age,
            joint_age=joint_age,
        )
        source_delta = self._source_delta_rad()
        if source_delta is None:
            source_agreement = 'unavailable'
        elif source_delta <= self._source_agreement_tolerance_rad:
            source_agreement = 'agreeing'
        else:
            source_agreement = 'divergent'

        level = DiagnosticStatus.OK
        message = 'observing'
        if robot_age > self._stale_after_s or joint_age > self._stale_after_s:
            level = DiagnosticStatus.ERROR
            message = 'observation_stale'
        elif missing_joints:
            level = DiagnosticStatus.ERROR
            message = 'joint_mapping_incomplete'
        elif self._robot is not None and self._robot.err:
            level = DiagnosticStatus.ERROR
            message = 'observing_fault'
        elif not position_valid:
            level = DiagnosticStatus.ERROR
            message = 'position_unavailable'
        elif self._robot is not None and self._robot.warn:
            level = DiagnosticStatus.WARN
            message = 'observing_warning'

        values = [
            self._value('access_mode', 'read_only'),
            self._value('motion_admission', 'closed'),
            self._value('command_endpoints', 'suppressed'),
            self._value('robot_state_age_s', f'{robot_age:.6f}'),
            self._value('joint_state_age_s', f'{joint_age:.6f}'),
            self._value('missing_joints', ','.join(missing_joints)),
            self._value(
                'joint_position_initialized',
                str(self._position_initialized).lower(),
            ),
            self._value(
                'joint_position_valid',
                str(position_valid).lower(),
            ),
            self._value('joint_position_reason', position_reason),
            self._value('expected_servo_mask', self._expected_servo_mask),
            self._value('robot_joint_agreement', source_agreement),
            self._value(
                'robot_joint_max_abs_delta_rad',
                (
                    'unavailable'
                    if source_delta is None
                    else f'{source_delta:.9f}'
                ),
            ),
        ]
        if self._robot is not None:
            values.extend(
                [
                    self._value('robot_state', self._robot.state),
                    self._value('robot_mode', self._robot.mode),
                    self._value('robot_error', self._robot.err),
                    self._value('robot_warning', self._robot.warn),
                    self._value('robot_brake_mask', self._robot.mt_brake),
                    self._value('robot_servo_mask', self._robot.mt_able),
                ]
            )
        status = DiagnosticStatus(
            level=level,
            name='manyforge/xarm/read_only_observation',
            message=message,
            hardware_id='lite6',
            values=values,
        )
        output = DiagnosticArray()
        output.header.stamp = self.get_clock().now().to_msg()
        output.status = [status]
        self._diagnostics.publish(output)


def main(args: list[str] | None = None) -> None:
    """Run the read-only observer."""
    rclpy.init(args=args)
    node = ReadOnlyObserver()
    try:
        rclpy.spin(node)
    except (ExternalShutdownException, KeyboardInterrupt):
        pass
    finally:
        # A terminal SIGINT reaches the foreground process group, then
        # ros2 launch forwards SIGINT to each child. Ignore that duplicate
        # delivery once shutdown has begun so it cannot interrupt cleanup.
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        node.destroy_node()
        rclpy.try_shutdown()


if __name__ == '__main__':
    main()
