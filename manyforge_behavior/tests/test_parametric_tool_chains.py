"""Tools with chains (OD-352, OD-357): tool ends, generation, component
semantics, and what capture carries to Load."""

from __future__ import annotations

import copy
from pathlib import Path
import xml.etree.ElementTree as ElementTree

import pytest
import yaml

from manyforge_behavior import parametric_tool
from manyforge_behavior.model_source_capture import capture_deployment_model_sources
from manyforge_behavior.parametric_tool import ParametricToolError
from manyforge_behavior.project_paths import ProjectPathRule


ROOT = Path(__file__).resolve().parents[2]
EXAMPLES = ROOT / "manyforge_example_models/models/tool"


def _example(name: str) -> dict:
    return yaml.safe_load((EXAMPLES / f"{name}.tool.yaml").read_text())


def _generate(document: dict, mesh_files: dict[str, str] | None = None
              ) -> parametric_tool.GeneratedTool:
    raw = yaml.safe_dump(document).encode()
    return parametric_tool.generate(parametric_tool.load_parameters(raw, source="test"),
                                    mesh_files or {})


def _tree(xml: bytes) -> ElementTree.Element:
    return ElementTree.fromstring(xml)


def _joints(urdf: bytes) -> dict[str, ElementTree.Element]:
    return {joint.get("name"): joint for joint in _tree(urdf).iter("joint")}


def test_the_two_jaw_pair_gripper_generates_one_group_and_target_per_pair() -> None:
    tool = _generate(_example("dual_jaw_gripper"))
    links = {link.get("name") for link in _tree(tool.urdf).iter("link")}
    assert links == {
        "tool_mount", "tool_body",
        "tool_outer", "tool_outer_positive_jaw", "tool_outer_negative_jaw",
        "tool_outer_positive_pad", "tool_outer_negative_pad",
        "tool_inner", "tool_inner_positive_jaw", "tool_inner_negative_jaw",
        "tool_bracket", "tool_tcp", "tool_tcp_inner_tcp",
    }
    joints = _joints(tool.urdf)
    commanded, follower = joints["tool_outer_joint"], joints["tool_outer_mimic"]
    assert commanded.get("type") == follower.get("type") == "prismatic"
    assert commanded.find("limit").get("upper") == "0.02"  # (0.04 - 0) / 2
    assert follower.find("mimic").get("joint") == "tool_outer_joint"
    assert follower.find("origin").get("rpy") == "0 0 3.14159265359"
    assert commanded.find("mimic") is None
    assert joints["tool_inner_joint"].find("limit").get("upper") == "0.01"

    srdf = _tree(tool.srdf)
    assert [group.get("name") for group in srdf.findall("group")] == ["outer", "inner"]
    states = {(state.get("group"), state.get("name")): {
        joint.get("name"): joint.get("value") for joint in state.findall("joint")}
        for state in srdf.findall("group_state")}
    assert states[("outer", "open")] == {"tool_outer_joint": "0.02", "tool_outer_mimic": "0.02"}
    assert states[("outer", "closed")] == {"tool_outer_joint": "0.01", "tool_outer_mimic": "0.01"}
    assert states[("inner", "closed")] == {"tool_inner_joint": "0", "tool_inner_mimic": "0"}
    pairs = {(row.get("link1"), row.get("link2")) for row in srdf.findall("disable_collisions")}
    # Adjacent pairs only; a jaw pair's frame link has no geometry, so its
    # jaws pair with the body it hangs from.
    assert pairs == {
        ("tool_body", "tool_outer_positive_jaw"), ("tool_body", "tool_outer_negative_jaw"),
        ("tool_outer_positive_jaw", "tool_outer_positive_pad"),
        ("tool_outer_negative_jaw", "tool_outer_negative_pad"),
        ("tool_body", "tool_inner_positive_jaw"), ("tool_body", "tool_inner_negative_jaw"),
        ("tool_body", "tool_bracket"),
    }
    assert tool.robot_name == "tool"


