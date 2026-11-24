"""Bounded private transport to the independently restartable Program runner."""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from copy import deepcopy
from typing import Any, Protocol
from urllib.parse import urlsplit, urlunsplit

import httpx
from pydantic import ValidationError

from .internal_http import INTERNAL_HTTP_CLIENT_LIMITS
from .native import strict_json_object
from .program_cycle import ProgramCommandReplyDTO


MAX_PROGRAM_RUNNER_RESPONSE_BYTES = 32 * 1024 * 1024


class ProgramRunnerClientError(RuntimeError):
    """The configured Program runner is unavailable or rejected a request."""

    def __init__(
        self,
        code: str,
        message: str,
        status_code: int | None = None,
        *,
        details: Mapping[str, Any] | None = None,
        producer: str = "program_runner",
    ) -> None:
        super().__init__(message)
        self.code = code
        self.status_code = status_code
        self.details = deepcopy(dict(details or {}))
        self.producer = producer


_PERSISTENT_VALUE_ACTION_SUFFIXES = {"set": "", "reset": "/reset", "clear": "/clear"}


def persistent_values_path(action: str | None = None) -> str:
    """Path of the Applied Scenario's persistent blackboard values, or of one edit.

    Values belong to the Applied Scenario project; the Program runner resolves
    it, and an edit names the project folder the operator saw in its body.
    """

    if action is not None and action not in _PERSISTENT_VALUE_ACTION_SUFFIXES:
        raise ValueError("persistent-value action is unsupported")
    suffix = "" if action is None else _PERSISTENT_VALUE_ACTION_SUFFIXES[action]
    return f"/api/program/persistent-values{suffix}"


