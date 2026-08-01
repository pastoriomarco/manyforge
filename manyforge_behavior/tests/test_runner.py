"""Cutover gate for Program-Binding execution."""

import pytest

from manyforge_behavior.runner import (
    ORCHESTRATOR_RUNNER_RESTORATION_GATES,
    OrchestratorRunner,
    OrchestratorRunnerUnavailableError,
)


def test_runner_fails_before_artifact_kernel_or_callback_access() -> None:
    class PoisonCollaborator:
        def __getattribute__(self, name: str) -> object:
            raise AssertionError(f"runner collaborator was accessed: {name}")

    poison = PoisonCollaborator()
    with pytest.raises(
        OrchestratorRunnerUnavailableError,
        match="composable_cell_orchestrator_runner_unavailable",
    ):
        OrchestratorRunner(poison, poison, poison, event_recorder=poison)

    assert ORCHESTRATOR_RUNNER_RESTORATION_GATES == ("I6", "N")
