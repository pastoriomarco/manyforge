"""Bounded thin HTTP client shared by CLI, Composer, and assistant adapters."""

from __future__ import annotations

from dataclasses import dataclass
import json
import re
from typing import Any, Iterable, Mapping, Sequence

import httpx

from .native import strict_json_object


MAX_HTTP_RESPONSE_BYTES = 64 * 1024 * 1024
_CONDITION_ID_PATTERN = re.compile(r"^condition-[0-9a-f]{24}$")


class HeadlessClientError(RuntimeError):
    def __init__(self, code: str, message: str, status_code: int | None = None) -> None:
        super().__init__(message)
        self.code = code
        self.status_code = status_code


@dataclass(frozen=True, slots=True)
class CallerIdentity:
    request_owner_epoch: str
    source_label: str


class HeadlessHttpClient:
    """Transport only: never cache, reconcile, or infer owner state."""

    def __init__(
        self,
        base_url: str,
        *,
        timeout_seconds: float = 2.0,
        client: httpx.Client | None = None,
    ) -> None:
        if timeout_seconds <= 0 or timeout_seconds > 30:
            raise ValueError("HTTP timeout must be in (0, 30] seconds")
        self._owns_client = client is None
        self._client = client or httpx.Client(
            base_url=base_url.rstrip("/"),
            timeout=httpx.Timeout(timeout_seconds),
        )

    @staticmethod
    def _error_from_response(
        value: dict[str, Any],
        status_code: int,
    ) -> HeadlessClientError:
        detail = value.get("detail")
        if isinstance(detail, dict):
            code = detail.get("code")
            message = detail.get("message")
        else:
            error = value.get("error")
            code = error.get("code") if isinstance(error, dict) else None
            message = error.get("message") if isinstance(error, dict) else None
        return HeadlessClientError(
            code if isinstance(code, str) and code else "headless_http_error",
            message
            if isinstance(message, str) and message
            else f"headless API returned HTTP {status_code}",
            status_code,
        )

    def request(
        self,
        method: str,
        path: str,
        *,
        params: Sequence[tuple[str, str | int]] = (),
        body: Mapping[str, Any] | None = None,
    ) -> dict[str, Any]:
        if not path.startswith("/api/") or ".." in path:
            raise ValueError("headless API path is invalid")
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
                    if size > MAX_HTTP_RESPONSE_BYTES:
                        raise HeadlessClientError(
                            "headless_http_response_oversized",
                            "headless API response exceeds its byte bound",
                            response.status_code,
                        )
                    chunks.append(chunk)
        except HeadlessClientError:
            raise
        except httpx.HTTPError as exc:
            raise HeadlessClientError(
                "headless_http_unavailable",
                f"headless API request failed: {exc}",
            ) from exc
        payload = b"".join(chunks)
        try:
            text = payload.decode("utf-8", errors="strict")
        except UnicodeDecodeError as exc:
            raise HeadlessClientError(
                "headless_http_response_invalid",
                "headless API response is not UTF-8",
                response.status_code,
            ) from exc
        try:
            value = strict_json_object(text, label="headless HTTP response")
        except RuntimeError as exc:
            raise HeadlessClientError(
                getattr(exc, "code", "headless_http_response_invalid"),
                str(exc),
                response.status_code,
            ) from exc
        if response.status_code >= 400:
            raise self._error_from_response(value, response.status_code)
        return value

    def runtime(self) -> dict[str, Any]:
        return self.request("GET", "/api/runtime")

    def health(self) -> dict[str, Any]:
        return self.request("GET", "/api/health")

    def manager_status(self) -> dict[str, Any]:
        return self.request("GET", "/api/manager/status")

    def manager_graph(self) -> dict[str, Any]:
        return self.request("GET", "/api/manager/graph")

    def manager_diff(self) -> dict[str, Any]:
        return self.request("GET", "/api/manager/diff")

    def node_environment(self) -> dict[str, Any]:
        return self.request("GET", "/api/node-environment")

    def manager_apply(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self.request("POST", "/api/manager/apply", body=body)

    def execution_status(self) -> dict[str, Any]:
        return self.request("GET", "/api/execution/status")

    def execute(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self.request("POST", "/api/execution/operations", body=body)

    @staticmethod
    def _operation_path(operation_id: str) -> str:
        if not operation_id or len(operation_id.encode("utf-8")) > 256:
            raise ValueError("operation_id must be bounded non-empty text")
        if "/" in operation_id or ".." in operation_id:
            raise ValueError("operation_id is invalid for an HTTP path")
        return f"/api/execution/operations/{operation_id}"

    def operation(self, operation_id: str) -> dict[str, Any]:
        return self.request("GET", self._operation_path(operation_id))

    def _operation_control(
        self,
        operation_id: str,
        control: str,
        body: Mapping[str, Any],
    ) -> dict[str, Any]:
        return self.request(
            "POST",
            f"{self._operation_path(operation_id)}/{control}",
            body=body,
        )

    def pause_operation(
        self, operation_id: str, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self._operation_control(operation_id, "pause", body)

    def resume_operation(
        self, operation_id: str, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self._operation_control(operation_id, "resume", body)

    def reset_operation(
        self, operation_id: str, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self._operation_control(operation_id, "reset", body)

    def acknowledge_completed_operation(
        self, operation_id: str, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self._operation_control(
            operation_id, "acknowledge-completed", body
        )

    def stop_operations(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self.request("POST", "/api/execution/stops", body=body)

    def set_operation_runtime_scale(
        self, operation_id: str, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self._operation_control(operation_id, "runtime-scale", body)

    def operation_runtime_scale(self, operation_id: str) -> dict[str, Any]:
        return self.request(
            "GET", f"{self._operation_path(operation_id)}/runtime-scale"
        )

    def scene(self) -> dict[str, Any]:
        return self.request("GET", "/api/scene")

    def alarms(
        self,
        *,
        limit: int | None = None,
        states: Iterable[str] = (),
        ordering: str = "default",
    ) -> dict[str, Any]:
        if ordering not in {"default", "attention_newest"}:
            raise ValueError("alarm snapshot ordering is unsupported")
        params: list[tuple[str, str | int]] = []
        if limit is not None:
            params.append(("limit", limit))
        params.extend(("states", state) for state in states)
        if ordering != "default":
            params.append(("ordering", ordering))
        return self.request("GET", "/api/execution/alarms", params=params)

    def alarm_metrics(self) -> dict[str, Any]:
        return self.request("GET", "/api/execution/alarms/metrics")

    def acknowledge_alarm(
        self,
        condition_id: str,
        body: Mapping[str, Any],
    ) -> dict[str, Any]:
        if _CONDITION_ID_PATTERN.fullmatch(condition_id) is None:
            raise ValueError("condition_id is not a canonical alarm condition id")
        return self.request(
            "POST",
            f"/api/execution/alarms/{condition_id}/acknowledge",
            body=body,
        )

    def acknowledge_all(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self.request(
            "POST",
            "/api/execution/alarms/acknowledge-all",
            body=body,
        )

    def history(
        self,
        *,
        params: Sequence[tuple[str, str | int]] = (),
    ) -> dict[str, Any]:
        return self.request("GET", "/api/execution/history", params=params)

    def historian_health(self) -> dict[str, Any]:
        return self.request("GET", "/api/execution/history/health")

    def reconcile_historian(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self.request(
            "POST",
            "/api/execution/history/reconcile",
            body=body,
        )

    def close(self) -> None:
        if self._owns_client:
            self._client.close()

    def __enter__(self) -> "HeadlessHttpClient":
        return self

    def __exit__(self, *_args: object) -> None:
        self.close()
