"""Tests for QueryModelNode (OP-7).

The model HTTP call is patched so tests run without external dependencies.
Tests verify non-blocking execution, blackboard writes, schema validation,
audit event recording, and error handling.
"""

from __future__ import annotations

import json
import threading
import time
from dataclasses import dataclass, field
from typing import Any, Optional
from unittest.mock import MagicMock, patch

import py_trees
import pytest

from manyforge_behavior.blackboard_namespace import (
    make_runtime_field_ref,
    read_namespace_bundle,
    write_namespace_bundle,
)
from manyforge_behavior.deployment import (
    ModelAuditPolicy,
    ModelEndpoint,
    PromptTemplate,
)
from manyforge_behavior.nodes.model import (
    QueryModelNode,
    _call_model,
    _parse_json_text,
    _render_prompt_template,
)
from manyforge_behavior.runtime_pause import RuntimePauseRegistry


@pytest.fixture(autouse=True)
def reset_blackboard():
    py_trees.blackboard.Blackboard.enable_activity_stream()
    yield
    py_trees.blackboard.Blackboard.clear()


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _make_endpoint(**overrides) -> ModelEndpoint:
    defaults = dict(
        alias="test-ep",
        provider="openai",
        model_id="gpt-4o-mini",
        base_url="http://localhost:11434/v1",
        api_key_env=None,
        max_tokens=256,
    )
    defaults.update(overrides)
    return ModelEndpoint(**defaults)


def _make_template(
    text="Hello {{name}}",
    output_schema=None,
    response_tool=None,
    **overrides,
) -> PromptTemplate:
    defaults = dict(
        template_id="greet-v1",
        version="1.0",
        template_text=text,
    )
    defaults.update(overrides)
    t = PromptTemplate(**defaults)
    t.output_schema = output_schema
    t.response_tool = response_tool
    return t


def _make_node(
    endpoint=None,
    template=None,
    template_variables=None,
    output_key_prefix="model/test",
    event_recorder=None,
    audit_policy=None,
    min_confidence=0.0,
    include_scene_snapshot=False,
    scene_snapshot_provider=None,
    **kwargs,
) -> QueryModelNode:
    return QueryModelNode(
        name="test_query",
        model_endpoint_alias=(endpoint or _make_endpoint()).alias,
        prompt_template_id=(template or _make_template()).template_id,
        template_variables=template_variables or {},
        output_key_prefix=output_key_prefix,
        endpoint=endpoint or _make_endpoint(),
        template=template or _make_template(),
        event_recorder=event_recorder,
        audit_policy=audit_policy,
        min_confidence=min_confidence,
        include_scene_snapshot=include_scene_snapshot,
        scene_snapshot_provider=scene_snapshot_provider,
        **kwargs,
    )


def _tick_until_done(tree, max_ticks=50):
    """Tick until root is not RUNNING, return final status.

    A brief sleep between ticks allows the ThreadPoolExecutor to complete
    the model call future (the mock returns instantly but the GIL needs
    to be released for the thread pool worker to run).
    """
    import time as _time
    for _ in range(max_ticks):
        tree.tick()
        if tree.root.status != py_trees.common.Status.RUNNING:
            return tree.root.status
        _time.sleep(0.005)
    return tree.root.status


def _mock_call_result(text="hello", parsed=None, model_version="test-v1"):
    return {
        "text": text,
        "parsed": parsed,
        "model_version": model_version,
        "tool_call": None,
        "usage": {"input_tokens": 10, "output_tokens": 5},
    }


def _read_model_bundle(prefix: str = "model/test") -> dict[str, Any]:
    bb = py_trees.blackboard.Client(name=f"reader_{prefix.replace('/', '_')}")
    return read_namespace_bundle(bb, prefix)


def _assert_only_runtime_fault(
    registry: RuntimePauseRegistry,
    *,
    reason_code: str,
) -> None:
    faults = registry.active_faults()
    assert len(faults) == 1
    assert faults[0].domain == "model"
    assert faults[0].reason_code == reason_code


# ---------------------------------------------------------------------------
# Basic lifecycle
# ---------------------------------------------------------------------------

