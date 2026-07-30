from __future__ import annotations

from dataclasses import fields, replace
from inspect import signature

import pytest

from manyforge_supervision.runtime_observation import (
    ManagedCommandSurfaceState,
    ProcessPresenceObservation,
    ProcessPresenceState,
    RuntimeConvergenceState,
    RuntimeLifecycleControl,
    RuntimeObservationReducer,
    RuntimeSourceHealth,
    RuntimeSourcePhase,
    RuntimeUnitExpectation,
    RuntimeUnitProcessBinding,
    RuntimeUnitSourceReport,
    validate_runtime_unit_process_bindings,
)
from verification.composable_cell_i4.runtime_observation_fixture import (
    FixtureCommandSurfaceState,
    FixtureDerivedState,
    FixtureLifecycleControl,
    FixturePresenceState,
    FixtureProcessPresence,
    FixtureRuntimeExpectation,
    FixtureSourceHealth,
    FixtureSourcePhase,
    FixtureSourceReport,
    RuntimeObservationFixtureReducer,
)


class ManualClock:
    def __init__(self) -> None:
        self.now_ns = 1

    def __call__(self) -> int:
        return self.now_ns

    def advance(self, amount_ns: int) -> None:
        self.now_ns += amount_ns


def reducer(
    clock: ManualClock,
    *runtime_unit_ids: str,
) -> RuntimeObservationReducer:
    return RuntimeObservationReducer(
        clock_ns=clock,
        declared_runtime_unit_ids=tuple(runtime_unit_ids),
    )


def expectation(
    runtime_unit_id: str,
    *,
    lifecycle_control: RuntimeLifecycleControl = (
        RuntimeLifecycleControl.MANAGER_MANAGED
    ),
    product_identity: str = "product-v1",
    configuration_identity: str = "configuration-v1",
    maximum_presence_age_ns: int = 100,
    maximum_report_age_ns: int = 100,
    maximum_progress_age_ns: int | None = None,
    dependencies: tuple[str, ...] = (),
    roles: tuple[str, ...] = ("status",),
    command_surface: ManagedCommandSurfaceState = (
        ManagedCommandSurfaceState.CLOSED_NO_ACTIVE_EFFECT
    ),
) -> RuntimeUnitExpectation:
    return RuntimeUnitExpectation(
        runtime_unit_id=runtime_unit_id,
        lifecycle_control=lifecycle_control,
        product_identity=product_identity,
        configuration_identity=configuration_identity,
        maximum_presence_age_ns=maximum_presence_age_ns,
        maximum_report_age_ns=maximum_report_age_ns,
        maximum_progress_age_ns=maximum_progress_age_ns,
        dependency_runtime_unit_ids=dependencies,
        required_runtime_role_ids=roles,
        required_command_surface_state=command_surface,
    )


def presence(
    runtime_unit_id: str,
    process_incarnation: str,
    *,
    observer: str = "observer-a",
    sequence: int = 1,
) -> ProcessPresenceObservation:
    return ProcessPresenceObservation(
        runtime_unit_id=runtime_unit_id,
        observer_instance_id=observer,
        observation_sequence=sequence,
        state=ProcessPresenceState.PRESENT,
        process_incarnation=process_incarnation,
    )


def source_report(
    runtime_unit_id: str,
    process_incarnation: str,
    *,
    sequence: int = 1,
    progress: int = 1,
    phase: RuntimeSourcePhase = RuntimeSourcePhase.SERVING,
    health: RuntimeSourceHealth = RuntimeSourceHealth.HEALTHY,
    product_identity: str = "product-v1",
    configuration_identity: str = "configuration-v1",
    roles: tuple[str, ...] = ("status",),
    command_surface: ManagedCommandSurfaceState = (
        ManagedCommandSurfaceState.CLOSED_NO_ACTIVE_EFFECT
    ),
    reasons: tuple[str, ...] = (),
) -> RuntimeUnitSourceReport:
    return RuntimeUnitSourceReport(
        runtime_unit_id=runtime_unit_id,
        process_incarnation=process_incarnation,
        source_report_sequence=sequence,
        critical_progress_sequence=progress,
        local_phase=phase,
        source_health=health,
        product_identity=product_identity,
        configuration_identity=configuration_identity,
        ready_runtime_role_ids=roles,
        command_surface_state=command_surface,
        reason_codes=reasons,
    )


