"""Tests for repeat and retry decorator nodes.

py_trees decorator semantics: one cycle/attempt per tree tick.
repeat(3) over an instant-SUCCESS child takes 3 tree ticks to complete.
retry(5) over a fail-then-succeed child takes N+1 tree ticks (N failures + 1 success).
"""

import py_trees
import pytest
from pathlib import Path

from manyforge_behavior.nodes.control_flow import RepeatDecorator, RetryDecorator
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.catalog_schema import (
    CatalogSchemaError,
    parse_schema,
    validate_value,
)
from manyforge_behavior.program_runtime import ProgramRunAttemptSource


@pytest.mark.parametrize("retry", [False, True])
def test_indefinite_execution_yields_each_tick_and_has_no_lifetime_counter(retry):
    leaf = CountingLeaf("leaf", fail_until=100_000 if retry else 0)
    node = (
        RetryDecorator("forever", leaf) if retry else RepeatDecorator("forever", leaf)
    )
    tree = py_trees.trees.BehaviourTree(node)
    for _ in range(10_000):
        tree.tick()
        assert node.status is py_trees.common.Status.RUNNING
    assert leaf.tick_count == 10_000
    assert getattr(node, "_attempt_count" if retry else "_cycle_count") == 0
    node.stop(py_trees.common.Status.INVALID)
    assert leaf.status is py_trees.common.Status.INVALID
    # Resume starts an ordinary fresh attempt, with no discarded backlog.
    tree.tick()
    assert leaf.tick_count == 10_001


def test_attempt_ids_retain_no_counts_or_completed_identity_history():
    source = ProgramRunAttemptSource("run.indefinite")
    before = vars(source).copy()
    identifiers = {source.begin("root/leaf").node_attempt_id for _ in range(20_000)}
    assert len(identifiers) == 20_000
    assert vars(source) == before


def test_loop_schema_and_json_schema_reject_zero_but_accept_indefinite():
    catalog = NodeCatalog(Path(__file__).parents[1] / "resources/node_catalog.yaml")
    for node_id in ("repeat", "retry"):
        parameter = catalog.get(node_id).parameters[0]
        for value in (-1, 1, 1_000_000):
            assert not validate_value(
                parameter.schema, value, registry=catalog.schema_registry
            )
        for value in (-2, 0, 1_000_001, True, 1.5):
            assert validate_value(
                parameter.schema, value, registry=catalog.schema_registry
            )
        projected = catalog.schema_registry.to_json_schema(parameter.schema)
        assert projected["minimum"] == -1
        assert projected["not"] == {"const": 0}


@pytest.mark.parametrize(
    "value", [True, "0", float("nan"), float("inf"), float("-inf")]
)
def test_numeric_exclusion_rejects_nonfinite_or_untyped_constraint(value):
    with pytest.raises(CatalogSchemaError):
        parse_schema({"type": "int", "constraints": {"not_equal": value}}, path="loop")


class CountingLeaf(py_trees.behaviour.Behaviour):
    """Leaf that tracks tick count and can be configured to fail/succeed."""

    def __init__(self, name, fail_until=0):
        super().__init__(name)
        self.tick_count = 0
        self.fail_until = fail_until  # fail for first N ticks

    def update(self):
        self.tick_count += 1
        if self.tick_count <= self.fail_until:
            return py_trees.common.Status.FAILURE
        return py_trees.common.Status.SUCCESS


class TestRepeatDecorator:
    """Tests for RepeatDecorator."""

    def test_repeat_fixed_count(self):
        """repeat(num_cycles=3): child succeeds immediately each tick.
        Takes 3 tree ticks — first 2 return RUNNING, 3rd returns SUCCESS."""
        leaf = CountingLeaf("leaf", fail_until=0)
        repeat = RepeatDecorator("repeat", child=leaf, num_cycles=3)
        tree = py_trees.trees.BehaviourTree(root=repeat)
        tree.setup()

        # Ticks 1 and 2: child succeeds, but more cycles remain -> RUNNING
        tree.tick()
        assert tree.root.status == py_trees.common.Status.RUNNING
        assert leaf.tick_count == 1
        tree.tick()
        assert tree.root.status == py_trees.common.Status.RUNNING
        assert leaf.tick_count == 2
        # Tick 3: 3rd cycle completes -> SUCCESS
        tree.tick()
        assert tree.root.status == py_trees.common.Status.SUCCESS
        assert leaf.tick_count == 3

    def test_repeat_single_cycle(self):
        """repeat(num_cycles=1): completes in one tick."""
        leaf = CountingLeaf("leaf", fail_until=0)
        repeat = RepeatDecorator("repeat", child=leaf, num_cycles=1)
        tree = py_trees.trees.BehaviourTree(root=repeat)
        tree.setup()

        tree.tick()
        assert tree.root.status == py_trees.common.Status.SUCCESS
        assert leaf.tick_count == 1

    @pytest.mark.parametrize("count", [-2, 0, 1_000_001])
    def test_repeat_rejects_out_of_schema_counts(self, count):
        leaf = CountingLeaf("leaf", fail_until=0)
        with pytest.raises(ValueError):
            RepeatDecorator("repeat", child=leaf, num_cycles=count)

    def test_repeat_propagates_failure(self):
        """repeat(num_cycles=5) with a leaf that always fails.
        Should return FAILURE on first tick."""
        leaf = CountingLeaf("leaf", fail_until=10)  # always fails
        repeat = RepeatDecorator("repeat", child=leaf, num_cycles=5)
        tree = py_trees.trees.BehaviourTree(root=repeat)
        tree.setup()

        tree.tick()
        assert tree.root.status == py_trees.common.Status.FAILURE
        assert leaf.tick_count == 1


