"""Strict Headless owner-adapter admission wire validation."""

from __future__ import annotations

import inspect
import errno
import os
import socket
import struct
import threading
import time
from typing import Any

import pytest

from manyforge_headless_api import cell_admission
from manyforge_headless_api.cell_admission import (
    CellAdmissionClient,
    CellAdmissionError,
)


def test_client_has_no_headless_import_or_readiness_writer() -> None:
    source = inspect.getsource(cell_admission)
    assert "manyforge_program_runner" not in source
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
        lambda _packet, **_kwargs: {
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

    def exchange(packet: dict[str, Any], **_kwargs: Any) -> dict[str, Any]:
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

    def exchange(packet: dict[str, Any], **_kwargs: Any) -> dict[str, Any]:
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
        lambda _packet, **_kwargs: {
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
    monkeypatch.setattr(client, "_exchange", lambda _packet, **_kwargs: response)
    with pytest.raises(CellAdmissionError) as invalid:
        client.get()
    assert invalid.value.code == "execution_command_response_invalid"
    assert invalid.value.outcome_unknown is True


def _attention_projection(generation: int = 4) -> dict[str, Any]:
    return {
        "active_configuration": {
            "graph_revision": "graph.1",
            "retained_bundle_id": "bundle.1",
        },
        "cell_id": "cell.test",
        "generation": generation,
        "members": [
            {
                "activation_id": "activation.1",
                "condition_id": "condition.1",
                "record_revision": 1,
                "subject": {
                    "resource_ids": ["robot.joint_1"],
                    "subject_id": "robot",
                    "subject_kind": "component",
                },
            }
        ],
        "owner_epoch": "alarm.owner.1",
        "owner_host_boot_id": "boot.test",
        "owner_incarnation_us": 42,
        "reconciled": True,
        "schema_version": "manyforge.operator_alarm_motion_attention.v3",
    }


def test_operator_alarm_attention_is_published_on_the_cell_admission_port(
    monkeypatch: pytest.MonkeyPatch, tmp_path
) -> None:
    client = CellAdmissionClient(tmp_path)
    sent: list[dict[str, Any]] = []
    deadlines: list[float | None] = []
    socket_files: list[str | None] = []
    command = "cell_admission_publish_operator_alarm_attention"

    def exchange(packet: dict[str, Any], **kwargs: Any) -> dict[str, Any]:
        sent.append(packet)
        deadlines.append(kwargs.get("deadline"))
        socket_files.append(kwargs.get("socket_file"))
        return {
            "api_version": "manyforge.execution_command_response.v1",
            "command": command,
            "ok": True,
            "operator_alarm_attention": {
                "generation": 4,
                "owner_epoch": "alarm.owner.1",
            },
        }

    monkeypatch.setattr(client, "_exchange", exchange)
    projection = _attention_projection()
    deadline = time.monotonic() + 0.2
    assert client.publish_operator_alarm_attention(projection, deadline=deadline) == {
        "generation": 4,
        "owner_epoch": "alarm.owner.1",
    }
    # The overall publication deadline reaches the socket exchange, and the
    # heartbeat travels beside Stop on the independent control socket.
    assert deadlines == [deadline]
    assert socket_files == [cell_admission.EXECUTION_CONTROL_SOCKET_FILE]
    assert sent == [
        {
            "api_version": "manyforge.execution_command.v1",
            "command": command,
            "request": projection,
        }
    ]
    # An acceptance naming another fence is not this publication.
    monkeypatch.setattr(
        client,
        "_exchange",
        lambda _packet, **_kwargs: {
            **exchange(_packet),
            "operator_alarm_attention": {"generation": 3, "owner_epoch": "alarm.owner.1"},
        },
    )
    with pytest.raises(CellAdmissionError) as mismatched:
        client.publish_operator_alarm_attention(
            projection, deadline=time.monotonic() + 0.2
        )
    assert mismatched.value.code == "execution_command_response_invalid"


def test_publication_deadline_bounds_the_whole_exchange_not_each_step(
    tmp_path,
) -> None:
    """A peer that accepts but never answers ends the call at its deadline.

    The per-operation timeout (1 s here) would let connect, send and receive
    each take that long; the overall monotonic deadline caps them together.
    """
    run_root = tmp_path / "run"
    run_root.mkdir(mode=0o700)
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    listener.bind(str(run_root / cell_admission.EXECUTION_CONTROL_SOCKET_FILE))
    os.chmod(run_root / cell_admission.EXECUTION_CONTROL_SOCKET_FILE, 0o600)
    listener.listen(1)
    accepted: list[socket.socket] = []

    def accept() -> None:
        connection, _address = listener.accept()
        accepted.append(connection)
        connection.recv(cell_admission.MAXIMUM_PACKET_BYTES)  # never answers

    peer = threading.Thread(target=accept, daemon=True)
    peer.start()
    try:
        client = CellAdmissionClient(run_root, timeout_seconds=1.0)
        started = time.monotonic()
        with pytest.raises(CellAdmissionError) as failed:
            client.publish_operator_alarm_attention(
                _attention_projection(), deadline=started + 0.15
            )
        elapsed = time.monotonic() - started
        assert failed.value.code == "execution_owner_unavailable"
        assert failed.value.outcome_unknown
        assert 0.15 <= elapsed < 0.5
        # An already elapsed deadline sends nothing.
        with pytest.raises(CellAdmissionError) as elapsed_deadline:
            client.publish_operator_alarm_attention(
                _attention_projection(), deadline=time.monotonic()
            )
        assert "deadline elapsed" in str(elapsed_deadline.value)
        assert len(accepted) == 1
    finally:
        peer.join(timeout=5.0)
        for connection in accepted:
            connection.close()
        listener.close()


def test_transport_failure_logs_syscall_errno_and_peer_without_changing_error(
    tmp_path, monkeypatch, caplog
) -> None:
    from contextlib import contextmanager

    class RunRoot:
        @contextmanager
        def socket_address(self, _filename):
            yield "/private/execution-command.sock"

    class BrokenSocket:
        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return False

        def setsockopt(self, *_args):
            pass

        def settimeout(self, _timeout):
            pass

        def connect(self, _address):
            pass

        def getsockopt(self, *_args):
            return struct.pack("3i", 1234, os.geteuid(), os.getegid())

        def send(self, _packet):
            raise BrokenPipeError(errno.EPIPE, "private-path-or-user-text")

        def close(self):
            pass

    client = CellAdmissionClient(tmp_path)
    client._run_root = RunRoot()
    from manyforge_headless_api.execution import ExecutionCommandClient, ExecutionObservationSession
    from manyforge_headless_api.native import HeadlessApiError

    execution = ExecutionCommandClient(tmp_path)
    execution._run_root = RunRoot()
    monkeypatch.setattr(cell_admission.socket, "socket", lambda *_args: BrokenSocket())
    with pytest.raises(CellAdmissionError) as failure:
        client.get()
    assert failure.value.code == "execution_owner_unavailable"
    assert failure.value.outcome_unknown
    assert str(failure.value) == "native execution transport failed: [Errno 32] private-path-or-user-text"
    assert "syscall=send errno=32 peer_socket=execution-command.sock peer_pid=1234" in caplog.text
    assert "exception_type=BrokenPipeError" in caplog.text
    assert "private-path-or-user-text" not in caplog.text
    caplog.clear()
    with pytest.raises(HeadlessApiError) as execution_failure:
        execution.identity()
    assert execution_failure.value.code == "execution_owner_unavailable"
    assert str(execution_failure.value) == "native execution transport failed: [Errno 32] private-path-or-user-text"
    assert "syscall=send errno=32 peer_socket=execution-command.sock" in caplog.text
    assert "private-path-or-user-text" not in caplog.text
    caplog.clear()
    with pytest.raises(HeadlessApiError) as control_failure:
        execution._exchange_on({"command": "test"}, execution._control_socket_path)
    assert control_failure.value.code == "execution_owner_unavailable"
    assert "syscall=send errno=32 peer_socket=execution-control.sock" in caplog.text
    caplog.clear()
    with ExecutionObservationSession(RunRoot(), tmp_path / "execution-observation.sock") as session:
        with pytest.raises(HeadlessApiError) as observation_failure:
            session.next(None, threading.Event())
    assert observation_failure.value.code == "execution_owner_unavailable"
    assert "syscall=send errno=32 peer_socket=execution-observation.sock" in caplog.text
    assert "private-path-or-user-text" not in caplog.text


@pytest.mark.parametrize(
    ("producer", "expected_producer"),
    [("component_manager", "component_manager"), (None, "kernel")],
)
def test_cell_admission_error_retains_optional_producer(
    tmp_path, monkeypatch, producer, expected_producer
) -> None:
    client = CellAdmissionClient(tmp_path)
    error = {
        "code": "stale_generation",
        "details": {"reason": "expected_generation_stale"},
        "message": "stale admission generation",
    }
    if producer is not None:
        error["producer"] = producer
    monkeypatch.setattr(
        client,
        "_exchange",
        lambda _packet, **_kwargs: {
            "api_version": cell_admission.EXECUTION_RESPONSE_VERSION,
            "command": "cell_admission_get",
            "error": error,
            "ok": False,
        },
    )

    with pytest.raises(CellAdmissionError) as refused:
        client.get()

    assert refused.value.code == "stale_generation"
    assert refused.value.details == {"reason": "expected_generation_stale"}
    assert refused.value.producer == expected_producer