def test_missing_report_and_first_convergence_have_distinct_states() -> None:
    clock = ManualClock()
    subject = reducer(clock, "unit-a")
    target = expectation("unit-a")

    missing_process = subject.evaluate((target,)).observation("unit-a")
    assert missing_process is not None
    assert missing_process.derived_state is RuntimeConvergenceState.LOST
    assert missing_process.reason_codes == ("process_presence_missing",)

    assert subject.observe_process(presence("unit-a", "epoch-a"))
    missing_report = subject.evaluate((target,)).observation("unit-a")
    assert missing_report is not None
    assert missing_report.derived_state is RuntimeConvergenceState.NONCONVERGED
    assert missing_report.reason_codes == ("source_report_missing",)

    assert subject.ingest_source_report(source_report("unit-a", "epoch-a"))
    converged = subject.evaluate((target,)).observation("unit-a")
    assert converged is not None
    assert converged.converged
    assert converged.runtime_epoch == "epoch-a"
    assert converged.process_observer_instance_id == "observer-a"
    assert converged.process_observation_sequence == 1
    assert converged.source_report_sequence == 1
    assert converged.critical_progress_sequence == 1
    assert converged.reason_codes == ()


def test_source_report_cannot_publish_manager_owned_fields() -> None:
    field_names = {item.name for item in fields(RuntimeUnitSourceReport)}

    assert "derived_state" not in field_names
    assert "runtime_epoch" not in field_names
    assert "dependency_runtime_epochs" not in field_names
    assert "retained_bundle_id" not in field_names
    assert "presence_age_ns" not in field_names
    assert "report_age_ns" not in field_names
    assert "progress_age_ns" not in field_names


def test_receiver_clock_is_the_only_freshness_timestamp_source() -> None:
    observe_parameters = signature(
        RuntimeObservationReducer.observe_process
    ).parameters
    report_parameters = signature(
        RuntimeObservationReducer.ingest_source_report
    ).parameters

    assert "received_at_ns" not in observe_parameters
    assert "received_at_ns" not in report_parameters


def test_duplicate_conflict_and_regression_do_not_renew_report() -> None:
    clock = ManualClock()
    subject = reducer(clock, "unit-a")
    target = expectation(
        "unit-a",
        maximum_presence_age_ns=200,
        maximum_report_age_ns=100,
    )
    first = source_report("unit-a", "epoch-a", sequence=2, progress=2)
    assert subject.observe_process(presence("unit-a", "epoch-a"))
    assert subject.ingest_source_report(first)

    clock.advance(50)
    assert not subject.ingest_source_report(first)
    assert not subject.ingest_source_report(
        replace(first, source_health=RuntimeSourceHealth.DEGRADED)
    )
    assert not subject.ingest_source_report(
        source_report("unit-a", "epoch-a", sequence=1, progress=1)
    )
    assert subject.observe_process(
        presence("unit-a", "epoch-a", sequence=2)
    )

    clock.advance(51)
    observed = subject.evaluate((target,)).observation("unit-a")
    assert observed is not None
    assert observed.derived_state is RuntimeConvergenceState.LOST
    assert observed.report_age_ns == 101
    assert observed.reason_codes == ("source_report_stale",)
    assert subject.metrics.duplicate_total == 1
    assert dict(subject.metrics.rejection_reasons) == {
        "source_report_sequence_conflict": 1,
        "source_report_sequence_regressed": 1,
    }


def test_report_freshness_does_not_fabricate_progress() -> None:
    clock = ManualClock()
    subject = reducer(clock, "unit-a")
    target = expectation(
        "unit-a",
        maximum_presence_age_ns=200,
        maximum_report_age_ns=50,
        maximum_progress_age_ns=100,
    )
    assert subject.observe_process(presence("unit-a", "epoch-a"))
    assert subject.ingest_source_report(
        source_report("unit-a", "epoch-a", sequence=1, progress=1)
    )

    clock.advance(80)
    assert subject.observe_process(
        presence("unit-a", "epoch-a", sequence=2)
    )
    assert subject.ingest_source_report(
        source_report("unit-a", "epoch-a", sequence=2, progress=1)
    )
    clock.advance(21)

    observed = subject.evaluate((target,)).observation("unit-a")
    assert observed is not None
    assert observed.report_age_ns == 21
    assert observed.progress_age_ns == 101
    assert observed.derived_state is RuntimeConvergenceState.LOST
    assert observed.reason_codes == ("critical_progress_stale",)


