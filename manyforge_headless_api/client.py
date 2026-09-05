"""Bounded thin HTTP client shared by CLI, Composer, and assistant adapters."""

from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass
import hashlib
import json
import re
import threading
from typing import Any, Iterable, Mapping, Sequence
from urllib.parse import quote, urlsplit, urlunsplit

import httpx
from websockets.sync.client import ClientConnection, connect as connect_websocket

from .native import (
    MAX_ASSISTANT_MODE_CONTEXT_RESPONSE_BYTES,
    MAX_SOURCE_MODEL_CONTEXT_RESPONSE_BYTES,
    MAX_VIEWER_MODEL_RESOURCE_BYTES,
    MAX_VIEWER_MODEL_RESOURCE_URI_BYTES,
    MAX_VIEWER_MODEL_URDF_BYTES,
    ViewerModelAsset,
    strict_json_object,
)
from .program_binding_transport import ProgramBindingTransportError
from .program_binding_projection import (
    validate_applied_program_selection,
    validate_program_apply_result,
    validate_scenario_program_selection,
)


MAX_HTTP_RESPONSE_BYTES = 64 * 1024 * 1024
_CONDITION_ID_PATTERN = re.compile(r"^condition-[0-9a-f]{24}$")
_ASSISTANT_MODE_PATTERN = re.compile(r"^[a-z][a-z0-9.-]*$")
_SHA256_PATTERN = re.compile(r"^sha256:[0-9a-f]{64}$")
_OBSERVATION_CREDIT_SCHEMA = "manyforge.execution_observation_credit.v1"
_MAXIMUM_OBSERVATION_FRAME_BYTES = 4 * 1024 * 1024


