from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from threading import Barrier, Event, Lock

import pytest

from verification.composable_cell_i1a.contracts import (
    SCHEMA_ID,
    ApplyRequest,
    ApplyTransactionId,
    CellGraph,
    GraphRevision,
    RequestId,
    RequestMetadata,
    RuntimeUnitId,
    RuntimeUnitSpec,
)
from verification.composable_cell_i1a.fakes import FakeClock, FakeProcessAdapter
from verification.composable_cell_i1a.manager_prototype import (
    DeterministicManagerCrash,
    DurableComponentManager,
    crash_once,
    effect_checkpoints,
)


MANAGER_ID = RuntimeUnitId("component-manager")


class RecordingProcessAdapter:
    def __init__(self, delegate: FakeProcessAdapter) -> None:
        self.delegate = delegate
        self.effects: list[tuple[str, str]] = []

    def start(self, spec: RuntimeUnitSpec):
        self.effects.append(("start", str(spec.runtime_unit_id)))
        return self.delegate.start(spec)

    def stop(self, runtime_unit_id: RuntimeUnitId):
        self.effects.append(("stop", str(runtime_unit_id)))
        return self.delegate.stop(runtime_unit_id)

    def observe(self, runtime_unit_id: RuntimeUnitId):
        return self.delegate.observe(runtime_unit_id)

    def observe_all(self):
        return self.delegate.observe_all()

    def observed_spec_fingerprint(self, runtime_unit_id: RuntimeUnitId):
        return self.delegate.observed_spec_fingerprint(runtime_unit_id)


class AmbiguousAfterStartAdapter(RecordingProcessAdapter):
    def __init__(self, delegate: FakeProcessAdapter) -> None:
        super().__init__(delegate)
        self.ambiguous = False
        self.inject_after_start = True

    def start(self, spec: RuntimeUnitSpec):
        observation = super().start(spec)
        if self.inject_after_start:
            self.ambiguous = True
            raise OSError("connection_lost_after_start")
        return observation

    def observe(self, runtime_unit_id: RuntimeUnitId):
        if self.ambiguous:
            raise OSError("process_observation_unavailable")
        return super().observe(runtime_unit_id)


class BlockingStartAdapter(RecordingProcessAdapter):
    def __init__(self, delegate: FakeProcessAdapter) -> None:
        super().__init__(delegate)
        self.entered = Event()
        self.release = Event()
        self._calls_lock = Lock()
        self.start_calls = 0

    def start(self, spec: RuntimeUnitSpec):
        with self._calls_lock:
            self.start_calls += 1
        self.entered.set()
        if not self.release.wait(timeout=5):
            raise TimeoutError("test did not release blocking start")
        return super().start(spec)


class DriftAfterEffectVerificationAdapter(RecordingProcessAdapter):
    """Crash a unit after an effect's final exact-spec observation."""

    def __init__(
        self,
        delegate: FakeProcessAdapter,
        runtime_unit_id: RuntimeUnitId,
    ) -> None:
        super().__init__(delegate)
        self.runtime_unit_id = runtime_unit_id
        self.inject = False
        self._remaining_fingerprint_reads = 0

    def observed_spec_fingerprint(self, runtime_unit_id: RuntimeUnitId):
        fingerprint = super().observed_spec_fingerprint(runtime_unit_id)
        if self.inject and runtime_unit_id == self.runtime_unit_id:
            if self._remaining_fingerprint_reads == 0:
                self._remaining_fingerprint_reads = 2
            self._remaining_fingerprint_reads -= 1
            if self._remaining_fingerprint_reads == 0:
                self.inject = False
                self.delegate.crash(runtime_unit_id)
        return fingerprint


def unit(
    unit_id: str,
    *,
    dependencies: tuple[str, ...] = (),
    command: tuple[str, ...] = (),
) -> RuntimeUnitSpec:
    return RuntimeUnitSpec(
        runtime_unit_id=RuntimeUnitId(unit_id),
        kind="process",
        dependencies=tuple(RuntimeUnitId(item) for item in dependencies),
        command=command,
    )


