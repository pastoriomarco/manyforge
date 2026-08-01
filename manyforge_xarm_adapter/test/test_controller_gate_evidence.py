"""Pure pose reduction and old controller/gate disposition tests."""

from __future__ import annotations

import math

from manyforge_xarm_adapter import controller_gate_evidence as subject
import pytest


HISTORICAL_TEST_BLOB = "3f425fd9f180c07009a9c00d4205b8537e274688"
HISTORICAL_CASE_DISPOSITION = {
    "test_controller_gate_capture_passes_exact_no_trajectory_sequence": (
        "I7/L0",
        "I8/P5",
        "I10/R",
    ),
    "test_controller_gate_capture_fails_pose_limit_but_still_closes": (
        "retained_reducer",
        "I8/P5",
        "I10/R",
    ),
    "test_controller_gate_capture_requires_exact_confirmation": (
        "I7/L0",
        "I8/P5",
        "I10/R",
    ),
    "test_joint_state_message_dependency_is_installed": ("I10/R",),
}


def _sample(position: float) -> subject.JointSample:
    return subject.JointSample(received_at_ns=1, positions=(position,) * 6)


def test_pose_deviation_reducer_preserves_the_explicit_bound_input() -> None:
    assert subject.max_pose_deviation([_sample(0.0), _sample(0.0002)]) == (
        pytest.approx(0.0002)
    )
    assert subject.max_pose_deviation([]) == math.inf


@pytest.mark.parametrize(
    "samples",
    [
        [subject.JointSample(1, (0.0,)), subject.JointSample(2, (0.0, 1.0))],
        [subject.JointSample(1, (math.nan,))],
    ],
)
def test_pose_deviation_reducer_rejects_malformed_samples(
    samples: list[subject.JointSample],
) -> None:
    with pytest.raises(ValueError):
        subject.max_pose_deviation(samples)


def test_old_ros_gate_runner_is_unavailable_before_effects(capsys) -> None:
    assert subject.main() == 78
    assert capsys.readouterr().err.strip() == subject.UNAVAILABLE_MESSAGE
    assert not hasattr(subject, "capture_controller_gate_evidence")
    assert not hasattr(subject, "_RosCapture")


def test_source_and_all_four_historical_cohorts_remain_pinned() -> None:
    assert subject.HISTORICAL_SOURCE_BLOB == (
        "84d16253b1a9b0f4925a70e5b00eedd2cee1df2a"
    )
    assert HISTORICAL_TEST_BLOB == (
        "3f425fd9f180c07009a9c00d4205b8537e274688"
    )
    assert len(HISTORICAL_CASE_DISPOSITION) == 4
    assert all("I10/R" in gates for gates in HISTORICAL_CASE_DISPOSITION.values())
