from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path

import pytest
from pydantic import ValidationError

from manyforge_headless_api.control_protocol import (
    CONTROL_FRESHNESS_VALID_FOR_NS,
    CONTROL_PROTOCOL_VERSION,
    ControlClientIncidentDTO,
    ControlFreshnessAuthority,
    ControlProtocolError,
    EXECUTION_COMMAND_SCHEMA,
    ExecutionCommandEnvelopeDTO,
    ExecutionCommandRequestDTO,
    ExecutionControlDeltaDTO,
)


@dataclass
class ManualClock:
    now_ns: int = 1_000_000_000

    def __call__(self) -> int:
        return self.now_ns

    def advance(self, nanoseconds: int) -> None:
        self.now_ns += nanoseconds


def _envelope(
    authority: ControlFreshnessAuthority,
    *,
    revision: int = 4,
) -> ExecutionCommandEnvelopeDTO:
    return ExecutionCommandEnvelopeDTO(
        schema_version=EXECUTION_COMMAND_SCHEMA,
        command_kind="program_run",
        source_label="test-local-adapter",
        control_session_number=1,
        client_sequence="1",
        freshness=authority.issue(execution_control_revision=revision),
        expected_execution_control_revision=revision,
    )


def test_freshness_is_stateless_signed_and_expires_at_exact_backend_boundary() -> None:
    clock = ManualClock()
    authority = ControlFreshnessAuthority(
        clock_ns=clock,
        secret=b"x" * 32,
    )
    context = _envelope(authority).freshness
    issued_at = clock.now_ns

    deadline = authority.validate(
        context,
        current_control_revision=4,
        received_at_ns=issued_at + CONTROL_FRESHNESS_VALID_FOR_NS,
    )
    assert deadline == 1_000_000_000 + CONTROL_FRESHNESS_VALID_FOR_NS
    with pytest.raises(ControlProtocolError) as expired:
        authority.validate(
            context,
            current_control_revision=4,
            received_at_ns=issued_at + CONTROL_FRESHNESS_VALID_FOR_NS + 1,
        )
    assert expired.value.code == "control_freshness_expired"


def test_freshness_age_is_measured_at_receipt_not_at_validation() -> None:
    """OD-240: a slow backend never expires a command it received in time."""

    clock = ManualClock()
    authority = ControlFreshnessAuthority(
        clock_ns=clock,
        secret=b"x" * 32,
    )
    context = _envelope(authority).freshness
    received_in_time = clock.now_ns + CONTROL_FRESHNESS_VALID_FOR_NS - 1
    received_late = clock.now_ns + CONTROL_FRESHNESS_VALID_FOR_NS + 1
    # The backend validates long after both receipts.
    clock.advance(10 * CONTROL_FRESHNESS_VALID_FOR_NS)

    authority.validate(
        context, current_control_revision=4, received_at_ns=received_in_time
    )
    with pytest.raises(ControlProtocolError) as expired:
        authority.validate(
            context, current_control_revision=4, received_at_ns=received_late
        )
    assert expired.value.code == "control_freshness_expired"


def test_freshness_profile_and_delta_shape_are_one_closed_contract() -> None:
    authority = ControlFreshnessAuthority(secret=b"x" * 32)
    context = _envelope(authority).freshness.model_dump(mode="json")
    with pytest.raises(ValidationError):
        type(_envelope(authority).freshness).model_validate(
            {**context, "valid_for_ns": CONTROL_FRESHNESS_VALID_FOR_NS - 1}
        )

    fixture = json.loads(
        (Path(__file__).parent / "fixtures/control-v2-local-delta.json").read_text()
    )
    delta = fixture["local_delta"]
    parsed = ExecutionControlDeltaDTO.model_validate(delta)
    assert parsed.model_dump(mode="json") == delta
    for invalid in fixture["invalid_overlays"]:
        with pytest.raises(ValidationError):
            ExecutionControlDeltaDTO.model_validate({**delta, **invalid})
    with pytest.raises(ValidationError):
        ExecutionControlDeltaDTO.model_validate({**delta, "shell": None})


def test_freshness_rejects_tamper_and_revision() -> None:
    clock = ManualClock()
    authority = ControlFreshnessAuthority(
        clock_ns=clock,
        secret=b"x" * 32,
    )
    context = _envelope(authority).freshness
    with pytest.raises(ControlProtocolError) as rejected:
        authority.validate(
            context, current_control_revision=5, received_at_ns=clock.now_ns
        )
    assert rejected.value.code == "control_projection_stale"

    # A middle character carries six signature bits (the last one carries
    # only two, and may already be "A"): any change alters the token.
    position = len(context.context_id) // 2
    replacement = "B" if context.context_id[position] == "A" else "A"
    tampered = context.model_copy(
        update={
            "context_id": context.context_id[:position]
            + replacement
            + context.context_id[position + 1 :]
        }
    )
    with pytest.raises(ControlProtocolError) as invalid:
        authority.validate(
            tampered,
            current_control_revision=4,
            received_at_ns=clock.now_ns,
        )
    assert invalid.value.code == "control_freshness_invalid"


