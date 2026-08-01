from __future__ import annotations

from dataclasses import fields, replace
from inspect import signature

import pytest

from verification.composable_cell_i4.runtime_observation_fixture import (
    FixtureCommandSurfaceState,
    FixtureDerivedState,
    FixtureLifecycleControl,
    FixturePresenceState,
    FixtureProcessPresence,
    FixtureRuntimeExpectation,
    FixtureRuntimeProcessBinding,
    FixtureSourceHealth,
    FixtureSourcePhase,
    FixtureSourceReport,
    RuntimeObservationFixtureReducer,
    validate_fixture_process_bindings,
)


class ManualClock:
    def __init__(self, now_ns: int = 1) -> None:
        self.now_ns = now_ns

    def __call__(self) -> int:
        return self.now_ns

    def advance(self, amount_ns: int) -> int:
        self.now_ns += amount_ns
        return self.now_ns


def expectation(
    runtime_unit_id: str,
    *,
    product: str = "fixture-product-v1",
    configuration: str = "fixture-config-v1",
    dependencies: tuple[str, ...] = (),
    maximum_presence_age_ns: int = 100,
    maximum_report_age_ns: int = 100,
    maximum_progress_age_ns: int | None = None,
) -> FixtureRuntimeExpectation:
    return FixtureRuntimeExpectation(
        runtime_unit_id=runtime_unit_id,
        lifecycle_control=FixtureLifecycleControl.MANAGER_MANAGED,
        product_identity=product,
        configuration_identity=configuration,
        maximum_presence_age_ns=maximum_presence_age_ns,
        maximum_report_age_ns=maximum_report_age_ns,
        maximum_progress_age_ns=maximum_progress_age_ns,
        dependency_runtime_unit_ids=dependencies,
        required_runtime_role_ids=("fixture.runtime-role",),
    )


def fixture_reducer(
    clock: ManualClock,
    *runtime_unit_ids: str,
) -> RuntimeObservationFixtureReducer:
    return RuntimeObservationFixtureReducer(
        clock_ns=clock,
        declared_runtime_unit_ids=tuple(runtime_unit_ids),
    )


def presence(
    runtime_unit_id: str,
    incarnation: str,
    *,
    sequence: int = 1,
    observer: str = "observer-a",
) -> FixtureProcessPresence:
    return FixtureProcessPresence(
        runtime_unit_id=runtime_unit_id,
        observer_instance_id=observer,
        observation_sequence=sequence,
        state=FixturePresenceState.PRESENT,
        process_incarnation=incarnation,
    )


def report(
    runtime_unit_id: str,
    incarnation: str,
    *,
    sequence: int = 1,
    progress: int = 1,
    product: str = "fixture-product-v1",
    configuration: str = "fixture-config-v1",
    phase: FixtureSourcePhase = FixtureSourcePhase.SERVING,
    health: FixtureSourceHealth = FixtureSourceHealth.HEALTHY,
) -> FixtureSourceReport:
    return FixtureSourceReport(
        runtime_unit_id=runtime_unit_id,
        process_incarnation=incarnation,
        source_report_sequence=sequence,
        critical_progress_sequence=progress,
        local_phase=phase,
        source_health=health,
        product_identity=product,
        configuration_identity=configuration,
        ready_runtime_role_ids=("fixture.runtime-role",),
        command_surface_state=(
            FixtureCommandSurfaceState.CLOSED_NO_ACTIVE_EFFECT
        ),
    )


def test_presence_and_source_report_are_independent_evidence() -> None:
    clock = ManualClock()
    reducer = fixture_reducer(clock, "unit-a")
    target = expectation("unit-a")

    assert reducer.observe_process(presence("unit-a", "epoch-a"))
    observed = reducer.evaluate((target,)).observation("unit-a")

    assert observed is not None
    assert observed.derived_state is FixtureDerivedState.NONCONVERGED
    assert not observed.converged
    assert observed.runtime_epoch == "epoch-a"
    assert observed.reason_codes == ("source_report_missing",)

    assert reducer.ingest_report(report("unit-a", "epoch-a"))
    observed = reducer.evaluate((target,)).observation("unit-a")
    assert observed is not None
    assert observed.converged


