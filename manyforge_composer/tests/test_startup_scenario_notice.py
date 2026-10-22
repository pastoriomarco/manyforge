"""A refused startup project leaves Composer serving an empty workspace."""

from __future__ import annotations

from typing import Any

import pytest
from fastapi import HTTPException
from fastapi.testclient import TestClient

from manyforge_composer import __main__ as composer_main
from manyforge_composer.backend import routes_scenario_documents
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.authoring_admission import scenario_artifact_http_exception
from manyforge_composer.backend.authoring_workspace import scenario_project_facts
from manyforge_composer.backend.scenario_artifacts import (
    ScenarioArtifactError,
    build_scenario_document,
)
from manyforge_composer.backend.state import ComposerState


class _Headless:
    def authoring_admission(self) -> dict[str, Any]:
        return {"authoring_allowed": True, "admission_generation": 1}

    def health(self) -> dict[str, Any]:
        return {"status": "ok", "headless_projection_ready": True}


class _Admission:
    owner_epoch = "composer-owner-1"

    def recover(self) -> None:
        return None


@pytest.mark.parametrize(
    ("refusal", "code", "reason"),
    [
        (
            scenario_artifact_http_exception(ScenarioArtifactError(
                "scenario_close_required", "Close first before loading a scenario",
                status_code=409,
            )),
            "scenario_close_required",
            "a Scenario is still applied. Close it, then Load.",
        ),
        (RuntimeError("Manager unreachable"), "startup_scenario_load_failed", "Manager unreachable"),
    ],
)
def test_refused_startup_project_starts_empty_with_a_visible_notice(
    monkeypatch: pytest.MonkeyPatch, refusal: Exception, code: str, reason: str,
) -> None:
    state = ComposerState(headless_client=_Headless())  # type: ignore[arg-type]
    state.authoring_admission = _Admission()  # type: ignore[assignment]
    calls: list[str] = []

    def refuse(_state: ComposerState, request: Any) -> dict[str, Any]:
        calls.append(request.path)
        raise refusal

    monkeypatch.setattr(routes_scenario_documents, "load_exact_scenario_document", refuse)
    before = state.authoring_workspace.snapshot().as_dict()

    composer_main._open_startup_scenario_document(state, state.headless_client, "/p/cell.scenario")

    assert calls == ["/p/cell.scenario"]  # One attempt, no retry.
    assert state.authoring_workspace.snapshot().as_dict() == before
    assert state.authoring_workspace.snapshot().as_dict()["project"]["document_path"] is None
    notice = {
        "path": "/p/cell.scenario", "code": code,
        "message": f"Startup project /p/cell.scenario was not opened: {reason}",
    }
    assert state.startup_scenario_notice == notice
    health = TestClient(create_app(state)).get("/api/health")
    assert health.status_code == 200
    assert health.json()["startup_scenario_notice"] == notice


def test_unconfirmed_startup_activation_says_the_project_was_opened(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Refused after the workspace commit: the project is open, nothing rolled back."""

    state = ComposerState(headless_client=_Headless())  # type: ignore[arg-type]
    state.authoring_admission = _Admission()  # type: ignore[assignment]
    route_message = (
        "The Scenario workspace was replaced, but Manager activation was not "
        "confirmed in this response."
    )

    def commit_then_refuse(_state: ComposerState, request: Any) -> dict[str, Any]:
        document = build_scenario_document(
            scenario_id="cell", name="cell", description="", program_draft=None,
            deployment_draft=None, program_role_assignment=None,
            manyforge_version="1.0.0", last_known_status="incomplete",
        )
        state.authoring_workspace.commit_project(facts=scenario_project_facts(
            document, document_path=request.path,
            source_document_digest="sha256:" + "0" * 64,
        ))
        raise HTTPException(status_code=503, detail={
            "code": "scenario_load_activation_not_confirmed", "message": route_message,
            "path": request.path, "managerOutcome": "not_confirmed",
        })

    monkeypatch.setattr(routes_scenario_documents, "load_exact_scenario_document", commit_then_refuse)

    composer_main._open_startup_scenario_document(state, state.headless_client, "/p/cell.scenario")

    project = state.authoring_workspace.snapshot().as_dict()["project"]
    assert project["document_path"] == "/p/cell.scenario"  # Committed and kept.
    assert state.startup_scenario_notice == {
        "path": "/p/cell.scenario", "code": "scenario_load_activation_not_confirmed",
        "message": "Startup project /p/cell.scenario was opened in the workspace, but its "
        f"activation was not confirmed: {route_message}",
    }
