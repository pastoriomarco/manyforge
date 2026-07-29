"""Compile an immutable experimental Lite6 binding manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile
from typing import Any

from .binding import compile_lite6_binding
from .binding import Lite6BindingInputs
from .controller_binding import compile_production_controller_binding
from .profile import load_profile


BINDING_SCHEMA = 'manyforge.xarm_adapter.binding.v1.experimental'
MODEL_SCHEMA = 'manyforge.xarm_adapter.fixed_model.v0.experimental'
SOURCE_SCHEMA = 'manyforge.xarm_ros2_source.v0.experimental'


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def _json(path: Path, schema: str) -> dict[str, Any]:
    payload = json.loads(path.read_text(encoding='utf-8'))
    if not isinstance(payload, dict) or payload.get('schema') != schema:
        raise ValueError(f'{path} is not a {schema} document')
    return payload


def _model_artifact(
    model_manifest: dict[str, Any],
    manifest_path: Path,
    role: str,
) -> tuple[Path, str]:
    files = model_manifest.get('files')
    if not isinstance(files, list):
        raise ValueError('model manifest files must be a list')
    matches = [
        item
        for item in files
        if isinstance(item, dict) and item.get('role') == role
    ]
    if len(matches) != 1:
        raise ValueError(
            f'model manifest must contain exactly one {role} artifact'
        )
    relative = matches[0].get('path')
    expected_digest = matches[0].get('sha256')
    if (
        not isinstance(relative, str)
        or not relative
        or Path(relative).name != relative
    ):
        raise ValueError(f'model artifact {role} path must be one filename')
    if (
        not isinstance(expected_digest, str)
        or len(expected_digest) != 64
        or any(character not in '0123456789abcdef' for character in expected_digest)
    ):
        raise ValueError(f'model artifact {role} has an invalid digest')
    path = manifest_path.parent / relative
    if not path.is_file():
        raise ValueError(f'model artifact {role} is missing: {path}')
    actual_digest = _sha256(path)
    if actual_digest != expected_digest:
        raise ValueError(f'model artifact {role} digest does not match')
    return path, path.read_text(encoding='utf-8')


def compile_manifest(
    *,
    profile_path: Path,
    model_manifest_path: Path,
    source_manifest_path: Path,
) -> dict[str, Any]:
    """Compile one binding without reading or retaining the robot IP."""
    profile = load_profile(profile_path)
    model_manifest = _json(model_manifest_path, MODEL_SCHEMA)
    source_manifest = _json(source_manifest_path, SOURCE_SCHEMA)
    binding = compile_lite6_binding(
        Lite6BindingInputs(
            ros_namespace=profile.ros_namespace,
            prefix=profile.prefix,
            hardware_namespace=profile.hardware_namespace,
            controller_name=profile.controller_name,
        )
    )
    _, supervised_runtime_urdf = _model_artifact(
        model_manifest,
        model_manifest_path,
        'supervised_runtime_urdf_template',
    )
    _, ros2_controllers_yaml = _model_artifact(
        model_manifest,
        model_manifest_path,
        'ros2_controllers',
    )
    controller_binding = compile_production_controller_binding(
        resolved=binding,
        supervised_runtime_urdf=supervised_runtime_urdf,
        ros2_controllers_yaml=ros2_controllers_yaml,
    )
    if model_manifest.get('profile_id') != profile.profile_id:
        raise ValueError('model manifest profile_id does not match the adapter profile')
    if model_manifest.get('source', {}).get('content_sha256') != source_manifest.get(
        'content_sha256'
    ):
        raise ValueError('model and xArm source manifests identify different source trees')

    identity = {
        'profile_sha256': _sha256(profile_path),
        'model_manifest_sha256': _sha256(model_manifest_path),
        'source_manifest_sha256': _sha256(source_manifest_path),
        'binding_generation_id': binding.generation_id,
        'controller_binding_generation_id': controller_binding.generation_id,
    }
    generation_id = hashlib.sha256(
        json.dumps(identity, sort_keys=True, separators=(',', ':')).encode('utf-8')
    ).hexdigest()
    return {
        'schema': BINDING_SCHEMA,
        'generation_id': generation_id,
        'profile_id': profile.profile_id,
        'binding': binding.to_dict(),
        'controller_binding': controller_binding.to_dict(),
        'model': {
            'manifest_path': str(model_manifest_path),
            'manifest_sha256': identity['model_manifest_sha256'],
            'generation_id': model_manifest['generation_id'],
        },
        'source': {
            'manifest_path': str(source_manifest_path),
            'manifest_sha256': identity['source_manifest_sha256'],
            'repository': source_manifest['repository'],
            'commit': source_manifest['commit'],
            'tree': source_manifest['tree'],
            'content_sha256': source_manifest['content_sha256'],
            'qualification': 'local_experimental',
        },
        'network': {
            'address_source': f'environment:{profile.address_source_env}',
            'address_retained': False,
        },
        'robot_family': {
            'expected_axis': 6,
            'expected_device_type': 9,
        },
        'command_surface': {
            'startup_mode': 'disconnected_template',
            'motion_admission': 'closed',
            'read_only_driver_command_endpoints': 'suppressed',
            'supervised_legacy_command_endpoints': 'suppressed',
            'controller_manager_started': False,
            'trajectory_controller_started': False,
            'tool_command_provider_started': False,
            'real_transport_backend': 'not_bound',
            'runtime_binding_verification': 'required_before_dispatch',
        },
    }


def _atomic_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(
        prefix=f'.{path.name}.',
        dir=path.parent,
        text=True,
    )
    try:
        with os.fdopen(descriptor, 'w', encoding='utf-8') as handle:
            json.dump(payload, handle, indent=2, sort_keys=True)
            handle.write('\n')
        os.replace(temporary, path)
    except BaseException:
        Path(temporary).unlink(missing_ok=True)
        raise


def main() -> int:
    """CLI entry point."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', type=Path, required=True)
    parser.add_argument('--model-manifest', type=Path, required=True)
    parser.add_argument('--source-manifest', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    payload = compile_manifest(
        profile_path=args.profile.resolve(),
        model_manifest_path=args.model_manifest.resolve(),
        source_manifest_path=args.source_manifest.resolve(),
    )
    _atomic_json(args.output.resolve(), payload)
    print(args.output.resolve())
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
