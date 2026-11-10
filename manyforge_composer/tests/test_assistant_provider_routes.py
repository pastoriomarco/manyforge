from __future__ import annotations

from copy import deepcopy
from types import SimpleNamespace
from typing import Any

import pytest
from fastapi.testclient import TestClient

from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.assistant_provider_client import (
    AssistantProviderClientError,
)
from manyforge_composer.backend.state import ComposerState


_URL = "http://127.0.0.1:43017"


class RecordingProvider:
    def __init__(self) -> None:
        self.submissions: list[dict[str, Any]] = []
        self.status_reads: list[str] = []
        self.cancellations: list[str] = []
        self.urls: list[str] = []
        # Per request id: the state status reads report, or the error raised.
        self.states: dict[str, str | AssistantProviderClientError] = {}

    @staticmethod
    def _status(request_id: str, state: str = "running") -> dict[str, Any]:
        return {
            "schemaVersion": "manyforge.assistant.provider_status.v1",
            "requestId": request_id,
            "requestOwnerEpoch": "page.1",
            "sourceLabel": "composer_ui",
            "correlationId": None,
            "conversationId": "conversation.1",
            "assistantMode": "composer-assistant",
            "state": state,
            "response": None,
            "error": None,
        }

    def submit(self, url: str, body) -> dict[str, Any]:
        self.urls.append(url)
        self.submissions.append(deepcopy(dict(body)))
        return self._status(body["requestId"])

    def status(self, url: str, request_id: str) -> dict[str, Any]:
        self.urls.append(url)
        self.status_reads.append(request_id)
        state = self.states.get(request_id, "running")
        if isinstance(state, AssistantProviderClientError):
            raise state
        return self._status(request_id, state)

    def cancel(self, url: str, request_id: str) -> dict[str, Any]:
        self.urls.append(url)
        self.cancellations.append(request_id)
        return self._status(request_id, "cancelled")


class AdmissionHeadless:
    """Headless as the assistant routes read it: the mode index, the
    component states and the Program runs."""

    def __init__(self) -> None:
        self.assistant_component_id: str | None = "assistant"
        self.runs: list[dict[str, Any]] = []
        self.assistant: dict[str, Any] = {
            "component_id": "assistant",
            "lifecycle": "enabled",
            "endpoints": [{"role_id": "assistant_request", "url": _URL}],
        }
        self.model: dict[str, Any] = {
            "component_id": "assistant_model", "lifecycle": "enabled",
        }

    def authoring_admission(self) -> dict[str, Any]:
        return {"authoring_allowed": True, "admission_generation": 7}

    def recover_authoring_transaction(self, _body: dict[str, Any]) -> dict[str, Any]:
        return {
            "recovery_receipt": {
                "status": "unknown",
                "kernel_owner_epoch": "kernel.1",
                "held_claims": [],
                "current_owner_epochs": [],
            }
        }

    def reconcile_authoring_owner(self, _body: dict[str, Any]) -> dict[str, Any]:
        return {"admission": {"authoring_allowed": True, "admission_generation": 7}}

    def assistant_modes(self) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.assistant_mode_index.v2",
            "assistant_component_id": self.assistant_component_id,
            "ai_model_component_id": "assistant_model",
            "modes": [],
        }

    def program_runs(self, *, active_only: bool = False, limit: int = 128) -> dict[str, Any]:
        assert active_only is True
        return {"runs": deepcopy(self.runs)}

    def components(self) -> list[Any]:
        return [deepcopy(self.assistant), deepcopy(self.model)]


def _mode() -> dict[str, Any]:
    return {
        "mode": "composer-assistant",
        "grant_policy": {
            "approval_mode": None,
            "review_policy": "required",
            "session_kind": None,
        },
        "nodes": [],
        "skills": [],
        "tools": [
            {
                "tool_id": "program_read",
                "description": "Read Draft",
                "effect": "read_only",
                "input_schema": {
                    "type": "object",
                    "additionalProperties": False,
                    "properties": {},
                },
                "input_schema_hash": "sha256:" + "1" * 64,
            }
        ],
        "assistant_projection_digest": "sha256:" + "2" * 64,
        "retained_bundle_id": "bundle.1",
        "graph_revision": "graph.1",
        "source": {"source_content_digest": "sha256:" + "3" * 64},
    }


