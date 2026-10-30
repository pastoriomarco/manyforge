"""Merge 6 slice A: the robot/tool asset split and its maintained data."""

from __future__ import annotations

import json
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest
import yaml

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "models"))
sys.path.insert(0, str(ROOT))

import cut_combined_model  # noqa: E402
from scripts.components import mock_controller_profiles  # noqa: E402

MODELS = ROOT / "manyforge_example_models" / "models"
MAINTAINED = ROOT / "manyforge_deployment" / "resources" / "maintained"
COMPONENT_TYPES = MAINTAINED / "catalog" / "component_type"

# combined bundle, mount joint, arm, tool, combined file stem of the tool cut
CUTS = (
    ("lite6_lite_gripper", "gripper_fix", "lite6", "lite_gripper", "lite6_lite_gripper"),
    ("uf850_xarm_gripper", "gripper_fix", "uf850", "xarm_gripper", "uf850_xarm_gripper"),
    ("ur10e_robotiq_2f_140", "gripper_base_joint", "ur10e", "robotiq_2f_140", "ur10e_robotiq_2f_140"),
    ("ur10e_robotiq_2f_140", "gripper_base_joint", "ur10e", "robotiq_2f_140.structural",
     "ur10e_robotiq_2f_140.structural"),
    ("ur5e_robotiq_2f_85", "robotiq_85_base_joint", "ur5e", "robotiq_2f_85", "ur5e_robotiq_2f_85"),
)
ARMS = ("lite6", "uf850", "ur10e", "ur5e")
TOOLS = ("lite_gripper", "xarm_gripper", "robotiq_2f_140", "robotiq_2f_85")

# D5: old profile id -> new id (absent: removed); new ids not listed are new.
RENAMED = {
    "lite6_isaac_commanded_v0": "lite6_isaac_commanded_v0",
    "lite6_mock_commanded_v0": "lite6_mock_commanded_v0",
    "lite6_real_measured_v0": "lite6_real_measured_v0",
    "uf850_xarm_gripper_mock_commanded_v0": "uf850_mock_commanded_v0",
    "ur10e_robotiq_assistant_isaac_commanded_v0": "ur10e_assistant_isaac_commanded_v0",
    "ur10e_robotiq_assistant_mock_commanded_v0": "ur10e_assistant_mock_commanded_v0",
    "ur10e_robotiq_isaac_commanded_v0": "ur10e_isaac_commanded_v0",
    "ur10e_robotiq_mock_commanded_v0": "ur10e_mock_commanded_v0",
    "ur5e_robotiq_mock_commanded_v0": "ur5e_mock_commanded_v0",
}
TOOL_PROFILES = {
    "lite_gripper_isaac_commanded_v0": ("lite_gripper", "isaac_profile"),
    "lite_gripper_mock_commanded_v0": ("lite_gripper", "mock_profile"),
    "lite_gripper_pneumatic_v0": ("lite_gripper", None),
    "robotiq_2f_140_isaac_commanded_v0": ("robotiq_2f_140", "isaac_profile"),
    "robotiq_2f_140_mock_commanded_v0": ("robotiq_2f_140", "mock_profile"),
    "robotiq_2f_85_mock_commanded_v0": ("robotiq_2f_85", "mock_profile"),
    "xarm_gripper_mock_commanded_v0": ("xarm_gripper", "mock_profile"),
}


def _description() -> dict:
    return json.loads((MAINTAINED / "maintained_deployment_sources.v1.json").read_text())


def _families() -> dict[str, dict]:
    return {row["model_family_id"]: row for row in _description()["model_families"]}


def _profiles() -> dict[str, dict]:
    return {
        row["runtime_restoration_profile_id"]: row
        for row in _description()["runtime_restoration_profiles"]
    }


def _normalized(element: ET.Element) -> bytes:
    copy = ET.fromstring(ET.tostring(element))
    for node in copy.iter():
        node.tail = None
        node.text = (node.text or "").strip() or None
    return ET.tostring(copy)


