"""Concrete Program model transport stays bounded and site-qualified."""

from __future__ import annotations

import json
from pathlib import Path
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
    ProgramModelEndpointConfig,
    load_program_model_endpoint_configs,
)


_DIGEST = "sha256:" + "a" * 64


def _config() -> ProgramModelEndpointConfig:
    return ProgramModelEndpointConfig(
        external_resource_alias="site_llm",
        chat_completions_url="http://models.test/v1/chat/completions",
        model_endpoint_aliases=("planning_model",),
        model_ids=("model-a",),
        headers={"X-Site": "cell-a"},
        bearer_token="secret",
    )


def _request(*, tool: bool = False) -> ProgramModelRequest:
    return ProgramModelRequest(
        model_endpoint_alias="planning_model",
        external_resource_alias="site_llm",
        model_id="model-a",
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


def test_json_request_uses_explicit_site_endpoint_and_bounded_contract() -> None:
    captured: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        captured.append(request)
        return httpx.Response(
            200,
            json={
                "model": "model-a.2026-08",
                "choices": [
                    {"message": {"content": '{"selected_object_id":"part"}'}}
                ],
                "usage": {"total_tokens": 17},
            },
        )

    client = httpx.Client(transport=httpx.MockTransport(handler))
    transport = OpenAICompatibleProgramModelTransport((_config(),), client=client)
    transport.validate_endpoint(
        model_endpoint_alias="planning_model",
        external_resource_alias="site_llm",
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


def test_tool_call_response_is_transport_neutral() -> None:
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

    transport = OpenAICompatibleProgramModelTransport(
        (_config(),),
        client=httpx.Client(transport=httpx.MockTransport(handler)),
    )

    response = transport.complete(_request(tool=True), Event())

    assert response.tool_name == "return_selection"
    assert response.tool_arguments == '{"selected_object_id":"part"}'


def test_site_allowlist_and_pre_dispatch_cancellation_fail_closed() -> None:
    called = False

    def handler(_request: httpx.Request) -> httpx.Response:
        nonlocal called
        called = True
        return httpx.Response(500)

    transport = OpenAICompatibleProgramModelTransport(
        (_config(),),
        client=httpx.Client(transport=httpx.MockTransport(handler)),
    )
    with pytest.raises(ProgramModelTransportError, match="not allowed"):
        transport.validate_endpoint(
            model_endpoint_alias="planning_model",
            external_resource_alias="site_llm",
            model_id="other-model",
            request_contract={"tool_call_family": "openai_chat_tools"},
        )
    cancelled = Event()
    cancelled.set()
    with pytest.raises(ProgramModelTransportError, match="cancelled"):
        transport.complete(_request(), cancelled)
    assert called is False


def test_site_endpoint_rejects_credential_urls_and_ambiguous_headers() -> None:
    with pytest.raises(ValueError, match="absolute HTTP"):
        ProgramModelEndpointConfig(
            external_resource_alias="site_llm",
            chat_completions_url="https://secret@models.test/v1/chat/completions",
            model_endpoint_aliases=("planning_model",),
            model_ids=("model-a",),
        )
    with pytest.raises(ValueError, match="reserved"):
        ProgramModelEndpointConfig(
            external_resource_alias="site_llm",
            chat_completions_url="https://models.test/v1/chat/completions",
            model_endpoint_aliases=("planning_model",),
            model_ids=("model-a",),
            headers={"content-type": "text/plain"},
        )
    with pytest.raises(ValueError, match="Authorization"):
        ProgramModelEndpointConfig(
            external_resource_alias="site_llm",
            chat_completions_url="https://models.test/v1/chat/completions",
            model_endpoint_aliases=("planning_model",),
            model_ids=("model-a",),
            headers={"Authorization": "other"},
            bearer_token="secret",
        )


def test_launcher_site_config_loads_strict_allowlist_and_indirect_secret(
    tmp_path: Path,
) -> None:
    path = tmp_path / "model-site.json"
    path.write_text(
        json.dumps(
            {
                "api_version": "manyforge.program_model_site_config.v1",
                "endpoints": [
                    {
                        "bearer_token_env": "MANYFORGE_TEST_MODEL_TOKEN",
                        "chat_completions_url": (
                            "http://127.0.0.1:8000/v1/chat/completions"
                        ),
                        "external_resource_alias": "selection_model",
                        "headers": {"X-Site": "cell-a"},
                        "model_endpoint_aliases": ["selection_model"],
                        "model_ids": ["model-a", "model-b"],
                        "tool_call_families": ["openai_chat_tools"],
                    }
                ],
            },
            sort_keys=True,
        ),
        encoding="utf-8",
    )

    loaded = load_program_model_endpoint_configs(
        path,
        environ={"MANYFORGE_TEST_MODEL_TOKEN": "site-secret"},
    )

    assert len(loaded) == 1
    assert loaded[0].external_resource_alias == "selection_model"
    assert loaded[0].model_ids == ("model-a", "model-b")
    assert loaded[0].bearer_token == "site-secret"
    assert "site-secret" not in path.read_text(encoding="utf-8")


def test_launcher_site_config_rejects_unknown_fields_symlinks_and_missing_secret(
    tmp_path: Path,
) -> None:
    value = {
        "api_version": "manyforge.program_model_site_config.v1",
        "endpoints": [
            {
                "bearer_token_env": "MISSING_MODEL_TOKEN",
                "chat_completions_url": "http://models.test/v1/chat/completions",
                "external_resource_alias": "site_llm",
                "headers": {},
                "model_endpoint_aliases": ["planning_model"],
                "model_ids": ["model-a"],
                "tool_call_families": ["openai_chat_tools"],
            }
        ],
    }
    path = tmp_path / "model-site.json"
    path.write_text(json.dumps(value), encoding="utf-8")
    with pytest.raises(ValueError, match="unset environment variable"):
        load_program_model_endpoint_configs(path, environ={})

    value["unexpected"] = True
    path.write_text(json.dumps(value), encoding="utf-8")
    with pytest.raises(ValueError, match="unknown or missing"):
        load_program_model_endpoint_configs(path, environ={})

    link = tmp_path / "model-site-link.json"
    link.symlink_to(path)
    with pytest.raises(ValueError, match="readable plain file"):
        load_program_model_endpoint_configs(link, environ={})


def test_transport_failure_does_not_expose_the_physical_endpoint() -> None:
    def handler(request: httpx.Request) -> httpx.Response:
        raise httpx.ConnectError(
            "provider diagnostic contains site-only details",
            request=request,
        )

    transport = OpenAICompatibleProgramModelTransport(
        (
            ProgramModelEndpointConfig(
                external_resource_alias="site_llm",
                chat_completions_url=(
                    "https://models.test/v1/chat/completions?site_key=secret"
                ),
                model_endpoint_aliases=("planning_model",),
                model_ids=("model-a",),
            ),
        ),
        client=httpx.Client(transport=httpx.MockTransport(handler)),
    )

    with pytest.raises(ProgramModelTransportError) as captured:
        transport.complete(_request(), Event())

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
