"""The Program runner's execution-host calls through the core's gateway:
the cell-admission client over a fake gateway, and the Kernel client over
the real core against a fake SEQPACKET host in a private run root."""

from __future__ import annotations

import json
import os
from pathlib import Path
import socket
import threading
import time
from typing import Any, Callable

import pytest
import rfc8785

core = pytest.importorskip("manyforge_headless_py")

from manyforge_behavior.kernel_client import ExecutionCommandError, KernelClient
from manyforge_behavior.orchestration_ports import ProgramSelectionReleaseRejectedError
from manyforge_program_runner.cell_admission import CellAdmission, CellAdmissionError

from .fake_gateway import RecordingGateway

_RESPONSE = "manyforge.execution_command_response.v6"
_INTERNAL = {"transport": "internal", "assurance": "unverified", "principal_id": None}


# --- Cell admission over the gateway ---------------------------------------


def _intent() -> dict[str, Any]:
    return {"claim_kind": "program_run", "request_key": "a" * 32, "scope": {"kind": "cell"}}


def _claim(intent: dict[str, Any]) -> dict[str, Any]:
    return {"accepted_at_ns": 5, "acquired_admission_generation": 2,
            "claim_generation": 3, "intent": intent}


def test_admission_sends_each_command_with_its_request_member() -> None:
    authoring = {"admission_generation": 1, "authoring_allowed": True}
    receipt = {"status": "held_exact", "held_claims": [], "admission_generation": 1,
               "matched_claim": None, "matched_release": None}
    gateway = RecordingGateway({
        "cell_admission_get": {"ok": True, "authoring_admission": authoring},
        "cell_admission_acquire": lambda members: {"ok": True,
                                                   "claim": _claim(members["request"])},
        "cell_admission_release": {"ok": True, "authoring_admission": authoring},
        "cell_admission_recovery_receipt": {"ok": True, "recovery_receipt": receipt},
    })
    admission = CellAdmission(gateway)
    status = {"state": "resetting"}
    assert admission.get() == authoring
    assert admission.acquire(_intent()) == _claim(_intent())
    assert admission.acquire(_intent(), program_cycle_status=status) == _claim(_intent())
    assert admission.release({"claim_generation": 3}) == authoring
    assert admission.recovery_receipt({"intent": _intent()}) == receipt
    assert gateway.sent == [
        ("cell_admission_get", {}),
        ("cell_admission_acquire", {"request": _intent()}),
        # Only a Program Reset claim carries the runner's cycle status (OD-139).
        ("cell_admission_acquire", {"request": _intent(), "program_cycle_status": status}),
        ("cell_admission_release", {"request": {"claim_generation": 3}}),
        ("cell_admission_recovery_receipt", {"request": {"intent": _intent()}}),
    ]


def test_admission_refusal_keeps_its_code_and_a_lost_reply_is_unknown() -> None:
    gateway = RecordingGateway({"cell_admission_get": [
        {"ok": False, "error": {"code": "cell_admission_held", "message": "held",
                                "details": {"reason": "x"}}},
        {"ok": False, "error": {"code": "cell_admission_held"}},
        core.GatewayError("execution_owner_unavailable", "lost"),
        core.GatewayError("execution_command_invalid", "not encodable"),
    ]})
    admission = CellAdmission(gateway)
    with pytest.raises(CellAdmissionError) as refused:
        admission.get()
    assert (refused.value.code, refused.value.details, refused.value.producer,
            refused.value.outcome_unknown) == ("cell_admission_held", {"reason": "x"}, "kernel", False)
    with pytest.raises(CellAdmissionError) as malformed:
        admission.get()
    assert (malformed.value.code, malformed.value.outcome_unknown) == (
        "execution_command_response_invalid", True)
    with pytest.raises(CellAdmissionError) as lost:
        admission.get()
    assert (lost.value.code, lost.value.outcome_unknown) == ("execution_owner_unavailable", True)
    with pytest.raises(CellAdmissionError) as unsent:
        admission.get()
    assert (unsent.value.code, unsent.value.outcome_unknown) == ("execution_command_invalid", False)


