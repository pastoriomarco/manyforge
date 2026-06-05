"""Unit tests for the bridge-tool argument normalizer.

The normalizer rewrites snake_case + flat-compound model output into
the camelCase + nested-object form the validator requires (see
``_normalize_bridge_tool_arguments`` in ``routes_assistant``). It is
the load-bearing surface for cosmos-reason2-8b's well-known habit of
emitting Python-style argument names.

These tests pin the contract per the canonical schemas declared in
``manyforge_composer.backend.assistant_tool_schemas``:

* ``tree_draft_insert_node`` — ``nodeName`` / ``parentName`` / ``index``
  / ``afterName`` / ``beforeName`` are ALL top-level. ``node`` does NOT
  carry ``name``. There is NO ``position`` wrapper.
* ``scene_draft_add_object`` — ``objectId`` (not ``name``),
  ``shape.box_dimensions_m`` (not ``box_dims``).

External-review findings 3, 4, 5 (2026-06-03) trace to bugs the
prior implementation had against these schemas; each failure mode
gets a regression test below.

We import ``_normalize_bridge_tool_arguments`` directly rather than
spinning a FastAPI client because the normalizer is a pure function
and the schema-shape contract is what we're guarding.
"""
from __future__ import annotations

import pytest

from manyforge_composer.backend.routes_assistant import (
    _diff_against_schema,
    _normalize_bridge_tool_arguments,
)


# ---------------------------------------------------------------------------
# tree_draft_insert_node
# ---------------------------------------------------------------------------


def test_insert_node_renames_top_level_snake_keys() -> None:
    """All snake_case top-level keys move to their camelCase canonicals.

    Covers reviewer finding 3 — previously ``after_name`` was mapped
    under a nonexistent ``position`` wrapper; ``node_name`` was
    incorrectly nested into ``node``.
    """
    raw = {
        "parent_name": "pick_and_place",
        "node_name": "wait_for_signal",
        "after_name": "approach_pose",
        # ``index`` is the same in both forms — passes through.
        "index": 0,
    }
    out = _normalize_bridge_tool_arguments("tree_draft_insert_node", raw)
    assert out == {
        "parentName": "pick_and_place",
        "nodeName": "wait_for_signal",
        "afterName": "approach_pose",
        "index": 0,
    }


def test_insert_node_nests_node_payload_without_name() -> None:
    """``node_id`` / ``node_params`` / ``node_children`` collapse into
    ``node: {id, params, children}``. Crucially ``node`` MUST NOT
    carry a ``name`` key — the instance name is the top-level
    ``nodeName`` argument (reviewer finding 3)."""
    raw = {
        "parent_name": "root_sequence",
        "node_name": "approach",
        "node_id": "move_to_pose",
        "node_params": {"velocity_scale": 0.3},
    }
    out = _normalize_bridge_tool_arguments("tree_draft_insert_node", raw)
    assert out == {
        "parentName": "root_sequence",
        "nodeName": "approach",
        "node": {"id": "move_to_pose", "params": {"velocity_scale": 0.3}},
    }
    assert "name" not in out["node"], (
        "node payload must not carry 'name'; instance name is the "
        "top-level nodeName argument per the schema"
    )


def test_insert_node_does_not_create_position_wrapper() -> None:
    """``afterName`` / ``beforeName`` / ``index`` are top-level — there
    is NO ``position`` nest. The prior implementation buggily wrapped
    them; this regression-pins the absence."""
    raw = {
        "parent_name": "p",
        "node_name": "n",
        "before_name": "sibling",
    }
    out = _normalize_bridge_tool_arguments("tree_draft_insert_node", raw)
    assert "position" not in out
    assert out["beforeName"] == "sibling"


def test_insert_node_partial_canonical_partial_flat_merges() -> None:
    """Mixed canonical + flat in one call: the canonical wins on
    conflict, but flat keys missing from canonical are still merged in.
    """
    raw = {
        "parentName": "root",
        "node_name": "leaf",
        # Mixed: canonical node has id, flat carries params.
        "node": {"id": "move_to_pose"},
        "node_params": {"velocity_scale": 0.2},
    }
    out = _normalize_bridge_tool_arguments("tree_draft_insert_node", raw)
    assert out["parentName"] == "root"
    assert out["nodeName"] == "leaf"
    assert out["node"]["id"] == "move_to_pose"  # canonical preserved
    assert out["node"]["params"] == {"velocity_scale": 0.2}  # flat merged


