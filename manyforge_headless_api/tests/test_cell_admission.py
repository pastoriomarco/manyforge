"""The Headless and runner admission client: gateway calls for the claim
commands, its own stop-ended exchange for the operator-alarm attention."""

from __future__ import annotations

import inspect
import os
import socket
import threading
from typing import Any

import pytest

from manyforge_headless_api import cell_admission
from manyforge_headless_api.cell_admission import (
    CellAdmissionClient,
    CellAdmissionError,
)
from manyforge_headless_api.tests.fake_gateway import FakeGateway, GatewayError


def test_client_has_no_headless_import_or_readiness_writer() -> None:
    source = inspect.getsource(cell_admission)
    assert "manyforge_program_runner" not in source
    assert "cell_admission_update_readiness" not in source
    assert not hasattr(CellAdmissionClient, "update_readiness")


def _client(tmp_path, answer) -> tuple[CellAdmissionClient, FakeGateway]:
    client = CellAdmissionClient(tmp_path)
    gateway = FakeGateway(answer)
    client._gateway = gateway  # noqa: SLF001
    return client, gateway


def _accepted(command: str, **result: Any) -> dict[str, Any]:
    return {"api_version": cell_admission.EXECUTION_RESPONSE_VERSION,
            "command": command, "ok": True, **result}


def _intent() -> dict[str, Any]:
    return {
        "claim_kind": "program_cycle",
        "expected_admission_generation": 7,
        "intervention_evidence": None,
        "owner_role": "program_runner",
        "owner_transaction_id": 1,
        "program_cycle_evidence": {
            "applied_program_revision": "sha256:" + "2" * 64,
            "draft_program_revision": "sha256:" + "2" * 64,
        },
        # The Run request's class 2 key, part 0 (run 1, attempt 0).
        "request_key": "80" + "00000000000001" + "0000000000000000",
        "scope": {"configurations": [], "resources": []},
    }


def _release() -> dict[str, Any]:
    return {
        "acquired_admission_generation": 7,
        "claim_generation": 3,
        "owner_role": "program_runner",
        "owner_transaction_id": 1,
        "request_key": "0" * 31 + "1",
        "settlement": {"settlement_kind": "explicit_reset", "evidence_digest": "sha256:" + "b" * 64},
    }


def test_claim_commands_are_one_gateway_call_each_and_return_their_result(tmp_path) -> None:
    intent, release = _intent(), _release()
    status = {"schema_version": "manyforge.program_cycle_status.v1"}
    query = {"expected_claim": None, "expected_release": None, "intent": intent}
    admission = {"admission_generation": 8, "authoring_allowed": True}
    claim = {"accepted_at_ns": 5, "acquired_admission_generation": 7,
             "claim_generation": 3, "intent": intent}
    receipt = {"admission_generation": 8, "held_claims": [], "matched_claim": None,
               "matched_release": None, "status": "unknown"}
    results = {
        "cell_admission_get": {"authoring_admission": admission},
        "cell_admission_acquire": {"claim": claim},
        "cell_admission_release": {"authoring_admission": admission},
        "cell_admission_recovery_receipt": {"recovery_receipt": receipt},
    }
    client, gateway = _client(
        tmp_path, lambda command: _accepted(command["command"], **results[command["command"]])
    )
    assert client.get() == admission
    assert client.acquire(intent) == claim
    assert client.acquire(intent, program_cycle_status=status) == claim
    assert client.release(release) == admission
    assert client.recovery_receipt(query) == receipt
    assert gateway.calls == [
        ("cell_admission_get", {}, None),
        ("cell_admission_acquire", {"request": intent}, None),
        # Only a Program Reset claim carries the runner's cycle status (OD-139).
        ("cell_admission_acquire", {"request": intent, "program_cycle_status": status}, None),
        ("cell_admission_release", {"request": release}, None),
        ("cell_admission_recovery_receipt", {"request": query}, None),
    ]


