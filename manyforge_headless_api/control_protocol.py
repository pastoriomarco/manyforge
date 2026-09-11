"""Strict UI6-G execution-control protocol and freshness authority.

The DTOs in this module are ROS-free and transport-neutral.  HTTP, WebSocket,
and in-process adapters decode into the same request and call the same ingress;
none of them owns Program or robot lifecycle semantics.
"""

from __future__ import annotations

from dataclasses import dataclass
import base64
import hashlib
import hmac
import json
import secrets
from threading import Lock
import time
from typing import Annotated, Any, Callable, Literal, Mapping, Optional
import unicodedata
import uuid

from pydantic import (
    BaseModel,
    ConfigDict,
    Field,
    field_validator,
    model_serializer,
    model_validator,
)


CONTROL_PROTOCOL_VERSION = "manyforge.execution_control.v2"
EXECUTION_COMMAND_SCHEMA = "manyforge.execution_command.v1"
EXECUTION_COMMAND_RECEIPT_SCHEMA = "manyforge.execution_command_receipt.v1"
EXECUTION_CONTROL_PROJECTION_SCHEMA = "manyforge.execution_control_projection.v1"

# One product profile, deliberately not runtime-tunable.
CONTROL_HEARTBEAT_PERIOD_NS = 250_000_000
CONTROL_HEARTBEAT_DELAYED_NS = 500_000_000
CONTROL_HEARTBEAT_STALE_NS = 1_000_000_000
CONTROL_FRESHNESS_VALID_FOR_NS = 750_000_000
CONTROL_NODE_PROGRESS_PERIOD_NS = 50_000_000
CONTROL_COLD_COMBINE_NS = 50_000_000
CONTROL_MAXIMUM_CLIENTS = 16
CONTROL_MAXIMUM_MESSAGE_BYTES = 64 * 1024
CONTROL_MAXIMUM_SNAPSHOT_BYTES = 16 * 1024 * 1024
CONTROL_MAXIMUM_MESSAGES_PER_SECOND = 64

MAXIMUM_U64 = 18_446_744_073_709_551_615
MAXIMUM_SAFE_INTEGER = 9_007_199_254_740_991
MAXIMUM_IDENTITY_BYTES = 256
MAXIMUM_SOURCE_LABEL_BYTES = 128

ExecutionCommandKind = Literal[
    "program_run",
    "program_motion_stop",
    "program_motion_resume",
    "program_full_stop",
    "program_stop",
    "program_reset",
    "program_resume_full_stop",
    "program_discard_full_stop",
    "program_runtime_override_stage",
    "program_runtime_override_activate",
    "program_runtime_override_clear",
    "program_runtime_override_promote",
    "program_runtime_blackboard_write",
    "program_manual_result_stage",
    "program_manual_handover",
    "gripper_confirm",
    "operation_start",
    "operation_stop",
    "operation_pause",
    "operation_resume",
    "operation_reset",
    "operation_acknowledge_completed",
    "pick_sequence_start",
    "place_sequence_start",
    "cell_full_stop",
    "execution_recovery",
    "stop_recovery",
    "runtime_scale",
]
ExecutionCommandDisposition = Literal[
    "received",
    "rejected",
    "joined",
    "already_current",
    "no_effect",
]
ProgramCycleState = Literal[
    "idle_after_reset",
    "starting",
    "running",
    "stopping",
    "resetting",
    "terminal_awaiting_reset",
    "recovery_required",
]
ControlSemanticStatus = Literal["current", "synchronizing", "unavailable"]
ControlClientIncidentKind = Literal[
    "browser_render_delayed",
    "command_delivery_unconfirmed",
    "control_disconnected",
    "history_detail_request_failed",
    "history_request_failed",
]

_STOP_KINDS = frozenset(
    {
        "program_motion_stop",
        "program_full_stop",
        "program_stop",
        "operation_stop",
        "cell_full_stop",
    }
)


def _bounded_nfc(value: str, *, label: str, maximum_bytes: int) -> str:
    if not value or unicodedata.normalize("NFC", value) != value:
        raise ValueError(f"{label} must be non-empty NFC text")
    if len(value.encode("utf-8")) > maximum_bytes:
        raise ValueError(f"{label} exceeds {maximum_bytes} UTF-8 bytes")
    return value