def _by_name(root: ET.Element, tag: str) -> dict[str, bytes]:
    return {node.get("name"): _normalized(node) for node in root.findall(tag)}


@pytest.mark.parametrize(("combined", "mount", "arm", "tool", "stem"), CUTS)
def test_checked_in_bundles_are_the_cut_of_the_combined_bundles(
    combined: str, mount: str, arm: str, tool: str, stem: str
) -> None:
    tool_name = tool.split(".")[0]
    model = cut_combined_model.cut_combined_model(
        (MODELS / combined / f"{stem}.urdf").read_text(),
        (MODELS / combined / f"{stem}.srdf").read_text(),
        mount_joint=mount, arm_name=arm, tool_name=tool_name,
    )
    assert model.arm_urdf == (MODELS / arm / f"{arm}.urdf").read_text()
    assert model.arm_srdf == (MODELS / arm / f"{arm}.srdf").read_text()
    assert model.tool_urdf == (MODELS / tool_name / f"{tool}.urdf").read_text()
    assert model.tool_srdf == (MODELS / tool_name / f"{tool}.srdf").read_text()


@pytest.mark.parametrize(("combined", "mount", "arm", "tool", "stem"), CUTS)
def test_the_cut_keeps_every_link_joint_and_pose(
    combined: str, mount: str, arm: str, tool: str, stem: str
) -> None:
    tool_name = tool.split(".")[0]
    whole = ET.parse(MODELS / combined / f"{stem}.urdf").getroot()
    arm_root = ET.parse(MODELS / arm / f"{arm}.urdf").getroot()
    tool_root = ET.parse(MODELS / tool_name / f"{tool}.urdf").getroot()
    mount_link = f"{tool_name}_mount"
    parent = whole.find(f"joint[@name='{mount}']/parent").get("link")
    for tag, extra in (("link", {mount_link}), ("joint", set())):
        combined_items = _by_name(whole, tag)
        arm_items, tool_items = _by_name(arm_root, tag), _by_name(tool_root, tag)
        assert not set(arm_items) & set(tool_items)
        assert set(arm_items) | set(tool_items) == set(combined_items) | extra
        for name, value in combined_items.items():
            got = arm_items.get(name) or tool_items[name]
            if name == mount:
                got = got.replace(f'"{mount_link}"'.encode(), f'"{parent}"'.encode())
            assert got == value, name
    # The tool's root is its mount link; the mount joint hangs below it.
    children = {joint.find("child").get("link") for joint in tool_root.findall("joint")}
    roots = [link.get("name") for link in tool_root.findall("link") if link.get("name") not in children]
    assert roots == [mount_link]
    assert tool_root.find(f"joint[@name='{mount}']/parent").get("link") == mount_link
    assert arm_root.get("name") == arm
    assert tool_root.get("name") == tool_name


@pytest.mark.parametrize(("combined", "mount", "arm", "tool", "stem"), CUTS)
def test_only_collision_pairs_across_the_mount_are_dropped(
    combined: str, mount: str, arm: str, tool: str, stem: str
) -> None:
    tool_name = tool.split(".")[0]

    def pairs(path: Path) -> list[tuple[str, str]]:
        return [
            (row.get("link1"), row.get("link2"))
            for row in ET.parse(path).getroot().findall("disable_collisions")
        ]

    def links(path: Path) -> set[str]:
        return {link.get("name") for link in ET.parse(path).getroot().findall("link")}

    arm_links = links(MODELS / arm / f"{arm}.urdf")
    tool_links = links(MODELS / tool_name / f"{tool}.urdf")
    arm_pairs = pairs(MODELS / arm / f"{arm}.srdf")
    tool_pairs = pairs(MODELS / tool_name / f"{tool}.srdf")
    assert all({a, b} <= arm_links for a, b in arm_pairs)
    assert all({a, b} <= tool_links for a, b in tool_pairs)
    for pair in pairs(MODELS / combined / f"{stem}.srdf"):
        crosses = (pair[0] in arm_links) != (pair[1] in arm_links)
        assert crosses != (pair in arm_pairs or pair in tool_pairs), pair


