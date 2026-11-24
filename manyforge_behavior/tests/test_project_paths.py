"""The one project path rule and the activation scene-mesh capture (OD-335)."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path

import pytest

from manyforge_behavior import model_source_capture
from manyforge_behavior.model_source_capture import (
    SCENE_MESH_CLOSURE_SCHEMA,
    SceneMeshPathError,
    capture_program_scene_meshes,
    decode_model_source_artifact,
    program_scene_mesh_uris,
    resolve_scene_mesh_path,
)
from manyforge_behavior.project_paths import ProjectPathError, ProjectPathRule


@pytest.fixture()
def layout(tmp_path: Path) -> dict[str, Path]:
    examples = tmp_path / "examples"
    project = examples / "cell.scenario"
    shared = examples / "shared" / "meshes"
    other = tmp_path / "projects" / "site"
    outside = tmp_path / "outside"
    for folder in (project / "meshes", shared, other / "meshes", outside):
        folder.mkdir(parents=True)
    (project / "meshes" / "part.stl").write_bytes(b"part mesh")
    (shared / "table.obj").write_bytes(b"table mesh")
    (other / "meshes" / "x.stl").write_bytes(b"site mesh")
    (outside / "secret.stl").write_bytes(b"outside")
    return {"examples": examples, "project": project, "other": other,
            "outside": outside, "tmp": tmp_path}


def _roots(layout: dict[str, Path]) -> ProjectPathRule:
    return ProjectPathRule([str(layout["examples"]), str(layout["other"])])


def test_relative_uri_resolves_against_the_project_folder(layout) -> None:
    path = resolve_scene_mesh_path(
        "meshes/part.stl", project_folder=layout["project"], paths=_roots(layout))
    assert path == layout["project"] / "meshes/part.stl"


def test_absolute_uri_inside_an_allowed_folder(layout) -> None:
    target = layout["other"] / "meshes/x.stl"
    assert resolve_scene_mesh_path(
        str(target), project_folder=layout["project"], paths=_roots(layout)) == target
    # An absolute path needs no project folder.
    assert resolve_scene_mesh_path(
        str(target), project_folder=None, paths=_roots(layout)) == target


def test_dot_dot_is_allowed_while_it_stays_inside(layout) -> None:
    path = resolve_scene_mesh_path(
        "../shared/meshes/table.obj", project_folder=layout["project"],
        paths=_roots(layout))
    assert path == layout["examples"] / "shared/meshes/table.obj"


def test_dot_dot_escaping_every_allowed_folder_is_refused(layout) -> None:
    with pytest.raises(SceneMeshPathError) as raised:
        resolve_scene_mesh_path(
            "../../outside/secret.stl", project_folder=layout["project"],
            paths=_roots(layout))
    assert raised.value.code == "scene_mesh_unavailable"
    message = str(raised.value)
    assert str(layout["outside"] / "secret.stl") in message
    assert str(layout["examples"]) in message and str(layout["other"]) in message


@pytest.mark.parametrize("uri", [
    "file:///tmp/x.stl", "package://pkg/meshes/x.stl", "asset://x", "http://host/x.stl"])
def test_uri_schemes_are_refused(layout, uri: str) -> None:
    with pytest.raises(SceneMeshPathError, match="URI scheme") as raised:
        resolve_scene_mesh_path(uri, project_folder=layout["project"],
                                paths=_roots(layout))
    assert str(layout["examples"]) in str(raised.value)


def test_missing_file_names_the_path_and_the_allowed_folders(layout) -> None:
    with pytest.raises(SceneMeshPathError, match="does not exist") as raised:
        resolve_scene_mesh_path(
            "meshes/missing.stl", project_folder=layout["project"],
            paths=_roots(layout))
    assert str(layout["project"] / "meshes/missing.stl") in str(raised.value)
    assert str(layout["other"]) in str(raised.value)


def test_no_current_directory_or_root_fallback(layout, monkeypatch) -> None:
    # The file exists below an allowed root and below the current directory,
    # but not below the project folder.
    monkeypatch.chdir(layout["examples"])
    with pytest.raises(SceneMeshPathError, match="does not exist"):
        resolve_scene_mesh_path(
            "shared/meshes/table.obj", project_folder=layout["project"],
            paths=_roots(layout))
    with pytest.raises(SceneMeshPathError, match="no Scenario project folder"):
        resolve_scene_mesh_path(
            "shared/meshes/table.obj", project_folder=None, paths=_roots(layout))


def test_directories_and_symlinks_are_refused(layout) -> None:
    with pytest.raises(SceneMeshPathError, match="not a regular file"):
        resolve_scene_mesh_path("meshes", project_folder=layout["project"],
                                paths=_roots(layout))
    link = layout["project"] / "meshes/link.stl"
    os.symlink(layout["outside"] / "secret.stl", link)
    with pytest.raises(SceneMeshPathError, match="symlink"):
        resolve_scene_mesh_path("meshes/link.stl", project_folder=layout["project"],
                                paths=_roots(layout))


def _program(*uris: str) -> dict:
    # Initial scene meshes and a scene.add_object node parameter.
    return {
        "scene": {"objects": [
            {"id": f"o{index}", "shape": {"type": "mesh", "mesh_resource_uri": uri,
                                           "mesh_scale": [1, 1, 1]}}
            for index, uri in enumerate(uris[:-1])
        ]},
        "nodes": [{"node_id": "scene.add_object", "parameters": {
            "shape": {"type": "mesh", "mesh_resource_uri": uris[-1], "mesh_scale": [2, 2, 2]},
        }}, {"parameters": {"shape": {"type": "box", "box_dimensions_m": [1, 1, 1]}}}],
    }


def test_capture_rows_are_sorted_unique_and_cover_node_parameters(layout) -> None:
    absolute = str(layout["other"] / "meshes/x.stl")
    program = _program("meshes/part.stl", absolute, "meshes/part.stl",
                       "../shared/meshes/table.obj")
    assert program_scene_mesh_uris(program) == sorted(
        {"meshes/part.stl", absolute, "../shared/meshes/table.obj"})
    closure = capture_program_scene_meshes(
        program, project_folder=layout["project"], paths=_roots(layout))
    assert closure["schema_version"] == SCENE_MESH_CLOSURE_SCHEMA
    rows = closure["meshes"]
    assert [row["resource_uri"] for row in rows] == program_scene_mesh_uris(program)
    expected = {
        "meshes/part.stl": b"part mesh",
        absolute: b"site mesh",
        "../shared/meshes/table.obj": b"table mesh",
    }
    for row in rows:
        value = expected[row["resource_uri"]]
        assert set(row) == {"resource_uri", "byte_size", "chunks", "content_digest",
                            "encoding", "media_type", "transferred_byte_size"}
        assert row["media_type"] == "application/octet-stream"
        assert row["encoding"] == "gzip+base64"
        assert row["byte_size"] == len(value)
        assert row["content_digest"] == "sha256:" + hashlib.sha256(value).hexdigest()
        artifact = {key: item for key, item in row.items() if key != "resource_uri"}
        assert decode_model_source_artifact(
            artifact, maximum_bytes=1024, label="mesh") == value


def test_capture_of_no_program_or_no_mesh_is_empty(layout) -> None:
    for program in (None, {"scene": {"objects": []}, "nodes": []}):
        assert capture_program_scene_meshes(
            program, project_folder=layout["project"], paths=_roots(layout)
        ) == {"schema_version": SCENE_MESH_CLOSURE_SCHEMA, "meshes": []}


def test_capture_refuses_an_unresolvable_mesh(layout) -> None:
    with pytest.raises(SceneMeshPathError) as raised:
        capture_program_scene_meshes(
            _program("../../outside/secret.stl"), project_folder=layout["project"],
            paths=_roots(layout))
    assert raised.value.uri == "../../outside/secret.stl"


def test_capture_bounds(layout, monkeypatch) -> None:
    program = _program("meshes/part.stl", "../shared/meshes/table.obj")
    monkeypatch.setattr(model_source_capture, "MAX_CLOSURE_BYTES", 12)
    with pytest.raises(SceneMeshPathError) as raised:
        capture_program_scene_meshes(
            program, project_folder=layout["project"], paths=_roots(layout))
    assert raised.value.code == "scene_mesh_bounds_exceeded"
    monkeypatch.setattr(model_source_capture, "MAX_CLOSURE_BYTES", 1 << 20)
    monkeypatch.setattr(model_source_capture, "MAX_SCENE_MESH_BYTES", 5)
    with pytest.raises(SceneMeshPathError) as raised:
        capture_program_scene_meshes(
            program, project_folder=layout["project"], paths=_roots(layout))
    assert raised.value.code == "scene_mesh_bounds_exceeded"
    monkeypatch.setattr(model_source_capture, "MAX_SCENE_MESH_BYTES", 1 << 20)
    monkeypatch.setattr(model_source_capture, "MAX_SCENE_MESHES", 1)
    with pytest.raises(SceneMeshPathError) as raised:
        capture_program_scene_meshes(
            program, project_folder=layout["project"], paths=_roots(layout))
    assert raised.value.code == "scene_mesh_bounds_exceeded"


def test_example_programs_resolve_their_meshes() -> None:
    root = Path(__file__).resolve().parents[2] / "examples"
    from manyforge_behavior.canonical_artifact import load_strict_json_yaml

    for program_path, folder in (
        (root / "isaac_manipulator_scene_light.program.yaml", root),
        (root / "sim_ur10e_pick_and_place.scenario/program.yaml",
         root / "sim_ur10e_pick_and_place.scenario"),
    ):
        program = load_strict_json_yaml(program_path)
        closure = capture_program_scene_meshes(
            program, project_folder=folder, paths=ProjectPathRule([str(root)]))
        assert len(closure["meshes"]) == 5


def test_the_rule_resolves_folders_and_refuses_relative_allowed_roots(layout) -> None:
    rule = _roots(layout)
    assert rule.resolve("meshes", project_folder=layout["project"],
                        label="package root", kind="directory") == layout["project"] / "meshes"
    # An allowed folder itself may be named as a folder.
    assert rule.resolve(str(layout["other"]), project_folder=None,
                        label="package root", kind="directory") == layout["other"]
    with pytest.raises(ProjectPathError, match="is not a folder"):
        rule.resolve("meshes/part.stl", project_folder=layout["project"],
                     label="package root", kind="directory")
    with pytest.raises(ProjectPathError, match="not an absolute path"):
        ProjectPathRule(["relative/root"])


def test_rebase_keeps_each_mesh_naming_the_same_file(tmp_path: Path) -> None:
    from manyforge_behavior.model_source_capture import rebase_program_scene_mesh_paths

    source = tmp_path / "a" / "cell.scenario"
    destination = tmp_path / "b" / "deeper" / "cell.scenario"
    program = {
        "scene": [{"shape": {"type": "mesh", "mesh_resource_uri": "meshes/x.stl"}},
                  {"shape": {"type": "mesh", "mesh_resource_uri": "/abs/y.stl"}},
                  {"shape": {"type": "mesh", "mesh_resource_uri": "package://p/z.stl"}}],
        "forest": [{"parameters": {"shape": {
            "type": "mesh", "mesh_resource_uri": "../shared/w.stl"}}}],
    }
    rebased = rebase_program_scene_mesh_paths(
        program, source_project_folder=source, destination_project_folder=destination)
    shapes = [row["shape"]["mesh_resource_uri"] for row in rebased["scene"]]
    assert shapes == ["../../../a/cell.scenario/meshes/x.stl", "/abs/y.stl",
                      "package://p/z.stl"]
    assert rebased["forest"][0]["parameters"]["shape"]["mesh_resource_uri"] == (
        "../../../a/shared/w.stl")
    assert (destination / shapes[0]).resolve() == (source / "meshes/x.stl").resolve()
    assert program["scene"][0]["shape"]["mesh_resource_uri"] == "meshes/x.stl"
