"""I3-independent runtime-observation evidence for composable-cell I4.

This module is deliberately an unstable verification fixture.  It exercises
the ownership and failure semantics that I4 can prove before I3 freezes the
exact per-runtime-unit product identity and final progress-report wire shape.
It must not be imported by production runtime code.

The fixture keeps three facts separate:

* a process observer reports presence and independently observed incarnation;
* the runtime source reports its own local state and meaningful progress; and
* the Manager attaches receiver-local freshness, dependency epochs, and the
  derived convergence/loss/recovery state.

No value here is a robot, controller, scene, Kernel, or Apply authority.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import StrEnum
from types import MappingProxyType
from typing import Callable, Mapping, Sequence

_MAX_IDENTIFIER_LENGTH = 256
_MAX_RUNTIME_UNITS = 64
_MAX_DEPENDENCIES = 32
_MAX_RUNTIME_ROLES = 256
_MAX_SOURCE_REASON_CODES = 32
_MAX_MANAGER_REASON_CODES = 64
_MAX_OBSERVER_SESSIONS_PER_RUNTIME_UNIT = 64
_MAX_INCARNATIONS_PER_RUNTIME_UNIT = 64


class FixtureLifecycleControl(StrEnum):
    """The already-frozen process-lifecycle split, only for fixture use."""

    MANAGER_MANAGED = "manager_managed"
    EXTERNALLY_MANAGED = "externally_managed"


class FixturePresenceState(StrEnum):
    UNKNOWN = "unknown"
    ABSENT = "absent"
    PRESENT = "present"


class FixtureSourcePhase(StrEnum):
    """Source-local phase; notably excludes Manager-derived loss/recovery."""

    STARTING = "starting"
    SERVING = "serving"
    STOPPING = "stopping"
    STOPPED = "stopped"
    FAULTED = "faulted"


class FixtureSourceHealth(StrEnum):
    """A source's local health claim, not aggregate convergence."""

    HEALTHY = "healthy"
    DEGRADED = "degraded"
    FAULTED = "faulted"


class FixtureCommandSurfaceState(StrEnum):
    """State of ManyForge-managed command surfaces only."""

    CLOSED_NO_ACTIVE_EFFECT = "closed_no_active_effect"
    OPEN = "open"
    UNAVAILABLE = "unavailable"


class FixtureDerivedState(StrEnum):
    """Manager-derived state. Sources never publish these values."""

    CONVERGED = "converged"
    NONCONVERGED = "nonconverged"
    LOST = "lost"
    RECOVERING = "recovering"


def _identifier(value: object, label: str, *, optional: bool = False) -> str:
    if optional and value in (None, ""):
        return ""
    if (
        not isinstance(value, str)
        or not value
        or len(value) > _MAX_IDENTIFIER_LENGTH
        or any(ord(character) < 0x21 or ord(character) > 0x7E for character in value)
    ):
        raise ValueError(f"{label} must be a bounded printable identifier")
    return value


