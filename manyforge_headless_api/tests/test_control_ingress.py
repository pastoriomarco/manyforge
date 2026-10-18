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


def test_expired_ordinary_command_fails_before_dispatch_but_stop_remains_containable() -> (
    None
):
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
    assert stamps[0].valid_until_monotonic_ns is None


def test_forwarded_deadline_preserves_authenticated_issue_time_not_receipt_time() -> (
    None
):
    clock = _Clock()
    ingress, stamps = _ingress(clock)
    request = _request(ingress)
    issued_at = clock.now_ns
    clock.now_ns += 200_000_000
    try:
        asyncio.run(ingress.submit(request, transport_kind="rest"))
    finally:
        ingress.close()
    assert stamps[0].received_monotonic_ns == clock.now_ns
    assert stamps[0].valid_until_monotonic_ns == (
        issued_at + request.command_envelope.freshness.valid_for_ns
    )


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


def test_program_stop_dispatch_never_resamples_program_projection() -> None:
    class Service:
        program_runner = object()

        def __init__(self) -> None:
            self.request = None

        @staticmethod
        def program_cycle_status() -> dict[str, Any]:
            raise AssertionError("recognized Stop synchronously read Program state")

        def program_run_motion_stop(self, program_run_id, request):
            self.request = request
            assert program_run_id == "program-run.1"
            return {"accepted": True}

    clock = _Clock()
    ingress, _stamps = _ingress(clock)
    try:
        request = _request(
            ingress,
            kind="program_motion_stop",
            correlation="request.motion-stop",
        ).model_copy(
            update={
                "payload": {
                    "request_id": "request.motion-stop",
                    "program_run_id": "program-run.1",
                    "expected_owner_epoch": "runner.1",
                    "reason": "operator Stop",
                }
            }
        )
    finally:
        ingress.close()
    service = Service()
    disposition, result = HeadlessExecutionControlDispatch(service)(  # type: ignore[arg-type]
        request,
        ExecutionIngressStamp(
            transport_kind="websocket",
            receiver_incarnation="receiver.1",
            received_server_sequence=1,
            received_monotonic_ns=1,
        ),
    )

    assert disposition.disposition == "received"
    assert disposition.semantic_owner_epoch == "runner.1"
    assert disposition.execution_control_revision == 7
    assert disposition.current_identity is None
    assert result == {"accepted": True}
    assert service.request is not None


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


def test_native_command_uses_projected_execution_owner_fence_and_surfaces_rejection() -> (
    None
):
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
    assert accepted_service.request.request_context.correlation_id == ("request.scale")

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


