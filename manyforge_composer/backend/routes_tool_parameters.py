"""A Tool's parameters on its component page (OD-357): check and preview
them while they are edited, and export the model they generate.

Both routes read what the Draft row holds in ``model_source.parameters``
(inline parameters, or a parameter-file reference) and change no Draft. The
preview is the tool's shapes and TCP frames in its mount frame
(``parametric_tool.preview``) with what Load derives from the parameters
(tool ends, collision bodies, TCPs) and the folder its mesh paths are
relative to (the editor's mesh picker writes paths relative to it); a refusal
names its field, so the editor shows it there. Mesh paths follow the project
path rule (OD-364). Each mesh part of the preview names its GLB (the scene
mesh conversion), served while the current Deployment Draft row's
parameters name that mesh, or why it cannot be drawn. Export writes the
existing export's files (``tool.urdf``, ``tool.srdf``) into ``<project folder>/<component id>/``; the URDF names each
mesh by its path relative to that folder, or, with ``copyMeshes``, a copy
written under its ``meshes/``.
"""

from __future__ import annotations

import hashlib
import os
from pathlib import Path, PurePosixPath
from typing import Any
from urllib.parse import urlencode

from fastapi import APIRouter, HTTPException, Path as ApiPath, Query
from fastapi.responses import Response
from manyforge_behavior import parametric_tool
from manyforge_behavior.model_source_capture import (
    ModelSourceCaptureError,
    load_tool_parameters,
    read_tool_mesh,
    resolve_tool_meshes,
)
from pydantic import BaseModel, ConfigDict, Field

from .routes_scene_meshes import (
    SceneMeshAssetService,
    SceneMeshBoundsError,
    SceneMeshInputError,
    get_scene_mesh_asset_service,
)
from .state import ComposerState


_DTO = ConfigDict(populate_by_name=True, extra="forbid", allow_inf_nan=False)
_EXPORT_FORMATS = ("urdf", "srdf")
_COMPONENT_ID = r"^[A-Za-z0-9_.-]{1,128}$"


class ToolParametersRequest(BaseModel):
    model_config = _DTO
    component_id: str = Field(alias="componentId", pattern=_COMPONENT_ID)
    parameters: dict[str, Any]


class ToolExportRequest(ToolParametersRequest):
    # Copy the meshes into the export folder instead of naming the originals.
    copy_meshes: bool = Field(default=False, alias="copyMeshes")


def _refusal(code: str, message: str, field: str | None) -> dict[str, Any]:
    return {"code": code, "message": message, "field": field}


def _project_root(state: ComposerState) -> Path:
    project = state.deployment_source_project_path
    if not isinstance(project, str):
        raise HTTPException(
            status_code=409,
            detail={"code": "activation_source_project_unavailable",
                    "message": "The Deployment has no project folder"},
        )
    return Path(project)


def _loaded(state: ComposerState, component_id: str, source: Any):
    """(parameters, the folder their mesh paths are relative to, each mesh
    path's file), or a refusal for the editor (with that folder when known)."""

    project = _project_root(state)
    folder = (project if parametric_tool.is_parameter_document(source)
              else None)
    try:
        parameters, folder = load_tool_parameters(
            component_id, source,
            project_root=project, paths=state.project_paths,
        )
        meshes = resolve_tool_meshes(
            component_id, parameters, mesh_folder=folder, paths=state.project_paths)
        return parameters, folder, meshes, None
    except parametric_tool.ParametricToolError as error:
        return None, folder, None, _refusal(error.code, str(error), error.field)
    except ModelSourceCaptureError as error:
        return None, folder, None, _refusal(
            error.code or "activation_model_source_invalid", str(error), None)


def _mesh_source(shape: dict[str, Any], meshes: dict[str, Path]) -> tuple[Path, list[float]] | None:
    """A preview shape's mesh file and scale; None for a primitive."""

    mesh = shape["geometry"].get("mesh")
    return None if mesh is None else (meshes[mesh["path"]], mesh["scale"])


def _mesh_asset(service: SceneMeshAssetService, state: ComposerState, component_id: str,
                source: tuple[Path, list[float]]) -> dict[str, Any]:
    """The GLB URL of one mesh part, or why it cannot be drawn."""

    try:
        asset = service.tool_mesh_asset(source[0], state.project_paths.roots, source[1])
    except SceneMeshBoundsError as error:
        return {"url": None, "error": {"code": "scene_mesh_bounds_exceeded", "message": str(error)}}
    except SceneMeshInputError as error:
        return {"url": None, "error": {"code": "scene_mesh_input_invalid", "message": str(error)}}
    query = urlencode({"component_id": component_id})
    return {"url": f"/api/deployment/draft/tool-meshes/{asset}.glb?{query}", "error": None}


