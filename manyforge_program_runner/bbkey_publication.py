"""One ordered BB key sender per source: a captured Run, or the cell between Runs.

Every writer (tree node, operator control, REST, MCP) commits through the
source's value store. Under the store lock the store hands each committed
change of a subscribed key to this publisher, which only captures it: the
frame (values and the next sequence number) joins the source's frames in
commit order, one frame per write (never merged). No lock is held while a
frame is sent: one drain request at a time sends the captured frames in
sequence order. A Run source drains on the Run's own sender, so a value
written before a node's next request reaches the Kernel before that request;
once the Run's sender has closed (the source lives until Reset), and for the
cell-level source between Runs, the source's own sender drains. A drain
request is never withdrawn: a committed value is state, and a frame left out
would be a sequence gap (source loss). A sent frame completes with its real
reply. The Kernel evaluates its rules before it ACKs.

A write returns once committed, with the receipt of its frame
(``PublicationReceipt``), answered once the Kernel answered that frame: the
writing node holds RUNNING and a command thread waits for it, so each returns
only after the host's answer, as when the write sent its frame itself (OD-213
part 3). A frame that is never sent (source not installed yet, superseded or
retired) is answered when it is dropped; a refused one, and every write
behind it, once the source is declared lost and its containment handed over.

The baseline (message 1) carries every key of the union (null when unset);
an update carries only the written key. Writes committed while the baseline
is on its way follow it as updates. While the Kernel answers the baseline
`bb_source_not_ready` (the matching Apply is still installing; or the Kernel
has not yet observed the Runner's newer Program cycle), the source does not
exist yet: the updates behind that baseline are dropped, writes only commit,
and the owner's next start sends a baseline built from the store at that
moment. An explicit `superseded` reply (the frame's occurrence was retired by
an authoritative replacement) is no fault. A source the Runner itself retired
(its own Reset, a new Run or a new Applied publication) never faults: a late
reply for one of its frames is recorded only. Any other refusal or unknown
reply is source loss and latches a publisher fault: publishing stops for
good (frames not sent yet are dropped), the sender makes the Kernel's source
unavailable (an exact-sender sequence gap, sent next on the same drain) so
its rules can't watch and hold their scope, then the Run's Program Stop is
requested, then the fault is reported to the owner's ``on_fault``. There are
no retries and no resends.
"""

from __future__ import annotations

from collections import deque
from collections.abc import Callable, Mapping
from dataclasses import dataclass
import logging
from math import isfinite
from threading import Lock
from typing import Any, Final
import unicodedata

from manyforge_behavior.orchestration_values import (
    ProgramValueStore,
    PublicationReceipt,
    PublishedKeyChange,
)
from manyforge_behavior.run_sender import RunSender


