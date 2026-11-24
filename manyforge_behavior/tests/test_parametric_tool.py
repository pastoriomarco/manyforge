"""Tools from parameters (OD-352, OD-357): parameters, generation, capture;
mesh paths under the project path rule (OD-364)."""

from __future__ import annotations

import base64
import copy
import gzip
import os
from pathlib import Path
import xml.etree.ElementTree as ElementTree

import pytest
import yaml

from manyforge_behavior import parametric_tool
from manyforge_behavior.model_source_capture import (
    ModelSourceCaptureError,
    capture_deployment_model_sources,
)
from manyforge_behavior.parametric_tool import ParametricToolError
from manyforge_behavior.project_paths import ProjectPathRule


ROOT = Path(__file__).resolve().parents[2]
EXAMPLE = ROOT / "manyforge_example_models/models/tool/probe.tool.yaml"


def _probe() -> dict:
    return yaml.safe_load(EXAMPLE.read_text())


def _generate(document: dict, mesh_files: dict[str, str] | None = None
              ) -> parametric_tool.GeneratedTool:
    raw = yaml.safe_dump(document).encode()
    return parametric_tool.generate(parametric_tool.load_parameters(raw, source="test"),
                                    mesh_files or {})


def _links(urdf: bytes) -> dict[str, ElementTree.Element]:
    return {link.get("name"): link for link in ElementTree.fromstring(urdf).iter("link")}


def _tip(document: dict) -> dict:
    return document["chains"][0]["elements"][0]["tip"]


def test_the_example_probe_generates_the_family_links() -> None:
    tool = _generate(_probe())
    links = _links(tool.urdf)
    assert set(links) == {
        "tool_mount", "tool_body", "tool_tip", "tool_tcp", "tool_tcp_flange_side",
    }
    assert len(links["tool_body"].findall("collision")) == 2
    assert len(links["tool_tip"].findall("collision")) == 1
    mass = links["tool_body"].find("inertial/mass")
    assert mass is not None and mass.get("value") == "0.25"
    joints = {joint.get("name"): joint for joint in ElementTree.fromstring(tool.urdf).iter("joint")}
    assert joints["tool_tip_fix"].find("origin").get("xyz") == "0 0 0.1"
    # The default TCP belongs to the tip, at its origin.
    assert joints["tool_tcp_fix"].find("parent").get("link") == "tool_tip"
    assert joints["tool_tcp_fix"].find("origin").get("xyz") == "0 0 0"
    assert joints["tool_tcp_flange_side_fix"].find("parent").get("link") == "tool_body"
    assert joints["tool_tcp_flange_side_fix"].find("origin").get("rpy") == (
        "0 1.5707963268 0")
    srdf = ElementTree.fromstring(tool.srdf)
    assert srdf.get("name") == ElementTree.fromstring(tool.urdf).get("name") == "tool"
    (pair,) = srdf.findall("disable_collisions")
    assert (pair.get("link1"), pair.get("link2")) == ("tool_body", "tool_tip")
    assert tool.mesh_paths == ()
    # The tip is its TCP's contact body; a TCP on the body has none.
    assert tool.component["tcps"] == [
        {"id": "tcp", "frame": "tool_tcp", "tool_end": "tip", "contact_body_role_ids": ["tip"]},
        {"id": "flange_side", "frame": "tool_tcp_flange_side", "tool_end": None,
         "contact_body_role_ids": []},
    ]
    assert tool.component["tool_ends"] == [{
        "id": "tip", "kind": "tip", "actuated": False,
        "collision_body_role_ids": ["tip"], "tcp_ids": ["tcp"]}]


def test_generation_is_deterministic() -> None:
    first = _generate(_probe())
    second = _generate(_probe())
    assert (first.urdf, first.srdf) == (second.urdf, second.srdf)
    # The same parameters in another key order and number spelling.
    reordered = dict(reversed(list(_probe().items())))
    reordered["mass_kg"] = 0.250
    third = _generate(reordered)
    assert (third.urdf, third.srdf) == (first.urdf, first.srdf)


def test_without_a_tip_there_is_no_tip_link_and_no_exclusion() -> None:
    document = _probe()
    document["chains"] = []
    document["tcps"][0].pop("attach_to")
    tool = _generate(document)
    assert "tool_tip" not in _links(tool.urdf)
    assert ElementTree.fromstring(tool.srdf).findall("disable_collisions") == []
    assert tool.component["tcps"][0]["contact_body_role_ids"] == []


