"""Headless publication of operator-alarm motion attention to the Kernel."""

from __future__ import annotations

import os
from pathlib import Path
import socket
import threading
import time
from typing import Any

import pytest

from manyforge_headless_api import cell_admission
from manyforge_headless_api.alarm_attention import (
    KERNEL_FRESHNESS_SECONDS,
    PUBLICATION_PERIOD_SECONDS,
    AttentionPublicationCounts,
    OperatorAlarmAttentionPublisher,
)
from manyforge_headless_api.cell_admission import CellAdmissionClient
from manyforge_headless_api.execution import ExecutionEventPump
from manyforge_headless_api.tests.paged_event_stream import paged_subscription


class _Core:
    def __init__(self) -> None:
        self.generation = 1
        self.reads = 0

    def operator_alarm_motion_attention(self) -> dict[str, Any]:
        self.reads += 1
        return {"generation": self.generation}


class _Kernel:
    def __init__(self, wanted: int = 0) -> None:
        self.published: list[dict[str, Any]] = []
        self.received_at: list[float] = []
        self.failure: Exception | None = None
        self.wanted = wanted
        self.enough = threading.Event()

    def publish_operator_alarm_attention(
        self, projection: dict[str, Any], *, stop_fd: int | None
    ) -> dict[str, Any]:
        # The publisher's stop pipe once started; none for a direct call.
        assert stop_fd is None or isinstance(stop_fd, int)
        if self.failure is not None:
            raise self.failure
        self.published.append(projection)
        self.received_at.append(time.monotonic())
        if self.wanted and len(self.published) >= self.wanted:
            self.enough.set()
        return {"generation": projection["generation"]}


def test_every_publication_carries_the_current_projection() -> None:
    core, kernel = _Core(), _Kernel()
    publisher = OperatorAlarmAttentionPublisher(core, kernel)
    assert publisher.publish_once()
    core.generation = 2  # e.g. an acknowledgement
    assert publisher.publish_once()
    # Unchanged content is republished: it is the Kernel's heartbeat.
    assert publisher.publish_once()
    assert [row["generation"] for row in kernel.published] == [1, 2, 2]


def test_failed_publication_is_retried_on_the_next_period() -> None:
    core, kernel = _Core(), _Kernel()
    publisher = OperatorAlarmAttentionPublisher(core, kernel)
    kernel.failure = OSError("execution host unavailable")
    assert not publisher.publish_once()
    kernel.failure = None
    assert publisher.publish_once()
    assert [row["generation"] for row in kernel.published] == [1]


def test_the_period_fits_the_kernel_freshness_bound() -> None:
    # Production: F = 2 s = 8 P.
    assert 4 * PUBLICATION_PERIOD_SECONDS <= KERNEL_FRESHNESS_SECONDS
    with pytest.raises(ValueError):
        OperatorAlarmAttentionPublisher(_Core(), _Kernel(), period_seconds=0.6)


class _HeldKernel(_Kernel):
    """A Kernel port whose first answer waits until the test releases it."""

    def __init__(self) -> None:
        super().__init__()
        self.started_at: list[float] = []
        self.in_flight = threading.Event()
        self.release = threading.Event()
        self.second = threading.Event()

    def publish_operator_alarm_attention(
        self, projection: dict[str, Any], *, stop_fd: int | None
    ) -> dict[str, Any]:
        self.started_at.append(time.monotonic())
        if len(self.started_at) == 1:
            self.in_flight.set()
            self.release.wait()
        else:
            self.second.set()
        return super().publish_operator_alarm_attention(projection, stop_fd=stop_fd)


def test_a_late_answer_is_waited_for_and_the_next_publication_follows_at_once() -> None:
    """OD-307: no publication deadline; the Kernel's answer ends the wait."""
    kernel = _HeldKernel()
    publisher = OperatorAlarmAttentionPublisher(_Core(), kernel)
    publisher.start()
    try:
        assert kernel.in_flight.wait(timeout=5.0)
        # Held well past one period (and past the former 200 ms deadline):
        # nothing abandons it and no second publication starts meanwhile.
        time.sleep(2 * PUBLICATION_PERIOD_SECONDS)
        assert len(kernel.started_at) == 1
        assert publisher.counts == AttentionPublicationCounts(0, 0)
        answered = time.monotonic()
        kernel.release.set()
        assert kernel.second.wait(timeout=5.0)
    finally:
        publisher.shutdown()
    assert publisher.counts.published >= 1 and publisher.counts.failed == 0
    # The overrun period publishes again at once rather than bursting.
    assert kernel.started_at[1] - answered < PUBLICATION_PERIOD_SECONDS