class TestRetryDecorator:
    """Tests for RetryDecorator."""

    def test_retry_succeeds_after_failures(self):
        """retry(num_attempts=5) with leaf that fails twice then succeeds.
        Takes 3 tree ticks."""
        leaf = CountingLeaf("leaf", fail_until=2)
        retry = RetryDecorator("retry", child=leaf, num_attempts=5)
        tree = py_trees.trees.BehaviourTree(root=retry)
        tree.setup()

        # Tick 1: child fails, attempt 1, RUNNING
        tree.tick()
        assert tree.root.status == py_trees.common.Status.RUNNING
        assert leaf.tick_count == 1
        # Tick 2: child fails, attempt 2, RUNNING
        tree.tick()
        assert tree.root.status == py_trees.common.Status.RUNNING
        assert leaf.tick_count == 2
        # Tick 3: child succeeds, SUCCESS
        tree.tick()
        assert tree.root.status == py_trees.common.Status.SUCCESS
        assert leaf.tick_count == 3

    def test_retry_gives_up(self):
        """retry(num_attempts=2) with leaf that always fails.
        After 2 failures, returns FAILURE."""
        leaf = CountingLeaf("leaf", fail_until=10)
        retry = RetryDecorator("retry", child=leaf, num_attempts=2)
        tree = py_trees.trees.BehaviourTree(root=retry)
        tree.setup()

        # Tick 1: child fails, attempt 1, RUNNING
        tree.tick()
        assert tree.root.status == py_trees.common.Status.RUNNING
        assert leaf.tick_count == 1
        # Tick 2: child fails, attempt 2 (exhausted), FAILURE
        tree.tick()
        assert tree.root.status == py_trees.common.Status.FAILURE
        assert leaf.tick_count == 2

    @pytest.mark.parametrize("count", [-2, 0, 1_000_001])
    def test_retry_rejects_out_of_schema_counts(self, count):
        leaf = CountingLeaf("leaf", fail_until=0)
        with pytest.raises(ValueError):
            RetryDecorator("retry", child=leaf, num_attempts=count)

    def test_retry_immediate_success(self):
        """retry(num_attempts=5) with leaf that succeeds immediately.
        Returns SUCCESS on first tick."""
        leaf = CountingLeaf("leaf", fail_until=0)
        retry = RetryDecorator("retry", child=leaf, num_attempts=5)
        tree = py_trees.trees.BehaviourTree(root=retry)
        tree.setup()

        tree.tick()
        assert tree.root.status == py_trees.common.Status.SUCCESS
        assert leaf.tick_count == 1


class TestNestedRetryRepeat:
    """Tests for nested retry and repeat decorators."""

    def test_nested_retry_repeat(self):
        """The cycle manager pattern:
        retry(2) > repeat(3) > sequence([leaf]).
        Takes 3 tree ticks — repeat advances once per tick, then retry sees SUCCESS."""
        leaf = CountingLeaf("leaf", fail_until=0)
        sequence = py_trees.composites.Sequence(
            "sequence", memory=True, children=[leaf]
        )
        repeat = RepeatDecorator("repeat", child=sequence, num_cycles=3)
        retry = RetryDecorator("retry", child=repeat, num_attempts=2)
        tree = py_trees.trees.BehaviourTree(root=retry)
        tree.setup()

        # First 2 ticks: repeat RUNNING
        tree.tick()
        assert tree.root.status == py_trees.common.Status.RUNNING
        tree.tick()
        assert tree.root.status == py_trees.common.Status.RUNNING
        # Tick 3: repeat finishes, retry SUCCESS
        tree.tick()
        assert tree.root.status == py_trees.common.Status.SUCCESS
        assert leaf.tick_count == 3


class TestCatalogEntries:
    """Tests for catalog entries."""

    def test_catalog_has_entries(self):
        """Load node_catalog.yaml, verify repeat and retry entries exist
        with correct kind=decorator, children constraints, and parameters."""
        catalog_path = Path(__file__).parent.parent / "resources" / "node_catalog.yaml"
        catalog = NodeCatalog(catalog_path)

        repeat_entry = None
        retry_entry = None
        for entry in catalog.entries:
            if entry.id == "repeat":
                repeat_entry = entry
            elif entry.id == "retry":
                retry_entry = entry

        assert repeat_entry is not None, "repeat entry not found in catalog"
        assert retry_entry is not None, "retry entry not found in catalog"

        assert repeat_entry.kind == "decorator"
        assert repeat_entry.children_min == 1
        assert repeat_entry.children_max == 1
        assert len(repeat_entry.parameters) == 1
        assert repeat_entry.parameters[0].id == "num_cycles"
        assert repeat_entry.parameters[0].required is False
        assert repeat_entry.parameters[0].default == -1
        assert repeat_entry.parameters[0].schema.schema_type == "int"
        assert dict(repeat_entry.parameters[0].schema.constraints) == {
            "min_inclusive": -1,
            "max_inclusive": 1_000_000,
            "not_equal": 0,
        }

        assert retry_entry.kind == "decorator"
        assert retry_entry.children_min == 1
        assert retry_entry.children_max == 1
        assert len(retry_entry.parameters) == 1
        assert retry_entry.parameters[0].id == "num_attempts"
        assert retry_entry.parameters[0].required is False
        assert retry_entry.parameters[0].default == -1
        assert retry_entry.parameters[0].schema.schema_type == "int"
        assert dict(retry_entry.parameters[0].schema.constraints) == {
            "min_inclusive": -1,
            "max_inclusive": 1_000_000,
            "not_equal": 0,
        }
