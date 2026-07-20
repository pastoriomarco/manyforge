#!/usr/bin/env python3
"""Collect one RP5 live Isaac drop-approach qualification stratum.

Run this script inside the active Composer ROS container. It consumes existing
kernel lifecycle stamps and the opt-in planner qualification recorder; it does
not add timers, callbacks, or sampling work to the execution hot path.
"""

from __future__ import annotations

import argparse
import bisect
import copy
import hashlib
import json
import math
import os
import re
import shlex
import statistics
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any, Callable, NamedTuple


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
    _capture_payload,
    _read_yaml,
    loaded_program_payload_sha256,
    require_loaded_artifacts_match,
    reset_and_start_simulation,
)


_ISAAC_PROGRAM_RELATIVE = Path("examples/isaac_manipulator_scene_light.program.yaml")
_ISAAC_DEPLOYMENT_RELATIVE = Path(
    "examples/isaac_manipulator_scene_light_ros2_control.deployment.yaml"
)
_ISAAC_PROGRAM_NAME = "isaac_manipulator_scene_light"

_KERNEL_RUNTIME_METRIC_PREFIXES = (
    "controller_",
    "runtime_collision_",
    "scene_projection_",
    "validation_collision_",
)
_KERNEL_RUNTIME_REQUIRED_METRICS = frozenset(
    {
        "controller_command_progress_samples_total",
        "controller_executor_thread_count",
        "controller_last_command_progress_latency_ns",
        "controller_max_command_progress_latency_ns",
        "runtime_collision_tick_late_periods",
        "runtime_collision_tick_max_ns",
        "runtime_collision_tick_missed_periods",
        "runtime_collision_ticks_completed",
        "runtime_collision_ticks_faulted",
        "runtime_collision_ticks_started",
        "runtime_collision_lookahead_cycle_budget_ns",
        "runtime_collision_lookahead_jobs_clear",
        "runtime_collision_lookahead_jobs_cycle_timeout",
        "runtime_collision_lookahead_jobs_inconclusive",
        "runtime_collision_lookahead_jobs_query_timeout",
        "runtime_collision_lookahead_jobs_started",
        "runtime_collision_lookahead_max_cycle_duration_ns",
        "runtime_collision_lookahead_max_start_jitter_ns",
        "runtime_collision_lookahead_max_worker_active_duration_ns",
        "runtime_collision_lookahead_max_samples",
        "runtime_collision_lookahead_workers_effective",
        "runtime_collision_stop_longest_unavailable_ns",
        "runtime_collision_stop_unavailable_ticks",
        "runtime_collision_stop_zero_check_windows",
        "scene_projection_completion_callback_exceptions_total",
        "scene_projection_misses_total",
        "scene_projection_terminal_retry_exhausted_total",
        "validation_collision_workers_effective",
    }
)
_KERNEL_RUNTIME_COUNTER_METRICS = frozenset(
    {
        "controller_action_feedback_callbacks_total",
        "controller_action_result_callbacks_total",
        "controller_cancel_confirmations_total",
        "controller_command_progress_samples_total",
        "controller_jtc_cancel_reference_publications_total",
        "controller_jtc_cancel_reference_rejections_total",
        "controller_observed_state_reader_retry_exhausted_total",
        "controller_state_callbacks_total",
        "controller_stop_confirmations_total",
        "runtime_collision_stop_queries_executed",
        "runtime_collision_stop_ticks_attempted",
        "runtime_collision_stop_unavailable_ticks",
        "runtime_collision_stop_windows_completed",
        "runtime_collision_stop_windows_started",
        "runtime_collision_stop_zero_check_windows",
        "runtime_collision_tick_late_periods",
        "runtime_collision_tick_missed_periods",
        "runtime_collision_ticks_completed",
        "runtime_collision_ticks_faulted",
        "runtime_collision_ticks_started",
        "runtime_collision_lookahead_clear_permit_try_lock_misses",
        "runtime_collision_lookahead_clear_permits_invalidated",
        "runtime_collision_lookahead_jobs_clear",
        "runtime_collision_lookahead_jobs_collision",
        "runtime_collision_lookahead_jobs_cycle_timeout",
        "runtime_collision_lookahead_jobs_inconclusive",
        "runtime_collision_lookahead_jobs_query_timeout",
        "runtime_collision_lookahead_jobs_started",
        "runtime_collision_lookahead_jobs_superseded",
        "runtime_collision_lookahead_reruns_coalesced",
        "runtime_collision_lookahead_stale_results",
        "runtime_collision_snapshot_refresh_failures",
        "runtime_collision_unavailable_events_suppressed",
        "scene_projection_active_requests_total",
        "scene_projection_completion_callback_exceptions_total",
        "scene_projection_joint_requests_total",
        "scene_projection_misses_total",
        "scene_projection_requests_coalesced_total",
        "scene_projection_requests_discarded_total",
        "scene_projection_requests_rejected_while_suspended_total",
        "scene_projection_successful_updates_total",
        "scene_projection_terminal_retries_scheduled_total",
        "scene_projection_terminal_retry_exhausted_total",
        "scene_projection_terminal_retry_successes_total",
        "validation_collision_jobs_abandoned",
        "validation_collision_jobs_cancelled",
        "validation_collision_jobs_completed",
        "validation_collision_jobs_dropped",
    }
)
_KERNEL_RUNTIME_FRESH_BASELINE_METRICS = frozenset(
    {
        "controller_command_progress_samples_total",
        "controller_last_command_progress_latency_ns",
        "controller_max_command_progress_latency_ns",
        "runtime_collision_tick_max_ns",
        "runtime_collision_ticks_started",
        "runtime_collision_lookahead_jobs_started",
        "runtime_collision_lookahead_max_cycle_duration_ns",
        "runtime_collision_lookahead_max_start_jitter_ns",
        "runtime_collision_lookahead_max_worker_active_duration_ns",
        "validation_collision_jobs_abandoned",
        "validation_collision_jobs_cancelled",
        "validation_collision_jobs_completed",
        "validation_collision_jobs_dropped",
        "validation_collision_jobs_queued",
    }
)

