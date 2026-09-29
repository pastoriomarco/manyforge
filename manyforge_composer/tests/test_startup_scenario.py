from __future__ import annotations

from copy import deepcopy
import hashlib
import json
from pathlib import Path
from threading import Lock
from typing import Any

import pytest

from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.program import Program
from manyforge_composer.backend.draft_session import (
    ComposerDraftSession,
    DraftSessionError,
)
from manyforge_composer.backend.authoring_workspace import AuthoringWorkspace
from manyforge_composer.backend.deployment_draft import (
    CompiledDeploymentDocument,
    DeploymentDraftSession,
)
from manyforge_composer.backend.state import ComposerState
from manyforge_headless_api.client import HeadlessClientError


_ROOT = Path(__file__).resolve().parents[2]
_PROGRAM_PATH = _ROOT / "examples/lite6_lite_gripper_pick_and_place.program.yaml"
_DEPLOYMENT_PATH = (
    _ROOT / "examples/lite6_lite_gripper_pick_and_place.deployment.yaml"
)


def _fixture_deployment_compiler(source: str) -> CompiledDeploymentDocument:
    """Project orchestration fixture; native compilation is tested separately."""

    document = json.loads(source)
    canonical = json.dumps(document, sort_keys=True, separators=(",", ":"))
    graph_revision = "sha256:" + hashlib.sha256(
        json.dumps(
            document["graph"], sort_keys=True, separators=(",", ":")
        ).encode()
    ).hexdigest()
    return CompiledDeploymentDocument(
        document=document,
        canonical_json=canonical,
        deployment_artifact_id=document["deployment_artifact_id"],
        deployment_revision=document["deployment_revision"],
        graph_revision=graph_revision,
    )


