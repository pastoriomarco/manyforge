"""Execution/event recording for ManyForge Composer observability surfaces."""
from __future__ import annotations

from collections import deque
from copy import deepcopy
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import threading
import time
from typing import Any, Optional


@dataclass
class _RunInfo:
    run_id: str
    started_at_ns: int
    file_path: Optional[Path] = None


class EventRecorder:
    """Record execution/cycle/node events with bounded retention and JSONL sink."""

    def __init__(
        self,
        *,
        trace_dir: str | Path | None = None,
        max_events: int = 4096,
    ) -> None:
        self._trace_dir = Path(trace_dir) if trace_dir is not None else None
        if self._trace_dir is not None:
            self._trace_dir.mkdir(parents=True, exist_ok=True)

        self._events: deque[dict[str, Any]] = deque(maxlen=max_events)
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

        self._lock = threading.Lock()

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
                deepcopy(event)
                for event in self._events
                if event["event_index"] > event_index
                and (include_node_transitions or event["type"] != "node_status_transition")
            ]
        return result

    def history(
        self,
        *,
        run_id: Optional[str] = None,
        limit: Optional[int] = None,
    ) -> dict[str, Any]:
        selected_run_id = run_id or self.last_run_id
        events = self._load_events_for_run(selected_run_id, limit=limit)

        regular_events = [event for event in events if event["type"] != "node_status_transition"]
        node_transitions = [
            {
                "timestamp_ns": event["timestamp_ns"],
                "run_id": event.get("run_id"),
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
    ) -> list[dict[str, Any]]:
        return self._load_events_for_run(run_id or self.last_run_id)

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
            payload["params"] = deepcopy(params)
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
                payload["argument_values"] = deepcopy(argument_values)

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
                payload["response_fields"] = deepcopy(response_fields)
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
                events = [deepcopy(event) for event in self._events]
                if limit is not None and limit >= 0:
                    return events[-limit:]
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
            events = [deepcopy(event) for event in self._events if event.get("run_id") == run_id]
            if limit is not None and limit >= 0:
                return events[-limit:]
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
            self._record_event(
                event_type="kernel_state_transition",
                execution_id=current_execution_id,
                move_id=current_move_id,
                details={
                    "from": previous_state,
                    "to": current_state,
                    "progress": status.progress,
                    "error": snapshot.error,
                },
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
        timestamp_ns = timestamp_ns or time.monotonic_ns()
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
            event: dict[str, Any] = {
                "event_index": self._event_index,
                "timestamp_ns": timestamp_ns,
                "run_id": run_id,
                "execution_id": execution_id,
                "type": event_type,
            }
            if move_id:
                event["move_id"] = move_id
            if details:
                event["details"] = details
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

            self._events.append(deepcopy(event))
            self._append_jsonl_locked(event)

    def _intervention_run_id(self, session) -> str:
        """Return the correlated run_id for an intervention session."""
        return session.parent_run_id or f"intervention-{session.session_id}"

    def _handle_intervention_events(self, session) -> None:
        """Project intervention session events into the unified recorder stream."""
        if session is None:
            return

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
            self._last_intervention_event_index = max(
                self._last_intervention_event_index,
                int(event.get("event_index", 0)),
            )

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

    def _append_jsonl_locked(self, event: dict[str, Any]) -> None:
        run_id = event.get("run_id")
        file_path = self._path_for_run(run_id)
        if file_path is None:
            return
        with file_path.open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(event, sort_keys=True))
            handle.write("\n")
