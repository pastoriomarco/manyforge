"""Composer-owned incomplete Program source and deterministic candidate derivation."""

from __future__ import annotations

from copy import deepcopy
import math
import re
import uuid
from typing import Any, Mapping, NoReturn

from manyforge_behavior.canonical_artifact import compute_content_identity
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.catalog_schema import validate_value
from manyforge_behavior.value_path import ValuePathError, parse_value_path
from manyforge_behavior.tcp_migration import migrate_tcp_selectors
from manyforge_behavior.program import (
    Program,
    ProgramValidationError,
    TcpEndpointAliasAmbiguous,
    catalog_version_compatible,
    check_draft_formulas,
)
from manyforge_behavior.program_expressions import CheckedExpression


AUTHORING_FOREST_VERSION = "manyforge.composer_authoring_forest.v1"
PROGRAM_API_VERSION = "manyforge.program.v2"
MAXIMUM_NODES = 100_000
MAXIMUM_DEPTH = 256
MAXIMUM_POSITIONS = MAXIMUM_NODES
MAXIMUM_POSITION_MAGNITUDE = 10_000_000.0
_PROGRAM_REVISION_PLACEHOLDER = "sha256:" + ("0" * 64)
_PROGRAM_PATH = re.compile(r"^(?P<path>[^ ]+)(?: (?P<message>.*))?$")


class AuthoringForestError(RuntimeError):
    """One accepted-edit invariant or supplied field value is invalid."""

    def __init__(self, code: str, message: str, *, detail: Any | None = None) -> None:
        super().__init__(message)
        self.code = code
        self.detail = message if detail is None else detail


def _fail(code: str, message: str, *, detail: Any | None = None) -> NoReturn:
    raise AuthoringForestError(code, message, detail=detail)


def from_program(program: Program) -> dict[str, Any]:
    """Map one strict Program to the sole canonical one-root authoring source."""

    document = program.to_dict()
    root = document.pop("tree")
    document.pop("program_revision")
    return {
        "schema_version": AUTHORING_FOREST_VERSION,
        **document,
        "forest": [root],
        "positions": {},
    }


def adopt_active_catalog(authoring: dict[str, Any], catalog: NodeCatalog) -> None:
    """Select the active catalog version in one loaded authoring source.

    Only an older compatible ``content_version`` (same major, lower minor) is
    replaced, so Save writes the active version. Every other field, and any
    other selector, is kept for the normal validator to report.
    """

    selected = authoring.get("catalog")
    if (
        type(selected) is dict
        and selected.get("content_version") != catalog.content_version
        and catalog_version_compatible(
            selected.get("content_version"), catalog.content_version
        )
    ):
        if selected.get("content_version") == "2.8.0" and catalog.content_version == "2.9.0":
            roots = authoring.get("forest")
            if type(roots) is list and all(type(root) is dict for root in roots):
                ambiguous = migrate_tcp_selectors(
                    (node for node, _path, _parent in _walk(roots)), authoring
                )
                if ambiguous:
                    raise TcpEndpointAliasAmbiguous(ambiguous)
        selected["content_version"] = catalog.content_version


def empty_authoring(catalog: NodeCatalog) -> dict[str, Any]:
    """Create a retained New Draft without inventing a placeholder Program tree."""

    return {
        "schema_version": AUTHORING_FOREST_VERSION,
        "api_version": PROGRAM_API_VERSION,
        "program_id": f"program-{uuid.uuid4().hex}",
        "program_profile": "composable-cell-v1",
        "name": "Untitled Program",
        "catalog": {
            "api_version": catalog.api_version,
            "content_version": catalog.content_version,
        },
        "component_roles": {},
        "target_roles": {},
        "parameters": [
            {
                "parameter_id": "acceleration_scale",
                "type": "float64",
                "default": 1.0,
                "constraints": {"min_exclusive": 0.0, "max_inclusive": 1.0},
                "description": "Default acceleration scale for motion nodes.",
            },
            {
                "parameter_id": "blend_radius_m",
                "type": "float64",
                "default": 0.0,
                "constraints": {"min_inclusive": 0.0},
                "description": "Default Cartesian corner blend radius (m); 0 keeps exact corners.",
            },
            {
                "parameter_id": "translation_tol_m",
                "type": "float64",
                "default": 0.005,
                "constraints": {"min_exclusive": 0.0},
                "description": "Default Cartesian translation tolerance for motion nodes.",
            },
            {
                "parameter_id": "velocity_scale",
                "type": "float64",
                "default": 1.0,
                "constraints": {"min_exclusive": 0.0, "max_inclusive": 1.0},
                "description": "Default velocity scale for motion nodes.",
            },
        ],
        "forest": [],
        "positions": {},
    }


