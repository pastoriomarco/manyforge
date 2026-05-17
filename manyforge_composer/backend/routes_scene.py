"""Scene routes - /api/scene/*"""
from __future__ import annotations

import logging
import time
from typing import Any, Optional

from fastapi import APIRouter, HTTPException, Query
from fastapi.responses import JSONResponse, Response

from manyforge_behavior.cycle_manager import CycleState
from manyforge_behavior.kernel_client import SceneEditRequest
from manyforge_behavior.model_paths import (
    default_package_paths,
    resolve_package_uri,
    resolve_robot_model,
)

from .models import (
    SceneDiagnosticBackendResponse,
    SceneDiagnosticHmiResponse,
    SceneDiagnosticObjectResponse,
    SceneDiagnosticsResponse,
    SceneDiagnosticsSummaryResponse,
    SceneEditApiRequest,
    SceneEditResponse,
    SceneFrameResponse,
    SceneFramesResponse,
    SceneFrameGizmoSizeRequest,
    SceneFrameGizmoSizeResponse,
    SceneFrameVisibilityRequest,
    SceneFrameVisibilityResponse,
    SceneObjectFrameVisibilityRequest,
    SceneObjectFrameVisibilityResponse,
    SceneObjectResponse,
    SceneRobotResponse,
    SceneShapeResponse,
    SceneStateResponse,
)
from .state import ComposerState

logger = logging.getLogger(__name__)


def canonical_scene_world_frame(state: ComposerState) -> str:
    """Return the backend's canonical frame for authored world-scene objects."""
    deployment = state.deployment
    context_adapter = None
    if deployment is not None:
        context_adapter = deployment.adapters.get("context")
    context_config = getattr(context_adapter, "config", {}) or {}
    for key in ("scene_world_frame", "world_frame", "sceneRootFrame", "scene_root_frame"):
        candidate = context_config.get(key)
        if isinstance(candidate, str) and candidate.strip():
            return candidate.strip()
    adapter_id = str(getattr(context_adapter, "adapter_id", ""))
    if "roboplan" in adapter_id:
        return "universe"
    return "universe"


def _make_package_handler(package_dirs: list[str]):
    def handler(fname: str) -> str:
        if not fname.startswith("package://"):
            return fname
        resolved = resolve_package_uri(fname, package_dirs)
        if resolved is not None:
            return resolved
        return fname

    return handler


def _load_model_frame_names(state: ComposerState) -> list[str]:
    deployment = state.deployment
    if deployment is None:
        return []
    if not state.deployment_source_path:
        return []

    robot_config = deployment.robot_config
    if not robot_config.urdf_path:
        return []

    extra_package_paths = (
        []
        if robot_config.package_paths
        else default_package_paths()
    )
    try:
        resolved_model = resolve_robot_model(
            urdf_path=robot_config.urdf_path,
            srdf_path=robot_config.srdf_path,
            package_paths=robot_config.package_paths,
            deployment_path=state.deployment_source_path,
            extra_package_paths=extra_package_paths,
        )
    except Exception:
        logger.debug("Could not resolve robot model while building frame inventory", exc_info=True)
        return []

    try:
        import yourdfpy

        if resolved_model.package_paths:
            model = yourdfpy.URDF.load(
                resolved_model.urdf_path,
                filename_handler=_make_package_handler(resolved_model.package_paths),
                load_meshes=False,
            )
        else:
            model = yourdfpy.URDF.load(
                resolved_model.urdf_path,
                load_meshes=False,
            )
    except Exception:
        logger.debug("Could not load URDF while building frame inventory", exc_info=True)
        return []

    link_map = getattr(model, "link_map", {}) or {}
    frame_names = sorted(str(name) for name in link_map.keys())
    return frame_names


