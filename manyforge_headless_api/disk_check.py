"""The disk check (OD-215): operation stops while the disk cannot keep
ManyForge's records.

One owner of the condition, on a thread of its own. It is never the event
pump, never under the Headless core lock, and never on a Stop, tick or
feedback path. Every ``SIGNAL_PERIOD_SECONDS`` it reads, from memory, the
two record writers' "queue full" levels (History's through its own writer
lock, the Kernel journal's as the event pump last saw it in the runtime
faults); once a second, and at once after a write failed because the disk
was full, it reads free space on each file system that holds records (the
History folder and the Kernel journal's folder, read once per device). The
floor rule is the launchers' one (``run_session.free_for_floor``): no free
inode counts as no free space.

Entry (free space below the floor, or a queue full): one Halt to the
Kernel's Stop socket, as the Manager's Halt for a late execution host, with
its own source label; then one alarm,
``manyforge.runtime.disk_cannot_keep_records``, that gates motion while it
is active and whose facts are fixed at entry. A Halt that cannot be sent is
logged and not retried. Leaving (free space at the floor plus the margin
everywhere, and both queues written empty): the alarm clears. Nothing moves
by itself: the operator continues with Execute, then Resume or Run.

Nothing else waits for this thread. A free-space read on a stalled disk
blocks only here: the read holds no lock (``os.fstatvfs`` releases the
interpreter lock), and shutdown does not wait for it.
"""

from __future__ import annotations

from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
import logging
import os
from pathlib import Path
import threading
import time
from typing import Any

from manyforge_operator.run_session import (
    DiskReading,
    _size_text as size_text,
    free_for_floor,
    read_disk,
)

from .native import HeadlessApiError

_LOGGER = logging.getLogger(__name__)

#: The Halt's source label; the run status names its cause from it.
HALT_SOURCE_LABEL = "headless.disk_cannot_keep_records"
#: How often the writers' levels are read (memory only).
SIGNAL_PERIOD_SECONDS = 0.1
#: How often free space is read: a sampling period, not a timeout.
READ_PERIOD_SECONDS = 1.0
#: Shutdown waits at most this long; a read blocked on a stalled disk is
#: left behind (the thread is a daemon).
_SHUTDOWN_WAIT_SECONDS = 1.0

OPERATION_JOURNAL = "operation_journal"
EVENT_HISTORY = "event_history"
_WRITER_TEXT = {
    OPERATION_JOURNAL: "the operation journal",
    EVENT_HISTORY: "the event history",
}


@dataclass(frozen=True)
class RecordFolder:
    """A folder that holds one writer's records."""

    writer: str
    path: Path


def fd_folder_reader() -> Callable[[Path], tuple[int, DiskReading]]:
    """The production reader: (device, reading) of a folder's file system.

    Each folder is opened once, on the disk check's thread, and then read
    with ``fstatvfs`` (no path lookup per read).
    """

    descriptors: dict[Path, tuple[int, int]] = {}

    def read(path: Path) -> tuple[int, DiskReading]:
        held = descriptors.get(path)
        if held is None:
            fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
            held = descriptors[path] = (fd, os.fstat(fd).st_dev)
        return held[1], read_disk(held[0])

    return read


