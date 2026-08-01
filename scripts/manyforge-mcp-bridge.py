#!/usr/bin/env python3
"""Typed cutover boundary for the historical stdio/HTTP MCP bridge.

The complete implementation is pinned at
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` (blob
``cbc9f9772d24ddace7e635d28e00e1fccae59513``).  It is design input, not a
runnable fallback.  Its responsibilities are assigned exactly by symbol:

* ``_ssl_context``, ``_http``, ``_HttpError``, ``_refresh_mode_manifest`` and
  ``_get_mode_manifest``: future authenticated headless assistant client;
* ``_mcp_tool_descriptor`` through ``_visible_tool_ids``: future trusted
  catalog projection, after the public tool contract exists;
* ``_handle_initialize``, ``_handle_tools_list``, ``_normalize_tool_id`` and
  ``_handle``: future thin MCP transport;
* ``_handle_tools_call``, ``_bridge_error_to_mcp`` and ``_log_tool_event``:
  future operation client plus audit projection, never runtime authority; and
* ``_trace``, ``_trace_event`` and ``main``: future bounded operator bridge.

The old Composer URLs, caller-selected principal, mode manifest, tool IDs,
aliases, mutation routes, and MCP response shapes are intentionally absent.
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
        "_ssl_context, _http, _HttpError, _refresh_mode_manifest, "
        "_get_mode_manifest",
        "authenticated headless assistant client",
    ),
    RestorationAssignment(
        "_mcp_tool_descriptor, _mode_tool_entries, _visible_tool_entries, "
        "_visible_tool_ids",
        "trusted bounded catalog projection",
    ),
    RestorationAssignment(
        "_handle_initialize, _handle_tools_list, _normalize_tool_id, _handle",
        "thin MCP transport over the future public contract",
    ),
    RestorationAssignment(
        "_handle_tools_call, _bridge_error_to_mcp, _log_tool_event",
        "operation client and audit projection over owner-issued facts",
    ),
    RestorationAssignment(
        "_trace, _trace_event, main",
        "bounded operator bridge lifecycle",
    ),
)


def unavailable_payload() -> dict[str, object]:
    return {
        "schema_version": "manyforge.operator_unavailable.v1",
        "reason_code": UNAVAILABLE_REASON_CODE,
        "script": "manyforge-mcp-bridge.py",
        "historical_source": HISTORICAL_SOURCE,
        "exit_code": UNAVAILABLE_EXIT_CODE,
        "assignments": [
            assignment._asdict() for assignment in RESTORATION_ASSIGNMENTS
        ],
    }


def main(argv: list[str] | None = None) -> int:
    """Fail before environment, TLS, network, stdio-loop, or trace effects."""

    del argv
    print(json.dumps(unavailable_payload(), sort_keys=True), file=sys.stderr)
    return UNAVAILABLE_EXIT_CODE


if __name__ == "__main__":
    raise SystemExit(main())
