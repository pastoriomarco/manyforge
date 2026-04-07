"""Tests for CycleManager node status collection."""
import pytest
import py_trees
from unittest.mock import MagicMock

from manyforge_behavior.cycle_manager import CycleManager, _collect_statuses


def _simulate_tick_status_collection(cm):
    """Simulate what the tick loop does: collect statuses and store atomically."""
    if cm._tree is None:
        return
    statuses = {}
    _collect_statuses(cm._tree.root, statuses)
    cm._node_statuses = statuses


def test_get_node_statuses_no_tree():
    """get_node_statuses returns empty dict when no tree."""
    # Create a CycleManager with mock runner/kernel but don't build tree
    kernel = MagicMock()
    runner = MagicMock()
    cm = CycleManager(runner, kernel)
    
    # Tree is None by default
    assert cm._tree is None
    
    statuses = cm.get_node_statuses()
    assert statuses == {}


def test_get_node_statuses_after_tick():
    """After ticking, all nodes have a status."""
    # Build a simple tree manually using py_trees
    root = py_trees.composites.Sequence(name="main_seq", memory=True)
    child1 = py_trees.behaviours.Success(name="always_success")
    child2 = py_trees.behaviours.Running(name="always_running")
    root.add_children([child1, child2])
    
    tree = py_trees.trees.BehaviourTree(root=root)
    tree.setup()
    tree.tick()
    
    # Create a minimal CycleManager and inject the tree
    kernel = MagicMock()
    runner = MagicMock()
    cm = CycleManager(runner, kernel)
    cm._tree = tree
    _simulate_tick_status_collection(cm)

    statuses = cm.get_node_statuses()

    assert "main_seq" in statuses
    assert "always_success" in statuses
    assert "always_running" in statuses
    assert statuses["always_success"] == "SUCCESS"
    assert statuses["always_running"] == "RUNNING"


def test_get_node_statuses_nested_tree():
    """Statuses are collected from nested tree structures."""
    # Build a nested tree
    root = py_trees.composites.Sequence(name="root_seq", memory=True)
    
    seq1 = py_trees.composites.Sequence(name="seq1", memory=True)
    action1 = py_trees.behaviours.Success(name="action1")
    seq1.add_child(action1)
    
    seq2 = py_trees.composites.Sequence(name="seq2", memory=True)
    action2 = py_trees.behaviours.Failure(name="action2")
    seq2.add_child(action2)
    
    root.add_children([seq1, seq2])
    
    tree = py_trees.trees.BehaviourTree(root=root)
    tree.setup()
    tree.tick()
    
    # Create a minimal CycleManager and inject the tree
    kernel = MagicMock()
    runner = MagicMock()
    cm = CycleManager(runner, kernel)
    cm._tree = tree
    _simulate_tick_status_collection(cm)

    statuses = cm.get_node_statuses()

    # All nodes should be present
    assert "root_seq" in statuses
    assert "seq1" in statuses
    assert "action1" in statuses
    assert "seq2" in statuses
    assert "action2" in statuses

    # Check statuses
    assert statuses["action1"] == "SUCCESS"
    assert statuses["action2"] == "FAILURE"


def test_get_node_statuses_with_decorator():
    """Statuses are collected from decorator nodes."""
    # Build a tree with a decorator
    root = py_trees.composites.Sequence(name="root_seq", memory=True)
    
    # Inverter decorator wrapping a Success node
    inner_success = py_trees.behaviours.Success(name="inner_success")
    inverter = py_trees.decorators.Inverter("inverter_node", inner_success)
    root.add_child(inverter)
    
    tree = py_trees.trees.BehaviourTree(root=root)
    tree.setup()
    tree.tick()
    
    # Create a minimal CycleManager and inject the tree
    kernel = MagicMock()
    runner = MagicMock()
    cm = CycleManager(runner, kernel)
    cm._tree = tree
    _simulate_tick_status_collection(cm)

    statuses = cm.get_node_statuses()

    # All nodes should be present
    assert "root_seq" in statuses
    assert "inverter_node" in statuses
    assert "inner_success" in statuses


def test_get_node_statuses_invalid_status():
    """Nodes without a status return INVALID."""
    # Build a tree but don't tick it
    root = py_trees.composites.Sequence(name="main_seq", memory=True)
    child1 = py_trees.behaviours.Success(name="always_success")
    root.add_child(child1)
    
    tree = py_trees.trees.BehaviourTree(root=root)
    tree.setup()
    # Don't tick - nodes will have INVALID status
    
    # Create a minimal CycleManager and inject the tree
    kernel = MagicMock()
    runner = MagicMock()
    cm = CycleManager(runner, kernel)
    cm._tree = tree
    _simulate_tick_status_collection(cm)

    statuses = cm.get_node_statuses()

    assert "main_seq" in statuses
    assert "always_success" in statuses
    assert statuses["main_seq"] == "INVALID"
    assert statuses["always_success"] == "INVALID"
