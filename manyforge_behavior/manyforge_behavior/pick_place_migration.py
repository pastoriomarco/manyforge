"""Explicit offline migration from composite Pick/Place nodes to ordinary subtrees."""

from __future__ import annotations

from copy import deepcopy
from typing import Any

from manyforge_behavior.canonical_artifact import (
    StrictArtifactError,
    canonical_json_bytes,
    compute_content_identity,
    verify_content_identity,
)
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.pick_place_templates import (
    PICK_PLACE_RESULT_FIELDS,
    PickPlaceTemplateError,
    build_pick_place_subtree,
)
from manyforge_behavior.program import (
    Program,
    ProgramValidationError,
    TcpEndpointAliasAmbiguous,
)
from manyforge_behavior.tcp_migration import migrate_tcp_selectors


_PROGRAM_API_VERSION = "manyforge.program.v2"
_LEGACY_CATALOG_API_VERSION = "manyforge.node_catalog.v2"
_LEGACY_CATALOG_CONTENT_VERSION = "2.2.0"
_MAX_TREE_NODES = 100_000
_WRAPPER_FIELDS = (
    "metadata",
    "recovery_policy",
    "operator_stop_policy",
    "execution_groups",
)
_COMPOSITE_IDS = {
    "motion.pick_object": "pick",
    "motion.place_object": "place",
}


def _error(message: str) -> None:
    raise ProgramValidationError(f"Pick/Place migration: {message}")


def _mapping(value: Any, *, field: str) -> dict[str, Any]:
    if type(value) is not dict:
        _error(f"{field} must be a mapping")
    return value


def _list(value: Any, *, field: str) -> list[Any]:
    if type(value) is not list:
        _error(f"{field} must be a list")
    return value


def _collect_instance_ids(root: Any) -> set[str]:
    result: set[str] = set()
    pending = [root]
    visited = 0
    while pending:
        node = _mapping(pending.pop(), field="tree node")
        visited += 1
        if visited > _MAX_TREE_NODES:
            _error("tree node count exceeds the migration bound")
        instance_id = node.get("instance_id")
        if type(instance_id) is not str or not instance_id:
            _error("every tree node must have a non-empty instance_id")
        if instance_id in result:
            _error(f"duplicate tree instance_id {instance_id!r}")
        result.add(instance_id)
        pending.extend(reversed(_list(node.get("children"), field=f"{instance_id}.children")))
    return result


def _pick_touch_body_roles(
    document: dict[str, Any], parameters: dict[str, Any]
) -> list[str]:
    tool_target_role = parameters.get("tool_target_role")
    target_roles = _mapping(document.get("target_roles"), field="target_roles")
    target = _mapping(
        target_roles.get(tool_target_role),
        field=f"target_roles.{tool_target_role}",
    )
    component_role = target.get("component_role")
    required_roles = target.get("required_collision_body_roles")
    if type(component_role) is not str or type(required_roles) is not list:
        _error(
            f"tool target role {tool_target_role!r} does not declare collision-body roles"
        )
    capabilities = _list(
        target.get("required_capabilities"),
        field=f"target_roles.{tool_target_role}.required_capabilities",
    )
    if not any(
        type(row) is dict and row.get("capability_id") == "model.kinematics.fk"
        for row in capabilities
    ):
        capabilities.append(
            {
                "capability_id": "model.kinematics.fk",
                "parameters": {},
                "disposition": "required",
            }
        )
        capabilities.sort(key=lambda row: row["capability_id"])
    policy_id = parameters.get("contact_policy_id")
    matches = [
        row
        for row in _list(document.get("contact_policies"), field="contact_policies")
        if type(row) is dict and row.get("policy_id") == policy_id
    ]
    if len(matches) != 1:
        _error(f"contact policy {policy_id!r} is not uniquely declared")

    policy_roles: set[str] = set()
    for zone in _list(matches[0].get("zones"), field=f"contact policy {policy_id}.zones"):
        zone_row = _mapping(zone, field=f"contact policy {policy_id} zone")
        for pair in _list(
            zone_row.get("body_pairs"),
            field=f"contact policy {policy_id} body_pairs",
        ):
            pair_row = _mapping(pair, field=f"contact policy {policy_id} body pair")
            for side_name in ("body_a", "body_b"):
                side = _mapping(
                    pair_row.get(side_name),
                    field=f"contact policy {policy_id} {side_name}",
                )
                if (
                    side.get("kind") == "component_model_body"
                    and side.get("component_role_id") == component_role
                    and type(side.get("collision_body_role_id")) is str
                ):
                    policy_roles.add(side["collision_body_role_id"])
    touch_roles = sorted(policy_roles.intersection(required_roles))
    if not touch_roles:
        _error(
            f"contact policy {policy_id!r} does not identify a touch body for "
            f"tool target role {tool_target_role!r}"
        )
    return touch_roles


