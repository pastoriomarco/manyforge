from __future__ import annotations

import asyncio
import json
import threading
from typing import Any

from fastapi import WebSocketDisconnect
import pytest

from manyforge_headless_api.control_protocol import (
    CONTROL_MAXIMUM_CLIENTS,
    CONTROL_MAXIMUM_MESSAGES_PER_SECOND,
    CONTROL_PROTOCOL_VERSION,
    EXECUTION_COMMAND_SCHEMA,
    ControlFreshnessAuthority,
    ExecutionControlCommandDTO,
    ExecutionIngressResult,
)
from manyforge_headless_api.control_session import (
    ExecutionControlSession,
    _CONTROL_DEAD_PEER_TIMEOUT_NS,
    _OutboundSlots,
)
from manyforge_headless_api.control_state import (
    ControlStateFrame,
    ControlStateSubscription,
    ControlStateUnavailableError,
    HeadlessControlStateSource,
    _fresh_runtime_expiry_delay_ns,
    build_control_projection,
)
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
        "snapshot",
    ]
    assert socket.sent[1]["complete"] is True
    assert socket.sent[1]["control"]["cycle_state"] == "idle_after_reset"


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
        "server_hello"
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


def test_heartbeat_keeps_one_challenge_outstanding_and_reclaims_a_dead_peer() -> None:
    class CountingSource(_Source):
        def __init__(self, frame: ControlStateFrame) -> None:
            super().__init__(frame)
            self.current_calls = 0

        def current(self) -> ControlStateFrame:
            self.current_calls += 1
            return super().current()

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
        assert source.current_calls == 1

        # UI staleness at one second disables commands, but does not create
        # more server work or tear down a recoverable transport.
        assert session._next_heartbeat(now_ns + 1_000_000_001) is None  # noqa: SLF001
        assert source.current_calls == 1

        session._last_echoed_challenge = 1  # noqa: SLF001
        session._last_client_sequence = 1  # noqa: SLF001
        session._last_heartbeat_ns = now_ns + 1_000_000_001  # noqa: SLF001
        second = session._next_heartbeat(  # noqa: SLF001
            now_ns + 1_250_000_001
        )
        assert second is not None
        assert second.challenge_sequence == "2"
        assert second.received_client_sequence == "1"
        assert source.current_calls == 2

        with pytest.raises(TimeoutError, match="stopped answering"):
            session._next_heartbeat(  # noqa: SLF001
                session._last_heartbeat_ns  # noqa: SLF001
                + _CONTROL_DEAD_PEER_TIMEOUT_NS
                + 1
            )
    finally:
        ingress.close()


def test_source_coherence_gap_pauses_readiness_without_retiring_the_peer() -> None:
    class TemporarilyUnavailableSource(_Source):
        def current(self) -> ControlStateFrame:
            raise ControlStateUnavailableError("control state is not synchronized")

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
        assert session._next_heartbeat(  # noqa: SLF001
            now_ns + _CONTROL_DEAD_PEER_TIMEOUT_NS + 1
        ) is None
        assert session._challenge_sequence == 0  # noqa: SLF001
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


def test_cold_freshness_uses_one_deadline_and_cancels_it_without_clients() -> None:
    runtime = {
        "sections": {
            "manager": {"freshness": "fresh", "receiver_age_ns": 400},
            "logical_scene": {"freshness": "fresh", "receiver_age_ns": 250},
            "scene_runtime": {"freshness": "stale", "receiver_age_ns": 900},
        }
    }
    assert _fresh_runtime_expiry_delay_ns(runtime, 1_000) == 601
    assert _fresh_runtime_expiry_delay_ns(
        {"sections": {"manager": {"freshness": "stale"}}},
        1_000,
    ) is None

    ingress = _ingress([])

    async def exercise() -> None:
        source = HeadlessControlStateSource(
            object(),  # type: ignore[arg-type]
            ingress,
            program_runner_websocket_url=None,
            cold_freshness_budget_ns=1_000,
        )
        source._loop = asyncio.get_running_loop()  # noqa: SLF001
        subscription = ControlStateSubscription(asyncio.Event())
        source._subscriptions.add(subscription)  # noqa: SLF001
        source._schedule_cold_expiry({"runtime": runtime})  # noqa: SLF001
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
        source._shell = _shell_for_cycle(_cycle())  # noqa: SLF001
        source._program_source_current = True  # noqa: SLF001
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
        source._subscriptions.clear()  # noqa: SLF001
        await source.close()

    try:
        asyncio.run(exercise())
    finally:
        ingress.close()


def test_control_source_never_publishes_a_cycle_with_a_stale_shell_basis() -> None:
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
        source._shell = _shell_for_cycle(original_cycle)  # noqa: SLF001
        source._program_source_current = True  # noqa: SLF001

        replacement_cycle = {**original_cycle, "cycle_generation": 3}
        source._accept_cycle(replacement_cycle, "critical")  # noqa: SLF001
        assert source.take(subscription) is None
        with pytest.raises(RuntimeError, match="not synchronized"):
            source.current()

        source._shell = _shell_for_cycle(replacement_cycle)  # noqa: SLF001
        source._publish("cold")  # noqa: SLF001
        frame = source.take(subscription)
        assert frame is not None
        assert frame.shell["program_cycle"]["cycle_generation"] == 3
        assert frame.shell["component_overview"]["basis"][
            "program_cycle_generation"
        ] == 3
        source._subscriptions.clear()  # noqa: SLF001
        await source.close()

    try:
        asyncio.run(exercise())
    finally:
        ingress.close()
