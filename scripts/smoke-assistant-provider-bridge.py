#!/usr/bin/env python3
"""Check the active ai_model endpoint and its read-only assistant tool path.

This host-side smoke uses Composer's current ``/api/assistant`` routes. It
reads the active ``composer-assistant`` projection, checks the named model at
``{base_url}/models``, then asks the assistant to read the Program Draft with
the sole allowed tool ``program_read``. It never submits a Draft mutation.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from typing import Any, Final


ASSISTANT_MODE: Final = "composer-assistant"
SOURCE_LABEL: Final = "assistant_smoke_cli"
READ_TOOL: Final = "program_read"
STATUS_POLL_INTERVAL_S: Final = 0.5
NOT_FOUND_READ_LIMIT: Final = 4
STATUS_READ_FAILURE_LIMIT: Final = 4
MAX_RESPONSE_BYTES: Final = 2 * 1024 * 1024
AI_TOKEN_ENV_PATTERN: Final = re.compile(r"MANYFORGE_AI_[A-Z0-9_]+\Z")


class SmokeFailure(RuntimeError):
    """A bounded assistant smoke check failed."""


class HttpFailure(SmokeFailure):
    """An HTTP request failed with a known status code."""

    def __init__(self, status_code: int, detail: str) -> None:
        super().__init__(detail)
        self.status_code = status_code


@dataclass(frozen=True)
class ModelTarget:
    component_id: str
    model_id: str
    base_url: str
    api_key_env: str | None


def _json_bytes(payload: dict[str, Any]) -> bytes:
    try:
        return json.dumps(
            payload,
            ensure_ascii=False,
            allow_nan=False,
            separators=(",", ":"),
        ).encode("utf-8")
    except (TypeError, ValueError) as error:
        raise SmokeFailure(f"request is not valid JSON: {error}") from error


def _read_json_response(response: Any, *, label: str) -> dict[str, Any]:
    raw = response.read(MAX_RESPONSE_BYTES + 1)
    if len(raw) > MAX_RESPONSE_BYTES:
        raise SmokeFailure(f"{label} response exceeds the 2 MiB smoke bound")
    try:
        value = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise SmokeFailure(f"{label} returned invalid JSON") from error
    if type(value) is not dict:
        raise SmokeFailure(f"{label} response must be a JSON object")
    return value


def _http_json(
    method: str,
    url: str,
    *,
    payload: dict[str, Any] | None = None,
    timeout_s: float,
    headers: dict[str, str] | None = None,
) -> dict[str, Any]:
    request_headers = {"Accept": "application/json"}
    if headers:
        request_headers.update(headers)
    data = None
    if payload is not None:
        request_headers["Content-Type"] = "application/json"
        data = _json_bytes(payload)
    request = urllib.request.Request(
        url,
        data=data,
        headers=request_headers,
        method=method,
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout_s) as response:
            return _read_json_response(response, label=f"{method} {url}")
    except urllib.error.HTTPError as error:
        detail = ""
        try:
            raw = error.read(16_384)
            decoded = raw.decode("utf-8", errors="replace")
            try:
                body = json.loads(decoded)
                if isinstance(body, dict):
                    inner = body.get("detail", body)
                    if isinstance(inner, dict):
                        code = inner.get("code")
                        message = inner.get("message")
                        if isinstance(code, str) and isinstance(message, str):
                            detail = f" {code}: {message}"
                        elif isinstance(message, str):
                            detail = f" {message}"
                if not detail:
                    detail = f" {decoded[:1_000]}" if decoded else ""
            except json.JSONDecodeError:
                detail = f" {decoded[:1_000]}" if decoded else ""
        except OSError:
            pass
        raise HttpFailure(
            error.code,
            f"HTTP {error.code} for {method} {url}.{detail}",
        ) from error
    except (urllib.error.URLError, TimeoutError, OSError) as error:
        reason = getattr(error, "reason", error)
        raise SmokeFailure(f"{method} {url} failed: {reason}") from error


def composer_get(
    composer_url: str,
    path: str,
    *,
    timeout_s: float,
) -> dict[str, Any]:
    return _http_json(
        "GET",
        composer_url.rstrip("/") + path,
        timeout_s=timeout_s,
    )


def _model_url(base_url: str) -> str:
    parsed = urllib.parse.urlsplit(base_url)
    if (
        parsed.scheme not in {"http", "https"}
        or not parsed.netloc
        or parsed.username is not None
        or parsed.password is not None
        or parsed.query
        or parsed.fragment
    ):
        raise SmokeFailure(
            "active model base_url must be an absolute HTTP(S) URL without "
            "credentials, query, or fragment"
        )
    return base_url.rstrip("/") + "/models"


def _model_auth_headers(api_key_env: str | None) -> dict[str, str]:
    if api_key_env is None:
        return {}
    if AI_TOKEN_ENV_PATTERN.fullmatch(api_key_env) is None:
        raise SmokeFailure(
            "active api_key_env is not a MANYFORGE_AI_[A-Z0-9_]+ name"
        )
    token = os.environ.get(api_key_env)
    if not token:
        raise SmokeFailure(
            f"environment variable {api_key_env} is not exported in this shell"
        )
    return {"Authorization": f"Bearer {token}"}


def _check_model_server(target: ModelTarget, *, timeout_s: float) -> None:
    url = _model_url(target.base_url)
    response = _http_json(
        "GET",
        url,
        timeout_s=timeout_s,
        headers=_model_auth_headers(target.api_key_env),
    )
    rows = response.get("data")
    if type(rows) is not list:
        raise SmokeFailure("model server GET /models response has no data array")
    served_ids = {
        row.get("id")
        for row in rows
        if type(row) is dict and type(row.get("id")) is str
    }
    if target.model_id not in served_ids:
        listed = sorted(served_ids)
        raise SmokeFailure(
            f"model server does not list the active model {target.model_id!r}; "
            f"served IDs: {listed[:20]}"
        )


def preflight(
    composer_url: str,
    *,
    timeout_s: float,
    expected_component_id: str | None = None,
) -> ModelTarget:
    """Verify Composer, the chat alias projection, and its served model."""

    mode_index = composer_get(
        composer_url,
        "/api/assistant/modes",
        timeout_s=timeout_s,
    )
    if mode_index.get("schema_version") != "manyforge.assistant_mode_index.v2":
        raise SmokeFailure("Composer returned an unsupported assistant mode index")
    assistant_component_id = mode_index.get("assistant_component_id")
    if type(assistant_component_id) is not str or not assistant_component_id:
        raise SmokeFailure("active Deployment has no assistant component")
    modes = mode_index.get("modes")
    if type(modes) is not list or not any(
        type(row) is dict and row.get("mode") == ASSISTANT_MODE for row in modes
    ):
        raise SmokeFailure(f"active Deployment has no {ASSISTANT_MODE!r} mode")
    component_id = mode_index.get("ai_model_component_id")
    if type(component_id) is not str or not component_id:
        raise SmokeFailure("active chat alias has no ai_model component")
    if expected_component_id is not None and component_id != expected_component_id:
        raise SmokeFailure(
            f"chat alias resolves to {component_id!r}, expected {expected_component_id!r}"
        )

    mode = composer_get(
        composer_url,
        f"/api/assistant/modes/{ASSISTANT_MODE}",
        timeout_s=timeout_s,
    )
    if mode.get("schema_version") != "manyforge.assistant_mode_projection.v2":
        raise SmokeFailure("Composer returned an unsupported assistant mode projection")
    endpoint = mode.get("model_endpoint")
    if type(endpoint) is not dict:
        raise SmokeFailure("active assistant mode has no model_endpoint")
    if (
        mode.get("mode") != ASSISTANT_MODE
        or endpoint.get("model_endpoint_alias") != "assistant_model"
        or endpoint.get("ai_model_component_id") != component_id
    ):
        raise SmokeFailure(
            "chat alias projection does not match the active ai_model component"
        )
    model_id = endpoint.get("model_id")
    base_url = endpoint.get("base_url")
    api_key_env = endpoint.get("api_key_env")
    if type(model_id) is not str or not model_id.strip():
        raise SmokeFailure("active ai_model component has no model id")
    if type(base_url) is not str or not base_url.strip():
        raise SmokeFailure("active ai_model component has no base_url")
    if api_key_env is not None and type(api_key_env) is not str:
        raise SmokeFailure("active ai_model api_key_env must be a string or null")

    target = ModelTarget(
        component_id=component_id,
        model_id=model_id,
        base_url=base_url,
        api_key_env=api_key_env,
    )
    _check_model_server(target, timeout_s=timeout_s)
    return target


def draft_snapshot(composer_url: str, *, timeout_s: float) -> dict[str, Any]:
    draft = composer_get(composer_url, "/api/program/draft", timeout_s=timeout_s)
    if (
        draft.get("program_loaded") is not True
        or type(draft.get("authoring")) is not dict
    ):
        raise SmokeFailure(
            "load a Program Draft in Composer before running the assistant smoke"
        )
    return draft


def make_request(
    *,
    message: str,
    requested_tools: list[str],
    conversation_id: str | None = None,
) -> dict[str, Any]:
    request_id = f"assistant-smoke.{uuid.uuid4().hex}"
    return {
        "requestId": request_id,
        "requestOwnerEpoch": f"assistant-smoke-owner.{uuid.uuid4().hex}",
        "sourceLabel": SOURCE_LABEL,
        "correlationId": None,
        "conversationId": conversation_id
        or f"assistant-smoke-conversation.{uuid.uuid4().hex}",
        "assistantMode": ASSISTANT_MODE,
        "message": message,
        "requestedTools": sorted(set(requested_tools)),
        "fieldPolicy": {},
    }


def _validate_status_identity(
    status: dict[str, Any],
    request: dict[str, Any],
) -> None:
    for field in (
        "requestId",
        "requestOwnerEpoch",
        "sourceLabel",
        "correlationId",
        "conversationId",
        "assistantMode",
    ):
        if status.get(field) != request[field]:
            raise SmokeFailure(
                f"assistant status changed request identity field {field}"
            )


def submit_and_wait(
    composer_url: str,
    request: dict[str, Any],
    *,
    timeout_s: float,
    http_timeout_s: float,
) -> dict[str, Any]:
    """Submit once and reconcile that request by ID; never resubmit it."""

    request_id = request["requestId"]
    accepted: dict[str, Any] | None = None
    submit_failure: Exception | None = None
    try:
        accepted = _http_json(
            "POST",
            composer_url.rstrip("/") + "/api/assistant/requests",
            payload=request,
            timeout_s=http_timeout_s,
        )
        _validate_status_identity(accepted, request)
    except SmokeFailure as error:
        submit_failure = error

    deadline = time.monotonic() + timeout_s
    not_found_count = 0
    read_failure_count = 0
    status = accepted
    while time.monotonic() < deadline:
        if status is None or status.get("state") == "running":
            try:
                status = composer_get(
                    composer_url,
                    f"/api/assistant/requests/{urllib.parse.quote(request_id, safe='')}",
                    timeout_s=http_timeout_s,
                )
                _validate_status_identity(status, request)
                read_failure_count = 0
                not_found_count = 0
            except HttpFailure as error:
                if error.status_code == 404:
                    not_found_count += 1
                    if not_found_count >= NOT_FOUND_READ_LIMIT:
                        detail = (
                            f"no bridge status was found for {request_id}; "
                            "the submission outcome is unknown"
                        )
                        if submit_failure is not None:
                            detail += f" (submit returned: {submit_failure})"
                        raise SmokeFailure(detail) from error
                else:
                    read_failure_count += 1
                    if read_failure_count >= STATUS_READ_FAILURE_LIMIT:
                        raise SmokeFailure(
                            f"status for {request_id} is unreachable; its outcome is unknown"
                        ) from error
            except SmokeFailure as error:
                read_failure_count += 1
                if read_failure_count >= STATUS_READ_FAILURE_LIMIT:
                        raise SmokeFailure(
                            f"status for {request_id} could not be reconciled; "
                            f"its outcome is unknown: {error}"
                        ) from error
            if status is None or status.get("state") == "running":
                time.sleep(
                    min(
                        STATUS_POLL_INTERVAL_S,
                        max(0.0, deadline - time.monotonic()),
                    )
                )
                continue
        _validate_status_identity(status, request)
        state = status.get("state")
        if state not in {"completed", "failed", "cancelled"}:
            raise SmokeFailure(f"assistant returned unsupported terminal state {state!r}")
        if state != "completed":
            error = status.get("error")
            if type(error) is dict:
                code, message = error.get("code"), error.get("message")
                raise SmokeFailure(f"assistant request {state}: {code}: {message}")
            raise SmokeFailure(f"assistant request ended in state {state}")
        return status

    raise SmokeFailure(
        f"request {request_id} is still running or its outcome is unknown; "
        "inspect Composer before retrying"
    )


def completed_response(
    status: dict[str, Any],
    request: dict[str, Any],
    *,
    require_draft_mutation: bool,
) -> dict[str, Any]:
    response = status.get("response")
    if type(response) is not dict:
        raise SmokeFailure("completed assistant status has no response")
    if (
        response.get("schemaVersion")
        != "manyforge.assistant.provider_response.v1"
    ):
        raise SmokeFailure("assistant returned an unsupported provider response")
    _validate_status_identity(response, request)
    if response.get("requiresReview") is not True or response.get("mutated") is not False:
        raise SmokeFailure("assistant response violated the Draft review boundary")
    if response.get("draftMutated") is not require_draft_mutation:
        expected = "a Draft mutation" if require_draft_mutation else "no Draft mutation"
        raise SmokeFailure(f"assistant response did not report {expected}")
    calls = response.get("toolCalls")
    if type(calls) is not list:
        raise SmokeFailure("assistant response has no toolCalls array")
    return response


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--composer-url",
        default=os.environ.get("MANYFORGE_COMPOSER_URL", "http://127.0.0.1:3000"),
        help="Composer API root (default: %(default)s)",
    )
    parser.add_argument(
        "--expected-component-id",
        help="optionally require this component for the chat alias",
    )
    parser.add_argument(
        "--timeout-seconds",
        type=float,
        default=180.0,
        help="maximum time to reconcile the assistant request (default: %(default)s)",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    if not 1.0 <= args.timeout_seconds <= 900.0:
        raise SystemExit("--timeout-seconds must be between 1 and 900")
    try:
        target = preflight(
            args.composer_url,
            timeout_s=min(15.0, args.timeout_seconds),
            expected_component_id=args.expected_component_id,
        )
        before_draft = draft_snapshot(
            args.composer_url,
            timeout_s=min(15.0, args.timeout_seconds),
        )
        request = make_request(
            message=(
                "Read the current Program Draft using the program_read tool. "
                "Then describe its top-level contents in one short sentence. "
                "Do not edit the Draft."
            ),
            requested_tools=[READ_TOOL],
        )
        status = submit_and_wait(
            args.composer_url,
            request,
            timeout_s=args.timeout_seconds,
            http_timeout_s=min(15.0, args.timeout_seconds),
        )
        after_draft = draft_snapshot(
            args.composer_url,
            timeout_s=min(15.0, args.timeout_seconds),
        )
        if before_draft != after_draft:
            raise SmokeFailure(
                "Composer Draft snapshot changed during the read-only request; "
                "inspect for concurrent Draft edits before interpreting the result"
            )
        response = completed_response(
            status,
            request,
            require_draft_mutation=False,
        )
        calls = response["toolCalls"]
        if len(calls) != 1:
            raise SmokeFailure(f"expected one read tool call, received {len(calls)}")
        call = calls[0]
        if (
            type(call) is not dict
            or call.get("toolId") != READ_TOOL
            or call.get("status") != "completed"
            or call.get("effect") != "read_only"
            or call.get("draftMutated") is not False
        ):
            raise SmokeFailure("program_read did not complete as a read-only tool call")
        print(
            "PASS: assistant provider and Draft read; before/after Composer "
            "Draft snapshots match; "
            f"component={target.component_id} model={target.model_id} "
            f"request={request['requestId']}"
        )
        return 0
    except SmokeFailure as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
