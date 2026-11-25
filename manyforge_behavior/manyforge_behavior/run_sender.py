"""One Program run's ordered sender: slow requests leave the tick.

A node whose request to another process is slow hands it to its run's
sender and returns RUNNING. The sender makes the requests one at a time, in
the order they were submitted, on a thread of its own, and keeps each answer
as state the node reads on a later tick. The tick never waits on it: submit,
withdraw and every read take one short memory-only lock, never held across a
request.

A request is *queued* until the sender hands it to the transport (starts the
call); from that moment it is *sent*. A queued request can be withdrawn and is
then never sent: a Stop or Halt does this for an atomic action that has not
left yet. A sent request is never withdrawn and has no time limit of its own;
its wait ends with its answer: a result, or the error the call raised (with
its typed code).

A request submitted as not withdrawable is never withdrawn: a Halt, a
Program Stop and ``close`` leave it queued and it is still sent. Its users:
a Program value publication's frames (a committed value is state, not a
command, and the host expects every frame of its sequence); the settle steps
base made inside the tick that saw a task end (the acknowledgement, the
close, a settling task's read, the pneumatic watch release, the gripper's
grasp read); the Kernel scope reads (a node's admission hold and Stop scope,
the run's selection, Halting and Stop-settlement scope, the run's tick
reads); and the Mock simulation's END and pause resends. It is also accepted
after ``close``: the reads and acknowledgements a run makes after its tree
ended (its Stop's settlement, its Reset containment, a stopped node's real
result) leave the tick too, each at most one at a time per run and only
while that end is settling.

A motion or gripper start is submitted with the target whose Kernel
operation it opens. The Program's own Stop nodes (`kernel.stop_operation`,
`kernel.cell_stop`) are sent inside the tick, never on the sender; first each
withdraws the queued starts it covers (``stop_starts``: its target, or every
target for the cell) and, while such a start is sent and not answered,
waits for that admission's answer, as base sent the Stop after the start
inside the same tick. Nothing else is withdrawn or waited for.

A request is made in place (at once on the submitting thread, ahead of
anything queued, answered before ``submit`` returns, also after ``close``)
when it is submitted from inside a command the run's tick applies with
``apply_in_place`` (Resume, Run, Retry, Continue, the manual hand-over,
Execute after Halt: base made their owner calls inside the tick), or on the
sender's own thread from inside the request it is making.

Memory only and bounded: the run builds it with its node count, and a node
keeps at most one request queued; a value publication source keeps at most
one not-withdrawable request queued, outside that bound. It ends with the run
(``close``): what is still queued is withdrawn (except not-withdrawable
requests, which are still sent), a sent request completes, and only
not-withdrawable requests are accepted. No thread is kept while nothing is
queued.

With the runs' wake set (S2, D3) each recorded answer wakes the run's tick
at once (``WakeSet.wake``, memory only), and the run's nodes register the
operations and planning jobs whose pushed changes wake it
(``watch_operation``, ``watch_planning_job``); ``close`` ends the run's
watches. A node holds at most its attempt's operation and one it still
drains (or one planning job and one it still follows), so the run keeps the
latest two watches per node; an older one ends (its operation is settled,
or its changes are seen at the run's period). The wake set has one leaf
lock; nothing here waits on it.
"""

from __future__ import annotations

from collections import deque
from collections.abc import Callable
from enum import Enum
from threading import Lock, Thread, get_ident
from typing import Any


class RunRequestState(Enum):
    QUEUED = "queued"  # Not handed to the transport yet; can be withdrawn.
    SENT = "sent"  # Handed to the transport; its answer is awaited.
    ANSWERED = "answered"  # The call returned its result.
    FAILED = "failed"  # The call raised; ``error`` carries its typed code.
    WITHDRAWN = "withdrawn"  # Never sent.


class RunSenderError(RuntimeError):
    """The sender refused a request: a withdrawable one after the run ended
    (closed), or one over its bound, which only a defect can cause. Nothing
    was sent."""


