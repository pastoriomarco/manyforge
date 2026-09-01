from __future__ import annotations

from copy import deepcopy
import hashlib
import json
from pathlib import Path
from typing import Any

from fastapi import FastAPI
from fastapi.testclient import TestClient

from manyforge_composer.backend.deployment_draft import (
    CompiledDeploymentDocument,
    DeploymentDraftError,
    DeploymentDraftSession,
)
from manyforge_composer.backend.routes_deployment_draft import (
    create_deployment_draft_router,
)
from manyforge_composer.backend.routes_headless import create_headless_router
from manyforge_composer.backend.state import ComposerState


_DIGEST = "sha256:" + "1" * 64


def _compile(source: str) -> CompiledDeploymentDocument:
    document = json.loads(source)
    if not isinstance(document, dict) or document.get("api_version") != "manyforge.deployment.v1":
        raise DeploymentDraftError(
            "deployment_draft_invalid", "unsupported Deployment document"
        )
    if not isinstance(document.get("graph"), dict):
        raise DeploymentDraftError(
            "deployment_draft_invalid", "Deployment graph is required"
        )
    graph = deepcopy(document["graph"])
    graph.pop("graph_revision", None)
    graph_basis = json.dumps(graph, sort_keys=True, separators=(",", ":"))
    graph_revision = "sha256:" + hashlib.sha256(graph_basis.encode()).hexdigest()
    graph["graph_revision"] = graph_revision
    document["graph"] = graph
    identity_basis = deepcopy(document)
    identity_basis.pop("deployment_revision", None)
    canonical_basis = json.dumps(identity_basis, sort_keys=True, separators=(",", ":"))
    revision = "sha256:" + hashlib.sha256(canonical_basis.encode()).hexdigest()
    document["deployment_revision"] = revision
    canonical = json.dumps(document, sort_keys=True, separators=(",", ":"))
    return CompiledDeploymentDocument(
        document=document,
        canonical_json=canonical,
        deployment_artifact_id=document["deployment_artifact_id"],
        deployment_revision=revision,
        graph_revision=graph_revision,
    )


def _document() -> dict[str, Any]:
    return {
        "api_version": "manyforge.deployment.v1",
        "deployment_artifact_id": "cell_a",
        "deployment_revision": _DIGEST,
        "cell_id": "cell_a",
        "name": "Cell A",
        "graph": {"runtime_units": []},
    }


def _focused_document() -> dict[str, Any]:
    document = _document()
    document["graph"]["runtime_units"] = [
        {
            "runtime_unit_id": "robot.native_execution",
            "configuration": {
                "mode": "simulated",
                "execution_tick_hz": 100,
                "collision_monitor_period_ms": 50,
                "planner_rng_seed": 12345,
                "initial_joint_positions_rad": [0.0, 1.0],
                "motion_target_limits": [
                    {
                        "controllable_component_id": "robot",
                        "command_target_id": "arm",
                        "maximum_tcp_linear_speed_mps": 0.15,
                    }
                ],
            },
        }
    ]
    stop_parameters = {
        "allow_natural_completion_on_short_path": True,
        "deceleration_time_s": 1.0,
        "per_resource_limits": [
            {
                "resource_id": "robot.arm.coordinate.000000",
                "max_deceleration": {
                    "coordinate_kind": "angular_rad_s2",
                    "value_rad_s2": 4.0,
                },
                "settle_velocity_threshold": {
                    "coordinate_kind": "angular_rad_s",
                    "value_rad_s": 0.01,
                },
            }
        ],
        "stop_mode": "soft_on_path",
    }
    document["policies"] = {
        "stop_stage_parameters": [
            {
                "control_binding_id": "route-a",
                "stop_escalation_stage_id": "motion_controlled_stop",
                "provider_contract_ref": {
                    "provider_contract_role_id": "stop_contract"
                },
                "default_contract_parameters": deepcopy(stop_parameters),
            },
            {
                "control_binding_id": "route-b",
                "stop_escalation_stage_id": "motion_controlled_stop",
                "provider_contract_ref": {
                    "provider_contract_role_id": "stop_contract"
                },
                "default_contract_parameters": deepcopy(stop_parameters),
            },
            {
                "control_binding_id": "route-a",
                "stop_escalation_stage_id": "software_hard",
                "provider_contract_ref": {
                    "provider_contract_role_id": "software_hard_stop"
                },
                "default_contract_parameters": {},
            },
        ]
    }
    return document


