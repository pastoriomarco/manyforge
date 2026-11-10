"""Host of the Composer AI chat bridge of one ManyForge ``assistant`` component.

The process of one ``assistant`` component (adapter family ``assistant``),
started by the execution host at Prepare. It owns the backend that runs the
bridge (``local_process``: a child Python process). At start it binds a
listening TCP socket on ``127.0.0.1:0`` and publishes an empty ``endpoint`` and
``busy`` false. While the adapter asks it to serve (``serving``, latched Bool)
it starts the bridge on that socket, polls its ``/health`` until it answers and
then publishes ``endpoint`` (latched String, ``http://127.0.0.1:<port>``). On
``serving`` false it clears the endpoint first, then stops the bridge (SIGTERM,
SIGKILL after a bound; the bridge's shutdown cancels its running turns).

While serving, its own thread polls ``/health`` every ``health_period_s``: each
answer is one ``health`` Header (the component's ``bridge`` stream) and sets
``busy`` (latched Bool) from the reported activity. If the bridge exits while
it should serve, the host exits with the bridge's exit status (never zero): the
component's ``fault_process_exited``. An ordered shutdown (SIGINT/SIGTERM)
stops the bridge and exits zero. A bad setting exits with status 78.

The bridge's audit log goes next to the host's ROS logs:
``MANYFORGE_ASSISTANT_AUDIT_LOG`` = the parent of ``ROS_LOG_DIR`` (the
component's launch artifacts folder) ``/assistant-audit.jsonl``.
"""

from __future__ import annotations

from collections.abc import Callable, Mapping, Sequence
import http.client
import json
import math
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import threading
from typing import Final, Protocol
import urllib.request

CONFIG_INVALID_EXIT_STATUS: Final = 78
HEALTH_SCHEMA: Final = "manyforge.assistant.bridge_health.v2"
AUDIT_LOG_ENV: Final = "MANYFORGE_ASSISTANT_AUDIT_LOG"
AUDIT_LOG_NAME: Final = "assistant-audit.jsonl"
BRIDGE_COMMAND: Final = (sys.executable, "-m", "manyforge_assistant_bridge.bridge")
# The bridge's shutdown cancels its turns and closes the audit log in well
# under a second; this only bounds a bridge that hangs.
STOP_GRACE_S: Final = 5.0
HEALTH_TIMEOUT_S: Final = 1.0
# How often the host asks a starting bridge whether it answers yet.
READY_POLL_S: Final = 0.05
_MAX_HEALTH_BYTES: Final = 16 * 1024

# (url, timeout_s) -> (status, body)
Fetch = Callable[[str, float], tuple[int, bytes]]


def http_get(url: str, timeout_s: float) -> tuple[int, bytes]:
    with urllib.request.urlopen(url, timeout=timeout_s) as response:
        return response.status, response.read(_MAX_HEALTH_BYTES)


def health_busy(fetch: Fetch, url: str) -> bool | None:
    """Whether the bridge runs a turn, or ``None`` when ``/health`` didn't answer v2."""

    try:
        status, body = fetch(url, HEALTH_TIMEOUT_S)
        value = json.loads(body)
    except (OSError, http.client.HTTPException, ValueError):
        return None
    if (
        status != 200
        or type(value) is not dict
        or value.get("schemaVersion") != HEALTH_SCHEMA
        or value.get("status") != "ready"
        or value.get("activity") not in ("busy", "idle")
    ):
        return None
    return value["activity"] == "busy"


def audit_log_path(environ: Mapping[str, str]) -> str | None:
    """The bridge's audit log next to the host's ROS logs; ``None`` without ``ROS_LOG_DIR``."""

    logs = environ.get("ROS_LOG_DIR")
    return str(Path(logs).parent / AUDIT_LOG_NAME) if logs else None


def exit_status(returncode: int) -> int:
    """The host's exit status for a bridge that exited while it should serve: never zero."""

    if returncode < 0:  # killed by a signal
        return 128 - returncode
    return returncode or 1


