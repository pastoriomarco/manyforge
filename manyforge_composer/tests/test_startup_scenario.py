from __future__ import annotations

from copy import deepcopy
import json
from pathlib import Path
from typing import Any

import pytest

from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.program import Program
from manyforge_composer.backend.draft_session import (
    ComposerDraftSession,
    DraftSessionError,
)
from manyforge_composer.backend.deployment_draft import DeploymentDraftSession
from manyforge_composer.backend.state import ComposerState
from manyforge_composer.backend.authoring_transaction_journal import (
    AuthoringJournalError,
)
from manyforge_headless_api.client import HeadlessClientError


_ROOT = Path(__file__).resolve().parents[2]
_PROGRAM_PATH = _ROOT / "examples/lite6_lite_gripper_pick_and_place.program.yaml"
_DEPLOYMENT_PATH = (
    _ROOT / "examples/lite6_lite_gripper_pick_and_place.deployment.yaml"
)


def _selection(program: dict[str, Any]) -> dict[str, Any]:
    return {
        "scenario_id": "lite6_pick_and_place",
        "scenario_catalog_revision": "sha256:" + "2" * 64,
        "assistant_mode": "forbidden",
        "assistant_projection": None,
        "deployment_artifact_id": "lite6_lite_gripper_pick_and_place",
        "deployment_revision": "sha256:" + "3" * 64,
        "retained_bundle_id": "sha256:" + "4" * 64,
        "graph_revision": "sha256:" + "5" * 64,
        "node_environment_digest": "sha256:" + "8" * 64,
        "owner_identities": [],
        "program_id": program["program_id"],
        "program_revision": program["program_revision"],
        "program_binding_id": "sha256:" + "6" * 64,
        "program": deepcopy(program),
        "program_binding": {"program_binding_id": "sha256:" + "6" * 64},
        "typed_parameter_overrides": {},
        "deployment": json.loads(_DEPLOYMENT_PATH.read_text(encoding="utf-8")),
    }


def _applied(selection: dict[str, Any]) -> dict[str, Any]:
    compiled_binding_id = "sha256:" + "9" * 64
    return {
        **deepcopy(selection),
        "baseline_program_binding_id": selection["program_binding_id"],
        "program_binding_id": compiled_binding_id,
        "program_binding": {"program_binding_id": compiled_binding_id},
        "source": {"record_digest": "sha256:" + "7" * 64},
    }


class _Headless:
    def __init__(self, selection: dict[str, Any]) -> None:
        self.selection = selection
        self.applied: dict[str, Any] | None = None
        self.manager_target = False
        self.manager_apply_bodies: list[dict[str, Any]] = []
        self.program_apply_bodies: list[dict[str, Any]] = []
        self.scene_preparations: list[dict[str, Any]] = []

    def scenario_program_selection(self, scenario_id: str) -> dict[str, Any]:
        assert scenario_id == self.selection["scenario_id"]
        return deepcopy(self.selection)

    def scenario_load_selection(
        self, scenario_id: str, *, timeout_seconds: float | None = None
    ) -> dict[str, Any]:
        assert scenario_id == self.selection["scenario_id"]
        assert timeout_seconds is None or timeout_seconds > 0
        return deepcopy(self.selection)

    def program_applied(self) -> dict[str, Any]:
        if self.applied is None:
            raise HeadlessClientError(
                "program_applied_selection_not_found",
                "no Program is Applied",
                status_code=404,
            )
        return deepcopy(self.applied)

    def authoring_admission(self) -> dict[str, Any]:
        return {"authoring_allowed": True, "admission_generation": 7}

    def manager_status(self) -> dict[str, Any]:
        return {
            "source": {
                "available": True,
                "freshness": "fresh",
                "known_gaps": [],
                "source_owner_epoch": "manager-owner-1",
            },
            "status": {
                "manager_ready": True,
                "active": {
                    "retained_bundle_id": (
                        self.selection["retained_bundle_id"]
                        if self.manager_target
                        else "sha256:" + "a" * 64
                    ),
                    "graph_revision": (
                        self.selection["graph_revision"]
                        if self.manager_target
                        else "sha256:" + "b" * 64
                    ),
                },
                "runtime": {"converged": True},
            },
        }

    def manager_diff(self) -> dict[str, Any]:
        return {
            "source": {
                "available": True,
                "freshness": "fresh",
                "known_gaps": [],
            },
            "converged": True,
        }

    def manager_apply(self, body: dict[str, Any]) -> dict[str, Any]:
        self.manager_apply_bodies.append(deepcopy(body))
        self.manager_target = True
        receipt = body["candidate_receipt"]
        return {
            "ok": True,
            "state": "succeeded",
            "apply_id": "apply-1",
            "apply_generation": 1,
            "request_id": body["request_id"],
            "request_owner_epoch": body["request_owner_epoch"],
            "candidate_projection_revision": {
                "retained_bundle_id": receipt["candidate_retained_bundle_id"],
                "graph_revision": receipt["candidate_graph_revision"],
            },
        }

    def manager_deployment_candidate_preview(
        self, body: dict[str, Any]
    ) -> dict[str, Any]:
        return {
            "ok": True,
            "candidate_receipt": {
                "schema_version": "manyforge.manager_deployment_candidate_receipt.v1",
                "deployment_artifact_id": body["deployment_artifact_id"],
                "deployment_revision": body["deployment_revision"],
                "scenario_catalog_revision": self.selection[
                    "scenario_catalog_revision"
                ],
                "candidate_retained_bundle_id": self.selection[
                    "retained_bundle_id"
                ],
                "candidate_graph_revision": self.selection["graph_revision"],
                "expected_current": deepcopy(body["expected_current"]),
                "expected_admission_generation": body[
                    "expected_admission_generation"
                ],
                "manager_owner_epoch": body["request_owner_epoch"],
                "receipt_digest": "sha256:" + "8" * 64,
            },
        }

    def program_apply(self, body: dict[str, Any]) -> dict[str, Any]:
        self.program_apply_bodies.append(deepcopy(body))
        self.applied = _applied(self.selection)
        return {
            "schema_version": "manyforge.program_apply_result.v2",
            "status": "applied",
            "preparation": {},
            "applied": deepcopy(self.applied),
        }

    def program_scene_prepare(self, body: dict[str, Any]) -> dict[str, Any]:
        self.scene_preparations.append(deepcopy(body))
        assert self.applied is not None
        return {
            "schema_version": "manyforge.program_scene_preparation.v1",
            "status": "prepared",
            "applied_record_digest": self.applied["source"]["record_digest"],
            "program_revision": self.applied["program_revision"],
            "admission_generation": 7,
            "prepared_scene": {
                "schema_version": "manyforge.prepared_program_scene.v1"
            },
        }


