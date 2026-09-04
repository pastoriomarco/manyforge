from __future__ import annotations

import asyncio
from dataclasses import dataclass
from typing import Any

import pytest

from manyforge_headless_api.control_dispatch import HeadlessExecutionControlDispatch
from manyforge_headless_api.control_protocol import (
    EXECUTION_COMMAND_SCHEMA,
    MAXIMUM_U64,
    ControlFreshnessAuthority,
    ControlProtocolError,
    ExecutionCommandRequestDTO,
    ExecutionIngressResult,
    ExecutionIngressStamp,
)
from manyforge_headless_api.execution_control import ExecutionControlIngress


@dataclass
class _Clock:
    now_ns: int = 1_000_000_000

    def __call__(self) -> int:
        return self.now_ns


def _ingress(
    clock: _Clock,
) -> tuple[ExecutionControlIngress, list[ExecutionIngressStamp]]:
    stamps: list[ExecutionIngressStamp] = []
    authority = ControlFreshnessAuthority(
        clock_ns=clock,
        receiver_incarnation="receiver.1",
        secret=b"x" * 32,
    )

    def dispatch(request, stamp):
        stamps.append(stamp)
        return (
            ExecutionIngressResult(
                disposition="received",
                code=f"{request.command_envelope.command_kind}_received",
                semantic_owner_epoch="runner.1",
                execution_control_revision=7,
                current_identity={"cycle_generation": 3},
            ),
            {"accepted": True},
        )

    return (
        ExecutionControlIngress(
            projection=lambda: ("runner.1", 7),
            dispatch=dispatch,
            authority=authority,
            clock_ns=clock,
        ),
        stamps,
    )


def _request(
    ingress: ExecutionControlIngress,
    *,
    kind: str = "program_run",
    correlation: str = "request.1",
) -> ExecutionCommandRequestDTO:
    freshness = ingress.current_freshness()
    return ExecutionCommandRequestDTO.model_validate(
        {
            "command_envelope": {
                "schema_version": EXECUTION_COMMAND_SCHEMA,
                "command_kind": kind,
                "correlation_id": correlation,
                "source_label": "test",
                "control_session_id": None,
                "client_sequence": None,
                "freshness": freshness.model_dump(mode="json"),
                "expected_semantic_owner_epoch": "runner.1",
                "expected_execution_control_revision": 7,
            },
            "payload": {},
        }
    )


def test_rest_and_local_adapters_share_one_ingress_and_result_contract() -> None:
    clock = _Clock()
    ingress, stamps = _ingress(clock)

    async def submit_rest():
        return await ingress.submit(
            _request(ingress, correlation="request.rest"),
            transport_kind="rest",
            trusted_principal_id="operator.1",
            assurance="externally_authenticated",
        )

    async def submit_local():
        return await ingress.submit(
            _request(ingress, correlation="request.local"),
            transport_kind="local",
            trusted_principal_id="plc.1",
            assurance="configured_local_adapter",
        )

    try:
        rest = asyncio.run(submit_rest())
        local = asyncio.run(submit_local())
    finally:
        ingress.close()

    assert rest.receipt.disposition == local.receipt.disposition == "received"
    assert rest.receipt.code == local.receipt.code == "program_run_received"
    assert rest.result == local.result == {"accepted": True}
    assert [stamp.transport_kind for stamp in stamps] == ["rest", "local"]
    assert stamps[0].trusted_principal_id == "operator.1"
    assert stamps[1].trusted_principal_id == "plc.1"
    assert stamps[1].received_server_sequence == (
        stamps[0].received_server_sequence + 1
    )


def test_expired_ordinary_command_fails_before_dispatch_but_stop_remains_containable() -> None:
    clock = _Clock()
    ingress, stamps = _ingress(clock)
    ordinary = _request(ingress, kind="program_run")
    stop = _request(ingress, kind="program_stop", correlation="request.stop")
    clock.now_ns += ordinary.command_envelope.freshness.valid_for_ns + 1

    async def exercise():
        with pytest.raises(ControlProtocolError) as expired:
            await ingress.submit(ordinary, transport_kind="local")
        contained = await ingress.submit(stop, transport_kind="local")
        return expired, contained

    try:
        expired, contained = asyncio.run(exercise())
    finally:
        ingress.close()

    assert expired.value.code == "control_freshness_expired"
    assert contained.receipt.code == "program_stop_received"
    assert len(stamps) == 1
    assert stamps[0].transport_kind == "local"


