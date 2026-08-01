"""Unavailable boundary for the historical Isaac mesh-transfer topic RPC.

The complete implementation is pinned at source blob
``ed0b7fa46046f9e257baae9f6f07c97049b11718``.  It owned the private
``manyforge.isaac.mesh_transfer_{request,status,chunk}.v0.1`` wire, chunk
framing and integrity checks, DDS transport, filesystem materialization, and a
live ROS CLI.  Those responsibilities cannot remain callable while I9 is
unavailable, and the v0.1 wire is not a future compatibility contract.

I9/I must recover the useful bounded-transfer properties through the frozen
retained-product client.  Historical test blob
``3653dea92b75948e9bd8a47d1b9ce6af4c3bcc66`` preserves the packet integrity
case at lines 11-37 and the live two-chunk transfer/cleanup case at lines
40-156.
"""

from __future__ import annotations

import sys
from typing import Final


UNAVAILABLE_EXIT_CODE: Final = 78
UNAVAILABLE_REASON_CODE: Final = "composable_cell_runtime_unavailable"
RESTORATION_GATE: Final = "I9/I"
HISTORICAL_SOURCE_BLOB: Final = "ed0b7fa46046f9e257baae9f6f07c97049b11718"
HISTORICAL_TEST_BLOB: Final = "3653dea92b75948e9bd8a47d1b9ce6af4c3bcc66"
UNAVAILABLE_MESSAGE: Final = (
    f"{UNAVAILABLE_REASON_CODE}: live Isaac mesh transfer requires the "
    f"{RESTORATION_GATE} restoration gate"
)


def unavailable_payload() -> dict[str, object]:
    return {
        "schema_version": "manyforge.qualification_unavailable.v1",
        "reason_code": UNAVAILABLE_REASON_CODE,
        "restoration_gate": RESTORATION_GATE,
        "historical_source_blob": HISTORICAL_SOURCE_BLOB,
        "historical_test_blob": HISTORICAL_TEST_BLOB,
        "removed_contracts": [
            "manyforge.isaac.mesh_transfer_request.v0.1",
            "manyforge.isaac.mesh_transfer_status.v0.1",
            "manyforge.isaac.mesh_transfer_chunk.v0.1",
        ],
        "historical_acceptance": [
            "lines 11-37: packet framing, length, and sha256 integrity",
            "lines 40-156: correlated two-chunk ROS transfer and bounded cleanup",
        ],
        "exit_code": UNAVAILABLE_EXIT_CODE,
    }


def main(argv: list[str] | None = None) -> int:
    """Fail before arguments, ROS, transport, paths, or filesystem writes."""

    del argv
    print(UNAVAILABLE_MESSAGE, file=sys.stderr)
    return UNAVAILABLE_EXIT_CODE


if __name__ == "__main__":
    raise SystemExit(main())
