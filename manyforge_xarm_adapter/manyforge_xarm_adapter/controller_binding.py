"""Compile and verify a generated Lite6 controller artifact offline."""

from __future__ import annotations

from dataclasses import asdict
from dataclasses import dataclass
import hashlib
import json
from typing import Any
import xml.etree.ElementTree as ET

import yaml

from .binding import ResolvedLite6Binding


CONTROLLER_BINDING_SCHEMA = (
    'manyforge.xarm_adapter.controller_binding.v1.experimental'
)
ROS2_CONTROL_EXECUTION_BINDING_SCHEMA = (
    'manyforge.ros2_control.execution_binding.v0.experimental'
)
SUPERVISED_HARDWARE_PLUGIN = (
    'uf_robot_hardware/UFRobotSystemHardware'
)
SUPERVISED_FAKE_HARDWARE_PLUGIN = (
    'mock_components/GenericSystem'
)
TRAJECTORY_CONTROLLER_TYPE = (
    'joint_trajectory_controller/JointTrajectoryController'
)
JOINT_STATE_BROADCASTER_TYPE = (
    'joint_state_broadcaster/JointStateBroadcaster'
)
EXPECTED_COMMAND_INTERFACES = ('position', 'velocity')
EXPECTED_STATE_INTERFACES = ('position', 'velocity')
ACTIVITY_QOS = {
    'depth': 1,
    'durability': 'transient_local',
    'reliability': 'reliable',
}


def _sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode('utf-8')).hexdigest()


def _joint_state_broadcaster_topic(ros_namespace: str) -> str:
    namespace = ros_namespace.strip('/')
    return f'/{namespace}/joint_states' if namespace else '/joint_states'


def _positive_number(value: Any, label: str) -> float:
    if (
        not isinstance(value, (int, float))
        or isinstance(value, bool)
        or value <= 0
    ):
        raise ValueError(f'{label} must be positive')
    return float(value)


def _string_list(value: Any, label: str) -> tuple[str, ...]:
    if (
        not isinstance(value, list)
        or not value
        or not all(isinstance(item, str) and item for item in value)
    ):
        raise ValueError(f'{label} must be a non-empty string list')
    if len(set(value)) != len(value):
        raise ValueError(f'{label} contains duplicates')
    return tuple(value)


def _hardware_parameters(hardware: ET.Element) -> dict[str, str]:
    result: dict[str, str] = {}
    for parameter in hardware.findall('./param'):
        name = parameter.get('name', '')
        value = parameter.text or ''
        if not name:
            raise ValueError(
                'supervised hardware parameters require non-empty names'
            )
        if name in result:
            raise ValueError(
                f'supervised hardware parameter is duplicated: {name}'
            )
        result[name] = value
    return result


def _namespace_controller_parameters(
    payload: dict[str, Any],
    ros_namespace: str,
) -> dict[str, Any]:
    if not ros_namespace:
        return payload
    namespace = ros_namespace.strip('/')
    current: Any = payload
    for segment in namespace.split('/'):
        if not isinstance(current, dict) or segment not in current:
            raise ValueError(
                'controller YAML does not contain the resolved ROS namespace'
            )
        current = current[segment]
    if not isinstance(current, dict):
        raise ValueError('controller YAML namespace root must be a mapping')
    return current