def test_the_gripper_semantics_default_to_pads_else_fingers() -> None:
    component = _generate(_example("dual_jaw_gripper")).component
    assert component["collision_bodies"] == {
        "body": ["tool_body"],
        "outer_fingers": ["tool_outer_positive_jaw", "tool_outer_negative_jaw"],
        "outer_pads": ["tool_outer_positive_pad", "tool_outer_negative_pad"],
        "inner_fingers": ["tool_inner_positive_jaw", "tool_inner_negative_jaw"],
        "bracket": ["tool_bracket"],
    }
    assert component["tcps"] == [
        {"id": "tcp", "frame": "tool_tcp", "tool_end": "outer",
         "contact_body_role_ids": ["outer_pads"]},
        {"id": "inner_tcp", "frame": "tool_tcp_inner_tcp", "tool_end": "inner",
         "contact_body_role_ids": ["inner_fingers"]},
    ]
    assert [(end["id"], end["kind"], end["actuated"], end["tcp_ids"])
            for end in component["tool_ends"]] == [
        ("outer", "jaw_pair", True, ["tcp"]), ("inner", "jaw_pair", True, ["inner_tcp"])]
    outer, inner = component["gripper_targets"]
    assert (outer["command_target_id"], outer["group"], outer["joint"]) == (
        "outer", "outer", "tool_outer_joint")
    assert outer["grip_body_role_id"] == "outer_pads"
    assert "touch_links" not in outer
    assert outer["collision_body_role_ids"] == ["outer_fingers", "outer_pads"]
    assert inner["collision_body_role_ids"] == ["inner_fingers"]
    assert (outer["open_position_m"], outer["closed_position_m"]) == (0.02, 0.01)
    assert inner["grip_body_role_id"] == "inner_fingers"
    assert (inner["open_position_m"], inner["closed_position_m"]) == (0.01, 0.0)
    # What the wiring row does not repeat: states, frames, motion time.
    assert outer["states"] == {"open": "open", "closed": "closed"}
    assert (outer["attach_frame"], outer["tcp_frame"]) == ("tool_outer", "tool_tcp")
    assert inner["tcp_frame"] == "tool_tcp_inner_tcp"
    assert outer["command_duration_s"] == parametric_tool.DEFAULT_COMMAND_DURATION_S
    assert component["spring_sliders"] == []


def test_the_spring_probe_declares_its_rod_and_tcp_contact_body() -> None:
    tool = _generate(_example("spring_probe"))
    joints = _joints(tool.urdf)
    assert joints["tool_stylus_fix"].get("type") == "fixed"
    assert all(joint.get("type") == "fixed" for joint in joints.values())
    assert tool.component["collision_bodies"]["stylus"] == ["tool_stylus"]
    assert tool.component["tcps"] == [
        {"id": "tcp", "frame": "tool_tcp", "tool_end": "stylus",
         "contact_body_role_ids": ["stylus"]}]
    assert tool.component["spring_sliders"] == [{
        "collision_body_role_id": "stylus", "axis": [0.0, 0.0, 1.0],
        "stroke_m": 0.005, "trip_depth_m": 0.001, "max_compression_step_m": 0.002}]
    assert _tree(tool.srdf).findall("group") == []


def test_a_spring_slider_declares_its_compression_step_limit() -> None:
    document = _example("spring_probe")
    document["chains"][0]["elements"][0]["spring_slider"]["max_compression_step_m"] = 0.001
    slider = _generate(document).component["spring_sliders"][0]
    assert slider["max_compression_step_m"] == 0.001
    document["chains"][0]["elements"][0]["spring_slider"]["max_compression_step_m"] = 0
    with pytest.raises(ParametricToolError, match="max_compression_step_m"):
        _generate(document)


def test_a_spring_slider_declares_its_release_depth_below_its_trip() -> None:
    # OD-371: the depth at which a Mock input that follows the slider releases.
    document = _example("spring_probe")
    slider = document["chains"][0]["elements"][0]["spring_slider"]
    slider["release_depth_m"] = 0.0005
    assert _generate(document).component["spring_sliders"][0]["release_depth_m"] == 0.0005
    for value in (0.001, 0.002, 0):  # trip_depth_m is 0.001
        slider["release_depth_m"] = value
        with pytest.raises(ParametricToolError, match="release_depth_m"):
            _generate(document)