class DiskRecordsCheck:
    """The one owner of "the disk cannot keep ManyForge's records"."""

    def __init__(
        self,
        *,
        folders: Sequence[RecordFolder],
        floor_bytes: int,
        margin_bytes: int,
        history_signals: Callable[[], Mapping[str, bool]],
        journal_signals: Callable[[], tuple[bool, bool]],
        halt: Callable[[], Mapping[str, Any]],
        observe_condition: Callable[[Mapping[str, Any]], Any],
        reader: Callable[[Path], tuple[int, DiskReading]] | None = None,
        clock: Callable[[], float] = time.monotonic,
        signal_period: float = SIGNAL_PERIOD_SECONDS,
        read_period: float = READ_PERIOD_SECONDS,
    ) -> None:
        self._folders = tuple(folders)
        self._floor = floor_bytes
        self._margin = margin_bytes
        self._history_signals = history_signals
        self._journal_signals = journal_signals
        self._halt = halt
        self._observe_condition = observe_condition
        self._reader = reader or fd_folder_reader()
        self._clock = clock
        self._signal_period = signal_period
        self._read_period = read_period
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        # Owned by the check's thread (or by `step` in tests).
        self._next_read = float("-inf")
        # writer -> latest free bytes of its folder's file system (None when
        # the folder cannot be read).
        self._free: dict[str, int | None] = {}
        self._history_storage_full = False
        self._journal_storage_failure = False
        self._history_unreadable = False
        self._unreadable: set[str] = set()
        self.active = False
        # The alarm condition wanted and the one the core holds.
        self._wanted: Mapping[str, Any] = {"active": False, "details": {}}
        self._published: Mapping[str, Any] | None = {"active": False, "details": {}}
        self._publish_failed = False

    def start(self) -> None:
        if self._thread is not None:
            return
        self._thread = threading.Thread(
            target=self._run, name="manyforge-disk-check", daemon=True
        )
        self._thread.start()

    def shutdown(self) -> None:
        self._stop.set()
        thread = self._thread
        if thread is not None:
            thread.join(timeout=_SHUTDOWN_WAIT_SECONDS)
            if thread.is_alive():
                _LOGGER.warning(
                    "disk check is still reading the disk; ManyForge stops without waiting for it"
                )
        self._thread = None

    def _run(self) -> None:
        while not self._stop.is_set():
            self.step()
            self._stop.wait(self._signal_period)

    def step(self) -> None:
        """One reading and decision (the thread's loop body)."""

        now = self._clock()
        history_full, history_storage_full = self._read_history()
        journal_full, journal_storage_failure = self._journal_signals()
        # The backstop: a write that failed because the disk was full makes
        # free space be read now rather than at the next second.
        failed_write = (history_storage_full and not self._history_storage_full) or (
            journal_storage_failure and not self._journal_storage_failure
        )
        self._history_storage_full = history_storage_full
        self._journal_storage_failure = journal_storage_failure
        if failed_write or now >= self._next_read:
            self._read_free_space()
            self._next_read = now + self._read_period
        if not self.active:
            entry = self._entry(journal_full, history_full)
            if entry is not None:
                self._enter(entry)
        elif not journal_full and not history_full and all(
            free is None or free >= self._floor + self._margin
            for free in self._free.values()
        ):
            self._leave()
        if self._published != self._wanted:
            self._publish()

    def _read_history(self) -> tuple[bool, bool]:
        try:
            signals = self._history_signals()
        except HeadlessApiError as error:
            if not self._history_unreadable:
                self._history_unreadable = True
                _LOGGER.warning("disk check cannot read History's queue: %s", error)
            return False, False
        if self._history_unreadable:
            self._history_unreadable = False
            _LOGGER.info("disk check reads History's queue again")
        return signals["queue_full"], signals["storage_full"]

    def _read_free_space(self) -> None:
        # Each file system once: a second folder on it reuses its reading.
        devices: dict[int, int] = {}
        free: dict[str, int | None] = {}
        for folder in self._folders:
            try:
                device, reading = self._reader(folder.path)
            except OSError as error:
                # A folder that cannot be opened holds no records to judge
                # (as the launchers' start check); logged once.
                if folder.writer not in self._unreadable:
                    self._unreadable.add(folder.writer)
                    _LOGGER.warning("disk check cannot read %s: %s", folder.path, error)
                free[folder.writer] = None
                continue
            self._unreadable.discard(folder.writer)
            if device not in devices:
                devices[device] = free_for_floor(reading)
            free[folder.writer] = devices[device]
        self._free = free

    def _entry(self, journal_full: bool, history_full: bool) -> dict[str, Any] | None:
        for folder in self._folders:
            free = self._free.get(folder.writer)
            if free is not None and free < self._floor:
                need = self._floor + self._margin - free
                return self._details(
                    "below_floor", folder, free, None,
                    f"Disk nearly full: {size_text(free)} free on the disk that holds "
                    f"{folder.path}. ManyForge stops below {size_text(self._floor)}. "
                    f"Free at least {size_text(need, up=True)} on that disk.",
                )
        for writer, full in ((OPERATION_JOURNAL, journal_full), (EVENT_HISTORY, history_full)):
            folder = next((item for item in self._folders if item.writer == writer), None)
            if full and folder is not None:
                return self._details(
                    "queue_full", folder, self._free.get(writer), writer,
                    f"Disk not taking records: {_WRITER_TEXT[writer]} could not write its "
                    f"records ({folder.path}). Check the disk.",
                )
        return None

    def _details(
        self, cause: str, folder: RecordFolder, free: int | None, writer: str | None,
        headline: str,
    ) -> dict[str, Any]:
        # The facts fixed at entry; the headline is composed from them.
        return {
            "cause": cause,
            "path": str(folder.path),
            "free_bytes": free,
            "floor_bytes": self._floor,
            "margin_bytes": self._margin,
            "writer": writer,
            "failure": {"message": headline},
        }

    def _enter(self, details: dict[str, Any]) -> None:
        self.active = True
        self._publish_failed = False
        headline = details["failure"]["message"]
        _LOGGER.warning("ManyForge stops operation: %s", headline)
        # The Halt first: it needs neither the core lock nor the disk.
        try:
            response = self._halt()
            if response.get("ok") is not True:
                error = response.get("error")
                code = error.get("code") if isinstance(error, dict) else None
                _LOGGER.error("disk check Halt was refused (%s); not retried", code)
        except HeadlessApiError as error:
            _LOGGER.error("disk check Halt could not be sent (%s); not retried", error.code)
        self._wanted = {"active": True, "details": details}

    def _leave(self) -> None:
        self.active = False
        self._publish_failed = False
        _LOGGER.warning(
            "The disk takes ManyForge's records again; operation can continue (Execute, then Resume or Run)"
        )
        self._wanted = {"active": False, "details": {}}

    def _publish(self) -> None:
        wanted = self._wanted
        try:
            self._observe_condition(wanted)
        except HeadlessApiError as error:
            # Retried at the next step while it differs (a level, as the
            # History alarm's cursor resubmits).
            if not self._publish_failed:
                self._publish_failed = True
                _LOGGER.error("disk check alarm could not be updated: %s", error)
            return
        self._published = wanted
