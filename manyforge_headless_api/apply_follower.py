"""Follow each Apply Headless forwarded until Manager commits it.

Manager's command socket only answers requests and its published source is
read on demand, so no push reaches Headless when an Apply commits. Headless
forwards every named Apply, so it follows each one it forwarded: it asks
Manager for that Apply's status until the outcome is final, then waits until
its own committed-activation projection names that Apply or a later one as
the active configuration's Apply (Manager publishes its source just after
recording the outcome; Apply numbers count up within one start) and wakes the
Program runner once. Neither wait has a limit of its own (OD-214): Manager
ends every Apply (at the latest after its own Apply bound), a succeeded Apply
always carries its publication, and closing the service ends both. An Apply a
later one already superseded is covered by that later one.
A failed or unknown-effect Apply never published anything and wakes nothing.
The wake carries nothing: the runner reads the committed activation itself,
so a late or repeated wake is harmless.
"""

from __future__ import annotations

from collections.abc import Callable, Mapping
import logging
from threading import Event, Lock, Thread
from typing import Any, Final


_LOGGER = logging.getLogger(__name__)
_MAXIMUM_FOLLOWED: Final = 16


class CommittedApplyFollower:
    def __init__(
        self,
        *,
        observe_apply: Callable[[str, int], Mapping[str, Any]],
        committed_activation: Callable[[], Mapping[str, Any]],
        wake: Callable[[], None],
        poll_seconds: float = 0.25,
    ) -> None:
        if not 0 < poll_seconds <= 5:
            raise ValueError("Apply follower poll period is invalid")
        self._observe_apply = observe_apply
        self._committed_activation = committed_activation
        self._wake = wake
        self._poll = poll_seconds
        self._stop = Event()
        self._lock = Lock()
        self._followed: dict[tuple[str, int], Thread] = {}

    def follow(self, apply_id: int, apply_generation: int) -> None:
        """Follow one forwarded Apply; a repeated identity is followed once."""

        identity = (apply_id, apply_generation)
        with self._lock:
            if self._stop.is_set() or identity in self._followed:
                return
            if len(self._followed) >= _MAXIMUM_FOLLOWED:
                # Manager runs one Apply at a time; this bound is never
                # reached in operation. A skipped Apply is still reconciled
                # (and reported) by the runner's own backstops.
                _LOGGER.warning(
                    "not following committed Apply %s/%s: follower is full",
                    apply_id,
                    apply_generation,
                )
                return
            thread = Thread(
                target=self._run,
                args=identity,
                name=f"apply-follower-{apply_generation}",
                daemon=True,
            )
            self._followed[identity] = thread
        thread.start()

    def close(self, timeout_seconds: float = 1.0) -> None:
        self._stop.set()
        with self._lock:
            threads = list(self._followed.values())
        for thread in threads:
            thread.join(timeout_seconds)

    def _run(self, apply_id: int, apply_generation: int) -> None:
        try:
            if self._committed(apply_id, apply_generation):
                self._await_publication(apply_id)
                if not self._stop.is_set():
                    self._wake()
        except Exception:  # A follower never takes the process down.
            _LOGGER.exception(
                "following committed Apply %s/%s failed", apply_id, apply_generation
            )
        finally:
            with self._lock:
                self._followed.pop((apply_id, apply_generation), None)

    def _committed(self, apply_id: int, apply_generation: int) -> bool:
        while not self._stop.is_set():
            try:
                status = self._observe_apply(apply_id, apply_generation)
            except Exception as error:
                status = {}
                _LOGGER.debug("Apply status unavailable: %s", error)
            state = status.get("state") if status.get("ok") is True else None
            if state == "succeeded":
                return True
            if state in {"failed", "unknown_effect"}:
                return False
            self._stop.wait(self._poll)
        return False

    def _projected(self, apply_id: int) -> bool:
        try:
            active = self._committed_activation()["apply_id"]
        except Exception:
            return False
        return active >= apply_id

    def _await_publication(self, apply_id: int) -> None:
        while not self._stop.is_set():
            if self._projected(apply_id):
                return
            self._stop.wait(self._poll)


__all__ = ["CommittedApplyFollower"]
