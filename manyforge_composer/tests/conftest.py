from __future__ import annotations

import pytest
import py_trees


@pytest.fixture(autouse=True)
def reset_py_trees_blackboard():
    """Prevent composer tests from leaking py_trees global state across cases."""
    py_trees.blackboard.Blackboard.enable_activity_stream()
    py_trees.blackboard.Blackboard.clear()
    yield
    py_trees.blackboard.Blackboard.clear()
