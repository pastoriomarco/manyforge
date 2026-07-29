from __future__ import annotations

from dataclasses import dataclass

from manyforge_ros.execution_admission import _ingress_node_name
from manyforge_ros.execution_admission import EXECUTION_ADMISSION_SCHEMA
from manyforge_ros.execution_admission import ExecutionAdmissionIngress
from manyforge_ros.execution_admission import (
    ExecutionAdmissionIngressConfig,
)
import pytest


@dataclass
class Message:
    schema: str = EXECUTION_ADMISSION_SCHEMA
    provider_instance_id: str = 'lite6-a:provider:session-1'
    robot_id: str = 'lite6-a'
    binding_contract_generation_id: str = 'binding-1'
    runtime_generation_id: str = 'runtime-1'
    provider_component_epoch: str = 'provider-epoch-1'
    transport_owner_component_epoch: str = 'owner-epoch-1'
    authority_component_epoch: str = 'authority-epoch-1'
    cell_supervisor_component_epoch: str = 'supervisor-epoch-1'
    publication_generation: int = 1
    lifecycle_generation: int = 1
    source_snapshot_generation: int = 10
    projected_at_steady_time_ns: int = 900
    lease_duration_ns: int = 250
    controller_ready: bool = True
    motion_admitted: bool = True
    reason_codes: tuple[str, ...] = ()


def _ingress() -> ExecutionAdmissionIngress:
    return ExecutionAdmissionIngress(
        ExecutionAdmissionIngressConfig(
            topic='/manyforge_execution_admission',
            provider_instance_id='lite6-a:provider:session-1',
            robot_id='lite6-a',
            binding_contract_generation_id='binding-1',
            runtime_generation_id='runtime-1',
            cell_supervisor_component_epoch='supervisor-epoch-1',
            maximum_lease_ns=250,
        )
    )


def test_config_rejects_values_that_would_only_work_after_coercion() -> None:
    with pytest.raises(ValueError, match='topic'):
        ExecutionAdmissionIngressConfig(
            topic=123,  # type: ignore[arg-type]
            provider_instance_id='provider-1',
            robot_id='lite6-a',
            binding_contract_generation_id='binding-1',
            runtime_generation_id='runtime-1',
            cell_supervisor_component_epoch='supervisor-epoch-1',
        )
    with pytest.raises(ValueError, match='provider_instance_id'):
        ExecutionAdmissionIngressConfig(
            topic='/admission',
            provider_instance_id='provider\u00a0one',
            robot_id='lite6-a',
            binding_contract_generation_id='binding-1',
            runtime_generation_id='runtime-1',
            cell_supervisor_component_epoch='supervisor-epoch-1',
        )


def test_ingress_node_name_is_stable_and_unique_per_exact_identity() -> None:
    first = ExecutionAdmissionIngressConfig(
        topic='/admission',
        provider_instance_id='lite6-a:provider:session-1',
        robot_id='lite6-a',
        binding_contract_generation_id='binding-1',
        runtime_generation_id='runtime-1',
        cell_supervisor_component_epoch='supervisor-epoch-1',
    )
    second = ExecutionAdmissionIngressConfig(
        topic='/admission',
        provider_instance_id='lite6-a:provider:session-2',
        robot_id='lite6-a',
        binding_contract_generation_id='binding-1',
        runtime_generation_id='runtime-1',
        cell_supervisor_component_epoch='supervisor-epoch-1',
    )

    assert _ingress_node_name(first) == _ingress_node_name(first)
    assert _ingress_node_name(first) != _ingress_node_name(second)
    assert _ingress_node_name(first).startswith('manyforge_lite6_a_')
    assert '-' not in _ingress_node_name(first)


def test_translates_exact_message_without_extending_source_lease() -> None:
    ingress = _ingress()

    snapshot = ingress.accept(Message(), 1_000)

    assert snapshot == {
        'generation': 1,
        'lifecycle_generation': 1,
        'robot_id': 'lite6-a',
        'binding_contract_generation_id': 'binding-1',
        'runtime_generation_id': 'runtime-1',
        'provider_component_epoch': 'provider-epoch-1',
        'transport_owner_component_epoch': 'owner-epoch-1',
        'authority_component_epoch': 'authority-epoch-1',
        'cell_supervisor_component_epoch': 'supervisor-epoch-1',
        'observed_timestamp_ns': 900,
        'received_timestamp_ns': 1_000,
        'fresh_until_ns': 1_150,
        'controller_ready': True,
        'motion_admitted': True,
        'reason_codes': [],
    }
    assert ingress.accepted_total == 1


