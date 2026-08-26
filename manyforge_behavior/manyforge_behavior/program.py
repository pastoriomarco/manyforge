"""Strict v1 Program authoring and bounded legacy-neutral tree helpers.

The loader validates portable authored meaning against one independently
verified v2 catalog. It does not restore the former v0 singleton schema,
runtime factories, deployment-wide capability union, or binding authority.
"""

from __future__ import annotations

import hashlib
import json
import math
import re
from copy import deepcopy
from dataclasses import dataclass, field
from pathlib import Path
from types import MappingProxyType
from typing import Any, Final, Mapping, NoReturn

from manyforge_behavior.canonical_artifact import (
    StrictArtifactError,
    canonical_json_bytes,
    load_strict_json_yaml,
    verify_content_identity,
)
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.catalog_schema import CatalogSchema, validate_value
from manyforge_behavior.skill_loader import SkillCatalog

_MAX_TREE_DEPTH: Final = 256
_MAX_TREE_NODES: Final = 100_000
_MAX_CHILDREN_PER_NODE: Final = 10_000
_MAX_RECOVERY_RETRIES: Final = 1_000_000
_MAX_IDENTIFIER_CHARS: Final = 256
_MAX_DESCRIPTION_CHARS: Final = 4096
_MAX_VALUE_DEPTH: Final = 32
_MAX_VALUE_NODES: Final = 250_000
_MAX_MAPPING_ITEMS: Final = 4096
_MAX_SEQUENCE_ITEMS: Final = 10_000
_MAX_VALUE_STRING_CHARS: Final = 1_000_000
_MAX_DOCUMENT_TEXT_CHARS: Final = 8 * 1024 * 1024
_MAX_INTEGER_BITS: Final = 256
_MAX_CANONICAL_BYTES: Final = 96 * 1024 * 1024
_PROGRAM_API_VERSION: Final = "manyforge.program.v1"
_PROGRAM_PROFILE: Final = "composable-cell-v1"
_CATALOG_API_VERSION: Final = "manyforge.node_catalog.v2"
_IDENTIFIER_RE: Final = re.compile(r"^[a-z][a-z0-9_.-]*$")
_MAX_PROGRAM_ROLES: Final = 4096
_MAX_PROGRAM_DECLARATIONS: Final = 100_000


class ProgramValidationError(ValueError):
    """A Program artifact or retained tree-document value is malformed."""


class _TraversalBudget:
    """Shared bounds for one complete authoring operation."""

    def __init__(self) -> None:
        self.tree_nodes = 0
        self.value_nodes = 0
        self.text_chars = 0

    def consume_tree_node(self) -> None:
        self.tree_nodes += 1
        if self.tree_nodes > _MAX_TREE_NODES:
            raise ProgramValidationError("tree node count exceeds bounded maximum")

    def consume_value_node(self, *, path: str) -> None:
        self.value_nodes += 1
        if self.value_nodes > _MAX_VALUE_NODES:
            raise ProgramValidationError(
                f"{path} exceeds the bounded document node count"
            )

    def consume_text(self, value: str, *, path: str, limit: int) -> str:
        if len(value) > limit:
            raise ProgramValidationError(f"{path} exceeds its text bound")
        self.text_chars += len(value)
        if self.text_chars > _MAX_DOCUMENT_TEXT_CHARS:
            raise ProgramValidationError("tree document exceeds its total text bound")
        return value


def _bounded_text(
    value: Any,
    *,
    path: str,
    budget: _TraversalBudget,
    limit: int = _MAX_IDENTIFIER_CHARS,
    allow_empty: bool = False,
) -> str:
    if type(value) is not str or (not allow_empty and not value):
        qualifier = "a string" if allow_empty else "a non-empty string"
        raise ProgramValidationError(f"{path} must be {qualifier}")
    return budget.consume_text(value, path=path, limit=limit)


def _validate_path_label(path: Any) -> str:
    if type(path) is not str or not path or len(path) > _MAX_DESCRIPTION_CHARS:
        raise ProgramValidationError("tree path must be a bounded non-empty string")
    return path


def _validate_closed_mapping(
    value: Any,
    *,
    allowed: frozenset[str],
    path: str,
) -> dict[str, Any]:
    """Accept only a built-in dict and inspect no unsafe key representation."""

    if type(value) is not dict:
        raise ProgramValidationError(f"{path} must be a built-in mapping")
    if len(value) > len(allowed):
        raise ProgramValidationError(f"{path} contains too many fields")
    for key in value:
        if type(key) is not str:
            raise ProgramValidationError(f"{path} contains a non-string field name")
        if not key or len(key) > _MAX_IDENTIFIER_CHARS:
            raise ProgramValidationError(f"{path} contains an invalid field name")
    unknown = sorted(key for key in value if key not in allowed)
    if unknown:
        raise ProgramValidationError(f"{path} contains unknown fields: {unknown}")
    return value


def _bounded_json_value(
    value: Any,
    *,
    path: str,
    depth: int,
    budget: _TraversalBudget,
    ancestors: set[int],
    copy_value: bool,
) -> Any:
    """Validate one exact JSON-compatible value and optionally copy it."""

    if depth > _MAX_VALUE_DEPTH:
        raise ProgramValidationError(f"{path} exceeds the bounded value depth")
    budget.consume_value_node(path=path)

    if value is None or type(value) is bool:
        return value
    if type(value) is int:
        if value.bit_length() > _MAX_INTEGER_BITS:
            raise ProgramValidationError(f"{path} integer exceeds the numeric bound")
        return value
    if type(value) is float:
        if not math.isfinite(value):
            raise ProgramValidationError(f"{path} must be finite")
        return value
    if type(value) is str:
        return budget.consume_text(
            value,
            path=path,
            limit=_MAX_VALUE_STRING_CHARS,
        )

    if type(value) is dict:
        if len(value) > _MAX_MAPPING_ITEMS:
            raise ProgramValidationError(f"{path} exceeds the mapping item bound")
        identity = id(value)
        if identity in ancestors:
            raise ProgramValidationError(f"{path} contains a container cycle")
        ancestors.add(identity)
        copied: dict[str, Any] | None = {} if copy_value else None
        try:
            for key, item in value.items():
                if type(key) is not str:
                    raise ProgramValidationError(
                        f"{path} contains a non-string mapping key"
                    )
                normalized_key = budget.consume_text(
                    key,
                    path=f"{path} mapping key",
                    limit=_MAX_IDENTIFIER_CHARS,
                )
                normalized_item = _bounded_json_value(
                    item,
                    path=f"{path}.{normalized_key}",
                    depth=depth + 1,
                    budget=budget,
                    ancestors=ancestors,
                    copy_value=copy_value,
                )
                if copied is not None:
                    copied[normalized_key] = normalized_item
        finally:
            ancestors.remove(identity)
        return copied if copied is not None else value

    if type(value) in {list, tuple}:
        if len(value) > _MAX_SEQUENCE_ITEMS:
            raise ProgramValidationError(f"{path} exceeds the sequence item bound")
        identity = id(value)
        if identity in ancestors:
            raise ProgramValidationError(f"{path} contains a container cycle")
        ancestors.add(identity)
        copied_items: list[Any] | None = [] if copy_value else None
        try:
            for index, item in enumerate(value):
                normalized_item = _bounded_json_value(
                    item,
                    path=f"{path}[{index}]",
                    depth=depth + 1,
                    budget=budget,
                    ancestors=ancestors,
                    copy_value=copy_value,
                )
                if copied_items is not None:
                    copied_items.append(normalized_item)
        finally:
            ancestors.remove(identity)
        return copied_items if copied_items is not None else value

    raise ProgramValidationError(
        f"{path} contains a value outside the exact JSON-compatible subset"
    )


def _bounded_json_mapping(
    value: Any,
    *,
    path: str,
    budget: _TraversalBudget,
    copy_value: bool,
    ancestors: set[int] | None = None,
) -> dict[str, Any]:
    if type(value) is not dict:
        raise ProgramValidationError(f"{path} must be a built-in mapping")
    normalized = _bounded_json_value(
        value,
        path=path,
        depth=0,
        budget=budget,
        ancestors=ancestors if ancestors is not None else set(),
        copy_value=copy_value,
    )
    if type(normalized) is not dict:
        raise ProgramValidationError(f"{path} must be a built-in mapping")
    return normalized


@dataclass(frozen=True)
class BlackboardKey:
    """Transport-neutral key declaration used by intervention validation."""

    id: str
    type: str
    key: str
    description: str = ""

    def __post_init__(self) -> None:
        budget = _TraversalBudget()
        _bounded_text(self.id, path="blackboard key id", budget=budget)
        _bounded_text(self.type, path="blackboard key type", budget=budget)
        _bounded_text(self.key, path="blackboard key name", budget=budget)
        _bounded_text(
            self.description,
            path="blackboard key description",
            budget=budget,
            limit=_MAX_DESCRIPTION_CHARS,
            allow_empty=True,
        )


@dataclass
class TreeNode:
    """One bounded authoring-tree value; it is not an executable node owner."""

    id: str
    name: str
    params: dict[str, Any] = field(default_factory=dict)
    children: list["TreeNode"] = field(default_factory=list)
    modification_policy: dict[str, str] = field(default_factory=dict)
    recovery_policy: dict[str, Any] = field(default_factory=dict)
    operator_stop_policy: str = "inherit"

    def __post_init__(self) -> None:
        budget = _TraversalBudget()
        self.id = _bounded_text(self.id, path="tree.id", budget=budget)
        self.name = _bounded_text(self.name, path="tree.name", budget=budget)
        self.params = _bounded_json_mapping(
            self.params,
            path="tree.params",
            budget=budget,
            copy_value=True,
        )
        if type(self.children) is not list:
            raise ProgramValidationError("tree.children must be a built-in list")
        if len(self.children) > _MAX_CHILDREN_PER_NODE:
            raise ProgramValidationError("tree.children exceeds the item bound")
        if any(type(child) is not TreeNode for child in self.children):
            raise ProgramValidationError("tree.children must contain TreeNode values")
        self.children = list(self.children)
        self.modification_policy = _parse_modification_policy(
            self.modification_policy,
            path="tree",
        )
        self.recovery_policy = _parse_recovery_policy(
            self.recovery_policy,
            path="tree",
        )
        self.operator_stop_policy = _parse_operator_stop_policy(
            self.operator_stop_policy,
            path="tree",
        )
        _validate_tree_graph(self)

    def to_dict(self) -> dict[str, Any]:
        """Return a detached bounded document or reject a mutated/cyclic tree."""

        return _tree_document(self, copy_values=True)


def compute_program_tree_hash(tree: TreeNode) -> str:
    """Hash one normalized tree value; this is not Program artifact identity."""
    document = _tree_document(tree, copy_values=True)
    try:
        canonical = json.dumps(
            document,
            allow_nan=False,
            ensure_ascii=True,
            sort_keys=True,
            separators=(",", ":"),
        )
    except (TypeError, ValueError, RecursionError) as exc:
        raise ProgramValidationError("tree could not be canonically encoded") from exc
    canonical_bytes = canonical.encode("utf-8")
    if len(canonical_bytes) > _MAX_CANONICAL_BYTES:
        raise ProgramValidationError("canonical tree exceeds the byte bound")
    return hashlib.sha256(canonical_bytes).hexdigest()


_MODIFICATION_POLICY_ALLOWED_VALUES: Final = {
    "runtime_override": frozenset({"forbidden", "human_approval", "llm_allowed"}),
    "persistent_edit": frozenset({"forbidden", "human_approval", "llm_allowed"}),
    "intervention_delegate": frozenset(
        {"forbidden", "human_only", "llm_allowed", "program_only"}
    ),
}
_MODIFICATION_POLICY_FIELDS: Final = frozenset(_MODIFICATION_POLICY_ALLOWED_VALUES)
OPERATOR_STOP_POLICY_VALUES: Final = frozenset({"inherit", "hold", "continue"})
_RECOVERY_POLICY_ALLOWED_ACTIONS: Final = frozenset(
    {"runtime_override", "intervention"}
)
_RECOVERY_POLICY_PERSISTENCE_CEILINGS: Final = frozenset(
    {"session_only", "runtime_override", "draft_patch"}
)
_RECOVERY_POLICY_FIELDS: Final = frozenset(
    {
        "allow_intervention",
        "allowed_recovery_actions",
        "max_retries",
        "allowed_skills",
        "persistence_ceiling",
    }
)
_TREE_FIELDS: Final = frozenset(
    {
        "id",
        "name",
        "params",
        "children",
        "modification_policy",
        "recovery_policy",
        "operator_stop_policy",
    }
)


def _parse_operator_stop_policy(raw_policy: Any, *, path: str) -> str:
    if raw_policy is None:
        return "inherit"
    if type(raw_policy) is not str or raw_policy not in OPERATOR_STOP_POLICY_VALUES:
        allowed = ", ".join(sorted(OPERATOR_STOP_POLICY_VALUES))
        raise ProgramValidationError(
            f"{path}.operator_stop_policy must be one of: {allowed}"
        )
    return raw_policy


def _parse_modification_policy(raw_policy: Any, *, path: str) -> dict[str, str]:
    if raw_policy is None:
        return {}
    policy = _validate_closed_mapping(
        raw_policy,
        allowed=_MODIFICATION_POLICY_FIELDS,
        path=f"{path}.modification_policy",
    )
    parsed: dict[str, str] = {}
    for key, value in policy.items():
        allowed = _MODIFICATION_POLICY_ALLOWED_VALUES.get(key)
        if type(value) is not str or allowed is None or value not in allowed:
            raise ProgramValidationError(
                f"{path}.modification_policy.{key} has an unsupported value"
            )
        parsed[key] = value
    return parsed


