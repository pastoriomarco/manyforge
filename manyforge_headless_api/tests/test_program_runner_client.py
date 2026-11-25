"""Headless's link to the isolated Program runner (the core's ProgramRunnerLink)."""

from __future__ import annotations

import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import textwrap
import threading
from typing import Any

import pytest

import manyforge_headless_py as core

from manyforge_headless_api.program_runner_client import (
    ProgramRunnerClient,
    ProgramRunnerClientError,
)

# The runner's commands whose reply is its row at acceptance (OD-214).
_COMMANDS = frozenset({
    "start_run",
    "start_motion",
    "resume_motion",
    "prepare_manual_handover",
    "resume_halt",
    "write_runtime_blackboard",
    "stage_manual_result",
    "apply_recovery_action",
    "reset_cycle",
    "set_persistent_value",
    "reset_persistent_value",
    "clear_persistent_values",
})


def _row(request_key: str, kind: str = "start") -> dict:
    return {
        "kind": kind,
        "request_key": request_key,
        "step": "admitting",
        "accepted_at_ns": "1763112345678901234",
        "end": None,
    }


def _command_reply(key: str) -> dict:
    return {"schema_version": "manyforge.program_command_reply.v1", "command": _row(key)}


class _Link:
    """Records each call and answers like the runner."""

    def __init__(self, answer=None) -> None:
        self.calls: list[tuple[str, dict[str, Any]]] = []
        self.limits: dict[str, float | None] = {}
        self._answer = answer

    def call(
        self, command: str, request: dict[str, Any], limit_s: float | None = None
    ) -> dict[str, Any]:
        self.calls.append((command, request))
        self.limits[command] = limit_s
        if self._answer is not None:
            return self._answer(command, request)
        if command in _COMMANDS:
            body = request.get("body", request)
            return {"ok": True, "value": _command_reply(body["request_key"])}
        return {"ok": True, "value": {"ok": True}}


def test_program_runner_client_names_commands_and_carries_requests() -> None:
    link = _Link()
    client = ProgramRunnerClient("/unused", link=link)
    assert client.runs(active_only=True, limit=7) == {"ok": True}
    assert client.start_run({"request_key": "1" * 32}) == _command_reply("1" * 32)
    assert client.read_run(1) == {"ok": True}
    assert client.start_motion(1, {"request_key": "2" * 32}) == _command_reply("2" * 32)
    assert client.prepare_manual_handover(1, {"request_key": "3" * 32}) == (
        _command_reply("3" * 32)
    )
    assert client.cycle_status(before_command=True) == {"ok": True}
    assert client.health() == {"ok": True}
    assert link.calls == [
        ("runs", {"active_only": True, "limit": 7}),
        ("start_run", {"request_key": "1" * 32}),
        ("read_run", {"program_run_id": 1}),
        ("start_motion", {"program_run_id": 1, "body": {"request_key": "2" * 32}}),
        (
            "prepare_manual_handover",
            {"program_run_id": 1, "body": {"request_key": "3" * 32}},
        ),
        ("cycle_status", {}),
        ("health", {}),
    ]


def test_reads_keep_the_whole_limit_and_commands_wait_for_their_answer() -> None:
    """As the former HTTP client: every read ends within the timeout; a
    command and the cycle status read before a command are not limited."""

    link = _Link()
    client = ProgramRunnerClient("/unused", timeout_seconds=7.0, link=link)
    client.runs()
    client.node_availability({})
    client.diagnostic_health()
    client.health()
    client.prepare_applied_scene({})
    client.read_run(1)
    client.preview_node_correction(1, {})
    client.cycle_status()
    client.runtime_controls(1)
    client.runtime_blackboard_values(1)
    client.persistent_values()
    client.reconcile_persistent_values()
    reads = dict(link.limits)
    assert set(reads.values()) == {7.0}
    assert len(reads) == 12
    link.limits.clear()
    client.cycle_status(before_command=True)
    client.start_run({"request_key": "1" * 32})
    client.start_motion(1, {"request_key": "2" * 32})
    client.resume_motion(1, {"request_key": "3" * 32})
    client.prepare_manual_handover(1, {"request_key": "4" * 32})
    client.resume_halt(1, {"request_key": "5" * 32})
    client.reset_cycle({"request_key": "6" * 32})
    client.runtime_control_action(1, "manual-result", {"request_key": "7" * 32})
    client.apply_recovery_action({"request_key": "8" * 32})
    client.persistent_value_action("set", {"request_key": "9" * 32})
    assert set(link.limits.values()) == {None}
    assert len(link.limits) == 10