def test_per_part_mass_sums_on_each_link() -> None:
    document = _probe()
    del document["mass_kg"], document["center_of_mass_m"]
    document["body"][0]["mass_kg"] = 0.2
    document["body"][1]["mass_kg"] = 0.05
    document["body"][1]["center_of_mass_m"] = [0, 0, 0.01]
    _tip(document)["parts"][0]["mass_kg"] = 0.001
    links = _links(_generate(document).urdf)
    body = links["tool_body"].find("inertial")
    assert body.find("mass").get("value") == "0.25"
    # (0.2 * 0.03 + 0.05 * 0.09) / 0.25
    assert body.find("origin").get("xyz") == "0 0 0.042"
    assert links["tool_tip"].find("inertial/mass").get("value") == "0.001"


def test_meshes_take_the_file_names_the_caller_gives() -> None:
    document = _probe()
    document["body"][0]["visual"] = {"mesh": {"path": "../meshes/housing.stl",
                                              "scale": [0.001, 0.001, 0.001]}}
    tool = _generate(document, {"../meshes/housing.stl": "package://tool/meshes/abc.stl"})
    mesh = _links(tool.urdf)["tool_body"].find("visual/geometry/mesh")
    assert mesh.get("filename") == "package://tool/meshes/abc.stl"
    assert mesh.get("scale") == "0.001 0.001 0.001"
    assert tool.mesh_paths == ("../meshes/housing.stl",)
    assert parametric_tool.mesh_paths(parametric_tool.load_parameter_document(
        document, source="t")) == ("../meshes/housing.stl",)


def test_a_captured_mesh_is_named_by_its_content() -> None:
    content = b"solid housing\nendsolid housing\n"
    uri = parametric_tool.captured_mesh_uri("meshes/housing.STL", content)
    assert uri.startswith("package://tool/meshes/") and uri.endswith(".stl")
    assert parametric_tool.captured_mesh_uri("../other/place.stl", content) == uri
    assert parametric_tool.captured_mesh_uri("meshes/housing.stl", content + b" ") != uri


def _with(change):
    document = _probe()
    change(document)
    return document


@pytest.mark.parametrize(
    ("change", "code", "text"),
    [
        (lambda d: d.update(api_version="manyforge.parametric_tool.v0"),
         "parametric_tool_invalid", "api_version"),
        (lambda d: d.update(kind="pneumatic_parallel_gripper"),
         "parametric_tool_kind_unsupported", "kind"),
        (lambda d: d.update(colour="red"), "parametric_tool_invalid", "colour"),
        (lambda d: d.update(body=[]), "parametric_tool_invalid", "body"),
        (lambda d: [(part.pop("collision"), part.update(visual={"sphere": {"radius_m": 0.1}}))
                    for part in d["body"]],
         "parametric_tool_invalid", "collision geometry"),
        (lambda d: (_tip(d)["parts"][0].pop("collision"),
                    _tip(d)["parts"][0].update(visual={"sphere": {"radius_m": 0.1}})),
         "parametric_tool_invalid", "collision geometry"),
        (lambda d: d.update(kind="passive_tool"), "parametric_tool_kind_unsupported", "kind"),
        (lambda d: d["tcps"][0].update(attach_to="nowhere"), "parametric_tool_invalid",
         "attach_to"),
        (lambda d: d["body"][0].update(collision={"box": {"size_m": [0.1, 0, 0.1]}}),
         "parametric_tool_invalid", "size_m[1]"),
        (lambda d: d["body"][0].update(collision={"box": {}, "sphere": {}}),
         "parametric_tool_invalid", "exactly one of"),
        (lambda d: d["body"][0].update(collision={"mesh": {"path": " "}}),
         "parametric_tool_invalid", "file path"),
        (lambda d: d["body"][0].update(collision={"mesh": {"path": "part.obj"}}),
         "parametric_tool_invalid", ".stl or .dae"),
        (lambda d: d["body"][1].update(name="housing"),
         "parametric_tool_invalid", "unique"),
        (lambda d: d["body"][0].update(pose={"orientation": {"xyzw": [0, 0, 0, 0]}}),
         "parametric_tool_invalid", "norm 0"),
        (lambda d: d["body"][0].update(pose={"orientation": {"rpy_deg": [0, 0, 0],
                                                             "xyzw": [0, 0, 0, 1]}}),
         "parametric_tool_invalid", "exactly one of"),
        (lambda d: d.update(tcps=[]), "parametric_tool_invalid", "tcps"),
        (lambda d: d["tcps"][0].update(id="probe"), "parametric_tool_invalid", "'tcp'"),
        (lambda d: d["tcps"][1].update(id="tcp"), "parametric_tool_invalid", "reserved"),
        (lambda d: d["tcps"].append({"id": "flange_side"}),
         "parametric_tool_invalid", "duplicate"),
        (lambda d: d["body"][0].update(mass_kg=0.1), "parametric_tool_invalid", "not both"),
        (lambda d: d.pop("mass_kg"), "parametric_tool_invalid", "needs the tool's mass_kg"),
        (lambda d: d.update(mass_kg=-1), "parametric_tool_invalid", ">= 0"),
        (lambda d: d.update(mass_kg=True), "parametric_tool_invalid", "number"),
    ],
)
def test_invalid_parameters_are_refused_with_their_code(change, code, text) -> None:
    with pytest.raises(ParametricToolError) as raised:
        _generate(_with(change))
    assert raised.value.code == code
    assert text in str(raised.value)


