"""A refused startup project leaves Composer serving an empty workspace."""

from __future__ import annotations

from typing import Any

import pytest
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
                "scenario_activation_incomplete",
                "only a complete v2 Scenario document can be loaded",
                status_code=409,
            )),
            "scenario_activation_incomplete",
            "only a complete v2 Scenario document can be loaded",
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

    def refuse(_state: ComposerState, path: str) -> dict[str, Any]:
        calls.append(path)
        raise refusal

    monkeypatch.setattr(routes_scenario_documents, "load_exact_scenario_document", refuse)
    before = state.authoring_workspace.snapshot().as_dict()

    composer_main._open_startup_scenario_document(state, state.headless_client, "/p/cell.scenario")

    assert calls == ["/p/cell.scenario"]  # One attempt, no retry.
    assert state.authoring_workspace.snapshot().as_dict() == before
    assert state.authoring_workspace.snapshot().as_dict()["project"]["document_path"] is None
    notice = {
        "path": "/p/cell.scenario", "code": code,
        "message": f"Startup project /p/cell.scenario was not activated: {reason}",
    }
    assert state.startup_scenario_notice == notice
    health = TestClient(create_app(state)).get("/api/health")
    assert health.status_code == 200
    assert health.json()["startup_scenario_notice"] == notice


def test_failed_startup_load_reports_its_failed_step(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """The orchestrated Load ended `failed` after the workspace commit: the
    project stays open, nothing is rolled back or retried."""

    state = ComposerState(headless_client=_Headless())  # type: ignore[arg-type]
    state.authoring_admission = _Admission()  # type: ignore[assignment]

    def commit_then_fail(_state: ComposerState, path: str) -> dict[str, Any]:
        document = build_scenario_document(
            scenario_id="cell", name="cell", description="", program_draft=None,
            deployment_draft=None,
            manyforge_version="1.0.0", last_known_status="incomplete",
        )
        state.authoring_workspace.commit_project(facts=scenario_project_facts(
            document, document_path=path,
            source_document_digest="sha256:" + "0" * 64,
        ))
        return {"outcome": "failed", "components": [], "error": {
            "code": "manager_apply_rejected", "message": "Manager rejected the candidate",
        }}

    monkeypatch.setattr(routes_scenario_documents, "load_exact_scenario_document", commit_then_fail)

    composer_main._open_startup_scenario_document(state, state.headless_client, "/p/cell.scenario")

    project = state.authoring_workspace.snapshot().as_dict()["project"]
    assert project["document_path"] == "/p/cell.scenario"  # Committed and kept.
    assert state.startup_scenario_notice == {
        "path": "/p/cell.scenario", "code": "manager_apply_rejected",
        "message": "Startup project /p/cell.scenario was not activated: "
        "Manager rejected the candidate",
    }


@pytest.mark.parametrize(
    ("graph", "code"),
    [
        (
            {"model_instances": [{"id": "lite6"}], "controllable_components": [{"id": "arm"}]},
            "scenario_new_required",
        ),
        (None, "scenario_load_current_configuration_unavailable"),
    ],
)
def test_restart_with_a_startup_project_keeps_an_applied_scenario(
    graph: dict[str, Any] | None, code: str,
) -> None:
    """A Composer restart never replaces the Scenario still applied in
    the Manager; the startup Load is refused before any effect."""

    class _AppliedHeadless(_Headless):
        def manager_status(self) -> dict[str, Any]:
            active: dict[str, Any] = {"retained_bundle_id": "b", "graph_revision": "g"}
            if graph is not None:
                active["cell_graph"] = graph
            return {"status": {"manager_ready": True, "active": active}}

    state = ComposerState(headless_client=_AppliedHeadless())  # type: ignore[arg-type]
    state.authoring_admission = _Admission()  # type: ignore[assignment]
    before = state.authoring_workspace.snapshot().as_dict()

    composer_main._open_startup_scenario_document(state, state.headless_client, "/p/cell.scenario")

    assert state.authoring_workspace.snapshot().as_dict() == before
    assert state.startup_scenario_notice is not None
    assert state.startup_scenario_notice["code"] == code


def test_startup_load_proceeds_over_an_empty_cell() -> None:
    class _EmptyHeadless(_Headless):
        def manager_status(self) -> dict[str, Any]:
            return {"status": {"active": {"cell_graph": {
                "model_instances": [], "controllable_components": [],
            }}}}

    state = ComposerState(headless_client=_EmptyHeadless())  # type: ignore[arg-type]
    routes_scenario_documents.require_empty_cell(state)  # no refusal
