"""Node-status traversal moves to the typed native execution snapshot."""

import pytest

from manyforge_behavior.cycle_manager import (
    CycleManager,
    CycleManagerUnavailableError,
)


def test_status_path_fails_before_touching_a_python_tree() -> None:
    class PoisonTree:
        def __getattribute__(self, name: str) -> object:
            raise AssertionError(f"Python tree was accessed: {name}")

    poison = PoisonTree()
    with pytest.raises(
        CycleManagerUnavailableError,
        match="composable_cell_cycle_manager_unavailable",
    ):
        CycleManager(poison, poison)
