"""Lock order: the transition lock before the liveness lock, never the reverse.

The reconciliation heartbeat once held the liveness lock while it waited for
the transition lock, and every ordinary command takes them the other way
round. A Manager back after a pause while one command ran and a second
waited deadlocked the Runner for good. This test forces that interleaving.
"""

from __future__ import annotations

from pathlib import Path
from threading import Event, RLock, Thread, current_thread
from typing import Any

from manyforge_headless_api.models import ProgramRunStartRequest
from manyforge_program_runner import liveness as L
from manyforge_program_runner import runtime as R

_JOIN_TIMEOUT_S = 5.0


class _GatedLock:
    """The real RLock; reports waiters and holds back the gated threads."""

    def __init__(self, gated: set[str], gate: Event) -> None:
        self._lock = RLock()
        self._gated = gated
        self._gate = gate
        self.waiting: dict[str, Event] = {}
        self.holding: dict[str, Event] = {}

    def event(self, table: dict[str, Event], name: str) -> Event:
        return table.setdefault(name, Event())

    def __enter__(self) -> "_GatedLock":
        name = current_thread().name
        self.event(self.waiting, name).set()
        if name in self._gated:
            self._gate.wait()
        self._lock.acquire()
        self.event(self.holding, name).set()
        return self

    def __exit__(self, *_exc: object) -> bool:
        self._lock.release()
        return False


class _Journal:
    """X1 stays in its first read (long I/O under the transition lock)."""

    def __init__(self) -> None:
        self.x1_in_io = Event()
        self.x1_release = Event()

    def read(self) -> Any:
        if current_thread().name == "X1":
            self.x1_in_io.set()
            self.x1_release.wait()
            raise R.ProgramRunnerError("test_x1_done", "X1 transition ends")
        raise RuntimeError("journal unavailable in this test")

    def liveness(self) -> Any:
        raise RuntimeError("journal unavailable in this test")


class _Owner:
    def list_snapshots(self, **_kwargs: Any) -> Any:
        raise RuntimeError("owner unavailable in this test")


class _Clock:
    now = 10_000_000_000

    def __call__(self) -> int:
        return self.now


def _heartbeat(sequence: int) -> dict[str, Any]:
    return {
        "active_bundle_id": "bundle",
        "api_version": L.API_VERSION,
        "cell_id": "cell",
        "heartbeat_sequence": sequence,
        "kind": "heartbeat",
        "manager_owner_epoch": "manager-owner",
        "manager_runtime_epoch": "manager-epoch",
        "manager_runtime_unit_id": "manager-unit",
        "receiver_runtime_epoch": "runner-epoch",
        "receiver_runtime_unit_id": "runner-unit",
        "reconciliation_digest": "sha256:" + "a" * 64,
        "required_runtime_set_digest": "sha256:" + "b" * 64,
        "runtime_policy_id": L.POLICY_ID,
        "runtime_policy_revision": L.POLICY_REVISION,
    }


def test_reconciliation_heartbeat_and_waiting_command_cannot_deadlock() -> None:
    gate = Event()
    clock = _Clock()
    runtime = object.__new__(R.ProgramRuntime)
    runtime._closed = False
    runtime._started = True
    runtime._journal = _Journal()
    runtime._admission = object()
    runtime._owner = _Owner()
    runtime.config = type("Config", (), {"maximum_retained_runs": 8})()
    lock = _GatedLock({"H", "W"}, gate)
    runtime._transition_lock = lock
    receiver = L.ProgramRunnerLivenessReceiver(
        Path("/nonexistent/program-runner-liveness.sock"),
        cell_id="cell",
        receiver_runtime_unit_id="runner-unit",
        receiver_runtime_epoch="runner-epoch",
        retained=L.RetainedLiveness("manager-epoch", "manager-owner", False, None),
        on_expiry=runtime._contain_manager_loss,
        on_reconciliation=runtime._reconcile_manager_liveness,
        cycle_status=lambda: {},
        monotonic_ns=clock,
    )
    runtime._liveness = receiver
    receiver._state = "current"
    receiver._fenced = False
    receiver._last_sequence = 5
    receiver._last_accepted_ns = clock.now
    results: dict[str, Any] = {}

    def thread(name: str, body: Any) -> Thread:
        def run() -> None:
            try:
                results[name] = body()
            except BaseException as error:  # noqa: BLE001
                results[name] = error

        return Thread(target=run, name=name, daemon=True)

    request = object.__new__(ProgramRunStartRequest)
    x1 = thread("X1", lambda: runtime.start_run(request))
    x2 = thread("X2", lambda: runtime.start_run(request))
    watchdog = thread("W", receiver.check_expiry)
    heartbeat = thread("H", lambda: receiver._handle(_heartbeat(6)))

    x1.start()
    assert runtime._journal.x1_in_io.wait(_JOIN_TIMEOUT_S)
    x2.start()
    assert lock.event(lock.waiting, "X2").wait(_JOIN_TIMEOUT_S)
    clock.now += L.EXPIRY_TIMEOUT_NS + 1  # the Manager was silent > 1 s
    watchdog.start()
    assert lock.event(lock.waiting, "W").wait(_JOIN_TIMEOUT_S)
    heartbeat.start()
    assert lock.event(lock.waiting, "H").wait(_JOIN_TIMEOUT_S)
    runtime._journal.x1_release.set()
    # The losing order: the waiting command gets the transition lock first.
    assert lock.event(lock.holding, "X2").wait(_JOIN_TIMEOUT_S)
    gate.set()

    for worker in (x1, x2, watchdog, heartbeat):
        worker.join(_JOIN_TIMEOUT_S)
    stuck = [w.name for w in (x1, x2, watchdog, heartbeat) if w.is_alive()]
    assert stuck == [], f"deadlocked threads: {stuck}"
    assert getattr(results["X1"], "code", None) == "test_x1_done"
    assert getattr(results["X2"], "code", None) == (
        "program_runner_manager_liveness_unavailable"
    )
    assert results["W"] is True
    # The test journal cannot reconcile, so the heartbeat is refused and
    # the Runner stays fenced until the Manager's next renewal.
    assert isinstance(results["H"], RuntimeError)
    assert receiver.status()["state"] == "expired"