class TestQueryModelNodeLifecycle:

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_success_plain_text(self, mock_call):
        """Plain text response (no schema) → SUCCESS, blackboard written."""
        mock_call.return_value = _mock_call_result(text="world")
        node = _make_node()
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        status = _tick_until_done(tree)
        assert status == py_trees.common.Status.SUCCESS

        bundle = _read_model_bundle()
        assert bundle["value"] == "world"
        assert bundle["meta"]["status"] == "succeeded"
        assert bundle["meta"]["model_version"] == "test-v1"

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_success_json_response(self, mock_call):
        """JSON response with no schema → parsed dict on blackboard."""
        result_data = {"action": "pick", "confidence": 0.95}
        mock_call.return_value = _mock_call_result(
            text=json.dumps(result_data), parsed=result_data,
        )
        node = _make_node()
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        status = _tick_until_done(tree)
        assert status == py_trees.common.Status.SUCCESS

        bundle = _read_model_bundle()
        assert bundle["value"] == result_data
        assert bundle["meta"]["confidence"] == 0.95

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_no_endpoint_returns_failure(self, mock_call):
        """Missing endpoint configuration → immediate FAILURE."""
        node = _make_node(endpoint=_make_endpoint())
        # Overwrite endpoint to None after construction
        node._endpoint = None
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        status = _tick_until_done(tree)
        assert status == py_trees.common.Status.FAILURE
        mock_call.assert_not_called()

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_no_template_returns_failure(self, mock_call):
        """Missing template → immediate FAILURE."""
        node = _make_node()
        node._template = None
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        status = _tick_until_done(tree)
        assert status == py_trees.common.Status.FAILURE
        mock_call.assert_not_called()


# ---------------------------------------------------------------------------
# Schema validation
# ---------------------------------------------------------------------------

