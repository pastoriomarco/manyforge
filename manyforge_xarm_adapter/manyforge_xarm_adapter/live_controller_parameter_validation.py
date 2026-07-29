"""Verify that the loaded JTC matches its generated safety contract."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import sys
import tempfile
from typing import Any, Callable

from .supervised_runtime_binding import RUNTIME_BINDING_SCHEMA


LIVE_CONTROLLER_PARAMETER_VALIDATION_SCHEMA = (
    'manyforge.xarm_adapter.live_controller_parameter_validation.v1'
)


def _mapping(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValueError(f'{label} must be a mapping')
    return value


def expected_live_controller_parameters(
    controller_binding: dict[str, Any],
) -> tuple[str, dict[str, bool | float]]:
    """Project the retained controller contract into ROS parameter names."""
    controller_name = controller_binding.get('trajectory_controller_name')
    if (
        not isinstance(controller_name, str)
        or not controller_name.startswith('/')
    ):
        raise ValueError(
            'controller binding trajectory_controller_name must be absolute'
        )
    joints = controller_binding.get('arm_joint_names')
    if (
        not isinstance(joints, list)
        or not joints
        or not all(isinstance(joint, str) and joint for joint in joints)
        or len(set(joints)) != len(joints)
    ):
        raise ValueError(
            'controller binding arm_joint_names must be unique strings'
        )
    state_interfaces = controller_binding.get(
        'controller_state_interfaces'
    )
    if not isinstance(state_interfaces, list) or 'velocity' not in (
        state_interfaces
    ):
        raise ValueError(
            'cancel deceleration requires controller velocity state'
        )
    if controller_binding.get('decelerate_on_cancel') is not True:
        raise ValueError(
            'controller binding decelerate_on_cancel must be true'
        )
    limits = _mapping(
        controller_binding.get('max_deceleration_on_cancel_rad_s2'),
        'controller binding max_deceleration_on_cancel_rad_s2',
    )
    if set(limits) != set(joints):
        raise ValueError(
            'controller binding cancel-deceleration joints do not match'
        )

    expected: dict[str, bool | float] = {
        'constraints.decelerate_on_cancel': True,
    }
    for joint_name in joints:
        value = limits[joint_name]
        if (
            isinstance(value, bool)
            or not isinstance(value, (int, float))
            or not math.isfinite(value)
            or value <= 0.0
        ):
            raise ValueError(
                f'cancel deceleration for {joint_name} must be positive'
            )
        expected[
            f'constraints.{joint_name}.max_deceleration_on_cancel'
        ] = float(value)
    return controller_name, expected


def validate_live_controller_parameters(
    *,
    controller_binding: dict[str, Any],
    observed_parameters: dict[str, Any],
) -> dict[str, Any]:
    """Require exact live agreement with the retained cancel contract."""
    controller_name, expected = expected_live_controller_parameters(
        controller_binding
    )
    mismatches: list[dict[str, Any]] = []
    for name, expected_value in expected.items():
        if name not in observed_parameters:
            mismatches.append(
                {
                    'parameter': name,
                    'expected': expected_value,
                    'observed': None,
                    'reason': 'missing',
                }
            )
            continue
        observed_value = observed_parameters[name]
        if isinstance(expected_value, bool):
            matches = (
                isinstance(observed_value, bool)
                and observed_value is expected_value
            )
        else:
            matches = (
                not isinstance(observed_value, bool)
                and isinstance(observed_value, (int, float))
                and math.isfinite(float(observed_value))
                and math.isclose(
                    float(observed_value),
                    expected_value,
                    rel_tol=0.0,
                    abs_tol=1e-12,
                )
            )
        if not matches:
            mismatches.append(
                {
                    'parameter': name,
                    'expected': expected_value,
                    'observed': observed_value,
                    'reason': 'value_mismatch',
                }
            )
    if mismatches:
        names = ', '.join(item['parameter'] for item in mismatches)
        raise ValueError(
            'loaded controller disagrees with the generated safety '
            f'contract: {names}'
        )
    return {
        'schema': LIVE_CONTROLLER_PARAMETER_VALIDATION_SCHEMA,
        'validatedAt': datetime.now(timezone.utc).isoformat(),
        'result': 'pass',
        'controllerNode': controller_name,
        'controllerBindingGenerationId': controller_binding.get(
            'generation_id'
        ),
        'expectedParameters': expected,
        'observedParameters': {
            name: observed_parameters[name] for name in expected
        },
    }


def _query_ros_parameters(
    *,
    controller_name: str,
    parameter_names: list[str],
    timeout_s: float,
) -> dict[str, Any]:
    try:
        import rclpy
        from rclpy.parameter import parameter_value_to_python
        from rclpy.parameter_client import AsyncParameterClient
    except ImportError as exc:
        raise RuntimeError(
            'live controller validation requires a sourced ROS environment'
        ) from exc

    rclpy.init(args=None)
    node = rclpy.create_node(
        'manyforge_xarm_live_controller_parameter_validator'
    )
    try:
        client = AsyncParameterClient(node, controller_name)
        if not client.wait_for_services(timeout_sec=timeout_s):
            raise RuntimeError(
                f'{controller_name} parameter service did not become ready'
            )
        future = client.get_parameters(parameter_names)
        rclpy.spin_until_future_complete(
            node,
            future,
            timeout_sec=timeout_s,
        )
        if not future.done():
            raise RuntimeError(
                f'{controller_name} parameter query timed out'
            )
        response = future.result()
        if response is None or len(response.values) != len(parameter_names):
            raise RuntimeError(
                f'{controller_name} returned an incomplete parameter response'
            )
        return {
            name: parameter_value_to_python(value)
            for name, value in zip(parameter_names, response.values)
        }
    finally:
        node.destroy_node()
        rclpy.shutdown()


def run_live_controller_parameter_validation(
    *,
    runtime_binding_path: Path,
    timeout_s: float,
    query: Callable[..., dict[str, Any]] = _query_ros_parameters,
) -> dict[str, Any]:
    """Load one runtime binding and validate its live controller node."""
    payload = json.loads(runtime_binding_path.read_text(encoding='utf-8'))
    root = _mapping(payload, 'runtime binding')
    if root.get('schema') != RUNTIME_BINDING_SCHEMA:
        raise ValueError(
            f'runtime binding schema must be {RUNTIME_BINDING_SCHEMA}'
        )
    if (
        isinstance(timeout_s, bool)
        or not isinstance(timeout_s, (int, float))
        or not math.isfinite(timeout_s)
        or timeout_s <= 0.0
        or timeout_s > 120.0
    ):
        raise ValueError('timeout_s must be in (0, 120]')
    controller_binding = _mapping(
        root.get('controller_binding'),
        'controller binding',
    )
    controller_name, expected = expected_live_controller_parameters(
        controller_binding
    )
    observed = query(
        controller_name=controller_name,
        parameter_names=list(expected),
        timeout_s=float(timeout_s),
    )
    result = validate_live_controller_parameters(
        controller_binding=controller_binding,
        observed_parameters=observed,
    )
    result['runtimeGenerationId'] = root.get('generation_id')
    return result


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


def main() -> int:
    """CLI entry point."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime-binding', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout-s', type=float, default=10.0)
    args = parser.parse_args()
    output = args.output.resolve()
    try:
        result = run_live_controller_parameter_validation(
            runtime_binding_path=args.runtime_binding.resolve(),
            timeout_s=args.timeout_s,
        )
        status = 0
    except Exception as exc:
        result = {
            'schema': LIVE_CONTROLLER_PARAMETER_VALIDATION_SCHEMA,
            'validatedAt': datetime.now(timezone.utc).isoformat(),
            'result': 'fail',
            'error': str(exc),
        }
        print(str(exc), file=sys.stderr)
        status = 1
    _atomic_text(
        output,
        json.dumps(result, indent=2, sort_keys=True) + '\n',
    )
    print(output)
    return status


if __name__ == '__main__':
    raise SystemExit(main())