def test_robotiq_2f_140_keeps_its_structural_projection() -> None:
    folder = MODELS / "robotiq_2f_140"
    material = (
        '\n      <material name="">\n        <color rgba="0.9 0.0 0.0 1"/>\n'
        "      </material>"
    )
    full = (folder / "robotiq_2f_140.urdf").read_text()
    assert full.count(material) == 2
    assert full.replace(material, "") == (folder / "robotiq_2f_140.structural.urdf").read_text()
    srdf = (folder / "robotiq_2f_140.srdf").read_text()
    start = srdf.rfind("\n", 0, srdf.index("<gripper")) + 1
    end = srdf.index("</gripper>") + len("</gripper>\n")
    assert srdf[:start] + srdf[end:] == (folder / "robotiq_2f_140.structural.srdf").read_text()
    # The arm half has neither extension: its plain files are structural.
    assert "<gripper" not in (MODELS / "ur10e" / "ur10e.srdf").read_text()
    assert material not in (MODELS / "ur10e" / "ur10e.urdf").read_text()


def test_cut_refuses_a_moving_or_missing_mount_joint() -> None:
    urdf = (MODELS / "lite6_lite_gripper" / "lite6_lite_gripper.urdf").read_text()
    srdf = (MODELS / "lite6_lite_gripper" / "lite6_lite_gripper.srdf").read_text()
    with pytest.raises(cut_combined_model.CutError, match="must be fixed"):
        cut_combined_model.cut_combined_model(
            urdf, srdf, mount_joint="joint6", arm_name="a", tool_name="t")
    with pytest.raises(cut_combined_model.CutError, match="no joint"):
        cut_combined_model.cut_combined_model(
            urdf, srdf, mount_joint="missing", arm_name="a", tool_name="t")


def test_cut_command_line_check_mode() -> None:
    arguments = [
        "--urdf", str(MODELS / "lite6_lite_gripper" / "lite6_lite_gripper.urdf"),
        "--srdf", str(MODELS / "lite6_lite_gripper" / "lite6_lite_gripper.srdf"),
        "--mount-joint", "gripper_fix",
        "--arm-name", "lite6", "--arm-out", str(MODELS / "lite6" / "lite6"),
        "--tool-name", "lite_gripper",
        "--tool-out", str(MODELS / "lite_gripper" / "lite_gripper"),
        "--check",
    ]
    assert cut_combined_model.main(arguments) == 0


@pytest.mark.parametrize("family_id", ARMS + TOOLS)
def test_split_families_name_their_own_model_files(family_id: str) -> None:
    family = _families()[family_id]
    source = family["source"]
    urdf = ET.parse(ROOT / source["urdf_path"]).getroot()
    srdf = ET.parse(ROOT / source["srdf_path"]).getroot()
    links = {link.get("name") for link in urdf.findall("link")}
    joints = {joint.get("name"): joint for joint in urdf.findall("joint")}
    groups = {group.get("name") for group in srdf.findall("group")}
    semantics = family["semantics"]
    frames = {row["frame_role_id"]: row["source_link_name"] for row in semantics["frame_role_bindings"]}
    resources = {row["resource_role_id"]: row["source_joint_names"] for row in semantics["resource_role_bindings"]}
    assert set(frames.values()) <= links
    assert all(joints[name].get("type") != "fixed" for names in resources.values() for name in names)
    assert {row["source_group_name"] for row in semantics["model_group_role_bindings"]} <= groups
    for row in semantics["collision_body_role_bindings"]:
        assert set(row["source_link_names"]) <= links
    # Meshes stay in the combined bundle the prefix and resource root name.
    prefix = source["accepted_package_uri_prefix"]
    assert prefix == f"package://manyforge_example_models/{source['resource_root'][len('manyforge_example_models/'):]}/"
    for mesh in urdf.iter("mesh"):
        assert mesh.get("filename").startswith(prefix)
        assert (ROOT / source["resource_root"] / mesh.get("filename")[len(prefix):]).is_file()
    if family_id in TOOLS:
        assert "arm_joints" not in resources and set(resources) == {"fingers"}
        assert semantics["root_frame_role_id"] == "mount"
        assert frames["mount"] == f"{family_id}_mount"
        assert {"tcp", "mount"} <= set(frames)
        # The pneumatic preset check and the gripper rows read these states.
        states = {(state.get("group"), state.get("name")) for state in srdf.findall("group_state")}
        gripper_group = semantics["model_group_role_bindings"][0]["source_group_name"]
        assert {(gripper_group, "open"), (gripper_group, "closed")} <= states
    else:
        assert set(resources) == {"arm_joints"}
        assert {"base", "flange"} <= set(frames) and "tcp" not in frames
        assert all(not row["collision_body_role_id"].startswith("gripper")
                   for row in semantics["collision_body_role_bindings"])


