from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shlex
import subprocess

from manyforge_composer.backend.robot_lifecycle import (
    validate_real_robot_runtime_record,
)
from manyforge_xarm_adapter.model_generation import ARTIFACT_NAMES
from manyforge_xarm_adapter.real_robot_deployment import (
    resolve_real_robot_deployment,
    RUNTIME_RECORD_SCHEMA,
)
from manyforge_xarm_adapter.supervised_runtime_binding import (
    RUNTIME_BINDING_SCHEMA,
)
import pytest
import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]
CELL = REPO_ROOT / 'examples' / (
    'lite6_fixed_pneumatic_commissioning.real-robot-cell.yaml'
)
TEMPLATE = REPO_ROOT / 'examples' / (
    'lite6_fixed_pneumatic_commissioning.deployment.template.yaml'
)
PROGRAM = REPO_ROOT / 'examples' / (
    'lite6_fixed_pneumatic_commissioning.program.yaml'
)


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _fixture(tmp_path: Path) -> tuple[Path, Path]:
    model_root = tmp_path / 'model'
    model_root.mkdir()
    for name in {
        ARTIFACT_NAMES['model_urdf'],
        ARTIFACT_NAMES['srdf'],
        ARTIFACT_NAMES['ros2_controllers'],
        'MODEL_MANIFEST.json',
    }:
        (model_root / name).write_text(f'{name}\n', encoding='utf-8')
    runtime_urdf = tmp_path / 'runtime.urdf'
    runtime_urdf.write_text('<robot name="lite6"/>\n', encoding='utf-8')
    provider = {
        'schema': 'manyforge.robot_lifecycle.ros_provider.v1',
        'robot_id': 'lite6-a',
        'runtime_generation_id': 'b' * 64,
        'cell_supervisor_component_epoch': 'supervisor-epoch-1',
        'provider_instance_id': 'lite6-a:provider:test',
        'status_topic': '/manyforge/robot_lifecycle/status',
        'open_session_service': '/manyforge/robot_lifecycle/open',
        'close_session_service': '/manyforge/robot_lifecycle/close',
        'submit_command_service': '/manyforge/robot_lifecycle/submit',
        'reconcile_command_service': '/manyforge/robot_lifecycle/reconcile',
        'caller_assurance': 'unverified',
        'maximum_authority_lease_ms': 300_000,
        'authority_lease_duration_ms': 5_000,
        'authority_heartbeat_period_ms': 1_000,
        'authority_health_max_age_ms': 2_000,
    }
    controller = {
        'generation_id': 'c' * 64,
        'follow_joint_trajectory_action': (
            '/lite6_traj_controller/follow_joint_trajectory'
        ),
    }
    cell_supervision = {
        'schema': 'manyforge.cell_supervision.v1',
        'cell_id': 'lite6-a',
        'robot_ids': ['lite6-a'],
        'authority_profile': 'local_operator',
        'supervisor_heartbeat_path': (
            str(tmp_path / 'cell-supervisor-heartbeat.json')
        ),
        'cell_supervisor_component_epoch': 'supervisor-epoch-1',
        'components': [
            {
                'role': 'cell_supervisor',
                'id': 'lite6-a:cell-supervisor',
                'maximum_heartbeat_age_ms': 1_000,
                'maximum_critical_progress_age_ms': 2_000,
                'dependencies': [],
                'restart_domain': 'cell-control',
            },
        ],
    }
    binding = {
        'schema': RUNTIME_BINDING_SCHEMA,
        'generation_id': 'b' * 64,
        'runtime': {
            'robot_instance_id': 'lite6-a',
            'robot_ip': '192.168.1.31',
            'transport_owner_id': 'lite6-a:sole-owner',
            'authority_profile': 'local_operator',
            'lifecycle_provider_instance_id': 'lite6-a:provider:test',
            'urdf_path': str(runtime_urdf),
            'urdf_sha256': _sha256(runtime_urdf),
        },
        'controller_binding': controller,
        'robot_lifecycle_provider': provider,
        'cell_supervision': cell_supervision,
        'manyforge_execution_config': {
            'action_name': controller['follow_joint_trajectory_action'],
            'arm_action': controller['follow_joint_trajectory_action'],
            'decelerate_on_cancel': True,
            'velocity_state_interface_joints': [
                f'joint{index}' for index in range(1, 7)
            ],
            'max_deceleration_on_cancel_rad_s2': {
                f'joint{index}': 2.0 for index in range(1, 7)
            },
            'controller_binding': controller,
            'robot_lifecycle_provider': provider,
            'cell_supervision': cell_supervision,
            'require_execution_admission_snapshot': True,
        },
    }
    binding_path = tmp_path / 'runtime-binding.json'
    binding_path.write_text(
        json.dumps(binding),
        encoding='utf-8',
    )
    return model_root, binding_path


