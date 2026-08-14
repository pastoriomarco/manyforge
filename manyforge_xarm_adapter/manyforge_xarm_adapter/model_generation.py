"""Generate offline fixed-pneumatic Lite6 qualification artifacts."""

from __future__ import annotations

import argparse
from copy import deepcopy
import hashlib
import json
import os
from pathlib import Path
import tempfile
from typing import Any
import xml.etree.ElementTree as ET

import yaml

from .binding import compile_lite6_binding
from .binding import Lite6BindingInputs
from .controller_binding import EXPECTED_COMMAND_INTERFACES
from .controller_binding import EXPECTED_STATE_INTERFACES
from .controller_binding import SUPERVISED_FAKE_HARDWARE_PLUGIN
from .controller_binding import SUPERVISED_HARDWARE_PLUGIN
from .profile import Lite6Profile
from .profile import load_profile


MODEL_SCHEMA = 'manyforge.xarm_adapter.fixed_model.v0.experimental'
SOURCE_SCHEMA = 'manyforge.xarm_ros2_source.v0.experimental'
ROBOT_IP_TOKEN = '__MANYFORGE_RUNTIME_ROBOT_IP__'
TRANSPORT_OWNER_ID_TOKEN = '__MANYFORGE_TRANSPORT_OWNER_ID__'
REAL_OBSERVATION_LEASE_MS = 1_000
REAL_COMMAND_GATE_MAX_LEASE_MS = REAL_OBSERVATION_LEASE_MS
MANYFORGE_CANCEL_DECELERATION_RAD_S2 = 2.0
_HEX_LENGTHS = {
    'commit': 40,
    'tree': 40,
    'content_sha256': 64,
    'xc4_base_commit': 40,
    'xc4_base_tree': 40,
}
ARTIFACT_NAMES = {
    'model_urdf': 'lite6_fixed_pneumatic.model.urdf',
    'runtime_urdf_template': 'lite6_fixed_pneumatic.runtime.urdf',
    'supervised_runtime_urdf_template': (
        'lite6_fixed_pneumatic.supervised.runtime.urdf'
    ),
    'supervised_fake_runtime_urdf': (
        'lite6_fixed_pneumatic.supervised_fake.runtime.urdf'
    ),
    'srdf': 'lite6_fixed_pneumatic.srdf',
    'kinematics': 'kinematics.yaml',
    'joint_limits': 'joint_limits.yaml',
    'moveit_controllers': 'moveit_controllers.yaml',
    'ros2_controllers': 'ros2_controllers.yaml',
}


def apply_manyforge_jtc_qualification(
    controller_payload: dict[str, Any],
    profile: Lite6Profile,
) -> dict[str, Any]:
    """Add the ManyForge-only cancel contract to generated xArm parameters."""
    qualified = deepcopy(controller_payload)
    resolved = compile_lite6_binding(
        Lite6BindingInputs(
            ros_namespace=profile.ros_namespace,
            prefix=profile.prefix,
            hardware_namespace=profile.hardware_namespace,
            controller_name=profile.controller_name,
        )
    )
    namespace_root: Any = qualified
    for segment in resolved.ros_namespace.split('/'):
        if not segment:
            continue
        if not isinstance(namespace_root, dict):
            raise ValueError(
                'generated controller namespace root must be a mapping'
            )
        namespace_root = namespace_root.get(segment)
    if not isinstance(namespace_root, dict):
        raise ValueError(
            'generated controller YAML lacks the resolved ROS namespace'
        )

    controller_name = resolved.trajectory_controller.rsplit('/', 1)[-1]
    controller = namespace_root.get(controller_name)
    if not isinstance(controller, dict):
        raise ValueError(
            f'generated controller YAML lacks {controller_name}'
        )
    parameters = controller.get('ros__parameters')
    if not isinstance(parameters, dict):
        raise ValueError(
            f'{controller_name}.ros__parameters must be a mapping'
        )
    if parameters.get('joints') != list(resolved.arm_joint_names):
        raise ValueError(
            'generated controller joints disagree with the Lite6 profile'
        )
    constraints = parameters.get('constraints')
    if not isinstance(constraints, dict):
        raise ValueError(
            f'{controller_name}.constraints must be a mapping'
        )

    constraints['decelerate_on_cancel'] = True
    for joint_name in resolved.arm_joint_names:
        joint_constraints = constraints.get(joint_name)
        if not isinstance(joint_constraints, dict):
            raise ValueError(
                f'{controller_name}.constraints.{joint_name} '
                'must be a mapping'
            )
        joint_constraints['max_deceleration_on_cancel'] = (
            MANYFORGE_CANCEL_DECELERATION_RAD_S2
        )
    return qualified


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def _canonical_xml(root: ET.Element) -> bytes:
    ET.indent(root, space='  ')
    return ET.tostring(root, encoding='utf-8', xml_declaration=True)


