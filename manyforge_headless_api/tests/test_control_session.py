from __future__ import annotations

import asyncio
import json
import threading
from typing import Any

from fastapi import WebSocketDisconnect
import pytest

from manyforge_headless_api.control_protocol import (
    CONTROL_HEARTBEAT_DELAYED_NS,
    CONTROL_HEARTBEAT_STALE_NS,
    CONTROL_MAXIMUM_CLIENTS,
    CONTROL_MAXIMUM_MESSAGES_PER_SECOND,
    CONTROL_PROTOCOL_VERSION,
    EXECUTION_COMMAND_SCHEMA,
    ControlFreshnessAuthority,
    ExecutionControlCommandDTO,
    ExecutionControlDeltaDTO,
    ExecutionIngressResult,
)
from manyforge_headless_api.control_session import (
    ExecutionControlSession,
    _CONTROL_DEAD_PEER_TIMEOUT_NS,
    _OutboundSlots,
)
from manyforge_headless_api.control_state import (
    ControlSemanticContext,
    ControlStateFrame,
    ControlStateSubscription,
    HeadlessControlStateSource,
    _shell_key,
    build_control_projection,
)
from manyforge_headless_api.component_overview import ComponentOverviewFacts
from manyforge_headless_api.execution_control import ExecutionControlIngress


def _cycle() -> dict[str, Any]:
    return {
        "program_runner_owner_epoch": "runner.1",
        "execution_control_revision": 4,
        "cycle_generation": 2,
        "state": "idle_after_reset",
        "program_run_id": None,
        "run_revision": None,
        "allowed_commands": [
            {
                "command_kind": "program_run",
                "semantic_target": {"cycle_generation": 2},
                "allowed": True,
                "disabled_reason": None,
            }
        ],
        "causal_correlation_id": None,
        "causal_session_id": None,
        "causal_client_sequence": None,
        "failure": None,
    }


def _ingress(stamps: list[Any]) -> ExecutionControlIngress:
    def dispatch(request, stamp):
        stamps.append(stamp)
        return (
            ExecutionIngressResult(
                disposition="received",
                code=f"{request.command_envelope.command_kind}_received",
                semantic_owner_epoch="runner.1",
                execution_control_revision=4,
                current_identity={"cycle_generation": 2},
            ),
            {"accepted": True},
        )

    return ExecutionControlIngress(
        projection=lambda: ("runner.1", 4),
        dispatch=dispatch,
        authority=ControlFreshnessAuthority(
            receiver_incarnation="receiver.1",
            secret=b"x" * 32,
        ),
    )


class _Source:
    def __init__(self, frame: ControlStateFrame) -> None:
        self.frame = frame
        self.subscription: ControlStateSubscription | None = None
        self.unsubscribed = False
        self.messages: list[tuple[str, int]] = []
        self.heartbeat_rtt: list[int] = []
        self.delay_transitions: list[bool] = []
        self.incidents: list[dict[str, Any]] = []

    def record_message(self, kind: str, byte_count: int) -> None:
        self.messages.append((kind, byte_count))

    def record_state_replacement(self) -> None:
        return None

    def record_heartbeat_echo(self, round_trip_ns: int) -> None:
        self.heartbeat_rtt.append(round_trip_ns)

    def record_delay_transition(self, stale: bool) -> None:
        self.delay_transitions.append(stale)

    @staticmethod
    def relative_monotonic_ns(now_ns: int) -> int:
        return now_ns

    def record_incident(self, **record: Any) -> None:
        self.incidents.append(record)

    def record_client_incident_transition(self, **record: Any) -> None:
        self.incidents.append(record)

    async def subscribe(self) -> ControlStateSubscription:
        self.subscription = ControlStateSubscription(asyncio.Event(), self.frame)
        self.subscription.wake.set()
        return self.subscription

    def take(self, subscription: ControlStateSubscription):
        value = subscription.frame
        subscription.frame = None
        subscription.wake.clear()
        return value

    def current(self) -> ControlStateFrame:
        return self.frame

    def semantic_context(self) -> ControlSemanticContext:
        return ControlSemanticContext(
            status="current",
            reason=None,
            semantic_owner_epoch=self.frame.control.semantic_owner_epoch,
            execution_control_revision=self.frame.control.execution_control_revision,
        )

    def notify_control(self) -> None:
        return None

    async def unsubscribe(self, subscription: ControlStateSubscription) -> None:
        subscription.closed = True
        self.unsubscribed = True


class _HandshakeSocket:
    def __init__(self) -> None:
        self.accepted = False
        self.closed = False
        self.receives = 0
        self.sent: list[dict[str, Any]] = []

    async def accept(self) -> None:
        self.accepted = True

    async def receive_text(self) -> str:
        self.receives += 1
        if self.receives == 1:
            return json.dumps(
                {
                    "schema_version": CONTROL_PROTOCOL_VERSION,
                    "message_kind": "client_hello",
                    "supported_versions": [CONTROL_PROTOCOL_VERSION],
                    "client_incarnation": "client.1",
                    "source_label": "test",
                }
            )
        raise WebSocketDisconnect()

    async def receive(self) -> dict[str, str]:
        await asyncio.Event().wait()
        raise AssertionError("unreachable")

    async def send_text(self, payload: str) -> None:
        self.sent.append(json.loads(payload))

    async def close(self, *_args: Any, **_kwargs: Any) -> None:
        self.closed = True


class _CommandSocket(_HandshakeSocket):
    def __init__(self, frame: ControlStateFrame) -> None:
        super().__init__()
        self.frame = frame
        self.session_id: str | None = None
        self.done = asyncio.Event()

    async def receive_text(self) -> str:
        self.receives += 1
        if self.receives == 1:
            return json.dumps(
                {
                    "schema_version": CONTROL_PROTOCOL_VERSION,
                    "message_kind": "client_hello",
                    "supported_versions": [CONTROL_PROTOCOL_VERSION],
                    "client_incarnation": "client.1",
                    "source_label": "test",
                }
            )
        if self.receives == 2:
            while self.session_id is None:
                await asyncio.sleep(0)
            control = self.frame.control
            return json.dumps(
                {
                    "schema_version": CONTROL_PROTOCOL_VERSION,
                    "message_kind": "command",
                    "control_session_id": self.session_id,
                    "client_sequence": "1",
                    "command": {
                        "command_envelope": {
                            "schema_version": EXECUTION_COMMAND_SCHEMA,
                            "command_kind": "program_run",
                            "correlation_id": "request.1",
                            "source_label": "test",
                            "control_session_id": self.session_id,
                            "client_sequence": "1",
                            "freshness": control.freshness.model_dump(mode="json"),
                            "expected_semantic_owner_epoch": (
                                control.semantic_owner_epoch
                            ),
                            "expected_execution_control_revision": (
                                control.execution_control_revision
                            ),
                        },
                        "payload": {},
                    },
                }
            )
        await self.done.wait()
        raise WebSocketDisconnect()

    async def send_text(self, payload: str) -> None:
        value = json.loads(payload)
        self.sent.append(value)
        if value["message_kind"] == "server_hello":
            self.session_id = value["control_session_id"]
        if value["message_kind"] == "command_receipt":
            self.done.set()
            raise WebSocketDisconnect()


