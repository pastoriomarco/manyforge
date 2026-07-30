"""ROS-free runtime-unit observation and convergence reduction.

This module owns the Component Manager's receiver-local interpretation of
runtime-unit evidence.  It deliberately keeps three fact families separate:

* a process observer reports presence and an independently evidenced process
  incarnation;
* the runtime source reports its local phase, health, meaningful progress,
  exact product/configuration evidence, role readiness, and the state of
  ManyForge-managed command surfaces; and
* the Component Manager attaches receiver-local freshness, joins dependency
  epochs, and derives convergence, loss, and recovery.

The values here are an internal production domain model, not the public
``RuntimeUnitProgressDTO`` wire contract.  In particular, the product and
configuration identities remain opaque until the I3 retained-product contract
is frozen.  This module performs no ROS I/O, process effect, Apply operation,
active-pointer mutation, robot-readiness inference, or lifecycle transition.

``RuntimeObservationReducer`` is a bounded, single-owner reducer.  Adapters
must serialize calls into it; immutable snapshots may be shared freely.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import StrEnum
from types import MappingProxyType
from typing import Callable, Mapping

_MAX_IDENTIFIER_LENGTH = 256
_MAX_RUNTIME_UNITS = 64
_MAX_DEPENDENCIES = 32
_MAX_RUNTIME_ROLES = 256
_MAX_SOURCE_REASON_CODES = 32
_MAX_MANAGER_REASON_CODES = 64
_MAX_MANAGER_REASON_CODE_LENGTH = _MAX_IDENTIFIER_LENGTH + 64
_MAX_REJECTION_REASONS = 32
_MAX_OBSERVER_SESSIONS_PER_RUNTIME_UNIT = 64
_MAX_INCARNATIONS_PER_RUNTIME_UNIT = 64


class RuntimeLifecycleControl(StrEnum):
    """Whether Manager may mutate a runtime unit's process lifecycle."""

    MANAGER_MANAGED = "manager_managed"
    EXTERNALLY_MANAGED = "externally_managed"


class ProcessPresenceState(StrEnum):
    """Process-presence evidence, independent of source health."""

    UNKNOWN = "unknown"
    ABSENT = "absent"
    PRESENT = "present"


class RuntimeSourcePhase(StrEnum):
    """Source-local phase; it never includes Manager-derived recovery."""

    STARTING = "starting"
    SERVING = "serving"
    STOPPING = "stopping"
    STOPPED = "stopped"
    FAULTED = "faulted"


class RuntimeSourceHealth(StrEnum):
    """A source-local health claim, not aggregate convergence."""

    HEALTHY = "healthy"
    DEGRADED = "degraded"
    FAULTED = "faulted"


class ManagedCommandSurfaceState(StrEnum):
    """State of ManyForge-managed command surfaces only."""

    CLOSED_NO_ACTIVE_EFFECT = "closed_no_active_effect"
    OPEN = "open"
    UNAVAILABLE = "unavailable"


class RuntimeConvergenceState(StrEnum):
    """Manager-derived state; a runtime source never publishes these values."""

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
        or any(
            ord(character) < 0x21 or ord(character) > 0x7E
            for character in value
        )
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


def _optional_nonnegative_integer(value: object, label: str) -> int | None:
    if value is None:
        return None
    return _nonnegative_integer(value, label)


def _optional_positive_integer(value: object, label: str) -> int | None:
    if value is None:
        return None
    return _positive_integer(value, label)


def _identifier_tuple(
    values: object,
    label: str,
    *,
    maximum: int,
    maximum_length: int = _MAX_IDENTIFIER_LENGTH,
) -> tuple[str, ...]:
    if not isinstance(values, tuple) or len(values) > maximum:
        raise ValueError(f"{label} must be a bounded tuple")
    checked: list[str] = []
    for item in values:
        if (
            not isinstance(item, str)
            or not item
            or len(item) > maximum_length
            or any(
                ord(character) < 0x21 or ord(character) > 0x7E
                for character in item
            )
        ):
            raise ValueError(f"{label} must contain bounded printable values")
        checked.append(item)
    normalized = tuple(checked)
    if len(set(normalized)) != len(normalized):
        raise ValueError(f"{label} must contain unique values")
    return tuple(sorted(normalized))