class TestQueryModelSchemaValidation:

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_valid_schema(self, mock_call):
        """Response matching output_schema → SUCCESS."""
        schema = {
            "type": "object",
            "required": ["action"],
            "properties": {"action": {"type": "string"}},
        }
        template = _make_template(output_schema=schema)
        result = {"action": "pick"}
        mock_call.return_value = _mock_call_result(
            text=json.dumps(result), parsed=result,
        )
        node = _make_node(template=template)
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        status = _tick_until_done(tree)
        assert status == py_trees.common.Status.SUCCESS

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_schema_validation_failure(self, mock_call):
        """Response missing required field → FAILURE."""
        schema = {
            "type": "object",
            "required": ["action", "target"],
            "properties": {
                "action": {"type": "string"},
                "target": {"type": "string"},
            },
        }
        template = _make_template(output_schema=schema)
        result = {"action": "pick"}  # missing "target"
        mock_call.return_value = _mock_call_result(
            text=json.dumps(result), parsed=result,
        )
        node = _make_node(template=template)
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        status = _tick_until_done(tree)
        assert status == py_trees.common.Status.FAILURE

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_non_json_with_schema_fails(self, mock_call):
        """Schema requires JSON but response is plain text → FAILURE."""
        schema = {"type": "object", "properties": {}}
        template = _make_template(output_schema=schema)
        mock_call.return_value = _mock_call_result(text="not json", parsed=None)
        node = _make_node(template=template)
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        status = _tick_until_done(tree)
        assert status == py_trees.common.Status.FAILURE

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_runtime_enum_schema_accepts_candidate_from_template_variable(self, mock_call):
        """selected_object_id can be constrained by a runtime enum sourced from blackboard."""
        writer = py_trees.blackboard.Client(name="writer_runtime_enum")
        writer.register_key(
            key="selection/context",
            access=py_trees.common.Access.WRITE,
        )
        write_namespace_bundle(
            writer,
            "selection/context",
            value={"candidate_object_ids": ["sphere_part", "cube_part"]},
            status="built",
        )

        schema = {
            "type": "object",
            "required": ["selected_object_id"],
            "properties": {
                "selected_object_id": {
                    "type": "string",
                    "x-manyforge-enum-template-variable": "candidate_object_ids",
                },
            },
        }
        template = _make_template(output_schema=schema)
        result = {"selected_object_id": "sphere_part"}
        mock_call.return_value = _mock_call_result(
            text=json.dumps(result),
            parsed=result,
        )
        node = _make_node(
            template=template,
            template_variables={
                "candidate_object_ids": make_runtime_field_ref(
                    "selection/context",
                    "candidate_object_ids",
                ),
            },
        )
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        status = _tick_until_done(tree)
        assert status == py_trees.common.Status.SUCCESS

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_runtime_enum_schema_rejects_non_candidate(self, mock_call):
        """Dynamic enum validation fails closed when model invents an object id."""
        writer = py_trees.blackboard.Client(name="writer_runtime_enum_invalid")
        writer.register_key(
            key="selection/context",
            access=py_trees.common.Access.WRITE,
        )
        write_namespace_bundle(
            writer,
            "selection/context",
            value={"candidate_object_ids": ["sphere_part", "cube_part"]},
            status="built",
        )

        schema = {
            "type": "object",
            "required": ["selected_object_id"],
            "properties": {
                "selected_object_id": {
                    "type": "string",
                    "x-manyforge-enum-template-variable": "candidate_object_ids",
                },
            },
        }
        template = _make_template(output_schema=schema)
        result = {"selected_object_id": "object_4"}
        mock_call.return_value = _mock_call_result(
            text=json.dumps(result),
            parsed=result,
        )
        node = _make_node(
            template=template,
            template_variables={
                "candidate_object_ids": make_runtime_field_ref(
                    "selection/context",
                    "candidate_object_ids",
                ),
            },
        )
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        status = _tick_until_done(tree)
        assert status == py_trees.common.Status.FAILURE

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_runtime_enum_schema_can_allow_empty_string(self, mock_call):
        """Dynamic enum can explicitly allow empty string for no_match/ambiguous paths."""
        writer = py_trees.blackboard.Client(name="writer_runtime_enum_empty")
        writer.register_key(
            key="selection/context",
            access=py_trees.common.Access.WRITE,
        )
        write_namespace_bundle(
            writer,
            "selection/context",
            value={"candidate_object_ids": ["sphere_part", "cube_part"]},
            status="built",
        )

        schema = {
            "type": "object",
            "required": ["outcome", "selected_object_id"],
            "properties": {
                "outcome": {
                    "type": "string",
                    "enum": ["selected", "no_match", "ambiguous"],
                },
                "selected_object_id": {
                    "type": "string",
                    "x-manyforge-enum-template-variable": "candidate_object_ids",
                    "x-manyforge-enum-allow-empty-string": True,
                },
            },
        }
        template = _make_template(output_schema=schema)
        result = {"outcome": "no_match", "selected_object_id": ""}
        mock_call.return_value = _mock_call_result(
            text=json.dumps(result),
            parsed=result,
        )
        node = _make_node(
            template=template,
            template_variables={
                "candidate_object_ids": make_runtime_field_ref(
                    "selection/context",
                    "candidate_object_ids",
                ),
            },
        )
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        status = _tick_until_done(tree)
        assert status == py_trees.common.Status.SUCCESS

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_response_tool_accepts_runtime_enum_candidate(self, mock_call):
        """Tool-call arguments can use the same runtime enum expansion path."""
        writer = py_trees.blackboard.Client(name="writer_response_tool")
        writer.register_key(
            key="selection/context",
            access=py_trees.common.Access.WRITE,
        )
        write_namespace_bundle(
            writer,
            "selection/context",
            value={"candidate_object_ids": ["sphere_part", "cube_part"]},
            status="built",
        )

        response_tool = {
            "name": "select_scene_object",
            "description": "Choose a scene object",
            "parameters": {
                "type": "object",
                "required": ["outcome", "selected_object_id", "confidence", "reason"],
                "properties": {
                    "outcome": {
                        "type": "string",
                        "enum": ["selected", "no_match", "ambiguous"],
                    },
                    "selected_object_id": {
                        "type": "string",
                        "x-manyforge-enum-template-variable": "candidate_object_ids",
                        "x-manyforge-enum-allow-empty-string": True,
                    },
                    "confidence": {"type": "number"},
                    "reason": {"type": "string"},
                },
            },
        }
        result = {
            "outcome": "selected",
            "selected_object_id": "sphere_part",
            "confidence": 0.95,
            "reason": "shape matched",
        }
        mock_call.return_value = {
            "text": "",
            "parsed": result,
            "model_version": "test-v1",
            "tool_call": {
                "name": "select_scene_object",
                "arguments_text": json.dumps(result),
                "id": "call_1",
                "raw": {},
            },
            "usage": {"input_tokens": 10, "output_tokens": 5},
        }
        template = _make_template(response_tool=response_tool)
        node = _make_node(
            template=template,
            template_variables={
                "candidate_object_ids": make_runtime_field_ref(
                    "selection/context",
                    "candidate_object_ids",
                ),
            },
        )
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        status = _tick_until_done(tree)
        assert status == py_trees.common.Status.SUCCESS

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_response_tool_requires_matching_tool_call(self, mock_call):
        """Configured response tools fail closed when the model does not call them."""
        response_tool = {
            "name": "select_scene_object",
            "parameters": {
                "type": "object",
                "required": ["outcome", "selected_object_id", "confidence", "reason"],
                "properties": {
                    "outcome": {
                        "type": "string",
                        "enum": ["selected", "no_match", "ambiguous"],
                    },
                    "selected_object_id": {"type": "string"},
                    "confidence": {"type": "number"},
                    "reason": {"type": "string"},
                },
            },
        }
        mock_call.return_value = _mock_call_result(
            text=json.dumps({"outcome": "selected"}),
            parsed={"outcome": "selected"},
        )
        template = _make_template(response_tool=response_tool)
        node = _make_node(template=template)
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        status = _tick_until_done(tree)
        assert status == py_trees.common.Status.FAILURE
        assert "tool_call_missing_or_mismatched" in node.feedback_message


