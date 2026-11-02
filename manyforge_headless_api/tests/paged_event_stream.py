"""Test double of the execution event subscription (contract §8.1) for pump
fakes written as pages: ``paged_subscription(fake, ...)`` serves the stream
from the fake's ``identity()`` (the owner, ready or typed idle) and its page
function ``events(owner_epoch, after_sequence, limit=...)``, translated into
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


def _message(kind: str, owner_epoch: str, **fields: Any) -> dict[str, Any]:
    return {"api_version": EVENT_STREAM_API_VERSION, "owner_epoch": owner_epoch,
            "type": kind, **fields}


def _state(page: dict[str, Any]) -> dict[str, Any]:
    return {
        "admission_generation": page["admission_generation"],
        "carried_object_last_losses": [],
        "carried_object_lost_signals": [],
        "execution_owner_ready": page["execution_owner_ready"],
        "interrupt_rule_catalog": page.get("interrupt_rule_catalog"),
        "interrupt_rule_state_baseline": page.get("interrupt_rule_state_baseline"),
        "runtime_faults": page["runtime_faults"],
        **{name: page[name] for name in COMPONENT_MODEL_STATE_FIELDS if name in page},
    }


def _owner_epoch(identity: dict[str, Any]) -> str:
    if identity.get("ok") is True:
        return identity["identity"]["owner_epoch"]
    error = identity.get("error") if isinstance(identity.get("error"), dict) else {}
    details = error.get("details") if isinstance(error.get("details"), dict) else {}
    owner_epoch = details.get("owner_epoch")
    if error.get("code") != "execution_owner_not_ready" or not isinstance(owner_epoch, str):
        raise HeadlessApiError(
            error.get("code") or "execution_owner_unavailable",
            "the fake execution owner names no owner epoch",
        )
    return owner_epoch


class _PagedSubscription:
    def __init__(self, fake: Any, after_cursor: int) -> None:
        self._fake = fake
        self._cursor = after_cursor
        self._owner_epoch: str | None = None
        self._queue: list[dict[str, Any]] = []
        self._state: dict[str, Any] | None = None
        self._progress: list[Any] = []
        self._quiet_rounds = 0

    def _read(self) -> None:
        assert self._owner_epoch is not None
        while True:
            page = self._fake.events(self._owner_epoch, self._cursor, limit=1)
            state = _state(page)
            if self._state is None:
                self._queue.append(_message("snapshot", self._owner_epoch, cursor=self._cursor,
                                            reason="subscribed", state=state))
            for event in page["events"]:
                self._queue.append(_message("event", self._owner_epoch, event=event))
            self._cursor = page["next_after_sequence"]
            if self._state is not None and state != self._state:
                self._queue.append(_message("state", self._owner_epoch, state=state))
            self._state = state
            if page["operation_progress"] != self._progress:
                self._progress = page["operation_progress"]
                self._queue.append(_message("progress", self._owner_epoch,
                                            operation_progress=self._progress))
            if not page["has_more"]:
                return

    def receive(self, stop: Event | None = None) -> dict[str, Any] | None:
        stop = stop or Event()
        while not self._queue:
            if self._owner_epoch is None:
                self._owner_epoch = _owner_epoch(self._fake.identity())
            elif stop.wait(PERIOD_SECONDS):
                return None
            self._read()
            self._quiet_rounds = 0 if self._queue else self._quiet_rounds + 1
            if self._quiet_rounds >= HEARTBEAT_ROUNDS:
                self._quiet_rounds = 0
                self._queue.append(_message("heartbeat", self._owner_epoch, cursor=self._cursor))
        return self._queue.pop(0)


@contextmanager
def paged_subscription(fake: Any, after_cursor: int, _owner_epoch: str | None
                       ) -> Iterator[_PagedSubscription]:
    yield _PagedSubscription(fake, after_cursor)
