"""Focused N7-a5.3c Program model-request and selection contracts."""

from __future__ import annotations

from copy import deepcopy
from hashlib import sha256
from pathlib import Path
from threading import Event
import json
import time
from typing import Any, Mapping

import pytest

from manyforge_behavior.assistant_program_port import (
    HeadlessProgramSceneSource,
    ProgramAssistantPort,
    ProgramAssistantPortError,
    ProgramModelRequest,
    ProgramModelResponse,
)
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.nodes.selection import (
    SceneSelectionError,
    build_scene_selection_context,
    validate_scene_selection,
)
from manyforge_behavior.orchestration_ports import (
    NodeAttemptIdentity,
    OwnerNodeInputs,
    OwnerTaskSnapshot,
    OwnerTaskState,
    ProgramRunPortContext,
)
from manyforge_behavior.orchestration_values import ProgramValueStore
from manyforge_behavior.program import Program, ProgramTreeNode


_ROOT = Path(__file__).resolve().parents[2]
_DIGEST_A = "sha256:" + "a" * 64
_DIGEST_B = "sha256:" + "b" * 64
_DIGEST_C = "sha256:" + "c" * 64
_DIGEST_D = "sha256:" + "d" * 64
_DIGEST_E = "sha256:" + "e" * 64


def _catalog() -> NodeCatalog:
    return NodeCatalog(_ROOT / "manyforge_behavior/resources/node_catalog.yaml")


def _program(filename: str = "semantic_shape_pick.program.yaml") -> Program:
    return Program(_ROOT / "examples" / filename, _catalog())


def _walk(node: ProgramTreeNode) -> tuple[ProgramTreeNode, ...]:
    return (node, *(nested for child in node.children for nested in _walk(child)))


def _node(program: Program, instance_id: str) -> ProgramTreeNode:
    return next(node for node in _walk(program.tree) if node.instance_id == instance_id)


def _assistant_projection() -> dict[str, Any]:
    return {
        "api_version": "manyforge.retained_assistant.v1",
        "projection_digest": _DIGEST_D,
        "audit_policy": {
            "redact_argument_values": True,
            "store_full_prompts": False,
            "store_full_responses": False,
        },
        "model_endpoint_aliases": [
            {
                "model_endpoint_alias": "selection_model",
                "external_resource_alias": "selection_model",
                "model_id": "selection-model-v1",
                "request_contract": {
                    "default_timeout_s": 30.0,
                    "max_output_tokens": 384,
                    "supported_image_transports": [],
                    "supports_image_input": False,
                    "supports_json_object": True,
                    "supports_response_tools": True,
                    "tool_call_family": "openai_chat_tools",
                },
            }
        ],
        "provider_aliases": [],
        "modes": [],
        "prompt_templates": [
            {
                "content_digest": _DIGEST_E,
                "prompt_template_id": "select_scene_object_v1",
                "response_contract": {
                    "response_mode": "tool_call",
                    "tool_description": "Choose one candidate scene object.",
                    "tool_name": "select_scene_object",
                },
                "template_text": (
                    "Select for {{selection_request_text}} from "
                    "{{candidate_object_ids}} using {{scene_context_json}}"
                ),
                "variable_ids": [
                    "candidate_object_ids",
                    "scene_context_json",
                    "selection_request_text",
                ],
                "version": "1",
            }
        ],
        "instruments": {
            "api_version": "manyforge.assistant_instrument_registry.v1",
            "registry_digest": _DIGEST_A,
            "skills": [],
            "tools": [],
            "nodes": [],
        },
    }


def _context(
    program: Program,
    *,
    assistant_projection: Mapping[str, Any] | None = None,
) -> ProgramRunPortContext:
    return ProgramRunPortContext(
        program_run_id="program-run-1",
        scenario_id="scenario-1",
        scenario_catalog_revision=_DIGEST_A,
        program_id=program.program_id,
        program_revision=program.program_revision,
        program_binding_id=_DIGEST_B,
        retained_bundle_id=_DIGEST_C,
        graph_revision=_DIGEST_D,
        node_environment_digest=_DIGEST_E,
        program_binding={
            "program_binding_id": _DIGEST_B,
            "program_id": program.program_id,
            "program_revision": program.program_revision,
            "retained_bundle_id": _DIGEST_C,
            "graph_revision": _DIGEST_D,
        },
        program=program.to_dict(),
        assistant_projection=assistant_projection,
    )


