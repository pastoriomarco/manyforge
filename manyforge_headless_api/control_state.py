"""Demand-driven current-state source for the UI6-G control session.

The source is deliberately small: Program Runner wakes it through one private
revision stream, filesystem owners wake one cold-combine bit, and each browser
holds only the newest assembled value.  It is neither an event log nor a
semantic owner.
"""

from __future__ import annotations

import asyncio
from collections import deque
from contextlib import aclosing
from dataclasses import dataclass, replace
from functools import partial
import json
import logging
from pathlib import Path
import time
import threading
from typing import Any, Callable, Iterable

import rfc8785
from watchfiles import awatch
from websockets.asyncio.client import connect as connect_websocket
from websockets.exceptions import ConnectionClosed

from .control_protocol import (
    CONTROL_COLD_COMBINE_NS,
    CONTROL_MAXIMUM_CLIENTS,
    CONTROL_MAXIMUM_SNAPSHOT_BYTES,
    EXECUTION_CONTROL_PROJECTION_SCHEMA,
    ExecutionControlProjectionDTO,
)
from .component_overview import ComponentOverviewFacts
from .execution_control import ExecutionControlIngress
from .held_run_authoring import held_run_draft_edit_allowed
from .native import HeadlessApiError, strict_json_object
from .program_cycle import (
    PROGRAM_CONTROL_STATE_SCHEMA,
    ProgramCycleFrameDTO,
    program_cycle_control_key,
    program_operation_failures,
)
from .service import HeadlessService


_MAXIMUM_ACTIVE_CLIENT_INCIDENTS = CONTROL_MAXIMUM_CLIENTS * 8
_SOURCE_RETRY_INITIAL_SECONDS = 0.05
_SOURCE_RETRY_MAX_SECONDS = 1.0
_LOGGER = logging.getLogger(__name__)
_PROGRAM_FAILURE_CONDITION_SCHEMA = "manyforge.program_failure_condition.v1"
# Run states holding an unresolved failure: held for it, ended failed, or
# stopped with it. Only the Run's own resolution clears the condition.
_ORDINARY_FAILURE_EFFECTS = frozenset({"no_effect", "effect_settled"})
# These keep their own native alarms (runtime collision, Stop fault) or are
# no failure at all (an operator Stop).
_NATIVE_ALARM_RECOVERY_CLASSES = frozenset(
    {"runtime_collision", "stop_fault_intervention", "ordinary_operator_stop"}
)


async def _drained_owner_read(
    callback: Callable[..., Any], *, stop: threading.Event | None = None, **kwargs: Any
) -> Any:
    """Keep the one physical read alive until teardown can actually drain it.

    Cancelling to_thread's waiter cannot cancel a running native/HTTP call.
    Shielding and draining it lets the existing lifecycle lock serialize source
    generations, without another executor, work queue, or detached read.
    """
    read = asyncio.create_task(asyncio.to_thread(callback, **kwargs))
    try:
        return await asyncio.shield(read)
    except asyncio.CancelledError:
        if stop is not None:
            stop.set()
        await asyncio.gather(read, return_exceptions=True)
        raise


@dataclass(frozen=True, slots=True)
class ControlStateFrame:
    generation: int
    event_class: str
    control: ExecutionControlProjectionDTO
    shell: dict[str, Any]


@dataclass(eq=False)
class ControlStateSubscription:
    wake: asyncio.Event
    frame: ControlStateFrame | None = None
    closed: bool = False


class ControlStateUnavailableError(RuntimeError):
    """The owner inputs do not yet form one coherent control snapshot."""


@dataclass(frozen=True, slots=True)
class ControlSemanticContext:
    """Cached semantic availability used by heartbeat without owner reads."""

    status: str
    reason: str | None
    semantic_owner_epoch: str | None = None
    execution_control_revision: int | None = None


def _runtime_payload_key(name: str, payload: Any) -> str:
    """Match the native control-file hint's observation-only exclusions.

    Keep this aligned with runtime_snapshot.cpp::ControlPayload. The hint
    wakes collection; this key governs both publication and session deltas.
    Dropping a semantic change here would strand already-connected clients.
    """
    if isinstance(payload, dict):
        payload = dict(payload)
        if name == "manager" and isinstance(payload.get("runtime"), dict):
            runtime = dict(payload["runtime"])
            rows = runtime.get("observed_units")
            if isinstance(rows, list):
                runtime["observed_units"] = [
                    {
                        key: value for key, value in row.items()
                        if key not in {
                            "process_observation_sequence", "source_report_sequence"
                        }
                    } if isinstance(row, dict) else row
                    for row in rows
                ]
            payload["runtime"] = runtime
        elif name == "scene_runtime" and isinstance(payload.get("state"), dict):
            payload["state"] = {
                key: value for key, value in payload["state"].items()
                if key not in {"pose_sources", "updated_at_ns"}
            }
    return json.dumps(payload, sort_keys=True, separators=(",", ":"))


def _meaningful_runtime_key(runtime: dict[str, Any]) -> tuple[Any, ...]:
    """Ignore aggregate receiver heartbeats while retaining owner changes."""

    sections = runtime.get("sections")
    sections = sections if isinstance(sections, dict) else {}

    def source(name: str, generation: bool) -> tuple[Any, ...]:
        row = sections.get(name)
        row = row if isinstance(row, dict) else {}
        return (
            row.get("available"),
            row.get("freshness"),
            tuple(row.get("known_gaps", ())),
            row.get("source_owner_id"),
            row.get("source_owner_epoch"),
            row.get("source_generation") if generation else None,
            row.get("source_content_digest") if generation else None,
            _runtime_payload_key(name, row.get("payload")) if not generation else None,
        )

    return (
        runtime.get("projection_owner_id"),
        runtime.get("projection_owner_epoch"),
        runtime.get("complete"),
        tuple(runtime.get("reasons", ())),
        tuple(runtime.get("unavailable_sections", ())),
        source("manager", False),
        source("logical_scene", True),
        # Scene-runtime generations also advance for high-rate pose samples.
        # Preserve initialization/error meaning in the payload key; motion
        # itself belongs to the capacity-one Viewer stream.
        source("scene_runtime", False),
    )


def _meaningful_alarm_key(alarms: dict[str, Any]) -> str:
    """Exclude recorder time/counters that do not change alarm meaning."""

    payload = {
        key: value
        for key, value in alarms.items()
        if key not in {"state_revision", "timestamp_ns"}
    }
    return json.dumps(payload, sort_keys=True, separators=(",", ":"))


def _meaningful_overview_key(overview: dict[str, Any]) -> str:
    """Compare operator status, not reduction/publication bookkeeping."""

    payload = {
        key: value
        for key, value in overview.items()
        if key not in {"overview_revision", "basis"}
    }
    components = payload.get("components")
    if isinstance(components, list):
        payload["components"] = [
            (
                {key: value for key, value in row.items() if key != "generation"}
                if isinstance(row, dict)
                else row
            )
            for row in components
        ]
    return json.dumps(payload, sort_keys=True, separators=(",", ":"))


