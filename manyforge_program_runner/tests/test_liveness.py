from __future__ import annotations

from pathlib import Path
import socket
import time

import pytest

from manyforge_program_runner.liveness import (
    API_VERSION,
    POLICY_ID,
    POLICY_REVISION,
    ProgramRunnerLivenessError,
    ProgramRunnerLivenessReceiver,
)


def _cycle_status(**changes):
    return {
        "cycle_generation": 7,
        "state": "idle_after_reset",
        "program_run_id": None,
        "cycle_claim_generation": None,
        "terminal_or_settlement_reference": None,
        "execution_control_revision": 1,
        "causal_request_key": None,
        "current_command_kind": None,
        "runner_commands": {"in_progress": None, "ended": []},
        "failure": None,
        "reset_phase": None,
        "reset_options": None,
        "reset_robot_position": {"available": False, "reason": "unsupported"},
        "accepted_reset": None,
        "allowed_commands": [],
        **changes,
    }


def _receiver(tmp_path: Path):
    return ProgramRunnerLivenessReceiver(
        tmp_path / "runner.sock",
        cell_id="cell",
        receiver_runtime_unit_id="program-runner.instance",
        receiver_runtime_epoch="runner-epoch",
        cycle_status=_cycle_status,
    )


def _heartbeat(*, sequence: int = 1):
    return {
        "active_bundle_id": "sha256:" + "a" * 64,
        "api_version": API_VERSION,
        "cell_id": "cell",
        "heartbeat_sequence": sequence,
        "kind": "heartbeat",
        "manager_runtime_unit_id": "component-manager.cell",
        "receiver_runtime_epoch": "runner-epoch",
        "receiver_runtime_unit_id": "program-runner.instance",
        "reconciliation_digest": "sha256:" + "b" * 64,
        "required_runtime_set_digest": "sha256:" + "c" * 64,
        "runtime_policy_id": POLICY_ID,
        "runtime_policy_revision": POLICY_REVISION,
    }


def test_starts_unreconciled_and_accepts_only_fresh_exact_heartbeat(tmp_path: Path):
    receiver = _receiver(tmp_path)

    assert receiver.status()["state"] == "unreconciled"
    with pytest.raises(ProgramRunnerLivenessError):
        receiver.require_current()

    acknowledgement = receiver._handle(_heartbeat())  # noqa: SLF001

    assert acknowledgement["state"] == "current"
    assert acknowledgement["receiver_status_sequence"] == 2
    assert acknowledgement["program_cycle_status"]["state"] == "idle_after_reset"
    receiver.require_current()
    with pytest.raises(ProgramRunnerLivenessError):
        receiver._handle(_heartbeat(sequence=0))  # noqa: SLF001


def test_rejects_malformed_heartbeat(tmp_path: Path):
    receiver = _receiver(tmp_path)
    packet = {**_heartbeat(), "manager_runtime_unit_id": ""}

    with pytest.raises(ProgramRunnerLivenessError):
        receiver._handle(packet)  # noqa: SLF001


@pytest.mark.parametrize("initially_current", [False, True])
def test_heartbeat_without_readiness_renews_only_liveness(
    tmp_path: Path, initially_current: bool
):
    receiver = _receiver(tmp_path)
    if initially_current:
        receiver._handle(_heartbeat())
    for sequence in range(2, 6):
        result = receiver._handle(
            {**_heartbeat(sequence=sequence), "reconciliation_digest": None}
        )
        assert result["state"] == ("current" if initially_current else "unreconciled")
        assert result["command_fenced"] is not initially_current
    receiver._handle(_heartbeat(sequence=6))
    receiver.require_current()


def test_the_one_managers_heartbeats_form_one_series(tmp_path: Path):
    # One Manager per application start: no Manager epoch is carried.
    receiver = _receiver(tmp_path)
    receiver._handle(_heartbeat(sequence=9))  # noqa: SLF001
    receiver.require_current()
    assert "manager_runtime_epoch" not in receiver.status()

    # Manager epochs are gone from the heartbeat (1.16.4 rule 1).
    for removed in ("manager_owner_epoch", "manager_runtime_epoch"):
        with pytest.raises(ProgramRunnerLivenessError, match="non-closed shape"):
            receiver._handle(  # noqa: SLF001
                {**_heartbeat(sequence=10), removed: "manager-1"}
            )