class _DisconnectBeforeSnapshotSocket(_HandshakeSocket):
    async def receive(self) -> dict[str, str]:
        return {"type": "websocket.disconnect"}


class _PendingSource(_Source):
    async def subscribe(self) -> ControlStateSubscription:
        self.subscription = ControlStateSubscription(asyncio.Event())
        return self.subscription


def _frame(ingress: ExecutionControlIngress) -> ControlStateFrame:
    cycle = _cycle()
    return ControlStateFrame(
        generation=1,
        event_class="critical",
        control=build_control_projection(cycle, ingress),
        shell={"program_cycle": cycle},
    )


def _shell_for_cycle(cycle: dict[str, Any]) -> dict[str, Any]:
    runtime = {
        "projection_owner_epoch": "runtime.1",
        "sections": {
            "manager": {"source_owner_epoch": "manager.1"},
            "logical_scene": {
                "source_owner_epoch": "scene.1",
                "source_generation": 3,
            },
            "scene_runtime": {
                "source_owner_epoch": "scene-runtime.1",
                "source_generation": 5,
            },
        },
    }
    alarms = {
        "owner_epoch": "alarms.1",
        "generation": 7,
        "state_revision": 11,
    }
    authoring = {"admission_generation": 13}
    return {
        "runtime": runtime,
        "alarms": alarms,
        "authoring_admission": authoring,
        "component_overview": {
            "basis": {
                "runtime_projection_owner_epoch": "runtime.1",
                "manager_source_owner_epoch": "manager.1",
                "logical_scene_source_owner_epoch": "scene.1",
                "logical_scene_source_generation": 3,
                "scene_runtime_source_owner_epoch": "scene-runtime.1",
                "scene_runtime_source_generation": 5,
                "alarm_owner_epoch": "alarms.1",
                "alarm_generation": 7,
                "alarm_state_revision": 11,
                "admission_generation": 13,
                "program_runner_owner_epoch": cycle[
                    "program_runner_owner_epoch"
                ],
                "program_cycle_generation": cycle["cycle_generation"],
            }
        },
        "execution_targets": {},
        "execution_control_state": {},
    }


def _facts_for_cycle(cycle: dict[str, Any]) -> ComponentOverviewFacts:
    shell = _shell_for_cycle(cycle)
    return ComponentOverviewFacts(
        runtime=shell["runtime"],
        alarms=shell["alarms"],
        authoring_admission=shell["authoring_admission"],
        program_cycle=cycle,
        applied_program={},
        source_model={},
        execution_identity_response={},
        execution_targets_response={},
        execution_control_response={},
        errors={},
    )


def test_control_session_starts_with_versioned_current_snapshot() -> None:
    stamps: list[Any] = []
    ingress = _ingress(stamps)
    frame = _frame(ingress)
    source = _Source(frame)
    socket = _HandshakeSocket()
    try:
        asyncio.run(
            ExecutionControlSession(  # type: ignore[arg-type]
                socket, source, ingress
            ).serve()
        )
    finally:
        ingress.close()

    assert socket.accepted and socket.closed and source.unsubscribed
    assert ingress.active_control_sessions == 0
    assert [message["message_kind"] for message in socket.sent] == [
        "server_hello",
        "heartbeat_challenge",
        "snapshot",
    ]
    assert socket.sent[1]["semantic_status"] == "current"
    assert socket.sent[2]["complete"] is True
    assert socket.sent[2]["control"]["cycle_state"] == "idle_after_reset"


def test_control_delta_replaces_only_changed_immutable_owner_facts() -> None:
    ingress = _ingress([])
    cycle = _cycle()
    shell = {**_shell_for_cycle(cycle), "program_cycle": cycle}
    previous = ControlStateFrame(
        generation=1,
        event_class="critical",
        control=build_control_projection(cycle, ingress),
        shell=shell,
    )
    changed_cycle = {**cycle, "node_status_revision": 2}
    current = ControlStateFrame(
        generation=2,
        event_class="node_progress",
        control=build_control_projection(changed_cycle, ingress),
        shell={**shell, "program_cycle": changed_cycle},
    )
    session = ExecutionControlSession(  # type: ignore[arg-type]
        object(), _Source(previous), ingress
    )
    try:
        message = session._delta_message(current, previous)  # noqa: SLF001
    finally:
        ingress.close()

    assert message.shell == {
        "complete": False,
        "program_cycle": changed_cycle,
    }


def test_control_delta_keeps_native_target_and_state_capture_together() -> None:
    ingress = _ingress([])
    cycle = _cycle()
    shell = {**_shell_for_cycle(cycle), "program_cycle": cycle}
    previous = ControlStateFrame(
        generation=1,
        event_class="cold",
        control=build_control_projection(cycle, ingress),
        shell=shell,
    )
    current_shell = {
        **shell,
        "execution_targets": {"capture": 2},
        "execution_control_state": {"capture": 2},
    }
    current = ControlStateFrame(
        generation=2,
        event_class="cold",
        control=build_control_projection(cycle, ingress),
        shell=current_shell,
    )
    session = ExecutionControlSession(  # type: ignore[arg-type]
        object(), _Source(previous), ingress
    )
    try:
        message = session._delta_message(current, previous)  # noqa: SLF001
    finally:
        ingress.close()

    assert message.shell == {
        "complete": False,
        "execution_targets": {"capture": 2},
        "execution_control_state": {"capture": 2},
    }


def test_control_delta_reuses_immutable_targets_for_control_only_change() -> None:
    ingress = _ingress([])
    cycle = _cycle()
    shell = {**_shell_for_cycle(cycle), "program_cycle": cycle}
    previous = ControlStateFrame(
        generation=1,
        event_class="cold",
        control=build_control_projection(cycle, ingress),
        shell=shell,
    )
    current = ControlStateFrame(
        generation=2,
        event_class="critical",
        control=build_control_projection(cycle, ingress),
        shell={**shell, "execution_control_state": {"capture": 2}},
    )
    session = ExecutionControlSession(  # type: ignore[arg-type]
        object(), _Source(previous), ingress
    )
    try:
        message = session._delta_message(current, previous)  # noqa: SLF001
    finally:
        ingress.close()

    assert message.shell == {
        "complete": False,
        "execution_control_state": {"capture": 2},
    }


