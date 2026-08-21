from __future__ import annotations

import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from threading import Thread
from typing import Any

import pytest

from scripts.qualification import current_workbench_browser_collector as collector

_REAL_RAW_WEBDRIVER = collector.RawWebDriver


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


def _snapshot(*, active: bool = True, long_tasks: bool = False) -> dict[str, Any]:
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
        "viewport": {"width": 1280, "height": 720, "devicePixelRatio": 1.0},
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
    inactive_take_frames: list[int] = []

    def __init__(self, endpoint: str, timeout_s: float = 5.0) -> None:
        self.endpoint = endpoint
        self.timeout_s = timeout_s
        self.session_id: str | None = None
        self.operations_collapsed = False
        self.inactive_take_frames = list(self.__class__.inactive_take_frames)
        self.deleted = False
        self.commands: list[str] = []
        self.scripts: list[str] = []
        self.__class__.instances.append(self)

    def create_session(self) -> str:
        self.session_id = "exact-session"
        return self.session_id

    def command(self, method: str, path: str, payload: dict[str, Any] | None = None) -> Any:
        self.commands.append(path)
        if path == "/url":
            self.operations_collapsed = False
            return None
        if path == "/execute/sync":
            script = str((payload or {}).get("script"))
            self.scripts.append(script)
            if script == collector.OBSERVATION_SCRIPT:
                snapshot = _snapshot(
                    active=not self.operations_collapsed,
                    long_tasks=self.long_tasks,
                )
                return {
                    "qualification": snapshot,
                    "connected": True,
                    "operationsPanePresent": True,
                    "operationsCollapsed": self.operations_collapsed,
                    "viewerPresent": True,
                    "wireframeUnchecked": True,
                }
            if script == collector.TAKE_SCRIPT:
                snapshot = _snapshot(
                    active=not self.operations_collapsed,
                    long_tasks=self.long_tasks,
                )
                if self.operations_collapsed and self.inactive_take_frames:
                    snapshot["performance"]["renderedFrames"] = self.inactive_take_frames.pop(0)
                return snapshot
            if script == collector.COLLAPSE_SCRIPT:
                self.operations_collapsed = True
                return True
            if script == collector.EXPAND_SCRIPT:
                self.operations_collapsed = False
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
    assert result["topology"]["inactive"]["canvasCount"] == 0
    assert result["topology"]["inactive"]["activeSockets"] == 1
    assert result["topology"]["returned"]["canvasCount"] == 1
    assert result["topology"]["reload"]["maxActiveSockets"] == 1
    assert driver.scripts.count(collector.COLLAPSE_SCRIPT) == 1
    assert driver.scripts.count(collector.EXPAND_SCRIPT) == 1
    assert result["longTasks"] == {"supported": False, "count": None}


def test_inactive_transition_frame_is_discarded_but_post_transition_frame_fails(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    class TransitionFrameDriver(_FakeDriver):
        inactive_take_frames = [1, 0]

    monkeypatch.setattr(collector, "RawWebDriver", TransitionFrameDriver)
    result = collector.collect_current_workbench("http://webdriver.test", "http://composer.test/")
    assert result["topology"]["inactive"]["canvasCount"] == 0
    assert TransitionFrameDriver.instances[-1].deleted is True

    class PostTransitionFrameDriver(_FakeDriver):
        inactive_take_frames = [1, 1]

    monkeypatch.setattr(collector, "RawWebDriver", PostTransitionFrameDriver)
    with pytest.raises(collector.CollectorError, match="continued to render"):
        collector.collect_current_workbench("http://webdriver.test", "http://composer.test/")
    assert PostTransitionFrameDriver.instances[-1].deleted is True


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