class RunRequest:
    """One submitted request and, once answered, its answer."""

    __slots__ = (
        "_sender", "_call", "_withdrawable", "_start_target", "_stopped", "_state", "_result",
        "_error",
    )

    def __init__(
        self,
        sender: "RunSender",
        call: Callable[[], Any],
        withdrawable: bool,
        start_target: Any = None,
    ) -> None:
        self._sender = sender
        self._call: Callable[[], Any] | None = call
        self._withdrawable = withdrawable
        # A motion or gripper start: the target whose Kernel operation it
        # opens (None: not such a start). A Program Stop withdraws it.
        self._start_target = start_target
        # Withdrawn by a Program Stop (not by a Halt or the run's end).
        self._stopped = False
        self._state = RunRequestState.QUEUED
        self._result: Any = None
        self._error: Exception | None = None

    @property
    def state(self) -> RunRequestState:
        with self._sender._lock:
            return self._state

    @property
    def sent(self) -> bool:
        """Handed to the transport (whether answered yet or not)."""
        return self.state in {
            RunRequestState.SENT,
            RunRequestState.ANSWERED,
            RunRequestState.FAILED,
        }

    @property
    def answered(self) -> bool:
        return self.state in {RunRequestState.ANSWERED, RunRequestState.FAILED}

    @property
    def withdrawn(self) -> bool:
        return self.state is RunRequestState.WITHDRAWN

    @property
    def withdrawn_by_program_stop(self) -> bool:
        """Withdrawn, never sent, because a Program Stop was sent
        (`RunSender.stop_starts`)."""
        with self._sender._lock:
            return self._stopped

    @property
    def error(self) -> Exception | None:
        """What the call raised (FAILED), else None."""
        with self._sender._lock:
            return self._error

    @property
    def failure_code(self) -> str | None:
        """The typed code of what the call raised, when it carries one."""
        code = getattr(self.error, "code", None)
        return code if isinstance(code, str) and code else None

    def result(self) -> Any:
        """The call's result; what it raised is raised here."""
        with self._sender._lock:
            state, result, error = self._state, self._result, self._error
        if state is RunRequestState.FAILED:
            assert error is not None
            raise error
        if state is not RunRequestState.ANSWERED:
            raise RunSenderError(f"the request is {state.value}, not answered")
        return result

    def withdraw(self) -> bool:
        """Withdraw a request that is not sent yet. True when it is (now or
        already) withdrawn and will never be sent; False once it was sent or
        when it is not withdrawable."""
        return self._sender._withdraw(self)


def _covers(target: Any, request: RunRequest) -> bool:
    """A Program Stop on ``target`` (None: the cell) covers this start."""
    return target is None or request._start_target == target


def _start_thread(name: str, work: Callable[[], None]) -> None:
    Thread(target=work, name=name, daemon=True).start()