def _positive_integer(value: object, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise ValueError(f"{label} must be a positive integer")
    return value


def _nonnegative_integer(value: object, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ValueError(f"{label} must be a non-negative integer")
    return value


def _identifier_tuple(
    values: object,
    label: str,
    *,
    maximum: int,
) -> tuple[str, ...]:
    if not isinstance(values, tuple) or len(values) > maximum:
        raise ValueError(f"{label} must be a bounded tuple")
    normalized = tuple(_identifier(item, label) for item in values)
    if len(set(normalized)) != len(normalized):
        raise ValueError(f"{label} must contain unique values")
    return tuple(sorted(normalized))


def fixture_runtime_unit_ids(values: object) -> tuple[str, ...]:
    """Validate one immutable, bounded set of fixture runtime-unit IDs."""

    return _identifier_tuple(
        values,
        "declared_runtime_unit_ids",
        maximum=_MAX_RUNTIME_UNITS,
    )


def _frozen_mapping(values: Mapping[str, str]) -> Mapping[str, str]:
    return MappingProxyType(dict(sorted(values.items())))


@dataclass(frozen=True, slots=True)
class FixtureRuntimeExpectation:
    """One desired runtime unit using opaque, fixture-only product evidence."""

    runtime_unit_id: str
    lifecycle_control: FixtureLifecycleControl
    product_identity: str
    configuration_identity: str
    maximum_presence_age_ns: int
    maximum_report_age_ns: int
    maximum_progress_age_ns: int | None = None
    dependency_runtime_unit_ids: tuple[str, ...] = ()
    required_runtime_role_ids: tuple[str, ...] = ()
    required_command_surface_state: FixtureCommandSurfaceState = (
        FixtureCommandSurfaceState.CLOSED_NO_ACTIVE_EFFECT
    )

    def __post_init__(self) -> None:
        _identifier(self.runtime_unit_id, "runtime_unit_id")
        if not isinstance(self.lifecycle_control, FixtureLifecycleControl):
            raise ValueError("lifecycle_control is invalid")
        _identifier(self.product_identity, "fixture product_identity")
        _identifier(
            self.configuration_identity,
            "fixture configuration_identity",
        )
        for label, value in (
            ("maximum_presence_age_ns", self.maximum_presence_age_ns),
            ("maximum_report_age_ns", self.maximum_report_age_ns),
        ):
            _positive_integer(value, label)
        if self.maximum_progress_age_ns is not None:
            _positive_integer(
                self.maximum_progress_age_ns,
                "maximum_progress_age_ns",
            )
        dependencies = _identifier_tuple(
            self.dependency_runtime_unit_ids,
            "dependency_runtime_unit_ids",
            maximum=_MAX_DEPENDENCIES,
        )
        if self.runtime_unit_id in dependencies:
            raise ValueError("a runtime unit cannot depend on itself")
        object.__setattr__(self, "dependency_runtime_unit_ids", dependencies)
        object.__setattr__(
            self,
            "required_runtime_role_ids",
            _identifier_tuple(
                self.required_runtime_role_ids,
                "required_runtime_role_ids",
                maximum=_MAX_RUNTIME_ROLES,
            ),
        )
        if not isinstance(
            self.required_command_surface_state,
            FixtureCommandSurfaceState,
        ):
            raise ValueError("required_command_surface_state is invalid")


@dataclass(frozen=True, slots=True)
class FixtureProcessPresence:
    """One process-observer sample, separate from source health/progress."""

    runtime_unit_id: str
    observer_instance_id: str
    observation_sequence: int
    state: FixturePresenceState
    process_incarnation: str = ""

    def __post_init__(self) -> None:
        _identifier(self.runtime_unit_id, "runtime_unit_id")
        _identifier(self.observer_instance_id, "observer_instance_id")
        _positive_integer(self.observation_sequence, "observation_sequence")
        if not isinstance(self.state, FixturePresenceState):
            raise ValueError("presence state is invalid")
        if self.state is FixturePresenceState.PRESENT:
            _identifier(self.process_incarnation, "process_incarnation")
        elif self.process_incarnation:
            raise ValueError(
                "non-present process evidence cannot carry an incarnation"
            )


@dataclass(frozen=True, slots=True)
class FixtureSourceReport:
    """Source-attested local report with no Manager-owned fields."""

    runtime_unit_id: str
    process_incarnation: str
    source_report_sequence: int
    critical_progress_sequence: int
    local_phase: FixtureSourcePhase
    source_health: FixtureSourceHealth
    product_identity: str
    configuration_identity: str
    ready_runtime_role_ids: tuple[str, ...]
    command_surface_state: FixtureCommandSurfaceState
    reason_codes: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        for label, value in (
            ("runtime_unit_id", self.runtime_unit_id),
            ("process_incarnation", self.process_incarnation),
            ("fixture product_identity", self.product_identity),
            ("fixture configuration_identity", self.configuration_identity),
        ):
            _identifier(value, label)
        _positive_integer(self.source_report_sequence, "source_report_sequence")
        _nonnegative_integer(
            self.critical_progress_sequence,
            "critical_progress_sequence",
        )
        if not isinstance(self.local_phase, FixtureSourcePhase):
            raise ValueError("local_phase is invalid")
        if not isinstance(self.source_health, FixtureSourceHealth):
            raise ValueError("source_health is invalid")
        if not isinstance(
            self.command_surface_state,
            FixtureCommandSurfaceState,
        ):
            raise ValueError("command_surface_state is invalid")
        object.__setattr__(
            self,
            "ready_runtime_role_ids",
            _identifier_tuple(
                self.ready_runtime_role_ids,
                "ready_runtime_role_ids",
                maximum=_MAX_RUNTIME_ROLES,
            ),
        )
        object.__setattr__(
            self,
            "reason_codes",
            _identifier_tuple(
                self.reason_codes,
                "reason_codes",
                maximum=_MAX_SOURCE_REASON_CODES,
            ),
        )


@dataclass(frozen=True, slots=True)
class FixtureRuntimeObservation:
    """Immutable observation with Manager and source reasons kept separate."""

    runtime_unit_id: str
    lifecycle_control: FixtureLifecycleControl
    derived_state: FixtureDerivedState
    process_presence: FixturePresenceState
    runtime_epoch: str
    presence_age_ns: int | None
    report_age_ns: int | None
    progress_age_ns: int | None
    observed_product_identity: str
    observed_configuration_identity: str
    source_phase: FixtureSourcePhase | None
    source_health: FixtureSourceHealth | None
    ready_runtime_role_ids: tuple[str, ...]
    command_surface_state: FixtureCommandSurfaceState | None
    dependency_runtime_epochs: Mapping[str, str]
    source_reason_codes: tuple[str, ...]
    reason_codes: tuple[str, ...]

    @property
    def converged(self) -> bool:
        return self.derived_state is FixtureDerivedState.CONVERGED


@dataclass(frozen=True, slots=True)
class FixtureObservationSnapshot:
    evaluated_at_ns: int
    observations: tuple[FixtureRuntimeObservation, ...]

    def observation(
        self,
        runtime_unit_id: str,
    ) -> FixtureRuntimeObservation | None:
        return next(
            (
                item
                for item in self.observations
                if item.runtime_unit_id == runtime_unit_id
            ),
            None,
        )


@dataclass(frozen=True, slots=True)
class FixtureRuntimeProcessBinding:
    """Fixture evidence for a runtime-unit/failure-boundary mapping."""

    runtime_unit_id: str
    process_boundary_id: str
    lifecycle_control: FixtureLifecycleControl
    process_observation_handle_id: str
    status_source_id: str
    progress_source_id: str
    incarnation_source_id: str
    managed_lifecycle_handle_id: str = ""

    def __post_init__(self) -> None:
        for label, value in (
            ("runtime_unit_id", self.runtime_unit_id),
            ("process_boundary_id", self.process_boundary_id),
            (
                "process_observation_handle_id",
                self.process_observation_handle_id,
            ),
            ("status_source_id", self.status_source_id),
            ("progress_source_id", self.progress_source_id),
            ("incarnation_source_id", self.incarnation_source_id),
        ):
            _identifier(value, label)
        if not isinstance(self.lifecycle_control, FixtureLifecycleControl):
            raise ValueError("lifecycle_control is invalid")
        if self.lifecycle_control is FixtureLifecycleControl.MANAGER_MANAGED:
            _identifier(
                self.managed_lifecycle_handle_id,
                "managed_lifecycle_handle_id",
            )
        elif self.managed_lifecycle_handle_id:
            raise ValueError(
                "externally managed bindings cannot expose a Manager "
                "lifecycle handle"
            )


def validate_fixture_process_bindings(
    bindings: Sequence[FixtureRuntimeProcessBinding],
) -> None:
    """Reject fake independent units that share an inseparable process handle."""

    if not isinstance(bindings, Sequence) or len(bindings) > _MAX_RUNTIME_UNITS:
        raise ValueError("process bindings must be a bounded sequence")
    runtime_ids = tuple(item.runtime_unit_id for item in bindings)
    if len(set(runtime_ids)) != len(runtime_ids):
        raise ValueError("runtime-unit process bindings must be unique")

    handle_owners: dict[str, str] = {}
    for binding in bindings:
        for attribute in (
            "process_observation_handle_id",
            "status_source_id",
            "progress_source_id",
            "incarnation_source_id",
            "managed_lifecycle_handle_id",
        ):
            handle_id = getattr(binding, attribute)
            if not handle_id:
                continue
            existing_owner = handle_owners.get(handle_id)
            if (
                existing_owner is not None
                and existing_owner != binding.runtime_unit_id
            ):
                raise ValueError(
                    "runtime-unit process bindings lack independent "
                    f"concrete handles: {handle_id}"
                )
            handle_owners[handle_id] = binding.runtime_unit_id

    by_boundary: dict[str, list[FixtureRuntimeProcessBinding]] = {}
    for binding in bindings:
        if not isinstance(binding, FixtureRuntimeProcessBinding):
            raise TypeError("process binding has an invalid type")
        by_boundary.setdefault(binding.process_boundary_id, []).append(binding)
    for boundary_id, grouped in by_boundary.items():
        if len(grouped) < 2:
            continue
        for attribute in (
            "process_observation_handle_id",
            "status_source_id",
            "progress_source_id",
            "incarnation_source_id",
        ):
            values = tuple(getattr(item, attribute) for item in grouped)
            if len(set(values)) != len(values):
                raise ValueError(
                    "co-located runtime units lack independent "
                    f"{attribute}: {boundary_id}"
                )
        managed_handles = tuple(
            item.managed_lifecycle_handle_id
            for item in grouped
            if item.lifecycle_control
            is FixtureLifecycleControl.MANAGER_MANAGED
        )
        if len(set(managed_handles)) != len(managed_handles):
            raise ValueError(
                "co-located managed runtime units lack independent "
                f"lifecycle handles: {boundary_id}"
            )


@dataclass(slots=True)
class _AcceptedPresence:
    sample: FixtureProcessPresence
    received_at_ns: int
    accepted_event_generation: int


@dataclass(slots=True)
class _AcceptedReport:
    sample: FixtureSourceReport
    report_received_at_ns: int
    progress_received_at_ns: int
    accepted_event_generation: int


@dataclass(slots=True)
class _DependencyJoin:
    report_identity: tuple[object, ...]
    runtime_epochs: Mapping[str, str] = field(default_factory=dict)


class RuntimeObservationFixtureReducer:
    """Bounded receiver-local reducer used only by the I4 fixture."""

    def __init__(
        self,
        *,
        clock_ns: Callable[[], int],
        declared_runtime_unit_ids: tuple[str, ...],
    ) -> None:
        if not callable(clock_ns):
            raise TypeError("clock_ns must be callable")
        self._clock_ns = clock_ns
        declared = fixture_runtime_unit_ids(declared_runtime_unit_ids)
        self._declared_runtime_unit_ids = frozenset(declared)
        self._presence: dict[str, _AcceptedPresence] = {}
        self._active_observer_ids: dict[str, str] = {}
        self._observer_ledgers: dict[
            str,
            dict[str, _AcceptedPresence],
        ] = {runtime_unit_id: {} for runtime_unit_id in declared}
        self._retired_observer_ids: dict[str, set[str]] = {
            runtime_unit_id: set() for runtime_unit_id in declared
        }
        self._known_incarnations: dict[str, set[str]] = {
            runtime_unit_id: set() for runtime_unit_id in declared
        }
        self._retired_incarnations: dict[str, set[str]] = {
            runtime_unit_id: set() for runtime_unit_id in declared
        }
        self._last_present_incarnation: dict[str, str] = {}
        self._epoch_change_event_generations: dict[str, int] = {}
        self._reports: dict[str, dict[str, _AcceptedReport]] = {
            runtime_unit_id: {} for runtime_unit_id in declared
        }
        self._dependency_joins: dict[str, _DependencyJoin] = {}
        self._last_converged_epoch: dict[str, str] = {}
        self.accepted_presence_total = 0
        self.accepted_report_total = 0
        self.duplicate_total = 0
        self.rejected_total = 0
        self.rejection_reasons: dict[str, int] = {}
        self._accepted_event_generation = 0

    def _reject(self, reason: str) -> bool:
        self.rejected_total += 1
        self.rejection_reasons[reason] = (
            self.rejection_reasons.get(reason, 0) + 1
        )
        return False

    def observe_process(
        self,
        sample: FixtureProcessPresence,
        *,
        received_at_ns: int | None = None,
    ) -> bool:
        if not isinstance(sample, FixtureProcessPresence):
            return self._reject("process_presence_invalid")
        if sample.runtime_unit_id not in self._declared_runtime_unit_ids:
            return self._reject("process_presence_runtime_unit_undeclared")
        active_observer_id = self._active_observer_ids.get(
            sample.runtime_unit_id
        )
        if (
            active_observer_id is not None
            and sample.observer_instance_id != active_observer_id
        ):
            if (
                sample.observer_instance_id
                in self._retired_observer_ids[sample.runtime_unit_id]
            ):
                return self._reject("process_observer_session_retired")
            return self._reject("process_observer_session_not_active")
        now_ns = self._receipt_time(received_at_ns)
        return self._accept_process_presence(
            sample,
            received_at_ns=now_ns,
            replacing_observer=False,
        )

    def replace_process_observer(
        self,
        sample: FixtureProcessPresence,
        *,
        received_at_ns: int | None = None,
    ) -> bool:
        """Explicitly fence one observer session before accepting its replacement."""

        if not isinstance(sample, FixtureProcessPresence):
            return self._reject("process_presence_invalid")
        runtime_unit_id = sample.runtime_unit_id
        if runtime_unit_id not in self._declared_runtime_unit_ids:
            return self._reject("process_presence_runtime_unit_undeclared")
        active_observer_id = self._active_observer_ids.get(runtime_unit_id)
        if (
            active_observer_id is None
            or active_observer_id == sample.observer_instance_id
        ):
            return self.observe_process(
                sample,
                received_at_ns=received_at_ns,
            )
        if (
            sample.observer_instance_id
            in self._retired_observer_ids[runtime_unit_id]
            or sample.observer_instance_id
            in self._observer_ledgers[runtime_unit_id]
        ):
            return self._reject("process_observer_session_reused")
        if (
            len(self._observer_ledgers[runtime_unit_id])
            >= _MAX_OBSERVER_SESSIONS_PER_RUNTIME_UNIT
        ):
            return self._reject("process_observer_session_limit_exceeded")
        now_ns = self._receipt_time(received_at_ns)
        return self._accept_process_presence(
            sample,
            received_at_ns=now_ns,
            replacing_observer=True,
        )

    def _accept_process_presence(
        self,
        sample: FixtureProcessPresence,
        *,
        received_at_ns: int,
        replacing_observer: bool,
    ) -> bool:
        runtime_unit_id = sample.runtime_unit_id
        observer_ledger = self._observer_ledgers[runtime_unit_id]
        current_for_observer = observer_ledger.get(sample.observer_instance_id)
        if current_for_observer is not None:
            if (
                sample.observation_sequence
                < current_for_observer.sample.observation_sequence
            ):
                return self._reject("process_observation_sequence_regressed")
            if (
                sample.observation_sequence
                == current_for_observer.sample.observation_sequence
            ):
                if sample == current_for_observer.sample:
                    self.duplicate_total += 1
                    return False
                return self._reject("process_observation_sequence_conflict")

        current_epoch = self._last_present_incarnation.get(
            runtime_unit_id,
            "",
        )
        candidate_epoch = (
            sample.process_incarnation
            if sample.state is FixturePresenceState.PRESENT
            else ""
        )
        if (
            candidate_epoch
            and candidate_epoch in self._retired_incarnations[runtime_unit_id]
            and candidate_epoch != current_epoch
        ):
            return self._reject("process_incarnation_reused")
        if sample.state is FixturePresenceState.PRESENT:
            if not self._register_incarnation(
                runtime_unit_id,
                candidate_epoch,
            ):
                return False
        accepted_event_generation = self._next_accepted_event_generation()
        if sample.state is FixturePresenceState.PRESENT:
            if current_epoch and candidate_epoch != current_epoch:
                self._retired_incarnations[runtime_unit_id].add(current_epoch)
                self._reports[runtime_unit_id].pop(current_epoch, None)
            if candidate_epoch != current_epoch:
                self._epoch_change_event_generations[runtime_unit_id] = (
                    accepted_event_generation
                )
            self._last_present_incarnation[runtime_unit_id] = candidate_epoch
        elif sample.state is FixturePresenceState.ABSENT and current_epoch:
            self._retired_incarnations[runtime_unit_id].add(current_epoch)
            self._reports[runtime_unit_id].pop(current_epoch, None)
            self._last_present_incarnation.pop(runtime_unit_id, None)

        accepted = _AcceptedPresence(
            sample=sample,
            received_at_ns=received_at_ns,
            accepted_event_generation=accepted_event_generation,
        )
        if replacing_observer:
            previous_observer_id = self._active_observer_ids[runtime_unit_id]
            self._retired_observer_ids[runtime_unit_id].add(
                previous_observer_id
            )
        observer_ledger[sample.observer_instance_id] = accepted
        self._active_observer_ids[runtime_unit_id] = sample.observer_instance_id
        self._presence[runtime_unit_id] = accepted
        self.accepted_presence_total += 1
        return True

    def ingest_report(
        self,
        report: FixtureSourceReport,
        *,
        received_at_ns: int | None = None,
    ) -> bool:
        if not isinstance(report, FixtureSourceReport):
            return self._reject("source_report_invalid")
        runtime_unit_id = report.runtime_unit_id
        if runtime_unit_id not in self._declared_runtime_unit_ids:
            return self._reject("source_report_runtime_unit_undeclared")
        presence = self._presence.get(runtime_unit_id)
        if (
            presence is None
            or presence.sample.state is not FixturePresenceState.PRESENT
            or presence.sample.process_incarnation
            != report.process_incarnation
        ):
            return self._reject(
                "source_report_without_matching_process_presence"
            )
        if (
            report.process_incarnation
            in self._retired_incarnations[runtime_unit_id]
        ):
            return self._reject("source_report_retired_incarnation")
        if not self._register_incarnation(
            runtime_unit_id,
            report.process_incarnation,
        ):
            return False
        now_ns = self._receipt_time(received_at_ns)
        reports_by_incarnation = self._reports[runtime_unit_id]
        current = reports_by_incarnation.get(report.process_incarnation)
        if current is not None:
            if report.source_report_sequence < current.sample.source_report_sequence:
                return self._reject("source_report_sequence_regressed")
            if report.source_report_sequence == current.sample.source_report_sequence:
                if report == current.sample:
                    self.duplicate_total += 1
                    return False
                return self._reject("source_report_sequence_conflict")
            if (
                report.critical_progress_sequence
                < current.sample.critical_progress_sequence
            ):
                return self._reject("critical_progress_sequence_regressed")
            progress_received_at_ns = (
                now_ns
                if report.critical_progress_sequence
                > current.sample.critical_progress_sequence
                else current.progress_received_at_ns
            )
        else:
            progress_received_at_ns = now_ns
        accepted_event_generation = self._next_accepted_event_generation()
        reports_by_incarnation[report.process_incarnation] = _AcceptedReport(
            sample=report,
            report_received_at_ns=now_ns,
            progress_received_at_ns=progress_received_at_ns,
            accepted_event_generation=accepted_event_generation,
        )
        self.accepted_report_total += 1
        return True

    def _register_incarnation(
        self,
        runtime_unit_id: str,
        process_incarnation: str,
    ) -> bool:
        known = self._known_incarnations[runtime_unit_id]
        if process_incarnation in known:
            return True
        if len(known) >= _MAX_INCARNATIONS_PER_RUNTIME_UNIT:
            return self._reject("process_incarnation_limit_exceeded")
        known.add(process_incarnation)
        return True

    def evaluate(
        self,
        expectations: Sequence[FixtureRuntimeExpectation],
        *,
        now_ns: int | None = None,
    ) -> FixtureObservationSnapshot:
        evaluated_at_ns = self._evaluation_time(now_ns)
        ordered_expectations = self._validate_and_order_expectations(
            expectations,
            declared_runtime_unit_ids=self._declared_runtime_unit_ids,
        )
        derived: dict[str, FixtureRuntimeObservation] = {}
        for expectation in ordered_expectations:
            derived[expectation.runtime_unit_id] = self._evaluate_one(
                expectation,
                derived=derived,
                now_ns=evaluated_at_ns,
            )
        observations = tuple(
            derived[expectation.runtime_unit_id]
            for expectation in sorted(
                ordered_expectations,
                key=lambda item: item.runtime_unit_id,
            )
        )
        return FixtureObservationSnapshot(
            evaluated_at_ns=evaluated_at_ns,
            observations=observations,
        )

    def _evaluate_one(
        self,
        expectation: FixtureRuntimeExpectation,
        *,
        derived: Mapping[str, FixtureRuntimeObservation],
        now_ns: int,
    ) -> FixtureRuntimeObservation:
        runtime_unit_id = expectation.runtime_unit_id
        presence = self._presence.get(runtime_unit_id)
        reasons: set[str] = set()
        dependency_epochs: dict[str, str] = {}
        presence_age_ns: int | None = None
        report_age_ns: int | None = None
        progress_age_ns: int | None = None
        runtime_epoch = ""

        if presence is None:
            return self._observation(
                expectation,
                state=FixtureDerivedState.LOST,
                process_presence=FixturePresenceState.UNKNOWN,
                reasons={"process_presence_missing"},
            )

        presence_age_ns, presence_time_valid = self._age(
            now_ns,
            presence.received_at_ns,
        )
        if not presence_time_valid:
            reasons.add("process_presence_receipt_in_future")
        if presence_age_ns > expectation.maximum_presence_age_ns:
            reasons.add("process_presence_stale")
        process_presence = presence.sample.state
        if process_presence is FixturePresenceState.UNKNOWN:
            reasons.add("process_presence_unknown")
        elif process_presence is FixturePresenceState.ABSENT:
            reasons.add("process_absent")
        else:
            runtime_epoch = presence.sample.process_incarnation

        if reasons:
            return self._observation(
                expectation,
                state=FixtureDerivedState.LOST,
                process_presence=process_presence,
                runtime_epoch=runtime_epoch,
                presence_age_ns=presence_age_ns,
                reasons=reasons,
            )

        report_receipt = self._reports[runtime_unit_id].get(runtime_epoch)
        if report_receipt is None:
            return self._observation(
                expectation,
                state=(
                    FixtureDerivedState.RECOVERING
                    if runtime_unit_id in self._last_converged_epoch
                    else FixtureDerivedState.NONCONVERGED
                ),
                process_presence=process_presence,
                runtime_epoch=runtime_epoch,
                presence_age_ns=presence_age_ns,
                reasons={"source_report_missing"},
            )

        report = report_receipt.sample
        report_age_ns, report_time_valid = self._age(
            now_ns,
            report_receipt.report_received_at_ns,
        )
        progress_age_ns, progress_time_valid = self._age(
            now_ns,
            report_receipt.progress_received_at_ns,
        )
        if not report_time_valid:
            reasons.add("source_report_receipt_in_future")
        if not progress_time_valid:
            reasons.add("critical_progress_receipt_in_future")
        if report_age_ns > expectation.maximum_report_age_ns:
            reasons.add("source_report_stale")
        if (
            expectation.maximum_progress_age_ns is not None
            and progress_age_ns > expectation.maximum_progress_age_ns
        ):
            reasons.add("critical_progress_stale")
        if reasons:
            return self._observation(
                expectation,
                state=FixtureDerivedState.LOST,
                process_presence=process_presence,
                runtime_epoch=runtime_epoch,
                presence_age_ns=presence_age_ns,
                report_age_ns=report_age_ns,
                progress_age_ns=progress_age_ns,
                report=report,
                reasons=reasons,
            )

        if report.product_identity != expectation.product_identity:
            reasons.add("runtime_product_identity_mismatch")
        if (
            report.configuration_identity
            != expectation.configuration_identity
        ):
            reasons.add("runtime_configuration_identity_mismatch")
        if report.local_phase is not FixtureSourcePhase.SERVING:
            reasons.add(f"runtime_source_phase_{report.local_phase.value}")
        if report.source_health is not FixtureSourceHealth.HEALTHY:
            reasons.add(f"runtime_source_health_{report.source_health.value}")
        missing_roles = sorted(
            set(expectation.required_runtime_role_ids)
            - set(report.ready_runtime_role_ids)
        )
        if missing_roles:
            reasons.add("runtime_roles_not_ready")
        if (
            report.command_surface_state
            is not expectation.required_command_surface_state
        ):
            reasons.add("runtime_command_surface_state_mismatch")

        for dependency_id in expectation.dependency_runtime_unit_ids:
            dependency = derived[dependency_id]
            if not dependency.converged:
                reasons.add(f"runtime_dependency_not_converged:{dependency_id}")
            elif not dependency.runtime_epoch:
                reasons.add(f"runtime_dependency_epoch_missing:{dependency_id}")
            else:
                dependency_epochs[dependency_id] = dependency.runtime_epoch
                dependency_epoch_generation = (
                    self._epoch_change_event_generations.get(dependency_id)
                )
                if (
                    dependency_epoch_generation is None
                    or report_receipt.accepted_event_generation
                    <= dependency_epoch_generation
                ):
                    reasons.add("runtime_dependency_epoch_changed")

        report_identity = (
            report.process_incarnation,
            report.source_report_sequence,
            report.product_identity,
            report.configuration_identity,
        )
        if not reasons:
            join = self._dependency_joins.get(runtime_unit_id)
            if (
                join is not None
                and join.report_identity == report_identity
                and dict(join.runtime_epochs) != dependency_epochs
            ):
                reasons.add("runtime_dependency_epoch_changed")
            elif join is None or join.report_identity != report_identity:
                self._dependency_joins[runtime_unit_id] = _DependencyJoin(
                    report_identity=report_identity,
                    runtime_epochs=_frozen_mapping(dependency_epochs),
                )

        if reasons:
            state = (
                FixtureDerivedState.RECOVERING
                if "runtime_dependency_epoch_changed" in reasons
                else FixtureDerivedState.NONCONVERGED
            )
        else:
            state = FixtureDerivedState.CONVERGED
            self._last_converged_epoch[runtime_unit_id] = runtime_epoch

        return self._observation(
            expectation,
            state=state,
            process_presence=process_presence,
            runtime_epoch=runtime_epoch,
            presence_age_ns=presence_age_ns,
            report_age_ns=report_age_ns,
            progress_age_ns=progress_age_ns,
            report=report,
            dependency_epochs=dependency_epochs,
            reasons=reasons,
        )

    @staticmethod
    def _observation(
        expectation: FixtureRuntimeExpectation,
        *,
        state: FixtureDerivedState,
        process_presence: FixturePresenceState,
        runtime_epoch: str = "",
        presence_age_ns: int | None = None,
        report_age_ns: int | None = None,
        progress_age_ns: int | None = None,
        report: FixtureSourceReport | None = None,
        dependency_epochs: Mapping[str, str] | None = None,
        reasons: set[str],
    ) -> FixtureRuntimeObservation:
        reason_codes = tuple(sorted(reasons))
        if len(reason_codes) > _MAX_MANAGER_REASON_CODES:
            raise RuntimeError("fixture generated too many Manager reason codes")
        return FixtureRuntimeObservation(
            runtime_unit_id=expectation.runtime_unit_id,
            lifecycle_control=expectation.lifecycle_control,
            derived_state=state,
            process_presence=process_presence,
            runtime_epoch=runtime_epoch,
            presence_age_ns=presence_age_ns,
            report_age_ns=report_age_ns,
            progress_age_ns=progress_age_ns,
            observed_product_identity=(
                report.product_identity if report is not None else ""
            ),
            observed_configuration_identity=(
                report.configuration_identity if report is not None else ""
            ),
            source_phase=report.local_phase if report is not None else None,
            source_health=report.source_health if report is not None else None,
            ready_runtime_role_ids=(
                report.ready_runtime_role_ids if report is not None else ()
            ),
            command_surface_state=(
                report.command_surface_state if report is not None else None
            ),
            dependency_runtime_epochs=_frozen_mapping(
                dependency_epochs or {}
            ),
            source_reason_codes=(
                report.reason_codes if report is not None else ()
            ),
            reason_codes=reason_codes,
        )

    def _receipt_time(self, value: int | None) -> int:
        receipt = self._clock_ns() if value is None else value
        return _positive_integer(receipt, "receipt time")

    def _evaluation_time(self, value: int | None) -> int:
        evaluated = self._clock_ns() if value is None else value
        return _positive_integer(evaluated, "evaluation time")

    def _next_accepted_event_generation(self) -> int:
        self._accepted_event_generation += 1
        return self._accepted_event_generation

    @staticmethod
    def _age(now_ns: int, received_at_ns: int) -> tuple[int, bool]:
        return max(0, now_ns - received_at_ns), received_at_ns <= now_ns

    @staticmethod
    def _validate_and_order_expectations(
        expectations: Sequence[FixtureRuntimeExpectation],
        *,
        declared_runtime_unit_ids: frozenset[str],
    ) -> tuple[FixtureRuntimeExpectation, ...]:
        if (
            not isinstance(expectations, Sequence)
            or len(expectations) > _MAX_RUNTIME_UNITS
        ):
            raise ValueError("runtime expectations must be a bounded sequence")
        by_id: dict[str, FixtureRuntimeExpectation] = {}
        for expectation in expectations:
            if not isinstance(expectation, FixtureRuntimeExpectation):
                raise TypeError("runtime expectation has an invalid type")
            if expectation.runtime_unit_id in by_id:
                raise ValueError("runtime expectations must be unique")
            if expectation.runtime_unit_id not in declared_runtime_unit_ids:
                raise ValueError(
                    "runtime expectation references an undeclared runtime unit"
                )
            by_id[expectation.runtime_unit_id] = expectation
        unknown = sorted(
            {
                dependency_id
                for expectation in expectations
                for dependency_id in expectation.dependency_runtime_unit_ids
                if dependency_id not in by_id
            }
        )
        if unknown:
            raise ValueError(
                "runtime expectations contain unknown dependencies: "
                + ", ".join(unknown)
            )

        indegree = {runtime_unit_id: 0 for runtime_unit_id in by_id}
        dependants = {runtime_unit_id: set() for runtime_unit_id in by_id}
        for runtime_unit_id, expectation in by_id.items():
            for dependency_id in expectation.dependency_runtime_unit_ids:
                indegree[runtime_unit_id] += 1
                dependants[dependency_id].add(runtime_unit_id)
        ready = sorted(
            runtime_unit_id
            for runtime_unit_id, degree in indegree.items()
            if degree == 0
        )
        ordered: list[FixtureRuntimeExpectation] = []
        while ready:
            runtime_unit_id = ready.pop(0)
            ordered.append(by_id[runtime_unit_id])
            for dependant_id in sorted(dependants[runtime_unit_id]):
                indegree[dependant_id] -= 1
                if indegree[dependant_id] == 0:
                    ready.append(dependant_id)
                    ready.sort()
        if len(ordered) != len(by_id):
            raise ValueError("runtime expectation dependencies contain a cycle")
        return tuple(ordered)
