"""Small receiving adapter; the host owns motion, Reset and incident handling."""
from __future__ import annotations

from dataclasses import dataclass
import json
from threading import Lock
from typing import Any, Callable

from .frame import project_joint_state, stamp_ns
from .guard import CommandGuard, GuardConfig, GuardStatus


@dataclass(frozen=True)
class RuntimeConfig:
    articulation_prim_path: str
    joint_names: tuple[str, ...]
    command_topic: str
    authorization_topic: str
    guard_status_topic: str
    max_command_age_ns: int

    def __post_init__(self) -> None:
        GuardConfig(self.joint_names, self.max_command_age_ns)
        for value in (self.articulation_prim_path, self.command_topic,
                      self.authorization_topic, self.guard_status_topic):
            if type(value) is not str or not value:
                raise ValueError("explicit prim and ROS topics required")


def status_fields(status: GuardStatus, joint_names: tuple[str, ...]) -> tuple[tuple[str, str], ...]:
    """Exact DiagnosticStatus values; an absent authorization is not stamp zero."""
    state = "tripped" if status.tripped else "open" if status.authorized else "closed"
    fields = [("state", state),
              ("joint_names", json.dumps(joint_names, separators=(",", ":")))]
    if status.authorization_stamp_ns is not None:
        seconds, nanoseconds = divmod(status.authorization_stamp_ns, 1_000_000_000)
        fields.extend((("authorized_after.sec", str(seconds)),
                       ("authorized_after.nanosec", str(nanoseconds))))
    if status.reason is not None:
        fields.append(("reason", status.reason))
    return tuple(fields)


class GuardRuntime:
    """One latest Time, one existing guard frame, one selected SDK writer.

    Publication is serialized separately from guard state. The monitor only
    latches local facts; publish_status runs on the ROS owner, never that monitor.
    No lock covers an SDK call. Publication exceptions have unknown delivery;
    they close local admission and remain evidence, with no automatic retry.
    """

    def __init__(self, config: RuntimeConfig, local_clock: Callable[[], int],
                 publish_status: Callable[[GuardStatus], None]) -> None:
        self.config = config
        self.guard = CommandGuard(GuardConfig(config.joint_names, config.max_command_age_ns),
                                  local_clock)
        self._clock = local_clock
        self._publish = publish_status
        self._publication_lock = Lock()
        self._admission_lock = Lock()
        self._writer: Callable[[tuple[float, ...]], None] | None = None
        self._admitting = False
        self._pending_authorization: int | None = None
        self._last_published: tuple[tuple[str, str], ...] | None = None
        self.publication_error: Exception | None = None
        self.failed_statuses: tuple[GuardStatus, ...] = ()

    def bind_writer(self, writer: Callable[[tuple[float, ...]], None]) -> None:
        """Sole simulator thread calls after valid prim/SDK/exact DOF binding.

        The authored scene cutover is an integration prerequisite, not a new
        boolean ownership certificate or dynamically scanned runtime fact.
        """
        if not callable(writer):
            raise TypeError("bound SDK writer required")
        with self._admission_lock:
            if self.publication_error is not None:
                raise RuntimeError("runtime publication failed; explicit lifecycle restart required")
            self._writer = writer
            self._admitting = True

    @property
    def ready(self) -> bool:
        with self._admission_lock:
            return self._admitting and self._writer is not None

    def receive_authorization(self, message: Any) -> bool:
        value = stamp_ns(message)
        with self._admission_lock:
            if not self._admitting:
                return False
            if self._pending_authorization is None or value > self._pending_authorization:
                self._pending_authorization = value
            return True

    def receive_command(self, message: Any) -> bool:
        received = self._clock()  # Entry time is distinct from decode completion.
        command = project_joint_state(message, self.config.joint_names)
        if command is None:
            return False
        with self._admission_lock:
            if not self._admitting:
                return False
            return self.guard.offer(command, received)

    def monitor(self) -> GuardStatus:
        """No publication lock or DDS call; trip can latch during blocked I/O."""
        return self.guard.monitor()

    def _emit(self, statuses: tuple[GuardStatus, ...]) -> None:
        for index, status in enumerate(statuses):
            identity = status_fields(status, self.config.joint_names)
            if identity == self._last_published:
                continue
            try:
                self._publish(status)
            except Exception as error:
                self.publication_error = error
                self.failed_statuses = statuses[index:]
                # No claim the failed publish was not delivered. Local closure
                # preserves the known trip, exact T and command ordering floor.
                with self._admission_lock:
                    self._admitting = False
                    self._pending_authorization = None
                    self.guard.close()
                raise
            self._last_published = identity

    def publish_status(self) -> None:
        with self._publication_lock:
            if self.publication_error is not None:
                return  # No silent status retry or new readiness claim.
            self._emit((self.guard.monitor(),))

    def step(self) -> bool:
        """Sole writer processes authorization then consumes immediately.

        A monitor/close may race an already reserved SDK call. Its return is
        weaker than an applied target, and this method claims no interrupt/rest.
        """
        with self._publication_lock:
            with self._admission_lock:
                if not self._admitting or self._writer is None:
                    return False
                authorization, self._pending_authorization = self._pending_authorization, None
                writer = self._writer
                if authorization is not None:
                    _, prior, result = self.guard.authorize_with_status(authorization)
                else:
                    prior = result = self.guard.monitor()
            self._emit((prior, result))
            command = self.guard.take_latest()
            # Final age check is AFTER any potentially blocking publication.
            # No DDS, initialization or extra queue follows this reservation.
        if command is None:
            return False
        writer(command.positions)
        return True  # SDK call returned; not verified application.

    def close(self) -> None:
        # Close admission BEFORE waiting for publication serialization. This
        # cannot wait behind an SDK effect; the already reserved call may finish.
        with self._admission_lock:
            self._admitting = False
            self._pending_authorization = None
            result = self.guard.close()
        with self._publication_lock:
            if self.publication_error is None:
                self._emit((result,))
