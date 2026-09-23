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
    compile_deployment_document,
)
from manyforge_composer.backend.draft_session import ComposerDraftSession
from manyforge_composer.backend.routes_deployment_draft import (
    _raise,
    create_deployment_draft_router,
)
from manyforge_composer.backend.authoring_admission import (
    AuthoringOperationError,
    LocalAuthoringCommit,
)
from manyforge_composer.backend.authoring_workspace import AuthoringWorkspace
from manyforge_composer.backend.routes_headless import create_headless_router
from manyforge_composer.backend.state import ComposerState
from manyforge_composer.backend.app import create_app


_DIGEST = "sha256:" + "1" * 64


def test_scene_preparation_unavailable_keeps_typed_http_error() -> None:
    failure = AuthoringOperationError(
        "program_scene_preparation_unavailable",
        "Program is applied, but backend authoring readiness is not established",
    )
    with pytest.raises(HTTPException) as error:
        _raise(failure)
    assert error.value.status_code == 503
    assert error.value.detail == {"code": failure.code, "message": str(failure)}


def _import_jerk_source():
    document = json.loads(
        (
            Path(__file__).resolve().parents[2]
            / "examples/lite6_lite_gripper_pick_and_place.deployment.yaml"
        ).read_text()
    )
    runtime = document["graph"]["runtime_units"][0]
    target = runtime["configuration"]["motion_target_limits"][0]
    return (
        document,
        target["ordinary_motion_jerk"],
        (
            runtime["runtime_unit_id"],
            target["controllable_component_id"],
            target["command_target_id"],
            [f"joint{index}" for index in range(1, 7)],
        ),
    )


def test_moveit_jerk_import_preserves_activation_overrides_and_source_metadata():
    import manyforge_headless_py as native

    document, settings, selection = _import_jerk_source()
    settings["joint_limits"] = [{"source_joint_name": "joint1", "override_rad_s3": 10}]
    raw = """joint_limits:
  joint1: {has_jerk_limits: true, max_jerk: 12}
  joint2: {has_jerk_limits: false, max_jerk: 99}
  joint3: {has_jerk_limits: false, max_jerk: 0}
  unrelated: {has_jerk_limits: true, max_jerk: 100}
"""
    original = json.dumps(document)
    source_id = "robot_moveit_config/config/joint_limits.yaml"
    result = native.import_moveit_angular_jerk_limits(
        original, raw, source_id, *selection
    )
    assert result == native.compile_deployment_draft(result["canonical_json"])
    assert result == native.import_moveit_angular_jerk_limits(
        result["canonical_json"], raw, source_id, *selection
    )
    imported = json.loads(result["canonical_json"])
    target = imported["graph"]["runtime_units"][0]["configuration"][
        "motion_target_limits"
    ][0]
    assert target["ordinary_motion_jerk"]["enabled"] is False
    assert target["ordinary_motion_jerk"]["preset_reference_time_s"] == 1
    rows = target["ordinary_motion_jerk"]["joint_limits"]
    assert [row["source_joint_name"] for row in rows] == ["joint1", "joint2"]
    assert rows[0]["override_rad_s3"] == 10
    for row, enabled, maximum in zip(rows, [True, False], [12, 99], strict=True):
        assert row["imported"] == {
            "enabled": enabled,
            "maximum_rad_s3": maximum,
            "machine_required": False,
            "source_id": source_id,
            "source_digest": "sha256:" + hashlib.sha256(raw.encode()).hexdigest(),
        }
    assert imported["policies"] == document["policies"]
    assert json.dumps(document) == original