def _u64_text(value: str, *, label: str) -> str:
    if not value.isascii() or not value.isdigit() or value.startswith("0"):
        raise ValueError(f"{label} must be canonical positive uint64 text")
    parsed = int(value)
    if not 1 <= parsed <= MAXIMUM_U64 or str(parsed) != value:
        raise ValueError(f"{label} is outside uint64")
    return value


class _StrictDTO(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True, frozen=True)


class ControlFreshnessContextDTO(_StrictDTO):
    context_id: str
    receiver_incarnation: str
    semantic_owner_epoch: str
    execution_control_revision: Annotated[int, Field(ge=1, le=MAXIMUM_SAFE_INTEGER)]
    issued_server_sequence: str
    valid_for_ns: Literal[CONTROL_FRESHNESS_VALID_FOR_NS]

    @field_validator("context_id", "receiver_incarnation", "semantic_owner_epoch")
    @classmethod
    def validate_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAXIMUM_IDENTITY_BYTES,
        )

    @field_validator("issued_server_sequence")
    @classmethod
    def validate_sequence(cls, value: str) -> str:
        return _u64_text(value, label="issued_server_sequence")


class ExecutionCommandEnvelopeDTO(_StrictDTO):
    schema_version: Literal["manyforge.execution_command.v1"]
    command_kind: ExecutionCommandKind
    correlation_id: str
    source_label: str
    control_session_id: str | None = None
    client_sequence: str | None = None
    freshness: ControlFreshnessContextDTO
    expected_semantic_owner_epoch: str
    expected_execution_control_revision: Annotated[
        int, Field(ge=1, le=MAXIMUM_SAFE_INTEGER)
    ]

    @field_validator(
        "correlation_id", "control_session_id", "expected_semantic_owner_epoch"
    )
    @classmethod
    def validate_identity(cls, value: str | None, info) -> str | None:
        if value is None:
            return None
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAXIMUM_IDENTITY_BYTES,
        )

    @field_validator("source_label")
    @classmethod
    def validate_source_label(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="source_label",
            maximum_bytes=MAXIMUM_SOURCE_LABEL_BYTES,
        )

    @field_validator("client_sequence")
    @classmethod
    def validate_client_sequence(cls, value: str | None) -> str | None:
        if value is None:
            return None
        return _u64_text(value, label="client_sequence")

    @model_validator(mode="after")
    def paired_session_sequence(self) -> "ExecutionCommandEnvelopeDTO":
        if (self.control_session_id is None) != (self.client_sequence is None):
            raise ValueError("control_session_id and client_sequence must be paired")
        if self.freshness.semantic_owner_epoch != self.expected_semantic_owner_epoch:
            raise ValueError("freshness owner does not match expected owner")
        if (
            self.freshness.execution_control_revision
            != self.expected_execution_control_revision
        ):
            raise ValueError("freshness revision does not match expected revision")
        return self


class ExecutionCommandRequestDTO(_StrictDTO):
    """One strict metadata envelope plus one command-specific strict payload."""

    command_envelope: ExecutionCommandEnvelopeDTO
    payload: dict[str, Any]

    @model_validator(mode="after")
    def bounded_payload(self) -> "ExecutionCommandRequestDTO":
        encoded = json.dumps(
            self.payload,
            allow_nan=False,
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")
        if len(encoded) > CONTROL_MAXIMUM_MESSAGE_BYTES:
            raise ValueError("execution command payload exceeds its byte bound")
        return self


class ExecutionCommandReceiptDTO(_StrictDTO):
    schema_version: Literal["manyforge.execution_command_receipt.v1"]
    correlation_id: str
    control_session_id: str | None
    client_sequence: str | None
    receiver_incarnation: str
    received_server_sequence: str
    received_monotonic_ns: str
    disposition: ExecutionCommandDisposition
    code: str
    semantic_owner_epoch: str
    execution_control_revision: Annotated[int, Field(ge=1, le=MAXIMUM_SAFE_INTEGER)]
    current_identity: dict[str, Any] | None = None

    @field_validator(
        "correlation_id",
        "control_session_id",
        "receiver_incarnation",
        "code",
        "semantic_owner_epoch",
    )
    @classmethod
    def validate_identity(cls, value: str | None, info) -> str | None:
        if value is None:
            return None
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAXIMUM_IDENTITY_BYTES,
        )

    @field_validator(
        "client_sequence", "received_server_sequence", "received_monotonic_ns"
    )
    @classmethod
    def validate_sequence(cls, value: str | None, info) -> str | None:
        if value is None:
            return None
        return _u64_text(value, label=info.field_name)

    @model_validator(mode="after")
    def paired_session_sequence(self) -> "ExecutionCommandReceiptDTO":
        if (self.control_session_id is None) != (self.client_sequence is None):
            raise ValueError("receipt session and client sequence must be paired")
        return self