_STEP18_SUPPORT_CONTACT_MODES = frozenset(
    {"signed_penetration", "binary_contact", "bounded_penetration"}
)
_STEP18_BASELINE_MODE = "current_all_active_distance"
_STEP18_LIVE_MODES = _STEP18_SUPPORT_CONTACT_MODES | {_STEP18_BASELINE_MODE}
_STEP18_SUPPORTED_OBJECT_ID = "isaac_cheese_box"
_STEP18_SUPPORT_OBJECT_ID = "isaac_table_low_327"
_STEP18_BOUNDED_PENETRATION_M = 0.03


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


def _reload_matched_artifacts(
    base_url: str, *, program_path: Path, deployment_path: Path
) -> dict[str, Any]:
    """Force a disk-backed runtime rebuild before every formal live row."""

    request = {
        "path": str(program_path.resolve()),
        "deploymentPath": str(deployment_path.resolve()),
        "forceDiscardOverrides": True,
    }
    response = _request_json(
        base_url,
        "/api/program/load",
        method="POST",
        payload=request,
        timeout_s=60.0,
    )
    return {
        "schema_version": "manyforge.step18_artifact_reload.v1",
        "endpoint": "/api/program/load",
        "program_path": request["path"],
        "deployment_path": request["deploymentPath"],
        "force_discard_overrides": True,
        "response_payload_sha256": loaded_program_payload_sha256(response),
    }


def _kernel_runtime_metrics(
    base_url: str, *, timeout_s: float = 15.0
) -> dict[str, Any]:
    """Capture the native counters that prove controller/RT interference."""

    payload = _request_json(
        base_url, "/api/kernel/metrics", timeout_s=timeout_s
    )
    missing = sorted(_KERNEL_RUNTIME_REQUIRED_METRICS - payload.keys())
    if missing:
        raise RuntimeError(
            "kernel metrics omitted required runtime evidence: "
            + ", ".join(missing)
        )
    return {
        key: payload[key]
        for key in sorted(payload)
        if key.startswith(_KERNEL_RUNTIME_METRIC_PREFIXES)
    }


def _kernel_runtime_counter_deltas(
    before: dict[str, Any], after: dict[str, Any]
) -> dict[str, int]:
    """Derive explicit deltas without treating gauges/maxima as counters."""

    deltas: dict[str, int] = {}
    for key in sorted(_KERNEL_RUNTIME_COUNTER_METRICS):
        before_value = before.get(key)
        after_value = after.get(key)
        if (
            isinstance(before_value, bool)
            or not isinstance(before_value, int)
            or isinstance(after_value, bool)
            or not isinstance(after_value, int)
        ):
            raise RuntimeError(f"kernel runtime counter {key!r} is not an integer")
        if after_value < before_value:
            raise RuntimeError(f"kernel runtime counter {key!r} regressed")
        deltas[key] = after_value - before_value
    return deltas


def _nonnegative_metric(metrics: dict[str, Any], key: str) -> int:
    value = metrics.get(key)
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise RuntimeError(f"kernel runtime metric {key!r} is not non-negative integer")
    return value