def test_progress_expiry_is_optional_manager_policy() -> None:
    clock = ManualClock()
    subject = reducer(clock, "unit-a")
    no_progress_expiry = expectation(
        "unit-a",
        maximum_presence_age_ns=200,
        maximum_report_age_ns=100,
    )
    assert subject.observe_process(presence("unit-a", "epoch-a"))
    assert subject.ingest_source_report(
        source_report("unit-a", "epoch-a", sequence=1, progress=1)
    )

    clock.advance(80)
    assert subject.observe_process(
        presence("unit-a", "epoch-a", sequence=2)
    )
    assert subject.ingest_source_report(
        source_report("unit-a", "epoch-a", sequence=2, progress=1)
    )
    clock.advance(21)

    without_expiry = subject.evaluate(
        (no_progress_expiry,)
    ).observation("unit-a")
    assert without_expiry is not None
    assert without_expiry.progress_age_ns == 101
    assert without_expiry.converged

    with_expiry = subject.evaluate(
        (replace(no_progress_expiry, maximum_progress_age_ns=100),)
    ).observation("unit-a")
    assert with_expiry is not None
    assert with_expiry.derived_state is RuntimeConvergenceState.LOST
    assert with_expiry.reason_codes == ("critical_progress_stale",)


def test_observer_replacement_fences_retired_session_replay() -> None:
    clock = ManualClock()
    subject = reducer(clock, "unit-a")
    target = expectation(
        "unit-a",
        maximum_presence_age_ns=40,
        maximum_report_age_ns=100,
    )
    first = presence(
        "unit-a",
        "epoch-a",
        observer="observer-a",
    )
    replacement_observation = presence(
        "unit-a",
        "epoch-a",
        observer="observer-b",
    )
    assert subject.observe_process(first)
    assert subject.ingest_source_report(source_report("unit-a", "epoch-a"))
    clock.advance(1)
    assert subject.replace_process_observer(replacement_observation)

    clock.advance(41)
    assert not subject.observe_process(first)
    assert not subject.observe_process(replacement_observation)
    observed = subject.evaluate((target,)).observation("unit-a")

    assert observed is not None
    assert observed.derived_state is RuntimeConvergenceState.LOST
    assert observed.presence_age_ns == 41
    assert observed.reason_codes == ("process_presence_stale",)
    assert subject.metrics.accepted_presence_total == 2
    assert subject.metrics.duplicate_total == 1
    assert dict(subject.metrics.rejection_reasons) == {
        "process_observer_session_retired": 1,
    }


def test_presence_duplicate_conflict_and_regression_do_not_renew_freshness() -> None:
    clock = ManualClock()
    subject = reducer(clock, "unit-a")
    target = expectation(
        "unit-a",
        maximum_presence_age_ns=100,
        maximum_report_age_ns=200,
    )
    first = presence("unit-a", "epoch-a", sequence=2)
    assert subject.observe_process(first)
    assert subject.ingest_source_report(source_report("unit-a", "epoch-a"))

    clock.advance(50)
    assert not subject.observe_process(first)
    assert not subject.observe_process(
        replace(first, process_incarnation="epoch-conflict")
    )
    assert not subject.observe_process(
        presence("unit-a", "epoch-a", sequence=1)
    )
    clock.advance(51)

    observed = subject.evaluate((target,)).observation("unit-a")
    assert observed is not None
    assert observed.derived_state is RuntimeConvergenceState.LOST
    assert observed.presence_age_ns == 101
    assert observed.reason_codes == ("process_presence_stale",)
    assert subject.metrics.duplicate_total == 1
    assert dict(subject.metrics.rejection_reasons) == {
        "process_observation_sequence_conflict": 1,
        "process_observation_sequence_regressed": 1,
    }


