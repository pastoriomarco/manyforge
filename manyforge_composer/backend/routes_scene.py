"""Scene routes - /api/scene/*"""
from __future__ import annotations

import logging
import math
import os
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
    PoseSourceApplyRequest,
    PoseSourceApplyResponse,
    PoseSourceObjectStateResponse,
    PoseSourceSnapshotRequest,
    PoseSourceStatusResponse,
    SceneRobotResponse,
    SceneShapeResponse,
    SceneStateResponse,
    VisualStateRouteDiagnosticsResponse,
)
from .state import ComposerState
from .visual_state import (
    VisualSnapshotUnavailable,
    build_robot_visual_snapshot,
    relative_pose_from_world,
)

logger = logging.getLogger(__name__)
_POSE_SOURCE_STATUSES = {"disabled", "fresh", "stale", "rejected"}
_SCENE_STATE_WARN_MS = 250.0
_SCENE_STATE_SUBSTEP_WARN_MS = 100.0


def _elapsed_ms(start: float) -> float:
    return (time.monotonic() - start) * 1000.0


def _visual_publish_period_s() -> float:
    raw_value = os.environ.get("MANYFORGE_VISUAL_STATE_PUBLISH_HZ")
    if raw_value is None:
        return 1.0 / 15.0
    try:
        hz = float(raw_value)
    except ValueError:
        logger.warning(
            "invalid MANYFORGE_VISUAL_STATE_PUBLISH_HZ=%r; using 15 Hz",
            raw_value,
            extra={"event": "visual_state_invalid_publish_hz"},
        )
        return 1.0 / 15.0
    hz = max(1.0, min(hz, 60.0))
    return 1.0 / hz


def _visual_snapshot_stale_after_ms() -> float:
    raw_value = os.environ.get("MANYFORGE_VISUAL_STATE_STALE_AFTER_MS")
    if raw_value is not None:
        try:
            configured_ms = float(raw_value)
        except ValueError:
            logger.warning(
                "invalid MANYFORGE_VISUAL_STATE_STALE_AFTER_MS=%r; using cadence-derived threshold",
                raw_value,
                extra={"event": "manyforge.visual.invalid_stale_threshold"},
            )
        else:
            if math.isfinite(configured_ms) and configured_ms > 0.0:
                return max(50.0, configured_ms)
            logger.warning(
                "invalid MANYFORGE_VISUAL_STATE_STALE_AFTER_MS=%r; using cadence-derived threshold",
                raw_value,
                extra={"event": "manyforge.visual.invalid_stale_threshold"},
            )
    return max(250.0, _visual_publish_period_s() * 3000.0)


def _visual_snapshot_is_stale(snapshot_meta: dict[str, Any] | None) -> bool:
    if not snapshot_meta:
        return False
    if snapshot_meta.get("last_error"):
        return True
    age_ms = snapshot_meta.get("age_ms")
    if age_ms is None:
        return False
    try:
        return float(age_ms) > _visual_snapshot_stale_after_ms()
    except (TypeError, ValueError):
        return False