def test_stop_is_stamped_at_lane_admission_without_a_program_projection_read() -> None:
    clock = _Clock()
    projection_calls = 0
    stamps: list[ExecutionIngressStamp] = []
    authority = ControlFreshnessAuthority(
        clock_ns=clock,
        receiver_incarnation="receiver.1",
        secret=b"x" * 32,
    )

    def projection() -> tuple[str, int]:
        nonlocal projection_calls
        projection_calls += 1
        clock.now_ns += 100_000_000
        return "runner.1", 7

    def dispatch(_request, stamp):
        stamps.append(stamp)
        return (
            ExecutionIngressResult(
                disposition="received",
                code="program_stop_received",
                semantic_owner_epoch="runner.1",
                execution_control_revision=7,
            ),
            None,
        )

    ingress = ExecutionControlIngress(
        projection=projection,
        dispatch=dispatch,
        authority=authority,
        clock_ns=clock,
    )
    stop = _request(ingress, kind="program_stop", correlation="request.stop")
    projection_calls = 0
    clock.now_ns = 2_000_000_000

    try:
        submission = asyncio.run(ingress.submit(stop, transport_kind="websocket"))
    finally:
        ingress.close()

    assert submission.receipt.disposition == "received"
    assert projection_calls == 0
    assert stamps[0].received_monotonic_ns == 2_000_000_000
    assert submission.receipt.received_monotonic_ns == "2000000000"


def test_completed_commands_leave_no_live_receipt_history_or_count_limit() -> None:
    clock = _Clock()
    ingress, stamps = _ingress(clock)
    async def exercise() -> None:
        for index in range(2_048):
            submission = await ingress.submit(
                _request(ingress, correlation=f"request.{index + 1}"),
                transport_kind="local",
            )
            assert submission.receipt.disposition == "received"

    try:
        asyncio.run(exercise())
    finally:
        ingress.close()

    assert len(stamps) == 2_048
    assert not hasattr(ingress, "_receipts")
    assert not hasattr(ingress, "_request_ledger")


def test_control_sequences_rotate_together_only_after_the_last_session_closes() -> None:
    clock = _Clock()
    ingress, _stamps = _ingress(clock)
    original_incarnation = ingress.receiver_incarnation
    ingress.open_control_session()
    ingress._authority._sequence = MAXIMUM_U64  # noqa: SLF001
    ingress._received_sequence = MAXIMUM_U64  # noqa: SLF001

    try:
        with pytest.raises(ControlProtocolError) as blocked:
            ingress.current_freshness()
        assert blocked.value.code == "control_sequence_rollover_required"
        assert ingress.receiver_incarnation == original_incarnation

        ingress.close_control_session()
        assert ingress.active_control_sessions == 0
        assert ingress.receiver_incarnation != original_incarnation
        assert ingress._received_sequence == 0  # noqa: SLF001
        assert ingress.current_freshness().issued_server_sequence == "1"
    finally:
        ingress.close()


