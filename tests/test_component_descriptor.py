"""`manyforge component`: descriptors, check, new, register and the curated list."""

from __future__ import annotations

import copy
import json
import shutil
import sys
from pathlib import Path

import pytest
import yaml

from scripts.components import cli, descriptor

ROOT = Path(__file__).resolve().parents[1]
ROBOTS = {
    "lite6": "ufactory.lite6",
    "uf850": "ufactory.uf850",
    "ur10e": "universal_robots.ur10e",
    "ur5e": "universal_robots.ur5e",
}
TOOLS = {
    "lite_gripper": "ufactory.lite_gripper",
    "xarm_gripper": "ufactory.xarm_gripper",
    "robotiq_2f_140": "robotiq.robotiq_2f_140",
    "robotiq_2f_85": "robotiq.robotiq_2f_85",
}
RUNNABLE = tuple(ROBOTS)
IO = ROOT / "components/io_device/manyforge.mock_pneumatic_gripper_io"


def _robot(family: str, root: Path = ROOT) -> Path:
    return root / descriptor.ROBOT_ROOT / family


def _tool(name: str, root: Path = ROOT) -> Path:
    return root / descriptor.ROBOT_ROOT / name


def _codes(report: descriptor.Report) -> list[str]:
    return [error.code for error in report.errors]


def _workspace(tmp_path: Path, *families: str) -> Path:
    """A minimal workspace: the rows, the templates and some robots."""
    root = tmp_path / "ws"
    files = [descriptor.DESCRIPTION]
    for family in families:
        files += [
            f"{descriptor.MODEL_ASSETS}/{family}.model_asset.json",
            f"{descriptor.COMPONENT_TYPES}/{family}.component.json",
        ]
    for relative in files:
        (root / relative).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(ROOT / relative, root / relative)
    shutil.copytree(ROOT / descriptor.TEMPLATES, root / descriptor.TEMPLATES)
    for family in families:
        shutil.copytree(_robot(family), _robot(family, root))
        source = next(
            row["source"] for row in json.loads(
                (ROOT / descriptor.DESCRIPTION).read_text(encoding="utf-8")
            )["model_families"] if row["model_family_id"] == family
        )
        mesh_root = ROOT / source["resource_root"]
        target_root = root / source["resource_root"]
        if mesh_root != _robot(family) and not target_root.exists():
            shutil.copytree(mesh_root, target_root)
    return root


def _drop_family_rows(path: Path, family: str) -> None:
    """Cut a family's model family row out of the description text; every
    other byte stays (register writes only the rows it changes)."""
    text = path.read_text(encoding="utf-8")
    decoder = json.JSONDecoder()
    for key, id_key, ids in (
        ("model_families", "model_family_id", {family}),
    ):
        index = text.index(f'"{key}": [') + len(f'"{key}": [')
        while True:
            separator = index
            while text[index] in " \n,":
                index += 1
            if text[index] == "]":
                break
            row, end = decoder.raw_decode(text, index)
            if row[id_key] not in ids:
                index = end
            elif text[separator] == ",":
                text = text[:separator] + text[end:]
                index = separator
            else:
                following = end
                while text[following] in " \n,":
                    following += 1
                text = text[:index] + text[following:]
    path.write_text(text, encoding="utf-8")


def _edit(folder: Path, change) -> None:
    path = folder / descriptor.DESCRIPTOR
    data = yaml.safe_load(path.read_text(encoding="utf-8"))
    change(data)
    path.write_text(yaml.safe_dump(data, sort_keys=False), encoding="utf-8")


# --- curated descriptors --------------------------------------------------


@pytest.mark.parametrize("family", RUNNABLE)
def test_curated_robots_agree_with_their_model_and_rows(family: str) -> None:
    report, loaded = descriptor.check(ROOT, _robot(family))
    assert report.errors == []
    assert loaded is not None and loaded.id == ROBOTS[family]
    assert loaded.name == family
    # The vendor TCP limit is the user's datasheet value; check says so.
    assert any("from the robot's datasheet" in note and "your responsibility" in note
               for note in report.notes)


@pytest.mark.parametrize("name", TOOLS)
def test_curated_tools_agree_with_their_model_and_rows(name: str) -> None:
    report, loaded = descriptor.check(ROOT, _tool(name))
    assert report.errors == []
    assert loaded is not None and loaded.id == TOOLS[name] and loaded.kind == "tool"


def test_curated_io_device_passes() -> None:
    report, loaded = descriptor.check(ROOT, IO)
    assert report.errors == []
    assert loaded is not None and loaded.kind == "io_device"


def _scenario(root: Path, family: str, speed: float = 0.15) -> Path:
    scenario = root / f"{family}.scenario"
    scenario.parent.mkdir(parents=True, exist_ok=True)
    scenario.mkdir()
    robot = yaml.safe_load((_robot(family) / descriptor.DESCRIPTOR).read_text(encoding="utf-8"))
    joints = robot["roles"]["joints"]["arm_joints"]
    deployment = {
        "components": [{
            "component_id": f"{family}_robot",
            "component_profile_id": ROBOTS[family],
            "lane": "mock",
            "configuration": {
                "runtime_configuration": {
                    "initial_joint_positions_rad": [0.0] * len(joints),
                    "component_streams": [{"stream_id": "measured_joints"}],
                },
                "default_fault_stop_profile": {
                    "per_joint_max_deceleration_rad_s2": {joint: 1.0 for joint in joints},
                },
            },
            "target_settings": {
                "arm": {
                    "controlled_stop": {
                        "per_joint_limits": [{"source_joint_name": joint}
                                              for joint in joints],
                    },
                    "maximum_tcp_linear_speed_mps": speed,
                },
            },
        }],
    }
    (scenario / "deployment.yaml").write_text(yaml.safe_dump(deployment), encoding="utf-8")
    return scenario


@pytest.mark.parametrize("family", RUNNABLE)
def test_arm_only_scenario_matches_robot_descriptor(tmp_path: Path, family: str) -> None:
    root = _workspace(tmp_path, family)
    report, _loaded = descriptor.check(root, _robot(family, root), _scenario(tmp_path, family))
    assert report.errors == []


def test_machine_arm_scenario_takes_its_owners_lane(tmp_path: Path) -> None:
    # A member names no lane: it runs on its owner's.
    root = _workspace(tmp_path, "lite6")
    scenario = _scenario(tmp_path, "lite6")
    path = scenario / "deployment.yaml"
    deployment = yaml.safe_load(path.read_text(encoding="utf-8"))
    arm = deployment["components"][0]
    del arm["lane"]
    arm.update(owner="lite6", role="arm")
    owner = {"component_id": "lite6", "component_profile_id": "ufactory.lite6_machine",
             "configuration": {}, "lane": "mock"}
    deployment["components"].insert(0, owner)
    path.write_text(yaml.safe_dump(deployment), encoding="utf-8")
    assert descriptor.check(root, _robot("lite6", root), scenario)[0].errors == []
    owner["lane"] = "plc"
    path.write_text(yaml.safe_dump(deployment), encoding="utf-8")
    assert "component_lane_invalid" in _codes(descriptor.check(root, _robot("lite6", root),
                                                               scenario)[0])


