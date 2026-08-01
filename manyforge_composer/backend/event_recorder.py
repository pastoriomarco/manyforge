"""Bounded, transport-neutral event-history presentation helpers.

The historical :class:`EventRecorder` at
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` mixed several
responsibilities in one Composer process:

* polling CycleManager and Kernel state and synthesizing semantic events;
* ordering native evidence and assembling incidents and Stop decisions;
* owning JSONL durability, rotation, pruning, failure archives, and shutdown;
* retaining current run/channel state; and
* filtering and projecting already-recorded events for clients.

Only the last, value-level responsibility still belongs in this Python
package.  The headless C++ Historian returns at I5/H0 and owns asynchronous
durable raw/lifecycle records.  Source owners emit their own facts, and I8/P5
owns Stop causality and terminality.  This module therefore contains no
filesystem, thread, queue, clock, runtime, scene, CycleManager, Kernel,
alarm-manager, or Stop-assembler dependency.

The ``EventRecorder`` name remains only as a fail-before-state cutover gate so
an unmigrated caller cannot silently recreate Composer-local history truth.
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any, Final, Iterable, Mapping, NoReturn, Sequence


HISTORIAN_CLIENT_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_historian_client_unavailable"
)
HISTORIAN_RESTORATION_GATES: Final = ("I5", "H0")
HISTORY_SOURCE_REQUALIFICATION_GATES: Final = (
    "I6",
    "N",
    "I7",
    "L0",
    "I8",
    "P5",
)

DEFAULT_HISTORY_PAGE_LIMIT: Final = 1_000
MAX_HISTORY_PAGE_LIMIT: Final = 10_000
LATENCY_BUCKET_BOUNDS_NS: Final = (
    1_000_000,
    2_000_000,
    5_000_000,
    10_000_000,
    20_000_000,
    50_000_000,
    100_000_000,
    200_000_000,
    500_000_000,
    1_000_000_000,
    2_000_000_000,
    5_000_000_000,
    10_000_000_000,
)

_SEVERITY_ORDER: Final = {
    "DEBUG": 10,
    "INFO": 20,
    "WARN": 30,
    "WARNING": 30,
    "ERROR": 40,
    "CRITICAL": 50,
}
_EPOCH_UTC: Final = datetime(1970, 1, 1, tzinfo=timezone.utc)


class HistorianClientUnavailableError(RuntimeError):
    """Raised before a former local recorder or collaborator is touched."""


class EventPageBoundsError(ValueError):
    """Raised when a client projection is asked to consume an unbounded page."""


def _raise_historian_unavailable() -> NoReturn:
    raise HistorianClientUnavailableError(
        f"{HISTORIAN_CLIENT_UNAVAILABLE_REASON_CODE}: the asynchronous "
        "headless Historian and its thin query client return at I5/H0"
    )


def _normalized_values(
    values: Sequence[str] | None,
    *,
    field_name: str,
) -> tuple[str, ...]:
    if values is None:
        return ()
    if isinstance(values, (str, bytes)):
        raise TypeError(f"{field_name} must be a sequence of strings")
    normalized: list[str] = []
    for raw_value in values:
        if not isinstance(raw_value, str):
            raise TypeError(f"{field_name} must contain only strings")
        value = raw_value.strip()
        if value and value not in normalized:
            normalized.append(value)
    if len(normalized) > 256:
        raise EventPageBoundsError(f"{field_name} exceeds 256 entries")
    return tuple(normalized)


def _optional_nonnegative_int(value: Any) -> int | None:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        return None
    return value


def _required_nonnegative_int(value: Any, *, field_name: str) -> int:
    result = _optional_nonnegative_int(value)
    if result is None:
        raise ValueError(f"{field_name} must be a non-negative integer")
    return result


def _bounded_page_limit(limit: int) -> int:
    if isinstance(limit, bool) or not isinstance(limit, int):
        raise TypeError("limit must be an integer")
    if limit < 0:
        raise ValueError("limit must be non-negative")
    return min(limit, MAX_HISTORY_PAGE_LIMIT)


@dataclass(frozen=True, slots=True)
class HistoryQuery:
    """Pure filters for one already-bounded Historian result page.

    These fields select source-owned facts; they do not infer missing
    identities, repair events, or establish causal order.
    """

    start_ns: int | None = None
    end_ns: int | None = None
    from_wall_ns: int | None = None
    to_wall_ns: int | None = None
    min_severity: str | None = None
    components: Sequence[str] = ()
    event_types: Sequence[str] = ()
    execution_id: int | None = None
    kernel_execution_id: int | None = None
    program_run_id: str | None = None
    intervention_run_id: str | None = None
    action_span_id: str | None = None
    node_instance_id: str | None = None
    node_attempt_id: str | None = None
    move_id: str | None = None
    contact_policy_id: str | None = None

    def __post_init__(self) -> None:
        for field_name in ("start_ns", "end_ns", "from_wall_ns", "to_wall_ns"):
            value = getattr(self, field_name)
            if value is not None:
                _required_nonnegative_int(value, field_name=field_name)
        if (
            self.start_ns is not None
            and self.end_ns is not None
            and self.start_ns > self.end_ns
        ):
            raise ValueError("start_ns must not exceed end_ns")
        if (
            self.from_wall_ns is not None
            and self.to_wall_ns is not None
            and self.from_wall_ns > self.to_wall_ns
        ):
            raise ValueError("from_wall_ns must not exceed to_wall_ns")

        if self.min_severity is not None and not isinstance(
            self.min_severity,
            str,
        ):
            raise TypeError("min_severity must be a string or None")
        severity = self.min_severity.upper() if self.min_severity else None
        if severity is not None and severity not in _SEVERITY_ORDER:
            raise ValueError(f"unsupported severity level: {self.min_severity}")
        object.__setattr__(self, "min_severity", severity)
        object.__setattr__(
            self,
            "components",
            _normalized_values(self.components, field_name="components"),
        )
        object.__setattr__(
            self,
            "event_types",
            _normalized_values(self.event_types, field_name="event_types"),
        )

        for field_name in ("execution_id", "kernel_execution_id"):
            value = getattr(self, field_name)
            if value is not None:
                _required_nonnegative_int(value, field_name=field_name)
        if (
            self.execution_id is not None
            and self.kernel_execution_id is not None
            and self.execution_id != self.kernel_execution_id
        ):
            raise ValueError(
                "execution_id and kernel_execution_id must agree when both are set"
            )


def parse_wall_timestamp_ns(value: Any) -> int | None:
    """Parse an ISO-8601 timestamp to integer epoch nanoseconds.

    Naive timestamps retain the historical UTC interpretation.  Returning
    ``None`` lets a presentation query keep malformed evidence visible rather
    than silently discard it.
    """

    if not isinstance(value, str) or not value.strip():
        return None
    text = value.strip()
    if text.endswith("Z"):
        text = text[:-1] + "+00:00"
    try:
        parsed = datetime.fromisoformat(text)
    except ValueError:
        return None
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    delta = parsed - _EPOCH_UTC
    return (
        (delta.days * 86_400 + delta.seconds) * 1_000_000_000
        + delta.microseconds * 1_000
    )


def _event_text_field(event: Mapping[str, Any], key: str) -> str | None:
    value = event.get(key)
    details = event.get("details")
    if value is None and isinstance(details, Mapping):
        value = details.get(key)
    return None if value is None else str(value)


def event_matches_history_query(
    event: Mapping[str, Any],
    query: HistoryQuery,
) -> bool:
    """Return whether one source-owned event matches presentation filters."""

    timestamp_ns = _optional_nonnegative_int(event.get("timestamp_ns"))
    if query.start_ns is not None and (
        timestamp_ns is None or timestamp_ns < query.start_ns
    ):
        return False
    if query.end_ns is not None and (
        timestamp_ns is None or timestamp_ns > query.end_ns
    ):
        return False

    if query.from_wall_ns is not None or query.to_wall_ns is not None:
        wall_ns = parse_wall_timestamp_ns(
            event.get("timestamp_wall") or event.get("timestamp")
        )
        if wall_ns is not None:
            if query.from_wall_ns is not None and wall_ns < query.from_wall_ns:
                return False
            if query.to_wall_ns is not None and wall_ns > query.to_wall_ns:
                return False

    if query.min_severity is not None:
        current_severity = _SEVERITY_ORDER.get(
            str(event.get("severity", "")).upper(),
            0,
        )
        if current_severity < _SEVERITY_ORDER[query.min_severity]:
            return False

    component = str(event.get("component", ""))
    if query.components and not any(
        component == prefix or component.startswith(f"{prefix}.")
        for prefix in query.components
    ):
        return False
    if query.event_types and event.get("type") not in query.event_types:
        return False

    requested_execution_id = (
        query.execution_id
        if query.execution_id is not None
        else query.kernel_execution_id
    )
    if requested_execution_id is not None:
        if (
            _optional_nonnegative_int(event.get("execution_id"))
            != requested_execution_id
        ):
            return False

    for field_name in (
        "program_run_id",
        "intervention_run_id",
        "action_span_id",
        "node_instance_id",
        "node_attempt_id",
        "move_id",
        "contact_policy_id",
    ):
        expected = getattr(query, field_name)
        if (
            expected is not None
            and _event_text_field(event, field_name) != expected
        ):
            return False
    return True


def _copy_event(event: Mapping[str, Any]) -> dict[str, Any]:
    copied = dict(event)
    details = copied.get("details")
    if isinstance(details, Mapping):
        copied["details"] = dict(details)
    return copied


def filter_history_events(
    events: Iterable[Mapping[str, Any]],
    *,
    query: HistoryQuery | None = None,
    limit: int = DEFAULT_HISTORY_PAGE_LIMIT,
    max_scanned_events: int = MAX_HISTORY_PAGE_LIMIT,
) -> list[dict[str, Any]]:
    """Filter a bounded page and return its most-recent matching tail.

    The future Historian owns pagination and persistence.  This client helper
    refuses to scan more than ``MAX_HISTORY_PAGE_LIMIT`` events, so it cannot
    become an accidental in-process historian.
    """

    effective_limit = _bounded_page_limit(limit)
    if (
        isinstance(max_scanned_events, bool)
        or not isinstance(max_scanned_events, int)
        or max_scanned_events < 1
        or max_scanned_events > MAX_HISTORY_PAGE_LIMIT
    ):
        raise EventPageBoundsError(
            f"max_scanned_events must be in [1, {MAX_HISTORY_PAGE_LIMIT}]"
        )
    if effective_limit == 0:
        return []

    effective_query = query or HistoryQuery()
    selected: deque[dict[str, Any]] = deque(maxlen=effective_limit)
    for index, event in enumerate(events):
        if index >= max_scanned_events:
            raise EventPageBoundsError(
                f"event page exceeds {max_scanned_events} scanned events"
            )
        if not isinstance(event, Mapping):
            raise TypeError("events must contain mappings")
        if event_matches_history_query(event, effective_query):
            selected.append(_copy_event(event))
    return list(selected)


def project_history_page(
    events: Iterable[Mapping[str, Any]],
    *,
    query: HistoryQuery | None = None,
    limit: int = DEFAULT_HISTORY_PAGE_LIMIT,
    observed_at_ns: int,
    selected_run_id: str | None = None,
    run_ids: Sequence[str] = (),
    runs_truncated: bool = False,
    scanned_run_count: int = 0,
    cross_run_scan_limit: int = 0,
) -> dict[str, Any]:
    """Project one bounded Historian page into the existing HMI view shape."""

    observed_at_ns = _required_nonnegative_int(
        observed_at_ns,
        field_name="observed_at_ns",
    )
    filtered = filter_history_events(events, query=query, limit=limit)
    regular_events = [
        event for event in filtered if event.get("type") != "node_status_transition"
    ]
    node_transitions = []
    for event in filtered:
        if event.get("type") != "node_status_transition":
            continue
        node_transitions.append(
            {
                key: event.get(key)
                for key in (
                    "timestamp_ns",
                    "timestamp",
                    "run_id",
                    "program_run_id",
                    "intervention_run_id",
                    "session_id",
                    "session_kind",
                    "authority_source",
                    "principal_id",
                    "grant_type",
                    "parent_run_id",
                    "trigger_reason",
                    "trigger_node_instance",
                    "execution_mode",
                    "execution_id",
                    "node",
                    "from",
                    "to",
                )
            }
        )

    trailing_execution_id = 0
    for event in reversed(filtered):
        candidate = _optional_nonnegative_int(event.get("execution_id"))
        if candidate:
            trailing_execution_id = candidate
            break

    normalized_run_ids = _normalized_values(run_ids, field_name="run_ids")
    return {
        "run_id": selected_run_id,
        "run_ids": list(normalized_run_ids),
        "runs_truncated": bool(runs_truncated),
        "scanned_run_count": _required_nonnegative_int(
            scanned_run_count,
            field_name="scanned_run_count",
        ),
        "cross_run_scan_limit": _required_nonnegative_int(
            cross_run_scan_limit,
            field_name="cross_run_scan_limit",
        ),
        "execution_id": trailing_execution_id,
        "events": regular_events,
        "node_status_transitions": node_transitions,
        "timestamp_ns": observed_at_ns,
    }


def project_normalized_trace(
    events: Iterable[Mapping[str, Any]],
    *,
    exported_at_ns: int,
    trace_source: str,
    run_id: str | None = None,
    max_events: int = MAX_HISTORY_PAGE_LIMIT,
) -> dict[str, Any]:
    """Purely project one bounded page to ``manyforge.ros2_medkit_trace.v1``.

    This helper performs no file read/write and does not push to an external
    system.  The source Historian remains responsible for completeness,
    ordering, pagination, retention, and durability.
    """

    exported_at_ns = _required_nonnegative_int(
        exported_at_ns,
        field_name="exported_at_ns",
    )
    if not isinstance(trace_source, str) or not trace_source.strip():
        raise ValueError("trace_source must be a non-empty string")
    effective_max = _bounded_page_limit(max_events)
    materialized = filter_history_events(
        events,
        limit=effective_max,
        max_scanned_events=max(1, effective_max),
    )

    program_run_id = next(
        (
            str(event["program_run_id"])
            for event in materialized
            if isinstance(event.get("program_run_id"), str)
            and event.get("program_run_id")
        ),
        run_id,
    )
    intervention_run_ids = sorted(
        {
            str(event["intervention_run_id"])
            for event in materialized
            if isinstance(event.get("intervention_run_id"), str)
            and event.get("intervention_run_id")
        }
    )
    parent_run_ids = sorted(
        {
            str(event["parent_run_id"])
            for event in materialized
            if isinstance(event.get("parent_run_id"), str)
            and event.get("parent_run_id")
        }
    )

    records: list[dict[str, Any]] = []
    for event in materialized:
        timeline_id = (
            event.get("intervention_run_id")
            or event.get("program_run_id")
            or event.get("run_id")
        )
        attributes: dict[str, Any] = {}
        details = event.get("details")
        if isinstance(details, Mapping):
            attributes.update(details)
        for key in ("authority_source", "grant_type", "session_kind"):
            if isinstance(event.get(key), str):
                attributes[key] = event[key]

        event_index = _optional_nonnegative_int(event.get("event_index")) or 0
        records.append(
            {
                "record_id": f"{timeline_id or 'unknown'}:{event_index}",
                "timeline_id": timeline_id,
                "parent_timeline_id": event.get("parent_run_id"),
                "event_index": event_index,
                "monotonic_ns": (
                    _optional_nonnegative_int(event.get("timestamp_ns")) or 0
                ),
                "wall_time": event.get("timestamp_wall") or event.get("timestamp"),
                "level": event.get("severity", "INFO"),
                "component": event.get("component", "manyforge.runtime"),
                "event_type": event.get("type", "unknown"),
                "message": event.get("message", ""),
                "execution_mode": event.get("execution_mode"),
                "execution_id": (
                    _optional_nonnegative_int(event.get("execution_id")) or 0
                ),
                "operation_id": event.get("operation_id"),
                "operation_revision": event.get("operation_revision"),
                "action_span_id": _event_text_field(event, "action_span_id"),
                "move_id": _event_text_field(event, "move_id"),
                "node": event.get("node"),
                "node_instance_id": _event_text_field(
                    event,
                    "node_instance_id",
                ),
                "node_attempt_id": _event_text_field(event, "node_attempt_id"),
                "session_id": event.get("session_id"),
                "principal_id": event.get("principal_id"),
                "trigger_reason": event.get("trigger_reason"),
                "trigger_node_instance": event.get("trigger_node_instance"),
                "attributes": attributes,
            }
        )

    return {
        "schema_version": "manyforge.ros2_medkit_trace.v1",
        "trace_source": trace_source.strip(),
        "exported_at_ns": exported_at_ns,
        "run_id": run_id,
        "program_run_id": program_run_id,
        "intervention_run_ids": intervention_run_ids,
        "parent_run_ids": parent_run_ids,
        "records": records,
    }


class LatencyHistogram:
    """Fixed-memory presentation summary over source-provided durations."""

    __slots__ = ("_counts", "_count", "_sum_ns", "_min_ns", "_max_ns")

    def __init__(self) -> None:
        self._counts = [0] * (len(LATENCY_BUCKET_BOUNDS_NS) + 1)
        self._count = 0
        self._sum_ns = 0
        self._min_ns: int | None = None
        self._max_ns: int | None = None

    def observe(self, duration_ns: int) -> None:
        if isinstance(duration_ns, bool) or not isinstance(duration_ns, int):
            raise TypeError("duration_ns must be an integer")
        if duration_ns < 0:
            return
        bucket_index = len(LATENCY_BUCKET_BOUNDS_NS)
        for index, bound in enumerate(LATENCY_BUCKET_BOUNDS_NS):
            if duration_ns <= bound:
                bucket_index = index
                break
        self._counts[bucket_index] += 1
        self._count += 1
        self._sum_ns += duration_ns
        if self._min_ns is None or duration_ns < self._min_ns:
            self._min_ns = duration_ns
        if self._max_ns is None or duration_ns > self._max_ns:
            self._max_ns = duration_ns

    def percentile_ns(self, fraction: float) -> int | None:
        if not isinstance(fraction, (int, float)) or isinstance(fraction, bool):
            raise TypeError("fraction must be numeric")
        if not 0.0 <= float(fraction) <= 1.0:
            raise ValueError("fraction must be in [0, 1]")
        if self._count == 0:
            return None

        target = float(fraction) * self._count
        cumulative = 0
        for index, bucket_count in enumerate(self._counts):
            if bucket_count == 0:
                continue
            if cumulative + bucket_count >= target:
                lower = LATENCY_BUCKET_BOUNDS_NS[index - 1] if index > 0 else 0
                if index >= len(LATENCY_BUCKET_BOUNDS_NS):
                    return LATENCY_BUCKET_BOUNDS_NS[-1]
                upper = LATENCY_BUCKET_BOUNDS_NS[index]
                within = (target - cumulative) / bucket_count
                return int(lower + (upper - lower) * within)
            cumulative += bucket_count
        return self._max_ns

    def to_payload(self) -> dict[str, Any]:
        return {
            "count": self._count,
            "sum_ns": self._sum_ns,
            "min_ns": self._min_ns,
            "max_ns": self._max_ns,
            "p50_ns": self.percentile_ns(0.50),
            "p95_ns": self.percentile_ns(0.95),
            "p99_ns": self.percentile_ns(0.99),
            "bucket_bounds_ns": list(LATENCY_BUCKET_BOUNDS_NS),
            "bucket_counts": list(self._counts),
        }


class EventRecorder:
    """Unavailable compatibility name; Composer no longer owns history."""

    def __init__(self, *_args: object, **_kwargs: object) -> None:
        _raise_historian_unavailable()


def require_historian_client(*_args: object, **_kwargs: object) -> NoReturn:
    """Fail closed before inspecting any former runtime or storage input."""

    _raise_historian_unavailable()


__all__ = [
    "DEFAULT_HISTORY_PAGE_LIMIT",
    "EventPageBoundsError",
    "EventRecorder",
    "HISTORIAN_CLIENT_UNAVAILABLE_REASON_CODE",
    "HISTORIAN_RESTORATION_GATES",
    "HISTORY_SOURCE_REQUALIFICATION_GATES",
    "HistorianClientUnavailableError",
    "HistoryQuery",
    "LATENCY_BUCKET_BOUNDS_NS",
    "LatencyHistogram",
    "MAX_HISTORY_PAGE_LIMIT",
    "event_matches_history_query",
    "filter_history_events",
    "parse_wall_timestamp_ns",
    "project_history_page",
    "project_normalized_trace",
    "require_historian_client",
]
