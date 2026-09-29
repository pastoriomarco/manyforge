"""Editable Scenario-project persistence and discovery contracts."""

from __future__ import annotations

import json
import os
from copy import deepcopy
from pathlib import Path

import pytest
from fastapi import FastAPI
from fastapi.testclient import TestClient

from manyforge_composer.backend import routes_scenario_documents
from manyforge_composer.backend.routes_scenario_documents import (
    _document_content_digest,
    create_scenario_document_router,
)
from manyforge_composer.backend.authoring_admission import LocalAuthoringCommit
from manyforge_composer.backend.authoring_forest import from_program, derive_candidate
from manyforge_composer.backend.routes_draft import create_draft_router
from manyforge_composer.backend.routes_deployment_draft import create_deployment_draft_router
from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.program import Program
from manyforge_headless_api.client import HeadlessClientError
from manyforge_composer.backend.scenario_artifacts import (
    ScenarioArtifactError,
    build_scenario_document,
    capture_scenario_artifact,
    capture_scenario_artifact_with_diagnostics,
    discover_scenario_artifacts,
    export_scenario_artifact,
    load_scenario_artifact,
    save_scenario_artifact,
)
from manyforge_composer.backend.state import ComposerState


_PROGRAM_FIXTURE = from_program(
    Program(
        Path(__file__).resolve().parents[2] / "examples/pick_and_place_minimal.program.yaml",
        build_node_environment().catalog,
    )
)


def _deployment_intent() -> dict:
    return {
        "api_version": "manyforge.deployment_intent.v2",
        "deployment_artifact_id": "fixture_deployment",
        "name": "Fixture deployment",
        "components": [
            {
                "component_id": "fixture_robot",
                "component_profile_id": "fixture_native_v1",
                "model_source": {
                    "urdf": {"project_path": "../model.urdf"},
                    "package_roots": {},
                },
                "root_pose_in_cell": {
                    "position_m": [0.0, 0.0, 0.0],
                    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                },
                "target_settings": {},
                "configuration": {},
            }
        ],
        "attachments": [],
        "policies": {},
    }


def _document(*, version: str = "1.0.0", overrides: dict | None = None) -> dict:
    return build_scenario_document(
        scenario_id="cell_fixture",
        name="Cell fixture",
        description="Editable Scenario fixture",
        program_draft=deepcopy(_PROGRAM_FIXTURE),
        deployment_draft=_deployment_intent(),
        program_role_assignment={"component_roles": [], "target_roles": []},
        typed_parameter_overrides={} if overrides is None else overrides,
        manyforge_version=version,
        last_known_status="ready",
    )


def test_persisted_document_identity_includes_authorable_metadata() -> None:
    original = _document()
    renamed = {**original, "name": "Renamed cell"}
    redescribed = {**original, "description": "Updated description"}

    assert _document_content_digest(original) != _document_content_digest(renamed)
    assert _document_content_digest(original) != _document_content_digest(
        redescribed
    )


