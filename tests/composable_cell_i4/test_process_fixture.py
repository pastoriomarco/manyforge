from __future__ import annotations

from verification.composable_cell_i4.process_fixture import (
    ExternalProcessFixture,
    FixtureProcessOperation,
    FixtureProcessResult,
    ManagedProcessFixture,
    ObservationOnlyProcessFixture,
)
from verification.composable_cell_i4.runtime_observation_fixture import (
    FixtureDerivedState,
    FixtureLifecycleControl,
    FixturePresenceState,
    FixtureRuntimeExpectation,
    RuntimeObservationFixtureReducer,
)


class ManualClock:
    def __init__(self, now_ns: int = 1) -> None:
        self.now_ns = now_ns

    def __call__(self) -> int:
        return self.now_ns

    def advance(self, amount_ns: int) -> int:
        self.now_ns += amount_ns
        return self.now_ns


def external_expectation(runtime_unit_id: str) -> FixtureRuntimeExpectation:
    return FixtureRuntimeExpectation(
        runtime_unit_id=runtime_unit_id,
        lifecycle_control=FixtureLifecycleControl.EXTERNALLY_MANAGED,
        product_identity="fixture-product-v1",
        configuration_identity="fixture-config-v1",
        maximum_presence_age_ns=100,
        maximum_report_age_ns=100,
        maximum_progress_age_ns=100,
        required_runtime_role_ids=("fixture.runtime-role",),
    )


def test_observer_replacement_does_not_mint_external_epoch_or_health() -> None:
    with ExternalProcessFixture(("external-unit",)) as external:
        process_epoch = external.start("external-unit")
        first_observer = ObservationOnlyProcessFixture(external)
        first = first_observer.observe("external-unit")
        replacement_observer = ObservationOnlyProcessFixture(external)
        replacement = replacement_observer.observe("external-unit")

        assert first.observer_instance_id != replacement.observer_instance_id
        assert first.observation_sequence == replacement.observation_sequence == 1
        assert first.process_incarnation == replacement.process_incarnation
        assert replacement.process_incarnation == process_epoch

        clock = ManualClock()
        reducer = RuntimeObservationFixtureReducer(
            clock_ns=clock,
            declared_runtime_unit_ids=("external-unit",),
        )
        assert reducer.observe_process(first)
        clock.advance(1)
        assert reducer.replace_process_observer(replacement)
        observed = reducer.evaluate(
            (external_expectation("external-unit"),)
        ).observation("external-unit")

        assert observed is not None
        assert observed.runtime_epoch == process_epoch
        assert observed.derived_state is FixtureDerivedState.NONCONVERGED
        assert observed.reason_codes == ("source_report_missing",)


def test_external_process_restart_is_the_event_that_changes_incarnation() -> None:
    with ExternalProcessFixture(("external-unit",)) as external:
        first_epoch = external.start("external-unit")
        observer = ObservationOnlyProcessFixture(external)
        first = observer.observe("external-unit")

        second_epoch = external.restart("external-unit")
        second = observer.observe("external-unit")

        assert first.state is FixturePresenceState.PRESENT
        assert second.state is FixturePresenceState.PRESENT
        assert second.observation_sequence == 2
        assert first_epoch != second_epoch
        assert first.process_incarnation == first_epoch
        assert second.process_incarnation == second_epoch


def test_observation_only_adapter_has_no_process_or_robot_mutation_surface() -> None:
    with ExternalProcessFixture(("external-unit",)) as external:
        observer = ObservationOnlyProcessFixture(external)

        for forbidden in (
            "start",
            "stop",
            "restart",
            "enable",
            "disable",
            "move",
            "recover",
            "resume",
            "reset",
            "stop_motion",
        ):
            assert not hasattr(observer, forbidden)