def test_virtual_time_beyond_thirty_years_creates_no_retained_history() -> None:
    clock = ManualClock()
    authority = ControlFreshnessAuthority(
        clock_ns=clock,
        secret=b"x" * 32,
    )
    thirty_one_years_ns = 31 * 366 * 24 * 60 * 60 * 1_000_000_000
    clock.advance(thirty_one_years_ns)
    context = authority.issue(execution_control_revision=1)
    authority.validate(
        context,
        current_control_revision=1,
        received_at_ns=clock.now_ns,
    )
    assert not hasattr(authority, "_contexts")
    assert not hasattr(authority, "_issued_at_by_sequence")


def test_command_envelope_is_closed_paired_and_payload_bounded() -> None:
    authority = ControlFreshnessAuthority(secret=b"x" * 32)
    envelope = _envelope(authority)
    request = ExecutionCommandRequestDTO(
        command_envelope=envelope,
        payload={"scenario_id": "lite6"},
    )
    assert request.command_envelope.client_sequence == "1"
    with pytest.raises(ValidationError):
        ExecutionCommandEnvelopeDTO.model_validate(
            {
                **envelope.model_dump(mode="json"),
                "control_session_number": None,
                "extra": True,
            }
        )
    with pytest.raises(ValidationError):
        ExecutionCommandRequestDTO(
            command_envelope=envelope,
            payload={"value": "x" * (64 * 1024)},
        )


def test_client_incident_is_a_closed_non_authoritative_enum() -> None:
    incident = {
        "schema_version": CONTROL_PROTOCOL_VERSION,
        "message_kind": "client_incident",
        "control_session_number": 1,
        "client_sequence": "1",
        "incident_id": "incident.1",
        "incident_kind": "control_disconnected",
        "phase": "activated",
        "occurrence_count": 1,
        "maximum_delay_ns": None,
        "reason_code": "control_websocket_closed",
    }
    assert ControlClientIncidentDTO.model_validate(incident).incident_kind == (
        "control_disconnected"
    )
    with pytest.raises(ValidationError):
        ControlClientIncidentDTO.model_validate(
            {**incident, "incident_kind": "execute_arbitrary_payload"}
        )
    with pytest.raises(ValidationError):
        ControlClientIncidentDTO.model_validate({**incident, "payload": {}})
    for kind in (
        "control_projection_invalid",
        "viewer_model_unavailable",
        "viewer_scene_unverified",
        "viewer_observation_stale",
        "viewer_geometry_unavailable",
    ):
        assert (
            ControlClientIncidentDTO.model_validate(
                {**incident, "incident_kind": kind, "reason_code": kind}
            ).incident_kind
            == kind
        )


def test_receipt_v3_always_carries_the_key_and_the_command_row() -> None:
    """OD-214: the receipt names the formed key and a runner command's row;
    both members are always present (null for a command never forwarded)."""

    from manyforge_headless_api.control_protocol import (
        EXECUTION_COMMAND_RECEIPT_SCHEMA,
        ExecutionCommandReceiptDTO,
    )

    assert EXECUTION_COMMAND_RECEIPT_SCHEMA == "manyforge.execution_command_receipt.v3"
    base = {
        "schema_version": EXECUTION_COMMAND_RECEIPT_SCHEMA,
        "control_session_number": 1,
        "client_sequence": "3",
        "received_server_sequence": "9",
        "received_monotonic_ns": "100",
        "disposition": "rejected",
        "code": "command_not_allowed",
        "execution_control_revision": 7,
    }
    rejected = ExecutionCommandReceiptDTO.model_validate(base).model_dump(mode="json")
    assert rejected["request_key"] is None and rejected["command"] is None
    row = {"kind": "start", "request_key": "0" * 32, "step": "admitting",
           "accepted_at_ns": "1763112345678901234", "end": None}
    received = ExecutionCommandReceiptDTO.model_validate({
        **base, "disposition": "received", "code": "program_run_received",
        "request_key": "0" * 32, "command": row,
    }).model_dump(mode="json")
    assert received["command"] == row
    with pytest.raises(ValidationError):
        ExecutionCommandReceiptDTO.model_validate(
            {**base, "schema_version": "manyforge.execution_command_receipt.v2"}
        )