def _result_declaration(key: str) -> dict[str, Any]:
    return {
        "key": key,
        "type": "pick_planning_result",
        "access": "read_write",
        "persistence": "run",
        "fields": deepcopy(PICK_PLACE_RESULT_FIELDS),
    }


def migrate_composite_pick_place_program(
    data: dict[str, Any], catalog: NodeCatalog
) -> Program:
    """Create a current Program; never mutate or silently execute the source."""

    if type(data) is not dict:
        _error("Program root must be a mapping")
    source = deepcopy(data)
    try:
        canonical_json_bytes(source)
        verify_content_identity(
            source,
            own_field="program_revision",
            identity_domain="program-revision",
            api_version=_PROGRAM_API_VERSION,
            label="Pick/Place migration input",
        )
    except StrictArtifactError as exc:
        raise ProgramValidationError(str(exc)) from exc
    if source.get("api_version") != _PROGRAM_API_VERSION:
        _error(f"input api_version must be {_PROGRAM_API_VERSION}")
    source_catalog = _mapping(source.get("catalog"), field="catalog")
    if source_catalog != {
        "api_version": _LEGACY_CATALOG_API_VERSION,
        "content_version": _LEGACY_CATALOG_CONTENT_VERSION,
    }:
        _error(
            "input must name the exact pre-cutover node catalog "
            f"{_LEGACY_CATALOG_API_VERSION}/{_LEGACY_CATALOG_CONTENT_VERSION}"
        )

    occupied_ids = _collect_instance_ids(source.get("tree"))
    # The old aliases must be compared across the entire original tree before
    # a composite is expanded into several ordinary nodes.
    pending = [source["tree"]]
    nodes = []
    while pending:
        node = pending.pop()
        nodes.append(node)
        pending.extend(reversed(node["children"]))
    ambiguous = migrate_tcp_selectors(nodes)
    if ambiguous:
        raise TcpEndpointAliasAmbiguous(ambiguous)
    blackboard = _mapping(source.get("blackboard"), field="blackboard")
    namespaces = _list(blackboard.get("namespaces"), field="blackboard.namespaces")
    keys = _list(blackboard.get("keys"), field="blackboard.keys")
    occupied_keys = {
        row.get("key") for row in keys if type(row) is dict and type(row.get("key")) is str
    }
    generated_keys: list[str] = []
    planning_output_keys: set[str] = set()

    def unique_child_prefix(instance_id: str, suffixes: tuple[str, ...]) -> str:
        candidate = instance_id
        serial = 1
        while any(f"{candidate}.{suffix}" in occupied_ids for suffix in suffixes):
            candidate = (
                f"{instance_id}.generated"
                if serial == 1
                else f"{instance_id}.generated{serial}"
            )
            serial += 1
        occupied_ids.update(f"{candidate}.{suffix}" for suffix in suffixes)
        return candidate

    def unique_result_key(instance_id: str) -> str:
        base = f"pick_place.{instance_id}"
        candidate = base
        serial = 2
        while candidate in occupied_keys:
            candidate = f"{base}.{serial}"
            serial += 1
        occupied_keys.add(candidate)
        generated_keys.append(candidate)
        return candidate

    visited = 0
    converted_count = 0

    def convert(raw_node: Any) -> dict[str, Any]:
        nonlocal converted_count, visited
        node = _mapping(raw_node, field="tree node")
        visited += 1
        if visited > _MAX_TREE_NODES:
            _error("tree node count exceeds the migration bound")
        node_id = node.get("node_id")
        if node_id in {"planning.plan_pick_sequence", "planning.plan_place_sequence"}:
            output_key = _mapping(
                node.get("parameters"), field=f"{node.get('instance_id')}.parameters"
            ).get("output_key")
            if type(output_key) is str:
                planning_output_keys.add(output_key)
        kind = _COMPOSITE_IDS.get(node_id)
        if kind is None:
            migrated = deepcopy(node)
            migrated["children"] = [
                convert(child)
                for child in _list(
                    node.get("children"), field=f"{node.get('instance_id')}.children"
                )
            ]
            return migrated

        instance_id = node.get("instance_id")
        converted_count += 1
        if type(instance_id) is not str or not instance_id:
            _error("composite node has no valid instance_id")
        children = _list(node.get("children"), field=f"{instance_id}.children")
        if children:
            _error(f"{instance_id} composite node unexpectedly has children")
        parameters = _mapping(node.get("parameters"), field=f"{instance_id}.parameters")
        sources = node.get("value_sources", {})
        sources = _mapping(sources, field=f"{instance_id}.value_sources")
        suffixes = (
            "plan",
            "approach",
            "descend",
            "close" if kind == "pick" else "open",
            "attach" if kind == "pick" else "detach",
            "retreat",
        )
        result_key = unique_result_key(instance_id)
        child_prefix = unique_child_prefix(instance_id, suffixes)
        wrapper_fields = {
            field: deepcopy(node[field]) for field in _WRAPPER_FIELDS if field in node
        }
        try:
            subtree = build_pick_place_subtree(
                kind,
                instance_id=instance_id,
                child_prefix=child_prefix,
                parameters=parameters,
                value_sources=sources,
                result_key=result_key,
                touch_body_roles=(
                    _pick_touch_body_roles(source, parameters)
                    if kind == "pick"
                    else ()
                ),
                wrapper_fields=wrapper_fields,
            )
        except PickPlaceTemplateError as exc:
            _error(f"{instance_id}: {exc}")
        planning_output_keys.add(result_key)
        return subtree

    source["tree"] = convert(source.get("tree"))
    if converted_count == 0:
        _error("input contains no composite Pick/Place node")
    namespace_rows = {
        row.get("namespace_id"): row for row in namespaces if type(row) is dict
    }
    namespace = namespace_rows.get("pick_place")
    if namespace is None:
        namespaces.append({"namespace_id": "pick_place", "owner": "program"})
    elif namespace.get("owner") != "program":
        _error("existing pick_place namespace is not Program-owned")
    keys.extend(_result_declaration(key) for key in generated_keys)
    for row in keys:
        if type(row) is dict and row.get("key") in planning_output_keys:
            if row.get("type") != "pick_planning_result":
                _error(f"planning output key {row.get('key')!r} has an incompatible type")
            row["fields"] = deepcopy(PICK_PLACE_RESULT_FIELDS)
    namespaces.sort(key=lambda row: row["namespace_id"])
    keys.sort(key=lambda row: row["key"])
    source["catalog"] = {
        "api_version": catalog.api_version,
        "content_version": catalog.content_version,
    }
    source["program_revision"] = compute_content_identity(
        source,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version=_PROGRAM_API_VERSION,
    )
    return Program.from_dict(source, catalog, path="<pick-place-migration>")


__all__ = ["migrate_composite_pick_place_program"]
