from __future__ import annotations

import importlib.util
from pathlib import Path
from unittest.mock import patch

from scripts.qualification.viewer_qualification_contract import (
    ViewerQualificationMatrixV1,
    ViewerQualificationSampleV1,
)


_REPO_ROOT = Path(__file__).resolve().parents[2]
_RUNNER_PATH = _REPO_ROOT / "scripts" / "qualify-viewer-data-plane.py"
_SPEC = importlib.util.spec_from_file_location("viewer_qualification_runner", _RUNNER_PATH)
assert _SPEC is not None and _SPEC.loader is not None
_RUNNER = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_RUNNER)


def _sample(
    index: int,
    *,
    builds: int,
    client_received_at_ns: int,
    cpu_percent: float | None,
) -> ViewerQualificationSampleV1:
    return ViewerQualificationSampleV1(
        index=index,
        monotonic_ns=1_000_000_000 + index,
        viewer_route_latency_ms=1.0 + index,
        kernel_route_latency_ms=2.0 + index,
        viewer_status={
            "producer_epoch": "producer-a",
            "manifest_revision": 1,
            "client_count": 1,
            "active_client_count": 1,
            "builds_started": builds,
            "fanout": {
                "clients": {
                    "client": {
                        "client_diagnostics_received_at_ns": client_received_at_ns,
                        "client_diagnostics": {
                            "window_duration_ms": 100.0,
                            "frame_messages": 10,
                            "long_tasks": 0,
                            "materialization_max_ms": 2.0,
                            "react_commits": 0,
                            "topology_rebuilds": 0,
                        },
                    }
                }
            },
        },
        kernel_metrics={"events_emitted": 100 + index},
        kernel_snapshot={"state": "idle"},
        processes={
            "composer": {
                "cpu_percent": cpu_percent,
                "rss_bytes": 1000 + index,
            }
        },
        errors=[],
    )


def test_qualification_summary_uses_deltas_and_deduplicates_client_windows() -> None:
    samples = [
        _sample(0, builds=5, client_received_at_ns=2_000_000_000, cpu_percent=None),
        _sample(1, builds=8, client_received_at_ns=2_000_000_000, cpu_percent=20.0),
        _sample(2, builds=9, client_received_at_ns=3_000_000_000, cpu_percent=30.0),
    ]

    summary = _RUNNER._build_summary(samples)

    assert summary["viewer_counter_delta"]["builds_started"] == 4.0
    assert summary["kernel_counter_delta"]["events_emitted"] == 2.0
    assert len(summary["client_reported_windows"]) == 2
    assert summary["processes"]["composer"]["cpu_percent"]["p95"] == 30.0


def test_qualification_observers_use_nested_counter_deltas() -> None:
    first = _sample(
        0,
        builds=5,
        client_received_at_ns=2_000_000_000,
        cpu_percent=None,
    )
    last = _sample(
        1,
        builds=6,
        client_received_at_ns=3_000_000_000,
        cpu_percent=10.0,
    )
    first.observers = {"point_cloud": {"published": 10, "nested": {"sent": 4}}}
    last.observers = {"point_cloud": {"published": 13, "nested": {"sent": 6}}}

    summary = _RUNNER._build_summary([first, last])

    assert summary["observer_counter_delta"] == {
        "point_cloud": {"published": 3.0, "nested.sent": 2.0}
    }
    failures = _RUNNER._evaluate(
        summary,
        require_active_client=False,
        require_no_client=False,
        require_zero_builds=False,
        client_count_min=None,
        client_count_max=None,
        active_client_count_min=None,
        max_viewer_route_p95_ms=50.0,
        max_process_cpu={},
        max_client_long_tasks=None,
        max_client_materialization_ms=None,
        max_client_react_commits_per_frame=None,
        max_client_unmatched_topology_rebuilds=None,
        max_build_failures=0,
        max_publication_sink_drops=0,
        minimum_observer_counter_deltas={
            "point_cloud": {"published": 4.0, "nested.sent": 1.0}
        },
    )
    assert failures == [
        "observer counter delta point_cloud.published=3 is below required 4"
    ]


