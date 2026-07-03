"""Deterministic assistant tools used before any model-backed assistant runs."""
from __future__ import annotations

import importlib
import math
import sys
import time
from pathlib import Path
from typing import Any

# Canonical pose parser + quaternion->Euler converter (single source in the
# geometry module) so a pose supplied as orientation_xyzw is accepted without
# pre-converting, and a disagreeing orientation_xyzw/orientation_rpy_deg pair is
# rejected rather than silently picking one.
from manyforge_behavior.geometry import (
    PoseError,
    parse_pose,
    xyzw_to_rpy_deg as _xyzw_to_rpy_deg,
)


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
                "type": "object",
                "properties": {
                    "includeSubtrees": {
                        "type": "array",
                        "items": {"type": "string"},
                    },
                    "excludeSubtrees": {
                        "type": "array",
                        "items": {"type": "string"},
                    },
                    "probePrims": {
                        "type": "array",
                        "items": {"type": "string"},
                    },
                    "outputProfile": {
                        "type": "string",
                        "enum": ["llm_scene_authoring", "raw_inventory"],
                    },
                    "meshExportDir": {
                        "type": "string",
                        "description": (
                            "Optional Isaac-side directory where logical object "
                            "meshes should be exported as OBJ files."
                        ),
                    },
                    "meshSourcePreference": {
                        "type": "string",
                        "enum": ["auto", "original", "collider", "primitive"],
                        "description": (
                            "Select which mesh source should drive scene-object "
                            "recommendations. Use collider when the user wants "
                            "the collision mesh instead of the original visual mesh."
                        ),
                    },
                },
                "additionalProperties": False,
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
                "type": "object",
                "required": ["inspectionReport"],
                "properties": {
                    "inspectionReport": {"type": "object"},
                    "sourceRoots": {
                        "type": "array",
                        "items": {"type": "string"},
                    },
                    "objectIdPrefix": {"type": "string"},
                    "meshResourceOverrides": {
                        "type": "object",
                        "additionalProperties": {"type": "string"},
                        "description": (
                            "Optional sourceRoot -> mesh URI/path map. Use when "
                            "the user selected an existing mesh asset instead of "
                            "using the probe recommendation."
                        ),
                    },
                },
                "additionalProperties": False,
            },
        },
        {
            "name": "propose_scene_object_nodes",
            "description": (
                "Build reviewable tree-node draft proposals for scene object "
                "operations such as add/upsert/remove/attach/detach."
            ),
            "mutatesState": False,
            "requiresReview": True,
            "inputSchema": {
                "type": "object",
                "required": ["sceneResources", "operations"],
                "properties": {
                    "sceneResources": {
                        "type": "array",
                        "items": {"type": "object"},
                    },
                    "operations": {
                        "type": "array",
                        "items": {
                            "type": "string",
                            "enum": [
                                "add_collision_object",
                                "upsert_collision_object",
                                "remove_collision_object",
                                "attach_object_to_link",
                                "detach_object_from_link",
                            ],
                        },
                    },
                    "linkFrame": {"type": "string"},
                    "gripperId": {"type": "string"},
                },
                "additionalProperties": False,
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
            mesh_export_dir=request.mesh_export_dir,
            mesh_source_preference=request.mesh_source_preference,
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
    mesh_resource_overrides: dict[str, str] | None = None,
    max_proposals: int | None = None,
) -> dict[str, Any]:
    """Build non-mutating ManyForge scene-resource proposals from an Isaac report."""
    if not isinstance(inspection_report, dict):
        raise AssistantToolError("inspectionReport must be an object")
    objects = inspection_report.get("objects")
    if not isinstance(objects, list):
        raise AssistantToolError("inspectionReport.objects must be a list")

    selected = set(source_roots or [])
    mesh_overrides = mesh_resource_overrides or {}
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
                mesh_resource_uri_override=mesh_overrides.get(source_root),
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


def propose_scene_object_nodes(
    scene_resources: list[dict[str, Any]],
    *,
    operations: list[str] | None = None,
    link_frame: str = "tool0",
    gripper_id: str | None = None,
    name_prefix: str = "",
) -> dict[str, Any]:
    """Build non-mutating tree-node proposals for scene object operations."""
    if not isinstance(scene_resources, list):
        raise AssistantToolError("sceneResources must be a list")
    selected_operations = operations or ["add_collision_object"]
    for operation in selected_operations:
        if operation not in _SCENE_OBJECT_NODE_OPERATIONS:
            raise AssistantToolError(f"unsupported scene object node operation: {operation}")
    if any(
        operation in {"attach_object_to_link", "detach_object_from_link"}
        for operation in selected_operations
    ) and not link_frame.strip():
        raise AssistantToolError("linkFrame is required for attach/detach operations")

    proposals: list[dict[str, Any]] = []
    tree_nodes: list[dict[str, Any]] = []
    warnings: list[str] = []
    used_names: set[str] = set()

    for resource in scene_resources:
        if not isinstance(resource, dict):
            warnings.append("Skipped a non-object entry in sceneResources.")
            continue
        try:
            object_id = _required_string(resource, "objectId")
            shape = _validated_shape(resource.get("shape"))
            pose = _validated_pose(resource.get("pose"))
        except AssistantToolError as exc:
            warnings.append(f"Skipped invalid scene resource: {exc}")
            continue

        for operation in selected_operations:
            node = _scene_object_tree_node(
                operation=operation,
                object_id=object_id,
                shape=shape,
                pose=pose,
                link_frame=link_frame,
                gripper_id=gripper_id,
                name_prefix=name_prefix,
                used_names=used_names,
            )
            proposals.append(_proposal_from_tree_node(object_id, operation, node))
            tree_nodes.append(node)

    return {
        "toolName": "propose_scene_object_nodes",
        "proposals": proposals,
        "treeNodes": tree_nodes,
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
    mesh_resource_uri_override: str | None,
    used_ids: set[str],
) -> dict[str, Any]:
    source_root = _required_string(obj, "source_root")
    recommendation = obj.get("recommended_manyforge_shape")
    if not isinstance(recommendation, dict):
        raise AssistantToolError("missing recommended_manyforge_shape")

    shape = _validated_shape(recommendation.get("shape"))
    pose = _validated_pose(recommendation.get("pose"))
    if mesh_resource_uri_override is not None:
        if not isinstance(mesh_resource_uri_override, str) or not mesh_resource_uri_override.strip():
            raise AssistantToolError("mesh resource override must be a non-empty string")
        shape = {
            "type": "mesh",
            "mesh_resource_uri": mesh_resource_uri_override.strip(),
            "mesh_scale": [1.0, 1.0, 1.0],
        }
    object_id = _unique_id(
        _sanitize_object_id(f"{object_id_prefix}{obj.get('id_hint') or source_root}"),
        used_ids,
    )
    return {
        "objectId": object_id,
        "shape": shape,
        "pose": pose,
        "metadata": {},
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


_SCENE_OBJECT_NODE_OPERATIONS = {
    "add_collision_object",
    "upsert_collision_object",
    "remove_collision_object",
    "attach_object_to_link",
    "detach_object_from_link",
}


def _scene_object_tree_node(
    *,
    operation: str,
    object_id: str,
    shape: dict[str, Any],
    pose: dict[str, Any],
    link_frame: str,
    gripper_id: str | None,
    name_prefix: str,
    used_names: set[str],
) -> dict[str, Any]:
    safe_id = _sanitize_object_id(object_id)
    node_name = _unique_id(
        _sanitize_object_id(f"{name_prefix}{operation}_{safe_id}"),
        used_names,
    )
    params: dict[str, Any] = {"object_id": object_id}
    if operation in {"add_collision_object", "upsert_collision_object"}:
        params.update(
            {
                "shape_type": shape["type"],
                "pose": pose,
                "frame_id": pose["frame_id"],
            }
        )
        if shape["type"] == "box":
            params["box_dimensions_m"] = shape["box_dimensions_m"]
        elif shape["type"] == "sphere":
            params["sphere_radius_m"] = shape["sphere_radius_m"]
        elif shape["type"] == "mesh":
            params["mesh_resource_uri"] = shape["mesh_resource_uri"]
            params["mesh_scale"] = shape.get("mesh_scale", [1.0, 1.0, 1.0])
    elif operation in {"attach_object_to_link", "detach_object_from_link"}:
        params["link_frame"] = link_frame
        if operation == "attach_object_to_link" and gripper_id:
            params["gripper_id"] = gripper_id
    return {
        "id": operation,
        "name": node_name,
        "params": params,
        "children": [],
    }


def _proposal_from_tree_node(
    object_id: str,
    operation: str,
    tree_node: dict[str, Any],
) -> dict[str, Any]:
    return {
        "id": f"proposal-tree-node-{tree_node['name']}",
        "type": "tree_node_add",
        "title": f"Add {operation} node for {object_id}",
        "summary": (
            "Draft behavior-tree node for a scene object operation. "
            "This is not inserted into the tree until reviewed."
        ),
        "status": "draft",
        "reviewRequired": True,
        "payload": {
            "action": "add_tree_node",
            "treeNode": tree_node,
            "sourceObjectId": object_id,
            "operation": operation,
        },
        "warnings": [
            "Tree-node proposal is not applied. User review and insertion point selection are required."
        ],
        "source": {
            "kind": "assistant_scene_object_node",
            "sourceObjectId": object_id,
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
    if shape_type == "cylinder":
        radius = _finite_number(value.get("cylinder_radius_m"), "cylinder_radius_m")
        height = _finite_number(value.get("cylinder_height_m"), "cylinder_height_m")
        if radius <= 0.0:
            raise AssistantToolError("cylinder_radius_m must be > 0")
        if height <= 0.0:
            raise AssistantToolError("cylinder_height_m must be > 0")
        return {
            "type": "cylinder",
            "cylinder_radius_m": radius,
            "cylinder_height_m": height,
        }
    if shape_type == "mesh":
        resource_uri = value.get("mesh_resource_uri")
        if not isinstance(resource_uri, str) or not resource_uri.strip():
            raise AssistantToolError("mesh_resource_uri is required")
        scale = _finite_number_list(value.get("mesh_scale", [1.0, 1.0, 1.0]), 3, "mesh_scale")
        if any(component <= 0.0 for component in scale):
            raise AssistantToolError("mesh_scale values must be > 0")
        return {
            "type": "mesh",
            "mesh_resource_uri": resource_uri.strip(),
            "mesh_scale": scale,
        }
    raise AssistantToolError(f"unsupported recommended shape type: {shape_type!r}")


def _validated_pose(value: Any) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise AssistantToolError("recommended pose must be an object")
    # Fail-closed canonical gate: requires an orientation (no silent identity),
    # accepts orientation_xyzw OR orientation_rpy_deg, and rejects a both-present
    # pair that disagrees — the exact footgun where a model emitted a correct
    # quaternion alongside a wrong/zeroed rpy and the wrong one was kept.
    try:
        parse_pose(value)
    except PoseError as exc:
        raise AssistantToolError(str(exc)) from exc
    frame_id = _required_string(value, "frame_id")
    position = _finite_number_list(value.get("position_m"), 3, "position_m")
    # Emit the canonical single-orientation artifact shape (program.py requires
    # exactly one form). Prefer the author's orientation_rpy_deg verbatim; when
    # only the quaternion is given, convert it once via the shared geometry
    # converter so the model never does quaternion->Euler math itself.
    if value.get("orientation_rpy_deg") is not None:
        rpy = _finite_number_list(value.get("orientation_rpy_deg"), 3, "orientation_rpy_deg")
    else:
        quat = _finite_number_list(value.get("orientation_xyzw"), 4, "orientation_xyzw")
        rpy = [float(v) for v in _xyzw_to_rpy_deg(quat)]
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
