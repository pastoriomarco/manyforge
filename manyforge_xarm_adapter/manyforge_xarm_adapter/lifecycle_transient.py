"""Pure reduction of bounded joint/state samples around lifecycle evidence.

The ROS sampler from historical source blob
``cd4a2a765ac484dd80d0d797c3c691ab297654e8`` depended on the superseded
``SupervisedLifecycleState`` owner and was reachable only from the removed
attended-command runner.  Its subscriptions, executor thread, waits, and
teardown are assigned to the selected I10/R measured-state/evidence adapter.

The effect-free sample DTO and reducer remain because they express same-role
acceptance behavior: invalid zero initialization is never treated as pose,
and an explicit transient deviation bound fails closed.  Their two exact
historical cases are retained in test blob
``2532213557764ac05823bd97d22a5f3f85d88ec7``.
"""

from __future__ import annotations

from dataclasses import dataclass
import statistics
from typing import Final


HISTORICAL_SOURCE_BLOB: Final = (
    "cd4a2a765ac484dd80d0d797c3c691ab297654e8"
)


@dataclass(frozen=True)
class TransientJointSample:
    """One ordered joint sample with contemporaneous observed-state facts."""

    received_at_steady_ns: int
    positions: tuple[float, ...]
    state_generation: int
    position_valid: bool
    brake_mask: int
    servo_enable_mask: int


def summarize_transient(
    *,
    samples: list[TransientJointSample],
    command_started_ns: int,
    command_finished_ns: int,
    joint_count: int,
    capture_window_ms: int,
    max_deviation_rad: float | None,
) -> dict[str, object]:
    """Reduce raw samples without treating invalid zeroes as measured pose."""

    expected_mask = (1 << joint_count) - 1
    post_samples = [
        sample
        for sample in samples
        if sample.received_at_steady_ns >= command_started_ns
    ]
    qualified = [
        sample
        for sample in post_samples
        if (
            sample.position_valid
            and sample.brake_mask & expected_mask == expected_mask
            and sample.servo_enable_mask & expected_mask == expected_mask
        )
    ]
    max_deviation = None
    if qualified:
        anchor = qualified[0].positions
        max_deviation = max(
            abs(value - anchor[index])
            for sample in qualified
            for index, value in enumerate(sample.positions)
        )
    interarrival_ms = [
        (
            samples[index].received_at_steady_ns
            - samples[index - 1].received_at_steady_ns
        )
        / 1_000_000.0
        for index in range(1, len(samples))
        if (
            samples[index].received_at_steady_ns
            > samples[index - 1].received_at_steady_ns
        )
    ]
    capture_complete = len(post_samples) >= 2 and len(qualified) >= 2
    within_bound = (
        max_deviation is not None
        and max_deviation_rad is not None
        and max_deviation <= max_deviation_rad
    )
    if not capture_complete:
        assessment = "capture_incomplete"
    elif max_deviation_rad is None:
        assessment = "captured_requires_bound_adjudication"
    elif within_bound:
        assessment = "within_explicit_bound"
    else:
        assessment = "exceeds_explicit_bound"
    first_qualified_delay_ms = (
        (qualified[0].received_at_steady_ns - command_started_ns)
        / 1_000_000.0
        if qualified
        else None
    )
    return {
        "capture_window_ms": capture_window_ms,
        "sample_count": len(samples),
        "post_command_sample_count": len(post_samples),
        "qualified_post_command_sample_count": len(qualified),
        "command_duration_ms": (
            command_finished_ns - command_started_ns
        )
        / 1_000_000.0,
        "first_qualified_delay_ms": first_qualified_delay_ms,
        "median_sample_interarrival_ms": (
            statistics.median(interarrival_ms) if interarrival_ms else None
        ),
        "first_post_command_positions_rad": (
            list(post_samples[0].positions) if post_samples else None
        ),
        "first_qualified_positions_rad": (
            list(qualified[0].positions) if qualified else None
        ),
        "last_qualified_positions_rad": (
            list(qualified[-1].positions) if qualified else None
        ),
        "max_qualified_pose_deviation_rad": max_deviation,
        "max_pose_deviation_limit_rad": max_deviation_rad,
        "capture_complete": capture_complete,
        "within_explicit_bound": (
            within_bound if max_deviation_rad is not None else None
        ),
        "assessment": assessment,
    }