class _Coordinator:
    def execute(
        self,
        _envelope,
        *,
        prepare_effect,
        validate_under_claim,
        effect,
        post_image,
        after_release=None,
        **_kwargs,
    ):
        prepare_effect()
        if validate_under_claim is not None:
            validate_under_claim({"claim": "test"})
        result = effect({"claim": "test"})
        post_image()
        if after_release is not None:
            after_release()
        return result


class _Headless:
    def scenarios(self):
        return {"scenario_catalog_revision": _DIGEST, "scenarios": []}

    def scenario_program_selection(self, _scenario_id: str):
        return {"program_binding": {"program_binding_id": _DIGEST}}


def _request(workspace: dict[str, Any], **extra: Any) -> dict[str, Any]:
    return {
        "draftId": workspace["draft"]["id"],
        "expectedDraftRevision": workspace["draft"]["revision"],
        "expectedAdmissionGeneration": 1,
        "requestOwnerEpoch": "browser-test",
        "requestId": "request-" + str(workspace["draft"]["revision"]) + "-" + extra.pop("kind", "edit"),
        **extra,
    }


def _client(tmp_path: Path) -> tuple[TestClient, ComposerState]:
    state = ComposerState()
    state.deployment_draft_session = DeploymentDraftSession(compiler=_compile)
    state.authoring_admission = _Coordinator()  # type: ignore[assignment]
    state.headless_client = _Headless()  # type: ignore[assignment]
    state.allowed_fs_roots = [str(tmp_path)]
    state.allowed_deployment_write_roots = [str(tmp_path)]
    state.current_scenario = lambda: {  # type: ignore[method-assign]
        "ready": False,
        "current_scenario": None,
        "unavailable_reasons": ["not_applied"],
    }
    app = FastAPI()
    app.include_router(create_deployment_draft_router(state))
    return TestClient(app), state


def test_deployment_open_edit_save_discard_share_one_revision_fenced_workspace(
    tmp_path: Path,
):
    client, state = _client(tmp_path)
    source = tmp_path / "cell_a.deployment.yaml"
    source.write_text(json.dumps(_document()), encoding="utf-8")

    workspace = client.get("/api/deployment/workspace").json()
    response = client.post(
        "/api/deployment/open-draft",
        json=_request(workspace, path=str(source), confirmDiscard=False, kind="open"),
    )
    assert response.status_code == 200, response.text
    workspace = response.json()
    assert workspace["draft"]["deploymentArtifactId"] == "cell_a"
    assert workspace["draft"]["differsFromSaved"] is False

    response = client.post(
        "/api/deployment/draft/mutations",
        json=_request(workspace, path=["name"], value="Edited cell"),
    )
    assert response.status_code == 200, response.text
    workspace = response.json()
    assert workspace["draft"]["document"]["name"] == "Edited cell"
    assert workspace["draft"]["differsFromSaved"] is True

    saved = tmp_path / "saved"
    response = client.post(
        "/api/deployment/save-as",
        json=_request(workspace, path=str(saved), confirmOverwrite=False, kind="save"),
    )
    assert response.status_code == 200, response.text
    workspace = response.json()
    assert workspace["saved"]["path"].endswith("saved.deployment.yaml")
    assert json.loads((tmp_path / "saved.deployment.yaml").read_text())["name"] == "Edited cell"

    response = client.post(
        "/api/deployment/draft/mutations",
        json=_request(workspace, path=["name"], value="Unsaved"),
    )
    workspace = response.json()
    response = client.post(
        "/api/deployment/discard",
        json=_request(workspace, kind="discard"),
    )
    assert response.status_code == 200, response.text
    assert response.json()["draft"]["document"]["name"] == "Edited cell"
    assert state.authoring_retained_state()["schema_version"] == "manyforge.composer_authoring_workspace.v2"