@pytest.mark.parametrize(
    "row",
    [
        "{has_jerk_limits: true}",
        "{has_jerk_limits: true, max_jerk: 0}",
        "{has_jerk_limits: 'true', max_jerk: 12}",
        "{has_jerk_limits: true, max_jerk: true}",
        "{has_jerk_limits: false, max_jerk: -1}",
        "{max_jerk: 12, max_jerk: 14}",
    ],
)
def test_moveit_jerk_import_rejects_malformed_limits(row):
    import manyforge_headless_py as native

    document, _settings, selection = _import_jerk_source()
    with pytest.raises(Exception):
        native.import_moveit_angular_jerk_limits(
            json.dumps(document),
            "joint_limits:\n  joint1: " + row,
            "robot/config/joint_limits.yaml",
            *selection,
        )


def test_moveit_import_cannot_weaken_required_limits_or_overrides():
    import manyforge_headless_py as native

    document, settings, selection = _import_jerk_source()
    settings["enabled"] = True
    settings["joint_limits"] = [
        {
            "source_joint_name": "joint1",
            "override_rad_s3": 10,
            "imported": {
                "maximum_rad_s3": 12,
                "enabled": True,
                "machine_required": True,
                "source_id": "robot/config/joint_limits.yaml",
                "source_digest": _DIGEST,
            },
        }
    ]
    for row in [
        "{}",
        "{has_jerk_limits: false, max_jerk: 12}",
        "{has_jerk_limits: true, max_jerk: 13}",
        "{has_jerk_limits: true, max_jerk: 9}",
    ]:
        with pytest.raises(Exception, match="required limit|override"):
            native.import_moveit_angular_jerk_limits(
                json.dumps(document),
                "joint_limits:\n  joint1: " + row,
                "robot/config/joint_limits.yaml",
                *selection,
            )
    with pytest.raises(Exception, match="unique"):
        native.import_moveit_angular_jerk_limits(
            json.dumps(document),
            "joint_limits: {}",
            "robot/config/joint_limits.yaml",
            *selection[:3],
            ["joint1", "joint1"],
        )
    with pytest.raises(Exception, match="motion target"):
        native.import_moveit_angular_jerk_limits(
            json.dumps(document),
            "joint_limits: {}",
            "robot/config/joint_limits.yaml",
            *selection[:2],
            "foreign-target",
            selection[3],
        )


def test_pinned_lite6_moveit_source_without_jerk_does_not_invent_limits():
    import manyforge_headless_py as native

    document, _settings, selection = _import_jerk_source()
    source_id = "xarm_moveit_config/config/lite6/joint_limits.yaml"
    raw = (
        Path(__file__).resolve().parents[2]
        / "manyforge_example_models/models"
        / "lite6_lite_gripper/upstream_source"
        / source_id
    ).read_text()
    result = native.import_moveit_angular_jerk_limits(
        json.dumps(document), raw, source_id, *selection
    )
    assert result == native.compile_deployment_draft(json.dumps(document))


