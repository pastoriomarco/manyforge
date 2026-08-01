"""Control flow decorator nodes for ManyForge behavior tree.

py_trees Decorator.tick() already ticks the child before calling update().
In update(), read self.decorated.status — do NOT call self.decorated.tick() again.
Each cycle/attempt advances once per tree tick.
"""

import py_trees


_MAX_BOUNDED_COUNT = 1_000_000


def _validated_bounded_count(value: int, *, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise TypeError(f"{field} must be an integer")
    if value < 1 or value > _MAX_BOUNDED_COUNT:
        raise ValueError(f"{field} must be in [1, {_MAX_BOUNDED_COUNT}]")
    return value


class RepeatDecorator(py_trees.decorators.Decorator):
    """Repeat a child a bounded ``num_cycles`` times.

    Each tree tick advances at most one cycle: when the child returns SUCCESS,
    the cycle counter increments and RUNNING is returned so the tree ticks again.
    When all cycles complete, SUCCESS is returned.  FAILURE from the child
    propagates immediately.
    """

    def __init__(self, name: str, child: py_trees.behaviour.Behaviour, num_cycles: int):
        super().__init__(name=name, child=child)
        self._num_cycles = _validated_bounded_count(
            num_cycles,
            field="num_cycles",
        )
        self._cycle_count = 0

    def initialise(self):
        self._cycle_count = 0

    def update(self):
        if self.decorated.status == py_trees.common.Status.SUCCESS:
            self._cycle_count += 1
            if self._cycle_count >= self._num_cycles:
                return py_trees.common.Status.SUCCESS
            # More cycles needed — reset child so it re-runs on the next tree tick
            return py_trees.common.Status.RUNNING
        if self.decorated.status == py_trees.common.Status.FAILURE:
            return py_trees.common.Status.FAILURE
        return py_trees.common.Status.RUNNING


class RetryDecorator(py_trees.decorators.Decorator):
    """Retry a child on FAILURE up to a bounded ``num_attempts`` count.

    Each tree tick advances at most one attempt: when the child returns FAILURE,
    the attempt counter increments and RUNNING is returned so the tree ticks again.
    SUCCESS from the child propagates immediately.
    """

    def __init__(self, name: str, child: py_trees.behaviour.Behaviour, num_attempts: int):
        super().__init__(name=name, child=child)
        self._num_attempts = _validated_bounded_count(
            num_attempts,
            field="num_attempts",
        )
        self._attempt_count = 0

    def initialise(self):
        self._attempt_count = 0

    def update(self):
        if self.decorated.status == py_trees.common.Status.SUCCESS:
            return py_trees.common.Status.SUCCESS
        if self.decorated.status == py_trees.common.Status.FAILURE:
            self._attempt_count += 1
            if self._attempt_count >= self._num_attempts:
                return py_trees.common.Status.FAILURE
            # More attempts available — reset child so it re-runs on the next tree tick
            return py_trees.common.Status.RUNNING
        return py_trees.common.Status.RUNNING
