"""Focused tests for the local (non-durable) Composer authoring mutation path.

Covered here: the fixed admission sequence, the single deferred wake, proven
no-effect settlement for a refusal, containment and recovery of an uncertain
settlement, and the whole-workspace replacement session. These tests use
deterministic doubles; route-level integration is owned by the route migration.
"""

from __future__ import annotations

from copy import deepcopy
from dataclasses import replace
from typing import Any
from types import SimpleNamespace

import pytest
from fastapi import FastAPI, HTTPException
from fastapi.testclient import TestClient
from manyforge_headless_api.client import HeadlessClientError

from manyforge_composer.backend.authoring_admission import (
    AuthoringAdmissionCoordinator,
    AuthoringMutationEnvelope,
)
from manyforge_composer.backend.authoring_workspace import (
    AuthoringWorkspace,
    AuthoringWorkspaceError,
    AuthoringWorkspaceIdentity,
    scenario_project_facts,
)
from manyforge_composer.backend.routes_authoring_workspace import create_authoring_workspace_router


class _ProgramSession:
    """Minimal composed Program session double for the workspace owner."""

    def __init__(self, draft_id: str = "program-draft-1") -> None:
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

    def install(self, program_revision: str) -> bool:
        """Emulate one accepted ordinary Program Draft mutation."""

        self._authoring = {"forest": [{"instance_id": program_revision}]}
        self._program = {"program_revision": program_revision}
        self._diagnostics = []
        self._revision += 1
        return True

    def install_no_op(self) -> bool:
        """Emulate a validated mutation that changed nothing."""

        return False

    def replace_with(self, draft_id: str, program_revision: str) -> bool:
        """Emulate a whole-Draft replacement that rotates the identity."""

        self._draft_id = draft_id
        self._revision = 1
        self._authoring = {"forest": [{"instance_id": program_revision}]}
        self._program = {"program_revision": program_revision}
        self._diagnostics = []
        self._saved_path = None
        self._saved_program_revision = None
        self._saved_document = None
        return True


class _DeploymentSession:
    """Minimal composed Deployment session double for the workspace owner."""

    def __init__(self, draft_id: str = "deployment-draft-1") -> None:
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


class _Headless:
    """Record the exact claim protocol the local path must use."""

    def __init__(self, *, generation: int = 7, allowed: bool = True) -> None:
        self.generation = generation
        self.allowed = allowed
        self.events: list[str] = []
        self.claims: list[dict[str, Any]] = []
        self.settlements: list[dict[str, Any]] = []
        self.settle_error: Exception | None = None
        self.acquire_error: Exception | None = None
        self.reconcile_error: Exception | None = None
        self.recovery_receipt: dict[str, Any] = {
            "status": "unknown",
            "kernel_owner_epoch": "kernel.1",
            "held_claims": [],
            "current_owner_epochs": [],
        }
        self.reconciliations: list[dict[str, Any]] = []

    def authoring_admission(self) -> dict[str, Any]:
        return {
            "authoring_allowed": self.allowed,
            "admission_generation": self.generation,
        }

    def acquire_authoring_transaction(
        self, body: dict[str, Any]
    ) -> dict[str, Any]:
        if self.acquire_error is not None:
            raise self.acquire_error
        self.claims.append(body)
        self.events.append("acquire")
        return {
            "claim": {
                "intent": {
                    "claim_kind": "authoring_mutation",
                    "owner_role": "authoring_owner",
                },
                "claim_generation": 1,
                "acquired_admission_generation": self.generation,
                "accepted_at_ns": 1,
            }
        }

    def settle_authoring_transaction(
        self, body: dict[str, Any]
    ) -> dict[str, Any]:
        if self.settle_error is not None:
            raise self.settle_error
        self.settlements.append(body)
        self.events.append(f"settle:{body['settlement_kind']}")
        return {"admission": {"admission_generation": self.generation + 1}}

    def recover_authoring_transaction(
        self, _body: dict[str, Any]
    ) -> dict[str, Any]:
        return {"recovery_receipt": self.recovery_receipt}

    def reconcile_authoring_owner(
        self, body: dict[str, Any]
    ) -> dict[str, Any]:
        if self.reconcile_error is not None:
            raise self.reconcile_error
        self.reconciliations.append(body)
        return {"admission": {"admission_generation": self.generation}}