def _scene_object(
    resource_id: str,
    *,
    position: tuple[float, float, float],
    shape: str = "box",
    carried: bool = False,
) -> dict[str, Any]:
    collision_shape: dict[str, Any]
    if shape == "sphere":
        collision_shape = {"type": "sphere", "radius_m": 0.01}
    else:
        collision_shape = {
            "type": "box",
            "dimensions_m": {"x": 0.02, "y": 0.02, "z": 0.02},
        }
    placement = (
        {
            "kind": "carried",
            "carried_object_attachment_id": f"attachment.{resource_id}",
            "carried_object_attachment_revision": _DIGEST_A,
        }
        if carried
        else {
            "kind": "cell_fixed",
            "cell_id": "cell-a",
            "pose_in_cell": {
                "position_m": {
                    "x": position[0],
                    "y": position[1],
                    "z": position[2],
                },
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            },
        }
    )
    return {
        "scene_resource_id": resource_id,
        "collision_shapes": [collision_shape],
        "placement": placement,
        "provider_evidence": None,
        "support_contacts": [],
    }


class _SceneTransport:
    def __init__(self, objects: list[dict[str, Any]] | None = None) -> None:
        self.objects = objects or []
        self.resolved_poses: dict[str, dict[str, Any]] = {}
        self.pose_queries: list[dict[str, Any]] = []
        self.mutate_pose_response = False
        self.revision = {
            "retained_bundle_id": _DIGEST_C,
            "graph_revision": _DIGEST_D,
            "scene_revision": {
                "scene_id": _DIGEST_A,
                "revision_id": 7,
                "content_hash": _DIGEST_B,
            },
        }

    def scene(self) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.scene_projection.v1",
            "projection_generation": 7,
            "logical_scene": {
                "available": True,
                "freshness": "fresh",
                "known_gaps": [],
                "payload": {
                    "api_version": "manyforge.logical_scene_source.v1",
                    "access": {
                        "active_projection": deepcopy(self.revision),
                        "phase": "active",
                        "projection_leases_open": True,
                        "scene_writes_open": True,
                    },
                    "snapshot": {
                        "cell_id": "cell-a",
                        "revision": deepcopy(self.revision["scene_revision"]),
                        "transaction_sequence": 1,
                        "topology_epoch": 1,
                        "policy_epoch": 1,
                        "configured_collision_policy": {},
                        "objects": deepcopy(self.objects),
                        "carried_object_attachments": [],
                    },
                },
            },
            "scene_runtime": {},
        }

    def resolve_scene_resources(
        self, query: Mapping[str, Any]
    ) -> dict[str, Any]:
        self.pose_queries.append(deepcopy(dict(query)))
        response = {
            "api_version": "manyforge.scene_resource_pose_response.v1",
            "poses": [
                {
                    "scene_resource_id": resource_id,
                    "pose_in_cell": deepcopy(self.resolved_poses[resource_id]),
                }
                for resource_id in query["scene_resource_ids"]
            ],
            "projection_revision": deepcopy(
                query["expected_projection_revision"]
            ),
        }
        if self.mutate_pose_response:
            response["projection_revision"]["graph_revision"] = _DIGEST_A
        return response


class _ModelTransport:
    def __init__(self, responses: list[ProgramModelResponse] | None = None) -> None:
        self.responses = responses or []
        self.validated: list[dict[str, Any]] = []
        self.requests: list[ProgramModelRequest] = []
        self.reject_validation = False

    def validate_endpoint(self, **endpoint: Any) -> None:
        if self.reject_validation:
            raise RuntimeError("site endpoint is absent")
        self.validated.append(deepcopy(endpoint))

    def complete(
        self, request: ProgramModelRequest, _cancel_event: Event
    ) -> ProgramModelResponse:
        self.requests.append(request)
        return self.responses.pop(0)