def _submit(state: ComposerState, monkeypatch) -> TestClient:
    from manyforge_composer.backend import routes_assistant

    node = SimpleNamespace(payload={"environment_digest": "sha256:" + "4" * 64,
                                    "source": {"source_content_digest": "sha256:" + "5" * 64}})
    source = SimpleNamespace(payload={"source": {"source_content_digest": "sha256:" + "6" * 64}})
    context = SimpleNamespace(assistant_mode=SimpleNamespace(payload=_mode()),
                              node_environment=node, source_model=source)
    monkeypatch.setattr(routes_assistant, "read_active_assistant_context", lambda _state, *, mode: context)
    with TestClient(create_app(state)) as client:
        return client.post("/api/assistant/requests", json={
            "requestId": "request.1", "requestOwnerEpoch": "page.1", "sourceLabel": "composer_ui",
            "correlationId": None, "conversationId": "conversation.1",
            "assistantMode": "composer-assistant", "message": "Read the Draft",
            "requestedTools": [], "fieldPolicy": {}})


def _loaded_state(monkeypatch, provider: RecordingProvider) -> ComposerState:
    state = ComposerState(headless_client=AdmissionHeadless(),  # type: ignore[arg-type]
                          assistant_client=provider)  # type: ignore[arg-type]
    draft = state.draft_session.snapshot()
    draft["program_loaded"] = True
    draft["authoring"] = {"schema_version": "manyforge.composer_authoring_forest.v1", "forest": []}
    draft["diagnostics"] = [{"code": "authoring_empty", "path": "/forest", "message": "empty"}]
    monkeypatch.setattr(state.draft_session, "snapshot", lambda: deepcopy(draft))
    return state


def test_request_carries_one_reference_validation_facts_and_diagnostics(monkeypatch) -> None:
    from manyforge_composer.backend import routes_draft

    provider = RecordingProvider()
    state = _loaded_state(monkeypatch, provider)
    facts = {key: [] for key in ("components", "targets", "signals", "frames", "bodies", "rules")}
    row = {"code": "component_reference_missing",
           "location": {"document": "program", "path": "/forest/0", "node_instance_id": "a", "parameter": "target"},
           "payload": {"component_id": "ghost"}}
    calls = []

    def validate(actual_state, authoring, *, expected_identity):
        calls.append((authoring, expected_identity))
        return {"reference_validation": {"selector_facts": facts, "diagnostics": [row]}}

    monkeypatch.setattr(routes_draft, "validate_captured_program_source", validate)
    assert _submit(state, monkeypatch).status_code == 202
    draft = provider.submissions[0]["composerDraft"]
    assert draft["selectorFacts"] == facts
    assert draft["referenceDiagnostics"] == [row]
    assert calls == [(draft["authoring"], state.authoring_workspace.identity())]


def test_request_reports_unavailable_reference_validation_as_one_typed_row(monkeypatch) -> None:
    provider = RecordingProvider()
    state = _loaded_state(monkeypatch, provider)  # no Deployment Draft source root
    assert _submit(state, monkeypatch).status_code == 202
    draft = provider.submissions[0]["composerDraft"]
    assert draft["selectorFacts"] is None
    assert [row["code"] for row in draft["referenceDiagnostics"]] == ["deployment_draft_unavailable"]


