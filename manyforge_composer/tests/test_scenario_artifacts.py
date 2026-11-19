"""Editable Scenario-project persistence and discovery contracts."""

from __future__ import annotations

import json
import os
import shutil
import uuid
from copy import deepcopy
from pathlib import Path
from typing import Any

import pytest
from fastapi import FastAPI
from fastapi.testclient import TestClient

from manyforge_composer.backend import routes_scenario_documents, scenario_artifacts
from manyforge_composer.backend.routes_scenario_documents import (
    _document_content_digest,
    create_scenario_document_router,
)
from manyforge_composer.backend.authoring_admission import (
    AuthoringAdmissionCoordinator,
    AuthoringMutationEnvelope,
    AuthoringOperationError,
    LocalAuthoringCommit,
)
from manyforge_composer.backend.authoring_forest import from_program, derive_candidate, empty_authoring
from manyforge_composer.backend.routes_draft import create_draft_router
from manyforge_composer.backend.routes_deployment_draft import create_deployment_draft_router
from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.program import Program
from manyforge_headless_api.client import HeadlessClientError
from manyforge_headless_api.models import ManagerApplyAcknowledgementContext
from manyforge_composer.backend.scenario_artifacts import (
    UNTITLED_SCENARIO_PATH,
    ScenarioArtifactError,
    build_scenario_document,
    capture_scenario_artifact,
    capture_scenario_artifact_with_diagnostics,
    discover_scenario_artifacts,
    export_scenario_artifact,
    load_scenario_artifact,
    prepare_scenario_artifact,
    save_scenario_artifact,
    scenario_name_valid,
    scenario_project_path,
    scenario_project_stem,
    validate_scenario_document,
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


def _document(name: str = "cell_fixture", *, version: str = "1.0.0") -> dict:
    """One Scenario named *name*: save it as the folder ``<name>.scenario``."""

    return build_scenario_document(
        scenario_id=name,
        name=name,
        description="Editable Scenario fixture",
        program_draft=deepcopy(_PROGRAM_FIXTURE),
        deployment_draft=_deployment_intent(),
        manyforge_version=version,
        last_known_status="ready",
    )


def _save_target(target: Path) -> dict[str, str]:
    """The Save request location of the project folder *target*."""

    return {"directory": str(target.parent), "name": scenario_project_stem(target)}


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
    state = ComposerState(application_start_id=0x3F9A0000000000A1, allowed_fs_roots=[str(tmp_path)])
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
    target = tmp_path / "saved_cell.scenario"
    before = state.authoring_workspace.identity()
    response = client.post("/api/scenario-documents/save", json={
        **_fence(state), **_save_target(target),
        "description": "", "confirmOverwrite": False,
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
        "deployment.yaml", "program.yaml", "scenario.yaml"
    ]

    # Save As changes the project location, not the referenced model. A later
    # Deployment-only Save must keep the same source after rebasing as well.
    (tmp_path / "nested").mkdir()
    copy = tmp_path / "nested" / "saved_copy.scenario"
    response = client.post("/api/scenario-documents/save", json={
        **_fence(state), **_save_target(copy),
        "description": "", "confirmOverwrite": False,
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


@pytest.mark.parametrize(
    ("program_unapplied", "deployment_applied", "deployment_matches", "expected"),
    [
        (False, 7, True, "ready"),
        (True, 7, True, "unknown"),
        (None, 7, True, "unknown"),
        (False, 7, False, "unknown"),
        (False, None, True, "unknown"),
    ],
)
def test_scenario_save_records_ready_only_for_the_applied_content(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    program_unapplied: bool | None,
    deployment_applied: int | None,
    deployment_matches: bool,
    expected: str,
) -> None:
    state = ComposerState(application_start_id=0x3F9A0000000000A1, allowed_fs_roots=[str(tmp_path)])
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
    # A role assignment makes the saved Scenario complete.
    state.authoring_workspace.remember_project(document=_document())
    # The Applied views at the current identity: Program Draft vs Applied and
    # Deployment Draft vs Manager Applied, decided by run admission.
    monkeypatch.setattr(routes_scenario_documents, "program_workspace", lambda s: {
        "hasUnappliedChanges": program_unapplied,
        "authoringWorkspace": s.authoring_workspace.identity().as_dict(),
    })
    monkeypatch.setattr(routes_scenario_documents, "deployment_workspace", lambda s: {
        "draft": {"document": s.authoring_workspace.snapshot().as_dict()["deployment"]["document"],
                  "deploymentRevision": None},
        "managerApplied": {"deploymentRevision": deployment_applied},
        "runAdmission": {"ready": deployment_matches},
        "authoringWorkspace": s.authoring_workspace.identity().as_dict(),
    })
    target = tmp_path / "saved_cell.scenario"
    response = _scenario_client(state).post("/api/scenario-documents/save", json={
        **_fence(state), **_save_target(target),
        "description": "", "confirmOverwrite": False,
    })
    assert response.status_code == 200, response.text
    assert response.json()["last_known_status"] == expected
    disk, _ = load_scenario_artifact(str(target), allowed_roots=[str(tmp_path)])
    assert disk["saved_status"]["last_known_status"] == expected


@pytest.fixture(autouse=True)
def _fixture_model_source(tmp_path: Path) -> None:
    (tmp_path / "model.urdf").write_text(
        '<robot name="fixture"/>\n', encoding="utf-8"
    )


class _PassThroughAdmission:
    def __init__(self) -> None:
        self.events: list[str] = []

    def settle_before_preparation(self) -> None:
        """No local claim is pending in these route-unit tests."""

    def manager_submission_sent(self, _claim) -> None:
        """The parent claim's hand-over is the real coordinator's concern."""

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
        validate_under_claim=None,
        prepare_project_under_claim=None,
        **_kwargs,
    ):
        """Commit through the real composite owner; only the Kernel is stubbed."""

        if validate_under_claim is not None:
            validate_under_claim()
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


def _preparation_reply(request: dict) -> dict:
    """Headless's answer to an activation preparation that already ended
    ``succeeded`` (OD-214); its result is read by digest."""

    return {
        "schema_version": "manyforge.activation_preparation_reply.v1",
        "command": {
            "kind": "activation_preparation", "request_key": request["request_key"],
            "captured_input_digest": request["captured_input_digest"],
            "step": "preparing", "accepted_at_ns": "1",
            "end": {"outcome": "succeeded", "code": None, "details": None,
                    "finished_at_ns": "2"},
        },
    }


class _CommandClient(TestClient):
    """A TestClient that sends Load and New as Apply-level commands (OD-214)."""

    def __init__(self, app: FastAPI, state: ComposerState) -> None:
        super().__init__(app)
        self.state = state

    def command(self, path: str, body: dict) -> tuple[Any, dict | None]:
        """Post with a fresh request key; the row once it ended (an accepted
        command), else ``None`` (refused at once)."""

        key = uuid.uuid4().hex
        response = self.post(path, json={**body, "request_key": key})
        if response.status_code != 200:
            return response, None
        assert response.json()["schema_version"] == "manyforge.apply_command_reply.v1"
        assert response.json()["command"]["request_key"] == key
        row = self.state.apply_commands.ended_row(key)
        assert row is not None and row["end"] is not None
        return response, row


def _scenario_client(state: ComposerState) -> _CommandClient:
    app = FastAPI()
    app.include_router(create_scenario_document_router(state))
    return _CommandClient(app, state)


class _ManagerStub:
    """The Headless reads of an orchestrated Load: an idle
    Program cycle, an empty cell, admission generation 1, and a Manager Apply
    that succeeds."""

    def manager_status(self) -> dict:
        return {"status": {"active": {}}}

    def program_cycle_status(self) -> dict:
        return {"state": "idle_after_reset"}

    def execution_control_state(self) -> dict:
        return {"control_state": {"manual_operations": []}}

    def components(self) -> list:
        return []

    def authoring_admission(self) -> dict:
        return {"authoring_allowed": True, "admission_generation": 1}

    def manager_apply_status(self, _apply_id, _generation, *, application_start_id) -> dict:
        return {"ok": True, "state": "succeeded"}

    def manager_apply_release_for_new(self, context) -> dict:
        # The real client validates the context strictly before sending.
        ManagerApplyAcknowledgementContext.model_validate(context)
        return {"ok": True, "released": None}

    def program_applied(self) -> dict:
        raise HeadlessClientError(
            "program_applied_selection_not_found", "no Program is Applied", status_code=404
        )


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
        "applicationStartId": "3f9a0000000000a1",
        "requestKey": "5ce0" * 8,
        "expectedWorkspaceEpoch": identity.workspace_epoch,
        "expectedWorkspaceRevision": identity.workspace_revision,
    }


class _DraftEditAdmission:
    """Commits a Draft-only edit through the real workspace owner."""

    def __init__(self) -> None:
        self.sessions: list[str] = []

    def execute_draft_edit(
        self, _envelope, *, workspace, session, draft_id, expected_identity,
        install, validate=None, **_kwargs,
    ):
        self.sessions.append(session)
        if validate is not None:
            validate()
        assert session == "scenario"
        return workspace.replace(expected_identity=expected_identity, install=install)


@pytest.mark.parametrize(
    ("program_pending", "deployment_pending", "expected"),
    [
        (True, True, ["deployment", "program"]),
        (True, False, ["program"]),
        (False, True, ["deployment"]),
        (False, False, []),
    ],
)
def test_scenario_discard_commits_every_pending_draft_in_one_scenario_commit(
    monkeypatch: pytest.MonkeyPatch,
    program_pending: bool,
    deployment_pending: bool,
    expected: list[str],
) -> None:
    state = ComposerState(application_start_id=0x3F9A0000000000A1)
    admission = _DraftEditAdmission()
    state.authoring_admission = admission  # type: ignore[assignment]
    installed: list[str] = []
    validated: list[str] = []
    prepared: list[tuple[str, str, int]] = []
    monkeypatch.setattr(routes_scenario_documents, "program_workspace",
                        lambda _s: {"hasUnappliedChanges": program_pending})
    monkeypatch.setattr(routes_scenario_documents, "_deployment_unapplied",
                        lambda _view: deployment_pending)
    monkeypatch.setattr(routes_scenario_documents, "deployment_workspace", lambda _s: {})

    def prepare(document: str):
        def run(_state, draft_id, revision):
            prepared.append((document, draft_id, revision))
            return (lambda: validated.append(document)), (lambda: installed.append(document) or True)
        return run

    monkeypatch.setattr(routes_scenario_documents, "prepare_deployment_discard", prepare("deployment"))
    monkeypatch.setattr(routes_scenario_documents, "prepare_program_discard", prepare("program"))
    fence = _fence(state)
    before = state.authoring_workspace.identity().workspace_revision
    response = _scenario_client(state).post("/api/scenario-documents/discard", json=fence)

    if not expected:
        assert response.status_code == 409, response.text
        assert response.json()["detail"]["code"] == "scenario_discard_nothing_to_discard"
        assert admission.sessions == []
        return
    assert response.status_code == 200, response.text
    # One whole-Scenario commit: every pending Draft, fenced on the client's
    # own Draft identities, validated then installed together.
    assert admission.sessions == ["scenario"]
    assert [document for document, *_ in prepared] == expected
    for document, draft_id, revision in prepared:
        prefix = "deployment" if document == "deployment" else ""
        assert draft_id == fence["deploymentDraftId" if prefix else "draftId"]
        assert revision == fence[
            "expectedDeploymentDraftRevision" if prefix else "expectedDraftRevision"
        ]
    assert validated == expected and installed == expected
    assert state.authoring_workspace.identity().workspace_revision == before + 1


def test_scenario_discard_refuses_a_stale_workspace_before_any_discard(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    state = ComposerState(application_start_id=0x3F9A0000000000A1)
    state.authoring_admission = _DraftEditAdmission()  # type: ignore[assignment]
    monkeypatch.setattr(routes_scenario_documents, "prepare_program_discard",
                        lambda *_args: pytest.fail("a stale request must not prepare a discard"))
    fence = {**_fence(state), "expectedWorkspaceRevision": _fence(state)["expectedWorkspaceRevision"] + 1}
    response = _scenario_client(state).post("/api/scenario-documents/discard", json=fence)

    assert response.status_code == 409, response.text
    assert response.json()["detail"]["code"] == "composer_workspace_stale"


@pytest.mark.parametrize(
    "stale_field", ["expectedDeploymentDraftRevision", "expectedDraftRevision"]
)
def test_scenario_discard_refuses_a_stale_draft_fence_without_containing(
    monkeypatch: pytest.MonkeyPatch, stale_field: str
) -> None:
    state = ComposerState(application_start_id=0x3F9A0000000000A1)
    admission = _DraftEditAdmission()
    state.authoring_admission = admission  # type: ignore[assignment]
    monkeypatch.setattr(routes_scenario_documents, "program_workspace",
                        lambda _s: {"hasUnappliedChanges": True})
    monkeypatch.setattr(routes_scenario_documents, "_deployment_unapplied",
                        lambda _view: True)
    monkeypatch.setattr(routes_scenario_documents, "deployment_workspace", lambda _s: {})

    # Like the real installs, each fences its own Draft identity.
    def prepare_deployment(_state, draft_id, revision):
        def install() -> bool:
            state.deployment_draft_session.require_current(draft_id, revision)
            return True
        return (lambda: None), install

    def prepare_program(_state, draft_id, revision):
        def install() -> bool:
            state.draft_session.require_identity(draft_id, revision)
            return True
        return (lambda: None), install

    monkeypatch.setattr(routes_scenario_documents, "prepare_deployment_discard",
                        prepare_deployment)
    monkeypatch.setattr(routes_scenario_documents, "prepare_program_discard", prepare_program)
    program_before = state.draft_session.retained_state()
    deployment_before = state.deployment_draft_session.retained_state()
    # The workspace revision is current; only one Draft fence is stale.
    fence = _fence(state)
    response = _scenario_client(state).post(
        "/api/scenario-documents/discard", json={**fence, stale_field: fence[stale_field] + 1}
    )

    assert response.status_code == 409, response.text
    assert response.json()["detail"]["code"] == "scenario_workspace_stale"
    assert admission.sessions == []
    assert state.draft_session.retained_state() == program_before
    assert state.deployment_draft_session.retained_state() == deployment_before
    assert state.authoring_workspace.failed_closed is None
    # The workspace stays usable: the current fence commits.
    revision = state.authoring_workspace.identity().workspace_revision
    retry = _scenario_client(state).post("/api/scenario-documents/discard", json=fence)
    assert retry.status_code == 200, retry.text
    assert state.authoring_workspace.identity().workspace_revision == revision + 1


def test_save_load_and_discovery_preserve_one_editable_project(
    tmp_path: Path,
) -> None:
    saved_path = save_scenario_artifact(
        _document("cell"),
        str(tmp_path / "cell.scenario"),
        allowed_roots=[str(tmp_path)],
        overwrite=False,
    )

    assert saved_path == str((tmp_path / "cell.scenario").resolve())
    assert sorted(path.name for path in Path(saved_path).iterdir()) == [
        "deployment.yaml",
        "program.yaml",
        "scenario.yaml",
    ]
    loaded, source = load_scenario_artifact(
        saved_path,
        allowed_roots=[str(tmp_path)],
    )
    assert source == saved_path
    assert loaded == _document("cell")
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
            "scenario_id": "cell",
            "name": "cell",
            "folder_name": "cell",
            "location": ".",
            "description": "Editable Scenario fixture",
            "saved_manyforge_version": "1.0.0",
            "display_status": "ready",
            "status_detail": "Previously recorded as load-ready. Current files and model sources are validated when you load this Scenario.",
        }
    ]


def test_listing_reports_history_not_current_content_validation(tmp_path: Path) -> None:
    target = Path(
        save_scenario_artifact(
            _document("cell"),
            str(tmp_path / "cell.scenario"),
            allowed_roots=[str(tmp_path)],
            overwrite=False,
        )
    )
    edited = deepcopy(_document("cell")["program_draft"])
    edited["description"] = "Edited outside"
    (target / "program.yaml").write_text(json.dumps(edited), encoding="utf-8")

    _, changed = discover_scenario_artifacts(
        str(tmp_path),
        allowed_roots=[str(tmp_path)],
        manyforge_version="1.0.0",
    )
    assert changed[0]["display_status"] == "ready"
    assert "Previously recorded" in changed[0]["status_detail"]

    save_scenario_artifact(
        _document("cell"),
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
        _document("cell"), str(tmp_path / "cell.scenario"),
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
        _document("valid"),
        str(tmp_path / "valid.scenario"),
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

    assert [entry["name"] for entry in entries] == ["broken", "valid"]
    assert entries[0]["display_status"] == "unsupported"
    assert entries[1]["display_status"] == "ready"


def test_save_requires_explicit_overwrite_and_stays_inside_roots(
    tmp_path: Path,
) -> None:
    target = tmp_path / "cell.scenario"
    save_scenario_artifact(
        _document("cell"),
        str(target),
        allowed_roots=[str(tmp_path)],
        overwrite=False,
    )
    with pytest.raises(ScenarioArtifactError) as existing:
        save_scenario_artifact(
            _document("cell"),
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
    target = tmp_path / "cell.scenario"
    save_scenario_artifact(
        _document("cell"),
        str(target),
        allowed_roots=[str(tmp_path)],
        overwrite=False,
    )
    previous_manifest = (target / "scenario.yaml").read_bytes()
    replacement = _document("cell")
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

    assert interrupted.value.code == "scenario_artifact_save_incomplete"
    assert interrupted.value.status_code == 500
    assert (target / "scenario.yaml").read_bytes() == previous_manifest
    assert (target / ".manyforge-save-pending.json").is_file()
    assert list(target.glob(".*.tmp-*")) == []
    with pytest.raises(ScenarioArtifactError) as reopened:
        load_scenario_artifact(str(target), allowed_roots=[str(tmp_path)])
    assert reopened.value.code == "scenario_project_save_interrupted"


def test_routes_reject_loading_incomplete_scenario_without_workspace_mutation(
    tmp_path: Path,
) -> None:
    state = ComposerState(application_start_id=0x3F9A0000000000A1, allowed_fs_roots=[str(tmp_path)])
    state.authoring_admission = _PassThroughAdmission()  # type: ignore[assignment]
    client = _scenario_client(state)
    target = tmp_path / "draft_cell.scenario"

    saved = client.post(
        "/api/scenario-documents/save",
        json={
            **_fence(state),
            **_save_target(target),
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
    rejected, _rejected_row = client.command("/api/scenario/load", {"path": str(target)})
    assert rejected.status_code == 409
    assert rejected.json()["detail"]["code"] == "scenario_activation_incomplete"
    assert rejected.json()["detail"]["effect"] == "none"
    # The refusal names the missing member under ``details`` (OD-260); both
    # members are empty here and the Program's is named first.
    assert rejected.json()["detail"]["details"] == {
        "member": "program.yaml", "diagnostic_count": 0,
    }
    assert "context" not in rejected.json()["detail"]
    assert state.authoring_workspace.snapshot().identity == before_load


def test_load_of_a_program_that_cannot_be_built_names_its_first_diagnostic(
    tmp_path: Path,
) -> None:
    state = ComposerState(application_start_id=0x3F9A0000000000A1, allowed_fs_roots=[str(tmp_path)])
    state.authoring_admission = _PassThroughAdmission()  # type: ignore[assignment]
    client = _scenario_client(state)
    # The root's children become the top-level nodes: the Program has more
    # than one root and cannot be built.
    authoring = deepcopy(_PROGRAM_FIXTURE)
    root = authoring["forest"][0]
    assert len(root["children"]) > 1
    authoring["forest"] = root["children"]
    authoring["positions"] = {
        key: value for key, value in authoring["positions"].items()
        if key != root["instance_id"]
    }
    document = _document("cell")
    document["program_draft"] = authoring
    target = Path(save_scenario_artifact(
        document, str(tmp_path / "cell.scenario"),
        allowed_roots=[str(tmp_path)], overwrite=False,
    ))
    expected = derive_candidate(authoring, state.node_catalog)[1]
    assert expected[0]["code"] == "authoring_forest_root_count"

    before_load = state.authoring_workspace.snapshot().identity
    rejected, _rejected_row = client.command("/api/scenario/load", {"path": str(target)})
    assert rejected.status_code == 409
    detail = rejected.json()["detail"]
    assert detail["code"] == "scenario_activation_incomplete"
    assert detail["effect"] == "none"
    assert detail["details"] == {
        "member": "program.yaml",
        "cause_code": "authoring_forest_root_count",
        "path": "forest",
        "diagnostic_count": len(expected),
    }
    assert "context" not in detail
    assert state.authoring_workspace.snapshot().identity == before_load

    # A complete Program with an empty Deployment names deployment.yaml.
    no_deployment = _document("other")
    no_deployment["deployment_draft"] = None
    other = save_scenario_artifact(
        no_deployment, str(tmp_path / "other.scenario"),
        allowed_roots=[str(tmp_path)], overwrite=False,
    )
    rejected, _rejected_row = client.command("/api/scenario/load", {"path": other})
    assert rejected.status_code == 409
    assert rejected.json()["detail"]["details"] == {
        "member": "deployment.yaml", "diagnostic_count": 0,
    }


def test_save_keeps_written_file_but_refuses_a_stale_saved_baseline(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    state = ComposerState(application_start_id=0x3F9A0000000000A1, allowed_fs_roots=[str(tmp_path)])
    state.authoring_admission = _PassThroughAdmission()  # type: ignore[assignment]
    client = _scenario_client(state)
    target = tmp_path / "raced-save.scenario"
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
            **_save_target(target),
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
            _document("loaded-project"),
            str(tmp_path / "loaded-project.scenario"),
            allowed_roots=[str(tmp_path)],
            overwrite=False,
        )
    )
    state = ComposerState(application_start_id=0x3F9A0000000000A1, allowed_fs_roots=[str(tmp_path)])
    admission = _PassThroughAdmission()
    state.authoring_admission = admission  # type: ignore[assignment]
    state.headless_client = _ManagerStub()  # type: ignore[assignment]
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
    observed_generations: list[tuple[int, int | None]] = []
    before = state.authoring_workspace.snapshot()
    def prepare_or_apply(
        intent,
        claim,
        *,
        apply,
        prepared=None,
        prepare_only=False,
        confirmed=False,
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
            "schema_version": "manyforge.prepared_activation_input.v7",
            "prepared_activation_id": "prepared-load-test",
            "origin": {
                "kind": "scenario",
                "scenario_id": intent["scenario_id"],
                "source_document_digest": intent["source_document_digest"],
                "project_path": intent["project_path"],
            },
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
        }
        if prepare_only:
            # Load is the operator's confirmation: one preparation, no token.
            assert confirmed is True
            assert intent["confirmation_token"] is None
            assert intent["scene_initialization"] == "initialize"
            return {
                "prepared_activation": candidate,
                "candidate_preview": dict(preview),
                "manager_apply": None,
                "confirmation_required": False,
                "confirmation_token": None,
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

    monkeypatch.setattr(state, "_manager_current", lambda _status, **_kw: (
        "current-bundle", "current-graph"
    ))
    monkeypatch.setattr(
        state, "execute_authored_deployment_candidate", prepare_or_apply
    )
    monkeypatch.setattr(
        routes_scenario_documents,
        "program_workspace",
        lambda _state: {"authoringWorkspace": {"workspaceEpoch": "test"}},
    )
    refused_startup = {"path": "/p/other.scenario", "code": "scenario_load_failed", "message": "m"}
    state.startup_scenario_notice = refused_startup
    response, response_row = client.command("/api/scenario/load", {"path": str(target)})

    if preparation_fails:
        # The preparation runs after the Load was accepted (OD-214): its
        # definite refusal ends the row ``refused``, with no effect.
        assert response.status_code == 200, response.text
        assert response_row["end"] == {
            "outcome": "refused",
            "code": "activation_preparation_failed",
            "details": {"preparer_detail": "invalid candidate geometry"},
            "finished_at_ns": response_row["end"]["finished_at_ns"],
        }
        assert admission.events == []
        assert state.authoring_workspace.snapshot() == before
        assert state.startup_scenario_notice == refused_startup  # Nothing was opened.
        return

    assert response.status_code == 200, response.text
    assert response_row["end"]["outcome"] == "succeeded"
    # A succeeded Apply-level row keeps the ApplyResult's component rows
    # (OD-214 contract §3.1); this Scenario has no components.
    assert response_row["end"]["details"] == {"components": []}
    assert response_row["kind"] == "scenario_load"
    # Load = Close, then Apply of the file: two orchestrated Applies.
    assert admission.events == ["validate", "commit", "submit", "wake"] * 2
    # Each: Preview, then the Apply's own preparation and its submission.
    assert observed_generations == [(1, None), (1, None), (1, 2)] * 2
    assert state.scenario_document_id == "loaded-project"
    assert state.startup_scenario_notice is None  # An opened Scenario clears it.
    loaded = state.authoring_workspace.snapshot().as_dict()
    # The reply is the row (OD-214); the committed snapshot is read from the
    # workspace, as after every other authoring change.
    assert set(response.json()) == {"schema_version", "command"}
    assert loaded["schemaVersion"] == (
        "manyforge.composer_authoring_workspace_snapshot.v3"
    )
    assert loaded["program"]["saved"]["matchesDraft"] is True
    assert loaded["program"]["saved"]["path"] == str(target / "program.yaml")
    assert loaded["deployment"]["differsFromSaved"] is False
    assert loaded["deployment"]["savedPath"] == str(target / "deployment.yaml")


@pytest.mark.parametrize(
    ("failure_code", "settled"),
    [
        ("manager_apply_rejected", True),
        ("manager_apply_submission_uncertain", False),
        ("manager_apply_acceptance_invalid", False),
    ],
)
def test_scenario_load_settles_only_proven_preacceptance_rejection(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    failure_code: str,
    settled: bool,
) -> None:
    target = Path(
        save_scenario_artifact(
            _document("loaded-project"),
            str(tmp_path / "loaded-project.scenario"),
            allowed_roots=[str(tmp_path)],
            overwrite=False,
        )
    )
    state = ComposerState(application_start_id=0x3F9A0000000000A1, allowed_fs_roots=[str(tmp_path)])
    claim_owner = _SettlingHeadless()
    coordinator = AuthoringAdmissionCoordinator(claim_owner, 0x3F9A0000000000A1)  # type: ignore[arg-type]
    state.authoring_admission = coordinator  # type: ignore[assignment]
    state.headless_client = _ManagerStub()  # type: ignore[assignment]
    client = _scenario_client(state)
    before = state.authoring_workspace.snapshot()

    candidate = {
        "schema_version": "manyforge.prepared_activation_input.v7",
        "prepared_activation_id": "prepared-load-test",
        "origin": {
            "kind": "scenario",
            "scenario_id": "loaded-project",
            "source_document_digest": "sha256:" + "1" * 64,
            "project_path": str(target),
        },
        "captured_input_digest": "sha256:" + "2" * 64,
        "deployment_artifact_id": "fixture_deployment",
        "deployment_revision": "deployment-revision",
        "retained_bundle_id": "bundle-load-test",
        "graph_revision": "graph-load-test",
        "program_id": "program-load-test",
        "program_revision": "program-revision",
        "program_outcome": {
            "kind": "bound",
            "program_binding_id": "binding-load-test",
        },
        "node_environment_digest": "sha256:" + "a" * 64,
        "assistant_mode": "forbidden",
    }

    def prepare_or_reject(
        _intent,
        _claim,
        *,
        apply,
        prepared=None,
        prepare_only=False,
        confirmed=False,
    ):
        assert apply is True
        if prepare_only:
            assert confirmed is True
            return {
                "prepared_activation": candidate,
                "candidate_preview": {"diff": {}},
                "manager_apply": {"request_key": "1" * 32},
                "confirmation_required": False,
                "confirmation_token": None,
                "preparation": None,
                "phases_ms": {},
            }
        assert prepared is not None
        # Each code is the Manager request's own outcome: it was sent.
        coordinator.manager_submission_sent(_claim)
        raise AuthoringOperationError(
            failure_code,
            (
                "Manager rejected the exact Deployment candidate: "
                "apply_rejected: "
                "named Isaac world is unavailable"
            ),
        )

    monkeypatch.setattr(
        state,
        "_manager_current",
        lambda _status, **_kw: ("current-bundle", "current-graph"),
    )
    monkeypatch.setattr(
        state, "execute_authored_deployment_candidate", prepare_or_reject
    )
    response, response_row = client.command("/api/scenario/load", {"path": str(target)})

    assert state.authoring_workspace.snapshot().identity != before.identity
    # A failed Manager step ends the Load with its typed error (no retry);
    # here it is the Close's, so the file's Apply never starts.
    assert response.status_code == 200, response.text
    assert response_row["end"]["outcome"] == "failed"
    assert response_row["end"]["code"] == failure_code
    if settled:
        # The row carries the code only; its text comes from the catalogue.
        assert "message" not in response_row["end"]
        assert claim_owner.settlements == ["owner_commit"]
        assert coordinator.local_claim_pending is False
        assert coordinator.mutation_ready is True
        # The server names the Load's request itself: one settled outcome.
        [outcome] = coordinator._outcomes.values()
        assert outcome["kind"] == "settled"
    else:
        # Sent with an uncertain outcome: the Manager owns the parent claim.
        # Composer neither releases it nor blocks authoring (OD-196).
        assert claim_owner.settlements == []
        assert coordinator.local_claim_pending is False
        assert coordinator.mutation_ready is True


def test_new_after_a_failed_apply_with_a_lost_answer_works_end_to_end(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """OD-196, study section C: an Apply was sent, its answer lost, and it
    failed. New's Manager release frees its parent claim (the Manager is the
    only releaser of it); Composer holds no record of that claim, so New, then
    a Draft edit and a Load, work."""

    from manyforge_composer.tests.test_authoring_local_mutation import _Cell

    class Cell(_Cell, _ManagerStub):
        """Kernel and Manager double plus the stub's other Headless reads."""

    target = Path(
        save_scenario_artifact(
            _document("loaded-project"),
            str(tmp_path / "loaded-project.scenario"),
            allowed_roots=[str(tmp_path)],
            overwrite=False,
        )
    )
    state = ComposerState(application_start_id=0x3F9A0000000000A1, allowed_fs_roots=[str(tmp_path)])
    cell = Cell()
    coordinator = AuthoringAdmissionCoordinator(cell, 0x3F9A0000000000A1)  # type: ignore[arg-type]
    state.authoring_admission = coordinator  # type: ignore[assignment]
    state.headless_client = cell  # type: ignore[assignment]
    monkeypatch.setattr(state, "_manager_current", lambda _status, **_kw: ("bundle", "graph"))
    monkeypatch.setattr(routes_scenario_documents, "program_workspace",
                        lambda _state: {"authoringWorkspace": {"workspaceEpoch": "test"}})

    def submit_through_manager(intent_key: str, claim: dict) -> dict:
        accepted = state.apply_manager_deployment_candidate(
            {"manager_apply": {"request_key": intent_key}}, claim
        )
        return {
            "status": "accepted",
            "apply_id": accepted["apply_id"],
            "apply_generation": accepted["apply_generation"],
        }

    # The earlier Apply: sent, accepted, answer lost, then failed.
    cell.manager_fault = "lost_after_accept"
    with pytest.raises(AuthoringOperationError):
        coordinator.execute_external_submission(
            AuthoringMutationEnvelope(
                expected_admission_generation=cell.generation,
                application_start_id=0x3F9A0000000000A1,
                request_key=bytes.fromhex("0a" * 16),
            ),
            operation_kind="apply_program",
            draft_id=state.draft_session.draft_id,
            draft_revision=state.draft_session.revision,
            payload={},
            validate_under_claim=lambda _claim: None,
            submit=lambda claim: submit_through_manager("1" * 32, claim),
        )
    cell.end_apply(1, "failed")
    assert list(cell.held) == ["0a" * 16]
    assert coordinator.local_claim_pending is False
    cell.calls.clear()

    def prepare_or_apply(
        intent, claim, *, apply, prepared=None, prepare_only=False, confirmed=False
    ):
        if prepare_only:
            # The real preparation first settles a lost answer's claim
            # (state._prepare_authored_deployment_candidate), then Headless
            # waits for open admission.
            state.authoring_admission.settle_before_preparation()
            cell.calls.append(("prepare", not cell.held))
            return {"confirmation_required": False, "prepared_activation": {},
                    "candidate_preview": {"impact": {"components": []}}}
        return submit_through_manager(intent["request_key"], claim)

    monkeypatch.setattr(state, "execute_authored_deployment_candidate", prepare_or_apply)
    client = _scenario_client(state)

    response, response_row = client.command("/api/scenario/new", {"discard_unsaved": True})

    assert response.status_code == 200, response.text
    assert response_row["end"]["outcome"] == "succeeded"
    claim_and_manager = [
        call for call in cell.calls
        if call[0] not in {"authoring_admission", "manager_apply_status"}
    ]
    # The Manager releases the failed Apply's parent (claim 1); Composer
    # never touches it. New's own claim 2 is the Manager's once sent, and the
    # Manager releases it when New's Apply succeeds.
    assert claim_and_manager[:5] == [
        ("release_for_new",),
        ("manager_release", 1),
        ("prepare", True),
        ("prepare", True),
        ("acquire", claim_and_manager[4][1]),
    ]
    assert claim_and_manager[5:] == [("manager_apply", 2), ("manager_release", 2)]
    assert not any(call[0] == "settle" for call in cell.calls)
    assert cell.held == {}
    assert coordinator.local_claim_pending is False
    assert coordinator.mutation_ready is True

    # A Draft edit works.
    coordinator.execute_draft_edit(
        AuthoringMutationEnvelope(
            expected_admission_generation=cell.generation,
            application_start_id=0x3F9A0000000000A1,
            request_key=bytes.fromhex("0b" * 16),
        ),
        operation_kind="discard_scenario",
        workspace=state.authoring_workspace,
        draft_id=state.draft_session.draft_id,
        expected_identity=state.authoring_workspace.identity(),
        install=lambda: False,
        session="scenario",
    )

    # A Load works: its New has nothing left to release, then the file applies.
    cell.calls.clear()
    loaded, loaded_row = client.command("/api/scenario/load", {"path": str(target)})
    assert loaded.status_code == 200, loaded.text
    assert loaded_row["end"]["outcome"] == "succeeded"
    assert [call[0] for call in cell.calls if call[0] in {
        "release_for_new", "acquire", "settle", "manager_apply", "manager_release",
    }] == [
        "release_for_new",
        "acquire", "manager_apply", "manager_release",  # the New inside Load
        "acquire", "manager_apply", "manager_release",  # the file's Apply
    ]
    assert cell.held == {}
    assert coordinator.local_claim_pending is False


def test_scenario_load_partial_contained_install_keeps_the_claim(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    target = Path(
        save_scenario_artifact(
            _document("loaded-project"),
            str(tmp_path / "loaded-project.scenario"),
            allowed_roots=[str(tmp_path)],
            overwrite=False,
        )
    )
    state = ComposerState(application_start_id=0x3F9A0000000000A1, allowed_fs_roots=[str(tmp_path)])
    headless = _SettlingHeadless()
    coordinator = AuthoringAdmissionCoordinator(headless, 0x3F9A0000000000A1)  # type: ignore[arg-type]
    state.authoring_admission = coordinator  # type: ignore[assignment]
    state.headless_client = _ManagerStub()  # type: ignore[assignment]
    client = _scenario_client(state)
    submitted: list[bool] = []

    def prepare_or_apply(intent, claim, *, apply, prepared=None, prepare_only=False, confirmed=False):
        if not prepare_only:
            submitted.append(True)
        return {
            "prepared_activation": {"prepared_activation_id": "prepared-load-test"},
            "candidate_preview": {"diff": {}},
            "manager_apply": None,
            "confirmation_required": False,
            "confirmation_token": None,
            "preparation": None,
            "phases_ms": {},
        }

    def broken_install(*_args, **_kwargs):
        raise RuntimeError("Deployment install failed")

    monkeypatch.setattr(state, "_manager_current", lambda _status, **_kw: (
        "current-bundle", "current-graph"
    ))
    monkeypatch.setattr(state, "execute_authored_deployment_candidate", prepare_or_apply)
    # The Program installs first; the Deployment install then fails.
    monkeypatch.setattr(
        state.deployment_draft_session, "install_prepared_replacement", broken_install
    )
    response, response_row = client.command("/api/scenario/load", {"path": str(target)})

    # The contained workspace refuses every later read; the Load's row
    # ends ``failed`` with the containment's code.
    assert response.status_code == 200, response.text
    assert response_row["end"]["outcome"] == "failed"
    assert response_row["end"]["code"] == "composer_workspace_failed_closed"
    assert state.authoring_workspace.failed_closed is not None
    assert submitted == []
    # Never settled as proven no effect: the claim is retained, admission blocked.
    assert headless.settlements == []
    assert coordinator.local_claim_pending is True
    assert coordinator.mutation_ready is False


_UNTITLED_DEPLOYMENT = {
    "api_version": "manyforge.deployment_intent.v2",
    "deployment_artifact_id": "empty_scenario", "name": "Empty Deployment",
    "components": [], "attachments": [], "policies": {},
}


def test_untitled_scenario_resource_is_the_canonical_empty_project(tmp_path: Path) -> None:
    """The packaged Untitled files are exactly Save's encoding of the empty Scenario.

    After an intentional change (for example a new empty Program format),
    rewrite the four files from ``members`` and ``manifest`` below.
    """

    authoring = empty_authoring(build_node_environment().catalog)
    authoring.update(program_id="program-untitled", name="Untitled")
    document = build_scenario_document(
        scenario_id="Untitled", name="Untitled", description="",
        program_draft=authoring, deployment_draft=deepcopy(_UNTITLED_DEPLOYMENT),
        manyforge_version="0.3.1", last_known_status="incomplete",
    )
    members, manifest = scenario_artifacts._project_payload(document, source_snapshot={
        "schema_version": scenario_artifacts.SCENARIO_SOURCE_SNAPSHOT_VERSION, "entries": [],
    })
    assert {path.name: path.read_bytes() for path in UNTITLED_SCENARIO_PATH.iterdir()} == {
        **members, scenario_artifacts.SCENARIO_MANIFEST: manifest,
    }
    # Readable outside every allowed root, unchanged, and refused as a Save target.
    captured, source, _digest, changed, diagnostics = scenario_artifacts._capture_project(
        str(UNTITLED_SCENARIO_PATH), allowed_roots=[str(tmp_path)]
    )
    assert (captured, source, changed, diagnostics) == (document, str(UNTITLED_SCENARIO_PATH), False, [])
    with pytest.raises(ScenarioArtifactError) as refused:
        prepare_scenario_artifact(
            document, str(UNTITLED_SCENARIO_PATH), allowed_roots=[str(UNTITLED_SCENARIO_PATH.parent)]
        )
    assert refused.value.code == "scenario_project_read_only"


# New is the orchestrated Apply of the built-in Untitled Scenario (the empty
# cell): no name or destination, Save disabled, Save As places it in a user folder.
def test_new_loads_the_read_only_untitled_scenario_and_save_as_places_it(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    untitled_files = {path.name: path.read_bytes() for path in UNTITLED_SCENARIO_PATH.iterdir()}
    # A repository-wide root may also contain the package: it is still never written.
    state = ComposerState(application_start_id=0x3F9A0000000000A1, allowed_fs_roots=[str(tmp_path), str(UNTITLED_SCENARIO_PATH.parent)])
    state.authoring_admission = _PassThroughAdmission()  # type: ignore[assignment]
    state.headless_client = _ManagerStub()  # type: ignore[assignment]
    monkeypatch.setattr(state, "_manager_current", lambda _status, **_kw: ("bundle", "graph"))
    monkeypatch.setattr(routes_scenario_documents, "program_workspace",
                        lambda _state: {"authoringWorkspace": {"workspaceEpoch": "test"}})
    intents: list[dict] = []

    def prepare_or_apply(
        intent, claim, *, apply, prepared=None, prepare_only=False, confirmed=False
    ):
        intents.append(intent)
        if prepare_only:
            assert confirmed is True
            return {"confirmation_required": False, "prepared_activation": {},
                    "candidate_preview": {"impact": {"components": []}}}
        return {"status": "accepted", "apply_id": 1, "apply_generation": 1}

    monkeypatch.setattr(state, "execute_authored_deployment_candidate", prepare_or_apply)
    state.startup_scenario_notice = {"path": "/p/x.scenario", "code": "scenario_load_failed", "message": "m"}
    before = state.authoring_workspace.snapshot()
    client = _scenario_client(state)
    response, response_row = client.command("/api/scenario/new", {"discard_unsaved": True})
    assert response.status_code == 200, response.text
    assert response_row["end"]["outcome"] == "succeeded"
    assert intents[-1]["scene_initialization"] == "initialize"
    assert response_row["kind"] == "scenario_new"
    assert state.scenario_document_id == "Untitled"
    intent = intents[-1]
    assert intent["scenario_id"] == "Untitled"
    assert intent["project_path"] == str(UNTITLED_SCENARIO_PATH)
    assert intent["program"] is None and intent["program_authoring"]["forest"] == []
    assert intent["deployment"] == _UNTITLED_DEPLOYMENT
    project = state.authoring_workspace.snapshot().as_dict()["project"]
    assert (project["document_path"], project["read_only"]) == (str(UNTITLED_SCENARIO_PATH), True)
    assert state.startup_scenario_notice is None

    in_place = client.post("/api/scenario-documents/save", json={
        **_fence(state), **_save_target(UNTITLED_SCENARIO_PATH), "description": "", "confirmOverwrite": True,
    })
    assert in_place.status_code == 409, in_place.text
    assert in_place.json()["detail"]["code"] == "scenario_project_read_only"
    assert {path.name: path.read_bytes() for path in UNTITLED_SCENARIO_PATH.iterdir()} == untitled_files

    target = tmp_path / "lab_cell.scenario"
    saved = client.post("/api/scenario-documents/save", json={
        **_fence(state), **_save_target(target), "description": "", "confirmOverwrite": False,
    })
    assert saved.status_code == 200, saved.text
    project = state.authoring_workspace.snapshot().as_dict()["project"]
    assert (project["document_path"], project["read_only"], project["document_id"]) == (str(target), False, "lab_cell")
    program_saved = state.authoring_workspace.snapshot().as_dict()["program"]["saved"]
    assert program_saved["available"] and program_saved["matchesDraft"]
    assert program_saved["path"] == str(target / "program.yaml")
    copy, _path, _digest = capture_scenario_artifact(str(target), allowed_roots=[str(tmp_path)])
    assert copy["deployment_draft"] == _UNTITLED_DEPLOYMENT
    assert {path.name: path.read_bytes() for path in UNTITLED_SCENARIO_PATH.iterdir()} == untitled_files




def _sha(character: str) -> str:
    return "sha256:" + character * 64


class _RunnerKernelCell:
    """The Headless reads and claims a Load or New makes, over one Runner
    cycle and the Kernel's authoring admission.

    As live, the Kernel learns the Runner's cycle through the Manager's
    readiness update: its admission reopens ``lag`` admission reads after the
    Runner reports Idle. Activation preparation waits for the open admission,
    as Headless does, and refuses ``authoring_not_ready`` only when it
    stays closed; authoring claims need it open, as the Kernel refuses them.
    The Manager adopts each accepted Apply's claim and releases it once the
    Apply has succeeded.
    """

    current = {"retained_bundle_id": _sha("c"), "graph_revision": _sha("d")}

    def __init__(self, state: str, *, motion_held: bool = False, lag: int = 2) -> None:
        self.cycle: dict = {"state": state, "cycle_generation": 4, "program_run_id": 1}
        if state == "running":
            self.cycle["run"] = {
                "state": "halted" if motion_held else "running",
                "active_operation_count": 0,
                "capture": {"program_run_id": 1},
            }
        self.kernel_cycle = state
        self.lag = lag
        self.claimed = False
        self.generation = 1
        self.resets: list[dict] = []
        self.prepared: list[str] = []
        self.applies: list[str] = []
        self.apply_failure: dict | None = None
        self.held_by_failed_apply = False
        self.release_requests: list[dict] = []
        self.manual_operations: list[dict] = []
        self.prepared_activations: dict[str, dict] = {}

    # Runner (through Headless)
    def program_cycle_status(self) -> dict:
        return deepcopy(self.cycle)

    def execution_control_state(self) -> dict:
        return {"control_state": {"manual_operations": deepcopy(self.manual_operations)}}

    def program_cycle_reset(self, body: dict) -> dict:
        self.resets.append(body)
        self.cycle = {"state": "idle_after_reset", "cycle_generation": 5}
        # The runner's row (OD-214), here already ended in its reply.
        return {"schema_version": "manyforge.program_command_reply.v1", "command": {
            "kind": "reset", "request_key": body["request_key"], "step": "releasing",
            "accepted_at_ns": "1",
            "end": {"outcome": "succeeded", "code": None, "details": None,
                    "finished_at_ns": "2"}}}

    # Kernel authoring admission
    def authoring_admission(self) -> dict:
        if self.kernel_cycle != self.cycle["state"]:
            if self.lag == 0:
                self.kernel_cycle = self.cycle["state"]
            else:
                self.lag -= 1
        return {
            "authoring_allowed": self.kernel_cycle == "idle_after_reset" and not self.claimed,
            "admission_generation": self.generation,
        }

    def _require_admission(self) -> None:
        if self.authoring_admission()["authoring_allowed"] is not True:
            raise HeadlessClientError(
                "authoring_not_ready", "requires the current idle admission", status_code=409
            )

    def acquire_authoring_transaction(self, _body: dict) -> dict:
        self._require_admission()
        self.claimed = True
        self.generation += 1
        return {"claim": {"claim_generation": self.generation,
                          "acquired_admission_generation": self.generation}}

    def settle_authoring_transaction(self, _body: dict) -> dict:
        self.claimed = False
        return {"admission": {"admission_generation": self.generation}}

    # Activation preparer and Manager
    def prepare_activation(self, request: dict) -> dict:
        # Headless waits for a momentarily closed admission (bounded).
        for _ in range(8):
            if self.authoring_admission()["authoring_allowed"] is True:
                break
        self._require_admission()
        self.prepared.append(request["scenario_id"])
        program = request["program"] is not None
        self.prepared_activations[request["captured_input_digest"]] = {"prepared_activation": {
            "schema_version": "manyforge.prepared_activation_input.v7",
            "prepared_activation_id": _sha("1"),
            "origin": {"kind": "scenario", "scenario_id": request["scenario_id"],
                       "source_document_digest": request["source_document_digest"],
                       "project_path": request["project_path"]},
            "captured_input_digest": request["captured_input_digest"],
            "deployment_artifact_id": "deployment",
            "deployment_revision": _sha("2"),
            "retained_bundle_id": _sha("3"),
            "graph_revision": _sha("4"),
            "program_id": "program" if program else None,
            "program_revision": _sha("5") if program else None,
            "program_outcome": ({"kind": "bound", "program_binding_id": _sha("6")}
                                if program else {"kind": "no_program"}),
            "node_environment_digest": _sha("7"),
            "assistant_mode": "forbidden",
        }}
        return _preparation_reply(request)

    def prepared_activation(self, captured_input_digest: str) -> dict:
        return deepcopy(self.prepared_activations[captured_input_digest])

    def manager_status(self) -> dict:
        return {
            "source": {"available": True, "freshness": "fresh", "known_gaps": []},
            "status": {"manager_ready": True, "active": dict(self.current)},
        }

    def manager_deployment_candidate_preview(self, request: dict) -> dict:
        return {"ok": True, "diff": {}, "impact": {"components": []}, "candidate_receipt": {
            "candidate_retained_bundle_id": request["candidate_retained_bundle_id"],
            "candidate_graph_revision": request["candidate_graph_revision"],
            "expected_current": request["expected_current"],
            "impact_digest": _sha("8"),
        }}

    def manager_apply(self, body: dict) -> dict:
        assert self.claimed and body["parent_authoring_claim"]
        self.applies.append(body["request_key"])
        return {"ok": True, "state": "pending", "apply_id": len(self.applies),
                "apply_generation": 1}

    def manager_apply_status(self, _apply_id, _generation, *, application_start_id) -> dict:
        if self.apply_failure is not None:
            failure, self.apply_failure = self.apply_failure, None
            if failure.get("effect_boundary") == "before_effect":
                # Refused before any effect: the Manager releases the claim.
                self.claimed = False
                return {"ok": True, "state": "failed", "failure": failure}
            # An Apply that ended after it started changing the cell: the
            # Manager keeps its adopted claim.
            self.held_by_failed_apply = True
            return {"ok": True, "state": "unknown_effect", "failure": failure}
        self.claimed = False  # the Manager releases the adopted claim
        return {"ok": True, "state": "succeeded"}

    def manager_apply_release_for_new(self, context: dict) -> dict:
        self.release_requests.append(context)
        if not self.held_by_failed_apply:
            return {"ok": True, "released": None}
        self.held_by_failed_apply = self.claimed = False
        return {"ok": True, "released": {"apply_id": len(self.applies),
                                         "apply_generation": 1}}

    def components(self) -> list:
        return []


def _runner_kernel_client(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, cell: _RunnerKernelCell
) -> TestClient:
    """The real Load/New route, admission coordinator and candidate
    preparation; only Headless is a double (plus the read-only views)."""

    state = ComposerState(application_start_id=0x3F9A0000000000A1, allowed_fs_roots=[str(tmp_path), str(UNTITLED_SCENARIO_PATH.parent)])
    state.authoring_admission = AuthoringAdmissionCoordinator(cell, 0x3F9A0000000000A1)  # type: ignore[arg-type]
    state.headless_client = cell  # type: ignore[assignment]
    monkeypatch.setattr(state, "published_scenario", lambda: {"published_scenario": None})
    monkeypatch.setattr(routes_scenario_documents, "program_workspace",
                        lambda _state: {"authoringWorkspace": {"workspaceEpoch": "test"}})
    from manyforge_composer.backend.routes_deployment_draft import HeadlessApplyPorts

    monkeypatch.setattr(HeadlessApplyPorts, "_POLL_SECONDS", 0.0)
    return _scenario_client(state)


@pytest.mark.parametrize("motion_held", [False, True], ids=["finished", "held"])
def test_load_after_a_finished_or_held_run_resets_it_first(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, motion_held: bool
) -> None:
    # A finished run awaiting Reset, or a run whose motion is
    # all held, is Reset first, as part of the Load; the Load proceeds
    # once the Kernel admission has reopened after that Reset.
    target = save_scenario_artifact(
        _document("loaded-project"), str(tmp_path / "loaded-project.scenario"),
        allowed_roots=[str(tmp_path)], overwrite=False,
    )
    cell = _RunnerKernelCell(
        "running" if motion_held else "terminal_awaiting_reset", motion_held=motion_held
    )
    client = _runner_kernel_client(tmp_path, monkeypatch, cell)
    response, response_row = client.command("/api/scenario/load", {"path": target, "discard_unsaved": True})
    assert response.status_code == 200, response.text
    assert response_row["end"]["outcome"] == "succeeded", response.json()
    assert client.state.scenario_document_id == "loaded-project"
    [reset] = cell.resets
    assert (reset["program_run_id"], reset["expected_cycle_generation"]) == (1, 4)
    # The New (its Preview, then the Apply's own preparation), then the file.
    assert cell.prepared == ["Untitled", "Untitled", "loaded-project", "loaded-project"]
    assert len(cell.applies) == 2 and cell.claimed is False


def test_new_after_a_finished_run_resets_it_first(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    cell = _RunnerKernelCell("terminal_awaiting_reset")
    client = _runner_kernel_client(tmp_path, monkeypatch, cell)
    response, response_row = client.command("/api/scenario/new", {"discard_unsaved": True})
    assert response.status_code == 200, response.text
    assert response_row["end"]["outcome"] == "succeeded", response.json()
    assert len(cell.resets) == 1
    assert cell.prepared == ["Untitled", "Untitled"] and len(cell.applies) == 1


def test_new_releases_an_unknown_effect_apply_and_load_works_again(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    # An Apply that failed after it started changing
    # the cell leaves its claim held, so authoring is refused; New releases
    # it and empties the cell, and Load (New plus Apply) works again.
    target = save_scenario_artifact(
        _document("loaded-project"), str(tmp_path / "loaded-project.scenario"),
        allowed_roots=[str(tmp_path)], overwrite=False,
    )
    cell = _RunnerKernelCell("idle_after_reset")
    client = _runner_kernel_client(tmp_path, monkeypatch, cell)
    cell.apply_failure = {
        "code": "configuration_preservation_failed", "effect_boundary": "unknown",
        "message": "Execution host: retained native owner differs from the planned predecessor",
    }
    failed, failed_row = client.command("/api/scenario/load", {"path": target, "discard_unsaved": True})
    assert failed.status_code == 200, failed.text
    # The Manager's terminal is ``unknown_effect``: the row ends ``unknown``
    # (OD-214 contract §3.1).
    assert failed_row["end"]["outcome"] == "unknown"
    assert cell.held_by_failed_apply and cell.claimed
    assert cell.authoring_admission()["authoring_allowed"] is False
    # That Load's own New had nothing to release yet.
    assert len(cell.release_requests) == 1

    response, response_row = client.command("/api/scenario/new", {"discard_unsaved": True})
    assert response.status_code == 200, response.text
    assert response_row["end"]["outcome"] == "succeeded", response.json()
    release = cell.release_requests[-1]
    assert release["source_label"] == "manyforge_composer"
    # New's own fresh key. The context is exactly what the Headless client
    # accepts: its model forbids any other member.
    assert len(release["request_key"]) == 32
    ManagerApplyAcknowledgementContext.model_validate(release)
    assert not cell.held_by_failed_apply and not cell.claimed
    assert cell.authoring_admission()["authoring_allowed"] is True

    applies = len(cell.applies)
    loaded, loaded_row = client.command("/api/scenario/load", {"path": target, "discard_unsaved": True})
    assert loaded.status_code == 200, loaded.text
    assert loaded_row["end"]["outcome"] == "succeeded", loaded.json()
    assert client.state.scenario_document_id == "loaded-project"
    # Its New found nothing left to release.
    assert len(cell.release_requests) == 3 and len(cell.applies) == applies + 2


@pytest.mark.parametrize("boundary", ["before_effect", "unknown"])
def test_load_failed_before_any_effect_restores_the_previous_drafts(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, boundary: str
) -> None:
    # OD-87: the Load commits its workspace before the Manager submission. A
    # Manager Apply that then fails before any effect leaves the cell running
    # the previous Scenario, so the previous Drafts come back and the typed
    # failure is the Load's result. An unknown effect restores nothing.
    target = save_scenario_artifact(
        _document("loaded-project"), str(tmp_path / "loaded-project.scenario"),
        allowed_roots=[str(tmp_path)], overwrite=False,
    )
    cell = _RunnerKernelCell("idle_after_reset")
    state = ComposerState(application_start_id=0x3F9A0000000000A1, allowed_fs_roots=[str(tmp_path), str(UNTITLED_SCENARIO_PATH.parent)])
    state.authoring_admission = AuthoringAdmissionCoordinator(cell, 0x3F9A0000000000A1)  # type: ignore[arg-type]
    state.headless_client = cell  # type: ignore[assignment]
    monkeypatch.setattr(state, "published_scenario", lambda: {"published_scenario": None})
    monkeypatch.setattr(routes_scenario_documents, "program_workspace",
                        lambda _state: {"authoringWorkspace": {"workspaceEpoch": "test"}})
    from manyforge_composer.backend.routes_deployment_draft import HeadlessApplyPorts

    monkeypatch.setattr(HeadlessApplyPorts, "_POLL_SECONDS", 0.0)
    client = _scenario_client(state)
    loaded, loaded_row = client.command("/api/scenario/load", {"path": target, "discard_unsaved": True})
    assert loaded_row["end"]["outcome"] == "succeeded", loaded.json()

    def drafts() -> tuple[dict, dict, dict]:
        program = state.draft_session.retained_state()
        deployment = state.deployment_draft_session.retained_state()
        program.pop("draft_id")
        deployment.pop("draft_id")
        return program, deployment, state.authoring_workspace.project_facts()

    before = drafts()
    assert before[2]["document_id"] == "loaded-project"
    cell.apply_failure = {
        "code": "apply_fence_evidence_failed", "effect_boundary": boundary,
        "message": "Managed Kernel Apply-fence owner: no reconciled active configuration",
        "cause_details": {"cause_code": "kernel_apply_fence_request_failed",
                          "cause": "the execution owner has no reconciled active configuration"},
    }
    failed, failed_row = client.command("/api/scenario/load", {"path": target, "discard_unsaved": True})
    assert failed.status_code == 200, failed.text
    # A Manager terminal ``unknown_effect`` ends the row ``unknown`` (OD-214
    # contract §3.1); a failure before any effect ends it ``failed``.
    assert failed_row["end"]["outcome"] == (
        "failed" if boundary == "before_effect" else "unknown"
    )
    assert failed_row["end"]["code"] == "kernel_apply_fence_request_failed"
    if boundary == "before_effect":
        assert drafts() == before
        assert state.authoring_workspace.snapshot().as_dict()["project"][
            "document_id"] == "loaded-project"
        assert cell.authoring_admission()["authoring_allowed"] is True
    else:
        assert drafts() != before
        assert state.authoring_workspace.project_facts()["document_id"] != "loaded-project"


class _FailedStopKernel:
    """The Kernel reads one real Runner port makes at Reset: its Program-run
    Stop left the run's node faulted (nothing contained)."""

    def identity(self) -> dict:
        return {"ok": True, "identity": {"application_start_id": "3f9a00000000002a"}}

    def stop_program_run(self, body: dict) -> dict:
        assert set(body) == {"request_context", "program_run_id"}
        return {"ok": True, "command": "stop_program_run", "accepted": {
            "request_key": body["request_context"]["request_key"], "event_id": 1,
            "hold_revision": 1, "replay_status": "accepted_new",
            "program_run_id": body["program_run_id"], "requested": "program_end",
            "applied": "program_end", "widen_reason": "none", "covered": [],
            "stale_diagnostics": [], "stale_total": 0, "accepted_at_ns": 1}}

    def read_program_execution_scope(self, program_run_id: int) -> dict:
        return {"ok": True, "scope": {
            "schema_version": "manyforge.program_execution_scope.v3",
            "claim_generation": 1, "program_run_id": program_run_id,
            "hold_revision": 1, "ready_node_indices": [], "held_node_indices": [],
            "stopping_node_indices": [], "faulted_node_indices": [0],
            "selection_release_block_reason": None, "stopped_by": None,
            "rule_id": None, "frozen": "program_end", "frozen_by": 1,
            "all_groups_held": False}}

    def __getattr__(self, name: str):
        if name in {"submit", "read", "reset", "acknowledge_completed", "stop"}:
            return lambda *_args: pytest.fail(f"Reset made an unexpected {name}")
        raise AttributeError(name)


class _FailedStopRunnerCell(_RunnerKernelCell):
    """The finished run's Reset goes through the real Runner port, whose
    Program-run Stop ended ``failed``."""

    def __init__(self) -> None:
        from manyforge_behavior.kernel_client import KernelClient
        from manyforge_behavior.production_ports import NativeKernelProgramPort

        super().__init__("terminal_awaiting_reset")
        self.port = NativeKernelProgramPort(KernelClient(_FailedStopKernel()))
        self.port.scope.request_stop(1, bytes(range(16)), "operator Stop")

    def program_cycle_reset(self, body: dict) -> dict:
        self.port.scope.reset(body["program_run_id"])
        return super().program_cycle_reset(body)


@pytest.mark.parametrize("route", ["new", "load"])
def test_new_and_load_succeed_after_a_failed_program_stop(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, route: str
) -> None:
    # A failed Stop leaves the cell latch to block execution; the Reset
    # that New (and Load, New plus Apply) makes first still completes.
    cell = _FailedStopRunnerCell()
    client = _runner_kernel_client(tmp_path, monkeypatch, cell)
    if route == "new":
        response, response_row = client.command("/api/scenario/new", {"discard_unsaved": True})
    else:
        target = save_scenario_artifact(
            _document("loaded-project"), str(tmp_path / "loaded-project.scenario"),
            allowed_roots=[str(tmp_path)], overwrite=False,
        )
        response, response_row = client.command(
            "/api/scenario/load", {"path": target, "discard_unsaved": True}
        )
    assert response.status_code == 200, response.text
    assert response_row["end"]["outcome"] == "succeeded", response.json()
    assert len(cell.resets) == 1
    assert cell.port.scope.observe(1).state.value == "settled"


def test_load_while_run_motion_executes_is_refused_before_any_effect(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    target = save_scenario_artifact(
        _document("loaded-project"), str(tmp_path / "loaded-project.scenario"),
        allowed_roots=[str(tmp_path)], overwrite=False,
    )
    cell = _RunnerKernelCell("running")
    client = _runner_kernel_client(tmp_path, monkeypatch, cell)
    response, response_row = client.command("/api/scenario/load", {"path": target, "discard_unsaved": True})
    assert response.status_code == 409, response.text
    assert response.json()["detail"]["code"] == "apply_refused_run_in_progress"
    assert cell.resets == [] and cell.prepared == [] and cell.applies == []


# New waits while motion executes or a Stop is in progress (maintainer,
# 2026-11-13): refused before it releases or changes anything.
def test_new_while_run_motion_executes_is_refused_before_it_releases_anything(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    cell = _RunnerKernelCell("running")
    cell.held_by_failed_apply = True
    client = _runner_kernel_client(tmp_path, monkeypatch, cell)
    response, response_row = client.command("/api/scenario/new", {"discard_unsaved": True})
    assert response.status_code == 409, response.text
    detail = response.json()["detail"]
    assert (detail["code"], detail["effect"]) == ("apply_refused_run_in_progress", "none")
    assert cell.release_requests == [] and cell.held_by_failed_apply
    assert cell.resets == [] and cell.prepared == [] and cell.applies == []


@pytest.mark.parametrize("operation", ["executing", "stopping"])
def test_new_during_a_manual_move_is_refused_before_any_effect_and_works_after_it(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, operation: str
) -> None:
    cell = _RunnerKernelCell("idle_after_reset")
    cell.manual_operations = [{"resident": True, "operation": {"state": operation}}]
    client = _runner_kernel_client(tmp_path, monkeypatch, cell)
    response, response_row = client.command("/api/scenario/new", {"discard_unsaved": True})
    assert response.status_code == 409, response.text
    detail = response.json()["detail"]
    assert (detail["code"], detail["effect"]) == ("authoring_not_ready", "none")
    assert cell.release_requests == [] and cell.prepared == [] and cell.applies == []

    cell.manual_operations = [{"resident": False, "operation": {"state": "stopped"}}]
    response, response_row = client.command("/api/scenario/new", {"discard_unsaved": True})
    assert response.status_code == 200, response.text
    assert response_row["end"]["outcome"] == "succeeded", response.json()
    assert len(cell.release_requests) == 1 and len(cell.applies) == 1


def test_new_with_all_run_motion_held_resets_the_run_first(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    # Unchanged by New's motion check: a held run is Reset, then New runs.
    cell = _RunnerKernelCell("running", motion_held=True)
    client = _runner_kernel_client(tmp_path, monkeypatch, cell)
    response, response_row = client.command("/api/scenario/new", {"discard_unsaved": True})
    assert response.status_code == 200, response.text
    assert response_row["end"]["outcome"] == "succeeded", response.json()
    assert len(cell.resets) == 1 and len(cell.applies) == 1


class _BusyComponentCell(_RunnerKernelCell):
    """Two enabled arms the New removes; ``arm_b`` runs a Reset position
    (OD-194). Records every component action and authoring claim acquire."""

    def __init__(self) -> None:
        super().__init__("idle_after_reset")
        self.states: list[dict] = [
            {"component_id": component_id, "lifecycle": "enabled", "activity": "idle",
             "streams": [], "capabilities": [], "observed_at_ns": 1}
            for component_id in ("arm_a", "arm_b")
        ]
        self.states[1].update(
            activity="busy", action={"action": "reset_position", "started_at_ns": 1}
        )
        self.component_actions: list[tuple[str, str]] = []
        self.acquires = 0

    def action_ends(self) -> None:
        self.states[1].pop("action")
        self.states[1]["activity"] = "idle"

    def components(self) -> list:
        return deepcopy(self.states)

    def component_action(self, component_id: str, action: str) -> dict:
        from manyforge_headless_api.models import ComponentActionRequest

        ComponentActionRequest.model_validate({"action": action})
        self.component_actions.append((component_id, action))
        row = next(row for row in self.states if row["component_id"] == component_id)
        row["lifecycle"] = "prepared"
        row["last_action"] = {"action": action, "outcome": "succeeded",
                              "started_at_ns": 2, "finished_at_ns": 3}
        return {"ok": True}

    def acquire_authoring_transaction(self, body: dict) -> dict:
        self.acquires += 1
        return super().acquire_authoring_transaction(body)

    def manager_deployment_candidate_preview(self, request: dict) -> dict:
        preview = super().manager_deployment_candidate_preview(request)
        preview["impact"]["components"] = [
            {"controllable_component_id": component_id, "disposition": "remove"}
            for component_id in ("arm_a", "arm_b")
        ]
        return preview


def _assert_refused_for_the_busy_component(response, cell: _BusyComponentCell) -> None:
    assert response.status_code == 409, response.text
    detail = response.json()["detail"]
    assert (detail["code"], detail["effect"]) == ("component_action_in_progress", "none")
    assert detail["details"] == {"component_id": "arm_b", "action": "reset_position"}
    # Nothing disabled, released, claimed, prepared or applied.
    assert cell.component_actions == [] and cell.acquires == 0
    assert cell.release_requests == [] and cell.prepared == [] and cell.applies == []


def test_new_during_a_component_action_is_refused_before_any_disable_or_claim(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    # OD-194: New is refused up front while any component action runs.
    cell = _BusyComponentCell()
    client = _runner_kernel_client(tmp_path, monkeypatch, cell)
    response, response_row = client.command("/api/scenario/new", {"discard_unsaved": True})
    _assert_refused_for_the_busy_component(response, cell)

    cell.action_ends()
    response, response_row = client.command("/api/scenario/new", {"discard_unsaved": True})
    assert response.status_code == 200, response.text
    assert response_row["end"]["outcome"] == "succeeded", response.json()
    assert sorted(cell.component_actions) == [("arm_a", "disable"), ("arm_b", "disable")]
    assert cell.acquires >= 1 and len(cell.applies) == 1


def test_load_during_a_component_action_is_refused_before_its_new_disables_or_claims(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    # OD-194: Load's New step shares New's refusal.
    target = save_scenario_artifact(
        _document("loaded-project"), str(tmp_path / "loaded-project.scenario"),
        allowed_roots=[str(tmp_path)], overwrite=False,
    )
    cell = _BusyComponentCell()
    client = _runner_kernel_client(tmp_path, monkeypatch, cell)
    response, response_row = client.command("/api/scenario/load", {"path": target, "discard_unsaved": True})
    _assert_refused_for_the_busy_component(response, cell)

    cell.action_ends()
    response, response_row = client.command("/api/scenario/load", {"path": target, "discard_unsaved": True})
    assert response.status_code == 200, response.text
    assert response_row["end"]["outcome"] == "succeeded", response.json()
    # The New disables both arms; the file's Apply finds them already gone.
    assert sorted(cell.component_actions) == [("arm_a", "disable"), ("arm_b", "disable")]
    assert cell.acquires >= 1 and len(cell.applies) == 2

_INVALID_SCENARIO_NAMES = [
    "New cell", "new\tcell", " new_cell", "new_cell ", "-leading", "_leading",
    "dotted.name", "slash/name", "", "caf\u00e9", "\u0441ell", "a" * 129,
    "../escape", "name.scenario", "new_cell\n",
]


@pytest.mark.parametrize("name", _INVALID_SCENARIO_NAMES)
def test_save_as_refuses_an_invalid_scenario_name(tmp_path: Path, name: str) -> None:
    state = ComposerState(application_start_id=0x3F9A0000000000A1, allowed_fs_roots=[str(tmp_path)])
    state.authoring_admission = _PassThroughAdmission()  # type: ignore[assignment]
    before = state.authoring_workspace.snapshot()
    response = _scenario_client(state).post("/api/scenario-documents/save", json={
        **_fence(state), "directory": str(tmp_path), "name": name,
        "description": "", "confirmOverwrite": False,
    })
    assert response.status_code == 422, response.text
    assert state.authoring_workspace.snapshot() == before
    assert [path for path in tmp_path.iterdir() if path.is_dir()] == []


@pytest.mark.parametrize("name", [
    "a", "A", "7", "Lite6_pick-and_place", "mock_ur5_pick_and_place_minimal",
    "sim_lite6_pick_and_place", "X" * 128,
])
def test_scenario_name_rule_accepts_one_word_names(name: str) -> None:
    assert scenario_name_valid(name)
    assert scenario_project_path("/cells", name) == f"/cells/{name}.scenario"
    assert scenario_project_stem(f"/cells/{name}.scenario") == name


@pytest.mark.parametrize("name", _INVALID_SCENARIO_NAMES)
def test_scenario_name_rule_refuses_other_names(name: str) -> None:
    assert not scenario_name_valid(name)
    with pytest.raises(ScenarioArtifactError) as raised:
        scenario_project_path("/cells", name)
    assert raised.value.code == "scenario_name_invalid"


def test_capture_reads_one_source_identity(tmp_path: Path) -> None:
    target = tmp_path / "captured_cell.scenario"
    original = build_scenario_document(
        scenario_id="captured_cell",
        name="captured_cell",
        description="",
        program_draft=None,
        deployment_draft=None,
        manyforge_version="1.0.0",
        last_known_status="incomplete",
    )
    replacement = build_scenario_document(
        scenario_id="captured_cell",
        name="captured_cell",
        description="replacement",
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
    captured, source, digest = capture_scenario_artifact(
        str(target), allowed_roots=[str(tmp_path)]
    )
    save_scenario_artifact(
        replacement,
        str(target),
        allowed_roots=[str(tmp_path)],
        overwrite=True,
    )
    assert (captured["scenario_id"], captured["description"]) == ("captured_cell", "")
    assert source == str(target.resolve())
    assert digest.startswith("sha256:")


def test_removed_parameter_override_forms_are_refused_not_read(tmp_path: Path) -> None:
    # The in-memory document form.
    with pytest.raises(ScenarioArtifactError) as document_form:
        validate_scenario_document(
            {**_document(), "typed_parameter_overrides": {}}, source="<test>"
        )
    assert document_form.value.code == "scenario_artifact_version_unsupported"
    assert "overrides.yaml) were removed" in str(document_form.value)

    # A current project with a leftover overrides.yaml member.
    target = Path(save_scenario_artifact(
        _document("leftover"), str(tmp_path / "leftover.scenario"),
        allowed_roots=[str(tmp_path)], overwrite=False,
    ))
    (target / "overrides.yaml").write_text("{}\n", encoding="utf-8")
    with pytest.raises(ScenarioArtifactError) as leftover:
        load_scenario_artifact(str(target), allowed_roots=[str(tmp_path)])
    assert leftover.value.code == "scenario_artifact_version_unsupported"
    # Save never silently drops or rewrites the removed member.
    with pytest.raises(ScenarioArtifactError) as overwrite:
        save_scenario_artifact(
            _document("leftover"), str(target), allowed_roots=[str(tmp_path)], overwrite=True,
        )
    assert overwrite.value.code == "scenario_artifact_version_unsupported"
    assert (target / "overrides.yaml").read_text(encoding="utf-8") == "{}\n"

    # A former v1 project (overrides.yaml in its member table).
    (target / "overrides.yaml").unlink()
    manifest = json.loads((target / "scenario.yaml").read_text(encoding="utf-8"))
    manifest["api_version"] = "manyforge.scenario.project.v1"
    (target / "scenario.yaml").write_text(json.dumps(manifest), encoding="utf-8")
    with pytest.raises(ScenarioArtifactError) as former:
        load_scenario_artifact(str(target), allowed_roots=[str(tmp_path)])
    assert former.value.code == "scenario_artifact_version_unsupported"
    assert "manyforge.scenario.project.v1" in str(former.value)
    _, entries = discover_scenario_artifacts(
        str(tmp_path), allowed_roots=[str(tmp_path)], manyforge_version="1.0.0",
    )
    assert entries[0]["display_status"] == "unsupported"
    assert "overrides.yaml) were removed" in entries[0]["status_detail"]


def test_routes_refuse_leftover_overrides_before_any_effect(tmp_path: Path) -> None:
    """Load and Save refuse a leftover overrides.yaml with no file or workspace effect.

    Both routes refuse before touching the project or the authoring
    workspace, so their operation boundary marks the refusal as no-effect.
    """

    state = ComposerState(application_start_id=0x3F9A0000000000A1, allowed_fs_roots=[str(tmp_path)])
    state.authoring_admission = _PassThroughAdmission()  # type: ignore[assignment]
    client = _scenario_client(state)
    target = Path(save_scenario_artifact(
        _document("leftover"), str(tmp_path / "leftover.scenario"),
        allowed_roots=[str(tmp_path)], overwrite=False,
    ))
    (target / "overrides.yaml").write_text("speed: 0.5\n", encoding="utf-8")
    before_files = {
        path.name: path.read_bytes() for path in sorted(target.iterdir())
    }
    before_workspace = state.authoring_workspace.snapshot().as_dict()

    responses = [
        client.command("/api/scenario/load", {
            "path": str(target), "discard_unsaved": True,
        })[0],
        client.post("/api/scenario-documents/save", json={
            **_fence(state), **_save_target(target),
            "description": "", "confirmOverwrite": True,
        }),
    ]

    for response in responses:
        assert response.status_code == 409
        assert response.json()["detail"]["code"] == (
            "scenario_artifact_version_unsupported"
        )
        assert "overrides.yaml) were removed" in response.json()["detail"]["message"]
        assert response.json()["detail"]["effect"] == "none"
    assert {
        path.name: path.read_bytes() for path in sorted(target.iterdir())
    } == before_files
    assert state.authoring_workspace.snapshot().as_dict() == before_workspace


def _project_bytes(project: Path) -> dict[str, bytes]:
    return {
        path.name: path.read_bytes()
        for path in sorted(project.iterdir())
        if path.is_file()
    }


def test_save_failure_codes_never_mean_both_refused_and_partially_written(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    """A pre-write code is a refusal; after the first write there is one code.

    The browser releases its guard for a pre-write 400 and keeps a partial Save
    unconfirmed, so no code may be raised on both sides of the first write.
    """

    roots = [str(tmp_path)]
    project = Path(save_scenario_artifact(
        _document("cell"), str(tmp_path / "cell.scenario"), allowed_roots=roots, overwrite=False,
    ))
    before = _project_bytes(project)

    # Pre-write: the target is not a directory / the project cannot be listed.
    (tmp_path / "plain.scenario").write_text("not a project\n", encoding="utf-8")
    with pytest.raises(ScenarioArtifactError) as not_directory:
        save_scenario_artifact(
            _document("plain"), str(tmp_path / "plain.scenario"), allowed_roots=roots, overwrite=True,
        )
    assert not_directory.value.code == "scenario_artifact_target_invalid"
    assert not_directory.value.status_code == 400
    assert (tmp_path / "plain.scenario").read_text(encoding="utf-8") == "not a project\n"

    def unlistable(_fd):
        raise OSError("listing refused")

    with monkeypatch.context() as patch:
        patch.setattr(scenario_artifacts.os, "listdir", unlistable)
        with pytest.raises(ScenarioArtifactError) as unreadable:
            save_scenario_artifact(
                _document("cell"), str(project), allowed_roots=roots, overwrite=True,
            )
    assert unreadable.value.code == "scenario_artifact_write_failed"
    assert unreadable.value.status_code == 400
    assert _project_bytes(project) == before

    # Post-write: the same causes after the save marker was written.
    replace = scenario_artifacts._replace_regular_at

    def fail_after_marker(directory_fd, name, raw):
        if name != scenario_artifacts.SCENARIO_SAVE_MARKER:
            raise OSError("No space left on device")
        replace(directory_fd, name, raw)

    with monkeypatch.context() as patch:
        patch.setattr(scenario_artifacts, "_replace_regular_at", fail_after_marker)
        with pytest.raises(ScenarioArtifactError) as write_failed:
            save_scenario_artifact(
                _document("cell"), str(project), allowed_roots=roots, overwrite=True,
            )
    assert write_failed.value.code == "scenario_artifact_save_incomplete"
    assert write_failed.value.status_code == 500
    assert "No space left on device" in str(write_failed.value)
    assert (project / scenario_artifacts.SCENARIO_SAVE_MARKER).is_file()

    other = Path(save_scenario_artifact(
        _document("other"), str(tmp_path / "other.scenario"), allowed_roots=roots, overwrite=False,
    ))
    (other / "program.yaml").unlink()
    (other / "program.yaml").mkdir()
    with pytest.raises(ScenarioArtifactError) as member_invalid:
        save_scenario_artifact(
            _document("other"), str(other), allowed_roots=roots, overwrite=True,
        )
    assert member_invalid.value.code == "scenario_artifact_save_incomplete"
    assert "is not a regular file" in str(member_invalid.value)
    assert (other / scenario_artifacts.SCENARIO_SAVE_MARKER).is_file()
    with pytest.raises(ScenarioArtifactError) as reopened:
        load_scenario_artifact(str(other), allowed_roots=roots)
    assert reopened.value.code == "scenario_project_save_interrupted"


def test_save_route_reports_a_partial_write_as_its_distinct_outcome(
    tmp_path: Path,
) -> None:
    state = ComposerState(application_start_id=0x3F9A0000000000A1, allowed_fs_roots=[str(tmp_path)])
    state.authoring_admission = _PassThroughAdmission()  # type: ignore[assignment]
    client = _scenario_client(state)
    project = Path(save_scenario_artifact(
        _document(), str(tmp_path / "cell_fixture.scenario"),
        allowed_roots=[str(tmp_path)], overwrite=False,
    ))
    (project / "program.yaml").unlink()
    (project / "program.yaml").mkdir()

    response = client.post("/api/scenario-documents/save", json={
        **_fence(state), **_save_target(project),
        "description": "", "confirmOverwrite": True,
    })

    assert response.status_code == 500
    assert response.json()["detail"]["code"] == "scenario_artifact_save_incomplete"


def test_single_file_scenario_is_not_a_project(tmp_path: Path) -> None:
    source = tmp_path / "legacy.scenario.yaml"
    source.write_text(json.dumps(_document()), encoding="utf-8")

    with pytest.raises(ScenarioArtifactError) as direct:
        load_scenario_artifact(str(source), allowed_roots=[str(tmp_path)])
    assert direct.value.code == "scenario_artifact_path_invalid"


def test_valid_hand_edit_is_loaded_without_rechecking_during_discovery(tmp_path: Path) -> None:
    target = Path(
        save_scenario_artifact(
            _document("edited"),
            str(tmp_path / "edited.scenario"),
            allowed_roots=[str(tmp_path)],
            overwrite=False,
        )
    )
    edited = deepcopy(_document("edited")["program_draft"])
    edited["description"] = "Edited outside"
    (target / "program.yaml").write_text(json.dumps(edited) + "\n", encoding="utf-8")

    loaded, _ = load_scenario_artifact(
        str(target), allowed_roots=[str(tmp_path)]
    )
    assert loaded["program_draft"] == edited
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
            _document("original"),
            str(tmp_path / "original.scenario"),
            allowed_roots=[str(tmp_path)],
            overwrite=False,
        )
    )
    (tmp_path / "nested").mkdir()
    copied = Path(
        save_scenario_artifact(
            _document("copy"),
            str(tmp_path / "nested" / "copy.scenario"),
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


@pytest.mark.parametrize(
    "scenario",
    ["mock_dual_arm_shared_group", "mock_dual_arm_separate_groups"],
)
def test_saving_unchanged_generated_scenario_reproduces_generator_bytes(
    scenario: str,
) -> None:
    # Saving an unchanged project reproduces its checked-in bytes.
    examples = Path(__file__).resolve().parents[2] / "examples"
    project = examples / f"{scenario}.scenario"
    document, path = load_scenario_artifact(
        str(project), allowed_roots=[str(examples)]
    )

    _, target, members, manifest = prepare_scenario_artifact(
        document,
        path,
        allowed_roots=[str(examples)],
        source_project_path=path,
    )

    assert target == project
    for filename, raw in members.items():
        assert raw == (project / filename).read_bytes(), filename
    assert manifest == (project / "scenario.yaml").read_bytes()


def test_source_changes_are_advisory_and_export_is_self_contained(
    tmp_path: Path,
) -> None:
    original = Path(
        save_scenario_artifact(
            _document("original"),
            str(tmp_path / "original.scenario"),
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

    (tmp_path / "portable").mkdir()
    exported = Path(
        export_scenario_artifact(
            captured,
            str(tmp_path / "portable" / "original.scenario"),
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


def test_export_can_be_loaded_from_a_separate_allowed_project_root(
    tmp_path: Path,
) -> None:
    examples = tmp_path / "examples"
    projects = tmp_path / "user projects"
    examples.mkdir()
    projects.mkdir()
    (examples / "model.urdf").write_text('<robot name="fixture"/>\n', encoding="utf-8")
    roots = [str(examples), str(projects)]
    original = Path(save_scenario_artifact(
        _document("original"), str(examples / "original.scenario"),
        allowed_roots=roots, overwrite=False,
    ))
    document, _ = load_scenario_artifact(str(original), allowed_roots=roots)

    exported = Path(export_scenario_artifact(
        document, str(projects / "original.scenario"),
        source_project_path=str(original), allowed_roots=roots,
    ))
    shutil.rmtree(original)
    (examples / "model.urdf").unlink()

    reopened, reopened_path = load_scenario_artifact(
        str(exported), allowed_roots=[str(projects)],
    )
    assert reopened_path == str(exported.resolve())
    assert reopened["scenario_id"] == "original"
    model = reopened["deployment_draft"]["components"][0]["model_source"]["urdf"]
    assert (exported / model["project_path"]).read_text(encoding="utf-8") == (
        '<robot name="fixture"/>\n'
    )


def test_dynamic_index_lists_only_project_documents(tmp_path: Path) -> None:
    save_scenario_artifact(
        _document("saved"),
        str(tmp_path / "saved.scenario"),
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

    state = ComposerState(application_start_id=0x3F9A0000000000A1, allowed_fs_roots=[str(tmp_path)])
    state.headless_client = Headless()  # type: ignore[assignment]
    response = _scenario_client(state).get(
        "/api/scenario-documents",
        params={"directory": str(tmp_path)},
    )

    assert response.status_code == 200
    body = response.json()
    assert [(row["source_kind"], row["scenario_id"]) for row in body["entries"]] == [
        ("saved", "saved"),
    ]


@pytest.mark.parametrize("destructive", [False, True])
def test_load_confirmation_accepts_a_disruptive_impact_in_one_preparation(
    tmp_path: Path, destructive: bool
) -> None:
    """Load is the operator's confirmation: one preparation, no token round trip."""

    current = {"retained_bundle_id": "bundle-current", "graph_revision": "graph-current"}
    source_document_digest = "sha256:" + "1" * 64
    prepared_activation = {
        "schema_version": "manyforge.prepared_activation_input.v7",
        "prepared_activation_id": "prepared-1",
        "origin": {"kind": "scenario", "scenario_id": "cell",
                   "source_document_digest": source_document_digest,
                   "project_path": "/projects/cell.scenario"},
        "captured_input_digest": "sha256:" + "2" * 64,
        "deployment_artifact_id": "deployment",
        "deployment_revision": "deployment-revision",
        "retained_bundle_id": "bundle-next",
        "graph_revision": "graph-next",
        "program_id": "program",
        "program_revision": "program-revision",
        "program_outcome": {"kind": "bound", "program_binding_id": "binding"},
        "node_environment_digest": "sha256:" + "3" * 64,
        "assistant_mode": "forbidden",
    }

    class Headless:
        prepare_calls = 0

        def prepare_activation(self, request):
            Headless.prepare_calls += 1
            return _preparation_reply(request)

        def prepared_activation(self, _captured_input_digest):
            return {"prepared_activation": deepcopy(prepared_activation)}

        def manager_status(self):
            return {
                "source": {"available": True, "freshness": "fresh", "known_gaps": []},
                "status": {"manager_ready": True, "active": dict(current)},
            }

        def manager_deployment_candidate_preview(self, _request):
            return {
                "ok": True,
                "diff": {"controllable_components": {"added": ["robot"]}},
                "impact": {"destructive": destructive},
                "candidate_receipt": {
                    "candidate_retained_bundle_id": "bundle-next",
                    "candidate_graph_revision": "graph-next",
                    "expected_current": dict(current),
                    "impact_digest": "sha256:" + "4" * 64,
                },
            }

    state = ComposerState(application_start_id=0x3F9A0000000000A1, allowed_fs_roots=[str(tmp_path)])
    state.headless_client = Headless()  # type: ignore[assignment]
    intent = {
        "scenario_id": "cell",
        "project_path": "/projects/cell.scenario",
        "source_document_digest": source_document_digest,
        "captured_input_digest": prepared_activation["captured_input_digest"],
        "deployment": {},
        "model_source_closure": {},
        "program": None,
        "program_role_assignment": {},
        "expected_manager_current": dict(current),
        "expected_admission_generation": 1,
        "application_start_id": "3f9a0000000000a1",
        "request_key": "d1" * 16,
        "confirmation_token": None,
        "draft_id": "draft",
        "draft_revision": 1,
    }

    unconfirmed = state.execute_authored_deployment_candidate(
        intent, None, apply=True, prepare_only=True
    )
    assert unconfirmed["confirmation_required"] is True
    assert unconfirmed["manager_apply"] is None

    Headless.prepare_calls = 0
    confirmed = state.execute_authored_deployment_candidate(
        intent, None, apply=True, prepare_only=True, confirmed=True
    )
    assert Headless.prepare_calls == 1
    assert confirmed["confirmation_required"] is False
    assert confirmed["confirmation_token"] is None
    assert confirmed["manager_apply"]["accepted_impact_digest"] == (
        "sha256:" + "4" * 64 if destructive else None
    )


@pytest.mark.parametrize("code", ["stale_generation", "authoring_not_ready"])
@pytest.mark.parametrize("claimed", [False, True])
def test_preparation_admission_refusal_is_marked_only_before_the_claim(
    tmp_path: Path, code: str, claimed: bool
) -> None:
    """Apply/Load/New preparation runs before every claim and effect, so the
    preparer's admission refusal there changed nothing. Under a claim the same
    code proves no such thing and stays unmarked."""

    from fastapi import HTTPException

    class Headless:
        manager_calls = 0

        def prepare_activation(self, _request):
            raise HeadlessClientError(code, "admission changed", status_code=409)

        def manager_status(self):
            Headless.manager_calls += 1
            raise AssertionError("no Manager read after a refused preparation")

    state = ComposerState(application_start_id=0x3F9A0000000000A1, allowed_fs_roots=[str(tmp_path)])
    state.headless_client = Headless()  # type: ignore[assignment]
    intent = {
        "scenario_id": "cell",
        "project_path": "/projects/cell.scenario",
        "source_document_digest": "sha256:" + "1" * 64,
        "captured_input_digest": "sha256:" + "2" * 64,
        "deployment": {},
        "model_source_closure": {},
        "program": None,
        "program_role_assignment": {},
        "expected_manager_current": {},
        "expected_admission_generation": 1,
        "application_start_id": "3f9a0000000000a1",
    }
    claim = {"claim": "test"} if claimed else None

    if claimed:
        with pytest.raises(HeadlessClientError) as unmarked:
            state.execute_authored_deployment_candidate(intent, claim, apply=True)
        assert unmarked.value.code == code
    else:
        with pytest.raises(HTTPException) as refused:
            state.execute_authored_deployment_candidate(
                intent, None, apply=True, prepare_only=True
            )
        assert refused.value.status_code == 409
        assert refused.value.detail == {
            "code": code,
            "message": "admission changed",
            "effect": "none",
        }
    assert Headless.manager_calls == 0


def test_other_unclaimed_preparation_failures_stay_unmarked(tmp_path: Path) -> None:
    class Headless:
        def prepare_activation(self, _request):
            raise HeadlessClientError("headless_unavailable", "timed out")

    state = ComposerState(application_start_id=0x3F9A0000000000A1, allowed_fs_roots=[str(tmp_path)])
    state.headless_client = Headless()  # type: ignore[assignment]
    with pytest.raises(HeadlessClientError):
        state.execute_authored_deployment_candidate(
            {
                "scenario_id": "cell",
                "project_path": "/projects/cell.scenario",
                "source_document_digest": "sha256:" + "1" * 64,
                "captured_input_digest": "sha256:" + "2" * 64,
                "deployment": {},
                "model_source_closure": {},
                "program": None,
                "program_role_assignment": {},
                "expected_admission_generation": 1,
                "application_start_id": "3f9a0000000000a1",
            },
            None,
            apply=True,
            prepare_only=True,
        )


class _SettlingHeadless:
    """Kernel authoring-claim double that records every settlement kind."""

    def __init__(self) -> None:
        self.settlements: list[str] = []

    def authoring_admission(self) -> dict:
        return {"authoring_allowed": True, "admission_generation": 1}

    def acquire_authoring_transaction(self, _body: dict) -> dict:
        return {"claim": {"claim_generation": 1, "acquired_admission_generation": 1}}

    def settle_authoring_transaction(self, body: dict) -> dict:
        self.settlements.append(body["settlement_kind"])
        return {"admission": {"admission_generation": 1}}


def _claimed_scenario_state(
    tmp_path: Path,
) -> tuple[ComposerState, TestClient, _SettlingHeadless]:
    """A complete authored workspace whose Saves take a real local claim."""

    state = ComposerState(application_start_id=0x3F9A0000000000A1, allowed_fs_roots=[str(tmp_path)])
    headless = _SettlingHeadless()
    state.authoring_admission = AuthoringAdmissionCoordinator(headless, 0x3F9A0000000000A1)  # type: ignore[arg-type]
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
    return state, TestClient(app), headless


def _save_scenario(client: TestClient, state: ComposerState, target: Path, **extra):
    return client.post("/api/scenario-documents/save", json={
        **_fence(state), **_save_target(target), "description": "written by this request",
        "confirmOverwrite": False, **extra,
    })


@pytest.mark.parametrize("failure", ["readback", "workspace_stale", "workspace_install"])
def test_scenario_save_after_its_write_is_saved_not_adopted_and_never_no_effect(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, failure: str,
) -> None:
    """(a) readback and (b) workspace failures after the project was written."""

    state, client, headless = _claimed_scenario_state(tmp_path)
    coordinator = state.authoring_admission
    target = tmp_path / "saved.scenario"
    before = state.authoring_workspace.snapshot().as_dict()
    original_save = routes_scenario_documents.save_scenario_artifact

    if failure == "readback":
        def unreadable(*_args, **_kwargs):
            raise ScenarioArtifactError(
                "scenario_source_invalid", "model source vanished", status_code=409
            )

        monkeypatch.setattr(
            routes_scenario_documents, "capture_scenario_artifact", unreadable
        )
    elif failure == "workspace_stale":
        def write_then_edit(*args, **kwargs) -> str:
            saved = original_save(*args, **kwargs)
            snapshot = state.authoring_workspace.snapshot()
            project = snapshot.as_dict()["project"]
            project["document_description"] = "concurrent edit"
            state.authoring_workspace.commit_project(
                expected_identity=snapshot.identity, facts=project
            )
            return saved

        monkeypatch.setattr(
            routes_scenario_documents, "save_scenario_artifact", write_then_edit
        )
    else:
        def broken_install(*_args, **_kwargs):
            raise RuntimeError("in-memory install failed")

        monkeypatch.setattr(
            state.draft_session, "install_prepared_save", broken_install
        )

    response = _save_scenario(client, state, target)

    # The destination holds exactly the intended project.
    monkeypatch.undo()
    written, _source = load_scenario_artifact(str(target), allowed_roots=[str(tmp_path)])
    assert written["description"] == "written by this request"
    assert not (target / scenario_artifacts.SCENARIO_SAVE_MARKER).exists()
    # A definite outcome naming the written path, never "no effect".
    assert response.status_code == 409, response.text
    detail = response.json()["detail"]
    assert detail["code"] == "scenario_saved_baseline_not_adopted"
    assert detail["path"] == str(target.resolve())
    assert str(target.resolve()) in detail["message"]
    assert "effect" not in detail
    assert detail["causeCode"] == {
        "readback": "scenario_source_invalid",
        "workspace_stale": "composer_workspace_stale",
        "workspace_install": "composer_workspace_install_failed",
    }[failure]
    if failure == "workspace_install":
        # An in-memory install with an unknown outcome contains the workspace;
        # the claim stays held for diagnosis and is never "proven no effect".
        assert headless.settlements == []
        assert coordinator.local_claim_pending is True
        assert coordinator.mutation_ready is False
        return
    # The claim settles from the recorded write: effect committed, not
    # proven no-effect, and authoring stays usable.
    assert headless.settlements == ["owner_commit"]
    assert coordinator.local_claim_pending is False
    assert coordinator.mutation_ready is True
    current = state.authoring_workspace.snapshot().as_dict()
    assert current["project"]["document_path"] is None
    assert current["program"]["saved"]["available"] is False
    if failure == "readback":
        assert current["workspaceRevision"] == before["workspaceRevision"]
    else:
        assert detail["currentWorkspace"]["workspaceRevision"] == (
            before["workspaceRevision"] + 1
        )
    # The exact request reads back as settled, so the browser is not locked.
    outcome = coordinator.submission_outcome(AuthoringMutationEnvelope(
        expected_admission_generation=1,
        application_start_id=0x3F9A0000000000A1,
        request_key=bytes.fromhex("5ce0" * 8),
    ))
    assert outcome["outcome"]["kind"] == "settled"


@pytest.mark.parametrize("cause", ["confirmation", "marker_write"])
def test_scenario_save_failure_before_its_write_is_not_saved_and_no_effect(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, cause: str,
) -> None:
    """(c) a failure before the destination changed: Not saved."""

    state, client, headless = _claimed_scenario_state(tmp_path)
    target = Path(save_scenario_artifact(
        _document("existing"), str(tmp_path / "existing.scenario"),
        allowed_roots=[str(tmp_path)], overwrite=False,
    ))
    before_files = _project_bytes(target)
    before = state.authoring_workspace.snapshot().as_dict()
    replace = scenario_artifacts._replace_regular_at

    def full_disk(directory_fd, name, raw):
        if name == scenario_artifacts.SCENARIO_SAVE_MARKER:
            raise OSError("No space left on device")
        replace(directory_fd, name, raw)

    if cause == "marker_write":
        monkeypatch.setattr(scenario_artifacts, "_replace_regular_at", full_disk)
    response = _save_scenario(
        client, state, target, confirmOverwrite=cause == "marker_write"
    )

    assert _project_bytes(target) == before_files
    assert list(target.glob(".*.tmp-*")) == []
    detail = response.json()["detail"]
    assert detail["code"] == {
        "confirmation": "scenario_artifact_overwrite_confirmation_required",
        "marker_write": "scenario_artifact_write_failed",
    }[cause]
    assert detail["effect"] == "none"
    assert "path" not in detail
    assert headless.settlements == ["proven_no_effect"]
    assert state.authoring_admission.mutation_ready is True
    assert state.authoring_workspace.snapshot().as_dict() == before


def test_scenario_save_partial_write_is_unverified_with_its_path(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    """A failure that leaves the project neither unchanged nor as intended."""

    state, client, headless = _claimed_scenario_state(tmp_path)
    target = Path(save_scenario_artifact(
        _document("existing"), str(tmp_path / "existing.scenario"),
        allowed_roots=[str(tmp_path)], overwrite=False,
    ))
    replace = scenario_artifacts._replace_regular_at

    def fail_after_marker(directory_fd, name, raw):
        if name == "deployment.yaml":
            raise OSError("No space left on device")
        replace(directory_fd, name, raw)

    monkeypatch.setattr(scenario_artifacts, "_replace_regular_at", fail_after_marker)
    response = _save_scenario(client, state, target, confirmOverwrite=True)

    assert (target / scenario_artifacts.SCENARIO_SAVE_MARKER).is_file()
    assert response.status_code == 500, response.text
    detail = response.json()["detail"]
    assert detail["code"] == "scenario_artifact_save_incomplete"
    assert detail["path"] == str(target)
    assert "effect" not in detail
    assert headless.settlements == ["owner_commit"]
    assert state.authoring_admission.mutation_ready is True


def _member_save(
    tmp_path: Path, member: str
) -> tuple[ComposerState, TestClient, _SettlingHeadless, Path, dict]:
    """Save one project, then edit one member Draft; return its member Save."""

    state, client, headless = _claimed_scenario_state(tmp_path)
    target = tmp_path / "project.scenario"
    assert _save_scenario(client, state, target).status_code == 200
    headless.settlements.clear()
    if member == "program":
        program_state = state.draft_session.retained_state()
        program_state["authoring"]["name"] = "Edited Program"
        edited, diagnostics = derive_candidate(
            program_state["authoring"], state.node_catalog
        )
        program_state["program"] = edited.to_dict()
        program_state["diagnostics"] = diagnostics
        program_state["draft_revision"] += 1
        state.draft_session.install_prepared_replacement(
            state.draft_session.prepare_load_replacement(
                program_state, state.node_catalog
            )
        )
    else:
        deployment_state = state.deployment_draft_session.retained_state()
        deployment_state["document"]["name"] = "Edited Deployment"
        deployment_state["draft_revision"] += 1
        state.deployment_draft_session.install_prepared_replacement(
            state.deployment_draft_session.prepare_load_replacement(
                deployment_state,
                source_project_root=state.deployment_draft_session.source_project_root,
            )
        )
    fence = _fence(state)
    if member == "deployment":
        fence["draftId"] = fence["deploymentDraftId"]
        fence["expectedDraftRevision"] = fence["expectedDeploymentDraftRevision"]
    del fence["deploymentDraftId"], fence["expectedDeploymentDraftRevision"]
    return state, client, headless, target.resolve(), fence


def _member_name(project: Path, member: str) -> str:
    disk, _ = load_scenario_artifact(str(project), allowed_roots=[str(project.parent)])
    return disk[f"{member}_draft"]["name"]


@pytest.mark.parametrize("member", ["program", "deployment"])
@pytest.mark.parametrize(
    "failure", ["readback", "workspace_stale", "workspace_install", "before_write"]
)
def test_scenario_member_save_reports_the_effect_it_recorded(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, member: str, failure: str,
) -> None:
    """(a) readback, (b) workspace commit and (c) pre-write member failures."""

    state, client, headless, project, fence = _member_save(tmp_path, member)
    original_save = scenario_artifacts.save_scenario_artifact
    original_capture = scenario_artifacts.capture_scenario_artifact
    written: list[str] = []

    def save_then(*args, **kwargs) -> str:
        saved = original_save(*args, **kwargs)
        written.append(saved)
        if failure == "workspace_stale":
            snapshot = state.authoring_workspace.snapshot()
            facts = snapshot.as_dict()["project"]
            facts["document_description"] = "concurrent edit"
            state.authoring_workspace.commit_project(
                expected_identity=snapshot.identity, facts=facts
            )
        return saved

    def capture(*args, **kwargs):
        if failure == "readback" and written:
            raise ScenarioArtifactError(
                "scenario_source_invalid", "model source vanished", status_code=409
            )
        return original_capture(*args, **kwargs)

    monkeypatch.setattr(scenario_artifacts, "save_scenario_artifact", save_then)
    monkeypatch.setattr(scenario_artifacts, "capture_scenario_artifact", capture)
    if failure == "before_write":
        # An interrupted earlier save is refused when the project is captured.
        (project / scenario_artifacts.SCENARIO_SAVE_MARKER).write_text("{}\n")
    if failure == "workspace_install":
        def broken_install(*_args, **_kwargs):
            raise RuntimeError("in-memory install failed")

        session = (
            state.draft_session if member == "program"
            else state.deployment_draft_session
        )
        monkeypatch.setattr(session, "install_prepared_save", broken_install)
    before_files = _project_bytes(project)

    response = client.post(f"/api/{member}/save", json=fence)

    detail = response.json()["detail"]
    if failure == "before_write":
        assert written == []
        assert _project_bytes(project) == before_files
        assert detail["code"] == "scenario_project_save_interrupted"
        assert detail["effect"] == "none"
        assert headless.settlements == ["proven_no_effect"]
        return
    monkeypatch.undo()
    expected_name = "Edited Program" if member == "program" else "Edited Deployment"
    assert _member_name(project, member) == expected_name
    assert response.status_code == 409, response.text
    assert detail["code"] == f"{member}_saved_baseline_not_adopted"
    assert detail["path"] == str(project)
    assert str(project) in detail["message"]
    assert "effect" not in detail
    if failure == "workspace_install":
        # Contained in-memory install: the claim stays held, never no-effect.
        assert detail["causeCode"] == "composer_workspace_install_failed"
        assert headless.settlements == []
        assert state.authoring_admission.mutation_ready is False
        return
    assert headless.settlements == ["owner_commit"]
    assert state.authoring_admission.mutation_ready is True
    current = state.authoring_workspace.snapshot().as_dict()
    if member == "program":
        assert current["program"]["saved"]["matchesDraft"] is False
    else:
        assert current["deployment"]["differsFromSaved"] is True


def test_f6b_manager_projection_unavailable_carries_bounded_diagnostics(
    caplog: pytest.LogCaptureFixture,
) -> None:
    """Item 40: the 503 says which check failed, where, and how old the source was."""

    from manyforge_composer.backend import state as state_module
    from manyforge_composer.backend.authoring_admission import draft_session_http_exception
    from manyforge_composer.backend.draft_session import DraftSessionError

    current = {
        "source": {
            "available": True, "freshness": "fresh", "known_gaps": [],
            "receiver_age_ns": 400_000_000, "source_generation": 41,
            "source_owner_id": "manager",
            "projection_generation": 90,
        },
        "status": {"manager_ready": True, "active": {
            "retained_bundle_id": "bundle", "graph_revision": "graph"}},
    }
    stale = deepcopy(current)
    stale["source"].update({
        "freshness": "stale", "known_gaps": ["manager_status_unavailable"],
        "receiver_age_ns": 2_600_000_000, "source_generation": 42,
    })
    before = state_module._MANAGER_PROJECTION_GAPS["count"]
    # Unchanged on success; it marks the last current projection.
    assert ComposerState._manager_current(current) == ("bundle", "graph")
    with caplog.at_level("WARNING", logger=state_module.__name__):
        with pytest.raises(DraftSessionError) as error:
            ComposerState._manager_current(stale, phase="scenario_load.claim")

    assert error.value.code == "manager_projection_unavailable"
    assert error.value.status_code == 503
    detail = error.value.detail
    assert detail["phase"] == "scenario_load.claim"
    assert detail["failed_checks"] == ["source_fresh", "no_known_gaps"]
    assert detail["freshness"] == "stale"
    assert detail["known_gaps"] == ["manager_status_unavailable"]
    assert detail["receiver_age_ms"] == 2_600
    assert isinstance(detail["source_last_update_unix_ms"], int)
    assert detail["source_owner_id"] == "manager"
    assert detail["source_generation"] == 42
    assert detail["projection_generation"] == 90
    assert detail["manager_ready"] is True
    assert detail["unavailable_count"] == before + 1
    assert isinstance(detail["since_last_current_ms"], int)
    # One bounded log line per occurrence.
    lines = [record for record in caplog.records if "manager_projection_unavailable" in record.getMessage()]
    assert len(lines) == 1
    assert len(lines[0].getMessage()) <= len("manager_projection_unavailable ") + 1_024
    # The HTTP refusal keeps `effect: none` and carries the diagnostics.
    http = draft_session_http_exception(error.value)
    assert http.status_code == 503
    assert http.detail["effect"] == "none"
    assert http.detail["context"] == detail

    # OD-281: a section too large for the status frame is refused with its
    # own typed cause.
    oversized = deepcopy(current)
    oversized["source"].update({
        "freshness": "unavailable", "known_gaps": ["manager_status_section_oversized"],
    })
    with pytest.raises(DraftSessionError) as error:
        ComposerState._manager_current(oversized, phase="scenario_load.claim")
    assert error.value.code == "manager_status_section_oversized"
    assert error.value.status_code == 503

    # No source at all: every check is named, nothing raises while reporting it.
    with pytest.raises(DraftSessionError) as missing:
        ComposerState._manager_current({}, phase="activation.after_preparation")
    assert missing.value.detail["failed_checks"] == [
        "source_available", "source_fresh", "no_known_gaps",
        "manager_ready", "active_identity",
    ]
    assert missing.value.detail["receiver_age_ms"] is None


def _authored_save_client(tmp_path: Path) -> tuple[ComposerState, TestClient]:
    """A complete authored workspace whose Scenario Save passes admission."""

    state = ComposerState(application_start_id=0x3F9A0000000000A1, allowed_fs_roots=[str(tmp_path)])
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
    return state, _scenario_client(state)


def test_save_as_writes_one_name_and_requires_overwrite_confirmation(
    tmp_path: Path,
) -> None:
    state, client = _authored_save_client(tmp_path)
    (tmp_path / "cells").mkdir()
    (tmp_path / "cells" / "model.urdf").write_text('<robot name="fixture"/>\n')
    body = {
        **_fence(state), "directory": str(tmp_path / "cells"), "name": "Pick_Cell-1",
        "description": "Trial (a) automatic retry", "confirmOverwrite": False,
    }

    response = client.post("/api/scenario-documents/save", json=body)

    assert response.status_code == 200, response.text
    target = tmp_path / "cells" / "Pick_Cell-1.scenario"
    assert response.json()["path"] == str(target.resolve())
    assert response.json()["scenario_id"] == "Pick_Cell-1"
    manifest = json.loads((target / "scenario.yaml").read_text(encoding="utf-8"))
    assert (manifest["scenario_id"], manifest["name"], manifest["description"]) == (
        "Pick_Cell-1", "Pick_Cell-1", "Trial (a) automatic retry",
    )

    # Another project at the same name is replaced only on explicit request.
    other, other_client = _authored_save_client(tmp_path)
    refused = other_client.post("/api/scenario-documents/save", json={
        **body, **_fence(other), "description": "second",
    })
    assert refused.status_code == 409, refused.text
    assert json.loads((target / "scenario.yaml").read_text(encoding="utf-8"))[
        "description"
    ] == "Trial (a) automatic retry"
    replaced = other_client.post("/api/scenario-documents/save", json={
        **body, **_fence(other), "description": "second", "confirmOverwrite": True,
    })
    assert replaced.status_code == 200, replaced.text
    assert json.loads((target / "scenario.yaml").read_text(encoding="utf-8"))[
        "description"
    ] == "second"


def test_save_as_requires_a_parent_directory_inside_the_roots(tmp_path: Path) -> None:
    root = tmp_path / "root"
    root.mkdir()
    state = ComposerState(application_start_id=0x3F9A0000000000A1, allowed_fs_roots=[str(root)])
    state.authoring_admission = _PassThroughAdmission()  # type: ignore[assignment]
    client = _scenario_client(state)
    for directory in (str(tmp_path), str(root / ".."), str(root / "missing")):
        response = client.post("/api/scenario-documents/save", json={
            **_fence(state), "directory": directory, "name": "cell",
            "description": "", "confirmOverwrite": False,
        })
        assert response.status_code in {403, 404}, response.text
    assert not (tmp_path / "cell.scenario").exists()
    assert not (root / "cell.scenario").exists()


def _rewrite_manifest(project: Path, **fields: str) -> None:
    manifest = json.loads((project / "scenario.yaml").read_text(encoding="utf-8"))
    manifest.update(fields)
    (project / "scenario.yaml").write_text(json.dumps(manifest), encoding="utf-8")


@pytest.mark.parametrize("case", ["old_id", "title_name", "other_id", "no_suffix"])
def test_projects_outside_the_one_name_rule_are_refused(tmp_path: Path, case: str) -> None:
    """No legacy compatibility: folder ``<name>.scenario``, scenario_id == name == <name>."""

    project = Path(save_scenario_artifact(
        _document("cell"), str(tmp_path / "cell.scenario"),
        allowed_roots=[str(tmp_path)], overwrite=False,
    ))
    if case == "old_id":
        _rewrite_manifest(project, scenario_id="legacy.cell-v1", name="legacy.cell-v1")
    elif case == "title_name":
        _rewrite_manifest(project, name="Cell (old title)")
    elif case == "other_id":
        _rewrite_manifest(project, scenario_id="other", name="other")
    else:
        project = project.rename(tmp_path / "cell")

    with pytest.raises(ScenarioArtifactError) as refused:
        capture_scenario_artifact(str(project), allowed_roots=[str(tmp_path)])
    assert refused.value.code == (
        "scenario_artifact_invalid" if case == "old_id" else "scenario_project_name_invalid"
    )
    _directory, entries = discover_scenario_artifacts(
        str(tmp_path), allowed_roots=[str(tmp_path)], manyforge_version="1.0.0",
    )
    assert [(row["display_status"], row["scenario_id"]) for row in entries] == [
        ("unsupported", None),
    ]


@pytest.mark.parametrize("folder", ["cell", "other.scenario", "legacy.cell.scenario"])
def test_save_and_export_write_only_a_folder_named_by_the_scenario(
    tmp_path: Path, folder: str,
) -> None:
    roots = [str(tmp_path)]
    with pytest.raises(ScenarioArtifactError) as saved:
        save_scenario_artifact(
            _document("cell"), str(tmp_path / folder), allowed_roots=roots, overwrite=False,
        )
    assert saved.value.code == "scenario_project_name_invalid"
    source = Path(save_scenario_artifact(
        _document("cell"), str(tmp_path / "cell.scenario"), allowed_roots=roots, overwrite=False,
    ))
    (tmp_path / "out").mkdir()
    with pytest.raises(ScenarioArtifactError) as exported:
        export_scenario_artifact(
            _document("cell"), str(tmp_path / "out" / folder),
            source_project_path=str(source), allowed_roots=roots,
        )
    assert exported.value.code == "scenario_project_name_invalid"
    assert list((tmp_path / "out").iterdir()) == []


def test_same_named_projects_in_different_folders_are_each_listed_in_their_own_directory(
    tmp_path: Path,
) -> None:
    """The index lists exactly one directory, non-recursively."""

    for parent in ("a", "b"):
        (tmp_path / parent).mkdir()
        (tmp_path / parent / "model.urdf").write_text('<robot name="fixture"/>\n')
        save_scenario_artifact(
            _document("pick"), str(tmp_path / parent / "pick.scenario"),
            allowed_roots=[str(tmp_path)], overwrite=False,
        )
    for parent in ("a", "b"):
        directory, entries = discover_scenario_artifacts(
            str(tmp_path / parent), allowed_roots=[str(tmp_path)], manyforge_version="1.0.0",
        )
        assert directory == str((tmp_path / parent).resolve())
        assert [(row["path"], row["folder_name"]) for row in entries] == [
            (str((tmp_path / parent / "pick.scenario").resolve()), "pick"),
        ]
    _directory, root_entries = discover_scenario_artifacts(
        str(tmp_path), allowed_roots=[str(tmp_path)], manyforge_version="1.0.0",
    )
    assert root_entries == []


def test_index_names_entries_by_folder_and_marks_misnamed_folders(tmp_path: Path) -> None:
    cells = tmp_path / "lab" / "cells"
    cells.mkdir(parents=True)
    (cells / "model.urdf").write_text('<robot name="fixture"/>\n')
    for name in ("pick", "other"):
        save_scenario_artifact(
            _document(name), str(cells / f"{name}.scenario"),
            allowed_roots=[str(tmp_path)], overwrite=False,
        )
    shutil.copytree(cells / "pick.scenario", cells / "pick")

    _directory, entries = discover_scenario_artifacts(
        str(cells), allowed_roots=[str(tmp_path)], manyforge_version="1.0.0",
    )

    assert [
        (Path(row["path"]).name, row["folder_name"], row["location"], row["display_status"])
        for row in entries
    ] == [
        ("other.scenario", "other", "lab/cells", "ready"),
        ("pick", "pick", "lab/cells", "unsupported"),
        ("pick.scenario", "pick", "lab/cells", "ready"),
    ]


def test_every_repository_example_lists_and_loads_by_its_folder_name() -> None:
    root = Path(__file__).resolve().parents[2]
    examples = root / "examples"
    _directory, entries = discover_scenario_artifacts(
        str(examples), allowed_roots=[str(root)], manyforge_version="development",
    )
    projects = sorted(path for path in examples.glob("*.scenario") if path.is_dir())
    assert projects
    assert sorted(row["path"] for row in entries) == [str(path) for path in projects]
    for row in entries:
        assert row["display_status"] != "unsupported", row
        assert row["folder_name"] == Path(row["path"]).name[: -len(".scenario")]
        assert row["location"] == "examples"
        capture_scenario_artifact(row["path"], allowed_roots=[str(root)])
    names = [row["folder_name"] for row in entries]
    assert len(set(names)) == len(names)
    assert {"mock_ur5_pick_and_place", "mock_ur5_pick_and_place_minimal"} <= set(names)


def _gripper_closes(nodes: list[dict]) -> list[dict]:
    found = []
    for node in nodes:
        parameters = node.get("parameters", {})
        if (
            node.get("node_id") == "gripper.command_position"
            and parameters.get("position", {}).get("name") == "closed"
        ):
            found.append(parameters)
        found.extend(_gripper_closes(node.get("children", [])))
    return found


def test_grasp_confirmed_example_requires_confirmation_on_its_mounted_tool() -> None:
    # The split-form grasp confirmation example is mock_lite6_pneumatic_pick_and_place with
    # the policy on the tool's gripper row and no Close required inputs.
    root = Path(__file__).resolve().parents[2]
    examples = root / "examples"
    _directory, entries = discover_scenario_artifacts(
        str(examples), allowed_roots=[str(root)], manyforge_version="development",
    )
    rows = {row["folder_name"]: row for row in entries}
    row = rows["mock_lite6_pneumatic_grasp_confirmation"]
    assert row["display_status"] != "unsupported", row
    document, _ = load_scenario_artifact(row["path"], allowed_roots=[str(root)])
    base, _ = load_scenario_artifact(
        rows["mock_lite6_pneumatic_pick_and_place"]["path"], allowed_roots=[str(root)]
    )

    deployment = document["deployment_draft"]
    components = {item["component_id"]: item for item in deployment["components"]}
    tool = components["lite_gripper"]
    assert tool["component_profile_id"] == "ufactory.lite_gripper"
    assert "root_pose_in_cell" not in tool
    # Mount collision exclusions are checked by the mount-exclusion tests.
    assert [
        {key: value for key, value in row.items() if key != "collision_exclusions"}
        for row in deployment["attachments"]
    ] == [
        {
            "child": {"component_id": "lite_gripper", "interface": "mount"},
            "parent": {"component_id": "lite6_robot", "interface": "flange"},
        }
    ]
    assert "gripper_target_configurations" not in (
        components["lite6_robot"]["configuration"]["runtime_configuration"]
    )
    (gripper,) = tool["configuration"]["runtime_configuration"][
        "gripper_target_configurations"
    ]
    assert gripper["controllable_component_id"] == "lite_gripper"
    assert gripper["grasp_confirmation"] == {"policy": "required"}
    assert gripper["pneumatic_io"]["inputs"]["workpiece_grasped"]["signal"] == {
        "controllable_component_id": "gripper_io",
        "signal_name": "workpiece_grasped",
    }
    # Apart from the policy and its names, the cell is the plain Mock example.
    expected = deepcopy(base["deployment_draft"])
    expected_tool = next(
        item for item in expected["components"] if item["component_id"] == "lite_gripper"
    )
    expected_tool["configuration"]["runtime_configuration"][
        "gripper_target_configurations"
    ][0]["grasp_confirmation"] = {"policy": "required"}
    for field in ("deployment_artifact_id", "name", "description"):
        expected[field] = deployment[field]
    assert deployment == expected

    (close,) = _gripper_closes(document["program_draft"]["forest"])
    assert "required_inputs" not in close
    (base_close,) = _gripper_closes(base["program_draft"]["forest"])
    assert base_close["required_inputs"] == [{"input": "workpiece_grasped", "value": True}]