def _parse_recovery_policy(raw_policy: Any, *, path: str) -> dict[str, Any]:
    if raw_policy is None:
        return {}
    policy = _validate_closed_mapping(
        raw_policy,
        allowed=_RECOVERY_POLICY_FIELDS,
        path=f"{path}.recovery_policy",
    )
    parsed: dict[str, Any] = {}
    if "allow_intervention" in policy:
        value = policy["allow_intervention"]
        if type(value) is not bool:
            raise ProgramValidationError(
                f"{path}.recovery_policy.allow_intervention must be boolean"
            )
        parsed["allow_intervention"] = value
    if "allowed_recovery_actions" in policy:
        actions = policy["allowed_recovery_actions"]
        if (
            type(actions) is not list
            or len(actions) > 32
            or any(
                type(action) is not str
                or action not in _RECOVERY_POLICY_ALLOWED_ACTIONS
                for action in actions
            )
            or len(actions) != len(set(actions))
        ):
            raise ProgramValidationError(
                f"{path}.recovery_policy.allowed_recovery_actions is invalid"
            )
        parsed["allowed_recovery_actions"] = list(actions)
    if "max_retries" in policy:
        retries = policy["max_retries"]
        if type(retries) is not int or retries < 0 or retries > _MAX_RECOVERY_RETRIES:
            raise ProgramValidationError(
                f"{path}.recovery_policy.max_retries must be bounded"
            )
        parsed["max_retries"] = retries
    if "allowed_skills" in policy:
        skills = policy["allowed_skills"]
        if (
            type(skills) is not list
            or len(skills) > 256
            or any(
                type(skill) is not str
                or not skill
                or len(skill) > _MAX_IDENTIFIER_CHARS
                for skill in skills
            )
            or len(skills) != len(set(skills))
        ):
            raise ProgramValidationError(
                f"{path}.recovery_policy.allowed_skills is invalid"
            )
        parsed["allowed_skills"] = list(skills)
    if "persistence_ceiling" in policy:
        ceiling = policy["persistence_ceiling"]
        if (
            type(ceiling) is not str
            or ceiling not in _RECOVERY_POLICY_PERSISTENCE_CEILINGS
        ):
            raise ProgramValidationError(
                f"{path}.recovery_policy.persistence_ceiling is invalid"
            )
        parsed["persistence_ceiling"] = ceiling
    return parsed


_CANONICAL_MODIFICATION_POLICY_FIELDS: Final = frozenset(
    {
        "runtime_override",
        "persistent_edit",
        "intervention_delegate",
    }
)
_CANONICAL_RECOVERY_POLICY_FIELDS: Final = frozenset(
    {
        "allow_intervention",
        "allowed_recovery_actions",
        "max_retries",
        "allowed_skills",
        "persistence_ceiling",
    }
)
_CANONICAL_RECOVERY_ACTIONS: Final = frozenset(
    {"intervention", "runtime_override"}
)
_CANONICAL_RECOVERY_PERSISTENCE_CEILINGS: Final = frozenset(
    {"session_only", "runtime_override", "draft_patch"}
)
_CANONICAL_DEFAULT_MODIFICATION_POLICY: Final = {
    "runtime_override": "forbidden",
    "persistent_edit": "forbidden",
    "intervention_delegate": "forbidden",
}
_CANONICAL_DEFAULT_RECOVERY_POLICY: Final = {
    "allow_intervention": False,
    "allowed_recovery_actions": [],
    "max_retries": 0,
    "allowed_skills": [],
    "persistence_ceiling": "session_only",
}


def _parse_canonical_modification_policy(
    raw_policy: Any,
    *,
    path: str,
) -> dict[str, str]:
    if type(raw_policy) is not dict:
        raise ProgramValidationError(
            f"{path}.modification_policy must be a complete object"
        )
    policy = _validate_closed_mapping(
        raw_policy,
        allowed=_CANONICAL_MODIFICATION_POLICY_FIELDS,
        path=f"{path}.modification_policy",
    )
    if set(policy) != _CANONICAL_MODIFICATION_POLICY_FIELDS:
        raise ProgramValidationError(
            f"{path}.modification_policy must contain exactly "
            f"{sorted(_CANONICAL_MODIFICATION_POLICY_FIELDS)}"
        )
    result: dict[str, str] = {}
    for key, allowed in _MODIFICATION_POLICY_ALLOWED_VALUES.items():
        value = policy[key]
        if type(value) is not str or value not in allowed:
            raise ProgramValidationError(
                f"{path}.modification_policy.{key} has an unsupported value"
            )
        result[key] = value
    return result


def _parse_canonical_recovery_policy(
    raw_policy: Any,
    *,
    path: str,
) -> dict[str, Any]:
    if type(raw_policy) is not dict:
        raise ProgramValidationError(
            f"{path}.recovery_policy must be a complete object"
        )
    policy = _validate_closed_mapping(
        raw_policy,
        allowed=_CANONICAL_RECOVERY_POLICY_FIELDS,
        path=f"{path}.recovery_policy",
    )
    if set(policy) != _CANONICAL_RECOVERY_POLICY_FIELDS:
        raise ProgramValidationError(
            f"{path}.recovery_policy must contain exactly "
            f"{sorted(_CANONICAL_RECOVERY_POLICY_FIELDS)}"
        )
    allow_intervention = policy["allow_intervention"]
    if type(allow_intervention) is not bool:
        raise ProgramValidationError(
            f"{path}.recovery_policy.allow_intervention must be boolean"
        )

    actions = policy["allowed_recovery_actions"]
    if (
        type(actions) is not list
        or len(actions) > len(_CANONICAL_RECOVERY_ACTIONS)
        or any(
            type(action) is not str
            or action not in _CANONICAL_RECOVERY_ACTIONS
            for action in actions
        )
        or len(actions) != len(set(actions))
        or actions != sorted(actions)
    ):
        raise ProgramValidationError(
            f"{path}.recovery_policy.allowed_recovery_actions is invalid"
        )

    max_retries = policy["max_retries"]
    if type(max_retries) is not int or not 0 <= max_retries <= 255:
        raise ProgramValidationError(
            f"{path}.recovery_policy.max_retries must be an integer in 0..255"
        )

    skills = policy["allowed_skills"]
    if type(skills) is not list or len(skills) > 256:
        raise ProgramValidationError(
            f"{path}.recovery_policy.allowed_skills is invalid"
        )
    normalized_skills: list[str] = []
    for index, skill in enumerate(skills):
        skill_path = f"{path}.recovery_policy.allowed_skills[{index}]"
        skill_id = _program_identifier(skill, path=skill_path)
        try:
            skill_bytes = skill_id.encode("ascii")
        except UnicodeEncodeError:
            raise ProgramValidationError(
                f"{skill_path} must be an ASCII portable lower-case ID"
            ) from None
        if len(skill_bytes) > 128:
            raise ProgramValidationError(
                f"{skill_path} exceeds the 128-byte skill ID bound"
            )
        normalized_skills.append(skill_id)
    if (
        normalized_skills != sorted(normalized_skills)
        or len(normalized_skills) != len(set(normalized_skills))
    ):
        raise ProgramValidationError(
            f"{path}.recovery_policy.allowed_skills is not canonical-sorted"
        )

    persistence_ceiling = policy["persistence_ceiling"]
    if (
        type(persistence_ceiling) is not str
        or persistence_ceiling not in _CANONICAL_RECOVERY_PERSISTENCE_CEILINGS
    ):
        raise ProgramValidationError(
            f"{path}.recovery_policy.persistence_ceiling is invalid"
        )
    return {
        "allow_intervention": allow_intervention,
        "allowed_recovery_actions": list(actions),
        "max_retries": max_retries,
        "allowed_skills": normalized_skills,
        "persistence_ceiling": persistence_ceiling,
    }


def _normalize_canonical_node_policies(
    node: Mapping[str, Any],
    *,
    path: str,
) -> tuple[dict[str, str] | None, dict[str, Any] | None, str | None]:
    if "modification_policy" in node:
        modification = _parse_canonical_modification_policy(
            node["modification_policy"], path=path
        )
    else:
        modification = dict(_CANONICAL_DEFAULT_MODIFICATION_POLICY)
    if "recovery_policy" in node:
        recovery = _parse_canonical_recovery_policy(
            node["recovery_policy"], path=path
        )
    else:
        recovery = dict(_CANONICAL_DEFAULT_RECOVERY_POLICY)
    if "operator_stop_policy" in node:
        operator_stop = node["operator_stop_policy"]
        if type(operator_stop) is not str or operator_stop not in OPERATOR_STOP_POLICY_VALUES:
            raise ProgramValidationError(
                f"{path}.operator_stop_policy must be inherit, hold, or continue"
            )
    else:
        operator_stop = "inherit"

    actions = set(recovery["allowed_recovery_actions"])
    if recovery["allow_intervention"] != ("intervention" in actions):
        raise ProgramValidationError(
            f"{path}.recovery_policy.allow_intervention must match "
            "the intervention action"
        )
    if recovery["allowed_skills"] and "intervention" not in actions:
        raise ProgramValidationError(
            f"{path}.recovery_policy.allowed_skills requires intervention"
        )
    if (
        "runtime_override" in actions
        and modification["runtime_override"] == "forbidden"
    ):
        raise ProgramValidationError(
            f"{path}.recovery_policy.runtime_override requires a permitted "
            "modification_policy.runtime_override"
        )
    if (
        recovery["persistence_ceiling"] == "runtime_override"
        and modification["runtime_override"] == "forbidden"
    ):
        raise ProgramValidationError(
            f"{path}.recovery_policy.runtime_override persistence requires "
            "a permitted modification_policy.runtime_override"
        )
    if (
        recovery["persistence_ceiling"] == "draft_patch"
        and modification["persistent_edit"] == "forbidden"
    ):
        raise ProgramValidationError(
            f"{path}.recovery_policy.draft_patch persistence requires a "
            "permitted modification_policy.persistent_edit"
        )
    if recovery != _CANONICAL_DEFAULT_RECOVERY_POLICY and not actions:
        raise ProgramValidationError(
            f"{path}.recovery_policy non-default values require an action"
        )

    normalized_modification = (
        None
        if modification == _CANONICAL_DEFAULT_MODIFICATION_POLICY
        else modification
    )
    normalized_recovery = (
        None
        if recovery == _CANONICAL_DEFAULT_RECOVERY_POLICY
        else recovery
    )
    normalized_stop = None if operator_stop == "inherit" else operator_stop
    return normalized_modification, normalized_recovery, normalized_stop


def _remove_explicit_default_policy_spelling(
    root: dict[str, Any],
) -> dict[str, Any]:
    """Remove only the exact source spellings permitted by C1 migration."""

    def is_default_modification(value: Any) -> bool:
        return (
            type(value) is dict
            and set(value) == _CANONICAL_MODIFICATION_POLICY_FIELDS
            and all(
                type(value[field]) is str
                and value[field] == _CANONICAL_DEFAULT_MODIFICATION_POLICY[field]
                for field in _CANONICAL_MODIFICATION_POLICY_FIELDS
            )
        )

    def is_default_recovery(value: Any) -> bool:
        return (
            type(value) is dict
            and set(value) == _CANONICAL_RECOVERY_POLICY_FIELDS
            and type(value["allow_intervention"]) is bool
            and value["allow_intervention"] is False
            and type(value["allowed_recovery_actions"]) is list
            and value["allowed_recovery_actions"] == []
            and type(value["max_retries"]) is int
            and value["max_retries"] == 0
            and type(value["allowed_skills"]) is list
            and value["allowed_skills"] == []
            and type(value["persistence_ceiling"]) is str
            and value["persistence_ceiling"] == "session_only"
        )

    candidate = dict(root)
    remaining_nodes = [_MAX_TREE_NODES]
    ancestors: set[int] = set()

    def visit(node: Any, *, depth: int) -> Any:
        if type(node) is not dict:
            return node
        if depth > _MAX_TREE_DEPTH:
            raise ProgramValidationError("tree depth exceeds bounded maximum")
        identity = id(node)
        if identity in ancestors:
            raise ProgramValidationError("tree contains a cycle")
        remaining_nodes[0] -= 1
        if remaining_nodes[0] < 0:
            raise ProgramValidationError("tree node count exceeds bounded maximum")

        migrated = dict(node)
        if is_default_modification(migrated.get("modification_policy")):
            del migrated["modification_policy"]
        if is_default_recovery(migrated.get("recovery_policy")):
            del migrated["recovery_policy"]
        if type(migrated.get("operator_stop_policy")) is str and migrated[
            "operator_stop_policy"
        ] == "inherit":
            del migrated["operator_stop_policy"]
        children = migrated.get("children")
        if type(children) is list:
            if len(children) > _MAX_CHILDREN_PER_NODE:
                raise ProgramValidationError(
                    "tree child count exceeds bounded maximum"
                )
            ancestors.add(identity)
            try:
                migrated["children"] = [
                    visit(child, depth=depth + 1) for child in children
                ]
            finally:
                ancestors.remove(identity)
        return migrated

    candidate["tree"] = visit(candidate.get("tree"), depth=0)
    return candidate


def _select_identity_verified_program_root(
    root: dict[str, Any],
    *,
    source: str,
) -> dict[str, Any]:
    """Verify identity before semantic validation, with one bounded migration."""

    # Preserve the established shape diagnostics for documents that do not
    # contain a usable identity field at all.  A stale, otherwise well-shaped
    # identity must not be hidden by authored semantic errors.
    if (
        root.get("api_version") != _PROGRAM_API_VERSION
        or type(root.get("program_revision")) is not str
        or re.fullmatch(r"sha256:[0-9a-f]{64}", root["program_revision"]) is None
    ):
        return root
    try:
        verify_content_identity(
            root,
            own_field="program_revision",
            identity_domain="program-revision",
            api_version=_PROGRAM_API_VERSION,
            label=source,
        )
        return root
    except StrictArtifactError as raw_identity_error:
        if not str(raw_identity_error).startswith(
            f"{source}.program_revision: content identity mismatch;"
        ):
            _program_error(f"{source}.program_revision", str(raw_identity_error))
        try:
            candidate = _remove_explicit_default_policy_spelling(root)
        except ProgramValidationError:
            _program_error(f"{source}.program_revision", str(raw_identity_error))
        try:
            verify_content_identity(
                candidate,
                own_field="program_revision",
                identity_domain="program-revision",
                api_version=_PROGRAM_API_VERSION,
                label=source,
            )
        except StrictArtifactError as exc:
            _program_error(f"{source}.program_revision", str(exc))
        return candidate


