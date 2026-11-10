"""The assistant host (ROS-free part): socket, serving, ``/health``, exit status.

The bridge is a small fake HTTP server started like the real one
(``<command> --fd <N>``); the real bridge isn't imported.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path
import socket
import sys
import textwrap
import threading
import time
import urllib.error
import urllib.request

import pytest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:  # run from the source tree without a colcon build
    sys.path.insert(0, str(ROOT))

from manyforge_ros_bringup import assistant_host as host_module  # noqa: E402
from manyforge_ros_bringup.assistant_host import (  # noqa: E402
    AssistantHost,
    LocalProcessBackend,
    audit_log_path,
    exit_status,
    health_busy,
    make_backend,
)

FAKE_BRIDGE = textwrap.dedent(
    """\
    import http.server, json, os, signal, socket, sys, threading
    from pathlib import Path

    root = Path(sys.argv[1])
    fd = int(sys.argv[sys.argv.index("--fd") + 1])
    in_flight = threading.Event()

    def terminated(signum, frame):
        if in_flight.is_set():
            (root / "cancelled").write_text("1")
        os._exit(0)

    signal.signal(signal.SIGTERM, terminated)
    (root / "audit").write_text(os.environ.get("MANYFORGE_ASSISTANT_AUDIT_LOG", ""))

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            if self.path == "/health":
                activity = (root / "activity").read_text()
                if activity == "broken":
                    self.send_response(500)
                    self.end_headers()
                    return
                body = json.dumps({"schemaVersion": "manyforge.assistant.bridge_health.v2",
                                   "status": "ready", "activity": activity}).encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            elif self.path.startswith("/exit/"):
                os._exit(int(self.path.rsplit("/", 1)[1]))
            elif self.path == "/slow":
                in_flight.set()
                threading.Event().wait()

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler,
                                             bind_and_activate=False)
    server.socket.close()
    server.socket = socket.socket(fileno=fd)
    server.serve_forever(poll_interval=0.05)
    """
)


class Recorder:
    """The host's publications, in order."""

    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.events: list[tuple[str, object]] = []

    def add(self, topic: str, value: object = None) -> None:
        with self.lock:
            self.events.append((topic, value))

    def values(self, topic: str) -> list[object]:
        with self.lock:
            return [value for name, value in self.events if name == topic]

    def snapshot(self) -> list[tuple[str, object]]:
        with self.lock:
            return list(self.events)


def wait_for(condition, timeout_s: float = 10.0) -> None:
    deadline = time.monotonic() + timeout_s
    while not condition():
        assert time.monotonic() < deadline, "condition not reached"
        time.sleep(0.01)


@pytest.fixture
def bridge(tmp_path: Path):
    script = tmp_path / "fake_bridge.py"
    script.write_text(FAKE_BRIDGE, encoding="utf-8")
    (tmp_path / "activity").write_text("idle")
    return tmp_path, [sys.executable, str(script), str(tmp_path)]


class Running:
    def __init__(self, backend: LocalProcessBackend, health_period_s: float = 0.05) -> None:
        self.recorder = Recorder()
        self.backend = backend
        self.host = AssistantHost(
            backend,
            lambda url: self.recorder.add("endpoint", url),
            lambda busy: self.recorder.add("busy", busy),
            lambda: self.recorder.add("health"),
            health_period_s,
        )
        self.status: list[int] = []
        self.thread = threading.Thread(target=lambda: self.status.append(self.host.run()))
        self.thread.start()

    def endpoint(self) -> object:
        values = self.recorder.values("endpoint")
        return values[-1] if values else None

    def join(self) -> int:
        self.thread.join(15.0)
        assert not self.thread.is_alive()
        return self.status[0]


def _get(url: str, timeout_s: float = 2.0) -> bytes:
    with urllib.request.urlopen(url, timeout=timeout_s) as response:
        return response.read()


def test_socket_is_bound_before_start_and_endpoint_published_only_while_serving(bridge):
    root, command = bridge
    running = Running(LocalProcessBackend(command, {"ROS_LOG_DIR": str(root / "ros")}))
    port = int(running.host.endpoint.rsplit(":", 1)[1])
    assert running.host.endpoint == f"http://127.0.0.1:{port}"
    # Bound and listening before any bridge runs; nothing is published but "".
    with socket.create_connection(("127.0.0.1", port), timeout=2.0):
        pass
    assert running.recorder.snapshot() == [("endpoint", ""), ("busy", False)]
    assert running.backend.exited() is None
    running.host.request_serving(True)
    wait_for(lambda: running.endpoint() == running.host.endpoint)
    events = running.recorder.snapshot()
    # A /health answer precedes the endpoint.
    assert events.index(("health", None)) < events.index(("endpoint", running.host.endpoint))
    assert b'"activity": "idle"' in _get(running.host.endpoint + "/health")
    assert (root / "audit").read_text() == str(root / "assistant-audit.jsonl")
    running.host.request_serving(False)
    wait_for(lambda: running.endpoint() == "")
    wait_for(lambda: running.backend._process is None)
    with pytest.raises(OSError):
        _get(running.host.endpoint + "/health", timeout_s=0.5)
    # Serving again reuses the same socket.
    running.host.request_serving(True)
    wait_for(lambda: running.endpoint() == running.host.endpoint)
    running.host.request_stop()
    assert running.join() == 0
    assert running.endpoint() == ""


