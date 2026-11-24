#!/usr/bin/env python3
"""Convert a Scenario saved before collision rules to the current file formats.

    python3 scripts/convert_scenario_od46.py <Scenario folder | Program file> [...]
    python3 scripts/convert_scenario_od46.py --dry-run <path> [...]

A Scenario folder is converted as a whole: `program.yaml` (Program v3 -> v4),
`deployment.yaml` (Deployment v2 -> v3) and the member digests in
`scenario.yaml`. A single Program file is converted alone. Files already in
the current format are left alone. When something cannot be converted, the
script names it, writes nothing for that path and exits with status 1.

What changes:

Deployment
- the robots' `collision_clearance_m` becomes the first collision rule
  (`collision_rules[0]`, `rule_id: everything`); 1 mm when no robot set it;
- gripper `touch_links` are removed (the component type's grip body replaces
  them).

Program
- a node's `clearance_m` becomes its collision rule without selections;
- a node's `contact_policy_id` (with `contact_scene_resource_id`) becomes
  collision rules on that node: a contact-grasp policy a `contact` rule, a
  contact-release policy a `release` rule; on a pick sequence the rule is for
  the descend leg;
- the Program's `contact_policies` list is removed;
- a Scene object's `support_contacts` become rules on single moves: a
  `release` rule on the lift (the first move after the object is attached, and
  the retreat leg of its pick sequence) and a `contact` rule on the last place
  move (the last move before the object is detached, and the descend leg of
  its place sequence);
- the node catalog pin becomes 4.0.0.

The script needs only Python 3 and PyYAML.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Any

import yaml

PROGRAM_V3 = "manyforge.program.v3"
PROGRAM_V4 = "manyforge.program.v4"
DEPLOYMENT_V2 = "manyforge.deployment_intent.v2"
DEPLOYMENT_V3 = "manyforge.deployment_intent.v3"
CATALOG_OLD = "3.0.0"
CATALOG_NEW = "4.0.0"
DEFAULT_CLEARANCE_M = 0.001
EVERYTHING_RULE_ID = "everything"
DEFAULT_END_FROM_FRACTION = 0.8
WHOLE_MOVE_END_FROM_FRACTION = 0  # a contact row that may touch from the first sample
MAX_RULES = 64

MOTION_NODE_IDS = frozenset({
    "motion.move_to_joint_target",
    "motion.move_to_pose",
    "motion.follow_cartesian_path",
    "motion.move_until_signal",
    "motion.probe_search",
})
PICK_SEQUENCE = "planning.plan_pick_sequence"
PLACE_SEQUENCE = "planning.plan_place_sequence"
ATTACH = "scene.attach_carried_object"
DETACH = "scene.detach_carried_object"
RELEASE_KEYS = (
    "release_evidence_mode",
    "enforce_max_start_penetration",
    "max_start_penetration_m",
    "min_release_progress_m",
    "release_tolerance_m",
    "require_strictly_decreasing_penetration",
    "require_separation_by_end",
    "allow_recontact",
)


class ConversionError(Exception):
    """Something in the file has no counterpart in the current format."""


# --------------------------------------------------------------------------
# Reading and writing in the file's own style


class _Document:
    """One member file: its value and how to write it back."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.raw = path.read_bytes()
        text = self.raw.decode("utf-8")
        self.is_json = text.lstrip().startswith("{")
        if self.is_json:
            self.value = json.loads(text)
        else:
            self.value = yaml.safe_load(text)
        if not isinstance(self.value, dict):
            raise ConversionError(f"{path}: the file is not a mapping")
        self.original = copy.deepcopy(self.value)
        self.round_trips = self._dump(self.value) == self.raw
        self.sorted_keys = list(self.value) == sorted(self.value)

    def _dump(self, value: Any) -> bytes:
        if self.is_json:
            text = json.dumps(value, indent=2, ensure_ascii=False) + "\n"
        else:
            text = yaml.dump(value, Dumper=yaml.SafeDumper, sort_keys=False, width=100,
                             default_flow_style=False, allow_unicode=True)
        return text.encode("utf-8")

    def changed(self) -> bool:
        return self.value != self.original

    def output(self) -> tuple[bytes, bool]:
        """The new bytes, and whether hand formatting had to be rewritten."""
        if self.round_trips:
            return self._dump(self.value), False
        patched = self._patch_pins()
        if patched is not None:
            return patched, False
        return self._dump(self.value), True

    def _patch_pins(self) -> bytes | None:
        """A file with hand formatting whose only changes are the version
        pins keeps its formatting: the pins are replaced in the text."""
        if self.is_json:
            return None
        text = self.raw.decode("utf-8")
        text = re.sub(rf"(?m)^api_version: {re.escape(PROGRAM_V3)}$",
                      f"api_version: {PROGRAM_V4}", text, count=1)
        text = re.sub(rf"(?m)^(  content_version: )(['\"]?){re.escape(CATALOG_OLD)}\2$",
                      rf"\g<1>\g<2>{CATALOG_NEW}\g<2>", text, count=1)
        try:
            reread = yaml.safe_load(text)
        except yaml.YAMLError:
            return None
        return text.encode("utf-8") if reread == self.value else None


