"""Test double of the execution event subscription for pump
fakes written as pages: ``paged_subscription(fake, ...)`` serves the stream
from the fake's ``identity()`` (ready or typed idle; any other reply ends the
subscription) and its page function ``events(after_sequence, limit=...)``,
translated into
``snapshot`` / ``event`` / ``state`` / ``progress`` / ``heartbeat`` messages
the way the host sends them. A fake's failure ends the subscription, as a lost
stream does.

``FollowedEventSource`` is the test double of the core's
``manyforge_headless_py.ExecutionEvents`` over such a fake (or any fake with a
``subscribe_events(after_cursor)`` context manager): one follower thread,
``subscribe()`` returning a subscription with ``next(timeout_s)`` and
``close()``, ``{"type": "disconnected", "code", "message"}`` on a lost
subscription, then a resubscription after the last delivered cursor."""

from __future__ import annotations

from collections import deque
from contextlib import contextmanager
from threading import Condition, Event, Thread
from typing import Any, Iterator

from manyforge_headless_api.execution import COMPONENT_MODEL_STATE_FIELDS

EVENT_STREAM_API_VERSION = "manyforge.execution_event_stream.v4"
from manyforge_headless_api.native import HeadlessApiError

# One re-read of the fake per period, like a heartbeat round.
PERIOD_SECONDS = 0.005
# A heartbeat after this many quiet rounds (well inside the dead-link bound).
HEARTBEAT_ROUNDS = 40


def _message(kind: str, **fields: Any) -> dict[str, Any]:
    return {"api_version": EVENT_STREAM_API_VERSION, "type": kind, **fields}


def _state(page: dict[str, Any]) -> dict[str, Any]:
    return {
        "admission_generation": page["admission_generation"],
        "carried_object_last_losses": [],
        "carried_object_lost_signals": [],
        "execution_owner_ready": page["execution_owner_ready"],
        "interrupt_rule_catalog": page.get("interrupt_rule_catalog"),
        "interrupt_rule_state_baseline": page.get("interrupt_rule_state_baseline"),
        "runtime_faults": page["runtime_faults"],
        # Kernel memory facts of stream v3 (OD-205 (b)), when the fake has them.
        **{
            name: page[name]
            for name in ("operations", "last_stop_event_id", "stop_progress_sequence",
                         "active_tcp_baseline")
            if name in page
        },
        **{name: page[name] for name in COMPONENT_MODEL_STATE_FIELDS if name in page},
    }


def _require_reachable(identity: dict[str, Any]) -> None:
    if identity.get("ok") is True:
        return
    error = identity.get("error") if isinstance(identity.get("error"), dict) else {}
    if error.get("code") != "execution_owner_not_ready":
        raise HeadlessApiError(
            error.get("code") or "execution_owner_unavailable",
            "the fake execution host is unreachable",
        )


class _PagedSubscription:
    def __init__(self, fake: Any, after_cursor: int) -> None:
        self._fake = fake
        self._cursor = after_cursor
        self._subscribed = False
        self._queue: list[dict[str, Any]] = []
        self._state: dict[str, Any] | None = None
        self._progress: list[Any] = []
        self._quiet_rounds = 0

    def _read(self) -> None:
        assert self._subscribed
        while True:
            page = self._fake.events(self._cursor, limit=1)
            state = _state(page)
            if self._state is None:
                self._queue.append(_message("snapshot", cursor=self._cursor,
                                            reason="subscribed", state=state))
            # As the host: a changed state first, then the round's rows.
            elif state != self._state:
                self._queue.append(_message("state", state=state))
            self._state = state
            for event in page["events"]:
                self._queue.append(_message("event", event=event))
            self._cursor = page["next_after_sequence"]
            if page["operation_progress"] != self._progress:
                self._progress = page["operation_progress"]
                self._queue.append(_message("progress",
                                            operation_progress=self._progress))
            if not page["has_more"]:
                return

    def receive(self, stop: Event | None = None) -> dict[str, Any] | None:
        stop = stop or Event()
        while not self._queue:
            if not self._subscribed:
                _require_reachable(self._fake.identity())
                self._subscribed = True
            elif stop.wait(PERIOD_SECONDS):
                return None
            self._read()
            self._quiet_rounds = 0 if self._queue else self._quiet_rounds + 1
            if self._quiet_rounds >= HEARTBEAT_ROUNDS:
                self._quiet_rounds = 0
                self._queue.append(_message("heartbeat", cursor=self._cursor))
        return self._queue.pop(0)


@contextmanager
def paged_subscription(fake: Any, after_cursor: int) -> Iterator[_PagedSubscription]:
    yield _PagedSubscription(fake, after_cursor)


class _Subscription:
    def __init__(self) -> None:
        self._changed = Condition()
        self._queue: deque[dict[str, Any]] = deque()
        self._closed = False

    def push(self, message: dict[str, Any]) -> None:
        with self._changed:
            self._queue.append(message)
            self._changed.notify_all()

    def next(self, timeout_s: float) -> dict[str, Any] | None:
        with self._changed:
            self._changed.wait_for(lambda: self._queue or self._closed, timeout=timeout_s)
            return self._queue.popleft() if self._queue else None

    def close(self) -> None:
        with self._changed:
            self._closed = True
            self._changed.notify_all()


class FollowedEventSource:
    """Mixin: the fake is its own ``ExecutionEvents`` double (see above)."""

    def subscribe(self) -> _Subscription:
        subscription = _Subscription()
        self.__dict__.setdefault("_followed_subscriptions", []).append(subscription)
        return subscription

    def start(self) -> None:
        stop = self.__dict__.setdefault("_followed_stop", Event())
        if self.__dict__.get("_followed_thread") is None:
            thread = Thread(target=self._follow, args=(stop,), daemon=True)
            self.__dict__["_followed_thread"] = thread
            thread.start()

    def close(self) -> None:
        self.__dict__.setdefault("_followed_stop", Event()).set()
        thread = self.__dict__.get("_followed_thread")
        if thread is not None:
            thread.join(timeout=5.0)
        for subscription in self.__dict__.get("_followed_subscriptions", []):
            subscription.close()

    def _deliver(self, message: dict[str, Any]) -> None:
        for subscription in self.__dict__.get("_followed_subscriptions", []):
            subscription.push(message)

    def _follow(self, stop: Event) -> None:
        cursor = 0
        while not stop.is_set():
            try:
                with self.subscribe_events(cursor) as subscription:
                    while True:
                        message = subscription.receive(stop)
                        if message is None:
                            return
                        self._deliver(message)
                        if message["type"] in ("snapshot", "heartbeat"):
                            cursor = message["cursor"]
                        elif message["type"] == "event":
                            cursor = message["event"]["sequence"]
            except Exception as error:  # noqa: BLE001 - the core's follower too
                self._deliver({
                    "type": "disconnected",
                    "code": getattr(error, "code", "execution_owner_unavailable"),
                    "message": str(error),
                })
            if stop.wait(PERIOD_SECONDS):
                return

    def subscribe_events(self, after_cursor: int):
        return paged_subscription(self, after_cursor)


class _FollowedPages(FollowedEventSource):
    def __init__(self, identity: Any, events: Any) -> None:
        self.identity = identity
        self.events = events


def followed_pages(identity: Any, events: Any) -> FollowedEventSource:
    """An ``ExecutionEvents`` double over two page functions."""

    return _FollowedPages(identity, events)
