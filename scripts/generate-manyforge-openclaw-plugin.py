#!/usr/bin/env python3
"""Validate candidate tool descriptors without reviving the MCP-v0 plugin.

The full generator is pinned at
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` (blob
``2e0c732d22e2107443b63d1580bc7a8205e0e6e9``).  Historical symbols map as
follows:

* ``fetch_manyforge_tools`` performed live ``tools/list`` discovery against
  the deleted Composer MCP route;
* ``build_plugin_manifest`` and ``build_plugin_index`` encoded its endpoint,
  principal, forwarding, terminal-answer, and tool-call wire contracts; and
* ``main`` combined those owners and wrote a runnable plugin tree.

Those responsibilities return only with a qualified assistant/MCP client.
The bounded descriptor validator is retained because checking an already
captured candidate catalog is owner-neutral; it does not mint tool authority,
perform discovery, or generate a transport implementation.
"""

from __future__ import annotations

import copy
import json
import math
import sys
from typing import Any, Final, NamedTuple


UNAVAILABLE_EXIT_CODE: Final = 78
UNAVAILABLE_REASON_CODE: Final = "composable_cell_runtime_unavailable"
HISTORICAL_SOURCE: Final = (
    "manyforge@08222188293df8e12f90dc72e204c21f22f92ea1"
)
MAX_TOOL_COUNT: Final = 512
MAX_TOOL_NAME_CHARS: Final = 256
MAX_TOOL_DESCRIPTION_CHARS: Final = 16_384
MAX_CATALOG_JSON_BYTES: Final = 4 * 1024 * 1024
MAX_SCHEMA_DEPTH: Final = 32
MAX_SCHEMA_NODES: Final = 16_384
MAX_SCHEMA_COLLECTION_ITEMS: Final = 4096
MAX_SCHEMA_STRING_CHARS: Final = 16_384
MAX_SCHEMA_KEY_CHARS: Final = 256
MAX_SCHEMA_INTEGER_ABS: Final = 2**63 - 1


class RestorationAssignment(NamedTuple):
    historical_symbols: str
    required_behavior: str


RESTORATION_ASSIGNMENTS: Final = (
    RestorationAssignment(
        "fetch_manyforge_tools",
        "discover a trusted catalog from the future headless assistant client",
    ),
    RestorationAssignment(
        "build_plugin_manifest, build_plugin_index",
        "generate only the transport and reporting contract qualified at that gate",
    ),
    RestorationAssignment(
        "main",
        "write a runnable plugin only after discovery and transport are qualified",
    ),
)


