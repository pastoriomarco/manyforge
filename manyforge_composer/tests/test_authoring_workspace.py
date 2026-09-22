"""Focused tests for the composite in-memory authoring workspace owner.

These tests exercise the owner's own contract only: composite identity, single
revision advance per real commit, closed snapshot shape, whole-workspace
replacement, stale rejection, revision exhaustion, observer ordering and
concurrent Program/Deployment commits. The composed sessions are deterministic
doubles so the revision and lock semantics are provable without models,
derivation, disk or network.

Real-session and route integration is covered by the UA2D.5/UA2D.7 route and
control-stream tests.
"""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from copy import deepcopy
import threading
from typing import Any

from fastapi.testclient import TestClient
import pytest

from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.authoring_workspace import (
    AUTHORING_WORKSPACE_MAXIMUM_REVISION,
    SNAPSHOT_API_VERSION,
    AuthoringWorkspace,
    AuthoringWorkspaceError,
    AuthoringWorkspaceIdentity,
    AuthoringWorkspaceSnapshot,
)
from manyforge_composer.backend.state import ComposerState


class _ProgramSession:
    """Deterministic stand-in for ComposerDraftSession's composed surface."""

    def __init__(self, *, draft_id: str = "program-draft-1") -> None:
        self._draft_id = draft_id
        self._revision = 0
        self._authoring: dict[str, Any] | None = None
        self._program: dict[str, Any] | None = None
        self._diagnostics: list[dict[str, str]] = []
        self._saved_path: str | None = None
        self._saved_program_revision: str | None = None
        self._saved_document: dict[str, Any] | None = None

    @property
    def draft_id(self) -> str:
        return self._draft_id

    @property
    def revision(self) -> int:
        return self._revision

    def retained_state(self) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.composer_retained_draft.v2",
            "draft_id": self._draft_id,
            "draft_revision": self._revision,
            "authoring": deepcopy(self._authoring),
            "program": deepcopy(self._program),
            "diagnostics": deepcopy(self._diagnostics),
            "saved_path": self._saved_path,
            "saved_program_revision": self._saved_program_revision,
            "saved_document": deepcopy(self._saved_document),
        }

    def install_program(self, program_revision: str) -> bool:
        """Emulate one accepted ordinary Program Draft mutation."""

        self._authoring = {"forest": [{"instance_id": program_revision}]}
        self._program = {"program_revision": program_revision}
        self._diagnostics = []
        self._revision += 1
        return True

    def install_no_op(self) -> bool:
        """Emulate a validated mutation that changed nothing."""

        return False

    def rotate_draft(self, draft_id: str) -> bool:
        """Emulate a whole-workspace replacement identity rotation."""

        self._draft_id = draft_id
        self._revision += 1
        return True

    def replace_with(self, draft_id: str, program_revision: str) -> None:
        self._draft_id = draft_id
        self._revision = 1
        self._authoring = {"forest": [{"instance_id": program_revision}]}
        self._program = {"program_revision": program_revision}
        self._diagnostics = []
        self._saved_path = None
        self._saved_program_revision = None
        self._saved_document = None


class _DeploymentSession:
    """Deterministic stand-in for DeploymentDraftSession's composed surface."""

    def __init__(self, *, draft_id: str = "deployment-draft-1") -> None:
        self._draft_id = draft_id
        self._revision = 0
        self._document: dict[str, Any] | None = None
        self._saved_path: str | None = None
        self._saved_document: dict[str, Any] | None = None

    @property
    def draft_id(self) -> str:
        return self._draft_id

    @property
    def revision(self) -> int:
        return self._revision

    def retained_state(self) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.composer_retained_deployment_draft.v1",
            "draft_id": self._draft_id,
            "draft_revision": self._revision,
            "document": deepcopy(self._document),
            "saved_path": self._saved_path,
            "saved_document": deepcopy(self._saved_document),
        }

    def install_document(self, revision: str) -> bool:
        self._document = {"deployment_revision": revision}
        self._revision += 1
        return True

    def replace_with(self, draft_id: str, revision: str) -> None:
        self._draft_id = draft_id
        self._revision = 1
        self._document = {"deployment_revision": revision}
        self._saved_path = None
        self._saved_document = None


