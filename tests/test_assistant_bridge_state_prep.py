"""Tests for the bridge's pure-helper projections.

Covers:
  - `_build_program_summary` two-tier projection (always-complete
    nodes_index + first-N detail)
  - `_build_scene_summary` two-tier projection (always-complete
    object_ids + first-N detail)
  - `_project_scene_object` non-null-only shape projection

These are unit tests against the pure helpers; they do NOT require
fastapi or the live agent loop, so they run in any environment that
can import the bridge module's helpers (the module's top-level
imports do require fastapi, so we skip if missing for parity with
the existing test file).
"""
from __future__ import annotations

from typing import Any

import pytest

pytest.importorskip("fastapi")

from manyforge_assistant_bridge import bridge


# ---- _project_scene_object ---------------------------------------------

def test_project_scene_object_drops_null_shape_siblings() -> None:
    """Box-shaped object → only `box_dimensions_m` retained; the
    null `sphere_radius_m`, `cylinder_*`, etc. siblings are dropped."""
    obj: dict[str, Any] = {
        "object_id": "graspable",
        "shape": {
            "type": "box",
            "box_dimensions_m": [0.1, 0.01, 0.01],
            "sphere_radius_m": None,
            "cylinder_radius_m": None,
            "cylinder_height_m": None,
            "mesh_resource_uri": None,
            "mesh_scale": None,
        },
        "pose_in_universe": {
            "position_m": [0.3, -0.3, 0.15],
            "orientation_xyzw": [0.0, 0.0, -0.382, 0.923],
        },
        "attached": False,
        "attached_to": None,
        "source": "authored",
        "persistable": True,
    }
    out = bridge._project_scene_object(obj)
    assert out["object_id"] == "graspable"
    assert out["shape"] == {
        "type": "box",
        "box_dimensions_m": [0.1, 0.01, 0.01],
    }
    assert out["pose_in_universe"]["position_m"] == [0.3, -0.3, 0.15]
    assert out["attached"] is False
    # `attached_to` is None → omitted; `source` / `persistable` are
    # not in the model's compose-args path, so they're stripped.
    assert "attached_to" not in out
    assert "source" not in out
    assert "persistable" not in out


# ---- _build_scene_summary ----------------------------------------------

def _make_object(idx: int, *, shape_type: str = "box") -> dict[str, Any]:
    return {
        "object_id": f"obj_{idx}",
        "shape": {
            "type": shape_type,
            "box_dimensions_m": [0.1, 0.1, 0.1],
            "sphere_radius_m": None,
            "cylinder_radius_m": None,
            "cylinder_height_m": None,
            "mesh_resource_uri": None,
            "mesh_scale": None,
        },
        "pose_in_universe": {
            "position_m": [float(idx), 0.0, 0.0],
            "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
        },
        "attached": False,
        "attached_to": None,
    }


def test_scene_summary_small_scene_full_detail() -> None:
    """Scene under the detail cap → every object kept full + index
    matches; no `__objects_detail_omitted_count__`."""
    snap = {
        "robot": {
            "robot_id": "ur10e",
            "tcp_frame": "grasp_frame",
        },
        "objects": [_make_object(i) for i in range(3)],
    }
    out = bridge._build_scene_summary(snap)
    assert out["object_ids"] == ["obj_0", "obj_1", "obj_2"]
    assert len(out["objects"]) == 3
    assert "__objects_detail_omitted_count__" not in out
    # Robot block carried over.
    assert out["robot"]["robot_id"] == "ur10e"


def test_scene_summary_large_scene_keeps_first_n_detail_only() -> None:
    """Scene with > _DETAIL_RESOURCE_LIMIT objects: first N kept as
    detail, while the complete `object_ids` index always lists every id."""
    n_total = bridge._DETAIL_RESOURCE_LIMIT + 5
    beyond_cap_idx = n_total - 1
    snap = {
        "robot": {"robot_id": "ur10e"},
        "objects": [_make_object(i) for i in range(n_total)],
    }
    out = bridge._build_scene_summary(snap)
    # Index is complete.
    assert len(out["object_ids"]) == n_total
    assert out["object_ids"][beyond_cap_idx] == f"obj_{beyond_cap_idx}"
    # Detail count = first N only. Prompt-derived promotion is deliberately gone.
    detail_ids = [o["object_id"] for o in out["objects"]]
    assert f"obj_{beyond_cap_idx}" not in detail_ids
    assert len(detail_ids) == bridge._DETAIL_RESOURCE_LIMIT
    # And the omitted-count counts what didn't get detail.
    assert out["__objects_detail_omitted_count__"] == n_total - bridge._DETAIL_RESOURCE_LIMIT


def test_scene_summary_handles_missing_objects_block() -> None:
    """If the snapshot has no `objects` key at all, the projection
    still returns a usable dict (robot only)."""
    out = bridge._build_scene_summary({"robot": {"robot_id": "ur10e"}})
    assert out["robot"]["robot_id"] == "ur10e"
    assert "objects" not in out
    assert "object_ids" not in out


# ---- _build_program_summary --------------------------------------------

def _make_node(name: str, kind: str, children: list | None = None) -> dict[str, Any]:
    n: dict[str, Any] = {
        "id": kind,
        "name": name,
        "params": {"foo": "bar"} if name != "leaf" else {},
    }
    if children:
        n["children"] = children
    return n