def test_absence_retires_epoch_and_reuse_fails_closed() -> None:
    clock = ManualClock()
    subject = reducer(clock, "unit-a")
    target = expectation("unit-a")
    assert subject.observe_process(presence("unit-a", "epoch-a"))
    assert subject.ingest_source_report(source_report("unit-a", "epoch-a"))
    assert subject.evaluate((target,)).observation("unit-a").converged

    clock.advance(1)
    assert subject.observe_process(
        ProcessPresenceObservation(
            runtime_unit_id="unit-a",
            observer_instance_id="observer-a",
            observation_sequence=2,
            state=ProcessPresenceState.ABSENT,
        )
    )
    assert not subject.observe_process(
        presence("unit-a", "epoch-a", sequence=3)
    )
    assert not subject.ingest_source_report(
        source_report("unit-a", "epoch-a", sequence=2, progress=2)
    )
    absent = subject.evaluate((target,)).observation("unit-a")
    assert absent is not None
    assert absent.derived_state is RuntimeConvergenceState.LOST
    assert absent.process_presence is ProcessPresenceState.ABSENT

    assert subject.observe_process(
        presence("unit-a", "epoch-b", sequence=4)
    )
    recovering = subject.evaluate((target,)).observation("unit-a")
    assert recovering is not None
    assert recovering.derived_state is RuntimeConvergenceState.RECOVERING
    assert recovering.reason_codes == ("source_report_missing",)


def test_unknown_presence_does_not_fabricate_a_restart() -> None:
    clock = ManualClock()
    subject = reducer(clock, "unit-a")
    target = expectation("unit-a")
    assert subject.observe_process(presence("unit-a", "epoch-a"))
    assert subject.ingest_source_report(source_report("unit-a", "epoch-a"))
    assert subject.evaluate((target,)).observation("unit-a").converged

    clock.advance(1)
    assert subject.observe_process(
        ProcessPresenceObservation(
            runtime_unit_id="unit-a",
            observer_instance_id="observer-a",
            observation_sequence=2,
            state=ProcessPresenceState.UNKNOWN,
        )
    )
    unknown = subject.evaluate((target,)).observation("unit-a")
    assert unknown is not None
    assert unknown.derived_state is RuntimeConvergenceState.LOST
    assert unknown.runtime_epoch == ""
    assert unknown.reason_codes == ("process_presence_unknown",)

    assert subject.observe_process(
        presence("unit-a", "epoch-a", sequence=3)
    )
    assert subject.evaluate((target,)).observation("unit-a").converged


def test_direct_incarnation_replacement_requires_new_source_report() -> None:
    clock = ManualClock()
    subject = reducer(clock, "unit-a")
    target = expectation("unit-a")
    assert subject.observe_process(presence("unit-a", "epoch-a"))
    assert subject.ingest_source_report(source_report("unit-a", "epoch-a"))
    assert subject.evaluate((target,)).observation("unit-a").converged

    clock.advance(1)
    assert subject.observe_process(
        presence("unit-a", "epoch-b", sequence=2)
    )
    before_report = subject.evaluate((target,)).observation("unit-a")
    assert before_report is not None
    assert before_report.derived_state is RuntimeConvergenceState.RECOVERING
    assert before_report.runtime_epoch == "epoch-b"
    assert before_report.reason_codes == ("source_report_missing",)
    assert not subject.ingest_source_report(
        source_report("unit-a", "epoch-a", sequence=2, progress=2)
    )

    assert subject.ingest_source_report(source_report("unit-a", "epoch-b"))
    assert subject.evaluate((target,)).observation("unit-a").converged


def test_off_presence_report_cannot_become_latent_evidence() -> None:
    clock = ManualClock()
    subject = reducer(clock, "unit-a")
    target = expectation("unit-a")
    assert subject.observe_process(presence("unit-a", "epoch-a"))
    assert subject.ingest_source_report(source_report("unit-a", "epoch-a"))
    assert subject.evaluate((target,)).observation("unit-a").converged
    assert not subject.ingest_source_report(
        source_report("unit-a", "epoch-b")
    )

    clock.advance(1)
    assert subject.observe_process(
        presence("unit-a", "epoch-b", sequence=2)
    )
    before_report = subject.evaluate((target,)).observation("unit-a")
    assert before_report is not None
    assert before_report.derived_state is RuntimeConvergenceState.RECOVERING
    assert before_report.reason_codes == ("source_report_missing",)