def _traverse_tree(
    node: Any,
    *,
    path: str,
    depth: int,
    budget: _TraversalBudget,
    ancestors: set[int],
    build_document: bool,
) -> dict[str, Any] | None:
    if type(node) is not TreeNode:
        raise ProgramValidationError(f"{path} must be a TreeNode")
    if depth > _MAX_TREE_DEPTH:
        raise ProgramValidationError("tree depth exceeds bounded maximum")
    identity = id(node)
    if identity in ancestors:
        raise ProgramValidationError(f"{path} contains a tree cycle")
    budget.consume_tree_node()
    ancestors.add(identity)
    try:
        node_id = _bounded_text(node.id, path=f"{path}.id", budget=budget)
        name = _bounded_text(node.name, path=f"{path}.name", budget=budget)
        params = _bounded_json_mapping(
            node.params,
            path=f"{path}.params",
            budget=budget,
            copy_value=build_document,
            ancestors=ancestors,
        )
        modification_policy = _parse_modification_policy(
            node.modification_policy,
            path=path,
        )
        if modification_policy:
            modification_policy = _bounded_json_mapping(
                modification_policy,
                path=f"{path}.modification_policy",
                budget=budget,
                copy_value=build_document,
                ancestors=ancestors,
            )
        recovery_policy = _parse_recovery_policy(
            node.recovery_policy,
            path=path,
        )
        if recovery_policy:
            recovery_policy = _bounded_json_mapping(
                recovery_policy,
                path=f"{path}.recovery_policy",
                budget=budget,
                copy_value=build_document,
                ancestors=ancestors,
            )
        operator_stop_policy = _parse_operator_stop_policy(
            node.operator_stop_policy,
            path=path,
        )
        operator_stop_policy = _bounded_text(
            operator_stop_policy,
            path=f"{path}.operator_stop_policy",
            budget=budget,
        )
        if type(node.children) is not list:
            raise ProgramValidationError(f"{path}.children must be a built-in list")
        if len(node.children) > _MAX_CHILDREN_PER_NODE:
            raise ProgramValidationError(f"{path}.children exceeds the item bound")

        document: dict[str, Any] | None = (
            {"id": node_id, "name": name} if build_document else None
        )
        if document is not None:
            if params:
                document["params"] = params
            if operator_stop_policy != "inherit":
                document["operator_stop_policy"] = operator_stop_policy
            if modification_policy:
                document["modification_policy"] = modification_policy
            if recovery_policy:
                document["recovery_policy"] = recovery_policy

        child_documents: list[dict[str, Any]] | None = (
            [] if build_document and node.children else None
        )
        for index, child in enumerate(node.children):
            child_document = _traverse_tree(
                child,
                path=f"{path}.children[{index}]",
                depth=depth + 1,
                budget=budget,
                ancestors=ancestors,
                build_document=build_document,
            )
            if child_documents is not None:
                if child_document is None:
                    raise ProgramValidationError("tree serialization failed closed")
                child_documents.append(child_document)
        if document is not None and child_documents is not None:
            document["children"] = child_documents
        return document
    finally:
        ancestors.remove(identity)


def _validate_tree_graph(node: Any) -> None:
    _traverse_tree(
        node,
        path="tree",
        depth=0,
        budget=_TraversalBudget(),
        ancestors=set(),
        build_document=False,
    )


def _tree_document(node: Any, *, copy_values: bool) -> dict[str, Any]:
    if not copy_values:
        raise ProgramValidationError("tree document copies must be explicit")
    document = _traverse_tree(
        node,
        path="tree",
        depth=0,
        budget=_TraversalBudget(),
        ancestors=set(),
        build_document=True,
    )
    if document is None:
        raise ProgramValidationError("tree serialization failed closed")
    return document


def tree_node_from_dict(
    data: Mapping[str, Any],
    *,
    path: str = "tree",
) -> TreeNode:
    """Parse one closed bounded tree document without catalog or file access."""
    root_path = _validate_path_label(path)
    budget = _TraversalBudget()
    ancestors: set[int] = set()

    def parse(value: Any, current_path: str, depth: int) -> TreeNode:
        if type(value) is not dict:
            raise ProgramValidationError(f"{current_path} must be a built-in mapping")
        if depth > _MAX_TREE_DEPTH:
            raise ProgramValidationError("tree depth exceeds bounded maximum")
        identity = id(value)
        if identity in ancestors:
            raise ProgramValidationError(f"{current_path} contains a tree cycle")
        budget.consume_tree_node()
        ancestors.add(identity)
        try:
            document = _validate_closed_mapping(
                value,
                allowed=_TREE_FIELDS,
                path=current_path,
            )
            node_id = _bounded_text(
                document.get("id"),
                path=f"{current_path}.id",
                budget=budget,
            )
            name = _bounded_text(
                document.get("name", node_id),
                path=f"{current_path}.name",
                budget=budget,
            )
            raw_params = document.get("params", {})
            if raw_params is None:
                raw_params = {}
            params = _bounded_json_mapping(
                raw_params,
                path=f"{current_path}.params",
                budget=budget,
                copy_value=True,
                ancestors=ancestors,
            )
            modification_policy = _parse_modification_policy(
                document.get("modification_policy"),
                path=current_path,
            )
            if modification_policy:
                modification_policy = _bounded_json_mapping(
                    modification_policy,
                    path=f"{current_path}.modification_policy",
                    budget=budget,
                    copy_value=True,
                    ancestors=ancestors,
                )
            recovery_policy = _parse_recovery_policy(
                document.get("recovery_policy"),
                path=current_path,
            )
            if recovery_policy:
                recovery_policy = _bounded_json_mapping(
                    recovery_policy,
                    path=f"{current_path}.recovery_policy",
                    budget=budget,
                    copy_value=True,
                    ancestors=ancestors,
                )
            operator_stop_policy = _parse_operator_stop_policy(
                document.get("operator_stop_policy"),
                path=current_path,
            )
            _bounded_text(
                operator_stop_policy,
                path=f"{current_path}.operator_stop_policy",
                budget=budget,
            )

            raw_children = document.get("children", [])
            if raw_children is None:
                raw_children = []
            if type(raw_children) is not list:
                raise ProgramValidationError(
                    f"{current_path}.children must be a built-in list"
                )
            if len(raw_children) > _MAX_CHILDREN_PER_NODE:
                raise ProgramValidationError(
                    f"{current_path}.children exceeds the item bound"
                )
            children_identity = id(raw_children)
            if children_identity in ancestors:
                raise ProgramValidationError(
                    f"{current_path}.children contains a container cycle"
                )
            ancestors.add(children_identity)
            try:
                children = [
                    parse(child, f"{current_path}.children[{index}]", depth + 1)
                    for index, child in enumerate(raw_children)
                ]
            finally:
                ancestors.remove(children_identity)

            return TreeNode(
                id=node_id,
                name=name,
                params=params,
                children=children,
                modification_policy=modification_policy,
                recovery_policy=recovery_policy,
                operator_stop_policy=operator_stop_policy,
            )
        finally:
            ancestors.remove(identity)

    return parse(data, root_path, 0)


def find_tree_node_by_name(node: TreeNode, node_name: str) -> TreeNode | None:
    """Find the first preorder match after validating the complete tree."""

    _validate_tree_graph(node)
    target = _bounded_text(
        node_name,
        path="node_name",
        budget=_TraversalBudget(),
    )
    pending = [node]
    visited_rows = 0
    while pending:
        current = pending.pop()
        visited_rows += 1
        if visited_rows > _MAX_TREE_NODES:
            raise ProgramValidationError("tree node count exceeds bounded maximum")
        if current.name == target:
            return current
        pending.extend(reversed(current.children))
    return None


def _merge_normalized_mappings(
    base: dict[str, Any],
    patch: dict[str, Any],
    *,
    depth: int,
) -> dict[str, Any]:
    if depth > _MAX_VALUE_DEPTH:
        raise ProgramValidationError("parameter patch exceeds the merge depth bound")
    if "type" in base and "type" in patch and base.get("type") != patch.get("type"):
        return dict(patch)
    merged = dict(base)
    for key, value in patch.items():
        existing = merged.get(key)
        if type(existing) is dict and type(value) is dict:
            merged[key] = _merge_normalized_mappings(
                existing,
                value,
                depth=depth + 1,
            )
        else:
            merged[key] = value
    return merged


def merge_param_patch(
    base: Mapping[str, Any],
    patch: Mapping[str, Any],
) -> dict[str, Any]:
    """Deep-merge two bounded exact mappings into a detached JSON value."""

    input_budget = _TraversalBudget()
    normalized_base = _bounded_json_mapping(
        base,
        path="base",
        budget=input_budget,
        copy_value=True,
    )
    normalized_patch = _bounded_json_mapping(
        patch,
        path="patch",
        budget=input_budget,
        copy_value=True,
    )
    merged = _merge_normalized_mappings(
        normalized_base,
        normalized_patch,
        depth=0,
    )
    return _bounded_json_mapping(
        merged,
        path="merged parameters",
        budget=_TraversalBudget(),
        copy_value=True,
    )


def _normalize_overrides(
    overrides_by_name: Any,
) -> dict[str, dict[str, Any]]:
    if type(overrides_by_name) is not dict:
        raise ProgramValidationError("overrides_by_name must be a built-in mapping")
    if len(overrides_by_name) > _MAX_TREE_NODES:
        raise ProgramValidationError("overrides_by_name exceeds the item bound")
    budget = _TraversalBudget()
    normalized: dict[str, dict[str, Any]] = {}
    for raw_name, raw_patch in overrides_by_name.items():
        name = _bounded_text(
            raw_name,
            path="override node name",
            budget=budget,
        )
        normalized[name] = _bounded_json_mapping(
            raw_patch,
            path=f"overrides_by_name.{name}",
            budget=budget,
            copy_value=True,
        )
    return normalized


def apply_tree_param_overrides(
    node: TreeNode,
    overrides_by_name: Mapping[str, Mapping[str, Any]],
) -> TreeNode:
    """Apply bounded name-addressed patches without mutating the source tree."""

    _validate_tree_graph(node)
    overrides = _normalize_overrides(overrides_by_name)
    output_budget = _TraversalBudget()
    ancestors: set[int] = set()

    def rebuild(current: TreeNode, *, path: str, depth: int) -> TreeNode:
        if depth > _MAX_TREE_DEPTH:
            raise ProgramValidationError("tree depth exceeds bounded maximum")
        identity = id(current)
        if identity in ancestors:
            raise ProgramValidationError(f"{path} contains a tree cycle")
        output_budget.consume_tree_node()
        ancestors.add(identity)
        try:
            node_id = _bounded_text(
                current.id,
                path=f"{path}.id",
                budget=output_budget,
            )
            name = _bounded_text(
                current.name,
                path=f"{path}.name",
                budget=output_budget,
            )
            patch = overrides.get(name)
            merged_params = _merge_normalized_mappings(
                current.params,
                patch if patch is not None else {},
                depth=0,
            )
            params = _bounded_json_mapping(
                merged_params,
                path=f"{path}.params",
                budget=output_budget,
                copy_value=True,
                ancestors=ancestors,
            )
            modification_policy = _parse_modification_policy(
                current.modification_policy,
                path=path,
            )
            if modification_policy:
                modification_policy = _bounded_json_mapping(
                    modification_policy,
                    path=f"{path}.modification_policy",
                    budget=output_budget,
                    copy_value=True,
                    ancestors=ancestors,
                )
            recovery_policy = _parse_recovery_policy(
                current.recovery_policy,
                path=path,
            )
            if recovery_policy:
                recovery_policy = _bounded_json_mapping(
                    recovery_policy,
                    path=f"{path}.recovery_policy",
                    budget=output_budget,
                    copy_value=True,
                    ancestors=ancestors,
                )
            operator_stop_policy = _parse_operator_stop_policy(
                current.operator_stop_policy,
                path=path,
            )
            _bounded_text(
                operator_stop_policy,
                path=f"{path}.operator_stop_policy",
                budget=output_budget,
            )
            children = [
                rebuild(
                    child,
                    path=f"{path}.children[{index}]",
                    depth=depth + 1,
                )
                for index, child in enumerate(current.children)
            ]
            return TreeNode(
                id=node_id,
                name=name,
                params=params,
                children=children,
                modification_policy=modification_policy,
                recovery_policy=recovery_policy,
                operator_stop_policy=operator_stop_policy,
            )
        finally:
            ancestors.remove(identity)

    return rebuild(node, path="tree", depth=0)


@dataclass(frozen=True, slots=True)
class ProgramTreeNode:
    """One validated v1 node instance with portable authored values only."""

    node_id: str
    instance_id: str
    parameters: Mapping[str, Any]
    value_sources: Mapping[str, Mapping[str, Any]]
    children: tuple["ProgramTreeNode", ...]
    metadata: Mapping[str, Any] | None = None
    modification_policy: Mapping[str, str] | None = None
    recovery_policy: Mapping[str, Any] | None = None
    operator_stop_policy: str | None = None

    def to_dict(self) -> dict[str, Any]:
        result: dict[str, Any] = {
            "node_id": self.node_id,
            "instance_id": self.instance_id,
            "parameters": deepcopy(dict(self.parameters)),
            "children": [child.to_dict() for child in self.children],
        }
        if self.value_sources:
            result["value_sources"] = {
                parameter_id: deepcopy(dict(source))
                for parameter_id, source in self.value_sources.items()
            }
        if self.metadata is not None:
            result["metadata"] = deepcopy(dict(self.metadata))
        if self.modification_policy is not None:
            result["modification_policy"] = deepcopy(
                dict(self.modification_policy)
            )
        if self.recovery_policy is not None:
            result["recovery_policy"] = deepcopy(dict(self.recovery_policy))
        if self.operator_stop_policy is not None:
            result["operator_stop_policy"] = self.operator_stop_policy
        return result