class _ControllerProgressSampleTracker:
    """Capture every native command-progress latency without approximating p95.

    The backend publishes count/last/max as one coherent observer snapshot.  A
    transition by exactly one therefore identifies one exact latency sample.
    Poll gaps fail the run instead of being interpolated or treated as a sampled
    gauge.  Formal collection also starts from the required zero-count baseline.
    """

    _COUNT = "controller_command_progress_samples_total"
    _LAST = "controller_last_command_progress_latency_ns"
    _MAXIMUM = "controller_max_command_progress_latency_ns"

    def __init__(
        self, baseline: dict[str, Any], *, observer_mode: str = "coherent_native"
    ) -> None:
        if observer_mode not in {"coherent_native", "quiescent_repeated"}:
            raise RuntimeError("unsupported controller-progress observer mode")
        self._observer_mode = observer_mode
        self._baseline_count = _nonnegative_metric(baseline, self._COUNT)
        self._last_count = self._baseline_count
        self._last_native_max = _nonnegative_metric(baseline, self._MAXIMUM)
        self._samples: list[dict[str, int]] = []
        if self._baseline_count == 0:
            if _nonnegative_metric(baseline, self._LAST) != 0:
                raise RuntimeError(
                    "zero controller-progress baseline has a nonzero last latency"
                )
            if self._last_native_max != 0:
                raise RuntimeError(
                    "zero controller-progress baseline has a nonzero maximum latency"
                )

    @property
    def sample_count(self) -> int:
        return len(self._samples)

    def observe(
        self, metrics: dict[str, Any], *, observed_monotonic_ns: int | None = None
    ) -> None:
        count = _nonnegative_metric(metrics, self._COUNT)
        last = _nonnegative_metric(metrics, self._LAST)
        maximum = _nonnegative_metric(metrics, self._MAXIMUM)
        if count < self._last_count:
            raise RuntimeError("controller-progress sample counter regressed")
        if maximum < self._last_native_max:
            raise RuntimeError("controller-progress native maximum regressed")
        delta = count - self._last_count
        if delta > 1:
            raise RuntimeError(
                "controller-progress collector missed exact samples: "
                f"counter advanced by {delta} between polls"
            )
        if delta == 1:
            if last > maximum:
                raise RuntimeError(
                    "controller-progress coherent snapshot has last above maximum"
                )
            self._samples.append(
                {
                    "counter_value": count,
                    "latency_ns": last,
                    "native_max_ns": maximum,
                    "observed_monotonic_ns": (
                        time.monotonic_ns()
                        if observed_monotonic_ns is None
                        else observed_monotonic_ns
                    ),
                    "quiescent_reads": (
                        3 if self._observer_mode == "quiescent_repeated" else 0
                    ),
                }
            )
        self._last_count = count
        self._last_native_max = maximum

    def finalize(self, metrics: dict[str, Any]) -> dict[str, Any]:
        self.observe(metrics)
        final_count = _nonnegative_metric(metrics, self._COUNT)
        expected = final_count - self._baseline_count
        if expected != len(self._samples):
            raise RuntimeError(
                "controller-progress raw sample cardinality disagrees with native "
                f"counter delta: {len(self._samples)} != {expected}"
            )
        if not self._samples:
            raise RuntimeError("live run recorded no controller-progress latency samples")
        raw = [sample["latency_ns"] for sample in self._samples]
        native_max = _nonnegative_metric(metrics, self._MAXIMUM)
        if max(raw) != native_max:
            raise RuntimeError(
                "controller-progress raw sample maximum disagrees with native maximum: "
                f"{max(raw)} != {native_max}"
            )
        return {
            "source": (
                "collector_quiescent_repeated_counter_transition"
                if self._observer_mode == "quiescent_repeated"
                else "coherent_native_counter_transition"
            ),
            "instrumentation": (
                "collector_side_unpatched_backend"
                if self._observer_mode == "quiescent_repeated"
                else "native_coherent_observer"
            ),
            "completeness_rule": (
                "three identical quiescent snapshots and every counter transition "
                "is exactly one; gaps fail"
                if self._observer_mode == "quiescent_repeated"
                else "every counter transition is exactly one; gaps fail"
            ),
            "baseline_counter": self._baseline_count,
            "final_counter": final_count,
            "native_counter_delta": expected,
            "native_max_ns": native_max,
            "latency_ns": _summary(raw, include_p99=True),
            "samples": list(self._samples),
        }


def _controller_progress_metrics_snapshot(
    base_url: str, *, observer_mode: str
) -> dict[str, Any]:
    if observer_mode == "coherent_native":
        return _kernel_runtime_metrics(base_url)
    if observer_mode != "quiescent_repeated":
        raise RuntimeError("unsupported controller-progress observer mode")
    stable: dict[str, int] | None = None
    stable_reads = 0
    latest: dict[str, Any] | None = None
    keys = (
        "controller_command_progress_samples_total",
        "controller_last_command_progress_latency_ns",
        "controller_max_command_progress_latency_ns",
    )
    for _ in range(30):
        latest = _kernel_runtime_metrics(base_url)
        triple = {key: _nonnegative_metric(latest, key) for key in keys}
        if triple == stable:
            stable_reads += 1
        else:
            stable = triple
            stable_reads = 1
        if stable_reads >= 3:
            return latest
        time.sleep(0.001)
    raise RuntimeError(
        "controller-progress observer could not obtain three identical "
        "quiescent snapshots"
    )


def _require_fresh_kernel_runtime_baseline(metrics: dict[str, Any]) -> None:
    """Make maxima/counters attributable to exactly this live stratum."""

    dirty = {
        key: metrics.get(key)
        for key in sorted(_KERNEL_RUNTIME_FRESH_BASELINE_METRICS)
        if metrics.get(key) != 0
    }
    if dirty:
        details = ", ".join(f"{key}={value!r}" for key, value in dirty.items())
        raise RuntimeError(
            "live qualification requires a fresh kernel metric baseline; "
            "reload the matched program/deployment before retrying: "
            + details
        )


def _failure_runtime_evidence(base_url: str) -> dict[str, Any]:
    """Retain actionable native/cycle state even when collection aborts."""

    evidence: dict[str, Any] = {}
    capture_errors: dict[str, str] = {}
    try:
        evidence["cycle_status"] = _request_json(
            base_url, "/api/cycle/status", timeout_s=2.0
        )
    except Exception as exc:  # noqa: BLE001 - preserve the original failure.
        capture_errors["cycle_status"] = f"{type(exc).__name__}: {exc}"
    try:
        evidence["kernel_runtime_metrics"] = _kernel_runtime_metrics(
            base_url, timeout_s=2.0
        )
    except Exception as exc:  # noqa: BLE001 - preserve the original failure.
        capture_errors["kernel_runtime_metrics"] = f"{type(exc).__name__}: {exc}"
    if capture_errors:
        evidence["capture_errors"] = capture_errors
    return evidence


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