class _Coordinator:
    owner_epoch = "composer-owner-1"
    mutation_ready = True

    def __init__(self) -> None:
        self.executions: list[dict[str, Any]] = []

    def recover(self) -> None:
        self.mutation_ready = True

    def execute(self, envelope: Any, **arguments: Any) -> Any:
        prepared = arguments["prepare_effect"]()
        self.executions.append(
            {
                "envelope": envelope,
                "operation_kind": arguments["operation_kind"],
                "prepared": deepcopy(prepared),
            }
        )
        result = arguments["effect"](
            {"intent": {"claim_kind": "authoring_mutation"}}
        )
        arguments["post_image"]()
        after_release = arguments.get("after_release")
        if after_release is not None:
            after_release()
        return result


def _state(
    headless: _Headless,
    coordinator: _Coordinator,
    *,
    program: Program | None,
) -> ComposerState:
    state = object.__new__(ComposerState)
    state.node_environment = build_node_environment()
    state.allowed_fs_roots = [str(_ROOT / "examples")]
    state.allowed_program_write_roots = [str(_ROOT / "examples")]
    state.headless_client = headless
    state.authoring_admission = coordinator
    state.draft_session = ComposerDraftSession(
        program,
        source_path=(str(_PROGRAM_PATH) if program is not None else None),
    )
    state.deployment_draft_session = DeploymentDraftSession()
    state._active_authoring_catalog = lambda: state.node_catalog  # type: ignore[method-assign]
    return state


def test_deployment_candidate_preparation_is_reused_for_activation() -> None:
    program = Program(str(_PROGRAM_PATH), build_node_environment().catalog)
    selection = _selection(program.to_dict())
    headless = _Headless(selection)
    headless.applied = _applied(selection)
    state = _state(headless, _Coordinator(), program=None)
    state._restore_authoring_post_image = lambda _value: None  # type: ignore[method-assign]
    compilations: list[dict[str, Any]] = []

    def compile_candidate(body: dict[str, Any]) -> dict[str, Any]:
        compilations.append(deepcopy(body))
        return {
            "compilation": {
                "schema_version": "manyforge.deployment_candidate_compilation.v1",
                **{
                    field: selection[field]
                    for field in (
                        "scenario_id",
                        "scenario_catalog_revision",
                        "deployment_artifact_id",
                        "deployment_revision",
                        "retained_bundle_id",
                        "graph_revision",
                        "program_id",
                        "program_revision",
                        "program_binding_id",
                    )
                },
            }
        }

    headless.compile_deployment_candidate = compile_candidate  # type: ignore[attr-defined]

    def apply_candidate(body: dict[str, Any]) -> dict[str, Any]:
        headless.manager_apply_bodies.append(deepcopy(body))
        headless.manager_target = True
        receipt = body["candidate_receipt"]
        return {
            "ok": True,
            "state": "succeeded",
            "apply_id": "apply-1",
            "apply_generation": 1,
            "request_id": body["request_id"],
            "request_owner_epoch": body["request_owner_epoch"],
            "candidate_projection_revision": {
                "retained_bundle_id": receipt["candidate_retained_bundle_id"],
                "graph_revision": receipt["candidate_graph_revision"],
            },
        }

    headless.manager_apply = apply_candidate  # type: ignore[method-assign]
    intent = {
        "schema_version": "manyforge.composer_authoring_effect.v1",
        "operation_kind": "apply_deployment_candidate",
        "effect_class": "external",
        "external_kind": "deployment_candidate_apply",
        "scenario_id": selection["scenario_id"],
        "expected_scenario_catalog_revision": selection[
            "scenario_catalog_revision"
        ],
        "deployment": selection["deployment"],
        "program": selection["program"],
        "expected_manager_current": {
            "retained_bundle_id": "sha256:" + "a" * 64,
            "graph_revision": "sha256:" + "b" * 64,
        },
        "expected_admission_generation": 7,
        "manager_request_id": "manager-request-1",
        "correlation_id": "composer-request-1",
        "confirmation_token": None,
        "preparation_request_owner_epoch": "browser-1",
        "preparation_request_id": "composer-request-1",
        "draft_id": "draft-1",
        "draft_revision": 1,
        "post_state": {},
    }
    claim = {"intent": {"claim_kind": "authoring_mutation"}}

    prepared = state.execute_authored_deployment_candidate(
        intent, claim, apply=True, prepare_only=True
    )
    assert len(compilations) == 1
    assert prepared["manager_apply"]["request_id"] == "manager-request-1"

    # Recovery receives the journaled effect intent, not the route's in-memory
    # preparation object.  Prove that this path reuses the exact candidate and
    # Manager request without compiling or previewing again.
    retained_intent = {
        **intent,
        "prepared_deployment_candidate": deepcopy(prepared),
    }
    # Revert preparations created before activation can still carry derived
    # graph/binding values from the predecessor selection.  The Manager
    # receipt and Program Apply are the authorities for those values.
    retained_intent["prepared_deployment_candidate"]["compilation"][
        "graph_revision"
    ] = "sha256:" + "b" * 64
    retained_intent["prepared_deployment_candidate"]["compilation"][
        "program_binding_id"
    ] = "sha256:" + "c" * 64
    result = state.execute_authored_deployment_candidate(
        retained_intent, claim, apply=True
    )

    assert result["status"] == "applied"
    assert result["apply_id"] == "apply-1"
    assert len(compilations) == 1
    assert len(headless.manager_apply_bodies) == 1
    assert headless.manager_apply_bodies[0]["request_id"] == "manager-request-1"