def test_dependency_epoch_change_requires_fresh_dependent_report() -> None:
    clock = ManualClock()
    subject = reducer(clock, "dependency", "dependant")
    dependency = expectation("dependency")
    dependant = expectation(
        "dependant",
        dependencies=("dependency",),
    )
    assert subject.observe_process(
        presence("dependency", "dependency-epoch-1")
    )
    assert subject.observe_process(
        presence("dependant", "dependant-epoch-1")
    )
    assert subject.ingest_source_report(
        source_report("dependency", "dependency-epoch-1")
    )
    assert subject.ingest_source_report(
        source_report("dependant", "dependant-epoch-1")
    )

    initial = subject.evaluate((dependant, dependency))
    assert initial.observation("dependency").converged
    assert initial.observation("dependant").converged

    clock.advance(1)
    assert subject.observe_process(
        presence("dependency", "dependency-epoch-2", sequence=2)
    )
    assert subject.ingest_source_report(
        source_report("dependency", "dependency-epoch-2")
    )
    assert subject.observe_process(
        presence("dependant", "dependant-epoch-1", sequence=2)
    )
    replaced_dependency = subject.evaluate((dependency, dependant))
    dependant_observation = replaced_dependency.observation("dependant")
    assert dependant_observation is not None
    assert (
        dependant_observation.derived_state
        is RuntimeConvergenceState.RECOVERING
    )
    assert dependant_observation.reason_codes == (
        "runtime_dependency_epoch_changed",
    )

    assert subject.ingest_source_report(
        source_report(
            "dependant",
            "dependant-epoch-1",
            sequence=2,
            progress=2,
        )
    )
    recovered = subject.evaluate((dependency, dependant))
    assert recovered.observation("dependant").converged
    assert dict(
        recovered.observation("dependant").dependency_runtime_epochs
    ) == {"dependency": "dependency-epoch-2"}


def test_dependent_report_before_epoch_change_cannot_satisfy_new_join() -> None:
    clock = ManualClock()
    subject = reducer(clock, "dependency", "dependant")
    dependency = expectation("dependency")
    dependant = expectation(
        "dependant",
        dependencies=("dependency",),
    )
    assert subject.observe_process(
        presence("dependency", "dependency-epoch-1")
    )
    assert subject.ingest_source_report(
        source_report("dependency", "dependency-epoch-1")
    )
    assert subject.observe_process(
        presence("dependant", "dependant-epoch-1")
    )
    assert subject.ingest_source_report(
        source_report("dependant", "dependant-epoch-1")
    )
    assert subject.evaluate((dependency, dependant)).observation(
        "dependant"
    ).converged

    clock.advance(1)
    assert subject.observe_process(
        presence("dependency", "dependency-epoch-1", sequence=2)
    )
    assert subject.evaluate((dependency, dependant)).observation(
        "dependant"
    ).converged

    clock.advance(1)
    pre_restart_report = source_report(
        "dependant",
        "dependant-epoch-1",
        sequence=2,
        progress=2,
    )
    assert subject.ingest_source_report(pre_restart_report)
    clock.advance(1)
    assert subject.observe_process(
        presence("dependency", "dependency-epoch-2", sequence=3)
    )
    assert subject.ingest_source_report(
        source_report("dependency", "dependency-epoch-2")
    )
    assert not subject.ingest_source_report(pre_restart_report)

    fenced = subject.evaluate((dependency, dependant)).observation("dependant")
    assert fenced is not None
    assert fenced.derived_state is RuntimeConvergenceState.RECOVERING
    assert fenced.reason_codes == ("runtime_dependency_epoch_changed",)
    assert dict(fenced.dependency_runtime_epochs) == {
        "dependency": "dependency-epoch-2"
    }

    assert subject.ingest_source_report(
        source_report(
            "dependant",
            "dependant-epoch-1",
            sequence=3,
            progress=3,
        )
    )
    assert subject.evaluate((dependency, dependant)).observation(
        "dependant"
    ).converged


