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
    MAXIMUM_U64,
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
    owner: str = "runner.1",
    revision: int = 4,
) -> ExecutionCommandEnvelopeDTO:
    return ExecutionCommandEnvelopeDTO(
        schema_version=EXECUTION_COMMAND_SCHEMA,
        command_kind="program_run",
        correlation_id="correlation.1",
        source_label="test-local-adapter",
        control_session_id="session.1",
        client_sequence="1",
        freshness=authority.issue(
            semantic_owner_epoch=owner,
            execution_control_revision=revision,
        ),
        expected_semantic_owner_epoch=owner,
        expected_execution_control_revision=revision,
    )


def test_freshness_is_stateless_signed_and_expires_at_exact_backend_boundary() -> None:
    clock = ManualClock()
    authority = ControlFreshnessAuthority(
        clock_ns=clock,
        receiver_incarnation="receiver.1",
        secret=b"x" * 32,
    )
    context = _envelope(authority).freshness

    clock.advance(CONTROL_FRESHNESS_VALID_FOR_NS)
    deadline = authority.validate(
        context,
        current_owner_epoch="runner.1",
        current_control_revision=4,
    )
    assert deadline == 1_000_000_000 + CONTROL_FRESHNESS_VALID_FOR_NS
    clock.advance(1)
    with pytest.raises(ControlProtocolError) as expired:
        authority.validate(
            context,
            current_owner_epoch="runner.1",
            current_control_revision=4,
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


def test_freshness_rejects_tamper_owner_revision_and_receiver_replacement() -> None:
    clock = ManualClock()
    authority = ControlFreshnessAuthority(
        clock_ns=clock,
        receiver_incarnation="receiver.1",
        secret=b"x" * 32,
    )
    context = _envelope(authority).freshness
    for owner, revision, code in (
        ("runner.2", 4, "control_projection_stale"),
        ("runner.1", 5, "control_projection_stale"),
    ):
        with pytest.raises(ControlProtocolError) as rejected:
            authority.validate(
                context,
                current_owner_epoch=owner,
                current_control_revision=revision,
            )
        assert rejected.value.code == code

    tampered = context.model_copy(update={"context_id": context.context_id[:-1] + "A"})
    with pytest.raises(ControlProtocolError) as invalid:
        authority.validate(
            tampered,
            current_owner_epoch="runner.1",
            current_control_revision=4,
        )
    assert invalid.value.code == "control_freshness_invalid"

    authority.rotate_quiescent(active_sessions=0, inflight_commands=0)
    with pytest.raises(ControlProtocolError) as replaced:
        authority.validate(
            context,
            current_owner_epoch="runner.1",
            current_control_revision=4,
        )
    assert replaced.value.code in {
        "control_freshness_invalid",
        "control_receiver_replaced",
    }


def test_sequence_is_lossless_and_rolls_only_at_a_quiescent_boundary() -> None:
    authority = ControlFreshnessAuthority(
        receiver_incarnation="receiver.1",
        secret=b"x" * 32,
    )
    authority._sequence = MAXIMUM_U64 - 1  # exercise checked boundary directly
    final = authority.issue(
        semantic_owner_epoch="runner.1",
        execution_control_revision=1,
    )
    assert final.issued_server_sequence == str(MAXIMUM_U64)
    with pytest.raises(ControlProtocolError) as exhausted:
        authority.issue(
            semantic_owner_epoch="runner.1",
            execution_control_revision=1,
        )
    assert exhausted.value.code == "control_sequence_rollover_required"
    with pytest.raises(ControlProtocolError) as busy:
        authority.rotate_quiescent(active_sessions=1, inflight_commands=0)
    assert busy.value.code == "control_sequence_rollover_busy"
    authority.rotate_quiescent(active_sessions=0, inflight_commands=0)
    assert (
        authority.issue(
            semantic_owner_epoch="runner.1",
            execution_control_revision=1,
        ).issued_server_sequence
        == "1"
    )


def test_virtual_time_beyond_thirty_years_creates_no_retained_history() -> None:
    clock = ManualClock()
    authority = ControlFreshnessAuthority(
        clock_ns=clock,
        receiver_incarnation="receiver.1",
        secret=b"x" * 32,
    )
    thirty_one_years_ns = 31 * 366 * 24 * 60 * 60 * 1_000_000_000
    clock.advance(thirty_one_years_ns)
    context = authority.issue(
        semantic_owner_epoch="runner.1",
        execution_control_revision=1,
    )
    authority.validate(
        context,
        current_owner_epoch="runner.1",
        current_control_revision=1,
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
                "control_session_id": None,
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
        "control_session_id": "session.1",
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
