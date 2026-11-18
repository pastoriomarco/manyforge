"""Private receiver of the Manager's identity reports for the Program Runner.

Reports never expire: a quiet Manager closes, fences and stops nothing here.
"""

from __future__ import annotations

from collections.abc import Callable, Mapping
import json
import logging
import os
from pathlib import Path
import select
import socket
import stat
import struct
from threading import Event, RLock, Thread
from typing import Any, Final
import unicodedata

from manyforge_headless_api.program_cycle import AcceptedProgramResetDTO


API_VERSION: Final = "manyforge.program_runner_liveness.v2"
POLICY_ID: Final = "manyforge.program-runner.manager-presence.v1"
POLICY_REVISION: Final = "1"
# Cadence at which pending BB key sources are started.
PUBLICATION_CHECK_INTERVAL_S: Final = 0.0625
MAXIMUM_PACKET_BYTES: Final = 16_384
MAXIMUM_SEQUENCE: Final = 9_007_199_254_740_991
# A Runner fault (OD-267): the closed set of codes that fence the cell.
PUBLISHER_FAULT_CODE: Final = "program_bbkey_publication_failed"
TICK_WORKER_FAULT_CODE: Final = "program_tick_worker_failed"
RUNNER_FAULT_CODES: Final = frozenset({PUBLISHER_FAULT_CODE, TICK_WORKER_FAULT_CODE})
_MAXIMUM_RUNNER_FAULT_MESSAGE_BYTES: Final = 1_024


_LOGGER = logging.getLogger(__name__)


class ProgramRunnerLivenessError(RuntimeError):
    """A malformed, foreign, or stale private liveness packet.

    ``code`` is the typed cause of a fence that has one (a failed tick
    worker), else None.
    """

    def __init__(self, message: str, *, code: str | None = None) -> None:
        super().__init__(message)
        self.code = code


