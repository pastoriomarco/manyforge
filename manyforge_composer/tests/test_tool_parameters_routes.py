"""A Tool's parameters on its component page (OD-357): the preview route
checks them and names a refused field; Export writes the generated model
into the project folder, naming the meshes where they are or copying them
(OD-364); a mesh part's GLB is served while the Deployment Draft's tool names
it."""

from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace

from fastapi import FastAPI
from fastapi.testclient import TestClient
import yaml

from manyforge_behavior.project_paths import ProjectPathRule
from manyforge_composer.backend.routes_tool_parameters import create_tool_parameters_router


ROOT = Path(__file__).resolve().parents[2]
EXAMPLE = ROOT / "manyforge_example_models/models/tool/dual_jaw_gripper.tool.yaml"


def _client(project: Path, allowed: Path | None = None, draft: dict | None = None) -> TestClient:
    app = FastAPI()
    state = SimpleNamespace(
        deployment_source_project_path=str(project),
        project_paths=ProjectPathRule([allowed or project]),
        deployment_draft_session=SimpleNamespace(retained_state=lambda: {"document": draft}))
    app.include_router(create_tool_parameters_router(state))  # type: ignore[arg-type]
    return TestClient(app)


def _parameters() -> dict:
    return yaml.safe_load(EXAMPLE.read_text(encoding="utf-8"))


def test_the_preview_shows_the_tool_ends_and_shapes(tmp_path: Path) -> None:
    response = _client(tmp_path).post("/api/deployment/tool-parameters/preview",
                                      json={"componentId": "gripper", "parameters": _parameters()})
    assert response.status_code == 200
    body = response.json()
    assert body["ok"] is True
    assert [(end["id"], end["actuated"]) for end in body["toolEnds"]] == [
        ("outer", True), ("inner", True)]
    assert "outer_pads" in body["collisionBodies"]
    assert {shape["part"] for shape in body["preview"]["shapes"]} >= {"housing", "outer_pad"}
    assert [tcp["id"] for tcp in body["preview"]["tcps"]] == ["tcp", "inner_tcp"]
    assert body["meshFolder"] == str(tmp_path)


def test_a_refusal_names_its_field(tmp_path: Path) -> None:
    parameters = _parameters()
    parameters["chains"][0]["elements"][0]["jaw_pair"]["max_aperture_m"] = 0.0
    body = _client(tmp_path).post("/api/deployment/tool-parameters/preview",
                                  json={"componentId": "gripper", "parameters": parameters}).json()
    assert body["ok"] is False
    assert body["error"]["code"] == "parametric_tool_invalid"
    assert body["error"]["field"] == "chains[0].elements[0].jaw_pair.max_aperture_m"
    assert body["meshFolder"] == str(tmp_path)


def test_a_parameter_file_is_read_from_the_project(tmp_path: Path) -> None:
    (tmp_path / "tool").mkdir()
    (tmp_path / "tool" / "gripper.tool.yaml").write_text(EXAMPLE.read_text(encoding="utf-8"))
    body = _client(tmp_path).post("/api/deployment/tool-parameters/preview", json={
        "componentId": "gripper", "parameters": {"project_path": "tool/gripper.tool.yaml"}}).json()
    assert body["ok"] is True
    assert body["meshFolder"] == str(tmp_path / "tool")


def test_export_writes_the_generated_model_into_the_project(tmp_path: Path) -> None:
    from manyforge_behavior import parametric_tool  # noqa: PLC0415

    response = _client(tmp_path).post("/api/deployment/tool-parameters/export",
                                      json={"componentId": "gripper", "parameters": _parameters()})
    assert response.status_code == 200
    assert response.json()["written"] == ["gripper/tool.urdf", "gripper/tool.srdf"]
    expected = parametric_tool.generate(
        parametric_tool.load_parameter_document(_parameters(), source="test"), {})
    assert (tmp_path / "gripper" / "tool.urdf").read_bytes() == expected.urdf
    assert (tmp_path / "gripper" / "tool.srdf").read_bytes() == expected.srdf

    refused = _parameters()
    refused["kind"] = "gripper"
    response = _client(tmp_path).post("/api/deployment/tool-parameters/export",
                                      json={"componentId": "gripper", "parameters": refused})
    assert response.status_code == 409
    assert response.json()["detail"]["code"] == "parametric_tool_kind_unsupported"


def _with_mesh(path: str) -> dict:
    parameters = _parameters()
    parameters["body"][0]["visual"] = {"mesh": {"path": path}}
    return parameters


def _mesh_project(tmp_path: Path) -> tuple[Path, bytes]:
    project = tmp_path / "cell.scenario"
    project.mkdir()
    (tmp_path / "meshes").mkdir()
    mesh = b"solid housing\nendsolid housing\n"
    (tmp_path / "meshes" / "housing.stl").write_bytes(mesh)
    return project, mesh