def runtime_unit_ids(values: object) -> tuple[str, ...]:
    """Validate and canonicalize one immutable bounded runtime-unit set."""

    return _identifier_tuple(
        values,
        "declared_runtime_unit_ids",
        maximum=_MAX_RUNTIME_UNITS,
    )


def _frozen_identifier_mapping(
    values: Mapping[str, str],
    *,
    label: str,
    maximum: int,
) -> Mapping[str, str]:
    if not isinstance(values, Mapping) or len(values) > maximum:
        raise ValueError(f"{label} must be a bounded mapping")
    normalized: dict[str, str] = {}
    for key, value in values.items():
        normalized[_identifier(key, f"{label} key")] = _identifier(
            value,
            f"{label} value",
        )
    return MappingProxyType(dict(sorted(normalized.items())))


@dataclass(frozen=True, slots=True)
class RuntimeUnitExpectation:
    """Manager policy and exact desired evidence for one runtime unit."""

    runtime_unit_id: str
    lifecycle_control: RuntimeLifecycleControl
    product_identity: str
    configuration_identity: str
    maximum_presence_age_ns: int
    maximum_report_age_ns: int
    maximum_progress_age_ns: int | None = None
    dependency_runtime_unit_ids: tuple[str, ...] = ()
    required_runtime_role_ids: tuple[str, ...] = ()
    required_command_surface_state: ManagedCommandSurfaceState = (
        ManagedCommandSurfaceState.CLOSED_NO_ACTIVE_EFFECT
    )

    def __post_init__(self) -> None:
        _identifier(self.runtime_unit_id, "runtime_unit_id")
        if not isinstance(self.lifecycle_control, RuntimeLifecycleControl):
            raise ValueError("lifecycle_control is invalid")
        _identifier(self.product_identity, "product_identity")
        _identifier(self.configuration_identity, "configuration_identity")
        _positive_integer(
            self.maximum_presence_age_ns,
            "maximum_presence_age_ns",
        )
        _positive_integer(
            self.maximum_report_age_ns,
            "maximum_report_age_ns",
        )
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
        object.__setattr__(
            self,
            "dependency_runtime_unit_ids",
            dependencies,
        )
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
            ManagedCommandSurfaceState,
        ):
            raise ValueError("required_command_surface_state is invalid")


@dataclass(frozen=True, slots=True)
class ProcessPresenceObservation:
    """One process-observer sample, separate from source reports."""

    runtime_unit_id: str
    observer_instance_id: str
    observation_sequence: int
    state: ProcessPresenceState
    process_incarnation: str = ""

    def __post_init__(self) -> None:
        _identifier(self.runtime_unit_id, "runtime_unit_id")
        _identifier(self.observer_instance_id, "observer_instance_id")
        _positive_integer(self.observation_sequence, "observation_sequence")
        if not isinstance(self.state, ProcessPresenceState):
            raise ValueError("presence state is invalid")
        if self.state is ProcessPresenceState.PRESENT:
            _identifier(self.process_incarnation, "process_incarnation")
        elif self.process_incarnation:
            raise ValueError(
                "non-present process evidence cannot carry an incarnation"
            )