def _insert(mapping: dict[str, Any], key: str, value: Any, position: int | None = None) -> None:
    """Add a key where the mapping's own order expects it: in sorted position
    in a sorted mapping, else at `position` (where a replaced key was), else
    at the end."""
    if key in mapping:
        mapping[key] = value
        return
    keys = list(mapping)
    if keys == sorted(keys):
        position = sum(1 for existing in keys if existing < key)
    elif position is None or position > len(keys):
        position = len(keys)
    items = list(mapping.items())
    items.insert(position, (key, value))
    mapping.clear()
    mapping.update(items)


def _pop(mapping: dict[str, Any], key: str) -> tuple[Any, int | None]:
    if key not in mapping:
        return None, None
    position = list(mapping).index(key)
    return mapping.pop(key), position


def _styled(value: Any, sort: bool) -> Any:
    if isinstance(value, dict):
        items = sorted(value.items()) if sort else value.items()
        return {key: _styled(item, sort) for key, item in items}
    if isinstance(value, list):
        return [_styled(item, sort) for item in value]
    return value


# --------------------------------------------------------------------------
# Deployment


def convert_deployment(document: _Document) -> list[str]:
    value = document.value
    version = value.get("api_version")
    if version == DEPLOYMENT_V3:
        return []
    if version != DEPLOYMENT_V2:
        raise ConversionError(f"{document.path}: api_version {version!r} is not {DEPLOYMENT_V2}")
    notes: list[str] = []
    mounted = {
        attachment.get("child", {}).get("component_id")
        for attachment in value.get("attachments") or []
        if isinstance(attachment, dict) and isinstance(attachment.get("child"), dict)
    }
    clearances: dict[str, float] = {}
    touch_links_removed = 0
    for component in value.get("components") or []:
        if not isinstance(component, dict):
            continue
        component_id = str(component.get("component_id"))
        configuration = component.get("configuration")
        if not isinstance(configuration, dict):
            continue
        runtime = configuration.get("runtime_configuration")
        if not isinstance(runtime, dict):
            continue
        is_robot = (configuration.get("runtime_kind") == "ros2_control"
                    and component_id not in mounted)
        authored, _ = _pop(runtime, "collision_clearance_m")
        if authored is not None:
            clearances[component_id] = authored
        elif is_robot:
            clearances[component_id] = DEFAULT_CLEARANCE_M
        for gripper in runtime.get("gripper_target_configurations") or []:
            if isinstance(gripper, dict) and "touch_links" in gripper:
                del gripper["touch_links"]
                touch_links_removed += 1
    distinct = sorted(set(clearances.values()))
    if len(distinct) > 1:
        listed = ", ".join(f"{component_id}: {clearance} m"
                           for component_id, clearance in sorted(clearances.items()))
        raise ConversionError(
            f"{document.path}: the robots have different collision clearances ({listed}); "
            "the first collision rule holds one value. Set one clearance on all robots, "
            "convert, then add a collision rule per robot in the Deployment.")
    clearance = distinct[0] if distinct else DEFAULT_CLEARANCE_M
    if "collision_rules" in value:
        raise ConversionError(f"{document.path}: a v2 Deployment already has collision_rules")
    _insert(value, "collision_rules",
            [_styled({"rule_id": EVERYTHING_RULE_ID, "clearance_m": clearance},
                     document.sorted_keys)])
    value["api_version"] = DEPLOYMENT_V3
    notes.append(f"first collision rule: clearance {clearance} m")
    if touch_links_removed:
        notes.append(f"touch_links removed from {touch_links_removed} gripper(s)")
    return notes