@dataclass(frozen=True, slots=True)
class ProgramNodePolicy:
    """One complete immutable non-default policy row for a Program node."""

    instance_id: str
    modification_policy: Mapping[str, str]
    recovery_policy: Mapping[str, Any]
    operator_stop_policy: str

    def __post_init__(self) -> None:
        if (
            type(self.instance_id) is not str
            or _IDENTIFIER_RE.fullmatch(self.instance_id) is None
            or len(self.instance_id.encode("ascii")) > 128
        ):
            raise ProgramValidationError(
                "node policy instance_id must be a bounded portable ID"
            )
        modification = _parse_canonical_modification_policy(
            dict(self.modification_policy),
            path=f"node_policies.{self.instance_id}",
        )
        recovery_input = {
            key: list(value) if isinstance(value, tuple) else value
            for key, value in self.recovery_policy.items()
        }
        recovery = _parse_canonical_recovery_policy(
            recovery_input,
            path=f"node_policies.{self.instance_id}",
        )
        normalized = _normalize_canonical_node_policies(
            {
                "modification_policy": modification,
                "recovery_policy": recovery,
                "operator_stop_policy": self.operator_stop_policy,
            },
            path=f"node_policies.{self.instance_id}",
        )
        if normalized == (None, None, None):
            raise ProgramValidationError(
                "default node-policy rows must be omitted"
            )
        if recovery["allowed_skills"]:
            raise ProgramValidationError(
                "Program Run rejects nonempty allowed_skills until an exact "
                "revision-qualified retained skill referent exists"
            )
        object.__setattr__(
            self,
            "modification_policy",
            MappingProxyType(modification),
        )
        object.__setattr__(
            self,
            "recovery_policy",
            MappingProxyType(
                {
                    key: tuple(value) if isinstance(value, list) else value
                    for key, value in recovery.items()
                }
            ),
        )

    def to_dict(self) -> dict[str, Any]:
        return {
            "instance_id": self.instance_id,
            "modification_policy": dict(self.modification_policy),
            "recovery_policy": {
                key: list(value) if isinstance(value, tuple) else value
                for key, value in self.recovery_policy.items()
            },
            "operator_stop_policy": self.operator_stop_policy,
        }


def program_node_policies(program: "Program") -> tuple[ProgramNodePolicy, ...]:
    """Independently derive the canonical sparse policy vector for ``program``."""

    if not isinstance(program, Program):
        raise TypeError("program must be one validated Program")
    rows: list[ProgramNodePolicy] = []

    def visit(node: ProgramTreeNode) -> None:
        modification = {
            **_CANONICAL_DEFAULT_MODIFICATION_POLICY,
            **(
                {}
                if node.modification_policy is None
                else dict(node.modification_policy)
            ),
        }
        recovery = {
            **_CANONICAL_DEFAULT_RECOVERY_POLICY,
            **({} if node.recovery_policy is None else dict(node.recovery_policy)),
        }
        stop = node.operator_stop_policy or "inherit"
        if recovery["allowed_skills"]:
            raise ProgramValidationError(
                "Program Run rejects nonempty allowed_skills until an exact "
                "revision-qualified retained skill referent exists"
            )
        if (
            modification != _CANONICAL_DEFAULT_MODIFICATION_POLICY
            or recovery != _CANONICAL_DEFAULT_RECOVERY_POLICY
            or stop != "inherit"
        ):
            rows.append(
                ProgramNodePolicy(
                    instance_id=node.instance_id,
                    modification_policy=modification,
                    recovery_policy=recovery,
                    operator_stop_policy=stop,
                )
            )
        for child in node.children:
            visit(child)

    visit(program.tree)
    rows.sort(key=lambda row: row.instance_id)
    return tuple(rows)


def require_exact_program_binding_node_policies(
    program: "Program",
    program_binding: Mapping[str, Any],
) -> tuple[ProgramNodePolicy, ...]:
    """Reject a Binding unless its policy vector exactly equals the Program."""

    if not isinstance(program_binding, Mapping):
        raise TypeError("program_binding must be one verified mapping")
    expected = program_node_policies(program)
    raw_rows = program_binding.get("node_policies")
    if type(raw_rows) is not list or len(raw_rows) > _MAX_TREE_NODES:
        raise ProgramValidationError(
            "Program Binding node_policies must be one bounded required list"
        )
    expected_documents = [row.to_dict() for row in expected]
    try:
        raw_bytes = canonical_json_bytes(raw_rows)
        expected_bytes = canonical_json_bytes(expected_documents)
    except StrictArtifactError as exc:
        raise ProgramValidationError(
            f"Program Binding node_policies are malformed: {exc}"
        ) from exc
    if raw_bytes != expected_bytes:
        raise ProgramValidationError(
            "Program Binding node_policies do not exactly equal the selected Program"
        )
    return expected


@dataclass(frozen=True, slots=True)
class ProgramParameterDeclaration:
    parameter_id: str
    type: str
    default: Any
    constraints: Mapping[str, Any]
    runtime_mutability: str
    description: str = ""


@dataclass(frozen=True, slots=True)
class BlackboardFieldDeclaration:
    field: str
    type: str


@dataclass(frozen=True, slots=True)
class BlackboardKeyDeclaration:
    key: str
    type: str
    access: str
    persistence: str
    fields: Mapping[str, BlackboardFieldDeclaration]
    description: str = ""


def _program_error(path: str, message: str) -> NoReturn:
    raise ProgramValidationError(f"{path}: {message}")


def _program_mapping(value: Any, *, path: str) -> dict[str, Any]:
    if type(value) is not dict:
        _program_error(path, "must be a mapping")
    return value


def _program_list(value: Any, *, path: str) -> list[Any]:
    if type(value) is not list:
        _program_error(path, "must be a list")
    if len(value) > _MAX_PROGRAM_DECLARATIONS:
        _program_error(path, "exceeds the fixed item bound")
    return value


def _program_closed(
    value: Mapping[str, Any],
    *,
    required: frozenset[str],
    optional: frozenset[str] = frozenset(),
    path: str,
) -> None:
    fields = set(value)
    missing = required - fields
    unknown = fields - required - optional
    if missing:
        _program_error(path, f"is missing required fields {sorted(missing)}")
    if unknown:
        _program_error(path, f"contains unknown fields {sorted(unknown)}")


def _program_text(
    value: Any,
    *,
    path: str,
    allow_empty: bool = False,
    max_chars: int = _MAX_VALUE_STRING_CHARS,
) -> str:
    if type(value) is not str or (not allow_empty and not value):
        qualifier = "a string" if allow_empty else "a non-empty string"
        _program_error(path, f"must be {qualifier}")
    if len(value) > max_chars or "\x00" in value:
        _program_error(path, "exceeds its text bound or contains NUL")
    return value


def _program_identifier(value: Any, *, path: str) -> str:
    text = _program_text(value, path=path, max_chars=_MAX_IDENTIFIER_CHARS)
    if _IDENTIFIER_RE.fullmatch(text) is None:
        _program_error(path, "must be a portable lower-case identifier")
    return text


def _program_number(value: Any, *, path: str) -> float:
    if type(value) not in {int, float}:
        _program_error(path, "must be a finite number")
    if type(value) is int and value.bit_length() > _MAX_INTEGER_BITS:
        _program_error(path, "exceeds the integer bound")
    try:
        normalized = float(value)
    except (OverflowError, ValueError):
        _program_error(path, "must be a finite number")
    if not math.isfinite(normalized):
        _program_error(path, "must be a finite number")
    return normalized


def _canonical_identifier_list(
    value: Any,
    *,
    path: str,
    allow_empty: bool = True,
) -> tuple[str, ...]:
    items = tuple(
        _program_identifier(item, path=f"{path}[{index}]")
        for index, item in enumerate(_program_list(value, path=path))
    )
    if not allow_empty and not items:
        _program_error(path, "must not be empty")
    if tuple(sorted(items)) != items or len(items) != len(set(items)):
        _program_error(path, "must be duplicate-free and canonical-sorted")
    return items


def _bounded_text_list(value: Any, *, path: str) -> tuple[str, ...]:
    """Validate an authored ordered string list without treating text as IDs."""

    return tuple(
        _program_text(item, path=f"{path}[{index}]")
        for index, item in enumerate(_program_list(value, path=path))
    )


def _canonical_mapping_keys(value: Mapping[str, Any], *, path: str) -> None:
    keys = tuple(value)
    for key in keys:
        _program_identifier(key, path=f"{path} key")
    if tuple(sorted(keys)) != keys:
        _program_error(path, "keys must be canonical-sorted")


def _freeze_mapping(value: Mapping[str, Any]) -> Mapping[str, Any]:
    return MappingProxyType(deepcopy(dict(value)))


def _schema_semantic_types(
    schema: CatalogSchema,
    catalog: NodeCatalog,
    *,
    stack: tuple[int, ...] = (),
) -> frozenset[str]:
    identity = id(schema)
    if identity in stack:
        return frozenset()
    concrete = catalog.schema_registry.resolve(schema)
    result: set[str] = set()
    if concrete.semantic_type is not None:
        result.add(concrete.semantic_type)
    next_stack = (*stack, identity)
    for child in concrete.properties.values():
        result.update(_schema_semantic_types(child, catalog, stack=next_stack))
    if concrete.items is not None:
        result.update(_schema_semantic_types(concrete.items, catalog, stack=next_stack))
    if concrete.additional_property_schema is not None:
        result.update(
            _schema_semantic_types(
                concrete.additional_property_schema,
                catalog,
                stack=next_stack,
            )
        )
    return frozenset(result)


@dataclass(slots=True)
class _ProgramValidationContext:
    catalog: NodeCatalog
    component_roles: dict[str, dict[str, Any]]
    target_roles: dict[str, dict[str, Any]]
    scene_resource_ids: set[str]
    parameters: dict[str, ProgramParameterDeclaration]
    blackboard_namespaces: set[str]
    blackboard_keys: dict[str, BlackboardKeyDeclaration]
    contact_policy_ids: set[str]
    tree_instance_ids: set[str] = field(default_factory=set)


_TOP_LEVEL_FIELDS: Final = frozenset(
    {
        "api_version",
        "program_id",
        "program_revision",
        "program_profile",
        "name",
        "description",
        "catalog",
        "compatibility",
        "viewer",
        "component_roles",
        "target_roles",
        "scene",
        "parameters",
        "blackboard",
        "contact_policies",
        "tree",
    }
)
_PROGRAM_PARAMETER_TYPES: Final = frozenset(
    {
        "bool",
        "float64",
        "int64",
        "model_response",
        "pick_planning_result",
        "scene_resource",
        "scene_selection_context",
        "scene_selection_result",
        "stored_pose",
        "string",
        "string_list",
    }
)
_RUNTIME_MUTABILITY_VALUES: Final = frozenset(
    {"immutable", "before_run", "between_operations"}
)
_BLACKBOARD_ACCESS_VALUES: Final = frozenset({"read", "write", "read_write"})
_BLACKBOARD_PERSISTENCE_VALUES: Final = frozenset({"node", "run", "persistent"})
_DYNAMIC_SOURCE_FORBIDDEN_SEMANTICS: Final = frozenset(
    {
        "collision_body_role",
        "component_role",
        "frame_role",
        "resource_role",
        "source_joint",
        "target_role",
    }
)
_DYNAMIC_SOURCE_FORBIDDEN_PARAMETERS: Final = frozenset(
    {
        "contact_policy_id",
        "model_endpoint_alias",
        "prompt_template_id",
    }
)


def _validate_compatibility(value: Any, *, path: str) -> None:
    row = _program_mapping(value, path=path)
    _program_closed(
        row,
        required=frozenset(),
        optional=frozenset({"lanes", "assistant", "requires", "tags"}),
        path=path,
    )
    if "lanes" in row:
        lanes = _canonical_identifier_list(row["lanes"], path=f"{path}.lanes")
        unknown = set(lanes) - {"manyforge-only", "isaac-sim", "real-robot"}
        if unknown:
            _program_error(f"{path}.lanes", f"contains unknown lanes {sorted(unknown)}")
    if "assistant" in row and row["assistant"] not in {
        "required",
        "optional",
        "forbidden",
    }:
        _program_error(f"{path}.assistant", "must be required, optional, or forbidden")
    for field_name in ("requires", "tags"):
        if field_name in row:
            _canonical_identifier_list(row[field_name], path=f"{path}.{field_name}")


def _validate_viewer(value: Any, *, path: str) -> None:
    row = _program_mapping(value, path=path)
    _program_closed(
        row,
        required=frozenset(),
        optional=frozenset(
            {
                "frame_role_visibility",
                "frame_gizmo_size_m",
                "resource_role_visibility",
                "object_axes_gizmo_size_m",
            }
        ),
        path=path,
    )
    for field_name in ("frame_role_visibility", "resource_role_visibility"):
        if field_name not in row:
            continue
        visibility = _program_mapping(row[field_name], path=f"{path}.{field_name}")
        _canonical_mapping_keys(visibility, path=f"{path}.{field_name}")
        if any(type(item) is not bool for item in visibility.values()):
            _program_error(f"{path}.{field_name}", "values must be booleans")
    for field_name in ("frame_gizmo_size_m", "object_axes_gizmo_size_m"):
        if (
            field_name in row
            and _program_number(row[field_name], path=f"{path}.{field_name}") <= 0.0
        ):
            _program_error(f"{path}.{field_name}", "must be positive")


def _validate_viewer_references(
    value: Any,
    *,
    path: str,
    target_roles: Mapping[str, dict[str, Any]],
    scene_resource_roles: set[str],
) -> None:
    row = _program_mapping(value, path=path)
    declared_frame_roles = {
        role
        for target in target_roles.values()
        for role in target.get("required_frame_roles", ())
    }
    declared_resource_roles = scene_resource_roles | {
        role
        for target in target_roles.values()
        for role in target.get("required_resource_roles", ())
    }
    for field_name, declared in (
        ("frame_role_visibility", declared_frame_roles),
        ("resource_role_visibility", declared_resource_roles),
    ):
        for role in _program_mapping(
            row.get(field_name, {}), path=f"{path}.{field_name}"
        ):
            if role not in declared:
                _program_error(
                    f"{path}.{field_name}.{role}",
                    "does not name a declared role",
                )


def _validate_component_roles(value: Any, *, path: str) -> dict[str, dict[str, Any]]:
    rows = _program_mapping(value, path=path)
    if not rows or len(rows) > _MAX_PROGRAM_ROLES:
        _program_error(path, "must be a bounded non-empty mapping")
    _canonical_mapping_keys(rows, path=path)
    result: dict[str, dict[str, Any]] = {}
    allowed = frozenset(
        {
            "required_component_type_tags",
            "required_model_roles",
            "required_attachment_interfaces",
        }
    )
    for role_id, raw in rows.items():
        role_path = f"{path}.{role_id}"
        row = _program_mapping(raw, path=role_path)
        _program_closed(
            row,
            required=frozenset(),
            optional=allowed,
            path=role_path,
        )
        normalized = dict(row)
        for field_name in allowed:
            if field_name in row:
                normalized[field_name] = _canonical_identifier_list(
                    row[field_name], path=f"{role_path}.{field_name}"
                )
        result[role_id] = normalized
    return result