def test_composer_forwards_exact_owner_context_without_owning_request_state(
    monkeypatch, tmp_path,
) -> None:
    from manyforge_composer.backend import routes_assistant

    provider = RecordingProvider()
    state = ComposerState(
        headless_client=AdmissionHeadless(),  # type: ignore[arg-type]
        assistant_client=provider,  # type: ignore[arg-type]
    )
    node = SimpleNamespace(
        payload={
            "environment_digest": "sha256:" + "4" * 64,
            "source": {"source_content_digest": "sha256:" + "5" * 64},
        }
    )
    source = SimpleNamespace(
        payload={"source": {"source_content_digest": "sha256:" + "6" * 64}},
    )
    context = SimpleNamespace(
        assistant_mode=SimpleNamespace(payload=_mode()),
        node_environment=node,
        source_model=source,
    )
    mode = context.assistant_mode
    monkeypatch.setattr(
        routes_assistant,
        "read_active_assistant_context",
        lambda _state, *, mode: context,
    )

    with TestClient(create_app(state)) as client:
        accepted = client.post(
            "/api/assistant/requests",
            json={
                "requestId": "request.1",
                "requestOwnerEpoch": "page.1",
                "sourceLabel": "composer_ui",
                "correlationId": None,
                "conversationId": "conversation.1",
                "assistantMode": "composer-assistant",
                "message": "Read the Draft",
                "requestedTools": [],
                "fieldPolicy": {},
            },
        )
        status = client.get("/api/assistant/requests/request.1")
        cancelled = client.post("/api/assistant/requests/request.1/cancel")

    assert accepted.status_code == 202
    assert status.status_code == 200
    assert cancelled.json()["state"] == "cancelled"
    assert provider.status_reads == ["request.1"]
    assert provider.cancellations == ["request.1"]
    forwarded = provider.submissions[0]
    assert forwarded["schemaVersion"] == "manyforge.assistant.provider_request.v1"
    assert forwarded["modeProjection"] == mode.payload
    assert forwarded["composerDraft"]["draftId"] == state.draft_session.draft_id
    assert forwarded["composerDraft"]["programLoaded"] is False
    assert forwarded["composerDraft"]["authoring"] is None
    assert forwarded["composerDraft"]["candidateAvailable"] is False
    assert forwarded["composerDraft"]["diagnostics"] == []
    assert forwarded["composerDraft"]["selectorFacts"] is None
    assert forwarded["composerDraft"]["referenceDiagnostics"] == []
    assert forwarded["composerDraft"]["expectedAdmissionGeneration"] == 7
    assert set(forwarded["resolvedCatalog"]) == {
        "catalogHash",
        "grantPolicy",
        "nodes",
        "skills",
        "tools",
    }


def _headless_state(headless: AdmissionHeadless, provider: RecordingProvider) -> ComposerState:
    return ComposerState(headless_client=headless,  # type: ignore[arg-type]
                         assistant_client=provider)  # type: ignore[arg-type]


def test_bridge_url_is_resolved_from_the_assistant_component_per_call(monkeypatch) -> None:
    headless = AdmissionHeadless()
    provider = RecordingProvider()
    state = _headless_state(headless, provider)
    assert _submit(state, monkeypatch).status_code == 202
    # A restarted assistant publishes a new port: the next call follows it.
    headless.assistant["endpoints"] = [
        {"role_id": "assistant_request", "url": "http://127.0.0.1:50001"}
    ]
    with TestClient(create_app(state)) as client:
        assert client.get("/api/assistant/requests/request.1").status_code == 200
    assert provider.urls == [_URL, "http://127.0.0.1:50001"]


def _absent(headless: AdmissionHeadless) -> None:
    headless.assistant_component_id = None


def _prepared(headless: AdmissionHeadless) -> None:
    headless.assistant = {"component_id": "assistant", "lifecycle": "prepared"}


def _no_endpoint(headless: AdmissionHeadless) -> None:
    headless.assistant.pop("endpoints")


def _model_disabled(headless: AdmissionHeadless) -> None:
    headless.model["lifecycle"] = "prepared"


def _run(state: str):
    def mutate(headless: AdmissionHeadless) -> None:
        headless.runs = [{"program_run_id": "run.1", "state": state}]
    return mutate


@pytest.mark.parametrize(
    ("mutate", "code"),
    [
        (_absent, "assistant_component_absent"),
        (_prepared, "assistant_component_not_enabled"),
        (_no_endpoint, "assistant_component_not_enabled"),
        (_model_disabled, "assistant_ai_model_not_enabled"),
        (_run("running"), "assistant_program_active"),
        (_run("paused"), "assistant_program_active"),
        (_run("held"), "assistant_program_active"),
    ],
    ids=["absent", "prepared", "no-endpoint", "model-not-enabled", "run", "paused-run",
         "held-run"],
)
def test_submit_is_refused_typed_before_the_bridge(monkeypatch, mutate, code: str) -> None:
    headless = AdmissionHeadless()
    provider = RecordingProvider()
    mutate(headless)
    response = _submit(_headless_state(headless, provider), monkeypatch)
    assert response.status_code == 409
    assert response.json()["detail"]["code"] == code
    assert provider.submissions == []