def test_failures_are_counted() -> None:
    core, kernel = _Core(), _Kernel()
    publisher = OperatorAlarmAttentionPublisher(core, kernel)
    kernel.failure = OSError("execution host unavailable")
    assert not publisher.publish_once()
    kernel.failure = None
    assert publisher.publish_once()
    assert publisher.counts == AttentionPublicationCounts(1, 1)


class _SilentKernelSocket:
    """A real component socket whose peer accepts and reads but never answers."""

    def __init__(self, tmp_path: Path) -> None:
        self.run_root = tmp_path / "run"
        self.run_root.mkdir(mode=0o700)
        path = self.run_root / cell_admission.EXECUTION_COMPONENT_SOCKET_FILE
        self._listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self._listener.bind(str(path))
        os.chmod(path, 0o600)
        self._listener.listen(1)
        self.received = threading.Event()
        self._done = threading.Event()
        self._peer = threading.Thread(target=self._serve, name="test-silent-kernel")
        self._peer.start()

    def _serve(self) -> None:
        connection, _address = self._listener.accept()
        with connection:
            connection.recv(cell_admission.MAXIMUM_PACKET_BYTES)
            self.received.set()
            self._done.wait()

    def close(self) -> None:
        self._done.set()
        self._peer.join()
        self._listener.close()


def test_shutdown_ends_an_outstanding_publication_on_the_real_transport(
    tmp_path: Path,
) -> None:
    """OD-307: the publisher's stop pipe ends the wait, not a deadline."""
    kernel = _SilentKernelSocket(tmp_path)
    try:
        publisher = OperatorAlarmAttentionPublisher(
            _Core(), CellAdmissionClient(kernel.run_root, timeout_seconds=1.0)
        )
        publisher.start()
        assert publisher._stop_read_fd is not None  # noqa: SLF001
        assert kernel.received.wait(timeout=5.0)
        publisher.shutdown()
        # The stop pipe is closed once the publisher thread has ended.
        assert publisher._stop_read_fd is None  # noqa: SLF001
        assert publisher._stop_write_fd is None  # noqa: SLF001
        # Ended by the stop: neither published nor counted as failed.
        assert publisher.counts == AttentionPublicationCounts(0, 0)
        assert publisher.kernel_held_attention() is None
    finally:
        kernel.close()


class _PumpCore:
    def refresh_alarm_sources(self) -> dict[str, Any]:
        return {"refreshed": False}

    def ingest_operation_event(self, **_kwargs: Any) -> dict[str, Any]:
        return {"status": "ingested"}

    def complete_operation_event_snapshot(self, **_kwargs: Any) -> dict[str, Any]:
        return {"status": "complete"}

    def mark_operation_event_source_unknown(self, **_kwargs: Any) -> dict[str, Any]:
        return {"status": "marked_unknown"}


class _BlockedSourceRefresh:
    """An execution source whose first refresh blocks until released."""

    def __init__(self) -> None:
        self.entered = threading.Event()
        self.release = threading.Event()

    def subscribe_events(self, after_cursor: int):
        return paged_subscription(self, after_cursor)

    def identity(self) -> dict[str, Any]:
        self.entered.set()
        self.release.wait()
        return {"ok": False, "error": {"code": "execution_host_unavailable"}}