def _fixture(
    tmp_path, *, generation: int = 7, allowed: bool = True
) -> tuple[
    AuthoringWorkspace,
    _ProgramSession,
    _DeploymentSession,
    _Headless,
    AuthoringAdmissionCoordinator,
]:
    program = _ProgramSession()
    deployment = _DeploymentSession()
    workspace = AuthoringWorkspace(
        program_session=program,
        deployment_session=deployment,
    )
    headless = _Headless(generation=generation, allowed=allowed)
    coordinator = AuthoringAdmissionCoordinator(headless)
    coordinator.recover()
    assert coordinator.mutation_ready
    return workspace, program, deployment, headless, coordinator


def _envelope(generation: int = 7) -> AuthoringMutationEnvelope:
    return AuthoringMutationEnvelope(
        expected_admission_generation=generation,
        request_owner_epoch="browser.1",
        request_id="request.1",
    )


def _run_local(
    coordinator: AuthoringAdmissionCoordinator,
    workspace: AuthoringWorkspace,
    program: _ProgramSession,
    headless: _Headless,
    *,
    install=None,
    validate_under_claim=None,
    session: str = "program",
    expected_identity: AuthoringWorkspaceIdentity | None = None,
    envelope: AuthoringMutationEnvelope | None = None,
):
    workspace.set_observer(lambda: headless.events.append("wake"))

    def tracked_install() -> bool:
        headless.events.append("install")
        if install is None:
            return program.install("rev-1")
        return install()

    return coordinator.execute_local_mutation(
        envelope or _envelope(),
        operation_kind="mutate_draft",
        draft_id=program.draft_id,
        draft_revision=program.revision,
        payload={"mutation": "tree_draft_rename_node", "arguments": {}},
        workspace=workspace,
        install=tracked_install,
        validate_under_claim=validate_under_claim,
        session=session,
        expected_identity=expected_identity,
    )


def test_local_mutation_acquires_commits_settles_then_wakes_once(tmp_path) -> None:
    workspace, program, _deployment, headless, coordinator = _fixture(tmp_path)

    commit = _run_local(coordinator, workspace, program, headless)

    assert headless.events == [
        "acquire",
        "install",
        "settle:owner_commit",
        "wake",
    ]
    assert commit.snapshot.workspace_revision == 2
    assert commit.admission_generation == 8
    assert workspace.revision == 2
    assert len(headless.claims) == 1
    assert headless.settlements[0]["settlement_kind"] == "owner_commit"
    assert headless.settlements[0]["claim"]["claim_generation"] == 1
    assert headless.settlements[0]["evidence_digest"].startswith("sha256:")
    assert coordinator.local_claim_pending is False
    assert coordinator.mutation_ready is True


def test_validated_no_op_does_not_advance_or_wake(tmp_path) -> None:
    workspace, program, _deployment, headless, coordinator = _fixture(tmp_path)

    commit = _run_local(
        coordinator,
        workspace,
        program,
        headless,
        install=program.install_no_op,
    )

    assert headless.events == ["acquire", "install", "settle:owner_commit"]
    assert commit.snapshot.workspace_revision == 1
    assert commit.admission_generation == 8
    assert workspace.revision == 1

    # A settled no-op is provable even though the revision did not advance.
    assert coordinator.submission_outcome(_envelope())["outcome"] == {
        "kind": "settled", "operation_kind": "mutate_draft", "apply": None,
    }


