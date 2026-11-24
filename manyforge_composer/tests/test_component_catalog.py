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
        entry for entry in _row_entries(catalog)
        if entry["componentId"] == component_id
        and f"examples/{scenario}" in entry["sources"]
    )


def _row_entries(catalog: dict) -> list[dict]:
    """The example-row entries (machine entries have their own snake_case shape)."""

    return [entry for entry in catalog["entries"] if entry["kind"] != "machine"]


def _intent(rows: list[dict]) -> dict:
    # A model row without a root pose is a mounted tool: give it a mount row.
    return {
        "api_version": "manyforge.deployment_intent.v3",
        "collision_rules": [{"rule_id": "everything", "clearance_m": 0.001}],
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


def _models(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, families: dict[str, dict]
) -> None:
    """Maintained families, each naming its model descriptor (JSON is YAML).

    ``families``: family id -> ``{"id": model id, "lanes": {...}, "payload": ...}``.
    """

    rows = []
    for family, spec in families.items():
        (tmp_path / f"{family}.component.yaml").write_text(
            json.dumps({"id": spec["id"], "lanes": spec.get("lanes", {})})
        )
        asset = {"model_asset_id": family}
        if "payload" in spec:
            asset["payload"] = spec["payload"]
        (tmp_path / f"{family}.model_asset.json").write_text(json.dumps(asset))
        rows.append({
            "model_family_id": family, "model_asset_id": family,
            "catalog_source": f"{family}.model_asset.json",
            "component_descriptor": f"{family}.component.yaml",
        })
    sources = tmp_path / "sources.json"
    sources.write_text(json.dumps({"model_families": rows}))
    monkeypatch.setattr(catalog_module, "_SOURCES", sources)
    monkeypatch.setattr(catalog_module, "_REPOSITORY", tmp_path)


def test_catalog_offers_curated_rows_with_kinds_tags_and_assemblies() -> None:
    catalog = component_catalog(None)

    assert catalog["schemaVersion"] == "manyforge.composer_component_catalog.v1"
    kinds = {entry["kind"] for entry in catalog["entries"]}
    assert {"robot", "io_device", "pose_source", "ai_model"} <= kinds
    # Merge 6: the robot stands alone; the pneumatic tool names its I/O
    # device, so both are added together.
    robot = _entry(catalog, "mock_ur10e_pick_and_place.scenario", "ur10e_robot")
    assert robot["kind"] == "robot"
    assert robot["componentProfileId"] == "universal_robots.ur10e"
    assert robot["modelFamilyId"] == "ur10e"
    assert "articulated-arm" in robot["tags"] and "gripper" not in robot["tags"]
    assert robot["lane"] == "mock"  # the row's own placement lane
    assert [row["component_id"] for row in robot["components"]] == ["ur10e_robot"]
    # A UFactory arm is always a machine member, added with its machine
    # entry; no catalog entry offers it (or any machine row) alone.
    assert not [entry for entry in _row_entries(catalog)
                if {"lite6_robot", "lite6", "uf850_robot"}
                & {row["component_id"] for row in entry["components"]}]
    tool = _entry(catalog, "mock_lite6_pneumatic_pick_and_place.scenario", "lite_gripper")
    assert tool["kind"] == "tool"
    assert tool["componentProfileId"] == "ufactory.lite_gripper"
    assert tool["modelFamilyId"] == "lite_gripper"
    assert [row["component_id"] for row in tool["components"]] == [
        "gripper_io", "lite_gripper",
    ]
    # Facts cover every component model, for the Draft's rows: its lanes too.
    assert catalog["componentProfiles"]["universal_robots.ur10e"]["tags"] == robot["tags"]
    assert catalog["componentProfiles"]["universal_robots.ur10e"]["lanes"] == ["isaac", "mock"]
    assert catalog["componentProfiles"]["ufactory.lite6"]["lanes"] == ["isaac", "mock", "real"]
    device = _entry(catalog, "mock_lite6_pneumatic_pick_and_place.scenario", "gripper_io")
    assert device["kind"] == "io_device"
    assert [row["component_id"] for row in device["components"]] == ["gripper_io"]
    # Identical rows of several Scenarios are one entry listing every source.
    shared = _entry(catalog, "mock_dual_arm_workbench.scenario", "ur5e_robot")
    assert {
        "examples/mock_dual_arm_shared_group.scenario",
        "examples/mock_dual_arm_separate_groups.scenario",
    } <= set(shared["sources"])
    keys = [(entry["componentId"], str(entry["components"])) for entry in _row_entries(catalog)]
    assert len(keys) == len(set(keys))
    # Every offered row set is a valid Deployment intent component list.
    for entry in _row_entries(catalog):
        validate_deployment_intent(_intent(entry["components"]), source=entry["entryId"])


def test_profiles_without_a_curated_row_are_named_not_invented() -> None:
    catalog = component_catalog(None)

    offered = {entry["componentProfileId"] for entry in _row_entries(catalog)}
    assert not offered & set(catalog["profilesWithoutEntry"])


def test_model_sources_resolve_to_the_same_files_from_any_project(tmp_path: Path) -> None:
    project = tmp_path / "cell.scenario"
    project.mkdir()
    absolute = _entry(component_catalog(None), "mock_ur10e_pick_and_place.scenario", "ur10e_robot")
    relative = _entry(
        component_catalog(str(project)), "mock_ur10e_pick_and_place.scenario", "ur10e_robot"
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
    state = ComposerState(application_start_id=0x3F9A0000000000A1)
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
    _models(tmp_path, monkeypatch, {
        "arm": {"id": "vendor.arm", "lanes": {"mock": {}, "real": {}}},
        "hand": {"id": "vendor.hand"},
    })
    model = {"urdf": {"project_path": "model.urdf"}, "package_roots": {}}
    arm = {
        "component_id": "arm", "component_profile_id": "vendor.arm", "lane": "mock",
        "model_source": model, "target_settings": {},
        "root_pose_in_cell": {"position_m": [0, 0, 0], "orientation_xyzw": [0, 0, 0, 1]},
        "configuration": {"runtime_kind": "ros2_control"},
    }
    # The mounted tool has no root pose: its Scenario's mount row places it.
    hand = {
        "component_id": "hand", "component_profile_id": "vendor.hand",
        "model_source": model, "target_settings": {},
        "configuration": {"runtime_kind": "ros2_control"},
    }
    scenario = tmp_path / "examples" / "cell.scenario"
    scenario.mkdir(parents=True)
    (scenario / "deployment.yaml").write_text(json.dumps({
        "api_version": "manyforge.deployment_intent.v3",
        "collision_rules": [{"rule_id": "everything", "clearance_m": 0.001}],
        "components": [arm, hand],
        "attachments": [{
            "parent": {"component_id": "arm", "interface": "flange"},
            "child": {"component_id": "hand", "interface": "mount"},
        }],
        "policies": {},
    }))
    # The entry describes its example by the Scenario's one description.
    (scenario / "scenario.yaml").write_text(json.dumps({"description": "Cell"}))
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
    assert entries["arm"]["description"] == "Cell"
    assert catalog["componentProfiles"] == {
        "vendor.arm": {
            "lanes": ["mock", "real"], "tags": ["articulated-arm"],
            "attachmentInterfaces": ["flange"], "tcpTargets": [], "frameTargets": [],
            "collisionBodyTargets": [],
        },
        "vendor.hand": {
            "lanes": [], "tags": ["gripper", "tool"],
            "attachmentInterfaces": [], "tcpTargets": [], "frameTargets": [],
            "collisionBodyTargets": [],
        },
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
    # The model asset's own payload (kg; m in its root link); absent means 0.
    _models(tmp_path, monkeypatch, {
        "hand": {"id": "vendor.hand",
                 "payload": {"mass_kg": 0.6, "center_of_mass_m": [0.0, 0.01, 0.05]}},
        "plain_hand": {"id": "vendor.plain_hand"},
    })
    (tmp_path / "examples").mkdir()
    monkeypatch.setattr(catalog_module, "_COMPONENT_TYPES", types)
    monkeypatch.setattr(catalog_module, "_EXAMPLES", tmp_path / "examples")

    profiles = component_catalog(None)["componentProfiles"]

    assert profiles["vendor.hand"]["payload"] == {
        "massKg": 0.6, "centerOfMassM": [0.0, 0.01, 0.05],
    }
    assert "payload" not in profiles["vendor.plain_hand"]


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
    # A two-target tool with a `tcp` on both targets (its Deployment row
    # chooses the default), a one-target tool and a bracket without TCPs.
    for family, targets in (
        ("dual", [target("tool_b", ["tcp"], "tcp"), target("tool_a", ["tcp", "edge"], "tcp")]),
        ("single", [target("gripper", ["tcp"], "tcp")]),
        ("bracket", [target("mount", [], None)]),
    ):
        (types / f"{family}.component.json").write_text(json.dumps({
            "tags": ["tool"], "attachment_interfaces": [], "command_targets": targets,
        }))
    _models(tmp_path, monkeypatch, {
        family: {"id": f"{family}_model"} for family in ("dual", "single", "bracket")
    })
    (tmp_path / "examples").mkdir()
    monkeypatch.setattr(catalog_module, "_COMPONENT_TYPES", types)
    monkeypatch.setattr(catalog_module, "_EXAMPLES", tmp_path / "examples")

    profiles = component_catalog(None)["componentProfiles"]

    assert profiles["dual_model"]["tcpTargets"] == [
        {"commandTargetId": "tool_a", "tcpFrameRoleIds": ["edge", "tcp"], "defaultTcpFrameRoleId": "tcp"},
        {"commandTargetId": "tool_b", "tcpFrameRoleIds": ["tcp"], "defaultTcpFrameRoleId": "tcp"},
    ]
    assert profiles["single_model"]["tcpTargets"] == [
        {"commandTargetId": "gripper", "tcpFrameRoleIds": ["tcp"], "defaultTcpFrameRoleId": "tcp"},
    ]
    assert profiles["bracket_model"]["tcpTargets"] == []
    assert profiles["bracket_model"]["frameTargets"] == [
        {"commandTargetId": "mount", "frameRoleIds": ["base"]},
    ]


def test_profile_facts_carry_each_targets_collision_bodies_contact_bodies_and_grip_body(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    def bodies(*roles: str) -> list[dict]:
        return [{"collision_body_role_id": role, "model_role_id": "tool_model"} for role in roles]

    types = tmp_path / "component_type"
    types.mkdir()
    (types / "gripper.component.json").write_text(json.dumps({
        "tags": ["tool", "gripper"], "attachment_interfaces": [],
        "command_targets": [{
            "command_target_id": "gripper",
            "collision_body_roles": bodies("gripper_pads", "gripper_collision"),
            "grip_body_role_id": "gripper_collision",
            "default_tcp_frame_role_id": "tcp",
            "frame_roles": [
                {"frame_role_id": "mount"},
                {"frame_role_id": "tcp", "tcp": True,
                 "contact_body_role_ids": ["gripper_pads"]},
                {"frame_role_id": "edge", "tcp": True},
            ],
        }],
    }))
    # A passive tool: collision bodies, no grip body, a TCP without contact bodies.
    (types / "probe.component.json").write_text(json.dumps({
        "tags": ["tool"], "attachment_interfaces": [],
        "command_targets": [
            {"command_target_id": "tool", "collision_body_roles": bodies("default_collision"),
             "default_tcp_frame_role_id": "tcp",
             "frame_roles": [{"frame_role_id": "tcp", "tcp": True}]},
            {"command_target_id": "bare", "frame_roles": []},
        ],
    }))
    _models(tmp_path, monkeypatch, {
        "gripper": {"id": "gripper_model"}, "probe": {"id": "probe_model"},
    })
    (tmp_path / "examples").mkdir()
    monkeypatch.setattr(catalog_module, "_COMPONENT_TYPES", types)
    monkeypatch.setattr(catalog_module, "_EXAMPLES", tmp_path / "examples")

    profiles = component_catalog(None)["componentProfiles"]

    assert profiles["gripper_model"]["collisionBodyTargets"] == [{
        "commandTargetId": "gripper",
        "collisionBodyRoleIds": ["gripper_collision", "gripper_pads"],
        "tcpContactBodyRoleIds": {"edge": [], "tcp": ["gripper_pads"]},
        "gripBodyRoleId": "gripper_collision",
    }]
    # A target without collision bodies is not listed.
    assert profiles["probe_model"]["collisionBodyTargets"] == [{
        "commandTargetId": "tool",
        "collisionBodyRoleIds": ["default_collision"],
        "tcpContactBodyRoleIds": {"tcp": []},
        "gripBodyRoleId": None,
    }]


def test_maintained_models_declare_one_tcp_target_each() -> None:
    profiles = component_catalog(None)["componentProfiles"]

    assert profiles["ufactory.lite6"]["tcpTargets"] == [
        {"commandTargetId": "arm", "tcpFrameRoleIds": ["flange"], "defaultTcpFrameRoleId": "flange"},
    ]
    assert profiles["ufactory.lite_gripper"]["tcpTargets"] == [
        {"commandTargetId": "gripper", "tcpFrameRoleIds": ["tcp"], "defaultTcpFrameRoleId": "tcp"},
    ]
    # No maintained model needs a Deployment `default_tcp`.
    assert all(len(facts["tcpTargets"]) <= 1 for facts in profiles.values())


# --- machines ---------------------------------------------------------------


def test_machine_entries_come_from_the_machine_models() -> None:
    catalog = component_catalog(None)
    machines = {
        entry["model_id"]: entry for entry in catalog["entries"] if entry["kind"] == "machine"
    }
    lite6 = machines["ufactory.lite6_machine"]
    assert set(lite6) == {
        "kind", "model_id", "title", "lanes", "default_owner_id", "owner_row", "members",
    }
    assert lite6["lanes"] == ["isaac", "mock", "real"]  # its arm's lanes
    assert lite6["default_owner_id"] == "lite6"
    assert lite6["owner_row"] == {
        "component_id": "lite6",
        "component_profile_id": "ufactory.lite6_machine",
        "configuration": {},
    }
    assert [(m["role"], m["model_id"]) for m in lite6["members"]] == [
        ("arm", "ufactory.lite6"),
        ("controller_io", "ufactory.lite6_controller_io"),
        ("tool_io", "ufactory.lite6_tool_io"),
    ]
    assert all(set(m) == {"role", "model_id", "title", "row"}
               for m in lite6["members"])
    arm, controller_io, _tool_io = lite6["members"]
    assert arm["title"] == "UFACTORY Lite 6"
    assert controller_io["row"] == {
        "component_id": "lite6_controller_io",
        "component_profile_id": "ufactory.lite6_controller_io",
    }
    row = arm["row"]
    assert row["component_id"] == "lite6_arm"
    assert not {"owner", "role", "lane"} & set(row)
    assert [
        stream["controllable_component_id"]
        for stream in row["configuration"]["runtime_configuration"]["component_streams"]
    ] == ["lite6_arm"]
    assert row["component_profile_id"] == "ufactory.lite6"
    assert row["model_source"]["urdf"]["external_path"].endswith("models/lite6/lite6.urdf")
    assert row["configuration"]["runtime_kind"] == "ros2_control"
    assert "device_address_ref" not in row["configuration"]["runtime_configuration"]
    assert row["configuration"]["runtime_configuration"]["initial_joint_positions_rad"] == [
        0.0, 0.0, 1.57, 0.0, 0.0, 0.0,
    ]
    # A new arm is applicable as added: its controlled Stop is filled from the
    # model's deceleration, one limit per arm joint.
    stop = row["target_settings"]["arm"]["controlled_stop"]
    assert stop["stop_mode"] == "soft_on_path"
    assert [(limit["source_joint_name"], limit["max_deceleration_rad_s2"])
            for limit in stop["per_joint_limits"]] == [(f"joint{i}", 10.0) for i in range(1, 7)]
    # Its fault Stop (OD-373): a runtime collision Stop is controlled and retryable.
    fault = row["configuration"]["default_fault_stop_profile"]
    assert fault["stop_mode"] == "soft_on_path"
    assert fault["per_joint_max_deceleration_rad_s2"] == {f"joint{i}": 10.0 for i in range(1, 7)}
    # Filled with ids, owner and role, the rows are a valid machine in a Draft.
    rows = [
        {**lite6["owner_row"], "lane": "mock"},
        {**row, "owner": "lite6", "role": "arm"},
        {**controller_io["row"], "owner": "lite6", "role": "controller_io"},
    ]
    validate_deployment_intent(_intent(rows), source="machine")


def test_sensor_entries_come_from_the_sensor_models() -> None:
    catalog = component_catalog(None)
    [entry] = [entry for entry in catalog["entries"] if entry["kind"] == "sensor"]
    assert entry["entryId"] == "model/manyforge.pointcloud_source"
    assert entry["componentId"] == "pointcloud_source"
    assert entry["componentProfileId"] == "manyforge.pointcloud_source"
    assert entry["lane"] is None and entry["modelFamilyId"] is None
    assert entry["sources"] == ["components/sensor/manyforge.pointcloud_source/component.yaml"]
    assert entry["components"] == [{
        "component_id": "pointcloud_source",
        "component_profile_id": "manyforge.pointcloud_source",
        "target_settings": {},
        "configuration": {"runtime_kind": "sensor", "runtime_configuration": {}},
    }]
    # A sensor has no lane (PC-1).
    assert catalog["componentProfiles"]["manyforge.pointcloud_source"]["lanes"] == []
    assert _kind(entry["components"][0], []) == "sensor"
    row = {
        **entry["components"][0],
        "configuration": {"runtime_kind": "sensor",
                          "runtime_configuration": {"topic": "/camera/points",
                                                    "source_frame_id": "camera_link"}},
    }
    assert validate_deployment_intent(_intent([row]), source="sensor")["components"] == [row]


def test_the_passive_tool_is_offered_from_its_model(tmp_path: Path) -> None:
    # OD-336: one generic passive tool, mount and TCP frames only, no lane;
    # its default model is the placeholder a project points at its own files.
    catalog = component_catalog(None)
    [entry] = [entry for entry in catalog["entries"]
               if entry.get("entryId") == "model/manyforge.passive_tool"]
    assert entry["kind"] == "tool"
    assert entry["componentId"] == "passive_tool"
    assert entry["modelFamilyId"] == "passive_tool"
    assert entry["lane"] is None and entry["tags"] == ["tool"]
    assert entry["sources"] == ["manyforge_example_models/models/passive_tool/component.yaml"]
    [row] = entry["components"]
    assert row["component_profile_id"] == "manyforge.passive_tool"
    assert "lane" not in row and "root_pose_in_cell" not in row
    assert row["configuration"] == {
        "runtime_kind": "ros2_control",
        "runtime_configuration": {"component_streams": []},
    }
    source = row["model_source"]
    assert source["urdf"]["external_path"].endswith("models/passive_tool/passive_tool.urdf")
    assert source["srdf"]["external_path"].endswith("models/passive_tool/passive_tool.srdf")
    # The model folder is its own package: mesh URIs `package://passive_tool/...`.
    assert set(source["package_roots"]) == {"passive_tool"}
    assert source["package_roots"]["passive_tool"]["external_path"].endswith(
        "manyforge_example_models/models/passive_tool")
    validate_deployment_intent(_intent([row]), source=entry["entryId"])
    facts = catalog["componentProfiles"]["manyforge.passive_tool"]
    assert facts["lanes"] == [] and facts["tags"] == ["tool"]
    assert facts["attachmentInterfaces"] == ["mount"]
    assert facts["tcpTargets"] == [
        {"commandTargetId": "tool", "tcpFrameRoleIds": ["tcp"], "defaultTcpFrameRoleId": "tcp"},
    ]
    assert facts["hasMotionTarget"] is False
    # From a project, the same files, relative to it.
    project = tmp_path / "cell.scenario"
    project.mkdir()
    [relative] = [entry for entry in component_catalog(str(project))["entries"]
                  if entry.get("entryId") == "model/manyforge.passive_tool"]
    resolved = resolve_source_reference(
        relative["components"][0]["model_source"]["urdf"], project_root=project)
    assert resolved == Path(source["urdf"]["external_path"])


def test_the_tool_is_offered_with_its_starter_parameters_inline() -> None:
    # OD-357: the Tool from parameters; its row holds them inline.
    catalog = component_catalog(None)
    [entry] = [entry for entry in catalog["entries"]
               if entry.get("entryId") == "model/manyforge.tool"]
    assert entry["kind"] == "tool"
    assert entry["componentId"] == "tool"
    assert entry["lane"] is None
    [row] = entry["components"]
    assert row["component_profile_id"] == "manyforge.tool"
    parameters = row["model_source"]["parameters"]
    assert set(row["model_source"]) == {"parameters"}
    assert parameters["api_version"] == "manyforge.parametric_tool.v1"
    assert parameters["kind"] == "tool"
    assert [tcp["id"] for tcp in parameters["tcps"]] == ["tcp"]
    validate_deployment_intent(_intent([row]), source=entry["entryId"])


def test_owner_less_io_banks_come_from_their_models() -> None:
    catalog = component_catalog(None)
    [entry] = [entry for entry in catalog["entries"]
               if entry.get("entryId") == "model/manyforge.io_bank_64x64"]
    assert entry["kind"] == "io_device"
    assert entry["componentId"] == "io_bank_64x64"
    assert entry["lane"] == "mock" and entry["modelFamilyId"] is None
    assert entry["sources"] == ["components/io_device/manyforge.io_bank_64x64/component.yaml"]
    assert entry["components"] == [{
        "component_id": "io_bank_64x64",
        "component_profile_id": "manyforge.io_bank_64x64",
        "configuration": {},
        "lane": "mock",
    }]
    # It runs on every lane: Mock and Isaac through the host's Mock I/O.
    assert catalog["componentProfiles"]["manyforge.io_bank_64x64"]["lanes"] == [
        "isaac", "mock", "real"]
    # A machine's I/O banks are offered only with their machine.
    assert not [entry for entry in catalog["entries"]
                if entry.get("componentProfileId") == "ufactory.lite6_controller_io"]
    validate_deployment_intent(_intent(entry["components"]), source=entry["entryId"])


def test_the_assistant_model_is_offered_like_a_sensor(monkeypatch) -> None:
    path = "components/assistant/manyforge.assistant/component.yaml"
    descriptor = {
        "kind": "assistant",
        "id": "manyforge.assistant",
        "title": "AI assistant (Composer chat bridge)",
        "adapter_row": {"adapter": "assistant", "backend": "local_process",
                        "health_period_s": 1.0, "stream_budget_s": 5.0},
    }
    read = catalog_module._descriptor
    monkeypatch.setattr(catalog_module, "_descriptor",
                        lambda name: descriptor if name == path else read(name))
    sources = json.loads(catalog_module._SOURCES.read_text(encoding="utf-8"))
    sources["component_models"] = [
        name for name in sources["component_models"] if name != path
    ] + [path]

    entries = catalog_module._lane_free_entries(sources)

    assert [entry["kind"] for entry in entries] == ["sensor", "assistant"]
    entry = entries[-1]
    assert entry["entryId"] == "model/manyforge.assistant"
    assert entry["componentId"] == "assistant"
    assert entry["lane"] is None and entry["modelFamilyId"] is None
    # No sensor defaults: the Add panel asks for the model and the profile.
    assert entry["components"] == [{
        "component_id": "assistant",
        "component_profile_id": "manyforge.assistant",
        "target_settings": {},
        "configuration": {"runtime_kind": "assistant", "runtime_configuration": {}},
    }]
    assert _kind(entry["components"][0], []) == "assistant"


def test_machine_rows_are_not_offered_alone_and_owner_is_not_followed() -> None:
    catalog = component_catalog(None)
    real = [
        entry for entry in _row_entries(catalog)
        if "examples/real_lite6_pneumatic_pick_and_place.scenario" in entry["sources"]
    ]
    offered = {row["component_id"] for entry in real for row in entry["components"]}
    # The owner, its members and the gripper wired to a member's output come
    # with the machine entry, never as separate rows.
    assert not offered & {
        "lite6", "lite6_robot", "lite6_robot_controller_io", "lite6_robot_tool_io",
        "lite_gripper",
    }
    rows = {
        "lite6": {"component_id": "lite6"},
        "lite6_robot": {"component_id": "lite6_robot", "owner": "lite6"},
    }
    # The assembly follows component references only, not `owner`.
    assert catalog_module._assembly("lite6_robot", rows) == ["lite6_robot"]
