"""Bounded tree-document helpers and the fenced I6/N Program client.

The former v0 Program parser/validator, top-level robot/scene/singleton schema,
mutable catalog coupling, and file writer are absent.  These transport-neutral
tree values remain useful to retained clients while the native Program and
addressing contract is qualified.
"""

from __future__ import annotations

import hashlib
import json
import math
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Final, Mapping, NoReturn

from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.skill_loader import SkillCatalog


PROGRAM_ARTIFACT_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_program_artifact_unavailable"
)
PROGRAM_RESTORATION_GATES: Final = ("I6", "N")
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


class ProgramValidationError(ValueError):
    """A retained tree-document value is malformed."""


class ProgramArtifactUnavailableError(ProgramValidationError):
    """The native Program/addressing client has not passed I6/N."""


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


def _raise_program_artifact_unavailable() -> NoReturn:
    raise ProgramArtifactUnavailableError(
        f"{PROGRAM_ARTIFACT_UNAVAILABLE_REASON_CODE}: the v0 Program schema "
        "and mutable catalog validator were removed; authoring returns at I6/N"
    )


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
_MODIFICATION_POLICY_FIELDS: Final = frozenset(
    _MODIFICATION_POLICY_ALLOWED_VALUES
)
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
        if (
            type(retries) is not int
            or retries < 0
            or retries > _MAX_RECOVERY_RETRIES
        ):
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
            raise ProgramValidationError(
                f"{current_path} must be a built-in mapping"
            )
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


class Program:
    """Stable I6/N reconnection marker; it performs no file or catalog I/O."""

    def __init__(
        self,
        path: str | Path,
        catalog: NodeCatalog,
        *,
        skill_catalog: SkillCatalog | None = None,
        deployment_capability_keys: set[str] | None = None,
    ) -> None:
        del path, catalog, skill_catalog, deployment_capability_keys
        _raise_program_artifact_unavailable()

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
        del cls, data, catalog, path, skill_catalog, deployment_capability_keys
        _raise_program_artifact_unavailable()


__all__ = [
    "BlackboardKey",
    "Program",
    "ProgramArtifactUnavailableError",
    "ProgramValidationError",
    "TreeNode",
    "apply_tree_param_overrides",
    "compute_program_tree_hash",
    "find_tree_node_by_name",
    "merge_param_patch",
    "tree_node_from_dict",
]
