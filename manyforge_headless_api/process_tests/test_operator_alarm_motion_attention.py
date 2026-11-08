"""Headless operator-alarm attention publication on the real core and Kernel.

Publication timing in the composed process stack. Attention is
published, never a gate: acknowledgement never gates work.
"""

from __future__ import annotations

from contextlib import contextmanager
import json
import math
import os
from pathlib import Path
import shutil
import socket
import sys
from tempfile import TemporaryDirectory
import threading
import time
from typing import Any, Callable, Iterator

import pytest

from manyforge_headless_api.alarm_attention import (
    KERNEL_FRESHNESS_SECONDS,
    PUBLICATION_DEADLINE_SECONDS,
    PUBLICATION_PERIOD_SECONDS,
    OperatorAlarmAttentionPublisher,
)
from manyforge_headless_api.cell_admission import (
    EXECUTION_CONTROL_SOCKET_FILE,
    MAXIMUM_RESPONSE_BYTES,
    CellAdmissionClient,
)
from manyforge_headless_api.execution import ExecutionCommandClient, ExecutionEventPump
from manyforge_headless_api.native import HeadlessApiConfig, NativeHeadlessCore
from manyforge_headless_api.process_tests.test_process_stack import (
    keep_failure_evidence,
    CATALOG,
    NODE_CATALOG,
    NODE_ENVIRONMENT_MANIFEST,
    ProcessStack,
    _read_only_tree,
    _required_path,
)
from manyforge_headless_api.process_tests.test_dual_robot_grouped_execution import (
    ADMISSION_FILENAME,
    CELL_ID,
)


# --- Real-path publication timing qualification -------------

_PUBLISH_COMMAND = b'"command":"cell_admission_publish_operator_alarm_attention"'


class _KernelRelay:
    """Same-user relay in front of the real native host's control socket.

    The attention heartbeat is published beside Stop on the control socket,
    so that is the socket the relay serves and forwards to.

    It forwards every packet unchanged and records, for each publication, when
    the request arrived and when the Kernel's acceptance came back. A
    configurable delay holds the publication before the Kernel sees it, which
    is a Kernel that answers late; abandoned client sockets are tolerated.
    """

    def __init__(self, upstream: Path, run_root: Path) -> None:
        self._upstream = upstream
        run_root.mkdir(mode=0o700)
        self.run_root = run_root
        self._listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        path = run_root / EXECUTION_CONTROL_SOCKET_FILE
        self._listener.bind(str(path))
        os.chmod(path, 0o600)
        self._listener.listen(16)
        self.delay_seconds = 0.0
        self.lock = threading.Lock()
        self.requested_at: list[float] = []
        self.accepted_at: list[float] = []
        self.in_flight = threading.Event()
        self._closed = threading.Event()
        self._forwarders: list[threading.Thread] = []
        self._thread = threading.Thread(target=self._accept, daemon=True)
        self._thread.start()

    def _accept(self) -> None:
        while True:
            try:
                connection, _address = self._listener.accept()
            except OSError:
                return
            forwarder = threading.Thread(
                target=self._forward, args=(connection,), daemon=True
            )
            with self.lock:
                self._forwarders.append(forwarder)
            forwarder.start()

    def _forward(self, connection: socket.socket) -> None:
        with connection:
            request = connection.recv(MAXIMUM_RESPONSE_BYTES)
            publication = _PUBLISH_COMMAND in request
            if publication:
                with self.lock:
                    self.requested_at.append(time.monotonic())
                    delay = self.delay_seconds
                self.in_flight.set()
                # A closed relay ends a held exchange at once; it never
                # forwards to a stack that has already been torn down.
                if self._closed.wait(delay):
                    return
            if self._closed.is_set():
                return
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as upstream:
                upstream.setsockopt(
                    socket.SOL_SOCKET, socket.SO_RCVBUF, MAXIMUM_RESPONSE_BYTES
                )
                upstream.settimeout(10.0)
                upstream.connect(str(self._upstream))
                upstream.send(request)
                response = upstream.recv(MAXIMUM_RESPONSE_BYTES)
            if publication and json.loads(response).get("ok") is True:
                with self.lock:
                    self.accepted_at.append(time.monotonic())
            try:
                connection.send(response)
            except OSError:
                pass  # the publisher abandoned this exchange at its deadline

    def mark(self) -> tuple[int, int]:
        with self.lock:
            return len(self.requested_at), len(self.accepted_at)

    def since(self, mark: tuple[int, int]) -> tuple[list[float], list[float]]:
        with self.lock:
            return self.requested_at[mark[0]:], self.accepted_at[mark[1]:]

    def close(self) -> None:
        self._closed.set()
        self._listener.close()
        self._thread.join(timeout=10.0)
        with self.lock:
            forwarders = list(self._forwarders)
        for forwarder in forwarders:
            forwarder.join(timeout=15.0)
            assert not forwarder.is_alive(), "relay exchange outlived its test"


