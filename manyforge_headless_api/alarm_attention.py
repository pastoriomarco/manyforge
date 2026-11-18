"""Publish the Alarm Lifecycle Owner's motion attention to the native Kernel.

Headless owns the alarm lifecycle; the Kernel owns admission. This
adapter only carries the owner's immutable, generation-fenced projection to
the Kernel's existing cell-admission fact port. It evaluates nothing: the
Kernel applies the resource test at admission and at the final
pre-dispatch recheck, and fails closed when the projection is missing or
older than its freshness bound.

Timing contract. Publication runs on its own thread, which
does nothing else. Every period ``P`` it starts one publication:

* the owner read copies the owner's published projection; it takes neither
  the Headless core coordination lock nor a source refresh, so no concurrent
  core call can delay it (``HeadlessApiCore::OperatorAlarmMotionAttention``);
* connect fails at once when the Kernel's socket is missing, closed or full;
  the wait for the Kernel's acceptance has no time limit (OD-307). It ends by
  the answer, by the socket closing when the host exits, or by the
  publisher's own shutdown (``CellAdmissionClient.publish_operator_alarm_attention``).

No timer of the publisher judges staleness. The Kernel freshness bound ``F``
(``kOperatorAlarmAttentionMaximumSnapshotAgeNs``) alone does: while the
Kernel answers, consecutive receipts are about ``P`` apart (an answer later
than ``P`` delays the next start, which then follows at once), well inside

    P = 0.25 s, F = 2 s = 8 P

and a Kernel that does not answer within ``F`` lets its copy age out, so its
admission fails closed by its own judgment. Shutdown writes the publisher's
stop pipe, which ends the wait of the one outstanding exchange. The real-path qualification
(process_tests/test_operator_alarm_motion_attention.py) prints the measured
receipt intervals under core-lock contention, delayed Kernel responses and
shutdown during an outstanding exchange.

Kernel-held copy. The Kernel answers each accepted publication with the
generation it now retains. The publisher keeps that acknowledged
projection's identity (``kernel_held_attention``), and Headless names it as
``kernel_attention`` in its alarm snapshots, so a client can wait for the
Kernel's own admission state (reconciled, for the active configuration)
instead of retrying refused motion while the Kernel's copy lags the owner.
"""

from __future__ import annotations

from collections.abc import Callable, Mapping
from dataclasses import dataclass
import logging
import os
import threading
import time
from typing import Any, Protocol


PUBLICATION_PERIOD_SECONDS = 0.25
#: The Kernel's freshness bound F (``kOperatorAlarmAttentionMaximumSnapshotAgeNs``).
KERNEL_FRESHNESS_SECONDS = 2.0


class _AttentionSource(Protocol):
    def operator_alarm_motion_attention(self) -> dict[str, Any]: ...


class _AttentionSink(Protocol):
    def publish_operator_alarm_attention(
        self, projection: Mapping[str, Any], *, stop_fd: int | None
    ) -> dict[str, Any]: ...


@dataclass(frozen=True)
class AttentionPublicationCounts:
    """Outcomes since start: acknowledged, failed."""

    published: int
    failed: int


@dataclass(frozen=True)
class KernelHeldAttention:
    """The motion-attention projection the Kernel acknowledged retaining.

    The Kernel answers each accepted publication with the generation it now
    retains, and one generation names exactly one content, so the acknowledged projection is the Kernel's held copy.
    Only its admission-relevant identity is kept: whether it is reconciled
    and which active configuration it was judged against. Clients use it to
    anticipate admission without polling the Kernel: a Run is
    admitted only once the Kernel holds a reconciled projection of the active
    configuration, which lags the owner by about ``P`` plus the Kernel's
    answer time.
    """

    generation: int
    reconciled: bool
    #: ``{"retained_bundle_id", "graph_revision"}``, or None before the owner
    #: observed a configuration.
    active_configuration: dict[str, str] | None
    #: Monotonic time the Kernel's acknowledgement was received.
    acknowledged_at: float

    def projection(self) -> dict[str, Any]:
        """The public ``kernel_attention`` member (no receipt time)."""
        return {
            "active_configuration": (
                None
                if self.active_configuration is None
                else dict(self.active_configuration)
            ),
            "generation": self.generation,
            "reconciled": self.reconciled,
        }


def _held_from(projection: Mapping[str, Any], at: float) -> KernelHeldAttention | None:
    configuration = projection.get("active_configuration")
    if configuration is not None:
        if not isinstance(configuration, Mapping):
            return None
        bundle = configuration.get("retained_bundle_id")
        graph = configuration.get("graph_revision")
        if not isinstance(bundle, str) or not isinstance(graph, str):
            return None
        configuration = {"graph_revision": graph, "retained_bundle_id": bundle}
    generation = projection.get("generation")
    reconciled = projection.get("reconciled")
    if (
        type(generation) is not int
        or type(reconciled) is not bool
    ):
        return None
    return KernelHeldAttention(
        generation=generation,
        reconciled=reconciled,
        active_configuration=configuration,
        acknowledged_at=at,
    )


