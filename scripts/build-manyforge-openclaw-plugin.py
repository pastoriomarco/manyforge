#!/usr/bin/env python3
"""OpenClaw artifact cutover gate with historical restoration mapping.

The complete historical wrapper is pinned at
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` (blob
``ceaba90fbe034f481e53d02a315e486c264cb22f``).  Its ``main`` function
discovered the removed MCP surface by executing
``generate-manyforge-openclaw-plugin.py`` and then published ``latest``
metadata for that historical wire contract.  That live-contract workflow is
not current and returns with the qualified assistant/MCP client.

The former content hash and archive helpers have no current producer or
consumer.  They are removed rather than retained as an unbounded, untested
publication path; the future qualified producer must restore bounded input,
exclusive output, symlink/special-file rejection, and byte-reproducibility.
"""

from __future__ import annotations

import json
import sys
from typing import Final, NamedTuple


UNAVAILABLE_EXIT_CODE: Final = 78
UNAVAILABLE_REASON_CODE: Final = "composable_cell_runtime_unavailable"
HISTORICAL_SOURCE: Final = (
    "manyforge@08222188293df8e12f90dc72e204c21f22f92ea1"
)


class RestorationAssignment(NamedTuple):
    historical_symbols: str
    required_behavior: str


RESTORATION_ASSIGNMENTS: Final = (
    RestorationAssignment(
        "main: live generator invocation and latest-pointer publication",
        "build a versioned artifact only from a qualified generated plugin",
    ),
    RestorationAssignment(
        "main: tool_count and mcp_endpoint metadata",
        "derive metadata from the future assistant contract, not MCP v0",
    ),
    RestorationAssignment(
        "_sha256_bytes, _surface_hash, _write_reproducible_archive",
        "H0 bounded artifact hashing and exclusive byte-reproducible archive "
        "publication after a qualified plugin producer exists",
    ),
)


def unavailable_payload() -> dict[str, object]:
    return {
        "schema_version": "manyforge.operator_unavailable.v1",
        "reason_code": UNAVAILABLE_REASON_CODE,
        "script": "build-manyforge-openclaw-plugin.py",
        "historical_source": HISTORICAL_SOURCE,
        "exit_code": UNAVAILABLE_EXIT_CODE,
        "assignments": [
            assignment._asdict() for assignment in RESTORATION_ASSIGNMENTS
        ],
    }


def main(argv: list[str] | None = None) -> int:
    """Fail before subprocess, filesystem publication, or network effects."""

    del argv
    print(json.dumps(unavailable_payload(), sort_keys=True), file=sys.stderr)
    return UNAVAILABLE_EXIT_CODE


if __name__ == "__main__":
    raise SystemExit(main())
