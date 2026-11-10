"""Unavailable boundary for the historical Isaac scene-inspection topic RPC.

The former implementation is pinned at source blob
``1d074cc77f3a25ccb1af9f0616bdff2bf1298ccc``.  It owned the private
``manyforge.isaac.collider_{query,report}.v0.1`` String-topic wire, DDS retry,
correlation, response validation, and a live ROS CLI.  This obsolete wire is
not retained as a callable fallback.

A thin client returns only after the Isaac retained-product contract is
frozen.  Historical test blob
``03e1c87e5a8689fa2fa832b500395b115028bbff`` preserves its six acceptance
cases: the live correlated-RPC cohort at lines 9-96 and five strict response
rejection cases at lines 98-154.
"""

from __future__ import annotations

import sys
from typing import Final


UNAVAILABLE_EXIT_CODE: Final = 78
UNAVAILABLE_REASON_CODE: Final = "composable_cell_runtime_unavailable"
RESTORATION_GATE: Final = "I9/I"
HISTORICAL_SOURCE_BLOB: Final = "1d074cc77f3a25ccb1af9f0616bdff2bf1298ccc"
HISTORICAL_TEST_BLOB: Final = "03e1c87e5a8689fa2fa832b500395b115028bbff"
UNAVAILABLE_MESSAGE: Final = (
    f"{UNAVAILABLE_REASON_CODE}: live Isaac scene inspection is not available"
)


def unavailable_payload() -> dict[str, object]:
    return {
        "schema_version": "manyforge.qualification_unavailable.v1",
        "reason_code": UNAVAILABLE_REASON_CODE,
        "restoration_gate": RESTORATION_GATE,
        "historical_source_blob": HISTORICAL_SOURCE_BLOB,
        "historical_test_blob": HISTORICAL_TEST_BLOB,
        "removed_contracts": [
            "manyforge.isaac.collider_query.v0.1",
            "manyforge.isaac.collider_report.v0.1",
        ],
        "historical_acceptance": [
            "lines 9-96: correlated read-only ROS topic RPC and bounded cleanup",
            "lines 98-154: five fail-closed response validation cases",
        ],
        "exit_code": UNAVAILABLE_EXIT_CODE,
    }


def main(argv: list[str] | None = None) -> int:
    """Fail before arguments, ROS imports, topic access, or runtime state."""

    del argv
    print(UNAVAILABLE_MESSAGE, file=sys.stderr)
    return UNAVAILABLE_EXIT_CODE


if __name__ == "__main__":
    raise SystemExit(main())
