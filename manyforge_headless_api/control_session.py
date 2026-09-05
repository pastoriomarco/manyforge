"""Versioned bidirectional UI6-G execution-control WebSocket session."""

from __future__ import annotations

import asyncio
from collections import deque
from contextlib import suppress
import json
import logging
import time
from typing import Any, Callable
import uuid

from fastapi import WebSocket, WebSocketDisconnect
from pydantic import ValidationError

from .control_protocol import (
    CONTROL_HEARTBEAT_DELAYED_NS,
    CONTROL_HEARTBEAT_PERIOD_NS,
    CONTROL_HEARTBEAT_STALE_NS,
    CONTROL_MAXIMUM_MESSAGE_BYTES,
    CONTROL_MAXIMUM_MESSAGES_PER_SECOND,
    CONTROL_MAXIMUM_SNAPSHOT_BYTES,
    CONTROL_PROTOCOL_VERSION,
    MAXIMUM_U64,
    ControlClientHelloDTO,
    ControlClientIncidentDTO,
    ControlHeartbeatChallengeDTO,
    ControlHeartbeatEchoDTO,
    ControlResyncRequestDTO,
    ControlResyncRequiredDTO,
    ControlServerHelloDTO,
    ControlStateAppliedDTO,
    ExecutionCommandReceiptMessageDTO,
    ExecutionControlCommandDTO,
    ExecutionControlDeltaDTO,
    ExecutionControlSnapshotDTO,
    is_stop_command,
)
from .control_state import (
    ControlStateFrame,
    ControlStateSubscription,
    HeadlessControlStateSource,
    control_shell_fact_key,
)
from .execution_control import (
    ExecutionControlBusyError,
    ExecutionControlIngress,
    ExecutionControlSubmission,
)


_SEND_TIMEOUT_SECONDS = 0.25
_CLIENT_HELLO_TIMEOUT_SECONDS = 1.0
_CONTROL_DEAD_PEER_TIMEOUT_NS = 30_000_000_000
_MAXIMUM_RECEIPTS = 8
_MAXIMUM_CLIENT_INCIDENTS_PER_SECOND = 8
_LOGGER = logging.getLogger(__name__)


def _encode(value: Any, *, maximum_bytes: int) -> str:
    payload = json.dumps(
        value.model_dump(mode="json") if hasattr(value, "model_dump") else value,
        allow_nan=False,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    )
    if len(payload.encode("utf-8")) > maximum_bytes:
        raise ValueError("control message exceeds its byte bound")
    return payload


class _OutboundSlots:
    """Priority receipts plus capacity-one state and heartbeat slots."""

    def __init__(self) -> None:
        self.wake = asyncio.Event()
        self.receipts: deque[Any] = deque()
        self.state: Any | None = None
        self.heartbeat: Any | None = None
        self.closed = False

    def put_receipt(self, payload: Any) -> None:
        if len(self.receipts) >= _MAXIMUM_RECEIPTS:
            raise RuntimeError("control receipt delivery is saturated")
        self.receipts.append(payload)
        self.wake.set()

    def put_state(self, payload: Any) -> bool:
        replaced = self.state is not None
        self.state = payload
        self.wake.set()
        return replaced

    def put_heartbeat(self, payload: Any) -> None:
        self.heartbeat = payload
        self.wake.set()

    async def take(
        self,
        *,
        state_allowed: Callable[[], bool] = lambda: True,
    ) -> Any | None:
        while not self.closed:
            if self.receipts:
                return self.receipts.popleft()
            if self.heartbeat is not None:
                payload, self.heartbeat = self.heartbeat, None
                return payload
            if state_allowed() and self.state is not None:
                payload, self.state = self.state, None
                return payload
            self.wake.clear()
            await self.wake.wait()
        return None

    def close(self) -> None:
        self.closed = True
        self.wake.set()


