from __future__ import annotations

import json
from pathlib import Path

from manyforge_xarm_adapter.live_controller_parameter_validation import (
    expected_live_controller_parameters,
)
from manyforge_xarm_adapter.live_controller_parameter_validation import (
    run_live_controller_parameter_validation,
)
from manyforge_xarm_adapter.live_controller_parameter_validation import (
    validate_live_controller_parameters,
)
from manyforge_xarm_adapter.supervised_runtime_binding import (
    RUNTIME_BINDING_SCHEMA,
)
import pytest


def _controller_binding() -> dict[str, object]:
    joints = [f'joint{index}' for index in range(1, 7)]
    return {
        'generation_id': 'a' * 64,
        'trajectory_controller_name': '/lite6_traj_controller',
        'arm_joint_names': joints,
        'controller_state_interfaces': ['position', 'velocity'],
        'decelerate_on_cancel': True,
        'max_deceleration_on_cancel_rad_s2': {
            joint: 2.0 for joint in joints
        },
    }


def test_projects_and_validates_exact_live_parameters() -> None:
    binding = _controller_binding()
    controller_name, expected = expected_live_controller_parameters(
        binding
    )

    result = validate_live_controller_parameters(
        controller_binding=binding,
        observed_parameters=dict(expected),
    )

    assert controller_name == '/lite6_traj_controller'
    assert result['result'] == 'pass'
    assert result['expectedParameters'] == expected
    assert result['observedParameters'] == expected


@pytest.mark.parametrize(
    ('parameter', 'value'),
    [
        ('constraints.decelerate_on_cancel', False),
        ('constraints.joint6.max_deceleration_on_cancel', 0.0),
    ],
)
def test_rejects_live_parameter_drift(
    parameter: str,
    value: object,
) -> None:
    binding = _controller_binding()
    _, observed = expected_live_controller_parameters(binding)
    observed[parameter] = value

    with pytest.raises(ValueError, match=parameter):
        validate_live_controller_parameters(
            controller_binding=binding,
            observed_parameters=observed,
        )


def test_runtime_query_uses_retained_controller_contract(
    tmp_path: Path,
) -> None:
    binding = _controller_binding()
    runtime_path = tmp_path / 'runtime-binding.json'
    runtime_path.write_text(
        json.dumps(
            {
                'schema': RUNTIME_BINDING_SCHEMA,
                'generation_id': 'b' * 64,
                'controller_binding': binding,
            }
        ),
        encoding='utf-8',
    )
    calls: list[dict[str, object]] = []

    def query(**kwargs):
        calls.append(kwargs)
        _, expected = expected_live_controller_parameters(binding)
        return expected

    result = run_live_controller_parameter_validation(
        runtime_binding_path=runtime_path,
        timeout_s=12.0,
        query=query,
    )

    assert result['runtimeGenerationId'] == 'b' * 64
    assert calls == [
        {
            'controller_name': '/lite6_traj_controller',
            'parameter_names': [
                'constraints.decelerate_on_cancel',
                *[
                    (
                        f'constraints.joint{index}.'
                        'max_deceleration_on_cancel'
                    )
                    for index in range(1, 7)
                ],
            ],
            'timeout_s': 12.0,
        }
    ]