# --------------------------------------------------------------------------
# Program


def _walk(node: dict[str, Any]):
    yield node
    for child in node.get("children") or []:
        if isinstance(child, dict):
            yield from _walk(child)


def _node_name(node: dict[str, Any]) -> str:
    return f"node {node.get('instance_id', '?')!r} ({node.get('node_id', '?')})"


def _is_motion(node: dict[str, Any]) -> bool:
    return node.get("node_id") in MOTION_NODE_IDS and not node.get("children")


def _scene_object_selection(node: dict[str, Any], parameter: str, where: str) -> dict[str, Any]:
    """The Scene object a node names through `parameter`: a literal id, or
    the blackboard value its value source reads."""
    parameters = node.get("parameters") or {}
    sources = node.get("value_sources") or {}
    if isinstance(parameters.get(parameter), str):
        return {"kind": "scene_object", "scene_resource_id": parameters[parameter]}
    source = sources.get(parameter)
    if isinstance(source, dict) and source.get("kind") == "blackboard_key":
        return {"kind": "scene_object", "blackboard_key": source.get("reference")}
    if isinstance(source, dict) and source.get("kind") == "blackboard_field":
        return {"kind": "scene_object", "blackboard_key": source.get("reference"),
                "field": source.get("field")}
    raise ConversionError(
        f"{where}: {_node_name(node)} uses a contact policy but its {parameter} is neither "
        "a Scene object id nor a blackboard value")


def _robot_selection(selector: dict[str, Any], where: str) -> dict[str, Any]:
    kind = selector.get("kind")
    if kind == "motion_endpoint":
        return {"kind": "active_tcp_contact_bodies"}
    if kind == "component_model_body" and isinstance(selector.get("body"), dict):
        body = selector["body"]
        return {"kind": "collision_body", "component_id": body.get("component_id"),
                "collision_body_id": body.get("collision_body_id")}
    raise ConversionError(f"{where}: contact body selector {selector!r} is not known")


def _policy_rows(policy: dict[str, Any], node: dict[str, Any], where: str) -> list[dict[str, Any]]:
    """The collision rules one contact policy becomes on one node."""
    policy_id = policy.get("policy_id")
    mode = policy.get("mode")
    if mode not in ("contact_grasp", "contact_release"):
        raise ConversionError(f"{where}: contact policy {policy_id!r} has mode {mode!r}")
    is_pick = node.get("node_id") == PICK_SEQUENCE
    object_parameter = "selected_object_id" if is_pick else "contact_scene_resource_id"
    rows: list[dict[str, Any]] = []
    zones = policy.get("zones") or []
    for zone in zones:
        if mode == "contact_grasp":
            contact: dict[str, Any] = {}
            if "max_penetration_m" in zone:
                contact["max_penetration_m"] = zone["max_penetration_m"]
            contact["end_from_fraction"] = zone.get("terminal_window_start_fraction",
                                                    DEFAULT_END_FROM_FRACTION)
            value_key, value = "contact", contact
        else:
            value_key, value = "release", {key: zone[key] for key in RELEASE_KEYS if key in zone}
        # One rule holds every pair of one member of `a` and one of `b`: the
        # zone's pairs are grouped by their object.
        groups: list[tuple[dict[str, Any], list[dict[str, Any]]]] = []
        for pair in zone.get("body_pairs") or []:
            ends = [pair.get("body_a") or {}, pair.get("body_b") or {}]
            objects = [end for end in ends if end.get("kind") in ("runtime_object", "scene_resource")]
            robots = [end for end in ends if end.get("kind") not in ("runtime_object", "scene_resource")]
            if len(objects) != 1 or len(robots) != 1:
                raise ConversionError(
                    f"{where}: contact policy {policy_id!r} has a pair that does not join "
                    "one robot body to one Scene object")
            if objects[0].get("kind") == "scene_resource":
                selected = {"kind": "scene_object",
                            "scene_resource_id": objects[0].get("scene_resource_id")}
            else:
                selected = _scene_object_selection(node, object_parameter, where)
            robot = _robot_selection(robots[0], where)
            for existing_object, existing_robots in groups:
                if existing_object == selected:
                    if robot not in existing_robots:
                        existing_robots.append(robot)
                    break
            else:
                groups.append((selected, [robot]))
        for index, (selected, robots) in enumerate(groups):
            rule_id = str(policy_id)
            if len(zones) > 1 or len(groups) > 1:
                rule_id = f"{policy_id}_{zone.get('zone_id', 'zone')}_{index + 1}"
            row: dict[str, Any] = {"rule_id": rule_id}
            if is_pick:
                row["leg"] = "descend"
            row["a"] = robots
            row["b"] = [selected]
            row[value_key] = copy.deepcopy(value)
            rows.append(row)
    return rows


