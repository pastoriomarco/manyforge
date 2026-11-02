#!/usr/bin/env python3
"""Retain bounded Lite6 commissioning math behind an I10/R client gate.

The historical script combined useful, transport-neutral safety checks with
removed Composer status and candidate endpoints.  This file now retains only
the effect-free checks: a six-joint observation must be finite and fresh, and
one preview delta must be finite, non-zero, and at most one degree.  Joint
names come from the future compiled binding, so this helper does not recreate
the old singleton ``joint1`` ... ``joint6`` addressing contract.

Direct execution is unavailable before argument parsing.  The future thin
client must bind these checks to the I6-I8 caller-neutral status/admission
contracts and pass the I10/R fake/offline and attended acceptance rows.

Exact historical source: ManyForge
``08222188293df8e12f90dc72e204c21f22f92ea1``, blob
``1bb14aa35ed0b1c2ab7909dff8be86f016179754``.  Its old HTTP adapter,
Composer DTO preconditions, candidate request/response, and preview-evidence
writer remain recoverable there as reimplementation input, not live fallback.
"""

from __future__ import annotations

from dataclasses import dataclass
import math
import sys
from typing import Final, Sequence


LITE6_COMMISSIONING_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_lite6_commissioning_unavailable"
)
HISTORICAL_SOURCE_COMMIT: Final = (
    "08222188293df8e12f90dc72e204c21f22f92ea1"
)
HISTORICAL_SOURCE_BLOB: Final = (
    "1bb14aa35ed0b1c2ab7909dff8be86f016179754"
)
EXPECTED_LITE6_JOINT_COUNT: Final = 6
DEFAULT_MAX_SOURCE_AGE_MS: Final = 250.0
MAX_ABS_DELTA_DEG: Final = 1.0
_MAX_JOINT_NAME_CHARS: Final = 256


class CommissioningRefusal(ValueError):
    """A bounded, effect-free commissioning precondition failed."""


@dataclass(frozen=True, slots=True)
class BoundedJointDeltaPreview:
    """Internal preview value with no runtime address or wire-contract role."""

    joint_names: tuple[str, ...]
    start_positions_rad: tuple[float, ...]
    target_positions_rad: tuple[float, ...]
    target_joint: str
    delta_deg: float


def _finite_number(value: object, *, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise CommissioningRefusal(f"{label} is not numeric")
    result = float(value)
    if not math.isfinite(result):
        raise CommissioningRefusal(f"{label} is not finite")
    return result


def require_fresh_source_age(
    source_age_ns: object,
    *,
    max_source_age_ms: float = DEFAULT_MAX_SOURCE_AGE_MS,
) -> float:
    """Return a finite source age in milliseconds or refuse it."""

    maximum = _finite_number(max_source_age_ms, label="max_source_age_ms")
    if not 0.0 < maximum <= DEFAULT_MAX_SOURCE_AGE_MS:
        raise CommissioningRefusal(
            "max_source_age_ms must be in (0, 250]"
        )
    age_ns = _finite_number(source_age_ns, label="source_age_ns")
    if age_ns < 0.0:
        raise CommissioningRefusal("source_age_ns is negative")
    age_ms = age_ns / 1_000_000.0
    if age_ms > maximum:
        raise CommissioningRefusal(
            f"observed state is stale ({age_ms:.1f} ms; limit {maximum:.1f} ms)"
        )
    return age_ms


def preview_bounded_joint_delta(
    *,
    joint_names: Sequence[str],
    start_positions_rad: Sequence[float],
    target_joint: str,
    delta_deg: float,
) -> BoundedJointDeltaPreview:
    """Compute one effect-free, caller-addressed Lite6 joint preview."""

    if len(joint_names) != EXPECTED_LITE6_JOINT_COUNT:
        raise CommissioningRefusal("a Lite6 observation must contain six joints")
    names = tuple(joint_names)
    if any(
        not isinstance(name, str)
        or not name
        or len(name) > _MAX_JOINT_NAME_CHARS
        or "\x00" in name
        for name in names
    ):
        raise CommissioningRefusal("joint names must be bounded non-empty strings")
    if len(set(names)) != len(names):
        raise CommissioningRefusal("joint names must be unique")
    if target_joint not in names:
        raise CommissioningRefusal("target_joint is not present in the observation")

    if len(start_positions_rad) != EXPECTED_LITE6_JOINT_COUNT:
        raise CommissioningRefusal(
            "start positions must match the six observed joints"
        )
    positions = tuple(
        _finite_number(value, label=f"start_positions_rad[{index}]")
        for index, value in enumerate(start_positions_rad)
    )
    delta = _finite_number(delta_deg, label="delta_deg")
    if delta == 0.0 or abs(delta) > MAX_ABS_DELTA_DEG:
        raise CommissioningRefusal("absolute delta_deg must be in (0, 1]")

    target_index = names.index(target_joint)
    targets = list(positions)
    targets[target_index] += math.radians(delta)
    return BoundedJointDeltaPreview(
        joint_names=names,
        start_positions_rad=positions,
        target_positions_rad=tuple(targets),
        target_joint=target_joint,
        delta_deg=delta,
    )


def main() -> int:
    """Fail before parsing arguments or contacting a runtime."""

    print(
        f"{LITE6_COMMISSIONING_UNAVAILABLE_REASON_CODE}: the caller-neutral "
        "commissioning client returns at I10/R after I6-I8",
        file=sys.stderr,
    )
    return 78


if __name__ == "__main__":
    raise SystemExit(main())
