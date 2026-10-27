"""Thin bounded HTTP client for the independently running assistant bridge."""

from __future__ import annotations

from typing import Any, Mapping
from urllib.parse import quote
import json
import re
import unicodedata

import httpx


_MAXIMUM_RESPONSE_BYTES = 8 * 1024 * 1024
_STATUS_VERSION = "manyforge.assistant.provider_status.v1"
_RESPONSE_VERSION = "manyforge.assistant.provider_response.v1"
_DIGEST_PATTERN = re.compile(r"^sha256:[0-9a-f]{64}$")
_STATUS_FIELDS = {
    "schemaVersion",
    "requestId",
    "requestOwnerEpoch",
    "sourceLabel",
    "correlationId",
    "conversationId",
    "assistantMode",
    "state",
    "response",
    "error",
}
_RESPONSE_FIELDS = {
    "schemaVersion",
    "requestId",
    "requestOwnerEpoch",
    "sourceLabel",
    "correlationId",
    "conversationId",
    "assistantMode",
    "catalogHash",
    "message",
    "toolCalls",
    "warnings",
    "requiresReview",
    "mutated",
    "draftMutated",
    "needsClarification",
    "exitReason",
    "turnCount",
}
_TOOL_CALL_REQUIRED_FIELDS = {"toolId", "status", "effect", "arguments"}
_TOOL_CALL_OPTIONAL_FIELDS = {"result", "draftMutated"}


class AssistantProviderClientError(RuntimeError):
    def __init__(self, code: str, message: str, status_code: int = 503) -> None:
        super().__init__(message)
        self.code = code
        self.status_code = status_code


class AssistantProviderHttpClient:
    """Submit and reconcile requests without owning a provider registry."""

    def __init__(
        self,
        endpoint_url: str,
        *,
        timeout_seconds: float = 10.0,
        client: httpx.Client | None = None,
    ) -> None:
        endpoint = endpoint_url.strip().rstrip("/")
        if not endpoint or len(endpoint) > 4_096:
            raise ValueError("assistant bridge endpoint URL is invalid")
        if not 0 < timeout_seconds <= 60:
            raise ValueError("assistant bridge timeout must be in (0, 60]")
        if endpoint.endswith("/v1/manyforge/assistant"):
            self._endpoint = endpoint
        else:
            self._endpoint = endpoint + "/v1/manyforge/assistant"
        self._client = client or httpx.Client(timeout=timeout_seconds)
        self._owns_client = client is None

    def submit(self, body: Mapping[str, Any]) -> dict[str, Any]:
        request_id = body.get("requestId")
        return self._request(
            "POST",
            self._endpoint,
            body=body,
            expected_request_id=_request_id(request_id),
        )

    def status(self, request_id: str) -> dict[str, Any]:
        request_id = _request_id(request_id)
        return self._request(
            "GET",
            f"{self._endpoint}/{quote(request_id, safe='')}",
            expected_request_id=request_id,
        )

    def cancel(self, request_id: str) -> dict[str, Any]:
        request_id = _request_id(request_id)
        return self._request(
            "POST",
            f"{self._endpoint}/{quote(request_id, safe='')}/cancel",
            expected_request_id=request_id,
        )

    def close(self) -> None:
        if self._owns_client:
            self._client.close()
            self._owns_client = False

    def _request(
        self,
        method: str,
        url: str,
        *,
        body: Mapping[str, Any] | None = None,
        expected_request_id: str,
    ) -> dict[str, Any]:
        try:
            with self._client.stream(method, url, json=body) as response:
                chunks: list[bytes] = []
                size = 0
                for chunk in response.iter_bytes():
                    size += len(chunk)
                    if size > _MAXIMUM_RESPONSE_BYTES:
                        raise AssistantProviderClientError(
                            "assistant_provider_response_oversized",
                            "assistant bridge response exceeds its byte bound",
                            502,
                        )
                    chunks.append(chunk)
        except AssistantProviderClientError:
            raise
        except httpx.TimeoutException as error:
            raise AssistantProviderClientError(
                "assistant_provider_timeout", "assistant bridge request timed out", 504
            ) from error
        except httpx.HTTPError as error:
            raise AssistantProviderClientError(
                "assistant_provider_unavailable", str(error), 503
            ) from error
        try:
            payload = json.loads(
                b"".join(chunks),
                parse_constant=_reject_nonfinite_json,
                object_pairs_hook=_object_without_duplicates,
            )
        except (UnicodeDecodeError, json.JSONDecodeError, ValueError) as error:
            raise AssistantProviderClientError(
                "assistant_provider_response_invalid",
                "assistant bridge returned invalid JSON",
                502,
            ) from error
        if response.status_code >= 400:
            detail = payload.get("detail") if type(payload) is dict else None
            code = detail.get("code") if type(detail) is dict else None
            message = detail.get("message") if type(detail) is dict else None
            raise AssistantProviderClientError(
                code if type(code) is str else "assistant_provider_rejected",
                message if type(message) is str else "assistant bridge rejected the request",
                response.status_code,
            )
        return _status(payload, expected_request_id=expected_request_id)


