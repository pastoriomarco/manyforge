"""Gate for simulator-control behavior nodes."""

from __future__ import annotations

from typing import Final


SIMULATION_NODE_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_simulation_node_unavailable"
)


class SimulationNodeUnavailableError(RuntimeError):
    """Raised before executor, simulator, blackboard, or pause state is used."""


class _UnavailableSimulationNode:
    def __init__(self, *_args: object, **_kwargs: object) -> None:
        raise SimulationNodeUnavailableError(
            f"{SIMULATION_NODE_UNAVAILABLE_REASON_CODE}: bound simulator "
            "control is not available"
        )


class ResetSimulation(_UnavailableSimulationNode):
    pass


class SetSimulationState(_UnavailableSimulationNode):
    pass


class StepSimulation(_UnavailableSimulationNode):
    pass


class SetEntityState(_UnavailableSimulationNode):
    pass
