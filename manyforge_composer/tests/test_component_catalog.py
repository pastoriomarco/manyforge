"""Read-only Add component catalog over the curated example Scenarios."""

from __future__ import annotations

import json
from pathlib import Path

import pytest
from fastapi import FastAPI
from fastapi.testclient import TestClient

from manyforge_composer.backend import component_catalog as catalog_module
from manyforge_composer.backend.component_catalog import _kind, component_catalog
from manyforge_composer.backend.deployment_intent import (
    iter_source_references,
    resolve_source_reference,
    validate_deployment_intent,
)
from manyforge_composer.backend.routes_deployment_draft import (
    create_deployment_draft_router,
)
from manyforge_composer.backend.state import ComposerState

_REPOSITORY = Path(__file__).resolve().parents[2]


def _entry(catalog: dict, scenario: str, component_id: str) -> dict:
    """The entry offering ``component_id`` as written in one example Scenario."""

    return next(
        entry for entry in catalog["entries"]
        if entry["componentId"] == component_id
        and f"examples/{scenario}" in entry["sources"]
    )


def _intent(rows: list[dict]) -> dict:
    # A model row without a root pose is a mounted tool: give it a mount row.
    return {
        "api_version": "manyforge.deployment_intent.v2",
        "deployment_artifact_id": "catalog_check",
        "name": "catalog check",
        "components": rows,
        "attachments": [
            {
                "parent": {"component_id": "catalog_robot", "interface": "flange"},
                "child": {"component_id": row["component_id"], "interface": "mount"},
            }
            for row in rows
            if "model_source" in row and "root_pose_in_cell" not in row
        ],
        "policies": {},
    }


def test_catalog_offers_curated_rows_with_kinds_tags_and_assemblies() -> None:
    catalog = component_catalog(None)

    assert catalog["schemaVersion"] == "manyforge.composer_component_catalog.v1"
    kinds = {entry["kind"] for entry in catalog["entries"]}
    assert {"robot", "io_device", "pose_source", "ai_model"} <= kinds
    # Merge 6: the robot stands alone; the pneumatic tool names its I/O
    # device, so both are added together.
    robot = _entry(catalog, "lite6_pneumatic_mock.scenario", "lite6_robot")
    assert robot["kind"] == "robot"
    assert robot["componentProfileId"] == "lite6_mock_commanded_v0"
    assert robot["modelFamilyId"] == "lite6"
    assert "articulated-arm" in robot["tags"] and "gripper" not in robot["tags"]
    assert robot["lane"] == "manyforge-only"
    assert [row["component_id"] for row in robot["components"]] == ["lite6_robot"]
    tool = _entry(catalog, "lite6_pneumatic_mock.scenario", "lite_gripper")
    assert tool["kind"] == "tool"
    assert tool["componentProfileId"] == "lite_gripper_pneumatic_v0"
    assert tool["modelFamilyId"] == "lite_gripper"
    assert [row["component_id"] for row in tool["components"]] == [
        "gripper_io", "lite_gripper",
    ]
    # Type facts cover every profile with a component type, for the Draft's rows.
    assert catalog["componentProfiles"]["lite6_mock_commanded_v0"]["tags"] == robot["tags"]
    device = _entry(catalog, "lite6_pneumatic_mock.scenario", "gripper_io")
    assert device["kind"] == "io_device"
    assert [row["component_id"] for row in device["components"]] == ["gripper_io"]
    # Identical rows of several Scenarios are one entry listing every source.
    shared = _entry(catalog, "lite6_pick_and_place.scenario", "lite6_robot")
    assert {
        "examples/lite6_grasp_lost.scenario",
        "examples/ur5e_lite6_wall_workbench.scenario",
    } <= set(shared["sources"])
    keys = [(entry["componentId"], str(entry["components"])) for entry in catalog["entries"]]
    assert len(keys) == len(set(keys))
    # Every offered row set is a valid Deployment intent component list.
    for entry in catalog["entries"]:
        validate_deployment_intent(_intent(entry["components"]), source=entry["entryId"])


def test_profiles_without_a_curated_row_are_named_not_invented() -> None:
    catalog = component_catalog(None)

    offered = {entry["componentProfileId"] for entry in catalog["entries"]}
    assert not offered & set(catalog["profilesWithoutEntry"])


def test_model_sources_resolve_to_the_same_files_from_any_project(tmp_path: Path) -> None:
    project = tmp_path / "cell.scenario"
    project.mkdir()
    absolute = _entry(component_catalog(None), "lite6_pick_and_place.scenario", "lite6_robot")
    relative = _entry(
        component_catalog(str(project)), "lite6_pick_and_place.scenario", "lite6_robot"
    )

    absolute_paths = [
        reference for _path, reference, _directory
        in iter_source_references(_intent(absolute["components"]))
    ]
    relative_paths = [
        resolve_source_reference(reference, project_root=project)
        for _path, reference, _directory
        in iter_source_references(_intent(relative["components"]))
    ]
    assert all(set(reference) == {"external_path"} for reference in absolute_paths)
    assert [Path(reference["external_path"]) for reference in absolute_paths] == relative_paths
    assert all(path.exists() for path in relative_paths)
    assert all(str(path).startswith(str(_REPOSITORY)) for path in relative_paths)


