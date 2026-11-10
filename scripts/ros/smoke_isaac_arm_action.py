#!/usr/bin/env python3
"""Unavailable live Isaac arm-action smoke entrypoint.

The old probe addressed one hard-coded controller action directly and issued
motion outside the new component/binding and Kernel admission contracts.  A
future smoke must be a thin client of those qualified contracts; this file
contains no direct-ROS fallback motion path.
"""

from __future__ import annotations

import sys
from typing import Sequence


UNAVAILABLE_REASON_CODE = "composable_cell_runtime_unavailable"
UNAVAILABLE_MESSAGE = (
    f"{UNAVAILABLE_REASON_CODE}: live Isaac arm smoke is not available yet"
)


def main(argv: Sequence[str] | None = None) -> int:
    """Fail before ROS discovery, action creation, or motion dispatch."""

    del argv
    print(UNAVAILABLE_MESSAGE, file=sys.stderr)
    return 78


if __name__ == "__main__":
    raise SystemExit(main())
