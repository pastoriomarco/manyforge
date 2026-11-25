"""A test-only probe of the execution host's sockets for the process tests.

``HostProbe`` is the Headless execution adapter over a real gateway core
(``manyforge_headless_py.Gateway``) plus what only these tests need: the
``events`` page read, the internal-only commands the Program runner sends,
and a raw one-packet exchange on any host socket for the host's own refusals
of packets the gateway never sends.
"""

from __future__ import annotations

from pathlib import Path
import socket
from typing import Any

import manyforge_headless_py as core  # type: ignore[import-not-found]
import rfc8785

from manyforge_headless_api.execution import (
    MAX_EXECUTION_RESPONSE_BYTES,
    ExecutionCommandClient,
    _decode_observation_response,
    _events_page,
)
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.private_transport import PinnedRunRoot


class HostProbe(ExecutionCommandClient):
    def __init__(
        self,
        run_root: Path,
        *,
        timeout_seconds: float = 10.0,
        attribution_transport: str = "http",
    ) -> None:
        super().__init__(
            core.Gateway(
                str(run_root), None, attribution=attribution_transport,
                connect_limit_s=timeout_seconds,
            ),
            observation_run_root=run_root,
        )
        self._probe_root = PinnedRunRoot(run_root)
        self._timeout_seconds = timeout_seconds
        self._socket_path = self._probe_root.path / "execution-command.sock"
        self._control_socket_path = self._probe_root.path / "execution-control.sock"

    def events(self, after_sequence: int, *, limit: int = 16) -> dict[str, Any]:
        """One ``events`` page with the reply's facts (the pump's page shape)."""

        reply = self.require_success(
            self._host("events", {"after_sequence": after_sequence, "limit": limit})
        )
        page = reply["event_page"]
        return _events_page(
            reply, page["events"], has_more=page["has_more"],
            after_sequence=after_sequence, progress=reply["operation_progress"],
        )

    def query_operation(self, query: Any) -> dict[str, Any]:
        body = query.model_dump(mode="json") if hasattr(query, "model_dump") else dict(query)
        return self.require_success(self._host("query_operation", {"query": body}))

    def stop_program_run(self, request: Any) -> dict[str, Any]:
        from manyforge_headless_api.execution import _sparse_request_with_complete_context

        return self._host(
            "stop_program_run", {"request": _sparse_request_with_complete_context(request)}
        )

    def read_stop(self, event_id: int) -> dict[str, Any]:
        return self._host("read_stop", {"event_id": event_id})

    def _exchange(self, command: dict[str, Any]) -> dict[str, Any]:
        return self._exchange_on(command, self._socket_path)

    def _exchange_on(self, command: dict[str, Any], socket_path: Path) -> dict[str, Any]:
        """One raw packet on ``socket_path`` and its decoded answer."""

        encoded = rfc8785.dumps(command)
        try:
            with self._probe_root.socket_address(Path(socket_path).name) as address:
                with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                    client.setsockopt(
                        socket.SOL_SOCKET, socket.SO_RCVBUF, MAX_EXECUTION_RESPONSE_BYTES
                    )
                    client.settimeout(self._timeout_seconds)
                    client.connect(address)
                    client.settimeout(None)
                    client.send(encoded)
                    payload, _ancillary, flags, _address = client.recvmsg(
                        MAX_EXECUTION_RESPONSE_BYTES
                    )
        except OSError as error:
            raise HeadlessApiError(
                "execution_owner_unavailable", f"probe transport failed: {error}"
            ) from error
        return _decode_observation_response(payload, flags)


__all__ = ["HostProbe"]
