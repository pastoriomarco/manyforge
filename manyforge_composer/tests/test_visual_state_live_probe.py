"""Retained-value and disposition tests for the visual-state live probe.

Historical blob ``7730d7524644c22a3c382f30f6877ba7b27bed19`` at
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` contains three cases.
The camel-case route was superseded by the native observation stream and
current_workbench_browser_collector.py, not a pending restoration. Scoring already-
normalized observer samples for latency, staleness, fallback, and publisher
health remains same-role pure behavior and is active now.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
from typing import Any, Final

_REPO_ROOT = Path(__file__).resolve().parents[2]


def _load_probe_module() -> Any:
    module_path = _REPO_ROOT / "scripts" / "qualify-visual-state-live.py"
    spec = importlib.util.spec_from_file_location(
        "manyforge_visual_probe_test",
        module_path,
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


HISTORICAL_CASE_FAMILIES: Final = (
    (
        "composer_route_sample_projection",
        "historical line 23",
        1,
        (),
        "superseded by test_execution_observation_stream.py and "
        "test_current_workbench_browser_collector.py; no historical HTTP sampler",
    ),
    (
        "normalized_latency_staleness_and_no_fallback_scoring",
        "historical lines 72 and 123",
        2,
        (),
        "normalized samples still fail or pass the same bounded qualification "
        "budgets without owning a route or runtime",
    ),
)


def test_visual_state_probe_cases_are_fully_disposed() -> None:
    assert sum(family[2] for family in HISTORICAL_CASE_FAMILIES) == 3
    assert all(not family[3] for family in HISTORICAL_CASE_FAMILIES)
    for successor in (
        "test_execution_observation_stream.py",
        "test_current_workbench_browser_collector.py",
    ):
        assert (Path(__file__).parent / successor).is_file()


def test_summary_and_evaluation_fail_on_legacy_stale_and_latency() -> None:
    probe = _load_probe_module()
    samples = [
        probe.Sample(
            index=0,
            ok=True,
            latency_ms=10.0,
            mode="visual_snapshot",
            served_from_visual_snapshot=True,
        ),
        probe.Sample(
            index=1,
            ok=True,
            latency_ms=50.0,
            mode="visual_snapshot_pending",
            snapshot_stale=True,
        ),
        probe.Sample(
            index=2,
            ok=True,
            latency_ms=250.0,
            mode="legacy_live",
            fallback_reason="visual_snapshot_error",
        ),
    ]

    summary = probe._build_summary(samples)
    failures = probe._evaluate(
        summary,
        max_error_count=0,
        max_legacy_count=0,
        max_non_snapshot_count=0,
        max_p95_ms=100.0,
        max_stale_ratio=0.10,
        max_pending_ratio=0.10,
    )

    assert summary["sample_count"] == 3
    assert summary["legacy_count"] == 1
    assert summary["non_snapshot_count"] == 1
    assert summary["stale_ratio"] == 1 / 3
    assert summary["pending_ratio"] == 1 / 3
    assert summary["latency_ms"]["p95"] > 100.0
    assert any("legacy-mode samples" in failure for failure in failures)
    assert any("non-snapshot samples" in failure for failure in failures)
    assert any("p95 latency" in failure for failure in failures)
    assert any("stale ratio" in failure for failure in failures)
    assert any("pending ratio" in failure for failure in failures)


def test_evaluation_accepts_snapshot_only_samples_inside_budget() -> None:
    probe = _load_probe_module()
    samples = [
        probe.Sample(
            index=index,
            ok=True,
            latency_ms=15.0 + index,
            mode="visual_snapshot",
            served_from_visual_snapshot=True,
            snapshot_age_ms=20.0,
            snapshot_sequence_number=index,
        )
        for index in range(5)
    ]

    summary = probe._build_summary(samples)
    failures = probe._evaluate(
        summary,
        max_error_count=0,
        max_legacy_count=0,
        max_non_snapshot_count=0,
        max_p95_ms=100.0,
        max_stale_ratio=0.05,
        max_pending_ratio=0.10,
    )

    assert summary["mode_counts"] == {"visual_snapshot": 5}
    assert summary["legacy_count"] == 0
    assert summary["pending_count"] == 0
    assert failures == []
