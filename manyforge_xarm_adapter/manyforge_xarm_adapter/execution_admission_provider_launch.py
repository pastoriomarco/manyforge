"""Unavailable boundary for the removed Lite6 admission-provider launcher.

The historical module compiled and executed the exclusive
``manyforge.robot_lifecycle.execution_admission.v0.experimental`` wire.  It
also carried the rejected caller/session authority model and constructed one
brand-specific admission process from singleton robot/controller names.  No
owner-neutral launcher algorithm remains after those contracts are removed.

Exact reimplementation input is retained at ManyForge commit
``08222188293df8e12f90dc72e204c21f22f92ea1``: this source is blob
``79ba4bde9268b907f6e34a48d2301fe80fcf39fe``; its six command-validation
test cohorts are blob ``ae5d6c8074ccad8b39c1120a34dfe9c0e52d3507``; and the
installed-provider freshness process case is blob
``a598f1d499ca32feeb2a90bf6fea8738042cb555``.

Caller-neutral lifecycle/controller transactions return at I7/L0, Kernel
admission and Stop ownership at I8/P5, and focused xArm ROS translation at
I10/R.  The historical closed-by-default, exact-identity, freshness-expiry,
and no-lifecycle-command cases remain acceptance input at those gates; the old
provider wire and launcher do not return.
"""

from __future__ import annotations

import sys
from typing import Final


UNAVAILABLE_REASON_CODE: Final = "composable_cell_runtime_unavailable"
RESTORATION_GATES: Final = ("I7/L0", "I8/P5", "I10/R")
UNAVAILABLE_MESSAGE: Final = (
    f"{UNAVAILABLE_REASON_CODE}: the removed Lite6 execution-admission "
    "provider launcher has no current owner; lifecycle, Kernel admission, "
    "and xArm translation must pass I7/L0, I8/P5, and I10/R"
)
HISTORICAL_SOURCE_BLOB: Final = (
    "79ba4bde9268b907f6e34a48d2301fe80fcf39fe"
)


def main() -> int:
    """Fail before argument, file, process, ROS, or robot access."""

    print(UNAVAILABLE_MESSAGE, file=sys.stderr)
    return 78


if __name__ == "__main__":
    raise SystemExit(main())