def test_a_tcp_cannot_take_a_spring_slider_frame_id() -> None:
    # OD-371: each spring slider is a frame of the `tool` target under its id.
    document = _example("spring_probe")
    document["tcps"].append({"id": "stylus", "attach_to": "stylus"})
    with pytest.raises(ParametricToolError, match="frame of a spring slider"):
        _generate(document)


def _collisions(urdf: bytes, link: str) -> list[tuple[str, dict, list[float]]]:
    rows = []
    for element in _tree(urdf).iter("link"):
        if element.get("name") != link:
            continue
        for collision in element.iter("collision"):
            shape = next(iter(collision.find("geometry")))
            xyz = [float(value) for value in collision.find("origin").get("xyz").split()]
            rows.append((shape.tag, dict(shape.attrib), xyz))
    return rows


def test_a_swept_space_covers_the_slider_parts_over_the_whole_stroke() -> None:
    document = _example("spring_probe")
    document["chains"][0]["elements"][0]["spring_slider"]["swept_space"] = True
    tool = _generate(document)
    component = tool.component
    assert component["collision_bodies"]["stylus_swept"] == ["tool_stylus_swept"]
    assert component["tool_ends"][0]["collision_body_role_ids"] == ["stylus", "stylus_swept"]
    # Not a contact body: only the rod may touch what it presses.
    assert component["tcps"][0]["contact_body_role_ids"] == ["stylus"]
    assert _joints(tool.urdf)["tool_stylus_swept_fix"].get("type") == "fixed"
    swept = _collisions(tool.urdf, "tool_stylus_swept")
    # The rod (cylinder along the axis): 5 mm longer, moved back by 2.5 mm.
    assert swept[0][0] == "cylinder"
    assert float(swept[0][1]["length"]) == pytest.approx(0.045)
    assert swept[0][2] == pytest.approx([0.0, 0.0, 0.0175])
    # The ball: a cylinder between its end positions and a sphere at each end.
    assert [row[0] for row in swept[1:]] == ["cylinder", "sphere", "sphere"]
    assert float(swept[1][1]["length"]) == pytest.approx(0.005)
    assert swept[1][2] == pytest.approx([0.0, 0.0, 0.0375])
    assert swept[2][2] == pytest.approx([0.0, 0.0, 0.04])
    assert swept[3][2] == pytest.approx([0.0, 0.0, 0.035])
    assert not [visual for link in _tree(tool.urdf).iter("link")
                if link.get("name") == "tool_stylus_swept" for visual in link.iter("visual")]
    disabled = {(row.get("link1"), row.get("link2"))
                for row in _tree(tool.srdf).iter("disable_collisions")}
    assert {("tool_stylus", "tool_stylus_swept"), ("tool_body", "tool_stylus_swept")} <= disabled


def test_a_swept_box_is_lengthened_along_its_own_axis() -> None:
    document = _example("spring_probe")
    slider = document["chains"][0]["elements"][0]["spring_slider"]
    slider.update(swept_space=True, axis=[1, 0, 0])
    slider["parts"] = [{"name": "plunger", "pose": {"position_m": [0.01, 0, 0]},
                        "collision": {"box": {"size_m": [0.02, 0.004, 0.004]}}}]
    swept = _collisions(_generate(document).urdf, "tool_stylus_swept")
    assert swept[0][0] == "box"
    assert [float(value) for value in swept[0][1]["size"].split()] == pytest.approx(
        [0.025, 0.004, 0.004])
    assert swept[0][2] == pytest.approx([0.0075, 0.0, 0.0])


def test_without_swept_space_there_is_no_swept_body() -> None:
    tool = _generate(_example("spring_probe"))
    assert "stylus_swept" not in tool.component["collision_bodies"]
    assert _collisions(tool.urdf, "tool_stylus_swept") == []


