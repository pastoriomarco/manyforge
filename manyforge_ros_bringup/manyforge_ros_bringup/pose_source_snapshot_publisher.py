"""Unavailable boundary for the ROS pose-source snapshot publisher.

Pose-source normalization remains in :mod:`manyforge_ros.pose_source_sidecar`.
The former process loaded the deleted deployment-v0 runtime model and posted to
Composer-owned scene endpoints, so its process body is not a valid dormant
fallback.  A new thin ROS client returns only after the headless scene owner
and the Isaac retained product are qualified.
"""

from __future__ import annotations

import sys


UNAVAILABLE_REASON_CODE = "composable_cell_runtime_unavailable"
RESTORATION_GATE = "I5/H0 + I9/I"
UNAVAILABLE_MESSAGE = (
    f"{UNAVAILABLE_REASON_CODE}: pose-source snapshot publication requires "
    f"the {RESTORATION_GATE} headless-owner and Isaac-client gates"
)


def main(args: list[str] | None = None) -> int:
    """Fail before ROS initialization, artifact loading, or network effects."""

    del args
    print(UNAVAILABLE_MESSAGE, file=sys.stderr)
    return 78


if __name__ == "__main__":
    raise SystemExit(main())