@pytest.mark.parametrize(
    ("report_change", "expected_reason"),
    (
        (
            {"product_identity": "wrong-product"},
            "runtime_product_identity_mismatch",
        ),
        (
            {"configuration_identity": "wrong-configuration"},
            "runtime_configuration_identity_mismatch",
        ),
        (
            {"phase": RuntimeSourcePhase.STARTING},
            "runtime_source_phase_starting",
        ),
        (
            {"health": RuntimeSourceHealth.DEGRADED},
            "runtime_source_health_degraded",
        ),
        ({"roles": ()}, "runtime_roles_not_ready"),
        (
            {"command_surface": ManagedCommandSurfaceState.OPEN},
            "runtime_command_surface_state_mismatch",
        ),
    ),
)
def test_exact_source_evidence_is_required_for_convergence(
    report_change: dict[str, object],
    expected_reason: str,
) -> None:
    clock = ManualClock()
    subject = reducer(clock, "unit-a")
    assert subject.observe_process(presence("unit-a", "epoch-a"))
    assert subject.ingest_source_report(
        source_report("unit-a", "epoch-a", **report_change)
    )

    observed = subject.evaluate(
        (expectation("unit-a"),)
    ).observation("unit-a")
    assert observed is not None
    assert observed.derived_state is RuntimeConvergenceState.NONCONVERGED
    assert observed.reason_codes == (expected_reason,)


def test_source_and_manager_reasons_remain_independent_and_immutable() -> None:
    clock = ManualClock()
    subject = reducer(clock, "unit-a")
    source_reasons = tuple(f"source-reason-{index:02}" for index in range(32))
    assert subject.observe_process(presence("unit-a", "epoch-a"))
    assert subject.ingest_source_report(
        source_report(
            "unit-a",
            "epoch-a",
            reasons=source_reasons,
        )
    )
    clock.advance(101)

    observed = subject.evaluate(
        (
            expectation(
                "unit-a",
                maximum_presence_age_ns=200,
                maximum_report_age_ns=100,
            ),
        )
    ).observation("unit-a")
    assert observed is not None
    assert observed.source_reason_codes == source_reasons
    assert observed.reason_codes == ("source_report_stale",)
    with pytest.raises(TypeError):
        observed.dependency_runtime_epochs["other"] = "epoch"
    with pytest.raises(TypeError):
        subject.metrics.rejection_reasons["forged"] = 1


def test_expectation_graph_is_strict_bounded_and_acyclic() -> None:
    clock = ManualClock()
    subject = reducer(clock, "unit-a", "unit-b")

    with pytest.raises(ValueError, match="unknown dependencies"):
        subject.evaluate(
            (expectation("unit-a", dependencies=("missing",)),)
        )
    with pytest.raises(ValueError, match="contain a cycle"):
        subject.evaluate(
            (
                expectation("unit-a", dependencies=("unit-b",)),
                expectation("unit-b", dependencies=("unit-a",)),
            )
        )
    with pytest.raises(ValueError, match="bounded tuple"):
        RuntimeObservationReducer(
            clock_ns=clock,
            declared_runtime_unit_ids=tuple(
                f"unit-{index}" for index in range(65)
            ),
        )


def test_undeclared_ingress_and_unbounded_source_sets_fail_closed() -> None:
    clock = ManualClock()
    subject = reducer(clock, "unit-a")

    assert not subject.observe_process(presence("unit-b", "epoch-b"))
    assert not subject.ingest_source_report(
        source_report("unit-b", "epoch-b")
    )
    assert dict(subject.metrics.rejection_reasons) == {
        "process_presence_runtime_unit_undeclared": 1,
        "source_report_runtime_unit_undeclared": 1,
    }
    with pytest.raises(ValueError, match="bounded tuple"):
        replace(
            source_report("unit-a", "epoch-a"),
            reason_codes=tuple(f"reason-{index}" for index in range(33)),
        )
    with pytest.raises(ValueError, match="unique"):
        replace(
            source_report("unit-a", "epoch-a"),
            ready_runtime_role_ids=("status", "status"),
        )


