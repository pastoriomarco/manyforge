"""Private Manager-presence receiver for the separately hosted Program Runner."""

from __future__ import annotations

from collections.abc import Callable, Mapping
from dataclasses import dataclass
import errno
import hashlib
import json
import os
from pathlib import Path
import select
import socket
import stat
import struct
from threading import Event, RLock, Thread
import time
from typing import Any, Final


API_VERSION: Final = "manyforge.program_runner_liveness.v1"
POLICY_ID: Final = "manyforge.program-runner.manager-presence.v1"
POLICY_REVISION: Final = "1"
LOCAL_SAFE_STATE_CONTRACT_ID: Final = (
    "manyforge.program-runner.manager-loss-containment.v1"
)
LOCAL_SAFE_STATE_CONTRACT_REVISION: Final = "1"
RENEWAL_INTERVAL_NS: Final = 250_000_000
EXPIRY_TIMEOUT_NS: Final = 1_000_000_000
MAXIMUM_PACKET_BYTES: Final = 16_384
MAXIMUM_SEQUENCE: Final = 9_007_199_254_740_991


class ProgramRunnerLivenessError(RuntimeError):
    """A malformed, foreign, or stale private liveness packet."""


@dataclass(frozen=True, slots=True)
class RetainedLiveness:
    manager_runtime_epoch: str | None
    manager_owner_epoch: str | None
    expired: bool
    fault_reference: str | None