class ProgramRunnerLivenessReceiver:
    """Mode-0600 seqpacket listener plus the pending BB key-source check."""

    def __init__(
        self,
        path: Path,
        *,
        cell_id: str,
        receiver_runtime_unit_id: str,
        receiver_runtime_epoch: str,
        cycle_status: Callable[[], Mapping[str, Any]],
        publication_check: Callable[[], None] | None = None,
    ) -> None:
        if not path.is_absolute() or path.name in {"", ".", ".."}:
            raise ValueError("Program Runner liveness socket must be absolute")
        self._path = path
        self._cell_id = cell_id
        self._runtime_unit_id = receiver_runtime_unit_id
        self._runtime_epoch = receiver_runtime_epoch
        self._cycle_status = cycle_status
        # Starts pending BB key sources, run on the publication thread.
        self._publication_check = publication_check
        self._lock = RLock()
        self._stop = Event()
        self._listener: socket.socket | None = None
        self._listener_identity: tuple[int, int] | None = None
        self._run_root_fd: int | None = None
        self._run_root_identity: tuple[int, int] | None = None
        self._server_thread: Thread | None = None
        self._publication_thread: Thread | None = None
        # The serving thread's stop event (OD-296 (a)): close() writes it, so
        # the thread waits for a connection or a request without a time limit.
        self._stop_read_fd: int | None = None
        self._stop_write_fd: int | None = None
        self._receiver_status_sequence = 1
        self._state = "unreconciled"
        self._fenced = True
        self._last_rejection: str | None = None
        # Independent of Manager presence: no heartbeat or reconciliation
        # clears it, and it keeps commands fenced while Manager stays current.
        self._runner_fault: dict[str, Any] | None = None
        self._runner_fault_reference: str | None = None

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
            self._stop_read_fd, self._stop_write_fd = os.pipe2(
                os.O_CLOEXEC | os.O_NONBLOCK
            )
            self._listener = listener
            self._server_thread = Thread(
                target=self._serve,
                name="program-runner-liveness",
                daemon=True,
            )
            self._publication_thread = Thread(
                target=self._publication_loop,
                name="program-runner-bbkey-publication",
                daemon=True,
            )
            self._server_thread.start()
            self._publication_thread.start()
        except BaseException:
            self._stop.set()
            self._signal_stop()
            if listener is not None:
                listener.close()
            for thread in (self._server_thread, self._publication_thread):
                if thread is not None and thread.is_alive():
                    thread.join()
            self._close_stop_pipe()
            self._remove_endpoint(descriptor, bound_identity)
            os.close(descriptor)
            self._run_root_fd = None
            self._run_root_identity = None
            self._listener = None
            raise

    def _signal_stop(self) -> None:
        if self._stop_write_fd is not None:
            try:
                os.write(self._stop_write_fd, b"\0")
            except BlockingIOError:
                pass  # Already signalled.

    def _close_stop_pipe(self) -> None:
        for descriptor in (self._stop_read_fd, self._stop_write_fd):
            if descriptor is not None:
                os.close(descriptor)
        self._stop_read_fd = None
        self._stop_write_fd = None

    def close(self) -> None:
        self._stop.set()
        self._signal_stop()
        listener = self._listener
        for thread in (self._server_thread, self._publication_thread):
            if thread is not None:
                thread.join()
        if listener is not None:
            listener.close()
        self._close_stop_pipe()
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
            if self._runner_fault is not None:
                code = self._runner_fault["code"]
                raise ProgramRunnerLivenessError(
                    f"Program Runner is fenced by a Runner fault: {code}",
                    code=code if code == TICK_WORKER_FAULT_CODE else None,
                )
            if self._state != "current" or self._fenced:
                raise ProgramRunnerLivenessError(
                    "Program Runner awaits the Manager's reconciliation"
                )

    def latch_runner_fault(
        self, fault: Mapping[str, Any], reference: str
    ) -> bool:
        """Latch a Runner fault. A BB publication fault is latched while no
        fault is, until its owner clears it (`clear_runner_fault`). A tick
        worker fault replaces a BB publication fault and never clears.
        True when this call latched it."""

        code = fault.get("code")
        message = fault.get("message")
        if (
            set(fault) != {"code", "message", "details"}
            or code not in RUNNER_FAULT_CODES
            or fault.get("details") != {}
            or not isinstance(message, str)
            or not message
            or len(message.encode("utf-8")) > _MAXIMUM_RUNNER_FAULT_MESSAGE_BYTES
            or unicodedata.normalize("NFC", message) != message
            or not isinstance(reference, str)
            or len(reference) != 71
            or not reference.startswith("sha256:")
        ):
            raise ProgramRunnerLivenessError("malformed Runner fault")
        with self._lock:
            if self._runner_fault is not None and (
                code == PUBLISHER_FAULT_CODE
                or self._runner_fault["code"] == TICK_WORKER_FAULT_CODE
            ):
                return False
            self._runner_fault = {
                "code": code,
                "message": message,
                "details": {},
            }
            self._runner_fault_reference = reference
            if self._receiver_status_sequence < MAXIMUM_SEQUENCE:
                self._receiver_status_sequence += 1
        return True

    def clear_runner_fault(self, reference: str) -> bool:
        """Clear exactly the latched BB publication fault with this reference
        (a source whose loss the Kernel confirmed, or that a newer source
        superseded). True when it was cleared."""

        with self._lock:
            if (
                self._runner_fault is None
                or self._runner_fault["code"] != PUBLISHER_FAULT_CODE
                or self._runner_fault_reference != reference
            ):
                return False
            self._runner_fault = None
            self._runner_fault_reference = None
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
        with self._lock:
            runner_fault = (
                None if self._runner_fault is None else dict(self._runner_fault)
            )
            return {
                "schema_version": "manyforge.program_runner_liveness_status.v2",
                "receiver_status_sequence": self._receiver_status_sequence,
                "state": self._state,
                # A Runner fault reuses the digested fence; its reference
                # is the digest of the emitted Runner-fault ErrorDTO.
                "command_fenced": self._fenced or runner_fault is not None,
                "fault_reference": (
                    None if runner_fault is None else self._runner_fault_reference
                ),
                "runner_fault": runner_fault,
            }

    def _serve(self) -> None:
        """Serve the Manager's liveness exchanges (OD-296 (a)): each wait ends
        on its event, never on a time limit. Waiting for a connection or a
        request ends on its arrival, on the Manager closing the connection
        or on close(); a Runner that is slow to answer is never lost."""

        stop_fd = self._stop_read_fd
        while not self._stop.is_set():
            listener = self._listener
            if listener is None or stop_fd is None:
                return
            try:
                readable, _, _ = select.select([listener, stop_fd], [], [])
                if stop_fd in readable:
                    return
                connection, _ = listener.accept()
            except BlockingIOError:
                continue  # The connection went before it was accepted.
            except (OSError, ValueError):
                if self._stop.is_set():
                    return
                continue
            with connection:
                connection.setblocking(True)
                try:
                    credentials = connection.getsockopt(
                        socket.SOL_SOCKET, socket.SO_PEERCRED, 12
                    )
                    _peer_pid, peer_uid, _peer_gid = struct.unpack("3i", credentials)
                    if peer_uid != os.geteuid():
                        raise ProgramRunnerLivenessError("foreign peer uid")
                    # The request, the Manager closing the connection (an
                    # empty read below) or close().
                    readable, _, _ = select.select([connection, stop_fd], [], [])
                    if stop_fd in readable:
                        return
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
                "causal_request_key",
                "cycle_generation",
                "current_command_kind",
                "runner_commands",
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
                "cycle_generation",
                "state",
                "program_run_id",
                "cycle_claim_generation",
                "terminal_or_settlement_reference",
                "accepted_reset",
            )
        }
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
            type(generation) is not int
            or not 1 <= generation <= MAXIMUM_SEQUENCE
            or state
            not in {
                "idle_after_reset",
                "starting",
                "running",
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
            # A start announces its run only after the Kernel claim (1.15.5).
            and not (state == "starting" and run_id is None and claim_generation is None)
            and (
                type(run_id) is not int
                or not 1 <= run_id <= MAXIMUM_SEQUENCE
            )
        ):
            raise ProgramRunnerLivenessError("malformed Program-cycle run identity")
        if claim_generation is not None and type(run_id) is not int:
            raise ProgramRunnerLivenessError("malformed Program-cycle run identity")
        claimless_terminal = state == "terminal_awaiting_reset" and terminal is not None
        if (
            not idle
            and claim_generation is None
            and state not in {"starting", "resetting", "recovery_required"}
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

    def _handle(self, request: Mapping[str, Any]) -> dict[str, Any]:
        if request.get("api_version") != API_VERSION:
            raise ProgramRunnerLivenessError("unsupported liveness API")
        kind = request.get("kind")
        if kind == "probe":
            self._exact(request, {"api_version", "cell_id", "kind"})
            return {
                "api_version": API_VERSION,
                "cell_id": self._cell_id,
                "kind": "probe_ack",
                "program_cycle_status": self._project_cycle_status(),
                "receiver_runtime_epoch": self._runtime_epoch,
                "receiver_runtime_unit_id": self._runtime_unit_id,
                "runtime_policy_id": POLICY_ID,
                "runtime_policy_revision": POLICY_REVISION,
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
            "manager_runtime_unit_id",
            "required_runtime_set_digest",
        ):
            if not isinstance(request[key], str) or not request[key]:
                raise ProgramRunnerLivenessError(f"invalid {key}")
        sequence = request["heartbeat_sequence"]
        if type(sequence) is not int or not 1 <= sequence <= MAXIMUM_SEQUENCE:
            raise ProgramRunnerLivenessError("foreign heartbeat identity")
        reconciliation = request["reconciliation_digest"]
        if reconciliation is not None and (
            not isinstance(reconciliation, str)
            or not reconciliation.startswith("sha256:")
            or len(reconciliation) != 71
        ):
            raise ProgramRunnerLivenessError("invalid reconciliation digest")

        # Heartbeats are served one at a time on this receiver's single
        # serving thread; this lock is held for memory-only work.
        with self._lock:
            needs_reconciliation = self._state != "current"
            if self._receiver_status_sequence >= MAXIMUM_SEQUENCE:
                raise ProgramRunnerLivenessError(
                    "receiver status sequence is exhausted"
                )
            self._receiver_status_sequence += 1
            # Receipt proves peer liveness, not runtime readiness. During Apply
            # a healthy Manager may have no reconciled native projection yet.
            if reconciliation is not None:
                self._state = "current"
                self._fenced = False
            elif needs_reconciliation:
                self._state = "unreconciled"
                self._fenced = True
        status = self.status()
        return {
            **status,
            "api_version": API_VERSION,
            "kind": "heartbeat_ack",
            "program_cycle_status": self._project_cycle_status(),
        }

    def _publication_loop(self) -> None:
        while not self._stop.wait(PUBLICATION_CHECK_INTERVAL_S):
            self.check_publication()


__all__ = [
    "API_VERSION",
    "POLICY_ID",
    "POLICY_REVISION",
    "PUBLICATION_CHECK_INTERVAL_S",
    "PUBLISHER_FAULT_CODE",
    "RUNNER_FAULT_CODES",
    "TICK_WORKER_FAULT_CODE",
    "ProgramRunnerLivenessError",
    "ProgramRunnerLivenessReceiver",
]
