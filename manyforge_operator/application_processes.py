"""The Manager's application process list, written by both launchers.

The Manager is the one supervisor of the application (build spec 2026-11-12,
3.3, S3; contract 1.1). A launcher prepares the session, writes this document
into ``current/``, starts the Manager with ``--application-processes <path>``
and waits on it. The Manager starts every ``immediate`` entry in list order
once it has minted the application-start identity, every ``after_baseline``
entry on its first observation tick after its first converged baseline,
appends ``--application-start-id <16 hex>`` to each argv, ends the application
when any of them exits, and stops them in reverse list order.
"""

from __future__ import annotations

import ctypes
from dataclasses import dataclass, field
import json
import os
from pathlib import Path
import re
import signal
import socket
from typing import Any, Callable, Mapping, Sequence

API_VERSION = "manyforge.application_processes.v1"
START_IMMEDIATE = "immediate"
START_AFTER_BASELINE = "after_baseline"
# The three roles the Manager starts from the list (contract 1.1, 1.2); the
# execution host is not listed.
ROLES = ("headless", "program_runner", "composer")
MAXIMUM_ARGUMENTS = 256
MAXIMUM_ARGUMENT_BYTES = 4096
MAXIMUM_ENVIRONMENT_MEMBERS = 64
MAXIMUM_ENVIRONMENT_VALUE_BYTES = 4096
# Manager exit statuses a launcher reports. 72 is the Manager's
# kApplicationComponentExitStatus (an application process or the execution
# host exited); 74 its kApplicationStopIncompleteExitStatus (an orderly stop
# in which a step did not complete: a process killed, the execution host's
# exit not clean, the Halt not complete); 70 and 71 stay its startup and
# internal failures. Only MANAGER_STOPPED is a clean stop.
MANAGER_STOPPED = 0
MANAGER_APPLICATION_PROCESS_EXITED = 72
MANAGER_STOP_INCOMPLETE = 74
_ENVIRONMENT_NAME = re.compile(r"[A-Z_][A-Z0-9_]*\Z")
_PR_SET_PDEATHSIG = 1
_SHUTDOWN_COMMAND = b'{"api_version":"manyforge.manager_command.v2","command":"shutdown_runtime"}'


@dataclass(frozen=True, slots=True)
class ApplicationProcess:
    """One application process the Manager starts, watches and stops."""

    role: str
    argv: tuple[str, ...]
    environment: Mapping[str, str] = field(default_factory=dict)
    start: str = START_IMMEDIATE


def document(processes: Sequence[ApplicationProcess]) -> dict[str, Any]:
    """Return the ``manyforge.application_processes.v1`` document."""

    if not processes:
        raise ValueError("an application has at least one listed process")
    roles = [process.role for process in processes]
    if len(set(roles)) != len(roles):
        raise ValueError("each application process role is listed at most once")
    rows: list[dict[str, Any]] = []
    for process in processes:
        if process.role not in ROLES:
            raise ValueError(f"application process role is not listable: {process.role!r}")
        if (not 1 <= len(process.argv) <= MAXIMUM_ARGUMENTS
                or not all(isinstance(item, str) and "\0" not in item
                           and 1 <= len(item.encode("utf-8")) <= MAXIMUM_ARGUMENT_BYTES
                           for item in process.argv)
                or not Path(process.argv[0]).is_absolute()):
            raise ValueError(f"{process.role} needs 1-{MAXIMUM_ARGUMENTS} arguments of "
                             f"1-{MAXIMUM_ARGUMENT_BYTES} bytes, the first an absolute path")
        if process.start not in (START_IMMEDIATE, START_AFTER_BASELINE):
            raise ValueError(f"{process.role} has an unknown start condition")
        if len(process.environment) > MAXIMUM_ENVIRONMENT_MEMBERS:
            raise ValueError(f"{process.role} has more than {MAXIMUM_ENVIRONMENT_MEMBERS} environment entries")
        for key, value in process.environment.items():
            if (not _ENVIRONMENT_NAME.fullmatch(key) or "\0" in value
                    or len(value.encode("utf-8")) > MAXIMUM_ENVIRONMENT_VALUE_BYTES):
                raise ValueError(f"{process.role} has an invalid environment entry {key!r}")
        rows.append({
            "role": process.role,
            "argv": list(process.argv),
            "environment": dict(sorted(process.environment.items())),
            "start": process.start,
        })
    return {"api_version": API_VERSION, "processes": rows}


def write_document(path: Path, processes: Sequence[ApplicationProcess]) -> Path:
    """Write the process list once, as a private regular file, and return it."""

    path = Path(path)
    if not path.is_absolute():
        raise ValueError("the application process list path must be absolute")
    payload = (json.dumps(document(processes), sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, "wb") as output:
        output.write(payload)
    return path


def parent_death_preexec() -> Callable[[], None]:
    """Return a ``preexec_fn`` that ends the Manager when its launcher ends.

    The child sets ``PR_SET_PDEATHSIG(SIGTERM)`` and then checks that the
    launcher is still its parent (it may have died between fork and prctl).
    The C library is loaded here, in the launcher, never after fork.
    """

    libc = ctypes.CDLL(None, use_errno=True)
    launcher = os.getpid()

    def preexec() -> None:
        if libc.prctl(_PR_SET_PDEATHSIG, signal.SIGTERM, 0, 0, 0) != 0 or os.getppid() != launcher:
            os._exit(70)

    return preexec


def request_manager_shutdown(socket_path: Path, *, timeout_seconds: float) -> dict[str, Any] | None:
    """Send ``shutdown_runtime`` to the Manager; return its receipt, or None."""

    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
            client.settimeout(timeout_seconds)
            client.connect(str(socket_path))
            client.sendall(_SHUTDOWN_COMMAND)
            receipt = json.loads(client.recv(64 * 1024))
    except (OSError, ValueError):
        return None
    return receipt if isinstance(receipt, dict) else None


__all__ = [
    "API_VERSION",
    "ApplicationProcess",
    "MANAGER_APPLICATION_PROCESS_EXITED",
    "MANAGER_STOP_INCOMPLETE",
    "MANAGER_STOPPED",
    "ROLES",
    "START_AFTER_BASELINE",
    "START_IMMEDIATE",
    "document",
    "parent_death_preexec",
    "request_manager_shutdown",
    "write_document",
]