def test_robotiq_2f_85_default_tcp_is_the_mount() -> None:
    # D3: the UR5e `tcp` was `tool0`; the tool's mount sits on `tool0`.
    frames = {
        row["frame_role_id"]: row["source_link_name"]
        for row in _families()["robotiq_2f_85"]["semantics"]["frame_role_bindings"]
    }
    assert frames["tcp"] == frames["mount"] == "robotiq_2f_85_mount"
    # Its second descriptor TCP keeps its name.
    assert frames["base"] == "robotiq_85_base_link"
    assert _families()["robotiq_2f_85"]["tcp_frame_role_ids"] == ["tcp", "base"]
    ur5e = {
        row["frame_role_id"]: row["source_link_name"]
        for row in _families()["ur5e"]["semantics"]["frame_role_bindings"]
    }
    assert ur5e["flange"] == "tool0"


@pytest.mark.parametrize("family_id", ARMS + TOOLS)
def test_model_asset_catalog_matches_its_family(family_id: str) -> None:
    family = _families()[family_id]
    catalog = json.loads((ROOT / family["catalog_source"]).read_text())
    assert catalog["model_asset_id"] == family_id
    for field in ("collision_body_role_bindings", "frame_role_bindings",
                  "instance_cardinality", "model_group_role_bindings",
                  "resource_role_bindings", "root_frame_role_id"):
        assert catalog[field] == family["semantics"][field], field


def test_standalone_robotiq_fixture_keeps_its_own_catalog() -> None:
    standalone = MAINTAINED / "model_sources" / "robotiq_2f_85"
    catalog = json.loads((standalone / "robotiq_2f_85.model_asset.json").read_text())
    roots = {row["frame_role_id"]: row["source_link_name"] for row in catalog["frame_role_bindings"]}
    assert roots["root"] == "robotiq_85_base_link"


@pytest.mark.parametrize("family_id", ARMS)
def test_robot_component_type_offers_its_flange(family_id: str) -> None:
    for suffix in (".component", ".fixed_scale.component"):
        document = json.loads((COMPONENT_TYPES / f"{family_id}{suffix}.json").read_text())
        assert document["attachment_interfaces"] == [{
            "accepts_tags": ["tool"], "attachment_interface_id": "flange",
            "frame_role_id": "flange", "max_attachments": 1, "model_role_id": "arm_model",
        }]
        assert document["tags"] == ["articulated-arm"]
        (target,) = document["command_targets"]
        assert target["command_target_id"] == "arm"
        assert [row["frame_role_id"] for row in target["frame_roles"]] == ["base", "flange"]


@pytest.mark.parametrize("family_id", TOOLS)
def test_tool_component_type_mounts_at_its_root(family_id: str) -> None:
    for suffix in (".component", ".fixed_scale.component"):
        document = json.loads((COMPONENT_TYPES / f"{family_id}{suffix}.json").read_text())
        assert document["model_roles"] == [{"cardinality": "exactly_one", "model_role_id": "tool_model"}]
        assert document["component_root"] == {"frame_role_id": "mount", "model_role_id": "tool_model"}
        assert document["attachment_interfaces"] == [{
            "accepts_tags": ["articulated-arm"], "attachment_interface_id": "mount",
            "frame_role_id": "mount", "max_attachments": 1, "model_role_id": "tool_model",
        }]
        assert sorted(document["tags"]) == ["gripper", "tool"]
        (target,) = document["command_targets"]
        assert target["command_target_id"] == "gripper"
        # Each TCP under its own name (the 2F-85 also has `base`), `tcp` the default.
        tcps = _families()[family_id].get("tcp_frame_role_ids", ["tcp"])
        assert [(row["frame_role_id"], row["asset_frame_role_id"], row["tcp"])
                for row in target["frame_roles"]] == [(role, role, True) for role in sorted(tcps)]
        assert target["default_tcp_frame_role_id"] == "tcp"


