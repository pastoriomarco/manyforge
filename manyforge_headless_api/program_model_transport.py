"""Concrete site transport for compiler-qualified Program model requests."""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass, field
import json
from threading import Event, RLock
from types import MappingProxyType
from typing import Any, Final
from urllib.parse import urlsplit

import httpx

from manyforge_behavior.assistant_program_port import (
    ProgramModelRequest,
    ProgramModelResponse,
    ProgramModelTransportError,
)
from manyforge_headless_api.native import HeadlessCorePort


_MAX_RESPONSE_BYTES: Final = 4 * 1024 * 1024
_MAX_REQUEST_BYTES: Final = 8 * 1024 * 1024
_STREAM_CHUNK_BYTES: Final = 64 * 1024
_RESERVED_REQUEST_HEADERS: Final = frozenset(
    {
        "connection",
        "content-length",
        "content-type",
        "host",
        "transfer-encoding",
    }
)


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


@dataclass(frozen=True, slots=True)
class ProgramModelEndpointConfig:
    """One launcher-supplied external resource; never authored in a Program."""

    external_resource_alias: str
    chat_completions_url: str
    model_endpoint_aliases: tuple[str, ...]
    model_ids: tuple[str, ...]
    tool_call_families: tuple[str, ...] = ("openai_chat_tools",)
    headers: Mapping[str, str] = field(default_factory=dict, repr=False)
    bearer_token: str | None = field(default=None, repr=False)

    def __post_init__(self) -> None:
        for field_name in ("external_resource_alias", "chat_completions_url"):
            value = getattr(self, field_name)
            if (
                not isinstance(value, str)
                or not value
                or len(value.encode("utf-8")) > 4_096
            ):
                raise ValueError(f"{field_name} must be bounded non-empty text")
        parsed = urlsplit(self.chat_completions_url)
        if (
            parsed.scheme not in {"http", "https"}
            or not parsed.netloc
            or parsed.username is not None
            or parsed.password is not None
        ):
            raise ValueError("chat_completions_url must be an absolute HTTP(S) URL")
        for field_name in (
            "model_endpoint_aliases",
            "model_ids",
            "tool_call_families",
        ):
            values = getattr(self, field_name)
            if (
                not isinstance(values, tuple)
                or not values
                or tuple(sorted(set(values))) != values
                or any(
                    not isinstance(value, str)
                    or not value
                    or len(value.encode("utf-8")) > 512
                    for value in values
                )
            ):
                raise ValueError(f"{field_name} must be sorted unique bounded text")
        if any(
            family != "openai_chat_tools" for family in self.tool_call_families
        ):
            raise ValueError(
                "the concrete baseline transport supports openai_chat_tools only"
            )
        if not isinstance(self.headers, Mapping) or any(
            not isinstance(key, str)
            or not key
            or not isinstance(value, str)
            or len(key.encode("utf-8")) > 256
            or len(value.encode("utf-8")) > 4_096
            for key, value in self.headers.items()
        ):
            raise ValueError("site model headers are invalid")
        normalized_header_names = [key.lower() for key in self.headers]
        if (
            len(normalized_header_names) != len(set(normalized_header_names))
            or _RESERVED_REQUEST_HEADERS.intersection(normalized_header_names)
        ):
            raise ValueError("site model headers contain ambiguous reserved fields")
        if self.bearer_token is not None and (
            not isinstance(self.bearer_token, str)
            or not self.bearer_token
            or len(self.bearer_token.encode("utf-8")) > 16_384
        ):
            raise ValueError("bearer_token must be absent or bounded text")
        if self.bearer_token is not None and "authorization" in (
            normalized_header_names
        ):
            raise ValueError(
                "bearer_token and an explicit Authorization header are ambiguous"
            )
        object.__setattr__(self, "headers", MappingProxyType(dict(self.headers)))


class OpenAICompatibleProgramModelTransport:
    """Bounded OpenAI-compatible transport over explicit site configuration."""

    def __init__(
        self,
        endpoints: tuple[ProgramModelEndpointConfig, ...],
        *,
        client: httpx.Client | None = None,
    ) -> None:
        if not endpoints or any(
            not isinstance(endpoint, ProgramModelEndpointConfig)
            for endpoint in endpoints
        ):
            raise ValueError("at least one valid site model endpoint is required")
        by_resource = {
            endpoint.external_resource_alias: endpoint for endpoint in endpoints
        }
        if len(by_resource) != len(endpoints):
            raise ValueError("site model external-resource aliases must be unique")
        aliases: set[str] = set()
        for endpoint in endpoints:
            overlap = aliases & set(endpoint.model_endpoint_aliases)
            if overlap:
                raise ValueError(
                    f"site model endpoint aliases are ambiguous: {sorted(overlap)}"
                )
            aliases.update(endpoint.model_endpoint_aliases)
        self._endpoints = MappingProxyType(by_resource)
        self._client = client or httpx.Client()
        self._owns_client = client is None
        self._closed = False
        self._lock = RLock()

    def _endpoint(
        self,
        *,
        model_endpoint_alias: str,
        external_resource_alias: str,
        model_id: str,
        tool_call_family: str,
    ) -> ProgramModelEndpointConfig:
        try:
            endpoint = self._endpoints[external_resource_alias]
        except KeyError as exc:
            raise ProgramModelTransportError(
                "retained external model resource is not configured at this site"
            ) from exc
        if model_endpoint_alias not in endpoint.model_endpoint_aliases:
            raise ProgramModelTransportError(
                "retained model endpoint alias is not allowed by site configuration"
            )
        if model_id not in endpoint.model_ids:
            raise ProgramModelTransportError(
                "retained model ID is not allowed by site configuration"
            )
        if tool_call_family not in endpoint.tool_call_families:
            raise ProgramModelTransportError(
                "retained model parser family is unavailable at this site"
            )
        return endpoint

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
        family = request_contract.get("tool_call_family")
        if not isinstance(family, str):
            raise ProgramModelTransportError("model parser family is absent")
        self._endpoint(
            model_endpoint_alias=model_endpoint_alias,
            external_resource_alias=external_resource_alias,
            model_id=model_id,
            tool_call_family=family,
        )

    @staticmethod
    def _request_body(request: ProgramModelRequest) -> dict[str, Any]:
        content = request.prompt
        if request.attachment is not None:
            if request.attachment.get("kind") != "state_json":
                raise ProgramModelTransportError(
                    "site transport received an unsupported attachment"
                )
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
        endpoint = self._endpoint(
            model_endpoint_alias=request.model_endpoint_alias,
            external_resource_alias=request.external_resource_alias,
            model_id=request.model_id,
            tool_call_family=request.tool_call_family,
        )
        with self._lock:
            if self._closed:
                raise ProgramModelTransportError("site model transport is closed")
        headers = dict(endpoint.headers)
        if endpoint.bearer_token is not None:
            headers["Authorization"] = f"Bearer {endpoint.bearer_token}"
        headers["Content-Type"] = "application/json"
        request_bytes = self._request_bytes(request)
        try:
            with self._client.stream(
                "POST",
                endpoint.chat_completions_url,
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
    "ProgramModelEndpointConfig",
]
