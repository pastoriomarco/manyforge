"""Pure reduction tests for lifecycle transient evidence."""

from __future__ import annotations

from manyforge_xarm_adapter.lifecycle_transient import summarize_transient
from manyforge_xarm_adapter.lifecycle_transient import TransientJointSample


def _sample(
    received_at_ns: int,
    position: float,
    *,
    qualified: bool,
) -> TransientJointSample:
    return TransientJointSample(
        received_at_steady_ns=received_at_ns,
        positions=(position,) * 6,
        state_generation=1,
        position_valid=qualified,
        brake_mask=63 if qualified else 0,
        servo_enable_mask=63 if qualified else 0,
    )


def test_invalid_zero_initialization_is_not_treated_as_motion() -> None:
    summary = summarize_transient(
        samples=[
            _sample(900, 0.0, qualified=False),
            _sample(1100, 0.0, qualified=False),
            _sample(1200, 1.0, qualified=True),
            _sample(1300, 1.0002, qualified=True),
        ],
        command_started_ns=1000,
        command_finished_ns=1050,
        joint_count=6,
        capture_window_ms=3000,
        max_deviation_rad=None,
    )

    assert summary['capture_complete'] is True
    assert summary['first_post_command_positions_rad'] == [0.0] * 6
    assert summary['first_qualified_positions_rad'] == [1.0] * 6
    assert summary['max_qualified_pose_deviation_rad'] == (
        0.00019999999999997797
    )
    assert summary['assessment'] == (
        'captured_requires_bound_adjudication'
    )


def test_explicit_transient_bound_is_enforced() -> None:
    summary = summarize_transient(
        samples=[
            _sample(1100, 1.0, qualified=True),
            _sample(1200, 1.01, qualified=True),
        ],
        command_started_ns=1000,
        command_finished_ns=1050,
        joint_count=6,
        capture_window_ms=3000,
        max_deviation_rad=0.001,
    )

    assert summary['capture_complete'] is True
    assert summary['within_explicit_bound'] is False
    assert summary['assessment'] == 'exceeds_explicit_bound'