def _support_rows(entry: dict[str, Any], gripper: str, leg: str | None, lift: bool) -> dict[str, Any]:
    support = entry["support_object_id"]
    bounded = entry.get("mode", "contact_release") == "bounded_penetration"
    row: dict[str, Any] = {"rule_id": f"{'lift_from' if lift else 'place_on'}_{support}"}
    if leg is not None:
        row["leg"] = leg
    row["a"] = [{"kind": "attached_object", "component_id": gripper}]
    row["b"] = [{"kind": "scene_object", "scene_resource_id": support}]
    if bounded:
        # A bounded support contact held for the whole move: fraction 0.
        contact = {}
        if "max_contact_penetration_m" in entry:
            contact["max_penetration_m"] = entry["max_contact_penetration_m"]
        contact["end_from_fraction"] = WHOLE_MOVE_END_FROM_FRACTION
        row["contact"] = contact
    elif lift:
        row["release"] = {key: entry[key] for key in RELEASE_KEYS if key in entry}
    else:
        contact = {}
        if entry.get("enforce_max_start_penetration", True) and \
                entry.get("release_evidence_mode", "signed_penetration") == "signed_penetration":
            contact["max_penetration_m"] = entry.get("max_start_penetration_m", 0.0)
        contact["end_from_fraction"] = DEFAULT_END_FROM_FRACTION
        row["contact"] = contact
    return row


def _add_rows(node: dict[str, Any], rows: list[dict[str, Any]], *, sort: bool,
              position: int | None = None, first: bool = False) -> None:
    if not rows:
        return
    parameters = node.setdefault("parameters", {})
    existing = parameters.get("collision_rules")
    merged = list(existing) if isinstance(existing, list) else []
    used = {row.get("rule_id") for row in merged}
    styled_rows = []
    for row in rows:
        row = copy.deepcopy(row)
        base, suffix = row["rule_id"], 2
        while row["rule_id"] in used:
            row["rule_id"] = f"{base}_{suffix}"
            suffix += 1
        used.add(row["rule_id"])
        styled_rows.append(_styled(row, sort))
    merged = styled_rows + merged if first else merged + styled_rows
    if len(merged) > MAX_RULES:
        raise ConversionError(f"{_node_name(node)} would get more than {MAX_RULES} collision rules")
    _insert(parameters, "collision_rules", merged, position)


def _object_of(node: dict[str, Any], sequences_by_key: dict[str, dict[str, Any]]) -> str | None:
    """The Scene object an attach, detach or sequence node handles, when the
    Program states it; None when it is chosen while the Program runs."""
    parameter = "selected_object_id" if node.get("node_id") in (PICK_SEQUENCE, PLACE_SEQUENCE) \
        else "scene_resource_id"
    literal = (node.get("parameters") or {}).get(parameter)
    if isinstance(literal, str):
        return literal
    source = (node.get("value_sources") or {}).get(parameter)
    if (isinstance(source, dict) and source.get("kind") == "blackboard_field"
            and source.get("field") == "selected_object_id"):
        sequence = sequences_by_key.get(source.get("reference"))
        if sequence is not None and sequence is not node:
            return _object_of(sequence, sequences_by_key)
    return None