class ControlClientHelloDTO(_StrictDTO):
    schema_version: Literal["manyforge.execution_control.v2"]
    message_kind: Literal["client_hello"]
    supported_versions: tuple[Literal["manyforge.execution_control.v2"]]
    client_incarnation: str
    source_label: str

    @field_validator("client_incarnation")
    @classmethod
    def validate_client_incarnation(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="client_incarnation",
            maximum_bytes=MAXIMUM_IDENTITY_BYTES,
        )

    @field_validator("source_label")
    @classmethod
    def validate_hello_source_label(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="source_label",
            maximum_bytes=MAXIMUM_SOURCE_LABEL_BYTES,
        )


class ControlHeartbeatEchoDTO(_StrictDTO):
    schema_version: Literal["manyforge.execution_control.v2"]
    message_kind: Literal["heartbeat_echo"]
    control_session_id: str
    client_sequence: str
    challenge_sequence: str

    @field_validator("control_session_id")
    @classmethod
    def validate_heartbeat_session(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="control_session_id",
            maximum_bytes=MAXIMUM_IDENTITY_BYTES,
        )

    @field_validator("client_sequence", "challenge_sequence")
    @classmethod
    def validate_heartbeat_sequence(cls, value: str, info) -> str:
        return _u64_text(value, label=info.field_name)


class ControlStateAppliedDTO(_StrictDTO):
    """Browser credit for one consumed replaceable state value."""

    schema_version: Literal["manyforge.execution_control.v2"]
    message_kind: Literal["state_applied"]
    control_session_id: str
    state_sequence: str

    @field_validator("control_session_id")
    @classmethod
    def validate_state_session(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="control_session_id",
            maximum_bytes=MAXIMUM_IDENTITY_BYTES,
        )

    @field_validator("state_sequence")
    @classmethod
    def validate_state_sequence(cls, value: str) -> str:
        return _u64_text(value, label="state_sequence")


class ControlClientIncidentDTO(_StrictDTO):
    """Strict non-authoritative browser-only incident transition."""

    schema_version: Literal["manyforge.execution_control.v2"]
    message_kind: Literal["client_incident"]
    control_session_id: str
    client_sequence: str
    incident_id: str
    incident_kind: ControlClientIncidentKind
    phase: Literal["activated", "resolved"]
    occurrence_count: Annotated[int, Field(ge=1, le=1_000_000)]
    maximum_delay_ns: Annotated[int, Field(ge=0, le=MAXIMUM_SAFE_INTEGER)] | None
    reason_code: str

    @field_validator("control_session_id", "incident_id")
    @classmethod
    def validate_incident_identity(cls, value: str, info) -> str:
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAXIMUM_IDENTITY_BYTES,
        )

    @field_validator("client_sequence")
    @classmethod
    def validate_incident_sequence(cls, value: str) -> str:
        return _u64_text(value, label="client_sequence")

    @field_validator("reason_code")
    @classmethod
    def validate_incident_reason(cls, value: str) -> str:
        return _bounded_nfc(value, label="reason_code", maximum_bytes=256)


class ExecutionControlCommandDTO(_StrictDTO):
    schema_version: Literal["manyforge.execution_control.v2"]
    message_kind: Literal["command"]
    control_session_id: str
    client_sequence: str
    command: ExecutionCommandRequestDTO

    @field_validator("control_session_id")
    @classmethod
    def validate_command_session(cls, value: str) -> str:
        return _bounded_nfc(
            value,
            label="control_session_id",
            maximum_bytes=MAXIMUM_IDENTITY_BYTES,
        )

    @field_validator("client_sequence")
    @classmethod
    def validate_command_sequence(cls, value: str) -> str:
        return _u64_text(value, label="client_sequence")

    @model_validator(mode="after")
    def exact_command_session(self) -> "ExecutionControlCommandDTO":
        envelope = self.command.command_envelope
        if (
            envelope.control_session_id != self.control_session_id
            or envelope.client_sequence != self.client_sequence
        ):
            raise ValueError("command envelope does not match its control session")
        return self