def _workspace() -> tuple[AuthoringWorkspace, _ProgramSession, _DeploymentSession]:
    program = _ProgramSession()
    deployment = _DeploymentSession()
    workspace = AuthoringWorkspace(
        program_session=program,
        deployment_session=deployment,
    )
    return workspace, program, deployment


def _project_document() -> dict[str, Any]:
    return {
        "scenario_id": "cell-a",
        "name": "Cell A",
        "description": "assembly",
        "program_role_assignment": {"roles": ["picker"]},
        "typed_parameter_overrides": {"speed": 1},
        "program_draft": {"forest": []},
        "deployment_draft": {"components": []},
    }


def test_snapshot_shape_is_closed_and_carries_the_composite_identity() -> None:
    workspace, program, _deployment = _workspace()
    program.install_program("rev-1")

    captured = workspace.snapshot()

    assert set(captured.as_dict()) == {
        "schemaVersion",
        "workspaceEpoch",
        "workspaceRevision",
        "program",
        "deployment",
        "project",
    }
    payload = captured.as_dict()
    assert payload["schemaVersion"] == SNAPSHOT_API_VERSION
    assert payload["workspaceEpoch"] == workspace.epoch
    assert payload["workspaceRevision"] == workspace.revision == 1
    assert set(payload["program"]) == {
        "draftId",
        "draftRevision",
        "programLoaded",
        "authoring",
        "candidateAvailable",
        "programRevision",
        "program",
        "diagnostics",
        "saved",
    }
    assert set(payload["deployment"]) == {
        "draftId",
        "draftRevision",
        "document",
        "savedPath",
        "savedDocument",
        "differsFromSaved",
    }
    assert set(payload["project"]) == {
        "document_path",
        "document_id",
        "document_name",
        "document_description",
        "source_diagnostics",
        "source_document_digest",
        "program_role_assignment",
        "typed_parameter_overrides",
        "source_program_digest",
        "source_deployment_digest",
    }
    assert captured.identity == AuthoringWorkspaceIdentity(
        workspace_epoch=workspace.epoch,
        workspace_revision=1,
        program_draft_id="program-draft-1",
        program_draft_revision=1,
        deployment_draft_id="deployment-draft-1",
        deployment_draft_revision=0,
    )
    # No runtime/Applied/Manager truth leaks into the composite projection.
    assert "applied" not in payload["program"]
    assert "applicability" not in payload["program"]


def test_snapshot_rejects_an_added_or_missing_member() -> None:
    workspace, _program, _deployment = _workspace()
    captured = workspace.snapshot()

    with pytest.raises(ValueError):
        AuthoringWorkspaceSnapshot(
            workspace_epoch=captured.workspace_epoch,
            workspace_revision=captured.workspace_revision,
            program={**captured.program, "applied": {}},
            deployment=captured.deployment,
            project=captured.project,
        )

    with pytest.raises(ValueError):
        AuthoringWorkspaceSnapshot(
            workspace_epoch=captured.workspace_epoch,
            workspace_revision=captured.workspace_revision,
            program=captured.program,
            deployment=captured.deployment,
            project={
                key: value
                for key, value in captured.project.items()
                if key != "document_path"
            },
        )


def test_one_program_commit_advances_the_composite_revision_once() -> None:
    workspace, program, _deployment = _workspace()
    before = workspace.revision

    captured = workspace.commit_program(
        draft_id=program.draft_id,
        install=lambda: program.install_program("rev-1"),
    )

    assert workspace.revision == before + 1
    assert captured.workspace_revision == before + 1
    assert captured.program["programRevision"] == "rev-1"


