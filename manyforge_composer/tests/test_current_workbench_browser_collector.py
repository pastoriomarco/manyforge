from __future__ import annotations

import base64
import hashlib
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Thread
from typing import Any

import pytest

from scripts.qualification import current_workbench_browser_collector as collector

_REAL_RAW_WEBDRIVER = collector.RawWebDriver
_PNG = b"\x89PNG\r\n\x1a\nfixture"


def _performance(*, active: bool = True, long_tasks: bool = False) -> dict[str, Any]:
    return {
        "windowDurationMs": 40.0,
        "modelBuilds": 1 if active else 0,
        "modelBuildTotalMs": 2.0 if active else 0.0,
        "modelBuildMaxMs": 2.0 if active else 0.0,
        "reactCommits": 2 if active else 0,
        "reactCommitTotalMs": 3.0 if active else 0.0,
        "reactCommitMaxMs": 2.0 if active else 0.0,
        "renderedFrames": 2 if active else 0,
        "drawCallsMax": 4 if active else 0,
        "trianglesMax": 120 if active else 0,
        "canvasPixelsMax": 640_000 if active else 0,
        "renderIntervalP95Ms": 16.0 if active else None,
        "renderIntervalMaxMs": 17.0 if active else None,
        "longTaskSupported": long_tasks,
        "longTasks": 0,
        "longTaskTotalMs": 0.0,
        "longTaskMaxMs": 0.0,
    }


def _snapshot(
    *,
    active: bool = True,
    long_tasks: bool = False,
    viewport: tuple[int, int] = (1280, 720),
) -> dict[str, Any]:
    status = {
        "schemaVersion": collector.QUALIFICATION_SCHEMA,
        "viewerActive": active,
        "canvasCount": 1 if active else 0,
        "wireframeDefault": False,
        "wireframeVisible": False,
        "model": {"ready": active, "geometryCount": 4 if active else 0},
        "renderer": (
            {
                "vendor": "Test Vendor",
                "renderer": "Test Renderer",
                "software": False,
                "effectiveDpr": 1.0,
                "antialias": True,
                "preserveDrawingBuffer": False,
            }
            if active
            else None
        ),
        "browser": {"userAgent": "Fake Firefox", "platform": "test"},
        "viewport": {
            "width": viewport[0],
            "height": viewport[1],
            "devicePixelRatio": 1.0,
        },
        "headless": {
            "socketPath": "/ws/state",
            "sourceCount": 1,
            "activeSockets": 1,
            "socketCreations": 1,
            "maxActiveSockets": 1,
        },
        "errors": [],
    }
    return {
        "schemaVersion": collector.QUALIFICATION_SCHEMA,
        "status": status,
        "performance": _performance(active=active, long_tasks=long_tasks),
    }


class _FakeDriver:
    instances: list["_FakeDriver"] = []
    long_tasks = False

    def __init__(self, endpoint: str, timeout_s: float = 5.0) -> None:
        self.endpoint = endpoint
        self.timeout_s = timeout_s
        self.session_id: str | None = None
        self.active_workspace = "program"
        self.viewport = (1280, 720)
        self.deleted = False
        self.commands: list[str] = []
        self.scripts: list[str] = []
        self.__class__.instances.append(self)

    def create_session(self) -> str:
        self.session_id = "exact-session"
        return self.session_id

    def set_window_rect(self, width: int, height: int) -> None:
        self.command(
            "POST",
            "/window/rect",
            {"width": width, "height": height},
        )

    def screenshot_png(self) -> bytes:
        encoded = self.command("GET", "/screenshot")
        assert isinstance(encoded, str)
        return base64.b64decode(encoded)

    def command(self, method: str, path: str, payload: dict[str, Any] | None = None) -> Any:
        self.commands.append(path)
        if path == "/url":
            self.active_workspace = "program"
            return None
        if path == "/window/rect":
            assert payload is not None
            self.viewport = (int(payload["width"]), int(payload["height"]))
            return {
                "x": 0,
                "y": 0,
                "width": self.viewport[0],
                "height": self.viewport[1],
            }
        if path == "/screenshot":
            return base64.b64encode(_PNG).decode("ascii")
        if path == "/execute/sync":
            script = str((payload or {}).get("script"))
            self.scripts.append(script)
            if script == collector.QUALIFICATION_READY_SCRIPT:
                return True
            if script == collector.OBSERVATION_SCRIPT:
                snapshot = _snapshot(
                    active=True,
                    long_tasks=self.long_tasks,
                    viewport=self.viewport,
                )
                return {
                    "qualification": snapshot,
                    "connected": True,
                    "operationsPanePresent": True,
                    "operationsCollapsed": False,
                    "viewerPresent": True,
                    "wireframeUnchecked": True,
                    "activeWorkspace": self.active_workspace,
                    "activeWorkspacePresent": True,
                }
            if script == collector.TAKE_SCRIPT:
                snapshot = _snapshot(
                    active=True,
                    long_tasks=self.long_tasks,
                    viewport=self.viewport,
                )
                return snapshot
            if script == collector.OPEN_VIEWER_TOOLS_SCRIPT:
                return True
            for workspace in ("program", "scene", "deployment"):
                if script == collector._select_workspace_script(workspace):
                    self.active_workspace = workspace
                    return True
        raise AssertionError(f"unexpected WebDriver command: {method} {path}")

    def delete_session(self) -> None:
        assert self.session_id == "exact-session"
        self.deleted = True
        self.session_id = None


