"""One bounded, lazy delivery slot set for Program forensic captures.

This is only a transport to the existing Historian, not another log or owner.
No I/O, wait, retry or executor submission occurs on the producer path.
"""

from __future__ import annotations

from collections import deque
from datetime import datetime, timezone
import logging
from threading import Condition, Thread
from time import monotonic_ns, time_ns
from typing import Any, Mapping


_LOGGER = logging.getLogger(__name__)
_MAX_COUNT = 2**53 - 1  # Diagnostic saturation, never an execution lifetime bound.


class ProgramDiagnosticPublisher:
    def __init__(self, send, *, capacity: int = 16) -> None:
        if not callable(send) or type(capacity) is not int or not 1 <= capacity <= 16:
            raise ValueError("invalid bounded Program diagnostic publisher")
        self._send = send
        self._capacity = capacity
        self._pending = deque()
        self._condition = Condition()
        self._thread = None
        self._closed = False
        self._lost = 0
        self._sent = 0
        self._failed = False
        self._notice_pending = False

    def submit(self, record) -> None:
        with self._condition:
            if self._closed:
                self._loss_locked()
                return
            if len(self._pending) >= self._capacity:
                # Preserve the Historian's important tier before HTTP delivery.
                # Scan at most 16 pending captures, never the in-flight record.
                victim = None
                if record.get("state") in ("failed", "intervention_required"):
                    victim = next(
                        (
                            index
                            for index, pending in enumerate(self._pending)
                            if pending.get("state") in ("succeeded", "held")
                        ),
                        None,
                    )
                self._loss_locked()
                if victim is None:
                    return
                del self._pending[victim]
            self._pending.append(record)
            if self._thread is None:
                thread = Thread(target=self._run, name="program-diagnostics", daemon=True)
                try:
                    thread.start()
                except RuntimeError:
                    self._pending.pop()
                    self._loss_locked()
                    return
                self._thread = thread
            self._condition.notify()

    def _loss_locked(self) -> None:
        self._lost = min(_MAX_COUNT, self._lost + 1)
        if not self._failed:
            self._notice_pending = True
        self._failed = True
        self._condition.notify_all()

    def _run(self) -> None:
        while True:
            with self._condition:
                self._condition.wait_for(lambda: self._pending or self._closed)
                if not self._pending:
                    return
                record = self._pending.popleft()
                lost = self._lost
            try:
                result = self._send({**record, "delivery_loss_count": lost})
                if result.get("recorded") is not True:
                    raise RuntimeError("Historian rejected Program diagnostic")
            except Exception:
                with self._condition:
                    self._loss_locked()
                    notice, self._notice_pending = self._notice_pending, False
                if notice:
                    _LOGGER.exception(
                        "Program forensic delivery unavailable; history may be incomplete"
                    )
            else:
                with self._condition:
                    self._sent = min(_MAX_COUNT, self._sent + 1)
                    notice, self._notice_pending = self._notice_pending, False
                    recovered = self._failed and lost == self._lost
                    if recovered:
                        self._failed = False
                if notice:
                    _LOGGER.error(
                        "Program forensic delivery unavailable; history may be incomplete"
                    )
                if recovered:
                    _LOGGER.info("Program forensic delivery recovered; prior gaps remain recorded")

    def health(self) -> dict:
        with self._condition:
            return {
                "schema_version": "manyforge.program_diagnostic_health.v1",
                "delivery_failed": self._failed,
                "lost_count": str(self._lost),
                "enqueued_count": len(self._pending),
                "sent_count": str(self._sent),
                "capacity": self._capacity,
                "closed": self._closed,
            }

    def close(self) -> None:
        with self._condition:
            self._closed = True
            # Do not let diagnostic draining extend execution-owner shutdown.
            if self._pending:
                self._lost = min(_MAX_COUNT, self._lost + len(self._pending))
                self._failed = True
            self._pending.clear()
            self._condition.notify_all()
            thread = self._thread
        if thread is not None:
            # The one in-flight delivery ends by Headless's answer or by its
            # connection failing; no time limit decides it (OD-309).
            thread.join()


def _wall(utc_ns: int) -> str:
    return datetime.fromtimestamp(utc_ns / 1e9, timezone.utc).isoformat()


