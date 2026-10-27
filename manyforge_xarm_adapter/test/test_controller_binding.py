from __future__ import annotations

from pathlib import Path
import xml.etree.ElementTree as ET

from manyforge_xarm_adapter.binding import compile_lite6_binding
from manyforge_xarm_adapter.binding import Lite6BindingInputs
from manyforge_xarm_adapter.controller_binding import (
    compile_production_controller_binding,
)
from manyforge_xarm_adapter.controller_binding import (
    SUPERVISED_HARDWARE_PLUGIN,
)
from manyforge_xarm_adapter.model_generation import supervised_runtime_urdf
from manyforge_xarm_adapter.profile import load_profile
import pytest
import yaml


_PROFILE = Path(__file__).resolve().parents[1] / 'config' / (
    'lite6_fixed_pneumatic.profile.yaml'
)


def _runtime_urdf() -> str:
    joints = '\n'.join(
        f"""\
    <joint name="joint{index}">
      <command_interface name="position"/>
      <command_interface name="velocity"/>
      <state_interface name="position"/>
      <state_interface name="velocity"/>
    </joint>"""
        for index in range(1, 7)
    )
    return f"""\
<robot name="lite6">
  <link name="world"/>
  <ros2_control name="vendor" type="system">
    <hardware>
      <plugin>uf_robot_hardware/UFRobotSystemHardware</plugin>
      <param name="robot_ip">__MANYFORGE_RUNTIME_ROBOT_IP__</param>
    </hardware>
{joints}
  </ros2_control>
</robot>
"""


def _controllers() -> dict[str, object]:
    joints = [f'joint{index}' for index in range(1, 7)]
    return {
        'controller_manager': {
            'ros__parameters': {
                'update_rate': 150,
                'joint_state_broadcaster': {
                    'type': (
                        'joint_state_broadcaster/JointStateBroadcaster'
                    )
                },
                'lite6_traj_controller': {
                    'type': (
                        'joint_trajectory_controller/'
                        'JointTrajectoryController'
                    )
                },
            }
        },
        'lite6_traj_controller': {
            'ros__parameters': {
                'command_interfaces': ['position'],
                'state_interfaces': ['position', 'velocity'],
                'joints': joints,
                'state_publish_rate': 25.0,
                'action_monitor_rate': 10.0,
                'constraints': {
                    'decelerate_on_cancel': True,
                    **{
                        joint: {
                            'max_deceleration_on_cancel': 2.0,
                        }
                        for joint in joints
                    },
                },
            }
        },
    }


def _compile(
    controller_payload: dict[str, object] | None = None,
    *,
    test_only_fake: bool = False,
):
    profile = load_profile(_PROFILE)
    runtime = supervised_runtime_urdf(
        _runtime_urdf(),
        profile,
        test_only_fake=test_only_fake,
    )
    return compile_production_controller_binding(
        resolved=compile_lite6_binding(Lite6BindingInputs()),
        supervised_runtime_urdf=runtime,
        ros2_controllers_yaml=yaml.safe_dump(
            controller_payload or _controllers(),
            sort_keys=False,
        ),
    )


def test_compiles_exact_generated_production_binding() -> None:
    binding = _compile()

    assert binding.controller_manager == '/controller_manager'
    assert (
        binding.controller_manager_activity_topic
        == '/controller_manager/activity'
    )
    assert binding.controller_manager_update_rate_hz == 150
    assert binding.controller_manager_activity_qos == {
        'depth': 1,
        'durability': 'transient_local',
        'reliability': 'reliable',
    }
    assert binding.binding_snapshot_max_age_ms == 1000
    assert binding.ros_namespace == ''
    assert binding.model_prefix == ''
    assert binding.hardware_namespace == 'ufactory'
    assert binding.hardware_component_name == 'lite6_system'
    assert binding.hardware_plugin == SUPERVISED_HARDWARE_PLUGIN
    assert (
        binding.trajectory_controller_manager_name
        == 'lite6_traj_controller'
    )
    assert not any('supervised' in key for key in binding.to_dict())
    assert (
        binding.trajectory_controller_type
        == 'joint_trajectory_controller/JointTrajectoryController'
    )
    assert binding.arm_joint_names == tuple(
        f'joint{index}' for index in range(1, 7)
    )
    execution_binding = binding.to_ros2_control_execution_binding()
    assert execution_binding['trajectory_controller_manager_name'] == (
        'lite6_traj_controller'
    )
    assert execution_binding['trajectory_controller_ros_name'] == (
        '/lite6_traj_controller'
    )
    assert binding.joint_state_topic == '/joint_states'
    assert execution_binding['joint_state_topic'] == '/joint_states'
    assert execution_binding['required_joints'] == list(
        binding.arm_joint_names
    )
    assert execution_binding['snapshot_max_age_ms'] == 1000
    assert binding.hardware_command_interfaces == ('position', 'velocity')
    assert binding.controller_command_interfaces == ('position',)
    assert binding.hardware_state_interfaces == ('position', 'velocity')
    assert binding.controller_state_interfaces == ('position', 'velocity')
    assert binding.decelerate_on_cancel is True
    assert binding.max_deceleration_on_cancel_rad_s2 == {
        f'joint{index}': 2.0 for index in range(1, 7)
    }
    assert binding.claimed_command_interface_names == tuple(
        f'joint{index}/position'
        for index in range(1, 7)
    )
    assert binding.state_interface_names == tuple(
        f'joint{index}/{interface}'
        for index in range(1, 7)
        for interface in ('position', 'velocity')
    )