def test_program_runner_client_preserves_stable_remote_errors() -> None:
    def refuse(_command, _request):
        return {
            "ok": False,
            "error": {
                "code": "program_run_already_active",
                "details": {"active_run": 1},
                "message": "one active Program run is already present",
                "producer": "kernel",
            },
        }

    client = ProgramRunnerClient("/unused", link=_Link(refuse))
    with pytest.raises(ProgramRunnerClientError) as captured:
        client.start_run({"request_key": "1" * 32})

    assert captured.value.code == "program_run_already_active"
    assert str(captured.value) == "one active Program run is already present"
    assert captured.value.details == {"active_run": 1}
    assert captured.value.producer == "kernel"


def test_program_runner_client_addresses_applied_persistent_values() -> None:
    link = _Link()
    client = ProgramRunnerClient("/unused", link=link)
    assert client.persistent_values() == {"ok": True}
    for action in ("set", "reset", "clear"):
        key = {"set": "1", "reset": "2", "clear": "3"}[action] * 32
        assert client.persistent_value_action(action, {"request_key": key})[
            "command"
        ]["request_key"] == key
    with pytest.raises(ValueError, match="unsupported"):
        client.persistent_value_action("delete", {})
    assert [command for command, _request in link.calls] == [
        "read_persistent_values",
        "set_persistent_value",
        "reset_persistent_value",
        "clear_persistent_values",
    ]
    assert link.calls[2][1] == {"request_key": "2" * 32}


def test_program_runner_client_names_run_actions() -> None:
    link = _Link()
    client = ProgramRunnerClient("/unused", link=link)
    key = {"request_key": "4" * 32}
    client.runtime_control_action(1, "blackboard-values", key)
    client.runtime_control_action(1, "manual-result", key)
    client.runtime_controls(2)
    client.runtime_blackboard_values(3)
    with pytest.raises(ValueError, match="unsupported"):
        client.runtime_control_action(1, "runtime-overrides", key)
    with pytest.raises(ValueError, match="program_run_id"):
        client.read_run(0)
    assert link.calls == [
        ("write_runtime_blackboard", {"program_run_id": 1, "body": key}),
        ("stage_manual_result", {"program_run_id": 1, "body": key}),
        ("read_runtime_controls", {"program_run_id": 2}),
        ("read_runtime_blackboard", {"program_run_id": 3}),
    ]


@pytest.mark.parametrize(
    "value",
    [
        {"ok": True},
        {"schema_version": "manyforge.program_command_reply.v1"},
        {"schema_version": "manyforge.program_command_reply.v1", "command": {"kind": "start"}},
        {
            "schema_version": "manyforge.program_command_reply.v1",
            "command": {**_row("5" * 32), "message": "text"},
        },
    ],
)
def test_program_runner_client_refuses_an_invalid_command_reply(value: dict) -> None:
    client = ProgramRunnerClient(
        "/unused", link=_Link(lambda _command, _request: {"ok": True, "value": value})
    )
    with pytest.raises(ProgramRunnerClientError) as captured:
        client.reset_cycle({"request_key": "5" * 32})
    assert captured.value.code == "program_runner_response_invalid"


def test_program_runner_client_returns_a_replayed_ended_row_as_answered() -> None:
    ended = {
        **_row("6" * 32, "reset"),
        "step": "releasing",
        "end": {
            "outcome": "refused",
            "code": "program_run_request_conflict",
            "details": {"program_run_id": 3},
            "finished_at_ns": "1763112345678909999",
        },
    }
    body = {"schema_version": "manyforge.program_command_reply.v1", "command": ended}
    client = ProgramRunnerClient(
        "/unused", link=_Link(lambda _command, _request: {"ok": True, "value": body})
    )
    assert client.reset_cycle({"request_key": "6" * 32}) == body


def test_program_runner_client_refuses_a_non_object_value() -> None:
    client = ProgramRunnerClient(
        "/unused", link=_Link(lambda _command, _request: {"ok": True, "value": []})
    )
    with pytest.raises(ProgramRunnerClientError) as captured:
        client.runs()
    assert captured.value.code == "program_runner_response_invalid"


# --- over the real sockets ---------------------------------------------------


@pytest.fixture
def socket_root(tmp_path: Path) -> Path:
    root = tmp_path / "sockets"
    root.mkdir(mode=0o700)
    return root