def test_source_report_has_no_manager_owned_dependency_or_bundle_fields() -> None:
    field_names = {item.name for item in fields(FixtureSourceReport)}

    assert "dependency_epochs" not in field_names
    assert "dependency_runtime_epochs" not in field_names
    assert "retained_bundle_id" not in field_names
    assert "derived_state" not in field_names


def test_receiver_clock_is_the_only_freshness_timestamp_source() -> None:
    receiver_ingress_methods = (
        RuntimeObservationFixtureReducer.observe_process,
        RuntimeObservationFixtureReducer.replace_process_observer,
        RuntimeObservationFixtureReducer.ingest_report,
    )

    for ingress_method in receiver_ingress_methods:
        assert "received_at_ns" not in signature(ingress_method).parameters


def test_duplicate_conflict_and_regression_do_not_renew_freshness() -> None:
    clock = ManualClock()
    reducer = fixture_reducer(clock, "unit-a")
    target = expectation(
        "unit-a",
        maximum_presence_age_ns=200,
        maximum_report_age_ns=100,
        maximum_progress_age_ns=100,
    )
    first_presence = presence("unit-a", "epoch-a")
    first_report = report("unit-a", "epoch-a", sequence=2, progress=2)
    assert reducer.observe_process(first_presence)
    assert reducer.ingest_report(first_report)

    clock.advance(50)
    assert not reducer.ingest_report(first_report)
    conflict = replace(first_report, source_health=FixtureSourceHealth.DEGRADED)
    assert not reducer.ingest_report(conflict)
    assert not reducer.ingest_report(
        report("unit-a", "epoch-a", sequence=1, progress=1)
    )
    assert reducer.observe_process(
        presence("unit-a", "epoch-a", sequence=2)
    )

    clock.advance(51)
    observed = reducer.evaluate((target,)).observation("unit-a")

    assert observed is not None
    assert observed.derived_state is FixtureDerivedState.LOST
    assert "source_report_stale" in observed.reason_codes
    assert reducer.duplicate_total == 1
    assert reducer.rejection_reasons == {
        "source_report_sequence_conflict": 1,
        "source_report_sequence_regressed": 1,
    }


def test_report_freshness_does_not_fake_meaningful_progress() -> None:
    clock = ManualClock()
    reducer = fixture_reducer(clock, "unit-a")
    target = expectation(
        "unit-a",
        maximum_presence_age_ns=200,
        maximum_report_age_ns=50,
        maximum_progress_age_ns=100,
    )
    assert reducer.observe_process(presence("unit-a", "epoch-a"))
    assert reducer.ingest_report(
        report("unit-a", "epoch-a", sequence=1, progress=1)
    )

    clock.advance(80)
    assert reducer.observe_process(
        presence("unit-a", "epoch-a", sequence=2)
    )
    assert reducer.ingest_report(
        report("unit-a", "epoch-a", sequence=2, progress=1)
    )
    clock.advance(21)
    observed = reducer.evaluate((target,)).observation("unit-a")

    assert observed is not None
    assert observed.report_age_ns == 21
    assert observed.progress_age_ns == 101
    assert observed.reason_codes == ("critical_progress_stale",)
    assert observed.derived_state is FixtureDerivedState.LOST


def test_source_local_state_is_not_manager_convergence() -> None:
    clock = ManualClock()
    reducer = fixture_reducer(clock, "unit-a")
    target = expectation("unit-a")
    reducer.observe_process(presence("unit-a", "epoch-a"))
    reducer.ingest_report(
        report(
            "unit-a",
            "epoch-a",
            phase=FixtureSourcePhase.STARTING,
            health=FixtureSourceHealth.HEALTHY,
        )
    )

    observed = reducer.evaluate((target,)).observation("unit-a")

    assert observed is not None
    assert observed.derived_state is FixtureDerivedState.NONCONVERGED
    assert observed.reason_codes == ("runtime_source_phase_starting",)


