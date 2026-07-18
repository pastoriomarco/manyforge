"""Tests for FallbackOverrideNode and RecoverySkillNode (OP-6).

Follows the test_repeat_retry.py / test_nodes.py pattern: direct py_trees
construction, tree.tick() assertions, blackboard verification.
"""

import py_trees
import pytest

from manyforge_behavior.blackboard_namespace import read_namespace_bundle
from manyforge_behavior.nodes.recovery import FallbackOverrideNode, RecoverySkillNode


@pytest.fixture(autouse=True)
def reset_blackboard():
    py_trees.blackboard.Blackboard.enable_activity_stream()
    yield
    py_trees.blackboard.Blackboard.clear()


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

class ConfigurableLeaf(py_trees.behaviour.Behaviour):
    """Leaf that reads behaviour from the action-node ``_params`` convention."""

    def __init__(self, name, fail_until=0, **params):
        super().__init__(name)
        self.tick_count = 0
        self.fail_until = fail_until
        self._params = dict(params)

    def initialise(self):
        pass

    def update(self):
        self.tick_count += 1
        if self.tick_count <= self.fail_until:
            return py_trees.common.Status.FAILURE
        return py_trees.common.Status.SUCCESS


class AlwaysFail(py_trees.behaviour.Behaviour):
    def update(self):
        return py_trees.common.Status.FAILURE


class AlwaysSucceed(py_trees.behaviour.Behaviour):
    def update(self):
        return py_trees.common.Status.SUCCESS


class RunningThenSucceed(py_trees.behaviour.Behaviour):
    """Returns RUNNING for a configurable number of ticks, then SUCCESS."""
    def __init__(self, name, running_ticks=1):
        super().__init__(name)
        self._running_ticks = running_ticks
        self._ticks = 0

    def initialise(self):
        self._ticks = 0

    def update(self):
        self._ticks += 1
        if self._ticks <= self._running_ticks:
            return py_trees.common.Status.RUNNING
        return py_trees.common.Status.SUCCESS


# ---------------------------------------------------------------------------
# FallbackOverrideNode tests
# ---------------------------------------------------------------------------

class TestFallbackOverrideNode:

    def test_child_succeeds_immediately(self):
        """When the child succeeds on first tick, the decorator returns SUCCESS."""
        child = ConfigurableLeaf("child", fail_until=0, speed=1.0)
        node = FallbackOverrideNode(
            "fallback", child, fallback_params={"speed": 0.5}, max_retries=2,
        )
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        tree.tick()
        assert tree.root.status == py_trees.common.Status.SUCCESS

    def test_child_fails_then_retries_with_overrides(self):
        """Child fails once, fallback params applied, retried, succeeds."""
        child = ConfigurableLeaf("child", fail_until=1, speed=1.0)
        node = FallbackOverrideNode(
            "fallback", child, fallback_params={"speed": 0.5}, max_retries=2,
        )
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        # Tick 1: child fails, decorator returns RUNNING (will retry)
        tree.tick()
        assert tree.root.status == py_trees.common.Status.RUNNING
        # Verify fallback params were applied to child
        assert child._params["speed"] == 0.5

        # Tick 2: child succeeds with overridden params
        tree.tick()
        assert tree.root.status == py_trees.common.Status.SUCCESS

    def test_retries_exhausted(self):
        """When retries are exhausted, returns FAILURE."""
        child = ConfigurableLeaf("child", fail_until=100, speed=1.0)
        node = FallbackOverrideNode(
            "fallback", child, fallback_params={"speed": 0.5}, max_retries=2,
        )
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        # Tick 1: child fails, retry 1
        tree.tick()
        assert tree.root.status == py_trees.common.Status.RUNNING

        # Tick 2: child fails again, retry 2
        tree.tick()
        assert tree.root.status == py_trees.common.Status.RUNNING

        # Tick 3: child fails, retries exhausted
        tree.tick()
        assert tree.root.status == py_trees.common.Status.FAILURE

    def test_zero_retries(self):
        """max_retries=0: child gets one attempt, no retries."""
        child = ConfigurableLeaf("child", fail_until=100)
        node = FallbackOverrideNode(
            "fallback", child, fallback_params={}, max_retries=0,
        )
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        tree.tick()
        assert tree.root.status == py_trees.common.Status.FAILURE

    def test_params_restored_across_runs(self):
        """Fallback param overrides from a previous run do not leak into the next."""
        child = ConfigurableLeaf("child", fail_until=1, speed=1.0, accel=0.8)
        node = FallbackOverrideNode(
            "fallback", child, fallback_params={"speed": 0.3}, max_retries=2,
        )
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        # Run 1: fails then succeeds
        tree.tick()  # fail → apply override
        assert child._params["speed"] == 0.3
        tree.tick()  # succeed

        # Force re-initialise for a fresh run
        # (simulates cycle_manager re-entering the tree)
        node.initialise()
        child.tick_count = 0
        child.fail_until = 0

        # After re-init, original speed must be restored
        assert child._params["speed"] == 1.0
        assert child._params["accel"] == 0.8

    def test_blackboard_recovery_metadata(self):
        """Recovery events are recorded on the blackboard."""
        child = ConfigurableLeaf("child", fail_until=1)
        node = FallbackOverrideNode(
            "fallback", child, fallback_params={"x": 1}, max_retries=2,
        )
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        tree.tick()  # fail → retry
        tree.tick()  # succeed

        bb = py_trees.blackboard.Client(name="reader")
        bundle = read_namespace_bundle(bb, "_recovery/fallback")
        assert bundle["meta"]["status"] == "succeeded"
        assert bundle["value"]["retry_count"] == 1

    def test_child_running_propagated(self):
        """When child returns RUNNING, decorator also returns RUNNING."""
        child = RunningThenSucceed("child", running_ticks=2)
        node = FallbackOverrideNode(
            "fallback", child, fallback_params={}, max_retries=1,
        )
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        tree.tick()
        assert tree.root.status == py_trees.common.Status.RUNNING
        tree.tick()
        assert tree.root.status == py_trees.common.Status.RUNNING
        tree.tick()
        assert tree.root.status == py_trees.common.Status.SUCCESS


