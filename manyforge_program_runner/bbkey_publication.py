"""One ordered change-only BB key sender per captured Program Run (contract 52).

The Run's fixed referenced-key union is subscribed once on its value store: an
atomic baseline is message 1, and every later committed change of a subscribed
key records a pending publication obligation in the same store critical
section. The baseline carries every key of the union (null when unset);
an update carries only its changed keys (bb-wire-shapes03). One request is in
flight; only the exact-sequence success reply clears an obligation. An
explicit `superseded` reply (the frame's occurrence was retired by an
authoritative replacement) retires only that exact obligation, with no fault.
A source the Runner itself retired (its own Reset, a new Run or a new Applied
publication) never faults: a late reply for one of its frames is recorded
only. `bb_source_not_ready` (SX-2143: the matching Apply is still installing;
or the Kernel has not yet observed the Runner's newer Program cycle, e.g. the
between-Runs baseline right after Reset journals the new Idle cycle) keeps the frame and retries it on the next liveness observation within the
same overall command budget. An obligation older than the command budget, or
an unknown/failed reply for the current source, latches a publisher fault:
publishing stops for good, the Run's Program Stop is requested first, then the
fault is reported to the owner's ``on_fault``. For a Run's source the owner
fences the Manager-presence liveness receiver; for the cell-level source
between Runs (no Run to stop) the fence is held until the Kernel confirms
the source lost (`declare_lost`) or a newer source superseded it (F-2). There are no renewals and no resends of an unknown reply.
"""

from __future__ import annotations

from collections import deque
from collections.abc import Callable, Mapping
from dataclasses import dataclass, field
import logging
from math import isfinite
from threading import Lock
import time
from typing import Any, Final
import unicodedata

from manyforge_behavior.orchestration_values import (
    ProgramValueStore,
    PublishedKeyChange,
)


SCHEMA_VERSION: Final = "manyforge.program_bbkey_publication.v1"
COMMAND: Final = "publish_program_bbkeys"
FAULT_CODE: Final = "program_bbkey_publication_failed"
# SX-2143/2146: the matching current Apply is still installing the source, or
# the Kernel has not yet observed the Runner's newer Program cycle.
SOURCE_NOT_READY_CODE: Final = "bb_source_not_ready"
# The Kernel's typed proof that an exact-sender gap lost this source (F-2).
SEQUENCE_GAP_REASON: Final = "program_bb_sequence_gap"
# Unsent frames beyond this fail closed as source loss; the same depth as the
# host's per-source I/O ingress default (contract 48).
MAXIMUM_PENDING_FRAMES: Final = 64
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


