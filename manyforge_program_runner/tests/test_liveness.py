from __future__ import annotations

from pathlib import Path
import hashlib
import socket

import pytest

from manyforge_program_runner.liveness import (
    API_VERSION,
    EXPIRY_TIMEOUT_NS,
    POLICY_ID,
    POLICY_REVISION,
    ProgramRunnerLivenessError,
    ProgramRunnerLivenessReceiver,
    RetainedLiveness,
)
from manyforge_program_runner.cycle_journal import ProgramCycleJournal


class _Clock:
    now = 10

    def __call__(self) -> int:
        return self.now


def _receiver(tmp_path: Path):
    clock = _Clock()
    losses: list[str] = []
    reconciliations: list[tuple[str, str, str]] = []
    receiver = ProgramRunnerLivenessReceiver(
        tmp_path / "runner.sock",
        cell_id="cell",
        receiver_runtime_unit_id="program-runner.instance",
        receiver_runtime_epoch="runner-epoch",
        retained=RetainedLiveness(None, None, False, None),
        on_expiry=lambda reference: losses.append(reference) or reference,
        on_reconciliation=lambda digest, runtime, owner: reconciliations.append(
            (digest, runtime, owner)
        ),
        cycle_status=lambda: {
            "program_runner_owner_epoch": "runner-owner",
            "cycle_generation": 7,
            "state": "idle_after_reset",
            "program_run_id": None,
            "cycle_claim_generation": None,
            "terminal_or_settlement_reference": None,
        },
        monotonic_ns=clock,
    )
    return receiver, clock, losses, reconciliations


def _heartbeat(*, sequence: int = 1, manager: str = "manager-1"):
    return {
        "active_bundle_id": "sha256:" + "a" * 64,
        "api_version": API_VERSION,
        "cell_id": "cell",
        "heartbeat_sequence": sequence,
        "kind": "heartbeat",
        "manager_owner_epoch": "owner-" + manager,
        "manager_runtime_epoch": manager,
        "manager_runtime_unit_id": "component-manager.cell",
        "receiver_runtime_epoch": "runner-epoch",
        "receiver_runtime_unit_id": "program-runner.instance",
        "reconciliation_digest": "sha256:" + "b" * 64,
        "required_runtime_set_digest": "sha256:" + "c" * 64,
        "runtime_policy_id": POLICY_ID,
        "runtime_policy_revision": POLICY_REVISION,
    }


def _expected_status_digest(status) -> str:
    material = bytearray(b"manyforge.manager-liveness.status.v1|")
    for value in (
        status["cell_id"],
        status["receiver_runtime_unit_id"],
        status["receiver_runtime_epoch"],
        status["manager_runtime_unit_id"],
        status["manager_runtime_epoch"],
        status["manager_owner_epoch"],
        status["runtime_policy_id"],
        status["runtime_policy_revision"],
        status["last_accepted_heartbeat_sequence"],
        status["receiver_status_sequence"],
        status["local_heartbeat_age_ns"],
        status["state"],
        "true" if status["command_fenced"] else "false",
        status["fault_reference"] or "",
    ):
        encoded = str(value).encode()
        material.extend(str(len(encoded)).encode() + b":" + encoded + b"|")
    return "sha256:" + hashlib.sha256(material).hexdigest()


def test_starts_unreconciled_and_accepts_only_fresh_exact_heartbeat(tmp_path: Path):
    receiver, _clock, _losses, reconciliations = _receiver(tmp_path)

    assert receiver.status()["state"] == "unreconciled"
    with pytest.raises(ProgramRunnerLivenessError):
        receiver.require_current()

    acknowledgement = receiver._handle(_heartbeat())  # noqa: SLF001

    assert acknowledgement["state"] == "current"
    assert acknowledgement["last_accepted_heartbeat_sequence"] == 1
    assert acknowledgement["receiver_status_sequence"] == 2
    assert acknowledgement["manager_runtime_unit_id"] == "component-manager.cell"
    assert acknowledgement["receiver_status_digest"] == _expected_status_digest(
        acknowledgement
    )
    assert acknowledgement["program_cycle_status"]["state"] == "idle_after_reset"
    receiver.require_current()
    assert len(reconciliations) == 1
    with pytest.raises(ProgramRunnerLivenessError):
        receiver._handle(_heartbeat())  # noqa: SLF001
    with pytest.raises(ProgramRunnerLivenessError):
        receiver._handle(_heartbeat(sequence=0))  # noqa: SLF001