def test_invalid_or_stale_deployment_mutation_never_changes_the_draft(
    tmp_path: Path,
) -> None:
    client, _state = _client(tmp_path)
    source = tmp_path / "cell_a.deployment.yaml"
    source.write_text(json.dumps(_document()), encoding="utf-8")
    initial = client.get("/api/deployment/workspace").json()
    opened = client.post(
        "/api/deployment/open-draft",
        json=_request(initial, path=str(source), confirmDiscard=False, kind="open"),
    ).json()

    invalid = client.post(
        "/api/deployment/draft/mutations",
        json=_request(opened, path=["graph"], value=None, kind="invalid"),
    )
    assert invalid.status_code == 400
    assert invalid.json()["detail"]["code"] == "deployment_draft_invalid"
    after_invalid = client.get("/api/deployment/workspace").json()
    assert after_invalid["draft"] == opened["draft"]

    accepted = client.post(
        "/api/deployment/draft/mutations",
        json=_request(opened, path=["name"], value="Current", kind="accepted"),
    )
    assert accepted.status_code == 200
    current = accepted.json()
    redundant = client.post(
        "/api/deployment/draft/mutations",
        json=_request(current, path=["name"], value="Current", kind="redundant"),
    )
    assert redundant.status_code == 200
    assert redundant.json()["draft"]["revision"] == current["draft"]["revision"]
    stale = client.post(
        "/api/deployment/draft/mutations",
        json=_request(opened, path=["name"], value="Stale", kind="stale"),
    )
    assert stale.status_code == 409
    assert stale.json()["detail"]["code"] == "deployment_draft_stale"
    assert client.get("/api/deployment/workspace").json()["draft"]["document"][
        "name"
    ] == "Current"


def test_authored_deployment_preview_then_apply_preserves_the_exact_program(
    tmp_path: Path,
) -> None:
    client, state = _client(tmp_path)
    source = tmp_path / "cell_a.deployment.yaml"
    source.write_text(json.dumps(_document()), encoding="utf-8")
    initial = client.get("/api/deployment/workspace").json()
    opened = client.post(
        "/api/deployment/open-draft",
        json=_request(
            initial, path=str(source), confirmDiscard=False, kind="open"
        ),
    ).json()
    base_revision = opened["draft"]["deploymentRevision"]
    edited = client.post(
        "/api/deployment/draft/mutations",
        json=_request(opened, path=["name"], value="Authored", kind="edit"),
    ).json()
    candidate_revision = edited["draft"]["deploymentRevision"]
    assert candidate_revision != base_revision
    program = {
        "api_version": "manyforge.program.v2",
        "program_id": "program_a",
        "program_revision": "sha256:" + "7" * 64,
    }
    current = {
        "scenario_id": "cell_a",
        "scenario_catalog_revision": "sha256:" + "6" * 64,
        "deployment_artifact_id": "cell_a",
        "deployment_revision": base_revision,
        "retained_bundle_id": "sha256:" + "2" * 64,
        "graph_revision": "sha256:" + "3" * 64,
        "program_id": "program_a",
        "program_revision": program["program_revision"],
        "program_binding_id": "sha256:" + "5" * 64,
    }

    class CandidateHeadless(_Headless):
        def program_applied(self):
            return {
                **current,
                "program": deepcopy(program),
                "source": {"record_digest": "sha256:" + "8" * 64},
            }

        def manager_status(self):
            return {
                "source": {
                    "available": True,
                    "freshness": "fresh",
                    "known_gaps": [],
                    "source_owner_epoch": "manager.epoch.1",
                },
                "status": {
                    "manager_ready": True,
                    "active": {
                        "retained_bundle_id": current["retained_bundle_id"],
                        "graph_revision": current["graph_revision"],
                    },
                },
            }

    state.headless_client = CandidateHeadless()  # type: ignore[assignment]
    state.current_scenario = lambda: {  # type: ignore[method-assign]
        "ready": True,
        "current_scenario": deepcopy(current),
        "unavailable_reasons": [],
    }
    effects: list[tuple[dict[str, Any], bool]] = []

    def execute_candidate(intent, _claim, *, apply):
        effects.append((deepcopy(intent), apply))
        compilation = {
            "schema_version": "manyforge.deployment_candidate_compilation.v1",
            "scenario_id": current["scenario_id"],
            "scenario_catalog_revision": "sha256:" + "9" * 64,
            "deployment_artifact_id": "cell_a",
            "deployment_revision": candidate_revision,
            "retained_bundle_id": "sha256:" + "a" * 64,
            "graph_revision": edited["draft"]["graphRevision"],
            "program_id": "program_a",
            "program_revision": program["program_revision"],
            "program_binding_id": "sha256:" + "b" * 64,
        }
        if apply:
            current.update({
                key: value
                for key, value in compilation.items()
                if key != "schema_version"
            })
        return {
            "status": "applied" if apply else "previewed",
            "compilation": compilation,
            "candidate_preview": None,
            "manager_apply": None,
            "selection": {
                key: value
                for key, value in compilation.items()
                if key != "schema_version"
            },
        }

    state.execute_authored_deployment_candidate = execute_candidate  # type: ignore[method-assign]
    prepared: list[tuple[str, str]] = []
    state.prepare_current_applied_scene = lambda **kwargs: prepared.append(  # type: ignore[method-assign]
        (kwargs["request_owner_epoch"], kwargs["request_id"])
    )

    preview = client.post(
        "/api/deployment/draft/preview",
        json=_request(edited, kind="preview"),
    )
    assert preview.status_code == 200, preview.text
    assert preview.json()["result"]["status"] == "previewed"
    assert current["deployment_revision"] == base_revision
    assert effects[-1][0]["program"] == program
    assert effects[-1][0]["deployment"] == edited["draft"]["document"]
    assert prepared == []

    applied = client.post(
        "/api/deployment/draft/apply",
        json=_request(edited, kind="apply"),
    )
    assert applied.status_code == 200, applied.text
    assert applied.json()["result"]["status"] == "applied"
    assert applied.json()["workspace"]["runAdmission"]["ready"] is True
    assert current["program_revision"] == program["program_revision"]
    assert prepared == [("browser-test", "request-2-apply")]


