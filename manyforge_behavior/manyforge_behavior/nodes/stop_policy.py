"""Operator motion-stop policy wrappers for behavior-tree subtrees."""

from __future__ import annotations

import py_trees


class OperatorMotionStopPolicy(py_trees.decorators.Decorator):
    """Hold a subtree during operator Motion Stop without terminating it.

    `CycleManager.stop()` uses a separate `motion_stop_active` blackboard key
    for the cooperative motion-stop path. This decorator implements the
    conservative `hold` policy: while motion stop is active, the wrapped subtree
    simply reports RUNNING and is not ticked, so it performs no side effects and
    keeps any in-flight child state intact for resume.
    """

    def __init__(
        self,
        *,
        name: str,
        child: py_trees.behaviour.Behaviour,
        policy: str,
    ) -> None:
        if policy not in {"hold", "continue"}:
            raise ValueError("operator motion stop policy must be hold or continue")
        super().__init__(name=name, child=child)
        self._policy = policy
        self._manyforge_internal_status_node = True
        self._blackboard = py_trees.blackboard.Client(
            name=f"{name}_operator_stop_policy"
        )
        self._blackboard.register_key(
            key="motion_stop_active",
            access=py_trees.common.Access.READ,
        )

    def _motion_stop_active(self) -> bool:
        try:
            return bool(self._blackboard.get("motion_stop_active"))
        except (KeyError, AttributeError):
            return False

    def __getattr__(self, name: str):
        """Delegate legacy/test introspection to the wrapped behavior.

        The policy node is an implementation wrapper; callers that previously
        inspected attributes on the authored root behavior should keep seeing
        the wrapped node's fields.
        """
        decorated = self.__dict__.get("decorated")
        if decorated is not None:
            return getattr(decorated, name)
        raise AttributeError(name)

    def update(self) -> py_trees.common.Status:
        return self.decorated.status

    def tick(self):
        if self._policy == "hold" and self._motion_stop_active():
            if self.status != py_trees.common.Status.RUNNING:
                self.initialise()
            self.feedback_message = "held by operator motion stop"
            self.status = py_trees.common.Status.RUNNING
            yield self
            return
        yield from super().tick()
