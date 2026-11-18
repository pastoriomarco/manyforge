"""Test double of the execution event subscription for pump
fakes written as pages: ``paged_subscription(fake, ...)`` serves the stream
from the fake's ``identity()`` (ready or typed idle; any other reply ends the
subscription) and its page function ``events(after_sequence, limit=...)``,
translated into
``snapshot`` / ``event`` / ``state`` / ``progress`` / ``heartbeat`` messages
the way the host sends them. A fake's failure ends the subscription, as a lost
stream does."""

from __future__ import annotations

from contextlib import contextmanager
from threading import Event
from typing import Any, Iterator

from manyforge_headless_api.execution import COMPONENT_MODEL_STATE_FIELDS
from manyforge_headless_api.execution_event_stream import EVENT_STREAM_API_VERSION
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