@dataclass(frozen=True, slots=True)
class RuntimeUnitSourceReport:
    """Source-attested local report containing no Manager-derived fields."""

    runtime_unit_id: str
    process_incarnation: str
    source_report_sequence: int
    critical_progress_sequence: int
    local_phase: RuntimeSourcePhase
    source_health: RuntimeSourceHealth
    product_identity: str
    configuration_identity: str
    ready_runtime_role_ids: tuple[str, ...]
    command_surface_state: ManagedCommandSurfaceState
    reason_codes: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        for label, value in (
            ("runtime_unit_id", self.runtime_unit_id),
            ("process_incarnation", self.process_incarnation),
            ("product_identity", self.product_identity),
            ("configuration_identity", self.configuration_identity),
        ):
            _identifier(value, label)
        _positive_integer(
            self.source_report_sequence,
            "source_report_sequence",
        )
        _nonnegative_integer(
            self.critical_progress_sequence,
            "critical_progress_sequence",
        )
        if not isinstance(self.local_phase, RuntimeSourcePhase):
            raise ValueError("local_phase is invalid")
        if not isinstance(self.source_health, RuntimeSourceHealth):
            raise ValueError("source_health is invalid")
        if not isinstance(
            self.command_surface_state,
            ManagedCommandSurfaceState,
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
class RuntimeUnitObservation:
    """Immutable Manager projection with source and Manager reasons separate."""

    runtime_unit_id: str
    lifecycle_control: RuntimeLifecycleControl
    derived_state: RuntimeConvergenceState
    process_presence: ProcessPresenceState
    process_observer_instance_id: str
    process_observation_sequence: int | None
    runtime_epoch: str
    presence_age_ns: int | None
    report_age_ns: int | None
    progress_age_ns: int | None
    observed_product_identity: str
    observed_configuration_identity: str
    source_phase: RuntimeSourcePhase | None
    source_health: RuntimeSourceHealth | None
    source_report_sequence: int | None
    critical_progress_sequence: int | None
    ready_runtime_role_ids: tuple[str, ...]
    command_surface_state: ManagedCommandSurfaceState | None
    dependency_runtime_epochs: Mapping[str, str]
    source_reason_codes: tuple[str, ...]
    reason_codes: tuple[str, ...]

    def __post_init__(self) -> None:
        _identifier(self.runtime_unit_id, "runtime_unit_id")
        if not isinstance(self.lifecycle_control, RuntimeLifecycleControl):
            raise ValueError("lifecycle_control is invalid")
        if not isinstance(self.derived_state, RuntimeConvergenceState):
            raise ValueError("derived_state is invalid")
        if not isinstance(self.process_presence, ProcessPresenceState):
            raise ValueError("process_presence is invalid")
        if self.process_observer_instance_id:
            _identifier(
                self.process_observer_instance_id,
                "process_observer_instance_id",
            )
        _optional_positive_integer(
            self.process_observation_sequence,
            "process_observation_sequence",
        )
        if bool(self.process_observer_instance_id) != (
            self.process_observation_sequence is not None
        ):
            raise ValueError(
                "process observer identity and sequence must appear together"
            )
        if self.runtime_epoch:
            _identifier(self.runtime_epoch, "runtime_epoch")
        if self.process_presence is ProcessPresenceState.PRESENT:
            if not self.runtime_epoch:
                raise ValueError("present process observation requires an epoch")
        elif self.runtime_epoch:
            raise ValueError("non-present process observation cannot carry an epoch")
        for label, value in (
            ("presence_age_ns", self.presence_age_ns),
            ("report_age_ns", self.report_age_ns),
            ("progress_age_ns", self.progress_age_ns),
        ):
            _optional_nonnegative_integer(value, label)
        for label, value in (
            ("observed_product_identity", self.observed_product_identity),
            (
                "observed_configuration_identity",
                self.observed_configuration_identity,
            ),
        ):
            if value:
                _identifier(value, label)
        if self.source_phase is not None and not isinstance(
            self.source_phase,
            RuntimeSourcePhase,
        ):
            raise ValueError("source_phase is invalid")
        if self.source_health is not None and not isinstance(
            self.source_health,
            RuntimeSourceHealth,
        ):
            raise ValueError("source_health is invalid")
        _optional_positive_integer(
            self.source_report_sequence,
            "source_report_sequence",
        )
        _optional_nonnegative_integer(
            self.critical_progress_sequence,
            "critical_progress_sequence",
        )
        if self.command_surface_state is not None and not isinstance(
            self.command_surface_state,
            ManagedCommandSurfaceState,
        ):
            raise ValueError("command_surface_state is invalid")
        has_report = self.source_report_sequence is not None
        report_fields_complete = all(
            (
                self.source_phase is not None,
                self.source_health is not None,
                self.critical_progress_sequence is not None,
                bool(self.observed_product_identity),
                bool(self.observed_configuration_identity),
                self.command_surface_state is not None,
            )
        )
        if has_report != report_fields_complete:
            raise ValueError("source report projection is incomplete")
        if has_report and self.process_presence is not ProcessPresenceState.PRESENT:
            raise ValueError("source report projection requires process presence")
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
            "dependency_runtime_epochs",
            _frozen_identifier_mapping(
                self.dependency_runtime_epochs,
                label="dependency_runtime_epochs",
                maximum=_MAX_DEPENDENCIES,
            ),
        )
        object.__setattr__(
            self,
            "source_reason_codes",
            _identifier_tuple(
                self.source_reason_codes,
                "source_reason_codes",
                maximum=_MAX_SOURCE_REASON_CODES,
            ),
        )
        object.__setattr__(
            self,
            "reason_codes",
            _identifier_tuple(
                self.reason_codes,
                "reason_codes",
                maximum=_MAX_MANAGER_REASON_CODES,
                maximum_length=_MAX_MANAGER_REASON_CODE_LENGTH,
            ),
        )

    @property
    def converged(self) -> bool:
        return self.derived_state is RuntimeConvergenceState.CONVERGED