# ---------------------------------------------------------------------------
# RecoverySkillNode tests
# ---------------------------------------------------------------------------

def _make_skill_expander(recovery_behaviour):
    """Create a skill_expander that returns a fixed behaviour."""
    def expander(skill_id, params):
        return recovery_behaviour
    return expander


class TestRecoverySkillNode:

    def test_child_succeeds_no_recovery(self):
        """When child succeeds, no recovery skill is invoked."""
        child = ConfigurableLeaf("child", fail_until=0)
        expander = _make_skill_expander(AlwaysSucceed("recovery"))
        node = RecoverySkillNode(
            "recovery_node", child,
            recovery_skill_id="fix_it",
            recovery_params={},
            max_retries=2,
            skill_expander=expander,
        )
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        tree.tick()
        assert tree.root.status == py_trees.common.Status.SUCCESS

    def test_recovery_skill_runs_on_failure(self):
        """Child fails → recovery skill runs and succeeds → child retried."""
        child = ConfigurableLeaf("child", fail_until=1)
        recovery = AlwaysSucceed("recovery_skill")
        expander = _make_skill_expander(recovery)
        node = RecoverySkillNode(
            "recovery_node", child,
            recovery_skill_id="fix_it",
            max_retries=2,
            skill_expander=expander,
        )
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        # Tick 1: child fails → recovery started (instant succeed) → RUNNING (will retry child)
        tree.tick()
        assert tree.root.status == py_trees.common.Status.RUNNING

        # Tick 2: child succeeds on second attempt
        tree.tick()
        assert tree.root.status == py_trees.common.Status.SUCCESS

    def test_recovery_skill_fails_aborts(self):
        """If the recovery skill itself fails, the decorator returns FAILURE."""
        child = ConfigurableLeaf("child", fail_until=100)
        recovery = AlwaysFail("bad_recovery")
        expander = _make_skill_expander(recovery)
        node = RecoverySkillNode(
            "recovery_node", child,
            recovery_skill_id="fix_it",
            max_retries=2,
            skill_expander=expander,
        )
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        # Tick 1: child fails → recovery skill fails → FAILURE
        tree.tick()
        assert tree.root.status == py_trees.common.Status.FAILURE

    def test_retries_exhausted(self):
        """When max_retries exceeded, returns FAILURE even if recovery succeeds."""
        child = ConfigurableLeaf("child", fail_until=100)
        recovery = AlwaysSucceed("recovery_skill")
        expander = _make_skill_expander(recovery)
        node = RecoverySkillNode(
            "recovery_node", child,
            recovery_skill_id="fix_it",
            max_retries=1,
            skill_expander=expander,
        )
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        # Tick 1: child fails → recovery succeeds → RUNNING
        tree.tick()
        assert tree.root.status == py_trees.common.Status.RUNNING

        # Tick 2: child fails again → retries exhausted → FAILURE
        tree.tick()
        assert tree.root.status == py_trees.common.Status.FAILURE

    def test_no_skill_expander(self):
        """Without skill_expander, decorator returns FAILURE on child failure."""
        child = ConfigurableLeaf("child", fail_until=100)
        node = RecoverySkillNode(
            "recovery_node", child,
            recovery_skill_id="fix_it",
            max_retries=2,
            skill_expander=None,
        )
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        tree.tick()
        assert tree.root.status == py_trees.common.Status.FAILURE

    def test_unknown_skill_id(self):
        """If skill_expander returns None (unknown skill), returns FAILURE."""
        child = ConfigurableLeaf("child", fail_until=100)
        expander = _make_skill_expander(None)  # always returns None
        node = RecoverySkillNode(
            "recovery_node", child,
            recovery_skill_id="nonexistent",
            max_retries=2,
            skill_expander=expander,
        )
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        tree.tick()
        assert tree.root.status == py_trees.common.Status.FAILURE

    def test_blackboard_recovery_metadata(self):
        """Recovery metadata is written to the blackboard."""
        child = ConfigurableLeaf("child", fail_until=1)
        recovery = AlwaysSucceed("recovery_skill")
        expander = _make_skill_expander(recovery)
        node = RecoverySkillNode(
            "recovery_node", child,
            recovery_skill_id="fix_it",
            max_retries=2,
            skill_expander=expander,
        )
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        tree.tick()  # fail → recovery → RUNNING
        tree.tick()  # succeed

        bb = py_trees.blackboard.Client(name="reader")
        bundle = read_namespace_bundle(bb, "_recovery/recovery_node")
        assert bundle["meta"]["status"] == "succeeded"
        assert bundle["value"]["recovery_skill_id"] == "fix_it"

    def test_child_running_propagated(self):
        """When child returns RUNNING, decorator also returns RUNNING."""
        child = RunningThenSucceed("child", running_ticks=2)
        node = RecoverySkillNode(
            "recovery_node", child,
            recovery_skill_id="fix_it",
            max_retries=1,
            skill_expander=_make_skill_expander(AlwaysSucceed("recovery")),
        )
        tree = py_trees.trees.BehaviourTree(root=node)
        tree.setup()

        tree.tick()
        assert tree.root.status == py_trees.common.Status.RUNNING
        tree.tick()
        assert tree.root.status == py_trees.common.Status.RUNNING
        tree.tick()
        assert tree.root.status == py_trees.common.Status.SUCCESS


class TestRecoveryCatalogEntries:
    """Verify catalog entries exist for recovery nodes."""

    def test_catalog_has_recovery_entries(self):
        from pathlib import Path
        from manyforge_behavior.catalog import NodeCatalog

        catalog_path = Path(__file__).parent.parent / "resources" / "node_catalog.yaml"
        catalog = NodeCatalog(catalog_path)

        fallback_entry = None
        recovery_entry = None
        for entry in catalog.entries:
            if entry.id == "fallback_override_node":
                fallback_entry = entry
            elif entry.id == "recovery_skill_node":
                recovery_entry = entry

        assert fallback_entry is not None, "fallback_override_node not in catalog"
        assert recovery_entry is not None, "recovery_skill_node not in catalog"

        assert fallback_entry.kind == "decorator"
        assert fallback_entry.children_min == 1
        assert fallback_entry.children_max == 1

        assert recovery_entry.kind == "decorator"
        assert recovery_entry.children_min == 1
        assert recovery_entry.children_max == 1