class ProgramRunnerPort(Protocol):
    @property
    def control_state_websocket_url(self) -> str | None: ...

    def node_availability(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def runs(self, *, active_only: bool, limit: int) -> dict[str, Any]: ...

    def diagnostic_health(self) -> dict[str, Any]: ...

    def start_run(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def prepare_applied_scene(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def read_run(self, program_run_id: int) -> dict[str, Any]: ...

    def preview_node_correction(
        self, program_run_id: int, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def start_motion(
        self,
        program_run_id: int,
        body: Mapping[str, Any],
    ) -> dict[str, Any]: ...

    def resume_halt(
        self,
        program_run_id: int,
        body: Mapping[str, Any],
    ) -> dict[str, Any]: ...

    def resume_motion(
        self, program_run_id: int, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def prepare_manual_handover(
        self, program_run_id: int, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def cycle_status(self, *, before_command: bool = False) -> dict[str, Any]: ...

    def reset_cycle(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def runtime_controls(self, program_run_id: int) -> dict[str, Any]: ...

    def runtime_blackboard_values(self, program_run_id: int) -> dict[str, Any]: ...

    def runtime_control_action(
        self, program_run_id: int, suffix: str, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def apply_recovery_action(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def persistent_values(self) -> dict[str, Any]: ...

    def persistent_value_action(
        self, action: str, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def reconcile_persistent_values(self) -> dict[str, Any]: ...

    def close(self) -> None: ...


class ProgramRunnerHttpClient:
    """Transport only; all Program-run meaning remains in the remote owner."""

    def __init__(
        self,
        base_url: str,
        *,
        timeout_seconds: float = 30.0,
        client: httpx.Client | None = None,
    ) -> None:
        if not 0 < timeout_seconds <= 60:
            raise ValueError("Program-runner timeout must be in (0, 60] seconds")
        parsed = urlsplit(base_url)
        if (
            parsed.scheme not in {"http", "https"}
            or not parsed.netloc
            or parsed.username is not None
            or parsed.password is not None
            or parsed.path not in {"", "/"}
            or bool(parsed.query)
            or bool(parsed.fragment)
        ):
            raise ValueError(
                "Program-runner URL must be absolute credential-free HTTP(S)"
            )
        self._owns_client = client is None
        self._base_url = base_url.rstrip("/")
        # A runner command answers at its acceptance and publishes its end
        # in its row (OD-214): only the connection is limited, never the
        # wait for the answer (the Manager's 250 ms liveness probe detects a
        # lost runner). So is the cycle status read before a command is
        # forwarded; other status reads keep the whole limit.
        self._command_timeout = httpx.Timeout(None, connect=timeout_seconds)
        self._client = client or httpx.Client(
            base_url=self._base_url,
            timeout=httpx.Timeout(timeout_seconds),
            limits=INTERNAL_HTTP_CLIENT_LIMITS,
        )

    @property
    def control_state_websocket_url(self) -> str:
        """Private event-driven Program control projection URL."""

        parsed = urlsplit(self._base_url)
        return urlunsplit(
            (
                "wss" if parsed.scheme == "https" else "ws",
                parsed.netloc,
                "/ws/program/control-state",
                "",
                "",
            )
        )

    @staticmethod
    def _error(
        value: Mapping[str, Any],
        status_code: int,
    ) -> ProgramRunnerClientError:
        detail = value.get("detail")
        code = detail.get("code") if isinstance(detail, Mapping) else None
        message = detail.get("message") if isinstance(detail, Mapping) else None
        details = detail.get("details") if isinstance(detail, Mapping) else None
        producer = detail.get("producer") if isinstance(detail, Mapping) else None
        return ProgramRunnerClientError(
            code if isinstance(code, str) and code else "program_runner_http_error",
            (
                message
                if isinstance(message, str) and message
                else f"Program runner returned HTTP {status_code}"
            ),
            status_code,
            details=details if isinstance(details, Mapping) else None,
            producer=(
                producer
                if isinstance(producer, str) and producer
                else "program_runner"
            ),
        )

    def _request(
        self,
        method: str,
        path: str,
        *,
        params: Sequence[tuple[str, str | int]] = (),
        body: Mapping[str, Any] | None = None,
        command: bool = False,
        wait_for_answer: bool = False,
    ) -> dict[str, Any]:
        if (
            not (
                path.startswith("/api/program/")
                or path
                in {
                    "/api/cycle/status",
                    "/api/cycle/recovery-actions",
                }
            )
            or ".." in path
        ):
            raise ValueError("Program-runner path is invalid")
        try:
            with self._client.stream(
                method,
                path,
                params=params,
                json=dict(body) if body is not None else None,
                **(
                    {"timeout": self._command_timeout}
                    if command or wait_for_answer
                    else {}
                ),
            ) as response:
                chunks: list[bytes] = []
                size = 0
                for chunk in response.iter_bytes():
                    size += len(chunk)
                    if size > MAX_PROGRAM_RUNNER_RESPONSE_BYTES:
                        raise ProgramRunnerClientError(
                            "program_runner_response_oversized",
                            "Program-runner response exceeds its byte bound",
                            response.status_code,
                        )
                    chunks.append(chunk)
        except ProgramRunnerClientError:
            raise
        except httpx.HTTPError as exc:
            # The transport failure's class (for example `ConnectError`,
            # `RemoteProtocolError`, `ReadTimeout`) names the real cause.
            raise ProgramRunnerClientError(
                "program_runner_unavailable",
                "independent Program runner is unavailable",
                details={"transport_error": type(exc).__name__},
            ) from exc
        raw = b"".join(chunks)
        try:
            payload = strict_json_object(
                raw.decode("utf-8", errors="strict"),
                label="Program-runner response",
            )
        except (UnicodeError, RuntimeError) as exc:
            raise ProgramRunnerClientError(
                "program_runner_response_invalid",
                "Program runner returned an invalid response",
                response.status_code,
            ) from exc
        if response.status_code >= 400:
            raise self._error(payload, response.status_code)
        if command:
            try:
                ProgramCommandReplyDTO.model_validate(payload)
            except ValidationError as exc:
                raise ProgramRunnerClientError(
                    "program_runner_response_invalid",
                    "Program runner returned an invalid command reply",
                    response.status_code,
                ) from exc
        return payload

    def _command(self, path: str, body: Mapping[str, Any]) -> dict[str, Any]:
        """One runner command: its ``program_command_reply.v1`` at acceptance."""

        return self._request("POST", path, body=body, command=True)

    @staticmethod
    def _run_path(program_run_id: int) -> str:
        if type(program_run_id) is not int or not 1 <= program_run_id <= 9_007_199_254_740_991:
            raise ValueError("program_run_id is invalid")
        return f"/api/program/runs/{program_run_id}"

    def runs(
        self,
        *,
        active_only: bool = False,
        limit: int = 128,
    ) -> dict[str, Any]:
        if type(active_only) is not bool:
            raise ValueError("active_only must be boolean")
        if type(limit) is not int or not 1 <= limit <= 4_096:
            raise ValueError("Program-run index limit is outside its bound")
        return self._request(
            "GET",
            "/api/program/runs",
            params=(
                ("active_only", "true" if active_only else "false"),
                ("limit", limit),
            ),
        )

    def node_availability(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self._request("POST", "/api/program/node-availability", body=body)

    def diagnostic_health(self) -> dict[str, Any]:
        return self._request("GET", "/api/program/diagnostics/health")

    def start_run(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self._command("/api/program/runs", body)

    def prepare_applied_scene(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self._request("POST", "/api/program/scene-preparations", body=body)

    def read_run(self, program_run_id: int) -> dict[str, Any]:
        return self._request("GET", self._run_path(program_run_id))

    def preview_node_correction(
        self, program_run_id: int, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self._request(
            "POST",
            f"{self._run_path(program_run_id)}/node-correction-previews",
            body=body,
        )

    def start_motion(
        self,
        program_run_id: int,
        body: Mapping[str, Any],
    ) -> dict[str, Any]:
        return self._command(f"{self._run_path(program_run_id)}/motion-start", body)

    def resume_motion(
        self, program_run_id: int, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self._command(f"{self._run_path(program_run_id)}/motion-resume", body)

    def prepare_manual_handover(
        self, program_run_id: int, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self._command(f"{self._run_path(program_run_id)}/manual-handover", body)

    def resume_halt(
        self,
        program_run_id: int,
        body: Mapping[str, Any],
    ) -> dict[str, Any]:
        return self._command(f"{self._run_path(program_run_id)}/resume-halt", body)

    def cycle_status(self, *, before_command: bool = False) -> dict[str, Any]:
        """The cycle status; ``before_command``: the read that decides whether
        a command is forwarded, which waits for the runner's answer."""

        return self._request(
            "GET", "/api/cycle/status", wait_for_answer=before_command
        )

    def reset_cycle(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self._command("/api/program/cycle/reset", body)

    def runtime_controls(self, program_run_id: int) -> dict[str, Any]:
        return self._request(
            "GET", f"{self._run_path(program_run_id)}/runtime-controls"
        )

    def runtime_blackboard_values(self, program_run_id: int) -> dict[str, Any]:
        return self._request(
            "GET", f"{self._run_path(program_run_id)}/blackboard-values"
        )

    def runtime_control_action(
        self,
        program_run_id: int,
        suffix: str,
        body: Mapping[str, Any],
    ) -> dict[str, Any]:
        allowed = {
            "blackboard-values",
            "manual-result",
        }
        if suffix not in allowed:
            raise ValueError("runtime-control action is unsupported")
        return self._command(f"{self._run_path(program_run_id)}/{suffix}", body)

    def apply_recovery_action(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self._command("/api/cycle/recovery-actions", body)

    def persistent_values(self) -> dict[str, Any]:
        return self._request("GET", persistent_values_path())

    def persistent_value_action(
        self, action: str, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self._command(persistent_values_path(action), body)

    def reconcile_persistent_values(self) -> dict[str, Any]:
        return self._request("POST", "/api/program/persistent-values/reconcile")

    def close(self) -> None:
        if self._owns_client:
            self._client.close()

    def __enter__(self) -> "ProgramRunnerHttpClient":
        return self

    def __exit__(self, *_args: object) -> None:
        self.close()


__all__ = [
    "persistent_values_path",
    "ProgramRunnerClientError",
    "ProgramRunnerHttpClient",
    "ProgramRunnerPort",
]