def model_only_urdf(runtime_urdf: str) -> str:
    """Remove execution-only blocks while preserving model semantics."""
    root = ET.fromstring(runtime_urdf)
    for child in list(root):
        if child.tag in {'ros2_control', 'transmission', 'gazebo'}:
            root.remove(child)
    return _canonical_xml(root).decode('utf-8')


def supervised_runtime_urdf(
    runtime_urdf: str,
    profile: Lite6Profile,
    *,
    test_only_fake: bool,
) -> str:
    """Replace only the ros2_control owner while preserving model semantics."""
    root = ET.fromstring(runtime_urdf)
    controls = root.findall('./ros2_control')
    if len(controls) != 1:
        raise ValueError('runtime URDF must contain exactly one ros2_control block')
    control = controls[0]
    control.set('name', f'{profile.prefix}lite6_system')
    hardware = control.find('./hardware')
    if hardware is None:
        raise ValueError('runtime URDF has no ros2_control hardware block')
    plugin = hardware.find('./plugin')
    if plugin is None:
        raise ValueError('runtime URDF has no ros2_control hardware plugin')
    plugin.text = (
        SUPERVISED_FAKE_HARDWARE_PLUGIN
        if test_only_fake
        else SUPERVISED_HARDWARE_PLUGIN
    )
    for parameter in list(hardware.findall('./param')):
        hardware.remove(parameter)

    binding = compile_lite6_binding(
        Lite6BindingInputs(
            ros_namespace=profile.ros_namespace,
            prefix=profile.prefix,
            hardware_namespace=profile.hardware_namespace,
            controller_name=profile.controller_name,
        )
    )
    if test_only_fake:
        parameters = {
            'mock_sensor_commands': 'false',
            'calculate_dynamics': 'false',
        }
    else:
        parameters = {
            'robot_ip': ROBOT_IP_TOKEN,
            'transport_owner_id': TRANSPORT_OWNER_ID_TOKEN,
            'expected_robot_device_type': '9',
            'access_mode': 'supervised_lifecycle',
            'report_type': profile.report_type,
            'dof': str(profile.dof),
            'robot_type': profile.robot_type,
            'prefix': profile.prefix,
            'hw_ns': binding.hardware_namespace,
            'velocity_control': 'false',
            'add_gripper': 'false',
            'add_bio_gripper': 'false',
            'observation_lease_ms': str(REAL_OBSERVATION_LEASE_MS),
            'joint_state_lease_ms': '100',
            'io_period_ms': '5',
            'transport_loss_timeout_ms': '2000',
            'command_gate_max_lease_ms': str(
                REAL_COMMAND_GATE_MAX_LEASE_MS
            ),
            'lifecycle_command_timeout_ms': '4000',
            'source_agreement_tolerance_rad': '0.002',
            'position_initialization_samples': '3',
            'shutdown_stationary_tolerance_rad': '0.001',
            'shutdown_stationary_dwell_ms': '1000',
            'trajectory_controller_name': (
                binding.trajectory_controller.rsplit('/', 1)[-1]
            ),
            'controller_manager_activity_topic': (
                f'{binding.controller_manager}/activity'
            ),
        }
    for name, value in parameters.items():
        parameter = ET.SubElement(hardware, 'param', {'name': name})
        parameter.text = value
    return _canonical_xml(root).decode('utf-8')