def test_maximum_length_dependency_id_keeps_precise_manager_reason() -> None:
    dependency_id = "d" * 256
    clock = ManualClock()
    subject = reducer(clock, dependency_id, "dependant")
    assert subject.observe_process(presence("dependant", "dependant-epoch"))
    assert subject.ingest_source_report(
        source_report("dependant", "dependant-epoch")
    )

    observed = subject.evaluate(
        (
            expectation("dependant", dependencies=(dependency_id,)),
            expectation(dependency_id),
        )
    ).observation("dependant")
    assert observed is not None
    assert observed.derived_state is RuntimeConvergenceState.NONCONVERGED
    assert observed.reason_codes == (
        f"runtime_dependency_not_converged:{dependency_id}",
    )


def test_observer_and_incarnation_replay_ledgers_are_bounded() -> None:
    clock = ManualClock()
    observer_subject = reducer(clock, "unit-a")
    assert observer_subject.observe_process(
        presence("unit-a", "epoch-a", observer="observer-0")
    )
    for index in range(1, 64):
        assert observer_subject.replace_process_observer(
            presence(
                "unit-a",
                "epoch-a",
                observer=f"observer-{index}",
            )
        )
    assert not observer_subject.replace_process_observer(
        presence("unit-a", "epoch-a", observer="observer-64")
    )
    assert dict(observer_subject.metrics.rejection_reasons) == {
        "process_observer_session_limit_exceeded": 1,
    }

    incarnation_subject = reducer(clock, "unit-a")
    for index in range(64):
        assert incarnation_subject.observe_process(
            presence(
                "unit-a",
                f"epoch-{index}",
                sequence=index + 1,
            )
        )
        assert incarnation_subject.ingest_source_report(
            source_report("unit-a", f"epoch-{index}")
        )
    assert not incarnation_subject.observe_process(
        presence("unit-a", "epoch-64", sequence=65)
    )
    assert dict(incarnation_subject.metrics.rejection_reasons) == {
        "process_incarnation_limit_exceeded": 1,
    }
    assert tuple(incarnation_subject._reports["unit-a"]) == ("epoch-63",)


def binding(
    runtime_unit_id: str,
    *,
    boundary: str,
    suffix: str,
    lifecycle_control: RuntimeLifecycleControl = (
        RuntimeLifecycleControl.MANAGER_MANAGED
    ),
) -> RuntimeUnitProcessBinding:
    return RuntimeUnitProcessBinding(
        runtime_unit_id=runtime_unit_id,
        process_boundary_id=boundary,
        lifecycle_control=lifecycle_control,
        process_observation_handle_id=f"observation-{suffix}",
        status_source_id=f"status-{suffix}",
        progress_source_id=f"progress-{suffix}",
        incarnation_source_id=f"incarnation-{suffix}",
        managed_lifecycle_handle_id=(
            f"lifecycle-{suffix}"
            if lifecycle_control is RuntimeLifecycleControl.MANAGER_MANAGED
            else ""
        ),
    )


def test_process_binding_separates_observation_and_manager_effects() -> None:
    validate_runtime_unit_process_bindings(
        (
            binding("unit-a", boundary="shared-process", suffix="a"),
            binding("unit-b", boundary="shared-process", suffix="b"),
        )
    )
    with pytest.raises(ValueError, match="lack independent"):
        validate_runtime_unit_process_bindings(
            (
                binding("unit-a", boundary="shared-process", suffix="a"),
                binding("unit-b", boundary="shared-process", suffix="a"),
            )
        )

    first = binding("unit-a", boundary="claimed-a", suffix="a")
    second = binding("unit-b", boundary="claimed-b", suffix="b")
    for handle_field in (
        "process_observation_handle_id",
        "status_source_id",
        "progress_source_id",
        "incarnation_source_id",
        "managed_lifecycle_handle_id",
    ):
        aliased = replace(
            second,
            **{handle_field: getattr(first, handle_field)},
        )
        with pytest.raises(ValueError, match="lack independent"):
            validate_runtime_unit_process_bindings((first, aliased))

    external = binding(
        "external",
        boundary="external-process",
        suffix="external",
        lifecycle_control=RuntimeLifecycleControl.EXTERNALLY_MANAGED,
    )
    assert external.managed_lifecycle_handle_id == ""
    with pytest.raises(ValueError, match="cannot expose"):
        replace(external, managed_lifecycle_handle_id="forbidden")