def _program_source(authoring: dict[str, Any], root: dict[str, Any]) -> dict[str, Any]:
    document = {
        key: deepcopy(value)
        for key, value in authoring.items()
        if key not in {"schema_version", "forest", "positions"}
    }
    document["program_revision"] = _PROGRAM_REVISION_PLACEHOLDER
    document["tree"] = deepcopy(root)
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version=PROGRAM_API_VERSION,
    )
    return document


def _diagnostic(code: str, path: str, message: str) -> dict[str, str]:
    return {
        "code": code,
        "path": path,
        "message": message,
        "severity": "error",
    }


def _program_diagnostic(message: str) -> dict[str, str]:
    normalized = message.replace("<composer-authoring-candidate>.tree", "forest[0]")
    normalized = normalized.replace("<composer-authoring-candidate>", "program")
    match = _PROGRAM_PATH.match(normalized)
    path = match.group("path") if match else "program"
    detail = match.group("message") if match and match.group("message") else normalized
    return _diagnostic("authoring_program_invalid", path, detail)


def _walk(
    roots: list[dict[str, Any]],
) -> list[tuple[dict[str, Any], str, str | None]]:
    result: list[tuple[dict[str, Any], str, str | None]] = []
    object_ids: set[int] = set()
    pending: list[tuple[dict[str, Any], str, str | None, int]] = [
        (root, f"forest[{index}]", None, 0)
        for index, root in reversed(list(enumerate(roots)))
    ]
    while pending:
        node, path, parent_id, depth = pending.pop()
        object_id = id(node)
        if object_id in object_ids:
            _fail(
                "composer_authoring_forest_cycle_invalid",
                "authoring forest contains a repeated node or container cycle",
                detail={"path": path},
            )
        object_ids.add(object_id)
        if depth > MAXIMUM_DEPTH:
            _fail(
                "composer_authoring_forest_depth_exceeded",
                "authoring forest exceeds the bounded depth",
                detail={"path": path, "maximumDepth": MAXIMUM_DEPTH},
            )
        result.append((node, path, parent_id))
        if len(result) > MAXIMUM_NODES:
            _fail(
                "composer_authoring_forest_size_exceeded",
                "authoring forest exceeds the bounded node count",
            )
        children = node.get("children")
        if type(children) is not list:
            _fail(
                "composer_authoring_node_shape_invalid",
                "authoring node children must be a list",
                detail={"path": f"{path}.children"},
            )
        instance_id = node.get("instance_id")
        for index in range(len(children) - 1, -1, -1):
            child = children[index]
            if type(child) is not dict:
                _fail(
                    "composer_authoring_node_shape_invalid",
                    "authoring node children must contain objects",
                    detail={"path": f"{path}.children[{index}]"},
                )
            pending.append(
                (child, f"{path}.children[{index}]", instance_id, depth + 1)
            )
    return result