# ---------------------------------------------------------------------------
# Error handling
# ---------------------------------------------------------------------------

class TestQueryModelErrors:

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_model_call_exception(self, mock_call):
        """A transient model call fault pauses for an exact recovery action."""
        mock_call.side_effect = RuntimeError("connection refused")
        registry = RuntimePauseRegistry()
        node = _make_node(runtime_pause_registry=registry)
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        status = _tick_until_done(tree)
        assert status == py_trees.common.Status.RUNNING
        _assert_only_runtime_fault(registry, reason_code="model_call_failed")

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_model_call_exception_can_be_authored_as_condition_failure(self, mock_call):
        mock_call.side_effect = RuntimeError("connection refused")
        registry = RuntimePauseRegistry()
        node = _make_node(
            model_runtime_failure_policy="condition_failure",
            runtime_pause_registry=registry,
        )
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        assert _tick_until_done(tree) == py_trees.common.Status.FAILURE
        assert registry.active_faults() == ()

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_snapshot_requested_without_provider_fails(self, mock_call):
        """Snapshot-backed queries fail closed when no render provider exists."""
        node = _make_node(include_scene_snapshot=True)
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        status = _tick_until_done(tree)
        assert status == py_trees.common.Status.FAILURE
        mock_call.assert_not_called()

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_min_confidence_threshold(self, mock_call):
        """Low-confidence output pauses rather than selecting a fallback branch."""
        mock_call.return_value = _mock_call_result(text="maybe", parsed=None)
        registry = RuntimePauseRegistry()
        node = _make_node(min_confidence=0.8, runtime_pause_registry=registry)
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        status = _tick_until_done(tree)
        assert status == py_trees.common.Status.RUNNING
        _assert_only_runtime_fault(
            registry,
            reason_code="model_confidence_below_threshold",
        )

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_min_confidence_feedback_includes_structured_reason(self, mock_call):
        """Low-confidence structured output should surface outcome/reason in feedback."""
        result = {
            "outcome": "no_match",
            "selected_object_id": "",
            "matched_attributes": [],
            "confidence": 0.1,
            "reason": "No selection request was provided.",
            "request_interpretation": "I could not identify a concrete request.",
        }
        mock_call.return_value = _mock_call_result(
            text=json.dumps(result),
            parsed=result,
        )
        registry = RuntimePauseRegistry()
        node = _make_node(min_confidence=0.7, runtime_pause_registry=registry)
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        status = _tick_until_done(tree)
        assert status == py_trees.common.Status.RUNNING
        assert "confidence 0.10 below threshold 0.70" in node.feedback_message
        assert "outcome=no_match" in node.feedback_message
        assert "reason=No selection request was provided." in node.feedback_message
        _assert_only_runtime_fault(
            registry,
            reason_code="model_confidence_below_threshold",
        )