@pytest.mark.parametrize(
    "enabled,machine_required,two_robots,unit_index",
    [
        (False, False, False, 0),
        (True, False, False, 0),
        (True, True, False, 0),
        (True, False, True, 0),
        (True, False, True, 1),
    ],
)
def test_ordinary_jerk_source_roundtrip_preserves_choice_and_provenance(
    tmp_path, enabled, machine_required, two_robots, unit_index
):
    source = json.loads(
        (
            Path(__file__).resolve().parents[2]
            / "examples/lite6_lite_gripper_pick_and_place.deployment.yaml"
        ).read_text()
    )
    if two_robots:
        # Reuse the real component definition, with disjoint instance identities.
        # Shared cell-wide providers/catalogs remain singletons.
        for section in (source, source["graph"], source["policies"]):
            for rows in section.values():
                if not isinstance(rows, list):
                    continue
                rows.extend(
                    json.loads(json.dumps(row).replace("lite6_robot", "peer_robot"))
                    for row in list(rows)
                    if isinstance(row, dict)
                    and any(
                        isinstance(value, str) and value.startswith("lite6_robot")
                        for value in row.values()
                    )
                )
        assert len(source["graph"]["runtime_units"]) == 2
    target = source["graph"]["runtime_units"][unit_index]["configuration"][
        "motion_target_limits"
    ][0]
    assert target["ordinary_motion_jerk"]["enabled"] is False
    settings = {
        "enabled": enabled,
        "preset_reference_time_s": 1.25,
        "joint_limits": [
            {
                "source_joint_name": "joint1",
                "override_rad_s3": 10.0,
                "imported": {
                    "maximum_rad_s3": 12.0,
                    "enabled": True,
                    "machine_required": machine_required,
                    "source_id": "robot_moveit_config/config/joint_limits.yaml",
                    "source_digest": _DIGEST,
                },
            }
        ],
    }
    client, state = _client(tmp_path)
    # Keep the existing lightweight admission double, but use the real source
    # compiler and actual workspace mutation/save/open routes.
    _replace_deployment_session(state, DeploymentDraftSession())
    original = tmp_path / "original.deployment.yaml"
    original.write_text(json.dumps(source))
    workspace = client.get("/api/deployment/workspace").json()
    opened = client.post(
        "/api/deployment/open-draft",
        json=_request(workspace, path=str(original), confirmDiscard=False, kind="open"),
    )
    assert opened.status_code == 200, opened.text
    changed = client.post(
        "/api/deployment/draft/mutations",
        json=_request(
            opened.json(),
            path=[
                "graph",
                "runtime_units",
                unit_index,
                "configuration",
                "motion_target_limits",
                0,
                "ordinary_motion_jerk",
            ],
            value=settings,
        ),
    )
    assert changed.status_code == 200, changed.text
    assert changed.json()["draft"]["differsFromSaved"] is True
    if machine_required:
        jerk_path = [
            "graph",
            "runtime_units",
            unit_index,
            "configuration",
            "motion_target_limits",
            0,
            "ordinary_motion_jerk",
        ]
        for suffix, value in [
            (["enabled"], False),
            (["joint_limits", 0, "imported", "enabled"], False),
            (["joint_limits", 0, "override_rad_s3"], 13.0),
        ]:
            rejected = client.post(
                "/api/deployment/draft/mutations",
                json=_request(
                    changed.json(),
                    path=jerk_path + suffix,
                    value=value,
                    kind="invalid-" + "-".join(map(str, suffix)),
                ),
            )
            assert rejected.status_code == 400, (suffix, rejected.status_code)
            assert rejected.json()["detail"]["code"] == "deployment_draft_invalid"
            assert (
                client.get("/api/deployment/workspace").json()["draft"]
                == changed.json()["draft"]
            )
    saved = tmp_path / "jerk.deployment.yaml"
    saved_response = client.post(
        "/api/deployment/save-as",
        json=_request(
            changed.json(), path=str(saved), confirmOverwrite=False, kind="save"
        ),
    )
    assert saved_response.status_code == 200, saved_response.text
    opened_again = client.post(
        "/api/deployment/open-draft",
        json=_request(
            saved_response.json(), path=str(saved), confirmDiscard=False, kind="reopen"
        ),
    )
    assert opened_again.status_code == 200, opened_again.text
    assert opened_again.json()["draft"]["differsFromSaved"] is False
    reopened = compile_deployment_document(saved.read_text())
    assert reopened.document == opened_again.json()["draft"]["document"]
    assert (
        reopened.document["graph"]["runtime_units"][unit_index]["configuration"][
            "motion_target_limits"
        ][0]["ordinary_motion_jerk"]
        == settings
    )
    # Off retains the numbers but does not activate them; the independent
    # controlled-Stop choices are unchanged in either case.
    assert reopened.document["policies"] == source["policies"]
    if two_robots:
        untouched = source["graph"]["runtime_units"][1 - unit_index]
        peer = next(
            unit
            for unit in reopened.document["graph"]["runtime_units"]
            if unit["runtime_unit_id"] == untouched["runtime_unit_id"]
        )
        assert peer == untouched
    restored = DeploymentDraftSession()
    restored.restore_retained_state(state.deployment_draft_session.retained_state())
    assert restored.snapshot() == state.deployment_draft_session.snapshot()
    client.close()