def test_validated_no_op_does_not_advance_the_revision() -> None:
    workspace, program, _deployment = _workspace()
    before = workspace.revision

    captured = workspace.commit_program(
        draft_id=program.draft_id,
        install=program.install_no_op,
    )

    assert workspace.revision == before
    assert captured.workspace_revision == before
    assert captured.program["programLoaded"] is False


def test_deployment_commit_advances_once_and_leaves_the_program_fence_alone() -> None:
    workspace, program, deployment = _workspace()
    program.install_program("rev-1")
    baseline = workspace.snapshot()

    captured = workspace.commit_deployment(
        draft_id=deployment.draft_id,
        expected_identity=baseline.identity,
        install=lambda: deployment.install_document("dep-1"),
    )

    assert captured.workspace_revision == baseline.workspace_revision + 1
    assert captured.deployment["document"] == {"deployment_revision": "dep-1"}
    assert captured.program["programRevision"] == "rev-1"


def test_simultaneous_program_and_deployment_commits_each_advance_once() -> None:
    workspace, program, deployment = _workspace()
    before = workspace.revision
    start = threading.Event()

    def commit_program() -> None:
        start.wait(timeout=5)
        workspace.commit_program(
            draft_id=program.draft_id,
            install=lambda: program.install_program("rev-1"),
        )

    def commit_deployment() -> None:
        start.wait(timeout=5)
        workspace.commit_deployment(
            draft_id=deployment.draft_id,
            install=lambda: deployment.install_document("dep-1"),
        )

    with ThreadPoolExecutor(max_workers=2) as pool:
        futures = [pool.submit(commit_program), pool.submit(commit_deployment)]
        start.set()
        for future in futures:
            future.result(timeout=10)

    captured = workspace.snapshot()
    assert captured.workspace_revision == before + 2
    # No lost update: both committed domains are visible in one snapshot.
    assert captured.program["programRevision"] == "rev-1"
    assert captured.deployment["document"] == {"deployment_revision": "dep-1"}


def test_simultaneous_edits_from_one_identity_accept_exactly_one() -> None:
    workspace, program, deployment = _workspace()
    baseline = workspace.identity()
    start = threading.Barrier(2)

    def commit_program() -> str:
        start.wait(timeout=5)
        workspace.commit_program(
            draft_id=program.draft_id,
            expected_identity=baseline,
            install=lambda: program.install_program("rev-1"),
        )
        return "program"

    def commit_deployment() -> str:
        start.wait(timeout=5)
        workspace.commit_deployment(
            draft_id=deployment.draft_id,
            expected_identity=baseline,
            install=lambda: deployment.install_document("dep-1"),
        )
        return "deployment"

    accepted: list[str] = []
    rejected: list[str] = []
    with ThreadPoolExecutor(max_workers=2) as pool:
        futures = [pool.submit(commit_program), pool.submit(commit_deployment)]
        for future in futures:
            try:
                accepted.append(future.result(timeout=10))
            except AuthoringWorkspaceError as error:
                rejected.append(error.code)

    assert len(accepted) == 1
    assert rejected == ["composer_workspace_stale"]
    assert workspace.revision == baseline.workspace_revision + 1


def test_new_backend_workspace_does_not_restore_unsaved_edits() -> None:
    workspace, program, _deployment = _workspace()
    workspace.commit_program(
        draft_id=program.draft_id,
        install=lambda: program.install_program("unsaved"),
    )
    assert workspace.snapshot().program["programRevision"] == "unsaved"

    # A clean backend restart creates a new in-memory owner. Saved project
    # files may be opened explicitly, but an unsaved predecessor post-image is
    # deliberately not replayed.
    successor, _successor_program, _successor_deployment = _workspace()
    captured = successor.snapshot()
    assert captured.program["programLoaded"] is False
    assert captured.program["programRevision"] is None
    assert captured.workspace_epoch != workspace.epoch


