"""Editable Scenario artifact persistence and discovery contracts."""

from __future__ import annotations

import json
import os
from pathlib import Path

import pytest
from fastapi import FastAPI
from fastapi.testclient import TestClient

from manyforge_composer.backend.scenario_artifacts import (
    ScenarioArtifactError,
    build_scenario_document,
    discover_scenario_artifacts,
    load_scenario_artifact,
    save_scenario_artifact,
)
from manyforge_composer.backend.routes_scenario_documents import (
    LoadScenarioDocumentRequest,
    _base_scenario_for_deployment,
    create_scenario_document_router,
)
from manyforge_composer.backend.state import ComposerState


def _document(*, version: str = "1.0.0") -> dict:
    return build_scenario_document(
        scenario_id="cell_fixture",
        name="Cell fixture",
        description="Editable Scenario fixture",
        base_scenario_id="maintained_cell",
        program_draft={"schema_version": "program-draft", "forest": []},
        deployment_draft={"api_version": "deployment-draft", "components": []},
        manyforge_version=version,
        last_known_status="ready",
    )


class _PassThroughAdmission:
    def execute(
        self,
        _envelope,
        *,
        effect,
        prepare_effect=None,
        validate_under_claim=None,
        **_kwargs,
    ):
        if prepare_effect is not None:
            prepare_effect()
        if validate_under_claim is not None:
            validate_under_claim({})
        return effect({})


class _PrepareThenAdmission(_PassThroughAdmission):
    def __init__(self, after_prepare) -> None:
        self._after_prepare = after_prepare

    def execute(self, *args, prepare_effect=None, **kwargs):
        if prepare_effect is not None:
            prepare_effect()
        self._after_prepare()
        validate = kwargs.get("validate_under_claim")
        if validate is not None:
            validate({})
        return kwargs["effect"]({})


def _scenario_client(state: ComposerState) -> TestClient:
    app = FastAPI()
    app.include_router(create_scenario_document_router(state))
    return TestClient(app)


def _fence(state: ComposerState) -> dict:
    program = state.draft_session.retained_state()
    deployment = state.deployment_draft_session.retained_state()
    return {
        "draftId": program["draft_id"],
        "expectedDraftRevision": program["draft_revision"],
        "deploymentDraftId": deployment["draft_id"],
        "expectedDeploymentDraftRevision": deployment["draft_revision"],
        "expectedAdmissionGeneration": 1,
        "requestOwnerEpoch": "browser-test-epoch",
        "requestId": "scenario-test-request",
    }


def test_save_load_and_discovery_preserve_one_editable_document(tmp_path: Path) -> None:
    target = tmp_path / "cell"
    saved_path = save_scenario_artifact(
        _document(),
        str(target),
        allowed_roots=[str(tmp_path)],
        overwrite=False,
    )

    assert saved_path == str((tmp_path / "cell.scenario.yaml").resolve())
    loaded, source = load_scenario_artifact(
        saved_path,
        allowed_roots=[str(tmp_path)],
    )
    assert source == saved_path
    assert loaded == _document()
    directory, entries = discover_scenario_artifacts(
        str(tmp_path),
        allowed_roots=[str(tmp_path)],
        manyforge_version="1.0.0",
    )
    assert directory == str(tmp_path.resolve())
    assert entries == [
        {
            "source_kind": "saved",
            "path": saved_path,
            "scenario_id": "cell_fixture",
            "name": "Cell fixture",
            "description": "Editable Scenario fixture",
            "base_scenario_id": "maintained_cell",
            "saved_manyforge_version": "1.0.0",
            "display_status": "ready",
            "status_detail": "Configuration was load-ready when last checked.",
        }
    ]


def test_external_content_and_version_changes_are_advisory(tmp_path: Path) -> None:
    target = Path(
        save_scenario_artifact(
            _document(),
            str(tmp_path / "cell.scenario.yaml"),
            allowed_roots=[str(tmp_path)],
            overwrite=False,
        )
    )
    edited = json.loads(target.read_text(encoding="utf-8"))
    edited["base_scenario_id"] = "another_cell"
    target.write_text(json.dumps(edited), encoding="utf-8")

    _, changed = discover_scenario_artifacts(
        str(tmp_path),
        allowed_roots=[str(tmp_path)],
        manyforge_version="1.0.0",
    )
    assert changed[0]["display_status"] == "changed"
    assert changed[0]["status_detail"] == "Content changed since the recorded status."

    edited["saved_status"]["checked_content_digest"] = build_scenario_document(
        scenario_id=edited["scenario_id"],
        name=edited["name"],
        description=edited["description"],
        base_scenario_id=edited["base_scenario_id"],
        program_draft=edited["program_draft"],
        deployment_draft=edited["deployment_draft"],
        manyforge_version="1.0.0",
        last_known_status="ready",
    )["saved_status"]["checked_content_digest"]
    target.write_text(json.dumps(edited), encoding="utf-8")
    _, version_changed = discover_scenario_artifacts(
        str(tmp_path),
        allowed_roots=[str(tmp_path)],
        manyforge_version="2.0.0",
    )
    assert version_changed[0]["display_status"] == "version_changed"