def test_resolves_template_and_validates_every_retained_artifact(
    tmp_path: Path,
) -> None:
    model_root, binding_path = _fixture(tmp_path)
    output_deployment = tmp_path / 'resolved' / 'commissioning.deployment.yaml'
    output_record = tmp_path / 'resolved' / 'runtime-record.json'

    record = resolve_real_robot_deployment(
        cell_path=CELL,
        template_path=TEMPLATE,
        program_path=PROGRAM,
        model_root=model_root,
        runtime_binding_path=binding_path,
        output_deployment_path=output_deployment,
        output_record_path=output_record,
    )

    assert record['schema'] == RUNTIME_RECORD_SCHEMA
    assert record['robot']['address'] == '192.168.1.31'
    assert record['robot']['authorityHealthPolicy'] == {
        'leaseDurationMs': 5_000,
        'heartbeatPeriodMs': 1_000,
        'criticalProgressMaxAgeMs': 2_000,
        'providerMaximumLeaseMs': 300_000,
    }
    assert record['cellSupervision']['supervisorComponentEpoch'] == (
        'supervisor-epoch-1'
    )
    resolved = yaml.safe_load(output_deployment.read_text(encoding='utf-8'))
    config = resolved['adapters']['execution']['config']
    assert '__MANYFORGE_' not in output_deployment.read_text(encoding='utf-8')
    assert config['robot_lifecycle_provider']['provider_instance_id'] == (
        'lite6-a:provider:test'
    )
    assert config['operator_stop_profile']['stop_mode'] == 'soft_on_path'
    assert config['default_fault_stop_profile']['deceleration_time_s'] == 2.0
    assert config['decelerate_on_cancel'] is True
    assert config['velocity_state_interface_joints'] == [
        f'joint{index}' for index in range(1, 7)
    ]
    assert config['max_deceleration_on_cancel_rad_s2'] == {
        f'joint{index}': 2.0 for index in range(1, 7)
    }
    assert config['start_tolerance_rad'] == 0.01
    assert config['final_tolerance_rad'] == 0.01
    assert config['real_robot_runtime_record']['path'] == str(output_record)
    joint_limits = resolved['robot_config']['robot_control_joint_limits']
    assert set(joint_limits) == {
        'joint1',
        'joint2',
        'joint3',
        'joint4',
        'joint5',
        'joint6',
    }
    assert all(
        limit == {
            'max_velocity_rad_s': 0.1,
            'max_acceleration_rad_s2': 0.2,
        }
        for limit in joint_limits.values()
    )
    validated = validate_real_robot_runtime_record(
        deployment_path=output_deployment,
        execution_config=config,
    )
    assert validated['runtimeGenerationId'] == 'b' * 64


def test_runtime_record_rejects_artifact_drift(tmp_path: Path) -> None:
    model_root, binding_path = _fixture(tmp_path)
    output_deployment = tmp_path / 'resolved' / 'commissioning.deployment.yaml'
    output_record = tmp_path / 'resolved' / 'runtime-record.json'
    resolve_real_robot_deployment(
        cell_path=CELL,
        template_path=TEMPLATE,
        program_path=PROGRAM,
        model_root=model_root,
        runtime_binding_path=binding_path,
        output_deployment_path=output_deployment,
        output_record_path=output_record,
    )
    resolved = yaml.safe_load(output_deployment.read_text(encoding='utf-8'))
    config = resolved['adapters']['execution']['config']
    (model_root / ARTIFACT_NAMES['model_urdf']).write_text(
        'drift\n',
        encoding='utf-8',
    )

    with pytest.raises(ValueError, match='modelUrdf.*digest mismatch'):
        validate_real_robot_runtime_record(
            deployment_path=output_deployment,
            execution_config=config,
        )


def test_runtime_record_rejects_authority_health_policy_drift(
    tmp_path: Path,
) -> None:
    model_root, binding_path = _fixture(tmp_path)
    output_deployment = tmp_path / 'resolved' / 'commissioning.deployment.yaml'
    output_record = tmp_path / 'resolved' / 'runtime-record.json'
    resolve_real_robot_deployment(
        cell_path=CELL,
        template_path=TEMPLATE,
        program_path=PROGRAM,
        model_root=model_root,
        runtime_binding_path=binding_path,
        output_deployment_path=output_deployment,
        output_record_path=output_record,
    )
    resolved = yaml.safe_load(output_deployment.read_text(encoding='utf-8'))
    config = resolved['adapters']['execution']['config']
    config['robot_lifecycle_provider']['authority_lease_duration_ms'] = 6_000

    with pytest.raises(ValueError, match='authority-health policy mismatch'):
        validate_real_robot_runtime_record(
            deployment_path=output_deployment,
            execution_config=config,
        )