def _validate_generated_model(
    *,
    profile: Lite6Profile,
    runtime_urdf: str,
    model_urdf: str,
    srdf: str,
    supervised_runtime: str,
    supervised_fake_runtime: str,
) -> None:
    runtime_root = ET.fromstring(runtime_urdf)
    model_root = ET.fromstring(model_urdf)
    semantic_root = ET.fromstring(srdf)

    if ROBOT_IP_TOKEN not in runtime_urdf:
        raise ValueError('runtime URDF does not contain the required IP token')
    if ROBOT_IP_TOKEN in model_urdf:
        raise ValueError('model-only URDF retained the runtime IP token')
    if model_root.findall('./ros2_control'):
        raise ValueError('model-only URDF retained ros2_control')

    controls = runtime_root.findall('./ros2_control')
    if len(controls) != 1:
        raise ValueError('runtime URDF must contain exactly one ros2_control block')
    plugin = controls[0].findtext('./hardware/plugin')
    if plugin != 'uf_robot_hardware/UFRobotSystemHardware':
        raise ValueError(f'unexpected xArm hardware plugin: {plugin!r}')

    expected_arm_joints = {f'{profile.prefix}joint{index}' for index in range(1, 7)}
    actual_joints = {joint.get('name') for joint in model_root.findall('./joint')}
    missing_joints = sorted(expected_arm_joints - actual_joints)
    if missing_joints:
        raise ValueError(f'generated model is missing arm joints: {missing_joints}')

    expected_links = {
        f'{profile.prefix}other_geometry_link',
        f'{profile.prefix}link_tcp',
    }
    actual_links = {link.get('name') for link in model_root.findall('./link')}
    missing_links = sorted(expected_links - actual_links)
    if missing_links:
        raise ValueError(f'generated model is missing fixed-tool links: {missing_links}')

    tool_joint = model_root.find(
        f"./joint[@name='{profile.prefix}other_geometry_fix']"
    )
    if tool_joint is None or tool_joint.get('type') != 'fixed':
        raise ValueError('custom pneumatic tool must attach through one fixed joint')
    tool_link = model_root.find(
        f"./link[@name='{profile.prefix}other_geometry_link']"
    )
    meshes = [
        mesh.get('filename', '')
        for mesh in tool_link.findall('.//mesh')
    ]
    if not meshes or not all(
        mesh.endswith(f'/other/{profile.geometry_mesh_filename}') for mesh in meshes
    ):
        raise ValueError('custom pneumatic visual/collision mesh does not match profile')

    group = semantic_root.find(f"./group[@name='{profile.prefix}lite6']")
    if group is None:
        raise ValueError('SRDF does not contain the Lite6 planning group')
    group_joints = {joint.get('name') for joint in group.findall('./joint')}
    for required in (
        f'{profile.prefix}other_geometry_fix',
        f'{profile.prefix}joint_tcp',
    ):
        if required not in group_joints:
            raise ValueError(f'SRDF Lite6 group is missing {required}')

    other_group = semantic_root.find(
        f"./group[@name='{profile.prefix}other_geometry']"
    )
    if other_group is None:
        raise ValueError('SRDF does not contain the fixed-tool group')

    normalized_runtime = model_only_urdf(runtime_urdf)
    if normalized_runtime != model_urdf:
        raise ValueError('runtime/model URDF model semantics diverged')

    for (
        label,
        candidate,
        expected_plugin,
        expected_backend,
        expected_params,
    ) in (
        (
            'supervised production',
            supervised_runtime,
            SUPERVISED_HARDWARE_PLUGIN,
            None,
            None,
        ),
        (
            'supervised fake',
            supervised_fake_runtime,
            SUPERVISED_FAKE_HARDWARE_PLUGIN,
            None,
            {
                'mock_sensor_commands': 'false',
                'calculate_dynamics': 'false',
            },
        ),
    ):
        candidate_root = ET.fromstring(candidate)
        candidate_controls = candidate_root.findall('./ros2_control')
        if len(candidate_controls) != 1:
            raise ValueError(f'{label} URDF must contain one ros2_control block')
        candidate_control = candidate_controls[0]
        candidate_plugin = candidate_control.findtext('./hardware/plugin')
        if candidate_plugin != expected_plugin:
            raise ValueError(f'{label} URDF has the wrong hardware owner')
        parameters = {
            item.get('name'): item.text
            for item in candidate_control.findall('./hardware/param')
        }
        if (
            (expected_backend is None and 'backend' in parameters)
            or (
                expected_backend is not None
                and parameters.get('backend') != expected_backend
            )
        ):
            raise ValueError(f'{label} URDF has the wrong backend policy')
        if expected_params is not None and parameters != expected_params:
            raise ValueError(f'{label} URDF has the wrong hardware parameters')
        candidate_joints = candidate_control.findall('./joint')
        if tuple(joint.get('name') for joint in candidate_joints) != tuple(
            f'{profile.prefix}joint{index}' for index in range(1, 7)
        ):
            raise ValueError(f'{label} URDF changed the arm joint order')
        for joint in candidate_joints:
            commands = tuple(
                item.get('name')
                for item in joint.findall('./command_interface')
            )
            states = tuple(
                item.get('name')
                for item in joint.findall('./state_interface')
            )
            if commands != EXPECTED_COMMAND_INTERFACES:
                raise ValueError(
                    f'{label} URDF changed command interfaces for '
                    f"{joint.get('name')}"
                )
            if states != EXPECTED_STATE_INTERFACES:
                raise ValueError(
                    f'{label} URDF changed state interfaces for '
                    f"{joint.get('name')}"
                )
        if model_only_urdf(candidate) != model_urdf:
            raise ValueError(f'{label} URDF changed model semantics')


