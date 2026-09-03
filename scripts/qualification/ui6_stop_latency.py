"""Measure repeated O1.5-UI6 Stop latency without retaining raw traces.

One browser alternates exact Resume Motion and ordinary motion Stop boundaries
inside a native Program run.  A second row adds an independently busy browser
and bounded read-only HTTP pressure.  The first sample in each row is reported
separately; the following 100 warm samples support nearest-rank p99 results.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import math
import sys
import threading
import time
from typing import Any, Callable

from current_workbench_browser_collector import (
    CollectorError,
    RawWebDriver,
    _execute,
    _url_with_opt_in,
)
from ui5_native_browser_journey import (
    DEFAULT_TIMEOUT_S,
    _assert_transition_fences,
    _click_selector,
    _dom_state,
    _set_range,
    _wait,
)
from ui6_native_flow import (
    STOP_PROBE_INSTALL_SCRIPT,
    _client_count,
    _create_browser,
    _ensure_idle,
    _flow,
    _history_sequence,
    _http_json,
    _monotonic_wall_offset_ns,
    _performance,
    _require,
    _reset_performance_metrics,
    _resume_and_reset,
    _stop_latency,
    _stop_probe,
    _wait_ready,
    _wait_client_baseline,
    _wait_running_node,
    _wait_status,
    _wait_status_stable,
    _wait_stop_terminal,
    _viewer_status,
)


RESULT_SCHEMA = "manyforge.ui6.stop-latency.v1"
WARM_SAMPLES = 100
CONTROL_SCALE = 0.01
PRESSURE_WORK_MS = 12
PRESSURE_PERIOD_MS = 40
PRESSURE_REQUEST_INTERVAL_S = 0.025
LATENCY_FIELDS = (
    "clickToDispatchMs",
    "dispatchToComposerIngressMs",
    "composerIngressToKernelAcceptanceMs",
    "kernelAcceptanceToComposerResponseMs",
    "composerResponseToBrowserMs",
    "kernelAcceptanceToResponseMs",
    "kernelAcceptanceToPhysicalSettlementMs",
    "physicalSettlementToStoppedPresentationMs",
    "clickToStoppedPresentationMs",
)

# Frozen before the repeated candidate run.  The old ten-request sample had
# Composer endpoint outliers up to 466.8 ms; UI6 must close that tail rather
# than normalize it.  Settlement and presentation have separate bounds: the
# retained operator profile intentionally commands a 1.0 s soft-on-path ramp
# inside a 1.75 s nominal terminal stage, and the UI must wait for owner-issued
# rest rather than infer it.  That physical safety profile is not an ingress
# latency regression.
WARM_BUDGETS_MS: dict[str, dict[str, float]] = {
    "clickToDispatchMs": {"p99": 5.0, "max": 20.0},
    "dispatchToComposerIngressMs": {"p99": 25.0, "max": 100.0},
    "composerIngressToKernelAcceptanceMs": {"p99": 100.0, "max": 250.0},
    "kernelAcceptanceToResponseMs": {"p99": 100.0, "max": 250.0},
    "kernelAcceptanceToPhysicalSettlementMs": {"p99": 1_500.0, "max": 1_750.0},
    "physicalSettlementToStoppedPresentationMs": {"p99": 750.0, "max": 1_000.0},
    "clickToStoppedPresentationMs": {"p99": 2_250.0, "max": 2_500.0},
}


def _progress(label: str) -> None:
    print(f"ui6-stop: {label}", file=sys.stderr, flush=True)


def _nearest_rank(values: list[float], quantile: float) -> float:
    if not values or not 0.0 < quantile <= 1.0:
        raise ValueError("nearest-rank percentile requires values and 0 < q <= 1")
    ordered = sorted(values)
    return ordered[max(0, math.ceil(quantile * len(ordered)) - 1)]


def _summarize(samples: list[dict[str, Any]]) -> dict[str, Any]:
    _require(len(samples) >= WARM_SAMPLES, "too few warm Stop samples for p99")
    metrics: dict[str, Any] = {}
    for field in LATENCY_FIELDS:
        values = [float(sample[field]) for sample in samples]
        _require(
            all(math.isfinite(value) and value >= -2.0 for value in values),
            f"Stop samples contain an invalid {field}",
        )
        maximum = max(values)
        metrics[field] = {
            "p50": _nearest_rank(values, 0.50),
            "p95": _nearest_rank(values, 0.95),
            "p99": _nearest_rank(values, 0.99),
            "max": maximum,
            "maxSample": values.index(maximum) + 1,
        }
    return {"sampleCount": len(samples), "metricsMs": metrics}


def _assert_budgets(summary: dict[str, Any], label: str) -> None:
    metrics = summary.get("metricsMs")
    _require(isinstance(metrics, dict), f"{label} Stop summary is malformed")
    for field, budgets in WARM_BUDGETS_MS.items():
        result = metrics.get(field)
        _require(isinstance(result, dict), f"{label} {field} is missing")
        for statistic, budget in budgets.items():
            value = result.get(statistic)
            _require(
                isinstance(value, (int, float)) and float(value) <= budget,
                f"{label} {field}.{statistic} exceeded {budget:.1f} ms: {value} "
                f"(warm sample {result.get('maxSample')})",
            )


def _report_summary(summary: dict[str, Any], label: str) -> None:
    """Emit bounded aggregates before a frozen-budget assertion can stop a run."""

    metrics = summary.get("metricsMs")
    _require(isinstance(metrics, dict), f"{label} Stop summary is malformed")
    compact = {
        field: {
            key: round(float(metrics[field][key]), 3)
            for key in ("p50", "p95", "p99", "max", "maxSample")
        }
        for field in LATENCY_FIELDS
    }
    _progress(
        f"{label} aggregate "
        + json.dumps(compact, separators=(",", ":"), sort_keys=True)
    )


def _assert_interference(
    baseline: dict[str, Any],
    pressured: dict[str, Any],
) -> dict[str, Any]:
    comparison: dict[str, Any] = {}
    for field in LATENCY_FIELDS:
        base = float(baseline["metricsMs"][field]["p99"])
        loaded = float(pressured["metricsMs"][field]["p99"])
        delta = loaded - base
        comparison[field] = {
            "baselineP99Ms": base,
            "pressuredP99Ms": loaded,
            "deltaMs": delta,
        }

    # Viewer/history work must not materially perturb the command handoff.
    handoff_fields = (
        "clickToDispatchMs",
        "dispatchToComposerIngressMs",
        "composerIngressToKernelAcceptanceMs",
    )
    baseline_handoff = sum(
        float(baseline["metricsMs"][field]["p99"])
        for field in handoff_fields
    )
    pressured_handoff = sum(
        float(pressured["metricsMs"][field]["p99"])
        for field in handoff_fields
    )
    allowance = max(25.0, baseline_handoff * 0.50)
    _require(
        pressured_handoff <= baseline_handoff + allowance,
        "viewer/history pressure materially delayed Stop handoff: "
        f"{baseline_handoff:.3f} -> {pressured_handoff:.3f} ms",
    )
    comparison["handoff"] = {
        "baselineP99Ms": baseline_handoff,
        "pressuredP99Ms": pressured_handoff,
        "allowedIncreaseMs": allowance,
    }
    return comparison


STOP_CLICK_SCRIPT = r"""return (() => {
  const stop = document.querySelector('.viewer-transport .transport-stop-primary');
  const probe = window.__manyforgeUi6StopProbe;
  if (!(stop instanceof HTMLButtonElement) || stop.disabled || probe === undefined) {
    return false;
  }
  probe.clickPerfMs = performance.now();
  probe.clickWallMs = Date.now();
  stop.click();
  return true;
})();"""


STOP_PRESENTATION_WAIT_SCRIPT = r"""
const done = arguments[arguments.length - 1];
const transport = document.querySelector('.viewer-transport');
let finished = false;
let timeoutId;
let observer;
const read = () => {
  const run = document.querySelector('.viewer-transport .transport-run');
  const stop = document.querySelector(
    '.viewer-transport .transport-stop-primary',
  );
  const velocity = document.querySelector(
    '.viewer-transport .transport-velocity input',
  );
  return {
    runText: run?.textContent?.trim() ?? null,
    runDisabled: run instanceof HTMLButtonElement ? run.disabled : null,
    stopText: stop?.textContent?.trim() ?? null,
    stopDisabled: stop instanceof HTMLButtonElement ? stop.disabled : null,
    velocityIdentityStable: window.__manyforgeUi5Velocity === undefined
      || window.__manyforgeUi5Velocity === velocity,
  };
};
const trace = [];
const finish = (value) => {
  if (finished) return;
  finished = true;
  window.clearTimeout(timeoutId);
  observer?.disconnect();
  done(value);
};
const inspect = () => {
  const state = read();
  const previous = trace.at(-1);
  if (previous === undefined
    || Object.keys(state).some((key) => state[key] !== previous[key])) {
    trace.push(state);
  }
  if (state.stopText === 'Stopped'
    && state.stopDisabled === true
    && state.runText === 'Resume Motion'
    && state.runDisabled === false) {
    finish({ state, trace });
  }
};
if (!(transport instanceof HTMLElement)) {
  finish({ error: 'transport unavailable', state: read(), trace });
} else {
  observer = new MutationObserver(inspect);
  observer.observe(transport, {
    attributes: true,
    attributeFilter: ['disabled'],
    characterData: true,
    childList: true,
    subtree: true,
  });
  timeoutId = window.setTimeout(() => {
    finish({ error: 'timed out', state: read(), trace });
  }, 4000);
  inspect();
}
"""


def _await_stopped_presentation(
    driver: RawWebDriver,
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    """Observe the transition in-page without perturbing Firefox at 20 Hz."""

    value = driver.command(
        "POST",
        "/execute/async",
        {"script": STOP_PRESENTATION_WAIT_SCRIPT, "args": []},
    )
    if not isinstance(value, dict):
        raise CollectorError("Stopped presentation wait returned an invalid result")
    state = value.get("state")
    trace = value.get("trace")
    error = value.get("error")
    if error is not None:
        raise CollectorError(
            f"Stopped presentation wait failed: {error}: "
            f"{json.dumps(state, sort_keys=True)}"
        )
    if not isinstance(state, dict) or not isinstance(trace, list) or not trace:
        raise CollectorError("Stopped presentation wait returned no transition trace")
    if any(not isinstance(row, dict) for row in trace):
        raise CollectorError("Stopped presentation trace is malformed")
    return state, trace


def _one_stop(
    driver: RawWebDriver,
    composer_url: str,
    *,
    start_label: str,
) -> dict[str, Any]:
    current, _ = _wait(
        lambda: _dom_state(driver),
        lambda state: (
            state["runText"] == start_label
            and state["runDisabled"] is False
            and state["stopDisabled"] is True
        ),
        f"{start_label} before repeated Stop",
        DEFAULT_TIMEOUT_S,
    )
    _require(current["velocityValue"] == str(CONTROL_SCALE), "runtime scale drifted")
    before_flow = _flow(driver)
    before_frame = before_flow.get("lastApplied")
    _require(isinstance(before_frame, dict), "pre-Stop frame identity is missing")
    before_sequence = before_frame.get("sourceSequence")
    _require(type(before_sequence) is int, "pre-Stop source sequence is malformed")
    before_history = _history_sequence(composer_url)
    _require(
        _execute(driver, STOP_PROBE_INSTALL_SCRIPT) is True,
        "Stop probe could not be installed",
    )

    _click_selector(driver, ".transport-run", start_label)
    _running, run_trace = _wait(
        lambda: _dom_state(driver),
        lambda state: (
            state["runText"] == "Running"
            and state["runDisabled"] is True
            and state["stopDisabled"] is False
        ),
        f"Running after {start_label}",
        DEFAULT_TIMEOUT_S,
    )
    _assert_transition_fences(run_trace, f"UI6 repeated {start_label}")
    _wait_running_node(composer_url)
    _moved, _ = _wait(
        lambda: _flow(driver),
        lambda state: (
            isinstance(state.get("lastApplied"), dict)
            and state["lastApplied"].get("sourceSequence", -1) > before_sequence
        ),
        f"new applied motion frame after {start_label}",
        DEFAULT_TIMEOUT_S,
    )

    _clicked, _ = _wait(
        lambda: _execute(driver, STOP_CLICK_SCRIPT),
        lambda clicked: clicked is True,
        "eligible Stop click after observed motion",
        DEFAULT_TIMEOUT_S,
    )
    _stopped, stop_trace = _await_stopped_presentation(driver)
    _assert_transition_fences(stop_trace, "UI6 repeated Stop")
    _require(
        any(state["stopText"] == "Stopping" for state in stop_trace),
        "repeated Stop never presented Stopping",
    )
    terminal = _wait_stop_terminal(composer_url, before_history)
    return _stop_latency(_stop_probe(driver), terminal, _monotonic_wall_offset_ns())


def _stop_row(
    driver: RawWebDriver,
    composer_url: str,
    *,
    label: str,
    warm_samples: int,
) -> dict[str, Any]:
    _require(warm_samples >= WARM_SAMPLES, "p99 row requires at least 100 samples")
    scaled = _set_range(driver, CONTROL_SCALE)
    _require(
        scaled["identityStable"] and scaled["focused"],
        f"{label} runtime scaling lost pointer identity/focus",
    )
    _wait(
        lambda: _dom_state(driver),
        lambda state: state["velocityValue"] == str(CONTROL_SCALE),
        f"{label} runtime scale",
        DEFAULT_TIMEOUT_S,
    )

    cold = _one_stop(driver, composer_url, start_label="Run")
    warm: list[dict[str, Any]] = []
    for index in range(warm_samples):
        warm.append(
            _one_stop(driver, composer_url, start_label="Resume Motion")
        )
        if (index + 1) % 10 == 0:
            _progress(f"{label}: {index + 1}/{warm_samples} warm Stops")

    restored = _set_range(driver, 1.0)
    _require(restored["identityStable"], f"{label} scale restoration rebound control")
    _resume_and_reset(driver)
    summary = _summarize(warm)
    _report_summary(summary, label)
    _assert_budgets(summary, label)
    return {"cold": cold, "warm": summary}


PRESSURE_SCRIPT = f"""return (() => {{
  if (window.__manyforgeUi6Pressure !== undefined) return false;
  window.__manyforgeUi6Pressure = window.setInterval(() => {{
    const deadline = performance.now() + {PRESSURE_WORK_MS};
    while (performance.now() < deadline) {{ /* bounded UI6 CPU pressure */ }}
  }}, {PRESSURE_PERIOD_MS});
  return true;
}})();"""


CLEAR_PRESSURE_SCRIPT = r"""return (() => {
  if (window.__manyforgeUi6Pressure === undefined) return false;
  window.clearInterval(window.__manyforgeUi6Pressure);
  delete window.__manyforgeUi6Pressure;
  return true;
})();"""


@dataclass
class _HttpPressure:
    composer_url: str
    composer_backend_url: str
    workers: int

    def __post_init__(self) -> None:
        self.stop = threading.Event()
        self.threads: list[threading.Thread] = []
        self.lock = threading.Lock()
        self.requests = 0
        self.errors: list[str] = []

    def start(self) -> None:
        def work(index: int) -> None:
            reads: tuple[Callable[[], dict[str, Any]], ...] = (
                lambda: _http_json(
                    self.composer_url,
                    "/api/execution/history",
                    params=[("limit", 32)],
                ),
                lambda: _http_json(
                    self.composer_backend_url,
                    "/api/scene/viewer/status",
                ),
                lambda: _http_json(
                    self.composer_url,
                    "/api/component-overview",
                ),
            )
            position = index % len(reads)
            while not self.stop.is_set():
                try:
                    reads[position]()
                    with self.lock:
                        self.requests += 1
                except CollectorError as error:
                    with self.lock:
                        if len(self.errors) < 16:
                            self.errors.append(str(error))
                position = (position + 1) % len(reads)
                self.stop.wait(PRESSURE_REQUEST_INTERVAL_S)

        for index in range(self.workers):
            thread = threading.Thread(
                target=work,
                args=(index,),
                name=f"ui6-read-pressure-{index}",
            )
            thread.start()
            self.threads.append(thread)

    def close(self) -> dict[str, Any]:
        self.stop.set()
        for thread in self.threads:
            thread.join(timeout=10.0)
        _require(
            all(not thread.is_alive() for thread in self.threads),
            "HTTP pressure worker did not stop",
        )
        with self.lock:
            result = {
                "workers": self.workers,
                "requestCount": self.requests,
                "errors": list(self.errors),
            }
        _require(not result["errors"], "HTTP pressure produced request errors")
        return result


def collect_ui6_stop_latency(
    geckodriver_endpoint: str,
    pressure_geckodriver_endpoint: str,
    composer_url: str,
    composer_backend_url: str,
    *,
    warm_samples: int = WARM_SAMPLES,
    pressure_workers: int = 2,
    viewport_width: int = 1440,
    viewport_height: int = 960,
    require_software_renderer: bool = False,
) -> dict[str, Any]:
    _require(pressure_workers >= 1, "at least one HTTP pressure worker is required")
    target_url = _url_with_opt_in(composer_url)
    baseline_status = _wait_client_baseline(composer_backend_url)
    baseline_clients = _client_count(baseline_status)
    primary: RawWebDriver | None = None
    pressure_browser: RawWebDriver | None = None
    pressure: _HttpPressure | None = None
    pressure_result: dict[str, Any] | None = None
    try:
        primary = _create_browser(
            geckodriver_endpoint,
            target_url,
            width=viewport_width,
            height=viewport_height,
            isolate_firefox=require_software_renderer,
        )
        ready = _wait_ready(primary, "UI6 repeated-Stop browser")
        _ensure_idle(primary)
        _wait_status_stable(
            composer_backend_url,
            lambda status: _client_count(status) == baseline_clients + 1,
            "repeated-Stop browser demand",
        )
        _progress("baseline repeated-Stop row")
        baseline = _stop_row(
            primary,
            composer_url,
            label="baseline",
            warm_samples=warm_samples,
        )

        pressure_browser = _create_browser(
            pressure_geckodriver_endpoint,
            target_url,
            width=viewport_width,
            height=viewport_height,
            isolate_firefox=require_software_renderer,
        )
        pressure_ready = _wait_ready(pressure_browser, "UI6 pressure browser")
        _wait_status_stable(
            composer_backend_url,
            lambda status: _client_count(status) == baseline_clients + 2,
            "two-browser repeated-Stop pressure",
        )
        _require(
            _execute(pressure_browser, PRESSURE_SCRIPT) is True,
            "browser CPU pressure could not be installed",
        )
        pressure = _HttpPressure(
            composer_url,
            composer_backend_url,
            pressure_workers,
        )
        pressure.start()
        _progress("pressured repeated-Stop row")
        _reset_performance_metrics(primary)
        before_performance = _performance(primary)
        loaded = _stop_row(
            primary,
            composer_url,
            label="pressured",
            warm_samples=warm_samples,
        )
        after_performance = _performance(primary)
        pressure_result = pressure.close()
        pressure = None
        _require(
            _execute(pressure_browser, CLEAR_PRESSURE_SCRIPT) is True,
            "browser CPU pressure could not be cleared",
        )
        comparison = _assert_interference(baseline["warm"], loaded["warm"])
        renderer = ready["qualification"]["renderer"]
        _require(
            not require_software_renderer
            or (
                renderer["software"] is True
                and pressure_ready["qualification"]["renderer"]["software"] is True
            ),
            "forced-software Stop row did not use software WebGL in both browsers",
        )
        _require(
            before_performance["performance"]["modelBuilds"] == 0
            and after_performance["performance"]["modelBuilds"] == 0,
            "repeated Stop pressure rebuilt static viewer geometry",
        )
        return {
            "schemaVersion": RESULT_SCHEMA,
            "success": True,
            "scope": "O1.5-UI6 repeated native/browser Stop latency",
            "warmSamplesPerRow": warm_samples,
            "frozenBudgetsMs": WARM_BUDGETS_MS,
            "renderer": renderer,
            "pressureRenderer": pressure_ready["qualification"]["renderer"],
            "baseline": baseline,
            "pressured": loaded,
            "interference": comparison,
            "pressure": {
                "browserWorkMs": PRESSURE_WORK_MS,
                "browserPeriodMs": PRESSURE_PERIOD_MS,
                "http": pressure_result,
            },
            "finalViewerStatus": _viewer_status(composer_backend_url),
            "claims": [
                "each warm p99 is supported by at least 100 exact native Stops",
                "cold samples are excluded from warm percentiles",
                "browser dispatch, Composer ingress, Kernel acceptance, physical settlement, and Stopped presentation are separately timed",
                "one independently busy browser and bounded history/status pressure do not materially delay the Stop handoff",
                "the native cycle completes and resets after each row",
            ],
        }
    finally:
        active_error = sys.exc_info()[1]
        cleanup_errors: list[str] = []
        if pressure is not None:
            try:
                pressure.close()
            except CollectorError as error:
                cleanup_errors.append(str(error))
        for driver in (pressure_browser, primary):
            if driver is not None and driver.session_id is not None:
                try:
                    driver.timeout_s = 30.0
                    driver.delete_session()
                except CollectorError as error:
                    cleanup_errors.append(str(error))
        try:
            _wait_status(
                composer_backend_url,
                lambda status: _client_count(status) <= baseline_clients,
                "repeated-Stop client cleanup",
            )
        except CollectorError as error:
            cleanup_errors.append(str(error))
        if active_error is None and cleanup_errors:
            raise CollectorError("; ".join(cleanup_errors))


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--geckodriver-endpoint", required=True)
    parser.add_argument("--pressure-geckodriver-endpoint", required=True)
    parser.add_argument("--composer-url", required=True)
    parser.add_argument("--composer-backend-url", required=True)
    parser.add_argument("--warm-samples", type=int, default=WARM_SAMPLES)
    parser.add_argument("--pressure-workers", type=int, default=2)
    parser.add_argument("--viewport-width", type=int, default=1440)
    parser.add_argument("--viewport-height", type=int, default=960)
    parser.add_argument("--require-software-renderer", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    try:
        result = collect_ui6_stop_latency(
            arguments.geckodriver_endpoint,
            arguments.pressure_geckodriver_endpoint,
            arguments.composer_url,
            arguments.composer_backend_url,
            warm_samples=arguments.warm_samples,
            pressure_workers=arguments.pressure_workers,
            viewport_width=arguments.viewport_width,
            viewport_height=arguments.viewport_height,
            require_software_renderer=arguments.require_software_renderer,
        )
    except CollectorError as error:
        print(json.dumps({
            "schemaVersion": RESULT_SCHEMA,
            "success": False,
            "error": str(error),
        }, separators=(",", ":"), sort_keys=True))
        return 1
    print(json.dumps(result, separators=(",", ":"), sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