class Backend(Protocol):
    """Where the bridge runs. Every method runs on the host's worker thread."""

    def prepare(self) -> socket.socket:
        """The bound, listening socket the bridge will serve on."""

    def start_serving(self, listener: socket.socket) -> None: ...

    def stop_serving(self) -> None:
        """Stops the bridge; returns once it is gone (bounded)."""

    def health_url(self) -> str: ...

    def exited(self) -> int | None:
        """The bridge's exit code once it exited by itself; ``None`` while it runs."""


class LocalProcessBackend:
    """The bridge as a child process serving on the inherited socket (``--fd``)."""

    def __init__(
        self,
        command: Sequence[str] = BRIDGE_COMMAND,
        environ: Mapping[str, str] = os.environ,
        stop_grace_s: float = STOP_GRACE_S,
    ) -> None:
        self._command = list(command)
        self._environ = dict(environ)
        audit = audit_log_path(environ)
        if audit is not None:
            self._environ[AUDIT_LOG_ENV] = audit
        self._stop_grace_s = stop_grace_s
        self._port = 0
        self._process: subprocess.Popen[bytes] | None = None

    def prepare(self) -> socket.socket:
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.bind(("127.0.0.1", 0))
        listener.listen(socket.SOMAXCONN)
        listener.set_inheritable(True)
        self._port = listener.getsockname()[1]
        return listener

    def start_serving(self, listener: socket.socket) -> None:
        fd = listener.fileno()
        self._process = subprocess.Popen(
            [*self._command, "--fd", str(fd)], pass_fds=(fd,), env=self._environ
        )

    def stop_serving(self) -> None:
        process, self._process = self._process, None
        if process is None or process.poll() is not None:
            return
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(self._stop_grace_s)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()

    def health_url(self) -> str:
        return f"http://127.0.0.1:{self._port}/health"

    def exited(self) -> int | None:
        return None if self._process is None else self._process.poll()


BACKENDS: Final[Mapping[str, Callable[[], Backend]]] = {"local_process": LocalProcessBackend}


def make_backend(name: str) -> Backend:
    """The named backend; ``ValueError`` for an unknown one."""

    if name not in BACKENDS:
        raise ValueError(f"backend {name!r} is unknown (known: {', '.join(BACKENDS)})")
    return BACKENDS[name]()


class AssistantHost:
    """The host's logic, ROS-free: ``request_serving``/``request_stop`` only set
    flags; ``run`` does the backend work and the ``/health`` poll on its own
    thread and returns the host's exit status."""

    def __init__(
        self,
        backend: Backend,
        publish_endpoint: Callable[[str], None],
        publish_busy: Callable[[bool], None],
        publish_health: Callable[[], None],
        health_period_s: float,
        fetch: Fetch = http_get,
    ) -> None:
        if not math.isfinite(health_period_s) or not 0.05 <= health_period_s <= 60.0:
            raise ValueError("health_period_s must be in [0.05, 60]")
        self._backend = backend
        self._publish_endpoint = publish_endpoint
        self._publish_busy = publish_busy
        self._publish_health = publish_health
        self._health_period_s = health_period_s
        self._fetch = fetch
        self._wake = threading.Event()
        self._lock = threading.Lock()
        self._serving = False
        self._stopping = False
        self._busy = False
        self._listener = backend.prepare()
        self.endpoint = f"http://127.0.0.1:{self._listener.getsockname()[1]}"
        self._publish_endpoint("")
        self._publish_busy(False)

    def request_serving(self, serving: bool) -> None:
        with self._lock:
            self._serving = serving
        self._wake.set()

    def request_stop(self) -> None:
        with self._lock:
            self._stopping = True
        self._wake.set()

    def _wanted(self) -> tuple[bool, bool]:
        with self._lock:
            return self._serving, self._stopping

    def _set_busy(self, busy: bool) -> None:
        if busy != self._busy:
            self._busy = busy
            self._publish_busy(busy)

    def _stop_bridge(self) -> None:
        self._publish_endpoint("")
        self._backend.stop_serving()
        self._set_busy(False)

    def run(self) -> int:
        running = False  # the bridge was started
        served = False  # its endpoint is published
        try:
            while True:
                serving, stopping = self._wanted()
                if stopping:
                    if running:
                        self._stop_bridge()
                    return 0
                if running and not serving:
                    self._stop_bridge()
                    running = served = False
                elif serving and not running:
                    self._backend.start_serving(self._listener)
                    running = True
                if running:
                    returncode = self._backend.exited()
                    if returncode is not None:
                        self._publish_endpoint("")
                        print(
                            f"assistant_bridge_exited: returncode={returncode}",
                            file=sys.stderr,
                        )
                        return exit_status(returncode)
                    busy = health_busy(self._fetch, self._backend.health_url())
                    if busy is not None:
                        self._publish_health()
                        self._set_busy(busy)
                        if not served:
                            self._publish_endpoint(self.endpoint)
                            served = True
                period = self._health_period_s if served else READY_POLL_S
                self._wake.wait(period if running else None)
                self._wake.clear()
        finally:
            self._listener.close()


