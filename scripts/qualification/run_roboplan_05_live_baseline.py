#!/usr/bin/env python3
"""Collect one RP5 live Isaac drop-approach qualification stratum.

Run this script inside the active Composer ROS container. It consumes existing
kernel lifecycle stamps and the opt-in planner qualification recorder; it does
not add timers, callbacks, or sampling work to the execution hot path.
"""

from __future__ import annotations

import argparse
import bisect
import hashlib
import json
import math
import os
import shlex
import statistics
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any, NamedTuple


def _repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[2]


REPO_ROOT = _repo_root_from_script()
sys.path.insert(0, str(REPO_ROOT))

from scripts.qualification.roboplan_05_qualification import (  # noqa: E402
    EVIDENCE_SCHEMA,
    collect_inventory,
    cycle_failure_reason as _cycle_failure_reason,
    require_source_inventory_stable,
    validate_evidence,
    write_evidence_exclusive as _write_evidence_exclusive,
)
from scripts.qualification.capture_isaac_drop_approach import (  # noqa: E402
    require_loaded_artifacts_match,
    reset_and_start_simulation,
)


_ISAAC_PROGRAM = REPO_ROOT / "examples/isaac_manipulator_scene_light.program.yaml"
_ISAAC_DEPLOYMENT = (
    REPO_ROOT
    / "examples/isaac_manipulator_scene_light_ros2_control.deployment.yaml"
)
_ISAAC_PROGRAM_NAME = "isaac_manipulator_scene_light"


def _request_json(
    base_url: str,
    path: str,
    *,
    method: str = "GET",
    payload: dict[str, Any] | None = None,
    timeout_s: float = 15.0,
) -> dict[str, Any]:
    body = None if payload is None else json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(
        f"{base_url.rstrip('/')}{path}",
        data=body,
        method=method,
        headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout_s) as response:
            value = json.load(response)
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"{method} {path} failed ({exc.code}): {detail}") from exc
    if not isinstance(value, dict):
        raise RuntimeError(f"{method} {path} returned a non-object payload")
    return value


def _percentile(sorted_values: list[int], percentile: float) -> float:
    if not sorted_values:
        raise ValueError("cannot summarize an empty sample")
    position = (len(sorted_values) - 1) * percentile
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return float(sorted_values[lower])
    weight = position - lower
    return float(
        sorted_values[lower] * (1.0 - weight) + sorted_values[upper] * weight
    )


def _summary(values: list[int], *, include_p99: bool = False) -> dict[str, Any]:
    ordered = sorted(int(value) for value in values)
    if not ordered or ordered[0] < 0:
        raise ValueError("latency samples must be non-negative and non-empty")
    median = statistics.median(ordered)
    deviations = sorted(abs(value - median) for value in ordered)
    return {
        "p50": _percentile(ordered, 0.50),
        "p95": _percentile(ordered, 0.95),
        "p99": _percentile(ordered, 0.99) if include_p99 else None,
        "max": float(ordered[-1]),
        "mad": float(statistics.median(deviations)),
    }


class _CpuSample(NamedTuple):
    monotonic_ns: int
    process_cpu_ns: int