def _visual_route_diagnostics(
    *,
    timings_ms: dict[str, float],
    total_ms: float,
    slow_steps: dict[str, float],
    mode: str = "visual_snapshot_pending",
    served_from_visual_snapshot: bool = False,
    fallback_reason: str | None = None,
    objects_source: str = "visual_snapshot_pending",
    objects_age_ms: float | None = None,
    objects_refresh_running: bool = False,
    objects_refresh_error: str | None = None,
    snapshot_meta: dict[str, Any] | None = None,
) -> VisualStateRouteDiagnosticsResponse:
    if fallback_reason is None and not served_from_visual_snapshot:
        fallback_reason = "visual_snapshot_publisher_unavailable"
    return VisualStateRouteDiagnosticsResponse(
        mode=mode,
        served_from_visual_snapshot=served_from_visual_snapshot,
        fallback_reason=fallback_reason,
        total_ms=round(total_ms, 1),
        timings_ms={name: round(duration, 1) for name, duration in timings_ms.items()},
        slow_steps=slow_steps,
        objects_source=objects_source,
        objects_age_ms=None if objects_age_ms is None else round(objects_age_ms, 1),
        objects_refresh_running=objects_refresh_running,
        objects_refresh_error=objects_refresh_error,
        snapshot_age_ms=(
            None
            if snapshot_meta is None or snapshot_meta.get("age_ms") is None
            else round(float(snapshot_meta["age_ms"]), 1)
        ),
        snapshot_sequence_number=(
            None if snapshot_meta is None else snapshot_meta.get("sequence_number")
        ),
        snapshot_build_ms=(
            None
            if snapshot_meta is None or snapshot_meta.get("last_duration_ms") is None
            else round(float(snapshot_meta["last_duration_ms"]), 1)
        ),
        snapshot_publisher_running=bool(
            snapshot_meta.get("publisher_running", False) if snapshot_meta else False
        ),
        snapshot_publisher_error=(
            None if snapshot_meta is None else snapshot_meta.get("last_error")
        ),
        snapshot_publisher_error_detail=(
            None if snapshot_meta is None else snapshot_meta.get("last_error_detail")
        ),
        snapshot_stale=_visual_snapshot_is_stale(snapshot_meta),
    )