@pytest.mark.parametrize("part", [
    {"name": "rod", "collision": {"mesh": {"path": "rod.stl"}}},
    {"name": "rod", "pose": {"orientation": {"rpy_deg": [90, 0, 0]}},
     "collision": {"cylinder": {"radius_m": 0.002, "length_m": 0.04}}},
])
def test_a_part_that_cannot_be_swept_is_refused(part) -> None:
    document = _example("spring_probe")
    slider = document["chains"][0]["elements"][0]["spring_slider"]
    slider.update(swept_space=True, parts=[part])
    with pytest.raises(ParametricToolError) as raised:
        _generate(document)
    assert raised.value.code == "parametric_tool_swept_space_unsupported"
    assert raised.value.field == "chains[0].elements[0].spring_slider.parts[0].collision"


def test_swept_space_must_be_a_boolean() -> None:
    document = _example("spring_probe")
    document["chains"][0]["elements"][0]["spring_slider"]["swept_space"] = "yes"
    with pytest.raises(ParametricToolError, match="swept_space"):
        _generate(document)


def test_a_tcp_on_the_body_has_no_default_contact_bodies() -> None:
    document = _example("spring_probe")
    del document["tcps"][0]["attach_to"]
    assert _generate(document).component["tcps"][0]["contact_body_role_ids"] == []
    document["tcps"][0]["contact_bodies"] = ["stylus", "body"]
    assert _generate(document).component["tcps"][0]["contact_body_role_ids"] == [
        "stylus", "body"]


def test_a_chain_attaches_to_a_jaw_of_an_earlier_pair() -> None:
    document = _example("dual_jaw_gripper")
    document["chains"].append({
        "attach_to": "outer", "jaw": "negative",
        "pose": {"position_m": [0, 0.01, 0.02]},
        "elements": [{"fixed": {"id": "sensor", "parts": [
            {"name": "sensor_box", "collision": {"box": {"size_m": [0.01, 0.01, 0.01]}}}]}}],
    })
    tool = _generate(document)
    joint = _joints(tool.urdf)["tool_sensor_fix"]
    assert joint.find("parent").get("link") == "tool_outer_negative_jaw"
    assert joint.find("origin").get("xyz") == "0 0.01 0.02"
    pairs = {(row.get("link1"), row.get("link2"))
             for row in _tree(tool.srdf).findall("disable_collisions")}
    assert ("tool_outer_negative_jaw", "tool_sensor") in pairs


def test_an_explicit_negative_jaw_is_given_in_the_pair_frame() -> None:
    document = _example("dual_jaw_gripper")
    pair = document["chains"][1]["elements"][0]["jaw_pair"]
    pair["negative_jaw"] = [{"name": "inner_left", "pose": {"position_m": [0.001, -0.004, 0.02]},
                             "collision": {"box": {"size_m": [0.01, 0.006, 0.04]}}}]
    links = {link.get("name"): link for link in _tree(_generate(document).urdf).iter("link")}
    origin = links["tool_inner_negative_jaw"].find("collision/origin")
    # Turned 180 degrees about z into the -y jaw's frame.
    assert origin.get("xyz") == "-0.001 0.004 0.02"


def test_generation_is_deterministic() -> None:
    document = _example("dual_jaw_gripper")
    assert _generate(document) == _generate(copy.deepcopy(document))


def _with(change) -> dict:
    document = _example("dual_jaw_gripper")
    change(document)
    return document


def _outer(document: dict) -> dict:
    return document["chains"][0]["elements"][0]["jaw_pair"]