# ---------------------------------------------------------------------------
# Template variable resolution
# ---------------------------------------------------------------------------

class TestQueryModelTemplateVars:

    def test_render_prompt_template_allows_whitespace_inside_placeholders(self):
        """Deployment templates commonly use ``{{ var }}`` with spaces."""
        rendered = _render_prompt_template(
            "Request: {{ selection_request_text }} | Context: {{scene_context_json}}",
            {
                "selection_request_text": "Select the sphere object.",
                "scene_context_json": '{"candidate_object_ids":["sphere_part","cube_part"]}',
            },
        )
        assert "Select the sphere object." in rendered
        assert "{{ selection_request_text }}" not in rendered
        assert "{{scene_context_json}}" not in rendered

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_template_variables_resolved_from_blackboard(self, mock_call):
        """Template variables are resolved from blackboard keys."""
        mock_call.return_value = _mock_call_result(text="ok")
        # Set up BB with a value for the template variable
        writer = py_trees.blackboard.Client(name="writer")
        writer.register_key(key="scene/description", access=py_trees.common.Access.WRITE)
        writer.set("scene/description", "a red box on a table")

        template = _make_template(text="Analyze: {{scene}}")
        node = _make_node(
            template=template,
            template_variables={"scene": "scene/description"},
        )
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        status = _tick_until_done(tree)
        assert status == py_trees.common.Status.SUCCESS
        # Verify the resolved prompt was passed to _call_model
        call_kwargs = mock_call.call_args[1]
        assert call_kwargs["prompt"] == "Analyze: a red box on a table"

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_template_variables_resolve_with_spaced_placeholders(self, mock_call):
        """Blackboard substitution should handle ``{{ variable }}`` placeholders too."""
        mock_call.return_value = _mock_call_result(text="ok")
        writer = py_trees.blackboard.Client(name="writer_spaced")
        writer.register_key(
            key="selection/request",
            access=py_trees.common.Access.WRITE,
        )
        writer.set("selection/request", "Select the sphere object.")

        template = _make_template(text="Selection request: {{ selection_request_text }}")
        node = _make_node(
            template=template,
            template_variables={
                "selection_request_text": "selection/request",
            },
        )
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        status = _tick_until_done(tree)
        assert status == py_trees.common.Status.SUCCESS
        call_kwargs = mock_call.call_args[1]
        assert call_kwargs["prompt"] == "Selection request: Select the sphere object."

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_scene_snapshot_is_attached_to_model_call(self, mock_call):
        """Snapshot bytes are rendered in-process and forwarded to the model call."""
        mock_call.return_value = _mock_call_result(text="ok")
        render_calls = []

        def render(width, height, format):
            render_calls.append((width, height, format))
            return b"fake-image"

        node = _make_node(
            include_scene_snapshot=True,
            scene_snapshot_provider=render,
            scene_snapshot_width=320,
            scene_snapshot_height=240,
            scene_snapshot_format="png",
        )
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        status = _tick_until_done(tree)
        assert status == py_trees.common.Status.SUCCESS
        assert render_calls == [(320, 240, "png")]
        call_kwargs = mock_call.call_args[1]
        assert call_kwargs["scene_snapshot_bytes"] == b"fake-image"
        assert call_kwargs["scene_snapshot_url"] is None
        assert call_kwargs["scene_snapshot_format"] == "png"

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_scene_snapshot_waits_non_blockingly_for_cached_frame(self, mock_call):
        """Snapshot-backed queries stay RUNNING until a cached frame becomes available."""
        mock_call.return_value = _mock_call_result(text="ok")
        render_results = [None, b"fake-image"]

        def render(width, height, format):
            assert (width, height, format) == (320, 240, "png")
            return render_results.pop(0)

        node = _make_node(
            include_scene_snapshot=True,
            scene_snapshot_provider=render,
            scene_snapshot_width=320,
            scene_snapshot_height=240,
            scene_snapshot_format="png",
        )
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        tree.tick()
        assert tree.root.status == py_trees.common.Status.RUNNING
        assert node.feedback_message == "waiting_for_scene_snapshot"
        assert not mock_call.called

        status = _tick_until_done(tree)
        assert status == py_trees.common.Status.SUCCESS
        call_kwargs = mock_call.call_args[1]
        assert call_kwargs["scene_snapshot_bytes"] == b"fake-image"
        assert call_kwargs["scene_snapshot_url"] is None

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_scene_snapshot_can_be_published_as_http_url(self, mock_call):
        """HTTP transport publishes the exact snapshot and passes the URL downstream."""
        mock_call.return_value = _mock_call_result(text="ok")

        def render(width, height, format):
            assert (width, height, format) == (320, 240, "png")
            return b"fake-image"

        published = []

        endpoint = _make_endpoint(
            base_url="http://172.17.0.1:8000/v1",
            image_fetch_base_url="http://172.17.0.1:9000",
        )

        def publish(image_bytes, width, height, format, **kwargs):
            published.append((image_bytes, width, height, format, kwargs))
            return {
                "snapshot_id": "snap-123",
                "url": "http://172.17.0.1:9000/api/scene/snapshots/snap-123",
            }

        node = _make_node(
            endpoint=endpoint,
            include_scene_snapshot=True,
            scene_snapshot_transport="http_url",
            scene_snapshot_provider=render,
            scene_snapshot_url_publisher=publish,
            scene_snapshot_width=320,
            scene_snapshot_height=240,
            scene_snapshot_format="png",
        )
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        status = _tick_until_done(tree)
        assert status == py_trees.common.Status.SUCCESS
        assert published == [(
            b"fake-image",
            320,
            240,
            "png",
            {
                "fetch_base_url": "http://172.17.0.1:9000",
                "model_base_url": "http://172.17.0.1:8000/v1",
            },
        )]
        call_kwargs = mock_call.call_args[1]
        assert call_kwargs["scene_snapshot_bytes"] == b"fake-image"
        assert call_kwargs["scene_snapshot_url"] == "http://172.17.0.1:9000/api/scene/snapshots/snap-123"
        assert call_kwargs["scene_snapshot_format"] == "png"

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_http_scene_snapshot_transport_fails_without_publisher(self, mock_call):
        """HTTP transport fails closed when no URL publisher is available."""
        node = _make_node(
            include_scene_snapshot=True,
            scene_snapshot_transport="http_url",
            scene_snapshot_provider=lambda width, height, format: b"fake-image",
        )
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        status = _tick_until_done(tree)
        assert status == py_trees.common.Status.FAILURE
        assert node.feedback_message == "scene_snapshot_url_publisher_unavailable"
        assert not mock_call.called

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_structured_response_is_written_to_namespace_value_bundle(self, mock_call):
        """Validated JSON fields are published under the namespace root bundle."""
        result_data = {
            "pick_left_bin": True,
            "selected_object_id": "left_part",
            "confidence": 0.82,
        }
        mock_call.return_value = _mock_call_result(
            text=json.dumps(result_data),
            parsed=result_data,
        )
        node = _make_node()
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        status = _tick_until_done(tree)
        assert status == py_trees.common.Status.SUCCESS

        bundle = _read_model_bundle()
        assert bundle["value"]["pick_left_bin"] is True
        assert bundle["value"]["selected_object_id"] == "left_part"
        assert bundle["meta"]["confidence"] == pytest.approx(0.82)

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_nested_structured_object_field_is_written_inside_bundle_value(self, mock_call):
        """Object-valued fields stay nested inside the published bundle value."""
        result_data = {
            "grasp_pose": {
                "position_m": [0.42, -0.05, 0.31],
                "orientation_rpy_deg": [180.0, 0.0, -90.0],
            },
            "confidence": 0.94,
        }
        mock_call.return_value = _mock_call_result(
            text=json.dumps(result_data),
            parsed=result_data,
        )
        node = _make_node()
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        status = _tick_until_done(tree)
        assert status == py_trees.common.Status.SUCCESS

        bundle = _read_model_bundle()
        assert bundle["value"]["grasp_pose"] == result_data["grasp_pose"]
        assert bundle["value"]["grasp_pose"]["position_m"] == [0.42, -0.05, 0.31]

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_next_success_replaces_prior_bundle_value_shape(self, mock_call):
        """A later success replaces the entire namespace bundle value."""
        first = {"pick_left_bin": True, "selected_object_id": "left_part"}
        second = {"pick_left_bin": False}
        mock_call.side_effect = [
            _mock_call_result(text=json.dumps(first), parsed=first),
            _mock_call_result(text=json.dumps(second), parsed=second),
        ]

        first_node = _make_node()
        first_tree = py_trees.trees.BehaviourTree(root=first_node)
        first_tree.setup()
        assert _tick_until_done(first_tree) == py_trees.common.Status.SUCCESS

        second_node = _make_node()
        second_tree = py_trees.trees.BehaviourTree(root=second_node)
        second_tree.setup()
        assert _tick_until_done(second_tree) == py_trees.common.Status.SUCCESS

        bundle = _read_model_bundle()
        assert bundle["value"]["pick_left_bin"] is False
        assert "selected_object_id" not in bundle["value"]