def _generator():
    sys.path.insert(0, str(ROOT / "manyforge_deployment" / "tools"))
    try:
        import generate_maintained_deployment_occurrences as generator  # noqa: PLC0415
    finally:
        sys.path.pop(0)
    return generator


@pytest.mark.parametrize(
    "family_id",
    sorted(family_id for family_id, family in _families().items()
           if "tcp_frame_role_ids" not in family),
)
def test_component_types_without_a_tcp_list_are_unchanged(family_id: str) -> None:
    # A family whose only TCP is the assumed one (a tool's `tcp`, a robot's
    # `flange`) generates the same bytes as the shipped component types.
    generator = _generator()
    family = _families()[family_id]
    for runtime_scaling, suffix in ((True, ".component"), (False, ".fixed_scale.component")):
        document = generator.component_type_document(family, runtime_scaling=runtime_scaling)
        shipped = (COMPONENT_TYPES / f"{family_id}{suffix}.json").read_bytes()
        assert generator.pretty_bytes(document) == shipped, suffix


def test_a_tool_tcp_list_names_its_tcps_and_default() -> None:
    # A registered tool with `tcps: [cup_center, tip]`: its gripper target
    # exposes both under their names, both TCPs with the finger contact bodies,
    # `cup_center` the default; no `tcp` role.
    generator = _generator()
    family = json.loads(json.dumps(_families()["lite_gripper"]))
    family["model_family_id"] = "cup"
    family["semantics"]["frame_role_bindings"] = [
        {"frame_role_id": "cup_center", "source_link_name": "link_tcp"},
        {"frame_role_id": "mount", "source_link_name": "lite_gripper_mount"},
        {"frame_role_id": "tip", "source_link_name": "lite_gripper_base_link"},
    ]
    family["tcp_frame_role_ids"] = ["cup_center", "tip"]
    for runtime_scaling in (True, False):
        document = generator.component_type_document(family, runtime_scaling=runtime_scaling)
        (target,) = document["command_targets"]
        assert target["frame_roles"] == [
            {"asset_frame_role_id": role, "contact_body_role_ids": ["gripper_fingers"],
             "frame_role_id": role, "model_role_id": "tool_model", "tcp": True}
            for role in ("cup_center", "tip")
        ]
        assert target["default_tcp_frame_role_id"] == "cup_center"
        assert document["component_root"] == {"frame_role_id": "mount",
                                              "model_role_id": "tool_model"}
    family["tcp_frame_role_ids"] = ["tip", "cup_center"]
    (target,) = generator.component_type_document(family)["command_targets"]
    assert target["default_tcp_frame_role_id"] == "tip"


def test_a_robot_tcp_list_names_its_tcps_and_default() -> None:
    # D1: a robot's own default TCP is its first descriptor TCP, here `pen`.
    generator = _generator()
    family = json.loads(json.dumps(_families()["lite6"]))
    family["semantics"]["frame_role_bindings"].append(
        {"frame_role_id": "pen", "source_link_name": "link6"})
    family["semantics"]["frame_role_bindings"].sort(key=lambda row: row["frame_role_id"])
    family["tcp_frame_role_ids"] = ["pen", "flange"]
    (target,) = generator.component_type_document(family)["command_targets"]
    assert target["command_target_id"] == "arm"
    assert {row["frame_role_id"]: row.get("tcp", False) for row in target["frame_roles"]} == {
        "base": False, "flange": True, "pen": True}
    assert target["default_tcp_frame_role_id"] == "pen"


