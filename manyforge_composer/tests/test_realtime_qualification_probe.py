"""Tests for the RT-6 realtime qualification evidence probe."""
from __future__ import annotations

import importlib.util
from pathlib import Path
from typing import Any


_REPO_ROOT = Path(__file__).resolve().parents[2]
_PROBE = _REPO_ROOT / "manyforge_core" / "tools" / "realtime_qualification_probe.py"


def _load_probe() -> Any:
    spec = importlib.util.spec_from_file_location("manyforge_rt6_probe_test", _PROBE)
    assert spec is not None
    assert spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_rt6_probe_reports_source_architecture_and_collision_resource_policy(
    monkeypatch,
) -> None:
    probe = _load_probe()
    monkeypatch.setattr(probe, "_current_scheduler_policy", lambda: (0, None))
    monkeypatch.setattr(probe, "_current_affinity", lambda: ([0, 1, 2, 3], None))
    monkeypatch.setattr(probe, "_locked_memory_bytes", lambda: (0, None))

    evidence = probe.build_evidence({})

    assert evidence["schema_version"] == "manyforge.rt6_qualification.v1"
    assert evidence["passed"] is True
    assert evidence["source_level_architecture"]["highest_runtime_role"] == (
        "controller_command"
    )
    assert evidence["source_level_architecture"]["runtime_collision_role"] == (
        "runtime_safety"
    )
    assert evidence["source_level_architecture"]["python_routes_realtime_authority"] is False
    assert evidence["collision_resources"] == {
        "resource_policy": "role_owned_collision_resources",
        "validation_runtime_concurrency_mode": (
            "independent_validation_resources_or_fail_closed_defer"
        ),
        "shared_single_context_runtime_validation": "fail_closed_deferred",
        "runtime_snapshot_coherence": (
            "world_policy_paired_bundle_observed_state_may_lead_with_budget"
        ),
    }
    assert evidence["environment_dependent_gates"]["isaac_ros_loaded_gate"] == (
        "not_run_by_probe"
    )


def test_rt6_probe_marks_requested_os_realtime_mismatch_as_failed(monkeypatch) -> None:
    probe = _load_probe()
    monkeypatch.setattr(probe, "_current_scheduler_policy", lambda: (0, None))
    monkeypatch.setattr(probe, "_current_affinity", lambda: ([0, 1], None))
    monkeypatch.setattr(probe, "_locked_memory_bytes", lambda: (0, None))

    evidence = probe.build_evidence(
        {
            "MANYFORGE_RT6_REQUESTED_SCHED_POLICY": "SCHED_FIFO",
            "MANYFORGE_RT6_REQUESTED_CPU_AFFINITY": "0-2",
            "MANYFORGE_RT6_REQUESTED_MLOCK": "1",
        }
    )

    assert evidence["passed"] is False
    assert evidence["failed_checks"] == [
        "scheduler_policy",
        "cpu_affinity",
        "memory_locking",
    ]
    assert evidence["os_realtime"]["scheduler_policy"]["accepted"] is False
    assert evidence["os_realtime"]["cpu_affinity"]["accepted"] is False
    assert evidence["os_realtime"]["memory_locking"]["accepted"] is False


def test_rt6_probe_requires_exact_cpu_affinity_match(monkeypatch) -> None:
    probe = _load_probe()
    monkeypatch.setattr(probe, "_current_scheduler_policy", lambda: (0, None))
    monkeypatch.setattr(probe, "_current_affinity", lambda: ([0, 1, 2, 3], None))
    monkeypatch.setattr(probe, "_locked_memory_bytes", lambda: (0, None))

    superset = probe.build_evidence({"MANYFORGE_RT6_REQUESTED_CPU_AFFINITY": "2-3"})
    exact = probe.build_evidence({"MANYFORGE_RT6_REQUESTED_CPU_AFFINITY": "0-3"})

    assert superset["passed"] is False
    assert superset["failed_checks"] == ["cpu_affinity"]
    assert superset["os_realtime"]["cpu_affinity"]["accepted"] is False
    assert exact["passed"] is True
    assert exact["os_realtime"]["cpu_affinity"]["accepted"] is True


def test_rt6_probe_fails_requested_os_realtime_when_unobservable(monkeypatch) -> None:
    probe = _load_probe()
    monkeypatch.setattr(probe, "_current_scheduler_policy", lambda: (0, None))
    monkeypatch.setattr(probe, "_current_affinity", lambda: (None, "unavailable"))
    monkeypatch.setattr(probe, "_locked_memory_bytes", lambda: (None, "unavailable"))

    evidence = probe.build_evidence(
        {
            "MANYFORGE_RT6_REQUESTED_CPU_AFFINITY": "0",
            "MANYFORGE_RT6_REQUESTED_MLOCK": "1",
        }
    )

    assert evidence["passed"] is False
    assert evidence["failed_checks"] == ["cpu_affinity", "memory_locking"]
    assert evidence["os_realtime"]["cpu_affinity"]["accepted"] is False
    assert evidence["os_realtime"]["cpu_affinity"]["error"] == "unavailable"
    assert evidence["os_realtime"]["memory_locking"]["accepted"] is False
    assert evidence["os_realtime"]["memory_locking"]["error"] == "unavailable"


def test_rt6_probe_fails_malformed_requested_cpu_affinity(monkeypatch) -> None:
    probe = _load_probe()
    monkeypatch.setattr(probe, "_current_scheduler_policy", lambda: (0, None))
    monkeypatch.setattr(probe, "_current_affinity", lambda: ([0, 1], None))
    monkeypatch.setattr(probe, "_locked_memory_bytes", lambda: (0, None))

    evidence = probe.build_evidence({"MANYFORGE_RT6_REQUESTED_CPU_AFFINITY": "3-1"})

    assert evidence["passed"] is False
    assert evidence["failed_checks"] == ["cpu_affinity"]
    assert evidence["os_realtime"]["cpu_affinity"]["accepted"] is False
    assert "descending CPU range" in evidence["os_realtime"]["cpu_affinity"]["error"]
