"""Timestamp-only receiving guard; no lifecycle or motion ownership."""
from __future__ import annotations

from dataclasses import dataclass
from threading import Lock
from typing import Callable

from .frame import JointCommand, validate_command


@dataclass(frozen=True)
class GuardConfig:
    joint_names: tuple[str, ...]
    max_command_age_ns: int

    def __post_init__(self) -> None:
        if (type(self.joint_names) is not tuple or not self.joint_names or
                any(type(name) is not str or not name for name in self.joint_names) or
                len(set(self.joint_names)) != len(self.joint_names)):
            raise ValueError("configured joints must be nonempty and unique")
        if type(self.max_command_age_ns) is not int or self.max_command_age_ns <= 0:
            raise ValueError("command age must be explicitly configured and positive")


@dataclass(frozen=True)
class GuardStatus:
    authorized: bool
    tripped: bool
    reason: str | None
    authorization_stamp_ns: int | None
    last_command_stamp_ns: int | None
    progress_receipt_ns: int | None


class CommandGuard:
    """One immutable pending command, standard producer stamps, local receipt age.

    The upstream Reset owner supplies the controller-stamped hold cutoff after
    actual old-control cutover. authorize() is processed on the sole simulator
    writer thread, never a ROS callback racing an already reserved SDK effect.
    This class establishes no SDK application, receipt or physical-rest fact.
    """

    def __init__(self, config: GuardConfig, local_monotonic_clock: Callable[[], int]) -> None:
        if not callable(local_monotonic_clock):
            raise TypeError("local monotonic clock required")
        self.config = config
        self._clock = local_monotonic_clock
        self._lock = Lock()
        self._clock_ns: int | None = None
        self._authorization: int | None = None
        self._last_stamp: int | None = None
        self._progress: int | None = None
        self._latest: JointCommand | None = None
        self._authorized = False
        self._tripped = False
        self._reason: str | None = None

    def _trip(self, reason: str) -> None:
        self._authorized = False
        self._latest = None
        if not self._tripped:
            self._reason = reason
        self._tripped = True

    def _now(self) -> int | None:
        # The injected reader is bounded, local and contains no ROS/SDK/I/O.
        # Sampling under this short lock avoids reversed thread sampling order.
        now = self._clock()
        if type(now) is not int or now < 0 or (self._clock_ns is not None and now < self._clock_ns):
            self._trip("local_clock_invalid_or_regressed")
            return None
        self._clock_ns = now
        return now

    def _expire(self, now: int) -> None:
        if (self._authorized and self._progress is not None and
                now - self._progress >= self.config.max_command_age_ns):
            self._trip("command_age_expired")

    def authorize(self, controller_hold_stamp_ns: int) -> bool:
        """Accept only a newer trusted Reset/startup cutoff; this is no hold proof."""
        if type(controller_hold_stamp_ns) is not int:
            raise ValueError("authorization requires an exact controller timestamp")
        with self._lock:
            now = self._now()
            if now is None or (self._authorization is not None and
                               controller_hold_stamp_ns <= self._authorization):
                return False
            self._authorization = controller_hold_stamp_ns
            if self._last_stamp is None or self._last_stamp < controller_hold_stamp_ns:
                self._last_stamp = controller_hold_stamp_ns
            self._latest = None
            self._progress = now
            self._authorized = True
            self._tripped = False
            self._reason = None
            return True

    def offer(self, command: JointCommand, received_local_ns: int) -> bool:
        validate_command(command, self.config.joint_names)
        with self._lock:
            now = self._now()
            if now is None:
                return False
            self._expire(now)  # A queued frame never hides an expired gap.
            if not self._authorized:
                return False
            if (self._last_stamp is not None and command.stamp_ns <= self._last_stamp):
                return False
            if (type(received_local_ns) is not int or received_local_ns < 0 or
                    received_local_ns > now):
                self._trip("invalid_local_receipt_time")
                return False
            if self._progress is not None and received_local_ns < self._progress:
                return False  # Earlier callbacks completing later are stale evidence.
            if now - received_local_ns >= self.config.max_command_age_ns:
                self._trip("received_command_expired")
                return False
            self._last_stamp = command.stamp_ns
            self._progress = received_local_ns
            self._latest = command
            return True

    def take_latest(self) -> JointCommand | None:
        """Sole writer consumes immediately; never queue this snapshot elsewhere."""
        with self._lock:
            now = self._now()
            if now is None:
                return None
            self._expire(now)
            if not self._authorized:
                return None
            command, self._latest = self._latest, None
            return command

    def monitor(self) -> GuardStatus:
        with self._lock:
            now = self._now()
            if now is not None:
                self._expire(now)
            return GuardStatus(self._authorized, self._tripped, self._reason,
                               self._authorization, self._last_stamp, self._progress)