def test_a_missing_runner_is_unavailable_with_its_cause(socket_root: Path) -> None:
    client = ProgramRunnerClient(socket_root, timeout_seconds=1.0)
    with pytest.raises(ProgramRunnerClientError) as captured:
        client.cycle_status()
    assert captured.value.code == "program_runner_unavailable"
    assert captured.value.producer == "program_runner"
    assert captured.value.details == {"transport_error": "connect_failed"}
    with pytest.raises(ProgramRunnerClientError) as subscribed:
        client.subscribe_status()
    assert subscribed.value.code == "program_runner_unavailable"


def test_a_reply_waits_for_the_runner_and_keeps_its_values(socket_root: Path) -> None:
    """OD-214: a command's reply comes at acceptance; only the connect is
    limited, and the values cross unchanged (floats stay floats)."""

    server = core.ProgramCommandServer(str(socket_root))
    server.start()
    client = ProgramRunnerClient(socket_root, timeout_seconds=1.0)
    answers: list[Any] = []
    try:
        caller = threading.Thread(
            target=lambda: answers.append(
                client.node_availability({"scale": 1.0, "text": "café"})
            )
        )
        caller.start()
        token, request = server.next(10.0)
        assert request == {
            "command": "node_availability",
            "request": {"scale": 1.0, "text": "café"},
        }
        assert caller.is_alive()
        server.reply(token, {"ok": True, "value": {"scale": 1.0, "items": [1, 2.5]}})
        caller.join()
        assert answers == [{"scale": 1.0, "items": [1, 2.5]}]
        assert type(answers[0]["scale"]) is float
    finally:
        server.close()


def test_a_runner_that_ends_before_its_reply_is_unavailable(socket_root: Path) -> None:
    server = core.ProgramCommandServer(str(socket_root))
    server.start()
    client = ProgramRunnerClient(socket_root, timeout_seconds=1.0)
    failures: list[ProgramRunnerClientError] = []

    def call() -> None:
        try:
            client.cycle_status(before_command=True)
        except ProgramRunnerClientError as error:
            failures.append(error)

    caller = threading.Thread(target=call)
    caller.start()
    assert server.next(10.0) is not None
    server.close()
    caller.join()
    assert [failure.code for failure in failures] == ["program_runner_unavailable"]
    assert failures[0].details == {"transport_error": "closed_without_reply"}


def _raw_runner(root: Path, *packets: bytes) -> threading.Thread:
    """A runner that reads one request and answers with ``packets`` as sent,
    then closes."""

    listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    path = root / "program-command.sock"
    listener.bind(str(path))
    os.chmod(path, 0o600)
    listener.listen(1)

    def serve() -> None:
        with listener:
            connection, _ = listener.accept()
            with connection:
                # Every packet of a message but its last starts with NUL.
                while connection.recv(1 << 20).startswith(b"\0"):
                    pass
                for packet in packets:
                    connection.sendall(packet)

    thread = threading.Thread(target=serve)
    thread.start()
    return thread


@pytest.mark.parametrize(
    "reply",
    [
        b"[]",
        b"not json",
        b'{"api_version":"manyforge.program_command_response.v1","ok":true,"ok":false}',
        b'{"api_version":"manyforge.program_command_response.v0","ok":true,"value":{}}',
        b'{"api_version":"manyforge.program_command_response.v1","ok":true}',
        b'{"api_version":"manyforge.program_command_response.v1","ok":false,'
        b'"error":{"code":"x"}}',
        b'{"api_version":"manyforge.program_command_response.v1","ok":true,'
        b'"value":NaN}',
    ],
)
def test_a_malformed_reply_is_invalid(socket_root: Path, reply: bytes) -> None:
    runner = _raw_runner(socket_root, reply)
    client = ProgramRunnerClient(socket_root, timeout_seconds=1.0)
    with pytest.raises(ProgramRunnerClientError) as captured:
        client.runs()
    runner.join()
    assert captured.value.code == "program_runner_response_invalid"


def test_a_runner_that_ends_in_the_middle_of_its_reply_is_unavailable(
    socket_root: Path,
) -> None:
    """A reply cut short by the runner's end is the runner unavailable, as an
    HTTP response cut short was, not an invalid reply."""

    runner = _raw_runner(
        socket_root, b'\0{"api_version":"manyforge.program_command_response.v1",'
    )
    client = ProgramRunnerClient(socket_root, timeout_seconds=1.0)
    with pytest.raises(ProgramRunnerClientError) as captured:
        client.runs()
    runner.join()
    assert captured.value.code == "program_runner_unavailable"
    assert captured.value.details == {"transport_error": "closed_mid_reply"}


