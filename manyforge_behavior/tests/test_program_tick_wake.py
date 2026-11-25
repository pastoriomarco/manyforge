"""S2 (OD-380 D3): the scheduler waits on the runs' wake set.

A woken run (a sender's answer, a pushed change it watches) is ticked at
once, not at the next period; one being ticked is ticked again right after,
never twice at once; a core call blocked with the GIL released never delays
a tick. Synchronization is by events; each period here is long, so only a
wake can explain a prompt tick.
"""

from __future__ import annotations

from threading import Condition, Event, Thread
import time
from types import SimpleNamespace

import pytest

from manyforge_behavior.program_run_owner import ProgramRunOwner, ProgramRunState
from manyforge_behavior.program_scheduling import ProgramRunTickHost
from manyforge_behavior.run_sender import RunSender

core = pytest.importorskip("manyforge_headless_py")

_WAIT_S = 5.0


class _Owner(ProgramRunOwner):
    """The run owner as the tick host sees it: each tick runs ``tick``."""

    def __init__(self, tick) -> None:  # noqa: D401 - no owner state needed.
        self._tick = tick

    def advance_once(self, program_run_id, *, hold_facts=None):
        self._tick(program_run_id)
        return self.read(program_run_id)

    def read(self, program_run_id):
        return SimpleNamespace(
            capture=SimpleNamespace(program_run_id=program_run_id),
            run_revision=0,
            node_status_revision=0,
            state=ProgramRunState.RUNNING,
        )

    def tick_intents_pending(self, program_run_id) -> bool:
        return False  # No command is ever handed to these ticks.

    def close_run_senders(self) -> None:
        pass

    def fail_tick_intents(self, unavailable) -> None:
        pass


class _Ticks:
    """Each run's tick times; waiters wait for a count, never by sleeping."""

    def __init__(self) -> None:
        self._changed = Condition()
        self.times: dict[int, list[float]] = {}
        self.inside: dict[int, int] = {}
        self.overlapped = False

    def enter(self, run_id: int) -> None:
        with self._changed:
            self.inside[run_id] = self.inside.get(run_id, 0) + 1
            self.overlapped |= self.inside[run_id] > 1
            self.times.setdefault(run_id, []).append(time.monotonic())
            self._changed.notify_all()

    def leave(self, run_id: int) -> None:
        with self._changed:
            self.inside[run_id] -= 1

    def count(self, run_id: int) -> int:
        with self._changed:
            return len(self.times.get(run_id, ()))

    def wait_for(self, run_id: int, count: int, timeout: float = _WAIT_S) -> bool:
        with self._changed:
            return self._changed.wait_for(
                lambda: len(self.times.get(run_id, ())) >= count, timeout
            )


def _host(tick, *, interval: float, wake) -> ProgramRunTickHost:
    return ProgramRunTickHost(
        _Owner(tick), tick_interval_s=interval, maximum_workers=4, wake_set=wake
    )


def test_a_woken_run_is_ticked_at_once_and_alone() -> None:
    wake, ticks = core.WakeSet(), _Ticks()

    def tick(run_id):
        ticks.enter(run_id)
        ticks.leave(run_id)

    host = _host(tick, interval=1.0, wake=wake)
    host.schedule(1)
    host.schedule(2)
    host.start()
    try:
        assert ticks.wait_for(1, 1) and ticks.wait_for(2, 1)  # The first period.
        woken_at = time.monotonic()
        wake.wake(1)
        assert ticks.wait_for(1, 2, timeout=0.5)  # Long before the next period.
        assert ticks.times[1][1] - woken_at < 0.5
        assert ticks.count(2) == 1  # Only the woken run was ticked.
    finally:
        host.shutdown()


def test_a_run_woken_during_its_tick_is_ticked_again_right_after_never_twice_at_once() -> None:
    wake, ticks = core.WakeSet(), _Ticks()
    entered, release = Event(), Event()

    def tick(run_id):
        ticks.enter(run_id)
        if ticks.count(run_id) == 1:
            entered.set()
            assert release.wait(_WAIT_S)
        ticks.leave(run_id)

    host = _host(tick, interval=1.0, wake=wake)
    host.schedule(1)
    host.start()
    try:
        assert entered.wait(_WAIT_S)
        wake.wake(1)
        wake.wake(1)  # Coalesced.
        released_at = time.monotonic()
        release.set()
        assert ticks.wait_for(1, 2, timeout=0.5)
        assert ticks.times[1][1] - released_at < 0.5  # Right after, not at the period.
        assert not ticks.overlapped
    finally:
        host.shutdown()
    assert ticks.count(1) == 2


def test_a_sender_answer_wakes_its_run_without_waiting_for_the_period() -> None:
    wake, ticks = core.WakeSet(), _Ticks()
    gate = Event()
    sender = RunSender(1, program_run_id=1, wake_set=wake)
    requests = []

    def tick(run_id):
        ticks.enter(run_id)
        if not requests:
            requests.append(sender.submit(lambda: gate.wait(_WAIT_S)))
        ticks.leave(run_id)

    host = _host(tick, interval=1.0, wake=wake)
    host.schedule(1)
    host.start()
    try:
        assert ticks.wait_for(1, 1)
        answered_at = time.monotonic()
        gate.set()
        assert ticks.wait_for(1, 2, timeout=0.5)
        assert ticks.times[1][1] - answered_at < 0.5
        assert requests[0].answered
    finally:
        host.shutdown()
        sender.close()


def test_a_blocked_core_call_with_the_gil_released_does_not_delay_ticks() -> None:
    wake, ticks = core.WakeSet(), _Ticks()
    blocker = core.WakeSet()
    sender = RunSender(1, program_run_id=1, wake_set=wake)
    requests = []

    def tick(run_id):
        ticks.enter(run_id)
        if run_id == 1 and not requests:
            # The run's request blocks in the core (GIL released) until woken.
            requests.append(sender.submit(lambda: blocker.wait(time.monotonic() + 30.0)))
        ticks.leave(run_id)

    # A second blocked core call on a thread of its own.
    other = Thread(target=lambda: blocker.wait(time.monotonic() + 30.0), daemon=True)
    other.start()
    host = _host(tick, interval=0.01, wake=wake)
    host.schedule(1)
    host.schedule(2)
    host.start()
    try:
        # 20 periods of both runs while both calls stay blocked.
        assert ticks.wait_for(1, 20) and ticks.wait_for(2, 20)
        assert requests and not requests[0].answered and other.is_alive()
    finally:
        # One wake releases one waiter of the blocker: wake until both ended.
        deadline = time.monotonic() + _WAIT_S
        while (other.is_alive() or not requests[0].answered) and time.monotonic() < deadline:
            blocker.wake(9)
            other.join(0.01)
        host.shutdown()
        sender.close()
