"""Concrete Program model transport calls the request's AI model component."""

from __future__ import annotations

import base64
from dataclasses import replace
from hashlib import sha256
import json
from threading import Event

import httpx
import pytest

from manyforge_behavior.assistant_program_port import (
    ProgramModelRequest,
    ProgramModelTransportError,
)
from manyforge_headless_api.program_model_transport import (
    HeadlessProgramModelAuditSink,
    OpenAICompatibleProgramModelTransport,
    ProgramModelAuditCommitError,
)


_DIGEST = "sha256:" + "a" * 64
_JPEG = b"\xff\xd8\xff\xe0manyforge-model-snapshot"


@pytest.mark.parametrize("mode", ["validation", "serialization"])
def test_model_attempt_openapi_exposes_lossless_int64_wire_forms(mode):
    from manyforge_headless_api.models import ProgramModelAttemptRequest
    from manyforge_behavior.json_scalars import MAXIMUM_SAFE_INTEGER

    schema = ProgramModelAttemptRequest.model_json_schema(mode=mode)
    numeric, decimal = schema["properties"]["occurred_at_ns"]["anyOf"]
    assert numeric == {"type": "integer", "minimum": 0, "maximum": MAXIMUM_SAFE_INTEGER}
    assert decimal["type"] == "string"
    assert decimal["maxLength"] == 19
    assert decimal["format"] == "nonnegative-int64-decimal"
    # The existing exact decoder/serializer remains the range authority.
    timestamp = 31 * 366 * 86400 * 10**9
    assert ProgramModelAttemptRequest.exact_occurrence_time(str(timestamp)) == timestamp
    instance = ProgramModelAttemptRequest.model_construct(occurred_at_ns=timestamp)
    assert instance.model_dump(include={"occurred_at_ns"}) == {
        "occurred_at_ns": str(timestamp)
    }


_TOKEN_ENV = "MANYFORGE_AI_TEST_TOKEN"


def _transport(handler) -> OpenAICompatibleProgramModelTransport:
    return OpenAICompatibleProgramModelTransport(
        client=httpx.Client(transport=httpx.MockTransport(handler))
    )


def _request(
    *,
    tool: bool = False,
    base_url: str = "http://models.test/v1",
    api_key_env: str | None = _TOKEN_ENV,
) -> ProgramModelRequest:
    return ProgramModelRequest(
        model_endpoint_alias="planning_model",
        external_resource_alias="planning_model",
        model_id="model-a",
        base_url=base_url,
        api_key_env=api_key_env,
        prompt_template_id="select",
        prompt_template_version="1",
        prompt_template_digest=_DIGEST,
        prompt="Select one object",
        timeout_s=2.0,
        max_output_tokens=128,
        response_mode="tool_call" if tool else "json_object",
        tool_call_family="openai_chat_tools",
        response_tool=(
            {
                "name": "return_selection",
                "description": "Return selection",
                "parameters": {"type": "object"},
            }
            if tool
            else None
        ),
        attachment={
            "kind": "state_json",
            "state_json": '{"objects":[]}',
        },
    )


def _snapshot_attachment() -> dict[str, object]:
    return {
        "data_url": "data:image/jpeg;base64," + base64.b64encode(_JPEG).decode("ascii"),
        "kind": "snapshot_image",
        "media_type": "image/jpeg",
        "source": {
            "height": 480,
            "image_sha256": "sha256:" + sha256(_JPEG).hexdigest(),
            "schema_version": "manyforge.program_snapshot_source.v1",
            "viewer_generation": 19,
            "viewer_owner_epoch": "headless.epoch.1",
            "width": 640,
        },
        "transport": "inline_data_url",
    }