@pytest.mark.parametrize(
    "change",
    [
        {"cell_id": "foreign"},
        {"receiver_runtime_epoch": "retired"},
        {"runtime_policy_id": "foreign-policy"},
        {"runtime_policy_revision": "2"},
        {"manager_runtime_unit_id": ""},
        {"reconciliation_digest": None},
    ],
)
def test_rejects_foreign_or_unreconciled_heartbeat(tmp_path: Path, change):
    receiver, _clock, _losses, _reconciliations = _receiver(tmp_path)
    packet = {**_heartbeat(), **change}

    with pytest.raises(ProgramRunnerLivenessError):
        receiver._handle(packet)  # noqa: SLF001


def test_fake_clock_expiry_fences_and_allows_new_manager_only_after_loss(
    tmp_path: Path,
):
    receiver, clock, losses, _reconciliations = _receiver(tmp_path)
    receiver._handle(_heartbeat())  # noqa: SLF001
    with pytest.raises(ProgramRunnerLivenessError):
        receiver._handle(_heartbeat(manager="manager-2"))  # noqa: SLF001

    clock.now += EXPIRY_TIMEOUT_NS + 1
    assert receiver.check_expiry()
    assert receiver.status()["state"] == "expired"
    assert receiver.status()["command_fenced"] is True
    assert len(losses) == 1

    receiver._handle(_heartbeat(manager="manager-2"))  # noqa: SLF001
    assert receiver.status()["manager_runtime_epoch"] == "manager-2"
    assert receiver.status()["state"] == "current"


def test_listener_is_mode_0600_and_removes_only_owned_socket(tmp_path: Path):
    receiver, _clock, _losses, _reconciliations = _receiver(tmp_path)
    receiver.start()
    path = tmp_path / "runner.sock"
    try:
        assert path.stat().st_mode & 0o777 == 0o600
    finally:
        receiver.close()
    assert not path.exists()


def test_start_refuses_to_replace_active_same_user_listener(tmp_path: Path):
    path = tmp_path / "runner.sock"
    active = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    active.bind(str(path))
    path.chmod(0o600)
    active.listen(1)
    receiver, _clock, _losses, _reconciliations = _receiver(tmp_path)
    try:
        with pytest.raises(
            ProgramRunnerLivenessError, match="active listener"
        ):
            receiver.start()
        assert path.exists()
    finally:
        active.close()


def test_start_recovers_same_user_stale_socket_inode(tmp_path: Path):
    path = tmp_path / "runner.sock"
    crashed = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    crashed.bind(str(path))
    path.chmod(0o600)
    crashed.close()

    receiver, _clock, _losses, _reconciliations = _receiver(tmp_path)
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
            assert (
                acknowledgement["program_cycle_status"]["cycle_generation"] == 7
            )
        finally:
            connection.close()
    finally:
        receiver.close()


def test_listener_pins_run_root_and_cleans_its_original_socket(
    tmp_path: Path,
):
    run_root = tmp_path / "run"
    run_root.mkdir(mode=0o700)
    receiver, _clock, _losses, _reconciliations = _receiver(run_root)
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
    receiver, _clock, _losses, _reconciliations = _receiver(unsafe)
    with pytest.raises(ProgramRunnerLivenessError, match="unsafe"):
        receiver.start()

    real = tmp_path / "real"
    real.mkdir(mode=0o700)
    alias = tmp_path / "alias"
    alias.symlink_to(real, target_is_directory=True)
    receiver, _clock, _losses, _reconciliations = _receiver(alias)
    with pytest.raises(ProgramRunnerLivenessError, match="unsafe"):
        receiver.start()


