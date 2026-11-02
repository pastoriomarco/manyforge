"""Concrete transport for compiler-qualified Program model requests."""

from __future__ import annotations

from collections.abc import Mapping
import json
import os
from threading import Event, RLock
from typing import Any, Final

import httpx

from manyforge_behavior.assistant_program_port import (
    ProgramAssistantPortError,
    ProgramModelRequest,
    ProgramModelResponse,
    ProgramModelTransportError,
    validate_program_snapshot_attachment,
)
from manyforge_headless_api.native import HeadlessCorePort


_MAX_RESPONSE_BYTES: Final = 4 * 1024 * 1024
_MAX_REQUEST_BYTES: Final = 8 * 1024 * 1024
_STREAM_CHUNK_BYTES: Final = 64 * 1024
_TOOL_CALL_FAMILY: Final = "openai_chat_tools"


class ProgramModelAuditCommitError(RuntimeError):
    """The synchronous Historian boundary rejected an attempt record."""


class HeadlessProgramModelAuditSink:
    """Commit every terminal model attempt before its task can advance."""

    def __init__(self, core: HeadlessCorePort) -> None:
        if not callable(getattr(core, "commit_program_model_attempt", None)):
            raise TypeError("core must expose the Program model-attempt boundary")
        self._core = core

    def commit(self, record: Mapping[str, Any]) -> None:
        if not isinstance(record, Mapping):
            raise TypeError("Program model-attempt record must be a mapping")
        model_attempt_id = record.get("model_attempt_id")
        if not isinstance(model_attempt_id, str) or not model_attempt_id:
            raise ProgramModelAuditCommitError(
                "Program model-attempt identity is absent"
            )
        response = self._core.commit_program_model_attempt(record)
        if (
            not isinstance(response, Mapping)
            or set(response)
            != {
                "event_id",
                "model_attempt_id",
                "schema_version",
                "status",
            }
            or response.get("schema_version")
            != "manyforge.program_model_attempt_commit.v1"
            or response.get("model_attempt_id") != model_attempt_id
            or response.get("status") not in {"stored", "duplicate"}
            or not isinstance(response.get("event_id"), str)
            or not response["event_id"]
        ):
            raise ProgramModelAuditCommitError(
                "Historian returned an invalid Program model-attempt receipt"
            )


def _strict_object(raw: bytes, *, label: str) -> dict[str, Any]:
    def object_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate key {key!r}")
            result[key] = value
        return result

    try:
        value = json.loads(
            raw,
            parse_constant=lambda token: (_ for _ in ()).throw(
                ValueError(f"non-finite number {token}")
            ),
            object_pairs_hook=object_pairs,
        )
    except (RecursionError, UnicodeError, ValueError, TypeError) as exc:
        raise ProgramModelTransportError(f"{label} is not strict JSON: {exc}") from exc
    if not isinstance(value, dict):
        raise ProgramModelTransportError(f"{label} root must be an object")
    return value