def _request_id(value: Any) -> str:
    if (
        type(value) is not str
        or not value
        or value != value.strip()
        or len(value.encode("utf-8")) > 256
    ):
        raise AssistantProviderClientError(
            "assistant_request_invalid", "requestId is invalid", 400
        )
    return value


def _invalid(message: str) -> AssistantProviderClientError:
    return AssistantProviderClientError(
        "assistant_provider_response_invalid",
        message,
        502,
    )


def _text(value: Any, label: str, *, maximum: int = 256) -> str:
    if (
        type(value) is not str
        or not value
        or value != value.strip()
        or unicodedata.normalize("NFC", value) != value
        or len(value.encode("utf-8")) > maximum
    ):
        raise _invalid(f"assistant bridge {label} is invalid")
    return value


def _response(value: Any, *, status: Mapping[str, Any]) -> dict[str, Any]:
    if type(value) is not dict or set(value) != _RESPONSE_FIELDS:
        raise _invalid("assistant bridge response has an unsupported shape")
    if (
        value["schemaVersion"] != _RESPONSE_VERSION
        or value["requiresReview"] is not True
        or value["mutated"] is not False
        or type(value["draftMutated"]) is not bool
        or type(value["needsClarification"]) is not bool
    ):
        raise _invalid("assistant bridge response violates the review boundary")
    for field in (
        "requestId",
        "requestOwnerEpoch",
        "sourceLabel",
        "conversationId",
        "assistantMode",
    ):
        _text(value[field], f"response {field}")
        if value[field] != status[field]:
            raise _invalid(f"assistant bridge response changed {field}")
    if value["correlationId"] != status["correlationId"]:
        raise _invalid("assistant bridge response changed correlationId")
    if type(value["catalogHash"]) is not str or _DIGEST_PATTERN.fullmatch(
        value["catalogHash"]
    ) is None:
        raise _invalid("assistant bridge response catalogHash is invalid")
    if type(value["message"]) is not str or len(value["message"].encode("utf-8")) > 65_536:
        raise _invalid("assistant bridge response message is invalid")
    for field in ("toolCalls", "warnings"):
        if type(value[field]) is not list or len(value[field]) > 4_096:
            raise _invalid(f"assistant bridge response {field} is invalid")
    for item in value["toolCalls"]:
        if (
            type(item) is not dict
            or not _TOOL_CALL_REQUIRED_FIELDS <= set(item)
            or set(item) - _TOOL_CALL_REQUIRED_FIELDS - _TOOL_CALL_OPTIONAL_FIELDS
        ):
            raise _invalid("assistant bridge tool-call result has an unsupported shape")
        for field in ("toolId", "status", "effect"):
            _text(item[field], f"tool-call {field}")
        if item["status"] not in {"completed", "failed"}:
            raise _invalid("assistant bridge tool-call status is unsupported")
        if item["effect"] not in {"read_only", "composer_draft_mutating"}:
            raise _invalid("assistant bridge tool-call effect is unsupported")
        if type(item["arguments"]) is not dict:
            raise _invalid("assistant bridge tool-call arguments are invalid")
        if "draftMutated" in item and type(item["draftMutated"]) is not bool:
            raise _invalid("assistant bridge tool-call draftMutated is invalid")
    for item in value["warnings"]:
        _text(item, "response warning", maximum=16_384)
    _text(value["exitReason"], "response exitReason")
    if type(value["turnCount"]) is not int or not 1 <= value["turnCount"] <= 64:
        raise _invalid("assistant bridge response turnCount is invalid")
    return value


def _status(value: Any, *, expected_request_id: str) -> dict[str, Any]:
    if type(value) is not dict or set(value) != _STATUS_FIELDS:
        raise _invalid("assistant bridge status has an unsupported shape")
    if value["schemaVersion"] != _STATUS_VERSION or value["state"] not in {
        "running",
        "completed",
        "failed",
        "cancelled",
    }:
        raise _invalid("assistant bridge status version or state is unsupported")
    for field in (
        "requestId",
        "requestOwnerEpoch",
        "sourceLabel",
        "conversationId",
        "assistantMode",
    ):
        _text(value[field], f"status {field}")
    if value["requestId"] != expected_request_id:
        raise _invalid("assistant bridge changed requestId")
    if value["correlationId"] is not None:
        _text(value["correlationId"], "status correlationId")
    state = value["state"]
    if state == "completed":
        value["response"] = _response(value["response"], status=value)
    elif value["response"] is not None:
        raise _invalid("non-completed assistant status carried a response")
    if state == "failed":
        error = value["error"]
        if type(error) is not dict or set(error) != {"code", "message"}:
            raise _invalid("failed assistant status lacks its closed error")
        _text(error["code"], "status error code")
        _text(error["message"], "status error message", maximum=16_384)
    elif value["error"] is not None:
        raise _invalid("non-failed assistant status carried an error")
    return value


def _reject_nonfinite_json(value: str) -> None:
    raise ValueError(f"non-finite JSON constant is unsupported: {value}")


def _object_without_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key is unsupported: {key}")
        result[key] = value
    return result


__all__ = ["AssistantProviderClientError", "AssistantProviderHttpClient"]