def test_submission_readback_is_exact_read_only_and_bounded(tmp_path) -> None:
    workspace, program, _deployment, headless, coordinator = _fixture(tmp_path)
    _run_local(coordinator, workspace, program, headless)
    events = list(headless.events)
    assert coordinator.submission_outcome(_envelope())["outcome"]["kind"] == "settled"
    for changed in [replace(_envelope(), request_id="other"),
                    replace(_envelope(), request_owner_epoch="other"), _envelope(8)]:
        assert coordinator.submission_outcome(changed)["outcome"] is None
    assert AuthoringAdmissionCoordinator(headless).submission_outcome(_envelope())["outcome"] is None
    assert headless.events == events
    app = FastAPI()
    app.include_router(create_authoring_workspace_router(SimpleNamespace(authoring_admission=coordinator)))
    response = TestClient(app).get('/api/authoring/submission-outcome', params={
        'request_owner_epoch': 'browser.1', 'request_id': 'request.1', 'expected_admission_generation': 7,
    })
    assert response.status_code == 200
    assert response.json() == coordinator.submission_outcome(_envelope())
    assert headless.events == events
    for index in range(128):
        coordinator._record_outcome(replace(_envelope(), request_id=str(index)), "mutate_draft")
    assert coordinator.submission_outcome(_envelope())["outcome"] is None


@pytest.mark.parametrize("session", ["project", "whole-workspace"])
def test_under_claim_project_preparation_runs_between_acquire_and_commit(tmp_path, session) -> None:
    workspace, program, _deployment, headless, coordinator = _fixture(tmp_path)
    workspace.set_observer(lambda: headless.events.append("wake"))

    def prepare_project() -> dict[str, Any]:
        assert headless.events == ["acquire"]
        headless.events.append("prepare_project")
        return scenario_project_facts(
            {
                "scenario_id": "saved-scenario",
                "name": "Saved Scenario",
                "description": "",
                "program_draft": None,
                "deployment_draft": None,
                "program_role_assignment": None,
                "typed_parameter_overrides": {},
            },
            document_path="scenario.test",
            source_document_digest="sha256:" + "a" * 64,
        )

    def install() -> bool:
        headless.events.append("install")
        return program.install_no_op()

    commit = coordinator.execute_local_mutation(
        _envelope(),
        operation_kind="save_scenario_document",
        draft_id=program.draft_id,
        draft_revision=program.revision,
        payload={"path": "scenario.test"},
        workspace=workspace,
        session=session,
        install=install if session == "whole-workspace" else None,
        prepare_project_under_claim=prepare_project,
    )

    assert headless.events == [
        "acquire",
        "prepare_project",
        *(["install"] if session == "whole-workspace" else []),
        "settle:owner_commit",
        "wake",
    ]
    assert commit.snapshot.project["document_id"] == "saved-scenario"


@pytest.mark.parametrize("session", ["project", "whole-workspace"])
def test_stale_generation_refuses_before_under_claim_project_effect(tmp_path, session) -> None:
    workspace, program, _deployment, headless, coordinator = _fixture(tmp_path)
    headless.generation = 8
    effects: list[str] = []
    initial_revision = workspace.revision

    with pytest.raises(HTTPException) as raised:
        coordinator.execute_local_mutation(
            _envelope(),
            operation_kind="save_scenario_document",
            draft_id=program.draft_id,
            draft_revision=program.revision,
            payload={"path": "scenario.test"},
            workspace=workspace,
            session=session,
            install=program.install_no_op if session == "whole-workspace" else None,
            prepare_project_under_claim=lambda: effects.append("write")
            or scenario_project_facts(None),
        )

    assert raised.value.detail["code"] == "stale_generation"
    assert effects == []
    assert headless.claims == []
    assert workspace.revision == initial_revision
    assert coordinator.local_claim_pending is False


def test_refusal_settles_proven_no_effect_and_keeps_the_typed_error(
    tmp_path,
) -> None:
    workspace, program, _deployment, headless, coordinator = _fixture(tmp_path)
    # A stale composite identity is refused before the install can run.
    stale = replace(workspace.identity(), workspace_revision=999)

    with pytest.raises(AuthoringWorkspaceError) as raised:
        _run_local(
            coordinator,
            workspace,
            program,
            headless,
            expected_identity=stale,
        )

    assert raised.value.code == "composer_workspace_stale"
    assert headless.events == ["acquire", "settle:proven_no_effect"]
    assert headless.settlements[0]["settlement_kind"] == "proven_no_effect"
    assert workspace.revision == 1
    assert program.revision == 0
    assert coordinator.local_claim_pending is False
    assert coordinator.mutation_ready is True