def test_replaced_sparse_state_retains_facts_the_client_never_applied() -> None:
    ingress = _ingress([])
    initial_cycle = {**_cycle(), "state": "resetting", "cycle_generation": 1}
    initial_shell = {**_shell_for_cycle(initial_cycle), "program_cycle": initial_cycle}
    initial = ControlStateFrame(
        generation=1,
        event_class="critical",
        control=build_control_projection(initial_cycle, ingress),
        shell=initial_shell,
    )
    idle_cycle = {**initial_cycle, "state": "idle_after_reset", "cycle_generation": 2}
    idle_shell = {**initial_shell, "program_cycle": idle_cycle}
    idle = ControlStateFrame(
        generation=2,
        event_class="critical",
        control=build_control_projection(idle_cycle, ingress),
        shell=idle_shell,
    )
    ready_shell = {
        **idle_shell,
        "authoring_admission": {
            "authoring_allowed": True,
            "admission_generation": 3,
        },
    }
    ready = ControlStateFrame(
        generation=3,
        event_class="cold",
        control=build_control_projection(idle_cycle, ingress),
        shell=ready_shell,
    )
    session = ExecutionControlSession(  # type: ignore[arg-type]
        object(), _Source(initial), ingress
    )
    try:
        session._put_state(session._delta_message(idle, initial))  # noqa: SLF001
        session._put_state(session._delta_message(ready, idle))  # noqa: SLF001
        pending = session._slots.state  # noqa: SLF001
    finally:
        ingress.close()

    assert isinstance(pending, ExecutionControlDeltaDTO)
    assert pending.shell["program_cycle"] == idle_cycle
    assert pending.shell["authoring_admission"] == ready_shell["authoring_admission"]
    assert pending.shell["complete"] is False


def test_websocket_command_uses_the_same_ingress_without_waiting_for_state_poll() -> None:
    stamps: list[Any] = []
    ingress = _ingress(stamps)
    frame = _frame(ingress)
    source = _Source(frame)
    socket = _CommandSocket(frame)
    try:
        asyncio.run(
            ExecutionControlSession(  # type: ignore[arg-type]
                socket, source, ingress
            ).serve()
        )
    finally:
        ingress.close()

    receipts = [
        message for message in socket.sent
        if message["message_kind"] == "command_receipt"
    ]
    assert len(receipts) == 1
    assert receipts[0]["receipt"]["correlation_id"] == "request.1"
    assert receipts[0]["receipt"]["disposition"] == "received"
    assert int(receipts[0]["receipt"]["received_monotonic_ns"]) > 0
    assert int(receipts[0]["emitted_monotonic_ns"]) >= int(
        receipts[0]["receipt"]["received_monotonic_ns"]
    )
    assert receipts[0]["result"] is None
    assert len(stamps) == 1
    assert stamps[0].transport_kind == "websocket"


def test_client_disconnect_during_initial_sync_releases_its_subscription() -> None:
    ingress = _ingress([])
    source = _PendingSource(_frame(ingress))
    socket = _DisconnectBeforeSnapshotSocket()
    try:
        asyncio.run(
            ExecutionControlSession(  # type: ignore[arg-type]
                socket, source, ingress
            ).serve()
        )
    finally:
        ingress.close()

    assert socket.closed and source.unsubscribed
    assert [message["message_kind"] for message in socket.sent] == [
        "server_hello",
        "heartbeat_challenge",
    ]


def test_runtime_scale_and_one_discrete_command_can_progress_independently() -> None:
    async def exercise() -> None:
        released = threading.Event()
        kinds: list[str] = []

        def dispatch(request, _stamp):
            kinds.append(request.command_envelope.command_kind)
            assert released.wait(1.0)
            return (
                ExecutionIngressResult(
                    disposition="received",
                    code=f"{request.command_envelope.command_kind}_received",
                    semantic_owner_epoch="runner.1",
                    execution_control_revision=4,
                    current_identity={"cycle_generation": 2},
                ),
                {"accepted": True},
            )

        ingress = ExecutionControlIngress(
            projection=lambda: ("runner.1", 4),
            dispatch=dispatch,
            authority=ControlFreshnessAuthority(
                receiver_incarnation="receiver.1",
                secret=b"x" * 32,
            ),
        )
        source = _Source(_frame(ingress))
        session = ExecutionControlSession(  # type: ignore[arg-type]
            _HandshakeSocket(), source, ingress
        )
        session._latest_frame = source.frame  # noqa: SLF001

        def command(kind: str, sequence: int) -> ExecutionControlCommandDTO:
            control = source.frame.control
            return ExecutionControlCommandDTO.model_validate(
                {
                    "schema_version": CONTROL_PROTOCOL_VERSION,
                    "message_kind": "command",
                    "control_session_id": session._session_id,  # noqa: SLF001
                    "client_sequence": str(sequence),
                    "command": {
                        "command_envelope": {
                            "schema_version": EXECUTION_COMMAND_SCHEMA,
                            "command_kind": kind,
                            "correlation_id": f"request.{sequence}",
                            "source_label": "test",
                            "control_session_id": session._session_id,  # noqa: SLF001
                            "client_sequence": str(sequence),
                            "freshness": control.freshness.model_dump(mode="json"),
                            "expected_semantic_owner_epoch": (
                                control.semantic_owner_epoch
                            ),
                            "expected_execution_control_revision": (
                                control.execution_control_revision
                            ),
                        },
                        "payload": {},
                    },
                }
            )

        try:
            session._receive_command(command("runtime_scale", 1))  # noqa: SLF001
            session._receive_command(command("program_run", 2))  # noqa: SLF001
            scale = session._runtime_scale_pending  # noqa: SLF001
            discrete = session._ordinary_pending  # noqa: SLF001
            assert scale is not None and not scale.done()
            assert discrete is not None and not discrete.done()
            released.set()
            await asyncio.gather(scale, discrete)
            await asyncio.sleep(0)
            assert sorted(kinds) == ["program_run", "runtime_scale"]
            receipts = list(session._slots.receipts)  # noqa: SLF001
            assert len(receipts) == 2
            by_code = {message.receipt.code: message for message in receipts}
            assert by_code["program_run_received"].result is None
            assert by_code["runtime_scale_received"].result == {"accepted": True}
        finally:
            released.set()
            session._closed = True  # noqa: SLF001
            ingress.close()

    asyncio.run(exercise())


