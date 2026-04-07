"""Tests for capability key registry and profile definitions."""

import pytest

from manyforge_behavior.capabilities import (
    V0_0_REQUIRED_KEYS,
    V0_1_REQUIRED_KEYS,
    get_required_keys,
)


class TestV00RequiredKeys:
    def test_v0_0_required_keys_count(self):
        """Verify V0_0_REQUIRED_KEYS has exactly 19 keys."""
        assert len(V0_0_REQUIRED_KEYS) == 19

    def test_v0_0_required_keys_is_frozenset(self):
        """Verify V0_0_REQUIRED_KEYS is a frozenset (immutable)."""
        assert isinstance(V0_0_REQUIRED_KEYS, frozenset)

    def test_v0_0_required_keys_contains_planning_keys(self):
        """Verify planning capability keys are present."""
        assert "planning.motion.joint_goal" in V0_0_REQUIRED_KEYS
        assert "planning.motion.pose_goal" in V0_0_REQUIRED_KEYS
        assert "planning.trajectory.time_parameterized" in V0_0_REQUIRED_KEYS

    def test_v0_0_required_keys_contains_context_keys(self):
        """Verify context capability keys are present."""
        assert "context.state.current" in V0_0_REQUIRED_KEYS
        assert "context.validation.trajectory" in V0_0_REQUIRED_KEYS
        assert "context.collision.check" in V0_0_REQUIRED_KEYS
        assert "context.kinematics.fk" in V0_0_REQUIRED_KEYS
        assert "context.limits.joint" in V0_0_REQUIRED_KEYS

    def test_v0_0_required_keys_contains_scene_keys(self):
        """Verify scene capability keys are present."""
        assert "scene.shape.box" in V0_0_REQUIRED_KEYS
        assert "scene.shape.sphere" in V0_0_REQUIRED_KEYS
        assert "scene.edit.add" in V0_0_REQUIRED_KEYS
        assert "scene.edit.remove" in V0_0_REQUIRED_KEYS
        assert "scene.edit.update_pose" in V0_0_REQUIRED_KEYS

    def test_v0_0_required_keys_contains_execution_keys(self):
        """Verify execution capability keys are present."""
        assert "execution.trajectory.execute" in V0_0_REQUIRED_KEYS
        assert "execution.feedback.phase_events" in V0_0_REQUIRED_KEYS
        assert "execution.stop.soft" in V0_0_REQUIRED_KEYS
        assert "execution.pause" in V0_0_REQUIRED_KEYS
        assert "execution.resume" in V0_0_REQUIRED_KEYS
        assert "execution.reset" in V0_0_REQUIRED_KEYS


class TestGetRequiredKeys:
    def test_get_required_keys_v0_0(self):
        """Returns V0_0_REQUIRED_KEYS for v0.0 profile."""
        result = get_required_keys("v0.0")
        assert result == V0_0_REQUIRED_KEYS

    def test_get_required_keys_v0_1(self):
        """Returns V0_1_REQUIRED_KEYS for v0.1 profile."""
        result = get_required_keys("v0.1")
        assert result == V0_1_REQUIRED_KEYS

    def test_v0_1_includes_v0_0_plus_object_relative_keys(self):
        """v0.1 is additive over v0.0 for object-relative manipulation."""
        assert V0_0_REQUIRED_KEYS.issubset(V0_1_REQUIRED_KEYS)
        assert "context.pose.resolve_reference" in V0_1_REQUIRED_KEYS
        assert "context.scene.object_query" in V0_1_REQUIRED_KEYS
        assert "scene.edit.attach_detach" in V0_1_REQUIRED_KEYS

    def test_get_required_keys_unknown_profile(self):
        """Raises ValueError for unknown profile."""
        with pytest.raises(ValueError, match="Unknown program profile"):
            get_required_keys("unknown_profile")

    def test_get_required_keys_empty_string(self):
        """Raises ValueError for empty string profile."""
        with pytest.raises(ValueError, match="Unknown program profile"):
            get_required_keys("")