def test_whole_workspace_replacement_rotates_both_identities_in_one_advance() -> None:
    workspace, program, deployment = _workspace()
    program.install_program("rev-1")
    baseline = workspace.snapshot()

    def install() -> bool:
        program.replace_with("program-draft-2", "rev-2")
        deployment.replace_with("deployment-draft-2", "dep-2")
        return True

    captured = workspace.replace(
        expected_identity=baseline.identity,
        install=install,
        project={
            "document_path": "/tmp/cell-b",
            "document_id": "cell-b",
            "document_name": "Cell B",
            "document_description": "",
            "source_diagnostics": [],
            "source_document_digest": "sha256:bb",
            "program_role_assignment": {"roles": ["placer"]},
            "typed_parameter_overrides": {},
            "source_program_digest": "sha256:cc",
            "source_deployment_digest": "sha256:dd",
        },
    )

    assert captured.workspace_revision == baseline.workspace_revision + 1
    assert captured.program["draftId"] == "program-draft-2"
    assert captured.deployment["draftId"] == "deployment-draft-2"
    assert captured.project["document_path"] == "/tmp/cell-b"
    assert captured.identity.program_draft_id == "program-draft-2"
    assert captured.identity.deployment_draft_id == "deployment-draft-2"


def test_invalid_replacement_project_is_rejected_before_install() -> None:
    workspace, program, _deployment = _workspace()
    installed = False

    def install() -> bool:
        nonlocal installed
        installed = True
        return program.install_program("must-not-install")

    with pytest.raises(AuthoringWorkspaceError) as raised:
        workspace.replace(install=install, project={"document_id": "partial"})

    assert raised.value.code == "composer_workspace_project_invalid"
    assert installed is False
    assert workspace.snapshot().program["programRevision"] is None


def test_stale_composite_identity_is_rejected_with_the_current_identity() -> None:
    workspace, program, _deployment = _workspace()
    stale = workspace.identity()
    program.install_program("rev-1")
    workspace.commit_program(
        draft_id=program.draft_id,
        install=lambda: program.install_program("rev-2"),
    )
    current = workspace.identity()

    with pytest.raises(AuthoringWorkspaceError) as raised:
        workspace.commit_program(
            draft_id=program.draft_id,
            expected_identity=stale,
            install=lambda: program.install_program("rev-3"),
        )

    error = raised.value
    assert error.code == "composer_workspace_stale"
    assert error.status_code == 409
    assert error.detail["workspaceEpoch"] == current.workspace_epoch
    assert error.detail["workspaceRevision"] == current.workspace_revision
    # The refused commit changed nothing.
    assert workspace.identity() == current
    assert workspace.snapshot().program["programRevision"] == "rev-2"


def test_stale_epoch_is_rejected_even_at_the_same_revision() -> None:
    workspace, program, _deployment = _workspace()
    other = AuthoringWorkspaceIdentity(
        workspace_epoch="workspace-someone-else",
        workspace_revision=workspace.revision,
        program_draft_id=program.draft_id,
        program_draft_revision=program.revision,
        deployment_draft_id="deployment-draft-1",
        deployment_draft_revision=0,
    )

    with pytest.raises(AuthoringWorkspaceError) as raised:
        workspace.commit_program(
            draft_id=program.draft_id,
            expected_identity=other,
            install=lambda: program.install_program("rev-1"),
        )

    assert raised.value.code == "composer_workspace_stale"
    assert workspace.revision == other.workspace_revision
    assert program.retained_state()["program"] is None