def test_combined_families_stay_as_fixtures_without_profiles() -> None:
    # D2: the combined families and their types remain for the test fixtures;
    # no maintained profile selects them, so the Real arm-only type is gone.
    families = _families()
    used = {row.get("model_family_id") for row in _profiles().values()}
    for combined in ("lite6_lite_gripper", "uf850_xarm_gripper",
                     "ur10e_robotiq_2f_140", "ur5e_robotiq_2f_85"):
        assert combined in families and combined not in used
        assert (COMPONENT_TYPES / f"{combined}.fixed_scale.component.json").is_file()
    assert not (COMPONENT_TYPES / "lite6_lite_gripper.fixed_scale.arm_only.component.json").exists()


def test_profiles_follow_the_d5_names() -> None:
    profiles = _profiles()
    for old, new in RENAMED.items():
        assert new in profiles
        if old != new:
            assert old not in profiles
    assert set(TOOL_PROFILES) <= set(profiles)
    assert sorted(profiles) == list(profiles)


@pytest.mark.parametrize("profile_id", sorted(set(RENAMED.values())))
def test_robot_profiles_have_no_gripper(profile_id: str) -> None:
    profile = _profiles()[profile_id]
    assert profile["model_family_id"] in ARMS
    runtime = profile["ros2_control_runtime"]
    assert "gripper_target_configurations" not in runtime
    assert "gripper_target_configurations" not in profile["source_fields"]
    assert all(row["stream_id"] == "measured_joints" for row in runtime.get("component_streams", []))
    family = _families()[profile["model_family_id"]]
    (arm_joints,) = [row["source_joint_names"] for row in family["semantics"]["resource_role_bindings"]]
    for stop in profile["stop_profiles"].values():
        assert set(stop["per_joint_max_deceleration_rad_s2"]) == set(arm_joints)
    if "mock_profile" in runtime:
        assert runtime["mock_profile"] == profile["model_family_id"]


@pytest.mark.parametrize("profile_id", sorted(TOOL_PROFILES))
def test_tool_profiles_select_their_own_lane(profile_id: str) -> None:
    family, selector = TOOL_PROFILES[profile_id]
    profile = _profiles()[profile_id]
    assert profile["model_family_id"] == family
    runtime = profile["ros2_control_runtime"]
    lanes = {"isaac_profile", "mock_profile", "real_profile"} & set(runtime)
    assert lanes == ({selector} if selector else set())
    if selector:
        assert runtime[selector] == family
    assert profile["stop_profiles"] == {} and profile["acceptance_obligations"] == []
    for row in runtime.get("gripper_target_configurations", []):
        assert row["controllable_component_id"] == family
        assert row["mount_frame"] == f"{family}_mount"
    streams = [row["stream_id"] for row in runtime.get("component_streams", [])]
    if "component_streams" in runtime:
        assert streams == (["gripper_feedback", "measured_joints"] if selector else [])


def test_mock_rows_split_arm_and_gripper_controllers() -> None:
    rows = {row.mock_profile: row for row in mock_controller_profiles.load(
        ROOT / mock_controller_profiles.DEFAULT_PATH)}
    for arm in ARMS:
        assert rows[arm].arm_controller and rows[arm].gripper_controller is None
    for tool in TOOLS:
        assert rows[tool].arm_controller is None and rows[tool].gripper_controller
        assert rows[tool].vendor_joint_constraints is None
    lines = mock_controller_profiles.cpp_rows([rows["lite_gripper"]], "x").splitlines()
    # M6 ARRIVE: the last column is the arm arrival tolerance (0.0 without an arm).
    assert lines[1] == (
        '{"lite_gripper", "MockLiteGripperSystem", ArmTemplate::kUFactory, "", '
        'std::nullopt, "lite_gripper_controller", true, 1.5, 5.0, 0.0},'
    )
    # Every Mock selector of a maintained profile has a row.
    selected = {
        row["ros2_control_runtime"]["mock_profile"]
        for row in _profiles().values()
        if "mock_profile" in row.get("ros2_control_runtime", {})
    }
    assert selected <= set(rows)