def test_dependency_epoch_change_requires_fresh_dependent_report() -> None:
    clock = ManualClock()
    reducer = fixture_reducer(clock, "dependency", "dependant")
    dependency = expectation("dependency")
    dependant = expectation("dependant", dependencies=("dependency",))
    reducer.observe_process(presence("dependency", "dependency-epoch-1"))
    reducer.observe_process(presence("dependant", "dependant-epoch-1"))
    reducer.ingest_report(report("dependency", "dependency-epoch-1"))
    reducer.ingest_report(report("dependant", "dependant-epoch-1"))

    initial = reducer.evaluate((dependant, dependency))
    assert initial.observation("dependency").converged
    assert initial.observation("dependant").converged
    assert dict(
        initial.observation("dependant").dependency_runtime_epochs
    ) == {"dependency": "dependency-epoch-1"}

    clock.advance(1)
    reducer.observe_process(
        presence("dependency", "dependency-epoch-2", sequence=2)
    )
    reducer.ingest_report(
        report("dependency", "dependency-epoch-2", sequence=1)
    )
    reducer.observe_process(
        presence("dependant", "dependant-epoch-1", sequence=2)
    )

    replaced = reducer.evaluate((dependency, dependant))
    dependant_observation = replaced.observation("dependant")
    assert dependant_observation is not None
    assert dependant_observation.derived_state is FixtureDerivedState.RECOVERING
    assert dependant_observation.reason_codes == (
        "runtime_dependency_epoch_changed",
    )

    reducer.ingest_report(
        report("dependant", "dependant-epoch-1", sequence=2, progress=2)
    )
    recovered = reducer.evaluate((dependency, dependant))
    assert recovered.observation("dependant").converged
    assert dict(
        recovered.observation("dependant").dependency_runtime_epochs
    ) == {"dependency": "dependency-epoch-2"}


def test_dependent_report_before_epoch_change_cannot_satisfy_new_join() -> None:
    clock = ManualClock()
    reducer = fixture_reducer(clock, "dependency", "dependant")
    dependency = expectation("dependency")
    dependant = expectation("dependant", dependencies=("dependency",))
    assert reducer.observe_process(
        presence("dependency", "dependency-epoch-1")
    )
    assert reducer.ingest_report(
        report("dependency", "dependency-epoch-1")
    )
    assert reducer.observe_process(
        presence("dependant", "dependant-epoch-1")
    )
    assert reducer.ingest_report(report("dependant", "dependant-epoch-1"))
    assert reducer.evaluate((dependency, dependant)).observation(
        "dependant"
    ).converged

    clock.advance(1)
    assert reducer.observe_process(
        presence("dependency", "dependency-epoch-1", sequence=2)
    )
    assert reducer.evaluate((dependency, dependant)).observation(
        "dependant"
    ).converged

    clock.advance(1)
    pre_restart_report = report(
        "dependant",
        "dependant-epoch-1",
        sequence=2,
        progress=2,
    )
    assert reducer.ingest_report(pre_restart_report)
    clock.advance(1)
    assert reducer.observe_process(
        presence("dependency", "dependency-epoch-2", sequence=3)
    )
    assert reducer.ingest_report(
        report("dependency", "dependency-epoch-2")
    )
    assert not reducer.ingest_report(pre_restart_report)

    fenced = reducer.evaluate((dependency, dependant)).observation("dependant")
    assert fenced is not None
    assert fenced.derived_state is FixtureDerivedState.RECOVERING
    assert fenced.reason_codes == ("runtime_dependency_epoch_changed",)
    assert dict(fenced.dependency_runtime_epochs) == {
        "dependency": "dependency-epoch-2"
    }

    assert reducer.ingest_report(
        report(
            "dependant",
            "dependant-epoch-1",
            sequence=3,
            progress=3,
        )
    )
    assert reducer.evaluate((dependency, dependant)).observation(
        "dependant"
    ).converged