def test_qualification_summary_excludes_client_window_that_straddles_warmup() -> None:
    sample = _sample(
        0,
        builds=5,
        client_received_at_ns=1_050_000_000,
        cpu_percent=None,
    )
    diagnostics = sample.viewer_status["fanout"]["clients"]["client"][
        "client_diagnostics"
    ]
    diagnostics["topology_rebuilds"] = 1

    summary = _RUNNER._build_summary([sample])

    assert summary["client_reported_windows"] == []
    assert summary["client_maxima"] == {}


def test_static_client_window_has_zero_commits_per_frame_without_frame_churn() -> None:
    sample = _sample(
        0,
        builds=5,
        client_received_at_ns=2_000_000_000,
        cpu_percent=None,
    )
    diagnostics = sample.viewer_status["fanout"]["clients"]["client"][
        "client_diagnostics"
    ]
    diagnostics["frame_messages"] = 0
    diagnostics["react_commits"] = 0

    summary = _RUNNER._build_summary([sample])

    assert summary["client_reported_windows"]
    assert summary["client_react_commits_per_frame"] == 0.0


def test_topology_gate_allows_only_rebuilds_backed_by_manifest_changes() -> None:
    first = _sample(
        0,
        builds=5,
        client_received_at_ns=2_000_000_000,
        cpu_percent=None,
    )
    last = _sample(
        1,
        builds=6,
        client_received_at_ns=3_000_000_000,
        cpu_percent=10.0,
    )
    last.viewer_status["manifest_revision"] = 2
    last.viewer_status["fanout"]["clients"]["client"]["client_diagnostics"][
        "topology_rebuilds"
    ] = 1
    summary = _RUNNER._build_summary([first, last])

    assert summary["client_expected_topology_rebuilds"] == 1
    assert summary["client_unmatched_topology_rebuilds"] == 0
    common = {
        "require_active_client": False,
        "require_no_client": False,
        "require_zero_builds": False,
        "client_count_min": None,
        "client_count_max": None,
        "active_client_count_min": None,
        "max_viewer_route_p95_ms": 50.0,
        "max_process_cpu": {},
        "max_client_long_tasks": None,
        "max_client_materialization_ms": None,
        "max_client_react_commits_per_frame": None,
        "max_client_unmatched_topology_rebuilds": 0,
        "max_build_failures": 0,
        "max_publication_sink_drops": 0,
    }
    assert _RUNNER._evaluate(summary, **common) == []

    last.viewer_status["fanout"]["clients"]["client"]["client_diagnostics"][
        "topology_rebuilds"
    ] = 2
    summary = _RUNNER._build_summary([first, last])
    assert summary["client_unmatched_topology_rebuilds"] == 1
    assert _RUNNER._evaluate(summary, **common) == [
        "client topology rebuilds without a matching manifest change 1 exceed 0"
    ]


def test_qualification_rejects_named_workload_when_required_counters_do_not_move() -> None:
    summary = _RUNNER._build_summary(
        [
            _sample(
                0,
                builds=5,
                client_received_at_ns=2_000_000_000,
                cpu_percent=None,
            ),
            _sample(
                1,
                builds=6,
                client_received_at_ns=3_000_000_000,
                cpu_percent=10.0,
            ),
        ]
    )
    summary["last_viewer_status"]["layers"] = ["canonical"]
    summary["viewer_counter_delta"]["render_revision"] = 0.0
    summary["kernel_counter_delta"]["controller_command_progress_samples_total"] = 0.0
    summary["client_totals"]["rendered_frames"] = 0.0

    failures = _RUNNER._evaluate(
        summary,
        require_active_client=False,
        require_no_client=False,
        require_zero_builds=False,
        client_count_min=None,
        client_count_max=None,
        active_client_count_min=None,
        max_viewer_route_p95_ms=50.0,
        max_process_cpu={},
        max_client_long_tasks=None,
        max_client_materialization_ms=None,
        max_client_react_commits_per_frame=None,
        max_client_unmatched_topology_rebuilds=None,
        max_build_failures=0,
        max_publication_sink_drops=0,
        required_layers=["measured_robot"],
        minimum_viewer_counter_deltas={"render_revision": 1.0},
        minimum_kernel_counter_deltas={
            "controller_command_progress_samples_total": 1.0
        },
        minimum_kernel_state_samples={"executing": 1},
        minimum_client_totals={"rendered_frames": 1.0},
    )

    assert failures == [
        "missing required viewer layers: measured_robot",
        "viewer counter delta render_revision=0 is below required 1",
        "kernel counter delta controller_command_progress_samples_total=0 is below required 1",
        "kernel state executing sampled 0 times; required 1",
        "client total rendered_frames=0 is below required 1",
    ]