def test_a_refusal_names_its_field() -> None:
    document = _probe()
    document["body"][0]["collision"] = {"box": {"size_m": [0.1, 0, 0.1]}}
    with pytest.raises(ParametricToolError) as raised:
        _generate(document)
    assert raised.value.field == "body[0].collision.box.size_m[1]"


def test_inline_parameters_load_as_a_file_does() -> None:
    document = _probe()
    assert parametric_tool.is_parameter_document(document)
    assert not parametric_tool.is_parameter_document({"project_path": "tool/probe.tool.yaml"})
    inline = parametric_tool.generate(
        parametric_tool.load_parameter_document(document, source="row"), {})
    assert inline == _generate(document)


def test_the_preview_places_shapes_and_tcps_in_the_mount_frame() -> None:
    preview = parametric_tool.preview(parametric_tool.load_parameter_document(_probe(), source="t"))
    ball = next(shape for shape in preview["shapes"] if shape["part"] == "stylus_ball")
    assert ball["geometry"] == {"sphere": {"radius_m": 0.003}}
    assert ball["position_m"] == [0.0, 0.0, 0.1]
    assert [tcp["id"] for tcp in preview["tcps"]] == ["tcp", "flange_side"]
    assert preview["tcps"][0]["position_m"] == [0.0, 0.0, 0.1]


def test_export_files_are_the_generated_bytes() -> None:
    tool = _generate(_probe())
    assert parametric_tool.export_files(tool, ["urdf", "srdf"]) == {
        "tool.urdf": tool.urdf, "tool.srdf": tool.srdf}


def test_export_formats_refuse_usda_for_now() -> None:
    assert parametric_tool.require_export_formats(["urdf", "srdf"]) == ("urdf", "srdf")
    with pytest.raises(ParametricToolError) as raised:
        parametric_tool.require_export_formats(["urdf", "usda"])
    assert raised.value.code == "parametric_tool_export_format_unsupported"


def _decoded(artifact: dict) -> bytes:
    return gzip.decompress(b"".join(base64.b64decode(chunk) for chunk in artifact["chunks"]))


def _parameter_deployment(path: str = "tool/probe.tool.yaml") -> dict:
    return {"components": [{
        "component_id": "probe",
        "model_source": {"parameters": {"project_path": path}},
    }]}