class ProgramRunnerLivenessReceiver:
    """Mode-0600 seqpacket listener plus receiver-local monotonic watchdog."""

    def __init__(
        self,
        path: Path,
        *,
        cell_id: str,
        receiver_runtime_unit_id: str,
        receiver_runtime_epoch: str,
        retained: RetainedLiveness,
        on_expiry: Callable[[str], str],
        on_reconciliation: Callable[[str, str, str], None],
        cycle_status: Callable[[], Mapping[str, Any]],
        monotonic_ns: Callable[[], int] = time.monotonic_ns,
    ) -> None:
        if not path.is_absolute():
            raise ValueError("Program Runner liveness socket must be absolute")
        self._path = path
        self._cell_id = cell_id
        self._runtime_unit_id = receiver_runtime_unit_id
        self._runtime_epoch = receiver_runtime_epoch
        self._on_expiry = on_expiry
        self._on_reconciliation = on_reconciliation
        self._cycle_status = cycle_status
        self._clock = monotonic_ns
        self._started_ns = self._clock()
        self._lock = RLock()
        self._stop = Event()
        self._listener: socket.socket | None = None
        self._listener_identity: tuple[int, int] | None = None
        self._server_thread: Thread | None = None
        self._watchdog_thread: Thread | None = None
        self._manager_runtime_epoch = retained.manager_runtime_epoch
        self._manager_owner_epoch = retained.manager_owner_epoch
        self._manager_runtime_unit_id: str | None = None
        self._last_sequence: int | None = None
        self._last_accepted_ns: int | None = None
        self._receiver_status_sequence = 1
        self._state = "unreconciled"
        self._fenced = True
        self._fault_reference = retained.fault_reference
        self._predecessor_expired = retained.expired

    def start(self) -> None:
        self._path.parent.mkdir(parents=True, exist_ok=True)
        try:
            existing = self._path.lstat()
        except FileNotFoundError:
            pass
        else:
            if (
                not stat.S_ISSOCK(existing.st_mode)
                or existing.st_uid != os.geteuid()
            ):
                raise ProgramRunnerLivenessError(
                    "liveness path exists and is not an owned socket"
                )
            identity = (existing.st_dev, existing.st_ino)
            probe = socket.socket(
                socket.AF_UNIX, socket.SOCK_SEQPACKET | socket.SOCK_CLOEXEC
            )
            probe.settimeout(0.2)
            try:
                probe.connect(str(self._path))
            except OSError as error:
                if error.errno != errno.ECONNREFUSED:
                    raise ProgramRunnerLivenessError(
                        "liveness socket state is ambiguous"
                    ) from error
            else:
                raise ProgramRunnerLivenessError(
                    "liveness socket already has an active listener"
                )
            finally:
                probe.close()
            try:
                current = self._path.lstat()
            except FileNotFoundError as error:
                raise ProgramRunnerLivenessError(
                    "liveness socket changed during stale-path recovery"
                ) from error
            if (
                (current.st_dev, current.st_ino) != identity
                or not stat.S_ISSOCK(current.st_mode)
                or current.st_uid != os.geteuid()
            ):
                raise ProgramRunnerLivenessError(
                    "liveness socket changed during stale-path recovery"
                )
            self._path.unlink()
        listener = socket.socket(
            socket.AF_UNIX, socket.SOCK_SEQPACKET | socket.SOCK_CLOEXEC
        )
        bound_identity: tuple[int, int] | None = None
        try:
            listener.bind(str(self._path))
            bound = self._path.lstat()
            bound_identity = (bound.st_dev, bound.st_ino)
            os.chmod(self._path, 0o600)
            current = self._path.lstat()
            if (current.st_dev, current.st_ino) != bound_identity:
                raise ProgramRunnerLivenessError(
                    "liveness socket changed while binding"
                )
            self._listener_identity = bound_identity
            listener.listen(8)
            listener.setblocking(False)
        except BaseException:
            listener.close()
            try:
                current = self._path.lstat()
                if (
                    bound_identity is not None
                    and (current.st_dev, current.st_ino) == bound_identity
                    and stat.S_ISSOCK(current.st_mode)
                ):
                    self._path.unlink()
            except FileNotFoundError:
                pass
            raise
        # Reserve the unique listener identity before retrying retained
        # containment, but do not start the serving thread (and therefore
        # cannot accept reconciliation) until that retry has completed.
        if self._predecessor_expired:
            reference = self._fault_reference
            if reference is None:
                material = (
                    "manyforge.program-runner.manager-loss.v1\0"
                    + (self._manager_runtime_epoch or "unknown")
                    + "\0"
                    + self._runtime_epoch
                )
                reference = (
                    "sha256:" + hashlib.sha256(material.encode()).hexdigest()
                )
            try:
                reference = self._on_expiry(reference)
            except Exception:
                pass
            with self._lock:
                self._fault_reference = reference
        self._listener = listener
        self._server_thread = Thread(
            target=self._serve, name="program-runner-liveness", daemon=True
        )
        self._watchdog_thread = Thread(
            target=self._watchdog, name="program-runner-watchdog", daemon=True
        )
        self._server_thread.start()
        self._watchdog_thread.start()

    def close(self) -> None:
        self._stop.set()
        listener = self._listener
        if listener is not None:
            listener.close()
        for thread in (self._server_thread, self._watchdog_thread):
            if thread is not None:
                thread.join(timeout=2.0)
        try:
            current = self._path.lstat()
            identity = (current.st_dev, current.st_ino)
            if identity == self._listener_identity and stat.S_ISSOCK(current.st_mode):
                self._path.unlink()
        except FileNotFoundError:
            pass

    def require_current(self) -> None:
        with self._lock:
            if self._state != "current" or self._fenced:
                raise ProgramRunnerLivenessError(
                    "Program Runner is fenced by Manager-presence liveness"
                )

    def status(self) -> dict[str, Any]:
        now = self._clock()
        with self._lock:
            age = (
                None
                if self._last_accepted_ns is None
                else max(0, now - self._last_accepted_ns)
            )
            return {
                "schema_version": "manyforge.program_runner_liveness_status.v1",
                "cell_id": self._cell_id,
                "receiver_runtime_unit_id": self._runtime_unit_id,
                "receiver_runtime_epoch": self._runtime_epoch,
                "manager_runtime_unit_id": self._manager_runtime_unit_id,
                "manager_runtime_epoch": self._manager_runtime_epoch,
                "manager_owner_epoch": self._manager_owner_epoch,
                "last_accepted_heartbeat_sequence": self._last_sequence,
                "receiver_status_sequence": self._receiver_status_sequence,
                "local_heartbeat_age_ns": age,
                "state": self._state,
                "command_fenced": self._fenced,
                "fault_reference": self._fault_reference,
                "runtime_policy_id": POLICY_ID,
                "runtime_policy_revision": POLICY_REVISION,
                "local_safe_state_contract_id": LOCAL_SAFE_STATE_CONTRACT_ID,
                "local_safe_state_contract_revision": (
                    LOCAL_SAFE_STATE_CONTRACT_REVISION
                ),
            }

    def _serve(self) -> None:
        while not self._stop.is_set():
            listener = self._listener
            if listener is None:
                return
            try:
                readable, _, _ = select.select([listener], [], [], 0.05)
                if not readable:
                    continue
                connection, _ = listener.accept()
            except (OSError, ValueError):
                if self._stop.is_set():
                    return
                continue
            with connection:
                connection.settimeout(0.2)
                try:
                    credentials = connection.getsockopt(
                        socket.SOL_SOCKET, socket.SO_PEERCRED, 12
                    )
                    _peer_pid, peer_uid, _peer_gid = struct.unpack(
                        "3i", credentials
                    )
                    if peer_uid != os.geteuid():
                        raise ProgramRunnerLivenessError("foreign peer uid")
                    packet, _ancillary, flags, _address = connection.recvmsg(
                        MAXIMUM_PACKET_BYTES + 1
                    )
                    if (
                        not packet
                        or len(packet) > MAXIMUM_PACKET_BYTES
                        or flags & socket.MSG_TRUNC
                    ):
                        raise ProgramRunnerLivenessError(
                            "empty or oversized liveness packet"
                        )
                    request = self._decode(packet)
                    response = self._handle(request)
                    encoded = self._encode(response)
                    if connection.send(encoded) != len(encoded):
                        raise ProgramRunnerLivenessError(
                            "liveness acknowledgement was truncated"
                        )
                except Exception:
                    # Strict rejection is intentionally response-free.
                    continue

    @staticmethod
    def _decode(packet: bytes) -> Mapping[str, Any]:
        try:
            text = packet.decode("utf-8")
            value = json.loads(text)
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise ProgramRunnerLivenessError("packet is not strict JSON") from error
        if not isinstance(value, dict) or ProgramRunnerLivenessReceiver._encode(
            value
        ) != packet:
            raise ProgramRunnerLivenessError("packet is not canonical JSON")
        return value

    @staticmethod
    def _encode(value: Mapping[str, Any]) -> bytes:
        return json.dumps(
            dict(value), ensure_ascii=False, sort_keys=True, separators=(",", ":")
        ).encode("utf-8")

    @staticmethod
    def _exact(value: Mapping[str, Any], keys: set[str]) -> None:
        if set(value) != keys:
            raise ProgramRunnerLivenessError("packet has a non-closed shape")

    def _project_cycle_status(self) -> dict[str, Any]:
        status = dict(self._cycle_status())
        self._exact(
            status,
            {
                "program_runner_owner_epoch",
                "cycle_generation",
                "state",
                "program_run_id",
                "cycle_claim_generation",
                "terminal_or_settlement_reference",
            },
        )
        owner = status["program_runner_owner_epoch"]
        generation = status["cycle_generation"]
        state = status["state"]
        run_id = status["program_run_id"]
        claim_generation = status["cycle_claim_generation"]
        terminal = status["terminal_or_settlement_reference"]
        if (
            not isinstance(owner, str)
            or not owner
            or len(owner.encode("utf-8")) > 256
            or type(generation) is not int
            or not 1 <= generation <= MAXIMUM_SEQUENCE
            or state
            not in {
                "idle_after_reset",
                "starting",
                "running",
                "stopping",
                "terminal_awaiting_reset",
            }
        ):
            raise ProgramRunnerLivenessError("malformed Program-cycle identity")
        idle = state == "idle_after_reset"
        if idle != (
            run_id is None and claim_generation is None and terminal is None
        ):
            raise ProgramRunnerLivenessError("malformed Program-cycle state shape")
        if not idle and (
            not isinstance(run_id, str)
            or not run_id
            or len(run_id.encode("utf-8")) > 256
            or type(claim_generation) is not int
            or not 1 <= claim_generation <= MAXIMUM_SEQUENCE
        ):
            raise ProgramRunnerLivenessError("malformed active Program cycle")
        if terminal is not None and (
            not isinstance(terminal, str)
            or len(terminal) != 71
            or not terminal.startswith("sha256:")
            or any(character not in "0123456789abcdef" for character in terminal[7:])
        ):
            raise ProgramRunnerLivenessError("malformed Program-cycle reference")
        if state == "terminal_awaiting_reset" and terminal is None:
            raise ProgramRunnerLivenessError("terminal Program cycle lacks evidence")
        return status

    @staticmethod
    def _status_digest(status: Mapping[str, Any]) -> str:
        material = bytearray(b"manyforge.manager-liveness.status.v1|")

        def append(value: str | int) -> None:
            encoded = str(value).encode("utf-8")
            material.extend(str(len(encoded)).encode("ascii"))
            material.extend(b":")
            material.extend(encoded)
            material.extend(b"|")

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
            append(value)
        return "sha256:" + hashlib.sha256(material).hexdigest()

    def _handle(self, request: Mapping[str, Any]) -> dict[str, Any]:
        if request.get("api_version") != API_VERSION:
            raise ProgramRunnerLivenessError("unsupported liveness API")
        kind = request.get("kind")
        if kind == "probe":
            self._exact(request, {"api_version", "cell_id", "kind"})
            if request["cell_id"] != self._cell_id:
                raise ProgramRunnerLivenessError("foreign cell")
            return {
                "api_version": API_VERSION,
                "cell_id": self._cell_id,
                "kind": "probe_ack",
                "program_cycle_status": self._project_cycle_status(),
                "receiver_runtime_epoch": self._runtime_epoch,
                "receiver_runtime_unit_id": self._runtime_unit_id,
                "runtime_policy_id": POLICY_ID,
                "runtime_policy_revision": POLICY_REVISION,
                "local_safe_state_contract_id": LOCAL_SAFE_STATE_CONTRACT_ID,
                "local_safe_state_contract_revision": (
                    LOCAL_SAFE_STATE_CONTRACT_REVISION
                ),
            }
        if kind != "heartbeat":
            raise ProgramRunnerLivenessError("unsupported liveness packet kind")
        self._exact(
            request,
            {
                "active_bundle_id",
                "api_version",
                "cell_id",
                "heartbeat_sequence",
                "kind",
                "manager_owner_epoch",
                "manager_runtime_epoch",
                "manager_runtime_unit_id",
                "receiver_runtime_epoch",
                "receiver_runtime_unit_id",
                "reconciliation_digest",
                "required_runtime_set_digest",
                "runtime_policy_id",
                "runtime_policy_revision",
            },
        )
        for key in (
            "active_bundle_id",
            "manager_owner_epoch",
            "manager_runtime_epoch",
            "manager_runtime_unit_id",
            "required_runtime_set_digest",
        ):
            if not isinstance(request[key], str) or not request[key]:
                raise ProgramRunnerLivenessError(f"invalid {key}")
        sequence = request["heartbeat_sequence"]
        if (
            type(sequence) is not int
            or not 1 <= sequence <= MAXIMUM_SEQUENCE
            or request["cell_id"] != self._cell_id
            or request["receiver_runtime_unit_id"] != self._runtime_unit_id
            or request["receiver_runtime_epoch"] != self._runtime_epoch
            or request["runtime_policy_id"] != POLICY_ID
            or request["runtime_policy_revision"] != POLICY_REVISION
        ):
            raise ProgramRunnerLivenessError("foreign heartbeat identity")
        reconciliation = request["reconciliation_digest"]
        if reconciliation is not None and (
            not isinstance(reconciliation, str)
            or not reconciliation.startswith("sha256:")
            or len(reconciliation) != 71
        ):
            raise ProgramRunnerLivenessError("invalid reconciliation digest")

        with self._lock:
            incoming_manager = request["manager_runtime_epoch"]
            needs_reconciliation = self._state != "current"
            if self._manager_runtime_epoch is not None:
                if incoming_manager != self._manager_runtime_epoch:
                    if not self._predecessor_expired:
                        raise ProgramRunnerLivenessError(
                            "predecessor Manager has not expired"
                        )
                    self._last_sequence = None
                    needs_reconciliation = True
                else:
                    if (
                        self._manager_owner_epoch is not None
                        and request["manager_owner_epoch"]
                        != self._manager_owner_epoch
                    ):
                        raise ProgramRunnerLivenessError(
                            "Manager owner epoch changed without runtime expiry"
                        )
                    if (
                        self._last_sequence is not None
                        and sequence <= self._last_sequence
                    ):
                        raise ProgramRunnerLivenessError(
                            "stale heartbeat sequence"
                        )
            if reconciliation is None:
                raise ProgramRunnerLivenessError(
                    "Runner renewal requires exact Manager reconciliation"
                )
            if self._receiver_status_sequence >= MAXIMUM_SEQUENCE:
                raise ProgramRunnerLivenessError(
                    "receiver status sequence is exhausted"
                )
            if needs_reconciliation:
                self._on_reconciliation(
                    reconciliation,
                    incoming_manager,
                    request["manager_owner_epoch"],
                )
            self._manager_runtime_epoch = incoming_manager
            self._manager_owner_epoch = request["manager_owner_epoch"]
            self._manager_runtime_unit_id = request["manager_runtime_unit_id"]
            self._last_sequence = sequence
            self._last_accepted_ns = self._clock()
            self._receiver_status_sequence += 1
            self._predecessor_expired = False
            self._state = "current"
            self._fenced = False
        status = self.status()
        return {
            **status,
            "active_bundle_id": request["active_bundle_id"],
            "api_version": API_VERSION,
            "kind": "heartbeat_ack",
            "program_cycle_status": self._project_cycle_status(),
            "required_runtime_set_digest": request[
                "required_runtime_set_digest"
            ],
            "receiver_status_digest": self._status_digest(status),
        }

    def _watchdog(self) -> None:
        while not self._stop.wait(RENEWAL_INTERVAL_NS / 1_000_000_000 / 4):
            self.check_expiry()

    def check_expiry(self) -> bool:
        """Perform one deterministic watchdog check (also used by tests)."""
        with self._lock:
            accepted = self._last_accepted_ns
            if (
                self._state == "expired"
                or (
                    accepted is None
                    and (
                        self._manager_runtime_epoch is None
                        or self._predecessor_expired
                        or self._clock() - self._started_ns
                        <= EXPIRY_TIMEOUT_NS
                    )
                )
                or (
                    accepted is not None
                    and (
                        self._state != "current"
                        or self._clock() - accepted <= EXPIRY_TIMEOUT_NS
                    )
                )
            ):
                return False
            manager_epoch = self._manager_runtime_epoch or "unknown"
            self._state = "expired"
            self._fenced = True
            self._predecessor_expired = True
            if self._receiver_status_sequence < MAXIMUM_SEQUENCE:
                self._receiver_status_sequence += 1
        material = (
            "manyforge.program-runner.manager-loss.v1\0"
            + manager_epoch
            + "\0"
            + self._runtime_epoch
        )
        fallback = "sha256:" + hashlib.sha256(material.encode()).hexdigest()
        try:
            reference = self._on_expiry(fallback)
        except Exception:
            reference = fallback
        with self._lock:
            self._fault_reference = reference
        return True


__all__ = [
    "API_VERSION",
    "EXPIRY_TIMEOUT_NS",
    "LOCAL_SAFE_STATE_CONTRACT_ID",
    "LOCAL_SAFE_STATE_CONTRACT_REVISION",
    "POLICY_ID",
    "POLICY_REVISION",
    "ProgramRunnerLivenessError",
    "ProgramRunnerLivenessReceiver",
    "RENEWAL_INTERVAL_NS",
    "RetainedLiveness",
]
