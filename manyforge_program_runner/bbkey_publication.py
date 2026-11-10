"""One ordered BB key sender per source: a captured Run, or the cell between Runs.

Every writer (tree node, operator control, REST, MCP) commits through
the source's value store, and the store forwards each committed change of a
subscribed key synchronously under its lock, so writes reach the Kernel in
commit order, one frame per write (never merged), and a write returns only
after the Kernel's reply. The Kernel evaluates its rules before it ACKs.

The baseline (message 1) carries every key of the union (null when unset);
an update carries only the written key. While the Kernel
answers the baseline `bb_source_not_ready` (the matching Apply is
still installing; or the Kernel has not yet observed the Runner's newer
Program cycle), the source does not exist yet: writes only commit, and the
owner's next start sends a baseline built from the store at that moment.
An explicit `superseded` reply (the frame's occurrence was retired by an
authoritative replacement) is no fault. A source the Runner itself retired
(its own Reset, a new Run or a new Applied publication) never faults: a late
reply for one of its frames is recorded only. Any other refusal or unknown
reply is source loss and latches a publisher fault: publishing stops for
good, the Run's Program Stop is requested first, then the fault is reported
to the owner's ``on_fault``. Before any of that the sender makes the Kernel's
source unavailable (an exact-sender sequence gap), so its rules can't watch
and hold their scope. There are no
retries and no resends.
"""

from __future__ import annotations

from collections.abc import Callable, Mapping
from dataclasses import dataclass
import logging
from math import isfinite
from threading import Lock
from typing import Any, Final
import unicodedata

from manyforge_behavior.orchestration_values import (
    ProgramValueStore,
    PublishedKeyChange,
)


SCHEMA_VERSION: Final = "manyforge.program_bbkey_publication.v1"
COMMAND: Final = "publish_program_bbkeys"
FAULT_CODE: Final = "program_bbkey_publication_failed"
# The matching current Apply is still installing the source, or
# the Kernel has not yet observed the Runner's newer Program cycle.
SOURCE_NOT_READY_CODE: Final = "bb_source_not_ready"
# The Kernel's typed proof that an exact-sender gap lost this source.
SEQUENCE_GAP_REASON: Final = "program_bb_sequence_gap"
MAXIMUM_SEQUENCE: Final = 2**64 - 1
_MAXIMUM_FAULT_MESSAGE_BYTES: Final = 1_024

_LOGGER = logging.getLogger(__name__)


@dataclass(frozen=True, slots=True)
class ProgramBlackboardPublicationScope:
    """The exact identities every frame repeats.

    A Run source names its captured claim and Run. The cell-level source
    between Runs has neither (both None): it is identified by the Runner
    owner epoch, the Applied binding and publication (the host's existing
    ``active_publication_generation``) and the Program-cycle generation, which
    the existing Reset advances. Every new Applied publication is a new source.
    """

    owner_epoch: str
    program_runner_owner_epoch: str
    claim_generation: int | None
    program_run_id: str | None
    program_binding_id: str
    active_publication_generation: int
    cycle_generation: int


class _Unrepresentable(ValueError):
    pass


def _row(change: PublishedKeyChange) -> dict[str, Any]:
    value = change.value
    if not change.present:
        # An actually unset key is published as null, never initialized.
        wire: Any = None
    elif change.value_type == "bool" and type(value) is bool:
        wire = value
    elif change.value_type == "int64" and type(value) is int:
        wire = str(value)
    elif (
        change.value_type == "float64"
        and not isinstance(value, bool)
        and isinstance(value, (int, float))
        and isfinite(float(value))
    ):
        wire = float(value)
    elif (
        change.value_type == "string"
        and isinstance(value, str)
        and unicodedata.normalize("NFC", value) == value
    ):
        wire = value
    else:
        raise _Unrepresentable(f"referenced key {change.key!r} is not publishable")
    return {"name": change.key, "value_type": change.value_type, "value": wire}


def publisher_fault_error(message: str) -> dict[str, Any]:
    """The one closed publisher-fault ErrorDTO the Manager accepts."""

    text = unicodedata.normalize("NFC", message) or FAULT_CODE
    encoded = text.encode("utf-8")[:_MAXIMUM_FAULT_MESSAGE_BYTES]
    return {
        "code": FAULT_CODE,
        "message": encoded.decode("utf-8", errors="ignore"),
        "details": {},
    }


