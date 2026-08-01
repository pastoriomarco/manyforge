#!/usr/bin/env python3
"""Typed boundary for the historical launcher scenario-row adapter.

The complete adapter remains available at
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` (blob
``916b265d221ee13b47f046e3a7f8a8d553de8403``).  It translated host paths,
built the Python node environment, read deployment-v0 scenario metadata, and
emitted the launcher's private pipe-delimited row contract.  That contract is
not a current artifact or headless API and is therefore not retained as a
dormant loader.

I6/N restores bounded scenario discovery as a thin client of the addressed
Program/artifact owners.  Its acceptance evidence must cover the historical
host/runtime path translation, alias metadata, deterministic ordering, and
fail-closed path validation without reviving deployment-v0 loading.
"""

from __future__ import annotations

import json
import sys
from typing import Final


UNAVAILABLE_EXIT_CODE: Final = 78
UNAVAILABLE_REASON_CODE: Final = "composable_cell_scenario_discovery_unavailable"
HISTORICAL_SOURCE_BLOB: Final = "916b265d221ee13b47f046e3a7f8a8d553de8403"
RESTORATION_GATE: Final = "I6/N"


def unavailable_payload() -> dict[str, object]:
    """Describe the cutover without reading arguments, files, or environment."""

    return {
        "schema_version": "manyforge.qualification_unavailable.v1",
        "reason_code": UNAVAILABLE_REASON_CODE,
        "restoration_gate": RESTORATION_GATE,
        "historical_source_blob": HISTORICAL_SOURCE_BLOB,
        "exit_code": UNAVAILABLE_EXIT_CODE,
        "removed_contract": "legacy_pipe_delimited_deployment_v0_scenario_rows",
    }


def main(argv: list[str] | None = None) -> int:
    """Fail before arguments, environment, imports, filesystem, or output rows."""

    del argv
    print(json.dumps(unavailable_payload(), sort_keys=True), file=sys.stderr)
    return UNAVAILABLE_EXIT_CODE


if __name__ == "__main__":
    raise SystemExit(main())