def _visual_fk_joint_inputs(
    *,
    joint_names: list[str],
    joint_positions_rad: list[float] | None,
    grippers: list[SceneGripperStateResponse],
) -> tuple[list[str], list[float] | None]:
    """Merge execution joints and visual-only gripper feedback for FK.

    The scene-state robot block remains the execution group reported by the
    kernel. The visual FK surface also needs actuated gripper joints so links
    below the gripper mount move coherently in the viewer. Treat those values
    as observer inputs only; they are not execution authority for the arm.
    """

    if joint_positions_rad is None or len(joint_names) != len(joint_positions_rad):
        return list(joint_names), None

    # This function intentionally accepts a list of gripper state objects
    # rather than reading global deployment state. Today Composer has one
    # active robot model per scene-state response; future multi-robot support
    # can call this per visual robot with only that robot's grippers.
    visual_names = list(joint_names)
    visual_positions_by_name = {
        str(name): float(position)
        for name, position in zip(joint_names, joint_positions_rad)
    }
    for gripper in grippers:
        joint_name = (gripper.joint_name or "").strip()
        if not joint_name or gripper.current_position is None:
            continue
        try:
            position = float(gripper.current_position)
        except (TypeError, ValueError):
            continue
        if not math.isfinite(position):
            continue
        if joint_name not in visual_positions_by_name:
            visual_names.append(joint_name)
        visual_positions_by_name[joint_name] = position

    return visual_names, [visual_positions_by_name[name] for name in visual_names]


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
    build_started = time.monotonic()
    timings_ms: dict[str, float] = {}

    def mark_step(name: str, step_started: float) -> None:
        timings_ms[name] = _elapsed_ms(step_started)

    timestamp_ns = time.monotonic_ns()
    frame_roles = _scene_frame_roles(state)
    frame_names = set(frame_roles.keys())
    step_started = time.monotonic()
    frame_names.update(_load_model_frame_names(state))
    mark_step("frame_inventory", step_started)

    world_frame = canonical_scene_world_frame(state)

    sorted_frame_names = sorted(frame_names)
    pose_lookup: dict[str, Any] = {}
    unresolved_frame_names = [
        frame_name for frame_name in sorted_frame_names if frame_name != world_frame
    ]
    visual_route_mode = "visual_snapshot"
    visual_route_served = not bool(unresolved_frame_names)
    visual_route_fallback: str | None = None
    snapshot_meta: dict[str, Any] = {}
    if unresolved_frame_names:
        step_started = time.monotonic()
        _ensure_visual_state_publisher(state, reason="scene_frames_route")
        visual_snapshot, snapshot_meta = state.visual_state_snapshot()
        mark_step("snapshot_read", step_started)
        if visual_snapshot is not None:
            if _visual_snapshot_is_stale(snapshot_meta):
                visual_snapshot.stale = True
                visual_route_fallback = (
                    snapshot_meta.get("last_error") or "visual_snapshot_stale"
                )
            pose_lookup = dict(visual_snapshot.link_poses)
            world_frame = visual_snapshot.world_frame or world_frame
            visual_route_mode = "visual_snapshot"
            visual_route_served = True
        else:
            state.request_visual_state_publish(reason="scene_frames_cache_empty")
            visual_route_mode = "visual_snapshot_pending"
            visual_route_fallback = (
                snapshot_meta.get("last_error") or "visual_snapshot_pending"
            )
    if unresolved_frame_names and not visual_route_served:
        # The frames endpoint is an observer path. It never falls back to
        # route-time FK; the background publisher will fill the cache and
        # callers get poseAvailable=false until then.
        timings_ms.setdefault("visual_frame_snapshot", 0.0)

    frames: list[SceneFrameResponse] = []
    for frame_name in sorted_frame_names:
        pose_available = frame_name == world_frame
        pose_in_world: Optional[dict[str, Any]] = None
        if frame_name == world_frame:
            pose_in_world = {
                "position_m": [0.0, 0.0, 0.0],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            }
        else:
            pose = pose_lookup.get(frame_name)
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

    total_ms = _elapsed_ms(build_started)
    slow_steps = {
        name: round(duration, 1)
        for name, duration in timings_ms.items()
        if duration >= _SCENE_STATE_SUBSTEP_WARN_MS
    }
    diagnostics = _visual_route_diagnostics(
        timings_ms=timings_ms,
        total_ms=total_ms,
        slow_steps=slow_steps,
        mode=visual_route_mode,
        served_from_visual_snapshot=visual_route_served,
        fallback_reason=visual_route_fallback,
        snapshot_meta=snapshot_meta,
    )
    payload = SceneFramesResponse(
        frames=frames,
        hmiAvailable=True,
        frameGizmoSizeM=state.get_frame_gizmo_size(),
        visualStateDiagnostics=diagnostics,
        timestampNs=timestamp_ns,
    ).model_dump(by_alias=True)
    logger.debug(
        "scene frames build: mode=%s total_ms=%.1f frames=%d",
        diagnostics.mode,
        total_ms,
        len(frames),
        extra={
            "event": "scene_frames_build",
            "route_mode": diagnostics.mode,
            "served_from_visual_snapshot": diagnostics.served_from_visual_snapshot,
            "duration_ms": round(total_ms, 1),
            "slow_steps": slow_steps,
        },
    )
    if total_ms >= _SCENE_STATE_WARN_MS or slow_steps:
        logger.warning(
            "scene frames build slow: mode=%s total_ms=%.1f slow_steps=%s "
            "timings_ms=%s frames=%d",
            diagnostics.mode,
            total_ms,
            slow_steps,
            {name: round(duration, 1) for name, duration in timings_ms.items()},
            len(frames),
            extra={
                "event": "scene_frames_build_slow",
                "route_mode": diagnostics.mode,
                "served_from_visual_snapshot": diagnostics.served_from_visual_snapshot,
            },
        )
    return payload


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


def _scene_object_responses(
    state: ComposerState,
    scene_objects: list[Any],
) -> list[SceneObjectResponse]:
    authored_object_ids = {
        resource.object_id
        for resource in (state.program.scene if state.program is not None else [])
    }
    objects: list[SceneObjectResponse] = []
    for scene_object in scene_objects:
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
    return sorted(objects, key=lambda obj: obj.object_id)