class RunSender:
    """The ordered sender of one Program run (see the module text)."""

    def __init__(
        self,
        capacity: int,
        *,
        program_run_id: int | None = None,
        wake_set: Any = None,
        name: str = "manyforge-program-sender",
        spawn: Callable[[str, Callable[[], None]], None] = _start_thread,
    ) -> None:
        if type(capacity) is not int or capacity < 1:
            raise ValueError("sender capacity must be a positive node count")
        if wake_set is not None and (
            type(program_run_id) is not int or program_run_id < 1
        ):
            raise ValueError("a sender that wakes its run needs the run number")
        self._capacity = capacity
        # The runs' wake set (``manyforge_headless_py.WakeSet``) and this
        # run's number; None: nothing is woken (the run's period only).
        self._wake_set = wake_set
        self._program_run_id = program_run_id
        # The run's watches, oldest first: ("operation", id) / ("job", id).
        self._watches: dict[tuple[str, int | str], None] = {}
        self._name = name
        # Starts the thread that empties the queue (tests run it in place).
        self._spawn = spawn
        self._lock = Lock()
        self._queue: deque[RunRequest] = deque()
        # Queued withdrawable requests (the node bound counts only these).
        self._queued_withdrawable = 0
        self._draining = False
        # The thread making a request now and that request (None between
        # requests).
        self._sending_thread: int | None = None
        self._sending: RunRequest | None = None
        # The run's tick while it applies a command in place (`apply_in_place`).
        self._applying_thread: int | None = None
        self._closed = False

    def submit(
        self, call: Callable[[], Any], *, withdrawable: bool = True, start_target: Any = None
    ) -> RunRequest:
        """Queue one request behind the run's earlier ones; never waits.
        A not ``withdrawable`` request is never withdrawn (see the module text).
        ``start_target`` marks a motion or gripper start with the target whose
        Kernel operation it opens (`stop_starts`).

        Submitted from inside a command the run's tick applies in place
        (`apply_in_place`), or on the sender's own thread from inside the
        request it is making, the request is made in place at once, ahead of
        anything queued, and answered before this returns, as base made it
        inside the command. Otherwise after ``close`` only a not
        ``withdrawable`` request is accepted."""
        if not callable(call):
            raise TypeError("a sent request is one callable")
        if start_target is not None and not withdrawable:
            raise ValueError("a motion or gripper start is withdrawable")
        request = RunRequest(self, call, withdrawable, start_target)
        with self._lock:
            in_place = get_ident() in (self._sending_thread, self._applying_thread)
            if self._closed and withdrawable and not in_place:
                raise RunSenderError("the Program run has ended; nothing more is sent")
            if in_place:
                request._state = RunRequestState.SENT
                request._call = None
            else:
                if withdrawable:
                    if self._queued_withdrawable >= self._capacity:
                        raise RunSenderError("the run's sender exceeded its node bound")
                    self._queued_withdrawable += 1
                self._queue.append(request)
                start = not self._draining
                self._draining = True
        if in_place:
            self._send(request, call)
        elif start:
            self._spawn(self._name, self._drain)
        return request

    def apply_in_place(self, apply: Callable[[], Any]) -> Any:
        """Apply one command on this thread, the run's tick (Resume, Run,
        Retry, Continue, the manual hand-over, Execute after Halt; OD-146):
        every request it submits is made in place (see ``submit``), as base
        made the command's owner calls inside the tick. Its result is
        returned; what it raised is raised."""
        ident = get_ident()
        with self._lock:
            previous, self._applying_thread = self._applying_thread, ident
        try:
            return apply()
        finally:
            with self._lock:
                self._applying_thread = previous

    def _drain(self) -> None:
        while True:
            with self._lock:
                if not self._queue:
                    self._draining = False
                    return
                request = self._queue.popleft()
                if request._withdrawable:
                    self._queued_withdrawable -= 1
                # Sent from here: handed to the transport by the call below.
                request._state = RunRequestState.SENT
                call, request._call = request._call, None
                self._sending_thread = get_ident()
                self._sending = request
            assert call is not None
            self._send(request, call)
            with self._lock:
                self._sending_thread = None
                self._sending = None

    def _send(self, request: RunRequest, call: Callable[[], Any]) -> None:
        """Make one sent request and keep its answer."""
        try:
            result = call()
        except Exception as error:  # The answer is what the call raised.
            with self._lock:
                request._error = error
                request._state = RunRequestState.FAILED
        else:
            with self._lock:
                request._result = result
                request._state = RunRequestState.ANSWERED
        # The run's tick reads the answer at once (S2), not at its period.
        if self._wake_set is not None:
            self._wake_set.wake(self._program_run_id)

    def _withdraw(self, request: RunRequest) -> bool:
        with self._lock:
            if request._state is RunRequestState.QUEUED and request._withdrawable:
                self._queue.remove(request)
                self._queued_withdrawable -= 1
                request._state = RunRequestState.WITHDRAWN
                request._call = None
            return request._state is RunRequestState.WITHDRAWN

    def watch_operation(self, operation_id: int) -> None:
        """A pushed change of this Kernel operation wakes the run's tick.
        Memory only; nothing once the run ended (its watches are gone)."""

        self._watch(("operation", operation_id))

    def watch_planning_job(self, job_id: str) -> None:
        """A pushed change of this planning job wakes the run's tick."""

        self._watch(("job", job_id))

    def _watch(self, key: tuple[str, int | str]) -> None:
        with self._lock:
            wake = self._wake_set
            if wake is None or self._closed:
                return
            run_id = self._program_run_id
            kind, identity = key
            if key in self._watches:
                del self._watches[key]  # Watched already: now the latest.
            elif kind == "operation":
                wake.watch_operation(run_id, identity)
            else:
                wake.watch_planning_job(run_id, identity)
            self._watches[key] = None
            if len(self._watches) > 2 * self._capacity:
                (old_kind, old_identity) = next(iter(self._watches))
                del self._watches[(old_kind, old_identity)]
                if old_kind == "operation":
                    wake.unwatch_operation(run_id, old_identity)
                else:
                    wake.unwatch_planning_job(run_id, old_identity)

    def stop_starts(self, target: Any = None) -> bool:
        """A Program Stop is to be sent: withdraw every queued motion and
        gripper start on ``target`` (every target when None); each node sees
        its start withdrawn by the Stop on its next tick, and nothing else
        is touched. True while such a start is sent and not answered: the
        Stop then waits for that admission's answer (which wakes the run),
        as base sent it after the start inside the same tick."""
        with self._lock:
            kept: deque[RunRequest] = deque()
            for request in self._queue:
                if request._start_target is None or not _covers(target, request):
                    kept.append(request)
                    continue
                request._state = RunRequestState.WITHDRAWN
                request._stopped = True
                request._call = None
                self._queued_withdrawable -= 1
            self._queue = kept
            sending = self._sending
            return (
                sending is not None
                and sending._start_target is not None
                and sending._state is RunRequestState.SENT
                and _covers(target, sending)
            )

    def withdraw_queued(self) -> None:
        """Withdraw every request not sent yet (a Halt of the whole run);
        each node sees its own request withdrawn on its next tick."""
        with self._lock:
            self._withdraw_all_locked()

    def close(self) -> None:
        """The run ended: what is queued is withdrawn (a not-withdrawable
        request is still sent) and only not-withdrawable requests are
        accepted. A sent request completes and keeps its answer; it still
        wakes the run, whose ticks settle its end."""
        with self._lock:
            self._closed = True
            self._withdraw_all_locked()
            if self._wake_set is not None:
                self._watches.clear()
                self._wake_set.unwatch_run(self._program_run_id)

    def _withdraw_all_locked(self) -> None:
        kept: deque[RunRequest] = deque()
        for request in self._queue:
            if not request._withdrawable:
                kept.append(request)
                continue
            request._state = RunRequestState.WITHDRAWN
            request._call = None
        self._queue = kept
        self._queued_withdrawable = 0


__all__ = ["RunRequest", "RunRequestState", "RunSender", "RunSenderError"]
