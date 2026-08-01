#!/usr/bin/env python3
"""Pure visual-observer evidence scoring plus the I9/I live-probe gate.

The complete historical probe is preserved at
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` (blob
``e14ef71ce3e4c1416cb7b29cb0d7bf541ebadc3f``):

* lines 25-68 and 189-330 supplied the sample value, scalar normalization,
  percentile, summary, and budget checks retained here with explicit bounds;
* lines 70-187 translated the old snake/camel ``/api/scene/state`` response
  and performed HTTP sampling.  That route adapter is an I9/I restoration
  target and is intentionally absent; and
* lines 332-440 parsed the old CLI, sampled its route on a clock, and wrote
  JSON/JSONL evidence.  Live collection and file output also return at I9/I.

``Sample`` now means an already-normalized, source-owned observer record.  No
function in this module discovers a route, fetches Composer state, reads a
clock, or writes evidence.  Value scoring alone is not live qualification.
"""

from __future__ import annotations

from collections import Counter
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
import math
import sys
from typing import Any, Final, NoReturn


RUNTIME_UNAVAILABLE_REASON_CODE: Final = "composable_cell_runtime_unavailable"
VISUAL_PROBE_RESTORATION_GATES: Final = ("I9", "I")
MAX_VISUAL_PROBE_SAMPLES: Final = 1_000_000
MAX_SAMPLE_TEXT_CHARS: Final = 4_096

DEFAULT_SNAPSHOT_MODES: Final = frozenset(
    {"visual_snapshot", "visual_snapshot_pending"}
)
DEFAULT_PENDING_MODES: Final = frozenset({"visual_snapshot_pending"})
DEFAULT_LEGACY_MODES: Final = frozenset({"legacy_live"})


class VisualProbeUnavailableError(RuntimeError):
    """Raised before a route, runtime, clock, or evidence file is touched."""


class VisualProbeBoundsError(ValueError):
    """Raised when value-only probe evidence is not finitely bounded."""


def _raise_visual_probe_unavailable() -> NoReturn:
    raise VisualProbeUnavailableError(
        f"{RUNTIME_UNAVAILABLE_REASON_CODE}: the observer-owned live visual "
        "qualification probe returns at I9/I"
    )


