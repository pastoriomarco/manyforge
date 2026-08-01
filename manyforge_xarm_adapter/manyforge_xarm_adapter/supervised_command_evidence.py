"""Unavailable boundary for the removed attended-command evidence runner.

The former runner resolved an old runtime binding, opened caller authority
sessions, executed the legacy lifecycle evidence binary, and wrote command
evidence.  Its argv, result schema, confirmation wire, and replay behavior are
inseparable from the superseded lifecycle/session owner.  The reusable,
command-free transient reduction remains in :mod:`lifecycle_transient`.

Exact reimplementation input is retained at ManyForge commit
``08222188293df8e12f90dc72e204c21f22f92ea1``: source blob
``cabaf2b1e0f15972a39563e0357c7b02418d5c0c``; seven deterministic test
cohorts in blob ``56f15e21dfb8886027577f4046f70d4ce8fb23eb``; and the
installed successive-client/replay process cohort in blob
``5f1b9cdb9f27819a5ca833308c59433de942b7e5``.

Caller-neutral command transactions return at I7/L0, exact Kernel dispatch
and Stop evidence at I8/P5, and attended Lite6 evidence capture at I10/R.
Timeouts, exact request/result correlation, confirmation, transient capture,
replay, and primitive audit remain acceptance requirements; authority-session
and direct legacy executable paths do not return.
"""

from __future__ import annotations

import sys
from typing import Final


UNAVAILABLE_REASON_CODE: Final = "composable_cell_runtime_unavailable"
RESTORATION_GATES: Final = ("I7/L0", "I8/P5", "I10/R")
UNAVAILABLE_MESSAGE: Final = (
    f"{UNAVAILABLE_REASON_CODE}: attended Lite6 command evidence requires "
    "caller-neutral lifecycle, Kernel evidence, and real-lane qualification "
    "at I7/L0, I8/P5, and I10/R"
)
HISTORICAL_SOURCE_BLOB: Final = (
    "cabaf2b1e0f15972a39563e0357c7b02418d5c0c"
)


def main() -> int:
    """Fail before inputs, package lookup, processes, ROS, files, or commands."""

    print(UNAVAILABLE_MESSAGE, file=sys.stderr)
    return 78


if __name__ == "__main__":
    raise SystemExit(main())