def validate_invariants(authoring: dict[str, Any], catalog: NodeCatalog) -> None:
    """Reject only states that cannot be a bounded ordered forest."""

    if type(authoring) is not dict or authoring.get("schema_version") != AUTHORING_FOREST_VERSION:
        _fail(
            "composer_authoring_forest_invalid",
            "authoring forest has an unsupported schema",
        )
    roots = authoring.get("forest")
    if type(roots) is not list or any(type(root) is not dict for root in roots):
        _fail(
            "composer_authoring_forest_invalid",
            "authoring forest roots must be a list of objects",
        )
    rows = _walk(roots)
    ids: set[str] = set()
    for node, path, _parent_id in rows:
        instance_id = node.get("instance_id")
        node_id = node.get("node_id")
        if type(instance_id) is not str or not instance_id:
            _fail(
                "composer_authoring_node_shape_invalid",
                "authoring node instance_id must be a non-empty string",
                detail={"path": f"{path}.instance_id"},
            )
        if instance_id in ids:
            _fail(
                "composer_authoring_node_duplicate",
                f"authoring node identity is duplicated: {instance_id}",
                detail={"path": f"{path}.instance_id", "instanceId": instance_id},
            )
        ids.add(instance_id)
        if type(node_id) is not str or not node_id:
            _fail(
                "composer_authoring_node_shape_invalid",
                "authoring node node_id must be a non-empty string",
                detail={"path": f"{path}.node_id"},
            )
        try:
            entry = catalog.get(node_id)
        except KeyError:
            _fail(
                "composer_authoring_node_unknown",
                f"authoring node is absent from the active catalog: {node_id}",
                detail={"path": f"{path}.node_id", "nodeId": node_id},
            )
        children = node["children"]
        if len(children) > entry.children_max:
            _fail(
                "composer_authoring_child_capacity_exceeded",
                f"{instance_id} accepts at most {entry.children_max} children",
                detail={"path": f"{path}.children", "instanceId": instance_id},
            )
        parameters = node.get("parameters")
        sources = node.get("value_sources", {})
        if type(parameters) is not dict or type(sources) is not dict:
            _fail(
                "composer_authoring_node_shape_invalid",
                "authoring node parameters and value_sources must be objects",
                detail={"path": path},
            )
        known = {parameter.id: parameter for parameter in entry.parameters}
        source_roots: set[str] = set()
        for source_path in sources:
            try:
                source_roots.add(str(parse_value_path(source_path)[0]))
            except ValuePathError as error:
                _fail(
                    "composer_authoring_value_path_invalid",
                    f"{instance_id} value source {source_path!r}: {error}",
                    detail={"path": path, "valuePath": source_path, "reason": error.code},
                )
        unknown = sorted((set(parameters) | source_roots) - set(known))
        if unknown:
            _fail(
                "composer_authoring_parameter_unknown",
                f"{instance_id} supplies unknown parameters: {', '.join(unknown)}",
                detail={"path": path, "parameterIds": unknown},
            )
        for parameter_id, value in parameters.items():
            issues = validate_value(
                known[parameter_id].schema,
                value,
                registry=catalog.schema_registry,
                path=f"{path}.parameters.{parameter_id}",
            )
            if issues:
                issue = issues[0]
                _fail(
                    "composer_authoring_field_invalid",
                    issue.message,
                    detail={"path": issue.path, "message": issue.message},
                )

    positions = authoring.get("positions")
    if type(positions) is not dict or len(positions) > MAXIMUM_POSITIONS:
        _fail(
            "composer_authoring_positions_invalid",
            "authoring positions must be one bounded object",
        )
    unknown_positions = sorted(set(positions) - ids)
    if unknown_positions:
        _fail(
            "composer_authoring_positions_invalid",
            "authoring positions name unknown nodes",
            detail={"instanceIds": unknown_positions},
        )
    for instance_id, position in positions.items():
        if (
            type(position) is not dict
            or set(position) != {"x", "y"}
            or any(type(position[axis]) not in {int, float} for axis in ("x", "y"))
            or any(not math.isfinite(float(position[axis])) for axis in ("x", "y"))
            or any(abs(float(position[axis])) > MAXIMUM_POSITION_MAGNITUDE for axis in ("x", "y"))
        ):
            _fail(
                "composer_authoring_positions_invalid",
                f"authoring position is invalid for {instance_id}",
                detail={"instanceId": instance_id},
            )