@pytest.fixture(autouse=True)
def _fake_driver_reset(monkeypatch: pytest.MonkeyPatch) -> None:
    _FakeDriver.instances.clear()
    monkeypatch.setattr(collector, "RawWebDriver", _FakeDriver)
    monkeypatch.setattr(collector.time, "sleep", lambda _seconds: None)


def test_collector_cleans_up_exact_session_on_success_and_uses_exact_opt_in() -> None:
    result = collector.collect_current_workbench("http://webdriver.test", "http://composer.test/")

    driver = _FakeDriver.instances[0]
    assert driver.deleted is True
    assert f"{collector.QUALIFICATION_QUERY_KEY}=1" == result["optIn"]
    assert result["topology"]["initial"]["canvasCount"] == 1
    assert result["topology"]["initial"]["socketPath"] == "/ws/state"
    assert set(result["topology"]["workspaces"]) == {"program", "scene", "deployment"}
    assert all(
        topology["canvasCount"] == 1
        for topology in result["topology"]["workspaces"].values()
    )
    assert result["topology"]["reload"]["maxActiveSockets"] == 1
    for workspace in ("program", "scene", "deployment"):
        assert driver.scripts.count(collector._select_workspace_script(workspace)) == 1
    assert driver.scripts.count(collector.QUALIFICATION_READY_SCRIPT) == 2
    assert driver.scripts.count(collector.OPEN_VIEWER_TOOLS_SCRIPT) == 2
    assert result["longTasks"] == {"supported": False, "count": None}


def test_live_observation_uses_the_single_integrated_runtime_status_entry() -> None:
    assert '.runtime-status-button[data-backend-connected="true"]' in collector.OBSERVATION_SCRIPT
    assert ".header-boundary" not in collector.OBSERVATION_SCRIPT
    assert 'input[aria-label="Wireframe overlays"]' in collector.OBSERVATION_SCRIPT
    assert ".viewer-surface-launchers > button" in collector.OPEN_VIEWER_TOOLS_SCRIPT


def test_collector_sets_and_proves_viewport_and_writes_bounded_screenshot(
    tmp_path: Path,
) -> None:
    screenshot = tmp_path / "ui1-1920.png"
    result = collector.collect_current_workbench(
        "http://webdriver.test",
        "http://composer.test/",
        viewport_width=1920,
        viewport_height=1080,
        screenshot_path=screenshot,
    )

    assert result["requestedViewport"] == {"width": 1920, "height": 1080}
    assert result["viewport"] == {
        "width": 1920,
        "height": 1080,
        "devicePixelRatio": 1.0,
    }
    assert result["screenshotSha256"] == hashlib.sha256(_PNG).hexdigest()
    assert screenshot.read_bytes() == _PNG
    assert "/window/rect" in _FakeDriver.instances[0].commands
    assert "/screenshot" in _FakeDriver.instances[0].commands


@pytest.mark.parametrize(
    ("width", "height", "message"),
    [
        (1920, None, "supplied together"),
        (319, 900, "viewport width"),
        (1440, 319, "viewport height"),
    ],
)
def test_collector_rejects_incomplete_or_unbounded_viewport(
    width: int | None,
    height: int | None,
    message: str,
) -> None:
    with pytest.raises(collector.CollectorError, match=message):
        collector.collect_current_workbench(
            "http://webdriver.test",
            "http://composer.test/",
            viewport_width=width,
            viewport_height=height,
        )