@dataclass(frozen=True, slots=True)
class RuntimeObservationSnapshot:
    """One immutable, deterministically ordered evaluation snapshot."""

    evaluated_at_ns: int
    observations: tuple[RuntimeUnitObservation, ...]

    def __post_init__(self) -> None:
        _positive_integer(self.evaluated_at_ns, "evaluated_at_ns")
        if (
            not isinstance(self.observations, tuple)
            or len(self.observations) > _MAX_RUNTIME_UNITS
            or any(
                not isinstance(item, RuntimeUnitObservation)
                for item in self.observations
            )
        ):
            raise ValueError("observations must be a bounded typed tuple")
        identifiers = tuple(item.runtime_unit_id for item in self.observations)
        if len(set(identifiers)) != len(identifiers):
            raise ValueError("observations must contain unique runtime units")
        object.__setattr__(
            self,
            "observations",
            tuple(
                sorted(
                    self.observations,
                    key=lambda item: item.runtime_unit_id,
                )
            ),
        )

    def observation(
        self,
        runtime_unit_id: str,
    ) -> RuntimeUnitObservation | None:
        _identifier(runtime_unit_id, "runtime_unit_id")
        return next(
            (
                item
                for item in self.observations
                if item.runtime_unit_id == runtime_unit_id
            ),
            None,
        )


@dataclass(frozen=True, slots=True)
class RuntimeUnitProcessBinding:
    """Observed/effect handles proving one runtime-unit failure boundary."""

    runtime_unit_id: str
    process_boundary_id: str
    lifecycle_control: RuntimeLifecycleControl
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
        if not isinstance(self.lifecycle_control, RuntimeLifecycleControl):
            raise ValueError("lifecycle_control is invalid")
        if self.lifecycle_control is RuntimeLifecycleControl.MANAGER_MANAGED:
            _identifier(
                self.managed_lifecycle_handle_id,
                "managed_lifecycle_handle_id",
            )
        elif self.managed_lifecycle_handle_id:
            raise ValueError(
                "externally managed bindings cannot expose a Manager "
                "lifecycle handle"
            )


def validate_runtime_unit_process_bindings(
    bindings: tuple[RuntimeUnitProcessBinding, ...],
) -> None:
    """Reject fake independent units sharing an inseparable process handle."""

    if not isinstance(bindings, tuple) or len(bindings) > _MAX_RUNTIME_UNITS:
        raise ValueError("process bindings must be a bounded tuple")
    if any(not isinstance(item, RuntimeUnitProcessBinding) for item in bindings):
        raise TypeError("process binding has an invalid type")
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

    by_boundary: dict[str, list[RuntimeUnitProcessBinding]] = {}
    for binding in bindings:
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
            if item.lifecycle_control is RuntimeLifecycleControl.MANAGER_MANAGED
        )
        if len(set(managed_handles)) != len(managed_handles):
            raise ValueError(
                "co-located managed runtime units lack independent "
                f"lifecycle handles: {boundary_id}"
            )