class ProgramBlackboardKeyPublisher:
    """Forward one source's key writes synchronously, in commit order."""

    def __init__(
        self,
        scope: ProgramBlackboardPublicationScope,
        *,
        publish: Callable[[Mapping[str, Any]], Mapping[str, Any]],
        submit: Callable[[Callable[[], None]], Any],
        stop_run: Callable[[str], None],
        on_fault: Callable[[Mapping[str, Any], str], None],
        fault_reference: Callable[[Mapping[str, Any]], str],
        on_baseline_accepted: Callable[[], None] | None = None,
        source_ordinal: int = 0,
    ) -> None:
        self._scope = scope
        self._publish = publish
        # Containment (Program Stop, fault report) runs off the store lock.
        self._submit = submit
        self._stop_run = stop_run
        self._on_fault = on_fault
        # The Kernel ACKed this source's baseline (successor proof).
        self._on_baseline_accepted = on_baseline_accepted
        # The owner's monotonic creation order of its sources.
        self._source_ordinal = source_ordinal
        self._baseline_accepted = False
        self._fault_reference = fault_reference
        # Never held across I/O, a store call or a callback.
        self._lock = Lock()
        # Current published row of every key of the fixed union.
        self._current: dict[str, dict[str, Any]] = {}
        self._next_sequence = 1
        self._subscribed = False
        self._retired = False
        self._source_epoch: str | None = None
        self._fault: dict[str, Any] | None = None
        # The Kernel confirmed this faulted source lost.
        self._loss_confirmed = False

    @property
    def scope(self) -> ProgramBlackboardPublicationScope:
        return self._scope

    @property
    def source_ordinal(self) -> int:
        return self._source_ordinal

    @property
    def baseline_accepted(self) -> bool:
        with self._lock:
            return self._baseline_accepted

    @property
    def fault(self) -> dict[str, Any] | None:
        with self._lock:
            return None if self._fault is None else dict(self._fault)

    @property
    def loss_confirmed(self) -> bool:
        with self._lock:
            return self._loss_confirmed

    def start(self, store: ProgramValueStore, keys: tuple[str, ...]) -> bool:
        """Send the baseline from the store's current values.

        The first call subscribes the fixed union; a later call (the Kernel
        had not installed the source yet) resends a baseline built from the
        store at that moment. False while the Kernel answers
        `bb_source_not_ready`: until then a write only commits.
        """

        with self._lock:
            first = not self._subscribed
            self._subscribed = True
        if first:
            store.subscribe_published_keys(
                keys, baseline=self._send_baseline, on_change=self._on_change
            )
        else:
            store.read_published_baseline(self._send_baseline)
        with self._lock:
            return self._baseline_accepted or self._fault is not None or self._retired

    def _send_baseline(self, changes: tuple[PublishedKeyChange, ...]) -> None:
        # Store lock held: no commit falls between this baseline and the
        # forwards that follow it.
        try:
            values = {change.key: _row(change) for change in changes}
        except _Unrepresentable as error:
            self._forward_failed(str(error))
            return
        with self._lock:
            if self._fault is not None or self._retired or self._baseline_accepted:
                return
            self._current = values
        if self._forward("baseline", 1, values, not_ready_allowed=True):
            with self._lock:
                self._baseline_accepted = True
                self._next_sequence = 2
            if self._on_baseline_accepted is not None:
                self._submit_or_run(self._on_baseline_accepted)

    def _on_change(self, change: PublishedKeyChange) -> None:
        # Store lock held: one write, one frame, forwarded before the write
        # returns. Before the source exists a write only commits.
        try:
            row = _row(change)
        except _Unrepresentable as error:
            self._forward_failed(str(error))
            return
        with self._lock:
            if self._fault is not None or self._retired:
                return
            self._current[change.key] = row
            if not self._baseline_accepted:
                return
            if self._next_sequence > MAXIMUM_SEQUENCE:
                failure: str | None = "BB publication sequence is exhausted"
            else:
                failure = None
                sequence = self._next_sequence
                self._next_sequence += 1
        if failure is not None:
            self._forward_failed(failure)
            return
        self._forward("update", sequence, {change.key: row})

    def _submit_or_run(self, work: Callable[[], None]) -> None:
        try:
            self._submit(work)
        except Exception:
            work()

    def _request(self, kind: str, sequence: int, values: Mapping[str, Any]) -> dict[str, Any]:
        scope = self._scope
        return {
            "schema_version": SCHEMA_VERSION,
            "owner_epoch": scope.owner_epoch,
            "program_runner_owner_epoch": scope.program_runner_owner_epoch,
            "claim_generation": scope.claim_generation,
            "program_run_id": scope.program_run_id,
            "program_binding_id": scope.program_binding_id,
            "active_publication_generation": scope.active_publication_generation,
            "cycle_generation": scope.cycle_generation,
            "kind": kind,
            "message_sequence": str(sequence),
            "previous_sequence": str(sequence - 1),
            "values": [dict(values[name]) for name in sorted(values)],
        }

    def _forward(
        self,
        kind: str,
        sequence: int,
        values: Mapping[str, Any],
        *,
        not_ready_allowed: bool = False,
    ) -> bool:
        """One exchange; True when the Kernel accepted it. Never resent."""

        try:
            response = self._publish(self._request(kind, sequence, values))
            status = response.get("status")
            source_epoch = response.get("source_epoch")
            if response.get("command") != COMMAND or status not in {
                "accepted", "superseded"
            }:
                raise ValueError("BB publication reply has the wrong kind or status")
            if status == "accepted" and (
                not isinstance(source_epoch, str) or not source_epoch
            ):
                raise ValueError("BB publication reply has no source epoch")
        except Exception as error:
            with self._lock:
                retired = self._retired
            code = getattr(error, "code", type(error).__name__)
            details = getattr(error, "details", None)
            reason = details.get("reason") if isinstance(details, Mapping) else None
            if retired:
                # This sender already retired the source itself: a stale
                # or superseded refusal is recorded, never a fault.
                _LOGGER.info(
                    "BB publication %s for a retired source answered %s: run=%s",
                    sequence, code, self._scope.program_run_id,
                )
                return False
            if not_ready_allowed and code == SOURCE_NOT_READY_CODE:
                # The Kernel has not installed the source yet (Apply or Reset).
                _LOGGER.info(
                    "BB baseline: source not ready yet (reason=%s); writes only commit",
                    reason,
                )
                return False
            # Refused or unanswered after the source exists: source loss.
            self._forward_failed(
                f"BB publication {sequence} was not acknowledged: "
                f"code={code} reason={reason}: {error}"
            )
            return False
        if status == "superseded":
            # Confirmed retirement of this occurrence by its replacement: no
            # fault, and nothing more to send for it.
            with self._lock:
                self._retired = True
            _LOGGER.info(
                "BB publication %s superseded: run=%s cycle=%s",
                sequence, self._scope.program_run_id, self._scope.cycle_generation,
            )
            return False
        with self._lock:
            changed = self._source_epoch not in {None, source_epoch}
            if not changed:
                self._source_epoch = source_epoch
        if changed:
            self._forward_failed("BB publication source epoch changed within the Run")
            return False
        return True

    def _forward_failed(self, message: str) -> None:
        # Store lock held: latch and lose the Kernel's source before the
        # write returns (no later write or admission sees the old value),
        # contain off the lock (Program Stop may block).
        error = self._latch(message)
        if error is not None:
            self._lose()
            self._submit_or_run(lambda: self._contain(error))

    def fail(self, message: str) -> None:
        """Latch once: stop publishing, lose the Kernel's source, request
        Program Stop, report the fault."""

        error = self._latch(message)
        if error is not None:
            self._lose()
            self._contain(error)

    def _lose(self) -> None:
        lost = self._declare_lost()
        with self._lock:
            self._loss_confirmed = lost

    def _latch(self, message: str) -> dict[str, Any] | None:
        error = publisher_fault_error(message)
        with self._lock:
            if self._fault is not None or self._retired:
                return None
            self._fault = error
        return error

    def _contain(self, error: dict[str, Any]) -> None:
        reference = self._fault_reference(error)
        try:
            # The cell-level source between Runs has no Run to stop.
            if self._scope.program_run_id is not None:
                self._stop_run(self._scope.program_run_id)
        except Exception:
            _LOGGER.exception(
                "Program Stop after BB publication fault failed: run=%s",
                self._scope.program_run_id,
            )
        try:
            self._on_fault(error, reference)
        except Exception:
            _LOGGER.exception("BB publication fault could not reach liveness")
        _LOGGER.error(
            "BB publication fault: run=%s %s",
            self._scope.program_run_id,
            error["message"],
        )

    def _declare_lost(self) -> bool:
        """After a fault: make the Kernel's source of this exact scope
        unavailable, never stale-but-usable. One update
        frame with a sequence gap is the Kernel's existing exact-sender loss
        (`program_bb_sequence_gap`); every BB-backed rule input of this source
        then can't watch and holds its scope until a new source (Reset or Run)
        replaces it. True only when the Kernel answered with that typed loss."""

        with self._lock:
            if self._fault is None or not self._current:
                return False
            name = min(self._current)
            request = self._request(
                "update", MAXIMUM_SEQUENCE, {name: dict(self._current[name])}
            )
        try:
            self._publish(request)
        except Exception as error:
            details = getattr(error, "details", None)
            return (
                isinstance(details, Mapping)
                and details.get("reason") == SEQUENCE_GAP_REASON
            )
        return False

    def retire(self) -> None:
        """Reset, next Run start, new Applied publication or shutdown: the
        Runner retires this source itself; the replacement baseline carries
        the current values (run-only keys at their declared `initial` after
        Reset)."""

        with self._lock:
            self._retired = True


__all__ = [
    "COMMAND",
    "FAULT_CODE",
    "ProgramBlackboardKeyPublisher",
    "ProgramBlackboardPublicationScope",
    "SCHEMA_VERSION",
    "SOURCE_NOT_READY_CODE",
    "publisher_fault_error",
]