def _pose_sources_payload(state: ComposerState) -> list[PoseSourceStatusResponse]:
    """Merge deployment pose-source declarations with latest sidecar snapshots.

    This is intentionally observe-only. Runtime sidecars can report freshness
    and object poses, but this payload does not alter `objects[].pose_in_universe`
    or any kernel planning-scene state.
    """
    deployment = state.deployment
    pose_sources = (
        getattr(deployment, "pose_sources", {}) if deployment is not None else {}
    )
    if not pose_sources:
        return []
    snapshots = state.pose_source_snapshots()
    responses: list[PoseSourceStatusResponse] = []

    for source_id, source in sorted(pose_sources.items()):
        snapshot = snapshots.get(source_id, {})
        status = str(
            snapshot.get("status") or ("stale" if source.enabled else "disabled")
        )
        if status not in _POSE_SOURCE_STATUSES:
            status = "rejected"
        if not source.enabled:
            status = "disabled"
        snapshot_objects = {
            str(item.get("object_id") or item.get("objectId") or ""): item
            for item in snapshot.get("objects", [])
            if isinstance(item, dict)
        }
        object_states: list[PoseSourceObjectStateResponse] = []
        for object_id, binding in sorted(source.objects.items()):
            observed = snapshot_objects.get(object_id, {})
            object_status = str(observed.get("status") or status)
            if object_status not in _POSE_SOURCE_STATUSES:
                object_status = "rejected"
            if not source.enabled:
                object_status = "disabled"
            object_states.append(
                PoseSourceObjectStateResponse(
                    object_id=object_id,
                    entity=binding.entity,
                    frame_id=binding.frame_id or source.frame_id,
                    apply_policy=binding.apply_policy,
                    status=object_status,
                    age_s=observed.get("age_s"),
                    update=observed.get("update"),
                    rejection=observed.get("rejection"),
                )
            )
        responses.append(
            PoseSourceStatusResponse(
                pose_source_id=source_id,
                kind=source.kind,
                enabled=source.enabled,
                status=status,
                max_update_hz=source.max_update_hz,
                stale_after_s=source.stale_after_s,
                frame_id=source.frame_id,
                description=source.description,
                received_at_ns=snapshot.get("received_at_ns"),
                objects=object_states,
            )
        )
    return responses


def _base_robot_route_metadata(
    state: ComposerState,
) -> tuple[str | None, str | None, str, str, dict[str, Any]]:
    robot_id = None
    group_id = None
    base_frame = "base_link"
    tcp_frame = "tool0"
    base_pose: dict[str, Any] = {}
    if state.deployment is not None and state.deployment.robot_config is not None:
        robot_id = state.deployment.robot_config.robot_id
        group_id = state.deployment.robot_config.group_id
    program_robot = getattr(state.program, "robot", None) if state.program is not None else None
    if program_robot is not None:
        base_frame = program_robot.base_frame
        tcp_frame = program_robot.tcp_frame
        base_pose = dict(program_robot.base_pose or {})
    return robot_id, group_id, base_frame, tcp_frame, base_pose


def _build_visual_state_snapshot_for_publisher(state: ComposerState):
    """Build the complete visual scene snapshot on the publisher thread.

    This is the only visual-snapshot path that may call backend observer APIs.
    HTTP routes must read the latest published snapshot instead of calling this
    function directly.
    """
    if state.kernel is None:
        raise VisualSnapshotUnavailable("kernel_unavailable")

    robot_id, group_id, base_frame, tcp_frame, base_pose = _base_robot_route_metadata(state)
    joint_names = state.get_drivable_joint_names()
    live_positions = state.get_current_joint_positions(
        joint_names=joint_names,
        allow_backend_joint_names=False,
    )
    joint_positions_by_name: dict[str, float] = {}
    if live_positions is not None and len(live_positions) == len(joint_names):
        joint_positions_by_name = {
            name: position for name, position in zip(joint_names, live_positions)
        }
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
    )

    grippers = _collect_gripper_states(state, advance_commands=False)
    pose_sources = _pose_sources_payload(state)

    try:
        from .routes_urdf import visual_link_names_for_fk
    except Exception as exc:
        raise VisualSnapshotUnavailable("visual_links_unavailable", str(exc)) from exc

    world_frame = canonical_scene_world_frame(state)
    link_names = sorted({*visual_link_names_for_fk(state), base_frame, tcp_frame})
    visual_joint_names, visual_joint_positions = _visual_fk_joint_inputs(
        joint_names=joint_names,
        joint_positions_rad=live_positions,
        grippers=grippers,
    )
    visual_state = build_robot_visual_snapshot(
        state,
        robot=robot_info,
        joint_names=visual_joint_names,
        joint_positions_rad=visual_joint_positions,
        link_names=link_names,
        reference_frame=world_frame,
        base_frame=base_frame,
        base_pose=base_pose,
        grippers=grippers,
        pose_sources=pose_sources,
    )
    link_poses = dict(visual_state.link_poses)
    if base_frame in link_poses and tcp_frame in link_poses:
        try:
            robot_info.tcp_pose_in_base = relative_pose_from_world(
                child_pose=link_poses[tcp_frame],
                parent_pose=link_poses[base_frame],
            )
        except VisualSnapshotUnavailable:
            logger.debug(
                "visual-state publisher could not derive tcp_pose_in_base",
                exc_info=True,
                extra={"event": "visual_snapshot_tcp_pose_unavailable"},
            )
    scene_objects, objects_meta = state.visual_scene_objects_for_link_poses(link_poses)
    object_refresh_attempt_age_ms = objects_meta.get("refresh_attempt_age_ms")
    object_refresh_retry_due = (
        object_refresh_attempt_age_ms is None
        or float(object_refresh_attempt_age_ms) >= 1000.0
    )
    if (
        not scene_objects
        and objects_meta.get("timestamp_ns", 0) == 0
        and not objects_meta.get("refresh_running", False)
        and object_refresh_retry_due
    ):
        state.request_visual_scene_objects_refresh(reason="visual_state_cache_empty")
    objects = _scene_object_responses(state, scene_objects)
    visual_state.robot = robot_info
    visual_state.objects = objects
    visual_state.grippers = grippers
    visual_state.pose_sources = pose_sources
    return visual_state


