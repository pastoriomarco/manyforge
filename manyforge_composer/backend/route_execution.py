"""Transport-neutral bounded executors for future NRT API clients.

Executor separation is reusable client infrastructure; it does not own
runtime, lifecycle, execution, or Stop semantics.  The former direct Kernel
Stop backstop and CycleManager-named wrappers were removed.  Future H0/L0/P5
routes may submit one already-typed synchronous client request through
``run_route_role`` and translate ``RouteExecutorBusyError`` at the transport
boundary.
"""

from __future__ import annotations

import asyncio
import os
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from enum import Enum
from typing import Any, Callable, TypeVar


T = TypeVar("T")


class RouteExecutionRole(str, Enum):
    """NRT scheduling roles; none implies semantic or command authority."""

    CONTROL = "control"
    LIFECYCLE = "lifecycle"
    BACKGROUND = "background"


class RouteExecutorBusyError(RuntimeError):
    """Transport-neutral bounded-admission rejection."""

    def __init__(self, *, role: RouteExecutionRole, max_admitted: int) -> None:
        super().__init__(
            f"route executor {role.value} is at its admission limit "
            f"({max_admitted})"
        )
        self.role = role
        self.max_admitted = max_admitted


def _positive_int_env(name: str, default: int, *, minimum: int = 1) -> int:
    raw = os.environ.get(name, "").strip()
    if not raw:
        return max(default, minimum)
    try:
        value = int(raw)
    except ValueError:
        return max(default, minimum)
    return value if value >= minimum else max(default, minimum)


def _admission_limit_env(name: str, default: int, *, workers: int) -> int:
    return _positive_int_env(name, default, minimum=workers)


@dataclass(frozen=True, slots=True)
class RouteExecutorSpec:
    max_workers: int
    max_admitted: int
    thread_name_prefix: str
    description: str

    def __post_init__(self) -> None:
        if self.max_workers < 1:
            raise ValueError("max_workers must be positive")
        if self.max_admitted < self.max_workers:
            raise ValueError("max_admitted must be at least max_workers")
        if not self.thread_name_prefix:
            raise ValueError("thread_name_prefix must be non-empty")


class BoundedRouteExecutor:
    """Bounded, restartable executor for one NRT scheduling role."""

    def __init__(
        self,
        role: RouteExecutionRole,
        spec: RouteExecutorSpec,
    ) -> None:
        self._role = role
        self._spec = spec
        self._semaphore = threading.BoundedSemaphore(spec.max_admitted)
        self._lock = threading.Lock()
        self._idle = threading.Condition(self._lock)
        self._executor: ThreadPoolExecutor | None = self._make_executor()
        self._admitted = 0
        self._running = 0
        self._restarts = 0
        self._rejected = 0

    def _make_executor(self) -> ThreadPoolExecutor:
        return ThreadPoolExecutor(
            max_workers=self._spec.max_workers,
            thread_name_prefix=self._spec.thread_name_prefix,
        )

    def _executor_for_submit(self) -> ThreadPoolExecutor:
        with self._lock:
            if self._executor is None:
                self._executor = self._make_executor()
                self._restarts += 1
            return self._executor

    def _run(self, function: Callable[[], T]) -> T:
        with self._lock:
            self._running += 1
        try:
            return function()
        finally:
            with self._lock:
                self._running -= 1
                self._idle.notify_all()

    async def run(self, function: Callable[[], T]) -> T:
        """Run one admitted callback without blocking the caller event loop."""

        if not self._semaphore.acquire(blocking=False):
            with self._lock:
                self._rejected += 1
            raise RouteExecutorBusyError(
                role=self._role,
                max_admitted=self._spec.max_admitted,
            )
        with self._lock:
            self._admitted += 1
        try:
            loop = asyncio.get_running_loop()
            executor = self._executor_for_submit()
            return await loop.run_in_executor(executor, self._run, function)
        finally:
            with self._lock:
                self._admitted -= 1
                self._idle.notify_all()
            self._semaphore.release()

    def shutdown(self) -> None:
        """Cancel queued work; the next request recreates the pool lazily."""

        with self._lock:
            executor = self._executor
            self._executor = None
        if executor is not None:
            executor.shutdown(wait=False, cancel_futures=True)

    def wait_for_idle(self, timeout_s: float) -> bool:
        deadline = time.monotonic() + max(0.0, timeout_s)
        with self._lock:
            while self._admitted > 0 or self._running > 0:
                remaining = deadline - time.monotonic()
                if remaining <= 0.0:
                    break
                self._idle.wait(timeout=remaining)
            return self._admitted == 0 and self._running == 0

    def status(self) -> dict[str, Any]:
        with self._lock:
            admitted = self._admitted
            running = self._running
            rejected = self._rejected
            restarts = self._restarts
        return {
            "max_workers": self._spec.max_workers,
            "max_admitted": self._spec.max_admitted,
            "admitted": admitted,
            "running": running,
            "queued": max(0, admitted - running),
            "rejected": rejected,
            "restarts": restarts,
            "thread_name_prefix": self._spec.thread_name_prefix,
            "description": self._spec.description,
        }