def test_unrelated_expectation_does_not_replace_valid_epoch_or_product() -> None:
    clock = ManualClock()
    reducer = fixture_reducer(clock, "retained", "unrelated")
    retained = expectation("retained")
    reducer.observe_process(presence("retained", "retained-epoch"))
    reducer.ingest_report(report("retained", "retained-epoch"))
    first = reducer.evaluate((retained,)).observation("retained")
    assert first is not None and first.converged

    with_unrelated = reducer.evaluate(
        (retained, expectation("unrelated"))
    )

    retained_again = with_unrelated.observation("retained")
    unrelated = with_unrelated.observation("unrelated")
    assert retained_again is not None and retained_again.converged
    assert retained_again.runtime_epoch == "retained-epoch"
    assert (
        retained_again.observed_product_identity
        == "fixture-product-v1"
    )
    assert unrelated is not None
    assert unrelated.derived_state is FixtureDerivedState.LOST


def test_changed_expected_product_requires_fresh_exact_source_evidence() -> None:
    clock = ManualClock()
    reducer = fixture_reducer(clock, "unit-a")
    old = expectation("unit-a", product="fixture-product-old")
    reducer.observe_process(presence("unit-a", "epoch-a"))
    reducer.ingest_report(
        report("unit-a", "epoch-a", product="fixture-product-old")
    )
    assert reducer.evaluate((old,)).observation("unit-a").converged

    new = expectation("unit-a", product="fixture-product-new")
    stale_product = reducer.evaluate((new,)).observation("unit-a")
    assert stale_product is not None
    assert stale_product.derived_state is FixtureDerivedState.NONCONVERGED
    assert stale_product.reason_codes == (
        "runtime_product_identity_mismatch",
    )

    reducer.ingest_report(
        report(
            "unit-a",
            "epoch-a",
            sequence=2,
            progress=2,
            product="fixture-product-new",
        )
    )
    assert reducer.evaluate((new,)).observation("unit-a").converged


def test_retired_observer_replay_cannot_renew_presence_freshness() -> None:
    clock = ManualClock()
    reducer = fixture_reducer(clock, "unit-a")
    target = expectation(
        "unit-a",
        maximum_presence_age_ns=40,
        maximum_report_age_ns=100,
    )
    first = presence("unit-a", "epoch-a", observer="observer-a")
    replacement = presence(
        "unit-a",
        "epoch-a",
        observer="observer-b",
    )
    assert reducer.observe_process(first)
    assert reducer.ingest_report(report("unit-a", "epoch-a"))
    clock.advance(1)
    assert reducer.replace_process_observer(replacement)

    clock.advance(41)
    assert not reducer.observe_process(first)
    assert not reducer.observe_process(replacement)
    observed = reducer.evaluate((target,)).observation("unit-a")

    assert observed is not None
    assert observed.derived_state is FixtureDerivedState.LOST
    assert observed.presence_age_ns == 41
    assert observed.reason_codes == ("process_presence_stale",)
    assert reducer.accepted_presence_total == 2
    assert reducer.duplicate_total == 1
    assert reducer.rejection_reasons == {
        "process_observer_session_retired": 1,
    }


def test_reports_from_other_incarnations_cannot_rearm_cached_report() -> None:
    clock = ManualClock()
    reducer = fixture_reducer(clock, "unit-a")
    target = expectation(
        "unit-a",
        maximum_presence_age_ns=200,
        maximum_report_age_ns=100,
    )
    current = report("unit-a", "epoch-b")
    old = report("unit-a", "epoch-a")
    assert reducer.observe_process(presence("unit-a", "epoch-b"))
    assert reducer.ingest_report(current)

    clock.advance(50)
    assert not reducer.ingest_report(old)
    clock.advance(51)
    assert reducer.observe_process(
        presence("unit-a", "epoch-b", sequence=2)
    )
    assert not reducer.ingest_report(current)
    observed = reducer.evaluate((target,)).observation("unit-a")

    assert observed is not None
    assert observed.derived_state is FixtureDerivedState.LOST
    assert observed.report_age_ns == 101
    assert observed.progress_age_ns == 101
    assert observed.reason_codes == ("source_report_stale",)
    assert reducer.accepted_report_total == 1
    assert reducer.duplicate_total == 1
    assert reducer.rejection_reasons == {
        "source_report_without_matching_process_presence": 1,
    }