def test_outbound_state_and_heartbeat_are_latest_only_while_receipts_keep_order() -> None:
    async def exercise() -> None:
        slots = _OutboundSlots()
        slots.put_state("old-state")
        slots.put_state("current-state")
        slots.put_heartbeat("old-heartbeat")
        slots.put_heartbeat("current-heartbeat")
        slots.put_receipt("receipt-1")
        slots.put_receipt("receipt-2")
        assert await slots.take() == "receipt-1"
        assert await slots.take() == "receipt-2"
        assert await slots.take() == "current-heartbeat"
        assert await slots.take() == "current-state"
        slots.close()

    asyncio.run(exercise())


def test_inflight_command_does_not_hold_authoritative_state() -> None:
    async def exercise() -> None:
        ingress = _ingress([])
        session = ExecutionControlSession(  # type: ignore[arg-type]
            _HandshakeSocket(), _Source(_frame(ingress)), ingress
        )
        try:
            session._slots.put_state("newest-state")  # noqa: SLF001
            session._slots.put_heartbeat("heartbeat")  # noqa: SLF001
            allowed = lambda: session._state_inflight is None  # noqa: E731, SLF001

            assert await session._slots.take(  # noqa: SLF001
                state_allowed=allowed
            ) == "heartbeat"
            assert await session._slots.take(  # noqa: SLF001
                state_allowed=allowed
            ) == "newest-state"
        finally:
            session._slots.close()  # noqa: SLF001
            ingress.close()

    asyncio.run(exercise())


def test_one_state_credit_bounds_delivery_without_blocking_receipt_or_heartbeat() -> None:
    async def exercise() -> None:
        ingress = _ingress([])
        source = _Source(_frame(ingress))
        socket = _HandshakeSocket()
        session = ExecutionControlSession(  # type: ignore[arg-type]
            socket, source, ingress
        )
        try:
            await session._send_message(  # noqa: SLF001
                session._snapshot_message(source.frame)  # noqa: SLF001
            )
            assert session._state_inflight == 1  # noqa: SLF001

            session._put_state("newest-state")  # noqa: SLF001
            session._slots.put_heartbeat("heartbeat")  # noqa: SLF001
            session._slots.put_receipt("receipt")  # noqa: SLF001
            state_allowed = lambda: session._state_inflight is None  # noqa: E731,SLF001

            assert await session._slots.take(  # noqa: SLF001
                state_allowed=state_allowed
            ) == "receipt"
            assert await session._slots.take(  # noqa: SLF001
                state_allowed=state_allowed
            ) == "heartbeat"

            blocked = asyncio.create_task(
                session._slots.take(state_allowed=state_allowed)  # noqa: SLF001
            )
            await asyncio.sleep(0)
            assert not blocked.done()

            # This is the state transition performed only after an exact
            # state_applied message for sequence 1 is validated.
            session._state_inflight = None  # noqa: SLF001
            session._slots.wake.set()  # noqa: SLF001
            assert await blocked == "newest-state"
        finally:
            session._slots.close()  # noqa: SLF001
            ingress.close()

    asyncio.run(exercise())


def test_heartbeat_keeps_one_challenge_outstanding_and_reclaims_a_dead_peer() -> None:
    class CountingSource(_Source):
        def __init__(self, frame: ControlStateFrame) -> None:
            super().__init__(frame)
            self.current_calls = 0
            self.semantic_calls = 0

        def current(self) -> ControlStateFrame:
            self.current_calls += 1
            return super().current()

        def semantic_context(self) -> ControlSemanticContext:
            self.semantic_calls += 1
            return super().semantic_context()

    now_ns = 10_000_000_000
    ingress = _ingress([])
    source = CountingSource(_frame(ingress))
    session = ExecutionControlSession(  # type: ignore[arg-type]
        _HandshakeSocket(),
        source,
        ingress,
        clock_ns=lambda: now_ns,
    )
    try:
        first = session._next_heartbeat(  # noqa: SLF001
            now_ns + 250_000_000
        )
        assert first is not None
        assert first.challenge_sequence == "1"
        assert first.semantic_status == "current"
        assert first.semantic_reason is None
        assert first.freshness is not None
        assert source.current_calls == 0
        assert source.semantic_calls == 1

        # UI staleness at one second disables commands, but does not create
        # more server work or tear down a recoverable transport.
        assert session._next_heartbeat(now_ns + 1_000_000_001) is None  # noqa: SLF001
        assert source.current_calls == 0
        assert source.semantic_calls == 1

        session._last_echoed_challenge = 1  # noqa: SLF001
        session._last_client_sequence = 1  # noqa: SLF001
        session._last_heartbeat_ns = now_ns + 1_000_000_001  # noqa: SLF001
        second = session._next_heartbeat(  # noqa: SLF001
            now_ns + 1_250_000_001
        )
        assert second is not None
        assert second.challenge_sequence == "2"
        assert second.received_client_sequence == "1"
        assert source.current_calls == 0
        assert source.semantic_calls == 2

        with pytest.raises(TimeoutError, match="stopped answering"):
            session._next_heartbeat(  # noqa: SLF001
                session._last_heartbeat_ns  # noqa: SLF001
                + _CONTROL_DEAD_PEER_TIMEOUT_NS
                + 1
            )
    finally:
        ingress.close()


def test_semantic_gap_keeps_heartbeat_alive_without_owner_read() -> None:
    class TemporarilyUnavailableSource(_Source):
        def semantic_context(self) -> ControlSemanticContext:
            return ControlSemanticContext(
                status="unavailable",
                reason="program_control_source_unavailable",
            )

    now_ns = 10_000_000_000
    ingress = _ingress([])
    source = TemporarilyUnavailableSource(_frame(ingress))
    session = ExecutionControlSession(  # type: ignore[arg-type]
        _HandshakeSocket(),
        source,
        ingress,
        clock_ns=lambda: now_ns,
    )
    try:
        challenge = session._next_heartbeat(  # noqa: SLF001
            now_ns + 250_000_000
        )
        assert challenge is not None
        assert challenge.semantic_status == "unavailable"
        assert challenge.semantic_reason == "program_control_source_unavailable"
        assert challenge.freshness is None
        assert session._challenge_sequence == 1  # noqa: SLF001
    finally:
        ingress.close()


