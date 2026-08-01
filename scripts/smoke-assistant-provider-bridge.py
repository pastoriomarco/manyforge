#!/usr/bin/env python3
"""Disposition gate for the historical assistant-provider HTTP smoke.

The complete implementation is pinned at
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` (blob
``176c6308b9ab9e1e599aad6f41bea2f85005a47b``).  ``SmokeFailure``,
``_parse_tool_spec`` and
``_build_request_payload`` encoded ``manyforge.assistant.provider_request.v0``;
``_post_json`` drove the deleted provider route; and ``_validate_response``
asserted the matching response/tool-call wire.  ``main`` combined those
responsibilities into a live smoke.  None is a current public contract.

The case intent remains assigned to the future assistant client: bounded tool
catalog input, request identity, authentication/timeout behavior, malformed
response rejection, allowed-tool enforcement, tool-call structure, and
provider error reporting.  The old DTO is absent rather than retained behind
an unreachable early return.
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
        "SmokeFailure, _parse_args, _parse_tool_spec, _build_request_payload",
        "bounded configuration over the future provider request contract",
    ),
    RestorationAssignment(
        "_post_json",
        "authenticated bounded provider client with explicit timeouts",
    ),
    RestorationAssignment(
        "_validate_response, _require_list",
        "strict response and allowed-tool validation",
    ),
    RestorationAssignment(
        "main",
        "focused provider smoke after the assistant client is qualified",
    ),
)


def unavailable_payload() -> dict[str, object]:
    return {
        "schema_version": "manyforge.qualification_unavailable.v1",
        "reason_code": UNAVAILABLE_REASON_CODE,
        "script": "smoke-assistant-provider-bridge.py",
        "historical_source": HISTORICAL_SOURCE,
        "exit_code": UNAVAILABLE_EXIT_CODE,
        "assignments": [
            assignment._asdict() for assignment in RESTORATION_ASSIGNMENTS
        ],
    }


def main(argv: list[str] | None = None) -> int:
    """Fail before argument-derived DTO creation or network effects."""

    del argv
    print(json.dumps(unavailable_payload(), sort_keys=True), file=sys.stderr)
    return UNAVAILABLE_EXIT_CODE


if __name__ == "__main__":
    raise SystemExit(main())