class _LateModelTransport(_ModelTransport):
    def __init__(self, response: ProgramModelResponse) -> None:
        super().__init__([response])
        self.started = Event()
        self.release = Event()

    def complete(
        self, request: ProgramModelRequest, _cancel_event: Event
    ) -> ProgramModelResponse:
        self.requests.append(request)
        self.started.set()
        assert self.release.wait(timeout=1.0)
        return self.responses.pop(0)


def _identity(attempt: str = "attempt-1") -> NodeAttemptIdentity:
    return NodeAttemptIdentity("program-run-1", "node-1", attempt)


def _wait_terminal(
    port: ProgramAssistantPort,
    task_id: str,
) -> OwnerTaskSnapshot:
    for _ in range(1_000):
        snapshot = port.observe(task_id)
        if snapshot.state is not OwnerTaskState.ACTIVE:
            return snapshot
        time.sleep(0.001)
    raise AssertionError("assistant task did not settle")


def _selection_inputs() -> OwnerNodeInputs:
    return OwnerNodeInputs(
        parameters={
            "selection_request_text": "Select the sphere object.",
            "output_key": "selection.context",
            "object_source": "backend_scene",
            "candidate_object_ids": ["sphere_part", "cube_part"],
            "image_source": "none",
        }
    )


def _query_inputs(
    selection_context: Mapping[str, Any],
    *,
    retry_on_parse_failure: bool = False,
    timeout_s: float = 20.0,
) -> OwnerNodeInputs:
    return OwnerNodeInputs(
        parameters={
            "model_endpoint_alias": "selection_model",
            "prompt_template_id": "select_scene_object_v1",
            "template_variables": [
                {
                    "variable_id": "candidate_object_ids",
                    "source": {
                        "kind": "blackboard_field",
                        "reference": "selection.context",
                        "field": "candidate_object_ids",
                    },
                },
                {
                    "variable_id": "scene_context_json",
                    "source": {
                        "kind": "blackboard_field",
                        "reference": "selection.context",
                        "field": "scene_context_json",
                    },
                },
                {
                    "variable_id": "selection_request_text",
                    "source": {
                        "kind": "blackboard_field",
                        "reference": "selection.context",
                        "field": "selection_request_text",
                    },
                },
            ],
            "scene_attachment": "none",
            "output_key": "selection.response",
            "timeout_s": timeout_s,
            "min_confidence": 0.7,
            "retry_on_parse_failure": retry_on_parse_failure,
        },
        embedded_values={
            "/template_variables/0/source": selection_context[
                "candidate_object_ids"
            ],
            "/template_variables/1/source": selection_context[
                "scene_context_json"
            ],
            "/template_variables/2/source": selection_context[
                "selection_request_text"
            ],
        },
    )


def _selected_response() -> ProgramModelResponse:
    return ProgramModelResponse(
        tool_name="select_scene_object",
        tool_arguments={
            "confidence": 0.92,
            "matched_attributes": ["shape=sphere"],
            "outcome": "selected",
            "reason": "sphere requested",
            "selected_object_id": "sphere_part",
        },
        model_version="selection-model-v1.2",
        usage={"input_tokens": 50, "output_tokens": 12},
    )