class ControlServerHelloDTO(_StrictDTO):
    schema_version: Literal["manyforge.execution_control.v2"]
    message_kind: Literal["server_hello"]
    receiver_incarnation: str
    control_session_id: str
    heartbeat_period_ns: Literal[250_000_000]
    heartbeat_delayed_ns: Literal[500_000_000]
    heartbeat_stale_ns: Literal[1_000_000_000]
    maximum_message_bytes: Literal[65_536]
    maximum_messages_per_second: Literal[64]


class ControlHeartbeatChallengeDTO(_StrictDTO):
    schema_version: Literal["manyforge.execution_control.v2"]
    message_kind: Literal["heartbeat_challenge"]
    control_session_id: str
    server_sequence: str
    challenge_sequence: str
    received_client_sequence: str | None
    semantic_status: ControlSemanticStatus
    semantic_reason: str | None
    freshness: ControlFreshnessContextDTO | None

    @model_validator(mode="after")
    def paired_semantic_context(self) -> "ControlHeartbeatChallengeDTO":
        if self.semantic_status == "current":
            if self.semantic_reason is not None or self.freshness is None:
                raise ValueError("current heartbeat requires only freshness")
        elif self.semantic_reason is None or self.freshness is not None:
            raise ValueError("non-current heartbeat requires a reason and no freshness")
        if self.semantic_reason is not None:
            _bounded_nfc(
                self.semantic_reason,
                label="semantic_reason",
                maximum_bytes=1024,
            )
        return self


class ExecutionControlSnapshotDTO(_StrictDTO):
    schema_version: Literal["manyforge.execution_control.v2"]
    message_kind: Literal["snapshot"]
    control_session_id: str
    server_sequence: str
    state_sequence: str
    received_client_sequence: str | None
    control: "ExecutionControlProjectionDTO"
    shell: dict[str, Any]
    complete: Literal[True]


class ExecutionControlDeltaDTO(_StrictDTO):
    schema_version: Literal["manyforge.execution_control.v2"]
    message_kind: Literal["delta"]
    control_session_id: str
    server_sequence: str
    state_sequence: str | None = None
    received_client_sequence: str | None = None
    event_class: Literal["critical", "node_progress", "cold"]
    control: Optional["ExecutionControlProjectionDTO"]
    # Every current producer carries a shell: owner deltas carry at least the
    # Program fact and Composer-local deltas carry the Draft identity. Keeping
    # an unused control-only variant would make the generated and browser
    # contracts disagree for no operational benefit.
    shell: dict[str, Any]

    @model_validator(mode="after")
    def paired_owner_state(self) -> "ExecutionControlDeltaDTO":
        if self.control is None:
            if {"state_sequence", "received_client_sequence"} & self.model_fields_set:
                raise ValueError(
                    "local Draft delta cannot claim owner state or receipt"
                )
            if self.event_class != "cold":
                raise ValueError("local Draft delta must be cold")
        else:
            if self.state_sequence is None:
                raise ValueError("owner delta requires its state sequence")
            _u64_text(self.state_sequence, label="state_sequence")
            if "received_client_sequence" not in self.model_fields_set:
                raise ValueError("owner delta requires its receive watermark")
            if self.received_client_sequence is not None:
                _u64_text(
                    self.received_client_sequence, label="received_client_sequence"
                )
        if self.shell.get("complete") is not False:
            raise ValueError("delta shell must be explicitly incomplete")
        return self

    @model_serializer(mode="wrap")
    def serialize_delta(self, handler: Any) -> dict[str, Any]:
        value = handler(self)
        if self.control is None:
            value.pop("state_sequence", None)
            value.pop("received_client_sequence", None)
        return value


class ExecutionCommandReceiptMessageDTO(_StrictDTO):
    schema_version: Literal["manyforge.execution_control.v2"]
    message_kind: Literal["command_receipt"]
    control_session_id: str
    server_sequence: str
    received_client_sequence: str
    emitted_monotonic_ns: str
    receipt: ExecutionCommandReceiptDTO
    result: dict[str, Any] | None

    @model_validator(mode="after")
    def cumulative_receive_watermark(self) -> "ExecutionCommandReceiptMessageDTO":
        _u64_text(self.received_client_sequence, label="received_client_sequence")
        if self.control_session_id != self.receipt.control_session_id or int(
            self.received_client_sequence
        ) < int(self.receipt.client_sequence):
            raise ValueError("receipt must name its session and a covered command")
        return self

    @field_validator("emitted_monotonic_ns")
    @classmethod
    def validate_emitted_time(cls, value: str) -> str:
        return _u64_text(value, label="emitted_monotonic_ns")