def test_native_command_uses_projected_execution_owner_fence_and_surfaces_rejection() -> None:
    class Service:
        def __init__(self, result: dict[str, Any]) -> None:
            self.result = result
            self.request = None

        @staticmethod
        def program_cycle_status() -> dict[str, Any]:
            return {
                "program_runner_owner_epoch": "runner.1",
                "execution_control_revision": 7,
                "cycle_generation": 3,
                "program_run_id": None,
                "state": "idle_after_reset",
                "allowed_commands": [
                    {
                        "command_kind": "runtime_scale",
                        "allowed": True,
                        "disabled_reason": None,
                    }
                ],
            }

        def execution_set_runtime_scale_control(self, request, attribution):
            del attribution
            self.request = request
            return self.result

    def command() -> ExecutionCommandRequestDTO:
        ingress, _stamps = _ingress(_Clock())
        try:
            request = _request(
                ingress,
                kind="runtime_scale",
                correlation="request.scale",
            )
        finally:
            ingress.close()
        return request.model_copy(
            update={
                "payload": {
                    "expected_execution_owner_epoch": "kernel.epoch.4",
                    "request_context": {
                        "request_id": "request.scale.intent",
                        "request_owner_epoch": "kernel.epoch.4",
                        "source_label": "test",
                        "correlation_id": "request.scale",
                    },
                    "selector": {
                        "program_binding_id": "binding.1",
                        "component_role_id": "controlled_component",
                        "target_role_id": "command_target",
                    },
                    "expected_control_generation": 0,
                    "scale": 0.25,
                }
            }
        )

    stamp = ExecutionIngressStamp(
        transport_kind="websocket",
        receiver_incarnation="receiver.1",
        received_server_sequence=1,
        received_monotonic_ns=1,
    )
    accepted_service = Service({"ok": True, "accepted": {}})
    accepted, _ = HeadlessExecutionControlDispatch(accepted_service)(  # type: ignore[arg-type]
        command(), stamp
    )
    assert accepted.disposition == "received"
    assert accepted_service.request.request_context.request_owner_epoch == (
        "kernel.epoch.4"
    )
    assert accepted_service.request.request_context.request_id == (
        "request.scale.intent"
    )
    assert accepted_service.request.request_context.correlation_id == (
        "request.scale"
    )

    rejected_service = Service(
        {
            "ok": False,
            "error": {"code": "stale_request_owner_epoch", "message": "stale"},
        }
    )
    rejected, result = HeadlessExecutionControlDispatch(rejected_service)(  # type: ignore[arg-type]
        command(), stamp
    )
    assert rejected.disposition == "rejected"
    assert rejected.code == "stale_request_owner_epoch"
    assert result == rejected_service.result


def test_standalone_native_control_does_not_require_a_program_runner() -> None:
    class Service:
        program_runner = None

        def __init__(self) -> None:
            self.request = None

        @staticmethod
        def execution_identity() -> dict[str, Any]:
            return {
                "ok": True,
                "identity": {"owner_epoch": "kernel.epoch.standalone"},
            }

        def execution_set_runtime_scale_control(self, request, attribution):
            del attribution
            self.request = request
            return {"ok": True, "accepted": {}}

        @staticmethod
        def program_cycle_status() -> dict[str, Any]:
            raise AssertionError("standalone native control read Program Runner")

    clock = _Clock()
    service = Service()
    dispatch = HeadlessExecutionControlDispatch(service)  # type: ignore[arg-type]
    ingress = ExecutionControlIngress(
        projection=dispatch.owner_projection,
        dispatch=dispatch,
        authority=ControlFreshnessAuthority(
            clock_ns=clock,
            receiver_incarnation="receiver.standalone",
            secret=b"x" * 32,
        ),
        clock_ns=clock,
    )
    freshness = ingress.current_freshness()
    command = ExecutionCommandRequestDTO.model_validate(
        {
            "command_envelope": {
                "schema_version": EXECUTION_COMMAND_SCHEMA,
                "command_kind": "runtime_scale",
                "correlation_id": "request.scale.standalone",
                "source_label": "test",
                "control_session_id": None,
                "client_sequence": None,
                "freshness": freshness.model_dump(mode="json"),
                "expected_semantic_owner_epoch": "kernel.epoch.standalone",
                "expected_execution_control_revision": 1,
            },
            "payload": {
                "expected_execution_owner_epoch": "kernel.epoch.standalone",
                "selector": {
                    "program_binding_id": "binding.1",
                    "component_role_id": "controlled_component",
                    "target_role_id": "command_target",
                },
                "expected_control_generation": 0,
                "scale": 0.5,
            },
        }
    )

    try:
        submission = asyncio.run(
            ingress.submit(command, transport_kind="rest")
        )
    finally:
        ingress.close()

    assert submission.receipt.disposition == "received"
    assert submission.receipt.semantic_owner_epoch == "kernel.epoch.standalone"
    assert service.request.request_context.request_owner_epoch == (
        "kernel.epoch.standalone"
    )
