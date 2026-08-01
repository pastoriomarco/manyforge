#!/usr/bin/env python3
"""Unavailable live Isaac drop-approach capture entrypoint.

The ROS-free ``manyforge.roboplan_transition_capture.v1`` parser and validator
remain in ``scripts/qualification/roboplan_05_qualification.py``.  This live
producer previously depended on deleted Composer, Program-v0, singleton
group, planning-capture, scene-authority, and cycle-control wires.  It must be
rebuilt against the qualified native/Kernel/Isaac owners, not kept dormant.
"""

from __future__ import annotations

import sys


UNAVAILABLE_REASON_CODE = "composable_cell_runtime_unavailable"
RESTORATION_GATE = "I6/N + I8/P5 + I9/I"
UNAVAILABLE_MESSAGE = (
    f"{UNAVAILABLE_REASON_CODE}: live Isaac drop-approach capture requires "
    f"the {RESTORATION_GATE} restoration gates"
)


def main(argv: list[str] | None = None) -> int:
    """Fail before artifact reads, network access, or runtime mutation."""

    del argv
    print(UNAVAILABLE_MESSAGE, file=sys.stderr)
    return 78


if __name__ == "__main__":
    raise SystemExit(main())