class OpenAICompatibleProgramModelTransport:
    """Bounded OpenAI-compatible transport to the Deployment's AI model component.

    Each request carries the component's ``base_url`` and token variable name;
    the token is read from the environment at call time.
    """

    def __init__(self, *, client: httpx.Client | None = None) -> None:
        self._client = client or httpx.Client()
        self._owns_client = client is None
        self._closed = False
        self._lock = RLock()

    def validate_endpoint(
        self,
        *,
        model_endpoint_alias: str,
        external_resource_alias: str,
        model_id: str,
        request_contract: Mapping[str, Any],
    ) -> None:
        if not isinstance(request_contract, Mapping):
            raise ProgramModelTransportError("model request contract is invalid")
        if request_contract.get("tool_call_family") != _TOOL_CALL_FAMILY:
            raise ProgramModelTransportError(
                "the OpenAI-compatible transport supports openai_chat_tools only"
            )

    @staticmethod
    def _request_body(request: ProgramModelRequest) -> dict[str, Any]:
        content: str | list[dict[str, Any]] = request.prompt
        if request.attachment is not None:
            attachment_kind = request.attachment.get("kind")
            if attachment_kind == "snapshot_image":
                try:
                    attachment = validate_program_snapshot_attachment(
                        request.attachment
                    )
                except ProgramAssistantPortError as exc:
                    raise ProgramModelTransportError(
                        "model transport received an invalid snapshot attachment"
                    ) from exc
                content = [
                    {"type": "text", "text": request.prompt},
                    {
                        "type": "image_url",
                        "image_url": {"url": attachment["data_url"]},
                    },
                ]
            elif attachment_kind != "state_json":
                raise ProgramModelTransportError(
                    "model transport received an unsupported attachment"
                )
            else:
                state_json = request.attachment.get("state_json")
                if isinstance(state_json, str):
                    try:
                        state_value = _strict_object(
                            state_json.encode("utf-8"),
                            label="state attachment",
                        )
                    except UnicodeError as exc:
                        raise ProgramModelTransportError(
                            "state attachment is not UTF-8 JSON"
                        ) from exc
                elif isinstance(state_json, Mapping):
                    state_value = dict(state_json)
                else:
                    raise ProgramModelTransportError(
                        "state attachment root must be an object"
                    )
                try:
                    encoded = json.dumps(
                        state_value,
                        allow_nan=False,
                        ensure_ascii=False,
                        separators=(",", ":"),
                        sort_keys=True,
                    )
                except (RecursionError, TypeError, ValueError) as exc:
                    raise ProgramModelTransportError(
                        "state attachment is not finite JSON"
                    ) from exc
                content = f"{content}\n\nAttached state JSON:\n{encoded}"
        body: dict[str, Any] = {
            "model": request.model_id,
            "messages": [{"role": "user", "content": content}],
            "max_tokens": request.max_output_tokens,
            "stream": False,
        }
        if request.response_mode == "json_object":
            body["response_format"] = {"type": "json_object"}
        else:
            assert request.response_tool is not None
            tool = dict(request.response_tool)
            body["tools"] = [{"type": "function", "function": tool}]
            body["tool_choice"] = {
                "type": "function",
                "function": {"name": tool["name"]},
            }
        return body

    @classmethod
    def _request_bytes(cls, request: ProgramModelRequest) -> bytes:
        try:
            encoded = json.dumps(
                cls._request_body(request),
                allow_nan=False,
                ensure_ascii=False,
                separators=(",", ":"),
                sort_keys=True,
            ).encode("utf-8")
        except (RecursionError, TypeError, ValueError) as exc:
            raise ProgramModelTransportError(
                "model request is not finite JSON"
            ) from exc
        if len(encoded) > _MAX_REQUEST_BYTES:
            raise ProgramModelTransportError(
                "model request exceeds its fixed byte bound"
            )
        return encoded

    def complete(
        self,
        request: ProgramModelRequest,
        cancel_event: Event,
    ) -> ProgramModelResponse:
        if not isinstance(request, ProgramModelRequest) or not isinstance(
            cancel_event, Event
        ):
            raise TypeError("model request or cancellation token is invalid")
        if cancel_event.is_set():
            raise ProgramModelTransportError("model request was cancelled")
        if request.tool_call_family != _TOOL_CALL_FAMILY:
            raise ProgramModelTransportError(
                "the OpenAI-compatible transport supports openai_chat_tools only"
            )
        with self._lock:
            if self._closed:
                raise ProgramModelTransportError("model transport is closed")
        headers = {"Content-Type": "application/json"}
        if request.api_key_env is not None:
            token = os.environ.get(request.api_key_env)
            if not token:
                raise ProgramModelTransportError(
                    f"the AI model token variable {request.api_key_env} is unset",
                    reason_code="ai_model_token_unavailable",
                )
            headers["Authorization"] = f"Bearer {token}"
        request_bytes = self._request_bytes(request)
        try:
            with self._client.stream(
                "POST",
                request.base_url.rstrip("/") + "/chat/completions",
                headers=headers,
                content=request_bytes,
                timeout=httpx.Timeout(request.timeout_s),
            ) as response:
                if response.status_code < 200 or response.status_code >= 300:
                    raise ProgramModelTransportError(
                        f"model endpoint returned HTTP {response.status_code}"
                    )
                body = bytearray()
                for chunk in response.iter_bytes(
                    chunk_size=_STREAM_CHUNK_BYTES
                ):
                    if cancel_event.is_set():
                        raise ProgramModelTransportError(
                            "model request was cancelled"
                        )
                    if len(body) + len(chunk) > _MAX_RESPONSE_BYTES:
                        raise ProgramModelTransportError(
                            "model response exceeds its fixed byte bound"
                        )
                    body.extend(chunk)
        except ProgramModelTransportError:
            raise
        except httpx.TimeoutException as exc:
            raise ProgramModelTransportError("model request timed out") from exc
        except httpx.HTTPError as exc:
            # The provider URL and low-level client diagnostic may contain
            # site-only routing or credential material.  Keep them in the
            # exception chain for local debugging, never in the Program's
            # durable public attempt record.
            raise ProgramModelTransportError("model transport failed") from exc
        if cancel_event.is_set():
            raise ProgramModelTransportError("model request was cancelled")
        payload = _strict_object(bytes(body), label="model response")
        choices = payload.get("choices")
        if not isinstance(choices, list) or len(choices) != 1:
            raise ProgramModelTransportError(
                "model response must contain exactly one choice"
            )
        choice = choices[0]
        message = choice.get("message") if isinstance(choice, Mapping) else None
        if not isinstance(message, Mapping):
            raise ProgramModelTransportError("model response message is invalid")
        model_version = payload.get("model", "")
        usage = payload.get("usage")
        if not isinstance(model_version, str) or (
            usage is not None and not isinstance(usage, Mapping)
        ):
            raise ProgramModelTransportError("model response metadata is invalid")
        if request.response_mode == "json_object":
            text = message.get("content")
            if not isinstance(text, str):
                raise ProgramModelTransportError(
                    "model JSON response has no textual content"
                )
            return ProgramModelResponse(
                text=text,
                model_version=model_version,
                usage=usage,
            )
        tool_calls = message.get("tool_calls")
        if not isinstance(tool_calls, list) or len(tool_calls) != 1:
            raise ProgramModelTransportError(
                "model response must contain exactly one tool call"
            )
        tool_call = tool_calls[0]
        function = (
            tool_call.get("function")
            if isinstance(tool_call, Mapping)
            else None
        )
        if not isinstance(function, Mapping):
            raise ProgramModelTransportError("model tool call is invalid")
        name = function.get("name")
        arguments = function.get("arguments")
        if not isinstance(name, str) or not name or not isinstance(arguments, str):
            raise ProgramModelTransportError("model tool call fields are invalid")
        return ProgramModelResponse(
            tool_name=name,
            tool_arguments=arguments,
            model_version=model_version,
            usage=usage,
        )

    def close(self) -> None:
        with self._lock:
            if self._closed:
                return
            self._closed = True
        if self._owns_client:
            self._client.close()


__all__ = [
    "HeadlessProgramModelAuditSink",
    "OpenAICompatibleProgramModelTransport",
    "ProgramModelAuditCommitError",
]