@pytest.mark.parametrize(
    ("producer", "expected_producer"),
    [("component_manager", "component_manager"), (None, "kernel")],
)
def test_a_refusal_is_its_typed_error_with_a_known_outcome(
    tmp_path, producer, expected_producer
) -> None:
    error = {
        "code": "stale_generation",
        "details": {"reason": "expected_generation_stale"},
        "message": "stale admission generation",
    }
    if producer is not None:
        error["producer"] = producer
    client, _gateway = _client(tmp_path, lambda command: {
        "api_version": cell_admission.EXECUTION_RESPONSE_VERSION,
        "command": command["command"], "error": error, "ok": False,
    })
    with pytest.raises(CellAdmissionError) as refused:
        client.get()
    assert refused.value.code == "stale_generation"
    assert str(refused.value) == "stale admission generation"
    assert refused.value.details == {"reason": "expected_generation_stale"}
    assert refused.value.producer == expected_producer
    assert refused.value.outcome_unknown is False


@pytest.mark.parametrize(
    ("code", "outcome_unknown"),
    [
        ("execution_owner_unavailable", True),
        ("execution_command_transport_failure", True),
        ("execution_command_response_invalid", True),
        ("execution_command_invalid", False),
        ("execution_command_oversized", False),
    ],
)
def test_a_core_failure_keeps_its_code_and_says_whether_it_was_sent(
    tmp_path, caplog, code, outcome_unknown
) -> None:
    def fail(_command: dict[str, Any]) -> dict[str, Any]:
        raise GatewayError(code, "the host link failed")

    client, _gateway = _client(tmp_path, fail)
    with pytest.raises(CellAdmissionError) as failed:
        client.acquire(_intent())
    assert failed.value.code == code
    assert str(failed.value) == "the host link failed"
    assert failed.value.outcome_unknown is outcome_unknown
    assert failed.value.producer == "headless_api"
    assert f"command=cell_admission_acquire code={code}" in caplog.text


def test_operator_alarm_attention_is_published_on_its_own_exchange(
    monkeypatch: pytest.MonkeyPatch, tmp_path
) -> None:
    client, gateway = _client(tmp_path, lambda _command: pytest.fail("gateway call"))
    sent: list[dict[str, Any]] = []
    stop_fds: list[Any] = []
    command = "cell_admission_publish_operator_alarm_attention"

    def exchange(packet: dict[str, Any], **kwargs: Any) -> dict[str, Any]:
        sent.append(packet)
        stop_fds.append(kwargs.get("stop_fd"))
        return _accepted(command, operator_alarm_attention={"generation": 4})

    monkeypatch.setattr(client, "_exchange", exchange)
    projection = _attention_projection()
    assert client.publish_operator_alarm_attention(projection, stop_fd=7) == {
        "generation": 4,
    }
    # The publisher's stop pipe reaches the socket exchange (no deadline,
    # OD-307), which uses the independent component socket.
    assert stop_fds == [7]
    assert cell_admission.EXECUTION_COMPONENT_SOCKET_FILE == "execution-component.sock"
    assert sent == [
        {
            "api_version": "manyforge.execution_command.v4",
            "command": command,
            "request": projection,
        }
    ]
    # An acceptance naming another fence is not this publication.
    monkeypatch.setattr(
        client,
        "_exchange",
        lambda _packet, **_kwargs: _accepted(command, operator_alarm_attention={"generation": 3}),
    )
    with pytest.raises(CellAdmissionError) as mismatched:
        client.publish_operator_alarm_attention(projection)
    assert mismatched.value.code == "execution_command_response_invalid"
    # Another envelope version is not this protocol's answer.
    monkeypatch.setattr(
        client,
        "_exchange",
        lambda _packet, **_kwargs: {
            **_accepted(command, operator_alarm_attention={"generation": 4}),
            "api_version": "manyforge.execution_command_response.v2",
        },
    )
    with pytest.raises(CellAdmissionError) as wrong_version:
        client.publish_operator_alarm_attention(projection)
    assert wrong_version.value.outcome_unknown is True
    assert gateway.calls == []


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
        "owner_host_boot_id": "boot.test",
        "reconciled": True,
        "schema_version": "manyforge.operator_alarm_motion_attention.v4",
    }