@pytest.mark.parametrize(
    "kind,case",
    [
        ("operation_reset", "current"),
        ("operation_acknowledge_completed", "current"),
        ("operation_reset", "other_operation"),
        ("operation_reset", "old_revision"),
        ("operation_reset", "old_owner"),
        ("operation_reset", "retired"),
        ("operation_reset", "not_allowed"),
        ("operation_reset", "recovery"),
    ],
)
def test_running_program_controls_only_exact_native_manual_operation(kind, case):
    calls = []

    class Service:
        @staticmethod
        def program_cycle_status():
            return {
                "program_runner_owner_epoch": "runner.1",
                "execution_control_revision": 7,
                "cycle_generation": 3,
                "program_run_id": "program.1",
                "state": "recovery_required" if case == "recovery" else "running",
                "allowed_commands": [
                    {
                        "command_kind": kind,
                        "allowed": case != "not_allowed",
                        "disabled_reason": "program_cycle_state_resetting",
                    }
                ],
            }

        @staticmethod
        def execution_control_state():
            calls.append("projection")
            return {
                "identity": {
                    "owner_epoch": "old" if case == "old_owner" else "kernel.1"
                },
                "control_state": {
                    "schema_version": "manyforge.execution_control_state.v6",
                    "manual_operations": [
                        {
                            "resident": case != "retired",
                            "operation": {
                                "operation_id": (
                                    "other" if case == "other_operation" else "manual.1"
                                ),
                                "operation_revision": (
                                    2 if case == "old_revision" else 3
                                ),
                            },
                        }
                    ],
                },
            }

        @staticmethod
        def execute_control(request, attribution):
            del attribution
            calls.append("effect")
            assert request.operation_id == "manual.1"
            assert request.expected_operation_revision == 3
            assert request.request_context.request_owner_epoch == "kernel.1"
            return {"ok": True, "accepted": {}}

        execution_reset = execution_acknowledge_completed = execute_control

    ingress, _ = _ingress(_Clock())
    try:
        request = _request(ingress, kind=kind).model_copy(
            update={
                "payload": {
                    "expected_execution_owner_epoch": "kernel.1",
                    "operation_id": "manual.1",
                    "expected_operation_revision": 3,
                }
            }
        )
    finally:
        ingress.close()
    result, _ = HeadlessExecutionControlDispatch(Service())(
        request,
        ExecutionIngressStamp(
            transport_kind="websocket",
            receiver_incarnation="receiver.1",
            received_server_sequence=1,
            received_monotonic_ns=1,
        ),
    )
    if case in {"current", "recovery"}:
        assert result.disposition == "received"
        assert calls == ["projection", "effect"]
    else:
        assert result.disposition == "rejected"
        assert calls == ([] if case == "not_allowed" else ["projection"])


@pytest.mark.parametrize("context_present", [False, True])
def test_held_manual_start_requires_context_and_native_decision(context_present):
    from manyforge_headless_api.tests.test_execution_events import (
        _direct_command_request,
    )

    submitted = []

    class Service:
        @staticmethod
        def program_cycle_status():
            return {
                "program_runner_owner_epoch": "runner.1",
                "execution_control_revision": 7,
                "cycle_generation": 3,
                "program_run_id": "program.1",
                "state": "running",
                "allowed_commands": [
                    {
                        "command_kind": "operation_start",
                        "allowed": True,
                        "disabled_reason": None,
                    }
                ],
            }

        @staticmethod
        def execution_submit(request, attribution):
            del attribution
            submitted.append(request)
            # A copied context is not permission. The native owner's exact
            # rejection must survive dispatch rather than become acceptance.
            return {"ok": False, "error": {"code": "manual_handover_scope_stale"}}

    payload = _direct_command_request().model_dump(mode="json")
    payload["expected_execution_owner_epoch"] = "kernel.1"
    if context_present:
        payload["manual_handover"] = {
            "program_runner_owner_epoch": "runner.1",
            "program_run_id": "program.1",
            "claim_generation": 1,
            "selection_revision": 0,
        }
    ingress, _ = _ingress(_Clock())
    try:
        request = _request(ingress, kind="operation_start").model_copy(
            update={"payload": payload}
        )
    finally:
        ingress.close()
    result, _ = HeadlessExecutionControlDispatch(Service())(
        request,
        ExecutionIngressStamp(
            transport_kind="websocket",
            receiver_incarnation="receiver.1",
            received_server_sequence=1,
            received_monotonic_ns=1,
        ),
    )
    assert result.disposition == "rejected"
    assert result.code == (
        "manual_handover_scope_stale" if context_present else "manual_handover_required"
    )
    assert len(submitted) == int(context_present)