# ---------------------------------------------------------------------------
# scene_draft_add_object
# ---------------------------------------------------------------------------


def test_scene_add_object_renames_object_id() -> None:
    """``object_id`` → ``objectId`` (reviewer finding 4). The prior
    implementation had no entry for ``scene_draft_add_object`` so this
    rename silently passed through and broke at the validator."""
    raw = {
        "object_id": "fixture_1",
        "shape": {"type": "box", "box_dimensions_m": [0.4, 0.02, 0.2]},
        "pose": {"position": [0.0, 0.0, 0.0], "orientation_quat": [0, 0, 0, 1]},
    }
    out = _normalize_bridge_tool_arguments("scene_draft_add_object", raw)
    assert out["objectId"] == "fixture_1"
    assert "object_id" not in out


def test_scene_add_object_renames_shape_box_dims() -> None:
    """``shape.box_dims`` → ``shape.box_dimensions_m`` (reviewer finding
    4). Models commonly shorten ``box_dimensions_m`` to ``box_dims``."""
    raw = {
        "objectId": "fixture_1",
        "shape": {"type": "box", "box_dims": [0.4, 0.02, 0.2]},
        "pose": {"position": [0.0, 0.0, 0.0], "orientation_quat": [0, 0, 0, 1]},
    }
    out = _normalize_bridge_tool_arguments("scene_draft_add_object", raw)
    assert out["shape"]["box_dimensions_m"] == [0.4, 0.02, 0.2]
    assert "box_dims" not in out["shape"]


# ---------------------------------------------------------------------------
# Deep-copy / no-mutation guarantee (reviewer finding 5)
# ---------------------------------------------------------------------------


def test_normalizer_does_not_mutate_input() -> None:
    """The normalizer must not mutate the caller's input dict OR any
    nested object inside it. The prior shallow-copy implementation
    would mutate the inner ``wrapper`` dict via ``setdefault``."""
    original_wrapper = {"id": "repeat", "name": "outer_repeat"}
    raw = {
        "target_name": "pick_and_place",
        "wrapper": original_wrapper,
        # Flat params would merge into wrapper via Pass 1's setdefault.
        "wrapper_params": {"max_iterations": 3},
    }
    raw_snapshot = {
        "target_name": "pick_and_place",
        "wrapper": {"id": "repeat", "name": "outer_repeat"},
        "wrapper_params": {"max_iterations": 3},
    }
    _normalize_bridge_tool_arguments("tree_draft_wrap_node", raw)
    # Caller's dict is unchanged, including the inner wrapper.
    assert raw == raw_snapshot
    # And the original wrapper object itself was not mutated.
    assert original_wrapper == {"id": "repeat", "name": "outer_repeat"}


def test_normalizer_unknown_tool_passes_through_unchanged_and_deep_copies() -> None:
    """A tool with no rename/nest map still gets a deep copy. Callers
    can rely on the no-mutation guarantee unconditionally."""
    raw = {"foo": "bar", "nested": {"baz": 1}}
    out = _normalize_bridge_tool_arguments("tree_draft_unknown_tool", raw)
    assert out == raw
    # Mutating the output's nested dict must not touch the input.
    out["nested"]["baz"] = 999
    assert raw["nested"]["baz"] == 1


def test_normalizer_non_dict_input_passes_through() -> None:
    """Non-dict input (None, str, list) is returned unchanged. This
    is a defensive path; callers should never hit it."""
    assert _normalize_bridge_tool_arguments("tree_draft_wrap_node", None) is None  # type: ignore[arg-type]
    assert _normalize_bridge_tool_arguments("tree_draft_wrap_node", "x") == "x"  # type: ignore[arg-type]


# ---------------------------------------------------------------------------
# Error envelope diff quality (reviewer finding 3 follow-up — verifies
# the diff hint correctly maps insert_node's TOP-LEVEL afterName, NOT
# position.afterName)
# ---------------------------------------------------------------------------

_INSERT_NODE_SCHEMA = {
    "type": "object",
    "additionalProperties": False,
    "required": ["nodeName", "node", "parentName"],
    "properties": {
        "nodeName": {"type": "string"},
        "node": {"type": "object"},
        "parentName": {"type": "string"},
        "index": {"type": "integer"},
        "afterName": {"type": "string"},
        "beforeName": {"type": "string"},
        "forceDiscardOverrides": {"type": "boolean"},
    },
}