def test_under_claim_validation_refusal_settles_without_install(tmp_path) -> None:
    workspace, program, _deployment, headless, coordinator = _fixture(tmp_path)

    def reject_changed_source() -> None:
        headless.events.append("validate")
        raise AuthoringWorkspaceError(
            "external_source_changed",
            "the prepared external source changed",
        )

    with pytest.raises(AuthoringWorkspaceError) as raised:
        _run_local(
            coordinator,
            workspace,
            program,
            headless,
            validate_under_claim=reject_changed_source,
        )

    assert raised.value.code == "external_source_changed"
    assert headless.events == ["acquire", "validate", "settle:proven_no_effect"]
    assert workspace.revision == 1
    assert program.revision == 0
    assert coordinator.local_claim_pending is False
    assert coordinator.mutation_ready is True


def test_uncertain_settlement_contains_authoring_without_rollback(
    tmp_path,
) -> None:
    workspace, program, _deployment, headless, coordinator = _fixture(tmp_path)
    headless.settle_error = HeadlessClientError(
        "admission_transport_failed", "the exact claim could not be settled"
    )

    with pytest.raises(HTTPException) as raised:
        _run_local(coordinator, workspace, program, headless)

    assert raised.value.status_code == 503
    assert raised.value.detail["code"] == "composer_authoring_release_uncertain"
    assert coordinator.submission_outcome(_envelope())["outcome"] is None
    # The committed in-memory edit is retained, not rolled back, and no wake
    # was published because the claim settlement is unresolved.
    assert workspace.revision == 2
    assert "wake" not in headless.events
    assert coordinator.local_claim_pending is True
    assert coordinator.mutation_ready is False


def test_recovery_settles_the_retained_local_claim(tmp_path) -> None:
    workspace, program, _deployment, headless, coordinator = _fixture(tmp_path)
    headless.settle_error = HeadlessClientError(
        "admission_transport_failed", "the exact claim could not be settled"
    )
    with pytest.raises(HTTPException):
        _run_local(coordinator, workspace, program, headless)

    headless.settle_error = None
    coordinator.recover()

    assert coordinator.local_claim_pending is False
    assert coordinator.mutation_ready is True
    assert headless.settlements[-1]["settlement_kind"] == "owner_commit"
    assert headless.events[-2:] == ["settle:owner_commit", "wake"]


def test_recovery_stays_closed_while_the_local_claim_is_unsettled(
    tmp_path,
) -> None:
    workspace, program, _deployment, headless, coordinator = _fixture(tmp_path)
    headless.settle_error = HeadlessClientError(
        "admission_transport_failed", "the exact claim could not be settled"
    )
    with pytest.raises(HTTPException):
        _run_local(coordinator, workspace, program, headless)

    with pytest.raises(HTTPException) as raised:
        coordinator.recover()

    assert raised.value.detail["code"] == "composer_authoring_release_uncertain"
    assert coordinator.local_claim_pending is True
    assert coordinator.mutation_ready is False


def test_next_local_mutation_settles_the_retained_claim_first(tmp_path) -> None:
    workspace, program, _deployment, headless, coordinator = _fixture(tmp_path)
    headless.settle_error = HeadlessClientError(
        "admission_transport_failed", "the exact claim could not be settled"
    )
    with pytest.raises(HTTPException):
        _run_local(coordinator, workspace, program, headless)

    # A settlement that keeps failing refuses the next edit before acquiring.
    headless.events.clear()
    headless.claims.clear()
    with pytest.raises(HTTPException) as raised:
        _run_local(coordinator, workspace, program, headless)
    assert raised.value.detail["code"] == "composer_authoring_release_uncertain"
    assert headless.claims == []
    assert headless.events == []


