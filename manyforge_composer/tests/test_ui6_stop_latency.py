"""Contract tests for the UI6 repeated Stop-latency collector."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import sys

import pytest


_ROOT = Path(__file__).resolve().parents[2]
_QUALIFICATION = _ROOT / "scripts" / "qualification"
sys.path.insert(0, str(_QUALIFICATION))
_SPEC = importlib.util.spec_from_file_location(
    "ui6_stop_latency_test",
    _QUALIFICATION / "ui6_stop_latency.py",
)
assert _SPEC is not None and _SPEC.loader is not None
ui6_stop = importlib.util.module_from_spec(_SPEC)
sys.modules[_SPEC.name] = ui6_stop
_SPEC.loader.exec_module(ui6_stop)


def _sample(value: float) -> dict[str, float]:
    return {field: value for field in ui6_stop.LATENCY_FIELDS}


def test_nearest_rank_uses_the_99th_value_for_one_hundred_samples() -> None:
    values = [float(index) for index in range(1, 101)]

    assert ui6_stop._nearest_rank(values, 0.50) == 50.0
    assert ui6_stop._nearest_rank(values, 0.95) == 95.0
    assert ui6_stop._nearest_rank(values, 0.99) == 99.0
    assert ui6_stop._nearest_rank(values, 1.0) == 100.0


def test_summary_requires_one_hundred_warm_samples_and_excludes_cold() -> None:
    with pytest.raises(ui6_stop.CollectorError, match="too few"):
        ui6_stop._summarize([_sample(1.0)] * 99)

    summary = ui6_stop._summarize(
        [_sample(float(index)) for index in range(1, 101)]
    )

    assert summary["sampleCount"] == 100
    assert summary["metricsMs"]["clickToDispatchMs"] == {
        "p50": 50.0,
        "p95": 95.0,
        "p99": 99.0,
        "max": 100.0,
        "maxSample": 100,
    }


def test_pressure_interference_is_bounded_only_on_stop_handoff() -> None:
    baseline = ui6_stop._summarize([_sample(20.0)] * 100)
    loaded = ui6_stop._summarize([_sample(25.0)] * 100)

    comparison = ui6_stop._assert_interference(baseline, loaded)

    assert comparison["handoff"] == {
        "baselineP99Ms": 60.0,
        "pressuredP99Ms": 75.0,
        "allowedIncreaseMs": 30.0,
    }


def test_frozen_absolute_budgets_reject_old_composer_tail() -> None:
    samples = [_sample(1.0) for _ in range(100)]
    samples[98]["composerIngressToKernelAcceptanceMs"] = 466.8
    samples[99]["composerIngressToKernelAcceptanceMs"] = 466.8
    summary = ui6_stop._summarize(samples)

    with pytest.raises(ui6_stop.CollectorError, match="exceeded 100.0 ms"):
        ui6_stop._assert_budgets(summary, "candidate")


def test_stopped_presentation_wait_uses_one_browser_side_observer() -> None:
    state = {
        "runText": "Resume Motion",
        "runDisabled": False,
        "stopText": "Stopped",
        "stopDisabled": True,
        "velocityIdentityStable": True,
    }
    trace = [
        {
            **state,
            "runText": "Running",
            "runDisabled": True,
            "stopText": "Stopping",
        },
        state,
    ]

    class Driver:
        def __init__(self) -> None:
            self.calls: list[tuple[str, str, dict[str, object]]] = []

        def command(
            self,
            method: str,
            path: str,
            payload: dict[str, object],
        ) -> dict[str, object]:
            self.calls.append((method, path, payload))
            return {"state": state, "trace": trace}

    driver = Driver()

    observed_state, observed_trace = ui6_stop._await_stopped_presentation(driver)

    assert observed_state == state
    assert observed_trace == trace
    assert driver.calls == [
        (
            "POST",
            "/execute/async",
            {"script": ui6_stop.STOP_PRESENTATION_WAIT_SCRIPT, "args": []},
        )
    ]
    assert "MutationObserver" in ui6_stop.STOP_PRESENTATION_WAIT_SCRIPT