def test_scenario_save_adopts_both_baselines_and_member_saves_preserve_other_draft(
    tmp_path: Path,
) -> None:
    state = ComposerState(allowed_fs_roots=[str(tmp_path)])
    state.authoring_admission = _PassThroughAdmission()  # type: ignore[assignment]
    authoring = deepcopy(_PROGRAM_FIXTURE)
    program, diagnostics = derive_candidate(authoring, state.node_catalog)
    post = routes_scenario_documents._workspace_post_state(
        state, authoring, program, diagnostics, _deployment_intent()
    )
    state.draft_session.install_prepared_replacement(
        state.draft_session.prepare_load_replacement(post["program"], state.node_catalog)
    )
    state.deployment_draft_session.install_prepared_replacement(
        state.deployment_draft_session.prepare_load_replacement(post["deployment"])
    )
    app = FastAPI()
    app.include_router(create_scenario_document_router(state))
    app.include_router(create_draft_router(state))
    app.include_router(create_deployment_draft_router(state))
    client = TestClient(app)
    target = tmp_path / "saved"
    before = state.authoring_workspace.identity()
    response = client.post("/api/scenario-documents/save", json={
        **_fence(state), "path": str(target), "scenarioId": "saved_cell",
        "name": "Saved cell", "description": "", "confirmOverwrite": False,
    })
    assert response.status_code == 200, response.text
    assert response.json()["workspace"]["hasUnsavedChanges"] is False
    assert response.json()["workspace"]["applied"]["available"] is False
    saved = state.authoring_workspace.snapshot().as_dict()
    assert saved["workspaceRevision"] == before.workspace_revision + 1
    assert saved["program"]["saved"]["matchesDraft"] is True
    assert saved["deployment"]["differsFromSaved"] is False
    assert state.current_drafts_match_remembered_scenario()

    # Two unrelated unsaved edits. Save Program must not persist Deployment.
    program_state = state.draft_session.retained_state()
    program_state["authoring"]["name"] = "Edited Program"
    edited, diagnostics = derive_candidate(program_state["authoring"], state.node_catalog)
    assert edited is not None
    program_state["program"] = edited.to_dict()
    program_state["diagnostics"] = diagnostics
    program_state["draft_revision"] += 1
    state.draft_session.install_prepared_replacement(
        state.draft_session.prepare_load_replacement(program_state, state.node_catalog)
    )
    deployment_state = state.deployment_draft_session.retained_state()
    deployment_state["document"]["name"] = "Edited Deployment"
    deployment_state["draft_revision"] += 1
    state.deployment_draft_session.install_prepared_replacement(
        state.deployment_draft_session.prepare_load_replacement(deployment_state)
    )

    def member_fence(*, deployment: bool = False) -> dict:
        fence = _fence(state)
        if deployment:
            fence["draftId"] = fence["deploymentDraftId"]
            fence["expectedDraftRevision"] = fence["expectedDeploymentDraftRevision"]
        del fence["deploymentDraftId"], fence["expectedDeploymentDraftRevision"]
        return fence

    response = client.post("/api/program/save", json=member_fence())
    assert response.status_code == 200, response.text
    disk, _ = load_scenario_artifact(str(target), allowed_roots=[str(tmp_path)])
    assert disk["program_draft"]["name"] == "Edited Program"
    assert disk["deployment_draft"]["name"] == "Fixture deployment"
    saved = state.authoring_workspace.snapshot().as_dict()
    assert saved["program"]["saved"]["matchesDraft"] is True
    assert saved["deployment"]["differsFromSaved"] is True
    assert not state.current_drafts_match_remembered_scenario()
    response = client.post("/api/deployment/save", json=member_fence(deployment=True))
    assert response.status_code == 200, response.text
    disk, _ = load_scenario_artifact(str(target), allowed_roots=[str(tmp_path)])
    assert disk["deployment_draft"]["name"] == "Edited Deployment"
    assert state.current_drafts_match_remembered_scenario()
    assert state.authoring_workspace.snapshot().deployment["differsFromSaved"] is False
    assert sorted(path.name for path in target.iterdir()) == [
        "assignment.yaml", "deployment.yaml", "overrides.yaml", "program.yaml", "scenario.yaml"
    ]

    # Save As changes the project location, not the referenced model. A later
    # Deployment-only Save must keep the same source after rebasing as well.
    (tmp_path / "nested").mkdir()
    copy = tmp_path / "nested" / "copy"
    response = client.post("/api/scenario-documents/save", json={
        **_fence(state), "path": str(copy), "scenarioId": "saved_copy",
        "name": "Saved copy", "description": "", "confirmOverwrite": False,
    })
    assert response.status_code == 200, response.text
    deployment_state = state.deployment_draft_session.retained_state()
    deployment_state["document"]["name"] = "Edited after Save As"
    deployment_state["draft_revision"] += 1
    state.deployment_draft_session.install_prepared_replacement(
        state.deployment_draft_session.prepare_load_replacement(
            deployment_state,
            source_project_root=state.deployment_draft_session.source_project_root,
        )
    )
    response = client.post("/api/deployment/save", json=member_fence(deployment=True))
    assert response.status_code == 200, response.text
    disk, _ = load_scenario_artifact(str(copy), allowed_roots=[str(tmp_path)])
    reference = disk["deployment_draft"]["components"][0]["model_source"]["urdf"]["project_path"]
    assert (copy / reference).resolve() == tmp_path / "model.urdf"
    assert state.current_drafts_match_remembered_scenario()


@pytest.fixture(autouse=True)
def _fixture_model_source(tmp_path: Path) -> None:
    (tmp_path / "model.urdf").write_text(
        '<robot name="fixture"/>\n', encoding="utf-8"
    )


