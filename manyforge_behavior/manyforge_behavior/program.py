"""Strict v2 Program authoring and bounded legacy-neutral tree helpers.

The loader validates portable authored meaning against one independently
verified v2 catalog. It does not restore the former v0 singleton schema,
runtime factories, deployment-wide capability union, or binding authority.
"""

from __future__ import annotations

import hashlib
import json
import math
import re
from copy import copy, deepcopy
from dataclasses import dataclass, field
from pathlib import Path
from types import MappingProxyType
from typing import Any, Final, Iterable, Mapping, NoReturn

from manyforge_behavior.canonical_artifact import (
    StrictArtifactError,
    canonical_json_bytes,
    compute_content_identity,
    load_strict_json_yaml,
    verify_content_identity,
)
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.interrupt_condition import (
    InterruptConditionError,
    check_condition,
    condition_keys,
    condition_signals,
)
from manyforge_behavior.catalog_schema import (
    CatalogSchema,
    CatalogSchemaRegistry,
    validate_value,
)
from manyforge_behavior.nodes.logic import comparison_supported
from manyforge_behavior.program_expressions import (
    DESTINATION_TYPES,
    CheckedExpression,
    ComputedKeyCycleError,
    ExpressionEnvironment,
    ExpressionError,
    computed_key_order,
    describe_diagnostic,
)
from manyforge_behavior.value_path import (
    Segment,
    ValuePathError,
    parse_value_path,
    value_at_path,
)
from manyforge_behavior.skill_loader import SkillCatalog

_MAX_TREE_DEPTH: Final = 256
_MAX_TREE_NODES: Final = 100_000
_MAX_CHILDREN_PER_NODE: Final = 10_000
_MAX_IDENTIFIER_CHARS: Final = 256
_MAX_DESCRIPTION_CHARS: Final = 4096
_MAX_VALUE_DEPTH: Final = 32
_MAX_VALUE_NODES: Final = 250_000
_MAX_MAPPING_ITEMS: Final = 4096
_MAX_SEQUENCE_ITEMS: Final = 10_000
_MAX_VALUE_STRING_CHARS: Final = 1_000_000
_MAX_DOCUMENT_TEXT_CHARS: Final = 8 * 1024 * 1024
# Program integer literals stay exact through canonical JSON: anything
# outside ±(2^53−1) is refused at parse, never rounded later.
MAX_SAFE_INTEGER_LITERAL: Final = 9_007_199_254_740_991
_MAX_CANONICAL_BYTES: Final = 96 * 1024 * 1024
_PROGRAM_API_VERSION: Final = "manyforge.program.v2"
_PREVIOUS_PROGRAM_API_VERSION: Final = "manyforge.program.v1"
_MAX_EXECUTION_GROUPS: Final = 4096
_MAX_RESOLVED_GROUP_MEMBERSHIPS: Final = _MAX_TREE_NODES
_MAX_RESOLVED_TARGET_EFFECTS: Final = _MAX_TREE_NODES
_PROGRAM_PROFILE: Final = "composable-cell-v1"
_CATALOG_API_VERSION: Final = "manyforge.node_catalog.v2"
_IDENTIFIER_RE: Final = re.compile(r"^[a-z][a-z0-9_.-]*$")
_MAX_PROGRAM_ROLES: Final = 4096
_MAX_PROGRAM_DECLARATIONS: Final = 100_000
_PROGRAM_MOTION_DEFAULTS: Final = frozenset(
    {
        "velocity_scale",
        "acceleration_scale",
        "translation_tol_m",
        "max_blend_deviation_rad",
    }
)
_PROGRAM_MOTION_CEILINGS: Final = frozenset(
    {"velocity_scale", "acceleration_scale"}
)


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


