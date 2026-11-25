"""History records of Program run events.

One event per Program run start (``program_run_started``), one per run end
in any end state (``program_run_finished``: succeeded, failed or stopped) and
one per audit event of the Program Runner (a recovery action applied, an
operator question and its answer, a manual result, an automatic retry, a
preemption, an interrupt result, an I/O source change, a simulation hold,
...). The Runner stamps each at production; ``message`` is the History list's
display line, built here from the typed fields. The native historian
validates the record and stores it in the existing bounded History.

Entries the Runner dropped before delivery (its queue full or closed, a
delivery failed) are counted into its next batch; that count is one
``records_lost`` record.
"""

from __future__ import annotations

from collections.abc import Iterable, Mapping
import json
import math
from typing import Any, Final
import unicodedata

from .activity_history import (
    SCHEMA_VERSION,
    _bounded,
    _line,
    _typed_error,
    _words,
    record_activity,
)

# A record keeps 3 required facts and at most 29 of the event's own, each a
# scalar (text cut to its bound), well within the 64 KiB record bound.
_MAX_EVENT_FACTS: Final = 29
_FACT_TEXT_BYTES: Final = 1024
_CALLER_TEXT_BYTES: Final = 256
_MAX_SAFE_INTEGER: Final = 2**53 - 1
# Facts that name who asked for the event go to the record's `caller`.
_CALLER_FACTS: Final = ("request_key", "source_label")
# Facts named on the display line, in this order.
_LINE_FACTS: Final = (
    ("node_instance_id", "node"),
    ("node_attempt_id", "attempt"),
    ("action_id", "action"),
    ("option_id", "option"),
    ("code", "code"),
)


def _text(value: str, limit: int) -> str:
    return _bounded(unicodedata.normalize("NFC", value), limit)


def _scalar(value: Any) -> Any:
    """One fact as History keeps it: null, bool, finite number or bounded
    text; a structured value becomes its compact JSON text."""

    if value is None or isinstance(value, bool):
        return value
    if isinstance(value, int):
        return value if abs(value) <= _MAX_SAFE_INTEGER else str(value)
    if isinstance(value, float):
        return value if math.isfinite(value) else str(value)
    if not isinstance(value, str):
        value = json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True)
    return _text(value, _FACT_TEXT_BYTES)


def _occurred_at_ns(text: str) -> int | str:
    value = int(text)
    return value if value <= _MAX_SAFE_INTEGER else text


def program_run_event_record(entry: Mapping[str, Any]) -> dict[str, Any]:
    """One Program run event (``ProgramRunEventFacts``) as a History record:
    subject the event name, outcome its label, the typed error only when it
    failed, its facts as bounded scalars and its caller when it names one."""

    event = entry["event"]
    outcome = entry["outcome"]
    program_run_id = entry["program_run_id"]
    typed = _typed_error(entry.get("error")) if outcome == "failed" else None
    facts = dict(entry["facts"])
    caller = {
        name: _text(facts.pop(name), _CALLER_TEXT_BYTES)
        for name in _CALLER_FACTS
        if isinstance(facts.get(name), str) and facts[name]
    }
    run_event = {
        name: _scalar(value)
        for name, value in list(facts.items())[:_MAX_EVENT_FACTS]
    }
    run_event.update({
        "program_run_id": program_run_id,
        "occurred_at_ns": _occurred_at_ns(entry["occurred_at_ns"]),
        "occurred_at_wall": entry["occurred_at_wall"],
    })
    if event == "program_run_started":
        parts = [f"Program run {program_run_id} started"]
        for name, label in (("scenario_id", "scenario"), ("program_revision", "Program")):
            if isinstance(facts.get(name), str):
                parts.append(f"{label} {facts[name][:19]}")
    elif event == "program_run_finished":
        parts = [_line(f"Program run {program_run_id}", outcome, typed)]
        if facts.get("end_reason") not in (None, outcome, "completed"):
            parts.append(_words(str(facts["end_reason"])))
        for name in ("stopped_by", "rule_id", "component_id", "node_instance_id"):
            if isinstance(facts.get(name), str):
                parts.append(f"{_words(name)} {facts[name]}")
    else:
        # An audit event: its name and the node, attempt, action or option
        # it concerns.
        parts = [_line(f"Program run {program_run_id} {_words(event)}", outcome, typed)
                 if outcome != "recorded"
                 else f"Program run {program_run_id}: {_words(event)}"]
        for name, label in _LINE_FACTS:
            value = facts.get(name)
            if isinstance(value, (str, int)) and not isinstance(value, bool):
                parts.append(f"{label} {value}")
    if caller.get("source_label"):
        parts.append(caller["source_label"])
    record = {
        "schema_version": SCHEMA_VERSION,
        "kind": "program_run_event",
        "subject": event,
        "action": None,
        "outcome": outcome,
        "error": typed,
        "components": [],
        "message": _text(" · ".join(parts), 4096),
        "run_event": run_event,
    }
    if caller:
        record["caller"] = caller
    return record


def record_program_run_events(core: Any, entries: Iterable[Mapping[str, Any]]) -> int:
    """Hand each run event to the historian; the number it took."""

    return sum(record_activity(core, program_run_event_record(entry)) for entry in entries)


RUNNER_PUBLISHER: Final = "program_runner_history_publisher"


def runner_records_lost_record(loss: Mapping[str, Any]) -> dict[str, Any]:
    """The Runner's node attempts and run events lost since its last
    delivered batch (``lost_since_last_batch``) as one ``records_lost``
    record."""

    count = loss["count"]
    first, last = loss["first_lost_wall"], loss["last_lost_wall"]
    when = f"at {first}" if first == last else f"between {first} and {last}"
    return {
        "schema_version": SCHEMA_VERSION,
        "kind": "records_lost",
        "subject": RUNNER_PUBLISHER,
        "action": None,
        "outcome": "lost",
        "error": None,
        "components": [],
        "message": (
            f"Program Runner history lost {count} "
            f"{'record' if count == 1 else 'records'} {when}"
        ),
        "records_lost": {
            "lost_count": count,
            "first_lost_wall": first,
            "last_lost_wall": last,
        },
    }


def record_runner_records_lost(core: Any, loss: Mapping[str, Any] | None) -> None:
    """Hand the Runner's loss count, when it has one, to the historian."""

    if loss is not None:
        record_activity(core, runner_records_lost_record(loss))


__all__ = [
    "RUNNER_PUBLISHER",
    "program_run_event_record",
    "record_program_run_events",
    "record_runner_records_lost",
    "runner_records_lost_record",
]
