"""Bounded H0 observation client; provider and authoring loops remain gated."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any, Final, Protocol

from manyforge_headless_api.read_client import HeadlessReadClient


ASSISTANT_H0_READ_OPERATIONS: Final = frozenset(HeadlessReadClient.operations())


class AssistantHeadlessReadError(ValueError):
    """An assistant requested behavior outside the closed H0 read surface."""


class HeadlessReadPort(Protocol):
    def invoke(
        self,
        operation: str,
        arguments: Mapping[str, Any] | None = None,
    ) -> dict[str, Any]: ...


class AssistantHeadlessReadClient:
    """Expose owner-issued facts without callbacks, tools, or mutation."""

    def __init__(self, reader: HeadlessReadPort) -> None:
        if not callable(getattr(reader, "invoke", None)):
            raise TypeError("reader must provide the headless read port")
        self._reader = reader

    def observe(
        self,
        operation: str,
        arguments: Mapping[str, Any] | None = None,
    ) -> dict[str, Any]:
        if operation not in ASSISTANT_H0_READ_OPERATIONS:
            raise AssistantHeadlessReadError(
                f"assistant operation {operation!r} is unavailable at H0; "
                "Program authoring and runtime mutation return at their later gates"
            )
        return self._reader.invoke(operation, arguments)


__all__ = [
    "ASSISTANT_H0_READ_OPERATIONS",
    "AssistantHeadlessReadClient",
    "AssistantHeadlessReadError",
]