def test_a_claim_of_another_intent_is_not_this_acquisition() -> None:
    other = {**_intent(), "request_key": "b" * 32}
    gateway = RecordingGateway({"cell_admission_acquire": {"ok": True, "claim": _claim(other)}})
    with pytest.raises(CellAdmissionError) as mismatched:
        CellAdmission(gateway).acquire(_intent())
    assert (mismatched.value.code, mismatched.value.outcome_unknown) == (
        "execution_command_response_invalid", True)


# --- The real core against a fake SEQPACKET host ---------------------------


class _Host:
    """One fake execution host: the command and control sockets of a private
    run root (0700, sockets 0600), answering each request with ``answer``."""

    def __init__(self, root: Path, answer: Callable[[dict[str, Any]], dict[str, Any]],
                 leaves: tuple[str, ...] = ("execution-command.sock", "execution-control.sock"),
                 response_version: str = _RESPONSE) -> None:
        self.answer = answer
        self.response_version = response_version
        self.received: list[tuple[str, dict[str, Any]]] = []
        self._sockets: list[socket.socket] = []
        self._threads: list[threading.Thread] = []
        for leaf in leaves:
            listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            path = root / leaf
            listener.bind(str(path))
            os.chmod(path, 0o600)
            listener.listen(8)
            self._sockets.append(listener)
            thread = threading.Thread(target=self._serve, args=(listener, leaf), daemon=True)
            thread.start()
            self._threads.append(thread)

    def _serve(self, listener: socket.socket, leaf: str) -> None:
        while True:
            try:
                connection, _address = listener.accept()
            except OSError:
                return
            with connection:
                request = json.loads(connection.recv(4 * 1024 * 1024))
                self.received.append((leaf, request))
                connection.send(rfc8785.dumps(
                    {"api_version": self.response_version, **self.answer(request)}))

    def close(self) -> None:
        for listener in self._sockets:
            # Wakes the blocked accept (Linux), which ends the thread.
            listener.shutdown(socket.SHUT_RDWR)
            listener.close()
        for thread in self._threads:
            thread.join()


@pytest.fixture
def run_root(tmp_path: Path) -> Path:
    root = Path(os.path.realpath(tmp_path)) / "run"
    root.mkdir(mode=0o700)
    os.chmod(root, 0o700)
    return root


def test_release_halt_crosses_the_core_with_internal_attribution(run_root: Path) -> None:
    scope = {"hold_revision": 8, "program_run_id": 3}
    host = _Host(run_root, lambda request: {
        "command": request["command"], "ok": True, "scope": scope})
    try:
        client = KernelClient(core.Gateway(str(run_root), None, attribution="internal"))
        assert client.release_halt(3, 7) == scope
    finally:
        host.close()
    assert host.received == [("execution-command.sock", {
        "api_version": "manyforge.execution_command.v4",
        "attribution": _INTERNAL,
        "command": "release_halt",
        "expected_hold_revision": 7,
        "program_run_id": 3,
    })]


