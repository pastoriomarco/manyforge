"""Regression tests for the visual-state live qualification probe."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
from typing import Any

_REPO_ROOT = Path(__file__).resolve().parents[2]


def _load_probe_module() -> Any:
    module_path = _REPO_ROOT / "scripts" / "qualify-visual-state-live.py"
    spec = importlib.util.spec_from_file_location("manyforge_visual_probe_test", module_path)
    assert spec is not None
    assert spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def test_sample_scene_state_accepts_camel_case_route_diagnostics(
    monkeypatch,
) -> None:
    probe = _load_probe_module()

    monkeypatch.setattr(
        probe,
        "_get_json",
        lambda *_args, **_kwargs: {
            "objects": [{"object_id": "box"}],
            "grippers": [{"gripper_id": "primary"}],
            "poseSources": [{"sourceId": "isaac", "isFresh": True}],
            "visualStateDiagnostics": {
                "mode": "visual_snapshot",
                "servedFromVisualSnapshot": True,
                "fallbackReason": None,
                "total_ms": 2.4,
                "snapshotAgeMs": 18.0,
                "snapshotSequenceNumber": 42,
                "snapshotBuildMs": 3.5,
                "snapshotStale": False,
                "snapshotPublisherRunning": True,
                "snapshotPublisherError": None,
                "objectsSource": "visual_snapshot",
            },
        },
    )

    sample = probe._sample_scene_state(
        base_url="http://composer.invalid",
        index=7,
        timeout_s=0.01,
    )

    assert sample.ok is True
    assert sample.index == 7
    assert sample.mode == "visual_snapshot"
    assert sample.served_from_visual_snapshot is True
    assert sample.snapshot_sequence_number == 42
    assert sample.snapshot_age_ms == 18.0
    assert sample.snapshot_build_ms == 3.5
    assert sample.snapshot_publisher_running is True
    assert sample.objects_source == "visual_snapshot"
    assert sample.object_count == 1
    assert sample.gripper_count == 1
    assert sample.pose_source_count == 1
    assert sample.fresh_pose_source_count == 1


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
    assert any("legacy_live samples" in failure for failure in failures)
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