# ---------------------------------------------------------------------------
# Audit event recording
# ---------------------------------------------------------------------------

class TestQueryModelAudit:

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_events_recorded(self, mock_call):
        """Started and completed events are recorded on the event_recorder."""
        mock_call.return_value = _mock_call_result(text="done")
        recorder = MagicMock()
        node = _make_node(event_recorder=recorder)
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        status = _tick_until_done(tree)
        assert status == py_trees.common.Status.SUCCESS

        calls = recorder.record_model_event.call_args_list
        assert len(calls) == 2
        assert calls[0][1]["action"] == "started"
        assert calls[1][1]["action"] == "completed"

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_failure_event_recorded(self, mock_call):
        """Failed model call records a 'failed' event."""
        mock_call.side_effect = RuntimeError("timeout")
        recorder = MagicMock()
        registry = RuntimePauseRegistry()
        node = _make_node(
            event_recorder=recorder,
            runtime_pause_registry=registry,
        )
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        status = _tick_until_done(tree)
        assert status == py_trees.common.Status.RUNNING

        calls = recorder.record_model_event.call_args_list
        assert len(calls) == 2
        assert calls[0][1]["action"] == "started"
        assert calls[1][1]["action"] == "failed"
        assert "timeout" in calls[1][1]["error"]
        _assert_only_runtime_fault(registry, reason_code="model_call_failed")

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_intervention_context_propagated(self, mock_call):
        """run_id, session_id, principal_id are forwarded to event recorder."""
        mock_call.return_value = _mock_call_result(text="ok")
        recorder = MagicMock()
        node = _make_node(
            event_recorder=recorder,
            run_id="run-001",
            session_id="sess-042",
            principal_id="llm:planner-v2",
        )
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        _tick_until_done(tree)

        for call in recorder.record_model_event.call_args_list:
            kwargs = call[1]
            assert kwargs.get("run_id") == "run-001"
            assert kwargs.get("session_id") == "sess-042"
            assert kwargs.get("principal_id") == "llm:planner-v2"