def test_qualification_evaluation_enforces_no_demand_zero_build_contract() -> None:
    summary = _RUNNER._build_summary(
        [
            _sample(
                0,
                builds=5,
                client_received_at_ns=2_000_000_000,
                cpu_percent=None,
            ),
            _sample(
                1,
                builds=6,
                client_received_at_ns=3_000_000_000,
                cpu_percent=10.0,
            ),
        ]
    )
    summary["last_viewer_status"]["client_count"] = 0
    summary["last_viewer_status"]["active_client_count"] = 0

    failures = _RUNNER._evaluate(
        summary,
        require_active_client=False,
        require_no_client=True,
        require_zero_builds=True,
        client_count_min=None,
        client_count_max=0,
        active_client_count_min=0,
        max_viewer_route_p95_ms=50.0,
        max_process_cpu={"composer": 50.0},
        max_client_long_tasks=0,
        max_client_materialization_ms=5.0,
        max_client_react_commits_per_frame=0.0,
        max_client_unmatched_topology_rebuilds=0,
        max_build_failures=0,
        max_publication_sink_drops=0,
    )

    assert failures == ["viewer builds advanced without demand: 1"]


def test_checked_matrix_covers_every_policy_required_profile() -> None:
    matrix_path = (
        _REPO_ROOT
        / "scripts"
        / "qualification"
        / "viewer_qualification_matrix.v1.json"
    )
    matrix = ViewerQualificationMatrixV1.model_validate_json(
        matrix_path.read_text(encoding="utf-8")
    )

    required = set(matrix.policy["required_matrix"])
    actual = {scenario.name for scenario in matrix.scenarios}
    assert actual == required
    assert all(scenario.preconditions for scenario in matrix.scenarios)
    assert all(
        "composer" in scenario.required_process_labels
        for scenario in matrix.scenarios
    )
    assert all(
        set(scenario.requirements.observer_counter_delta_min)
        <= set(scenario.observer_paths)
        for scenario in matrix.scenarios
    )
    focused_motion = next(
        scenario for scenario in matrix.scenarios if scenario.name == "focused_motion"
    )
    assert focused_motion.requirements.kernel_state_sample_min == {"executing": 1}
    assert (
        "controller_command_progress_samples_total"
        not in focused_motion.requirements.kernel_counter_delta_min
    )


def test_qualification_accepts_long_motion_without_a_new_dispatch() -> None:
    first = _sample(
        0,
        builds=5,
        client_received_at_ns=2_000_000_000,
        cpu_percent=None,
    )
    second = _sample(
        1,
        builds=6,
        client_received_at_ns=3_000_000_000,
        cpu_percent=10.0,
    )
    first.kernel_snapshot = {"state": "executing", "execution_id": 7}
    second.kernel_snapshot = {"state": "executing", "execution_id": 7}
    first.kernel_metrics["controller_command_progress_samples_total"] = 4
    second.kernel_metrics["controller_command_progress_samples_total"] = 4

    summary = _RUNNER._build_summary([first, second])

    assert summary["kernel_state_samples"] == {"executing": 2}
    assert _RUNNER._evaluate(
        summary,
        require_active_client=False,
        require_no_client=False,
        require_zero_builds=False,
        client_count_min=None,
        client_count_max=None,
        active_client_count_min=None,
        max_viewer_route_p95_ms=50.0,
        max_process_cpu={},
        max_client_long_tasks=None,
        max_client_materialization_ms=None,
        max_client_react_commits_per_frame=None,
        max_client_unmatched_topology_rebuilds=None,
        max_build_failures=0,
        max_publication_sink_drops=0,
        minimum_kernel_state_samples={"executing": 1},
    ) == []