def test_off_presence_report_cannot_become_latent_fresh_evidence() -> None:
    clock = ManualClock()
    reducer = fixture_reducer(clock, "unit-a")
    target = expectation("unit-a")
    assert reducer.observe_process(presence("unit-a", "epoch-a"))
    assert reducer.ingest_report(report("unit-a", "epoch-a"))
    assert reducer.evaluate((target,)).observation("unit-a").converged
    assert not reducer.ingest_report(report("unit-a", "epoch-b"))

    clock.advance(1)
    assert reducer.observe_process(
        presence("unit-a", "epoch-b", sequence=2)
    )
    before_fresh_report = reducer.evaluate((target,)).observation("unit-a")
    assert before_fresh_report is not None
    assert before_fresh_report.derived_state is FixtureDerivedState.RECOVERING
    assert before_fresh_report.reason_codes == ("source_report_missing",)

    assert reducer.ingest_report(report("unit-a", "epoch-b"))
    assert reducer.evaluate((target,)).observation("unit-a").converged


def test_epoch_reuse_after_observed_absence_fails_closed() -> None:
    clock = ManualClock()
    reducer = fixture_reducer(clock, "unit-a")
    target = expectation("unit-a")
    assert reducer.observe_process(presence("unit-a", "epoch-a"))
    assert reducer.ingest_report(report("unit-a", "epoch-a"))
    assert reducer.evaluate((target,)).observation("unit-a").converged

    clock.advance(1)
    assert reducer.observe_process(
        FixtureProcessPresence(
            runtime_unit_id="unit-a",
            observer_instance_id="observer-a",
            observation_sequence=2,
            state=FixturePresenceState.ABSENT,
        )
    )
    assert not reducer.observe_process(
        presence("unit-a", "epoch-a", sequence=3)
    )
    assert not reducer.ingest_report(
        report("unit-a", "epoch-a", sequence=2, progress=2)
    )
    absent = reducer.evaluate((target,)).observation("unit-a")
    assert absent is not None
    assert absent.derived_state is FixtureDerivedState.LOST
    assert absent.process_presence is FixturePresenceState.ABSENT

    assert reducer.observe_process(
        presence("unit-a", "epoch-b", sequence=4)
    )
    recovering = reducer.evaluate((target,)).observation("unit-a")
    assert recovering is not None
    assert recovering.derived_state is FixtureDerivedState.RECOVERING
    assert recovering.reason_codes == ("source_report_missing",)
    assert reducer.ingest_report(report("unit-a", "epoch-b"))
    assert reducer.evaluate((target,)).observation("unit-a").converged


def test_unknown_presence_does_not_fabricate_an_incarnation_boundary() -> None:
    clock = ManualClock()
    reducer = fixture_reducer(clock, "unit-a")
    target = expectation("unit-a")
    assert reducer.observe_process(presence("unit-a", "epoch-a"))
    assert reducer.ingest_report(report("unit-a", "epoch-a"))
    assert reducer.evaluate((target,)).observation("unit-a").converged

    clock.advance(1)
    assert reducer.observe_process(
        FixtureProcessPresence(
            runtime_unit_id="unit-a",
            observer_instance_id="observer-a",
            observation_sequence=2,
            state=FixturePresenceState.UNKNOWN,
        )
    )
    unknown = reducer.evaluate((target,)).observation("unit-a")
    assert unknown is not None
    assert unknown.derived_state is FixtureDerivedState.LOST
    assert unknown.reason_codes == ("process_presence_unknown",)

    assert reducer.observe_process(
        presence("unit-a", "epoch-a", sequence=3)
    )
    assert reducer.evaluate((target,)).observation("unit-a").converged


