"""Control flow decorator nodes for ManyForge behavior tree.

py_trees Decorator.tick() already ticks the child before calling update().
In update(), read self.decorated.status — do NOT call self.decorated.tick() again.
Each cycle/attempt advances once per tree tick.
"""

import sys
import py_trees


class RepeatDecorator(py_trees.decorators.Decorator):
    """Repeat child num_cycles times. -1 = forever (use sys.maxsize internally).

    Each tree tick advances at most one cycle: when the child returns SUCCESS,
    the cycle counter increments and RUNNING is returned so the tree ticks again.
    When all cycles complete, SUCCESS is returned.  FAILURE from the child
    propagates immediately.
    """

    def __init__(self, name: str, child: py_trees.behaviour.Behaviour, num_cycles: int = -1):
        super().__init__(name=name, child=child)
        self._num_cycles = sys.maxsize if num_cycles == -1 else num_cycles
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
    """Retry child on FAILURE up to num_attempts times. -1 = forever.

    Each tree tick advances at most one attempt: when the child returns FAILURE,
    the attempt counter increments and RUNNING is returned so the tree ticks again.
    SUCCESS from the child propagates immediately.
    """

    def __init__(self, name: str, child: py_trees.behaviour.Behaviour, num_attempts: int = -1):
        super().__init__(name=name, child=child)
        self._num_attempts = sys.maxsize if num_attempts == -1 else num_attempts
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
