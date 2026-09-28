"""Per-run operator motion-Stop hold over Kernel-owned Stop truth."""

from __future__ import annotations

from dataclasses import dataclass
from threading import RLock
from typing import Callable, Iterator, Mapping

import py_trees


@dataclass(frozen=True, slots=True)
class OperatorMotionStopSnapshot:
    """Scheduling-only projection of one current Kernel selection revision."""

    selection_revision: int = 0
    held_nodes: frozenset[str] = frozenset()
    stopping_nodes: frozenset[str] = frozenset()
    faulted_nodes: frozenset[str] = frozenset()
    ready_nodes: frozenset[str] = frozenset()


class OperatorMotionStopState:
    """Small process-local view fed only by exact owner observations.

    This is scheduling state, not Stop authority. The Kernel owns acceptance,
    containment, certificates, and recovery; the Program run merely remembers
    the current owner-issued held-node set so authored hold/continue policy
    can affect tree ticking. Completing one task never releases that scope.
    """

    def __init__(
        self,
        node_children: Mapping[str, tuple[str, ...]],
        *,
        read_scope: Callable[[], OperatorMotionStopSnapshot] | None = None,
    ) -> None:
        # The validated Program supplies child-before-parent order. Each edge
        # is retained once; never materialize a descendant set for every node.
        self._node_children = tuple(node_children.items())
        self._node_ids = frozenset(node_children)
        seen: set[str] = set()
        for node_id, children in self._node_children:
            if any(child not in seen for child in children):
                raise ValueError("captured Stop topology must be child-before-parent")
            seen.add(node_id)
        self._read_scope = read_scope
        self._snapshot = OperatorMotionStopSnapshot()
        self._has_snapshot = False
        self._initial_ready_nodes: frozenset[str] | None = None
        self._held_subtrees: frozenset[str] = frozenset()
        self._observation_changed = False
        self._observation_error: str | None = None
        # Tree-worker-only traversal guard. Nested held wrappers share one
        # observation pass; this is neither an owner flag nor a group latch.
        self.draining_results = False
        self._lock = RLock()

    def publish(
        self, snapshot: OperatorMotionStopSnapshot, *, notify_observation: bool = True
    ) -> None:
        if (
            not isinstance(snapshot, OperatorMotionStopSnapshot)
            or type(snapshot.selection_revision) is not int
            or snapshot.selection_revision < 0
            or not isinstance(snapshot.held_nodes, frozenset)
            or not snapshot.held_nodes <= self._node_ids
            or not isinstance(snapshot.ready_nodes, frozenset)
            or not snapshot.ready_nodes <= snapshot.held_nodes
            or not isinstance(snapshot.stopping_nodes, frozenset)
            or not isinstance(snapshot.faulted_nodes, frozenset)
            or not (snapshot.stopping_nodes | snapshot.faulted_nodes) <= snapshot.held_nodes
            or snapshot.stopping_nodes & snapshot.faulted_nodes
            or snapshot.ready_nodes & (snapshot.stopping_nodes | snapshot.faulted_nodes)
        ):
            raise ValueError("operator Stop projection is outside its captured scope")
        with self._lock:
            if self._initial_ready_nodes is None and snapshot.selection_revision == 0:
                self._initial_ready_nodes = snapshot.ready_nodes
            first_snapshot = not self._has_snapshot
            self._has_snapshot = True
            if not first_snapshot:
                if snapshot.selection_revision < self._snapshot.selection_revision:
                    return
                if snapshot.selection_revision == self._snapshot.selection_revision:
                    if snapshot.held_nodes != self._snapshot.held_nodes:
                        raise ValueError(
                            "operator Stop projection changed without a revision"
                        )
                    if snapshot.ready_nodes != self._snapshot.ready_nodes:
                        raise ValueError("operator hold cause changed without a revision")
                    # Within one selection, settlement is monotonic. A crossing
                    # older read cannot put settled work back into Stopping or
                    # erase a fault. Release/reset has a new selection revision.
                    if (
                        not snapshot.stopping_nodes
                        <= self._snapshot.stopping_nodes
                        or not self._snapshot.faulted_nodes
                        <= snapshot.faulted_nodes
                    ):
                        return
                    if (
                        not (snapshot.faulted_nodes - self._snapshot.faulted_nodes)
                        <= self._snapshot.stopping_nodes
                    ):
                        raise ValueError(
                            "settled operator Stop changed its terminal outcome"
                        )
                    if self._observation_error is not None:
                        self._observation_error = None
                        self._observation_changed |= notify_observation
                    if snapshot == self._snapshot:
                        return
            if snapshot.held_nodes != self._snapshot.held_nodes:
                held_subtrees: set[str] = set()
                for node_id, children in self._node_children:
                    if node_id in snapshot.held_nodes and all(
                        child in held_subtrees for child in children
                    ):
                        held_subtrees.add(node_id)
                self._held_subtrees = frozenset(held_subtrees)
            self._snapshot = snapshot
            self._observation_error = None
            # Commands advance the Run revision themselves. Do not count the
            # same publication again, or erase a crossing real observation.
            self._observation_changed |= notify_observation

    def note_task_observation_change(self) -> None:
        """Coalesce a held task's terminal change into the existing Run wake."""
        with self._lock:
            self._observation_changed = True

    def take_observation_change(self) -> bool:
        with self._lock:
            changed, self._observation_changed = self._observation_changed, False
            return changed

    def refresh(self) -> None:
        if self._read_scope is None:
            raise RuntimeError("operator Stop has no captured owner projection")
        # No owner I/O under the scheduling-view lock. A crossing newer
        # selection publication wins by its existing Kernel revision.
        basis = self.snapshot()
        try:
            self.publish(self._read_scope())
        except Exception:
            with self._lock:
                # A late failed read cannot poison a newer accepted Stop or
                # settled view, which may no longer need another owner read.
                if self._snapshot == basis and self._observation_error is None:
                    self._observation_error = "program_selection_observation_unavailable"
                    self._observation_changed = True
            raise

    def observation_error(self) -> str | None:
        with self._lock:
            return self._observation_error

    def snapshot(self) -> OperatorMotionStopSnapshot:
        with self._lock:
            return self._snapshot

    def initial_ready_nodes(self) -> frozenset[str]:
        with self._lock:
            return self._initial_ready_nodes or frozenset()

    def holds_subtree(self, node_instance_id: str) -> bool:
        with self._lock:
            return node_instance_id in self._held_subtrees


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
        drain_children: bool = False,
    ) -> None:
        super().__init__(name=f"operator-stop:{node_instance_id}", child=child)
        self._node_instance_id = node_instance_id
        self._stop_state = stop_state
        self._terminal_forwarded = False
        self._drain_children = drain_children

    def _observe_active_tasks(self) -> bool:
        changed = False
        for node in self.decorated.iterate():
            observe = getattr(node, "observe_while_operator_stop_held", None)
            if callable(observe):
                before = node.status
                observe()
                changed |= node.status is not before
            if isinstance(node, OperatorMotionStopPolicy):
                # Selected recovery may have observed a terminal result before
                # this tree tick. It still needs forwarding exactly once.
                changed |= not node._terminal_forwarded and node.decorated.status in {
                    py_trees.common.Status.SUCCESS,
                    py_trees.common.Status.FAILURE,
                }
        return changed

    @property
    def presented_status(self) -> py_trees.common.Status:
        # A new iteration can be held without repainting the last completed
        # action as Running. The scope projection separately reports the hold.
        return getattr(self.decorated, "presented_status", self.decorated.status)

    @property
    def presented_held(self) -> bool:
        """Waiting for Run/Resume: held here before the node started, or
        held inside. A composite already running its children is not held;
        its children say which ones wait."""
        if getattr(self.decorated, "presented_held", False):
            return True
        return (
            self.status is py_trees.common.Status.RUNNING
            and self.decorated.status is py_trees.common.Status.INVALID
            and self._stop_state.holds_subtree(self._node_instance_id)
        )

    def tick(self) -> Iterator[py_trees.behaviour.Behaviour]:
        if not self._stop_state.holds_subtree(self._node_instance_id):
            yield from super().tick()
            self._terminal_forwarded = self.status in {
                py_trees.common.Status.SUCCESS,
                py_trees.common.Status.FAILURE,
            }
            return
        changed = False if self._stop_state.draining_results else self._observe_active_tasks()
        if self.decorated.status in {
            py_trees.common.Status.SUCCESS,
            py_trees.common.Status.FAILURE,
        }:
            if not self._terminal_forwarded:
                self._terminal_forwarded = True
                self.status = self.decorated.status
                yield self
                return
        elif self._drain_children and (changed or self._stop_state.draining_results):
            # Only local result propagation. Every selected child still has
            # its own pre-tick hold; no new action or iteration can start.
            already_draining = self._stop_state.draining_results
            self._stop_state.draining_results = True
            try:
                drain = getattr(self.decorated, "tick_held_results", None)
                if callable(drain):
                    yield from drain()
                    self.status = self.decorated.status
                    yield self
                else:
                    yield from super().tick()
                self._terminal_forwarded = self.status in {
                    py_trees.common.Status.SUCCESS,
                    py_trees.common.Status.FAILURE,
                }
            finally:
                self._stop_state.draining_results = already_draining
            return
        self.status = py_trees.common.Status.RUNNING
        yield self

    def update(self) -> py_trees.common.Status:
        return self.decorated.status


__all__ = [
    "OperatorMotionStopPolicy",
    "OperatorMotionStopSnapshot",
    "OperatorMotionStopState",
]
