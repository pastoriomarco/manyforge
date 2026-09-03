"""Qualify O1.5-UI6's bounded viewer flow and priority Stop path.

The caller owns one native workbench and two geckodriver processes.  This
collector creates isolated Firefox sessions, records compact aggregate facts,
and deletes only those sessions.  It never writes Program or Deployment state.
"""
from __future__ import annotations

import argparse
import json
import math
import sys
import threading
import time
from typing import Any, Callable
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode, urljoin
from urllib.request import Request, urlopen

from current_workbench_browser_collector import (
    CollectorError,
    QUALIFICATION_GLOBAL,
    RawWebDriver,
    _execute,
    _url_with_opt_in,
)
from ui5_native_browser_journey import (
    DEFAULT_TIMEOUT_S,
    TERMINAL_TIMEOUT_S,
    _assert_transition_fences,
    _click_selector,
    _dom_state,
    _set_range,
    _wait,
)


RESULT_SCHEMA = "manyforge.ui6.native-flow.v1"
FLOW_SCHEMA_KEYS = {
    "appliedFrames",
    "applyIntervalMaxMs",
    "applyIntervalP95Ms",
    "lastApplied",
    "lastReceived",
    "maxUnappliedFrames",
    "receivedFrames",
    "receiveToApplyMaxMs",
    "receiveToApplyP95Ms",
    "replacedBeforeApply",
}
STATIC_WINDOW_SECONDS = 2.0
FROZEN_LEASE_SECONDS = 45.0
FROZEN_WINDOW_SECONDS = 55.0
POLL_SECONDS = 0.05


def _progress(label: str) -> None:
    print(f"ui6: {label}", file=sys.stderr, flush=True)


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise CollectorError(message)


def _http_json(
    base_url: str,
    path: str,
    *,
    params: list[tuple[str, str | int]] | None = None,
) -> dict[str, Any]:
    target = urljoin(base_url.rstrip("/") + "/", path.lstrip("/"))
    if params:
        target = f"{target}?{urlencode(params)}"
    request = Request(target, headers={"Accept": "application/json"})
    try:
        with urlopen(request, timeout=5.0) as response:
            raw = response.read(4_000_001)
    except (HTTPError, URLError, TimeoutError, OSError) as error:
        raise CollectorError(f"HTTP read failed for {path}") from error
    if len(raw) > 4_000_000:
        raise CollectorError(f"HTTP response for {path} is oversized")
    try:
        value = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CollectorError(f"HTTP response for {path} is malformed") from error
    if not isinstance(value, dict):
        raise CollectorError(f"HTTP response for {path} is not an object")
    return value


def _viewer_status(composer_backend_url: str) -> dict[str, Any]:
    value = _http_json(composer_backend_url, "/api/scene/viewer/status")
    source = value.get("source")
    fanout = value.get("fanout")
    if (
        value.get("schema_version")
        != "manyforge.execution_observation_stream_status.v2"
        or not isinstance(source, dict)
        or not isinstance(fanout, dict)
    ):
        raise CollectorError("viewer source status is malformed")
    return value


def _count(status: dict[str, Any], group: str, key: str) -> int:
    value = status[group].get(key)
    if type(value) is not int or value < 0:
        raise CollectorError(f"viewer status {group}.{key} is malformed")
    return value


def _counter_delta(
    before: dict[str, Any],
    after: dict[str, Any],
) -> dict[str, int]:
    keys = {
        "sourceConnections": ("source", "connection_count"),
        "sourceWakes": ("source", "wake_count"),
        "published": ("source", "published_count"),
        "sourceErrors": ("source", "error_count"),
        "credits": ("fanout", "credit_count"),
        "sent": ("fanout", "sent_count"),
        "coalesced": ("fanout", "coalesced_frames_total"),
    }
    result: dict[str, int] = {}
    for label, (group, key) in keys.items():
        left = _count(before, group, key)
        right = _count(after, group, key)
        _require(right >= left, f"viewer counter {group}.{key} regressed")
        result[label] = right - left
    return result


def _client_count(status: dict[str, Any]) -> int:
    return _count(status, "fanout", "client_count")