def test_pure_selection_restores_deterministic_context_and_closed_validation() -> None:
    context = build_scene_selection_context(
        selection_request_text="Select the sphere object.",
        scene_objects=[
            {
                **_scene_object(
                    "sphere_part", position=(0.4, -0.14, 0.009), shape="sphere"
                ),
                "pose_in_cell": {
                    "position_m": {"x": 0.4, "y": -0.14, "z": 0.009},
                    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                },
                "attached": False,
            },
            {
                **_scene_object("cube_part", position=(0.4, 0.14, 0.009)),
                "pose_in_cell": {
                    "position_m": {"x": 0.4, "y": 0.14, "z": 0.009},
                    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                },
                "attached": False,
            },
        ],
        candidate_object_ids=["sphere_part", "cube_part"],
        frame_id="cell-a",
    )

    assert context["candidate_object_ids"] == ["cube_part", "sphere_part"]
    manifest = json.loads(context["scene_context_json"])
    assert manifest["spatial_semantics"]["rankings"][
        "left_to_right_by_base_y_desc"
    ] == ["cube_part", "sphere_part"]
    assert validate_scene_selection(
        selection_context=context,
        selection_response={
            "confidence": 0.92,
            "matched_attributes": ["shape=sphere"],
            "outcome": "selected",
            "reason": "sphere requested",
            "selected_object_id": "sphere_part",
        },
    )["selected_object_id"] == "sphere_part"

    tampered = {**context, "candidate_object_ids": ["sphere_part"]}
    with pytest.raises(SceneSelectionError, match="manifest disagrees"):
        validate_scene_selection(
            selection_context=tampered,
            selection_response={
                "confidence": 0.92,
                "matched_attributes": [],
                "outcome": "selected",
                "reason": "",
                "selected_object_id": "sphere_part",
            },
        )


def test_no_match_and_ambiguous_remain_values_for_the_authored_policy() -> None:
    program = _program()
    store = ProgramValueStore.from_program(program)
    no_match = {
        "confidence": 0.2,
        "matched_attributes": [],
        "outcome": "no_match",
        "reason": "",
        "selected_object_id": "",
    }

    store.write("selection.response", no_match)
    store.write("selection.validated", no_match)
    assert store.read("selection.response") == no_match


@pytest.mark.parametrize(
    ("response", "reason_code"),
    [
        (
            {
                "confidence": 0.92,
                "matched_attributes": [],
                "outcome": "selected",
                "reason": "not in the candidate set",
                "selected_object_id": "table",
            },
            "selected_object_not_in_context",
        ),
        (
            {
                "confidence": 0.2,
                "matched_attributes": [],
                "outcome": "no_match",
                "reason": "",
                "selected_object_id": "",
            },
            "selection_no_match",
        ),
    ],
)
def test_selection_validation_preserves_authored_failure_policy(
    response: dict[str, Any],
    reason_code: str,
) -> None:
    context = build_scene_selection_context(
        selection_request_text="Select the sphere object.",
        scene_objects=[
            {
                **_scene_object(
                    "sphere_part",
                    position=(0.4, -0.14, 0.009),
                    shape="sphere",
                ),
                "pose_in_cell": {
                    "position_m": {"x": 0.4, "y": -0.14, "z": 0.009},
                    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                },
                "attached": False,
            }
        ],
        candidate_object_ids=["sphere_part"],
        frame_id="cell-a",
    )
    with pytest.raises(SceneSelectionError) as captured:
        validate_scene_selection(
            selection_context=context,
            selection_response=response,
        )
    assert captured.value.reason_code == reason_code

    if response["outcome"] == "no_match":
        assert validate_scene_selection(
            selection_context=context,
            selection_response=response,
            allow_no_match=True,
        )["outcome"] == "no_match"


def test_bind_requires_model_contract_only_for_programs_that_query_models() -> None:
    scene = HeadlessProgramSceneSource(_SceneTransport())
    transport = _ModelTransport()
    port = ProgramAssistantPort(transport, scene)
    try:
        ordinary = _program("pick_and_place_minimal.program.yaml")
        port.bind(_context(ordinary))

        with pytest.raises(ProgramAssistantPortError, match="no retained"):
            port.bind(_context(_program()))

        transport.reject_validation = True
        with pytest.raises(ProgramAssistantPortError, match="site transport"):
            port.bind(
                _context(
                    _program(),
                    assistant_projection=_assistant_projection(),
                )
            )
    finally:
        port.close()


