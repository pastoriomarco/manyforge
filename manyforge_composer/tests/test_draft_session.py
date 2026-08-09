"""Atomic, Draft-only Composer Program authoring semantics."""

from __future__ import annotations

from copy import deepcopy
from pathlib import Path

import pytest

from manyforge_behavior.canonical_artifact import compute_content_identity
from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.program import Program
from manyforge_composer.backend.draft_session import (
    ComposerDraftSession,
    DraftSessionError,
    SUPPORTED_DRAFT_MUTATIONS,
)


_REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
_PROGRAM_PATH = _REPOSITORY_ROOT / "examples" / "pick_and_place_minimal.program.yaml"


def _program(*, metadata: dict | None = None) -> Program:
    catalog = build_node_environment().catalog
    program = Program(_PROGRAM_PATH, catalog)
    if metadata is None:
        return program
    document = program.to_dict()
    document["scene"][0]["metadata"] = metadata
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v1",
    )
    return Program.from_dict(document, catalog)


def _box_resource(identity: str) -> dict:
    return {
        "scene_resource_id": identity,
        "shape": {"type": "box", "box_dimensions_m": [0.1, 0.2, 0.3]},
        "pose": {
            "frame_ref": {"kind": "cell_frame"},
            "position_m": [0.0, 0.0, 0.0],
            "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
        },
    }


def _mutate(session: ComposerDraftSession, tool_id: str, arguments: dict):
    return session.mutate(
        tool_id=tool_id,
        arguments=arguments,
        expected_revision=session.revision,
        catalog=build_node_environment().catalog,
    )


def test_session_is_an_unsaved_unapplied_in_memory_draft() -> None:
    session = ComposerDraftSession(_program())

    snapshot = session.snapshot()

    assert snapshot["schema_version"] == "manyforge.composer_draft.v1"
    assert snapshot["draft_id"].startswith("draft-")
    assert snapshot["draft_revision"] == 1
    assert snapshot["program_loaded"] is True
    assert snapshot["saved"] is False
    assert snapshot["applied"] is False


def test_successful_edit_reidentifies_and_atomically_swaps_the_program() -> None:
    original = _program()
    session = ComposerDraftSession(original)

    result = _mutate(
        session,
        "scene_draft_add_object",
        {"scene_resource": _box_resource("workpiece")},
    )

    assert result.snapshot["draft_revision"] == 2
    assert result.snapshot["program_revision"] != original.program_revision
    assert [row["scene_resource_id"] for row in result.snapshot["program"]["scene"]] == [
        "table",
        "workpiece",
    ]
    assert [row["scene_resource_id"] for row in original.to_dict()["scene"]] == [
        "table"
    ]


def test_invalid_candidate_and_stale_revision_leave_the_draft_unchanged() -> None:
    session = ComposerDraftSession(_program())
    before = deepcopy(session.snapshot())

    with pytest.raises(DraftSessionError, match="scene resource"):
        _mutate(
            session,
            "scene_draft_remove_objects",
            {"scene_resource_ids": ["table"]},
        )
    assert session.snapshot() == before

    with pytest.raises(DraftSessionError) as stale:
        session.mutate(
            tool_id="scene_draft_add_object",
            arguments={"scene_resource": _box_resource("workpiece")},
            expected_revision=0,
            catalog=build_node_environment().catalog,
        )
    assert stale.value.code == "composer_draft_revision_stale"
    assert stale.value.status_code == 409
    assert session.snapshot() == before


def test_relative_orientation_resolves_to_absolute_and_preserves_metadata() -> None:
    session = ComposerDraftSession(_program(metadata={"operator_note": "retain"}))
    resource = _box_resource("table")
    resource["shape"] = {"type": "box", "box_dimensions_m": [1.0, 0.6, 0.02]}
    resource["pose"] = {
        "frame_ref": {
            "kind": "component_frame",
            "component_role": "robot",
            "model_role": "arm_model",
            "frame_role": "base",
        },
        "position_m": [0.5, 0.0, 0.4],
        "rotate_local_rpy_deg": [0.0, 0.0, 90.0],
    }

    result = _mutate(
        session,
        "scene_draft_update_object",
        {"scene_resource": resource},
    )
    updated = result.snapshot["program"]["scene"][0]

    assert updated["metadata"] == {"operator_note": "retain"}
    assert set(updated["pose"]) == {"frame_ref", "position_m", "orientation"}
    assert updated["pose"]["orientation"]["xyzw"] == pytest.approx(
        [0.0, 0.0, 2**-0.5, 2**-0.5]
    )