def _write_rows(tmp_path: Path, row: dict) -> Path:
    path = tmp_path / "rows.yaml"
    path.write_text(yaml.safe_dump({"mock_profiles": [row]}), encoding="utf-8")
    return path


def test_mock_rows_need_a_controller_and_vendor_constraints_need_an_arm(tmp_path: Path) -> None:
    base = {"mock_profile": "t", "template": "ufactory", "hardware_component_name": "MockT",
            "hard_stop_completion_timeout_s": 1.5, "gripper_result_timeout_s": 5.0}
    with pytest.raises(mock_controller_profiles.MockControllerProfileError, match="names no controller"):
        mock_controller_profiles.load(_write_rows(tmp_path, base))
    with pytest.raises(mock_controller_profiles.MockControllerProfileError,
                       match="absent without an arm_controller"):
        mock_controller_profiles.load(_write_rows(tmp_path, {
            **base, "gripper_controller": "g",
            "vendor_joint_constraints": {"trajectory": "1.0"}}))
    with pytest.raises(mock_controller_profiles.MockControllerProfileError, match="copied from the vendor"):
        mock_controller_profiles.load(_write_rows(tmp_path, {**base, "arm_controller": "a"}))
    (row,) = mock_controller_profiles.load(_write_rows(tmp_path, {**base, "gripper_controller": "g"}))
    assert row.arm_controller is None


def test_schema_enums_list_every_selected_profile() -> None:
    schema = yaml.safe_load((ROOT / "manyforge_deployment/resources/closed_schema/value/"
                             "manyforge.value_schema.runtime.ros2_control_execution.v1.yaml").read_text())

    def enum(field_id: str) -> set[str]:
        for node in _walk(schema):
            if node.get("field_id") == field_id:
                for constraint in node["value_schema"]["constraints"]:
                    if constraint["kind"] == "enum":
                        return set(constraint["values"])
        raise AssertionError(field_id)

    selected: dict[str, set[str]] = {"isaac_profile": set(), "mock_profile": set()}
    for row in _profiles().values():
        for key, values in selected.items():
            if key in row.get("ros2_control_runtime", {}):
                values.add(row["ros2_control_runtime"][key])
    assert selected["isaac_profile"] <= enum("isaac_profile")
    assert selected["mock_profile"] <= enum("mock_profile")
    # Isaac: arm-only robot rows and tool rows; the enum lists exactly these
    # (the combined ids are Mock test fixtures only, D2).
    table = (ROOT / "manyforge_roboplan/src/retained_isaac_ros2_control_profile.cpp").read_text()
    assert '{"lite6", "IsaacLite6TopicBasedSystem", 8.0, 1.5, nullptr}' in table
    assert '{"ur10e", "IsaacUR10eTopicBasedSystem", 10.0, 0.5, nullptr}' in table
    assert '{"lite_gripper", "", 0.0, 0.0, &kLiteGripperFacts}' in table
    assert '{"robotiq_2f_140", "", 0.0, 0.0, &kRobotiq2F140Facts}' in table
    assert enum("isaac_profile") == {"lite6", "lite_gripper", "robotiq_2f_140", "ur10e"}
    assert "lite6_lite_gripper" not in table and "ur10e_robotiq_2f_140" not in table
    # Decoder parity: the C++ Deployment decoder accepts exactly the enum's
    # Mock and Isaac values (B1: a value only in the enum fails Apply).
    decoder = (
        ROOT / "manyforge_deployment/src/ros2_control_execution_runtime_unit_internal.cpp"
    ).read_text()
    for selector, field_id in (("mock", "mock_profile"), ("isaac", "isaac_profile")):
        accepted = set(re.findall(rf'\*{selector} != "([a-z0-9_]+)"', decoder))
        assert accepted == enum(field_id), field_id


def _walk(value):
    if isinstance(value, dict):
        yield value
        for item in value.values():
            yield from _walk(item)
    elif isinstance(value, list):
        for item in value:
            yield from _walk(item)