def test_ignores_exact_duplicate_without_restamping_lease() -> None:
    ingress = _ingress()
    message = Message()
    assert ingress.accept(message, 1_000) is not None

    assert ingress.accept(message, 1_010) is None
    assert ingress.duplicate_total == 1
    assert ingress.rejected_total == 0


def test_native_sink_rejection_terminally_fences_the_ingress() -> None:
    ingress = _ingress()
    ingress.mark_sink_rejection()

    assert ingress.accept(Message(), 1_000) is None
    assert ingress.terminal_reason == 'execution_admission_sink_rejected'
    assert ingress.rejected_total == 2


def test_allows_lease_renewal_without_lifecycle_generation_change() -> None:
    ingress = _ingress()
    assert ingress.accept(Message(), 1_000) is not None
    renewal = Message(
        publication_generation=2,
        source_snapshot_generation=10,
        projected_at_steady_time_ns=1_050,
    )

    snapshot = ingress.accept(renewal, 1_060)

    assert snapshot is not None
    assert snapshot['lifecycle_generation'] == 1
    assert snapshot['fresh_until_ns'] == 1_300


def test_semantic_change_requires_new_lifecycle_generation() -> None:
    ingress = _ingress()
    assert ingress.accept(Message(), 1_000) is not None
    hidden_change = Message(
        publication_generation=2,
        projected_at_steady_time_ns=1_050,
        controller_ready=False,
        motion_admitted=False,
        reason_codes=('controller_not_ready',),
    )

    assert ingress.accept(hidden_change, 1_060) is None
    assert ingress.terminal_reason == (
        'execution_admission_hidden_semantic_change'
    )


def test_provider_replacement_after_acceptance_is_terminal() -> None:
    ingress = _ingress()
    assert ingress.accept(Message(), 1_000) is not None
    replacement = Message(
        provider_instance_id='lite6-a:provider:session-2',
        publication_generation=2,
        projected_at_steady_time_ns=1_050,
    )

    assert ingress.accept(replacement, 1_060) is None
    assert ingress.terminal_reason == (
        'execution_admission_provider_mismatch'
    )


@pytest.mark.parametrize(
    'message',
    [
        Message(schema='wrong'),
        Message(robot_id='other'),
        Message(binding_contract_generation_id='other'),
        Message(projected_at_steady_time_ns=1_001),
        Message(lease_duration_ns=0),
        Message(reason_codes=('b', 'a')),
        Message(reason_codes=('a', 'a')),
        Message(motion_admitted=True, controller_ready=False),
        Message(publication_generation=True),
        Message(controller_ready=1),
        Message(reason_codes=(1,)),
    ],
)
def test_rejects_malformed_or_mismatched_messages(message: Message) -> None:
    ingress = _ingress()

    assert ingress.accept(message, 1_000) is None
    assert ingress.rejected_total == 1


def test_rejects_message_stale_on_arrival() -> None:
    ingress = _ingress()

    assert ingress.accept(Message(lease_duration_ns=99), 1_000) is None
    assert ingress.terminal_reason == ''


def test_rejects_source_deadline_that_exceeds_signed_wire_range() -> None:
    ingress = _ingress()
    maximum_int64 = (1 << 63) - 1

    assert ingress.accept(
        Message(
            projected_at_steady_time_ns=maximum_int64,
            lease_duration_ns=1,
        ),
        maximum_int64,
    ) is None
    assert ingress.rejected_total == 1


def test_rejects_generation_regression_terminally() -> None:
    ingress = _ingress()
    assert ingress.accept(Message(publication_generation=2), 1_000)
    regression = Message(
        publication_generation=1,
        projected_at_steady_time_ns=950,
    )

    assert ingress.accept(regression, 1_000) is None
    assert ingress.terminal_reason == (
        'execution_admission_generation_regression'
    )
