"""Validate a generated Lite6 controller binding without contacting ROS."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import tempfile
from typing import Any

from .binding import ResolvedLite6Binding
from .binding_manifest import BINDING_SCHEMA
from .controller_binding import compile_production_controller_binding
from .model_generation import ARTIFACT_NAMES


def _json_mapping(path: Path) -> dict[str, Any]:
    payload = json.loads(path.read_text(encoding='utf-8'))
    if not isinstance(payload, dict):
        raise ValueError(f'{path} must contain a JSON object')
    return payload


def _atomic_text(path: Path, value: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(
        prefix=f'.{path.name}.',
        dir=path.parent,
        text=True,
    )
    try:
        with os.fdopen(descriptor, 'w', encoding='utf-8') as handle:
            handle.write(value)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    except BaseException:
        Path(temporary).unlink(missing_ok=True)
        raise


def validate_controller_binding(
    *,
    binding_manifest_path: Path,
    model_root: Path,
) -> dict[str, object]:
    """Recompile the controller contract and require byte-level agreement."""
    manifest = _json_mapping(binding_manifest_path)
    if manifest.get('schema') != BINDING_SCHEMA:
        raise ValueError(
            f'{binding_manifest_path} is not a {BINDING_SCHEMA} document'
        )
    binding_payload = manifest.get('binding')
    if not isinstance(binding_payload, dict):
        raise ValueError('binding manifest has no resolved binding')
    binding_values = dict(binding_payload)
    arm_joint_names = binding_values.get('arm_joint_names')
    if not isinstance(arm_joint_names, list):
        raise ValueError('binding arm_joint_names must be a list')
    binding_values['arm_joint_names'] = tuple(arm_joint_names)
    try:
        resolved = ResolvedLite6Binding(**binding_values)
    except TypeError as exc:
        raise ValueError('binding manifest resolved binding is malformed') from exc

    runtime_path = (
        model_root / ARTIFACT_NAMES['supervised_runtime_urdf_template']
    )
    controllers_path = model_root / ARTIFACT_NAMES['ros2_controllers']
    if not runtime_path.is_file() or not controllers_path.is_file():
        raise ValueError('generated supervised binding artifacts are missing')
    compiled = compile_production_controller_binding(
        resolved=resolved,
        supervised_runtime_urdf=runtime_path.read_text(encoding='utf-8'),
        ros2_controllers_yaml=controllers_path.read_text(encoding='utf-8'),
    )
    recorded = manifest.get('controller_binding')
    if recorded != compiled.to_dict():
        raise ValueError(
            'controller binding manifest disagrees with generated artifacts'
        )
    return {
        'schema': 'manyforge.xarm_adapter.controller_binding_validation.v1',
        'binding_generation_id': compiled.generation_id,
        'ros_namespace': compiled.ros_namespace,
        'model_prefix': compiled.model_prefix,
        'hardware_namespace': compiled.hardware_namespace,
        'controller_manager': compiled.controller_manager,
        'hardware_component_name': compiled.hardware_component_name,
        'trajectory_controller_name': compiled.trajectory_controller_name,
        'joint_count': len(compiled.arm_joint_names),
        'command_interfaces': list(compiled.controller_command_interfaces),
        'state_interfaces': list(compiled.controller_state_interfaces),
        'decelerate_on_cancel': compiled.decelerate_on_cancel,
        'max_deceleration_on_cancel_rad_s2': dict(
            compiled.max_deceleration_on_cancel_rad_s2
        ),
        'claimed_command_interface_names': list(
            compiled.claimed_command_interface_names
        ),
        'state_interface_names': list(compiled.state_interface_names),
        'result': 'pass',
    }


def main() -> int:
    """CLI entry point."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binding-manifest', type=Path, required=True)
    parser.add_argument('--model-root', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = validate_controller_binding(
        binding_manifest_path=args.binding_manifest.resolve(),
        model_root=args.model_root.resolve(),
    )
    serialized = json.dumps(result, indent=2, sort_keys=True) + '\n'
    if args.output is None:
        print(serialized, end='')
    else:
        output = args.output.resolve()
        _atomic_text(output, serialized)
        print(output)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
