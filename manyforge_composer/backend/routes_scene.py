"""Scene routes - /api/scene/*"""
from __future__ import annotations

import logging
import time
from typing import Any

from fastapi import APIRouter, Query
from fastapi.responses import JSONResponse, Response

from manyforge_behavior.cycle_manager import CycleState
from manyforge_behavior.kernel_client import SceneEditRequest

from .models import (
    SceneDiagnosticBackendResponse,
    SceneDiagnosticHmiResponse,
    SceneDiagnosticObjectResponse,
    SceneDiagnosticsResponse,
    SceneDiagnosticsSummaryResponse,
    SceneEditApiRequest,
    SceneEditResponse,
    SceneObjectResponse,
    SceneRobotResponse,
    SceneShapeResponse,
    SceneStateResponse,
)
from .state import ComposerState


def _scene_shape_response(scene_object) -> SceneShapeResponse | None:
    if scene_object.shape is None:
        return None
    return SceneShapeResponse(
        type=scene_object.shape.type,
        box_dimensions_m=scene_object.shape.box_dimensions_m,
        sphere_radius_m=scene_object.shape.sphere_radius_m,
        cylinder_radius_m=scene_object.shape.cylinder_radius_m,
        cylinder_height_m=scene_object.shape.cylinder_height_m,
        mesh_resource_uri=scene_object.shape.mesh_resource_uri,
        mesh_scale=scene_object.shape.mesh_scale,
    )


def build_scene_state_payload(state: ComposerState) -> dict[str, Any]:
    """Build backend-authoritative scene state."""
    timestamp_ns = time.monotonic_ns()
    robot_id = None
    group_id = None
    base_frame = "base_link"
    tcp_frame = "tool0"

    if state.deployment is not None and state.deployment.robot_config is not None:
        robot_id = state.deployment.robot_config.robot_id
        group_id = state.deployment.robot_config.group_id
    if state.program is not None and state.program.robot is not None:
        base_frame = state.program.robot.base_frame
        tcp_frame = state.program.robot.tcp_frame

    joint_positions_by_name: dict[str, float] = {}
    joint_names: list[str] = []
    if state.kernel is not None:
        joint_names = state.kernel.get_joint_names()
        snapshot = (
            state.cycle_manager.get_execution_snapshot()
            if state.cycle_manager is not None
            else state.kernel.get_execution_snapshot()
        )
        if snapshot.joint_positions_rad:
            joint_positions_by_name = {
                name: pos
                for name, pos in zip(joint_names, snapshot.joint_positions_rad)
            }

    tcp_pose_in_base = None
    if state.kernel is not None:
        tcp_pose = state.kernel.get_link_pose(tcp_frame, base_frame)
        if tcp_pose is not None:
            tcp_pose_in_base = {
                "position_m": list(tcp_pose["position_m"]),
                "orientation_xyzw": list(tcp_pose["orientation_xyzw"]),
            }

    robot_info = None
    if state.deployment is not None or state.kernel is not None:
        robot_info = SceneRobotResponse(
            robot_id=robot_id,
            group_id=group_id,
            base_frame=base_frame,
            tcp_frame=tcp_frame,
            joint_names=joint_names,
            joint_positions=joint_positions_by_name,
            tcp_pose_in_base=tcp_pose_in_base,
        )

    objects: list[SceneObjectResponse] = []
    if state.kernel is not None:
        for scene_object in state.kernel.get_scene_objects():
            objects.append(
                SceneObjectResponse(
                    object_id=scene_object.object_id,
                    shape=_scene_shape_response(scene_object),
                    pose_in_universe=scene_object.pose_in_universe,
                    attached=scene_object.attached,
                    attached_to=scene_object.frame_id if scene_object.attached else None,
                )
            )
    objects.sort(key=lambda obj: obj.object_id)

    return SceneStateResponse(
        robot=robot_info,
        objects=objects,
        timestamp_ns=timestamp_ns,
    ).model_dump()