def _write_text(path: Path, text: str) -> None:
    path.write_text(text if text.endswith('\n') else text + '\n', encoding='utf-8')


def _write_yaml(path: Path, payload: Any) -> None:
    path.write_text(
        yaml.safe_dump(payload, sort_keys=True),
        encoding='utf-8',
    )


def _load_source_manifest(path: Path) -> dict[str, Any]:
    payload = json.loads(path.read_text(encoding='utf-8'))
    if not isinstance(payload, dict) or payload.get('schema') != SOURCE_SCHEMA:
        raise ValueError(f'{path} is not a {SOURCE_SCHEMA} document')
    required = {
        'repository',
        'commit',
        'tree',
        'content_sha256',
        'dirty',
        'xc4_base_commit',
        'xc4_base_tree',
        'qualification',
    }
    missing = sorted(required - set(payload))
    if missing:
        raise ValueError(f"source manifest is missing fields: {', '.join(missing)}")
    for field, length in _HEX_LENGTHS.items():
        value = payload[field]
        if (
            not isinstance(value, str)
            or len(value) != length
            or any(character not in '0123456789abcdef' for character in value)
        ):
            raise ValueError(
                f'source manifest {field} must be {length} lowercase hex digits'
            )
    if not isinstance(payload['dirty'], bool):
        raise ValueError('source manifest dirty must be a boolean')
    if payload['dirty']:
        raise ValueError('source manifest must identify a clean xArm worktree')
    if payload['qualification'] != 'local_experimental':
        raise ValueError('source manifest is not an experimental XC-6 input')
    return payload


