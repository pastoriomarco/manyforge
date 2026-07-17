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
                # The kernel has already committed Completed, but its native
                # terminal event/outcome batch may still be in flight. Keep
                # this transaction alive and retry on the next BT tick so the
                # acknowledgement cannot overtake that causal evidence.
                retryable = ack.details.get("retryable")
                if (
                    ack.reason_code == "terminal_publication_in_progress"
                    and retryable in (True, 1, "1", "true")
                ):
                    return MotionTransactionUpdate(state="running", status=status)
                return MotionTransactionUpdate(
                    state="failed",
                    status=status,
                    error=ack.error or "Completed execution acknowledgement failed",
                )
            return MotionTransactionUpdate(state="succeeded", status=status)

        if status.state in {
            ExecutionState.INTERRUPTED,
            ExecutionState.FAILED,
            ExecutionState.STOPPED,
            ExecutionState.INTERVENTION_REQUIRED,
        }:
            if status.execution_id != int(execution_id):
                return MotionTransactionUpdate(
                    state="failed",
                    status=status,
                    error=(
                        "Terminal execution id mismatch: "
                        f"current={status.execution_id}, owned={execution_id}"
                    ),
                )
            # Pull the kernel's structured failure cause from the
            # execution snapshot, which carries the underlying error
            # string (planner error code, collision detail, joint-limit,
            # time-limit, etc.). Without this we surfaced only the
            # state enum ("failed"/"stopped") and the operator had no
            # way to diagnose what actually went wrong.
            cause: Optional[str] = None
            try:
                snapshot = self._kernel.get_execution_snapshot()
            except Exception:  # noqa: BLE001 — kernel client may not implement snapshots in all configurations
                snapshot = None
            if snapshot is not None and snapshot.error:
                cause = str(snapshot.error).strip() or None
            error_message = (
                cause
                if cause
                else f"Motion execution ended in {status.state.value}"
            )
            return MotionTransactionUpdate(
                state="failed",
                status=status,
                error=error_message,
            )

        return MotionTransactionUpdate(state="running", status=status)