def test_program_summary_carries_full_tree_when_under_cap() -> None:
    tree = _make_node("pick_and_place", "sequence", children=[
        _make_node("approach", "MoveManipulator"),
        _make_node("pick", "MoveManipulator"),
        _make_node("retreat", "MoveManipulator"),
    ])
    snap = {
        "programTreeHash": "abc123",
        "program": {
            "name": "demo",
            "description": "pick and place demo",
            "tree": tree,
            "parameters": [{"name": "speed", "value": 0.5}],
            "blackboard": {"in_pick": False},
        },
    }
    out = bridge._build_program_summary(snap)
    assert out["programTreeHash"] == "abc123"
    assert out["program"]["name"] == "demo"
    assert out["program"]["description"] == "pick and place demo"
    # Index covers every node by name.
    assert set(out["program"]["nodes_index"]) == {
        "pick_and_place", "approach", "pick", "retreat",
    }
    # Tree fully detailed (under cap).
    assert "__nodes_detail_omitted_count__" not in out["program"]
    root = out["program"]["tree"]
    assert root["name"] == "pick_and_place"
    child_names = [c["name"] for c in root["children"]]
    assert child_names == ["approach", "pick", "retreat"]
    # params_keys, not values.
    assert root["params_keys"] == ["foo"]
    # Parameters & blackboard projection.
    assert out["program"]["parameters"] == ["speed"]
    assert out["program"]["blackboard_keys"] == ["in_pick"]


def test_program_summary_keeps_complete_index_without_prompt_promotion() -> None:
    """Build a wide tree where most nodes are on irrelevant branches.
    The complete index keeps every name, but detail past the first-N
    cap is omitted regardless of prompt wording."""
    # Layout: root with many sibling branches. One branch contains the
    # referenced leaf; the rest are filler that should get omitted.
    target_leaf = _make_node("target_leaf", "leaf")
    target_branch = _make_node("target_branch", "sequence",
                                children=[target_leaf])
    filler_count = bridge._DETAIL_NODE_LIMIT * 2  # ensures cap is hit
    fillers = [
        _make_node(f"filler_{i}", "MoveManipulator")
        for i in range(filler_count)
    ]
    root = _make_node("root", "sequence", children=[target_branch, *fillers])
    snap = {
        "programTreeHash": "h",
        "program": {"name": "p", "description": "d", "tree": root},
    }
    out = bridge._build_program_summary(snap)
    # Index is always complete (target + branch + root + every filler).
    assert "target_leaf" in out["program"]["nodes_index"]
    assert "filler_0" in out["program"]["nodes_index"]
    assert f"filler_{filler_count - 1}" in out["program"]["nodes_index"]
    # Walk the projected tree.
    found_nodes: list[dict[str, Any]] = []

    def visit(node: dict[str, Any]) -> None:
        if node.get("name") == "target_leaf":
            found_nodes.append(node)
        for c in node.get("children") or []:
            visit(c)

    visit(out["program"]["tree"])
    # This specific shape appears before the detail cap, so target_leaf remains detailed.
    assert len(found_nodes) == 1
    assert found_nodes[0].get("__detail_omitted__") is not True
    assert found_nodes[0]["id"] == "leaf"
    # Filler nodes past the cap rendered as stubs (still carry name).
    omitted_count = out["program"].get("__nodes_detail_omitted_count__", 0)
    assert omitted_count > 0
    stubs = []

    def find_stubs(node: dict[str, Any]) -> None:
        if node.get("__detail_omitted__"):
            stubs.append(node)
        for c in node.get("children") or []:
            find_stubs(c)

    find_stubs(out["program"]["tree"])
    assert any(s.get("name", "").startswith("filler_") for s in stubs), \
        "filler nodes past cap should be detail-omitted stubs (with name preserved)"


def test_program_summary_omitted_stub_carries_name() -> None:
    """A node beyond the detail cap should still have its name in the
    stub so structural traversal still works."""
    n_total = bridge._DETAIL_NODE_LIMIT + 3
    leaf = _make_node(f"node_{n_total - 1}", "leaf")
    cur = leaf
    for i in reversed(range(n_total - 1)):
        cur = _make_node(f"node_{i}", "MoveManipulator", children=[cur])
    snap = {"program": {"tree": cur}}
    out = bridge._build_program_summary(snap)
    # Find any stub.
    stubs = []

    def visit(node: dict[str, Any]) -> None:
        if node.get("__detail_omitted__"):
            stubs.append(node)
        for c in node.get("children") or []:
            visit(c)

    visit(out["program"]["tree"])
    assert stubs, "expected at least one detail-omitted stub past the cap"
    for s in stubs:
        assert "name" in s, "stub must carry the node's name"


def test_program_summary_handles_missing_tree() -> None:
    out = bridge._build_program_summary({"program": {"name": "p"}})
    assert out["program"]["name"] == "p"
    # No tree key, no nodes_index, no error
    assert "tree" not in out["program"]
    assert "nodes_index" not in out["program"]


def test_program_summary_handles_invalid_snapshot() -> None:
    out = bridge._build_program_summary("not a dict")  # type: ignore[arg-type]
    assert "__error__" in out
    out2 = bridge._build_scene_summary([])  # type: ignore[arg-type]
    assert "__error__" in out2
