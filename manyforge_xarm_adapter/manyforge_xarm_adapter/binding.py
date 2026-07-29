"""Deterministic xArm endpoint compilation with no ROS runtime dependency."""

from __future__ import annotations

from dataclasses import asdict
from dataclasses import dataclass
import hashlib
import json
import re


_ROS_TOKEN = re.compile(r'^[A-Za-z_][A-Za-z0-9_]*$')


def _token(label: str, value: str, *, allow_empty: bool = False) -> str:
    if not isinstance(value, str):
        raise ValueError(f'{label} must be a string')
    if not value:
        if allow_empty:
            return ''
        raise ValueError(f'{label} must not be empty')
    if not _ROS_TOKEN.fullmatch(value):
        raise ValueError(f'{label} is not a valid ROS name token: {value!r}')
    return value


def _namespace(value: str) -> str:
    if not isinstance(value, str):
        raise ValueError('ros_namespace must be a string')
    normalized = value.strip('/')
    if not normalized:
        return ''
    for part in normalized.split('/'):
        _token('ros_namespace segment', part)
    return normalized


def _absolute_name(namespace: str, *parts: str) -> str:
    return '/' + '/'.join(part for part in (namespace, *parts) if part)


@dataclass(frozen=True)
class Lite6BindingInputs:
    """Inputs that determine every ROS name for one Lite6 instance."""

    ros_namespace: str = ''
    prefix: str = ''
    hardware_namespace: str = 'ufactory'
    controller_name: str = 'lite6_traj_controller'


@dataclass(frozen=True)
class ResolvedLite6Binding:
    """Immutable names used by observation now and controller binding later."""

    generation_id: str
    ros_namespace: str
    prefix: str
    hardware_namespace: str
    driver_node: str
    joint_state_topic: str
    validated_joint_state_topic: str
    robot_state_topic: str
    diagnostics_topic: str
    controller_io_state_topic: str
    controller_manager: str
    trajectory_controller: str
    follow_joint_trajectory_action: str
    supervised_state_topic: str
    supervised_execute_service: str
    supervised_command_gate_service: str
    supervised_shutdown_controller_service: str
    arm_joint_names: tuple[str, ...]

    def to_dict(self) -> dict[str, object]:
        """Return a JSON-serializable manifest fragment."""
        payload = asdict(self)
        payload['arm_joint_names'] = list(self.arm_joint_names)
        return payload


def compile_lite6_binding(inputs: Lite6BindingInputs) -> ResolvedLite6Binding:
    """Resolve the xArm prefix/namespace convention once and fail closed."""
    namespace = _namespace(inputs.ros_namespace)
    prefix = _token('prefix', inputs.prefix, allow_empty=True)
    hardware_namespace = _token(
        'hardware_namespace',
        inputs.hardware_namespace,
    )
    controller_name = _token('controller_name', inputs.controller_name)

    resolved_hardware_namespace = f'{prefix}{hardware_namespace}'
    resolved_controller = f'{prefix}{controller_name}'
    joints = tuple(f'{prefix}joint{index}' for index in range(1, 7))
    identity = {
        'ros_namespace': namespace,
        'prefix': prefix,
        'hardware_namespace': hardware_namespace,
        'controller_name': controller_name,
        'arm_joint_names': joints,
    }
    generation_id = hashlib.sha256(
        json.dumps(identity, sort_keys=True, separators=(',', ':')).encode('utf-8')
    ).hexdigest()

    return ResolvedLite6Binding(
        generation_id=generation_id,
        ros_namespace=namespace,
        prefix=prefix,
        hardware_namespace=resolved_hardware_namespace,
        driver_node=_absolute_name(namespace, 'ufactory_driver'),
        joint_state_topic=_absolute_name(
            namespace,
            resolved_hardware_namespace,
            'joint_states',
        ),
        validated_joint_state_topic=_absolute_name(
            namespace,
            'manyforge',
            'xarm',
            'validated_joint_states',
        ),
        robot_state_topic=_absolute_name(
            namespace,
            resolved_hardware_namespace,
            'robot_states',
        ),
        diagnostics_topic=_absolute_name(
            namespace,
            'manyforge',
            'xarm',
            'diagnostics',
        ),
        controller_io_state_topic=_absolute_name(
            namespace,
            resolved_hardware_namespace,
            'xarm_cgpio_states',
        ),
        controller_manager=_absolute_name(namespace, 'controller_manager'),
        trajectory_controller=_absolute_name(namespace, resolved_controller),
        follow_joint_trajectory_action=_absolute_name(
            namespace,
            resolved_controller,
            'follow_joint_trajectory',
        ),
        supervised_state_topic=_absolute_name(
            namespace,
            resolved_hardware_namespace,
            'supervised_lifecycle',
            'state',
        ),
        supervised_execute_service=_absolute_name(
            namespace,
            resolved_hardware_namespace,
            'supervised_lifecycle',
            'execute',
        ),
        supervised_command_gate_service=_absolute_name(
            namespace,
            resolved_hardware_namespace,
            'supervised_lifecycle',
            'set_command_gate',
        ),
        supervised_shutdown_controller_service=_absolute_name(
            namespace,
            resolved_hardware_namespace,
            'supervised_lifecycle',
            'shutdown_controller',
        ),
        arm_joint_names=joints,
    )