@pytest.mark.parametrize("enabled", [False, True])
def test_explicit_stop_jerk_migration_recompiles_and_preserves_existing_settings(
    enabled,
):
    import manyforge_headless_py

    source = json.loads(
        (
            Path(__file__).resolve().parents[2]
            / "examples/lite6_lite_gripper_pick_and_place.deployment.yaml"
        ).read_text()
    )
    rows = [
        row
        for row in source["policies"]["stop_stage_parameters"]
        if row["provider_contract_ref"]["contract_schema_id"]
        == "manyforge.value_schema.provider_contract.motion_controlled_stop.v1"
    ]
    assert len(rows) >= 2
    preserved = deepcopy(source)
    for row in rows:
        del row["default_contract_parameters"]["jerk_limits_enabled"]
        row["provider_contract_ref"][
            "contract_schema_revision"
        ] = "sha256:afbbcda3b81eabff2bbdffb5658fb1fcd3331cde896cadf0681793a2f9db90dd"
    # A reviewed choice must survive migration of another route/target.
    rows[0]["default_contract_parameters"]["jerk_limits_enabled"] = not enabled
    original = json.dumps(source)
    with pytest.raises(Exception):
        manyforge_headless_py.compile_deployment_draft(original)
    result = manyforge_headless_py.migrate_controlled_stop_jerk_choice(
        original, enabled
    )
    compiled = manyforge_headless_py.compile_deployment_draft(result["canonical_json"])
    assert result == compiled
    migrated = json.loads(result["canonical_json"])
    expected = deepcopy(preserved)
    expected_rows = [
        row
        for row in expected["policies"]["stop_stage_parameters"]
        if row["provider_contract_ref"]["contract_schema_id"]
        == "manyforge.value_schema.provider_contract.motion_controlled_stop.v1"
    ]
    for index, row in enumerate(expected_rows):
        row["default_contract_parameters"]["jerk_limits_enabled"] = (
            not enabled if index == 0 else enabled
        )
    expected_result = manyforge_headless_py.compile_deployment_draft(
        json.dumps(expected)
    )
    assert migrated == json.loads(expected_result["canonical_json"])
    assert json.dumps(source) == original
    # Idempotent even with the opposite migration choice: explicit values win.
    assert (
        manyforge_headless_py.migrate_controlled_stop_jerk_choice(
            result["canonical_json"], not enabled
        )
        == result
    )
    rows[0]["provider_contract_ref"]["contract_schema_revision"] = _DIGEST
    with pytest.raises(Exception, match="recognized schema revision"):
        manyforge_headless_py.migrate_controlled_stop_jerk_choice(
            json.dumps(source), enabled
        )
    with pytest.raises(TypeError):
        manyforge_headless_py.migrate_controlled_stop_jerk_choice(original, "on")