def _meaningful_execution_control_key(control_response: Any) -> str:
    """Keep control meaning while motion samples remain Viewer-only."""

    control = dict(control_response) if isinstance(control_response, dict) else {}
    state = control.get("control_state")
    if isinstance(state, dict):
        state = dict(state)
        # execution_control_state.v6: samples live only in observed_targets;
        # keep the row identities, drop the Viewer-only observation values.
        rows = state.get("observed_targets")
        if isinstance(rows, list):
            state["observed_targets"] = [
                {key: value for key, value in row.items() if key != "observed"}
                if isinstance(row, dict)
                else row
                for row in rows
            ]
        control["control_state"] = state
    return json.dumps(control, sort_keys=True, separators=(",", ":"))


def execution_observation_values_key(control_response: Any) -> str:
    """Return only the observed joint values that the meaningful key drops.

    Samples are not published while a motion runs, but a robot that has come
    to rest must not leave an older sample as its control-state pose.  This
    key detects that difference for a settle refresh and for session deltas.
    """

    control = control_response if isinstance(control_response, dict) else {}
    state = control.get("control_state")
    rows = state.get("observed_targets") if isinstance(state, dict) else None
    values = [
        (
            row.get("controllable_component_id"),
            row.get("command_target_id"),
            row.get("observed"),
        )
        for row in rows
        if isinstance(row, dict)
    ] if isinstance(rows, list) else []
    return json.dumps(values, sort_keys=True, separators=(",", ":"))


def control_shell_fact_key(shell: dict[str, Any], name: str) -> Any:
    """Return one immutable domain's operator-relevant equality key."""

    if name == "runtime":
        return _meaningful_runtime_key(shell[name])
    if name == "alarms":
        return _meaningful_alarm_key(shell[name])
    if name == "component_overview":
        return _meaningful_overview_key(shell[name])
    if name == "execution_targets":
        return json.dumps(shell.get(name), sort_keys=True, separators=(",", ":"))
    if name == "execution_control_state":
        return _meaningful_execution_control_key(shell.get(name))
    if name == "authoring_admission":
        value = shell[name]
        return (
            value.get("authoring_allowed"),
            value.get("admission_generation"),
        )
    if name == "program_cycle":
        return json.dumps(shell[name], sort_keys=True, separators=(",", ":"))
    return shell.get(name)


def _shell_key(shell: dict[str, Any]) -> tuple[Any, ...]:
    return (
        control_shell_fact_key(shell, "runtime"),
        control_shell_fact_key(shell, "alarms"),
        control_shell_fact_key(shell, "authoring_admission"),
        control_shell_fact_key(shell, "held_run_draft_edit_allowed"),
        control_shell_fact_key(shell, "execution_targets"),
        control_shell_fact_key(shell, "execution_control_state"),
        control_shell_fact_key(shell, "component_overview"),
    )


def _source_file_observation(path: Path) -> tuple[str | None, int | None]:
    """Use the existing native codec off-loop, without holding Python's GIL."""
    from manyforge_headless_py import read_control_source_file_hint

    return read_control_source_file_hint(str(path)) or (None, None)


def program_failure_origins(cycle: dict[str, Any]) -> dict[str, dict[str, Any]]:
    """Ordinary operation failures the accepted Run currently holds, by origin.

    The Runner publishes one row per active failure hold (and one for a failed
    tree's tip), drops it when that hold is released and keeps the rest on a
    stopped or failed Run until Reset; row presence alone is the hold. The
    row's selected cause decides: its recovery class must not keep its own
    native alarm, and no participant's terminal fact may show an uncertain
    or still-active effect. The facts are every participant's raw fact
    (SX-1985), so a co-participant that succeeded or failed differently
    never suppresses the alarm. A pure logic failure carries no operation
    row and never activates the alarm.
    """

    run = cycle.get("run")
    if not isinstance(run, dict):
        return {}
    origins: dict[str, dict[str, Any]] = {}
    for row in program_operation_failures(run):
        facts = row["terminal_facts"]
        if (
            row["recovery_class"] in _NATIVE_ALARM_RECOVERY_CLASSES
            or not facts
            or any(fact["effect"] not in _ORDINARY_FAILURE_EFFECTS for fact in facts)
        ):
            continue
        origin = {
            "program_runner_owner_epoch": cycle["program_runner_owner_epoch"],
            "program_run_id": cycle["program_run_id"],
            "occurrence": {
                "node_instance_id": row["node_instance_id"],
                "node_attempt_id": row["node_attempt_id"],
                "operator_facing_run_revision": row["operator_facing_run_revision"],
            },
            "operation": dict(row["operation"]),
            "resource_ids": list(row["resource_ids"]),
            "failure": dict(row["failure"]),
        }
        key = json.dumps(
            [origin[name] for name in (
                "program_runner_owner_epoch", "program_run_id", "occurrence", "operation"
            )],
            sort_keys=True,
            separators=(",", ":"),
        )
        origins.setdefault(key, origin)
    return origins


@dataclass(slots=True)
class _ProgramFailureOccurrence:
    origin: dict[str, Any]
    activation_control_revision: int
    run_revision: int


def build_control_projection(
    cycle: dict[str, Any], ingress: ExecutionControlIngress
) -> ExecutionControlProjectionDTO:
    owner_epoch = cycle.get("program_runner_owner_epoch")
    revision = cycle.get("execution_control_revision")
    if not isinstance(owner_epoch, str) or not owner_epoch:
        raise HeadlessApiError(
            "execution_control_projection_invalid",
            "Program owner omitted its owner epoch",
        )
    if type(revision) is not int or revision < 1:
        raise HeadlessApiError(
            "execution_control_projection_invalid",
            "Program owner omitted its control revision",
        )
    failure = cycle.get("failure")
    state = cycle.get("state")
    return ExecutionControlProjectionDTO.model_validate(
        {
            "schema_version": EXECUTION_CONTROL_PROJECTION_SCHEMA,
            "receiver_incarnation": ingress.receiver_incarnation,
            "semantic_owner_epoch": owner_epoch,
            "execution_control_revision": revision,
            "cycle_state": state,
            "cycle_generation": cycle.get("cycle_generation"),
            "program_run_id": cycle.get("program_run_id"),
            "run_revision": cycle.get("run_revision"),
            "allowed_commands": cycle.get("allowed_commands"),
            "causal_correlation_id": cycle.get("causal_correlation_id"),
            "causal_session_id": cycle.get("causal_session_id"),
            "causal_client_sequence": cycle.get("causal_client_sequence"),
            "current_rejection": None,
            "current_failure": failure,
            "recovery_required": state == "recovery_required",
            "freshness": ingress.freshness_for(owner_epoch, revision),
        }
    )


