from __future__ import annotations

from copy import deepcopy
import hashlib
import json
from pathlib import Path
from types import SimpleNamespace
from typing import Any

from fastapi import FastAPI, HTTPException
from fastapi.testclient import TestClient
import pytest

from manyforge_behavior.program import Program
from manyforge_composer.backend.deployment_draft import (
    CompiledDeploymentDocument,
    DeploymentDraftError,
    DeploymentDraftSession,
    _edit_controlled_stop_profile,
    validate_deployment_draft_document,
)
from manyforge_composer.backend.draft_session import ComposerDraftSession
from manyforge_composer.backend.routes_deployment_draft import (
    _raise,
    create_deployment_draft_router,
)
from manyforge_composer.backend.authoring_admission import (
    AuthoringAdmissionCoordinator,
    AuthoringOperationError,
    LocalAuthoringCommit,
)
from manyforge_composer.backend.authoring_workspace import AuthoringWorkspace
from manyforge_composer.backend.routes_headless import create_headless_router
from manyforge_composer.backend.state import (
    ComposerState,
)
from manyforge_composer.backend.app import create_app
from manyforge_headless_api.client import HeadlessClientError


_DIGEST = "sha256:" + "1" * 64
_APPLY_KEY = "e" * 32
# A Deployment intent field an ordinary Draft edit changes.
_CLEARANCE = ["collision_rules", 0, "clearance_m"]


def _command(client: TestClient, path: str, body: dict[str, Any], key: str = _APPLY_KEY):
    """Post one Apply-level command (OD-214) with its request key."""

    return client.post(path, json={**body, "request_key": key})


def _ended(state: ComposerState, key: str = _APPLY_KEY) -> dict[str, Any]:
    """The command's row once it ended."""

    row = state.apply_commands.ended_row(key)
    assert row is not None and row["end"] is not None
    return row


def test_scene_preparation_unavailable_keeps_typed_http_error() -> None:
    failure = AuthoringOperationError(
        "program_scene_preparation_unavailable",
        "Program is applied, but backend authoring readiness is not established",
    )
    with pytest.raises(HTTPException) as error:
        _raise(failure)
    assert error.value.status_code == 503
    assert error.value.detail == {"code": failure.code, "message": str(failure)}


@pytest.mark.parametrize("enabled", [False, True])
def test_jerk_draft_choice_preserves_units_inactive_values_and_substitution(enabled):
    source = _focused_document()
    for row in source["policies"]["stop_stage_parameters"][:2]:
        row["default_contract_parameters"][
            "recoverable_substitution_mode"
        ] = "spring_back_hold"
    profile = {
        "stop_mode": "soft_on_path",
        "deceleration_time_s": 1.0,
        "allow_natural_completion_on_short_path": True,
        "jerk_limits_enabled": enabled,
        "per_resource_limits": [
            {
                "resource_id": "robot.arm.coordinate.000000",
                "max_deceleration": {"coordinate_kind": "angular_rad_s2", "value": 4.0},
                "settle_velocity_threshold": {
                    "coordinate_kind": "angular_rad_s",
                    "value": 0.01,
                },
                "max_jerk": {"coordinate_kind": "angular_rad_s3", "value": 20.0},
            }
        ],
    }
    selection = {
        "controllable_component_id": "robot",
        "command_target_id": "arm",
    }
    result = _edit_controlled_stop_profile(source, profile, **selection)
    for row in result["policies"]["stop_stage_parameters"][:2]:
        parameters = row["default_contract_parameters"]
        assert parameters["jerk_limits_enabled"] is enabled
        # Canonical write: the legacy substitution becomes the fallback list.
        assert "recoverable_substitution_mode" not in parameters
        assert parameters["fallback_stop_modes"] == ["spring_back_hold"]
        assert parameters["per_resource_limits"][0]["max_jerk"] == {
            "coordinate_kind": "angular_rad_s3",
            "value_rad_s3": 20.0,
        }
    profile["jerk_limits_enabled"] = None
    with pytest.raises(DeploymentDraftError, match="explicitly"):
        _edit_controlled_stop_profile(source, profile, **selection)
    profile["jerk_limits_enabled"] = enabled
    profile["per_resource_limits"][0]["max_jerk"]["coordinate_kind"] = "linear_m_s3"
    with pytest.raises(DeploymentDraftError, match="units"):
        _edit_controlled_stop_profile(source, profile, **selection)


def _compiled_stop_profile(stop_mode: str, **extra: Any) -> dict[str, Any]:
    return {
        "stop_mode": stop_mode,
        "deceleration_time_s": 1.0,
        "allow_natural_completion_on_short_path": False,
        "jerk_limits_enabled": False,
        "per_resource_limits": [
            {
                "resource_id": "robot.arm.coordinate.000000",
                "max_deceleration": {"coordinate_kind": "angular_rad_s2", "value": 4.0},
                "settle_velocity_threshold": {
                    "coordinate_kind": "angular_rad_s",
                    "value": 0.01,
                },
            }
        ],
        **extra,
    }


def _compiled_stop_parameters(document: dict[str, Any]) -> list[dict[str, Any]]:
    return [
        row["default_contract_parameters"]
        for row in document["policies"]["stop_stage_parameters"]
        if row["stop_escalation_stage_id"] == "motion_controlled_stop"
        and row["provider_contract_ref"]["provider_contract_role_id"]
        == "stop_contract"
    ]


def test_compiled_stop_fallbacks_write_canonical_field_and_prune_on_mode_change():
    selection = {"controllable_component_id": "robot", "command_target_id": "arm"}
    legacy = _focused_document()
    for parameters in _compiled_stop_parameters(legacy):
        parameters["recoverable_substitution_mode"] = "spring_back_hold"
    # Without the field: the legacy Soft substitution reads as [spring_back_hold]
    # and is normalized on write; it is dropped once the primary is not Soft.
    soft = _edit_controlled_stop_profile(
        legacy, _compiled_stop_profile("soft_on_path"), **selection
    )
    for parameters in _compiled_stop_parameters(soft):
        assert parameters["fallback_stop_modes"] == ["spring_back_hold"]
        assert "recoverable_substitution_mode" not in parameters
    hold = _edit_controlled_stop_profile(
        legacy, _compiled_stop_profile("spring_back_hold"), **selection
    )
    for parameters in _compiled_stop_parameters(hold):
        assert "fallback_stop_modes" not in parameters
        assert "recoverable_substitution_mode" not in parameters
    # Explicit lists are validated and written; [] writes nothing.
    full = _edit_controlled_stop_profile(
        legacy,
        _compiled_stop_profile(
            "soft_on_path",
            fallback_stop_modes=["spring_back_hold", "spring_back_fast"],
        ),
        **selection,
    )
    for parameters in _compiled_stop_parameters(full):
        assert parameters["fallback_stop_modes"] == [
            "spring_back_hold",
            "spring_back_fast",
        ]
    # Recorded new list, profile without the field, primary moves down: prune.
    pruned = _edit_controlled_stop_profile(
        full, _compiled_stop_profile("spring_back_hold"), **selection
    )
    for parameters in _compiled_stop_parameters(pruned):
        assert parameters["fallback_stop_modes"] == ["spring_back_fast"]
    cleared = _edit_controlled_stop_profile(
        full, _compiled_stop_profile("soft_on_path", fallback_stop_modes=[]), **selection
    )
    for parameters in _compiled_stop_parameters(cleared):
        assert "fallback_stop_modes" not in parameters
    for invalid in (
        ["soft_on_path"],
        ["spring_back_fast", "spring_back_hold"],
        ["spring_back_hold", "spring_back_hold"],
        ["hard_deceleration"],
        ["unknown"],
    ):
        with pytest.raises(DeploymentDraftError, match="fallback_stop_modes"):
            _edit_controlled_stop_profile(
                legacy,
                _compiled_stop_profile("soft_on_path", fallback_stop_modes=invalid)
                if invalid != ["soft_on_path"]
                else _compiled_stop_profile(
                    "spring_back_hold", fallback_stop_modes=invalid
                ),
                **selection,
            )


def test_compiled_stop_with_old_and_new_fallback_fields_is_ambiguous():
    selection = {"controllable_component_id": "robot", "command_target_id": "arm"}
    source = _focused_document()
    for parameters in _compiled_stop_parameters(source):
        parameters["recoverable_substitution_mode"] = "spring_back_hold"
        parameters["fallback_stop_modes"] = ["spring_back_hold"]
    with pytest.raises(DeploymentDraftError, match="both fallback_stop_modes"):
        _edit_controlled_stop_profile(
            source, _compiled_stop_profile("soft_on_path"), **selection
        )


def test_max_stop_time_edit_converges_all_bindings_for_one_authored_target():
    source = _focused_document()
    other_binding = deepcopy(source["graph"]["control_bindings"][0])
    other_binding["control_binding_id"] = "other_binding"
    source["graph"]["control_bindings"].append(other_binding)
    other_stage = deepcopy(source["policies"]["stop_stage_parameters"][0])
    other_stage["control_binding_id"] = "other_binding"
    source["policies"]["stop_stage_parameters"].append(other_stage)
    saved_source = deepcopy(source)
    selection = {"controllable_component_id": "robot", "command_target_id": "arm"}
    profile = {
        "stop_mode": "soft_on_path",
        "deceleration_time_s": 1.0,
        "max_stop_time_s": 3.0,
        "allow_natural_completion_on_short_path": True,
        "jerk_limits_enabled": False,
        "per_resource_limits": [
            {
                "resource_id": "robot.arm.coordinate.000000",
                "max_deceleration": {"coordinate_kind": "angular_rad_s2", "value": 4.0},
                "settle_velocity_threshold": {
                    "coordinate_kind": "angular_rad_s",
                    "value": 0.01,
                },
            }
        ],
    }
    result = _edit_controlled_stop_profile(source, profile, **selection)
    assert source == saved_source
    assert "stop_timing_parameters" not in result["policies"]
    for row in result["policies"]["stop_stage_parameters"]:
        if row.get("control_binding_id") == "arm_binding" and row.get(
            "stop_escalation_stage_id"
        ) == source["policies"]["stop_stage_parameters"][0]["stop_escalation_stage_id"]:
            assert row["default_contract_parameters"]["max_stop_time_s"] == 3.0
    cleared = dict(profile)
    del cleared["max_stop_time_s"]
    restored = _edit_controlled_stop_profile(result, cleared, **selection)
    for row in restored["policies"]["stop_stage_parameters"]:
        assert "max_stop_time_s" not in row["default_contract_parameters"]
    with pytest.raises(DeploymentDraftError, match="no longer matches"):
        _edit_controlled_stop_profile(
            source,
            profile,
            controllable_component_id="other",
            command_target_id="arm",
        )
    for invalid in (0.0, -1.0, float("inf"), True, "3"):
        with pytest.raises(DeploymentDraftError):
            _edit_controlled_stop_profile(
                source, {**profile, "max_stop_time_s": invalid}, **selection
            )
    with pytest.raises(DeploymentDraftError):
        _edit_controlled_stop_profile(source, None, **selection)


def _compile(source: str) -> CompiledDeploymentDocument:
    document = json.loads(source)
    if (
        not isinstance(document, dict)
        or document.get("api_version") != "manyforge.deployment.v1"
    ):
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
        deployment_revision=revision,
        graph_revision=graph_revision,
    )


def _document() -> dict[str, Any]:
    return {
        "api_version": "manyforge.deployment.v1",
        "deployment_revision": _DIGEST,
        "cell_id": "cell_a",
        "capability_provider_bindings": [],
        "graph": {
            "component_attachments": [],
            "controllable_components": [],
            "control_bindings": [],
            "control_domains": [],
            "model_instances": [],
            "runtime_units": [],
        },
    }


def test_prepared_deployment_replacement_is_deeply_immutable() -> None:
    session = DeploymentDraftSession(compiler=_compile)
    compiled = session.preview_source(json.dumps(_document()))
    prepared = session.prepare_open_replacement(compiled, "/tmp/deployment.yaml")

    with pytest.raises(TypeError):
        prepared.compiled.document["graph"]["runtime_units"] = ["tampered"]

    session.install_prepared_replacement(prepared)
    assert session.snapshot()["draft"]["document"]["graph"][
        "runtime_units"
    ] == []