def test_cpp_draft_normalizer_reports_minimum_for_a_decimal_stop_duration():
    document = json.loads(
        (
            Path(__file__).resolve().parents[2]
            / "examples/lite6_lite_gripper_pick_and_place.deployment.yaml"
        ).read_text()
    )
    binding_revisions = {
        row["control_binding_id"]: row["control_binding_revision"]
        for row in document["graph"]["control_bindings"]
    }
    assert document["policies"]["stop_timing_parameters"]
    for selected in document["policies"]["stop_timing_parameters"]:
        assert (
            selected["control_binding_revision"]
            == binding_revisions[selected["control_binding_id"]]
        )
    binding = next(
        row
        for row in document["graph"]["control_bindings"]
        if row["capability_id"] == "execution.trajectory.execute"
    )
    for row in document["policies"]["stop_stage_parameters"]:
        if (
            row["control_binding_id"] == binding["control_binding_id"]
            and row["stop_escalation_stage_id"] == "motion_controlled_stop"
        ):
            row["default_contract_parameters"]["deceleration_time_s"] = 1.1
    document["policies"]["stop_timing_parameters"] = [
        {
            "control_binding_id": binding["control_binding_id"],
            "control_binding_revision": binding["control_binding_revision"],
            "timing": {
                "command_latency_allowance_ns": 250_000_000,
                "settling_allowance_ns": 750_000_000,
                "operational_timeout_ns": 2_100_000_000,
            },
        }
    ]
    compiled = compile_deployment_document(json.dumps(document))
    assert (
        compiled.document["policies"]["stop_timing_parameters"]
        == document["policies"]["stop_timing_parameters"]
    )
    document["policies"]["stop_timing_parameters"][0]["timing"][
        "operational_timeout_ns"
    ] -= 1
    with pytest.raises(DeploymentDraftError, match="minimum 2.100000000 s"):
        compile_deployment_document(json.dumps(document))


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
        "timing": None,
    }
    result = _edit_controlled_stop_profile(source, profile, **selection)
    for row in result["policies"]["stop_stage_parameters"][:2]:
        parameters = row["default_contract_parameters"]
        assert parameters["jerk_limits_enabled"] is enabled
        assert parameters["recoverable_substitution_mode"] == "spring_back_hold"
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


def test_stop_timing_edit_converges_all_bindings_for_one_authored_target():
    source = _focused_document()
    other_binding = deepcopy(source["graph"]["control_bindings"][0])
    other_binding["control_binding_id"] = "other_binding"
    source["graph"]["control_bindings"].append(other_binding)
    other_stage = deepcopy(source["policies"]["stop_stage_parameters"][0])
    other_stage["control_binding_id"] = "other_binding"
    source["policies"]["stop_stage_parameters"].append(other_stage)
    timing = {
        "command_latency_allowance_ns": 250_000_000,
        "settling_allowance_ns": 750_000_000,
        "operational_timeout_ns": 3_000_000_000,
    }
    other = {
        "control_binding_id": "other_binding",
        "control_binding_revision": _DIGEST,
        "timing": timing,
    }
    source["policies"]["stop_timing_parameters"] = [deepcopy(other)]
    saved_source = deepcopy(source)
    selection = {"controllable_component_id": "robot", "command_target_id": "arm"}
    # A timing-only edit does not require a fictitious motion profile.
    result = _edit_controlled_stop_profile(source, None, **selection, timing=timing)
    assert source == saved_source
    assert (
        result["policies"]["stop_stage_parameters"]
        == source["policies"]["stop_stage_parameters"]
    )
    assert result["policies"]["stop_timing_parameters"] == [
        {
            "control_binding_id": "arm_binding",
            "control_binding_revision": _DIGEST,
            "timing": timing,
        },
        other,
    ]
    restored = _edit_controlled_stop_profile(result, None, **selection, timing=None)
    assert "stop_timing_parameters" not in restored["policies"]
    with pytest.raises(DeploymentDraftError, match="no longer matches"):
        _edit_controlled_stop_profile(
            source,
            None,
            controllable_component_id="other",
            command_target_id="arm",
            timing=timing,
        )
    with pytest.raises(DeploymentDraftError, match="integer nanosecond"):
        _edit_controlled_stop_profile(
            source, None, **selection, timing={**timing, "settling_allowance_ns": True}
        )


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
        **_kwargs,
    ):
        if validate_under_claim is not None:
            validate_under_claim()
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


class _Headless:
    pass


