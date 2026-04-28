"""Deterministic assistant tools used before any model-backed assistant runs."""
from __future__ import annotations

import importlib
import math
import sys
import time
from pathlib import Path
from typing import Any


class AssistantToolError(RuntimeError):
    """Raised when an assistant tool cannot complete safely."""


def assistant_tool_catalog() -> list[dict[str, Any]]:
    """Return the deterministic assistant tool catalog."""
    return [
        {
            "name": "inspect_isaac_scene",
            "description": (
                "Read-only Isaac Sim collider/object inspection through the validated "
                "ManyForge ROS scene-inspection client."
            ),
            "mutatesState": False,
            "requiresReview": True,
            "inputSchema": {
                "includeSubtrees": "list[str]",
                "excludeSubtrees": "list[str]",
                "probePrims": "list[str]",
                "outputProfile": "llm_scene_authoring | raw_inventory",
            },
        },
        {
            "name": "propose_scene_objects",
            "description": (
                "Convert selected Isaac scene-inspection objects into reviewable "
                "ManyForge scene-resource draft proposals."
            ),
            "mutatesState": False,
            "requiresReview": True,
            "inputSchema": {
                "inspectionReport": "inspect_isaac_scene response",
                "sourceRoots": "list[str], optional explicit selection",
                "objectIdPrefix": "string, optional",
            },
        },
    ]


def inspect_isaac_scene_report(request: Any) -> dict[str, Any]:
    """Run the Isaac scene inspection client with lazy ROS imports."""
    inspect_module = _import_manyforge_ros_inspector()
    rclpy = importlib.import_module("rclpy")

    initialized_here = False
    if not rclpy.ok():
        rclpy.init(args=None)
        initialized_here = True

    node = rclpy.create_node(request.node_name)
    try:
        query = inspect_module.IsaacSceneQuery(
            include_subtrees=tuple(request.include_subtrees),
            exclude_subtrees=tuple(request.exclude_subtrees),
            probe_prims=tuple(request.probe_prims),
            include_hidden=request.include_hidden,
            include_cooked_convex=request.include_cooked_convex,
            include_cooked_samples=request.include_cooked_samples,
            max_colliders=request.max_colliders,
            purpose=tuple(request.purpose),
            output_profile=request.output_profile,
        )
        kwargs: dict[str, Any] = {"timeout_s": request.timeout_s}
        if request.request_topic:
            kwargs["request_topic"] = request.request_topic
        if request.response_topic:
            kwargs["response_topic"] = request.response_topic
        client = inspect_module.InspectIsaacSceneClient(node, **kwargs)
        return client.inspect(query)
    finally:
        node.destroy_node()
        if initialized_here:
            rclpy.shutdown()


def propose_scene_object_resources(
    inspection_report: dict[str, Any],
    *,
    source_roots: list[str] | None = None,
    object_id_prefix: str = "",
    max_proposals: int | None = None,
) -> dict[str, Any]:
    """Build non-mutating ManyForge scene-resource proposals from an Isaac report."""
    if not isinstance(inspection_report, dict):
        raise AssistantToolError("inspectionReport must be an object")
    objects = inspection_report.get("objects")
    if not isinstance(objects, list):
        raise AssistantToolError("inspectionReport.objects must be a list")

    selected = set(source_roots or [])
    warnings: list[str] = []
    proposals: list[dict[str, Any]] = []
    scene_resources: list[dict[str, Any]] = []
    used_ids: set[str] = set()

    for obj in objects:
        if not isinstance(obj, dict):
            warnings.append("Skipped a non-object entry in inspectionReport.objects.")
            continue
        try:
            source_root = _required_string(obj, "source_root")
        except AssistantToolError as exc:
            warnings.append(f"Skipped object with invalid source root: {exc}")
            continue
        if selected and source_root not in selected:
            continue

        try:
            scene_resource = _scene_resource_from_object(
                obj,
                object_id_prefix=object_id_prefix,
                used_ids=used_ids,
            )
        except AssistantToolError as exc:
            warnings.append(f"Skipped {source_root}: {exc}")
            continue

        proposal = _proposal_from_scene_resource(obj, scene_resource)
        proposals.append(proposal)
        scene_resources.append(scene_resource)
        if max_proposals is not None and len(proposals) >= max_proposals:
            break

    if selected:
        found_roots = {item.get("source_root") for item in objects if isinstance(item, dict)}
        missing = sorted(selected - found_roots)
        warnings.extend(f"Selected source root was not found: {root}" for root in missing)

    return {
        "toolName": "propose_scene_objects",
        "proposals": proposals,
        "sceneResources": scene_resources,
        "warnings": warnings,
        "mutated": False,
        "timestampNs": time.monotonic_ns(),
    }