class HeadlessControlStateSource:
    """One demand-scoped Program stream and one capacity-one fanout."""

    def __init__(
        self,
        service: HeadlessService,
        ingress: ExecutionControlIngress,
        *,
        program_runner_websocket_url: str | None,
        cold_watch_paths: Iterable[Path] = (),
        cold_freshness_paths: Iterable[Path] = (),
        cold_freshness_budget_ns: int | None = None,
        incident_recorder: Callable[..., dict[str, Any]] | None = None,
        program_failure_observer: Callable[[str], dict[str, Any]] | None = None,
    ) -> None:
        self._service = service
        self._ingress = ingress
        self._runner_url = program_runner_websocket_url
        self._watch_paths = tuple(
            dict.fromkeys(Path(path).absolute() for path in cold_watch_paths)
        )
        self._watch_roots = tuple(
            dict.fromkeys(path.parent for path in self._watch_paths)
        )
        self._freshness_paths = frozenset(
            Path(path).absolute() for path in cold_freshness_paths
        )
        if cold_freshness_budget_ns is not None and cold_freshness_budget_ns < 1:
            raise ValueError("cold freshness budget must be positive")
        self._cold_freshness_budget_ns = cold_freshness_budget_ns
        self._incident_recorder = incident_recorder
        self._diagnostic_origin_ns = time.monotonic_ns()
        self._diagnostics = {
            "owner_reads": 0,
            "reductions": 0,
            "unchanged_reductions": 0,
            "state_replacements": 0,
            "bytes_sent": 0,
            "heartbeat_challenges": 0,
            "heartbeat_echoes": 0,
            "delayed_transitions": 0,
            "stale_transitions": 0,
            "source_failures": 0,
            "incident_recording_failures": 0,
        }
        self._publications = {"critical": 0, "node_progress": 0, "cold": 0}
        self._messages = {
            "server_hello": 0,
            "heartbeat_challenge": 0,
            "snapshot": 0,
            "delta": 0,
            "command_receipt": 0,
            "resync_required": 0,
        }
        self._heartbeat_rtt_ns: deque[int] = deque(maxlen=256)
        self._active_client_incidents: dict[tuple[str, str], tuple[str, int, str]] = {}
        self._subscriptions: set[ControlStateSubscription] = set()
        self._loop: asyncio.AbstractEventLoop | None = None
        self._runner_task: asyncio.Task[None] | None = None
        self._cold_task: asyncio.Task[None] | None = None
        self._watch_task: asyncio.Task[None] | None = None
        self._cold_expiry_handle: asyncio.TimerHandle | None = None
        self._watch_keys: dict[Path, str | None] = {}
        self._watch_modified: dict[Path, int | None] = {}
        self._watch_fresh: dict[Path, bool] = {}
        self._lifecycle_lock = asyncio.Lock()
        self._closed = False
        self._generation = 0
        self._cycle: dict[str, Any] | None = None
        self._facts: ComponentOverviewFacts | None = None
        self._shell: dict[str, Any] | None = None
        self._shell_key: tuple[Any, ...] | None = None
        # Joint values last published to clients, and whether a motion has
        # settled since (see notify_motion_settled).
        self._published_observation_key: str | None = None
        self._observation_refresh_requested = False
        self._cold_wake: asyncio.Event | None = None
        self._program_wake: asyncio.Event | None = None
        self._program_source_current = False
        self._watch_source_current = True
        self._source_incidents: dict[str, tuple[int, int]] = {}
        self._incident_recording_failed = False
        self._semantic_reason: str | None = None
        # Event-pump facts carried beside the shell: operation progress and
        # the global rule states (None: unknown).
        self._pump_facts: dict[str, Any] = {
            "operation_progress": [], "global_rules": None,
        }
        self._progress_lock = threading.Lock()
        self._pending_pump_facts: dict[str, Any] = {}
        # Application-lifetime demand for the Program feed alone, so the
        # Program-failure condition follows the Run without a browser.
        self._program_demand = False
        self._program_failure_observer = program_failure_observer
        self._program_failure_epoch: str | None = None
        self._program_failure_control_revision: int | None = None
        self._program_failures: dict[str, _ProgramFailureOccurrence] = {}
        self._program_failures_resend = False
        self._program_failure_observer_failed = False

    def notify_operation_progress(self, rows: list[dict[str, Any]]) -> None:
        self._notify_pump_fact("operation_progress", rows)

    def notify_global_rules(self, rows: list[dict[str, Any]] | None) -> None:
        self._notify_pump_fact("global_rules", rows)

    def _notify_pump_fact(self, name: str, value: Any) -> None:
        # Keep one latest value even before the first browser subscribes.
        with self._progress_lock:
            scheduled = bool(self._pending_pump_facts)
            self._pending_pump_facts[name] = value
        loop = self._loop
        if loop is not None and not loop.is_closed():
            if not scheduled:
                loop.call_soon_threadsafe(self._drain_pump_facts)

    def _drain_pump_facts(self) -> None:
        with self._progress_lock:
            facts, self._pending_pump_facts = self._pending_pump_facts, {}
        changed = {
            name: value for name, value in facts.items()
            if value != self._pump_facts[name]
        }
        if not changed:
            return
        self._pump_facts = {**self._pump_facts, **changed}
        # Reuse already-captured owner facts. Pump facts never rebuild the
        # cold projection or grant new command authority.
        self._publish("cold")

    @property
    def active_clients(self) -> int:
        return len(self._subscriptions)

    def diagnostics(self) -> dict[str, Any]:
        """Return fixed-size control-plane counters without touching an owner."""

        samples = sorted(self._heartbeat_rtt_ns)

        def percentile(fraction: float) -> int | None:
            if not samples:
                return None
            return samples[min(len(samples) - 1, int((len(samples) - 1) * fraction))]

        return {
            "schema_version": "manyforge.control_plane_diagnostics.v1",
            "active_clients": len(self._subscriptions),
            **self._diagnostics,
            "publications": dict(self._publications),
            "messages_sent": dict(self._messages),
            "heartbeat_rtt_sample_count": len(samples),
            "heartbeat_rtt_p50_ns": percentile(0.50),
            "heartbeat_rtt_p95_ns": percentile(0.95),
            "heartbeat_rtt_p99_ns": percentile(0.99),
            "heartbeat_rtt_max_ns": samples[-1] if samples else None,
        }

    def record_message(self, kind: str, byte_count: int) -> None:
        self._messages[kind] = self._messages.get(kind, 0) + 1
        self._diagnostics["bytes_sent"] += byte_count
        if kind == "heartbeat_challenge":
            self._diagnostics["heartbeat_challenges"] += 1

    def record_state_replacement(self) -> None:
        self._diagnostics["state_replacements"] += 1

    def record_heartbeat_echo(self, round_trip_ns: int) -> None:
        self._diagnostics["heartbeat_echoes"] += 1
        self._heartbeat_rtt_ns.append(max(0, round_trip_ns))

    def record_delay_transition(self, stale: bool) -> None:
        key = "stale_transitions" if stale else "delayed_transitions"
        self._diagnostics[key] += 1

    def record_incident(self, **record: Any) -> None:
        """Schedule one sparse native enqueue outside transport hot paths.

        The native recorder itself is the existing bounded asynchronous
        Historian ingress. Scheduling that constant-time enqueue for the next
        event-loop turn avoids both transport-path work and an additional
        unbounded Python executor queue.
        """

        recorder = self._incident_recorder
        loop = self._loop
        if recorder is None or loop is None:
            return
        # Copy only fixed scalar references now. The queued recorder must not
        # substitute a later cycle/scene for the incident-time source facts.
        record["causal_context"] = self._incident_context()
        callback = partial(self._record_incident_now, recorder, record)
        loop.call_soon(callback)

    def _incident_context(self) -> dict[str, str]:
        def mapping(value: Any) -> dict:
            return value if isinstance(value, dict) else {}

        cycle = self._cycle or {}
        capture = mapping(mapping(cycle.get("run")).get("capture"))
        runtime = self._facts.runtime if self._facts is not None else None
        scene = mapping(mapping(mapping(runtime).get("sections")).get("logical_scene"))
        projection = mapping(
            mapping(mapping(scene.get("payload")).get("access")).get(
                "active_projection"
            )
        )
        scene_revision = mapping(projection.get("scene_revision"))
        context: dict[str, str] = {
            "program_source_current": str(self._program_source_current).lower(),
            "cold_source_current": str(self._watch_source_current).lower(),
        }
        for source, names in (
            (
                cycle,
                {
                    key: key
                    for key in (
                        "program_run_id",
                        "program_runner_owner_epoch",
                        "cycle_generation",
                        "execution_control_revision",
                        "run_revision",
                        "reset_phase",
                    )
                }
                | {"state": "cycle_state"},
            ),
            (
                capture,
                {
                    key: key
                    for key in (
                        "program_id",
                        "program_revision",
                        "graph_revision",
                        "retained_bundle_id",
                        "program_binding_id",
                        "node_environment_digest",
                    )
                },
            ),
            (
                scene,
                {
                    key: f"scene_{key}"
                    for key in (
                        "source_owner_id",
                        "source_owner_epoch",
                        "source_generation",
                    )
                }
                | {"freshness": "scene_source_freshness"},
            ),
            (
                scene_revision,
                {
                    "scene_id": "scene_id",
                    "revision_id": "scene_revision_id",
                    "content_hash": "scene_content_hash",
                },
            ),
        ):
            for name, target in names.items():
                value = source.get(name)
                if (
                    isinstance(value, str)
                    and value
                    and len(value.encode("utf-8")) <= 1024
                ) or type(value) is int:
                    context[target] = str(value)
        return context

    def _record_incident_now(
        self, recorder: Callable[..., dict[str, Any]], record: dict[str, Any]
    ) -> None:
        try:
            result = recorder(**record)
            if result.get("recorded") is False:
                raise OSError("bounded historian ingress refused the incident")
        except Exception:
            self._diagnostics["incident_recording_failures"] += 1
            if not self._incident_recording_failed:
                _LOGGER.exception(
                    "Control incident recorder unavailable; trace may be incomplete"
                )
            self._incident_recording_failed = True
        else:
            if self._incident_recording_failed:
                _LOGGER.info("Control incident recorder recovered")
            self._incident_recording_failed = False

    def _source_transition(self, source: str, *, failed: bool) -> None:
        """One bounded episode per upstream source; never a retry event log."""
        now = self.relative_monotonic_ns(time.monotonic_ns())
        active = self._source_incidents.get(source)
        if failed:
            self._diagnostics["source_failures"] += 1
            if active is not None:
                self._source_incidents[source] = (active[0], active[1] + 1)
                return
            first, count = now, 1
            self._source_incidents[source] = (first, count)
            _LOGGER.warning("Control source unavailable: %s", source)
        else:
            if active is None:
                return
            first, count = self._source_incidents.pop(source)
        self.record_incident(
            incident_id=f"{source}.{first}",
            incident_kind="control_source_unavailable",
            phase="activated" if failed else "resolved",
            control_session_id=self._ingress.receiver_incarnation,
            client_incarnation="headless-source",
            source_label=source,
            reason_code="source_unavailable" if failed else "source_recovered",
            occurrence_count=count,
            maximum_delay_ns=None,
            first_observed_ns=first,
            last_observed_ns=now,
        )

    def _invalidate_source(self, source: str) -> None:
        if source == "program":
            if self._program_source_current:
                self._report_program_failures_unknown()
            self._program_source_current = False
        elif source == "file_watch":
            self._watch_source_current = False
            if self._cold_expiry_handle is not None:
                self._cold_expiry_handle.cancel()
                self._cold_expiry_handle = None
        self._source_transition(source, failed=True)
        self._facts = None
        for subscription in self._subscriptions:
            subscription.frame = None

    def record_client_incident_transition(
        self,
        *,
        client_incarnation: str,
        incident_id: str,
        incident_kind: str,
        phase: str,
        control_session_id: str,
        source_label: str,
        occurrence_count: int,
        maximum_delay_ns: int | None,
        reason_code: str,
        now_ns: int,
    ) -> None:
        """Aggregate a browser-only incident across socket reconnects."""

        key = (client_incarnation, incident_id)
        now = self.relative_monotonic_ns(now_ns)
        if phase == "activated":
            active = self._active_client_incidents.get(key)
            if active is not None:
                if active[0] != incident_kind:
                    raise ValueError("client incident identity changed kind")
                return
            if len(self._active_client_incidents) >= _MAXIMUM_ACTIVE_CLIENT_INCIDENTS:
                # This cache only correlates a later resolution; the activation
                # is already durable. Expiring the oldest abandoned browser
                # episode is safe and avoids a lifetime exhaustion counter.
                oldest = next(iter(self._active_client_incidents))
                self._active_client_incidents.pop(oldest)
            self._active_client_incidents[key] = (incident_kind, now, source_label)
            first = now
        else:
            active = self._active_client_incidents.pop(key, None)
            if active is None:
                return
            if active[0] != incident_kind:
                raise ValueError("client incident identity changed kind")
            first = active[1]
        self.record_incident(
            incident_id=incident_id,
            incident_kind=incident_kind,
            phase=phase,
            control_session_id=control_session_id,
            client_incarnation=client_incarnation,
            source_label=source_label,
            reason_code=reason_code,
            occurrence_count=occurrence_count,
            maximum_delay_ns=maximum_delay_ns,
            first_observed_ns=first,
            last_observed_ns=now,
        )

    def relative_monotonic_ns(self, now_ns: int) -> int:
        return max(0, now_ns - self._diagnostic_origin_ns)

    def _program_demanded(self) -> bool:
        return bool(self._subscriptions) or self._program_demand

    def _start_program_reader(self, loop: asyncio.AbstractEventLoop) -> None:
        self._program_wake = asyncio.Event()
        self._program_wake.set()
        self._runner_task = loop.create_task(self._run_program_source())

    async def start_program_demand(self) -> None:
        """Keep the one Program reader running for the application lifetime.

        This is internal demand on the same reader, not a client: it assembles
        no cold projection and publishes no frame. ``close`` releases it.
        """

        loop = asyncio.get_running_loop()
        async with self._lifecycle_lock:
            if self._closed:
                raise RuntimeError("control state source is closed")
            if self._loop is not None and self._loop is not loop:
                raise RuntimeError("control state source crossed event loops")
            self._loop = loop
            self._program_demand = True
            if self._runner_task is None or self._runner_task.done():
                self._start_program_reader(loop)

    async def subscribe(self) -> ControlStateSubscription:
        loop = asyncio.get_running_loop()
        async with self._lifecycle_lock:
            if self._closed:
                raise RuntimeError("control state source is closed")
            if self._loop is not None and self._loop is not loop:
                raise RuntimeError("control state source crossed event loops")
            if len(self._subscriptions) >= CONTROL_MAXIMUM_CLIENTS:
                raise RuntimeError("control state client limit reached")
            self._loop = loop
            self._drain_pump_facts()
            subscription = ControlStateSubscription(asyncio.Event())
            self._subscriptions.add(subscription)
            if len(self._subscriptions) == 1:
                self._cold_wake = asyncio.Event()
                # A reader that ended unexpectedly restarts with a new client,
                # as it did before internal Program demand existed.
                if self._runner_task is None or self._runner_task.done():
                    self._start_program_reader(loop)
                self._cold_task = loop.create_task(self._run_cold_assembler())
                if self._watch_roots:
                    self._watch_task = loop.create_task(self._run_file_wakes())
                self.notify_cold()
            elif self._is_synchronized():
                subscription.frame = self._frame("critical")
                subscription.wake.set()
            return subscription

    async def unsubscribe(self, subscription: ControlStateSubscription) -> None:
        async with self._lifecycle_lock:
            subscription.closed = True
            subscription.frame = None
            subscription.wake.set()
            self._subscriptions.discard(subscription)
            await self._release_demand_locked()

    async def _release_demand_locked(self) -> None:
        if self._subscriptions:
            return
        # Internal Program demand keeps only the Program reader and its
        # current cycle; client-only cold state is always released.
        tasks = tuple(
            task
            for task in (
                None if self._program_demand else self._runner_task,
                self._cold_task,
                self._watch_task,
            )
            if task is not None
        )
        for task in tasks:
            if not task.done():
                task.cancel()
        if tasks:
            await asyncio.gather(*tasks, return_exceptions=True)
        self._cold_task = None
        self._watch_task = None
        if self._cold_expiry_handle is not None:
            self._cold_expiry_handle.cancel()
            self._cold_expiry_handle = None
        self._facts = None
        self._shell = None
        self._shell_key = None
        self._cold_wake = None
        self._watch_keys.clear()
        self._watch_modified.clear()
        self._watch_fresh.clear()
        self._watch_source_current = True
        self._semantic_reason = None
        if self._program_demand:
            return
        self._runner_task = None
        self._cycle = None
        self._program_wake = None
        self._program_source_current = False
        self._loop = None

    def notify_cold(self) -> None:
        loop = self._loop
        wake = self._cold_wake
        if loop is not None and wake is not None:
            loop.call_soon_threadsafe(wake.set)

    def notify_motion_settled(self) -> None:
        """Publish the rest pose once after any motion operation settles.

        Called from the execution event thread when an operation of any target
        reaches a resting state.  The next cold reduction then publishes if the
        observed joint values differ from the last published ones, even though
        the meaningful control key (which ignores samples) is unchanged.
        """

        loop = self._loop
        if loop is None:
            return
        loop.call_soon_threadsafe(self._request_observation_refresh)

    def _request_observation_refresh(self) -> None:
        self._observation_refresh_requested = True
        if self._cold_wake is not None:
            self._cold_wake.set()

    def notify_control(self) -> None:
        """Refresh non-Program facts after one accepted control effect.

        Program lifecycle has its own change stream.  Kernel authoring
        admission does not, so command completion must also wake the one
        capacity-one cold reduction instead of waiting for an unrelated
        Manager or execution-event pulse.
        """

        self.notify_cold()

        loop = self._loop
        wake = self._program_wake
        if loop is not None and wake is not None and self._runner_url is None:
            loop.call_soon_threadsafe(wake.set)

    async def close(self) -> None:
        self._closed = True
        subscriptions = tuple(self._subscriptions)
        for subscription in subscriptions:
            await self.unsubscribe(subscription)
        async with self._lifecycle_lock:
            self._program_demand = False
            await self._release_demand_locked()

    def take(self, subscription: ControlStateSubscription) -> ControlStateFrame | None:
        frame = subscription.frame
        subscription.frame = None
        subscription.wake.clear()
        return frame

    def current(self) -> ControlStateFrame:
        if not self._is_synchronized():
            raise ControlStateUnavailableError("control state is not synchronized")
        return self._frame("critical")

    def current_component_overview(self) -> dict[str, Any] | None:
        """Return the already-reduced Overview while the shared source is active.

        ``None`` means there is no control demand and therefore no retained fact
        set.  A caller may then perform one standalone capture.  While control
        demand exists, returning anything except the shared current reduction
        would reread owners and reintroduce the crossed-projection path UI6-G
        removes.
        """

        if not self._subscriptions:
            return None
        if not self._is_synchronized():
            raise ControlStateUnavailableError("control state is not synchronized")
        assert self._shell is not None
        overview = self._shell.get("component_overview")
        if not isinstance(overview, dict):
            raise ControlStateUnavailableError("component overview is unavailable")
        return dict(overview)

    def semantic_context(self) -> ControlSemanticContext:
        """Return cached semantic state without reading or reducing an owner."""

        cycle = self._cycle
        if "program" in self._source_incidents:
            return ControlSemanticContext(
                status="unavailable", reason="program_control_source_unavailable"
            )
        if not self._watch_source_current:
            return ControlSemanticContext(
                status="unavailable", reason="control_file_watch_unavailable"
            )
        if cycle is None:
            return ControlSemanticContext(
                status="synchronizing",
                reason="program_control_state_synchronizing",
            )
        if not self._program_source_current:
            return ControlSemanticContext(
                status="unavailable",
                reason="program_control_source_unavailable",
            )
        if self._semantic_reason is not None:
            return ControlSemanticContext(
                status="unavailable",
                reason=self._semantic_reason,
            )
        if not self._is_synchronized():
            return ControlSemanticContext(
                status="synchronizing",
                reason="control_projection_synchronizing",
            )
        owner_epoch = cycle.get("program_runner_owner_epoch")
        revision = cycle.get("execution_control_revision")
        if not isinstance(owner_epoch, str) or type(revision) is not int:
            return ControlSemanticContext(
                status="unavailable",
                reason="program_control_identity_invalid",
            )
        return ControlSemanticContext(
            status="current",
            reason=None,
            semantic_owner_epoch=owner_epoch,
            execution_control_revision=revision,
        )

    def _is_synchronized(self) -> bool:
        cycle = self._cycle
        facts = self._facts
        return bool(
            cycle is not None
            and facts is not None
            and self._shell is not None
            and self._program_source_current
            and self._watch_source_current
            and self._semantic_reason is None
            and facts.program_cycle == cycle
        )

    def _frame(self, event_class: str) -> ControlStateFrame:
        assert self._cycle is not None
        assert self._shell is not None
        return ControlStateFrame(
            generation=self._generation,
            event_class=event_class,
            control=build_control_projection(self._cycle, self._ingress),
            shell={**self._shell, **self._pump_facts},
        )

    def _publish(self, event_class: str) -> None:
        if not self._is_synchronized():
            return
        self._generation += 1
        self._publications[event_class] += 1
        frame = self._frame(event_class)
        self._published_observation_key = execution_observation_values_key(
            frame.shell.get("execution_control_state")
        )
        for subscription in self._subscriptions:
            if subscription.closed:
                continue
            subscription.frame = frame
            subscription.wake.set()

    async def _read_current_cycle(self, event_class: str) -> bool:
        try:
            cycle = await _drained_owner_read(self._service.program_cycle_status)
            self._accept_cycle(cycle, event_class)
            return True
        except (HeadlessApiError, OSError, ValueError):
            self._invalidate_source("program")
            return False
        except Exception:
            self._invalidate_source("program")
            _LOGGER.exception("Unexpected Program control read failure")
            raise

    def _accept_cycle(self, cycle: dict[str, Any], event_class: str) -> None:
        previous = self._cycle
        recovering = not self._program_source_current
        if previous == cycle and not recovering:
            return
        # Validate before replacing the last coherent owner fact. A malformed
        # identity must not poison recovery or publish new command freshness.
        ProgramCycleFrameDTO.model_validate(cycle)
        if cycle["run"] is not None:
            HeadlessService._validate_program_run_snapshot(cycle["run"])
        build_control_projection(cycle, self._ingress)
        self._program_source_current = True
        self._source_transition("program", failed=False)
        self._cycle = cycle
        self._observe_program_failures(cycle)
        if self._facts is None:
            self.notify_cold()
        else:
            # Program is not reread through Component Overview. Replacing the
            # one captured Program fact and rerunning the pure reducer is the
            # complete update, even if another owner advanced independently.
            facts = replace(self._facts, program_cycle=cycle)
            try:
                shell = self._shell_from_facts(facts)
            except ControlStateUnavailableError as error:
                self._semantic_reason = str(error)
                return
            self._facts = facts
            self._diagnostics["reductions"] += 1
            self._shell = shell
            self._shell_key = _shell_key(shell)
            self._semantic_reason = None
        self._publish(event_class)
        if (
            event_class == "critical"
            and self._facts is not None
            and (
                previous is None
                or program_cycle_control_key(previous)
                != program_cycle_control_key(cycle)
            )
        ):
            # Program lifecycle and Kernel authoring admission are distinct
            # authorities, but claim/release changes occur at these sparse
            # lifecycle boundaries. Refresh the dependent cold facts once;
            # group hold/Resume and node progress do not change the cycle claim.
            self.notify_cold()

    def _observe_program_failures(self, cycle: dict[str, Any]) -> None:
        """Report the accepted Run's ordinary operation-failure conditions.

        Active follows the Run's failure row. Clear comes only under a later
        accepted control revision: the row is gone from the same Run (that
        hold was released by Retry, Continue or a manual result, whatever
        other holds remain), a completed Reset, or the same owner showing
        another Run id (a new Run started). A missing run or an owner
        replacement never clears; the native source registration makes a
        replaced owner's conditions unknown.
        """

        if self._program_failure_observer is None:
            return
        epoch = cycle["program_runner_owner_epoch"]
        control = cycle["execution_control_revision"]
        if epoch != self._program_failure_epoch:
            self._program_failures.clear()
            self._program_failure_epoch = epoch
        self._program_failure_control_revision = control
        run_id = cycle.get("program_run_id")
        run_revision = cycle.get("run_revision")
        run = cycle.get("run")
        current = program_failure_origins(cycle)
        resend, self._program_failures_resend = self._program_failures_resend, False
        for key, occurrence in tuple(self._program_failures.items()):
            if occurrence.origin["program_run_id"] == run_id and run_revision is not None:
                occurrence.run_revision = run_revision
            if key in current:
                continue
            resolved = control > occurrence.activation_control_revision and (
                cycle["state"] == "idle_after_reset"
                or (run_id == occurrence.origin["program_run_id"] and isinstance(run, dict))
                # The same owner started another Run: the earlier Run (and
                # so its failure) no longer exists, even without an observed
                # idle_after_reset in between.
                or (
                    isinstance(run_id, str)
                    and run_id != occurrence.origin["program_run_id"]
                )
            )
            if resolved:
                if self._send_program_failure(occurrence, "clear", control):
                    del self._program_failures[key]
            elif resend:
                self._send_program_failure(occurrence, "active", control)
        for key, origin in current.items():
            occurrence = self._program_failures.get(key)
            if occurrence is None:
                occurrence = _ProgramFailureOccurrence(origin, control, run_revision)
                self._program_failures[key] = occurrence
            elif not resend:
                continue
            self._send_program_failure(occurrence, "active", control)

    def _report_program_failures_unknown(self) -> None:
        control = self._program_failure_control_revision
        if self._program_failure_observer is None or control is None:
            return
        # The next accepted cycle restates every retained occurrence.
        self._program_failures_resend = True
        for occurrence in tuple(self._program_failures.values()):
            self._send_program_failure(occurrence, "unknown", control)

    def _send_program_failure(
        self, occurrence: _ProgramFailureOccurrence, state: str, control: int
    ) -> bool:
        observer = self._program_failure_observer
        assert observer is not None
        origin = occurrence.origin
        condition = {
            "schema_version": _PROGRAM_FAILURE_CONDITION_SCHEMA,
            "program_runner_owner_epoch": origin["program_runner_owner_epoch"],
            "execution_control_revision": control,
            "program_run_id": origin["program_run_id"],
            "run_revision": occurrence.run_revision,
            "state": state,
            "occurrence": origin["occurrence"],
            "operation": origin["operation"],
            "resource_ids": origin["resource_ids"],
            "failure": origin["failure"],
        }
        try:
            observer(rfc8785.dumps(condition).decode("utf-8"))
        except Exception:
            # The alarm path must never stop the Program reader. Restate the
            # retained conditions on the next accepted cycle instead.
            self._program_failures_resend = True
            if not self._program_failure_observer_failed:
                _LOGGER.exception("Program failure condition observation failed")
            self._program_failure_observer_failed = True
            return False
        if self._program_failure_observer_failed:
            _LOGGER.info("Program failure condition observation recovered")
        self._program_failure_observer_failed = False
        return True

    async def _run_program_source(self) -> None:
        if self._runner_url is None:
            assert self._program_wake is not None
            wake = self._program_wake
            retry_seconds = _SOURCE_RETRY_INITIAL_SECONDS
            while self._program_demanded() and not self._closed:
                await wake.wait()
                wake.clear()
                if await self._read_current_cycle("critical"):
                    retry_seconds = _SOURCE_RETRY_INITIAL_SECONDS
                else:
                    await asyncio.sleep(retry_seconds)
                    retry_seconds = min(retry_seconds * 2, _SOURCE_RETRY_MAX_SECONDS)
                    wake.set()
            return
        retry_seconds = _SOURCE_RETRY_INITIAL_SECONDS
        while self._program_demanded() and not self._closed:
            try:
                async with connect_websocket(
                    self._runner_url,
                    max_size=CONTROL_MAXIMUM_SNAPSHOT_BYTES,
                    open_timeout=1.0,
                    close_timeout=0.25,
                ) as websocket:
                    previous_sequence = 0
                    async for message in websocket:
                        if not isinstance(message, str):
                            raise ValueError("Program control frame is not text")
                        value = strict_json_object(
                            message,
                            label="Program control state",
                            maximum_bytes=CONTROL_MAXIMUM_SNAPSHOT_BYTES,
                        )
                        if (
                            set(value)
                            != {
                                "program_cycle",
                                "schema_version",
                                "server_sequence",
                                "event_class",
                            }
                            or value.get("schema_version")
                            != PROGRAM_CONTROL_STATE_SCHEMA
                            or value.get("event_class")
                            not in ("critical", "node_progress")
                        ):
                            raise ValueError(
                                "Program control frame has an invalid shape"
                            )
                        sequence_text = value.get("server_sequence")
                        if (
                            not isinstance(sequence_text, str)
                            or not sequence_text.isascii()
                            or not sequence_text.isdigit()
                            or sequence_text.startswith("0")
                        ):
                            raise ValueError("Program control sequence is invalid")
                        sequence = int(sequence_text)
                        if sequence <= previous_sequence:
                            raise ValueError("Program control sequence regressed")
                        previous_sequence = sequence
                        cycle = value.get("program_cycle")
                        if not isinstance(cycle, dict):
                            raise ValueError("Program control state omitted its cycle")
                        event_class = value["event_class"]
                        self._accept_cycle(cycle, event_class)
                        retry_seconds = _SOURCE_RETRY_INITIAL_SECONDS
                # A clean peer close also invalidates the cached authority.
                self._invalidate_source("program")
            except asyncio.CancelledError:
                raise
            except (
                ConnectionClosed,
                OSError,
                TimeoutError,
                ValueError,
                HeadlessApiError,
            ):
                # Reconnection is source recovery, never command replay. Keep
                # the newest visual state but stop issuing new frames until a
                # current owner snapshot arrives.
                self._invalidate_source("program")
            except Exception:
                self._invalidate_source("program")
                _LOGGER.exception("Unexpected Program control source failure")
                raise
            await asyncio.sleep(retry_seconds)
            retry_seconds = min(retry_seconds * 2, _SOURCE_RETRY_MAX_SECONDS)

    async def _capture_facts(self) -> ComponentOverviewFacts:
        cycle = self._cycle
        if cycle is None:
            raise ControlStateUnavailableError("program_control_state_synchronizing")
        # Position-Reset availability is a host fact that the Runner samples
        # only when its own cycle changes. A cold reduction follows the host
        # changes, so refresh that one field here; the stream owns the rest.
        # The field is taken only while the Program source is current, from a
        # read of the same owner, cycle and control revision, and while the
        # cached cycle stayed unchanged; it never restores source currency.
        try:
            fresh = await _drained_owner_read(self._service.program_cycle_status)
        except (HeadlessApiError, OSError, ValueError):
            fresh = None
        if (
            fresh is not None
            and self._program_source_current
            and self._cycle is cycle
            and program_cycle_control_key(fresh) == program_cycle_control_key(cycle)
            and fresh["reset_robot_position"] != cycle["reset_robot_position"]
        ):
            self._accept_cycle(
                {**cycle, "reset_robot_position": fresh["reset_robot_position"]},
                "cold",
            )
        cycle = self._cycle
        if cycle is None:
            raise ControlStateUnavailableError("program_control_state_synchronizing")
        stop = threading.Event()
        facts = await _drained_owner_read(
            self._service.component_overview_facts,
            stop=stop,
            cancelled=stop.is_set,
            program_cycle=cycle,
        )
        self._diagnostics["owner_reads"] += 8
        # Program may advance while the other independent facts are captured.
        # Replacing that one cached value is sufficient; no owner is reread.
        current_cycle = self._cycle
        return (
            facts
            if current_cycle == cycle
            else replace(
                facts,
                program_cycle=current_cycle,
            )
        )

    @staticmethod
    def _shell_from_facts(facts: ComponentOverviewFacts) -> dict[str, Any]:
        required = {
            "runtime": facts.runtime,
            "alarms": facts.alarms,
            "authoring_admission": facts.authoring_admission,
            "program_cycle": facts.program_cycle,
        }
        missing = next(
            (name for name, value in required.items() if not isinstance(value, dict)),
            None,
        )
        if missing is not None:
            error = facts.errors.get(missing, {})
            code = error.get("code", f"{missing}_projection_unavailable")
            raise ControlStateUnavailableError(str(code))
        targets = facts.execution_targets_response
        control = facts.execution_control_response
        # These sequential reads may straddle native-owner replacement. The
        # browser consumes one identity-bound pair, never a half/crossed pair.
        # Preserve the original facts for readiness/diagnostics and publish
        # unavailable until the existing owner-change wake captures both.
        if (
            targets is None
            or control is None
            or targets.get("ok") is not True
            or control.get("ok") is not True
            or targets.get("identity") != control.get("identity")
        ):
            targets = control = None
        return {
            "runtime": dict(facts.runtime),
            "alarms": dict(facts.alarms),
            "component_overview": facts.reduce(),
            "authoring_admission": dict(facts.authoring_admission),
            # Beside the Kernel's Idle admission: whether the existing
            # Program run leaves the Draft editable (one rule, also applied
            # by Composer to every Draft edit).
            "held_run_draft_edit_allowed": held_run_draft_edit_allowed(
                facts.program_cycle, control
            ),
            "execution_targets": targets,
            "execution_control_state": control,
            "program_cycle": dict(facts.program_cycle),
        }

    async def _assemble_shell(
        self,
    ) -> tuple[ComponentOverviewFacts, dict[str, Any], tuple[Any, ...]]:
        facts = await self._capture_facts()
        shell = self._shell_from_facts(facts)
        return facts, shell, _shell_key(shell)

    async def _run_cold_assembler(self) -> None:
        assert self._cold_wake is not None
        wake = self._cold_wake
        retry_seconds = _SOURCE_RETRY_INITIAL_SECONDS
        while self._subscriptions and not self._closed:
            await wake.wait()
            wake.clear()
            await asyncio.sleep(CONTROL_COLD_COMBINE_NS / 1_000_000_000)
            observation_refresh = self._observation_refresh_requested
            self._observation_refresh_requested = False
            try:
                facts, shell, key = await self._assemble_shell()
            except ControlStateUnavailableError as error:
                self._semantic_reason = str(error)
                if self._cycle is not None:
                    # Normal owner read failures are captured as missing facts
                    # by HeadlessService. They are an incident too, not just
                    # failures that happen to escape as an I/O exception.
                    self._invalidate_source("cold_projection")
                self._observation_refresh_requested |= observation_refresh
                await asyncio.sleep(retry_seconds)
                retry_seconds = min(retry_seconds * 2, _SOURCE_RETRY_MAX_SECONDS)
                wake.set()
                continue
            except (HeadlessApiError, OSError, ValueError):
                self._semantic_reason = "control_projection_source_unavailable"
                self._invalidate_source("cold_projection")
                self._observation_refresh_requested |= observation_refresh
                await asyncio.sleep(retry_seconds)
                retry_seconds = min(retry_seconds * 2, _SOURCE_RETRY_MAX_SECONDS)
                wake.set()
                continue
            except Exception:
                self._semantic_reason = "control_projection_assembly_failed"
                self._invalidate_source("cold_projection")
                _LOGGER.exception("Unexpected control projection failure")
                raise
            if not self._program_source_current or not self._watch_source_current:
                # Keep a pending settle refresh for the next current reduction.
                self._observation_refresh_requested |= observation_refresh
                continue
            observation_key = execution_observation_values_key(
                shell.get("execution_control_state")
            )
            changed = (
                key != self._shell_key
                or self._facts is None
                or (
                    observation_refresh
                    and observation_key != self._published_observation_key
                )
            )
            self._facts = facts
            self._shell = shell
            self._shell_key = key
            self._diagnostics["reductions"] += 1
            self._semantic_reason = None
            retry_seconds = _SOURCE_RETRY_INITIAL_SECONDS
            self._source_transition("cold_projection", failed=False)
            if changed or self._generation == 0:
                self._publish("cold")
            else:
                self._diagnostics["unchanged_reductions"] += 1
            if self._watch_source_current:
                self._source_transition("file_watch", failed=False)

    async def _refresh_file_watches(
        self, *, notify: bool, paths: Iterable[Path] | None = None
    ) -> None:
        """Read each changed source once, using existing bounded work capacity."""
        changed = False
        selected = self._watch_paths if paths is None else paths
        for path in selected:
            key, modified = await _drained_owner_read(
                _source_file_observation, path=path
            )
            if path in self._watch_keys and self._watch_keys[path] != key:
                changed = True
            self._watch_keys[path] = key
            self._watch_modified[path] = modified
        self._refresh_file_freshness(notify=notify, changed=changed)

    def _refresh_file_freshness(self, *, notify: bool, changed: bool = False) -> None:
        """One cached deadline; expiry does no file I/O, parsing or hashing."""
        now_ns = time.time_ns()
        budget_ns = self._cold_freshness_budget_ns or 0
        deadlines = []
        for path in self._freshness_paths:
            modified = self._watch_modified.get(path)
            fresh = modified is not None and modified <= now_ns <= modified + budget_ns
            if path in self._watch_fresh and self._watch_fresh[path] != fresh:
                changed = True
            self._watch_fresh[path] = fresh
            if fresh:
                deadlines.append(modified + budget_ns + 1)

        if self._cold_expiry_handle is not None:
            self._cold_expiry_handle.cancel()
            self._cold_expiry_handle = None
        loop = self._loop
        if deadlines and loop is not None and self._subscriptions:
            delay_ns = max(1, min(deadlines) - now_ns)
            self._cold_expiry_handle = loop.call_later(
                delay_ns / 1_000_000_000,
                partial(self._refresh_file_freshness, notify=True),
            )
        if changed and notify:
            self.notify_cold()

    async def _run_file_wakes(self) -> None:
        retry_seconds = _SOURCE_RETRY_INITIAL_SECONDS
        while self._subscriptions and not self._closed:
            try:
                await self._refresh_file_watches(notify=False)
                self._watch_source_current = True
                self.notify_cold()  # Recovery captures current owners once.
                async with aclosing(awatch(*self._watch_roots)) as watcher:
                    async for changes in watcher:
                        if not self._subscriptions or self._closed:
                            return
                        paths = {Path(name) for _kind, name in changes}
                        if any(
                            root == path or path in root.parents
                            for root in self._watch_roots
                            for path in paths
                        ):
                            # A root-only remove/rename event invalidates every
                            # read from that registration. Re-register the same
                            # watcher and capture current files on recovery.
                            break
                        await self._refresh_file_watches(
                            notify=True,
                            paths=(path for path in self._watch_paths if path in paths),
                        )
                        self._source_transition("file_watch", failed=False)
                        retry_seconds = _SOURCE_RETRY_INITIAL_SECONDS
                self._invalidate_source("file_watch")
            except asyncio.CancelledError:
                raise
            except OSError:
                self._invalidate_source("file_watch")
            except Exception:
                self._invalidate_source("file_watch")
                _LOGGER.exception("Unexpected control file-watch failure")
                raise
            await asyncio.sleep(retry_seconds)
            retry_seconds = min(retry_seconds * 2, _SOURCE_RETRY_MAX_SECONDS)


__all__ = [
    "ControlSemanticContext",
    "ControlStateFrame",
    "ControlStateSubscription",
    "ControlStateUnavailableError",
    "HeadlessControlStateSource",
    "build_control_projection",
    "control_shell_fact_key",
]
