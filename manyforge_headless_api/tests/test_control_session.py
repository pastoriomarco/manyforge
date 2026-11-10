from __future__ import annotations

import asyncio
from dataclasses import replace
import json
import hashlib
import threading
from types import SimpleNamespace
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
from manyforge_headless_api.execution import ExecutionEventPump
from manyforge_headless_api.tests.paged_event_stream import paged_subscription
from manyforge_headless_api.execution_control import (
    ExecutionControlBusyError,
    ExecutionControlLane,
)
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.service import HeadlessService
from manyforge_headless_api import control_state as control_state_module
from manyforge_headless_api.program_cycle import (
    program_cycle_event_class as _program_event_class,
)
from manyforge_headless_api.tests.source_snapshot_fixture import source_snapshot_text


def _cycle() -> dict[str, Any]:
    return {
        "program_runner_owner_epoch": "runner.1",
        "execution_control_revision": 4,
        "cycle_generation": 2,
        "state": "idle_after_reset",
        "program_run_id": None,
        "run_revision": None,
        "node_status_revision": None,
        "runtime_control_revision": None,
        "run": None,
        "runtime_controls": None,
        "recovery_offer": None,
        "cycle_claim_generation": None,
        "terminal_or_settlement_reference": None,
        "reset_phase": None,
        "reset_options": None,
        "reset_robot_position": {"available": False, "reason": "unsupported"},
        "current_command_kind": None,
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


def _running_cycle(node_revision: int) -> dict[str, Any]:
    digest = "sha256:" + "a" * 64
    capture = {
        key: digest
        for key in (
            "applied_record_digest",
            "source_document_digest",
            "program_revision",
            "program_binding_id",
            "retained_bundle_id",
            "graph_revision",
            "node_environment_digest",
        )
    }
    capture.update(
        {
            "schema_version": "manyforge.program_run_capture.v2",
            "program_run_id": "run.1",
            "scenario_id": "pick",
            "program_id": "program.pick",
            "node_policies": [],
            "node_policy_digest": "sha256:" + hashlib.sha256(b"[]").hexdigest(),
            "owner_identities": [
                {
                    "owner_role": "program_runner",
                    "owner_id": "runner",
                    "owner_epoch": "runner.1",
                }
            ],
        }
    )
    run = {
        "schema_version": "manyforge.program_run_status.v4",
        "owner_id": "runner",
        "owner_epoch": "runner.1",
        "run_revision": 1,
        "capture": capture,
        "state": "running",
        "tree_status": "running",
        "node_status_revision": node_revision,
        "node_statuses": {"root": "running"},
        "active_operation_count": 1,
        "reason_code": None,
        "message": "",
        "details": None,
        "origin": None,
        "operation_failures": [],
    }
    return {
        **_cycle(),
        "state": "running",
        "program_run_id": "run.1",
        "cycle_claim_generation": 1,
        "run": run,
        "run_revision": 1,
        "node_status_revision": node_revision,
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
                "program_runner_owner_epoch": cycle["program_runner_owner_epoch"],
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
        component_model=None,
        errors={},
    )


def test_event_pump_reopens_admission_for_existing_session_without_events() -> None:
    async def exercise() -> None:
        admission = {"admission_generation": 29, "allowed": False}
        identity_reads: list[None] = []
        ingested: list[dict[str, Any]] = []
        loop = asyncio.get_running_loop()
        baseline_observed = asyncio.Event()

        def identity():
            identity_reads.append(None)
            # Ready identity deliberately has no admission generation.
            return {"ok": True, "identity": {"owner_epoch": "execution.1"}}

        def events(owner_epoch, after_sequence, *, limit):
            return {
                "owner_epoch": owner_epoch,
                "execution_owner_ready": True,
                "runtime_faults": {
                    "operation_journal_write_fault": None,
                    "program_runner_loss": None,
                    "kernel_internal_fault": None,
                },
                "operation_progress": [],
                "admission_generation": admission["admission_generation"],
                "events": [],
                "has_more": False,
                "next_after_sequence": after_sequence,
            }

        def facts(*, program_cycle, cancelled=None):
            if identity_reads and admission["admission_generation"] == 29:
                loop.call_soon_threadsafe(baseline_observed.set)
            return replace(
                _facts_for_cycle(program_cycle), authoring_admission=dict(admission)
            )

        ingress = _ingress([])
        source = HeadlessControlStateSource(
            SimpleNamespace(program_cycle_status=_cycle, component_overview_facts=facts),
            ingress,
            program_runner_websocket_url=None,
        )
        pump = ExecutionEventPump(
            SimpleNamespace(
                ingest_operation_event=lambda **row: ingested.append(row),
                complete_operation_event_snapshot=lambda **kwargs: None,
                mark_operation_event_source_unknown=lambda **kwargs: None,
                observe_runtime_fault_conditions=lambda **kwargs: {
                    "status": "observed",
                    "changed": False,
                },
            ),
            execution := SimpleNamespace(
                identity=identity,
                events=events,
                subscribe_events=lambda after_cursor, owner_epoch: paged_subscription(
                    execution, after_cursor, owner_epoch
                ),
            ),
            poll_seconds=0.01,
        )
        pump.set_change_observer(source.notify_cold)
        subscription = await source.subscribe()
        try:
            await asyncio.wait_for(subscription.wake.wait(), 1)
            previous = source.take(subscription)
            assert previous.shell["authoring_admission"] == admission
            pump.start()
            await asyncio.wait_for(baseline_observed.wait(), 1)
            # Reset has finished; only the Kernel admission generation advances.
            admission = {"admission_generation": 30, "allowed": True}
            await asyncio.wait_for(subscription.wake.wait(), 1)
            current = source.take(subscription)
            session = ExecutionControlSession(object(), source, ingress)
            delta = session._delta_message(current, previous)  # noqa: SLF001
            assert delta.shell["authoring_admission"] == admission
            assert current.shell["program_cycle"] == previous.shell["program_cycle"]
            assert identity_reads == [None]  # no reconnect
            assert ingested == []
            assert pump.last_error is None
        finally:
            pump.shutdown()
            await source.unsubscribe(subscription)
            ingress.close()

    asyncio.run(exercise())


@pytest.mark.parametrize(
    "target_owner,control_owner",
    [
        (None, "new"),
        ("old", None),
        ("old", "new"),
        ("new", "new"),
    ],
)
def test_control_shell_never_publishes_a_partial_or_crossed_execution_pair(
    target_owner,
    control_owner,
) -> None:
    targets = (
        None
        if target_owner is None
        else {"ok": True, "identity": {"owner_epoch": target_owner}}
    )
    control = (
        None
        if control_owner is None
        else {"ok": True, "identity": {"owner_epoch": control_owner}}
    )
    facts = replace(
        _facts_for_cycle(_cycle()),
        execution_targets_response=targets,
        execution_control_response=control,
    )
    shell = HeadlessControlStateSource._shell_from_facts(facts)
    coherent = target_owner is not None and target_owner == control_owner
    assert shell["execution_targets"] == (targets if coherent else None)
    assert shell["execution_control_state"] == (control if coherent else None)
    assert facts.execution_targets_response is targets
    assert facts.execution_control_response is control


@pytest.mark.parametrize("paired", [True, False])
def test_control_shell_publishes_the_held_run_draft_edit_rule(paired) -> None:
    held = {
        **_cycle(),
        "state": "running",
        "program_run_id": "run.1",
        "run": {"state": "running", "capture": {"program_run_id": "run.1"}},
        "recovery_offer": None,
        "runtime_controls": {"execution_selections": {"motion_held": True}},
    }
    identity = {"owner_epoch": "kernel.1"}
    facts = replace(
        _facts_for_cycle(held),
        execution_targets_response={"ok": True, "identity": identity},
        execution_control_response={
            "ok": True,
            "identity": identity if paired else {"owner_epoch": "kernel.2"},
            "control_state": {"manual_operations": []},
        },
    )
    shell = HeadlessControlStateSource._shell_from_facts(facts)
    # Manual-operation settlement is only proven by a coherent control capture.
    assert shell["held_run_draft_edit_allowed"] is paired
    idle = HeadlessControlStateSource._shell_from_facts(_facts_for_cycle(_cycle()))
    assert idle["held_run_draft_edit_allowed"] is False


@pytest.mark.parametrize(
    "targets_ok,control_ok", [(False, False), (False, True), (True, False)]
)
def test_control_shell_preserves_failed_native_responses_only_as_diagnostic_facts(
    targets_ok,
    control_ok,
) -> None:
    facts = replace(
        _facts_for_cycle(_cycle()),
        execution_targets_response={"ok": targets_ok, "error": "owner_unavailable"},
        execution_control_response={"ok": control_ok, "error": "owner_unavailable"},
    )
    shell = HeadlessControlStateSource._shell_from_facts(facts)
    assert shell["execution_targets"] is None
    assert shell["execution_control_state"] is None
    assert facts.execution_targets_response["ok"] is targets_ok
    assert facts.execution_control_response["ok"] is control_ok


def test_control_session_starts_with_versioned_current_snapshot() -> None:
    stamps: list[Any] = []
    ingress = _ingress(stamps)
    frame = _frame(ingress)
    source = _Source(frame)
    socket = _HandshakeSocket()
    try:
        asyncio.run(
            ExecutionControlSession(socket, source, ingress).serve()  # type: ignore[arg-type]
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


def test_operation_progress_reuses_cached_facts_and_sends_only_progress() -> None:
    async def exercise() -> None:
        ingress = _ingress([])
        source = HeadlessControlStateSource(
            SimpleNamespace(program_cycle_status=_cycle, component_overview_facts=lambda **_kwargs: _facts_for_cycle(_cycle())),
            ingress, program_runner_websocket_url=None,
        )
        sample = {"owner_epoch": "owner.1", "operation_id": "operation.1", "operation_revision": 1, "progress": 0.5}
        source.notify_operation_progress([{**sample, "progress": 0.1}])
        subscription = await source.subscribe()
        try:
            await asyncio.wait_for(subscription.wake.wait(), 1)
            previous = source.take(subscription)
            assert previous.shell["operation_progress"] == [{**sample, "progress": 0.1}]
            reads = source.diagnostics()["owner_reads"]
            source.notify_operation_progress([{**sample, "progress": 0.1}])
            source.notify_operation_progress([sample])
            await asyncio.wait_for(subscription.wake.wait(), 1)
            current = source.take(subscription)
            assert current is not None
            assert source.diagnostics()["owner_reads"] == reads
            session = ExecutionControlSession(object(), source, ingress)
            assert session._delta_message(current, previous).shell == {
                "complete": False, "operation_progress": [sample],
            }
        finally:
            await source.unsubscribe(subscription)
            ingress.close()
    asyncio.run(exercise())


def test_global_rules_ride_the_shell_beside_progress_without_owner_reads() -> None:
    async def exercise() -> None:
        ingress = _ingress([])
        source = HeadlessControlStateSource(
            SimpleNamespace(program_cycle_status=_cycle, component_overview_facts=lambda **_kwargs: _facts_for_cycle(_cycle())),
            ingress, program_runner_websocket_url=None,
        )
        rules = [{"rule_id": "door", "state": "inactive", "keys": []}]
        subscription = await source.subscribe()
        try:
            await asyncio.wait_for(subscription.wake.wait(), 1)
            previous = source.take(subscription)
            assert previous.shell["global_rules"] is None
            assert previous.shell["operation_progress"] == []
            reads = source.diagnostics()["owner_reads"]
            source.notify_global_rules(rules)
            await asyncio.wait_for(subscription.wake.wait(), 1)
            current = source.take(subscription)
            assert source.diagnostics()["owner_reads"] == reads
            session = ExecutionControlSession(object(), source, ingress)
            assert session._delta_message(current, previous).shell == {
                "complete": False, "global_rules": rules,
            }
            source.notify_global_rules(list(rules))  # unchanged: no publication
            await asyncio.sleep(0.05)
            assert not subscription.wake.is_set()
        finally:
            await source.unsubscribe(subscription)
            ingress.close()
    asyncio.run(exercise())


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
    session = ExecutionControlSession(object(), _Source(initial), ingress)  # type: ignore[arg-type]
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


def test_websocket_command_uses_the_same_ingress_without_waiting_for_state_poll() -> (
    None
):
    stamps: list[Any] = []
    ingress = _ingress(stamps)
    frame = _frame(ingress)
    source = _Source(frame)
    socket = _CommandSocket(frame)
    try:
        asyncio.run(
            ExecutionControlSession(socket, source, ingress).serve()  # type: ignore[arg-type]
        )
    finally:
        ingress.close()

    receipts = [
        message
        for message in socket.sent
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
            ExecutionControlSession(socket, source, ingress).serve()  # type: ignore[arg-type]
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


def test_outbound_state_and_heartbeat_are_latest_only_while_receipts_keep_order() -> (
    None
):
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

            assert (
                await session._slots.take(state_allowed=allowed) == "heartbeat"
            )  # noqa: SLF001
            assert (
                await session._slots.take(state_allowed=allowed)
                == "newest-state"  # noqa: SLF001
            )
        finally:
            session._slots.close()  # noqa: SLF001
            ingress.close()

    asyncio.run(exercise())


def test_one_state_credit_bounds_delivery_without_blocking_receipt_or_heartbeat() -> (
    None
):
    async def exercise() -> None:
        ingress = _ingress([])
        source = _Source(_frame(ingress))
        socket = _HandshakeSocket()
        session = ExecutionControlSession(socket, source, ingress)  # type: ignore[arg-type]
        try:
            await session._send_message(  # noqa: SLF001
                session._snapshot_message(source.frame)  # noqa: SLF001
            )
            assert session._state_inflight == 1  # noqa: SLF001

            session._put_state("newest-state")  # noqa: SLF001
            session._slots.put_heartbeat("heartbeat")  # noqa: SLF001
            session._slots.put_receipt("receipt")  # noqa: SLF001
            state_allowed = lambda: session._state_inflight is None  # noqa: E731,SLF001

            assert (
                await session._slots.take(state_allowed=state_allowed)
                == "receipt"  # noqa: SLF001
            )
            assert (
                await session._slots.take(state_allowed=state_allowed)  # noqa: SLF001
                == "heartbeat"
            )

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
        first = session._next_heartbeat(now_ns + 250_000_000)  # noqa: SLF001
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
        second = session._next_heartbeat(now_ns + 1_250_000_001)  # noqa: SLF001
        assert second is not None
        assert second.challenge_sequence == "2"
        assert second.received_client_sequence == "1"
        assert source.current_calls == 0
        assert source.semantic_calls == 2

        with pytest.raises(TimeoutError, match="stopped answering"):
            session._next_heartbeat(  # noqa: SLF001
                session._last_heartbeat_ns
                + _CONTROL_DEAD_PEER_TIMEOUT_NS
                + 1  # noqa: SLF001
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
        challenge = session._next_heartbeat(now_ns + 250_000_000)  # noqa: SLF001
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
        assert (
            session._next_heartbeat(
                now_ns + CONTROL_HEARTBEAT_DELAYED_NS + 1
            )  # noqa: SLF001
            is None
        )
        assert (
            session._next_heartbeat(now_ns + CONTROL_HEARTBEAT_STALE_NS + 1)
            is None  # noqa: SLF001
        )
        assert [
            (event["incident_kind"], event["phase"]) for event in source.incidents
        ] == [
            ("control_delayed", "activated"),
            ("control_stale", "activated"),
        ]

        session._last_echoed_challenge = 1  # noqa: SLF001
        session._resolve_heartbeat_incidents(  # noqa: SLF001
            now_ns + CONTROL_HEARTBEAT_STALE_NS + 2,
            CONTROL_HEARTBEAT_STALE_NS + 2,
        )
        assert len(source.incidents) == 2  # The late echo is not recovery.
        session._challenge_issued_ns = None  # noqa: SLF001
        session._delay_level = 0  # noqa: SLF001
        # Consecutive delayed challenges join the same two bounded episodes.
        for index in range(2, 102):
            issued = now_ns + index * 2_000_000_000
            assert session._next_heartbeat(issued) is not None  # noqa: SLF001
            session._classify_outstanding_heartbeat(  # noqa: SLF001
                issued + CONTROL_HEARTBEAT_STALE_NS + 1
            )
            session._resolve_heartbeat_incidents(  # noqa: SLF001
                issued + CONTROL_HEARTBEAT_STALE_NS + 2,
                CONTROL_HEARTBEAT_STALE_NS + 2,
            )
            session._last_echoed_challenge = index  # noqa: SLF001
            session._challenge_issued_ns = None  # noqa: SLF001
            session._delay_level = 0  # noqa: SLF001
        assert len(source.incidents) == 2
        assert len(session._server_incidents) == 2  # noqa: SLF001
        session._resolve_heartbeat_incidents(
            now_ns + 205_000_000_000, 10_000_000
        )  # noqa: SLF001
        assert [
            (event["incident_kind"], event["phase"]) for event in source.incidents
        ] == [
            ("control_delayed", "activated"),
            ("control_stale", "activated"),
            ("control_delayed", "resolved"),
            ("control_stale", "resolved"),
        ]
        assert source.delay_transitions == [False, True] * 101
        for activated, resolved in zip(source.incidents[:2], source.incidents[2:]):
            assert resolved["incident_id"] == activated["incident_id"]
            assert resolved["first_observed_ns"] == activated["first_observed_ns"]
            assert resolved["occurrence_count"] == 101
            assert resolved["maximum_delay_ns"] == CONTROL_HEARTBEAT_STALE_NS + 2
        assert not session._server_incidents  # noqa: SLF001
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


@pytest.mark.parametrize("section,path,value", [
    ("manager", ("apply", "records", 0, "incomplete"), False),
    ("manager", ("apply", "records", 0, "phase"), "terminal"),
    ("manager", ("apply", "records", 0, "terminal_state"), "failed"),
    ("manager", ("apply", "mutation_available"), False),
    ("manager", ("manager_ready",), True),
    ("manager", ("runtime", "converged"), True),
    ("manager", ("runtime", "observed_units", 0, "source_health"), "faulted"),
    ("manager", ("runtime", "observed_units", 0, "critical_progress_sequence"), 2),
    ("manager", ("active", "graph_revision"), "graph.2"),
    ("scene_runtime", ("state", "state"), "open"),
    ("scene_runtime", ("state", "error"), "initialization_failed"),
    ("scene_runtime", ("state", "initialization_generation"), 2),
])
def test_runtime_semantics_reach_existing_session_delta(section, path, value) -> None:
    ingress = _ingress([])
    cycle = _cycle()
    shell = _shell_for_cycle(cycle)
    sections = shell["runtime"]["sections"]
    sections["manager"]["payload"] = {
        "manager_ready": False,
        "active": {"graph_revision": "graph.1"},
        "apply": {"mutation_available": True, "records": [{
            "apply_id": "apply.1", "incomplete": True,
            "phase": "runtime_converged", "terminal_state": None,
        }]},
        "runtime": {"converged": False, "observed_units": [{
            "source_health": "healthy", "critical_progress_sequence": 1,
        }]},
    }
    sections["scene_runtime"]["payload"] = {"state": {
        "state": "initializing", "error": None, "initialization_generation": 1,
    }}
    previous = replace(_frame(ingress), shell=shell)
    changed = json.loads(json.dumps(shell))
    target = changed["runtime"]["sections"][section]["payload"]
    for key in path[:-1]:
        target = target[key]
    target[path[-1]] = value
    current = replace(previous, generation=2, shell=changed)
    session = ExecutionControlSession(object(), _Source(previous), ingress)
    try:
        # No Scene revision or metadata change accompanies the owner change.
        # Both assembler publication and an existing session must carry it.
        assert _shell_key(changed) != _shell_key(shell)
        delta = session._delta_message(current, previous)  # noqa: SLF001
        assert delta.shell == {"complete": False, "runtime": changed["runtime"]}
        assert {**shell, **{k: v for k, v in delta.shell.items() if k != "complete"}} == changed
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
    first_runtime["sections"]["manager"]["payload"] = {"runtime": {
        "observed_units": [{"process_observation_sequence": 1, "source_report_sequence": 1}],
    }}
    first_runtime["sections"]["scene_runtime"]["payload"] = {"state": {
        "state": "open", "updated_at_ns": 1, "pose_sources": [{"source_sequence": 1}],
    }}
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
    later["runtime"]["sections"]["manager"]["source_content_digest"] = "idle-report"
    later["runtime"]["sections"]["manager"]["payload"]["runtime"]["observed_units"][0].update(
        process_observation_sequence=2, source_report_sequence=2,
    )
    later["runtime"]["sections"]["scene_runtime"]["payload"]["state"].update(
        updated_at_ns=2, pose_sources=[{"source_sequence": 2}],
    )

    assert _shell_key(later) == _shell_key(first)
    ingress = _ingress([])
    previous = replace(_frame(ingress), shell=first)
    session = ExecutionControlSession(object(), _Source(previous), ingress)
    try:
        delta = session._delta_message(replace(previous, shell=later), previous)
        assert delta.shell == {"complete": False}
    finally:
        ingress.close()

    alarm_change = json.loads(json.dumps(later))
    alarm_change["alarms"]["generation"] += 1
    assert _shell_key(alarm_change) != _shell_key(first)


def test_source_file_freshness_pulse_does_not_wake_owner_reduction(tmp_path) -> None:
    source_path = tmp_path / "manager-source.json"

    def publish(*, generation: int, report: int, health: str = "healthy") -> None:
        source_path.write_text(
            source_snapshot_text(
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
        asyncio.run(source._refresh_file_watches(notify=False))  # noqa: SLF001
        publish(generation=2, report=2)
        asyncio.run(source._refresh_file_watches(notify=True))  # noqa: SLF001
        assert wakes == []

        publish(generation=3, report=3, health="faulted")
        asyncio.run(source._refresh_file_watches(notify=True))  # noqa: SLF001
        assert wakes == [True]
    finally:
        ingress.close()


def test_scene_runtime_pose_only_update_does_not_wake_cold_projection(
    tmp_path,
) -> None:
    source_path = tmp_path / "scene-runtime-source.json"

    def publish(*, generation: int, sequence: int, state: str = "open") -> None:
        source_path.write_text(
            source_snapshot_text(
                {
                    "api_version": "manyforge.source_snapshot.v1",
                    "observed_at_ns": sequence,
                    "payload": {
                        "api_version": "manyforge.scene_runtime_source.v1",
                        "cell_id": "cell.1",
                        "state": {
                            "api_version": "manyforge.scene_runtime_state.v2",
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
        asyncio.run(source._refresh_file_watches(notify=False))  # noqa: SLF001
        publish(generation=2, sequence=2)
        asyncio.run(source._refresh_file_watches(notify=True))  # noqa: SLF001
        assert wakes == []

        publish(generation=3, sequence=3, state="resetting")
        asyncio.run(source._refresh_file_watches(notify=True))  # noqa: SLF001
        assert wakes == [True]
    finally:
        ingress.close()


def test_server_sequence_is_assigned_after_latest_slot_replacement() -> None:
    async def exercise() -> None:
        ingress = _ingress([])
        source = _Source(_frame(ingress))
        socket = _HandshakeSocket()
        session = ExecutionControlSession(socket, source, ingress)  # type: ignore[arg-type]
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
        session = ExecutionControlSession(socket, source, ingress)  # type: ignore[arg-type]
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


@pytest.mark.parametrize(
    "malformed,event_class",
    [
        ({"state": "running"}, "critical"),
        ({**_cycle(), "node_status_revision": 0}, "critical"),
        ({**_cycle(), "state": "secret-frame-value"}, "critical"),
        (
            {
                key: value
                for key, value in _cycle().items()
                if key != "runtime_controls"
            },
            "critical",
        ),
        ({**_cycle(), "run_revision": 1}, "critical"),
        ({**_cycle(), "runtime_control_revision": 1}, "critical"),
        (_cycle(), None),
        (_cycle(), "unknown"),
        (_cycle(), {}),
        *[
            (
                {
                    **_running_cycle(1),
                    "run": {**_running_cycle(1)["run"], field: value},
                },
                "critical",
            )
            for field, value in (
                ("state", []),
                ("tree_status", {}),
                ("node_statuses", {"root": []}),
                ("details", {"unregistered": 1}),
                (
                    "origin",
                    {"kind": "kernel", "node_instance_id": None, "owner_api": None},
                ),
            )
        ],
    ],
)
def test_program_source_recovers_same_current_cycle_after_malformed_identity(
    monkeypatch, caplog, malformed, event_class
) -> None:
    async def exercise() -> None:
        ingress = _ingress([])
        requests: asyncio.Queue[tuple[dict[str, Any], Any]] = asyncio.Queue()
        attempts: asyncio.Queue[int] = asyncio.Queue()
        count = 0

        class Upstream:
            async def __aenter__(self):
                nonlocal count
                count += 1
                self.sequence = 0
                attempts.put_nowait(count)
                return self

            async def __aexit__(self, *_args):
                return None

            def __aiter__(self):
                return self

            async def __anext__(self):
                cycle, classification = await requests.get()
                self.sequence += 1
                return json.dumps(
                    {
                        "schema_version": "manyforge.program_control_state.v2",
                        "server_sequence": str(self.sequence),
                        **(
                            {"event_class": classification}
                            if classification is not None
                            else {}
                        ),
                        "program_cycle": cycle,
                    }
                )

        monkeypatch.setattr(
            control_state_module, "connect_websocket", lambda *_a, **_k: Upstream()
        )
        monkeypatch.setattr(control_state_module, "_SOURCE_RETRY_INITIAL_SECONDS", 0)
        source = HeadlessControlStateSource(
            SimpleNamespace(
                program_cycle_status=_cycle,
                component_overview_facts=lambda *, program_cycle, cancelled=None: _facts_for_cycle(
                    program_cycle
                ),
            ),
            ingress,
            program_runner_websocket_url="ws://runner.test/state",
        )
        first = await source.subscribe()
        second = await source.subscribe()
        try:
            assert await asyncio.wait_for(attempts.get(), 1) == 1
            requests.put_nowait((_cycle(), "critical"))
            await asyncio.wait_for(first.wake.wait(), 1)
            source.take(first)
            source.take(second)
            # A new connection containing a bad identity invalidates semantics
            # without dropping either browser or poisoning the cached cycle.
            requests.put_nowait((malformed, event_class))
            assert await asyncio.wait_for(attempts.get(), 1) == 2
            assert "Program control source failure: type=" in caplog.text
            assert "secret-frame-value" not in caplog.text
            assert source.semantic_context().status == "unavailable"
            assert source._cycle == _cycle()  # noqa: SLF001
            assert source.take(first) is None and source.take(second) is None
            requests.put_nowait((malformed, event_class))
            assert await asyncio.wait_for(attempts.get(), 1) == 3
            requests.put_nowait((_cycle(), "critical"))
            await asyncio.wait_for(first.wake.wait(), 1)
            assert source.semantic_context().status == "current"
            assert source.take(first).shell["program_cycle"] == _cycle()
            assert source.take(second).shell["program_cycle"] == _cycle()
            assert not source._runner_task.done()  # noqa: SLF001
        finally:
            await source.unsubscribe(first)
            await source.unsubscribe(second)
            assert source._runner_task is None  # noqa: SLF001
            ingress.close()

    asyncio.run(exercise())


def test_program_source_logs_typed_owner_error(monkeypatch, caplog) -> None:
    async def exercise() -> None:
        attempts: asyncio.Queue[int] = asyncio.Queue()
        count = 0

        class Upstream:
            async def __aenter__(self):
                nonlocal count
                count += 1
                attempts.put_nowait(count)
                raise HeadlessApiError(
                    "program_cycle_reset_phase_invalid", "secret-message", details={"secret": "secret-detail"},
                    producer="program_runner",
                )

            async def __aexit__(self, *_args):
                return None

        monkeypatch.setattr(
            control_state_module, "connect_websocket", lambda *_a, **_k: Upstream()
        )
        ingress = _ingress([])
        source = HeadlessControlStateSource(
            SimpleNamespace(program_cycle_status=_cycle), ingress,
            program_runner_websocket_url="ws://runner.test/state",
        )
        subscription = await source.subscribe()
        try:
            assert await asyncio.wait_for(attempts.get(), 1) == 1
            assert await asyncio.wait_for(attempts.get(), 1) == 2
            assert "type=HeadlessApiError code=program_cycle_reset_phase_invalid" in caplog.text
            assert "secret-message" not in caplog.text
            assert "secret-detail" not in caplog.text
            assert caplog.text.count("Program control source failure:") == 1
            assert source.semantic_context().status == "unavailable"
        finally:
            await source.unsubscribe(subscription)
            ingress.close()

    asyncio.run(exercise())


def test_program_source_logs_clean_peer_close(monkeypatch, caplog) -> None:
    async def exercise() -> None:
        attempts: asyncio.Queue[None] = asyncio.Queue()

        class Upstream:
            async def __aenter__(self):
                attempts.put_nowait(None)
                return self

            async def __aexit__(self, *_args):
                return None

            def __aiter__(self):
                return self

            async def __anext__(self):
                raise StopAsyncIteration

        monkeypatch.setattr(
            control_state_module, "connect_websocket", lambda *_a, **_k: Upstream()
        )
        ingress = _ingress([])
        source = HeadlessControlStateSource(
            SimpleNamespace(program_cycle_status=_cycle), ingress,
            program_runner_websocket_url="ws://runner.test/state",
        )
        subscription = await source.subscribe()
        try:
            await asyncio.wait_for(attempts.get(), 1)
            await asyncio.wait_for(attempts.get(), 1)
            await asyncio.wait_for(attempts.get(), 1)
            assert "Program control source failure: type=peer_closed" in caplog.text
            assert caplog.text.count("Program control source failure:") == 1
            assert source.semantic_context().status == "unavailable"
        finally:
            await source.unsubscribe(subscription)
            ingress.close()

    asyncio.run(exercise())


def test_fallback_read_recovers_and_bursts_use_one_existing_task(monkeypatch) -> None:
    async def exercise() -> None:
        ingress = _ingress([])
        calls = 0
        loop = asyncio.get_running_loop()
        recovered_read = asyncio.Event()

        def read():
            nonlocal calls
            calls += 1
            if calls == 1:
                raise OSError("source temporarily unavailable")
            loop.call_soon_threadsafe(recovered_read.set)
            return _cycle()

        monkeypatch.setattr(control_state_module, "_SOURCE_RETRY_INITIAL_SECONDS", 0)
        source = HeadlessControlStateSource(
            SimpleNamespace(
                program_cycle_status=read,
                component_overview_facts=lambda *, program_cycle, cancelled=None: _facts_for_cycle(
                    program_cycle
                ),
            ),
            ingress,
            program_runner_websocket_url=None,
        )
        subscription = await source.subscribe()
        try:
            task = source._runner_task  # noqa: SLF001
            await asyncio.wait_for(subscription.wake.wait(), 1)
            assert source.semantic_context().status == "current"
            source.take(subscription)
            # Failed read, recovered read, and the cold reduction's direct
            # reset_robot_position read.
            assert calls == 3
            # Let a startup cold wake that landed in the combine window settle.
            await asyncio.sleep(0.2)
            before = calls
            recovered_read.clear()
            for _ in range(1000):
                source.notify_control()
            await asyncio.wait_for(recovered_read.wait(), 1)
            await asyncio.sleep(0.2)
            # The burst is one Program read plus one cold-reduction read, not
            # 1000 read tasks.
            assert calls == before + 2
            assert source._runner_task is task and not task.done()  # noqa: SLF001
            assert source.diagnostics()["source_failures"] == 1
        finally:
            await source.unsubscribe(subscription)
            assert (
                source._runner_task is None and source._cold_task is None
            )  # noqa: SLF001
            assert source._program_wake is None  # noqa: SLF001
            ingress.close()

    asyncio.run(exercise())


@pytest.mark.parametrize("missing_fact", [False, True])
def test_cold_source_failure_recovers_without_a_second_file_event(
    monkeypatch, missing_fact
) -> None:
    async def exercise() -> None:
        ingress = _ingress([])
        calls = 0

        def facts(*, program_cycle, cancelled=None):
            nonlocal calls
            calls += 1
            if calls == 1:
                if missing_fact:
                    return replace(
                        _facts_for_cycle(program_cycle),
                        runtime=None,
                        errors={"runtime": {"code": "runtime_source_unavailable"}},
                    )
                raise OSError("read temporarily failed")
            return _facts_for_cycle(program_cycle)

        monkeypatch.setattr(control_state_module, "_SOURCE_RETRY_INITIAL_SECONDS", 0)
        source = HeadlessControlStateSource(
            SimpleNamespace(
                program_cycle_status=_cycle, component_overview_facts=facts
            ),
            ingress,
            program_runner_websocket_url=None,
        )
        records = []
        source.record_incident = lambda **record: records.append(record)  # type: ignore[method-assign]
        subscription = await source.subscribe()
        try:
            await asyncio.wait_for(subscription.wake.wait(), 1)
            assert source.semantic_context().status == "current"
            assert calls == 2
            assert [(r["incident_kind"], r["phase"]) for r in records] == [
                ("control_source_unavailable", "activated"),
                ("control_source_unavailable", "resolved"),
            ]
            assert records[0]["incident_id"] == records[1]["incident_id"]
        finally:
            await source.unsubscribe(subscription)
            ingress.close()

    asyncio.run(exercise())


def test_incident_recorder_failure_is_counted_and_logged_once_per_episode(
    caplog,
) -> None:
    ingress = _ingress([])
    source = HeadlessControlStateSource(
        object(), ingress, program_runner_websocket_url=None
    )

    def fail(**_record):
        raise OSError("history sink failed")

    try:
        for _ in range(100):
            source._record_incident_now(fail, {})  # noqa: SLF001
        assert source.diagnostics()["incident_recording_failures"] == 100
        assert (
            sum("trace may be incomplete" in row.message for row in caplog.records) == 1
        )
        source._record_incident_now(lambda **_record: {}, {})  # noqa: SLF001
        source._record_incident_now(fail, {})  # noqa: SLF001
        assert (
            sum("trace may be incomplete" in row.message for row in caplog.records) == 2
        )
        source._record_incident_now(
            lambda **_record: {"recorded": False}, {}
        )  # noqa: SLF001
        assert source.diagnostics()["incident_recording_failures"] == 102
    finally:
        ingress.close()


def test_file_watcher_failure_invalidates_and_recovers_without_client_churn(
    monkeypatch, tmp_path
) -> None:
    async def exercise() -> None:
        ingress = _ingress([])
        fail = asyncio.Event()
        retry = asyncio.Event()
        count = 0

        async def watches(*_args):
            nonlocal count
            count += 1
            if count == 1:
                await fail.wait()
                raise OSError("watch root disappeared")
            retry.set()
            yield set()
            await asyncio.Event().wait()

        monkeypatch.setattr(control_state_module, "awatch", watches)
        monkeypatch.setattr(control_state_module, "_SOURCE_RETRY_INITIAL_SECONDS", 0)
        source = HeadlessControlStateSource(
            SimpleNamespace(
                program_cycle_status=_cycle,
                component_overview_facts=lambda *, program_cycle, cancelled=None: _facts_for_cycle(
                    program_cycle
                ),
            ),
            ingress,
            program_runner_websocket_url=None,
            cold_watch_paths=(tmp_path / "source.json",),
        )
        subscription = await source.subscribe()
        try:
            await asyncio.wait_for(subscription.wake.wait(), 1)
            source.take(subscription)
            fail.set()
            await asyncio.wait_for(retry.wait(), 1)
            await asyncio.wait_for(subscription.wake.wait(), 1)
            assert source.semantic_context().status == "current"
            assert source.diagnostics()["source_failures"] == 1
            assert not source._watch_task.done()  # noqa: SLF001
        finally:
            await source.unsubscribe(subscription)
            assert source._watch_task is None  # noqa: SLF001
            assert source._cold_expiry_handle is None  # noqa: SLF001
            ingress.close()

    asyncio.run(exercise())


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


def test_last_client_drains_physical_read_and_cancels_remaining_capture() -> None:
    async def exercise() -> None:
        ingress = _ingress([])
        loop = asyncio.get_running_loop()
        entered = asyncio.Event()
        released = threading.Event()
        calls: list[str] = []

        class Service:
            program_cycle_status = staticmethod(_cycle)
            component_overview_facts = HeadlessService.component_overview_facts

            def runtime(self):
                calls.append("runtime")
                loop.call_soon_threadsafe(entered.set)
                if not released.wait(2):
                    raise AssertionError("test did not release physical owner read")
                return {}

            def __getattr__(self, name):
                # No later owner in the old capture may be read after cancel.
                def read(*_args, **_kwargs):
                    calls.append(name)
                    return {}

                return read

        source = HeadlessControlStateSource(
            Service(), ingress, program_runner_websocket_url=None
        )
        first = await source.subscribe()
        await asyncio.wait_for(entered.wait(), 1)
        teardown = asyncio.create_task(source.unsubscribe(first))
        await asyncio.sleep(0)
        replacement = asyncio.create_task(source.subscribe())
        await asyncio.sleep(0)
        try:
            assert not teardown.done() and not replacement.done()
            assert calls == ["runtime"]
            released.set()
            await asyncio.wait_for(teardown, 1)
            second = await asyncio.wait_for(replacement, 1)
            # The replacement's cold combine has not begun. The old physical
            # call is finished and did not launch any of its seven later reads.
            assert calls == ["runtime"]
            await source.unsubscribe(second)
        finally:
            released.set()
            await source.close()
            ingress.close()

    asyncio.run(exercise())


@pytest.mark.parametrize("busy", [False, True])
def test_stop_rejection_does_not_require_initial_semantic_snapshot(
    monkeypatch, busy, caplog
) -> None:
    async def exercise() -> None:
        ingress = _ingress([])
        source = HeadlessControlStateSource(
            object(), ingress, program_runner_websocket_url=None
        )
        session = ExecutionControlSession(_HandshakeSocket(), source, ingress)
        session_id = session._session_id  # noqa: SLF001
        command = ExecutionControlCommandDTO.model_validate(
            {
                "schema_version": CONTROL_PROTOCOL_VERSION,
                "message_kind": "command",
                "control_session_id": session_id,
                "client_sequence": "1",
                "command": {
                    "command_envelope": {
                        "schema_version": EXECUTION_COMMAND_SCHEMA,
                        "command_kind": "program_stop",
                        "correlation_id": "stop.before.snapshot",
                        "source_label": "test",
                        "control_session_id": session_id,
                        "client_sequence": "1",
                        "freshness": ingress.freshness_for("runner.1", 4).model_dump(
                            mode="json"
                        ),
                        "expected_semantic_owner_epoch": "runner.1",
                        "expected_execution_control_revision": 4,
                    },
                    "payload": {},
                },
            }
        )
        failed = asyncio.get_running_loop().create_future()

        def start(*_args, **_kwargs):
            if busy:
                raise ExecutionControlBusyError(ExecutionControlLane.STOP, 1)
            failed.set_exception(OSError("Stop dispatch unavailable"))
            return failed

        monkeypatch.setattr(ingress, "start", start)
        try:
            assert source.semantic_context().status == "synchronizing"
            session._receive_command(command)  # noqa: SLF001
            await asyncio.sleep(0)
            receipts = list(session._slots.receipts)  # noqa: SLF001
            assert len(receipts) == 1
            assert receipts[0].receipt.disposition == "rejected"
            assert receipts[0].receipt.correlation_id == "stop.before.snapshot"
            assert receipts[0].receipt.current_identity is None
            if not busy:
                assert "correlation_id=stop.before.snapshot" in caplog.text
                assert "command=program_stop" in caplog.text
                assert "Stop dispatch unavailable" in caplog.text
            assert not session._closed  # noqa: SLF001
            assert session._next_heartbeat(1) is not None  # noqa: SLF001
        finally:
            session._closed = True  # noqa: SLF001
            ingress.close()

    asyncio.run(exercise())


def test_cold_freshness_uses_one_deadline_and_cancels_it_without_clients(
    tmp_path,
) -> None:
    source_path = tmp_path / "manager-source.json"
    source_path.write_text(
        source_snapshot_text(
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
        await source._refresh_file_watches(notify=False)  # noqa: SLF001
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
            source._subscriptions.add(
                ControlStateSubscription(asyncio.Event())
            )  # noqa: SLF001
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


def test_client_incident_episode_survives_reconnect_without_duplicate_activation() -> (
    None
):
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
        source._cycle = _running_cycle(1)  # noqa: SLF001
        source._program_source_current = True  # noqa: SLF001
        source._facts = SimpleNamespace(
            runtime={
                "sections": {
                    "logical_scene": {  # noqa: SLF001
                        "source_owner_epoch": "scene.owner.at.incident",
                        "freshness": "fresh",
                        "payload": {
                            "access": {
                                "active_projection": {
                                    "scene_revision": {
                                        "scene_id": "scene.at.incident",
                                        "revision_id": 4,
                                        "content_hash": "sha256:" + "b" * 64,
                                    }
                                }
                            }
                        },
                    }
                }
            }
        )
        source.record_incident(incident_id="incident.1", phase="activated")
        assert recorded == []
        source._cycle = _cycle()  # noqa: SLF001
        source._facts = None  # noqa: SLF001
        await asyncio.sleep(0)
        assert len(recorded) == 1
        context = recorded[0].pop("causal_context")
        assert context["program_run_id"] == "run.1"
        assert context["program_source_current"] == "true"
        assert context["scene_id"] == "scene.at.incident"
        assert context["scene_revision_id"] == "4"
        assert context["scene_source_owner_epoch"] == "scene.owner.at.incident"
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

    assert (
        len(source._active_client_incidents) == CONTROL_MAXIMUM_CLIENTS * 8
    )  # noqa: SLF001
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


def test_group_stop_and_resume_are_critical_without_a_cycle_revision_change() -> None:
    def cycle(state: str, **changes: Any) -> dict[str, Any]:
        return {
            "execution_control_revision": 3,
            "node_status_revision": 1,
            "runtime_controls": {
                "execution_selections": {
                    "groups": [
                        {
                            "group_id": "arm",
                            "state": state,
                            "stop_allowed": True,
                            "resume_allowed": state == "held",
                            **changes,
                        }
                    ]
                }
            },
        }

    for before, after in (
        ("active", "stopping"),
        ("stopping", "held"),
        ("held", "active"),
    ):
        assert _program_event_class(cycle(before), cycle(after)) == "critical"
    assert (
        _program_event_class(
            cycle("active"), {**cycle("active"), "node_status_revision": 2}
        )
        == "node_progress"
    )
    assert (
        _program_event_class(cycle("held"), cycle("held", held_node_count=2))
        == "node_progress"
    )
    assert _program_event_class(
        cycle("mixed", ordinary_held_node_count=0),
        cycle("mixed", ordinary_held_node_count=1),
    ) == "critical"
    assert (
        _program_event_class(cycle("held"), cycle("held", resume_allowed=False))
        == "critical"
    )
    assert _program_event_class(None, cycle("active")) == "critical"
    assert (
        _program_event_class(
            cycle("active"), {**cycle("active"), "execution_control_revision": 4}
        )
        == "critical"
    )


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
                _running_cycle(node_revision),
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
            _running_cycle(2),
            "node_progress",
        )
        first = source.take(current)
        assert first is not None
        assert first.shell["program_cycle"]["node_status_revision"] == 2

        source._accept_cycle(  # noqa: SLF001
            _running_cycle(3),
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
        assert (
            frame.shell["component_overview"]["basis"]["program_cycle_generation"] == 3
        )
        assert cold_wakes == [None]
        source._subscriptions.clear()  # noqa: SLF001
        await source.close()

    try:
        asyncio.run(exercise())
    finally:
        ingress.close()


def _control_with_pose(positions: list[float]) -> tuple[dict[str, Any], dict[str, Any]]:
    identity = {"owner_epoch": "execution.1"}
    row = {
        "controllable_component_id": "ur5e_robot",
        "command_target_id": "arm",
        "observed": {"kind": "joint_positions", "positions_rad": positions},
    }
    return (
        {"ok": True, "identity": identity, "targets": []},
        {"ok": True, "identity": identity, "control_state": {"observed_targets": [row]}},
    )


def test_active_tcp_is_a_meaningful_control_change_but_joint_samples_are_not() -> None:
    _targets, before = _control_with_pose([0.0])
    _targets, after = _control_with_pose([0.4])
    first = {"entry": None, "display": None, "owner_epoch": "execution.1",
             "registration_generation": 1, "sequence": 0}
    before["control_state"]["observed_targets"][0]["active_tcp"] = first
    after["control_state"]["observed_targets"][0]["active_tcp"] = dict(first)
    key = control_state_module._meaningful_execution_control_key  # noqa: SLF001
    assert key(before) == key(after)
    after["control_state"]["observed_targets"][0]["active_tcp"]["sequence"] = 1
    assert key(before) != key(after)


def test_settled_motion_publishes_rest_pose_that_samples_alone_do_not() -> None:
    async def exercise() -> None:
        pose = [-0.25, -1.43, 1.0]

        def facts(*, program_cycle, cancelled=None):
            targets, control = _control_with_pose(list(pose))
            return replace(
                _facts_for_cycle(program_cycle),
                execution_targets_response=targets,
                execution_control_response=control,
            )

        ingress = _ingress([])
        source = HeadlessControlStateSource(
            SimpleNamespace(program_cycle_status=_cycle, component_overview_facts=facts),
            ingress,
            program_runner_websocket_url=None,
        )
        subscription = await source.subscribe()
        try:
            await asyncio.wait_for(subscription.wake.wait(), 1)
            previous = source.take(subscription)

            async def reduced(count: int) -> None:
                while source.diagnostics()["reductions"] < count:
                    await asyncio.sleep(0.005)

            # A moving robot's samples alone never publish.
            pose[:] = [-0.35, -1.39, 1.1]
            reductions = source.diagnostics()["reductions"]
            source.notify_cold()
            await asyncio.wait_for(reduced(reductions + 1), 1)
            assert not subscription.wake.is_set()

            # Once a motion settles, the rest pose is published and delivered.
            pose[:] = [-0.45, -1.35, 1.2]
            source.notify_motion_settled()
            await asyncio.wait_for(subscription.wake.wait(), 1)
            current = source.take(subscription)
            rows = current.shell["execution_control_state"]["control_state"]["observed_targets"]
            assert rows[0]["observed"]["positions_rad"] == [-0.45, -1.35, 1.2]
            session = ExecutionControlSession(object(), source, ingress)  # type: ignore[arg-type]
            delta = session._delta_message(current, previous)  # noqa: SLF001
            assert delta.shell["execution_control_state"] == current.shell["execution_control_state"]

            # A second settle with an unchanged pose publishes nothing new.
            reductions = source.diagnostics()["reductions"]
            source.notify_motion_settled()
            await asyncio.wait_for(reduced(reductions + 1), 1)
            assert not subscription.wake.is_set()
        finally:
            await source.unsubscribe(subscription)
            ingress.close()

    asyncio.run(exercise())


def test_cold_reduction_refreshes_only_reset_robot_position_from_direct_read() -> None:
    async def exercise() -> None:
        mode = "initial"
        admission = {"admission_generation": 29, "allowed": False}
        available = {"available": True, "reason": None}

        def read() -> dict[str, Any]:
            if mode == "fail":
                raise HeadlessApiError("program_runner_unavailable", "down")
            cycle = _cycle()
            if mode == "foreign":
                cycle["reset_robot_position"] = available
                cycle["execution_control_revision"] = 5
            elif mode == "same":
                cycle["reset_robot_position"] = available
                cycle["causal_correlation_id"] = "corr.direct"  # never taken
            return cycle

        def facts(*, program_cycle, cancelled=None):
            return replace(
                _facts_for_cycle(program_cycle), authoring_admission=dict(admission)
            )

        ingress = _ingress([])
        source = HeadlessControlStateSource(
            SimpleNamespace(program_cycle_status=read, component_overview_facts=facts),
            ingress,
            program_runner_websocket_url=None,
        )
        subscription = await source.subscribe()

        async def cold() -> ControlStateFrame:
            source.notify_cold()
            await asyncio.wait_for(subscription.wake.wait(), 1)
            frame = source.take(subscription)
            await asyncio.sleep(0.2)  # let any follow-up reduction settle
            if subscription.wake.is_set():
                frame = source.take(subscription)
            assert frame.event_class == "cold"
            return frame

        try:
            await asyncio.wait_for(subscription.wake.wait(), 1)
            cached = source.take(subscription).shell["program_cycle"]
            assert cached["reset_robot_position"]["available"] is False

            # A read of another control revision is foreign: nothing is taken.
            mode = "foreign"
            admission = {"admission_generation": 30, "allowed": True}
            frame = await cold()
            assert frame.shell["authoring_admission"] == admission
            assert frame.shell["program_cycle"] == cached

            # Same identity: only the host fact is refreshed.
            mode = "same"
            cycle = (await cold()).shell["program_cycle"]
            assert cycle == {**cached, "reset_robot_position": available}

            # A failed direct read keeps the cached value; cold still publishes.
            mode = "fail"
            admission = {"admission_generation": 31, "allowed": False}
            frame = await cold()
            assert frame.shell["authoring_admission"] == admission
            assert frame.shell["program_cycle"] == cycle
        finally:
            await source.unsubscribe(subscription)
            ingress.close()

    asyncio.run(exercise())


def test_cold_reduction_drops_direct_reset_read_overtaken_by_the_stream() -> None:
    async def exercise() -> None:
        mode = "initial"
        loop = asyncio.get_running_loop()
        entered = asyncio.Event()
        release = threading.Event()

        def read() -> dict[str, Any]:
            nonlocal mode
            if mode == "fail":
                raise HeadlessApiError("program_runner_unavailable", "down")
            cycle = _cycle()
            if mode == "late":
                mode = "fail"
                loop.call_soon_threadsafe(entered.set)
                assert release.wait(1)
                cycle["reset_robot_position"] = {"available": True, "reason": None}
            return cycle

        ingress = _ingress([])
        source = HeadlessControlStateSource(
            SimpleNamespace(
                program_cycle_status=read,
                component_overview_facts=lambda *, program_cycle, cancelled=None: _facts_for_cycle(
                    program_cycle
                ),
            ),
            ingress,
            program_runner_websocket_url=None,
        )
        subscription = await source.subscribe()
        try:
            await asyncio.wait_for(subscription.wake.wait(), 1)
            source.take(subscription)
            mode = "late"
            source.notify_cold()
            await asyncio.wait_for(entered.wait(), 1)
            # The stream delivers a newer cycle (same control key) mid-read.
            newer = {**_cycle(), "causal_correlation_id": "corr.newer"}
            source._accept_cycle(newer, "critical")  # noqa: SLF001
            release.set()
            reductions = source.diagnostics()["reductions"]
            while source.diagnostics()["reductions"] <= reductions:
                await asyncio.sleep(0.005)
            await asyncio.sleep(0.1)
            assert source._cycle == newer  # noqa: SLF001
            assert source.take(subscription).shell["program_cycle"] == newer
        finally:
            release.set()
            await source.unsubscribe(subscription)
            ingress.close()

    asyncio.run(exercise())


def test_cold_reduction_does_not_revive_an_invalidated_program_source() -> None:
    async def exercise() -> None:
        mode = "initial"
        loop = asyncio.get_running_loop()
        direct_read = asyncio.Event()

        def read() -> dict[str, Any]:
            cycle = _cycle()
            if mode == "same":
                loop.call_soon_threadsafe(direct_read.set)
                cycle["reset_robot_position"] = {"available": True, "reason": None}
            return cycle

        ingress = _ingress([])
        source = HeadlessControlStateSource(
            SimpleNamespace(
                program_cycle_status=read,
                component_overview_facts=lambda *, program_cycle, cancelled=None: _facts_for_cycle(
                    program_cycle
                ),
            ),
            ingress,
            program_runner_websocket_url=None,
        )
        subscription = await source.subscribe()
        try:
            await asyncio.wait_for(subscription.wake.wait(), 1)
            source.take(subscription)
            source._invalidate_source("program")  # noqa: SLF001  stream failed
            mode = "same"
            source.notify_cold()
            await asyncio.wait_for(direct_read.wait(), 1)
            await asyncio.sleep(0.2)
            assert source._program_source_current is False  # noqa: SLF001
            assert source._cycle == _cycle()  # noqa: SLF001
            assert source.semantic_context().status != "current"
            assert not subscription.wake.is_set()
            assert source.take(subscription) is None
        finally:
            await source.unsubscribe(subscription)
            ingress.close()

    asyncio.run(exercise())


def test_control_delta_carries_changed_rest_pose_without_meaningful_change() -> None:
    ingress = _ingress([])
    cycle = _cycle()
    shell = {**_shell_for_cycle(cycle), "program_cycle": cycle}
    _targets, before = _control_with_pose([-0.25, -1.43, 1.0])
    _targets, after = _control_with_pose([-0.45, -1.35, 1.2])
    previous = ControlStateFrame(
        generation=1,
        event_class="cold",
        control=build_control_projection(cycle, ingress),
        shell={**shell, "execution_control_state": before},
    )
    current = ControlStateFrame(
        generation=2,
        event_class="cold",
        control=build_control_projection(cycle, ingress),
        shell={**shell, "execution_control_state": after},
    )
    session = ExecutionControlSession(  # type: ignore[arg-type]
        object(), _Source(previous), ingress
    )
    try:
        message = session._delta_message(current, previous)  # noqa: SLF001
        unchanged = session._delta_message(previous, previous)  # noqa: SLF001
    finally:
        ingress.close()

    assert message.shell == {"complete": False, "execution_control_state": after}
    assert unchanged.shell == {"complete": False}