def test_a_reply_in_several_packets_arrives_whole(socket_root: Path) -> None:
    runner = _raw_runner(
        socket_root,
        b'\0{"api_version":"manyforge.program_command_response.v1",',
        b'\0"ok":true,',
        b'"value":{"n":1}}',
    )
    client = ProgramRunnerClient(socket_root, timeout_seconds=1.0)
    assert client.runs() == {"n": 1}
    runner.join()


def test_a_read_ends_at_its_limit_while_a_command_waits_on(socket_root: Path) -> None:
    """A read whose answer does not come within the timeout is the runner
    unavailable (the former HTTP read timeout); a command sent before it
    still waits for, and gets, its answer."""

    server = core.ProgramCommandServer(str(socket_root))
    server.start()
    client = ProgramRunnerClient(socket_root, timeout_seconds=0.2)
    answers: list[Any] = []
    try:
        caller = threading.Thread(
            target=lambda: answers.append(client.start_run({"request_key": "1" * 32}))
        )
        caller.start()
        token, request = server.next(10.0)
        assert request["command"] == "start_run"
        with pytest.raises(ProgramRunnerClientError) as captured:
            client.runs()
        assert captured.value.code == "program_runner_unavailable"
        assert captured.value.details == {"transport_error": "read_timeout"}
        # The command has now waited longer than the read limit.
        assert caller.is_alive()
        server.reply(token, {"ok": True, "value": _command_reply("1" * 32)})
        caller.join()
        assert answers == [_command_reply("1" * 32)]
    finally:
        server.close()


_GIL_PROBE = textwrap.dedent(
    """
    import sys, threading
    import manyforge_headless_py as core

    root = sys.argv[1]
    server = core.ProgramCommandServer(root)
    server.start()
    publisher = core.ProgramStatusPublisher(root)
    publisher.start()
    link = core.ProgramRunnerLink(root, 5.0)
    subscription = link.subscribe_status()
    results = []
    caller = threading.Thread(
        target=lambda: results.append(link.call("cycle_status", {})))
    reader = threading.Thread(target=lambda: results.append(subscription.next(60.0)))
    caller.start()
    reader.start()
    # Both threads now wait in the core: the runner withholds its reply and
    # nothing is published. This thread, and another one, still run Python.
    token, _request = server.next(30.0)
    counted = []
    other = threading.Thread(target=lambda: counted.append(sum(range(100000))))
    other.start()
    other.join()
    server.reply(token, {"ok": True, "value": {"counted": counted[0]}})
    publisher.publish({"n": 1}, True)
    caller.join()
    reader.join()
    subscription.close()
    publisher.close()
    server.close()
    reply = next(item for item in results if "ok" in item)
    frame = next(item for item in results if "ok" not in item)
    assert reply == {"ok": True, "value": {"counted": 4999950000}}, reply
    assert frame["program_cycle"] == {"n": 1}, frame
    print("released")
    """
)


def test_waiting_calls_and_status_reads_release_the_gil(socket_root: Path) -> None:
    """A call waiting for a withheld reply and a status read waiting for a
    frame let every other Python thread run. Run in its own process: a held
    GIL would block this test's own thread, so the limit is the process's."""

    completed = subprocess.run(
        [sys.executable, "-c", _GIL_PROBE, str(socket_root)],
        capture_output=True,
        text=True,
        timeout=120,
        env=os.environ,
    )
    assert completed.returncode == 0, completed.stderr
    assert completed.stdout.strip() == "released"


def test_status_subscription_delivers_frames_and_typed_loss(socket_root: Path) -> None:
    publisher = core.ProgramStatusPublisher(str(socket_root))
    publisher.start()
    client = ProgramRunnerClient(socket_root, timeout_seconds=1.0)
    try:
        publisher.publish({"b": 1.5, "a": "x"}, True)
        subscription = client.subscribe_status()
        frame = subscription.next(10.0)
        assert frame == {
            "event_class": "critical",
            "program_cycle": {"a": "x", "b": 1.5},
            "schema_version": "manyforge.program_control_state.v4",
            "server_sequence": "1",
        }
        assert subscription.next(0.0) is None
        publisher.close()
        with pytest.raises(ProgramRunnerClientError) as captured:
            subscription.next(10.0)
        assert captured.value.code == "program_runner_unavailable"
        subscription.close()
    finally:
        publisher.close()
    assert json.dumps(frame["program_cycle"], sort_keys=True) == '{"a": "x", "b": 1.5}'
