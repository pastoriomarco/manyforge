"""Execution/event recording for ManyForge Composer observability surfaces."""
from __future__ import annotations

from collections import deque
from copy import deepcopy
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import heapq
from itertools import islice
import json
import logging
import os
import queue
import shutil
from pathlib import Path
import threading
import time
from typing import Any, Optional

logger = logging.getLogger(__name__)

_ALLOWED_EVENT_TYPES = {
    "cache_hit",
    "clock_anchor",
    "cycle_error",
    "cycle_started",
    "cycle_state_changed",
    "delegated_recovery_requested",
    "collision_detected",
    "controlled_stop_completed",
    "error_raised",
    "execute_completed",
    "execute_failed",
    "execute_started",
    "intervention_required",
    "kernel_error",
    "kernel_event_gap",
    "stop_decision_chain",
    "trace_pruned",
    "kernel_flight_dump",
    "kernel_state_transition",
    "node_status_transition",
    "pause_completed",
    "plan_completed",
    "plan_failed",
    "plan_started",
    "stop_completed",
    "validation_failed",
    "validation_passed",
    "validation_started",
    "recovery_offer_published",
    "recovery_action_requested",
    "recovery_action_accepted",
    "recovery_action_rejected",
}
_ALLOWED_EVENT_PREFIXES = (
    "cycle_reset_",
    "kernel_native_",
    "intervention_",
    "model_call_",
    "pose_source_",
    "runtime_override_",
    "simulation_control_",
    "tool_dispatch_",
)
# INFO-severity events whose loss would break restart continuity or violate
# spec 520 §4.3 (the act of pruning must itself survive): these fsync like
# WARN+ events despite their severity. Keep this set tiny — every entry is a
# synchronous disk barrier on the writer thread.
_DURABLE_INFO_EVENT_TYPES = frozenset(
    {
        "clock_anchor",
        "stop_decision_chain",
        "trace_pruned",
        "recovery_offer_published",
        "recovery_action_requested",
        "recovery_action_accepted",
    }
)
_RUNS_INDEX_FILENAME = "runs.index.jsonl"
# Durable events recorded before any run exists (startup clock anchor,
# startup prune evidence) land here instead of vanishing: no run file can
# receive them yet, but their loss would break restart forensics.
_NO_RUN_FILENAME = "_norun.events.jsonl"
_RUNS_INDEX_REHYDRATE_LINES = 4000
_DEFAULT_TRACE_MAX_AGE_S = 30 * 24 * 3600.0
_DEFAULT_TRACE_MAX_BYTES = 2 * 1024 * 1024 * 1024
_KERNEL_NATIVE_CHANNEL = "kernel_native"
_RUNTIME_COLLISION_FAULT_SOURCE_ID = "manyforge.kernel.runtime_collision_fault"
# OW6: fixed log-scale latency buckets (ns). Percentiles are interpolated
# within the winning bucket; the overflow bucket reports its lower bound.
_LATENCY_BUCKET_BOUNDS_NS = (
    1_000_000,       # 1 ms
    2_000_000,
    5_000_000,
    10_000_000,
    20_000_000,
    50_000_000,
    100_000_000,
    200_000_000,
    500_000_000,
    1_000_000_000,   # 1 s
    2_000_000_000,
    5_000_000_000,
    10_000_000_000,  # 10 s
)
_LATENCY_METRICS = ("stop", "plan", "validate", "execute")
# Bounded start-timestamp tracking per metric: interleaved executions are
# keyed by execution_id; the cap only matters if completions are lost.
_LATENCY_PENDING_CAP = 64
_SCENE_SNAPSHOT_LEASE_COUNTER_KEYS = (
    "accepted_bounded_divergence_total",
    "lease_expired_rejected_total",
    "stale_source_rejected_total",
    "topology_invalidated_total",
    "policy_invalidated_total",
    "target_drift_rejected_total",
    "obstacle_drift_rejected_total",
    "runtime_dynamic_not_qualified_total",
    "missing_coverage_rejected_total",
    "missing_lease_rejected_total",
    "live_snapshot_unavailable_rejected_total",
)
_SCENE_SNAPSHOT_LEASE_REASON_COUNTERS = {
    "scene_snapshot_lease_live_divergence_accepted": (
        "accepted_bounded_divergence_total"
    ),
    "scene_snapshot_lease_expired": "lease_expired_rejected_total",
    "observed_scene_source_stale": "stale_source_rejected_total",
    "scene_topology_changed_after_validation": "topology_invalidated_total",
    "collision_policy_changed_after_validation": "policy_invalidated_total",
    "target_moved_after_validation": "target_drift_rejected_total",
    "obstacle_moved_after_validation": "obstacle_drift_rejected_total",
    "runtime_dynamic_scene_not_qualified": (
        "runtime_dynamic_not_qualified_total"
    ),
    "scene_snapshot_lease_missing_collision_coverage": (
        "missing_coverage_rejected_total"
    ),
    "scene_snapshot_lease_missing": "missing_lease_rejected_total",
    "live_scene_snapshot_unavailable": (
        "live_snapshot_unavailable_rejected_total"
    ),
}


class _LatencyHistogram:
    """Fixed-bucket latency histogram fed on the recorder thread — derived
    from event timestamps the pipeline already carries, so no new hot-path
    instrumentation (observability plan OW6)."""

    __slots__ = ("counts", "count", "sum_ns", "min_ns", "max_ns")

    def __init__(self) -> None:
        self.counts = [0] * (len(_LATENCY_BUCKET_BOUNDS_NS) + 1)
        self.count = 0
        self.sum_ns = 0
        self.min_ns: Optional[int] = None
        self.max_ns: Optional[int] = None

    def observe(self, duration_ns: int) -> None:
        if duration_ns < 0:
            return
        index = len(_LATENCY_BUCKET_BOUNDS_NS)
        for i, bound in enumerate(_LATENCY_BUCKET_BOUNDS_NS):
            if duration_ns <= bound:
                index = i
                break
        self.counts[index] += 1
        self.count += 1
        self.sum_ns += duration_ns
        if self.min_ns is None or duration_ns < self.min_ns:
            self.min_ns = duration_ns
        if self.max_ns is None or duration_ns > self.max_ns:
            self.max_ns = duration_ns

    def percentile_ns(self, fraction: float) -> Optional[int]:
        if self.count == 0:
            return None
        target = fraction * self.count
        cumulative = 0
        for i, bucket_count in enumerate(self.counts):
            if bucket_count == 0:
                continue
            if cumulative + bucket_count >= target:
                lo = _LATENCY_BUCKET_BOUNDS_NS[i - 1] if i > 0 else 0
                if i >= len(_LATENCY_BUCKET_BOUNDS_NS):
                    return _LATENCY_BUCKET_BOUNDS_NS[-1]
                hi = _LATENCY_BUCKET_BOUNDS_NS[i]
                within = (target - cumulative) / bucket_count
                return int(lo + (hi - lo) * within)
            cumulative += bucket_count
        return self.max_ns

    def to_payload(self) -> dict[str, Any]:
        return {
            "count": self.count,
            "sum_ns": self.sum_ns,
            "min_ns": self.min_ns,
            "max_ns": self.max_ns,
            "p50_ns": self.percentile_ns(0.50),
            "p95_ns": self.percentile_ns(0.95),
            "p99_ns": self.percentile_ns(0.99),
            "bucket_bounds_ns": list(_LATENCY_BUCKET_BOUNDS_NS),
            "bucket_counts": list(self.counts),
        }


# Incident-embed proximity window (recorder event indices): a chain older
# than this many recorder events is never attached by the channel fallback.
_STOP_DECISION_EMBED_WINDOW = 500
_STOP_TERMINAL_STATES = frozenset({"Stopped", "Interrupted", "InterventionRequired"})
_EPISODE_ABANDON_STATES = frozenset(
    {"Executing", "Planning", "Validating", "Idle", "Completed"}
)
# Curated subset of stop/preempt detail keys carried into the decision chain.
_STOP_CHAIN_DETAIL_KEYS = (
    "requested_stop_mode",
    "executed_stop_mode",
    "controlled_stop_success",
    "emergency_fallback",
    "hard_stop_transitioned",
    "stop_command_source",
)
_KERNEL_NATIVE_CHANNEL_LIMIT = 2000
_KERNEL_NATIVE_ERROR_TYPES = {
    "error_raised",
    "collision_detected",
    "motion_trip_failed",
}
_KERNEL_NATIVE_WARN_TYPES = {
    "interrupt_latched",
    "interrupt_preempt_applied",
    "interrupt_dropped_queue_overflow",
    "interrupt_ingest_overflow",
    "stop_requested",
    "stop_issued",
    "motion_trip_secondary_signal",
}


def _stable_recovery_context_id(
    *,
    run_id: Optional[str],
    execution_id: int,
    latch_id: Optional[str] = None,
    evidence_refs: Any = None,
    offer_id: Optional[str] = None,
) -> Optional[str]:
    """Return one stable recorder-level join key for recovery evidence."""
    identity = ""
    if execution_id > 0:
        identity = f"execution:{execution_id}"
    elif latch_id:
        identity = f"latch:{latch_id}"
    elif isinstance(evidence_refs, (list, tuple)):
        # RuntimePauseFault ids are opaque, generation-scoped identities shared
        # by the immutable offer and every action event. They are the strongest
        # cycle-only join key when no kernel execution or latch exists.
        runtime_fault_refs = sorted(
            {
                ref[:256]
                for ref in evidence_refs
                if isinstance(ref, str) and ref.startswith("runtime_fault:")
            }
        )
        if runtime_fault_refs:
            identity = f"evidence:{runtime_fault_refs[0]}"
    if not identity and offer_id:
        # A non-runtime cycle offer may have neither kernel nor fault identity.
        # Its immutable offer id still joins its own requested/result records
        # without fabricating a relation to an incident or stop episode.
        identity = f"offer:{offer_id[:256]}"
    if not identity:
        return None
    material = f"manyforge.recovery-context.v1\0{run_id or '_norun'}\0{identity}"
    digest = hashlib.sha256(material.encode("utf-8")).hexdigest()[:24]
    return f"recovery-context-{digest}"


def _stable_stop_decision_id(
    *,
    recovery_context_id: str,
    terminal_state: str,
    terminal_kernel_sequence: str,
) -> str:
    material = (
        "manyforge.stop-decision.v1\0"
        f"{recovery_context_id}\0{terminal_state}\0{terminal_kernel_sequence}"
    )
    digest = hashlib.sha256(material.encode("utf-8")).hexdigest()[:24]
    return f"stop-decision-{digest}"

_SEVERITY_ORDER = {"DEBUG": 10, "INFO": 20, "WARN": 30, "WARNING": 30, "ERROR": 40, "CRITICAL": 50}
_FAILURE_ARCHIVE_EVENT_TYPES = {
    "cycle_error",
    "kernel_flight_dump",
    "execute_failed",
    "intervention_required",
    "plan_failed",
    "validation_failed",
}
_FAILURE_ARCHIVE_RECENT_EVENT_LIMIT = 1000
_FAILURE_ARCHIVE_QUEUE_SIZE = 16
# Audit 6B: bounded JSONL writer queue (events, small strings). Sized for
# bursts; overflow drops from the file sink only and is counted.
_JSONL_QUEUE_SIZE = 1024
# Audit 6B: minimum wall-clock interval between full observe() state scans
# from polling callers. Half the fastest UI poll cadence (100 ms), so
# pollers see at most one-interval-stale events while the scan+record cost
# is paid at a bounded rate.
_OBSERVE_MIN_INTERVAL_S = 0.05
_DEFAULT_FAILURE_ARCHIVE_MAX_AGE_S = 30 * 24 * 60 * 60
_DEFAULT_FAILURE_ARCHIVE_MAX_BYTES = 1024 * 1024 * 1024
_DEFAULT_HISTORY_LIMIT = 1000
_MAX_HISTORY_LIMIT = 10000
_DEFAULT_CHANNEL_EVENT_LIMIT = 300
_DEFAULT_INCIDENT_SNAPSHOT_LIMIT = 32
_INCIDENT_ALL_EVENT_LIMIT = 200
_INCIDENT_CHANNEL_EVENT_LIMIT = 80
_EVENT_CHANNEL_LIMITS = {
    "controlled_stop": 500,
    "stop_decision": 200,
    _KERNEL_NATIVE_CHANNEL: _KERNEL_NATIVE_CHANNEL_LIMIT,
    "fault_safety_stop": 1000,
    "command_runtime": 800,
    "execution_lifecycle": 1000,
    "pose_source": 300,
    "telemetry_debug": 500,
}
_NATIVE_KERNEL_ERROR_EVENT_TYPES_TO_RECORD = {
    "error_raised",
    "collision_detected",
}
_NATIVE_KERNEL_RESUME_AUDIT_EVENT_TYPES_TO_RECORD = {
    "plan_started",
    "plan_completed",
    "validation_started",
    "validation_passed",
    "validation_failed",
}
_NATIVE_KERNEL_CONTROLLED_STOP_SUCCESS_EVENT_TYPES_TO_RECORD = {
    "interrupt_preempt_applied",
    "motion_trip_completed",
    "stop_issued",
}
_GENERIC_TERMINAL_MESSAGES = {
    "Motion execution ended in stopped",
    "Motion execution ended in interrupted",
    "Motion execution ended in failed",
    "Motion execution rejected",
}


def _is_resume_audit_native_event(native_event: dict[str, Any]) -> bool:
    details = native_event.get("details")
    if not isinstance(details, dict):
        return False
    return (
        details.get("reason") == "resume_replan"
        or str(details.get("resume_replan", "")).lower() == "true"
    )


def _is_controlled_stop_success_native_event(native_event: dict[str, Any]) -> bool:
    if (
        native_event.get("type")
        not in _NATIVE_KERNEL_CONTROLLED_STOP_SUCCESS_EVENT_TYPES_TO_RECORD
    ):
        return False
    details = native_event.get("details")
    return (
        isinstance(details, dict)
        and str(details.get("controlled_stop_success", "")).lower() == "true"
    )


def _wall_timestamp_iso() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


_EPOCH_UTC = datetime(1970, 1, 1, tzinfo=timezone.utc)


def _wall_iso_to_ns(value: Any) -> Optional[int]:
    """Parse an ISO-8601 wall timestamp (as emitted by _wall_timestamp_iso)
    into epoch nanoseconds. Returns None for anything unparseable.

    Lexicographic comparison of these strings is unsafe — fractional seconds
    are variable-width — so wall-window filtering parses to an integer instead
    of comparing text. Computed via timedelta (not float * 1e9) so the result
    is exact to the microsecond resolution the strings actually carry.
    """
    if not isinstance(value, str) or not value:
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


# Cross-run query (OW7): bound how many index runs a single wall-window query
# will scan, independent of the per-query event limit.
_MAX_CROSS_RUN_RUNS = 2000


def _run_overlaps_wall_window(
    started_ns: Optional[int],
    effective_end_ns: Optional[int],
    from_wall_ns: Optional[int],
    to_wall_ns: Optional[int],
) -> bool:
    """True if a run's wall territory can intersect the window.

    Only the start-after-window case is safe to reject from the run index.
    The upper bound is advisory only: wall clocks can step backward between
    runs, and native/kernel evidence can be explicitly attributed to an older
    run after a newer run starts. Per-event wall filtering is therefore the
    authority for upper bounds; pruning on effective_end would silently drop
    evidence (P4). A run with no parseable start is not excluded."""
    _ = effective_end_ns
    if (
        to_wall_ns is not None
        and started_ns is not None
        and started_ns > to_wall_ns
    ):
        return False
    return True


def _event_in_wall_window(
    event: dict[str, Any],
    from_wall_ns: Optional[int],
    to_wall_ns: Optional[int],
) -> bool:
    """True if an event's wall timestamp falls within the window. Events whose
    wall time cannot be parsed are included rather than dropped — they come
    from a run the index already placed in-window, and silently discarding
    evidence would violate loud-loss (P4)."""
    wall_ns = _wall_iso_to_ns(
        event.get("timestamp_wall") or event.get("timestamp")
    )
    if wall_ns is None:
        return True
    if from_wall_ns is not None and wall_ns < from_wall_ns:
        return False
    if to_wall_ns is not None and wall_ns > to_wall_ns:
        return False
    return True


def _event_wall_sort_ns(
    event: dict[str, Any],
    from_wall_ns: Optional[int],
    to_wall_ns: Optional[int],
) -> int:
    """Sort key for cross-run wall pages.

    An unparseable event wall stamp is still evidence accepted by the run-index
    window. Keep it at the recent edge of the requested window instead of
    giving it a zero timestamp that would be evicted first from the bounded
    heap.
    """
    wall_ns = _wall_iso_to_ns(
        event.get("timestamp_wall") or event.get("timestamp")
    )
    if wall_ns is not None:
        return wall_ns
    _ = from_wall_ns
    if to_wall_ns is not None:
        return to_wall_ns
    return (1 << 63) - 1


def _validate_event_type(event_type: str) -> None:
    if event_type in _ALLOWED_EVENT_TYPES:
        return
    if any(event_type.startswith(prefix) for prefix in _ALLOWED_EVENT_PREFIXES):
        return
    raise ValueError(f"Unsupported event type: {event_type}")


def _severity_for_event(event_type: str, details: Optional[dict[str, Any]]) -> str:
    if event_type in {
        "cycle_error",
        "kernel_error",
        "kernel_flight_dump",
        "intervention_required",
        "error_raised",
        "collision_detected",
    }:
        return "ERROR"
    if event_type == "kernel_event_gap":
        return "WARN"
    if event_type == "recovery_action_rejected":
        return "WARN"
    if event_type.startswith("kernel_native_"):
        native_type = event_type[len("kernel_native_"):]
        if native_type in _KERNEL_NATIVE_ERROR_TYPES:
            return "ERROR"
        if native_type in _KERNEL_NATIVE_WARN_TYPES:
            return "WARN"
        return "INFO"
    if event_type.endswith("_failed"):
        return "ERROR"
    if details and details.get("error"):
        return "ERROR"
    return "INFO"


