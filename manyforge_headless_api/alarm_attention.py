"""Publish the Alarm Lifecycle Owner's motion attention to the native Kernel.

Headless owns the alarm lifecycle; the Kernel owns admission (ADR-0026). This
adapter only carries the owner's immutable, generation-fenced projection to
the Kernel's existing cell-admission fact port. It evaluates nothing: the
Kernel applies the spec 450 §5.2 resource test at admission and at the final
pre-dispatch recheck, and fails closed when the projection is missing or
older than its freshness bound.

Timing contract (spec 530 §11). Publication runs on its own thread, which
does nothing else. Every period ``P`` it starts one publication with a
monotonic deadline ``D`` after its start:

* the owner read copies the owner's published projection; it takes neither
  the Headless core coordination lock nor a source refresh, so no concurrent
  core call can delay it (``HeadlessApiCore::OperatorAlarmMotionAttention``);
* connect, send and receive share the one deadline
  (``CellAdmissionClient.publish_operator_alarm_attention``);
* a publication not acknowledged by its deadline is abandoned and counted,
  and since ``D < P`` it never delays the next period's start.

A change therefore reaches the Kernel within ``P + D`` of its owner
generation, and an unchanged projection is republished as the Kernel's
heartbeat (identical content refreshes the Kernel's receipt time). While the
path is healthy consecutive Kernel receipts are at most ``P + D`` apart, which
the Kernel freshness bound ``F`` exceeds with margin:

    P = 0.25 s, D = 0.2 s, P + D = 0.45 s < F = 2 s = 8 P

(``kOperatorAlarmAttentionMaximumSnapshotAgeNs``), so ``F`` tolerates
``floor((F - D) / P) - 1 = 6`` consecutive abandoned or failed publications
before admission fails closed. Shutdown waits for at most one outstanding
publication, i.e. at most ``D``. The real-path qualification
(process_tests/test_operator_alarm_motion_attention.py) prints the measured
receipt intervals under core-lock contention, delayed Kernel responses and
shutdown during an outstanding exchange.

Kernel-held copy. The Kernel answers each accepted publication with the owner
epoch and generation it now retains. The publisher keeps that acknowledged
projection's identity (``kernel_held_attention``), and Headless names it as
``kernel_attention`` in its alarm snapshots, so a client can wait for the
Kernel's own admission state (reconciled, for the active configuration)
instead of retrying refused motion during the ``P + D`` lag.
"""

from __future__ import annotations

from collections.abc import Callable, Mapping
from dataclasses import dataclass
import logging
import threading
import time
from typing import Any, Protocol


PUBLICATION_PERIOD_SECONDS = 0.25
#: Overall monotonic deadline D of one publication: owner read, connect,
#: send and receive together.
PUBLICATION_DEADLINE_SECONDS = 0.2
#: The Kernel's freshness bound F (``kOperatorAlarmAttentionMaximumSnapshotAgeNs``).
KERNEL_FRESHNESS_SECONDS = 2.0
#: Shutdown slack beyond one outstanding publication's deadline.
_SHUTDOWN_SLACK_SECONDS = 0.5


class _AttentionSource(Protocol):
    def operator_alarm_motion_attention(self) -> dict[str, Any]: ...


class _AttentionSink(Protocol):
    def publish_operator_alarm_attention(
        self, projection: Mapping[str, Any], *, deadline: float
    ) -> dict[str, Any]: ...


@dataclass(frozen=True)
class AttentionPublicationCounts:
    """Outcomes since start: acknowledged, abandoned at D, failed before D."""

    published: int
    abandoned: int
    failed: int


