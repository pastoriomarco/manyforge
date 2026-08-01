"""Retained-value and disposition tests for viewer qualification helpers.

Historical blob ``bbc5c47065cf9c1e909f4152c8f063531cf29727`` at
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` contains 12 cases.
Pure normalized counter/observer/state/matrix checks remain.  Viewer-v1
client windows, workload vocabulary, process collection, namespace identity,
and the complete live qualification claims return at I9/I.
"""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
from typing import Any, Final

import pytest

from scripts.qualification.viewer_qualification_contract import (
    ViewerQualificationSampleV1,
)


_REPO_ROOT = Path(__file__).resolve().parents[2]
_RUNNER_PATH = _REPO_ROOT / "scripts" / "qualify-viewer-data-plane.py"
_SPEC = importlib.util.spec_from_file_location(
    "viewer_qualification_runner_test",
    _RUNNER_PATH,
)
assert _SPEC is not None and _SPEC.loader is not None
runner = importlib.util.module_from_spec(_SPEC)
sys.modules[_SPEC.name] = runner
_SPEC.loader.exec_module(runner)


HISTORICAL_CASE_FAMILIES: Final = (
    (
        "mixed_counter_process_and_viewer_v1_window_summary",
        "historical line 67",
        1,
        ("I9", "I"),
        "counter/process math is retained and tested separately; the complete "
        "case also asserts old fanout-window deduplication and returns at I9/I",
    ),
    (
        "nested_observer_counter_deltas",
        "historical line 82",
        1,
        (),
        "bounded nested numeric observer deltas remain same-role value behavior",
    ),
    (
        "viewer_v1_window_warmup_commit_and_topology_accounting",
        "historical lines 128, 146, and 165",
        3,
        ("I9", "I"),
        "client-window and manifest/producer semantics are recreated against "
        "normalized observer evidence, not inferred from the deleted wire",
    ),
    (
        "viewer_v1_workload_and_no_demand_contracts",
        "historical lines 214 and 270",
        2,
        ("I9", "I"),
        "old layer/counter/build vocabulary is requalified after the new "
        "observer and viewer contract exists",
    ),
    (
        "matrix_cross_reference_validation",
        "historical line 311",
        1,
        (),
        "the checked machine-readable matrix remains internally complete",
    ),
    (
        "kernel_state_sample_budget",
        "historical line 345",
        1,
        (),
        "already-normalized state samples can satisfy a duration-independent budget",
    ),
    (
        "process_generation_and_namespace_identity",
        "historical lines 386, 420, and 447",
        3,
        ("I9", "I"),
        "live process-tree collection and worker PID ownership return at I9/I",
    ),
)


def _sample(
    index: int,
    *,
    builds: int,
    cpu_percent: float | None,
) -> ViewerQualificationSampleV1:
    return ViewerQualificationSampleV1(
        index=index,
        monotonic_ns=1_000_000_000 + index,
        viewer_route_latency_ms=1.0 + index,
        kernel_route_latency_ms=2.0 + index,
        viewer_status={
            "client_count": 1,
            "active_client_count": 1,
            "builds_started": builds,
            "layers": ["canonical"],
        },
        kernel_metrics={"events_emitted": 100 + index},
        kernel_snapshot={"state": "idle"},
        processes={
            "observer": {
                "cpu_percent": cpu_percent,
                "rss_bytes": 1_000 + index,
            }
        },
        errors=[],
    )


def _policy_defaults() -> dict[str, Any]:
    return {
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
        "max_client_unmatched_topology_rebuilds": None,
        "max_build_failures": 0,
        "max_publication_sink_drops": 0,
    }


def test_viewer_qualification_cases_are_fully_disposed() -> None:
    assert sum(family[2] for family in HISTORICAL_CASE_FAMILIES) == 12
    assert sum(family[2] for family in HISTORICAL_CASE_FAMILIES if family[3]) == 9
    assert sum(family[2] for family in HISTORICAL_CASE_FAMILIES if not family[3]) == 3