def _focused_document() -> dict[str, Any]:
    document = _document()
    document["graph"]["control_bindings"] = [
        {
            "control_binding_id": "arm_binding",
            "control_binding_revision": _DIGEST,
            "controllable_component_id": "robot",
            "command_target_id": "arm",
            "legal_control_route_ids": ["route-a", "route-b"],
        }
    ]
    document["graph"]["runtime_units"] = [
        {
            "runtime_unit_id": "robot.ros2_control_execution",
            "runtime_profile_id": "manyforge.ros2_control_execution_runtime",
            "runtime_profile_revision": _DIGEST,
            "dependency_runtime_unit_ids": [],
            "configuration": {
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
        "jerk_limits_enabled": True,
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
                "control_binding_id": "arm_binding",
                "control_binding_revision": _DIGEST,
                "control_route_id": "route-a",
                "stop_escalation_stage_id": "motion_controlled_stop",
                "provider_contract_ref": {"provider_contract_role_id": "stop_contract"},
                "default_contract_parameters": deepcopy(stop_parameters),
            },
            {
                "control_binding_id": "arm_binding",
                "control_binding_revision": _DIGEST,
                "control_route_id": "route-b",
                "stop_escalation_stage_id": "motion_controlled_stop",
                "provider_contract_ref": {"provider_contract_role_id": "stop_contract"},
                "default_contract_parameters": deepcopy(stop_parameters),
            },
            {
                "control_binding_id": "arm_binding",
                "control_binding_revision": _DIGEST,
                "control_route_id": "route-a",
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
    def settle_before_preparation(self) -> None:
        """No local claim is pending in these route-unit tests."""

    def manager_submission_sent(self, _claim) -> None:
        """The parent claim's hand-over is the real coordinator's concern."""

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
        claim = {"claim": "test"}
        validate_under_claim(claim)
        if commit_before_submit is not None:
            commit_before_submit()
        result = submit(claim)
        if after_release is not None:
            after_release()
        return result

    def execute_local_mutation(
        self,
        _envelope,
        *,
        session="program",
        draft_id=None,
        workspace,
        install=None,
        validate_under_claim=None,
        expected_identity=None,
        project=None,
        prepare_project_under_claim=None,
        **_kwargs,
    ):
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

    def execute_draft_edit(
        self, _envelope, *, workspace, session, draft_id, expected_identity,
        install, validate=None, **_kwargs,
    ):
        if validate is not None:
            validate()
        if session == "scenario":
            return LocalAuthoringCommit(
                snapshot=workspace.replace(
                    expected_identity=expected_identity, install=install
                ),
                admission_generation=2,
            )
        commit = (
            workspace.commit_deployment if session == "deployment"
            else workspace.commit_program
        )
        return LocalAuthoringCommit(
            snapshot=commit(
                draft_id=draft_id, expected_identity=expected_identity, install=install
            ),
            admission_generation=2,
        )


class _Headless:
    pass


def _request(workspace: dict[str, Any], **extra: Any) -> dict[str, Any]:
    extra.pop("local", True)
    identity = workspace["authoringWorkspace"]
    request = {
        "draftId": workspace["draft"]["id"],
        "expectedDraftRevision": workspace["draft"]["revision"],
        "expectedAdmissionGeneration": 1,
        "applicationStartId": "3f9a0000000000a1",
        # One key per (revision, kind), as distinct browser requests.
        "requestKey": f"{workspace['draft']['revision']:016x}"
        + extra.pop("kind", "edit").encode("utf-8").hex()[:16].ljust(16, "0"),
        "expectedWorkspaceEpoch": identity["workspaceEpoch"],
        "expectedWorkspaceRevision": identity["workspaceRevision"],
        **extra,
    }
    return request


def _replace_deployment_session(
    state: ComposerState, session: DeploymentDraftSession
) -> None:
    state.deployment_draft_session = session
    state.authoring_workspace = AuthoringWorkspace(
        program_session=state.draft_session,
        deployment_session=session,
    )


def _seed_existing_deployment(
    state: ComposerState,
    document: dict[str, Any],
    *,
    saved_path: str | None = None,
    compiler=_compile,
) -> None:
    """Seed a pre-existing live composition for Draft-only parameter tests."""

    session = DeploymentDraftSession(compiler=compiler)
    compiled = session.preview_source(json.dumps(document))
    retained = session.retained_state()
    retained.update(
        {
            "draft_revision": 1,
            "document": compiled.document,
            "saved_path": saved_path,
            "saved_document": compiled.document,
        }
    )
    session.restore_retained_state(retained)
    _replace_deployment_session(state, session)


def _client(tmp_path: Path) -> tuple[TestClient, ComposerState]:
    state = ComposerState(application_start_id=0x3F9A0000000000A1)
    _replace_deployment_session(
        state,
        DeploymentDraftSession(compiler=_compile),
    )
    state.authoring_admission = _Coordinator()  # type: ignore[assignment]
    state.headless_client = _Headless()  # type: ignore[assignment]
    state.allowed_fs_roots = [str(tmp_path)]
    app = FastAPI()
    app.include_router(create_deployment_draft_router(state))
    return TestClient(app), state


def test_workspace_read_while_apply_publishes_shows_not_ready(
    tmp_path: Path,
) -> None:
    """OD-265: one publication read; an Apply ending mid-read is no 500."""

    client, state = _client(tmp_path)
    digest = "sha256:" + "a" * 64
    published = {
        "ready": True,
        "unavailable_reasons": [],
        "published_scenario": {
            "deployment_revision": digest,
            "retained_bundle_id": digest,
            "graph_revision": digest,
            "program_revision": digest,
            "scenario_id": "s",
            "source_document_digest": digest,
            "program_outcome": {"kind": "bound", "program_binding_id": digest},
        },
    }
    reads = iter(
        [
            {
                "ready": False,
                "published_scenario": None,
                "unavailable_reasons": ["deployment_not_published"],
            },
            published,
        ]
    )
    # The Apply's publication becomes ready after the first read, and the
    # Manager has converged on it.
    state.published_scenario = lambda: next(reads)  # type: ignore[method-assign]
    fresh = {"available": True, "freshness": "fresh", "known_gaps": []}
    state.headless_client.manager_status = lambda: {  # type: ignore[union-attr]
        "source": fresh,
        "status": {
            "manager_ready": True,
            "runtime": {"converged": True},
            "active": {"retained_bundle_id": digest, "graph_revision": digest},
        },
    }
    state.headless_client.manager_diff = lambda: {  # type: ignore[union-attr]
        "source": fresh,
        "converged": True,
    }

    response = client.get("/api/deployment/workspace")

    assert response.status_code == 200, response.text
    applied = response.json()["managerApplied"]
    assert applied["deploymentRevision"] is None
    assert applied["scenarioId"] is None
    assert applied["activationOrder"] is None


def test_deployment_open_edit_discard_share_one_revision_fenced_workspace(
    tmp_path: Path,
):
    client, state = _client(tmp_path)
    # Apply captures only authored intent (model_source_capture requires its
    # component inventory), so the Applied baseline is a canonical intent.
    _replace_deployment_session(state, DeploymentDraftSession())
    authored = {
        "api_version": "manyforge.deployment_intent.v3",
        "collision_rules": [{"rule_id": "everything", "clearance_m": 0.001}],
        "components": [],
        "attachments": [],
        "policies": {},
    }
    source = tmp_path / "cell_a.deployment.yaml"
    source.write_text(json.dumps(authored), encoding="utf-8")

    workspace = client.get("/api/deployment/workspace").json()
    response = client.post(
        "/api/deployment/open-draft",
        json=_request(workspace, path=str(source), confirmDiscard=False, kind="open"),
    )
    assert response.status_code == 200, response.text
    workspace = response.json()
    assert workspace["draft"]["differsFromSaved"] is False
    applied = deepcopy(workspace["draft"]["document"])
    # Manager assigns the prepared Deployment revision; intent has none.
    applied_revision = _DIGEST

    response = client.post(
        "/api/deployment/draft/mutations",
        json=_request(workspace, path=_CLEARANCE, value=0.002),
    )
    assert response.status_code == 200, response.text
    workspace = response.json()
    assert workspace["draft"]["document"]["collision_rules"][0]["clearance_m"] == 0.002
    assert workspace["draft"]["differsFromSaved"] is True
    assert applied is not None
    publication = {
        "origin": {"kind": "scenario", "scenario_id": "cell_a", "source_document_digest": _DIGEST,
                   "project_path": "/projects/cell_a.scenario"},
        "deployment_revision": applied_revision,
        "retained_bundle_id": _DIGEST,
        "graph_revision": _DIGEST,
        "program_revision": _DIGEST,
        "program_outcome": {
            "kind": "bound",
            "program_binding_id": _DIGEST,
        },
    }
    captured = state._captured_deployment_context(
        {
            "deployment": applied,
            "model_source_closure": {},
            "scene_mesh_closure": {},
            "program_role_assignment": {},
            "source_document_digest": _DIGEST,
            "scenario_id": "cell_a",
            "project_path": "/projects/cell_a.scenario",
        },
        publication,
    )
    state._retain_submitted_deployment(captured)
    # CurrentScenario is an authored Scenario projection, not a prepared receipt.
    origin = publication.pop("origin")
    publication.update(scenario_id=origin["scenario_id"],
                       source_document_digest=origin["source_document_digest"])
    state.published_scenario = lambda: {  # type: ignore[method-assign]
        "ready": True,
        "published_scenario": publication,
        "unavailable_reasons": [],
    }
    response = client.post(
        "/api/deployment/discard",
        json=_request(workspace, kind="discard"),
    )
    assert response.status_code == 200, response.text
    assert response.json()["draft"]["document"]["collision_rules"] == applied["collision_rules"]
    assert response.json()["managerApplied"]["activationOrder"]["componentIds"] == [
        row["component_id"] for row in applied["components"]
    ]
    assert (
        state.authoring_retained_state()["schema_version"]
        == "manyforge.composer_authoring_workspace.v2"
    )


def test_pose_only_controllable_component_edit_remains_draftable(
    tmp_path: Path,
) -> None:
    client, state = _client(tmp_path)
    source = _document()
    source["graph"]["controllable_components"] = [
        {
            "controllable_component_id": "robot",
            "root_pose_in_cell": {
                "position_m": [0.0, 0.0, 0.0],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            },
        }
    ]
    source["graph"]["model_instances"] = [
        {
            "model_instance_id": "robot.model",
            "model_asset_id": "robot_model",
            "model_revision": _DIGEST,
        }
    ]
    _seed_existing_deployment(state, source)
    before = client.get("/api/deployment/workspace").json()
    pose = {
        "position_m": [1.0, 0.0, 0.0],
        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
    }

    changed = client.post(
        "/api/deployment/draft/mutations",
        json=_request(
            before,
            path=["graph", "controllable_components", 0, "root_pose_in_cell"],
            value=pose,
            kind="placement-preview",
        ),
    )

    assert changed.status_code == 200, changed.text
    # The browser parses this reply as a Deployment workspace whose
    # authoringWorkspace is exactly its Draft-matched identity.
    identity = changed.json()["authoringWorkspace"]
    assert set(identity) == {
        "workspaceEpoch", "workspaceRevision", "programDraftId",
        "programDraftRevision", "deploymentDraftId", "deploymentDraftRevision",
    }
    assert identity["deploymentDraftId"] == changed.json()["draft"]["id"]
    assert identity["deploymentDraftRevision"] == changed.json()["draft"]["revision"]
    assert changed.json()["draft"]["document"]["graph"][
        "controllable_components"
    ][0]["root_pose_in_cell"] == pose


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
        json=_request(opened, path=["cell_id"], value="current", kind="accepted"),
    )
    assert accepted.status_code == 200
    current = accepted.json()
    redundant = client.post(
        "/api/deployment/draft/mutations",
        json=_request(current, path=["cell_id"], value="current", kind="redundant"),
    )
    assert redundant.status_code == 200
    assert redundant.json()["draft"]["revision"] == current["draft"]["revision"]
    stale = client.post(
        "/api/deployment/draft/mutations",
        json=_request(opened, path=["cell_id"], value="stale", kind="stale"),
    )
    assert stale.status_code == 409
    assert stale.json()["detail"]["code"] == "deployment_draft_stale"
    assert (
        client.get("/api/deployment/workspace").json()["draft"]["document"]["cell_id"]
        == "current"
    )


def test_deployment_preview_and_apply_are_orchestrated_server_side(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    from manyforge_composer.backend import routes_deployment_draft

    client, state = _client(tmp_path)
    project = (
        Path(__file__).resolve().parents[2]
        / "examples/mock_lite6_pick_and_place.scenario"
    )
    source = project / "deployment.yaml"
    state.allowed_fs_roots.append(str(project.parents[1]))
    _replace_deployment_session(state, DeploymentDraftSession())
    compiled = state.deployment_draft_session.preview_source(
        source.read_text(encoding="utf-8")
    )
    retained = state.deployment_draft_session.retained_state()
    retained.update(
        {
            "draft_revision": 1,
            "document": compiled.document,
            "saved_path": str(source),
            "saved_document": compiled.document,
        }
    )
    state.deployment_draft_session.restore_retained_state(retained)
    opened = client.get("/api/deployment/workspace").json()
    base_revision = "sha256:" + "1" * 64
    edited = client.post(
        "/api/deployment/draft/mutations",
        json=_request(opened, path=_CLEARANCE, value=0.002, kind="edit"),
    ).json()
    candidate_revision = "sha256:" + "8" * 64
    assert candidate_revision != base_revision
    program_model = Program(
        str(
            Path(__file__).resolve().parents[2]
            / "examples/lite6_lite_gripper_pick_and_place.program.yaml"
        ),
        state.node_environment.catalog,
    )
    program = program_model.to_dict()
    state.draft_session = ComposerDraftSession(program_model)
    state.authoring_workspace = AuthoringWorkspace(
        program_session=state.draft_session,
        deployment_session=state.deployment_draft_session,
    )
    edited = client.get("/api/deployment/workspace").json()
    state.authoring_workspace.remember_project(
        document={
            "scenario_id": "mock_lite6_pick_and_place",
        },
        document_path=str(project),
        source_document_digest="sha256:" + "6" * 64,
    )
    edited = client.get("/api/deployment/workspace").json()
    capture_calls = 0
    capture_sources = routes_deployment_draft._capture_model_sources

    def counted_capture(*args, **kwargs):
        nonlocal capture_calls
        capture_calls += 1
        return capture_sources(*args, **kwargs)

    monkeypatch.setattr(
        routes_deployment_draft, "_capture_model_sources", counted_capture
    )
    current = {
        "deployment_revision": base_revision,
        "retained_bundle_id": "sha256:" + "2" * 64,
        "graph_revision": "sha256:" + "3" * 64,
    }

    class CandidateHeadless(_Headless):
        def manager_status(self):
            return {
                "source": {
                    "available": True,
                    "freshness": "fresh",
                    "known_gaps": [],
                },
                "status": {
                    "manager_ready": True,
                    "active": {
                        "retained_bundle_id": current["retained_bundle_id"],
                        "graph_revision": current["graph_revision"],
                    },
                },
            }

        def execution_control_state(self):
            # No manual operation: a held run is Reset by the Apply.
            return {"control_state": {"manual_operations": []}}

    lifecycles = {"lite6_robot": "enabled", "lite_gripper": "enabled", "camera": "enabled"}
    actions: list[tuple[str, str]] = []
    results: dict[str, dict[str, Any]] = {}
    cycle = {"state": "idle_after_reset"}

    def _components(_self):
        rows = []
        for component_id, lifecycle in lifecycles.items():
            row: dict[str, Any] = {"component_id": component_id, "lifecycle": lifecycle}
            if component_id in results:
                row["last_action"] = results[component_id]
            rows.append(row)
        return rows

    def _component_action(_self, component_id, action):
        actions.append((action, component_id))
        lifecycles[component_id] = "prepared"
        results[component_id] = {
            "action": action, "outcome": "succeeded",
            "started_at_ns": len(actions), "finished_at_ns": len(actions),
        }
        return {"action": action}

    CandidateHeadless.components = _components  # type: ignore[attr-defined]
    CandidateHeadless.component_action = _component_action  # type: ignore[attr-defined]
    CandidateHeadless.program_cycle_status = lambda _self: dict(cycle)  # type: ignore[attr-defined]
    CandidateHeadless.authoring_admission = lambda _self: {  # type: ignore[attr-defined]
        "authoring_allowed": True, "admission_generation": 3,
    }
    CandidateHeadless.manager_apply_status = (  # type: ignore[attr-defined]
        lambda _self, apply_id, generation, *, application_start_id: {"ok": True, "state": "succeeded"}
    )
    state.headless_client = CandidateHeadless()  # type: ignore[assignment]
    impact = {
        "components": [
            {"controllable_component_id": "lite6_robot", "disposition": "replace",
             "replacement_reasons": ["description"]},
            {"controllable_component_id": "lite_gripper", "disposition": "remove",
             "replacement_reasons": []},
            {"controllable_component_id": "camera", "disposition": "retain",
             "replacement_reasons": []},
        ]
    }
    submitted: list[dict[str, Any]] = []

    def execute_candidate(intent, claim, *, apply, prepared=None,
                          prepare_only=False, confirmed=False):
        result = {
            "status": "accepted" if apply else "previewed",
            "confirmation_token": None,
            "prepared_activation": {},
            "candidate_preview": {"impact": impact},
            "manager_apply": None,
            "preparation": None,
            "phases_ms": {},
            "apply_id": 1 if apply else None,
            "apply_generation": 1 if apply else None,
        }
        if prepare_only:
            assert claim is None
            assert confirmed is apply, "the server confirms its own Apply"
            return {"result": result}
        if apply:
            submitted.append(deepcopy(intent))
            # The Manager removes the gripper and restarts the robot.
            lifecycles.pop("lite_gripper")
            lifecycles["lite6_robot"] = "not_prepared"
        return result

    state.execute_authored_deployment_candidate = execute_candidate  # type: ignore[method-assign]

    preview = client.post(
        "/api/deployment/preview", json={}
    )
    assert preview.status_code == 200, preview.text
    previewed = preview.json()
    assert previewed["components"] == [
        {"component_id": "lite6_robot", "disposition": "replace",
         "reasons": ["description"], "restarts": True},
        {"component_id": "lite_gripper", "disposition": "remove",
         "reasons": [], "restarts": False},
        {"component_id": "camera", "disposition": "retain",
         "reasons": [], "restarts": False},
    ]
    assert previewed["attachment_changed_components"] == []
    assert previewed["detached_components"] == []
    assert previewed["program"]["executable"] is True
    scope = previewed["scope"]
    assert scope["deployment_draft_id"] == edited["draft"]["id"]
    assert scope["source_document_digest"] == "sha256:" + "6" * 64
    assert actions == []

    cycle["state"] = "running"
    refused = _command(
        client, "/api/deployment/apply", {"scope": scope}, "1" * 32
    )
    assert refused.status_code == 409, refused.text
    assert refused.json()["detail"]["code"] == "apply_refused_run_in_progress"
    # A refusal at once keeps its reply and is a refused row.
    refused_row = state.apply_commands.rows.find("1" * 32)
    assert refused_row["accepted_at_ns"] is None
    assert refused_row["end"]["code"] == "apply_refused_run_in_progress"
    assert actions == [] and submitted == []

    cycle["state"] = "idle_after_reset"
    # Bound to the Preview the user saw: a changed input is refused first.
    stale = _command(
        client, "/api/deployment/apply",
        {"scope": {**scope, "deployment_draft_revision": -1}}, "2" * 32,
    )
    assert stale.status_code == 409, stale.text
    assert stale.json()["detail"]["code"] == "activation_input_changed_after_confirmation"
    assert actions == [] and submitted == []

    applied = _command(client, "/api/deployment/apply", {"scope": scope})
    assert applied.status_code == 200, applied.text
    assert applied.json()["schema_version"] == "manyforge.apply_command_reply.v1"
    assert applied.json()["command"]["kind"] == "deployment_apply"
    ended = _ended(state)
    assert actions == [
        ("disable", "lite_gripper"),
        ("disable", "lite6_robot"),
        ("prepare", "lite6_robot"),
    ]
    assert submitted[-1]["expected_admission_generation"] == 3
    assert submitted[-1]["scene_initialization"] == "preserve"
    assert ended["end"]["outcome"] == "succeeded"
    # A succeeded Apply-level row keeps its component rows (OD-214 contract
    # §3.1): the panel's success text is built from them.
    assert ended["end"]["details"] == {"components": [
        {"component_id": "lite6_robot", "disposition": "replace",
         "lifecycle_after": "prepared"},
        {"component_id": "lite_gripper", "disposition": "remove"},
        {"component_id": "camera", "disposition": "retain",
         "lifecycle_after": "enabled"},
    ]}
    # The workspace document publishes the row (v2).
    workspace = client.get("/api/deployment/workspace").json()
    assert workspace["schemaVersion"] == "manyforge.composer_deployment_workspace.v2"
    assert workspace["apply_commands"]["ended"][0] == ended

    # Nothing changed since Applied: refused before the finished run's Reset
    # or any disable (409 deployment_apply_nothing_to_apply).
    from manyforge_composer.backend.deployment_defaults import with_defaults

    document = with_defaults(state.deployment_draft_session.retained_state()["document"])
    applied_context = {
        "deployment": document,
        "model_source_closure": capture_sources(state, document),
        "scene_mesh_closure": state.capture_scene_mesh_closure(
            state.draft_session.retained_state()["program"],
            project_path=state.scenario_project_path,
        ),
        "source_document_digest": state.scenario_source_document_digest,
        "scenario_id": state.scenario_document_id,
        "scenario_project_path": state.scenario_project_path,
    }
    # A published Scenario names its retained bundle and graph.
    monkeypatch.setattr(state, "published_scenario", lambda: {"published_scenario": {
        "retained_bundle_id": current["retained_bundle_id"],
        "graph_revision": current["graph_revision"],
    }})
    monkeypatch.setattr(
        state, "applied_activation_for_publication", lambda _publication: applied_context
    )
    monkeypatch.setattr(
        routes_deployment_draft, "_applied_program_revision",
        lambda _state: state.draft_session.retained_state()["program"]["program_revision"],
    )
    resets: list[Any] = []
    CandidateHeadless.program_cycle_reset = (  # type: ignore[attr-defined]
        lambda _self, request: resets.append(request)
    )
    cycle["state"] = "terminal_awaiting_reset"
    before = (list(actions), len(submitted))
    unchanged = _command(
        client, "/api/deployment/apply", {"scope": client.post(
            "/api/deployment/preview", json={}).json()["scope"]}, "3" * 32,
    )
    assert unchanged.status_code == 409, unchanged.text
    assert unchanged.json()["detail"]["code"] == "deployment_apply_nothing_to_apply"
    assert resets == [] and (actions, len(submitted)) == before


def test_effective_values_read_the_current_deployment_draft(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    client, state = _client(tmp_path)
    assert client.get("/api/deployment/effective").json() == {"fields": {}}
    retained = state.deployment_draft_session.retained_state()
    retained["document"] = {
        "api_version": "manyforge.deployment_intent.v3",
        "components": [{"component_id": "robot", "configuration": {
            "runtime_kind": "ros2_control",
            "runtime_configuration": {"planner_rng_seed": 7}}}],
        "attachments": [],
    }
    # This fixture's compiler reads only v1 documents; the route reads the
    # retained Draft document as is.
    monkeypatch.setattr(
        state.deployment_draft_session, "retained_state", lambda: retained
    )
    fields = client.get("/api/deployment/effective").json()["fields"]
    path = "components.0.configuration.runtime_configuration."
    assert fields[path + "planner_rng_seed"] == {"value": 7, "default": 12345, "set": True}
    assert fields[path + "execution_tick_hz"] == {"value": 100, "default": 100, "set": False}


def test_draft_edits_accept_structural_changes(tmp_path: Path) -> None:
    """The addition-only rule is gone: a removal is an ordinary Draft edit."""

    client, _state = _client(tmp_path)
    source = _document()
    source["graph"]["control_bindings"] = [{"control_binding_id": "robot.arm"}]
    artifact = tmp_path / "with-binding.deployment.yaml"
    artifact.write_text(json.dumps(source), encoding="utf-8")
    initial = client.get("/api/deployment/workspace").json()
    opened = client.post(
        "/api/deployment/open-draft",
        json=_request(initial, path=str(artifact), confirmDiscard=False, kind="open"),
    )
    assert opened.status_code == 200, opened.text
    removed = client.post(
        "/api/deployment/draft/mutations",
        json=_request(
            opened.json(), path=["graph", "control_bindings"], value=[], kind="remove"
        ),
    )
    assert removed.status_code == 200, removed.text
    assert removed.json()["draft"]["document"]["graph"]["control_bindings"] == []


def _scenario_revert_setup(tmp_path: Path, monkeypatch: pytest.MonkeyPatch):
    """Unapplied edits in both Drafts of a Scenario whose Saved Deployment
    differs from Applied; the Manager is recorded."""

    from manyforge_composer.backend import routes_deployment_draft
    from manyforge_composer.backend.authoring_forest import derive_candidate

    client, state = _client(tmp_path)
    monkeypatch.setattr(
        routes_deployment_draft, "_active_catalog", lambda s: s.node_environment.catalog
    )
    project = Path(__file__).resolve().parents[2] / "examples/mock_lite6_pick_and_place.scenario"
    source = project / "deployment.yaml"
    state.allowed_fs_roots.append(str(project.parents[1]))
    _replace_deployment_session(state, DeploymentDraftSession())
    compiled = state.deployment_draft_session.preview_source(source.read_text(encoding="utf-8"))
    retained = state.deployment_draft_session.retained_state()
    retained.update({
        "draft_revision": 1,
        "document": compiled.document,
        "saved_path": str(source),
        "saved_document": compiled.document,
    })
    state.deployment_draft_session.restore_retained_state(retained)
    program_model = Program(
        str(Path(__file__).resolve().parents[2]
            / "examples/lite6_lite_gripper_pick_and_place.program.yaml"),
        state.node_environment.catalog,
    )
    saved_program = program_model.to_dict()
    state.draft_session = ComposerDraftSession(program_model)
    state.authoring_workspace = AuthoringWorkspace(
        program_session=state.draft_session,
        deployment_session=state.deployment_draft_session,
    )
    state.authoring_workspace.remember_project(
        document={
            "scenario_id": "mock_lite6_pick_and_place",
        },
        document_path=str(project),
        source_document_digest="sha256:" + "6" * 64,
    )
    # Unapplied edits in both Drafts.
    opened = client.get("/api/deployment/workspace").json()
    client.post(
        "/api/deployment/draft/mutations",
        json=_request(opened, path=_CLEARANCE, value=0.002, kind="edit"),
    )
    program_state = state.draft_session.retained_state()
    program_state["saved_document"] = deepcopy(saved_program)
    program_state["saved_path"] = str(project / "program.yaml")
    program_state["saved_program_revision"] = saved_program["program_revision"]
    program_state["authoring"]["compatibility"]["tags"] = ["edited-only-in-the-draft"]
    program_state["draft_revision"] += 1
    candidate, program_state["diagnostics"] = derive_candidate(
        program_state["authoring"], state.node_environment.catalog)
    program_state["program"] = (
        candidate.to_dict() if hasattr(candidate, "to_dict") else candidate
    )
    state.authoring_workspace.commit_program(
        draft_id=program_state["draft_id"],
        install=lambda: state.draft_session.install_prepared_replacement(
            state.draft_session.prepare_load_replacement(
                program_state, state.node_environment.catalog),
            expected_draft_id=program_state["draft_id"],
            expected_revision=program_state["draft_revision"] - 1,
        ),
    )
    assert state.draft_session.retained_state()["authoring"]["compatibility"]["tags"] == [
        "edited-only-in-the-draft"
    ]

    class CandidateHeadless(_Headless):
        def manager_status(self):
            return {
                "source": {"available": True, "freshness": "fresh", "known_gaps": []},
                "status": {"manager_ready": True, "active": {
                    "retained_bundle_id": "sha256:" + "2" * 64,
                    "graph_revision": "sha256:" + "3" * 64,
                }},
            }

        # The orchestrated Revert's reads: an idle cycle, an empty cell.
        def program_cycle_status(self):
            return {"state": "idle_after_reset"}

        def components(self):
            return []

        def authoring_admission(self):
            return {"authoring_allowed": True, "admission_generation": 1}

    state.headless_client = CandidateHeadless()  # type: ignore[assignment]
    effects: list[dict[str, Any]] = []

    def execute_candidate(intent, _claim, *, apply, prepared=None, prepare_only=False,
                          confirmed=False):
        assert apply
        if prepare_only:
            assert confirmed, "the server confirms its own Revert"
            return {"result": {"status": "applied"}, "confirmation_required": False}
        effects.append(deepcopy(intent))
        return {"status": "applied"}

    state.execute_authored_deployment_candidate = execute_candidate  # type: ignore[method-assign]
    return client, state, compiled, saved_program, program_model, effects


def test_scenario_revert_activates_saved_program_and_deployment_and_restores_both_drafts(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Revert acts on the whole Scenario: one activation of the Saved
    Deployment with the Saved Program (never the Program Draft), and both
    Drafts return to Saved in the same commit."""

    client, state, compiled, saved_program, program_model, effects = (
        _scenario_revert_setup(tmp_path, monkeypatch)
    )
    response = _command(client, "/api/deployment/revert", {})

    assert response.status_code == 200, response.text
    assert response.json()["command"]["kind"] == "deployment_revert"
    assert _ended(state)["end"]["outcome"] == "succeeded"
    assert len(effects) == 1
    assert effects[0]["deployment"] == compiled.document
    assert effects[0]["program"] == saved_program
    assert effects[0]["scene_initialization"] == "preserve"
    assert state.deployment_draft_session.retained_state()["document"] == compiled.document
    assert state.draft_session.retained_state()["authoring"]["compatibility"]["tags"] == (
        program_model.to_dict()["compatibility"]["tags"]
    )


def test_scenario_revert_goes_through_its_preview_and_is_bound_to_it(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    client, state, _compiled, _saved_program, _program_model, effects = (
        _scenario_revert_setup(tmp_path, monkeypatch)
    )
    preview = client.post("/api/deployment/revert/preview", json={})
    assert preview.status_code == 200, preview.text
    assert preview.json()["components"] == []
    assert effects == []  # the Preview changes nothing

    scope = preview.json()["scope"]
    stale = _command(
        client, "/api/deployment/revert",
        {"scope": {**scope, "program_draft_revision": -1}}, "4" * 32,
    )
    assert stale.status_code == 409, stale.text
    assert stale.json()["detail"]["code"] == "activation_input_changed_after_confirmation"
    assert effects == []

    reverted = _command(client, "/api/deployment/revert", {"scope": scope})
    assert reverted.status_code == 200, reverted.text
    _ended(state)
    assert len(effects) == 1


def test_scenario_revert_partial_contained_install_keeps_the_claim(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    from manyforge_composer.tests.test_authoring_local_mutation import _Headless

    client, state, _compiled, _saved_program, _program_model, effects = (
        _scenario_revert_setup(tmp_path, monkeypatch)
    )
    headless = _Headless(generation=1)
    coordinator = AuthoringAdmissionCoordinator(headless, 0x3F9A0000000000A1)  # type: ignore[arg-type]
    state.authoring_admission = coordinator

    def fail_program_restore(*_args, **_kwargs):
        raise RuntimeError("Program restore failed")

    # The Deployment restore installs first; the Program restore then fails.
    monkeypatch.setattr(
        state.draft_session, "install_prepared_replacement", fail_program_restore
    )
    response = _command(client, "/api/deployment/revert", {})

    # The orchestrated Revert ends `failed` with the failed step's code.
    assert response.status_code == 200, response.text
    end = _ended(state)["end"]
    assert end["outcome"] == "failed"
    assert end["code"] == "composer_workspace_install_failed"
    assert state.authoring_workspace.failed_closed is not None
    assert effects == []
    # Never settled as proven no effect: the claim is retained, admission blocked.
    assert headless.settlements == []
    assert coordinator.local_claim_pending is True
    assert coordinator.mutation_ready is False


def test_activation_submits_exact_preparation_to_manager_without_post_rebind() -> None:
    state = ComposerState(application_start_id=0x3F9A0000000000A1)
    receipt = {
        "expected_current": {
            "retained_bundle_id": "sha256:" + "2" * 64,
            "graph_revision": "sha256:" + "3" * 64,
        },
        "candidate_retained_bundle_id": "sha256:" + "4" * 64,
        "candidate_graph_revision": "sha256:" + "5" * 64,
    }
    prepared_activation = {
        "schema_version": "manyforge.prepared_activation_input.v8",
        "origin": {"kind": "scenario", "scenario_id": "scenario-a",
                   "source_document_digest": "sha256:" + "7" * 64,
                   "project_path": "/projects/scenario-a.scenario"},
        "prepared_activation_id": "sha256:" + "6" * 64,
        "captured_input_digest": "sha256:" + "8" * 64,
        "deployment_revision": "sha256:" + "9" * 64,
        "retained_bundle_id": receipt["candidate_retained_bundle_id"],
        "graph_revision": receipt["candidate_graph_revision"],
        "program_revision": "sha256:" + "a" * 64,
        "program_outcome": {"kind": "bound", "program_binding_id": "sha256:" + "b" * 64},
        "node_environment_digest": "sha256:" + "c" * 64,
        "assistant_mode": "forbidden",
        "scene_meshes": [],
    }
    manager_requests: list[dict[str, Any]] = []

    class Headless:
        def manager_apply(self, body):
            manager_requests.append(deepcopy(body))
            return {
                "ok": True,
                "state": "pending",
                "apply_id": 1,
                "apply_generation": 1,
                "candidate_projection_revision": {
                    "retained_bundle_id": receipt["candidate_retained_bundle_id"],
                    "graph_revision": receipt["candidate_graph_revision"],
                },
            }

    state.headless_client = Headless()  # type: ignore[assignment]
    parent_claim = {"intent": {"owner_role": "authoring_owner"}}
    result = state.execute_authored_deployment_candidate(
        {
            "deployment": {"api_version": "manyforge.deployment_intent.v3"},
            "model_source_closure": {},
            "scene_mesh_closure": {},
            "program_role_assignment": {},
            "source_document_digest": prepared_activation["origin"]["source_document_digest"],
            "scenario_id": prepared_activation["origin"]["scenario_id"],
            "project_path": prepared_activation["origin"]["project_path"],
        },
        parent_claim,
        apply=True,
        prepared={
            "prepared_activation": prepared_activation,
            "candidate_preview": {"candidate_receipt": receipt},
            "manager_apply": {
                "request_key": "1" * 32,
                "application_start_id": "3f9a0000000000a1",
                "source_label": "manyforge_composer_deployment_apply",
                "deployment_revision": "sha256:" + "9" * 64,
                "expected_current": receipt["expected_current"],
                "reject_if_affected_busy": True,
                "prepared_activation": deepcopy(prepared_activation),
                "expected_admission_generation": 7,
                "candidate_receipt": receipt,
            },
            "confirmation_required": False,
            "confirmation_token": None,
            "preparation": None,
            "phases_ms": {},
        },
    )

    assert result["status"] == "accepted"
    assert manager_requests[0]["prepared_activation"] == prepared_activation
    assert "scenario_id" not in manager_requests[0]["prepared_activation"]
    assert "source_document_digest" not in manager_requests[0]["prepared_activation"]
    assert manager_requests[0]["prepared_activation"]["program_outcome"] == (
        prepared_activation["program_outcome"]
    )
    assert "expected_admission_generation" not in manager_requests[0]
    assert manager_requests[0]["parent_authoring_claim"] == parent_claim


def test_manager_apply_rejection_keeps_manager_code_and_message() -> None:
    state = ComposerState(application_start_id=0x3F9A0000000000A1)

    class Headless:
        def manager_apply(self, _body):
            return {
                "ok": False,
                "error": {
                    "code": "stale_admission_generation",
                    "message": "admission generation 7 is not current (8)",
                },
            }

    state.headless_client = Headless()  # type: ignore[assignment]
    with pytest.raises(AuthoringOperationError) as caught:
        state.apply_manager_deployment_candidate({"manager_apply": {"request_key": "1" * 32}})
    # The Composer code that drives settlement stays the same; only the text
    # names Manager's precise cause.
    assert caught.value.code == "manager_apply_rejected"
    assert str(caught.value) == (
        "Manager rejected the exact Deployment candidate: "
        "stale_admission_generation: admission generation 7 is not current (8)"
    )

    class Bare:
        def manager_apply(self, _body):
            return {"ok": False}

    state.headless_client = Bare()  # type: ignore[assignment]
    with pytest.raises(AuthoringOperationError) as bare:
        state.apply_manager_deployment_candidate({"manager_apply": {"request_key": "2" * 32}})
    assert str(bare.value) == "Manager rejected the exact Deployment candidate"


def test_only_typed_preacceptance_rejection_becomes_definite_manager_refusal() -> None:
    state = ComposerState(application_start_id=0x3F9A0000000000A1)

    class Headless:
        effect: str | None = "none"

        def manager_apply(self, _body):
            message = "named Isaac world is unavailable"
            details = {"effect": self.effect} if self.effect is not None else {}
            raise HeadlessClientError(
                "apply_rejected",
                message,
                status_code=400,
                details=details,
                response={
                    "api_version": "manyforge.manager_command_response.v3",
                    "ok": False,
                    "error": {
                        "code": "apply_rejected",
                        "message": message,
                        "details": details,
                    },
                },
            )

    headless = Headless()
    state.headless_client = headless  # type: ignore[assignment]
    with pytest.raises(AuthoringOperationError) as rejected:
        state.apply_manager_deployment_candidate(
            {"manager_apply": {"request_key": "3" * 32}}
        )
    assert rejected.value.code == "manager_apply_rejected"
    assert str(rejected.value) == (
        "Manager rejected the exact Deployment candidate: "
        "apply_rejected: "
        "named Isaac world is unavailable"
    )

    # The generic Manager catch also covers failures during or after durable
    # acceptance. Its identical-looking text is not no-effect evidence.
    headless.effect = None
    with pytest.raises(HeadlessClientError) as unknown:
        state.apply_manager_deployment_candidate(
            {"manager_apply": {"request_key": "4" * 32}}
        )
    assert unknown.value.code == "apply_rejected"


def test_focused_motion_target_limit_updates_one_exact_target_atomically(
    tmp_path: Path,
) -> None:
    client, state = _client(tmp_path)
    source = tmp_path / "cell_a.deployment.yaml"
    focused = _focused_document()
    source.write_text(json.dumps(focused), encoding="utf-8")
    _seed_existing_deployment(state, focused, saved_path=str(source))
    opened = client.get("/api/deployment/workspace").json()

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
    assert (
        current["draft"]["document"]["graph"]["runtime_units"][0]["configuration"][
            "motion_target_limits"
        ][0]["maximum_tcp_linear_speed_mps"]
        == 0.1
    )
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


@pytest.mark.parametrize("natural_completion", [False, True])
def test_focused_controlled_stop_updates_all_derived_routes_and_not_hard_stop(
    tmp_path: Path,
    natural_completion: bool,
) -> None:
    client, state = _client(tmp_path)
    source = tmp_path / "cell_a.deployment.yaml"
    document = _focused_document()
    other_binding = deepcopy(document["graph"]["control_bindings"][0])
    other_binding["control_binding_id"] = "other_binding"
    document["graph"]["control_bindings"].append(other_binding)
    other_stage = deepcopy(document["policies"]["stop_stage_parameters"][0])
    other_stage["control_binding_id"] = "other_binding"
    document["policies"]["stop_stage_parameters"].append(other_stage)
    source.write_text(json.dumps(document), encoding="utf-8")
    _seed_existing_deployment(state, document, saved_path=str(source))
    opened = client.get("/api/deployment/workspace").json()
    profile = {
        "stop_mode": "spring_back_hold",
        "jerk_limits_enabled": False,
        "deceleration_time_s": 1.25,
        "allow_natural_completion_on_short_path": natural_completion,
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
        json=_request(
            opened,
            profile=profile,
            kind="controlled-stop",
            controllable_component_id="robot",
            command_target_id="arm",
        ),
    )
    assert updated.status_code == 200, updated.text
    rows = updated.json()["draft"]["document"]["policies"]["stop_stage_parameters"]
    controlled = [
        row
        for row in rows
        if row["stop_escalation_stage_id"] == "motion_controlled_stop"
    ]
    assert len(controlled) == 3
    assert all(
        row["default_contract_parameters"]
        == {
            "allow_natural_completion_on_short_path": natural_completion,
            "jerk_limits_enabled": False,
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
    assert rows[3]["control_binding_id"] == "other_binding"
    workspace = updated.json()
    # The edited policies round-trip through a written Deployment document.
    written = tmp_path / "written.deployment.yaml"
    written.write_text(json.dumps(workspace["draft"]["document"]), encoding="utf-8")
    reopened = client.post(
        "/api/deployment/open-draft",
        json=_request(
            workspace,
            path=str(written),
            confirmDiscard=True,
            kind="reopen-stop",
        ),
    )
    assert reopened.status_code == 200, reopened.text
    assert (
        reopened.json()["draft"]["document"]["policies"]
        == workspace["draft"]["document"]["policies"]
    )


def test_deployment_artifacts_remain_confined(
    tmp_path: Path,
) -> None:
    client, _state = _client(tmp_path)
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
        assert client.get("/api/deployment/workspace").json()["draft"] == initial["draft"]
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
        ["cell_id"],
        "retained_edit",
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


def test_save_as_path_rebase_matches_saved_scenario_but_real_edits_do_not() -> None:
    from manyforge_composer.backend.authoring_workspace import scenario_project_facts
    from manyforge_composer.backend.deployment_intent import (
        rebase_deployment_intent_sources,
    )

    state = ComposerState(application_start_id=0x3F9A0000000000A1)
    source_root = Path("/workspace/source/cell")
    destination_root = Path("/workspace/copies/deep/cell")
    deployment = {
        "api_version": "manyforge.deployment_intent.v3",
        "collision_rules": [{"rule_id": "everything", "clearance_m": 0.001}],
        "components": [
            {
                "component_id": "fixture_robot",
                "component_profile_id": "fixture_native_v1",
                "model_source": {
                    "urdf": {"project_path": "../models/fixture.urdf"},
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
    compiled = state.deployment_draft_session.preview_source(json.dumps(deployment))
    opened = state.deployment_draft_session.prepare_open_replacement(
        compiled, str(source_root / "deployment.yaml")
    )
    state.deployment_draft_session.install_prepared_replacement(opened)
    assert state.deployment_source_project_path == str(source_root)

    saved_deployment = rebase_deployment_intent_sources(
        compiled.document,
        source_project_root=source_root,
        destination_project_root=destination_root,
    )
    saved_document = {
        "scenario_id": "fixture_cell",
        "name": "Fixture cell",
        "description": "",
        "program_draft": None,
        "deployment_draft": saved_deployment,
        "program_role_assignment": None,
    }
    state.authoring_workspace.remember_project(
        document=saved_document,
        document_path=str(destination_root),
        source_document_digest=_DIGEST,
    )

    # Save As changes only the relative spelling needed to keep the same
    # physical model source. The unsaved-draft guard compares that meaning,
    # not the path spelling, while Apply still resolves from the old root.
    assert state.current_drafts_match_remembered_scenario()

    draft = state.deployment_draft_session.retained_state()
    edited = deepcopy(compiled.document)
    edited["collision_rules"][0]["clearance_m"] = 0.002  # an actual edit
    changed = state.deployment_draft_session.preview_source(json.dumps(edited))
    state.deployment_draft_session.commit_compiled(
        changed,
        draft_id=draft["draft_id"],
        expected_revision=draft["draft_revision"],
    )
    assert not state.current_drafts_match_remembered_scenario()


def test_run_rejects_a_loaded_deployment_draft_that_is_not_manager_applied():
    state = ComposerState(application_start_id=0x3F9A0000000000A1)
    _replace_deployment_session(
        state,
        DeploymentDraftSession(compiler=_compile),
    )
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
    state.published_scenario = lambda: {  # type: ignore[method-assign]
        "ready": True,
        "published_scenario": {
            "deployment_revision": "sha256:" + "2" * 64,
            "program_revision": _DIGEST,
        },
    }
    state.applied_deployment_for_publication = lambda _current: {  # type: ignore[method-assign]
        "different": "deployment"
    }
    # Editable Deployment intent does not carry a compiled revision. The
    # former revision-based guard skipped the mismatch in this state.
    original_snapshot = state.deployment_draft_session.snapshot

    def intent_snapshot() -> dict[str, Any]:
        snapshot = original_snapshot()
        snapshot["draft"]["deploymentRevision"] = None
        return snapshot

    state.deployment_draft_session.snapshot = intent_snapshot  # type: ignore[method-assign]

    class Headless:
        def program_run_start(self, _body):
            raise AssertionError("Run must be rejected before transport")

    state.authoring_admission = AuthoringAdmissionCoordinator(None, 0x3F9A0000000000A1)  # type: ignore[arg-type]
    app = FastAPI()
    app.include_router(create_headless_router(Headless(), state))  # type: ignore[arg-type]
    response = TestClient(app).post(
        "/api/program/runs",
        json={
            "request_key": "a1" * 16,
            "application_start_id": "3f9a0000000000a1",
            "source_label": "test",
            "scenario_id": "cell-a",
            "source_document_digest": _DIGEST,
            "expected_applied_record_digest": _DIGEST,
            "draft_program_revision": _DIGEST,
        },
    )
    assert response.status_code == 409
    assert (
        response.json()["detail"]["code"]
        == "execution_deployment_draft_unapplied"
    )


def test_run_uses_the_existing_transport_when_deployment_draft_matches_applied():
    state = ComposerState(application_start_id=0x3F9A0000000000A1)
    _replace_deployment_session(
        state,
        DeploymentDraftSession(compiler=_compile),
    )
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
    state.published_scenario = lambda: {  # type: ignore[method-assign]
        "ready": True,
        "published_scenario": {
            "deployment_revision": compiled.deployment_revision,
            "program_revision": _DIGEST,
        },
    }
    state.applied_deployment_for_publication = lambda _current: compiled.document  # type: ignore[method-assign]

    class Headless:
        def program_run_start(self, body):
            return {"accepted_request_key": body["request_key"]}

    state.authoring_admission = AuthoringAdmissionCoordinator(None, 0x3F9A0000000000A1)  # type: ignore[arg-type]
    app = FastAPI()
    app.include_router(create_headless_router(Headless(), state))  # type: ignore[arg-type]
    response = TestClient(app).post(
        "/api/program/runs",
        json={
            "request_key": "a1" * 16,
            "application_start_id": "3f9a0000000000a1",
            "source_label": "test",
            "scenario_id": "cell-a",
            "source_document_digest": _DIGEST,
            "expected_applied_record_digest": _DIGEST,
            "draft_program_revision": _DIGEST,
        },
    )
    assert response.status_code == 200
    assert response.json() == {"accepted_request_key": "a1" * 16}


def test_run_rejects_unapplied_program_draft_before_transport() -> None:
    state = ComposerState(application_start_id=0x3F9A0000000000A1)
    state.draft_session.snapshot = lambda: {"program_revision": _DIGEST}  # type: ignore[method-assign]
    state.published_scenario = lambda: {  # type: ignore[method-assign]
        "ready": True,
        "published_scenario": {"program_revision": "sha256:" + "2" * 64},
    }

    class Headless:
        def program_run_start(self, _body):
            raise AssertionError("Run must be rejected before transport")

    state.authoring_admission = AuthoringAdmissionCoordinator(None, 0x3F9A0000000000A1)  # type: ignore[arg-type]
    app = FastAPI()
    app.include_router(create_headless_router(Headless(), state))  # type: ignore[arg-type]
    response = TestClient(app).post(
        "/api/program/runs",
        json={
            "request_key": "a1" * 16,
            "application_start_id": "3f9a0000000000a1",
            "source_label": "test",
            "scenario_id": "cell-a",
            "source_document_digest": _DIGEST,
            "expected_applied_record_digest": _DIGEST,
            "draft_program_revision": _DIGEST,
        },
    )
    assert response.status_code == 409
    assert response.json()["detail"]["code"] == "execution_program_draft_unapplied"


@pytest.mark.parametrize("command_kind", ["program_motion_start", "operation_start",
                                         "direct_http_operation", "direct_http_refused"])
def test_only_program_start_rejects_unapplied_deployment_before_transport(command_kind: str) -> None:
    state = ComposerState(application_start_id=0x3F9A0000000000A1)
    _replace_deployment_session(
        state,
        DeploymentDraftSession(compiler=_compile),
    )
    compiled = _compile(json.dumps(_document()))
    retained = state.deployment_draft_session.retained_state()
    state.deployment_draft_session.commit_open(
        compiled,
        "/workspace/cell_a.deployment.yaml",
        draft_id=retained["draft_id"],
        expected_revision=0,
        confirm_discard=False,
    )
    state.published_scenario = lambda: {  # type: ignore[method-assign]
        "ready": True,
        "published_scenario": {"deployment_revision": compiled.deployment_revision},
    }
    state.applied_deployment_for_publication = lambda _current: {  # type: ignore[method-assign]
        "different": "deployment"
    }

    class Headless:
        def submit_control_command(self, _body, *, received_monotonic_ns=None):
            assert command_kind != "program_motion_start"
            return {"accepted": True}

        def execute(self, _body):
            if command_kind == "direct_http_refused":
                from manyforge_headless_api.client import HeadlessClientError
                raise HeadlessClientError("authoring_not_ready", "activation holds admission", status_code=409)
            return {"accepted": True}

    state.authoring_admission = AuthoringAdmissionCoordinator(None, 0x3F9A0000000000A1)  # type: ignore[arg-type]
    app = FastAPI()
    app.include_router(create_headless_router(Headless(), state))  # type: ignore[arg-type]
    if command_kind.startswith("direct_http_"):
        body = {
            "request_context": {"request_key": "b1" * 16, "source_label": "test"},
            "application_start_id": "3f9a0000000000a1",
            "program_revision": _DIGEST,
            "program_binding_id": _DIGEST, "program_run_id": None,
            "expected_graph_revision": _DIGEST, "expected_retained_bundle_id": _DIGEST,
            "expected_scene_revision": {"scene_id": _DIGEST, "revision_id": 1, "content_hash": _DIGEST},
            "participants": [{"target": {"component_id": "robot", "command_target_id": "arm"},
                              "required_capability_id": "execution.trajectory.execute", "action_span": None,
                              "payload": {
                                  "kind": "motion", "move_id": "manual-move",
                                  "base_frame": {"component_id": "robot", "frame_id": "base"},
                                  "tcp_endpoint": {"kind": "current"},
                                  "tolerances": {"joint_position_tol_rad": 0.05, "translation_tol_m": 0.005,
                                                 "rotation_tol_rad": 0.05},
                                  "speed_profile": {"velocity_scale": 1.0, "acceleration_scale": 1.0,
                                                    "max_tcp_linear_speed_mps": None},
                                  "timeouts": {"planning_timeout_s": 2.0, "execution_timeout_s": 10.0},
                                  "cache_mode": "default", "collision_rules": [],
                                  "target_mode": "absolute", "per_joint_dynamics_limits": {},
                                  "motion_interrupt_scopes": [],
                                  "motion": {"kind": "joint_goal", "joint_names": ["joint1"], "positions_rad": [0.1]},
                              }}],
            "coordination_policy": "independent",
        }
        response = TestClient(app).post("/api/execution/operations", json=body)
        assert response.status_code == (409 if command_kind == "direct_http_refused" else 200), response.text
        if command_kind == "direct_http_refused":
            assert response.json()["detail"]["code"] == "authoring_not_ready"
        else:
            assert response.json() == {"accepted": True}
        return
    response = TestClient(app).post(
        "/api/control/commands",
        json={
            "command_envelope": {
                "schema_version": "manyforge.execution_command.v2",
                "command_kind": command_kind,
                "source_label": "test",
                "control_session_number": None,
                "client_sequence": None,
                "freshness": {
                    "context_id": "freshness-1",
                    "execution_control_revision": 1,
                    "issued_server_sequence": "1",
                    "valid_for_ns": 750_000_000,
                },
                "expected_execution_control_revision": 1,
            },
            # A session-less command carries its key and `A` in the payload.
            "payload": {
                "request_key": "c1" * 16,
                "application_start_id": "3f9a0000000000a1",
            },
        },
    )
    if command_kind == "program_motion_start":
        assert response.status_code == 409
        assert response.json()["detail"]["code"] == "execution_deployment_draft_unapplied"
    else:
        assert response.status_code == 200
        assert response.json() == {"accepted": True}


@pytest.mark.parametrize("route", ["control_command", "recovery_action"])
def test_retry_is_refused_with_unapplied_draft_and_reset_is_not(route: str) -> None:
    state = ComposerState(application_start_id=0x3F9A0000000000A1)
    applied = {"program_revision": "sha256:" + "2" * 64}
    state.draft_session.snapshot = lambda: {  # type: ignore[method-assign]
        "program_loaded": True, "program_revision": _DIGEST,
    }
    state.published_scenario = lambda: {  # type: ignore[method-assign]
        "ready": True, "published_scenario": applied,
    }
    submitted: list[str] = []

    class Headless:
        def submit_control_command(self, body, *, received_monotonic_ns):
            # Composer's receipt time, taken before its own work (OD-240).
            assert type(received_monotonic_ns) is int
            submitted.append(body["payload"]["action_id"])
            return {"accepted": True}

        def cycle_recovery_action(self, body):
            submitted.append(body["action_id"])
            return {"accepted": True}

    state.authoring_admission = AuthoringAdmissionCoordinator(None, 0x3F9A0000000000A1)  # type: ignore[arg-type]
    app = FastAPI()
    app.include_router(create_headless_router(Headless(), state))  # type: ignore[arg-type]
    client = TestClient(app)

    def recover(action_id: str):
        payload = {
            "request_key": action_id.encode("utf-8").hex()[:32].ljust(32, "0"),
            "application_start_id": "3f9a0000000000a1",
            "source_label": "test",
            "offer_id": _DIGEST,
            "offer_revision": 1,
            "action_id": action_id,
            "reason": "operator-selected recovery action",
        }
        if route == "recovery_action":
            return client.post("/api/cycle/recovery-actions", json=payload)
        return client.post("/api/control/commands", json={
            "command_envelope": {
                "schema_version": "manyforge.execution_command.v2",
                "command_kind": "execution_recovery",
                "source_label": "test",
                "control_session_number": None,
                "client_sequence": None,
                "freshness": {
                    "context_id": "freshness-1",
                    "execution_control_revision": 1,
                    "issued_server_sequence": "1",
                    "valid_for_ns": 750_000_000,
                },
                "expected_execution_control_revision": 1,
            },
            "payload": payload,
        })

    refused = recover("retry_motion_from_current_state")
    assert refused.status_code == 409
    assert refused.json()["detail"]["code"] == "execution_program_draft_unapplied"
    assert refused.json()["detail"]["message"].startswith("Apply or Discard first")
    assert recover("acknowledge_and_continue").status_code == 409
    # Reset ends the run; it is how an Apply of the edited Draft begins.
    assert recover("reset_cycle").status_code == 200
    assert submitted == ["reset_cycle"]

    # Applied (or Discarded): the Draft is exactly what is Applied again.
    applied["program_revision"] = _DIGEST
    assert recover("retry_motion_from_current_state").status_code == 200
    assert submitted == ["reset_cycle", "retry_motion_from_current_state"]


def _configuration_preparation(tmp_path: Path):
    digest = "sha256:" + "a" * 64
    current = {"retained_bundle_id": "bundle.current", "graph_revision": "graph.current"}
    prepared_activation = {
        "schema_version": "manyforge.prepared_activation_input.v8",
        "prepared_activation_id": digest, "origin": {"kind": "scenario", "scenario_id": "cell",
        "source_document_digest": digest, "project_path": "/projects/cell.scenario"},
        "captured_input_digest": digest,
        "deployment_revision": digest,
        "retained_bundle_id": "bundle.next", "graph_revision": "graph.next",
        "program_revision": None,
        "program_outcome": {"kind": "no_program"},
        "node_environment_digest": digest, "assistant_mode": "forbidden", "scene_meshes": [],
    }
    preview = {
        "api_version": "manyforge.manager_command_response.v3", "ok": True,
        "converged": False,
        "diff": {kind: {"added": [], "changed": [], "removed": []} for kind in (
            "component_attachments", "controllable_components", "control_bindings",
            "control_domains", "model_instances", "runtime_units",
        )},
        "candidate_receipt": {
            "schema_version": "manyforge.manager_deployment_candidate_receipt.v3",
            "deployment_revision": digest,
            "prepared_activation_id": digest,
            "candidate_retained_bundle_id": "bundle.next", "candidate_graph_revision": "graph.next",
            "expected_current": current, "expected_admission_generation": 1,
            "impact_digest": digest,
            "destructive_impact": False, "receipt_digest": digest,
        },
        "impact": {
            "attachment_changed_components": [], "component_delta_digest": digest,
            "components": [
                {"controllable_component_id": "arm.1", "disposition": "retain", "replacement_reasons": []},
                {"controllable_component_id": "arm.2", "disposition": "retain", "replacement_reasons": []},
            ],
            "destructive": False, "detached_components": [], "shared_scene_projection_changed": False,
            "program": {"executable": False, "non_executable": None, "program_revision": None},

        },
    }

    class Headless:
        def prepare_activation(self, request):
            # OD-214: answered at acceptance; this preparation already ended.
            return {
                "schema_version": "manyforge.activation_preparation_reply.v1",
                "command": {
                    "kind": "activation_preparation",
                    "request_key": request["request_key"],
                    "captured_input_digest": request["captured_input_digest"],
                    "step": "preparing", "accepted_at_ns": "1",
                    "end": {"outcome": "succeeded", "code": None, "details": None,
                            "finished_at_ns": "2"},
                },
            }

        def prepared_activation(self, captured_input_digest):
            assert captured_input_digest == digest
            return {"prepared_activation": deepcopy(prepared_activation)}

        def manager_status(self):
            return {
                "source": {"available": True, "freshness": "fresh", "known_gaps": []},
                "status": {"manager_ready": True, "active": dict(current)},
            }

        def manager_deployment_candidate_preview(self, _request):
            return deepcopy(preview)

    state = ComposerState(application_start_id=0x3F9A0000000000A1, allowed_fs_roots=[str(tmp_path)])
    state.headless_client = Headless()  # type: ignore[assignment]
    intent = {
        "scenario_id": "cell", "project_path": "/projects/cell.scenario",
        "source_document_digest": digest, "captured_input_digest": digest,
        "deployment": {}, "model_source_closure": {}, "scene_mesh_closure": {}, "program": None, "program_role_assignment": {},
        "expected_manager_current": current, "expected_admission_generation": 1,
        "application_start_id": "3f9a0000000000a1", "request_key": "d1" * 16,
        "confirmation_token": None, "draft_id": "draft", "draft_revision": 1,
    }
    return state, intent, preview


@pytest.mark.parametrize("destructive", [False, True])
def test_configuration_confirmation_never_authorizes_bring_up(tmp_path: Path, destructive: bool) -> None:
    state, intent, preview = _configuration_preparation(tmp_path)
    preview["impact"]["destructive"] = destructive
    preview["candidate_receipt"]["destructive_impact"] = destructive
    prepared = state._prepare_authored_deployment_candidate(intent, None, apply=True)
    assert prepared["confirmation_required"] is destructive
    if destructive:
        intent["confirmation_token"] = prepared["confirmation_token"]
        prepared = state._prepare_authored_deployment_candidate(intent, None, apply=True)
    assert prepared["manager_apply"] is not None
    assert prepared["manager_apply"]["accepted_impact_digest"] == (
        preview["candidate_receipt"]["impact_digest"] if destructive else None
    )


def test_concurrent_preparations_wait_for_each_other_instead_of_refusing(tmp_path: Path) -> None:
    import threading

    state, intent, _preview = _configuration_preparation(tmp_path)
    prepare = state.headless_client.prepare_activation
    first_entered = threading.Event()
    release_first = threading.Event()
    active = 0
    overlapped = False

    def one_at_a_time(request):
        nonlocal active, overlapped
        active += 1
        overlapped = overlapped or active > 1
        first_entered.set()
        release_first.wait(5)
        active -= 1
        return prepare(request)

    state.headless_client.prepare_activation = one_at_a_time  # type: ignore[union-attr]
    results: list[dict[str, Any]] = []

    def run() -> None:
        results.append(state._prepare_authored_deployment_candidate(intent, None, apply=False))

    first = threading.Thread(target=run)
    first.start()
    assert first_entered.wait(5)
    second = threading.Thread(target=run)
    second.start()
    second.join(0.2)
    assert second.is_alive()  # waits for the first preparation
    release_first.set()
    first.join(5)
    second.join(5)
    assert not overlapped
    assert len(results) == 2


def _preparation_follow(state: ComposerState, statuses: list[Any]) -> list[dict[str, Any]]:
    """Make Headless accept the preparation and publish ``statuses`` after."""

    accepted = state.headless_client.prepare_activation
    replies: list[dict[str, Any]] = []

    def prepare(request):
        reply = accepted(request)
        reply["command"]["end"] = None  # still running when answered
        reply["command"]["step"] = "awaiting_admission"
        replies.append(reply)
        return reply

    def cycle_status():
        value = statuses.pop(0)
        if isinstance(value, Exception):
            raise value
        return value

    state.headless_client.prepare_activation = prepare  # type: ignore[union-attr]
    state.headless_client.program_cycle_status = cycle_status  # type: ignore[union-attr]
    state._ACTIVATION_PREPARATION_POLL_S = 0.0  # type: ignore[misc]
    return replies


def _preparation_rows(key: str, digest: str, end: dict[str, Any] | None) -> dict[str, Any]:
    row = {"kind": "activation_preparation", "request_key": key,
           "captured_input_digest": digest, "step": "preparing",
           "accepted_at_ns": "1", "end": end}
    return ({"in_progress": row, "ended": []} if end is None
            else {"in_progress": None, "ended": [row]})


def test_a_preparation_is_followed_to_its_end_in_the_status(tmp_path: Path) -> None:
    # OD-214 (T35): no limit on the wait; the status says when it ended,
    # and the prepared activation is read by its digest.
    state, intent, _preview = _configuration_preparation(tmp_path)
    statuses: list[Any] = []
    replies = _preparation_follow(state, statuses)
    succeeded = {"outcome": "succeeded", "code": None, "details": None, "finished_at_ns": "2"}

    def fill(request_key: str) -> None:
        digest = intent["captured_input_digest"]
        statuses.extend([
            {"activation_preparation": _preparation_rows(request_key, digest, None)},
            # The runner's absence never hides the preparation's own status.
            HeadlessClientError("program_runner_unavailable", "gone", 503, details={
                "activation_preparation": _preparation_rows(request_key, digest, None)}),
            {"activation_preparation": _preparation_rows(request_key, digest, succeeded)},
        ])

    original = state.headless_client.prepare_activation

    def prepare(request):
        fill(request["request_key"])
        return original(request)

    state.headless_client.prepare_activation = prepare  # type: ignore[union-attr]
    prepared = state._prepare_authored_deployment_candidate(intent, None, apply=True)
    assert prepared["manager_apply"] is not None
    assert statuses == []
    assert len(replies) == 1 and len(replies[0]["command"]["request_key"]) == 32


@pytest.mark.parametrize("outcome,status", [("refused", 409), ("failed", None),
                                            ("unknown", None)])
def test_a_preparation_that_did_not_succeed_raises_its_code(
    tmp_path: Path, outcome: str, status: int | None
) -> None:
    state, intent, _preview = _configuration_preparation(tmp_path)
    statuses: list[Any] = []
    _preparation_follow(state, statuses)
    original = state.headless_client.prepare_activation
    end = {"outcome": outcome, "code": "activation_preparation_failed",
           "details": {"preparer_detail": "x"}, "finished_at_ns": "2"}

    def prepare(request):
        statuses.append({"activation_preparation": _preparation_rows(
            request["request_key"], intent["captured_input_digest"], end)})
        return original(request)

    state.headless_client.prepare_activation = prepare  # type: ignore[union-attr]
    expected = HTTPException if status is not None else HeadlessClientError
    with pytest.raises(expected) as raised:
        state._prepare_authored_deployment_candidate(intent, None, apply=True)
    if status is not None:
        # A refused end is the definite refusal before any effect.
        assert raised.value.status_code == 409
        assert raised.value.detail["code"] == "activation_preparation_failed"
        assert raised.value.detail["details"] == {"preparer_detail": "x"}
    else:
        assert raised.value.code == "activation_preparation_failed"
        assert raised.value.status_code is None


def test_activation_order_comes_from_exact_applied_capture_not_draft(monkeypatch: pytest.MonkeyPatch) -> None:
    from manyforge_composer.backend.routes_deployment_draft import _workspace
    state = ComposerState(application_start_id=0x3F9A0000000000A1)
    state.headless_client = object()  # type: ignore[assignment]
    publication = {"scenario_id": "cell", "deployment_revision": _DIGEST,
                   "retained_bundle_id": "bundle", "graph_revision": _DIGEST}
    monkeypatch.setattr(state, "published_scenario", lambda: {"ready": True, "published_scenario": publication})
    def captured(current):
        assert current == publication
        return {"components": [{"component_id": "z"}, {"component_id": "a"}]}
    monkeypatch.setattr(state, "applied_deployment_for_publication", captured)
    assert _workspace(state)["managerApplied"]["activationOrder"] == {
        "retainedBundleId": "bundle", "graphRevision": _DIGEST, "componentIds": ["z", "a"]}
    monkeypatch.setattr(state, "applied_deployment_for_publication", lambda _current: None)
    assert _workspace(state)["managerApplied"]["activationOrder"] is None


def test_workspace_publishes_whether_saved_differs_from_the_exact_applied_deployment(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from manyforge_composer.backend.routes_deployment_draft import _workspace
    state = ComposerState(application_start_id=0x3F9A0000000000A1)
    state.headless_client = object()  # type: ignore[assignment]
    session = DeploymentDraftSession(compiler=_compile)
    _replace_deployment_session(state, session)
    publication = {"scenario_id": "cell_a", "deployment_revision": _DIGEST,
                   "retained_bundle_id": "bundle", "graph_revision": _DIGEST}
    monkeypatch.setattr(state, "published_scenario", lambda: {"ready": True, "published_scenario": publication})
    saved = _compile(json.dumps({**_document(), "components": []}))
    other = _compile(json.dumps({**_document(), "components": [], "cell_id": "cell_b"}))
    applied: dict[str, Any] = {"document": saved.document}
    monkeypatch.setattr(state, "applied_deployment_for_publication",
                        lambda _current: applied["document"])
    # No Saved Deployment: unknown.
    assert _workspace(state)["saved"]["differsFromApplied"] is None
    session.commit_open(saved, "/workspace/cell_a.deployment.yaml",
                        draft_id=session.retained_state()["draft_id"],
                        expected_revision=0, confirm_discard=False)
    # After a Revert (Saved == Applied) only the Draft is edited: Discard's job.
    session.commit_compiled(other, draft_id=session.retained_state()["draft_id"],
                            expected_revision=session.snapshot()["draft"]["revision"])
    view = _workspace(state)
    assert view["draft"]["differsFromSaved"] is True
    assert view["runAdmission"]["ready"] is False
    assert view["saved"]["differsFromApplied"] is False
    applied["document"] = other.document
    assert _workspace(state)["saved"]["differsFromApplied"] is True
    # No exact Applied Deployment: unknown.
    applied["document"] = None
    assert _workspace(state)["saved"]["differsFromApplied"] is None
    # A Save landing after the view captured its snapshot never pairs the
    # captured Saved with a comparison of the newer one.
    applied["document"] = saved.document
    retained = session.retained_state

    def saved_after_capture(current):
        monkeypatch.setattr(session, "retained_state",
                            lambda: {**retained(), "saved_document": other.document})
        return applied["document"]

    monkeypatch.setattr(state, "applied_deployment_for_publication", saved_after_capture)
    view = _workspace(state)
    assert view["saved"]["differsFromApplied"] is False


def test_draft_without_required_defaults_matches_the_deployment_it_applied(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """A Draft built from catalog rows leaves the robot's required runtime
    defaults absent; Apply captures the document with them filled. Run
    admission, Saved-vs-Applied and the Execute gate compare the same way."""

    from manyforge_composer.backend.deployment_defaults import with_defaults
    from manyforge_composer.backend.routes_deployment_draft import _workspace
    from manyforge_composer.backend.routes_headless import (
        _require_applied_authoring_drafts,
    )
    from manyforge_composer.backend.scenario_restore import saved_deployment_differs

    state = ComposerState(application_start_id=0x3F9A0000000000A1)
    state.headless_client = object()  # type: ignore[assignment]
    session = DeploymentDraftSession(compiler=_compile)
    _replace_deployment_session(state, session)
    robot = {
        "component_id": "lite6_robot",
        "target_settings": {"arm": {"maximum_tcp_linear_speed_mps": 0.25}},
        "configuration": {"runtime_kind": "ros2_control",
                          "runtime_configuration": {"component_streams": []}},
    }
    draft = _compile(json.dumps({**_document(), "components": [robot]}))
    session.commit_open(draft, "/workspace/cell_a.deployment.yaml",
                        draft_id=session.retained_state()["draft_id"],
                        expected_revision=0, confirm_discard=False)
    document = session.retained_state()["document"]
    applied = with_defaults(document)
    assert applied != document
    publication = {"scenario_id": "cell_a", "deployment_revision": _DIGEST,
                   "retained_bundle_id": "bundle", "graph_revision": _DIGEST,
                   "program_revision": _DIGEST}
    monkeypatch.setattr(state, "published_scenario",
                        lambda: {"ready": True, "published_scenario": publication})
    monkeypatch.setattr(state, "applied_deployment_for_publication",
                        lambda _current: deepcopy(applied))
    view = _workspace(state)
    assert view["runAdmission"] == {"ready": True, "reason": None}
    _require_applied_authoring_drafts(state)
    authored = {key: value for key, value in document.items()
                if key != "deployment_revision"}
    assert saved_deployment_differs(
        authored, {key: value for key, value in applied.items()
                   if key != "deployment_revision"}) is False
    # A real difference still counts.
    changed = deepcopy(applied)
    changed["components"][0]["configuration"]["runtime_configuration"][
        "execution_tick_hz"] = 50
    monkeypatch.setattr(state, "applied_deployment_for_publication",
                        lambda _current: deepcopy(changed))
    assert _workspace(state)["runAdmission"]["ready"] is False
    with pytest.raises(HTTPException):
        _require_applied_authoring_drafts(state)


@pytest.mark.parametrize("origin", [
    {"kind": "scenario", "scenario_id": "foreign", "source_document_digest": _DIGEST,
     "project_path": "/projects/foreign.scenario"},
    {"kind": "scenario", "scenario_id": "cell_a", "source_document_digest": "sha256:" + "f" * 64,
     "project_path": "/projects/cell_a.scenario"},
    # The same Scenario prepared from another project folder is another capture.
    {"kind": "scenario", "scenario_id": "cell_a", "source_document_digest": _DIGEST,
     "project_path": "/elsewhere/cell_a.scenario"},
    None,
])
def test_captured_authoring_context_requires_checked_scenario_origin(origin) -> None:
    state = ComposerState(application_start_id=0x3F9A0000000000A1)
    with pytest.raises(AuthoringOperationError) as error:
        state._captured_deployment_context({"deployment": {"cell_id": "current"},
            "scenario_id": "cell_a", "source_document_digest": _DIGEST,
            "project_path": "/projects/cell_a.scenario"}, {"origin": origin})
    assert error.value.code == "activation_authoring_capture_mismatch"


@pytest.mark.parametrize("purpose", ["preserve", "initialize"])
def test_captured_scene_initialization_is_part_of_manager_request(tmp_path, purpose):
    state, intent, _preview = _configuration_preparation(tmp_path)
    intent["scene_initialization"] = purpose
    prepared = state._prepare_authored_deployment_candidate(intent, None, apply=True)
    assert prepared["manager_apply"]["scene_initialization"] == purpose
    # Changing the local intent after capture cannot alter its prepared request.
    intent["scene_initialization"] = "initialize" if purpose == "preserve" else "preserve"
    assert prepared["manager_apply"]["scene_initialization"] == purpose


def _io_component(identity: str, timeout_s: int = 30) -> dict[str, Any]:
    return {
        "component_id": identity,
        "component_profile_id": "io_device_v0",
        "target_settings": {},
        "configuration": {
            "runtime_kind": "io_provider",
            "runtime_configuration": {
                "io_provider_devices": [
                    {
                        "controllable_component_id": identity,
                        "lifecycle_command_timeout_s": timeout_s,
                    }
                ],
                "io_signals": [],
            },
        },
    }


def _io_intent(*components: dict[str, Any]) -> dict[str, Any]:
    return {
        "api_version": "manyforge.deployment_intent.v3",
        "collision_rules": [{"rule_id": "everything", "clearance_m": 0.001}],
        "components": list(components),
        "attachments": [],
        "policies": {},
    }


def test_add_component_is_a_draft_only_edit(tmp_path: Path) -> None:
    """Add component composes into the Draft; Manager Applied is not touched."""

    client, state = _client(tmp_path)
    _seed_existing_deployment(
        state, _io_intent(_io_component("station_io")),
        compiler=validate_deployment_draft_document,
    )
    before = client.get("/api/deployment/workspace").json()

    added = client.post(
        "/api/deployment/draft/mutations",
        json=_request(
            before,
            path=["components"],
            operation="append",
            value=_io_component("plc_io"),
            kind="add-component",
        ),
    )

    assert added.status_code == 200, added.text
    rows = added.json()["draft"]["document"]["components"]
    assert [row["component_id"] for row in rows] == ["plc_io", "station_io"]
    assert added.json()["draft"]["revision"] == before["draft"]["revision"] + 1
    assert added.json()["managerApplied"] == before["managerApplied"]


@pytest.mark.parametrize(
    "after",
    [
        # An addition that also changes an existing row.
        lambda: [_io_component("plc_io"), _io_component("station_io", 12)],
        # Removing a component.
        lambda: [],
        # Replacing one component by another.
        lambda: [_io_component("plc_io")],
    ],
)
def test_every_component_edit_is_an_ordinary_draft_edit(
    tmp_path: Path, after
) -> None:
    client, state = _client(tmp_path)
    _seed_existing_deployment(
        state, _io_intent(_io_component("station_io")),
        compiler=validate_deployment_draft_document,
    )
    before = client.get("/api/deployment/workspace").json()

    edited = client.post(
        "/api/deployment/draft/mutations",
        json=_request(
            before,
            path=["components"],
            operation="set",
            value=after(),
            kind="not-an-addition",
        ),
    )

    assert edited.status_code == 200, edited.text
    assert edited.json()["draft"]["revision"] == before["draft"]["revision"] + 1


def test_removing_a_mounted_tool_drops_its_mount_row_in_the_same_edit(tmp_path: Path) -> None:
    """A mounted tool and its attachment row leave together; the Draft never
    holds an unattached tool without a root pose in between."""

    client, state = _client(tmp_path)
    _seed_existing_deployment(
        state, _scenario_robot_and_tool(), compiler=validate_deployment_draft_document,
    )
    before = client.get("/api/deployment/workspace").json()

    removed = client.post(
        "/api/deployment/draft/components/remove",
        json=_request(before, component_id="hand", kind="remove-hand"),
    )

    assert removed.status_code == 200, removed.text
    document = removed.json()["draft"]["document"]
    assert [row["component_id"] for row in document["components"]] == ["arm"]
    assert document["attachments"] == []
    assert removed.json()["draft"]["revision"] == before["draft"]["revision"] + 1
    assert removed.json()["managerApplied"] == before["managerApplied"]

    unknown = client.post(
        "/api/deployment/draft/components/remove",
        json=_request(removed.json(), component_id="hand", kind="remove-again"),
    )
    assert unknown.status_code == 404, unknown.text
    assert "component_not_found" in unknown.text


def _scenario_robot_and_tool() -> dict[str, Any]:
    """A Scenario Draft: a Real robot and a gripper tool on its flange."""

    arm = _model_component("arm")
    arm["lane"] = "real"
    arm["configuration"] = {
        "runtime_kind": "ros2_control",
        "runtime_configuration": {"device_address_ref": "cell_robot"},
    }
    hand = _model_component("hand", mounted=True)
    hand["configuration"] = {
        "runtime_kind": "ros2_control",
        "runtime_configuration": {
            "gripper_target_configurations": [
                {
                    "controllable_component_id": "hand",
                    "command_target_id": "gripper",
                    "actuation_kind": "position_commanded",
                }
            ],
        },
    }
    intent = _io_intent(arm, hand)
    intent["attachments"] = [_mount("arm", "hand")]
    return intent


def test_grasp_confirmation_edit_enters_a_scenario_draft(tmp_path: Path) -> None:
    """A Settings edit on a Scenario component is a Draft edit, not a refusal."""

    client, state = _client(tmp_path)
    _seed_existing_deployment(
        state, _scenario_robot_and_tool(), compiler=validate_deployment_draft_document,
    )
    before = client.get("/api/deployment/workspace").json()
    path = [
        "components", 1, "configuration", "runtime_configuration",
        "gripper_target_configurations", 0, "grasp_confirmation",
    ]
    value = {"policy": "required", "stall_window": {"min_position": 0.001, "max_position": 0.02}}

    edited = client.post(
        "/api/deployment/draft/mutations",
        json=_request(before, path=path, operation="set", value=value, kind="grasp"),
    )

    assert edited.status_code == 200, edited.text
    row = edited.json()["draft"]["document"]["components"][1]["configuration"][
        "runtime_configuration"
    ]["gripper_target_configurations"][0]
    assert row["grasp_confirmation"] == value
    assert edited.json()["draft"]["revision"] == before["draft"]["revision"] + 1
    # Nothing takes effect before the confirmed Apply.
    assert edited.json()["managerApplied"] == before["managerApplied"]


def test_endpoint_edit_enters_the_draft_and_applies_as_a_confirmed_replace(
    tmp_path: Path,
) -> None:
    """An endpoint field is a Draft parameter too; the Manager's Apply replaces
    the component, and that destructive impact needs the confirmation."""

    client, state = _client(tmp_path)
    _seed_existing_deployment(
        state, _scenario_robot_and_tool(), compiler=validate_deployment_draft_document,
    )
    before = client.get("/api/deployment/workspace").json()

    edited = client.post(
        "/api/deployment/draft/mutations",
        json=_request(
            before,
            path=["components", 0, "configuration", "runtime_configuration",
                  "device_address_ref"],
            operation="set",
            value="spare_robot",
            kind="endpoint",
        ),
    )

    assert edited.status_code == 200, edited.text
    document = edited.json()["draft"]["document"]
    assert document["components"][0]["configuration"]["runtime_configuration"][
        "device_address_ref"
    ] == "spare_robot"
    assert edited.json()["managerApplied"] == before["managerApplied"]

    # Apply carries the Draft; the Manager reports the changed controller
    # profile row and address as a replace (spec sections `address`, `launch`),
    # which is destructive, so Composer asks for the confirmation first.
    apply_state, intent, preview = _configuration_preparation(tmp_path)
    intent["deployment"] = document
    preview["impact"]["components"][0] = {
        "controllable_component_id": "arm",
        "disposition": "replace",
        "replacement_reasons": ["address", "launch"],
    }
    preview["impact"]["destructive"] = True
    preview["candidate_receipt"]["destructive_impact"] = True
    prepared = apply_state._prepare_authored_deployment_candidate(intent, None, apply=True)
    assert prepared["confirmation_required"] is True
    assert prepared["manager_apply"] is None
    intent["confirmation_token"] = prepared["confirmation_token"]
    confirmed = apply_state._prepare_authored_deployment_candidate(intent, None, apply=True)
    assert confirmed["manager_apply"]["accepted_impact_digest"] == (
        preview["candidate_receipt"]["impact_digest"]
    )


def _mount(robot: str, tool: str) -> dict[str, Any]:
    return {
        "parent": {"component_id": robot, "interface": "flange"},
        "child": {"component_id": tool, "interface": "mount"},
    }


def _model_component(identity: str, *, mounted: bool = False) -> dict[str, Any]:
    """A model row; a mounted tool carries no root pose (its mount places it)."""

    row: dict[str, Any] = {
        "component_id": identity,
        "component_profile_id": f"{identity}_v0",
        "model_source": {"urdf": {"project_path": f"{identity}.urdf"}, "package_roots": {}},
        "target_settings": {},
        "configuration": {"runtime_kind": "ros2_control"},
    }
    if not mounted:
        row["root_pose_in_cell"] = {
            "position_m": [0.0, 0.0, 0.0], "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
        }
    return row


def test_add_tool_with_its_mount_is_a_draft_only_edit(tmp_path: Path) -> None:
    """A new tool (no root pose) and its new attachment row are one pure addition."""

    client, state = _client(tmp_path)
    _seed_existing_deployment(
        state, _io_intent(_model_component("arm")),
        compiler=validate_deployment_draft_document,
    )
    before = client.get("/api/deployment/workspace").json()

    added = client.post(
        "/api/deployment/draft/components",
        json=_request(
            before,
            components=[_model_component("hand", mounted=True)],
            attachments=[_mount("arm", "hand")],
            kind="add-tool",
        ),
    )

    assert added.status_code == 200, added.text
    document = added.json()["draft"]["document"]
    rows = {row["component_id"]: row for row in document["components"]}
    assert list(rows) == ["arm", "hand"]
    assert "root_pose_in_cell" not in rows["hand"]
    assert document["attachments"] == [_mount("arm", "hand")]
    assert added.json()["draft"]["revision"] == before["draft"]["revision"] + 1
    assert added.json()["managerApplied"] == before["managerApplied"]


def test_add_tool_refuses_a_root_pose_on_the_mounted_tool(tmp_path: Path) -> None:
    """Slice B's intent validator: a mounted child takes no root pose."""

    client, state = _client(tmp_path)
    _seed_existing_deployment(
        state, _io_intent(_model_component("arm")),
        compiler=validate_deployment_draft_document,
    )
    before = client.get("/api/deployment/workspace").json()

    refused = client.post(
        "/api/deployment/draft/components",
        json=_request(
            before,
            components=[_model_component("hand")],
            attachments=[_mount("arm", "hand")],
            kind="posed-tool",
        ),
    )

    assert refused.status_code == 400, refused.text
    assert "root_pose_in_cell" in refused.text
    assert client.get("/api/deployment/workspace").json()["draft"] == before["draft"]


@pytest.mark.parametrize("after", [lambda: [], lambda: [_mount("arm", "station")]])
def test_changing_or_removing_an_attachment_is_a_draft_edit(
    tmp_path: Path, after
) -> None:
    client, state = _client(tmp_path)
    intent = _io_intent(_io_component("arm"), _io_component("hand"), _io_component("station"))
    intent["attachments"] = [_mount("arm", "hand")]
    _seed_existing_deployment(state, intent, compiler=validate_deployment_draft_document)
    before = client.get("/api/deployment/workspace").json()

    edited = client.post(
        "/api/deployment/draft/mutations",
        json=_request(
            before, path=["attachments"], operation="set", value=after(), kind="re-mount",
        ),
    )

    assert edited.status_code == 200, edited.text
    assert edited.json()["draft"]["document"]["attachments"] == after()


def _tool_component(identity: str, profile: str) -> dict[str, Any]:
    """A tool row: no root pose (its mount places it, or it is unmounted)."""

    row = _model_component(identity, mounted=True)
    row["component_profile_id"] = profile
    return row


def test_an_unmounted_tool_is_accepted_in_the_draft_but_not_a_robot(tmp_path: Path) -> None:
    """OD-366: a tool may be added with no robot (no attachment, no root pose);
    another model component without a root pose is still refused."""

    client, state = _client(tmp_path)
    _seed_existing_deployment(
        state, _io_intent(_model_component("arm")),
        compiler=validate_deployment_draft_document,
    )
    before = client.get("/api/deployment/workspace").json()

    added = client.post(
        "/api/deployment/draft/components",
        json=_request(
            before, components=[_tool_component("probe", "manyforge.passive_tool")],
            attachments=[], kind="add-tool",
        ),
    )

    assert added.status_code == 200, added.text
    document = added.json()["draft"]["document"]
    rows = {row["component_id"]: row for row in document["components"]}
    assert "root_pose_in_cell" not in rows["probe"]
    assert document["attachments"] == []

    refused = client.post(
        "/api/deployment/draft/components",
        json=_request(
            added.json(), components=[_model_component("arm_2", mounted=True)],
            attachments=[], kind="add-robot",
        ),
    )
    assert refused.status_code == 400
    assert refused.json()["detail"]["code"] == "deployment_draft_invalid"
    assert "only a tool may be unmounted" in refused.json()["detail"]["message"]


def test_preview_and_apply_refuse_an_unmounted_tool(tmp_path: Path) -> None:
    """OD-366: the cell never holds a floating tool; the refusal names it."""

    _unused, state = _client(tmp_path)
    deployment = validate_deployment_draft_document(json.dumps(_io_intent(
        _model_component("arm"), _tool_component("probe", "manyforge.passive_tool"),
    ))).document

    for apply in (False, True):
        with pytest.raises(HTTPException) as refused:
            state._prepare_authored_deployment_candidate(
                {"deployment": deployment}, None, apply=apply
            )
        assert refused.value.status_code == 409
        assert refused.value.detail["code"] == "deployment_tool_unmounted"
        assert refused.value.detail["message"].startswith("Tool probe is not mounted")
        assert refused.value.detail["details"] == {"component_ids": ["probe"]}
        assert refused.value.detail["effect"] == "none"


def test_replace_tool_moves_the_mount_and_rewrites_both_drafts(tmp_path: Path) -> None:
    """OD-367: the new tool takes over the old tool's mount and every reference
    to it in the Deployment and Program Drafts in one edit; the old tool
    leaves, and what the new tool does not declare is listed."""

    client, state = _client(tmp_path)
    state.draft_session = ComposerDraftSession(Program(
        Path(__file__).resolve().parents[2]
        / "examples/lite6_lite_gripper_pick_and_place.program.yaml",
        state.node_catalog,
    ))
    mount = {**_mount("arm", "lite_gripper"), "primary": True,
             "pose": {"position_m": [0.0, 0.0, 0.01], "orientation_xyzw": [0.0, 0.0, 0.0, 1.0]}}
    intent = _io_intent(
        _model_component("arm"),
        _tool_component("lite_gripper", "ufactory.lite_gripper"),
        _tool_component("probe", "manyforge.passive_tool"),
    )
    intent["attachments"] = [mount]
    intent["collision_rules"].append({
        "rule_id": "fingers_clear",
        "a": [{"kind": "collision_body", "component_id": "lite_gripper",
               "collision_body_id": "gripper_fingers"}],
        "b": [{"kind": "scene_objects"}],
        "clearance_m": 0.002,
    })
    _seed_existing_deployment(state, intent, compiler=validate_deployment_draft_document)
    before = client.get("/api/deployment/workspace").json()
    program_revision = state.draft_session.revision

    replaced = client.post(
        "/api/deployment/draft/components/replace-tool",
        json=_request(before, component_id="lite_gripper",
                      replacement_component_id="probe", kind="replace"),
    )

    assert replaced.status_code == 200, replaced.text
    body = replaced.json()
    document = body["workspace"]["draft"]["document"]
    assert [row["component_id"] for row in document["components"]] == ["arm", "probe"]
    assert document["attachments"] == [{**mount, "child": {"component_id": "probe", "interface": "mount"}}]
    assert document["collision_rules"][1]["a"][0]["component_id"] == "probe"
    assert body["workspace"]["draft"]["revision"] == before["draft"]["revision"] + 1
    forest = json.dumps(state.draft_session.snapshot()["authoring"]["forest"])
    assert '"lite_gripper"' not in forest
    assert state.draft_session.revision == program_revision + 1
    assert body["replacement"]["programReferences"] == 8
    # The passive tool's target is `tool`, its bodies default_collision and tip.
    assert body["replacement"]["lacks"] == {
        "collisionBodies": ["gripper_collision", "gripper_fingers"],
        "commandTargets": ["gripper"],
        "frames": [],
    }


def test_replace_tool_refuses_what_is_not_a_mounted_tool(tmp_path: Path) -> None:
    client, state = _client(tmp_path)
    _seed_existing_deployment(
        state,
        _io_intent(_model_component("arm"), _tool_component("probe", "manyforge.passive_tool")),
        compiler=validate_deployment_draft_document,
    )
    before = client.get("/api/deployment/workspace").json()

    for previous, replacement in (("probe", "arm"), ("arm", "probe")):
        refused = client.post(
            "/api/deployment/draft/components/replace-tool",
            json=_request(before, component_id=previous,
                          replacement_component_id=replacement, kind="replace"),
        )
        assert refused.status_code == 409
        assert refused.json()["detail"]["code"] == "deployment_tool_replacement_invalid"
    assert client.get("/api/deployment/workspace").json()["draft"] == before["draft"]