class _TimedAttentionSource:
    """The real core, with each owner read's wall duration recorded."""

    def __init__(self, core: NativeHeadlessCore) -> None:
        self._core = core
        self.read_seconds: list[float] = []
        self.last: dict[str, Any] | None = None

    def operator_alarm_motion_attention(self) -> dict[str, Any]:
        started = time.monotonic()
        projection = self._core.operator_alarm_motion_attention()
        self.read_seconds.append(time.monotonic() - started)
        self.last = projection
        return projection


def _intervals(times: list[float]) -> list[float]:
    return [later - earlier for earlier, later in zip(times, times[1:])]


def _summary(values: list[float]) -> str:
    ordered = sorted(values)
    p99 = ordered[max(0, math.ceil(0.99 * len(ordered)) - 1)]
    return (
        f"n={len(ordered)} min={ordered[0] * 1000:.1f}ms "
        f"p99={p99 * 1000:.1f}ms max={ordered[-1] * 1000:.1f}ms"
    )


def _wait_for(predicate: Callable[[], bool], timeout_seconds: float, what: str) -> None:
    deadline = time.monotonic() + timeout_seconds
    while not predicate():
        if time.monotonic() >= deadline:
            raise AssertionError(f"timed out waiting for {what}")
        time.sleep(0.01)


@contextmanager
def _native_host_stack() -> Iterator[ProcessStack]:
    """Manager and the real execution host, without a Headless API."""
    with TemporaryDirectory(prefix="mf-oaq-") as temporary_root:
        root = Path(temporary_root)
        bootstrap = root / "cell-bootstrap"
        shutil.copytree(_required_path("MANYFORGE_DUAL_ROBOT_CELL_BOOTSTRAP_ROOT"), bootstrap)
        stack_root = root / "stack"
        stack_root.mkdir()
        with _read_only_tree(bootstrap):
            stack = ProcessStack(
                stack_root,
                package_root=bootstrap,
                cell_id=CELL_ID,
                freshness_milliseconds=120_000,
            )
            try:
                stack.prepare_native_package(ADMISSION_FILENAME, ros2_control=True)
                stack.start_manager()
                admission = CellAdmissionClient(stack.execution_run_root, timeout_seconds=1.0)

                def host_ready() -> bool:
                    try:
                        admission.get()
                    except Exception:
                        return False
                    return True

                _wait_for(host_ready, 60.0, "the execution host")
                yield stack
            except BaseException:
                stack.log.flush()
                keep_failure_evidence(root)
                log_path = stack_root / "process-stack.log"
                if log_path.is_file():
                    print(
                        "\n--- attention qualification process log ---\n"
                        + log_path.read_text(encoding="utf-8", errors="replace"),
                        file=sys.stderr,
                    )
                raise
            finally:
                stack.close()