def test_capture_generates_the_model_from_a_parameter_file(tmp_path: Path) -> None:
    document = _probe()
    document["body"][0]["visual"] = {"mesh": {"path": "meshes/housing.stl"}}
    (tmp_path / "tool" / "meshes").mkdir(parents=True)
    (tmp_path / "tool" / "probe.tool.yaml").write_text(yaml.safe_dump(document))
    mesh = b"solid housing\nendsolid housing\n"
    (tmp_path / "tool" / "meshes" / "housing.stl").write_bytes(mesh)

    captured = capture_deployment_model_sources(
        _parameter_deployment(), project_root=tmp_path, paths=ProjectPathRule([tmp_path])
    )
    (component,) = captured["components"]
    uri = parametric_tool.captured_mesh_uri("meshes/housing.stl", mesh)
    expected = _generate(document, {"meshes/housing.stl": uri})
    assert _decoded(component["entrypoints"]["urdf"]) == expected.urdf
    assert _decoded(component["entrypoints"]["srdf"]) == expected.srdf
    (resource,) = component["resources"]
    assert resource["logical_uri"] == uri
    assert _decoded(resource) == mesh
    # Captured again, the same bytes and digests.
    again = capture_deployment_model_sources(
        _parameter_deployment(), project_root=tmp_path, paths=ProjectPathRule([tmp_path])
    )
    assert again == captured


def test_capture_takes_inline_parameters_with_meshes_in_the_project(tmp_path: Path) -> None:
    document = _probe()
    document["body"][0]["visual"] = {"mesh": {"path": "tool/meshes/housing.stl"}}
    (tmp_path / "tool" / "meshes").mkdir(parents=True)
    mesh = b"solid housing\nendsolid housing\n"
    (tmp_path / "tool" / "meshes" / "housing.stl").write_bytes(mesh)
    captured = capture_deployment_model_sources(
        {"components": [{"component_id": "probe", "model_source": {"parameters": document}}]},
        project_root=tmp_path, paths=ProjectPathRule([tmp_path]),
    )
    (component,) = captured["components"]
    uri = parametric_tool.captured_mesh_uri("tool/meshes/housing.stl", mesh)
    assert _decoded(component["entrypoints"]["urdf"]) == _generate(
        document, {"tool/meshes/housing.stl": uri}).urdf
    (resource,) = component["resources"]
    assert resource["logical_uri"] == uri
    assert _decoded(resource) == mesh
    assert component["generated_component"] == _generate(
        document, {"tool/meshes/housing.stl": uri}).component


def test_capture_refuses_a_parameter_file_with_its_code(tmp_path: Path) -> None:
    (tmp_path / "tool").mkdir()
    document = _probe()
    document["body"][0]["collision"] = {"mesh": {"path": "meshes/missing.stl"}}
    (tmp_path / "tool" / "probe.tool.yaml").write_text(yaml.safe_dump(document))
    with pytest.raises(ModelSourceCaptureError) as raised:
        capture_deployment_model_sources(
            _parameter_deployment(), project_root=tmp_path, paths=ProjectPathRule([tmp_path])
        )
    assert raised.value.code == "parametric_tool_mesh_unavailable"

    document = _probe()
    document["kind"] = "gripper"
    (tmp_path / "tool" / "probe.tool.yaml").write_text(yaml.safe_dump(document))
    with pytest.raises(ModelSourceCaptureError) as raised:
        capture_deployment_model_sources(
            _parameter_deployment(), project_root=tmp_path, paths=ProjectPathRule([tmp_path])
        )
    assert raised.value.code == "parametric_tool_kind_unsupported"

    mixed = _parameter_deployment()
    mixed["components"][0]["model_source"]["package_roots"] = {}
    with pytest.raises(ModelSourceCaptureError) as raised:
        capture_deployment_model_sources(
            mixed, project_root=tmp_path, paths=ProjectPathRule([tmp_path])
        )
    assert raised.value.code == "parametric_tool_invalid"


def test_capture_applies_the_project_path_rule_to_a_parameter_file(tmp_path: Path) -> None:
    project = tmp_path / "project"
    project.mkdir()
    (tmp_path / "probe.tool.yaml").write_text(yaml.safe_dump(_probe()))
    with pytest.raises(ModelSourceCaptureError) as raised:
        capture_deployment_model_sources(
            _parameter_deployment("../probe.tool.yaml"),
            project_root=project, paths=ProjectPathRule([project]),
        )
    assert raised.value.code is None
    assert "allowed folders" in str(raised.value)


def test_an_authored_urdf_capture_keeps_no_code(tmp_path: Path) -> None:
    deployment = copy.deepcopy(_parameter_deployment())
    deployment["components"][0]["model_source"] = {
        "urdf": {"project_path": "missing.urdf"}, "package_roots": {}}
    with pytest.raises(ModelSourceCaptureError) as raised:
        capture_deployment_model_sources(
            deployment, project_root=tmp_path, paths=ProjectPathRule([tmp_path])
        )
    assert raised.value.code is None