def build_scene_diagnostics_payload(state: ComposerState) -> dict[str, Any]:
    """Build backend vs HMI scene diagnostics."""
    timestamp_ns = time.monotonic_ns()
    hmi_server = getattr(state, "hmi_server", None)
    hmi_available = bool(hmi_server and hasattr(hmi_server, "get_object_render_diagnostics"))

    objects: list[SceneDiagnosticObjectResponse] = []
    if state.kernel is not None:
        for scene_object in sorted(state.kernel.get_scene_objects(), key=lambda item: item.object_id):
            backend = SceneDiagnosticBackendResponse(
                exists=True,
                attached=scene_object.attached,
                frame_id=scene_object.frame_id,
                pose_in_universe=scene_object.pose_in_universe,
            )

            mismatch_reason = None
            if hmi_available:
                render_diag = hmi_server.get_object_render_diagnostics(scene_object.object_id)
                render_info = render_diag.get("render") or {}
                rendered = bool(render_info.get("scene_node_present"))
                render_state = render_info.get("render_state")
                render_state_label = "missing"
                if rendered:
                    render_state_label = "synced" if render_state is not None else "rendered"
                hmi = SceneDiagnosticHmiResponse(
                    rendered=rendered,
                    scene_name=render_info.get("scene_name"),
                    render_state=render_state_label,
                )
                if not rendered:
                    mismatch_reason = "Object exists in backend but not rendered in HMI"
                elif scene_object.attached and not render_diag.get("frame_path"):
                    mismatch_reason = "Object is attached in backend but HMI render is not parented to a robot frame"
            else:
                hmi = SceneDiagnosticHmiResponse(
                    rendered=False,
                    scene_name=None,
                    render_state="unavailable",
                )
                mismatch_reason = "HMI unavailable"

            objects.append(
                SceneDiagnosticObjectResponse(
                    object_id=scene_object.object_id,
                    backend=backend,
                    hmi=hmi,
                    match=mismatch_reason is None,
                    mismatch_reason=mismatch_reason,
                )
            )

    return SceneDiagnosticsResponse(
        objects=objects,
        summary=SceneDiagnosticsSummaryResponse(
            total=len(objects),
            matched=sum(1 for obj in objects if obj.match),
            mismatched=sum(1 for obj in objects if not obj.match),
            hmiAvailable=hmi_available,
        ),
        timestamp_ns=timestamp_ns,
    ).model_dump(by_alias=True)


def create_scene_router(state: ComposerState) -> APIRouter:
    """Create the scene router with the given state."""
    router = APIRouter()

    @router.get("/state")
    def get_scene_state() -> dict[str, Any]:
        """Get full scene state including robot and objects."""
        return build_scene_state_payload(state)

    @router.get("/diagnostics")
    def get_scene_diagnostics() -> dict[str, Any]:
        """Compare backend-authoritative scene truth with HMI render state."""
        return build_scene_diagnostics_payload(state)

    @router.post("/edit")
    def edit_scene(validated: SceneEditApiRequest) -> dict[str, Any]:
        """Edit the scene at runtime (add, remove, update_pose, attach, detach)."""
        with state.scene_edit_lock:
            if state.cycle_manager is not None and state.cycle_manager.state == CycleState.RUNNING:
                return JSONResponse(
                    status_code=409,
                    content={
                        "success": False,
                        "error": "Scene edits are not allowed while execution is RUNNING",
                        "timestamp_ns": time.monotonic_ns(),
                    },
                )

            kernel = state.kernel
            if kernel is None:
                return JSONResponse(
                    status_code=500,
                    content={
                        "success": False,
                        "error": "Kernel not available",
                        "timestamp_ns": time.monotonic_ns(),
                    },
                )

            scene_request = SceneEditRequest(
                operation=validated.operation,
                object_id=validated.object_id,
                shape_type=validated.shape_type,
                box_dimensions_m=validated.box_dimensions_m,
                sphere_radius_m=validated.sphere_radius_m,
                pose_position_m=validated.pose_position_m,
                pose_orientation_xyzw=validated.pose_orientation_xyzw,
                frame_id=validated.frame_id,
            )
            result = kernel.scene_edit(scene_request)
        return SceneEditResponse(
            success=result.success,
            error=result.error,
            timestamp_ns=time.monotonic_ns(),
        ).model_dump()

    @router.get("/snapshot")
    def get_scene_snapshot(
        width: int = Query(default=640, ge=64, le=1920),
        height: int = Query(default=480, ge=64, le=1080),
        format: str = Query(default="jpeg", pattern="^(jpeg|png)$"),
    ) -> Response:
        """Render the Viser 3D viewport as an image.

        Requires the HMI server to be running (--hmi-port) and at least one
        connected Viser client.
        """
        hmi = state.hmi_server
        if hmi is None:
            return JSONResponse(
                status_code=503,
                content={"detail": "HMI server not running (start with --hmi-port)"},
            )

        image_bytes = hmi.render_scene(width=width, height=height, format=format)
        if image_bytes is None:
            return JSONResponse(
                status_code=503,
                content={"detail": "No Viser client connected for rendering"},
            )

        media_type = "image/jpeg" if format == "jpeg" else "image/png"
        return Response(content=image_bytes, media_type=media_type)

    @router.get("/snapshots/{snapshot_id}")
    def get_published_scene_snapshot(snapshot_id: str) -> Response:
        """Serve a previously captured scene snapshot by stable id."""
        snapshot = state.get_published_scene_snapshot(snapshot_id)
        if snapshot is None:
            return JSONResponse(
                status_code=404,
                content={"detail": "Snapshot not found"},
            )
        return Response(
            content=snapshot.image_bytes,
            media_type=snapshot.media_type,
            headers={
                "Cache-Control": "no-store, max-age=0",
                "ETag": snapshot.sha256,
            },
        )

    return router