def _supports_for(object_id: str | None, supports: dict[str, list[dict[str, Any]]],
                  node: dict[str, Any], where: str) -> list[dict[str, Any]]:
    if object_id is not None:
        return supports.get(object_id, [])
    # The object is chosen at run time: every declared support applies, and
    # they must agree where they name the same support.
    merged: dict[str, dict[str, Any]] = {}
    for entries in supports.values():
        for entry in entries:
            known = merged.setdefault(entry["support_object_id"], entry)
            if known != entry:
                raise ConversionError(
                    f"{where}: {_node_name(node)} handles an object chosen at run time, and "
                    f"the Scene objects declare different support contacts on "
                    f"{entry['support_object_id']!r}; add the collision rules by hand")
    return list(merged.values())


def _strings(value: Any):
    if isinstance(value, dict):
        for item in value.values():
            yield from _strings(item)
    elif isinstance(value, list):
        for item in value:
            yield from _strings(item)
    elif isinstance(value, str):
        yield value


def _drop_unused_policy_keys(value: dict[str, Any], policy_keys: set[Any],
                             notes: list[str]) -> None:
    """A blackboard key that only bound a contact policy to its object, and
    that nothing else in the Program names, is removed with the policies."""
    blackboard = value.get("blackboard")
    keys = blackboard.get("keys") if isinstance(blackboard, dict) else None
    if not isinstance(keys, list):
        return
    for key in sorted(k for k in policy_keys if isinstance(k, str)):
        declarations = [row for row in keys if isinstance(row, dict) and row.get("key") == key]
        if len(declarations) != 1 or "initial" in declarations[0]:
            continue
        rest = copy.deepcopy(value)
        rest["blackboard"]["keys"] = [row for row in keys if row is not declarations[0]]
        token = re.compile(rf"(?<![A-Za-z0-9_.]){re.escape(key)}(?![A-Za-z0-9_.])")
        if any(token.search(text) for text in _strings(rest)):
            continue
        keys.remove(declarations[0])
        notes.append(f"blackboard key {key!r} only bound a contact policy to its object; removed")