@pytest.mark.parametrize("family", ROBOTS)
def test_register_on_a_curated_robot_writes_nothing(tmp_path: Path, family: str) -> None:
    root = _workspace(tmp_path, family)
    before = {path: (root / path).read_bytes() for path in (descriptor.DESCRIPTION,)}
    report, written = descriptor.register(root, _robot(family, root))
    assert report.errors == [] and written == []
    assert {path: (root / path).read_bytes() for path in before} == before


def test_register_writes_the_shipped_family_row_from_the_descriptor(tmp_path: Path) -> None:
    # Remove UF850's family row, register its descriptor: the row comes back
    # byte for byte. Its lane rows stay in the descriptor (no profile or
    # controller rows are written).
    root = _workspace(tmp_path, "uf850")
    description_path = root / descriptor.DESCRIPTION
    original = json.loads(description_path.read_text(encoding="utf-8"))["model_families"]
    _drop_family_rows(description_path, "uf850")

    report, _loaded = descriptor.check(root, _robot("uf850", root))
    assert "component_not_registered" in _codes(report)

    report, written = descriptor.register(root, _robot("uf850", root))
    assert report.errors == []
    assert written == [f"{descriptor.DESCRIPTION}: model family row uf850"]
    families = json.loads(description_path.read_text(encoding="utf-8"))["model_families"]
    assert families == original
    report, _loaded = descriptor.check(root, _robot("uf850", root))
    assert report.errors == []


def test_check_and_register_agree_that_a_reordered_row_matches(tmp_path: Path) -> None:
    # Binding order is not a change: check passes and register writes nothing.
    root = _workspace(tmp_path, "lite6")
    path = root / descriptor.DESCRIPTION
    text = path.read_text(encoding="utf-8")
    row = next(row for row in json.loads(text)["model_families"]
               if row["model_family_id"] == "lite6")
    row["semantics"]["frame_role_bindings"].reverse()
    path.write_text(descriptor._with_json_row(text, "model_families", "model_family_id", row),
                    encoding="utf-8")
    reordered = path.read_bytes()
    assert descriptor.check(root, _robot("lite6", root))[0].errors == []
    report, written = descriptor.register(root, _robot("lite6", root))
    assert report.errors == [] and written == []
    assert path.read_bytes() == reordered


def test_mock_row_needs_its_controllers_file(tmp_path: Path) -> None:
    root = _workspace(tmp_path, "ur10e")
    folder = _robot("ur10e", root)
    (folder / "mock_controllers.yaml").unlink()
    report, _loaded = descriptor.check(root, folder)
    assert _codes(report) == ["component_lane_invalid"]
    assert "controllers_file 'mock_controllers.yaml' is not a file" in report.errors[0].message


def test_curated_mock_rows_name_their_adapter_and_controllers() -> None:
    for name in (*ROBOTS, *TOOLS):
        folder = _robot(name)
        data = yaml.safe_load((folder / descriptor.DESCRIPTOR).read_text(encoding="utf-8"))
        row = data["lanes"]["mock"]
        assert row["adapter"] == "mock"
        assert ("arrival_tolerance_rad" in row) == (name in ROBOTS)
        assert ("runtime_scale" in row) == (name in ROBOTS)
        if "vendor" in row:
            continue
        controllers = yaml.safe_load((folder / row["controllers_file"]).read_text())
        assert "/**/controller_manager" in controllers and "/**/__controller__" in controllers


def test_ufactory_arms_run_the_xarm_controllers_on_mock_and_isaac() -> None:
    # Plan "Vendor driver configuration": the vendor base plus ManyForge's
    # additions on every lane; no copied per-model controllers file.
    for name in ("lite6", "uf850"):
        lanes = yaml.safe_load((_robot(name) / descriptor.DESCRIPTOR).read_text())["lanes"]
        for lane in ("mock", "isaac"):
            if lane not in lanes:
                continue
            row = lanes[lane]
            assert row["vendor"] == "xarm" and "controllers_file" not in row, (name, lane)
            assert row["vendor_model"] == lanes["real"]["vendor_model"]
            assert row["controller"] == lanes["real"]["controller"]
        assert not (_robot(name) / "mock_controllers.yaml").exists()


def test_mock_row_of_a_vendor_model_needs_no_controllers_file(tmp_path: Path) -> None:
    root = _workspace(tmp_path, "lite6")
    report, _loaded = descriptor.check(root, _robot("lite6", root))
    assert "component_lane_invalid" not in _codes(report)


def test_ur_mock_controllers_keep_the_ur_values() -> None:
    controllers = yaml.safe_load((_robot("ur10e") / "mock_controllers.yaml").read_text())
    manager = controllers["/**/controller_manager"]["ros__parameters"]
    controller = controllers["/**/__controller__"]["ros__parameters"]
    assert manager["update_rate"] == 100
    assert controller["state_publish_rate"] == 100.0
    assert controller["constraints"] == {
        "goal_time": 0.0, "stopped_velocity_tolerance": 0.2, "decelerate_on_cancel": True,
        "__joint__": {"trajectory": 0.2, "max_deceleration_on_cancel": None}}


def test_gripper_mock_controllers_use_stock_defaults() -> None:
    controllers = yaml.safe_load((_tool("lite_gripper") / "mock_controllers.yaml").read_text())
    assert controllers["/**/__controller__"]["ros__parameters"] == {
        "joint": "__gripper_joint__", "goal_tolerance": 0.01,
        "stall_velocity_threshold": 0.001, "stall_timeout": 1.0, "allow_stalling": False}


# --- clear errors ---------------------------------------------------------


def _set(path: str, value):
    def change(data):
        *parents, last = path.split(".")
        target = data
        for key in parents:
            target = target[int(key)] if isinstance(target, list) else target[key]
        if value is None:
            del target[last]
        else:
            target[last] = value
    return change


@pytest.mark.parametrize(
    ("change", "code", "text"),
    [
        # The TCP named like the frame role stays on its link (else the TCP check fires).
        (lambda data: (_set("roles.frames.flange", "link_tcpx")(data),
                       _set("tcps.0.frame", "link_tcpx")(data)),
         "component_role_unknown_link",
         "roles.frames.flange: link 'link_tcpx' is not in lite6.urdf"),
        (_set("roles.joints.arm_joints", ["joint_eef", "joint2", "joint3", "joint4",
                                         "joint5", "joint6"]),
         "component_joint_invalid", "joint 'joint_eef' is fixed, not a commanded joint"),
        (_set("roles.groups.arm", "lite7"), "component_group_unknown",
         "group 'lite7' is not in lite6.srdf"),
        (_set("lanes.mock.controller", "not a name"),
         "component_manifest_invalid", "lanes.mock.controller: must be a lower-case portable id"),
        (_set("lanes.mock.arrival_tolerance_rad", None), "component_lane_invalid",
         "a robot's row needs arrival_tolerance_rad"),
        (_set("lanes.mock.runtime_scale", None), "component_lane_invalid",
         "lanes.mock.runtime_scale: a robot's row declares runtime_scale"),
        (_set("lanes.mock.runtime_scale", "partial"), "component_lane_invalid",
         "lanes.mock.runtime_scale: a robot's row declares runtime_scale"),
        (_set("lanes.mock.vendor", None), "component_lane_invalid",
         "a Mock row needs a vendor or a controllers_file"),
        (_set("lanes.kuka", {"adapter": "kuka", "controller": "c"}),
         "component_manifest_invalid", "lanes: has unknown fields kuka"),
        (_set("extra", 1), "component_manifest_invalid", "has unknown fields extra"),
        (_set("version", "1.0"), "component_manifest_invalid", "MAJOR.MINOR.PATCH"),
        (_set("id", "Lite6"), "component_manifest_invalid", "must be <vendor>.<name>"),
        (_set("limits", None), "component_manifest_invalid", "is missing limits"),
        (_set("tcps", [{"id": "tool", "frame": "link_tcpx"}]), "component_role_unknown_link",
         "tcps[0].frame: link 'link_tcpx' is not in lite6.urdf"),
        (_set("defaults.initial_joint_positions_rad", [0.0] * 5),
         "component_joint_count_mismatch", "has 5 values for 6 arm joints"),
        (_set("defaults.maximum_tcp_linear_speed_mps", 0.9), "component_limit_exceeded",
         "0.9 exceeds limits.max_tcp_linear_speed_mps 0.5"),
    ],
)
def test_broken_descriptors_give_one_clear_line(
    tmp_path: Path, change, code: str, text: str
) -> None:
    root = _workspace(tmp_path, "lite6")
    folder = _robot("lite6", root)
    _edit(folder, change)
    report, _loaded = descriptor.check(root, folder)
    assert code in _codes(report)
    line = next(error.line() for error in report.errors if error.code == code)
    assert text in line
    assert line.startswith("error: ") and line.endswith(f"[{code}]") and "\n" not in line