@pytest.mark.parametrize(
    ("change", "text"),
    [
        (lambda d: d.update(chains={}), "chains"),
        (lambda d: d["chains"][1].update(attach_to="nowhere"), "attach_to"),
        (lambda d: d["chains"][0].update(jaw="positive"), "jaw"),
        (lambda d: _outer(d).pop("closed_aperture_m"), "closed_aperture_m"),
        (lambda d: _outer(d).update(max_aperture_m=0.0), "max_aperture_m"),
        (lambda d: _outer(d).update(open_aperture_m=0.01), "open_aperture_m"),
        (lambda d: _outer(d).update(stroke_m=0.01), "unknown field"),
        (lambda d: d["chains"][1]["elements"][1]["fixed"].update(id="outer"), "duplicate"),
        (lambda d: d["chains"][1]["elements"][1]["fixed"].update(id="outer_pads"), "twice"),
        (lambda d: d["tcps"][0].update(id="first"), "'tcp'"),
        (lambda d: d["tcps"][1].update(contact_bodies=["x"]), "unknown collision body"),
        (lambda d: d["tcps"][1].update(attach_to="bracket"), "tool end"),
        (lambda d: d["chains"][0]["elements"].append({"slider": {}}), "exactly one of"),
        (lambda d: d["chains"][0]["elements"].append({"tcp": {"id": "x"}}), "exactly one of"),
        (lambda d: d.update(tcps=[]), "tcps"),
        (lambda d: d["chains"][1]["elements"][1]["fixed"]["parts"][0].update(name="housing"),
         "unique"),
    ],
)
def test_invalid_chains_are_refused(change, text) -> None:
    with pytest.raises(ParametricToolError) as raised:
        _generate(_with(change))
    assert raised.value.code == "parametric_tool_invalid"
    assert text in str(raised.value)


def test_a_spring_slider_trip_depth_stays_within_its_stroke() -> None:
    document = _example("spring_probe")
    document["chains"][0]["elements"][0]["spring_slider"]["trip_depth_m"] = 0.006
    with pytest.raises(ParametricToolError, match="trip_depth_m"):
        _generate(document)


def test_a_jaw_pair_may_not_take_the_body_target_id() -> None:
    document = _example("dual_jaw_gripper")
    document["chains"][0]["elements"][0]["jaw_pair"]["id"] = "tool"
    with pytest.raises(ParametricToolError, match="reserved element id 'tool'"):
        _generate(document)


def test_meshes_use_the_names_the_caller_gives() -> None:
    document = _example("spring_probe")
    document["body"][0]["visual"] = {"mesh": {"path": "meshes/housing.stl"}}
    tool = _generate(document, {"meshes/housing.stl": "../meshes/housing.stl"})
    assert tool.mesh_paths == ("meshes/housing.stl",)
    assert b'filename="../meshes/housing.stl"' in tool.urdf


def test_a_tip_is_a_tool_end_and_its_tcps_contact_body() -> None:
    document = _example("spring_probe")
    document["chains"].append({"attach_to": "body", "elements": [{"tip": {
        "id": "needle", "pose": {"position_m": [0.03, 0, 0.02]},
        "parts": [{"name": "needle_tip", "collision": {"sphere": {"radius_m": 0.001}}}]}}]})
    document["tcps"].append({"id": "needle_tcp", "attach_to": "needle"})
    component = _generate(document).component
    assert component["collision_bodies"]["needle"] == ["tool_needle"]
    assert component["tcps"][1] == {"id": "needle_tcp", "frame": "tool_tcp_needle_tcp",
                                    "tool_end": "needle", "contact_body_role_ids": ["needle"]}
    assert [end["kind"] for end in component["tool_ends"]] == ["spring_slider", "tip"]
    assert component["gripper_targets"] == []


def test_capture_carries_what_a_kind_tool_file_declares(tmp_path: Path) -> None:
    (tmp_path / "tool").mkdir()
    document = _example("dual_jaw_gripper")
    (tmp_path / "tool" / "gripper.tool.yaml").write_text(yaml.safe_dump(document))
    captured = capture_deployment_model_sources(
        {"components": [{"component_id": "gripper", "model_source": {
            "parameters": {"project_path": "tool/gripper.tool.yaml"}}}]},
        project_root=tmp_path, paths=ProjectPathRule([tmp_path]),
    )
    (component,) = captured["components"]
    assert set(component) == {
        "component_id", "entrypoints", "resources", "generated_component"}
    declared = component["generated_component"]
    assert declared == _generate(document).component
    assert [row["command_target_id"] for row in declared["gripper_targets"]] == [
        "outer", "inner"]
