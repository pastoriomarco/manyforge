"""Skill catalog loader and expander for ManyForge behavior trees."""

from __future__ import annotations

from copy import deepcopy
import logging
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

import yaml

from manyforge_behavior.capabilities import CAPABILITY_REGISTRY
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.parameters import PARAM_REF_RE

logger = logging.getLogger(__name__)

_EXPECTED_API_VERSION = "manyforge.skill.v0"
_MAX_NESTING_DEPTH = 2
_INLINE_PARAM_RE = re.compile(r"\$\{param:([^}]+)\}")


@dataclass(frozen=True)
class SkillParameter:
    """A single parameter in a skill definition."""

    name: str
    type: str
    required: bool
    default: Any = None
    description: str = ""


@dataclass(frozen=True)
class SkillDefinition:
    """A loaded skill definition."""

    skill_id: str
    version: str
    name: str
    description: str
    compatible_profiles: list[str]
    requires: list[str]
    provides: list[str]
    modifies: list[str]
    requires_capabilities: list[str]
    requires_tools: list[str]
    parameters: list[SkillParameter]
    body: dict[str, Any]
    example: dict[str, Any]
    validated_by: str = ""

    @property
    def id(self) -> str:
        """Compatibility alias for legacy callers."""
        return self.skill_id

    @property
    def expands_to(self) -> dict[str, Any]:
        """Compatibility alias for legacy callers."""
        return self.body


@dataclass
class SkillCatalog:
    """Collection of loaded and validated skill definitions."""

    skills: list[SkillDefinition] = field(default_factory=list)
    _by_id: dict[str, SkillDefinition] = field(default_factory=dict, repr=False)

    def get(self, skill_id: str) -> Optional[SkillDefinition]:
        """Look up a skill by ID."""
        return self._by_id.get(skill_id)

    def contains(self, skill_id: str) -> bool:
        """Return True when the catalog contains the given skill."""
        return skill_id in self._by_id

    @property
    def skill_ids(self) -> list[str]:
        return [s.skill_id for s in self.skills]

    def expand_node_dict(
        self,
        node: dict[str, Any],
        *,
        max_depth: int = _MAX_NESTING_DEPTH,
    ) -> dict[str, Any]:
        """Expand a tree node dict if it refers to a skill invocation."""
        return self._expand_node_dict(node, depth=0, max_depth=max_depth)

    def validate_invocation(
        self,
        skill_id: str,
        params: dict[str, Any],
    ) -> dict[str, Any]:
        """Validate skill invocation parameters and return merged args."""
        skill = self.get(skill_id)
        if skill is None:
            raise ValueError(f"Unknown skill: '{skill_id}'")

        merged = {
            parameter.name: deepcopy(parameter.default)
            for parameter in skill.parameters
            if parameter.default is not None
        }
        for key, value in params.items():
            if key not in {parameter.name for parameter in skill.parameters}:
                raise ValueError(
                    f"Unknown parameter '{key}' for skill '{skill_id}'"
                )
            merged[key] = deepcopy(value)

        for parameter in skill.parameters:
            if parameter.required and parameter.name not in merged:
                raise ValueError(
                    f"Missing required parameter '{parameter.name}' for skill '{skill_id}'"
                )
            if parameter.name in merged:
                type_error = _validate_skill_param_type(
                    parameter.type,
                    merged[parameter.name],
                )
                if type_error is not None:
                    raise ValueError(
                        f"Parameter '{parameter.name}' for skill '{skill_id}' {type_error}"
                    )

        return merged

    def _expand_node_dict(
        self,
        node: dict[str, Any],
        *,
        depth: int,
        max_depth: int,
        prefix: str = "",
    ) -> dict[str, Any]:
        if not isinstance(node, dict):
            raise ValueError("Skill expansion expects dict nodes")

        node_id = node.get("id")
        node_name = node.get("name", node_id)
        if not isinstance(node_name, str) or not node_name:
            raise ValueError("Expanded skill node name must be a non-empty string")

        full_name = f"{prefix}{node_name}" if prefix else node_name

        if self.contains(node_id):
            if depth >= max_depth:
                raise ValueError(
                    f"Skill nesting exceeds maximum depth {max_depth}: '{node_id}'"
                )
            if node.get("children"):
                raise ValueError(
                    f"Skill '{node_id}' may not define explicit children at invocation time"
                )
            invocation_args = self.validate_invocation(node_id, dict(node.get("params", {})))
            skill = self._by_id[node_id]
            rendered = _render_value(deepcopy(skill.body), invocation_args)
            if not isinstance(rendered, dict):
                raise ValueError(f"Skill '{node_id}' body must expand to a mapping")
            rendered["name"] = full_name
            return self._expand_non_skill_node(
                rendered,
                depth=depth + 1,
                max_depth=max_depth,
                child_prefix=f"{full_name}/",
            )

        return self._expand_non_skill_node(
            deepcopy(node),
            depth=depth,
            max_depth=max_depth,
            child_prefix=f"{full_name}/",
            rename_to=full_name,
        )

    def _expand_non_skill_node(
        self,
        node: dict[str, Any],
        *,
        depth: int,
        max_depth: int,
        child_prefix: str,
        rename_to: str | None = None,
    ) -> dict[str, Any]:
        expanded = deepcopy(node)
        if rename_to is not None:
            expanded["name"] = rename_to

        children = expanded.get("children", []) or []
        expanded["children"] = [
            self._expand_node_dict(
                child,
                depth=depth,
                max_depth=max_depth,
                prefix=child_prefix,
            )
            for child in children
        ]
        return expanded