def _component_for_event(event_type: str) -> str:
    if event_type.startswith("model_call_"):
        return "manyforge.model"
    if event_type.startswith("tool_dispatch_"):
        return "manyforge.tool_dispatch"
    if event_type.startswith("pose_source_"):
        return "manyforge.scene"
    if event_type.startswith("simulation_control_"):
        return "manyforge.simulation"
    if event_type.startswith("runtime_override_"):
        return "manyforge.runtime_override"
    if event_type.startswith("intervention_"):
        return "manyforge.intervention"
    if event_type == "controlled_stop_completed":
        return "manyforge.execution"
    if event_type.startswith("cycle_"):
        return "manyforge.cycle"
    if event_type.startswith("recovery_"):
        return "manyforge.cycle_recovery"
    if event_type.startswith("kernel_") or event_type in {
        "plan_started",
        "plan_completed",
        "plan_failed",
        "validation_started",
        "validation_passed",
        "validation_failed",
        "execute_started",
        "execute_completed",
        "execute_failed",
        "pause_completed",
        "stop_completed",
        "cache_hit",
        "error_raised",
        "collision_detected",
    }:
        return "manyforge.execution"
    if event_type == "node_status_transition":
        return "manyforge.behavior_tree"
    if event_type == "delegated_recovery_requested":
        return "manyforge.recovery"
    return "manyforge.runtime"


def _message_for_event(
    event_type: str,
    *,
    details: Optional[dict[str, Any]],
    node: Optional[str],
    previous_status: Optional[str],
    current_status: Optional[str],
) -> str:
    if event_type == "node_status_transition" and node is not None:
        return f"{node}: {previous_status or 'UNKNOWN'} -> {current_status or 'UNKNOWN'}"
    if event_type in {"cycle_state_changed", "kernel_state_transition"} and details:
        return (
            f"{event_type}: {details.get('from', 'unknown')} -> "
            f"{details.get('to', 'unknown')}"
        )
    if details and isinstance(details.get("error"), str) and details["error"]:
        return details["error"]
    return event_type.replace("_", " ")


def _event_channel(event_type: str, details: Optional[dict[str, Any]]) -> str:
    """Classify an event into an independently retained diagnostic stream."""
    details = details or {}
    if event_type.startswith("kernel_native_") or event_type == "kernel_event_gap":
        return _KERNEL_NATIVE_CHANNEL
    if event_type == "stop_decision_chain":
        return "stop_decision"
    if (
        event_type == "controlled_stop_completed"
        or str(details.get("controlled_stop_success", "")).lower() == "true"
    ):
        return "controlled_stop"
    stop_keys = {
        "requested_stop_mode",
        "executed_stop_mode",
        "fallback_pair",
        "fallback_reason",
        "hard_stop_transitioned",
        "controlled_stop_error_code",
        "controlled_stop_error_reason",
    }
    if (
        event_type in {
            "cycle_error",
            "kernel_error",
            "intervention_required",
            "error_raised",
            "collision_detected",
        }
        or event_type.endswith("_failed")
        or any(key in details for key in stop_keys)
    ):
        return "fault_safety_stop"
    if event_type.startswith("pose_source_"):
        return "pose_source"
    if event_type.startswith("model_call_"):
        return "telemetry_debug"
    if (
        event_type.startswith("runtime_override_")
        or event_type.startswith("simulation_control_")
        or event_type.startswith("intervention_")
        or event_type == "delegated_recovery_requested"
        or event_type.startswith("cycle_reset_")
    ):
        return "command_runtime"
    return "execution_lifecycle"


def _event_should_create_incident(
    event_type: str,
    details: Optional[dict[str, Any]],
    severity: str,
    channel: str,
) -> bool:
    severity_value = _SEVERITY_ORDER.get(severity.upper(), 0)
    return (
        severity_value >= _SEVERITY_ORDER["WARN"]
        or channel == "fault_safety_stop"
        or event_type in _FAILURE_ARCHIVE_EVENT_TYPES
    )


@dataclass
class _RunInfo:
    run_id: str
    started_at_ns: int
    file_path: Optional[Path] = None


@dataclass(frozen=True)
class _StoredEvent:
    event_index: int
    run_id: Optional[str]
    event_type: str
    json_text: str


@dataclass(frozen=True)
class _ArchiveRecord:
    path: Path
    modified_at_s: float
    size_bytes: int


@dataclass(frozen=True)
class _FailureArchiveJob:
    event: dict[str, Any]
    run_id: Optional[str]
    trace_file: Optional[Path]
    recent_events: list[dict[str, Any]]


