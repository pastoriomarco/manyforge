#!/usr/bin/env python3
"""Lite6 smoke evidence envelope plus an unavailable live Isaac runner.

Artifact validation remains active in
``scripts/ros/validate-isaac-lite6-smoke-artifact.py``.  This file retains the
matching ROS-free result envelope, while the direct ROS action/effort command
probe is removed until it can use qualified component bindings and the single
Kernel execution path.
"""

from __future__ import annotations

import sys
from dataclasses import asdict, dataclass, field
from typing import Any, Sequence


LITE6_ARM_JOINTS = ("joint1", "joint2", "joint3", "joint4", "joint5", "joint6")
LITE6_GRIPPER_JOINT = "right_finger_joint"
RESULT_SCHEMA = "manyforge.isaac_lite6_live_smoke.v1"

UNAVAILABLE_REASON_CODE = "composable_cell_runtime_unavailable"
RESTORATION_GATE = "I6/N + I8/P5 + I9/I"
UNAVAILABLE_MESSAGE = (
    f"{UNAVAILABLE_REASON_CODE}: live Isaac Lite6 smoke requires the "
    f"{RESTORATION_GATE} restoration gates"
)


@dataclass(frozen=True)
class SmokeResult:
    """Transport-neutral result used by the retained artifact validator."""

    passed: bool
    details: dict[str, Any] = field(default_factory=dict)


def build_smoke_artifact(
    result: SmokeResult,
    *,
    started_at_utc: str,
    ended_at_utc: str,
) -> dict[str, Any]:
    """Build the stable live-smoke envelope without writing or publishing it."""

    if not started_at_utc or not ended_at_utc:
        raise ValueError("smoke evidence timestamps must be non-empty")
    payload = asdict(result)
    details = payload["details"]
    details["schema"] = RESULT_SCHEMA
    details["started_at_utc"] = started_at_utc
    details["ended_at_utc"] = ended_at_utc
    return payload


def main(argv: Sequence[str] | None = None) -> int:
    """Fail before ROS discovery, controller commands, or artifact writes."""

    del argv
    print(UNAVAILABLE_MESSAGE, file=sys.stderr)
    return 78


if __name__ == "__main__":
    raise SystemExit(main())