def _canonical_json_sha256(value: Any) -> str:
    return hashlib.sha256(
        json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()


def _program_scene_resources(program: dict[str, Any], field: str) -> list[dict[str, Any]]:
    resources = program.get("sceneResources")
    if not isinstance(resources, list) or not resources:
        raise RuntimeError(f"{field} has no sceneResources")
    if any(not isinstance(resource, dict) for resource in resources):
        raise RuntimeError(f"{field}.sceneResources contains a non-object")
    return copy.deepcopy(resources)


def _authored_step18_contact(
    scene: list[dict[str, Any]], field: str
) -> tuple[dict[str, Any], dict[str, Any]]:
    supported = next(
        (
            resource
            for resource in scene
            if resource.get("objectId") == _STEP18_SUPPORTED_OBJECT_ID
        ),
        None,
    )
    if supported is None:
        raise RuntimeError(
            f"{field} has no {_STEP18_SUPPORTED_OBJECT_ID!r} scene resource"
        )
    metadata = supported.get("metadata")
    if not isinstance(metadata, dict):
        raise RuntimeError(f"{field} supported object has no metadata")
    contacts = metadata.get("support_contacts")
    if not isinstance(contacts, list):
        raise RuntimeError(f"{field} supported object has no support_contacts list")
    matching = [
        contact
        for contact in contacts
        if isinstance(contact, dict)
        and contact.get("support_object_id") == _STEP18_SUPPORT_OBJECT_ID
    ]
    if len(matching) != 1:
        raise RuntimeError(
            f"{field} must contain exactly one Step 18 support-contact pair"
        )
    return supported, matching[0]


def _step18_contact_for_mode(
    saved_contact: dict[str, Any],
    mode: str,
    bounded_penetration_m: float,
) -> dict[str, Any]:
    if mode not in _STEP18_LIVE_MODES:
        raise RuntimeError(f"unsupported Step 18 support-contact mode {mode!r}")
    if (
        saved_contact.get("mode", "contact_release") != "contact_release"
        or saved_contact.get("release_evidence_mode", "signed_penetration")
        != "signed_penetration"
    ):
        raise RuntimeError(
            "saved Isaac support contact is not the pinned precise baseline"
        )
    if mode in {_STEP18_BASELINE_MODE, "signed_penetration"}:
        return copy.deepcopy(saved_contact)
    if mode == "binary_contact":
        return {
            "support_object_id": _STEP18_SUPPORT_OBJECT_ID,
            "mode": "contact_release",
            "release_evidence_mode": "binary_contact",
            "enforce_max_start_penetration": False,
            "max_start_penetration_m": 0.0,
            "release_tolerance_m": 0.0,
            "min_release_progress_m": 0.0,
            "require_separation_by_end": True,
            "require_strictly_decreasing_penetration": False,
            "allow_recontact": False,
        }
    if not math.isfinite(bounded_penetration_m) or bounded_penetration_m < 0.0:
        raise RuntimeError("bounded Step 18 penetration must be finite and non-negative")
    return {
        "support_object_id": _STEP18_SUPPORT_OBJECT_ID,
        "mode": "bounded_penetration",
        "max_contact_penetration_m": bounded_penetration_m,
    }


def _resolved_step18_contact(
    authority: dict[str, Any], expected_mode: str
) -> dict[str, Any]:
    policy = authority.get("configured_collision_policy")
    if not isinstance(policy, dict):
        raise RuntimeError("scene authority omitted configured collision policy")
    contacts = policy.get("support_contacts")
    if not isinstance(contacts, list):
        raise RuntimeError("configured collision policy omitted support contacts")
    matching: list[dict[str, Any]] = []
    for contact in contacts:
        if not isinstance(contact, dict):
            continue
        pair = contact.get("pair")
        if not isinstance(pair, dict):
            continue
        if {pair.get("body_a"), pair.get("body_b")} == {
            _STEP18_SUPPORTED_OBJECT_ID,
            _STEP18_SUPPORT_OBJECT_ID,
        }:
            matching.append(contact)
    if len(matching) != 1:
        raise RuntimeError(
            "resolved collision policy must contain exactly one Step 18 pair"
        )
    contact = matching[0]
    if expected_mode == "bounded_penetration":
        valid = contact.get("mode") == "bounded_penetration"
    elif expected_mode == _STEP18_BASELINE_MODE:
        valid = (
            contact.get("mode") == "contact_release"
            and contact.get("release_evidence_mode")
            in {None, "signed_penetration"}
        )
    else:
        valid = (
            contact.get("mode") == "contact_release"
            and contact.get("release_evidence_mode") == expected_mode
        )
    if not valid:
        raise RuntimeError(
            "resolved Step 18 support-contact mode disagrees with authored mode"
        )
    return copy.deepcopy(contact)


def _apply_step18_support_contact_mode(
    base_url: str,
    *,
    mode: str,
    bounded_penetration_m: float,
) -> dict[str, Any]:
    """Apply one in-memory policy arm while retaining the canonical artifact."""

    saved_program = _request_json(base_url, "/api/program/saved")
    saved_scene = _program_scene_resources(saved_program, "saved program")
    supported, saved_contact = _authored_step18_contact(saved_scene, "saved program")
    expected_contact = _step18_contact_for_mode(
        saved_contact, mode, bounded_penetration_m
    )
    supported_metadata = copy.deepcopy(supported["metadata"])
    supported_metadata["support_contacts"] = [
        expected_contact
        if contact.get("support_object_id") == _STEP18_SUPPORT_OBJECT_ID
        else copy.deepcopy(contact)
        for contact in supported_metadata["support_contacts"]
    ]
    supported["metadata"] = supported_metadata

    response = _request_json(
        base_url,
        "/api/session/apply",
        method="POST",
        payload={"scene": saved_scene},
    )
    if response.get("status") != "ok":
        raise RuntimeError("Composer did not confirm Step 18 scene apply")

    effective_program = _request_json(base_url, "/api/program")
    effective_scene = _program_scene_resources(effective_program, "effective program")
    _, effective_contact = _authored_step18_contact(
        effective_scene, "effective program"
    )
    if effective_contact != expected_contact:
        raise RuntimeError(
            "effective Step 18 support contact differs from the applied policy"
        )
    authority_snapshot = _request_json(
        base_url, "/api/debug/qualification/scene-authority-snapshot"
    )
    resolved_contact = _resolved_step18_contact(authority_snapshot, mode)
    if mode == "bounded_penetration" and not math.isclose(
        float(resolved_contact.get("max_contact_penetration_m", math.nan)),
        bounded_penetration_m,
        rel_tol=0.0,
        abs_tol=0.0,
    ):
        raise RuntimeError("resolved bounded-penetration threshold changed")
    return {
        "mode": mode,
        "bounded_penetration_m": (
            bounded_penetration_m if mode == "bounded_penetration" else None
        ),
        "saved_scene_sha256": _canonical_json_sha256(
            _program_scene_resources(saved_program, "saved program")
        ),
        "effective_scene_sha256": _canonical_json_sha256(effective_scene),
        "authored_contact": effective_contact,
        "resolved_contact": resolved_contact,
        "scene_authority_after_apply": _scene_authority_identity(
            authority_snapshot
        ),
    }


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
    prepare_next_fresh_plan: Callable[[], None],
    controller_progress_tracker: _ControllerProgressSampleTracker | None = None,
    controller_progress_observer_mode: str = "coherent_native",
    mechanism_snapshot_out: dict[str, Any] | None = None,
) -> tuple[list[dict[str, Any]], int, int]:
    deadline = time.monotonic() + timeout_s
    events: list[dict[str, Any]] = []
    last_viewer_check = 0.0
    last_status_check = 0.0
    prepared_through_success_count = 0
    cache_invalidation_calls = 0
    while time.monotonic() < deadline:
        if controller_progress_tracker is not None:
            controller_progress_tracker.observe(
                _controller_progress_metrics_snapshot(
                    base_url,
                    observer_mode=controller_progress_observer_mode,
                )
            )
        payload = _request_json(base_url, f"/api/kernel/events?since={cursor}")
        batch = list(payload.get("events") or [])
        if batch:
            sequence_tracker.observe(batch)
            events.extend(batch)
            cursor = int(batch[-1].get("event_index") or cursor)
            if mechanism_snapshot_out is not None and not mechanism_snapshot_out:
                for event in batch:
                    if (
                        event.get("type") != "plan_started"
                        or _event_move_id(event) != "drop_approach"
                    ):
                        continue
                    execution_id = int(event.get("execution_id") or 0)
                    kernel_before = _request_json(base_url, "/api/kernel/snapshot")
                    if (
                        kernel_before.get("state") != "planning"
                        or kernel_before.get("active_move_id") != "drop_approach"
                        or int(kernel_before.get("execution_id") or 0)
                        != execution_id
                    ):
                        continue
                    scene_state = _request_json(base_url, "/api/scene/state")
                    diagnostics = scene_state.get("visual_state_diagnostics") or {}
                    robot = scene_state.get("robot") or {}
                    if (
                        diagnostics.get("served_from_visual_snapshot") is not True
                        or diagnostics.get("snapshot_stale") is True
                        or diagnostics.get("snapshot_publisher_error")
                        or diagnostics.get("objects_source") != "visual_snapshot"
                        or not robot.get("joint_names")
                        or set(robot.get("joint_names") or [])
                        != set((robot.get("joint_positions") or {}).keys())
                    ):
                        continue
                    authority_snapshot = _request_json(
                        base_url,
                        "/api/debug/qualification/scene-authority-snapshot",
                    )
                    cheese = next(
                        (
                            item
                            for item in authority_snapshot.get("objects") or []
                            if item.get("object_id") == "isaac_cheese_box"
                        ),
                        None,
                    )
                    kernel_after = _request_json(base_url, "/api/kernel/snapshot")
                    if (
                        not cheese
                        or cheese.get("attached") is not True
                        or cheese.get("frame_id") != "grasp_frame"
                        or kernel_after.get("state") != "planning"
                        or kernel_after.get("active_move_id") != "drop_approach"
                        or int(kernel_after.get("execution_id") or 0)
                        != execution_id
                    ):
                        continue
                    mechanism_snapshot_out.update(
                        {
                            "execution_id": execution_id,
                            "scene_state": scene_state,
                            "authority_snapshot": authority_snapshot,
                        }
                    )
                    break
            successful_count = len(_successful_drop_execution_ids(events))
            if successful_count >= required_successes:
                if sequence_tracker.settled:
                    return events, cursor, cache_invalidation_calls
                # The final sample exists; only sequence settlement remains.
                # Do not perturb cache contents after the measured population.
            elif successful_count > prepared_through_success_count:
                # The production program deliberately reuses trajectories. This
                # probe measures fresh planning, so clear the cache only after a
                # complete target action and only while this qualification
                # capture still owns its generation. One clear is enough even
                # when a delayed poll observes more than one new completion.
                prepare_next_fresh_plan()
                cache_invalidation_calls += 1
                prepared_through_success_count = successful_count
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
    expected_scene_revision: dict[str, Any] | None = None,
    expected_policy_revision: dict[str, Any] | None = None,
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
    validation_details = by_type["validation_passed"].get("details")
    if not isinstance(validation_details, dict):
        raise RuntimeError(
            f"execution {execution_id} validation verdict has no telemetry details"
        )
    required_validation_details = {
        "collision_workers_requested",
        "collision_workers_effective",
        "collision_workers_used",
        "collision_parallel_fallback_reason",
        "collision_ordered_contact_summary_used",
        "collision_validation_wall_ns",
    }
    if not required_validation_details.issubset(validation_details):
        raise RuntimeError(
            f"execution {execution_id} validation telemetry is incomplete"
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
    if expected_scene_revision is not None and bound_revision != expected_scene_revision:
        raise RuntimeError(
            f"execution {execution_id} did not consume the applied scene revision"
        )
    if expected_policy_revision is not None and request.get("contact_policy_id"):
        raise RuntimeError(
            f"execution {execution_id} selected an explicit contact policy instead "
            "of the configured default"
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
            **(
                {
                    "configured_policy_revision": copy.deepcopy(
                        expected_policy_revision
                    )
                }
                if expected_policy_revision is not None
                else {}
            ),
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
                "collision_workers_requested": validation_details[
                    "collision_workers_requested"
                ],
                "collision_workers_effective": validation_details[
                    "collision_workers_effective"
                ],
                "collision_workers_used": validation_details[
                    "collision_workers_used"
                ],
                "collision_parallel_fallback_reason": validation_details[
                    "collision_parallel_fallback_reason"
                ],
                "collision_ordered_contact_summary_used": validation_details[
                    "collision_ordered_contact_summary_used"
                ],
                "collision_validation_wall_ns": validation_details[
                    "collision_validation_wall_ns"
                ],
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
    cache_control: dict[str, Any],
    kernel_metrics_before: dict[str, Any],
    kernel_metrics_after: dict[str, Any],
    controller_progress_evidence: dict[str, Any],
    support_contact_evidence: dict[str, Any] | None,
    mechanism_replay_input: dict[str, Any] | None,
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
            "qualification_cache_control": cache_control,
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
            "kernel_runtime_metrics": {
                "before": kernel_metrics_before,
                "after": kernel_metrics_after,
                "counter_deltas": _kernel_runtime_counter_deltas(
                    kernel_metrics_before, kernel_metrics_after
                ),
            },
            "controller_progress_latency": controller_progress_evidence,
            "step18_support_contact": support_contact_evidence,
            "mechanism_replay_input": mechanism_replay_input,
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


def _required_artifact_provenance(artifact_root: Path) -> dict[str, Any]:
    program = artifact_root / _ISAAC_PROGRAM_RELATIVE
    deployment = artifact_root / _ISAAC_DEPLOYMENT_RELATIVE
    return {
        "program": {
            "name": _ISAAC_PROGRAM_NAME,
            "path": str(_ISAAC_PROGRAM_RELATIVE),
            "sha256": hashlib.sha256(program.read_bytes()).hexdigest(),
        },
        "deployment": {
            "path": str(_ISAAC_DEPLOYMENT_RELATIVE),
            "sha256": hashlib.sha256(deployment.read_bytes()).hexdigest(),
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
        "configured_collision_policy": copy.deepcopy(policy),
    }


def _require_step18_authority_binding(
    observed: dict[str, Any], expected: dict[str, Any], *, phase: str
) -> None:
    if observed.get("revision") != expected.get("revision"):
        raise RuntimeError(f"{phase} scene revision differs from the applied policy")
    if observed.get("configured_collision_policy") != expected.get(
        "configured_collision_policy"
    ):
        raise RuntimeError(
            f"{phase} configured collision policy differs from the applied policy"
        )


def _source_and_environment(
    inventory: dict[str, Any],
    *,
    stratum: str,
    cpu_set: list[int],
    artifact_provenance: dict[str, Any],
    command: str,
    expected_source_fingerprint_sha256: str | None,
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
        "manyforge_worktree_delta_sha256": source_inventory[
            "manyforge_worktree_delta_sha256"
        ],
        "manyforge_source_fingerprint_sha256": source_inventory[
            "manyforge_source_fingerprint_sha256"
        ],
        "manyforge_source_identity": source_inventory[
            "manyforge_source_identity"
        ],
        "manyforge_source_identity_mode": (
            "frozen_source_fingerprint_sha256"
            if source_inventory["manyforge_dirty"]
            and expected_source_fingerprint_sha256 is not None
            and source_inventory["manyforge_source_fingerprint_sha256"]
            == expected_source_fingerprint_sha256
            else "clean_git_commit"
            if not source_inventory["manyforge_dirty"]
            else "dirty_preflight"
        ),
        "manyforge_expected_source_fingerprint_sha256": (
            expected_source_fingerprint_sha256
        ),
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
        "artifact_reload": artifact_provenance.get("artifact_reload"),
        "matched_input_sha256": artifact_provenance.get(
            "matched_input_sha256"
        ),
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
    dirty = bool(inventory["source"]["manyforge_dirty"])
    actual_source_fingerprint_sha256 = str(
        inventory["source"]["manyforge_source_fingerprint_sha256"]
    )
    if args.expected_source_fingerprint_sha256 is not None:
        if (
            re.fullmatch(
                r"[0-9a-f]{64}", args.expected_source_fingerprint_sha256
            )
            is None
        ):
            raise RuntimeError("expected source identity is not a SHA-256 digest")
        if (
            actual_source_fingerprint_sha256
            != args.expected_source_fingerprint_sha256
        ):
            raise RuntimeError(
                "ManyForge source identity differs from the frozen candidate: "
                f"{actual_source_fingerprint_sha256} != "
                f"{args.expected_source_fingerprint_sha256}"
            )
    if (
        dirty
        and args.expected_source_fingerprint_sha256 is None
        and not args.allow_dirty_source
    ):
        raise RuntimeError(
            f"formal {args.phase} evidence requires either clean source or an exact "
            "--expected-source-fingerprint-sha256 frozen-candidate identity; use "
            "--allow-dirty-source only for an explicitly labeled preflight"
        )
    artifact_root = args.artifact_root.resolve()
    program_path = artifact_root / _ISAAC_PROGRAM_RELATIVE
    deployment_path = artifact_root / _ISAAC_DEPLOYMENT_RELATIVE
    artifact_reload = _reload_matched_artifacts(
        base_url,
        program_path=program_path,
        deployment_path=deployment_path,
    )
    artifact_provenance = require_loaded_artifacts_match(
        base_url,
        program_path=program_path,
        deployment_path=deployment_path,
        expected_program_name=_ISAAC_PROGRAM_NAME,
    )
    if (
        artifact_reload["response_payload_sha256"]
        != artifact_provenance["program"]["loaded_payload_sha256"]
    ):
        raise RuntimeError(
            "program-load response differs from the saved disk-backed program payload"
        )
    artifact_provenance["artifact_reload"] = artifact_reload
    support_contact_evidence = None
    if args.support_contact_mode != "as_loaded":
        support_contact_evidence = _apply_step18_support_contact_mode(
            base_url,
            mode=args.support_contact_mode,
            bounded_penetration_m=args.max_contact_penetration_m,
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
    kernel_metrics_before = _controller_progress_metrics_snapshot(
        base_url,
        observer_mode=args.controller_progress_observer_mode,
    )
    _require_fresh_kernel_runtime_baseline(kernel_metrics_before)
    controller_progress_tracker = _ControllerProgressSampleTracker(
        kernel_metrics_before,
        observer_mode=args.controller_progress_observer_mode,
    )
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

    cache_control = {
        "mode": "explicit_fresh_planning_samples",
        "scope": "all_trajectories",
        "initial_invalidations": 0,
        "post_sample_invalidations": 0,
        "production_cache_configuration_changed": False,
        "production_cache_contents_intentionally_invalidated": True,
    }

    def invalidate_qualification_cache() -> None:
        response = _request_json(
            base_url,
            "/api/debug/qualification/planning-cache/invalidate",
            method="POST",
            payload={"generation": generation},
        )
        if (
            response.get("invalidated") is not True
            or response.get("scope") != "all_trajectories"
        ):
            raise RuntimeError("qualification cache invalidation was not confirmed")

    sampler = _ProcessCpuSampler(args.composer_pid, args.cpu_sample_period_s)
    sampler_started = False
    cycle_started = False
    captured_events: list[dict[str, Any]] = []
    stopped_capture: dict[str, Any] | None = None
    mechanism_snapshot: dict[str, Any] | None = (
        {} if args.capture_mechanism_replay_input else None
    )
    try:
        invalidate_qualification_cache()
        cache_control["initial_invalidations"] = 1
        reset_and_start_simulation(base_url, 30.0)
        time.sleep(args.simulation_settle_s)
        authority_before = _scene_authority_identity(
            _request_json(
                base_url,
                "/api/debug/qualification/scene-authority-snapshot",
            )
        )
        if support_contact_evidence is not None:
            _require_step18_authority_binding(
                authority_before,
                support_contact_evidence["scene_authority_after_apply"],
                phase="pre-run",
            )
        sampler.start()
        sampler_started = True
        _request_json(base_url, "/api/cycle/run", method="POST", payload={})
        cycle_started = True
        captured_events, cursor, post_sample_invalidations = _wait_for_successes(
            base_url,
            cursor=cursor,
            sequence_tracker=sequence_tracker,
            required_successes=args.successful_observations,
            timeout_s=args.timeout_s,
            viewer_stratum=args.viewer_stratum,
            focused_attested=args.operator_attests_viewer_focused,
            poll_period_s=args.poll_period_s,
            prepare_next_fresh_plan=invalidate_qualification_cache,
            controller_progress_tracker=controller_progress_tracker,
            controller_progress_observer_mode=(
                args.controller_progress_observer_mode
            ),
            mechanism_snapshot_out=mechanism_snapshot,
        )
        cache_control["post_sample_invalidations"] = post_sample_invalidations
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
        if support_contact_evidence is not None:
            _require_step18_authority_binding(
                authority_after,
                support_contact_evidence["scene_authority_after_apply"],
                phase="post-run",
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
            authority_before["revision"],
            authority_before["configured_collision_policy"]["revision"],
        )
        for execution_id in selected_ids
    ]
    mechanism_replay_input = None
    if args.capture_mechanism_replay_input:
        if not mechanism_snapshot:
            raise RuntimeError(
                "live observation did not retain an exact drop-planning scene snapshot"
            )
        mechanism_execution_id = int(mechanism_snapshot["execution_id"])
        if mechanism_execution_id not in selected_ids:
            raise RuntimeError(
                "mechanism replay input is outside the selected live population"
            )
        mechanism_record = paired[mechanism_execution_id]
        action_span = (mechanism_record.get("request") or {}).get(
            "action_span"
        ) or {}
        mechanism_run_id = str(action_span.get("program_run_id") or "")
        if not mechanism_run_id:
            raise RuntimeError(
                "mechanism replay planner record has no program run identity"
            )
        model_manifest = _request_json(base_url, "/api/scene/urdf/manifest")
        mechanism_replay_input = _capture_payload(
            program=_read_yaml(program_path),
            visual_scene_state=mechanism_snapshot["scene_state"],
            authority_snapshot=mechanism_snapshot["authority_snapshot"],
            planning_record=mechanism_record,
            run_id=mechanism_run_id,
            planning_execution_id=mechanism_execution_id,
            program_path=program_path,
            deployment_path=deployment_path,
            repo_root=args.artifact_root.resolve(),
            model_manifest=model_manifest,
        )
        mechanism_sample = next(
            sample
            for sample in samples
            if int(sample["execution_id"]) == mechanism_execution_id
        )
        if (
            mechanism_replay_input["planning_result"]["trajectory_sha256"]
            != mechanism_sample["result"]["trajectory_sha256"]
        ):
            raise RuntimeError(
                "mechanism replay trajectory differs from its selected live sample"
            )
    failed_observations = _failed_drop_observations(
        captured_events, through_execution_id=selected_ids[-1]
    )
    pipeline_after = _pipeline_status(base_url)
    kernel_metrics_after = _controller_progress_metrics_snapshot(
        base_url,
        observer_mode=args.controller_progress_observer_mode,
    )
    controller_progress_evidence = controller_progress_tracker.finalize(
        kernel_metrics_after
    )
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
    if require_loaded_artifacts_match(
        base_url,
        program_path=program_path,
        deployment_path=deployment_path,
        expected_program_name=_ISAAC_PROGRAM_NAME,
        allow_runtime_program_override=(
            args.support_contact_mode != "as_loaded"
        ),
    ) != artifact_provenance:
        raise RuntimeError("loaded program/deployment provenance changed during the run")

    source, environment = _source_and_environment(
        inventory,
        stratum=args.viewer_stratum,
        cpu_set=sorted(os.sched_getaffinity(0)),
        artifact_provenance=artifact_provenance,
        command=args.command,
        expected_source_fingerprint_sha256=(
            args.expected_source_fingerprint_sha256
        ),
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
        cache_control=cache_control,
        kernel_metrics_before=kernel_metrics_before,
        kernel_metrics_after=kernel_metrics_after,
        controller_progress_evidence=controller_progress_evidence,
        support_contact_evidence=support_contact_evidence,
        mechanism_replay_input=mechanism_replay_input,
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
        artifact_provenance=_required_artifact_provenance(
            args.artifact_root.resolve()
        ),
        command=args.command,
        expected_source_fingerprint_sha256=(
            args.expected_source_fingerprint_sha256
        ),
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
                    "step18_support_contact_mode": args.support_contact_mode,
                    "step18_max_contact_penetration_m": (
                        args.max_contact_penetration_m
                    ),
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
                    "runtime_failure_evidence": _failure_runtime_evidence(
                        args.composer_url.rstrip("/")
                    ),
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
    parser.add_argument(
        "--artifact-root",
        type=Path,
        default=REPO_ROOT,
        help=(
            "root of the program/deployment artifacts loaded by Composer; this "
            "may differ from the qualification-tool root for pristine baselines"
        ),
    )
    parser.add_argument("--composer-url", default="http://127.0.0.1:9000")
    parser.add_argument(
        "--viewer-stratum", choices=("disconnected", "focused"), required=True
    )
    parser.add_argument(
        "--measurement-mode",
        choices=("primary", "collision_probe_attribution"),
        default="primary",
    )
    parser.add_argument(
        "--support-contact-mode",
        choices=("as_loaded", *sorted(_STEP18_LIVE_MODES)),
        default="as_loaded",
        help=(
            "apply one in-memory Step 18 support-contact arm before collection; "
            "the saved program artifact remains unchanged"
        ),
    )
    parser.add_argument(
        "--max-contact-penetration-m",
        type=float,
        default=_STEP18_BOUNDED_PENETRATION_M,
        help="bounded_penetration threshold for the Step 18 live arm",
    )
    parser.add_argument("--operator-attests-viewer-focused", action="store_true")
    parser.add_argument("--successful-observations", type=int, default=30)
    parser.add_argument("--capture-capacity", type=int, default=1024)
    parser.add_argument("--capture-mechanism-replay-input", action="store_true")
    parser.add_argument("--composer-pid", type=int, default=1)
    parser.add_argument("--cpu-sample-period-s", type=float, default=0.005)
    parser.add_argument("--poll-period-s", type=float, default=0.2)
    parser.add_argument(
        "--controller-progress-observer-mode",
        choices=("coherent_native", "quiescent_repeated"),
        default="coherent_native",
        help=(
            "use collector-side repeated quiescent snapshots for the pristine "
            "unpatched baseline, or the candidate's coherent native observer"
        ),
    )
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
    parser.add_argument(
        "--expected-source-fingerprint-sha256",
        help=(
            "accept a dirty formal candidate only when its versioned commit/tree/"
            "delta source fingerprint matches this SHA-256 before and after"
        ),
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
    if (
        not math.isfinite(args.max_contact_penetration_m)
        or args.max_contact_penetration_m < 0.0
    ):
        parser.error("--max-contact-penetration-m must be finite and non-negative")
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