class _PassThroughAdmission:
    def __init__(self) -> None:
        self.events: list[str] = []

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

    def execute_local_mutation(
        self,
        _envelope,
        *,
        session="program",
        draft_id=None,
        workspace,
        install=None,
        expected_identity=None,
        project=None,
        prepare_project_under_claim=None,
        **_kwargs,
    ):
        """Commit through the real composite owner; only the Kernel is stubbed."""

        if prepare_project_under_claim is not None:
            project = prepare_project_under_claim()
        if session == "whole-workspace":
            snapshot = workspace.replace(
                expected_identity=expected_identity,
                install=install,
                project=project,
            )
        elif session == "project":
            snapshot = workspace.commit_project(
                expected_identity=expected_identity,
                facts=project,
            )
        elif session == "deployment":
            snapshot = workspace.commit_deployment(
                draft_id=draft_id,
                expected_identity=expected_identity,
                install=install,
            )
        else:
            snapshot = workspace.commit_program(
                draft_id=draft_id,
                expected_identity=expected_identity,
                install=install,
            )
        return LocalAuthoringCommit(snapshot=snapshot, admission_generation=2)

    def execute_external_submission(
        self,
        _envelope,
        *,
        validate_under_claim,
        submit,
        commit_before_submit=None,
        after_release=None,
        **_kwargs,
    ):
        self.events.append("validate")
        claim = {
            "claim": "test",
            "acquired_admission_generation": 2,
        }
        validate_under_claim(claim)
        if commit_before_submit is not None:
            self.events.append("commit")
            commit_before_submit()
        self.events.append("submit")
        result = submit(claim)
        if after_release is not None:
            self.events.append("wake")
            after_release()
        return result


def _scenario_client(state: ComposerState) -> TestClient:
    app = FastAPI()
    app.include_router(create_scenario_document_router(state))
    return TestClient(app)


def _fence(state: ComposerState) -> dict:
    program = state.draft_session.retained_state()
    deployment = state.deployment_draft_session.retained_state()
    identity = state.authoring_workspace.identity()
    return {
        "draftId": program["draft_id"],
        "expectedDraftRevision": program["draft_revision"],
        "deploymentDraftId": deployment["draft_id"],
        "expectedDeploymentDraftRevision": deployment["draft_revision"],
        "expectedAdmissionGeneration": 1,
        "requestOwnerEpoch": "browser-test-epoch",
        "requestId": "scenario-test-request",
        "expectedWorkspaceEpoch": identity.workspace_epoch,
        "expectedWorkspaceRevision": identity.workspace_revision,
    }


def test_save_load_and_discovery_preserve_one_editable_project(
    tmp_path: Path,
) -> None:
    saved_path = save_scenario_artifact(
        _document(),
        str(tmp_path / "cell"),
        allowed_roots=[str(tmp_path)],
        overwrite=False,
    )

    assert saved_path == str((tmp_path / "cell").resolve())
    assert sorted(path.name for path in Path(saved_path).iterdir()) == [
        "assignment.yaml",
        "deployment.yaml",
        "overrides.yaml",
        "program.yaml",
        "scenario.yaml",
    ]
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
            "saved_manyforge_version": "1.0.0",
            "display_status": "ready",
            "status_detail": "Previously recorded as load-ready. Current files and model sources are validated when you load this Scenario.",
        }
    ]


def test_listing_reports_history_not_current_content_validation(tmp_path: Path) -> None:
    target = Path(
        save_scenario_artifact(
            _document(),
            str(tmp_path / "cell"),
            allowed_roots=[str(tmp_path)],
            overwrite=False,
        )
    )
    (target / "overrides.yaml").write_text(
        json.dumps({"speed": 0.5}), encoding="utf-8"
    )

    _, changed = discover_scenario_artifacts(
        str(tmp_path),
        allowed_roots=[str(tmp_path)],
        manyforge_version="1.0.0",
    )
    assert changed[0]["display_status"] == "ready"
    assert "Previously recorded" in changed[0]["status_detail"]

    save_scenario_artifact(
        _document(overrides={"speed": 0.5}),
        str(target),
        allowed_roots=[str(tmp_path)],
        overwrite=True,
    )
    _, version_changed = discover_scenario_artifacts(
        str(tmp_path),
        allowed_roots=[str(tmp_path)],
        manyforge_version="2.0.0",
    )
    assert version_changed[0]["display_status"] == "version_changed"


@pytest.mark.parametrize("broken_member", ["program.yaml", "../model.urdf"])
def test_discovery_reads_only_manifest_but_load_still_validates(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, broken_member: str,
) -> None:
    from manyforge_composer.backend import scenario_artifacts

    target = Path(save_scenario_artifact(
        _document(), str(tmp_path / "cell"),
        allowed_roots=[str(tmp_path)], overwrite=False,
    ))
    (target / broken_member).unlink()
    original_read = scenario_artifacts._read_regular_at
    reads: list[str] = []

    def manifest_only(directory_fd: int, name: str, *, source: str) -> bytes:
        reads.append(name)
        assert name == "scenario.yaml"
        return original_read(directory_fd, name, source=source)

    def forbid_source_capture(*args: object, **kwargs: object) -> None:
        pytest.fail("Discovery must not capture model resources")

    with monkeypatch.context() as patch:
        patch.setattr(scenario_artifacts, "_read_regular_at", manifest_only)
        patch.setattr(scenario_artifacts, "capture_deployment_model_sources", forbid_source_capture)
        _, entries = discover_scenario_artifacts(
            str(tmp_path), allowed_roots=[str(tmp_path)], manyforge_version="1.0.0",
        )
    assert reads == ["scenario.yaml"]
    assert entries[0]["display_status"] == "ready"
    assert "Previously recorded" in entries[0]["status_detail"]
    with pytest.raises(ScenarioArtifactError):
        load_scenario_artifact(str(target), allowed_roots=[str(tmp_path)])


