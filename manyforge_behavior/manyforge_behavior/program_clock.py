"""The run-scoped Program clock that waits and the timer read time from.

One clock belongs to one executable Program run. It is monotonic and stands
still while the run is frozen: its owner freezes it whenever the run's tree
is not scheduled to tick (a Halt, or a node-failure pause), and resumes it
when ticking resumes. An operator Stop does not freeze it; whether a held
node's attempt counts that held time is the node's own choice
(:class:`AttemptTime`).
"""

from __future__ import annotations

from collections.abc import Callable
from threading import Lock
import time

from manyforge_behavior.nodes.stop_policy import OperatorMotionStopState


class ProgramClock:
    """Monotonic Program time of one run, excluding the time it is frozen."""

    def __init__(self, source: Callable[[], float] = time.monotonic) -> None:
        if not callable(source):
            raise TypeError("Program clock source must be callable")
        self._source = source
        self._lock = Lock()
        self._frozen_at: float | None = None
        self._frozen_total = 0.0

    def now(self) -> float:
        """Current Program time. Only differences are meaningful."""

        with self._lock:
            reading = self._source() if self._frozen_at is None else self._frozen_at
            return reading - self._frozen_total

    @property
    def frozen(self) -> bool:
        with self._lock:
            return self._frozen_at is not None

    def freeze(self) -> None:
        """Stop Program time; idempotent."""

        with self._lock:
            if self._frozen_at is None:
                self._frozen_at = self._source()

    def resume(self) -> None:
        """Let Program time advance again; idempotent."""

        with self._lock:
            if self._frozen_at is not None:
                self._frozen_total += self._source() - self._frozen_at
                self._frozen_at = None

    def set_running(self, running: bool) -> None:
        if running:
            self.resume()
        else:
            self.freeze()


class AttemptTime:
    """Elapsed Program time of one node attempt.

    Program time already leaves out the time the run is frozen (Halt). When
    the attempt excludes held time, the time an operator Stop holds the node
    is left out as well: the time its hold scope (the nearest self-or-ancestor
    whose effective stop policy is `hold`) is held, as the run's Stop state
    measures it on this same Program clock, so no interval is subtracted
    twice. A node without a hold scope is never held and always counts.
    """

    def __init__(
        self,
        clock: ProgramClock,
        *,
        operator_stop_state: OperatorMotionStopState | None = None,
        operator_stop_hold_scope: str | None = None,
    ) -> None:
        if not isinstance(clock, ProgramClock):
            raise TypeError("attempt time requires the run's Program clock")
        if (operator_stop_state is None) != (operator_stop_hold_scope is None):
            raise ValueError("a held-time scope needs its operator Stop state")
        self._clock = clock
        self._stop_state = operator_stop_state
        self._hold_scope = operator_stop_hold_scope
        self._started: float | None = None
        # Held seconds at attempt start; set only when held time is excluded.
        self._held_basis: float | None = None

    def start(self, *, exclude_held: bool) -> None:
        self._started = self._clock.now()
        self._held_basis = (
            self._stop_state.held_seconds(self._hold_scope)
            if exclude_held
            and self._stop_state is not None
            and self._hold_scope is not None
            else None
        )

    def elapsed(self) -> float:
        if self._started is None:
            raise RuntimeError("attempt time was not started")
        elapsed = self._clock.now() - self._started
        if self._held_basis is not None:
            assert self._stop_state is not None  # Set with the basis.
            assert self._hold_scope is not None
            elapsed -= self._stop_state.held_seconds(self._hold_scope) - self._held_basis
        return elapsed


__all__ = ["AttemptTime", "ProgramClock"]