def test_workspace_switch_fails_if_it_reconstructs_the_shared_source(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    class ReconstructingDriver(_FakeDriver):
        def command(self, method: str, path: str, payload: dict[str, Any] | None = None) -> Any:
            value = super().command(method, path, payload)
            if (
                path == "/execute/sync"
                and str((payload or {}).get("script")) == collector.OBSERVATION_SCRIPT
                and self.active_workspace == "scene"
            ):
                assert isinstance(value, dict)
                value["qualification"]["status"]["headless"]["socketCreations"] = 2
            return value

    monkeypatch.setattr(collector, "RawWebDriver", ReconstructingDriver)
    with pytest.raises(collector.CollectorError, match="changed viewer/HeadlessStateSource"):
        collector.collect_current_workbench("http://webdriver.test", "http://composer.test/")
    assert ReconstructingDriver.instances[-1].deleted is True


def test_collector_cleans_up_exact_session_when_navigation_fails() -> None:
    class FailingDriver(_FakeDriver):
        def command(self, method: str, path: str, payload: dict[str, Any] | None = None) -> Any:
            if path == "/url":
                raise collector.CollectorError("navigation failed")
            return super().command(method, path, payload)

    monkeypatch = pytest.MonkeyPatch()
    monkeypatch.setattr(collector, "RawWebDriver", FailingDriver)
    try:
        with pytest.raises(collector.CollectorError, match="navigation failed"):
            collector.collect_current_workbench("http://webdriver.test", "http://composer.test/")
        assert FailingDriver.instances[0].deleted is True
    finally:
        monkeypatch.undo()


def test_exact_opt_in_replaces_old_value_and_rejects_malformed_duplicate_state() -> None:
    target = collector._url_with_opt_in(
        "http://composer.test/?manyforge_viewer_qualification=0&keep=x"
    )
    assert target == "http://composer.test/?keep=x&manyforge_viewer_qualification=1"
    with pytest.raises(collector.CollectorError):
        collector._validate_observation(
            {
                "qualification": None,
                "connected": True,
                "operationsPanePresent": True,
                "operationsCollapsed": False,
                "viewerPresent": True,
                "wireframeUnchecked": True,
                "activeWorkspace": "program",
                "activeWorkspacePresent": True,
            }
        )
    with pytest.raises(collector.CollectorError):
        collector._validate_performance({})


def test_unsupported_long_task_is_not_zero_success_evidence() -> None:
    window = _snapshot(long_tasks=False)
    combined = collector._combine_windows([window])
    assert combined["longTaskSupported"] is False
    assert {
        "supported": combined["longTaskSupported"],
        "count": combined["longTasks"] if combined["longTaskSupported"] else None,
    } == {"supported": False, "count": None}


def test_duplicate_canvas_or_socket_fails_closed() -> None:
    observation = collector._validate_observation(
        {
            "qualification": _snapshot(),
            "connected": True,
            "operationsPanePresent": True,
            "operationsCollapsed": False,
            "viewerPresent": True,
            "wireframeUnchecked": True,
            "activeWorkspace": "program",
            "activeWorkspacePresent": True,
        }
    )
    status = observation.qualification["status"]
    status["canvasCount"] = 2
    with pytest.raises(collector.CollectorError, match="exactly one"):
        collector._require_ready(observation)
    status["canvasCount"] = 1
    status["headless"]["activeSockets"] = 2
    with pytest.raises(collector.CollectorError, match="exactly one"):
        collector._require_ready(observation)


def test_polling_is_bounded() -> None:
    calls = 0

    def read(_driver: object) -> collector._Observation:
        nonlocal calls
        calls += 1
        return collector._validate_observation(
            {
                "qualification": _snapshot(),
                "connected": False,
                "operationsPanePresent": True,
                "operationsCollapsed": False,
                "viewerPresent": True,
                "wireframeUnchecked": True,
                "activeWorkspace": "program",
                "activeWorkspacePresent": True,
            }
        )

    original_read = collector._read
    original_monotonic = collector.time.monotonic
    collector._read = read  # type: ignore[assignment]
    ticks = iter([0.0, 0.2, 0.4, 1.1])
    collector.time.monotonic = lambda: next(ticks)
    try:
        with pytest.raises(collector.CollectorError, match="bounded"):
            collector._wait(object(), lambda item: item.connected, "bounded", 1.0)
    finally:
        collector._read = original_read
        collector.time.monotonic = original_monotonic
    assert calls <= 4


class _WebDriverHandler(BaseHTTPRequestHandler):
    requests: list[tuple[str, str, dict[str, Any] | None]] = []

    def do_POST(self) -> None:  # noqa: N802
        length = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(length)) if length else None
        self.__class__.requests.append(("POST", self.path, body))
        if self.path == "/session":
            self._reply({"value": {"sessionId": "http-session"}})
        else:
            self._reply({})

    def do_DELETE(self) -> None:  # noqa: N802
        self.__class__.requests.append(("DELETE", self.path, None))
        self._reply({"value": None})

    def _reply(self, payload: dict[str, Any]) -> None:
        encoded = json.dumps(payload).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def log_message(self, _format: str, *_args: object) -> None:
        return


def test_raw_webdriver_http_session_cleanup_and_unsupported_response() -> None:
    _WebDriverHandler.requests.clear()
    server = ThreadingHTTPServer(("127.0.0.1", 0), _WebDriverHandler)
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        driver = _REAL_RAW_WEBDRIVER(f"http://127.0.0.1:{server.server_port}")
        assert driver.create_session() == "http-session"
        driver.delete_session()
        assert ("DELETE", "/session/http-session", None) in _WebDriverHandler.requests
        with pytest.raises(collector.CollectorError, match="unsupported"):
            driver._request("POST", "/unsupported", {})
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=1.0)
