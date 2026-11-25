"""Automatic motion-trace capture and its History record.

When an operation with a motion participant ends failed, interrupted or
needing intervention, or a participant's Stop escalated, Headless has the
execution host write its volatile commanded-vs-measured trace to the trace
store (``MANYFORGE_MOTION_TRACE_DIR``, bounded by the host) and records one
``motion_trace_captured`` History row naming the file.

The event pump only hands the request over (memory only, never waits); one
worker thread asks the host for the export, one at a time, with at most one
request pending per robot (later ones are counted into it). The host copies
the trace under its own lock and writes the file on its motion-trace lane. A
failed capture is recorded as a failed row, logged once until a capture
succeeds again, and nothing is retried.
"""

from __future__ import annotations

from collections import deque
from collections.abc import Mapping
import logging
import threading
from typing import Any, Final

from .activity_history import SCHEMA_VERSION, _bounded, _line, record_activity
from .native import HeadlessApiError

MOTION_TRACE_OUTCOMES: Final = frozenset({"succeeded", "failed"})
MOTION_TRACE_CAUSES: Final = frozenset({"operation_failed", "stop_escalated"})
# The host's per-file bound (`VolatileMotionTrace::kMaximumTraceBytes`).
MOTION_TRACE_ALLOWANCE_BYTES: Final = 5 * 1024 * 1024
# Robots with a capture waiting (memory only); a request beyond is dropped
# and counted.
MAXIMUM_PENDING_CAPTURES: Final = 16
# Operations already captured, per cause, so a repeated terminal row does
# not capture again (memory only; the oldest is forgotten first).
MAXIMUM_REMEMBERED_OPERATIONS: Final = 64
_FAULT_STATES: Final = frozenset({"failed", "interrupted", "intervention_required"})
_TERMINAL_STATES: Final = _FAULT_STATES | {"completed", "stopped", "cancelled_pre_dispatch"}
_LOGGER = logging.getLogger(__name__)


def motion_trace_record(
    subject: str, outcome: str, error: Any, facts: Mapping[str, Any]
) -> dict[str, Any]:
    """One automatic motion-trace capture of robot ``subject``: the file
    written (``facts["file"]``) or the typed error, and its cause."""

    typed = None
    if outcome == "failed" and isinstance(error, Mapping) and error.get("code"):
        typed = {
            name: _bounded(error[name])
            for name in ("code", "message")
            if error.get(name)
        }
    kept = {
        name: _bounded(value) if isinstance(value, str) else value
        for name, value in facts.items()
        if value is not None
    }
    if outcome == "succeeded":
        parts = [f"Motion trace of {subject} saved: {kept.get('file')}"]
    else:
        parts = [_line(f"Motion trace of {subject}", outcome, typed)]
    if kept.get("cause") == "stop_escalated":
        parts.append(
            f"Stop {kept['stop_event_id']} escalated"
            if kept.get("stop_event_id") is not None else "Stop escalated"
        )
    elif kept.get("operation_id") is not None:
        parts.append(f"operation {kept['operation_id']} {kept.get('operation_state', 'failed')}")
    if kept.get("request_count", 1) > 1:
        parts.append(f"{kept['request_count']} requests")
    return {
        "schema_version": SCHEMA_VERSION,
        "kind": "motion_trace_captured",
        "subject": _bounded(subject, 256),
        "action": None,
        "outcome": outcome,
        "error": typed,
        "components": [],
        "message": _bounded(" · ".join(parts), 4096),
        "motion_trace": kept,
    }


def motion_trace_cause(event: Mapping[str, Any]) -> tuple[str, dict[str, Any]] | None:
    """The robot and cause facts of an ingested operation event that calls
    for a capture: a terminal state row with a motion participant whose Stop
    escalated, or that ended in a fault state; otherwise None."""

    if event.get("record_kind") != "state_observed":
        return None
    state = event["payload"]["operation_state"]
    if state.get("state") not in _TERMINAL_STATES:
        return None
    motion = [
        participant
        for participant in state.get("participants", ())
        if participant.get("payload_kind") == "motion"
    ]
    escalated = [
        participant
        for participant in motion
        if isinstance(participant.get("stop"), Mapping)
        and participant["stop"].get("escalated") is True
    ]
    if escalated:
        participant, cause = escalated[0], "stop_escalated"
        stop_event_id = participant["stop"].get("stop_event_id")
    elif state["state"] in _FAULT_STATES and motion:
        failed = [p for p in motion if p.get("state") in _FAULT_STATES]
        participant, cause = (failed or motion)[0], "operation_failed"
        stop_event_id = state.get("stop_event_id")
    else:
        return None
    subject = participant.get("controllable_component_id")
    if not isinstance(subject, str) or not subject:
        return None
    facts: dict[str, Any] = {
        "cause": cause,
        "operation_id": state["operation_id"],
        "operation_revision": state["operation_revision"],
        "operation_state": state["state"],
        "stop_event_id": stop_event_id,
        "program_run_id": state.get("program_run_id"),
    }
    return subject, facts


