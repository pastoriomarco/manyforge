"""Focused tests for the local (non-durable) Composer authoring mutation path.

Covered here: the fixed admission sequence, the single deferred wake, proven
no-effect settlement for a refusal, containment and recovery of an uncertain
settlement, and the whole-workspace replacement session. These tests use
deterministic doubles; route-level integration is owned by the route migration.
"""

from __future__ import annotations

from copy import deepcopy
from dataclasses import replace
import json
import logging
from typing import Any
from types import SimpleNamespace

import httpx
import pytest
from fastapi import FastAPI, HTTPException
from fastapi.testclient import TestClient
from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient
from manyforge_headless_api.models import (
    AuthoringMutationContext,
    AuthoringMutationSettlementRequest,
    ManagerApplyAcknowledgementContext,
)

from manyforge_composer.backend.authoring_admission import (
    AuthoringAdmissionCoordinator,
    AuthoringMutationEnvelope,
    AuthoringOperationError,
    OperationEffects,
)
from manyforge_composer.backend.state import ComposerState
from manyforge_composer.backend.authoring_workspace import (
    AuthoringWorkspace,
    AuthoringWorkspaceError,
    AuthoringWorkspaceIdentity,
    scenario_project_facts,
)
from manyforge_composer.backend.apply_orchestration import ApplyCommands
from manyforge_composer.backend.headless_ws import HeadlessStateSource
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

    def formula_references(self) -> dict[str, Any]:
        return {"computedKeys": {}, "nodes": {}}

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

    def program_cycle_status(self) -> dict[str, Any]:
        return {"state": "idle_after_reset"}

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
    coordinator = AuthoringAdmissionCoordinator(headless, 0x3F9A0000000000A1)
    assert coordinator.mutation_ready
    return workspace, program, deployment, headless, coordinator


_BROWSER_A = 0x3F9A0000000000A1
_BROWSER_KEY = bytes.fromhex("0123456789abcdef0123456789abcdef")


