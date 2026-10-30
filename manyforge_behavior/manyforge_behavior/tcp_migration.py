"""Portable migration of legacy motion and planning TCP selectors."""

from __future__ import annotations

from typing import Any, Iterable


_SELECTOR_NODES = frozenset({
    "motion.move_to_joint_target",
    "motion.move_to_pose",
    "motion.follow_cartesian_path",
    "motion.move_until_signal",
    "motion.probe_search",
    "planning.plan_pick_sequence",
    "planning.plan_place_sequence",
    "motion.pick_object",
    "motion.place_object",
})


def migrate_tcp_selectors(nodes: Iterable[dict[str, Any]], document: dict[str, Any] | None = None) -> tuple[tuple[str, str], ...]:
    """Convert a bounded legacy tree, or return all ambiguous moves unchanged.

    Each moving robot is considered separately. The caller validates the tree
    bounds and turns an ambiguous result into the typed Load refusal.
    """
    rows: list[tuple[dict[str, Any], dict[str, str], str, str]] = []
    for node in nodes:
        node_id = node.get("node_id")
        if type(node_id) is not str or node_id not in _SELECTOR_NODES:
            continue
        parameters = node.get("parameters")
        if type(parameters) is not dict:
            continue
        endpoint = parameters.get("tcp_endpoint")
        moving = parameters.get("target_role")
        instance = node.get("instance_id")
        if (
            type(endpoint) is dict
            and set(endpoint) == {"target_role", "frame_role"}
            and type(endpoint["target_role"]) is str
            and type(endpoint["frame_role"]) is str
            and type(moving) is str
            and type(instance) is str
        ):
            rows.append((parameters, endpoint, moving, instance))
    selections: dict[str, set[tuple[str, str]]] = {}
    for _, endpoint, moving, _ in rows:
        selections.setdefault(moving, set()).add(
            (endpoint["target_role"], endpoint["frame_role"])
        )
    ambiguous = tuple(sorted(
        (instance, moving)
        for _, endpoint, moving, instance in rows
        if endpoint["target_role"] == moving and endpoint["frame_role"] == "tcp"
        and len(selections[moving]) > 1
    ))
    if ambiguous:
        return ambiguous
    for parameters, endpoint, moving, _ in rows:
        owner, frame = endpoint["target_role"], endpoint["frame_role"]
        if frame == "tcp" and owner == moving:
            parameters["tcp_endpoint"] = {"kind": "current"}
        elif frame == "tcp":
            parameters["tcp_endpoint"] = {"kind": "default_of_role", "role": owner}
        else:
            parameters["tcp_endpoint"] = {"kind": "entry", **endpoint}
    if document is not None:
        drop_unused_tcp_requirements(document)
    return ()


def drop_unused_tcp_requirements(document: dict[str, Any]) -> None:
    """Drop requirements only when no remaining TCP literal can use them.

    References with an explicit target role are scoped; an unscoped literal,
    including a sourced value, conservatively keeps all roles.
    """
    targets = document.get("target_roles")
    if not isinstance(targets, dict):
        return
    roles = set(targets)
    used: set[str] = set()

    def visit(value: Any, uncertain_scope: bool = False,
              carrier_role_key: str | None = None) -> None:
        if isinstance(value, dict):
            sources = value.get("value_sources")
            uncertain_scope = uncertain_scope or (
                isinstance(sources, dict)
                and ("target_role" in sources or "tool_target_role" in sources)
            )
            role = value.get("target_role")
            for key, item in value.items():
                if key == "required_frame_roles":
                    continue
                if item == "tcp":
                    scoped_role = value.get(carrier_role_key) if (
                        key == "carrier_frame_role" and carrier_role_key is not None
                    ) else role if key in {"frame_role", "frame_role_id", "base_frame_role"} else None
                    if isinstance(scoped_role, str) and not uncertain_scope:
                        used.add(scoped_role)
                    else:
                        used.update(roles)
                else:
                    carrier_key = None
                    if key == "parameters":
                        if value.get("node_id") == "scene.attach_carried_object":
                            carrier_key = "target_role"
                        elif value.get("node_id") in (
                            "planning.plan_pick_sequence", "planning.plan_place_sequence"
                        ):
                            carrier_key = "tool_target_role"
                    visit(item, uncertain_scope, carrier_key)
        elif isinstance(value, list):
            for item in value:
                visit(item, uncertain_scope)
        elif value == "tcp":
            used.update(roles)

    visit(document)
    blackboard = document.get("blackboard")
    keys = blackboard.get("keys", []) if isinstance(blackboard, dict) else []
    if isinstance(keys, list):
        for key in keys:
            if not isinstance(key, dict):
                continue
            types = [key.get("type")]
            fields = key.get("fields", [])
            if isinstance(fields, list):
                types.extend(field.get("type") for field in fields if isinstance(field, dict))
            if any(kind in ("tcp_entry", "cartesian_path", "model_response",
                            "pick_planning_result", "scene_selection_context",
                            "scene_selection_result") for kind in types):
                used.update(roles)
            if "stored_pose" in types and (key.get("type") != "stored_pose"
                                           or "allowed_frames" not in key):
                used.update(roles)
    for role in roles - used:
        target = targets.get(role)
        if isinstance(target, dict) and isinstance(target.get("required_frame_roles"), list):
            target["required_frame_roles"] = [
                frame for frame in target["required_frame_roles"] if frame != "tcp"
            ]