@pytest.mark.parametrize("replay_status", ["accepted_new", "replayed_exact"])
@pytest.mark.parametrize("foreign_status", [False, True])
def test_candidate_apply_retains_acceptance_across_status_polling(
    monkeypatch: pytest.MonkeyPatch,
    replay_status: str,
    foreign_status: bool,
) -> None:
    program = Program(str(_PROGRAM_PATH), build_node_environment().catalog)
    headless = _Headless(_selection(program.to_dict()))
    headless.manager_target = True
    state = _state(headless, _Coordinator(), program=None)
    acceptance = {
        "api_version": "manyforge.manager_command_response.v1",
        "apply_id": "apply-1",
        "apply_generation": 1,
        "request_id": "request-1",
        "request_owner_epoch": "manager-owner-1",
        "replay_status": replay_status,
        "ok": True,
        "error": None,
        "state": "pending",
        "candidate_projection_revision": None,
    }
    terminal = {
        key: value
        for key, value in acceptance.items()
        if key not in ("request_id", "request_owner_epoch", "replay_status")
    }
    terminal.update(
        state="succeeded",
        apply_id="foreign-apply" if foreign_status else "apply-1",
        candidate_projection_revision={
            "retained_bundle_id": headless.selection["retained_bundle_id"],
            "graph_revision": headless.selection["graph_revision"],
        },
    )
    monkeypatch.setattr(headless, "manager_apply", lambda _body: deepcopy(acceptance))

    def status(
        apply_id: str,
        generation: int,
        *,
        timeout_seconds: float | None = None,
    ) -> dict[str, Any]:
        assert (apply_id, generation) == ("apply-1", 1)
        assert timeout_seconds is None or timeout_seconds > 0
        return deepcopy(terminal)

    monkeypatch.setattr(headless, "manager_apply_status", status, raising=False)
    monkeypatch.setattr("manyforge_composer.backend.state.time.sleep", lambda _: None)
    intent = {
        "manager_apply": {
            "candidate_receipt": {
                "candidate_retained_bundle_id": headless.selection[
                    "retained_bundle_id"
                ],
                "candidate_graph_revision": headless.selection["graph_revision"],
            }
        }
    }
    if foreign_status:
        with pytest.raises(AuthoringJournalError) as error:
            state.apply_manager_deployment_candidate(intent, {})
        assert error.value.code == "manager_apply_status_identity_mismatch"
    else:
        result = state.apply_manager_deployment_candidate(intent, {})
        assert result == {**acceptance, **terminal}
        assert acceptance["state"] == "pending"