class HeadlessClientError(RuntimeError):
    def __init__(
        self,
        code: str,
        message: str,
        status_code: int | None = None,
        *,
        details: Mapping[str, Any] | None = None,
        response: Mapping[str, Any] | None = None,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.status_code = status_code
        self.details = deepcopy(dict(details or {}))
        self.response = dict(response) if response is not None else None


@dataclass(frozen=True, slots=True)
class CallerIdentity:
    request_owner_epoch: str
    source_label: str


class HeadlessExecutionObservationStream:
    """One credited WebSocket to the Headless native-observation proxy."""

    def __init__(self, base_url: str, timeout_seconds: float) -> None:
        parsed = urlsplit(base_url)
        if parsed.scheme not in {"http", "https"} or not parsed.netloc:
            raise ValueError("headless base URL is invalid")
        path = parsed.path.rstrip("/") + "/ws/execution/observations"
        self._url = urlunsplit(
            (
                "wss" if parsed.scheme == "https" else "ws",
                parsed.netloc,
                path,
                "",
                "",
            )
        )
        self._timeout_seconds = timeout_seconds
        self._connection: ClientConnection | None = None

    def __enter__(self) -> "HeadlessExecutionObservationStream":
        self._connection = connect_websocket(
            self._url,
            open_timeout=self._timeout_seconds,
            close_timeout=min(self._timeout_seconds, 1.0),
            max_size=_MAXIMUM_OBSERVATION_FRAME_BYTES,
        )
        return self

    def close(self) -> None:
        connection = self._connection
        self._connection = None
        if connection is not None:
            connection.close()

    def __exit__(self, *_exc: object) -> None:
        self.close()

    def next(
        self,
        cursor: Mapping[str, Any] | None,
        cancelled: threading.Event,
    ) -> dict[str, Any]:
        connection = self._connection
        if connection is None:
            raise HeadlessClientError(
                "headless_observation_stream_closed",
                "headless execution observation stream is closed",
            )
        last_applied = None if cursor is None else dict(cursor)
        connection.send(
            json.dumps(
                {
                    "last_applied": last_applied,
                    "schema_version": _OBSERVATION_CREDIT_SCHEMA,
                },
                allow_nan=False,
                ensure_ascii=False,
                separators=(",", ":"),
                sort_keys=True,
            )
        )
        while not cancelled.is_set():
            try:
                payload = connection.recv(timeout=0.25)
            except TimeoutError:
                continue
            if not isinstance(payload, str):
                raise HeadlessClientError(
                    "headless_observation_stream_invalid",
                    "headless execution observation frame is not text",
                )
            try:
                return strict_json_object(
                    payload,
                    label="headless execution observation frame",
                    maximum_bytes=_MAXIMUM_OBSERVATION_FRAME_BYTES,
                )
            except RuntimeError as exc:
                raise HeadlessClientError(
                    "headless_observation_stream_invalid",
                    str(exc),
                ) from exc
        raise HeadlessClientError(
            "headless_observation_stream_cancelled",
            "headless execution observation demand ended",
        )


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
        self._base_url = base_url.rstrip("/")
        self._client = client or httpx.Client(
            base_url=self._base_url,
            timeout=httpx.Timeout(timeout_seconds),
        )
        self._timeout_seconds = timeout_seconds

    @property
    def control_websocket_url(self) -> str:
        """Public low-latency execution-control session URL."""

        parsed = urlsplit(self._base_url)
        return urlunsplit(
            (
                "wss" if parsed.scheme == "https" else "ws",
                parsed.netloc,
                parsed.path.rstrip("/") + "/ws/control",
                "",
                "",
            )
        )

    def open_execution_observation_stream(
        self,
    ) -> HeadlessExecutionObservationStream:
        return HeadlessExecutionObservationStream(
            self._base_url,
            self._timeout_seconds,
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
            details = detail.get("details")
        else:
            error = value.get("error")
            code = error.get("code") if isinstance(error, dict) else None
            message = error.get("message") if isinstance(error, dict) else None
            details = error.get("details") if isinstance(error, dict) else None
        normalized_details = dict(details) if isinstance(details, dict) else {}
        return HeadlessClientError(
            code if isinstance(code, str) and code else "headless_http_error",
            message
            if isinstance(message, str) and message
            else f"headless API returned HTTP {status_code}",
            status_code,
            details=normalized_details,
            response=value,
        )

    @staticmethod
    def _response_bound(method: str, path: str) -> int:
        maximum = MAX_HTTP_RESPONSE_BYTES
        if method.upper() == "GET":
            if path == "/api/source-model-context":
                maximum = MAX_SOURCE_MODEL_CONTEXT_RESPONSE_BYTES
            elif path.startswith("/api/assistant/modes/") and path.endswith(
                "/context"
            ):
                maximum = MAX_ASSISTANT_MODE_CONTEXT_RESPONSE_BYTES
        return maximum

    def _request_payload(
        self,
        method: str,
        path: str,
        *,
        params: Sequence[tuple[str, str | int]] = (),
        body: Mapping[str, Any] | None = None,
    ) -> tuple[bytes, int, int]:
        """Read one bounded response without imposing projection semantics."""

        if not path.startswith("/api/") or ".." in path:
            raise ValueError("headless API path is invalid")
        maximum_response_bytes = self._response_bound(method, path)
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
                    if size > maximum_response_bytes:
                        raise HeadlessClientError(
                            "headless_http_response_oversized",
                            "headless API response exceeds its byte bound",
                            response.status_code,
                        )
                    chunks.append(chunk)
                status_code = response.status_code
        except HeadlessClientError:
            raise
        except httpx.HTTPError as exc:
            raise HeadlessClientError(
                "headless_http_unavailable",
                f"headless API request failed: {exc}",
            ) from exc
        return b"".join(chunks), status_code, maximum_response_bytes

    @staticmethod
    def _decode_response(
        payload: bytes,
        *,
        status_code: int,
        maximum_response_bytes: int,
    ) -> dict[str, Any]:
        try:
            text = payload.decode("utf-8", errors="strict")
        except UnicodeDecodeError as exc:
            raise HeadlessClientError(
                "headless_http_response_invalid",
                "headless API response is not UTF-8",
                status_code,
            ) from exc
        try:
            return strict_json_object(
                text,
                label="headless HTTP response",
                maximum_bytes=maximum_response_bytes,
            )
        except RuntimeError as exc:
            raise HeadlessClientError(
                getattr(exc, "code", "headless_http_response_invalid"),
                str(exc),
                status_code,
            ) from exc

    def request(
        self,
        method: str,
        path: str,
        *,
        params: Sequence[tuple[str, str | int]] = (),
        body: Mapping[str, Any] | None = None,
    ) -> dict[str, Any]:
        payload, status_code, maximum = self._request_payload(
            method,
            path,
            params=params,
            body=body,
        )
        value = self._decode_response(
            payload,
            status_code=status_code,
            maximum_response_bytes=maximum,
        )
        if status_code >= 400:
            raise self._error_from_response(value, status_code)
        return value

    def get_json_payload(
        self,
        path: str,
        *,
        params: Sequence[tuple[str, str | int]] = (),
    ) -> bytes:
        """Relay one bounded owner JSON object without parse/re-encode churn.

        This is for transparent in-process proxy routes whose browser consumer
        already strictly validates the public DTO. Error responses are still
        decoded here so Composer preserves the normal typed HTTP failure.
        """

        payload, status_code, maximum = self._request_payload(
            "GET", path, params=params
        )
        if status_code >= 400:
            value = self._decode_response(
                payload,
                status_code=status_code,
                maximum_response_bytes=maximum,
            )
            raise self._error_from_response(value, status_code)
        try:
            text = payload.decode("utf-8", errors="strict")
        except UnicodeDecodeError as exc:
            raise HeadlessClientError(
                "headless_http_response_invalid",
                "headless API response is not UTF-8",
                status_code,
            ) from exc
        # Full public DTO validation happens once in the browser. Retaining
        # object framing here prevents an accidental non-JSON transport reply
        # without repeating the expensive parse/allocate/encode cycle.
        stripped = text.strip()
        if not stripped.startswith("{") or not stripped.endswith("}"):
            raise HeadlessClientError(
                "headless_http_response_invalid",
                "headless API response is not a JSON object",
                status_code,
            )
        return payload

    def _request_viewer_asset(
        self,
        path: str,
        *,
        params: Sequence[tuple[str, str]] = (),
        expected_kind: str,
        expected_retained_bundle_id: str | None,
        expected_logical_uri: str | None,
        maximum_bytes: int,
    ) -> ViewerModelAsset:
        try:
            with self._client.stream("GET", path, params=params) as response:
                chunks: list[bytes] = []
                size = 0
                for chunk in response.iter_bytes():
                    size += len(chunk)
                    if size > maximum_bytes:
                        raise HeadlessClientError(
                            "headless_http_response_oversized",
                            "viewer-model asset exceeds its byte bound",
                            response.status_code,
                        )
                    chunks.append(chunk)
                payload = b"".join(chunks)
                if response.status_code >= 400:
                    try:
                        error_payload = strict_json_object(
                            payload.decode("utf-8", errors="strict"),
                            label="headless viewer-model error response",
                        )
                    except (UnicodeDecodeError, RuntimeError) as exc:
                        raise HeadlessClientError(
                            "headless_http_response_invalid",
                            "headless viewer-model error response is invalid",
                            response.status_code,
                        ) from exc
                    raise self._error_from_response(
                        error_payload,
                        response.status_code,
                    )
                headers = response.headers
                metadata = {
                    "asset_kind": headers.get("x-manyforge-asset-kind"),
                    "cell_id": headers.get("x-manyforge-cell-id"),
                    "content_digest": headers.get(
                        "x-manyforge-content-digest"
                    ),
                    "graph_revision": headers.get(
                        "x-manyforge-graph-revision"
                    ),
                    "product_contract_id": headers.get(
                        "x-manyforge-product-contract-id"
                    ),
                    "product_contract_revision": headers.get(
                        "x-manyforge-product-contract-revision"
                    ),
                    "product_id": headers.get("x-manyforge-product-id"),
                    "retained_bundle_id": headers.get(
                        "x-manyforge-retained-bundle-id"
                    ),
                    "viewer_model_revision": headers.get(
                        "x-manyforge-viewer-model-revision"
                    ),
                }
                if metadata["asset_kind"] != expected_kind:
                    raise HeadlessClientError(
                        "headless_http_response_invalid",
                        "headless viewer-model asset kind is invalid",
                        response.status_code,
                    )
                for field in (
                    "cell_id",
                    "graph_revision",
                    "product_contract_id",
                    "product_id",
                ):
                    value = metadata[field]
                    if not isinstance(value, str) or not value:
                        raise HeadlessClientError(
                            "headless_http_response_invalid",
                            f"headless viewer-model {field} is invalid",
                            response.status_code,
                        )
                for field in (
                    "content_digest",
                    "product_contract_revision",
                    "retained_bundle_id",
                    "viewer_model_revision",
                ):
                    value = metadata[field]
                    if (
                        not isinstance(value, str)
                        or _SHA256_PATTERN.fullmatch(value) is None
                    ):
                        raise HeadlessClientError(
                            "headless_http_response_invalid",
                            f"headless viewer-model {field} is invalid",
                            response.status_code,
                        )
                if (
                    metadata["product_id"] != "cell_viewer_model"
                    or metadata["product_contract_id"]
                    != "manyforge.viewer_model"
                    or (
                        expected_retained_bundle_id is not None
                        and metadata["retained_bundle_id"]
                        != expected_retained_bundle_id
                    )
                ):
                    raise HeadlessClientError(
                        "headless_http_response_invalid",
                        "headless viewer-model selection identity is invalid",
                        response.status_code,
                    )
                media_type = headers.get("content-type", "").split(";", 1)[0]
                if not media_type or not payload:
                    raise HeadlessClientError(
                        "headless_http_response_invalid",
                        "headless viewer-model asset is empty or untyped",
                        response.status_code,
                    )
                actual_digest = f"sha256:{hashlib.sha256(payload).hexdigest()}"
                if metadata["content_digest"] != actual_digest:
                    raise HeadlessClientError(
                        "headless_http_response_invalid",
                        "headless viewer-model bytes do not match their digest",
                        response.status_code,
                    )
                return ViewerModelAsset(
                    asset_kind=expected_kind,
                    cell_id=metadata["cell_id"],
                    graph_revision=metadata["graph_revision"],
                    retained_bundle_id=metadata["retained_bundle_id"],
                    product_id=metadata["product_id"],
                    product_contract_id=metadata["product_contract_id"],
                    product_contract_revision=metadata[
                        "product_contract_revision"
                    ],
                    viewer_model_revision=metadata["viewer_model_revision"],
                    logical_uri=expected_logical_uri,
                    media_type=media_type,
                    byte_size=len(payload),
                    content_digest=metadata["content_digest"],
                    bytes=payload,
                )
        except HeadlessClientError:
            raise
        except httpx.HTTPError as exc:
            raise HeadlessClientError(
                "headless_http_unavailable",
                f"headless API request failed: {exc}",
            ) from exc

    def runtime(self) -> dict[str, Any]:
        return self.request("GET", "/api/runtime")

    def component_overview(self) -> dict[str, Any]:
        return self.request("GET", "/api/component-overview")

    def component_overview_payload(self) -> bytes:
        return self.get_json_payload("/api/component-overview")

    def authoring_admission(self) -> dict[str, Any]:
        return self.request("GET", "/api/cell/authoring-admission")

    def acquire_authoring_transaction(
        self, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self.request(
            "POST", "/api/program/authoring-transactions", body=body
        )

    def settle_authoring_transaction(
        self, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self.request(
            "POST", "/api/program/authoring-transactions/settle", body=body
        )

    def recover_authoring_transaction(
        self, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self.request(
            "POST",
            "/api/program/authoring-transactions/recovery-receipt",
            body=body,
        )

    def reconcile_authoring_owner(
        self, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self.request(
            "POST",
            "/api/program/authoring-transactions/reconcile-owner",
            body=body,
        )

    def health(self) -> dict[str, Any]:
        return self.request("GET", "/api/health")

    def manager_status(self) -> dict[str, Any]:
        return self.request("GET", "/api/manager/status")

    def manager_graph(self) -> dict[str, Any]:
        return self.request("GET", "/api/manager/graph")

    def manager_diff(self) -> dict[str, Any]:
        return self.request("GET", "/api/manager/diff")

    @staticmethod
    def _cclc_identity_path(prefix: str, identity: str) -> str:
        if not identity or len(identity.encode("utf-8")) > 256:
            raise ValueError("CCLC identity must be bounded non-empty text")
        if "/" in identity or ".." in identity:
            raise ValueError("CCLC identity is invalid for an HTTP path")
        return f"{prefix}/{identity}"

    def cclc_component(self, component_id: str) -> dict[str, Any]:
        return self.request(
            "GET", self._cclc_identity_path("/api/cclc/components", component_id)
        )

    def cclc_owner_identity(self) -> dict[str, Any]:
        return self.request("GET", "/api/cclc/owner")

    def cclc_route_templates(self, component_id: str) -> dict[str, Any]:
        return self.request(
            "GET",
            self._cclc_identity_path("/api/cclc/components", component_id)
            + "/route-templates",
        )

    def submit_component_lifecycle(
        self, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self.request(
            "POST", "/api/cclc/lifecycle/requests", body=body
        )

    def component_lifecycle_status(
        self, transaction_id: str
    ) -> dict[str, Any]:
        return self.request(
            "GET",
            self._cclc_identity_path(
                "/api/cclc/lifecycle/transactions", transaction_id
            ),
        )

    def submit_route_selection(
        self, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self.request("POST", "/api/cclc/routes/requests", body=body)

    def route_selection_status(self, transaction_id: str) -> dict[str, Any]:
        return self.request(
            "GET",
            self._cclc_identity_path(
                "/api/cclc/routes/transactions", transaction_id
            ),
        )

    def node_environment(self) -> dict[str, Any]:
        return self.request("GET", "/api/node-environment")

    def source_model(self) -> dict[str, Any]:
        return self.request("GET", "/api/source-model")

    def source_model_context(self) -> dict[str, Any]:
        return self.request("GET", "/api/source-model-context")

    def scenarios(self) -> dict[str, Any]:
        return self.request("GET", "/api/scenarios")

    def scenario_program_selection(self, scenario_id: str) -> dict[str, Any]:
        if (
            not isinstance(scenario_id, str)
            or not scenario_id
            or len(scenario_id.encode("utf-8")) > 128
        ):
            raise ValueError("scenario_id is invalid")
        projection = self.request(
            "GET",
            f"/api/scenarios/{quote(scenario_id, safe='')}/program-selection",
        )
        try:
            validate_scenario_program_selection(
                projection,
                scenario_id=scenario_id,
            )
        except (ProgramBindingTransportError, TypeError, ValueError) as error:
            raise HeadlessClientError(
                "scenario_program_selection_invalid",
                "headless API returned an invalid scenario Program selection",
            ) from error
        return projection

    def scenario_load_selection(self, scenario_id: str) -> dict[str, Any]:
        if (
            not isinstance(scenario_id, str)
            or not scenario_id
            or len(scenario_id.encode("utf-8")) > 128
        ):
            raise ValueError("scenario_id is invalid")
        projection = self.request(
            "GET",
            f"/api/scenarios/{quote(scenario_id, safe='')}/load-selection",
        )
        try:
            validate_scenario_program_selection(
                projection,
                scenario_id=scenario_id,
                label="scenario_load_selection",
            )
        except (ProgramBindingTransportError, TypeError, ValueError) as error:
            raise HeadlessClientError(
                "scenario_program_selection_invalid",
                "headless API returned an invalid Scenario Load selection",
            ) from error
        return projection

    def program_applied(self) -> dict[str, Any]:
        projection = self.request("GET", "/api/program/applied")
        try:
            validate_applied_program_selection(projection)
        except (ProgramBindingTransportError, TypeError, ValueError) as error:
            raise HeadlessClientError(
                "program_applied_selection_invalid",
                "headless API returned an invalid Applied Program projection",
            ) from error
        return projection

    def program_apply(self, body: Mapping[str, Any]) -> dict[str, Any]:
        result = self.request("POST", "/api/program/apply", body=body)
        try:
            validate_program_apply_result(result)
        except (ProgramBindingTransportError, TypeError, ValueError) as error:
            raise HeadlessClientError(
                "program_apply_response_invalid",
                "headless API returned an invalid Program Apply result",
                status_code=502,
            ) from error
        return result

    def program_scene_prepare(self, body: Mapping[str, Any]) -> dict[str, Any]:
        result = self.request(
            "POST", "/api/program/scene-preparations", body=body
        )
        if (
            result.get("schema_version")
            != "manyforge.program_scene_preparation.v1"
            or result.get("status") != "prepared"
            or not isinstance(result.get("prepared_scene"), dict)
        ):
            raise HeadlessClientError(
                "program_scene_preparation_invalid",
                "headless API returned an invalid scene-preparation result",
                status_code=502,
            )
        return result

    def program_cycle_status(self) -> dict[str, Any]:
        return self.request("GET", "/api/cycle/status")

    def program_cycle_reset(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self.request("POST", "/api/program/cycle/reset", body=body)

    def cycle_recovery_action(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self.request("POST", "/api/cycle/recovery-actions", body=body)

    def commit_program_model_attempt(
        self,
        record: Mapping[str, Any],
    ) -> dict[str, Any]:
        return self.request(
            "POST",
            "/api/program/model-attempts",
            body=record,
        )

    @staticmethod
    def _program_run_path(program_run_id: str) -> str:
        if (
            not isinstance(program_run_id, str)
            or not program_run_id
            or len(program_run_id.encode("utf-8")) > 256
            or "/" in program_run_id
            or ".." in program_run_id
        ):
            raise ValueError("program_run_id is invalid")
        return f"/api/program/runs/{quote(program_run_id, safe='')}"

    def program_runs(
        self,
        *,
        active_only: bool = False,
        limit: int = 128,
    ) -> dict[str, Any]:
        if type(active_only) is not bool:
            raise ValueError("active_only must be boolean")
        if type(limit) is not int or not 1 <= limit <= 4_096:
            raise ValueError("Program-run index limit is outside its bound")
        return self.request(
            "GET",
            "/api/program/runs",
            params=(
                ("active_only", "true" if active_only else "false"),
                ("limit", limit),
            ),
        )

    def program_run_start(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self.request("POST", "/api/program/runs", body=body)

    def program_run(self, program_run_id: str) -> dict[str, Any]:
        return self.request("GET", self._program_run_path(program_run_id))

    def program_run_stop(
        self,
        program_run_id: str,
        body: Mapping[str, Any],
    ) -> dict[str, Any]:
        return self.request(
            "POST",
            f"{self._program_run_path(program_run_id)}/stop",
            body=body,
        )

    def program_run_motion_stop(
        self,
        program_run_id: str,
        body: Mapping[str, Any],
    ) -> dict[str, Any]:
        return self.request(
            "POST",
            f"{self._program_run_path(program_run_id)}/motion-stop",
            body=body,
        )

    def program_run_full_stop(
        self,
        program_run_id: str,
        body: Mapping[str, Any],
    ) -> dict[str, Any]:
        return self.request(
            "POST",
            f"{self._program_run_path(program_run_id)}/full-stop",
            body=body,
        )

    def program_run_resume_full_stop(
        self,
        program_run_id: str,
        body: Mapping[str, Any],
    ) -> dict[str, Any]:
        return self.request(
            "POST",
            f"{self._program_run_path(program_run_id)}/resume-full-stop",
            body=body,
        )

    def program_run_reset_full_stop(
        self,
        program_run_id: str,
        body: Mapping[str, Any],
    ) -> dict[str, Any]:
        return self.request(
            "POST",
            f"{self._program_run_path(program_run_id)}/reset",
            body=body,
        )

    def program_runtime_controls(self, program_run_id: str) -> dict[str, Any]:
        return self.request(
            "GET", f"{self._program_run_path(program_run_id)}/runtime-controls"
        )

    def program_runtime_control_action(
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
        return self.request(
            "POST",
            f"{self._program_run_path(program_run_id)}/{suffix}",
            body=body,
        )

    def viewer_model_urdf(self) -> ViewerModelAsset:
        asset = self._request_viewer_asset(
            "/api/viewer-model/urdf",
            expected_kind="urdf",
            expected_retained_bundle_id=None,
            expected_logical_uri=None,
            maximum_bytes=MAX_VIEWER_MODEL_URDF_BYTES,
        )
        if asset.media_type != "application/xml":
            raise HeadlessClientError(
                "headless_http_response_invalid",
                "headless viewer-model URDF has the wrong media type",
            )
        return asset

    def viewer_model_resource(
        self,
        retained_bundle_id: str,
        logical_uri: str,
    ) -> ViewerModelAsset:
        if _SHA256_PATTERN.fullmatch(retained_bundle_id) is None:
            raise ValueError("retained bundle ID is invalid")
        if (
            not isinstance(logical_uri, str)
            or not logical_uri
            or len(logical_uri.encode("utf-8"))
            > MAX_VIEWER_MODEL_RESOURCE_URI_BYTES
            or "\x00" in logical_uri
        ):
            raise ValueError("viewer-model resource URI is invalid")
        return self._request_viewer_asset(
            "/api/viewer-model/resource",
            params=(
                ("retained_bundle_id", retained_bundle_id),
                ("logical_uri", logical_uri),
            ),
            expected_kind="resource",
            expected_retained_bundle_id=retained_bundle_id,
            expected_logical_uri=logical_uri,
            maximum_bytes=MAX_VIEWER_MODEL_RESOURCE_BYTES,
        )

    def assistant_modes(self) -> dict[str, Any]:
        return self.request("GET", "/api/assistant/modes")

    def assistant_mode(self, mode_id: str) -> dict[str, Any]:
        if (
            not isinstance(mode_id, str)
            or len(mode_id.encode("utf-8")) > 256
            or _ASSISTANT_MODE_PATTERN.fullmatch(mode_id) is None
        ):
            raise ValueError("assistant mode ID is invalid")
        return self.request("GET", f"/api/assistant/modes/{mode_id}")

    def assistant_mode_context(self, mode_id: str) -> dict[str, Any]:
        if (
            not isinstance(mode_id, str)
            or re.fullmatch(r"[a-z][a-z0-9.-]*", mode_id) is None
            or len(mode_id.encode("utf-8")) > 256
        ):
            raise ValueError("assistant mode identity is invalid")
        return self.request("GET", f"/api/assistant/modes/{mode_id}/context")

    def manager_deployment_candidate_preview(
        self, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self.request(
            "POST", "/api/manager/deployment-candidates/preview", body=body
        )

    def compile_deployment_candidate(
        self, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self.request(
            "POST", "/api/manager/deployment-candidates/compile", body=body
        )

    def manager_apply(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self.request("POST", "/api/manager/apply", body=body)

    def manager_apply_status(
        self, apply_id: str, apply_generation: int
    ) -> dict[str, Any]:
        if (
            not isinstance(apply_id, str)
            or not apply_id
            or len(apply_id.encode("utf-8")) > 256
            or type(apply_generation) is not int
            or not 1 <= apply_generation <= 9_007_199_254_740_991
        ):
            raise ValueError("Manager Apply identity is invalid")
        return self.request(
            "GET",
            "/api/manager/apply/status",
            params=(("apply_id", apply_id), ("apply_generation", apply_generation)),
        )

    def execution_diagnostics(self) -> dict[str, Any]:
        return self.request("GET", "/api/execution/diagnostics")

    def execution_status(self) -> dict[str, Any]:
        """Compatibility alias for the aggregate diagnostic projection."""
        return self.request("GET", "/api/execution/status")

    def execution_identity(self) -> dict[str, Any]:
        return self.request("GET", "/api/execution/identity")

    def execution_targets(self) -> dict[str, Any]:
        return self.request("GET", "/api/execution/targets")

    def execution_control_state(self) -> dict[str, Any]:
        return self.request("GET", "/api/execution/control-state")

    def control_projection(self) -> dict[str, Any]:
        """Read the public transport-neutral execution-control projection."""

        return self.request("GET", "/api/control/state")

    def submit_control_command(self, body: Mapping[str, Any]) -> dict[str, Any]:
        """Submit one strict public command envelope without adding semantics."""

        return self.request("POST", "/api/control/commands", body=body)

    def execute(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self.request("POST", "/api/execution/operations", body=body)

    def preview_motion(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self.request(
            "POST", "/api/execution/motion-previews", body=body
        )

    def preview_capability_command(
        self, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self.request(
            "POST", "/api/execution/capability-command-previews", body=body
        )

    def plan_pick(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self.request(
            "POST", "/api/execution/pick-place/plans/pick", body=body
        )

    def plan_place(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self.request(
            "POST", "/api/execution/pick-place/plans/place", body=body
        )

    def start_pick(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self.request(
            "POST", "/api/execution/pick-place/sequences/pick", body=body
        )

    def start_place(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self.request(
            "POST", "/api/execution/pick-place/sequences/place", body=body
        )

    @staticmethod
    def _pick_place_sequence_path(sequence_id: str) -> str:
        if not sequence_id or len(sequence_id.encode("utf-8")) > 96:
            raise ValueError("sequence_id must be bounded non-empty text")
        if "/" in sequence_id or ".." in sequence_id:
            raise ValueError("sequence_id is invalid for an HTTP path")
        return f"/api/execution/pick-place/sequences/{sequence_id}"

    def pick_place_sequence(self, sequence_id: str) -> dict[str, Any]:
        return self.request("GET", self._pick_place_sequence_path(sequence_id))

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

    def recognized_malformed_stop(self, path: str) -> dict[str, Any]:
        if path not in {"/api/execution/stops", "/api/execution/full-stop"}:
            raise ValueError("recognized malformed Stop path is invalid")
        # The Headless adapter is the semantic Stop ingress. Forward one
        # deliberately non-normalizable bounded body through the exact route
        # so it mints the internal malformed-ingress identity and performs the
        # conservative Full-Stop promotion; Composer must not invent that
        # identity or duplicate Kernel admission semantics.
        return self.request("POST", path, body={})

    def full_stop(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self.request("POST", "/api/execution/full-stop", body=body)

    def stop_fault(self) -> dict[str, Any]:
        return self.request("GET", "/api/execution/stop-fault")

    def recover_stop_fault(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self.request(
            "POST", "/api/execution/stop-fault/recover", body=body
        )

    def set_runtime_scale_control(
        self, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self.request(
            "POST", "/api/execution/runtime-scale-control", body=body
        )

    def runtime_scale_control(
        self, selector: Mapping[str, str]
    ) -> dict[str, Any]:
        expected = {
            "program_binding_id",
            "component_role_id",
            "target_role_id",
        }
        if set(selector) != expected or any(not selector[key] for key in expected):
            raise ValueError("runtime-scale control selector is incomplete")
        return self.request(
            "GET",
            "/api/execution/runtime-scale-control",
            params=tuple((key, selector[key]) for key in sorted(expected)),
        )

    def scene(self) -> dict[str, Any]:
        return self.request("GET", "/api/scene")

    def resolve_scene_resources(
        self, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self.request(
            "POST", "/api/scene/resource-poses/query", body=body
        )

    def commit_scene_transaction(
        self, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self.request("POST", "/api/scene/transactions", body=body)

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

    def history_payload(
        self,
        *,
        params: Sequence[tuple[str, str | int]] = (),
    ) -> bytes:
        return self.get_json_payload("/api/execution/history", params=params)

    def history_detail(self, sequence: int) -> dict[str, Any]:
        if not 1 <= sequence <= 9_007_199_254_740_991:
            raise ValueError("history sequence must be a positive safe integer")
        return self.request("GET", f"/api/execution/history/{sequence}")

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
