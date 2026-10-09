"""Headless publication of operator-alarm motion attention to the Kernel."""

from __future__ import annotations

import threading
import time
from typing import Any

import pytest

from manyforge_headless_api.alarm_attention import (
    KERNEL_FRESHNESS_SECONDS,
    PUBLICATION_DEADLINE_SECONDS,
    PUBLICATION_PERIOD_SECONDS,
    AttentionPublicationCounts,
    OperatorAlarmAttentionPublisher,
)
from manyforge_headless_api.execution import ExecutionEventPump


class _Core:
    def __init__(self) -> None:
        self.generation = 1
        self.reads = 0

    def operator_alarm_motion_attention(self) -> dict[str, Any]:
        self.reads += 1
        return {"generation": self.generation, "owner_epoch": "alarm.owner.1"}


class _Kernel:
    def __init__(self, wanted: int = 0) -> None:
        self.published: list[dict[str, Any]] = []
        self.received_at: list[float] = []
        self.failure: Exception | None = None
        self.wanted = wanted
        self.enough = threading.Event()
        self.deadlines: list[float] = []

    def publish_operator_alarm_attention(
        self, projection: dict[str, Any], *, deadline: float
    ) -> dict[str, Any]:
        self.deadlines.append(deadline)
        if self.failure is not None:
            raise self.failure
        self.published.append(projection)
        self.received_at.append(time.monotonic())
        if self.wanted and len(self.published) >= self.wanted:
            self.enough.set()
        return {"generation": projection["generation"], "owner_epoch": "alarm.owner.1"}


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


def test_publication_timing_is_bounded_by_the_kernel_freshness_contract() -> None:
    # Production: D < P and P + D = 0.45 s < F = 2 s = 8 P.
    assert PUBLICATION_DEADLINE_SECONDS < PUBLICATION_PERIOD_SECONDS
    assert (
        PUBLICATION_PERIOD_SECONDS + PUBLICATION_DEADLINE_SECONDS
        < KERNEL_FRESHNESS_SECONDS
    )
    with pytest.raises(ValueError):
        OperatorAlarmAttentionPublisher(_Core(), _Kernel(), period_seconds=0.6)
    with pytest.raises(ValueError):
        # A deadline reaching the next period could delay that period.
        OperatorAlarmAttentionPublisher(_Core(), _Kernel(), deadline_seconds=0.25)


def test_each_publication_carries_one_deadline_from_its_start() -> None:
    core, kernel = _Core(), _Kernel()
    publisher = OperatorAlarmAttentionPublisher(core, kernel)
    started = time.monotonic()
    assert publisher.publish_once()
    assert started < kernel.deadlines[0] <= time.monotonic() + PUBLICATION_DEADLINE_SECONDS
    assert kernel.deadlines[0] - started <= PUBLICATION_DEADLINE_SECONDS + 0.05


class _DeadlineKernel(_Kernel):
    """A Kernel port that answers after ``delay`` unless the deadline ends first.

    Like the real transport it never blocks past the caller's deadline.
    """

    def __init__(self, delay: float, wanted: int = 0) -> None:
        super().__init__(wanted)
        self.delay = delay
        self.started_at: list[float] = []
        self.in_flight = threading.Event()

    def publish_operator_alarm_attention(
        self, projection: dict[str, Any], *, deadline: float
    ) -> dict[str, Any]:
        started = time.monotonic()
        self.started_at.append(started)
        self.in_flight.set()
        if started + self.delay > deadline:
            time.sleep(max(0.0, deadline - started))
            if self.wanted and len(self.started_at) >= self.wanted:
                self.enough.set()
            raise TimeoutError("cell-admission command deadline elapsed")
        time.sleep(self.delay)
        return super().publish_operator_alarm_attention(projection, deadline=deadline)


def test_a_publication_past_its_deadline_is_abandoned_counted_and_keeps_the_period() -> None:
    samples = 8
    core = _Core()
    kernel = _DeadlineKernel(delay=10.0, wanted=samples)
    publisher = OperatorAlarmAttentionPublisher(core, kernel)
    publisher.start()
    try:
        assert kernel.enough.wait(timeout=samples * PUBLICATION_PERIOD_SECONDS + 5.0)
    finally:
        publisher.shutdown()
    counts = publisher.counts
    assert counts.published == 0 and counts.failed == 0
    assert counts.abandoned >= samples
    starts = kernel.started_at[:samples]
    intervals = [later - earlier for earlier, later in zip(starts, starts[1:])]
    # Every abandoned publication ended inside its own period.
    assert max(intervals) < PUBLICATION_PERIOD_SECONDS + 0.1
    assert min(intervals) > PUBLICATION_PERIOD_SECONDS - 0.05


def test_failure_before_the_deadline_is_counted_separately() -> None:
    core, kernel = _Core(), _Kernel()
    publisher = OperatorAlarmAttentionPublisher(core, kernel)
    kernel.failure = OSError("execution host unavailable")
    assert not publisher.publish_once()
    kernel.failure = None
    assert publisher.publish_once()
    assert publisher.counts == AttentionPublicationCounts(1, 0, 1)


def test_shutdown_with_an_outstanding_publication_is_bounded_by_its_deadline() -> None:
    kernel = _DeadlineKernel(delay=10.0)
    publisher = OperatorAlarmAttentionPublisher(_Core(), kernel)
    publisher.start()
    assert kernel.in_flight.wait(timeout=5.0)
    started = time.monotonic()
    publisher.shutdown()
    assert time.monotonic() - started <= PUBLICATION_DEADLINE_SECONDS + 0.1


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
    pump = ExecutionEventPump(_PumpCore(), source, poll_seconds=0.1)  # type: ignore[arg-type]
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
        "owner_epoch": "alarm.owner.1",
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
