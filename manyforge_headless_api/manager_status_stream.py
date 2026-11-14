"""Follower of the Manager's status stream (OD-216).

The Manager serves its state from memory on a socket of its own,
``manager-status.sock`` in its run root: the connection itself is the
subscription. Each packet is one frame of ``manyforge.manager_status_stream.v1``
(``snapshot`` first, then a whole ``section`` frame each time a section
changes, a ``keepalive`` when only the Manager's main-loop tick advanced, or an
``error``). One follower thread feeds every frame to the native core's one
mirror, which validates it; the runtime snapshot, the control state and the
viewer read that mirror.

The Manager is "not answering" when its connection ended, or when no frame
showing a new tick of its main loop arrived within the freshness budget (the
tick advances every 500 ms). The connection's end is seen at once; the tick
rule is the only detector of a Manager whose main loop is held, so the
follower keeps the connection then and only wakes the readers at the deadline.
It reconnects after the connection ends, an invalid frame or an ``error``
frame, with backoff 0.1 s doubling to 0.5 s; every new ``snapshot``
resynchronises the mirror. The sliced receive and the reconnect loop are the
shared private stream client (``private_stream``).
"""

from __future__ import annotations

from collections.abc import Callable, Iterator, Mapping
from contextlib import contextmanager
import logging
from pathlib import Path
import socket
import threading
import time
from typing import Any, Final, Protocol

from .native import HeadlessApiError
from .private_stream import TIMED_OUT, PrivateStreamSubscription, follow_private_stream
from .private_transport import PinnedRunRoot, PrivateTransportCapabilityError

MANAGER_STATUS_SOCKET_FILE: Final = "manager-status.sock"
MANAGER_STATUS_STREAM_API_VERSION: Final = "manyforge.manager_status_stream.v1"
# The Manager's largest frame (its response bound) plus one byte: a longer
# packet is truncated and refused as invalid.
MAX_MANAGER_STATUS_FRAME_BYTES: Final = 3 * 128 * 1024 + 16 * 1024 + 1
RECONNECT_BACKOFF_SECONDS: Final = (0.1, 0.5)
# While the Manager does not answer, a receive waits this long per call; the
# stop is still seen within one receive slice.
_SILENT_RECEIVE_SECONDS: Final = 3600.0

_LOGGER = logging.getLogger(__name__)


class ManagerStatusCore(Protocol):
    def apply_manager_status_frame(self, frame: bytes) -> dict[str, Any]: ...

    def mark_manager_status_disconnected(self) -> bool: ...


def _frame(payload: bytes) -> dict[str, Any]:
    # The native mirror decodes and validates the frame.
    return {"frame": payload}


@contextmanager
def open_status_subscription(run_root: PinnedRunRoot) -> Iterator[PrivateStreamSubscription]:
    """Connect to the Manager's status socket; the connection subscribes."""

    client = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    try:
        with run_root.socket_address(MANAGER_STATUS_SOCKET_FILE) as address:
            client.connect(address)
    except PrivateTransportCapabilityError as error:
        client.close()
        raise HeadlessApiError(
            "manager_status_unavailable", f"Manager status socket is unavailable: {error}"
        ) from error
    except OSError as error:
        client.close()
        raise HeadlessApiError(
            "manager_status_unavailable", f"Manager status connect failed: {error}"
        ) from error
    except BaseException:
        client.close()
        raise
    subscription = PrivateStreamSubscription(
        client,
        max_bytes=MAX_MANAGER_STATUS_FRAME_BYTES,
        decode=_frame,
        unavailable_code="manager_status_unavailable",
        invalid_code="manager_status_stream_invalid",
        label="Manager status stream",
    )
    try:
        yield subscription
    finally:
        subscription.close()