def test_discovery_exposes_unsupported_files_without_hiding_valid_ones(
    tmp_path: Path,
) -> None:
    save_scenario_artifact(
        _document(),
        str(tmp_path / "valid.scenario.yaml"),
        allowed_roots=[str(tmp_path)],
        overwrite=False,
    )
    (tmp_path / "broken.scenario.yaml").write_text("{not-json", encoding="utf-8")

    _, entries = discover_scenario_artifacts(
        str(tmp_path),
        allowed_roots=[str(tmp_path)],
        manyforge_version="1.0.0",
    )

    assert [entry["name"] for entry in entries] == [
        "broken.scenario.yaml",
        "Cell fixture",
    ]
    assert entries[0]["display_status"] == "unsupported"
    assert entries[1]["display_status"] == "ready"


def test_save_requires_explicit_overwrite_and_stays_inside_roots(
    tmp_path: Path,
) -> None:
    target = tmp_path / "cell.scenario.yaml"
    save_scenario_artifact(
        _document(),
        str(target),
        allowed_roots=[str(tmp_path)],
        overwrite=False,
    )
    with pytest.raises(ScenarioArtifactError) as existing:
        save_scenario_artifact(
            _document(),
            str(target),
            allowed_roots=[str(tmp_path)],
            overwrite=False,
        )
    assert existing.value.code == "scenario_artifact_overwrite_confirmation_required"

    outside = tmp_path.parent / "outside.scenario.yaml"
    with pytest.raises(ScenarioArtifactError) as denied:
        save_scenario_artifact(
            _document(),
            str(outside),
            allowed_roots=[str(tmp_path)],
            overwrite=False,
        )
    assert denied.value.code == "scenario_artifact_path_invalid"


