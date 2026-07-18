"""Tests for repeat and retry decorator nodes.

py_trees decorator semantics: one cycle/attempt per tree tick.
repeat(3) over an instant-SUCCESS child takes 3 tree ticks to complete.
retry(5) over a fail-then-succeed child takes N+1 tree ticks (N failures + 1 success).
"""

import py_trees
from pathlib import Path
from manyforge_behavior.node_environment import build_node_environment

from manyforge_behavior.nodes.control_flow import RepeatDecorator, RetryDecorator
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.kernel_client import MockKernelClient
from manyforge_behavior.program import Program
from manyforge_behavior.runner import OrchestratorRunner


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

    def test_repeat_forever_caps(self):
        """repeat(num_cycles=-1): infinite repeat. After 10 ticks, still RUNNING."""
        leaf = CountingLeaf("leaf", fail_until=0)
        repeat = RepeatDecorator("repeat", child=leaf, num_cycles=-1)
        tree = py_trees.trees.BehaviourTree(root=repeat)
        tree.setup()

        for _ in range(10):
            tree.tick()
            assert tree.root.status == py_trees.common.Status.RUNNING
        assert leaf.tick_count == 10

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

    def test_retry_forever(self):
        """retry(num_attempts=-1) with leaf that fails 10 times then succeeds.
        Takes 11 tree ticks."""
        leaf = CountingLeaf("leaf", fail_until=10)
        retry = RetryDecorator("retry", child=leaf, num_attempts=-1)
        tree = py_trees.trees.BehaviourTree(root=retry)
        tree.setup()

        for _ in range(10):
            tree.tick()
            assert tree.root.status == py_trees.common.Status.RUNNING
        tree.tick()
        assert tree.root.status == py_trees.common.Status.SUCCESS
        assert leaf.tick_count == 11

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
        retry(-1) > repeat(3) > sequence([leaf]).
        Takes 3 tree ticks — repeat advances once per tick, then retry sees SUCCESS."""
        leaf = CountingLeaf("leaf", fail_until=0)
        sequence = py_trees.composites.Sequence("sequence", memory=True, children=[leaf])
        repeat = RepeatDecorator("repeat", child=sequence, num_cycles=3)
        retry = RetryDecorator("retry", child=repeat, num_attempts=-1)
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
        assert repeat_entry.parameters[0].name == "num_cycles"
        assert repeat_entry.parameters[0].type == "int"
        assert repeat_entry.parameters[0].default == -1

        assert retry_entry.kind == "decorator"
        assert retry_entry.children_min == 1
        assert retry_entry.children_max == 1
        assert len(retry_entry.parameters) == 1
        assert retry_entry.parameters[0].name == "num_attempts"
        assert retry_entry.parameters[0].type == "int"
        assert retry_entry.parameters[0].default == -1


class TestFixtureProgram:
    """Tests for the looping_program fixture."""

    def test_load_fixture_program(self):
        """Load the looping_program fixture via Program and build it via the runner.
        retry(-1) > repeat(3) > sequence > set_key_bool_value."""
        catalog_path = Path(__file__).parent.parent / "resources" / "node_catalog.yaml"
        program_path = Path(__file__).parent / "fixtures" / "looping_program.program.yaml"

        catalog = NodeCatalog(catalog_path)
        program = Program(program_path, catalog)

        assert program.tree.id == "retry"
        assert program.tree.name == "cycle_manager"
        assert program.tree.params["num_attempts"] == -1

        retry_child = program.tree.children[0]
        assert retry_child.id == "repeat"
        assert retry_child.name == "work_cycle"
        assert retry_child.params["num_cycles"] == 3

        repeat_child = retry_child.children[0]
        assert repeat_child.id == "sequence"
        assert repeat_child.name == "cycle_body"

        sequence_child = repeat_child.children[0]
        assert sequence_child.id == "set_key_bool_value"
        assert sequence_child.name == "mark_done"
        assert sequence_child.params["key"] == {"key_ref": "cycle_ran"}
        assert sequence_child.params["value"] is True

        # Build and run via the runner
        kernel = MockKernelClient()
        runner = OrchestratorRunner(
            node_environment=build_node_environment(catalog_path),
            program_path=str(program_path),
            kernel=kernel,
        )
        runner.load()
        tree = runner.build_tree()

        assert tree.root.name == "cycle_manager"
        assert isinstance(tree.root, RetryDecorator)

        retry_child = tree.root.children[0]
        assert retry_child.name == "work_cycle"
        assert isinstance(retry_child, RepeatDecorator)

        # Run: retry(-1) > repeat(3) completes in 3 ticks
        result = runner.run(max_ticks=100)
        assert result is True
        assert tree.root.status == py_trees.common.Status.SUCCESS