def apply_request(
    apply_id: str,
    *,
    expected_revision: int,
    target_revision: int,
    units: tuple[RuntimeUnitSpec, ...],
    request_id: str | None = None,
) -> ApplyRequest:
    return ApplyRequest(
        metadata=RequestMetadata(
            request_id=RequestId(request_id or f"request-{apply_id}"),
            source_label="test",
            transport="in_process",
        ),
        apply_id=ApplyTransactionId(apply_id),
        expected_graph_revision=GraphRevision(expected_revision),
        desired_graph=CellGraph(
            schema=SCHEMA_ID,
            graph_revision=GraphRevision(target_revision),
            runtime_units=units,
        ),
    )


def manager(
    database_path: Path,
    adapter: RecordingProcessAdapter | FakeProcessAdapter,
    clock: FakeClock,
) -> DurableComponentManager:
    return DurableComponentManager(
        database_path=database_path,
        manager_runtime_unit_id=MANAGER_ID,
        process_adapter=adapter,
        clock=clock,
    )


@pytest.mark.parametrize(
    "checkpoint",
    effect_checkpoints(0, "start", RuntimeUnitId("execution-host")),
)
def test_restart_reconciles_every_start_effect_crash_without_repeating_effect(
    tmp_path: Path, checkpoint: str
) -> None:
    clock = FakeClock()
    adapter = RecordingProcessAdapter(FakeProcessAdapter(clock))
    database = tmp_path / "journal.sqlite3"
    request = apply_request(
        "apply-start",
        expected_revision=0,
        target_revision=1,
        units=(unit("execution-host"),),
    )

    with pytest.raises(DeterministicManagerCrash, match=checkpoint):
        manager(database, adapter, clock).apply(
            request, crash_injector=crash_once(checkpoint)
        )

    restarted = manager(database, adapter, clock)
    result = restarted.recover(request.apply_id)

    assert result.phase == "committed"
    assert adapter.effects.count(("start", "execution-host")) == 1
    assert restarted.journal(request.apply_id)[0].state == "effected"


@pytest.mark.parametrize(
    "checkpoint",
    effect_checkpoints(0, "stop", RuntimeUnitId("execution-host")),
)
def test_restart_reconciles_every_stop_effect_crash_without_repeating_effect(
    tmp_path: Path, checkpoint: str
) -> None:
    clock = FakeClock()
    adapter = RecordingProcessAdapter(FakeProcessAdapter(clock))
    database = tmp_path / "journal.sqlite3"
    first = apply_request(
        "apply-start",
        expected_revision=0,
        target_revision=1,
        units=(unit("execution-host"),),
    )
    running_manager = manager(database, adapter, clock)
    assert running_manager.apply(first).phase == "committed"
    adapter.effects.clear()
    stop_request = apply_request(
        "apply-stop",
        expected_revision=1,
        target_revision=2,
        units=(),
    )

    with pytest.raises(DeterministicManagerCrash, match=checkpoint):
        running_manager.apply(
            stop_request, crash_injector=crash_once(checkpoint)
        )

    restarted = manager(database, adapter, clock)
    result = restarted.recover(stop_request.apply_id)

    assert result.phase == "committed"
    assert adapter.effects.count(("stop", "execution-host")) == 1


@pytest.mark.parametrize(
    "checkpoint",
    (
        "apply:after_transaction_intent",
        "apply:forward:before_commit",
        "apply:forward:after_commit",
    ),
)
def test_apply_transaction_boundaries_are_restart_safe(
    tmp_path: Path, checkpoint: str
) -> None:
    clock = FakeClock()
    adapter = RecordingProcessAdapter(FakeProcessAdapter(clock))
    database = tmp_path / "journal.sqlite3"
    request = apply_request(
        "apply-boundary",
        expected_revision=0,
        target_revision=1,
        units=(unit("worker"),),
    )

    with pytest.raises(DeterministicManagerCrash, match=checkpoint):
        manager(database, adapter, clock).apply(
            request, crash_injector=crash_once(checkpoint)
        )

    restarted = manager(database, adapter, clock)
    assert restarted.apply(request).phase == "committed"
    assert adapter.effects.count(("start", "worker")) == 1