def _validate_target_roles(
    value: Any,
    *,
    path: str,
    component_roles: Mapping[str, dict[str, Any]],
) -> dict[str, dict[str, Any]]:
    rows = _program_mapping(value, path=path)
    if len(rows) > _MAX_PROGRAM_ROLES:
        _program_error(path, "exceeds the fixed role bound")
    _canonical_mapping_keys(rows, path=path)
    result: dict[str, dict[str, Any]] = {}
    role_fields = (
        "required_frame_roles",
        "required_resource_roles",
        "required_collision_body_roles",
        "required_model_roles",
    )
    for role_id, raw in rows.items():
        role_path = f"{path}.{role_id}"
        row = _program_mapping(raw, path=role_path)
        _program_closed(
            row,
            required=frozenset({"component_role", "required_capabilities"}),
            optional=frozenset(role_fields),
            path=role_path,
        )
        component_role = _program_identifier(
            row["component_role"], path=f"{role_path}.component_role"
        )
        if component_role not in component_roles:
            _program_error(
                f"{role_path}.component_role",
                "does not name a declared component role",
            )
        raw_capabilities = _program_list(
            row["required_capabilities"],
            path=f"{role_path}.required_capabilities",
        )
        if not raw_capabilities:
            _program_error(f"{role_path}.required_capabilities", "must not be empty")
        capabilities: list[dict[str, Any]] = []
        capability_ids: list[str] = []
        for index, raw_capability in enumerate(raw_capabilities):
            capability_path = f"{role_path}.required_capabilities[{index}]"
            capability = _program_mapping(raw_capability, path=capability_path)
            _program_closed(
                capability,
                required=frozenset({"capability_id", "parameters", "disposition"}),
                path=capability_path,
            )
            capability_id = _program_identifier(
                capability["capability_id"],
                path=f"{capability_path}.capability_id",
            )
            parameters = _program_mapping(
                capability["parameters"], path=f"{capability_path}.parameters"
            )
            canonical_json_bytes(parameters)
            if capability["disposition"] not in {"required", "optional"}:
                _program_error(
                    f"{capability_path}.disposition",
                    "must be required or optional",
                )
            capability_ids.append(capability_id)
            capabilities.append(deepcopy(capability))
        if capability_ids != sorted(capability_ids) or len(capability_ids) != len(
            set(capability_ids)
        ):
            _program_error(
                f"{role_path}.required_capabilities",
                "must be unique and canonical-sorted by capability_id",
            )
        normalized: dict[str, Any] = {
            "component_role": component_role,
            "required_capabilities": tuple(capabilities),
        }
        for field_name in role_fields:
            normalized[field_name] = (
                _canonical_identifier_list(
                    row[field_name], path=f"{role_path}.{field_name}"
                )
                if field_name in row
                else ()
            )
        result[role_id] = normalized
    return result


def _validate_orientation(value: Any, *, path: str) -> None:
    row = _program_mapping(value, path=path)
    if set(row) not in ({"xyzw"}, {"rpy_deg"}):
        _program_error(path, "must contain exactly one of xyzw or rpy_deg")
    field_name = next(iter(row))
    values = _program_list(row[field_name], path=f"{path}.{field_name}")
    expected = 4 if field_name == "xyzw" else 3
    if len(values) != expected:
        _program_error(f"{path}.{field_name}", f"must contain {expected} numbers")
    normalized = [
        _program_number(item, path=f"{path}.{field_name}[{index}]")
        for index, item in enumerate(values)
    ]
    if field_name == "xyzw":
        squared_norm = sum(item * item for item in normalized)
        if abs(squared_norm - 1.0) > 1e-6:
            _program_error(f"{path}.xyzw", "must already be a unit quaternion")


def _validate_initial_scene_pose(
    value: Any,
    *,
    path: str,
    component_roles: Mapping[str, dict[str, Any]],
) -> None:
    pose = _program_mapping(value, path=path)
    _program_closed(
        pose,
        required=frozenset({"frame_ref", "position_m", "orientation"}),
        path=path,
    )
    position = _program_list(pose["position_m"], path=f"{path}.position_m")
    if len(position) != 3:
        _program_error(f"{path}.position_m", "must contain three numbers")
    for index, item in enumerate(position):
        _program_number(item, path=f"{path}.position_m[{index}]")
    _validate_orientation(pose["orientation"], path=f"{path}.orientation")
    frame = _program_mapping(pose["frame_ref"], path=f"{path}.frame_ref")
    kind = frame.get("kind")
    if kind == "cell_frame":
        _program_closed(
            frame,
            required=frozenset({"kind"}),
            path=f"{path}.frame_ref",
        )
        return
    if kind != "component_frame":
        _program_error(
            f"{path}.frame_ref.kind", "must be cell_frame or component_frame"
        )
    _program_closed(
        frame,
        required=frozenset({"kind", "component_role", "model_role", "frame_role"}),
        path=f"{path}.frame_ref",
    )
    component_role = _program_identifier(
        frame["component_role"], path=f"{path}.frame_ref.component_role"
    )
    if component_role not in component_roles:
        _program_error(
            f"{path}.frame_ref.component_role",
            "does not name a declared component role",
        )
    model_role = _program_identifier(
        frame["model_role"], path=f"{path}.frame_ref.model_role"
    )
    _program_identifier(frame["frame_role"], path=f"{path}.frame_ref.frame_role")
    required_models = component_roles[component_role].get("required_model_roles", ())
    if required_models and model_role not in required_models:
        _program_error(
            f"{path}.frame_ref.model_role",
            "is not required by the selected component role",
        )


def _validate_scene(
    value: Any,
    *,
    path: str,
    catalog: NodeCatalog,
    component_roles: Mapping[str, dict[str, Any]],
) -> tuple[set[str], set[str], set[str]]:
    rows = _program_list(value, path=path)
    scene_ids: list[str] = []
    scene_resource_roles: set[str] = set()
    collision_policy_refs: set[str] = set()
    shape_schema = catalog.schema_registry.types["shape"]
    for index, raw in enumerate(rows):
        row_path = f"{path}[{index}]"
        row = _program_mapping(raw, path=row_path)
        _program_closed(
            row,
            required=frozenset({"scene_resource_id", "shape", "pose"}),
            optional=frozenset({"resource_roles", "metadata", "collision_policy_ids"}),
            path=row_path,
        )
        scene_id = _program_identifier(
            row["scene_resource_id"], path=f"{row_path}.scene_resource_id"
        )
        scene_ids.append(scene_id)
        shape_issues = validate_value(
            shape_schema,
            row["shape"],
            registry=catalog.schema_registry,
            path=f"{row_path}.shape",
        )
        if shape_issues:
            _program_error(shape_issues[0].path, shape_issues[0].message)
        _validate_initial_scene_pose(
            row["pose"], path=f"{row_path}.pose", component_roles=component_roles
        )
        if "resource_roles" in row:
            scene_resource_roles.update(
                _canonical_identifier_list(
                    row["resource_roles"], path=f"{row_path}.resource_roles"
                )
            )
        if "metadata" in row:
            canonical_json_bytes(
                _program_mapping(row["metadata"], path=f"{row_path}.metadata")
            )
        if "collision_policy_ids" in row:
            collision_policy_refs.update(
                _canonical_identifier_list(
                    row["collision_policy_ids"],
                    path=f"{row_path}.collision_policy_ids",
                )
            )
    if scene_ids != sorted(scene_ids) or len(scene_ids) != len(set(scene_ids)):
        _program_error(path, "must be unique and canonical-sorted by scene_resource_id")
    return set(scene_ids), scene_resource_roles, collision_policy_refs


def _validate_pose_frame_ref(
    value: Any,
    *,
    path: str,
    target_roles: Mapping[str, dict[str, Any]],
    scene_resource_ids: set[str],
) -> None:
    frame = _program_mapping(value, path=path)
    kind = frame.get("kind")
    if kind == "cell_frame":
        _program_closed(frame, required=frozenset({"kind"}), path=path)
        return
    if kind == "scene_resource":
        _program_closed(
            frame,
            required=frozenset({"kind", "scene_resource_id"}),
            path=path,
        )
        resource_id = _program_identifier(
            frame["scene_resource_id"], path=f"{path}.scene_resource_id"
        )
        if resource_id not in scene_resource_ids:
            _program_error(
                f"{path}.scene_resource_id",
                "does not name a declared scene resource",
            )
        return
    if kind != "target_frame":
        _program_error(
            f"{path}.kind", "must be cell_frame, target_frame, or scene_resource"
        )
    _program_closed(
        frame,
        required=frozenset({"kind", "target_role", "frame_role"}),
        path=path,
    )
    target_role = _program_identifier(frame["target_role"], path=f"{path}.target_role")
    frame_role = _program_identifier(frame["frame_role"], path=f"{path}.frame_role")
    target = target_roles.get(target_role)
    if target is None:
        _program_error(f"{path}.target_role", "does not name a declared target role")
    if frame_role not in target.get("required_frame_roles", ()):
        _program_error(
            f"{path}.frame_role",
            "is not required by the selected target role",
        )


def _validate_declared_type_value(
    declared_type: str,
    value: Any,
    *,
    path: str,
    scene_resource_ids: set[str] | None = None,
    target_roles: Mapping[str, dict[str, Any]] | None = None,
) -> None:
    if declared_type == "bool":
        if type(value) is not bool:
            _program_error(path, "must be a boolean")
        return
    if declared_type == "float64":
        _program_number(value, path=path)
        return
    if declared_type == "int64":
        if type(value) is not int or value < -(1 << 63) or value >= 1 << 63:
            _program_error(path, "must be a signed 64-bit integer")
        return
    if declared_type == "string":
        _program_text(value, path=path)
        return
    if declared_type == "scene_resource":
        resource_id = _program_identifier(value, path=path)
        if scene_resource_ids is not None and resource_id not in scene_resource_ids:
            _program_error(path, "does not name a declared scene resource")
        return
    if declared_type == "string_list":
        _bounded_text_list(value, path=path)
        return
    if declared_type == "stored_pose":
        row = _program_mapping(value, path=path)
        _program_closed(
            row,
            required=frozenset({"frame_ref", "position_m", "orientation"}),
            path=path,
        )
        position = _program_list(row["position_m"], path=f"{path}.position_m")
        if len(position) != 3:
            _program_error(f"{path}.position_m", "must contain three numbers")
        for index, item in enumerate(position):
            _program_number(item, path=f"{path}.position_m[{index}]")
        _validate_orientation(row["orientation"], path=f"{path}.orientation")
        _validate_pose_frame_ref(
            row["frame_ref"],
            path=f"{path}.frame_ref",
            target_roles=target_roles or {},
            scene_resource_ids=scene_resource_ids or set(),
        )
        return
    # Structured runtime values are declared by type but are not valid Program
    # defaults. Their exact shape is produced and consumed through catalog
    # contracts at node-attempt time.
    if value is not None:
        _program_error(path, f"{declared_type} cannot have an authored default")


def _validate_constraints(
    value: Any,
    *,
    path: str,
    declared_type: str,
) -> dict[str, Any]:
    constraints = _program_mapping(value, path=path)
    allowed = frozenset(
        {
            "min_inclusive",
            "min_exclusive",
            "max_inclusive",
            "max_exclusive",
            "min_length",
            "max_length",
        }
    )
    _program_closed(
        constraints,
        required=frozenset(),
        optional=allowed,
        path=path,
    )
    numeric_constraints = {
        "min_inclusive",
        "min_exclusive",
        "max_inclusive",
        "max_exclusive",
    }
    length_constraints = {"min_length", "max_length"}
    if set(constraints) & numeric_constraints and declared_type not in {
        "float64",
        "int64",
    }:
        _program_error(path, f"numeric constraints are unsupported for {declared_type}")
    if set(constraints) & length_constraints and declared_type not in {
        "string",
        "string_list",
    }:
        _program_error(path, f"length constraints are unsupported for {declared_type}")
    if {"min_inclusive", "min_exclusive"}.issubset(constraints):
        _program_error(path, "cannot declare two minimum bounds")
    if {"max_inclusive", "max_exclusive"}.issubset(constraints):
        _program_error(path, "cannot declare two maximum bounds")
    for name, item in constraints.items():
        if name in {"min_length", "max_length"}:
            if type(item) is not int or item < 0:
                _program_error(f"{path}.{name}", "must be a non-negative integer")
        else:
            if declared_type == "int64" and type(item) is not int:
                _program_error(f"{path}.{name}", "must be an integer bound")
            _program_number(item, path=f"{path}.{name}")
    for low, high in (
        ("min_inclusive", "max_inclusive"),
        ("min_inclusive", "max_exclusive"),
        ("min_exclusive", "max_inclusive"),
        ("min_exclusive", "max_exclusive"),
        ("min_length", "max_length"),
    ):
        if low not in constraints or high not in constraints:
            continue
        if constraints[low] > constraints[high]:
            _program_error(path, f"{low} exceeds {high}")
        if constraints[low] == constraints[high] and (
            low.endswith("exclusive") or high.endswith("exclusive")
        ):
            _program_error(path, f"{low} and {high} define an empty range")
    return deepcopy(constraints)


def _validate_default_constraints(
    declaration: ProgramParameterDeclaration,
    *,
    path: str,
) -> None:
    value = declaration.default
    constraints = declaration.constraints
    if value is None:
        return
    if declaration.type in {"float64", "int64"}:
        numeric = float(value)
        for name, predicate in (
            ("min_inclusive", lambda bound: numeric >= float(bound)),
            ("min_exclusive", lambda bound: numeric > float(bound)),
            ("max_inclusive", lambda bound: numeric <= float(bound)),
            ("max_exclusive", lambda bound: numeric < float(bound)),
        ):
            if name in constraints and not predicate(constraints[name]):
                _program_error(path, f"default violates {name}")
    if declaration.type in {"string", "string_list"}:
        length = len(value)
        if "min_length" in constraints and length < constraints["min_length"]:
            _program_error(path, "default violates min_length")
        if "max_length" in constraints and length > constraints["max_length"]:
            _program_error(path, "default violates max_length")