def derive_candidate(
    authoring: dict[str, Any], catalog: NodeCatalog
) -> tuple[Program | None, list[dict[str, str]]]:
    """Run one deterministic full pass and return diagnostics plus candidate."""

    validate_invariants(authoring, catalog)
    roots = authoring["forest"]
    diagnostics: list[dict[str, str]] = []
    if len(roots) != 1:
        diagnostics.append(
            _diagnostic(
                "authoring_forest_root_count",
                "forest",
                f"Program requires exactly one root; found {len(roots)}.",
            )
        )

    for node, path, _parent_id in _walk(roots):
        entry = catalog.get(node["node_id"])
        child_count = len(node["children"])
        if child_count < entry.children_min:
            diagnostics.append(
                _diagnostic(
                    "authoring_node_children_incomplete",
                    f"{path}.children",
                    f"Requires at least {entry.children_min} children; found {child_count}.",
                )
            )
        supplied = set(node.get("parameters", {})) | set(node.get("value_sources", {}))
        for parameter in entry.parameters:
            if parameter.required and not parameter.has_default and parameter.id not in supplied:
                diagnostics.append(
                    _diagnostic(
                        "authoring_parameter_required",
                        f"{path}.parameters.{parameter.id}",
                        "Required value is missing.",
                    )
                )

    candidate: Program | None = None
    if len(roots) == 1:
        try:
            candidate = Program.from_dict(
                _program_source(authoring, roots[0]),
                catalog,
                path="<composer-authoring-candidate>",
            )
        except ProgramValidationError as error:
            diagnostics.append(_program_diagnostic(str(error)))

    deduplicated: list[dict[str, str]] = []
    seen: set[tuple[str, str, str]] = set()
    for diagnostic in diagnostics:
        identity = (
            diagnostic["code"],
            diagnostic["path"],
            diagnostic["message"],
        )
        if identity in seen:
            continue
        seen.add(identity)
        deduplicated.append(diagnostic)
    deduplicated.sort(key=lambda item: (item["path"], item["code"], item["message"]))
    return (candidate if not deduplicated else None), deduplicated


def _references_projection(
    computed_keys: Mapping[str, CheckedExpression],
    value_expressions: Mapping[tuple[str, str], CheckedExpression],
) -> dict[str, Any]:
    def names(checked: CheckedExpression) -> list[str]:
        return sorted(f"{source}.{name}" for source, name in checked.references)

    nodes: dict[str, dict[str, list[str]]] = {}
    for (instance_id, path), checked in sorted(value_expressions.items()):
        nodes.setdefault(instance_id, {})[path] = names(checked)
    return {
        "computedKeys": {
            key: names(checked) for key, checked in sorted(computed_keys.items())
        },
        "nodes": nodes,
    }


def formula_references(authoring: dict[str, Any], catalog: NodeCatalog) -> dict[str, Any]:
    """The `param.`/`key.` names each formula of the forest reads, per the engine.

    Computed keys by key and node formulas by instance and value path; a
    formula that does not check is absent (its diagnostics say why).
    """

    try:
        computed, fields = check_draft_formulas(
            authoring,
            (node for node, _path, _parent in _walk(authoring["forest"])),
            catalog=catalog,
        )
    except ProgramValidationError:
        computed, fields = {}, {}
    return _references_projection(computed, fields)


def program_formula_references(program: Program | None) -> dict[str, Any]:
    """:func:`formula_references` of a validated Program, from its own checks."""

    if program is None:
        return _references_projection({}, {})
    return _references_projection(
        program.expressions.computed_keys, program.expressions.value_expressions
    )


__all__ = [
    "AUTHORING_FOREST_VERSION",
    "AuthoringForestError",
    "derive_candidate",
    "empty_authoring",
    "formula_references",
    "from_program",
    "program_formula_references",
    "validate_invariants",
]
