from __future__ import annotations

from datetime import datetime
from datetime import timezone
import json
from pathlib import Path
from typing import Sequence

from manyforge_xarm_adapter.binding_manifest import BINDING_SCHEMA
from manyforge_xarm_adapter.live_evidence import capture_live_evidence
from manyforge_xarm_adapter.live_evidence import CommandOutcome


def _manifest(path: Path) -> Path:
    payload = {
        'schema': BINDING_SCHEMA,
        'generation_id': 'a' * 64,
        'binding': {
            'ros_namespace': '',
            'driver_node': '/ufactory_driver',
            'diagnostics_topic': '/manyforge/xarm/diagnostics',
            'robot_state_topic': '/ufactory/robot_states',
            'joint_state_topic': '/ufactory/joint_states',
            'validated_joint_state_topic': (
                '/manyforge/xarm/validated_joint_states'
            ),
        },
        'controller_binding': {
            'generation_id': 'b' * 64,
            'controller_manager': '/controller_manager',
            'controller_manager_activity_topic': (
                '/controller_manager/activity'
            ),
            'hardware_component_name': 'lite6_system',
            'trajectory_controller_name': '/lite6_traj_controller',
            'arm_joint_names': [
                f'joint{index}' for index in range(1, 7)
            ],
            'trajectory_controller_type': (
                'joint_trajectory_controller/JointTrajectoryController'
            ),
            'controller_command_interfaces': ['position', 'velocity'],
            'controller_state_interfaces': ['position', 'velocity'],
            'claimed_command_interface_names': [
                f'joint{index}/{interface}'
                for index in range(1, 7)
                for interface in ('position', 'velocity')
            ],
            'state_interface_names': [
                f'joint{index}/{interface}'
                for index in range(1, 7)
                for interface in ('position', 'velocity')
            ],
        },
    }
    path.write_text(json.dumps(payload), encoding='utf-8')
    return path


class FakeRunner:
    def __init__(self, *, fail_nodes: bool = False) -> None:
        self.fail_nodes = fail_nodes
        self.calls: list[tuple[tuple[str, ...], float]] = []

    def __call__(
        self,
        argv: Sequence[str],
        timeout_s: float,
    ) -> CommandOutcome:
        command = tuple(argv)
        self.calls.append((command, timeout_s))
        if command[:3] == ('ros2', 'node', 'list'):
            if self.fail_nodes:
                return CommandOutcome(124, b'', b'', True, 5000.0)
            output = (
                b'/ufactory_driver\n'
                b'/lite6_robot_state_publisher\n'
                b'/manyforge_xarm_read_only_observer\n'
            )
        elif command[:3] == ('ros2', 'action', 'list'):
            output = b''
        elif command[:3] == ('ros2', 'service', 'list'):
            output = b'/ufactory_driver/get_parameters\n'
        elif command[-1] == '/manyforge/xarm/diagnostics':
            output = (
                b'key: access_mode\nvalue: read_only\n'
                b'key: motion_admission\nvalue: closed\n'
                b'key: command_endpoints\nvalue: suppressed\n'
            )
        elif command[-1] == '/manyforge/xarm/validated_joint_states':
            return CommandOutcome(124, b'', b'', True, 5000.0)
        elif command[-1] in (
            '/ufactory/robot_states',
            '/ufactory/joint_states',
        ):
            output = b'header:\n  stamp:\n    sec: 1\n'
        elif 'list_hardware_components' in command:
            output = (
                b'Hardware Component 1\n'
                b'  name: lite6_system\n'
                b'  state: id=3 label=active\n'
            )
        elif 'list_hardware_interfaces' in command:
            command_lines = [
                f'{joint}/{interface} [claimed]'
                for joint in (
                    f'joint{index}' for index in range(1, 7)
                )
                for interface in ('position', 'velocity')
            ]
            state_lines = [
                f'{joint}/{interface}'
                for joint in (
                    f'joint{index}' for index in range(1, 7)
                )
                for interface in ('position', 'velocity')
            ]
            output = (
                'command interfaces\n'
                + '\n'.join(command_lines)
                + '\nstate interfaces\n'
                + '\n'.join(state_lines)
            ).encode()
        elif 'list_controllers' in command:
            output = (
                b'lite6_traj_controller '
                b'joint_trajectory_controller/JointTrajectoryController '
                b'active\n'
            )
        elif command[-1] == '/controller_manager/activity':
            output = b'name: lite6_traj_controller\nstate: active\n'
        else:
            raise AssertionError(f'unexpected probe: {command}')
        return CommandOutcome(0, output, b'', False, 0.25)


