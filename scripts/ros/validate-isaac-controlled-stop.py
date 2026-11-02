#!/usr/bin/env python3
"""ROS-free controlled-stop seam analysis plus an unavailable live runner.

The measurement and verdict logic remains useful qualification behavior: it
reduces timestamped commanded/observed samples without owning Stop.  The old
live probe also constructed and preempted trajectories directly, including a
competing cancel/send strategy.  That command body is deleted; a future I9
probe must observe the single I8 Kernel Stop path.
"""

from __future__ import annotations

import math
from numbers import Real
import statistics
import sys
from dataclasses import asdict, dataclass, field
from itertools import islice
from typing import Any, Iterable, Sequence


UNAVAILABLE_REASON_CODE = "composable_cell_runtime_unavailable"
RESTORATION_GATE = "I8/P5 + I9/I"
UNAVAILABLE_MESSAGE = (
    f"{UNAVAILABLE_REASON_CODE}: live Isaac controlled-stop qualification "
    f"requires the {RESTORATION_GATE} restoration gates"
)
MAX_EVIDENCE_SAMPLES = 4096
MAX_VECTOR_ITEMS = 4096


@dataclass(frozen=True)
class StateSample:
    """One controller-state observation stamped on monotonic receipt."""

    t_mono: float
    ref_pos: tuple[float, ...]
    ref_vel: tuple[float, ...]
    fb_pos: tuple[float, ...]
    fb_vel: tuple[float, ...]


@dataclass(frozen=True)
class SeamThresholds:
    """Acceptance bounds independent of a Stop implementation strategy."""

    min_stop_speed_rad_s: float = 0.01
    max_seam_ratio: float = 4.0
    max_seam_abs_step_rad_s: float = 0.05
    max_backward_reversal_rad_s: float = 0.02


@dataclass(frozen=True)
class CaseResult:
    """Transport-neutral evidence result for one observed Stop."""

    name: str
    passed: bool
    accepted: bool
    details: dict[str, Any] = field(default_factory=dict)
    samples: tuple[dict[str, Any], ...] = ()

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


def _max_reference_velocity_step(samples: Sequence[StateSample]) -> float:
    worst = 0.0
    for previous, current in zip(samples, samples[1:], strict=False):
        if len(previous.ref_vel) != len(current.ref_vel):
            raise ValueError("reference velocity dimensions changed during capture")
        step = max(
            (
                abs(current_value - previous_value)
                for current_value, previous_value in zip(
                    current.ref_vel, previous.ref_vel, strict=True
                )
            ),
            default=0.0,
        )
        worst = max(worst, step)
    return worst


def analyze_reference_seam(
    samples: Sequence[StateSample],
    *,
    seam_time_s: float,
    velocity_at_seam: Sequence[float],
    seam_window_s: float,
    stopped_velocity_tolerance_rad_s: float,
) -> dict[str, Any]:
    """Reduce a bounded sample sequence into implementation-neutral metrics."""

    seam_time_s = _finite_real(seam_time_s, label="seam_time_s")
    seam_window_s = _finite_real(seam_window_s, label="seam_window_s")
    stopped_velocity_tolerance_rad_s = _finite_real(
        stopped_velocity_tolerance_rad_s,
        label="stopped_velocity_tolerance_rad_s",
    )
    if seam_window_s <= 0.0:
        raise ValueError("seam_window_s must be positive")
    if stopped_velocity_tolerance_rad_s < 0.0:
        raise ValueError("stopped velocity tolerance must be non-negative")
    records = list(
        _bounded_items(
            samples,
            label="controlled-stop samples",
            limit=MAX_EVIDENCE_SAMPLES,
        )
    )
    if len(records) < 3:
        return {"error": "insufficient samples", "samples": len(records)}
    seam_velocity = _finite_vector(
        velocity_at_seam,
        label="velocity_at_seam",
    )
    if not seam_velocity:
        raise ValueError("velocity_at_seam must be non-empty")
    previous_time: float | None = None
    for index, sample in enumerate(records):
        if not isinstance(sample, StateSample):
            raise ValueError(f"sample {index} must be a StateSample")
        sample_time = _finite_real(sample.t_mono, label=f"sample {index} t_mono")
        if previous_time is not None and sample_time <= previous_time:
            raise ValueError("sample timestamps must be strictly increasing")
        previous_time = sample_time
        vectors = (
            _finite_vector(sample.ref_pos, label=f"sample {index} ref_pos"),
            _finite_vector(sample.ref_vel, label=f"sample {index} ref_vel"),
            _finite_vector(sample.fb_pos, label=f"sample {index} fb_pos"),
            _finite_vector(sample.fb_vel, label=f"sample {index} fb_vel"),
        )
        if not vectors[0] or len({len(vector) for vector in vectors}) != 1:
            raise ValueError(
                "sample position and velocity vectors must be non-empty and equal in length"
            )
        if len(vectors[1]) != len(seam_velocity):
            raise ValueError("sample velocity dimension differs from seam state")

    pre = [sample for sample in records if sample.t_mono < seam_time_s - 1e-4]
    seam = [
        sample
        for sample in records
        if seam_time_s - seam_window_s
        <= sample.t_mono
        <= seam_time_s + seam_window_s
    ]
    post = [sample for sample in records if sample.t_mono > seam_time_s + 1e-4]

    baseline_step = _max_reference_velocity_step(pre) if len(pre) >= 2 else 0.0
    seam_step = _max_reference_velocity_step(seam) if len(seam) >= 2 else 0.0
    ratio = seam_step / baseline_step if baseline_step > 1e-6 else None

    signs = [1.0 if velocity >= 0.0 else -1.0 for velocity in seam_velocity]
    reversal = 0.0
    for sample in post:
        if len(sample.ref_vel) != len(signs):
            raise ValueError("reference velocity dimension differs from seam state")
        for velocity, sign in zip(sample.ref_vel, signs, strict=True):
            reversal = max(reversal, -velocity * sign)

    settle_s: float | None = None
    for sample in post:
        if max((abs(value) for value in sample.ref_vel), default=0.0) <= (
            stopped_velocity_tolerance_rad_s
        ):
            settle_s = sample.t_mono - seam_time_s
            break

    nearest = min(records, key=lambda sample: abs(sample.t_mono - seam_time_s))
    if len(nearest.ref_pos) != len(nearest.fb_pos):
        raise ValueError("reference and feedback position dimensions differ")
    ref_feedback_gap = max(
        (
            abs(reference - feedback)
            for reference, feedback in zip(
                nearest.ref_pos, nearest.fb_pos, strict=True
            )
        ),
        default=0.0,
    )
    publish_dt = statistics.median(
        current.t_mono - previous.t_mono
        for previous, current in zip(records, records[1:], strict=False)
    )

    return {
        "baseline_ref_vel_step_rad_s": round(baseline_step, 6),
        "seam_ref_vel_step_rad_s": round(seam_step, 6),
        "seam_step_ratio": round(ratio, 3) if ratio is not None else None,
        "backward_reversal_rad_s": round(reversal, 6),
        "settle_time_s": round(settle_s, 4) if settle_s is not None else None,
        "ref_feedback_pos_gap_rad": round(ref_feedback_gap, 6),
        "controller_state_dt_s": round(publish_dt, 5),
        "pre_samples": len(pre),
        "seam_samples": len(seam),
        "post_samples": len(post),
    }