def test_core_refusals_and_selection_rejections_keep_their_codes(run_root: Path) -> None:
    replies = iter([
        {"ok": False, "error": {"code": "invalid_state_transition", "message": "stale",
                                "details": {"reason": "program_halt_release_stale"}}},
        {"ok": False, "error": {"code": "stop_rejected", "message": "no", "details": {},
                                "producer": "kernel"}},
    ])
    host = _Host(run_root, lambda request: {"command": request["command"], **next(replies)})
    try:
        client = KernelClient(core.Gateway(str(run_root), None, attribution="internal"))
        with pytest.raises(ProgramSelectionReleaseRejectedError) as rejected:
            client.release_halt(3, 7)
        assert rejected.value.code == "invalid_state_transition"
        assert rejected.value.details == {"reason": "program_halt_release_stale"}
        with pytest.raises(ExecutionCommandError) as refused:
            client.cell_stop({"request_context": {"request_key": "c" * 32,
                                                  "source_label": "program"}})
        assert (refused.value.code, refused.value.producer) == ("stop_rejected", "kernel")
    finally:
        host.close()
    # cell_stop goes to the control socket with the internal attribution.
    assert host.received[1] == ("execution-control.sock", {
        "api_version": "manyforge.execution_command.v4",
        "attribution": _INTERNAL,
        "command": "cell_stop",
        "request": {"request_context": {"request_key": "c" * 32, "source_label": "program"}},
    })


def test_a_missing_host_is_the_typed_transport_failure(run_root: Path) -> None:
    client = KernelClient(core.Gateway(str(run_root), None, attribution="internal",
                                       connect_limit_s=1.0))
    with pytest.raises(core.GatewayError) as lost:
        client.identity()
    assert lost.value.code == "execution_owner_unavailable"


def test_the_runners_scene_commit_and_lookup_carry_the_http_attribution(run_root: Path) -> None:
    """The Manager records the runner's scene commits as when they went
    through Headless's scene route: http, unverified, no principal."""

    from manyforge_behavior.production_ports import HeadlessSceneProgramPort

    manager = _Host(
        run_root,
        lambda request: (
            {"ok": True, "result": None}
            if request["command"] == "scene_transaction_status"
            else {"ok": False, "error": {"code": "invalid_request", "message": "no"}}
        ),
        leaves=("manager-command.sock",),
        response_version="manyforge.manager_command_response.v3",
    )
    transaction = {
        "request_context": {"request_key": "d" * 32, "source_label": "program"},
        "expected_projection_revision": {},
        "operation": {"kind": "attach_carried_object"},
        "program_node_evidence": {"program_run_id": 3},
        "expected_admission_generation": 2,
    }
    try:
        gateway = core.Gateway(None, str(run_root), attribution="internal")
        gateway.scene_commit(transaction)
        assert HeadlessSceneProgramPort(gateway)._receipt(transaction) is None
    finally:
        manager.close()
    http = {"assurance": "unverified", "principal_id": None, "transport": "http"}
    assert [(leaf, request["command"], request["transaction"]["attribution"])
            for leaf, request in manager.received] == [
        ("manager-command.sock", "scene_commit", http),
        ("manager-command.sock", "scene_transaction_status", http),
    ]


def test_the_scene_ports_manager_reads_end_at_the_runners_read_limit(run_root: Path) -> None:
    """A Manager read with no reply within the runner's read limit ends as
    the runner's timed-out Headless read did; other failures keep their code."""

    from manyforge_behavior.production_ports import HeadlessSceneProgramPort

    path = run_root / "manager-command.sock"
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    listener.bind(str(path))
    os.chmod(path, 0o600)
    listener.listen(1)
    release = threading.Event()

    def serve() -> None:
        connection, _address = listener.accept()
        with connection:
            connection.recv(4 * 1024 * 1024)
            release.wait(5.0)  # Takes the read and never answers.

    worker = threading.Thread(target=serve)
    worker.start()
    port = HeadlessSceneProgramPort(
        core.Gateway(None, str(run_root), attribution="internal"), read_limit_s=0.2
    )
    started = time.monotonic()
    try:
        with pytest.raises(core.GatewayError) as expired:
            port._read("scene_owner_read")
        elapsed = time.monotonic() - started
    finally:
        release.set()
        listener.close()
        worker.join(timeout=1.0)
    assert expired.value.code == "headless_http_timeout"
    assert 0.15 <= elapsed < 2.0
    with pytest.raises(core.GatewayError) as refused:
        port._read("scene_owner_read")  # Nothing listens any more.
    assert refused.value.code == "manager_command_unavailable"