def test_delayed_and_stale_heartbeat_are_one_sparse_incident_episode() -> None:
    now_ns = 10_000_000_000
    ingress = _ingress([])
    source = _Source(_frame(ingress))
    session = ExecutionControlSession(  # type: ignore[arg-type]
        _HandshakeSocket(),
        source,
        ingress,
        clock_ns=lambda: now_ns,
    )
    try:
        assert session._next_heartbeat(now_ns) is not None  # noqa: SLF001
        assert session._next_heartbeat(  # noqa: SLF001
            now_ns + CONTROL_HEARTBEAT_DELAYED_NS + 1
        ) is None
        assert session._next_heartbeat(  # noqa: SLF001
            now_ns + CONTROL_HEARTBEAT_STALE_NS + 1
        ) is None
        assert [
            (event["incident_kind"], event["phase"])
            for event in source.incidents
        ] == [
            ("control_delayed", "activated"),
            ("control_stale", "activated"),
        ]

        session._last_echoed_challenge = 1  # noqa: SLF001
        session._resolve_heartbeat_incidents(  # noqa: SLF001
            now_ns + CONTROL_HEARTBEAT_STALE_NS + 2,
            CONTROL_HEARTBEAT_STALE_NS + 2,
        )
        assert [
            (event["incident_kind"], event["phase"])
            for event in source.incidents
        ] == [
            ("control_delayed", "activated"),
            ("control_stale", "activated"),
            ("control_delayed", "resolved"),
            ("control_stale", "resolved"),
        ]
        assert source.delay_transitions == [False, True]
    finally:
        ingress.close()


def test_virtual_heartbeat_churn_never_reads_or_reduces_owner_state() -> None:
    ingress = _ingress([])

    class NoReadService:
        def __getattr__(self, name: str):
            raise AssertionError(f"heartbeat must not read {name}")

    source = HeadlessControlStateSource(
        NoReadService(),  # type: ignore[arg-type]
        ingress,
        program_runner_websocket_url=None,
    )
    source._cycle = _cycle()  # noqa: SLF001
    source._facts = _facts_for_cycle(_cycle())  # noqa: SLF001
    source._shell = _shell_for_cycle(_cycle())  # noqa: SLF001
    source._program_source_current = True  # noqa: SLF001
    now_ns = 10_000_000_000
    session = ExecutionControlSession(  # type: ignore[arg-type]
        _HandshakeSocket(),
        source,
        ingress,
        clock_ns=lambda: now_ns,
    )
    try:
        for sequence in range(1, 6_001):
            challenge = session._next_heartbeat(now_ns)  # noqa: SLF001
            assert challenge is not None
            assert challenge.challenge_sequence == str(sequence)
            session._last_echoed_challenge = sequence  # noqa: SLF001
            session._challenge_issued_ns = None  # noqa: SLF001
            session._last_heartbeat_ns = now_ns  # noqa: SLF001
            source.record_heartbeat_echo(1_000_000)
            now_ns += 250_000_000

        diagnostics = source.diagnostics()
        assert diagnostics["owner_reads"] == 0
        assert diagnostics["reductions"] == 0
        assert diagnostics["unchanged_reductions"] == 0
        assert diagnostics["publications"] == {
            "critical": 0,
            "node_progress": 0,
            "cold": 0,
        }
        assert diagnostics["heartbeat_echoes"] == 6_000
        assert diagnostics["heartbeat_rtt_sample_count"] == 256
    finally:
        ingress.close()


def test_volatile_receiver_counters_do_not_create_a_cold_state_change() -> None:
    cycle = _cycle()
    first = _shell_for_cycle(cycle)
    first["component_overview"]["overview_revision"] = "overview.7"
    first_runtime = first["runtime"]
    first_runtime["projection_generation"] = 41
    first_runtime["sections"]["manager"]["receiver_age_ns"] = 10
    first_runtime["sections"]["manager"]["source_generation"] = 80
    first["alarms"]["state_revision"] = 20

    later = json.loads(json.dumps(first))
    later["runtime"]["projection_generation"] = 99
    later["runtime"]["sections"]["manager"]["receiver_age_ns"] = 900_000_000
    later["runtime"]["sections"]["manager"]["source_generation"] = 81
    later["runtime"]["sections"]["scene_runtime"]["source_generation"] += 1
    later["runtime"]["sections"]["scene_runtime"][
        "source_content_digest"
    ] = "pose-only-change"
    later["alarms"]["state_revision"] = 21

    assert _shell_key(later) == _shell_key(first)

    alarm_change = json.loads(json.dumps(later))
    alarm_change["alarms"]["generation"] += 1
    assert _shell_key(alarm_change) != _shell_key(first)


def test_source_file_freshness_pulse_does_not_wake_owner_reduction(tmp_path) -> None:
    source_path = tmp_path / "manager-source.json"

    def publish(*, generation: int, report: int, health: str = "healthy") -> None:
        source_path.write_text(
            json.dumps(
                {
                    "api_version": "manyforge.source_snapshot.v1",
                    "observed_at_ns": report,
                    "payload": {
                        "runtime": {
                            "observed_units": [
                                {
                                    "process_observation_sequence": report,
                                    "source_report_sequence": report,
                                    "source_health": health,
                                }
                            ]
                        }
                    },
                    "record_digest": f"record.{report}",
                    "semantic_content_digest": f"semantic.{report}",
                    "source_generation": generation,
                    "source_owner_epoch": "manager.1",
                    "source_owner_id": "manager",
                    "source_owner_kind": "component_manager",
                }
            ),
            encoding="utf-8",
        )

    ingress = _ingress([])
    source = HeadlessControlStateSource(
        object(),  # type: ignore[arg-type]
        ingress,
        program_runner_websocket_url=None,
        cold_watch_paths=(source_path,),
    )
    wakes: list[bool] = []
    source.notify_cold = lambda: wakes.append(True)  # type: ignore[method-assign]
    try:
        publish(generation=1, report=1)
        source._refresh_file_watches(notify=False)  # noqa: SLF001
        publish(generation=2, report=2)
        source._refresh_file_watches(notify=True)  # noqa: SLF001
        assert wakes == []

        publish(generation=3, report=3, health="faulted")
        source._refresh_file_watches(notify=True)  # noqa: SLF001
        assert wakes == [True]
    finally:
        ingress.close()


def test_scene_runtime_pose_only_update_does_not_wake_cold_projection(
    tmp_path,
) -> None:
    source_path = tmp_path / "scene-runtime-source.json"

    def publish(*, generation: int, sequence: int, state: str = "open") -> None:
        source_path.write_text(
            json.dumps(
                {
                    "api_version": "manyforge.source_snapshot.v1",
                    "observed_at_ns": sequence,
                    "payload": {
                        "api_version": "manyforge.scene_runtime_source.v1",
                        "cell_id": "cell.1",
                        "state": {
                            "api_version": "manyforge.scene_runtime_state.v1",
                            "active_replay_id": None,
                            "counters": {"buffered": 0},
                            "deferred": False,
                            "error": None,
                            "initialization_generation": 1,
                            "initialization_token": None,
                            "pose_sources": [{"source_sequence": sequence}],
                            "reason": "ready",
                            "runtime_identity": {"runtime_generation": 1},
                            "started_at_ns": 1,
                            "state": state,
                            "updated_at_ns": sequence,
                        },
                    },
                    "record_digest": f"record.{sequence}",
                    "semantic_content_digest": f"semantic.{sequence}",
                    "source_generation": generation,
                    "source_owner_epoch": "scene-runtime.1",
                    "source_owner_id": "scene-runtime",
                    "source_owner_kind": "scene_runtime",
                }
            ),
            encoding="utf-8",
        )

    ingress = _ingress([])
    source = HeadlessControlStateSource(
        object(),  # type: ignore[arg-type]
        ingress,
        program_runner_websocket_url=None,
        cold_watch_paths=(source_path,),
    )
    wakes: list[bool] = []
    source.notify_cold = lambda: wakes.append(True)  # type: ignore[method-assign]
    try:
        publish(generation=1, sequence=1)
        source._refresh_file_watches(notify=False)  # noqa: SLF001
        publish(generation=2, sequence=2)
        source._refresh_file_watches(notify=True)  # noqa: SLF001
        assert wakes == []

        publish(generation=3, sequence=3, state="resetting")
        source._refresh_file_watches(notify=True)  # noqa: SLF001
        assert wakes == [True]
    finally:
        ingress.close()


