"""Test double of ``manyforge_headless_py.Gateway`` for the Headless adapters.

``FakeGateway(answer)`` hands every host call to ``answer`` as the command
dict the wire would carry without its attribution
(``{"api_version", "command", **members}``) and returns its reply; the core
adds the attribution, picks the lane socket and checks the reply envelope
(``manyforge_headless/test/test_gateway_core.cpp``). ``calls`` records
``(command, members, principal_id)``. ``answer`` may raise
``manyforge_headless_py.GatewayError`` as the core does on transport loss.
"""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

import manyforge_headless_py as core  # type: ignore[import-not-found]

GatewayError = core.GatewayError


class FakeGateway:
    def __init__(self, answer: Callable[[dict[str, Any]], dict[str, Any]]) -> None:
        self.answer = answer
        self.calls: list[tuple[str, dict[str, Any], str | None]] = []

    def host(
        self,
        command: str,
        members: dict[str, Any] | None = None,
        principal_id: str | None = None,
    ) -> dict[str, Any]:
        members = dict(members or {})
        self.calls.append((command, members, principal_id))
        return self.answer(
            {"api_version": "manyforge.execution_command.v4", "command": command, **members}
        )


def fake_client(answer: Callable[[dict[str, Any]], dict[str, Any]]) -> Any:
    """An ``ExecutionCommandClient`` over a ``FakeGateway``."""

    from manyforge_headless_api.execution import ExecutionCommandClient

    return ExecutionCommandClient(FakeGateway(answer))