def _require_safe_integer_literal(value: int, *, path: str) -> None:
    if not -MAX_SAFE_INTEGER_LITERAL <= value <= MAX_SAFE_INTEGER_LITERAL:
        raise ProgramValidationError(
            f"{path} integer literal is outside the exact range ±(2^53−1) "
            f"(±{MAX_SAFE_INTEGER_LITERAL})"
        )


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
        _require_safe_integer_literal(value, path=path)
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
    recovery_policy: dict[str, Any] = field(default_factory=dict)
    operator_stop_policy: str = "inherit"
    execution_groups: str | tuple[str, ...] = "inherit"

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
        self.recovery_policy = _parse_recovery_policy(
            self.recovery_policy,
            path="tree",
        )
        self.operator_stop_policy = _parse_operator_stop_policy(
            self.operator_stop_policy,
            path="tree",
        )
        self.execution_groups = parse_execution_groups(
            self.execution_groups, path="tree"
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


OPERATOR_STOP_POLICY_VALUES: Final = frozenset({"inherit", "hold", "continue"})
# The authored recovery policy has two fields: an explicit opt-in to failure
# intervention (default off) and the explicit failure-retry budget, where zero
# means Unlimited (never "disabled"). The intervention stop scope is optional
# and defaults to all groups; the canonical form omits the default.
_RECOVERY_POLICY_FIELDS: Final = frozenset({"allow_intervention", "max_retries"})
_RECOVERY_POLICY_ALLOWED_FIELDS: Final = _RECOVERY_POLICY_FIELDS | {
    "intervention_stop_scope"
}
INTERVENTION_STOP_SCOPE_VALUES: Final = frozenset({"all", "own_groups"})
DEFAULT_INTERVENTION_STOP_SCOPE: Final = "all"
MAX_RECOVERY_POLICY_RETRIES: Final = 255
_TREE_FIELDS: Final = frozenset(
    {
        "id",
        "name",
        "params",
        "children",
        "recovery_policy",
        "operator_stop_policy",
        "execution_groups",
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


def parse_execution_groups(raw: Any, *, path: str) -> str | tuple[str, ...]:
    """Normalize one closed authoring value; no runtime group state lives here."""

    if type(raw) is str and raw in {"inherit", "all"}:
        return raw
    if (
        type(raw) not in {list, tuple}
        or not 1 <= len(raw) <= _MAX_EXECUTION_GROUPS
        or any(
            type(name) is not str
            or len(name) > 128
            or _IDENTIFIER_RE.fullmatch(name) is None
            or name in {"inherit", "all"}
            for name in raw
        )
        or len(set(raw)) != len(raw)
    ):
        raise ProgramValidationError(
            f"{path}.execution_groups must be inherit, all, or a bounded nonempty "
            "set of unique portable group IDs"
        )
    return tuple(sorted(raw))


def _execution_groups_document(groups: str | tuple[str, ...]) -> str | list[str]:
    return groups if isinstance(groups, str) else list(groups)


def _parse_recovery_policy(raw_policy: Any, *, path: str) -> dict[str, Any]:
    if raw_policy is None:
        return {}
    policy = _validate_closed_mapping(
        raw_policy,
        allowed=_RECOVERY_POLICY_ALLOWED_FIELDS,
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
    if "max_retries" in policy:
        retries = policy["max_retries"]
        if (
            type(retries) is not int
            or not 0 <= retries <= MAX_RECOVERY_POLICY_RETRIES
        ):
            raise ProgramValidationError(
                f"{path}.recovery_policy.max_retries must be an integer in "
                f"0..{MAX_RECOVERY_POLICY_RETRIES} (0 is Unlimited)"
            )
        parsed["max_retries"] = retries
    if "intervention_stop_scope" in policy:
        scope = _intervention_stop_scope(
            policy["intervention_stop_scope"],
            path=f"{path}.recovery_policy",
        )
        if scope != DEFAULT_INTERVENTION_STOP_SCOPE:
            parsed["intervention_stop_scope"] = scope
    return parsed


def _intervention_stop_scope(value: Any, *, path: str) -> str:
    if type(value) is not str or value not in INTERVENTION_STOP_SCOPE_VALUES:
        allowed = " or ".join(sorted(INTERVENTION_STOP_SCOPE_VALUES))
        raise ProgramValidationError(
            f"{path}.intervention_stop_scope must be {allowed}"
        )
    return value


CANONICAL_DEFAULT_RECOVERY_POLICY: Final = {
    "allow_intervention": False,
    "max_retries": 0,
}


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
        allowed=_RECOVERY_POLICY_ALLOWED_FIELDS,
        path=f"{path}.recovery_policy",
    )
    if not _RECOVERY_POLICY_FIELDS <= set(policy):
        raise ProgramValidationError(
            f"{path}.recovery_policy must contain at least "
            f"{sorted(_RECOVERY_POLICY_FIELDS)}"
        )
    allow_intervention = policy["allow_intervention"]
    if type(allow_intervention) is not bool:
        raise ProgramValidationError(
            f"{path}.recovery_policy.allow_intervention must be boolean"
        )
    max_retries = policy["max_retries"]
    if (
        type(max_retries) is not int
        or not 0 <= max_retries <= MAX_RECOVERY_POLICY_RETRIES
    ):
        raise ProgramValidationError(
            f"{path}.recovery_policy.max_retries must be an integer in "
            f"0..{MAX_RECOVERY_POLICY_RETRIES} (0 is Unlimited)"
        )
    canonical = {"allow_intervention": allow_intervention, "max_retries": max_retries}
    scope = _intervention_stop_scope(
        policy.get("intervention_stop_scope", DEFAULT_INTERVENTION_STOP_SCOPE),
        path=f"{path}.recovery_policy",
    )
    if scope != DEFAULT_INTERVENTION_STOP_SCOPE:
        canonical["intervention_stop_scope"] = scope
    return canonical


def _normalize_canonical_node_policies(
    node: Mapping[str, Any],
    *,
    path: str,
) -> tuple[dict[str, Any] | None, str | None]:
    if "recovery_policy" in node:
        recovery = _parse_canonical_recovery_policy(node["recovery_policy"], path=path)
    else:
        recovery = dict(CANONICAL_DEFAULT_RECOVERY_POLICY)
    if "operator_stop_policy" in node:
        operator_stop = node["operator_stop_policy"]
        if (
            type(operator_stop) is not str
            or operator_stop not in OPERATOR_STOP_POLICY_VALUES
        ):
            raise ProgramValidationError(
                f"{path}.operator_stop_policy must be inherit, hold, or continue"
            )
    else:
        operator_stop = "inherit"
    normalized_recovery = (
        None if recovery == CANONICAL_DEFAULT_RECOVERY_POLICY else recovery
    )
    normalized_stop = None if operator_stop == "inherit" else operator_stop
    return normalized_recovery, normalized_stop


def _remove_explicit_default_policy_spelling(
    root: dict[str, Any],
    *,
    execution_groups_supported: bool = False,
) -> dict[str, Any]:
    """Remove only the exact source spellings permitted by C1 migration."""

    def is_default_recovery(value: Any) -> bool:
        return (
            type(value) is dict
            and set(value) == _RECOVERY_POLICY_FIELDS
            and type(value["allow_intervention"]) is bool
            and value["allow_intervention"] is False
            and type(value["max_retries"]) is int
            and value["max_retries"] == 0
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
        if execution_groups_supported and "execution_groups" in migrated:
            groups = parse_execution_groups(migrated["execution_groups"], path="tree")
            if groups == "inherit":
                del migrated["execution_groups"]
            else:
                migrated["execution_groups"] = _execution_groups_document(groups)
        recovery = migrated.get("recovery_policy")
        if (
            type(recovery) is dict
            and recovery.get("intervention_stop_scope") == DEFAULT_INTERVENTION_STOP_SCOPE
        ):
            recovery = {
                key: value
                for key, value in recovery.items()
                if key != "intervention_stop_scope"
            }
            migrated["recovery_policy"] = recovery
        if is_default_recovery(recovery):
            del migrated["recovery_policy"]
        if (
            type(migrated.get("operator_stop_policy")) is str
            and migrated["operator_stop_policy"] == "inherit"
        ):
            del migrated["operator_stop_policy"]
        children = migrated.get("children")
        if type(children) is list:
            if len(children) > _MAX_CHILDREN_PER_NODE:
                raise ProgramValidationError("tree child count exceeds bounded maximum")
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
    api_version: str = _PROGRAM_API_VERSION,
) -> dict[str, Any]:
    """Verify identity before semantic validation, with one bounded migration."""

    # Preserve the established shape diagnostics for documents that do not
    # contain a usable identity field at all.  A stale, otherwise well-shaped
    # identity must not be hidden by authored semantic errors.
    if (
        root.get("api_version") != api_version
        or type(root.get("program_revision")) is not str
        or re.fullmatch(r"sha256:[0-9a-f]{64}", root["program_revision"]) is None
    ):
        return root
    try:
        verify_content_identity(
            root,
            own_field="program_revision",
            identity_domain="program-revision",
            api_version=api_version,
            label=source,
        )
        return root
    except StrictArtifactError as raw_identity_error:
        if not str(raw_identity_error).startswith(
            f"{source}.program_revision: content identity mismatch;"
        ):
            _program_error(f"{source}.program_revision", str(raw_identity_error))
        try:
            candidate = _remove_explicit_default_policy_spelling(
                root, execution_groups_supported=api_version == _PROGRAM_API_VERSION
            )
        except ProgramValidationError:
            _program_error(f"{source}.program_revision", str(raw_identity_error))
        try:
            verify_content_identity(
                candidate,
                own_field="program_revision",
                identity_domain="program-revision",
                api_version=api_version,
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
        execution_groups = parse_execution_groups(node.execution_groups, path=path)
        for group in (
            (execution_groups,)
            if isinstance(execution_groups, str)
            else execution_groups
        ):
            _bounded_text(group, path=f"{path}.execution_groups", budget=budget)
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
            if execution_groups != "inherit":
                document["execution_groups"] = _execution_groups_document(
                    execution_groups
                )
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
            execution_groups = parse_execution_groups(
                document.get("execution_groups", "inherit"), path=current_path
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
                recovery_policy=recovery_policy,
                operator_stop_policy=operator_stop_policy,
                execution_groups=execution_groups,
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
                recovery_policy=recovery_policy,
                operator_stop_policy=operator_stop_policy,
                execution_groups=current.execution_groups,
            )
        finally:
            ancestors.remove(identity)

    return rebuild(node, path="tree", depth=0)


@dataclass(frozen=True, slots=True)
class ProgramTreeNode:
    """One validated node instance with portable authored values only."""

    node_id: str
    instance_id: str
    parameters: Mapping[str, Any]
    value_sources: Mapping[str, Mapping[str, Any]]
    children: tuple["ProgramTreeNode", ...]
    metadata: Mapping[str, Any] | None = None
    recovery_policy: Mapping[str, Any] | None = None
    operator_stop_policy: str | None = None
    execution_groups: str | tuple[str, ...] = "inherit"

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
        if self.recovery_policy is not None:
            result["recovery_policy"] = deepcopy(dict(self.recovery_policy))
        if self.operator_stop_policy is not None:
            result["operator_stop_policy"] = self.operator_stop_policy
        if self.execution_groups != "inherit":
            result["execution_groups"] = _execution_groups_document(
                self.execution_groups
            )
        return result


def _resolve_execution_selections(
    tree: ProgramTreeNode,
) -> Mapping[str, tuple[str, ...]]:
    """Compile bounded selection membership once, without any hold state."""

    members: dict[str, list[str]] = {"all": []}
    pending = [(tree, ())]
    memberships = 0
    while pending:
        node, inherited = pending.pop()
        groups = (
            inherited
            if node.execution_groups == "inherit"
            else () if node.execution_groups == "all" else node.execution_groups
        )
        assert isinstance(groups, tuple)
        members["all"].append(node.instance_id)
        memberships += len(groups)
        if memberships > _MAX_RESOLVED_GROUP_MEMBERSHIPS:
            raise ProgramValidationError(
                "resolved execution-group memberships exceed the node bound"
            )
        for name in groups:
            members.setdefault(name, []).append(node.instance_id)
        if len(members) > _MAX_EXECUTION_GROUPS + 1:
            raise ProgramValidationError("execution groups exceed the group bound")
        pending.extend((child, groups) for child in reversed(node.children))
    return MappingProxyType(
        {name: tuple(sorted(nodes)) for name, nodes in sorted(members.items())}
    )


# The three plain motion nodes: the Program grammar's ordinary motion nodes
# (execution groups, automatic retry, and the owner's motion routing).
MOTION_NODE_IDS: Final = frozenset(
    {
        "motion.move_to_joint_target",
        "motion.move_to_pose",
        "motion.follow_cartesian_path",
    }
)


# The two interrupt motion nodes: each carries one per-node interrupt
# condition (schema02 §2, schema03).
INTERRUPT_MOTION_NODE_IDS: Final = frozenset(
    {"motion.move_until_signal", "motion.probe_search"}
)
# Signal-reading io nodes: their one signal is (target_role, resource_role).
IO_SIGNAL_INPUT_NODE_IDS: Final = frozenset(
    {"io.get_input", "io.check_input", "io.wait_for_input"}
)
MOCK_SIMULATION_NODE_IDS: Final = INTERRUPT_MOTION_NODE_IDS | IO_SIGNAL_INPUT_NODE_IDS
# Node Mock simulation refusals (contract item 133, spec 410 §4.5).
MOCK_SIMULATION_INVALID: Final = "mock_simulation_invalid"
MOCK_SIMULATION_SIGNAL_UNUSED: Final = "mock_simulation_signal_unused"
MOCK_SIMULATION_SIGNAL_DUPLICATE: Final = "mock_simulation_signal_duplicate"
MOCK_SIMULATION_SOURCE_REFUSED: Final = "mock_simulation_value_source_refused"

# System-owned node outcome keys (contract items 42, 46(f), 105): every
# motion node's outcome lives in `node_outcome.<instance_id>`, declared
# implicitly, readable like any run key and written only by that node when it
# ends. The system defines its start value: `none` (no result yet in this Run)
# at Run start and after Reset; afterwards it keeps the last outcome (43).
NODE_OUTCOME_NODE_IDS: Final = MOTION_NODE_IDS | INTERRUPT_MOTION_NODE_IDS
NODE_OUTCOME_NAMESPACE: Final = "node_outcome"
NODE_OUTCOME_NONE: Final = "none"
# The outcomes a node writes when it ends (never `none`).
NODE_ENDED_OUTCOMES: Final = (
    "completed",
    "failed",
    "preempted",
    "superseded",
    "refused_by_priority",
    "signal_trip",
    "no_signal_trip",
)
# Every value a node outcome key can hold.
NODE_OUTCOME_VALUES: Final = (*NODE_ENDED_OUTCOMES, NODE_OUTCOME_NONE)


def node_outcome_key(instance_id: str) -> str:
    """The reserved blackboard key holding one motion node's last outcome."""

    return f"{NODE_OUTCOME_NAMESPACE}.{instance_id}"


def is_node_outcome_key(key: str) -> bool:
    return key.startswith(f"{NODE_OUTCOME_NAMESPACE}.")


# System-owned global-rule state keys (contract items 63, 64): each Deployment
# global rule publishes `interrupt_state.<rule_id>`, read-only, readable like
# any key. Reserved like node_outcome: never authored nor written by nodes.
# A Program alone cannot see the Deployment, so its validation declares the
# key of each rule id it references (read-only; writes stay refused). The join
# against the Deployment's declared rules is where both are known: the native
# Apply validation view (rules from the retained bundle), the Runner's
# with_interrupt_state_keys (Applied rule ids) and the Composer GUI (Draft
# rules); `require_interrupt_state_rules` is the Python form of that check.
INTERRUPT_STATE_NAMESPACE: Final = "interrupt_state"
INTERRUPT_STATE_VALUES: Final = ("clear", "tripped", "latched")


def is_interrupt_state_key(key: str) -> bool:
    return key.startswith(f"{INTERRUPT_STATE_NAMESPACE}.")


def interrupt_state_key(rule_id: str) -> str:
    """The reserved key mirroring one global rule's Kernel state (item 64)."""

    return f"{INTERRUPT_STATE_NAMESPACE}.{rule_id}"


def interrupt_state_declaration(rule_id: str) -> BlackboardKeyDeclaration:
    """Read-only, system-written, always current (also between Runs): the
    Runner's mirror is its only writer and Reset never clears it."""

    if type(rule_id) is not str or _IDENTIFIER_RE.fullmatch(rule_id) is None:
        raise ProgramValidationError(f"global rule id {rule_id!r} is not an identifier")
    return BlackboardKeyDeclaration(
        key=interrupt_state_key(rule_id),
        type="string",
        access="read",
        persistence="run",
        fields=MappingProxyType({}),
        description=(
            f"Current state of global interrupt rule {rule_id}: "
            + " | ".join(INTERRUPT_STATE_VALUES)
        ),
    )


_INTERRUPT_STATE_REFERENCE_RE: Final = re.compile(
    r"(?<![A-Za-z0-9_.])key\.interrupt_state\.([a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)*)"
)


def _interrupt_state_references(value: Any) -> set[str]:
    """Rule ids one unvalidated value names: an exact ``interrupt_state.<id>``
    string (key parameters, condition operands, key value sources) or a
    ``key.interrupt_state.<id>`` name inside an expression."""

    found: set[str] = set()
    pending = [value]
    seen = 0
    while pending and seen <= _MAX_TREE_NODES:
        item = pending.pop()
        seen += 1
        if type(item) is dict:
            pending.extend(item.values())
        elif type(item) is list:
            pending.extend(item)
        elif type(item) is str:
            if is_interrupt_state_key(item):
                rule_id = item[len(INTERRUPT_STATE_NAMESPACE) + 1 :]
                if _IDENTIFIER_RE.fullmatch(rule_id) is not None:
                    found.add(rule_id)
                continue
            for match in _INTERRUPT_STATE_REFERENCE_RE.finditer(item):
                rule_id = match.group(1)
                # `key.interrupt_state.a.size()`: the last segment is a call.
                if item[match.end() : match.end() + 1] == "(" and "." in rule_id:
                    rule_id = rule_id.rsplit(".", 1)[0]
                found.add(rule_id)
    return found


def _declare_interrupt_state_keys(
    keys: dict[str, BlackboardKeyDeclaration], roots: Iterable[Any], blackboard: Any
) -> dict[str, BlackboardKeyDeclaration]:
    """Add the read-only ``interrupt_state.<rule_id>`` key of every rule id
    the tree's parameters/value sources or a computed key's expression name.

    A tolerant pre-scan like ``_declare_node_outcome_keys``: validation refuses
    anything malformed afterwards; the result stays sorted by key.
    """

    rule_ids: set[str] = set()
    pending = list(roots)
    seen = 0
    while pending and seen <= _MAX_TREE_NODES:
        node = pending.pop()
        seen += 1
        if type(node) is not dict:
            continue
        rule_ids |= _interrupt_state_references(
            [node.get("parameters"), node.get("value_sources")]
        )
        children = node.get("children")
        if type(children) is list:
            pending.extend(children)
    rows = blackboard.get("keys") if type(blackboard) is dict else None
    if type(rows) is list:
        rule_ids |= _interrupt_state_references(
            [row.get("expression") for row in rows if type(row) is dict]
        )
    for rule_id in rule_ids:
        declaration = interrupt_state_declaration(rule_id)
        keys[declaration.key] = declaration
    return dict(sorted(keys.items()))


def require_interrupt_state_rules(program: "Program", rule_ids: Iterable[str]) -> None:
    """The Program x Deployment join (contract 64): every
    ``interrupt_state.<rule_id>`` the Program reads names a declared global
    rule of the Deployment."""

    missing = sorted(set(program.interrupt_state_rule_ids) - set(rule_ids))
    if missing:
        raise ProgramValidationError(
            "Program reads interrupt_state key(s) of global rule(s) the "
            f"Deployment does not declare: {', '.join(missing)}"
        )


# Contract 87: every blackboard key an interrupt condition reads (a node rule
# on move_until_signal / probe_search, or a Deployment global rule) declares
# `initial:` (presence only; false / 0 are valid). Same reason as the Core.
# System keys (node_outcome.*, interrupt_state.*) are exempt: the system
# defines their start value (contract 105).
INTERRUPT_KEY_INITIAL_REQUIRED: Final = "interrupt_key_initial_required"


def require_interrupt_key_initials(
    program: "Program", global_rules: Iterable[Mapping[str, Any]] = ()
) -> None:
    """The Program x Deployment check of contract 87, where both are known.

    Undeclared keys and malformed conditions are refused by their own checks
    and are not reported here.
    """

    used: dict[str, list[str]] = {}
    for key in program.interrupt_condition_keys:
        used.setdefault(key, []).append("a node rule")
    for rule in global_rules:
        try:
            keys = condition_keys(rule.get("condition"))
        except Exception:  # Malformed: refused by the rule codec.
            continue
        for key in keys:
            used.setdefault(key, []).append(f"global rule {rule.get('rule_id')!r}")
    missing = sorted(
        key
        for key in used
        if key in program.blackboard_keys
        and _reserved_namespace(key) is None
        and not program.blackboard_keys[key].has_initial
    )
    if missing:
        detail = "; ".join(f"{key} (read by {', '.join(used[key])})" for key in missing)
        raise ProgramValidationError(
            f"{INTERRUPT_KEY_INITIAL_REQUIRED}: interrupt condition keys must declare "
            f"an initial value: {detail}"
        )


# Contracts 106, 110, 111: the checks of global rules' key operands.
GLOBAL_RULE_KEY_UNDECLARED: Final = "global_rule_key_undeclared"
GLOBAL_RULE_STATE_OPERAND_UNSUPPORTED: Final = "global_rule_state_operand_unsupported"


def _global_rule_keys(rule: Mapping[str, Any]) -> frozenset[str]:
    try:
        return condition_keys(rule.get("condition"))
    except Exception:  # Malformed: refused by the rule codec.
        return frozenset()


def require_global_rule_operands(global_rules: Iterable[Mapping[str, Any]]) -> None:
    """A global rule's condition never reads an ``interrupt_state.*`` key
    (contract 111); Program nodes and node rules keep reading them."""

    found = sorted(
        (str(rule.get("rule_id")), key)
        for rule in global_rules
        for key in _global_rule_keys(rule)
        if is_interrupt_state_key(key)
    )
    if found:
        detail = "; ".join(f"global rule {rule!r} reads {key}" for rule, key in found)
        raise ProgramValidationError(
            f"{GLOBAL_RULE_STATE_OPERAND_UNSUPPORTED}: a global rule condition "
            f"cannot read an interrupt_state key: {detail}"
        )


def require_global_rule_keys(
    program: "Program", global_rules: Iterable[Mapping[str, Any]]
) -> None:
    """Applying a Program that does not declare a blackboard key some global
    rule reads is refused, naming the rule and the key (contract 106).

    ``interrupt_state.*`` operands are refused by
    ``require_global_rule_operands`` and are not reported here.
    """

    missing = sorted(
        (str(rule.get("rule_id")), key)
        for rule in global_rules
        for key in _global_rule_keys(rule)
        if not is_interrupt_state_key(key) and key not in program.blackboard_keys
    )
    if missing:
        detail = "; ".join(f"global rule {rule!r} reads {key}" for rule, key in missing)
        raise ProgramValidationError(
            f"{GLOBAL_RULE_KEY_UNDECLARED}: the Program does not declare a key a "
            f"global rule reads: {detail}"
        )


_RESERVED_NAMESPACES: Final = MappingProxyType(
    {
        NODE_OUTCOME_NAMESPACE: "node outcome keys",
        INTERRUPT_STATE_NAMESPACE: "interrupt rule state keys",
    }
)


def _reserved_namespace(name: str) -> str | None:
    """The system namespace a key or parameter name uses (the bare namespace
    name or any name under it), or None (contract 105(c))."""

    namespace = name.split(".", 1)[0]
    return namespace if namespace in _RESERVED_NAMESPACES else None


def _node_outcome_declaration(instance_id: str) -> BlackboardKeyDeclaration:
    return BlackboardKeyDeclaration(
        key=node_outcome_key(instance_id),
        type="string",
        access="read",
        persistence="run",
        fields=MappingProxyType({}),
        description=(
            f"Last outcome of motion node {instance_id}: "
            + " | ".join(NODE_OUTCOME_VALUES)
            + f" ({NODE_OUTCOME_NONE} = no result yet in this Run)"
        ),
        has_initial=True,
        initial=NODE_OUTCOME_NONE,
    )


def _declare_node_outcome_keys(
    keys: dict[str, BlackboardKeyDeclaration], roots: Iterable[Any]
) -> dict[str, BlackboardKeyDeclaration]:
    """Add the implicit outcome key of every motion node under ``roots``.

    A tolerant pre-scan of unvalidated tree mappings: tree validation refuses
    anything malformed afterwards. The result stays sorted by key.
    """

    pending = list(roots)
    seen = 0
    while pending and seen <= _MAX_TREE_NODES:
        node = pending.pop()
        seen += 1
        if type(node) is not dict:
            continue
        instance_id = node.get("instance_id")
        if (
            node.get("node_id") in NODE_OUTCOME_NODE_IDS
            and type(instance_id) is str
            and _IDENTIFIER_RE.fullmatch(instance_id) is not None
        ):
            declaration = _node_outcome_declaration(instance_id)
            keys[declaration.key] = declaration
        children = node.get("children")
        if type(children) is list:
            pending.extend(children)
    return dict(sorted(keys.items()))


_EXECUTION_POLICIES: Final = frozenset(
    {
        "independent",
        "common_start",
        "common_start_and_finish",
        "sync_offset_traj",
    }
)


def _parse_execution_group_policies(raw: Any, *, path: str) -> Mapping[str, str]:
    if raw is None:
        return MappingProxyType({})
    if type(raw) is not dict:
        raise ProgramValidationError(f"{path} must be a mapping")
    if len(raw) > _MAX_EXECUTION_GROUPS:
        raise ProgramValidationError(f"{path} groups exceed the supported limit")
    if list(raw) != sorted(raw):
        raise ProgramValidationError(f"{path} keys must be canonical-sorted")
    result: dict[str, str] = {}
    for name, definition in raw.items():
        if (
            type(name) is not str
            or len(name) > 128
            or _IDENTIFIER_RE.fullmatch(name) is None
            or name in {"all", "inherit"}
        ):
            raise ProgramValidationError(f"{path} contains an invalid group ID")
        if type(definition) is not dict or set(definition) != {"execution_policy"}:
            raise ProgramValidationError(
                f"{path}.{name} must contain only execution_policy"
            )
        policy = definition["execution_policy"]
        if type(policy) is not str or policy not in _EXECUTION_POLICIES:
            raise ProgramValidationError(
                f"{path}.{name}.execution_policy is unsupported"
            )
        result[name] = policy
    return MappingProxyType(result)


def _validate_execution_group_policies(
    tree: ProgramTreeNode,
    policies: Mapping[str, str],
) -> Mapping[str, tuple[str, ...]]:
    """Validate declarations and coordinated shapes without adding an owner."""

    selections = _resolve_execution_selections(tree)
    authored_names = set(selections) - {"all"}
    declared_names = set(policies)
    if authored_names != declared_names:
        missing = sorted(authored_names - declared_names)
        unused = sorted(declared_names - authored_names)
        details = []
        if missing:
            details.append(f"undeclared groups {missing}")
        if unused:
            details.append(f"unused declarations {unused}")
        raise ProgramValidationError("execution_groups has " + " and ".join(details))
    coordinated_names = tuple(
        name for name, policy in policies.items() if policy != "independent"
    )
    if not coordinated_names:
        return MappingProxyType({})
    nodes: dict[str, ProgramTreeNode] = {}
    parents: dict[str, ProgramTreeNode | None] = {}
    pending = [(tree, None)]
    while pending:
        node, parent = pending.pop()
        nodes[node.instance_id] = node
        parents[node.instance_id] = parent
        pending.extend((child, node) for child in reversed(node.children))

    result: dict[str, tuple[str, ...]] = {}
    already_coordinated: set[str] = set()
    for group_name in coordinated_names:
        members = selections.get(group_name)
        if members is None or len(members) < 2:
            raise ProgramValidationError(
                f"execution_groups.{group_name} must select at least two nodes for "
                f"{policies[group_name]}"
            )
        member_nodes = [nodes[instance_id] for instance_id in members]
        if any(node.node_id not in MOTION_NODE_IDS for node in member_nodes):
            raise ProgramValidationError(
                f"execution_groups.{group_name} may contain only ordinary motion nodes "
                f"for {policies[group_name]}"
            )
        if any(
            not isinstance(node.execution_groups, tuple)
            or group_name not in node.execution_groups
            for node in member_nodes
        ):
            raise ProgramValidationError(
                f"execution_groups.{group_name} must be assigned directly to every "
                "coordinated member"
            )
        parent = parents[members[0]]
        if (
            parent is None
            or parent.node_id != "parallel"
            or parent.parameters.get("success_policy", "all") != "all"
            # A sourced policy is resolved per attempt and could be `one`.
            or "success_policy" in parent.value_sources
            or any(parents[instance_id] is not parent for instance_id in members[1:])
        ):
            raise ProgramValidationError(
                f"execution_groups.{group_name} must contain sibling motions under one "
                "Success-on-all Parallel for coordinated execution"
            )
        overlap = already_coordinated.intersection(members)
        if overlap:
            raise ProgramValidationError(
                "a motion node cannot belong to more than one coordinated execution group"
            )
        already_coordinated.update(members)
        result[group_name] = members
    return MappingProxyType(result)


@dataclass(frozen=True, slots=True)
class ProgramNodePolicy:
    """One complete immutable non-default policy row for a Program node."""

    instance_id: str
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
        recovery = _parse_canonical_recovery_policy(
            dict(self.recovery_policy),
            path=f"node_policies.{self.instance_id}",
        )
        normalized = _normalize_canonical_node_policies(
            {
                "recovery_policy": recovery,
                "operator_stop_policy": self.operator_stop_policy,
            },
            path=f"node_policies.{self.instance_id}",
        )
        if normalized == (None, None):
            raise ProgramValidationError("default node-policy rows must be omitted")
        object.__setattr__(self, "recovery_policy", MappingProxyType(recovery))

    def to_dict(self) -> dict[str, Any]:
        return {
            "instance_id": self.instance_id,
            "recovery_policy": dict(self.recovery_policy),
            "operator_stop_policy": self.operator_stop_policy,
        }


def program_node_policies(program: "Program") -> tuple[ProgramNodePolicy, ...]:
    """Independently derive the canonical sparse policy vector for ``program``."""

    if not isinstance(program, Program):
        raise TypeError("program must be one validated Program")
    rows: list[ProgramNodePolicy] = []

    def visit(node: ProgramTreeNode) -> None:
        recovery = {
            **CANONICAL_DEFAULT_RECOVERY_POLICY,
            **({} if node.recovery_policy is None else dict(node.recovery_policy)),
        }
        stop = node.operator_stop_policy or "inherit"
        if recovery != CANONICAL_DEFAULT_RECOVERY_POLICY or stop != "inherit":
            rows.append(
                ProgramNodePolicy(
                    instance_id=node.instance_id,
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
    # `initial` is meaningful only when `has_initial`; an omitted initial value
    # leaves the key empty until it is written.
    has_initial: bool = False
    initial: Any = None
    # A computed key's formula; its value is always the formula's result.
    expression: str | None = None
    # Pose keys only: the declared frames a value may be stated in, in
    # canonical order; None admits any declared frame.
    allowed_frames: tuple[Mapping[str, Any], ...] | None = None


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
    if type(value) is int:
        _require_safe_integer_literal(value, path=path)
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
    contact_policy_modes: dict[str, str]
    expressions: ExpressionEnvironment
    execution_groups_supported: bool = True
    target_effect_count: int = 0
    tree_instance_ids: set[str] = field(default_factory=set)
    # Checked field expressions by (node instance, value path).
    value_expressions: dict[tuple[str, str], CheckedExpression] = field(
        default_factory=dict
    )


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
        "execution_groups",
        "tree",
    }
)
_PROGRAM_PARAMETER_TYPES: Final = frozenset(
    {
        "bool",
        "cartesian_path",
        "coordinate_position",
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
_BLACKBOARD_ACCESS_VALUES: Final = frozenset({"read", "write", "read_write"})
_BLACKBOARD_PERSISTENCE_VALUES: Final = frozenset({"run", "persistent"})
_DYNAMIC_SOURCE_FORBIDDEN_SEMANTICS: Final = frozenset(
    {
        "blackboard_key",
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
        "tcp_endpoint",
        "model_endpoint_alias",
        "prompt_template_id",
        # Automatic retry settings are literal values (spec 400 §7).
        "automatic_retries",
        "automatic_retry_delay_s",
        # Preemption priority and result are literal values (contract item 33).
        "preemption_priority",
        "preempted_result",
        # Per-node interrupt rules are sealed literals (schema02 §2).
        "interrupt_condition",
        "on_signal",
        "on_no_signal",
        "priority",
        "stop_entry_stage_id",
        "success_through_stage_id",
        # Node Mock simulation rows are literal-only (contract item 133).
        "mock_simulation",
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
                "scene_resource_axes_visibility",
                "object_axes_gizmo_size_m",
                "hidden_execution_groups",
            }
        ),
        path=path,
    )
    if "hidden_execution_groups" in row:
        # Motion panel presentation only; names are checked against the
        # declared execution groups once those are parsed.
        _canonical_identifier_list(
            row["hidden_execution_groups"], path=f"{path}.hidden_execution_groups",
        )
    for field_name in (
        "frame_role_visibility",
        "resource_role_visibility",
        "scene_resource_axes_visibility",
    ):
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
    scene_resource_ids: set[str],
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
        ("scene_resource_axes_visibility", scene_resource_ids),
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
    # An empty mapping is a logic-only Program (no component role).
    rows = _program_mapping(value, path=path)
    if len(rows) > _MAX_PROGRAM_ROLES:
        _program_error(path, "must be a bounded mapping")
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
    support_contact_refs: list[tuple[str, str, str]] = []
    shape_schema = catalog.schema_registry.types["shape"]
    for index, raw in enumerate(rows):
        row_path = f"{path}[{index}]"
        row = _program_mapping(raw, path=row_path)
        _program_closed(
            row,
            required=frozenset({"scene_resource_id", "shape", "pose"}),
            optional=frozenset({"resource_roles", "metadata"}),
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
            metadata = _program_mapping(row["metadata"], path=f"{row_path}.metadata")
            canonical_json_bytes(metadata)
            if "support_contacts" in metadata:
                contacts = _program_list(
                    metadata["support_contacts"],
                    path=f"{row_path}.metadata.support_contacts",
                )
                support_ids: list[str] = []
                for contact_index, raw_contact in enumerate(contacts):
                    contact_path = (
                        f"{row_path}.metadata.support_contacts[{contact_index}]"
                    )
                    contact = _program_mapping(raw_contact, path=contact_path)
                    support_id = _program_identifier(
                        contact.get("support_object_id"),
                        path=f"{contact_path}.support_object_id",
                    )
                    support_ids.append(support_id)
                    mode = contact.get("mode", "contact_release")
                    if mode == "bounded_penetration":
                        _program_closed(
                            contact,
                            required=frozenset({"support_object_id", "mode"}),
                            optional=frozenset({"max_contact_penetration_m"}),
                            path=contact_path,
                        )
                        if (
                            "max_contact_penetration_m" in contact
                            and _program_number(
                                contact["max_contact_penetration_m"],
                                path=f"{contact_path}.max_contact_penetration_m",
                            )
                            < 0.0
                        ):
                            _program_error(
                                f"{contact_path}.max_contact_penetration_m",
                                "must be non-negative",
                            )
                    elif mode == "contact_process":
                        _program_closed(
                            contact,
                            required=frozenset({"support_object_id", "mode"}),
                            path=contact_path,
                        )
                    elif mode == "contact_release":
                        _program_closed(
                            contact,
                            required=frozenset({"support_object_id"}),
                            optional=frozenset(
                                {
                                    "mode",
                                    "release_evidence_mode",
                                    "require_separation_by_end",
                                    "allow_recontact",
                                    "enforce_max_start_penetration",
                                    "max_start_penetration_m",
                                    "release_tolerance_m",
                                    "min_release_progress_m",
                                    "require_strictly_decreasing_penetration",
                                }
                            ),
                            path=contact_path,
                        )
                        evidence = contact.get(
                            "release_evidence_mode", "signed_penetration"
                        )
                        if evidence not in {"signed_penetration", "binary_contact"}:
                            _program_error(
                                f"{contact_path}.release_evidence_mode",
                                "must be signed_penetration or binary_contact",
                            )
                        for field in (
                            "require_separation_by_end",
                            "allow_recontact",
                            "enforce_max_start_penetration",
                            "require_strictly_decreasing_penetration",
                        ):
                            if field in contact and type(contact[field]) is not bool:
                                _program_error(
                                    f"{contact_path}.{field}", "must be a boolean"
                                )
                        for field in (
                            "max_start_penetration_m",
                            "release_tolerance_m",
                            "min_release_progress_m",
                        ):
                            if (
                                field in contact
                                and _program_number(
                                    contact[field], path=f"{contact_path}.{field}"
                                )
                                < 0.0
                            ):
                                _program_error(
                                    f"{contact_path}.{field}",
                                    "must be non-negative",
                                )
                        if evidence == "binary_contact" and (
                            contact.get("enforce_max_start_penetration", False)
                            or contact.get("max_start_penetration_m", 0.0) != 0.0
                            or contact.get("release_tolerance_m", 0.0) != 0.0
                            or contact.get("min_release_progress_m", 0.0) != 0.0
                            or contact.get(
                                "require_strictly_decreasing_penetration", False
                            )
                        ):
                            _program_error(
                                contact_path,
                                "binary_contact forbids signed-distance fields",
                            )
                    else:
                        _program_error(
                            f"{contact_path}.mode", "has an unsupported mode"
                        )
                    support_contact_refs.append((scene_id, support_id, contact_path))
                if support_ids != sorted(set(support_ids)):
                    _program_error(
                        f"{row_path}.metadata.support_contacts",
                        "must be unique and canonical-sorted by support_object_id",
                    )
    if scene_ids != sorted(scene_ids) or len(scene_ids) != len(set(scene_ids)):
        _program_error(path, "must be unique and canonical-sorted by scene_resource_id")
    declared_scene_ids = set(scene_ids)
    supports_by_resource: dict[str, set[str]] = {
        scene_id: set() for scene_id in scene_ids
    }
    for supported_id, support_id, contact_path in support_contact_refs:
        if support_id == supported_id:
            _program_error(
                f"{contact_path}.support_object_id",
                "cannot name the supported object itself",
            )
        if support_id not in declared_scene_ids:
            _program_error(
                f"{contact_path}.support_object_id",
                "does not name a declared Program scene resource",
            )
        supports_by_resource[supported_id].add(support_id)
    pending = set(scene_ids)
    while pending:
        removable = {
            scene_id
            for scene_id in pending
            if not (supports_by_resource[scene_id] & pending)
        }
        if not removable:
            _program_error(
                path,
                "contains a cyclic support-contact dependency",
            )
        pending -= removable
    return declared_scene_ids, scene_resource_roles


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


def _frame_ref_order(frame: Mapping[str, Any]) -> tuple[str, str, str]:
    """Canonical order of pose frames: by kind, then target role or scene
    resource, then frame role."""

    return (
        str(frame["kind"]),
        str(frame.get("target_role", frame.get("scene_resource_id", ""))),
        str(frame.get("frame_role", "")),
    )


def _validate_allowed_frames(
    value: Any,
    *,
    path: str,
    declared_type: str,
    target_roles: Mapping[str, dict[str, Any]],
    scene_resource_ids: set[str],
) -> tuple[Mapping[str, Any], ...]:
    """A closed, non-empty, duplicate-free, canonically ordered list of
    declared pose frames; valid only for pose keys."""

    if declared_type != "stored_pose":
        _program_error(path, "is valid only for a stored_pose key")
    rows = _program_list(value, path=path)
    if not rows:
        _program_error(path, "must not be empty; omit it to allow every declared frame")
    for index, row in enumerate(rows):
        _validate_pose_frame_ref(
            row,
            path=f"{path}[{index}]",
            target_roles=target_roles,
            scene_resource_ids=scene_resource_ids,
        )
    order = [_frame_ref_order(row) for row in rows]
    if order != sorted(order) or len(order) != len(set(order)):
        _program_error(
            path,
            "must be duplicate-free and canonical-sorted by kind, target role or "
            "scene resource, and frame role",
        )
    return tuple(MappingProxyType(dict(row)) for row in rows)


def _require_allowed_frame(
    value: Any,
    allowed_frames: tuple[Mapping[str, Any], ...] | None,
    *,
    path: str,
) -> None:
    """A pose whose frame is outside the key's `allowed_frames` is refused."""

    if allowed_frames is None:
        return
    frame = value["frame_ref"]
    if not any(dict(allowed) == frame for allowed in allowed_frames):
        _program_error(f"{path}.frame_ref", "is not one of the key's allowed_frames")


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
        if "runtime_mutability" in row:
            _program_error(
                f"{row_path}.runtime_mutability",
                "was removed: Program parameters have no override path and "
                "always take their Program default; remove the field (values "
                "that change during a run belong in blackboard keys)",
            )
        _program_closed(
            row,
            required=frozenset({"parameter_id", "type", "default"}),
            optional=frozenset({"constraints", "description"}),
            path=row_path,
        )
        parameter_id = _program_identifier(
            row["parameter_id"], path=f"{row_path}.parameter_id"
        )
        reserved = _reserved_namespace(parameter_id)
        if reserved is not None:
            _program_error(
                f"{row_path}.parameter_id",
                f"{parameter_id} uses the reserved system name {reserved} "
                f"({_RESERVED_NAMESPACES[reserved]}); choose another parameter ID",
            )
        declared_type = _program_identifier(row["type"], path=f"{row_path}.type")
        if declared_type not in _PROGRAM_PARAMETER_TYPES:
            _program_error(f"{row_path}.type", "is not a registered Program type")
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
    scene_resource_ids: set[str],
    target_roles: Mapping[str, dict[str, Any]],
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
        if namespace_id in _RESERVED_NAMESPACES:
            _program_error(
                f"{row_path}.namespace_id",
                f"{namespace_id} is reserved for the system-owned "
                f"{_RESERVED_NAMESPACES[namespace_id]} and cannot be declared",
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
            optional=frozenset(
                {"fields", "description", "initial", "expression", "allowed_frames"}
            ),
            path=row_path,
        )
        key = _program_identifier(row["key"], path=f"{row_path}.key")
        reserved = _reserved_namespace(key)
        if reserved is not None:
            _program_error(
                f"{row_path}.key",
                f"{key} is in the reserved {reserved} namespace: "
                f"{_RESERVED_NAMESPACES[reserved]} are declared by the system, "
                "never authored",
            )
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
        if persistence == "node":
            _program_error(
                f"{row_path}.persistence",
                "node was removed; use run or persistent, and reset the key "
                "explicitly where the Program needs it",
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
        allowed_frames = (
            None
            if "allowed_frames" not in row
            else _validate_allowed_frames(
                row["allowed_frames"],
                path=f"{row_path}.allowed_frames",
                declared_type=declared_type,
                target_roles=target_roles,
                scene_resource_ids=scene_resource_ids,
            )
        )
        has_initial = "initial" in row
        initial = deepcopy(row.get("initial"))
        if has_initial:
            if initial is None:
                _program_error(
                    f"{row_path}.initial",
                    "must be omitted instead of null for an empty key",
                )
            _validate_declared_type_value(
                declared_type,
                initial,
                path=f"{row_path}.initial",
                scene_resource_ids=scene_resource_ids,
                target_roles=target_roles,
            )
            _require_allowed_frame(initial, allowed_frames, path=f"{row_path}.initial")
        expression: str | None = None
        if "expression" in row:
            expression = _program_text(
                row["expression"], path=f"{row_path}.expression"
            )
            if has_initial:
                _program_error(
                    f"{row_path}.initial",
                    "a computed key has no initial value; its value is always "
                    "its expression's result",
                )
            if persistence != "run":
                _program_error(
                    f"{row_path}.persistence", "a computed key cannot be persistent"
                )
            if access != "read":
                _program_error(
                    f"{row_path}.access",
                    "a computed key is read-only for nodes and the operator; "
                    "declare access read",
                )
            if fields or declared_type not in DESTINATION_TYPES:
                _program_error(
                    f"{row_path}.type",
                    "a computed key must be bool, int64, float64 or string",
                )
        key_ids.append(key)
        keys[key] = BlackboardKeyDeclaration(
            key=key,
            type=declared_type,
            access=access,
            persistence=persistence,
            has_initial=has_initial,
            initial=initial,
            expression=expression,
            allowed_frames=allowed_frames,
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


def motion_tcp_selection(
    parameters: Mapping[str, Any], *, path: str = "parameters"
) -> dict[str, Any]:
    """Validate the portable TCP selector without resolving deployment frames.

    Program validation additionally checks the selected target/frame requirements;
    lowering uses this same shape check on immutable captured inputs.
    """
    if "tcp_frame_role" in parameters:
        _program_error(path, "tcp_frame_role is unsupported; select tcp_endpoint")
    if "tcp_endpoint" not in parameters:
        _program_error(path, "requires tcp_endpoint")
    value = parameters["tcp_endpoint"]
    if not isinstance(value, Mapping) or set(value) != {"target_role", "frame_role"}:
        _program_error(f"{path}.tcp_endpoint", "requires exactly target_role and frame_role")
    return {
        "tcp_endpoint": {
            name: _program_identifier(value[name], path=f"{path}.tcp_endpoint.{name}")
            for name in ("target_role", "frame_role")
        }
    }


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
    if kind == "motion_endpoint":
        _program_closed(row, required=frozenset({"kind"}), path=path)
        return kind, ""
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
) -> dict[str, str]:
    rows = _program_list(value, path=path)
    policy_ids: list[str] = []
    policy_modes: dict[str, str] = {}
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
            policy["mode"] not in ("contact_grasp", "contact_release")
            or policy["sensing_mode"] != "geometry_only"
            or policy["default_unmatched_response"] != "hard_stop"
        ):
            _program_error(
                policy_path,
                "is outside the qualified geometry-only contact policy shape",
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
            release = policy["mode"] == "contact_release"
            _program_closed(
                zone,
                required=frozenset({"zone_id", "body_pairs", "violation_response"})
                | (frozenset({"release_evidence_mode"}) if release else
                   frozenset({"max_penetration_m", "terminal_window_start_fraction"})),
                optional=(frozenset({
                    "enforce_max_start_penetration", "max_start_penetration_m",
                    "min_release_progress_m", "release_tolerance_m",
                    "require_strictly_decreasing_penetration",
                    "require_separation_by_end", "allow_recontact",
                }) if release else frozenset()),
                path=zone_path,
            )
            zone_id = _program_identifier(zone["zone_id"], path=f"{zone_path}.zone_id")
            zone_ids.append(zone_id)
            if zone["violation_response"] != "hard_stop":
                _program_error(f"{zone_path}.violation_response", "must be hard_stop")
            if release:
                evidence = zone["release_evidence_mode"]
                if evidence not in ("signed_penetration", "binary_contact"):
                    _program_error(f"{zone_path}.release_evidence_mode",
                                   "must be signed_penetration or binary_contact")
                for field in ("enforce_max_start_penetration",
                              "require_strictly_decreasing_penetration",
                              "require_separation_by_end", "allow_recontact"):
                    if field in zone and type(zone[field]) is not bool:
                        _program_error(f"{zone_path}.{field}", "must be a boolean")
                for field in ("max_start_penetration_m", "min_release_progress_m",
                              "release_tolerance_m"):
                    if field in zone and not 0 <= _program_number(
                        zone[field], path=f"{zone_path}.{field}"
                    ) <= 1000:
                        _program_error(f"{zone_path}.{field}", "must be in [0, 1000]")
                if evidence == "binary_contact" and (
                    zone.get("enforce_max_start_penetration", False)
                    or zone.get("require_strictly_decreasing_penetration", False)
                    or any(zone.get(field, 0.0) != 0.0 for field in (
                        "max_start_penetration_m", "min_release_progress_m",
                        "release_tolerance_m"))
                ):
                    _program_error(zone_path, "binary_contact forbids signed-distance fields")
            else:
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
                if kinds not in (
                    {"component_model_body", "runtime_object"},
                    {"motion_endpoint", "runtime_object"},
                ):
                    _program_error(
                        pair_path,
                        "must join one component body or motion endpoint to one runtime object",
                    )
                if "motion_endpoint" in kinds and len(pairs) != 1:
                    _program_error(
                        f"{zone_path}.body_pairs",
                        "motion_endpoint requires exactly one body pair",
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
        policy_modes[policy_id] = policy["mode"]
    if policy_ids != sorted(policy_ids) or len(policy_ids) != len(set(policy_ids)):
        _program_error(path, "must be unique and canonical-sorted by policy_id")
    return policy_modes


def _validate_semantic_value(
    schema: CatalogSchema,
    value: Any,
    *,
    path: str,
    context: _ProgramValidationContext,
    node_parameters: Mapping[str, Any],
    sibling_target_role: str | None = None,
) -> None:
    """Check declared identities. A role beside a target role in the same
    object (a pose frame, a robot-qualified reference) belongs to that target;
    otherwise it is scoped by the schema's semantic scope parameter."""

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
                if sibling_target_role is not None:
                    scoped_target = context.target_roles.get(sibling_target_role)
                    accepted = scoped_target is not None and identity in scoped_target.get(
                        role_field, ()
                    )
                    scope_description = f"target role {sibling_target_role!r}"
                elif scope_parameter is None:
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
                    accepted = (
                        scoped_target is not None
                        and identity
                        in scoped_target.get(
                            role_field,
                            (),
                        )
                    )
                    scope_description = f"target role {scope_value!r}"
                if not accepted:
                    _program_error(
                        path,
                        f"does not name a {semantic_type} required by {scope_description}",
                    )
    if concrete.schema_type == "object" and type(value) is dict:
        sibling_target = next(
            (
                value[name]
                for name, property_schema in concrete.properties.items()
                if type(value.get(name)) is str
                and context.catalog.schema_registry.resolve(property_schema).semantic_type
                == "target_role"
            ),
            None,
        )
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
                    sibling_target_role=sibling_target,
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
    if destination.schema_type == "key_value":
        return source_type in destination.value_types
    if destination.schema_type == "float":
        return source_type == "float64"
    if destination.schema_type == "int":
        return source_type == "int64"
    if destination.schema_type == "bool":
        return source_type == "bool"
    if destination.schema_type in {"string", "enum"}:
        return source_type == "string"
    if destination.schema_type == "array":
        if destination.items is None:
            return False
        item = context.catalog.schema_registry.resolve(destination.items)
        if source_type == "string_list":
            return item.schema_type == "string"
        if source_type == "cartesian_path":
            return item.schema_type == "object" and {
                "position_m",
                "orientation",
            }.issubset(item.properties)
        return False
    if destination.schema_type == "object":
        if source_type == "stored_pose":
            return {"position_m", "orientation"}.issubset(destination.properties)
        if source_type == "coordinate_position":
            return {"kind", "value_m", "value_rad"}.issubset(
                destination.properties
            )
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
    effects: set[tuple[str, str]] = set()
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
        effects.add((target_role, requirement.capability_id))
    context.target_effect_count += len(effects)
    if (
        context.execution_groups_supported
        and context.target_effect_count > _MAX_RESOLVED_TARGET_EFFECTS
    ):
        _program_error(path, "resolved target effects exceed fixed bound")


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
        # Several bindings of one key parameter declare the admitted types.
        admitted: dict[str, list[str]] = {}
        for binding in bindings:
            admitted.setdefault(binding.key_parameter, []).append(binding.value_type)
        for key_parameter, value_types in admitted.items():
            key = parameters.get(key_parameter)
            if type(key) is not str:
                _program_error(
                    f"{path}.parameters.{key_parameter}",
                    "must be one literal declared blackboard key",
                )
            declaration = context.blackboard_keys.get(key)
            if declaration is None:
                _program_error(
                    f"{path}.parameters.{key_parameter}",
                    "does not name a declared blackboard key",
                )
            if declaration.type not in value_types:
                expected = " or ".join(repr(item) for item in value_types)
                _program_error(
                    f"{path}.parameters.{key_parameter}",
                    f"declares type {declaration.type!r}, expected {expected}",
                )
            if direction == "write" and is_node_outcome_key(key):
                _program_error(
                    f"{path}.parameters.{key_parameter}",
                    f"names system-owned node outcome key {key!r}, which only "
                    "its motion node writes",
                )
            if direction == "write" and is_interrupt_state_key(key):
                _program_error(
                    f"{path}.parameters.{key_parameter}",
                    f"names system-owned interrupt rule state key {key!r}, "
                    "which only its rule publishes",
                )
            if direction == "write" and declaration.expression is not None:
                _program_error(
                    f"{path}.parameters.{key_parameter}",
                    f"names computed key {key!r}, which nodes cannot write",
                )
            permitted = (
                declaration.access in {"read", "read_write"}
                if direction == "read"
                else declaration.access in {"write", "read_write"}
            )
            if not permitted:
                _program_error(
                    f"{path}.parameters.{key_parameter}",
                    f"does not permit {direction} access",
                )


def _validate_key_typed_parameters(
    entry: Any,
    parameters: Mapping[str, Any],
    *,
    path: str,
    context: _ProgramValidationContext,
) -> None:
    """Type each key_value literal by its key (after the key bindings)."""

    for catalog_parameter in entry.parameters:
        schema = catalog_parameter.schema
        if schema.value_type_from_key is None:
            continue
        key_type = context.blackboard_keys[parameters[schema.value_type_from_key]].type
        parameter_id = catalog_parameter.id
        if parameter_id in parameters:
            _validate_declared_type_value(
                key_type,
                parameters[parameter_id],
                path=f"{path}.parameters.{parameter_id}",
                scene_resource_ids=context.scene_resource_ids,
                target_roles=context.target_roles,
            )


def _parse_value_source_paths(
    value_sources: Mapping[str, Any], *, path: str
) -> dict[str, tuple[Segment, ...]]:
    """Parse every key; refuse unsorted keys and overlapping entries."""

    parsed: dict[str, tuple[Segment, ...]] = {}
    for key in value_sources:
        try:
            parsed[key] = parse_value_path(key)
        except ValuePathError as error:
            _program_error(f"{path} key {key!r}", f"{error.code}: {error}")
    if tuple(sorted(value_sources)) != tuple(value_sources):
        _program_error(path, "keys must be canonical-sorted")
    by_segments = {segments: key for key, segments in parsed.items()}
    for key, segments in parsed.items():
        for length in range(1, len(segments)):
            outer = by_segments.get(segments[:length])
            if outer is not None:
                _program_error(
                    f"{path}.{key}",
                    f"overlaps the value_sources entry {outer!r}; a value bound "
                    "as a whole cannot also have entries for its own fields",
                )
    return parsed


_SCHEMA_EXPRESSION_TYPES: Final = MappingProxyType(
    {"bool": "bool", "int": "int64", "float": "float64", "string": "string"}
)


def _value_source_destination(
    catalog_parameter: Any,
    segments: tuple[Segment, ...],
    parameters: Mapping[str, Any],
    *,
    path: str,
    catalog: NodeCatalog,
    blackboard_keys: Mapping[str, BlackboardKeyDeclaration],
) -> tuple[CatalogSchema | None, str | None]:
    """Return the destination schema, or the key type of a whole key_value."""

    registry = catalog.schema_registry
    schema = catalog_parameter.schema
    key_type: str | None = None
    if schema.value_type_from_key is not None:
        key_type = blackboard_keys[parameters[schema.value_type_from_key]].type
        if len(segments) == 1:
            return None, key_type
        if key_type != "stored_pose":
            _program_error(path, f"{key_type} values have no fields")
        schema = registry.types["stored_pose"]
    if len(segments) > 1:
        if catalog_parameter.id in parameters:
            current = parameters[catalog_parameter.id]
        elif catalog_parameter.has_default:
            current = catalog_parameter.default
        else:
            _program_error(path, "names no value in the node's parameters")
        try:
            # A source may supply an absent map entry (an object's additional
            # property); every other path names an authored value.
            value_at_path(
                current,
                segments[:-1] if _names_map_entry(segments, schema, registry) else segments,
            )
        except KeyError:
            _program_error(path, "names no value in the node's parameters")
    previous: Segment = segments[0]
    for segment in segments[1:]:
        concrete = registry.resolve(schema)
        if type(segment) is int:
            if concrete.schema_type != "array" or concrete.items is None:
                _program_error(path, "indexes a value that is not a list")
            schema = concrete.items
        else:
            if previous == "orientation" and segment == "xyzw":
                _program_error(
                    path,
                    "quaternion components depend on each other; an xyzw "
                    "orientation is typed or bound whole (per-component "
                    "sources are allowed only under rpy_deg)",
                )
            child = (
                concrete.properties.get(segment)
                if concrete.schema_type == "object"
                else None
            )
            if child is None and concrete.schema_type == "object":
                child = concrete.additional_property_schema
            if child is None:
                _program_error(path, f"names no field {segment!r}")
            schema = child
        previous = segment
    return schema, None


def _names_map_entry(
    segments: tuple[Segment, ...],
    schema: CatalogSchema,
    registry: CatalogSchemaRegistry,
) -> bool:
    """True when ``segments`` ends at an entry of a map (an object's additional
    properties); a path the schema does not admit is not a map entry."""

    for segment in segments[1:-1]:
        concrete = registry.resolve(schema)
        if type(segment) is int:
            child = concrete.items if concrete.schema_type == "array" else None
        elif concrete.schema_type == "object":
            child = concrete.properties.get(segment) or concrete.additional_property_schema
        else:
            child = None
        if child is None:
            return False
        schema = child
    concrete = registry.resolve(schema)
    return (
        type(segments[-1]) is str
        and concrete.schema_type == "object"
        and segments[-1] not in concrete.properties
        and concrete.additional_property_schema is not None
    )


def _expression_destination(
    concrete: CatalogSchema | None, key_type: str | None
) -> str | None:
    """The type a formula computes for this destination, if it may have one."""

    if concrete is not None:
        if concrete.semantic_type is not None:
            return None
        return _SCHEMA_EXPRESSION_TYPES.get(concrete.schema_type)
    return key_type if key_type in DESTINATION_TYPES else None


def _validate_node_value_sources(
    entry: Any,
    instance_id: str,
    parameters: Mapping[str, Any],
    value_sources: Mapping[str, Any],
    source_paths: Mapping[str, tuple[Segment, ...]],
    *,
    path: str,
    context: _ProgramValidationContext,
) -> None:
    """Check each path-keyed source against the value it replaces (effect
    selector roots are refused before the literals are checked)."""

    registry = context.catalog.schema_registry
    by_id = {parameter.id: parameter for parameter in entry.parameters}
    for key, segments in source_paths.items():
        entry_path = f"{path}.value_sources.{key}"
        catalog_parameter = by_id[segments[0]]
        schema, key_type = _value_source_destination(
            catalog_parameter,
            segments,
            parameters,
            path=entry_path,
            catalog=context.catalog,
            blackboard_keys=context.blackboard_keys,
        )
        concrete = registry.resolve(schema) if schema is not None else None
        if concrete is not None and (
            concrete.semantic_type in _DYNAMIC_SOURCE_FORBIDDEN_SEMANTICS
            or "source_joint" in _schema_semantic_types(schema, context.catalog)
        ):
            _program_error(entry_path, "cannot dynamically select runtime/binding semantics")
        nested = len(segments) > 1
        if (
            concrete is not None
            and nested
            and (
                concrete.schema_type in {"string", "enum"}
                or concrete.semantic_type is not None
            )
        ):
            _program_error(
                entry_path,
                "nested text, choice and identifier fields stay literal",
            )
        source = _program_mapping(value_sources[key], path=entry_path)
        if source.get("kind") == "expression":
            _program_closed(
                source, required=frozenset({"kind", "expression"}), path=entry_path
            )
            text = _program_text(source["expression"], path=f"{entry_path}.expression")
            destination = _expression_destination(concrete, key_type)
            if destination is None:
                _program_error(
                    entry_path,
                    "an expression computes only a bool, int64, float64 or "
                    "string value (not a choice, identifier or whole value)",
                )
            try:
                checked = context.expressions.check(text, destination)
            except ExpressionError as error:
                _program_error(
                    f"{entry_path}.expression",
                    describe_diagnostic(error.diagnostics[0]),
                )
            context.value_expressions[(instance_id, key)] = checked
            continue
        if concrete is None:
            source_type = _validate_value_source(
                source, path=entry_path, context=context, destination_schema=None
            )
            if source_type != key_type:
                key_name = parameters[catalog_parameter.schema.value_type_from_key]
                _program_error(
                    entry_path,
                    f"source type {source_type!r} does not match the "
                    f"{key_type!r} type of key {key_name!r}",
                )
            continue
        _validate_value_source(
            source, path=entry_path, context=context, destination_schema=schema
        )


def _goal_frame(
    pose: Any, parameters: Mapping[str, Any]
) -> Mapping[str, Any] | None:
    """The authored frame of a goal pose; None for the moving robot's base."""

    frame = pose.get("frame_ref") if type(pose) is dict else None
    if frame is None or frame == {
        "kind": "target_frame",
        "target_role": parameters.get("target_role"),
        "frame_role": parameters.get("base_frame_role"),
    }:
        return None
    return frame


def _validate_node_cross_fields(
    node_id: str,
    parameters: Mapping[str, Any],
    *,
    sourced: frozenset[str],
    path: str,
    context: _ProgramValidationContext,
) -> None:
    """Checks across literal fields. ``parameters`` holds the literals; a
    parameter in ``sourced`` comes whole from a value source and is checked
    when its value is resolved at node start."""

    goal = parameters.get("goal")
    if node_id == "motion.move_to_joint_target" and type(goal) is list:
        joints = [item.get("joint_name") for item in goal if type(item) is dict]
        if len(joints) != len(set(joints)):
            _program_error(f"{path}.parameters.goal", "contains duplicate joint names")
    if node_id in {"motion.move_to_pose", "motion.follow_cartesian_path"}:
        mode = parameters.get("target_mode", "absolute")
        if (
            mode == "relative"
            and "target_reference" not in parameters
            and "target_reference" not in sourced
        ):
            _program_error(
                f"{path}.parameters.target_reference",
                "is required for relative pose motion",
            )
        poses = (
            [goal]
            if node_id == "motion.move_to_pose"
            else parameters.get("path") if type(parameters.get("path")) is list else []
        )
        frames = [_goal_frame(pose, parameters) for pose in poses if type(pose) is dict]
        field = "goal" if node_id == "motion.move_to_pose" else "path"
        if any(frame != frames[0] for frame in frames):
            _program_error(f"{path}.parameters.{field}", "waypoints must all use one frame")
        if frames and frames[0] is not None and mode == "relative":
            _program_error(
                f"{path}.parameters.{field}",
                "a relative goal is stated in the moving robot's base frame; "
                "its reference is target_reference",
            )
    if "contact_policy_id" in parameters:
        policy_id = _program_identifier(
            parameters["contact_policy_id"],
            path=f"{path}.parameters.contact_policy_id",
        )
        if policy_id not in context.contact_policy_modes:
            _program_error(
                f"{path}.parameters.contact_policy_id",
                "does not name a declared contact policy",
            )
        if (node_id == "planning.plan_pick_sequence"
                and context.contact_policy_modes[policy_id] != "contact_grasp"):
            _program_error(f"{path}.parameters.contact_policy_id",
                           "pick requires a contact_grasp policy")
    if node_id in {"logic.check_value", "logic.wait_for_value"}:
        key_type = context.blackboard_keys[parameters["key"]].type
        reason = comparison_supported(
            key_type,
            parameters.get("operator", "=="),
            has_tolerance="tolerance" in parameters,
        )
        if reason is not None:
            _program_error(f"{path}.parameters.operator", reason)
    if node_id in INTERRUPT_MOTION_NODE_IDS and "interrupt_condition" in parameters:
        try:
            check_condition(
                parameters["interrupt_condition"],
                key_types={
                    key: declaration.type
                    for key, declaration in context.blackboard_keys.items()
                },
                target_resource_roles={
                    role: frozenset(target.get("required_resource_roles", ()))
                    for role, target in context.target_roles.items()
                },
                path=f"{path}.parameters.interrupt_condition",
            )
        except InterruptConditionError as error:
            _program_error(error.path, f"{error.reason}: {error.message}")
    if node_id in MOCK_SIMULATION_NODE_IDS and "mock_simulation" in parameters:
        if node_id in INTERRUPT_MOTION_NODE_IDS:
            used = (
                condition_signals(parameters["interrupt_condition"])
                if "interrupt_condition" in parameters
                else frozenset()
            )
        elif {"target_role", "resource_role"} <= set(parameters):
            used = frozenset({(parameters["target_role"], parameters["resource_role"])})
        else:
            used = frozenset()
        _validate_mock_simulation(
            parameters["mock_simulation"],
            used,
            path=f"{path}.parameters.mock_simulation",
        )
    if node_id == "io.wait_for_input" and (
        "edge" in sourced or parameters.get("edge", "level") != "level"
    ):
        # Only level waits run in this slice; an edge is refused, never inert.
        _program_error(
            f"{path}.parameters.edge",
            "io_watch_condition_unsupported: only level waits are supported",
        )
    if node_id == "geometry.check_pose_bounds":
        _validate_pose_frame_ref(
            parameters["frame_ref"],
            path=f"{path}.parameters.frame_ref",
            target_roles=context.target_roles,
            scene_resource_ids=context.scene_resource_ids,
        )
        if any(
            low > high
            for low, high in zip(parameters["min_position_m"], parameters["max_position_m"])
        ):
            _program_error(
                f"{path}.parameters.min_position_m", "exceeds max_position_m"
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


def _validate_mock_simulation(
    rows: list[Mapping[str, Any]],
    used: frozenset[tuple[str, str]],
    *,
    path: str,
) -> None:
    """Each row names one distinct signal the node itself reads (catalog
    schema already checked the row shape and bool values)."""

    seen: set[tuple[str, str]] = set()
    for index, row in enumerate(rows):
        signal = (row["signal"]["target_role"], row["signal"]["resource_role"])
        row_path = f"{path}[{index}].signal"
        if signal not in used:
            _program_error(
                row_path,
                f"{MOCK_SIMULATION_SIGNAL_UNUSED}: signal {signal[0]}/{signal[1]} "
                "is not read by this node",
            )
        if signal in seen:
            _program_error(
                row_path,
                f"{MOCK_SIMULATION_SIGNAL_DUPLICATE}: signal {signal[0]}/{signal[1]} "
                "already has a Mock simulation row",
            )
        seen.add(signal)


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
                "recovery_policy",
                "operator_stop_policy",
            }
        )
        | (
            frozenset({"execution_groups"})
            if context.execution_groups_supported
            else frozenset()
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
        if node_id in {
            "controller.select_route",
            "lifecycle.prepare_component",
            "lifecycle.enable_component",
            "lifecycle.disable_component",
            "lifecycle.recover_component",
        }:
            _program_error(
                f"{path}.node_id",
                "is an unsupported lifecycle Program node; use the explicit "
                "idle-time component-management API",
            )
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
    source_paths = _parse_value_source_paths(
        value_sources, path=f"{path}.value_sources"
    )

    catalog_parameters = {parameter.id: parameter for parameter in entry.parameters}
    unknown = sorted(
        (set(parameters) | {segments[0] for segments in source_paths.values()})
        - set(catalog_parameters)
    )
    if unknown:
        _program_error(path, f"supplies unknown catalog parameters {unknown}")

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
    for key, segments in source_paths.items():
        if segments[0] == "mock_simulation":
            _program_error(
                f"{path}.value_sources.{key}",
                f"{MOCK_SIMULATION_SOURCE_REFUSED}: Mock simulation rows are "
                "literal values only",
            )
        if (
            segments[0] in _DYNAMIC_SOURCE_FORBIDDEN_PARAMETERS
            or segments[0] in selector_parameters
        ):
            _program_error(
                f"{path}.value_sources.{key}",
                "cannot dynamically select runtime/binding semantics",
            )
    effective_parameters = {
        parameter_id: catalog_parameter.default
        for parameter_id, catalog_parameter in catalog_parameters.items()
        if catalog_parameter.has_default
    }
    effective_parameters.update(parameters)

    if "tcp_endpoint" in catalog_parameters:
        motion_tcp_selection(parameters, path=f"{path}.parameters")

    for parameter_id, catalog_parameter in catalog_parameters.items():
        supplied_literal = parameter_id in parameters
        # A whole-value source supplies the parameter; the literal, if kept,
        # is what "Type values" restores when the source is removed.
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
                _program_error(
                    issues[0].path,
                    f"{MOCK_SIMULATION_INVALID}: {issues[0].message}"
                    if parameter_id == "mock_simulation"
                    else issues[0].message,
                )
            _validate_semantic_value(
                catalog_parameter.schema,
                parameters[parameter_id],
                path=f"{path}.parameters.{parameter_id}",
                context=context,
                node_parameters=effective_parameters,
            )

    for parameter_id in sorted(_PROGRAM_MOTION_DEFAULTS & set(catalog_parameters)):
        declaration = context.parameters.get(parameter_id)
        if declaration is None:
            continue
        ceiling = declaration.default
        if type(ceiling) not in {int, float} or not math.isfinite(float(ceiling)):
            _program_error(
                f"program.parameters.{parameter_id}.default",
                "must be one finite Program motion default",
            )
        if parameter_id in parameters:
            value = parameters[parameter_id]
            if (
                parameter_id in _PROGRAM_MOTION_CEILINGS
                and (type(value) not in {int, float} or float(value) > float(ceiling))
            ):
                _program_error(
                    f"{path}.parameters.{parameter_id}",
                    f"must not exceed Program {parameter_id}={float(ceiling):g}",
                )
            if parameter_id not in value_sources:
                continue
        source = value_sources.get(parameter_id)
        if source != {
            "kind": "program_parameter",
            "reference": parameter_id,
        }:
            _program_error(
                f"{path}.value_sources.{parameter_id}",
                f"must default to the Program {parameter_id} parameter",
            )

    _validate_node_capabilities(entry, parameters, path=path, context=context)
    _validate_catalog_blackboard_bindings(entry, parameters, path=path, context=context)
    _validate_key_typed_parameters(entry, parameters, path=path, context=context)
    _validate_node_value_sources(
        entry,
        instance_id,
        parameters,
        value_sources,
        source_paths,
        path=path,
        context=context,
    )
    # Cross-field checks read literals; a parameter supplied whole by a source
    # is checked when its value is resolved at node start.
    _validate_node_cross_fields(
        node_id,
        {
            name: value
            for name, value in parameters.items()
            if name not in value_sources
        },
        sourced=frozenset(str(segments[0]) for segments in source_paths.values()),
        path=path,
        context=context,
    )

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
    recovery_policy, operator_stop_policy = _normalize_canonical_node_policies(
        node, path=path
    )
    execution_groups = parse_execution_groups(
        node.get("execution_groups", "inherit"), path=path
    )
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
        recovery_policy=(
            _freeze_mapping(recovery_policy) if recovery_policy is not None else None
        ),
        operator_stop_policy=operator_stop_policy,
        execution_groups=execution_groups,
    )


def program_value_declarations(
    document: Mapping[str, Any], *, catalog: NodeCatalog, source: str = "program"
) -> tuple[dict[str, ProgramParameterDeclaration], dict[str, BlackboardKeyDeclaration]]:
    """Validate only the declarations expressions read: Program parameters and
    blackboard keys, with the roles and scene their values may name.

    Used where a whole Program is not required, such as checking a formula
    while the rest of a Draft is still being edited.
    """

    root = _program_mapping(dict(document), path=source)
    if "component_roles" not in root:
        _program_error(f"{source}.component_roles", "is required")
    component_roles = _validate_component_roles(
        root["component_roles"], path=f"{source}.component_roles"
    )
    target_roles = _validate_target_roles(
        root.get("target_roles", {}),
        path=f"{source}.target_roles",
        component_roles=component_roles,
    )
    scene_resource_ids, _roles = _validate_scene(
        root.get("scene", []),
        path=f"{source}.scene",
        catalog=catalog,
        component_roles=component_roles,
    )
    parameters = _validate_parameters(
        root.get("parameters", []),
        path=f"{source}.parameters",
        scene_resource_ids=scene_resource_ids,
        target_roles=target_roles,
    )
    _namespaces, keys = _validate_blackboard(
        root.get("blackboard", {"namespaces": [], "keys": []}),
        path=f"{source}.blackboard",
        scene_resource_ids=scene_resource_ids,
        target_roles=target_roles,
    )
    # A Program's tree, or a Draft's authoring forest.
    forest = root.get("forest")
    roots = [root.get("tree"), *(forest if type(forest) is list else [])]
    keys = _declare_node_outcome_keys(keys, roots)
    return parameters, _declare_interrupt_state_keys(keys, roots, root.get("blackboard"))


def check_draft_formulas(
    document: Mapping[str, Any],
    nodes: Iterable[Mapping[str, Any]],
    *,
    catalog: NodeCatalog,
    source: str = "program",
) -> tuple[dict[str, CheckedExpression], dict[tuple[str, str], CheckedExpression]]:
    """Check the formulas of a Draft that need not be a whole Program yet.

    Returns the computed keys by key and the node field formulas by
    ``(node instance, value path)``, typed as Program validation types them.
    A formula that does not check is left out; Program validation reports why.
    Raises ProgramValidationError when the declarations themselves are invalid.
    """

    parameters, keys = program_value_declarations(
        document, catalog=catalog, source=source
    )
    environment = ExpressionEnvironment.from_declarations(parameters, keys)
    computed: dict[str, CheckedExpression] = {}
    for name, declaration in keys.items():
        if declaration.expression is None:
            continue
        try:
            computed[name] = environment.check(declaration.expression, declaration.type)
        except ExpressionError:
            continue
    fields: dict[tuple[str, str], CheckedExpression] = {}
    for node in nodes:
        values = node.get("parameters", {})
        for key, value_source in node.get("value_sources", {}).items():
            if (
                type(value_source) is not dict
                or value_source.get("kind") != "expression"
                or type(value_source.get("expression")) is not str
            ):
                continue
            try:
                segments = parse_value_path(key)
                catalog_parameter = {
                    parameter.id: parameter
                    for parameter in catalog.get(node["node_id"]).parameters
                }[segments[0]]
                schema, key_type = _value_source_destination(
                    catalog_parameter,
                    segments,
                    values,
                    path=key,
                    catalog=catalog,
                    blackboard_keys=keys,
                )
                destination = _expression_destination(
                    catalog.schema_registry.resolve(schema)
                    if schema is not None
                    else None,
                    key_type,
                )
                if destination is None:
                    continue
                fields[(node["instance_id"], key)] = environment.check(
                    value_source["expression"], destination
                )
            # An unknown node or field, or an unbound or undeclared key.
            except (
                ExpressionError,
                KeyError,
                ProgramValidationError,
                ValuePathError,
            ):
                continue
    return computed, fields


@dataclass(frozen=True, slots=True)
class ProgramExpressions:
    """The checked expressions of one validated Program.

    ``computed_key_order`` lists computed keys so each follows the computed
    keys it reads; ``value_expressions`` holds field expressions by
    ``(node instance, value path)``.
    """

    environment: ExpressionEnvironment
    computed_keys: Mapping[str, CheckedExpression]
    computed_key_order: tuple[str, ...]
    value_expressions: Mapping[tuple[str, str], CheckedExpression]


def _check_computed_keys(
    keys: Mapping[str, BlackboardKeyDeclaration],
    expressions: ExpressionEnvironment,
    *,
    path: str,
) -> dict[str, CheckedExpression]:
    checked: dict[str, CheckedExpression] = {}
    for index, (key, declaration) in enumerate(keys.items()):
        if declaration.expression is None:
            continue
        try:
            checked[key] = expressions.check(declaration.expression, declaration.type)
        except ExpressionError as error:
            _program_error(
                f"{path}[{index}].expression",
                describe_diagnostic(error.diagnostics[0]),
            )
    try:
        computed_key_order(checked)
    except ComputedKeyCycleError as error:
        _program_error(path, f"computed keys read each other in a cycle: {error}")
    return checked


def _validate_program_document(
    data: dict[str, Any],
    *,
    catalog: NodeCatalog,
    source: str,
    api_version: str = _PROGRAM_API_VERSION,
) -> tuple[
    dict[str, Any],
    ProgramTreeNode,
    dict[str, ProgramParameterDeclaration],
    dict[str, BlackboardKeyDeclaration],
    "ProgramExpressions",
]:
    root = _program_mapping(data, path=source)
    root = _select_identity_verified_program_root(
        root, source=source, api_version=api_version
    )
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
    if root["api_version"] != api_version:
        _program_error(f"{source}.api_version", f"must be {api_version}")
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
    scene_resource_ids, scene_resource_roles = _validate_scene(
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
            scene_resource_ids=scene_resource_ids,
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
        scene_resource_ids=scene_resource_ids,
        target_roles=target_roles,
    )
    blackboard_keys = _declare_node_outcome_keys(blackboard_keys, [root.get("tree")])
    blackboard_keys = _declare_interrupt_state_keys(
        blackboard_keys, [root.get("tree")], root.get("blackboard")
    )
    expressions = ExpressionEnvironment.from_declarations(parameters, blackboard_keys)
    computed_keys = _check_computed_keys(
        blackboard_keys, expressions, path=f"{source}.blackboard.keys"
    )
    contact_policy_modes = _validate_contact_policies(
        root.get("contact_policies", []),
        path=f"{source}.contact_policies",
        component_roles=component_roles,
        target_roles=target_roles,
        blackboard_keys=blackboard_keys,
    )
    context = _ProgramValidationContext(
        catalog=catalog,
        component_roles=component_roles,
        target_roles=target_roles,
        scene_resource_ids=scene_resource_ids,
        parameters=parameters,
        blackboard_namespaces=namespaces,
        blackboard_keys=blackboard_keys,
        contact_policy_modes=contact_policy_modes,
        expressions=expressions,
        execution_groups_supported=api_version == _PROGRAM_API_VERSION,
    )
    tree = _validate_program_tree(
        root["tree"],
        path=f"{source}.tree",
        depth=0,
        node_budget=[_MAX_TREE_NODES],
        context=context,
    )
    execution_group_policies = _parse_execution_group_policies(
        root.get("execution_groups"),
        path=f"{source}.execution_groups",
    )
    if execution_group_policies and api_version != _PROGRAM_API_VERSION:
        _program_error(
            f"{source}.execution_groups",
            "is unavailable in the previous Program contract",
        )
    _validate_execution_group_policies(tree, execution_group_policies)
    viewer = root.get("viewer")
    if isinstance(viewer, dict) and "hidden_execution_groups" in viewer:
        declared_groups = set(execution_group_policies) | {"all"}
        for name in viewer["hidden_execution_groups"]:
            if name not in declared_groups:
                _program_error(
                    f"{source}.viewer.hidden_execution_groups.{name}",
                    "does not name a declared execution group",
                )
    normalized_root = deepcopy(root)
    normalized_root["tree"] = tree.to_dict()
    if execution_group_policies:
        normalized_root["execution_groups"] = {
            name: {"execution_policy": policy}
            for name, policy in execution_group_policies.items()
        }
    else:
        normalized_root.pop("execution_groups", None)
    try:
        verify_content_identity(
            normalized_root,
            own_field="program_revision",
            identity_domain="program-revision",
            api_version=api_version,
            label=source,
        )
    except StrictArtifactError as exc:
        _program_error(f"{source}.program_revision", str(exc))
    return normalized_root, tree, parameters, blackboard_keys, ProgramExpressions(
        environment=expressions,
        computed_keys=MappingProxyType(computed_keys),
        computed_key_order=computed_key_order(computed_keys),
        value_expressions=MappingProxyType(dict(context.value_expressions)),
    )


class Program:
    """Validated portable ``manyforge.program.v2`` authoring artifact.

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
                "v0 skill expansion is unavailable; v2 Programs contain only "
                "ordinary active-catalog nodes"
            )
        if deployment_capability_keys is not None:
            raise ProgramValidationError(
                "deployment-wide capability unions cannot validate a v2 Program"
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
                "v0 skill expansion is unavailable; v2 Programs contain only "
                "ordinary active-catalog nodes"
            )
        if deployment_capability_keys is not None:
            raise ProgramValidationError(
                "deployment-wide capability unions cannot validate a v2 Program"
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
            (
                normalized,
                tree,
                parameters,
                blackboard_keys,
                expressions,
            ) = _validate_program_document(
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
        self.execution_selections = _resolve_execution_selections(tree)
        self.execution_group_policies = _parse_execution_group_policies(
            normalized.get("execution_groups"),
            path=f"{source}.execution_groups",
        )
        self.coordinated_execution_selections = _validate_execution_group_policies(
            tree, self.execution_group_policies
        )
        self.parameters = MappingProxyType(dict(parameters))
        self.blackboard_keys = MappingProxyType(dict(blackboard_keys))
        self.expressions = expressions
        self.canonical_bytes = canonical
        self.interrupt_condition_keys = _interrupt_condition_keys(tree)
        # The global rule ids this Program reads (`interrupt_state.<rule_id>`).
        self.interrupt_state_rule_ids = tuple(
            key[len(INTERRUPT_STATE_NAMESPACE) + 1 :]
            for key in self.blackboard_keys
            if is_interrupt_state_key(key)
        )

    def with_interrupt_state_keys(self, rule_ids: Iterable[str]) -> "Program":
        """This Program plus the reserved ``interrupt_state.<rule_id>`` key of
        each Applied global rule (contract 64).

        The Runner calls this with the Applied Deployment's global rule ids.
        Keys the Program itself reads are already declared by validation.
        """

        keys = dict(self.blackboard_keys)
        for rule_id in rule_ids:
            declaration = interrupt_state_declaration(rule_id)
            keys[declaration.key] = declaration
        clone = copy(self)
        clone.blackboard_keys = MappingProxyType(dict(sorted(keys.items())))
        return clone

    def validate_key_value(self, key: str, value: Any, *, path: str) -> None:
        """Validate one value against the declared type of ``key``.

        The one check for initial, node, operator and restored values alike:
        pose frames are checked against this Program's declared target roles
        and scene resources, and against the key's ``allowed_frames``.
        """

        declaration = self.blackboard_keys.get(key)
        if declaration is None:
            _program_error(path, f"blackboard key {key!r} is undeclared")
        _validate_declared_type_value(
            declaration.type,
            value,
            path=path,
            scene_resource_ids=self._scene_resource_ids(),
            target_roles=self._data["target_roles"],
        )
        if declaration.type == "stored_pose":
            _require_allowed_frame(value, declaration.allowed_frames, path=path)

    def validate_pose_frame(self, value: Any, *, path: str) -> None:
        """Validate one pose ``frame_ref`` against this Program's declared
        target roles and scene resources (a pose inside a structured value)."""

        _validate_pose_frame_ref(
            value,
            path=path,
            target_roles=self._data["target_roles"],
            scene_resource_ids=self._scene_resource_ids(),
        )

    def _scene_resource_ids(self) -> set[str]:
        return {row["scene_resource_id"] for row in self._data.get("scene", [])}

    def to_dict(self) -> dict[str, Any]:
        """Return one detached exact v2 authoring document."""

        return deepcopy(self._data)


def _interrupt_condition_keys(tree: ProgramTreeNode) -> tuple[str, ...]:
    """The Run's fixed referenced-key union (contract 52, SX-2091): exactly the
    keys the Program's interrupt conditions name as operands, sorted. A
    computed key's own dependencies are not included."""

    keys: set[str] = set()
    pending = [tree]
    while pending:
        node = pending.pop()
        if node.node_id in INTERRUPT_MOTION_NODE_IDS:
            condition = node.parameters.get("interrupt_condition")
            if condition is not None:
                keys |= condition_keys(condition)
        pending.extend(node.children)
    return tuple(sorted(keys))


def migrate_program_v1(data: dict[str, Any], catalog: NodeCatalog) -> Program:
    """Explicit offline conversion; the runtime loader never calls this path.

    Validate the old closed schema and its original identity before creating
    the successor. The input and any existing binding remain untouched.
    """

    if type(data) is not dict:
        raise ProgramValidationError("Program root must be a built-in mapping")
    try:
        canonical_json_bytes(data)
        normalized, *_validated = _validate_program_document(
            deepcopy(data),
            catalog=catalog,
            source="v1 migration",
            api_version=_PREVIOUS_PROGRAM_API_VERSION,
        )
        normalized["api_version"] = _PROGRAM_API_VERSION
        normalized["program_revision"] = compute_content_identity(
            normalized,
            own_field="program_revision",
            identity_domain="program-revision",
            api_version=_PROGRAM_API_VERSION,
        )
    except StrictArtifactError as exc:
        raise ProgramValidationError(str(exc)) from exc
    return Program.from_dict(normalized, catalog)


def migrate_program_execution_groups(
    data: dict[str, Any], catalog: NodeCatalog
) -> Program:
    """Explicitly replace the former synchronization list in one v2 Program."""

    if type(data) is not dict or data.get("api_version") != _PROGRAM_API_VERSION:
        raise ProgramValidationError("execution-group migration requires a v2 Program")
    if "execution_groups" in data:
        raise ProgramValidationError("Program already uses execution group definitions")
    try:
        verify_content_identity(
            data,
            own_field="program_revision",
            identity_domain="program-revision",
            api_version=_PROGRAM_API_VERSION,
            label="execution-group migration input",
        )
    except StrictArtifactError as exc:
        raise ProgramValidationError(str(exc)) from exc
    selected = data.get("synchronized_execution_groups", [])
    synchronized = _parse_synchronized_execution_groups_legacy(
        selected, path="synchronized_execution_groups"
    )
    migrated = deepcopy(data)
    migrated.pop("synchronized_execution_groups", None)
    names: set[str] = set()
    pending = [migrated.get("tree")]
    while pending:
        node = pending.pop()
        if type(node) is not dict:
            raise ProgramValidationError("Program tree is invalid")
        memberships = node.get("execution_groups")
        if type(memberships) is list:
            names.update(memberships)
        children = node.get("children", [])
        if type(children) is not list:
            raise ProgramValidationError("Program tree children are invalid")
        pending.extend(children)
    unknown = set(synchronized) - names
    if unknown:
        raise ProgramValidationError(
            f"synchronized execution groups are unused: {sorted(unknown)}"
        )
    if names:
        migrated["execution_groups"] = {
            name: {
                "execution_policy": (
                    "common_start" if name in synchronized else "independent"
                )
            }
            for name in sorted(names)
        }
    migrated["program_revision"] = compute_content_identity(
        migrated,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version=_PROGRAM_API_VERSION,
    )
    return Program.from_dict(migrated, catalog)


def _parse_synchronized_execution_groups_legacy(
    raw: Any, *, path: str
) -> tuple[str, ...]:
    if (
        type(raw) is not list
        or len(raw) > _MAX_EXECUTION_GROUPS
        or raw != sorted(raw)
        or len(set(raw)) != len(raw)
        or any(
            type(name) is not str
            or len(name) > 128
            or _IDENTIFIER_RE.fullmatch(name) is None
            or name in {"all", "inherit"}
            for name in raw
        )
    ):
        raise ProgramValidationError(f"{path} is invalid")
    return tuple(raw)


__all__ = [
    "BlackboardKey",
    "BlackboardFieldDeclaration",
    "BlackboardKeyDeclaration",
    "Program",
    "ProgramExpressions",
    "ProgramNodePolicy",
    "ProgramParameterDeclaration",
    "ProgramTreeNode",
    "ProgramValidationError",
    "TreeNode",
    "apply_tree_param_overrides",
    "check_draft_formulas",
    "compute_program_tree_hash",
    "find_tree_node_by_name",
    "merge_param_patch",
    "migrate_program_v1",
    "migrate_program_execution_groups",
    "program_node_policies",
    "program_value_declarations",
    "INTERRUPT_MOTION_NODE_IDS",
    "INTERRUPT_STATE_NAMESPACE",
    "INTERRUPT_STATE_VALUES",
    "interrupt_state_declaration",
    "require_interrupt_state_rules",
    "require_interrupt_key_initials",
    "require_global_rule_keys",
    "require_global_rule_operands",
    "GLOBAL_RULE_KEY_UNDECLARED",
    "GLOBAL_RULE_STATE_OPERAND_UNSUPPORTED",
    "INTERRUPT_KEY_INITIAL_REQUIRED",
    "interrupt_state_key",
    "is_interrupt_state_key",
    "NODE_ENDED_OUTCOMES",
    "NODE_OUTCOME_NAMESPACE",
    "NODE_OUTCOME_NODE_IDS",
    "NODE_OUTCOME_NONE",
    "NODE_OUTCOME_VALUES",
    "is_node_outcome_key",
    "node_outcome_key",
    "require_exact_program_binding_node_policies",
    "tree_node_from_dict",
]
