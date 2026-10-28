"""Mocked-HTTP coverage for the assistant smoke command flows."""

from __future__ import annotations

import importlib.util
import io
import json
from pathlib import Path
import sys
from types import ModuleType
from urllib.parse import urlsplit

import pytest


SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"


def _load_script(name: str, filename: str) -> ModuleType:
    spec = importlib.util.spec_from_file_location(
        name,
        SCRIPTS / filename,
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


PROVIDER = _load_script(
    "manyforge_assistant_provider_smoke_workflow_test",
    "smoke-assistant-provider-bridge.py",
)
SCENE = _load_script(
    "manyforge_assistant_scene_smoke_workflow_test",
    "smoke-assistant-scene-authoring.py",
)


class _JsonResponse:
    def __init__(self, payload: dict) -> None:
        self._body = io.BytesIO(json.dumps(payload).encode("utf-8"))

    def __enter__(self) -> _JsonResponse:
        return self

    def __exit__(self, *_args) -> None:
        return None

    def read(self, limit: int = -1) -> bytes:
        return self._body.read(limit)


class _ProviderHttp:
    def __init__(self, *, change_draft_after_request: bool = False) -> None:
        self.change_draft_after_request = change_draft_after_request
        self.calls: list[tuple[str, str]] = []
        self.draft_reads = 0

    def urlopen(self, request, *, timeout: float):
        method = request.get_method()
        url = request.full_url
        path = urlsplit(url).path
        self.calls.append((method, path))

        if method == "GET" and path == "/api/health":
            return _JsonResponse({"components": {"assistant_bridge": "configured"}})
        if method == "GET" and path == "/api/assistant/modes":
            return _JsonResponse(
                {
                    "schema_version": "manyforge.assistant_mode_index.v1",
                    "modes": [{"mode": PROVIDER.ASSISTANT_MODE}],
                    "ai_model_component_id": "mock_chat_model",
                }
            )
        if method == "GET" and path == "/api/assistant/modes/composer-assistant":
            return _JsonResponse(
                {
                    "schema_version": "manyforge.assistant_mode_projection.v1",
                    "mode": PROVIDER.ASSISTANT_MODE,
                    "model_endpoint": {
                        "model_endpoint_alias": "assistant_model",
                        "ai_model_component_id": "mock_chat_model",
                        "model_id": "local-model",
                        "base_url": "http://model.test/v1",
                        "api_key_env": None,
                    },
                }
            )
        if method == "GET" and path == "/v1/models":
            return _JsonResponse({"data": [{"id": "local-model"}]})
        if method == "GET" and path == "/api/program/draft":
            self.draft_reads += 1
            revision = (
                1
                if self.change_draft_after_request and self.draft_reads == 2
                else 0
            )
            return _JsonResponse(
                {
                    "program_loaded": True,
                    "authoring": {"draftRevision": revision, "scene": []},
                }
            )
        if method == "POST" and path == "/api/assistant/requests":
            submitted = json.loads(request.data.decode("utf-8"))
            identity = {
                field: submitted[field]
                for field in (
                    "requestId",
                    "requestOwnerEpoch",
                    "sourceLabel",
                    "correlationId",
                    "conversationId",
                    "assistantMode",
                )
            }
            response = {
                "schemaVersion": "manyforge.assistant.provider_response.v1",
                **identity,
                "requiresReview": True,
                "mutated": False,
                "draftMutated": False,
                "toolCalls": [
                    {
                        "toolId": "program_read",
                        "status": "completed",
                        "effect": "read_only",
                        "draftMutated": False,
                    }
                ],
            }
            return _JsonResponse({**identity, "state": "completed", "response": response})

        raise AssertionError(f"unexpected mocked HTTP request: {method} {url}")


def test_provider_smoke_compares_matching_draft_snapshots(monkeypatch, capsys) -> None:
    http = _ProviderHttp()
    monkeypatch.setattr(PROVIDER.urllib.request, "urlopen", http.urlopen)

    assert PROVIDER.main([]) == 0

    assert http.draft_reads == 2
    assert http.calls.count(("POST", "/api/assistant/requests")) == 1
    assert "before/after Composer Draft snapshots match" in capsys.readouterr().out


def test_provider_smoke_flags_changed_snapshot_as_possible_concurrent_edit(
    monkeypatch, capsys
) -> None:
    http = _ProviderHttp(change_draft_after_request=True)
    monkeypatch.setattr(PROVIDER.urllib.request, "urlopen", http.urlopen)

    assert PROVIDER.main([]) == 1

    captured = capsys.readouterr()
    assert "snapshot changed during the read-only request" in captured.err
    assert "concurrent Draft edits" in captured.err
    assert http.draft_reads == 2
    assert http.calls.count(("POST", "/api/assistant/requests")) == 1


def test_uncertain_submission_is_not_resent(monkeypatch) -> None:
    posts: list[str] = []
    status_reads: list[str] = []

    def urlopen(request, *, timeout: float):
        method = request.get_method()
        url = request.full_url
        if method == "POST":
            posts.append(url)
            raise PROVIDER.urllib.error.URLError("response lost")
        status_reads.append(url)
        raise PROVIDER.urllib.error.HTTPError(
            url,
            404,
            "Not found",
            {},
            io.BytesIO(b""),
        )

    monkeypatch.setattr(PROVIDER.urllib.request, "urlopen", urlopen)
    monkeypatch.setattr(PROVIDER.time, "sleep", lambda _seconds: None)
    request = PROVIDER.make_request(
        message="Read the Draft.",
        requested_tools=[PROVIDER.READ_TOOL],
    )

    with pytest.raises(PROVIDER.SmokeFailure, match="submission outcome is unknown"):
        PROVIDER.submit_and_wait(
            "http://127.0.0.1:3000",
            request,
            timeout_s=2.0,
            http_timeout_s=0.1,
        )

    assert len(posts) == 1
    assert len(status_reads) == PROVIDER.NOT_FOUND_READ_LIMIT


def test_scene_smoke_refuses_without_explicit_draft_confirmation(
    monkeypatch, capsys
) -> None:
    requests: list[str] = []

    def unexpected_request(request, *, timeout: float):
        requests.append(request.full_url)
        raise AssertionError("unconfirmed scene smoke must not send HTTP requests")

    monkeypatch.setattr(SCENE._provider.urllib.request, "urlopen", unexpected_request)

    assert SCENE.main([]) == 2

    assert requests == []
    assert "--confirm-draft-only" in capsys.readouterr().err