def _parse_parameter(raw: dict[str, Any]) -> SkillParameter:
    return SkillParameter(
        name=raw["name"],
        type=raw.get("type", "string"),
        required=raw.get("required", False),
        default=raw.get("default"),
        description=raw.get("description", ""),
    )


def _is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _number_list_error(value: Any, *, expected_len: int | None = None) -> str | None:
    if not isinstance(value, list) or not all(_is_number(item) for item in value):
        return "must be a list of numeric values"
    if expected_len is not None and len(value) != expected_len:
        return f"must contain exactly {expected_len} numeric values"
    return None


def _validate_skill_pose(value: Any) -> str | None:
    if not isinstance(value, dict):
        return f"must be a pose mapping, got {type(value).__name__}"
    if "position_m" not in value:
        return "must include position_m"
    position_error = _number_list_error(value.get("position_m"), expected_len=3)
    if position_error is not None:
        return f"position_m {position_error}"
    has_xyzw = "orientation_xyzw" in value
    has_rpy = "orientation_rpy_deg" in value
    if has_xyzw == has_rpy:
        return "must include exactly one of orientation_xyzw or orientation_rpy_deg"
    if has_xyzw:
        orientation_error = _number_list_error(value.get("orientation_xyzw"), expected_len=4)
        if orientation_error is not None:
            return f"orientation_xyzw {orientation_error}"
    if has_rpy:
        orientation_error = _number_list_error(value.get("orientation_rpy_deg"), expected_len=3)
        if orientation_error is not None:
            return f"orientation_rpy_deg {orientation_error}"
    frame_id = value.get("frame_id")
    if frame_id is not None and not isinstance(frame_id, str):
        return "frame_id must be a string when provided"
    return None


def _validate_skill_param_type(param_type: str, value: Any) -> str | None:
    if param_type == "string":
        if not isinstance(value, str):
            return f"must be a string, got {type(value).__name__}"
        return None
    if param_type == "float":
        if not _is_number(value):
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
    if param_type == "pose":
        return _validate_skill_pose(value)
    if param_type == "float_list":
        return _number_list_error(value)
    if param_type == "string_list":
        if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
            return "must be a list of strings"
        return None
    if param_type in {"dict", "object"}:
        if not isinstance(value, dict):
            return f"must be a mapping, got {type(value).__name__}"
        return None
    return None


