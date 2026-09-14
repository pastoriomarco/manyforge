"""Contract tests for the UI6 repeated Stop-latency collector."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
from types import SimpleNamespace

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


def test_motion_setup_selects_exact_program_node_history(monkeypatch) -> None:
    event = {"sequence": 12, "event": {"event_type": "operation_executing"}}

    def history(_url, path, *, params):
        assert path == "/api/execution/history"
        assert dict(params) == {
            "event_types": "operation_executing",
            "limit": 32,
            "program_run_id": "run.1",
            "node_instance_id": "move_to_rest",
        }
        return {"events": [event]}

    monkeypatch.setattr(ui6_stop, "_http_json", history)
    assert (
        ui6_stop._wait_history_event_after(
            "http://composer",
            "operation_executing",
            11,
            program_run_id="run.1",
            node_instance_id="move_to_rest",
        )
        == event
    )


def test_nearest_rank_uses_the_99th_value_for_one_hundred_samples() -> None:
    values = [float(index) for index in range(1, 101)]

    assert ui6_stop._nearest_rank(values, 0.50) == 50.0
    assert ui6_stop._nearest_rank(values, 0.95) == 95.0
    assert ui6_stop._nearest_rank(values, 0.99) == 99.0
    assert ui6_stop._nearest_rank(values, 1.0) == 100.0


def test_summary_requires_one_hundred_warm_samples_and_excludes_cold() -> None:
    with pytest.raises(ui6_stop.CollectorError, match="too few"):
        ui6_stop._summarize([_sample(1.0)] * 99)

    summary = ui6_stop._summarize([_sample(float(index)) for index in range(1, 101)])

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

    comparison = ui6_stop._compare_interference(baseline, loaded)

    assert comparison["handoff"] == {
        "baselineP99Ms": 60.0,
        "pressuredP99Ms": 75.0,
        "allowedIncreaseMs": 30.0,
        "passed": True,
    }


def test_frozen_absolute_budgets_reject_old_composer_tail() -> None:
    samples = [_sample(1.0) for _ in range(100)]
    samples[98]["controlIngressToKernelAcceptanceMs"] = 466.8
    samples[99]["controlIngressToKernelAcceptanceMs"] = 466.8
    summary = ui6_stop._summarize(samples)

    failures = ui6_stop._budget_failures(summary, "candidate")
    assert len(failures) == 2
    assert "exceeded 100.0 ms" in failures[0]
    assert "exceeded 250.0 ms" in failures[1]


@pytest.mark.parametrize("cleanup_fails", [False, True])
def test_completed_rows_collect_all_failures_without_skipping_pressure(
    monkeypatch, cleanup_fails
) -> None:
    samples = []
    resets = []
    closed = []

    def browser(endpoint, *_, **__):
        def close():
            closed.append(endpoint)
            if cleanup_fails:
                raise ui6_stop.CollectorError("browser cleanup failed")

        return SimpleNamespace(session_id=endpoint, delete_session=close)

    def stop(*_, **__):
        row = _sample(1.0)
        pressured = len(samples) >= 101
        row["kernelAcceptanceToReceiptMs"] = 400.0 if pressured else 150.0
        row["controlIngressToKernelAcceptanceMs"] = 50.0 if pressured else 1.0
        samples.append(row)
        return row

    monkeypatch.setattr(ui6_stop, "_create_browser", browser)
    monkeypatch.setattr(ui6_stop, "_one_stop", stop)
    monkeypatch.setattr(ui6_stop, "_wait_client_baseline", lambda *_: {})
    monkeypatch.setattr(ui6_stop, "_client_count", lambda *_: 0)
    monkeypatch.setattr(
        ui6_stop,
        "_wait_ready",
        lambda *_: {"qualification": {"renderer": {"software": False}}},
    )
    for name in (
        "_ensure_idle",
        "_wait_status_stable",
        "_prepare_stop_row",
        "_wait",
        "_wait_runtime_scale",
        "_report_summary",
        "_reset_performance_metrics",
        "_wait_status",
    ):
        monkeypatch.setattr(ui6_stop, name, lambda *_, **__: None)
    monkeypatch.setattr(ui6_stop, "_set_range", lambda *_: {"identityStable": True})
    monkeypatch.setattr(
        ui6_stop,
        "_resume_and_reset_during_motion",
        lambda *_: resets.append(len(samples)),
    )
    monkeypatch.setattr(ui6_stop, "_execute", lambda *_: True)
    monkeypatch.setattr(
        ui6_stop, "_performance", lambda *_: {"performance": {"modelBuilds": 1}}
    )
    monkeypatch.setattr(ui6_stop, "_viewer_status", lambda *_: {})
    monkeypatch.setattr(
        ui6_stop,
        "_HttpPressure",
        lambda *_: SimpleNamespace(
            start=lambda: None, close=lambda: {"errors": ["history unavailable"]}
        ),
    )

    result = ui6_stop.collect_ui6_stop_latency(
        "primary", "pressure", "http://ui", "http://api", require_software_renderer=True
    )
    assert len(samples) == 202
    assert resets == [101, 202]
    assert closed == ["pressure", "primary"]
    assert result["success"] is False
    assert result["claims"] == []
    assert result["baseline"]["warm"]["sampleCount"] == 100
    assert result["pressured"]["warm"]["sampleCount"] == 100
    for reason in (
        "baseline kernelAcceptanceToReceiptMs.p99",
        "pressured kernelAcceptanceToReceiptMs.p99",
        "pressured kernelAcceptanceToReceiptMs.max",
        "materially delayed Stop handoff",
        "HTTP pressure: history unavailable",
        "software WebGL",
        "static viewer geometry",
    ):
        assert any(reason in error for error in result["failures"]), reason
    assert any("cleanup:" in error for error in result["failures"]) is cleanup_fails


@pytest.mark.parametrize("success", [False, True])
def test_cli_exit_status_preserves_aggregate_failure(monkeypatch, capsys, success):
    result = {
        "success": success,
        "failures": [] if success else ["baseline", "pressured"],
    }
    monkeypatch.setattr(ui6_stop, "collect_ui6_stop_latency", lambda *_, **__: result)
    code = ui6_stop.main(
        [
            "--geckodriver-endpoint",
            "primary",
            "--pressure-geckodriver-endpoint",
            "pressure",
            "--composer-url",
            "http://ui",
            "--composer-backend-url",
            "http://api",
        ]
    )
    assert code == (0 if success else 1)
    assert json.loads(capsys.readouterr().out) == result


def test_stop_dispatch_and_presentation_wait_share_one_browser_task() -> None:
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

    observed_state, observed_trace = ui6_stop._click_and_await_stopped_presentation(
        driver
    )

    assert observed_state == state
    assert observed_trace == trace
    assert driver.calls == [
        (
            "POST",
            "/execute/async",
            {
                "script": ui6_stop.STOP_CLICK_AND_PRESENTATION_WAIT_SCRIPT,
                "args": [],
            },
        )
    ]
    script = ui6_stop.STOP_CLICK_AND_PRESENTATION_WAIT_SCRIPT
    assert "MutationObserver" in script
    assert script.index("new MutationObserver") < script.index("stop.click()")