def test_apply_is_cas_idempotent_and_rejects_conflicting_id(
    tmp_path: Path,
) -> None:
    clock = FakeClock()
    adapter = RecordingProcessAdapter(FakeProcessAdapter(clock))
    candidate = manager(tmp_path / "journal.sqlite3", adapter, clock)
    request = apply_request(
        "apply-1",
        expected_revision=0,
        target_revision=1,
        units=(unit("worker", command=("worker", "--one")),),
    )

    first = candidate.apply(request)
    replay = candidate.apply(request)
    conflicting = candidate.apply(
        apply_request(
            "apply-1",
            request_id="different-request",
            expected_revision=0,
            target_revision=1,
            units=(unit("worker", command=("worker", "--two")),),
        )
    )

    assert first == replay
    assert adapter.effects == [("start", "worker")]
    assert conflicting.phase == "rejected"
    assert conflicting.detail == "conflicting_apply_id"
    assert adapter.effects == [("start", "worker")]


def test_concurrent_apply_has_exactly_one_cas_winner(tmp_path: Path) -> None:
    database = tmp_path / "journal.sqlite3"
    clock = FakeClock()
    adapter = FakeProcessAdapter(clock)
    manager(database, adapter, clock)
    barrier = Barrier(2)

    def submit(apply_id: str):
        candidate = manager(database, adapter, clock)
        barrier.wait()
        return candidate.apply(
            apply_request(
                apply_id,
                expected_revision=0,
                target_revision=1,
                units=(),
            )
        )

    with ThreadPoolExecutor(max_workers=2) as executor:
        results = tuple(
            executor.map(submit, ("concurrent-a", "concurrent-b"))
        )

    assert sorted(result.phase for result in results) == [
        "committed",
        "rejected",
    ]
    rejected = next(result for result in results if result.phase == "rejected")
    # The loser linearizes either while the winner is reserved or just after it
    # commits. Both states reject it without effects; only the truthful detail
    # differs at that boundary.
    assert rejected.detail in {"apply_in_progress", "stale_graph_revision"}


def test_apply_rejects_chaining_until_reserved_transaction_is_resolved(
    tmp_path: Path,
) -> None:
    database = tmp_path / "journal.sqlite3"
    clock = FakeClock()
    adapter = RecordingProcessAdapter(FakeProcessAdapter(clock))
    candidate = manager(database, adapter, clock)
    first = apply_request(
        "apply-0-to-1",
        expected_revision=0,
        target_revision=1,
        units=(unit("worker"),),
    )

    with pytest.raises(
        DeterministicManagerCrash, match="apply:after_transaction_intent"
    ):
        candidate.apply(
            first,
            crash_injector=crash_once("apply:after_transaction_intent"),
        )

    premature = candidate.apply(
        apply_request(
            "apply-1-to-2-premature",
            expected_revision=1,
            target_revision=2,
            units=(unit("worker"), unit("api", dependencies=("worker",))),
        )
    )
    assert premature.phase == "rejected"
    assert premature.detail == "apply_in_progress"
    assert candidate.recover(first.apply_id).phase == "committed"

    accepted = candidate.apply(
        apply_request(
            "apply-1-to-2",
            expected_revision=1,
            target_revision=2,
            units=(unit("worker"), unit("api", dependencies=("worker",))),
        )
    )
    assert accepted.phase == "committed"
    assert candidate.observed_state().graph_revision == GraphRevision(2)


def test_concurrent_same_id_recovery_serializes_the_external_effect(
    tmp_path: Path,
) -> None:
    database = tmp_path / "journal.sqlite3"
    clock = FakeClock()
    adapter = BlockingStartAdapter(FakeProcessAdapter(clock))
    candidate = manager(database, adapter, clock)
    request = apply_request(
        "concurrent-recovery",
        expected_revision=0,
        target_revision=1,
        units=(unit("worker"),),
    )
    with pytest.raises(
        DeterministicManagerCrash, match="apply:after_transaction_intent"
    ):
        candidate.apply(
            request,
            crash_injector=crash_once("apply:after_transaction_intent"),
        )

    barrier = Barrier(2)

    def recover():
        barrier.wait()
        return manager(database, adapter, clock).recover(request.apply_id)

    with ThreadPoolExecutor(max_workers=2) as executor:
        futures = [executor.submit(recover) for _ in range(2)]
        assert adapter.entered.wait(timeout=5)
        adapter.release.set()
        results = tuple(future.result(timeout=5) for future in futures)

    assert [result.phase for result in results] == ["committed", "committed"]
    assert adapter.start_calls == 1
    assert adapter.effects == [("start", "worker")]