def test_listener_is_mode_0600_and_removes_only_owned_socket(tmp_path: Path):
    receiver = _receiver(tmp_path)
    receiver.start()
    path = tmp_path / "runner.sock"
    try:
        assert path.stat().st_mode & 0o777 == 0o600
    finally:
        receiver.close()
    assert not path.exists()


def test_listener_answers_a_probe(tmp_path: Path):
    path = tmp_path / "runner.sock"
    receiver = _receiver(tmp_path)
    receiver.start()
    try:
        connection = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        connection.settimeout(0.5)
        try:
            connection.connect(str(path))
            connection.send(
                receiver._encode(  # noqa: SLF001
                    {"api_version": API_VERSION, "cell_id": "cell", "kind": "probe"}
                )
            )
            acknowledgement = receiver._decode(connection.recv(4096))  # noqa: SLF001
            assert acknowledgement["kind"] == "probe_ack"
            assert acknowledgement["receiver_runtime_epoch"] == "runner-epoch"
            assert acknowledgement["program_cycle_status"]["cycle_generation"] == 7
        finally:
            connection.close()
    finally:
        receiver.close()


def test_listener_waits_for_a_request_without_a_time_limit_and_close_ends_it(
    tmp_path: Path,
):
    # OD-296 (a): a request that comes later than the former 0.2 s limit is
    # still answered, and close() ends a wait for a request that never comes.
    path = tmp_path / "runner.sock"
    receiver = _receiver(tmp_path)
    receiver.start()
    idle = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as connection:
            connection.settimeout(5.0)
            connection.connect(str(path))
            time.sleep(0.4)
            connection.send(
                receiver._encode(  # noqa: SLF001
                    {"api_version": API_VERSION, "cell_id": "cell", "kind": "probe"}
                )
            )
            acknowledgement = receiver._decode(connection.recv(4096))  # noqa: SLF001
            assert acknowledgement["kind"] == "probe_ack"
        idle.connect(str(path))
        time.sleep(0.1)
    finally:
        receiver.close()
        idle.close()


def test_listener_pins_run_root_and_cleans_its_original_socket(
    tmp_path: Path,
):
    run_root = tmp_path / "run"
    run_root.mkdir(mode=0o700)
    receiver = _receiver(run_root)
    receiver.start()
    retained_root = tmp_path / "retained-run"
    run_root.rename(retained_root)
    run_root.mkdir(mode=0o700)

    receiver.close()

    assert not (retained_root / "runner.sock").exists()
    assert not (run_root / "runner.sock").exists()


def test_start_rejects_unsafe_or_noncanonical_run_root(tmp_path: Path):
    unsafe = tmp_path / "unsafe"
    unsafe.mkdir(mode=0o700)
    unsafe.chmod(0o720)
    receiver = _receiver(unsafe)
    with pytest.raises(ProgramRunnerLivenessError, match="unsafe"):
        receiver.start()

    real = tmp_path / "real"
    real.mkdir(mode=0o700)
    alias = tmp_path / "alias"
    alias.symlink_to(real, target_is_directory=True)
    receiver = _receiver(alias)
    with pytest.raises(ProgramRunnerLivenessError, match="unsafe"):
        receiver.start()


def test_noncanonical_and_open_packets_are_rejected(tmp_path: Path):
    receiver = _receiver(tmp_path)
    with pytest.raises(ProgramRunnerLivenessError):
        receiver._decode(b'{ "api_version":"x"}')  # noqa: SLF001
    with pytest.raises(ProgramRunnerLivenessError):
        receiver._handle({**_heartbeat(), "unexpected": True})  # noqa: SLF001


def test_probe_fails_closed_when_owner_cannot_issue_current_cycle_status(
    tmp_path: Path,
):
    receiver = _receiver(tmp_path)
    receiver._cycle_status = lambda: (_ for _ in ()).throw(  # noqa: SLF001
        ProgramRunnerLivenessError("stale owner")
    )

    with pytest.raises(ProgramRunnerLivenessError, match="stale owner"):
        receiver._handle(  # noqa: SLF001
            {"api_version": API_VERSION, "cell_id": "cell", "kind": "probe"}
        )


