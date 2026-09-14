from __future__ import annotations

from copy import deepcopy
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

    def scenario_load_selection(self, scenario_id: str) -> dict[str, Any]:
        assert scenario_id == self.selection["scenario_id"]
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
        return {"ok": True, "state": "succeeded"}

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

    def __init__(self) -> None:
        self.executions: list[dict[str, Any]] = []

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
        candidate_projection_revision={"retained_bundle_id": "candidate"},
    )
    monkeypatch.setattr(headless, "manager_apply", lambda _body: deepcopy(acceptance))

    def status(apply_id: str, generation: int) -> dict[str, Any]:
        assert (apply_id, generation) == ("apply-1", 1)
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