def controlled_stop_evidence_passes(
    *,
    accepted: bool,
    max_speed_at_stop_rad_s: float,
    details: dict[str, Any],
    thresholds: SeamThresholds = SeamThresholds(),
) -> bool:
    """Apply the retained seam/no-reversal acceptance rule."""

    if not isinstance(accepted, bool):
        raise ValueError("accepted must be a boolean")
    max_speed = _finite_nonnegative_real(
        max_speed_at_stop_rad_s, label="max_speed_at_stop_rad_s"
    )
    min_stop_speed = _finite_nonnegative_real(
        thresholds.min_stop_speed_rad_s,
        label="thresholds.min_stop_speed_rad_s",
    )
    max_ratio = _finite_nonnegative_real(
        thresholds.max_seam_ratio,
        label="thresholds.max_seam_ratio",
    )
    max_step = _finite_nonnegative_real(
        thresholds.max_seam_abs_step_rad_s,
        label="thresholds.max_seam_abs_step_rad_s",
    )
    max_reversal = _finite_nonnegative_real(
        thresholds.max_backward_reversal_rad_s,
        label="thresholds.max_backward_reversal_rad_s",
    )
    if not isinstance(details, dict):
        raise ValueError("details must be an object")
    required_metrics = (
        "seam_step_ratio",
        "seam_ref_vel_step_rad_s",
        "backward_reversal_rad_s",
    )
    missing_metrics = [key for key in required_metrics if key not in details]
    if missing_metrics:
        raise ValueError(
            "details missing required controlled-stop metrics: "
            + ", ".join(missing_metrics)
        )
    raw_ratio = details["seam_step_ratio"]
    ratio = (
        None
        if raw_ratio is None
        else _finite_nonnegative_real(raw_ratio, label="details.seam_step_ratio")
    )
    reversal = _finite_nonnegative_real(
        details["backward_reversal_rad_s"],
        label="details.backward_reversal_rad_s",
    )
    step = _finite_nonnegative_real(
        details["seam_ref_vel_step_rad_s"],
        label="details.seam_ref_vel_step_rad_s",
    )
    if not accepted or max_speed < min_stop_speed:
        return False
    ratio_ok = ratio is not None and ratio <= max_ratio
    step_ok = step <= max_step
    reversal_ok = reversal <= max_reversal
    return bool((ratio_ok or step_ok) and reversal_ok)


def _bounded_items(
    values: Iterable[Any], *, label: str, limit: int
) -> tuple[Any, ...]:
    try:
        items = tuple(islice(iter(values), limit + 1))
    except TypeError as exc:
        raise ValueError(f"{label} must be iterable") from exc
    if len(items) > limit:
        raise ValueError(f"{label} exceeds the {limit}-item bound")
    return items


def _finite_real(value: Any, *, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, Real):
        raise ValueError(f"{label} must be a real number, not a coercible value")
    result = float(value)
    if not math.isfinite(result):
        raise ValueError(f"{label} must be finite")
    return result


def _finite_nonnegative_real(value: Any, *, label: str) -> float:
    result = _finite_real(value, label=label)
    if result < 0.0:
        raise ValueError(f"{label} must be non-negative")
    return result


def _finite_vector(values: Iterable[Any], *, label: str) -> tuple[float, ...]:
    bounded = _bounded_items(values, label=label, limit=MAX_VECTOR_ITEMS)
    return tuple(
        _finite_real(value, label=f"{label}[{index}]")
        for index, value in enumerate(bounded)
    )


def main(argv: Sequence[str] | None = None) -> int:
    """Fail before ROS discovery, command construction, or Stop effects."""

    del argv
    print(UNAVAILABLE_MESSAGE, file=sys.stderr)
    return 78


if __name__ == "__main__":
    raise SystemExit(main())