def test_probe_rejects_open_or_malformed_cycle_projection(tmp_path: Path):
    receiver = _receiver(tmp_path)
    receiver._cycle_status = lambda: {  # type: ignore[method-assign]  # noqa: SLF001
        **_cycle_status(),
        "foreign": True,
    }
    with pytest.raises(ProgramRunnerLivenessError, match="non-closed"):
        receiver._handle(  # noqa: SLF001
            {"api_version": API_VERSION, "cell_id": "cell", "kind": "probe"}
        )

    receiver._cycle_status = lambda: _cycle_status(  # type: ignore[method-assign]  # noqa: SLF001
        state="running",
    )
    with pytest.raises(ProgramRunnerLivenessError, match="run identity"):
        receiver._handle(  # noqa: SLF001
            {"api_version": API_VERSION, "cell_id": "cell", "kind": "probe"}
        )


@pytest.mark.parametrize("state", ["starting", "resetting"])
def test_probe_reports_pending_cycle_without_a_claim(
    tmp_path: Path,
    state: str,
):
    receiver = _receiver(tmp_path)
    receiver._cycle_status = lambda: _cycle_status(  # type: ignore[method-assign]  # noqa: SLF001
        state=state,
        program_run_id=7,
        reset_phase="stopping" if state == "resetting" else None,
    )

    response = receiver._handle(  # noqa: SLF001
        {"api_version": API_VERSION, "cell_id": "cell", "kind": "probe"}
    )

    projected = response["program_cycle_status"]
    assert projected["state"] == state
    assert projected["cycle_claim_generation"] is None
    assert set(projected) == {
        "cycle_generation",
        "state",
        "program_run_id",
        "cycle_claim_generation",
        "terminal_or_settlement_reference",
        "accepted_reset",
    }


def test_probe_reports_starting_window_before_the_run_number(tmp_path: Path):
    # 1.15.5: a start announces its run number only after the Kernel claim.
    receiver = _receiver(tmp_path)
    receiver._cycle_status = lambda: _cycle_status(state="starting")  # type: ignore[method-assign]  # noqa: SLF001

    projected = receiver._project_cycle_status()  # noqa: SLF001

    assert projected["state"] == "starting"
    assert projected["program_run_id"] is None
    assert "program_runner_owner_epoch" not in projected


@pytest.mark.parametrize("packet", [b"{}", b"x" * 16_385])
def test_listener_drops_malformed_and_oversized_packets(tmp_path: Path, packet):
    receiver = _receiver(tmp_path)
    receiver.start()
    connection = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    connection.settimeout(0.2)
    try:
        connection.connect(str(tmp_path / "runner.sock"))
        connection.send(packet)
        with pytest.raises((TimeoutError, ConnectionError, OSError)):
            acknowledgement = connection.recv(1024)
            if not acknowledgement:
                raise ConnectionError("strict receiver closed without an ack")
    finally:
        connection.close()
        receiver.close()


@pytest.mark.parametrize("claim_generation", [None, 11])
def test_private_heartbeat_reports_selected_idle_reset_without_fictitious_run(tmp_path, claim_generation):
    receiver = _receiver(tmp_path)
    context = {"request_key": "0" * 31 + "1",
               "request_payload_digest": "sha256:" + "a" * 64,
               "reload_initial_scene": True, "reset_robot_position": False,
               "reset_claim_generation": claim_generation}
    receiver._cycle_status = lambda: _cycle_status(state="resetting", accepted_reset=context,
        reset_options={"reload_initial_scene": True, "reset_robot_position": False},
        reset_phase="restoring_scene", current_command_kind="program_reset")
    projected = receiver._project_cycle_status()
    assert projected["program_run_id"] is None and projected["cycle_claim_generation"] is None
    assert projected["accepted_reset"] == context
    receiver._cycle_status = lambda: _cycle_status(accepted_reset=context)
    with pytest.raises(ProgramRunnerLivenessError, match="no current Reset"):
        receiver._project_cycle_status()