def main(args: list[str] | None = None) -> int:
    import rclpy
    from rclpy.context import Context
    from rclpy.executors import ExternalShutdownException, SingleThreadedExecutor
    from rclpy.qos import DurabilityPolicy, HistoryPolicy, QoSProfile, ReliabilityPolicy
    from std_msgs.msg import Bool, Header, String

    latched = QoSProfile(
        history=HistoryPolicy.KEEP_LAST,
        depth=1,
        reliability=ReliabilityPolicy.RELIABLE,
        durability=DurabilityPolicy.TRANSIENT_LOCAL,
    )
    context = Context()
    rclpy.init(args=args, context=context)
    node = rclpy.create_node("assistant_host", context=context)
    executor = SingleThreadedExecutor(context=context)
    host: AssistantHost | None = None
    worker: threading.Thread | None = None
    status: list[int] = []
    try:
        node.declare_parameter("backend", "")
        node.declare_parameter("health_period_s", 1.0)
        endpoint = node.create_publisher(String, "endpoint", latched)
        busy = node.create_publisher(Bool, "busy", latched)
        health = node.create_publisher(Header, "health", 1)

        def publish_health() -> None:
            health.publish(Header(stamp=node.get_clock().now().to_msg()))

        try:
            host = AssistantHost(
                make_backend(str(node.get_parameter("backend").value)),
                lambda url: endpoint.publish(String(data=url)),
                lambda value: busy.publish(Bool(data=value)),
                publish_health,
                float(node.get_parameter("health_period_s").value),
            )
        except ValueError as error:
            print(f"assistant_host_failed: {error}", file=sys.stderr)
            return CONFIG_INVALID_EXIT_STATUS
        # The callback only sets a flag; the worker does the backend work.
        node.create_subscription(
            Bool, "serving", lambda message: host.request_serving(message.data), latched
        )
        def work() -> None:
            try:
                status.append(host.run())
            except BaseException:
                status.append(1)  # a crash of the worker is the host's exit, not zero
                raise

        worker = threading.Thread(target=work, name="manyforge-assistant-host")
        worker.start()
        executor.add_node(node)
        node.get_logger().info(f"assistant host ready on {host.endpoint}")
        while context.ok() and worker.is_alive():
            executor.spin_once(timeout_sec=0.1)
    except (KeyboardInterrupt, ExternalShutdownException):
        pass
    finally:
        if host is not None and worker is not None:
            host.request_stop()
            worker.join()
        executor.shutdown(timeout_sec=2.0)
        node.destroy_node()
        if context.ok():
            rclpy.shutdown(context=context)
    return status[0] if status else 0


if __name__ == "__main__":
    raise SystemExit(main())
