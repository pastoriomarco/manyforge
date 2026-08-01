"""Unavailable boundary for the removed duplicate Lite6 observer owner.

The former ROS node republished vendor state as a separately initialized joint
state and owned an xArm-specific diagnostics truth.  Although it exposed no
command endpoint, it was the duplicate read-only owner that I10 replaces with
one selected measured-state provider plus owner-issued headless projections.
Keeping the node class below a fenced console would preserve that competing
owner and its ROS threads.

Exact reimplementation input is retained at ManyForge commit
``08222188293df8e12f90dc72e204c21f22f92ea1``: source blob
``9d51f0b9910e2051f247726bef6796ef3ddc9da3``; command-boundary test blob
``5a80ce7fcef5cd1416a1c7e96e37cb0cc4af0070``; fake observed-state component
test blob ``82e952d6ae64cf4f992246488fe6d7b9bec3a40d``; and disconnected-process
test blob ``b86f316e8b5546e8073a2d1a6ff10a41bb142f03``.

The command-free constraint, finite/complete joint mapping, freshness,
source-agreement, fault/warning presentation, and clean shutdown cases remain
acceptance input.  The measured-state adapter returns at I10/R and its
headless status/alarm projection at I5/H0; this duplicate ROS publisher does
not return.
"""

from __future__ import annotations

import sys
from typing import Final


UNAVAILABLE_REASON_CODE: Final = "composable_cell_runtime_unavailable"
RESTORATION_GATES: Final = ("I5/H0", "I10/R")
UNAVAILABLE_MESSAGE: Final = (
    f"{UNAVAILABLE_REASON_CODE}: the duplicate Lite6 read-only observer was "
    "removed; selected measured-state and headless projections return at "
    "I10/R and I5/H0"
)
HISTORICAL_SOURCE_BLOB: Final = (
    "9d51f0b9910e2051f247726bef6796ef3ddc9da3"
)


def main(_args: object = None) -> int:
    """Fail without inspecting arguments or importing/initializing ROS."""

    print(UNAVAILABLE_MESSAGE, file=sys.stderr)
    return 78


if __name__ == "__main__":
    raise SystemExit(main())