def test_revision_exhaustion_fails_closed_before_any_install() -> None:
    workspace, program, _deployment = _workspace()
    program.install_program("rev-1")
    workspace._revision = AUTHORING_WORKSPACE_MAXIMUM_REVISION
    before = program.draft_id, program.revision, program.retained_state()

    with pytest.raises(AuthoringWorkspaceError) as raised:
        workspace.commit_program(
            draft_id=program.draft_id,
            install=lambda: program.install_program("rev-2"),
        )

    assert raised.value.code == "composer_workspace_revision_exhausted"
    assert raised.value.status_code == 503
    # Fails closed without wrapping and without installing anything.
    assert workspace.revision == AUTHORING_WORKSPACE_MAXIMUM_REVISION
    assert (program.draft_id, program.revision, program.retained_state()) == before


def test_ordinary_commit_cannot_rotate_the_domain_identity() -> None:
    workspace, program, _deployment = _workspace()
    original = program.draft_id

    with pytest.raises(AuthoringWorkspaceError) as raised:
        workspace.commit_program(
            draft_id=original,
            install=lambda: program.rotate_draft("program-draft-2"),
        )

    assert raised.value.code == "composer_workspace_domain_identity_changed"
    assert raised.value.status_code == 503
    assert raised.value.detail == {
        "session": "program",
        "expectedDraftId": original,
        "currentDraftId": "program-draft-2",
    }
    # The workspace is contained rather than committing or retrying.
    with pytest.raises(AuthoringWorkspaceError) as contained:
        workspace.commit_program(
            draft_id="program-draft-2",
            install=program.install_no_op,
        )
    assert contained.value.code == "composer_workspace_failed_closed"


def test_domain_identity_mismatch_is_refused_before_the_install() -> None:
    workspace, program, _deployment = _workspace()
    before = program.retained_state()

    with pytest.raises(AuthoringWorkspaceError) as raised:
        workspace.commit_program(
            draft_id="program-draft-stale",
            install=lambda: program.install_program("rev-1"),
        )

    assert raised.value.code == "composer_workspace_domain_identity_changed"
    assert program.retained_state() == before


def test_observer_runs_once_after_every_lock_is_released() -> None:
    workspace, program, _deployment = _workspace()
    observed: list[int] = []
    lock_free_from_other_thread: list[bool] = []

    def probe_lock() -> bool:
        lock = workspace._lock
        acquired = lock.acquire(blocking=False)
        if acquired:
            lock.release()
        return acquired

    def observer() -> None:
        # Reading the committed identity proves the commit finished first.
        observed.append(workspace.identity().workspace_revision)
        with ThreadPoolExecutor(max_workers=1) as pool:
            lock_free_from_other_thread.append(pool.submit(probe_lock).result())

    workspace.set_observer(observer)

    captured = workspace.commit_program(
        draft_id=program.draft_id,
        install=lambda: program.install_program("rev-1"),
    )

    assert observed == [captured.workspace_revision]
    assert lock_free_from_other_thread == [True]

    # A validated no-op produces no wake at all.
    workspace.commit_program(draft_id=program.draft_id, install=program.install_no_op)
    assert observed == [captured.workspace_revision]


def test_project_facts_commit_advances_once_and_a_repeat_does_not() -> None:
    workspace, _program, _deployment = _workspace()
    before = workspace.revision

    captured = workspace.remember_project(
        document=_project_document(),
        document_path="/tmp/cell-a",
        source_document_digest="sha256:aa",
        source_diagnostics=[],
    )
    assert captured.workspace_revision == before + 1
    assert captured.project["document_id"] == "cell-a"
    assert captured.project["document_path"] == "/tmp/cell-a"
    assert captured.as_dict()["project"]["program_role_assignment"] == {
        "roles": ["picker"]
    }
    assert captured.project["source_program_digest"].startswith("sha256:")
    assert captured.project["source_deployment_digest"].startswith("sha256:")

    repeated = workspace.remember_project(
        document=_project_document(),
        document_path="/tmp/cell-a",
        source_document_digest="sha256:aa",
        source_diagnostics=[],
    )
    assert repeated.workspace_revision == captured.workspace_revision

    cleared = workspace.remember_project(document=None)
    assert cleared.project["document_id"] is None
    assert cleared.project["document_path"] == "/tmp/cell-a"
    assert cleared.workspace_revision == captured.workspace_revision + 1


