"""Scene routes - /api/scene/*"""
from __future__ import annotations

import logging
import math
import os
import time
import uuid
from typing import Any, Optional

from fastapi import APIRouter, HTTPException, Query
from fastapi.responses import JSONResponse, Response

from manyforge_behavior.kernel_client import SceneEditRequest

from .gripper_command import (
    GRIPPER_REASON_BACKEND_UNAVAILABLE,
    gripper_cycle_busy_detail,
    validate_gripper_command,
)

from .models import (
    SceneDiagnosticBackendResponse,
    SceneDiagnosticObjectResponse,
    SceneDiagnosticViewerResponse,
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
from manyforge_behavior.geometry import (
    rpy_deg_to_xyzw as _rpy_deg_to_xyzw,
    xyzw_to_rpy_deg as _xyzw_to_rpy_deg,
)
from .route_execution import run_route_background
from .state import ComposerState
from .viewer_data_plane import (
    ViewerInputUnchanged,
    ViewerSnapshotBuild,
    freeze_viewer_render_signature,
    freeze_viewer_signature,
)


def augment_pose_orientation_rpy_deg(value: Any) -> Any:
    """Recursively add ``orientation_rpy_deg`` next to any ``orientation_xyzw``.

    Scene state is authored/stored as quaternions, but the assistant pose-writing
    tools/nodes take Euler degrees. Small models reliably botch the quaternion->
    Euler conversion, so assistant-facing scene reads surface BOTH forms and the
    model copies the degrees directly (as it already copies position_m). This is
    lane-independent: applied to every assistant scene read — the direct provider
    context, the in-tree scene_inspect tool, and the MCP scene-state tool used by
    the gateway lanes. Additive + idempotent; ignores malformed quaternions.
    """
    if isinstance(value, dict):
        quat = value.get("orientation_xyzw")
        if (
            "orientation_rpy_deg" not in value
            and isinstance(quat, (list, tuple))
            and len(quat) == 4
        ):
            try:
                value["orientation_rpy_deg"] = _xyzw_to_rpy_deg(list(quat))
            except Exception:  # noqa: BLE001 — never let a bad quat break state
                pass
        for v in value.values():
            augment_pose_orientation_rpy_deg(v)
    elif isinstance(value, list):
        for v in value:
            augment_pose_orientation_rpy_deg(v)
    return value
from .visual_state import (
    VisualSnapshotUnavailable,
    visual_fk_model_descriptor,
)

logger = logging.getLogger(__name__)
_POSE_SOURCE_STATUSES = {"disabled", "fresh", "stale", "rejected"}
_SCENE_STATE_WARN_MS = 250.0
_SCENE_STATE_SUBSTEP_WARN_MS = 100.0


def _elapsed_ms(start: float) -> float:
    return (time.monotonic() - start) * 1000.0


def _continuous_pose_update_rejection(
    *,
    source_status: str,
    source_stale_after_s: float,
    source_id: str,
    object_state: dict[str, Any],
) -> str | None:
    """Classify whether one observation is usable for automatic scene apply."""
    if source_status != "fresh":
        return "source_not_fresh"
    if str(object_state.get("status") or "") != "fresh":
        return "object_not_fresh"
    if isinstance(object_state.get("rejection"), dict):
        return "object_rejected"
    age_s = object_state.get("age_s")
    if age_s is not None:
        try:
            parsed_age_s = float(age_s)
        except (TypeError, ValueError):
            return "age_invalid"
        if (
            not math.isfinite(parsed_age_s)
            or parsed_age_s < 0.0
            or parsed_age_s > float(source_stale_after_s)
        ):
            return "age_out_of_budget"
    update = object_state.get("update")
    if not isinstance(update, dict):
        return "update_missing"
    object_id = str(object_state.get("object_id") or "")
    if (
        str(update.get("pose_source_id") or "") != source_id
        or str(update.get("object_id") or "") != object_id
    ):
        return "update_identity_mismatch"
    pose = update.get("pose")
    if not isinstance(pose, dict):
        return "pose_missing"
    position = pose.get("position_xyz")
    orientation = pose.get("orientation_xyzw")
    if not isinstance(position, list) or len(position) != 3:
        return "position_shape_invalid"
    if not isinstance(orientation, list) or len(orientation) != 4:
        return "orientation_shape_invalid"
    try:
        position_values = [float(value) for value in position]
        orientation_values = [float(value) for value in orientation]
    except (TypeError, ValueError):
        return "pose_not_numeric"
    if not all(math.isfinite(value) for value in (*position_values, *orientation_values)):
        return "pose_not_finite"
    orientation_norm = math.sqrt(
        sum(value * value for value in orientation_values)
    )
    if orientation_norm <= 1e-12:
        return "orientation_zero_norm"
    return None


def _visual_publish_period_s() -> float:
    raw_value = os.environ.get("MANYFORGE_VIEWER_MAX_FRAME_HZ")
    if raw_value is None:
        return 1.0 / 15.0
    try:
        hz = float(raw_value)
    except ValueError:
        logger.warning(
            "invalid MANYFORGE_VIEWER_MAX_FRAME_HZ=%r; using 15 Hz",
            raw_value,
            extra={"event": "viewer_invalid_max_frame_hz"},
        )
        return 1.0 / 15.0
    hz = max(1.0, min(hz, 60.0))
    return 1.0 / hz


def _visual_snapshot_stale_after_ms() -> float:
    raw_value = os.environ.get("MANYFORGE_VIEWER_DEBUG_SNAPSHOT_STALE_AFTER_MS")
    if raw_value is not None:
        try:
            configured_ms = float(raw_value)
        except ValueError:
            logger.warning(
                "invalid MANYFORGE_VIEWER_DEBUG_SNAPSHOT_STALE_AFTER_MS=%r; using cadence-derived threshold",
                raw_value,
                extra={"event": "manyforge.viewer.invalid_debug_stale_threshold"},
            )
        else:
            if math.isfinite(configured_ms) and configured_ms > 0.0:
                return max(50.0, configured_ms)
            logger.warning(
                "invalid MANYFORGE_VIEWER_DEBUG_SNAPSHOT_STALE_AFTER_MS=%r; using cadence-derived threshold",
                raw_value,
                extra={"event": "manyforge.viewer.invalid_debug_stale_threshold"},
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
    return sorted(
        str(link.name)
        for link in getattr(entry, "links", ())
        if getattr(link, "name", None)
    )


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
            add_role(getattr(config, "mount_frame", ""), f"{gripper_id}:mount")
            add_role(getattr(config, "attach_frame", ""), f"{gripper_id}:attach")
            add_role(getattr(config, "tcp_frame", ""), f"{gripper_id}:tcp")
            for frame_name in getattr(config, "auxiliary_frames", ()):
                add_role(frame_name, f"{gripper_id}:auxiliary")

    return roles


def build_scene_frames_payload(state: ComposerState) -> dict[str, Any]:
    """Build known frame inventory plus current viewer visibility.

    Visibility state is owned by ComposerState (see `set_frame_visibility` /
    `get_frame_visibility` on the state object), so the response is independent
    of any browser connection. ``viewer_available`` therefore reports whether
    the native viewer surface is available for this route.
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
                # The snapshot is shared read-only; mutate a copy.
                visual_snapshot = visual_snapshot.model_copy(
                    update={"stale": True}
                )
                visual_route_fallback = (
                    snapshot_meta.get("last_error") or "visual_snapshot_stale"
                )
            pose_lookup = dict(visual_snapshot.link_poses)
            world_frame = visual_snapshot.world_frame or world_frame
            visual_route_mode = "visual_snapshot"
            visual_route_served = True
        else:
            state.request_visual_state_one_shot(reason="scene_frames_cache_empty")
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
        viewerAvailable=True,
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
                support_contacts=[
                    dict(item)
                    for item in getattr(scene_object, "support_contacts", [])
                    if isinstance(item, dict)
                ],
            )
        )
    return sorted(objects, key=lambda obj: obj.object_id)


def _pose_sources_payload(
    state: ComposerState,
    *,
    snapshots: dict[str, dict[str, Any]] | None = None,
) -> list[PoseSourceStatusResponse]:
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
    if snapshots is None:
        snapshots = state.pose_source_snapshots()
    drift_states = state.pose_source_drift_states()
    responses: list[PoseSourceStatusResponse] = []

    def scene_admission_payload(binding: Any) -> dict[str, Any] | None:
        admission = getattr(binding, "scene_admission", None)
        if admission is None:
            return None
        return {
            "role": admission.role,
            "leaseMaxAgeS": admission.lease_max_age_s,
            "targetDriftToleranceM": admission.target_drift_tolerance_m,
            "targetAngularToleranceRad": admission.target_angular_tolerance_rad,
            "obstacleDriftToleranceM": admission.obstacle_drift_tolerance_m,
            "obstacleAngularToleranceRad": admission.obstacle_angular_tolerance_rad,
            "allowRuntimeDynamicObstacle": admission.allow_runtime_dynamic_obstacle,
        }

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
                    scene_admission=scene_admission_payload(binding),
                    status=object_status,
                    age_s=observed.get("age_s"),
                    update=observed.get("update"),
                    rejection=observed.get("rejection"),
                    measured_state_ghost=(
                        None
                        if binding.measured_state_ghost is None
                        else {
                            "enabled": binding.measured_state_ghost.enabled,
                            "robotTcp": binding.measured_state_ghost.robot_tcp,
                            "object": binding.measured_state_ghost.object,
                            "opacity": binding.measured_state_ghost.opacity,
                        }
                    ),
                    attached_drift=drift_states.get((source_id, object_id)),
                )
            )
        measured_tcp = snapshot.get("measured_tcp") or snapshot.get("measuredTcp")
        measured_robot = snapshot.get("measured_robot") or snapshot.get("measuredRobot")
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
                measured_tcp=measured_tcp if isinstance(measured_tcp, dict) else None,
                measured_robot=(
                    measured_robot if isinstance(measured_robot, dict) else None
                ),
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


def _execution_authority_metadata(
    state: ComposerState,
    snapshot: Any | None = None,
) -> dict[str, Any]:
    """Return state-authority metadata for scene robot snapshots.

    Phase 4-P3 requires `/api/scene/state` consumers (native viewer and
    Cartesian gizmo included) to be auditable against the same state authority
    as `/api/kernel/snapshot`. This helper is only called by the visual-state
    publisher or its cold-cache fallback, never while serving a cached
    scene-state snapshot.
    """

    if state.kernel is None:
        return {}
    try:
        if snapshot is None:
            snapshot = (
                state.cycle_manager.get_execution_snapshot()
                if state.cycle_manager is not None
                else state.kernel.get_execution_snapshot()
            )
    except Exception:
        logger.debug(
            "execution authority metadata unavailable for scene robot state",
            exc_info=True,
            extra={"event": "manyforge.visual.authority_metadata_unavailable"},
        )
        return {}

    metadata: dict[str, Any] = {}
    state_source = getattr(snapshot, "state_source", "") or None
    if state_source is not None:
        metadata["state_source"] = state_source
    source_timestamp_ns = getattr(snapshot, "source_timestamp_ns", None)
    if source_timestamp_ns is not None:
        metadata["source_timestamp_ns"] = source_timestamp_ns
    source_age_ns = getattr(snapshot, "source_age_ns", None)
    if source_age_ns is not None:
        metadata["source_age_ns"] = source_age_ns
    return metadata


def _build_visual_state_snapshot_for_publisher(state: ComposerState):
    """Sample one immutable input generation and build it in the NRT worker."""
    if state.kernel is None:
        raise VisualSnapshotUnavailable("kernel_unavailable")

    sample_started = time.monotonic()
    stage_started = sample_started
    sample_stages_ms: dict[str, float] = {}

    def mark_stage(name: str) -> None:
        nonlocal stage_started
        now = time.monotonic()
        sample_stages_ms[f"sample_{name}"] = (now - stage_started) * 1000.0
        stage_started = now

    robot_id, group_id, base_frame, tcp_frame, base_pose = _base_robot_route_metadata(state)
    joint_names = state.get_drivable_joint_names()
    mark_stage("route_metadata")
    try:
        execution_snapshot = (
            state.cycle_manager.get_execution_snapshot()
            if state.cycle_manager is not None
            else state.kernel.get_execution_snapshot()
        )
    except Exception as exc:
        raise VisualSnapshotUnavailable(
            "execution_snapshot_unavailable",
            str(exc),
        ) from exc
    live_positions = state.resolve_current_joint_positions(
        execution_snapshot,
        joint_names=joint_names,
    )
    snapshot_positions = getattr(execution_snapshot, "joint_positions_rad", None)
    visual_source_stale = not (
        snapshot_positions is not None
        and len(snapshot_positions) == len(joint_names)
        and len(joint_names) > 0
    )
    mark_stage("execution_snapshot")
    joint_positions_by_name: dict[str, float] = {}
    if live_positions is not None and len(live_positions) == len(joint_names):
        joint_positions_by_name = {
            name: position for name, position in zip(joint_names, live_positions)
        }
    source_revisions = state.viewer_source_revisions()
    urdf_model_key = None
    try:
        from .routes_urdf import active_urdf_model_key

        urdf_model_key = state.resolve_viewer_model_key(
            generation=(source_revisions[0], source_revisions[1]),
            resolver=lambda: active_urdf_model_key(state),
        )
    except Exception:
        urdf_model_key = None
    if not urdf_model_key:
        raise VisualSnapshotUnavailable("visual_model_unavailable")
    mark_stage("model_key")

    robot_info = SceneRobotResponse(
        robot_id=robot_id,
        group_id=group_id,
        urdf_model_key=urdf_model_key,
        base_frame=base_frame,
        tcp_frame=tcp_frame,
        base_pose=base_pose,
        joint_names=joint_names,
        joint_positions=joint_positions_by_name,
        **_execution_authority_metadata(state, execution_snapshot),
    )

    gripper_statuses, gripper_status_signature = _collect_gripper_statuses(state)
    mark_stage("grippers")
    pose_source_snapshots, _pose_source_revision = (
        state.pose_source_snapshots_with_revision()
    )
    pose_sources = _pose_sources_payload(
        state,
        snapshots=pose_source_snapshots,
    )
    mark_stage("pose_sources")
    object_input = state.visual_scene_objects_input_snapshot()
    mark_stage("scene_objects")
    demand = state.viewer_demand_summary()
    demanded_layers = demand.layers or frozenset({"canonical", "diagnostics"})
    input_signature = freeze_viewer_signature(
        (
            source_revisions[0],
            source_revisions[1],
            freeze_viewer_render_signature(pose_sources),
            object_input[2].get("revision", 0),
            source_revisions[4],
            urdf_model_key,
            (
                robot_info.robot_id,
                robot_info.group_id,
                robot_info.base_frame,
                robot_info.tcp_frame,
                robot_info.base_pose,
                robot_info.state_source,
            ),
            joint_names,
            live_positions,
            gripper_status_signature,
            sorted(demanded_layers),
        )
    )
    mark_stage("signature")
    if not state.visual_input_requires_build(input_signature):
        sample_stages_ms["sample"] = _elapsed_ms(sample_started)
        raise ViewerInputUnchanged(sample_stages_ms)

    # Static gripper metadata (limits, mimic presentation, and visualization
    # subtree) is revision-bound topology. Resolve it only for a generation
    # that will actually be built; unchanged observer probes need only the
    # backend-owned revisioned status captured above.
    grippers = _collect_gripper_states(state, statuses=gripper_statuses)
    mark_stage("gripper_metadata")
    visual_joint_names, visual_joint_positions = _visual_fk_joint_inputs(
        joint_names=joint_names,
        joint_positions_rad=live_positions,
        grippers=grippers,
    )
    if visual_joint_positions is None:
        raise VisualSnapshotUnavailable("joint_state_unavailable")
    mark_stage("visual_joint_inputs")

    scene_objects, attachment_offsets, objects_meta = object_input
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

    static_input_key = freeze_viewer_signature(
        (
            source_revisions[0],
            source_revisions[1],
            int(objects_meta.get("revision", 0)),
            source_revisions[4],
            urdf_model_key,
            base_frame,
            tcp_frame,
        )
    )

    def build_static_input() -> dict[str, Any]:
        try:
            from .routes_urdf import visual_link_names_for_fk
        except Exception as exc:
            raise VisualSnapshotUnavailable(
                "visual_links_unavailable",
                str(exc),
            ) from exc

        object_responses = {
            item.object_id: item
            for item in _scene_object_responses(state, scene_objects)
        }
        object_rows_base = tuple(
            {
                "object_id": item.object_id,
                "response": object_responses[item.object_id].model_dump(mode="json"),
                "frame_id": item.frame_id,
            }
            for item in sorted(
                scene_objects,
                key=lambda candidate: candidate.object_id,
            )
        )
        frame_roles = _scene_frame_roles(state)
        frame_names = tuple(sorted({*frame_roles, *_load_model_frame_names(state)}))
        descriptor = visual_fk_model_descriptor(state)
        return {
            "descriptor": descriptor,
            "world_frame": canonical_scene_world_frame(state),
            "link_names": tuple(
                sorted({*visual_link_names_for_fk(state), base_frame, tcp_frame})
            ),
            "object_rows_base": object_rows_base,
            "frame_roles": {
                name: tuple(sorted(roles)) for name, roles in frame_roles.items()
            },
            "frame_names": frame_names,
            "frame_visibility": {
                name: state.get_frame_visibility(
                    name,
                    default=bool(frame_roles.get(name)),
                )
                for name in frame_names
            },
            "object_axes_gizmo_size_m": state.get_object_axes_gizmo_size(),
            "frame_axes_gizmo_size_m": state.get_frame_gizmo_size(),
        }

    static_input = state.resolve_viewer_static_input(
        key=static_input_key,
        resolver=build_static_input,
    )
    mark_stage("static_input")
    descriptor = static_input["descriptor"]
    world_frame = static_input["world_frame"]
    link_names = static_input["link_names"]
    object_rows = tuple(
        {
            "response": row["response"],
            "frame_id": row["frame_id"],
            # Derived attachment offsets are a mutable observer cache within
            # one object revision. Keep them outside the topology cache so an
            # attached object follows the robot on the very next frame.
            "attachment_offset": attachment_offsets.get(row["object_id"]),
        }
        for row in static_input["object_rows_base"]
    )
    worker_payload = {
        "model_descriptor": {
            "model_key": descriptor.model_key,
            "canonical_urdf_path": descriptor.canonical_urdf_path,
            "authority_metadata": descriptor.authority_metadata,
        },
        "robot": robot_info.model_dump(mode="json"),
        "visual_joint_names": visual_joint_names,
        "visual_joint_positions": visual_joint_positions,
        "link_names": link_names,
        "world_frame": world_frame,
        "base_frame": base_frame,
        "base_pose": base_pose,
        "grippers": [item.model_dump(mode="json") for item in grippers],
        "pose_sources": [item.model_dump(mode="json") for item in pose_sources],
        "stale": visual_source_stale,
        "objects": object_rows,
        "object_revision": int(objects_meta.get("revision", 0)),
        "demanded_layers": sorted(demanded_layers),
        "frame_roles": static_input["frame_roles"],
        "frame_names": static_input["frame_names"],
        "frame_visibility": static_input["frame_visibility"],
        "object_axes_gizmo_size_m": static_input["object_axes_gizmo_size_m"],
        "frame_axes_gizmo_size_m": static_input["frame_axes_gizmo_size_m"],
    }
    mark_stage("payload_assembly")
    sample_duration_ms = _elapsed_ms(sample_started)
    sample_stages_ms["sample"] = sample_duration_ms
    build = state.build_visual_state_in_worker(
        worker_payload,
        input_signature=input_signature,
        sample_duration_ms=sample_duration_ms,
        sample_stage_durations_ms=sample_stages_ms,
    )
    # A newer source generation may arrive while the worker builds. This result
    # remains a coherent, timestamped generation and is safe to publish; the
    # wake counter immediately schedules the coalesced latest input. Only the
    # derived cache write is generation-fenced.
    state.commit_visual_attachment_offsets(
        expected_revision=build.object_revision or 0,
        offsets=build.derived_attachment_offsets,
    )
    return build


def _ensure_visual_state_publisher(state: ComposerState, *, reason: str) -> None:
    state.ensure_visual_state_publisher(
        lambda: _build_visual_state_snapshot_for_publisher(state),
        period_s=_visual_publish_period_s(),
    )


def ensure_viewer_state_publisher(state: ComposerState, *, reason: str) -> None:
    """Public demand-channel entry point; starting it creates no demand."""
    _ensure_visual_state_publisher(state, reason=reason)


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
        state.request_visual_state_one_shot(reason="scene_state_cache_empty")
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
            **_execution_authority_metadata(state),
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
        # The snapshot is shared read-only; mutate a copy.
        visual_state = visual_state.model_copy(update={"stale": True})
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


def _collect_gripper_statuses(
    state: ComposerState,
) -> tuple[dict[str, Any], tuple[Any, ...]]:
    """Capture dynamic gripper status once and derive its cheap build key.

    VP-P0 makes status reads pure and gives every status a monotonic revision.
    The additional fields preserve correctness for test or third-party clients
    that have not yet supplied a useful revision; static deployment/model
    metadata is fenced separately by the viewer source generations and model
    key.
    """

    deployment = state.deployment
    kernel = state.kernel
    if deployment is None or kernel is None:
        return {}, ()
    statuses: dict[str, Any] = {}
    rows: list[tuple[Any, ...]] = []
    for gripper_id in deployment.grippers:
        try:
            status = kernel.get_gripper_status(gripper_id)
        except Exception as exc:
            logger.debug(
                "get_gripper_status failed for %s",
                gripper_id,
                exc_info=True,
            )
            rows.append((gripper_id, "unavailable", type(exc).__name__))
            continue
        statuses[gripper_id] = status
        raw_state = getattr(status, "state", "")
        rows.append(
            (
                gripper_id,
                int(getattr(status, "revision", 0) or 0),
                int(getattr(status, "command_id", 0) or 0),
                int(getattr(status, "generation", 0) or 0),
                str(getattr(raw_state, "value", raw_state)),
                getattr(status, "current_position", None),
                getattr(status, "target_position", None),
                getattr(status, "commanded_position", None),
                getattr(status, "observed_position", None),
                str(getattr(status, "commanded_state", "") or ""),
                str(getattr(status, "actuation_kind", "") or ""),
                str(getattr(status, "state_authority", "") or ""),
                str(getattr(status, "completion_policy", "") or ""),
                str(getattr(status, "grasp_confirmation_policy", "") or ""),
                str(getattr(status, "transport_kind", "") or ""),
                getattr(status, "evidence", ()),
                str(getattr(status, "evidence_state", "") or ""),
                str(
                    getattr(status, "projection_state", "not_required")
                    or "not_required"
                ),
                str(getattr(status, "projection_reason", "") or ""),
                getattr(status, "error", None),
                getattr(status, "reason_code", None),
            )
        )
    return statuses, tuple(rows)


def _collect_gripper_states(
    state: ComposerState,
    *,
    statuses: dict[str, Any] | None = None,
) -> list[SceneGripperStateResponse]:
    """One entry per gripper declared in the active deployment. Surfaces
    enough metadata for the Robot panel's gripper widget to render a
    slider with correct URDF limits and any SRDF-named presets, plus the
    live command/status from the kernel."""
    deployment = state.deployment
    kernel = state.kernel
    if deployment is None or kernel is None:
        return []
    joint_limits: dict[str, tuple[Optional[float], Optional[float]]] = {}
    derived_joints_by_gripper: dict[str, list[str]] = {}
    try:
        from .routes_urdf import get_urdf_entry, robot_model_authority_metadata

        entry = get_urdf_entry(state)
        if entry is not None:
            joint_limits = {
                joint.name: (joint.limit_lower, joint.limit_upper)
                for joint in entry.joints
            }
        metadata = robot_model_authority_metadata(state)
        raw_grippers = metadata.get("grippers") if isinstance(metadata, dict) else None
        if isinstance(raw_grippers, dict):
            for gripper_id, profile in raw_grippers.items():
                if not isinstance(profile, dict):
                    continue
                raw_derived = profile.get("derived_joints", [])
                if isinstance(raw_derived, (list, tuple)):
                    derived_joints_by_gripper[str(gripper_id)] = [
                        str(name) for name in raw_derived
                    ]
    except Exception:
        logger.debug("gripper limit cache build failed", exc_info=True)
    # Resolve auto-discovered visualization root + tcp→root SE(3)
    # once per scene-state call. Pure read against the URDF cache, so
    # the cost is one helper call per gripper per poll. This powers the
    # Move-To / validated Robot Control candidate unreachable-ghost
    # frontend fallback so the operator always sees a recognizable
    # gripper silhouette at the unreachable target, regardless of whether
    # tcp_frame is itself a visual link.
    try:
        from .routes_urdf import resolve_gripper_visualization
    except Exception:
        resolve_gripper_visualization = None  # type: ignore[assignment]
        logger.debug("routes_urdf not importable; scene gripper viz disabled", exc_info=True)

    out: list[SceneGripperStateResponse] = []
    for gripper_id, config in deployment.grippers.items():
        if statuses is None:
            try:
                status = kernel.get_gripper_status(gripper_id)
            except Exception:
                logger.debug("get_gripper_status failed for %s", gripper_id, exc_info=True)
                continue
        else:
            status = statuses.get(gripper_id)
            if status is None:
                continue
        limit_lower, limit_upper = joint_limits.get(
            config.joint_name,
            (None, None),
        )
        derived_joints = derived_joints_by_gripper.get(gripper_id)
        if derived_joints is None:
            try:
                from .routes_urdf import get_urdf_entry

                entry = get_urdf_entry(state)
                derived_joints = [
                    joint.name
                    for joint in (entry.joints if entry is not None else ())
                    if joint.mimic is not None and joint.mimic[0] == config.joint_name
                ]
            except Exception:
                derived_joints = []
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
                derived_joints=list(derived_joints),
                state=str(getattr(status.state, "value", status.state)),
                command_id=int(getattr(status, "command_id", 0) or 0),
                generation=int(getattr(status, "generation", 0) or 0),
                revision=int(getattr(status, "revision", 0) or 0),
                current_position=status.current_position,
                target_position=status.target_position,
                commanded_position=getattr(status, "commanded_position", None),
                observed_position=getattr(status, "observed_position", None),
                commanded_state=str(getattr(status, "commanded_state", "") or ""),
                actuation_kind=str(getattr(status, "actuation_kind", "") or ""),
                state_authority=str(getattr(status, "state_authority", "") or ""),
                completion_policy=str(
                    getattr(status, "completion_policy", "") or ""
                ),
                grasp_confirmation_policy=str(
                    getattr(status, "grasp_confirmation_policy", "") or ""
                ),
                transport_kind=str(getattr(status, "transport_kind", "") or ""),
                evidence=list(getattr(status, "evidence", []) or []),
                evidence_state=str(getattr(status, "evidence_state", "") or ""),
                projection_state=str(
                    getattr(status, "projection_state", "not_required")
                    or "not_required"
                ),
                projection_reason=str(
                    getattr(status, "projection_reason", "") or ""
                ),
                limit_lower=limit_lower,
                limit_upper=limit_upper,
                motion_time_s=config.motion_time_s,
                srdf_presets=dict(config.srdf_presets or {}),
                visualization=visualization,
                error=status.error,
                reason_code=getattr(status, "reason_code", None),
            )
        )
    return out


def build_scene_diagnostics_payload(state: ComposerState) -> dict[str, Any]:
    """Build backend vs active viewer scene diagnostics."""
    timestamp_ns = time.monotonic_ns()
    visual_objects, visual_meta = state.visual_scene_objects_snapshot()
    visual_meta = visual_meta or {}
    visual_index = {obj.object_id: obj for obj in visual_objects}
    native_visual_available = bool(visual_meta.get("timestamp_ns") or visual_objects)
    viewer_available = native_visual_available

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
            if native_visual_available:
                visual_object = visual_index.get(scene_object.object_id)
                rendered = visual_object is not None
                render_state_label = "synced" if rendered else "missing"
                viewer = SceneDiagnosticViewerResponse(
                    rendered=rendered,
                    scene_name=scene_object.object_id if rendered else None,
                    render_state=render_state_label,
                )
                if visual_object is None:
                    mismatch_reason = "Object exists in backend but not rendered in native visual cache"
                elif bool(visual_object.attached) != bool(scene_object.attached):
                    mismatch_reason = (
                        "Object attachment state differs between backend and native visual cache"
                    )
                elif (
                    scene_object.frame_id is not None
                    and visual_object.frame_id is not None
                    and str(scene_object.frame_id) != str(visual_object.frame_id)
                ):
                    mismatch_reason = (
                        "Object frame differs between backend and native visual cache"
                    )
            else:
                viewer = SceneDiagnosticViewerResponse(
                    rendered=False,
                    scene_name=None,
                    render_state="unavailable",
                )
                mismatch_reason = "Native visual cache unavailable"

            objects.append(
                SceneDiagnosticObjectResponse(
                    object_id=scene_object.object_id,
                    backend=backend,
                    viewer=viewer,
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
            viewerAvailable=viewer_available,
        ),
        timestamp_ns=timestamp_ns,
    ).model_dump(by_alias=True)


def _preview_joint_link_poses_via_visual_fk(
    state: ComposerState,
    payload: dict[str, Any],
) -> dict[str, Any] | None:
    """Compute visual-only joint preview poses from the full URDF model.

    Native planner FK can be scoped to the active planning group. That is
    correct for planning, but gripper candidate ghosts need full-URDF visual
    links outside the arm group. The visual snapshot model is kinematics-only
    and observer-safe: it computes poses for rendering without mutating the
    live scene or commanding motion.
    """
    try:
        joint_names = [str(name) for name in (payload.get("joint_names") or [])]
        joint_positions = [
            float(position) for position in (payload.get("joint_positions_rad") or [])
        ]
        link_names = [str(name) for name in (payload.get("link_names") or [])]
        if not joint_names or len(joint_names) != len(joint_positions) or not link_names:
            return None

        _robot_id, _group_id, base_frame, _tcp_frame, base_pose = (
            _base_robot_route_metadata(state)
        )
        descriptor = visual_fk_model_descriptor(state)
        link_poses = state.compute_visual_fk_preview_in_worker(
            {
                "model_descriptor": {
                    "model_key": descriptor.model_key,
                    "canonical_urdf_path": descriptor.canonical_urdf_path,
                    "authority_metadata": descriptor.authority_metadata,
                },
                "joint_names": joint_names,
                "joint_positions_rad": joint_positions,
                "link_names": link_names,
                "reference_frame": str(
                    payload.get("base_frame") or canonical_scene_world_frame(state)
                ),
                "base_frame": base_frame,
                "base_pose": base_pose,
            }
        )
    except (TypeError, ValueError) as exc:
        logger.debug(
            "visual FK preview rejected malformed joint payload",
            exc_info=True,
            extra={"event": "preview_link_poses_visual_fk_malformed"},
        )
        return {"reachable": False, "reason": f"malformed_joint_preview: {exc}"}
    except (VisualSnapshotUnavailable, RuntimeError) as exc:
        reason = str(getattr(exc, "reason", "") or "visual_fk_unavailable")
        logger.debug(
            "visual FK preview unavailable: %s",
            reason,
            exc_info=True,
            extra={"event": "preview_link_poses_visual_fk_unavailable"},
        )
        return {"reachable": False, "reason": reason}
    return {
        "reachable": True,
        "joint_names": joint_names,
        "joint_positions_rad": joint_positions,
        "link_poses": link_poses,
    }


def create_scene_router(state: ComposerState) -> APIRouter:
    """Create the scene router with the given state."""
    router = APIRouter()

    def live_scene_mutation_block_reason() -> str | None:
        return state.live_scene_mutation_block_reason()

    def live_scene_mutation_block_message(reason: str) -> str:
        if reason == "resumable_stopped_context":
            return (
                "Live scene edits are not allowed while execution is stopped "
                "with a resumable behavior-tree context. Resume the cycle, or "
                "Reset/re-plan before changing the live scene."
            )
        if reason.startswith("execution_active:"):
            return (
                "Scene edits are not allowed while execution is active "
                f"(state={reason.split(':', 1)[1]})"
            )
        return f"Live scene mutation is not allowed in the current runtime state ({reason})"

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
            admission = getattr(binding, "scene_admission", None)
            object_state["scene_admission"] = (
                None
                if admission is None
                else {
                    "role": admission.role,
                    "leaseMaxAgeS": admission.lease_max_age_s,
                    "targetDriftToleranceM": admission.target_drift_tolerance_m,
                    "targetAngularToleranceRad": (
                        admission.target_angular_tolerance_rad
                    ),
                    "obstacleDriftToleranceM": (
                        admission.obstacle_drift_tolerance_m
                    ),
                    "obstacleAngularToleranceRad": (
                        admission.obstacle_angular_tolerance_rad
                    ),
                    "allowRuntimeDynamicObstacle": (
                        admission.allow_runtime_dynamic_obstacle
                    ),
                }
            )
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
        continuous_apply_suppressed: list[dict[str, str]] = []
        for continuous_policy in ("continuous", "continuous_unattached"):
            continuous_object_ids: list[str] = []
            for object_state in sanitized_objects:
                if object_state.get("apply_policy") != continuous_policy:
                    continue
                object_id = str(object_state.get("object_id") or "")
                rejection_reason = _continuous_pose_update_rejection(
                    source_status=request.status,
                    source_stale_after_s=source.stale_after_s,
                    source_id=pose_source_id,
                    object_state=object_state,
                )
                state.record_pose_source_ingest_apply_decision(
                    source_id=pose_source_id,
                    object_id=object_id,
                    policy=continuous_policy,
                    rejection_reason=rejection_reason,
                )
                if rejection_reason is None:
                    continuous_object_ids.append(object_id)
                else:
                    continuous_apply_suppressed.append(
                        {
                            "object_id": object_id,
                            "policy": continuous_policy,
                            "reason": rejection_reason,
                        }
                    )
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
        logger.debug(
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
        if continuous_apply_suppressed:
            response["continuous_apply_suppressed"] = continuous_apply_suppressed
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
        per-link FK through the kernel's focused kinematics port without
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
        payload = dict(payload)
        kind = str(payload.get("kind") or "joint")
        compute = getattr(state.kernel, "compute_target_link_poses", None)
        if compute is None:
            # Older kernel client without the FK preview binding.
            if kind == "joint":
                fallback = _preview_joint_link_poses_via_visual_fk(state, payload)
                if fallback is not None:
                    return fallback
            return {"reachable": False, "reason": "unsupported_capability"}
        # Default link_names to all visual links if caller didn't
        # supply (frontend usually has the URDF manifest and passes
        # the link list explicitly, but a missing list is non-fatal).
        if not payload.get("link_names"):
            try:
                from .routes_urdf import visual_link_names_for_fk
                payload["link_names"] = list(visual_link_names_for_fk(state))
            except Exception:
                payload["link_names"] = []
        if not payload.get("base_frame"):
            payload["base_frame"] = canonical_scene_world_frame(state)
        try:
            result = compute(payload)
            if (
                kind == "joint"
                and result.get("reachable") is True
                and not result.get("link_poses")
            ):
                fallback = _preview_joint_link_poses_via_visual_fk(state, payload)
                if fallback is not None:
                    return fallback
            return result
        except Exception as exc:  # pragma: no cover — defensive
            if kind == "joint":
                fallback = _preview_joint_link_poses_via_visual_fk(state, payload)
                if fallback is not None:
                    return fallback
            return {"reachable": False, "reason": f"internal_error: {exc}"}

    @router.get("/diagnostics")
    def get_scene_diagnostics() -> dict[str, Any]:
        """Compare backend-authoritative scene truth with the native visual cache."""
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
            viewerAvailable=True,
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
            viewerAvailable=True,
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
            viewerAvailable=True,
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
        # Reject ad hoc commands while another authority owns execution, then
        # validate via the shared helper (same checks the candidate path uses).
        busy = gripper_cycle_busy_detail(state)
        if busy is not None:
            return JSONResponse(status_code=409, content={"detail": busy})
        validation = validate_gripper_command(
            state,
            gripper_id=validated.gripper_id,
            target_position=validated.target_position,
            max_effort=validated.max_effort,
            duration_s=validated.duration_s,
        )
        if not validation.ok or validation.request is None:
            status_code = (
                503
                if validation.reason_code == GRIPPER_REASON_BACKEND_UNAVAILABLE
                else 400
            )
            return JSONResponse(
                status_code=status_code, content={"detail": validation.detail}
            )
        result = state.kernel.command_gripper(validation.request)
        details = dict(getattr(result, "details", {}) or {})
        return SceneGripperCommandApiResponse(
            accepted=bool(result.accepted),
            gripperId=validated.gripper_id,
            targetPosition=validation.request.target_position,
            commandId=int(details.get("command_id", 0) or 0),
            generation=int(details.get("generation", 0) or 0),
            error=(None if result.accepted else (result.error or "rejected")),
            reasonCode=getattr(result, "reason_code", None),
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
            block_reason = live_scene_mutation_block_reason()
            if block_reason is not None:
                return JSONResponse(
                    status_code=409,
                    content={
                        "success": False,
                        "error": live_scene_mutation_block_message(block_reason),
                        "cycleState": (
                            state.cycle_manager.state.value
                            if state.cycle_manager is not None
                            else "idle"
                        ),
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
                state.invalidate_pose_source_applies(
                    reason=f"scene_edit_{validated.operation}"
                )
                visual_update = state.apply_visual_scene_edit_success(scene_request)
                if visual_update.get("needs_refresh"):
                    state.request_visual_scene_objects_refresh(reason="scene_edit")
        return SceneEditResponse(
            success=result.success,
            error=result.error,
            timestamp_ns=time.monotonic_ns(),
        ).model_dump()

    @router.get("/snapshot")
    async def get_scene_snapshot(
        width: int = Query(default=640, ge=64, le=1920),
        height: int = Query(default=480, ge=64, le=1080),
        format: str = Query(default="jpeg", pattern="^(jpeg|png)$"),
    ) -> Response:
        """Render the active 3D viewport as an image.

        This serves through the native-viewer snapshot broker used by the
        BT-node provider path. Keep the failure modes distinct so operators and
        tests can tell "no snapshot source exists" from "a connected source
        timed out".
        """
        def command() -> Response:
            broker = getattr(state, "snapshot_broker", None)

            if broker is not None and broker.has_clients():
                image_bytes = broker.render_sync(width, height, format)
                if image_bytes is None:
                    return JSONResponse(
                        status_code=503,
                        content={
                            "detail": "Scene snapshot source did not return an image in time"
                        },
                    )
            else:
                return JSONResponse(
                    status_code=503,
                    content={
                        "detail": "No native scene snapshot viewer connected",
                    },
                )

            media_type = "image/jpeg" if format == "jpeg" else "image/png"
            return Response(content=image_bytes, media_type=media_type)

        return await run_route_background(command)

    @router.get("/snapshots/{snapshot_id}")
    async def get_published_scene_snapshot(snapshot_id: str) -> Response:
        """Serve a previously captured scene snapshot by stable id."""
        def command() -> Response:
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

        return await run_route_background(command)

    return router