def test_attention_publication_meets_its_deadline_on_the_real_core_and_kernel(
    capsys: pytest.CaptureFixture[str],
) -> None:
    """Real Headless core + real native host: P + D < F under adversity.

    The publisher reads the real core while other threads hold its
    coordination lock with source refreshes, while the Kernel answers late,
    and is shut down with one exchange outstanding.
    """
    period, deadline = PUBLICATION_PERIOD_SECONDS, PUBLICATION_DEADLINE_SECONDS
    with _native_host_stack() as stack:
        relay = _KernelRelay(
            stack.execution_run_root / EXECUTION_CONTROL_SOCKET_FILE,
            stack.root / "attention-relay-run",
        )
        core = NativeHeadlessCore(
            HeadlessApiConfig(
                projection_root=stack.publication_root,
                install_root=stack.package_root,
                artifact_root=stack.artifact_root,
                node_catalog_path=NODE_CATALOG,
                node_environment_manifest_path=NODE_ENVIRONMENT_MANIFEST,
                alarm_catalog_path=CATALOG,
                historian_database_path=stack.history_database,
                manager_run_root=stack.manager_run_root,
                execution_run_root=stack.execution_run_root,
                cell_id=CELL_ID,
                owner_id="headless-api.attention-qualification",
                freshness_milliseconds=120_000,
            )
        )
        source = _TimedAttentionSource(core)
        # Production composition: the event pump drives the alarm-source
        # refresh; the publisher reaches the Kernel through the relay.
        pump = ExecutionEventPump(
            core,
            ExecutionCommandClient(stack.execution_run_root, timeout_seconds=1.0),
            poll_seconds=0.1,
        )
        publisher = OperatorAlarmAttentionPublisher(
            source,
            CellAdmissionClient(relay.run_root, timeout_seconds=deadline),
        )
        report: list[str] = []
        load_stop = threading.Event()
        load_calls = [0]

        def core_load() -> None:
            # Each call takes the core coordination lock and runs a full
            # source refresh (RefreshLocked) under it.
            calls = (core.runtime_snapshot, core.alarm_snapshot, core.alarm_metrics)
            index = 0
            while not load_stop.is_set():
                calls[index % len(calls)]()
                index += 1
                load_calls[0] += 1

        def phase(name: str, samples: int) -> tuple[list[float], list[float]]:
            mark = relay.mark()
            reads = len(source.read_seconds)
            _wait_for(
                lambda: len(relay.since(mark)[1]) >= samples
                and len(relay.since(mark)[0]) >= samples,
                samples * period + 10.0,
                f"{samples} Kernel receipts in phase {name}",
            )
            requested, accepted = relay.since(mark)
            report.append(
                f"{name}: Kernel receipt intervals {_summary(_intervals(accepted))}; "
                f"publication start intervals {_summary(_intervals(requested))}; "
                f"owner read {_summary(source.read_seconds[reads:])}"
            )
            return requested, accepted

        loaders: list[threading.Thread] = []
        pump.start()
        try:
            publisher.start()
            # Steady state: the pump's refresh keeps the sources current.
            _wait_for(
                lambda: source.last is not None and source.last["reconciled"] is True,
                30.0,
                "a reconciled motion-attention projection",
            )
            _requested, baseline = phase("baseline", 16)
            assert max(_intervals(baseline)) < period + deadline

            loaders = [
                threading.Thread(target=core_load, name=f"core-load-{index}", daemon=True)
                for index in range(4)
            ]
            for loader in loaders:
                loader.start()
            reads = len(source.read_seconds)
            _requested, contended = phase("core-lock contention (4 refresh loops)", 40)
            contended_reads = source.read_seconds[reads:]
            assert load_calls[0] > 100
            assert max(contended_reads) < deadline
            assert max(_intervals(contended)) < period + deadline
            assert source.last is not None and source.last["reconciled"] is True

            relay.delay_seconds = deadline / 2
            _requested, delayed = phase(f"Kernel answers after {deadline / 2:.2f}s", 16)
            assert max(_intervals(delayed)) < period + deadline
            before = publisher.counts
            relay.delay_seconds = deadline + 0.15
            requested, late = phase(f"Kernel answers after {deadline + 0.15:.2f}s (> D)", 12)
            after = publisher.counts
            abandoned = after.abandoned - before.abandoned
            report.append(f"abandoned at D in the late phase: {abandoned}")
            assert abandoned >= 10
            # An abandoned publication never delays the next period's start.
            assert max(_intervals(requested)) < period + 0.1
            load_stop.set()
            for loader in loaders:
                loader.join(timeout=10.0)

            # Shutdown while one exchange is outstanding at the Kernel.
            relay.delay_seconds = 5.0
            relay.in_flight.clear()
            assert relay.in_flight.wait(timeout=5.0)
            started = time.monotonic()
            publisher.shutdown()
            shutdown_seconds = time.monotonic() - started
            report.append(f"shutdown with an outstanding exchange: {shutdown_seconds * 1000:.1f}ms")
            assert shutdown_seconds <= deadline + 0.1
        finally:
            load_stop.set()
            publisher.shutdown()
            pump.shutdown()
            core.shutdown()
            relay.close()
        counts = publisher.counts
        with capsys.disabled():
            print(
                "\nalarm attention real-path qualification "
                f"(P={period}s D={deadline}s F={KERNEL_FRESHNESS_SECONDS}s, "
                f"P+D={period + deadline:.2f}s < F):"
            )
            for line in report:
                print("  " + line)
            print(f"  outcomes: {counts}")