def test_process_sampler_treats_absent_or_restarted_worker_as_new_generation() -> None:
    previous: dict[str, tuple[int, int, float]] = {}
    inventories = [
        {1: (0, 100.0, 10_000)},
        {1: (0, 101.0, 10_000)},
        {42: (1, 10.0, 1000)},
        {42: (1, 10.5, 1000)},
        {84: (1, 1.0, 2000)},
    ]
    with patch.object(_RUNNER, "_proc_inventory", side_effect=inventories):
        absent_first = _RUNNER._sample_process_trees(
            {"viewer_worker": 0}, previous, now_ns=1_000_000_000
        )
        absent_second = _RUNNER._sample_process_trees(
            {"viewer_worker": 0}, previous, now_ns=2_000_000_000
        )
        started = _RUNNER._sample_process_trees(
            {"viewer_worker": 42}, previous, now_ns=3_000_000_000
        )
        running = _RUNNER._sample_process_trees(
            {"viewer_worker": 42}, previous, now_ns=4_000_000_000
        )
        restarted = _RUNNER._sample_process_trees(
            {"viewer_worker": 84}, previous, now_ns=5_000_000_000
        )

    assert absent_first["viewer_worker"]["cpu_percent"] is None
    assert absent_first["viewer_worker"]["process_count"] == 0
    assert absent_second["viewer_worker"]["cpu_percent"] == 0.0
    assert started["viewer_worker"]["cpu_percent"] is None
    assert running["viewer_worker"]["cpu_percent"] == 50.0
    assert restarted["viewer_worker"]["cpu_percent"] is None


def test_process_sampler_excludes_explicit_child_tree_from_named_parent() -> None:
    previous: dict[str, tuple[int, int, float]] = {}
    inventory = {
        1: (0, 10.0, 1000),
        2: (1, 5.0, 2000),
        3: (2, 2.0, 3000),
        4: (1, 1.0, 4000),
    }
    with (
        patch.object(_RUNNER, "_proc_inventory", return_value=inventory),
        patch.object(_RUNNER, "_thread_count", return_value=1),
        patch.object(_RUNNER, "_cpu_affinity", return_value="0-3"),
    ):
        sampled = _RUNNER._sample_process_trees(
            {"composer": 1, "viewer_worker": 2},
            previous,
            now_ns=1_000_000_000,
        )

    assert sampled["composer"]["process_count"] == 2
    assert sampled["composer"]["cpu_seconds"] == 11.0
    assert sampled["composer"]["rss_bytes"] == 5000
    assert sampled["viewer_worker"]["process_count"] == 2
    assert sampled["viewer_worker"]["cpu_seconds"] == 7.0
    assert sampled["viewer_worker"]["rss_bytes"] == 5000


def test_explicit_viewer_worker_pid_is_not_replaced_by_container_namespace_pid(
    monkeypatch,
    tmp_path: Path,
) -> None:
    artifact = tmp_path / "qualification.json"
    status = {
        "client_count": 0,
        "active_client_count": 0,
        "worker": {"pid": 17},
    }
    sampled_roots: list[dict[str, int]] = []

    monkeypatch.setattr(
        _RUNNER,
        "_parse_args",
        lambda: type(
            "Args",
            (),
            {
                "scenario": "namespace",
                "profile": None,
                "matrix": "unused",
                "acknowledge_preconditions": False,
                "list_profiles": False,
                "artifact": str(artifact),
                "base_url": "http://unused",
                "subject_source_commit": "subject",
                "subject_source_state": "clean",
                "subject_source_description": None,
                "viewer_status_path": "/api/scene/viewer/status",
                "token": None,
                "duration_s": 0.001,
                "warmup_s": 0.0,
                "hz": 1000.0,
                "timeout_s": 1.0,
                "pid": ["viewer_worker=17000"],
                "require_active_client": False,
                "require_no_client": False,
                "require_zero_builds": False,
                "max_viewer_route_p95_ms": None,
                "max_process_cpu": [],
                "max_client_long_tasks": None,
            },
        )(),
    )
    monkeypatch.setattr(
        _RUNNER,
        "_get_json",
        lambda _base, path, **_kwargs: (
            (status if path == "/api/scene/viewer/status" else {}),
            0.1,
        ),
    )
    monkeypatch.setattr(
        _RUNNER,
        "_sample_process_trees",
        lambda roots, _previous, **_kwargs: sampled_roots.append(dict(roots)) or {},
    )
    monkeypatch.setattr(_RUNNER, "_source_identity", lambda: {"commit": "observer"})

    assert _RUNNER.main() == 0
    assert sampled_roots
    assert all(roots["viewer_worker"] == 17000 for roots in sampled_roots)
    payload = __import__("json").loads(artifact.read_text(encoding="utf-8"))
    assert payload["configuration"]["viewer_worker_pid_source"] == "explicit"