def _scene_frame_roles(state: ComposerState) -> dict[str, set[str]]:
    roles: dict[str, set[str]] = {canonical_scene_world_frame(state): {"world"}}

    def add_role(frame_name: str, role: str) -> None:
        name = str(frame_name).strip()
        if not name:
            return
        roles.setdefault(name, set()).add(role)

    if state.program is not None and state.program.robot is not None:
        add_role(state.program.robot.base_frame, "robot_base")
        add_role(state.program.robot.tcp_frame, "program_tcp")

    if state.deployment is not None:
        for gripper_id, config in state.deployment.grippers.items():
            add_role(config.mount_frame, f"{gripper_id}:mount")
            add_role(config.attach_frame, f"{gripper_id}:attach")
            add_role(config.tcp_frame, f"{gripper_id}:tcp")
            for frame_name in config.auxiliary_frames:
                add_role(frame_name, f"{gripper_id}:auxiliary")

    return roles


def build_scene_frames_payload(state: ComposerState) -> dict[str, Any]:
    """Build known frame inventory plus current viewer visibility.

    Day-4 of the native-viewer migration: visibility state is owned by
    ComposerState (see `set_frame_visibility` / `get_frame_visibility`
    on the state object), so this works without the Viser HMI server
    attached. ``hmi_available`` in the response now reflects whether
    *any* viewer is wired up (always true now that state owns it) —
    the field is preserved for backward compatibility with clients
    that still gate on it; legacy Viser callers see the same value
    they did before.
    """
    timestamp_ns = time.monotonic_ns()
    frame_roles = _scene_frame_roles(state)
    frame_names = set(frame_roles.keys())
    frame_names.update(_load_model_frame_names(state))

    kernel = state.kernel
    world_frame = canonical_scene_world_frame(state)

    frames: list[SceneFrameResponse] = []
    for frame_name in sorted(frame_names):
        pose_available = frame_name == world_frame
        pose_in_world: Optional[dict[str, Any]] = None
        if frame_name == world_frame:
            pose_in_world = {
                "position_m": [0.0, 0.0, 0.0],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            }
        elif kernel is not None:
            try:
                pose = kernel.get_link_pose(frame_name, world_frame)
            except Exception:
                pose = None
            if pose is not None:
                pose_in_world = {
                    "position_m": list(pose["position_m"]),
                    "orientation_xyzw": list(pose["orientation_xyzw"]),
                }
                pose_available = True
            else:
                pose_available = False

        viewer_visible = state.get_frame_visibility(frame_name)

        frames.append(
            SceneFrameResponse(
                name=frame_name,
                roles=sorted(frame_roles.get(frame_name, set())),
                viewerVisible=viewer_visible,
                poseAvailable=pose_available,
                poseInWorld=pose_in_world,
            )
        )

    return SceneFramesResponse(
        frames=frames,
        hmiAvailable=True,
        frameGizmoSizeM=state.get_frame_gizmo_size(),
        timestampNs=timestamp_ns,
    ).model_dump(by_alias=True)


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
    base_pose: dict[str, Any] = {}

    if state.deployment is not None and state.deployment.robot_config is not None:
        robot_id = state.deployment.robot_config.robot_id
        group_id = state.deployment.robot_config.group_id
    if state.program is not None and state.program.robot is not None:
        base_frame = state.program.robot.base_frame
        tcp_frame = state.program.robot.tcp_frame
        base_pose = dict(state.program.robot.base_pose or {})

    joint_positions_by_name: dict[str, float] = {}
    joint_names: list[str] = []
    if state.kernel is not None:
        joint_names = state.kernel.get_joint_names()
        snapshot = (
            state.cycle_manager.get_execution_snapshot()
            if state.cycle_manager is not None
            else state.kernel.get_execution_snapshot()
        )
        # Joint-state readback fallback chain (Day 5c.1):
        # 1. Active execution snapshot (preferred — freshest).
        # 2. The state-cached last-known vector from a prior execution.
        # 3. Deployment's `execution.config.initial_joint_positions_rad`
        #    (set on deployment load; what the kernel uses as its
        #    initial configuration).
        # 4. Zeros (last-resort default; matches pre-5c.1 behaviour).
        live_positions: list[float] | None = None
        if snapshot.joint_positions_rad:
            live_positions = list(snapshot.joint_positions_rad)
            state._last_known_joint_positions_rad = live_positions
        elif state._last_known_joint_positions_rad is not None:
            live_positions = state._last_known_joint_positions_rad
        elif state.deployment is not None:
            try:
                execution_adapter = state.deployment.adapters.get("execution")
            except Exception:
                execution_adapter = None
            if execution_adapter is not None:
                initial = execution_adapter.config.get("initial_joint_positions_rad")
                if isinstance(initial, list) and len(initial) == len(joint_names):
                    live_positions = [float(v) for v in initial]
                    state._last_known_joint_positions_rad = live_positions
        if live_positions is not None and len(live_positions) == len(joint_names):
            joint_positions_by_name = {
                name: pos for name, pos in zip(joint_names, live_positions)
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
            base_pose=base_pose,
            joint_names=joint_names,
            joint_positions=joint_positions_by_name,
            tcp_pose_in_base=tcp_pose_in_base,
        )

    objects: list[SceneObjectResponse] = []
    authored_object_ids = {
        resource.object_id
        for resource in (state.program.scene if state.program is not None else [])
    }
    if state.kernel is not None:
        for scene_object in state.kernel.get_scene_objects():
            if scene_object.attached:
                source = "attached"
            elif scene_object.object_id in authored_object_ids:
                source = "authored"
            else:
                source = "runtime"
            viewer_frame_visible = state.get_object_frame_visibility(scene_object.object_id)
            objects.append(
                SceneObjectResponse(
                    object_id=scene_object.object_id,
                    shape=_scene_shape_response(scene_object),
                    pose_in_universe=scene_object.pose_in_universe,
                    attached=scene_object.attached,
                    attached_to=scene_object.frame_id if scene_object.attached else None,
                    source=source,
                    persistable=source == "authored",
                    viewer_frame_visible=viewer_frame_visible,
                )
            )
    objects.sort(key=lambda obj: obj.object_id)

    link_poses: dict[str, dict[str, Any]] = {}
    if state.kernel is not None:
        try:
            from .routes_urdf import visual_link_names_for_fk
        except Exception:
            visual_link_names_for_fk = None  # type: ignore[assignment]
        if visual_link_names_for_fk is not None:
            world_frame = canonical_scene_world_frame(state)
            for link_name in visual_link_names_for_fk(state):
                try:
                    pose = state.kernel.get_link_pose(link_name, world_frame)
                except Exception:
                    pose = None
                if pose is None:
                    continue
                link_poses[link_name] = {
                    "position_m": list(pose["position_m"]),
                    "orientation_xyzw": list(pose["orientation_xyzw"]),
                }

    return SceneStateResponse(
        robot=robot_info,
        objects=objects,
        link_poses=link_poses,
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

    @router.get("/frames")
    def get_scene_frames() -> dict[str, Any]:
        """Get known robot/tool frames plus current viewer visibility."""
        return build_scene_frames_payload(state)

    @router.post("/frames/visibility")
    def set_frame_visibility(validated: SceneFrameVisibilityRequest) -> dict[str, Any]:
        """Show/hide a named frame axis in the active viewer."""
        state.set_frame_visibility(validated.frame_name, validated.visible)
        return SceneFrameVisibilityResponse(
            success=True,
            frameName=validated.frame_name,
            visible=validated.visible,
            hmiAvailable=True,
            timestampNs=time.monotonic_ns(),
        ).model_dump(by_alias=True)

    @router.post("/frames/gizmo-size")
    def set_frame_gizmo_size(validated: SceneFrameGizmoSizeRequest) -> dict[str, Any]:
        """Set global viewer size for named frame axes."""
        try:
            state.set_frame_gizmo_size(validated.axes_length_m)
        except ValueError as exc:
            return JSONResponse(
                status_code=400,
                content={"detail": str(exc)},
            )
        return SceneFrameGizmoSizeResponse(
            success=True,
            axesLengthM=state.get_frame_gizmo_size(),
            hmiAvailable=True,
            timestampNs=time.monotonic_ns(),
        ).model_dump(by_alias=True)

    @router.post("/objects/frame-visibility")
    def set_object_frame_visibility(
        validated: SceneObjectFrameVisibilityRequest,
    ) -> dict[str, Any]:
        """Show/hide a scene object's pose axis in the active viewer."""
        state.set_object_frame_visibility(validated.object_id, validated.visible)
        return SceneObjectFrameVisibilityResponse(
            success=True,
            objectId=validated.object_id,
            visible=validated.visible,
            hmiAvailable=True,
            timestampNs=time.monotonic_ns(),
        ).model_dump(by_alias=True)

    @router.post("/program-resource/update")
    def update_program_scene_resource(payload: dict[str, Any]) -> dict[str, Any]:
        """Operator-facing wrapper around the assistant tool's
        scene_draft_update_object helper (Day-5c followup, issue #2).

        The native-viewer's Object Edit overlay POSTs here to persist
        an authored scene object's pose/shape changes into the
        program draft. The operator can then Save via ProgramPanel
        to commit to the YAML. Runtime / sensed / attached objects
        aren't represented in the program's scene resource list and
        can't be persisted this way — they should be edited via
        their source behavior-tree node (runtime override).
        """
        # Lazy import to avoid pulling the assistant-routes module
        # into the import chain at app construction time; the
        # underscore-prefixed helper is module-private but the route
        # is the documented operator-facing surface for the same
        # operation the assistant tool exposes to LLMs.
        from .routes_assistant import _apply_scene_draft_resource_single

        try:
            return _apply_scene_draft_resource_single(
                state,
                arguments=payload,
                operation="update",
            )
        except HTTPException:
            raise
        except Exception as exc:
            logger.warning("program scene resource update failed", exc_info=True)
            return JSONResponse(
                status_code=400,
                content={"detail": str(exc)},
            )

    @router.post("/edit")
    def edit_scene(validated: SceneEditApiRequest) -> dict[str, Any]:
        """Edit the scene at runtime (add, upsert, remove, update_pose, attach, detach)."""
        with state.scene_edit_lock:
            if state.cycle_manager is not None and state.cycle_manager.state in {
                CycleState.RUNNING,
                CycleState.INTERRUPT_HANDLER,
                CycleState.DIRECT_CONTROL,
            }:
                cycle_state = state.cycle_manager.state
                return JSONResponse(
                    status_code=409,
                    content={
                        "success": False,
                        "error": (
                            "Scene edits are not allowed while execution is active "
                            f"(state={cycle_state.name})"
                        ),
                        "cycleState": cycle_state.value,
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
                mesh_resource_uri=validated.mesh_resource_uri,
                mesh_scale=validated.mesh_scale,
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
        """Render the active 3D viewport as an image.

        Day-4 of the native-viewer migration routes this through
        ``ComposerState._resolve_scene_snapshot_provider()`` so the same
        broker → HMI fallback chain used by the BT-node provider path
        (Day 3) also serves this HTTP route. Callers (assistant tools,
        external debug) get bytes from whichever viewer is currently
        attached; the legacy Viser HMI is fallback when no native
        viewer is connected.
        """
        provider = state._resolve_scene_snapshot_provider()
        if provider is None:
            return JSONResponse(
                status_code=503,
                content={
                    "detail": (
                        "No scene snapshot source available "
                        "(open Composer with the viewer panel visible, or start --hmi-port)"
                    ),
                },
            )
        image_bytes = provider(width, height, format)
        if image_bytes is None:
            return JSONResponse(
                status_code=503,
                content={"detail": "Scene snapshot source did not return an image in time"},
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
