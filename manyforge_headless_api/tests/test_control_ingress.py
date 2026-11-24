from __future__ import annotations

import asyncio
from dataclasses import dataclass
from typing import Any

import pytest

from manyforge_headless_api.control_dispatch import HeadlessExecutionControlDispatch
from manyforge_headless_api.control_protocol import (
    EXECUTION_COMMAND_SCHEMA,
    ControlFreshnessAuthority,
    ControlProtocolError,
    ExecutionCommandRequestDTO,
    ExecutionIngressResult,
    ExecutionIngressStamp,
)
from manyforge_headless_api.execution_control import ExecutionControlIngress
from manyforge_headless_api.identity import RequestAdmission

APPLICATION_START_ID = "3f9a0000000000a1"


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
        secret=b"x" * 32,
    )

    def dispatch(request, stamp):
        stamps.append(stamp)
        return (
            ExecutionIngressResult(
                disposition="received",
                code=f"{request.command_envelope.command_kind}_received",
                execution_control_revision=7,
                current_identity={"cycle_generation": 3},
            ),
            {"accepted": True},
        )

    return (
        ExecutionControlIngress(
            projection=lambda: 7,
            dispatch=dispatch,
            application_start_id=APPLICATION_START_ID,
            authority=authority,
            clock_ns=clock,
        ),
        stamps,
    )


def _request(
    ingress: ExecutionControlIngress,
    *,
    kind: str = "program_run",
    client_sequence: int = 1,
    session: bool = True,
    payload: dict[str, Any] | None = None,
) -> ExecutionCommandRequestDTO:
    """A WebSocket command of session 1 by default; ``session=False`` is the
    session-less (HTTP) form."""

    freshness = ingress.current_freshness()
    return ExecutionCommandRequestDTO.model_validate(
        {
            "command_envelope": {
                "schema_version": EXECUTION_COMMAND_SCHEMA,
                "command_kind": kind,
                "source_label": "test",
                "control_session_number": 1 if session else None,
                "client_sequence": str(client_sequence) if session else None,
                "freshness": freshness.model_dump(mode="json"),
                "expected_execution_control_revision": 7,
            },
            "payload": {} if payload is None else payload,
        }
    )


def _dispatch(service: Any) -> HeadlessExecutionControlDispatch:
    return HeadlessExecutionControlDispatch(
        service, RequestAdmission(APPLICATION_START_ID)
    )


def _stamp(transport_kind: str = "websocket") -> ExecutionIngressStamp:
    return ExecutionIngressStamp(
        transport_kind=transport_kind,  # type: ignore[arg-type]
        received_server_sequence=1,
        received_monotonic_ns=1,
    )