def test_diff_hints_at_top_level_camel_not_position_nest() -> None:
    """The hint for ``after_name`` must point at top-level ``afterName``,
    NOT ``position.afterName`` (which doesn't exist on the schema)."""
    provided = {"after_name": "sibling"}
    diff = _diff_against_schema(provided=provided, schema=_INSERT_NODE_SCHEMA)
    # Unexpected keys list captures the wrong key + its suggestion.
    unexpected = {u["providedKey"]: u["suggestion"] for u in diff["unexpectedKeys"]}
    assert unexpected.get("after_name") == "afterName"
    # The hint string must mention the top-level rename (no "position.").
    assert "position" not in diff["hint"]
    assert "afterName" in diff["hint"]


# ---------------------------------------------------------------------------
# Schemas the bridge-side drift test discovered (deeper than the
# original reviewer findings) — change_node_kind uses `newId`, not
# `newKind`; replace_subtree uses `nodeName` + `subtree`, not
# `targetName` + `replacement`.
# ---------------------------------------------------------------------------


def test_change_node_kind_normalizes_new_kind_to_new_id() -> None:
    """``new_kind`` (and the slightly-wrong camelCase ``new_id``) both
    normalize to ``newId`` — the actual schema field."""
    raw = {"node_name": "approach", "new_kind": "wait_for_signal_bool"}
    out = _normalize_bridge_tool_arguments("tree_draft_change_node_kind", raw)
    assert out == {"nodeName": "approach", "newId": "wait_for_signal_bool"}
    # Also accept the snake-case variant of newId since some training
    # corpora prefer it.
    raw2 = {"node_name": "approach", "new_id": "wait_for_signal_bool"}
    out2 = _normalize_bridge_tool_arguments("tree_draft_change_node_kind", raw2)
    assert out2 == {"nodeName": "approach", "newId": "wait_for_signal_bool"}


def test_replace_subtree_normalizes_to_node_name_and_subtree() -> None:
    """The schema uses ``nodeName`` + ``subtree`` (not ``targetName``
    + ``replacement``). Model output emitting either snake_case
    variant should normalize correctly."""
    raw = {
        "target_name": "approach",
        "subtree_id": "sequence",
        "subtree_name": "new_approach",
        "subtree_params": {},
    }
    out = _normalize_bridge_tool_arguments("tree_draft_replace_subtree", raw)
    assert out == {
        "nodeName": "approach",
        "subtree": {"id": "sequence", "name": "new_approach", "params": {}},
    }


def test_replace_subtree_accepts_legacy_replacement_keys() -> None:
    """Models trained against the older ``replacement: {...}`` shape
    are forgiven — their flat ``replacement_*`` keys collapse into
    the canonical ``subtree`` nest, not a separate ``replacement``."""
    raw = {
        "node_name": "approach",
        "replacement_id": "sequence",
        "replacement_name": "new_seq",
    }
    out = _normalize_bridge_tool_arguments("tree_draft_replace_subtree", raw)
    assert out == {
        "nodeName": "approach",
        "subtree": {"id": "sequence", "name": "new_seq"},
    }
    assert "replacement" not in out
    assert "targetName" not in out


def test_scene_add_object_normalizes_pose_orientation_quat() -> None:
    """``pose.orientation_quat`` is the common ROS-trained
    confusion; canonical is ``pose.orientation_xyzw``."""
    raw = {
        "objectId": "x",
        "shape": {"type": "box", "box_dimensions_m": [1, 1, 1]},
        "pose": {
            "frame_id": "world",
            "position_m": [0, 0, 0],
            "orientation_quat": [0, 0, 0, 1],
        },
    }
    out = _normalize_bridge_tool_arguments("scene_draft_add_object", raw)
    assert "orientation_quat" not in out["pose"]
    assert out["pose"]["orientation_xyzw"] == [0, 0, 0, 1]


def test_diff_hints_for_scene_add_object_object_id() -> None:
    """Diff suggests ``objectId`` when caller passed ``object_id``."""
    scene_add_schema = {
        "type": "object",
        "additionalProperties": False,
        "required": ["objectId", "shape", "pose"],
        "properties": {
            "objectId": {"type": "string"},
            "shape": {"type": "object"},
            "pose": {"type": "object"},
        },
    }
    diff = _diff_against_schema(provided={"object_id": "x"}, schema=scene_add_schema)
    suggestions = {u["providedKey"]: u["suggestion"] for u in diff["unexpectedKeys"]}
    assert suggestions.get("object_id") == "objectId"
    assert "objectId" in diff["hint"]