def test_duplicate_keys_and_bad_yaml_are_refused(tmp_path: Path) -> None:
    root = _workspace(tmp_path, "lite6")
    path = _robot("lite6", root) / descriptor.DESCRIPTOR
    text = path.read_text(encoding="utf-8")
    path.write_text(text.replace("version: 1.0.0\n", "version: 1.0.0\nversion: 1.0.1\n"),
                    encoding="utf-8")
    report, _loaded = descriptor.check(root, path.parent)
    assert _codes(report) == ["component_manifest_invalid"]
    assert "duplicate key 'version'" in report.errors[0].message
    path.write_text("kind: [robot\n", encoding="utf-8")
    report, _loaded = descriptor.check(root, path.parent)
    assert _codes(report) == ["component_manifest_invalid"]
    assert "\n" not in report.errors[0].line()


def test_scenario_tcp_ceiling_above_the_robot_limit(tmp_path: Path) -> None:
    root = _workspace(tmp_path, "lite6")
    scenario = _scenario(tmp_path, "lite6", speed=0.75)
    report, _loaded = descriptor.check(root, _robot("lite6", root), scenario)
    assert _codes(report) == ["component_limit_exceeded"]
    assert "0.75 exceeds limits.max_tcp_linear_speed_mps 0.5" in report.errors[0].message


def test_tool_gripper_states_are_checked_separately_from_the_robot(tmp_path: Path) -> None:
    folder = tmp_path / "lite_gripper"
    shutil.copytree(_tool("lite_gripper"), folder)
    _edit(folder, _set("gripper.states.closed", "shut"))
    report, _loaded = descriptor.check(ROOT, folder)
    assert "component_state_unknown" in _codes(report)
    assert "'shut' is not a group_state of SRDF group 'lite_gripper'" in report.errors[0].message


def test_io_signals_are_checked(tmp_path: Path) -> None:
    folder = tmp_path / "io"
    shutil.copytree(IO, folder)
    _edit(folder, lambda data: data["signals"][0]["provider"].update(profile="mock"))
    _edit(folder, lambda data: data["signals"][1].pop("update_mode"))
    report, _loaded = descriptor.check(ROOT, folder)
    lines = [error.line() for error in report.errors]
    assert any("signals[0].provider.profile: must be one of mock_output" in line
               for line in lines)
    assert any("signals[1]: is missing update_mode" in line for line in lines)


# --- tools: register and mount -------------------------------------------


def _remove_rows(root: Path, family: str) -> None:
    """Drop a family's rows from the maintained description."""
    _drop_family_rows(root / descriptor.DESCRIPTION, family)


@pytest.mark.parametrize("name", TOOLS)
def test_register_on_a_curated_tool_writes_nothing(tmp_path: Path, name: str) -> None:
    root = _workspace(tmp_path, name)
    before = {path: (root / path).read_bytes() for path in (descriptor.DESCRIPTION,)}
    report, written = descriptor.register(root, _tool(name, root))
    assert report.errors == [] and written == []
    assert {path: (root / path).read_bytes() for path in before} == before


def test_register_writes_the_shipped_tool_family_row(tmp_path: Path) -> None:
    root = _workspace(tmp_path, "xarm_gripper")
    original = json.loads((root / descriptor.DESCRIPTION).read_text())["model_families"]
    _remove_rows(root, "xarm_gripper")
    report, _loaded = descriptor.check(root, _tool("xarm_gripper", root))
    assert "component_not_registered" in _codes(report)
    report, written = descriptor.register(root, _tool("xarm_gripper", root))
    assert report.errors == []
    assert written == [f"{descriptor.DESCRIPTION}: model family row xarm_gripper"]
    assert json.loads((root / descriptor.DESCRIPTION).read_text())["model_families"] == original
    report, written = descriptor.register(root, _tool("xarm_gripper", root))
    assert report.errors == [] and written == []


def test_the_sources_checker_accepts_what_register_writes(tmp_path: Path) -> None:
    # One definition of a valid row: the maintained-sources checker accepts
    # the description register writes.
    sys.path.insert(0, str(ROOT / "manyforge_deployment" / "tools"))
    try:
        import check_maintained_deployment_sources as checker  # noqa: PLC0415
    finally:
        sys.path.pop(0)
    root = _workspace(tmp_path, "xarm_gripper")
    _remove_rows(root, "xarm_gripper")
    report, written = descriptor.register(root, _tool("xarm_gripper", root))
    assert report.errors == [] and written
    checker.validate(root / descriptor.DESCRIPTION, ROOT)


@pytest.mark.parametrize(
    ("change", "code", "text"),
    [
        (_set("lanes.mock.arrival_tolerance_rad", 0.01),
         "component_lane_invalid", "a tool's row has none"),
        (_set("lanes.mock.runtime_scale", "supported"),
         "component_lane_invalid", "lanes.mock.runtime_scale: a tool's row has none"),
        (_set("lanes.mock.controller", None),
         "component_manifest_invalid", "lanes.mock: is missing controller"),
        (_set("gripper.command_duration_s", None), "component_manifest_invalid",
         "a pneumatic gripper needs command_duration_s"),
        (_set("gripper.touch_links", None), "component_manifest_invalid",
         "gripper: is missing touch_links"),
        (_set("model.srdf", None), "component_manifest_invalid", "model: is missing srdf"),
        (_set("mount.frame", "lite_gripper_mountx"), "component_role_unknown_link",
         "mount.frame: link 'lite_gripper_mountx' is not in lite_gripper.urdf"),
    ],
)
def test_broken_tool_descriptors_give_one_clear_line(
    tmp_path: Path, change, code: str, text: str
) -> None:
    root = _workspace(tmp_path, "lite_gripper")
    folder = _tool("lite_gripper", root)
    _edit(folder, change)
    report, _loaded = descriptor.check(root, folder)
    assert code in _codes(report)
    line = next(error.line() for error in report.errors if error.code == code)
    assert text in line
    assert line.startswith("error: ") and line.endswith(f"[{code}]") and "\n" not in line