class ControlResyncRequiredDTO(_StrictDTO):
    schema_version: Literal["manyforge.execution_control.v2"]
    message_kind: Literal["resync_required"]
    control_session_id: str
    server_sequence: str
    reason_code: str


class AllowedExecutionCommandDTO(_StrictDTO):
    command_kind: ExecutionCommandKind
    semantic_target: dict[str, Any]
    allowed: bool
    disabled_reason: str | None

    @model_validator(mode="after")
    def exact_disabled_reason(self) -> "AllowedExecutionCommandDTO":
        if self.allowed == (self.disabled_reason is not None):
            raise ValueError(
                "allowed commands omit a disabled reason only when allowed"
            )
        if self.disabled_reason is not None:
            _bounded_nfc(
                self.disabled_reason,
                label="disabled_reason",
                maximum_bytes=1024,
            )
        return self


class ExecutionControlProjectionDTO(_StrictDTO):
    schema_version: Literal["manyforge.execution_control_projection.v1"]
    receiver_incarnation: str
    semantic_owner_epoch: str
    execution_control_revision: Annotated[int, Field(ge=1, le=MAXIMUM_SAFE_INTEGER)]
    cycle_state: ProgramCycleState
    cycle_generation: Annotated[int, Field(ge=1, le=MAXIMUM_SAFE_INTEGER)]
    program_run_id: str | None
    run_revision: Annotated[int, Field(ge=1, le=MAXIMUM_SAFE_INTEGER)] | None
    allowed_commands: list[AllowedExecutionCommandDTO]
    causal_correlation_id: str | None
    causal_session_id: str | None
    causal_client_sequence: str | None
    current_rejection: dict[str, Any] | None
    current_failure: dict[str, Any] | None
    recovery_required: bool
    freshness: ControlFreshnessContextDTO

    @field_validator(
        "receiver_incarnation",
        "semantic_owner_epoch",
        "program_run_id",
        "causal_correlation_id",
        "causal_session_id",
    )
    @classmethod
    def validate_identity(cls, value: str | None, info) -> str | None:
        if value is None:
            return None
        return _bounded_nfc(
            value,
            label=info.field_name,
            maximum_bytes=MAXIMUM_IDENTITY_BYTES,
        )

    @field_validator("causal_client_sequence")
    @classmethod
    def validate_sequence(cls, value: str | None) -> str | None:
        if value is None:
            return None
        return _u64_text(value, label="causal_client_sequence")

    @model_validator(mode="after")
    def exact_shape(self) -> "ExecutionControlProjectionDTO":
        if (self.causal_session_id is None) != (self.causal_client_sequence is None):
            raise ValueError("causal session and client sequence must be paired")
        if self.freshness.receiver_incarnation != self.receiver_incarnation:
            raise ValueError("projection freshness receiver differs")
        if self.freshness.semantic_owner_epoch != self.semantic_owner_epoch:
            raise ValueError("projection freshness owner differs")
        if self.freshness.execution_control_revision != self.execution_control_revision:
            raise ValueError("projection freshness revision differs")
        kinds = [item.command_kind for item in self.allowed_commands]
        if kinds != sorted(kinds) or len(kinds) != len(set(kinds)):
            raise ValueError("allowed_commands must be sorted and unique")
        return self


@dataclass(frozen=True, slots=True)
class ExecutionIngressStamp:
    transport_kind: Literal["rest", "websocket", "ros2", "local"]
    receiver_incarnation: str
    received_server_sequence: int
    received_monotonic_ns: int
    trusted_principal_id: str | None = None
    assurance: str | None = None
    # Authenticated issue-time expiry in this receiver's monotonic domain.
    # It is never renewed by queue admission or forwarded public timestamps.
    valid_until_monotonic_ns: int | None = None


@dataclass(frozen=True, slots=True)
class ExecutionIngressResult:
    disposition: ExecutionCommandDisposition
    code: str
    semantic_owner_epoch: str
    execution_control_revision: int
    current_identity: Mapping[str, Any] | None = None