def test_health_activity_maps_to_busy_and_health_only_on_success(bridge):
    root, command = bridge
    running = Running(LocalProcessBackend(command, {}))
    running.host.request_serving(True)
    wait_for(lambda: running.endpoint() == running.host.endpoint)
    (root / "activity").write_text("busy")
    wait_for(lambda: running.recorder.values("busy")[-1] is True)
    (root / "activity").write_text("idle")
    wait_for(lambda: running.recorder.values("busy")[-1] is False)
    # Published only on change.
    assert running.recorder.values("busy") == [False, True, False]
    (root / "activity").write_text("broken")
    time.sleep(0.2)  # let an in-flight answer land
    answers = len(running.recorder.values("health"))
    time.sleep(0.3)
    assert len(running.recorder.values("health")) == answers
    (root / "activity").write_text("idle")
    wait_for(lambda: len(running.recorder.values("health")) > answers)
    running.host.request_stop()
    assert running.join() == 0


def test_bridge_exit_while_serving_exits_the_host_non_zero(bridge):
    _, command = bridge
    running = Running(LocalProcessBackend(command, {}))
    running.host.request_serving(True)
    wait_for(lambda: running.endpoint() == running.host.endpoint)
    with pytest.raises(OSError):
        _get(running.host.endpoint + "/exit/3")
    assert running.join() == 3
    assert running.endpoint() == ""


def test_ordered_stop_while_serving_stops_the_bridge_and_exits_zero(bridge):
    _, command = bridge
    running = Running(LocalProcessBackend(command, {}))
    running.host.request_serving(True)
    wait_for(lambda: running.endpoint() == running.host.endpoint)
    process = running.backend._process
    running.host.request_stop()
    assert running.join() == 0
    assert process.poll() == 0
    assert running.endpoint() == ""


def test_stop_serving_cancels_a_request_in_flight(bridge):
    root, command = bridge
    running = Running(LocalProcessBackend(command, {}))
    running.host.request_serving(True)
    wait_for(lambda: running.endpoint() == running.host.endpoint)
    failures: list[BaseException] = []

    def slow() -> None:
        try:
            _get(running.host.endpoint + "/slow", timeout_s=10.0)
        except (OSError, urllib.error.URLError) as error:
            failures.append(error)

    client = threading.Thread(target=slow)
    client.start()
    time.sleep(0.2)
    running.host.request_serving(False)
    client.join(10.0)
    assert not client.is_alive()
    assert failures
    assert (root / "cancelled").read_text() == "1"
    assert running.endpoint() == ""
    running.host.request_stop()
    assert running.join() == 0


def test_a_bridge_that_ignores_sigterm_is_killed_after_the_grace(tmp_path: Path):
    script = tmp_path / "stubborn.py"
    script.write_text(
        "import signal, sys, time\n"
        "signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
        "while True:\n    time.sleep(1)\n",
        encoding="utf-8",
    )
    backend = LocalProcessBackend([sys.executable, str(script)], {}, stop_grace_s=0.3)
    listener = backend.prepare()
    backend.start_serving(listener)
    process = backend._process
    time.sleep(0.2)
    backend.stop_serving()
    assert process.returncode == -9
    listener.close()


def test_health_parsing_exit_status_audit_path_and_backends() -> None:
    def answer(status: int, body: bytes):
        return lambda url, timeout_s: (status, body)

    ok = b'{"schemaVersion": "manyforge.assistant.bridge_health.v2", "status": "ready", '
    assert health_busy(answer(200, ok + b'"activity": "busy"}'), "u") is True
    assert health_busy(answer(200, ok + b'"activity": "idle"}'), "u") is False
    assert health_busy(answer(200, ok + b'"activity": "unknown"}'), "u") is None
    assert health_busy(answer(500, ok + b'"activity": "idle"}'), "u") is None
    assert health_busy(answer(200, b'{"status": "ready", "activity": "idle"}'), "u") is None
    assert health_busy(answer(200, b"not json"), "u") is None

    def unreachable(url, timeout_s):
        raise ConnectionRefusedError()

    assert health_busy(unreachable, "u") is None
    assert exit_status(0) == 1
    assert exit_status(3) == 3
    assert exit_status(-9) == 137
    assert audit_log_path({"ROS_LOG_DIR": "/run/artifacts/assistant/ros"}) == (
        "/run/artifacts/assistant/assistant-audit.jsonl"
    )
    assert audit_log_path({}) is None
    assert isinstance(make_backend("local_process"), LocalProcessBackend)
    with pytest.raises(ValueError, match="openshell"):
        make_backend("openshell")
    assert host_module.CONFIG_INVALID_EXIT_STATUS == 78


def test_health_period_out_of_range_is_refused(bridge):
    _, command = bridge
    for period in (0.01, 61.0, float("nan")):
        with pytest.raises(ValueError, match="health_period_s"):
            AssistantHost(LocalProcessBackend(command, {}), print, print, print, period)


def test_launch_runs_the_host_with_the_component_settings() -> None:
    pytest.importorskip("launch_ros")
    spec = importlib.util.spec_from_file_location(
        "assistant_launch", ROOT / "launch/assistant.launch.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    kwargs = module.node_kwargs("/manyforge/assistant", "local_process", "1.0")
    assert kwargs["executable"] == "assistant_host"
    assert kwargs["name"] == "assistant_host"
    assert kwargs["namespace"] == "/manyforge/assistant"
    assert kwargs["parameters"] == [{"backend": "local_process", "health_period_s": 1.0}]
    with pytest.raises(ValueError, match="namespace"):
        module.node_kwargs("assistant", "local_process", "1.0")
    with pytest.raises(ValueError, match="backend"):
        module.node_kwargs("/ns", "", "1.0")
