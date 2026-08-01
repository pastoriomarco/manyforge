"""Unavailable boundary for the deleted Isaac DemoSupervisor owner.

The historical node combined process readiness, Composer artifact loading,
command gating, cycle reset, and runtime-status publication.  Those
responsibilities return through Component Manager, headless, Kernel, and Isaac
owners; this module deliberately contains no backup supervisor state machine.
"""

from __future__ import annotations

import sys


UNAVAILABLE_REASON_CODE = "composable_cell_runtime_unavailable"
RESTORATION_GATE = "I5/H0 + I6/N + I8/P5 + I9/I"
UNAVAILABLE_MESSAGE = (
    f"{UNAVAILABLE_REASON_CODE}: the Isaac product requires the "
    f"{RESTORATION_GATE} restoration gates; no DemoSupervisor fallback exists"
)


def main() -> int:
    """Fail before ROS initialization, HTTP access, or command publication."""

    print(UNAVAILABLE_MESSAGE, file=sys.stderr)
    return 78


if __name__ == "__main__":
    raise SystemExit(main())
