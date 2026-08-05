"""Pure normalization plus the transport-neutral H0 MCP read adapter.

No MCP server, JSON-RPC session, tool catalog, or Composer mutation surface is
constructed here. The closed read catalog delegates to the shared headless
client; Program, recovery, and mutation tools remain at their later gates.
"""

from __future__ import annotations

import re
from collections.abc import Mapping
from types import MappingProxyType
from typing import Any, Final, Protocol


_CAMEL_TO_SNAKE_RE_1 = re.compile(r"(.)([A-Z][a-z]+)")
_CAMEL_TO_SNAKE_RE_2 = re.compile(r"([a-z0-9])([A-Z])")

MCP_HEADLESS_READ_TOOLS: Final = MappingProxyType(
    {
        "manyforge_headless_health": "health",
        "manyforge_headless_runtime": "runtime",
        "manyforge_manager_status": "manager_status",
        "manyforge_manager_graph": "manager_graph",
        "manyforge_manager_diff": "manager_diff",
        "manyforge_node_environment": "node_environment",
        "manyforge_scene_snapshot": "scene",
        "manyforge_operator_alarms": "alarms",
        "manyforge_alarm_metrics": "alarm_metrics",
        "manyforge_event_history": "history",
        "manyforge_historian_health": "historian_health",
    }
)


class McpHeadlessReadError(ValueError):
    """A tool request is outside the transport-neutral H0 read catalog."""


class HeadlessReadPort(Protocol):
    def invoke(
        self,
        operation: str,
        arguments: Mapping[str, Any] | None = None,
    ) -> dict[str, Any]: ...


def camel_to_snake(value: str) -> str:
    """Convert one camel-case key to lowercase snake case."""

    value = _CAMEL_TO_SNAKE_RE_1.sub(r"\1_\2", value)
    value = _CAMEL_TO_SNAKE_RE_2.sub(r"\1_\2", value)
    return value.lower()


def snakecase_keys(value: Any) -> Any:
    """Recursively normalize mapping keys without changing scalar values."""

    if isinstance(value, dict):
        return {
            camel_to_snake(str(key)): snakecase_keys(item)
            for key, item in value.items()
        }
    if isinstance(value, list):
        return [snakecase_keys(item) for item in value]
    return value


def invoke_headless_read_tool(
    reader: HeadlessReadPort,
    tool_name: str,
    arguments: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    """Map one closed MCP-facing read name to the shared bounded HTTP client.

    This is deliberately not an MCP server or JSON-RPC session. Later-gate
    authoring, execution, recovery, and scene mutation tools are absent.
    """

    operation = MCP_HEADLESS_READ_TOOLS.get(tool_name)
    if operation is None:
        raise McpHeadlessReadError(
            f"unsupported H0 MCP read tool: {tool_name}"
        )
    if arguments is not None and not isinstance(arguments, Mapping):
        raise McpHeadlessReadError("MCP read arguments must be an object")
    return reader.invoke(operation, arguments)


__all__ = [
    "MCP_HEADLESS_READ_TOOLS",
    "McpHeadlessReadError",
    "camel_to_snake",
    "invoke_headless_read_tool",
    "snakecase_keys",
]
