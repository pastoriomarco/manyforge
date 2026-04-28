"""Runtime-owned motion transaction helpers."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Literal, Optional

from manyforge_behavior.kernel_client import (
    ExecuteRequest,
    ExecuteResult,
    ExecutionState,
    ExecutionStatus,
    KernelClient,
)


MotionTransactionState = Literal["running", "succeeded", "failed"]


@dataclass
class MotionTransactionUpdate:
    state: MotionTransactionState
    status: ExecutionStatus
    error: Optional[str] = None


class MotionExecutionCoordinator:
    """Owns the runtime protocol for one-motion-at-a-time kernel transactions.

    Authored behavior nodes request a motion and observe the transaction result.
    The coordinator owns the kernel terminal acknowledgement protocol so every
    successful motion is acknowledged against the exact execution_id before the
    behavior tree advances to the next motion.
    """

    def __init__(self, kernel: KernelClient):
        self._kernel = kernel

    @property
    def kernel(self) -> KernelClient:
        return self._kernel

    def execute(self, request: ExecuteRequest) -> ExecuteResult:
        return self._kernel.execute(request)

    def update(self, execution_id: int) -> MotionTransactionUpdate:
        status = self._kernel.get_status()
        if status.state == ExecutionState.COMPLETED:
            if status.execution_id != int(execution_id):
                return MotionTransactionUpdate(
                    state="failed",
                    status=status,
                    error=(
                        "Completed execution id mismatch: "
                        f"current={status.execution_id}, owned={execution_id}"
                    ),
                )
            ack = self._kernel.acknowledge_completed(execution_id)
            if not ack.accepted:
                return MotionTransactionUpdate(
                    state="failed",
                    status=status,
                    error=ack.error or "Completed execution acknowledgement failed",
                )
            return MotionTransactionUpdate(state="succeeded", status=status)

        if status.state in {ExecutionState.FAILED, ExecutionState.STOPPED}:
            if status.execution_id != int(execution_id):
                return MotionTransactionUpdate(
                    state="failed",
                    status=status,
                    error=(
                        "Terminal execution id mismatch: "
                        f"current={status.execution_id}, owned={execution_id}"
                    ),
                )
            return MotionTransactionUpdate(
                state="failed",
                status=status,
                error=f"Motion execution ended in {status.state.value}",
            )

        return MotionTransactionUpdate(state="running", status=status)