@dataclass(frozen=True)
class ProductionControllerBinding:
    """Immutable facts compiled from generated URDF and controller YAML."""

    schema: str
    generation_id: str
    ros_namespace: str
    model_prefix: str
    hardware_namespace: str
    controller_manager: str
    controller_manager_activity_topic: str
    controller_manager_update_rate_hz: float
    controller_manager_activity_qos: dict[str, object]
    binding_snapshot_max_age_ms: int
    hardware_component_name: str
    hardware_plugin: str
    joint_state_broadcaster_manager_name: str
    trajectory_controller_manager_name: str
    trajectory_controller_name: str
    trajectory_controller_type: str
    follow_joint_trajectory_action: str
    supervised_state_topic: str
    supervised_execute_service: str
    supervised_command_gate_service: str
    supervised_shutdown_controller_service: str
    controller_state_topic: str
    joint_state_topic: str
    validated_joint_state_topic: str
    arm_joint_names: tuple[str, ...]
    hardware_command_interfaces: tuple[str, ...]
    hardware_state_interfaces: tuple[str, ...]
    controller_command_interfaces: tuple[str, ...]
    controller_state_interfaces: tuple[str, ...]
    decelerate_on_cancel: bool
    max_deceleration_on_cancel_rad_s2: dict[str, float]
    claimed_command_interface_names: tuple[str, ...]
    state_interface_names: tuple[str, ...]
    state_publish_rate_hz: float
    action_monitor_rate_hz: float
    runtime_urdf_sha256: str
    controllers_yaml_sha256: str

    def to_dict(self) -> dict[str, object]:
        """Return a JSON-serializable binding document."""
        payload = asdict(self)
        for field in (
            'arm_joint_names',
            'hardware_command_interfaces',
            'hardware_state_interfaces',
            'controller_command_interfaces',
            'controller_state_interfaces',
            'claimed_command_interface_names',
            'state_interface_names',
        ):
            payload[field] = list(payload[field])
        return payload

    def to_ros2_control_execution_binding(self) -> dict[str, object]:
        """Project vendor facts into the retained generic-contract fixture."""
        return {
            'schema': ROS2_CONTROL_EXECUTION_BINDING_SCHEMA,
            'generation_id': self.generation_id,
            'controller_manager': self.controller_manager,
            'activity_topic': self.controller_manager_activity_topic,
            'hardware_component_name': self.hardware_component_name,
            'joint_state_broadcaster_manager_name': (
                self.joint_state_broadcaster_manager_name
            ),
            'trajectory_controller_manager_name': (
                self.trajectory_controller_manager_name
            ),
            'trajectory_controller_ros_name': (
                self.trajectory_controller_name
            ),
            'trajectory_controller_type': self.trajectory_controller_type,
            'follow_joint_trajectory_action': (
                self.follow_joint_trajectory_action
            ),
            'controller_state_topic': self.controller_state_topic,
            'joint_state_topic': self.joint_state_topic,
            'required_joints': list(self.arm_joint_names),
            'claimed_command_interfaces': list(
                self.claimed_command_interface_names
            ),
            'required_state_interfaces': list(self.state_interface_names),
            'snapshot_max_age_ms': self.binding_snapshot_max_age_ms,
        }