@dataclass(frozen=True, slots=True)
class RuntimeObservationReducerMetrics:
    """Immutable diagnostic counters; reading them cannot renew freshness."""

    accepted_presence_total: int
    accepted_source_report_total: int
    duplicate_total: int
    rejected_total: int
    rejection_reasons: Mapping[str, int]

    def __post_init__(self) -> None:
        for label, value in (
            ("accepted_presence_total", self.accepted_presence_total),
            (
                "accepted_source_report_total",
                self.accepted_source_report_total,
            ),
            ("duplicate_total", self.duplicate_total),
            ("rejected_total", self.rejected_total),
        ):
            _nonnegative_integer(value, label)
        if not isinstance(self.rejection_reasons, Mapping):
            raise ValueError("rejection_reasons must be a mapping")
        if len(self.rejection_reasons) > _MAX_REJECTION_REASONS:
            raise ValueError("rejection_reasons must be bounded")
        normalized: dict[str, int] = {}
        for reason, count in self.rejection_reasons.items():
            normalized[_identifier(reason, "rejection reason")] = (
                _positive_integer(count, "rejection count")
            )
        object.__setattr__(
            self,
            "rejection_reasons",
            MappingProxyType(dict(sorted(normalized.items()))),
        )


@dataclass(frozen=True, slots=True)
class _AcceptedPresence:
    sample: ProcessPresenceObservation
    received_at_ns: int
    accepted_event_generation: int


@dataclass(frozen=True, slots=True)
class _AcceptedReport:
    sample: RuntimeUnitSourceReport
    report_received_at_ns: int
    progress_received_at_ns: int
    accepted_event_generation: int


@dataclass(frozen=True, slots=True)
class _DependencyJoin:
    report_identity: tuple[object, ...]
    runtime_epochs: Mapping[str, str] = field(default_factory=dict)


