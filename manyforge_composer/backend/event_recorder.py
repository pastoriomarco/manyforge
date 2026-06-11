"""Execution/event recording for ManyForge Composer observability surfaces."""
from __future__ import annotations

from collections import deque
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import queue
import shutil
from pathlib import Path
import threading
import time
from typing import Any, Optional

_ALLOWED_EVENT_TYPES = {
    "cache_hit",
    "cycle_error",
    "cycle_started",
    "cycle_state_changed",
    "delegated_recovery_requested",
    "collision_detected",
    "error_raised",
    "execute_completed",
    "execute_failed",
    "execute_started",
    "intervention_required",
    "kernel_error",
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
}
_ALLOWED_EVENT_PREFIXES = (
    "intervention_",
    "model_call_",
    "pose_source_",
    "runtime_override_",
    "simulation_control_",
)
_SEVERITY_ORDER = {"DEBUG": 10, "INFO": 20, "WARN": 30, "WARNING": 30, "ERROR": 40, "CRITICAL": 50}
_FAILURE_ARCHIVE_EVENT_TYPES = {
    "cycle_error",
    "execute_failed",
    "intervention_required",
    "plan_failed",
    "validation_failed",
}
_FAILURE_ARCHIVE_RECENT_EVENT_LIMIT = 1000
_FAILURE_ARCHIVE_QUEUE_SIZE = 16
_DEFAULT_FAILURE_ARCHIVE_MAX_AGE_S = 30 * 24 * 60 * 60
_DEFAULT_FAILURE_ARCHIVE_MAX_BYTES = 1024 * 1024 * 1024
_DEFAULT_HISTORY_LIMIT = 1000
_MAX_HISTORY_LIMIT = 10000
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


def _is_resume_audit_native_event(native_event: dict[str, Any]) -> bool:
    details = native_event.get("details")
    if not isinstance(details, dict):
        return False
    return (
        details.get("reason") == "resume_replan"
        or str(details.get("resume_replan", "")).lower() == "true"
    )


def _wall_timestamp_iso() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


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
        "intervention_required",
        "error_raised",
        "collision_detected",
    }:
        return "ERROR"
    if event_type.endswith("_failed"):
        return "ERROR"
    if details and details.get("error"):
        return "ERROR"
    return "INFO"


