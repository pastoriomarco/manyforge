"""Tests for QueryModelNode (OP-7).

The model HTTP call is patched so tests run without external dependencies.
Tests verify non-blocking execution, blackboard writes, schema validation,
audit event recording, and error handling.
"""

from __future__ import annotations

import json
import time
from dataclasses import dataclass, field
from typing import Any, Optional
from unittest.mock import MagicMock, patch

import py_trees
import pytest

from manyforge_behavior.deployment import (
    ModelAuditPolicy,
    ModelEndpoint,
    PromptTemplate,
)
from manyforge_behavior.nodes.model import QueryModelNode, _call_model


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


def _make_template(text="Hello {{name}}", output_schema=None, **overrides) -> PromptTemplate:
    defaults = dict(
        template_id="greet-v1",
        version="1.0",
        template_text=text,
    )
    defaults.update(overrides)
    t = PromptTemplate(**defaults)
    t.output_schema = output_schema
    return t


def _make_node(
    endpoint=None,
    template=None,
    template_variables=None,
    output_key_prefix="model/test",
    event_recorder=None,
    audit_policy=None,
    min_confidence=0.0,
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
        "usage": {"input_tokens": 10, "output_tokens": 5},
    }


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

        bb = py_trees.blackboard.Client(name="reader")
        bb.register_key(key="model/test/response", access=py_trees.common.Access.READ)
        bb.register_key(key="model/test/status", access=py_trees.common.Access.READ)
        bb.register_key(key="model/test/model_version", access=py_trees.common.Access.READ)
        assert bb.get("model/test/response") == "world"
        assert bb.get("model/test/status") == "succeeded"
        assert bb.get("model/test/model_version") == "test-v1"

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

        bb = py_trees.blackboard.Client(name="reader")
        bb.register_key(key="model/test/response", access=py_trees.common.Access.READ)
        bb.register_key(key="model/test/confidence", access=py_trees.common.Access.READ)
        assert bb.get("model/test/response") == result_data
        assert bb.get("model/test/confidence") == 1.0  # parsed → confidence=1.0

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


# ---------------------------------------------------------------------------
# Error handling
# ---------------------------------------------------------------------------

class TestQueryModelErrors:

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_model_call_exception(self, mock_call):
        """Exception from model call → FAILURE."""
        mock_call.side_effect = RuntimeError("connection refused")
        node = _make_node()
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        status = _tick_until_done(tree)
        assert status == py_trees.common.Status.FAILURE

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_min_confidence_threshold(self, mock_call):
        """Plain text response has confidence=0.5 which is below min_confidence=0.8 → FAILURE."""
        mock_call.return_value = _mock_call_result(text="maybe", parsed=None)
        node = _make_node(min_confidence=0.8)
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        status = _tick_until_done(tree)
        assert status == py_trees.common.Status.FAILURE


# ---------------------------------------------------------------------------
# Template variable resolution
# ---------------------------------------------------------------------------

class TestQueryModelTemplateVars:

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
        node = _make_node(event_recorder=recorder)
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        status = _tick_until_done(tree)
        assert status == py_trees.common.Status.FAILURE

        calls = recorder.record_model_event.call_args_list
        assert len(calls) == 2
        assert calls[0][1]["action"] == "started"
        assert calls[1][1]["action"] == "failed"
        assert "timeout" in calls[1][1]["error"]

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