def test_managed_process_effects_are_separate_and_restart_mints_fresh_epoch() -> None:
    with ManagedProcessFixture(("managed-unit",)) as managed:
        started = managed.start("managed-unit")
        observed = managed.observe("managed-unit")
        restarted = managed.restart("managed-unit")
        stopped = managed.stop("managed-unit")

        assert started.state is FixturePresenceState.PRESENT
        assert observed.process_incarnation == started.process_incarnation
        assert restarted.state is FixturePresenceState.PRESENT
        assert restarted.process_incarnation != started.process_incarnation
        assert stopped.state is FixturePresenceState.ABSENT
        assert [
            (request.operation, request.runtime_unit_id)
            for request in managed.requests
        ] == [
            (FixtureProcessOperation.START, "managed-unit"),
            (FixtureProcessOperation.RESTART, "managed-unit"),
            (FixtureProcessOperation.STOP, "managed-unit"),
        ]
        assert [
            reconciliation.result
            for reconciliation in managed.reconciliations
        ] == [
            FixtureProcessResult.CHANGED,
            FixtureProcessResult.CHANGED,
            FixtureProcessResult.CHANGED,
        ]


def test_managed_fixture_has_no_robot_lifecycle_or_motion_surface() -> None:
    with ManagedProcessFixture(("managed-unit",)) as managed:
        for forbidden in (
            "enable",
            "disable",
            "move",
            "recover_robot",
            "resume",
            "reset",
            "stop_motion",
            "switch_controller",
        ):
            assert not hasattr(managed, forbidden)


def test_external_absence_is_observed_without_an_effect() -> None:
    with ExternalProcessFixture(("missing-unit",)) as external:
        observer = ObservationOnlyProcessFixture(external)
        sample = observer.observe("missing-unit")

        assert sample.state is FixturePresenceState.ABSENT
        assert sample.process_incarnation == ""
        assert sample.observation_sequence == 1


def test_managed_requests_are_distinct_from_reconciled_effects() -> None:
    with ManagedProcessFixture(("managed-unit",)) as managed:
        first = managed.start("managed-unit")
        duplicate_start = managed.start("managed-unit")
        first_stop = managed.stop("managed-unit")
        duplicate_stop = managed.stop("managed-unit")

        assert first.process_incarnation == duplicate_start.process_incarnation
        assert first_stop.state is FixturePresenceState.ABSENT
        assert duplicate_stop.state is FixturePresenceState.ABSENT
        assert len(managed.requests) == 4
        assert [
            item.result for item in managed.reconciliations
        ] == [
            FixtureProcessResult.CHANGED,
            FixtureProcessResult.ALREADY_SATISFIED,
            FixtureProcessResult.CHANGED,
            FixtureProcessResult.ALREADY_SATISFIED,
        ]
        assert not hasattr(managed, "effects")


def test_process_fixtures_reject_undeclared_runtime_units() -> None:
    with ExternalProcessFixture(("declared",)) as external:
        observer = ObservationOnlyProcessFixture(external)
        for operation in (
            lambda: external.start("undeclared"),
            lambda: external.stop("undeclared"),
            lambda: observer.observe("undeclared"),
        ):
            try:
                operation()
            except ValueError as exc:
                assert "not declared" in str(exc)
            else:
                raise AssertionError("undeclared runtime unit was accepted")

    with ManagedProcessFixture(("declared",)) as managed:
        for operation in (
            lambda: managed.start("undeclared"),
            lambda: managed.observe("undeclared"),
        ):
            try:
                operation()
            except ValueError as exc:
                assert "not declared" in str(exc)
            else:
                raise AssertionError("undeclared runtime unit was accepted")


def test_managed_request_and_reconciliation_history_is_bounded() -> None:
    with ManagedProcessFixture(("managed-unit",)) as managed:
        for _ in range(256):
            managed.start("managed-unit")

        try:
            managed.start("managed-unit")
        except RuntimeError as exc:
            assert "request limit" in str(exc)
        else:
            raise AssertionError("unbounded process request was accepted")
        assert len(managed.requests) == 256
        assert len(managed.reconciliations) == 256
