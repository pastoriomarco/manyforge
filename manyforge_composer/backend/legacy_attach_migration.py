"""Type-aware conversion of the removed legacy holder TCP alias."""

from __future__ import annotations

from typing import Any

from manyforge_behavior.tcp_migration import drop_unused_tcp_requirements

from .component_catalog import _profiles


def migrate_legacy_attach(authoring: dict[str, Any], deployment: Any, assignment: Any) -> bool:
    """Convert known removed aliases in an isolated authoring copy."""
    if not isinstance(deployment, dict) or not isinstance(assignment, dict):
        return False
    target_roles = authoring.get("target_roles")
    components = deployment.get("components")
    component_assignments = assignment.get("component_roles")
    target_assignments = assignment.get("target_roles")
    if (not isinstance(target_roles, dict) or not isinstance(components, list)
            or not isinstance(component_assignments, list) or not isinstance(target_assignments, list)):
        return False
    try:
        _model_ids, _families, facts = _profiles()
    except (OSError, ValueError, KeyError, TypeError):
        return False
    roots = authoring.get("forest")
    if roots is None:
        roots = [authoring.get("tree")]
    if not isinstance(roots, list):
        return False
    pending = list(roots)
    converted: set[str] = set()
    while pending:
        node = pending.pop()
        if not isinstance(node, dict):
            continue
        children = node.get("children")
        if isinstance(children, list):
            pending.extend(children)
        if node.get("node_id") != "scene.attach_carried_object":
            continue
        params = node.get("parameters")
        if not isinstance(params, dict) or params.get("carrier_frame_role") != "tcp" or "carrier_frame" in params:
            continue
        sources = node.get("value_sources", {})
        if not isinstance(sources, dict) or any(
            key == "target_role" or key == "carrier_frame_role" for key in sources
        ):
            continue
        role = params.get("target_role")
        target = target_roles.get(role) if isinstance(role, str) else None
        if not isinstance(target, dict):
            continue
        component_role = target.get("component_role")
        assigned_components = [r for r in component_assignments if isinstance(r, dict)
                               and r.get("component_role_id") == component_role]
        assigned_targets = [r for r in target_assignments if isinstance(r, dict)
                            and r.get("target_role_id") == role]
        if len(assigned_components) != 1 or len(assigned_targets) != 1:
            continue
        component_id = assigned_components[0].get("controllable_component_id")
        rows = [r for r in components if isinstance(r, dict) and r.get("component_id") == component_id]
        if len(rows) != 1:
            continue
        component = rows[0]
        profile = facts.get(component.get("component_profile_id"))
        target_id = assigned_targets[0].get("command_target_id")
        if not isinstance(profile, dict) or profile.get("hasMotionTarget") is not False:
            continue
        targets = [item for item in profile["frameTargets"] if item["commandTargetId"] == target_id]
        if len(targets) != 1 or "tcp" in targets[0]["frameRoleIds"]:
            continue
        custom = component.get("custom_tcps", [])
        if not isinstance(custom, list) or any(not isinstance(item, dict) for item in custom):
            continue
        has_custom_tcp = any(item.get("tcp_id") == "tcp" for item in custom)
        target_has_tcps = any(item["commandTargetId"] == target_id for item in profile["tcpTargets"])
        if has_custom_tcp and target_has_tcps:
            continue
        params.pop("carrier_frame_role")
        params["carrier_frame"] = {"kind": "active_tcp"}
        converted.add(role)
    if converted:
        drop_unused_tcp_requirements(authoring)
    return bool(converted)
