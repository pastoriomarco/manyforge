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
    _resume_and_reset_during_motion,
    _stop_latency,
    _stop_probe,
    _wait_ready,
    _wait_client_baseline,
    _wait_runtime_scale,
    _wait_running_node,
    _wait_status,
    _wait_status_stable,
    _wait_stop_terminal,
    _viewer_status,
)


RESULT_SCHEMA = "manyforge.ui6.stop-latency.v1"
WARM_SAMPLES = 100
CONTROL_SCALE = 0.01
MIN_MOTION_FRAME_ADVANCE = 3
PRESSURE_WORK_MS = 12
PRESSURE_PERIOD_MS = 40
PRESSURE_REQUEST_INTERVAL_S = 0.025
LATENCY_FIELDS = (
    "clickToDispatchMs",
    "dispatchToControlIngressMs",
    "controlIngressToKernelAcceptanceMs",
    "kernelAcceptanceToReceiptEmissionMs",
    "receiptEmissionToBrowserMs",
    "kernelAcceptanceToReceiptMs",
    "kernelAcceptanceToStoppingPresentationMs",
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
    "dispatchToControlIngressMs": {"p99": 25.0, "max": 100.0},
    "controlIngressToKernelAcceptanceMs": {"p99": 100.0, "max": 250.0},
    "kernelAcceptanceToReceiptMs": {"p99": 100.0, "max": 250.0},
    "kernelAcceptanceToStoppingPresentationMs": {"p99": 250.0, "max": 500.0},
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


def _budget_failures(summary: dict[str, Any], label: str) -> list[str]:
    metrics = summary.get("metricsMs")
    _require(isinstance(metrics, dict), f"{label} Stop summary is malformed")
    failures: list[str] = []
    for field, budgets in WARM_BUDGETS_MS.items():
        result = metrics.get(field)
        _require(isinstance(result, dict), f"{label} {field} is missing")
        for statistic, budget in budgets.items():
            value = result.get(statistic)
            if not isinstance(value, (int, float)) or not float(value) <= budget:
                failures.append(
                    f"{label} {field}.{statistic} exceeded {budget:.1f} ms: {value} "
                    f"(warm sample {result.get('maxSample')})"
                )
    return failures


def _report_summary(summary: dict[str, Any], label: str) -> None:
    """Emit bounded aggregates while independent workload rows continue."""

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


def _compare_interference(
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
        "dispatchToControlIngressMs",
        "controlIngressToKernelAcceptanceMs",
    )
    baseline_handoff = sum(
        float(baseline["metricsMs"][field]["p99"]) for field in handoff_fields
    )
    pressured_handoff = sum(
        float(pressured["metricsMs"][field]["p99"]) for field in handoff_fields
    )
    allowance = max(25.0, baseline_handoff * 0.50)
    comparison["handoff"] = {
        "baselineP99Ms": baseline_handoff,
        "pressuredP99Ms": pressured_handoff,
        "allowedIncreaseMs": allowance,
        "passed": pressured_handoff <= baseline_handoff + allowance,
    }
    return comparison


STOP_CLICK_AND_PRESENTATION_WAIT_SCRIPT = r"""
const done = arguments[arguments.length - 1];
const transport = document.querySelector('.viewer-transport');
const stop = document.querySelector(
  '.viewer-transport .transport-stop-primary',
);
const probe = window.__manyforgeUi6StopProbe;
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
if (!(transport instanceof HTMLElement)
  || !(stop instanceof HTMLButtonElement)
  || stop.disabled
  || probe === undefined) {
  finish({ error: 'eligible Stop unavailable', state: read(), trace });
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
  probe.clickPerfMs = performance.now();
  probe.clickWallMs = Date.now();
  stop.click();
  inspect();
}
"""


def _click_and_await_stopped_presentation(
    driver: RawWebDriver,
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    """Dispatch and observe Stop in one browser task without remote-command churn."""

    value = driver.command(
        "POST",
        "/execute/async",
        {"script": STOP_CLICK_AND_PRESENTATION_WAIT_SCRIPT, "args": []},
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
    running = _wait_running_node(composer_url, "move_to_rest")
    _wait_history_event_after(
        composer_url,
        "operation_executing",
        before_history,
        program_run_id=running["capture"]["program_run_id"],
        node_instance_id="move_to_rest",
    )
    before_sequence = _flow(driver)["lastApplied"]["sourceSequence"]
    # Do not sample the controller's first acceleration instant.  The latency
    # row measures an in-flight Stop, while a Stop issued on the first sample
    # can legitimately exercise the separately-qualified hard-stop fallback.
    # Three source revisions are still only a bounded, sub-frame-rate setup
    # guard and are outside every timed Stop interval below.
    _moved, _ = _wait(
        lambda: _flow(driver),
        lambda state: (
            isinstance(state.get("lastApplied"), dict)
            and state["lastApplied"].get("sourceSequence", -1)
            >= before_sequence + MIN_MOTION_FRAME_ADVANCE
        ),
        f"new applied motion frame after {start_label}",
        DEFAULT_TIMEOUT_S,
    )

    _stopped, stop_trace = _click_and_await_stopped_presentation(driver)
    _assert_transition_fences(stop_trace, "UI6 repeated Stop")
    terminal = _wait_stop_terminal(composer_url, before_history)
    if not any(state["stopText"] == "Stopping" for state in stop_trace):
        print(
            json.dumps(
                {
                    "failedStopTrace": stop_trace[:32],
                    "traceTruncated": len(stop_trace) > 32,
                    "probe": _stop_probe(driver),
                    "terminal": terminal,
                },
                sort_keys=True,
            ),
            file=sys.stderr,
            flush=True,
        )
    _require(
        any(state["stopText"] == "Stopping" for state in stop_trace),
        "repeated Stop never presented Stopping",
    )
    return _stop_latency(_stop_probe(driver), terminal, _monotonic_wall_offset_ns())


def _wait_history_event_after(
    composer_url: str,
    event_type: str,
    after_sequence: int,
    timeout_s: float = DEFAULT_TIMEOUT_S,
    *,
    program_run_id: str,
    node_instance_id: str,
) -> dict[str, Any]:
    """Wait for owner evidence that the operation crossed the named boundary."""

    deadline = time.monotonic() + timeout_s
    while True:
        page = _http_json(
            composer_url,
            "/api/execution/history",
            params=[
                ("event_types", event_type),
                ("limit", 32),
                ("program_run_id", program_run_id),
                ("node_instance_id", node_instance_id),
            ],
        )
        events = page.get("events")
        if not isinstance(events, list):
            raise CollectorError("historian operation page is malformed")
        for row in events:
            if not isinstance(row, dict):
                continue
            sequence = row.get("sequence")
            summary = row.get("event")
            if (
                type(sequence) is int
                and sequence > after_sequence
                and isinstance(summary, dict)
                and summary.get("event_type") == event_type
            ):
                return row
        if time.monotonic() >= deadline:
            raise CollectorError(
                f"timed out waiting for {event_type} after sequence {after_sequence}"
            )
        time.sleep(0.02)


def _prepare_stop_row(
    driver: RawWebDriver,
    composer_url: str,
    *,
    label: str,
) -> None:
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
    _wait_runtime_scale(composer_url, CONTROL_SCALE)


def _stop_row(
    driver: RawWebDriver,
    composer_url: str,
    *,
    label: str,
    warm_samples: int,
) -> dict[str, Any]:
    _require(warm_samples >= WARM_SAMPLES, "p99 row requires at least 100 samples")
    _prepare_stop_row(driver, composer_url, label=label)

    cold = _one_stop(driver, composer_url, start_label="Run")
    warm: list[dict[str, Any]] = []
    for index in range(warm_samples):
        warm.append(_one_stop(driver, composer_url, start_label="Resume Motion"))
        if (index + 1) % 10 == 0:
            _progress(f"{label}: {index + 1}/{warm_samples} warm Stops")

    _wait(
        lambda: _dom_state(driver),
        lambda state: state["velocityDisabled"] is False,
        f"{label} post-Stop runtime velocity availability",
        DEFAULT_TIMEOUT_S,
    )
    restored = _set_range(driver, 1.0)
    _require(restored["identityStable"], f"{label} scale restoration rebound control")
    _wait_runtime_scale(composer_url, 1.0)
    summary = _summarize(warm)
    _report_summary(summary, label)
    _resume_and_reset_during_motion(driver)
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
    diagnostic_samples: int = 0,
) -> dict[str, Any]:
    _require(0 <= diagnostic_samples <= 10, "diagnostic sample count must be 0..10")
    _require(pressure_workers >= 1, "at least one HTTP pressure worker is required")
    target_url = _url_with_opt_in(composer_url)
    baseline_status = _wait_client_baseline(composer_backend_url)
    baseline_clients = _client_count(baseline_status)
    primary: RawWebDriver | None = None
    pressure_browser: RawWebDriver | None = None
    pressure: _HttpPressure | None = None
    pressure_result: dict[str, Any] | None = None
    result: dict[str, Any] | None = None
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
        if diagnostic_samples:
            _execute(primary, "window.__manyforgeUi6Diagnose = true; return true;")
            _prepare_stop_row(primary, composer_url, label="diagnostic")
            _execute(primary, "performance.clearResourceTimings(); return true;")
            samples = []
            for index in range(diagnostic_samples):
                samples.append(
                    {
                        "timing": _one_stop(
                            primary,
                            composer_url,
                            start_label="Run" if index == 0 else "Resume Motion",
                        ),
                        "probe": _stop_probe(primary),
                    }
                )
            availability_reads = lambda: _execute(
                primary,
                """
                return performance.getEntriesByType('resource').filter(
                  entry => new URL(entry.name).pathname === '/api/execution/manual-handover'
                ).length;
            """,
            )
            _require(
                availability_reads() == 0,
                "closed Robot panel queried manual availability",
            )
            _click_selector(
                primary, ".viewer-surface-launchers button:first-child", "Robot panel"
            )
            _wait(
                lambda: _execute(
                    primary,
                    """
                return {available: document.querySelector(
                  '.robot-control-number input[aria-label$="target degrees"]'
                )?.disabled === false};
            """,
                ),
                lambda state: state["available"],
                "held manual panel availability",
                DEFAULT_TIMEOUT_S,
            )
            _require(
                availability_reads() > 0, "opened Robot panel did not query its owner"
            )
            _click_selector(
                primary, '[aria-label="Close viewer inspector"]', "close Robot panel"
            )
            return {
                "diagnosticOnly": True,
                "qualificationPassed": False,
                "manualAvailabilityDemandChecked": True,
                "samples": samples,
                "viewer": _performance(primary),
                "flow": _flow(primary),
            }
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
        comparison = _compare_interference(baseline["warm"], loaded["warm"])
        failures = _budget_failures(baseline["warm"], "baseline")
        failures.extend(_budget_failures(loaded["warm"], "pressured"))
        if not comparison["handoff"]["passed"]:
            failures.append(
                "viewer/history pressure materially delayed Stop handoff: "
                f"{comparison['handoff']['baselineP99Ms']:.3f} -> "
                f"{comparison['handoff']['pressuredP99Ms']:.3f} ms"
            )
        failures.extend(
            f"HTTP pressure: {error}" for error in pressure_result["errors"]
        )
        renderer = ready["qualification"]["renderer"]
        if require_software_renderer and not (
            renderer["software"] is True
            and pressure_ready["qualification"]["renderer"]["software"] is True
        ):
            failures.append(
                "forced-software Stop row did not use software WebGL in both browsers"
            )
        if (
            before_performance["performance"]["modelBuilds"] != 0
            or after_performance["performance"]["modelBuilds"] != 0
        ):
            failures.append("repeated Stop pressure rebuilt static viewer geometry")
        result = {
            "schemaVersion": RESULT_SCHEMA,
            "success": not failures,
            "failures": failures,
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
            "claims": (
                []
                if failures
                else [
                    "each warm p99 is supported by at least 100 exact native Stops",
                    "cold samples are excluded from warm percentiles",
                    "browser dispatch, Composer ingress, Kernel acceptance, physical settlement, and Stopped presentation are separately timed",
                    "one independently busy browser and bounded history/status pressure do not materially delay the Stop handoff",
                    "the native cycle completes and resets after each row",
                ]
            ),
        }
        return result
    finally:
        active_error = sys.exc_info()[1]
        cleanup_errors: list[str] = []
        if pressure is not None:
            try:
                pressure.close()
            except CollectorError as error:
                cleanup_errors.append(str(error))
        if active_error is not None:
            # Capture current evidence before disposable owners and browsers
            # disappear. Failure-only reads cannot perturb measured samples.
            for path in (
                "/api/program/runs?active_only=true&limit=1",
                "/api/component-overview",
                "/api/execution/history?limit=16",
            ):
                try:
                    active_error.add_note(
                        f"Stop failure {path}: "
                        + json.dumps(_http_json(composer_url, path), sort_keys=True)
                    )
                except CollectorError as error:
                    active_error.add_note(f"Stop failure {path}: {error}")
            if primary is not None and primary.session_id is not None:
                try:
                    _execute(
                        primary,
                        "document.querySelector('[aria-label=\"Expand runtime details\"]')?.click(); return true;",
                    )
                    active_error.add_note(
                        "Stop failure attention: "
                        + json.dumps(_dom_state(primary), sort_keys=True)
                    )
                except CollectorError as error:
                    active_error.add_note(f"Stop failure attention: {error}")
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
        if cleanup_errors:
            if active_error is not None:
                active_error.add_note("Stop cleanup: " + "; ".join(cleanup_errors))
            elif result is not None:
                result["failures"].extend(
                    f"cleanup: {error}" for error in cleanup_errors
                )
                result["success"] = False
                result["claims"] = []
            else:
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
        print(
            json.dumps(
                {
                    "schemaVersion": RESULT_SCHEMA,
                    "success": False,
                    "error": str(error),
                },
                separators=(",", ":"),
                sort_keys=True,
            )
        )
        return 1
    print(json.dumps(result, separators=(",", ":"), sort_keys=True))
    return 0 if result["success"] else 1


if __name__ == "__main__":
    sys.exit(main())