@pytest.mark.parametrize(
    ("intent", "expected"),
    [
        ({"rotate_world_rpy_deg": [0.0, 0.0, 90.0]}, [0.0, 0.0, 2**-0.5, 2**-0.5]),
        ({"rpy_delta_deg": [0.0, 0.0, 90.0]}, [0.0, 0.0, 2**-0.5, 2**-0.5]),
        ({"orientation_from": {"source": "current_pose"}}, [0.0, 0.0, 0.0, 1.0]),
        (
            {
                "orientation_from": {
                    "source": "scene_resource",
                    "scene_resource_id": "table",
                }
            },
            [0.0, 0.0, 0.0, 1.0],
        ),
    ],
)
def test_every_source_local_relative_orientation_form_becomes_absolute(
    intent: dict,
    expected: list[float],
) -> None:
    session = ComposerDraftSession(_program())
    resource = _box_resource("table")
    resource["shape"] = {"type": "box", "box_dimensions_m": [1.0, 0.6, 0.02]}
    resource["pose"] = {
        "frame_ref": {
            "kind": "component_frame",
            "component_role": "robot",
            "model_role": "arm_model",
            "frame_role": "base",
        },
        "position_m": [0.5, 0.0, 0.4],
        **intent,
    }

    result = _mutate(
        session,
        "scene_draft_update_object",
        {"scene_resource": resource},
    )

    pose = result.snapshot["program"]["scene"][0]["pose"]
    assert set(pose) == {"frame_ref", "position_m", "orientation"}
    assert pose["orientation"]["xyzw"] == pytest.approx(expected)


def test_new_resource_requires_an_absolute_orientation_even_for_direct_calls() -> None:
    session = ComposerDraftSession(_program())
    before = session.snapshot()
    resource = _box_resource("workpiece")
    resource["pose"].pop("orientation")
    resource["pose"]["orientation_from"] = {
        "source": "scene_resource",
        "scene_resource_id": "table",
    }

    with pytest.raises(DraftSessionError) as invalid:
        _mutate(
            session,
            "scene_draft_add_object",
            {"scene_resource": resource},
        )

    assert invalid.value.code == "composer_draft_validation_failed"
    assert session.snapshot() == before


def test_binding_dependent_target_frame_intent_is_typed_unavailable_and_atomic() -> None:
    session = ComposerDraftSession(_program())
    before = session.snapshot()
    resource = _box_resource("table")
    resource["pose"].pop("orientation")
    resource["pose"]["orientation_from"] = {
        "source": "target_frame",
        "component_id": "lite6_arm",
        "target_id": "arm",
        "frame_role_id": "tcp",
    }

    with pytest.raises(DraftSessionError) as unavailable:
        _mutate(
            session,
            "scene_draft_update_object",
            {"scene_resource": resource},
        )

    assert unavailable.value.code == "draft_pose_reference_unavailable"
    assert session.snapshot() == before


def test_tree_edits_use_instance_ids_and_the_explicit_root_alias() -> None:
    session = ComposerDraftSession(_program())
    inserted = _mutate(
        session,
        "tree_draft_insert_node",
        {
            "parent_instance_id": "@root",
            "index": 2,
            "node": {
                "node_id": "timer",
                "instance_id": "settle",
                "parameters": {"duration_s": 0.1},
                "children": [],
            },
        },
    )
    assert inserted.details["parent_instance_id"] == "main_sequence"

    wrapped = _mutate(
        session,
        "tree_draft_wrap_node",
        {
            "target_instance_id": "@root",
            "wrapper": {
                "node_id": "fallback",
                "instance_id": "fallback_root",
                "parameters": {},
            },
        },
    )
    root = wrapped.snapshot["program"]["tree"]
    assert root["instance_id"] == "fallback_root"
    assert [child["instance_id"] for child in root["children"]] == [
        "main_sequence"
    ]


def test_tree_pose_patch_preserves_frame_and_position_and_resolves_orientation() -> None:
    session = ComposerDraftSession(_program())
    stored_pose = {
        "frame_ref": {"kind": "cell_frame"},
        "position_m": [0.3, -0.3, 0.15],
        "orientation": {"rpy_deg": [0.0, 0.0, 0.0]},
    }
    _mutate(
        session,
        "tree_draft_insert_node",
        {
            "parent_instance_id": "@root",
            "node": {
                "node_id": "scene.update_object_pose",
                "instance_id": "assistant_pose_update",
                "parameters": {
                    "scene_resource_id": "table",
                    "pose": stored_pose,
                },
                "children": [],
            },
        },
    )

    result = _mutate(
        session,
        "tree_draft_update_node_params",
        {
            "instance_id": "assistant_pose_update",
            "expected_node_id": "scene.update_object_pose",
            "parameters": {
                "pose": {"rotate_local_rpy_deg": [0.0, 0.0, 90.0]},
            },
        },
    )
    node = next(
        child
        for child in result.snapshot["program"]["tree"]["children"]
        if child["instance_id"] == "assistant_pose_update"
    )
    pose = node["parameters"]["pose"]
    assert pose["frame_ref"] == stored_pose["frame_ref"]
    assert pose["position_m"] == stored_pose["position_m"]
    assert pose["orientation"]["xyzw"] == pytest.approx(
        [0.0, 0.0, 2**-0.5, 2**-0.5]
    )

    before = session.snapshot()
    with pytest.raises(DraftSessionError) as unresolved:
        _mutate(
            session,
            "tree_draft_update_node_params",
            {
                "instance_id": "assistant_pose_update",
                "expected_node_id": "scene.update_object_pose",
                "parameters": {
                    "pose": {
                        "orientation_from": {
                            "source": "scene_resource",
                            "scene_resource_id": "missing",
                        }
                    }
                },
            },
        )
    assert unresolved.value.code == "draft_scene_resource_not_found"
    assert session.snapshot() == before