class TestQueryModelComplianceFixes:

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_retry_on_parse_failure_retries_once_and_succeeds(self, mock_call):
        """A single repair retry is allowed when structured parsing fails."""
        schema = {
            "type": "object",
            "required": ["action"],
            "properties": {"action": {"type": "string"}},
        }
        template = _make_template(output_schema=schema)
        mock_call.side_effect = [
            _mock_call_result(text="not-json", parsed=None),
            _mock_call_result(text='{"action":"pick"}', parsed={"action": "pick"}),
        ]
        node = _make_node(
            template=template,
            retry_on_parse_failure=True,
        )
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        status = _tick_until_done(tree)
        assert status == py_trees.common.Status.SUCCESS
        assert mock_call.call_count == 2

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_stop_requested_cancels_running_node(self, mock_call):
        """The BT stop_requested path causes QueryModelNode to fail closed."""
        release = threading.Event()

        def delayed_call(*args, **kwargs):
            release.wait(timeout=1.0)
            return _mock_call_result(text="ok")

        mock_call.side_effect = delayed_call
        node = _make_node()
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        tree.tick()
        assert tree.root.status == py_trees.common.Status.RUNNING

        stopper = py_trees.blackboard.Client(name="stop_writer")
        stopper.register_key(key="stop_requested", access=py_trees.common.Access.WRITE)
        stopper.stop_requested = True

        tree.tick()
        release.set()

        assert tree.root.status == py_trees.common.Status.FAILURE
        assert node.feedback_message == "stop_requested"

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_output_commit_rolls_back_on_failure(self, mock_call):
        """Namespace writes are rolled back if any write fails mid-commit."""
        result = {"action": "pick", "confidence": 0.9}
        mock_call.return_value = _mock_call_result(
            text=json.dumps(result),
            parsed=result,
        )
        registry = RuntimePauseRegistry()
        node = _make_node(runtime_pause_registry=registry)
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        with patch(
            "manyforge_behavior.nodes.model.write_namespace_bundle",
            side_effect=RuntimeError("simulated write failure"),
        ):
            status = _tick_until_done(tree)

        assert status == py_trees.common.Status.RUNNING
        _assert_only_runtime_fault(
            registry,
            reason_code="model_output_commit_failed",
        )

        with pytest.raises(KeyError):
            _read_model_bundle()


