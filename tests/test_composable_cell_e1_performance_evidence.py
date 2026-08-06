"""Contract tests for compact, comparable E1 performance evidence."""

from __future__ import annotations

import json
import math
import statistics
from collections.abc import Callable
from pathlib import Path

import pytest

from verification.composable_cell_e1 import aggregate_performance_evidence as evidence


_WORKLOADS = (
    "scene_construction",
    "collision_query",
    "rrt_joint",
    "simple_ik_pose",
    "toppra_retime",
    "cartesian_line",
)

_PATCH0007_EVIDENCE = (
    Path(__file__).parents[1] / "verification/composable_cell_e1/evidence/"
    "roboplan-051-patch0007-collision-reuse-ab.v1.json"
)


def _identity(label: str) -> dict[str, str]:
    digit = "1" if label == "control" else "2"
    return {
        "label": label,
        "commit": digit * 40,
        "tree": ("3" if label == "control" else "4") * 40,
        "patch_set_sha256": digit * 64,
        "dependency_closure_sha256": ("5" if label == "control" else "6") * 64,
        "build_identity_sha256": ("7" if label == "control" else "8") * 64,
        "build_type": "Release",
        "compiler_id": "GNU-12.2.0",
    }


def _native_modules(label: str) -> dict[str, dict[str, str]]:
    names = ("core", "rrt", "simple_ik", "toppra", "cartesian")
    return {
        name: {
            "path": f"/{label}/{name}.so",
            "sha256": f"{index + (1 if label == 'control' else 6):x}" * 64,
        }
        for index, name in enumerate(names)
    }


def _runtime(label: str) -> dict[str, object]:
    return {
        "architecture": "x86_64",
        "python": "3.12.13",
        "executable": f"/{label}/python",
        "cpu_affinity": [0],
    }


def _thread_policy() -> dict[str, str]:
    return {
        "OMP_NUM_THREADS": "1",
        "OPENBLAS_NUM_THREADS": "1",
        "MKL_NUM_THREADS": "1",
        "VECLIB_MAXIMUM_THREADS": "1",
        "NUMEXPR_NUM_THREADS": "1",
    }


def _details(name: str) -> dict[str, object]:
    return {
        "scene_construction": {"joint_count": 6, "collision_at_default": False},
        "collision_query": {"query": 1, "collision": True},
        "rrt_joint": {"waypoints": 20},
        "simple_ik_pose": {"position_error_m": 0.001, "rotation_matrix_error": 0.001},
        "toppra_retime": {"samples": 20, "duration_s": 1.0},
        "cartesian_line": {
            "samples": 20,
            "duration_s": 1.0,
            "position_error_m": 0.001,
            "peak_velocity_ratio": 0.5,
            "peak_acceleration_ratio": 0.5,
        },
    }[name]


def _offline(label: str, value: float) -> dict[str, object]:
    identity = _identity(label)
    return {
        "schema": "manyforge.e1.roboplan_offline_benchmark.v1",
        "label": label,
        "source_identity": {
            key: item for key, item in identity.items() if key != "label"
        },
        "runtime_identity": _runtime(label),
        "thread_policy": _thread_policy(),
        "native_modules": _native_modules(label),
        "first_scene_ms": value,
        "memory_before": {"rss_bytes": value, "peak_rss_bytes": value},
        "memory_after": {"rss_bytes": value, "peak_rss_bytes": value},
        "workloads": {
            name: {
                "warmups": 1,
                "iterations": 10,
                "latency": {
                    "p50_ms": value,
                    "p95_ms": value + 1.0,
                    "p99_ms": value + 2.0,
                    "mean_ms": value + 0.5,
                    "max_ms": value + 3.0,
                },
                "throughput_hz": value,
                "final_details": _details(name),
            }
            for name in _WORKLOADS
        },
    }