def test_unexpected_install_scope_violation_contains_the_workspace(
    tmp_path,
) -> None:
    workspace, program, _deployment, headless, coordinator = _fixture(tmp_path)

    with pytest.raises(AuthoringWorkspaceError) as raised:
        _run_local(
            coordinator,
            workspace,
            program,
            headless,
            install=lambda: program.replace_with("program-draft-9", "rev-9"),
        )

    assert raised.value.code == "composer_workspace_domain_identity_changed"
    # The install reported a change but rotated the fenced identity, so the
    # outcome is unknown: authoring is contained with the claim retained.
    assert workspace.failed_closed is not None
    assert coordinator.local_claim_pending is True
    assert coordinator.mutation_ready is False
    assert headless.settlements == []


def test_admission_refusal_takes_no_claim(tmp_path) -> None:
    workspace, program, _deployment, headless, coordinator = _fixture(
        tmp_path, allowed=False
    )

    with pytest.raises(HTTPException) as raised:
        _run_local(coordinator, workspace, program, headless)

    assert raised.value.status_code == 409
    assert raised.value.detail["code"] == "authoring_not_ready"
    assert headless.claims == []
    assert headless.settlements == []
    assert headless.events == []


def test_stale_admission_generation_takes_no_claim(tmp_path) -> None:
    workspace, program, _deployment, headless, coordinator = _fixture(tmp_path)

    with pytest.raises(HTTPException) as raised:
        _run_local(
            coordinator,
            workspace,
            program,
            headless,
            envelope=_envelope(generation=6),
        )

    assert raised.value.status_code == 409
    assert raised.value.detail["code"] == "stale_generation"
    assert headless.claims == []
    assert headless.events == []


def test_whole_workspace_session_uses_the_replacement_commit(tmp_path) -> None:
    workspace, program, _deployment, headless, coordinator = _fixture(tmp_path)

    commit = _run_local(
        coordinator,
        workspace,
        program,
        headless,
        session="whole-workspace",
        install=lambda: program.replace_with("program-draft-2", "rev-2"),
    )

    assert headless.events == [
        "acquire",
        "install",
        "settle:owner_commit",
        "wake",
    ]
    assert commit.snapshot.identity.program_draft_id == "program-draft-2"
    assert commit.snapshot.workspace_revision == 2
    assert coordinator.mutation_ready is True
    assert coordinator.local_claim_pending is False


def test_uncertain_acquire_retains_exact_request_and_recovers_no_effect(
    tmp_path,
) -> None:
    workspace, program, _deployment, headless, coordinator = _fixture(tmp_path)
    headless.acquire_error = HeadlessClientError(
        "headless_transport_error", "the exact claim outcome is unknown"
    )

    with pytest.raises(HTTPException) as raised:
        _run_local(coordinator, workspace, program, headless)

    assert raised.value.status_code == 503
    assert raised.value.detail["code"] == "headless_transport_error"
    assert coordinator.mutation_ready is False
    assert coordinator.local_claim_pending is True
    assert workspace.revision == 1

    headless.acquire_error = None
    coordinator.recover()

    assert coordinator.local_claim_pending is False
    assert coordinator.mutation_ready is True
    assert headless.settlements[-1]["settlement_kind"] == "proven_no_effect"
    assert "install" not in headless.events


def test_uncertain_no_effect_settlement_retains_its_disposition(tmp_path) -> None:
    workspace, program, _deployment, headless, coordinator = _fixture(tmp_path)
    stale = replace(workspace.identity(), workspace_revision=999)
    headless.settle_error = HeadlessClientError(
        "admission_transport_failed", "the no-effect release response was lost"
    )

    with pytest.raises(HTTPException):
        _run_local(
            coordinator,
            workspace,
            program,
            headless,
            expected_identity=stale,
        )

    assert coordinator.local_claim_pending is True
    assert workspace.revision == 1
    headless.settle_error = None
    coordinator.recover()

    assert coordinator.local_claim_pending is False
    assert coordinator.mutation_ready is True
    assert headless.settlements[-1]["settlement_kind"] == "proven_no_effect"
    assert "wake" not in headless.events