class RuntimeObservationReducer:
    """Bounded receiver-local reducer for one declared runtime-unit set."""

    def __init__(
        self,
        *,
        clock_ns: Callable[[], int],
        declared_runtime_unit_ids: tuple[str, ...],
    ) -> None:
        if not callable(clock_ns):
            raise TypeError("clock_ns must be callable")
        declared = runtime_unit_ids(declared_runtime_unit_ids)
        self._clock_ns = clock_ns
        self._last_clock_ns: int | None = None
        self._declared_runtime_unit_ids = frozenset(declared)
        self._ordered_runtime_unit_ids = declared
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
        self._accepted_presence_total = 0
        self._accepted_source_report_total = 0
        self._duplicate_total = 0
        self._rejected_total = 0
        self._rejection_reasons: dict[str, int] = {}
        self._accepted_event_generation = 0

    @property
    def declared_runtime_unit_ids(self) -> tuple[str, ...]:
        return self._ordered_runtime_unit_ids

    @property
    def metrics(self) -> RuntimeObservationReducerMetrics:
        return RuntimeObservationReducerMetrics(
            accepted_presence_total=self._accepted_presence_total,
            accepted_source_report_total=self._accepted_source_report_total,
            duplicate_total=self._duplicate_total,
            rejected_total=self._rejected_total,
            rejection_reasons=self._rejection_reasons,
        )

    def _reject(self, reason: str) -> bool:
        self._rejected_total += 1
        self._rejection_reasons[reason] = (
            self._rejection_reasons.get(reason, 0) + 1
        )
        return False

    def observe_process(self, sample: ProcessPresenceObservation) -> bool:
        """Accept one sample from the current process-observer session."""

        if not isinstance(sample, ProcessPresenceObservation):
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
        return self._accept_process_presence(
            sample,
            received_at_ns=self._receiver_now(),
            replacing_observer=False,
        )

    def replace_process_observer(
        self,
        sample: ProcessPresenceObservation,
    ) -> bool:
        """Fence the active observer before accepting an explicit replacement."""

        if not isinstance(sample, ProcessPresenceObservation):
            return self._reject("process_presence_invalid")
        runtime_unit_id = sample.runtime_unit_id
        if runtime_unit_id not in self._declared_runtime_unit_ids:
            return self._reject("process_presence_runtime_unit_undeclared")
        active_observer_id = self._active_observer_ids.get(runtime_unit_id)
        if (
            active_observer_id is None
            or active_observer_id == sample.observer_instance_id
        ):
            return self.observe_process(sample)
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
        return self._accept_process_presence(
            sample,
            received_at_ns=self._receiver_now(),
            replacing_observer=True,
        )

    def _accept_process_presence(
        self,
        sample: ProcessPresenceObservation,
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
                    self._duplicate_total += 1
                    return False
                return self._reject("process_observation_sequence_conflict")

        current_epoch = self._last_present_incarnation.get(
            runtime_unit_id,
            "",
        )
        candidate_epoch = (
            sample.process_incarnation
            if sample.state is ProcessPresenceState.PRESENT
            else ""
        )
        if (
            candidate_epoch
            and candidate_epoch in self._retired_incarnations[runtime_unit_id]
            and candidate_epoch != current_epoch
        ):
            return self._reject("process_incarnation_reused")
        if sample.state is ProcessPresenceState.PRESENT:
            if not self._register_incarnation(
                runtime_unit_id,
                candidate_epoch,
            ):
                return False
        accepted_event_generation = self._next_accepted_event_generation()
        if sample.state is ProcessPresenceState.PRESENT:
            if current_epoch and candidate_epoch != current_epoch:
                self._retired_incarnations[runtime_unit_id].add(current_epoch)
                self._reports[runtime_unit_id].pop(current_epoch, None)
            if candidate_epoch != current_epoch:
                self._epoch_change_event_generations[runtime_unit_id] = (
                    accepted_event_generation
                )
            self._last_present_incarnation[runtime_unit_id] = candidate_epoch
        elif sample.state is ProcessPresenceState.ABSENT and current_epoch:
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
        self._accepted_presence_total += 1
        return True

    def ingest_source_report(self, report: RuntimeUnitSourceReport) -> bool:
        """Accept one monotonic source report for the current process epoch."""

        if not isinstance(report, RuntimeUnitSourceReport):
            return self._reject("source_report_invalid")
        runtime_unit_id = report.runtime_unit_id
        if runtime_unit_id not in self._declared_runtime_unit_ids:
            return self._reject("source_report_runtime_unit_undeclared")
        presence = self._presence.get(runtime_unit_id)
        if (
            presence is None
            or presence.sample.state is not ProcessPresenceState.PRESENT
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

        reports_by_incarnation = self._reports[runtime_unit_id]
        current = reports_by_incarnation.get(report.process_incarnation)
        if current is not None:
            if (
                report.source_report_sequence
                < current.sample.source_report_sequence
            ):
                return self._reject("source_report_sequence_regressed")
            if (
                report.source_report_sequence
                == current.sample.source_report_sequence
            ):
                if report == current.sample:
                    self._duplicate_total += 1
                    return False
                return self._reject("source_report_sequence_conflict")
            if (
                report.critical_progress_sequence
                < current.sample.critical_progress_sequence
            ):
                return self._reject("critical_progress_sequence_regressed")

        received_at_ns = self._receiver_now()
        if current is None:
            progress_received_at_ns = received_at_ns
        elif (
            report.critical_progress_sequence
            > current.sample.critical_progress_sequence
        ):
            progress_received_at_ns = received_at_ns
        else:
            progress_received_at_ns = current.progress_received_at_ns
        accepted_event_generation = self._next_accepted_event_generation()
        reports_by_incarnation[report.process_incarnation] = _AcceptedReport(
            sample=report,
            report_received_at_ns=received_at_ns,
            progress_received_at_ns=progress_received_at_ns,
            accepted_event_generation=accepted_event_generation,
        )
        self._accepted_source_report_total += 1
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
        expectations: tuple[RuntimeUnitExpectation, ...],
    ) -> RuntimeObservationSnapshot:
        """Derive one immutable snapshot without performing external effects."""

        evaluated_at_ns = self._receiver_now()
        ordered_expectations = self._validate_and_order_expectations(
            expectations,
            declared_runtime_unit_ids=self._declared_runtime_unit_ids,
        )
        derived: dict[str, RuntimeUnitObservation] = {}
        for expectation in ordered_expectations:
            derived[expectation.runtime_unit_id] = self._evaluate_one(
                expectation,
                derived=derived,
                now_ns=evaluated_at_ns,
            )
        return RuntimeObservationSnapshot(
            evaluated_at_ns=evaluated_at_ns,
            observations=tuple(
                derived[expectation.runtime_unit_id]
                for expectation in ordered_expectations
            ),
        )

    def _evaluate_one(
        self,
        expectation: RuntimeUnitExpectation,
        *,
        derived: Mapping[str, RuntimeUnitObservation],
        now_ns: int,
    ) -> RuntimeUnitObservation:
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
                state=RuntimeConvergenceState.LOST,
                process_presence=ProcessPresenceState.UNKNOWN,
                reasons={"process_presence_missing"},
            )

        presence_age_ns = now_ns - presence.received_at_ns
        if presence_age_ns > expectation.maximum_presence_age_ns:
            reasons.add("process_presence_stale")
        process_presence = presence.sample.state
        if process_presence is ProcessPresenceState.UNKNOWN:
            reasons.add("process_presence_unknown")
        elif process_presence is ProcessPresenceState.ABSENT:
            reasons.add("process_absent")
        else:
            runtime_epoch = presence.sample.process_incarnation

        if reasons:
            return self._observation(
                expectation,
                state=RuntimeConvergenceState.LOST,
                process_presence=process_presence,
                process_observer_instance_id=(
                    presence.sample.observer_instance_id
                ),
                process_observation_sequence=(
                    presence.sample.observation_sequence
                ),
                runtime_epoch=runtime_epoch,
                presence_age_ns=presence_age_ns,
                reasons=reasons,
            )

        report_receipt = self._reports[runtime_unit_id].get(runtime_epoch)
        if report_receipt is None:
            return self._observation(
                expectation,
                state=(
                    RuntimeConvergenceState.RECOVERING
                    if runtime_unit_id in self._last_converged_epoch
                    else RuntimeConvergenceState.NONCONVERGED
                ),
                process_presence=process_presence,
                process_observer_instance_id=(
                    presence.sample.observer_instance_id
                ),
                process_observation_sequence=(
                    presence.sample.observation_sequence
                ),
                runtime_epoch=runtime_epoch,
                presence_age_ns=presence_age_ns,
                reasons={"source_report_missing"},
            )

        report = report_receipt.sample
        report_age_ns = now_ns - report_receipt.report_received_at_ns
        progress_age_ns = now_ns - report_receipt.progress_received_at_ns
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
                state=RuntimeConvergenceState.LOST,
                process_presence=process_presence,
                process_observer_instance_id=(
                    presence.sample.observer_instance_id
                ),
                process_observation_sequence=(
                    presence.sample.observation_sequence
                ),
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
        if report.local_phase is not RuntimeSourcePhase.SERVING:
            reasons.add(f"runtime_source_phase_{report.local_phase.value}")
        if report.source_health is not RuntimeSourceHealth.HEALTHY:
            reasons.add(f"runtime_source_health_{report.source_health.value}")
        if not set(expectation.required_runtime_role_ids).issubset(
            report.ready_runtime_role_ids
        ):
            reasons.add("runtime_roles_not_ready")
        if (
            report.command_surface_state
            is not expectation.required_command_surface_state
        ):
            reasons.add("runtime_command_surface_state_mismatch")

        for dependency_id in expectation.dependency_runtime_unit_ids:
            dependency = derived[dependency_id]
            if not dependency.converged:
                reasons.add(
                    f"runtime_dependency_not_converged:{dependency_id}"
                )
            elif not dependency.runtime_epoch:
                reasons.add(
                    f"runtime_dependency_epoch_missing:{dependency_id}"
                )
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
                    runtime_epochs=_frozen_identifier_mapping(
                        dependency_epochs,
                        label="dependency_runtime_epochs",
                        maximum=_MAX_DEPENDENCIES,
                    ),
                )

        if reasons:
            state = (
                RuntimeConvergenceState.RECOVERING
                if "runtime_dependency_epoch_changed" in reasons
                else RuntimeConvergenceState.NONCONVERGED
            )
        else:
            state = RuntimeConvergenceState.CONVERGED
            self._last_converged_epoch[runtime_unit_id] = runtime_epoch

        return self._observation(
            expectation,
            state=state,
            process_presence=process_presence,
            process_observer_instance_id=presence.sample.observer_instance_id,
            process_observation_sequence=presence.sample.observation_sequence,
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
        expectation: RuntimeUnitExpectation,
        *,
        state: RuntimeConvergenceState,
        process_presence: ProcessPresenceState,
        process_observer_instance_id: str = "",
        process_observation_sequence: int | None = None,
        runtime_epoch: str = "",
        presence_age_ns: int | None = None,
        report_age_ns: int | None = None,
        progress_age_ns: int | None = None,
        report: RuntimeUnitSourceReport | None = None,
        dependency_epochs: Mapping[str, str] | None = None,
        reasons: set[str],
    ) -> RuntimeUnitObservation:
        reason_codes = tuple(sorted(reasons))
        if len(reason_codes) > _MAX_MANAGER_REASON_CODES:
            raise RuntimeError("generated too many Manager reason codes")
        return RuntimeUnitObservation(
            runtime_unit_id=expectation.runtime_unit_id,
            lifecycle_control=expectation.lifecycle_control,
            derived_state=state,
            process_presence=process_presence,
            process_observer_instance_id=process_observer_instance_id,
            process_observation_sequence=process_observation_sequence,
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
            source_report_sequence=(
                report.source_report_sequence if report is not None else None
            ),
            critical_progress_sequence=(
                report.critical_progress_sequence
                if report is not None
                else None
            ),
            ready_runtime_role_ids=(
                report.ready_runtime_role_ids if report is not None else ()
            ),
            command_surface_state=(
                report.command_surface_state if report is not None else None
            ),
            dependency_runtime_epochs=dependency_epochs or {},
            source_reason_codes=(
                report.reason_codes if report is not None else ()
            ),
            reason_codes=reason_codes,
        )

    def _receiver_now(self) -> int:
        now_ns = _positive_integer(self._clock_ns(), "receiver clock")
        if self._last_clock_ns is not None and now_ns < self._last_clock_ns:
            raise RuntimeError("receiver monotonic clock regressed")
        self._last_clock_ns = now_ns
        return now_ns

    def _next_accepted_event_generation(self) -> int:
        self._accepted_event_generation += 1
        return self._accepted_event_generation

    @staticmethod
    def _validate_and_order_expectations(
        expectations: tuple[RuntimeUnitExpectation, ...],
        *,
        declared_runtime_unit_ids: frozenset[str],
    ) -> tuple[RuntimeUnitExpectation, ...]:
        if (
            not isinstance(expectations, tuple)
            or len(expectations) > _MAX_RUNTIME_UNITS
        ):
            raise ValueError("runtime expectations must be a bounded tuple")
        by_id: dict[str, RuntimeUnitExpectation] = {}
        for expectation in expectations:
            if not isinstance(expectation, RuntimeUnitExpectation):
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
        ordered: list[RuntimeUnitExpectation] = []
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


__all__ = [
    "ManagedCommandSurfaceState",
    "ProcessPresenceObservation",
    "ProcessPresenceState",
    "RuntimeConvergenceState",
    "RuntimeLifecycleControl",
    "RuntimeObservationReducer",
    "RuntimeObservationReducerMetrics",
    "RuntimeObservationSnapshot",
    "RuntimeSourceHealth",
    "RuntimeSourcePhase",
    "RuntimeUnitExpectation",
    "RuntimeUnitObservation",
    "RuntimeUnitProcessBinding",
    "RuntimeUnitSourceReport",
    "runtime_unit_ids",
    "validate_runtime_unit_process_bindings",
]