def _worker(label: str, value: float) -> dict[str, object]:
    identity = _identity(label)
    worker_identity = dict(identity)
    worker_identity["source_commit"] = worker_identity.pop("commit")
    worker_identity["source_tree"] = worker_identity.pop("tree")
    projection_digest = "sha256:" + "a" * 64
    result = {
        "ok": True,
        "code": "ok",
        "projection_generation": 1000,
        "projection_digest": projection_digest,
        "result_transport": "sealed_memfd_v1",
        "trajectory_shape": {"samples": 20, "dof": 6},
    }
    native_modules = _native_modules(label)
    return {
        "schema": "manyforge.e1.candidate_worker_result.v1",
        "evidence_identity": worker_identity,
        "iterations": 100,
        "worker_runtime_identity": _runtime(label),
        "worker_thread_policy": _thread_policy(),
        "worker_native_modules": {
            name: native_modules[name] for name in ("core", "rrt")
        },
        "initial_worker": {
            "pid": 100,
            "generation": 1,
            "start_and_native_warm_ms": value,
            "first_request_ms": value,
            "memory_before_requests": {"rss_bytes": value, "peak_rss_bytes": value},
            "memory_after_requests": {"rss_bytes": value, "peak_rss_bytes": value},
        },
        "final_worker": {
            "pid": 300,
            "generation": 3,
            "memory": {"rss_bytes": value, "peak_rss_bytes": value},
        },
        "worker_limits": {
            "address_space_bytes": 1024 * 1024 * 1024,
            "file_bytes": 64 * 1024 * 1024,
            "cpu_seconds": 30,
            "core_bytes": 0,
        },
        "bound_projection": {"generation": 1000, "digest": projection_digest},
        "warmed_latency_ms": {
            "p50": value,
            "p95": value + 1.0,
            "p99": value + 2.0,
            "mean": value + 0.5,
            "max": value + 3.0,
        },
        "representative_result": {
            "transport": "sealed_memfd_v1",
            "candidate_digest": "candidate:1:1:1000",
            "samples": 2,
            "dof": 6,
            "start": [0.0] * 6,
            "goal": [1.0] * 6,
        },
        "dispatch_count": 105,
        "replacement_count": 2,
        "cancel": {
            "ok": False,
            "code": "planner_cancelled",
            "reason": "cancel_killed_worker",
            "progress": [
                "admitted",
                "dispatched",
                "running",
                "cancel_requested",
                "worker_retired",
                "replacement_ready",
                "terminal",
            ],
            "request_id": 202,
            "worker_generation": 1,
            "worker_pid": 100,
            "replacement_generation": 2,
            "replacement_pid": 200,
            "projection_generation": 1000,
            "kill_ms": value,
            "replacement_ms": value,
            "wall_ms": value,
        },
        "deadline": {
            "ok": False,
            "code": "planner_deadline_exceeded",
            "reason": "deadline_killed_worker",
            "progress": [
                "admitted",
                "dispatched",
                "running",
                "deadline_reached",
                "worker_retired",
                "replacement_ready",
                "terminal",
            ],
            "request_id": 204,
            "worker_generation": 2,
            "worker_pid": 200,
            "replacement_generation": 3,
            "replacement_pid": 300,
            "projection_generation": 1000,
            "kill_ms": value,
            "replacement_ms": value,
            "wall_ms": value,
        },
        "post_cancel_success": {
            **result,
            "reason": "candidate_ready",
            "progress": ["admitted", "dispatched", "running", "terminal"],
            "request_id": 203,
            "worker_generation": 2,
            "worker_pid": 200,
            "candidate_digest": "candidate:2:203:1000",
        },
        "post_deadline_success": {
            **result,
            "reason": "candidate_ready",
            "progress": ["admitted", "dispatched", "running", "terminal"],
            "request_id": 205,
            "worker_generation": 3,
            "worker_pid": 300,
            "candidate_digest": "candidate:3:205:1000",
        },
    }


def _write_rows(
    root: Path,
    prefix: str,
    make: Callable[[str, float], dict[str, object]],
    label: str,
    base: float,
) -> list[Path]:
    paths: list[Path] = []
    for index in range(5):
        path = root / f"{prefix}-{index}.json"
        path.write_text(
            json.dumps(make(label, base + index)),
            encoding="utf-8",
        )
        paths.append(path)
    return paths


def _cohorts(tmp_path: Path) -> tuple[list[Path], list[Path], list[Path], list[Path]]:
    return (
        _write_rows(tmp_path, "oc", _offline, "control", 10.0),
        _write_rows(tmp_path, "on", _offline, "candidate", 5.0),
        _write_rows(tmp_path, "wc", _worker, "control", 10.0),
        _write_rows(tmp_path, "wn", _worker, "candidate", 5.0),
    )


def _rewrite(path: Path, mutate: Callable[[dict[str, object]], None]) -> None:
    payload = json.loads(path.read_text(encoding="utf-8"))
    mutate(payload)
    path.write_text(json.dumps(payload), encoding="utf-8")


