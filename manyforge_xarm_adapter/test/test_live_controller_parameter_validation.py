"""Pure controller validation and live-entrypoint disposition tests."""

from __future__ import annotations

from datetime import datetime, timezone

from manyforge_xarm_adapter import live_controller_parameter_validation as subject
import pytest


HISTORICAL_TEST_BLOB = "c20a1e8e97339026baa437df9502ad1b04498a90"
HISTORICAL_CASE_DISPOSITION = {
    "test_projects_and_validates_exact_live_parameters": ("retained", "I10/R"),
    "test_rejects_live_parameter_drift": ("retained", "I10/R"),
    "test_runtime_query_uses_retained_controller_contract": ("I10/R",),
}


def _controller_binding() -> dict[str, object]:
    joints = [f"joint{index}" for index in range(1, 7)]
    return {
        "generation_id": "a" * 64,
        "trajectory_controller_name": "/lite6_traj_controller",
        "arm_joint_names": joints,
        "controller_state_interfaces": ["position", "velocity"],
        "decelerate_on_cancel": True,
        "max_deceleration_on_cancel_rad_s2": {
            joint: 2.0 for joint in joints
        },
    }


def test_projects_and_validates_exact_supplied_parameters() -> None:
    binding = _controller_binding()
    controller_name, expected = subject.expected_live_controller_parameters(
        binding
    )

    result = subject.validate_live_controller_parameters(
        controller_binding=binding,
        observed_parameters=dict(expected),
        validated_at=datetime(2026, 7, 31, tzinfo=timezone.utc),
    )

    assert controller_name == "/lite6_traj_controller"
    assert result["result"] == "pass"
    assert result["validatedAt"] == "2026-07-31T00:00:00+00:00"
    assert result["expectedParameters"] == expected
    assert result["observedParameters"] == expected


@pytest.mark.parametrize(
    ("parameter", "value"),
    [
        ("constraints.decelerate_on_cancel", False),
        ("constraints.joint6.max_deceleration_on_cancel", 0.0),
    ],
)
def test_rejects_supplied_parameter_drift(
    parameter: str,
    value: object,
) -> None:
    binding = _controller_binding()
    _, observed = subject.expected_live_controller_parameters(binding)
    observed[parameter] = value

    with pytest.raises(ValueError, match=parameter):
        subject.validate_live_controller_parameters(
            controller_binding=binding,
            observed_parameters=observed,
            validated_at=datetime(2026, 7, 31, tzinfo=timezone.utc),
        )


def test_live_query_and_writer_are_replaced_by_the_i10_gate(capsys) -> None:
    assert subject.main() == 78
    assert capsys.readouterr().err.strip() == subject.UNAVAILABLE_MESSAGE
    assert not hasattr(subject, "run_live_controller_parameter_validation")
    assert not hasattr(subject, "_query_ros_parameters")


def test_source_and_all_three_historical_cohorts_remain_assigned() -> None:
    assert subject.HISTORICAL_SOURCE_BLOB == (
        "c9c5a5be68df40bcd023ea6cd3ffc71788e0e9db"
    )
    assert HISTORICAL_TEST_BLOB == (
        "c20a1e8e97339026baa437df9502ad1b04498a90"
    )
    assert len(HISTORICAL_CASE_DISPOSITION) == 3
    assert all("I10/R" in gates for gates in HISTORICAL_CASE_DISPOSITION.values())
