"""
Deterministic component-health reduction and execution pinning.

The module deliberately has no ROS, Docker, web, vendor, or process-control
dependency. Adapters translate their evidence into ``ComponentHeartbeat``
values and apply the reducer's immutable decision elsewhere.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import StrEnum
from types import MappingProxyType
from typing import Callable, Mapping

_MAX_IDENTIFIER_LENGTH = 256
_MAX_COMPONENTS = 64
_MAX_DEPENDENCIES = 32
_MAX_REASONS = 128


class ComponentRole(StrEnum):
    TRANSPORT_OWNER = 'transport_owner'
    LIFECYCLE_PROVIDER = 'lifecycle_provider'
    EXECUTION_BACKEND = 'execution_backend'
    CONTROL_AUTHORITY = 'control_authority'
    CELL_SUPERVISOR = 'cell_supervisor'
    HMI = 'hmi'


class ComponentHealthState(StrEnum):
    STARTING = 'starting'
    HEALTHY = 'healthy'
    DEGRADED = 'degraded'
    LOST = 'lost'
    RECOVERING = 'recovering'


def _identifier(value: str, label: str) -> str:
    if (
        not isinstance(value, str)
        or not value
        or len(value) > _MAX_IDENTIFIER_LENGTH
        or any(ord(character) < 0x21 or ord(character) > 0x7E for character in value)
    ):
        raise ValueError(f'{label} must be a bounded printable identifier')
    return value


def _positive_integer(value: int, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise ValueError(f'{label} must be a positive integer')
    return value


def _frozen_mapping(
    values: Mapping[str, str],
    *,
    label: str,
    maximum: int,
) -> Mapping[str, str]:
    if not isinstance(values, Mapping) or len(values) > maximum:
        raise ValueError(f'{label} must be a bounded mapping')
    result: dict[str, str] = {}
    for key, value in values.items():
        result[_identifier(key, f'{label} key')] = _identifier(
            value, f'{label} value'
        )
    return MappingProxyType(dict(sorted(result.items())))


@dataclass(frozen=True, slots=True)
class ComponentHeartbeat:
    component_role: ComponentRole
    component_id: str
    robot_id: str
    cell_id: str
    component_epoch: str
    sequence: int
    health_state: ComponentHealthState
    critical_progress_sequence: int
    dependency_epochs: Mapping[str, str] = field(default_factory=dict)
    runtime_generation: str = ''
    binding_generation: str = ''
    configuration_generation: str = ''

    def __post_init__(self) -> None:
        if not isinstance(self.component_role, ComponentRole):
            raise ValueError('component_role is invalid')
        if not isinstance(self.health_state, ComponentHealthState):
            raise ValueError('health_state is invalid')
        for label, value in (
            ('component_id', self.component_id),
            ('robot_id', self.robot_id),
            ('cell_id', self.cell_id),
            ('component_epoch', self.component_epoch),
        ):
            _identifier(value, label)
        _positive_integer(self.sequence, 'sequence')
        _positive_integer(
            self.critical_progress_sequence,
            'critical_progress_sequence',
        )
        object.__setattr__(
            self,
            'dependency_epochs',
            _frozen_mapping(
                self.dependency_epochs,
                label='dependency_epochs',
                maximum=_MAX_DEPENDENCIES,
            ),
        )
        for label, value in (
            ('runtime_generation', self.runtime_generation),
            ('binding_generation', self.binding_generation),
            ('configuration_generation', self.configuration_generation),
        ):
            if value:
                _identifier(value, label)


@dataclass(frozen=True, slots=True)
class ComponentPolicy:
    component_role: ComponentRole
    component_id: str
    maximum_heartbeat_age_ns: int
    maximum_critical_progress_age_ns: int
    dependency_ids: tuple[str, ...] = ()
    restart_domain: str = ''
    required_for_dispatch: bool = True
    expected_runtime_generation: str = ''
    expected_binding_generation: str = ''
    expected_configuration_generation: str = ''

    def __post_init__(self) -> None:
        if not isinstance(self.component_role, ComponentRole):
            raise ValueError('component_role is invalid')
        _identifier(self.component_id, 'component_id')
        _positive_integer(
            self.maximum_heartbeat_age_ns,
            'maximum_heartbeat_age_ns',
        )
        _positive_integer(
            self.maximum_critical_progress_age_ns,
            'maximum_critical_progress_age_ns',
        )
        if (
            not isinstance(self.dependency_ids, tuple)
            or len(self.dependency_ids) > _MAX_DEPENDENCIES
            or len(set(self.dependency_ids)) != len(self.dependency_ids)
        ):
            raise ValueError('dependency_ids must be a bounded unique tuple')
        for dependency_id in self.dependency_ids:
            _identifier(dependency_id, 'dependency_id')
            if dependency_id == self.component_id:
                raise ValueError('a component cannot depend on itself')
        if self.restart_domain:
            _identifier(self.restart_domain, 'restart_domain')
        for label, value in (
            ('expected_runtime_generation', self.expected_runtime_generation),
            ('expected_binding_generation', self.expected_binding_generation),
            (
                'expected_configuration_generation',
                self.expected_configuration_generation,
            ),
        ):
            if value:
                _identifier(value, label)


@dataclass(frozen=True, slots=True)
class SupervisionPolicy:
    cell_id: str
    robot_ids: tuple[str, ...]
    authority_profile: str
    components: tuple[ComponentPolicy, ...]

    def __post_init__(self) -> None:
        _identifier(self.cell_id, 'cell_id')
        _identifier(self.authority_profile, 'authority_profile')
        if (
            not isinstance(self.robot_ids, tuple)
            or not self.robot_ids
            or len(self.robot_ids) > _MAX_COMPONENTS
            or len(set(self.robot_ids)) != len(self.robot_ids)
        ):
            raise ValueError('robot_ids must be a bounded unique tuple')
        for robot_id in self.robot_ids:
            _identifier(robot_id, 'robot_id')
        if (
            not isinstance(self.components, tuple)
            or not self.components
            or len(self.components) > _MAX_COMPONENTS
        ):
            raise ValueError('components must be a non-empty bounded tuple')
        component_ids = tuple(item.component_id for item in self.components)
        if len(set(component_ids)) != len(component_ids):
            raise ValueError('component policy ids must be unique')
        known = set(component_ids)
        for item in self.components:
            unknown = sorted(set(item.dependency_ids) - known)
            if unknown:
                raise ValueError(
                    'component dependencies are unknown: ' + ', '.join(unknown)
                )
        self._validate_acyclic()

    def _validate_acyclic(self) -> None:
        policies = {item.component_id: item for item in self.components}
        visiting: set[str] = set()
        complete: set[str] = set()

        def visit(component_id: str) -> None:
            if component_id in complete:
                return
            if component_id in visiting:
                raise ValueError('component dependency graph contains a cycle')
            visiting.add(component_id)
            for dependency_id in policies[component_id].dependency_ids:
                visit(dependency_id)
            visiting.remove(component_id)
            complete.add(component_id)

        for component_id in policies:
            visit(component_id)


@dataclass(frozen=True, slots=True)
class ComponentRecord:
    component_role: ComponentRole
    component_id: str
    component_epoch: str
    sequence: int
    heartbeat_age_ns: int | None
    critical_progress_sequence: int
    critical_progress_age_ns: int | None
    health_state: ComponentHealthState
    healthy: bool
    dependency_epochs: Mapping[str, str]
    reason_codes: tuple[str, ...]
    restart_domain: str
    runtime_generation: str
    binding_generation: str
    configuration_generation: str


@dataclass(frozen=True, slots=True)
class CellSupervisionSnapshot:
    generation: int
    evaluated_at_ns: int
    cell_id: str
    robot_ids: tuple[str, ...]
    authority_profile: str
    authority_component_epoch: str
    components: tuple[ComponentRecord, ...]
    dependency_graph_healthy: bool
    dispatch_admitted: bool
    active_environment_healthy: bool
    reason_codes: tuple[str, ...]
    requested_containment: str
    restart_domains: tuple[str, ...]

    def component(self, component_id: str) -> ComponentRecord | None:
        return next(
            (
                record
                for record in self.components
                if record.component_id == component_id
            ),
            None,
        )


@dataclass(frozen=True, slots=True)
class ExecutionEnvironmentPin:
    execution_id: str
    program_run_id: str
    robot_id: str
    cell_id: str
    program_generation: str
    deployment_generation: str
    runtime_generation: str
    controller_binding_generation: str
    authority_profile: str
    authority_component_epoch: str
    component_epochs: Mapping[str, str]
    lifecycle_generation: int
    admission_generation: int
    supervision_generation: int

    def __post_init__(self) -> None:
        for label, value in (
            ('execution_id', self.execution_id),
            ('robot_id', self.robot_id),
            ('cell_id', self.cell_id),
            ('program_generation', self.program_generation),
            ('deployment_generation', self.deployment_generation),
            ('runtime_generation', self.runtime_generation),
            (
                'controller_binding_generation',
                self.controller_binding_generation,
            ),
            ('authority_profile', self.authority_profile),
            ('authority_component_epoch', self.authority_component_epoch),
        ):
            _identifier(value, label)
        if self.program_run_id:
            _identifier(self.program_run_id, 'program_run_id')
        object.__setattr__(
            self,
            'component_epochs',
            _frozen_mapping(
                self.component_epochs,
                label='component_epochs',
                maximum=_MAX_COMPONENTS,
            ),
        )
        _positive_integer(self.lifecycle_generation, 'lifecycle_generation')
        _positive_integer(self.admission_generation, 'admission_generation')
        _positive_integer(self.supervision_generation, 'supervision_generation')


@dataclass(frozen=True, slots=True)
class PinValidation:
    valid: bool
    reason_codes: tuple[str, ...]


@dataclass(slots=True)
class _AcceptedHeartbeat:
    heartbeat: ComponentHeartbeat
    received_at_ns: int
    critical_progress_received_at_ns: int


class ComponentHealthReducer:
    """Reduce receiver-local heartbeat evidence into immutable health."""

    def __init__(
        self,
        policy: SupervisionPolicy,
        *,
        clock_ns: Callable[[], int],
    ) -> None:
        if not isinstance(policy, SupervisionPolicy):
            raise TypeError('policy must be a SupervisionPolicy')
        if not callable(clock_ns):
            raise TypeError('clock_ns must be callable')
        self.policy = policy
        self._clock_ns = clock_ns
        self._policies = {
            item.component_id: item for item in policy.components
        }
        self._accepted: dict[str, _AcceptedHeartbeat] = {}
        self._generation = 0
        self._semantic_signature: tuple[object, ...] | None = None
        self.accepted_total = 0
        self.duplicate_total = 0
        self.rejected_total = 0
        self.rejection_reasons: dict[str, int] = {}

    def _reject(self, reason: str) -> bool:
        self.rejected_total += 1
        self.rejection_reasons[reason] = (
            self.rejection_reasons.get(reason, 0) + 1
        )
        return False

    def ingest(
        self,
        heartbeat: ComponentHeartbeat,
        *,
        received_at_ns: int | None = None,
    ) -> bool:
        if not isinstance(heartbeat, ComponentHeartbeat):
            return self._reject('component_heartbeat_invalid')
        now_ns = self._clock_ns() if received_at_ns is None else received_at_ns
        if isinstance(now_ns, bool) or not isinstance(now_ns, int) or now_ns <= 0:
            return self._reject('component_heartbeat_receipt_time_invalid')
        policy = self._policies.get(heartbeat.component_id)
        if policy is None:
            return self._reject('component_heartbeat_unknown_component')
        if (
            heartbeat.component_role != policy.component_role
            or heartbeat.cell_id != self.policy.cell_id
            or heartbeat.robot_id not in self.policy.robot_ids
        ):
            return self._reject('component_heartbeat_identity_mismatch')
        current = self._accepted.get(heartbeat.component_id)
        if current is not None and heartbeat.component_epoch == (
            current.heartbeat.component_epoch
        ):
            if heartbeat.sequence < current.heartbeat.sequence:
                return self._reject('component_heartbeat_sequence_regressed')
            if heartbeat.sequence == current.heartbeat.sequence:
                if heartbeat == current.heartbeat:
                    self.duplicate_total += 1
                    return False
                return self._reject('component_heartbeat_sequence_conflict')
            if (
                heartbeat.critical_progress_sequence
                < current.heartbeat.critical_progress_sequence
            ):
                return self._reject(
                    'component_critical_progress_sequence_regressed'
                )
            critical_progress_received_at_ns = (
                now_ns
                if heartbeat.critical_progress_sequence
                > current.heartbeat.critical_progress_sequence
                else current.critical_progress_received_at_ns
            )
        else:
            critical_progress_received_at_ns = now_ns
        self._accepted[heartbeat.component_id] = _AcceptedHeartbeat(
            heartbeat=heartbeat,
            received_at_ns=now_ns,
            critical_progress_received_at_ns=critical_progress_received_at_ns,
        )
        self.accepted_total += 1
        return True

    def snapshot(self, now_ns: int | None = None) -> CellSupervisionSnapshot:
        evaluated_at_ns = self._clock_ns() if now_ns is None else now_ns
        if (
            isinstance(evaluated_at_ns, bool)
            or not isinstance(evaluated_at_ns, int)
            or evaluated_at_ns <= 0
        ):
            raise ValueError('snapshot time must be a positive integer')
        records: dict[str, ComponentRecord] = {}
        for policy in self.policy.components:
            accepted = self._accepted.get(policy.component_id)
            reasons: list[str] = []
            if accepted is None:
                records[policy.component_id] = ComponentRecord(
                    component_role=policy.component_role,
                    component_id=policy.component_id,
                    component_epoch='',
                    sequence=0,
                    heartbeat_age_ns=None,
                    critical_progress_sequence=0,
                    critical_progress_age_ns=None,
                    health_state=ComponentHealthState.LOST,
                    healthy=False,
                    dependency_epochs=MappingProxyType({}),
                    reason_codes=('component_heartbeat_missing',),
                    restart_domain=policy.restart_domain,
                    runtime_generation='',
                    binding_generation='',
                    configuration_generation='',
                )
                continue
            heartbeat = accepted.heartbeat
            heartbeat_age_ns = max(
                0, evaluated_at_ns - accepted.received_at_ns
            )
            progress_age_ns = max(
                0,
                evaluated_at_ns
                - accepted.critical_progress_received_at_ns,
            )
            if accepted.received_at_ns > evaluated_at_ns:
                reasons.append('component_heartbeat_receipt_in_future')
            if heartbeat_age_ns > policy.maximum_heartbeat_age_ns:
                reasons.append('component_heartbeat_stale')
            if progress_age_ns > policy.maximum_critical_progress_age_ns:
                reasons.append('component_critical_progress_stale')
            if heartbeat.health_state != ComponentHealthState.HEALTHY:
                reasons.append(
                    f'component_state_{heartbeat.health_state.value}'
                )
            for expected, observed, reason in (
                (
                    policy.expected_runtime_generation,
                    heartbeat.runtime_generation,
                    'component_runtime_generation_mismatch',
                ),
                (
                    policy.expected_binding_generation,
                    heartbeat.binding_generation,
                    'component_binding_generation_mismatch',
                ),
                (
                    policy.expected_configuration_generation,
                    heartbeat.configuration_generation,
                    'component_configuration_generation_mismatch',
                ),
            ):
                if expected and observed != expected:
                    reasons.append(reason)
            records[policy.component_id] = ComponentRecord(
                component_role=policy.component_role,
                component_id=policy.component_id,
                component_epoch=heartbeat.component_epoch,
                sequence=heartbeat.sequence,
                heartbeat_age_ns=heartbeat_age_ns,
                critical_progress_sequence=(
                    heartbeat.critical_progress_sequence
                ),
                critical_progress_age_ns=progress_age_ns,
                health_state=heartbeat.health_state,
                healthy=not reasons,
                dependency_epochs=heartbeat.dependency_epochs,
                reason_codes=tuple(sorted(set(reasons))),
                restart_domain=policy.restart_domain,
                runtime_generation=heartbeat.runtime_generation,
                binding_generation=heartbeat.binding_generation,
                configuration_generation=heartbeat.configuration_generation,
            )

        # The manifest is acyclic, so repeatedly applying dependency evidence
        # converges in at most component-count iterations without relying on
        # manifest ordering.
        for _ in range(len(records)):
            changed = False
            for policy in self.policy.components:
                record = records[policy.component_id]
                reasons = set(record.reason_codes)
                for dependency_id in policy.dependency_ids:
                    dependency = records[dependency_id]
                    expected_epoch = record.dependency_epochs.get(
                        dependency_id
                    )
                    if not dependency.healthy:
                        reasons.add(
                            f'component_dependency_unhealthy:{dependency_id}'
                        )
                    if (
                        not expected_epoch
                        or expected_epoch != dependency.component_epoch
                    ):
                        reasons.add(
                            f'component_dependency_epoch_mismatch:{dependency_id}'
                        )
                next_healthy = not reasons
                next_reasons = tuple(sorted(reasons))
                if (
                    next_healthy != record.healthy
                    or next_reasons != record.reason_codes
                ):
                    records[policy.component_id] = ComponentRecord(
                        component_role=record.component_role,
                        component_id=record.component_id,
                        component_epoch=record.component_epoch,
                        sequence=record.sequence,
                        heartbeat_age_ns=record.heartbeat_age_ns,
                        critical_progress_sequence=(
                            record.critical_progress_sequence
                        ),
                        critical_progress_age_ns=(
                            record.critical_progress_age_ns
                        ),
                        health_state=record.health_state,
                        healthy=next_healthy,
                        dependency_epochs=record.dependency_epochs,
                        reason_codes=next_reasons,
                        restart_domain=record.restart_domain,
                        runtime_generation=record.runtime_generation,
                        binding_generation=record.binding_generation,
                        configuration_generation=(
                            record.configuration_generation
                        ),
                    )
                    changed = True
            if not changed:
                break

        ordered = tuple(records[item.component_id] for item in self.policy.components)
        required = tuple(
            records[item.component_id]
            for item in self.policy.components
            if item.required_for_dispatch
        )
        unhealthy_required = tuple(item for item in required if not item.healthy)
        reason_codes = tuple(
            sorted(
                {
                    f'{record.component_id}:{reason}'
                    for record in unhealthy_required
                    for reason in record.reason_codes
                }
            )
        )[:_MAX_REASONS]
        authority = next(
            (
                item
                for item in ordered
                if item.component_role == ComponentRole.CONTROL_AUTHORITY
            ),
            None,
        )
        authority_epoch = (
            authority.component_epoch if authority is not None else ''
        )
        restart_domains = tuple(
            sorted(
                {
                    record.restart_domain
                    for record in unhealthy_required
                    if record.restart_domain
                }
            )
        )
        healthy = not unhealthy_required
        signature = (
            tuple(
                (
                    item.component_id,
                    item.component_epoch,
                    item.health_state.value,
                    item.healthy,
                    item.reason_codes,
                    tuple(item.dependency_epochs.items()),
                    item.runtime_generation,
                    item.binding_generation,
                    item.configuration_generation,
                )
                for item in ordered
            ),
            healthy,
            reason_codes,
            restart_domains,
            authority_epoch,
        )
        if signature != self._semantic_signature:
            self._generation += 1
            self._semantic_signature = signature
        return CellSupervisionSnapshot(
            generation=self._generation,
            evaluated_at_ns=evaluated_at_ns,
            cell_id=self.policy.cell_id,
            robot_ids=self.policy.robot_ids,
            authority_profile=self.policy.authority_profile,
            authority_component_epoch=authority_epoch,
            components=ordered,
            dependency_graph_healthy=healthy,
            dispatch_admitted=healthy,
            active_environment_healthy=healthy,
            reason_codes=reason_codes,
            requested_containment=(
                'none' if healthy else 'fence_stop_and_retire_execution'
            ),
            restart_domains=restart_domains,
        )

    def mint_execution_pin(
        self,
        *,
        snapshot: CellSupervisionSnapshot,
        execution_id: str,
        program_run_id: str,
        robot_id: str,
        program_generation: str,
        deployment_generation: str,
        runtime_generation: str,
        controller_binding_generation: str,
        lifecycle_generation: int,
        admission_generation: int,
    ) -> ExecutionEnvironmentPin:
        if not snapshot.dispatch_admitted:
            raise RuntimeError('execution_environment_not_admitted')
        if snapshot.cell_id != self.policy.cell_id:
            raise RuntimeError('execution_environment_cell_mismatch')
        if robot_id not in self.policy.robot_ids:
            raise RuntimeError('execution_environment_robot_mismatch')
        epochs = {
            item.component_id: item.component_epoch
            for item in snapshot.components
            if self._policies[item.component_id].required_for_dispatch
        }
        return ExecutionEnvironmentPin(
            execution_id=execution_id,
            program_run_id=program_run_id,
            robot_id=robot_id,
            cell_id=snapshot.cell_id,
            program_generation=program_generation,
            deployment_generation=deployment_generation,
            runtime_generation=runtime_generation,
            controller_binding_generation=controller_binding_generation,
            authority_profile=snapshot.authority_profile,
            authority_component_epoch=(
                snapshot.authority_component_epoch
            ),
            component_epochs=epochs,
            lifecycle_generation=lifecycle_generation,
            admission_generation=admission_generation,
            supervision_generation=snapshot.generation,
        )

    def validate_pin(
        self,
        pin: ExecutionEnvironmentPin,
        *,
        snapshot: CellSupervisionSnapshot | None = None,
    ) -> PinValidation:
        if not isinstance(pin, ExecutionEnvironmentPin):
            raise TypeError('pin must be an ExecutionEnvironmentPin')
        current = snapshot or self.snapshot()
        reasons: set[str] = set()
        if not current.active_environment_healthy:
            reasons.add('execution_environment_unhealthy')
        if pin.cell_id != current.cell_id:
            reasons.add('execution_environment_cell_changed')
        if pin.authority_profile != current.authority_profile:
            reasons.add('execution_environment_authority_profile_changed')
        if (
            pin.authority_component_epoch
            != current.authority_component_epoch
        ):
            reasons.add('execution_environment_authority_epoch_changed')
        current_epochs = {
            item.component_id: item.component_epoch
            for item in current.components
            if self._policies[item.component_id].required_for_dispatch
        }
        for component_id, expected_epoch in pin.component_epochs.items():
            if current_epochs.get(component_id) != expected_epoch:
                reasons.add(
                    f'execution_environment_component_epoch_changed:{component_id}'
                )
        if set(current_epochs) != set(pin.component_epochs):
            reasons.add('execution_environment_component_set_changed')
        return PinValidation(
            valid=not reasons,
            reason_codes=tuple(sorted(reasons)),
        )