def test_noncanonical_and_open_packets_are_rejected(tmp_path: Path):
    receiver, _clock, _losses, _reconciliations = _receiver(tmp_path)
    with pytest.raises(ProgramRunnerLivenessError):
        receiver._decode(b'{ "api_version":"x"}')  # noqa: SLF001
    with pytest.raises(ProgramRunnerLivenessError):
        receiver._handle({**_heartbeat(), "unexpected": True})  # noqa: SLF001


def test_probe_fails_closed_when_owner_cannot_issue_current_cycle_status(
    tmp_path: Path,
):
    receiver, _clock, _losses, _reconciliations = _receiver(tmp_path)
    receiver._cycle_status = lambda: (_ for _ in ()).throw(  # noqa: SLF001
        ProgramRunnerLivenessError("stale owner")
    )

    with pytest.raises(ProgramRunnerLivenessError, match="stale owner"):
        receiver._handle(  # noqa: SLF001
            {"api_version": API_VERSION, "cell_id": "cell", "kind": "probe"}
        )


def test_probe_rejects_open_or_malformed_cycle_projection(tmp_path: Path):
    receiver, _clock, _losses, _reconciliations = _receiver(tmp_path)
    receiver._cycle_status = lambda: {  # type: ignore[method-assign]  # noqa: SLF001
        "program_runner_owner_epoch": "runner-owner",
        "cycle_generation": 7,
        "state": "idle_after_reset",
        "program_run_id": None,
        "cycle_claim_generation": None,
        "terminal_or_settlement_reference": None,
        "foreign": True,
    }
    with pytest.raises(ProgramRunnerLivenessError, match="non-closed"):
        receiver._handle(  # noqa: SLF001
            {"api_version": API_VERSION, "cell_id": "cell", "kind": "probe"}
        )

    receiver._cycle_status = lambda: {  # type: ignore[method-assign]  # noqa: SLF001
        "program_runner_owner_epoch": "runner-owner",
        "cycle_generation": 7,
        "state": "running",
        "program_run_id": None,
        "cycle_claim_generation": None,
        "terminal_or_settlement_reference": None,
    }
    with pytest.raises(ProgramRunnerLivenessError, match="run identity"):
        receiver._handle(  # noqa: SLF001
            {"api_version": API_VERSION, "cell_id": "cell", "kind": "probe"}
        )


def test_probe_reports_pending_acquire_as_starting_without_a_claim(tmp_path: Path):
    receiver, _clock, _losses, _reconciliations = _receiver(tmp_path)
    receiver._cycle_status = lambda: {  # type: ignore[method-assign]  # noqa: SLF001
        "program_runner_owner_epoch": "runner-owner",
        "cycle_generation": 7,
        "state": "starting",
        "program_run_id": "program-cycle.7",
        "cycle_claim_generation": None,
        "terminal_or_settlement_reference": None,
    }

    response = receiver._handle(  # noqa: SLF001
        {"api_version": API_VERSION, "cell_id": "cell", "kind": "probe"}
    )

    assert response["program_cycle_status"]["state"] == "starting"
    assert response["program_cycle_status"]["cycle_claim_generation"] is None


@pytest.mark.parametrize("packet", [b"{}", b"x" * 16_385])
def test_listener_drops_malformed_and_oversized_packets(tmp_path: Path, packet):
    receiver, _clock, _losses, _reconciliations = _receiver(tmp_path)
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


def test_journal_retains_only_manager_loss_identity_and_evidence(tmp_path: Path):
    path = tmp_path / "cycle.sqlite3"
    journal = ProgramCycleJournal(path, owner_epoch="runner")
    fault = "sha256:" + "d" * 64
    journal.retain_liveness_manager(
        manager_runtime_epoch="manager-runtime",
        manager_owner_epoch="manager-owner",
    )
    journal.retain_liveness_loss(
        manager_runtime_epoch="manager-runtime",
        manager_owner_epoch="manager-owner",
        fault_reference=fault,
        expected_generation=1,
    )
    journal.close()

    reopened = ProgramCycleJournal(path, owner_epoch="runner")
    retained = reopened.liveness()
    assert retained == RetainedLiveness(
        "manager-runtime", "manager-owner", True, fault
    )
    assert reopened.read().reconciliation_required is True
    reopened.close()