@pytest.mark.parametrize(
    ("mutate", "code"),
    [
        (_absent, "assistant_component_absent"),
        (_prepared, "assistant_component_not_enabled"),
        (_model_disabled, None),
        (_run("held"), None),
    ],
    ids=["absent", "prepared", "model-not-enabled", "held-run"],
)
def test_status_and_cancel_need_only_the_enabled_assistant(mutate, code) -> None:
    headless = AdmissionHeadless()
    provider = RecordingProvider()
    mutate(headless)
    with TestClient(create_app(_headless_state(headless, provider))) as client:
        responses = [
            client.get("/api/assistant/requests/request.1"),
            client.post("/api/assistant/requests/request.1/cancel"),
        ]
    for response in responses:
        if code is None:
            assert response.status_code == 200
        else:
            assert response.status_code == 409
            assert response.json()["detail"]["code"] == code


def test_status_after_an_assistant_restart_is_the_typed_not_found() -> None:
    headless = AdmissionHeadless()
    provider = RecordingProvider()
    provider.states["request.1"] = AssistantProviderClientError(
        "assistant_request_not_found", "unknown requestId", 404
    )
    with TestClient(create_app(_headless_state(headless, provider))) as client:
        response = client.get("/api/assistant/requests/request.1")
    assert response.status_code == 404
    assert response.json()["detail"]["code"] == "assistant_request_not_found"


def test_in_flight_turns_are_kept_until_a_terminal_answer(monkeypatch) -> None:
    headless = AdmissionHeadless()
    provider = RecordingProvider()
    state = _headless_state(headless, provider)
    assert _submit(state, monkeypatch).status_code == 202
    assert state.assistant_turns == {"request.1"}
    with TestClient(create_app(state)) as client:
        client.get("/api/assistant/requests/request.1")
        assert state.assistant_turns == {"request.1"}
        provider.states["request.1"] = "completed"
        client.get("/api/assistant/requests/request.1")
        assert state.assistant_turns == set()
        state.assistant_turns.add("request.2")
        client.post("/api/assistant/requests/request.2/cancel")
    assert state.assistant_turns == set()


def test_submit_checks_runs_and_submits_under_the_workspace_fence(monkeypatch) -> None:
    headless = AdmissionHeadless()
    provider = RecordingProvider()
    state = _headless_state(headless, provider)
    fenced = []
    original = state.authoring_admission.submit_workspace_fenced

    def submit_workspace_fenced(callback):
        fenced.append(True)
        try:
            return original(callback)
        finally:
            fenced.pop()

    monkeypatch.setattr(state.authoring_admission, "submit_workspace_fenced",
                        submit_workspace_fenced)
    reads = []
    program_runs = headless.program_runs
    submit = provider.submit
    monkeypatch.setattr(headless, "program_runs",
                        lambda **kwargs: reads.append(bool(fenced)) or program_runs(**kwargs))
    monkeypatch.setattr(provider, "submit",
                        lambda url, body: reads.append(bool(fenced)) or submit(url, body))
    assert _submit(state, monkeypatch).status_code == 202
    assert reads == [True, True]
    assert state.assistant_turns == {"request.1"}


# --- Execute admission while a chat turn runs (§2a) ---

_DIGEST = "sha256:" + "1" * 64
_FRESHNESS = {
    "context_id": "freshness-1",
    "receiver_incarnation": "headless-1",
    "semantic_owner_epoch": "program-1",
    "execution_control_revision": 1,
    "issued_server_sequence": "1",
    "valid_for_ns": 750_000_000,
}


class ExecuteHeadless(AdmissionHeadless):
    def __init__(self) -> None:
        super().__init__()
        self.started: list[str] = []

    def program_run_start(self, body):
        self.started.append("program_run_start")
        return {"accepted": True}

    def submit_control_command(self, body):
        self.started.append(body["command_envelope"]["command_kind"])
        return {"accepted": True}

    def cycle_recovery_action(self, body):
        self.started.append("cycle_recovery_action")
        return {"accepted": True}


def _execute_state(headless: ExecuteHeadless, provider: RecordingProvider):
    from fastapi import FastAPI

    from manyforge_composer.backend.routes_headless import create_headless_router

    state = _headless_state(headless, provider)
    state.draft_session.snapshot = lambda: {"program_revision": _DIGEST}  # type: ignore[method-assign]
    state.published_scenario = lambda: {  # type: ignore[method-assign]
        "ready": True, "published_scenario": {"program_revision": _DIGEST},
    }
    app = FastAPI()
    app.include_router(create_headless_router(headless, state))  # type: ignore[arg-type]
    return state, TestClient(app)


