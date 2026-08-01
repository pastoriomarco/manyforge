"""Pure key-normalization helpers retained for future MCP clients.

No MCP server, JSON-RPC session, tool catalog, or Composer mutation surface is
constructed here.  MCP remains unavailable until its H0/I5 and N/I6 clients
are qualified.
"""

from __future__ import annotations

import re
from typing import Any


_CAMEL_TO_SNAKE_RE_1 = re.compile(r"(.)([A-Z][a-z]+)")
_CAMEL_TO_SNAKE_RE_2 = re.compile(r"([a-z0-9])([A-Z])")


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


__all__ = ["camel_to_snake", "snakecase_keys"]
