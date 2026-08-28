"""Per-run operator motion-Stop hold over Kernel-owned Stop truth."""

from __future__ import annotations

from dataclasses import dataclass
from threading import RLock
from typing import Any, Iterator

import py_trees


@dataclass(frozen=True, slots=True)
class OperatorMotionStopSnapshot:
    """The exact Stop identity currently observed by one Program run."""

    stop_id: str | None = None
    stop_generation: int = 0
    held: bool = False


class OperatorMotionStopState:
    """Small process-local view fed only by exact owner observations.

    This is scheduling state, not Stop authority. The Kernel owns acceptance,
    containment, certificates, and recovery; the Program run merely remembers
    which exact owner-issued Stop it observed so authored hold/continue policy
    can affect tree ticking.
    """

    def __init__(self) -> None:
        self._snapshot = OperatorMotionStopSnapshot()
        self._held_tasks: dict[str, tuple[str, int]] = {}
        self._lock = RLock()

    def hold(self, task_id: str, metadata: dict[str, Any] | Any) -> bool:
        if not isinstance(task_id, str) or not task_id:
            return False
        if not isinstance(metadata, dict):
            return False
        stop_id = metadata.get("active_stop_id")
        generation = metadata.get("active_stop_generation")
        if (
            not isinstance(stop_id, str)
            or not stop_id
            or type(generation) is not int
            or generation < 1
        ):
            return False
        with self._lock:
            self._held_tasks[task_id] = (stop_id, generation)
            newest_stop_id, newest_generation = max(
                self._held_tasks.values(), key=lambda row: row[1]
            )
            self._snapshot = OperatorMotionStopSnapshot(
                stop_id=newest_stop_id,
                stop_generation=newest_generation,
                held=True,
            )
        return True

    def snapshot(self) -> OperatorMotionStopSnapshot:
        with self._lock:
            return self._snapshot

    def resolve(self, task_id: str) -> None:
        with self._lock:
            self._held_tasks.pop(task_id, None)
            if not self._held_tasks:
                self._snapshot = OperatorMotionStopSnapshot()
                return
            newest_stop_id, newest_generation = max(
                self._held_tasks.values(), key=lambda row: row[1]
            )
            self._snapshot = OperatorMotionStopSnapshot(
                stop_id=newest_stop_id,
                stop_generation=newest_generation,
                held=True,
            )


class OperatorMotionStopPolicy(py_trees.decorators.Decorator):
    """Skip a held subtree while exact active owner tasks keep settling.

    The custom tick boundary is intentional: a normal decorator updates only
    after ticking its child, which would permit one effectful tick while held.
    """

    def __init__(
        self,
        *,
        child: py_trees.behaviour.Behaviour,
        node_instance_id: str,
        stop_state: OperatorMotionStopState,
    ) -> None:
        super().__init__(name=f"operator-stop:{node_instance_id}", child=child)
        self._stop_state = stop_state

    def _observe_active_tasks(self) -> None:
        for node in self.decorated.iterate():
            observe = getattr(node, "observe_while_operator_stop_held", None)
            if callable(observe):
                observe()

    def tick(self) -> Iterator[py_trees.behaviour.Behaviour]:
        if not self._stop_state.snapshot().held:
            yield from super().tick()
            return
        self._observe_active_tasks()
        self.status = py_trees.common.Status.RUNNING
        yield self

    def update(self) -> py_trees.common.Status:
        return self.decorated.status


__all__ = [
    "OperatorMotionStopPolicy",
    "OperatorMotionStopSnapshot",
    "OperatorMotionStopState",
]