def test_new_tool_check_and_register(tmp_path: Path) -> None:
    # `new tool`, then the model files and descriptor of a gripper: check lists
    # only the steps left, register writes the rows, then only the generated
    # catalog files and the schema id remain.
    root = _workspace(tmp_path, "lite_gripper")
    report, folder = descriptor.new_component(root, "tool", "acme.clamp")
    assert report.errors == [] and folder == _tool("clamp", root)
    template = yaml.safe_load((folder / descriptor.DESCRIPTOR).read_text(encoding="utf-8"))
    assert template["kind"] == "tool" and template["id"] == "acme.clamp"
    assert template["model"] == {"urdf": "clamp.urdf", "srdf": "clamp.srdf"}
    assert "arrival_tolerance_rad" not in template["lanes"]["mock"]
    assert "runtime_scale" not in template["lanes"]["mock"]
    source = _tool("lite_gripper", root)
    for suffix in ("urdf", "srdf"):
        shutil.copy(source / f"lite_gripper.{suffix}", folder / f"clamp.{suffix}")
    filled = yaml.safe_load((source / descriptor.DESCRIPTOR).read_text(encoding="utf-8"))
    filled.update(id="acme.clamp", title="Acme clamp",
                  model={"urdf": "clamp.urdf", "srdf": "clamp.srdf"})
    shutil.copy(source / "mock_controllers.yaml", folder / "mock_controllers.yaml")
    (folder / descriptor.DESCRIPTOR).write_text(yaml.safe_dump(filled, sort_keys=False),
                                                encoding="utf-8")
    report, _loaded = descriptor.check(root, folder)
    assert set(_codes(report)) == {"component_not_registered", "component_generated_missing"}

    report, written = descriptor.register(root, folder)
    assert report.errors == []
    assert written == [f"{descriptor.DESCRIPTION}: model family row clamp"]
    description = json.loads((root / descriptor.DESCRIPTION).read_text(encoding="utf-8"))
    family = next(row for row in description["model_families"]
                  if row["model_family_id"] == "clamp")
    semantics = family["semantics"]
    assert semantics["root_frame_role_id"] == "mount"
    assert {row["frame_role_id"]: row["source_link_name"]
            for row in semantics["frame_role_bindings"]} == {
        "mount": "lite_gripper_mount", "tcp": "link_tcp"}
    assert [row["resource_role_id"] for row in semantics["resource_role_bindings"]] == [
        "fingers"]
    assert family["source"]["urdf_path"] == f"{descriptor.ROBOT_ROOT}/clamp/clamp.urdf"
    assert family["source"]["resource_root"] == f"{descriptor.ROBOT_ROOT}/lite6_lite_gripper"
    report, _loaded = descriptor.check(root, folder)
    assert set(_codes(report)) == {"component_generated_missing"}
    assert descriptor.register(root, folder)[1] == []


def _cup_tool(root: Path, tcps: list[dict]) -> Path:
    """A lane-free copy of the Lite gripper, `acme.cup`, with the given TCPs."""
    report, folder = descriptor.new_component(root, "tool", "acme.cup")
    assert report.errors == [] and folder is not None
    source = _tool("lite_gripper", root)
    for suffix in ("urdf", "srdf"):
        shutil.copy(source / f"lite_gripper.{suffix}", folder / f"cup.{suffix}")
    data = yaml.safe_load((source / descriptor.DESCRIPTOR).read_text(encoding="utf-8"))
    data.update(id="acme.cup", title="Acme cup gripper",
                model={"urdf": "cup.urdf", "srdf": "cup.srdf"}, tcps=tcps)
    del data["lanes"]
    (folder / descriptor.DESCRIPTOR).write_text(yaml.safe_dump(data, sort_keys=False),
                                                encoding="utf-8")
    return folder


def _family(root: Path, family_id: str) -> dict:
    description = json.loads((root / descriptor.DESCRIPTION).read_text(encoding="utf-8"))
    return next(row for row in description["model_families"]
                if row["model_family_id"] == family_id)


def test_tool_tcps_keep_their_ids_and_the_first_is_the_default(tmp_path: Path) -> None:
    # Each descriptor TCP is a frame role under its own id; the family row
    # records them, the default first. No `tcp` role is invented.
    root = _workspace(tmp_path, "lite_gripper")
    folder = _cup_tool(root, [{"id": "cup_center", "frame": "link_tcp"},
                              {"id": "tip", "frame": "lite_gripper_base_link"}])
    report, written = descriptor.register(root, folder)
    assert report.errors == []
    assert written == [f"{descriptor.DESCRIPTION}: model family row cup"]
    family = _family(root, "cup")
    assert {row["frame_role_id"]: row["source_link_name"]
            for row in family["semantics"]["frame_role_bindings"]} == {
        "cup_center": "link_tcp", "mount": "lite_gripper_mount",
        "tip": "lite_gripper_base_link"}
    assert family["semantics"]["root_frame_role_id"] == "mount"
    assert family["tcp_frame_role_ids"] == ["cup_center", "tip"]


def test_tool_tcps_check_and_register_round_trip(tmp_path: Path) -> None:
    # check agrees with what register wrote; a changed default (TCP order) is
    # stale until register rewrites the row; then both agree again.
    root = _workspace(tmp_path, "lite_gripper")
    folder = _cup_tool(root, [{"id": "cup_center", "frame": "link_tcp"},
                              {"id": "tip", "frame": "lite_gripper_base_link"}])
    assert descriptor.register(root, folder)[0].errors == []
    report, _loaded = descriptor.check(root, folder)
    assert set(_codes(report)) == {"component_generated_missing"}
    assert descriptor.register(root, folder)[1] == []

    _edit(folder, lambda data: data["tcps"].reverse())
    report, _loaded = descriptor.check(root, folder)
    assert "component_registration_stale" in _codes(report)
    assert any("tcp_frame_role_ids" in error.message for error in report.errors)
    report, written = descriptor.register(root, folder)
    assert report.errors == []
    assert written == [f"{descriptor.DESCRIPTION}: model family row cup"]
    assert _family(root, "cup")["tcp_frame_role_ids"] == ["tip", "cup_center"]
    report, _loaded = descriptor.check(root, folder)
    assert set(_codes(report)) == {"component_generated_missing"}

    # Back to one TCP named `tcp`: the row no longer lists its TCPs.
    _edit(folder, lambda data: data.update(tcps=[{"id": "tcp", "frame": "link_tcp"}]))
    report, written = descriptor.register(root, folder)
    assert report.errors == [] and written
    family = _family(root, "cup")
    assert "tcp_frame_role_ids" not in family
    assert {row["frame_role_id"] for row in family["semantics"]["frame_role_bindings"]} == {
        "mount", "tcp"}


def test_a_single_tcp_tool_registers_as_before(tmp_path: Path) -> None:
    # A tool whose only TCP is `tcp` gets the same family row as before this
    # change: frame roles `mount` and `tcp`, no TCP list.
    root = _workspace(tmp_path, "lite_gripper")
    folder = _cup_tool(root, [{"id": "tcp", "frame": "link_tcp"}])
    assert descriptor.register(root, folder)[0].errors == []
    family = _family(root, "cup")
    assert "tcp_frame_role_ids" not in family
    assert sorted(family) == [
        "catalog_source", "component_descriptor", "model_asset_id", "model_family_id",
        "semantics", "source"]
    assert family["component_descriptor"] == f"{descriptor.ROBOT_ROOT}/cup/{descriptor.DESCRIPTOR}"
    assert {row["frame_role_id"]: row["source_link_name"]
            for row in family["semantics"]["frame_role_bindings"]} == {
        "mount": "lite_gripper_mount", "tcp": "link_tcp"}