def _required_nonnegative_float(value: object, *, field_name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise TypeError(f"{field_name} must be a finite number")
    parsed = float(value)
    if not math.isfinite(parsed) or parsed < 0.0:
        raise ValueError(f"{field_name} must be finite and non-negative")
    return parsed


def _validate_optional_nonnegative_float(
    value: object,
    *,
    field_name: str,
) -> None:
    if value is not None:
        _required_nonnegative_float(value, field_name=field_name)


@dataclass(slots=True)
class Sample:
    """One already-normalized observer sample; never a route response DTO."""

    index: int
    ok: bool
    latency_ms: float
    mode: str = "error"
    served_from_visual_snapshot: bool = False
    fallback_reason: str | None = None
    total_ms: float | None = None
    snapshot_age_ms: float | None = None
    snapshot_sequence_number: int | None = None
    snapshot_build_ms: float | None = None
    snapshot_stale: bool = False
    snapshot_publisher_running: bool = False
    snapshot_publisher_error: str | None = None
    objects_source: str | None = None
    object_count: int | None = None
    gripper_count: int | None = None
    pose_source_count: int | None = None
    fresh_pose_source_count: int | None = None
    error: str | None = None

    def __post_init__(self) -> None:
        if isinstance(self.index, bool) or not isinstance(self.index, int):
            raise TypeError("index must be an integer")
        if self.index < 0:
            raise ValueError("index must be non-negative")
        if not isinstance(self.ok, bool):
            raise TypeError("ok must be a boolean")
        _required_nonnegative_float(self.latency_ms, field_name="latency_ms")
        if not isinstance(self.mode, str):
            raise TypeError("mode must be a string")
        if not self.mode or len(self.mode) > MAX_SAMPLE_TEXT_CHARS:
            raise ValueError(
                f"mode must contain 1..{MAX_SAMPLE_TEXT_CHARS} characters"
            )
        for field_name in (
            "served_from_visual_snapshot",
            "snapshot_stale",
            "snapshot_publisher_running",
        ):
            if not isinstance(getattr(self, field_name), bool):
                raise TypeError(f"{field_name} must be a boolean")
        for field_name in (
            "total_ms",
            "snapshot_age_ms",
            "snapshot_build_ms",
        ):
            _validate_optional_nonnegative_float(
                getattr(self, field_name),
                field_name=field_name,
            )
        for field_name in (
            "snapshot_sequence_number",
            "object_count",
            "gripper_count",
            "pose_source_count",
            "fresh_pose_source_count",
        ):
            value = getattr(self, field_name)
            if value is None:
                continue
            if isinstance(value, bool) or not isinstance(value, int):
                raise TypeError(f"{field_name} must be an integer or None")
            if value < 0:
                raise ValueError(f"{field_name} must be non-negative")
        for field_name in (
            "fallback_reason",
            "snapshot_publisher_error",
            "objects_source",
            "error",
        ):
            value = getattr(self, field_name)
            if value is not None:
                if not isinstance(value, str):
                    raise TypeError(f"{field_name} must be a string or None")
                if len(value) > MAX_SAMPLE_TEXT_CHARS:
                    raise ValueError(
                        f"{field_name} exceeds {MAX_SAMPLE_TEXT_CHARS} characters"
                    )

    def to_json(self) -> dict[str, Any]:
        """Project the normalized sample into an artifact-ready mapping."""

        return {
            "index": self.index,
            "ok": self.ok,
            "latency_ms": round(self.latency_ms, 3),
            "mode": self.mode,
            "served_from_visual_snapshot": self.served_from_visual_snapshot,
            "fallback_reason": self.fallback_reason,
            "total_ms": self.total_ms,
            "snapshot_age_ms": self.snapshot_age_ms,
            "snapshot_sequence_number": self.snapshot_sequence_number,
            "snapshot_build_ms": self.snapshot_build_ms,
            "snapshot_stale": self.snapshot_stale,
            "snapshot_publisher_running": self.snapshot_publisher_running,
            "snapshot_publisher_error": self.snapshot_publisher_error,
            "objects_source": self.objects_source,
            "object_count": self.object_count,
            "gripper_count": self.gripper_count,
            "pose_source_count": self.pose_source_count,
            "fresh_pose_source_count": self.fresh_pose_source_count,
            "error": self.error,
        }


def _optional_float(value: Any) -> float | None:
    if value is None:
        return None
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return None
    if not math.isfinite(parsed):
        return None
    return parsed


def _optional_int(value: Any) -> int | None:
    if value is None:
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _percentile(values: Sequence[float], percentile: float) -> float | None:
    if len(values) > MAX_VISUAL_PROBE_SAMPLES:
        raise VisualProbeBoundsError(
            f"values exceeds {MAX_VISUAL_PROBE_SAMPLES} entries"
        )
    percentile_value = _required_nonnegative_float(
        percentile,
        field_name="percentile",
    )
    if percentile_value > 1.0:
        raise ValueError("percentile must be in [0, 1]")
    if not values:
        return None
    ordered = sorted(
        _required_nonnegative_float(value, field_name="values entry")
        for value in values
    )
    if len(ordered) == 1:
        return ordered[0]
    rank = (len(ordered) - 1) * percentile_value
    lower = math.floor(rank)
    upper = math.ceil(rank)
    if lower == upper:
        return ordered[lower]
    fraction = rank - lower
    return ordered[lower] + (ordered[upper] - ordered[lower]) * fraction


def _build_summary(
    samples: Sequence[Sample],
    *,
    snapshot_modes: frozenset[str] = DEFAULT_SNAPSHOT_MODES,
    pending_modes: frozenset[str] = DEFAULT_PENDING_MODES,
    legacy_modes: frozenset[str] = DEFAULT_LEGACY_MODES,
) -> dict[str, Any]:
    """Aggregate normalized samples without translating a live wire payload."""

    if isinstance(samples, (str, bytes)):
        raise TypeError("samples must be a sequence of Sample values")
    if len(samples) > MAX_VISUAL_PROBE_SAMPLES:
        raise VisualProbeBoundsError(
            f"samples exceeds {MAX_VISUAL_PROBE_SAMPLES} entries"
        )
    if not all(isinstance(sample, Sample) for sample in samples):
        raise TypeError("samples must contain only Sample values")
    ok_samples = [sample for sample in samples if sample.ok]
    latencies = [sample.latency_ms for sample in ok_samples]
    mode_counts = Counter(sample.mode for sample in ok_samples)
    fallback_counts = Counter(
        sample.fallback_reason or "none" for sample in ok_samples
    )
    publisher_errors = Counter(
        sample.snapshot_publisher_error
        for sample in ok_samples
        if sample.snapshot_publisher_error
    )
    stale_count = sum(1 for sample in ok_samples if sample.snapshot_stale)
    pending_count = sum(
        1 for sample in ok_samples if sample.mode in pending_modes
    )
    legacy_count = sum(1 for sample in ok_samples if sample.mode in legacy_modes)
    non_snapshot_count = sum(
        1 for sample in ok_samples if sample.mode not in snapshot_modes
    )
    ages = [
        sample.snapshot_age_ms
        for sample in ok_samples
        if sample.snapshot_age_ms is not None
    ]
    return {
        "sample_count": len(samples),
        "ok_count": len(ok_samples),
        "error_count": len(samples) - len(ok_samples),
        "mode_counts": dict(sorted(mode_counts.items())),
        "fallback_counts": dict(sorted(fallback_counts.items())),
        "publisher_errors": dict(sorted(publisher_errors.items())),
        "stale_count": stale_count,
        "pending_count": pending_count,
        "legacy_count": legacy_count,
        "non_snapshot_count": non_snapshot_count,
        "stale_ratio": (stale_count / len(ok_samples)) if ok_samples else None,
        "pending_ratio": (pending_count / len(ok_samples)) if ok_samples else None,
        "latency_ms": {
            "min": min(latencies, default=None),
            "p50": _percentile(latencies, 0.50),
            "p95": _percentile(latencies, 0.95),
            "p99": _percentile(latencies, 0.99),
            "max": max(latencies, default=None),
        },
        "snapshot_age_ms": {
            "p95": _percentile(ages, 0.95),
            "max": max(ages, default=None),
        },
    }


def _evaluate(
    summary: Mapping[str, Any],
    *,
    max_error_count: int,
    max_legacy_count: int,
    max_non_snapshot_count: int,
    max_p95_ms: float,
    max_stale_ratio: float,
    max_pending_ratio: float,
) -> list[str]:
    """Apply latency, staleness, fallback, and publisher-health budgets."""

    failures: list[str] = []
    if int(summary.get("ok_count", 0)) <= 0:
        failures.append("no successful visual-observer samples")
        return failures
    error_count = int(summary.get("error_count", 0))
    if error_count > max_error_count:
        failures.append(f"observer errors {error_count} > budget {max_error_count}")
    legacy_count = int(summary.get("legacy_count", 0))
    if legacy_count > max_legacy_count:
        failures.append(
            f"legacy-mode samples {legacy_count} > budget {max_legacy_count}"
        )
    non_snapshot_count = int(summary.get("non_snapshot_count", 0))
    if non_snapshot_count > max_non_snapshot_count:
        failures.append(
            "non-snapshot samples "
            f"{non_snapshot_count} > budget {max_non_snapshot_count}"
        )
    publisher_errors = summary.get("publisher_errors", {})
    if publisher_errors:
        failures.append(f"publisher errors observed: {publisher_errors}")
    latency = summary.get("latency_ms", {})
    p95 = latency.get("p95") if isinstance(latency, Mapping) else None
    if p95 is not None and p95 > max_p95_ms:
        failures.append(
            f"visual-observer p95 latency {p95:.1f} ms > budget {max_p95_ms:.1f} ms"
        )
    stale_ratio = summary.get("stale_ratio")
    if stale_ratio is not None and stale_ratio > max_stale_ratio:
        failures.append(
            f"snapshot stale ratio {stale_ratio:.3f} > budget {max_stale_ratio:.3f}"
        )
    pending_ratio = summary.get("pending_ratio")
    if pending_ratio is not None and pending_ratio > max_pending_ratio:
        failures.append(
            f"snapshot pending ratio {pending_ratio:.3f} > budget {max_pending_ratio:.3f}"
        )
    return failures


def require_visual_probe(
    *_args: object,
    **_kwargs: object,
) -> NoReturn:
    """Typed gate for callers that would perform live visual observation."""

    _raise_visual_probe_unavailable()


def main(_argv: list[str] | None = None) -> int:
    print(
        f"{RUNTIME_UNAVAILABLE_REASON_CODE}: historical live visual-state "
        "collection returns at I9/I",
        file=sys.stderr,
    )
    return 78


if __name__ == "__main__":
    raise SystemExit(main())


__all__ = [
    "DEFAULT_LEGACY_MODES",
    "DEFAULT_PENDING_MODES",
    "DEFAULT_SNAPSHOT_MODES",
    "RUNTIME_UNAVAILABLE_REASON_CODE",
    "Sample",
    "VISUAL_PROBE_RESTORATION_GATES",
    "VisualProbeBoundsError",
    "VisualProbeUnavailableError",
    "require_visual_probe",
]