def _validate_parameters(
    value: Any,
    *,
    path: str,
    scene_resource_ids: set[str],
    target_roles: Mapping[str, dict[str, Any]],
) -> dict[str, ProgramParameterDeclaration]:
    rows = _program_list(value, path=path)
    ids: list[str] = []
    result: dict[str, ProgramParameterDeclaration] = {}
    for index, raw in enumerate(rows):
        row_path = f"{path}[{index}]"
        row = _program_mapping(raw, path=row_path)
        _program_closed(
            row,
            required=frozenset(
                {"parameter_id", "type", "default", "runtime_mutability"}
            ),
            optional=frozenset({"constraints", "description"}),
            path=row_path,
        )
        parameter_id = _program_identifier(
            row["parameter_id"], path=f"{row_path}.parameter_id"
        )
        declared_type = _program_identifier(row["type"], path=f"{row_path}.type")
        if declared_type not in _PROGRAM_PARAMETER_TYPES:
            _program_error(f"{row_path}.type", "is not a registered Program type")
        runtime_mutability = _program_identifier(
            row["runtime_mutability"], path=f"{row_path}.runtime_mutability"
        )
        if runtime_mutability not in _RUNTIME_MUTABILITY_VALUES:
            _program_error(f"{row_path}.runtime_mutability", "has an unsupported value")
        default = deepcopy(row["default"])
        _validate_declared_type_value(
            declared_type,
            default,
            path=f"{row_path}.default",
            scene_resource_ids=scene_resource_ids,
            target_roles=target_roles,
        )
        constraints = _validate_constraints(
            row.get("constraints", {}),
            path=f"{row_path}.constraints",
            declared_type=declared_type,
        )
        declaration = ProgramParameterDeclaration(
            parameter_id=parameter_id,
            type=declared_type,
            default=default,
            constraints=_freeze_mapping(constraints),
            runtime_mutability=runtime_mutability,
            description=_program_text(
                row.get("description", ""),
                path=f"{row_path}.description",
                allow_empty=True,
                max_chars=_MAX_DESCRIPTION_CHARS,
            ),
        )
        _validate_default_constraints(declaration, path=f"{row_path}.default")
        ids.append(parameter_id)
        result[parameter_id] = declaration
    if ids != sorted(ids) or len(ids) != len(set(ids)):
        _program_error(path, "must be unique and canonical-sorted by parameter_id")
    return result


def _validate_blackboard(
    value: Any,
    *,
    path: str,
) -> tuple[set[str], dict[str, BlackboardKeyDeclaration]]:
    root = _program_mapping(value, path=path)
    _program_closed(
        root,
        required=frozenset({"namespaces", "keys"}),
        path=path,
    )
    namespace_rows = _program_list(root["namespaces"], path=f"{path}.namespaces")
    namespace_ids: list[str] = []
    for index, raw in enumerate(namespace_rows):
        row_path = f"{path}.namespaces[{index}]"
        row = _program_mapping(raw, path=row_path)
        _program_closed(
            row,
            required=frozenset({"namespace_id", "owner"}),
            optional=frozenset({"description"}),
            path=row_path,
        )
        namespace_id = _program_identifier(
            row["namespace_id"], path=f"{row_path}.namespace_id"
        )
        if row["owner"] != "program":
            _program_error(f"{row_path}.owner", "must be program")
        if "description" in row:
            _program_text(
                row["description"],
                path=f"{row_path}.description",
                allow_empty=True,
                max_chars=_MAX_DESCRIPTION_CHARS,
            )
        namespace_ids.append(namespace_id)
    if namespace_ids != sorted(namespace_ids) or len(namespace_ids) != len(
        set(namespace_ids)
    ):
        _program_error(
            f"{path}.namespaces",
            "must be unique and canonical-sorted by namespace_id",
        )
    namespaces = set(namespace_ids)

    key_rows = _program_list(root["keys"], path=f"{path}.keys")
    key_ids: list[str] = []
    keys: dict[str, BlackboardKeyDeclaration] = {}
    for index, raw in enumerate(key_rows):
        row_path = f"{path}.keys[{index}]"
        row = _program_mapping(raw, path=row_path)
        _program_closed(
            row,
            required=frozenset({"key", "type", "access", "persistence"}),
            optional=frozenset({"fields", "description"}),
            path=row_path,
        )
        key = _program_identifier(row["key"], path=f"{row_path}.key")
        if "." in key and key.split(".", 1)[0] not in namespaces:
            _program_error(
                f"{row_path}.key",
                "uses an undeclared blackboard namespace",
            )
        declared_type = _program_identifier(row["type"], path=f"{row_path}.type")
        if declared_type not in _PROGRAM_PARAMETER_TYPES:
            _program_error(f"{row_path}.type", "is not a registered dataflow type")
        access = _program_identifier(row["access"], path=f"{row_path}.access")
        if access not in _BLACKBOARD_ACCESS_VALUES:
            _program_error(f"{row_path}.access", "has an unsupported value")
        persistence = _program_identifier(
            row["persistence"], path=f"{row_path}.persistence"
        )
        if persistence not in _BLACKBOARD_PERSISTENCE_VALUES:
            _program_error(f"{row_path}.persistence", "has an unsupported value")
        fields: dict[str, BlackboardFieldDeclaration] = {}
        field_ids: list[str] = []
        for field_index, raw_field in enumerate(
            _program_list(row.get("fields", []), path=f"{row_path}.fields")
        ):
            field_path = f"{row_path}.fields[{field_index}]"
            field_row = _program_mapping(raw_field, path=field_path)
            _program_closed(
                field_row,
                required=frozenset({"field", "type"}),
                path=field_path,
            )
            field_id = _program_identifier(
                field_row["field"], path=f"{field_path}.field"
            )
            field_type = _program_identifier(
                field_row["type"], path=f"{field_path}.type"
            )
            if field_type not in _PROGRAM_PARAMETER_TYPES:
                _program_error(f"{field_path}.type", "is not registered")
            field_ids.append(field_id)
            fields[field_id] = BlackboardFieldDeclaration(field_id, field_type)
        if "fields" in row and not fields:
            _program_error(f"{row_path}.fields", "must not be empty when present")
        if field_ids != sorted(field_ids) or len(field_ids) != len(set(field_ids)):
            _program_error(
                f"{row_path}.fields",
                "must be unique and canonical-sorted by field",
            )
        if fields and declared_type not in {
            "model_response",
            "pick_planning_result",
            "scene_selection_context",
            "scene_selection_result",
        }:
            _program_error(
                f"{row_path}.fields",
                "are valid only for a declared structured bundle type",
            )
        key_ids.append(key)
        keys[key] = BlackboardKeyDeclaration(
            key=key,
            type=declared_type,
            access=access,
            persistence=persistence,
            fields=MappingProxyType(fields),
            description=_program_text(
                row.get("description", ""),
                path=f"{row_path}.description",
                allow_empty=True,
                max_chars=_MAX_DESCRIPTION_CHARS,
            ),
        )
    if key_ids != sorted(key_ids) or len(key_ids) != len(set(key_ids)):
        _program_error(f"{path}.keys", "must be unique and canonical-sorted by key")
    return namespaces, keys


def _validate_contact_selector(
    value: Any,
    *,
    path: str,
    component_roles: Mapping[str, dict[str, Any]],
    target_roles: Mapping[str, dict[str, Any]],
    blackboard_keys: Mapping[str, BlackboardKeyDeclaration],
) -> tuple[str, str]:
    row = _program_mapping(value, path=path)
    kind = row.get("kind")
    if kind == "component_model_body":
        _program_closed(
            row,
            required=frozenset({"kind", "component_role_id", "collision_body_role_id"}),
            optional=frozenset({"model_role_id"}),
            path=path,
        )
        component_role = _program_identifier(
            row["component_role_id"], path=f"{path}.component_role_id"
        )
        if component_role not in component_roles:
            _program_error(
                f"{path}.component_role_id",
                "does not name a declared component role",
            )
        collision_body_role = _program_identifier(
            row["collision_body_role_id"],
            path=f"{path}.collision_body_role_id",
        )
        model_role: str | None = None
        if "model_role_id" in row:
            model_role = _program_identifier(
                row["model_role_id"], path=f"{path}.model_role_id"
            )
            required_models = component_roles[component_role].get(
                "required_model_roles", ()
            )
            if required_models and model_role not in required_models:
                _program_error(
                    f"{path}.model_role_id",
                    "is not required by the selected component role",
                )
        matching_targets = [
            target_role
            for target_role, target in target_roles.items()
            if target["component_role"] == component_role
            and collision_body_role in target.get("required_collision_body_roles", ())
            and (
                model_role is None
                or model_role in target.get("required_model_roles", ())
            )
        ]
        if len(matching_targets) != 1:
            _program_error(
                path,
                "must resolve to exactly one declared Program target role",
            )
        return kind, canonical_json_bytes(row).decode("utf-8")
    if kind == "runtime_object":
        _program_closed(
            row,
            required=frozenset({"kind", "runtime_object_id"}),
            path=path,
        )
        runtime_object_id = _program_identifier(
            row["runtime_object_id"], path=f"{path}.runtime_object_id"
        )
        declaration = blackboard_keys.get(runtime_object_id)
        if declaration is None or declaration.type != "scene_resource":
            _program_error(
                f"{path}.runtime_object_id",
                "must name a declared scene_resource blackboard key",
            )
        return kind, runtime_object_id
    _program_error(f"{path}.kind", "has an unsupported contact body selector")


def _validate_contact_policies(
    value: Any,
    *,
    path: str,
    component_roles: Mapping[str, dict[str, Any]],
    target_roles: Mapping[str, dict[str, Any]],
    blackboard_keys: Mapping[str, BlackboardKeyDeclaration],
) -> set[str]:
    rows = _program_list(value, path=path)
    policy_ids: list[str] = []
    for index, raw in enumerate(rows):
        policy_path = f"{path}[{index}]"
        policy = _program_mapping(raw, path=policy_path)
        _program_closed(
            policy,
            required=frozenset(
                {
                    "api_version",
                    "policy_id",
                    "version",
                    "mode",
                    "sensing_mode",
                    "default_unmatched_response",
                    "zones",
                }
            ),
            path=policy_path,
        )
        if policy["api_version"] != "manyforge.contact_policy.v1":
            _program_error(
                f"{policy_path}.api_version",
                "must be manyforge.contact_policy.v1",
            )
        policy_id = _program_identifier(
            policy["policy_id"], path=f"{policy_path}.policy_id"
        )
        try:
            verify_content_identity(
                policy,
                own_field="version",
                identity_domain="catalog-record-revision/contact_policy",
                api_version="manyforge.contact_policy.v1",
                label=policy_path,
            )
        except StrictArtifactError as exc:
            _program_error(f"{policy_path}.version", str(exc))
        if (
            policy["mode"] != "contact_grasp"
            or policy["sensing_mode"] != "geometry_only"
            or policy["default_unmatched_response"] != "hard_stop"
        ):
            _program_error(
                policy_path,
                "is outside the qualified geometry-only contact_grasp shape",
            )
        zones = _program_list(policy["zones"], path=f"{policy_path}.zones")
        if len(zones) != 1:
            _program_error(
                f"{policy_path}.zones",
                "must contain exactly one zone in the qualified profile",
            )
        zone_ids: list[str] = []
        runtime_objects: set[str] = set()
        for zone_index, raw_zone in enumerate(zones):
            zone_path = f"{policy_path}.zones[{zone_index}]"
            zone = _program_mapping(raw_zone, path=zone_path)
            _program_closed(
                zone,
                required=frozenset(
                    {
                        "zone_id",
                        "body_pairs",
                        "violation_response",
                        "max_penetration_m",
                        "terminal_window_start_fraction",
                    }
                ),
                path=zone_path,
            )
            zone_id = _program_identifier(zone["zone_id"], path=f"{zone_path}.zone_id")
            zone_ids.append(zone_id)
            if zone["violation_response"] != "hard_stop":
                _program_error(f"{zone_path}.violation_response", "must be hard_stop")
            if (
                _program_number(
                    zone["max_penetration_m"], path=f"{zone_path}.max_penetration_m"
                )
                <= 0.0
            ):
                _program_error(f"{zone_path}.max_penetration_m", "must be positive")
            terminal_fraction = _program_number(
                zone["terminal_window_start_fraction"],
                path=f"{zone_path}.terminal_window_start_fraction",
            )
            if terminal_fraction < 0.0 or terminal_fraction > 1.0:
                _program_error(
                    f"{zone_path}.terminal_window_start_fraction",
                    "must be in [0, 1]",
                )
            pairs = _program_list(zone["body_pairs"], path=f"{zone_path}.body_pairs")
            if not pairs:
                _program_error(f"{zone_path}.body_pairs", "must not be empty")
            pair_keys: list[tuple[str, str]] = []
            for pair_index, raw_pair in enumerate(pairs):
                pair_path = f"{zone_path}.body_pairs[{pair_index}]"
                pair = _program_mapping(raw_pair, path=pair_path)
                _program_closed(
                    pair,
                    required=frozenset({"body_a", "body_b"}),
                    path=pair_path,
                )
                left = _validate_contact_selector(
                    pair["body_a"],
                    path=f"{pair_path}.body_a",
                    component_roles=component_roles,
                    target_roles=target_roles,
                    blackboard_keys=blackboard_keys,
                )
                right = _validate_contact_selector(
                    pair["body_b"],
                    path=f"{pair_path}.body_b",
                    component_roles=component_roles,
                    target_roles=target_roles,
                    blackboard_keys=blackboard_keys,
                )
                kinds = {left[0], right[0]}
                if kinds != {"component_model_body", "runtime_object"}:
                    _program_error(
                        pair_path,
                        "must join one component body to one runtime object",
                    )
                runtime_object = left[1] if left[0] == "runtime_object" else right[1]
                runtime_objects.add(runtime_object)
                pair_keys.append(
                    tuple(
                        sorted(
                            (
                                f"{left[0]}:{left[1]}",
                                f"{right[0]}:{right[1]}",
                            )
                        )
                    )
                )
            if pair_keys != sorted(pair_keys) or len(pair_keys) != len(set(pair_keys)):
                _program_error(
                    f"{zone_path}.body_pairs",
                    "must be duplicate-free and canonical-sorted",
                )
        if zone_ids != sorted(zone_ids) or len(zone_ids) != len(set(zone_ids)):
            _program_error(
                f"{policy_path}.zones",
                "must be unique and canonical-sorted by zone_id",
            )
        if len(runtime_objects) != 1:
            _program_error(
                policy_path,
                "must use one shared declared runtime-object key",
            )
        policy_ids.append(policy_id)
    if policy_ids != sorted(policy_ids) or len(policy_ids) != len(set(policy_ids)):
        _program_error(path, "must be unique and canonical-sorted by policy_id")
    return set(policy_ids)