def test_robot_tcps_keep_their_ids_and_the_first_is_the_default(tmp_path: Path) -> None:
    # D1: a robot's own default TCP is its first `tcps` entry. A TCP id that
    # is also a `roles.frames` role on the same link is that role.
    root = _workspace(tmp_path, "lite6")
    folder = _robot("lite6", root)
    _edit(folder, lambda data: data.update(tcps=[{"id": "pen", "frame": "link6"},
                                                 {"id": "flange", "frame": "link_eef"}]))
    report = descriptor.Report()
    loaded = descriptor.load_descriptor(root, folder, report)
    assert report.errors == [] and loaded is not None
    row = descriptor.family_row(root, loaded)
    frames = {binding["frame_role_id"]: binding["source_link_name"]
              for binding in row["semantics"]["frame_role_bindings"]}
    assert frames == {"base": "link_base", "flange": "link_eef", "pen": "link6",
                      "root": "world"}
    assert row["semantics"]["root_frame_role_id"] == "root"
    assert row["tcp_frame_role_ids"] == ["pen", "flange"]


@pytest.mark.parametrize(
    ("family", "tcps", "role_link"),
    [
        # a robot `roles.frames` role
        ("lite6", [{"id": "flange", "frame": "link6"}], "link_eef"),
        # a robot's `root` role (no frame role on its root link)
        ("lite6", [{"id": "flange", "frame": "link_eef"}, {"id": "root", "frame": "link6"}],
         "world"),
        # a tool's `mount`
        ("lite_gripper", [{"id": "tcp", "frame": "link_tcp"},
                          {"id": "mount", "frame": "link_tcp"}], "lite_gripper_mount"),
    ],
)
def test_a_tcp_named_like_a_frame_role_on_another_link_is_refused(
    tmp_path: Path, family: str, tcps: list[dict], role_link: str
) -> None:
    # Any name, avoiding conflicts: check and register refuse it, naming both links.
    root = _workspace(tmp_path, family)
    folder = _robot(family, root)
    _edit(folder, lambda data: data.update(tcps=tcps))
    index, conflict = len(tcps) - 1, tcps[-1]
    for report in (descriptor.check(root, folder)[0], descriptor.register(root, folder)[0]):
        (error,) = report.errors
        assert error.code == "component_manifest_invalid"
        assert error.where.endswith(f"tcps[{index}].id")
        assert repr(role_link) in error.message and repr(conflict["frame"]) in error.message
    assert descriptor.register(root, folder)[1] == []


def test_a_non_default_tcp_may_be_named_tcp(tmp_path: Path) -> None:
    # `tcp` is an ordinary name (no Cell alias of the default
    # TCP), so a later TCP may use it; the first stays the default.
    root = _workspace(tmp_path, "lite_gripper")
    folder = _cup_tool(root, [{"id": "cup_center", "frame": "link_tcp"},
                              {"id": "tcp", "frame": "lite_gripper_base_link"}])
    report, written = descriptor.register(root, folder)
    assert report.errors == [] and written
    assert _family(root, "cup")["tcp_frame_role_ids"] == ["cup_center", "tcp"]
    # The default named `tcp`, beside another TCP, is fine.
    _edit(folder, lambda data: data.update(tcps=[
        {"id": "tcp", "frame": "link_tcp"}, {"id": "cup_center", "frame": "lite_gripper_base_link"}]))
    report, written = descriptor.register(root, folder)
    assert report.errors == [] and written
    assert _family(root, "cup")["tcp_frame_role_ids"] == ["tcp", "cup_center"]


def test_the_sources_checker_reads_a_family_tcp_list(tmp_path: Path) -> None:
    # The shipped 2F-85 row lists its two TCPs; a list naming no frame role,
    # or an empty one, is refused.
    sys.path.insert(0, str(ROOT / "manyforge_deployment" / "tools"))
    try:
        import check_maintained_deployment_sources as checker  # noqa: PLC0415
    finally:
        sys.path.pop(0)
    assert _family(ROOT, "robotiq_2f_85")["tcp_frame_role_ids"] == ["tcp", "base"]
    checker.validate(ROOT / descriptor.DESCRIPTION, ROOT)
    for tcps in (["tcp", "nope"], [], ["tcp", "tcp"]):
        description = json.loads((ROOT / descriptor.DESCRIPTION).read_text(encoding="utf-8"))
        next(row for row in description["model_families"]
             if row["model_family_id"] == "robotiq_2f_85")["tcp_frame_role_ids"] = tcps
        path = tmp_path / "description.json"
        path.write_text(json.dumps(description), encoding="utf-8")
        with pytest.raises(checker.SourceDescriptionError, match="tcp_frame_role_ids"):
            checker.validate(path, ROOT)


def _tool_component(deployment: dict) -> dict:
    return next(row for row in deployment["components"]
                if row["component_id"] == "lite_gripper")


# The lite_gripper runtime configuration of a curated Scenario per lane: none
# is the lane-free (pneumatic) tool, unwired.
_TOOL_RUNTIME_SOURCES = {
    None: "examples/real_lite6_arm_observation.scenario/deployment.yaml",
    "mock": "examples/mock_lite6_pick_and_place.scenario/deployment.yaml",
}


def _mounted_scenario(tmp_path: Path, lane: str | None = None,
                      change=None) -> Path:
    """lite6_robot (Mock) with lite_gripper mounted on its flange, and an I/O device."""
    scenario = _scenario(tmp_path / "ws" / "examples", "lite6")
    path = scenario / "deployment.yaml"
    deployment = yaml.safe_load(path.read_text(encoding="utf-8"))
    curated = json.loads((ROOT / _TOOL_RUNTIME_SOURCES[lane]).read_text(encoding="utf-8"))
    runtime = next(
        row for row in curated["components"] if row["component_id"] == "lite_gripper"
    )["configuration"]["runtime_configuration"]
    deployment["components"] += [
        {
            "component_id": "lite_gripper",
            "component_profile_id": "ufactory.lite_gripper",
            **({} if lane is None else {"lane": lane}),
            "model_source": {
                "urdf": {"project_path": "../../manyforge_example_models/models/lite_gripper/lite_gripper.urdf"},
                "srdf": {"project_path": "../../manyforge_example_models/models/lite_gripper/lite_gripper.srdf"},
                "package_roots": {
                    "manyforge_example_models": {"project_path": "../../manyforge_example_models"},
                },
            },
            "configuration": {
                "runtime_configuration": copy.deepcopy(runtime),
                "runtime_kind": "ros2_control",
            },
            "target_settings": {},
        },
        {"component_id": "gripper_io", "component_profile_id": "io_device_v0"},
    ]
    deployment["attachments"] = [{
        "parent": {"component_id": "lite6_robot", "interface": "flange"},
        "child": {"component_id": "lite_gripper", "interface": "mount"},
    }]
    if change is not None:
        change(deployment)
    path.write_text(yaml.safe_dump(deployment), encoding="utf-8")
    return scenario