def test_the_preview_refuses_a_mesh_outside_the_allowed_folders(tmp_path: Path) -> None:
    project, _mesh = _mesh_project(tmp_path)
    body = _client(project).post("/api/deployment/tool-parameters/preview", json={
        "componentId": "gripper", "parameters": _with_mesh("../meshes/housing.stl")}).json()
    assert body["ok"] is False
    assert body["error"]["code"] == "parametric_tool_mesh_unavailable"
    assert f"allowed folders: {project}" in body["error"]["message"]
    assert body["meshFolder"] == str(project)
    allowed = _client(project, tmp_path).post("/api/deployment/tool-parameters/preview", json={
        "componentId": "gripper", "parameters": _with_mesh("../meshes/housing.stl")}).json()
    assert allowed["ok"] is True


def test_export_names_the_meshes_where_they_are(tmp_path: Path) -> None:
    project, _mesh = _mesh_project(tmp_path)
    response = _client(project, tmp_path).post("/api/deployment/tool-parameters/export", json={
        "componentId": "gripper", "parameters": _with_mesh("../meshes/housing.stl")})
    assert response.status_code == 200
    assert response.json()["written"] == ["gripper/tool.urdf", "gripper/tool.srdf"]
    urdf = (project / "gripper" / "tool.urdf").read_bytes()
    # From <project>/gripper/ to <tmp>/meshes/housing.stl.
    assert b'filename="../../meshes/housing.stl"' in urdf
    assert sorted(path.name for path in (project / "gripper").iterdir()) == [
        "tool.srdf", "tool.urdf"]


def test_export_can_copy_the_meshes_beside_the_model(tmp_path: Path) -> None:
    import hashlib  # noqa: PLC0415

    project, mesh = _mesh_project(tmp_path)
    response = _client(project, tmp_path).post("/api/deployment/tool-parameters/export", json={
        "componentId": "gripper", "parameters": _with_mesh("../meshes/housing.stl"),
        "copyMeshes": True})
    assert response.status_code == 200
    name = f"meshes/{hashlib.sha256(mesh).hexdigest()[:16]}-housing.stl"
    assert response.json()["written"] == [
        "gripper/tool.urdf", "gripper/tool.srdf", f"gripper/{name}"]
    assert (project / "gripper" / name).read_bytes() == mesh
    assert f'filename="{name}"'.encode() in (project / "gripper" / "tool.urdf").read_bytes()
    # The original stays where it is.
    assert (tmp_path / "meshes" / "housing.stl").read_bytes() == mesh


_STL = b"""solid housing
facet normal 0 0 1
outer loop
vertex 0 0 0
vertex 0.01 0 0
vertex 0 0.01 0
endloop
endfacet
endsolid housing
"""


def _mesh_asset(client: TestClient, parameters: dict) -> dict:
    body = client.post("/api/deployment/tool-parameters/preview",
                       json={"componentId": "gripper", "parameters": parameters}).json()
    assert body["ok"] is True
    assert all("mesh_asset" not in shape for shape in body["preview"]["shapes"]
               if shape["part"] != "housing")
    [housing] = [shape for shape in body["preview"]["shapes"] if shape["part"] == "housing"]
    return housing["mesh_asset"]


def test_a_mesh_part_is_drawn_while_the_draft_tool_names_it(tmp_path: Path) -> None:
    (tmp_path / "meshes").mkdir()
    (tmp_path / "meshes" / "housing.stl").write_bytes(_STL)
    (tmp_path / "meshes" / "other.stl").write_bytes(_STL.replace(b"0.01", b"0.02"))
    (tmp_path / "meshes" / "housing.dae").write_bytes(b"<COLLADA/>")
    parameters = _with_mesh("meshes/housing.stl")
    client = _client(tmp_path, draft={"components": [
        {"component_id": "gripper", "model_source": {"parameters": parameters}}]})

    asset = _mesh_asset(client, parameters)
    assert asset["error"] is None
    assert asset["url"].startswith("/api/deployment/draft/tool-meshes/")
    assert asset["url"].endswith(".glb?component_id=gripper")
    glb = client.get(asset["url"])
    assert glb.status_code == 200
    assert glb.headers["content-type"] == "model/gltf-binary"
    assert glb.content.startswith(b"glTF")

    # Not for another component, nor a mesh the Draft's tool does not name.
    other_component = client.get(asset["url"].replace("=gripper", "=bracket"))
    assert other_component.status_code == 409
    assert other_component.json()["detail"]["code"] == "tool_mesh_not_draft_authorized"
    other = _mesh_asset(client, _with_mesh("meshes/other.stl"))
    refused = client.get(other["url"])
    assert refused.status_code == 409
    assert refused.json()["detail"]["code"] == "tool_mesh_not_draft_authorized"

    # A mesh the conversion cannot take names why.
    dae = _mesh_asset(client, _with_mesh("meshes/housing.dae"))
    assert dae["url"] is None
    assert dae["error"]["code"] == "scene_mesh_input_invalid"
