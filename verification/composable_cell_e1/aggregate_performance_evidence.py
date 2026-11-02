#!/usr/bin/env python3
"""Aggregate comparable E1 benchmark batches into one compact retained record."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import pathlib
import re
import statistics
import sys
from collections.abc import Callable, Sequence
from typing import Any


_OFFLINE_SCHEMA = "manyforge.e1.roboplan_offline_benchmark.v1"
_WORKER_SCHEMA = "manyforge.e1.candidate_worker_result.v1"
_RESULT_SCHEMA = "manyforge.e1.comparable_performance_evidence.v1"
_MATERIAL_PERCENT = 10.0
_NOISE_MULTIPLIER = 2.0
_MATERIAL_MEMORY_BYTES = 16 * 1024 * 1024
_WORKLOADS = (
    "scene_construction",
    "collision_query",
    "rrt_joint",
    "simple_ik_pose",
    "toppra_retime",
    "cartesian_line",
)
_THREAD_POLICY = {
    "OMP_NUM_THREADS": "1",
    "OPENBLAS_NUM_THREADS": "1",
    "MKL_NUM_THREADS": "1",
    "VECLIB_MAXIMUM_THREADS": "1",
    "NUMEXPR_NUM_THREADS": "1",
}
_SHA256_PATTERN = re.compile(r"[0-9a-f]{64}")
_SHA256_IDENTITY_PATTERN = re.compile(r"sha256:[0-9a-f]{64}")
_AMD64_WORKER_BOUNDS = {
    "start_and_native_warm_ms_max": 1000.0,
    "first_real_rrt_ms_max": 50.0,
    "warmed_real_rrt_p50_ms_max": 30.0,
    "warmed_real_rrt_p95_ms_max": 75.0,
    "warmed_real_rrt_p99_ms_max": 100.0,
    "warmed_real_rrt_throughput_hz_min": 20.0,
    "cancel_to_kill_ms_max": 100.0,
    "deadline_to_kill_ms_max": 100.0,
    "replacement_ms_max": 1000.0,
    "worker_rss_bytes_max": 256 * 1024 * 1024,
    "worker_peak_rss_bytes_max": 256 * 1024 * 1024,
}


def _load(paths: Sequence[pathlib.Path], schema: str) -> list[dict[str, Any]]:
    if len(paths) < 5:
        raise ValueError("at least five independent batches are required")
    rows: list[dict[str, Any]] = []
    for path in paths:
        raw = path.read_bytes()
        value = json.loads(raw)
        if type(value) is not dict or value.get("schema") != schema:
            raise ValueError(f"{path}: schema mismatch")
        value["_evidence_file"] = {
            "name": path.name,
            "sha256": hashlib.sha256(raw).hexdigest(),
        }
        rows.append(value)
    return rows


def _uniform(rows: Sequence[dict[str, Any]], key: str) -> Any:
    first = rows[0].get(key)
    if any(row.get(key) != first for row in rows[1:]):
        raise ValueError(f"{key} changes within one benchmark cohort")
    return first


def _offline_identity(row: dict[str, Any]) -> dict[str, str]:
    identity = row.get("source_identity")
    if type(identity) is not dict:
        raise ValueError("offline source identity is missing")
    normalized = {"label": str(row.get("label", "")), **identity}
    _validate_identity(normalized)
    return normalized


def _worker_identity(row: dict[str, Any]) -> dict[str, str]:
    identity = row.get("evidence_identity")
    if type(identity) is not dict:
        raise ValueError("worker evidence identity is missing")
    normalized = dict(identity)
    normalized["commit"] = normalized.pop("source_commit", "")
    normalized["tree"] = normalized.pop("source_tree", "")
    _validate_identity(normalized)
    return normalized


def _validate_identity(identity: dict[str, Any]) -> None:
    required = {
        "label",
        "commit",
        "tree",
        "patch_set_sha256",
        "dependency_closure_sha256",
        "build_identity_sha256",
        "build_type",
        "compiler_id",
    }
    if set(identity) != required or not all(
        type(identity.get(key)) is str and bool(identity[key]) for key in required
    ):
        raise ValueError("source/build identity shape is invalid")
    if re.fullmatch(r"[0-9a-f]{40}", identity["commit"]) is None:
        raise ValueError("source commit identity is invalid")
    if re.fullmatch(r"[0-9a-f]{40}", identity["tree"]) is None:
        raise ValueError("source tree identity is invalid")
    for key in (
        "patch_set_sha256",
        "dependency_closure_sha256",
        "build_identity_sha256",
    ):
        if _SHA256_PATTERN.fullmatch(identity[key]) is None:
            raise ValueError(f"{key} is invalid")


def _positive(value: object, label: str, *, allow_zero: bool = False) -> float:
    if type(value) not in {int, float} or not math.isfinite(float(value)):
        raise ValueError(f"{label} is not finite")
    selected = float(value)
    if selected < 0.0 or (selected == 0.0 and not allow_zero):
        raise ValueError(f"{label} is outside its finite positive range")
    return selected


def _validate_runtime(value: object, label: str) -> dict[str, Any]:
    if (
        type(value) is not dict
        or set(value) != {"architecture", "python", "executable", "cpu_affinity"}
        or not all(
            type(value.get(key)) is str and bool(value[key])
            for key in ("architecture", "python", "executable")
        )
        or not pathlib.PurePath(str(value["executable"])).is_absolute()
        or type(value.get("cpu_affinity")) is not list
        or not value["cpu_affinity"]
        or any(type(cpu) is not int or cpu < 0 for cpu in value["cpu_affinity"])
        or value["cpu_affinity"] != sorted(set(value["cpu_affinity"]))
    ):
        raise ValueError(f"{label} runtime identity is invalid")
    return value


def _validate_native_modules(
    value: object,
    label: str,
    *,
    required_names: set[str],
) -> dict[str, dict[str, str]]:
    if type(value) is not dict or set(value) != required_names:
        raise ValueError(f"{label} native module set is invalid")
    for name, identity in value.items():
        if (
            type(identity) is not dict
            or set(identity) != {"path", "sha256"}
            or type(identity.get("path")) is not str
            or not pathlib.PurePath(identity["path"]).is_absolute()
            or type(identity.get("sha256")) is not str
            or _SHA256_PATTERN.fullmatch(identity["sha256"]) is None
        ):
            raise ValueError(f"{label} native module identity is invalid: {name}")
    return value


def _validate_latency(value: object, label: str, *, suffix: str) -> None:
    keys = {
        f"p50{suffix}",
        f"p95{suffix}",
        f"p99{suffix}",
        f"mean{suffix}",
        f"max{suffix}",
    }
    if type(value) is not dict or set(value) != keys:
        raise ValueError(f"{label} latency summary is invalid")
    selected = {key: _positive(value[key], f"{label}.{key}") for key in keys}
    if (
        not (
            selected[f"p50{suffix}"]
            <= selected[f"p95{suffix}"]
            <= selected[f"p99{suffix}"]
            <= selected[f"max{suffix}"]
        )
        or selected[f"mean{suffix}"] > selected[f"max{suffix}"]
    ):
        raise ValueError(f"{label} percentile ordering is invalid")


def _validate_memory(value: object, label: str) -> None:
    if type(value) is not dict or set(value) != {"rss_bytes", "peak_rss_bytes"}:
        raise ValueError(f"{label} memory summary is invalid")
    _positive(value["rss_bytes"], f"{label}.rss_bytes")
    _positive(value["peak_rss_bytes"], f"{label}.peak_rss_bytes")


def _validate_offline_row(row: dict[str, Any]) -> None:
    _offline_identity(row)
    _validate_runtime(row.get("runtime_identity"), "offline")
    if row.get("thread_policy") != _THREAD_POLICY:
        raise ValueError(
            "offline thread policy is not the frozen single-thread profile"
        )
    _validate_native_modules(
        row.get("native_modules"),
        "offline",
        required_names={"core", "rrt", "simple_ik", "toppra", "cartesian"},
    )
    _positive(row.get("first_scene_ms"), "offline.first_scene_ms")
    _validate_memory(row.get("memory_before"), "offline.memory_before")
    _validate_memory(row.get("memory_after"), "offline.memory_after")
    workloads = row.get("workloads")
    if type(workloads) is not dict or set(workloads) != set(_WORKLOADS):
        raise ValueError("offline workload set is invalid")
    expected_details = {
        "scene_construction": {"joint_count", "collision_at_default"},
        "collision_query": {"query", "collision"},
        "rrt_joint": {"waypoints"},
        "simple_ik_pose": {"position_error_m", "rotation_matrix_error"},
        "toppra_retime": {"samples", "duration_s"},
        "cartesian_line": {
            "samples",
            "duration_s",
            "position_error_m",
            "peak_velocity_ratio",
            "peak_acceleration_ratio",
        },
    }
    for name in _WORKLOADS:
        workload = workloads[name]
        if (
            type(workload) is not dict
            or set(workload)
            != {
                "warmups",
                "iterations",
                "latency",
                "throughput_hz",
                "final_details",
            }
            or type(workload["warmups"]) is not int
            or workload["warmups"] < 0
            or type(workload["iterations"]) is not int
            or workload["iterations"] <= 0
        ):
            raise ValueError(f"offline workload shape is invalid: {name}")
        _validate_latency(workload["latency"], f"offline.{name}", suffix="_ms")
        _positive(workload["throughput_hz"], f"offline.{name}.throughput_hz")
        details = workload["final_details"]
        if type(details) is not dict or set(details) != expected_details[name]:
            raise ValueError(f"offline workload details are invalid: {name}")
        if name == "scene_construction":
            if (
                details["joint_count"] != 6
                or type(details["collision_at_default"]) is not bool
            ):
                raise ValueError("scene-construction self-check is invalid")
        elif name == "collision_query":
            if (
                type(details["query"]) is not int
                or type(details["collision"]) is not bool
            ):
                raise ValueError("collision-query self-check is invalid")
        elif name == "rrt_joint":
            if type(details["waypoints"]) is not int or details["waypoints"] < 2:
                raise ValueError("RRT self-check is invalid")
        else:
            for key, value in details.items():
                if key == "samples":
                    if type(value) is not int or value < 2:
                        raise ValueError(f"{name} sample self-check is invalid")
                else:
                    _positive(value, f"offline.{name}.{key}", allow_zero=True)


def _validate_success_result(
    row: dict[str, Any],
    name: str,
    *,
    worker_generation: int,
    worker_pid: int,
) -> None:
    result = row.get(name)
    bound = row["bound_projection"]
    shape = result.get("trajectory_shape") if type(result) is dict else None
    if (
        type(result) is not dict
        or result.get("ok") is not True
        or result.get("code") != "ok"
        or result.get("reason") != "candidate_ready"
        or result.get("progress") != ["admitted", "dispatched", "running", "terminal"]
        or result.get("worker_generation") != worker_generation
        or result.get("worker_pid") != worker_pid
        or result.get("projection_generation") != bound["generation"]
        or result.get("projection_digest") != bound["digest"]
        or result.get("result_transport") != "sealed_memfd_v1"
        or type(shape) is not dict
        or shape.get("dof") != 6
        or type(shape.get("samples")) is not int
        or shape["samples"] < 2
        or result.get("candidate_digest")
        != (
            f"candidate:{worker_generation}:{result.get('request_id')}:"
            f"{bound['generation']}"
        )
    ):
        raise ValueError(f"worker {name} recovery result is invalid")


def _validate_worker_row(row: dict[str, Any]) -> None:
    _worker_identity(row)
    if type(row.get("iterations")) is not int or row["iterations"] < 10:
        raise ValueError("worker iteration count is invalid")
    _validate_latency(row.get("warmed_latency_ms"), "worker.warmed", suffix="")
    runtime = _validate_runtime(row.get("worker_runtime_identity"), "worker")
    if runtime["architecture"] != "x86_64":
        raise ValueError("worker is outside the frozen AMD64 profile")
    if row.get("worker_thread_policy") != _THREAD_POLICY:
        raise ValueError("worker thread policy is not the frozen single-thread profile")
    _validate_native_modules(
        row.get("worker_native_modules"),
        "worker",
        required_names={"core", "rrt"},
    )
    bound = row.get("bound_projection")
    if (
        type(bound) is not dict
        or set(bound) != {"generation", "digest"}
        or bound["generation"] != 1000
        or type(bound["digest"]) is not str
        or _SHA256_IDENTITY_PATTERN.fullmatch(bound["digest"]) is None
    ):
        raise ValueError("worker caller-bound projection identity is invalid")
    representative = row.get("representative_result")
    if (
        type(representative) is not dict
        or representative.get("transport") != "sealed_memfd_v1"
        or type(representative.get("candidate_digest")) is not str
        or int(representative.get("samples", 0)) < 2
        or representative.get("dof") != 6
        or any(
            type(representative.get(key)) is not list
            or len(representative[key]) != 6
            or any(
                type(value) not in {int, float} or not math.isfinite(value)
                for value in representative[key]
            )
            for key in ("start", "goal")
        )
    ):
        raise ValueError("worker representative sealed result is invalid")
    if (
        row.get("dispatch_count") != row["iterations"] + 5
        or row.get("replacement_count") != 2
    ):
        raise ValueError("worker dispatch/replacement accounting is invalid")
    initial = row.get("initial_worker")
    final = row.get("final_worker")
    if (
        type(initial) is not dict
        or initial.get("generation") != 1
        or type(initial.get("pid")) is not int
        or type(final) is not dict
        or final.get("generation") != 3
        or type(final.get("pid")) is not int
    ):
        raise ValueError("worker generation identity is invalid")
    _positive(initial.get("start_and_native_warm_ms"), "worker.start")
    _positive(initial.get("first_request_ms"), "worker.first_request")
    _validate_memory(initial.get("memory_before_requests"), "worker.memory_before")
    _validate_memory(initial.get("memory_after_requests"), "worker.memory_after")
    _validate_memory(final.get("memory"), "worker.final_memory")
    limits = row.get("worker_limits")
    if (
        type(limits) is not dict
        or set(limits)
        != {
            "address_space_bytes",
            "file_bytes",
            "cpu_seconds",
            "core_bytes",
        }
        or limits["core_bytes"] != 0
        or any(
            type(limits[key]) is not int or limits[key] <= 0
            for key in ("address_space_bytes", "file_bytes", "cpu_seconds")
        )
    ):
        raise ValueError("worker resource-limit attestation is invalid")
    cancel = row.get("cancel")
    deadline = row.get("deadline")
    if (
        type(cancel) is not dict
        or cancel.get("ok") is not False
        or cancel.get("code") != "planner_cancelled"
        or cancel.get("reason") != "cancel_killed_worker"
        or cancel.get("progress")
        != [
            "admitted",
            "dispatched",
            "running",
            "cancel_requested",
            "worker_retired",
            "replacement_ready",
            "terminal",
        ]
        or cancel.get("worker_generation") != 1
        or cancel.get("worker_pid") != initial["pid"]
        or cancel.get("replacement_generation") != 2
        or type(cancel.get("replacement_pid")) is not int
        or cancel["replacement_pid"] == initial["pid"]
        or cancel.get("projection_generation") != bound["generation"]
        or type(deadline) is not dict
        or deadline.get("ok") is not False
        or deadline.get("code") != "planner_deadline_exceeded"
        or deadline.get("reason") != "deadline_killed_worker"
        or deadline.get("progress")
        != [
            "admitted",
            "dispatched",
            "running",
            "deadline_reached",
            "worker_retired",
            "replacement_ready",
            "terminal",
        ]
        or deadline.get("worker_generation") != 2
        or deadline.get("worker_pid") != cancel["replacement_pid"]
        or deadline.get("replacement_generation") != 3
        or deadline.get("replacement_pid") != final["pid"]
        or deadline.get("projection_generation") != bound["generation"]
    ):
        raise ValueError("worker cancel/deadline generation outcome is invalid")
    for name, outcome in (("cancel", cancel), ("deadline", deadline)):
        if any(
            outcome.get(key) is not None
            for key in (
                "candidate_digest",
                "projection_digest",
                "result_transport",
                "trajectory_positions",
            )
        ):
            raise ValueError(f"worker {name} published a result after termination")
        _positive(outcome.get("kill_ms"), f"worker.{name}.kill_ms")
        _positive(outcome.get("replacement_ms"), f"worker.{name}.replacement_ms")
        _positive(outcome.get("wall_ms"), f"worker.{name}.wall_ms")
    _validate_success_result(
        row,
        "post_cancel_success",
        worker_generation=2,
        worker_pid=cancel["replacement_pid"],
    )
    _validate_success_result(
        row,
        "post_deadline_success",
        worker_generation=3,
        worker_pid=final["pid"],
    )
    request_ids = [
        cancel.get("request_id"),
        row["post_cancel_success"].get("request_id"),
        deadline.get("request_id"),
        row["post_deadline_success"].get("request_id"),
    ]
    if any(
        type(request_id) is not int for request_id in request_ids
    ) or request_ids != list(range(request_ids[0], request_ids[0] + 4)):
        raise ValueError("worker recovery request sequence is invalid")


def _practical_bound_observations(rows: Sequence[dict[str, Any]]) -> dict[str, float]:
    observations = {
        "start_and_native_warm_ms_max": max(
            row["initial_worker"]["start_and_native_warm_ms"] for row in rows
        ),
        "first_real_rrt_ms_max": max(
            row["initial_worker"]["first_request_ms"] for row in rows
        ),
        "warmed_real_rrt_p50_ms_max": max(
            row["warmed_latency_ms"]["p50"] for row in rows
        ),
        "warmed_real_rrt_p95_ms_max": max(
            row["warmed_latency_ms"]["p95"] for row in rows
        ),
        "warmed_real_rrt_p99_ms_max": max(
            row["warmed_latency_ms"]["p99"] for row in rows
        ),
        "warmed_real_rrt_throughput_hz_min": min(
            1000.0 / row["warmed_latency_ms"]["mean"] for row in rows
        ),
        "cancel_to_kill_ms_max": max(row["cancel"]["kill_ms"] for row in rows),
        "deadline_to_kill_ms_max": max(row["deadline"]["kill_ms"] for row in rows),
        "replacement_ms_max": max(
            max(row["cancel"]["replacement_ms"], row["deadline"]["replacement_ms"])
            for row in rows
        ),
        "worker_rss_bytes_max": max(
            row["initial_worker"]["memory_after_requests"]["rss_bytes"] for row in rows
        ),
        "worker_peak_rss_bytes_max": max(
            row["final_worker"]["memory"]["peak_rss_bytes"] for row in rows
        ),
    }
    for key, bound in _AMD64_WORKER_BOUNDS.items():
        actual = float(observations[key])
        if key.endswith("_min"):
            if actual < bound:
                raise ValueError(f"worker practical lower bound failed: {key}")
        elif actual > bound:
            raise ValueError(f"worker practical upper bound failed: {key}")
    return {key: float(value) for key, value in observations.items()}


def _uniform_identity(
    rows: Sequence[dict[str, Any]],
    extract: Callable[[dict[str, Any]], dict[str, str]],
) -> dict[str, str]:
    first = extract(rows[0])
    if not all(first.values()) or any(extract(row) != first for row in rows[1:]):
        raise ValueError("source/build identity changes within one benchmark cohort")
    return first


def _median(
    rows: Sequence[dict[str, Any]], extract: Callable[[dict[str, Any]], float]
) -> float:
    return statistics.median(float(extract(row)) for row in rows)


def _comparison(control: float, candidate: float) -> dict[str, float]:
    return {
        "control_median": control,
        "candidate_median": candidate,
        "percent_delta": ((candidate - control) / control) * 100.0,
    }


def _metric(
    control: Sequence[dict[str, Any]],
    candidate: Sequence[dict[str, Any]],
    extract: Callable[[dict[str, Any]], float],
) -> dict[str, float]:
    control_values = [float(extract(row)) for row in control]
    candidate_values = [float(extract(row)) for row in candidate]
    if any(value == 0.0 for value in control_values):
        raise ValueError("paired performance control value is zero")
    paired_percent_deltas = [
        ((new - old) / old) * 100.0
        for old, new in zip(control_values, candidate_values, strict=True)
    ]
    paired_median = statistics.median(paired_percent_deltas)
    paired_mad = statistics.median(
        abs(value - paired_median) for value in paired_percent_deltas
    )
    return {
        **_comparison(
            statistics.median(control_values),
            statistics.median(candidate_values),
        ),
        "paired_percent_delta_median": paired_median,
        "paired_percent_delta_mad": paired_mad,
    }


def _relative_metric(
    control: Sequence[dict[str, Any]],
    candidate: Sequence[dict[str, Any]],
    extract: Callable[[dict[str, Any]], float],
    *,
    lower_is_better: bool,
) -> dict[str, float | bool]:
    result = _metric(control, candidate, extract)
    signed_regression = float(result["paired_percent_delta_median"])
    if not lower_is_better:
        signed_regression = -signed_regression
    regression_percent = max(0.0, signed_regression)
    result["regression_percent"] = regression_percent
    result["material_regression"] = (
        regression_percent > _MATERIAL_PERCENT
        and regression_percent
        > _NOISE_MULTIPLIER * float(result["paired_percent_delta_mad"])
    )
    return result


def _memory_metric(
    control: Sequence[dict[str, Any]],
    candidate: Sequence[dict[str, Any]],
    extract: Callable[[dict[str, Any]], float],
) -> dict[str, float | bool]:
    result = _metric(control, candidate, extract)
    absolute_delta = float(result["candidate_median"]) - float(result["control_median"])
    result["absolute_delta_bytes"] = absolute_delta
    result["material_regression"] = (
        absolute_delta > _MATERIAL_MEMORY_BYTES
        and float(result["paired_percent_delta_median"]) > _MATERIAL_PERCENT
    )
    return result


def _material_paths(prefix: str, value: Any) -> list[str]:
    if type(value) is not dict:
        return []
    if value.get("material_regression") is True:
        return [prefix]
    paths: list[str] = []
    for key, child in value.items():
        if type(child) is dict:
            child_prefix = f"{prefix}.{key}" if prefix else key
            paths.extend(_material_paths(child_prefix, child))
    return paths


def aggregate(
    offline_control: Sequence[pathlib.Path],
    offline_candidate: Sequence[pathlib.Path],
    worker_control: Sequence[pathlib.Path],
    worker_candidate: Sequence[pathlib.Path],
) -> dict[str, Any]:
    oc = _load(offline_control, _OFFLINE_SCHEMA)
    on = _load(offline_candidate, _OFFLINE_SCHEMA)
    wc = _load(worker_control, _WORKER_SCHEMA)
    wn = _load(worker_candidate, _WORKER_SCHEMA)
    if len({len(oc), len(on), len(wc), len(wn)}) != 1:
        raise ValueError("control/candidate offline/worker batch counts differ")
    for row in [*oc, *on]:
        _validate_offline_row(row)
    for row in [*wc, *wn]:
        _validate_worker_row(row)

    offline_control_identity = _uniform_identity(oc, _offline_identity)
    offline_candidate_identity = _uniform_identity(on, _offline_identity)
    worker_control_identity = _uniform_identity(wc, _worker_identity)
    worker_candidate_identity = _uniform_identity(wn, _worker_identity)
    for identity in (
        offline_control_identity,
        offline_candidate_identity,
        worker_control_identity,
        worker_candidate_identity,
    ):
        if identity.get("build_type") != "Release":
            raise ValueError("performance cohort is not a Release build")
    if (
        offline_control_identity != worker_control_identity
        or offline_candidate_identity != worker_candidate_identity
    ):
        raise ValueError("offline and worker source/build identities disagree")
    if (
        offline_control_identity["compiler_id"]
        != offline_candidate_identity["compiler_id"]
    ):
        raise ValueError("control and candidate compiler identities differ")
    control_modules = _uniform(oc, "native_modules")
    candidate_modules = _uniform(on, "native_modules")
    control_runtime = _uniform(oc, "runtime_identity")
    candidate_runtime = _uniform(on, "runtime_identity")
    control_worker_runtime = _uniform(wc, "worker_runtime_identity")
    candidate_worker_runtime = _uniform(wn, "worker_runtime_identity")
    control_architecture = control_runtime["architecture"]
    candidate_architecture = candidate_runtime["architecture"]
    if control_architecture != candidate_architecture:
        raise ValueError("control and candidate architectures differ")
    for offline_runtime, worker_runtime in (
        (control_runtime, control_worker_runtime),
        (candidate_runtime, candidate_worker_runtime),
    ):
        if offline_runtime != worker_runtime:
            raise ValueError("offline and actual worker runtime identities disagree")
    if (
        control_runtime["architecture"] != "x86_64"
        or control_runtime["python"] != candidate_runtime["python"]
        or control_runtime["cpu_affinity"] != candidate_runtime["cpu_affinity"]
        or _uniform(oc, "thread_policy") != _THREAD_POLICY
        or _uniform(on, "thread_policy") != _THREAD_POLICY
        or _uniform(wc, "worker_thread_policy") != _THREAD_POLICY
        or _uniform(wn, "worker_thread_policy") != _THREAD_POLICY
    ):
        raise ValueError("control/candidate runtime or thread profile differs")
    control_worker_modules = _uniform(wc, "worker_native_modules")
    candidate_worker_modules = _uniform(wn, "worker_native_modules")
    if control_worker_modules != {
        name: control_modules[name] for name in ("core", "rrt")
    } or candidate_worker_modules != {
        name: candidate_modules[name] for name in ("core", "rrt")
    }:
        raise ValueError(
            "actual worker native modules disagree with offline build identity"
        )
    projection_binding = _uniform(wc, "bound_projection")
    if _uniform(wn, "bound_projection") != projection_binding:
        raise ValueError("control/candidate caller-bound fixture projection differs")
    representative_start = _uniform(wc, "representative_result")["start"]
    representative_goal = _uniform(wc, "representative_result")["goal"]
    if any(
        row["representative_result"]["start"] != representative_start
        or row["representative_result"]["goal"] != representative_goal
        for row in wn
    ):
        raise ValueError("control/candidate worker problem statement differs")

    workload_metrics: dict[str, Any] = {}
    for workload in _WORKLOADS:
        workload_metrics[workload] = {
            percentile: _relative_metric(
                oc,
                on,
                lambda row, name=workload, metric=percentile: row["workloads"][name][
                    "latency"
                ][metric],
                lower_is_better=True,
            )
            for percentile in ("p50_ms", "p95_ms", "p99_ms")
        }
        workload_metrics[workload]["throughput_hz"] = _relative_metric(
            oc,
            on,
            lambda row, name=workload: row["workloads"][name]["throughput_hz"],
            lower_is_better=False,
        )

    worker_metrics = {
        "start_and_native_warm_ms": _relative_metric(
            wc,
            wn,
            lambda row: row["initial_worker"]["start_and_native_warm_ms"],
            lower_is_better=True,
        ),
        "first_real_rrt_ms": _relative_metric(
            wc,
            wn,
            lambda row: row["initial_worker"]["first_request_ms"],
            lower_is_better=True,
        ),
        "warmed_real_rrt_p50_ms": _relative_metric(
            wc,
            wn,
            lambda row: row["warmed_latency_ms"]["p50"],
            lower_is_better=True,
        ),
        "warmed_real_rrt_p95_ms": _relative_metric(
            wc,
            wn,
            lambda row: row["warmed_latency_ms"]["p95"],
            lower_is_better=True,
        ),
        "warmed_real_rrt_p99_ms": _relative_metric(
            wc,
            wn,
            lambda row: row["warmed_latency_ms"]["p99"],
            lower_is_better=True,
        ),
        "warmed_real_rrt_throughput_hz": _relative_metric(
            wc,
            wn,
            lambda row: 1000.0 / row["warmed_latency_ms"]["mean"],
            lower_is_better=False,
        ),
        "worker_rss_bytes": _memory_metric(
            wc,
            wn,
            lambda row: row["initial_worker"]["memory_after_requests"]["rss_bytes"],
        ),
        "worker_peak_rss_bytes": _memory_metric(
            wc,
            wn,
            lambda row: row["final_worker"]["memory"]["peak_rss_bytes"],
        ),
        "cancel_to_kill_ms": _relative_metric(
            wc,
            wn,
            lambda row: row["cancel"]["kill_ms"],
            lower_is_better=True,
        ),
        "cancel_replacement_ms": _relative_metric(
            wc,
            wn,
            lambda row: row["cancel"]["replacement_ms"],
            lower_is_better=True,
        ),
        "deadline_to_kill_ms": _relative_metric(
            wc,
            wn,
            lambda row: row["deadline"]["kill_ms"],
            lower_is_better=True,
        ),
        "deadline_replacement_ms": _relative_metric(
            wc,
            wn,
            lambda row: row["deadline"]["replacement_ms"],
            lower_is_better=True,
        ),
    }

    offline_metrics = {
        "first_scene_ms": _relative_metric(
            oc, on, lambda row: row["first_scene_ms"], lower_is_better=True
        ),
        "rss_bytes": _memory_metric(
            oc, on, lambda row: row["memory_after"]["rss_bytes"]
        ),
        "peak_rss_bytes": _memory_metric(
            oc, on, lambda row: row["memory_after"]["peak_rss_bytes"]
        ),
        "workloads": workload_metrics,
    }
    material_regressions = [
        *_material_paths("offline", offline_metrics),
        *_material_paths("worker", worker_metrics),
    ]
    practical_observations = _practical_bound_observations([*wc, *wn])

    return {
        "schema": _RESULT_SCHEMA,
        "batch_count_per_cohort": len(oc),
        "control_identity": offline_control_identity,
        "candidate_identity": offline_candidate_identity,
        "runtime_architecture": control_architecture,
        "runtime_profile": {
            "python": control_runtime["python"],
            "cpu_affinity": control_runtime["cpu_affinity"],
            "thread_policy": _THREAD_POLICY,
        },
        "worker_fixture_projection": projection_binding,
        "worker_fixture_problem": {
            "start": representative_start,
            "goal": representative_goal,
        },
        "control_native_modules": control_modules,
        "candidate_native_modules": candidate_modules,
        "offline": offline_metrics,
        "worker": worker_metrics,
        "materiality_rule": {
            "paired_cohorts": "argument_order",
            "relative_regression_percent": _MATERIAL_PERCENT,
            "paired_noise_statistic": "median_absolute_deviation_of_paired_percent_deltas",
            "noise_multiplier": _NOISE_MULTIPLIER,
            "memory_absolute_bytes": _MATERIAL_MEMORY_BYTES,
        },
        "material_regressions": material_regressions,
        "practical_qualification_profile": {
            "id": "amd64_release_single_thread_e1_fixture_v1",
            "scope": "E1 fixed UR5 RRT fixture; not a production I6 SLO",
            "bounds": _AMD64_WORKER_BOUNDS,
            "observed": practical_observations,
            "result": "passed",
        },
        "invariants": {
            "release_builds_and_actual_worker_native_module_hashes": "passed",
            "same_architecture_affinity_python_compiler_and_thread_policy": "passed",
            "offline_workload_shapes_and_producer_self_checks": "passed_in_every_batch",
            "sealed_real_rrt_result_transfer": "passed_in_every_batch",
            "caller_bound_projection_generation_and_content_digest": "passed_in_every_batch",
            "cancel_deadline_replacement_recovery": "passed_in_every_batch",
            "absolute_practical_bounds": "passed_in_every_batch",
        },
        "input_files": {
            "offline_control": [row["_evidence_file"] for row in oc],
            "offline_candidate": [row["_evidence_file"] for row in on],
            "worker_control": [row["_evidence_file"] for row in wc],
            "worker_candidate": [row["_evidence_file"] for row in wn],
        },
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--offline-control", type=pathlib.Path, nargs="+", required=True
    )
    parser.add_argument(
        "--offline-candidate", type=pathlib.Path, nargs="+", required=True
    )
    parser.add_argument("--worker-control", type=pathlib.Path, nargs="+", required=True)
    parser.add_argument(
        "--worker-candidate", type=pathlib.Path, nargs="+", required=True
    )
    args = parser.parse_args(argv)
    result = aggregate(
        args.offline_control,
        args.offline_candidate,
        args.worker_control,
        args.worker_candidate,
    )
    json.dump(
        result, sys.stdout, sort_keys=True, separators=(",", ":"), allow_nan=False
    )
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