@pytest.mark.parametrize("lane", [None, "mock"])
def test_mounted_tool_scenario_matches_robot_and_tool(tmp_path: Path, lane: str | None) -> None:
    root = _workspace(tmp_path, "lite6", "lite_gripper")
    scenario = _mounted_scenario(tmp_path, lane)
    report, _loaded = descriptor.check(root, _tool("lite_gripper", root), scenario)
    assert report.errors == []
    assert any("mounted and agree" in note for note in report.notes)
    report, _loaded = descriptor.check(root, _robot("lite6", root), scenario)
    assert report.errors == []


def test_mounted_tool_scenario_accepts_absolute_model_source_paths(tmp_path: Path) -> None:
    root = _workspace(tmp_path, "lite6", "lite_gripper")
    scenario = _mounted_scenario(tmp_path)
    path = scenario / "deployment.yaml"
    deployment = yaml.safe_load(path.read_text(encoding="utf-8"))
    source = _tool_component(deployment)["model_source"]
    source["urdf"] = {"external_path": str(
        root / "manyforge_example_models/models/lite_gripper/lite_gripper.urdf"
    )}
    source["srdf"] = {"external_path": str(
        root / "manyforge_example_models/models/lite_gripper/lite_gripper.srdf"
    )}
    source["package_roots"]["manyforge_example_models"] = {
        "external_path": str(root / "manyforge_example_models")
    }
    path.write_text(yaml.safe_dump(deployment), encoding="utf-8")

    report, _loaded = descriptor.check(root, _tool("lite_gripper", root), scenario)

    assert report.errors == []


@pytest.mark.parametrize(
    ("change", "message"),
    [
        (lambda deployment: _tool_component(deployment).pop("model_source"),
         "model_source is required"),
        (lambda deployment: _tool_component(deployment)["model_source"]["urdf"].update(
            project_path="../../manyforge_example_models/models/lite_gripper/other.urdf"),
         "model_source.urdf must resolve to the registered source path"),
        (lambda deployment: _tool_component(deployment)["model_source"]["srdf"].update(
            project_path="../../manyforge_example_models/models/lite_gripper/other.srdf"),
         "model_source.srdf must resolve to the registered source path"),
        (lambda deployment: _tool_component(deployment)["model_source"]["package_roots"][
            "manyforge_example_models"].update(project_path="../../other_package"),
         "must resolve to its registered package path"),
    ],
)
def test_mounted_tool_scenario_checks_registered_model_source(
    tmp_path: Path, change, message: str,
) -> None:
    root = _workspace(tmp_path, "lite6", "lite_gripper")
    scenario = _mounted_scenario(tmp_path)
    path = scenario / "deployment.yaml"
    deployment = yaml.safe_load(path.read_text(encoding="utf-8"))
    change(deployment)
    path.write_text(yaml.safe_dump(deployment), encoding="utf-8")

    report, _loaded = descriptor.check(root, _tool("lite_gripper", root), scenario)

    assert any(message in error.message for error in report.errors)


def _second_tool_on_the_same_flange(deployment: dict) -> None:
    second = copy.deepcopy(_tool_component(deployment))
    second["component_id"] = "lite_gripper_b"
    for row in second["configuration"]["runtime_configuration"][
        "gripper_target_configurations"
    ]:
        row["controllable_component_id"] = "lite_gripper_b"
    deployment["components"].append(second)
    deployment["attachments"].append({
        "parent": {"component_id": "lite6_robot", "interface": "flange"},
        "child": {"component_id": "lite_gripper_b", "interface": "mount"},
    })


def _attachment(side: str, key: str, value: str):
    def change(deployment: dict) -> None:
        deployment["attachments"][0][side][key] = value
    return change


def _tool_runtime(key: str, value):
    def change(deployment: dict) -> None:
        _tool_component(deployment)["configuration"]["runtime_configuration"][key] = value
    return change


def _tool_gripper(key: str, value):
    def change(deployment: dict) -> None:
        runtime = _tool_component(deployment)["configuration"]["runtime_configuration"]
        runtime["gripper_target_configurations"][0][key] = value
    return change


@pytest.mark.parametrize(
    ("change", "code", "text"),
    [
        (lambda d: _tool_component(d).update(root_pose_in_cell={"position_m": [0, 0, 0]}),
         "component_attachment_invalid", "omits root_pose_in_cell"),
        (lambda d: d.update(attachments=[]),
         "component_attachment_invalid", "needs exactly one attachments row"),
        (_attachment("child", "interface", "wrist"),
         "component_attachment_invalid", "child.interface must be 'mount'"),
        (_attachment("parent", "interface", "wrist"),
         "component_attachment_invalid", "parent.interface must be 'flange', got 'wrist'"),
        (_attachment("parent", "component_id", "ghost"),
         "component_attachment_invalid", "parent 'ghost' is not a component"),
        (_attachment("parent", "component_id", "gripper_io"),
         "component_attachment_invalid", "parent 'gripper_io' has no 'flange' interface"),
        (_second_tool_on_the_same_flange, "component_attachment_invalid",
         "the flange of 'lite6_robot' is not free"),
        (_tool_runtime("component_streams", [{"stream_id": "measured_joints"}]),
         "component_scenario_mismatch",
         "component_streams must declare exactly []"),
        (_tool_runtime("gripper_target_configurations", []),
         "component_scenario_mismatch", "gripper_target_configurations is empty"),
        (_tool_gripper("mount_frame", "link_eef"),
         "component_scenario_mismatch", "mount_frame 'link_eef' must be mount.frame"),
        (_tool_gripper("controllable_component_id", "lite6_robot"),
         "component_scenario_mismatch", "a tool's gripper row names the tool"),
        (_tool_gripper("open_state_ref", "wide"),
         "component_scenario_mismatch", "open_state_ref 'wide' is not a group_state"),
    ],
)
def test_mounted_tool_scenario_mistakes_give_one_clear_line(
    tmp_path: Path, change, code: str, text: str
) -> None:
    root = _workspace(tmp_path, "lite6", "lite_gripper")
    scenario = _mounted_scenario(tmp_path, change=change)
    report, _loaded = descriptor.check(root, _tool("lite_gripper", root), scenario)
    assert code in _codes(report)
    line = next(error.line() for error in report.errors
                if error.code == code and text in error.message)
    assert line.startswith("error: ") and line.endswith(f"[{code}]") and "\n" not in line


def test_tool_scenario_without_the_tool(tmp_path: Path) -> None:
    root = _workspace(tmp_path, "lite6", "lite_gripper")
    report, _loaded = descriptor.check(
        root, _tool("lite_gripper", root), _scenario(tmp_path, "lite6")
    )
    assert _codes(report) == ["component_scenario_mismatch"]
    assert "no component selects the model ufactory.lite_gripper" in (
        report.errors[0].message
    )


# --- new ------------------------------------------------------------------


@pytest.mark.parametrize("kind", [kind for kind in descriptor.KINDS
                                  if kind not in ("simulator", "sensor", "assistant")])
def test_new_scaffolds_a_folder_from_the_template(tmp_path: Path, kind: str) -> None:
    root = _workspace(tmp_path)
    report, folder = descriptor.new_component(root, kind, "acme.r6")
    assert report.errors == [] and folder is not None
    expected = (
        _robot("r6", root) if kind in ("robot", "tool")
        else root / "components" / kind / "acme.r6"
    )
    assert folder == expected
    text = (folder / descriptor.DESCRIPTOR).read_text(encoding="utf-8")
    assert "id: acme.r6\n" in text and "__" not in text
    report, _loaded = descriptor.check(root, folder)
    if kind == "io_device":
        # The I/O template is a working Mock example.
        assert report.errors == []
    else:
        # The template names every field; each unfilled one is one clear line.
        assert report.errors
        assert all(error.code.startswith("component_") for error in report.errors)
    report, _folder = descriptor.new_component(root, kind, "acme.r6")
    assert _codes(report) == ["component_folder_exists"]