def validate_tool_descriptors(tools: object) -> tuple[dict[str, Any], ...]:
    """Return a bounded copy of structurally valid candidate descriptors."""

    if not isinstance(tools, list):
        raise TypeError("tools must be a list")
    if not tools or len(tools) > MAX_TOOL_COUNT:
        raise ValueError(f"tools must contain 1..{MAX_TOOL_COUNT} entries")
    _validate_bounded_json(tools, label="tools")
    try:
        encoded_catalog = json.dumps(
            tools,
            allow_nan=False,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")
    except (TypeError, ValueError) as exc:
        raise ValueError("tools must contain JSON-compatible values") from exc
    if len(encoded_catalog) > MAX_CATALOG_JSON_BYTES:
        raise ValueError(
            f"tools exceed the {MAX_CATALOG_JSON_BYTES}-byte catalog bound"
        )

    normalized: list[dict[str, Any]] = []
    names: set[str] = set()
    for index, tool in enumerate(tools):
        if not isinstance(tool, dict):
            raise TypeError(f"tools[{index}] must be an object")
        name = tool.get("name")
        description = tool.get("description")
        schema = tool.get("inputSchema")
        if not isinstance(name, str) or not name.strip():
            raise ValueError(f"tools[{index}].name must be a non-empty string")
        name = name.strip()
        if len(name) > MAX_TOOL_NAME_CHARS:
            raise ValueError(f"tools[{index}].name is too long")
        if name in names:
            raise ValueError(f"duplicate tool name: {name}")
        if not isinstance(description, str):
            raise TypeError(f"tools[{index}].description must be a string")
        if len(description) > MAX_TOOL_DESCRIPTION_CHARS:
            raise ValueError(f"tools[{index}].description is too long")
        if not isinstance(schema, dict):
            raise TypeError(f"tools[{index}].inputSchema must be an object")
        names.add(name)
        normalized.append(
            {
                "name": name,
                "description": description,
                "inputSchema": copy.deepcopy(schema),
            }
        )
    return tuple(normalized)


def _validate_bounded_json(value: Any, *, label: str) -> None:
    node_count = 0
    text_count = 0
    active_containers: set[int] = set()

    def visit(item: Any, *, path: str, depth: int) -> None:
        nonlocal node_count, text_count
        node_count += 1
        if node_count > MAX_SCHEMA_NODES:
            raise ValueError(f"{label} exceeds the {MAX_SCHEMA_NODES}-node bound")
        if depth > MAX_SCHEMA_DEPTH:
            raise ValueError(f"{path} exceeds the {MAX_SCHEMA_DEPTH}-level bound")
        if item is None or type(item) is bool:
            return
        if isinstance(item, (int, float)):
            if isinstance(item, float) and not math.isfinite(item):
                raise ValueError(f"{path} must be finite")
            if isinstance(item, int) and abs(item) > MAX_SCHEMA_INTEGER_ABS:
                raise ValueError(f"{path} exceeds the signed 64-bit integer bound")
            return
        if isinstance(item, str):
            if len(item) > MAX_SCHEMA_STRING_CHARS:
                raise ValueError(
                    f"{path} exceeds the {MAX_SCHEMA_STRING_CHARS}-character bound"
                )
            text_count += len(item)
            if text_count > MAX_CATALOG_JSON_BYTES:
                raise ValueError(
                    f"{label} exceeds the {MAX_CATALOG_JSON_BYTES}-character bound"
                )
            return
        if not isinstance(item, (dict, list)):
            raise ValueError(f"{path} is not JSON-compatible")
        if len(item) > MAX_SCHEMA_COLLECTION_ITEMS:
            raise ValueError(
                f"{path} exceeds the {MAX_SCHEMA_COLLECTION_ITEMS}-item bound"
            )
        identity = id(item)
        if identity in active_containers:
            raise ValueError(f"{path} contains a recursive container")
        active_containers.add(identity)
        try:
            if isinstance(item, dict):
                for key, child in item.items():
                    if not isinstance(key, str):
                        raise ValueError(f"{path} contains a non-string key")
                    if len(key) > MAX_SCHEMA_KEY_CHARS:
                        raise ValueError(
                            f"{path} contains a key longer than "
                            f"{MAX_SCHEMA_KEY_CHARS} characters"
                        )
                    text_count += len(key)
                    if text_count > MAX_CATALOG_JSON_BYTES:
                        raise ValueError(
                            f"{label} exceeds the "
                            f"{MAX_CATALOG_JSON_BYTES}-character bound"
                        )
                    visit(child, path=f"{path}.{key}", depth=depth + 1)
            else:
                for index, child in enumerate(item):
                    visit(child, path=f"{path}[{index}]", depth=depth + 1)
        finally:
            active_containers.remove(identity)

    visit(value, path=label, depth=0)


def unavailable_payload() -> dict[str, object]:
    return {
        "schema_version": "manyforge.operator_unavailable.v1",
        "reason_code": UNAVAILABLE_REASON_CODE,
        "script": "generate-manyforge-openclaw-plugin.py",
        "historical_source": HISTORICAL_SOURCE,
        "exit_code": UNAVAILABLE_EXIT_CODE,
        "assignments": [
            assignment._asdict() for assignment in RESTORATION_ASSIGNMENTS
        ],
    }


def main(argv: list[str] | None = None) -> int:
    """Fail before discovery, filesystem publication, or network effects."""

    del argv
    print(json.dumps(unavailable_payload(), sort_keys=True), file=sys.stderr)
    return UNAVAILABLE_EXIT_CODE


if __name__ == "__main__":
    raise SystemExit(main())
