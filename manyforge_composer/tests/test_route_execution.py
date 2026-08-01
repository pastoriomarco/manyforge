"""Dependency-free tests for bounded NRT route executors."""

import asyncio
import threading

import pytest

from manyforge_composer.backend import route_execution
from manyforge_composer.backend.route_execution import (
    BoundedRouteExecutor,
    RouteExecutionRole,
    RouteExecutorBusyError,
    RouteExecutorSpec,
)


def _executor(*, max_admitted: int = 1) -> BoundedRouteExecutor:
    return BoundedRouteExecutor(
        RouteExecutionRole.BACKGROUND,
        RouteExecutorSpec(
            max_workers=1,
            max_admitted=max_admitted,
            thread_name_prefix="manyforge_test_route",
            description="focused bounded-executor test",
        ),
    )


def test_executor_runs_callback_on_its_named_worker_and_restarts() -> None:
    executor = _executor()
    try:
        first_name = asyncio.run(
            executor.run(lambda: threading.current_thread().name)
        )
        assert first_name.startswith("manyforge_test_route")

        executor.shutdown()
        second_name = asyncio.run(
            executor.run(lambda: threading.current_thread().name)
        )
        assert second_name.startswith("manyforge_test_route")
        assert executor.status()["restarts"] == 1
    finally:
        executor.shutdown()
        assert executor.wait_for_idle(1.0)


def test_admission_rejection_is_typed_and_never_runs_callback() -> None:
    executor = _executor()
    started = threading.Event()
    release = threading.Event()
    rejected_callback_ran = False

    def blocking_callback() -> str:
        started.set()
        release.wait(timeout=1.0)
        return "released"

    def rejected_callback() -> None:
        nonlocal rejected_callback_ran
        rejected_callback_ran = True

    async def exercise() -> None:
        first = asyncio.create_task(executor.run(blocking_callback))
        assert await asyncio.to_thread(started.wait, 1.0)
        try:
            with pytest.raises(RouteExecutorBusyError) as error:
                await executor.run(rejected_callback)
            assert error.value.role is RouteExecutionRole.BACKGROUND
            assert error.value.max_admitted == 1
        finally:
            release.set()
        assert await first == "released"

    try:
        asyncio.run(exercise())
        assert rejected_callback_ran is False
        assert executor.status()["rejected"] == 1
    finally:
        release.set()
        executor.shutdown()
        assert executor.wait_for_idle(1.0)


def test_executor_spec_rejects_unbounded_or_undersized_configuration() -> None:
    with pytest.raises(ValueError, match="max_workers"):
        RouteExecutorSpec(0, 1, "worker", "invalid")
    with pytest.raises(ValueError, match="max_admitted"):
        RouteExecutorSpec(2, 1, "worker", "invalid")


def test_old_stop_and_cycle_wrappers_are_absent() -> None:
    assert not hasattr(route_execution, "run_kernel_stop_backstop")
    assert not hasattr(route_execution, "run_cycle_control")
    assert not hasattr(route_execution, "run_cycle_lifecycle")
    assert "HTTPException" not in route_execution.__dict__