def _validate_semantic_value(
    schema: CatalogSchema,
    value: Any,
    *,
    path: str,
    context: _ProgramValidationContext,
    node_parameters: Mapping[str, Any],
) -> None:
    concrete = context.catalog.schema_registry.resolve(schema)
    semantic_type = concrete.semantic_type
    if semantic_type is not None:
        if semantic_type == "source_joint":
            _program_text(value, path=path, max_chars=_MAX_IDENTIFIER_CHARS)
        else:
            identity = _program_identifier(value, path=path)
            if semantic_type == "target_role" and identity not in context.target_roles:
                _program_error(path, "does not name a declared target role")
            if (
                semantic_type == "component_role"
                and identity not in context.component_roles
            ):
                _program_error(path, "does not name a declared component role")
            if (
                semantic_type == "scene_resource"
                and identity not in context.scene_resource_ids
            ):
                _program_error(path, "does not name a declared scene resource")
            if (
                semantic_type == "blackboard_key"
                and identity not in context.blackboard_keys
            ):
                _program_error(path, "does not name a declared blackboard key")
            if semantic_type in {
                "frame_role",
                "resource_role",
                "collision_body_role",
                "model_role",
            }:
                role_field = {
                    "frame_role": "required_frame_roles",
                    "resource_role": "required_resource_roles",
                    "collision_body_role": "required_collision_body_roles",
                    "model_role": "required_model_roles",
                }[semantic_type]
                scope_parameter = concrete.semantic_scope_parameter
                if scope_parameter is None:
                    accepted = any(
                        identity in target.get(role_field, ())
                        for target in context.target_roles.values()
                    )
                    scope_description = "any target role"
                else:
                    scope_value = node_parameters.get(scope_parameter)
                    if type(scope_value) is not str:
                        _program_error(
                            path,
                            f"cannot resolve semantic scope from {scope_parameter!r}",
                        )
                    scoped_target = context.target_roles.get(scope_value)
                    accepted = scoped_target is not None and identity in scoped_target.get(
                        role_field,
                        (),
                    )
                    scope_description = f"target role {scope_value!r}"
                if not accepted:
                    _program_error(
                        path,
                        f"does not name a {semantic_type} required by {scope_description}",
                    )
    if concrete.schema_type == "object" and type(value) is dict:
        for name, nested in value.items():
            child_schema = concrete.properties.get(name)
            if child_schema is None:
                child_schema = concrete.additional_property_schema
            if child_schema is not None:
                _validate_semantic_value(
                    child_schema,
                    nested,
                    path=f"{path}.{name}",
                    context=context,
                    node_parameters=node_parameters,
                )
        orientation = value.get("orientation")
        if orientation is not None:
            _validate_orientation(orientation, path=f"{path}.orientation")
        if value.get("type") == "runtime_object" and "runtime_object_id" in value:
            runtime_object = _program_identifier(
                value["runtime_object_id"], path=f"{path}.runtime_object_id"
            )
            declaration = context.blackboard_keys.get(runtime_object)
            if declaration is None or declaration.type != "scene_resource":
                _program_error(
                    f"{path}.runtime_object_id",
                    "must name a scene_resource blackboard key",
                )
    if (
        concrete.schema_type == "array"
        and type(value) is list
        and concrete.items is not None
    ):
        source_joints: list[str] = []
        for index, item in enumerate(value):
            _validate_semantic_value(
                concrete.items,
                item,
                path=f"{path}[{index}]",
                context=context,
                node_parameters=node_parameters,
            )
            item_schema = context.catalog.schema_registry.resolve(concrete.items)
            if (
                item_schema.schema_type == "object"
                and type(item) is dict
                and "joint_name" in item
                and "source_joint"
                in _schema_semantic_types(concrete.items, context.catalog)
            ):
                source_joints.append(str(item["joint_name"]))
        if len(source_joints) != len(set(source_joints)):
            _program_error(path, "contains duplicate source joint names")


def _source_declared_type(
    source: Mapping[str, Any],
    *,
    path: str,
    context: _ProgramValidationContext,
) -> str:
    _program_closed(
        source,
        required=frozenset({"kind", "reference"}),
        optional=frozenset({"field"}),
        path=path,
    )
    kind = source.get("kind")
    if kind not in {"program_parameter", "blackboard_key", "blackboard_field"}:
        _program_error(f"{path}.kind", "has an unsupported value-source kind")
    reference = _program_identifier(source.get("reference"), path=f"{path}.reference")
    if kind == "program_parameter":
        if "field" in source:
            _program_error(path, "program_parameter forbids field")
        declaration = context.parameters.get(reference)
        if declaration is None:
            _program_error(f"{path}.reference", "names no Program parameter")
        return declaration.type
    declaration = context.blackboard_keys.get(reference)
    if declaration is None:
        _program_error(f"{path}.reference", "names no blackboard key")
    if declaration.access not in {"read", "read_write"}:
        _program_error(f"{path}.reference", "does not permit reads")
    if kind == "blackboard_key":
        if "field" in source:
            _program_error(path, "blackboard_key forbids field")
        return declaration.type
    field_name = _program_identifier(source.get("field"), path=f"{path}.field")
    field_declaration = declaration.fields.get(field_name)
    if field_declaration is None:
        _program_error(f"{path}.field", "names no declared field")
    return field_declaration.type


def _source_type_compatible(
    source_type: str,
    destination_schema: CatalogSchema,
    *,
    context: _ProgramValidationContext,
) -> bool:
    destination = context.catalog.schema_registry.resolve(destination_schema)
    if destination.semantic_type == "scene_resource":
        return source_type == "scene_resource"
    if destination.schema_type == "float":
        return source_type == "float64"
    if destination.schema_type == "int":
        return source_type == "int64"
    if destination.schema_type == "bool":
        return source_type == "bool"
    if destination.schema_type in {"string", "enum"}:
        return source_type == "string"
    if destination.schema_type == "array":
        if source_type != "string_list" or destination.items is None:
            return False
        return (
            context.catalog.schema_registry.resolve(destination.items).schema_type
            == "string"
        )
    if destination.schema_type == "object" and source_type == "stored_pose":
        return {"position_m", "orientation"}.issubset(destination.properties)
    return False


def _validate_value_source(
    value: Any,
    *,
    path: str,
    context: _ProgramValidationContext,
    destination_schema: CatalogSchema | None,
) -> str:
    source = _program_mapping(value, path=path)
    source_type = _source_declared_type(source, path=path, context=context)
    if destination_schema is not None and not _source_type_compatible(
        source_type, destination_schema, context=context
    ):
        _program_error(
            path,
            f"source type {source_type!r} is incompatible with the destination schema",
        )
    return source_type


def _validate_nested_value_sources(
    schema: CatalogSchema,
    value: Any,
    *,
    path: str,
    context: _ProgramValidationContext,
) -> None:
    if schema.ref == "#/schema_types/value_source":
        _validate_value_source(
            value,
            path=path,
            context=context,
            destination_schema=None,
        )
        return
    concrete = context.catalog.schema_registry.resolve(schema)
    if concrete.schema_type == "object" and type(value) is dict:
        for name, nested in value.items():
            child_schema = concrete.properties.get(name)
            if child_schema is None:
                child_schema = concrete.additional_property_schema
            if child_schema is not None:
                _validate_nested_value_sources(
                    child_schema,
                    nested,
                    path=f"{path}.{name}",
                    context=context,
                )
    elif (
        concrete.schema_type == "array"
        and type(value) is list
        and concrete.items is not None
    ):
        for index, nested in enumerate(value):
            _validate_nested_value_sources(
                concrete.items,
                nested,
                path=f"{path}[{index}]",
                context=context,
            )


def _effective_parameter_value(
    entry_parameter: Any, parameters: Mapping[str, Any]
) -> Any:
    if entry_parameter.id in parameters:
        return parameters[entry_parameter.id]
    return entry_parameter.default if entry_parameter.has_default else None


def _reject_legacy_parameter_interpolation(value: Any, *, path: str) -> None:
    """Reject the removed v0 string-binding form at the Program authority."""

    pending: list[tuple[Any, str, int]] = [(value, path, 0)]
    visited = 0
    while pending:
        current, current_path, depth = pending.pop()
        visited += 1
        if visited > _MAX_VALUE_NODES:
            _program_error(path, "exceeds the fixed value-node bound")
        if depth > _MAX_VALUE_DEPTH:
            _program_error(current_path, "exceeds the fixed value-depth bound")
        if type(current) is str:
            reference = current[len("${param:") : -1]
            if (
                current.startswith("${param:")
                and current.endswith("}")
                and reference
                and "}" not in reference
            ):
                _program_error(
                    current_path,
                    "legacy ${param:...} interpolation is invalid; use value_sources",
                )
        elif type(current) is list:
            pending.extend(
                (item, f"{current_path}[{index}]", depth + 1)
                for index, item in reversed(tuple(enumerate(current)))
            )
        elif type(current) is dict:
            pending.extend(
                (item, f"{current_path}.{key}", depth + 1)
                for key, item in reversed(tuple(current.items()))
            )


def _validate_node_capabilities(
    entry: Any,
    parameters: Mapping[str, Any],
    *,
    path: str,
    context: _ProgramValidationContext,
) -> None:
    parameter_by_id = {parameter.id: parameter for parameter in entry.parameters}
    for requirement in entry.required_capabilities:
        if requirement.when_parameter is not None:
            selected = _effective_parameter_value(
                parameter_by_id[requirement.when_parameter], parameters
            )
            if selected != requirement.when_equals:
                continue
        if requirement.scope != "target_binding":
            continue
        selector = requirement.target_parameter
        if selector is None:
            _program_error(path, "catalog target capability has no selector")
        target_role = parameters.get(selector)
        if target_role is None:
            target_parameter = parameter_by_id[selector]
            if target_parameter.has_default:
                target_role = target_parameter.default
        if type(target_role) is not str or target_role not in context.target_roles:
            _program_error(
                f"{path}.parameters.{selector}",
                "cannot select a declared target role for capability validation",
            )
        declared = {
            item["capability_id"]
            for item in context.target_roles[target_role]["required_capabilities"]
        }
        if requirement.capability_id not in declared:
            _program_error(
                path,
                f"target role {target_role!r} does not declare required capability "
                f"{requirement.capability_id!r}",
            )


def _validate_catalog_blackboard_bindings(
    entry: Any,
    parameters: Mapping[str, Any],
    *,
    path: str,
    context: _ProgramValidationContext,
) -> None:
    for direction, bindings in (
        ("read", entry.blackboard_reads),
        ("write", entry.blackboard_writes),
    ):
        for binding in bindings:
            key = parameters.get(binding.key_parameter)
            if type(key) is not str:
                _program_error(
                    f"{path}.parameters.{binding.key_parameter}",
                    "must be one literal declared blackboard key",
                )
            declaration = context.blackboard_keys.get(key)
            if declaration is None:
                _program_error(
                    f"{path}.parameters.{binding.key_parameter}",
                    "does not name a declared blackboard key",
                )
            if declaration.type != binding.value_type:
                _program_error(
                    f"{path}.parameters.{binding.key_parameter}",
                    f"declares type {declaration.type!r}, expected {binding.value_type!r}",
                )
            permitted = (
                declaration.access in {"read", "read_write"}
                if direction == "read"
                else declaration.access in {"write", "read_write"}
            )
            if not permitted:
                _program_error(
                    f"{path}.parameters.{binding.key_parameter}",
                    f"does not permit {direction} access",
                )


def _validate_node_cross_fields(
    node_id: str,
    parameters: Mapping[str, Any],
    *,
    path: str,
    context: _ProgramValidationContext,
) -> None:
    goal = parameters.get("goal")
    if node_id == "motion.move_to_joint_target" and type(goal) is list:
        joints = [item.get("joint_name") for item in goal if type(item) is dict]
        if len(joints) != len(set(joints)):
            _program_error(f"{path}.parameters.goal", "contains duplicate joint names")
    if node_id == "motion.move_to_pose":
        mode = parameters.get("target_mode", "absolute")
        if mode == "relative" and "target_reference" not in parameters:
            _program_error(
                f"{path}.parameters.target_reference",
                "is required for relative pose motion",
            )
    if "contact_policy_id" in parameters:
        policy_id = _program_identifier(
            parameters["contact_policy_id"],
            path=f"{path}.parameters.contact_policy_id",
        )
        if policy_id not in context.contact_policy_ids:
            _program_error(
                f"{path}.parameters.contact_policy_id",
                "does not name a declared contact policy",
            )
    position = parameters.get("position")
    effort = parameters.get("max_effort")
    if type(position) is dict and type(effort) is dict:
        compatible = {
            "angular_rad": "angular_newton_meter",
            "linear_m": "linear_newton",
        }
        if compatible.get(position.get("kind")) != effort.get("kind"):
            _program_error(
                f"{path}.parameters.max_effort",
                "unit kind does not match the commanded position",
            )