def _ensure_visual_state_publisher(state: ComposerState, *, reason: str) -> None:
    state.ensure_visual_state_publisher(
        lambda: _build_visual_state_snapshot_for_publisher(state),
        period_s=_visual_publish_period_s(),
    )


def _scene_state_payload_from_visual_cache(
    state: ComposerState,
    *,
    build_started: float,
    timestamp_ns: int,
) -> dict[str, Any]:
    timings_ms: dict[str, float] = {}
    step_started = time.monotonic()
    _ensure_visual_state_publisher(state, reason="scene_state_route")
    visual_state, snapshot_meta = state.visual_state_snapshot()
    timings_ms["snapshot_read"] = _elapsed_ms(step_started)
    total_ms = _elapsed_ms(build_started)
    slow_steps = {
        name: round(duration, 1)
        for name, duration in timings_ms.items()
        if duration >= _SCENE_STATE_SUBSTEP_WARN_MS
    }
    if visual_state is None:
        state.request_visual_state_publish(reason="scene_state_cache_empty")
        cached_objects, objects_meta = state.visual_scene_objects_snapshot()
        objects = _scene_object_responses(state, cached_objects)
        pose_sources = _pose_sources_payload(state)
        robot_id, group_id, base_frame, tcp_frame, base_pose = _base_robot_route_metadata(state)
        joint_names = state.get_drivable_joint_names(allow_backend_refresh=False)
        joint_positions: dict[str, float] = {}
        live_positions = state.get_current_joint_positions(
            joint_names=joint_names,
            allow_backend_joint_names=False,
        )
        if live_positions is not None and len(live_positions) == len(joint_names):
            joint_positions = {
                name: position for name, position in zip(joint_names, live_positions)
            }
        robot_info = SceneRobotResponse(
            robot_id=robot_id,
            group_id=group_id,
            base_frame=base_frame,
            tcp_frame=tcp_frame,
            base_pose=base_pose,
            joint_names=joint_names,
            joint_positions=joint_positions,
        )
        diagnostics = _visual_route_diagnostics(
            timings_ms=timings_ms,
            total_ms=total_ms,
            slow_steps=slow_steps,
            mode="visual_snapshot_pending",
            served_from_visual_snapshot=False,
            fallback_reason=snapshot_meta.get("last_error") or "visual_snapshot_pending",
            objects_source=(
                "visual_cache_pending"
                if objects_meta.get("timestamp_ns", 0)
                else "visual_snapshot_pending"
            ),
            objects_age_ms=objects_meta.get("age_ms"),
            objects_refresh_running=bool(objects_meta.get("refresh_running", False)),
            objects_refresh_error=objects_meta.get("refresh_error"),
            snapshot_meta=snapshot_meta,
        )
        return SceneStateResponse(
            robot=robot_info,
            objects=objects,
            link_poses={},
            grippers=[],
            pose_sources=pose_sources,
            visual_state=None,
            visual_state_diagnostics=diagnostics,
            object_axes_gizmo_size_m=state.get_object_axes_gizmo_size(),
            timestamp_ns=timestamp_ns,
        ).model_dump()

    stale = _visual_snapshot_is_stale(snapshot_meta)
    if stale:
        visual_state.stale = True
    diagnostics = _visual_route_diagnostics(
        timings_ms=timings_ms,
        total_ms=total_ms,
        slow_steps=slow_steps,
        mode="visual_snapshot",
        served_from_visual_snapshot=True,
        fallback_reason=(
            snapshot_meta.get("last_error")
            or ("visual_snapshot_stale" if stale else None)
        ),
        objects_source="visual_snapshot",
        objects_age_ms=snapshot_meta.get("age_ms"),
        snapshot_meta=snapshot_meta,
    )
    payload = SceneStateResponse(
        robot=visual_state.robot,
        objects=list(visual_state.objects),
        link_poses=dict(visual_state.link_poses),
        grippers=list(visual_state.grippers),
        pose_sources=list(visual_state.pose_sources),
        visual_state=visual_state,
        visual_state_diagnostics=diagnostics,
        object_axes_gizmo_size_m=state.get_object_axes_gizmo_size(),
        timestamp_ns=timestamp_ns,
    ).model_dump()
    logger.debug(
        "scene state cache read: total_ms=%.1f sequence=%s age_ms=%s",
        total_ms,
        snapshot_meta.get("sequence_number"),
        snapshot_meta.get("age_ms"),
        extra={
            "event": "manyforge.visual.route_served_snapshot",
            "route_mode": diagnostics.mode,
            "served_from_visual_snapshot": diagnostics.served_from_visual_snapshot,
            "duration_ms": round(total_ms, 1),
            "snapshot_age_ms": snapshot_meta.get("age_ms"),
            "snapshot_stale": stale,
        },
    )
    if total_ms >= _SCENE_STATE_WARN_MS or slow_steps:
        logger.warning(
            "scene state cache read slow: total_ms=%.1f slow_steps=%s",
            total_ms,
            slow_steps,
            extra={
                "event": "manyforge.visual.route_latency_slow",
                "route_mode": diagnostics.mode,
                "served_from_visual_snapshot": diagnostics.served_from_visual_snapshot,
            },
        )
    return payload