def test_focused_motion_target_limit_updates_one_exact_target_atomically(
    tmp_path: Path,
) -> None:
    client, _state = _client(tmp_path)
    source = tmp_path / "cell_a.deployment.yaml"
    source.write_text(json.dumps(_focused_document()), encoding="utf-8")
    initial = client.get("/api/deployment/workspace").json()
    opened = client.post(
        "/api/deployment/open-draft",
        json=_request(initial, path=str(source), confirmDiscard=False, kind="open"),
    ).json()

    updated = client.post(
        "/api/deployment/draft/motion-target-limit",
        json=_request(
            opened,
            controllable_component_id="robot",
            command_target_id="arm",
            maximum_tcp_linear_speed_mps=0.1,
            kind="target-limit",
        ),
    )
    assert updated.status_code == 200, updated.text
    current = updated.json()
    assert current["draft"]["document"]["graph"]["runtime_units"][0][
        "configuration"
    ]["motion_target_limits"][0]["maximum_tcp_linear_speed_mps"] == 0.1
    assert current["draft"]["revision"] == opened["draft"]["revision"] + 1

    rejected = client.post(
        "/api/deployment/draft/motion-target-limit",
        json=_request(
            current,
            controllable_component_id="other",
            command_target_id="arm",
            maximum_tcp_linear_speed_mps=0.05,
            kind="wrong-target",
        ),
    )
    assert rejected.status_code == 409
    assert rejected.json()["detail"]["code"] == (
        "deployment_motion_target_limit_scope_mismatch"
    )
    assert client.get("/api/deployment/workspace").json()["draft"] == current["draft"]


def test_focused_controlled_stop_updates_all_derived_routes_and_not_hard_stop(
    tmp_path: Path,
) -> None:
    client, _state = _client(tmp_path)
    source = tmp_path / "cell_a.deployment.yaml"
    source.write_text(json.dumps(_focused_document()), encoding="utf-8")
    initial = client.get("/api/deployment/workspace").json()
    opened = client.post(
        "/api/deployment/open-draft",
        json=_request(initial, path=str(source), confirmDiscard=False, kind="open"),
    ).json()
    profile = {
        "stop_mode": "spring_back_hold",
        "deceleration_time_s": 1.25,
        "allow_natural_completion_on_short_path": False,
        "per_resource_limits": [
            {
                "resource_id": "robot.arm.coordinate.000000",
                "max_deceleration": {
                    "coordinate_kind": "angular_rad_s2",
                    "value": 3.5,
                },
                "settle_velocity_threshold": {
                    "coordinate_kind": "angular_rad_s",
                    "value": 0.02,
                },
            }
        ],
    }
    updated = client.post(
        "/api/deployment/draft/controlled-stop",
        json=_request(opened, profile=profile, kind="controlled-stop"),
    )
    assert updated.status_code == 200, updated.text
    rows = updated.json()["draft"]["document"]["policies"][
        "stop_stage_parameters"
    ]
    controlled = [
        row for row in rows
        if row["stop_escalation_stage_id"] == "motion_controlled_stop"
    ]
    assert len(controlled) == 2
    assert all(
        row["default_contract_parameters"] == {
            "allow_natural_completion_on_short_path": False,
            "deceleration_time_s": 1.25,
            "per_resource_limits": [
                {
                    "resource_id": "robot.arm.coordinate.000000",
                    "max_deceleration": {
                        "coordinate_kind": "angular_rad_s2",
                        "value_rad_s2": 3.5,
                    },
                    "settle_velocity_threshold": {
                        "coordinate_kind": "angular_rad_s",
                        "value_rad_s": 0.02,
                    },
                }
            ],
            "stop_mode": "spring_back_hold",
        }
        for row in controlled
    )
    assert rows[2]["default_contract_parameters"] == {}


