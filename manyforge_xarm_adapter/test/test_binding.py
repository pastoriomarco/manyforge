from __future__ import annotations

from manyforge_xarm_adapter.binding import compile_lite6_binding
from manyforge_xarm_adapter.binding import Lite6BindingInputs
import pytest


def test_default_binding_matches_xarm_single_robot_convention() -> None:
    binding = compile_lite6_binding(Lite6BindingInputs())

    assert binding.driver_node == '/ufactory_driver'
    assert binding.hardware_namespace == 'ufactory'
    assert binding.joint_state_topic == '/ufactory/joint_states'
    assert (
        binding.validated_joint_state_topic
        == '/manyforge/xarm/validated_joint_states'
    )
    assert binding.robot_state_topic == '/ufactory/robot_states'
    assert binding.diagnostics_topic == '/manyforge/xarm/diagnostics'
    assert binding.controller_io_state_topic == '/ufactory/xarm_cgpio_states'
    assert binding.controller_manager == '/controller_manager'
    assert binding.trajectory_controller == '/lite6_traj_controller'
    assert (
        binding.follow_joint_trajectory_action
        == '/lite6_traj_controller/follow_joint_trajectory'
    )
    # R7 package 5: the supervised mode is gone; no supervised endpoint.
    assert not any('supervised' in key for key in binding.to_dict())
    assert binding.arm_joint_names == tuple(f'joint{i}' for i in range(1, 7))


def test_prefixed_namespaced_binding_is_deterministic_and_isolated() -> None:
    inputs = Lite6BindingInputs(
        ros_namespace='/cell/a',
        prefix='L_',
        hardware_namespace='ufactory',
        controller_name='lite6_traj_controller',
    )

    first = compile_lite6_binding(inputs)
    second = compile_lite6_binding(inputs)

    assert first == second
    assert first.hardware_namespace == 'L_ufactory'
    assert first.driver_node == '/cell/a/ufactory_driver'
    assert first.joint_state_topic == '/cell/a/L_ufactory/joint_states'
    assert (
        first.validated_joint_state_topic
        == '/cell/a/manyforge/xarm/validated_joint_states'
    )
    assert first.diagnostics_topic == '/cell/a/manyforge/xarm/diagnostics'
    assert first.trajectory_controller == '/cell/a/L_lite6_traj_controller'
    assert first.arm_joint_names == tuple(f'L_joint{i}' for i in range(1, 7))


@pytest.mark.parametrize(
    ('field', 'value'),
    [
        ('ros_namespace', 'cell//robot'),
        ('prefix', 'bad/prefix'),
        ('hardware_namespace', '1factory'),
        ('controller_name', 'controller-name'),
    ],
)
def test_binding_rejects_invalid_ros_names(field: str, value: str) -> None:
    values = {
        'ros_namespace': '',
        'prefix': '',
        'hardware_namespace': 'ufactory',
        'controller_name': 'lite6_traj_controller',
    }
    values[field] = value

    with pytest.raises(ValueError):
        compile_lite6_binding(Lite6BindingInputs(**values))