# ---------------------------------------------------------------------------
# _call_model unit tests (provider dispatch)
# ---------------------------------------------------------------------------

class TestCallModelFunction:

    def test_unsupported_provider(self):
        with pytest.raises(ValueError, match="Unsupported model provider"):
            _call_model(
                provider="unknown",
                model_id="x",
                base_url=None,
                api_key_env=None,
                prompt="hi",
                max_tokens=10,
                timeout_s=5.0,
            )

    def test_parse_json_text_accepts_fenced_json(self):
        parsed = _parse_json_text(
            "```json\n{\"left_is_red\": true, \"confidence\": 1.0}\n```"
        )
        assert parsed == {"left_is_red": True, "confidence": 1.0}

    def test_openai_multimodal_request_uses_image_content(self):
        """OpenAI-compatible calls attach snapshots as image_url content parts."""
        client = MagicMock()
        response = MagicMock()
        response.json.return_value = {
            "choices": [{"message": {"content": "ok"}}],
            "model": "gemma-test",
            "usage": {"prompt_tokens": 12, "completion_tokens": 3},
        }
        response.raise_for_status.return_value = None
        client.post.return_value = response

        with patch("manyforge_behavior.nodes.model._get_http_client", return_value=client):
            result = _call_model(
                provider="openai",
                model_id="gemma-test",
                base_url="http://localhost:8000/v1",
                api_key_env=None,
                prompt="Describe the scene",
                max_tokens=128,
                timeout_s=5.0,
                scene_snapshot_bytes=b"\xff\xd8fake",
                scene_snapshot_format="jpeg",
            )

        assert result["text"] == "ok"
        _, kwargs = client.post.call_args
        payload = kwargs["json"]
        assert payload["messages"][0]["content"][0] == {
            "type": "text",
            "text": "Describe the scene",
        }
        image_part = payload["messages"][0]["content"][1]
        assert image_part["type"] == "image_url"
        assert image_part["image_url"]["url"].startswith("data:image/jpeg;base64,")

    def test_openai_multimodal_request_can_use_http_snapshot_url(self):
        """OpenAI-compatible calls may attach an HTTP-served snapshot URL."""
        client = MagicMock()
        response = MagicMock()
        response.json.return_value = {
            "choices": [{"message": {"content": "ok"}}],
            "model": "gemma-test",
            "usage": {"prompt_tokens": 12, "completion_tokens": 3},
        }
        response.raise_for_status.return_value = None
        client.post.return_value = response

        with patch("manyforge_behavior.nodes.model._get_http_client", return_value=client):
            result = _call_model(
                provider="openai",
                model_id="gemma-test",
                base_url="http://localhost:8000/v1",
                api_key_env=None,
                prompt="Describe the scene",
                max_tokens=128,
                timeout_s=5.0,
                scene_snapshot_bytes=b"\xff\xd8fake",
                scene_snapshot_url="http://localhost:9000/api/scene/snapshots/snap-123",
                scene_snapshot_format="jpeg",
            )

        assert result["text"] == "ok"
        _, kwargs = client.post.call_args
        payload = kwargs["json"]
        image_part = payload["messages"][0]["content"][1]
        assert image_part["type"] == "image_url"
        assert image_part["image_url"]["url"] == "http://localhost:9000/api/scene/snapshots/snap-123"
