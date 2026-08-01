"""I6/N + I9/I gate for simulator-control behavior nodes."""

from __future__ import annotations

from typing import Final


SIMULATION_NODE_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_simulation_node_unavailable"
)
SIMULATION_NODE_RESTORATION_GATES: Final = ("I6", "N", "I9", "I")


class SimulationNodeUnavailableError(RuntimeError):
    """Raised before executor, simulator, blackboard, or pause state is used."""


class _UnavailableSimulationNode:
    def __init__(self, *_args: object, **_kwargs: object) -> None:
        raise SimulationNodeUnavailableError(
            f"{SIMULATION_NODE_UNAVAILABLE_REASON_CODE}: bound simulator "
            "control returns at I6/N and the Isaac lane at I9/I"
        )


class ResetSimulation(_UnavailableSimulationNode):
    pass


class SetSimulationState(_UnavailableSimulationNode):
    pass


class StepSimulation(_UnavailableSimulationNode):
    pass


class SetEntityState(_UnavailableSimulationNode):
    pass