@pytest.mark.parametrize("native_ok", [True, False])
def test_idle_manual_gripper_start_during_cell_stop_forwards_exact_native_decision(native_ok):
    from manyforge_headless_api.tests.test_execution_events import _direct_command_request

    submitted = []
    native_result = (
        {"ok": True, "accepted": {"operation_id": "manual.pneumatic"}}
        if native_ok else {"ok": False, "error": {"code": "component_not_ready"}}
    )

    class Service:
        @staticmethod
        def program_cycle_status():
            return {
                "program_runner_owner_epoch": "runner.1",
                "execution_control_revision": 7,
                "cycle_generation": 3,
                "program_run_id": None,
                "state": "idle_after_reset",
                "allowed_commands": [
                    {"command_kind": "operation_start", "allowed": True,
                     "disabled_reason": None}
                ],
                "runtime_controls": {
                    "execution_selections": {
                        "groups": [{"group_id": "cell", "state": "stopping"}]
                    }
                },
            }

        @staticmethod
        def execution_submit(request, attribution):
            submitted.append((request, attribution))
            return native_result

    payload = _direct_command_request().model_dump(mode="json")
    payload["expected_execution_owner_epoch"] = "kernel.1"
    ingress, _ = _ingress(_Clock())
    try:
        request = _request(ingress, kind="operation_start").model_copy(
            update={"payload": payload}
        )
    finally:
        ingress.close()
    result, response = HeadlessExecutionControlDispatch(Service())(
        request,
        ExecutionIngressStamp(
            transport_kind="websocket", receiver_incarnation="receiver.1",
            received_server_sequence=1, received_monotonic_ns=1,
        ),
    )
    assert len(submitted) == 1
    forwarded = submitted[0][0].model_dump(mode="json")
    assert forwarded["participants"] == payload["participants"]
    for key in (
        "program_id", "program_revision", "program_binding_id", "program_run_id",
        "expected_graph_revision", "expected_retained_bundle_id", "expected_scene_revision",
    ):
        assert forwarded[key] == payload[key]
    assert forwarded.get("manual_handover") is None
    assert forwarded["request_context"]["request_owner_epoch"] == "kernel.1"
    assert response is native_result
    assert result.disposition == ("received" if native_ok else "rejected")
    assert result.code == (
        "operation_start_received" if native_ok else "component_not_ready"
    )


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
        submission = asyncio.run(ingress.submit(command, transport_kind="rest"))
    finally:
        ingress.close()

    assert submission.receipt.disposition == "received"
    assert submission.receipt.semantic_owner_epoch == "kernel.epoch.standalone"
    assert service.request.request_context.request_owner_epoch == (
        "kernel.epoch.standalone"
    )


@pytest.mark.parametrize("accepted", [(False, False), (True, False), (False, True), (True, True)])
@pytest.mark.parametrize("requested", [(False, False), (True, False), (False, True), (True, True)])
def test_reset_join_requires_exact_accepted_options(accepted, requested):
    clock = _Clock()
    ingress, _stamps = _ingress(clock)
    try:
        request = _request(ingress, kind="program_reset").model_copy(update={"payload": {
            "program_run_id": None, "expected_owner_epoch": "runner.1", "expected_cycle_generation": 3,
            "reload_initial_scene": requested[0], "reset_robot_position": requested[1],
        }})
        cycle = {"program_runner_owner_epoch": "runner.1", "execution_control_revision": 7,
                 "cycle_generation": 3, "program_run_id": None, "state": "resetting",
                 "reset_options": {"reload_initial_scene": accepted[0], "reset_robot_position": accepted[1]}}
        disposition, projection = HeadlessExecutionControlDispatch(object())._reduce_unavailable(request, cycle, None)
        assert disposition.disposition == ("joined" if accepted == requested else "rejected")
        assert disposition.code == ("program_reset_already_current" if accepted == requested else "program_run_request_conflict")
        assert projection == {"program_cycle": cycle}
    finally:
        ingress.close()


def _held_output_reset(payload_patch: dict[str, Any] | None = None) -> ExecutionCommandRequestDTO:
    ingress, _ = _ingress(_Clock())
    try:
        request = _request(ingress, kind="operation_reset")
    finally:
        ingress.close()
    payload = {
        "operation_id": "op.output",
        "expected_operation_revision": 4,
        "reason": "operator Reset acknowledging outputs with unknown acceptance",
        "expected_execution_owner_epoch": "kernel.1",
        "acknowledged_uncertain_outputs": [{"operation_id": "op.output", "operation_revision": 4}],
        **(payload_patch or {}),
    }
    return request.model_copy(update={"payload": {k: v for k, v in payload.items() if v is not None}})