def test_rest_and_local_adapters_share_one_ingress_and_result_contract() -> None:
    clock = _Clock()
    ingress, stamps = _ingress(clock)

    async def submit_rest():
        return await ingress.submit(
            _request(ingress, client_sequence=1),
            transport_kind="rest",
            trusted_principal_id="operator.1",
            assurance="externally_authenticated",
        )

    async def submit_local():
        return await ingress.submit(
            _request(ingress, client_sequence=2),
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
    stop = _request(ingress, kind="program_motion_stop", client_sequence=2)
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
    assert contained.receipt.code == "program_motion_stop_received"
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


def test_freshness_is_aged_at_receipt_not_after_a_slow_owner_read() -> None:
    """OD-240: a command received in time is admitted even though the backend
    validates it after the lifetime; one received too late is refused."""

    clock = _Clock()
    stamps: list[ExecutionIngressStamp] = []
    authority = ControlFreshnessAuthority(clock_ns=clock, secret=b"x" * 32)

    def slow_projection() -> int:
        # The owner read before validation outlasts the whole lifetime.
        clock.now_ns += 2 * 750_000_000
        return 7

    def dispatch(request, stamp):
        stamps.append(stamp)
        return (
            ExecutionIngressResult(
                disposition="received",
                code="program_run_received",
                execution_control_revision=7,
                current_identity=None,
            ),
            {"accepted": True},
        )

    ingress = ExecutionControlIngress(
        projection=slow_projection,
        dispatch=dispatch,
        application_start_id=APPLICATION_START_ID,
        authority=authority,
        clock_ns=clock,
    )

    def command() -> ExecutionCommandRequestDTO:
        freshness = authority.issue(execution_control_revision=7)
        return ExecutionCommandRequestDTO.model_validate(
            {
                "command_envelope": {
                    "schema_version": EXECUTION_COMMAND_SCHEMA,
                    "command_kind": "program_run",
                    "source_label": "test",
                    "control_session_number": None,
                    "client_sequence": None,
                    "freshness": freshness.model_dump(mode="json"),
                    "expected_execution_control_revision": 7,
                },
                "payload": {},
            }
        )

    async def exercise():
        results: dict[str, Any] = {}
        # Received by Headless 749 ms after issue.
        request = command()
        clock.now_ns += 749_000_000
        results["in_time"] = await ingress.submit(request, transport_kind="rest")
        # Received by Composer 700 ms after issue, forwarded and received by
        # Headless 300 ms later.
        request = command()
        forwarded = clock.now_ns + 700_000_000
        clock.now_ns += 1_000_000_000
        results["forwarded_in_time"] = await ingress.submit(
            request, transport_kind="rest", received_monotonic_ns=forwarded
        )
        # Received by Headless 751 ms after issue.
        request = command()
        clock.now_ns += 751_000_000
        with pytest.raises(ControlProtocolError) as late:
            await ingress.submit(request, transport_kind="rest")
        results["late"] = late.value.code
        # Received by Composer 751 ms after issue.
        request = command()
        forwarded = clock.now_ns + 751_000_000
        with pytest.raises(ControlProtocolError) as late:
            await ingress.submit(
                request, transport_kind="rest", received_monotonic_ns=forwarded
            )
        results["forwarded_late"] = late.value.code
        return results

    try:
        results = asyncio.run(exercise())
    finally:
        ingress.close()

    assert results["in_time"].receipt.disposition == "received"
    assert results["forwarded_in_time"].receipt.disposition == "received"
    assert results["late"] == "control_freshness_expired"
    assert results["forwarded_late"] == "control_freshness_expired"
    assert len(stamps) == 2


def test_stop_is_stamped_at_lane_admission_without_a_program_projection_read() -> None:
    clock = _Clock()
    projection_calls = 0
    stamps: list[ExecutionIngressStamp] = []
    authority = ControlFreshnessAuthority(
        clock_ns=clock,
        secret=b"x" * 32,
    )

    def projection() -> int:
        nonlocal projection_calls
        projection_calls += 1
        clock.now_ns += 100_000_000
        return 7

    def dispatch(_request, stamp):
        stamps.append(stamp)
        return (
            ExecutionIngressResult(
                disposition="received",
                code="program_motion_stop_received",
                execution_control_revision=7,
            ),
            None,
        )

    ingress = ExecutionControlIngress(
        projection=projection,
        dispatch=dispatch,
        application_start_id=APPLICATION_START_ID,
        authority=authority,
        clock_ns=clock,
    )
    stop = _request(ingress, kind="program_motion_stop")
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
        def program_cycle_status(**_kwargs: Any) -> dict[str, Any]:
            raise AssertionError("recognized Stop synchronously read Program state")

        def program_run_motion_stop(self, request, attribution):
            del attribution
            self.request = request
            assert request.program_run_id == 1
            return {"accepted": True}

    clock = _Clock()
    ingress, _stamps = _ingress(clock)
    try:
        request = _request(
            ingress,
            kind="program_motion_stop",
            client_sequence=4,
            payload={"program_run_id": 1, "reason": "operator Stop"},
        )
    finally:
        ingress.close()
    service = Service()
    disposition, result = _dispatch(service)(request, _stamp())

    assert disposition.disposition == "received"
    assert disposition.execution_control_revision == 7
    assert disposition.current_identity is None
    assert result == {"accepted": True}
    assert service.request is not None
    # The selection Stop's key is class 0: session 1, client sequence 4.
    assert service.request.request_key == "00" + "00000000000001" + "0000000000000004"
    # A WebSocket command's receipt names its key too (OD-214).
    assert disposition.request_key == service.request.request_key
    assert disposition.command is None


def test_completed_commands_leave_no_live_receipt_history_or_count_limit() -> None:
    clock = _Clock()
    ingress, stamps = _ingress(clock)

    async def exercise() -> None:
        for index in range(2_048):
            submission = await ingress.submit(
                _request(ingress, client_sequence=index + 1),
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


def test_native_command_uses_projected_execution_owner_fence_and_surfaces_rejection() -> (
    None
):
    class Service:
        def __init__(self, result: dict[str, Any]) -> None:
            self.result = result
            self.request = None

        @staticmethod
        def program_cycle_status(**_kwargs: Any) -> dict[str, Any]:
            return {
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
            request = _request(ingress, kind="runtime_scale", client_sequence=9)
        finally:
            ingress.close()
        return request.model_copy(
            update={
                "payload": {
                    "request_context": {"source_label": "test"},
                    "selector": {
                        "program_binding_id": "binding.1",
                        "component_id": "controlled_component",
                        "command_target_id": "command_target",
                    },
                    "expected_control_generation": 0,
                    "scale": 0.25,
                }
            }
        )

    stamp = _stamp()
    accepted_service = Service({"ok": True, "accepted": {}})
    accepted, _ = _dispatch(accepted_service)(command(), stamp)
    assert accepted.disposition == "received"
    # The nested context is formed once, here: the session's class 0 key.
    assert accepted_service.request.request_context.request_key == (
        "00" + "00000000000001" + "0000000000000009"
    )
    assert accepted_service.request.request_context.source_label == "test"

    rejected_service = Service(
        {
            "ok": False,
            "error": {"code": "request_id_conflict", "message": "conflict"},
        }
    )
    rejected, result = _dispatch(rejected_service)(command(), stamp)
    assert rejected.disposition == "rejected"
    assert rejected.code == "request_id_conflict"
    assert result == rejected_service.result


@pytest.mark.parametrize(
    "kind,case",
    [
        ("operation_reset", "current"),
        ("operation_acknowledge_completed", "current"),
        ("operation_reset", "other_operation"),
        ("operation_reset", "old_revision"),
        ("operation_reset", "retired"),
        ("operation_reset", "not_allowed"),
        ("operation_reset", "recovery"),
    ],
)
def test_running_program_controls_only_exact_native_manual_operation(kind, case):
    calls = []

    class Service:
        @staticmethod
        def program_cycle_status(**_kwargs):
            return {
                "execution_control_revision": 7,
                "cycle_generation": 3,
                "program_run_id": 1,
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
                "identity": {"application_start_id": "3f9a0000000000a1"},
                "control_state": {
                    "schema_version": "manyforge.execution_control_state.v8",
                    "manual_operations": [
                        {
                            "resident": case != "retired",
                            "operation": {
                                "operation_id": (
                                    8 if case == "other_operation" else 7
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
            assert request.operation_id == 7
            assert request.expected_operation_revision == 3
            assert request.request_context.request_key == (
                "00" + "00000000000001" + "0000000000000001"
            )
            return {"ok": True, "accepted": {}}

        execution_reset = execution_acknowledge_completed = execute_control

    ingress, _ = _ingress(_Clock())
    try:
        request = _request(
            ingress,
            kind=kind,
            payload={
                "request_context": {"source_label": "test"},
                "operation_id": 7,
                "expected_operation_revision": 3,
            },
        )
    finally:
        ingress.close()
    result, _ = _dispatch(Service())(request, _stamp())
    # Like Stop, a Reset resolves to the operation's current revision.
    if case in {"current", "recovery", "old_revision"}:
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
        def program_cycle_status(**_kwargs):
            return {
                "execution_control_revision": 7,
                "cycle_generation": 3,
                "program_run_id": 1,
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
    if context_present:
        payload["manual_handover"] = {
            "program_run_id": 1,
            "claim_generation": 1,
            "hold_revision": 0,
        }
    ingress, _ = _ingress(_Clock())
    try:
        request = _request(ingress, kind="operation_start", payload=payload)
    finally:
        ingress.close()
    result, _ = _dispatch(Service())(request, _stamp())
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
        {"ok": True, "accepted": {"operation_id": 5}}
        if native_ok else {"ok": False, "error": {"code": "component_not_ready"}}
    )

    class Service:
        @staticmethod
        def program_cycle_status(**_kwargs):
            return {
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
    ingress, _ = _ingress(_Clock())
    try:
        request = _request(ingress, kind="operation_start", payload=payload)
    finally:
        ingress.close()
    result, response = _dispatch(Service())(request, _stamp())
    assert len(submitted) == 1
    forwarded = submitted[0][0].model_dump(mode="json")
    assert forwarded["participants"] == payload["participants"]
    for key in (
        "program_revision", "program_binding_id", "program_run_id",
        "expected_graph_revision", "expected_retained_bundle_id", "expected_scene_revision",
    ):
        assert forwarded[key] == payload[key]
    assert forwarded.get("manual_handover") is None
    assert forwarded["request_context"]["request_key"] == (
        "00" + "00000000000001" + "0000000000000001"
    )
    assert response is native_result
    assert result.disposition == ("received" if native_ok else "rejected")
    assert result.code == (
        "operation_start_received" if native_ok else "component_not_ready"
    )


@pytest.mark.parametrize("accepted", [(False, False), (True, False), (False, True), (True, True)])
@pytest.mark.parametrize("requested", [(False, False), (True, False), (False, True), (True, True)])
def test_reset_join_requires_exact_accepted_options(accepted, requested):
    clock = _Clock()
    ingress, _stamps = _ingress(clock)
    try:
        request = _request(ingress, kind="program_reset").model_copy(update={"payload": {
            "program_run_id": None, "expected_cycle_generation": 3,
            "reload_initial_scene": requested[0], "reset_robot_position": requested[1],
        }})
        cycle = {"execution_control_revision": 7,
                 "cycle_generation": 3, "program_run_id": None, "state": "resetting",
                 "reset_options": {"reload_initial_scene": accepted[0], "reset_robot_position": accepted[1]}}
        disposition, projection = _dispatch(object())._reduce_unavailable(request, cycle, None)
        assert disposition.disposition == ("joined" if accepted == requested else "rejected")
        assert disposition.code == ("program_reset_already_current" if accepted == requested else "program_run_request_conflict")
        assert projection == {"program_cycle": cycle}
    finally:
        ingress.close()


def test_receipt_names_the_formed_key_and_the_runner_row() -> None:
    """Every forwarded command's receipt names the key it was forwarded with
    (BS 2.8; OD-214: WebSocket commands too) and a runner command's row."""

    class Service:
        def __init__(self) -> None:
            self.requests: list[Any] = []

        @staticmethod
        def program_cycle_status(**_kwargs: Any) -> dict[str, Any]:
            return {
                "execution_control_revision": 7,
                "cycle_generation": 3,
                "program_run_id": None,
                "state": "idle_after_reset",
                "allowed_commands": [
                    {"command_kind": "program_run", "allowed": True, "disabled_reason": None}
                ],
            }

        def program_run_start(self, request):
            self.requests.append(request)
            # The runner's reply at acceptance (OD-214): the row it took.
            return {
                "schema_version": "manyforge.program_command_reply.v1",
                "command": {
                    "kind": "start",
                    "request_key": request.request_key,
                    "step": "admitting",
                    "accepted_at_ns": "1763112345678901234",
                    "end": None,
                },
            }

        def execution_stop_all(self, request, attribution):
            del attribution
            self.requests.append(request)
            return {"ok": True, "accepted": {}}

    service = Service()
    dispatch = _dispatch(service)
    ingress = ExecutionControlIngress(
        projection=lambda: 7,
        dispatch=dispatch,
        application_start_id=APPLICATION_START_ID,
        authority=ControlFreshnessAuthority(clock_ns=_Clock(), secret=b"x" * 32),
        clock_ns=_Clock(),
    )
    digest = "sha256:" + "a" * 64
    run = {
        "scenario_id": "cell.demo",
        "source_document_digest": digest,
        "expected_applied_record_digest": digest,
        "draft_program_revision": digest,
    }
    client_key = "ff" + "0123456789abcd" + "0011223344556677"

    async def exercise():
        rest = await ingress.submit(
            _request(ingress, session=False, payload={**run, "request_key": client_key}),
            transport_kind="rest",
        )
        websocket = await ingress.submit(
            _request(ingress, client_sequence=3, payload=run),
            transport_kind="websocket",
        )
        stop = await ingress.submit(
            _request(
                ingress,
                kind="stop_all",
                session=False,
                payload={"request_context": {"source_label": "test"}},
            ),
            transport_kind="rest",
        )
        return rest, websocket, stop

    try:
        rest, websocket, stop = asyncio.run(exercise())
    finally:
        ingress.close()

    # Class 3, part 0: the client's bytes 1-15 with byte 0 overwritten.
    assert rest.receipt.request_key == "c0" + client_key[2:]
    assert service.requests[0].request_key == rest.receipt.request_key
    # Both receipts carry the runner's row, found again by the same key.
    assert rest.receipt.schema_version == "manyforge.execution_command_receipt.v3"
    assert rest.receipt.disposition == "received"
    assert rest.receipt.command == rest.result["command"]
    assert rest.receipt.command["request_key"] == rest.receipt.request_key
    assert websocket.receipt.request_key == "00" + "00000000000001" + "0000000000000003"
    assert service.requests[1].request_key == websocket.receipt.request_key
    assert websocket.receipt.command["request_key"] == websocket.receipt.request_key
    # A Stop without a key acts with a fresh Headless key: class 1, minter 5.
    key = stop.receipt.request_key
    assert stop.receipt.command is None
    assert key is not None and key[:16] == "40" + "00000000000005"
    assert int(key[16:], 16) >= 1
    assert service.requests[2].request_context.request_key == key
