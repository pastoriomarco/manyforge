"""Pure deterministic scene-selection context and response validation.

The pre-relocation nodes mixed these algorithms with Kernel discovery,
blackboard ownership, viewer rendezvous, and operator-pause state.  Only the
pure algorithms return here.  Production callers must provide one already
authoritative, revision-fenced scene projection and commit the result through
the Program value-store adapter.

Historical source: ``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1``
blob ``67559772eb32d1a4e8288f3f2d7140eb998d42b2``.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from copy import deepcopy
import json
import math
from typing import Any, Final

from manyforge_behavior.geometry import xyzw_to_rpy_deg


_MAX_CANDIDATES: Final = 4_096
_MAX_IDENTIFIER_BYTES: Final = 512
_MAX_REQUEST_BYTES: Final = 64 * 1024
_MAX_CONTEXT_BYTES: Final = 4 * 1024 * 1024
_MAX_MATCHED_ATTRIBUTES: Final = 512


class SceneSelectionError(ValueError):
    """A selection input is malformed, unavailable, or outside fixed bounds."""

    def __init__(self, reason_code: str, message: str) -> None:
        super().__init__(message)
        self.reason_code = reason_code


def _fail(reason_code: str, message: str) -> None:
    raise SceneSelectionError(reason_code, message)


def _object_without_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for name, value in pairs:
        if name in result:
            raise ValueError(f"duplicate JSON object name {name!r}")
        result[name] = value
    return result


def _reject_nonfinite_json(value: str) -> None:
    raise ValueError(f"non-finite JSON constant {value}")


def _copy_json(value: Any) -> Any:
    if isinstance(value, Mapping):
        return {name: _copy_json(nested) for name, nested in value.items()}
    if isinstance(value, (list, tuple)):
        return [_copy_json(nested) for nested in value]
    return deepcopy(value)


def _text(value: object, *, field: str, maximum_bytes: int) -> str:
    if (
        not isinstance(value, str)
        or not value
        or value != value.strip()
        or len(value.encode("utf-8")) > maximum_bytes
    ):
        _fail("selection_input_invalid", f"{field} must be bounded non-empty text")
    return value


def _number(value: object, *, field: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        _fail("selection_scene_projection_invalid", f"{field} must be numeric")
    result = float(value)
    if not math.isfinite(result):
        _fail("selection_scene_projection_invalid", f"{field} must be finite")
    return result


def _xyz(value: object, *, field: str) -> list[float]:
    if isinstance(value, Mapping):
        if set(value) != {"x", "y", "z"}:
            _fail(
                "selection_scene_projection_invalid",
                f"{field} must contain exactly x, y, and z",
            )
        raw = [value[axis] for axis in ("x", "y", "z")]
    elif (
        not isinstance(value, (str, bytes))
        and isinstance(value, Sequence)
        and len(value) == 3
    ):
        raw = list(value)
    else:
        _fail(
            "selection_scene_projection_invalid",
            f"{field} must contain three coordinates",
        )
    return [_number(item, field=f"{field}[{index}]") for index, item in enumerate(raw)]


def _quaternion(value: object, *, field: str) -> list[float]:
    if (
        isinstance(value, (str, bytes))
        or not isinstance(value, Sequence)
        or len(value) != 4
    ):
        _fail(
            "selection_scene_projection_invalid",
            f"{field} must contain four quaternion coordinates",
        )
    result = [
        _number(item, field=f"{field}[{index}]")
        for index, item in enumerate(value)
    ]
    norm = math.sqrt(sum(item * item for item in result))
    if abs(norm - 1.0) > 1.0e-6:
        _fail(
            "selection_scene_projection_invalid",
            f"{field} must be a unit quaternion",
        )
    return result


def _shape_summary(shape: Mapping[str, Any]) -> dict[str, Any]:
    kind = _text(
        shape.get("type"), field="collision shape type", maximum_bytes=64
    )
    if kind == "box":
        dimensions = _xyz(shape.get("dimensions_m"), field="box dimensions_m")
        if any(value <= 0.0 for value in dimensions):
            _fail(
                "selection_scene_projection_invalid",
                "box dimensions must be positive",
            )
        return {
            "type": "box",
            "box_dimensions_m": dimensions,
            "max_dimension_m": max(dimensions),
            "min_dimension_m": min(dimensions),
        }
    if kind == "sphere":
        radius = _number(shape.get("radius_m"), field="sphere radius_m")
        if radius <= 0.0:
            _fail(
                "selection_scene_projection_invalid",
                "sphere radius must be positive",
            )
        return {
            "type": "sphere",
            "sphere_radius_m": radius,
            "diameter_m": radius * 2.0,
            "max_dimension_m": radius * 2.0,
            "min_dimension_m": radius * 2.0,
        }
    if kind == "cylinder":
        radius = _number(shape.get("radius_m"), field="cylinder radius_m")
        height = _number(shape.get("height_m"), field="cylinder height_m")
        if radius <= 0.0 or height <= 0.0:
            _fail(
                "selection_scene_projection_invalid",
                "cylinder dimensions must be positive",
            )
        return {
            "type": "cylinder",
            "cylinder_radius_m": radius,
            "cylinder_height_m": height,
            "max_dimension_m": max(radius * 2.0, height),
            "min_dimension_m": min(radius * 2.0, height),
        }
    if kind == "mesh":
        resource_uri = _text(
            shape.get("resource_uri"),
            field="mesh resource_uri",
            maximum_bytes=4_096,
        )
        scale = _xyz(shape.get("scale", [1.0, 1.0, 1.0]), field="mesh scale")
        if any(value <= 0.0 for value in scale):
            _fail(
                "selection_scene_projection_invalid",
                "mesh scale must be positive",
            )
        return {
            "type": "mesh",
            "mesh_resource_uri": resource_uri,
            "mesh_scale": scale,
        }
    _fail(
        "selection_scene_projection_invalid",
        f"unsupported collision shape type {kind!r}",
    )


def _shape_top_offset(summary: Mapping[str, Any]) -> float:
    kind = summary["type"]
    if kind == "box":
        return float(summary["box_dimensions_m"][2]) / 2.0
    if kind == "sphere":
        return float(summary["sphere_radius_m"])
    if kind == "cylinder":
        return float(summary["cylinder_height_m"]) / 2.0
    return 0.0


def _candidate(raw: Mapping[str, Any], *, frame_id: str) -> dict[str, Any]:
    object_id = _text(
        raw.get("scene_resource_id"),
        field="scene_resource_id",
        maximum_bytes=_MAX_IDENTIFIER_BYTES,
    )
    raw_shapes = raw.get("collision_shapes")
    if (
        isinstance(raw_shapes, (str, bytes))
        or not isinstance(raw_shapes, Sequence)
        or not raw_shapes
        or len(raw_shapes) > 256
    ):
        _fail(
            "selection_scene_projection_invalid",
            f"scene resource {object_id!r} has no bounded collision shapes",
        )
    summaries: list[dict[str, Any]] = []
    for index, raw_shape in enumerate(raw_shapes):
        if not isinstance(raw_shape, Mapping):
            _fail(
                "selection_scene_projection_invalid",
                f"scene resource {object_id!r} shape {index} is not an object",
            )
        summaries.append(_shape_summary(raw_shape))
    shape: dict[str, Any]
    if len(summaries) == 1:
        shape = summaries[0]
    else:
        shape = {"type": "compound", "parts": summaries}

    pose = raw.get("pose_in_cell")
    if not isinstance(pose, Mapping) or set(pose) != {
        "position_m",
        "orientation_xyzw",
    }:
        _fail(
            "selection_object_pose_unavailable",
            f"scene resource {object_id!r} has no exact resolved cell pose",
        )
    position = _xyz(pose["position_m"], field=f"{object_id}.position_m")
    quaternion = _quaternion(
        pose["orientation_xyzw"], field=f"{object_id}.orientation_xyzw"
    )
    top_offset = max(_shape_top_offset(summary) for summary in summaries)
    return {
        "object_id": object_id,
        "shape": shape,
        "pose": {
            "frame_id": frame_id,
            "position_m": position,
            "orientation_xyzw": quaternion,
            "orientation_rpy_deg": xyzw_to_rpy_deg(quaternion),
        },
        "attached": raw.get("attached") is True,
        "frame_id": frame_id,
        "top_z_m": position[2] + top_offset,
        "spatial": {
            "distance_from_base_xy_m": math.hypot(position[0], position[1]),
            "distance_from_base_m": math.sqrt(sum(value * value for value in position)),
        },
    }


def build_scene_selection_context(
    *,
    selection_request_text: str,
    scene_objects: Sequence[Mapping[str, Any]],
    candidate_object_ids: Sequence[str] = (),
    frame_id: str,
    image_context: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    """Build the historical prompt-ready manifest from owner-issued facts.

    The public result contains exactly the three fields declared by the v1
    Program value contract.  The complete candidate manifest remains inside
    ``scene_context_json`` and therefore cannot become a second runtime cache.
    """

    request_text = _text(
        selection_request_text,
        field="selection_request_text",
        maximum_bytes=_MAX_REQUEST_BYTES,
    )
    frame = _text(frame_id, field="frame_id", maximum_bytes=_MAX_IDENTIFIER_BYTES)
    if (
        isinstance(scene_objects, (str, bytes))
        or not isinstance(scene_objects, Sequence)
        or len(scene_objects) > _MAX_CANDIDATES
    ):
        _fail(
            "selection_scene_projection_invalid",
            "scene object projection exceeds its fixed candidate bound",
        )
    if (
        isinstance(candidate_object_ids, (str, bytes))
        or not isinstance(candidate_object_ids, Sequence)
        or len(candidate_object_ids) > _MAX_CANDIDATES
    ):
        _fail(
            "selection_input_invalid",
            "candidate_object_ids exceeds its fixed bound",
        )
    requested = [
        _text(
            value,
            field=f"candidate_object_ids[{index}]",
            maximum_bytes=_MAX_IDENTIFIER_BYTES,
        )
        for index, value in enumerate(candidate_object_ids)
    ]
    if len(requested) != len(set(requested)):
        _fail("selection_input_invalid", "candidate_object_ids contains duplicates")
    requested_set = set(requested)

    candidates: list[dict[str, Any]] = []
    seen: set[str] = set()
    for index, raw in enumerate(scene_objects):
        if not isinstance(raw, Mapping):
            _fail(
                "selection_scene_projection_invalid",
                f"scene object {index} is not an object",
            )
        candidate = _candidate(raw, frame_id=frame)
        object_id = candidate["object_id"]
        if object_id in seen:
            _fail(
                "selection_scene_projection_invalid",
                f"scene projection repeats {object_id!r}",
            )
        seen.add(object_id)
        if not requested_set or object_id in requested_set:
            candidates.append(candidate)
    candidates.sort(key=lambda candidate: candidate["object_id"])
    if requested_set - seen:
        _fail(
            "selection_candidates_unavailable",
            "requested candidate scene resources are absent: "
            f"{sorted(requested_set - seen)}",
        )
    if not candidates:
        _fail(
            "selection_candidates_unavailable",
            "no candidate scene resources are available",
        )

    image = (
        {
            "source": "none",
            "role": "none",
            "is_canonical_view": False,
            "view_semantics": "no_image_used",
        }
        if image_context is None
        else _copy_json(image_context)
    )
    context = {
        "selection_request_text": request_text,
        "authoritative_source": "logical_scene_owner",
        "base_frame": frame,
        "spatial_semantics": {
            "frame_id": frame,
            "assumptions": [
                "object_ids_and_poses_come_from_the_logical_scene_owner",
                "left_means_greater_positive_y_in_cell_frame",
                "right_means_more_negative_y_in_cell_frame",
                "front_means_greater_positive_x_in_cell_frame",
                "back_means_more_negative_x_in_cell_frame",
                "up_means_greater_positive_z_in_cell_frame",
                "down_means_more_negative_z_in_cell_frame",
                "if_an_image_is_attached_it_is_auxiliary_evidence_only",
            ],
            "rankings": {
                "left_to_right_by_base_y_desc": [
                    row["object_id"]
                    for row in sorted(
                        candidates,
                        key=lambda item: (
                            -item["pose"]["position_m"][1],
                            item["object_id"],
                        ),
                    )
                ],
                "front_to_back_by_base_x_desc": [
                    row["object_id"]
                    for row in sorted(
                        candidates,
                        key=lambda item: (
                            -item["pose"]["position_m"][0],
                            item["object_id"],
                        ),
                    )
                ],
                "high_to_low_by_top_z_desc": [
                    row["object_id"]
                    for row in sorted(
                        candidates,
                        key=lambda item: (-item["top_z_m"], item["object_id"]),
                    )
                ],
            },
        },
        "image_context": image,
        "candidate_object_ids": [row["object_id"] for row in candidates],
        "candidate_count": len(candidates),
        "candidates": candidates,
    }
    encoded = json.dumps(
        context,
        allow_nan=False,
        ensure_ascii=False,
        indent=2,
        separators=(",", ": "),
    )
    if len(encoded.encode("utf-8")) > _MAX_CONTEXT_BYTES:
        _fail(
            "selection_context_oversized",
            "scene selection context exceeds its fixed byte bound",
        )
    return {
        "candidate_object_ids": context["candidate_object_ids"],
        "scene_context_json": encoded,
        "selection_request_text": request_text,
    }


def validate_scene_selection(
    *,
    selection_context: Mapping[str, Any],
    selection_response: Mapping[str, Any],
    allow_no_match: bool = False,
    allow_ambiguous: bool = False,
) -> dict[str, Any]:
    """Validate one model result against the retained candidate identity set."""

    if not isinstance(selection_context, Mapping) or not isinstance(
        selection_response, Mapping
    ):
        _fail(
            "selection_input_invalid",
            "selection context and response must be structured objects",
        )
    if type(allow_no_match) is not bool or type(allow_ambiguous) is not bool:
        _fail("selection_input_invalid", "selection policies must be boolean")
    if set(selection_context) != {
        "candidate_object_ids",
        "scene_context_json",
        "selection_request_text",
    }:
        _fail(
            "invalid_selection_context",
            "selection context does not have its closed Program shape",
        )
    if set(selection_response) != {
        "confidence",
        "matched_attributes",
        "outcome",
        "reason",
        "selected_object_id",
    }:
        _fail(
            "invalid_selection_response",
            "selection response does not have its closed Program shape",
        )
    raw_candidates = selection_context.get("candidate_object_ids")
    if (
        isinstance(raw_candidates, (str, bytes))
        or not isinstance(raw_candidates, Sequence)
        or not raw_candidates
        or len(raw_candidates) > _MAX_CANDIDATES
    ):
        _fail(
            "invalid_selection_context",
            "selection context has no bounded candidate identity set",
        )
    candidates = [
        _text(
            value,
            field=f"candidate_object_ids[{index}]",
            maximum_bytes=_MAX_IDENTIFIER_BYTES,
        )
        for index, value in enumerate(raw_candidates)
    ]
    if candidates != sorted(candidates) or len(candidates) != len(set(candidates)):
        _fail(
            "invalid_selection_context",
            "selection candidate identities are not canonical",
        )
    encoded_context = selection_context.get("scene_context_json")
    if (
        not isinstance(encoded_context, str)
        or len(encoded_context.encode("utf-8")) > _MAX_CONTEXT_BYTES
    ):
        _fail(
            "invalid_selection_context",
            "scene_context_json is absent or oversized",
        )
    try:
        manifest = json.loads(
            encoded_context,
            object_pairs_hook=_object_without_duplicates,
            parse_constant=_reject_nonfinite_json,
        )
    except (json.JSONDecodeError, RecursionError, TypeError, ValueError):
        _fail(
            "invalid_selection_context",
            "scene_context_json is not strict JSON",
        )
    if (
        not isinstance(manifest, dict)
        or manifest.get("candidate_object_ids") != candidates
        or manifest.get("selection_request_text")
        != selection_context.get("selection_request_text")
        or not isinstance(manifest.get("candidates"), list)
        or [
            row.get("object_id") if isinstance(row, dict) else None
            for row in manifest["candidates"]
        ]
        != candidates
    ):
        _fail(
            "invalid_selection_context",
            "scene selection manifest disagrees with its declared fields",
        )

    outcome = selection_response.get("outcome")
    if outcome not in {"selected", "no_match", "ambiguous"}:
        _fail(
            "invalid_selection_outcome",
            f"unsupported selection outcome {outcome!r}",
        )
    selected = selection_response.get("selected_object_id")
    if (
        not isinstance(selected, str)
        or len(selected.encode("utf-8")) > _MAX_IDENTIFIER_BYTES
    ):
        _fail(
            "invalid_selection_response",
            "selected_object_id must be bounded text",
        )
    reason = selection_response.get("reason")
    if not isinstance(reason, str) or len(reason.encode("utf-8")) > _MAX_REQUEST_BYTES:
        _fail("invalid_selection_response", "selection reason must be bounded text")
    confidence = selection_response.get("confidence")
    if (
        isinstance(confidence, bool)
        or not isinstance(confidence, (int, float))
        or not math.isfinite(float(confidence))
        or not 0.0 <= float(confidence) <= 1.0
    ):
        _fail(
            "invalid_selection_response",
            "selection confidence must be finite and within [0, 1]",
        )
    matched = selection_response.get("matched_attributes")
    if (
        isinstance(matched, (str, bytes))
        or not isinstance(matched, Sequence)
        or len(matched) > _MAX_MATCHED_ATTRIBUTES
    ):
        _fail(
            "invalid_selection_response",
            "matched_attributes must be a bounded string list",
        )
    matched_attributes = [
        _text(
            value,
            field=f"matched_attributes[{index}]",
            maximum_bytes=_MAX_IDENTIFIER_BYTES,
        )
        for index, value in enumerate(matched)
    ]

    if outcome == "selected":
        if not selected:
            _fail(
                "selected_object_id_empty",
                "selected outcome requires selected_object_id",
            )
        if selected not in set(candidates):
            _fail(
                "selected_object_not_in_context",
                f"selected_object_id {selected!r} is not in the candidate set",
            )
    else:
        if selected:
            _fail(
                "invalid_selection_response",
                f"{outcome} outcome requires an empty selected_object_id",
            )
        if outcome == "no_match" and not allow_no_match:
            _fail("selection_no_match", reason or "selection returned no_match")
        if outcome == "ambiguous" and not allow_ambiguous:
            _fail("selection_ambiguous", reason or "selection returned ambiguous")

    return {
        "confidence": float(confidence),
        "matched_attributes": matched_attributes,
        "outcome": outcome,
        "reason": reason,
        "selected_object_id": selected,
    }


__all__ = [
    "SceneSelectionError",
    "build_scene_selection_context",
    "validate_scene_selection",
]