@dataclass(frozen=True)
class KernelHeldAttention:
    """The motion-attention projection the Kernel acknowledged retaining.

    The Kernel answers each accepted publication with the owner epoch and
    generation it now retains, and one generation of one epoch names exactly
    one content, so the acknowledged projection is the Kernel's held copy.
    Only its admission-relevant identity is kept: whether it is reconciled
    and which active configuration it was judged against. Clients use it to
    anticipate admission (spec 530 §11) without polling the Kernel: a Run is
    admitted only once the Kernel holds a reconciled projection of the active
    configuration, which lags the owner by up to ``P + D``.
    """

    owner_epoch: str
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
            "owner_epoch": self.owner_epoch,
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
    owner_epoch = projection.get("owner_epoch")
    generation = projection.get("generation")
    reconciled = projection.get("reconciled")
    if (
        not isinstance(owner_epoch, str)
        or type(generation) is not int
        or type(reconciled) is not bool
    ):
        return None
    return KernelHeldAttention(
        owner_epoch=owner_epoch,
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
        deadline_seconds: float = PUBLICATION_DEADLINE_SECONDS,
    ) -> None:
        if not (
            0.0 < deadline_seconds < period_seconds
            and period_seconds + deadline_seconds < KERNEL_FRESHNESS_SECONDS
            and 4.0 * period_seconds <= KERNEL_FRESHNESS_SECONDS
        ):
            raise ValueError(
                "alarm attention publication timing does not fit the Kernel "
                "freshness bound"
            )
        self._core = core
        self._kernel = kernel
        self._period_seconds = period_seconds
        self._deadline_seconds = deadline_seconds
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._failing = False
        self._counts_lock = threading.Lock()
        self._published = 0
        self._abandoned = 0
        self._failed = 0
        self._held: KernelHeldAttention | None = None
        self._held_observer: Callable[[], None] | None = None

    def set_kernel_held_observer(self, observer: Callable[[], None] | None) -> None:
        """Observe a change of the Kernel-held projection's identity.

        Called on the publisher thread when the acknowledged owner epoch,
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
            return AttentionPublicationCounts(
                self._published, self._abandoned, self._failed
            )

    def publish_once(self) -> bool:
        """Read and publish the current projection by its deadline.

        Returns False when the publication failed or was abandoned at its
        deadline; the next period publishes the then-current projection.
        """
        deadline = time.monotonic() + self._deadline_seconds
        try:
            projection = self._core.operator_alarm_motion_attention()
            self._kernel.publish_operator_alarm_attention(
                projection, deadline=deadline
            )
        except Exception:
            # The Kernel fails closed once the last publication ages out; a
            # transient owner or transport failure is retried next period.
            now = time.monotonic()
            abandoned = now >= deadline
            with self._counts_lock:
                if abandoned:
                    self._abandoned += 1
                else:
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
                    "operator-alarm attention publication %s",
                    "abandoned at its deadline" if abandoned else "failed",
                    exc_info=True,
                )
            return False
        with self._counts_lock:
            self._published += 1
        # The Kernel acknowledged retaining exactly this epoch and generation
        # (the transport verifies the acknowledgement names them).
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
        self._thread = threading.Thread(
            target=self._run, name="manyforge-alarm-attention-publisher"
        )
        self._thread.start()

    def shutdown(self) -> None:
        self._stop.set()
        thread = self._thread
        if thread is not None:
            # Every blocking step of the one outstanding publication ends by
            # its deadline D, so the thread observes the stop within D.
            thread.join(timeout=self._deadline_seconds + _SHUTDOWN_SLACK_SECONDS)
            if thread.is_alive():
                raise RuntimeError("alarm attention publisher did not stop in time")
        self._thread = None

    def _run(self) -> None:
        next_at = time.monotonic()
        while not self._stop.is_set():
            self.publish_once()
            # Fixed-rate schedule. D < P keeps each publication inside its
            # period; after a scheduling overrun publish again at once rather
            # than bursting to catch up.
            now = time.monotonic()
            next_at = max(next_at + self._period_seconds, now)
            self._stop.wait(next_at - now)


__all__ = [
    "AttentionPublicationCounts",
    "KERNEL_FRESHNESS_SECONDS",
    "KernelHeldAttention",
    "OperatorAlarmAttentionPublisher",
    "PUBLICATION_DEADLINE_SECONDS",
    "PUBLICATION_PERIOD_SECONDS",
]