def test_product_launcher_separates_control_owner_from_composer() -> None:
    owner = (
        REPO_ROOT / 'scripts' / 'ros' / 'start-lite6-real-robot-owner.sh'
    ).read_text(encoding='utf-8')
    composer = (
        REPO_ROOT
        / 'scripts'
        / 'ros'
        / 'start-lite6-real-robot-composer.sh'
    ).read_text(encoding='utf-8')
    launcher = (
        REPO_ROOT / 'scripts' / 'lib' / 'real_robot.sh'
    ).read_text(encoding='utf-8')
    launch_entrypoint = (
        REPO_ROOT / 'scripts' / 'launch.sh'
    ).read_text(encoding='utf-8')
    smoke_validator = (
        REPO_ROOT
        / 'scripts'
        / 'xarm'
        / 'validate-lite6-product-lane-smoke.py'
    ).read_text(encoding='utf-8')
    transport_source = (
        REPO_ROOT
        / 'manyforge_xarm_control'
        / 'src'
        / 'xarm_ros_transport_session.cpp'
    ).read_text(encoding='utf-8')
    builder = (
        REPO_ROOT / 'scripts' / 'xarm' / 'build-real-lite6-image.sh'
    ).read_text(encoding='utf-8')
    dockerfile = (
        REPO_ROOT / 'Dockerfile.ros-jazzy-real-lite6'
    ).read_text(encoding='utf-8')

    assert 'controller_manager ros2_control_node' in owner
    assert 'manyforge-xarm-run-execution-admission-provider' in owner
    assert 'manyforge_composer' not in owner
    assert 'python -m manyforge_composer' in composer
    assert 'controller_manager ros2_control_node' not in composer
    assert 'manyforge-xarm-run-execution-admission-provider' not in composer
    assert 'manyforge-real-robot-owner' in launcher
    assert 'manyforge-real-robot-composer' in launcher
    assert '--cap-add SYS_NICE' in launcher
    assert '--ulimit rtprio=99:99' in launcher
    assert '--ulimit memlock=-1:-1' in launcher
    assert 'docker image inspect' in launcher
    assert 'immutable image identity' in launcher
    assert launcher.count('"${REAL_ROBOT_IMAGE_ID}"') == 2
    assert launcher.count(
        '"${REAL_ROBOT_BUILD_CACHE_VOLUME}:/tmp/manyforge-build"'
    ) == 2
    assert 'manyforge_real-lite6-build-cache:/tmp/manyforge-build' not in launcher
    assert 'manyforge_build-cache:/tmp/manyforge-build' not in launcher
    assert 'REAL_ROBOT_SUPERVISED_CONTRACT="1"' in launcher
    assert 'org.manyforge.xarm.supervised-contract' in launcher
    assert 'MANYFORGE_REAL_ROBOT_SMOKE_TIMEOUT_S' in launch_entrypoint
    assert 'validate-lite6-product-lane-smoke.py' in launch_entrypoint
    assert 'deadline = clock() + float(timeout_s)' in smoke_validator
    assert 'options.use_global_arguments(false);' in transport_source
    assert (
        'SUPERVISED_CONTRACT_MIN_COMMIT='
        'b951ad1f4ce7f1bceb077c74778327f2155e42b7'
    ) in builder
    assert 'XARM_SUPERVISED_CONTRACT_VERSION' in dockerfile
    assert 'org.manyforge.xarm.supervised-contract' in dockerfile


def test_real_robot_build_cache_is_bound_to_immutable_image_id() -> None:
    launcher = REPO_ROOT / 'scripts' / 'lib' / 'real_robot.sh'

    def resolve(image_id: str) -> subprocess.CompletedProcess[str]:
        command = (
            f'source {shlex.quote(str(launcher))}; '
            'real_robot_build_cache_volume_for_image_id '
            f'{shlex.quote(image_id)}'
        )
        return subprocess.run(
            ['bash', '-c', command],
            check=False,
            capture_output=True,
            text=True,
            env={'MANYFORGE_ROOT': str(REPO_ROOT), 'PATH': '/usr/bin:/bin'},
        )

    first_digest = 'a' * 64
    second_digest = 'b' * 64
    first = resolve(f'sha256:{first_digest}')
    second = resolve(f'sha256:{second_digest}')
    malformed = resolve('not-an-image-id')

    assert first.returncode == 0
    assert first.stdout.strip() == (
        f'manyforge_real-lite6-build-cache-{first_digest}'
    )
    assert second.returncode == 0
    assert second.stdout.strip() == (
        f'manyforge_real-lite6-build-cache-{second_digest}'
    )
    assert first.stdout != second.stdout
    assert malformed.returncode != 0
    assert malformed.stdout == ''


def test_product_capture_retains_bounded_recent_lifecycle_history() -> None:
    capture = (
        REPO_ROOT
        / 'scripts'
        / 'xarm'
        / 'capture-lite6-product-lane-state.sh'
    ).read_text(encoding='utf-8')

    assert '"robot-lifecycle-history"' in capture
    assert '?limit=300&channels=robot_lifecycle' in capture
    assert 'manyforge.robot_lifecycle.history_capture.v1' in capture