class ControlProtocolError(RuntimeError):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


class ControlFreshnessAuthority:
    """Issue stateless signed contexts against one injected monotonic clock."""

    def __init__(
        self,
        *,
        clock_ns: Callable[[], int] = time.monotonic_ns,
        receiver_incarnation: str | None = None,
        secret: bytes | None = None,
    ) -> None:
        self._clock_ns = clock_ns
        self._secret = secret or secrets.token_bytes(32)
        if len(self._secret) < 32:
            raise ValueError("freshness signing secret must contain at least 32 bytes")
        self._receiver_incarnation = receiver_incarnation or (
            f"control-receiver.{uuid.uuid4().hex}"
        )
        _bounded_nfc(
            self._receiver_incarnation,
            label="receiver_incarnation",
            maximum_bytes=MAXIMUM_IDENTITY_BYTES,
        )
        self._sequence = 0
        self._lock = Lock()

    @property
    def receiver_incarnation(self) -> str:
        return self._receiver_incarnation

    @property
    def rollover_required(self) -> bool:
        with self._lock:
            return self._sequence >= MAXIMUM_U64

    @property
    def sequence(self) -> int:
        return self._sequence

    @staticmethod
    def _canonical_claims(
        *,
        receiver_incarnation: str,
        semantic_owner_epoch: str,
        execution_control_revision: int,
        issued_server_sequence: int,
        issued_at_ns: int,
        valid_for_ns: int,
    ) -> bytes:
        return json.dumps(
            {
                "execution_control_revision": execution_control_revision,
                "issued_at_ns": str(issued_at_ns),
                "issued_server_sequence": str(issued_server_sequence),
                "receiver_incarnation": receiver_incarnation,
                "semantic_owner_epoch": semantic_owner_epoch,
                "valid_for_ns": valid_for_ns,
            },
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")

    def issue(
        self,
        *,
        semantic_owner_epoch: str,
        execution_control_revision: int,
        valid_for_ns: int = CONTROL_FRESHNESS_VALID_FOR_NS,
    ) -> ControlFreshnessContextDTO:
        _bounded_nfc(
            semantic_owner_epoch,
            label="semantic_owner_epoch",
            maximum_bytes=MAXIMUM_IDENTITY_BYTES,
        )
        if not 1 <= execution_control_revision <= MAXIMUM_SAFE_INTEGER:
            raise ValueError("execution_control_revision is outside its bound")
        if not 1 <= valid_for_ns <= CONTROL_FRESHNESS_VALID_FOR_NS:
            raise ValueError("valid_for_ns exceeds the frozen product profile")
        with self._lock:
            if self._sequence >= MAXIMUM_U64:
                raise ControlProtocolError(
                    "control_sequence_rollover_required",
                    "control receiver must rotate at a quiescent boundary",
                )
            self._sequence += 1
            issued_server_sequence = self._sequence
            issued_at_ns = self._clock_ns()
        if not 0 <= issued_at_ns <= MAXIMUM_U64:
            raise ControlProtocolError(
                "control_monotonic_clock_invalid",
                "backend monotonic clock is outside uint64",
            )
        claims = self._canonical_claims(
            receiver_incarnation=self._receiver_incarnation,
            semantic_owner_epoch=semantic_owner_epoch,
            execution_control_revision=execution_control_revision,
            issued_server_sequence=issued_server_sequence,
            issued_at_ns=issued_at_ns,
            valid_for_ns=valid_for_ns,
        )
        signature = hmac.new(self._secret, claims, hashlib.sha256).digest()
        context_id = (
            base64.urlsafe_b64encode(issued_at_ns.to_bytes(8, "big") + signature)
            .decode("ascii")
            .rstrip("=")
        )
        return ControlFreshnessContextDTO(
            context_id=context_id,
            receiver_incarnation=self._receiver_incarnation,
            semantic_owner_epoch=semantic_owner_epoch,
            execution_control_revision=execution_control_revision,
            issued_server_sequence=str(issued_server_sequence),
            valid_for_ns=valid_for_ns,
        )

    def validate(
        self,
        context: ControlFreshnessContextDTO,
        *,
        current_owner_epoch: str,
        current_control_revision: int,
    ) -> int:
        now_ns = self._clock_ns()
        try:
            sequence = int(context.issued_server_sequence)
            token = base64.urlsafe_b64decode(context.context_id + "===")
            issued_at_ns = int.from_bytes(token[:8], "big")
        except (ValueError, TypeError) as error:
            raise ControlProtocolError(
                "control_freshness_invalid",
                "freshness context is malformed",
            ) from error
        claims = self._canonical_claims(
            receiver_incarnation=context.receiver_incarnation,
            semantic_owner_epoch=context.semantic_owner_epoch,
            execution_control_revision=context.execution_control_revision,
            issued_server_sequence=sequence,
            issued_at_ns=issued_at_ns,
            valid_for_ns=context.valid_for_ns,
        )
        expected = hmac.new(self._secret, claims, hashlib.sha256).digest()
        if (
            len(token) != 40
            or int.from_bytes(token[:8], "big") != issued_at_ns
            or not hmac.compare_digest(token[8:], expected)
        ):
            raise ControlProtocolError(
                "control_freshness_invalid",
                "freshness context signature is invalid",
            )
        if context.receiver_incarnation != self._receiver_incarnation:
            raise ControlProtocolError(
                "control_receiver_replaced",
                "control receiver incarnation changed",
            )
        if (
            context.semantic_owner_epoch != current_owner_epoch
            or context.execution_control_revision != current_control_revision
        ):
            raise ControlProtocolError(
                "control_projection_stale",
                "semantic owner or control revision changed",
            )
        elapsed = now_ns - issued_at_ns
        if elapsed < 0 or elapsed > context.valid_for_ns:
            raise ControlProtocolError(
                "control_freshness_expired",
                "freshness context expired before command admission",
            )
        return issued_at_ns + context.valid_for_ns

    def rotate_quiescent(self, *, active_sessions: int, inflight_commands: int) -> None:
        if active_sessions or inflight_commands:
            raise ControlProtocolError(
                "control_sequence_rollover_busy",
                "control receiver can rotate only while quiescent",
            )
        with self._lock:
            self._receiver_incarnation = f"control-receiver.{uuid.uuid4().hex}"
            self._sequence = 0
            self._secret = secrets.token_bytes(32)


def is_stop_command(kind: ExecutionCommandKind) -> bool:
    return kind in _STOP_KINDS


__all__ = [
    "AllowedExecutionCommandDTO",
    "CONTROL_COLD_COMBINE_NS",
    "CONTROL_FRESHNESS_VALID_FOR_NS",
    "CONTROL_HEARTBEAT_DELAYED_NS",
    "CONTROL_HEARTBEAT_PERIOD_NS",
    "CONTROL_HEARTBEAT_STALE_NS",
    "CONTROL_MAXIMUM_CLIENTS",
    "CONTROL_MAXIMUM_MESSAGES_PER_SECOND",
    "CONTROL_MAXIMUM_MESSAGE_BYTES",
    "CONTROL_MAXIMUM_SNAPSHOT_BYTES",
    "CONTROL_NODE_PROGRESS_PERIOD_NS",
    "CONTROL_PROTOCOL_VERSION",
    "ControlClientHelloDTO",
    "ControlClientIncidentDTO",
    "ControlClientIncidentKind",
    "ControlFreshnessAuthority",
    "ControlFreshnessContextDTO",
    "ControlHeartbeatChallengeDTO",
    "ControlHeartbeatEchoDTO",
    "ControlProtocolError",
    "ControlResyncRequiredDTO",
    "ControlServerHelloDTO",
    "ControlStateAppliedDTO",
    "ControlSemanticStatus",
    "EXECUTION_COMMAND_RECEIPT_SCHEMA",
    "EXECUTION_COMMAND_SCHEMA",
    "EXECUTION_CONTROL_PROJECTION_SCHEMA",
    "ExecutionCommandEnvelopeDTO",
    "ExecutionCommandReceiptDTO",
    "ExecutionCommandRequestDTO",
    "ExecutionCommandReceiptMessageDTO",
    "ExecutionControlProjectionDTO",
    "ExecutionControlCommandDTO",
    "ExecutionControlDeltaDTO",
    "ExecutionControlSnapshotDTO",
    "ExecutionIngressResult",
    "ExecutionIngressStamp",
    "MAXIMUM_U64",
    "ProgramCycleState",
    "is_stop_command",
]
