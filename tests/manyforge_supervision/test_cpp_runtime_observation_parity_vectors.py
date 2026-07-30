from __future__ import annotations

from pathlib import Path

from manyforge_supervision.runtime_observation import (
    ManagedCommandSurfaceState,
    ProcessPresenceObservation,
    ProcessPresenceState,
    RuntimeObservationReducer,
    RuntimeSourceHealth,
    RuntimeSourcePhase,
    RuntimeUnitExpectation,
    RuntimeUnitSourceReport,
)


class ManualClock:
    def __init__(self) -> None:
        self.now_ns = 1

    def __call__(self) -> int:
        return self.now_ns


def expectation(
    runtime_unit_id: str,
    dependencies: tuple[str, ...] = (),
) -> RuntimeUnitExpectation:
    from manyforge_supervision.runtime_observation import (
        RuntimeLifecycleControl,
    )

    return RuntimeUnitExpectation(
        runtime_unit_id=runtime_unit_id,
        lifecycle_control=RuntimeLifecycleControl.MANAGER_MANAGED,
        product_identity="product-v1",
        configuration_identity="configuration-v1",
        maximum_presence_age_ns=100,
        maximum_report_age_ns=100,
        dependency_runtime_unit_ids=dependencies,
        required_runtime_role_ids=("status",),
    )


def presence(
    runtime_unit_id: str,
    incarnation: str,
    sequence: int = 1,
) -> ProcessPresenceObservation:
    return ProcessPresenceObservation(
        runtime_unit_id=runtime_unit_id,
        observer_instance_id="observer-a",
        observation_sequence=sequence,
        state=ProcessPresenceState.PRESENT,
        process_incarnation=incarnation,
    )


def report(
    runtime_unit_id: str,
    incarnation: str,
    sequence: int = 1,
    progress: int = 1,
) -> RuntimeUnitSourceReport:
    return RuntimeUnitSourceReport(
        runtime_unit_id=runtime_unit_id,
        process_incarnation=incarnation,
        source_report_sequence=sequence,
        critical_progress_sequence=progress,
        local_phase=RuntimeSourcePhase.SERVING,
        source_health=RuntimeSourceHealth.HEALTHY,
        product_identity="product-v1",
        configuration_identity="configuration-v1",
        ready_runtime_role_ids=("status",),
        command_surface_state=(
            ManagedCommandSurfaceState.CLOSED_NO_ACTIVE_EFFECT
        ),
    )


def serialize(checkpoint: str, observation: object) -> str:
    reasons = ",".join(observation.reason_codes)
    if not reasons:
        reasons = ",".join(
            f"{dependency}={epoch}"
            for dependency, epoch in (
                observation.dependency_runtime_epochs.items()
            )
        )
    return "|".join(
        (
            checkpoint,
            observation.runtime_unit_id,
            observation.derived_state.value,
            observation.process_presence.value,
            observation.runtime_epoch,
            reasons,
        )
    )


def test_cpp_and_python_runtime_observation_share_golden_vectors() -> None:
    clock = ManualClock()
    subject = RuntimeObservationReducer(
        clock_ns=clock,
        declared_runtime_unit_ids=("dependency", "worker"),
    )
    dependency = expectation("dependency")
    worker = expectation("worker", ("dependency",))

    assert subject.observe_process(
        presence("dependency", "dependency-epoch-1")
    )
    assert subject.observe_process(presence("worker", "worker-epoch-1"))
    assert subject.ingest_source_report(
        report("dependency", "dependency-epoch-1")
    )
    assert subject.ingest_source_report(report("worker", "worker-epoch-1"))
    actual = [
        serialize("initial", observation)
        for observation in subject.evaluate(
            (worker, dependency)
        ).observations
    ]

    clock.now_ns += 1
    assert subject.observe_process(
        presence("dependency", "dependency-epoch-2", 2)
    )
    assert subject.ingest_source_report(
        report("dependency", "dependency-epoch-2")
    )
    actual.extend(
        serialize("dependency_restarted", observation)
        for observation in subject.evaluate(
            (worker, dependency)
        ).observations
    )

    assert subject.ingest_source_report(
        report("worker", "worker-epoch-1", 2, 2)
    )
    actual.extend(
        serialize("worker_rejoined", observation)
        for observation in subject.evaluate(
            (worker, dependency)
        ).observations
    )

    golden_path = (
        Path(__file__).parents[2]
        / "manyforge_component_manager"
        / "testdata"
        / "runtime_observation_parity_v1.txt"
    )
    expected = golden_path.read_text(encoding="ascii").splitlines()
    assert actual == expected