# --- mesh paths under the project path rule (OD-364) -----------------------


def _inline(path: str) -> dict:
    document = _probe()
    document["body"][0]["visual"] = {"mesh": {"path": path}}
    return {"components": [{"component_id": "probe", "model_source": {"parameters": document}}]}


def _mesh_resource(deployment: dict, project: Path, allowed: list[Path]) -> dict:
    captured = capture_deployment_model_sources(
        deployment, project_root=project, paths=ProjectPathRule(allowed))
    (component,) = captured["components"]
    (resource,) = component["resources"]
    return resource


def test_a_mesh_path_may_leave_its_folder_inside_an_allowed_folder(tmp_path: Path) -> None:
    project = tmp_path / "cell.scenario"
    project.mkdir()
    (tmp_path / "meshes").mkdir()
    mesh = b"solid housing\nendsolid housing\n"
    (tmp_path / "meshes" / "housing.stl").write_bytes(mesh)
    resource = _mesh_resource(_inline("../meshes/housing.stl"), project, [tmp_path])
    assert resource["logical_uri"] == parametric_tool.captured_mesh_uri("housing.stl", mesh)
    assert _decoded(resource) == mesh
    # An absolute path inside an allowed folder resolves the same way.
    absolute = _mesh_resource(_inline(str(tmp_path / "meshes" / "housing.stl")),
                              project, [tmp_path])
    assert absolute == resource
    # Nothing is written into the project folders.
    assert sorted(path.name for path in tmp_path.rglob("*")) == [
        "cell.scenario", "housing.stl", "meshes"]


@pytest.mark.parametrize("path", ["../meshes/housing.stl", "{outside}/housing.stl"])
def test_a_mesh_outside_the_allowed_folders_is_refused(tmp_path: Path, path: str) -> None:
    project = tmp_path / "cell.scenario"
    project.mkdir()
    (tmp_path / "meshes").mkdir()
    (tmp_path / "meshes" / "housing.stl").write_bytes(b"solid housing\n")
    with pytest.raises(ModelSourceCaptureError) as raised:
        _mesh_resource(_inline(path.format(outside=tmp_path / "meshes")), project, [project])
    assert raised.value.code == "parametric_tool_mesh_unavailable"
    assert "outside the allowed folders" in str(raised.value)
    assert f"allowed folders: {project}" in str(raised.value)


def test_a_parameter_file_mesh_is_relative_to_the_file(tmp_path: Path) -> None:
    (tmp_path / "tool").mkdir()
    (tmp_path / "meshes").mkdir()
    document = _probe()
    document["body"][0]["visual"] = {"mesh": {"path": "../meshes/housing.stl"}}
    (tmp_path / "tool" / "probe.tool.yaml").write_text(yaml.safe_dump(document))
    mesh = b"solid housing\nendsolid housing\n"
    (tmp_path / "meshes" / "housing.stl").write_bytes(mesh)
    resource = _mesh_resource(_parameter_deployment(), tmp_path, [tmp_path])
    assert _decoded(resource) == mesh


def test_the_same_mesh_content_gives_the_same_model(tmp_path: Path) -> None:
    mesh = b"solid housing\nendsolid housing\n"
    captures = []
    for folder, path in (("a", "meshes/housing.stl"), ("b", "../shared/part.stl")):
        project = tmp_path / folder / "cell.scenario"
        project.mkdir(parents=True)
        target = Path(os.path.normpath(project / path))
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(mesh)
        captures.append(capture_deployment_model_sources(
            _inline(path), project_root=project, paths=ProjectPathRule([tmp_path / folder])))
    first, second = (capture["components"][0] for capture in captures)
    assert first["entrypoints"] == second["entrypoints"]
    assert first["resources"] == second["resources"]
    # Other content: another model.
    (tmp_path / "a" / "cell.scenario" / "meshes" / "housing.stl").write_bytes(mesh + b"\n")
    changed = _mesh_resource(_inline("meshes/housing.stl"), tmp_path / "a" / "cell.scenario",
                             [tmp_path / "a"])
    assert changed["logical_uri"] != first["resources"][0]["logical_uri"]