def test_discovery_exposes_broken_projects_without_listing_legacy_files(
    tmp_path: Path,
) -> None:
    save_scenario_artifact(
        _document(),
        str(tmp_path / "valid"),
        allowed_roots=[str(tmp_path)],
        overwrite=False,
    )
    broken = tmp_path / "broken"
    broken.mkdir()
    (broken / "scenario.yaml").write_text("{not-json", encoding="utf-8")
    (tmp_path / "legacy.scenario.yaml").write_text("{}", encoding="utf-8")

    _, entries = discover_scenario_artifacts(
        str(tmp_path),
        allowed_roots=[str(tmp_path)],
        manyforge_version="1.0.0",
    )

    assert [entry["name"] for entry in entries] == ["broken", "Cell fixture"]
    assert entries[0]["display_status"] == "unsupported"
    assert entries[1]["display_status"] == "ready"


def test_save_requires_explicit_overwrite_and_stays_inside_roots(
    tmp_path: Path,
) -> None:
    target = tmp_path / "cell"
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

    with pytest.raises(ScenarioArtifactError) as denied:
        save_scenario_artifact(
            _document(),
            str(tmp_path.parent / "outside"),
            allowed_roots=[str(tmp_path)],
            overwrite=False,
        )
    assert denied.value.code == "scenario_artifact_path_invalid"