def test_dependency_effect_order_and_manager_self_management_rejection(
    tmp_path: Path,
) -> None:
    clock = FakeClock()
    adapter = RecordingProcessAdapter(FakeProcessAdapter(clock))
    candidate = manager(tmp_path / "journal.sqlite3", adapter, clock)
    start = apply_request(
        "start-graph",
        expected_revision=0,
        target_revision=1,
        units=(
            unit("api", dependencies=("execution-host",)),
            unit("execution-host"),
        ),
    )

    assert candidate.apply(start).phase == "committed"
    assert adapter.effects == [
        ("start", "execution-host"),
        ("start", "api"),
    ]
    adapter.effects.clear()
    stop = apply_request(
        "stop-graph",
        expected_revision=1,
        target_revision=2,
        units=(),
    )
    assert candidate.apply(stop).phase == "committed"
    assert adapter.effects == [
        ("stop", "api"),
        ("stop", "execution-host"),
    ]
    adapter.effects.clear()

    rejected = candidate.apply(
        apply_request(
            "self-management",
            expected_revision=2,
            target_revision=3,
            units=(unit("component-manager"),),
        )
    )
    assert rejected.phase == "rejected"
    assert rejected.detail == "manager_self_management_forbidden"
    assert adapter.effects == []


def test_unsupported_busy_disposition_is_rejected_without_effect(
    tmp_path: Path,
) -> None:
    clock = FakeClock()
    adapter = RecordingProcessAdapter(FakeProcessAdapter(clock))
    candidate = manager(tmp_path / "journal.sqlite3", adapter, clock)
    base = apply_request(
        "unsupported-busy-disposition",
        expected_revision=0,
        target_revision=1,
        units=(unit("worker"),),
    )
    request = ApplyRequest(
        metadata=base.metadata,
        apply_id=base.apply_id,
        expected_graph_revision=base.expected_graph_revision,
        desired_graph=base.desired_graph,
        busy_disposition="stop_affected_work",
    )

    rejected = candidate.apply(request)

    assert rejected.phase == "rejected"
    assert rejected.detail == "unsupported_busy_disposition"
    assert adapter.effects == []


def test_unknown_effect_outcome_is_fail_closed_then_explicitly_rolled_back(
    tmp_path: Path,
) -> None:
    clock = FakeClock()
    delegate = FakeProcessAdapter(clock)
    adapter = AmbiguousAfterStartAdapter(delegate)
    candidate = manager(tmp_path / "journal.sqlite3", adapter, clock)
    request = apply_request(
        "ambiguous",
        expected_revision=0,
        target_revision=1,
        units=(unit("worker"),),
    )

    incomplete = candidate.apply(request)
    assert incomplete.phase == "incomplete"
    assert "ambiguous" in incomplete.detail

    adapter.ambiguous = False
    adapter.inject_after_start = False
    rolled_back = candidate.rollback(request.apply_id)
    assert rolled_back.phase == "rolled_back"
    assert candidate.observed_state().graph_revision == GraphRevision(0)
    assert [entry.direction for entry in candidate.journal(request.apply_id)] == [
        "forward",
        "rollback",
    ]
    assert adapter.effects == [("start", "worker"), ("stop", "worker")]


@pytest.mark.parametrize(
    "checkpoint",
    (
        *effect_checkpoints(1, "stop", RuntimeUnitId("worker")),
        "apply:rollback:before_commit",
        "apply:rollback:after_commit",
    ),
)
def test_rollback_direction_survives_every_crash_boundary(
    tmp_path: Path, checkpoint: str
) -> None:
    clock = FakeClock()
    delegate = FakeProcessAdapter(clock)
    adapter = AmbiguousAfterStartAdapter(delegate)
    database = tmp_path / "journal.sqlite3"
    candidate = manager(database, adapter, clock)
    request = apply_request(
        "rollback-crash",
        expected_revision=0,
        target_revision=1,
        units=(unit("worker"),),
    )
    assert candidate.apply(request).phase == "incomplete"
    adapter.ambiguous = False
    adapter.inject_after_start = False

    with pytest.raises(DeterministicManagerCrash, match=checkpoint):
        candidate.rollback(
            request.apply_id, crash_injector=crash_once(checkpoint)
        )

    restarted = manager(database, adapter, clock)
    recovered = restarted.recover(request.apply_id)

    assert recovered.phase == "rolled_back"
    assert restarted.observed_state().graph_revision == GraphRevision(0)
    assert adapter.effects.count(("start", "worker")) == 1
    assert adapter.effects.count(("stop", "worker")) == 1