def _validate_requires_capabilities(value: Any, path_name: str) -> list[str]:
    if value is None:
        return []
    if not isinstance(value, list) or not all(
        isinstance(capability, str) and capability.strip()
        for capability in value
    ):
        raise ValueError(
            f"{path_name}: requires_capabilities must be a list of non-empty strings"
        )
    unknown_capabilities = sorted(
        capability
        for capability in value
        if capability not in CAPABILITY_REGISTRY
    )
    if unknown_capabilities:
        raise ValueError(
            f"{path_name}: unknown requires_capabilities {unknown_capabilities}"
        )
    return list(value)


def _validate_requires_tools(value: Any, path_name: str) -> list[str]:
    if value is None:
        return []
    if not isinstance(value, list) or not all(
        isinstance(tool_id, str) and tool_id.strip()
        for tool_id in value
    ):
        raise ValueError(
            f"{path_name}: requires_tools must be a list of non-empty strings"
        )
    normalized = [tool_id.strip() for tool_id in value]
    duplicates = sorted({tool_id for tool_id in normalized if normalized.count(tool_id) > 1})
    if duplicates:
        raise ValueError(f"{path_name}: duplicate requires_tools {duplicates}")
    return normalized


def _render_string(template: str, args: dict[str, Any]) -> Any:
    exact_match = PARAM_REF_RE.match(template)
    if exact_match:
        parameter_name = exact_match.group(1)
        if parameter_name not in args:
            raise ValueError(f"Unknown skill parameter reference: '{parameter_name}'")
        return deepcopy(args[parameter_name])

    def repl(match: re.Match[str]) -> str:
        parameter_name = match.group(1)
        if parameter_name not in args:
            raise ValueError(f"Unknown skill parameter reference: '{parameter_name}'")
        return str(args[parameter_name])

    return _INLINE_PARAM_RE.sub(repl, template)


def _render_value(value: Any, args: dict[str, Any]) -> Any:
    if isinstance(value, str):
        return _render_string(value, args)
    if isinstance(value, list):
        return [_render_value(item, args) for item in value]
    if isinstance(value, dict):
        return {key: _render_value(item, args) for key, item in value.items()}
    return deepcopy(value)


def load_skill(path: Path) -> SkillDefinition:
    """Load a single skill YAML file. Raises ValueError on schema issues."""
    with open(path, encoding="utf-8") as handle:
        data = yaml.safe_load(handle)

    if not isinstance(data, dict):
        raise ValueError(f"{path.name}: skill file must be a YAML mapping")

    api_version = data.get("api_version", "")
    if api_version != _EXPECTED_API_VERSION:
        raise ValueError(
            f"{path.name}: expected api_version={_EXPECTED_API_VERSION}, got {api_version}"
        )

    skill_id = data.get("skill_id") or data.get("id")
    if not skill_id:
        raise ValueError(f"{path.name}: missing required field 'skill_id'")

    body = data.get("body")
    if body is None:
        body = data.get("expands_to")
    if body is None:
        raise ValueError(f"{path.name}: missing required field 'body'")
    if not isinstance(body, dict):
        raise ValueError(f"{path.name}: body must be a mapping")

    parameters = [_parse_parameter(p) for p in data.get("parameters", [])]
    requires_capabilities = _validate_requires_capabilities(
        data.get("requires_capabilities", []),
        path.name,
    )
    requires_tools = _validate_requires_tools(
        data.get("requires_tools", []),
        path.name,
    )

    return SkillDefinition(
        skill_id=skill_id,
        version=data.get("version", "0.0.0"),
        name=data.get("name", skill_id),
        description=data.get("description", ""),
        compatible_profiles=list(data.get("compatible_profiles", [])),
        requires=list(data.get("requires", [])),
        provides=list(data.get("provides", [])),
        modifies=list(data.get("modifies", [])),
        requires_capabilities=requires_capabilities,
        requires_tools=requires_tools,
        parameters=parameters,
        body=body,
        example=dict(data.get("example", {})),
        validated_by=data.get("validated_by", ""),
    )


