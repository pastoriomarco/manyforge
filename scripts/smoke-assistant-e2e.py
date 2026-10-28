#!/usr/bin/env python3
"""Run a bounded assistant check against a Mock Composer and local model.

The read-only phase is delegated to ``smoke-assistant-provider-bridge.py``.
With the explicit ``--confirm-draft-only`` flag, the second phase offers only
``scene_draft_add_object`` and leaves its result in the in-memory Program
Draft. The operator chooses Apply or Discard in Composer. This script never
saves, Applies, runs a Program, or commands a robot.

The pure validators below remain for their focused helper tests; the live
workflow uses the current Composer assistant request and status routes.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import math
from numbers import Real
import os
from pathlib import Path
import subprocess
import sys
from typing import Any, Final


MAX_VALIDATION_ITEMS: Final = 4096
MAX_VALIDATION_NODES: Final = 16_384
MAX_VALIDATION_DEPTH: Final = 16
MAX_VALIDATION_TEXT_CHARS: Final = 4096
MAX_VALIDATION_TOTAL_TEXT_CHARS: Final = 4 * 1024 * 1024
MAX_IDENTIFIER_CHARS: Final = 256
MAX_PROFILE_SELECTIONS: Final = 64
MAX_VALIDATION_ERRORS: Final = 256


@dataclass(frozen=True)
class Profile:
    """Legacy pure-helper profile name kept for bounded selection tests."""

    name: str
    note: str = ""


PROFILES: Final[dict[str, Profile]] = {
    "bridge-default": Profile(
        name="bridge-default",
        note="Uses the active Deployment model request contract; no local overrides.",
    ),
}
DEFAULT_PROFILES: Final = ["bridge-default"]


class SmokeFailure(RuntimeError):
    """Raised when a bounded assistant smoke check fails."""


def _bounded_identifier(value: Any, *, label: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise SmokeFailure(f"{label} must be a non-empty string")
    if value != value.strip():
        raise SmokeFailure(f"{label} must not contain surrounding whitespace")
    if len(value) > MAX_IDENTIFIER_CHARS:
        raise SmokeFailure(
            f"{label} exceeds the {MAX_IDENTIFIER_CHARS}-character bound"
        )
    return value


def _validate_bounded_value(value: Any, *, label: str) -> None:
    node_count = 0
    text_chars = 0
    active_containers: set[int] = set()

    def visit(item: Any, *, path: str, depth: int) -> None:
        nonlocal node_count, text_chars
        node_count += 1
        if node_count > MAX_VALIDATION_NODES:
            raise SmokeFailure(
                f"{label} exceeds the {MAX_VALIDATION_NODES}-node bound"
            )
        if depth > MAX_VALIDATION_DEPTH:
            raise SmokeFailure(
                f"{path} exceeds the {MAX_VALIDATION_DEPTH}-level bound"
            )
        if item is None or type(item) is bool:
            return
        if isinstance(item, Real):
            if not math.isfinite(float(item)):
                raise SmokeFailure(f"{path} must be finite")
            return
        if isinstance(item, str):
            if len(item) > MAX_VALIDATION_TEXT_CHARS:
                raise SmokeFailure(
                    f"{path} exceeds the "
                    f"{MAX_VALIDATION_TEXT_CHARS}-character bound"
                )
            text_chars += len(item)
            if text_chars > MAX_VALIDATION_TOTAL_TEXT_CHARS:
                raise SmokeFailure(
                    f"{label} exceeds the "
                    f"{MAX_VALIDATION_TOTAL_TEXT_CHARS}-character total bound"
                )
            return
        if not isinstance(item, (dict, list, tuple)):
            raise SmokeFailure(f"{path} is not a JSON-compatible value")
        identity = id(item)
        if identity in active_containers:
            raise SmokeFailure(f"{path} contains a recursive container")
        if len(item) > MAX_VALIDATION_ITEMS:
            raise SmokeFailure(
                f"{path} exceeds the {MAX_VALIDATION_ITEMS}-item bound"
            )
        active_containers.add(identity)
        try:
            if isinstance(item, dict):
                for key, child in item.items():
                    if not isinstance(key, str):
                        raise SmokeFailure(f"{path} contains a non-string key")
                    if len(key) > MAX_IDENTIFIER_CHARS:
                        raise SmokeFailure(
                            f"{path} contains a key longer than "
                            f"{MAX_IDENTIFIER_CHARS} characters"
                        )
                    text_chars += len(key)
                    if text_chars > MAX_VALIDATION_TOTAL_TEXT_CHARS:
                        raise SmokeFailure(
                            f"{label} exceeds the "
                            f"{MAX_VALIDATION_TOTAL_TEXT_CHARS}-character total bound"
                        )
                    visit(child, path=f"{path}.{key}", depth=depth + 1)
            else:
                for index, child in enumerate(item):
                    visit(child, path=f"{path}[{index}]", depth=depth + 1)
        finally:
            active_containers.remove(identity)

    visit(value, path=label, depth=0)


def _bounded_validation_errors(errors: list[str]) -> list[str]:
    if len(errors) <= MAX_VALIDATION_ERRORS:
        return errors
    return [
        *errors[: MAX_VALIDATION_ERRORS - 1],
        f"validation stopped after {MAX_VALIDATION_ERRORS - 1} errors",
    ]


def _finite_real(value: Any) -> float | None:
    if isinstance(value, bool) or not isinstance(value, Real):
        return None
    numeric = float(value)
    return numeric if math.isfinite(numeric) else None


def _safe_repr(value: Any, *, limit: int = 512) -> str:
    rendered = repr(value)
    if len(rendered) <= limit:
        return rendered
    return rendered[: limit - 3] + "..."


def _finite_list(
    value: Any, *, length: int, positive: bool = False
) -> tuple[float, ...] | None:
    if not isinstance(value, list) or len(value) != length:
        return None
    normalized: list[float] = []
    for item in value:
        numeric = _finite_real(item)
        if numeric is None or (positive and numeric <= 0.0):
            return None
        normalized.append(numeric)
    return tuple(normalized)


def _selected_profiles(names: list[str]) -> list[Profile]:
    if not isinstance(names, list):
        raise SmokeFailure("profile names must be a list")
    if len(names) > MAX_PROFILE_SELECTIONS:
        raise SmokeFailure(
            f"profile names exceed the {MAX_PROFILE_SELECTIONS}-item bound"
        )
    requested = names or list(DEFAULT_PROFILES)
    for index, name in enumerate(requested):
        _bounded_identifier(name, label=f"profile names[{index}]")
    if len(set(requested)) != len(requested):
        raise SmokeFailure("profile names contain duplicates")
    if requested == ["all"]:
        requested = list(PROFILES)
    unknown = sorted(set(requested) - set(PROFILES))
    if unknown:
        raise SmokeFailure(
            f"unknown profile(s): {unknown}; available: {sorted(PROFILES)}"
        )
    return [PROFILES[name] for name in requested]


def _golden_scene_resources(slug: str) -> list[dict[str, Any]]:
    _bounded_identifier(slug, label="scene resource slug")
    if len(f"golden_sphere_{slug}") > MAX_IDENTIFIER_CHARS:
        raise SmokeFailure("scene resource slug is too long for the object id")
    return [
        {
            "objectId": f"golden_box_{slug}",
            "shape": {"type": "box", "box_dimensions_m": [0.08, 0.04, 0.03]},
            "pose": {
                "frame_id": "universe",
                "position_m": [0.45, -0.10, 0.015],
                "orientation_rpy_deg": [0.0, 0.0, 0.0],
            },
        },
        {
            "objectId": f"golden_sphere_{slug}",
            "shape": {"type": "sphere", "sphere_radius_m": 0.025},
            "pose": {
                "frame_id": "universe",
                "position_m": [0.55, 0.12, 0.025],
                "orientation_rpy_deg": [0.0, 0.0, 0.0],
            },
        },
    ]


def _tree_insert_argument_errors(
    actual: dict[str, Any],
    expected: dict[str, Any],
    *,
    label: str,
) -> list[str]:
    if not isinstance(actual, dict) or not isinstance(expected, dict):
        return [f"{label} and its expected value must be objects"]
    try:
        _validate_bounded_value(actual, label=label)
        _validate_bounded_value(expected, label=f"{label}.expected")
    except SmokeFailure as exc:
        return [str(exc)]
    errors: list[str] = []
    for key in ("parentName",):
        try:
            actual_value = _bounded_identifier(
                actual.get(key), label=f"{label}.{key}"
            )
            expected_value = _bounded_identifier(
                expected.get(key), label=f"{label}.expected.{key}"
            )
        except SmokeFailure as exc:
            errors.append(str(exc))
            continue
        if actual_value != expected_value:
            errors.append(
                f"{label}.{key} expected {_safe_repr(expected_value)}, "
                f"got {_safe_repr(actual_value)}"
            )
    actual_index = actual.get("index")
    expected_index = expected.get("index")
    if (
        type(actual_index) is not int
        or actual_index < 0
        or type(expected_index) is not int
        or expected_index < 0
    ):
        errors.append(f"{label}.index values must be non-negative integers")
    elif actual_index != expected_index:
        errors.append(
            f"{label}.index expected {expected_index}, got {actual_index}"
        )
    errors.extend(
        _tree_node_match_errors(
            actual.get("node"),
            expected.get("node"),
            label=f"{label}.node",
        )
    )
    extra = sorted(
        set(actual)
        - {
            "parentName",
            "parent_name",
            "index",
            "node",
            "forceDiscardOverrides",
        }
    )
    if extra:
        errors.append(f"{label} contains unexpected keys: {extra}")
    return _bounded_validation_errors(errors)


def _tree_node_match_errors(
    actual: Any,
    expected: dict[str, Any],
    *,
    label: str,
) -> list[str]:
    if not isinstance(actual, dict) or not isinstance(expected, dict):
        return [f"{label} is missing or not an object"]
    errors: list[str] = []
    for key in ("id", "name"):
        try:
            actual_value = _bounded_identifier(
                actual.get(key), label=f"{label}.{key}"
            )
            expected_value = _bounded_identifier(
                expected.get(key), label=f"{label}.expected.{key}"
            )
        except SmokeFailure as exc:
            errors.append(str(exc))
            continue
        if actual_value != expected_value:
            errors.append(
                f"{label}.{key} expected {_safe_repr(expected_value)}, "
                f"got {_safe_repr(actual_value)}"
            )
    actual_params = actual.get("params") or {}
    expected_params = expected.get("params") or {}
    if not isinstance(actual_params, dict) or not isinstance(expected_params, dict):
        errors.append(f"{label}.params values must be objects")
        return _bounded_validation_errors(errors)
    if actual_params != expected_params:
        errors.append(
            f"{label}.params expected {_safe_repr(expected_params)}, "
            f"got {_safe_repr(actual_params)}"
        )
    return _bounded_validation_errors(errors)


def _program_parameter_result_errors(
    result: dict[str, Any],
    expected: dict[str, Any],
    *,
    label: str,
) -> list[str]:
    if not isinstance(result, dict) or not isinstance(expected, dict):
        return [f"{label} and its expected value must be objects"]
    try:
        _validate_bounded_value(result, label=label)
        _validate_bounded_value(expected, label=f"{label}.expected")
        expected_name = _bounded_identifier(
            expected.get("name"), label=f"{label}.expected.name"
        )
    except SmokeFailure as exc:
        return [str(exc)]
    errors: list[str] = []
    if result.get("draftMutation") != "program_parameters_upsert":
        errors.append(
            f"{label}.draftMutation expected 'program_parameters_upsert', "
            f"got {_safe_repr(result.get('draftMutation'))}"
        )
    raw_names = result.get("names")
    if not isinstance(raw_names, list):
        errors.append(f"{label}.names must be a list")
        names: list[str] = []
    else:
        names = []
        for index, value in enumerate(raw_names):
            try:
                names.append(
                    _bounded_identifier(
                        value, label=f"{label}.names[{index}]"
                    )
                )
            except SmokeFailure as exc:
                errors.append(str(exc))
    errors.extend(
        _exact_string_set_errors(names, [expected_name], label=f"{label}.names")
    )
    parameters = result.get("parameters")
    if not isinstance(parameters, list):
        errors.append(f"{label}.parameters must be a list")
        return errors
    by_name: dict[str, dict[str, Any]] = {}
    for index, item in enumerate(parameters):
        if not isinstance(item, dict):
            errors.append(f"{label}.parameters[{index}] must be an object")
            continue
        try:
            name = _bounded_identifier(
                item.get("name"), label=f"{label}.parameters[{index}].name"
            )
        except SmokeFailure as exc:
            errors.append(str(exc))
            continue
        if name in by_name:
            errors.append(f"{label}.parameters contains duplicate name: {name}")
            continue
        by_name[name] = item
    errors.extend(
        _program_parameter_match_errors(
            by_name.get(expected_name),
            expected,
            label=f"{label}.parameters[{expected_name}]",
        )
    )
    return _bounded_validation_errors(errors)


def _program_parameter_match_errors(
    actual: Any,
    expected: dict[str, Any],
    *,
    label: str,
) -> list[str]:
    if not isinstance(actual, dict):
        return [f"{label} is missing or not an object"]
    errors: list[str] = []
    for key in ("name", "type", "default", "description"):
        if actual.get(key) != expected.get(key):
            errors.append(
                f"{label}.{key} expected {_safe_repr(expected.get(key))}, "
                f"got {_safe_repr(actual.get(key))}"
            )
    return errors


def _blackboard_result_errors(
    result: dict[str, Any],
    expected_namespace: dict[str, Any],
    expected_key: dict[str, Any],
    *,
    label: str,
) -> list[str]:
    if (
        not isinstance(result, dict)
        or not isinstance(expected_namespace, dict)
        or not isinstance(expected_key, dict)
    ):
        return [f"{label} and its expected values must be objects"]
    try:
        _validate_bounded_value(result, label=label)
        _validate_bounded_value(
            expected_namespace, label=f"{label}.expected_namespace"
        )
        _validate_bounded_value(expected_key, label=f"{label}.expected_key")
        expected_namespace_id = _bounded_identifier(
            expected_namespace.get("id"),
            label=f"{label}.expected_namespace.id",
        )
        expected_key_id = _bounded_identifier(
            expected_key.get("id"), label=f"{label}.expected_key.id"
        )
    except SmokeFailure as exc:
        return [str(exc)]
    blackboard = result.get("blackboard")
    if not isinstance(blackboard, dict):
        return [f"{label}.blackboard must be an object"]
    namespaces = blackboard.get("namespaces")
    keys = blackboard.get("keys")
    errors: list[str] = []
    if not isinstance(namespaces, list):
        errors.append(f"{label}.blackboard.namespaces must be a list")
        namespaces = []
    if not isinstance(keys, list):
        errors.append(f"{label}.blackboard.keys must be a list")
        keys = []
    namespace_by_id: dict[str, dict[str, Any]] = {}
    for index, item in enumerate(namespaces):
        if not isinstance(item, dict):
            errors.append(
                f"{label}.blackboard.namespaces[{index}] must be an object"
            )
            continue
        try:
            item_id = _bounded_identifier(
                item.get("id"),
                label=f"{label}.blackboard.namespaces[{index}].id",
            )
        except SmokeFailure as exc:
            errors.append(str(exc))
            continue
        if item_id in namespace_by_id:
            errors.append(
                f"{label}.blackboard.namespaces contains duplicate id: {item_id}"
            )
            continue
        namespace_by_id[item_id] = item
    key_by_id: dict[str, dict[str, Any]] = {}
    for index, item in enumerate(keys):
        if not isinstance(item, dict):
            errors.append(f"{label}.blackboard.keys[{index}] must be an object")
            continue
        try:
            item_id = _bounded_identifier(
                item.get("id"), label=f"{label}.blackboard.keys[{index}].id"
            )
        except SmokeFailure as exc:
            errors.append(str(exc))
            continue
        if item_id in key_by_id:
            errors.append(f"{label}.blackboard.keys contains duplicate id: {item_id}")
            continue
        key_by_id[item_id] = item
    errors.extend(
        _blackboard_namespace_match_errors(
            namespace_by_id.get(expected_namespace_id),
            expected_namespace,
            label=(
                f"{label}.blackboard.namespaces[{expected_namespace_id}]"
            ),
        )
    )
    errors.extend(
        _blackboard_key_match_errors(
            key_by_id.get(expected_key_id),
            expected_key,
            label=f"{label}.blackboard.keys[{expected_key_id}]",
        )
    )
    return _bounded_validation_errors(errors)


def _blackboard_namespace_match_errors(
    actual: Any,
    expected: dict[str, Any],
    *,
    label: str,
) -> list[str]:
    if not isinstance(actual, dict):
        return [f"{label} is missing or not an object"]
    errors: list[str] = []
    for key in ("id", "contract", "prefix", "description"):
        if actual.get(key) != expected.get(key):
            errors.append(
                f"{label}.{key} expected {_safe_repr(expected.get(key))}, "
                f"got {_safe_repr(actual.get(key))}"
            )
    actual_fields = _normalize_blackboard_fields(actual.get("fields"))
    expected_fields = _normalize_blackboard_fields(expected.get("fields"))
    if actual_fields != expected_fields:
        errors.append(
            f"{label}.fields expected {_safe_repr(expected_fields)}, "
            f"got {_safe_repr(actual_fields)}"
        )
    return errors


def _normalize_blackboard_fields(raw: Any) -> Any:
    if not isinstance(raw, list):
        return raw
    normalized: list[Any] = []
    for item in raw:
        if not isinstance(item, dict):
            normalized.append(item)
            continue
        field = dict(item)
        field.setdefault("description", "")
        normalized.append(field)
    return normalized


def _blackboard_key_match_errors(
    actual: Any,
    expected: dict[str, Any],
    *,
    label: str,
) -> list[str]:
    if not isinstance(actual, dict):
        return [f"{label} is missing or not an object"]
    errors: list[str] = []
    for key in ("id", "type", "key", "description"):
        if actual.get(key) != expected.get(key):
            errors.append(
                f"{label}.{key} expected {_safe_repr(expected.get(key))}, "
                f"got {_safe_repr(actual.get(key))}"
            )
    return errors


def _tool_call_scene_resources(call: dict[str, Any]) -> list[Any]:
    arguments = call.get("arguments") if isinstance(call, dict) else {}
    if not isinstance(arguments, dict):
        return []
    resources = arguments.get("sceneResources")
    if resources is None:
        resources = arguments.get("scene_resources")
    return resources if isinstance(resources, list) else []


def _scene_resource_set_match_errors(
    actual_resources: Any,
    expected_resources: list[dict[str, Any]],
    *,
    label: str,
    require_exact: bool,
) -> list[str]:
    if type(require_exact) is not bool:
        return [f"{label}.require_exact must be a boolean"]
    try:
        _validate_bounded_value(actual_resources, label=label)
        _validate_bounded_value(
            expected_resources, label=f"{label}.expected"
        )
    except SmokeFailure as exc:
        return [str(exc)]
    if not isinstance(actual_resources, list):
        return [f"{label} must be a list"]
    if not isinstance(expected_resources, list):
        return [f"{label}.expected must be a list"]
    errors: list[str] = []
    actual_by_id: dict[str, dict[str, Any]] = {}
    duplicate_ids: set[str] = set()
    invalid_indices: list[int] = []
    for index, resource in enumerate(actual_resources):
        if not isinstance(resource, dict):
            invalid_indices.append(index)
            continue
        resource = _normalize_scene_resource_for_smoke(resource)
        object_id = resource.get("objectId") or resource.get("object_id")
        try:
            object_id = _bounded_identifier(
                object_id, label=f"{label}[{index}].objectId"
            )
        except SmokeFailure as exc:
            errors.append(str(exc))
            continue
        if object_id in actual_by_id:
            duplicate_ids.add(object_id)
            continue
        actual_by_id[object_id] = resource
    for index in invalid_indices:
        errors.append(f"{label}[{index}] must be an object")
    if duplicate_ids:
        errors.append(f"{label} contains duplicate objectIds: {sorted(duplicate_ids)}")

    expected_ids: list[str] = []
    for index, resource in enumerate(expected_resources):
        if not isinstance(resource, dict):
            errors.append(f"{label}.expected[{index}] must be an object")
            continue
        try:
            expected_ids.append(
                _bounded_identifier(
                    resource.get("objectId") or resource.get("object_id"),
                    label=f"{label}.expected[{index}].objectId",
                )
            )
        except SmokeFailure as exc:
            errors.append(str(exc))
    errors.extend(
        _exact_string_set_errors(
            list(actual_by_id),
            expected_ids,
            label=f"{label}.objectIds",
        )
        if require_exact
        else [
            f"{label} missing expected objectIds: {missing}"
            for missing in [sorted(set(expected_ids) - set(actual_by_id))]
            if missing
        ]
    )
    for expected in expected_resources:
        if not isinstance(expected, dict):
            continue
        object_id = expected.get("objectId") or expected.get("object_id")
        if object_id not in expected_ids:
            continue
        actual = actual_by_id.get(object_id)
        errors.extend(
            _scene_resource_match_errors(
                actual,
                expected,
                label=f"{label}[{object_id}]",
            )
        )
    return _bounded_validation_errors(errors)


def _normalize_scene_resource_for_smoke(
    resource: dict[str, Any],
) -> dict[str, Any]:
    """Canonicalize aliases captured by the historical acceptance corpus."""

    normalized = dict(resource)
    object_id = normalized.get("objectId") or normalized.get("object_id")
    if isinstance(object_id, str):
        normalized["objectId"] = object_id

    shape = (
        dict(normalized.get("shape") or {})
        if isinstance(normalized.get("shape"), dict)
        else {}
    )
    shape_type = (
        shape.get("type")
        or normalized.get("shapeType")
        or normalized.get("shape_type")
        or normalized.get("type")
    )
    if isinstance(shape_type, str):
        shape["type"] = shape_type
    box_dimensions = (
        shape.get("box_dimensions_m")
        or shape.get("dimensions_m")
        or shape.get("dimensions")
        or normalized.get("box_dimensions_m")
        or normalized.get("dimensions_m")
        or normalized.get("dimensions")
        or normalized.get("size")
        or normalized.get("size_m")
    )
    if isinstance(box_dimensions, list):
        shape["box_dimensions_m"] = box_dimensions
    radius = (
        shape.get("sphere_radius_m")
        or shape.get("radius_m")
        or shape.get("radius")
        or normalized.get("sphere_radius_m")
        or normalized.get("radius_m")
        or normalized.get("radius")
    )
    diameter = (
        shape.get("diameter_m")
        or shape.get("diameter")
        or normalized.get("diameter_m")
        or normalized.get("diameter")
    )
    normalized_radius = _finite_real(radius)
    normalized_diameter = _finite_real(diameter)
    if normalized_radius is not None:
        shape["sphere_radius_m"] = normalized_radius
    elif normalized_diameter is not None:
        shape["sphere_radius_m"] = normalized_diameter / 2.0
    if shape:
        normalized["shape"] = shape

    pose = (
        dict(normalized.get("pose") or {})
        if isinstance(normalized.get("pose"), dict)
        else {}
    )
    frame_id = (
        pose.get("frame_id")
        or pose.get("frameId")
        or normalized.get("frame_id")
        or normalized.get("frameId")
    )
    if isinstance(frame_id, str):
        pose["frame_id"] = frame_id
    position = (
        pose.get("position_m")
        or pose.get("position")
        or normalized.get("position_m")
        or normalized.get("position")
    )
    if isinstance(position, list):
        pose["position_m"] = position
    orientation_rpy = (
        pose.get("orientation_rpy_deg")
        or pose.get("orientation")
        or normalized.get("orientation_rpy_deg")
        or normalized.get("orientation")
    )
    if isinstance(orientation_rpy, list):
        pose["orientation_rpy_deg"] = orientation_rpy
    orientation_xyzw = (
        pose.get("orientation_xyzw") or normalized.get("orientation_xyzw")
    )
    if isinstance(orientation_xyzw, list):
        pose["orientation_xyzw"] = orientation_xyzw
    if pose:
        normalized["pose"] = pose
    return normalized


def _exact_string_set_errors(
    actual: list[str],
    expected: list[str],
    *,
    label: str,
) -> list[str]:
    if not isinstance(actual, list) or not isinstance(expected, list):
        return [f"{label} and its expected values must be lists"]
    try:
        _validate_bounded_value(actual, label=label)
        _validate_bounded_value(expected, label=f"{label}.expected")
        normalized_actual = [
            _bounded_identifier(value, label=f"{label}[{index}]")
            for index, value in enumerate(actual)
        ]
        normalized_expected = [
            _bounded_identifier(value, label=f"{label}.expected[{index}]")
            for index, value in enumerate(expected)
        ]
    except SmokeFailure as exc:
        return [str(exc)]
    if len(set(normalized_actual)) != len(normalized_actual):
        return [f"{label} contains duplicate values"]
    if len(set(normalized_expected)) != len(normalized_expected):
        return [f"{label}.expected contains duplicate values"]
    actual_set = set(normalized_actual)
    expected_set = set(normalized_expected)
    errors: list[str] = []
    missing = sorted(expected_set - actual_set)
    extra = sorted(actual_set - expected_set)
    if missing:
        errors.append(f"{label} missing expected values: {missing}")
    if extra:
        errors.append(f"{label} contains unexpected values: {extra}")
    return errors


def _scene_resource_match_errors(
    actual: dict[str, Any] | None,
    expected: dict[str, Any],
    *,
    label: str,
    tolerance: float = 1e-6,
) -> list[str]:
    normalized_tolerance = _finite_real(tolerance)
    if normalized_tolerance is None or normalized_tolerance <= 0.0:
        return [f"{label}.tolerance must be a finite positive real number"]
    try:
        _validate_bounded_value(actual, label=label)
        _validate_bounded_value(expected, label=f"{label}.expected")
    except SmokeFailure as exc:
        return [str(exc)]
    errors: list[str] = []
    if not isinstance(actual, dict) or not isinstance(expected, dict):
        return [f"{label} is missing or not an object"]
    actual_id = actual.get("objectId") or actual.get("object_id")
    expected_id = expected.get("objectId") or expected.get("object_id")
    try:
        actual_id = _bounded_identifier(
            actual_id, label=f"{label}.objectId"
        )
        expected_id = _bounded_identifier(
            expected_id, label=f"{label}.expected.objectId"
        )
    except SmokeFailure as exc:
        errors.append(str(exc))
    if actual_id != expected_id:
        errors.append(
            f"{label}.objectId expected {_safe_repr(expected_id)}, "
            f"got {_safe_repr(actual_id)}"
        )
    shape = actual.get("shape") or {}
    expected_shape = expected.get("shape") or {}
    if not isinstance(shape, dict) or not isinstance(expected_shape, dict):
        return _bounded_validation_errors(
            [*errors, f"{label}.shape and expected shape must be objects"]
        )
    if shape.get("type") != expected_shape.get("type"):
        errors.append(
            f"{label}.shape.type expected "
            f"{_safe_repr(expected_shape.get('type'))}, "
            f"got {_safe_repr(shape.get('type'))}"
        )
    if shape.get("type") == "box":
        actual_dimensions = _finite_list(
            shape.get("box_dimensions_m"), length=3, positive=True
        )
        expected_dimensions = _finite_list(
            expected_shape.get("box_dimensions_m"), length=3, positive=True
        )
        if (
            actual_dimensions is None
            or expected_dimensions is None
            or not _float_lists_close(
                shape.get("box_dimensions_m"),
                expected_shape.get("box_dimensions_m"),
                tolerance=normalized_tolerance,
                expected_length=3,
            )
        ):
            errors.append(
                f"{label}.shape.box_dimensions_m expected "
                f"{_safe_repr(expected_shape.get('box_dimensions_m'))}, "
                f"got {_safe_repr(shape.get('box_dimensions_m'))}"
            )
    if shape.get("type") == "sphere":
        radius = shape.get("sphere_radius_m")
        expected_radius = expected_shape.get("sphere_radius_m")
        normalized_radius = _finite_real(radius)
        normalized_expected_radius = _finite_real(expected_radius)
        if (
            normalized_radius is None
            or normalized_expected_radius is None
            or normalized_radius <= 0.0
            or normalized_expected_radius <= 0.0
        ):
            errors.append(
                f"{label}.shape.sphere_radius_m expected "
                f"{_safe_repr(expected_radius)}, got {_safe_repr(radius)}"
            )
        elif (
            abs(normalized_radius - normalized_expected_radius)
            > normalized_tolerance
        ):
            errors.append(
                f"{label}.shape.sphere_radius_m expected "
                f"{_safe_repr(expected_radius)}, got {_safe_repr(radius)}"
            )
    pose = actual.get("pose") or {}
    expected_pose = expected.get("pose") or {}
    if not isinstance(pose, dict) or not isinstance(expected_pose, dict):
        return _bounded_validation_errors(
            [*errors, f"{label}.pose and expected pose must be objects"]
        )
    actual_frame = pose.get("frame_id")
    expected_frame = expected_pose.get("frame_id")
    try:
        actual_frame = _bounded_identifier(
            actual_frame, label=f"{label}.pose.frame_id"
        )
        expected_frame = _bounded_identifier(
            expected_frame, label=f"{label}.expected.pose.frame_id"
        )
    except SmokeFailure as exc:
        errors.append(str(exc))
    if actual_frame != expected_frame:
        errors.append(
            f"{label}.pose.frame_id expected "
            f"{_safe_repr(expected_pose.get('frame_id'))}, "
            f"got {_safe_repr(pose.get('frame_id'))}"
        )
    if not _float_lists_close(
        pose.get("position_m"),
        expected_pose.get("position_m"),
        tolerance=normalized_tolerance,
        expected_length=3,
    ):
        errors.append(
            f"{label}.pose.position_m expected "
            f"{_safe_repr(expected_pose.get('position_m'))}, "
            f"got {_safe_repr(pose.get('position_m'))}"
        )
    for orientation_key in ("orientation_rpy_deg", "orientation_xyzw"):
        if orientation_key in expected_pose and not _float_lists_close(
            pose.get(orientation_key),
            expected_pose.get(orientation_key),
            tolerance=normalized_tolerance,
            expected_length=3 if orientation_key == "orientation_rpy_deg" else 4,
        ):
            errors.append(
                f"{label}.pose.{orientation_key} expected "
                f"{_safe_repr(expected_pose.get(orientation_key))}, "
                f"got {_safe_repr(pose.get(orientation_key))}"
            )
        if (
            orientation_key == "orientation_xyzw"
            and orientation_key in expected_pose
        ):
            actual_quaternion = _finite_list(
                pose.get(orientation_key), length=4
            )
            expected_quaternion = _finite_list(
                expected_pose.get(orientation_key), length=4
            )
            if (
                actual_quaternion is None
                or expected_quaternion is None
                or abs(
                    math.sqrt(sum(value * value for value in actual_quaternion))
                    - 1.0
                )
                > normalized_tolerance
                or abs(
                    math.sqrt(sum(value * value for value in expected_quaternion))
                    - 1.0
                )
                > normalized_tolerance
            ):
                errors.append(
                    f"{label}.pose.orientation_xyzw must be a finite unit quaternion"
                )
    return _bounded_validation_errors(errors)


def _float_lists_close(
    actual: Any,
    expected: Any,
    *,
    tolerance: float,
    expected_length: int | None = None,
) -> bool:
    normalized_tolerance = _finite_real(tolerance)
    if normalized_tolerance is None or normalized_tolerance <= 0.0:
        return False
    if not isinstance(actual, list) or not isinstance(expected, list):
        return False
    if len(actual) > MAX_VALIDATION_ITEMS or len(expected) > MAX_VALIDATION_ITEMS:
        return False
    if len(actual) != len(expected) or (
        expected_length is not None and len(actual) != expected_length
    ):
        return False
    for actual_value, expected_value in zip(actual, expected, strict=True):
        normalized_actual = _finite_real(actual_value)
        normalized_expected = _finite_real(expected_value)
        if normalized_actual is None or normalized_expected is None:
            return False
        if abs(normalized_actual - normalized_expected) > normalized_tolerance:
            return False
    return True


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--composer-url",
        default=os.environ.get("MANYFORGE_COMPOSER_URL", "http://127.0.0.1:3000"),
        help="Composer API root (default: %(default)s)",
    )
    parser.add_argument(
        "--expected-component-id",
        help="optionally require this component for the chat alias",
    )
    parser.add_argument(
        "--timeout-seconds",
        type=float,
        default=180.0,
        help="maximum time for each assistant request (default: %(default)s)",
    )
    parser.add_argument(
        "--confirm-draft-only",
        action="store_true",
        help="confirm that the second phase may edit the in-memory Program Draft",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    if not args.confirm_draft_only:
        print(
            "Refusing to run the Draft-mutating phase without --confirm-draft-only.",
            file=sys.stderr,
        )
        return 2
    if not 1.0 <= args.timeout_seconds <= 900.0:
        print("--timeout-seconds must be between 1 and 900", file=sys.stderr)
        return 2

    scripts = Path(__file__).resolve().parent
    common = [
        "--composer-url",
        args.composer_url,
        "--timeout-seconds",
        str(args.timeout_seconds),
    ]
    if args.expected_component_id:
        common.extend(("--expected-component-id", args.expected_component_id))
    phases = (
        (
            "read-only provider check",
            scripts / "smoke-assistant-provider-bridge.py",
            [*common],
        ),
        (
            "Draft-only scene authoring check",
            scripts / "smoke-assistant-scene-authoring.py",
            [*common, "--confirm-draft-only"],
        ),
    )
    for label, script, arguments in phases:
        print(f"Running {label}...", flush=True)
        try:
            result = subprocess.run(
                [sys.executable, str(script), *arguments],
                check=False,
            )
        except OSError as error:
            print(f"FAIL: could not start {script.name}: {error}", file=sys.stderr)
            return 1
        if result.returncode != 0:
            if label == "read-only provider check":
                print("Stopped before submitting a Draft edit.", file=sys.stderr)
            return result.returncode
    print(
        "PASS: end-to-end assistant smoke completed. Review the Program Draft "
        "in Composer, then choose Apply or Discard."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