def test_publications_keep_their_period_while_the_event_pump_is_blocked(
    capsys: pytest.CaptureFixture[str],
) -> None:
    """The complete path: owner read -> publisher thread -> Kernel port.

    The event pump is stuck in a slow source refresh and another thread
    keeps the interpreter busy; the publisher still delivers at its period.
    """
    samples = 12
    core, kernel = _Core(), _Kernel(wanted=samples)
    source = _BlockedSourceRefresh()
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        _PumpCore(), source, application_start_id="3f9a0000000000a1", poll_seconds=0.1
    )
    publisher = OperatorAlarmAttentionPublisher(core, kernel)
    loaded = threading.Event()

    def interpreter_load() -> None:
        # Pure-Python work contending for the GIL, as request handling does.
        while not loaded.is_set():
            sum(range(10_000))

    load = threading.Thread(target=interpreter_load, name="test-load")
    pump.start()
    try:
        assert source.entered.wait(timeout=5.0)
        load.start()
        publisher.start()
        try:
            assert kernel.enough.wait(
                timeout=samples * PUBLICATION_PERIOD_SECONDS + 5.0
            )
        finally:
            publisher.shutdown()
            loaded.set()
            load.join(timeout=5.0)
    finally:
        source.release.set()
        pump.shutdown()
    assert source.entered.is_set() and not load.is_alive()
    received = kernel.received_at[:samples]
    intervals = [later - earlier for earlier, later in zip(received, received[1:])]
    with capsys.disabled():
        print(
            "\nalarm attention publication intervals under load (s): "
            f"min={min(intervals):.4f} max={max(intervals):.4f} "
            f"mean={sum(intervals) / len(intervals):.4f} "
            f"P={PUBLICATION_PERIOD_SECONDS} F={KERNEL_FRESHNESS_SECONDS}"
        )
    # Each receipt follows the previous one by about P, far inside F.
    assert max(intervals) < PUBLICATION_PERIOD_SECONDS + 0.25
    assert max(intervals) < KERNEL_FRESHNESS_SECONDS / 4


class _ReconcilingCore(_Core):
    def __init__(self) -> None:
        super().__init__()
        self.reconciled = False
        self.configuration: dict[str, str] | None = {
            "graph_revision": "graph.1",
            "retained_bundle_id": "bundle.1",
        }

    def operator_alarm_motion_attention(self) -> dict[str, Any]:
        return {
            **super().operator_alarm_motion_attention(),
            "active_configuration": self.configuration,
            "reconciled": self.reconciled,
        }


def test_the_kernel_held_copy_is_the_last_acknowledged_projection(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    # Clients wait for the Kernel's admission state instead of retrying
    # refusals: the held copy is what the Kernel acknowledged, never the
    # owner's newer projection that has not reached it yet.
    core, kernel = _ReconcilingCore(), _Kernel()
    publisher = OperatorAlarmAttentionPublisher(core, kernel)
    changes: list[int] = []
    publisher.set_kernel_held_observer(lambda: changes.append(1))
    assert publisher.kernel_held_attention() is None

    assert publisher.publish_once()
    assert publisher.kernel_held_attention() == {
        "active_configuration": {"graph_revision": "graph.1", "retained_bundle_id": "bundle.1"},
        "generation": 1,
        "reconciled": False,
    }
    assert len(changes) == 1

    # The owner reconciles; until the Kernel acknowledges it the held copy
    # still reads unreconciled.
    core.generation, core.reconciled = 2, True
    kernel.failure = OSError("execution host unavailable")
    assert not publisher.publish_once()
    held = publisher.kernel_held_attention()
    assert held is not None and held["reconciled"] is False
    kernel.failure = None
    assert publisher.publish_once()
    held = publisher.kernel_held_attention()
    assert held is not None and held["reconciled"] is True and held["generation"] == 2
    assert len(changes) == 2
    # An unchanged heartbeat is no change.
    assert publisher.publish_once()
    assert len(changes) == 2

    # An Apply: the owner names the new configuration once it observed it.
    core.generation = 3
    core.configuration = {"graph_revision": "graph.2", "retained_bundle_id": "bundle.2"}
    assert publisher.publish_once()
    held = publisher.kernel_held_attention()
    assert held is not None and held["active_configuration"] == core.configuration
    assert len(changes) == 3

    # Without an acknowledgement within the Kernel freshness bound the
    # Kernel's copy is stale: none is reported.
    now = time.monotonic()
    monkeypatch.setattr(
        "manyforge_headless_api.alarm_attention.time.monotonic",
        lambda: now + KERNEL_FRESHNESS_SECONDS + 0.1,
    )
    assert publisher.kernel_held_attention() is None
    kernel.failure = OSError("execution host unavailable")
    assert not publisher.publish_once()
    assert len(changes) == 4