def compile_production_controller_binding(
    *,
    resolved: ResolvedLite6Binding,
    supervised_runtime_urdf: str,
    ros2_controllers_yaml: str,
    binding_snapshot_max_age_ms: int = 1000,
) -> ProductionControllerBinding:
    """Compile production-shaped facts offline and reject artifact drift."""
    if (
        not isinstance(binding_snapshot_max_age_ms, int)
        or isinstance(binding_snapshot_max_age_ms, bool)
        or binding_snapshot_max_age_ms <= 0
    ):
        raise ValueError('binding_snapshot_max_age_ms must be positive')

    root = ET.fromstring(supervised_runtime_urdf)
    controls = root.findall('./ros2_control')
    if len(controls) != 1:
        raise ValueError(
            'supervised runtime URDF must contain exactly one ros2_control block'
        )
    control = controls[0]
    component_name = control.get('name', '')
    if not component_name or control.get('type') != 'system':
        raise ValueError(
            'supervised ros2_control component must be a named system'
        )
    expected_component_name = f'{resolved.prefix}lite6_system'
    if component_name != expected_component_name:
        raise ValueError(
            'supervised hardware component name disagrees with the binding'
        )
    hardware = control.find('./hardware')
    if hardware is None:
        raise ValueError('supervised ros2_control hardware block is missing')
    plugin = hardware.findtext('./plugin', default='')
    if plugin != SUPERVISED_HARDWARE_PLUGIN:
        raise ValueError(
            f'unexpected supervised hardware plugin: {plugin!r}'
        )
    parameters = _hardware_parameters(hardware)
    if 'backend' in parameters:
        raise ValueError(
            'production controller binding must not use a ManyForge backend'
        )
    if parameters.get('access_mode') != 'supervised_lifecycle':
        raise ValueError(
            'production hardware must use supervised_lifecycle access'
        )
    if not parameters.get('transport_owner_id'):
        raise ValueError(
            'production hardware must declare one transport owner'
        )
    expected_activity_topic = f'{resolved.controller_manager}/activity'

    hardware_joint_names: list[str] = []
    hardware_command_interfaces: tuple[str, ...] | None = None
    hardware_state_interfaces: tuple[str, ...] | None = None
    for joint in control.findall('./joint'):
        name = joint.get('name', '')
        if not name:
            raise ValueError('ros2_control joint has no name')
        commands = tuple(
            item.get('name', '') for item in joint.findall('./command_interface')
        )
        states = tuple(
            item.get('name', '') for item in joint.findall('./state_interface')
        )
        if commands != EXPECTED_COMMAND_INTERFACES:
            raise ValueError(
                f'{name} command interfaces are not '
                f'{EXPECTED_COMMAND_INTERFACES!r}'
            )
        if states != EXPECTED_STATE_INTERFACES:
            raise ValueError(
                f'{name} state interfaces are not '
                f'{EXPECTED_STATE_INTERFACES!r}'
            )
        if hardware_command_interfaces is None:
            hardware_command_interfaces = commands
            hardware_state_interfaces = states
        hardware_joint_names.append(name)
    if tuple(hardware_joint_names) != resolved.arm_joint_names:
        raise ValueError(
            'supervised URDF joint order disagrees with the resolved binding'
        )

    controllers_payload = yaml.safe_load(ros2_controllers_yaml)
    if not isinstance(controllers_payload, dict):
        raise ValueError('controller YAML root must be a mapping')
    controllers_payload = _namespace_controller_parameters(
        controllers_payload,
        resolved.ros_namespace,
    )
    manager = controllers_payload.get('controller_manager')
    if not isinstance(manager, dict):
        raise ValueError('controller YAML has no controller_manager mapping')
    manager_parameters = manager.get('ros__parameters')
    if not isinstance(manager_parameters, dict):
        raise ValueError(
            'controller_manager.ros__parameters must be a mapping'
        )
    update_rate = _positive_number(
        manager_parameters.get('update_rate'),
        'controller_manager.update_rate',
    )
    controller_leaf = resolved.trajectory_controller.rsplit('/', 1)[-1]
    joint_state_broadcasters = tuple(
        name
        for name, value in manager_parameters.items()
        if (
            isinstance(value, dict)
            and value.get('type') == JOINT_STATE_BROADCASTER_TYPE
        )
    )
    if len(joint_state_broadcasters) != 1:
        raise ValueError(
            'controller manager must declare exactly one joint-state '
            'broadcaster'
        )
    joint_state_broadcaster = joint_state_broadcasters[0]
    declaration = manager_parameters.get(controller_leaf)
    if not isinstance(declaration, dict):
        raise ValueError(
            f'controller manager does not declare {controller_leaf}'
        )
    controller_type = declaration.get('type')
    if controller_type != TRAJECTORY_CONTROLLER_TYPE:
        raise ValueError(
            f'unexpected trajectory controller type: {controller_type!r}'
        )
    controller = controllers_payload.get(controller_leaf)
    if not isinstance(controller, dict):
        raise ValueError(
            f'controller YAML has no {controller_leaf} parameter mapping'
        )
    controller_parameters = controller.get('ros__parameters')
    if not isinstance(controller_parameters, dict):
        raise ValueError(
            f'{controller_leaf}.ros__parameters must be a mapping'
        )
    controller_joints = _string_list(
        controller_parameters.get('joints'),
        f'{controller_leaf}.joints',
    )
    if controller_joints != resolved.arm_joint_names:
        raise ValueError(
            'trajectory controller joint order disagrees with the binding'
        )
    command_interfaces = _string_list(
        controller_parameters.get('command_interfaces'),
        f'{controller_leaf}.command_interfaces',
    )
    if command_interfaces != EXPECTED_COMMAND_INTERFACES:
        raise ValueError(
            'trajectory controller command interfaces disagree with hardware'
        )
    state_interfaces = _string_list(
        controller_parameters.get('state_interfaces'),
        f'{controller_leaf}.state_interfaces',
    )
    if state_interfaces != EXPECTED_STATE_INTERFACES:
        raise ValueError(
            'trajectory controller state interfaces disagree with hardware'
        )
    state_publish_rate = _positive_number(
        controller_parameters.get('state_publish_rate'),
        f'{controller_leaf}.state_publish_rate',
    )
    action_monitor_rate = _positive_number(
        controller_parameters.get('action_monitor_rate'),
        f'{controller_leaf}.action_monitor_rate',
    )
    constraints = controller_parameters.get('constraints')
    if not isinstance(constraints, dict):
        raise ValueError(
            f'{controller_leaf}.constraints must be a mapping'
        )
    decelerate_on_cancel = constraints.get('decelerate_on_cancel')
    if decelerate_on_cancel is not True:
        raise ValueError(
            f'{controller_leaf}.constraints.decelerate_on_cancel '
            'must be true'
        )
    max_deceleration_on_cancel_rad_s2: dict[str, float] = {}
    for joint_name in controller_joints:
        joint_constraints = constraints.get(joint_name)
        if not isinstance(joint_constraints, dict):
            raise ValueError(
                f'{controller_leaf}.constraints.{joint_name} '
                'must be a mapping'
            )
        max_deceleration_on_cancel_rad_s2[joint_name] = _positive_number(
            joint_constraints.get('max_deceleration_on_cancel'),
            (
                f'{controller_leaf}.constraints.{joint_name}.'
                'max_deceleration_on_cancel'
            ),
        )

    runtime_digest = _sha256_text(supervised_runtime_urdf)
    controller_digest = _sha256_text(ros2_controllers_yaml)
    claimed_command_interface_names = tuple(
        f'{joint}/{interface}'
        for joint in controller_joints
        for interface in command_interfaces
    )
    state_interface_names = tuple(
        f'{joint}/{interface}'
        for joint in controller_joints
        for interface in state_interfaces
    )
    joint_state_topic = _joint_state_broadcaster_topic(
        resolved.ros_namespace
    )
    identity = {
        'resolved_binding_generation_id': resolved.generation_id,
        'ros_namespace': resolved.ros_namespace,
        'model_prefix': resolved.prefix,
        'hardware_namespace': resolved.hardware_namespace,
        'controller_manager': resolved.controller_manager,
        'controller_manager_activity_topic': expected_activity_topic,
        'controller_manager_update_rate_hz': update_rate,
        'controller_manager_activity_qos': ACTIVITY_QOS,
        'binding_snapshot_max_age_ms': binding_snapshot_max_age_ms,
        'hardware_component_name': component_name,
        'hardware_plugin': plugin,
        'joint_state_broadcaster_manager_name': joint_state_broadcaster,
        'trajectory_controller_manager_name': controller_leaf,
        'trajectory_controller_name': resolved.trajectory_controller,
        'trajectory_controller_type': controller_type,
        'follow_joint_trajectory_action': (
            resolved.follow_joint_trajectory_action
        ),
        'supervised_state_topic': resolved.supervised_state_topic,
        'supervised_execute_service': resolved.supervised_execute_service,
        'supervised_command_gate_service': (
            resolved.supervised_command_gate_service
        ),
        'supervised_shutdown_controller_service': (
            resolved.supervised_shutdown_controller_service
        ),
        'controller_state_topic': (
            f'{resolved.trajectory_controller}/controller_state'
        ),
        'joint_state_topic': joint_state_topic,
        'validated_joint_state_topic': resolved.validated_joint_state_topic,
        'arm_joint_names': controller_joints,
        'hardware_command_interfaces': hardware_command_interfaces,
        'hardware_state_interfaces': hardware_state_interfaces,
        'controller_command_interfaces': command_interfaces,
        'controller_state_interfaces': state_interfaces,
        'decelerate_on_cancel': decelerate_on_cancel,
        'max_deceleration_on_cancel_rad_s2': (
            max_deceleration_on_cancel_rad_s2
        ),
        'claimed_command_interface_names': (
            claimed_command_interface_names
        ),
        'state_interface_names': state_interface_names,
        'state_publish_rate_hz': state_publish_rate,
        'action_monitor_rate_hz': action_monitor_rate,
        'runtime_urdf_sha256': runtime_digest,
        'controllers_yaml_sha256': controller_digest,
    }
    generation_id = hashlib.sha256(
        json.dumps(identity, sort_keys=True, separators=(',', ':')).encode(
            'utf-8'
        )
    ).hexdigest()
    return ProductionControllerBinding(
        schema=CONTROLLER_BINDING_SCHEMA,
        generation_id=generation_id,
        ros_namespace=resolved.ros_namespace,
        model_prefix=resolved.prefix,
        hardware_namespace=resolved.hardware_namespace,
        controller_manager=resolved.controller_manager,
        controller_manager_activity_topic=expected_activity_topic,
        controller_manager_update_rate_hz=update_rate,
        controller_manager_activity_qos=dict(ACTIVITY_QOS),
        binding_snapshot_max_age_ms=binding_snapshot_max_age_ms,
        hardware_component_name=component_name,
        hardware_plugin=plugin,
        joint_state_broadcaster_manager_name=joint_state_broadcaster,
        trajectory_controller_manager_name=controller_leaf,
        trajectory_controller_name=resolved.trajectory_controller,
        trajectory_controller_type=controller_type,
        follow_joint_trajectory_action=(
            resolved.follow_joint_trajectory_action
        ),
        supervised_state_topic=resolved.supervised_state_topic,
        supervised_execute_service=resolved.supervised_execute_service,
        supervised_command_gate_service=(
            resolved.supervised_command_gate_service
        ),
        supervised_shutdown_controller_service=(
            resolved.supervised_shutdown_controller_service
        ),
        controller_state_topic=(
            f'{resolved.trajectory_controller}/controller_state'
        ),
        joint_state_topic=joint_state_topic,
        validated_joint_state_topic=resolved.validated_joint_state_topic,
        arm_joint_names=controller_joints,
        hardware_command_interfaces=hardware_command_interfaces
        or EXPECTED_COMMAND_INTERFACES,
        hardware_state_interfaces=hardware_state_interfaces
        or EXPECTED_STATE_INTERFACES,
        controller_command_interfaces=command_interfaces,
        controller_state_interfaces=state_interfaces,
        decelerate_on_cancel=decelerate_on_cancel,
        max_deceleration_on_cancel_rad_s2=(
            max_deceleration_on_cancel_rad_s2
        ),
        claimed_command_interface_names=(
            claimed_command_interface_names
        ),
        state_interface_names=state_interface_names,
        state_publish_rate_hz=state_publish_rate,
        action_monitor_rate_hz=action_monitor_rate,
        runtime_urdf_sha256=runtime_digest,
        controllers_yaml_sha256=controller_digest,
    )