def convert_program(document: _Document) -> list[str]:
    value = document.value
    where = str(document.path)
    version = value.get("api_version")
    if version == PROGRAM_V4:
        return []
    if version != PROGRAM_V3:
        raise ConversionError(f"{where}: api_version {version!r} is not {PROGRAM_V3}")
    notes: list[str] = []
    sort = document.sorted_keys
    if isinstance(value.get("forest"), list):
        roots = [root for root in value["forest"] if isinstance(root, dict)]
    elif isinstance(value.get("tree"), dict):
        roots = [value["tree"]]
    else:
        roots = []
    nodes = [node for root in roots for node in _walk(root)]

    # Contact policies and per-move clearance.
    policies: dict[str, dict[str, Any]] = {}
    for policy in value.get("contact_policies") or []:
        if isinstance(policy, dict):
            policies[str(policy.get("policy_id"))] = policy
    used_policies: set[str] = set()
    for node in nodes:
        parameters = node.get("parameters")
        if not isinstance(parameters, dict):
            continue
        sources = node.get("value_sources") if isinstance(node.get("value_sources"), dict) else {}
        for parameter in ("clearance_m", "contact_policy_id"):
            if parameter in sources:
                raise ConversionError(
                    f"{where}: {_node_name(node)} reads {parameter} from "
                    f"{sources[parameter].get('kind', 'a value source')}; collision rule values "
                    "are plain values. Write the value on the node, then convert.")
        rows: list[dict[str, Any]] = []
        position: int | None = None
        policy_id = parameters.get("contact_policy_id")
        if policy_id is not None:
            if policy_id not in policies:
                raise ConversionError(
                    f"{where}: {_node_name(node)} names contact policy {policy_id!r}, which the "
                    "Program does not declare")
            rows = _policy_rows(policies[policy_id], node, where)
            used_policies.add(policy_id)
        elif "contact_scene_resource_id" in parameters or "contact_scene_resource_id" in sources:
            notes.append(f"{_node_name(node)}: contact_scene_resource_id without a contact "
                         "policy removed")
        _, position = _pop(parameters, "contact_policy_id")
        _, object_position = _pop(parameters, "contact_scene_resource_id")
        clearance, clearance_position = _pop(parameters, "clearance_m")
        if position is None:
            position = clearance_position if clearance_position is not None else object_position
        if "contact_scene_resource_id" in sources:
            del sources["contact_scene_resource_id"]
            if not sources:
                del node["value_sources"]
        if clearance is not None:
            rows.insert(0, {"rule_id": EVERYTHING_RULE_ID, "clearance_m": clearance})
        if rows:
            _add_rows(node, rows, sort=sort, position=position)
            notes.append(f"{_node_name(node)}: {len(rows)} collision rule(s) from "
                         + ", ".join(filter(None, [
                             f"contact policy {policy_id!r}" if policy_id is not None else "",
                             "clearance_m" if clearance is not None else ""])))
    if "contact_policies" in value:
        unused = sorted(set(policies) - used_policies)
        if unused:
            notes.append("contact policies no node used, removed: " + ", ".join(unused))
        policy_keys = {
            end.get("runtime_object_id")
            for policy in policies.values() for zone in policy.get("zones") or []
            for pair in zone.get("body_pairs") or []
            for end in (pair.get("body_a") or {}, pair.get("body_b") or {})
            if end.get("kind") == "runtime_object"}
        del value["contact_policies"]
        _drop_unused_policy_keys(value, policy_keys, notes)

    # Support contacts of Scene objects become rules on single moves.
    supports: dict[str, list[dict[str, Any]]] = {}
    for row in value.get("scene") or []:
        if not isinstance(row, dict) or not isinstance(row.get("metadata"), dict):
            continue
        entries = row["metadata"].pop("support_contacts", None)
        if not row["metadata"]:
            del row["metadata"]
        if entries:
            supports[str(row.get("scene_resource_id"))] = [
                entry for entry in entries if isinstance(entry, dict)]
    if supports:
        sequences_by_key = {
            (node.get("parameters") or {}).get("output_key"): node
            for node in nodes if node.get("node_id") in (PICK_SEQUENCE, PLACE_SEQUENCE)}
        sequences_by_key.pop(None, None)
        handled: set[str] = set()
        for index, node in enumerate(nodes):
            node_id = node.get("node_id")
            if node_id not in (PICK_SEQUENCE, PLACE_SEQUENCE, ATTACH, DETACH):
                continue
            object_id = _object_of(node, sequences_by_key)
            entries = _supports_for(object_id, supports, node, where)
            if not entries:
                continue
            parameters = node.get("parameters") or {}
            gripper_key = "tool_target" if node_id in (PICK_SEQUENCE, PLACE_SEQUENCE) else "target"
            gripper = (parameters.get(gripper_key) or {}).get("component_id")
            if not isinstance(gripper, str):
                raise ConversionError(
                    f"{where}: {_node_name(node)} does not name its gripper ({gripper_key}); "
                    "add the collision rules for the carried object by hand")
            lift = node_id in (PICK_SEQUENCE, ATTACH)
            if node_id == PICK_SEQUENCE:
                target, leg = node, "retreat"
            elif node_id == PLACE_SEQUENCE:
                target, leg = node, "descend"
            elif node_id == ATTACH:
                target = next((later for later in nodes[index + 1:] if _is_motion(later)), None)
                leg = None
                if target is None:
                    raise ConversionError(
                        f"{where}: no move follows {_node_name(node)}, so the lift of "
                        f"{object_id or 'the carried object'!r} from its support cannot be "
                        "found; add the release rule by hand")
            else:
                target = next((earlier for earlier in reversed(nodes[:index])
                               if _is_motion(earlier)), None)
                leg = None
                if target is None:
                    notes.append(f"{_node_name(node)}: no move comes before it; no contact "
                                 "rule for the place was written")
                    continue
            rows = [_support_rows(entry, gripper, leg, lift) for entry in entries]
            if not lift and leg is None:
                # One move that both lifts and places keeps its release rule.
                held = {json.dumps(existing.get("b"), sort_keys=True)
                        for existing in (target.get("parameters") or {}).get("collision_rules", [])
                        if "release" in existing}
                kept = [row for row in rows if json.dumps(_styled(row["b"], sort), sort_keys=True)
                        not in held]
                if len(kept) != len(rows):
                    notes.append(f"{_node_name(target)} lifts and places in one move: it keeps "
                                 "the release rule only; check it")
                rows = kept
            _add_rows(target, rows, sort=sort)
            if node_id == ATTACH:
                handled.update(supports if object_id is None else {object_id})
            for row in rows:
                notes.append(f"{_node_name(target)}: rule {row['rule_id']!r} from the support "
                             f"contact of {object_id or 'the carried object'!r}")
        missing = sorted(set(supports) - handled)
        if missing:
            raise ConversionError(
                f"{where}: the Program never attaches {', '.join(repr(m) for m in missing)}, so "
                "the move that lifts it from its support cannot be found; remove the support "
                "contact or add the collision rules by hand")

    catalog = value.get("catalog")
    if isinstance(catalog, dict) and catalog.get("content_version") == CATALOG_OLD:
        catalog["content_version"] = CATALOG_NEW
    value["api_version"] = PROGRAM_V4
    if "program_revision" in value:
        notes.append("program_revision is stale: re-seal the Program "
                     "(scripts/regen.py --update --only program-revisions)")
    return notes


