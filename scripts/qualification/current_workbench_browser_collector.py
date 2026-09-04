"""Bounded native common-workbench qualification over raw W3C WebDriver HTTP.

The browser and Composer are owned by the caller.  This collector creates one
fresh Firefox WebDriver session, records only a compact qualification result,
and always deletes that exact session before returning.
"""
from __future__ import annotations

import argparse
import base64
from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path
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
MAX_SCREENSHOT_BYTES = 32 * 1024 * 1024
MAX_SCREENSHOT_RESPONSE_BYTES = (MAX_SCREENSHOT_BYTES * 4 // 3) + 4096
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
    active_workspace: str
    active_workspace_present: bool


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
    if headless["socketPath"] != "/ws/scene/viewer":
        raise CollectorError(
            "qualification did not observe the dedicated viewer socket"
        )
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
            "activeWorkspace",
            "activeWorkspacePresent",
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
        "activeWorkspacePresent",
    ):
        if not isinstance(result[key], bool):
            raise CollectorError(f"{key} is not boolean")
    if result["activeWorkspace"] not in {"program", "scene", "deployment"}:
        raise CollectorError("activeWorkspace is unsupported")
    return _Observation(
        qualification=qualification,
        connected=result["connected"],
        operations_pane_present=result["operationsPanePresent"],
        operations_collapsed=result["operationsCollapsed"],
        viewer_present=result["viewerPresent"],
        wireframe_unchecked=result["wireframeUnchecked"],
        active_workspace=result["activeWorkspace"],
        active_workspace_present=result["activeWorkspacePresent"],
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
        *,
        max_response_bytes: int = MAX_RESPONSE_BYTES,
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
                raw = response.read(max_response_bytes + 1)
        except HTTPError as error:
            raw = error.read(max_response_bytes + 1)
            detail = ""
            try:
                decoded_error = json.loads(raw.decode("utf-8"))
                value = decoded_error.get("value")
                if isinstance(value, dict):
                    parts = [
                        item for item in (value.get("error"), value.get("message"))
                        if isinstance(item, str) and item
                    ]
                    detail = f": {' · '.join(parts)}" if parts else ""
            except (AttributeError, UnicodeDecodeError, json.JSONDecodeError):
                pass
            raise CollectorError(
                f"WebDriver {method} {path} failed{detail}"
            ) from error
        except (URLError, TimeoutError, OSError) as error:
            raise CollectorError(f"WebDriver {method} {path} failed") from error
        if len(raw) > max_response_bytes:
            raise CollectorError("WebDriver response is oversized")
        try:
            decoded = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise CollectorError("WebDriver returned malformed JSON") from error
        if not isinstance(decoded, dict) or "value" not in decoded:
            raise CollectorError("WebDriver returned an unsupported command response")
        return decoded["value"]

    def create_session(
        self,
        *,
        firefox_args: tuple[str, ...] = ("-headless",),
    ) -> str:
        if not firefox_args or any(
            not isinstance(argument, str) or not argument
            for argument in firefox_args
        ):
            raise CollectorError("Firefox arguments must be non-empty strings")
        value = self._request(
            "POST",
            "/session",
            {
                "capabilities": {
                    "alwaysMatch": {
                        "browserName": "firefox",
                        "moz:firefoxOptions": {"args": list(firefox_args)},
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

    def command(
        self,
        method: str,
        path: str,
        payload: dict[str, Any] | None = None,
        *,
        max_response_bytes: int = MAX_RESPONSE_BYTES,
    ) -> Any:
        if self.session_id is None:
            raise CollectorError("WebDriver session is not active")
        return self._request(
            method,
            f"/session/{quote(self.session_id, safe='')}{path}",
            payload,
            max_response_bytes=max_response_bytes,
        )

    def delete_session(self) -> None:
        session_id = self.session_id
        self.session_id = None
        if session_id is not None:
            self._request("DELETE", f"/session/{quote(session_id, safe='')}")

    def set_window_rect(self, width: int, height: int) -> None:
        value = self.command(
            "POST",
            "/window/rect",
            {"width": width, "height": height},
        )
        if not isinstance(value, dict):
            raise CollectorError("WebDriver window rectangle response is malformed")

    def screenshot_png(self) -> bytes:
        value = self.command(
            "GET",
            "/screenshot",
            max_response_bytes=MAX_SCREENSHOT_RESPONSE_BYTES,
        )
        if not isinstance(value, str):
            raise CollectorError("WebDriver screenshot response is malformed")
        try:
            screenshot = base64.b64decode(value, validate=True)
        except (ValueError, TypeError) as error:
            raise CollectorError("WebDriver screenshot is not valid base64") from error
        if (
            not screenshot.startswith(b"\x89PNG\r\n\x1a\n")
            or len(screenshot) > MAX_SCREENSHOT_BYTES
        ):
            raise CollectorError("WebDriver screenshot is not a bounded PNG")
        return screenshot


OBSERVATION_SCRIPT = f"""return (() => {{
  const q = window.{QUALIFICATION_GLOBAL};
  const shell = document.querySelector('.workbench');
  const operations = document.querySelector('.workbench-commands');
  const pane = operations?.querySelector('.workbench-pane');
  const activeWorkspace = document.querySelector('.workspace-switcher [data-workspace][aria-pressed="true"]')?.dataset.workspace ?? null;
  const activeWorkspacePresent = activeWorkspace === 'program'
    ? document.querySelector('[aria-label="Program authoring workspace"]') !== null
    : activeWorkspace === 'scene'
      ? document.querySelector('.scene-workspace-host > .scene-authoring-workspace') !== null
      : activeWorkspace === 'deployment'
        ? document.querySelector('.deployment-workspace') !== null
        : false;
  return {{
    qualification: q === undefined ? null : q.snapshot(),
    connected: document.querySelector('.deployment-status-button[data-backend-connected="true"]') !== null,
    operationsPanePresent: operations !== null && pane !== null,
    operationsCollapsed: shell?.classList.contains('commands-collapsed') === true,
    viewerPresent: pane?.querySelector('.viewer-panel') !== null,
    wireframeUnchecked: document.querySelector('input[aria-label="Wireframe overlays"]')?.checked === false,
    activeWorkspace,
    activeWorkspacePresent
  }};
}})();"""
TAKE_SCRIPT = f"return window.{QUALIFICATION_GLOBAL}?.take() ?? null;"
QUALIFICATION_READY_SCRIPT = (
    f"return typeof window.{QUALIFICATION_GLOBAL}?.snapshot === 'function';"
)


def _select_workspace_script(workspace: str) -> str:
    return (
        "return (() => { const node = document.querySelector("
        f"'[data-workspace=\"{workspace}\"]'"
        "); if (!node) return false; node.click(); return true; })();"
    )


OPEN_VIEWER_TOOLS_SCRIPT = """return (() => {
  const button = [...document.querySelectorAll('.viewer-surface-launchers > button')]
    .find((candidate) => candidate.textContent?.trim() === 'Tools');
  if (button === undefined) return false;
  if (button.getAttribute('aria-pressed') !== 'true') button.click();
  return true;
})();"""
IDLE_CONTROL_READY_SCRIPT = """return (() => {
  const run = document.querySelector('.viewer-transport .transport-run');
  const velocity = document.querySelector('.viewer-transport .transport-velocity input');
  const loadingNotice = [...document.querySelectorAll('.program-workspace-notice')]
    .some((notice) => /Loading|Reading the Program workspace/i.test(notice.textContent ?? ''));
  return run instanceof HTMLButtonElement
    && !run.disabled
    && velocity instanceof HTMLInputElement
    && !velocity.disabled
    && !loadingNotice;
})();"""


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


def _wait_for_qualification(driver: RawWebDriver, timeout_s: float) -> None:
    """Wait for the lazily loaded viewer to install its strict probe."""
    deadline = time.monotonic() + min(timeout_s, WAIT_TIMEOUT_S)
    while True:
        if _execute(driver, QUALIFICATION_READY_SCRIPT) is True:
            return
        if time.monotonic() >= deadline:
            raise CollectorError("timed out waiting for the viewer qualification probe")
        time.sleep(POLL_INTERVAL_S)


def _wait_for_idle_controls(driver: RawWebDriver, timeout_s: float) -> None:
    """Require automatic owner-proven Idle readiness without an operator retry."""
    deadline = time.monotonic() + min(timeout_s, WAIT_TIMEOUT_S)
    while True:
        if _execute(driver, IDLE_CONTROL_READY_SCRIPT) is True:
            return
        if time.monotonic() >= deadline:
            raise CollectorError(
                "timed out waiting for automatic native Idle controls readiness"
            )
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
        or not observation.active_workspace_present
    ):
        raise CollectorError("Composer workbench is not connected with its viewer/operations pane available")
    if not observation.wireframe_unchecked:
        raise CollectorError("wireframe control is checked by default")
    if status["canvasCount"] != 1:
        raise CollectorError("qualification requires exactly one viewer Canvas")
    if (
        status["headless"]["socketPath"] != "/ws/scene/viewer"
        or status["headless"]["sourceCount"] != 1
        or status["headless"]["activeSockets"] != 1
        or status["headless"]["maxActiveSockets"] != 1
    ):
        raise CollectorError("qualification requires exactly one active viewer-observation socket")
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
    viewport_width: int | None = None,
    viewport_height: int | None = None,
    screenshot_path: Path | None = None,
) -> dict[str, Any]:
    """Collect one bounded native focused-idle common-workbench result."""
    if (viewport_width is None) != (viewport_height is None):
        raise CollectorError("viewport width and height must be supplied together")
    if viewport_width is not None and not 320 <= viewport_width <= 7680:
        raise CollectorError("viewport width must be between 320 and 7680 pixels")
    if viewport_height is not None and not 320 <= viewport_height <= 4320:
        raise CollectorError("viewport height must be between 320 and 4320 pixels")
    driver = RawWebDriver(
        geckodriver_endpoint,
        timeout_s=min(5.0, max(0.1, timeout_s)),
    )
    try:
        driver.create_session()
        if viewport_width is not None and viewport_height is not None:
            driver.set_window_rect(viewport_width, viewport_height)
        target_url = _url_with_opt_in(composer_url)
        driver.command("POST", "/url", {"url": target_url})
        _wait_for_qualification(driver, timeout_s)
        _wait(
            driver,
            lambda item: (
                item.connected
                and item.operations_pane_present
                and not item.operations_collapsed
                and item.viewer_present
                and item.active_workspace == "program"
            ),
            "connected workbench viewer and operations pane",
            timeout_s,
        )
        if _execute(driver, OPEN_VIEWER_TOOLS_SCRIPT) is not True:
            raise CollectorError("viewer Tools panel selection is unavailable")
        ready = _wait(
            driver,
            _viewer_ready,
            "connected solid viewer",
            timeout_s,
        )
        _require_ready(ready)
        _wait_for_idle_controls(driver, timeout_s)
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

        workspace_topology: dict[str, dict[str, Any]] = {}
        for workspace in ("scene", "deployment", "program"):
            if _execute(driver, _select_workspace_script(workspace)) is not True:
                raise CollectorError(f"{workspace} workspace selection is unavailable")
            selected = _wait(
                driver,
                lambda item, expected=workspace: (
                    _viewer_ready(item) and item.active_workspace == expected
                ),
                f"persistent viewer in {workspace} workspace",
                timeout_s,
            )
            _require_ready(selected)
            topology = _topology(selected)
            if (
                topology["sourceCount"] != initial["sourceCount"]
                or topology["socketCreations"] != initial["socketCreations"]
                or topology["canvasCount"] != 1
            ):
                raise CollectorError(
                    f"{workspace} selection changed viewer/HeadlessStateSource topology"
                )
            workspace_topology[workspace] = topology

        driver.command("POST", "/url", {"url": target_url})
        _wait_for_qualification(driver, timeout_s)
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
        if _execute(driver, OPEN_VIEWER_TOOLS_SCRIPT) is not True:
            raise CollectorError("reloaded viewer Tools panel selection is unavailable")
        reloaded = _wait(
            driver,
            lambda item: _viewer_ready(item)
            and item.qualification["status"]["headless"]["maxActiveSockets"] <= 1,
            "reload viewer topology",
            timeout_s,
        )
        _require_ready(reloaded)
        _wait_for_idle_controls(driver, timeout_s)
        reload_topology = _topology(reloaded)
        if reload_topology["maxActiveSockets"] != 1:
            raise CollectorError("reload accumulated duplicate active sockets")

        final_performance = _combine_windows([_validate_qualification(
            _execute(driver, TAKE_SCRIPT)
        )])
        viewport = reloaded.qualification["status"]["viewport"]
        if viewport_width is not None and viewport["width"] != viewport_width:
            raise CollectorError(
                "browser viewport width does not match the requested qualification width"
            )
        screenshot_sha256 = None
        if screenshot_path is not None:
            screenshot = driver.screenshot_png()
            screenshot_path.parent.mkdir(parents=True, exist_ok=True)
            screenshot_path.write_bytes(screenshot)
            screenshot_sha256 = hashlib.sha256(screenshot).hexdigest()
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
            "viewport": viewport,
            "requestedViewport": (
                {"width": viewport_width, "height": viewport_height}
                if viewport_width is not None and viewport_height is not None
                else None
            ),
            "screenshotSha256": screenshot_sha256,
            "topology": {
                "initial": initial,
                "workspaces": workspace_topology,
                "reload": reload_topology,
            },
            "warmupSampleCount": WARMUP_SAMPLE_COUNT,
            "sampleCount": SAMPLE_COUNT,
            "performance": active_performance,
            "reloadPerformance": final_performance,
            "longTasks": long_tasks,
            "claims": [
                "focused-idle persistent viewer across Program/Scene/Deployment",
                "one Canvas and one shared shell HeadlessStateSource socket",
                "solid rendering, bounded browser timings, and reload topology",
                "automatic native Idle Run and velocity readiness without operator retry",
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
    parser.add_argument("--viewport-width", type=int)
    parser.add_argument("--viewport-height", type=int)
    parser.add_argument("--screenshot", type=Path)
    return parser


def main(argv: list[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    endpoint = arguments.endpoint_option or arguments.endpoint
    composer = arguments.composer_option or arguments.composer
    if endpoint is None or composer is None:
        raise SystemExit("geckodriver endpoint and Composer URL are required")
    try:
        result = collect_current_workbench(
            endpoint,
            composer,
            timeout_s=arguments.timeout_s,
            viewport_width=arguments.viewport_width,
            viewport_height=arguments.viewport_height,
            screenshot_path=arguments.screenshot,
        )
    except CollectorError as error:
        print(json.dumps({"schemaVersion": RESULT_SCHEMA, "success": False, "error": str(error)}))
        return 1
    print(json.dumps(result, separators=(",", ":"), sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
