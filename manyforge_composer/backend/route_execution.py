"""Explicit executor roles for Composer HTTP route handlers.

Python route handling is not real-time authority, but route starvation must not
hide operator stop surfaces. Keep stop/control, lifecycle, and heavy diagnostic
work on separate executors so a wedged export/snapshot/history call cannot
consume the lane used by `/api/cycle/stop`, `/api/cycle/full-stop`, or the
kernel-level stop backstop.
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

from fastapi import HTTPException

T = TypeVar("T")


class RouteExecutionRole(str, Enum):
    """Route executor roles ordered by operator-control criticality."""

    CONTROL = "control"
    LIFECYCLE = "lifecycle"
    BACKGROUND = "background"


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


@dataclass(frozen=True)
class _RouteExecutorSpec:
    max_workers: int
    max_admitted: int
    thread_name_prefix: str
    description: str


_CONTROL_WORKERS = _positive_int_env(
    "MANYFORGE_ROUTE_CONTROL_WORKERS",
    2,
    minimum=2,
)
_LIFECYCLE_WORKERS = _positive_int_env("MANYFORGE_ROUTE_LIFECYCLE_WORKERS", 2)
_BACKGROUND_WORKERS = _positive_int_env("MANYFORGE_ROUTE_BACKGROUND_WORKERS", 4)

_ROLE_SPECS: dict[RouteExecutionRole, _RouteExecutorSpec] = {
    RouteExecutionRole.CONTROL: _RouteExecutorSpec(
        max_workers=_CONTROL_WORKERS,
        max_admitted=_admission_limit_env(
            "MANYFORGE_ROUTE_CONTROL_MAX_ADMITTED",
            6,
            workers=_CONTROL_WORKERS,
        ),
        thread_name_prefix="manyforge_cycle_control",
        description="operator stop/full-stop and kernel stop backstop",
    ),
    RouteExecutionRole.LIFECYCLE: _RouteExecutorSpec(
        max_workers=_LIFECYCLE_WORKERS,
        max_admitted=_admission_limit_env(
            "MANYFORGE_ROUTE_LIFECYCLE_MAX_ADMITTED",
            8,
            workers=_LIFECYCLE_WORKERS,
        ),
        thread_name_prefix="manyforge_cycle_lifecycle",
        description="run/reset/acknowledge lifecycle commands",
    ),
    RouteExecutionRole.BACKGROUND: _RouteExecutorSpec(
        max_workers=_BACKGROUND_WORKERS,
        max_admitted=_admission_limit_env(
            "MANYFORGE_ROUTE_BACKGROUND_MAX_ADMITTED",
            32,
            workers=_BACKGROUND_WORKERS,
        ),
        thread_name_prefix="manyforge_route_background",
        description="debug/export/history/snapshot and other diagnostic work",
    ),
}


class _RoleExecutor:
    """Bounded, restartable executor for one HTTP route role."""

    def __init__(self, role: RouteExecutionRole, spec: _RouteExecutorSpec) -> None:
        self._role = role
        self._spec = spec
        self._semaphore = threading.BoundedSemaphore(spec.max_admitted)
        self._lock = threading.Lock()
        self._idle = threading.Condition(self._lock)
        self._executor = self._make_executor()
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

    def _mark_admitted(self) -> None:
        with self._lock:
            self._admitted += 1

    def _mark_rejected(self) -> None:
        with self._lock:
            self._rejected += 1

    def _run(self, fn: Callable[[], T]) -> T:
        with self._lock:
            self._running += 1
        try:
            return fn()
        finally:
            with self._lock:
                self._running -= 1
                self._idle.notify_all()

    async def run(self, fn: Callable[[], T]) -> T:
        if not self._semaphore.acquire(blocking=False):
            self._mark_rejected()
            raise HTTPException(
                status_code=503,
                detail={
                    "error": "route_executor_busy",
                    "role": self._role.value,
                    "max_admitted": self._spec.max_admitted,
                },
            )
        self._mark_admitted()
        try:
            loop = asyncio.get_running_loop()
            executor = self._executor_for_submit()
            return await loop.run_in_executor(executor, self._run, fn)
        finally:
            with self._lock:
                self._admitted -= 1
                self._idle.notify_all()
            self._semaphore.release()

    def shutdown(self) -> None:
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


_EXECUTORS: dict[RouteExecutionRole, _RoleExecutor] = {
    role: _RoleExecutor(role, spec) for role, spec in _ROLE_SPECS.items()
}

_KERNEL_STOP_BACKSTOP_LOCK = threading.Lock()
_KERNEL_STOP_BACKSTOP_IN_FLIGHT = False


async def run_route_role(role: RouteExecutionRole, fn: Callable[[], T]) -> T:
    """Run a synchronous route command on its explicit executor role."""

    return await _EXECUTORS[role].run(fn)


async def run_cycle_control(fn: Callable[[], T]) -> T:
    return await run_route_role(RouteExecutionRole.CONTROL, fn)


async def run_cycle_lifecycle(fn: Callable[[], T]) -> T:
    return await run_route_role(RouteExecutionRole.LIFECYCLE, fn)


async def run_route_background(fn: Callable[[], T]) -> T:
    return await run_route_role(RouteExecutionRole.BACKGROUND, fn)


async def run_kernel_stop_backstop(
    fn: Callable[[], T],
    *,
    coalesced: Callable[[], T],
) -> T:
    """Run at most one direct kernel-stop backstop at a time.

    The backstop bypasses the cycle layer, so duplicate UI retries must not fill
    the reserved stop lane and starve `/api/cycle/full-stop`. One in-flight
    kernel stop is enough to carry the stop intent; later calls return a
    structured coalesced response without consuming a worker.
    """

    global _KERNEL_STOP_BACKSTOP_IN_FLIGHT
    with _KERNEL_STOP_BACKSTOP_LOCK:
        if _KERNEL_STOP_BACKSTOP_IN_FLIGHT:
            return coalesced()
        _KERNEL_STOP_BACKSTOP_IN_FLIGHT = True
    try:
        return await run_cycle_control(fn)
    finally:
        with _KERNEL_STOP_BACKSTOP_LOCK:
            _KERNEL_STOP_BACKSTOP_IN_FLIGHT = False


def shutdown_route_executors(*, wait_timeout_s: float = 1.0) -> bool:
    """Cancel queued route work and wait briefly for running work to finish.

    Executors are recreated lazily on the next route call, which keeps TestClient
    lifespans and dev reloads from inheriting stale pools.
    """

    for executor in _EXECUTORS.values():
        executor.shutdown()
    drained = [executor.wait_for_idle(wait_timeout_s) for executor in _EXECUTORS.values()]
    return all(drained)


def route_executor_status() -> dict[str, dict[str, Any]]:
    """Expose route-role metadata for tests and diagnostics."""

    return {role.value: _EXECUTORS[role].status() for role in _ROLE_SPECS}