class EventRecorder:
    """Record execution/cycle/node events with bounded retention and JSONL sink."""

    def __init__(
        self,
        *,
        trace_dir: str | Path | None = None,
        max_events: int = 4096,
        failure_archive_dir: str | Path | None = None,
        failure_archive_enabled: bool = True,
        failure_archive_max_age_s: float | None = None,
        failure_archive_max_bytes: int | None = None,
        observe_min_interval_s: float | None = None,
        trace_max_age_s: float | None = None,
        trace_max_bytes: int | None = None,
    ) -> None:
        self._trace_dir = Path(trace_dir) if trace_dir is not None else None
        if self._trace_dir is not None:
            self._trace_dir.mkdir(parents=True, exist_ok=True)
        self._failure_archive_dir: Path | None = None
        if failure_archive_enabled:
            if failure_archive_dir is not None:
                self._failure_archive_dir = Path(failure_archive_dir)
            elif self._trace_dir is not None:
                self._failure_archive_dir = self._trace_dir / "failure-archives"
        if self._failure_archive_dir is not None:
            self._failure_archive_dir.mkdir(parents=True, exist_ok=True)
        self._failure_archive_max_age_s = (
            failure_archive_max_age_s
            if failure_archive_max_age_s is not None
            else (
                float(_DEFAULT_FAILURE_ARCHIVE_MAX_AGE_S)
                if self._failure_archive_dir is not None
                else None
            )
        )
        self._failure_archive_max_bytes = (
            failure_archive_max_bytes
            if failure_archive_max_bytes is not None
            else (
                _DEFAULT_FAILURE_ARCHIVE_MAX_BYTES
                if self._failure_archive_dir is not None
                else None
            )
        )

        # OW5 trace retention budgets (None = disabled, e.g. no trace dir).
        self._trace_max_age_s = (
            trace_max_age_s
            if trace_max_age_s is not None
            else (_DEFAULT_TRACE_MAX_AGE_S if self._trace_dir is not None else None)
        )
        self._trace_max_bytes = (
            trace_max_bytes
            if trace_max_bytes is not None
            else (_DEFAULT_TRACE_MAX_BYTES if self._trace_dir is not None else None)
        )
        self._last_trace_prune: dict[str, Any] = {}
        self._run_index_severities: dict[str, str] = {}
        self._current_run_event_count = 0
        self._current_run_max_severity = "INFO"
        self._current_run_program_run_id: Optional[str] = None

        self._events: deque[_StoredEvent] = deque(maxlen=max_events)
        self._decoded_recent_events: deque[dict[str, Any]] = deque(
            maxlen=min(max_events, _INCIDENT_ALL_EVENT_LIMIT)
        )
        self._events_by_channel: dict[str, deque[_StoredEvent]] = {
            channel: deque(maxlen=limit)
            for channel, limit in _EVENT_CHANNEL_LIMITS.items()
        }
        self._decoded_events_by_channel: dict[str, deque[dict[str, Any]]] = {
            channel: deque(maxlen=limit)
            for channel, limit in _EVENT_CHANNEL_LIMITS.items()
        }
        self._incident_snapshots: deque[dict[str, Any]] = deque(
            maxlen=_DEFAULT_INCIDENT_SNAPSHOT_LIMIT
        )
        self._event_index = 0
        self._run_counter = 0
        self._current_run: Optional[_RunInfo] = None
        self._last_run: Optional[_RunInfo] = None

        self._last_cycle_state: Optional[str] = None
        self._last_kernel_state: Optional[str] = None
        self._last_execution_id: int = 0
        self._last_active_move_id: str = ""
        self._last_kernel_error: Optional[str] = None
        self._last_cycle_error: Optional[str] = None
        self._last_node_statuses: dict[str, str] = {}
        self._last_intervention_session_id: Optional[str] = None
        self._last_intervention_event_index: int = 0
        self._last_recorded_native_kernel_event_index: int = 0
        self._last_failure_archive_id: Optional[str] = None
        self._last_failure_archive_path: Optional[Path] = None
        self._failure_archive_count = 0
        self._failure_archive_dropped = 0
        self._last_archive_error: Optional[str] = None
        self._last_archive_cleanup: dict[str, Any] = {
            "deleted_count": 0,
            "deleted_bytes": 0,
            "reason": None,
            "timestamp_ns": None,
        }

        # Lock order: EventRecorder.observe() may hold this lock while reading
        # CycleManager snapshots. Do not call observe() while holding
        # CycleManager._lock; use recorder -> cycle-manager order only.
        self._lock = threading.RLock()
        self._failure_archive_queue: queue.Queue[_FailureArchiveJob | None] | None = None
        self._failure_archive_worker: threading.Thread | None = None
        if self._failure_archive_dir is not None:
            self._failure_archive_queue = queue.Queue(maxsize=_FAILURE_ARCHIVE_QUEUE_SIZE)
            self._failure_archive_worker = threading.Thread(
                target=self._failure_archive_worker_loop,
                name="manyforge-failure-archive",
                daemon=True,
            )
            self._failure_archive_worker.start()

        # Audit 6B (finding 2.3): per-run JSONL appends run on a dedicated
        # writer thread so observers never stall on disk latency inside the
        # `_lock` critical section. Serialization still happens under the
        # lock (ordering is fixed at enqueue time); only the file I/O is
        # asynchronous. Bounded queue: on overflow the event is dropped from
        # the FILE sink only (the in-memory deque and failure archive still
        # see it) and the drop is counted, never blocking the hot path.
        self._jsonl_queue: queue.Queue[tuple[Path, str, bool] | None] | None = None
        self._jsonl_worker: threading.Thread | None = None
        self._jsonl_dropped = 0
        self._last_jsonl_error: Optional[str] = None
        if self._trace_dir is not None:
            self._jsonl_queue = queue.Queue(maxsize=_JSONL_QUEUE_SIZE)
            self._jsonl_worker = threading.Thread(
                target=self._jsonl_writer_loop,
                name="manyforge-jsonl-writer",
                daemon=True,
            )
            self._jsonl_worker.start()

        # Audit 6B (finding 2.16): observe() is called from every polling
        # HTTP handler; rate-limit the full state scan by wall-clock budget.
        # Exactness-sensitive callers (debug export, recovery correlation)
        # pass force=True.
        self._observe_min_interval_s = (
            observe_min_interval_s
            if observe_min_interval_s is not None
            else _OBSERVE_MIN_INTERVAL_S
        )
        self._last_observe_monotonic = 0.0
        # OW1 loss-visibility state: last kernel dispatch sequence seen, the
        # last pipeline drop totals, and the full-fidelity dedup cursor.
        self._last_kernel_sequence = 0
        self._last_pipeline_drop_totals = (0, 0)
        self._last_full_fidelity_kernel_event_index = 0
        self._kernel_gap_records_total = 0
        self._last_kernel_gap_wall: Optional[str] = None
        # OW4 stop-decision assembler: one open "stop episode" at a time,
        # built from the full-fidelity native event stream and closed into a
        # durable stop_decision_chain record on a terminal transition.
        self._stop_episode: Optional[dict[str, Any]] = None
        self._last_stop_decision: Optional[dict[str, Any]] = None
        # OW6 per-run latency histograms + bounded pending-start tracking.
        self._latency_histograms: dict[str, _LatencyHistogram] = {
            metric: _LatencyHistogram() for metric in _LATENCY_METRICS
        }
        self._latency_pending: dict[str, dict[int, int]] = {
            metric: {} for metric in ("plan", "validate", "execute")
        }
        self._last_run_latency: Optional[dict[str, Any]] = None
        self._latency_run_id: Optional[str] = None
        # RT-7E lease accounting is recorder-thread derived from the deduped
        # native event stream. It is evidence, not admission authority.
        self._scene_snapshot_lease_stats: dict[str, int] = {
            key: 0 for key in _SCENE_SNAPSHOT_LEASE_COUNTER_KEYS
        }
        self._last_scene_snapshot_lease_reason: Optional[str] = None
        self._last_scene_snapshot_lease_wall: Optional[str] = None
        self._stop_chain_execution_marker: Optional[tuple[int, str]] = None
        # Monotonic<->wall clock anchor for kernel-stamped timestamps
        # (observability plan OW2). None until the kernel client provides one;
        # without it, kernel-event wall times fall back to receipt time.
        self._clock_anchor: Optional[tuple[int, int]] = None
        # OW5: restart continuity — recover last_run_id, the run counter, and
        # per-run severity knowledge from the append-only run index so
        # post-restart forensics does not need out-of-band run ids and
        # synthesized ids cannot collide with pre-restart ones.
        self._rehydrate_from_run_index()
        self._enqueue_trace_prune()

    def _runs_index_path(self) -> Optional[Path]:
        if self._trace_dir is None:
            return None
        return self._trace_dir / _RUNS_INDEX_FILENAME

    def _rehydrate_from_run_index(self) -> None:
        index_path = self._runs_index_path()
        if index_path is None or not index_path.exists():
            return
        try:
            entries = self._tail_jsonl(index_path, _RUNS_INDEX_REHYDRATE_LINES)
        except OSError:
            return
        last_run_id: Optional[str] = None
        max_counter = 0
        for entry in entries:
            run_id = str(entry.get("run_id") or "")
            if not run_id:
                continue
            kind = entry.get("kind")
            if kind == "run_started":
                last_run_id = run_id
                max_counter = max(max_counter, int(entry.get("run_counter") or 0))
            elif kind == "run_ended":
                severity = str(entry.get("max_severity") or "INFO")
                self._run_index_severities[run_id] = severity
        if last_run_id is not None:
            self._last_run = _RunInfo(
                run_id=last_run_id,
                started_at_ns=0,
                file_path=self._path_for_run(last_run_id),
            )
        self._run_counter = max(self._run_counter, max_counter)

    def _append_run_index_line(self, payload: dict[str, Any]) -> None:
        index_path = self._runs_index_path()
        queue_ = self._jsonl_queue
        if index_path is None or queue_ is None:
            return
        try:
            # Index lines are rare and load-bearing for rehydration: durable.
            queue_.put_nowait((index_path, json.dumps(payload, sort_keys=True), True))
        except queue.Full:
            self._jsonl_dropped += 1
            self._last_jsonl_error = "jsonl writer queue full"

    def list_runs(self, limit: int = 200) -> list[dict[str, Any]]:
        """Most-recent-first run listing from the append-only run index."""
        index_path = self._runs_index_path()
        if index_path is None or not index_path.exists():
            return []
        entries = self._tail_jsonl(index_path, _RUNS_INDEX_REHYDRATE_LINES)
        runs: dict[str, dict[str, Any]] = {}
        order: list[str] = []
        for entry in entries:
            run_id = str(entry.get("run_id") or "")
            if not run_id:
                continue
            if run_id not in runs:
                runs[run_id] = {"run_id": run_id}
                order.append(run_id)
            runs[run_id].update(
                {k: v for k, v in entry.items() if k not in {"kind"}}
            )
        return [runs[r] for r in reversed(order[-limit:])]

    def set_clock_anchor(self, monotonic_ns: int, wall_time_ns: int) -> None:
        """Install the kernel monotonic<->wall anchor and record it durably.

        `monotonic_ns` must come from the same IClock instance that stamps
        kernel events (the native bundle samples both clocks back-to-back).
        Kernel-stamped event wall times are then derived as
        `anchor_wall + (event_mono - anchor_mono)` instead of being skewed by
        drain latency.
        """
        with self._lock:
            self._clock_anchor = (int(monotonic_ns), int(wall_time_ns))
        self._record_event(
            event_type="clock_anchor",
            execution_id=0,
            details={
                "monotonic_ns": str(int(monotonic_ns)),
                "wall_time_ns": str(int(wall_time_ns)),
            },
            system_event=True,
        )

    def _wall_iso_from_kernel_ns(self, kernel_monotonic_ns: int) -> Optional[str]:
        with self._lock:
            anchor = self._clock_anchor
        if anchor is None or kernel_monotonic_ns <= 0:
            return None
        wall_ns = anchor[1] + (int(kernel_monotonic_ns) - anchor[0])
        return (
            datetime.fromtimestamp(wall_ns / 1e9, tz=timezone.utc)
            .isoformat()
            .replace("+00:00", "Z")
        )

    def ingest_kernel_flight_dumps(self, flight_dir: str) -> int:
        """Ingest fail-closed kernel flight dumps left by a previous process.

        Each `flight-*.jsonl` becomes one durable ERROR `kernel_flight_dump`
        event (which also triggers a failure archive), then the file is
        renamed `*.ingested` so restarts are idempotent. Returns the number
        of dumps ingested. Malformed files are ingested with a parse-error
        note rather than skipped — a torn dump from a dying kernel is still
        evidence.
        """
        ingested = 0
        try:
            entries = sorted(os.listdir(flight_dir))
        except OSError:
            return 0
        for name in entries:
            if not name.startswith("flight-") or not name.endswith(".jsonl"):
                continue
            path = os.path.join(flight_dir, name)
            details: dict[str, Any] = {"dump_path": path}
            try:
                with open(path, "r", encoding="utf-8") as handle:
                    header_line = handle.readline().strip()
                    record_count = sum(1 for _ in handle)
                header = json.loads(header_line) if header_line else {}
                details["reason"] = str(header.get("reason") or "unknown")
                details["records"] = str(record_count)
                details["dumped_wall_ns"] = str(header.get("dumped_wall_ns") or 0)
                details["torn"] = str(header.get("torn"))
                details["schema_version"] = str(
                    header.get("schema_version") or "unknown"
                )
            except (OSError, ValueError) as exc:
                details["parse_error"] = str(exc)
            self._record_event(
                event_type="kernel_flight_dump",
                execution_id=0,
                details=details,
            )
            try:
                os.replace(path, path + ".ingested")
            except OSError:
                logger.warning(
                    "failed to rename ingested flight dump", extra={"path": path}
                )
            ingested += 1
        return ingested

    @property
    def current_run_id(self) -> Optional[str]:
        with self._lock:
            return self._current_run.run_id if self._current_run is not None else None

    @property
    def last_run_id(self) -> Optional[str]:
        with self._lock:
            if self._current_run is not None:
                return self._current_run.run_id
            return self._last_run.run_id if self._last_run is not None else None

    def current_run_started_at_ns(self) -> Optional[int]:
        with self._lock:
            return self._current_run.started_at_ns if self._current_run is not None else None

    def failure_archive_status(self) -> dict[str, Any]:
        """Return failure-archive health and retention status for supervisors."""
        with self._lock:
            records = self._archive_records_locked()
            return {
                "enabled": self._failure_archive_dir is not None,
                "path": str(self._failure_archive_dir) if self._failure_archive_dir is not None else None,
                "archive_count": len(records),
                "total_bytes": sum(record.size_bytes for record in records),
                "max_age_s": self._failure_archive_max_age_s,
                "max_bytes": self._failure_archive_max_bytes,
                "last_archive_id": self._last_failure_archive_id,
                "last_archive_path": (
                    str(self._last_failure_archive_path)
                    if self._last_failure_archive_path is not None
                    else None
                ),
                "last_archive_error": self._last_archive_error,
                "last_cleanup": dict(self._last_archive_cleanup),
                "queue_depth": (
                    self._failure_archive_queue.qsize()
                    if self._failure_archive_queue is not None
                    else 0
                ),
                "queue_capacity": (
                    _FAILURE_ARCHIVE_QUEUE_SIZE
                    if self._failure_archive_queue is not None
                    else 0
                ),
                "dropped_archives": self._failure_archive_dropped,
                "worker_alive": (
                    self._failure_archive_worker.is_alive()
                    if self._failure_archive_worker is not None
                    else False
                ),
                # Audit 6B: JSONL sink health (async writer thread). Lives on
                # the same recorder-health surface supervisors already poll.
                "jsonl_worker_alive": (
                    self._jsonl_worker.is_alive()
                    if self._jsonl_worker is not None
                    else False
                ),
                "jsonl_dropped": self._jsonl_dropped,
                "last_jsonl_error": self._last_jsonl_error,
            }

    def observe(self, state, *, force: bool = False) -> None:
        """Poll the current ComposerState and record semantic deltas.

        Polling callers (history/digest routes, websocket pump) are
        rate-limited to one full scan per `_OBSERVE_MIN_INTERVAL_S`;
        exactness-sensitive callers (debug export, recovery correlation)
        pass ``force=True`` to bypass the budget (audit 6B, finding 2.16).
        """
        with self._lock:
            now = time.monotonic()
            if not force:
                if now - self._last_observe_monotonic < self._observe_min_interval_s:
                    return
            self._last_observe_monotonic = now
            cycle_manager = getattr(state, "cycle_manager", None)
            kernel = getattr(state, "_kernel", None)

            cycle_state = cycle_manager.state.value if cycle_manager is not None else None
            if cycle_state != self._last_cycle_state:
                self._handle_cycle_state_transition(
                    self._last_cycle_state,
                    cycle_state,
                    runner_run_id=getattr(cycle_manager, "program_run_id", None),
                )
                self._last_cycle_state = cycle_state

            status = None
            snapshot = None
            if kernel is not None:
                status = (
                    cycle_manager.get_status()
                    if cycle_manager is not None
                    else kernel.get_status()
                )
                snapshot = (
                    cycle_manager.get_execution_snapshot()
                    if cycle_manager is not None
                    else kernel.get_execution_snapshot()
                )
                self._handle_kernel_transition(status, snapshot)

            if cycle_manager is not None:
                self._handle_intervention_events(cycle_manager.intervention)
                self._handle_node_status_changes(
                    cycle_manager.get_node_statuses(),
                    status,
                    cycle_state=cycle_state,
                    intervention_session=cycle_manager.intervention,
                )
                error_reader = getattr(cycle_manager, "get_error_snapshot", None)
                if callable(error_reader):
                    cycle_error, cycle_error_code, cycle_error_details = error_reader()
                else:
                    cycle_error = cycle_manager.error
                    cycle_error_code = getattr(cycle_manager, "error_code", None)
                    cycle_error_details = getattr(cycle_manager, "error_details", None)
                if cycle_error != self._last_cycle_error and cycle_error:
                    event_details: dict[str, Any] = {"error": cycle_error}
                    if cycle_error_code:
                        event_details["error_code"] = str(cycle_error_code)
                    if isinstance(cycle_error_details, dict) and cycle_error_details:
                        event_details["error_details"] = deepcopy(cycle_error_details)
                    self._record_event(
                        event_type="cycle_error",
                        execution_id=status.execution_id if status is not None else 0,
                        move_id=status.active_move_id if status is not None else None,
                        details=event_details,
                        session_id=(
                            cycle_manager.intervention.session_id
                            if cycle_state == "direct_control"
                            and cycle_manager.intervention is not None
                            else None
                        ),
                        session_kind=(
                            cycle_manager.intervention.session_kind
                            if cycle_state == "direct_control"
                            and cycle_manager.intervention is not None
                            else None
                        ),
                        authority_source=(
                            cycle_manager.intervention.authority_source
                            if cycle_state == "direct_control"
                            and cycle_manager.intervention is not None
                            else None
                        ),
                        principal_id=(
                            getattr(cycle_manager.intervention, "principal_id", None)
                            if cycle_state == "direct_control"
                            and cycle_manager.intervention is not None
                            else None
                        ),
                        grant_type=(
                            getattr(cycle_manager.intervention, "grant_type", None)
                            if cycle_state == "direct_control"
                            and cycle_manager.intervention is not None
                            else None
                        ),
                        parent_run_id=(
                            cycle_manager.intervention.parent_run_id
                            if cycle_state == "direct_control"
                            and cycle_manager.intervention is not None
                            else None
                        ),
                        trigger_reason=(
                            cycle_manager.intervention.trigger_reason
                            if cycle_state == "direct_control"
                            and cycle_manager.intervention is not None
                            else None
                        ),
                        trigger_node_instance=(
                            cycle_manager.intervention.trigger_node_instance
                            if cycle_state == "direct_control"
                            and cycle_manager.intervention is not None
                            else None
                        ),
                        execution_mode="intervention" if cycle_state == "direct_control" else None,
                        run_id=(
                            self._intervention_run_id(cycle_manager.intervention)
                            if cycle_state == "direct_control"
                            and cycle_manager.intervention is not None
                            else None
                        ),
                    )
                self._last_cycle_error = cycle_error

    def get_events_since(
        self,
        event_index: int,
        *,
        include_node_transitions: bool = False,
    ) -> list[dict[str, Any]]:
        with self._lock:
            result = [
                self._decode_stored_event(event)
                for event in self._events
                if event.event_index > event_index
                and (include_node_transitions or event.event_type != "node_status_transition")
            ]
        return result

    def channel_histories(
        self,
        *,
        limit: Optional[int] = None,
        include_node_transitions: bool = False,
    ) -> dict[str, list[dict[str, Any]]]:
        """Return protected per-channel event windows.

        These rings are intentionally independent from the short chronological
        history so high-volume pose-source traffic cannot evict fault/stop
        causality.
        """
        effective_limit = (
            _DEFAULT_CHANNEL_EVENT_LIMIT
            if limit is None
            else max(0, min(int(limit), _MAX_HISTORY_LIMIT))
        )
        with self._lock:
            result: dict[str, list[dict[str, Any]]] = {}
            for channel, events in self._events_by_channel.items():
                decoded = [
                    self._decode_stored_event(event)
                    for event in events
                    if include_node_transitions
                    or event.event_type != "node_status_transition"
                ]
                result[channel] = [] if effective_limit == 0 else decoded[-effective_limit:]
            return result

    def incident_snapshots(self, *, limit: Optional[int] = None) -> list[dict[str, Any]]:
        """Return recent incident snapshots newest-last."""
        effective_limit = (
            _DEFAULT_INCIDENT_SNAPSHOT_LIMIT
            if limit is None
            else max(0, min(int(limit), _DEFAULT_INCIDENT_SNAPSHOT_LIMIT))
        )
        with self._lock:
            snapshots = [dict(snapshot) for snapshot in self._incident_snapshots]
        return [] if effective_limit == 0 else snapshots[-effective_limit:]

    def _observe_latency_event_locked(
        self, native_type: str, native_event: dict[str, Any]
    ) -> None:
        """Feed plan/validate/execute duration histograms from lifecycle
        event pairs (started -> completed), keyed by execution_id so
        interleaved executions cannot cross-contaminate. Only kernel-stamped
        timestamps are trusted for durations."""
        timestamp_ns = int(native_event.get("timestamp_ns") or 0)
        if timestamp_ns <= 0:
            return
        execution_id = int(native_event.get("execution_id") or 0)
        starts = {
            "plan_started": "plan",
            "validation_started": "validate",
            "execute_started": "execute",
        }
        completions = {
            "plan_completed": "plan",
            "validation_passed": "validate",
            "validation_failed": "validate",
            "execute_completed": "execute",
        }
        metric = starts.get(native_type)
        if metric is not None:
            pending = self._latency_pending[metric]
            if len(pending) >= _LATENCY_PENDING_CAP:
                pending.pop(next(iter(pending)))
            pending[execution_id] = timestamp_ns
            return
        metric = completions.get(native_type)
        if metric is not None:
            started = self._latency_pending[metric].pop(execution_id, None)
            if started is not None and timestamp_ns >= started:
                self._latency_histograms[metric].observe(
                    timestamp_ns - started
                )

    def _latency_payload_locked(self) -> dict[str, Any]:
        return {
            "run_id": self._latency_run_id,
            "metrics": {
                metric: histogram.to_payload()
                for metric, histogram in self._latency_histograms.items()
            },
        }

    def latency_histograms(self) -> dict[str, Any]:
        """Per-run latency histograms (spec 500 §5; observability plan OW6)."""
        with self._lock:
            return {
                "schema_version": "manyforge.latency_histograms.v1",
                "current_run": self._latency_payload_locked(),
                "last_run": (
                    dict(self._last_run_latency)
                    if self._last_run_latency
                    else None
                ),
            }

    def stop_latency_summary(self) -> dict[str, Any]:
        """p50/p99 stop latency for the heartbeat (current run, falling back
        to the last finished run)."""
        with self._lock:
            histogram = self._latency_histograms["stop"]
            if histogram.count:
                return {
                    "source_run": self._latency_run_id,
                    "count": histogram.count,
                    "p50_ns": histogram.percentile_ns(0.50),
                    "p99_ns": histogram.percentile_ns(0.99),
                }
            last = self._last_run_latency or {}
            stop = (last.get("metrics") or {}).get("stop") or {}
            return {
                "source_run": last.get("run_id"),
                "count": stop.get("count", 0),
                "p50_ns": stop.get("p50_ns"),
                "p99_ns": stop.get("p99_ns"),
            }

    def _observe_scene_snapshot_lease_event_locked(
        self,
        native_type: str,
        details: dict[str, Any],
    ) -> None:
        if native_type not in {"validation_passed", "error_raised"}:
            return
        reason = str(
            details.get("reason_code")
            or details.get("reason")
            or ""
        )
        counter = _SCENE_SNAPSHOT_LEASE_REASON_COUNTERS.get(reason)
        if counter is None:
            return
        self._scene_snapshot_lease_stats[counter] += 1
        self._last_scene_snapshot_lease_reason = reason
        self._last_scene_snapshot_lease_wall = _wall_timestamp_iso()

    def scene_snapshot_lease_status(self) -> dict[str, Any]:
        """RT-7E admission-lease counters derived from native events."""
        with self._lock:
            return {
                "schema_version": "manyforge.scene_snapshot_lease_stats.v1",
                "status": "healthy",
                "counters": dict(self._scene_snapshot_lease_stats),
                "last_reason": self._last_scene_snapshot_lease_reason,
                "last_wall": self._last_scene_snapshot_lease_wall,
            }

    def last_stop_decision(self) -> Optional[dict[str, Any]]:
        """Latest assembled stop-decision chain (digest/incident surface)."""
        with self._lock:
            return dict(self._last_stop_decision) if self._last_stop_decision else None

    def _observe_stop_decision_event_locked(
        self,
        native_type: str,
        native_event: dict[str, Any],
        details: dict[str, Any],
    ) -> Optional[dict[str, Any]]:
        """Advance the stop-episode state machine by one native event.

        Returns the finished chain payload when this event closed an episode
        (caller records it outside this locked section). Pure assembly over
        evidence the kernel already emits (plan OW4): opens on stop intent or
        an interrupt latch, accumulates preemption/issue evidence, closes on
        a terminal transition, abandons silently when execution resumes
        without a terminal stop. Terminal transitions with no visible cause
        still produce a chain (trigger="unobserved") — partial evidence is
        reported loudly, never dropped.
        """
        timestamp_ns = int(native_event.get("timestamp_ns") or 0)
        execution_id = int(native_event.get("execution_id") or 0)
        sequence = int(native_event.get("sequence") or 0)

        def _open_episode() -> dict[str, Any]:
            if self._stop_episode is None:
                self._stop_episode = {
                    "started_ts_ns": timestamp_ns,
                    "execution_id": execution_id,
                    "first_sequence": sequence,
                }
            return self._stop_episode

        if native_type == "stop_requested":
            episode = _open_episode()
            episode.setdefault("requested_ts_ns", timestamp_ns)
            reason = str(details.get("reason") or "")
            if reason:
                episode.setdefault("stop_reason", reason)
            return None
        if native_type == "interrupt_latched":
            episode = _open_episode()
            for key in ("latch_id", "source_id", "policy_id"):
                value = details.get(key)
                if value:
                    episode.setdefault(key, str(value))
            episode.setdefault("latched_ts_ns", timestamp_ns)
            return None
        if native_type == "interrupt_preempt_applied":
            if self._stop_episode is not None:
                for key in ("preemption_action", "preemption_result"):
                    value = details.get(key)
                    if value:
                        self._stop_episode[key] = str(value)
                for key in _STOP_CHAIN_DETAIL_KEYS:
                    value = details.get(key)
                    if value:
                        self._stop_episode.setdefault(key, str(value))
            return None
        if native_type == "stop_issued":
            # stop_issued is direct stop-intent evidence and must be able to
            # OPEN an episode: the pending-stop execute-admission path emits
            # only kStopIssued (no second kStopRequested), and the racing
            # ->Planning transition may have just abandoned the busy-path
            # episode.
            episode = _open_episode()
            episode.setdefault("issued_ts_ns", timestamp_ns)
            for key in _STOP_CHAIN_DETAIL_KEYS:
                value = details.get(key)
                if value:
                    episode.setdefault(key, str(value))
            return None
        if native_type != "state_transition":
            return None

        to_state = str(details.get("to_state") or "")
        if to_state in _EPISODE_ABANDON_STATES:
            # Execution resumed / completed / reset to idle without a stop
            # terminal: the episode (if any) never became a stop decision.
            self._stop_episode = None
            self._stop_chain_execution_marker = None
            return None
        if to_state == "Failed" and self._stop_episode is None:
            # A plain planning/validation failure is not a stop decision.
            return None
        if to_state != "Failed" and to_state not in _STOP_TERMINAL_STATES:
            return None

        episode = self._stop_episode
        abandoned_episode_execution: Optional[str] = None
        if (
            episode is not None
            and execution_id
            and int(episode.get("execution_id") or 0)
            and int(episode["execution_id"]) != execution_id
        ):
            # Terminal from a DIFFERENT execution than the open episode:
            # never merge cross-execution evidence into one chain. Abandon
            # the stale episode loudly (its id rides the new chain) and
            # treat this terminal on its own evidence.
            abandoned_episode_execution = str(episode["execution_id"])
            episode = None
            self._stop_episode = None
        if episode is None:
            if self._stop_chain_execution_marker == (execution_id, to_state):
                # A duplicate terminal for a stop already explained (the
                # commit-override event followed by the regular transition
                # emits the SAME to_state for the SAME execution). A LATER
                # terminal with a different state (e.g. Stopped ->
                # InterventionRequired after a failed reset) is a new
                # decision and must produce its own chain.
                return None
            episode = {
                "started_ts_ns": timestamp_ns,
                "execution_id": execution_id,
                "first_sequence": sequence,
            }
        self._stop_episode = None
        chain_execution_id = execution_id or int(episode.get("execution_id") or 0)
        self._stop_chain_execution_marker = (chain_execution_id, to_state)

        source_id = str(episode.get("source_id") or "")
        stop_reason = str(episode.get("stop_reason") or "")
        if source_id == _RUNTIME_COLLISION_FAULT_SOURCE_ID:
            trigger = "fault"
        elif source_id or episode.get("latch_id"):
            trigger = "interrupt"
        elif "reset" in stop_reason:
            trigger = "reset"
        elif "requested_ts_ns" in episode or "issued_ts_ns" in episode:
            trigger = "operator"
        else:
            trigger = "unobserved"

        chain: dict[str, Any] = {
            "trigger": trigger,
            "terminal_state": to_state,
            "kernel_execution_id": str(chain_execution_id),
        }
        if abandoned_episode_execution is not None:
            chain["abandoned_episode_execution_id"] = abandoned_episode_execution
        terminal_reason = str(details.get("reason") or "")
        if terminal_reason:
            chain["terminal_reason"] = terminal_reason
        if str(details.get("terminal_authority_override") or "") == "true":
            chain["override"] = "true"
            proposed = str(details.get("proposed_terminal") or "")
            if proposed:
                chain["proposed_terminal"] = proposed
        for key in (
            "stop_reason",
            "latch_id",
            "source_id",
            "policy_id",
            "preemption_action",
            "preemption_result",
            *_STOP_CHAIN_DETAIL_KEYS,
        ):
            value = episode.get(key)
            if value:
                chain[key] = str(value)
        requested = episode.get("requested_ts_ns")
        issued = episode.get("issued_ts_ns")
        latched = episode.get("latched_ts_ns")
        if requested and issued and issued >= requested:
            chain["request_to_issued_ns"] = str(issued - requested)
        if issued and timestamp_ns >= issued:
            chain["issued_to_terminal_ns"] = str(timestamp_ns - issued)
        if requested and timestamp_ns >= requested:
            chain["request_to_terminal_ns"] = str(timestamp_ns - requested)
        if latched and timestamp_ns >= latched:
            chain["latch_to_terminal_ns"] = str(timestamp_ns - latched)
        if episode.get("first_sequence"):
            chain["first_kernel_sequence"] = str(episode["first_sequence"])
        if sequence:
            chain["terminal_kernel_sequence"] = str(sequence)
        return chain

    def _record_kernel_event_gaps(
        self,
        events: list[dict[str, Any]],
        pipeline_status: Optional[dict[str, int]],
    ) -> None:
        """Turn sequence gaps and pipeline drop-counter advances into durable
        WARN evidence (observability plan P4: loud loss, never silent)."""
        gap_details: dict[str, str] = {}
        with self._lock:
            for native_event in events:
                sequence = int(native_event.get("sequence") or 0)
                if sequence <= 0:
                    continue
                if (
                    self._last_kernel_sequence
                    and sequence > self._last_kernel_sequence + 1
                ):
                    gap_details.setdefault(
                        "missed_min", str(self._last_kernel_sequence + 1)
                    )
                    gap_details["missed_max"] = str(sequence - 1)
                if sequence > self._last_kernel_sequence:
                    self._last_kernel_sequence = sequence
            if pipeline_status:
                collector = int(
                    pipeline_status.get("collector_dropped_total") or 0
                )
                manager = int(pipeline_status.get("manager_dropped_total") or 0)
                last_collector, last_manager = self._last_pipeline_drop_totals
                if collector > last_collector or manager > last_manager:
                    gap_details["collector_dropped_total"] = str(collector)
                    gap_details["manager_dropped_total"] = str(manager)
                    gap_details["collector_dropped_delta"] = str(
                        collector - last_collector
                    )
                    gap_details["manager_dropped_delta"] = str(
                        manager - last_manager
                    )
                self._last_pipeline_drop_totals = (collector, manager)
        if gap_details:
            with self._lock:
                self._kernel_gap_records_total += 1
                self._last_kernel_gap_wall = _wall_timestamp_iso()
                # Loss invalidates the open stop episode: evidence between
                # its events may be missing, so a chain assembled across the
                # gap could conflate decisions. The raw kernel_native lane
                # still carries whatever survived.
                self._stop_episode = None
                self._stop_chain_execution_marker = None
            self._record_event(
                event_type="kernel_event_gap",
                execution_id=0,
                details=gap_details,
            )

    def kernel_event_gap_status(self) -> dict[str, Any]:
        """Gap-record accounting for the heartbeat surface."""
        with self._lock:
            return {
                "gap_records_total": self._kernel_gap_records_total,
                "last_gap_wall": self._last_kernel_gap_wall,
            }

    def record_native_kernel_events(
        self,
        events: list[dict[str, Any]],
        *,
        pipeline_status: Optional[dict[str, int]] = None,
    ) -> None:
        """Project native kernel events into durable history.

        Two lanes (observability plan OW1):
        - FULL-FIDELITY: every native event becomes a durable
          ``kernel_native_<type>`` record on the ``kernel_native`` channel
          (batched-INFO durability; WARN/ERROR types fsync per the existing
          severity tiers), so the post-restart record contains the complete
          kernel timeline, not a curated subset.
        - CURATED (below, unchanged): UI-focused projections keep their
          existing types/channels.

        Loss is itself evidence: kernel dispatch sequences that skip, or
        pipeline drop counters that advance (``pipeline_status`` from
        ``CycleManager.kernel_event_pipeline_status()``), produce a durable
        WARN ``kernel_event_gap`` record.

        `event_index` dedupes events drained through both `/api/kernel/events`
        and the websocket hub.
        """
        self._record_kernel_event_gaps(events, pipeline_status)
        for native_event in events:
            native_type = str(native_event.get("type") or "")
            native_index = int(native_event.get("event_index") or 0)
            with self._lock:
                if (
                    native_index > 0
                    and native_index <= self._last_full_fidelity_kernel_event_index
                ):
                    record_full = False
                else:
                    record_full = bool(native_type)
                    if native_index > 0:
                        self._last_full_fidelity_kernel_event_index = native_index
            if record_full:
                details = self._native_event_details(native_event)
                error_message = details.pop("_error_message", None)
                if error_message:
                    details["error"] = error_message
                sequence = int(native_event.get("sequence") or 0)
                if sequence:
                    details["kernel_sequence"] = str(sequence)
                native_run_id = native_event.get("program_run_id")
                if not native_run_id and isinstance(
                    native_event.get("details"), dict
                ):
                    native_run_id = native_event["details"].get("program_run_id")
                self._record_event(
                    event_type=f"kernel_native_{native_type}",
                    execution_id=int(native_event.get("execution_id") or 0),
                    details=details or None,
                    run_id=str(native_run_id) if native_run_id else None,
                    timestamp_ns=int(
                        native_event.get("timestamp_ns") or time.monotonic_ns()
                    ),
                    kernel_timestamp=bool(native_event.get("timestamp_ns")),
                )
                # OW4: advance the stop-decision assembler on the deduped
                # native stream; a closed episode becomes one durable chain.
                # OW6: the same deduped stream feeds the per-run latency
                # histograms (recorder-thread work only).
                with self._lock:
                    self._observe_scene_snapshot_lease_event_locked(
                        native_type, details
                    )
                    self._observe_latency_event_locked(
                        native_type, native_event
                    )
                    finished_chain = self._observe_stop_decision_event_locked(
                        native_type, native_event, details
                    )
                    if finished_chain is not None:
                        # Prefer request->terminal, then latch->terminal, then
                        # issued->terminal. The last covers the pending-stop
                        # execute-admission path, which opens an episode on
                        # stop_issued alone (no stop_requested, no
                        # interrupt_latched) and would otherwise be silently
                        # excluded from the stop-latency histogram. All three
                        # are non-negative durations by assembler construction.
                        stop_latency = (
                            finished_chain.get("request_to_terminal_ns")
                            or finished_chain.get("latch_to_terminal_ns")
                            or finished_chain.get("issued_to_terminal_ns")
                        )
                        if stop_latency is not None:
                            self._latency_histograms["stop"].observe(
                                int(stop_latency)
                            )
                if finished_chain is not None:
                    self._record_event(
                        event_type="stop_decision_chain",
                        execution_id=int(native_event.get("execution_id") or 0),
                        details=finished_chain,
                        run_id=str(native_run_id) if native_run_id else None,
                        timestamp_ns=int(
                            native_event.get("timestamp_ns")
                            or time.monotonic_ns()
                        ),
                        kernel_timestamp=bool(native_event.get("timestamp_ns")),
                        system_event=True,
                    )
        for native_event in events:
            native_type = str(native_event.get("type") or "")
            native_index = int(native_event.get("event_index") or 0)
            is_controlled_stop_success = (
                _is_controlled_stop_success_native_event(native_event)
            )
            should_record = native_type in _NATIVE_KERNEL_ERROR_EVENT_TYPES_TO_RECORD
            should_record = should_record or (
                native_type in _NATIVE_KERNEL_RESUME_AUDIT_EVENT_TYPES_TO_RECORD
                and _is_resume_audit_native_event(native_event)
            )
            should_record = should_record or is_controlled_stop_success
            if not should_record:
                continue

            with self._lock:
                if (
                    native_index > 0
                    and native_index <= self._last_recorded_native_kernel_event_index
                ):
                    continue
                if native_index > 0:
                    self._last_recorded_native_kernel_event_index = native_index

            details = self._native_event_details(native_event)
            error_message = details.pop("_error_message", None)
            if error_message:
                details["error"] = error_message

            native_run_id = native_event.get("program_run_id")
            if not native_run_id and isinstance(
                native_event.get("details"), dict
            ):
                native_run_id = native_event["details"].get("program_run_id")
            self._record_event(
                event_type=(
                    "controlled_stop_completed"
                    if is_controlled_stop_success
                    else native_type
                ),
                execution_id=int(native_event.get("execution_id") or 0),
                details=details or None,
                # Kernel-stamped correlation id wins: the event belongs to
                # the run that dispatched it, even when drained late.
                run_id=str(native_run_id) if native_run_id else None,
                execution_mode=None,
                timestamp_ns=int(native_event.get("timestamp_ns") or time.monotonic_ns()),
                kernel_timestamp=bool(native_event.get("timestamp_ns")),
            )

    def history(
        self,
        *,
        run_id: Optional[str] = None,
        limit: Optional[int] = None,
        start_ns: Optional[int] = None,
        end_ns: Optional[int] = None,
        min_severity: Optional[str] = None,
        components: Optional[list[str]] = None,
        event_types: Optional[list[str]] = None,
        execution_id: Optional[int] = None,
        kernel_execution_id: Optional[int] = None,
        program_run_id: Optional[str] = None,
        intervention_run_id: Optional[str] = None,
        action_span_id: Optional[str] = None,
        node_instance_id: Optional[str] = None,
        node_attempt_id: Optional[str] = None,
        move_id: Optional[str] = None,
        contact_policy_id: Optional[str] = None,
        from_wall_ns: Optional[int] = None,
        to_wall_ns: Optional[int] = None,
    ) -> dict[str, Any]:
        # OW7: a wall-time window turns this into a cross-run query resolved
        # through the run index rather than a single-run read.
        if from_wall_ns is not None or to_wall_ns is not None:
            return self._history_across_runs(
                run_id=run_id,
                from_wall_ns=from_wall_ns,
                to_wall_ns=to_wall_ns,
                limit=limit,
                min_severity=min_severity,
                components=components,
                event_types=event_types,
                execution_id=execution_id,
                kernel_execution_id=kernel_execution_id,
                program_run_id=program_run_id,
                intervention_run_id=intervention_run_id,
                action_span_id=action_span_id,
                node_instance_id=node_instance_id,
                node_attempt_id=node_attempt_id,
                move_id=move_id,
                contact_policy_id=contact_policy_id,
            )
        selected_run_id = run_id or self.last_run_id
        if limit is None:
            effective_limit = _DEFAULT_HISTORY_LIMIT
        else:
            effective_limit = max(0, min(int(limit), _MAX_HISTORY_LIMIT))
        events = self._load_events_for_run(selected_run_id, limit=effective_limit)
        events = self._filter_events(
            events,
            start_ns=start_ns,
            end_ns=end_ns,
            min_severity=min_severity,
            components=components,
            event_types=event_types,
            execution_id=execution_id,
            kernel_execution_id=kernel_execution_id,
            program_run_id=program_run_id,
            intervention_run_id=intervention_run_id,
            action_span_id=action_span_id,
            node_instance_id=node_instance_id,
            node_attempt_id=node_attempt_id,
            move_id=move_id,
            contact_policy_id=contact_policy_id,
        )
        if effective_limit == 0:
            events = []
        else:
            events = events[-effective_limit:]
        return self._shape_history_payload(events, selected_run_id)

    @staticmethod
    def _shape_history_payload(
        events: list[dict[str, Any]],
        selected_run_id: Optional[str],
        run_ids: Optional[list[str]] = None,
        *,
        runs_truncated: bool = False,
        scanned_run_count: int = 0,
        cross_run_scan_limit: int = 0,
    ) -> dict[str, Any]:
        """Split a materialized, already-limited event list into the history
        response shape (regular events + node-status transitions + the trailing
        execution id). Shared by single-run and cross-run queries."""
        regular_events = [event for event in events if event["type"] != "node_status_transition"]
        node_transitions = [
            {
                "timestamp_ns": event["timestamp_ns"],
                "timestamp": event.get("timestamp"),
                "run_id": event.get("run_id"),
                "program_run_id": event.get("program_run_id"),
                "intervention_run_id": event.get("intervention_run_id"),
                "session_id": event.get("session_id"),
                "session_kind": event.get("session_kind"),
                "authority_source": event.get("authority_source"),
                "principal_id": event.get("principal_id"),
                "grant_type": event.get("grant_type"),
                "parent_run_id": event.get("parent_run_id"),
                "trigger_reason": event.get("trigger_reason"),
                "trigger_node_instance": event.get("trigger_node_instance"),
                "execution_mode": event.get("execution_mode"),
                "execution_id": event.get("execution_id", 0),
                "node": event["node"],
                "from": event["from"],
                "to": event["to"],
            }
            for event in events
            if event["type"] == "node_status_transition"
        ]
        execution_id = 0
        for event in reversed(events):
            execution_id = int(event.get("execution_id") or 0)
            if execution_id > 0:
                break

        return {
            "run_id": selected_run_id,
            "run_ids": run_ids if run_ids is not None else [],
            "runs_truncated": runs_truncated,
            "scanned_run_count": scanned_run_count,
            "cross_run_scan_limit": cross_run_scan_limit,
            "execution_id": execution_id,
            "events": regular_events,
            "node_status_transitions": node_transitions,
            "timestamp_ns": time.monotonic_ns(),
        }

    def _history_across_runs(
        self,
        *,
        run_id: Optional[str],
        from_wall_ns: Optional[int],
        to_wall_ns: Optional[int],
        limit: Optional[int],
        min_severity: Optional[str],
        components: Optional[list[str]],
        event_types: Optional[list[str]],
        execution_id: Optional[int],
        kernel_execution_id: Optional[int],
        program_run_id: Optional[str],
        intervention_run_id: Optional[str],
        action_span_id: Optional[str],
        node_instance_id: Optional[str],
        node_attempt_id: Optional[str],
        move_id: Optional[str],
        contact_policy_id: Optional[str],
    ) -> dict[str, Any]:
        """Resolve a wall-time window across runs via the run index (OW7).

        Runs that start after the upper wall bound are skipped from the index
        alone, without touching their JSONL. No index-derived upper bound is
        trusted: wall clocks can step backward between runs, and native/kernel
        evidence can be explicitly attributed to an older run after a newer run
        starts. Surviving runs are streamed one event at a time (bounded
        memory, torn-line tolerant) through the same filter stack as single-run
        history plus a wall-window predicate.

        The returned page is the MOST RECENT ``limit`` in-window events ordered
        by wall time (tie-broken by recorder event index) — kept in a bounded
        min-heap of size ``limit`` so memory is O(limit) regardless of how many
        events the window spans. Wall order (not index order) is used precisely
        because a wall-clock step between runs can put a later-started run's
        events earlier in wall time; sorting the collected page by wall is
        robust to that, where a newest-run-first early-stop was not."""
        if limit is None:
            effective_limit = _DEFAULT_HISTORY_LIMIT
        else:
            effective_limit = max(0, min(int(limit), _MAX_HISTORY_LIMIT))
        if effective_limit == 0:
            return self._shape_history_payload(
                [],
                None,
                run_ids=[],
                cross_run_scan_limit=1 if run_id else _MAX_CROSS_RUN_RUNS,
            )

        predicate = self._event_filter_predicate(
            min_severity=min_severity,
            components=components,
            event_types=event_types,
            execution_id=execution_id,
            kernel_execution_id=kernel_execution_id,
            program_run_id=program_run_id,
            intervention_run_id=intervention_run_id,
            action_span_id=action_span_id,
            node_instance_id=node_instance_id,
            node_attempt_id=node_attempt_id,
            move_id=move_id,
            contact_policy_id=contact_policy_id,
        )
        if run_id:
            runs = [{"run_id": run_id}]
            runs_truncated = False
            scan_limit = 1
        else:
            runs = self.list_runs(limit=_MAX_CROSS_RUN_RUNS)  # newest-first
            runs_truncated = len(runs) >= _MAX_CROSS_RUN_RUNS
            scan_limit = _MAX_CROSS_RUN_RUNS
        # Bounded min-heap of (key, event); key = (wall_ns, event_index,
        # scan_seq). scan_seq guarantees the key is unique so the heap never
        # falls back to comparing event dicts.
        heap: list[tuple[tuple[int, int, int], dict[str, Any]]] = []
        scan_seq = 0
        scanned_run_count = 0
        for index, run in enumerate(runs):
            run_id = str(run.get("run_id") or "")
            if not run_id:
                continue
            started_ns = _wall_iso_to_ns(run.get("started_wall"))
            # The next-newer run start is advisory only. It is passed so the
            # helper can document why upper-bound pruning is intentionally not
            # performed here.
            effective_end_ns = (
                _wall_iso_to_ns(runs[index - 1].get("started_wall"))
                if index > 0
                else None
            )
            if not run_id:
                if not _run_overlaps_wall_window(
                    started_ns, effective_end_ns, from_wall_ns, to_wall_ns
                ):
                    continue
            scanned_run_count += 1
            for event in self._stream_run_events(run_id):
                if not _event_in_wall_window(event, from_wall_ns, to_wall_ns):
                    continue
                if not predicate(event):
                    continue
                wall_ns = _event_wall_sort_ns(event, from_wall_ns, to_wall_ns)
                key = (wall_ns, int(event.get("event_index") or 0), scan_seq)
                scan_seq += 1
                if len(heap) < effective_limit:
                    heapq.heappush(heap, (key, event))
                elif key > heap[0][0]:
                    heapq.heapreplace(heap, (key, event))

        events = [event for _key, event in sorted(heap, key=lambda item: item[0])]
        run_ids: list[str] = list(
            dict.fromkeys(
                str(event.get("run_id"))
                for event in events
                if event.get("run_id")
            )
        )
        return self._shape_history_payload(
            events,
            None,
            run_ids=run_ids,
            runs_truncated=runs_truncated,
            scanned_run_count=scanned_run_count,
            cross_run_scan_limit=scan_limit,
        )

    def export_events(
        self,
        *,
        run_id: Optional[str] = None,
        start_ns: Optional[int] = None,
        end_ns: Optional[int] = None,
        min_severity: Optional[str] = None,
        components: Optional[list[str]] = None,
        event_types: Optional[list[str]] = None,
        execution_id: Optional[int] = None,
        kernel_execution_id: Optional[int] = None,
        program_run_id: Optional[str] = None,
        intervention_run_id: Optional[str] = None,
        action_span_id: Optional[str] = None,
        node_instance_id: Optional[str] = None,
        node_attempt_id: Optional[str] = None,
        move_id: Optional[str] = None,
        contact_policy_id: Optional[str] = None,
    ) -> list[dict[str, Any]]:
        return self._filter_events(
            self._load_events_for_run(run_id or self.last_run_id),
            start_ns=start_ns,
            end_ns=end_ns,
            min_severity=min_severity,
            components=components,
            event_types=event_types,
            execution_id=execution_id,
            kernel_execution_id=kernel_execution_id,
            program_run_id=program_run_id,
            intervention_run_id=intervention_run_id,
            action_span_id=action_span_id,
            node_instance_id=node_instance_id,
            node_attempt_id=node_attempt_id,
            move_id=move_id,
            contact_policy_id=contact_policy_id,
        )

    def export_ros2_medkit_trace(
        self,
        *,
        run_id: Optional[str] = None,
        exported_at_ns: Optional[int] = None,
    ) -> dict[str, Any]:
        """Export a normalized trace bundle shaped for ros2_medkit-style correlation."""
        selected_run_id = run_id or self.last_run_id
        events = self.export_events(run_id=selected_run_id)
        program_run_id = next(
            (
                event.get("program_run_id")
                for event in events
                if isinstance(event.get("program_run_id"), str)
                and event.get("program_run_id")
            ),
            selected_run_id,
        )
        intervention_run_ids = sorted(
            {
                event["intervention_run_id"]
                for event in events
                if isinstance(event.get("intervention_run_id"), str)
                and event.get("intervention_run_id")
            }
        )
        parent_run_ids = sorted(
            {
                event["parent_run_id"]
                for event in events
                if isinstance(event.get("parent_run_id"), str)
                and event.get("parent_run_id")
            }
        )

        records: list[dict[str, Any]] = []
        for event in events:
            timeline_id = (
                event.get("intervention_run_id")
                or event.get("program_run_id")
                or event.get("run_id")
            )
            attributes: dict[str, Any] = {}
            if isinstance(event.get("details"), dict):
                attributes.update(event["details"])
            if isinstance(event.get("authority_source"), str):
                attributes["authority_source"] = event["authority_source"]
            if isinstance(event.get("grant_type"), str):
                attributes["grant_type"] = event["grant_type"]
            if isinstance(event.get("session_kind"), str):
                attributes["session_kind"] = event["session_kind"]

            records.append(
                {
                    "record_id": (
                        f"{timeline_id or 'unknown'}:{event.get('event_index', 0)}"
                    ),
                    "timeline_id": timeline_id,
                    "parent_timeline_id": event.get("parent_run_id"),
                    "event_index": int(event.get("event_index", 0)),
                    "monotonic_ns": int(event.get("timestamp_ns", 0)),
                    "wall_time": event.get("timestamp_wall") or event.get("timestamp"),
                    "level": event.get("severity", "INFO"),
                    "component": event.get("component", "manyforge.runtime"),
                    "event_type": event.get("type", "unknown"),
                    "message": event.get("message", ""),
                    "execution_mode": event.get("execution_mode"),
                    "execution_id": int(event.get("execution_id") or 0),
                    "move_id": event.get("move_id"),
                    "node": event.get("node"),
                    "session_id": event.get("session_id"),
                    "principal_id": event.get("principal_id"),
                    "trigger_reason": event.get("trigger_reason"),
                    "trigger_node_instance": event.get("trigger_node_instance"),
                    "attributes": attributes,
                }
            )

        return {
            "schema_version": "manyforge.ros2_medkit_trace.v1",
            "trace_source": "manyforge_composer",
            "exported_at_ns": exported_at_ns or time.monotonic_ns(),
            "run_id": selected_run_id,
            "program_run_id": program_run_id,
            "intervention_run_ids": intervention_run_ids,
            "parent_run_ids": parent_run_ids,
            "records": records,
        }

    def _filter_events(
        self,
        events: list[dict[str, Any]],
        *,
        start_ns: Optional[int] = None,
        end_ns: Optional[int] = None,
        min_severity: Optional[str] = None,
        components: Optional[list[str]] = None,
        event_types: Optional[list[str]] = None,
        execution_id: Optional[int] = None,
        kernel_execution_id: Optional[int] = None,
        program_run_id: Optional[str] = None,
        intervention_run_id: Optional[str] = None,
        action_span_id: Optional[str] = None,
        node_instance_id: Optional[str] = None,
        node_attempt_id: Optional[str] = None,
        move_id: Optional[str] = None,
        contact_policy_id: Optional[str] = None,
    ) -> list[dict[str, Any]]:
        """Apply server-side query filters to decoded event dictionaries."""
        predicate = self._event_filter_predicate(
            start_ns=start_ns,
            end_ns=end_ns,
            min_severity=min_severity,
            components=components,
            event_types=event_types,
            execution_id=execution_id,
            kernel_execution_id=kernel_execution_id,
            program_run_id=program_run_id,
            intervention_run_id=intervention_run_id,
            action_span_id=action_span_id,
            node_instance_id=node_instance_id,
            node_attempt_id=node_attempt_id,
            move_id=move_id,
            contact_policy_id=contact_policy_id,
        )
        return [event for event in events if predicate(event)]

    def _event_filter_predicate(
        self,
        *,
        start_ns: Optional[int] = None,
        end_ns: Optional[int] = None,
        min_severity: Optional[str] = None,
        components: Optional[list[str]] = None,
        event_types: Optional[list[str]] = None,
        execution_id: Optional[int] = None,
        kernel_execution_id: Optional[int] = None,
        program_run_id: Optional[str] = None,
        intervention_run_id: Optional[str] = None,
        action_span_id: Optional[str] = None,
        node_instance_id: Optional[str] = None,
        node_attempt_id: Optional[str] = None,
        move_id: Optional[str] = None,
        contact_policy_id: Optional[str] = None,
    ):
        """Build a single-event predicate for the query filters. Normalization
        (and rejection of a bad severity level) happens once, so it can be
        applied per-event while streaming a run without materializing it."""
        normalized_components = [item for item in (components or []) if item]
        normalized_event_types = {item for item in (event_types or []) if item}
        severity_floor = None
        if min_severity is not None:
            severity_floor = _SEVERITY_ORDER.get(min_severity.upper())
            if severity_floor is None:
                raise ValueError(f"Unsupported severity level: {min_severity}")
        requested_execution_id = (
            execution_id if execution_id is not None else kernel_execution_id
        )

        def predicate(event: dict[str, Any]) -> bool:
            timestamp_ns = event.get("timestamp_ns")
            if start_ns is not None and (not isinstance(timestamp_ns, int) or timestamp_ns < start_ns):
                return False
            if end_ns is not None and (not isinstance(timestamp_ns, int) or timestamp_ns > end_ns):
                return False
            if severity_floor is not None:
                current = _SEVERITY_ORDER.get(str(event.get("severity", "")).upper(), 0)
                if current < severity_floor:
                    return False
            component = str(event.get("component", ""))
            if normalized_components and not any(
                component == prefix or component.startswith(f"{prefix}.")
                for prefix in normalized_components
            ):
                return False
            if normalized_event_types and event.get("type") not in normalized_event_types:
                return False
            if requested_execution_id is not None and int(event.get("execution_id") or 0) != requested_execution_id:
                return False
            if program_run_id is not None and event.get("program_run_id") != program_run_id:
                return False
            if intervention_run_id is not None and event.get("intervention_run_id") != intervention_run_id:
                return False
            if action_span_id is not None and self._event_text_field(event, "action_span_id") != action_span_id:
                return False
            if node_instance_id is not None and self._event_text_field(event, "node_instance_id") != node_instance_id:
                return False
            if node_attempt_id is not None and self._event_text_field(event, "node_attempt_id") != node_attempt_id:
                return False
            if move_id is not None and self._event_text_field(event, "move_id") != move_id:
                return False
            if contact_policy_id is not None and self._event_text_field(event, "contact_policy_id") != contact_policy_id:
                return False
            return True

        return predicate

    @staticmethod
    def _event_text_field(event: dict[str, Any], key: str) -> Optional[str]:
        value = event.get(key)
        if value is None and isinstance(event.get("details"), dict):
            value = event["details"].get(key)
        return None if value is None else str(value)

    @staticmethod
    def _native_event_details(native_event: dict[str, Any]) -> dict[str, Any]:
        details: dict[str, Any] = {}
        native_details = native_event.get("details")
        if isinstance(native_details, dict):
            details.update(native_details)

        native_index = native_event.get("event_index")
        if native_index is not None:
            details["native_kernel_event_index"] = int(native_index)
        native_type = native_event.get("type")
        if native_type is not None:
            details["native_kernel_event_type"] = native_type
        for key in (
            "from_state",
            "to_state",
            "action_source",
            "action_span_id",
            "action_span_program_run_id",
            "node_instance_id",
            "node_attempt_id",
            "parent_node_attempt_id",
            "contact_policy_id",
        ):
            value = native_event.get(key)
            if value is not None:
                details[key] = value

        error = native_event.get("error")
        if isinstance(error, dict):
            code = error.get("code")
            message = error.get("message")
            error_details = error.get("details")
            if isinstance(error_details, dict):
                details.setdefault("error_details", dict(error_details))
                for key, value in error_details.items():
                    details.setdefault(str(key), value)
            if code:
                details["error_code"] = code
            if message:
                details["_error_message"] = message
        elif isinstance(error, str) and error:
            details["_error_message"] = error
        return details

    def record_runtime_override_event(
        self,
        *,
        action: str,
        node_name: Optional[str] = None,
        params: Optional[dict[str, Any]] = None,
        activation_state: Optional[str] = None,
        base_program_hash: Optional[str] = None,
        run_id: Optional[str] = None,
        session_id: Optional[str] = None,
        authority_source: Optional[str] = None,
        principal_id: Optional[str] = None,
        grant_type: Optional[str] = None,
        trigger_node_instance: Optional[str] = None,
        details: Optional[dict[str, Any]] = None,
    ) -> None:
        payload = dict(details or {})
        if node_name is not None:
            payload["node_name"] = node_name
        if params is not None:
            payload["params"] = params
        if activation_state is not None:
            payload["activation_state"] = activation_state
        if base_program_hash is not None:
            payload["base_program_hash"] = base_program_hash
        self._record_event(
            event_type=f"runtime_override_{action}",
            details=payload,
            run_id=run_id,
            session_id=session_id,
            authority_source=authority_source,
            principal_id=principal_id,
            grant_type=grant_type,
            trigger_node_instance=trigger_node_instance,
            execution_mode="intervention" if session_id is not None else "program",
        )

    def record_pose_source_event(
        self,
        action: str,
        *,
        details: Optional[dict[str, Any]] = None,
    ) -> None:
        """Record scene pose-source apply/status events.

        Pose-source events are low-rate operational diagnostics used to explain
        why externally observed object poses did or did not enter the planning
        scene. High-rate raw observations stay out of the durable recorder.
        """
        self._record_event(
            event_type=f"pose_source_{action}",
            details=details,
        )

    def record_simulation_control_event(
        self,
        action: str,
        *,
        details: Optional[dict[str, Any]] = None,
        run_id: Optional[str] = None,
        authority_source: Optional[str] = None,
        trigger_node_instance: Optional[str] = None,
        execution_mode: Optional[str] = None,
    ) -> None:
        """Record a simulator lifecycle/control event."""
        self._record_event(
            event_type=f"simulation_control_{action}",
            details=details,
            run_id=run_id,
            authority_source=authority_source,
            trigger_node_instance=trigger_node_instance,
            execution_mode=execution_mode,
        )

    def record_cycle_reset_phase(self, phase_event: dict[str, Any]) -> None:
        """Record one bounded phase in the operator Reset workflow."""
        phase = str(phase_event.get("phase") or "unknown")
        status = str(phase_event.get("status") or "unknown")
        details = dict(phase_event)
        run_id = details.pop("program_run_id", None)
        details["phase"] = phase
        details["status"] = status
        self._record_event(
            event_type=f"cycle_reset_phase_{status}",
            details=details,
            run_id=run_id if isinstance(run_id, str) and run_id else None,
            execution_mode="program",
        )

    def record_recovery_offer_published(self, offer: dict[str, Any]) -> None:
        """Durably record one semantically new immutable cycle offer."""
        self._record_event(
            event_type="recovery_offer_published",
            execution_id=int(offer.get("executionId") or 0),
            details=dict(offer),
            execution_mode="program",
        )

    def record_recovery_action_event(
        self,
        action: str,
        details: dict[str, Any],
    ) -> None:
        if action not in {"requested", "accepted", "rejected"}:
            raise ValueError(f"Unsupported recovery action event: {action}")
        self._record_event(
            event_type=f"recovery_action_{action}",
            execution_id=int(details.get("execution_id") or 0),
            details=dict(details),
            principal_id=(
                str(details.get("principal_id"))
                if details.get("principal_id")
                else None
            ),
            execution_mode="program",
        )

    def record_delegated_recovery_requested(self, request) -> None:
        """Record a delegated-recovery request emitted by the running program."""
        self._record_event(
            event_type="delegated_recovery_requested",
            run_id=request.parent_run_id,
            session_kind="delegated_recovery",
            authority_source="program_node",
            trigger_reason=request.trigger_reason.value if hasattr(request.trigger_reason, "value") else request.trigger_reason,
            trigger_node_instance=request.trigger_node_instance,
            execution_mode="program",
            details={
                "request_id": request.request_id,
                "trigger_node_id": request.trigger_node_id,
                "delegate_policy": request.delegate_policy,
                "allowed_authority_sources": list(request.allowed_authority_sources),
                "allowed_actions": list(request.allowed_actions),
                "allowed_resume_targets": list(request.allowed_resume_targets),
                "default_resume_target": request.default_resume_target,
                "runtime_override_policy": request.runtime_override_policy,
                "persistent_edit_policy": request.persistent_edit_policy,
                "requested_from_state": request.requested_from_state,
            },
        )

    def record_tool_dispatch(
        self,
        *,
        tool_id: str,
        lane: str,
        principal_id: Optional[str] = None,
        arguments: Optional[dict[str, Any]] = None,
        success: bool = True,
        error: Optional[str] = None,
        duration_ms: Optional[float] = None,
        effect: Optional[str] = None,
        session_id: Optional[str] = None,
        run_id: Optional[str] = None,
    ) -> None:
        """Durable per-tool-dispatch audit record (all assistant/MCP lanes).

        The live UX feed keeps human-readable summaries; this record keeps
        the durable who/what/when/outcome that survives a restart. Argument
        VALUES are hashed, never stored raw — same default redaction as the
        model-call audit — while argument keys stay readable so an operator
        can see the call shape.
        """
        details: dict[str, Any] = {"tool_id": tool_id, "lane": lane}
        if effect:
            details["effect"] = effect
        if duration_ms is not None:
            details["duration_ms"] = round(float(duration_ms), 1)
        if arguments:
            details["argument_keys"] = sorted(arguments.keys())
            details["argument_value_hashes"] = {
                key: hashlib.sha256(
                    json.dumps(value, sort_keys=True, default=str).encode()
                ).hexdigest()
                for key, value in arguments.items()
            }
        if error:
            details["error"] = str(error)[:500]
        self._record_event(
            event_type=(
                "tool_dispatch_completed" if success else "tool_dispatch_failed"
            ),
            details=details,
            principal_id=principal_id or None,
            session_id=session_id,
            run_id=run_id,
        )

    def record_model_event(
        self,
        *,
        action: str,
        model_endpoint_alias: str,
        prompt_template_id: Optional[str] = None,
        prompt_template_hash: Optional[str] = None,
        prompt_text: Optional[str] = None,
        argument_keys: Optional[list[str]] = None,
        argument_values: Optional[dict[str, Any]] = None,
        response_text: Optional[str] = None,
        response_fields: Optional[dict[str, Any]] = None,
        model_version: Optional[str] = None,
        latency_ms: Optional[float] = None,
        token_counts: Optional[dict[str, int]] = None,
        error: Optional[str] = None,
        node_instance: Optional[str] = None,
        run_id: Optional[str] = None,
        session_id: Optional[str] = None,
        principal_id: Optional[str] = None,
        parent_run_id: Optional[str] = None,
        execution_mode: Optional[str] = None,
        audit_policy: Any = None,
    ) -> None:
        """Record a model call event with deployment-policy-driven redaction.

        ``audit_policy`` should be a ``ModelAuditPolicy`` instance (or None for
        defaults).  Redaction rules:
        - argument values are hashed (not stored raw) unless policy says otherwise
        - full prompt text is omitted unless ``store_full_prompts`` is True
        - full response text is omitted unless ``store_full_responses`` is True
        """
        # Resolve policy defaults
        store_full_prompts = False
        store_full_responses = False
        hash_algo = "sha256"
        redact_args = True
        if audit_policy is not None:
            store_full_prompts = getattr(audit_policy, "store_full_prompts", False)
            store_full_responses = getattr(audit_policy, "store_full_responses", False)
            hash_algo = getattr(audit_policy, "hash_algorithm", "sha256")
            redact_args = getattr(audit_policy, "redact_argument_values", True)

        payload: dict[str, Any] = {
            "model_endpoint_alias": model_endpoint_alias,
        }
        if prompt_template_id is not None:
            payload["prompt_template_id"] = prompt_template_id
        if prompt_template_hash is not None:
            payload["prompt_template_hash"] = prompt_template_hash
        if model_version is not None:
            payload["model_version"] = model_version
        if node_instance is not None:
            payload["node_instance"] = node_instance
        if latency_ms is not None:
            payload["latency_ms"] = latency_ms
        if token_counts is not None:
            payload["token_counts"] = dict(token_counts)
        if error is not None:
            payload["error"] = error

        # Argument redaction
        if argument_keys is not None:
            payload["argument_keys"] = list(argument_keys)
        if argument_values is not None:
            if redact_args:
                payload["argument_value_hashes"] = {
                    k: hashlib.new(
                        hash_algo,
                        json.dumps(v, sort_keys=True, default=str).encode(),
                    ).hexdigest()
                    for k, v in argument_values.items()
                }
            else:
                payload["argument_values"] = argument_values

        # Prompt redaction
        if prompt_text is not None:
            if store_full_prompts:
                payload["prompt_text"] = prompt_text
            else:
                payload["prompt_text_hash"] = hashlib.new(
                    hash_algo, prompt_text.encode()
                ).hexdigest()

        # Response redaction
        if response_text is not None:
            payload["response_hash"] = hashlib.new(
                hash_algo, response_text.encode()
            ).hexdigest()
            if store_full_responses:
                payload["response_text"] = response_text
        if response_fields is not None:
            if store_full_responses:
                payload["response_fields"] = response_fields
            else:
                # Store field keys only, not values
                payload["response_field_keys"] = sorted(response_fields.keys())

        # Derive execution_mode from context if not explicitly set:
        # present session_id → intervention, otherwise let _record_event
        # infer "program" from the current run.
        effective_mode = execution_mode
        if effective_mode is None and session_id is not None:
            effective_mode = "intervention"

        self._record_event(
            event_type=f"model_call_{action}",
            details=payload,
            run_id=run_id,
            session_id=session_id,
            principal_id=principal_id,
            parent_run_id=parent_run_id,
            trigger_node_instance=node_instance,
            execution_mode=effective_mode,
        )

    def _stream_run_events(self, run_id: Optional[str]):
        """Yield a run's events one at a time.

        For a run persisted to disk, the file path is snapshotted under the
        lock and then streamed OUTSIDE the lock (I/O off the recording path,
        P1), one line at a time, skipping torn lines (crash-truncated final
        line is expected evidence). For the current run, the in-memory tail is
        merged after the disk stream so a just-recorded event is visible even
        before the writer thread flushes it."""
        with self._lock:
            if run_id is None:
                snapshot = [self._decode_stored_event(e) for e in self._events]
                path = None
                memory_tail: list[dict[str, Any]] = []
            else:
                path = self._path_for_run(run_id)
                current_run = (
                    self._current_run is not None
                    and self._current_run.run_id == run_id
                )
                memory_tail = (
                    [
                        self._decode_stored_event(e)
                        for e in self._events
                        if e.run_id == run_id
                    ]
                    if current_run
                    else []
                )
                if path is None or not path.exists():
                    snapshot = memory_tail or [
                        self._decode_stored_event(e)
                        for e in self._events
                        if e.run_id == run_id
                    ]
                else:
                    snapshot = None
        if snapshot is not None:
            yield from snapshot
            return
        seen_indices: set[int] = set()
        try:
            with path.open("r", encoding="utf-8") as handle:
                for line in handle:
                    stripped = line.strip()
                    if not stripped:
                        continue
                    try:
                        event = json.loads(stripped)
                    except ValueError:
                        continue
                    seen_indices.add(int(event.get("event_index") or 0))
                    yield event
        except OSError:
            pass
        for event in memory_tail:
            event_index = int(event.get("event_index") or 0)
            if event_index not in seen_indices:
                yield event

    def _load_events_for_run(
        self,
        run_id: Optional[str],
        *,
        limit: Optional[int] = None,
    ) -> list[dict[str, Any]]:
        """Load events for a given run.

        When *limit* is set and the source is a JSONL file, only the last
        *limit* lines are materialized (streamed with a sliding window)
        instead of reading the entire file into memory.
        """
        with self._lock:
            if run_id is None:
                events = [self._decode_stored_event(event) for event in self._events]
                if limit is not None and limit >= 0:
                    return [] if limit == 0 else events[-limit:]
                return events
            path = self._path_for_run(run_id)
            if path is not None and path.exists():
                if limit is not None and limit >= 0:
                    return self._tail_jsonl(path, limit)
                events: list[dict[str, Any]] = []
                # Stream line-by-line (not read_text of the whole file) and skip
                # torn lines: a crashed run's torn final line is expected
                # evidence and must not fail a read — the same tolerance
                # _tail_jsonl already applies on the limited path.
                with path.open("r", encoding="utf-8") as handle:
                    for line in handle:
                        stripped = line.strip()
                        if not stripped:
                            continue
                        try:
                            events.append(json.loads(stripped))
                        except ValueError:
                            continue
                return events
            events = [
                self._decode_stored_event(event)
                for event in self._events
                if event.run_id == run_id
            ]
            if limit is not None and limit >= 0:
                return [] if limit == 0 else events[-limit:]
            return events

    @staticmethod
    def _tail_jsonl(path: Path, limit: int) -> list[dict[str, Any]]:
        """Read only the last *limit* lines from a JSONL file.

        Uses a bounded deque so at most *limit* parsed events are in
        memory at any time, avoiding O(file_size) allocation.
        """
        from collections import deque as _deque
        window: _deque[dict[str, Any]] = _deque(maxlen=limit)
        with path.open("r", encoding="utf-8") as fh:
            for line in fh:
                stripped = line.strip()
                if not stripped:
                    continue
                try:
                    window.append(json.loads(stripped))
                except ValueError:
                    # A torn line (crash mid-append) must not poison the
                    # valid prefix: restart continuity and history reads
                    # recover everything that was written whole.
                    continue
        return list(window)

    def _handle_cycle_state_transition(
        self,
        previous_state: Optional[str],
        current_state: Optional[str],
        runner_run_id: Optional[str] = None,
    ) -> None:
        if current_state == "running" and previous_state in {None, "idle"}:
            self._start_run(runner_run_id)

        self._record_event(
            event_type="cycle_state_changed",
            details={"from": previous_state, "to": current_state},
        )

        if current_state == "idle" and previous_state in {"running", "stopped"}:
            self._finish_current_run()

    def _handle_kernel_transition(self, status, snapshot) -> None:
        previous_state = self._last_kernel_state
        previous_execution_id = self._last_execution_id
        previous_move_id = self._last_active_move_id

        current_state = status.state.value
        current_execution_id = status.execution_id
        current_move_id = status.active_move_id or None

        if (
            current_state != previous_state
            or current_execution_id != previous_execution_id
            or (status.active_move_id or "") != previous_move_id
        ):
            details = {
                "from": previous_state,
                "to": current_state,
                "progress": status.progress,
                "error": snapshot.error,
            }
            error_code = getattr(snapshot, "error_code", None)
            if error_code:
                details["error_code"] = error_code
            error_details = getattr(snapshot, "error_details", None)
            if error_details:
                details["error_details"] = dict(error_details)
            self._record_event(
                event_type="kernel_state_transition",
                execution_id=current_execution_id,
                move_id=current_move_id,
                details=details,
            )
            self._record_semantic_execution_event(
                previous_state=previous_state,
                current_state=current_state,
                execution_id=current_execution_id,
                move_id=current_move_id,
                error=snapshot.error,
                progress=status.progress,
                previous_execution_id=previous_execution_id,
            )

        kernel_error = snapshot.error
        if kernel_error != self._last_kernel_error and kernel_error:
            self._record_event(
                event_type="kernel_error",
                execution_id=current_execution_id,
                move_id=current_move_id,
                details={"error": kernel_error},
            )

        self._last_kernel_state = current_state
        self._last_execution_id = current_execution_id
        self._last_active_move_id = status.active_move_id or ""
        self._last_kernel_error = kernel_error

    def _handle_node_status_changes(
        self,
        current_statuses: dict[str, str],
        status,
        *,
        cycle_state: str | None,
        intervention_session=None,
    ) -> None:
        changed_events: list[tuple[str, Optional[str], str]] = []
        all_names = set(self._last_node_statuses) | set(current_statuses)
        for node_name in sorted(all_names):
            previous = self._last_node_statuses.get(node_name)
            current = current_statuses.get(node_name)
            if previous == current or current is None:
                continue
            if previous in {None, "INVALID"} and current == "INVALID":
                continue
            if previous is None and current == "INVALID":
                continue
            changed_events.append((node_name, previous, current))

        intervention_run_id = None
        execution_mode = None
        session_id = None
        session_kind = None
        authority_source = None
        principal_id = None
        grant_type = None
        parent_run_id = None
        trigger_reason = None
        trigger_node_instance = None
        if cycle_state == "direct_control" and intervention_session is not None:
            intervention_run_id = self._intervention_run_id(intervention_session)
            execution_mode = "intervention"
            session_id = intervention_session.session_id
            session_kind = intervention_session.session_kind
            authority_source = intervention_session.authority_source
            principal_id = getattr(intervention_session, "principal_id", None)
            grant_type = getattr(intervention_session, "grant_type", None)
            parent_run_id = intervention_session.parent_run_id
            trigger_reason = intervention_session.trigger_reason
            trigger_node_instance = intervention_session.trigger_node_instance

        for node_name, previous, current in changed_events:
            self._record_event(
                event_type="node_status_transition",
                execution_id=status.execution_id if status is not None else 0,
                move_id=status.active_move_id if status is not None else None,
                node=node_name,
                previous_status=previous,
                current_status=current,
                run_id=intervention_run_id,
                session_id=session_id,
                session_kind=session_kind,
                authority_source=authority_source,
                principal_id=principal_id,
                grant_type=grant_type,
                parent_run_id=parent_run_id,
                trigger_reason=trigger_reason,
                trigger_node_instance=trigger_node_instance,
                execution_mode=execution_mode,
            )

        self._last_node_statuses = dict(current_statuses)

    # (previous_state, current_state) → semantic event name.
    # None in prev matches any state not covered by a more specific entry.
    _TRANSITION_TABLE: dict[tuple[Optional[str], str], str] = {
        (None, "planning"):       "plan_started",
        ("idle", "planning"):     "plan_started",
        ("stopped", "planning"):  "plan_started",
        ("completed", "planning"): "plan_started",
        ("failed", "planning"):   "plan_started",
        ("planning", "validating"): "plan_completed",
        ("idle", "validating"):   "cache_hit",
        ("stopped", "validating"): "validation_started",
        ("completed", "validating"): "validation_started",
        ("failed", "validating"): "validation_started",
        ("validating", "executing"): "execute_started",
        (None, "executing"):      "execute_started",
        ("idle", "executing"):    "execute_started",
        ("stopped", "executing"): "execute_started",
        ("completed", "executing"): "execute_started",
        ("executing", "completed"): "execute_completed",
        ("executing", "failed"):  "execute_failed",
        ("planning", "failed"):   "plan_failed",
        ("validating", "failed"): "validation_failed",
        ("planning", "intervention_required"): "intervention_required",
        ("validating", "intervention_required"): "intervention_required",
        ("executing", "intervention_required"): "intervention_required",
        ("paused", "intervention_required"): "intervention_required",
        ("stopping", "intervention_required"): "intervention_required",
        ("stopped", "intervention_required"): "intervention_required",
        ("completed", "intervention_required"): "intervention_required",
        ("failed", "intervention_required"): "intervention_required",
        ("executing", "paused"):  "pause_completed",
        ("executing", "stopped"): "stop_completed",
        ("planning", "stopped"):  "stop_completed",
        ("validating", "stopped"): "stop_completed",
        ("paused", "stopped"):    "stop_completed",
    }

    def _record_semantic_execution_event(
        self,
        *,
        previous_state: Optional[str],
        current_state: str,
        execution_id: int,
        move_id: Optional[str],
        error: Optional[str],
        progress: float,
        previous_execution_id: int = 0,
    ) -> None:
        details: dict[str, Any] = {"progress": progress}
        if error:
            details["error"] = error

        event_type = self._TRANSITION_TABLE.get((previous_state, current_state))

        # Audit 6G: the observer is poll-based, so a fast move can flash
        # through Executing between two polls and the table sees e.g.
        # validating -> completed — losing execute_started/execute_completed
        # even though `completed` PROVES an execution window happened.
        # Synthesize only the certainty cases (found by the 6G soak gate):
        #   * any non-executing state -> completed: an executing window was
        #     skipped — emit execute_started + execute_completed;
        #   * executing -> executing with a new execution_id: the previous
        #     move completed and a new one started between polls.
        # Failure edges are NOT interpolated (a plan/validation failure
        # never executed; fabricating execute_started there would lie).
        if (
            event_type is None
            and current_state == "completed"
            and previous_state is not None
            and previous_state != "executing"
        ):
            synthesized = dict(details)
            synthesized["synthesized_from_edge"] = (
                f"{previous_state}->{current_state}"
            )
            if previous_state == "planning":
                self._record_event(
                    event_type="plan_completed",
                    execution_id=execution_id,
                    move_id=move_id,
                    details=dict(synthesized),
                )
            self._record_event(
                event_type="execute_started",
                execution_id=execution_id,
                move_id=move_id,
                details=dict(synthesized),
            )
            self._record_event(
                event_type="execute_completed",
                execution_id=execution_id,
                move_id=move_id,
                details=synthesized,
            )
            return
        if (
            event_type is None
            and previous_state == "planning"
            and current_state == "executing"
        ):
            # The poll skipped only Validating: reaching Executing implies
            # the plan completed and validation passed.
            synthesized = dict(details)
            synthesized["synthesized_from_edge"] = "planning->executing"
            self._record_event(
                event_type="plan_completed",
                execution_id=execution_id,
                move_id=move_id,
                details=dict(synthesized),
            )
            self._record_event(
                event_type="execute_started",
                execution_id=execution_id,
                move_id=move_id,
                details=synthesized,
            )
            return
        if (
            event_type is None
            and current_state == "executing"
            and previous_state == "executing"
            and previous_execution_id
            and execution_id != previous_execution_id
        ):
            synthesized = dict(details)
            synthesized["synthesized_from_edge"] = "executing->executing"
            self._record_event(
                event_type="execute_completed",
                execution_id=previous_execution_id,
                details=dict(synthesized),
            )
            self._record_event(
                event_type="execute_started",
                execution_id=execution_id,
                move_id=move_id,
                details=synthesized,
            )
            return

        if event_type is not None:
            self._record_event(
                event_type=event_type,
                execution_id=execution_id,
                move_id=move_id,
                details=details,
            )

    def _record_event(
        self,
        *,
        event_type: str,
        execution_id: int = 0,
        move_id: Optional[str] = None,
        details: Optional[dict[str, Any]] = None,
        node: Optional[str] = None,
        previous_status: Optional[str] = None,
        current_status: Optional[str] = None,
        run_id: Optional[str] = None,
        session_id: Optional[str] = None,
        session_kind: Optional[str] = None,
        authority_source: Optional[str] = None,
        principal_id: Optional[str] = None,
        grant_type: Optional[str] = None,
        parent_run_id: Optional[str] = None,
        trigger_reason: Optional[str] = None,
        trigger_node_instance: Optional[str] = None,
        execution_mode: Optional[str] = None,
        timestamp_ns: Optional[int] = None,
        kernel_timestamp: bool = False,
        system_event: bool = False,
    ) -> None:
        _validate_event_type(event_type)
        timestamp_ns = timestamp_ns or time.monotonic_ns()
        # Kernel-stamped timestamps convert to wall time through the clock
        # anchor (no drain-latency skew); receipt time is the fallback.
        wall_timestamp = (
            self._wall_iso_from_kernel_ns(timestamp_ns) if kernel_timestamp else None
        ) or _wall_timestamp_iso()
        with self._lock:
            if run_id is None:
                if self._current_run is not None:
                    run_id = self._current_run.run_id
                elif not system_event and self._last_run is not None:
                    # System/durable events (prune evidence, startup anchors)
                    # must NOT attach to a finished or rehydrated run: a
                    # startup prune would otherwise record its evidence into
                    # the very run file it just deleted, recreating it.
                    run_id = self._last_run.run_id
            elif self._current_run is None and (
                self._last_run is None or self._last_run.run_id != run_id
            ):
                self._last_run = _RunInfo(
                    run_id=run_id,
                    started_at_ns=timestamp_ns,
                    file_path=self._path_for_run(run_id),
                )

            self._event_index += 1
            program_run_id = run_id if (execution_mode in (None, "program") and run_id is not None) else None
            intervention_run_id = None
            if execution_mode == "intervention":
                program_run_id = parent_run_id
                intervention_run_id = run_id

            details_payload = dict(details) if details else None
            if event_type == "stop_decision_chain":
                details_payload = details_payload or {}
                latch_id = str(details_payload.get("latch_id") or "") or None
                recovery_context_id = _stable_recovery_context_id(
                    run_id=run_id,
                    execution_id=execution_id,
                    latch_id=latch_id,
                )
                if recovery_context_id is not None:
                    details_payload.setdefault(
                        "recovery_context_id",
                        recovery_context_id,
                    )
                    details_payload.setdefault(
                        "stop_decision_id",
                        _stable_stop_decision_id(
                            recovery_context_id=recovery_context_id,
                            terminal_state=str(
                                details_payload.get("terminal_state") or "unknown"
                            ),
                            terminal_kernel_sequence=str(
                                details_payload.get("terminal_kernel_sequence")
                                or self._event_index
                            ),
                        ),
                    )
                # Correlation anchors for the incident-embed selector. Stamp
                # them before serialization so durable trace records and the
                # in-memory latest-chain view carry identical evidence.
                details_payload["recorded_event_index"] = str(self._event_index)
                details_payload["recorded_run_id"] = str(run_id or "")
                if recovery_context_id is not None:
                    for snapshot in reversed(self._incident_snapshots):
                        if snapshot.get("recoveryContextId") != recovery_context_id:
                            continue
                        details_payload["incident_id"] = snapshot["incidentId"]
                        snapshot["stopDecision"] = dict(details_payload)
                        snapshot["stopDecisionId"] = details_payload.get(
                            "stop_decision_id"
                        )
                        snapshot["stopDecisionCorrelation"] = (
                            "recovery_context_id_match"
                        )
                        break
                self._last_stop_decision = dict(details_payload)
            elif event_type.startswith("recovery_"):
                details_payload = details_payload or {}
                latch_id = str(
                    details_payload.get("latch_id")
                    or details_payload.get("latchId")
                    or ""
                ) or None
                recovery_context_id = _stable_recovery_context_id(
                    run_id=run_id,
                    execution_id=execution_id,
                    latch_id=latch_id,
                    evidence_refs=(
                        details_payload.get("evidence_refs")
                        or details_payload.get("evidenceRefs")
                    ),
                    offer_id=str(
                        details_payload.get("offer_id")
                        or details_payload.get("offerId")
                        or ""
                    )
                    or None,
                )
                if recovery_context_id is not None:
                    details_payload.setdefault(
                        "recovery_context_id",
                        recovery_context_id,
                    )
                    chain = self._last_stop_decision
                    if (
                        chain is not None
                        and chain.get("recovery_context_id") == recovery_context_id
                    ):
                        details_payload.setdefault(
                            "stop_decision_id",
                            chain.get("stop_decision_id"),
                        )
                        details_payload.setdefault(
                            "incident_id",
                            chain.get("incident_id"),
                        )
                    if not details_payload.get("incident_id"):
                        for snapshot in reversed(self._incident_snapshots):
                            if (
                                snapshot.get("recoveryContextId")
                                == recovery_context_id
                            ):
                                details_payload["incident_id"] = snapshot[
                                    "incidentId"
                                ]
                                break
            action_span_id = None
            action_source = None
            node_instance_id = None
            node_attempt_id = None
            parent_node_attempt_id = None
            contact_policy_id = None
            if details_payload:
                action_span_id = details_payload.get("action_span_id")
                action_source = details_payload.get("action_source")
                node_instance_id = details_payload.get("node_instance_id")
                node_attempt_id = details_payload.get("node_attempt_id")
                parent_node_attempt_id = details_payload.get(
                    "parent_node_attempt_id"
                )
                contact_policy_id = details_payload.get("contact_policy_id")
            channel = _event_channel(event_type, details_payload)
            severity = _severity_for_event(event_type, details_payload)
            if (
                self._current_run is not None
                and run_id == self._current_run.run_id
            ):
                self._current_run_event_count += 1
                if _SEVERITY_ORDER.get(severity, 20) > _SEVERITY_ORDER.get(
                    self._current_run_max_severity, 20
                ):
                    self._current_run_max_severity = severity
            event: dict[str, Any] = {
                "event_index": self._event_index,
                "sequence": self._event_index,
                "timestamp_ns": timestamp_ns,
                "timestamp": wall_timestamp,
                "timestamp_wall": wall_timestamp,
                "schema_version": "manyforge.execution_event.v1",
                "trace_source": "event_recorder",
                "run_id": run_id,
                "program_run_id": program_run_id,
                "execution_id": execution_id,
                "type": event_type,
                "event": event_type,
                "severity": severity,
                "component": _component_for_event(event_type),
                "channel": channel,
            }
            if action_span_id:
                event["action_span_id"] = str(action_span_id)
            if action_source:
                event["action_source"] = str(action_source)
            if node_instance_id:
                event["node_instance_id"] = str(node_instance_id)
            if node_attempt_id:
                event["node_attempt_id"] = str(node_attempt_id)
            if parent_node_attempt_id:
                event["parent_node_attempt_id"] = str(parent_node_attempt_id)
            if contact_policy_id:
                event["contact_policy_id"] = str(contact_policy_id)
            if intervention_run_id is not None:
                event["intervention_run_id"] = intervention_run_id
            if move_id:
                event["move_id"] = move_id
            if details_payload:
                event["details"] = details_payload
            if session_id is not None:
                event["session_id"] = session_id
            if session_kind is not None:
                event["session_kind"] = session_kind
            if authority_source is not None:
                event["authority_source"] = authority_source
            if principal_id is not None:
                event["principal_id"] = principal_id
            if grant_type is not None:
                event["grant_type"] = grant_type
            if parent_run_id is not None:
                event["parent_run_id"] = parent_run_id
            if trigger_reason is not None:
                event["trigger_reason"] = trigger_reason
            if trigger_node_instance is not None:
                event["trigger_node_instance"] = trigger_node_instance
            if execution_mode is not None:
                event["execution_mode"] = execution_mode
            elif run_id is not None:
                event["execution_mode"] = "program"
            if event_type == "node_status_transition":
                event["node"] = node
                event["from"] = previous_status
                event["to"] = current_status
            event["message"] = _message_for_event(
                event_type,
                details=details_payload,
                node=node,
                previous_status=previous_status,
                current_status=current_status,
            )

            json_text = self._serialize_event(event)
            stored = _StoredEvent(
                event_index=self._event_index,
                run_id=run_id,
                event_type=event_type,
                json_text=json_text,
            )
            self._events.append(stored)
            decoded_event = deepcopy(event)
            self._decoded_recent_events.append(decoded_event)
            self._events_by_channel.setdefault(
                channel,
                deque(maxlen=_DEFAULT_CHANNEL_EVENT_LIMIT),
            ).append(stored)
            self._decoded_events_by_channel.setdefault(
                channel,
                deque(maxlen=_DEFAULT_CHANNEL_EVENT_LIMIT),
            ).append(decoded_event)
            if _event_should_create_incident(
                event_type,
                details_payload,
                severity,
                channel,
            ):
                self._incident_snapshots.append(
                    self._build_incident_snapshot_locked(event)
                )
            self._enqueue_jsonl_locked(
                run_id=run_id,
                json_text=json_text,
                durable=(
                    _SEVERITY_ORDER.get(str(severity).upper(), 0)
                    >= _SEVERITY_ORDER["WARN"]
                    or event_type in _DURABLE_INFO_EVENT_TYPES
                ),
            )
            self._maybe_write_failure_archive_locked(event)

    def _intervention_run_id(self, session) -> str:
        """Return the correlated run_id for an intervention session."""
        return session.parent_run_id or f"intervention-{session.session_id}"

    def _handle_intervention_events(self, session) -> None:
        """Project intervention session events into the unified recorder stream."""
        if session is None:
            return

        with self._lock:
            if session.session_id != self._last_intervention_session_id:
                self._last_intervention_session_id = session.session_id
                self._last_intervention_event_index = 0

            run_id = self._intervention_run_id(session)
            session_events = session.events
            event_count = len(session_events)
            if event_count == 0:
                return
            first_index = int(session_events[0].get("event_index", 0) or 0)
            last_index = int(session_events[-1].get("event_index", 0) or 0)
            if self._last_intervention_event_index < first_index:
                start_offset = 0
            elif self._last_intervention_event_index >= last_index:
                return
            else:
                start_offset = self._last_intervention_event_index - first_index + 1
            new_events = list(islice(session_events, start_offset, None))
            if not new_events:
                return

            max_event_index = self._last_intervention_event_index
            for event in new_events:
                details = dict(event.get("details", {}))
                details.setdefault("entry_state", session.entry_state)
                details.setdefault("resume_target", session.resume_target)
                self._record_event(
                    event_type=f"intervention_{event['type']}",
                    execution_id=self._last_execution_id,
                    details=details,
                    run_id=run_id,
                    session_id=session.session_id,
                    session_kind=session.session_kind,
                    authority_source=session.authority_source,
                    principal_id=getattr(session, "principal_id", None),
                    grant_type=getattr(session, "grant_type", None),
                    parent_run_id=session.parent_run_id,
                    trigger_reason=session.trigger_reason,
                    trigger_node_instance=session.trigger_node_instance,
                    execution_mode="intervention",
                    timestamp_ns=event.get("timestamp_ns"),
                )
                max_event_index = max(
                    max_event_index,
                    int(event.get("event_index", 0)),
                )
            self._last_intervention_event_index = max_event_index

    def _start_run(self, runner_run_id: Optional[str] = None) -> None:
        now_ns = time.monotonic_ns()
        self._run_counter += 1
        if runner_run_id:
            # spec/500 §2.2: the orchestrator runner is the authoritative
            # minter. Adopting its id keeps ONE id space across durable
            # history, kernel-stamped events, and runner-side records —
            # otherwise programRunId filters silently miss kernel events.
            run_id = runner_run_id
        else:
            stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
            run_id = f"run-{stamp}-{self._run_counter:03d}"
        file_path = self._path_for_run(run_id)
        self._current_run = _RunInfo(run_id=run_id, started_at_ns=now_ns, file_path=file_path)
        self._last_run = self._current_run
        self._current_run_event_count = 0
        self._current_run_max_severity = "INFO"
        self._current_run_program_run_id = runner_run_id
        with self._lock:
            # Stash the OUTGOING run's histograms here, not at run-finish, so a
            # trailing completion observed in the window between finish and the
            # next start still lands in the run it belongs to before the
            # snapshot is taken. The cycle-state poll (which drives finish/start)
            # and the native event stream (which pairs durations) are distinct
            # entry points with no ordering guarantee, so a run's final
            # execute/stop sample can arrive after finish. The first run has no
            # predecessor to stash.
            if self._latency_run_id is not None:
                self._last_run_latency = self._latency_payload_locked()
            self._latency_histograms = {
                metric: _LatencyHistogram() for metric in _LATENCY_METRICS
            }
            self._latency_pending = {
                metric: {} for metric in ("plan", "validate", "execute")
            }
            self._latency_run_id = run_id
        self._append_run_index_line(
            {
                "kind": "run_started",
                "run_id": run_id,
                "program_run_id": runner_run_id,
                "run_counter": self._run_counter,
                "started_wall": _wall_timestamp_iso(),
                "started_at_ns": now_ns,
            }
        )
        # Re-record the stored clock anchor as the first event of the new run
        # so every run file is self-contained for wall-time derivation — no
        # reader has to hunt an earlier file for the anchor. The pair itself
        # never goes stale (it is a fact: "monotonic X was wall Y"), so
        # re-emitting the cached value costs one recorder-thread event and no
        # kernel call.
        with self._lock:
            anchor = self._clock_anchor
        if anchor is not None:
            self._record_event(
                event_type="clock_anchor",
                execution_id=0,
                details={
                    "monotonic_ns": str(anchor[0]),
                    "wall_time_ns": str(anchor[1]),
                },
            )

    def _finish_current_run(self) -> None:
        if self._current_run is not None:
            self._last_run = self._current_run
            # Latency histograms are stashed at the NEXT run's start (not here)
            # so a trailing sample landing after finish is still attributed to
            # this run — see _start_run.
            self._run_index_severities[self._current_run.run_id] = (
                self._current_run_max_severity
            )
            self._append_run_index_line(
                {
                    "kind": "run_ended",
                    "run_id": self._current_run.run_id,
                    "program_run_id": self._current_run_program_run_id,
                    "ended_wall": _wall_timestamp_iso(),
                    "event_count": self._current_run_event_count,
                    "max_severity": self._current_run_max_severity,
                }
            )
            # A finished run is a natural, cheap moment to enforce the trace
            # retention budgets off the recording path.
            self._enqueue_trace_prune()
        self._current_run = None

    def _path_for_run(self, run_id: Optional[str]) -> Optional[Path]:
        if self._trace_dir is None or run_id is None:
            return None
        return self._trace_dir / f"{run_id}.events.jsonl"

    @staticmethod
    def _serialize_event(event: dict[str, Any]) -> str:
        return json.dumps(event, sort_keys=True)

    @staticmethod
    def _decode_stored_event(event: _StoredEvent) -> dict[str, Any]:
        return json.loads(event.json_text)

    def _build_incident_snapshot_locked(self, trigger_event: dict[str, Any]) -> dict[str, Any]:
        """Freeze recent chronology + protected channels for an incident."""
        all_events = [deepcopy(event) for event in self._decoded_recent_events]
        # OW4: embed the latest stop-decision chain only when it can be
        # CORRELATED to this incident — a mislabeled association in a
        # postmortem artifact is interpretation, not evidence (P2/P3):
        # - exact: the trigger carries the chain's execution id;
        # - fallback (covers execution_id=0 triggers): the trigger is on a
        #   stop/fault channel AND the chain was recorded in the SAME run
        #   within the last _STOP_DECISION_EMBED_WINDOW recorder events.
        # The applied rule ships alongside the payload as
        # stopDecisionCorrelation so consumers never mistake a proximity
        # match for an exact one.
        stop_decision = None
        stop_decision_correlation = None
        if self._last_stop_decision is not None:
            chain = self._last_stop_decision
            trigger_exec = str(trigger_event.get("execution_id") or "")
            chain_exec = str(chain.get("kernel_execution_id") or "")
            if trigger_exec not in ("", "0") and trigger_exec == chain_exec:
                stop_decision_correlation = "execution_id_match"
            elif trigger_exec in ("", "0") and trigger_event.get("channel") in {
                "controlled_stop",
                "fault_safety_stop",
                "stop_decision",
            }:
                chain_run = str(chain.get("recorded_run_id") or "")
                trigger_run = str(trigger_event.get("run_id") or "")
                chain_index = int(chain.get("recorded_event_index") or 0)
                trigger_index = int(
                    trigger_event.get("event_index") or self._event_index
                )
                distance = trigger_index - chain_index
                if (
                    chain_run
                    and chain_run == trigger_run
                    and 0 <= distance <= _STOP_DECISION_EMBED_WINDOW
                ):
                    stop_decision_correlation = "same_run_recent_stop_channel"
            if stop_decision_correlation is not None:
                stop_decision = dict(chain)
        trigger_details = trigger_event.get("details")
        trigger_details = trigger_details if isinstance(trigger_details, dict) else {}
        nested_error_details = trigger_details.get("error_details")
        nested_error_details = (
            nested_error_details if isinstance(nested_error_details, dict) else {}
        )
        runtime_fault_id = str(
            nested_error_details.get("runtime_fault_id")
            or trigger_details.get("runtime_fault_id")
            or ""
        )
        recovery_context_id = _stable_recovery_context_id(
            run_id=(
                str(trigger_event.get("run_id"))
                if trigger_event.get("run_id")
                else None
            ),
            execution_id=int(trigger_event.get("execution_id") or 0),
            latch_id=(
                str(trigger_details.get("latch_id"))
                if trigger_details.get("latch_id")
                else None
            ),
            evidence_refs=(
                [f"runtime_fault:{runtime_fault_id}"]
                if runtime_fault_id
                else None
            ),
        )
        if stop_decision is not None and stop_decision.get("recovery_context_id"):
            recovery_context_id = str(stop_decision["recovery_context_id"])
        span_events = self._span_events_for_trigger(trigger_event, all_events)
        action_span = self._action_span_summary(trigger_event)
        span_scope = self._span_scope_summary(trigger_event)
        headline_cause = self._headline_cause(trigger_event, span_events)
        events_by_channel = {
            channel: [
                deepcopy(event)
                for event in list(events)[-_INCIDENT_CHANNEL_EVENT_LIMIT:]
            ]
            for channel, events in self._decoded_events_by_channel.items()
        }
        return {
            "incidentId": f"incident-{trigger_event.get('event_index', 0)}",
            "recoveryContextId": recovery_context_id,
            "stopDecisionId": (
                stop_decision.get("stop_decision_id")
                if stop_decision is not None
                else None
            ),
            "timestampNs": trigger_event.get("timestamp_ns"),
            "timestamp": trigger_event.get("timestamp"),
            "triggerEventIndex": trigger_event.get("event_index"),
            "triggerType": trigger_event.get("type"),
            "triggerChannel": trigger_event.get("channel"),
            "triggerSeverity": trigger_event.get("severity"),
            "triggerEvent": dict(trigger_event),
            "actionSpan": action_span,
            "spanScope": span_scope,
            "headlineCause": headline_cause,
            "recovery": headline_cause.get("recovery"),
            "spanEvents": span_events,
            "allEvents": all_events,
            "eventsByChannel": events_by_channel,
            "contextWindows": {
                "spanEvents": {
                    "label": span_scope.get("label"),
                    "scope": span_scope,
                    "count": len(span_events),
                },
                "allEvents": {
                    "label": "All-events context window",
                    "count": len(all_events),
                },
                "eventsByChannel": {
                    "label": "Events grouped by diagnostic channel",
                    "channels": {
                        channel: len(events)
                        for channel, events in events_by_channel.items()
                    },
                },
            },
            "channelEventCounts": {
                channel: len(events)
                for channel, events in events_by_channel.items()
            },
            "stopDecision": stop_decision,
            "stopDecisionCorrelation": stop_decision_correlation,
        }

    def _span_events_for_trigger(
        self,
        trigger_event: dict[str, Any],
        events: list[dict[str, Any]],
    ) -> list[dict[str, Any]]:
        """Return the causal span chain for an incident trigger.

        `allEvents` remains the broad chronological window. This narrower list
        is deliberately keyed by the strongest available span identity so stale
        sibling/previous-node events do not appear as the incident cause.
        """
        for key in ("action_span_id", "node_attempt_id", "node_instance_id"):
            value = self._event_text_field(trigger_event, key)
            if value:
                return [
                    event
                    for event in events
                    if self._event_text_field(event, key) == value
                ]
        execution_id = int(trigger_event.get("execution_id") or 0)
        if execution_id > 0:
            return [
                event
                for event in events
                if int(event.get("execution_id") or 0) == execution_id
            ]
        return []

    def _span_scope_summary(self, event: dict[str, Any]) -> dict[str, Any]:
        """Describe the strongest identity used for the span-local chain."""
        for key, label in (
            ("action_span_id", "action span"),
            ("node_attempt_id", "node attempt"),
            ("node_instance_id", "node instance"),
        ):
            value = self._event_text_field(event, key)
            if value:
                return {
                    "field": key,
                    "kind": key.removesuffix("_id"),
                    "value": value,
                    "label": f"Span-local chain scoped by {label}",
                    "attemptExact": key in {"action_span_id", "node_attempt_id"},
                }
        execution_id = int(event.get("execution_id") or 0)
        if execution_id > 0:
            return {
                "field": "kernel_execution_id",
                "kind": "kernel_execution",
                "value": str(execution_id),
                "label": "Span-local chain scoped by kernel execution",
                "attemptExact": False,
            }
        return {
            "field": "",
            "kind": "none",
            "value": "",
            "label": "No span-local identity available",
            "attemptExact": False,
        }

    def _headline_cause(
        self,
        trigger_event: dict[str, Any],
        span_events: list[dict[str, Any]],
    ) -> dict[str, Any]:
        """Build the operator-facing incident headline from the best span event."""
        cause_event = self._select_headline_event(trigger_event, span_events)
        details = cause_event.get("details") if isinstance(cause_event.get("details"), dict) else {}
        details = details if isinstance(details, dict) else {}
        error_code = (
            details.get("reason_code")
            or details.get("error_code")
            or cause_event.get("type")
        )
        raw_message = (
            details.get("error")
            if isinstance(details.get("error"), str)
            else cause_event.get("message")
        )
        message = str(raw_message) if raw_message else str(cause_event.get("type") or "incident")
        move_id = self._event_text_field(cause_event, "move_id")
        node = (
            self._event_text_field(cause_event, "node_instance_id")
            or self._event_text_field(cause_event, "trigger_node_instance")
            or self._event_text_field(cause_event, "node")
        )
        execution_id = int(cause_event.get("execution_id") or 0)

        if (
            str(error_code) == "planning_timeout"
            or "planning timed out" in message.lower()
        ):
            subject = node or move_id or "motion"
            if move_id and node and move_id != node:
                summary = (
                    f"Planning timed out while planning `{subject}` "
                    f"(`{move_id}`"
                    f"{', execution ' + str(execution_id) if execution_id > 0 else ''})."
                )
            else:
                summary = (
                    f"Planning timed out while planning `{subject}`"
                    f"{' (execution ' + str(execution_id) + ')' if execution_id > 0 else ''}."
                )
            summary += " Cycle stopped and can be resumed."
            recovery = "resume"
        else:
            summary = message
            recovery = "inspect_then_resume_or_reset"

        return {
            "eventIndex": cause_event.get("event_index"),
            "eventType": cause_event.get("type"),
            "errorCode": str(error_code) if error_code else None,
            "message": message,
            "summary": summary,
            "recovery": recovery,
            "moveId": move_id,
            "nodeInstanceId": node,
            "kernelExecutionId": execution_id if execution_id > 0 else None,
            "genericWrapperSuppressed": cause_event is not trigger_event,
        }

    def _select_headline_event(
        self,
        trigger_event: dict[str, Any],
        span_events: list[dict[str, Any]],
    ) -> dict[str, Any]:
        """Prefer the span-local structured cause over generic cycle wrappers."""
        candidates = list(span_events) if span_events else [trigger_event]
        for event in reversed(candidates):
            message = event.get("message")
            details = event.get("details") if isinstance(event.get("details"), dict) else {}
            error = details.get("error") if isinstance(details, dict) else None
            text = str(error or message or "")
            event_type = str(event.get("type") or "")
            if (
                event_type in {
                    "error_raised",
                    "kernel_error",
                    "plan_failed",
                    "validation_failed",
                    "execute_failed",
                    "collision_detected",
                    "cycle_error",
                }
                and text
                and text not in _GENERIC_TERMINAL_MESSAGES
            ):
                return event
        return trigger_event

    def _action_span_summary(self, event: dict[str, Any]) -> Optional[dict[str, Any]]:
        """Return a compact camelCase span summary for incident consumers."""
        fields = {
            "action_span_id": "actionSpanId",
            "action_source": "actionSource",
            "action_span_program_run_id": "actionSpanProgramRunId",
            "program_run_id": "programRunId",
            "node_instance_id": "nodeInstanceId",
            "node_attempt_id": "nodeAttemptId",
            "parent_node_attempt_id": "parentNodeAttemptId",
            "contact_policy_id": "contactPolicyId",
            "move_id": "moveId",
        }
        summary: dict[str, Any] = {}
        for source_key, target_key in fields.items():
            value = self._event_text_field(event, source_key)
            if value:
                summary[target_key] = value
        execution_id = int(event.get("execution_id") or 0)
        if execution_id > 0:
            summary["kernelExecutionId"] = execution_id
        return summary or None

    def _enqueue_jsonl_locked(
        self,
        *,
        run_id: Optional[str],
        json_text: str,
        durable: bool = False,
    ) -> None:
        """Hand the serialized event to the JSONL writer thread (audit 6B).

        Path resolution and FIFO ordering are fixed here, under `_lock`;
        only the disk append runs on the writer thread, so a slow disk can
        never stall observers contending on the recorder lock. `durable`
        events (WARN and above — faults, safety stops) are fsync'd so the
        incident that precedes a power loss still has its record on disk.
        """
        queue_ = self._jsonl_queue
        if queue_ is None:
            return
        file_path = self._path_for_run(run_id)
        if file_path is None:
            # No run to attach to (e.g. startup anchor before the first run).
            # Durable events must still reach disk; non-durable steady-state
            # events keep their memory-only behavior.
            if durable and self._trace_dir is not None:
                file_path = self._trace_dir / _NO_RUN_FILENAME
            else:
                return
        try:
            queue_.put_nowait((file_path, json_text, durable))
        except queue.Full:
            self._jsonl_dropped += 1
            self._last_jsonl_error = "jsonl writer queue full"

    def _jsonl_writer_loop(self) -> None:
        queue_ = self._jsonl_queue
        if queue_ is None:  # pragma: no cover - defensive
            return
        while True:
            job = queue_.get()
            try:
                if job is None:
                    return
                file_path, json_text, durable = job
                try:
                    self._write_jsonl_line(file_path, json_text, durable=durable)
                except Exception as exc:  # pragma: no cover - disk failure surface
                    with self._lock:
                        self._last_jsonl_error = str(exc)
            finally:
                queue_.task_done()

    @staticmethod
    def _write_jsonl_line(
        file_path: Path, json_text: str, *, durable: bool = False
    ) -> None:
        with file_path.open("a", encoding="utf-8") as handle:
            handle.write(json_text)
            handle.write("\n")
            if durable:
                # Fault-grade events must survive an immediate power loss —
                # steady-state events may stay in the page cache.
                handle.flush()
                os.fsync(handle.fileno())

    def wait_for_jsonl_idle(self, timeout_s: float = 5.0) -> bool:
        """Wait until queued JSONL appends have hit disk.

        For tests and controlled shutdown; request paths must not call this.
        """
        queue_ = self._jsonl_queue
        if queue_ is None:
            return True
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            if queue_.unfinished_tasks == 0:
                return True
            time.sleep(0.01)
        return queue_.unfinished_tasks == 0

    def close(self, timeout_s: float = 5.0) -> bool:
        """Stop both writer threads via sentinel and join them (audit 6B).

        Returns True when both workers exited within the timeout. Safe to
        call more than once.
        """
        ok = True
        # Order matters: the failure-archive/prune worker emits durable
        # evidence (trace_pruned, archives) through the JSONL writer. Joining
        # it FIRST guarantees a queued prune cannot delete run files after
        # the writer has exited and then enqueue evidence nobody will write —
        # the self-erasing-prune case spec 520 §7.3 exists to prevent.
        for queue_, worker in (
            (self._failure_archive_queue, self._failure_archive_worker),
            (self._jsonl_queue, self._jsonl_worker),
        ):
            if queue_ is None or worker is None:
                continue
            if worker.is_alive():
                queue_.put(None)
            worker.join(timeout=timeout_s)
            ok = ok and not worker.is_alive()
        return ok

    def _maybe_write_failure_archive_locked(self, event: dict[str, Any]) -> None:
        if self._failure_archive_dir is None:
            return
        if event.get("severity") not in {"ERROR", "CRITICAL"}:
            return
        event_type = str(event.get("type") or "")
        if event_type not in _FAILURE_ARCHIVE_EVENT_TYPES:
            return
        queue_ = self._failure_archive_queue
        if queue_ is None:
            return
        run_id = event.get("run_id") if isinstance(event.get("run_id"), str) else None
        trace_file = self._path_for_run(run_id)
        recent_events = [
            self._decode_stored_event(stored)
            for stored in self._events
            if run_id is None or stored.run_id == run_id
        ][-_FAILURE_ARCHIVE_RECENT_EVENT_LIMIT:]
        job = _FailureArchiveJob(
            event=dict(event),
            run_id=run_id,
            trace_file=trace_file,
            recent_events=recent_events,
        )
        try:
            queue_.put_nowait(job)
        except queue.Full:
            self._failure_archive_dropped += 1
            self._last_archive_error = "failure archive queue full"

    def wait_for_failure_archives_idle(self, timeout_s: float = 5.0) -> bool:
        """Wait until queued failure-archive jobs have finished.

        Intended for tests and controlled shutdown paths. Runtime request paths
        should not call this; archive IO is intentionally asynchronous.
        """
        queue_ = self._failure_archive_queue
        if queue_ is None:
            return True
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            if queue_.unfinished_tasks == 0:
                return True
            time.sleep(0.01)
        return queue_.unfinished_tasks == 0

    def _enqueue_trace_prune(self) -> None:
        queue_ = self._failure_archive_queue
        if queue_ is None or self._trace_dir is None:
            return
        try:
            queue_.put_nowait("prune_traces")
        except queue.Full:
            # Retention is best-effort per attempt; the next run-end retries.
            pass

    def _prune_trace_files(self) -> dict[str, Any]:
        """Enforce trace-dir retention budgets (spec 520 §4.3/4.4, plan OW5).

        Whole-run JSONL files are the pruning unit. Age pruning removes runs
        older than `trace_max_age_s`; size pruning removes oldest-first while
        the directory exceeds `trace_max_bytes`, preferring to evict
        INFO-only runs before runs whose index entry recorded ERROR/CRITICAL
        (protected severity tiers, spec 520 §5; runs with no index entry —
        e.g. a crash before run-end — are treated as protected). The current
        run's open file and the run index are never pruned. Every removal is
        recorded as a durable `trace_pruned` event so consumers know data
        was pruned rather than lost.
        """
        summary: dict[str, Any] = {
            "pruned_files": 0,
            "pruned_bytes": 0,
            "error": None,
        }
        if self._trace_dir is None:
            return summary
        try:
            with self._lock:
                current_path = (
                    self._current_run.file_path
                    if self._current_run is not None
                    else None
                )
                severities = dict(self._run_index_severities)
            candidates = []
            total_bytes = 0
            for path in self._trace_dir.glob("*.events.jsonl"):
                if current_path is not None and path == current_path:
                    continue
                if path.name == _NO_RUN_FILENAME:
                    continue
                try:
                    stat = path.stat()
                except OSError:
                    continue
                run_id = path.name[: -len(".events.jsonl")]
                severity = severities.get(run_id)
                protected = severity is None or _SEVERITY_ORDER.get(
                    severity, 20
                ) >= _SEVERITY_ORDER.get("ERROR", 40)
                candidates.append(
                    (stat.st_mtime, path, stat.st_size, run_id, protected)
                )
                total_bytes += stat.st_size
            candidates.sort()  # oldest first

            now = time.time()
            pruned: list[tuple[Path, int, str, str]] = []
            survivors = []
            for mtime, path, size, run_id, protected in candidates:
                if (
                    self._trace_max_age_s is not None
                    and now - mtime > self._trace_max_age_s
                ):
                    pruned.append((path, size, run_id, "age"))
                    total_bytes -= size
                else:
                    survivors.append((mtime, path, size, run_id, protected))
            if self._trace_max_bytes is not None:
                for pass_protected in (False, True):
                    if total_bytes <= self._trace_max_bytes:
                        break
                    for mtime, path, size, run_id, protected in list(survivors):
                        if total_bytes <= self._trace_max_bytes:
                            break
                        if protected != pass_protected:
                            continue
                        pruned.append((path, size, run_id, "size"))
                        survivors.remove((mtime, path, size, run_id, protected))
                        total_bytes -= size
            for path, size, run_id, reason in pruned:
                try:
                    path.unlink()
                except OSError as exc:
                    summary["error"] = str(exc)
                    continue
                summary["pruned_files"] += 1
                summary["pruned_bytes"] += size
                self._record_event(
                    event_type="trace_pruned",
                    execution_id=0,
                    details={
                        "pruned_run_id": run_id,
                        "bytes": str(size),
                        "reason": reason,
                    },
                    system_event=True,
                )
            summary["total_bytes_after"] = total_bytes
        except Exception as exc:  # noqa: BLE001 - retention must not raise
            summary["error"] = str(exc)
        with self._lock:
            self._last_trace_prune = summary
        return summary

    def trace_storage_status(self) -> dict[str, Any]:
        """Trace-directory storage accounting for the heartbeat surface."""
        if self._trace_dir is None:
            return {"enabled": False}
        total_bytes = 0
        file_count = 0
        oldest_mtime: Optional[float] = None
        try:
            for path in self._trace_dir.glob("*.events.jsonl"):
                stat = path.stat()
                total_bytes += stat.st_size
                file_count += 1
                if oldest_mtime is None or stat.st_mtime < oldest_mtime:
                    oldest_mtime = stat.st_mtime
        except OSError:
            pass
        with self._lock:
            last_prune = dict(self._last_trace_prune)
        return {
            "enabled": True,
            "trace_dir": str(self._trace_dir),
            "total_bytes": total_bytes,
            "run_file_count": file_count,
            "oldest_run_wall": (
                datetime.fromtimestamp(oldest_mtime, tz=timezone.utc)
                .isoformat()
                .replace("+00:00", "Z")
                if oldest_mtime is not None
                else None
            ),
            "volatile": str(self._trace_dir).startswith("/tmp"),
            "max_age_s": self._trace_max_age_s,
            "max_bytes": self._trace_max_bytes,
            "last_prune": last_prune,
        }

    def _failure_archive_worker_loop(self) -> None:
        queue_ = self._failure_archive_queue
        if queue_ is None:  # pragma: no cover - defensive
            return
        while True:
            job = queue_.get()
            try:
                # Audit 6B (finding 2.11): None is the shutdown sentinel
                # enqueued by close(); the worker exits cleanly instead of
                # blocking on get() forever.
                if job is None:
                    return
                if job == "prune_traces":
                    try:
                        self._prune_trace_files()
                    except Exception:  # pragma: no cover - defensive
                        pass
                    continue
                try:
                    archive_path = self._write_failure_archive(job)
                    cleanup = self._prune_failure_archives()
                    with self._lock:
                        self._last_failure_archive_id = archive_path.name
                        self._last_failure_archive_path = archive_path
                        self._last_archive_cleanup = cleanup
                        self._last_archive_error = None
                except Exception as exc:  # pragma: no cover - defensive logging surface
                    with self._lock:
                        self._last_archive_error = str(exc)
            finally:
                queue_.task_done()

    def _write_failure_archive(self, job: _FailureArchiveJob) -> Path:
        assert self._failure_archive_dir is not None
        event = job.event
        with self._lock:
            self._failure_archive_count += 1
            archive_count = self._failure_archive_count
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        archive_id = (
            f"incident-{stamp}-{int(event.get('event_index') or 0):06d}"
            f"-{archive_count:06d}"
        )
        archive_path = self._failure_archive_dir / archive_id
        suffix = 1
        while archive_path.exists():
            suffix += 1
            archive_path = self._failure_archive_dir / f"{archive_id}-{suffix:02d}"
        archive_path.mkdir(parents=True, exist_ok=False)

        manifest = {
            "schema_version": "manyforge.failure_archive.v1",
            "archive_id": archive_path.name,
            "created_at": _wall_timestamp_iso(),
            "created_at_ns": time.monotonic_ns(),
            "trigger": {
                "event_index": int(event.get("event_index") or 0),
                "type": event.get("type"),
                "severity": event.get("severity"),
                "message": event.get("message"),
                "run_id": job.run_id,
                "program_run_id": event.get("program_run_id"),
                "intervention_run_id": event.get("intervention_run_id"),
                "execution_id": int(event.get("execution_id") or 0),
                "move_id": event.get("move_id"),
            },
            "trace_file": str(job.trace_file) if job.trace_file is not None else None,
            "retention": {
                "max_age_s": self._failure_archive_max_age_s,
                "max_bytes": self._failure_archive_max_bytes,
                "policy": "fifo_oldest_archive_dirs_first",
            },
            "contents": {
                "manifest": "manifest.json",
                "trigger_event": "trigger_event.json",
                "recent_events": "recent_events.jsonl",
            },
        }

        (archive_path / "manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True),
            encoding="utf-8",
        )
        (archive_path / "trigger_event.json").write_text(
            json.dumps(event, indent=2, sort_keys=True),
            encoding="utf-8",
        )
        with (archive_path / "recent_events.jsonl").open("w", encoding="utf-8") as handle:
            for recent in job.recent_events:
                handle.write(json.dumps(recent, sort_keys=True))
                handle.write("\n")

        return archive_path

    def _prune_failure_archives(self) -> dict[str, Any]:
        if self._failure_archive_dir is None:
            return {
                "deleted_count": 0,
                "deleted_bytes": 0,
                "reason": None,
                "timestamp_ns": time.monotonic_ns(),
            }

        deleted_count = 0
        deleted_bytes = 0
        reasons: set[str] = set()
        records = self._archive_records()
        if self._failure_archive_max_age_s is not None:
            cutoff_s = time.time() - float(self._failure_archive_max_age_s)
            remaining: list[_ArchiveRecord] = []
            for record in records:
                if record.modified_at_s < cutoff_s:
                    deleted_bytes += record.size_bytes
                    deleted_count += 1
                    reasons.add("max_age_s")
                    shutil.rmtree(record.path, ignore_errors=True)
                else:
                    remaining.append(record)
            records = remaining

        if self._failure_archive_max_bytes is not None:
            max_bytes = int(self._failure_archive_max_bytes)
            total_bytes = sum(record.size_bytes for record in records)
            remaining = sorted(records, key=lambda record: record.modified_at_s)
            while total_bytes > max_bytes and remaining:
                record = remaining.pop(0)
                total_bytes -= record.size_bytes
                deleted_bytes += record.size_bytes
                deleted_count += 1
                reasons.add("max_bytes")
                shutil.rmtree(record.path, ignore_errors=True)

        return {
            "deleted_count": deleted_count,
            "deleted_bytes": deleted_bytes,
            "reason": ",".join(sorted(reasons)) if reasons else None,
            "timestamp_ns": time.monotonic_ns(),
        }

    def _archive_records_locked(self) -> list[_ArchiveRecord]:
        return self._archive_records()

    def _archive_records(self) -> list[_ArchiveRecord]:
        if self._failure_archive_dir is None or not self._failure_archive_dir.exists():
            return []
        records: list[_ArchiveRecord] = []
        for path in self._failure_archive_dir.iterdir():
            if not path.is_dir():
                continue
            try:
                stat = path.stat()
                records.append(
                    _ArchiveRecord(
                        path=path,
                        modified_at_s=stat.st_mtime,
                        size_bytes=self._directory_size(path),
                    )
                )
            except OSError:
                continue
        records.sort(key=lambda record: record.modified_at_s)
        return records

    @staticmethod
    def _directory_size(path: Path) -> int:
        total = 0
        for item in path.rglob("*"):
            if not item.is_file():
                continue
            try:
                total += item.stat().st_size
            except OSError:
                continue
        return total
