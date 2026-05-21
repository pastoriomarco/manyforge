"""Scene routes - /api/scene/*"""
from __future__ import annotations

import logging
import math
import time
from typing import Any, Optional

from fastapi import APIRouter, HTTPException, Query
from fastapi.responses import JSONResponse, Response

from manyforge_behavior.cycle_manager import CycleState
from manyforge_behavior.kernel_client import (
    GripperCommandRequest,
    SceneEditRequest,
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
    SceneGripperCommandApiRequest,
    SceneGripperCommandApiResponse,
    SceneGripperStateResponse,
    SceneObjectAxesGizmoSizeRequest,
    SceneObjectAxesGizmoSizeResponse,
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


def _load_model_frame_names(state: ComposerState) -> list[str]:
    try:
        from .routes_urdf import get_urdf_entry

        entry = get_urdf_entry(state)
    except Exception:
        logger.debug("Could not resolve URDF cache while building frame inventory", exc_info=True)
        return []
    if entry is None:
        return []
    return sorted(link.name for link in entry.links)


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

        # Only frames carrying a semantic role (world / program TCP /
        # gripper mount-attach-tcp-auxiliary) are visible by default;
        # structural URDF link frames stay off until the operator
        # explicitly toggles them on. Operator overrides (via
        # set_frame_visibility) take precedence over this default.
        default_visible = bool(frame_roles.get(frame_name))
        viewer_visible = state.get_frame_visibility(
            frame_name, default=default_visible
        )

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


def _scene_shape_response(state: ComposerState, scene_object) -> SceneShapeResponse | None:
    if scene_object.shape is None:
        return None
    mesh_asset_url = None
    mesh_asset_status = None
    if scene_object.shape.type == "mesh":
        try:
            from .routes_scene_meshes import scene_mesh_shape_metadata

            metadata = scene_mesh_shape_metadata(
                state,
                scene_object.shape.mesh_resource_uri,
                scene_object.shape.mesh_scale,
            )
            mesh_asset_url = metadata.get("mesh_asset_url")
            mesh_asset_status = metadata.get("mesh_asset_status")
        except Exception:
            logger.debug(
                "Could not resolve scene mesh visualization metadata for %s",
                getattr(scene_object, "object_id", "<unknown>"),
                exc_info=True,
            )
            mesh_asset_status = "parse_failed"
    payload: dict[str, Any] = {
        "type": scene_object.shape.type,
        "box_dimensions_m": scene_object.shape.box_dimensions_m,
        "sphere_radius_m": scene_object.shape.sphere_radius_m,
        "cylinder_radius_m": scene_object.shape.cylinder_radius_m,
        "cylinder_height_m": scene_object.shape.cylinder_height_m,
        "mesh_resource_uri": scene_object.shape.mesh_resource_uri,
        "mesh_scale": scene_object.shape.mesh_scale,
    }
    if scene_object.shape.type == "mesh":
        payload["mesh_asset_url"] = mesh_asset_url
        payload["mesh_asset_status"] = mesh_asset_status
    return SceneShapeResponse(**payload)


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

    _advance_gripper_commands(state)

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
        urdf_model_key = None
        try:
            from .routes_urdf import active_urdf_model_key

            urdf_model_key = active_urdf_model_key(state)
        except Exception:
            urdf_model_key = None
        robot_info = SceneRobotResponse(
            robot_id=robot_id,
            group_id=group_id,
            urdf_model_key=urdf_model_key,
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
                    shape=_scene_shape_response(state, scene_object),
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
            link_names = list(visual_link_names_for_fk(state))
            batch_getter = getattr(state.kernel, "get_link_poses", None)
            if callable(batch_getter):
                try:
                    raw_link_poses = batch_getter(link_names, world_frame)
                except Exception:
                    raw_link_poses = {}
                for link_name, pose in raw_link_poses.items():
                    link_poses[str(link_name)] = {
                        "position_m": list(pose["position_m"]),
                        "orientation_xyzw": list(pose["orientation_xyzw"]),
                    }
            else:
                for link_name in link_names:
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
        grippers=_collect_gripper_states(state, advance_commands=False),
        object_axes_gizmo_size_m=state.get_object_axes_gizmo_size(),
        timestamp_ns=timestamp_ns,
    ).model_dump()


def _gripper_joint_limits(
    state: ComposerState, joint_name: str
) -> tuple[Optional[float], Optional[float]]:
    """Look up a joint's URDF limit pair without re-importing the URDF
    parser at the top of this module (avoids a circular import)."""
    try:
        from .routes_urdf import get_urdf_entry
    except Exception:
        return (None, None)
    entry = get_urdf_entry(state)
    if entry is None:
        return (None, None)
    for joint in entry.joints:
        if joint.name == joint_name:
            return (joint.limit_lower, joint.limit_upper)
    return (None, None)


def _direct_gripper_cycle_blocker(state: ComposerState) -> JSONResponse | None:
    """Reject ad hoc gripper commands while another authority owns execution."""
    cycle_manager = state.cycle_manager
    if cycle_manager is None:
        return None
    cycle_state = cycle_manager.state
    if cycle_state in {
        CycleState.RUNNING,
        CycleState.INTERRUPT_HANDLER,
        CycleState.DIRECT_CONTROL,
    }:
        return JSONResponse(
            status_code=409,
            content={
                "detail": (
                    "Gripper direct commands are allowed only while the cycle is "
                    f"idle or stopped; current cycle state is {cycle_state.value}"
                )
            },
        )
    return None


def _advance_gripper_commands(state: ComposerState) -> None:
    kernel = state.kernel
    if kernel is None:
        return
    try:
        kernel.update_gripper_commands()
    except Exception:
        logger.debug("update_gripper_commands failed", exc_info=True)


def _collect_gripper_states(
    state: ComposerState,
    *,
    advance_commands: bool = True,
) -> list[SceneGripperStateResponse]:
    """One entry per gripper declared in the active deployment. Surfaces
    enough metadata for the Robot panel's gripper widget to render a
    slider with correct URDF limits and any SRDF-named presets, plus the
    live command/status from the kernel."""
    deployment = state.deployment
    kernel = state.kernel
    if deployment is None or kernel is None:
        return []
    # Advance any simulated_joint command in flight. The BT
    # `command_gripper` node ticks this on every BT tick; the operator's
    # widget is the only client outside the BT cycle, so without this
    # call the simulated finger position never progresses from its
    # "executing" snapshot toward the target.
    if advance_commands:
        _advance_gripper_commands(state)
    joint_limits: dict[str, tuple[Optional[float], Optional[float]]] = {}
    try:
        from .routes_urdf import get_urdf_entry

        entry = get_urdf_entry(state)
        if entry is not None:
            joint_limits = {
                joint.name: (joint.limit_lower, joint.limit_upper)
                for joint in entry.joints
            }
    except Exception:
        logger.debug("gripper limit cache build failed", exc_info=True)
    # Resolve auto-discovered visualization root + tcp→root SE(3)
    # once per scene-state call. Pure read against the URDF cache, so
    # the cost is one helper call per gripper per poll. ADR-0018 +
    # ADR-0016 audit-second-pass R-tier note: this powers the
    # Move-To unreachable-ghost frontend fallback so the operator
    # always sees a recognizable gripper silhouette at the unreachable
    # target, regardless of whether tcp_frame is itself a visual link.
    try:
        from .routes_urdf import resolve_gripper_visualization
    except Exception:
        resolve_gripper_visualization = None  # type: ignore[assignment]
        logger.debug("routes_urdf not importable; scene gripper viz disabled", exc_info=True)

    out: list[SceneGripperStateResponse] = []
    for gripper_id, config in deployment.grippers.items():
        try:
            status = kernel.get_gripper_status(gripper_id)
        except Exception:
            logger.debug("get_gripper_status failed for %s", gripper_id, exc_info=True)
            continue
        limit_lower, limit_upper = joint_limits.get(
            config.joint_name,
            (None, None),
        )
        visualization = None
        if resolve_gripper_visualization is not None:
            srdf_gripper_joints: tuple[str, ...] = ()
            joint_name = (config.joint_name or "").strip()
            if joint_name:
                srdf_gripper_joints = (joint_name,)
            try:
                resolution = resolve_gripper_visualization(
                    state,
                    mount_frame=config.mount_frame,
                    tcp_frame=config.tcp_frame,
                    srdf_gripper_joint_names=srdf_gripper_joints,
                )
            except Exception:
                logger.debug(
                    "Gripper visualization resolution failed for %s",
                    gripper_id,
                    exc_info=True,
                )
                resolution = None
            if resolution is not None:
                visualization = {
                    "gripper_root_link": resolution.gripper_root_link,
                    "tcp_to_root_position_m": list(
                        resolution.tcp_to_root_position_m
                    ),
                    "tcp_to_root_orientation_xyzw": list(
                        resolution.tcp_to_root_orientation_xyzw
                    ),
                    "subtree_links": [
                        {
                            "name": link.name,
                            "position_m": list(link.position_m),
                            "orientation_xyzw": list(link.orientation_xyzw),
                        }
                        for link in resolution.subtree_links
                    ],
                }
        out.append(
            SceneGripperStateResponse(
                gripper_id=gripper_id,
                joint_name=config.joint_name,
                state=str(getattr(status.state, "value", status.state)),
                current_position=status.current_position,
                target_position=status.target_position,
                limit_lower=limit_lower,
                limit_upper=limit_upper,
                motion_time_s=config.motion_time_s,
                srdf_presets=dict(config.srdf_presets or {}),
                visualization=visualization,
                error=status.error,
            )
        )
    return out


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

    @router.post("/preview-link-poses")
    def preview_link_poses(payload: dict[str, Any]) -> dict[str, Any]:
        """Compute link poses for a hypothetical robot configuration.

        Backs the Composer Move-To "ghost robot" preview. Stateless
        w.r.t. the live scene — runs IK (for cartesian targets) and
        per-link FK against the kernel's planning context without
        mutating scene joint state.

        Request:
            { "kind": "cartesian" | "joint",
              "base_frame": "base"|"universe"|...,
              # joint mode:
              "joint_names": [...], "joint_positions_rad": [...],
              # cartesian mode:
              "tcp_frame": "tool0",
              "tcp_position_m": [x, y, z],
              "tcp_orientation_xyzw": [x, y, z, w],
              # seed (cartesian, optional):
              "joint_names": [...], "joint_positions_rad": [...],
            }
        Response:
            { "reachable": true, "joint_names": [...],
              "joint_positions_rad": [...],
              "link_poses": {link: {position_m, orientation_xyzw}} }
            or:
            { "reachable": false, "reason": "ik_failed"|...,
              "tcp_pose": {position_m, orientation_xyzw} }
        """
        if state.kernel is None:
            return {"reachable": False, "reason": "no_kernel"}
        compute = getattr(state.kernel, "compute_target_link_poses", None)
        if compute is None:
            # Older kernel client without the FK preview binding.
            return {"reachable": False, "reason": "unsupported_capability"}
        # Default link_names to all visual links if caller didn't
        # supply (frontend usually has the URDF manifest and passes
        # the link list explicitly, but a missing list is non-fatal).
        payload = dict(payload)
        if not payload.get("link_names"):
            try:
                from .routes_urdf import visual_link_names_for_fk
                payload["link_names"] = list(visual_link_names_for_fk(state))
            except Exception:
                payload["link_names"] = []
        if not payload.get("base_frame"):
            payload["base_frame"] = canonical_scene_world_frame(state)
        try:
            return compute(payload)
        except Exception as exc:  # pragma: no cover — defensive
            return {"reachable": False, "reason": f"internal_error: {exc}"}

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

    @router.post("/gripper/command")
    def command_gripper(
        validated: SceneGripperCommandApiRequest,
    ) -> dict[str, Any]:
        """Send a position command to a deployment-declared gripper.

        Mirrors the ``command_gripper`` behaviour-tree node so the
        operator's interactive widget and the BT-driven sequencer share
        the exact same kernel API. The deployment YAML owns
        gripper-specific fields (``group_id``, ``joint_name``,
        ``motion_time_s``); the operator only specifies the target
        position and optional overrides.
        """
        deployment = state.deployment
        if deployment is None:
            return JSONResponse(
                status_code=400,
                content={"detail": "No deployment loaded"},
            )
        config = deployment.grippers.get(validated.gripper_id)
        if config is None:
            return JSONResponse(
                status_code=400,
                content={
                    "detail": (
                        f"Gripper '{validated.gripper_id}' is not declared in "
                        "the active deployment"
                    )
                },
            )
        if state.kernel is None:
            return JSONResponse(
                status_code=503,
                content={"detail": "Kernel is not attached"},
            )
        blocked = _direct_gripper_cycle_blocker(state)
        if blocked is not None:
            return blocked
        target_position = float(validated.target_position)
        if not math.isfinite(target_position):
            return JSONResponse(
                status_code=400,
                content={"detail": "targetPosition must be finite"},
            )
        max_effort = (
            None if validated.max_effort is None else float(validated.max_effort)
        )
        if max_effort is not None and (not math.isfinite(max_effort) or max_effort < 0.0):
            return JSONResponse(
                status_code=400,
                content={"detail": "maxEffort must be finite and >= 0"},
            )
        duration_s = (
            float(config.motion_time_s)
            if validated.duration_s is None
            else float(validated.duration_s)
        )
        if not math.isfinite(duration_s) or duration_s < 0.0:
            return JSONResponse(
                status_code=400,
                content={"detail": "durationS must be finite and >= 0"},
            )
        limit_lower, limit_upper = _gripper_joint_limits(state, config.joint_name)
        if limit_lower is None or limit_upper is None:
            return JSONResponse(
                status_code=400,
                content={
                    "detail": (
                        f"Cannot command gripper '{validated.gripper_id}' because "
                        f"joint '{config.joint_name}' has no finite URDF limits"
                    )
                },
            )
        if target_position < float(limit_lower) or target_position > float(limit_upper):
            return JSONResponse(
                status_code=400,
                content={
                    "detail": (
                        f"targetPosition {target_position} is outside limits "
                        f"[{float(limit_lower)}, {float(limit_upper)}] for "
                        f"gripper joint '{config.joint_name}'"
                    )
                },
            )
        request = GripperCommandRequest(
            gripper_id=validated.gripper_id,
            group_id=str(config.group_id),
            joint_name=str(config.joint_name),
            target_position=target_position,
            duration_s=duration_s,
            max_effort=max_effort,
        )
        result = state.kernel.command_gripper(request)
        return SceneGripperCommandApiResponse(
            accepted=bool(result.accepted),
            gripperId=validated.gripper_id,
            targetPosition=target_position,
            error=(None if result.accepted else (result.error or "rejected")),
            timestampNs=time.monotonic_ns(),
        ).model_dump(by_alias=True)

    @router.post("/objects/gizmo-size")
    def set_object_axes_gizmo_size(
        validated: SceneObjectAxesGizmoSizeRequest,
    ) -> dict[str, Any]:
        """Set global viewer size for scene-object axes (parallels the
        named-frame gizmo size, but tracked independently so operators
        can size each gizmo type to taste)."""
        try:
            state.set_object_axes_gizmo_size(validated.axes_length_m)
        except ValueError as exc:
            return JSONResponse(
                status_code=400,
                content={"detail": str(exc)},
            )
        return SceneObjectAxesGizmoSizeResponse(
            success=True,
            axesLengthM=state.get_object_axes_gizmo_size(),
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

            if (
                validated.operation in {"add", "upsert"}
                and validated.shape_type == "mesh"
            ):
                from .routes_scene_meshes import validate_scene_mesh_resource_or_409

                validate_scene_mesh_resource_or_409(
                    state,
                    validated.mesh_resource_uri,
                    validated.mesh_scale,
                )

            scene_request = SceneEditRequest(
                operation=validated.operation,
                object_id=validated.object_id,
                shape_type=validated.shape_type,
                box_dimensions_m=validated.box_dimensions_m,
                sphere_radius_m=validated.sphere_radius_m,
                cylinder_radius_m=validated.cylinder_radius_m,
                cylinder_height_m=validated.cylinder_height_m,
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

        Day-4 of the native-viewer migration serves this through the same
        native-viewer broker → legacy HMI fallback chain used by the BT-node
        provider path. Keep the failure modes distinct so operators and tests
        can tell "no snapshot source exists" from "a connected source timed
        out".
        """
        broker = getattr(state, "snapshot_broker", None)
        hmi_provider = (
            getattr(state.hmi_server, "get_cached_scene_snapshot", None)
            or getattr(state.hmi_server, "render_scene", None)
        )

        if broker is not None and broker.has_clients():
            image_bytes = broker.render_sync(width, height, format)
            if image_bytes is None:
                return JSONResponse(
                    status_code=503,
                    content={
                        "detail": "Scene snapshot source did not return an image in time"
                    },
                )
        elif hmi_provider is not None:
            image_bytes = hmi_provider(width, height, format)
            if image_bytes is None:
                return JSONResponse(
                    status_code=503,
                    content={"detail": "No Viser client connected"},
                )
        else:
            return JSONResponse(
                status_code=503,
                content={
                    "detail": (
                        "HMI server not running and no native scene snapshot "
                        "viewer connected"
                    ),
                },
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
