"""Program artifact loader and validator for ManyForge behavior trees."""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Mapping, Optional

import yaml

from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.parameters import PARAM_REF_RE, ParameterResolver
from manyforge_behavior.skill_loader import (
    SkillCatalog,
    default_skills_dir,
    load_skill_catalog,
)


EXPECTED_API_VERSION = "manyforge.program.v0"


@dataclass
class SceneResource:
    object_id: str
    shape: dict[str, Any]
    pose: dict[str, Any]


@dataclass
class ProgramParameter:
    name: str
    type: str
    default: Any
    constraints: dict[str, Any] = field(default_factory=dict)
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


class ProgramValidationError(Exception):
    pass


@dataclass
class TreeValidationIssue:
    path: str
    message: str


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
            raise ProgramValidationError(
                f"{path}.modification_policy.{key} is not a recognized policy field"
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
            raise ProgramValidationError(
                f"{path}.recovery_policy.{key} is not a recognized recovery policy field"
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
) -> list[TreeValidationIssue]:
    issues: list[TreeValidationIssue] = []

    if node.id == "move_manipulator_action":
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

    if node.id == "add_collision_object":
        shape_type = resolved_params.get("shape_type")
        pose = resolved_params.get("pose")
        if pose is None:
            issues.append(
                TreeValidationIssue(
                    path=f"{path}.params.pose",
                    message="pose is required for add_collision_object",
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

    if node.id == "update_collision_object_pose" and resolved_params.get("pose") is None:
        issues.append(
            TreeValidationIssue(
                path=f"{path}.params.pose",
                message="pose is required for update_collision_object_pose",
            )
        )

    return issues


def collect_tree_validation_issues(
    node: TreeNode,
    catalog: NodeCatalog,
    *,
    parameter_defaults: dict[str, Any] | None = None,
    skill_catalog: SkillCatalog | None = None,
    program_profile: str | None = None,
    deployment_capability_keys: set[str] | None = None,
    deployment: Any = None,
) -> tuple[list[TreeValidationIssue], list[TreeValidationIssue]]:
    """Validate a tree against the catalog and collect all issues."""
    errors: list[TreeValidationIssue] = []
    warnings: list[TreeValidationIssue] = []
    seen_names: dict[str, str] = {}
    resolver = ParameterResolver(parameter_defaults) if parameter_defaults is not None else None

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
            errors.append(
                TreeValidationIssue(
                    path=path,
                    message=(
                        f"Node '{current.name}' ({current.id}): allows at most "
                        f"{entry.children_max} children, got {n_children}"
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
            if param.required and param.name not in current.params and param.default is None:
                errors.append(
                    TreeValidationIssue(
                        path=f"{path}.params.{param.name}",
                        message=(
                            f"Node '{current.name}' ({current.id}): missing required "
                            f"parameter '{param.name}'"
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

            if param.type in {"float", "int"} and param.constraints:
                constraint_error = _validate_constraints(float(value), param.constraints)
                if constraint_error is not None:
                    errors.append(
                        TreeValidationIssue(
                            path=f"{path}.params.{param.name}",
                            message=f"{param.name} {constraint_error}",
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

        errors.extend(_validate_tree_node_semantics(current, resolved_params, path))

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
        self.tree = None
        self._load_from_data_and_validate(data)
        return self

    def _load_and_validate(self) -> None:
        with open(self._path) as f:
            data = yaml.safe_load(f)

        self._load_from_data_and_validate(data)

    def _load_from_data_and_validate(self, data: dict[str, Any]) -> None:
        self.scene = []
        self.parameters = {}
        self.tree = None

        if not isinstance(data, dict):
            raise ProgramValidationError("Program must be a YAML mapping")

        # Schema validation
        api_version = data.get("api_version")
        if api_version != EXPECTED_API_VERSION:
            raise ProgramValidationError(
                f"Expected api_version '{EXPECTED_API_VERSION}', got '{api_version}'"
            )

        self.program_profile = data.get("program_profile", "")
        if not self.program_profile:
            raise ProgramValidationError("program_profile is required")

        self.catalog_version = data.get("catalog_version", "")
        self.name = data.get("name", "")
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
        self.robot = RobotProfile(
            robot_id=robot_data["robot_id"],
            group_id=robot_data["group_id"],
            base_frame=robot_data["base_frame"],
            tcp_frame=robot_data["tcp_frame"],
        )

        # Scene resources
        scene_ids: set[str] = set()
        for s in data.get("scene", []):
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
                )
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
        d["scene"] = [
            {
                "object_id": s.object_id,
                "shape": s.shape,
                "pose": s.pose,
            }
            for s in self.scene
        ]
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