def test_server_sequence_is_assigned_after_latest_slot_replacement() -> None:
    async def exercise() -> None:
        ingress = _ingress([])
        source = _Source(_frame(ingress))
        socket = _HandshakeSocket()
        session = ExecutionControlSession(  # type: ignore[arg-type]
            socket, source, ingress
        )
        older_state = session._snapshot_message(source.frame)  # noqa: SLF001
        newer_state = session._snapshot_message(source.frame)  # noqa: SLF001
        session._slots.put_state(older_state)  # noqa: SLF001
        session._slots.put_state(newer_state)  # noqa: SLF001
        session._slots.put_receipt("receipt-first")  # noqa: SLF001
        assert await session._slots.take() == "receipt-first"  # noqa: SLF001
        queued = await session._slots.take()  # noqa: SLF001
        await session._send_message(queued)  # noqa: SLF001
        assert socket.sent[-1]["server_sequence"] == "1"
        ingress.close()

    asyncio.run(exercise())


def test_control_completion_wakes_non_program_facts_with_runner_stream() -> None:
    ingress = _ingress([])
    source = HeadlessControlStateSource(
        object(),  # type: ignore[arg-type]
        ingress,
        program_runner_websocket_url="ws://program-runner.test/ws/control-state",
    )
    wakes: list[bool] = []
    source.notify_cold = lambda: wakes.append(True)  # type: ignore[method-assign]
    try:
        source.notify_control()
        assert wakes == [True]
    finally:
        ingress.close()


def test_owner_replacement_requires_a_new_snapshot_barrier() -> None:
    async def exercise() -> None:
        stamps: list[Any] = []
        ingress = _ingress(stamps)
        source = _Source(_frame(ingress))
        socket = _HandshakeSocket()
        session = ExecutionControlSession(  # type: ignore[arg-type]
            socket, source, ingress
        )
        subscription = await source.subscribe()
        session._subscription = subscription  # noqa: SLF001
        session._latest_frame = source.frame  # noqa: SLF001
        session._snapshot_sent = True  # noqa: SLF001
        replacement_cycle = {**_cycle(), "program_runner_owner_epoch": "runner.2"}
        source.frame = ControlStateFrame(
            generation=2,
            event_class="critical",
            control=build_control_projection(replacement_cycle, ingress),
            shell={"program_cycle": replacement_cycle},
        )
        subscription.frame = source.frame
        subscription.wake.set()
        task = asyncio.create_task(session._state_sender())  # noqa: SLF001
        queued = await asyncio.wait_for(session._slots.take(), 0.2)  # noqa: SLF001
        payload = queued.model_dump(mode="json")
        assert payload["message_kind"] == "resync_required"
        assert payload["reason_code"] == "control_owner_replaced"
        assert session._resync_required is True  # noqa: SLF001
        session._closed = True  # noqa: SLF001
        subscription.closed = True
        subscription.wake.set()
        await task
        ingress.close()

    asyncio.run(exercise())


def test_no_control_client_creates_no_tasks_reads_or_timers() -> None:
    class NoReadService:
        def __getattr__(self, name: str):
            raise AssertionError(f"idle source must not read {name}")

    stamps: list[Any] = []
    ingress = _ingress(stamps)

    async def exercise() -> None:
        source = HeadlessControlStateSource(
            NoReadService(),  # type: ignore[arg-type]
            ingress,
            program_runner_websocket_url=None,
        )
        assert source.active_clients == 0
        assert source._runner_task is None  # noqa: SLF001
        assert source._cold_task is None  # noqa: SLF001
        assert source._watch_task is None  # noqa: SLF001
        assert source._cold_expiry_handle is None  # noqa: SLF001
        await source.close()

    try:
        asyncio.run(exercise())
    finally:
        ingress.close()
    assert stamps == []


def test_last_client_teardown_finishes_before_a_new_source_generation() -> None:
    class NoReadService:
        def __getattr__(self, name: str):
            raise AssertionError(f"lifecycle test must not read {name}")

    ingress = _ingress([])

    async def exercise() -> None:
        source = HeadlessControlStateSource(
            NoReadService(),  # type: ignore[arg-type]
            ingress,
            program_runner_websocket_url="ws://program.test/control-state",
        )
        runner_started = asyncio.Event()
        runner_cancelled = asyncio.Event()
        finish_cancel = asyncio.Event()
        start_count = 0

        async def delayed_runner() -> None:
            nonlocal start_count
            start_count += 1
            runner_started.set()
            try:
                await asyncio.Event().wait()
            except asyncio.CancelledError:
                runner_cancelled.set()
                await finish_cancel.wait()
                raise

        async def idle_cold_assembler() -> None:
            await asyncio.Event().wait()

        source._run_program_source = delayed_runner  # type: ignore[method-assign]  # noqa: SLF001
        source._run_cold_assembler = idle_cold_assembler  # type: ignore[method-assign]  # noqa: SLF001

        first = await source.subscribe()
        await runner_started.wait()
        teardown = asyncio.create_task(source.unsubscribe(first))
        await runner_cancelled.wait()

        second_attempted = asyncio.Event()

        async def subscribe_again() -> ControlStateSubscription:
            second_attempted.set()
            return await source.subscribe()

        replacement = asyncio.create_task(subscribe_again())
        await second_attempted.wait()
        assert not replacement.done()
        assert start_count == 1

        finish_cancel.set()
        await teardown
        second = await replacement
        while start_count != 2:
            await asyncio.sleep(0)
        await source.unsubscribe(second)
        await source.close()

    try:
        asyncio.run(exercise())
    finally:
        ingress.close()