def _wait_status(
    composer_backend_url: str,
    predicate: Callable[[dict[str, Any]], bool],
    label: str,
    timeout_s: float = DEFAULT_TIMEOUT_S,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_s
    while True:
        current = _viewer_status(composer_backend_url)
        if predicate(current):
            return current
        if time.monotonic() >= deadline:
            raise CollectorError(
                f"timed out waiting for {label}: "
                f"{json.dumps(current, separators=(',', ':'), sort_keys=True)}"
            )
        time.sleep(POLL_SECONDS)


def _wait_status_stable(
    composer_backend_url: str,
    predicate: Callable[[dict[str, Any]], bool],
    label: str,
    *,
    stable_s: float = 0.75,
    timeout_s: float = DEFAULT_TIMEOUT_S,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_s
    matched_since: float | None = None
    current: dict[str, Any] = {}
    while True:
        current = _viewer_status(composer_backend_url)
        now = time.monotonic()
        if predicate(current):
            matched_since = now if matched_since is None else matched_since
            if now - matched_since >= stable_s:
                return current
        else:
            matched_since = None
        if now >= deadline:
            raise CollectorError(
                f"timed out waiting for stable {label}: "
                f"{json.dumps(current, separators=(',', ':'), sort_keys=True)}"
            )
        time.sleep(POLL_SECONDS)


def _wait_client_baseline(
    composer_backend_url: str,
    *,
    stable_s: float = 0.75,
    timeout_s: float = DEFAULT_TIMEOUT_S,
) -> dict[str, Any]:
    """Capture ambient demand only after its aggregate count has settled."""

    deadline = time.monotonic() + timeout_s
    observed_count: int | None = None
    unchanged_since: float | None = None
    current: dict[str, Any] = {}
    while True:
        current = _viewer_status(composer_backend_url)
        count = _client_count(current)
        now = time.monotonic()
        if count != observed_count:
            observed_count = count
            unchanged_since = now
        elif unchanged_since is not None and now - unchanged_since >= stable_s:
            return current
        if now >= deadline:
            raise CollectorError(
                "timed out waiting for stable ambient viewer demand: "
                f"{json.dumps(current, separators=(',', ':'), sort_keys=True)}"
            )
        time.sleep(POLL_SECONDS)


def _flow(driver: RawWebDriver) -> dict[str, Any]:
    value = _execute(
        driver,
        f"return window.{QUALIFICATION_GLOBAL}?.observationFlow?.() ?? null;",
    )
    if not isinstance(value, dict) or set(value) != FLOW_SCHEMA_KEYS:
        raise CollectorError("browser observation-flow diagnostics are malformed")
    for key in (
        "appliedFrames",
        "maxUnappliedFrames",
        "receivedFrames",
        "replacedBeforeApply",
    ):
        if type(value[key]) is not int or value[key] < 0:
            raise CollectorError(f"browser observation-flow {key} is malformed")
    return value


def _reset_browser_metrics(driver: RawWebDriver) -> None:
    result = _execute(
        driver,
        f"""return (() => {{
          const q = window.{QUALIFICATION_GLOBAL};
          if (q === undefined) return false;
          q.reset();
          q.resetObservationFlow();
          return true;
        }})();""",
    )
    _require(result is True, "browser qualification metrics are unavailable")


def _reset_performance_metrics(driver: RawWebDriver) -> None:
    result = _execute(
        driver,
        f"""return (() => {{
          const q = window.{QUALIFICATION_GLOBAL};
          if (q === undefined) return false;
          q.reset();
          return true;
        }})();""",
    )
    _require(result is True, "browser performance metrics are unavailable")


def _performance(driver: RawWebDriver) -> dict[str, Any]:
    value = _execute(
        driver,
        f"return window.{QUALIFICATION_GLOBAL}?.take?.() ?? null;",
    )
    if not isinstance(value, dict) or not isinstance(value.get("performance"), dict):
        raise CollectorError("browser performance diagnostics are malformed")
    return value


def _qualification_status(driver: RawWebDriver) -> dict[str, Any]:
    value = _execute(
        driver,
        f"return window.{QUALIFICATION_GLOBAL}?.status?.() ?? null;",
    )
    if not isinstance(value, dict):
        raise CollectorError("browser qualification status is malformed")
    return value


def _wait_ready(driver: RawWebDriver, label: str) -> dict[str, Any]:
    ready, _trace = _wait(
        lambda: _dom_state(driver),
        lambda state: (
            state["deploymentConnected"]
            and state["canvasCount"] == 1
            and state["socketCount"] == 1
            and state["scenePresentation"] == "owner"
            and state["runText"] is not None
        ),
        label,
        DEFAULT_TIMEOUT_S,
    )
    qualified, _ = _wait(
        lambda: _qualification_status(driver),
        lambda state: (
            isinstance(state.get("model"), dict)
            and state["model"].get("ready") is True
            and isinstance(state.get("renderer"), dict)
            and not state.get("errors")
        ),
        f"{label} hydrated model and renderer",
        DEFAULT_TIMEOUT_S,
    )
    flow, _ = _wait(
        lambda: _flow(driver),
        lambda state: state["lastApplied"] is not None,
        f"{label} current frame application",
        DEFAULT_TIMEOUT_S,
    )
    ready["flow"] = flow
    ready["qualification"] = qualified
    return ready


def _create_browser(
    endpoint: str,
    target_url: str,
    *,
    width: int,
    height: int,
    isolate_firefox: bool,
) -> RawWebDriver:
    driver = RawWebDriver(endpoint, timeout_s=5.0)
    firefox_args = ("-headless", "-no-remote") if isolate_firefox else ("-headless",)
    driver.create_session(firefox_args=firefox_args)
    driver.set_window_rect(width, height)
    driver.command("POST", "/url", {"url": target_url})
    return driver


def _wait_slow_ready(
    driver: RawWebDriver,
    *,
    timeout_s: float = 120.0,
) -> dict[str, Any]:
    previous_timeout = driver.timeout_s
    driver.timeout_s = min(90.0, timeout_s)
    try:
        return _wait_ready(driver, "slow UI6 browser")
    finally:
        driver.timeout_s = previous_timeout


def _start_slow_freeze(
    driver: RawWebDriver,
    seconds: float,
) -> tuple[threading.Thread, dict[str, Any], float]:
    duration_ms = round(seconds * 1_000)
    outcome: dict[str, Any] = {}

    def freeze() -> None:
        try:
            outcome["browser"] = _execute(
                driver,
                f"""return (() => {{
                  const startedWallMs = Date.now();
                  const startedPerfMs = performance.now();
                  const deadline = startedPerfMs + {duration_ms};
                  while (performance.now() < deadline) {{ /* deliberate UI6 stall */ }}
                  return {{
                    startedWallMs,
                    endedWallMs: Date.now(),
                    durationMs: performance.now() - startedPerfMs,
                  }};
                }})();""",
            )
        except CollectorError as error:
            outcome["error"] = str(error)

    driver.timeout_s = max(driver.timeout_s, seconds + 30.0)
    worker = threading.Thread(target=freeze, name="ui6-slow-browser-freeze")
    started = time.monotonic()
    worker.start()
    # Give geckodriver enough time to enter the synchronous script. The main
    # collector never issues a second command to this session until it ends.
    time.sleep(0.25)
    return worker, outcome, started


def _static_visible_row(
    driver: RawWebDriver,
    composer_backend_url: str,
    duration_s: float,
) -> dict[str, Any]:
    # Preserve the exact last-applied cursor. Visibility recovery may retain
    # that already-current presentation and correctly send no duplicate frame.
    _reset_performance_metrics(driver)
    before = _viewer_status(composer_backend_url)
    before_flow = _flow(driver)
    time.sleep(duration_s)
    after = _viewer_status(composer_backend_url)
    after_flow = _flow(driver)
    delta = _counter_delta(before, after)
    _require(delta["sourceWakes"] == 0, "static visible scene woke the owner source")
    _require(delta["published"] == 0, "static visible scene built a transform frame")
    _require(delta["sent"] == 0, "static visible scene sent a transform frame")
    _require(delta["sourceErrors"] == 0, "static visible scene produced a source error")
    _require(
        after_flow["receivedFrames"] == before_flow["receivedFrames"]
        and after_flow["appliedFrames"] == before_flow["appliedFrames"],
        "static visible scene applied timer-created frames",
    )
    return {
        "durationSeconds": duration_s,
        "sourceDelta": delta,
        "browserFlow": after_flow,
    }


def _visibility_row(
    driver: RawWebDriver,
    composer_backend_url: str,
    baseline_clients: int,
) -> dict[str, Any]:
    first_handle = driver.command("GET", "/window")
    _require(isinstance(first_handle, str) and first_handle, "browser window is missing")
    opened = driver.command("POST", "/window/new", {"type": "tab"})
    if not isinstance(opened, dict) or not isinstance(opened.get("handle"), str):
        raise CollectorError("browser did not create a second tab")
    blank_handle = opened["handle"]
    driver.command("POST", "/window", {"handle": blank_handle})
    driver.command("POST", "/url", {"url": "about:blank"})
    hidden = _wait_status(
        composer_backend_url,
        lambda status: _client_count(status) == baseline_clients,
        "background browser demand release",
    )

    driver.command("POST", "/window", {"handle": first_handle})
    foreground = _wait_status(
        composer_backend_url,
        lambda status: _client_count(status) == baseline_clients + 1,
        "foreground browser demand recovery",
    )
    recovered, _ = _wait(
        lambda: _flow(driver),
        lambda state: state["lastApplied"] is not None,
        "foreground current frame",
        DEFAULT_TIMEOUT_S,
    )

    driver.command("POST", "/window", {"handle": blank_handle})
    driver.command("DELETE", "/window")
    driver.command("POST", "/window", {"handle": first_handle})
    _wait_status_stable(
        composer_backend_url,
        lambda status: _client_count(status) == baseline_clients + 1,
        "foreground browser after closing helper tab",
    )
    return {
        "hiddenClientCount": _client_count(hidden),
        "foregroundClientCount": _client_count(foreground),
        "recoveredCurrentSequence": recovered["lastApplied"]["sourceSequence"],
    }


STOP_PROBE_INSTALL_SCRIPT = r"""return (() => {
  if (window.__manyforgeUi6OriginalFetch === undefined) {
    window.__manyforgeUi6OriginalFetch = window.fetch.bind(window);
  }
  const originalFetch = window.__manyforgeUi6OriginalFetch;
  const stop = document.querySelector('.viewer-transport .transport-stop-primary');
  if (!(stop instanceof HTMLButtonElement)) return false;
  const probe = {
    clickPerfMs: null,
    clickWallMs: null,
    dispatchPerfMs: null,
    dispatchWallMs: null,
    responsePerfMs: null,
    responseWallMs: null,
    responseStatus: null,
    responsePath: null,
    responseBody: null,
    composerIngressMonotonicNs: null,
    composerResponseMonotonicNs: null,
    stoppingPerfMs: null,
    stoppedPerfMs: null,
  };
  window.__manyforgeUi6StopProbe = probe;
  window.fetch = async (...args) => {
    const input = args[0];
    const url = typeof input === 'string' ? input : input?.url;
    const isStop = typeof url === 'string'
      && /\/api\/program\/runs\/[^/]+\/(?:motion-stop|stop)$/.test(
        new URL(url, window.location.href).pathname,
      );
    if (isStop && probe.dispatchPerfMs === null) {
      probe.dispatchPerfMs = performance.now();
      probe.dispatchWallMs = Date.now();
      probe.responsePath = new URL(url, window.location.href).pathname;
    }
    const response = await originalFetch(...args);
    if (isStop && probe.responsePerfMs === null) {
      probe.responsePerfMs = performance.now();
      probe.responseWallMs = Date.now();
      probe.responseStatus = response.status;
      probe.composerIngressMonotonicNs = response.headers.get(
        'X-ManyForge-Composer-Control-Ingress-Monotonic-Ns',
      );
      probe.composerResponseMonotonicNs = response.headers.get(
        'X-ManyForge-Composer-Control-Response-Monotonic-Ns',
      );
      try { probe.responseBody = await response.clone().json(); } catch { /* bounded diagnostic */ }
    }
    return response;
  };
  const observer = new MutationObserver(() => {
    const text = stop.textContent?.trim();
    if (text === 'Stopping' && probe.stoppingPerfMs === null) {
      probe.stoppingPerfMs = performance.now();
    }
    if (text === 'Stopped' && probe.stoppedPerfMs === null) {
      probe.stoppedPerfMs = performance.now();
      observer.disconnect();
    }
  });
  observer.observe(stop, { childList: true, subtree: true, characterData: true });
  return true;
})();"""


def _history_sequence(composer_url: str) -> int:
    page = _http_json(composer_url, "/api/execution/history", params=[("limit", 1)])
    events = page.get("events")
    if not isinstance(events, list):
        raise CollectorError("historian page is malformed")
    if not events:
        return 0
    sequence = events[0].get("sequence") if isinstance(events[0], dict) else None
    if type(sequence) is not int or sequence < 1:
        raise CollectorError("historian event sequence is malformed")
    return sequence


def _wait_running_node(
    composer_url: str,
    timeout_s: float = DEFAULT_TIMEOUT_S,
) -> dict[str, Any]:
    """Wait for one owner-published running Program node before ordinary Stop."""

    deadline = time.monotonic() + timeout_s
    while True:
        index = _http_json(
            composer_url,
            "/api/program/runs",
            params=[("active_only", "true"), ("limit", 1)],
        )
        runs = index.get("runs")
        if not isinstance(runs, list) or len(runs) > 1:
            raise CollectorError("active Program-run index is malformed")
        if runs:
            run = runs[0]
            node_statuses = run.get("node_statuses") if isinstance(run, dict) else None
            if (
                isinstance(run, dict)
                and run.get("state") == "running"
                and isinstance(node_statuses, dict)
                and any(status == "running" for status in node_statuses.values())
            ):
                return run
        if time.monotonic() >= deadline:
            raise CollectorError(
                "timed out waiting for a running Program node before Stop"
            )
        time.sleep(POLL_SECONDS)


def _wait_stop_terminal(
    composer_url: str,
    after_sequence: int,
    timeout_s: float = DEFAULT_TIMEOUT_S,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_s
    while True:
        page = _http_json(
            composer_url,
            "/api/execution/history",
            params=[("event_types", "stop_terminal_projected"), ("limit", 32)],
        )
        events = page.get("events")
        if not isinstance(events, list):
            raise CollectorError("historian Stop page is malformed")
        for event in events:
            if not isinstance(event, dict):
                continue
            sequence = event.get("sequence")
            historian_event = event.get("event")
            wrapped_payload = (
                historian_event.get("payload")
                if isinstance(historian_event, dict)
                else None
            )
            payload = (
                wrapped_payload.get("journal_payload")
                if isinstance(wrapped_payload, dict)
                else None
            )
            if (
                type(sequence) is int
                and sequence > after_sequence
                and isinstance(payload, dict)
                and payload.get("record_kind") == "stop_terminal_projected"
            ):
                return {
                    "event": historian_event,
                    "payload": payload,
                    "sequence": sequence,
                }
        if time.monotonic() >= deadline:
            raise CollectorError("timed out waiting for the exact terminal Stop history")
        time.sleep(POLL_SECONDS)


def _stop_probe(driver: RawWebDriver) -> dict[str, Any]:
    value = _execute(
        driver,
        "return window.__manyforgeUi6StopProbe ?? null;",
    )
    if not isinstance(value, dict):
        raise CollectorError("browser Stop probe is unavailable")
    return value


def _finite(value: object, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise CollectorError(f"{label} is not numeric")
    result = float(value)
    if not math.isfinite(result):
        raise CollectorError(f"{label} is not finite")
    return result


def _monotonic_wall_offset_ns() -> int:
    samples = [time.time_ns() - time.monotonic_ns() for _ in range(9)]
    samples.sort()
    return samples[len(samples) // 2]


def _stop_latency(
    probe: dict[str, Any],
    terminal_event: dict[str, Any],
    monotonic_wall_offset_ns: int,
) -> dict[str, Any]:
    payload = terminal_event.get("payload")
    if not isinstance(payload, dict):
        raise CollectorError("terminal Stop history payload is missing")
    acceptance = payload.get("acceptance")
    if not isinstance(acceptance, dict):
        raise CollectorError("terminal Stop acceptance is missing")
    accepted_ns = acceptance.get("accepted_at_ns")
    completed_ns = payload.get("completed_at_ns")
    if type(accepted_ns) is not int or type(completed_ns) is not int:
        raise CollectorError("terminal Stop timing is malformed")
    _require(completed_ns >= accepted_ns, "terminal Stop preceded acceptance")
    click_perf = _finite(probe.get("clickPerfMs"), "Stop click time")
    dispatch_perf = _finite(probe.get("dispatchPerfMs"), "Stop dispatch time")
    response_perf = _finite(probe.get("responsePerfMs"), "Stop response time")
    stopped_perf = _finite(probe.get("stoppedPerfMs"), "Stopped presentation time")
    dispatch_wall = _finite(probe.get("dispatchWallMs"), "Stop dispatch wall time")
    ingress_raw = probe.get("composerIngressMonotonicNs")
    if not isinstance(ingress_raw, str) or not ingress_raw.isdigit():
        raise CollectorError("Composer Stop ingress timestamp is missing")
    ingress_ns = int(ingress_raw)
    response_raw = probe.get("composerResponseMonotonicNs")
    if not isinstance(response_raw, str) or not response_raw.isdigit():
        raise CollectorError("Composer Stop response timestamp is missing")
    composer_response_ns = int(response_raw)
    _require(ingress_ns <= accepted_ns, "Kernel accepted Stop before Composer ingress")
    _require(
        accepted_ns <= composer_response_ns,
        "Composer encoded the Stop response before Kernel acceptance",
    )
    ingress_wall_ms = (ingress_ns + monotonic_wall_offset_ns) / 1_000_000.0
    accepted_wall_ms = (accepted_ns + monotonic_wall_offset_ns) / 1_000_000.0
    completed_wall_ms = (completed_ns + monotonic_wall_offset_ns) / 1_000_000.0
    result = {
        "clickToDispatchMs": dispatch_perf - click_perf,
        "dispatchToComposerIngressMs": ingress_wall_ms - dispatch_wall,
        "composerIngressToKernelAcceptanceMs": (accepted_ns - ingress_ns)
        / 1_000_000.0,
        "kernelAcceptanceToResponseMs": response_perf - dispatch_perf
        - (accepted_wall_ms - dispatch_wall),
        "kernelAcceptanceToComposerResponseMs": (
            composer_response_ns - accepted_ns
        ) / 1_000_000.0,
        "composerResponseToBrowserMs": response_perf - dispatch_perf
        - (
            (composer_response_ns + monotonic_wall_offset_ns) / 1_000_000.0
            - dispatch_wall
        ),
        "kernelAcceptanceToPhysicalSettlementMs": (completed_ns - accepted_ns)
        / 1_000_000.0,
        "physicalSettlementToStoppedPresentationMs": stopped_perf - click_perf
        - (completed_wall_ms - _finite(probe.get("clickWallMs"), "Stop click wall time")),
        "clickToStoppedPresentationMs": stopped_perf - click_perf,
        "httpStatus": probe.get("responseStatus"),
        "responsePath": probe.get("responsePath"),
        "stopId": payload.get("stop_id"),
        "stopGeneration": payload.get("stop_generation"),
        "historianSequence": terminal_event.get("sequence"),
    }
    for key, value in result.items():
        if key.endswith("Ms"):
            _require(
                isinstance(value, (int, float)) and math.isfinite(float(value)),
                f"Stop latency {key} is invalid",
            )
    _require(result["httpStatus"] == 200, "Stop endpoint did not return HTTP 200")
    return result


def _run_and_stop(
    driver: RawWebDriver,
    composer_url: str,
) -> tuple[dict[str, Any], dict[str, Any], list[dict[str, Any]]]:
    idle, _ = _wait(
        lambda: _dom_state(driver),
        lambda state: state["runText"] == "Run" and state["runDisabled"] is False,
        "automatic Idle before UI6 Stop trace",
        DEFAULT_TIMEOUT_S,
    )
    _require(idle["stopDisabled"] is True, "Stop is enabled before Run")
    slowed = _set_range(driver, 0.25)
    _require(
        slowed["identityStable"] and slowed["focused"],
        "pre-Stop velocity scaling lost control identity/focus",
    )
    _scaled, _ = _wait(
        lambda: _dom_state(driver),
        lambda state: state["velocityValue"] == "0.25",
        "pre-Stop velocity scaling",
        DEFAULT_TIMEOUT_S,
    )
    before_history = _history_sequence(composer_url)
    _require(
        _execute(driver, STOP_PROBE_INSTALL_SCRIPT) is True,
        "Stop probe could not be installed",
    )
    _click_selector(driver, ".transport-run", "Run")
    _running, run_trace = _wait(
        lambda: _dom_state(driver),
        lambda state: (
            state["runText"] == "Running"
            and state["runDisabled"] is True
            and state["stopDisabled"] is False
        ),
        "Running before UI6 Stop",
        DEFAULT_TIMEOUT_S,
    )
    _assert_transition_fences(run_trace, "UI6 Run")
    _wait_running_node(composer_url)
    _motion_flow, _ = _wait(
        lambda: _flow(driver),
        lambda state: (
            state["receivedFrames"] >= 1
            and state["appliedFrames"] >= 1
        ),
        "first applied robot motion before UI6 Stop",
        DEFAULT_TIMEOUT_S,
    )
    clicked = _execute(driver, r"""return (() => {
      const stop = document.querySelector('.viewer-transport .transport-stop-primary');
      const probe = window.__manyforgeUi6StopProbe;
      if (!(stop instanceof HTMLButtonElement) || stop.disabled || probe === undefined) {
        return false;
      }
      probe.clickPerfMs = performance.now();
      probe.clickWallMs = Date.now();
      stop.click();
      return true;
    })();""")
    _require(clicked is True, "Stop was unavailable at dispatch")
    stopped, stop_trace = _wait(
        lambda: _dom_state(driver),
        lambda state: (
            state["stopText"] == "Stopped"
            and state["stopDisabled"] is True
            and state["runText"] == "Resume Motion"
            and state["runDisabled"] is False
        ),
        "Stopped presentation",
        DEFAULT_TIMEOUT_S,
    )
    _assert_transition_fences(stop_trace, "UI6 Stop")
    _require(
        any(state["stopText"] == "Stopping" for state in stop_trace),
        "Stop never presented Stopping",
    )
    terminal = _wait_stop_terminal(composer_url, before_history)
    restored = _set_range(driver, 1.0)
    _require(
        restored["identityStable"],
        "post-Stop velocity restoration rebound the control",
    )
    _restored, _ = _wait(
        lambda: _dom_state(driver),
        lambda state: state["velocityValue"] == "1",
        "post-Stop velocity restoration",
        DEFAULT_TIMEOUT_S,
    )
    return stopped, terminal, stop_trace


def _resume_and_reset(driver: RawWebDriver) -> None:
    _click_selector(driver, ".transport-run", "Resume Motion")
    _running, trace = _wait(
        lambda: _dom_state(driver),
        lambda state: (
            state["runText"] == "Running"
            and state["runDisabled"] is True
            and state["stopDisabled"] is False
        ),
        "resumed Running",
        DEFAULT_TIMEOUT_S,
    )
    _assert_transition_fences(trace, "UI6 Resume")
    _terminal, terminal_trace = _wait(
        lambda: _dom_state(driver),
        lambda state: state["resetDisabled"] is False and state["runDisabled"] is True,
        "terminal Program before UI6 Reset",
        TERMINAL_TIMEOUT_S,
    )
    _assert_transition_fences(terminal_trace, "UI6 terminal")
    _click_selector(driver, ".transport-reset", "Reset")
    _idle, reset_trace = _wait(
        lambda: _dom_state(driver),
        lambda state: (
            state["runText"] == "Run"
            and state["runDisabled"] is False
            and state["resetDisabled"] is True
        ),
        "automatic readiness after UI6 Reset",
        DEFAULT_TIMEOUT_S,
    )
    _assert_transition_fences(reset_trace, "UI6 Reset")


def _ensure_idle(driver: RawWebDriver) -> None:
    current, _ = _wait(
        lambda: _dom_state(driver),
        lambda state: (
            (state["runText"] == "Run" and state["runDisabled"] is False)
            or (
                state["runText"] == "Resume Motion"
                and state["runDisabled"] is False
            )
            or state["resetDisabled"] is False
        ),
        "actionable owner state before UI6 qualification",
        TERMINAL_TIMEOUT_S,
    )
    if current["runText"] == "Run" and current["runDisabled"] is False:
        return
    if current["runText"] == "Resume Motion" and current["runDisabled"] is False:
        _progress("settling retained stopped cycle before qualification")
        _resume_and_reset(driver)
        return
    if current["resetDisabled"] is False:
        _progress("resetting retained terminal cycle before qualification")
        _click_selector(driver, ".transport-reset", "Reset")
    _idle, trace = _wait(
        lambda: _dom_state(driver),
        lambda state: state["runText"] == "Run" and state["runDisabled"] is False,
        "automatic Idle before UI6 qualification",
        TERMINAL_TIMEOUT_S,
    )
    _assert_transition_fences(trace, "UI6 initial settlement")


def _slow_client_row(
    fast: RawWebDriver,
    slow: RawWebDriver,
    composer_url: str,
    composer_backend_url: str,
    baseline_clients: int,
    freeze_seconds: float,
    freeze_thread: threading.Thread,
    freeze_outcome: dict[str, Any],
    freeze_started: float,
) -> dict[str, Any]:
    _progress("fast-client metrics reset; slow client already armed")
    _reset_browser_metrics(fast)
    before = _viewer_status(composer_backend_url)
    before_clients = _client_count(before)
    _require(
        before_clients == baseline_clients + 2,
        "slow-client row did not start from exactly two qualification clients",
    )
    time.sleep(0.10)

    _progress("fast-client Run/Stop during slow-client freeze")
    stopped, terminal, stop_trace = _run_and_stop(fast, composer_url)
    fast_flow = _flow(fast)
    fast_snapshot = _performance(fast)
    _require(fast_flow["receivedFrames"] > 0, "fast client received no motion frames")
    _require(fast_flow["appliedFrames"] > 0, "fast client applied no motion frames")
    _require(fast_flow["replacedBeforeApply"] == 0, "fast client accumulated stale frames")
    _require(fast_flow["maxUnappliedFrames"] <= 1, "fast client exceeded one unapplied frame")
    _require(
        fast_snapshot["performance"]["modelBuilds"] == 0,
        "slow-client pressure rebuilt fast-client geometry",
    )
    _require(
        fast_snapshot["status"]["canvasCount"] == 1
        and fast_snapshot["status"]["headless"]["activeSockets"] == 1,
        "slow-client pressure changed fast-client Canvas/socket topology",
    )
    probe = _stop_probe(fast)
    latency = _stop_latency(probe, terminal, _monotonic_wall_offset_ns())

    _progress("frozen-client lease expiry")
    expired = _wait_status(
        composer_backend_url,
        lambda status: _client_count(status) == before_clients - 1,
        "frozen client lease expiry",
        FROZEN_LEASE_SECONDS + 10.0,
    )
    remaining = freeze_seconds - (time.monotonic() - freeze_started)
    if remaining > 0:
        time.sleep(remaining + 0.25)
    freeze_thread.join(timeout=30.0)
    _require(not freeze_thread.is_alive(), "slow-browser freeze did not finish")
    _require("error" not in freeze_outcome, "slow-browser freeze command failed")
    browser_freeze = freeze_outcome.get("browser")
    _require(isinstance(browser_freeze, dict), "slow-browser freeze result is missing")
    _progress("thawed-client current-only reconnect")
    recovered_status = _wait_status(
        composer_backend_url,
        lambda status: _client_count(status) == baseline_clients + 2,
        "thawed client current-only reconnect",
        DEFAULT_TIMEOUT_S,
    )
    fast_current = _flow(fast)["lastApplied"]
    _require(isinstance(fast_current, dict), "fast current frame identity is missing")
    slow.timeout_s = 15.0
    recovery_deadline = time.monotonic() + DEFAULT_TIMEOUT_S
    recovery_error: CollectorError | None = None
    while True:
        try:
            recovered_flow = _flow(slow)
            recovery_error = None
        except CollectorError as error:
            recovery_error = error
            recovered_flow = {}
        candidate = recovered_flow.get("lastApplied")
        if (
            isinstance(candidate, dict)
            and candidate.get("ownerEpoch") == fast_current["ownerEpoch"]
            and candidate.get("sourceEpoch") == fast_current["sourceEpoch"]
            and isinstance(candidate.get("sourceSequence"), int)
            and candidate["sourceSequence"] >= fast_current["sourceSequence"]
        ):
            break
        if time.monotonic() >= recovery_deadline:
            detail = "" if recovery_error is None else f": {recovery_error}"
            raise CollectorError(
                "timed out waiting for thawed client frame application" + detail
            )
        time.sleep(POLL_SECONDS)
    slow.timeout_s = 5.0
    slow_current = recovered_flow["lastApplied"]
    _require(isinstance(slow_current, dict), "slow current frame identity is missing")
    _require(
        slow_current["ownerEpoch"] == fast_current["ownerEpoch"]
        and slow_current["sourceEpoch"] == fast_current["sourceEpoch"]
        and slow_current["sourceSequence"] >= fast_current["sourceSequence"],
        "thawed client replayed stale motion instead of taking the current frame",
    )
    after = _viewer_status(composer_backend_url)
    _progress("fast-client Resume/completion/Reset")
    _resume_and_reset(fast)
    return {
        "freezeSeconds": freeze_seconds,
        "browserFreeze": browser_freeze,
        "initialClientCount": before_clients,
        "expiredClientCount": _client_count(expired),
        "recoveredClientCount": _client_count(recovered_status),
        "sourceDelta": _counter_delta(before, after),
        "fastFlow": fast_flow,
        "fastPerformance": fast_snapshot["performance"],
        "stopLatency": latency,
        "stoppingObserved": any(row["stopText"] == "Stopping" for row in stop_trace),
        "stoppedState": {
            "runText": stopped["runText"],
            "stopText": stopped["stopText"],
        },
        "slowRecoveredFrame": slow_current,
    }


def collect_ui6_native_flow(
    geckodriver_endpoint: str,
    slow_geckodriver_endpoint: str,
    composer_url: str,
    composer_backend_url: str,
    *,
    viewport_width: int = 1440,
    viewport_height: int = 960,
    static_seconds: float = STATIC_WINDOW_SECONDS,
    freeze_seconds: float = FROZEN_WINDOW_SECONDS,
    require_software_renderer: bool = False,
) -> dict[str, Any]:
    _require(static_seconds >= 1.0, "static interval must be at least one second")
    _require(
        freeze_seconds > FROZEN_LEASE_SECONDS + 1.0,
        "freeze interval must exceed the demand lease",
    )
    target_url = _url_with_opt_in(composer_url)
    baseline_status = _wait_client_baseline(composer_backend_url)
    baseline_clients = _client_count(baseline_status)
    fast: RawWebDriver | None = None
    slow: RawWebDriver | None = None
    freeze_thread: threading.Thread | None = None
    try:
        fast = _create_browser(
            geckodriver_endpoint,
            target_url,
            width=viewport_width,
            height=viewport_height,
            isolate_firefox=require_software_renderer,
        )
        _progress("fast browser connected")
        ready = _wait_ready(fast, "fast UI6 browser")
        _ensure_idle(fast)
        _wait_status_stable(
            composer_backend_url,
            lambda status: _client_count(status) == baseline_clients + 1,
            "first UI6 browser demand",
        )
        static_row = _static_visible_row(fast, composer_backend_url, static_seconds)
        _progress("visible-static row passed")
        visibility_row = _visibility_row(
            fast,
            composer_backend_url,
            baseline_clients,
        )
        _progress("visibility row passed")

        slow_started = time.monotonic()
        slow = _create_browser(
            slow_geckodriver_endpoint,
            target_url,
            width=viewport_width,
            height=viewport_height,
            isolate_firefox=require_software_renderer,
        )
        # The slow-client row intentionally permits a heavily loaded browser;
        # only the independent fast browser retains the strict 5 s command
        # timeout used to qualify operator responsiveness.
        slow.timeout_s = 90.0
        _progress(
            f"slow browser connected (fast={fast.session_id}, slow={slow.session_id})"
        )
        slow_ready = _wait_slow_ready(slow)
        slow_ready_ms = (time.monotonic() - slow_started) * 1_000.0
        _wait_status_stable(
            composer_backend_url,
            lambda status: _client_count(status) == baseline_clients + 2,
            "independent slow UI6 browser demand",
        )
        freeze_thread, freeze_outcome, freeze_started = _start_slow_freeze(
            slow,
            freeze_seconds,
        )
        _progress("slow browser ready and deliberate stall active")
        slow_row = _slow_client_row(
            fast,
            slow,
            composer_url,
            composer_backend_url,
            baseline_clients,
            freeze_seconds,
            freeze_thread,
            freeze_outcome,
            freeze_started,
        )
        renderer = ready["qualification"]["renderer"]
        _require(
            not require_software_renderer or renderer["software"] is True,
            "forced-software row did not create a software WebGL renderer",
        )
        return {
            "schemaVersion": RESULT_SCHEMA,
            "success": True,
            "scope": "O1.5-UI6 native viewer-flow and priority-control qualification",
            "baselineClients": baseline_clients,
            "renderer": renderer,
            "initial": {
                "canvasCount": ready["canvasCount"],
                "socketCount": ready["socketCount"],
                "sourceSequence": ready["flow"]["lastApplied"]["sourceSequence"],
            },
            "staticVisible": static_row,
            "visibility": visibility_row,
            "slowClient": slow_row,
            "secondBrowserInitialSequence": slow_ready["flow"]["lastApplied"][
                "sourceSequence"
            ],
            "slowBrowserReadyMs": slow_ready_ms,
            "claims": [
                "visible-static time creates no owner read, frame build, send, or apply",
                "hidden demand releases and visibility recovery applies only current state",
                "one frozen client expires without blocking the fast client or controls",
                "each browser retains at most one unapplied frame",
                "Stop dispatch precedes local pending-state work and reaches Kernel under viewer pressure",
                "thaw/reconnect sends current state without replaying missed motion",
                "control transitions preserve one Canvas/socket and do not rebuild geometry",
            ],
        }
    finally:
        active_error = sys.exc_info()[1]
        cleanup_errors: list[str] = []
        if freeze_thread is not None and freeze_thread.is_alive():
            freeze_thread.join(timeout=freeze_seconds + 30.0)
            if freeze_thread.is_alive():
                cleanup_errors.append("slow-browser freeze worker did not finish")
        for driver in (slow, fast):
            if driver is not None and driver.session_id is not None:
                try:
                    # A just-thawed Firefox content process can take longer
                    # than an ordinary command timeout to close cleanly.
                    driver.timeout_s = 30.0
                    driver.delete_session()
                except CollectorError as error:
                    cleanup_errors.append(str(error))
        try:
            _wait_status(
                composer_backend_url,
                lambda status: _client_count(status) <= baseline_clients,
                "UI6 qualification client cleanup",
            )
        except CollectorError as error:
            cleanup_errors.append(str(error))
        if active_error is None and cleanup_errors:
            raise CollectorError("; ".join(cleanup_errors))


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--geckodriver-endpoint", required=True)
    parser.add_argument("--slow-geckodriver-endpoint", required=True)
    parser.add_argument("--composer-url", required=True)
    parser.add_argument("--composer-backend-url", required=True)
    parser.add_argument("--viewport-width", type=int, default=1440)
    parser.add_argument("--viewport-height", type=int, default=960)
    parser.add_argument("--static-seconds", type=float, default=STATIC_WINDOW_SECONDS)
    parser.add_argument("--freeze-seconds", type=float, default=FROZEN_WINDOW_SECONDS)
    parser.add_argument(
        "--require-software-renderer",
        action="store_true",
        help="isolate Firefox and reject the row unless WebGL is software-rendered",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    try:
        result = collect_ui6_native_flow(
            arguments.geckodriver_endpoint,
            arguments.slow_geckodriver_endpoint,
            arguments.composer_url,
            arguments.composer_backend_url,
            viewport_width=arguments.viewport_width,
            viewport_height=arguments.viewport_height,
            static_seconds=arguments.static_seconds,
            freeze_seconds=arguments.freeze_seconds,
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