def test_read_only_capture_records_optional_timeout_without_passing_it(
    tmp_path: Path,
) -> None:
    runner = FakeRunner()
    output = tmp_path / 'evidence'

    summary = capture_live_evidence(
        mode='read_only',
        binding_manifest_path=_manifest(tmp_path / 'binding.json'),
        output_root=output,
        runner=runner,
        timeout_s=5.0,
        captured_at=datetime(2026, 7, 24, tzinfo=timezone.utc),
    )

    assert summary['result'] == 'pass'
    assert summary['required_failure_count'] == 0
    assert summary['optional_unavailable_count'] == 1
    optional = summary['events'][-1]
    assert optional['label'] == 'validated_joint_state'
    assert optional['timed_out'] is True
    assert optional['timeout_is_success'] is False
    assert optional['postcondition_passed'] is False
    assert json.loads((output / 'evidence.json').read_text())['result'] == (
        'pass'
    )
    assert len((output / 'events.jsonl').read_text().splitlines()) == 7


def test_required_probe_timeout_is_a_failed_evidence_gate(
    tmp_path: Path,
) -> None:
    summary = capture_live_evidence(
        mode='read_only',
        binding_manifest_path=_manifest(tmp_path / 'binding.json'),
        output_root=tmp_path / 'evidence',
        runner=FakeRunner(fail_nodes=True),
    )

    assert summary['result'] == 'fail'
    assert summary['required_failure_count'] == 1


def test_supervised_preflight_uses_manifest_selected_binding(
    tmp_path: Path,
) -> None:
    runner = FakeRunner()
    summary = capture_live_evidence(
        mode='supervised_preflight',
        binding_manifest_path=_manifest(tmp_path / 'binding.json'),
        output_root=tmp_path / 'evidence',
        runner=runner,
    )

    assert summary['result'] == 'pass'
    assert summary['required_failure_count'] == 0
    commands = [call[0] for call in runner.calls]
    assert any('/controller_manager' in command for command in commands)


def test_read_only_capture_rejects_an_unexpected_graph_participant(
    tmp_path: Path,
) -> None:
    class UnexpectedNodeRunner(FakeRunner):
        def __call__(
            self,
            argv: Sequence[str],
            timeout_s: float,
        ) -> CommandOutcome:
            result = super().__call__(argv, timeout_s)
            if tuple(argv[:3]) == ('ros2', 'node', 'list'):
                return CommandOutcome(
                    result.return_code,
                    result.stdout + b'/unexpected_command_owner\n',
                    result.stderr,
                    result.timed_out,
                    result.duration_ms,
                )
            return result

    summary = capture_live_evidence(
        mode='read_only',
        binding_manifest_path=_manifest(tmp_path / 'binding.json'),
        output_root=tmp_path / 'evidence',
        runner=UnexpectedNodeRunner(),
    )

    assert summary['result'] == 'fail'
    assert summary['required_failure_count'] == 1


def test_supervised_preflight_requires_claimed_interfaces(
    tmp_path: Path,
) -> None:
    class UnclaimedInterfaceRunner(FakeRunner):
        def __call__(
            self,
            argv: Sequence[str],
            timeout_s: float,
        ) -> CommandOutcome:
            result = super().__call__(argv, timeout_s)
            if 'list_hardware_interfaces' in tuple(argv):
                return CommandOutcome(
                    result.return_code,
                    result.stdout.replace(b' [claimed]', b''),
                    result.stderr,
                    result.timed_out,
                    result.duration_ms,
                )
            return result

    summary = capture_live_evidence(
        mode='supervised_preflight',
        binding_manifest_path=_manifest(tmp_path / 'binding.json'),
        output_root=tmp_path / 'evidence',
        runner=UnclaimedInterfaceRunner(),
    )

    assert summary['result'] == 'fail'
    assert summary['required_failure_count'] == 1


def test_read_only_graph_expectations_follow_the_resolved_namespace(
    tmp_path: Path,
) -> None:
    manifest_path = _manifest(tmp_path / 'binding.json')
    manifest = json.loads(manifest_path.read_text())
    manifest['binding']['ros_namespace'] = 'cell/a'
    manifest['binding']['driver_node'] = '/cell/a/ufactory_driver'
    manifest_path.write_text(json.dumps(manifest), encoding='utf-8')

    class NamespacedRunner(FakeRunner):
        def __call__(
            self,
            argv: Sequence[str],
            timeout_s: float,
        ) -> CommandOutcome:
            if tuple(argv[:3]) == ('ros2', 'node', 'list'):
                self.calls.append((tuple(argv), timeout_s))
                return CommandOutcome(
                    0,
                    (
                        b'/cell/a/ufactory_driver\n'
                        b'/cell/a/lite6_robot_state_publisher\n'
                        b'/cell/a/manyforge_xarm_read_only_observer\n'
                    ),
                    b'',
                    False,
                    0.25,
                )
            if tuple(argv[:3]) == ('ros2', 'service', 'list'):
                self.calls.append((tuple(argv), timeout_s))
                return CommandOutcome(
                    0,
                    b'/cell/a/ufactory_driver/get_parameters\n',
                    b'',
                    False,
                    0.25,
                )
            return super().__call__(argv, timeout_s)

    summary = capture_live_evidence(
        mode='read_only',
        binding_manifest_path=manifest_path,
        output_root=tmp_path / 'evidence',
        runner=NamespacedRunner(),
    )

    assert summary['result'] == 'pass'