class ManagerStatusFollower:
    """One thread keeping the one status subscription and the mirror current.

    On the follower thread, ``on_control_change`` runs when what the control
    state shows changed (not for pose samples alone), ``on_viewer_change``
    when the viewer's scene-runtime or pose-session sources changed, and both
    whenever the Manager starts or stops answering. They must not block; if
    one raises, the follower logs it and reconnects.
    """

    def __init__(
        self,
        core: ManagerStatusCore,
        run_root: Path | PinnedRunRoot,
        *,
        silence_seconds: float,
        on_control_change: Callable[[], None],
        on_viewer_change: Callable[[], None],
        backoff_seconds: tuple[float, float] = RECONNECT_BACKOFF_SECONDS,
    ) -> None:
        if silence_seconds <= 0:
            raise ValueError("the Manager silence bound must be positive")
        self._core = core
        self._run_root = run_root if isinstance(run_root, PinnedRunRoot) else PinnedRunRoot(run_root)
        self._silence_seconds = silence_seconds
        self._on_control_change = on_control_change
        self._on_viewer_change = on_viewer_change
        self._backoff_seconds = backoff_seconds
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        # Follower thread only: whether the Manager answers, and the deadline
        # of its next tick (monotonic seconds), taken after the mirror stamped
        # the frame so the mirror already reads "not answering" when it passes.
        self._answering = False
        self._deadline = 0.0

    def start(self) -> None:
        if self._thread is not None:
            raise RuntimeError("Manager status follower already started")
        self._thread = threading.Thread(
            target=self._run, name="manyforge-manager-status-stream", daemon=True
        )
        self._thread.start()

    def shutdown(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join()

    def _run(self) -> None:
        follow_private_stream(
            self._connect,
            self._apply,
            self._stop,
            backoff_seconds=self._backoff_seconds,
            on_disconnect=self._disconnected,
            label="Manager status stream",
            logger=_LOGGER,
        )

    @contextmanager
    def _connect(self) -> Iterator[_LivenessSubscription]:
        with open_status_subscription(self._run_root) as subscription:
            yield _LivenessSubscription(subscription, self)

    def _apply(self, message: Mapping[str, Any]) -> None:
        result = self._core.apply_manager_status_frame(message["frame"])
        control = result.get("control_changed") is True
        viewer = result.get("viewer_changed") is True
        if result.get("tick_advanced") is True:
            self._deadline = time.monotonic() + self._silence_seconds
            if not self._answering:
                self._answering = True
                control = viewer = True
        if control:
            self._on_control_change()
        if viewer:
            self._on_viewer_change()

    def _silent(self) -> None:
        """No new tick before the deadline: the Manager is not answering."""

        if self._answering:
            self._answering = False
            self._notify()

    def _receive_timeout(self) -> float:
        if not self._answering:
            return _SILENT_RECEIVE_SECONDS
        return max(self._deadline - time.monotonic(), 0.0)

    def _disconnected(self, _error: BaseException) -> None:
        self._answering = False
        if self._core.mark_manager_status_disconnected():
            self._notify()

    def _notify(self) -> None:
        self._on_control_change()
        self._on_viewer_change()


class _LivenessSubscription:
    """``receive(stop)`` for ``follow_private_stream``: a quiet link is a
    Manager that does not answer, not a reason to reconnect."""

    def __init__(self, stream: PrivateStreamSubscription, follower: ManagerStatusFollower) -> None:
        self._stream = stream
        self._follower = follower

    def receive(self, stop: threading.Event | None = None) -> dict[str, Any] | None:
        while True:
            message = self._stream.receive(stop, self._follower._receive_timeout())
            if message is not TIMED_OUT:
                return message
            self._follower._silent()


class ManagerStatusChanges:
    """Wakes asyncio readers (the viewer pose streams) from the follower
    thread: each registered event is set on its own loop."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._waiters: set[tuple[Any, Any]] = set()

    def register(self, loop: Any, event: Any) -> Callable[[], None]:
        entry = (loop, event)
        with self._lock:
            self._waiters.add(entry)

        def unregister() -> None:
            with self._lock:
                self._waiters.discard(entry)

        return unregister

    def notify(self) -> None:
        with self._lock:
            waiters = tuple(self._waiters)
        for loop, event in waiters:
            loop.call_soon_threadsafe(event.set)


__all__ = [
    "MANAGER_STATUS_SOCKET_FILE",
    "MANAGER_STATUS_STREAM_API_VERSION",
    "MAX_MANAGER_STATUS_FRAME_BYTES",
    "ManagerStatusChanges",
    "ManagerStatusFollower",
    "open_status_subscription",
]