def test_normalized_summary_retains_counter_and_process_metric_math() -> None:
    summary = runner._build_summary(
        [
            _sample(0, builds=5, cpu_percent=None),
            _sample(1, builds=8, cpu_percent=20.0),
            _sample(2, builds=9, cpu_percent=30.0),
        ]
    )

    assert summary["viewer_counter_delta"]["builds_started"] == 4.0
    assert summary["kernel_counter_delta"]["events_emitted"] == 2.0
    assert summary["processes"]["observer"]["cpu_percent"]["p95"] == 30.0
    assert "client_reported_windows" not in summary


def test_qualification_observers_use_nested_counter_deltas() -> None:
    first = _sample(0, builds=5, cpu_percent=None)
    last = _sample(1, builds=6, cpu_percent=10.0)
    first.observers = {"point_cloud": {"published": 10, "nested": {"sent": 4}}}
    last.observers = {"point_cloud": {"published": 13, "nested": {"sent": 6}}}

    summary = runner._build_summary([first, last])

    assert summary["observer_counter_delta"] == {
        "point_cloud": {"published": 3.0, "nested.sent": 2.0}
    }
    failures = runner._evaluate(
        summary,
        **_policy_defaults(),
        minimum_observer_counter_deltas={
            "point_cloud": {"published": 4.0, "nested.sent": 1.0}
        },
    )
    assert failures == [
        "observer counter delta point_cloud.published=3 is below required 4"
    ]


def test_checked_matrix_still_covers_policy_and_observer_cross_references() -> None:
    matrix_path = (
        _REPO_ROOT
        / "scripts"
        / "qualification"
        / "viewer_qualification_matrix.v1.json"
    )
    matrix = runner.validate_qualification_matrix(
        json.loads(matrix_path.read_text(encoding="utf-8"))
    )

    assert {scenario.name for scenario in matrix.scenarios} == set(
        matrix.policy["required_matrix"]
    )
    assert all(
        set(scenario.requirements.observer_counter_delta_min)
        <= set(scenario.observer_paths)
        for scenario in matrix.scenarios
    )


def test_qualification_accepts_normalized_long_state_without_new_counter_delta() -> None:
    first = _sample(0, builds=5, cpu_percent=None)
    second = _sample(1, builds=6, cpu_percent=10.0)
    first.kernel_snapshot = {"state": "executing", "execution_id": 7}
    second.kernel_snapshot = {"state": "executing", "execution_id": 7}
    first.kernel_metrics["progress_samples"] = 4
    second.kernel_metrics["progress_samples"] = 4

    summary = runner._build_summary([first, second])

    assert summary["kernel_state_samples"] == {"executing": 2}
    assert runner._evaluate(
        summary,
        **_policy_defaults(),
        minimum_kernel_state_samples={"executing": 1},
    ) == []


def test_artifact_validation_checks_summary_and_success_consistency() -> None:
    samples = [_sample(0, builds=5, cpu_percent=None)]
    summary = runner._build_summary(samples)
    artifact = runner.validate_qualification_artifact(
        {
            "schema_version": "manyforge.viewer.qualification.v1",
            "scenario": "normalized-values",
            "captured_at": "2026-07-31T00:00:00+00:00",
            "source": {},
            "environment": {},
            "configuration": {},
            "samples": [sample.model_dump(mode="json") for sample in samples],
            "summary": summary,
            "failures": [],
            "success": True,
        }
    )
    assert artifact.success is True

    inconsistent = artifact.model_dump(mode="json")
    inconsistent["success"] = False
    with pytest.raises(ValueError, match="success must equal not failures"):
        runner.validate_qualification_artifact(inconsistent)


def test_live_collector_is_a_typed_i9_i_gate() -> None:
    with pytest.raises(
        runner.ViewerQualificationUnavailableError,
        match="returns at I9/I",
    ):
        runner.require_viewer_qualification()