def _component_for_event(event_type: str) -> str:
    if event_type.startswith("model_call_"):
        return "manyforge.model"
    if event_type.startswith("pose_source_"):
        return "manyforge.scene"
    if event_type.startswith("simulation_control_"):
        return "manyforge.simulation"
    if event_type.startswith("runtime_override_"):
        return "manyforge.runtime_override"
    if event_type.startswith("intervention_"):
        return "manyforge.intervention"
    if event_type.startswith("cycle_"):
        return "manyforge.cycle"
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

        self._events: deque[_StoredEvent] = deque(maxlen=max_events)
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

        self._lock = threading.RLock()
        self._failure_archive_queue: queue.Queue[_FailureArchiveJob] | None = None
        self._failure_archive_worker: threading.Thread | None = None
        if self._failure_archive_dir is not None:
            self._failure_archive_queue = queue.Queue(maxsize=_FAILURE_ARCHIVE_QUEUE_SIZE)
            self._failure_archive_worker = threading.Thread(
                target=self._failure_archive_worker_loop,
                name="manyforge-failure-archive",
                daemon=True,
            )
            self._failure_archive_worker.start()

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
            }

    def observe(self, state) -> None:
        """Poll the current ComposerState and record semantic deltas."""
        cycle_manager = getattr(state, "cycle_manager", None)
        kernel = getattr(state, "_kernel", None)

        cycle_state = cycle_manager.state.value if cycle_manager is not None else None
        if cycle_state != self._last_cycle_state:
            self._handle_cycle_state_transition(self._last_cycle_state, cycle_state)
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
            cycle_error = cycle_manager.error
            if cycle_error != self._last_cycle_error and cycle_error:
                self._record_event(
                    event_type="cycle_error",
                    execution_id=status.execution_id if status is not None else 0,
                    move_id=status.active_move_id if status is not None else None,
                    details={"error": cycle_error},
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

    def record_native_kernel_events(self, events: list[dict[str, Any]]) -> None:
        """Project selected native kernel events into durable history.

        The native kernel event buffer is intentionally fine-grained and may
        include high-rate target-received/dispatched events. The recorder keeps
        durable trace volume bounded by recording session lifecycle, explicit
        hazard/error events, and resume-scoped replanning audit events only.
        `event_index` dedupes events drained through both `/api/kernel/events`
        and the websocket hub.
        """
        for native_event in events:
            native_type = str(native_event.get("type") or "")
            native_index = int(native_event.get("event_index") or 0)
            should_record = native_type in _NATIVE_KERNEL_ERROR_EVENT_TYPES_TO_RECORD
            should_record = should_record or (
                native_type in _NATIVE_KERNEL_RESUME_AUDIT_EVENT_TYPES_TO_RECORD
                and _is_resume_audit_native_event(native_event)
            )
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

            self._record_event(
                event_type=native_type,
                execution_id=int(native_event.get("execution_id") or 0),
                details=details or None,
                execution_mode=None,
                timestamp_ns=int(native_event.get("timestamp_ns") or time.monotonic_ns()),
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
        program_run_id: Optional[str] = None,
        intervention_run_id: Optional[str] = None,
    ) -> dict[str, Any]:
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
            program_run_id=program_run_id,
            intervention_run_id=intervention_run_id,
        )
        if effective_limit == 0:
            events = []
        else:
            events = events[-effective_limit:]

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
            "execution_id": execution_id,
            "events": regular_events,
            "node_status_transitions": node_transitions,
            "timestamp_ns": time.monotonic_ns(),
        }

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
        program_run_id: Optional[str] = None,
        intervention_run_id: Optional[str] = None,
    ) -> list[dict[str, Any]]:
        return self._filter_events(
            self._load_events_for_run(run_id or self.last_run_id),
            start_ns=start_ns,
            end_ns=end_ns,
            min_severity=min_severity,
            components=components,
            event_types=event_types,
            execution_id=execution_id,
            program_run_id=program_run_id,
            intervention_run_id=intervention_run_id,
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
        program_run_id: Optional[str] = None,
        intervention_run_id: Optional[str] = None,
    ) -> list[dict[str, Any]]:
        """Apply server-side query filters to decoded event dictionaries."""
        normalized_components = [item for item in (components or []) if item]
        normalized_event_types = {item for item in (event_types or []) if item}
        severity_floor = None
        if min_severity is not None:
            severity_floor = _SEVERITY_ORDER.get(min_severity.upper())
            if severity_floor is None:
                raise ValueError(f"Unsupported severity level: {min_severity}")

        filtered: list[dict[str, Any]] = []
        for event in events:
            timestamp_ns = event.get("timestamp_ns")
            if start_ns is not None and (not isinstance(timestamp_ns, int) or timestamp_ns < start_ns):
                continue
            if end_ns is not None and (not isinstance(timestamp_ns, int) or timestamp_ns > end_ns):
                continue
            if severity_floor is not None:
                current = _SEVERITY_ORDER.get(str(event.get("severity", "")).upper(), 0)
                if current < severity_floor:
                    continue
            component = str(event.get("component", ""))
            if normalized_components and not any(
                component == prefix or component.startswith(f"{prefix}.")
                for prefix in normalized_components
            ):
                continue
            if normalized_event_types and event.get("type") not in normalized_event_types:
                continue
            if execution_id is not None and int(event.get("execution_id") or 0) != execution_id:
                continue
            if program_run_id is not None and event.get("program_run_id") != program_run_id:
                continue
            if intervention_run_id is not None and event.get("intervention_run_id") != intervention_run_id:
                continue
            filtered.append(event)
        return filtered

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
        for key in ("from_state", "to_state"):
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
                for line in path.read_text(encoding="utf-8").splitlines():
                    if not line.strip():
                        continue
                    events.append(json.loads(line))
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
                window.append(json.loads(stripped))
        return list(window)

    def _handle_cycle_state_transition(
        self,
        previous_state: Optional[str],
        current_state: Optional[str],
    ) -> None:
        if current_state == "running" and previous_state in {None, "idle"}:
            self._start_run()

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
    ) -> None:
        details: dict[str, Any] = {"progress": progress}
        if error:
            details["error"] = error

        event_type = self._TRANSITION_TABLE.get((previous_state, current_state))

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
    ) -> None:
        _validate_event_type(event_type)
        timestamp_ns = timestamp_ns or time.monotonic_ns()
        wall_timestamp = _wall_timestamp_iso()
        with self._lock:
            if run_id is None:
                run_id = self._current_run.run_id if self._current_run is not None else (
                    self._last_run.run_id if self._last_run is not None else None
                )
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
                "severity": _severity_for_event(event_type, details_payload),
                "component": _component_for_event(event_type),
            }
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
            self._events.append(
                _StoredEvent(
                    event_index=self._event_index,
                    run_id=run_id,
                    event_type=event_type,
                    json_text=json_text,
                )
            )
            self._append_jsonl_locked(run_id=run_id, json_text=json_text)
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
            new_events = [
                event
                for event in session.events
                if event.get("event_index", 0) > self._last_intervention_event_index
            ]
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

    def _start_run(self) -> None:
        now_ns = time.monotonic_ns()
        self._run_counter += 1
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        run_id = f"run-{stamp}-{self._run_counter:03d}"
        file_path = self._path_for_run(run_id)
        self._current_run = _RunInfo(run_id=run_id, started_at_ns=now_ns, file_path=file_path)
        self._last_run = self._current_run

    def _finish_current_run(self) -> None:
        if self._current_run is not None:
            self._last_run = self._current_run
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

    def _append_jsonl_locked(self, *, run_id: Optional[str], json_text: str) -> None:
        file_path = self._path_for_run(run_id)
        if file_path is None:
            return
        with file_path.open("a", encoding="utf-8") as handle:
            handle.write(json_text)
            handle.write("\n")

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
        job = _FailureArchiveJob(
            event=dict(event),
            run_id=run_id,
            trace_file=trace_file,
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

    def _failure_archive_worker_loop(self) -> None:
        queue_ = self._failure_archive_queue
        if queue_ is None:  # pragma: no cover - defensive
            return
        while True:
            job = queue_.get()
            try:
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
            for recent in self._load_events_for_run(
                job.run_id,
                limit=_FAILURE_ARCHIVE_RECENT_EVENT_LIMIT,
            ):
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