class MotionTraceCapture:
    """One worker that has the host write the motion trace after a failure."""

    def __init__(
        self,
        core: Any,
        execution: Any,
        *,
        allowance_bytes: int = MOTION_TRACE_ALLOWANCE_BYTES,
    ) -> None:
        self._core = core
        self._execution = execution
        self._allowance_bytes = allowance_bytes
        self._changed = threading.Condition()
        # Robot -> the facts of its first waiting request and how many
        # requests it stands for; insertion order is capture order.
        self._pending: dict[str, dict[str, Any]] = {}
        self._remembered: deque[tuple[Any, str]] = deque(maxlen=MAXIMUM_REMEMBERED_OPERATIONS)
        self._stopped = False
        self._thread: threading.Thread | None = None
        self._failing = False
        self.dropped = 0
        self.failures = 0

    def start(self) -> None:
        with self._changed:
            self._stopped = False
        self._thread = threading.Thread(
            target=self._run,
            name="manyforge-motion-trace-capture",
            # An export in flight holds no durable work of Headless; it never
            # blocks interpreter exit.
            daemon=True,
        )
        self._thread.start()

    def shutdown(self) -> None:
        """Ends the worker after the export in flight, if any; never waits."""
        with self._changed:
            self._stopped = True
            self._pending.clear()
            self._changed.notify_all()
        self._thread = None

    def observe(self, event: Mapping[str, Any]) -> None:
        """Called by the event pump with each newly ingested event. Memory
        only: it never waits for an export."""

        found = motion_trace_cause(event)
        if found is None:
            return
        subject, facts = found
        key = (facts["operation_id"], facts["cause"])
        if key in self._remembered:
            return
        self._remembered.append(key)
        self.request(subject, facts)

    def request(self, subject: str, facts: Mapping[str, Any]) -> None:
        with self._changed:
            if self._stopped:
                return
            waiting = self._pending.get(subject)
            if waiting is not None:
                waiting["request_count"] += 1
                return
            if len(self._pending) >= MAXIMUM_PENDING_CAPTURES:
                self.dropped += 1
                return
            self._pending[subject] = {**facts, "request_count": 1}
            self._changed.notify()

    def _run(self) -> None:
        while True:
            with self._changed:
                while not self._pending and not self._stopped:
                    self._changed.wait()
                if self._stopped:
                    return
                subject = next(iter(self._pending))
                facts = self._pending.pop(subject)
            try:
                record_activity(self._core, self._capture(subject, facts))
            except Exception:  # noqa: BLE001 - a lost capture never ends the worker
                _LOGGER.exception("motion trace capture failed")

    def _capture(self, subject: str, facts: dict[str, Any]) -> dict[str, Any]:
        try:
            metadata = self._execution.capture_motion_trace_file(self._allowance_bytes)
        except HeadlessApiError as error:
            self.failures += 1
            if not self._failing:
                self._failing = True
                _LOGGER.warning(
                    "motion trace capture failed: %s: %s", error.code, error,
                )
            return motion_trace_record(
                subject, "failed", {"code": error.code, "message": str(error)}, facts
            )
        if self._failing:
            self._failing = False
            _LOGGER.info("motion trace capture recovered")
        return motion_trace_record(
            subject,
            "succeeded",
            None,
            {
                **facts,
                "file": metadata["file"],
                "bytes": metadata["bytes"],
                "status": metadata["status"],
            },
        )


__all__ = [
    "MAXIMUM_PENDING_CAPTURES",
    "MAXIMUM_REMEMBERED_OPERATIONS",
    "MOTION_TRACE_ALLOWANCE_BYTES",
    "MOTION_TRACE_CAUSES",
    "MOTION_TRACE_OUTCOMES",
    "MotionTraceCapture",
    "motion_trace_cause",
    "motion_trace_record",
]