def test_source_reasons_cannot_hide_manager_freshness_failure() -> None:
    clock = ManualClock()
    reducer = fixture_reducer(clock, "unit-a")
    target = expectation(
        "unit-a",
        maximum_presence_age_ns=100,
        maximum_report_age_ns=1,
    )
    source_reasons = tuple(f"source-reason-{index:02}" for index in range(32))
    assert reducer.observe_process(presence("unit-a", "epoch-a"))
    assert reducer.ingest_report(
        replace(
            report("unit-a", "epoch-a"),
            reason_codes=source_reasons,
        )
    )

    clock.advance(2)
    observed = reducer.evaluate((target,)).observation("unit-a")
    assert observed is not None
    assert observed.derived_state is FixtureDerivedState.LOST
    assert observed.reason_codes == ("source_report_stale",)
    assert observed.source_reason_codes == source_reasons


def test_progress_expiry_is_an_explicit_expectation_policy() -> None:
    clock = ManualClock()
    reducer = fixture_reducer(clock, "unit-a")
    no_progress_deadline = expectation(
        "unit-a",
        maximum_presence_age_ns=200,
        maximum_report_age_ns=100,
    )
    assert reducer.observe_process(presence("unit-a", "epoch-a"))
    assert reducer.ingest_report(
        report("unit-a", "epoch-a", sequence=1, progress=1)
    )

    clock.advance(80)
    assert reducer.observe_process(
        presence("unit-a", "epoch-a", sequence=2)
    )
    assert reducer.ingest_report(
        report("unit-a", "epoch-a", sequence=2, progress=1)
    )
    clock.advance(21)
    without_policy = reducer.evaluate(
        (no_progress_deadline,)
    ).observation("unit-a")
    assert without_policy is not None
    assert without_policy.progress_age_ns == 101
    assert without_policy.converged

    with_policy = replace(
        no_progress_deadline,
        maximum_progress_age_ns=100,
    )
    required = reducer.evaluate((with_policy,)).observation("unit-a")
    assert required is not None
    assert required.derived_state is FixtureDerivedState.LOST
    assert required.reason_codes == ("critical_progress_stale",)


def test_ingress_rejects_undeclared_units_and_declared_set_is_bounded() -> None:
    clock = ManualClock()
    reducer = fixture_reducer(clock, "unit-a")

    assert not reducer.observe_process(presence("unit-b", "epoch-b"))
    assert not reducer.ingest_report(report("unit-b", "epoch-b"))
    with pytest.raises(ValueError, match="undeclared"):
        reducer.evaluate((expectation("unit-b"),))
    with pytest.raises(ValueError, match="bounded tuple"):
        RuntimeObservationFixtureReducer(
            clock_ns=clock,
            declared_runtime_unit_ids=tuple(
                f"unit-{index}" for index in range(65)
            ),
        )


def test_observer_and_incarnation_replay_ledgers_are_bounded() -> None:
    clock = ManualClock()
    observer_reducer = fixture_reducer(clock, "unit-a")
    assert observer_reducer.observe_process(
        presence("unit-a", "epoch-a", observer="observer-0")
    )
    for index in range(1, 64):
        assert observer_reducer.replace_process_observer(
            presence(
                "unit-a",
                "epoch-a",
                observer=f"observer-{index}",
            )
        )
    assert not observer_reducer.replace_process_observer(
        presence("unit-a", "epoch-a", observer="observer-64")
    )
    assert observer_reducer.rejection_reasons == {
        "process_observer_session_limit_exceeded": 1,
    }

    incarnation_reducer = fixture_reducer(clock, "unit-a")
    for index in range(64):
        assert incarnation_reducer.observe_process(
            presence(
                "unit-a",
                f"epoch-{index}",
                sequence=index + 1,
            )
        )
        assert incarnation_reducer.ingest_report(
            report("unit-a", f"epoch-{index}")
        )
    assert not incarnation_reducer.observe_process(
        presence("unit-a", "epoch-64", sequence=65)
    )
    assert incarnation_reducer.rejection_reasons == {
        "process_incarnation_limit_exceeded": 1,
    }
    assert tuple(incarnation_reducer._reports["unit-a"]) == ("epoch-63",)