def test_migrated_project_fact_write_through_is_one_advance_per_field() -> None:
    workspace, _program, _deployment = _workspace()
    before = workspace.revision

    workspace.migrate_project_facts({"document_path": "/tmp/cell-a"})
    assert workspace.revision == before + 1
    assert workspace.project_facts()["document_path"] == "/tmp/cell-a"

    # An unchanged repeat is a validated no-op.
    workspace.migrate_project_facts({"document_path": "/tmp/cell-a"})
    assert workspace.revision == before + 1

    with pytest.raises(ValueError):
        workspace.migrate_project_facts({"not_a_fact": 1})
    with pytest.raises(ValueError):
        workspace.migrate_project_facts({})


def test_snapshot_copies_are_isolated_from_later_commits() -> None:
    workspace, program, _deployment = _workspace()
    program.install_program("rev-1")
    captured = workspace.snapshot()

    workspace.commit_program(
        draft_id=program.draft_id,
        install=lambda: program.install_program("rev-2"),
    )

    assert captured.program["programRevision"] == "rev-1"
    assert captured.workspace_revision == 1
    assert workspace.snapshot().program["programRevision"] == "rev-2"


def test_snapshot_payload_is_immutable_and_as_dict_returns_a_copy() -> None:
    workspace, program, _deployment = _workspace()
    program.install_program("rev-1")
    captured = workspace.snapshot()

    with pytest.raises(TypeError):
        captured.program["programRevision"] = "forged"  # type: ignore[index]

    payload = captured.as_dict()
    payload["program"]["programRevision"] = "forged"
    assert captured.program["programRevision"] == "rev-1"


def test_exact_identity_rejects_changed_domain_fence_at_same_workspace_revision() -> None:
    workspace, program, _deployment = _workspace()
    stale = workspace.identity()
    program.install_program("rev-outside-owner")

    with pytest.raises(AuthoringWorkspaceError) as raised:
        workspace.require_identity(stale)

    assert raised.value.code == "composer_workspace_stale"


def test_failed_install_contains_and_hides_the_workspace() -> None:
    workspace, program, _deployment = _workspace()

    def fail_after_change() -> bool:
        program.install_program("partial")
        raise RuntimeError("injected install failure")

    with pytest.raises(AuthoringWorkspaceError) as raised:
        workspace.commit_program(
            draft_id=program.draft_id,
            install=fail_after_change,
        )

    assert raised.value.code == "composer_workspace_install_failed"
    with pytest.raises(AuthoringWorkspaceError) as contained:
        workspace.snapshot()
    assert contained.value.code == "composer_workspace_failed_closed"


def test_false_change_result_cannot_hide_a_domain_change() -> None:
    workspace, program, _deployment = _workspace()

    def change_but_report_no_op() -> bool:
        program.install_program("hidden")
        return False

    with pytest.raises(AuthoringWorkspaceError) as raised:
        workspace.commit_program(
            draft_id=program.draft_id,
            install=change_but_report_no_op,
        )

    assert raised.value.code == "composer_workspace_install_scope_invalid"
    with pytest.raises(AuthoringWorkspaceError):
        workspace.snapshot()


def test_program_install_cannot_change_the_deployment_domain() -> None:
    workspace, program, deployment = _workspace()

    with pytest.raises(AuthoringWorkspaceError) as raised:
        workspace.commit_program(
            draft_id=program.draft_id,
            install=lambda: deployment.install_document("wrong-domain"),
        )

    assert raised.value.code == "composer_workspace_install_scope_invalid"


def test_workspace_route_returns_the_single_owner_snapshot() -> None:
    state = ComposerState()
    response = TestClient(create_app(state)).get("/api/authoring/workspace")

    assert response.status_code == 200
    assert response.json() == state.authoring_workspace.snapshot().as_dict()