def test_cold_freshness_uses_one_deadline_and_cancels_it_without_clients(
    tmp_path,
) -> None:
    source_path = tmp_path / "manager-source.json"
    source_path.write_text(
        json.dumps(
            {
                "api_version": "manyforge.source_snapshot.v1",
                "payload": {},
                "source_owner_epoch": "manager.1",
                "source_owner_id": "manager",
                "source_owner_kind": "component_manager",
            }
        ),
        encoding="utf-8",
    )
    ingress = _ingress([])

    async def exercise() -> None:
        source = HeadlessControlStateSource(
            object(),  # type: ignore[arg-type]
            ingress,
            program_runner_websocket_url=None,
            cold_watch_paths=(source_path,),
            cold_freshness_paths=(source_path,),
            cold_freshness_budget_ns=10_000_000_000,
        )
        source._loop = asyncio.get_running_loop()  # noqa: SLF001
        subscription = ControlStateSubscription(asyncio.Event())
        source._subscriptions.add(subscription)  # noqa: SLF001
        source._refresh_file_watches(notify=False)  # noqa: SLF001
        handle = source._cold_expiry_handle  # noqa: SLF001
        assert handle is not None
        await source.unsubscribe(subscription)
        assert handle.cancelled()
        assert source._cold_expiry_handle is None  # noqa: SLF001
        await source.close()

    try:
        asyncio.run(exercise())
    finally:
        ingress.close()


def test_control_source_rejects_clients_above_the_fixed_product_cap() -> None:
    class NoReadService:
        def __getattr__(self, name: str):
            raise AssertionError(f"client admission must not read {name}")

    ingress = _ingress([])

    async def exercise() -> None:
        source = HeadlessControlStateSource(
            NoReadService(),  # type: ignore[arg-type]
            ingress,
            program_runner_websocket_url=None,
        )
        for _ in range(CONTROL_MAXIMUM_CLIENTS):
            source._subscriptions.add(  # noqa: SLF001
                ControlStateSubscription(asyncio.Event())
            )
        with pytest.raises(RuntimeError, match="client limit reached"):
            await source.subscribe()
        source._subscriptions.clear()  # noqa: SLF001
        await source.close()

    try:
        asyncio.run(exercise())
    finally:
        ingress.close()


def test_control_session_bounds_rate_and_requires_contiguous_client_sequence() -> None:
    now = [1_000_000_000]
    ingress = _ingress([])
    source = _Source(_frame(ingress))
    session = ExecutionControlSession(  # type: ignore[arg-type]
        _HandshakeSocket(),
        source,
        ingress,
        clock_ns=lambda: now[0],
    )
    try:
        for _ in range(CONTROL_MAXIMUM_MESSAGES_PER_SECOND):
            session._admit_rate()  # noqa: SLF001
        with pytest.raises(ValueError, match="message rate exceeded"):
            session._admit_rate()  # noqa: SLF001

        now[0] += 1_000_000_000
        session._admit_rate()  # noqa: SLF001
        assert session._admit_client_sequence("1") == 1  # noqa: SLF001
        with pytest.raises(ValueError, match="not contiguous"):
            session._admit_client_sequence("1")  # noqa: SLF001
        with pytest.raises(ValueError, match="not contiguous"):
            session._admit_client_sequence("3")  # noqa: SLF001
        assert session._admit_client_sequence("2") == 2  # noqa: SLF001
    finally:
        ingress.close()


def test_client_incident_rate_resets_without_a_lifetime_exhaustion_counter() -> None:
    now = [1_000_000_000]
    ingress = _ingress([])
    session = ExecutionControlSession(  # type: ignore[arg-type]
        _HandshakeSocket(),
        _Source(_frame(ingress)),
        ingress,
        clock_ns=lambda: now[0],
    )
    try:
        for _window in range(1_000):
            for _message in range(8):
                session._admit_client_incident()  # noqa: SLF001
            with pytest.raises(ValueError, match="incident rate exceeded"):
                session._admit_client_incident()  # noqa: SLF001
            now[0] += 1_000_000_000
    finally:
        ingress.close()


def test_client_incident_episode_survives_reconnect_without_duplicate_activation() -> None:
    ingress = _ingress([])
    source = HeadlessControlStateSource(
        object(),  # type: ignore[arg-type]
        ingress,
        program_runner_websocket_url=None,
    )
    recorded: list[dict[str, Any]] = []
    source.record_incident = lambda **value: recorded.append(value)  # type: ignore[method-assign]
    source._diagnostic_origin_ns = 100  # noqa: SLF001
    common = {
        "client_incarnation": "client.1",
        "incident_id": "incident.1",
        "incident_kind": "control_disconnected",
        "source_label": "operator_ui",
        "occurrence_count": 1,
        "maximum_delay_ns": None,
        "reason_code": "control_websocket_closed",
    }
    source.record_client_incident_transition(
        **common,
        phase="activated",
        control_session_id="session.1",
        now_ns=200,
    )
    source.record_client_incident_transition(
        **common,
        phase="activated",
        control_session_id="session.2",
        now_ns=300,
    )
    source.record_client_incident_transition(
        **{**common, "reason_code": "control_websocket_recovered"},
        phase="resolved",
        control_session_id="session.2",
        now_ns=400,
    )

    assert [(item["phase"], item["control_session_id"]) for item in recorded] == [
        ("activated", "session.1"),
        ("resolved", "session.2"),
    ]
    assert recorded[1]["first_observed_ns"] == 100
    assert recorded[1]["last_observed_ns"] == 300
    ingress.close()


def test_incident_enqueue_uses_native_async_boundary_without_executor_queue() -> None:
    ingress = _ingress([])
    recorded: list[dict[str, Any]] = []

    async def exercise() -> None:
        source = HeadlessControlStateSource(
            object(),  # type: ignore[arg-type]
            ingress,
            program_runner_websocket_url=None,
            incident_recorder=lambda **value: recorded.append(value) or {},
        )
        source._loop = asyncio.get_running_loop()  # noqa: SLF001
        source.record_incident(incident_id="incident.1", phase="activated")
        assert recorded == []
        await asyncio.sleep(0)
        assert recorded == [{"incident_id": "incident.1", "phase": "activated"}]

    try:
        asyncio.run(exercise())
    finally:
        ingress.close()