def test_monotonic_receiver_clock_regression_fails_closed() -> None:
    clock = ManualClock()
    subject = reducer(clock, "unit-a")
    assert subject.observe_process(presence("unit-a", "epoch-a"))
    clock.now_ns = 10
    subject.evaluate((expectation("unit-a"),))
    clock.now_ns = 9
    with pytest.raises(RuntimeError, match="regressed"):
        subject.evaluate((expectation("unit-a"),))


def test_fixture_and_production_core_share_derived_state_semantics() -> None:
    clock = ManualClock()
    production = reducer(clock, "dependency", "dependant")
    fixture = RuntimeObservationFixtureReducer(
        clock_ns=clock,
        declared_runtime_unit_ids=("dependency", "dependant"),
    )
    production_expectations = (
        expectation("dependant", dependencies=("dependency",)),
        expectation("dependency"),
    )
    fixture_expectations = (
        FixtureRuntimeExpectation(
            runtime_unit_id="dependant",
            lifecycle_control=FixtureLifecycleControl.MANAGER_MANAGED,
            product_identity="product-v1",
            configuration_identity="configuration-v1",
            maximum_presence_age_ns=100,
            maximum_report_age_ns=100,
            dependency_runtime_unit_ids=("dependency",),
            required_runtime_role_ids=("status",),
            required_command_surface_state=(
                FixtureCommandSurfaceState.CLOSED_NO_ACTIVE_EFFECT
            ),
        ),
        FixtureRuntimeExpectation(
            runtime_unit_id="dependency",
            lifecycle_control=FixtureLifecycleControl.MANAGER_MANAGED,
            product_identity="product-v1",
            configuration_identity="configuration-v1",
            maximum_presence_age_ns=100,
            maximum_report_age_ns=100,
            required_runtime_role_ids=("status",),
            required_command_surface_state=(
                FixtureCommandSurfaceState.CLOSED_NO_ACTIVE_EFFECT
            ),
        ),
    )

    for runtime_unit_id, runtime_epoch in (
        ("dependency", "dependency-epoch"),
        ("dependant", "dependant-epoch"),
    ):
        assert production.observe_process(
            presence(runtime_unit_id, runtime_epoch)
        )
        assert fixture.observe_process(
            FixtureProcessPresence(
                runtime_unit_id=runtime_unit_id,
                observer_instance_id="observer-a",
                observation_sequence=1,
                state=FixturePresenceState.PRESENT,
                process_incarnation=runtime_epoch,
            )
        )
        assert production.ingest_source_report(
            source_report(runtime_unit_id, runtime_epoch)
        )
        assert fixture.ingest_report(
            FixtureSourceReport(
                runtime_unit_id=runtime_unit_id,
                process_incarnation=runtime_epoch,
                source_report_sequence=1,
                critical_progress_sequence=1,
                local_phase=FixtureSourcePhase.SERVING,
                source_health=FixtureSourceHealth.HEALTHY,
                product_identity="product-v1",
                configuration_identity="configuration-v1",
                ready_runtime_role_ids=("status",),
                command_surface_state=(
                    FixtureCommandSurfaceState.CLOSED_NO_ACTIVE_EFFECT
                ),
            )
        )

    production_snapshot = production.evaluate(production_expectations)
    fixture_snapshot = fixture.evaluate(fixture_expectations)
    assert tuple(
        (
            item.runtime_unit_id,
            item.derived_state.value,
            item.runtime_epoch,
            tuple(item.dependency_runtime_epochs.items()),
            item.reason_codes,
        )
        for item in production_snapshot.observations
    ) == tuple(
        (
            item.runtime_unit_id,
            item.derived_state.value,
            item.runtime_epoch,
            tuple(item.dependency_runtime_epochs.items()),
            item.reason_codes,
        )
        for item in fixture_snapshot.observations
    )
    assert all(
        item.derived_state is FixtureDerivedState.CONVERGED
        for item in fixture_snapshot.observations
    )
