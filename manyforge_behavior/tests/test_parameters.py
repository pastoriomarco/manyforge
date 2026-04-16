"""Tests for parameter resolution."""

import pytest
import py_trees

from manyforge_behavior.blackboard_namespace import (
    make_runtime_field_ref,
    write_namespace_bundle,
)
from manyforge_behavior.parameters import ParameterResolver, resolve_blackboard_value


class TestParameterResolver:
    def test_resolve_literal_values(self):
        resolver = ParameterResolver({"speed": 0.5})
        result = resolver.resolve({"velocity_scale": 1.0, "name": "test"})
        assert result == {"velocity_scale": 1.0, "name": "test"}

    def test_resolve_param_reference(self):
        resolver = ParameterResolver({"speed": 0.5})
        result = resolver.resolve({"velocity_scale": "${param:speed}"})
        assert result["velocity_scale"] == 0.5

    def test_override_takes_precedence(self):
        resolver = ParameterResolver(
            {"speed": 0.5}, overrides={"speed": 0.8}
        )
        result = resolver.resolve({"velocity_scale": "${param:speed}"})
        assert result["velocity_scale"] == 0.8

    def test_unknown_param_raises(self):
        resolver = ParameterResolver({"speed": 0.5})
        with pytest.raises(ValueError, match="Unknown parameter"):
            resolver.resolve({"velocity_scale": "${param:nonexistent}"})

    def test_non_string_passthrough(self):
        resolver = ParameterResolver({})
        result = resolver.resolve({"count": 42, "flag": True, "rate": 3.14})
        assert result == {"count": 42, "flag": True, "rate": 3.14}

    def test_nested_dict_resolution(self):
        resolver = ParameterResolver({"x": 1.0})
        result = resolver.resolve({
            "pose": {"position_m": ["${param:x}", 0.0, 0.5]}
        })
        assert result["pose"]["position_m"][0] == 1.0

    def test_list_resolution(self):
        resolver = ParameterResolver({"val": "hello"})
        result = resolver.resolve({"items": ["${param:val}", "world"]})
        assert result["items"] == ["hello", "world"]

    def test_partial_string_not_resolved(self):
        """Only exact ${param:name} matches are resolved, not embedded."""
        resolver = ParameterResolver({"name": "test"})
        result = resolver.resolve({"label": "prefix_${param:name}_suffix"})
        # Embedded references are not resolved (only exact match)
        assert result["label"] == "prefix_${param:name}_suffix"

    def test_empty_overrides(self):
        resolver = ParameterResolver({"a": 1}, overrides={})
        result = resolver.resolve({"x": "${param:a}"})
        assert result["x"] == 1

    def test_resolve_value_direct(self):
        resolver = ParameterResolver({"a": 42})
        assert resolver.resolve_value("${param:a}") == 42
        assert resolver.resolve_value("literal") == "literal"
        assert resolver.resolve_value(3.14) == 3.14


class TestBlackboardValueResolution:
    def test_runtime_field_ref_reads_declared_namespace_fields(self):
        bb = py_trees.blackboard.Client(name="test_fields")
        bb.register_key(key="model/test", access=py_trees.common.Access.WRITE)
        write_namespace_bundle(
            bb,
            "model/test",
            value={
                "selected_object_id": "aggregate_value",
                "grasp_pose": {
                    "position_m": [0.1, 0.2, 0.3],
                },
            },
            status="succeeded",
        )

        assert (
            resolve_blackboard_value(
                bb,
                make_runtime_field_ref("model/test", "selected_object_id"),
            )
            == "aggregate_value"
        )
        assert resolve_blackboard_value(
            bb,
            {
                "pose": make_runtime_field_ref("model/test", "grasp_pose"),
            },
        ) == {
            "pose": {"position_m": [0.1, 0.2, 0.3]},
        }