def test_deployment_artifacts_remain_confined_and_save_as_requires_overwrite(
    tmp_path: Path,
) -> None:
    client, _state = _client(tmp_path)
    source = tmp_path / "cell_a.deployment.yaml"
    source.write_text(json.dumps(_document()), encoding="utf-8")
    outside = tmp_path.parent / f"{tmp_path.name}-outside.deployment.yaml"
    outside.write_text(json.dumps(_document()), encoding="utf-8")
    try:
        initial = client.get("/api/deployment/workspace").json()
        rejected_open = client.post(
            "/api/deployment/open-draft",
            json=_request(
                initial,
                path=str(outside),
                confirmDiscard=False,
                kind="outside",
            ),
        )
        assert rejected_open.status_code == 403
        assert rejected_open.json()["detail"]["code"] == (
            "deployment_artifact_path_invalid"
        )

        opened = client.post(
            "/api/deployment/open-draft",
            json=_request(initial, path=str(source), confirmDiscard=False, kind="open"),
        ).json()
        existing = tmp_path / "existing.deployment.yaml"
        existing.write_text("unchanged\n", encoding="utf-8")
        rejected_save = client.post(
            "/api/deployment/save-as",
            json=_request(
                opened,
                path=str(existing),
                confirmOverwrite=False,
                kind="save-existing",
            ),
        )
        assert rejected_save.status_code == 409
        assert rejected_save.json()["detail"]["code"] == (
            "deployment_artifact_overwrite_confirmation_required"
        )
        assert existing.read_text(encoding="utf-8") == "unchanged\n"
        assert client.get("/api/deployment/workspace").json()["draft"] == opened[
            "draft"
        ]
    finally:
        outside.unlink(missing_ok=True)


def test_missing_deployment_artifact_has_a_stable_not_found_error(
    tmp_path: Path,
) -> None:
    client, _state = _client(tmp_path)
    workspace = client.get("/api/deployment/workspace").json()
    response = client.post(
        "/api/deployment/open-draft",
        json=_request(
            workspace,
            path=str(tmp_path / "missing.deployment.yaml"),
            confirmDiscard=False,
            kind="missing",
        ),
    )
    assert response.status_code == 404
    assert response.json()["detail"]["code"] == "deployment_artifact_not_found"


def test_deployment_draft_retained_state_round_trips_exactly() -> None:
    source = DeploymentDraftSession(compiler=_compile)
    initial = source.retained_state()
    compiled = source.preview_source(json.dumps(_document()))
    source.commit_open(
        compiled,
        "/workspace/cell_a.deployment.yaml",
        draft_id=initial["draft_id"],
        expected_revision=initial["draft_revision"],
        confirm_discard=False,
    )
    opened = source.snapshot()
    changed = source.preview_update(
        ["name"],
        "Retained edit",
        draft_id=opened["draft"]["id"],
        expected_revision=opened["draft"]["revision"],
    )
    source.commit_compiled(
        changed,
        draft_id=opened["draft"]["id"],
        expected_revision=opened["draft"]["revision"],
    )

    retained = source.retained_state()
    restored = DeploymentDraftSession(compiler=_compile)
    restored.restore_retained_state(retained)

    assert restored.retained_state() == retained
    assert restored.snapshot() == source.snapshot()