def _build_with_xarm(profile: Lite6Profile) -> tuple[str, str, dict[str, Any]]:
    try:
        from ament_index_python import get_package_share_directory
        from uf_ros_lib.moveit_configs_builder import MoveItConfigsBuilder
        from uf_ros_lib.uf_robot_utils import (
            generate_ros2_control_params_temp_file,
        )
    except ImportError as exc:
        raise RuntimeError(
            'the fixed model must be generated inside the real-Lite6 image'
        ) from exc

    mesh_path = (
        Path(get_package_share_directory('xarm_description'))
        / 'meshes'
        / 'other'
        / profile.geometry_mesh_filename
    )
    if not mesh_path.is_file():
        raise ValueError(f'fixed pneumatic mesh is missing: {mesh_path}')
    actual_mesh_sha256 = _sha256(mesh_path)
    if actual_mesh_sha256 != profile.geometry_mesh_sha256:
        raise ValueError(
            'fixed pneumatic mesh digest does not match the adapter profile'
        )

    builder = MoveItConfigsBuilder(
        context=None,
        controllers_name='controllers',
        robot_ip=ROBOT_IP_TOKEN,
        report_type=profile.report_type,
        dof=profile.dof,
        robot_type=profile.robot_type,
        prefix=profile.prefix,
        hw_ns=profile.hardware_namespace,
        limited=profile.limited,
        effort_control=False,
        velocity_control=False,
        attach_to='world',
        attach_xyz='0 0 0',
        attach_rpy='0 0 0',
        ros2_control_plugin='uf_robot_hardware/UFRobotSystemHardware',
        add_gripper=False,
        add_vacuum_gripper=False,
        add_bio_gripper=False,
        add_realsense_d435i=False,
        add_other_geometry=True,
        geometry_type='mesh',
        geometry_mass=profile.geometry_mass_kg,
        geometry_mesh_filename=profile.geometry_mesh_filename,
        geometry_mesh_origin_xyz=profile.geometry_mesh_origin_xyz,
        geometry_mesh_origin_rpy=profile.geometry_mesh_origin_rpy,
        geometry_mesh_tcp_xyz=profile.geometry_mesh_tcp_xyz,
        geometry_mesh_tcp_rpy=profile.geometry_mesh_tcp_rpy,
    )
    moveit = builder.to_moveit_configs()
    runtime_urdf = moveit.robot_description['robot_description']
    srdf = moveit.robot_description_semantic['robot_description_semantic']

    controller_source = (
        Path(get_package_share_directory('xarm_controller'))
        / 'config'
        / 'lite6_controllers.yaml'
    )
    generated_controller_path = Path(
        generate_ros2_control_params_temp_file(
            str(controller_source),
            prefix=profile.prefix,
            add_gripper=False,
            add_bio_gripper=False,
            robot_type=profile.robot_type,
        )
    )
    ros2_controllers = yaml.safe_load(
        generated_controller_path.read_text(encoding='utf-8')
    )
    if not isinstance(ros2_controllers, dict):
        raise ValueError('generated controller YAML root must be a mapping')
    ros2_controllers = apply_manyforge_jtc_qualification(
        ros2_controllers,
        profile,
    )
    moveit_controllers = moveit.trajectory_execution[
        'moveit_simple_controller_manager'
    ]
    return runtime_urdf, srdf, {
        'kinematics': moveit.robot_description_kinematics[
            'robot_description_kinematics'
        ],
        'joint_limits': moveit.joint_limits['robot_description_planning'],
        'moveit_controllers': moveit_controllers,
        'ros2_controllers': ros2_controllers,
    }


