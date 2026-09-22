from __future__ import annotations

from copy import deepcopy
import hashlib
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
    state.deployment_draft_session = DeploymentDraftSession(
        compiler=_fixture_deployment_compiler
    )
    state._active_authoring_catalog = lambda: state.node_catalog  # type: ignore[method-assign]
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