def test_expectation_dependency_graph_is_strict_and_acyclic() -> None:
    clock = ManualClock()
    reducer = fixture_reducer(clock, "unit-a", "unit-b")

    with pytest.raises(ValueError, match="unknown dependencies"):
        reducer.evaluate(
            (expectation("unit-a", dependencies=("missing",)),)
        )
    with pytest.raises(ValueError, match="contain a cycle"):
        reducer.evaluate(
            (
                expectation("unit-a", dependencies=("unit-b",)),
                expectation("unit-b", dependencies=("unit-a",)),
            )
        )


def test_maximum_length_dependency_id_keeps_precise_manager_reason() -> None:
    dependency_id = "d" * 256
    clock = ManualClock()
    reducer = fixture_reducer(clock, dependency_id, "dependant")
    assert reducer.observe_process(
        presence("dependant", "dependant-epoch")
    )
    assert reducer.ingest_report(
        report("dependant", "dependant-epoch")
    )

    observed = reducer.evaluate(
        (
            expectation("dependant", dependencies=(dependency_id,)),
            expectation(dependency_id),
        )
    ).observation("dependant")

    assert observed is not None
    assert observed.derived_state is FixtureDerivedState.NONCONVERGED
    assert observed.reason_codes == (
        f"runtime_dependency_not_converged:{dependency_id}",
    )


def test_source_reason_and_role_sets_are_bounded() -> None:
    with pytest.raises(ValueError, match="reason_codes"):
        replace(
            report("unit-a", "epoch-a"),
            reason_codes=tuple(f"reason-{index}" for index in range(33)),
        )
    with pytest.raises(ValueError, match="unique"):
        replace(
            report("unit-a", "epoch-a"),
            ready_runtime_role_ids=("role-a", "role-a"),
        )


def binding(
    runtime_unit_id: str,
    *,
    boundary: str,
    suffix: str,
    lifecycle_control: FixtureLifecycleControl = (
        FixtureLifecycleControl.MANAGER_MANAGED
    ),
) -> FixtureRuntimeProcessBinding:
    return FixtureRuntimeProcessBinding(
        runtime_unit_id=runtime_unit_id,
        process_boundary_id=boundary,
        lifecycle_control=lifecycle_control,
        process_observation_handle_id=f"observation-{suffix}",
        status_source_id=f"status-{suffix}",
        progress_source_id=f"progress-{suffix}",
        incarnation_source_id=f"incarnation-{suffix}",
        managed_lifecycle_handle_id=(
            f"lifecycle-{suffix}"
            if lifecycle_control
            is FixtureLifecycleControl.MANAGER_MANAGED
            else ""
        ),
    )


def test_colocated_units_require_independent_failure_handles() -> None:
    validate_fixture_process_bindings(
        (
            binding("unit-a", boundary="process-shared", suffix="a"),
            binding("unit-b", boundary="process-shared", suffix="b"),
        )
    )

    inseparable = binding(
        "unit-b",
        boundary="process-shared",
        suffix="a",
    )
    with pytest.raises(ValueError, match="lack independent"):
        validate_fixture_process_bindings(
            (
                binding("unit-a", boundary="process-shared", suffix="a"),
                inseparable,
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
            validate_fixture_process_bindings((first, aliased))


def test_external_binding_exposes_no_manager_lifecycle_handle() -> None:
    external = binding(
        "external",
        boundary="external-process",
        suffix="external",
        lifecycle_control=FixtureLifecycleControl.EXTERNALLY_MANAGED,
    )
    assert external.managed_lifecycle_handle_id == ""

    with pytest.raises(ValueError, match="cannot expose"):
        replace(external, managed_lifecycle_handle_id="forbidden-handle")