def _validate_skill_node(
    node: dict[str, Any],
    *,
    catalog: NodeCatalog,
    skills: SkillCatalog,
    path: str,
    depth: int,
) -> None:
    if not isinstance(node, dict):
        raise ValueError(f"{path}: node must be a mapping")
    if "id" not in node:
        raise ValueError(f"{path}: missing required field 'id'")
    if "name" not in node:
        raise ValueError(f"{path}: missing required field 'name'")

    node_id = node["id"]
    params = node.get("params", {}) or {}
    children = node.get("children", []) or []
    if not isinstance(params, dict):
        raise ValueError(f"{path}.params must be a mapping")
    if not isinstance(children, list):
        raise ValueError(f"{path}.children must be a list")

    if skills.contains(node_id):
        if depth >= _MAX_NESTING_DEPTH:
            raise ValueError(
                f"{path}: nested skill '{node_id}' exceeds maximum depth {_MAX_NESTING_DEPTH}"
            )
        nested = skills.get(node_id)
        assert nested is not None
        declared_params = {parameter.name for parameter in nested.parameters}
        for param_name in params:
            if param_name not in declared_params:
                raise ValueError(
                    f"{path}.params.{param_name}: unknown parameter for skill '{node_id}'"
                )
        if children:
            raise ValueError(
                f"{path}: skill invocations may not define explicit children"
            )
        _validate_skill_node(
            nested.body,
            catalog=catalog,
            skills=skills,
            path=f"{path}.body",
            depth=depth + 1,
        )
        return

    try:
        entry = catalog.get(node_id)
    except KeyError as exc:
        raise ValueError(f"{path}: unknown catalog entry '{node_id}'") from exc

    declared_params = {parameter.name for parameter in entry.parameters}
    for param_name in params:
        if param_name not in declared_params:
            raise ValueError(
                f"{path}.params.{param_name}: unknown parameter for node '{node_id}'"
            )

    for index, child in enumerate(children):
        _validate_skill_node(
            child,
            catalog=catalog,
            skills=skills,
            path=f"{path}.children[{index}]",
            depth=depth,
        )


def _validate_skill_catalog(
    skill_catalog: SkillCatalog,
    *,
    catalog: NodeCatalog,
) -> None:
    for skill in skill_catalog.skills:
        _validate_skill_node(
            skill.body,
            catalog=catalog,
            skills=skill_catalog,
            path=f"skills.{skill.skill_id}.body",
            depth=0,
        )
        skill_catalog.expand_node_dict(
            {
                "id": skill.skill_id,
                "name": skill.skill_id,
                "params": skill.example,
            }
        )


def load_skill_catalog(
    skills_dir: str | Path,
    *,
    catalog: NodeCatalog | None = None,
    strict: bool = False,
) -> SkillCatalog:
    """Load all ``*.skill.yaml`` files from a directory."""
    skills_path = Path(skills_dir)
    if not skills_path.is_dir():
        logger.warning("Skills directory not found: %s", skills_path)
        return SkillCatalog()

    skills: list[SkillDefinition] = []
    by_id: dict[str, SkillDefinition] = {}

    for path in sorted(skills_path.glob("*.skill.yaml")):
        try:
            skill = load_skill(path)
            if skill.skill_id in by_id:
                raise ValueError(f"duplicate skill ID '{skill.skill_id}'")
            skills.append(skill)
            by_id[skill.skill_id] = skill
            logger.debug("Loaded skill: %s (%s)", skill.skill_id, skill.version)
        except Exception as exc:
            if strict:
                raise ValueError(f"Failed to load skill {path.name}: {exc}") from exc
            logger.warning("Skipping malformed skill %s: %s", path.name, exc)

    skill_catalog = SkillCatalog(skills=skills, _by_id=by_id)

    if catalog is not None and skill_catalog.skills:
        try:
            _validate_skill_catalog(skill_catalog, catalog=catalog)
        except Exception as exc:
            if strict:
                raise ValueError(f"Invalid skill catalog: {exc}") from exc
            logger.warning("Skill catalog validation failed: %s", exc)

    logger.info("Loaded %d skill(s) from %s", len(skill_catalog.skills), skills_path)
    return skill_catalog


def default_skills_dir() -> Path:
    """Return the default skills directory."""
    return Path(__file__).parent.parent / "resources" / "skills"