def test_new_keeps_files_already_in_the_folder(tmp_path: Path) -> None:
    root = _workspace(tmp_path)
    folder = _robot("r6", root)
    folder.mkdir(parents=True)
    (folder / "README.md").write_text("vendor notes\n", encoding="utf-8")
    report, _folder = descriptor.new_component(root, "robot", "acme.r6")
    assert report.errors == []
    assert (folder / "README.md").read_text(encoding="utf-8") == "vendor notes\n"
    assert (folder / descriptor.DESCRIPTOR).is_file()


def test_new_refuses_a_bad_id_or_kind(tmp_path: Path) -> None:
    root = _workspace(tmp_path)
    assert _codes(descriptor.new_component(root, "robot", "R6")[0]) == [
        "component_manifest_invalid"]
    assert _codes(descriptor.new_component(root, "gripper", "acme.r6")[0]) == [
        "component_manifest_invalid"]


# --- the list Composer serves ---------------------------------------------


def test_curated_list() -> None:
    listing = descriptor.list_components(ROOT)
    assert listing["schemaVersion"] == "manyforge.component_list.v1"
    entries = {entry["id"]: entry for entry in listing["components"]}
    assert set(ROBOTS.values()) | set(TOOLS.values()) | {
        "manyforge.mock_pneumatic_gripper_io"
    } <= set(entries)
    # A UFACTORY arm is placed only as its machine's member.
    lite6 = entries["ufactory.lite6"]
    assert lite6["kind"] == "robot" and not lite6["addable"] and lite6["errors"] == []
    assert lite6["path"] == "manyforge_example_models/models/lite6"
    assert lite6["descriptor"]["limits"] == {"max_tcp_linear_speed_mps": 0.5}
    assert lite6["runtime"] == {
        "modelFamilyId": "lite6",
        "componentProfileId": "ufactory.lite6",
        "lanes": ["isaac", "mock", "real"],
    }
    uf850 = entries["ufactory.uf850"]
    assert not uf850["addable"] and uf850["errors"] == []
    assert entries["universal_robots.ur10e"]["addable"]
    for tool_id in TOOLS.values():
        assert entries[tool_id]["kind"] == "tool"
        assert entries[tool_id]["addable"] and entries[tool_id]["errors"] == []
    assert entries["ufactory.lite_gripper"]["runtime"] == {
        "modelFamilyId": "lite_gripper",
        "componentProfileId": "ufactory.lite_gripper",
        "lanes": ["isaac", "lane_free", "mock"],
    }
    io = entries["manyforge.mock_pneumatic_gripper_io"]
    assert io["addable"] and io["runtime"] == {"componentProfileId": "io_device_v0"}
    assert [entry["kind"] for entry in listing["components"]] == sorted(
        entry["kind"] for entry in listing["components"]
    )


def test_list_flags_a_conflict_and_lists_invalid_folders(tmp_path: Path) -> None:
    root = _workspace(tmp_path)
    for name in ("a", "b", "c"):
        shutil.copytree(IO, root / "components/io_device" / name)
    _edit(root / "components/io_device/b", _set("title", "different bytes"))
    (root / "components/io_device/c" / descriptor.DESCRIPTOR).write_text(
        "kind: io_device\n", encoding="utf-8"
    )
    entries = descriptor.list_components(root)["components"]
    by_path = {entry["path"]: entry for entry in entries}
    for name in ("a", "b"):
        entry = by_path[f"components/io_device/{name}"]
        assert "component_package_conflict" in [error["code"] for error in entry["errors"]]
        assert not entry["addable"]
    invalid = by_path["components/io_device/c"]
    assert invalid["errors"] and not invalid["addable"] and invalid["id"] is None


def test_list_leaves_out_an_identical_copy(tmp_path: Path) -> None:
    root = _workspace(tmp_path)
    for name in ("a", "b"):
        shutil.copytree(IO, root / "components/io_device" / name)
    entries = descriptor.list_components(root)["components"]
    assert [entry["path"] for entry in entries] == ["components/io_device/a"]
    assert entries[0]["addable"]


# --- command line ---------------------------------------------------------


def test_command_line(tmp_path: Path, capsys) -> None:
    lite6 = str(_robot("lite6"))
    assert cli.main(["--workspace-root", str(ROOT), "check", lite6]) == 0
    assert capsys.readouterr().out.startswith("ok: ")
    root = _workspace(tmp_path, "lite6")
    folder = _robot("lite6", root)
    _edit(folder, _set("roles.frames.flange", "link_tcpx"))
    assert cli.main(["--workspace-root", str(root), "check", str(folder)]) == 1
    out = capsys.readouterr().out.splitlines()
    assert out and all(line.startswith("error: ") for line in out)
    assert cli.main(["--workspace-root", str(ROOT), "list", "--json"]) == 0
    assert json.loads(capsys.readouterr().out)["schemaVersion"] == descriptor.LIST_SCHEMA
    assert cli.main(["--workspace-root", str(root), "new", "io_device", "acme.valves"]) == 0
    assert "created components/io_device/acme.valves/component.yaml" in capsys.readouterr().out
    clean = _workspace(tmp_path / "clean", "lite6")
    assert cli.main(["--workspace-root", str(clean), "register",
                     str(_robot("lite6", clean))]) == 0
    assert "nothing written" in capsys.readouterr().out
    tools = _workspace(tmp_path / "tools", "lite_gripper")
    assert cli.main(["--workspace-root", str(tools), "register",
                     str(_tool("lite_gripper", tools))]) == 0
    assert "nothing written" in capsys.readouterr().out


# --- machines and model-form I/O devices (owners and members) ----------


MACHINES = {
    "ufactory.lite6_machine": ("ufactory.lite6", "ufactory.lite6_controller_io",
                               "ufactory.lite6_tool_io"),
    "ufactory.uf850_machine": ("ufactory.uf850", "ufactory.uf850_controller_io",
                               "ufactory.uf850_tool_io"),
}


@pytest.mark.parametrize("machine", sorted(MACHINES))
def test_curated_machines_and_their_io_members_pass(machine: str) -> None:
    report, loaded = descriptor.check(ROOT, ROOT / "components/machine" / machine)
    assert report.errors == [] and loaded is not None and loaded.kind == "machine"
    members = [(m["role"], m["model"]) for m in loaded.data["members"]]
    arm, controller_io, tool_io = MACHINES[machine]
    assert members == [("arm", arm), ("controller_io", controller_io), ("tool_io", tool_io)]
    # No member is required; no Mock or Isaac row.
    assert loaded.data["lanes"] == {"real": {"adapter": "xarm"}}
    for model in (controller_io, tool_io):
        report, io = descriptor.check(ROOT, ROOT / "components/io_device" / model)
        assert report.errors == [] and io is not None and descriptor.is_io_model_form(io.data)
        assert io.data["lanes"]["real"]["adapter"] == "xarm"
        assert io.data["lanes"]["real"]["input_maximum_age_ms"] == 100


