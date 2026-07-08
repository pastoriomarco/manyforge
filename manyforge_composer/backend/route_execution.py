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
from concurrent.futures import ThreadPoolExecutor
from enum import Enum
from typing import Any, Callable, TypeVar

T = TypeVar("T")


class RouteExecutionRole(str, Enum):
    """Route executor roles ordered by operator-control criticality."""

    CONTROL = "control"
    LIFECYCLE = "lifecycle"
    BACKGROUND = "background"


def _positive_int_env(name: str, default: int) -> int:
    raw = os.environ.get(name, "").strip()
    if not raw:
        return default
    try:
        value = int(raw)
    except ValueError:
        return default
    return value if value > 0 else default


_ROLE_SPECS: dict[RouteExecutionRole, tuple[int, str, str]] = {
    RouteExecutionRole.CONTROL: (
        _positive_int_env("MANYFORGE_ROUTE_CONTROL_WORKERS", 2),
        "manyforge_cycle_control",
        "operator stop/full-stop and kernel stop backstop",
    ),
    RouteExecutionRole.LIFECYCLE: (
        _positive_int_env("MANYFORGE_ROUTE_LIFECYCLE_WORKERS", 2),
        "manyforge_cycle_lifecycle",
        "run/reset/acknowledge lifecycle commands",
    ),
    RouteExecutionRole.BACKGROUND: (
        _positive_int_env("MANYFORGE_ROUTE_BACKGROUND_WORKERS", 4),
        "manyforge_route_background",
        "debug/export/history/snapshot and other diagnostic work",
    ),
}

_EXECUTORS: dict[RouteExecutionRole, ThreadPoolExecutor] = {
    role: ThreadPoolExecutor(max_workers=max_workers, thread_name_prefix=prefix)
    for role, (max_workers, prefix, _description) in _ROLE_SPECS.items()
}


async def run_route_role(role: RouteExecutionRole, fn: Callable[[], T]) -> T:
    """Run a synchronous route command on its explicit executor role."""

    loop = asyncio.get_running_loop()
    return await loop.run_in_executor(_EXECUTORS[role], fn)


async def run_cycle_control(fn: Callable[[], T]) -> T:
    return await run_route_role(RouteExecutionRole.CONTROL, fn)


async def run_cycle_lifecycle(fn: Callable[[], T]) -> T:
    return await run_route_role(RouteExecutionRole.LIFECYCLE, fn)


async def run_route_background(fn: Callable[[], T]) -> T:
    return await run_route_role(RouteExecutionRole.BACKGROUND, fn)


def route_executor_status() -> dict[str, dict[str, Any]]:
    """Expose route-role metadata for tests and diagnostics."""

    return {
        role.value: {
            "max_workers": max_workers,
            "thread_name_prefix": prefix,
            "description": description,
        }
        for role, (max_workers, prefix, description) in _ROLE_SPECS.items()
    }