def test_unsupported_session_and_payload_are_rejected(tmp_path) -> None:
    workspace, program, _deployment, headless, coordinator = _fixture(tmp_path)

    with pytest.raises(ValueError):
        coordinator.execute_local_mutation(
            _envelope(),
            operation_kind="mutate_draft",
            draft_id=program.draft_id,
            draft_revision=program.revision,
            payload={},
            workspace=workspace,
            install=program.install_no_op,
            session="unexpected",
        )

    assert headless.claims == []
    assert headless.events == []


def test_startup_rotates_only_authoring_owner_and_retains_exact_claims() -> None:
    headless = _Headless()
    retained_claim = {"claim_generation": 4, "intent": {"owner_role": "authoring_owner"}}
    headless.recovery_receipt = {
        "status": "unknown",
        "kernel_owner_epoch": "kernel.7",
        "held_claims": [retained_claim],
        "current_owner_epochs": [
            {"owner_role": "program_runner", "owner_epoch": "runner.2"},
            {"owner_role": "authoring_owner", "owner_epoch": "composer.old"},
            {"owner_role": "manager", "owner_epoch": "manager.3"},
        ],
    }
    coordinator = AuthoringAdmissionCoordinator(headless)

    coordinator.recover()

    assert len(headless.reconciliations) == 1
    request = headless.reconciliations[0]
    assert request["kernel_owner_epoch"] == "kernel.7"
    assert request["retained_claims"] == [retained_claim]
    assert request["current_owner_epochs"] == [
        {"owner_role": "program_runner", "owner_epoch": "runner.2"},
        {"owner_role": "authoring_owner", "owner_epoch": coordinator.owner_epoch},
        {"owner_role": "manager", "owner_epoch": "manager.3"},
    ]
    assert request["reconciliation_evidence_digest"].startswith("sha256:")
    assert coordinator.mutation_ready is True


def test_startup_reconciliation_failure_keeps_authoring_contained() -> None:
    headless = _Headless()
    headless.reconcile_error = HeadlessClientError(
        "authoring_reconciliation_rejected", "predecessor effect is unresolved"
    )
    coordinator = AuthoringAdmissionCoordinator(headless)

    with pytest.raises(HTTPException) as raised:
        coordinator.recover()

    assert raised.value.status_code == 503
    assert raised.value.detail["code"] == "authoring_reconciliation_rejected"
    assert coordinator.mutation_ready is False


def test_external_submission_transfers_claim_after_manager_acceptance(tmp_path) -> None:
    _workspace, program, _deployment, headless, coordinator = _fixture(tmp_path)
    submitted: list[dict[str, Any]] = []

    result = coordinator.execute_external_submission(
        _envelope(),
        operation_kind="apply_program",
        draft_id=program.draft_id,
        draft_revision=program.revision,
        payload={"prepared_activation_id": "sha256:" + "1" * 64},
        validate_under_claim=lambda _claim: None,
        submit=lambda claim: submitted.append(claim) or {
            "status": "accepted",
            "state": "pending",
            "apply_id": "apply.1",
            "apply_generation": 1,
        },
    )

    assert result["state"] == "pending"
    assert coordinator.submission_outcome(_envelope())["outcome"] == {
        "kind": "manager_accepted", "operation_kind": "apply_program",
        "apply": {"apply_id": "apply.1", "apply_generation": 1},
    }
    assert len(submitted) == 1
    assert headless.settlements == []
    assert coordinator.local_claim_pending is False


def test_external_submission_contains_unclassified_owner_response(tmp_path) -> None:
    _workspace, program, _deployment, headless, coordinator = _fixture(tmp_path)

    with pytest.raises(HTTPException) as raised:
        coordinator.execute_external_submission(
            _envelope(),
            operation_kind="apply_program",
            draft_id=program.draft_id,
            draft_revision=program.revision,
            payload={},
            validate_under_claim=lambda _claim: None,
            submit=lambda _claim: {"status": "accepted", "apply_id": "apply.1"},
        )

    assert raised.value.detail["code"] == "manager_apply_acceptance_invalid"
    assert coordinator.submission_outcome(_envelope())["outcome"] is None
    assert coordinator.mutation_ready is False
    assert coordinator.local_claim_pending is True
    assert headless.settlements == []