def test_robot_models_declare_no_io_rows() -> None:
    for family in ("lite6", "uf850"):
        data = yaml.safe_load((_robot(family) / descriptor.DESCRIPTOR).read_text())
        assert "io" not in data["lanes"]["real"]


def test_model_form_io_device_refuses_providers_and_unknown_banks(tmp_path: Path) -> None:
    folder = tmp_path / "io"
    shutil.copytree(ROOT / "components/io_device/ufactory.lite6_tool_io", folder)
    _edit(folder, lambda data: data["signals"][0].update(provider={"profile": "mock"}))
    _edit(folder, lambda data: data["lanes"]["real"].update(bank="box"))
    _edit(folder, lambda data: data["lanes"]["real"].update(input_maximum_age_ms=0))
    lines = [error.line() for error in descriptor.check(ROOT, folder)[0].errors]
    assert any("signals[0]: has unknown fields provider" in line for line in lines)
    assert any("lanes.real.bank: must be one of controller, tool" in line for line in lines)
    assert any("lanes.real.input_maximum_age_ms: must be a positive number" in line
               for line in lines)


def test_machine_members_name_curated_member_models(tmp_path: Path) -> None:
    root = _workspace(tmp_path)
    folder = root / "components/machine/acme.cell"
    shutil.copytree(ROOT / "components/machine/ufactory.lite6_machine", folder)
    _edit(folder, _set("id", "acme.cell"))
    report, _loaded = descriptor.check(root, folder)
    lines = [error.line() for error in report.errors]
    # No curated member models in this workspace, and not registered yet.
    assert any("'ufactory.lite6' is not a curated component" in line for line in lines)
    assert any("component_not_registered" in line for line in lines)


def test_machine_member_slots_take_only_role_and_model(tmp_path: Path) -> None:
    folder = tmp_path / "machine"
    shutil.copytree(ROOT / "components/machine/ufactory.lite6_machine", folder)
    _edit(folder, lambda data: data["members"][0].update(required=True))
    lines = [error.line() for error in descriptor.check(ROOT, folder)[0].errors]
    assert any("members[0]: has unknown fields required" in line for line in lines)


def test_register_lists_a_machine_in_component_models(tmp_path: Path) -> None:
    root = _workspace(tmp_path)
    folder = root / "components/machine/acme.cell"
    shutil.copytree(ROOT / "components/machine/ufactory.lite6_machine", folder)
    _edit(folder, _set("id", "acme.cell"))
    report, written = descriptor.register(root, folder)
    listed = json.loads((root / descriptor.DESCRIPTION).read_text())["component_models"]
    assert report.errors == [] and written
    assert "components/machine/acme.cell/component.yaml" in listed and listed == sorted(listed)
    assert descriptor.register(root, folder)[1] == []


def test_curated_list_offers_machines_not_their_io_members() -> None:
    entries = {entry["id"]: entry for entry in descriptor.list_components(ROOT)["components"]}
    lite6 = entries["ufactory.lite6_machine"]
    assert lite6["kind"] == "machine" and lite6["addable"] and lite6["errors"] == []
    assert lite6["runtime"]["lanes"] == ["isaac", "mock", "real"]
    assert lite6["runtime"]["members"] == [
        {"role": "arm", "modelId": "ufactory.lite6"},
        {"role": "controller_io", "modelId": "ufactory.lite6_controller_io"},
        {"role": "tool_io", "modelId": "ufactory.lite6_tool_io"},
    ]
    io = entries["ufactory.lite6_controller_io"]
    assert not io["addable"] and io["runtime"] == {"componentProfileId": "ufactory.lite6_controller_io"}


# --- sensors (a point-cloud source) --------------------------------------

POINTCLOUD = ROOT / "components/sensor/manyforge.pointcloud_source"
ASSISTANT = ROOT / "components/assistant/manyforge.assistant"


def test_curated_pointcloud_source_passes_and_is_listed() -> None:
    report, loaded = descriptor.check(ROOT, POINTCLOUD)
    assert report.errors == [] and loaded is not None and loaded.kind == "sensor"
    assert loaded.data["sensor"] == "pointcloud"
    # One adapter on every lane: no lane rows, no frame default.
    assert "lanes" not in loaded.data
    assert loaded.data["adapter_row"] == {"adapter": "pointcloud", "stream_budget_s": 1.0}
    entry = next(entry for entry in descriptor.list_components(ROOT)["components"]
                 if entry["id"] == "manyforge.pointcloud_source")
    assert entry["kind"] == "sensor" and entry["addable"] and entry["errors"] == []
    assert entry["runtime"] == {"componentProfileId": "manyforge.pointcloud_source",
                                "lanes": ["isaac", "mock", "real"]}


def test_sensor_refuses_unknown_type_and_rows_without_adapter(tmp_path: Path) -> None:
    folder = tmp_path / "sensor"
    shutil.copytree(POINTCLOUD, folder)
    _edit(folder, _set("sensor", "lidar"))
    _edit(folder, lambda data: data["adapter_row"].pop("adapter"))
    lines = [error.line() for error in descriptor.check(ROOT, folder)[0].errors]
    assert any("sensor: must be one of pointcloud" in line for line in lines)
    assert any("adapter_row" in line and "adapter" in line for line in lines)


def test_sensor_refuses_lane_rows(tmp_path: Path) -> None:
    folder = tmp_path / "sensor"
    shutil.copytree(POINTCLOUD, folder)
    _edit(folder, _set("lanes", {"real": {"adapter": "pointcloud"}}))
    lines = [error.line() for error in descriptor.check(ROOT, folder)[0].errors]
    assert any("unknown fields lanes" in line for line in lines)


def test_register_lists_a_sensor_in_component_models(tmp_path: Path) -> None:
    root = _workspace(tmp_path)
    folder = root / "components/sensor/acme.cloud"
    shutil.copytree(POINTCLOUD, folder)
    _edit(folder, _set("id", "acme.cloud"))
    report, written = descriptor.register(root, folder)
    listed = json.loads((root / descriptor.DESCRIPTION).read_text())["component_models"]
    assert report.errors == [] and written
    assert "components/sensor/acme.cloud/component.yaml" in listed and listed == sorted(listed)


def test_register_lists_an_assistant_in_component_models(tmp_path: Path) -> None:
    root = _workspace(tmp_path)
    folder = root / "components/assistant/acme.assistant"
    shutil.copytree(ASSISTANT, folder)
    _edit(folder, _set("id", "acme.assistant"))
    report, written = descriptor.register(root, folder)
    listed = json.loads((root / descriptor.DESCRIPTION).read_text())["component_models"]
    assert report.errors == [] and written
    assert "components/assistant/acme.assistant/component.yaml" in listed
    assert listed == sorted(listed)


def test_an_assistant_needs_its_adapter_row(tmp_path: Path) -> None:
    root = _workspace(tmp_path)
    folder = root / "components/assistant/acme.assistant"
    shutil.copytree(ASSISTANT, folder)
    _edit(folder, _set("adapter_row", {"backend": "local_process"}))
    report, _ = descriptor.check(root, folder)
    assert "component_manifest_invalid" in _codes(report)


def test_new_refuses_a_sensor(tmp_path: Path) -> None:
    report, folder = descriptor.new_component(_workspace(tmp_path), "sensor", "acme.cloud")
    assert folder is None and _codes(report) == ["component_manifest_invalid"]