def _validate_program_tree(
    value: Any,
    *,
    path: str,
    depth: int,
    node_budget: list[int],
    context: _ProgramValidationContext,
) -> ProgramTreeNode:
    if depth > _MAX_TREE_DEPTH:
        _program_error(path, "exceeds the fixed tree-depth bound")
    node_budget[0] -= 1
    if node_budget[0] < 0:
        _program_error(path, "exceeds the fixed tree-node bound")
    node = _program_mapping(value, path=path)
    _program_closed(
        node,
        required=frozenset({"node_id", "instance_id", "parameters", "children"}),
        optional=frozenset(
            {
                "metadata",
                "value_sources",
                "modification_policy",
                "recovery_policy",
                "operator_stop_policy",
            }
        ),
        path=path,
    )
    node_id = _program_identifier(node["node_id"], path=f"{path}.node_id")
    instance_id = _program_identifier(node["instance_id"], path=f"{path}.instance_id")
    if instance_id in context.tree_instance_ids:
        _program_error(f"{path}.instance_id", "duplicates another tree instance")
    context.tree_instance_ids.add(instance_id)
    try:
        entry = context.catalog.get(node_id)
    except KeyError:
        _program_error(f"{path}.node_id", "is absent from the active v2 catalog")
    if entry.min_program_profile != _PROGRAM_PROFILE:
        _program_error(path, "requires an unsupported Program profile")

    parameters = _program_mapping(node["parameters"], path=f"{path}.parameters")
    if len(parameters) > _MAX_MAPPING_ITEMS:
        _program_error(f"{path}.parameters", "exceeds the fixed item bound")
    for parameter_id in parameters:
        _program_identifier(parameter_id, path=f"{path}.parameters key")

    raw_sources = node.get("value_sources", {})
    value_sources = _program_mapping(raw_sources, path=f"{path}.value_sources")
    if "value_sources" in node and not value_sources:
        _program_error(f"{path}.value_sources", "must be omitted when empty")
    if len(value_sources) > _MAX_MAPPING_ITEMS:
        _program_error(f"{path}.value_sources", "exceeds the fixed item bound")
    _canonical_mapping_keys(value_sources, path=f"{path}.value_sources")

    catalog_parameters = {parameter.id: parameter for parameter in entry.parameters}
    unknown = sorted((set(parameters) | set(value_sources)) - set(catalog_parameters))
    if unknown:
        _program_error(path, f"supplies unknown catalog parameters {unknown}")
    duplicate = sorted(set(parameters) & set(value_sources))
    if duplicate:
        _program_error(path, f"supplies parameters twice {duplicate}")

    condition_parameters = {
        requirement.when_parameter
        for requirement in entry.required_capabilities
        if requirement.when_parameter is not None
    }
    target_parameters = {
        requirement.target_parameter
        for requirement in entry.required_capabilities
        if requirement.target_parameter is not None
    }
    component_parameters = {
        requirement.component_parameter
        for requirement in entry.required_capabilities
        if requirement.component_parameter is not None
    }
    selector_parameters = (
        condition_parameters | target_parameters | component_parameters
    )
    effective_parameters = {
        parameter_id: catalog_parameter.default
        for parameter_id, catalog_parameter in catalog_parameters.items()
        if catalog_parameter.has_default
    }
    effective_parameters.update(parameters)

    for parameter_id, catalog_parameter in catalog_parameters.items():
        supplied_literal = parameter_id in parameters
        supplied_source = parameter_id in value_sources
        if (
            catalog_parameter.required
            and not catalog_parameter.has_default
            and not supplied_literal
            and not supplied_source
        ):
            _program_error(
                path,
                f"is missing required catalog parameter {parameter_id!r}",
            )
        if supplied_literal:
            _reject_legacy_parameter_interpolation(
                parameters[parameter_id],
                path=f"{path}.parameters.{parameter_id}",
            )
            issues = validate_value(
                catalog_parameter.schema,
                parameters[parameter_id],
                registry=context.catalog.schema_registry,
                path=f"{path}.parameters.{parameter_id}",
            )
            if issues:
                _program_error(issues[0].path, issues[0].message)
            _validate_semantic_value(
                catalog_parameter.schema,
                parameters[parameter_id],
                path=f"{path}.parameters.{parameter_id}",
                context=context,
                node_parameters=effective_parameters,
            )
            _validate_nested_value_sources(
                catalog_parameter.schema,
                parameters[parameter_id],
                path=f"{path}.parameters.{parameter_id}",
                context=context,
            )
        if supplied_source:
            semantic_types = _schema_semantic_types(
                catalog_parameter.schema, context.catalog
            )
            if (
                parameter_id in _DYNAMIC_SOURCE_FORBIDDEN_PARAMETERS
                or parameter_id in selector_parameters
                or "source_joint" in semantic_types
                or context.catalog.schema_registry.resolve(
                    catalog_parameter.schema
                ).semantic_type
                in _DYNAMIC_SOURCE_FORBIDDEN_SEMANTICS
            ):
                _program_error(
                    f"{path}.value_sources.{parameter_id}",
                    "cannot dynamically select runtime/binding semantics",
                )
            _validate_value_source(
                value_sources[parameter_id],
                path=f"{path}.value_sources.{parameter_id}",
                context=context,
                destination_schema=catalog_parameter.schema,
            )

    _validate_node_capabilities(entry, parameters, path=path, context=context)
    _validate_catalog_blackboard_bindings(entry, parameters, path=path, context=context)
    _validate_node_cross_fields(node_id, parameters, path=path, context=context)

    raw_children = _program_list(node["children"], path=f"{path}.children")
    if len(raw_children) < entry.children_min or len(raw_children) > entry.children_max:
        _program_error(
            f"{path}.children",
            f"must contain {entry.children_min}-{entry.children_max} children",
        )
    children = tuple(
        _validate_program_tree(
            child,
            path=f"{path}.children[{index}]",
            depth=depth + 1,
            node_budget=node_budget,
            context=context,
        )
        for index, child in enumerate(raw_children)
    )
    (
        modification_policy,
        recovery_policy,
        operator_stop_policy,
    ) = _normalize_canonical_node_policies(node, path=path)
    metadata: Mapping[str, Any] | None = None
    if "metadata" in node:
        metadata_row = _program_mapping(node["metadata"], path=f"{path}.metadata")
        canonical_json_bytes(metadata_row)
        metadata = _freeze_mapping(metadata_row)
    return ProgramTreeNode(
        node_id=node_id,
        instance_id=instance_id,
        parameters=_freeze_mapping(parameters),
        value_sources=MappingProxyType(
            {
                key: _freeze_mapping(
                    _program_mapping(value, path=f"{path}.value_sources.{key}")
                )
                for key, value in value_sources.items()
            }
        ),
        children=children,
        metadata=metadata,
        modification_policy=(
            _freeze_mapping(modification_policy)
            if modification_policy is not None
            else None
        ),
        recovery_policy=(
            _freeze_mapping(recovery_policy)
            if recovery_policy is not None
            else None
        ),
        operator_stop_policy=operator_stop_policy,
    )


def _validate_program_document(
    data: dict[str, Any],
    *,
    catalog: NodeCatalog,
    source: str,
) -> tuple[
    dict[str, Any],
    ProgramTreeNode,
    dict[str, ProgramParameterDeclaration],
    dict[str, BlackboardKeyDeclaration],
]:
    root = _program_mapping(data, path=source)
    root = _select_identity_verified_program_root(root, source=source)
    _program_closed(
        root,
        required=frozenset(
            {
                "api_version",
                "program_id",
                "program_revision",
                "program_profile",
                "name",
                "catalog",
                "component_roles",
                "target_roles",
                "tree",
            }
        ),
        optional=_TOP_LEVEL_FIELDS,
        path=source,
    )
    if root["api_version"] != _PROGRAM_API_VERSION:
        _program_error(f"{source}.api_version", f"must be {_PROGRAM_API_VERSION}")
    _program_identifier(root["program_id"], path=f"{source}.program_id")
    if root["program_profile"] != _PROGRAM_PROFILE:
        _program_error(f"{source}.program_profile", f"must be {_PROGRAM_PROFILE}")
    _program_text(
        root["name"],
        path=f"{source}.name",
        max_chars=_MAX_DESCRIPTION_CHARS,
    )
    if "description" in root:
        _program_text(
            root["description"],
            path=f"{source}.description",
            allow_empty=True,
            max_chars=_MAX_DESCRIPTION_CHARS,
        )
    catalog_ref = _program_mapping(root["catalog"], path=f"{source}.catalog")
    _program_closed(
        catalog_ref,
        required=frozenset({"api_version", "content_version"}),
        path=f"{source}.catalog",
    )
    if (
        catalog_ref["api_version"] != _CATALOG_API_VERSION
        or catalog_ref["api_version"] != catalog.api_version
        or catalog_ref["content_version"] != catalog.content_version
    ):
        _program_error(
            f"{source}.catalog",
            "does not select the exact active v2 catalog version",
        )
    if "compatibility" in root:
        _validate_compatibility(root["compatibility"], path=f"{source}.compatibility")
    component_roles = _validate_component_roles(
        root["component_roles"], path=f"{source}.component_roles"
    )
    target_roles = _validate_target_roles(
        root["target_roles"],
        path=f"{source}.target_roles",
        component_roles=component_roles,
    )
    (
        scene_resource_ids,
        scene_resource_roles,
        scene_collision_policy_refs,
    ) = _validate_scene(
        root.get("scene", []),
        path=f"{source}.scene",
        catalog=catalog,
        component_roles=component_roles,
    )
    if "viewer" in root:
        _validate_viewer(root["viewer"], path=f"{source}.viewer")
        _validate_viewer_references(
            root["viewer"],
            path=f"{source}.viewer",
            target_roles=target_roles,
            scene_resource_roles=scene_resource_roles,
        )
    parameters = _validate_parameters(
        root.get("parameters", []),
        path=f"{source}.parameters",
        scene_resource_ids=scene_resource_ids,
        target_roles=target_roles,
    )
    namespaces, blackboard_keys = _validate_blackboard(
        root.get("blackboard", {"namespaces": [], "keys": []}),
        path=f"{source}.blackboard",
    )
    contact_policy_ids = _validate_contact_policies(
        root.get("contact_policies", []),
        path=f"{source}.contact_policies",
        component_roles=component_roles,
        target_roles=target_roles,
        blackboard_keys=blackboard_keys,
    )
    unknown_scene_policies = scene_collision_policy_refs - contact_policy_ids
    if unknown_scene_policies:
        _program_error(
            f"{source}.scene",
            "references undeclared collision policies "
            f"{sorted(unknown_scene_policies)}",
        )
    context = _ProgramValidationContext(
        catalog=catalog,
        component_roles=component_roles,
        target_roles=target_roles,
        scene_resource_ids=scene_resource_ids,
        parameters=parameters,
        blackboard_namespaces=namespaces,
        blackboard_keys=blackboard_keys,
        contact_policy_ids=contact_policy_ids,
    )
    tree = _validate_program_tree(
        root["tree"],
        path=f"{source}.tree",
        depth=0,
        node_budget=[_MAX_TREE_NODES],
        context=context,
    )
    normalized_root = deepcopy(root)
    normalized_root["tree"] = tree.to_dict()
    try:
        verify_content_identity(
            normalized_root,
            own_field="program_revision",
            identity_domain="program-revision",
            api_version=_PROGRAM_API_VERSION,
            label=source,
        )
    except StrictArtifactError as exc:
        _program_error(f"{source}.program_revision", str(exc))
    return normalized_root, tree, parameters, blackboard_keys


class Program:
    """Validated portable ``manyforge.program.v1`` authoring artifact.

    This loader owns no binding, runtime, filesystem discovery, node factory,
    or deployment-capability union. It validates authored meaning against one
    already verified v2 catalog; exact retained coordinates remain the binding
    phase's responsibility.
    """

    def __init__(
        self,
        path: str | Path,
        catalog: NodeCatalog,
        *,
        skill_catalog: SkillCatalog | None = None,
        deployment_capability_keys: set[str] | None = None,
    ) -> None:
        if skill_catalog is not None:
            raise ProgramValidationError(
                "v0 skill expansion is unavailable; v1 Programs contain only "
                "ordinary active-catalog nodes"
            )
        if deployment_capability_keys is not None:
            raise ProgramValidationError(
                "deployment-wide capability unions cannot validate a v1 Program"
            )
        source_path = Path(path)
        try:
            data = load_strict_json_yaml(source_path)
        except StrictArtifactError as exc:
            raise ProgramValidationError(str(exc)) from exc
        self._initialize(data, catalog=catalog, source=str(source_path))
        self.source_path: Path | None = source_path

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
        if skill_catalog is not None:
            raise ProgramValidationError(
                "v0 skill expansion is unavailable; v1 Programs contain only "
                "ordinary active-catalog nodes"
            )
        if deployment_capability_keys is not None:
            raise ProgramValidationError(
                "deployment-wide capability unions cannot validate a v1 Program"
            )
        if type(data) is not dict:
            raise ProgramValidationError("Program root must be a built-in mapping")
        try:
            canonical_json_bytes(data)
        except StrictArtifactError as exc:
            raise ProgramValidationError(str(exc)) from exc
        self = cls.__new__(cls)
        self._initialize(deepcopy(data), catalog=catalog, source=str(path))
        self.source_path = None if str(path) == "<memory>" else Path(path)
        return self

    def _initialize(
        self,
        data: dict[str, Any],
        *,
        catalog: NodeCatalog,
        source: str,
    ) -> None:
        try:
            normalized, tree, parameters, blackboard_keys = _validate_program_document(
                data,
                catalog=catalog,
                source=source,
            )
            canonical = canonical_json_bytes(normalized)
        except ProgramValidationError:
            raise
        except StrictArtifactError as exc:
            raise ProgramValidationError(str(exc)) from exc
        self._data = normalized
        self._catalog = catalog
        self.program_id = str(normalized["program_id"])
        self.program_revision = str(normalized["program_revision"])
        self.program_profile = str(normalized["program_profile"])
        self.name = str(normalized["name"])
        self.description = str(normalized.get("description", ""))
        self.catalog_api_version = str(normalized["catalog"]["api_version"])
        self.catalog_content_version = str(normalized["catalog"]["content_version"])
        self.validated_catalog_digest = catalog.catalog_digest
        self.tree = tree
        self.parameters = MappingProxyType(dict(parameters))
        self.blackboard_keys = MappingProxyType(dict(blackboard_keys))
        self.canonical_bytes = canonical

    def to_dict(self) -> dict[str, Any]:
        """Return one detached exact v1 authoring document."""

        return deepcopy(self._data)


__all__ = [
    "BlackboardKey",
    "BlackboardFieldDeclaration",
    "BlackboardKeyDeclaration",
    "Program",
    "ProgramNodePolicy",
    "ProgramParameterDeclaration",
    "ProgramTreeNode",
    "ProgramValidationError",
    "TreeNode",
    "apply_tree_param_overrides",
    "compute_program_tree_hash",
    "find_tree_node_by_name",
    "merge_param_patch",
    "program_node_policies",
    "require_exact_program_binding_node_policies",
    "tree_node_from_dict",
]
