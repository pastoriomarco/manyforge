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

Memory only and bounded: the run builds it with its node count, and a node
keeps at most one request queued. It ends with the run (``close``): what is
still queued is withdrawn, a sent request completes. No thread is kept while
nothing is queued.
"""

from __future__ import annotations

from collections import deque
from collections.abc import Callable
from enum import Enum
from threading import Lock, Thread
from typing import Any


class RunRequestState(Enum):
    QUEUED = "queued"  # Not handed to the transport yet; can be withdrawn.
    SENT = "sent"  # Handed to the transport; its answer is awaited.
    ANSWERED = "answered"  # The call returned its result.
    FAILED = "failed"  # The call raised; ``error`` carries its typed code.
    WITHDRAWN = "withdrawn"  # Never sent.


class RunSenderError(RuntimeError):
    """The sender refused a request: the run ended (closed) or its bound
    was exceeded, which only a defect can cause. Nothing was sent."""


class RunRequest:
    """One submitted request and, once answered, its answer."""

    __slots__ = ("_sender", "_call", "_state", "_result", "_error")

    def __init__(self, sender: "RunSender", call: Callable[[], Any]) -> None:
        self._sender = sender
        self._call: Callable[[], Any] | None = call
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
        already) withdrawn and will never be sent; False once it was sent."""
        return self._sender._withdraw(self)


def _start_thread(name: str, work: Callable[[], None]) -> None:
    Thread(target=work, name=name, daemon=True).start()


class RunSender:
    """The ordered sender of one Program run (see the module text)."""

    def __init__(
        self,
        capacity: int,
        *,
        name: str = "manyforge-program-sender",
        spawn: Callable[[str, Callable[[], None]], None] = _start_thread,
    ) -> None:
        if type(capacity) is not int or capacity < 1:
            raise ValueError("sender capacity must be a positive node count")
        self._capacity = capacity
        self._name = name
        # Starts the thread that empties the queue (tests run it in place).
        self._spawn = spawn
        self._lock = Lock()
        self._queue: deque[RunRequest] = deque()
        self._draining = False
        self._closed = False

    def submit(self, call: Callable[[], Any]) -> RunRequest:
        """Queue one request behind the run's earlier ones; never waits."""
        if not callable(call):
            raise TypeError("a sent request is one callable")
        request = RunRequest(self, call)
        with self._lock:
            if self._closed:
                raise RunSenderError("the Program run has ended; nothing more is sent")
            if len(self._queue) >= self._capacity:
                raise RunSenderError("the run's sender exceeded its node bound")
            self._queue.append(request)
            start = not self._draining
            self._draining = True
        if start:
            self._spawn(self._name, self._drain)
        return request

    def _drain(self) -> None:
        while True:
            with self._lock:
                if not self._queue:
                    self._draining = False
                    return
                request = self._queue.popleft()
                # Sent from here: handed to the transport by the call below.
                request._state = RunRequestState.SENT
                call, request._call = request._call, None
            assert call is not None
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

    def _withdraw(self, request: RunRequest) -> bool:
        with self._lock:
            if request._state is RunRequestState.QUEUED:
                self._queue.remove(request)
                request._state = RunRequestState.WITHDRAWN
                request._call = None
            return request._state is RunRequestState.WITHDRAWN

    def withdraw_queued(self) -> None:
        """Withdraw every request not sent yet (a Halt of the whole run);
        each node sees its own request withdrawn on its next tick."""
        with self._lock:
            self._withdraw_all_locked()

    def close(self) -> None:
        """The run ended: what is queued is withdrawn and nothing more is
        accepted. A sent request completes and keeps its answer."""
        with self._lock:
            self._closed = True
            self._withdraw_all_locked()

    def _withdraw_all_locked(self) -> None:
        for request in self._queue:
            request._state = RunRequestState.WITHDRAWN
            request._call = None
        self._queue.clear()


__all__ = ["RunRequest", "RunRequestState", "RunSender", "RunSenderError"]