SCHEMA_VERSION: Final = "manyforge.program_bbkey_publication.v2"
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
    between Runs has neither (both None): it is identified by the Applied
    binding and publication (the host's existing
    ``active_publication_generation``) and the Program-cycle generation, which
    the existing Reset advances. Every new Applied publication is a new source.
    """

    claim_generation: int | None
    program_run_id: int | None
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


@dataclass(frozen=True, slots=True)
class _Frame:
    """One captured frame: its kind, sequence number, rows by key, and the
    receipt its writer holds."""

    kind: str
    sequence: int
    values: Mapping[str, dict[str, Any]]
    receipt: PublicationReceipt


@dataclass(frozen=True, slots=True)
class _Loss:
    """Queued after a latched fault: the drain sends the loss declaration
    next, then answers the receipts of the writes the fault ended."""

    receipts: list[PublicationReceipt]


class ProgramBlackboardKeyPublisher:
    """Capture one source's key writes in commit order; send them off the lock."""

    def __init__(
        self,
        scope: ProgramBlackboardPublicationScope,
        *,
        publish: Callable[[Mapping[str, Any]], Mapping[str, Any]],
        sender: RunSender,
        submit: Callable[[Callable[[], None]], Any],
        stop_run: Callable[[int], None],
        on_fault: Callable[[Mapping[str, Any], str], None],
        fault_reference: Callable[[Mapping[str, Any]], str],
        on_baseline_accepted: Callable[[], None] | None = None,
        source_ordinal: int = 0,
    ) -> None:
        self._scope = scope
        self._publish = publish
        # A Run's source drains on the Run's sender (its frames stay ordered
        # with the Run's requests; not withdrawable, so it is still sent
        # after the Run ends); the between-Runs source on its own sender.
        self._sender = sender
        # Containment (Program Stop, fault report) runs on the owner's work pool.
        self._submit = submit
        self._stop_run = stop_run
        self._on_fault = on_fault
        # The Kernel ACKed this source's baseline (successor proof).
        self._on_baseline_accepted = on_baseline_accepted
        # The owner's monotonic creation order of its sources.
        self._source_ordinal = source_ordinal
        self._fault_reference = fault_reference
        # Memory only: never held across I/O, a store call, a submit or a callback.
        self._lock = Lock()
        # Current published row of every key of the fixed union.
        self._current: dict[str, dict[str, Any]] = {}
        # Captured, not sent yet, in sequence order (a _Loss after a fault).
        self._frames: deque[_Frame | _Loss] = deque()
        # One drain request is queued or running.
        self._draining = False
        self._next_sequence = 1
        self._subscribed = False
        # The baseline is captured and its reply not read yet.
        self._baseline_in_flight = False
        self._baseline_accepted = False
        self._retired = False
        self._source_epoch: str | None = None
        self._fault: dict[str, Any] | None = None
        self._loss_declared = False
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
    def baseline_in_flight(self) -> bool:
        """The baseline is captured and its reply is not read yet."""
        with self._lock:
            return self._baseline_in_flight

    @property
    def settled(self) -> bool:
        """Nothing more to start: the baseline was ACKed, or the source
        faulted or retired."""
        with self._lock:
            return self._baseline_accepted or self._fault is not None or self._retired

    @property
    def fault(self) -> dict[str, Any] | None:
        with self._lock:
            return None if self._fault is None else dict(self._fault)

    @property
    def loss_confirmed(self) -> bool:
        with self._lock:
            return self._loss_confirmed

    def start(self, store: ProgramValueStore, keys: tuple[str, ...]) -> bool:
        """Capture the baseline from the store's current values and queue it.

        The first call subscribes the fixed union; a later call (the Kernel
        had not installed the source yet) captures a baseline from the store
        at that moment. True once nothing more is to be started (``settled``);
        False while the baseline is on its way or the Kernel answered
        `bb_source_not_ready` (until then a write only commits).
        """

        with self._lock:
            first = not self._subscribed
            self._subscribed = True
        if first:
            store.subscribe_published_keys(
                keys, baseline=self._capture_baseline, on_change=self._capture_change
            )
        else:
            store.read_published_baseline(self._capture_baseline)
        return self.settled

    def _capture_baseline(self, changes: tuple[PublishedKeyChange, ...]) -> None:
        # Store lock held: no commit falls between this baseline and the
        # updates captured after it.
        try:
            values = {change.key: _row(change) for change in changes}
        except _Unrepresentable as error:
            self._fail_queued(str(error))
            return
        with self._lock:
            if (
                self._fault is not None
                or self._retired
                or self._baseline_accepted
                or self._baseline_in_flight
            ):
                return
            self._current = values
            self._baseline_in_flight = True
            self._next_sequence = 2
            self._frames.append(_Frame("baseline", 1, values, PublicationReceipt()))
            drain = self._claim_drain_locked()
        if drain:
            self._request_drain()

    def _capture_change(self, change: PublishedKeyChange) -> PublicationReceipt | None:
        # Store lock held: one write, one frame, captured in commit order.
        # Before the source's baseline is captured a write only commits
        # (no receipt: nothing is sent).
        try:
            row = _row(change)
        except _Unrepresentable as error:
            return self._fail_queued(str(error))
        failure: str | None = None
        drain = False
        receipt: PublicationReceipt | None = None
        with self._lock:
            if self._fault is not None or self._retired:
                return None
            self._current[change.key] = row
            if not (self._baseline_accepted or self._baseline_in_flight):
                return None
            if self._next_sequence > MAXIMUM_SEQUENCE:
                failure = "BB publication sequence is exhausted"
            else:
                receipt = PublicationReceipt()
                self._frames.append(
                    _Frame("update", self._next_sequence, {change.key: row}, receipt)
                )
                self._next_sequence += 1
                drain = self._claim_drain_locked()
        if failure is not None:
            return self._fail_queued(failure)
        if drain:
            self._request_drain()
        return receipt

    def _claim_drain_locked(self) -> bool:
        if self._draining:
            return False  # The queued or running drain sends this frame too.
        self._draining = True
        return True

    def _request_drain(self) -> None:
        """Queue the one drain request; it is never withdrawn.

        Called while the store lock may be held: a submit only queues (no
        request is sent and nothing waits for one).
        """

        self._sender.submit(self._drain, withdrawable=False)

    def _drain(self) -> None:
        """Send the captured frames one at a time, in sequence order, with
        no lock held during a send; ends when none is left. A frame captured
        while this drain runs is sent by it too: a request submitted after
        that capture is queued behind this drain, so it follows the frame."""

        while True:
            with self._lock:
                if not self._frames:
                    self._draining = False
                    return
                item = self._frames.popleft()
            if isinstance(item, _Loss):
                self._lose_and_contain(item)
            elif self._send(item):
                item.receipt.answer()

    def _request(self, kind: str, sequence: int, values: Mapping[str, Any]) -> dict[str, Any]:
        scope = self._scope
        return {
            "schema_version": SCHEMA_VERSION,
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

    def _send(self, frame: _Frame) -> bool:
        """One exchange, its reply recorded. Never resent. False when the
        frame's receipt waits for the loss declaration its refusal queued."""

        baseline = frame.kind == "baseline"
        try:
            response = self._publish(
                self._request(frame.kind, frame.sequence, frame.values)
            )
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
            code = getattr(error, "code", type(error).__name__)
            details = getattr(error, "details", None)
            reason = details.get("reason") if isinstance(details, Mapping) else None
            dropped: list[PublicationReceipt] = []
            with self._lock:
                retired = self._retired
                not_ready = (
                    not retired and baseline and code == SOURCE_NOT_READY_CODE
                )
                if not_ready:
                    # The Kernel has not installed the source yet (Apply or
                    # Reset): the updates behind this baseline have no source;
                    # writes only commit until the owner's next start.
                    self._baseline_in_flight = False
                    self._next_sequence = 1
                    dropped = self._drop_frames_locked()
            _answer(dropped)
            if retired:
                # This sender already retired the source itself: a stale
                # or superseded refusal is recorded, never a fault.
                _LOGGER.info(
                    "BB publication %s for a retired source answered %s: run=%s",
                    frame.sequence, code, self._scope.program_run_id,
                )
                return True
            if not_ready:
                _LOGGER.info(
                    "BB baseline: source not ready yet (reason=%s); writes only commit",
                    reason,
                )
                return True
            # Refused or unanswered after the source exists: source loss.
            self._fail_in_drain(
                f"BB publication {frame.sequence} was not acknowledged: "
                f"code={code} reason={reason}: {error}",
                frame.receipt,
            )
            return False
        if status == "superseded":
            # Confirmed retirement of this occurrence by its replacement: no
            # fault, and nothing more to send for it.
            with self._lock:
                self._retired = True
                self._baseline_in_flight = False
                dropped = self._drop_frames_locked()
            _answer(dropped)
            _LOGGER.info(
                "BB publication %s superseded: run=%s cycle=%s",
                frame.sequence, self._scope.program_run_id, self._scope.cycle_generation,
            )
            return True
        with self._lock:
            changed = self._source_epoch not in {None, source_epoch}
            if not changed:
                self._source_epoch = source_epoch
                if baseline:
                    self._baseline_in_flight = False
                    self._baseline_accepted = True
        if changed:
            self._fail_in_drain(
                "BB publication source epoch changed within the Run", frame.receipt
            )
            return False
        if baseline and self._on_baseline_accepted is not None:
            self._submit_or_run(self._on_baseline_accepted)
        return True

    def _submit_or_run(self, work: Callable[[], None]) -> None:
        try:
            self._submit(work)
        except Exception:
            work()

    def _drop_frames_locked(self) -> list[PublicationReceipt]:
        """Drop the frames not sent yet (a queued loss declaration stays);
        their receipts are answered by the caller, off the lock."""

        dropped = [item.receipt for item in self._frames if isinstance(item, _Frame)]
        self._frames = deque(item for item in self._frames if isinstance(item, _Loss))
        return dropped

    def _fail_in_drain(self, message: str, receipt: PublicationReceipt) -> None:
        # On the drain: frames not sent yet are dropped; the loss declaration
        # is the next frame of this source. The refused frame's writer, and
        # the writers of the dropped ones, are answered after it.
        self._latch(message)
        with self._lock:
            receipts = [receipt, *self._drop_frames_locked()]
            for item in self._frames:
                assert isinstance(item, _Loss)
                receipts.extend(item.receipts)
            self._frames = deque([_Loss(receipts)])

    def _fail_queued(self, message: str) -> PublicationReceipt | None:
        # A writer or the owner found the fault (possibly under the store
        # lock): the frames already captured are sent first, then the loss.
        # The receipt is answered once the loss is declared (the writer that
        # found the fault); None when the source had already ended.
        if self._latch(message) is None:
            return None
        receipt = PublicationReceipt()
        with self._lock:
            self._frames.append(_Loss([receipt]))
            drain = self._claim_drain_locked()
        if drain:
            self._request_drain()
        return receipt

    def fail(self, message: str) -> None:
        """Latch once: stop publishing, lose the Kernel's source, request
        Program Stop, report the fault (on this source's drain)."""

        self._fail_queued(message)

    def _latch(self, message: str) -> dict[str, Any] | None:
        error = publisher_fault_error(message)
        with self._lock:
            if self._fault is not None or self._retired:
                return None
            self._fault = error
            self._baseline_in_flight = False
        return error

    def _lose_and_contain(self, loss: _Loss) -> None:
        with self._lock:
            error = self._fault
            first = error is not None and not self._loss_declared
            if first:
                self._loss_declared = True
        if first:
            assert error is not None
            lost = self._declare_lost()
            with self._lock:
                self._loss_confirmed = lost
            self._submit_or_run(lambda: self._contain(error))
        _answer(loss.receipts)

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
        Runner retires this source itself; frames not sent yet are dropped
        (a pending loss declaration still goes); the replacement baseline
        carries the current values (run-only keys at their declared
        `initial` after Reset)."""

        with self._lock:
            self._retired = True
            self._baseline_in_flight = False
            dropped = self._drop_frames_locked()
        _answer(dropped)


def _answer(receipts: list[PublicationReceipt]) -> None:
    for receipt in receipts:
        receipt.answer()


__all__ = [
    "COMMAND",
    "FAULT_CODE",
    "ProgramBlackboardKeyPublisher",
    "ProgramBlackboardPublicationScope",
    "SCHEMA_VERSION",
    "SOURCE_NOT_READY_CODE",
    "publisher_fault_error",
]
