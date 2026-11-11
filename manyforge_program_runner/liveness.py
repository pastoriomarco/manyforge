"""Private Manager-presence receiver for the separately hosted Program Runner."""

from __future__ import annotations

from collections.abc import Callable, Mapping
from dataclasses import dataclass
import errno
import hashlib
import json
import logging
import os
from pathlib import Path
import select
import socket
import stat
import struct
from threading import Event, RLock, Thread
import time
from typing import Any, Final
import unicodedata

from manyforge_headless_api.program_cycle import AcceptedProgramResetDTO


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
PUBLISHER_FAULT_CODE: Final = "program_bbkey_publication_failed"
_MAXIMUM_PUBLISHER_FAULT_MESSAGE_BYTES: Final = 1_024


_LOGGER = logging.getLogger(__name__)


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
        publication_check: Callable[[], None] | None = None,
    ) -> None:
        if not path.is_absolute() or path.name in {"", ".", ".."}:
            raise ValueError("Program Runner liveness socket must be absolute")
        self._path = path
        self._cell_id = cell_id
        self._runtime_unit_id = receiver_runtime_unit_id
        self._runtime_epoch = receiver_runtime_epoch
        self._on_expiry = on_expiry
        self._on_reconciliation = on_reconciliation
        self._cycle_status = cycle_status
        self._clock = monotonic_ns
        # Starts pending BB key sources, run on the watchdog.
        self._publication_check = publication_check
        self._started_ns = self._clock()
        self._lock = RLock()
        self._stop = Event()
        self._listener: socket.socket | None = None
        self._listener_identity: tuple[int, int] | None = None
        self._run_root_fd: int | None = None
        self._run_root_identity: tuple[int, int] | None = None
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
        self._last_rejection: str | None = None
        # Independent of Manager presence: no heartbeat or reconciliation
        # clears it, and it keeps commands fenced while Manager stays current.
        self._publisher_fault: dict[str, Any] | None = None
        self._publisher_fault_reference: str | None = None

    @staticmethod
    def _safe_run_root(metadata: os.stat_result) -> bool:
        required = stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR
        return (
            stat.S_ISDIR(metadata.st_mode)
            and metadata.st_uid == os.geteuid()
            and not metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
            and metadata.st_mode & required == required
        )

    def _open_run_root(self) -> int:
        root = self._path.parent
        try:
            before = root.lstat()
            canonical = root.resolve(strict=True)
            descriptor = os.open(
                root,
                os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
            )
        except (OSError, RuntimeError) as error:
            raise ProgramRunnerLivenessError(
                "liveness run capability is unavailable or unsafe"
            ) from error
        try:
            opened = os.fstat(descriptor)
            after = root.lstat()
        except OSError as error:
            os.close(descriptor)
            raise ProgramRunnerLivenessError(
                "liveness run capability changed while opening"
            ) from error
        identity = (before.st_dev, before.st_ino)
        if canonical != root or any(
            not self._safe_run_root(metadata)
            or (metadata.st_dev, metadata.st_ino) != identity
            for metadata in (before, opened, after)
        ):
            os.close(descriptor)
            raise ProgramRunnerLivenessError(
                "liveness run capability type, owner, mode, or identity is unsafe"
            )
        return descriptor

    def _validate_run_root(self, descriptor: int) -> None:
        try:
            opened = os.fstat(descriptor)
            current = self._path.parent.lstat()
        except OSError as error:
            raise ProgramRunnerLivenessError(
                "liveness run capability was replaced"
            ) from error
        identity = self._run_root_identity
        if (
            identity is None
            or not self._safe_run_root(opened)
            or not self._safe_run_root(current)
            or (opened.st_dev, opened.st_ino) != identity
            or (current.st_dev, current.st_ino) != identity
        ):
            raise ProgramRunnerLivenessError("liveness run capability was replaced")

    def _endpoint_status(self, descriptor: int) -> os.stat_result:
        return os.stat(
            self._path.name,
            dir_fd=descriptor,
            follow_symlinks=False,
        )

    def _remove_endpoint(
        self,
        descriptor: int,
        identity: tuple[int, int] | None,
    ) -> None:
        if identity is None:
            return
        try:
            current = self._endpoint_status(descriptor)
        except FileNotFoundError:
            return
        if (
            (current.st_dev, current.st_ino) == identity
            and stat.S_ISSOCK(current.st_mode)
            and current.st_uid == os.geteuid()
        ):
            os.unlink(self._path.name, dir_fd=descriptor)

    def start(self) -> None:
        if (
            self._run_root_fd is not None
            or self._listener is not None
            or self._stop.is_set()
        ):
            raise ProgramRunnerLivenessError(
                "liveness receiver cannot be started more than once"
            )
        descriptor = self._open_run_root()
        self._run_root_fd = descriptor
        opened = os.fstat(descriptor)
        self._run_root_identity = (opened.st_dev, opened.st_ino)
        socket_address = f"/proc/self/fd/{descriptor}/{self._path.name}"
        if len(os.fsencode(socket_address)) >= 104:
            os.close(descriptor)
            self._run_root_fd = None
            self._run_root_identity = None
            raise ProgramRunnerLivenessError(
                "liveness socket path exceeds the AF_UNIX bound"
            )
        listener: socket.socket | None = None
        bound_identity: tuple[int, int] | None = None
        try:
            try:
                existing = self._endpoint_status(descriptor)
            except FileNotFoundError:
                existing = None
            if existing is not None:
                if (
                    not stat.S_ISSOCK(existing.st_mode)
                    or existing.st_uid != os.geteuid()
                    or existing.st_nlink != 1
                    or stat.S_IMODE(existing.st_mode) != 0o600
                ):
                    raise ProgramRunnerLivenessError(
                        "liveness path exists and is not the exact owned socket"
                    )
                identity = (existing.st_dev, existing.st_ino)
                probe = socket.socket(
                    socket.AF_UNIX,
                    socket.SOCK_SEQPACKET | socket.SOCK_CLOEXEC,
                )
                probe.settimeout(0.2)
                try:
                    probe.connect(socket_address)
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
                    current = self._endpoint_status(descriptor)
                except FileNotFoundError as error:
                    raise ProgramRunnerLivenessError(
                        "liveness socket changed during stale-path recovery"
                    ) from error
                if (
                    (current.st_dev, current.st_ino) != identity
                    or not stat.S_ISSOCK(current.st_mode)
                    or current.st_uid != os.geteuid()
                    or current.st_nlink != 1
                    or stat.S_IMODE(current.st_mode) != 0o600
                ):
                    raise ProgramRunnerLivenessError(
                        "liveness socket changed during stale-path recovery"
                    )
                os.unlink(self._path.name, dir_fd=descriptor)

            listener = socket.socket(
                socket.AF_UNIX,
                socket.SOCK_SEQPACKET | socket.SOCK_CLOEXEC,
            )
            listener.bind(socket_address)
            bound = self._endpoint_status(descriptor)
            bound_identity = (bound.st_dev, bound.st_ino)
            os.chmod(self._path.name, 0o600, dir_fd=descriptor)
            current = self._endpoint_status(descriptor)
            if (
                (current.st_dev, current.st_ino) != bound_identity
                or not stat.S_ISSOCK(current.st_mode)
                or current.st_uid != os.geteuid()
                or current.st_nlink != 1
                or stat.S_IMODE(current.st_mode) != 0o600
            ):
                raise ProgramRunnerLivenessError(
                    "liveness socket changed while binding"
                )
            self._validate_run_root(descriptor)
            self._listener_identity = bound_identity
            listener.listen(8)
            listener.setblocking(False)
            # Reserve the unique listener identity before retrying retained
            # containment, but do not accept reconciliation until that retry
            # has completed.
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
                target=self._serve,
                name="program-runner-liveness",
                daemon=True,
            )
            self._watchdog_thread = Thread(
                target=self._watchdog,
                name="program-runner-watchdog",
                daemon=True,
            )
            self._server_thread.start()
            self._watchdog_thread.start()
        except BaseException:
            self._stop.set()
            if listener is not None:
                listener.close()
            for thread in (self._server_thread, self._watchdog_thread):
                if thread is not None and thread.is_alive():
                    thread.join(timeout=2.0)
            self._remove_endpoint(descriptor, bound_identity)
            os.close(descriptor)
            self._run_root_fd = None
            self._run_root_identity = None
            self._listener = None
            raise

    def close(self) -> None:
        self._stop.set()
        listener = self._listener
        if listener is not None:
            listener.close()
        for thread in (self._server_thread, self._watchdog_thread):
            if thread is not None:
                thread.join(timeout=2.0)
        descriptor = self._run_root_fd
        if descriptor is not None:
            try:
                self._remove_endpoint(descriptor, self._listener_identity)
            finally:
                os.close(descriptor)
                self._run_root_fd = None
                self._run_root_identity = None
        self._listener = None

    def require_current(self) -> None:
        with self._lock:
            if self._publisher_fault is not None:
                raise ProgramRunnerLivenessError(
                    "Program Runner is fenced by a BB key publication fault"
                )
            if self._state != "current" or self._fenced:
                raise ProgramRunnerLivenessError(
                    "Program Runner is fenced by Manager-presence liveness"
                )

    @property
    def manager_fault_reference(self) -> str | None:
        """The Manager-loss reference only, never a publisher fault."""

        with self._lock:
            return self._fault_reference

    def latch_publisher_fault(
        self, fault: Mapping[str, Any], reference: str
    ) -> bool:
        """Latch the first BB publication fault for this receiver's lifetime,
        unless its owner clears it (`clear_publisher_fault`). True when
        this call latched it."""

        message = fault.get("message")
        if (
            set(fault) != {"code", "message", "details"}
            or fault.get("code") != PUBLISHER_FAULT_CODE
            or fault.get("details") != {}
            or not isinstance(message, str)
            or not message
            or len(message.encode("utf-8")) > _MAXIMUM_PUBLISHER_FAULT_MESSAGE_BYTES
            or unicodedata.normalize("NFC", message) != message
            or not isinstance(reference, str)
            or len(reference) != 71
            or not reference.startswith("sha256:")
        ):
            raise ProgramRunnerLivenessError("malformed BB publication fault")
        with self._lock:
            if self._publisher_fault is not None:
                return False
            self._publisher_fault = {
                "code": PUBLISHER_FAULT_CODE,
                "message": message,
                "details": {},
            }
            self._publisher_fault_reference = reference
            if self._receiver_status_sequence < MAXIMUM_SEQUENCE:
                self._receiver_status_sequence += 1
        return True

    def clear_publisher_fault(self, reference: str) -> bool:
        """Clear exactly the latched fault with this reference (a
        between-Runs source whose loss the Kernel confirmed, or that a newer
        source superseded). True when it was cleared."""

        with self._lock:
            if (
                self._publisher_fault is None
                or self._publisher_fault_reference != reference
            ):
                return False
            self._publisher_fault = None
            self._publisher_fault_reference = None
            if self._receiver_status_sequence < MAXIMUM_SEQUENCE:
                self._receiver_status_sequence += 1
        return True

    def check_publication(self) -> None:
        """Start the BB key sources still pending (rule catalog unknown or
        source not ready yet) at this liveness observation."""

        check = self._publication_check
        if check is None:
            return
        try:
            check()
        except Exception:
            _LOGGER.exception("pending BB key source start failed")

    def status(self) -> dict[str, Any]:
        now = self._clock()
        with self._lock:
            age = (
                None
                if self._last_accepted_ns is None
                else max(0, now - self._last_accepted_ns)
            )
            publisher_fault = (
                None if self._publisher_fault is None else dict(self._publisher_fault)
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
                # A publisher fault reuses the digested fence; its reference
                # is the digest of the emitted publisher ErrorDTO.
                # A Manager-loss reference stays in manager_fault_reference.
                "command_fenced": self._fenced or publisher_fault is not None,
                "fault_reference": (
                    self._fault_reference
                    if publisher_fault is None
                    else self._publisher_fault_reference
                ),
                "publisher_fault": publisher_fault,
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
                    _peer_pid, peer_uid, _peer_gid = struct.unpack("3i", credentials)
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
                    self._last_rejection = None
                except Exception as error:
                    # Strict rejection is intentionally response-free.
                    rejection = f"{type(error).__name__}: {error}"
                    if rejection != self._last_rejection:
                        _LOGGER.warning(
                            "Program Runner rejected Manager liveness: %s",
                            rejection,
                        )
                        self._last_rejection = rejection
                    continue

    @staticmethod
    def _decode(packet: bytes) -> Mapping[str, Any]:
        try:
            text = packet.decode("utf-8")
            value = json.loads(text)
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise ProgramRunnerLivenessError("packet is not strict JSON") from error
        if (
            not isinstance(value, dict)
            or ProgramRunnerLivenessReceiver._encode(value) != packet
        ):
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
                "allowed_commands",
                "accepted_reset",
                "causal_client_sequence",
                "causal_correlation_id",
                "causal_session_id",
                "program_runner_owner_epoch",
                "cycle_generation",
                "current_command_kind",
                "execution_control_revision",
                "failure",
                "state",
                "program_run_id",
                "cycle_claim_generation",
                "reset_phase",
                "reset_options",
                "reset_robot_position",
                "terminal_or_settlement_reference",
            },
        )
        projected = {
            key: status[key]
            for key in (
                "program_runner_owner_epoch",
                "cycle_generation",
                "state",
                "program_run_id",
                "cycle_claim_generation",
                "terminal_or_settlement_reference",
                "accepted_reset",
            )
        }
        owner = projected["program_runner_owner_epoch"]
        generation = projected["cycle_generation"]
        state = projected["state"]
        run_id = projected["program_run_id"]
        claim_generation = projected["cycle_claim_generation"]
        terminal = projected["terminal_or_settlement_reference"]
        accepted_reset = projected["accepted_reset"]
        if accepted_reset is not None:
            try:
                AcceptedProgramResetDTO.model_validate(accepted_reset)
            except ValueError as error:
                raise ProgramRunnerLivenessError("malformed accepted Reset context") from error
            if state not in {"resetting", "recovery_required"}:
                raise ProgramRunnerLivenessError("accepted Reset context has no current Reset")
        idle_reset = (
            state in {"resetting", "recovery_required"}
            and run_id is None and claim_generation is None and terminal is None
            and accepted_reset is not None
            and (accepted_reset["reload_initial_scene"] or accepted_reset["reset_robot_position"])
        )
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
                "resetting",
                "terminal_awaiting_reset",
                "recovery_required",
            }
        ):
            raise ProgramRunnerLivenessError("malformed Program-cycle identity")
        idle = state == "idle_after_reset"
        if idle and not (
            run_id is None and claim_generation is None and terminal is None
        ):
            raise ProgramRunnerLivenessError("malformed idle Program cycle")
        recovery = state == "recovery_required"
        if (
            not idle
            and not recovery
            and not idle_reset
            and (
                not isinstance(run_id, str)
                or not run_id
                or len(run_id.encode("utf-8")) > 256
            )
        ):
            raise ProgramRunnerLivenessError("malformed Program-cycle run identity")
        if claim_generation is not None and not isinstance(run_id, str):
            raise ProgramRunnerLivenessError("malformed Program-cycle run identity")
        claimless_terminal = state == "terminal_awaiting_reset" and terminal is not None
        if (
            not idle
            and claim_generation is None
            and state not in {"starting", "stopping", "resetting", "recovery_required"}
            and not claimless_terminal
        ):
            raise ProgramRunnerLivenessError("malformed unacquired Program cycle")
        if claim_generation is not None and (
            type(claim_generation) is not int
            or not 1 <= claim_generation <= MAXIMUM_SEQUENCE
        ):
            raise ProgramRunnerLivenessError("malformed Program-cycle claim")
        if terminal is not None and (
            not isinstance(terminal, str)
            or len(terminal) != 71
            or not terminal.startswith("sha256:")
            or any(character not in "0123456789abcdef" for character in terminal[7:])
        ):
            raise ProgramRunnerLivenessError("malformed Program-cycle reference")
        if state == "terminal_awaiting_reset" and terminal is None:
            raise ProgramRunnerLivenessError("terminal Program cycle lacks evidence")
        return projected

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

        # Lock order: the Runner's transition lock before this lock, never
        # the reverse. The reconciliation takes the transition lock, so it
        # runs with this lock released; a change meanwhile (only the
        # watchdog's expiry can make one) refuses this heartbeat.
        self._lock.acquire()
        try:
            incoming_manager = request["manager_runtime_epoch"]
            needs_reconciliation = self._state != "current"
            if self._manager_runtime_epoch is not None:
                if incoming_manager != self._manager_runtime_epoch:
                    if not self._predecessor_expired or (
                        self._last_accepted_ns is not None and self._state != "expired"
                    ):
                        raise ProgramRunnerLivenessError(
                            "predecessor Manager has not expired"
                        )
                    self._last_sequence = None
                    needs_reconciliation = True
                else:
                    if (
                        self._manager_owner_epoch is not None
                        and request["manager_owner_epoch"] != self._manager_owner_epoch
                    ):
                        raise ProgramRunnerLivenessError(
                            "Manager owner epoch changed without runtime expiry"
                        )
                    if (
                        self._last_sequence is not None
                        and sequence <= self._last_sequence
                    ):
                        raise ProgramRunnerLivenessError("stale heartbeat sequence")
            if self._receiver_status_sequence >= MAXIMUM_SEQUENCE:
                raise ProgramRunnerLivenessError(
                    "receiver status sequence is exhausted"
                )
            if needs_reconciliation and reconciliation is not None:
                decided = self._reconciliation_basis()
                self._lock.release()
                try:
                    self._on_reconciliation(
                        reconciliation,
                        incoming_manager,
                        request["manager_owner_epoch"],
                    )
                finally:
                    self._lock.acquire()
                if self._reconciliation_basis() != decided:
                    raise ProgramRunnerLivenessError(
                        "liveness changed during reconciliation"
                    )
            self._manager_runtime_epoch = incoming_manager
            self._manager_owner_epoch = request["manager_owner_epoch"]
            self._manager_runtime_unit_id = request["manager_runtime_unit_id"]
            self._last_sequence = sequence
            self._last_accepted_ns = self._clock()
            self._receiver_status_sequence += 1
            # Receipt proves peer liveness, not runtime readiness. During Apply
            # a healthy Manager may have no reconciled native projection yet.
            if reconciliation is not None:
                self._predecessor_expired = False
                self._state = "current"
                self._fenced = False
            elif needs_reconciliation:
                self._state = "unreconciled"
                self._fenced = True
        finally:
            self._lock.release()
        status = self.status()
        return {
            **status,
            "active_bundle_id": request["active_bundle_id"],
            "api_version": API_VERSION,
            "kind": "heartbeat_ack",
            "program_cycle_status": self._project_cycle_status(),
            "required_runtime_set_digest": request["required_runtime_set_digest"],
            "receiver_status_digest": self._status_digest(status),
        }

    def _reconciliation_basis(self) -> tuple[Any, ...]:
        """What a heartbeat decided on; only `check_expiry` changes it."""
        return (
            self._state,
            self._predecessor_expired,
            self._manager_runtime_epoch,
            self._manager_owner_epoch,
            self._last_sequence,
            self._last_accepted_ns,
        )

    def _watchdog(self) -> None:
        while not self._stop.wait(RENEWAL_INTERVAL_NS / 1_000_000_000 / 4):
            self.check_expiry()
            self.check_publication()

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
                        or self._clock() - self._started_ns <= EXPIRY_TIMEOUT_NS
                    )
                )
                or (
                    accepted is not None
                    and (self._clock() - accepted <= EXPIRY_TIMEOUT_NS)
                )
            ):
                return False
            manager_epoch = self._manager_runtime_epoch or "unknown"
            self._state = "expired"
            self._fenced = True
            self._predecessor_expired = True
            if self._receiver_status_sequence < MAXIMUM_SEQUENCE:
                self._receiver_status_sequence += 1
            expired_sequence = self._last_sequence
            expired_age = None if accepted is None else self._clock() - accepted
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
        # Containment and durable fault publication precede best-effort logs.
        logging.getLogger(__name__).error(
            "Manager heartbeat expired: epoch=%s sequence=%s age_ns=%s",
            manager_epoch,
            expired_sequence,
            expired_age,
        )
        return True


__all__ = [
    "API_VERSION",
    "EXPIRY_TIMEOUT_NS",
    "LOCAL_SAFE_STATE_CONTRACT_ID",
    "LOCAL_SAFE_STATE_CONTRACT_REVISION",
    "POLICY_ID",
    "POLICY_REVISION",
    "PUBLISHER_FAULT_CODE",
    "ProgramRunnerLivenessError",
    "ProgramRunnerLivenessReceiver",
    "RENEWAL_INTERVAL_NS",
    "RetainedLiveness",
]
