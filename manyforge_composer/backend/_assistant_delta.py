"""Pure authoring-document tree/scene delta reduction.

This module owns no conversations, caches, Composer state, callback response,
or assistant wire. Session owners may feed explicit snapshots into this
bounded reducer after the new Program and scene contracts are qualified.
"""
from __future__ import annotations

from typing import Any


def _index_tree(tree_doc: Any) -> dict[str, dict[str, Any]]:
    """Index a tree by node name → {id, parent, position, params}."""
    out: dict[str, dict[str, Any]] = {}
    if not isinstance(tree_doc, dict):
        return out
    root = tree_doc.get("tree") if isinstance(tree_doc.get("tree"), dict) else tree_doc

    def walk(node: Any, parent_name: str | None, position: int) -> None:
        if not isinstance(node, dict):
            return
        name = node.get("name")
        if isinstance(name, str) and name:
            out[name] = {
                "id": node.get("id"),
                "parent": parent_name,
                "position": position,
                "params": node.get("params") or {},
            }
        children = node.get("children") if isinstance(node.get("children"), list) else []
        for i, child in enumerate(children):
            walk(child, name if isinstance(name, str) else None, i)

    walk(root, None, 0)
    return out


def _index_scene(scene_doc: Any) -> dict[str, dict[str, Any]]:
    """Index scene resources by id → {kind, pose, dimensions}."""
    out: dict[str, dict[str, Any]] = {}
    objs: list[Any]
    if isinstance(scene_doc, list):
        objs = scene_doc
    elif isinstance(scene_doc, dict):
        objs = scene_doc.get("objects") or scene_doc.get("resources") or []
    else:
        return out
    if not isinstance(objs, list):
        return out
    for obj in objs:
        if not isinstance(obj, dict):
            continue
        oid = obj.get("id") or obj.get("name")
        if not isinstance(oid, str) or not oid:
            continue
        out[oid] = {
            "kind": obj.get("kind"),
            "pose": obj.get("pose"),
            "dimensions": obj.get("dimensions"),
        }
    return out


def _field_diff(before: Any, after: Any) -> dict[str, dict[str, Any]] | None:
    """Per-key diff of two dicts; returns ``None`` when equal.

    Recurses one level when both sides are dicts so a `position`
    change inside a `pose` is reported as a sub-key change rather
    than emitting the whole pose blob.
    """
    if before == after:
        return None
    if not isinstance(before, dict) or not isinstance(after, dict):
        return {"__value__": {"before": before, "after": after}}
    keys = set(before) | set(after)
    rows: dict[str, dict[str, Any]] = {}
    for k in sorted(keys):
        b = before.get(k)
        a = after.get(k)
        if b == a:
            continue
        if isinstance(b, dict) and isinstance(a, dict):
            nested = _field_diff(b, a)
            if nested is not None:
                rows[k] = {"nested": nested}
        else:
            rows[k] = {"before": b, "after": a}
    return rows or None


def _diff_indices(
    base: dict[str, dict[str, Any]],
    cur: dict[str, dict[str, Any]],
    *,
    domain: str,
) -> dict[str, Any]:
    base_keys = set(base)
    cur_keys = set(cur)
    added_entries: list[dict[str, Any]] = []
    removed_keys = sorted(base_keys - cur_keys)
    moved: list[dict[str, Any]] = []
    kind_changed: list[dict[str, Any]] = []
    params_changed: list[dict[str, Any]] = []
    pose_changed: list[dict[str, Any]] = []
    dim_changed: list[dict[str, Any]] = []

    for k in sorted(cur_keys - base_keys):
        c = cur[k]
        if domain == "tree":
            added_entries.append({
                "name": k,
                "id": c.get("id"),
                "parent": c.get("parent"),
                "position": c.get("position"),
            })
        else:
            added_entries.append({
                "id": k,
                "kind": c.get("kind"),
                "pose": c.get("pose"),
                "dimensions": c.get("dimensions"),
            })

    for k in sorted(base_keys & cur_keys):
        b, c = base[k], cur[k]
        if domain == "tree":
            if b.get("parent") != c.get("parent") or b.get("position") != c.get("position"):
                moved.append({
                    "name": k,
                    "from": {"parent": b.get("parent"), "position": b.get("position")},
                    "to": {"parent": c.get("parent"), "position": c.get("position")},
                })
            if b.get("id") != c.get("id"):
                kind_changed.append({
                    "name": k,
                    "before": b.get("id"),
                    "after": c.get("id"),
                })
            params_diff = _field_diff(b.get("params") or {}, c.get("params") or {})
            if params_diff is not None:
                params_changed.append({"name": k, "diff": params_diff})
        else:
            if b.get("kind") != c.get("kind"):
                kind_changed.append({
                    "id": k,
                    "before": b.get("kind"),
                    "after": c.get("kind"),
                })
            pose_diff = _field_diff(b.get("pose") or {}, c.get("pose") or {})
            if pose_diff is not None:
                pose_changed.append({"id": k, "diff": pose_diff})
            dim_diff = _field_diff(b.get("dimensions") or {}, c.get("dimensions") or {})
            if dim_diff is not None:
                dim_changed.append({"id": k, "diff": dim_diff})

    if domain == "tree":
        return {
            "added": added_entries,
            "removed": removed_keys,
            "moved": moved,
            "kind_changed": kind_changed,
            "params_changed": params_changed,
        }
    return {
        "added": added_entries,
        "removed": removed_keys,
        "kind_changed": kind_changed,
        "pose_changed": pose_changed,
        "dimensions_changed": dim_changed,
    }


def _is_empty_diff(diff: dict[str, Any]) -> bool:
    return not any(diff.get(k) for k in (
        "added", "removed", "moved", "kind_changed",
        "params_changed", "pose_changed", "dimensions_changed",
    ))


def compute_document_delta(
    *,
    before_tree: dict[str, Any] | None,
    before_scene: list[Any] | dict[str, Any] | None,
    after_tree: dict[str, Any] | None,
    after_scene: list[Any] | dict[str, Any] | None,
) -> dict[str, Any]:
    """Compare explicit documents without retaining them or inferring effects."""
    before_tree_index = _index_tree(before_tree or {})
    before_scene_index = _index_scene(before_scene or [])
    after_tree_index = _index_tree(after_tree or {})
    after_scene_index = _index_scene(after_scene or [])
    tree_delta = _diff_indices(
        before_tree_index,
        after_tree_index,
        domain="tree",
    )
    scene_delta = _diff_indices(
        before_scene_index,
        after_scene_index,
        domain="scene",
    )
    root_name: str | None = None
    root_kind: str | None = None
    if isinstance(after_tree, dict):
        raw_name = after_tree.get("name")
        raw_kind = after_tree.get("id")
        root_name = raw_name if isinstance(raw_name, str) else None
        root_kind = raw_kind if isinstance(raw_kind, str) else None
    return {
        "root_name": root_name,
        "root_kind": root_kind,
        "tree": tree_delta,
        "scene": scene_delta,
        "unchanged": _is_empty_diff(tree_delta) and _is_empty_diff(scene_delta),
    }


__all__ = ["compute_document_delta"]
