"""Test double of the core's ``manyforge_headless_py.Gateway`` for runner tests.

``RecordingGateway(replies)`` has one method per named command, as the core
does: it records ``(command, members)`` in ``sent`` and answers from
``replies[command]`` (a reply dict, a list consumed in order, an exception
to raise, or a callable taking the members). A command without a reply is
missing, like an attribute the double does not have.
"""

from __future__ import annotations

from copy import deepcopy
from typing import Any, Callable, Mapping


class RecordingGateway:
    def __init__(self, replies: Mapping[str, Any] | None = None) -> None:
        self.replies: dict[str, Any] = dict(replies or {})
        self.sent: list[tuple[str, dict[str, Any]]] = []

    def __getattr__(self, command: str) -> Callable[..., Any]:
        if command.startswith("_") or command not in self.replies:
            raise AttributeError(command)

        def call(members: Mapping[str, Any] = {}, principal_id: str | None = None) -> Any:
            assert principal_id is None  # The runner's gateway is internal.
            self.sent.append((command, deepcopy(dict(members))))
            reply = self.replies[command]
            if isinstance(reply, list):
                reply = reply.pop(0)
            if isinstance(reply, BaseException):
                raise reply
            if callable(reply):
                reply = reply(dict(members))
            return deepcopy(reply)

        return call

    def commands(self) -> list[str]:
        return [command for command, _members in self.sent]
