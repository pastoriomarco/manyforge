"""Bounded private transport to the independently restartable Program runner."""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from copy import deepcopy
from typing import Any, Protocol
from urllib.parse import quote, urlsplit

import httpx

from .native import strict_json_object


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
    ) -> None:
        super().__init__(message)
        self.code = code
        self.status_code = status_code
        self.details = deepcopy(dict(details or {}))


class ProgramRunnerPort(Protocol):
    def runs(self, *, active_only: bool, limit: int) -> dict[str, Any]: ...

    def start_run(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def prepare_applied_scene(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def read_run(self, program_run_id: str) -> dict[str, Any]: ...

    def stop_run(
        self,
        program_run_id: str,
        body: Mapping[str, Any],
    ) -> dict[str, Any]: ...

    def stop_motion(
        self,
        program_run_id: str,
        body: Mapping[str, Any],
    ) -> dict[str, Any]: ...

    def full_stop(
        self,
        program_run_id: str,
        body: Mapping[str, Any],
    ) -> dict[str, Any]: ...

    def resume_full_stop(
        self,
        program_run_id: str,
        body: Mapping[str, Any],
    ) -> dict[str, Any]: ...

    def discard_full_stop(
        self,
        program_run_id: str,
        body: Mapping[str, Any],
    ) -> dict[str, Any]: ...

    def cycle_status(self) -> dict[str, Any]: ...

    def reset_cycle(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def runtime_controls(self, program_run_id: str) -> dict[str, Any]: ...

    def runtime_control_action(
        self, program_run_id: str, suffix: str, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def apply_recovery_action(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

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
        self._client = client or httpx.Client(
            base_url=base_url.rstrip("/"),
            timeout=httpx.Timeout(timeout_seconds),
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
        return ProgramRunnerClientError(
            code if isinstance(code, str) and code else "program_runner_http_error",
            message
            if isinstance(message, str) and message
            else f"Program runner returned HTTP {status_code}",
            status_code,
            details=details if isinstance(details, Mapping) else None,
        )

    def _request(
        self,
        method: str,
        path: str,
        *,
        params: Sequence[tuple[str, str | int]] = (),
        body: Mapping[str, Any] | None = None,
    ) -> dict[str, Any]:
        if (
            not (
                path.startswith("/api/program/")
                or path in {
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
            raise ProgramRunnerClientError(
                "program_runner_unavailable",
                "independent Program runner is unavailable",
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
        return payload

    @staticmethod
    def _run_path(program_run_id: str) -> str:
        if (
            not isinstance(program_run_id, str)
            or not program_run_id
            or len(program_run_id.encode("utf-8")) > 256
            or "/" in program_run_id
            or ".." in program_run_id
        ):
            raise ValueError("program_run_id is invalid")
        return f"/api/program/runs/{quote(program_run_id, safe='')}"

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

    def start_run(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self._request("POST", "/api/program/runs", body=body)

    def prepare_applied_scene(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self._request(
            "POST", "/api/program/scene-preparations", body=body
        )

    def read_run(self, program_run_id: str) -> dict[str, Any]:
        return self._request("GET", self._run_path(program_run_id))

    def stop_run(
        self,
        program_run_id: str,
        body: Mapping[str, Any],
    ) -> dict[str, Any]:
        return self._request(
            "POST",
            f"{self._run_path(program_run_id)}/stop",
            body=body,
        )

    def stop_motion(
        self,
        program_run_id: str,
        body: Mapping[str, Any],
    ) -> dict[str, Any]:
        return self._request(
            "POST",
            f"{self._run_path(program_run_id)}/motion-stop",
            body=body,
        )

    def full_stop(
        self,
        program_run_id: str,
        body: Mapping[str, Any],
    ) -> dict[str, Any]:
        return self._request(
            "POST",
            f"{self._run_path(program_run_id)}/full-stop",
            body=body,
        )

    def resume_full_stop(
        self,
        program_run_id: str,
        body: Mapping[str, Any],
    ) -> dict[str, Any]:
        return self._request(
            "POST",
            f"{self._run_path(program_run_id)}/resume-full-stop",
            body=body,
        )

    def discard_full_stop(
        self,
        program_run_id: str,
        body: Mapping[str, Any],
    ) -> dict[str, Any]:
        return self._request(
            "POST",
            f"{self._run_path(program_run_id)}/discard-full-stop",
            body=body,
        )

    def cycle_status(self) -> dict[str, Any]:
        return self._request("GET", "/api/cycle/status")

    def reset_cycle(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self._request("POST", "/api/program/cycle/reset", body=body)

    def runtime_controls(self, program_run_id: str) -> dict[str, Any]:
        return self._request(
            "GET", f"{self._run_path(program_run_id)}/runtime-controls"
        )

    def runtime_control_action(
        self,
        program_run_id: str,
        suffix: str,
        body: Mapping[str, Any],
    ) -> dict[str, Any]:
        allowed = {
            "runtime-overrides",
            "runtime-overrides/activate",
            "runtime-overrides/clear",
            "runtime-overrides/promote",
            "blackboard-values",
            "manual-result",
        }
        if suffix not in allowed:
            raise ValueError("runtime-control action is unsupported")
        return self._request(
            "POST",
            f"{self._run_path(program_run_id)}/{suffix}",
            body=body,
        )

    def apply_recovery_action(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self._request("POST", "/api/cycle/recovery-actions", body=body)

    def close(self) -> None:
        if self._owns_client:
            self._client.close()

    def __enter__(self) -> "ProgramRunnerHttpClient":
        return self

    def __exit__(self, *_args: object) -> None:
        self.close()


__all__ = [
    "ProgramRunnerClientError",
    "ProgramRunnerHttpClient",
    "ProgramRunnerPort",
]