def _envelope(generation: int = 7) -> AuthoringMutationEnvelope:
    return AuthoringMutationEnvelope(
        expected_admission_generation=generation,
        application_start_id=_BROWSER_A,
        request_key=_BROWSER_KEY,
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
    # The claim and its release carry the browser's `A` and key unchanged
    # (contract 1.15.2: the claim derives from the browser's request).
    claim_request = headless.claims[0]
    assert set(claim_request) == {
        "request_key", "application_start_id", "source_label",
        "expected_admission_generation",
    }
    assert claim_request["application_start_id"] == "3f9a0000000000a1"
    assert claim_request["request_key"] == _BROWSER_KEY.hex()
    # Headless's strict acquire model accepts the exact body Composer sends.
    assert AuthoringMutationContext.model_validate(claim_request).request_key == claim_request["request_key"]
    settlement = headless.settlements[0]
    assert settlement["application_start_id"] == "3f9a0000000000a1"
    assert settlement["request_key"] == claim_request["request_key"]
    # Headless's strict settle model accepts the exact body Composer sends.
    accepted = AuthoringMutationSettlementRequest.model_validate(settlement)
    assert accepted.request_key == settlement["request_key"]


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
    for changed in [replace(_envelope(), request_key=bytes(16)),
                    replace(_envelope(), application_start_id=0x1234), _envelope(8)]:
        assert coordinator.submission_outcome(changed)["outcome"] is None
    assert AuthoringAdmissionCoordinator(headless, 0x3F9A0000000000A1).submission_outcome(_envelope())["outcome"] is None
    assert headless.events == events
    app = FastAPI()
    app.include_router(create_authoring_workspace_router(SimpleNamespace(authoring_admission=coordinator)))
    response = TestClient(app).get('/api/authoring/submission-outcome', params={
        'application_start_id': '3f9a0000000000a1',
        'request_key': '0123456789ABCDEF0123456789abcdef',
        'expected_admission_generation': 7,
    })
    assert response.status_code == 200
    assert response.json() == coordinator.submission_outcome(_envelope())
    assert response.json() == {
        "schema_version": "manyforge.authoring_submission_outcome.v2",
        "application_start_id": "3f9a0000000000a1",
        "request_key": "0123456789abcdef0123456789abcdef",
        "expected_admission_generation": 7,
        "outcome": {"kind": "settled", "operation_kind": "mutate_draft", "apply": None},
    }
    for invalid in [
        {'application_start_id': '0000000000000000'},
        {'application_start_id': '3F9A0000000000A1'},
        {'request_key': '0123'},
    ]:
        refused = TestClient(app).get('/api/authoring/submission-outcome', params={
            'application_start_id': '3f9a0000000000a1',
            'request_key': '0123456789abcdef0123456789abcdef',
            'expected_admission_generation': 7,
            **invalid,
        })
        assert refused.status_code == 422
    assert headless.events == events
    for index in range(128):
        coordinator._record_outcome(
            replace(_envelope(), request_key=index.to_bytes(16, "big")), "mutate_draft"
        )
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
    # The record is kept for the next action's release; authoring is not
    # blocked (OD-196).
    assert coordinator.local_claim_pending is True
    assert coordinator.mutation_ready is True


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


def test_install_failure_after_a_save_write_is_logged_once_and_published(
    tmp_path, caplog
) -> None:
    """A Save wrote its file, then the in-memory install failed: the workspace
    is contained once, logged once, and published without an attempted edit."""

    workspace, program, _deployment, headless, coordinator = _fixture(tmp_path)
    source = HeadlessStateSource(
        headless,  # type: ignore[arg-type]
        admission=coordinator,
        workspace=workspace,
        # The Apply-level command rows (OD-214) publish on the same wake.
        apply_commands=ApplyCommands(),
    )
    # The shell publication reads the owner; this observer records its wake.
    workspace.set_observer(lambda: headless.events.append("wake"))
    saved = tmp_path / "saved.program.yaml"
    effects = OperationEffects()

    def write_under_claim() -> None:
        saved.write_text("program: saved\n", encoding="utf-8")
        effects.destination_published(str(saved))

    def failing_install() -> bool:
        raise RuntimeError("install exploded")

    caplog.set_level(logging.ERROR)
    with pytest.raises(AuthoringWorkspaceError) as raised:
        coordinator.execute_local_mutation(
            _envelope(),
            operation_kind="save_program",
            draft_id=program.draft_id,
            draft_revision=program.revision,
            payload={"path": str(saved)},
            workspace=workspace,
            install=failing_install,
            validate_under_claim=write_under_claim,
            effects=effects,
        )

    cause = "program in-memory install failed: RuntimeError: install exploded"
    assert raised.value.code == "composer_workspace_install_failed"
    assert raised.value.detail == {"cause": cause}
    # Containment is unchanged: the claim stays held and authoring blocked.
    assert workspace.failed_closed == cause
    assert coordinator.local_claim_pending is True
    assert coordinator.mutation_ready is False
    # One wake publishes the containment; nothing committed.
    assert headless.events == ["acquire", "wake"]

    template = json.dumps({
        "schema_version": "manyforge.execution_control.v4",
        "message_kind": "delta",
        "control_session_number": 1,
        "server_sequence": "2",
        "shell": {"complete": False},
    })
    shell = json.loads(source.local_delta(template))["shell"]
    assert shell["authoring_workspace_containment"] == {
        "cause": cause,
        "saved_path": str(saved),
    }
    assert shell["authoring_workspace"]["workspace_revision"] == 1

    # Every later edit is still refused before any effect, and logs nothing.
    with pytest.raises(HTTPException) as refused:
        _run_local(coordinator, workspace, program, headless)
    assert refused.value.status_code == 503
    assert refused.value.detail["code"] == "composer_workspace_failed_closed"
    assert refused.value.detail["effect"] == "none"
    with pytest.raises(HTTPException) as edit_refused:
        coordinator.execute_draft_edit(
            _envelope(),
            operation_kind="mutate_draft",
            workspace=workspace,
            draft_id=program.draft_id,
            expected_identity=workspace.publication()[0],
            install=lambda: program.install("rev-2"),
            session="program",
        )
    assert edit_refused.value.detail["code"] == "composer_workspace_failed_closed"
    assert workspace.failed_closed == cause

    records = [
        record
        for record in caplog.records
        if record.getMessage() == "composer_authoring_workspace_contained"
    ]
    assert len(records) == 1
    record = records[0]
    assert record.levelno == logging.ERROR
    assert record.name == "manyforge_composer.backend.authoring_workspace"
    assert record.operation == "save_program"
    assert record.install_label == "program"
    assert record.cause == cause
    assert record.written_path == str(saved)
    assert record.application_start_id == "3f9a0000000000a1"
    assert record.request_key == _BROWSER_KEY.hex()
    # The claim carries the browser's key (contract 1.15.2).
    assert record.owner_transaction_id == headless.claims[0]["request_key"]
    assert record.owner_transaction_id == _BROWSER_KEY.hex()
    assert record.workspace_epoch == workspace.epoch


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


def test_older_admission_generation_is_claim_evidence_only(tmp_path) -> None:
    workspace, program, _deployment, headless, coordinator = _fixture(tmp_path)

    # The browser read generation 6; the current generation is 7.
    _run_local(
        coordinator,
        workspace,
        program,
        headless,
        envelope=_envelope(generation=6),
    )

    assert [claim["expected_admission_generation"] for claim in headless.claims] == [6]
    assert headless.events[0] == "acquire"
    assert coordinator.local_claim_pending is False


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
    # The exact request is kept; authoring is not blocked (OD-196).
    assert coordinator.mutation_ready is True
    assert coordinator.local_claim_pending is True
    assert workspace.revision == 1
    retained = deepcopy(coordinator._pending_local_claim.acquire_request)

    # The next request replays the exact acquire (same key), settles it as
    # proven no-effect, then proceeds with its own claim.
    headless.acquire_error = None
    next_envelope = replace(_envelope(), request_key=bytes.fromhex("02" * 16))
    _run_local(coordinator, workspace, program, headless, envelope=next_envelope)

    assert headless.claims[0] == retained
    assert headless.claims[1]["request_key"] == "02" * 16
    assert headless.events == [
        "acquire", "settle:proven_no_effect",
        "acquire", "install", "settle:owner_commit", "wake",
    ]
    assert coordinator.local_claim_pending is False
    assert coordinator.mutation_ready is True
    assert workspace.revision == 2


@pytest.mark.parametrize("error_type", [httpx.ReadError, httpx.RemoteProtocolError, httpx.ReadTimeout])
def test_real_http_client_retains_claim_after_lost_acquire_reply(error_type) -> None:
    accepted = []

    def handler(request: httpx.Request) -> httpx.Response:
        assert request.method == "POST"
        assert request.url.path == "/api/program/authoring-transactions"
        accepted.append(json.loads(request.content))
        raise error_type("reply lost after acceptance", request=request)

    with httpx.Client(base_url="http://headless.test", transport=httpx.MockTransport(handler)) as raw:
        coordinator = AuthoringAdmissionCoordinator(HeadlessHttpClient("http://headless.test", client=raw), 0x3F9A0000000000A1)
        with pytest.raises(HTTPException) as raised:
            coordinator._acquire_local_claim(
                envelope=_envelope(), claim_key=bytes.fromhex("ab" * 16),
                payload_digest="sha256:" + "a" * 64,
                expected_admission_generation=7, operation_kind="load_scenario_document",
            )
        assert raised.value.detail["code"] == (
            "headless_http_timeout" if error_type is httpx.ReadTimeout else "headless_http_unavailable"
        )
        assert len(accepted) == 1
        assert coordinator.mutation_ready is True
        assert coordinator.local_claim_pending is True
        assert coordinator._pending_local_claim.acquire_request == accepted[0]


def test_real_http_client_authoritative_acquire_refusal_does_not_contain() -> None:
    def handler(request: httpx.Request) -> httpx.Response:
        return httpx.Response(409, json={"detail": {"code": "authoring_not_ready",
                                                    "message": "admission closed"}}, request=request)

    with httpx.Client(base_url="http://headless.test", transport=httpx.MockTransport(handler)) as raw:
        coordinator = AuthoringAdmissionCoordinator(HeadlessHttpClient("http://headless.test", client=raw), 0x3F9A0000000000A1)
        with pytest.raises(HTTPException) as raised:
            coordinator._acquire_local_claim(
                envelope=_envelope(), claim_key=bytes.fromhex("ab" * 16),
                payload_digest="sha256:" + "a" * 64,
                expected_admission_generation=7, operation_kind="load_scenario_document",
            )
        assert raised.value.detail["code"] == "authoring_not_ready"
        assert coordinator.mutation_ready is True
        assert coordinator.local_claim_pending is False


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
    settle_key = coordinator._pending_local_claim.settle_request_key.hex()
    headless.settle_error = None
    headless.events.clear()
    headless.settlements.clear()
    next_envelope = replace(_envelope(), request_key=bytes.fromhex("02" * 16))
    _run_local(coordinator, workspace, program, headless, envelope=next_envelope)

    # The retained claim settles first, with its own disposition and key;
    # the next request then proceeds (OD-196: no block).
    assert headless.settlements[0]["settlement_kind"] == "proven_no_effect"
    # The resent settlement reuses the key minted with the claim.
    assert headless.settlements[0]["request_key"] == settle_key
    assert headless.events == [
        "settle:proven_no_effect", "acquire", "install", "settle:owner_commit", "wake",
    ]
    assert coordinator.local_claim_pending is False
    assert coordinator.mutation_ready is True


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
            "apply_id": 1,
            "apply_generation": 1,
        },
    )

    assert result["state"] == "pending"
    assert coordinator.submission_outcome(_envelope())["outcome"] == {
        "kind": "manager_accepted", "operation_kind": "apply_program",
        "apply": {"apply_id": 1, "apply_generation": 1},
    }
    assert len(submitted) == 1
    assert headless.events == ["acquire"]
    assert headless.settlements == []
    assert coordinator.local_claim_pending is False


