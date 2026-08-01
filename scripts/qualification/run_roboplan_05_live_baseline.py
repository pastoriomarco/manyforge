#!/usr/bin/env python3
"""Relocation gate and pure evidence reducers for the historical RP5 live lane.

The complete collector is pinned at
manyforge@08222188293df8e12f90dc72e204c21f22f92ea1. It depended on the
removed Composer runtime/scene owners, the pre-cutover Kernel/Stop composition,
and the Isaac product lane. Recreate those interactions only through their new
resource-addressed owners at the assignments below.
"""

from __future__ import annotations

import json
import math
import statistics
import sys
from numbers import Real
from typing import Any, NamedTuple, Sequence


UNAVAILABLE_EXIT_CODE = 78
HISTORICAL_SOURCE = "manyforge@08222188293df8e12f90dc72e204c21f22f92ea1"
HISTORICAL_SOURCE_BLOB = "5d31237c04a5c3266a50572159921eeb515aa385"
HISTORICAL_TEST_BLOB = "0935aa3246f28dc7457706ae37db386759cf415d"
MAX_LATENCY_SAMPLES = 100_000
MAX_LATENCY_NS = (1 << 63) - 1


class RestorationAssignment(NamedTuple):
    historical_case: str
    historical_source_lines: str
    restoration_gate: str
    required_behavior: str


RESTORATION_ASSIGNMENTS = (
    RestorationAssignment(
        "kernel_lifecycle_and_planner_event_capture",
        "historical source blob lines 179-471, 604-1664, and 1744-2185",
        "I5/H0+I6/N",
        "consume headless lifecycle and source-owned planning facts by resource "
        "identity",
    ),
    RestorationAssignment(
        "native_planning_and_authoritative_validation",
        "historical source blob lines 894-1664 and 1744-2185",
        "I6/N+I8/P5",
        "measure the restored native plan/validate/execute path through one Kernel",
    ),
    RestorationAssignment(
        "runtime_collision_monitor_and_lookahead_metrics",
        "historical source blob lines 232-470, 1164-1605, and 1744-2185",
        "I8/P5",
        "consume scoped Stop and multi-domain runtime evidence from the final owner",
    ),
    RestorationAssignment(
        "isaac_reset_drop_execution_and_controller_progress",
        "historical source blob lines 179-471, 994-1162, and 1744-2185",
        "I9/I",
        "run the registered Isaac product through its manager-owned simulator "
        "lifecycle",
    ),
    RestorationAssignment(
        "viewer_disconnected_and_focused_strata",
        "historical source blob lines 658-893 and 1744-2185",
        "I9/I",
        "observe the restored Isaac viewer as a client without giving it runtime "
        "authority",
    ),
    RestorationAssignment(
        "support_contact_live_mode_campaign",
        "historical source blob lines 673-875, 1164-1561, and 1744-2185",
        "I8/P5+I9/I",
        "replay binary, signed, and bounded contact modes on owner-issued scene "
        "products",
    ),
    RestorationAssignment(
        "source_bound_success_and_failure_evidence",
        "retained source blob lines 473-500; producer lines 1666-2307 removed",
        "retained/offline-verifiers",
        "retain schema, latency reducers, and immutable evidence validation "
        "without a live run",
    ),
)


def _bounded_latency_values(
    values: Sequence[int], *, require_sorted: bool
) -> list[int]:
    if isinstance(values, (str, bytes, bytearray)) or not isinstance(
        values, Sequence
    ):
        raise TypeError("latency samples must be a sequence")
    if not values:
        raise ValueError("latency samples must be non-empty")
    if len(values) > MAX_LATENCY_SAMPLES:
        raise ValueError(
            f"latency samples exceed the {MAX_LATENCY_SAMPLES}-sample bound"
        )

    checked: list[int] = []
    previous: int | None = None
    for index, value in enumerate(values):
        if type(value) is not int:
            raise TypeError(f"latency sample {index} must be an exact integer")
        if value < 0 or value > MAX_LATENCY_NS:
            raise ValueError(
                f"latency sample {index} must be within the non-negative "
                "signed 64-bit nanosecond bound"
            )
        if require_sorted and previous is not None and value < previous:
            raise ValueError("percentile input must be sorted in ascending order")
        checked.append(value)
        previous = value
    return checked


def _percentile(sorted_values: Sequence[int], percentile: Real) -> float:
    """Retained interpolation used by the historical evidence envelope."""

    checked = _bounded_latency_values(sorted_values, require_sorted=True)
    if isinstance(percentile, bool) or not isinstance(percentile, Real):
        raise TypeError("percentile must be a real number, not bool")
    try:
        percentile_value = float(percentile)
    except (OverflowError, ValueError) as exc:
        raise ValueError("percentile must be finite and in [0, 1]") from exc
    if not math.isfinite(percentile_value) or not 0.0 <= percentile_value <= 1.0:
        raise ValueError("percentile must be finite and in [0, 1]")

    position = (len(checked) - 1) * percentile_value
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return float(checked[lower])
    weight = position - lower
    return float(
        checked[lower] * (1.0 - weight) + checked[upper] * weight
    )


def _summary(
    values: Sequence[int], *, include_p99: bool = False
) -> dict[str, Any]:
    """Summarize already-captured non-negative latency samples."""

    if type(include_p99) is not bool:
        raise TypeError("include_p99 must be an exact bool")
    ordered = sorted(_bounded_latency_values(values, require_sorted=False))
    median = statistics.median(ordered)
    deviations = sorted(abs(value - median) for value in ordered)
    return {
        "p50": _percentile(ordered, 0.50),
        "p95": _percentile(ordered, 0.95),
        "p99": _percentile(ordered, 0.99) if include_p99 else None,
        "max": float(ordered[-1]),
        "mad": float(statistics.median(deviations)),
    }


def unavailable_payload() -> dict[str, Any]:
    return {
        "schema_version": "manyforge.qualification_unavailable.v1",
        "script": "run_roboplan_05_live_baseline.py",
        "historical_source": HISTORICAL_SOURCE,
        "exit_code": UNAVAILABLE_EXIT_CODE,
        "assignments": [
            assignment._asdict() for assignment in RESTORATION_ASSIGNMENTS
        ],
        "historical_source_blob": HISTORICAL_SOURCE_BLOB,
        "historical_test_blob": HISTORICAL_TEST_BLOB,
    }


def main(argv: list[str] | None = None) -> int:
    del argv
    print(json.dumps(unavailable_payload(), sort_keys=True), file=sys.stderr)
    return UNAVAILABLE_EXIT_CODE


if __name__ == "__main__":
    raise SystemExit(main())
