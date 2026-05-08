"""Tests for the bridge's compound-prompt state-prep path.

Covers:
  - `_needs_state_prep` trigger phrases (positive + negative cases)
  - `_build_program_summary` two-tier projection (always-complete
    nodes_index + first-N + prompt-referenced detail)
  - `_build_scene_summary` two-tier projection (always-complete
    object_ids + first-N + prompt-referenced detail)
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


# ---- _needs_state_prep --------------------------------------------------

@pytest.mark.parametrize(
    "prompt,expected",
    [
        # Negative: simple action prompts with literals → no prep.
        ("add a repeat node as root", (False, False)),
        ("add a box of size 1.0, 0.02, 0.25 at position 0.0, -0.15, 0.125",
         (False, False)),
        ("remove the graspable object", (False, False)),

        # Positive: scene-only references.
        ("add a node that places graspable in the same position as scene start",
         (False, True)),
        ("create a sphere with the original size of graspable", (False, True)),
        ("place the box where it is now", (False, True)),

        # Positive: program-only references.
        ("insert a wait at the end of pick_and_place", (True, False)),
        ("add a node before the gripper open", (True, False)),

        # Positive: both (the canonical compound prompt).
        ("add an upsert node at the end of pick_and_place sequence "
         "that places 'graspable' object in the same position and "
         "orientation as the scene start, with the same original size",
         (True, True)),
    ],
)
def test_needs_state_prep(prompt: str, expected: tuple[bool, bool]) -> None:
    assert bridge._needs_state_prep(prompt.lower()) == expected


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


def test_scene_summary_large_scene_keeps_first_n_plus_referenced() -> None:
    """Scene with > _DETAIL_RESOURCE_LIMIT objects: first N kept, AND
    a prompt-referenced object beyond N is also kept. The complete
    `object_ids` index always lists every id."""
    n_total = bridge._DETAIL_RESOURCE_LIMIT + 5
    referenced_idx = n_total - 1  # last one — definitely past the cap
    snap = {
        "robot": {"robot_id": "ur10e"},
        "objects": [_make_object(i) for i in range(n_total)],
    }
    prompt = f"place obj_{referenced_idx} in the same position as scene start"
    out = bridge._build_scene_summary(snap, prompt_lower=prompt.lower())
    # Index is complete.
    assert len(out["object_ids"]) == n_total
    assert out["object_ids"][referenced_idx] == f"obj_{referenced_idx}"
    # Detail count = first N + the one extra referenced object.
    detail_ids = [o["object_id"] for o in out["objects"]]
    assert f"obj_{referenced_idx}" in detail_ids, \
        "prompt-referenced object beyond detail cap should still get a full record"
    assert len(detail_ids) == bridge._DETAIL_RESOURCE_LIMIT + 1
    # And the omitted-count counts what didn't get detail.
    assert out["__objects_detail_omitted_count__"] == n_total - (
        bridge._DETAIL_RESOURCE_LIMIT + 1
    )


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


def test_program_summary_keeps_prompt_referenced_node_past_detail_cap() -> None:
    """Build a wide tree where most nodes are on irrelevant branches.
    The user prompt references one specific node deep on a single
    branch — that node and its ancestors must end up with full detail
    (not stubs), AND nodes on the irrelevant branches that fall past
    the detail cap must end up as stubs (omitted_count > 0)."""
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
    prompt = "insert a wait before the target_leaf"
    out = bridge._build_program_summary(snap, prompt_lower=prompt.lower())
    # Index is always complete (target + branch + root + every filler).
    assert "target_leaf" in out["program"]["nodes_index"]
    assert "filler_0" in out["program"]["nodes_index"]
    assert f"filler_{filler_count - 1}" in out["program"]["nodes_index"]
    # Walk the projected tree.
    found_detail: list[dict[str, Any]] = []

    def visit(node: dict[str, Any]) -> None:
        if node.get("name") == "target_leaf":
            found_detail.append(node)
        for c in node.get("children") or []:
            visit(c)

    visit(out["program"]["tree"])
    # Referenced leaf reachable + carries full detail.
    assert len(found_detail) == 1
    assert found_detail[0].get("__detail_omitted__") is not True
    assert found_detail[0]["id"] == "leaf"
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
    """A node beyond the detail cap that isn't prompt-referenced
    should still have its name in the stub so structural traversal
    still works."""
    n_total = bridge._DETAIL_NODE_LIMIT + 3
    leaf = _make_node(f"node_{n_total - 1}", "leaf")
    cur = leaf
    for i in reversed(range(n_total - 1)):
        cur = _make_node(f"node_{i}", "MoveManipulator", children=[cur])
    snap = {"program": {"tree": cur}}
    out = bridge._build_program_summary(snap, prompt_lower="")
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