def _request(workspace: dict[str, Any], **extra: Any) -> dict[str, Any]:
    extra.pop("local", True)
    identity = workspace["authoringWorkspace"]
    request = {
        "draftId": workspace["draft"]["id"],
        "expectedDraftRevision": workspace["draft"]["revision"],
        "expectedAdmissionGeneration": 1,
        "requestOwnerEpoch": "browser-test",
        "requestId": "request-"
        + str(workspace["draft"]["revision"])
        + "-"
        + extra.pop("kind", "edit"),
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


def _client(tmp_path: Path) -> tuple[TestClient, ComposerState]:
    state = ComposerState()
    _replace_deployment_session(
        state,
        DeploymentDraftSession(compiler=_compile),
    )
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
    applied = deepcopy(workspace["draft"]["document"])
    applied_revision = workspace["draft"]["deploymentRevision"]

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
    assert (
        json.loads((tmp_path / "saved.deployment.yaml").read_text())["name"]
        == "Edited cell"
    )

    response = client.post(
        "/api/deployment/draft/mutations",
        json=_request(workspace, path=["name"], value="Unsaved"),
    )
    workspace = response.json()
    assert applied is not None
    state.applied_deployment_document = applied
    state.current_scenario = lambda: {  # type: ignore[method-assign]
        "ready": True,
        "current_scenario": {
            "scenario_id": "cell_a",
            "deployment_revision": applied_revision,
        },
        "unavailable_reasons": [],
    }
    response = client.post(
        "/api/deployment/discard",
        json=_request(workspace, kind="discard"),
    )
    assert response.status_code == 200, response.text
    assert response.json()["draft"]["document"]["name"] == applied["name"]
    assert (
        state.authoring_retained_state()["schema_version"]
        == "manyforge.composer_authoring_workspace.v2"
    )


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
    assert (
        client.get("/api/deployment/workspace").json()["draft"]["document"]["name"]
        == "Current"
    )


def test_authored_deployment_apply_carries_one_prepared_identity_through_confirmation(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    from manyforge_composer.backend import routes_deployment_draft

    client, state = _client(tmp_path)
    project = (
        Path(__file__).resolve().parents[2]
        / "examples/lite6_pick_and_place.scenario"
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
        json=_request(opened, path=["name"], value="Authored", kind="edit"),
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
        document=None,
        document_path=str(project),
        source_document_digest="sha256:" + "6" * 64,
        explicit={
            "program_role_assignment": json.loads(
                (project / "assignment.yaml").read_text(encoding="utf-8")
            ),
            "typed_parameter_overrides": {},
        },
    )
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
    effects: list[tuple[dict[str, Any], bool]] = []

    confirmation_token = "sha256:" + "c" * 64

    def execute_candidate(
        intent,
        _claim,
        *,
        apply,
        prepared=None,
        prepare_only=False,
    ):
        prepared_activation = {
            "schema_version": "manyforge.prepared_activation_input.v2",
            "prepared_activation_id": "sha256:" + "9" * 64,
            "source_document_digest": intent["source_document_digest"],
            "captured_input_digest": intent["captured_input_digest"],
            "deployment_artifact_id": edited["draft"]["deploymentArtifactId"],
            "deployment_revision": candidate_revision,
            "retained_bundle_id": "sha256:" + "a" * 64,
            "graph_revision": "sha256:" + "7" * 64,
            "program_id": program["program_id"],
            "program_revision": program["program_revision"],
            "program_binding_id": "sha256:" + "b" * 64,
            "node_environment_digest": "sha256:" + "d" * 64,
            "assistant_mode": "forbidden",
            "typed_parameter_overrides": {},
        }
        if prepared is None:
            confirmation_required = (
                apply and intent.get("confirmation_token") != confirmation_token
            )
            result = {
                "status": (
                    "confirmation_required"
                    if confirmation_required
                    else "applied" if apply else "previewed"
                ),
                "confirmation_token": (
                    confirmation_token if confirmation_required else None
                ),
                "prepared_activation": prepared_activation,
                "candidate_preview": (
                    {"diff": {"runtime_units": {"changed": ["runtime"]}}}
                    if confirmation_required
                    else None
                ),
                "manager_apply": None,
                "preparation": (
                    {
                        "request_owner_epoch": intent[
                            "preparation_request_owner_epoch"
                        ],
                        "request_id": intent["preparation_request_id"],
                    }
                    if confirmation_required
                    else None
                ),
                "phases_ms": {"preparation_ms": 1.0},
                "apply_id": None,
            }
            prepared = {"result": result}
        if prepare_only:
            return prepared
        effects.append((deepcopy(intent), apply))
        result = deepcopy(prepared["result"])
        if result["status"] == "confirmation_required":
            return result
        return result

    state.execute_authored_deployment_candidate = execute_candidate  # type: ignore[method-assign]

    class PreviewMustNotClaim(_Coordinator):
        def execute_external_submission(self, *args, **kwargs):
            pytest.fail("effect-free Deployment Preview must not claim admission")

    coordinator = state.authoring_admission
    state.authoring_admission = PreviewMustNotClaim()  # type: ignore[assignment]
    preview = client.post(
        "/api/deployment/draft/preview",
        json=_request(edited, kind="preview", local=False),
    )
    assert preview.status_code == 200, preview.text
    assert preview.json()["result"]["status"] == "previewed"
    state.authoring_admission = coordinator

    confirmation = client.post(
        "/api/deployment/draft/apply",
        json=_request(edited, kind="apply", local=False),
    )
    assert confirmation.status_code == 200, confirmation.text
    assert confirmation.json()["result"]["status"] == "confirmation_required"
    assert confirmation.json()["result"]["confirmation_token"] == confirmation_token
    assert current["deployment_revision"] == base_revision
    assert effects[-1][0]["program"] == program
    assert effects[-1][0]["deployment"] == edited["draft"]["document"]
    capture_calls_after_preparation = capture_calls

    applied = client.post(
        "/api/deployment/draft/apply",
        json=_request(
            edited,
            kind="apply-confirmed",
            confirmationToken=confirmation_token,
            preparationRequestOwnerEpoch=confirmation.json()["result"][
                "preparation"
            ]["request_owner_epoch"],
            preparationRequestId=confirmation.json()["result"]["preparation"][
                "request_id"
            ],
            local=False,
        ),
    )
    assert applied.status_code == 202, applied.text
    assert applied.json()["result"]["status"] == "applied"
    # This regression proves preparation reuse only. The test double does not
    # publish the candidate bundle through Manager's transition owner, so the
    # Manager-facing observation stays unchanged.
    assert current["deployment_revision"] == base_revision
    # Confirmation performs the same bounded source capture again and rejects
    # a changed digest before Manager teardown; Composer retains no operation
    # journal or hidden candidate registry.
    assert capture_calls == capture_calls_after_preparation + 2
    assert applied.json()["result"]["prepared_activation"] == (
        confirmation.json()["result"]["prepared_activation"]
    )


def test_activation_submits_exact_preparation_to_manager_without_post_rebind() -> None:
    state = ComposerState()
    receipt = {
        "expected_current": {
            "retained_bundle_id": "sha256:" + "2" * 64,
            "graph_revision": "sha256:" + "3" * 64,
        },
        "candidate_retained_bundle_id": "sha256:" + "4" * 64,
        "candidate_graph_revision": "sha256:" + "5" * 64,
    }
    prepared_activation = {
        "schema_version": "manyforge.prepared_activation_input.v2",
        "scenario_id": "scenario-a",
        "prepared_activation_id": "sha256:" + "6" * 64,
        "source_document_digest": "sha256:" + "7" * 64,
        "captured_input_digest": "sha256:" + "8" * 64,
        "deployment_artifact_id": "cell-a",
        "deployment_revision": "sha256:" + "9" * 64,
        "retained_bundle_id": receipt["candidate_retained_bundle_id"],
        "graph_revision": receipt["candidate_graph_revision"],
        "program_id": "program-a",
        "program_revision": "sha256:" + "a" * 64,
        "program_binding_id": "sha256:" + "b" * 64,
        "node_environment_digest": "sha256:" + "c" * 64,
        "assistant_mode": "forbidden",
        "typed_parameter_overrides": {},
    }
    manager_requests: list[dict[str, Any]] = []

    class Headless:
        def manager_apply(self, body):
            manager_requests.append(deepcopy(body))
            return {
                "ok": True,
                "state": "pending",
                "apply_id": "apply.1",
                "apply_generation": 1,
                "candidate_projection_revision": {
                    "retained_bundle_id": receipt["candidate_retained_bundle_id"],
                    "graph_revision": receipt["candidate_graph_revision"],
                },
            }

    state.headless_client = Headless()  # type: ignore[assignment]
    parent_claim = {"intent": {"owner_role": "authoring_owner"}}
    result = state.execute_authored_deployment_candidate(
        {},
        parent_claim,
        apply=True,
        prepared={
            "prepared_activation": prepared_activation,
            "candidate_preview": {"candidate_receipt": receipt},
            "manager_apply": {
                "request_id": "request.1",
                "request_owner_epoch": "manager.epoch.1",
                "source_label": "manyforge_composer_deployment_apply",
                "correlation_id": "browser.1",
                "deployment_artifact_id": "cell-a",
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
    assert manager_requests[0]["prepared_activation"]["program_binding_id"] == (
        prepared_activation["program_binding_id"]
    )
    assert "expected_admission_generation" not in manager_requests[0]
    assert manager_requests[0]["parent_authoring_claim"] == parent_claim


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
    client, _state = _client(tmp_path)
    source = tmp_path / "cell_a.deployment.yaml"
    document = _focused_document()
    other_binding = deepcopy(document["graph"]["control_bindings"][0])
    other_binding["control_binding_id"] = "other_binding"
    document["graph"]["control_bindings"].append(other_binding)
    other_stage = deepcopy(document["policies"]["stop_stage_parameters"][0])
    other_stage["control_binding_id"] = "other_binding"
    document["policies"]["stop_stage_parameters"].append(other_stage)
    source.write_text(json.dumps(document), encoding="utf-8")
    initial = client.get("/api/deployment/workspace").json()
    opened = client.post(
        "/api/deployment/open-draft",
        json=_request(initial, path=str(source), confirmDiscard=False, kind="open"),
    ).json()
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
            timing={
                "command_latency_allowance_ns": 250_000_000,
                "settling_allowance_ns": 750_000_000,
                "operational_timeout_ns": 3_000_000_000,
            },
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
    saved = client.post(
        "/api/deployment/save-as",
        json=_request(
            workspace,
            path=str(tmp_path / "saved.deployment.yaml"),
            confirmOverwrite=False,
            kind="save-stop",
        ),
    )
    assert saved.status_code == 200, saved.text
    reopened = client.post(
        "/api/deployment/open-draft",
        json=_request(
            saved.json(),
            path=str(tmp_path / "saved.deployment.yaml"),
            confirmDiscard=False,
            kind="reopen-stop",
        ),
    )
    assert reopened.status_code == 200, reopened.text
    assert (
        reopened.json()["draft"]["document"]["policies"]
        == workspace["draft"]["document"]["policies"]
    )


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
        assert (
            client.get("/api/deployment/workspace").json()["draft"] == opened["draft"]
        )
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


def test_run_rejects_a_loaded_deployment_draft_that_is_not_manager_applied():
    state = ComposerState()
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
            "source_document_digest": _DIGEST,
            "expected_applied_record_digest": _DIGEST,
            "draft_program_revision": _DIGEST,
            "expected_admission_generation": 1,
        },
    )
    assert response.status_code == 409
    assert (
        response.json()["detail"]["code"]
        == "program_run_deployment_draft_applied_mismatch"
    )


def test_run_uses_the_existing_transport_when_deployment_draft_matches_applied():
    state = ComposerState()
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
            "source_document_digest": _DIGEST,
            "expected_applied_record_digest": _DIGEST,
            "draft_program_revision": _DIGEST,
            "expected_admission_generation": 1,
        },
    )
    assert response.status_code == 200
    assert response.json() == {"accepted_request_id": "run-1"}