class _HeldOutputService:
    def __init__(self, **operation_patch: Any) -> None:
        self.reads: list[str] = []
        self.resident = operation_patch.pop("resident", True)
        self.operation = {
            "operation_id": "op.output",
            "operation_revision": 4,
            "program_run_id": "program-cycle.3",
            "participants": [{"participant_id": "p", "output_evidence": "acceptance_unknown"}],
            **operation_patch,
        }

    @staticmethod
    def execution_control_state() -> dict[str, Any]:
        return {
            "identity": {"owner_epoch": "kernel.1"},
            "control_state": {
                "schema_version": "manyforge.execution_control_state.v6",
                "manual_operations": [],
            },
        }

    def execution_read(self, operation_id: str) -> dict[str, Any]:
        self.reads.append(operation_id)
        return {"ok": True, "command": "read", "resident": self.resident, "operation": self.operation}


_HELD_CYCLE = {"state": "intervention_required", "program_run_id": "program-cycle.3"}


def test_operator_reset_of_a_held_program_uncertain_output_is_qualified() -> None:
    service = _HeldOutputService()
    dispatch = HeadlessExecutionControlDispatch(service)  # type: ignore[arg-type]
    assert dispatch._manual_scope_conflict(_held_output_reset(), _HELD_CYCLE) is None  # noqa: SLF001
    assert service.reads == ["op.output"]


@pytest.mark.parametrize(
    ("service", "payload", "cycle"),
    [
        # Not acknowledged, or acknowledging another revision.
        ({}, {"acknowledged_uncertain_outputs": None}, _HELD_CYCLE),
        ({}, {"acknowledged_uncertain_outputs": [{"operation_id": "op.output", "operation_revision": 3}]},
         _HELD_CYCLE),
        # Another revision, not resident, another Run, a known output, two participants.
        ({"operation_revision": 5}, {}, _HELD_CYCLE),
        ({"resident": False}, {}, _HELD_CYCLE),
        ({"program_run_id": "program-cycle.2"}, {}, _HELD_CYCLE),
        ({"participants": [{"output_evidence": "source_accepted"}]}, {}, _HELD_CYCLE),
        ({"participants": [{"output_evidence": "acceptance_unknown"}, {}]}, {}, _HELD_CYCLE),
        # A motion operation (no output evidence) of the held Run: no general bypass.
        ({"participants": [{"participant_id": "arm"}]}, {}, _HELD_CYCLE),
        # No current Run, or an execution scope.
        ({}, {}, {"state": "intervention_required", "program_run_id": None}),
        ({}, {"execution_scope": {"kind": "program"}}, _HELD_CYCLE),
    ],
)
def test_other_held_program_operations_stay_refused(service, payload, cycle) -> None:
    dispatch = HeadlessExecutionControlDispatch(_HeldOutputService(**service))  # type: ignore[arg-type]
    assert dispatch._manual_scope_conflict(  # noqa: SLF001
        _held_output_reset(payload), cycle
    ) == "manual_operation_scope_stale"


def test_acknowledge_completed_never_uses_the_uncertain_output_gate() -> None:
    service = _HeldOutputService()
    request = _held_output_reset()
    request = request.model_copy(update={"command_envelope": request.command_envelope.model_copy(
        update={"command_kind": "operation_acknowledge_completed"})})
    dispatch = HeadlessExecutionControlDispatch(service)  # type: ignore[arg-type]
    assert dispatch._manual_scope_conflict(request, _HELD_CYCLE) == "manual_operation_scope_stale"  # noqa: SLF001
    assert service.reads == []