def test_interrupted_overwrite_is_detected_without_rollback(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    target = tmp_path / "cell"
    save_scenario_artifact(
        _document(),
        str(target),
        allowed_roots=[str(tmp_path)],
        overwrite=False,
    )
    previous_manifest = (target / "scenario.yaml").read_bytes()
    replacement = _document()
    replacement["description"] = "replacement"
    original_replace = os.replace

    def fail_replace(source, destination, *args, **kwargs) -> None:
        if destination == "deployment.yaml":
            raise OSError("simulated interrupted publication")
        original_replace(source, destination, *args, **kwargs)

    monkeypatch.setattr(os, "replace", fail_replace)
    with pytest.raises(ScenarioArtifactError) as interrupted:
        save_scenario_artifact(
            replacement,
            str(target),
            allowed_roots=[str(tmp_path)],
            overwrite=True,
        )

    assert interrupted.value.code == "scenario_artifact_write_failed"
    assert (target / "scenario.yaml").read_bytes() == previous_manifest
    assert (target / ".manyforge-save-pending.json").is_file()
    assert list(target.glob(".*.tmp-*")) == []
    with pytest.raises(ScenarioArtifactError) as reopened:
        load_scenario_artifact(str(target), allowed_roots=[str(tmp_path)])
    assert reopened.value.code == "scenario_project_save_interrupted"


def test_routes_reject_loading_incomplete_scenario_without_workspace_mutation(
    tmp_path: Path,
) -> None:
    state = ComposerState(allowed_fs_roots=[str(tmp_path)])
    state.authoring_admission = _PassThroughAdmission()  # type: ignore[assignment]
    client = _scenario_client(state)
    target = tmp_path / "draft"

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
    assert saved.json()["workspace"]["authoringWorkspace"]["workspaceEpoch"]
    assert "authoringWorkspace" not in saved.json()
    assert target.is_dir()
    assert (target / "scenario.yaml").is_file()

    before_load = state.authoring_workspace.snapshot().identity
    rejected = client.post(
        "/api/scenario-documents/load",
        json={
            **_fence(state),
            "path": str(target),
            "confirmDiscard": False,
        },
    )
    assert rejected.status_code == 409
    assert rejected.json()["detail"]["code"] == "scenario_activation_incomplete"
    assert state.authoring_workspace.snapshot().identity == before_load


def test_save_keeps_written_file_but_refuses_a_stale_saved_baseline(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    state = ComposerState(allowed_fs_roots=[str(tmp_path)])
    state.authoring_admission = _PassThroughAdmission()  # type: ignore[assignment]
    client = _scenario_client(state)
    target = tmp_path / "raced-save"
    fence = _fence(state)
    original_save = routes_scenario_documents.save_scenario_artifact

    def write_then_commit_concurrent_edit(*args, **kwargs) -> str:
        saved_path = original_save(*args, **kwargs)
        snapshot = state.authoring_workspace.snapshot()
        project = snapshot.as_dict()["project"]
        project["document_description"] = "concurrent edit"
        state.authoring_workspace.commit_project(
            expected_identity=snapshot.identity,
            facts=project,
        )
        return saved_path

    monkeypatch.setattr(
        routes_scenario_documents,
        "save_scenario_artifact",
        write_then_commit_concurrent_edit,
    )
    response = client.post(
        "/api/scenario-documents/save",
        json={
            **fence,
            "path": str(target),
            "scenarioId": "raced_cell",
            "name": "Raced cell",
            "description": "captured before the race",
            "confirmOverwrite": False,
        },
    )

    assert response.status_code == 409
    assert response.json()["detail"]["code"] == (
        "scenario_saved_baseline_not_adopted"
    )
    assert response.json()["detail"]["path"] == str(target.resolve())
    written, _source = load_scenario_artifact(
        str(target), allowed_roots=[str(tmp_path)]
    )
    assert written["description"] == "captured before the race"
    current = state.authoring_workspace.snapshot().as_dict()
    assert current["project"]["document_description"] == "concurrent edit"
    assert current["program"]["saved"]["available"] is False
    assert current["deployment"]["savedPath"] is None


@pytest.mark.parametrize("preparation_fails", [False, True])
def test_scenario_load_uses_one_claim_for_workspace_commit_and_activation(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, preparation_fails: bool
) -> None:
    target = Path(
        save_scenario_artifact(
            _document(),
            str(tmp_path / "loaded-project"),
            allowed_roots=[str(tmp_path)],
            overwrite=False,
        )
    )
    state = ComposerState(allowed_fs_roots=[str(tmp_path)])
    admission = _PassThroughAdmission()
    state.authoring_admission = admission  # type: ignore[assignment]
    state.headless_client = type(
        "ManagerStub", (), {"manager_status": lambda _self: {}}
    )()  # type: ignore[assignment]
    client = _scenario_client(state)

    preview = {
        "diff": {},
        "candidate_receipt": {
            "expected_current": {
                "retained_bundle_id": "current-bundle",
                "graph_revision": "current-graph",
            }
        },
    }
    confirmation_token = "sha256:" + "c" * 64
    observed_generations: list[tuple[int, int | None]] = []
    before = state.authoring_workspace.snapshot()
    def prepare_or_apply(
        intent,
        claim,
        *,
        apply,
        prepared=None,
        prepare_only=False,
    ):
        if preparation_fails:
            raise HeadlessClientError(
                "activation_preparation_failed",
                "Activation preparation failed",
                status_code=409,
                details={"preparer_detail": "invalid candidate geometry"},
            )
        acquired_generation = (
            None
            if claim is None
            else claim.get("acquired_admission_generation")
        )
        observed_generations.append(
            (intent["expected_admission_generation"], acquired_generation)
        )
        candidate = {
            "schema_version": "manyforge.prepared_activation_input.v3",
            "prepared_activation_id": "prepared-load-test",
            "scenario_id": intent["scenario_id"],
            "source_document_digest": intent["source_document_digest"],
            "captured_input_digest": intent["captured_input_digest"],
            "deployment_artifact_id": "fixture_deployment",
            "deployment_revision": "deployment-revision",
            "retained_bundle_id": "bundle-load-test",
            "graph_revision": "graph-load-test",
            "program_id": "program-load-test",
            "program_revision": "program-revision",
            "program_outcome": {"kind": "bound", "program_binding_id": "binding-load-test"},
            "node_environment_digest": "sha256:" + "a" * 64,
            "assistant_mode": "forbidden",
            "typed_parameter_overrides": {},
        }
        if prepare_only:
            if intent["confirmation_token"] is not None:
                assert intent["confirmation_token"] == confirmation_token
            return {
                "prepared_activation": candidate,
                "candidate_preview": dict(preview),
                "manager_apply": None,
                "confirmation_required": intent["confirmation_token"] is None,
                "confirmation_token": (
                    confirmation_token
                    if intent["confirmation_token"] is None
                    else None
                ),
                "preparation": None,
                "phases_ms": {},
            }
        assert claim == {
            "claim": "test",
            "acquired_admission_generation": 2,
        }
        assert apply is True
        assert prepared["prepared_activation"] == candidate
        return {"status": "applied", "apply_id": None}

    monkeypatch.setattr(state, "_manager_current", lambda _status: (
        "current-bundle", "current-graph", "manager-epoch"
    ))
    monkeypatch.setattr(
        state, "execute_authored_deployment_candidate", prepare_or_apply
    )
    monkeypatch.setattr(
        routes_scenario_documents,
        "program_workspace",
        lambda _state: {"authoringWorkspace": {"workspaceEpoch": "test"}},
    )
    response = client.post(
        "/api/scenario-documents/load",
        json={**_fence(state), "path": str(target), "confirmDiscard": False},
    )

    if preparation_fails:
        assert response.status_code == 409, response.text
        assert response.json()["detail"] == {
            "code": "activation_preparation_failed",
            "message": "Activation preparation failed",
            "details": {"preparer_detail": "invalid candidate geometry"},
        }
        assert admission.events == []
        assert state.authoring_workspace.snapshot() == before
        return

    assert response.status_code == 200, response.text
    assert response.json()["status"] == "applied"
    assert response.json()["activation"] == {
        "status": "applied",
        "apply_id": None,
    }
    assert admission.events == ["validate", "commit", "submit", "wake"]
    assert observed_generations == [(1, None), (1, None), (1, 2)]
    assert state.scenario_document_id == "cell_fixture"
    loaded = state.authoring_workspace.snapshot().as_dict()
    # The reply carries the full committed snapshot the browser parser
    # requires (the same shape as every other authoring route), not only its
    # identity.
    assert response.json()["authoringWorkspace"] == loaded
    assert loaded["schemaVersion"] == (
        "manyforge.composer_authoring_workspace_snapshot.v1"
    )
    assert loaded["program"]["saved"]["matchesDraft"] is True
    assert loaded["program"]["saved"]["path"] == str(target / "program.yaml")
    assert loaded["deployment"]["differsFromSaved"] is False
    assert loaded["deployment"]["savedPath"] == str(target / "deployment.yaml")


@pytest.mark.parametrize("choice", ["empty", "keep_applied"])
@pytest.mark.parametrize("failure", [None, "prepare", "save", "submit", "stale", "unexpected_impact"])
def test_new_empty_scenario_saves_before_activation_and_preserves_failure(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, failure: str | None, choice: str
) -> None:
    state = ComposerState(allowed_fs_roots=[str(tmp_path)])
    admission = _PassThroughAdmission()
    state.authoring_admission = admission  # type: ignore[assignment]
    state.headless_client = type("Manager", (), {"manager_status": lambda _self: {}})()  # type: ignore[assignment]
    monkeypatch.setattr(state, "_manager_current", lambda _status: ("bundle", "graph", "epoch"))
    monkeypatch.setattr(routes_scenario_documents, "program_workspace",
                        lambda _state: {"authoringWorkspace": {"workspaceEpoch": "test"}})
    target = tmp_path / "new-project"
    old_project = tmp_path / "old-project"
    (tmp_path / "model.urdf").write_text('<robot name="fixture"><link name="base"/></robot>')
    save_scenario_artifact(_document(), str(old_project), allowed_roots=[str(tmp_path)], overwrite=False)
    original_files = {path.name: path.read_bytes() for path in old_project.iterdir() if path.is_file()}
    source_closure = routes_scenario_documents.capture_deployment_model_sources(
        _deployment_intent(), project_root=old_project)
    monkeypatch.setattr(state, "published_scenario", lambda: {"published_scenario": {"active": True}})
    monkeypatch.setattr(state, "applied_activation_for_publication", lambda _publication: {
        "deployment": _deployment_intent(), "project_path": str(old_project),
        "model_source_closure": source_closure,
    })
    before = state.authoring_workspace.snapshot()
    calls: list[str] = []

    def prepare_or_apply(intent, claim, *, apply, prepared=None, prepare_only=False):
        assert intent["program"] is None
        assert intent["program_authoring"]["forest"] == []
        assert not intent["program_authoring"].get("scene")
        if choice == "empty":
            assert intent["deployment"]["components"] == []
        else:
            assert intent["deployment"]["name"] == "Fixture deployment"
            assert len(intent["deployment"]["components"]) == 1
        assert intent["program_role_assignment"] == {"component_roles": [], "target_roles": []}
        assert intent["typed_parameter_overrides"] == {}
        if prepare_only:
            assert not target.exists()
            calls.append("prepare")
            if failure == "prepare":
                raise HeadlessClientError("activation_preparation_failed", "invalid candidate", status_code=409)
            return {"confirmation_required": False, "prepared_activation": {},
                    "candidate_preview": {"impact": {"components": [{
                        "controllable_component_id": "fixture_robot",
                        "disposition": "replace" if failure == "unexpected_impact" else
                            "retain" if choice == "keep_applied" else "remove",
                    }]}}}
        calls.append("submit")
        saved, _path, digest = capture_scenario_artifact(str(target), allowed_roots=[str(tmp_path)])
        assert digest == intent["source_document_digest"]
        assert saved["program_draft"] == intent["program_authoring"]
        assert saved["name"] == "new_cell"
        if failure == "submit":
            raise HeadlessClientError("manager_unavailable", "reply lost", status_code=503)
        return {"status": "accepted", "apply_id": "new-apply"}

    monkeypatch.setattr(state, "execute_authored_deployment_candidate", prepare_or_apply)
    if failure == "save":
        def refuse_save(*_args, **_kwargs):
            raise ScenarioArtifactError("scenario_save_failed", "disk unavailable", status_code=503)
        monkeypatch.setattr(routes_scenario_documents, "save_scenario_artifact", refuse_save)
    response = _scenario_client(state).post("/api/scenario-documents/new", json={
        **_fence(state), "path": str(target), "name": "new_cell", "deploymentChoice": choice,
        "confirmDiscard": True, "expectedRetainedBundleId": "other" if failure == "stale" else "bundle",
        "expectedGraphRevision": "graph",
    })
    assert {path.name: path.read_bytes() for path in old_project.iterdir() if path.is_file()} == original_files
    if failure in {"prepare", "save", "stale", "unexpected_impact"}:
        assert response.status_code in {409, 503}, response.text
        assert not target.exists()
        assert state.authoring_workspace.snapshot() == before
        assert "submit" not in calls
    elif failure == "submit":
        assert response.status_code == 503, response.text
        assert response.json()["detail"]["code"] == "scenario_created_not_activated"
        assert response.json()["detail"]["path"] == str(target)
        assert target.is_dir()
        assert calls == ["prepare", "submit"]  # no retry or rollback
    else:
        assert response.status_code == 200, response.text
        assert response.json()["activation"] == {"status": "accepted", "apply_id": "new-apply"}
        assert admission.events == ["validate", "commit", "submit", "wake"]
        assert state.draft_session.workspace_snapshot()["saved"]["matchesDraft"] is True
        assert state.draft_session.workspace_snapshot()["saved"]["available"] is True
        saved = state.authoring_workspace.snapshot().as_dict()["program"]["saved"]
        assert saved["available"] and saved["matchesDraft"] and saved["programRevision"] is None
        # Save As of an empty editable Program must move its Saved baseline too.
        copy_path = tmp_path / "new-copy"
        result = _scenario_client(state).post("/api/scenario-documents/save", json={
            **_fence(state), "path": str(copy_path), "scenarioId": state.scenario_document_id,
            "name": "New cell", "description": "", "confirmOverwrite": False,
        })
        assert result.status_code == 200, result.text
        saved = state.authoring_workspace.snapshot().as_dict()["program"]["saved"]
        assert saved["available"] and saved["matchesDraft"]
        assert saved["path"] == str(copy_path / "program.yaml")


def test_new_keep_refuses_missing_applied_source_instead_of_using_draft(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    state = ComposerState(allowed_fs_roots=[str(tmp_path)])
    monkeypatch.setattr(state, "published_scenario", lambda: {"published_scenario": None})
    before = state.authoring_workspace.snapshot()
    response = _scenario_client(state).post("/api/scenario-documents/new", json={
        **_fence(state), "path": str(tmp_path / "new"), "name": "New",
        "deploymentChoice": "keep_applied", "confirmDiscard": True,
        "expectedRetainedBundleId": "bundle", "expectedGraphRevision": "graph",
    })
    assert response.status_code == 409, response.text
    assert response.json()["detail"]["code"] == "scenario_applied_source_unavailable"
    assert state.authoring_workspace.snapshot() == before
    assert not (tmp_path / "new").exists()


@pytest.mark.parametrize("name", [
    "New cell", "new\tcell", " new_cell", "new_cell ", "-leading", "_leading",
    "dotted.name", "slash/name", "",
])
def test_new_scenario_name_is_one_word(tmp_path: Path, name: str) -> None:
    state = ComposerState(allowed_fs_roots=[str(tmp_path)])
    before = state.authoring_workspace.snapshot()
    response = _scenario_client(state).post("/api/scenario-documents/new", json={
        **_fence(state), "path": str(tmp_path / "new"), "name": name,
        "deploymentChoice": "empty", "confirmDiscard": True,
        "expectedRetainedBundleId": "bundle", "expectedGraphRevision": "graph",
    })
    assert response.status_code == 422, response.text
    assert state.authoring_workspace.snapshot() == before
    assert not (tmp_path / "new").exists()


def test_capture_reads_one_source_identity(tmp_path: Path) -> None:
    target = tmp_path / "captured"
    original = build_scenario_document(
        scenario_id="captured_cell",
        name="Captured cell",
        description="",
        program_draft=None,
        deployment_draft=None,
        program_role_assignment=None,
        typed_parameter_overrides={},
        manyforge_version="1.0.0",
        last_known_status="incomplete",
    )
    replacement = build_scenario_document(
        scenario_id="replacement_cell",
        name="Replacement cell",
        description="",
        program_draft=None,
        deployment_draft=None,
        program_role_assignment=None,
        typed_parameter_overrides={},
        manyforge_version="1.0.0",
        last_known_status="incomplete",
    )
    save_scenario_artifact(
        original,
        str(target),
        allowed_roots=[str(tmp_path)],
        overwrite=False,
    )
    captured, source, digest = capture_scenario_artifact(
        str(target), allowed_roots=[str(tmp_path)]
    )
    save_scenario_artifact(
        replacement,
        str(target),
        allowed_roots=[str(tmp_path)],
        overwrite=True,
    )
    assert captured["scenario_id"] == "captured_cell"
    assert source == str(target.resolve())
    assert digest.startswith("sha256:")


def test_single_file_scenario_is_not_a_project(tmp_path: Path) -> None:
    source = tmp_path / "legacy.scenario.yaml"
    source.write_text(json.dumps(_document()), encoding="utf-8")

    with pytest.raises(ScenarioArtifactError) as direct:
        load_scenario_artifact(str(source), allowed_roots=[str(tmp_path)])
    assert direct.value.code == "scenario_artifact_path_invalid"


def test_valid_hand_edit_is_loaded_without_rechecking_during_discovery(tmp_path: Path) -> None:
    target = Path(
        save_scenario_artifact(
            _document(),
            str(tmp_path / "edited"),
            allowed_roots=[str(tmp_path)],
            overwrite=False,
        )
    )
    (target / "overrides.yaml").write_text('{"speed": 0.25}\n', encoding="utf-8")

    loaded, _ = load_scenario_artifact(
        str(target), allowed_roots=[str(tmp_path)]
    )
    assert loaded["typed_parameter_overrides"] == {"speed": 0.25}
    _, entries = discover_scenario_artifacts(
        str(tmp_path),
        allowed_roots=[str(tmp_path)],
        manyforge_version="1.0.0",
    )
    assert entries[0]["display_status"] == "ready"


def test_save_as_rebases_project_source_references_without_copying(
    tmp_path: Path,
) -> None:
    original = Path(
        save_scenario_artifact(
            _document(),
            str(tmp_path / "original"),
            allowed_roots=[str(tmp_path)],
            overwrite=False,
        )
    )
    (tmp_path / "nested").mkdir()
    copied = Path(
        save_scenario_artifact(
            _document(),
            str(tmp_path / "nested" / "copy"),
            allowed_roots=[str(tmp_path)],
            overwrite=False,
            source_project_path=str(original),
        )
    )
    loaded, _ = load_scenario_artifact(str(copied), allowed_roots=[str(tmp_path)])

    reference = loaded["deployment_draft"]["components"][0]["model_source"][
        "urdf"
    ]["project_path"]
    assert (copied / reference).resolve() == (tmp_path / "model.urdf").resolve()
    assert not (copied / "sources").exists()


def test_source_changes_are_advisory_and_export_is_self_contained(
    tmp_path: Path,
) -> None:
    original = Path(
        save_scenario_artifact(
            _document(),
            str(tmp_path / "original"),
            allowed_roots=[str(tmp_path)],
            overwrite=False,
        )
    )
    (tmp_path / "model.urdf").write_text(
        '<robot name="fixture_changed"/>\n', encoding="utf-8"
    )
    captured, _, _, diagnostics = capture_scenario_artifact_with_diagnostics(
        str(original), allowed_roots=[str(tmp_path)]
    )
    assert diagnostics == [
        {
            "code": "scenario_source_changed",
            "source_id": "component:fixture_robot:urdf",
            "message": "Referenced source content changed",
        }
    ]

    exported = Path(
        export_scenario_artifact(
            captured,
            str(tmp_path / "portable"),
            source_project_path=str(original),
            allowed_roots=[str(tmp_path)],
        )
    )
    (tmp_path / "model.urdf").unlink()
    portable, _ = load_scenario_artifact(
        str(exported), allowed_roots=[str(exported)]
    )
    reference = portable["deployment_draft"]["components"][0]["model_source"][
        "urdf"
    ]["project_path"]
    assert (exported / reference).read_text(encoding="utf-8") == (
        '<robot name="fixture_changed"/>\n'
    )


def test_dynamic_index_lists_only_project_documents(tmp_path: Path) -> None:
    save_scenario_artifact(
        _document(),
        str(tmp_path / "saved"),
        allowed_roots=[str(tmp_path)],
        overwrite=False,
    )

    class Headless:
        def scenarios(self) -> dict:
            return {
                "source_document_digest": "sha256:" + "1" * 64,
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
    assert [(row["source_kind"], row["scenario_id"]) for row in body["entries"]] == [
        ("saved", "cell_fixture"),
    ]
