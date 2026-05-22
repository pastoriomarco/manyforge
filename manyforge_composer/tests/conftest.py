from __future__ import annotations

import pytest
import py_trees

from manyforge_composer.backend.state import stop_all_visual_state_publishers


@pytest.fixture(autouse=True)
def reset_py_trees_blackboard():
    """Prevent composer tests from leaking py_trees global state across cases."""
    py_trees.blackboard.Blackboard.enable_activity_stream()
    py_trees.blackboard.Blackboard.clear()
    yield
    stop_all_visual_state_publishers(timeout_s=1.0)
    py_trees.blackboard.Blackboard.clear()