def test_duplicate_declaration_identity_is_rejected_without_last_write_wins() -> None:
    session = ComposerDraftSession(_program())
    before = session.snapshot()
    declaration = {
        "parameter_id": "speed",
        "type": "float64",
        "default": 0.5,
        "constraints": {},
        "runtime_mutability": "before_run",
    }

    with pytest.raises(DraftSessionError) as duplicate:
        _mutate(
            session,
            "program_draft_upsert_parameters",
            {"parameters": [declaration, {**declaration, "default": 0.6}]},
        )

    assert duplicate.value.code == "draft_declaration_duplicate"
    assert session.snapshot() == before


def test_every_generated_draft_mutation_has_a_valid_atomic_path() -> None:
    session = ComposerDraftSession(_program())
    invoked: set[str] = set()

    def invoke(tool_id: str, arguments: dict):
        invoked.add(tool_id)
        return _mutate(session, tool_id, arguments)

    invoke(
        "blackboard_draft_upsert_namespaces",
        {"namespaces": [{"namespace_id": "scratch", "owner": "program"}]},
    )
    invoke(
        "blackboard_draft_upsert_keys",
        {
            "keys": [
                {
                    "key": "scratch.flag",
                    "type": "bool",
                    "access": "read_write",
                    "persistence": "run",
                }
            ]
        },
    )
    invoke(
        "program_draft_upsert_parameters",
        {
            "parameters": [
                {
                    "parameter_id": "scratch_limit",
                    "type": "int64",
                    "default": 2,
                    "constraints": {"min_inclusive": 1},
                    "runtime_mutability": "before_run",
                }
            ]
        },
    )

    first = _box_resource("assistant_first")
    second = _box_resource("assistant_second")
    invoke("scene_draft_add_object", {"scene_resource": first})
    updated = _box_resource("assistant_first")
    updated["pose"]["position_m"] = [0.1, 0.0, 0.0]
    invoke("scene_draft_update_object", {"scene_resource": updated})
    second["pose"]["position_m"] = [0.2, 0.0, 0.0]
    invoke(
        "scene_draft_upsert_objects",
        {"scene_resources": [updated, second]},
    )
    invoke(
        "scene_draft_remove_objects",
        {"scene_resource_ids": ["assistant_first", "assistant_second"]},
    )

    invoke(
        "tree_draft_insert_node",
        {
            "parent_instance_id": "@root",
            "node": {
                "node_id": "timer",
                "instance_id": "assistant_timer",
                "parameters": {"duration_s": 0.1},
                "children": [],
            },
        },
    )
    invoke(
        "tree_draft_update_node_params",
        {
            "instance_id": "assistant_timer",
            "expected_node_id": "timer",
            "parameters": {"duration_s": 0.2},
        },
    )
    invoke(
        "tree_draft_move_node",
        {
            "instance_id": "assistant_timer",
            "new_parent_instance_id": "@root",
            "index": 0,
        },
    )
    invoke(
        "tree_draft_replace_subtree",
        {
            "instance_id": "assistant_timer",
            "subtree": {
                "node_id": "timer",
                "instance_id": "assistant_timer_replaced",
                "parameters": {"duration_s": 0.3},
                "children": [],
            },
        },
    )
    invoke(
        "tree_draft_change_node_kind",
        {
            "instance_id": "assistant_timer_replaced",
            "replacement": {
                "node_id": "logic.set_bool",
                "parameters": {"key": "scratch.flag", "value": True},
            },
            "parameters_mode": "replace",
        },
    )
    invoke(
        "tree_draft_delete_node",
        {"instance_id": "assistant_timer_replaced"},
    )
    invoke(
        "tree_draft_wrap_node",
        {
            "target_instance_id": "@root",
            "wrapper": {
                "node_id": "fallback",
                "instance_id": "assistant_fallback",
                "parameters": {},
            },
        },
    )

    invoke(
        "program_draft_remove_parameters",
        {"parameter_ids": ["scratch_limit"]},
    )
    invoke("blackboard_draft_remove_keys", {"keys": ["scratch.flag"]})
    invoke(
        "blackboard_draft_remove_namespaces",
        {"namespace_ids": ["scratch"]},
    )

    assert invoked == set(SUPPORTED_DRAFT_MUTATIONS)
    assert session.snapshot()["saved"] is False
    assert session.snapshot()["applied"] is False
