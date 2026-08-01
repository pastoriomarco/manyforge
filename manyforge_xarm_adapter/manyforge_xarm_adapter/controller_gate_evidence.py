"""Pure pose-evidence reduction plus an unavailable old gate runner.

The historical ROS capture deactivated/reactivated a controller and opened the
now-rejected renewable xArm command gate.  Its service clients, executor,
threads, runtime-binding loader, orchestration, and evidence writer are the
obsolete I7/I8/I10 ownership path and are removed.  The small pose-deviation
reducer remains useful for future attended evidence without owning a command.

Exact source is retained at ManyForge commit
``08222188293df8e12f90dc72e204c21f22f92ea1``, blob
``84d16253b1a9b0f4925a70e5b00eedd2cee1df2a``.  Four historical test cohorts
are blob ``3f425fd9f180c07009a9c00d4205b8537e274688``.  Their no-trajectory
switch/re-arm, explicit pose bound, exact confirmation, and ROS-message
integration obligations map to caller-neutral CCLC at I7/L0, Kernel admission
at I8/P5, and attended xArm qualification at I10/R.  The renewable gate does
not return.
"""

from __future__ import annotations

from dataclasses import dataclass
import math
import sys
from typing import Final


UNAVAILABLE_REASON_CODE: Final = "composable_cell_runtime_unavailable"
RESTORATION_GATES: Final = ("I7/L0", "I8/P5", "I10/R")
UNAVAILABLE_MESSAGE: Final = (
    f"{UNAVAILABLE_REASON_CODE}: the old Lite6 controller/renewable-gate "
    "runner was removed; replacement evidence requires I7/L0, I8/P5, and "
    "I10/R"
)
HISTORICAL_SOURCE_BLOB: Final = (
    "84d16253b1a9b0f4925a70e5b00eedd2cee1df2a"
)


@dataclass(frozen=True)
class JointSample:
    """One effect-free joint sample used by attended evidence reduction."""

    received_at_ns: int
    positions: tuple[float, ...]


def max_pose_deviation(samples: list[JointSample]) -> float:
    """Return maximum absolute deviation from the first supplied pose."""

    if not samples:
        return math.inf
    anchor = samples[0].positions
    if not anchor or any(len(sample.positions) != len(anchor) for sample in samples):
        raise ValueError("joint samples must have one non-empty shared shape")
    if not all(
        math.isfinite(value)
        for sample in samples
        for value in sample.positions
    ):
        raise ValueError("joint samples must contain only finite positions")
    return max(
        abs(value - anchor[index])
        for sample in samples
        for index, value in enumerate(sample.positions)
    )


def main() -> int:
    """Fail before inputs, ROS imports/services, threads, files, or commands."""

    print(UNAVAILABLE_MESSAGE, file=sys.stderr)
    return 78


if __name__ == "__main__":
    raise SystemExit(main())