@dataclass(slots=True)
class _Frame:
    """The baseline (every key) or one update (``values`` = only ``changed``
    keys); a key's second change starts the next frame."""

    sequence: int
    kind: str
    recorded_ns: int
    values: dict[str, dict[str, Any]]
    changed: set[str] = field(default_factory=set)


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
    """Serialize one Run's BB key frames on the existing Runner worker pool."""

    def __init__(
        self,
        scope: ProgramBlackboardPublicationScope,
        *,
        publish: Callable[[Mapping[str, Any]], Mapping[str, Any]],
        submit: Callable[[Callable[[], None]], Any],
        stop_run: Callable[[str], None],
        on_fault: Callable[[Mapping[str, Any], str], None],
        fault_reference: Callable[[Mapping[str, Any]], str],
        budget_ns: int,
        monotonic_ns: Callable[[], int] = time.monotonic_ns,
        on_baseline_accepted: Callable[[], None] | None = None,
        source_ordinal: int = 0,
    ) -> None:
        if type(budget_ns) is not int or budget_ns <= 0:
            raise ValueError("publication budget must be a positive integer")
        self._scope = scope
        self._publish = publish
        self._submit = submit
        self._stop_run = stop_run
        self._on_fault = on_fault
        # The Kernel ACKed this source's baseline (F-2 successor proof).
        self._on_baseline_accepted = on_baseline_accepted
        # The owner's monotonic creation order of its sources (F-2).
        self._source_ordinal = source_ordinal
        self._baseline_accepted = False
        self._fault_reference = fault_reference
        self._budget_ns = budget_ns
        self._clock = monotonic_ns
        # Never held across I/O, a store call or a callback.
        self._lock = Lock()
        self._frames: deque[_Frame] = deque()
        # Current published row of every key of the fixed union.
        self._current: dict[str, dict[str, Any]] = {}
        self._next_sequence = 1
        self._in_flight = False
        self._draining = False
        self._started = False
        self._retired = False
        # The head frame got bb_source_not_ready: retried by check_budget.
        self._awaiting_ready = False
        self._source_epoch: str | None = None
        self._fault: dict[str, Any] | None = None

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

    def pending_sequences(self) -> tuple[int, ...]:
        with self._lock:
            return tuple(frame.sequence for frame in self._frames)

    def start(self, store: ProgramValueStore, keys: tuple[str, ...]) -> None:
        """Subscribe the fixed union and enqueue its baseline as message 1."""

        with self._lock:
            if self._started:
                raise RuntimeError("BB key publisher was already started")
            self._started = True
        store.subscribe_published_keys(
            keys, baseline=self._capture_baseline, on_change=self._on_change
        )
        self._pump()

    def _capture_baseline(self, changes: tuple[PublishedKeyChange, ...]) -> None:
        # Store lock held: bounded capture only.
        failure: str | None = None
        try:
            values = {change.key: _row(change) for change in changes}
        except _Unrepresentable as error:
            values = {}
            failure = str(error)
        with self._lock:
            self._current = values
            self._frames.append(
                _Frame(
                    sequence=1,
                    kind="baseline",
                    recorded_ns=self._clock(),
                    values=dict(values),
                )
            )
            self._next_sequence = 2
        if failure is not None:
            self._submit_or_run(lambda: self.fail(failure))

    def _on_change(self, change: PublishedKeyChange) -> None:
        # Store lock held: record the obligation and enqueue, never block.
        failure: str | None = None
        with self._lock:
            if self._fault is not None or self._retired:
                return
            try:
                row = _row(change)
            except _Unrepresentable as error:
                failure = str(error)
            else:
                self._current[change.key] = row
                tail = self._frames[-1] if self._frames else None
                if (
                    tail is not None
                    and not (self._in_flight and len(self._frames) == 1)
                    and tail.kind == "update"
                    and change.key not in tail.changed
                ):
                    tail.values[change.key] = row
                    tail.changed.add(change.key)
                elif len(self._frames) >= MAXIMUM_PENDING_FRAMES:
                    failure = "pending BB publication frames exceed their bound"
                elif self._next_sequence > MAXIMUM_SEQUENCE:
                    failure = "BB publication sequence is exhausted"
                else:
                    self._frames.append(
                        _Frame(
                            sequence=self._next_sequence,
                            kind="update",
                            recorded_ns=self._clock(),
                            values={change.key: row},
                            changed={change.key},
                        )
                    )
                    self._next_sequence += 1
        if failure is not None:
            # Program Stop may block: contain off the store lock.
            self._submit_or_run(lambda: self.fail(failure))
            return
        self._pump()

    def _submit_or_run(self, work: Callable[[], None]) -> None:
        try:
            self._submit(work)
        except Exception:
            work()

    def _pump(self) -> None:
        with self._lock:
            if (
                self._draining
                or self._awaiting_ready
                or self._fault is not None
                or self._retired
            ):
                return
            if not self._frames:
                return
            self._draining = True
        try:
            self._submit(self._drain)
        except Exception as error:
            with self._lock:
                self._draining = False
            # Bind the text now: ``error`` is unbound after this block.
            message = f"BB publication could not be scheduled: {error}"
            self._submit_or_run(lambda: self.fail(message))

    def _request(self, frame: _Frame) -> dict[str, Any]:
        scope = self._scope
        values = frame.values
        return {
            "schema_version": SCHEMA_VERSION,
            "owner_epoch": scope.owner_epoch,
            "program_runner_owner_epoch": scope.program_runner_owner_epoch,
            "claim_generation": scope.claim_generation,
            "program_run_id": scope.program_run_id,
            "program_binding_id": scope.program_binding_id,
            "active_publication_generation": scope.active_publication_generation,
            "cycle_generation": scope.cycle_generation,
            "kind": frame.kind,
            "message_sequence": str(frame.sequence),
            "previous_sequence": str(frame.sequence - 1),
            "values": [dict(values[name]) for name in sorted(values)],
        }

    def _drain(self) -> None:
        while True:
            with self._lock:
                if self._fault is not None or self._retired or not self._frames:
                    self._draining = False
                    return
                frame = self._frames[0]
                self._in_flight = True
                request = self._request(frame)
            try:
                response = self._publish(request)
                status = response.get("status")
                source_epoch = response.get("source_epoch")
                if response.get("command") != COMMAND or status not in {
                    "accepted", "superseded"
                }:
                    raise ValueError("BB publication reply does not answer this frame")
                if status == "accepted" and (
                    response.get("program_run_id") != self._scope.program_run_id
                    or response.get("owner_epoch") != self._scope.owner_epoch
                    or response.get("program_binding_id") != self._scope.program_binding_id
                    or response.get("active_publication_generation")
                    != self._scope.active_publication_generation
                    or response.get("cycle_generation") != self._scope.cycle_generation
                    or response.get("accepted_sequence") != str(frame.sequence)
                    or not isinstance(source_epoch, str)
                    or not source_epoch
                ):
                    raise ValueError("BB publication reply does not ACK this frame")
            except Exception as error:
                with self._lock:
                    retired = self._retired
                    not_ready = (
                        not retired
                        and getattr(error, "code", None) == SOURCE_NOT_READY_CODE
                    )
                    if not_ready:
                        # Definitely not admitted: keep the frame for retry.
                        self._awaiting_ready = True
                    if retired or not_ready:
                        self._in_flight = False
                        self._draining = False
                if retired:
                    # This sender already retired the source itself: a stale
                    # or superseded refusal is recorded, never a fault.
                    _LOGGER.info(
                        "BB publication %s for a retired source answered %s: run=%s",
                        frame.sequence,
                        getattr(error, "code", type(error).__name__),
                        self._scope.program_run_id,
                    )
                    return
                if not_ready:
                    details = getattr(error, "details", None)
                    _LOGGER.info(
                        "BB publication %s: source not ready yet (reason=%s); retrying",
                        frame.sequence,
                        details.get("reason") if isinstance(details, Mapping) else None,
                    )
                    return
                # Unknown or refused: never resend, never renew. The owner's
                # typed code and details.reason stay visible in the fault.
                details = getattr(error, "details", None)
                reason = details.get("reason") if isinstance(details, Mapping) else None
                self.fail(
                    f"BB publication {frame.sequence} was not acknowledged: "
                    f"code={getattr(error, 'code', type(error).__name__)} "
                    f"reason={reason}: {error}"
                )
                return
            if status == "superseded":
                # Confirmed retirement of this old occurrence: drop only this
                # exact obligation; its source_epoch is the replacement's.
                with self._lock:
                    if self._frames and self._frames[0] is frame:
                        self._frames.popleft()
                    self._in_flight = False
                _LOGGER.info(
                    "BB publication %s superseded: run=%s cycle=%s",
                    frame.sequence,
                    self._scope.program_run_id,
                    self._scope.cycle_generation,
                )
                continue
            with self._lock:
                if self._source_epoch not in {None, source_epoch}:
                    failure = "BB publication source epoch changed within the Run"
                else:
                    failure = None
                    self._source_epoch = source_epoch
                    if self._frames and self._frames[0] is frame:
                        self._frames.popleft()
                    first_ack = frame.kind == "baseline" and not self._baseline_accepted
                    if first_ack:
                        self._baseline_accepted = True
                self._in_flight = False
            if failure is not None:
                self.fail(failure)
                return
            if first_ack and self._on_baseline_accepted is not None:
                self._on_baseline_accepted()

    def check_budget(self) -> bool:
        """Latch the fault when the oldest obligation outlived the budget.

        Runs on the liveness receiver thread; it never waits on the sender.
        Within the budget, a frame refused as not ready is retried here.
        """

        now = self._clock()
        with self._lock:
            if self._fault is not None or self._retired or not self._frames:
                return False
            oldest = self._frames[0]
            retry = self._awaiting_ready
            if now - oldest.recorded_ns <= self._budget_ns:
                self._awaiting_ready = False
                if not retry:
                    return False
        if now - oldest.recorded_ns <= self._budget_ns:
            self._pump()
            return False
        self.fail(
            f"BB publication {oldest.sequence} was not acknowledged within the "
            "command budget" + (" (source not ready)" if retry else "")
        )
        return True

    def fail(self, message: str) -> None:
        """Latch once: stop publishing, request Program Stop, report the fault."""

        error = publisher_fault_error(message)
        with self._lock:
            if self._fault is not None or self._retired:
                return
            self._fault = error
            self._frames.clear()
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

    def declare_lost(self) -> bool:
        """After a fault: make the Kernel's source of this exact scope
        unavailable, never stale-but-usable (F-2). One update frame with a
        sequence gap is the Kernel's existing exact-sender loss
        (`program_bb_sequence_gap`); every BB-backed rule input of this source
        then fails closed until a new source (Reset or Run) replaces it.
        True only when the Kernel answered with that typed loss."""

        with self._lock:
            if self._fault is None or not self._current:
                return False
            name = min(self._current)
            request = self._request(
                _Frame(
                    sequence=MAXIMUM_SEQUENCE,
                    kind="update",
                    recorded_ns=self._clock(),
                    values={name: dict(self._current[name])},
                )
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
        Runner retires this source itself and drops its pending frames; the
        replacement baseline carries the current values (run-only keys at
        their declared `initial` after Reset)."""

        with self._lock:
            self._retired = True
            self._frames.clear()


__all__ = [
    "COMMAND",
    "FAULT_CODE",
    "MAXIMUM_PENDING_FRAMES",
    "ProgramBlackboardKeyPublisher",
    "ProgramBlackboardPublicationScope",
    "SCHEMA_VERSION",
    "SOURCE_NOT_READY_CODE",
    "publisher_fault_error",
]