def test_named_scenario_candidates_project_the_complete_exact_pairing(
    tmp_path: Path,
) -> None:
    client, state = _client(tmp_path)
    source = tmp_path / "cell_a.deployment.yaml"
    source.write_text(json.dumps(_document()), encoding="utf-8")
    initial = client.get("/api/deployment/workspace").json()
    opened = client.post(
        "/api/deployment/open-draft",
        json=_request(initial, path=str(source), confirmDiscard=False, kind="open"),
    ).json()
    deployment_revision = opened["draft"]["deploymentRevision"]
    state.headless_client.scenarios = lambda: {  # type: ignore[method-assign]
        "scenario_catalog_revision": _DIGEST,
        "scenarios": [{
            "scenario_id": "cell-a",
            "description": "Cell A",
            "deployment_artifact_id": "cell_a",
            "deployment_revision": deployment_revision,
            "retained_bundle_id": _DIGEST,
            "program_id": "pick",
            "program_revision": _DIGEST,
            "program_binding_id": _DIGEST,
        }],
    }

    candidate = client.get("/api/deployment/workspace").json()[
        "scenarioCandidates"
    ][0]
    assert candidate == {
        "scenarioId": "cell-a",
        "scenarioCatalogRevision": _DIGEST,
        "deploymentArtifactId": "cell_a",
        "deploymentRevision": deployment_revision,
        "retainedBundleId": _DIGEST,
        "programId": "pick",
        "programRevision": _DIGEST,
        "programBindingId": _DIGEST,
        "description": "Cell A",
    }


def test_run_rejects_a_loaded_deployment_draft_that_is_not_manager_applied():
    state = ComposerState()
    state.deployment_draft_session = DeploymentDraftSession(compiler=_compile)
    compiled = _compile(json.dumps(_document()))
    retained = state.deployment_draft_session.retained_state()
    state.deployment_draft_session.commit_open(
        compiled,
        "/workspace/cell_a.deployment.yaml",
        draft_id=retained["draft_id"],
        expected_revision=0,
        confirm_discard=False,
    )
    state.draft_session.snapshot = lambda: {"program_revision": _DIGEST}  # type: ignore[method-assign]
    state.current_scenario = lambda: {  # type: ignore[method-assign]
        "ready": True,
        "current_scenario": {"deployment_revision": "sha256:" + "2" * 64},
    }

    class Headless:
        def program_run_start(self, _body):
            raise AssertionError("Run must be rejected before transport")

    app = FastAPI()
    app.include_router(create_headless_router(Headless(), state))  # type: ignore[arg-type]
    response = TestClient(app).post(
        "/api/program/runs",
        json={
            "request_id": "run-1",
            "request_owner_epoch": "browser-1",
            "source_label": "test",
            "correlation_id": None,
            "scenario_id": "cell-a",
            "scenario_catalog_revision": _DIGEST,
            "expected_applied_record_digest": _DIGEST,
            "draft_program_revision": _DIGEST,
            "expected_admission_generation": 1,
        },
    )
    assert response.status_code == 409
    assert response.json()["detail"]["code"] == "program_run_deployment_draft_applied_mismatch"


def test_run_uses_the_existing_transport_when_deployment_draft_matches_applied():
    state = ComposerState()
    state.deployment_draft_session = DeploymentDraftSession(compiler=_compile)
    compiled = _compile(json.dumps(_document()))
    retained = state.deployment_draft_session.retained_state()
    state.deployment_draft_session.commit_open(
        compiled,
        "/workspace/cell_a.deployment.yaml",
        draft_id=retained["draft_id"],
        expected_revision=0,
        confirm_discard=False,
    )
    state.draft_session.snapshot = lambda: {"program_revision": _DIGEST}  # type: ignore[method-assign]
    state.current_scenario = lambda: {  # type: ignore[method-assign]
        "ready": True,
        "current_scenario": {"deployment_revision": compiled.deployment_revision},
    }

    class Headless:
        def program_run_start(self, body):
            return {"accepted_request_id": body["request_id"]}

    app = FastAPI()
    app.include_router(create_headless_router(Headless(), state))  # type: ignore[arg-type]
    response = TestClient(app).post(
        "/api/program/runs",
        json={
            "request_id": "run-1",
            "request_owner_epoch": "browser-1",
            "source_label": "test",
            "correlation_id": None,
            "scenario_id": "cell-a",
            "scenario_catalog_revision": _DIGEST,
            "expected_applied_record_digest": _DIGEST,
            "draft_program_revision": _DIGEST,
            "expected_admission_generation": 1,
        },
    )
    assert response.status_code == 200
    assert response.json() == {"accepted_request_id": "run-1"}
