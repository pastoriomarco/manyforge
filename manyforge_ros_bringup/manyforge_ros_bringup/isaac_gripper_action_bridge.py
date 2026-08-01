"""Unavailable boundary for the former Isaac gripper action owner.

The exact historical action server is preserved at ManyForge commit
08222188293df8e12f90dc72e204c21f22f92ea1, blob
ee6ede5e9d4efd1121a563c84600ef7d943e05be.  Its goal admission, command
publication, completion, cancellation, timeout, readiness, and lifecycle
behavior returns only through the addressed I9/I and I11/T adapter, with I8/P5
Stop acceptance.  Owner-neutral mapping and settled-observation mechanics
remain in ``isaac_gripper_command.py`` with focused tests.
"""

from __future__ import annotations

import sys


UNAVAILABLE_REASON_CODE = "composable_cell_runtime_unavailable"
RESTORATION_GATES = ("I8/P5", "I9/I", "I11/T")
UNAVAILABLE_MESSAGE = (
    f"{UNAVAILABLE_REASON_CODE}: the Isaac gripper action adapter requires "
    "the I9/I and I11/T restoration gates with I8/P5 Stop acceptance"
)


def main() -> int:
    """Fail before ROS imports, action admission, threads, or publication."""

    print(UNAVAILABLE_MESSAGE, file=sys.stderr)
    return 78


if __name__ == "__main__":
    raise SystemExit(main())