def test_recovery_replaces_healthy_same_id_process_with_wrong_spec(
    tmp_path: Path,
) -> None:
    clock = FakeClock()
    delegate = FakeProcessAdapter(clock)
    adapter = RecordingProcessAdapter(delegate)
    database = tmp_path / "journal.sqlite3"
    candidate = manager(database, adapter, clock)
    desired = unit("worker", command=("worker", "--desired"))
    request = apply_request(
        "spec-mismatch",
        expected_revision=0,
        target_revision=1,
        units=(desired,),
    )
    with pytest.raises(
        DeterministicManagerCrash, match="apply:after_transaction_intent"
    ):
        candidate.apply(
            request,
            crash_injector=crash_once("apply:after_transaction_intent"),
        )

    delegate.start(unit("worker", command=("worker", "--external")))
    recovered = candidate.recover(request.apply_id)

    assert recovered.phase == "committed"
    assert adapter.effects == [
        ("stop", "worker"),
        ("start", "worker"),
    ]
    assert candidate.observed_state().graph_revision == GraphRevision(1)


def test_retained_healthy_exact_unit_is_verified_without_restart(
    tmp_path: Path,
) -> None:
    clock = FakeClock()
    adapter = RecordingProcessAdapter(FakeProcessAdapter(clock))
    candidate = manager(tmp_path / "journal.sqlite3", adapter, clock)
    retained = unit("worker", command=("worker", "--stable"))
    assert candidate.apply(
        apply_request(
            "initial",
            expected_revision=0,
            target_revision=1,
            units=(retained,),
        )
    ).phase == "committed"
    adapter.effects.clear()

    result = candidate.apply(
        apply_request(
            "retained",
            expected_revision=1,
            target_revision=2,
            units=(retained,),
        )
    )

    assert result.phase == "committed"
    assert adapter.effects == []


def test_retained_absent_unit_is_restarted_before_revision_commits(
    tmp_path: Path,
) -> None:
    clock = FakeClock()
    delegate = FakeProcessAdapter(clock)
    adapter = RecordingProcessAdapter(delegate)
    candidate = manager(tmp_path / "journal.sqlite3", adapter, clock)
    retained = unit("worker")
    assert candidate.apply(
        apply_request(
            "initial",
            expected_revision=0,
            target_revision=1,
            units=(retained,),
        )
    ).phase == "committed"
    first_epoch = delegate.observe(RuntimeUnitId("worker")).runtime_epoch
    delegate.stop(RuntimeUnitId("worker"))
    adapter.effects.clear()

    result = candidate.apply(
        apply_request(
            "repair-absent",
            expected_revision=1,
            target_revision=2,
            units=(retained,),
        )
    )

    assert result.phase == "committed"
    assert adapter.effects == [("start", "worker")]
    assert delegate.observe(RuntimeUnitId("worker")).runtime_epoch != first_epoch


@pytest.mark.parametrize("health", ("crashed", "stalled", "unknown", "unhealthy"))
def test_retained_non_operational_unit_is_restarted_before_revision_commits(
    tmp_path: Path,
    health: str,
) -> None:
    clock = FakeClock()
    delegate = FakeProcessAdapter(clock)
    adapter = RecordingProcessAdapter(delegate)
    candidate = manager(tmp_path / "journal.sqlite3", adapter, clock)
    retained = unit("worker")
    assert candidate.apply(
        apply_request(
            f"initial-{health}",
            expected_revision=0,
            target_revision=1,
            units=(retained,),
        )
    ).phase == "committed"
    delegate.set_health(RuntimeUnitId("worker"), health)
    adapter.effects.clear()

    result = candidate.apply(
        apply_request(
            f"repair-{health}",
            expected_revision=1,
            target_revision=2,
            units=(retained,),
        )
    )

    assert result.phase == "committed"
    assert adapter.effects == [("stop", "worker"), ("start", "worker")]
    assert delegate.observe(RuntimeUnitId("worker")).health == "ready"


