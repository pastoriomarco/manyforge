"""Assistant outputs use static applied-model frame authority."""
from types import SimpleNamespace

import pytest

from manyforge_behavior.assistant_program_port import (
    HeadlessProgramSceneSource, _json_schema, _validate_program_model_outputs,
)
from manyforge_behavior.nodes.model import ModelOutputValidationError


def test_assistant_non_pose_output_requires_no_role_declarations_or_lookup():
    calls = []
    _validate_program_model_outputs({"flag": True}, {"fields": [{"field": "flag", "type": "bool"}]},
                                    {"scene": []}, calls.append)
    assert calls == []


def test_assistant_pose_frame_uses_exact_static_captured_lookup():
    frame = {"component_id": "tool", "frame_id": "TCP"}
    declaration = {"fields": [{"field": "pose", "type": "stored_pose"}]}
    schema = _json_schema(declaration, {})
    frame_schema = schema["properties"]["pose"]["properties"]["frame_ref"]
    assert set(frame_schema["properties"]) == {"kind", "frame", "scene_resource_id"}
    pose = {"frame_ref": {"kind": "target_frame", "frame": frame},
            "position_m": [0, 0, 0], "orientation": {"xyzw": [0, 0, 0, 1]}}
    requests = []
    def lookup(body):
        requests.append(body)
        return {**body, "schema_version": "manyforge.component_frame_lookup_result.v1", "exists": True}
    source = HeadlessProgramSceneSource(SimpleNamespace(scene=lambda: {}, lookup_component_frame=lookup))
    capture = SimpleNamespace(retained_bundle_id="bundle", graph_revision="graph")
    _validate_program_model_outputs({"pose": pose}, declaration, {"scene": []},
                                    lambda value: source.validate_component_frame(capture, value))
    assert requests == [{"schema_version": "manyforge.component_frame_lookup.v1",
                         "retained_bundle_id": "bundle", "graph_revision": "graph", "frame": frame}]


def test_assistant_missing_or_mismatched_frame_lookup_fails_closed():
    frame = {"component_id": "tool", "frame_id": "TCP"}
    capture = SimpleNamespace(retained_bundle_id="bundle", graph_revision="graph")
    class Missing(RuntimeError):
        code = "frame_missing"
    def missing(_request):
        raise Missing("absent")
    source = HeadlessProgramSceneSource(SimpleNamespace(scene=lambda: {}, lookup_component_frame=missing))
    with pytest.raises(ModelOutputValidationError):
        source.validate_component_frame(capture, frame)
    class Stale(RuntimeError):
        code = "component_frame_lookup_stale"
    def stale(_request):
        raise Stale("changed")
    source = HeadlessProgramSceneSource(SimpleNamespace(scene=lambda: {}, lookup_component_frame=stale))
    with pytest.raises(Stale):
        source.validate_component_frame(capture, frame)
    source = HeadlessProgramSceneSource(SimpleNamespace(scene=lambda: {},
        lookup_component_frame=lambda request: {**request, "schema_version": "manyforge.component_frame_lookup_result.v1",
                                                "frame": {**frame, "frame_id": "other"}, "exists": True}))
    with pytest.raises(RuntimeError, match="crossed its captured identity"):
        source.validate_component_frame(capture, frame)