# --------------------------------------------------------------------------
# Paths


def _digest(raw: bytes) -> str:
    return "sha256:" + hashlib.sha256(raw).hexdigest()


def convert_path(path: Path, *, dry_run: bool) -> list[str] | None:
    """Convert one Scenario folder or Program file. Returns what was done, or
    None when everything already is in the current format."""
    notes: list[str] = []
    outputs: dict[Path, bytes] = {}
    if path.is_dir():
        program_path, deployment_path = path / "program.yaml", path / "deployment.yaml"
        manifest_path = path / "scenario.yaml"
        if not program_path.exists() and not deployment_path.exists():
            raise ConversionError(f"{path}: no program.yaml or deployment.yaml in this folder")
        members: dict[str, _Document] = {}
        for member_path, convert in ((program_path, convert_program),
                                     (deployment_path, convert_deployment)):
            if not member_path.exists():
                continue
            if member_path.read_bytes().strip() in (b"null", b"~", b""):
                continue  # the Scenario has no such member
            document = _Document(member_path)
            member_notes = convert(document)
            if document.changed():
                raw, reformatted = document.output()
                outputs[member_path] = raw
                members[member_path.name] = document
                notes += [f"{member_path.name}: {note}" for note in member_notes]
                if reformatted:
                    notes.append(f"{member_path.name}: the file was written in the standard layout")
        if outputs and manifest_path.exists():
            manifest = _Document(manifest_path)
            pins = manifest.value.get("members")
            if isinstance(pins, dict):
                for name in members:
                    if name in pins:
                        pins[name] = _digest(outputs[path / name])
            status = manifest.value.get("saved_status")
            if isinstance(status, dict) and status.get("last_known_status") == "ready":
                status["last_known_status"] = "unknown"
            if manifest.changed():
                outputs[manifest_path] = manifest.output()[0]
            notes.append("scenario.yaml: member digests updated; the checked-content digest is "
                         "renewed by the next Save in the Composer")
    else:
        document = _Document(path)
        notes += convert_program(document)
        if document.changed():
            raw, reformatted = document.output()
            outputs[path] = raw
            if reformatted:
                notes.append("the file was written in the standard layout")
    if not outputs:
        return None
    if not dry_run:
        for output_path, raw in outputs.items():
            output_path.write_bytes(raw)
    return notes


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Convert Scenario folders or Program files saved before collision rules.")
    parser.add_argument("paths", nargs="+", type=Path,
                        help="a Scenario folder (program.yaml, deployment.yaml) or a Program file")
    parser.add_argument("--dry-run", action="store_true",
                        help="report what would change and write nothing")
    arguments = parser.parse_args(argv)
    failed = False
    for path in arguments.paths:
        try:
            if not path.exists():
                raise ConversionError(f"{path}: no such file or folder")
            notes = convert_path(path, dry_run=arguments.dry_run)
        except (ConversionError, OSError, ValueError, yaml.YAMLError) as error:
            failed = True
            print(f"NOT CONVERTED {error}", file=sys.stderr)
            continue
        if notes is None:
            print(f"{path}: already in the current format")
            continue
        print(f"{path}: " + ("would convert" if arguments.dry_run else "converted"))
        for note in notes:
            print(f"  {note}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