class _ProcessCpuSampler:
    """Sample another process without charging work to that process.

    Linux exposes aggregate process user/system CPU in clock ticks. At the
    configured cadence, the bracket width is also the measurement uncertainty;
    each live sample reports conservative lower/upper CPU bounds.
    """

    def __init__(self, pid: int, period_s: float):
        self._stat_path = Path(f"/proc/{pid}/stat")
        self._period_s = period_s
        self._clock_tick_hz = int(os.sysconf("SC_CLK_TCK"))
        self._samples: list[_CpuSample] = []
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._error: BaseException | None = None
        self._started = False
        self._thread = threading.Thread(
            target=self._run, name="rp5_process_cpu_sampler", daemon=True
        )

    @property
    def clock_tick_hz(self) -> int:
        return self._clock_tick_hz

    def _read_cpu_ns(self) -> int:
        text = self._stat_path.read_text(encoding="utf-8")
        fields = text[text.rfind(")") + 2 :].split()
        if len(fields) < 13:
            raise RuntimeError(f"malformed process stat: {self._stat_path}")
        ticks = int(fields[11]) + int(fields[12])
        return ticks * 1_000_000_000 // self._clock_tick_hz

    def _sample(self) -> None:
        before_ns = time.monotonic_ns()
        cpu_ns = self._read_cpu_ns()
        after_ns = time.monotonic_ns()
        sample = _CpuSample((before_ns + after_ns) // 2, cpu_ns)
        with self._lock:
            self._samples.append(sample)

    def _run(self) -> None:
        try:
            while not self._stop.is_set():
                self._sample()
                self._stop.wait(self._period_s)
            self._sample()
        except BaseException as exc:  # propagate sampler failures to the owner
            self._error = exc
            self._stop.set()

    def start(self) -> None:
        if self._started:
            raise RuntimeError("process CPU sampler already started")
        self._sample()
        self._started = True
        self._thread.start()

    def stop(self) -> None:
        if not self._started:
            return
        self._stop.set()
        self._thread.join(timeout=max(1.0, self._period_s * 4.0))
        if self._thread.is_alive():
            raise RuntimeError("process CPU sampler did not stop")
        if self._error is not None:
            raise RuntimeError("process CPU sampler failed") from self._error

    def interval_bounds(self, start_ns: int, end_ns: int) -> dict[str, int]:
        if end_ns < start_ns:
            raise ValueError("CPU interval end precedes start")
        with self._lock:
            samples = list(self._samples)
        times = [sample.monotonic_ns for sample in samples]
        start_right = bisect.bisect_left(times, start_ns)
        end_right = bisect.bisect_left(times, end_ns)
        if start_right == 0 or end_right >= len(samples):
            raise RuntimeError("CPU samples do not bracket the lifecycle interval")
        before_start = samples[start_right - 1]
        after_start = samples[start_right]
        before_end = samples[max(0, end_right - 1)]
        after_end = samples[end_right]
        lower = max(0, before_end.process_cpu_ns - after_start.process_cpu_ns)
        upper = max(0, after_end.process_cpu_ns - before_start.process_cpu_ns)
        if lower > upper:
            raise RuntimeError("process CPU accounting regressed")
        return {
            "estimate_ns": (lower + upper) // 2,
            "lower_bound_ns": lower,
            "upper_bound_ns": upper,
            "uncertainty_ns": upper - lower,
            "start_bracket_ns": after_start.monotonic_ns
            - before_start.monotonic_ns,
            "end_bracket_ns": after_end.monotonic_ns - before_end.monotonic_ns,
        }


class _KernelSequenceTracker:
    """Validate a contiguous kernel sequence under concurrent delivery.

    Kernel sequence is allocated before subscriber dispatch, so producer
    threads may reach the collector in a different order. Arrival order remains
    authoritative for the manager's event_index cursor, while this bounded
    reorder set proves that every kernel sequence eventually arrived.
    """

    def __init__(self, baseline_sequence: int, *, capacity: int = 4096) -> None:
        if baseline_sequence < 0:
            raise ValueError("kernel sequence baseline must be non-negative")
        if capacity <= 0:
            raise ValueError("kernel sequence reorder capacity must be positive")
        self._next_expected = baseline_sequence + 1
        self._capacity = capacity
        self._pending: set[int] = set()

    @property
    def committed_sequence(self) -> int:
        return self._next_expected - 1

    @property
    def settled(self) -> bool:
        return not self._pending

    def observe(self, events: list[dict[str, Any]]) -> None:
        for event in events:
            sequence = int(event.get("sequence") or 0)
            if sequence <= 0:
                raise RuntimeError("kernel event has no positive sequence")
            if sequence < self._next_expected or sequence in self._pending:
                raise RuntimeError(f"duplicate or regressed kernel sequence {sequence}")
            self._pending.add(sequence)

        while self._next_expected in self._pending:
            self._pending.remove(self._next_expected)
            self._next_expected += 1

        if len(self._pending) > self._capacity:
            self.require_settled(reason="reorder capacity exceeded")

    def require_settled(self, *, reason: str) -> None:
        if not self._pending:
            return
        next_available = min(self._pending)
        missing_end = next_available - 1
        raise RuntimeError(
            f"kernel event sequence gap at {reason}: expected "
            f"{self._next_expected}, next available {next_available}, "
            f"missing through {missing_end}"
        )


def _pipeline_status(base_url: str) -> dict[str, Any]:
    heartbeat = _request_json(base_url, "/api/infra/heartbeat")
    return dict(
        (heartbeat.get("components") or {}).get("kernel_event_pipeline") or {}
    )


def _viewer_clients(base_url: str) -> int:
    payload = _request_json(base_url, "/api/debug/qualification/environment")
    value = payload.get("state_websocket_clients")
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise RuntimeError("Composer did not report a valid viewer connection count")
    return value


def _enforce_viewer_stratum(
    base_url: str, *, stratum: str, focused_attested: bool
) -> int:
    clients = _viewer_clients(base_url)
    if stratum == "disconnected" and clients != 0:
        raise RuntimeError(
            f"disconnected stratum requires zero state clients; observed {clients}"
        )
    if stratum == "focused":
        if clients < 1:
            raise RuntimeError("focused stratum requires at least one state client")
        if not focused_attested:
            raise RuntimeError(
                "focused stratum requires --operator-attests-viewer-focused"
            )
    return clients


def _event_move_id(event: dict[str, Any]) -> str:
    return str((event.get("details") or {}).get("move_id") or "")


def _pair_planner_records(
    events: list[dict[str, Any]], records: list[dict[str, Any]]
) -> dict[int, dict[str, Any]]:
    """Pair serial kernel plan admissions with recorder sequence order."""
    move_by_execution: dict[int, str] = {}
    for event in events:
        move_id = _event_move_id(event)
        execution_id = int(event.get("execution_id") or 0)
        if execution_id > 0 and move_id:
            previous = move_by_execution.setdefault(execution_id, move_id)
            if previous != move_id:
                raise RuntimeError("one execution carried conflicting move ids")
    admissions = sorted(
        [
            event
            for event in events
            if event.get("type") == "plan_started"
            and int(event.get("execution_id") or 0) > 0
        ],
        key=lambda value: int(value.get("sequence") or 0),
    )
    ordered_records = sorted(
        records,
        key=lambda value: int(value.get("sequence") or 0),
    )
    if len(admissions) != len(ordered_records):
        raise RuntimeError(
            "planner recorder and kernel plan admissions have different cardinality: "
            f"{len(ordered_records)} records vs {len(admissions)} admissions"
        )
    result: dict[int, dict[str, Any]] = {}
    for admission, record in zip(admissions, ordered_records, strict=True):
        execution_id = int(admission["execution_id"])
        expected_move = move_by_execution.get(execution_id)
        actual_move = str((record.get("request") or {}).get("move_id") or "")
        if not expected_move or actual_move != expected_move:
            raise RuntimeError(
                "planner recorder order disagrees with kernel admission order: "
                f"execution {execution_id}, expected {expected_move!r}, "
                f"recorded {actual_move!r}"
            )
        result[execution_id] = record
    return result


def _events_for_execution(
    events: list[dict[str, Any]], execution_id: int
) -> dict[str, dict[str, Any]]:
    selected: dict[str, dict[str, Any]] = {}
    for event in events:
        if int(event.get("execution_id") or 0) != execution_id:
            continue
        event_type = str(event.get("type") or "")
        if event_type in selected and event_type in {
            "plan_started",
            "plan_completed",
            "validation_started",
            "validation_passed",
            "execute_started",
            "execute_completed",
        }:
            raise RuntimeError(
                f"execution {execution_id} has repeated {event_type}; "
                "qualification requires one-attempt successful planning"
            )
        selected[event_type] = event
    return selected


def _successful_drop_execution_ids(events: list[dict[str, Any]]) -> list[int]:
    candidates = {
        int(event.get("execution_id") or 0)
        for event in events
        if _event_move_id(event) == "drop_approach"
    }
    successful: list[int] = []
    for execution_id in sorted(value for value in candidates if value > 0):
        by_type = _events_for_execution(events, execution_id)
        required = {
            "plan_started",
            "plan_completed",
            "validation_started",
            "validation_passed",
            "execute_started",
            "execute_completed",
            # The behavior coordinator emits this only after it has consumed the
            # terminal kernel result. Requiring it prevents the collector from
            # halting the tree between native completion and the Python node's
            # own terminal qualification boundary.
            "acknowledge_completed",
        }
        if required.issubset(by_type):
            successful.append(execution_id)
    return successful


def _wait_for_successes(
    base_url: str,
    *,
    cursor: int,
    sequence_tracker: _KernelSequenceTracker,
    required_successes: int,
    timeout_s: float,
    viewer_stratum: str,
    focused_attested: bool,
    poll_period_s: float,
) -> tuple[list[dict[str, Any]], int]:
    deadline = time.monotonic() + timeout_s
    events: list[dict[str, Any]] = []
    last_viewer_check = 0.0
    last_status_check = 0.0
    while time.monotonic() < deadline:
        payload = _request_json(base_url, f"/api/kernel/events?since={cursor}")
        batch = list(payload.get("events") or [])
        if batch:
            sequence_tracker.observe(batch)
            events.extend(batch)
            cursor = int(batch[-1].get("event_index") or cursor)
            if (
                len(_successful_drop_execution_ids(events)) >= required_successes
                and sequence_tracker.settled
            ):
                return events, cursor
        now = time.monotonic()
        if now - last_viewer_check >= 1.0:
            _enforce_viewer_stratum(
                base_url,
                stratum=viewer_stratum,
                focused_attested=focused_attested,
            )
            last_viewer_check = now
        if now - last_status_check >= 1.0:
            status = _request_json(base_url, "/api/cycle/status")
            failure_reason = _cycle_failure_reason(status)
            if failure_reason is not None:
                raise RuntimeError(
                    "live cycle failed before enough successful observations: "
                    f"{failure_reason}"
                )
            last_status_check = now
        time.sleep(poll_period_s)
    sequence_tracker.require_settled(reason="live observation timeout")
    raise TimeoutError(
        f"timed out after {timeout_s}s with "
        f"{len(_successful_drop_execution_ids(events))}/{required_successes} "
        "successful drop-approach observations"
    )


def _drain_kernel_events_until_quiescent(
    base_url: str,
    *,
    cursor: int,
    sequence_tracker: _KernelSequenceTracker,
    timeout_s: float = 10.0,
    quiet_period_s: float = 1.0,
    minimum_wait_s: float = 2.0,
    poll_period_s: float = 0.02,
) -> tuple[list[dict[str, Any]], int]:
    """Drain post-stop events and prove that the sequence reorder set closes."""

    started = time.monotonic()
    deadline = started + timeout_s
    quiet_since = started
    events: list[dict[str, Any]] = []
    while time.monotonic() < deadline:
        payload = _request_json(base_url, f"/api/kernel/events?since={cursor}")
        batch = list(payload.get("events") or [])
        if batch:
            sequence_tracker.observe(batch)
            events.extend(batch)
            cursor = int(batch[-1].get("event_index") or cursor)
            quiet_since = time.monotonic()
        elif sequence_tracker.settled and (
            time.monotonic() - quiet_since >= quiet_period_s
            and time.monotonic() - started >= minimum_wait_s
        ):
            return events, cursor
        time.sleep(poll_period_s)
    sequence_tracker.require_settled(reason="post-stop quiescence timeout")
    raise TimeoutError("kernel event stream did not become quiescent after full stop")


def _sample_payload(
    execution_id: int,
    events: list[dict[str, Any]],
    planner_record: dict[str, Any],
    cpu_sampler: _ProcessCpuSampler,
    measurement_mode: str,
) -> dict[str, Any]:
    by_type = _events_for_execution(events, execution_id)
    transition = next(
        event
        for event in events
        if int(event.get("execution_id") or 0) == execution_id
        and event.get("type") == "state_transition"
        and event.get("to_state") == "Planning"
    )
    timestamp = {
        name: int(by_type[name]["timestamp_ns"])
        for name in (
            "plan_started",
            "plan_completed",
            "validation_started",
            "validation_passed",
            "execute_started",
            "execute_completed",
        )
    }
    admitted_ns = int(transition["timestamp_ns"])
    predispatch_end_ns = timestamp["execute_started"]
    action_end_ns = timestamp["execute_completed"]
    metrics = dict(planner_record.get("metrics") or {})
    if metrics.get("measurement_complete") is not True:
        raise RuntimeError(
            f"execution {execution_id} has incomplete planner instrumentation"
        )
    probe_available = metrics.get("collision_probe_available") is True
    if measurement_mode == "primary" and probe_available:
        raise RuntimeError(
            f"execution {execution_id} primary latency was contaminated by the "
            "collision attribution preload"
        )
    if measurement_mode == "collision_probe_attribution" and not probe_available:
        raise RuntimeError(
            f"execution {execution_id} has no planner collision probe; restart "
            "Composer with the RP5 qualification preload enabled"
        )
    if (
        measurement_mode == "collision_probe_attribution"
        and int(metrics.get("total_collision_query_count") or 0) <= 0
    ):
        raise RuntimeError(
            f"execution {execution_id} recorded no planner collision queries"
        )
    planner_backend_ns = int(metrics.get("total_duration_ns") or 0)
    kernel_planning_ns = timestamp["plan_completed"] - timestamp["plan_started"]
    if planner_backend_ns <= 0 or planner_backend_ns > kernel_planning_ns:
        raise RuntimeError(
            "planner backend duration is inconsistent with kernel lifecycle stamps"
        )
    if metrics.get("succeeded") is not True:
        raise RuntimeError(
            f"execution {execution_id} completed without a successful planner record"
        )
    for seed_field in ("rng_base_seed", "rng_seed", "shortcut_rng_seed_argument"):
        seed_value = metrics.get(seed_field)
        if isinstance(seed_value, bool) or not isinstance(seed_value, int):
            raise RuntimeError(
                f"execution {execution_id} has no deterministic {seed_field}"
            )
    validation_sequence = int(by_type["validation_passed"].get("sequence") or 0)
    if validation_sequence <= 0:
        raise RuntimeError(
            f"execution {execution_id} validation verdict has no kernel sequence"
        )
    request = dict(planner_record.get("request") or {})
    scene_binding = dict(request.get("scene_binding") or {})
    result = dict(planner_record.get("result") or {})
    scene_revision = dict(result.get("scene_revision") or {})
    for field, value in (
        ("request.scene_binding.revision_id", scene_binding.get("revision_id")),
        ("result.scene_revision.revision_id", scene_revision.get("revision_id")),
    ):
        if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
            raise RuntimeError(f"execution {execution_id} has invalid {field}")
    for field, value in (
        ("request.scene_binding.content_hash", scene_binding.get("content_hash")),
        ("result.scene_revision.content_hash", scene_revision.get("content_hash")),
    ):
        if not isinstance(value, str) or not value:
            raise RuntimeError(f"execution {execution_id} has invalid {field}")
    bound_revision = {
        "revision_id": scene_binding["revision_id"],
        "content_hash": scene_binding["content_hash"],
    }
    planned_revision = {
        "revision_id": scene_revision["revision_id"],
        "content_hash": scene_revision["content_hash"],
    }
    if bound_revision != planned_revision:
        raise RuntimeError(
            f"execution {execution_id} planner result changed its bound scene revision"
        )
    trajectory = result.get("trajectory") or {}
    trajectory_joint_names = list(trajectory.get("joint_names") or [])
    waypoints = list(trajectory.get("waypoints") or [])
    if not trajectory_joint_names or len(waypoints) < 2:
        raise RuntimeError(
            f"execution {execution_id} has no replayable planned trajectory"
        )
    joint_space_path_length_rad = 0.0
    previous_positions: list[float] | None = None
    for waypoint in waypoints:
        positions = [float(value) for value in waypoint.get("positions_rad") or []]
        if len(positions) != len(trajectory_joint_names) or not all(
            math.isfinite(value) for value in positions
        ):
            raise RuntimeError(
                f"execution {execution_id} has an invalid trajectory waypoint"
            )
        if previous_positions is not None:
            joint_space_path_length_rad += math.dist(previous_positions, positions)
        previous_positions = positions
    if int(metrics.get("waypoint_count") or 0) != len(waypoints):
        raise RuntimeError(
            f"execution {execution_id} planner waypoint accounting disagrees "
            "with the retained trajectory"
        )
    trajectory_sha256 = hashlib.sha256(
        json.dumps(trajectory, sort_keys=True, separators=(",", ":")).encode(
            "utf-8"
        )
    ).hexdigest()
    behavior = dict(planner_record.get("behavior_node") or {})
    if behavior.get("terminal_phase") != "succeeded":
        raise RuntimeError(
            f"execution {execution_id} is missing a successful behavior-node "
            "qualification interval"
        )
    behavior_started_ns = int(behavior.get("started_monotonic_ns") or 0)
    behavior_completed_ns = int(behavior.get("completed_monotonic_ns") or 0)
    if not (
        0 < behavior_started_ns <= admitted_ns <= action_end_ns <= behavior_completed_ns
    ):
        raise RuntimeError(
            f"execution {execution_id} behavior and kernel timestamps are not nested"
        )
    return {
        "execution_id": execution_id,
        "program_run_id": transition.get("program_run_id"),
        "node_attempt_id": transition.get("node_attempt_id"),
        "planner_record_sequence": int(planner_record.get("sequence") or 0),
        "wall_duration_ns": {
            "admission_to_plan_start": timestamp["plan_started"] - admitted_ns,
            "planning": kernel_planning_ns,
            "planner_backend": planner_backend_ns,
            "kernel_state_target_resolution_and_result_overhead": (
                kernel_planning_ns - planner_backend_ns
            ),
            "authoritative_validation": timestamp["validation_passed"]
            - timestamp["validation_started"],
            "dispatch_gate": timestamp["execute_started"]
            - timestamp["validation_passed"],
            "total_predispatch": predispatch_end_ns - admitted_ns,
            "execution": action_end_ns - timestamp["execute_started"],
            "total_kernel_action": action_end_ns - admitted_ns,
            "behavior_pre_admission": admitted_ns - behavior_started_ns,
            "behavior_post_kernel": behavior_completed_ns - action_end_ns,
            "total_behavior_node": behavior_completed_ns - behavior_started_ns,
        },
        "process_cpu": {
            "predispatch": cpu_sampler.interval_bounds(
                admitted_ns, predispatch_end_ns
            ),
            "kernel_action": cpu_sampler.interval_bounds(admitted_ns, action_end_ns),
            "behavior_node": cpu_sampler.interval_bounds(
                behavior_started_ns, behavior_completed_ns
            ),
        },
        "behavior_node": behavior,
        "request": {
            "contact_policy_id": str(request.get("contact_policy_id") or ""),
            "contact_policy_resolution": (
                "explicit"
                if request.get("contact_policy_id")
                else "configured_default"
            ),
            "scene_binding": scene_binding,
        },
        "planner_metrics": metrics,
        "result": {
            "succeeded": bool(metrics.get("succeeded")),
            "waypoint_count": int(metrics.get("waypoint_count") or 0),
            "trajectory_duration_s": float(
                metrics.get("trajectory_duration_s") or 0.0
            ),
            "trajectory_joint_names": trajectory_joint_names,
            "trajectory_sha256": trajectory_sha256,
            "joint_space_path_length_rad": joint_space_path_length_rad,
            "rng_base_seed": metrics.get("rng_base_seed"),
            "rng_seed": metrics.get("rng_seed"),
            "shortcut_rng_seed_argument": metrics.get(
                "shortcut_rng_seed_argument"
            ),
            "authoritative_validation": {
                "decision": "passed",
                "event_sequence": validation_sequence,
                "joint_limit_and_dynamics_compliance": True,
            },
            "scene_revision": scene_revision,
        },
    }


def _aggregate_live_record(
    *,
    samples: list[dict[str, Any]],
    stratum: str,
    measurement_mode: str,
    viewer_clients: int,
    pipeline_before: dict[str, Any],
    pipeline_after: dict[str, Any],
    cpu_clock_tick_hz: int,
    failed_observations: list[dict[str, Any]],
    authority_before: dict[str, Any],
    authority_after: dict[str, Any],
) -> dict[str, Any]:
    wall_keys = (
        "admission_to_plan_start",
        "planning",
        "planner_backend",
        "kernel_state_target_resolution_and_result_overhead",
        "authoritative_validation",
        "dispatch_gate",
        "total_predispatch",
        "execution",
        "total_kernel_action",
        "behavior_pre_admission",
        "behavior_post_kernel",
        "total_behavior_node",
    )
    wall = {
        key: _summary([sample["wall_duration_ns"][key] for sample in samples])
        for key in wall_keys
    }
    stage_names = sorted(
        {
            stage_name
            for sample in samples
            for stage_name in (sample["planner_metrics"].get("stages") or {})
        }
    )
    planner_stages: dict[str, Any] = {}
    for stage_name in stage_names:
        durations = [
            int(sample["planner_metrics"]["stages"][stage_name]["duration_ns"])
            for sample in samples
            if stage_name in (sample["planner_metrics"].get("stages") or {})
        ]
        if len(durations) != len(samples):
            raise RuntimeError(f"planner stage {stage_name!r} was not measured every run")
        planner_stages[stage_name] = _summary(durations)
    cpu_estimates = [
        sample["process_cpu"]["predispatch"]["estimate_ns"]
        for sample in samples
    ]
    action_cpu_estimates = [
        sample["process_cpu"]["kernel_action"]["estimate_ns"]
        for sample in samples
    ]
    behavior_cpu_estimates = [
        sample["process_cpu"]["behavior_node"]["estimate_ns"]
        for sample in samples
    ]
    cpu_uncertainties = [
        sample["process_cpu"]["predispatch"]["uncertainty_ns"]
        for sample in samples
    ]
    action_cpu_uncertainties = [
        sample["process_cpu"]["kernel_action"]["uncertainty_ns"]
        for sample in samples
    ]
    behavior_cpu_uncertainties = [
        sample["process_cpu"]["behavior_node"]["uncertainty_ns"]
        for sample in samples
    ]
    return {
        "record_type": "live_planning_stage",
        "fixture_id": "isaac_light_drop_approach",
        "passed": not failed_observations,
        "sample_count": len(samples),
        "latency_ns": wall["total_predispatch"],
        "cpu_time_ns": _summary(behavior_cpu_estimates),
        "details": {
            "viewer_stratum": stratum,
            "measurement_mode": measurement_mode,
            "viewer_state_websocket_clients": viewer_clients,
            "viewer_focus_evidence": (
                "operator_attested_and_state_client_connected"
                if stratum == "focused"
                else "server_verified_zero_state_clients"
            ),
            "wall_phase_latency_ns": wall,
            "planner_stage_latency_ns": planner_stages,
            "process_cpu_measurement": {
                "source": "/proc/PID/stat aggregate process user+system time",
                "clock_tick_hz": cpu_clock_tick_hz,
                "predispatch": {
                    "latency_ns": _summary(cpu_estimates),
                    "uncertainty_ns": _summary(cpu_uncertainties),
                },
                "kernel_action": {
                    "latency_ns": _summary(action_cpu_estimates),
                    "uncertainty_ns": _summary(action_cpu_uncertainties),
                },
                "behavior_node": {
                    "latency_ns": _summary(behavior_cpu_estimates),
                    "uncertainty_ns": _summary(behavior_cpu_uncertainties),
                },
            },
            "pipeline_before": pipeline_before,
            "pipeline_after": pipeline_after,
            "scene_authority_before": authority_before,
            "scene_authority_after": authority_after,
            "failed_or_timeout_observations": failed_observations,
            "samples": samples,
        },
    }


def _failed_drop_observations(
    events: list[dict[str, Any]], *, through_execution_id: int
) -> list[dict[str, Any]]:
    successful = set(_successful_drop_execution_ids(events))
    candidates = sorted(
        {
            int(event.get("execution_id") or 0)
            for event in events
            if _event_move_id(event) == "drop_approach"
            and 0 < int(event.get("execution_id") or 0) <= through_execution_id
        }
    )
    failures: list[dict[str, Any]] = []
    for execution_id in candidates:
        if execution_id in successful:
            continue
        selected = [
            event
            for event in events
            if int(event.get("execution_id") or 0) == execution_id
        ]
        terminal = next(
            (
                event
                for event in reversed(selected)
                if event.get("type")
                in {"error_raised", "validation_failed", "state_transition"}
                and (
                    event.get("type") != "state_transition"
                    or event.get("to_state")
                    in {"Failed", "InterventionRequired", "Stopped"}
                )
            ),
            None,
        )
        failures.append(
            {
                "execution_id": execution_id,
                "terminal_event": terminal,
            }
        )
    return failures


def _required_artifact_provenance() -> dict[str, Any]:
    return {
        "program": {
            "name": _ISAAC_PROGRAM_NAME,
            "path": str(_ISAAC_PROGRAM.relative_to(REPO_ROOT)),
            "sha256": hashlib.sha256(_ISAAC_PROGRAM.read_bytes()).hexdigest(),
        },
        "deployment": {
            "path": str(_ISAAC_DEPLOYMENT.relative_to(REPO_ROOT)),
            "sha256": hashlib.sha256(_ISAAC_DEPLOYMENT.read_bytes()).hexdigest(),
        },
    }


def _scene_authority_identity(snapshot: dict[str, Any]) -> dict[str, Any]:
    revision = dict(snapshot.get("revision") or {})
    policy = dict(snapshot.get("configured_collision_policy") or {})
    policy_revision = dict(policy.get("revision") or {})
    for field, value in (
        ("scene revision", revision.get("revision_id")),
        ("collision policy revision", policy_revision.get("revision_id")),
    ):
        if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
            raise RuntimeError(f"live scene authority has no valid {field} id")
    for field, value in (
        ("scene revision", revision.get("content_hash")),
        ("collision policy revision", policy_revision.get("content_hash")),
    ):
        if not isinstance(value, str) or not value:
            raise RuntimeError(f"live scene authority has no {field} hash")
    policy_id = policy.get("policy_id")
    if not isinstance(policy_id, str) or not policy_id:
        raise RuntimeError("live scene authority has no collision policy id")
    return {
        "revision": revision,
        "transaction_sequence": int(snapshot.get("transaction_sequence") or 0),
        "scene_admission_epoch": int(snapshot.get("scene_admission_epoch") or 0),
        "topology_epoch": int(snapshot.get("topology_epoch") or 0),
        "policy_epoch": int(snapshot.get("policy_epoch") or 0),
        "configured_collision_policy": {
            "policy_id": policy_id,
            "revision": policy_revision,
        },
    }


def _source_and_environment(
    inventory: dict[str, Any],
    *,
    stratum: str,
    cpu_set: list[int],
    artifact_provenance: dict[str, Any],
    command: str,
) -> tuple[dict[str, Any], dict[str, Any]]:
    source_inventory = inventory["source"]
    versions = source_inventory["roboplan_packages"]
    python_versions = source_inventory["python_packages"]
    source = {
        "manyforge_commit": source_inventory["manyforge_commit"],
        "manyforge_dirty": source_inventory["manyforge_dirty"],
        "manyforge_worktree_sha256": source_inventory[
            "manyforge_worktree_sha256"
        ],
        "roboplan_version": versions.get("roboplan", "unknown"),
        "roboplan_ref": source_inventory["roboplan_ref"],
        "roboplan_commit": source_inventory["roboplan_commit"],
        "roboplan_source_clean": source_inventory.get(
            "roboplan_source_clean", False
        ),
        "roboplan_package_versions": versions,
        "roboplan_patch_manifest_sha256": source_inventory.get(
            "roboplan_patch_manifest_sha256"
        ),
        "roboplan_patches": source_inventory.get("roboplan_patches", []),
        "pinocchio_version": python_versions.get("libpinocchio")
        or python_versions.get("pin"),
        "coal_version": python_versions.get("libcoal")
        or python_versions.get("coal"),
        "eigenpy_version": python_versions.get("eigenpy"),
        "program": artifact_provenance["program"],
        "deployment": artifact_provenance["deployment"],
    }
    environment_inventory = inventory["environment"]
    environment = {
        "lane": "ros_isaac_container",
        "container": os.uname().nodename,
        "architecture": environment_inventory["architecture"],
        "cpu_count": environment_inventory["cpu_count"],
        "cpu_set": ",".join(str(value) for value in cpu_set),
        "cpu_governor": ",".join(environment_inventory["cpu_governors"])
        or None,
        "viewer_stratum": stratum,
        "command": command,
    }
    return source, environment


def collect(args: argparse.Namespace) -> dict[str, Any]:
    base_url = args.composer_url.rstrip("/")
    inventory = collect_inventory(args.repo_root.resolve())
    if inventory["source"]["manyforge_dirty"] and not args.allow_dirty_source:
        raise RuntimeError(
            f"formal {args.phase} evidence requires a clean ManyForge source tree; "
            "use --allow-dirty-source only for an explicitly labeled preflight"
        )
    artifact_provenance = require_loaded_artifacts_match(
        base_url,
        program_path=_ISAAC_PROGRAM,
        deployment_path=_ISAAC_DEPLOYMENT,
        expected_program_name=_ISAAC_PROGRAM_NAME,
    )
    status = _request_json(base_url, "/api/cycle/status")
    if status.get("state") != "idle":
        raise RuntimeError("live qualification must start from an idle cycle")
    viewer_clients = _enforce_viewer_stratum(
        base_url,
        stratum=args.viewer_stratum,
        focused_attested=args.operator_attests_viewer_focused,
    )
    initial_events = _request_json(base_url, "/api/kernel/events?since=0")
    initial_batch = list(initial_events.get("events") or [])
    cursor = int(initial_events.get("last_event_index") or 0)
    pipeline_before = _pipeline_status(base_url)
    baseline_sequence = max(
        [int(event.get("sequence") or 0) for event in initial_batch]
        + [int(pipeline_before.get("last_sequence_seen") or 0)]
    )
    sequence_tracker = _KernelSequenceTracker(baseline_sequence)
    capture = _request_json(
        base_url,
        "/api/debug/qualification/planning-capture/start",
        method="POST",
        payload={"capacity": args.capture_capacity},
    )
    generation = int(capture.get("generation") or 0)
    if generation <= 0:
        raise RuntimeError("planning qualification capture did not start")

    sampler = _ProcessCpuSampler(args.composer_pid, args.cpu_sample_period_s)
    sampler_started = False
    cycle_started = False
    captured_events: list[dict[str, Any]] = []
    stopped_capture: dict[str, Any] | None = None
    try:
        reset_and_start_simulation(base_url, 30.0)
        time.sleep(args.simulation_settle_s)
        authority_before = _scene_authority_identity(
            _request_json(
                base_url,
                "/api/debug/qualification/scene-authority-snapshot",
            )
        )
        sampler.start()
        sampler_started = True
        _request_json(base_url, "/api/cycle/run", method="POST", payload={})
        cycle_started = True
        captured_events, cursor = _wait_for_successes(
            base_url,
            cursor=cursor,
            sequence_tracker=sequence_tracker,
            required_successes=args.successful_observations,
            timeout_s=args.timeout_s,
            viewer_stratum=args.viewer_stratum,
            focused_attested=args.operator_attests_viewer_focused,
            poll_period_s=args.poll_period_s,
        )
        _request_json(base_url, "/api/cycle/full-stop", method="POST", payload={})
        cycle_started = False
        # Drain terminal stop events so sequence/loss checks cover the complete
        # qualification-owned run, even though they are not timing samples.
        final_batch, cursor = _drain_kernel_events_until_quiescent(
            base_url,
            cursor=cursor,
            sequence_tracker=sequence_tracker,
        )
        captured_events.extend(final_batch)
        authority_after = _scene_authority_identity(
            _request_json(
                base_url,
                "/api/debug/qualification/scene-authority-snapshot",
            )
        )
        stopped_capture = _request_json(
            base_url,
            "/api/debug/qualification/planning-capture/stop",
            method="POST",
            payload={"generation": generation, "idle_timeout_ms": 10_000},
        )
        generation = 0
    finally:
        if cycle_started:
            try:
                _request_json(
                    base_url, "/api/cycle/full-stop", method="POST", payload={}
                )
            except Exception:
                pass
        if generation:
            try:
                stopped_capture = _request_json(
                    base_url,
                    "/api/debug/qualification/planning-capture/stop",
                    method="POST",
                    payload={"generation": generation, "idle_timeout_ms": 10_000},
                )
            except Exception:
                pass
        if sampler_started:
            sampler.stop()

    if stopped_capture is None:
        raise RuntimeError("planning qualification capture did not stop cleanly")
    capture_status = dict(stopped_capture.get("status") or {})
    if int(capture_status.get("dropped_total") or 0) != 0:
        raise RuntimeError("planning qualification recorder dropped evidence")
    if int(capture_status.get("in_flight") or 0) != 0:
        raise RuntimeError("planning qualification recorder stopped while in flight")
    if int(capture_status.get("behavior_dropped_total") or 0) != 0:
        raise RuntimeError("behavior qualification recorder dropped evidence")
    if int(capture_status.get("behavior_in_flight") or 0) != 0:
        raise RuntimeError("behavior qualification recorder stopped while in flight")
    planner_records = list(stopped_capture.get("records") or [])
    paired = _pair_planner_records(captured_events, planner_records)
    successful_ids = _successful_drop_execution_ids(captured_events)
    if len(successful_ids) < args.successful_observations:
        raise RuntimeError("successful observation count regressed after capture stop")
    selected_ids = successful_ids[: args.successful_observations]
    samples = [
        _sample_payload(
            execution_id,
            captured_events,
            paired[execution_id],
            sampler,
            args.measurement_mode,
        )
        for execution_id in selected_ids
    ]
    failed_observations = _failed_drop_observations(
        captured_events, through_execution_id=selected_ids[-1]
    )
    pipeline_after = _pipeline_status(base_url)
    for key in (
        "collector_dropped_total",
        "manager_dropped_total",
        "gap_records_total",
    ):
        if int(pipeline_after.get(key) or 0) != int(pipeline_before.get(key) or 0):
            raise RuntimeError(f"kernel event pipeline loss advanced during live run: {key}")
    require_source_inventory_stable(
        inventory,
        collect_inventory(args.repo_root.resolve()),
    )

    source, environment = _source_and_environment(
        inventory,
        stratum=args.viewer_stratum,
        cpu_set=sorted(os.sched_getaffinity(0)),
        artifact_provenance=artifact_provenance,
        command=args.command,
    )
    environment["source_verified_after_run"] = True
    record = _aggregate_live_record(
        samples=samples,
        stratum=args.viewer_stratum,
        measurement_mode=args.measurement_mode,
        viewer_clients=viewer_clients,
        pipeline_before=pipeline_before,
        pipeline_after=pipeline_after,
        cpu_clock_tick_hz=sampler.clock_tick_hz,
        failed_observations=failed_observations,
        authority_before=authority_before,
        authority_after=authority_after,
    )
    return {
        "schema_version": EVIDENCE_SCHEMA,
        "run_id": args.run_id
        or f"{args.phase.replace('_', '-')}-live-{args.viewer_stratum}-"
        f"{args.measurement_mode}-"
        f"{time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())}",
        "phase": args.phase,
        "source": source,
        "environment": environment,
        "records": [record],
        "inventory": {
            "build_references": inventory["build_references"],
            "capabilities": inventory["capabilities"],
            "workarounds": inventory["workarounds"],
        },
    }


def _failed_evidence(
    args: argparse.Namespace, failure: BaseException
) -> dict[str, Any]:
    """Preserve a failed/timeout live attempt as evidence instead of dropping it."""

    inventory = collect_inventory(args.repo_root.resolve())
    source, environment = _source_and_environment(
        inventory,
        stratum=args.viewer_stratum,
        cpu_set=sorted(os.sched_getaffinity(0)),
        artifact_provenance=_required_artifact_provenance(),
        command=args.command,
    )
    return {
        "schema_version": EVIDENCE_SCHEMA,
        "run_id": args.run_id
        or f"{args.phase.replace('_', '-')}-live-{args.viewer_stratum}-"
        f"{args.measurement_mode}-failed-"
        f"{time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())}",
        "phase": args.phase,
        "source": source,
        "environment": environment,
        "records": [
            {
                "record_type": "live_planning_stage",
                "fixture_id": "isaac_light_drop_approach",
                "passed": False,
                "sample_count": 0,
                "details": {
                    "viewer_stratum": args.viewer_stratum,
                    "measurement_mode": args.measurement_mode,
                    "required_successful_observations": (
                        args.successful_observations
                    ),
                    "failure_type": type(failure).__name__,
                    "failure": str(failure),
                    "failed_or_timeout_observations": [
                        {
                            "reason": str(failure),
                            "type": type(failure).__name__,
                        }
                    ],
                },
            }
        ],
        "inventory": {
            "build_references": inventory["build_references"],
            "capabilities": inventory["capabilities"],
            "workarounds": inventory["workarounds"],
        },
    }


def _failed_output_path(requested: Path) -> Path:
    timestamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    suffix = requested.suffix or ".json"
    stem = requested.name[: -len(suffix)] if requested.suffix else requested.name
    return requested.with_name(f"{stem}.failed-{timestamp}-{os.getpid()}{suffix}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=REPO_ROOT)
    parser.add_argument("--composer-url", default="http://127.0.0.1:9000")
    parser.add_argument(
        "--viewer-stratum", choices=("disconnected", "focused"), required=True
    )
    parser.add_argument(
        "--measurement-mode",
        choices=("primary", "collision_probe_attribution"),
        default="primary",
    )
    parser.add_argument("--operator-attests-viewer-focused", action="store_true")
    parser.add_argument("--successful-observations", type=int, default=30)
    parser.add_argument("--capture-capacity", type=int, default=1024)
    parser.add_argument("--composer-pid", type=int, default=1)
    parser.add_argument("--cpu-sample-period-s", type=float, default=0.005)
    parser.add_argument("--poll-period-s", type=float, default=0.2)
    parser.add_argument("--simulation-settle-s", type=float, default=3.0)
    parser.add_argument("--timeout-s", type=float, default=3600.0)
    parser.add_argument("--run-id")
    parser.add_argument(
        "--phase",
        choices=("rp5_0", "rp5_3", "rp5_7"),
        default="rp5_0",
        help="Qualification phase recorded in success and failure evidence.",
    )
    parser.add_argument(
        "--allow-dirty-source",
        action="store_true",
        help="permit an explicitly labeled development preflight on dirty source",
    )
    parser.add_argument("--output", type=Path, required=True)
    effective_argv = list(sys.argv[1:] if argv is None else argv)
    args = parser.parse_args(effective_argv)
    args.command = shlex.join(
        [sys.executable, str(Path(__file__).resolve()), *effective_argv]
    )
    if args.successful_observations < 1:
        parser.error("--successful-observations must be positive")
    if not 1 <= args.capture_capacity <= 4096:
        parser.error("--capture-capacity must be in [1, 4096]")
    for field in ("cpu_sample_period_s", "poll_period_s", "timeout_s"):
        value = float(getattr(args, field))
        if not math.isfinite(value) or value <= 0.0:
            parser.error(f"--{field.replace('_', '-')} must be finite and positive")
    if not math.isfinite(args.simulation_settle_s) or args.simulation_settle_s < 0:
        parser.error("--simulation-settle-s must be finite and non-negative")
    try:
        evidence = collect(args)
        _write_evidence_exclusive(
            args.output,
            evidence,
            validator=validate_evidence,
        )
    except (OSError, ValueError, RuntimeError, TimeoutError) as exc:
        try:
            evidence = _failed_evidence(args, exc)
            failed_output = _failed_output_path(args.output)
            _write_evidence_exclusive(
                failed_output,
                evidence,
                validator=validate_evidence,
            )
            print(f"RP5 live failure evidence: {failed_output}", file=sys.stderr)
        except Exception as evidence_error:  # noqa: BLE001 - retain root cause
            print(
                f"RP5 live failure evidence could not be written: {evidence_error}",
                file=sys.stderr,
            )
        print(f"RP5 live baseline failed: {exc}", file=sys.stderr)
        return 2
    print(args.output)
    return 0 if all(record.get("passed") is True for record in evidence["records"]) else 2


if __name__ == "__main__":
    raise SystemExit(main())
