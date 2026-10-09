"""Bounded receiving decisions; all SDK effects run outside this object's lock."""
from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from threading import Lock
from typing import Callable

from .frame import CommandFrame, GoalKey, GuardConfig, MAX_KEYS, uint64, validate_frame, validate_key


class OwnedOperation(Enum):
    EXECUTING = "executing"
    BRAKING = "braking"
    HOLDING = "holding"


class BoundaryOutcome(Enum):
    RETURNED = "returned"
    FAILED = "failed"
    UNKNOWN = "unknown"


class FenceBoundaryOutcome(Enum):
    RETURNED = "returned"
    FENCED = "fenced"
    DRAINED = "drained"
    FAILED = "failed"
    UNKNOWN = "unknown"


@dataclass(frozen=True)
class EffectToken:
    frame: CommandFrame | None
    key: GoalKey
    occurrence: str
    fence_sequence: int | None
    attempt: int


@dataclass(frozen=True)
class BoundaryFact:
    token: EffectToken
    outcome: BoundaryOutcome | FenceBoundaryOutcome
    # SDK return is not a qualified no-future-effect or rest certificate.


@dataclass(frozen=True)
class GuardStatus:
    closed: bool
    trip_reason: str | None
    owned_key: GoalKey | None
    owned_operation: OwnedOperation | None
    retained_keys: int
    in_flight: bool
    returned_boundary: BoundaryFact | None


@dataclass
class _Record:
    sequence: int = 0
    progress_ns: int | None = None
    fence_sequence: int | None = None
    returned_fence: BoundaryFact | None = None