@pytest.mark.parametrize(
    "payload",
    [
        b'{"api_version":"manyforge.execution_command_response.v5",'
        b'"api_version":"manyforge.execution_command_response.v5"}',
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


def test_publication_waits_for_its_answer_until_stopped(tmp_path) -> None:
    """OD-307: a peer that accepts but never answers is waited for; only the
    caller's stop pipe (or the peer closing) ends the wait."""
    run_root = tmp_path / "run"
    run_root.mkdir(mode=0o700)
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    listener.bind(str(run_root / cell_admission.EXECUTION_COMPONENT_SOCKET_FILE))
    os.chmod(run_root / cell_admission.EXECUTION_COMPONENT_SOCKET_FILE, 0o600)
    listener.listen(2)
    received, done = threading.Event(), threading.Event()

    def serve() -> None:
        connection, _address = listener.accept()
        with connection:
            connection.recv(cell_admission.MAXIMUM_PACKET_BYTES)
            received.set()
            done.wait()  # never answers

    peer = threading.Thread(target=serve)
    peer.start()
    stop_read, stop_write = os.pipe2(os.O_CLOEXEC | os.O_NONBLOCK)
    failures: list[CellAdmissionError] = []

    def publish() -> None:
        try:
            CellAdmissionClient(run_root, timeout_seconds=1.0).publish_operator_alarm_attention(
                _attention_projection(), stop_fd=stop_read
            )
        except CellAdmissionError as error:
            failures.append(error)

    publisher = threading.Thread(target=publish)
    publisher.start()
    try:
        assert received.wait(timeout=5.0)
        assert publisher.is_alive() and not failures
        os.write(stop_write, b"\0")
        publisher.join(timeout=5.0)
        assert not publisher.is_alive()
        assert len(failures) == 1
        assert failures[0].code == "execution_owner_unavailable"
        assert failures[0].outcome_unknown
        assert "stopped" in str(failures[0])
        # An exchange started after the stop ends at once: the pipe stays
        # readable, so its wait for the answer returns immediately (the
        # listener's backlog takes the connection; nobody answers it).
        with pytest.raises(CellAdmissionError) as stopped:
            CellAdmissionClient(run_root, timeout_seconds=1.0).publish_operator_alarm_attention(
                _attention_projection(), stop_fd=stop_read
            )
        assert stopped.value.code == "execution_owner_unavailable"
        assert "stopped" in str(stopped.value)
    finally:
        done.set()
        peer.join(timeout=5.0)
        publisher.join()
        listener.close()
        os.close(stop_read)
        os.close(stop_write)


def _host_that_exits_before_answering(run_root, socket_file: str):
    """A peer that takes the request and closes the connection unanswered,
    as an execution host process that exits does."""

    listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    listener.bind(str(run_root / socket_file))
    os.chmod(run_root / socket_file, 0o600)
    listener.listen(1)

    def serve() -> None:
        connection, _address = listener.accept()
        with connection:
            connection.recv(cell_admission.MAXIMUM_PACKET_BYTES)

    peer = threading.Thread(target=serve, daemon=True)
    peer.start()
    return listener, peer


def test_publication_ends_when_the_host_closes_the_connection(tmp_path) -> None:
    run_root = tmp_path / "run"
    run_root.mkdir(mode=0o700)
    listener, peer = _host_that_exits_before_answering(
        run_root, cell_admission.EXECUTION_COMPONENT_SOCKET_FILE
    )
    stop_read, stop_write = os.pipe2(os.O_CLOEXEC | os.O_NONBLOCK)
    try:
        # The stop pipe is never written: the peer's close ends the wait.
        with pytest.raises(CellAdmissionError) as lost:
            CellAdmissionClient(run_root, timeout_seconds=1.0).publish_operator_alarm_attention(
                _attention_projection(), stop_fd=stop_read
            )
        peer.join(timeout=5.0)
    finally:
        listener.close()
        os.close(stop_read)
        os.close(stop_write)
    assert lost.value.code == "execution_command_response_invalid"
    assert lost.value.outcome_unknown