def test_interrupted_overwrite_keeps_the_previous_scenario(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    target = tmp_path / "cell.scenario.yaml"
    save_scenario_artifact(
        _document(),
        str(target),
        allowed_roots=[str(tmp_path)],
        overwrite=False,
    )
    previous = target.read_bytes()
    replacement = _document()
    replacement["description"] = "replacement"

    def fail_replace(*_args, **_kwargs) -> None:
        raise OSError("simulated interrupted publication")

    monkeypatch.setattr(os, "replace", fail_replace)
    with pytest.raises(ScenarioArtifactError) as interrupted:
        save_scenario_artifact(
            replacement,
            str(target),
            allowed_roots=[str(tmp_path)],
            overwrite=True,
        )

    assert interrupted.value.code == "scenario_artifact_write_failed"
    assert target.read_bytes() == previous
    assert list(tmp_path.glob(".*.tmp-*")) == []


def test_routes_save_incomplete_setup_and_reopen_it_without_runtime_effect(
    tmp_path: Path,
) -> None:
    state = ComposerState(allowed_fs_roots=[str(tmp_path)])
    state.authoring_admission = _PassThroughAdmission()  # type: ignore[assignment]
    client = _scenario_client(state)
    target = tmp_path / "draft.scenario.yaml"

    saved = client.post(
        "/api/scenario-documents/save",
        json={
            **_fence(state),
            "path": str(target),
            "scenarioId": "draft_cell",
            "name": "Draft cell",
            "description": "",
            "confirmOverwrite": False,
        },
    )
    assert saved.status_code == 200
    assert saved.json()["status"] == "saved"
    assert saved.json()["last_known_status"] == "incomplete"
    assert target.is_file()

    state.headless_client = object()  # type: ignore[assignment]
    opened = client.post(
        "/api/scenario-documents/load",
        json={
            **_fence(state),
            "path": str(target),
            "alternateDeploymentPath": None,
            "confirmDiscard": False,
        },
    )
    assert opened.status_code == 200
    assert opened.json()["status"] == "opened"
    assert opened.json()["activation"] is None
    assert opened.json()["workspace"]["draft"]["programLoaded"] is False


def test_load_uses_the_captured_file_when_external_content_changes(
    tmp_path: Path,
) -> None:
    target = tmp_path / "captured.scenario.yaml"
    original = build_scenario_document(
        scenario_id="captured_cell",
        name="Captured cell",
        description="",
        base_scenario_id=None,
        program_draft=None,
        deployment_draft=None,
        manyforge_version="1.0.0",
        last_known_status="incomplete",
    )
    replacement = build_scenario_document(
        scenario_id="replacement_cell",
        name="Replacement cell",
        description="",
        base_scenario_id=None,
        program_draft=None,
        deployment_draft=None,
        manyforge_version="1.0.0",
        last_known_status="incomplete",
    )
    save_scenario_artifact(
        original,
        str(target),
        allowed_roots=[str(tmp_path)],
        overwrite=False,
    )
    state = ComposerState(allowed_fs_roots=[str(tmp_path)])
    state.headless_client = object()  # type: ignore[assignment]
    state.authoring_admission = _PrepareThenAdmission(
        lambda: target.write_text(json.dumps(replacement), encoding="utf-8")
    )  # type: ignore[assignment]

    response = _scenario_client(state).post(
        "/api/scenario-documents/load",
        json={
            **_fence(state),
            "path": str(target),
            "alternateDeploymentPath": None,
            "confirmDiscard": False,
        },
    )

    assert response.status_code == 200
    assert response.json()["scenario_id"] == "captured_cell"
    assert json.loads(target.read_text(encoding="utf-8"))["scenario_id"] == (
        "replacement_cell"
    )


def test_dynamic_index_merges_saved_and_current_qualified_scenarios(
    tmp_path: Path,
) -> None:
    save_scenario_artifact(
        _document(),
        str(tmp_path / "saved.scenario.yaml"),
        allowed_roots=[str(tmp_path)],
        overwrite=False,
    )

    class Headless:
        def scenarios(self) -> dict:
            return {
                "scenario_catalog_revision": "sha256:" + "1" * 64,
                "scenarios": [
                    {
                        "scenario_id": "installed_cell",
                        "description": "Installed fixture",
                    }
                ],
            }

    state = ComposerState(allowed_fs_roots=[str(tmp_path)])
    state.headless_client = Headless()  # type: ignore[assignment]
    response = _scenario_client(state).get(
        "/api/scenario-documents",
        params={"directory": str(tmp_path)},
    )

    assert response.status_code == 200
    body = response.json()
    assert body["catalog_revision"] == "sha256:" + "1" * 64
    assert [(row["source_kind"], row["scenario_id"]) for row in body["entries"]] == [
        ("saved", "cell_fixture"),
        ("qualified", "installed_cell"),
    ]


def test_explicit_installed_base_accepts_an_edited_deployment_artifact() -> None:
    class Headless:
        def scenarios(self) -> dict:
            return {
                "scenario_catalog_revision": "sha256:" + "1" * 64,
                "scenarios": [
                    {
                        "scenario_id": "maintained_cell",
                        "deployment_artifact_id": "original_artifact",
                    }
                ],
            }

    state = ComposerState()
    state.headless_client = Headless()  # type: ignore[assignment]

    assert _base_scenario_for_deployment(
        state,
        "user_edited_artifact",
        "maintained_cell",
    ) == ("maintained_cell", "sha256:" + "1" * 64)


def test_qualified_scenario_override_has_one_unambiguous_source() -> None:
    request = LoadScenarioDocumentRequest.model_validate(
        {
            "draftId": "draft-1",
            "expectedDraftRevision": 1,
            "deploymentDraftId": "deployment-draft-1",
            "expectedDeploymentDraftRevision": 1,
            "expectedAdmissionGeneration": 1,
            "requestOwnerEpoch": "browser",
            "requestId": "request",
            "path": None,
            "qualifiedScenarioId": "maintained_cell",
            "alternateDeploymentPath": "/workspace/alternate.deployment.yaml",
            "confirmDiscard": False,
        }
    )

    assert request.path is None
    assert request.qualified_scenario_id == "maintained_cell"
    with pytest.raises(ValueError, match="exactly one Scenario path"):
        LoadScenarioDocumentRequest.model_validate(
            {
                **request.model_dump(mode="json", by_alias=True),
                "path": "/workspace/cell.scenario.yaml",
            }
        )


def test_qualified_scenario_can_load_its_qualified_deployment() -> None:
    request = LoadScenarioDocumentRequest.model_validate(
        {
            "draftId": "draft-1",
            "expectedDraftRevision": 1,
            "deploymentDraftId": "deployment-draft-1",
            "expectedDeploymentDraftRevision": 1,
            "expectedAdmissionGeneration": 1,
            "requestOwnerEpoch": "browser",
            "requestId": "request",
            "qualifiedScenarioId": "maintained_cell",
        }
    )

    assert request.path is None
    assert request.qualified_scenario_id == "maintained_cell"
    assert request.alternate_deployment_path is None
