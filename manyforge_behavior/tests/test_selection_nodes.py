"""Tests for deterministic scene-selection context and validation nodes."""

from __future__ import annotations

import py_trees

from manyforge_behavior.kernel_client import MockKernelClient, SceneEditRequest
from manyforge_behavior.nodes.selection import (
    BuildSceneSelectionContextNode,
    ValidateSceneSelectionNode,
)


def _seed_scene(kernel: MockKernelClient) -> None:
    requests = [
        SceneEditRequest(
            operation="add",
            object_id="table",
            shape_type="box",
            box_dimensions_m=[0.8, 0.6, 0.04],
            pose_position_m=[0.45, 0.0, -0.121],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="base_link",
        ),
        SceneEditRequest(
            operation="add",
            object_id="sphere_part",
            shape_type="sphere",
            sphere_radius_m=0.01,
            pose_position_m=[0.40, -0.14, 0.009],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="base_link",
        ),
        SceneEditRequest(
            operation="add",
            object_id="cube_part",
            shape_type="box",
            box_dimensions_m=[0.02, 0.02, 0.02],
            pose_position_m=[0.40, 0.14, 0.009],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="base_link",
        ),
    ]
    for request in requests:
        result = kernel.scene_edit(request)
        assert result.success is True


def _tick_node(node: py_trees.behaviour.Behaviour) -> py_trees.common.Status:
    node.setup()
    node.initialise()
    return node.update()


def test_build_scene_selection_context_writes_prompt_ready_manifest():
    py_trees.blackboard.Blackboard.clear()
    kernel = MockKernelClient()
    _seed_scene(kernel)
    bb = py_trees.blackboard.Client(name="setup")
    bb.register_key(key="kernel", access=py_trees.common.Access.WRITE)
    bb.kernel = kernel

    node = BuildSceneSelectionContextNode(
        name="build_context",
        selection_request_text="Select the sphere object.",
        output_key_prefix="selection/context",
        object_source="backend_scene",
        candidate_object_ids=["sphere_part", "cube_part"],
        image_source="none",
        base_frame="base_link",
    )

    status = _tick_node(node)

    assert status == py_trees.common.Status.SUCCESS
    reader = py_trees.blackboard.Client(name="reader")
    reader.register_key(
        key="selection/context/fields/candidate_count",
        access=py_trees.common.Access.READ,
    )
    reader.register_key(
        key="selection/context/fields/candidate_object_ids",
        access=py_trees.common.Access.READ,
    )
    reader.register_key(
        key="selection/context/fields/scene_context_json",
        access=py_trees.common.Access.READ,
    )
    reader.register_key(
        key="selection/context/status",
        access=py_trees.common.Access.READ,
    )
    assert reader.get("selection/context/fields/candidate_count") == 2
    assert reader.get("selection/context/fields/candidate_object_ids") == [
        "cube_part",
        "sphere_part",
    ]
    assert '"selection_request_text": "Select the sphere object."' in reader.get(
        "selection/context/fields/scene_context_json"
    )
    assert reader.get("selection/context/status") == "built"


def test_build_scene_selection_context_fails_when_snapshot_requested_without_provider():
    py_trees.blackboard.Blackboard.clear()
    kernel = MockKernelClient()
    _seed_scene(kernel)
    bb = py_trees.blackboard.Client(name="setup")
    bb.register_key(key="kernel", access=py_trees.common.Access.WRITE)
    bb.kernel = kernel

    node = BuildSceneSelectionContextNode(
        name="build_context",
        selection_request_text="Select the sphere object.",
        output_key_prefix="selection/context",
        object_source="backend_scene",
        candidate_object_ids=["sphere_part", "cube_part"],
        image_source="hmi_snapshot",
        base_frame="base_link",
    )

    status = _tick_node(node)

    assert status == py_trees.common.Status.FAILURE
    reader = py_trees.blackboard.Client(name="reader")
    reader.register_key(
        key="selection/context/fields/outcome",
        access=py_trees.common.Access.READ,
    )
    assert reader.get("selection/context/fields/outcome") == "image_source_unavailable"


def test_validate_scene_selection_accepts_selected_candidate():
    py_trees.blackboard.Blackboard.clear()
    bb = py_trees.blackboard.Client(name="setup")
    bb.register_key(key="selection/context/fields", access=py_trees.common.Access.WRITE)
    bb.register_key(
        key="perception/object_selection/fields",
        access=py_trees.common.Access.WRITE,
    )
    bb.set(
        "selection/context/fields",
        {
            "selection_request_text": "Select the sphere object.",
            "candidates": [
                {"object_id": "sphere_part", "shape": {"type": "sphere"}},
                {"object_id": "cube_part", "shape": {"type": "box"}},
            ],
        },
    )
    bb.set(
        "perception/object_selection/fields",
        {
            "outcome": "selected",
            "selected_object_id": "sphere_part",
            "matched_attributes": ["shape=sphere"],
            "confidence": 0.92,
            "reason": "sphere requested",
        },
    )

    node = ValidateSceneSelectionNode(
        name="validate_selection",
        selection_context_prefix="selection/context",
        selection_response_prefix="perception/object_selection",
        output_key_prefix="selection/validated",
    )

    status = _tick_node(node)

    assert status == py_trees.common.Status.SUCCESS
    reader = py_trees.blackboard.Client(name="reader")
    reader.register_key(
        key="selection/validated/fields/selected_object_id",
        access=py_trees.common.Access.READ,
    )
    reader.register_key(
        key="selection/validated/fields/selected_candidate",
        access=py_trees.common.Access.READ,
    )
    assert reader.get("selection/validated/fields/selected_object_id") == "sphere_part"
    assert reader.get("selection/validated/fields/selected_candidate") == {
        "object_id": "sphere_part",
        "shape": {"type": "sphere"},
    }


def test_validate_scene_selection_rejects_non_candidate_object():
    py_trees.blackboard.Blackboard.clear()
    bb = py_trees.blackboard.Client(name="setup")
    bb.register_key(key="selection/context/fields", access=py_trees.common.Access.WRITE)
    bb.register_key(
        key="perception/object_selection/fields",
        access=py_trees.common.Access.WRITE,
    )
    bb.set(
        "selection/context/fields",
        {
            "selection_request_text": "Select the sphere object.",
            "candidates": [
                {"object_id": "sphere_part", "shape": {"type": "sphere"}},
            ],
        },
    )
    bb.set(
        "perception/object_selection/fields",
        {
            "outcome": "selected",
            "selected_object_id": "cube_part",
            "matched_attributes": ["shape=cube"],
            "confidence": 0.88,
            "reason": "cube selected",
        },
    )

    node = ValidateSceneSelectionNode(
        name="validate_selection",
        selection_context_prefix="selection/context",
        selection_response_prefix="perception/object_selection",
        output_key_prefix="selection/validated",
    )

    status = _tick_node(node)

    assert status == py_trees.common.Status.FAILURE
    reader = py_trees.blackboard.Client(name="reader")
    reader.register_key(
        key="selection/validated/fields/outcome",
        access=py_trees.common.Access.READ,
    )
    assert reader.get("selection/validated/fields/outcome") == "selected_object_not_in_context"