def test_json_request_uses_the_ai_model_component_and_bounded_contract(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv(_TOKEN_ENV, "secret")
    captured: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        captured.append(request)
        return httpx.Response(
            200,
            json={
                "model": "model-a.2026-08",
                "choices": [{"message": {"content": '{"selected_object_id":"part"}'}}],
                "usage": {"total_tokens": 17},
            },
        )

    transport = _transport(handler)
    transport.validate_endpoint(
        model_endpoint_alias="planning_model",
        external_resource_alias="planning_model",
        model_id="model-a",
        request_contract={"tool_call_family": "openai_chat_tools"},
    )

    response = transport.complete(_request(), Event())

    assert response.text == '{"selected_object_id":"part"}'
    assert response.model_version == "model-a.2026-08"
    assert response.usage == {"total_tokens": 17}
    request = captured[0]
    assert str(request.url) == "http://models.test/v1/chat/completions"
    assert request.headers["authorization"] == "Bearer secret"
    body = request.read().decode("utf-8")
    payload = httpx.Response(200, content=body).json()
    assert payload["messages"][0]["content"].endswith(
        'Attached state JSON:\n{"objects":[]}'
    )
    assert '"response_format":{"type":"json_object"}' in body
    assert "secret" not in body


def test_tool_call_response_is_transport_neutral(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv(_TOKEN_ENV, "secret")

    def handler(_request: httpx.Request) -> httpx.Response:
        return httpx.Response(
            200,
            json={
                "model": "model-a",
                "choices": [
                    {
                        "message": {
                            "tool_calls": [
                                {
                                    "function": {
                                        "name": "return_selection",
                                        "arguments": '{"selected_object_id":"part"}',
                                    }
                                }
                            ]
                        }
                    }
                ],
            },
        )

    transport = _transport(handler)

    response = transport.complete(_request(tool=True), Event())

    assert response.tool_name == "return_selection"
    assert response.tool_arguments == '{"selected_object_id":"part"}'


def test_snapshot_request_uses_standard_multimodal_content_parts(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv(_TOKEN_ENV, "secret")
    captured: list[dict[str, object]] = []

    def handler(request: httpx.Request) -> httpx.Response:
        captured.append(json.loads(request.read()))
        return httpx.Response(
            200,
            json={
                "model": "model-a",
                "choices": [{"message": {"content": '{"visible":true}'}}],
            },
        )

    transport = _transport(handler)
    request = replace(_request(), attachment=_snapshot_attachment())

    assert transport.complete(request, Event()).text == '{"visible":true}'
    assert captured[0]["messages"] == [
        {
            "role": "user",
            "content": [
                {"type": "text", "text": "Select one object"},
                {
                    "type": "image_url",
                    "image_url": {
                        "url": _snapshot_attachment()["data_url"],
                    },
                },
            ],
        }
    ]


def test_unsupported_family_unset_token_and_cancellation_fail_closed(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    called = False

    def handler(_request: httpx.Request) -> httpx.Response:
        nonlocal called
        called = True
        return httpx.Response(500)

    transport = _transport(handler)
    with pytest.raises(ProgramModelTransportError, match="openai_chat_tools only"):
        transport.validate_endpoint(
            model_endpoint_alias="planning_model",
            external_resource_alias="planning_model",
            model_id="model-a",
            request_contract={"tool_call_family": "qwen3_xml"},
        )
    monkeypatch.delenv(_TOKEN_ENV, raising=False)
    with pytest.raises(ProgramModelTransportError) as unset:
        transport.complete(_request(), Event())
    assert unset.value.reason_code == "ai_model_token_unavailable"
    cancelled = Event()
    cancelled.set()
    with pytest.raises(ProgramModelTransportError, match="cancelled"):
        transport.complete(_request(), cancelled)
    assert called is False


def test_ai_model_without_token_sends_no_authorization() -> None:
    captured: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        captured.append(request)
        return httpx.Response(
            200,
            json={"model": "model-a", "choices": [{"message": {"content": "{}"}}]},
        )

    request = _request(base_url="http://models.test/v1/", api_key_env=None)

    assert _transport(handler).complete(request, Event()).text == "{}"
    assert str(captured[0].url) == "http://models.test/v1/chat/completions"
    assert "authorization" not in captured[0].headers


def test_transport_failure_does_not_expose_the_physical_endpoint() -> None:
    def handler(request: httpx.Request) -> httpx.Response:
        raise httpx.ConnectError(
            "provider diagnostic contains site-only details",
            request=request,
        )

    transport = _transport(handler)
    request = _request(base_url="https://secret@models.test/v1", api_key_env=None)

    with pytest.raises(ProgramModelTransportError) as captured:
        transport.complete(request, Event())

    assert str(captured.value) == "model transport failed"


def test_model_audit_sink_requires_exact_durable_receipt() -> None:
    class Core:
        def __init__(self) -> None:
            self.records: list[dict[str, object]] = []

        def commit_program_model_attempt(
            self,
            record: dict[str, object],
        ) -> dict[str, object]:
            self.records.append(dict(record))
            return {
                "event_id": "event:model-attempt",
                "model_attempt_id": record["model_attempt_id"],
                "schema_version": "manyforge.program_model_attempt_commit.v1",
                "status": "stored",
            }

    core = Core()
    sink = HeadlessProgramModelAuditSink(core)  # type: ignore[arg-type]

    sink.commit({"model_attempt_id": "model-attempt:one"})

    assert core.records == [{"model_attempt_id": "model-attempt:one"}]


def test_model_audit_sink_rejects_mismatched_receipt() -> None:
    class Core:
        def commit_program_model_attempt(
            self,
            _record: dict[str, object],
        ) -> dict[str, object]:
            return {
                "event_id": "event:model-attempt",
                "model_attempt_id": "model-attempt:other",
                "schema_version": "manyforge.program_model_attempt_commit.v1",
                "status": "stored",
            }

    sink = HeadlessProgramModelAuditSink(Core())  # type: ignore[arg-type]

    with pytest.raises(ProgramModelAuditCommitError, match="invalid"):
        sink.commit({"model_attempt_id": "model-attempt:one"})
