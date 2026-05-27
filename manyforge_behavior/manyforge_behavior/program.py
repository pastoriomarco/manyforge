"""Program artifact loader and validator for ManyForge behavior trees."""

from __future__ import annotations

import hashlib
import json
import math
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Mapping, Optional

import yaml

from manyforge_behavior.blackboard_bindings import (
    field_ref_target as _field_ref_target,
    field_type_compatible as _field_type_compatible,
    key_ref_id as _key_ref_id,
    namespace_ref_id as _namespace_ref_id,
    resolve_field_binding as _resolve_field_binding,
    resolve_key_binding as _resolve_key_binding,
    resolve_namespace_binding as _resolve_namespace_binding,
    validate_declared_field_ref as _validate_declared_field_ref,
)
from manyforge_behavior.capabilities import get_required_keys
from manyforge_behavior.catalog import NodeCatalog, SEMVER_RE, profile_satisfied
from manyforge_behavior.parameters import (
    PARAM_REF_RE,
    ParameterResolver,
)
from manyforge_behavior.skill_loader import (
    SkillCatalog,
    default_skills_dir,
    load_skill_catalog,
)


EXPECTED_API_VERSION = "manyforge.program.v0"
BLACKBOARD_REF_RE = re.compile(r"^\$\{bb:([^}]+)\}$")
_REMOVED_COOPERATIVE_INTERRUPT_FIELDS = frozenset(
    {
        "interrupt_handlers",
        "interrupt_policies",
    }
)


@dataclass
class SceneResource:
    object_id: str
    shape: dict[str, Any]
    pose: dict[str, Any]
    metadata: dict[str, Any] = field(default_factory=dict)


@dataclass
class ProgramParameter:
    name: str
    type: str
    default: Any
    constraints: dict[str, Any] = field(default_factory=dict)
    description: str = ""


@dataclass
class BlackboardNamespace:
    id: str
    contract: str
    prefix: str
    description: str = ""
    fields: list["BlackboardNamespaceField"] = field(default_factory=list)


@dataclass
class BlackboardNamespaceField:
    name: str
    type: str
    description: str = ""


@dataclass
class BlackboardKey:
    id: str
    type: str
    key: str
    description: str = ""


@dataclass
class TreeNode:
    id: str
    name: str
    params: dict[str, Any] = field(default_factory=dict)
    children: list["TreeNode"] = field(default_factory=list)
    modification_policy: dict[str, str] = field(default_factory=dict)
    recovery_policy: dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> dict[str, Any]:
        """Serialize to dict matching program YAML schema."""
        d: dict[str, Any] = {"id": self.id, "name": self.name}
        if self.params:
            d["params"] = self.params
        if self.modification_policy:
            d["modification_policy"] = self.modification_policy
        if self.recovery_policy:
            d["recovery_policy"] = self.recovery_policy
        if self.children:
            d["children"] = [c.to_dict() for c in self.children]
        return d