def program_run_event(
    program_run_id: int,
    event: str,
    outcome: str,
    facts: Mapping[str, Any],
    error: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    """One Program run event (start, end or audit event) for History, stamped
    here, at production, on the Runner's clocks (raw CLOCK_MONOTONIC and UTC),
    like a node attempt's end. Delivery to History is later."""

    return {
        "program_run_id": program_run_id,
        "event": event,
        "outcome": outcome,
        "occurred_at_ns": str(monotonic_ns()),
        "occurred_at_wall": datetime.now(timezone.utc).isoformat(),
        "error": None if error is None else dict(error),
        "facts": dict(facts),
    }


class ProgramNodeAttemptPublisher:
    """Deliver finished node-attempt facts and Program run events to the
    Historian in batches (OD-375).

    ``submit`` and ``submit_run_event`` run on a run's tick or a command: they
    never wait. Both share one queue, bound and loss count. A full or closed
    queue drops the newest entry and counts it; so does a failed delivery.
    The next batch carries the losses since the last delivered batch
    (``lost_since_last_batch``), so History records them.
    """

    def __init__(self, send, application_start_id: str, *, capacity: int = 512, batch: int = 64) -> None:
        self._send = send
        self._application_start_id = application_start_id
        self._capacity = capacity
        self._batch = batch
        self._pending = deque()
        self._condition = Condition()
        self._thread = None
        self._closed = False
        self._lost = 0
        self._sent = 0
        self._failed = False
        self._notice_pending = False
        # Losses not yet carried by a delivered batch: (count, first UTC ns,
        # last UTC ns), or None.
        self._unreported = None

    def submit(self, facts) -> None:
        self._submit(("attempts", facts))

    def submit_run_event(self, event) -> None:
        self._submit(("run_events", event))

    def _submit(self, entry) -> None:
        with self._condition:
            if self._closed or len(self._pending) >= self._capacity:
                self._loss_locked(1)
                return
            self._pending.append(entry)
            if self._thread is None:
                thread = Thread(target=self._run, name="program-node-attempts", daemon=True)
                try:
                    thread.start()
                except RuntimeError:
                    self._pending.pop()
                    self._loss_locked(1)
                    return
                self._thread = thread
            self._condition.notify()

    def _loss_locked(self, count: int) -> None:
        self._lost = min(_MAX_COUNT, self._lost + count)
        if not self._failed:
            self._notice_pending = True
        self._failed = True
        now = time_ns()
        self._unreport_locked((count, now, now))

    def _unreport_locked(self, loss) -> None:
        if self._unreported is None:
            self._unreported = loss
        else:
            count, first, last = self._unreported
            self._unreported = (
                min(_MAX_COUNT, count + loss[0]), min(first, loss[1]), max(last, loss[2])
            )

    def _run(self) -> None:
        while True:
            with self._condition:
                self._condition.wait_for(lambda: self._pending or self._closed)
                if not self._pending:
                    return
                entries = [
                    self._pending.popleft()
                    for _ in range(min(self._batch, len(self._pending)))
                ]
                lost = self._lost
                unreported, self._unreported = self._unreported, None
            batch = {"attempts": [], "run_events": []}
            for kind, entry in entries:
                batch[kind].append(entry)
            try:
                result = self._send({
                    "schema_version": "manyforge.program_node_attempt_batch.v2",
                    "application_start_id": self._application_start_id,
                    **batch,
                    "lost_since_last_batch": (
                        None
                        if unreported is None
                        else {
                            "count": unreported[0],
                            "first_lost_wall": _wall(unreported[1]),
                            "last_lost_wall": _wall(unreported[2]),
                        }
                    ),
                })
                recorded = result.get("recorded")
                if type(recorded) is not int or not 0 <= recorded <= len(entries):
                    raise RuntimeError("Historian rejected Program node attempts")
            except Exception:
                with self._condition:
                    # The losses it carried go with the next batch.
                    if unreported is not None:
                        self._unreport_locked(unreported)
                    self._loss_locked(len(entries))
                    notice, self._notice_pending = self._notice_pending, False
                if notice:
                    _LOGGER.exception(
                        "Program node-attempt delivery unavailable; history may be incomplete"
                    )
            else:
                with self._condition:
                    self._sent = min(_MAX_COUNT, self._sent + recorded)
                    if recorded < len(entries):
                        self._loss_locked(len(entries) - recorded)
                    notice, self._notice_pending = self._notice_pending, False
                    recovered = self._failed and lost == self._lost
                    if recovered:
                        self._failed = False
                if notice:
                    _LOGGER.error(
                        "Program node-attempt delivery unavailable; history may be incomplete"
                    )
                if recovered:
                    _LOGGER.info("Program node-attempt delivery recovered; prior gaps remain recorded")

    def health(self) -> dict:
        with self._condition:
            return {
                "schema_version": "manyforge.program_node_attempt_health.v1",
                "delivery_failed": self._failed,
                "lost_count": str(self._lost),
                "enqueued_count": len(self._pending),
                "sent_count": str(self._sent),
                "capacity": self._capacity,
                "closed": self._closed,
            }

    def close(self) -> None:
        with self._condition:
            self._closed = True
            # Do not let delivery extend execution-owner shutdown.
            if self._pending:
                self._loss_locked(len(self._pending))
            self._pending.clear()
            self._condition.notify_all()
            thread = self._thread
        if thread is not None:
            # The one in-flight delivery ends by Headless's answer or by its
            # connection failing; no time limit decides it (OD-309).
            thread.join()