def _draft_mesh_sources(state: ComposerState, component_id: str) -> list[tuple[Path, list[float]]]:
    """The mesh files and scales the preview draws for the current Deployment
    Draft row's tool parameters, resolved here (none when they are refused)."""

    document = state.deployment_draft_session.retained_state().get("document")
    rows = document.get("components") if isinstance(document, dict) else None
    sources = [row["model_source"]["parameters"] for row in rows or []
               if isinstance(row, dict) and row.get("component_id") == component_id
               and isinstance(row.get("model_source"), dict)
               and row["model_source"].get("parameters") is not None]
    if not sources:
        return []
    parameters, _folder, meshes, refusal = _loaded(state, component_id, sources[0])
    if refusal is not None:
        return []
    return [mesh for shape in parametric_tool.preview(parameters)["shapes"]
            if (mesh := _mesh_source(shape, meshes)) is not None]


def create_tool_parameters_router(state: ComposerState) -> APIRouter:
    router = APIRouter()
    service = get_scene_mesh_asset_service(state)

    @router.post("/api/deployment/tool-parameters/preview")
    def preview_tool(body: ToolParametersRequest) -> dict[str, Any]:
        parameters, folder, meshes, refusal = _loaded(state, body.component_id, body.parameters)
        mesh_folder = None if folder is None else str(folder)
        if refusal is not None:
            return {"schemaVersion": "manyforge.composer_tool_preview.v1",
                    "ok": False, "error": refusal, "meshFolder": mesh_folder}
        component = parametric_tool.component_semantics(parameters)
        preview = parametric_tool.preview(parameters)
        for shape in preview["shapes"]:
            source = _mesh_source(shape, meshes)
            if source is not None:
                shape["mesh_asset"] = _mesh_asset(service, state, body.component_id, source)
        return {
            "schemaVersion": "manyforge.composer_tool_preview.v1",
            "ok": True,
            "meshFolder": mesh_folder,
            "preview": preview,
            "toolEnds": component["tool_ends"],
            "collisionBodies": sorted(component["collision_bodies"]),
            "tcps": component["tcps"],
        }

    @router.post("/api/deployment/tool-parameters/export")
    def export_tool(body: ToolExportRequest) -> dict[str, Any]:
        parameters, _folder, meshes, refusal = _loaded(state, body.component_id, body.parameters)
        if refusal is not None:
            raise HTTPException(status_code=409, detail=refusal)
        folder = _project_root(state) / body.component_id
        files: dict[str, bytes] = {}
        mesh_files: dict[str, str] = {}
        for relative, path in meshes.items():
            if body.copy_meshes:
                try:
                    value, _identity = read_tool_mesh(
                        body.component_id, path, paths=state.project_paths)
                except ModelSourceCaptureError as error:
                    raise HTTPException(status_code=409, detail=_refusal(
                        error.code, str(error), None)) from error
                name = f"meshes/{hashlib.sha256(value).hexdigest()[:16]}-{path.name}"
                files[name] = value
                mesh_files[relative] = name
            else:
                mesh_files[relative] = PurePosixPath(os.path.relpath(path, folder)).as_posix()
        tool = parametric_tool.generate(parameters, mesh_files)
        files = {**parametric_tool.export_files(tool, _EXPORT_FORMATS), **files}
        written = []
        for name, content in files.items():
            target = folder / name
            target.parent.mkdir(parents=True, exist_ok=True)
            temporary = target.with_name(f".{target.name}.tmp")
            temporary.write_bytes(content)
            os.replace(temporary, target)
            written.append(f"{body.component_id}/{name}")
        return {"schemaVersion": "manyforge.composer_tool_export.v1", "written": written}

    @router.get("/api/deployment/draft/tool-meshes/{asset_hash}.glb")
    def get_tool_mesh(
        asset_hash: str = ApiPath(min_length=16, max_length=16),
        component_id: str = Query(pattern=_COMPONENT_ID),
    ) -> Response:
        return service.get_tool_glb_response(
            asset_hash, _draft_mesh_sources(state, component_id), state.project_paths.roots)

    return router
