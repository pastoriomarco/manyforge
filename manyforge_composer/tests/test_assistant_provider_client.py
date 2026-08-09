from __future__ import annotations

from copy import deepcopy

import httpx
import pytest

from manyforge_composer.backend.assistant_provider_client import (
    AssistantProviderClientError,
    AssistantProviderHttpClient,
)


def _status() -> dict:
    return {
        "schemaVersion": "manyforge.assistant.provider_status.v1",
        "requestId": "request.1",
        "requestOwnerEpoch": "page.1",
        "sourceLabel": "composer_ui",
        "correlationId": None,
        "conversationId": "conversation.1",
        "assistantMode": "composer-assistant",
        "state": "completed",
        "response": {
            "schemaVersion": "manyforge.assistant.provider_response.v1",
            "requestId": "request.1",
            "requestOwnerEpoch": "page.1",
            "sourceLabel": "composer_ui",
            "correlationId": None,
            "conversationId": "conversation.1",
            "assistantMode": "composer-assistant",
            "catalogHash": "sha256:" + "1" * 64,
            "message": "Draft checked.",
            "proposals": [],
            "toolCalls": [],
            "warnings": [],
            "requiresReview": True,
            "mutated": False,
            "draftMutated": False,
            "needsClarification": False,
            "exitReason": "ok",
            "turnCount": 1,
        },
        "error": None,
    }


def _client(handler) -> tuple[AssistantProviderHttpClient, httpx.Client]:
    transport = httpx.MockTransport(handler)
    raw = httpx.Client(transport=transport)
    return AssistantProviderHttpClient("http://bridge.test", client=raw), raw


def test_provider_client_accepts_only_a_closed_attributed_terminal_status() -> None:
    def handler(request: httpx.Request) -> httpx.Response:
        assert request.url.path == "/v1/manyforge/assistant/request.1"
        return httpx.Response(200, json=_status(), request=request)

    client, raw = _client(handler)
    try:
        assert client.status("request.1")["response"]["message"] == "Draft checked."
    finally:
        raw.close()


@pytest.mark.parametrize(
    "mutation",
    ["outer_id", "inner_id", "runtime_mutation", "open_proposal"],
)
def test_provider_client_rejects_changed_identity_or_runtime_mutation(
    mutation: str,
) -> None:
    payload = deepcopy(_status())
    if mutation == "outer_id":
        payload["requestId"] = "request.other"
    elif mutation == "inner_id":
        payload["response"]["requestId"] = "request.other"
    elif mutation == "open_proposal":
        payload["response"]["proposals"] = [{"toolId": "runtime_stop"}]
    else:
        payload["response"]["mutated"] = True

    def handler(request: httpx.Request) -> httpx.Response:
        return httpx.Response(200, json=payload, request=request)

    client, raw = _client(handler)
    try:
        with pytest.raises(AssistantProviderClientError) as raised:
            client.status("request.1")
        assert raised.value.code == "assistant_provider_response_invalid"
    finally:
        raw.close()


def test_provider_client_rejects_invalid_request_id_before_transport() -> None:
    calls = 0

    def handler(request: httpx.Request) -> httpx.Response:
        nonlocal calls
        calls += 1
        return httpx.Response(500, request=request)

    client, raw = _client(handler)
    try:
        with pytest.raises(AssistantProviderClientError) as invalid:
            client.status(" request.1 ")
        assert invalid.value.code == "assistant_request_invalid"
        assert invalid.value.status_code == 400
        assert calls == 0
    finally:
        raw.close()


def test_provider_client_rejects_nonfinite_json_and_preserves_typed_http_error() -> None:
    calls = 0

    def handler(request: httpx.Request) -> httpx.Response:
        nonlocal calls
        calls += 1
        if calls == 1:
            return httpx.Response(
                200,
                content=b'{"value":NaN}',
                request=request,
            )
        return httpx.Response(
            409,
            json={
                "detail": {
                    "code": "assistant_request_id_conflict",
                    "message": "requestId content changed",
                }
            },
            request=request,
        )

    client, raw = _client(handler)
    try:
        with pytest.raises(AssistantProviderClientError) as invalid:
            client.status("request.1")
        assert invalid.value.code == "assistant_provider_response_invalid"
        with pytest.raises(AssistantProviderClientError) as conflict:
            client.status("request.1")
        assert conflict.value.code == "assistant_request_id_conflict"
        assert conflict.value.status_code == 409
    finally:
        raw.close()
