"""Typed I6/N gate for Program-Binding execution."""

from __future__ import annotations

from typing import Final


ORCHESTRATOR_RUNNER_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_orchestrator_runner_unavailable"
)
ORCHESTRATOR_RUNNER_RESTORATION_GATES: Final = ("I6", "N")


class OrchestratorRunnerUnavailableError(RuntimeError):
    """Raised before artifacts, bindings, Kernel, scene, or callbacks are used."""


class OrchestratorRunner:
    """Unavailable old singleton runner; no Python factory fallback remains."""

    def __init__(self, *_args: object, **_kwargs: object) -> None:
        raise OrchestratorRunnerUnavailableError(
            f"{ORCHESTRATOR_RUNNER_UNAVAILABLE_REASON_CODE}: native "
            "Program-Binding execution returns at I6/N"
        )


__all__ = [
    "ORCHESTRATOR_RUNNER_RESTORATION_GATES",
    "ORCHESTRATOR_RUNNER_UNAVAILABLE_REASON_CODE",
    "OrchestratorRunner",
    "OrchestratorRunnerUnavailableError",
]