def test_scene_selection_reads_fixed_and_native_resolved_carried_owner_poses() -> None:
    carried = _scene_object(
        "sphere_part",
        position=(0.4, -0.14, 0.009),
        shape="sphere",
        carried=True,
    )
    fixed = _scene_object("cube_part", position=(0.4, 0.14, 0.009))
    transport = _SceneTransport([carried, fixed])
    transport.resolved_poses["sphere_part"] = {
        "position_m": {"x": 0.6, "y": -0.1, "z": 0.5},
        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
    }
    source = HeadlessProgramSceneSource(transport)
    frame_id, objects = source.selection_objects(
        _context(_program(), assistant_projection=_assistant_projection()),
        ["sphere_part", "cube_part"],
    )

    assert frame_id == "cell-a"
    by_id = {row["scene_resource_id"]: row for row in objects}
    assert by_id["sphere_part"]["pose_in_cell"] == transport.resolved_poses[
        "sphere_part"
    ]
    assert by_id["sphere_part"]["attached"] is True
    assert by_id["cube_part"]["attached"] is False

    transport.mutate_pose_response = True
    with pytest.raises(ProgramAssistantPortError, match="changed owner revision"):
        source.selection_objects(
            _context(_program(), assistant_projection=_assistant_projection()),
            ["sphere_part"],
        )


def test_carried_scene_poses_use_bounded_queries_without_reducing_selection() -> None:
    carried = [
        _scene_object(
            f"part-{index:03d}",
            position=(0.0, 0.0, 0.0),
            carried=True,
        )
        for index in range(129)
    ]
    transport = _SceneTransport(carried)
    for row in carried:
        transport.resolved_poses[row["scene_resource_id"]] = {
            "position_m": {"x": 0.0, "y": 0.0, "z": 0.0},
            "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
        }

    _frame_id, objects = HeadlessProgramSceneSource(transport).selection_objects(
        _context(_program(), assistant_projection=_assistant_projection()),
        [],
    )

    assert len(objects) == 129
    assert [len(query["scene_resource_ids"]) for query in transport.pose_queries] == [
        128,
        1,
    ]


def test_selection_and_model_query_use_exact_captured_contracts() -> None:
    scene_transport = _SceneTransport(
        [
            _scene_object(
                "sphere_part", position=(0.4, -0.14, 0.009), shape="sphere"
            ),
            _scene_object("cube_part", position=(0.4, 0.14, 0.009)),
        ]
    )
    model_transport = _ModelTransport([_selected_response()])
    port = ProgramAssistantPort(
        model_transport,
        HeadlessProgramSceneSource(scene_transport),
    )
    try:
        port.bind(
            _context(_program(), assistant_projection=_assistant_projection())
        )
        assert len(model_transport.validated) == 1
        built = port.start(
            "assistant.build_scene_selection",
            _identity("build-1"),
            _selection_inputs(),
        )
        assert built.state is OwnerTaskState.SUCCEEDED

        started = port.start(
            "assistant.query_model",
            _identity("query-1"),
            _query_inputs(built.output),
        )
        assert started.state is OwnerTaskState.ACTIVE
        completed = _wait_terminal(port, started.task_id)

        assert completed.state is OwnerTaskState.SUCCEEDED
        assert completed.output["selected_object_id"] == "sphere_part"
        assert completed.metadata["model_endpoint_alias"] == "selection_model"
        assert completed.metadata["prompt_template_digest"] == _DIGEST_E
        assert completed.metadata["confidence"] == 0.92
        assert set(completed.metadata["argument_digests"]) == {
            "candidate_object_ids",
            "scene_context_json",
            "selection_request_text",
        }
        request = model_transport.requests[0]
        assert request.external_resource_alias == "selection_model"
        assert request.tool_call_family == "openai_chat_tools"
        assert request.response_tool is not None
        assert request.response_tool["name"] == "select_scene_object"
        assert request.timeout_s <= 20.0
        port.acknowledge(started.task_id, _identity("query-1"))
    finally:
        port.close()


