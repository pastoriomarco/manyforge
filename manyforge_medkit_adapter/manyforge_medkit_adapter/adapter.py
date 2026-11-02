"""Unavailable effectful boundary for the ros2_medkit adapter.

The transport-neutral fault lifecycle reducer remains in
``manyforge_medkit_adapter.forwarder``.  The removed process polled deleted
Composer endpoints and owned a concrete medkit service loop; retaining that
loop below an early return would preserve an invalid runtime integration.
The future adapter must consume the qualified addressed runtime diagnostic
source and remain only the medkit-specific ROS translation boundary.
"""

from __future__ import annotations

import sys


UNAVAILABLE_REASON_CODE = "composable_cell_runtime_unavailable"
RESTORATION_GATE = "I6/N"
UNAVAILABLE_MESSAGE = (
    f"{UNAVAILABLE_REASON_CODE}: ros2_medkit fault publication requires the "
    f"{RESTORATION_GATE} addressed runtime diagnostics source"
)


def main() -> int:
    """Fail before argument parsing, HTTP access, ROS, or fault publication."""

    print(UNAVAILABLE_MESSAGE, file=sys.stderr)
    return 78


if __name__ == "__main__":
    raise SystemExit(main())