@pytest.mark.parametrize("operation_kind", ["apply_program", "revert_program", "discard_draft"])
def test_accepted_apply_keeps_overlapping_authoring_at_the_owner_boundary(
    tmp_path, operation_kind
) -> None:
    workspace, program, _deployment, headless, coordinator = _fixture(tmp_path)

    def accept(_claim):
        # Manager adopts the parent claim before acknowledging acceptance.
        # Composer must neither release it nor invent an independent lock.
        headless.allowed = False
        headless.acquire_error = HeadlessClientError(
            "authoring_not_ready", "Manager owns the admission claim", status_code=409
        )
        return {"status": "accepted", "apply_id": "apply.1", "apply_generation": 1}

    coordinator.execute_external_submission(
        _envelope(), operation_kind="apply_program", draft_id=program.draft_id,
        draft_revision=program.revision, payload={},
        validate_under_claim=lambda _claim: None, submit=accept,
    )
    assert coordinator.local_claim_pending is False
    before = workspace.identity()
    with pytest.raises(HTTPException) as raised:
        if operation_kind == "discard_draft":
            coordinator.execute_local_mutation(
                replace(_envelope(), request_id="overlapping-discard"),
                operation_kind=operation_kind, draft_id=program.draft_id,
                draft_revision=program.revision, payload={}, workspace=workspace,
                install=lambda: pytest.fail("Discard must not install while Apply owns admission"),
            )
        else:
            coordinator.execute_external_submission(
                replace(_envelope(), request_id="overlapping-activation"),
                operation_kind=operation_kind, draft_id=program.draft_id,
                draft_revision=program.revision, payload={},
                validate_under_claim=lambda _claim: pytest.fail("Refused before validation"),
                submit=lambda _claim: pytest.fail("Must not submit another activation"),
            )
    assert raised.value.detail["code"] == "authoring_not_ready"
    assert workspace.identity() == before
    assert len(headless.claims) == 1
    assert headless.settlements == []
    assert coordinator.mutation_ready is True


def test_authoritative_acquire_rejection_does_not_contain(tmp_path) -> None:
    _workspace, program, _deployment, headless, coordinator = _fixture(tmp_path)
    headless.acquire_error = HeadlessClientError(
        "stale_generation", "claim was rejected", status_code=409
    )

    with pytest.raises(HTTPException):
        coordinator.execute_external_submission(
            _envelope(),
            operation_kind="apply_program",
            draft_id=program.draft_id,
            draft_revision=program.revision,
            payload={},
            validate_under_claim=lambda _claim: None,
            submit=lambda _claim: pytest.fail("rejected claim must not submit"),
        )

    assert coordinator.local_claim_pending is False
    assert coordinator.mutation_ready is True


def test_external_submission_rejects_composite_workspace_race(tmp_path) -> None:
    workspace, program, _deployment, _headless, coordinator = _fixture(tmp_path)
    captured = workspace.identity()
    submitted = False

    # A different browser wins the local workspace commit before this request
    # obtains admission. The under-claim recheck must reject the stale request
    # before any Manager submission occurs.
    program._revision += 1

    def validate(_claim: dict[str, Any]) -> None:
        workspace.require_identity(captured)

    def submit(_claim: dict[str, Any]) -> None:
        nonlocal submitted
        submitted = True

    with pytest.raises(AuthoringWorkspaceError):
        coordinator.execute_external_submission(
            _envelope(),
            operation_kind="apply_program",
            draft_id=program.draft_id,
            draft_revision=0,
            payload={},
            validate_under_claim=validate,
            submit=submit,
        )

    assert submitted is False
    assert coordinator.local_claim_pending is False
    assert coordinator.mutation_ready is True
