"""Assistant tool-call delta computation.

Computes per-conversation tree+scene state diffs returned in every
``/api/assistant/bridge/tools/{toolId}`` response. The MCP wrapper
running in the sandbox can't fetch state on its own — the OpenShell
SSRF policy denies arbitrary GETs — so the only native way to
deliver fresh state confirmation to the model is to attach the delta
to the bridge tool response itself. The wrapper passes that response
through to the model verbatim.

State is sourced from ``state.program.to_dict()`` so we never read
anything the bridge endpoint doesn't already have. Two diffs are
produced per call:

  * ``sinceCallStart``  : changes vs the snapshot from the previous
    tool call. Tells the model what *this* call did.
  * ``sinceConversationStart`` : cumulative changes vs the first call
    in this conversation. Tells the model how the world has drifted
    from the program-as-loaded.

Plus ``rootName`` / ``rootKind`` for cheap goal-completion checks
(rule 9 in the bridge prompt).

Baselines and last-snapshots are kept per ``conversation_id`` in a
module-level dict; eviction happens on Composer restart and on
explicit ``forget_conversation`` (intended to be called when a
program reload makes the prior baseline meaningless).
"""
from __future__ import annotations

from typing import Any


_BASELINES: dict[str, dict[str, Any]] = {}
_LAST_SNAPSHOTS: dict[str, dict[str, Any]] = {}


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
            "kindChanged": kind_changed,
            "paramsChanged": params_changed,
        }
    return {
        "added": added_entries,
        "removed": removed_keys,
        "kindChanged": kind_changed,
        "poseChanged": pose_changed,
        "dimChanged": dim_changed,
    }


def _is_empty_diff(diff: dict[str, Any]) -> bool:
    return not any(diff.get(k) for k in (
        "added", "removed", "moved", "kindChanged",
        "paramsChanged", "poseChanged", "dimChanged",
    ))


def snapshot_from_program(program_dict: dict[str, Any] | None) -> dict[str, Any]:
    """Build a comparable snapshot {tree, scene} from program.to_dict()."""
    if not isinstance(program_dict, dict):
        return {"tree": {}, "scene": []}
    tree = program_dict.get("tree") if isinstance(program_dict.get("tree"), dict) else {}
    scene = (
        program_dict.get("sceneResources")
        or program_dict.get("scene_resources")
        or program_dict.get("scene")
        or []
    )
    return {"tree": tree, "scene": scene}


def compute_delta(
    *,
    conversation_id: str,
    current_program_dict: dict[str, Any] | None,
) -> dict[str, Any]:
    """Compute the delta block to attach to a tool-call response.

    On the first call for ``conversation_id`` the current snapshot
    becomes the conversation baseline; both diffs come back empty
    (``noop``). On subsequent calls the diffs report changes since
    the previous call (``sinceCallStart``) and since the first call
    (``sinceConversationStart``).
    """
    if not isinstance(conversation_id, str) or not conversation_id:
        conversation_id = "(default)"

    current = snapshot_from_program(current_program_dict)

    baseline = _BASELINES.get(conversation_id)
    if baseline is None:
        _BASELINES[conversation_id] = current
        _LAST_SNAPSHOTS[conversation_id] = current
        baseline = current
        last = current
    else:
        last = _LAST_SNAPSHOTS.get(conversation_id, baseline)

    base_tree_idx = _index_tree(baseline.get("tree"))
    base_scene_idx = _index_scene(baseline.get("scene"))
    last_tree_idx = _index_tree(last.get("tree"))
    last_scene_idx = _index_scene(last.get("scene"))
    cur_tree_idx = _index_tree(current.get("tree"))
    cur_scene_idx = _index_scene(current.get("scene"))

    cumulative_tree = _diff_indices(base_tree_idx, cur_tree_idx, domain="tree")
    cumulative_scene = _diff_indices(base_scene_idx, cur_scene_idx, domain="scene")
    call_tree = _diff_indices(last_tree_idx, cur_tree_idx, domain="tree")
    call_scene = _diff_indices(last_scene_idx, cur_scene_idx, domain="scene")

    root_name: str | None = None
    root_kind: str | None = None
    cur_tree = current.get("tree")
    if isinstance(cur_tree, dict):
        n = cur_tree.get("name")
        i = cur_tree.get("id")
        if isinstance(n, str): root_name = n
        if isinstance(i, str): root_kind = i

    _LAST_SNAPSHOTS[conversation_id] = current

    return {
        "rootName": root_name,
        "rootKind": root_kind,
        "sinceCallStart": {
            "tree": call_tree,
            "scene": call_scene,
            "noop": _is_empty_diff(call_tree) and _is_empty_diff(call_scene),
        },
        "sinceConversationStart": {
            "tree": cumulative_tree,
            "scene": cumulative_scene,
            "noop": _is_empty_diff(cumulative_tree) and _is_empty_diff(cumulative_scene),
        },
    }


def forget_conversation(conversation_id: str) -> None:
    """Drop cached baseline + last snapshot for a conversation.

    Called when a program is reloaded so the next tool call
    re-baselines against the freshly loaded program.
    """
    _BASELINES.pop(conversation_id, None)
    _LAST_SNAPSHOTS.pop(conversation_id, None)


def reset_all() -> None:
    """Clear every cached baseline. Used on program/load."""
    _BASELINES.clear()
    _LAST_SNAPSHOTS.clear()
