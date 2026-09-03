"""Small bounded priority lanes for execution-control transport work.

This module owns no robot, Program, lifecycle, or command state.  It only keeps
ordinary synchronous HTTP/native client calls from consuming the worker needed
to hand a recognized Stop request to its existing reserved owner path.
"""

from __future__ import annotations

import asyncio
import threading
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from enum import Enum
from typing import Any, Callable, TypeVar


T = TypeVar("T")


class ExecutionControlLane(str, Enum):
    """The two transport scheduling classes required by UI6."""

    STOP = "stop"
    ORDINARY = "ordinary"


class ExecutionControlBusyError(RuntimeError):
    """Raised before submission when one bounded lane is full."""

    def __init__(self, lane: ExecutionControlLane, maximum_admitted: int) -> None:
        super().__init__(
            f"execution-control {lane.value} lane is at its admission limit "
            f"({maximum_admitted})"
        )
        self.lane = lane
        self.maximum_admitted = maximum_admitted


@dataclass(frozen=True, slots=True)
class ExecutionControlLaneSpec:
    workers: int
    maximum_admitted: int

    def __post_init__(self) -> None:
        if self.workers < 1:
            raise ValueError("workers must be positive")
        if self.maximum_admitted < self.workers:
            raise ValueError("maximum_admitted must be at least workers")


class _BoundedLane:
    def __init__(
        self,
        lane: ExecutionControlLane,
        spec: ExecutionControlLaneSpec,
        *,
        thread_name_prefix: str,
    ) -> None:
        self._lane = lane
        self._spec = spec
        self._thread_name_prefix = thread_name_prefix
        self._capacity = threading.BoundedSemaphore(spec.maximum_admitted)
        self._lock = threading.Lock()
        self._executor: ThreadPoolExecutor | None = None
        self._closed = False
        self._admitted = 0
        self._running = 0
        self._rejected = 0

    def _executor_for_submit(self) -> ThreadPoolExecutor:
        with self._lock:
            if self._closed:
                raise RuntimeError("execution-control client is closed")
            if self._executor is None:
                self._executor = ThreadPoolExecutor(
                    max_workers=self._spec.workers,
                    thread_name_prefix=self._thread_name_prefix,
                )
            return self._executor

    def _invoke(self, callback: Callable[[], T]) -> T:
        with self._lock:
            self._running += 1
        try:
            return callback()
        finally:
            with self._lock:
                self._running -= 1

    async def run(self, callback: Callable[[], T]) -> T:
        if not self._capacity.acquire(blocking=False):
            with self._lock:
                self._rejected += 1
            raise ExecutionControlBusyError(
                self._lane,
                self._spec.maximum_admitted,
            )
        with self._lock:
            self._admitted += 1
        try:
            executor = self._executor_for_submit()
            future = asyncio.get_running_loop().run_in_executor(
                executor,
                self._invoke,
                callback,
            )
        except BaseException:
            with self._lock:
                self._admitted -= 1
            self._capacity.release()
            raise

        def release_capacity(_completed: asyncio.Future[T]) -> None:
            with self._lock:
                self._admitted -= 1
            self._capacity.release()

        future.add_done_callback(release_capacity)
        # Cancelling a disconnected HTTP handler cannot cancel synchronous
        # transport work already running in a thread. Keep its admission until
        # the worker really finishes so disconnect storms cannot grow the lane.
        return await asyncio.shield(future)

    def close(self) -> None:
        with self._lock:
            self._closed = True
            executor = self._executor
            self._executor = None
        if executor is not None:
            executor.shutdown(wait=False, cancel_futures=True)

    def status(self) -> dict[str, Any]:
        with self._lock:
            admitted = self._admitted
            running = self._running
            rejected = self._rejected
            open_ = not self._closed
        return {
            "open": open_,
            "workers": self._spec.workers,
            "maximum_admitted": self._spec.maximum_admitted,
            "admitted": admitted,
            "running": running,
            "queued": max(0, admitted - running),
            "rejected": rejected,
        }


class ExecutionControlClient:
    """One reserved Stop lane plus one no-wait ordinary-command lane."""

    def __init__(
        self,
        *,
        stop: ExecutionControlLaneSpec = ExecutionControlLaneSpec(2, 2),
        ordinary: ExecutionControlLaneSpec = ExecutionControlLaneSpec(2, 2),
        thread_name_prefix: str = "manyforge_execution_control",
    ) -> None:
        self._lanes = {
            ExecutionControlLane.STOP: _BoundedLane(
                ExecutionControlLane.STOP,
                stop,
                thread_name_prefix=f"{thread_name_prefix}_stop",
            ),
            ExecutionControlLane.ORDINARY: _BoundedLane(
                ExecutionControlLane.ORDINARY,
                ordinary,
                thread_name_prefix=f"{thread_name_prefix}_ordinary",
            ),
        }

    async def stop(self, callback: Callable[[], T]) -> T:
        return await self._lanes[ExecutionControlLane.STOP].run(callback)

    async def ordinary(self, callback: Callable[[], T]) -> T:
        return await self._lanes[ExecutionControlLane.ORDINARY].run(callback)

    def close(self) -> None:
        for lane in self._lanes.values():
            lane.close()

    def status(self) -> dict[str, dict[str, Any]]:
        return {lane.value: self._lanes[lane].status() for lane in self._lanes}


__all__ = [
    "ExecutionControlBusyError",
    "ExecutionControlClient",
    "ExecutionControlLane",
    "ExecutionControlLaneSpec",
]
