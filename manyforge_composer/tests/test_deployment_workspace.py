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
    AuthoringAdmissionCoordinator,
    AuthoringOperationError,
    LocalAuthoringCommit,
)
from manyforge_composer.backend.authoring_workspace import AuthoringWorkspace
from manyforge_composer.backend.routes_headless import create_headless_router
from manyforge_composer.backend.state import (
    ComposerState,
    _controllable_composition_change_kinds,
)
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
    # Ordinary-motion jerk is a native-runtime field; the maintained offline
    # examples moved to the mock ROS lane (Q63-Q66), so the native import path
    # uses the frozen pre-cutover native Lite 6 deployment until the native
    # executor goes.
    document = json.loads(
        (
            Path(__file__).resolve().parents[2]
            / "manyforge_deployment/test/fixtures/native_execution/"
            "lite6_lite_gripper_pick_and_place.native.deployment.yaml"
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
    "enabled,machine_required,two_robots,target_index",
    [
        (False, False, False, 0),
        (True, False, False, 0),
        (True, True, False, 0),
        (True, False, True, 0),
        (True, False, True, 1),
    ],
)
def test_ordinary_jerk_source_roundtrip_preserves_choice_and_provenance(
    tmp_path, enabled, machine_required, two_robots, target_index
):
    source = json.loads(
        (
            Path(__file__).resolve().parents[2]
            # Ordinary-motion jerk is a native-runtime field; the maintained
            # offline examples moved to the mock ROS lane (Q63-Q66), so this
            # native authoring path uses the frozen pre-cutover native Lite 6
            # deployment until the native executor goes.
            / "manyforge_deployment/test/fixtures/native_execution/"
            "lite6_lite_gripper_pick_and_place.native.deployment.yaml"
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
        # Both components share the stable native cell host. Duplicate their
        # addressed configuration rows inside that host, as the compiler does.
        configuration = source["graph"]["runtime_units"][0]["configuration"]
        for name in (
            "motion_target_limits",
            "direct_control_joint_limits",
            "gripper_target_configurations",
        ):
            rows = configuration[name]
            rows.extend(
                json.loads(json.dumps(row).replace("lite6_robot", "peer_robot"))
                for row in list(rows)
            )
        configuration["initial_joint_positions_rad"] *= 2
    native_units = source["graph"]["runtime_units"]
    assert len(native_units) == 1
    assert native_units[0]["runtime_unit_id"] == "robot_cell.native_execution"
    configuration = native_units[0]["configuration"]
    targets = configuration["motion_target_limits"]
    assert [row["controllable_component_id"] for row in targets] == (
        ["lite6_robot", "peer_robot"] if two_robots else ["lite6_robot"]
    )
    target = targets[target_index]
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
    # Exercise an ordinary runtime-configuration Draft edit on an existing
    # component composition, without using Draft-only Open to add the robots.
    original = tmp_path / "original.deployment.yaml"
    original.write_text(json.dumps(source))
    _seed_existing_deployment(
        state, source, saved_path=str(original), compiler=compile_deployment_document
    )
    workspace = client.get("/api/deployment/workspace").json()
    changed = client.post(
        "/api/deployment/draft/mutations",
        json=_request(
            workspace,
            path=[
                "graph",
                "runtime_units",
                0,
                "configuration",
                "motion_target_limits",
                target_index,
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
            0,
            "configuration",
            "motion_target_limits",
            target_index,
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
        reopened.document["graph"]["runtime_units"][0]["configuration"][
            "motion_target_limits"
        ][target_index]["ordinary_motion_jerk"]
        == settings
    )
    # Off retains the numbers but does not activate them; the independent
    # controlled-Stop choices are unchanged in either case.
    assert reopened.document["policies"] == source["policies"]
    expected_native_unit = deepcopy(native_units[0])
    expected_native_unit["configuration"]["motion_target_limits"][target_index][
        "ordinary_motion_jerk"
    ] = settings
    # The selected target changes; every peer and host configuration field
    # survives the Save/Open round trip exactly.
    assert reopened.document["graph"]["runtime_units"] == [expected_native_unit]
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


def test_explicit_max_stop_time_is_profile_owned_and_not_refused_at_apply():
    # Q51/item 24 (as `main`): the robot's own `max_stop_time_s` is part of its
    # controlled-Stop profile; a value shorter than the deceleration is not an
    # Apply-time refusal, and the cell-wide timing table is gone.
    document = json.loads(
        (
            Path(__file__).resolve().parents[2]
            / "examples/lite6_lite_gripper_pick_and_place.deployment.yaml"
        ).read_text()
    )
    assert "stop_timing_parameters" not in document["policies"]
    binding = next(
        row
        for row in document["graph"]["control_bindings"]
        if row["capability_id"] == "execution.trajectory.execute"
    )
    rows = [
        row
        for row in document["policies"]["stop_stage_parameters"]
        if row["control_binding_id"] == binding["control_binding_id"]
        and row["stop_escalation_stage_id"] == "motion_controlled_stop"
    ]
    assert rows
    for row in rows:
        assert "max_stop_time_s" not in row["default_contract_parameters"]
        row["default_contract_parameters"]["deceleration_time_s"] = 1.1
        row["default_contract_parameters"]["max_stop_time_s"] = 0.5
    compiled = compile_deployment_document(json.dumps(document))
    for row in compiled.document["policies"]["stop_stage_parameters"]:
        if (
            row["control_binding_id"] == binding["control_binding_id"]
            and row["stop_escalation_stage_id"] == "motion_controlled_stop"
        ):
            assert row["default_contract_parameters"]["max_stop_time_s"] == 0.5
    document["policies"]["stop_timing_parameters"] = []
    with pytest.raises(DeploymentDraftError):
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
            "runtime_unit_id": "robot.native_execution",
            "runtime_profile_id": "manyforge.native_execution_runtime",
            "runtime_profile_revision": _DIGEST,
            "dependency_runtime_unit_ids": [],
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


@pytest.mark.parametrize("route", ["save", "save-as"])
def test_refused_deployment_save_writes_no_artifact(
    tmp_path: Path, route: str
) -> None:
    """Admission precedes the artifact write, so its refusal has no effect."""

    class Refusing:
        def authoring_admission(self) -> dict[str, Any]:
            return {"authoring_allowed": False, "admission_generation": 1}

        def acquire_authoring_transaction(self, _request):
            raise AssertionError("a refused Save takes no claim")

    client, state = _client(tmp_path)
    source = tmp_path / "cell_a.deployment.yaml"
    source.write_text(json.dumps(_document()), encoding="utf-8")
    workspace = client.get("/api/deployment/workspace").json()
    workspace = client.post(
        "/api/deployment/open-draft",
        json=_request(workspace, path=str(source), confirmDiscard=False, kind="open"),
    ).json()
    workspace = client.post(
        "/api/deployment/draft/mutations",
        json=_request(workspace, path=["name"], value="Edited cell"),
    ).json()
    source_bytes = source.read_bytes()
    state.authoring_admission = AuthoringAdmissionCoordinator(Refusing())  # type: ignore[arg-type]

    refused = client.post(
        f"/api/deployment/{route}",
        json=(
            _request(workspace, kind="save")
            if route == "save"
            else _request(
                workspace,
                path=str(tmp_path / "saved"),
                confirmOverwrite=False,
                kind="save",
            )
        ),
    )

    assert refused.status_code == 409, refused.text
    assert refused.json()["detail"]["code"] == "authoring_not_ready"
    assert refused.json()["detail"]["effect"] == "none"
    assert source.read_bytes() == source_bytes
    assert not (tmp_path / "saved.deployment.yaml").exists()
    assert client.get("/api/deployment/workspace").json() == workspace


@pytest.mark.parametrize("stale_at", ["before_write", "after_write"])
def test_deployment_save_draft_refusal_is_marked_only_before_the_write(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, stale_at: str
) -> None:
    client, state = _client(tmp_path)
    source = tmp_path / "cell_a.deployment.yaml"
    source.write_text(json.dumps(_document()), encoding="utf-8")
    workspace = client.get("/api/deployment/workspace").json()
    workspace = client.post(
        "/api/deployment/open-draft",
        json=_request(workspace, path=str(source), confirmDiscard=False, kind="open"),
    ).json()

    def stale(*_args, **_kwargs):
        raise DeploymentDraftError(
            "deployment_draft_stale", "Deployment Draft changed", status_code=409
        )

    monkeypatch.setattr(
        state.deployment_draft_session,
        "require_current" if stale_at == "before_write" else "install_prepared_save",
        stale,
    )
    target = tmp_path / "saved.deployment.yaml"
    refused = client.post(
        "/api/deployment/save-as",
        json=_request(
            workspace, path=str(tmp_path / "saved"), confirmOverwrite=False, kind="save"
        ),
    )

    detail = refused.json()["detail"]
    if stale_at == "before_write":
        # Nothing was written: the operation recorded no effect.
        assert refused.status_code == 409, refused.text
        assert detail["code"] == "deployment_draft_stale"
        assert detail["effect"] == "none"
        assert not target.exists()
    else:
        # Written, then the workspace owner contained the failed adoption.
        assert refused.status_code == 409, refused.text
        assert detail["code"] == "deployment_saved_baseline_not_adopted"
        assert detail["causeCode"] == "composer_workspace_install_failed"
        assert detail["path"] == str(target)
        assert "effect" not in detail
        assert target.exists()


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
    publication = {
        "scenario_id": "cell_a",
        "source_document_digest": _DIGEST,
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
            "program_role_assignment": {},
            "source_document_digest": _DIGEST,
            "scenario_id": "cell_a",
        },
        publication,
    )
    state._retain_submitted_deployment(captured)
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
    assert response.json()["draft"]["document"]["name"] == applied["name"]
    assert (
        state.authoring_retained_state()["schema_version"]
        == "manyforge.composer_authoring_workspace.v2"
    )


def test_opening_new_controllable_composition_requires_activation_without_mutation(
    tmp_path: Path,
) -> None:
    client, state = _client(tmp_path)
    source = _document()
    source["graph"]["controllable_components"] = [
        {"controllable_component_id": "robot"}
    ]
    artifact = tmp_path / "new-composition.deployment.yaml"
    artifact.write_text(json.dumps(source), encoding="utf-8")
    before = client.get("/api/deployment/workspace").json()

    rejected = client.post(
        "/api/deployment/open-draft",
        json=_request(
            before,
            path=str(artifact),
            confirmDiscard=False,
            kind="open-controllable-composition",
        ),
    )

    assert rejected.status_code == 409
    assert rejected.json()["detail"]["code"] == "deployment_activation_required"
    assert rejected.json()["detail"]["activationRequired"] is True
    assert rejected.json()["detail"]["changedGraphKinds"] == [
        "controllable_components"
    ]
    after = client.get("/api/deployment/workspace").json()
    assert after["draft"] == before["draft"]
    assert after["authoringWorkspace"] == before["authoringWorkspace"]
    assert state.deployment_draft_session.retained_state()["document"] is None


def test_generic_mutation_rejects_controllable_graph_change_without_mutation(
    tmp_path: Path,
) -> None:
    client, _state = _client(tmp_path)
    source = tmp_path / "empty-composition.deployment.yaml"
    source.write_text(json.dumps(_document()), encoding="utf-8")
    initial = client.get("/api/deployment/workspace").json()
    opened = client.post(
        "/api/deployment/open-draft",
        json=_request(initial, path=str(source), confirmDiscard=False, kind="open"),
    ).json()

    rejected = client.post(
        "/api/deployment/draft/mutations",
        json=_request(
            opened,
            path=["graph", "control_bindings"],
            value=[{"control_binding_id": "robot.arm"}],
            kind="add-control-binding",
        ),
    )

    assert rejected.status_code == 409
    assert rejected.json()["detail"]["code"] == "deployment_activation_required"
    assert rejected.json()["detail"]["activationRequired"] is True
    assert "control_bindings" in rejected.json()["detail"]["changedGraphKinds"]
    after = client.get("/api/deployment/workspace").json()
    assert after["draft"] == opened["draft"]
    assert after["authoringWorkspace"]["workspaceEpoch"] == opened[
        "authoringWorkspace"
    ]["workspaceEpoch"]
    assert after["authoringWorkspace"]["workspaceRevision"] == opened[
        "authoringWorkspace"
    ]["workspaceRevision"]


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


def test_draft_composition_classifier_uses_model_identity_not_placement_fields() -> None:
    intent_before = {
        "api_version": "manyforge.deployment_intent.v2",
        "components": [
            {
                "component_id": "robot",
                "component_profile_id": "robot_profile",
                "model_source": {"urdf": "robot.urdf"},
                "root_pose_in_cell": {"position_m": [0.0, 0.0, 0.0]},
                "target_settings": {},
                "configuration": {},
            }
        ],
        "attachments": [],
    }
    intent_after = deepcopy(intent_before)
    intent_after["components"][0]["root_pose_in_cell"] = {
        "position_m": [1.0, 0.0, 0.0]
    }
    assert _controllable_composition_change_kinds(intent_before, intent_after) == ()

    before = _document()
    before["graph"]["model_instances"] = [
        {
            "model_instance_id": "robot.model",
            "model_asset_id": "robot_model",
            "model_revision": _DIGEST,
            "root_pose_in_cell": {"position_m": [0.0, 0.0, 0.0]},
        }
    ]
    after = deepcopy(before)
    after["graph"]["model_instances"][0]["root_pose_in_cell"] = {
        "position_m": [2.0, 0.0, 0.0]
    }

    assert _controllable_composition_change_kinds(before, after) == ()

    after["graph"]["model_instances"][0]["model_asset_id"] = "other_model"
    assert _controllable_composition_change_kinds(before, after) == (
        "model_instances",
    )


def test_controllable_add_remove_and_replacement_are_activation_only() -> None:
    def component(identity: str, model: str = "robot.urdf") -> dict[str, Any]:
        return {
            "component_id": identity,
            "component_profile_id": "native_robot",
            "model_source": {"urdf": model},
            "target_settings": {},
            "configuration": {},
        }

    def intent(*components: dict[str, Any]) -> dict[str, Any]:
        return {
            "api_version": "manyforge.deployment_intent.v2",
            "components": list(components),
            "attachments": [],
        }

    empty = intent()
    one = intent(component("robot-a"))
    two = intent(component("robot-a"), component("robot-b"))
    replacement = intent(component("robot-a", "replacement.urdf"))
    for before, after, expected in (
        (empty, one, "controllable_components"),
        (one, two, "controllable_components"),
        (two, one, "controllable_components"),
        (one, replacement, "model_instances"),
    ):
        assert expected in _controllable_composition_change_kinds(before, after)


def test_draft_composition_classifier_fails_closed_on_mixed_or_unknown_schema() -> None:
    compiled = _document()
    unknown = deepcopy(compiled)
    unknown["api_version"] = "manyforge.deployment.v2"
    intent = {
        "api_version": "manyforge.deployment_intent.v2",
        "components": [],
        "attachments": [],
    }

    assert _controllable_composition_change_kinds(compiled, unknown) is None
    assert _controllable_composition_change_kinds(compiled, intent) is None


def test_opaque_runtime_or_controller_configuration_is_not_draft_classified() -> None:
    intent = {
        "api_version": "manyforge.deployment_intent.v2",
        "components": [
            {
                "component_id": "robot",
                "component_profile_id": "robot_profile",
                "model_source": {"urdf": "robot.urdf"},
                "target_settings": {},
                "configuration": {"controller_endpoint": "controller-a"},
            }
        ],
        "attachments": [],
    }
    changed_intent = deepcopy(intent)
    changed_intent["components"][0]["configuration"]["controller_endpoint"] = (
        "controller-b"
    )
    assert _controllable_composition_change_kinds(intent, changed_intent) is None

    compiled = _focused_document()
    changed_compiled = deepcopy(compiled)
    changed_compiled["graph"]["runtime_units"][0]["configuration"][
        "controller_endpoint"
    ] = "controller-b"
    assert _controllable_composition_change_kinds(compiled, changed_compiled) is None


def test_unclassifiable_deployment_shape_rejects_without_mutation(
    tmp_path: Path,
) -> None:
    client, _state = _client(tmp_path)
    source = tmp_path / "classifiable.deployment.yaml"
    source.write_text(json.dumps(_document()), encoding="utf-8")
    initial = client.get("/api/deployment/workspace").json()
    opened = client.post(
        "/api/deployment/open-draft",
        json=_request(initial, path=str(source), confirmDiscard=False, kind="open"),
    ).json()

    rejected = client.post(
        "/api/deployment/draft/mutations",
        json=_request(
            opened,
            path=["graph", "model_instances"],
            value="not-an-array",
            kind="unknown-graph-shape",
        ),
    )

    assert rejected.status_code == 409
    assert (
        rejected.json()["detail"]["code"]
        == "deployment_activation_classification_unavailable"
    )
    assert rejected.json()["detail"]["activationRequired"] is True
    assert rejected.json()["detail"]["classificationAvailable"] is False
    after = client.get("/api/deployment/workspace").json()
    assert after["draft"] == opened["draft"]
    assert after["authoringWorkspace"]["workspaceEpoch"] == opened[
        "authoringWorkspace"
    ]["workspaceEpoch"]
    assert after["authoringWorkspace"]["workspaceRevision"] == opened[
        "authoringWorkspace"
    ]["workspaceRevision"]


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
        document={
            "scenario_id": "lite6_pick_and_place",
            "program_role_assignment": json.loads(
                (project / "assignment.yaml").read_text(encoding="utf-8")
            ),
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
            "schema_version": "manyforge.prepared_activation_input.v5",
            "prepared_activation_id": "sha256:" + "9" * 64,
            "source_document_digest": intent["source_document_digest"],
            "captured_input_digest": intent["captured_input_digest"],
            "deployment_artifact_id": edited["draft"]["deploymentArtifactId"],
            "deployment_revision": candidate_revision,
            "retained_bundle_id": "sha256:" + "a" * 64,
            "graph_revision": "sha256:" + "7" * 64,
            "program_id": program["program_id"],
            "program_revision": program["program_revision"],
            "program_outcome": {"kind": "bound", "program_binding_id": "sha256:" + "b" * 64},
            "node_environment_digest": "sha256:" + "d" * 64,
            "assistant_mode": "forbidden",
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
            prepared = {
                "result": result,
                "confirmation_required": confirmation_required,
            }
        if prepare_only:
            assert _claim is None, "preparation must precede admission acquisition"
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
    confirmation = client.post(
        "/api/deployment/draft/apply",
        json=_request(edited, kind="apply", local=False),
    )
    assert confirmation.status_code == 202, confirmation.text
    assert confirmation.json()["result"]["status"] == "confirmation_required"
    assert confirmation.json()["result"]["confirmation_token"] == confirmation_token
    state.authoring_admission = coordinator
    assert current["deployment_revision"] == base_revision
    assert effects[-1][0]["program"] == program
    assert effects[-1][0]["deployment"] == edited["draft"]["document"]
    capture_calls_after_preparation = capture_calls

    # The confirmation covers the complete activation input: the Program
    # Draft, assignment and source the Deployment compiles with too. Any of
    # them changed since is a named pre-effect refusal.
    program_snapshot = state.draft_session.snapshot()
    scope = {
        "deploymentDraftId": edited["draft"]["id"],
        "deploymentDraftRevision": edited["draft"]["revision"],
        "deploymentRevision": edited["draft"]["deploymentRevision"],
        "programDraftId": program_snapshot["draft_id"],
        "programDraftRevision": program_snapshot["draft_revision"],
        "programRevision": program_snapshot["program_revision"],
        "sourceDocumentDigest": "sha256:" + "6" * 64,
        "programRoleAssignment": state.scenario_program_role_assignment,
    }
    effects_before_scope = len(effects)
    for index, changed in enumerate(
        (
            {"programDraftRevision": program_snapshot["draft_revision"] + 1},
            {"programRevision": "sha256:" + "0" * 64},
            {"sourceDocumentDigest": "sha256:" + "5" * 64},
            {"programRoleAssignment": {"component_roles": [], "target_roles": []}},
            {"deploymentDraftRevision": edited["draft"]["revision"] + 1},
        )
    ):
        refused = client.post(
            "/api/deployment/draft/apply",
            json=_request(
                edited,
                kind=f"apply-scope-changed-{index}",
                confirmationToken=confirmation_token,
                preparationRequestOwnerEpoch=confirmation.json()["result"][
                    "preparation"
                ]["request_owner_epoch"],
                preparationRequestId=confirmation.json()["result"]["preparation"][
                    "request_id"
                ],
                expectedActivationScope={**scope, **changed},
            ),
        )
        assert refused.status_code == 409, refused.text
        assert refused.json()["detail"] == {
            "code": "activation_input_changed_after_confirmation",
            "message": "Draft changed after confirmation; review again",
            "effect": "none",
        }
    assert len(effects) == effects_before_scope
    assert capture_calls == capture_calls_after_preparation

    applied = client.post(
        "/api/deployment/draft/apply",
        json=_request(
            edited,
            kind="apply-confirmed",
            expectedActivationScope=scope,
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

    # Nothing to apply: the Manager's Applied activation already is every
    # input (Deployment, sources, assignment, Scenario) and the Applied
    # Program is the Program Draft. Refused before admission or preparation.
    applied_context = {
        "deployment": deepcopy(edited["draft"]["document"]),
        "model_source_closure": capture_sources(
            state, edited["draft"]["document"]
        ),
        "program_role_assignment": deepcopy(state.scenario_program_role_assignment),
        "source_document_digest": "sha256:" + "6" * 64,
        "scenario_id": "lite6_pick_and_place",
    }
    monkeypatch.setattr(
        state, "published_scenario", lambda: {"published_scenario": {"ready": True}}
    )
    monkeypatch.setattr(
        state, "applied_activation_for_publication", lambda _p: deepcopy(applied_context)
    )
    applied_program = {"program_revision": program["program_revision"]}
    CandidateHeadless.program_applied = lambda _self: applied_program  # type: ignore[attr-defined]
    effects_before_no_op = len(effects)
    no_op = client.post(
        "/api/deployment/draft/apply",
        json=_request(edited, kind="apply-no-op", local=False),
    )
    assert no_op.status_code == 409, no_op.text
    assert no_op.json()["detail"]["code"] == "deployment_apply_nothing_to_apply"
    assert no_op.json()["detail"]["message"].startswith("Nothing to apply")
    assert len(effects) == effects_before_no_op
    # A Program Draft that differs from Applied is something to apply.
    applied_program["program_revision"] = "sha256:" + "0" * 64
    changed = client.post(
        "/api/deployment/draft/apply",
        json=_request(edited, kind="apply-program-differs", local=False),
    )
    assert changed.status_code == 202, changed.text


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
        "schema_version": "manyforge.prepared_activation_input.v5",
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
        "program_outcome": {"kind": "bound", "program_binding_id": "sha256:" + "b" * 64},
        "node_environment_digest": "sha256:" + "c" * 64,
        "assistant_mode": "forbidden",
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
        {
            "deployment": {"api_version": "manyforge.deployment_intent.v2"},
            "model_source_closure": {},
            "program_role_assignment": {},
            "source_document_digest": prepared_activation["source_document_digest"],
            "scenario_id": prepared_activation["scenario_id"],
        },
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
    assert manager_requests[0]["prepared_activation"]["program_outcome"] == (
        prepared_activation["program_outcome"]
    )
    assert "expected_admission_generation" not in manager_requests[0]
    assert manager_requests[0]["parent_authoring_claim"] == parent_claim


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


def test_save_as_path_rebase_matches_saved_scenario_but_real_edits_do_not() -> None:
    from manyforge_composer.backend.authoring_workspace import scenario_project_facts
    from manyforge_composer.backend.deployment_intent import (
        rebase_deployment_intent_sources,
    )

    state = ComposerState()
    source_root = Path("/workspace/source/cell")
    destination_root = Path("/workspace/copies/deep/cell")
    deployment = {
        "api_version": "manyforge.deployment_intent.v2",
        "deployment_artifact_id": "fixture_deployment",
        "name": "Fixture deployment",
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
    edited["name"] = "An actual edit"
    changed = state.deployment_draft_session.preview_source(json.dumps(edited))
    state.deployment_draft_session.commit_compiled(
        changed,
        draft_id=draft["draft_id"],
        expected_revision=draft["draft_revision"],
    )
    assert not state.current_drafts_match_remembered_scenario()


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

    state.authoring_admission = AuthoringAdmissionCoordinator(None)  # type: ignore[arg-type]
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
        == "execution_deployment_draft_unapplied"
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
            return {"accepted_request_id": body["request_id"]}

    state.authoring_admission = AuthoringAdmissionCoordinator(None)  # type: ignore[arg-type]
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


def test_run_rejects_unapplied_program_draft_before_transport() -> None:
    state = ComposerState()
    state.draft_session.snapshot = lambda: {"program_revision": _DIGEST}  # type: ignore[method-assign]
    state.published_scenario = lambda: {  # type: ignore[method-assign]
        "ready": True,
        "published_scenario": {"program_revision": "sha256:" + "2" * 64},
    }

    class Headless:
        def program_run_start(self, _body):
            raise AssertionError("Run must be rejected before transport")

    state.authoring_admission = AuthoringAdmissionCoordinator(None)  # type: ignore[arg-type]
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
    assert response.json()["detail"]["code"] == "execution_program_draft_unapplied"


@pytest.mark.parametrize("command_kind", ["program_motion_start", "operation_start",
                                         "direct_http_operation", "direct_http_refused"])
def test_only_program_start_rejects_unapplied_deployment_before_transport(command_kind: str) -> None:
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
    state.published_scenario = lambda: {  # type: ignore[method-assign]
        "ready": True,
        "published_scenario": {"deployment_revision": compiled.deployment_revision},
    }
    state.applied_deployment_for_publication = lambda _current: {  # type: ignore[method-assign]
        "different": "deployment"
    }

    class Headless:
        def submit_control_command(self, _body):
            assert command_kind != "program_motion_start"
            return {"accepted": True}

        def execute(self, _body):
            if command_kind == "direct_http_refused":
                from manyforge_headless_api.client import HeadlessClientError
                raise HeadlessClientError("authoring_not_ready", "activation holds admission", status_code=409)
            return {"accepted": True}

    state.authoring_admission = AuthoringAdmissionCoordinator(None)  # type: ignore[arg-type]
    app = FastAPI()
    app.include_router(create_headless_router(Headless(), state))  # type: ignore[arg-type]
    if command_kind.startswith("direct_http_"):
        body = {
            "request_context": {"request_id": "manual", "request_owner_epoch": "owner",
                                "source_label": "test", "correlation_id": None},
            "program_id": "manual", "program_revision": _DIGEST,
            "program_binding_id": _DIGEST, "program_run_id": "manual-run",
            "expected_graph_revision": _DIGEST, "expected_retained_bundle_id": _DIGEST,
            "expected_scene_revision": {"scene_id": _DIGEST, "revision_id": 1, "content_hash": _DIGEST},
            "participants": [{"component_role_id": "robot", "target_role_id": "arm",
                              "required_capability_id": "execution.trajectory.execute", "action_span": None,
                              "payload": {
                                  "kind": "motion", "move_id": "manual-move",
                                  "base_frame_role_id": "base", "tcp_frame_role_id": "tcp",
                                  "tolerances": {"joint_position_tol_rad": 0.05, "translation_tol_m": 0.005,
                                                 "rotation_tol_rad": 0.05},
                                  "speed_profile": {"velocity_scale": 1.0, "acceleration_scale": 1.0,
                                                    "max_tcp_linear_speed_mps": None},
                                  "timeouts": {"planning_timeout_s": 2.0, "execution_timeout_s": 10.0},
                                  "cache_mode": "default", "planning_profile": "balanced",
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
                "schema_version": "manyforge.execution_command.v1",
                "command_kind": command_kind,
                "correlation_id": "manual-start-1",
                "source_label": "test",
                "control_session_id": None,
                "client_sequence": None,
                "freshness": {
                    "context_id": "freshness-1",
                    "receiver_incarnation": "headless-1",
                    "semantic_owner_epoch": "program-1",
                    "execution_control_revision": 1,
                    "issued_server_sequence": "1",
                    "valid_for_ns": 750_000_000,
                },
                "expected_semantic_owner_epoch": "program-1",
                "expected_execution_control_revision": 1,
            },
            "payload": {},
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
    state = ComposerState()
    applied = {"program_revision": "sha256:" + "2" * 64}
    state.draft_session.snapshot = lambda: {  # type: ignore[method-assign]
        "program_loaded": True, "program_revision": _DIGEST,
    }
    state.published_scenario = lambda: {  # type: ignore[method-assign]
        "ready": True, "published_scenario": applied,
    }
    submitted: list[str] = []

    class Headless:
        def submit_control_command(self, body):
            submitted.append(body["payload"]["action_id"])
            return {"accepted": True}

        def cycle_recovery_action(self, body):
            submitted.append(body["action_id"])
            return {"accepted": True}

    state.authoring_admission = AuthoringAdmissionCoordinator(None)  # type: ignore[arg-type]
    app = FastAPI()
    app.include_router(create_headless_router(Headless(), state))  # type: ignore[arg-type]
    client = TestClient(app)

    def recover(action_id: str):
        payload = {
            "request_id": f"recovery-{action_id}",
            "request_owner_epoch": "browser-1",
            "source_label": "test",
            "correlation_id": None,
            "expected_owner_epoch": "program-1",
            "offer_id": _DIGEST,
            "offer_revision": 1,
            "action_id": action_id,
            "reason": "operator-selected recovery action",
        }
        if route == "recovery_action":
            return client.post("/api/cycle/recovery-actions", json=payload)
        return client.post("/api/control/commands", json={
            "command_envelope": {
                "schema_version": "manyforge.execution_command.v1",
                "command_kind": "execution_recovery",
                "correlation_id": f"recovery-{action_id}",
                "source_label": "test",
                "control_session_id": None,
                "client_sequence": None,
                "freshness": {
                    "context_id": "freshness-1",
                    "receiver_incarnation": "headless-1",
                    "semantic_owner_epoch": "program-1",
                    "execution_control_revision": 1,
                    "issued_server_sequence": "1",
                    "valid_for_ns": 750_000_000,
                },
                "expected_semantic_owner_epoch": "program-1",
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
