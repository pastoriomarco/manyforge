from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import subprocess

from launch import LaunchContext
from launch.utilities import perform_substitutions
import pytest
import yaml


_LAUNCH_FILE = (
    Path(__file__).resolve().parents[1]
    / 'launch'
    / 'lite6_real_read_only.launch.py'
)
_LAUNCH_SCRIPT = (
    Path(__file__).resolve().parents[2]
    / 'scripts'
    / 'ros'
    / 'launch-lite6-read-only.sh'
)


def _load_launch_module():
    spec = importlib.util.spec_from_file_location(
        'manyforge_lite6_real_read_only_launch_test',
        _LAUNCH_FILE,
    )
    assert spec is not None
    assert spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _model(tmp_path: Path) -> Path:
    path = tmp_path / 'lite6.urdf'
    path.write_text(
        """\
<robot name="lite6">
  <link name="world"/>
</robot>
""",
        encoding='utf-8',
    )
    return path


def _context(
    model: Path,
    *,
    connect: str = 'false',
    robot_ip: str = '',
    report_type: str = 'rich',
):
    context = LaunchContext()
    values = {
        'connect_to_robot': connect,
        'robot_ip': robot_ip,
        'report_type': report_type,
        'ros_namespace': '',
        'prefix': '',
        'hardware_namespace': 'ufactory',
        'controller_name': 'lite6_traj_controller',
        'model_urdf_path': str(model),
    }
    for name, value in values.items():
        context.launch_configurations[name] = value
    return context


def _resolved_text(value, context: LaunchContext) -> str:
    if isinstance(value, str):
        return value
    return ''.join(item.perform(context) for item in value)


def test_default_launch_has_observer_and_model_but_no_driver(tmp_path: Path) -> None:
    module = _load_launch_module()
    context = _context(_model(tmp_path))

    nodes = module._launch_setup(context)
    packages = [_resolved_text(node.node_package, context) for node in nodes]
    executables = [_resolved_text(node.node_executable, context) for node in nodes]

    assert packages == ['robot_state_publisher', 'manyforge_xarm_adapter']
    assert executables == [
        'robot_state_publisher',
        'manyforge-xarm-read-only-observer',
    ]
    assert 'controller_manager' not in packages


def test_explicit_connection_adds_only_enforced_read_only_driver(
    tmp_path: Path,
) -> None:
    module = _load_launch_module()
    context = _context(
        _model(tmp_path),
        connect='true',
        robot_ip='192.0.2.10',
    )

    nodes = module._launch_setup(context)
    packages = [_resolved_text(node.node_package, context) for node in nodes]

    assert packages == [
        'xarm_api',
        'robot_state_publisher',
        'manyforge_xarm_adapter',
    ]
    raw_parameters = nodes[0]._Node__parameters
    assert len(raw_parameters) == 1
    driver_parameters = {
        perform_substitutions(context, list(key)): value
        for key, value in raw_parameters[0].items()
    }
    assert driver_parameters['read_only'] is True
    assert driver_parameters['add_gripper'] is False
    assert driver_parameters['add_bio_gripper'] is False
    assert driver_parameters['add_vacuum_gripper'] is False
    assert 'expected_robot_sn' not in driver_parameters
    assert driver_parameters['expected_robot_device_type'] == 9

    observer_parameters = nodes[2]._Node__parameters
    assert len(observer_parameters) == 1
    observer_values = {
        perform_substitutions(context, list(key)): value
        for key, value in observer_parameters[0].items()
    }
    assert (
        yaml.safe_load(
            _resolved_text(
                observer_values['validated_joint_state_topic'],
                context,
            )
        )
        == '/manyforge/xarm/validated_joint_states'
    )


@pytest.mark.parametrize('address', ['', 'lite6.local', '::1'])
def test_connection_rejects_missing_or_non_ipv4_address(
    tmp_path: Path,
    address: str,
) -> None:
    module = _load_launch_module()

    with pytest.raises(ValueError):
        module._launch_setup(
            _context(_model(tmp_path), connect='true', robot_ip=address)
        )


def test_launch_rejects_non_rich_report_stream(tmp_path: Path) -> None:
    module = _load_launch_module()

    with pytest.raises(ValueError, match="report_type must be 'rich'"):
        module._launch_setup(
            _context(_model(tmp_path), report_type='normal')
        )


def test_shell_gate_requires_runtime_address_before_build() -> None:
    environment = os.environ.copy()
    environment.update(
        {
            'MANYFORGE_LITE6_CONNECT_READ_ONLY': 'ON',
            'MANYFORGE_LITE6_READ_ONLY_CONFIRM': 'CONNECT_READ_ONLY',
        }
    )
    environment.pop('MANYFORGE_LITE6_ROBOT_IP', None)

    result = subprocess.run(
        [str(_LAUNCH_SCRIPT)],
        cwd=_LAUNCH_SCRIPT.parents[2],
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )

    assert result.returncode == 2
    assert 'MANYFORGE_LITE6_ROBOT_IP is required' in result.stderr
    assert 'Starting >>>' not in result.stdout


def test_launch_and_shell_have_no_robot_serial_contract() -> None:
    launch_text = _LAUNCH_FILE.read_text(encoding='utf-8')
    shell_text = _LAUNCH_SCRIPT.read_text(encoding='utf-8')

    assert 'expected_robot_sn' not in launch_text
    assert 'MANYFORGE_LITE6_EXPECTED_ROBOT_SN' not in shell_text
