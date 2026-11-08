"""Cutover gates for motion-scoped nodes and transaction ownership."""

import pytest

from manyforge_behavior.motion_runtime import (
    MotionExecutionCoordinator,
    MotionTransactionOwnerUnavailableError,
)
from manyforge_behavior.nodes.motion_scoped import (
    MotionScopedNodeUnavailableError,
    MoveUntilSignal,
    ProbeSearchMove,
)


@pytest.mark.parametrize("node_type", [MoveUntilSignal, ProbeSearchMove])
def test_motion_scoped_nodes_fail_before_kernel_or_signal_access(node_type: type) -> None:
    class PoisonInput:
        def __getattribute__(self, name: str) -> object:
            raise AssertionError(f"motion input was accessed: {name}")

    poison = PoisonInput()
    with pytest.raises(
        MotionScopedNodeUnavailableError,
        match="composable_cell_motion_scoped_node_unavailable",
    ):
        node_type(poison, kernel=poison, signal=poison)


def test_motion_transaction_fails_before_kernel_access() -> None:
    class PoisonKernel:
        def __getattribute__(self, name: str) -> object:
            raise AssertionError(f"Kernel was accessed: {name}")

    with pytest.raises(
        MotionTransactionOwnerUnavailableError,
        match="composable_cell_motion_transaction_owner_unavailable",
    ):
        MotionExecutionCoordinator(PoisonKernel())