class CommandGuard:
    """One configured occurrence, four outstanding keys, one snapshot/attempt.

    Trusted owner ingress admits/arms keys and observes/releases retirement.
    Frames cannot create keys, arm, clear trips or repeat effect attempts.
    No SDK, ROS, timer, lifecycle, recovery, or physical-rest owner lives here.
    """

    def __init__(self, config: GuardConfig, local_monotonic_clock: Callable[[], int]):
        self.config = config
        self._local_clock = local_monotonic_clock
        self._lock = Lock()
        self._records: dict[GoalKey, _Record] = {}
        self._owned: GoalKey | None = None
        self._mode: OwnedOperation | None = None
        self._latest: CommandFrame | None = None
        self._flight: EffectToken | None = None
        self._returned: BoundaryFact | None = None
        self._attempt = 0
        self._clock_ns = 0
        self._closed = False
        self._trip: str | None = None

    def _trip_locked(self, reason: str) -> None:
        if self._trip is None:
            self._trip = reason
        self._closed = True
        self._latest = None

    def _check_clock(self) -> bool:
        # The trusted bounded clock read is serialized with its observation.
        # Caller-side samples can legitimately acquire this lock out of order.
        now_ns = self._local_clock()
        if type(now_ns) is not int or now_ns < self._clock_ns:
            self._trip_locked("local_clock_regression")
            return False
        self._clock_ns = now_ns
        if self._owned is not None:
            progress = self._records[self._owned].progress_ns
            if progress is not None and now_ns - progress >= self.config.max_command_age_ns:
                self._trip_locked("command_age_expired")
        return not self._closed

    def _reserve(self, key: GoalKey) -> _Record | None:
        if key not in self._records:
            if len(self._records) == MAX_KEYS:
                self._trip_locked("key_capacity_exhausted")
                return None
            self._records[key] = _Record()
        return self._records[key]

    def admit(self, key: GoalKey) -> bool:
        """Trusted owner only; its occurrence/sequence ingress handles replay."""
        validate_key(key, self.config)
        with self._lock:
            if self._closed:
                return False
            record = self._reserve(key)
            return record is not None and record.fence_sequence is None

    def arm(self, key: GoalKey, operation: OwnedOperation) -> bool:
        """Owned execution/braking/holding, including zero scale; never first-frame arm."""
        validate_key(key, self.config)
        if type(operation) is not OwnedOperation:
            raise ValueError("unknown owned operation")
        with self._lock:
            if not self._check_clock():
                return False
            record = self._records.get(key)
            if record is None or record.fence_sequence is not None:
                return False
            if self._owned != key:
                self._latest = None
                self._owned = key
                record.progress_ns = self._clock_ns
            self._mode = operation
            return True

    def offer(self, frame: CommandFrame, received_local_ns: int) -> bool:
        """Complete decode precedes this call; receipt is NRT callback-entry time."""
        validate_frame(frame, self.config)
        with self._lock:
            if not self._check_clock():
                return False
            now_ns = self._clock_ns
            record = self._records.get(frame.key)
            if record is None or frame.key != self._owned or record.fence_sequence is not None:
                return False
            if frame.producer_sequence <= record.sequence:
                return False
            if (type(received_local_ns) is not int or received_local_ns < 0 or
                    received_local_ns > now_ns or record.progress_ns is None):
                self._trip_locked("invalid_local_receipt_time")
                return False
            # Decode completion can legitimately reorder callback-entry samples.
            # Older receipt evidence cannot renew age, but is not clock reversal.
            if received_local_ns < record.progress_ns:
                return False
            if now_ns - received_local_ns >= self.config.max_command_age_ns:
                self._trip_locked("received_frame_expired")
                return False
            record.sequence = frame.producer_sequence
            record.progress_ns = received_local_ns
            self._latest = frame
            return True

    def monitor(self) -> GuardStatus:
        """Externally serviced local monotonic check, never a target write."""
        with self._lock:
            self._check_clock()
            return self._status_locked()

    def close_occurrence(self) -> None:
        with self._lock:
            self._trip_locked("occurrence_closed")

    def request_fence(self, key: GoalKey, request_sequence: int) -> bool:
        """Reserved request; closes/discards immediately but returns no receipt."""
        validate_key(key, self.config)
        uint64(request_sequence, nonzero=True)
        with self._lock:
            record = self._reserve(key)
            if record is None:
                return False
            if record.fence_sequence not in (None, request_sequence):
                return False
            record.fence_sequence = request_sequence
            if self._latest is not None and self._latest.key == key:
                self._latest = None
            return True

    def _begin(self, frame: CommandFrame | None, key: GoalKey, fence: int | None) -> EffectToken:
        self._attempt += 1
        token = EffectToken(frame, key, self.config.occurrence, fence, self._attempt)
        self._flight = token
        return token

    def begin_effect(self) -> EffectToken | None:
        """Reserve immediately before the sole simulator-thread SDK invocation.

        A later fence may overtake an already reserved attempt, but cannot
        acknowledge retirement until it returns and target handling completes.
        The token is not effect evidence or permission to queue another writer.
        """
        with self._lock:
            if not self._check_clock() or self._flight is not None or self._latest is None:
                return None
            frame = self._latest
            if self._records[frame.key].fence_sequence is not None:
                self._latest = None
                return None
            self._latest = None
            return self._begin(frame, frame.key, None)

    def _finish(self, token: EffectToken, outcome: BoundaryOutcome | FenceBoundaryOutcome) -> BoundaryFact:
        if self._flight is not token:
            raise ValueError("stale or foreign effect token")
        fact = BoundaryFact(token, outcome)
        self._returned = fact
        self._flight = None
        if token.fence_sequence is not None:
            self._records[token.key].returned_fence = fact
        elif outcome is not BoundaryOutcome.RETURNED:
            self._trip_locked("frame_effect_" + outcome.value)
        return fact

    def finish_effect(self, token: EffectToken, outcome: BoundaryOutcome) -> BoundaryFact:
        """Exact SDK return; does not assert delivery/rest/final realization."""
        if type(outcome) is not BoundaryOutcome or token.fence_sequence is not None:
            raise ValueError("invalid frame boundary outcome or token")
        with self._lock:
            return self._finish(token, outcome)

    def finish_fence_effect(self, token: EffectToken, outcome: FenceBoundaryOutcome) -> BoundaryFact:
        """Only the sole local boundary adapter supplies actual fence facts.

        SDK return permits RETURNED only. FENCED/DRAINED require the adapter's
        qualified rejection and persistent-target contract, never absence of
        an exception. This module cannot qualify that physical integration.
        """
        if type(outcome) is not FenceBoundaryOutcome or token.fence_sequence is None:
            raise ValueError("invalid fence boundary outcome or token")
        with self._lock:
            return self._finish(token, outcome)

    def begin_fence_effect(self, key: GoalKey) -> EffectToken | None:
        """Reserve configured target handling outside the lock, even after trip."""
        with self._lock:
            self._check_clock()
            record = self._records.get(key)
            if (self._flight is not None or record is None or record.fence_sequence is None or
                    record.returned_fence is not None):
                return None
            return self._begin(None, key, record.fence_sequence)

    def read_fence_fact(self, key: GoalKey) -> BoundaryFact | None:
        """Retained exact-key observation; reading never repeats target handling."""
        with self._lock:
            record = self._records.get(key)
            return record.returned_fence if record is not None else None

    def release_after_retirement(self, key: GoalKey, observed_token: EffectToken) -> bool:
        """Trusted owner observes our actual fence fact before releasing storage.

        Frame SDK return or fence RETURNED cannot authorize retirement. Removed
        keys stay unadmitted to frames. Existing trusted occurrence/sequence
        ingress must never reauthorize a retired identical key.
        """
        with self._lock:
            record = self._records.get(key)
            fact = record.returned_fence if record is not None else None
            if (fact is None or fact.token is not observed_token or
                    fact.outcome not in (FenceBoundaryOutcome.FENCED, FenceBoundaryOutcome.DRAINED) or
                    (self._flight is not None and self._flight.key == key)):
                return False
            del self._records[key]
            if self._owned == key:
                self._owned = None
                self._mode = None
            return True

    def _status_locked(self) -> GuardStatus:
        return GuardStatus(self._closed, self._trip, self._owned, self._mode,
                           len(self._records), self._flight is not None, self._returned)

    def status(self) -> GuardStatus:
        with self._lock:
            return self._status_locked()