def generate_model(
    *,
    profile_path: Path,
    source_manifest_path: Path,
    output_dir: Path,
) -> dict[str, Any]:
    """Generate deterministic artifacts and an integrity manifest."""
    profile = load_profile(profile_path)
    source = _load_source_manifest(source_manifest_path)
    if source['repository'] != profile.source_repository:
        raise ValueError('xArm source repository does not match the profile')
    if source.get('xc4_base_commit') != profile.xc4_base_commit:
        raise ValueError('xArm source does not declare the qualified XC-4 base')
    if source.get('xc4_base_tree') != profile.xc4_base_tree:
        raise ValueError('xArm source does not declare the qualified XC-4 tree')

    runtime_urdf, srdf, configs = _build_with_xarm(profile)
    model_urdf = model_only_urdf(runtime_urdf)
    supervised_runtime = supervised_runtime_urdf(
        runtime_urdf,
        profile,
        test_only_fake=False,
    )
    supervised_fake_runtime = supervised_runtime_urdf(
        runtime_urdf,
        profile,
        test_only_fake=True,
    )
    _validate_generated_model(
        profile=profile,
        runtime_urdf=runtime_urdf,
        model_urdf=model_urdf,
        srdf=srdf,
        supervised_runtime=supervised_runtime,
        supervised_fake_runtime=supervised_fake_runtime,
    )

    output_dir.mkdir(parents=True, exist_ok=True)
    _write_text(output_dir / ARTIFACT_NAMES['runtime_urdf_template'], runtime_urdf)
    _write_text(
        output_dir / ARTIFACT_NAMES['supervised_runtime_urdf_template'],
        supervised_runtime,
    )
    _write_text(
        output_dir / ARTIFACT_NAMES['supervised_fake_runtime_urdf'],
        supervised_fake_runtime,
    )
    _write_text(output_dir / ARTIFACT_NAMES['model_urdf'], model_urdf)
    _write_text(output_dir / ARTIFACT_NAMES['srdf'], srdf)
    for key in (
        'kinematics',
        'joint_limits',
        'moveit_controllers',
        'ros2_controllers',
    ):
        _write_yaml(output_dir / ARTIFACT_NAMES[key], configs[key])

    files = []
    for role, name in sorted(ARTIFACT_NAMES.items()):
        path = output_dir / name
        files.append(
            {
                'role': role,
                'path': name,
                'size_bytes': path.stat().st_size,
                'sha256': _sha256(path),
            }
        )
    identity = {
        'profile_sha256': _sha256(profile_path),
        'source_manifest_sha256': _sha256(source_manifest_path),
        'files': files,
    }
    generation_id = hashlib.sha256(
        json.dumps(identity, sort_keys=True, separators=(',', ':')).encode('utf-8')
    ).hexdigest()
    manifest = {
        'schema': MODEL_SCHEMA,
        'generation_id': generation_id,
        'profile_id': profile.profile_id,
        'tool_profile_id': profile.tool_profile_id,
        'model_contract': {
            'arm_joint_count': 6,
            'tool_actuation': 'none',
            'tool_attachment': 'fixed',
            'tool_mesh_sha256': profile.geometry_mesh_sha256,
            'runtime_ip_representation': 'unresolved_token',
            'model_contains_network_address': False,
            'runtime_and_model_semantics_equal': True,
        },
        'source': {
            'repository': source['repository'],
            'commit': source['commit'],
            'tree': source['tree'],
            'content_sha256': source['content_sha256'],
            'source_manifest_sha256': identity['source_manifest_sha256'],
            'qualification': 'local_experimental',
        },
        'profile_sha256': identity['profile_sha256'],
        'files': files,
    }
    manifest_path = output_dir / 'MODEL_MANIFEST.json'
    descriptor, temporary = tempfile.mkstemp(
        prefix='.MODEL_MANIFEST.',
        dir=output_dir,
        text=True,
    )
    try:
        with os.fdopen(descriptor, 'w', encoding='utf-8') as handle:
            json.dump(manifest, handle, indent=2, sort_keys=True)
            handle.write('\n')
        os.replace(temporary, manifest_path)
    except BaseException:
        Path(temporary).unlink(missing_ok=True)
        raise
    return manifest


def main() -> int:
    """CLI entry point."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', type=Path, required=True)
    parser.add_argument(
        '--source-manifest',
        type=Path,
        default=Path('/opt/manyforge/xarm/source-manifest.json'),
    )
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    manifest = generate_model(
        profile_path=args.profile.resolve(),
        source_manifest_path=args.source_manifest.resolve(),
        output_dir=args.output.resolve(),
    )
    print(manifest['generation_id'])
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