def test_query_model_performs_at_most_one_parse_repair_retry() -> None:
    raw_response = json.dumps(
        _selected_response().tool_arguments,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    )
    transport = _ModelTransport(
        [
            ProgramModelResponse(
                tool_name="select_scene_object",
                tool_arguments="not-json",
            ),
            ProgramModelResponse(
                tool_name="select_scene_object",
                tool_arguments=raw_response,
            ),
        ]
    )
    port = ProgramAssistantPort(
        transport,
        HeadlessProgramSceneSource(_SceneTransport()),
    )
    try:
        port.bind(
            _context(_program(), assistant_projection=_assistant_projection())
        )
        selection_context = build_scene_selection_context(
            selection_request_text="Select the sphere object.",
            scene_objects=[
                {
                    **_scene_object(
                        "sphere_part", position=(0.4, -0.14, 0.009), shape="sphere"
                    ),
                    "pose_in_cell": {
                        "position_m": {"x": 0.4, "y": -0.14, "z": 0.009},
                        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                    },
                    "attached": False,
                },
                {
                    **_scene_object("cube_part", position=(0.4, 0.14, 0.009)),
                    "pose_in_cell": {
                        "position_m": {"x": 0.4, "y": 0.14, "z": 0.009},
                        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                    },
                    "attached": False,
                },
            ],
            candidate_object_ids=["sphere_part", "cube_part"],
            frame_id="cell-a",
        )
        started = port.start(
            "assistant.query_model",
            _identity("retry-1"),
            _query_inputs(selection_context, retry_on_parse_failure=True),
        )
        completed = _wait_terminal(port, started.task_id)

        assert completed.state is OwnerTaskState.SUCCEEDED
        assert len(transport.requests) == 2
        assert "previous response did not satisfy" in transport.requests[1].prompt
        assert completed.metadata["model_call_attempt_count"] == 2
        assert completed.metadata["initial_prompt_digest"] != completed.metadata[
            "prompt_digest"
        ]
        assert completed.metadata["model_response_digest"] == (
            "sha256:" + sha256(raw_response.encode("utf-8")).hexdigest()
        )
    finally:
        port.close()


def test_model_transport_response_rejects_unbounded_or_non_json_metadata() -> None:
    with pytest.raises(ValueError, match="model_version"):
        ProgramModelResponse(model_version="x" * 4_097)

    cyclic_usage: dict[str, Any] = {}
    cyclic_usage["cycle"] = cyclic_usage
    with pytest.raises(ValueError, match="finite acyclic JSON"):
        ProgramModelResponse(usage=cyclic_usage)


@pytest.mark.parametrize(
    ("response", "reason_code"),
    [
        (
            ProgramModelResponse(
                tool_name="select_scene_object",
                tool_arguments={
                    "confidence": 0.92,
                    "matched_attributes": [],
                    "outcome": "selected",
                    "reason": "outside candidates",
                    "selected_object_id": "table",
                },
            ),
            "model_response_schema_invalid",
        ),
        (
            ProgramModelResponse(
                tool_name="select_scene_object",
                tool_arguments={
                    "confidence": 0.5,
                    "matched_attributes": ["shape=sphere"],
                    "outcome": "selected",
                    "reason": "low confidence",
                    "selected_object_id": "sphere_part",
                },
            ),
            "model_confidence_below_threshold",
        ),
    ],
)
def test_query_model_rejects_untrusted_identity_or_low_confidence(
    response: ProgramModelResponse,
    reason_code: str,
) -> None:
    transport = _ModelTransport([response])
    port = ProgramAssistantPort(
        transport,
        HeadlessProgramSceneSource(_SceneTransport()),
    )
    try:
        port.bind(
            _context(_program(), assistant_projection=_assistant_projection())
        )
        selection_context = build_scene_selection_context(
            selection_request_text="Select the sphere object.",
            scene_objects=[
                {
                    **_scene_object(
                        "sphere_part",
                        position=(0.4, -0.14, 0.009),
                        shape="sphere",
                    ),
                    "pose_in_cell": {
                        "position_m": {"x": 0.4, "y": -0.14, "z": 0.009},
                        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                    },
                    "attached": False,
                }
            ],
            candidate_object_ids=["sphere_part"],
            frame_id="cell-a",
        )
        started = port.start(
            "assistant.query_model",
            _identity(f"reject-{reason_code}"),
            _query_inputs(selection_context),
        )
        completed = _wait_terminal(port, started.task_id)

        assert completed.state is OwnerTaskState.FAILED
        assert completed.reason_code == reason_code
        assert completed.metadata["authored_failure_policy"] == (
            "pause_for_operator"
        )
        assert completed.metadata["authored_failure_policy_parameter"] == (
            "model_low_confidence_policy"
            if reason_code == "model_confidence_below_threshold"
            else "model_runtime_failure_policy"
        )
    finally:
        port.close()