def compute_program_tree_hash(tree: TreeNode) -> str:
    """Compute a deterministic SHA-256 hash of the normalized program tree.

    The hash covers node IDs, names, params, modification_policy, and children
    structure — everything that affects whether a runtime override is valid.
    """
    canonical = json.dumps(tree.to_dict(), sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


@dataclass
class RobotProfile:
    robot_id: str
    group_id: str
    base_frame: str
    tcp_frame: str
    base_pose: dict[str, Any] = field(default_factory=dict)


class ProgramValidationError(Exception):
    pass


@dataclass
class TreeValidationIssue:
    path: str
    message: str


@dataclass
class _BlackboardNamespaceProducer:
    path: str
    node_id: str
    node_name: str
    namespace_id: str
    contract: str
    prefix: str


@dataclass
class _BlackboardNamespaceReference:
    path: str
    param_name: str
    namespace_id: str
    expected_contract: str
    expected_label: str
    prefix: str


_MODIFICATION_POLICY_ALLOWED_VALUES: dict[str, set[str]] = {
    "runtime_override": {"forbidden", "human_approval", "llm_allowed"},
    "persistent_edit": {"forbidden", "human_approval", "llm_allowed"},
    "intervention_delegate": {"forbidden", "human_only", "llm_allowed", "program_only"},
}


def _parse_modification_policy(
    raw_policy: Any,
    *,
    path: str,
) -> dict[str, str]:
    if raw_policy is None:
        return {}
    if not isinstance(raw_policy, Mapping):
        raise ProgramValidationError(f"{path}.modification_policy must be a mapping")

    parsed: dict[str, str] = {}
    for key, value in raw_policy.items():
        if key not in _MODIFICATION_POLICY_ALLOWED_VALUES:
            valid_keys = ", ".join(sorted(_MODIFICATION_POLICY_ALLOWED_VALUES.keys()))
            raise ProgramValidationError(
                f"{path}.modification_policy.{key} is not a recognized policy "
                f"field. Valid fields: {valid_keys}. Omit the field entirely "
                f"if no policy override is needed."
            )
        if not isinstance(value, str) or value not in _MODIFICATION_POLICY_ALLOWED_VALUES[key]:
            allowed = ", ".join(sorted(_MODIFICATION_POLICY_ALLOWED_VALUES[key]))
            raise ProgramValidationError(
                f"{path}.modification_policy.{key} must be one of: {allowed}"
            )
        parsed[key] = value
    return parsed


_RECOVERY_POLICY_ALLOWED_ACTIONS = {"runtime_override", "intervention"}
_RECOVERY_POLICY_PERSISTENCE_CEILINGS = {"session_only", "runtime_override", "draft_patch"}


def _parse_recovery_policy(
    raw_policy: Any,
    *,
    path: str,
) -> dict[str, Any]:
    """Parse and validate a node-level recovery_policy mapping."""
    if raw_policy is None:
        return {}
    if not isinstance(raw_policy, Mapping):
        raise ProgramValidationError(f"{path}.recovery_policy must be a mapping")

    parsed: dict[str, Any] = {}
    recognized_keys = {
        "allow_intervention", "allowed_recovery_actions", "max_retries",
        "allowed_skills", "persistence_ceiling",
    }
    for key in raw_policy:
        if key not in recognized_keys:
            valid_keys = ", ".join(sorted(recognized_keys))
            raise ProgramValidationError(
                f"{path}.recovery_policy.{key} is not a recognized recovery "
                f"policy field. Valid fields: {valid_keys}. Omit the field "
                f"entirely if no recovery override is needed."
            )

    allow_intervention = raw_policy.get("allow_intervention")
    if allow_intervention is not None:
        if not isinstance(allow_intervention, bool):
            raise ProgramValidationError(
                f"{path}.recovery_policy.allow_intervention must be a boolean"
            )
        parsed["allow_intervention"] = allow_intervention

    allowed_actions = raw_policy.get("allowed_recovery_actions")
    if allowed_actions is not None:
        if not isinstance(allowed_actions, list):
            raise ProgramValidationError(
                f"{path}.recovery_policy.allowed_recovery_actions must be a list"
            )
        for action in allowed_actions:
            if action not in _RECOVERY_POLICY_ALLOWED_ACTIONS:
                allowed = ", ".join(sorted(_RECOVERY_POLICY_ALLOWED_ACTIONS))
                raise ProgramValidationError(
                    f"{path}.recovery_policy.allowed_recovery_actions: "
                    f"'{action}' is not valid (allowed: {allowed})"
                )
        parsed["allowed_recovery_actions"] = list(allowed_actions)

    max_retries = raw_policy.get("max_retries")
    if max_retries is not None:
        if not isinstance(max_retries, int) or isinstance(max_retries, bool) or max_retries < 0:
            raise ProgramValidationError(
                f"{path}.recovery_policy.max_retries must be a non-negative integer"
            )
        parsed["max_retries"] = max_retries

    allowed_skills = raw_policy.get("allowed_skills")
    if allowed_skills is not None:
        if not isinstance(allowed_skills, list):
            raise ProgramValidationError(
                f"{path}.recovery_policy.allowed_skills must be a list"
            )
        for skill_id in allowed_skills:
            if not isinstance(skill_id, str) or not skill_id:
                raise ProgramValidationError(
                    f"{path}.recovery_policy.allowed_skills entries must be non-empty strings"
                )
        parsed["allowed_skills"] = list(allowed_skills)

    persistence_ceiling = raw_policy.get("persistence_ceiling")
    if persistence_ceiling is not None:
        if persistence_ceiling not in _RECOVERY_POLICY_PERSISTENCE_CEILINGS:
            allowed = ", ".join(sorted(_RECOVERY_POLICY_PERSISTENCE_CEILINGS))
            raise ProgramValidationError(
                f"{path}.recovery_policy.persistence_ceiling must be one of: {allowed}"
            )
        parsed["persistence_ceiling"] = persistence_ceiling

    return parsed


def tree_node_from_dict(
    data: Mapping[str, Any],
    *,
    path: str = "tree",
) -> TreeNode:
    """Parse a raw mapping into a TreeNode with path-aware validation."""
    if not isinstance(data, Mapping):
        raise ProgramValidationError(f"{path} must be a mapping")

    node_id = data.get("id")
    if not isinstance(node_id, str) or not node_id:
        raise ProgramValidationError(f"{path}.id is required")

    name = data.get("name", node_id)
    if not isinstance(name, str) or not name:
        raise ProgramValidationError(f"{path}.name must be a non-empty string")

    params = data.get("params", {})
    if params is None:
        params = {}
    if not isinstance(params, Mapping):
        raise ProgramValidationError(f"{path}.params must be a mapping")

    children_data = data.get("children", [])
    if children_data is None:
        children_data = []
    if not isinstance(children_data, list):
        raise ProgramValidationError(f"{path}.children must be a list")

    children = [
        tree_node_from_dict(child, path=f"{path}.children[{index}]")
        for index, child in enumerate(children_data)
    ]
    modification_policy = _parse_modification_policy(
        data.get("modification_policy"),
        path=path,
    )
    recovery_policy = _parse_recovery_policy(
        data.get("recovery_policy"),
        path=path,
    )
    return TreeNode(
        id=node_id,
        name=name,
        params=dict(params),
        children=children,
        modification_policy=modification_policy,
        recovery_policy=recovery_policy,
    )


def find_tree_node_by_name(node: TreeNode, node_name: str) -> TreeNode | None:
    """Return the first node whose instance name matches node_name."""
    if node.name == node_name:
        return node
    for child in node.children:
        found = find_tree_node_by_name(child, node_name)
        if found is not None:
            return found
    return None


def apply_tree_param_overrides(
    node: TreeNode,
    overrides_by_name: Mapping[str, Mapping[str, Any]],
) -> TreeNode:
    """Return a deep-copied tree with per-node param patches applied by node name."""
    merged_params = dict(node.params)
    if node.name in overrides_by_name:
        merged_params.update(dict(overrides_by_name[node.name]))
    return TreeNode(
        id=node.id,
        name=node.name,
        params=merged_params,
        children=[
            apply_tree_param_overrides(child, overrides_by_name)
            for child in node.children
        ],
        modification_policy=dict(node.modification_policy),
        recovery_policy=dict(node.recovery_policy),
    )


def _contains_parameter_reference(value: Any) -> bool:
    if isinstance(value, str):
        return PARAM_REF_RE.match(value) is not None
    if isinstance(value, list):
        return any(_contains_parameter_reference(item) for item in value)
    if isinstance(value, dict):
        return any(_contains_parameter_reference(item) for item in value.values())
    return False


def _is_blackboard_reference_string(value: Any) -> bool:
    return isinstance(value, str) and BLACKBOARD_REF_RE.match(value) is not None


def _format_param_schema(entry: Any) -> str:
    """Compact one-line summary of a catalog entry's expected params, for error messages."""
    parts: list[str] = []
    for p in entry.parameters:
        bits = [p.name + ":" + (p.type or "any")]
        if p.required and p.default is None:
            bits.append("required")
        if p.enum_values:
            bits.append("enum=[" + "|".join(str(v) for v in p.enum_values) + "]")
        parts.append("{" + ",".join(bits) + "}")
    return "[" + ",".join(parts) + "]" if parts else "[]"


def _is_numeric(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _is_numeric_list(value: Any, expected_len: int | None = None) -> bool:
    if not isinstance(value, list):
        return False
    if expected_len is not None and len(value) != expected_len:
        return False
    return all(_is_numeric(item) for item in value)


def _validate_pose_value(value: Any) -> str | None:
    if not isinstance(value, dict):
        return "must be a mapping"
    if "position_m" in value and not _is_numeric_list(value["position_m"], 3):
        return "position_m must be a list of 3 numeric values"
    if "orientation_xyzw" in value and not _is_numeric_list(value["orientation_xyzw"], 4):
        return "orientation_xyzw must be a list of 4 numeric values"
    if "orientation_rpy_deg" in value and not _is_numeric_list(value["orientation_rpy_deg"], 3):
        return "orientation_rpy_deg must be a list of 3 numeric values"
    return None


def _validate_robot_base_pose_value(value: Any) -> str | None:
    pose_error = _validate_pose_value(value)
    if pose_error is not None:
        return f"base_pose {pose_error}"
    frame_id = value.get("frame_id")
    if not isinstance(frame_id, str) or not frame_id.strip():
        return "base_pose.frame_id is required"
    position = value.get("position_m")
    if not _is_numeric_list(position, 3):
        return "base_pose.position_m must be a list of 3 numeric values"
    if "orientation_xyzw" not in value and "orientation_rpy_deg" not in value:
        return "base_pose requires orientation_xyzw or orientation_rpy_deg"
    return None


def _validate_scene_shape_value(value: Any) -> str | None:
    if not isinstance(value, Mapping):
        return "shape must be a mapping"

    shape_type = value.get("type")
    if shape_type not in {"box", "sphere", "cylinder", "mesh"}:
        return "shape.type must be one of: box, sphere, cylinder, mesh"

    if shape_type == "box":
        dimensions = value.get("box_dimensions_m")
        if not _is_numeric_list(dimensions, 3):
            return "shape.box_dimensions_m must be a list of 3 numeric values"
        if any(float(component) <= 0.0 for component in dimensions):
            return "shape.box_dimensions_m values must be > 0"

    if shape_type == "sphere":
        radius = value.get("sphere_radius_m")
        if not _is_numeric(radius) or float(radius) <= 0.0:
            return "shape.sphere_radius_m must be > 0"

    if shape_type == "cylinder":
        radius = value.get("cylinder_radius_m")
        height = value.get("cylinder_height_m")
        if not _is_numeric(radius) or float(radius) <= 0.0:
            return "shape.cylinder_radius_m must be > 0"
        if not _is_numeric(height) or float(height) <= 0.0:
            return "shape.cylinder_height_m must be > 0"

    if shape_type == "mesh":
        resource_uri = value.get("mesh_resource_uri")
        if not isinstance(resource_uri, str) or not resource_uri.strip():
            return "shape.mesh_resource_uri is required"
        scale = value.get("mesh_scale")
        if scale is not None:
            if not _is_numeric_list(scale, 3):
                return "shape.mesh_scale must be a list of 3 numeric values"
            if any(float(component) <= 0.0 for component in scale):
                return "shape.mesh_scale values must be > 0"

    return None


def _validate_scene_resource_value(value: Any) -> str | None:
    if not isinstance(value, Mapping):
        return "must be a mapping"

    object_id = value.get("object_id")
    if not isinstance(object_id, str) or not object_id.strip():
        return "object_id is required"

    shape_error = _validate_scene_shape_value(value.get("shape"))
    if shape_error is not None:
        return shape_error

    pose = value.get("pose")
    if not isinstance(pose, Mapping):
        return "pose must be a mapping"

    frame_id = pose.get("frame_id")
    if not isinstance(frame_id, str) or not frame_id.strip():
        return "pose.frame_id is required"

    position = pose.get("position_m")
    if not _is_numeric_list(position, 3):
        return "pose.position_m must be a list of 3 numeric values"

    has_xyzw = "orientation_xyzw" in pose
    has_rpy = "orientation_rpy_deg" in pose
    if has_xyzw == has_rpy:
        return "pose must define exactly one of orientation_xyzw or orientation_rpy_deg"

    if has_xyzw and not _is_numeric_list(pose["orientation_xyzw"], 4):
        return "pose.orientation_xyzw must be a list of 4 numeric values"
    if has_rpy and not _is_numeric_list(pose["orientation_rpy_deg"], 3):
        return "pose.orientation_rpy_deg must be a list of 3 numeric values"

    metadata = value.get("metadata", {})
    if metadata is None:
        metadata = {}
    if not isinstance(metadata, Mapping):
        return "metadata must be a mapping"
    support_contacts = metadata.get("support_contacts")
    if support_contacts is not None:
        if not isinstance(support_contacts, list):
            return "metadata.support_contacts must be a list"
        for contact_index, contact in enumerate(support_contacts):
            if not isinstance(contact, Mapping):
                return f"metadata.support_contacts[{contact_index}] must be a mapping"
            support_object_id = contact.get("support_object_id")
            if not isinstance(support_object_id, str) or not support_object_id.strip():
                return (
                    f"metadata.support_contacts[{contact_index}].support_object_id "
                    "is required"
                )
            max_start_penetration_m = contact.get("max_start_penetration_m", 0.0)
            if not _is_numeric(max_start_penetration_m) or float(max_start_penetration_m) < 0.0:
                return (
                    f"metadata.support_contacts[{contact_index}]."
                    "max_start_penetration_m must be >= 0"
                )
            mode = contact.get("mode", "contact_release")
            if mode not in {"contact_release", "contact_process"}:
                return (
                    f"metadata.support_contacts[{contact_index}].mode must be "
                    "contact_release or contact_process"
                )
            for field_name in (
                "release_tolerance_m",
                "min_release_progress_m",
                "min_separation_clearance_m",
            ):
                value = contact.get(field_name, 0.0)
                if not _is_numeric(value) or float(value) < 0.0:
                    return (
                        f"metadata.support_contacts[{contact_index}]."
                        f"{field_name} must be >= 0"
                    )
            for field_name in (
                "require_separation_by_end",
                "require_strictly_decreasing_penetration",
                "allow_recontact",
            ):
                if field_name in contact and not isinstance(contact[field_name], bool):
                    return (
                        f"metadata.support_contacts[{contact_index}]."
                        f"{field_name} must be boolean"
                    )

    return None


_RELATIVE_MOVE_REFERENCE_TYPES = {"frame", "object", "robot_link", "pose"}


def _validate_relative_move_reference(
    value: Any,
    path_label: str,
    namespace_by_id: Mapping[str, BlackboardNamespace],
    namespace_fields_by_id: Mapping[str, Mapping[str, BlackboardNamespaceField]],
) -> str | None:
    if not isinstance(value, dict):
        return f"{path_label} must be a mapping"

    reference_type = value.get("type")
    if reference_type not in _RELATIVE_MOVE_REFERENCE_TYPES:
        return (
            f"{path_label}.type must be one of "
            f"{sorted(_RELATIVE_MOVE_REFERENCE_TYPES)}, got {reference_type!r}"
        )

    if reference_type == "pose":
        pose_value = value.get("pose")
        if pose_value is None:
            return f"{path_label}.pose is required when {path_label}.type='pose'"
        field_ref_target = _field_ref_target(pose_value)
        if field_ref_target is not None:
            namespace_id, field_name = field_ref_target
            return _validate_declared_field_ref(
                path_label=f"{path_label}.pose",
                namespace_id=namespace_id,
                field_name=field_name,
                namespace_by_id=namespace_by_id,
                namespace_fields_by_id=namespace_fields_by_id,
                expected_type="pose",
            )
        if _is_blackboard_reference_string(pose_value):
            return (
                f"{path_label}.pose must use a field_ref binding instead of a raw "
                "blackboard reference string"
            )
        pose_error = _validate_pose_value(pose_value)
        if pose_error is not None:
            return f"{path_label}.pose {pose_error}"
        return None

    reference_id = value.get("id")
    if not isinstance(reference_id, str) or not reference_id.strip():
        return f"{path_label}.id is required when {path_label}.type={reference_type!r}"
    return None


def _validate_relative_move_value(
    value: Any,
    namespace_by_id: Mapping[str, BlackboardNamespace],
    namespace_fields_by_id: Mapping[str, Mapping[str, BlackboardNamespaceField]],
) -> str | None:
    if not isinstance(value, dict):
        return "must be a mapping"

    target_reference = value.get("target_reference")
    if target_reference is None:
        return "target_reference is required"
    target_error = _validate_relative_move_reference(
        target_reference,
        "relative_move.target_reference",
        namespace_by_id,
        namespace_fields_by_id,
    )
    if target_error is not None:
        return target_error

    offset_reference = value.get("offset_reference")
    if offset_reference is not None:
        offset_ref_error = _validate_relative_move_reference(
            offset_reference,
            "relative_move.offset_reference",
            namespace_by_id,
            namespace_fields_by_id,
        )
        if offset_ref_error is not None:
            return offset_ref_error

    offset = value.get("offset")
    if offset is None:
        return "offset is required"
    offset_error = _validate_pose_value(offset)
    if offset_error is not None:
        return f"offset {offset_error}"

    return None


def _validate_template_variable_map(
    value: Any,
    namespace_by_id: Mapping[str, BlackboardNamespace],
    namespace_fields_by_id: Mapping[str, Mapping[str, BlackboardNamespaceField]],
    key_by_id: Mapping[str, BlackboardKey],
) -> str | None:
    if not isinstance(value, Mapping):
        return "template_variables must be a mapping"

    for variable_name, binding in value.items():
        if not isinstance(variable_name, str) or not variable_name.strip():
            return "template_variables keys must be non-empty strings"
        if _is_blackboard_reference_string(binding):
            return (
                f"template_variables.{variable_name} must use a key_ref or field_ref "
                "binding instead of a raw blackboard reference string"
            )
        if _key_ref_id(binding) is not None:
            _, binding_error = _resolve_key_binding(
                param_name=f"template_variables.{variable_name}",
                value=binding,
                key_by_id=key_by_id,
                expected_type=None,
            )
            return binding_error
        field_ref_target = _field_ref_target(binding)
        if field_ref_target is not None:
            namespace_id, field_name = field_ref_target
            return _validate_declared_field_ref(
                path_label=f"template_variables.{variable_name}",
                namespace_id=namespace_id,
                field_name=field_name,
                namespace_by_id=namespace_by_id,
                namespace_fields_by_id=namespace_fields_by_id,
                expected_type=None,
            )
        return (
            f"template_variables.{variable_name} must be a key_ref or field_ref binding"
        )

    return None


def _validate_constraints(value: float, constraints: dict[str, Any]) -> str | None:
    min_inclusive = constraints.get("min_inclusive")
    if min_inclusive is not None and value < min_inclusive:
        return f"must be >= {min_inclusive}, got {value}"

    min_exclusive = constraints.get("min_exclusive")
    if min_exclusive is not None and value <= min_exclusive:
        return f"must be > {min_exclusive}, got {value}"

    max_inclusive = constraints.get("max_inclusive")
    if max_inclusive is not None and value > max_inclusive:
        return f"must be <= {max_inclusive}, got {value}"

    max_exclusive = constraints.get("max_exclusive")
    if max_exclusive is not None and value >= max_exclusive:
        return f"must be < {max_exclusive}, got {value}"

    return None


def _blackboard_contract_label(contract: str) -> str:
    labels = {
        "selection_context_v1": "BuildSceneSelectionContext output namespace",
        "query_model_output_v1": "QueryModel output namespace",
        "validated_selection_v1": "ValidateSceneSelection output namespace",
        "pick_plan_v1": "PlanPickSequence output namespace",
    }
    return labels.get(contract, f"contract '{contract}'")


_BLACKBOARD_FIELD_TYPES = {
    "string",
    "float",
    "int",
    "bool",
    "pose",
    "float_list",
    "string_list",
    "dict",
}

def _validate_param_type(param_type: str, value: Any) -> str | None:
    if param_type == "string":
        if not isinstance(value, str):
            return f"must be a string, got {type(value).__name__}"
        return None

    if param_type == "float":
        if not _is_numeric(value):
            return f"must be a number, got {type(value).__name__}"
        return None

    if param_type == "int":
        if not isinstance(value, int) or isinstance(value, bool):
            return f"must be an integer, got {type(value).__name__}"
        return None

    if param_type == "bool":
        if not isinstance(value, bool):
            return f"must be a boolean, got {type(value).__name__}"
        return None

    if param_type == "enum":
        if not isinstance(value, str):
            return f"must be a string enum value, got {type(value).__name__}"
        return None

    if param_type == "pose":
        return _validate_pose_value(value)

    if param_type == "float_list":
        if not isinstance(value, list) or not all(_is_numeric(item) for item in value):
            return "must be a list of numeric values"
        return None

    if param_type == "string_list":
        if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
            return "must be a list of strings"
        return None

    if param_type == "dict":
        if not isinstance(value, dict):
            return f"must be a mapping, got {type(value).__name__}"
        return None

    return None


def _validate_tree_node_semantics(
    node: TreeNode,
    resolved_params: dict[str, Any],
    path: str,
    namespace_by_id: Mapping[str, BlackboardNamespace],
    namespace_fields_by_id: Mapping[str, Mapping[str, BlackboardNamespaceField]],
) -> list[TreeValidationIssue]:
    issues: list[TreeValidationIssue] = []

    # Scoped motion trip nodes share the same
    # motion-goal shape as `move_manipulator_action` (they declare the
    # full parameter set in the catalog for motion-field parity). The
    # original validator only covered `move_manipulator_action`, so
    # `move_until_signal` / `probe_search_move` would load with
    # missing joint targets / mismatched arrays / missing pose_goal /
    # malformed relative_move, and the error would surface at runtime
    # in the kernel. For industrial contact/search motion, fail-fast
    # at program load. Treat all three IDs uniformly here.
    if node.id in {
        "move_manipulator_action",
        "move_until_signal",
        "probe_search_move",
    }:
        motion_type = resolved_params.get("motion_type")
        if motion_type == "joint_goal":
            joint_names = resolved_params.get("joint_goal_names")
            joint_positions = resolved_params.get("joint_goal_positions_rad")
            if not joint_names:
                issues.append(
                    TreeValidationIssue(
                        path=f"{path}.params.joint_goal_names",
                        message="joint_goal_names is required when motion_type=joint_goal",
                    )
                )
            if not joint_positions:
                issues.append(
                    TreeValidationIssue(
                        path=f"{path}.params.joint_goal_positions_rad",
                        message="joint_goal_positions_rad is required when motion_type=joint_goal",
                    )
                )
            if (
                isinstance(joint_names, list)
                and isinstance(joint_positions, list)
                and joint_names
                and joint_positions
                and len(joint_names) != len(joint_positions)
            ):
                issues.append(
                    TreeValidationIssue(
                        path=f"{path}.params.joint_goal_positions_rad",
                        message=(
                            "joint_goal_names and joint_goal_positions_rad must have the same length"
                        ),
                    )
                )
        elif motion_type == "pose_goal" and not resolved_params.get("pose_goal"):
            issues.append(
                TreeValidationIssue(
                    path=f"{path}.params.pose_goal",
                    message="pose_goal is required when motion_type=pose_goal",
                )
            )
        elif motion_type == "relative_move" and not resolved_params.get("relative_move"):
            issues.append(
                TreeValidationIssue(
                    path=f"{path}.params.relative_move",
                    message="relative_move is required when motion_type=relative_move",
                )
            )
        elif motion_type == "relative_move":
            relative_move_error = _validate_relative_move_value(
                resolved_params.get("relative_move"),
                namespace_by_id,
                namespace_fields_by_id,
            )
            if relative_move_error is not None:
                issues.append(
                    TreeValidationIssue(
                        path=f"{path}.params.relative_move",
                        message=relative_move_error,
                    )
                )

    if node.id in {"add_collision_object", "upsert_collision_object"}:
        shape_type = resolved_params.get("shape_type")
        pose = resolved_params.get("pose")
        if pose is None:
            issues.append(
                TreeValidationIssue(
                    path=f"{path}.params.pose",
                    message=f"pose is required for {node.id}",
                )
            )
        if shape_type == "box":
            dimensions = resolved_params.get("box_dimensions_m")
            if not _is_numeric_list(dimensions, 3) or any(value <= 0 for value in dimensions):
                issues.append(
                    TreeValidationIssue(
                        path=f"{path}.params.box_dimensions_m",
                        message="box_dimensions_m must be a list of 3 positive numeric values",
                    )
                )
        elif shape_type == "sphere":
            radius = resolved_params.get("sphere_radius_m")
            if not _is_numeric(radius) or radius <= 0:
                issues.append(
                    TreeValidationIssue(
                        path=f"{path}.params.sphere_radius_m",
                        message="sphere_radius_m must be a positive numeric value",
                    )
                )
        elif shape_type == "cylinder":
            radius = resolved_params.get("cylinder_radius_m")
            height = resolved_params.get("cylinder_height_m")
            if not _is_numeric(radius) or radius <= 0:
                issues.append(
                    TreeValidationIssue(
                        path=f"{path}.params.cylinder_radius_m",
                        message="cylinder_radius_m must be a positive numeric value",
                    )
                )
            if not _is_numeric(height) or height <= 0:
                issues.append(
                    TreeValidationIssue(
                        path=f"{path}.params.cylinder_height_m",
                        message="cylinder_height_m must be a positive numeric value",
                    )
                )
        elif shape_type == "mesh":
            resource_uri = resolved_params.get("mesh_resource_uri")
            if not isinstance(resource_uri, str) or not resource_uri.strip():
                issues.append(
                    TreeValidationIssue(
                        path=f"{path}.params.mesh_resource_uri",
                        message="mesh_resource_uri is required when shape_type=mesh",
                    )
                )
            mesh_scale = resolved_params.get("mesh_scale")
            if mesh_scale is not None:
                if not _is_numeric_list(mesh_scale, 3) or any(value <= 0 for value in mesh_scale):
                    issues.append(
                        TreeValidationIssue(
                            path=f"{path}.params.mesh_scale",
                            message="mesh_scale must be a list of 3 positive numeric values",
                        )
                    )
        else:
            issues.append(
                TreeValidationIssue(
                    path=f"{path}.params.shape_type",
                    message="shape_type must be one of: box, sphere, cylinder, mesh",
                )
            )

    if node.id == "update_collision_object_pose" and resolved_params.get("pose") is None:
        issues.append(
            TreeValidationIssue(
                path=f"{path}.params.pose",
                message="pose is required for update_collision_object_pose",
            )
        )

    return issues


def _effective_node_required_capabilities(
    entry: Any,
    resolved_params: Mapping[str, Any],
) -> set[str]:
    required = set(entry.required_capabilities)
    for param in getattr(entry, "parameters", []):
        if getattr(param, "type", None) != "enum" or param.name not in resolved_params:
            continue
        option = _enum_option_for_value(param, resolved_params.get(param.name))
        if option is not None:
            required.update(option.required_capabilities)
    return required


def _enum_option_for_value(param: Any, value: Any) -> Any | None:
    for option in getattr(param, "enum_options", []):
        if option.value == value:
            return option
    return None


def collect_tree_validation_issues(
    node: TreeNode,
    catalog: NodeCatalog,
    *,
    parameter_defaults: dict[str, Any] | None = None,
    blackboard_namespaces: dict[str, BlackboardNamespace] | None = None,
    blackboard_keys: dict[str, BlackboardKey] | None = None,
    skill_catalog: SkillCatalog | None = None,
    program_profile: str | None = None,
    deployment_capability_keys: set[str] | None = None,
    deployment: Any = None,
) -> tuple[list[TreeValidationIssue], list[TreeValidationIssue]]:
    """Validate a tree against the catalog and collect all issues."""
    errors: list[TreeValidationIssue] = []
    warnings: list[TreeValidationIssue] = []
    seen_names: dict[str, str] = {}
    blackboard_namespace_producers: list[_BlackboardNamespaceProducer] = []
    blackboard_namespace_references: list[_BlackboardNamespaceReference] = []
    resolver = ParameterResolver(parameter_defaults) if parameter_defaults is not None else None
    namespace_by_id = dict(blackboard_namespaces or {})
    key_by_id = dict(blackboard_keys or {})
    namespace_fields_by_id = {
        namespace_id: {field.name: field for field in namespace.fields}
        for namespace_id, namespace in namespace_by_id.items()
    }

    def visit(current: TreeNode, path: str) -> None:
        registered_name = False
        if current.name in seen_names:
            errors.append(
                TreeValidationIssue(
                    path=path,
                    message=f"Duplicate node instance name: '{current.name}'",
                )
            )
        else:
            seen_names[current.name] = path
            registered_name = True

        if skill_catalog is not None and skill_catalog.contains(current.id):
            resolved_invocation_params = dict(current.params)
            if resolver is not None:
                try:
                    resolved_invocation_params = resolver.resolve(current.params)
                except ValueError as exc:
                    errors.append(
                        TreeValidationIssue(
                            path=f"{path}.params",
                            message=str(exc),
                        )
                    )
                    return

            skill = skill_catalog.get(current.id)
            assert skill is not None

            if current.children:
                errors.append(
                    TreeValidationIssue(
                        path=path,
                        message=f"Skill '{current.id}' may not define explicit children",
                    )
                )
                return

            try:
                skill_catalog.validate_invocation(current.id, resolved_invocation_params)
            except ValueError as exc:
                errors.append(
                    TreeValidationIssue(
                        path=f"{path}.params",
                        message=str(exc),
                    )
                )
                return

            if (
                program_profile is not None
                and skill.compatible_profiles
                and program_profile not in skill.compatible_profiles
            ):
                errors.append(
                    TreeValidationIssue(
                        path=path,
                        message=(
                            f"Skill '{current.id}' is not compatible with program profile "
                            f"'{program_profile}'"
                        ),
                    )
                )

            if deployment_capability_keys is not None:
                missing_capabilities = sorted(
                    capability
                    for capability in skill.requires_capabilities
                    if capability not in deployment_capability_keys
                )
                if missing_capabilities:
                    errors.append(
                        TreeValidationIssue(
                            path=path,
                            message=(
                                f"Skill '{current.id}' requires capabilities "
                                f"{missing_capabilities}"
                            ),
                        )
                    )

            try:
                expanded_tree = tree_node_from_dict(
                    skill_catalog.expand_node_dict(
                        {
                            "id": current.id,
                            "name": current.name,
                            "params": resolved_invocation_params,
                            "children": [],
                        }
                    ),
                    path=path,
                )
            except ValueError as exc:
                errors.append(
                    TreeValidationIssue(
                        path=path,
                        message=str(exc),
                    )
                )
                return

            if registered_name:
                seen_names.pop(current.name, None)
            visit(expanded_tree, path)
            return

        try:
            entry = catalog.get(current.id)
        except KeyError:
            errors.append(
                TreeValidationIssue(
                    path=path,
                    message=f"Unknown catalog entry: '{current.id}'",
                )
            )
            return

        if program_profile is not None and not profile_satisfied(entry.profiles_min, program_profile):
            errors.append(
                TreeValidationIssue(
                    path=path,
                    message=(
                        f"Node '{current.name}' ({current.id}) requires program profile "
                        f">= {entry.profiles_min}, got {program_profile}"
                    ),
                )
            )

        if current.modification_policy.get("intervention_delegate") == "program_only":
            # program_only is valid with recovery decorators; validate constraints
            rp = current.recovery_policy
            if rp.get("allow_intervention", False):
                errors.append(
                    TreeValidationIssue(
                        path=path,
                        message=(
                            "intervention_delegate=program_only requires "
                            "recovery_policy.allow_intervention=false"
                        ),
                    )
                )
            if "intervention" in rp.get("allowed_recovery_actions", []):
                errors.append(
                    TreeValidationIssue(
                        path=path,
                        message=(
                            "intervention_delegate=program_only must not include "
                            "'intervention' in recovery_policy.allowed_recovery_actions"
                        ),
                    )
                )

        n_children = len(current.children)
        if n_children < entry.children_min:
            errors.append(
                TreeValidationIssue(
                    path=path,
                    message=(
                        f"Node '{current.name}' ({current.id}): requires at least "
                        f"{entry.children_min} children, got {n_children}"
                    ),
                )
            )
        if entry.children_max >= 0 and n_children > entry.children_max:
            kind_hint = (
                "leaf nodes never accept children — pick a composite parent (sequence, fallback, "
                "parallel) or a decorator (repeat, retry, inverter)"
                if entry.children_max == 0
                else f"this node kind '{entry.kind}' accepts at most {entry.children_max} child"
                + ("" if entry.children_max == 1 else "ren")
            )
            errors.append(
                TreeValidationIssue(
                    path=path,
                    message=(
                        f"Node '{current.name}' ({current.id}): allows at most "
                        f"{entry.children_max} children, got {n_children}. {kind_hint}"
                    ),
                )
            )

        resolved_params = dict(current.params)
        if resolver is not None:
            try:
                resolved_params = resolver.resolve(current.params)
            except ValueError as exc:
                errors.append(
                    TreeValidationIssue(
                        path=f"{path}.params",
                        message=str(exc),
                    )
                )

        declared_params = {param.name: param for param in entry.parameters}
        for param in entry.parameters:
            param_profile_available = program_profile is None or profile_satisfied(
                param.profiles_min,
                program_profile,
            )
            if not param_profile_available:
                if param.name in current.params:
                    errors.append(
                        TreeValidationIssue(
                            path=f"{path}.params.{param.name}",
                            message=(
                                f"Parameter '{param.name}' requires program profile "
                                f">= {param.profiles_min}, got {program_profile}"
                            ),
                        )
                    )
                continue

            if param.required and param.name not in current.params and param.default is None:
                expected = _format_param_schema(entry)
                errors.append(
                    TreeValidationIssue(
                        path=f"{path}.params.{param.name}",
                        message=(
                            f"Node '{current.name}' ({current.id}): missing required "
                            f"parameter '{param.name}'. Got params={sorted(current.params.keys())}. "
                            f"Expected schema: {expected}"
                        ),
                    )
                )
                continue

            if param.name not in current.params:
                continue

            raw_value = current.params[param.name]
            if parameter_defaults is None and _contains_parameter_reference(raw_value):
                warnings.append(
                    TreeValidationIssue(
                        path=f"{path}.params.{param.name}",
                        message=(
                            "Parameter reference validation skipped because no program "
                            "parameter context is loaded"
                        ),
                    )
                )
                continue

            value = resolved_params.get(param.name, raw_value)
            skip_type_validation = False
            if param.blackboard_role in {"namespace_writer", "namespace_reader"}:
                value, binding_error = _resolve_namespace_binding(
                    param_name=param.name,
                    value=value,
                    namespace_by_id=namespace_by_id,
                    expected_contract=param.blackboard_contract,
                )
                if binding_error is not None:
                    errors.append(
                        TreeValidationIssue(
                            path=f"{path}.params.{param.name}",
                            message=binding_error,
                        )
                    )
                    continue
                resolved_params[param.name] = value

            if param.blackboard_role in {"key_reader", "key_writer"}:
                if _key_ref_id(value) is not None:
                    value, binding_error = _resolve_key_binding(
                        param_name=param.name,
                        value=value,
                        key_by_id=key_by_id,
                        expected_type=param.blackboard_value_type,
                    )
                elif param.blackboard_role == "key_reader":
                    value, binding_error = _resolve_field_binding(
                        param_name=param.name,
                        value=value,
                        namespace_by_id=namespace_by_id,
                        namespace_fields_by_id=namespace_fields_by_id,
                        expected_type=param.blackboard_value_type,
                        expected_contract=param.blackboard_contract,
                    )
                    skip_type_validation = True
                else:
                    value, binding_error = None, f"{param.name} must be a key_ref binding"
                if binding_error is not None:
                    errors.append(
                        TreeValidationIssue(
                            path=f"{path}.params.{param.name}",
                            message=binding_error,
                        )
                    )
                    continue
                resolved_params[param.name] = value

            if param.blackboard_role == "literal_or_key_reader":
                if _is_blackboard_reference_string(raw_value):
                    errors.append(
                        TreeValidationIssue(
                            path=f"{path}.params.{param.name}",
                            message=(
                                f"{param.name} must use a key_ref or field_ref binding instead of a raw "
                                "blackboard reference string"
                            ),
                        )
                    )
                    continue
                binding_error = None
                if _key_ref_id(value) is not None:
                    value, binding_error = _resolve_key_binding(
                        param_name=param.name,
                        value=value,
                        key_by_id=key_by_id,
                        expected_type=param.blackboard_value_type,
                    )
                elif _field_ref_target(value) is not None:
                    value, binding_error = _resolve_field_binding(
                        param_name=param.name,
                        value=value,
                        namespace_by_id=namespace_by_id,
                        namespace_fields_by_id=namespace_fields_by_id,
                        expected_type=param.blackboard_value_type,
                        expected_contract=param.blackboard_contract,
                    )
                    skip_type_validation = True
                if binding_error is not None:
                    errors.append(
                        TreeValidationIssue(
                            path=f"{path}.params.{param.name}",
                            message=binding_error,
                        )
                    )
                    continue
                resolved_params[param.name] = value

            if param.blackboard_role == "field_reader":
                if _is_blackboard_reference_string(raw_value):
                    errors.append(
                        TreeValidationIssue(
                            path=f"{path}.params.{param.name}",
                            message=(
                                f"{param.name} must use a field_ref binding instead of a raw "
                                "blackboard reference string"
                            ),
                        )
                    )
                    continue
                if _field_ref_target(value) is not None:
                    value, binding_error = _resolve_field_binding(
                        param_name=param.name,
                        value=value,
                        namespace_by_id=namespace_by_id,
                        namespace_fields_by_id=namespace_fields_by_id,
                        expected_type=param.blackboard_value_type or param.type,
                        expected_contract=param.blackboard_contract,
                    )
                    if binding_error is not None:
                        errors.append(
                            TreeValidationIssue(
                                path=f"{path}.params.{param.name}",
                                message=binding_error,
                            )
                        )
                        continue
                    resolved_params[param.name] = value
                    skip_type_validation = True

            if param.ui_editor == "template_variable_map":
                template_variables_error = _validate_template_variable_map(
                    value,
                    namespace_by_id,
                    namespace_fields_by_id,
                    key_by_id,
                )
                if template_variables_error is not None:
                    errors.append(
                        TreeValidationIssue(
                            path=f"{path}.params.{param.name}",
                            message=template_variables_error,
                        )
                    )
                    continue

            if not skip_type_validation:
                type_error = _validate_param_type(param.type, value)
                if type_error is not None:
                    errors.append(
                        TreeValidationIssue(
                            path=f"{path}.params.{param.name}",
                            message=f"{param.name} {type_error}",
                        )
                    )
                    continue

            if param.type == "enum" and value not in param.enum_values:
                errors.append(
                    TreeValidationIssue(
                        path=f"{path}.params.{param.name}",
                        message=(
                            f"{param.name} must be one of {param.enum_values}, got {value!r}"
                        ),
                    )
                )
                continue
            if param.type == "enum":
                option = _enum_option_for_value(param, value)
                if (
                    option is not None
                    and program_profile is not None
                    and not profile_satisfied(option.profiles_min, program_profile)
                ):
                    errors.append(
                        TreeValidationIssue(
                            path=f"{path}.params.{param.name}",
                            message=(
                                f"{param.name} value {value!r} requires program profile "
                                f">= {option.profiles_min}, got {program_profile}"
                            ),
                        )
                    )
                    continue

            if param.type in {"float", "int"} and param.constraints:
                constraint_error = _validate_constraints(float(value), param.constraints)
                if constraint_error is not None:
                    errors.append(
                        TreeValidationIssue(
                            path=f"{path}.params.{param.name}",
                            message=f"{param.name} {constraint_error}",
                        )
                    )

            if (
                param.blackboard_role == "namespace_writer"
                and isinstance(value, str)
                and value.strip()
            ):
                blackboard_namespace_producers.append(
                    _BlackboardNamespaceProducer(
                        path=f"{path}.params.{param.name}",
                        node_id=current.id,
                        node_name=current.name,
                        namespace_id=str(_namespace_ref_id(raw_value)),
                        contract=str(param.blackboard_contract or ""),
                        prefix=value.strip(),
                    )
                )

            if (
                param.blackboard_role == "namespace_reader"
                and isinstance(value, str)
                and value.strip()
            ):
                blackboard_namespace_references.append(
                    _BlackboardNamespaceReference(
                        path=f"{path}.params.{param.name}",
                        param_name=param.name,
                        namespace_id=str(_namespace_ref_id(raw_value)),
                        expected_contract=str(param.blackboard_contract or ""),
                        expected_label=_blackboard_contract_label(
                            str(param.blackboard_contract or "")
                        ),
                        prefix=value.strip(),
                    )
                )

        for param_name in current.params:
            if param_name not in declared_params:
                warnings.append(
                    TreeValidationIssue(
                        path=f"{path}.params.{param_name}",
                        message=(
                            f"Unexpected parameter '{param_name}' for node '{current.id}'"
                        ),
                    )
                )

        if deployment_capability_keys is not None:
            missing_capabilities = sorted(
                capability
                for capability in _effective_node_required_capabilities(
                    entry,
                    resolved_params,
                )
                if capability not in deployment_capability_keys
            )
            if missing_capabilities:
                errors.append(
                    TreeValidationIssue(
                        path=path,
                        message=(
                            f"Node '{current.name}' ({current.id}) requires capabilities "
                            f"{missing_capabilities}"
                        ),
                    )
                )

        errors.extend(
            _validate_tree_node_semantics(
                current,
                resolved_params,
                path,
                namespace_by_id,
                namespace_fields_by_id,
            )
        )
        # query_model deployment cross-validation (OP-7)
        if current.id == "query_model" and deployment is not None:
            ep_alias = resolved_params.get("model_endpoint_alias", "")
            tmpl_id = resolved_params.get("prompt_template_id", "")
            if ep_alias and hasattr(deployment, "model_endpoints"):
                if ep_alias not in deployment.model_endpoints:
                    errors.append(
                        TreeValidationIssue(
                            path=f"{path}.params.model_endpoint_alias",
                            message=f"Model endpoint alias '{ep_alias}' not found in deployment",
                        )
                    )
            if tmpl_id and hasattr(deployment, "prompt_templates"):
                if tmpl_id not in deployment.prompt_templates:
                    errors.append(
                        TreeValidationIssue(
                            path=f"{path}.params.prompt_template_id",
                            message=f"Prompt template '{tmpl_id}' not found in deployment",
                        )
                    )

        for index, child in enumerate(current.children):
            visit(child, f"{path}.children[{index}]")

    visit(node, "tree")

    producers_by_namespace_id: dict[str, list[_BlackboardNamespaceProducer]] = {}
    for producer in blackboard_namespace_producers:
        producers_by_namespace_id.setdefault(producer.namespace_id, []).append(producer)

    for namespace_id, producers in producers_by_namespace_id.items():
        if len(producers) <= 1:
            continue
        producer_list = ", ".join(
            f"'{producer.node_name}' ({producer.node_id})" for producer in producers
        )
        for producer in producers:
            errors.append(
                TreeValidationIssue(
                    path=producer.path,
                    message=(
                        f"output_key_prefix namespace_ref '{namespace_id}' is used by multiple producer nodes: "
                        f"{producer_list}"
                    ),
                )
            )

    for reference in blackboard_namespace_references:
        matching_producers = producers_by_namespace_id.get(reference.namespace_id, [])
        if matching_producers:
            continue
        declared_namespace = namespace_by_id.get(reference.namespace_id)
        if declared_namespace is None:
            message = (
                f"{reference.param_name} namespace_ref '{reference.namespace_id}' is not declared in "
                "program.blackboard.namespaces"
            )
        else:
            message = (
                f"{reference.param_name} namespace_ref '{reference.namespace_id}' "
                f"({declared_namespace.prefix}) does not match any {reference.expected_label} "
                "producer in the current tree"
            )
        errors.append(
            TreeValidationIssue(
                path=reference.path,
                message=message,
            )
        )

    for producer in blackboard_namespace_producers:
        declared_namespace = namespace_by_id.get(producer.namespace_id)
        if declared_namespace is None:
            errors.append(
                TreeValidationIssue(
                    path=producer.path,
                    message=(
                        f"output_key_prefix namespace_ref '{producer.namespace_id}' is not declared in "
                        "program.blackboard.namespaces"
                    ),
                )
            )
            continue
        if declared_namespace.contract != producer.contract:
            errors.append(
                TreeValidationIssue(
                    path=producer.path,
                    message=(
                        f"output_key_prefix namespace_ref '{producer.namespace_id}' is declared as contract "
                        f"'{declared_namespace.contract}', but node '{producer.node_name}' "
                        f"requires contract '{producer.contract}'"
                    ),
                )
            )

    return errors, warnings


class Program:
    """Loads and validates a ManyForge program artifact from YAML."""

    def __init__(
        self,
        path: str | Path,
        catalog: NodeCatalog,
        *,
        skill_catalog: SkillCatalog | None = None,
        deployment_capability_keys: set[str] | None = None,
    ):
        self._path = Path(path)
        self._catalog = catalog
        self._skill_catalog = skill_catalog or load_skill_catalog(
            default_skills_dir(),
            catalog=catalog,
            strict=True,
        )
        self._deployment_capability_keys = deployment_capability_keys
        self.name: str = ""
        self.description: str = ""
        self.program_profile: str = ""
        self.catalog_version: str = ""
        self.robot: Optional[RobotProfile] = None
        self.scene: list[SceneResource] = []
        self.parameters: dict[str, ProgramParameter] = {}
        self.blackboard_namespaces: dict[str, BlackboardNamespace] = {}
        self.blackboard_keys: dict[str, BlackboardKey] = {}
        self.tree: Optional[TreeNode] = None
        self._load_and_validate()

    @classmethod
    def from_dict(
        cls,
        data: dict[str, Any],
        catalog: NodeCatalog,
        path: str | Path = "<memory>",
        *,
        skill_catalog: SkillCatalog | None = None,
        deployment_capability_keys: set[str] | None = None,
    ) -> "Program":
        """Construct and validate a program directly from a mapping."""
        self = cls.__new__(cls)
        self._path = Path(path)
        self._catalog = catalog
        self._skill_catalog = skill_catalog or load_skill_catalog(
            default_skills_dir(),
            catalog=catalog,
            strict=True,
        )
        self._deployment_capability_keys = deployment_capability_keys
        self.name = ""
        self.description = ""
        self.program_profile = ""
        self.catalog_version = ""
        self.robot = None
        self.scene = []
        self.parameters = {}
        self.blackboard_namespaces = {}
        self.blackboard_keys = {}
        self.tree = None
        self._load_from_data_and_validate(data)
        return self

    def required_capabilities(self) -> set[str]:
        """Return the union of capability keys required by the current tree."""
        if self.tree is None:
            return set()

        resolver = (
            ParameterResolver(self.get_parameter_defaults())
            if self.parameters
            else None
        )
        required: set[str] = set()

        def visit(current: TreeNode) -> None:
            if self._skill_catalog is not None and self._skill_catalog.contains(current.id):
                skill = self._skill_catalog.get(current.id)
                assert skill is not None
                required.update(skill.requires_capabilities)
                resolved_params = dict(current.params)
                if resolver is not None:
                    resolved_params = resolver.resolve(current.params)
                expanded_tree = tree_node_from_dict(
                    self._skill_catalog.expand_node_dict(
                        {
                            "id": current.id,
                            "name": current.name,
                            "params": resolved_params,
                            "children": [],
                        }
                    ),
                    path="tree",
                )
                visit(expanded_tree)
                return

            entry = self._catalog.get(current.id)
            resolved_params = dict(current.params)
            if resolver is not None:
                resolved_params = resolver.resolve(current.params)
            required.update(
                _effective_node_required_capabilities(
                    entry,
                    resolved_params,
                )
            )
            for child in current.children:
                visit(child)

        visit(self.tree)
        return required

    def _load_and_validate(self) -> None:
        with open(self._path) as f:
            data = yaml.safe_load(f)

        self._load_from_data_and_validate(data)

    def _load_from_data_and_validate(self, data: dict[str, Any]) -> None:
        self.scene = []
        self.parameters = {}
        self.blackboard_namespaces = {}
        self.blackboard_keys = {}
        self.tree = None

        if not isinstance(data, dict):
            raise ProgramValidationError("Program must be a YAML mapping")

        removed_fields = sorted(_REMOVED_COOPERATIVE_INTERRUPT_FIELDS.intersection(data))
        if removed_fields:
            raise ProgramValidationError(
                "Cooperative interrupt fields were removed and must not be "
                f"present in program artifacts: {removed_fields}. Use the "
                "kernel-native interrupt model once it is available."
            )

        # Schema validation
        api_version = data.get("api_version")
        if api_version != EXPECTED_API_VERSION:
            raise ProgramValidationError(
                f"Expected api_version '{EXPECTED_API_VERSION}', got '{api_version}'"
            )

        self.program_profile = data.get("program_profile", "")
        if not isinstance(self.program_profile, str) or not self.program_profile.strip():
            raise ProgramValidationError("program_profile is required")
        try:
            get_required_keys(self.program_profile)
        except ValueError as exc:
            raise ProgramValidationError(str(exc)) from exc

        self.catalog_version = data.get("catalog_version", "")
        self.name = data.get("name", "")
        if not isinstance(self.catalog_version, str) or not self.catalog_version.strip():
            raise ProgramValidationError("catalog_version is required")
        if not SEMVER_RE.match(self.catalog_version):
            raise ProgramValidationError(
                f"catalog_version must match X.Y.Z, got '{self.catalog_version}'"
            )
        if not isinstance(self.name, str) or not self.name.strip():
            raise ProgramValidationError("name is required")
        self.description = data.get("description", "")

        # Robot profile
        robot_data = data.get("robot")
        if not robot_data:
            raise ProgramValidationError("robot section is required")
        for field_name in ("robot_id", "group_id", "base_frame", "tcp_frame"):
            if field_name not in robot_data:
                raise ProgramValidationError(
                    f"robot.{field_name} is required"
                )
        base_pose = robot_data.get("base_pose", {})
        if base_pose is None:
            base_pose = {}
        if base_pose:
            base_pose_error = _validate_robot_base_pose_value(base_pose)
            if base_pose_error is not None:
                raise ProgramValidationError(f"robot.{base_pose_error}")
        self.robot = RobotProfile(
            robot_id=robot_data["robot_id"],
            group_id=robot_data["group_id"],
            base_frame=robot_data["base_frame"],
            tcp_frame=robot_data["tcp_frame"],
            base_pose=dict(base_pose),
        )

        # Scene resources
        scene_data = data.get("scene", [])
        if not isinstance(scene_data, list):
            raise ProgramValidationError("scene must be a list")
        scene_ids: set[str] = set()
        for index, s in enumerate(scene_data):
            scene_error = _validate_scene_resource_value(s)
            if scene_error is not None:
                raise ProgramValidationError(f"scene[{index}] {scene_error}")
            oid = s.get("object_id", "")
            if oid in scene_ids:
                raise ProgramValidationError(
                    f"Duplicate scene object_id: '{oid}'"
                )
            scene_ids.add(oid)
            self.scene.append(
                SceneResource(
                    object_id=oid,
                    shape=s.get("shape", {}),
                    pose=s.get("pose", {}),
                    metadata=dict(s.get("metadata", {}) or {}),
                )
            )
        for resource in self.scene:
            for contact in resource.metadata.get("support_contacts", []) or []:
                support_object_id = contact.get("support_object_id")
                if support_object_id == resource.object_id:
                    raise ProgramValidationError(
                        "scene resource support_contacts cannot reference the same object"
                    )
                if support_object_id not in scene_ids:
                    raise ProgramValidationError(
                        f"scene resource '{resource.object_id}' support_contacts "
                        f"references unknown object_id '{support_object_id}'"
                    )

        # Parameters
        param_names: set[str] = set()
        for p in data.get("parameters", []):
            name = p.get("name", "")
            if name in param_names:
                raise ProgramValidationError(
                    f"Duplicate parameter name: '{name}'"
                )
            param_names.add(name)
            self.parameters[name] = ProgramParameter(
                name=name,
                type=p.get("type", "string"),
                default=p.get("default"),
                constraints=p.get("constraints", {}),
                description=p.get("description", ""),
            )

        # Blackboard declarations
        blackboard_data = data.get("blackboard", {}) or {}
        if blackboard_data and not isinstance(blackboard_data, Mapping):
            raise ProgramValidationError("blackboard must be a mapping")

        namespace_ids: set[str] = set()
        namespace_prefixes: set[str] = set()
        for namespace in blackboard_data.get("namespaces", []):
            namespace_id = str(namespace.get("id", "")).strip()
            contract = str(namespace.get("contract", "")).strip()
            prefix = str(namespace.get("prefix", "")).strip()
            if not namespace_id:
                raise ProgramValidationError("blackboard.namespaces[].id is required")
            if not contract:
                raise ProgramValidationError(
                    f"blackboard.namespaces[{namespace_id!r}].contract is required"
                )
            if not prefix:
                raise ProgramValidationError(
                    f"blackboard.namespaces[{namespace_id!r}].prefix is required"
                )
            if namespace_id in namespace_ids:
                raise ProgramValidationError(
                    f"Duplicate blackboard namespace id: '{namespace_id}'"
                )
            if prefix in namespace_prefixes:
                raise ProgramValidationError(
                    f"Duplicate blackboard namespace prefix: '{prefix}'"
                )
            raw_fields = namespace.get("fields", []) or []
            if not isinstance(raw_fields, list):
                raise ProgramValidationError(
                    f"blackboard.namespaces[{namespace_id!r}].fields must be a list"
                )
            field_entries: list[BlackboardNamespaceField] = []
            seen_field_names: set[str] = set()
            for field_decl in raw_fields:
                field_name = str(field_decl.get("name", "")).strip()
                field_type = str(field_decl.get("type", "")).strip()
                if not field_name:
                    raise ProgramValidationError(
                        f"blackboard.namespaces[{namespace_id!r}].fields[].name is required"
                    )
                if field_name in seen_field_names:
                    raise ProgramValidationError(
                        f"Duplicate blackboard field '{field_name}' in namespace '{namespace_id}'"
                    )
                if field_type not in _BLACKBOARD_FIELD_TYPES:
                    allowed = ", ".join(sorted(_BLACKBOARD_FIELD_TYPES))
                    raise ProgramValidationError(
                        f"blackboard.namespaces[{namespace_id!r}].fields[{field_name!r}].type "
                        f"must be one of: {allowed}"
                    )
                seen_field_names.add(field_name)
                field_entries.append(
                    BlackboardNamespaceField(
                        name=field_name,
                        type=field_type,
                        description=str(field_decl.get("description", "")),
                    )
                )
            namespace_ids.add(namespace_id)
            namespace_prefixes.add(prefix)
            self.blackboard_namespaces[namespace_id] = BlackboardNamespace(
                id=namespace_id,
                contract=contract,
                prefix=prefix,
                description=str(namespace.get("description", "")),
                fields=field_entries,
            )

        key_ids: set[str] = set()
        key_names: set[str] = set()
        for key_decl in blackboard_data.get("keys", []):
            key_id = str(key_decl.get("id", "")).strip()
            key_type = str(key_decl.get("type", "")).strip()
            key_name = str(key_decl.get("key", "")).strip()
            if not key_id:
                raise ProgramValidationError("blackboard.keys[].id is required")
            if not key_type:
                raise ProgramValidationError(
                    f"blackboard.keys[{key_id!r}].type is required"
                )
            if not key_name:
                raise ProgramValidationError(
                    f"blackboard.keys[{key_id!r}].key is required"
                )
            if key_id in key_ids:
                raise ProgramValidationError(
                    f"Duplicate blackboard key id: '{key_id}'"
                )
            if key_name in key_names:
                raise ProgramValidationError(
                    f"Duplicate blackboard key: '{key_name}'"
                )
            key_ids.add(key_id)
            key_names.add(key_name)
            self.blackboard_keys[key_id] = BlackboardKey(
                id=key_id,
                type=key_type,
                key=key_name,
                description=str(key_decl.get("description", "")),
            )

        # Tree
        tree_data = data.get("tree")
        if not tree_data:
            raise ProgramValidationError("tree section is required")
        self.tree = self._parse_tree(tree_data)

        # Structural validation
        self._validate_tree(self.tree)

    def _parse_tree(self, data: dict[str, Any]) -> TreeNode:
        return tree_node_from_dict(data)

    def _validate_tree(self, node: TreeNode) -> None:
        """Validate tree against catalog: node ids, required params, child counts, unique names."""
        errors, _warnings = collect_tree_validation_issues(
            node,
            self._catalog,
            parameter_defaults=self.get_parameter_defaults(),
            blackboard_namespaces=self.blackboard_namespaces,
            blackboard_keys=self.blackboard_keys,
            skill_catalog=self._skill_catalog,
            program_profile=self.program_profile,
            deployment_capability_keys=self._deployment_capability_keys,
        )
        if errors:
            raise ProgramValidationError(errors[0].message)

    def to_dict(self) -> dict[str, Any]:
        """Serialize to dict matching manyforge.program.v0 schema."""
        d: dict[str, Any] = {
            "api_version": EXPECTED_API_VERSION,
            "program_profile": self.program_profile,
            "name": self.name,
            "description": self.description,
        }
        if self.catalog_version:
            d["catalog_version"] = self.catalog_version
        d["robot"] = {
            "robot_id": self.robot.robot_id,
            "group_id": self.robot.group_id,
            "base_frame": self.robot.base_frame,
            "tcp_frame": self.robot.tcp_frame,
        }
        if self.robot.base_pose:
            d["robot"]["base_pose"] = self.robot.base_pose
        d["scene"] = []
        for s in self.scene:
            scene_resource = {
                "object_id": s.object_id,
                "shape": s.shape,
                "pose": s.pose,
            }
            if s.metadata:
                scene_resource["metadata"] = s.metadata
            d["scene"].append(scene_resource)
        d["parameters"] = [
            {
                "name": p.name,
                "type": p.type,
                "default": p.default,
                "constraints": p.constraints,
                "description": p.description,
            }
            for p in self.parameters.values()
        ]
        if self.blackboard_namespaces or self.blackboard_keys:
            d["blackboard"] = {
                "namespaces": [
                    {
                        "id": namespace.id,
                        "contract": namespace.contract,
                        "prefix": namespace.prefix,
                        "description": namespace.description,
                        "fields": [
                            {
                                "name": field.name,
                                "type": field.type,
                                "description": field.description,
                            }
                            for field in namespace.fields
                        ],
                    }
                    for namespace in self.blackboard_namespaces.values()
                ],
                "keys": [
                    {
                        "id": key.id,
                        "type": key.type,
                        "key": key.key,
                        "description": key.description,
                    }
                    for key in self.blackboard_keys.values()
                ],
            }
        d["tree"] = self.tree.to_dict() if self.tree else {}
        return d

    def save(self, path: str | Path) -> None:
        """Write program to YAML file."""
        import yaml
        with open(path, "w") as f:
            yaml.dump(self.to_dict(), f, sort_keys=False, default_flow_style=False)

    def update_tree(self, tree_data: dict[str, Any]) -> None:
        """Replace current tree from dict. Re-validates against catalog."""
        new_tree = self._parse_tree(tree_data)
        self._validate_tree(new_tree)
        self.tree = new_tree

    def get_parameter_defaults(self) -> dict[str, Any]:
        """Return {name: default_value} for all program parameters."""
        return {p.name: p.default for p in self.parameters.values()}
