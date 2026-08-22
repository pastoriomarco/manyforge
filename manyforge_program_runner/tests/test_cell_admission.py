"""Strict runner-local admission wire validation."""

from __future__ import annotations

import inspect
from typing import Any

import pytest

from manyforge_program_runner import cell_admission
from manyforge_program_runner.cell_admission import (
    CellAdmissionClient,
    CellAdmissionError,
)


def test_client_has_no_headless_import_or_readiness_writer() -> None:
    source = inspect.getsource(cell_admission)
    assert "manyforge_headless" not in source
    assert "cell_admission_update_readiness" not in source
    assert not hasattr(CellAdmissionClient, "update_readiness")


@pytest.mark.parametrize(
    "payload",
    [
        b'{"api_version":"manyforge.execution_command_response.v1",'
        b'"api_version":"manyforge.execution_command_response.v1"}',
        b'{"ok":NaN}',
        b'{ "ok":true}',
        b"[]",
        b"\xff",
    ],
)
def test_strict_response_rejects_duplicate_nonfinite_noncanonical_packets(
    payload: bytes,
) -> None:
    with pytest.raises(CellAdmissionError) as invalid:
        CellAdmissionClient._strict_object(payload)
    assert invalid.value.code == "execution_command_response_invalid"


def test_response_envelope_rejects_wrong_packet_version(
    monkeypatch: pytest.MonkeyPatch, tmp_path
) -> None:
    client = CellAdmissionClient(tmp_path)
    monkeypatch.setattr(
        client,
        "_exchange",
        lambda _packet: {
            "api_version": "manyforge.execution_command_response.v0",
            "command": "cell_admission_get",
            "ok": True,
            "authoring_admission": {
                "admission_generation": 1,
                "authoring_allowed": True,
            },
        },
    )
    with pytest.raises(CellAdmissionError) as invalid:
        client.get()
    assert invalid.value.code == "execution_command_response_invalid"


def _intent() -> dict[str, Any]:
    return {
        "claim_kind": "program_cycle",
        "expected_admission_generation": 7,
        "intervention_evidence": None,
        "owner_epoch": "runner-epoch",
        "owner_role": "program_runner",
        "owner_transaction_id": "program-cycle.1",
        "program_cycle_evidence": {
            "applied_program_revision": "sha256:" + "2" * 64,
            "draft_program_revision": "sha256:" + "2" * 64,
        },
        "request_id": "run.start.1",
        "request_payload_digest": "sha256:" + "3" * 64,
        "scope": {"configurations": [], "resources": []},
    }


def test_exact_native_get_request_and_admission_response(
    monkeypatch: pytest.MonkeyPatch, tmp_path
) -> None:
    client = CellAdmissionClient(tmp_path)
    packets: list[dict[str, Any]] = []

    def exchange(packet: dict[str, Any]) -> dict[str, Any]:
        packets.append(packet)
        return {
            "api_version": "manyforge.execution_command_response.v1",
            "authoring_admission": {
                "admission_generation": 7,
                "authoring_allowed": True,
            },
            "command": "cell_admission_get",
            "ok": True,
        }

    monkeypatch.setattr(client, "_exchange", exchange)
    assert client.get() == {
        "admission_generation": 7,
        "authoring_allowed": True,
    }
    assert packets == [
        {
            "api_version": "manyforge.execution_command.v1",
            "command": "cell_admission_get",
        }
    ]


def test_exact_native_acquire_request_and_claim_response(
    monkeypatch: pytest.MonkeyPatch, tmp_path
) -> None:
    client = CellAdmissionClient(tmp_path)
    intent = _intent()
    packets: list[dict[str, Any]] = []

    def exchange(packet: dict[str, Any]) -> dict[str, Any]:
        packets.append(packet)
        return {
            "api_version": "manyforge.execution_command_response.v1",
            "claim": {
                "accepted_at_ns": 42,
                "acquired_admission_generation": 7,
                "claim_generation": 3,
                "intent": intent,
            },
            "command": "cell_admission_acquire",
            "ok": True,
        }

    monkeypatch.setattr(client, "_exchange", exchange)
    assert client.acquire(intent)["claim_generation"] == 3
    assert packets == [
        {
            "api_version": "manyforge.execution_command.v1",
            "command": "cell_admission_acquire",
            "request": intent,
        }
    ]


def test_exact_native_error_is_preserved_as_known_outcome(
    monkeypatch: pytest.MonkeyPatch, tmp_path
) -> None:
    client = CellAdmissionClient(tmp_path)
    monkeypatch.setattr(
        client,
        "_exchange",
        lambda _packet: {
            "api_version": "manyforge.execution_command_response.v1",
            "command": "cell_admission_acquire",
            "error": {
                "code": "stale_generation",
                "details": {"admission_generation": "8"},
                "message": "expected generation is stale",
            },
            "ok": False,
        },
    )
    with pytest.raises(CellAdmissionError) as rejected:
        client.acquire(_intent())
    assert rejected.value.code == "stale_generation"
    assert rejected.value.details == {"admission_generation": "8"}
    assert rejected.value.outcome_unknown is False


@pytest.mark.parametrize(
    "response",
    [
        {
            "api_version": "manyforge.execution_command_response.v1",
            "command": "cell_admission_get",
            "ok": True,
            "result": {"admission_generation": 1, "authoring_allowed": True},
        },
        {
            "api_version": "manyforge.execution_command_response.v1",
            "authoring_admission": {
                "admission_generation": 1,
                "authoring_allowed": True,
            },
            "command": "cell_admission_get",
            "error": None,
            "ok": True,
        },
    ],
)
def test_legacy_generic_response_envelopes_are_rejected(
    monkeypatch: pytest.MonkeyPatch, tmp_path, response: dict[str, Any]
) -> None:
    client = CellAdmissionClient(tmp_path)
    monkeypatch.setattr(client, "_exchange", lambda _packet: response)
    with pytest.raises(CellAdmissionError) as invalid:
        client.get()
    assert invalid.value.code == "execution_command_response_invalid"
    assert invalid.value.outcome_unknown is True