_CONTROL_WORKERS = _positive_int_env(
    "MANYFORGE_ROUTE_CONTROL_WORKERS",
    2,
    minimum=2,
)
_LIFECYCLE_WORKERS = _positive_int_env(
    "MANYFORGE_ROUTE_LIFECYCLE_WORKERS",
    2,
)
_BACKGROUND_WORKERS = _positive_int_env(
    "MANYFORGE_ROUTE_BACKGROUND_WORKERS",
    4,
)

_ROLE_SPECS: dict[RouteExecutionRole, RouteExecutorSpec] = {
    RouteExecutionRole.CONTROL: RouteExecutorSpec(
        max_workers=_CONTROL_WORKERS,
        max_admitted=_admission_limit_env(
            "MANYFORGE_ROUTE_CONTROL_MAX_ADMITTED",
            6,
            workers=_CONTROL_WORKERS,
        ),
        thread_name_prefix="manyforge_api_control",
        description="bounded NRT control-request client work",
    ),
    RouteExecutionRole.LIFECYCLE: RouteExecutorSpec(
        max_workers=_LIFECYCLE_WORKERS,
        max_admitted=_admission_limit_env(
            "MANYFORGE_ROUTE_LIFECYCLE_MAX_ADMITTED",
            8,
            workers=_LIFECYCLE_WORKERS,
        ),
        thread_name_prefix="manyforge_api_lifecycle",
        description="bounded NRT lifecycle-request client work",
    ),
    RouteExecutionRole.BACKGROUND: RouteExecutorSpec(
        max_workers=_BACKGROUND_WORKERS,
        max_admitted=_admission_limit_env(
            "MANYFORGE_ROUTE_BACKGROUND_MAX_ADMITTED",
            32,
            workers=_BACKGROUND_WORKERS,
        ),
        thread_name_prefix="manyforge_api_background",
        description="bounded NRT query, export, and diagnostic client work",
    ),
}

_EXECUTORS: dict[RouteExecutionRole, BoundedRouteExecutor] = {
    role: BoundedRouteExecutor(role, spec) for role, spec in _ROLE_SPECS.items()
}


async def run_route_role(
    role: RouteExecutionRole,
    function: Callable[[], T],
) -> T:
    """Submit one source-owned client request to an explicit NRT role."""

    return await _EXECUTORS[role].run(function)


def shutdown_route_executors(*, wait_timeout_s: float = 1.0) -> bool:
    for executor in _EXECUTORS.values():
        executor.shutdown()
    drained = [
        executor.wait_for_idle(wait_timeout_s) for executor in _EXECUTORS.values()
    ]
    return all(drained)


def route_executor_status() -> dict[str, dict[str, Any]]:
    return {role.value: _EXECUTORS[role].status() for role in _ROLE_SPECS}


__all__ = [
    "BoundedRouteExecutor",
    "RouteExecutionRole",
    "RouteExecutorBusyError",
    "RouteExecutorSpec",
    "route_executor_status",
    "run_route_role",
    "shutdown_route_executors",
]