def test_catalog_route_is_read_only_and_uses_the_draft_project(tmp_path: Path) -> None:
    state = ComposerState()
    app = FastAPI()
    app.include_router(create_deployment_draft_router(state))
    client = TestClient(app)
    before = state.deployment_draft_session.retained_state()

    response = client.get("/api/deployment/component-catalog")

    assert response.status_code == 200, response.text
    assert response.json()["projectRoot"] == state.deployment_source_project_path
    assert response.json()["entries"]
    assert state.deployment_draft_session.retained_state() == before


def test_tool_kind_and_mount_interfaces_come_from_the_component_type(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    types = tmp_path / "component_type"
    types.mkdir()
    (types / "arm.component.json").write_text(json.dumps({
        "tags": ["articulated-arm"],
        "attachment_interfaces": [{
            "attachment_interface_id": "flange",
            "model_role_id": "arm_model",
            "frame_role_id": "flange",
            "accepts_tags": ["tool"],
            "max_attachments": 1,
        }],
    }))
    (types / "hand.component.json").write_text(json.dumps({
        "tags": ["tool", "gripper"], "attachment_interfaces": [],
    }))
    sources = tmp_path / "sources.json"
    sources.write_text(json.dumps({"runtime_restoration_profiles": [
        {"runtime_restoration_profile_id": "arm_mock_v0", "model_family_id": "arm"},
        {"runtime_restoration_profile_id": "hand_v0", "model_family_id": "hand"},
        {"runtime_restoration_profile_id": "io_device_v0"},
    ]}))
    model = {"urdf": {"project_path": "model.urdf"}, "package_roots": {}}
    arm = {
        "component_id": "arm", "component_profile_id": "arm_mock_v0",
        "model_source": model, "target_settings": {},
        "root_pose_in_cell": {"position_m": [0, 0, 0], "orientation_xyzw": [0, 0, 0, 1]},
        "configuration": {"runtime_kind": "ros2_control"},
    }
    # The mounted tool has no root pose: its Scenario's mount row places it.
    hand = {
        "component_id": "hand", "component_profile_id": "hand_v0",
        "model_source": model, "target_settings": {},
        "configuration": {"runtime_kind": "ros2_control"},
    }
    scenario = tmp_path / "examples" / "cell.scenario"
    scenario.mkdir(parents=True)
    (scenario / "deployment.yaml").write_text(json.dumps({
        "api_version": "manyforge.deployment_intent.v2",
        "deployment_artifact_id": "cell", "name": "Cell",
        "components": [arm, hand],
        "attachments": [{
            "parent": {"component_id": "arm", "interface": "flange"},
            "child": {"component_id": "hand", "interface": "mount"},
        }],
        "policies": {},
    }))
    monkeypatch.setattr(catalog_module, "_SOURCES", sources)
    monkeypatch.setattr(catalog_module, "_COMPONENT_TYPES", types)
    monkeypatch.setattr(catalog_module, "_EXAMPLES", tmp_path / "examples")

    catalog = component_catalog(None)

    entries = {entry["componentId"]: entry for entry in catalog["entries"]}
    assert [(entry["componentId"], entry["kind"]) for entry in catalog["entries"]] == [
        ("arm", "robot"), ("hand", "tool"),
    ]
    # The tool row stays without a root pose; the catalog offers no attachment.
    assert "root_pose_in_cell" not in entries["hand"]["components"][0]
    assert "root_pose_in_cell" in entries["arm"]["components"][0]
    assert catalog["componentProfiles"] == {
        "arm_mock_v0": {
            "tags": ["articulated-arm"], "attachmentInterfaces": ["flange"], "tcpTargets": [], "frameTargets": [],
        },
        "hand_v0": {"tags": ["gripper", "tool"], "attachmentInterfaces": [], "tcpTargets": [], "frameTargets": []},
    }
    model_row = {"configuration": {"runtime_kind": "ros2_control"}}
    assert _kind(model_row, ["gripper", "tool"]) == "tool"
    assert _kind(model_row, ["articulated-arm"]) == "robot"
    assert _kind({"configuration": {"runtime_kind": "io_provider"}}, ["tool"]) == "io_device"


def test_a_tool_profile_carries_its_model_payload_read_only(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    types = tmp_path / "component_type"
    types.mkdir()
    for family, tags in (("hand", ["tool"]), ("plain_hand", ["tool"])):
        (types / f"{family}.component.json").write_text(json.dumps({
            "tags": tags, "attachment_interfaces": [],
        }))
    assets = tmp_path / "model_assets"
    assets.mkdir()
    # The model asset's own payload (kg; m in its root link); absent means 0.
    (assets / "hand.model_asset.json").write_text(json.dumps({
        "model_asset_id": "hand",
        "payload": {"mass_kg": 0.6, "center_of_mass_m": [0.0, 0.01, 0.05]},
    }))
    (assets / "plain_hand.model_asset.json").write_text(json.dumps({
        "model_asset_id": "plain_hand",
    }))
    sources = tmp_path / "sources.json"
    sources.write_text(json.dumps({
        "runtime_restoration_profiles": [
            {"runtime_restoration_profile_id": "hand_v0", "model_family_id": "hand"},
            {"runtime_restoration_profile_id": "plain_hand_v0", "model_family_id": "plain_hand"},
        ],
        "model_families": [
            {"model_family_id": family, "model_asset_id": family,
             "catalog_source": f"model_assets/{family}.model_asset.json"}
            for family in ("hand", "plain_hand")
        ],
    }))
    (tmp_path / "examples").mkdir()
    monkeypatch.setattr(catalog_module, "_SOURCES", sources)
    monkeypatch.setattr(catalog_module, "_COMPONENT_TYPES", types)
    monkeypatch.setattr(catalog_module, "_EXAMPLES", tmp_path / "examples")
    monkeypatch.setattr(catalog_module, "_REPOSITORY", tmp_path)

    profiles = component_catalog(None)["componentProfiles"]

    assert profiles["hand_v0"]["payload"] == {
        "massKg": 0.6, "centerOfMassM": [0.0, 0.01, 0.05],
    }
    assert "payload" not in profiles["plain_hand_v0"]


def test_profile_facts_carry_each_targets_tcp_roles_and_default(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    def target(target_id: str, tcps: list[str], default: str | None) -> dict:
        row = {
            "command_target_id": target_id,
            "frame_roles": [{"frame_role_id": "base"}] + [
                {"frame_role_id": role, "tcp": True} for role in tcps
            ],
        }
        if default is not None:
            row["default_tcp_frame_role_id"] = default
        return row

    types = tmp_path / "component_type"
    types.mkdir()
    # A two-target tool with a `tcp` on both targets (B5: its Deployment row
    # chooses the default), a one-target tool and a bracket without TCPs.
    for family, targets in (
        ("dual", [target("tool_b", ["tcp"], "tcp"), target("tool_a", ["tcp", "edge"], "tcp")]),
        ("single", [target("gripper", ["tcp"], "tcp")]),
        ("bracket", [target("mount", [], None)]),
    ):
        (types / f"{family}.component.json").write_text(json.dumps({
            "tags": ["tool"], "attachment_interfaces": [], "command_targets": targets,
        }))
    sources = tmp_path / "sources.json"
    sources.write_text(json.dumps({"runtime_restoration_profiles": [
        {"runtime_restoration_profile_id": f"{family}_v0", "model_family_id": family}
        for family in ("dual", "single", "bracket")
    ]}))
    (tmp_path / "examples").mkdir()
    monkeypatch.setattr(catalog_module, "_SOURCES", sources)
    monkeypatch.setattr(catalog_module, "_COMPONENT_TYPES", types)
    monkeypatch.setattr(catalog_module, "_EXAMPLES", tmp_path / "examples")

    profiles = component_catalog(None)["componentProfiles"]

    assert profiles["dual_v0"]["tcpTargets"] == [
        {"commandTargetId": "tool_a", "tcpFrameRoleIds": ["edge", "tcp"], "defaultTcpFrameRoleId": "tcp"},
        {"commandTargetId": "tool_b", "tcpFrameRoleIds": ["tcp"], "defaultTcpFrameRoleId": "tcp"},
    ]
    assert profiles["single_v0"]["tcpTargets"] == [
        {"commandTargetId": "gripper", "tcpFrameRoleIds": ["tcp"], "defaultTcpFrameRoleId": "tcp"},
    ]
    assert profiles["bracket_v0"]["tcpTargets"] == []
    assert profiles["bracket_v0"]["frameTargets"] == [
        {"commandTargetId": "mount", "frameRoleIds": ["base"]},
    ]


def test_maintained_profiles_declare_one_tcp_target_each() -> None:
    profiles = component_catalog(None)["componentProfiles"]

    assert profiles["lite6_mock_commanded_v0"]["tcpTargets"] == [
        {"commandTargetId": "arm", "tcpFrameRoleIds": ["flange"], "defaultTcpFrameRoleId": "flange"},
    ]
    assert profiles["lite_gripper_mock_commanded_v0"]["tcpTargets"] == [
        {"commandTargetId": "gripper", "tcpFrameRoleIds": ["tcp"], "defaultTcpFrameRoleId": "tcp"},
    ]
    # No maintained profile needs a Deployment `default_tcp` (B5 sweep).
    assert all(len(facts["tcpTargets"]) <= 1 for facts in profiles.values())