def test_unbind_cancels_and_releases_unacknowledged_run_tasks() -> None:
    port = ProgramAssistantPort(
        _ModelTransport(),
        HeadlessProgramSceneSource(_SceneTransport()),
    )
    try:
        port.bind(
            _context(_program(), assistant_projection=_assistant_projection())
        )
        terminal = port.start(
            "assistant.validate_scene_selection",
            _identity("retained-until-unbind"),
            OwnerNodeInputs(
                parameters={},
                blackboard={
                    "selection_context_key": {},
                    "selection_response_key": {},
                },
            ),
        )
        assert terminal.state is OwnerTaskState.FAILED

        port.unbind("program-run-1")

        with pytest.raises(ProgramAssistantPortError, match="unknown"):
            port.observe(terminal.task_id)
    finally:
        port.close()


def test_timed_out_task_suppresses_a_transport_that_returns_late() -> None:
    transport = _LateModelTransport(_selected_response())
    port = ProgramAssistantPort(
        transport,
        HeadlessProgramSceneSource(_SceneTransport()),
    )
    try:
        port.bind(
            _context(_program(), assistant_projection=_assistant_projection())
        )
        selection_context = build_scene_selection_context(
            selection_request_text="Select the sphere object.",
            scene_objects=[
                {
                    **_scene_object(
                        "sphere_part",
                        position=(0.4, -0.14, 0.009),
                        shape="sphere",
                    ),
                    "pose_in_cell": {
                        "position_m": {"x": 0.4, "y": -0.14, "z": 0.009},
                        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                    },
                    "attached": False,
                }
            ],
            candidate_object_ids=["sphere_part"],
            frame_id="cell-a",
        )
        started = port.start(
            "assistant.query_model",
            _identity("timeout-1"),
            _query_inputs(selection_context, timeout_s=0.05),
        )
        assert transport.started.wait(timeout=1.0)
        time.sleep(0.08)

        timed_out = port.observe(started.task_id)
        assert timed_out.state is OwnerTaskState.FAILED
        assert timed_out.reason_code == "model_request_timeout"
        transport.release.set()
        time.sleep(0.01)
        assert port.observe(started.task_id) == timed_out
    finally:
        transport.release.set()
        port.close()


def test_cancelled_task_suppresses_a_transport_that_returns_late() -> None:
    transport = _LateModelTransport(_selected_response())
    port = ProgramAssistantPort(
        transport,
        HeadlessProgramSceneSource(_SceneTransport()),
    )
    try:
        port.bind(
            _context(_program(), assistant_projection=_assistant_projection())
        )
        selection_context = build_scene_selection_context(
            selection_request_text="Select the sphere object.",
            scene_objects=[
                {
                    **_scene_object(
                        "sphere_part", position=(0.4, -0.14, 0.009), shape="sphere"
                    ),
                    "pose_in_cell": {
                        "position_m": {"x": 0.4, "y": -0.14, "z": 0.009},
                        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                    },
                    "attached": False,
                },
                {
                    **_scene_object("cube_part", position=(0.4, 0.14, 0.009)),
                    "pose_in_cell": {
                        "position_m": {"x": 0.4, "y": 0.14, "z": 0.009},
                        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                    },
                    "attached": False,
                },
            ],
            candidate_object_ids=["sphere_part", "cube_part"],
            frame_id="cell-a",
        )
        identity = _identity("cancel-1")
        started = port.start(
            "assistant.query_model",
            identity,
            _query_inputs(selection_context),
        )
        assert transport.started.wait(timeout=1.0)

        port.cancel(started.task_id, identity)
        assert port.observe(started.task_id).state is OwnerTaskState.INTERRUPTED
        transport.release.set()
        time.sleep(0.01)
        assert port.observe(started.task_id).state is OwnerTaskState.INTERRUPTED
    finally:
        transport.release.set()
        port.close()