_RECOVERY = {
    "request_id": "recovery-1",
    "request_owner_epoch": "browser-1",
    "source_label": "test",
    "correlation_id": None,
    "expected_owner_epoch": "program-1",
    "offer_id": _DIGEST,
    "offer_revision": 1,
    "action_id": "retry_motion_from_current_state",
    "reason": "operator-selected recovery action",
}


def _execute(client: TestClient, start: str):
    if start == "program_runs":
        return client.post("/api/program/runs", json={
            "request_id": "run-1", "request_owner_epoch": "browser-1", "source_label": "test",
            "correlation_id": None, "scenario_id": "cell-a",
            "source_document_digest": _DIGEST, "expected_applied_record_digest": _DIGEST,
            "draft_program_revision": _DIGEST,
        })
    if start == "recovery_action":
        return client.post("/api/cycle/recovery-actions", json=_RECOVERY)
    return client.post("/api/control/commands", json={
        "command_envelope": {
            "schema_version": "manyforge.execution_command.v1",
            "command_kind": start,
            "correlation_id": "start-1",
            "source_label": "test",
            "control_session_id": None,
            "client_sequence": None,
            "freshness": _FRESHNESS,
            "expected_semantic_owner_epoch": "program-1",
            "expected_execution_control_revision": 1,
        },
        "payload": _RECOVERY if start == "execution_recovery" else {},
    })


_STARTS = [
    "program_runs", "recovery_action", "program_run", "program_motion_start",
    "program_motion_resume", "program_resume_full_stop", "execution_recovery",
]


@pytest.mark.parametrize("start", _STARTS)
def test_execute_is_refused_while_a_chat_turn_runs_and_nothing_is_cancelled(start) -> None:
    headless = ExecuteHeadless()
    provider = RecordingProvider()
    state, client = _execute_state(headless, provider)
    state.assistant_turns.add("request.1")
    response = _execute(client, start)
    assert response.status_code == 409
    assert response.json()["detail"]["code"] == "execution_assistant_turn_in_progress"
    assert headless.started == []
    assert provider.cancellations == []
    assert state.assistant_turns == {"request.1"}


@pytest.mark.parametrize(
    "gone",
    [
        "completed",
        AssistantProviderClientError("assistant_request_not_found", "unknown", 404),
        AssistantProviderClientError("assistant_provider_unavailable", "refused", 503),
        "prepared",
        "absent",
    ],
    ids=["terminal", "not-found", "bridge-unreachable", "assistant-disabled",
         "assistant-removed"],
)
def test_execute_refreshes_ended_turns_first(gone) -> None:
    headless = ExecuteHeadless()
    provider = RecordingProvider()
    state, client = _execute_state(headless, provider)
    state.assistant_turns.add("request.1")
    if gone == "prepared":
        _prepared(headless)
    elif gone == "absent":
        _absent(headless)
    else:
        provider.states["request.1"] = gone
    assert _execute(client, "program_runs").status_code == 200
    assert headless.started == ["program_run_start"]
    assert state.assistant_turns == set()
    assert provider.cancellations == []


def test_a_turn_whose_status_times_out_still_refuses_execute() -> None:
    headless = ExecuteHeadless()
    provider = RecordingProvider()
    state, client = _execute_state(headless, provider)
    state.assistant_turns.add("request.1")
    provider.states["request.1"] = AssistantProviderClientError(
        "assistant_provider_timeout", "timed out", 504
    )
    response = _execute(client, "program_motion_start")
    assert response.json()["detail"]["code"] == "execution_assistant_turn_in_progress"
    assert state.assistant_turns == {"request.1"}


def test_execute_checks_turns_under_the_workspace_fence(monkeypatch) -> None:
    headless = ExecuteHeadless()
    provider = RecordingProvider()
    state, client = _execute_state(headless, provider)
    state.assistant_turns.add("request.1")
    fenced = []
    original = state.authoring_admission.submit_workspace_fenced

    def submit_workspace_fenced(callback):
        fenced.append(True)
        try:
            return original(callback)
        finally:
            fenced.pop()

    monkeypatch.setattr(state.authoring_admission, "submit_workspace_fenced",
                        submit_workspace_fenced)
    reads = []
    status = provider.status
    monkeypatch.setattr(provider, "status",
                        lambda url, request_id: reads.append(bool(fenced)) or status(url, request_id))
    for start in ("program_runs", "program_run"):
        assert _execute(client, start).status_code == 409
    assert reads == [True, True]