@pytest.mark.parametrize("name", ARMS + TOOLS)
def test_descriptor_points_at_the_split_model(name: str) -> None:
    descriptor = yaml.safe_load((MODELS / name / "component.yaml").read_text())
    assert descriptor["kind"] == ("robot" if name in ARMS else "tool")
    assert descriptor["id"].split(".", 1)[1] == name
    assert (MODELS / name / descriptor["model"]["urdf"]).is_file()
    assert (MODELS / name / descriptor["model"]["srdf"]).is_file()
    if name in TOOLS:
        assert descriptor["mount"]["frame"] == f"{name}_mount"
        frames = {row["frame_role_id"]: row["source_link_name"]
                  for row in _families()[name]["semantics"]["frame_role_bindings"]}
        assert descriptor["tcps"][0]["frame"] == frames["tcp"]


def test_uf850_mock_scenario_uses_the_split_robot_and_tool() -> None:
    # Item 47 (UF850 runs on Mock): the arm and its xArm gripper as two
    # components, mounted flange -> mount, each on its own Mock row.
    project = ROOT / "examples" / "uf850_xarm_gripper_mock.scenario"
    deployment = json.loads((project / "deployment.yaml").read_text())
    assignment = json.loads((project / "assignment.yaml").read_text())
    profiles, families = _profiles(), _families()
    assert deployment["attachments"] == [{
        "child": {"component_id": "xarm_gripper", "interface": "mount"},
        "parent": {"component_id": "uf850_robot", "interface": "flange"},
    }]
    components = {row["component_id"]: row for row in deployment["components"]}
    assert set(components) == {"uf850_robot", "xarm_gripper"}
    assert "root_pose_in_cell" not in components["xarm_gripper"]
    for component_id, family_id in (("uf850_robot", "uf850"), ("xarm_gripper", "xarm_gripper")):
        component = components[component_id]
        profile = profiles[component["component_profile_id"]]
        assert profile["model_family_id"] == family_id
        runtime = component["configuration"]["runtime_configuration"]
        assert runtime["mock_profile"] == profile["ros2_control_runtime"]["mock_profile"] == family_id
        source = families[family_id]["source"]
        assert component["model_source"]["urdf"]["project_path"] == "../../" + source["urdf_path"]
        assert component["model_source"]["srdf"]["project_path"] == "../../" + source["srdf_path"]
        owners = {row["controllable_component_id"] for row in runtime["component_streams"]}
        assert owners == {component_id}
    robot = components["uf850_robot"]["configuration"]["runtime_configuration"]
    assert "gripper_target_configurations" not in robot
    assert [row["stream_id"] for row in robot["component_streams"]] == ["measured_joints"]
    tool = components["xarm_gripper"]["configuration"]["runtime_configuration"]
    (gripper,) = tool["gripper_target_configurations"]
    assert gripper["controllable_component_id"] == "xarm_gripper"
    assert gripper["mount_frame"] == "xarm_gripper_mount"
    assert sorted(row["stream_id"] for row in tool["component_streams"]) == [
        "gripper_feedback", "measured_joints"]
    roles = {row["component_role_id"]: row["controllable_component_id"]
             for row in assignment["component_roles"]}
    assert roles == {"robot": "uf850_robot", "tool": "xarm_gripper"}


def test_lite6_stop_contract_check_selects_the_lite6_family() -> None:
    # The generator's Lite6 Stop-contract check keys on a model family: it must
    # name the family the Lite6 profiles select, or it silently never runs.
    source = (
        ROOT / "manyforge_deployment/tools/generate_maintained_deployment_occurrences.py"
    ).read_text()
    [family] = re.findall(
        r'^LITE6_NATIVE_MODEL_FAMILY_ID: Final = "([a-z0-9_]+)"$', source, re.MULTILINE
    )
    profiles = [
        row for row in _description()["runtime_restoration_profiles"]
        if row["runtime_restoration_profile_id"].startswith("lite6_")
    ]
    assert profiles
    assert {row["model_family_id"] for row in profiles} == {family}