def test_aggregate_accepts_five_exact_comparable_cohorts(tmp_path: Path) -> None:
    offline_control = _write_rows(tmp_path, "oc", _offline, "control", 10.0)
    offline_candidate = _write_rows(tmp_path, "on", _offline, "candidate", 5.0)
    worker_control = _write_rows(tmp_path, "wc", _worker, "control", 10.0)
    worker_candidate = _write_rows(tmp_path, "wn", _worker, "candidate", 5.0)

    result = evidence.aggregate(
        offline_control,
        offline_candidate,
        worker_control,
        worker_candidate,
    )

    assert result["batch_count_per_cohort"] == 5
    assert result["offline"]["first_scene_ms"]["percent_delta"] < 0.0
    assert result["worker"]["warmed_real_rrt_p50_ms"]["percent_delta"] < 0.0
    assert result["invariants"]["sealed_real_rrt_result_transfer"] == (
        "passed_in_every_batch"
    )
    assert "p99_ms" in result["offline"]["workloads"]["rrt_joint"]
    assert "warmed_real_rrt_p99_ms" in result["worker"]
    assert "warmed_real_rrt_throughput_hz" in result["worker"]
    assert "worker_peak_rss_bytes" in result["worker"]
    assert result["practical_qualification_profile"]["result"] == "passed"
    assert result["worker_fixture_projection"] == {
        "generation": 1000,
        "digest": "sha256:" + "a" * 64,
    }
    assert result["worker_fixture_problem"] == {
        "start": [0.0] * 6,
        "goal": [1.0] * 6,
    }


def test_aggregate_flags_consistent_paired_latency_regression(tmp_path: Path) -> None:
    offline_control = _write_rows(tmp_path, "oc", _offline, "control", 10.0)
    offline_candidate = _write_rows(tmp_path, "on", _offline, "candidate", 12.0)
    worker_control = _write_rows(tmp_path, "wc", _worker, "control", 10.0)
    worker_candidate = _write_rows(tmp_path, "wn", _worker, "candidate", 12.0)

    result = evidence.aggregate(
        offline_control,
        offline_candidate,
        worker_control,
        worker_candidate,
    )

    metric = result["offline"]["workloads"]["rrt_joint"]["p50_ms"]
    assert metric["paired_percent_delta_median"] > 10.0
    assert metric["material_regression"] is True
    assert "offline.workloads.rrt_joint.p50_ms" in result["material_regressions"]


def test_aggregate_rejects_worker_binary_identity_drift(tmp_path: Path) -> None:
    offline_control = _write_rows(tmp_path, "oc", _offline, "control", 10.0)
    offline_candidate = _write_rows(tmp_path, "on", _offline, "candidate", 5.0)
    worker_control = _write_rows(tmp_path, "wc", _worker, "control", 10.0)
    worker_candidate = _write_rows(tmp_path, "wn", _worker, "candidate", 5.0)
    payload = json.loads(worker_candidate[-1].read_text(encoding="utf-8"))
    payload["evidence_identity"]["build_identity_sha256"] = "f" * 64
    worker_candidate[-1].write_text(json.dumps(payload), encoding="utf-8")

    with pytest.raises(ValueError, match="identity changes"):
        evidence.aggregate(
            offline_control,
            offline_candidate,
            worker_control,
            worker_candidate,
        )


@pytest.mark.parametrize(
    ("case", "match"),
    [
        ("affinity", "runtime or thread profile differs"),
        ("thread_policy", "frozen single-thread profile"),
        ("projection", "fixture projection differs"),
        ("native_module", "native modules disagree"),
        ("workload_self_check", "RRT self-check"),
        ("missing_p99", "latency summary is invalid"),
        ("recovery_progress", "generation outcome is invalid"),
    ],
)
def test_aggregate_rejects_untrustworthy_comparability_inputs(
    tmp_path: Path,
    case: str,
    match: str,
) -> None:
    offline_control, offline_candidate, worker_control, worker_candidate = _cohorts(
        tmp_path
    )

    if case == "affinity":
        for path in [*offline_candidate, *worker_candidate]:
            key = (
                "runtime_identity"
                if path in offline_candidate
                else "worker_runtime_identity"
            )
            _rewrite(
                path, lambda payload, key=key: payload[key].update(cpu_affinity=[1])
            )
    elif case == "thread_policy":
        _rewrite(
            offline_candidate[0],
            lambda payload: payload["thread_policy"].update(OMP_NUM_THREADS="2"),
        )
    elif case == "projection":
        drifted = "sha256:" + "b" * 64
        for path in worker_candidate:

            def mutate_projection(
                payload: dict[str, object], digest: str = drifted
            ) -> None:
                payload["bound_projection"]["digest"] = digest
                payload["post_cancel_success"]["projection_digest"] = digest
                payload["post_deadline_success"]["projection_digest"] = digest

            _rewrite(path, mutate_projection)
    elif case == "native_module":
        for path in worker_candidate:
            _rewrite(
                path,
                lambda payload: payload["worker_native_modules"]["rrt"].update(
                    sha256="f" * 64
                ),
            )
    elif case == "workload_self_check":
        _rewrite(
            offline_candidate[0],
            lambda payload: payload["workloads"]["rrt_joint"]["final_details"].update(
                waypoints=1
            ),
        )
    elif case == "missing_p99":
        _rewrite(
            worker_candidate[0],
            lambda payload: payload["warmed_latency_ms"].pop("p99"),
        )
    else:
        _rewrite(
            worker_candidate[0],
            lambda payload: payload["cancel"].update(progress=["terminal"]),
        )

    with pytest.raises(ValueError, match=match):
        evidence.aggregate(
            offline_control,
            offline_candidate,
            worker_control,
            worker_candidate,
        )