def test_external_submission_refused_before_acquire_when_authoring_not_allowed(
    tmp_path,
) -> None:
    _workspace, program, _deployment, headless, coordinator = _fixture(
        tmp_path, allowed=False
    )

    with pytest.raises(HTTPException) as raised:
        coordinator.execute_external_submission(
            _envelope(),
            operation_kind="apply_program",
            draft_id=program.draft_id,
            draft_revision=program.revision,
            payload={},
            validate_under_claim=lambda _claim: pytest.fail("refused before validation"),
            submit=lambda _claim: pytest.fail("refused before submission"),
        )

    assert raised.value.status_code == 409
    assert raised.value.detail["code"] == "authoring_not_ready"
    assert raised.value.detail["effect"] == "none"
    assert headless.claims == []
    assert headless.settlements == []
    assert headless.events == []
    assert coordinator.local_claim_pending is False
    assert coordinator.mutation_ready is True
    assert coordinator.submission_outcome(_envelope())["outcome"] is None


@pytest.mark.parametrize("sent", [False, True])
def test_external_submission_refuses_unclassified_owner_response(tmp_path, sent) -> None:
    """R6b: neither accepted nor applied. An unsent claim is released now; a
    sent one is the Manager's. The typed code stays; nothing is blocked."""

    _workspace, program, _deployment, headless, coordinator = _fixture(tmp_path)

    def submit(claim):
        if sent:
            coordinator.manager_submission_sent(claim)
        return {"status": "accepted", "apply_id": 1}

    with pytest.raises(HTTPException) as raised:
        coordinator.execute_external_submission(
            _envelope(),
            operation_kind="apply_program",
            draft_id=program.draft_id,
            draft_revision=program.revision,
            payload={},
            validate_under_claim=lambda _claim: None,
            submit=submit,
        )

    assert raised.value.detail["code"] == "manager_apply_acceptance_invalid"
    assert coordinator.submission_outcome(_envelope())["outcome"] is None
    assert coordinator.mutation_ready is True
    assert coordinator.local_claim_pending is False
    assert [body["settlement_kind"] for body in headless.settlements] == (
        [] if sent else ["proven_no_effect"]
    )


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
        return {"status": "accepted", "apply_id": 1, "apply_generation": 1}

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
                replace(_envelope(), request_key=bytes.fromhex("01" * 16)),
                operation_kind=operation_kind, draft_id=program.draft_id,
                draft_revision=program.revision, payload={}, workspace=workspace,
                install=lambda: pytest.fail("Discard must not install while Apply owns admission"),
            )
        else:
            coordinator.execute_external_submission(
                replace(_envelope(), request_key=bytes.fromhex("02" * 16)),
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
        "authoring_not_ready", "claim was rejected", status_code=409
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


def _held_cycle(
    *, run_state: str = "intervention_required", motion_held: bool = True,
    active_operation_count: int | None = None, containment_pending: bool = False,
) -> dict[str, Any]:
    return {
        "state": "running",
        "program_run_id": "run.1",
        "run": {
            "state": run_state,
            "capture": {"program_run_id": "run.1"},
            "active_operation_count": active_operation_count,
        },
        "recovery_offer": {"actions": [{
            "action_id": "retry_motion_from_current_state",
            "enabled": not containment_pending,
            "disabled_reason_code": (
                "failure_pause_containment_unconfirmed" if containment_pending else None
            ),
        }]},
        "runtime_controls": {"execution_selections": {
            "motion_held": motion_held,
            "groups": [{"group_id": "all", "state": "held", "node_count": 3,
                        "held_node_count": 3}],
        }},
    }


# The live case after an operator Stop: `all` is `ready` with four of seven
# nodes held (control flow keeps running, two motion nodes never started).
_LIVE_STOPPED_RUN = {
    **_held_cycle(run_state="running"),
    "recovery_offer": None,
    "runtime_controls": {"execution_selections": {
        "motion_held": True,
        "groups": [{"group_id": "all", "state": "ready", "node_count": 7,
                    "held_node_count": 4}],
    }},
}


class _HeldHeadless(_Headless):
    """A Program run exists: Kernel authoring admission is closed."""

    def __init__(self, cycle: dict[str, Any] | Exception, manual: list | None = None) -> None:
        super().__init__(allowed=False)
        self.cycle = cycle
        self.manual = [] if manual is None else manual

    def program_cycle_status(self) -> dict[str, Any]:
        if isinstance(self.cycle, Exception):
            raise self.cycle
        return self.cycle

    def execution_control_state(self) -> dict[str, Any]:
        return {"ok": True, "control_state": {"manual_operations": self.manual}}


def _draft_edit(coordinator, workspace, program, headless, envelope=None):
    workspace.set_observer(lambda: headless.events.append("wake"))

    def install() -> bool:
        headless.events.append("install")
        return program.install("rev-edit")

    return coordinator.execute_draft_edit(
        envelope or _envelope(),
        operation_kind="mutate_draft",
        workspace=workspace,
        draft_id=program.draft_id,
        expected_identity=workspace.identity(),
        install=install,
        session="program",
    )


def test_idle_draft_edit_commits_without_a_cell_claim(tmp_path) -> None:
    workspace, program, _deployment, headless, coordinator = _fixture(tmp_path)

    commit = _draft_edit(coordinator, workspace, program, headless)

    # Only the Draft changes: no Kernel claim is acquired or settled.
    assert headless.events == ["install", "wake"]
    assert headless.claims == [] and headless.settlements == []
    assert commit.snapshot.workspace_revision == 2
    assert commit.admission_generation == 7
    assert coordinator.submission_outcome(_envelope())["outcome"] == {
        "kind": "settled", "operation_kind": "mutate_draft", "apply": None,
    }


def _set_deployment(deployment, name: str) -> bool:
    deployment._document = {"name": name}
    deployment._revision += 1
    return True


def test_scenario_draft_edit_commits_both_drafts_in_one_revision(tmp_path) -> None:
    workspace, program, deployment, headless, coordinator = _fixture(tmp_path)

    commit = coordinator.execute_draft_edit(
        _envelope(),
        operation_kind="discard_scenario",
        workspace=workspace,
        draft_id=program.draft_id,
        expected_identity=workspace.identity(),
        install=lambda: _set_deployment(deployment, "applied") | program.install("rev-applied"),
        session="scenario",
    )

    # One Draft-only commit: both Drafts changed, one workspace revision, no claim.
    assert commit.snapshot.workspace_revision == 2
    assert deployment.revision == 1 and program.revision == 1
    assert headless.claims == [] and headless.settlements == []


def test_scenario_draft_edit_failure_contains_instead_of_reporting_no_effect(tmp_path) -> None:
    workspace, program, deployment, headless, coordinator = _fixture(tmp_path)

    def install() -> bool:
        _set_deployment(deployment, "applied")
        raise RuntimeError("second Draft install failed")

    with pytest.raises(AuthoringWorkspaceError) as failed:
        coordinator.execute_draft_edit(
            _envelope(),
            operation_kind="discard_scenario",
            workspace=workspace,
            draft_id=program.draft_id,
            expected_identity=workspace.identity(),
            install=install,
            session="scenario",
        )
    # A half-applied install is never a proven no-effect refusal: the
    # workspace is contained and every later edit is refused.
    assert failed.value.code == "composer_workspace_install_failed"
    with pytest.raises(AuthoringWorkspaceError):
        workspace.identity()


def test_contained_commit_before_submit_holds_the_claim_instead_of_no_effect(
    tmp_path,
) -> None:
    workspace, program, deployment, headless, coordinator = _fixture(tmp_path)

    def install() -> bool:
        _set_deployment(deployment, "saved")
        raise RuntimeError("Program restore failed")

    with pytest.raises(AuthoringWorkspaceError) as failed:
        coordinator.execute_external_submission(
            _envelope(),
            operation_kind="revert_program",
            draft_id=program.draft_id,
            draft_revision=program.revision,
            payload={},
            validate_under_claim=lambda _claim: None,
            commit_before_submit=lambda: workspace.replace(
                expected_identity=workspace.identity(), install=install
            ),
            submit=lambda _claim: pytest.fail("a contained commit must not submit"),
            workspace=workspace,
        )

    # As on the local path: never settled as proven no effect; the claim stays
    # held for diagnosis and authoring is blocked.
    assert failed.value.code == "composer_workspace_install_failed"
    assert headless.settlements == []
    assert coordinator.local_claim_pending is True
    assert coordinator.mutation_ready is False

    # The next submission settles the retained claim truthfully, never as
    # proven no effect, and admits no dependent work.
    with pytest.raises(HTTPException) as refused:
        coordinator.execute_external_submission(
            _envelope(),
            operation_kind="revert_program",
            draft_id=program.draft_id,
            draft_revision=program.revision,
            payload={},
            validate_under_claim=lambda _claim: pytest.fail("contained authoring admitted work"),
            submit=lambda _claim: pytest.fail("contained authoring admitted work"),
            workspace=workspace,
        )
    assert refused.value.detail["code"] == "composer_authoring_recovery_blocked"
    assert [body["settlement_kind"] for body in headless.settlements] == ["owner_commit"]
    assert coordinator.local_claim_pending is False
    assert coordinator.mutation_ready is False


@pytest.mark.parametrize(
    "cycle",
    [
        _held_cycle(),  # failure pause, Stop of other motion confirmed
        _held_cycle(run_state="running"),  # operator Stop holds all motion
        _LIVE_STOPPED_RUN,
        _held_cycle(run_state="halted", motion_held=False,
                    active_operation_count=0),  # settled Halt
    ],
)
@pytest.mark.parametrize(
    "manual",
    [
        [],
        [{"resident": False, "operation": {"state": "completed"}}],
        [{"resident": True, "operation": {"state": "cancelled_pre_dispatch"}}],
    ],
)
def test_held_run_with_all_motion_stopped_admits_a_draft_edit(
    tmp_path, cycle, manual
) -> None:
    workspace, program, _deployment, _headless, _coordinator = _fixture(tmp_path)
    headless = _HeldHeadless(cycle, manual)
    coordinator = AuthoringAdmissionCoordinator(headless, 0x3F9A0000000000A1)

    _draft_edit(coordinator, workspace, program, headless)

    assert headless.events == ["install", "wake"]
    assert headless.claims == []
    assert workspace.revision == 2


@pytest.mark.parametrize(
    "cycle, manual",
    [
        # The full matrix is the shared rule's (Headless held_run_authoring).
        (_held_cycle(motion_held=False), []),
        (_held_cycle(run_state="running", motion_held=False), []),
        (_held_cycle(containment_pending=True), []),
        (_held_cycle(run_state="halted", active_operation_count=None), []),
        ({**_held_cycle(), "recovery_offer": None}, []),  # failure pause unproven
        (_held_cycle(), [{"resident": True, "operation": {"state": "executing"}}]),
        (HeadlessClientError("headless_http_timeout", "timed out"), []),
    ],
)
def test_moving_settling_or_unknown_run_refuses_a_draft_edit(
    tmp_path, cycle, manual
) -> None:
    workspace, program, _deployment, _headless, _coordinator = _fixture(tmp_path)
    headless = _HeldHeadless(cycle, manual)
    coordinator = AuthoringAdmissionCoordinator(headless, 0x3F9A0000000000A1)

    with pytest.raises(HTTPException) as refused:
        _draft_edit(coordinator, workspace, program, headless)

    assert refused.value.status_code == 409
    assert refused.value.detail["code"] == "authoring_not_ready"
    # Refused before any effect: the client releases its submission guard.
    assert refused.value.detail["effect"] == "none"
    assert headless.events == []
    assert workspace.revision == 1


def test_draft_edit_is_not_fenced_by_the_admission_generation(tmp_path) -> None:
    workspace, program, _deployment, headless, coordinator = _fixture(tmp_path)

    # Stop, Reset or a component event moved the Kernel generation since the
    # client read it: an in-memory Draft edit is unaffected.
    _draft_edit(coordinator, workspace, program, headless, envelope=_envelope(6))

    assert headless.events == ["install", "wake"]
    assert headless.claims == []
    assert workspace.revision == 2


def test_idle_draft_edit_ignores_kernel_authoring_admission(tmp_path) -> None:
    # Another client holds an Apply claim (or a manual operation runs): Kernel
    # authoring admission is closed, but no Program run exists.
    workspace, program, _deployment, headless, coordinator = _fixture(tmp_path)
    headless.allowed = False

    _draft_edit(coordinator, workspace, program, headless)

    assert headless.events == ["install", "wake"]
    assert headless.claims == [] and headless.settlements == []
    assert workspace.revision == 2

    # The claim-taking paths still refuse while admission is closed.
    with pytest.raises(HTTPException) as refused:
        _run_local(coordinator, workspace, program, headless)
    assert refused.value.detail["code"] == "authoring_not_ready"
    assert headless.claims == []


@pytest.mark.parametrize(
    "cycle",
    [
        {"state": "starting"},
        {"state": "resetting"},
        {"state": "terminal_awaiting_reset"},
        {"state": "recovery_required"},
        HeadlessClientError("headless_http_timeout", "timed out"),
    ],
)
def test_non_idle_cycle_without_a_held_run_refuses_a_draft_edit(
    tmp_path, cycle
) -> None:
    workspace, program, _deployment, _headless, _coordinator = _fixture(tmp_path)
    headless = _HeldHeadless(cycle)
    coordinator = AuthoringAdmissionCoordinator(headless, 0x3F9A0000000000A1)

    with pytest.raises(HTTPException) as refused:
        _draft_edit(coordinator, workspace, program, headless)

    assert refused.value.status_code == 409
    assert refused.value.detail["code"] == "authoring_not_ready"
    assert refused.value.detail["effect"] == "none"
    assert headless.events == []
    assert workspace.revision == 1


# OD-196 (study "unblock-OD-196-composer-claim-settlement", option 2+): a lost
# answer leaves no permanent block. A claim never sent to the Manager is
# settled from the Kernel's answers at the next action; once the Manager
# request was sent, the claim is the Manager's and Composer never releases it.

_LOST = "headless_http_timeout"


class _Cell:
    """The Kernel's claims and the Manager's Applies behind one Headless double.

    Claims are keyed by their request key, as the Kernel's replay lookup is.
    ``calls`` records every Kernel and Manager request in order, and
    ``("manager_release", g)`` each claim the Manager releases itself. Claim
    request bodies are validated against the Headless ingress models.
    """

    def __init__(self) -> None:
        self.generation = 7
        self.calls: list[tuple[Any, ...]] = []
        self.held: dict[str, dict[str, Any]] = {}
        self.released: set[str] = set()
        self.applies: list[dict[str, Any]] = []
        self._claim_generation = 0
        # One-shot faults, consumed by the next request of their kind.
        self.acquire_fault: str | None = None
        self.settle_fault: str | None = None
        self.manager_fault: str | None = None

    def _held_key(self, claim_generation: int) -> str | None:
        for key, claim in self.held.items():
            if claim["claim_generation"] == claim_generation:
                return key
        return None

    def _release(self, key: str) -> None:
        del self.held[key]
        self.released.add(key)
        self.generation += 1

    def authoring_admission(self) -> dict[str, Any]:
        self.calls.append(("authoring_admission",))
        return {
            "authoring_allowed": not self.held,
            "admission_generation": self.generation,
        }

    def program_cycle_status(self) -> dict[str, Any]:
        return {"state": "idle_after_reset"}

    def acquire_authoring_transaction(self, body: dict[str, Any]) -> dict[str, Any]:
        AuthoringMutationContext.model_validate(body)
        key = body["request_key"]
        self.calls.append(("acquire", key))
        fault, self.acquire_fault = self.acquire_fault, None
        if fault == "lost_before":
            raise HeadlessClientError(_LOST, "the acquire reply was lost")
        if fault == "unavailable_definite":
            raise HeadlessClientError(
                "execution_owner_unavailable", "the Kernel refused", status_code=503
            )
        if key in self.held:
            return {"claim": deepcopy(self.held[key])}
        if key in self.released:
            raise HeadlessClientError(
                "request_result_expired", "the exact claim was released",
                status_code=409, producer="kernel",
            )
        if self.held:
            raise HeadlessClientError(
                "authoring_not_ready", "cell-authoring admission is not ready",
                status_code=409, producer="kernel",
            )
        self._claim_generation += 1
        claim = {
            "intent": {
                "claim_kind": "authoring_mutation",
                "owner_role": "authoring_owner",
                "owner_transaction_id": None,
                "request_key": key,
            },
            "claim_generation": self._claim_generation,
            "acquired_admission_generation": self.generation,
            "accepted_at_ns": 1,
        }
        self.held[key] = claim
        if fault == "lost_after":
            raise HeadlessClientError(_LOST, "the acquire reply was lost")
        if fault == "outcome_unknown_after":
            # Headless lost the Kernel's answer and says so (OD-196 flag).
            raise HeadlessClientError(
                "execution_owner_unavailable",
                "native execution transport failed: timed out",
                status_code=503,
                details={"outcome_unknown": True},
            )
        return {"claim": deepcopy(claim)}

    def settle_authoring_transaction(self, body: dict[str, Any]) -> dict[str, Any]:
        AuthoringMutationSettlementRequest.model_validate(body)
        generation = body["claim"]["claim_generation"]
        self.calls.append(("settle", generation, body["settlement_kind"]))
        fault, self.settle_fault = self.settle_fault, None
        if fault == "lost_before":
            raise HeadlessClientError(_LOST, "the release reply was lost")
        key = self._held_key(generation)
        if key is None:
            raise HeadlessClientError(
                "stale_generation", "cell-admission request rejected",
                status_code=409, details={"reason": "claim_release_not_exact"},
                producer="kernel",
            )
        self._release(key)
        if fault == "lost_after":
            raise HeadlessClientError(_LOST, "the release reply was lost")
        return {
            "admission": {
                "authoring_allowed": not self.held,
                "admission_generation": self.generation,
            }
        }

    def manager_apply(self, body: dict[str, Any]) -> dict[str, Any]:
        parent = body["parent_authoring_claim"]["claim_generation"]
        self.calls.append(("manager_apply", parent))
        fault, self.manager_fault = self.manager_fault, None
        if fault == "never_received":
            raise HeadlessClientError(_LOST, "the Manager did not answer in time")
        if fault == "rejected":
            raise HeadlessClientError(
                "apply_rejected", "refused before the journal", status_code=409,
                details={"effect": "none"},
            )
        apply = {
            "apply_id": len(self.applies) + 1,
            "parent": parent,
            # A parent check that fails at acceptance ends the Apply at once.
            "state": "failed" if fault == "terminal_failed" else "pending",
        }
        self.applies.append(apply)
        if fault == "lost_after_accept":
            raise HeadlessClientError(_LOST, "the Manager accepted; its answer was lost")
        return {
            "ok": True,
            "state": apply["state"],
            "apply_id": apply["apply_id"],
            "apply_generation": 1,
            "replay_status": "accepted_new",
        }

    def end_apply(self, apply_id: int, outcome: str) -> None:
        """The Manager's lane ends one Apply. It succeeds only while its parent
        is held (the per-pass parent check); success releases the parent."""

        apply = self.applies[apply_id - 1]
        key = self._held_key(apply["parent"])
        if outcome == "succeeded" and key is None:
            outcome = "failed"
        apply["state"] = outcome
        if outcome == "succeeded":
            self._release(key)
            self.calls.append(("manager_release", apply["parent"]))

    def manager_apply_status(
        self, apply_id: int, _apply_generation: int, *, application_start_id: str
    ) -> dict[str, Any]:
        self.calls.append(("manager_apply_status", apply_id))
        if self.applies[apply_id - 1]["state"] == "pending":
            self.end_apply(apply_id, "succeeded")
        return {"ok": True, "state": self.applies[apply_id - 1]["state"]}

    def manager_apply_release_for_new(self, context: dict[str, Any]) -> dict[str, Any]:
        # The real client validates the context strictly before sending.
        ManagerApplyAcknowledgementContext.model_validate(context)
        self.calls.append(("release_for_new",))
        for apply in reversed(self.applies):
            if apply["state"] in {"failed", "unknown_effect"} and not apply.get("new"):
                apply["new"] = True
                key = self._held_key(apply["parent"])
                if key is not None:
                    # The Manager releases the failed Apply's parent claim.
                    self._release(key)
                    self.calls.append(("manager_release", apply["parent"]))
                return {
                    "ok": True,
                    "released": {"apply_id": apply["apply_id"], "apply_generation": 1},
                }
        return {"ok": True, "released": None}


def _cell_fixture(tmp_path):
    program = _ProgramSession()
    workspace = AuthoringWorkspace(
        program_session=program, deployment_session=_DeploymentSession()
    )
    cell = _Cell()
    coordinator = AuthoringAdmissionCoordinator(cell, _BROWSER_A)  # type: ignore[arg-type]
    state = ComposerState(application_start_id=_BROWSER_A)
    state.headless_client = cell  # type: ignore[assignment]
    state.authoring_admission = coordinator
    return workspace, program, cell, coordinator, state


def _key(number: int) -> str:
    return f"{number:02x}" * 16


def _keyed(number: int) -> AuthoringMutationEnvelope:
    return replace(_envelope(), request_key=bytes.fromhex(_key(number)))


def _edit(coordinator, workspace, program, number: int):
    """One claim-taking local edit with request key ``_key(number)``."""

    return coordinator.execute_local_mutation(
        _keyed(number),
        operation_kind="mutate_draft",
        draft_id=program.draft_id,
        draft_revision=program.revision,
        payload={"mutation": "tree_draft_rename_node", "arguments": {}},
        workspace=workspace,
        install=lambda: program.install(f"rev-{number}"),
    )


def _external(coordinator, number: int, submit):
    return coordinator.execute_external_submission(
        _keyed(number),
        operation_kind="apply_program",
        draft_id="program-draft-1",
        draft_revision=0,
        payload={},
        validate_under_claim=lambda _claim: None,
        submit=submit,
    )


def _apply(coordinator, state, number: int):
    """One Program Apply submitted through Composer's real Manager request."""

    def submit(claim):
        accepted = state.apply_manager_deployment_candidate(
            {"manager_apply": {"request_key": "1" * 32}}, claim
        )
        return {
            "status": "accepted",
            "apply_id": accepted["apply_id"],
            "apply_generation": accepted["apply_generation"],
            "manager_apply": accepted,
        }

    return _external(coordinator, number, submit)


def _new(cell: _Cell) -> dict[str, Any]:
    """The Manager release New sends first (routes_scenario_documents)."""

    return cell.manager_apply_release_for_new(
        {"request_key": "ab" * 16, "source_label": "manyforge_composer"}
    )


def _clean_edit_calls(number: int, claim_generation: int) -> list[tuple[Any, ...]]:
    return [
        ("authoring_admission",),
        ("acquire", _key(number)),
        ("settle", claim_generation, "owner_commit"),
    ]


def test_r1_lost_acquire_reply_is_replayed_and_released_at_the_next_action(
    tmp_path,
) -> None:
    workspace, program, cell, coordinator, _state = _cell_fixture(tmp_path)
    cell.acquire_fault = "lost_after"

    with pytest.raises(HTTPException) as lost:
        _edit(coordinator, workspace, program, 1)

    assert lost.value.detail["code"] == "headless_http_timeout"
    assert cell.calls == [("authoring_admission",), ("acquire", _key(1))]
    # The Kernel admitted the claim; Composer keeps the exact request and
    # blocks nothing.
    assert list(cell.held) == [_key(1)]
    assert coordinator.local_claim_pending is True
    assert coordinator.mutation_ready is True
    assert workspace.revision == 1

    cell.calls.clear()
    _edit(coordinator, workspace, program, 2)

    assert cell.calls == [
        ("acquire", _key(1)),  # exact replay: the held claim
        ("settle", 1, "proven_no_effect"),  # never sent: Composer releases it
        *_clean_edit_calls(2, 2),
    ]
    assert cell.held == {}
    assert coordinator.local_claim_pending is False
    assert workspace.revision == 2


def test_r1_replay_refusal_forgets_and_unanswered_replay_refuses_this_attempt(
    tmp_path,
) -> None:
    workspace, program, cell, coordinator, _state = _cell_fixture(tmp_path)
    cell.acquire_fault = "lost_before"
    with pytest.raises(HTTPException):
        _edit(coordinator, workspace, program, 1)
    assert cell.held == {}
    assert coordinator.local_claim_pending is True

    # The replay gets no answer either: only this attempt is refused.
    cell.calls.clear()
    cell.acquire_fault = "lost_before"
    with pytest.raises(HTTPException) as unresolved:
        _edit(coordinator, workspace, program, 2)
    assert unresolved.value.status_code == 503
    assert unresolved.value.detail["code"] == "composer_authoring_claim_uncertain"
    assert cell.calls == [("acquire", _key(1))]
    assert coordinator.local_claim_pending is True
    assert coordinator.mutation_ready is True

    # Another owner holds the cell now: the Kernel refuses the replay, which
    # proves this request holds nothing. The record is forgotten and the
    # action is refused by admission, not by Composer.
    cell.held[_key(9)] = {"claim_generation": 99}
    cell.calls.clear()
    with pytest.raises(HTTPException) as refused:
        _edit(coordinator, workspace, program, 3)
    assert refused.value.detail["code"] == "authoring_not_ready"
    assert cell.calls == [("acquire", _key(1)), ("authoring_admission",)]
    assert coordinator.local_claim_pending is False
    assert coordinator.mutation_ready is True


def test_r2_headless_lost_kernel_answer_keeps_the_request_for_replay(tmp_path) -> None:
    workspace, program, cell, coordinator, _state = _cell_fixture(tmp_path)
    cell.acquire_fault = "outcome_unknown_after"

    with pytest.raises(HTTPException) as lost:
        _edit(coordinator, workspace, program, 1)

    assert lost.value.detail["code"] == "execution_owner_unavailable"
    assert list(cell.held) == [_key(1)]
    assert coordinator.local_claim_pending is True
    assert coordinator.mutation_ready is True

    cell.calls.clear()
    _edit(coordinator, workspace, program, 2)
    assert cell.calls == [
        ("acquire", _key(1)),
        ("settle", 1, "proven_no_effect"),
        *_clean_edit_calls(2, 2),
    ]
    assert cell.held == {}

    # The same code without the flag is the Kernel's definite refusal: the
    # request is forgotten at once, as before.
    cell.acquire_fault = "unavailable_definite"
    with pytest.raises(HTTPException):
        _edit(coordinator, workspace, program, 3)
    assert coordinator.local_claim_pending is False
    assert coordinator.mutation_ready is True


@pytest.mark.parametrize("fault", ["lost_before", "lost_after"])
def test_r3_lost_release_answer_settles_at_the_next_action(tmp_path, fault) -> None:
    workspace, program, cell, coordinator, _state = _cell_fixture(tmp_path)
    cell.settle_fault = fault

    with pytest.raises(HTTPException) as lost:
        _edit(coordinator, workspace, program, 1)

    assert lost.value.detail["code"] == "composer_authoring_release_uncertain"
    assert cell.calls == _clean_edit_calls(1, 1)
    assert workspace.revision == 2  # the committed edit stays
    assert (_key(1) in cell.held) is (fault == "lost_before")
    assert coordinator.local_claim_pending is True
    assert coordinator.mutation_ready is True

    cell.calls.clear()
    _edit(coordinator, workspace, program, 2)

    # Released now, or already released (the Kernel's claim_release_not_exact
    # is a definite answer): either way settled, then the edit proceeds.
    assert cell.calls == [("settle", 1, "owner_commit"), *_clean_edit_calls(2, 2)]
    assert cell.held == {}
    assert coordinator.local_claim_pending is False
    assert workspace.revision == 3


def test_r4_failure_before_the_manager_request_releases_the_claim_at_once(
    tmp_path,
) -> None:
    workspace, program, cell, coordinator, _state = _cell_fixture(tmp_path)

    def submit(_claim):
        # For example the Manager status read that precedes the request.
        raise HeadlessClientError(_LOST, "Manager status read timed out")

    with pytest.raises(HeadlessClientError):
        _external(coordinator, 1, submit)

    assert cell.calls == [
        ("authoring_admission",),
        ("acquire", _key(1)),
        ("settle", 1, "proven_no_effect"),
    ]
    assert cell.held == {}
    assert coordinator.local_claim_pending is False
    assert coordinator.mutation_ready is True

    cell.calls.clear()
    _edit(coordinator, workspace, program, 2)
    assert cell.calls == _clean_edit_calls(2, 2)


def _lost_accepted_apply(coordinator, state, cell) -> None:
    cell.manager_fault = "lost_after_accept"
    with pytest.raises(AuthoringOperationError) as uncertain:
        _apply(coordinator, state, 1)
    assert uncertain.value.code == "manager_apply_submission_uncertain"
    assert cell.calls == [
        ("authoring_admission",),
        ("acquire", _key(1)),
        ("manager_apply", 1),
    ]
    # Sent: the Manager owns the claim; Composer forgot it and blocks nothing.
    assert coordinator.local_claim_pending is False
    assert coordinator.mutation_ready is True
    assert list(cell.held) == [_key(1)]
    cell.calls.clear()


def test_r5a_accepted_apply_with_lost_answer_keeps_its_parent(tmp_path) -> None:
    workspace, program, cell, coordinator, state = _cell_fixture(tmp_path)
    _lost_accepted_apply(coordinator, state, cell)
    assert cell.applies[0]["state"] == "pending"

    # The next claim-taking action is refused by the Kernel's admission while
    # the Apply holds the cell; Composer releases nothing.
    with pytest.raises(HTTPException) as refused:
        _edit(coordinator, workspace, program, 2)
    assert refused.value.detail["code"] == "authoring_not_ready"
    assert cell.calls == [("authoring_admission",)]
    assert list(cell.held) == [_key(1)]
    assert coordinator.mutation_ready is True


def test_r5b_succeeded_apply_released_its_parent_and_the_next_action_proceeds(
    tmp_path,
) -> None:
    workspace, program, cell, coordinator, state = _cell_fixture(tmp_path)
    _lost_accepted_apply(coordinator, state, cell)

    cell.end_apply(1, "succeeded")
    assert cell.calls == [("manager_release", 1)]  # the Manager releases it

    cell.calls.clear()
    _edit(coordinator, workspace, program, 2)
    assert cell.calls == _clean_edit_calls(2, 2)
    assert cell.held == {}


def test_r5c_failed_apply_parent_is_released_by_new(tmp_path) -> None:
    workspace, program, cell, coordinator, state = _cell_fixture(tmp_path)
    _lost_accepted_apply(coordinator, state, cell)
    cell.end_apply(1, "failed")

    with pytest.raises(HTTPException) as refused:
        _edit(coordinator, workspace, program, 2)
    assert refused.value.detail["code"] == "authoring_not_ready"
    assert cell.calls == [("authoring_admission",)]

    cell.calls.clear()
    assert _new(cell)["released"] == {"apply_id": 1, "apply_generation": 1}
    assert cell.calls == [("release_for_new",), ("manager_release", 1)]

    cell.calls.clear()
    _edit(coordinator, workspace, program, 3)
    assert cell.calls == _clean_edit_calls(3, 2)
    assert cell.held == {}


def test_r5d_unrecorded_submission_leaves_an_orphan_claim_until_restart(
    tmp_path,
) -> None:
    """Accepted by the maintainer (OD-196): a submission the Manager never
    recorded while Composer got no definite answer still needs a restart."""

    workspace, program, cell, coordinator, state = _cell_fixture(tmp_path)
    cell.manager_fault = "never_received"
    with pytest.raises(AuthoringOperationError) as uncertain:
        _apply(coordinator, state, 1)
    assert uncertain.value.code == "manager_apply_submission_uncertain"
    assert cell.calls == [
        ("authoring_admission",),
        ("acquire", _key(1)),
        ("manager_apply", 1),
    ]
    assert cell.applies == []
    assert coordinator.local_claim_pending is False
    assert coordinator.mutation_ready is True

    # Remaining state: the Kernel holds the claim, Composer has no record of
    # it, the Manager has no Apply naming it. Every claim-taking action is
    # refused by admission and New has nothing to release.
    cell.calls.clear()
    with pytest.raises(HTTPException) as refused:
        _edit(coordinator, workspace, program, 2)
    assert refused.value.detail["code"] == "authoring_not_ready"
    assert _new(cell)["released"] is None
    assert cell.calls == [("authoring_admission",), ("release_for_new",)]
    assert list(cell.held) == [_key(1)]


def test_r6_terminal_manager_answer_is_valid_and_new_releases_the_parent(
    tmp_path,
) -> None:
    workspace, program, cell, coordinator, state = _cell_fixture(tmp_path)
    cell.manager_fault = "terminal_failed"

    result = _apply(coordinator, state, 1)

    assert result["manager_apply"]["state"] == "failed"
    assert cell.calls == [
        ("authoring_admission",),
        ("acquire", _key(1)),
        ("manager_apply", 1),
    ]
    assert coordinator.local_claim_pending is False
    assert coordinator.mutation_ready is True
    assert coordinator.submission_outcome(_keyed(1))["outcome"] == {
        "kind": "manager_accepted",
        "operation_kind": "apply_program",
        "apply": {"apply_id": 1, "apply_generation": 1},
    }

    cell.calls.clear()
    _new(cell)
    _edit(coordinator, workspace, program, 2)
    assert cell.calls == [
        ("release_for_new",),
        ("manager_release", 1),
        *_clean_edit_calls(2, 2),
    ]


@pytest.mark.parametrize("terminal", ["pending", "succeeded", "failed", "unknown_effect"])
def test_r6_parser_classifies_every_manager_apply_state(terminal) -> None:
    state = ComposerState(application_start_id=_BROWSER_A)

    class Headless:
        def manager_apply(self, _body):
            return {"ok": True, "state": terminal, "apply_id": 3, "apply_generation": 1}

    state.headless_client = Headless()  # type: ignore[assignment]
    accepted = state.apply_manager_deployment_candidate(
        {"manager_apply": {"request_key": "1" * 32}}
    )
    assert accepted["state"] == terminal

    class Unknown(Headless):
        def manager_apply(self, _body):
            return {"ok": True, "state": "bogus", "apply_id": 3, "apply_generation": 1}

    state.headless_client = Unknown()  # type: ignore[assignment]
    with pytest.raises(AuthoringOperationError) as invalid:
        state.apply_manager_deployment_candidate({"manager_apply": {"request_key": "1" * 32}})
    assert invalid.value.code == "manager_apply_acceptance_invalid"


def test_d_next_edit_never_releases_the_parent_of_an_accepted_apply(tmp_path) -> None:
    """Study section D: after an uncertain submission the Manager accepted,
    the next Draft edit proceeds without releasing the Apply's parent, so the
    Apply's per-pass parent check still holds and it can succeed."""

    workspace, program, cell, coordinator, state = _cell_fixture(tmp_path)
    _lost_accepted_apply(coordinator, state, cell)

    commit = coordinator.execute_draft_edit(
        _keyed(2),
        operation_kind="mutate_draft",
        workspace=workspace,
        draft_id=program.draft_id,
        expected_identity=workspace.identity(),
        install=lambda: program.install("rev-edit"),
        session="program",
    )

    assert commit.snapshot.workspace_revision == 2
    assert cell.calls == []  # no Kernel release, no claim
    assert list(cell.held) == [_key(1)]
    cell.end_apply(1, "succeeded")
    assert cell.applies[0]["state"] == "succeeded"
    assert cell.calls == [("manager_release", 1)]


def test_preparation_settles_a_lost_answer_claim_before_it_reads_admission(
    tmp_path,
) -> None:
    """Load, New and both Applies prepare before the coordinator runs; the
    preparation settles the record first, under the same lock."""

    workspace, program, cell, coordinator, state = _cell_fixture(tmp_path)
    cell.acquire_fault = "lost_after"
    with pytest.raises(HTTPException):
        _edit(coordinator, workspace, program, 1)
    cell.calls.clear()

    def prepare_activation(_body):
        cell.calls.append(("prepare_activation", not cell.held))
        raise HeadlessClientError("preparation_stopped_here", "test stop", status_code=409)

    cell.prepare_activation = prepare_activation  # type: ignore[attr-defined]
    intent = {
        "source_document_digest": "sha256:" + "1" * 64,
        "scenario_id": "cell",
        "project_path": "/projects/cell.scenario",
        "captured_input_digest": "sha256:" + "2" * 64,
        "deployment": {},
        "model_source_closure": {},
        "program": None,
        "expected_admission_generation": 7,
        "application_start_id": "3f9a0000000000a1",
    }
    with pytest.raises(HTTPException) as stopped:
        state._prepare_authored_deployment_candidate(intent, None, apply=True)

    assert stopped.value.detail["code"] == "preparation_stopped_here"
    assert cell.calls == [
        ("acquire", _key(1)),
        ("settle", 1, "proven_no_effect"),
        ("prepare_activation", True),
    ]
    assert coordinator.local_claim_pending is False