def build_scene_state_payload(state: ComposerState) -> dict[str, Any]:
    """Build backend-authoritative scene state from the visual snapshot cache.

    Scene-state reads are observer paths. They must not fall back to route-time
    FK, live scene-object reads, or other mutable planner-scene APIs. The
    background visual-state publisher owns those reads and exposes bounded
    snapshot diagnostics when a snapshot is not available yet.
    """

    return _scene_state_payload_from_visual_cache(
        state,
        build_started=time.monotonic(),
        timestamp_ns=time.monotonic_ns(),
    )


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

    @router.post("/pose-sources/{pose_source_id}/snapshot")
    def update_pose_source_snapshot(
        pose_source_id: str,
        request: PoseSourceSnapshotRequest,
    ) -> dict[str, Any]:
        """Record one latest-wins pose-source snapshot.

        Deployment configuration remains authoritative for source IDs, object
        bindings, and apply policies. Most policies are explicit safe-point
        latches; `continuous` is intentionally paced by this endpoint after the
        latest snapshot has been accepted, so provider cadence remains the only
        update loop and stale observations cannot backlog.
        """
        deployment = state.deployment
        source = None
        if deployment is not None:
            source = deployment.pose_sources.get(pose_source_id)
        if source is None:
            raise HTTPException(status_code=404, detail="pose source is not declared")
        if not source.enabled:
            raise HTTPException(status_code=409, detail="pose source is disabled")
        if request.pose_source_id != pose_source_id:
            raise HTTPException(
                status_code=400,
                detail="pose_source_id does not match path",
            )
        if request.status not in _POSE_SOURCE_STATUSES - {"disabled"}:
            raise HTTPException(status_code=400, detail="invalid pose-source status")

        payload = request.model_dump()
        sanitized_objects: list[dict[str, Any]] = []
        for object_state in payload.get("objects", []):
            object_id = str(object_state.get("object_id") or "")
            binding = source.objects.get(object_id)
            if binding is None:
                raise HTTPException(
                    status_code=400,
                    detail=(
                        f"pose source '{pose_source_id}' reported unknown "
                        f"object '{object_id}'"
                    ),
                )
            # The deployment, not the sidecar payload, owns policy and entity
            # binding. Normalize here so a stale or compromised sidecar cannot
            # alter scene-apply semantics.
            object_state["entity"] = binding.entity
            object_state["frame_id"] = binding.frame_id or source.frame_id
            object_state["apply_policy"] = binding.apply_policy
            update = object_state.get("update")
            if isinstance(update, dict):
                update["entity"] = binding.entity
                update["apply_policy"] = binding.apply_policy
            rejection = object_state.get("rejection")
            if isinstance(rejection, dict):
                rejection["entity"] = binding.entity
            sanitized_objects.append(object_state)
        payload["objects"] = sanitized_objects
        state.update_pose_source_snapshot(pose_source_id, payload)
        continuous_apply_results: list[dict[str, Any]] = []
        for continuous_policy in ("continuous", "continuous_unattached"):
            continuous_object_ids = [
                str(object_state.get("object_id") or "")
                for object_state in sanitized_objects
                if object_state.get("apply_policy") == continuous_policy
            ]
            if not continuous_object_ids:
                continue
            apply_result = state.enqueue_pose_source_apply(
                policy=continuous_policy,
                object_ids=continuous_object_ids,
                require_idle=False,
                reason=f"snapshot_{continuous_policy}",
            )
            continuous_apply_results.append(apply_result)
            if not apply_result.get("success", False):
                logger.warning(
                    "%s pose-source apply queue failed: source=%s objects=%s error=%s",
                    continuous_policy,
                    pose_source_id,
                    continuous_object_ids,
                    apply_result.get("error"),
                )
        logger.info(
            "pose source snapshot accepted: source=%s status=%s objects=%d",
            pose_source_id,
            request.status,
            len(sanitized_objects),
        )
        response = {"success": True, "pose_source_id": pose_source_id}
        if len(continuous_apply_results) == 1:
            response["continuous_apply"] = continuous_apply_results[0]
        elif continuous_apply_results:
            response["continuous_apply_results"] = continuous_apply_results
        return response

    @router.post("/pose-sources/apply", response_model=None)
    def apply_pose_source_updates(
        request: PoseSourceApplyRequest,
    ) -> Any:
        """Apply eligible pose-source updates to the planning scene.

        API callers may only use this as a bounded safe-point operation. The
        usual operator-facing path is `when_idle`; the behavior-tree
        before-planning latch calls the same ComposerState method directly.
        """
        if request.policy in {
            "continuous",
            "continuous_unattached",
            "before_planning",
            "between_nodes",
        }:
            return JSONResponse(
                status_code=409,
                content={
                    "success": False,
                    "policy": request.policy,
                    "error": (
                        f"{request.policy} pose-source applies are only allowed "
                        "from their configured runtime latch"
                    ),
                    "applied_count": 0,
                    "skipped_count": 0,
                    "rejected_count": 1,
                    "details": [],
                },
            )
        result = state.apply_pose_source_updates(
            policy=request.policy,
            object_ids=request.object_ids,
            require_idle=True,
            reason="api",
        )
        response = PoseSourceApplyResponse(**result).model_dump()
        if response["success"]:
            return response
        return JSONResponse(status_code=409, content=response)

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
            if result.success:
                visual_update = state.apply_visual_scene_edit_success(scene_request)
                if visual_update.get("needs_refresh"):
                    state.request_visual_scene_objects_refresh(reason="scene_edit")
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