def test_runtime_carries_only_the_unmodified_vendor_plugin_parameters() -> None:
    profile = load_profile(_PROFILE)
    runtime = supervised_runtime_urdf(
        _runtime_urdf(),
        profile,
        test_only_fake=False,
    )
    root = ET.fromstring(runtime)
    control = root.find('./ros2_control')
    assert control is not None
    assert control.get('name') == 'lite6_system'
    assert (
        control.findtext('./hardware/plugin')
        == SUPERVISED_HARDWARE_PLUGIN
    )
    parameters = {
        item.get('name'): item.text
        for item in control.findall('./hardware/param')
    }
    # R7 package 5: no supervised access mode, lease or owner parameter.
    assert set(parameters) == {
        'robot_ip',
        'report_type',
        'dof',
        'robot_type',
        'prefix',
        'hw_ns',
        'velocity_control',
        'add_gripper',
        'add_bio_gripper',
    }
    assert parameters['robot_ip'] == '__MANYFORGE_RUNTIME_ROBOT_IP__'
    assert parameters['velocity_control'] == 'false'



def test_binding_rejects_fake_qualification_urdf_as_production() -> None:
    with pytest.raises(ValueError, match='hardware plugin'):
        _compile(test_only_fake=True)


def test_binding_rejects_hardware_component_name_drift() -> None:
    profile = load_profile(_PROFILE)
    runtime = supervised_runtime_urdf(
        _runtime_urdf(),
        profile,
        test_only_fake=False,
    ).replace('name="lite6_system"', 'name="other_system"', 1)

    with pytest.raises(ValueError, match='component name'):
        compile_production_controller_binding(
            resolved=compile_lite6_binding(Lite6BindingInputs()),
            supervised_runtime_urdf=runtime,
            ros2_controllers_yaml=yaml.safe_dump(_controllers()),
        )


def test_binding_rejects_reordered_controller_joints() -> None:
    payload = _controllers()
    parameters = payload['lite6_traj_controller']['ros__parameters']
    parameters['joints'] = list(reversed(parameters['joints']))

    with pytest.raises(ValueError, match='joint order'):
        _compile(payload)


def test_binding_rejects_interface_or_type_drift() -> None:
    wrong_interfaces = _controllers()
    wrong_interfaces['lite6_traj_controller']['ros__parameters'][
        'command_interfaces'
    ] = ['position', 'velocity']
    with pytest.raises(ValueError, match='command interfaces'):
        _compile(wrong_interfaces)

    wrong_type = _controllers()
    wrong_type['controller_manager']['ros__parameters'][
        'lite6_traj_controller'
    ]['type'] = 'forward_command_controller/ForwardCommandController'
    with pytest.raises(ValueError, match='controller type'):
        _compile(wrong_type)


def test_binding_rejects_unqualified_cancel_deceleration() -> None:
    disabled = _controllers()
    disabled['lite6_traj_controller']['ros__parameters']['constraints'][
        'decelerate_on_cancel'
    ] = False
    with pytest.raises(ValueError, match='decelerate_on_cancel.*true'):
        _compile(disabled)

    missing_limit = _controllers()
    del missing_limit['lite6_traj_controller']['ros__parameters'][
        'constraints'
    ]['joint6']['max_deceleration_on_cancel']
    with pytest.raises(
        ValueError,
        match='joint6.max_deceleration_on_cancel.*positive',
    ):
        _compile(missing_limit)