def test_scene_preparation_retains_readiness_failure(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    program = Program(str(_PROGRAM_PATH), build_node_environment().catalog)
    headless = _Headless(_selection(program.to_dict()))
    headless.applied = _applied(headless.selection)
    monkeypatch.setattr(
        headless,
        "authoring_admission",
        lambda: {"admission_generation": 38, "authoring_allowed": False},
    )
    monkeypatch.setattr(
        headless,
        "program_scene_prepare",
        lambda _body: pytest.fail("unready scene preparation must not be submitted"),
    )
    state = _state(headless, _Coordinator(), program=None)
    with pytest.raises(AuthoringJournalError, match="authoring readiness") as error:
        state.prepare_current_applied_scene(
            request_owner_epoch="test-owner",
            request_id="test-apply",
        )
    assert error.value.code == "program_scene_preparation_unavailable"
    assert not headless.program_apply_bodies


def test_admitted_handover_rebinds_inactive_program(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    program = Program(str(_PROGRAM_PATH), build_node_environment().catalog)
    headless = _Headless(_selection(program.to_dict()))
    headless.applied = _applied(headless.selection)
    prior_digest = headless.applied["source"]["record_digest"]
    original = headless.program_applied

    def current_applied() -> dict[str, Any]:
        if headless.manager_target and not headless.program_apply_bodies:
            raise HeadlessClientError(
                "program_applied_selection_not_active",
                "old Deployment binding",
                status_code=409,
            )
        return original()

    monkeypatch.setattr(headless, "program_applied", current_applied)
    # Reuse this file's orchestration-only response fixture; wire validation
    # is exercised by the native browser workflow, not this stub.
    monkeypatch.setattr(
        "manyforge_composer.backend.state.validate_program_apply_result",
        lambda value: value,
    )
    state = _state(headless, _Coordinator(), program=None)
    state.load_startup_scenario(headless.selection["scenario_id"])
    assert len(headless.program_apply_bodies) == 1
    assert (
        headless.program_apply_bodies[0]["expected_applied_record_digest"]
        == prior_digest
    )


def test_startup_load_activates_deployment_then_program(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    program = Program(str(_PROGRAM_PATH), build_node_environment().catalog)
    selection = _selection(program.to_dict())
    headless = _Headless(selection)
    coordinator = _Coordinator()
    monkeypatch.setattr(
        "manyforge_composer.backend.state.validate_program_apply_result",
        lambda value: value,
    )

    state = _state(headless, coordinator, program=program)
    state._active_authoring_catalog = (  # type: ignore[method-assign]
        lambda: (_ for _ in ()).throw(
            AssertionError("admitted Scenario load re-read transient availability")
        )
    )

    result = state.load_startup_scenario(selection["scenario_id"])

    assert result["status"] == "loaded"
    assert coordinator.executions[0]["operation_kind"] == "load_scenario"
    assert coordinator.executions[0]["prepared"]["external_kind"] == "scenario_load"
    assert len(headless.manager_apply_bodies) == 1
    assert headless.manager_apply_bodies[0]["request_owner_epoch"] == "manager-owner-1"
    assert (
        headless.manager_apply_bodies[0]["parent_authoring_claim"]["intent"][
            "claim_kind"
        ]
        == "authoring_mutation"
    )
    assert len(headless.program_apply_bodies) == 1
    assert len(headless.scene_preparations) == 1
    assert headless.program_apply_bodies[0]["program"] == selection["program"]
    assert result["workspace"]["saved"]["matchesDraft"] is True
    assert result["workspace"]["applied"]["matchesDraft"] is True


def test_scenario_deployment_is_the_saved_revert_baseline() -> None:
    """Scenario selection must preserve a baseline for post-Apply Revert."""

    program = Program(str(_PROGRAM_PATH), build_node_environment().catalog)
    selection = _selection(program.to_dict())
    state = _state(_Headless(selection), _Coordinator(), program=None)

    # Initial Scenario load resolves the maintained Deployment artifact as the
    # Saved baseline, so Revert has a complete identity rather than an
    # unnamed revision.
    initial = state._scenario_post_state(
        selection,
        state.draft_session.retained_state(),
        program,
    )
    deployment = initial["deployment"]
    assert deployment["saved_path"] == str(_DEPLOYMENT_PATH.resolve())
    assert deployment["saved_document"] == selection["deployment"]

    # The equal-document branch is also used when a retained Draft already
    # names the selected Scenario but came from an older session without the
    # baseline.  It must be repaired without changing the Draft identity.
    retained = state.deployment_draft_session.retained_state()
    retained.update(
        {
            "document": deepcopy(selection["deployment"]),
            "saved_path": None,
            "saved_document": None,
        }
    )
    state.deployment_draft_session.restore_retained_state(retained)
    repaired = state._scenario_post_state(
        selection,
        state.draft_session.retained_state(),
        program,
    )
    assert repaired["deployment"]["draft_id"] == retained["draft_id"]
    assert repaired["deployment"]["saved_path"] == str(_DEPLOYMENT_PATH.resolve())
    assert repaired["deployment"]["saved_document"] == selection["deployment"]


def test_startup_load_waits_for_reconciled_authoring_admission(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    program = Program(str(_PROGRAM_PATH), build_node_environment().catalog)
    selection = _selection(program.to_dict())
    headless = _Headless(selection)
    coordinator = _Coordinator()
    admissions = [
        {"authoring_allowed": False, "admission_generation": 7},
        {"authoring_allowed": False, "admission_generation": 8},
        {"authoring_allowed": True, "admission_generation": 9},
    ]
    sleeps: list[float] = []
    headless.authoring_admission = (  # type: ignore[method-assign]
        lambda: admissions.pop(0) if len(admissions) > 1 else admissions[0]
    )
    monkeypatch.setattr("manyforge_composer.backend.state.time.sleep", sleeps.append)
    monkeypatch.setattr(
        "manyforge_composer.backend.state.validate_program_apply_result",
        lambda value: value,
    )

    result = _state(headless, coordinator, program=program).load_startup_scenario(
        selection["scenario_id"]
    )

    assert result["status"] == "loaded"
    assert sleeps == [0.1, 0.1]
    assert coordinator.executions[0]["envelope"].expected_admission_generation == 9


def test_startup_load_retries_incomplete_authoring_recovery(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    program = Program(str(_PROGRAM_PATH), build_node_environment().catalog)
    selection = _selection(program.to_dict())
    headless = _Headless(selection)

    class RecoveringCoordinator(_Coordinator):
        def __init__(self) -> None:
            super().__init__()
            self.mutation_ready = False
            self.recovery_attempts = 0

        def recover(self) -> None:
            self.recovery_attempts += 1
            self.mutation_ready = self.recovery_attempts == 2

    coordinator = RecoveringCoordinator()
    headless.authoring_admission = lambda: {  # type: ignore[method-assign]
        "authoring_allowed": coordinator.mutation_ready,
        "admission_generation": 9,
    }
    monkeypatch.setattr("manyforge_composer.backend.state.time.sleep", lambda _: None)
    monkeypatch.setattr(
        "manyforge_composer.backend.state.validate_program_apply_result",
        lambda value: value,
    )

    result = _state(headless, coordinator, program=program).load_startup_scenario(
        selection["scenario_id"]
    )

    assert result["status"] == "loaded"
    assert coordinator.recovery_attempts == 2
    assert coordinator.executions[0]["envelope"].expected_admission_generation == 9


def test_startup_load_is_effect_idempotent_when_scenario_is_already_current() -> None:
    program = Program(str(_PROGRAM_PATH), build_node_environment().catalog)
    selection = _selection(program.to_dict())
    headless = _Headless(selection)
    headless.manager_target = True
    headless.applied = _applied(selection)
    coordinator = _Coordinator()

    result = _state(headless, coordinator, program=program).load_startup_scenario(
        selection["scenario_id"]
    )

    assert result["status"] == "loaded"
    assert len(coordinator.executions) == 1
    assert headless.manager_apply_bodies == []
    assert headless.program_apply_bodies == []
    assert len(headless.scene_preparations) == 1


def test_startup_load_does_not_confirm_discard_for_an_empty_draft(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    program = Program(str(_PROGRAM_PATH), build_node_environment().catalog)
    selection = _selection(program.to_dict())
    headless = _Headless(selection)
    coordinator = _Coordinator()
    monkeypatch.setattr(
        "manyforge_composer.backend.state.validate_program_apply_result",
        lambda value: value,
    )

    result = _state(headless, coordinator, program=None).load_startup_scenario(
        selection["scenario_id"]
    )

    assert result["status"] == "loaded"
    assert len(headless.manager_apply_bodies) == 1
    assert len(headless.program_apply_bodies) == 1


def test_load_rejects_discarding_a_different_unapplied_draft() -> None:
    program = Program(str(_PROGRAM_PATH), build_node_environment().catalog)
    selected_program = Program(
        str(_ROOT / "examples/pick_and_place_minimal.program.yaml"),
        build_node_environment().catalog,
    ).to_dict()
    selection = _selection(selected_program)
    headless = _Headless(selection)
    coordinator = _Coordinator()

    with pytest.raises(
        DraftSessionError,
        match="discard unsaved or unapplied Draft changes",
    ):
        _state(headless, coordinator, program=program).load_startup_scenario(
            selection["scenario_id"]
        )

    assert coordinator.executions == []
    assert headless.manager_apply_bodies == []


def test_current_scenario_requires_both_exact_owner_projections() -> None:
    program = Program(str(_PROGRAM_PATH), build_node_environment().catalog)
    selection = _selection(program.to_dict())
    headless = _Headless(selection)
    state = _state(headless, _Coordinator(), program=program)

    assert state.current_scenario()["ready"] is False
    headless.applied = _applied(selection)
    assert state.current_scenario()["unavailable_reasons"] == [
        "deployment_not_active_and_converged"
    ]
    headless.manager_target = True
    current = state.current_scenario()
    assert current["ready"] is True
    assert current["current_scenario"]["scenario_id"] == "lite6_pick_and_place"


def test_published_scenario_does_not_wait_for_runtime_convergence() -> None:
    program = Program(str(_PROGRAM_PATH), build_node_environment().catalog)
    selection = _selection(program.to_dict())
    headless = _Headless(selection)
    state = _state(headless, _Coordinator(), program=program)
    headless.applied = _applied(selection)
    headless.manager_target = True

    # The Manager has durably published the candidate, but its native/runtime
    # children have not converged yet.  Authoring must still see the exact
    # Manager Applied identity so Apply/Discard/Revert do not oscillate.
    original_status = headless.manager_status

    def published_but_pending():
        status = original_status()
        status["status"]["manager_ready"] = False
        status["status"]["runtime"]["converged"] = False
        return status

    headless.manager_status = published_but_pending  # type: ignore[method-assign]
    headless.manager_diff = lambda: {  # type: ignore[method-assign]
        "source": {
            "available": True,
            "freshness": "fresh",
            "known_gaps": [],
        },
        "converged": False,
    }

    published = state.published_scenario()
    assert published["ready"] is True
    assert published["published_scenario"]["deployment_revision"] == selection[
        "deployment_revision"
    ]
    assert state.current_scenario()["ready"] is False
    assert state.current_scenario()["unavailable_reasons"] == [
        "deployment_not_active_and_converged"
    ]


class _Clock:
    """Deterministic replacement for the state module's clock/sleep boundary."""

    def __init__(self) -> None:
        self.now = 0.0
        self.sleeps: list[float] = []

    def monotonic(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.sleeps.append(seconds)
        self.now += seconds


def _activation_case(
    monkeypatch: pytest.MonkeyPatch,
    *,
    unavailable_selection_reads: float,
) -> dict[str, Any]:
    """Prepare one Manager-backed Apply with a scripted Scenario-reopen head."""

    program = Program(str(_PROGRAM_PATH), build_node_environment().catalog)
    selection = _selection(program.to_dict())
    headless = _Headless(selection)
    headless.applied = _applied(selection)
    state = _state(headless, _Coordinator(), program=None)
    restored: list[Any] = []
    state._restore_authoring_post_image = restored.append  # type: ignore[method-assign]
    clock = _Clock()
    monkeypatch.setattr("manyforge_composer.backend.state.time", clock)

    compilations: list[dict[str, Any]] = []
    previews: list[dict[str, Any]] = []
    original_preview = headless.manager_deployment_candidate_preview

    def compile_candidate(body: dict[str, Any]) -> dict[str, Any]:
        compilations.append(deepcopy(body))
        return {
            "compilation": {
                "schema_version": (
                    "manyforge.deployment_candidate_compilation.v1"
                ),
                **{
                    field: selection[field]
                    for field in (
                        "scenario_id",
                        "scenario_catalog_revision",
                        "deployment_artifact_id",
                        "deployment_revision",
                        "retained_bundle_id",
                        "graph_revision",
                        "program_id",
                        "program_revision",
                        "program_binding_id",
                    )
                },
            }
        }

    def count_preview(body: dict[str, Any]) -> dict[str, Any]:
        previews.append(deepcopy(body))
        return original_preview(body)

    headless.compile_deployment_candidate = compile_candidate  # type: ignore[attr-defined]
    monkeypatch.setattr(
        headless, "manager_deployment_candidate_preview", count_preview
    )

    failures = {"remaining": unavailable_selection_reads}

    def scripted_selection(
        _scenario_id: str, *, timeout_seconds: float | None = None
    ) -> dict[str, Any]:
        assert timeout_seconds is None or timeout_seconds > 0
        if failures["remaining"] <= 0:
            return deepcopy(selection)
        failures["remaining"] -= 1
        raise HeadlessClientError(
            "scenario_program_selection_unavailable",
            "the Manager has not reopened the selected Scenario",
            status_code=409,
        )

    monkeypatch.setattr(headless, "scenario_load_selection", scripted_selection)

    intent = {
        "schema_version": "manyforge.composer_authoring_effect.v1",
        "operation_kind": "apply_deployment_candidate",
        "effect_class": "external",
        "external_kind": "deployment_candidate_apply",
        "scenario_id": selection["scenario_id"],
        "expected_scenario_catalog_revision": selection[
            "scenario_catalog_revision"
        ],
        "deployment": selection["deployment"],
        "program": selection["program"],
        "expected_applied_record_digest": headless.applied["source"][
            "record_digest"
        ],
        "expected_manager_current": {
            "retained_bundle_id": "sha256:" + "a" * 64,
            "graph_revision": "sha256:" + "b" * 64,
        },
        "expected_admission_generation": 7,
        "manager_request_id": "manager-request-1",
        "correlation_id": "composer-request-1",
        "confirmation_token": None,
        "preparation_request_owner_epoch": "browser-1",
        "preparation_request_id": "composer-request-1",
        "draft_id": "draft-1",
        "draft_revision": 1,
        "post_state": {},
    }
    claim = {"intent": {"claim_kind": "authoring_mutation"}}
    prepared = state.execute_authored_deployment_candidate(
        deepcopy(intent), claim, apply=True, prepare_only=True
    )
    assert len(compilations) == 1
    assert len(previews) == 1
    assert prepared["manager_apply"]["request_id"] == "manager-request-1"

    def settle_selection() -> None:
        failures["remaining"] = 0

    return {
        "claim": claim,
        "clock": clock,
        "compilations": compilations,
        "headless": headless,
        "intent": intent,
        "prepared": prepared,
        "previews": previews,
        "restored": restored,
        "settle_selection": settle_selection,
        "state": state,
    }


def test_candidate_apply_confirms_selection_inside_the_shared_deadline(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Selection before expiry is unchanged and spends only part of the budget."""

    case = _activation_case(monkeypatch, unavailable_selection_reads=2)
    prepared = deepcopy(case["prepared"])

    result = case["state"].execute_authored_deployment_candidate(
        case["intent"], case["claim"], apply=True, prepared=prepared
    )

    assert result["status"] == "applied"
    assert result["apply_id"] == "apply-1"
    # Exactly one Apply/compile/preview: the normal path never retries.
    assert len(case["headless"].manager_apply_bodies) == 1
    assert len(case["compilations"]) == 1
    assert len(case["previews"]) == 1
    assert case["headless"].program_apply_bodies == []
    assert len(case["restored"]) == 1
    # Two polls of the one shared budget; nothing expired.
    assert case["clock"].sleeps == [0.1, 0.1]
    budget = case["state"].deployment_observation_timeout_seconds
    assert budget == 20.0
    assert case["clock"].now < budget


def test_candidate_apply_selection_timeout_reports_the_accepted_apply(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Persistent selection unavailability fails closed after one deadline."""

    case = _activation_case(monkeypatch, unavailable_selection_reads=float("inf"))
    prepared = deepcopy(case["prepared"])

    with pytest.raises(AuthoringJournalError) as failure:
        case["state"].execute_authored_deployment_candidate(
            case["intent"], case["claim"], apply=True, prepared=prepared
        )

    assert failure.value.code == "deployment_candidate_selection_timeout"
    assert "Manager applied; selection confirmation timed out" in str(
        failure.value
    )
    # The accepted Manager Apply identity is reported for later recovery.
    assert "apply-1" in str(failure.value)
    assert "generation 1" in str(failure.value)
    # The single 20 s budget expired once; every poll consumed that same budget.
    budget = case["state"].deployment_observation_timeout_seconds
    assert budget == 20.0
    assert budget - 0.1 <= case["clock"].now < budget + 0.1
    assert all(
        sleep == pytest.approx(0.1, abs=1e-9)
        for sleep in case["clock"].sleeps
    )
    # Exactly one Manager submission was ever made and nothing else was tried.
    assert len(case["headless"].manager_apply_bodies) == 1
    assert (
        case["headless"].manager_apply_bodies[0]["request_id"]
        == "manager-request-1"
    )
    assert case["headless"].program_apply_bodies == []
    assert len(case["compilations"]) == 1
    assert len(case["previews"]) == 1
    # The retained preparation and its partial phase context survive intact.
    assert case["restored"] == []
    assert "manager_apply_ms" in prepared["phases_ms"]
    assert "scenario_selection_ms" not in prepared["phases_ms"]


def test_candidate_apply_selection_timeout_recovery_reuses_the_retained_request(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Recovery settles on the retained Apply request instead of a new one."""

    case = _activation_case(monkeypatch, unavailable_selection_reads=float("inf"))
    prepared = deepcopy(case["prepared"])
    with pytest.raises(AuthoringJournalError) as failure:
        case["state"].execute_authored_deployment_candidate(
            case["intent"], case["claim"], apply=True, prepared=prepared
        )
    assert failure.value.code == "deployment_candidate_selection_timeout"
    assert len(case["headless"].manager_apply_bodies) == 1

    # The Scenario reopened after the failed attempt.  Recovery replays the
    # journaled effect intent instead of authoring another candidate.
    case["settle_selection"]()
    retained = {
        **deepcopy(case["intent"]),
        "prepared_deployment_candidate": deepcopy(prepared),
    }
    result = case["state"].execute_authored_deployment_candidate(
        retained, case["claim"], apply=True
    )

    assert result["status"] == "applied"
    assert result["apply_id"] == "apply-1"
    # Recovery may re-issue the accepted Manager request over the transport, but
    # that is the only permitted repetition: every Manager call — the timed-out
    # attempt and the recovery call alike — must carry the one retained
    # (request_id, request_owner_epoch) pair and the one retained candidate
    # receipt.  A second request identity or candidate would be a new logical
    # Apply, which recovery must never author.
    manager_calls = case["headless"].manager_apply_bodies
    assert manager_calls
    assert {(body["request_id"], body["request_owner_epoch"]) for body in manager_calls} == {
        ("manager-request-1", "manager-owner-1")
    }
    assert {body["candidate_receipt"]["receipt_digest"] for body in manager_calls} == {
        "sha256:" + "8" * 64
    }
    assert len(case["compilations"]) == 1
    assert len(case["previews"]) == 1


def test_candidate_apply_shares_the_deadline_with_manager_status_observation(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Manager status waits consume the same deadline, never a fresh full one."""

    case = _activation_case(monkeypatch, unavailable_selection_reads=0)
    pending = {
        "api_version": "manyforge.manager_command_response.v1",
        "apply_id": "apply-1",
        "apply_generation": 1,
        "request_id": "manager-request-1",
        "request_owner_epoch": "manager-owner-1",
        "ok": True,
        "error": None,
        "state": "pending",
        "candidate_projection_revision": None,
    }

    def pending_apply(body: dict[str, Any]) -> dict[str, Any]:
        case["headless"].manager_apply_bodies.append(deepcopy(body))
        return deepcopy(pending)

    monkeypatch.setattr(case["headless"], "manager_apply", pending_apply)
    monkeypatch.setattr(
        case["headless"],
        "manager_apply_status",
        lambda _apply_id, _generation, *, timeout_seconds=None: (
            deepcopy(pending)
            if timeout_seconds is None or timeout_seconds > 0
            else pytest.fail("status timeout must be positive")
        ),
        raising=False,
    )

    with pytest.raises(AuthoringJournalError) as failure:
        case["state"].execute_authored_deployment_candidate(
            case["intent"],
            case["claim"],
            apply=True,
            prepared=deepcopy(case["prepared"]),
        )

    assert failure.value.code == "manager_apply_observation_timeout"
    # Only one Apply was ever submitted; the wait was on the terminal receipt.
    assert len(case["headless"].manager_apply_bodies) == 1
    budget = case["state"].deployment_observation_timeout_seconds
    assert budget == 20.0
    assert budget - 0.1 <= case["clock"].now < budget + 0.1


def test_candidate_apply_preserves_unconfirmed_effect_when_status_disconnects(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    case = _activation_case(monkeypatch, unavailable_selection_reads=0)
    pending = {
        "api_version": "manyforge.manager_command_response.v1",
        "apply_id": "apply-1",
        "apply_generation": 1,
        "request_id": "manager-request-1",
        "request_owner_epoch": "manager-owner-1",
        "ok": True,
        "error": None,
        "state": "pending",
        "candidate_projection_revision": None,
    }

    def accepted(body: dict[str, Any]) -> dict[str, Any]:
        case["headless"].manager_apply_bodies.append(deepcopy(body))
        return deepcopy(pending)

    monkeypatch.setattr(case["headless"], "manager_apply", accepted)
    monkeypatch.setattr(
        case["headless"],
        "manager_apply_status",
        lambda *_args, **_kwargs: (_ for _ in ()).throw(
            HeadlessClientError(
                "headless_http_unavailable", "status connection was lost"
            )
        ),
        raising=False,
    )

    with pytest.raises(AuthoringJournalError) as failure:
        case["state"].execute_authored_deployment_candidate(
            case["intent"],
            case["claim"],
            apply=True,
            prepared=deepcopy(case["prepared"]),
        )

    assert failure.value.code == "manager_apply_observation_interrupted"
    assert "effect remains unconfirmed" in str(failure.value)
    assert len(case["headless"].manager_apply_bodies) == 1


def test_candidate_apply_rejects_manager_status_returning_after_deadline(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """A call started in budget cannot make a late response timely."""

    case = _activation_case(monkeypatch, unavailable_selection_reads=0)
    case["state"].deployment_observation_timeout_seconds = 1.0
    pending = {
        "api_version": "manyforge.manager_command_response.v1",
        "apply_id": "apply-1",
        "apply_generation": 1,
        "request_id": "manager-request-1",
        "request_owner_epoch": "manager-owner-1",
        "ok": True,
        "error": None,
        "state": "pending",
        "candidate_projection_revision": None,
    }
    monkeypatch.setattr(
        case["headless"],
        "manager_apply",
        lambda body: (
            case["headless"].manager_apply_bodies.append(deepcopy(body))
            or deepcopy(pending)
        ),
    )
    observed_budgets: list[float] = []

    def late_status(
        _apply_id: str,
        _generation: int,
        *,
        timeout_seconds: float | None = None,
    ) -> dict[str, Any]:
        assert timeout_seconds is not None
        observed_budgets.append(timeout_seconds)
        case["clock"].now += timeout_seconds + 0.01
        return {
            **pending,
            "state": "succeeded",
            "candidate_projection_revision": {
                "retained_bundle_id": case["headless"].selection[
                    "retained_bundle_id"
                ],
                "graph_revision": case["headless"].selection["graph_revision"],
            },
        }

    monkeypatch.setattr(
        case["headless"], "manager_apply_status", late_status, raising=False
    )

    with pytest.raises(AuthoringJournalError) as failure:
        case["state"].execute_authored_deployment_candidate(
            case["intent"],
            case["claim"],
            apply=True,
            prepared=deepcopy(case["prepared"]),
        )

    assert failure.value.code == "manager_apply_observation_timeout"
    assert len(case["headless"].manager_apply_bodies) == 1
    assert len(observed_budgets) == 1
    assert 0 < observed_budgets[0] < 1.0


def test_candidate_apply_rejects_selection_returning_after_deadline(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Late Scenario selection is not accepted after Manager success."""

    case = _activation_case(monkeypatch, unavailable_selection_reads=0)
    case["state"].deployment_observation_timeout_seconds = 1.0
    observed_budgets: list[float] = []

    def late_selection(
        _scenario_id: str, *, timeout_seconds: float | None = None
    ) -> dict[str, Any]:
        assert timeout_seconds is not None
        observed_budgets.append(timeout_seconds)
        case["clock"].now += timeout_seconds + 0.01
        return deepcopy(case["headless"].selection)

    monkeypatch.setattr(
        case["headless"], "scenario_load_selection", late_selection
    )

    with pytest.raises(AuthoringJournalError) as failure:
        case["state"].execute_authored_deployment_candidate(
            case["intent"],
            case["claim"],
            apply=True,
            prepared=deepcopy(case["prepared"]),
        )

    assert failure.value.code == "deployment_candidate_selection_timeout"
    assert len(case["headless"].manager_apply_bodies) == 1
    assert len(observed_budgets) == 1
    assert 0 < observed_budgets[0] <= 1.0
    assert case["headless"].program_apply_bodies == []