def test_patch0007_ab_evidence_proves_bounded_reuse_benefit() -> None:
    payload = json.loads(_PATCH0007_EVIDENCE.read_text(encoding="utf-8"))

    assert payload["schema"] == "manyforge.e1.patch0007_collision_reuse_ab.v1"
    assert payload["source_identity"] == {
        "commit": "e06d1a2f1b749c38742aee9e401bbe97fa967f7b",
        "tree": "d918767326a19d44886c0a4560cabe5a4ebe044d",
        "patch_set_sha256": (
            "6203705bb7bbbf8e01a89f6c1c628e7596d7ebf7529a4bb86636ffc4cdd72c43"
        ),
        "build_identity_sha256": (
            "4e93cfe207346c2e120441fa531e44b3bf06b31935e8a29ad3eefb60a1cbf9e3"
        ),
    }
    assert payload["runtime"] == {
        "architecture": "x86_64",
        "cpu_affinity": list(range(8, 16)),
        "thread_policy": {
            "OMP_NUM_THREADS": "1",
            "OPENBLAS_NUM_THREADS": "1",
            "MKL_NUM_THREADS": "1",
            "VECLIB_MAXIMUM_THREADS": "1",
            "NUMEXPR_NUM_THREADS": "1",
        },
    }
    assert payload["warmups"] == 2
    assert payload["iterations_per_mode_per_batch"] == 20
    assert payload["allocation_scope"] == (
        "C++ operator new/new[] calls during RRT::plan"
    )
    batches = payload["batches"]
    assert len(batches) == 5
    assert [batch["order"] for batch in batches] == [
        "ordinary_then_reused",
        "reused_then_ordinary",
        "ordinary_then_reused",
        "reused_then_ordinary",
        "ordinary_then_reused",
    ]
    metric_names = {
        "p50_ms",
        "p95_ms",
        "mean_ms",
        "throughput_hz",
        "operator_new_allocations_per_plan",
        "operator_new_bytes_per_plan",
        "waypoints",
    }
    for batch in batches:
        ordinary = batch["ordinary_private_context"]
        reused = batch["serialized_scene_reuse"]
        assert set(ordinary) == metric_names
        assert set(reused) == metric_names
        assert ordinary["waypoints"] == reused["waypoints"] == 20
        assert all(
            math.isfinite(value) and value > 0.0
            for name, value in ordinary.items()
            if name != "waypoints"
        )
        assert all(
            math.isfinite(value) and value > 0.0
            for name, value in reused.items()
            if name != "waypoints"
        )
        assert ordinary["p50_ms"] <= ordinary["p95_ms"]
        assert reused["p50_ms"] <= reused["p95_ms"]
        assert (
            reused["operator_new_allocations_per_plan"]
            < ordinary["operator_new_allocations_per_plan"]
        )
        assert (
            reused["operator_new_bytes_per_plan"]
            < ordinary["operator_new_bytes_per_plan"]
        )

    aggregate = payload["aggregate_batch_median"]
    for cohort in ("ordinary_private_context", "serialized_scene_reuse"):
        for metric in metric_names - {"waypoints"}:
            assert aggregate[cohort][metric] == pytest.approx(
                statistics.median(batch[cohort][metric] for batch in batches)
            )
    delta = aggregate["delta_percent"]
    assert delta["operator_new_allocations_per_plan"] <= -5.0
    assert delta["operator_new_bytes_per_plan"] <= -10.0
    assert delta["p50_ms"] <= 5.0