def _selection(program: dict[str, Any]) -> dict[str, Any]:
    return {
        "scenario_id": "lite6_pick_and_place",
        "source_document_digest": "sha256:" + "2" * 64,
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


def _non_executable_outcome(
    selection: dict[str, Any], *, command_target_id: str = "jaw"
) -> dict[str, Any]:
    return {
        "affected_execution_groups": ["grasp"],
        "affected_nodes": ["close_gripper"],
        "api_version": "manyforge.program-binding-outcome.v1",
        "graph_revision": selection["graph_revision"],
        "program_id": selection["program_id"],
        "program_revision": selection["program_revision"],
        "retained_bundle_id": selection["retained_bundle_id"],
        "role_assignment_digest": "sha256:" + "a" * 64,
        "unresolved_roles": [
            {
                "command_target_id": command_target_id,
                "component_role_id": "gripper",
                "controllable_component_id": "gripper-1",
                "detail": "The assigned target is not present in this Deployment.",
                "reason": "target_missing",
                "target_role_id": "jaw",
            }
        ],
    }


def _non_executable_status(
    selection: dict[str, Any], *, command_target_id: str = "jaw"
) -> dict[str, Any]:
    return {
        "deployment_artifact_id": selection["deployment_artifact_id"],
        "deployment_revision": selection["deployment_revision"],
        "executable": False,
        "graph_revision": selection["graph_revision"],
        "message": "Program references a gripper target removed from this Deployment.",
        "non_executable": _non_executable_outcome(
            selection, command_target_id=command_target_id
        ),
        "program": deepcopy(selection["program"]),
        "program_binding_id": None,
        "program_id": selection["program_id"],
        "program_revision": selection["program_revision"],
        "record_digest": "sha256:" + "b" * 64,
        "retained_bundle_id": selection["retained_bundle_id"],
        "scenario_id": selection["scenario_id"],
        "schema_version": "manyforge.applied_program_status.v2",
        "source_document_digest": selection["source_document_digest"],
    }


def _captured_activation(
    state: ComposerState,
    selection: dict[str, Any],
    program_outcome: dict[str, Any],
) -> tuple[Any, dict[str, Any]]:
    intent = {
        "deployment": deepcopy(selection["deployment"]),
        "model_source_closure": {},
        "program_role_assignment": {},
        "typed_parameter_overrides": {},
        "source_document_digest": selection["source_document_digest"],
        "scenario_id": selection["scenario_id"],
    }
    prepared = {
        "deployment_revision": selection["deployment_revision"],
        "graph_revision": selection["graph_revision"],
        "program_id": selection["program_id"],
        "program_revision": selection["program_revision"],
        "program_outcome": deepcopy(program_outcome),
        "retained_bundle_id": selection["retained_bundle_id"],
        "scenario_id": selection["scenario_id"],
        "source_document_digest": selection["source_document_digest"],
    }
    return state._captured_deployment_context(intent, prepared)


class _Headless:
    def __init__(self, selection: dict[str, Any]) -> None:
        self.selection = selection
        self.applied: dict[str, Any] | None = None
        self.applied_status: dict[str, Any] | None = None
        self.manager_target = False
        self.manager_apply_bodies: list[dict[str, Any]] = []

    def program_applied(self) -> dict[str, Any]:
        if self.applied is None:
            raise HeadlessClientError(
                "program_applied_selection_not_found",
                "no Program is Applied",
                status_code=404,
            )
        return deepcopy(self.applied)

    def program_applied_status(self) -> dict[str, Any]:
        if self.applied_status is not None:
            return deepcopy(self.applied_status)
        if self.applied is None:
            raise HeadlessClientError(
                "program_applied_selection_not_found",
                "no Program is Applied",
                status_code=404,
            )
        applied = self.applied
        executable = applied.get("program_binding_id") is not None
        return {
            "deployment_artifact_id": applied["deployment_artifact_id"],
            "deployment_revision": applied["deployment_revision"],
            "executable": executable,
            "graph_revision": applied["graph_revision"],
            "message": None,
            "non_executable": None,
            "program": None,
            "program_binding_id": applied.get("program_binding_id"),
            "program_id": applied["program_id"],
            "program_revision": applied["program_revision"],
            "record_digest": applied["source"]["record_digest"],
            "retained_bundle_id": applied["retained_bundle_id"],
            "scenario_id": applied["scenario_id"],
            "schema_version": "manyforge.applied_program_status.v2",
            "source_document_digest": applied["source_document_digest"],
        }

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
                "schema_version": "manyforge.manager_deployment_candidate_receipt.v2",
                "deployment_artifact_id": body["deployment_artifact_id"],
                "deployment_revision": body["deployment_revision"],
                "source_document_digest": self.selection[
                    "source_document_digest"
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
                "impact_digest": "sha256:" + "c" * 64,
                "destructive_impact": False,
                "receipt_digest": "sha256:" + "8" * 64,
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
    state.deployment_draft_session = DeploymentDraftSession(
        compiler=_fixture_deployment_compiler
    )
    state.authoring_workspace = AuthoringWorkspace(
        program_session=state.draft_session,
        deployment_session=state.deployment_draft_session,
    )
    state.authoring_workspace.remember_project(
        document=None,
        document_path=str(_DEPLOYMENT_PATH),
    )
    state._active_authoring_catalog = lambda: state.node_catalog  # type: ignore[method-assign]
    state._applied_deployment_lock = Lock()
    state.applied_deployment_document = None
    state._applied_activation_context = None
    state._applied_deployment_identity = None
    state._pending_applied_deployment = None
    return state


@pytest.mark.parametrize("replay_status", ["accepted_new", "replayed_exact"])
def test_candidate_apply_returns_acceptance_without_polling(
    monkeypatch: pytest.MonkeyPatch,
    replay_status: str,
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
    monkeypatch.setattr(headless, "manager_apply", lambda _body: deepcopy(acceptance))
    monkeypatch.setattr(
        headless,
        "manager_apply_status",
        lambda *_args, **_kwargs: pytest.fail(
            "Composer must not poll an accepted Manager operation"
        ),
        raising=False,
    )
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
    result = state.apply_manager_deployment_candidate(intent, {})
    assert result == acceptance
    assert result["state"] == "pending"


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


def test_no_program_publication_promotes_capture_without_becoming_executable() -> None:
    program = Program(str(_PROGRAM_PATH), build_node_environment().catalog)
    selection = _selection(program.to_dict())
    headless = _Headless(selection)
    headless.applied_status = _non_executable_status(selection)
    headless.applied_status.update(
        program_id=None, program_revision=None, program=None, non_executable=None,
        message="No Program is selected",
    )
    headless.manager_target = True
    state = _state(headless, _Coordinator(), program=None)
    selection.update(program_id=None, program_revision=None)
    state._retain_submitted_deployment(_captured_activation(state, selection, {"kind": "no_program"}))
    published = state.published_scenario()
    assert published["ready"] is True
    assert published["published_scenario"]["program_outcome"] == {"kind": "no_program"}
    assert state.applied_activation_for_publication(published["published_scenario"]) is not None
    assert state.current_scenario()["unavailable_reasons"] == ["program_not_selected"]


def test_non_executable_publication_promotes_exact_capture_but_is_not_runnable() -> None:
    program = Program(str(_PROGRAM_PATH), build_node_environment().catalog)
    selection = _selection(program.to_dict())
    headless = _Headless(selection)
    headless.applied_status = _non_executable_status(selection)
    headless.manager_target = True
    state = _state(headless, _Coordinator(), program=program)
    outcome = {
        "kind": "non_executable",
        "outcome": _non_executable_outcome(selection),
    }
    captured = _captured_activation(state, selection, outcome)
    state._retain_submitted_deployment(captured)

    published = state.published_scenario()
    assert published["ready"] is True
    projection = published["published_scenario"]
    assert projection["program_outcome"] == outcome
    assert state.applied_activation_for_publication(projection) is not None

    current = state.current_scenario()
    assert current["ready"] is False
    assert current["current_scenario"] is None
    assert current["unavailable_reasons"] == ["program_not_executable"]


@pytest.mark.parametrize(
    ("capture_outcome_kind", "published_outcome_kind", "published_target"),
    [
        ("bound", "non_executable", "jaw"),
        ("non_executable", "bound", "jaw"),
        ("non_executable", "non_executable", "different-jaw"),
    ],
)
def test_activation_capture_promotes_only_for_the_same_program_outcome(
    capture_outcome_kind: str,
    published_outcome_kind: str,
    published_target: str,
) -> None:
    program = Program(str(_PROGRAM_PATH), build_node_environment().catalog)
    selection = _selection(program.to_dict())
    headless = _Headless(selection)
    headless.manager_target = True
    if published_outcome_kind == "bound":
        headless.applied = _applied(selection)
    else:
        headless.applied_status = _non_executable_status(
            selection, command_target_id=published_target
        )
    state = _state(headless, _Coordinator(), program=program)
    if capture_outcome_kind == "bound":
        outcome = {
            "kind": "bound",
            "program_binding_id": selection["program_binding_id"],
        }
    else:
        outcome = {
            "kind": "non_executable",
            "outcome": _non_executable_outcome(selection),
        }
    state._retain_submitted_deployment(
        _captured_activation(state, selection, outcome)
    )

    publication = state.published_scenario()["published_scenario"]
    assert publication is not None
    assert state.applied_activation_for_publication(publication) is None
    assert state.applied_deployment_document is None


def test_contradictory_applied_program_status_is_not_a_publication() -> None:
    program = Program(str(_PROGRAM_PATH), build_node_environment().catalog)
    selection = _selection(program.to_dict())
    headless = _Headless(selection)
    headless.manager_target = True
    headless.applied_status = _non_executable_status(selection)
    headless.applied_status["executable"] = True
    state = _state(headless, _Coordinator(), program=program)

    published = state.published_scenario()
    assert published["ready"] is False
    assert published["published_scenario"] is None
    assert published["unavailable_reasons"] == [
        "program_applied_status_invalid"
    ]