def _import_manyforge_ros_inspector() -> Any:
    try:
        return importlib.import_module("manyforge_ros.inspect_isaac_scene")
    except ModuleNotFoundError:
        repo_root = Path(__file__).resolve().parents[2]
        ros_src = repo_root / "manyforge_ros"
        if ros_src.is_dir() and str(ros_src) not in sys.path:
            sys.path.insert(0, str(ros_src))
        try:
            return importlib.import_module("manyforge_ros.inspect_isaac_scene")
        except ModuleNotFoundError as exc:
            raise AssistantToolError(
                "manyforge_ros.inspect_isaac_scene is unavailable; run this tool "
                "from the ROS-capable ManyForge image or install the ROS package."
            ) from exc


def _scene_resource_from_object(
    obj: dict[str, Any],
    *,
    object_id_prefix: str,
    used_ids: set[str],
) -> dict[str, Any]:
    source_root = _required_string(obj, "source_root")
    recommendation = obj.get("recommended_manyforge_shape")
    if not isinstance(recommendation, dict):
        raise AssistantToolError("missing recommended_manyforge_shape")

    shape = _validated_shape(recommendation.get("shape"))
    pose = _validated_pose(recommendation.get("pose"))
    object_id = _unique_id(
        _sanitize_object_id(f"{object_id_prefix}{obj.get('id_hint') or source_root}"),
        used_ids,
    )
    return {
        "objectId": object_id,
        "shape": shape,
        "pose": pose,
    }


def _proposal_from_scene_resource(
    obj: dict[str, Any],
    scene_resource: dict[str, Any],
) -> dict[str, Any]:
    recommendation = obj.get("recommended_manyforge_shape") or {}
    source_root = _required_string(obj, "source_root")
    object_warnings = list(obj.get("warnings") or [])
    object_warnings.append(
        "Scene-resource proposal is not applied. User review is required before use."
    )
    return {
        "id": f"proposal-scene-object-{scene_resource['objectId']}",
        "type": "scene_resource_add",
        "title": f"Add planning object for {source_root}",
        "summary": (
            "Draft ManyForge scene resource derived from Isaac collider data. "
            "This is a planning approximation and must be reviewed."
        ),
        "status": "draft",
        "reviewRequired": True,
        "payload": {
            "action": "add_scene_resource",
            "sceneResource": scene_resource,
            "sourceRoot": source_root,
            "runtimePoseSourceHint": obj.get("runtime_pose_source_hint"),
            "confidence": recommendation.get("confidence"),
            "reason": recommendation.get("reason"),
            "colliderPaths": list(obj.get("collider_paths") or []),
        },
        "warnings": object_warnings,
        "source": {
            "kind": "isaac_scene_inspection",
            "sourceRoot": source_root,
        },
    }


def _validated_shape(value: Any) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise AssistantToolError("recommended shape must be an object")
    shape_type = value.get("type")
    if shape_type == "box":
        dims = _finite_number_list(value.get("box_dimensions_m"), 3, "box_dimensions_m")
        if any(component <= 0.0 for component in dims):
            raise AssistantToolError("box_dimensions_m values must be > 0")
        return {"type": "box", "box_dimensions_m": dims}
    if shape_type == "sphere":
        radius = _finite_number(value.get("sphere_radius_m"), "sphere_radius_m")
        if radius <= 0.0:
            raise AssistantToolError("sphere_radius_m must be > 0")
        return {"type": "sphere", "sphere_radius_m": radius}
    raise AssistantToolError(f"unsupported recommended shape type: {shape_type!r}")


def _validated_pose(value: Any) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise AssistantToolError("recommended pose must be an object")
    frame_id = _required_string(value, "frame_id")
    position = _finite_number_list(value.get("position_m"), 3, "position_m")
    rpy = _finite_number_list(value.get("orientation_rpy_deg"), 3, "orientation_rpy_deg")
    return {
        "frame_id": frame_id,
        "position_m": position,
        "orientation_rpy_deg": rpy,
    }


def _finite_number_list(value: Any, expected_len: int, label: str) -> list[float]:
    if not isinstance(value, list) or len(value) != expected_len:
        raise AssistantToolError(f"{label} must contain exactly {expected_len} values")
    return [_finite_number(item, f"{label}[{index}]") for index, item in enumerate(value)]


def _finite_number(value: Any, label: str) -> float:
    if isinstance(value, bool):
        raise AssistantToolError(f"{label} must be a finite number")
    try:
        result = float(value)
    except (TypeError, ValueError) as exc:
        raise AssistantToolError(f"{label} must be a finite number") from exc
    if not math.isfinite(result):
        raise AssistantToolError(f"{label} must be a finite number")
    return result


def _required_string(data: dict[str, Any], key: str) -> str:
    value = data.get(key)
    if not isinstance(value, str) or not value.strip():
        raise AssistantToolError(f"{key} must be a non-empty string")
    return value.strip()


def _sanitize_object_id(value: str) -> str:
    cleaned = []
    for char in value.strip("/"):
        cleaned.append(char if char.isalnum() or char == "_" else "_")
    return "".join(cleaned).strip("_") or "scene_object"


def _unique_id(base: str, used_ids: set[str]) -> str:
    candidate = base
    suffix = 2
    while candidate in used_ids:
        candidate = f"{base}_{suffix}"
        suffix += 1
    used_ids.add(candidate)
    return candidate