class OperatorAlarmAttentionPublisher:
    """Dedicated periodic publisher of the owner's motion-attention view."""

    def __init__(
        self,
        core: _AttentionSource,
        kernel: _AttentionSink,
        *,
        period_seconds: float = PUBLICATION_PERIOD_SECONDS,
    ) -> None:
        if not 0.0 < 4.0 * period_seconds <= KERNEL_FRESHNESS_SECONDS:
            raise ValueError(
                "alarm attention publication timing does not fit the Kernel "
                "freshness bound"
            )
        self._core = core
        self._kernel = kernel
        self._period_seconds = period_seconds
        self._stop = threading.Event()
        # The stop pipe (OD-296 pattern): created by start(), written by
        # shutdown(), polled with the socket by the outstanding exchange, and
        # closed once the publisher thread has ended.
        self._stop_read_fd: int | None = None
        self._stop_write_fd: int | None = None
        self._thread: threading.Thread | None = None
        self._failing = False
        self._counts_lock = threading.Lock()
        self._published = 0
        self._failed = 0
        self._held: KernelHeldAttention | None = None
        self._held_observer: Callable[[], None] | None = None

    def set_kernel_held_observer(self, observer: Callable[[], None] | None) -> None:
        """Observe a change of the Kernel-held projection's identity.

        Called on the publisher thread when the acknowledged
        generation, reconciliation or active configuration changes, or when
        the held copy is no longer known to be current; never for an
        unchanged heartbeat. The observer must not block.
        """
        if observer is not None and not callable(observer):
            raise TypeError("kernel-held attention observer must be callable")
        self._held_observer = observer

    def kernel_held_attention(self) -> dict[str, Any] | None:
        """The projection the Kernel last acknowledged, while still current.

        None before the first acknowledgement, or once no acknowledgement has
        arrived within the Kernel's freshness bound ``F``, after which the
        Kernel itself treats its copy as stale.
        """
        with self._counts_lock:
            held = self._held
        if held is None or time.monotonic() - held.acknowledged_at > KERNEL_FRESHNESS_SECONDS:
            return None
        return held.projection()

    def _record_held(self, held: KernelHeldAttention | None) -> None:
        with self._counts_lock:
            previous = self._held
            self._held = held
        before = None if previous is None else previous.projection()
        after = None if held is None else held.projection()
        if before == after:
            return
        observer = self._held_observer
        if observer is None:
            return
        try:
            observer()
        except Exception:
            logging.getLogger(__name__).exception(
                "kernel-held attention observer failed"
            )

    @property
    def counts(self) -> AttentionPublicationCounts:
        with self._counts_lock:
            return AttentionPublicationCounts(self._published, self._failed)

    def publish_once(self) -> bool:
        """Read and publish the current projection; wait for the Kernel.

        Returns False when the publication failed or was ended by the
        publisher's shutdown; the next period publishes the then-current
        projection.
        """
        try:
            projection = self._core.operator_alarm_motion_attention()
            self._kernel.publish_operator_alarm_attention(
                projection, stop_fd=self._stop_read_fd
            )
        except Exception:
            if self._stop.is_set():
                return False  # ended by shutdown: no failure
            # The Kernel fails closed once the last publication ages out; a
            # transient owner or transport failure is retried next period.
            now = time.monotonic()
            with self._counts_lock:
                self._failed += 1
                held = self._held
            if (
                held is not None
                and now - held.acknowledged_at > KERNEL_FRESHNESS_SECONDS
            ):
                # The Kernel's copy is stale by now: no longer report it.
                self._record_held(None)
            if not self._failing:
                self._failing = True
                logging.getLogger(__name__).warning(
                    "operator-alarm attention publication failed", exc_info=True
                )
            return False
        with self._counts_lock:
            self._published += 1
        # The Kernel acknowledged retaining exactly this generation (the
        # transport verifies the acknowledgement names it).
        self._record_held(_held_from(projection, time.monotonic()))
        if self._failing:
            self._failing = False
            logging.getLogger(__name__).info(
                "operator-alarm attention publication recovered"
            )
        return True

    def start(self) -> None:
        if self._thread is not None:
            return
        self._stop.clear()
        self._stop_read_fd, self._stop_write_fd = os.pipe2(
            os.O_CLOEXEC | os.O_NONBLOCK
        )
        self._thread = threading.Thread(
            target=self._run, name="manyforge-alarm-attention-publisher"
        )
        self._thread.start()

    def shutdown(self) -> None:
        self._stop.set()
        if self._stop_write_fd is not None:
            # Ends the wait of the one outstanding exchange (OD-307).
            try:
                os.write(self._stop_write_fd, b"\0")
            except BlockingIOError:
                pass  # Already signalled.
        thread = self._thread
        if thread is not None:
            thread.join()
        self._thread = None
        for descriptor in (self._stop_read_fd, self._stop_write_fd):
            if descriptor is not None:
                os.close(descriptor)
        self._stop_read_fd = self._stop_write_fd = None

    def _run(self) -> None:
        next_at = time.monotonic()
        while not self._stop.is_set():
            self.publish_once()
            # Fixed-rate schedule. After a publication answered later than
            # its period, publish again at once rather than bursting to catch
            # up.
            now = time.monotonic()
            next_at = max(next_at + self._period_seconds, now)
            self._stop.wait(next_at - now)


__all__ = [
    "AttentionPublicationCounts",
    "KERNEL_FRESHNESS_SECONDS",
    "KernelHeldAttention",
    "OperatorAlarmAttentionPublisher",
    "PUBLICATION_PERIOD_SECONDS",
]