def test_retained_wrong_fingerprint_is_replaced_before_revision_commits(
    tmp_path: Path,
) -> None:
    clock = FakeClock()
    delegate = FakeProcessAdapter(clock)
    adapter = RecordingProcessAdapter(delegate)
    candidate = manager(tmp_path / "journal.sqlite3", adapter, clock)
    retained = unit("worker", command=("worker", "--desired"))
    assert candidate.apply(
        apply_request(
            "initial",
            expected_revision=0,
            target_revision=1,
            units=(retained,),
        )
    ).phase == "committed"
    desired_fingerprint = delegate.observed_spec_fingerprint(
        RuntimeUnitId("worker")
    )
    delegate.drift_spec(
        RuntimeUnitId("worker"),
        unit("worker", command=("worker", "--external")),
    )
    adapter.effects.clear()

    result = candidate.apply(
        apply_request(
            "repair-fingerprint",
            expected_revision=1,
            target_revision=2,
            units=(retained,),
        )
    )

    assert result.phase == "committed"
    assert adapter.effects == [("stop", "worker"), ("start", "worker")]
    assert (
        delegate.observed_spec_fingerprint(RuntimeUnitId("worker"))
        == desired_fingerprint
    )


def test_apply_stops_unexpected_observed_process_without_restarting_exact_unit(
    tmp_path: Path,
) -> None:
    clock = FakeClock()
    delegate = FakeProcessAdapter(clock)
    adapter = RecordingProcessAdapter(delegate)
    candidate = manager(tmp_path / "journal.sqlite3", adapter, clock)
    retained = unit("worker")
    assert candidate.apply(
        apply_request(
            "initial",
            expected_revision=0,
            target_revision=1,
            units=(retained,),
        )
    ).phase == "committed"
    delegate.start(unit("external-process"))
    adapter.effects.clear()

    result = candidate.apply(
        apply_request(
            "remove-unexpected",
            expected_revision=1,
            target_revision=2,
            units=(retained,),
        )
    )

    assert result.phase == "committed"
    assert adapter.effects == [("stop", "external-process")]
    assert delegate.observe(RuntimeUnitId("external-process")) is None


def test_final_verification_prevents_commit_if_unit_drifts_after_ensure(
    tmp_path: Path,
) -> None:
    clock = FakeClock()
    delegate = FakeProcessAdapter(clock)
    adapter = DriftAfterEffectVerificationAdapter(
        delegate, RuntimeUnitId("worker")
    )
    candidate = manager(tmp_path / "journal.sqlite3", adapter, clock)
    retained = unit("worker")
    assert candidate.apply(
        apply_request(
            "initial",
            expected_revision=0,
            target_revision=1,
            units=(retained,),
        )
    ).phase == "committed"
    adapter.effects.clear()
    adapter.inject = True
    request = apply_request(
        "drift-before-commit",
        expected_revision=1,
        target_revision=2,
        units=(retained,),
    )

    incomplete = candidate.apply(request)

    assert incomplete.phase == "incomplete"
    assert incomplete.detail == "runtime_unit_not_converged:worker"
    assert candidate.observed_state().graph_revision == GraphRevision(1)

    recovered = candidate.recover(request.apply_id)
    assert recovered.phase == "committed"
    assert adapter.effects == [("stop", "worker"), ("start", "worker")]
    assert candidate.observed_state().graph_revision == GraphRevision(2)


def test_restart_reconstructs_real_epoch_and_has_no_robot_mutation_surface(
    tmp_path: Path,
) -> None:
    clock = FakeClock()
    process_adapter = FakeProcessAdapter(clock)
    adapter = RecordingProcessAdapter(process_adapter)
    database = tmp_path / "journal.sqlite3"
    candidate = manager(database, adapter, clock)
    request = apply_request(
        "epoch",
        expected_revision=0,
        target_revision=1,
        units=(unit("execution-host"),),
    )
    assert candidate.apply(request).phase == "committed"
    first_epoch = candidate.observed_state().runtime_units[0].runtime_epoch

    process_adapter.crash(RuntimeUnitId("execution-host"))
    process_adapter.start(unit("execution-host"))
    restarted = manager(database, adapter, clock)
    reconstructed = restarted.observed_state()

    assert reconstructed.runtime_units[0].runtime_epoch != first_epoch
    assert reconstructed.runtime_units[0].runtime_epoch == process_adapter.observe(
        RuntimeUnitId("execution-host")
    ).runtime_epoch
    for forbidden in (
        "enable",
        "disable",
        "move",
        "pause",
        "resume",
        "reset",
        "stop_motion",
    ):
        assert not hasattr(restarted, forbidden)
