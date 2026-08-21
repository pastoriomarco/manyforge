"""Bounded native common-workbench qualification over raw W3C WebDriver HTTP.

The browser and Composer are owned by the caller.  This collector creates one
fresh Firefox WebDriver session, records only a compact qualification result,
and always deletes that exact session before returning.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import math
import sys
import time
from typing import Any, Callable
from urllib.error import HTTPError, URLError
from urllib.parse import parse_qsl, quote, urlencode, urlsplit, urlunsplit
from urllib.request import Request, urlopen


QUALIFICATION_QUERY_KEY = "manyforge_viewer_qualification"
QUALIFICATION_GLOBAL = "manyforgeViewerQualification"
QUALIFICATION_SCHEMA = "manyforge.viewer.qualification.v1"
RESULT_SCHEMA = "manyforge.viewer.qualification.result.v1"
MAX_RESPONSE_BYTES = 1_000_000
POLL_INTERVAL_S = 0.05
WAIT_TIMEOUT_S = 20.0
WARMUP_SAMPLE_COUNT = 3
SAMPLE_COUNT = 5


class CollectorError(RuntimeError):
    """The browser did not provide a closed, qualifying observation."""


@dataclass(frozen=True)
class _Observation:
    qualification: dict[str, Any]
    connected: bool
    operations_pane_present: bool
    operations_collapsed: bool
    viewer_present: bool
    wireframe_unchecked: bool


def _keys(value: object, expected: set[str], label: str) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != expected:
        raise CollectorError(f"{label} has an unsupported response shape")
    return value


def _number(value: object, label: str, *, integer: bool = False) -> float | int:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise CollectorError(f"{label} is not numeric")
    if not math.isfinite(float(value)) or value < 0 or value > 1_000_000_000:
        raise CollectorError(f"{label} is not finite and non-negative")
    if integer and (not isinstance(value, int) or isinstance(value, bool)):
        raise CollectorError(f"{label} is not an integer")
    return value


def _validate_performance(value: object) -> dict[str, Any]:
    result = _keys(
        value,
        {
            "windowDurationMs",
            "modelBuilds",
            "modelBuildTotalMs",
            "modelBuildMaxMs",
            "reactCommits",
            "reactCommitTotalMs",
            "reactCommitMaxMs",
            "renderedFrames",
            "drawCallsMax",
            "trianglesMax",
            "canvasPixelsMax",
            "renderIntervalP95Ms",
            "renderIntervalMaxMs",
            "longTaskSupported",
            "longTasks",
            "longTaskTotalMs",
            "longTaskMaxMs",
        },
        "performance",
    )
    for key in (
        "windowDurationMs",
        "modelBuildTotalMs",
        "modelBuildMaxMs",
        "reactCommitTotalMs",
        "reactCommitMaxMs",
        "longTaskTotalMs",
        "longTaskMaxMs",
    ):
        _number(result[key], f"performance.{key}")
    for key in (
        "modelBuilds",
        "reactCommits",
        "renderedFrames",
        "drawCallsMax",
        "trianglesMax",
        "canvasPixelsMax",
        "longTasks",
    ):
        _number(result[key], f"performance.{key}", integer=True)
    for key in ("renderIntervalP95Ms", "renderIntervalMaxMs"):
        if result[key] is not None:
            _number(result[key], f"performance.{key}")
    if not isinstance(result["longTaskSupported"], bool):
        raise CollectorError("performance.longTaskSupported is not boolean")
    return result


def _validate_qualification(value: object) -> dict[str, Any]:
    result = _keys(
        value,
        {"schemaVersion", "status", "performance"},
        "qualification snapshot",
    )
    if result["schemaVersion"] != QUALIFICATION_SCHEMA:
        raise CollectorError("qualification schema version is unsupported")
    status = _keys(
        result["status"],
        {
            "schemaVersion",
            "viewerActive",
            "canvasCount",
            "wireframeDefault",
            "wireframeVisible",
            "model",
            "renderer",
            "browser",
            "viewport",
            "headless",
            "errors",
        },
        "qualification status",
    )
    if status["schemaVersion"] != QUALIFICATION_SCHEMA:
        raise CollectorError("qualification status schema version is unsupported")
    if not isinstance(status["viewerActive"], bool):
        raise CollectorError("viewerActive is not boolean")
    _number(status["canvasCount"], "canvasCount", integer=True)
    if not isinstance(status["wireframeDefault"], bool) or not isinstance(
        status["wireframeVisible"], bool
    ):
        raise CollectorError("wireframe state is malformed")
    model = _keys(status["model"], {"ready", "geometryCount"}, "model")
    if not isinstance(model["ready"], bool):
        raise CollectorError("model.ready is not boolean")
    _number(model["geometryCount"], "model.geometryCount", integer=True)
    renderer = status["renderer"]
    if renderer is not None:
        renderer = _keys(
            renderer,
            {"vendor", "renderer", "software", "effectiveDpr", "antialias", "preserveDrawingBuffer"},
            "renderer",
        )
        if not isinstance(renderer["vendor"], (str, type(None))) or not isinstance(
            renderer["renderer"], (str, type(None))
        ):
            raise CollectorError("renderer identity is malformed")
        if not isinstance(renderer["software"], bool):
            raise CollectorError("renderer software flag is malformed")
        _number(renderer["effectiveDpr"], "renderer.effectiveDpr")
        if not isinstance(renderer["antialias"], bool) or not isinstance(
            renderer["preserveDrawingBuffer"], bool
        ):
            raise CollectorError("renderer profile is malformed")
    browser = _keys(status["browser"], {"userAgent", "platform"}, "browser")
    if not isinstance(browser["userAgent"], str) or not isinstance(
        browser["platform"], str
    ):
        raise CollectorError("browser identity is malformed")
    viewport = _keys(
        status["viewport"],
        {"width", "height", "devicePixelRatio"},
        "viewport",
    )
    _number(viewport["width"], "viewport.width", integer=True)
    _number(viewport["height"], "viewport.height", integer=True)
    _number(viewport["devicePixelRatio"], "viewport.devicePixelRatio")
    headless = _keys(
        status["headless"],
        {"socketPath", "sourceCount", "activeSockets", "socketCreations", "maxActiveSockets"},
        "headless transport",
    )
    if headless["socketPath"] != "/ws/state":
        raise CollectorError("qualification did not observe the shared /ws/state socket")
    for key in ("sourceCount", "activeSockets", "socketCreations", "maxActiveSockets"):
        _number(headless[key], f"headless.{key}", integer=True)
    if not isinstance(status["errors"], list) or len(status["errors"]) > 16:
        raise CollectorError("qualification errors are malformed or oversized")
    if not all(isinstance(error, str) and len(error) <= 240 for error in status["errors"]):
        raise CollectorError("qualification errors are malformed")
    _validate_performance(result["performance"])
    return result


def _validate_observation(value: object) -> _Observation:
    result = _keys(
        value,
        {
            "qualification",
            "connected",
            "operationsPanePresent",
            "operationsCollapsed",
            "viewerPresent",
            "wireframeUnchecked",
        },
        "browser observation",
    )
    qualification = _validate_qualification(result["qualification"])
    for key in (
        "connected",
        "operationsPanePresent",
        "operationsCollapsed",
        "viewerPresent",
        "wireframeUnchecked",
    ):
        if not isinstance(result[key], bool):
            raise CollectorError(f"{key} is not boolean")
    return _Observation(
        qualification=qualification,
        connected=result["connected"],
        operations_pane_present=result["operationsPanePresent"],
        operations_collapsed=result["operationsCollapsed"],
        viewer_present=result["viewerPresent"],
        wireframe_unchecked=result["wireframeUnchecked"],
    )


class RawWebDriver:
    """Small W3C WebDriver HTTP client; no browser library is required."""

    def __init__(self, endpoint: str, timeout_s: float = 5.0) -> None:
        parsed = urlsplit(endpoint)
        if parsed.scheme not in {"http", "https"} or not parsed.netloc:
            raise CollectorError("geckodriver endpoint must be an HTTP URL")
        self.endpoint = endpoint.rstrip("/")
        self.timeout_s = max(0.1, min(timeout_s, 30.0))
        self.session_id: str | None = None

    def _request(
        self,
        method: str,
        path: str,
        payload: dict[str, Any] | None = None,
    ) -> Any:
        body = None if payload is None else json.dumps(payload, separators=(",", ":")).encode()
        request = Request(
            f"{self.endpoint}{path}",
            data=body,
            method=method,
            headers={"Accept": "application/json", "Content-Type": "application/json"},
        )
        try:
            with urlopen(request, timeout=self.timeout_s) as response:
                raw = response.read(MAX_RESPONSE_BYTES + 1)
        except (HTTPError, URLError, TimeoutError, OSError) as error:
            raise CollectorError(f"WebDriver {method} {path} failed") from error
        if len(raw) > MAX_RESPONSE_BYTES:
            raise CollectorError("WebDriver response is oversized")
        try:
            decoded = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise CollectorError("WebDriver returned malformed JSON") from error
        if not isinstance(decoded, dict) or "value" not in decoded:
            raise CollectorError("WebDriver returned an unsupported command response")
        value = decoded["value"]
        if isinstance(value, dict) and isinstance(value.get("error"), str):
            raise CollectorError(f"WebDriver command failed: {value['error']}")
        return value

    def create_session(self) -> str:
        value = self._request(
            "POST",
            "/session",
            {
                "capabilities": {
                    "alwaysMatch": {
                        "browserName": "firefox",
                        "moz:firefoxOptions": {"args": ["-headless"]},
                    }
                }
            },
        )
        if not isinstance(value, dict):
            raise CollectorError("new-session response is malformed")
        session_id = value.get("sessionId")
        if not isinstance(session_id, str) or not session_id:
            raise CollectorError("new-session response has no exact session id")
        self.session_id = session_id
        return session_id

    def command(self, method: str, path: str, payload: dict[str, Any] | None = None) -> Any:
        if self.session_id is None:
            raise CollectorError("WebDriver session is not active")
        return self._request(method, f"/session/{quote(self.session_id, safe='')}{path}", payload)

    def delete_session(self) -> None:
        session_id = self.session_id
        self.session_id = None
        if session_id is not None:
            self._request("DELETE", f"/session/{quote(session_id, safe='')}")


OBSERVATION_SCRIPT = f"""return (() => {{
  const q = window.{QUALIFICATION_GLOBAL};
  const shell = document.querySelector('.workbench');
  const operations = document.querySelector('.workbench-commands');
  const pane = operations?.querySelector('.workbench-pane');
  return {{
    qualification: q === undefined ? null : q.snapshot(),
    connected: document.querySelector('.header-boundary')?.textContent?.includes('Headless owner connected') === true,
    operationsPanePresent: operations !== null && pane !== null,
    operationsCollapsed: shell?.classList.contains('commands-collapsed') === true,
    viewerPresent: pane?.querySelector('.viewer-panel') !== null,
    wireframeUnchecked: document.querySelector('.viewer-controls input[type="checkbox"]')?.checked === false
  }};
}})();"""
TAKE_SCRIPT = f"return window.{QUALIFICATION_GLOBAL}?.take() ?? null;"
COLLAPSE_SCRIPT = "return (() => { const node = document.querySelector('[aria-label=\"Collapse viewer and operations pane\"]'); if (!node) return false; node.click(); return true; })();"
EXPAND_SCRIPT = "return (() => { const node = document.querySelector('[aria-label=\"Expand Operations pane\"]'); if (!node) return false; node.click(); return true; })();"


def _execute(driver: RawWebDriver, script: str) -> Any:
    value = driver.command("POST", "/execute/sync", {"script": script, "args": []})
    if value is None:
        raise CollectorError("WebDriver returned an unsupported command value")
    return value


def _read(driver: RawWebDriver) -> _Observation:
    return _validate_observation(_execute(driver, OBSERVATION_SCRIPT))


def _wait(
    driver: RawWebDriver,
    predicate: Callable[[_Observation], bool],
    label: str,
    timeout_s: float,
) -> _Observation:
    deadline = time.monotonic() + min(timeout_s, WAIT_TIMEOUT_S)
    while True:
        observation = _read(driver)
        if predicate(observation):
            return observation
        if time.monotonic() >= deadline:
            raise CollectorError(f"timed out waiting for {label}")
        time.sleep(POLL_INTERVAL_S)


def _url_with_opt_in(composer_url: str) -> str:
    parsed = urlsplit(composer_url)
    if parsed.scheme not in {"http", "https"} or not parsed.netloc:
        raise CollectorError("Composer URL must be an HTTP URL")
    query = [
        (key, value)
        for key, value in parse_qsl(parsed.query, keep_blank_values=True)
        if key != QUALIFICATION_QUERY_KEY
    ]
    query.append((QUALIFICATION_QUERY_KEY, "1"))
    return urlunsplit(
        (parsed.scheme, parsed.netloc, parsed.path, urlencode(query), parsed.fragment)
    )


def _require_ready(observation: _Observation) -> None:
    status = observation.qualification["status"]
    if (
        not observation.connected
        or not observation.operations_pane_present
        or observation.operations_collapsed
        or not observation.viewer_present
    ):
        raise CollectorError("Composer workbench is not connected with its viewer/operations pane available")
    if not observation.wireframe_unchecked:
        raise CollectorError("wireframe control is checked by default")
    if status["canvasCount"] != 1:
        raise CollectorError("qualification requires exactly one viewer Canvas")
    if (
        status["headless"]["socketPath"] != "/ws/state"
        or status["headless"]["sourceCount"] != 1
        or status["headless"]["activeSockets"] != 1
        or status["headless"]["maxActiveSockets"] != 1
    ):
        raise CollectorError("qualification requires exactly one active HeadlessStateSource socket")
    if status["wireframeDefault"] is not False or status["wireframeVisible"] is not False:
        raise CollectorError("qualification requires the solid wireframe default")
    if not status["model"]["ready"] or status["model"]["geometryCount"] <= 0:
        raise CollectorError("qualification model is not ready with geometry")
    renderer = status["renderer"]
    if renderer is None or not renderer["vendor"] or not renderer["renderer"]:
        raise CollectorError("renderer identity is missing")
    if not status["browser"]["userAgent"]:
        raise CollectorError("browser identity is missing")
    if status["errors"]:
        raise CollectorError("viewer browser/runtime error was reported")


def _viewer_ready(observation: _Observation) -> bool:
    if not (
        observation.connected
        and observation.operations_pane_present
        and not observation.operations_collapsed
        and observation.viewer_present
        and observation.wireframe_unchecked
    ):
        return False
    status = observation.qualification["status"]
    renderer = status["renderer"]
    return (
        status["canvasCount"] == 1
        and status["headless"]["sourceCount"] == 1
        and status["headless"]["activeSockets"] == 1
        and status["headless"]["maxActiveSockets"] == 1
        and status["wireframeDefault"] is False
        and status["wireframeVisible"] is False
        and status["model"]["ready"]
        and status["model"]["geometryCount"] > 0
        and renderer is not None
        and bool(renderer["vendor"])
        and bool(renderer["renderer"])
        and bool(status["browser"]["userAgent"])
        and not status["errors"]
    )


def _take(driver: RawWebDriver) -> dict[str, Any]:
    value = _validate_qualification(_execute(driver, TAKE_SCRIPT))
    return value


def _topology(observation: _Observation) -> dict[str, Any]:
    status = observation.qualification["status"]
    headless = status["headless"]
    return {
        "socketPath": headless["socketPath"],
        "canvasCount": status["canvasCount"],
        "sourceCount": headless["sourceCount"],
        "activeSockets": headless["activeSockets"],
        "socketCreations": headless["socketCreations"],
        "maxActiveSockets": headless["maxActiveSockets"],
    }


def _combine_windows(windows: list[dict[str, Any]]) -> dict[str, Any]:
    if not windows:
        raise CollectorError("qualification produced no bounded sample windows")
    numeric_sum = (
        "windowDurationMs",
        "modelBuilds",
        "modelBuildTotalMs",
        "reactCommits",
        "reactCommitTotalMs",
        "renderedFrames",
        "longTasks",
        "longTaskTotalMs",
    )
    result: dict[str, Any] = {key: sum(float(window["performance"][key]) for window in windows) for key in numeric_sum}
    result["modelBuildMaxMs"] = max(window["performance"]["modelBuildMaxMs"] for window in windows)
    result["reactCommitMaxMs"] = max(window["performance"]["reactCommitMaxMs"] for window in windows)
    result["drawCallsMax"] = max(window["performance"]["drawCallsMax"] for window in windows)
    result["trianglesMax"] = max(window["performance"]["trianglesMax"] for window in windows)
    result["canvasPixelsMax"] = max(window["performance"]["canvasPixelsMax"] for window in windows)
    result["renderIntervalP95Ms"] = max(
        (window["performance"]["renderIntervalP95Ms"] or 0.0 for window in windows),
        default=0.0,
    )
    result["renderIntervalMaxMs"] = max(
        (window["performance"]["renderIntervalMaxMs"] or 0.0 for window in windows),
        default=0.0,
    )
    result["longTaskSupported"] = all(
        window["performance"]["longTaskSupported"] for window in windows
    )
    result["longTaskMaxMs"] = max(window["performance"]["longTaskMaxMs"] for window in windows)
    for key in ("modelBuilds", "reactCommits", "renderedFrames", "longTasks"):
        result[key] = int(result[key])
    return result


def _collect_windows(driver: RawWebDriver, count: int) -> list[dict[str, Any]]:
    windows: list[dict[str, Any]] = []
    for _ in range(count):
        time.sleep(POLL_INTERVAL_S)
        windows.append(_take(driver))
    return windows


def collect_current_workbench(
    geckodriver_endpoint: str,
    composer_url: str,
    *,
    timeout_s: float = WAIT_TIMEOUT_S,
) -> dict[str, Any]:
    """Collect one bounded native focused-idle common-workbench result."""
    driver = RawWebDriver(
        geckodriver_endpoint,
        timeout_s=min(5.0, max(0.1, timeout_s)),
    )
    try:
        driver.create_session()
        target_url = _url_with_opt_in(composer_url)
        driver.command("POST", "/url", {"url": target_url})
        _wait(
            driver,
            lambda item: (
                item.connected
                and item.operations_pane_present
                and not item.operations_collapsed
                and item.viewer_present
            ),
            "connected workbench viewer and operations pane",
            timeout_s,
        )
        ready = _wait(
            driver,
            _viewer_ready,
            "connected solid viewer",
            timeout_s,
        )
        _require_ready(ready)
        initial = _topology(ready)

        warmup_windows = _collect_windows(driver, WARMUP_SAMPLE_COUNT)
        sample_windows = _collect_windows(driver, SAMPLE_COUNT)
        active_performance = _combine_windows(warmup_windows + sample_windows)
        if (
            active_performance["renderedFrames"] <= 0
            or active_performance["drawCallsMax"] <= 0
            or active_performance["trianglesMax"] <= 0
            or active_performance["canvasPixelsMax"] <= 0
            or active_performance["modelBuilds"] <= 0
        ):
            raise CollectorError("viewer produced no bounded geometry/render work")

        if _execute(driver, COLLAPSE_SCRIPT) is not True:
            raise CollectorError("viewer/operations collapse command returned unsupported response")
        inactive = _wait(driver, lambda item: (
            item.operations_collapsed
            and item.viewer_present
            and item.qualification["status"]["canvasCount"] == 0
            and item.qualification["status"]["viewerActive"] is False
            and item.qualification["status"]["headless"]["activeSockets"] == 1
            and item.qualification["status"]["headless"]["maxActiveSockets"] == 1
        ), "inactive viewer demand with shell socket retained", timeout_s)
        # Discard the transition/unmount window.  Collapsing the pane can leave
        # one final demand frame in the window that observed inactivity.
        _take(driver)
        time.sleep(POLL_INTERVAL_S)
        inactive_window = _take(driver)
        if (
            inactive_window["status"]["viewerActive"] is not False
            or inactive_window["status"]["canvasCount"] != 0
            or inactive_window["performance"]["renderedFrames"] != 0
        ):
            raise CollectorError("inactive viewer continued to render")
        if _execute(driver, EXPAND_SCRIPT) is not True:
            raise CollectorError("viewer/operations expand command returned unsupported response")
        returned = _wait(
            driver,
            _viewer_ready,
            "returned single viewer Canvas and shell socket",
            timeout_s,
        )
        _require_ready(returned)
        returned_topology = _topology(returned)
        if (
            returned_topology["sourceCount"] != initial["sourceCount"]
            or returned_topology["socketCreations"] != initial["socketCreations"]
            or returned_topology["canvasCount"] != 1
        ):
            raise CollectorError("viewer presentation changed HeadlessStateSource topology")

        driver.command("POST", "/url", {"url": target_url})
        _wait(
            driver,
            lambda item: (
                item.connected
                and item.operations_pane_present
                and not item.operations_collapsed
                and item.viewer_present
            ),
            "reloaded connected workbench viewer and operations pane",
            timeout_s,
        )
        reloaded = _wait(
            driver,
            lambda item: _viewer_ready(item)
            and item.qualification["status"]["headless"]["maxActiveSockets"] <= 1,
            "reload viewer topology",
            timeout_s,
        )
        _require_ready(reloaded)
        reload_topology = _topology(reloaded)
        if reload_topology["maxActiveSockets"] != 1:
            raise CollectorError("reload accumulated duplicate active sockets")

        final_performance = _combine_windows([_validate_qualification(
            _execute(driver, TAKE_SCRIPT)
        )])
        long_tasks = {
            "supported": active_performance["longTaskSupported"],
            "count": active_performance["longTasks"]
            if active_performance["longTaskSupported"]
            else None,
        }
        return {
            "schemaVersion": RESULT_SCHEMA,
            "success": True,
            "scope": "native-focused-idle-common-workbench",
            "optIn": f"{QUALIFICATION_QUERY_KEY}=1",
            "browser": reloaded.qualification["status"]["browser"],
            "renderer": reloaded.qualification["status"]["renderer"],
            "viewport": reloaded.qualification["status"]["viewport"],
            "topology": {
                "initial": initial,
                "inactive": _topology(inactive),
                "returned": returned_topology,
                "reload": reload_topology,
            },
            "warmupSampleCount": WARMUP_SAMPLE_COUNT,
            "sampleCount": SAMPLE_COUNT,
            "performance": active_performance,
            "reloadPerformance": final_performance,
            "longTasks": long_tasks,
            "claims": [
                "focused-idle native viewer-active/inactive presentation demand",
                "one Canvas and one shared shell HeadlessStateSource socket",
                "solid rendering, bounded browser timings, and reload topology",
            ],
            "excluded": [
                "Isaac/ROS/controller-manager",
                "measured ghost/point cloud/source flood",
                "viewer worker CPU, fanout, producer epoch, and lease",
            ],
        }
    finally:
        if driver.session_id is not None:
            driver.delete_session()


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("endpoint", nargs="?")
    parser.add_argument("composer", nargs="?")
    parser.add_argument(
        "--geckodriver-endpoint",
        "--webdriver-endpoint",
        "--webdriver-url",
        dest="endpoint_option",
    )
    parser.add_argument("--composer-url", dest="composer_option")
    parser.add_argument("--timeout-s", type=float, default=WAIT_TIMEOUT_S)
    return parser


def main(argv: list[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    endpoint = arguments.endpoint_option or arguments.endpoint
    composer = arguments.composer_option or arguments.composer
    if endpoint is None or composer is None:
        raise SystemExit("geckodriver endpoint and Composer URL are required")
    try:
        result = collect_current_workbench(endpoint, composer, timeout_s=arguments.timeout_s)
    except CollectorError as error:
        print(json.dumps({"schemaVersion": RESULT_SCHEMA, "success": False, "error": str(error)}))
        return 1
    print(json.dumps(result, separators=(",", ":"), sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
