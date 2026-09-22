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

import pytest
from fastapi import HTTPException
from manyforge_headless_api.client import HeadlessClientError

from manyforge_composer.backend.authoring_admission import (
    AuthoringAdmissionCoordinator,
    AuthoringMutationEnvelope,
)
from manyforge_composer.backend.authoring_workspace import (
    AuthoringWorkspace,
    AuthoringWorkspaceError,
    AuthoringWorkspaceIdentity,
)


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
        return {
            "recovery_receipt": {
                "status": "unknown",
                "kernel_owner_epoch": "kernel.1",
                "held_claims": [],
                "current_owner_epochs": [],
            }
        }

    def reconcile_authoring_owner(
        self, _body: dict[str, Any]
    ) -> dict[str, Any]:
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
    coordinator = AuthoringAdmissionCoordinator(headless, tmp_path)
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

    coordinator.recover()

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
        "admission_transport_failed", "the exact claim was not issued"
    )

    with pytest.raises(HTTPException) as raised:
        _run_local(coordinator, workspace, program, headless)

    assert raised.value.status_code == 503
    assert raised.value.detail["code"] == "admission_transport_failed"
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
