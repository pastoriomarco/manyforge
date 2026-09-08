"""One bounded, lazy delivery slot set for Program forensic captures.

This is only a transport to the existing Historian, not another log or owner.
No I/O, wait, retry or executor submission occurs on the producer path.
"""

from __future__ import annotations

from collections import deque
import logging
from threading import Condition, Thread


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
                            if pending.get("state") == "succeeded"
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
            thread.join(timeout=2.0)
            if thread.is_alive():
                raise RuntimeError("Program diagnostic transport exceeded its shutdown bound")