def test_abandoned_client_incident_correlations_remain_bounded_for_years() -> None:
    ingress = _ingress([])
    source = HeadlessControlStateSource(
        object(),  # type: ignore[arg-type]
        ingress,
        program_runner_websocket_url=None,
    )
    source.record_incident = lambda **_value: None  # type: ignore[method-assign]
    source._diagnostic_origin_ns = 0  # noqa: SLF001
    for index in range(10_000):
        source.record_client_incident_transition(
            client_incarnation=f"client.{index}",
            incident_id=f"incident.{index}",
            incident_kind="control_disconnected",
            phase="activated",
            control_session_id=f"session.{index}",
            source_label="operator_ui",
            occurrence_count=1,
            maximum_delay_ns=None,
            reason_code="control_websocket_closed",
            now_ns=index + 1,
        )

    assert len(source._active_client_incidents) == CONTROL_MAXIMUM_CLIENTS * 8  # noqa: SLF001
    # A resolution for an expired diagnostic correlation is harmless. It must
    # never close a healthy control session or exhaust the application.
    source.record_client_incident_transition(
        client_incarnation="client.0",
        incident_id="incident.0",
        incident_kind="control_disconnected",
        phase="resolved",
        control_session_id="session.current",
        source_label="operator_ui",
        occurrence_count=1,
        maximum_delay_ns=None,
        reason_code="control_websocket_recovered",
        now_ns=10_001,
    )
    ingress.close()


def test_control_source_keeps_only_the_newest_unapplied_owner_frame() -> None:
    class NoReadService:
        def __getattr__(self, name: str):
            raise AssertionError(f"capacity-one source must not read {name}")

    ingress = _ingress([])

    async def exercise() -> None:
        source = HeadlessControlStateSource(
            NoReadService(),  # type: ignore[arg-type]
            ingress,
            program_runner_websocket_url=None,
        )
        subscription = ControlStateSubscription(asyncio.Event())
        source._subscriptions.add(subscription)  # noqa: SLF001
        source._cycle = _cycle()  # noqa: SLF001
        source._facts = _facts_for_cycle(_cycle())  # noqa: SLF001
        source._shell = _shell_for_cycle(_cycle())  # noqa: SLF001
        source._program_source_current = True  # noqa: SLF001
        cold_wakes: list[None] = []
        source.notify_cold = lambda: cold_wakes.append(None)  # type: ignore[method-assign]
        for node_revision in (2, 3, 4):
            source._accept_cycle(  # noqa: SLF001
                {**_cycle(), "node_status_revision": node_revision},
                "node_progress",
            )
        frame = source.take(subscription)
        assert frame is not None
        assert frame.event_class == "node_progress"
        assert frame.shell["program_cycle"]["node_status_revision"] == 4
        assert subscription.frame is None
        assert cold_wakes == []
        source._subscriptions.clear()  # noqa: SLF001
        await source.close()

    try:
        asyncio.run(exercise())
    finally:
        ingress.close()


def test_slow_control_client_cannot_delay_or_backlog_another_client() -> None:
    class NoReadService:
        def __getattr__(self, name: str):
            raise AssertionError(f"capacity-one source must not read {name}")

    ingress = _ingress([])

    async def exercise() -> None:
        source = HeadlessControlStateSource(
            NoReadService(),  # type: ignore[arg-type]
            ingress,
            program_runner_websocket_url=None,
        )
        slow = ControlStateSubscription(asyncio.Event())
        current = ControlStateSubscription(asyncio.Event())
        source._subscriptions.update({slow, current})  # noqa: SLF001
        cycle = _cycle()
        source._cycle = cycle  # noqa: SLF001
        source._facts = _facts_for_cycle(cycle)  # noqa: SLF001
        source._shell = _shell_for_cycle(cycle)  # noqa: SLF001
        source._program_source_current = True  # noqa: SLF001

        source._accept_cycle(  # noqa: SLF001
            {**cycle, "node_status_revision": 2},
            "node_progress",
        )
        first = source.take(current)
        assert first is not None
        assert first.shell["program_cycle"]["node_status_revision"] == 2

        source._accept_cycle(  # noqa: SLF001
            {**cycle, "node_status_revision": 3},
            "node_progress",
        )
        newest = source.take(current)
        retained = source.take(slow)
        assert newest is not None and retained is not None
        assert newest.shell["program_cycle"]["node_status_revision"] == 3
        assert retained.shell["program_cycle"]["node_status_revision"] == 3
        source._subscriptions.clear()  # noqa: SLF001
        await source.close()

    try:
        asyncio.run(exercise())
    finally:
        ingress.close()


def test_control_source_reduces_new_cycle_from_cached_owner_facts() -> None:
    class NoReadService:
        def __getattr__(self, name: str):
            raise AssertionError(f"capacity-one source must not read {name}")

    ingress = _ingress([])

    async def exercise() -> None:
        source = HeadlessControlStateSource(
            NoReadService(),  # type: ignore[arg-type]
            ingress,
            program_runner_websocket_url=None,
        )
        subscription = ControlStateSubscription(asyncio.Event())
        source._subscriptions.add(subscription)  # noqa: SLF001
        original_cycle = _cycle()
        source._cycle = original_cycle  # noqa: SLF001
        source._facts = _facts_for_cycle(original_cycle)  # noqa: SLF001
        source._shell = _shell_for_cycle(original_cycle)  # noqa: SLF001
        source._program_source_current = True  # noqa: SLF001
        cold_wakes: list[None] = []
        source.notify_cold = lambda: cold_wakes.append(None)  # type: ignore[method-assign]

        replacement_cycle = {**original_cycle, "cycle_generation": 3}
        source._accept_cycle(replacement_cycle, "critical")  # noqa: SLF001
        frame = source.take(subscription)
        assert frame is not None
        assert frame.event_class == "critical"
        assert frame.shell["program_cycle"]["cycle_generation"] == 3
        assert frame.shell["component_overview"]["basis"][
            "program_cycle_generation"
        ] == 3
        assert cold_wakes == [None]
        source._subscriptions.clear()  # noqa: SLF001
        await source.close()

    try:
        asyncio.run(exercise())
    finally:
        ingress.close()


def test_standalone_overview_reuses_active_control_fact_reduction() -> None:
    class NoReadService:
        def __getattr__(self, name: str):
            raise AssertionError(f"active Overview must not read {name}")

    ingress = _ingress([])
    source = HeadlessControlStateSource(
        NoReadService(),  # type: ignore[arg-type]
        ingress,
        program_runner_websocket_url=None,
    )
    subscription = ControlStateSubscription(asyncio.Event())
    source._subscriptions.add(subscription)  # noqa: SLF001
    cycle = _cycle()
    source._cycle = cycle  # noqa: SLF001
    source._facts = _facts_for_cycle(cycle)  # noqa: SLF001
    source._shell = _shell_for_cycle(cycle)  # noqa: SLF001
    source._program_source_current = True  # noqa: SLF001

    overview = source.current_component_overview()

    assert overview == source._shell["component_overview"]  # noqa: SLF001
    assert overview is not source._shell["component_overview"]  # noqa: SLF001
    source._subscriptions.clear()  # noqa: SLF001
    assert source.current_component_overview() is None
    ingress.close()