class ExecutionControlSession:
    """Per-client transport state; execution meaning remains in owners."""

    def __init__(
        self,
        websocket: WebSocket,
        source: HeadlessControlStateSource,
        ingress: ExecutionControlIngress,
        *,
        shell_enricher: Callable[[dict[str, Any]], dict[str, Any]] | None = None,
        clock_ns: Callable[[], int] = time.monotonic_ns,
    ) -> None:
        self._websocket = websocket
        self._source = source
        self._ingress = ingress
        self._enrich = shell_enricher or (lambda value: value)
        self._clock_ns = clock_ns
        self._session_id = f"control-session.{uuid.uuid4().hex}"
        self._server_sequence = 0
        self._state_sequence = 0
        self._state_inflight: int | None = None
        self._challenge_sequence = 0
        self._last_echoed_challenge = 0
        self._challenge_issued_ns: int | None = None
        self._delay_level = 0
        self._last_client_sequence = 0
        self._last_heartbeat_ns = clock_ns()
        self._rate_window_ns = self._last_heartbeat_ns
        self._rate_count = 0
        self._ordinary_pending: asyncio.Future[ExecutionControlSubmission] | None = None
        self._runtime_scale_pending: (
            asyncio.Future[ExecutionControlSubmission] | None
        ) = None
        self._slots = _OutboundSlots()
        self._subscription: ControlStateSubscription | None = None
        self._latest_frame: ControlStateFrame | None = None
        self._snapshot_sent = False
        self._resync_required = False
        self._client_incarnation = "unknown-client"
        self._source_label = "unknown-client"
        self._server_incidents: dict[str, int] = {}
        self._incident_rate_window_ns = self._last_heartbeat_ns
        self._incident_rate_count = 0
        self._ingress_session_open = False
        self._closed = False

    def _next_server_sequence(self) -> str:
        if self._server_sequence >= MAXIMUM_U64:
            raise ValueError("control server sequence requires session renewal")
        self._server_sequence += 1
        return str(self._server_sequence)

    def _admit_rate(self) -> None:
        now = self._clock_ns()
        if now - self._rate_window_ns >= 1_000_000_000:
            self._rate_window_ns = now
            self._rate_count = 0
        self._rate_count += 1
        if self._rate_count > CONTROL_MAXIMUM_MESSAGES_PER_SECOND:
            raise ValueError("control client message rate exceeded")

    def _admit_client_sequence(self, value: str) -> int:
        sequence = int(value)
        if sequence != self._last_client_sequence + 1:
            raise ValueError("control client sequence is not contiguous")
        self._last_client_sequence = sequence
        return sequence

    def _admit_client_incident(self) -> None:
        now = self._clock_ns()
        if now - self._incident_rate_window_ns >= 1_000_000_000:
            self._incident_rate_window_ns = now
            self._incident_rate_count = 0
        self._incident_rate_count += 1
        if self._incident_rate_count > _MAXIMUM_CLIENT_INCIDENTS_PER_SECOND:
            raise ValueError("control client incident rate exceeded")

    def _shell(self, frame: ControlStateFrame) -> dict[str, Any]:
        return self._enrich(dict(frame.shell))

    def _snapshot_message(self, frame: ControlStateFrame) -> ExecutionControlSnapshotDTO:
        return ExecutionControlSnapshotDTO(
            schema_version=CONTROL_PROTOCOL_VERSION,
            message_kind="snapshot",
            control_session_id=self._session_id,
            server_sequence="1",
            state_sequence="1",
            received_client_sequence=(
                None
                if self._last_client_sequence == 0
                else str(self._last_client_sequence)
            ),
            control=frame.control,
            shell=self._shell(frame),
            complete=True,
        )

    def _delta_message(
        self,
        frame: ControlStateFrame,
        previous: ControlStateFrame | None,
    ) -> ExecutionControlDeltaDTO:
        """Carry only changed owner facts after the complete snapshot.

        A control snapshot is deliberately complete, but retransmitting its
        unchanged runtime, target, and runtime-control structures at every
        sparse lifecycle boundary can put hundreds of kilobytes ahead of a
        later command receipt on the ordered socket.  Top-level owner facts
        are immutable values, so replacement of only the changed facts is a
        complete and considerably cheaper delta contract.
        """

        current = self._shell(frame)
        prior = {} if previous is None else previous.shell
        shell: dict[str, Any] = {"complete": False}
        for name, value in current.items():
            if (
                name not in prior
                or control_shell_fact_key(current, name)
                != control_shell_fact_key(prior, name)
            ):
                shell[name] = value
        # Targets are immutable within their validated execution identity. A
        # target replacement still carries its matching control capture; an
        # ordinary control-state change can reuse the already-validated target.
        if "execution_targets" in shell:
            shell["execution_control_state"] = current[
                "execution_control_state"
            ]
        return ExecutionControlDeltaDTO(
            schema_version=CONTROL_PROTOCOL_VERSION,
            message_kind="delta",
            control_session_id=self._session_id,
            server_sequence="1",
            state_sequence="1",
            received_client_sequence=(
                None
                if self._last_client_sequence == 0
                else str(self._last_client_sequence)
            ),
            event_class=frame.event_class,  # type: ignore[arg-type]
            control=frame.control,
            shell=shell,
        )

    async def _send_direct(self, payload: str, kind: str) -> None:
        await asyncio.wait_for(
            self._websocket.send_text(payload),
            timeout=_SEND_TIMEOUT_SECONDS,
        )
        self._source.record_message(kind, len(payload.encode("utf-8")))

    async def _send_message(self, message: Any) -> None:
        """Assign transport order only when an item actually leaves its slot."""

        if isinstance(message, str):
            await self._send_direct(message, "unknown")
            return
        fields = getattr(type(message), "model_fields", {})
        if "server_sequence" in fields:
            updates = {"server_sequence": self._next_server_sequence()}
            if getattr(message, "message_kind", None) in {"snapshot", "delta"}:
                if self._state_inflight is not None:
                    raise RuntimeError("control state credit is already in flight")
                if self._state_sequence >= MAXIMUM_U64:
                    raise ValueError("control state sequence requires renewal")
                self._state_sequence += 1
                self._state_inflight = self._state_sequence
                updates["state_sequence"] = str(self._state_sequence)
            if getattr(message, "message_kind", None) == "command_receipt":
                # This is the emission boundary, not the earlier completion
                # callback that merely placed the receipt in its priority slot.
                updates["emitted_monotonic_ns"] = str(self._clock_ns())
            message = message.model_copy(update=updates)
        kind = getattr(message, "message_kind", None)
        maximum = (
            CONTROL_MAXIMUM_SNAPSHOT_BYTES
            if kind in {"snapshot", "delta", "command_receipt"}
            else CONTROL_MAXIMUM_MESSAGE_BYTES
        )
        await self._send_direct(_encode(message, maximum_bytes=maximum), str(kind))
        if kind == "snapshot":
            self._snapshot_sent = True

    def _put_state(self, payload: Any) -> None:
        pending = self._slots.state
        if (
            isinstance(pending, ExecutionControlDeltaDTO)
            and isinstance(payload, ExecutionControlDeltaDTO)
        ):
            # Each sparse delta is reduced against the preceding owner frame,
            # but a slow client may never have received that frame. Preserve
            # the union of changed top-level facts in the one pending slot;
            # newer values still replace older values for the same fact.
            payload = payload.model_copy(
                update={"shell": {**pending.shell, **payload.shell}}
            )
        elif (
            isinstance(pending, ExecutionControlSnapshotDTO)
            and isinstance(payload, ExecutionControlDeltaDTO)
        ):
            shell = {**pending.shell, **payload.shell}
            shell.pop("complete", None)
            payload = pending.model_copy(
                update={
                    "control": payload.control or pending.control,
                    "received_client_sequence": payload.received_client_sequence,
                    "shell": shell,
                }
            )
        if self._slots.put_state(payload):
            self._source.record_state_replacement()

    async def _handshake(self) -> None:
        raw = await asyncio.wait_for(
            self._websocket.receive_text(),
            timeout=_CLIENT_HELLO_TIMEOUT_SECONDS,
        )
        if len(raw.encode("utf-8")) > CONTROL_MAXIMUM_MESSAGE_BYTES:
            raise ValueError("control hello is oversized")
        hello = ControlClientHelloDTO.model_validate_json(raw, strict=True)
        if tuple(hello.supported_versions) != (CONTROL_PROTOCOL_VERSION,):
            raise ValueError("control protocol version is unsupported")
        self._client_incarnation = hello.client_incarnation
        self._source_label = hello.source_label
        subscription = await self._source.subscribe()
        self._subscription = subscription
        # Confirm transport immediately. Heartbeat and semantic synchronization
        # start as independent tasks after this response.
        hello_response = ControlServerHelloDTO(
            schema_version=CONTROL_PROTOCOL_VERSION,
            message_kind="server_hello",
            receiver_incarnation=self._ingress.receiver_incarnation,
            control_session_id=self._session_id,
            heartbeat_period_ns=CONTROL_HEARTBEAT_PERIOD_NS,
            heartbeat_delayed_ns=CONTROL_HEARTBEAT_DELAYED_NS,
            heartbeat_stale_ns=CONTROL_HEARTBEAT_STALE_NS,
            maximum_message_bytes=CONTROL_MAXIMUM_MESSAGE_BYTES,
            maximum_messages_per_second=CONTROL_MAXIMUM_MESSAGES_PER_SECOND,
        )
        await self._send_direct(
            _encode(hello_response, maximum_bytes=CONTROL_MAXIMUM_MESSAGE_BYTES),
            "server_hello",
        )
        self._last_heartbeat_ns = self._clock_ns()

    async def _writer(self) -> None:
        while not self._closed:
            payload = await self._slots.take(
                state_allowed=lambda: self._state_inflight is None
            )
            if payload is None:
                return
            await self._send_message(payload)

    async def _state_sender(self) -> None:
        subscription = self._subscription
        assert subscription is not None
        while not subscription.closed and not self._closed:
            await subscription.wake.wait()
            frame = self._source.take(subscription)
            if frame is None:
                continue
            previous = self._latest_frame
            self._latest_frame = frame
            if not self._snapshot_sent:
                self._put_state(self._snapshot_message(frame))
                continue
            if previous is not None and (
                previous.control.receiver_incarnation
                != frame.control.receiver_incarnation
                or previous.control.semantic_owner_epoch
                != frame.control.semantic_owner_epoch
            ):
                self._resync_required = True
                self._put_state(
                    ControlResyncRequiredDTO(
                        schema_version=CONTROL_PROTOCOL_VERSION,
                        message_kind="resync_required",
                        control_session_id=self._session_id,
                        server_sequence="1",
                        reason_code="control_owner_replaced",
                    )
                )
                continue
            if self._resync_required:
                continue
            self._put_state(self._delta_message(frame, previous))

    def _record_incident(
        self,
        *,
        incident_id: str,
        incident_kind: str,
        phase: str,
        first_observed_ns: int,
        last_observed_ns: int,
        occurrence_count: int,
        maximum_delay_ns: int | None,
        reason_code: str,
    ) -> None:
        self._source.record_incident(
            incident_id=incident_id,
            incident_kind=incident_kind,
            phase=phase,
            control_session_id=self._session_id,
            client_incarnation=self._client_incarnation,
            source_label=self._source_label,
            reason_code=reason_code,
            occurrence_count=occurrence_count,
            maximum_delay_ns=maximum_delay_ns,
            first_observed_ns=first_observed_ns,
            last_observed_ns=last_observed_ns,
        )

    def _classify_outstanding_heartbeat(self, now_ns: int) -> None:
        issued_ns = self._challenge_issued_ns
        if issued_ns is None:
            return
        elapsed = now_ns - issued_ns
        for level, threshold, kind, reason in (
            (1, CONTROL_HEARTBEAT_DELAYED_NS, "control_delayed", "heartbeat_delayed"),
            (2, CONTROL_HEARTBEAT_STALE_NS, "control_stale", "heartbeat_stale"),
        ):
            if elapsed < threshold or self._delay_level >= level:
                continue
            self._delay_level = level
            self._source.record_delay_transition(stale=level == 2)
            first = self._source.relative_monotonic_ns(now_ns)
            self._server_incidents[kind] = first
            self._record_incident(
                incident_id=f"{self._session_id}.{kind}.{self._challenge_sequence}",
                incident_kind=kind,
                phase="activated",
                first_observed_ns=first,
                last_observed_ns=first,
                occurrence_count=1,
                maximum_delay_ns=elapsed,
                reason_code=reason,
            )

    def _resolve_heartbeat_incidents(
        self,
        now_ns: int,
        elapsed_ns: int,
        *,
        reason_code: str = "heartbeat_recovered",
    ) -> None:
        count = max(1, elapsed_ns // CONTROL_HEARTBEAT_PERIOD_NS)
        last = self._source.relative_monotonic_ns(now_ns)
        for kind in ("control_delayed", "control_stale"):
            first = self._server_incidents.pop(kind, None)
            if first is None:
                continue
            self._record_incident(
                incident_id=f"{self._session_id}.{kind}.{self._challenge_sequence}",
                incident_kind=kind,
                phase="resolved",
                first_observed_ns=first,
                last_observed_ns=last,
                occurrence_count=count,
                maximum_delay_ns=elapsed_ns,
                reason_code=reason_code,
            )

    def _next_heartbeat(
        self,
        now_ns: int,
    ) -> ControlHeartbeatChallengeDTO | None:
        # One unanswered challenge is enough to prove liveness. Keeping it
        # outstanding prevents heartbeat queues and catch-up bursts when a
        # browser tab or remote browser main thread is temporarily paused.
        # The outbound slot gives this tiny challenge priority over replaceable
        # state so sustained node progress cannot starve session liveness.
        if self._challenge_sequence > self._last_echoed_challenge:
            self._classify_outstanding_heartbeat(now_ns)
            if now_ns - self._last_heartbeat_ns > _CONTROL_DEAD_PEER_TIMEOUT_NS:
                raise TimeoutError("control peer stopped answering heartbeats")
            return None
        semantic = self._source.semantic_context()
        freshness = None
        if semantic.status == "current":
            assert semantic.semantic_owner_epoch is not None
            assert semantic.execution_control_revision is not None
            freshness = self._ingress.freshness_for(
                semantic.semantic_owner_epoch,
                semantic.execution_control_revision,
            )
        self._challenge_sequence += 1
        self._challenge_issued_ns = now_ns
        return ControlHeartbeatChallengeDTO(
            schema_version=CONTROL_PROTOCOL_VERSION,
            message_kind="heartbeat_challenge",
            control_session_id=self._session_id,
            server_sequence="1",
            challenge_sequence=str(self._challenge_sequence),
            received_client_sequence=(
                None
                if self._last_client_sequence == 0
                else str(self._last_client_sequence)
            ),
            semantic_status=semantic.status,  # type: ignore[arg-type]
            semantic_reason=semantic.reason,
            freshness=freshness,
        )

    async def _heartbeat(self) -> None:
        period = CONTROL_HEARTBEAT_PERIOD_NS / 1_000_000_000
        while not self._closed:
            challenge = self._next_heartbeat(self._clock_ns())
            if challenge is not None:
                self._slots.put_heartbeat(challenge)
            await asyncio.sleep(period)

    def _receipt_done(
        self,
        request: ExecutionControlCommandDTO,
        future: asyncio.Future[ExecutionControlSubmission],
    ) -> None:
        if self._ordinary_pending is future:
            self._ordinary_pending = None
        if self._runtime_scale_pending is future:
            self._runtime_scale_pending = None
        if self._closed:
            return
        kind = request.command.command_envelope.command_kind
        try:
            submission = future.result()
            receipt = submission.receipt
            # The owner state is the execution truth and streams independently.
            # Only runtime scale currently needs one compact synchronous result;
            # shipping whole Program/operation snapshots in every receipt makes
            # browsers parse data they immediately discard.
            result = (
                dict(submission.result)
                if kind == "runtime_scale" and submission.result is not None
                else None
            )
        except Exception as error:
            frame = self._latest_frame or self._source.current()
            receipt = self._ingress.rejection_receipt(
                request.command,
                code=getattr(error, "code", "execution_control_delivery_uncertain"),
                semantic_owner_epoch=frame.control.semantic_owner_epoch,
                execution_control_revision=frame.control.execution_control_revision,
            )
            result = None
        message = ExecutionCommandReceiptMessageDTO(
            schema_version=CONTROL_PROTOCOL_VERSION,
            message_kind="command_receipt",
            control_session_id=self._session_id,
            server_sequence="1",
            received_client_sequence=request.client_sequence,
            emitted_monotonic_ns=str(self._clock_ns()),
            receipt=receipt,
            result=result,
        )
        try:
            self._slots.put_receipt(message)
        except (RuntimeError, ValueError):
            self._closed = True
            self._slots.close()
        self._source.notify_control()

    def _receive_command(self, message: ExecutionControlCommandDTO) -> None:
        kind = message.command.command_envelope.command_kind
        runtime_scale = kind == "runtime_scale"
        if not is_stop_command(kind):
            pending = (
                self._runtime_scale_pending
                if runtime_scale
                else self._ordinary_pending
            )
            if pending is not None and not pending.done():
                frame = self._latest_frame or self._source.current()
                receipt = self._ingress.rejection_receipt(
                    message.command,
                    code=(
                        "control_session_runtime_scale_pending"
                        if runtime_scale
                        else "control_session_ordinary_command_pending"
                    ),
                    semantic_owner_epoch=frame.control.semantic_owner_epoch,
                    execution_control_revision=frame.control.execution_control_revision,
                )
                self._slots.put_receipt(
                    ExecutionCommandReceiptMessageDTO(
                        schema_version=CONTROL_PROTOCOL_VERSION,
                        message_kind="command_receipt",
                        control_session_id=self._session_id,
                        server_sequence="1",
                        received_client_sequence=message.client_sequence,
                        emitted_monotonic_ns=str(self._clock_ns()),
                        receipt=receipt,
                        result=None,
                    )
                )
                return
        try:
            future = self._ingress.start(
                message.command,
                transport_kind="websocket",
            )
        except ExecutionControlBusyError:
            frame = self._latest_frame or self._source.current()
            receipt = self._ingress.rejection_receipt(
                message.command,
                code="execution_control_ingress_busy",
                semantic_owner_epoch=frame.control.semantic_owner_epoch,
                execution_control_revision=frame.control.execution_control_revision,
            )
            self._slots.put_receipt(
                ExecutionCommandReceiptMessageDTO(
                    schema_version=CONTROL_PROTOCOL_VERSION,
                    message_kind="command_receipt",
                    control_session_id=self._session_id,
                    server_sequence="1",
                    received_client_sequence=message.client_sequence,
                    emitted_monotonic_ns=str(self._clock_ns()),
                    receipt=receipt,
                    result=None,
                )
            )
            return
        if runtime_scale:
            self._runtime_scale_pending = future
        elif not is_stop_command(kind):
            self._ordinary_pending = future
        future.add_done_callback(lambda done: self._receipt_done(message, done))

    async def _receiver(self) -> None:
        while not self._closed:
            raw = await self._websocket.receive_text()
            if len(raw.encode("utf-8")) > CONTROL_MAXIMUM_MESSAGE_BYTES:
                raise ValueError("control client message is oversized")
            self._admit_rate()
            value = json.loads(raw)
            if not isinstance(value, dict):
                raise ValueError("control client message must be an object")
            kind = value.get("message_kind")
            if kind == "heartbeat_echo":
                message = ControlHeartbeatEchoDTO.model_validate(value)
                if message.control_session_id != self._session_id:
                    raise ValueError("control heartbeat names another session")
                self._admit_client_sequence(message.client_sequence)
                challenge = int(message.challenge_sequence)
                if (
                    challenge <= self._last_echoed_challenge
                    or challenge > self._challenge_sequence
                ):
                    raise ValueError("control heartbeat challenge is stale or unknown")
                self._last_echoed_challenge = challenge
                now_ns = self._clock_ns()
                issued_ns = self._challenge_issued_ns or now_ns
                round_trip_ns = max(0, now_ns - issued_ns)
                self._source.record_heartbeat_echo(round_trip_ns)
                self._resolve_heartbeat_incidents(now_ns, round_trip_ns)
                self._delay_level = 0
                self._challenge_issued_ns = None
                self._last_heartbeat_ns = now_ns
            elif kind == "state_applied":
                message = ControlStateAppliedDTO.model_validate(value)
                if message.control_session_id != self._session_id:
                    raise ValueError("control state credit names another session")
                applied = int(message.state_sequence)
                if self._state_inflight is None or applied != self._state_inflight:
                    raise ValueError("control state credit is stale or unknown")
                self._state_inflight = None
                self._slots.wake.set()
            elif kind == "command":
                message = ExecutionControlCommandDTO.model_validate(value)
                if message.control_session_id != self._session_id:
                    raise ValueError("control command names another session")
                self._admit_client_sequence(message.client_sequence)
                self._receive_command(message)
            elif kind == "client_incident":
                message = ControlClientIncidentDTO.model_validate(value)
                if message.control_session_id != self._session_id:
                    raise ValueError("client incident names another session")
                self._admit_client_sequence(message.client_sequence)
                self._admit_client_incident()
                self._source.record_client_incident_transition(
                    client_incarnation=self._client_incarnation,
                    incident_id=message.incident_id,
                    incident_kind=message.incident_kind,
                    phase=message.phase,
                    control_session_id=self._session_id,
                    source_label=self._source_label,
                    occurrence_count=message.occurrence_count,
                    maximum_delay_ns=message.maximum_delay_ns,
                    reason_code=message.reason_code,
                    now_ns=self._clock_ns(),
                )
            elif kind == "resync":
                message = ControlResyncRequestDTO.model_validate(value)
                if message.control_session_id != self._session_id:
                    raise ValueError("control resync names another session")
                self._admit_client_sequence(message.client_sequence)
                frame = self._source.current()
                self._latest_frame = frame
                self._resync_required = False
                self._put_state(self._snapshot_message(frame))
            else:
                raise ValueError("control client message kind is unsupported")

    async def serve(self) -> None:
        await self._websocket.accept()
        tasks: set[asyncio.Task[None]] = set()
        try:
            self._ingress.open_control_session()
            self._ingress_session_open = True
            await self._handshake()
            tasks = {
                asyncio.create_task(self._receiver()),
                asyncio.create_task(self._writer()),
                asyncio.create_task(self._state_sender()),
                asyncio.create_task(self._heartbeat()),
            }
            done, pending = await asyncio.wait(
                tasks,
                return_when=asyncio.FIRST_EXCEPTION,
            )
            for task in done:
                task.result()
        except WebSocketDisconnect:
            pass
        except (
            asyncio.TimeoutError,
            ConnectionError,
            RuntimeError,
            TimeoutError,
            ValidationError,
            ValueError,
        ) as error:
            _LOGGER.warning(
                "control session %s closed fail-closed: %s: %s",
                self._session_id,
                type(error).__name__,
                error,
            )
        finally:
            self._closed = True
            self._slots.close()
            if self._server_incidents:
                now_ns = self._clock_ns()
                issued_ns = self._challenge_issued_ns or now_ns
                self._resolve_heartbeat_incidents(
                    now_ns,
                    max(0, now_ns - issued_ns),
                    reason_code="control_session_closed",
                )
            for task in tasks:
                if not task.done():
                    task.cancel()
            for task in tasks:
                with suppress(
                    asyncio.CancelledError,
                    ConnectionError,
                    RuntimeError,
                    TimeoutError,
                    ValidationError,
                    ValueError,
                    WebSocketDisconnect,
                ):
                    await task
            if self._subscription is not None:
                await self._source.unsubscribe(self._subscription)
                self._subscription = None
            if self._ingress_session_open:
                self._ingress.close_control_session()
                self._ingress_session_open = False
            with suppress(RuntimeError, WebSocketDisconnect):
                await self._websocket.close(code=1000)


async def serve_execution_control(
    websocket: WebSocket,
    source: HeadlessControlStateSource,
    ingress: ExecutionControlIngress,
    *,
    shell_enricher: Callable[[dict[str, Any]], dict[str, Any]] | None = None,
) -> None:
    await ExecutionControlSession(
        websocket,
        source,
        ingress,
        shell_enricher=shell_enricher,
    ).serve()


__all__ = ["ExecutionControlSession", "serve_execution_control"]
