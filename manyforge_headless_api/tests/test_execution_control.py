"""Focused tests for the two execution-control lanes."""

from __future__ import annotations

import asyncio
import threading

import pytest

from manyforge_headless_api.execution_control import (
    ExecutionControlBusyError,
    ExecutionControlClient,
    ExecutionControlLane,
    ExecutionControlLaneSpec,
)


def _client() -> ExecutionControlClient:
    return ExecutionControlClient(
        stop=ExecutionControlLaneSpec(1, 1),
        ordinary=ExecutionControlLaneSpec(1, 1),
        thread_name_prefix="manyforge_test_control",
    )


def test_stop_progress_is_independent_of_full_ordinary_lane() -> None:
    client = _client()
    ordinary_started = threading.Event()
    release_ordinary = threading.Event()

    def block_ordinary() -> str:
        ordinary_started.set()
        release_ordinary.wait(timeout=2.0)
        return "ordinary"

    async def exercise() -> None:
        ordinary = asyncio.create_task(client.ordinary(block_ordinary))
        assert await asyncio.to_thread(ordinary_started.wait, 1.0)

        stop_thread = await asyncio.wait_for(
            client.stop(lambda: threading.current_thread().name),
            timeout=0.5,
        )
        assert stop_thread.startswith("manyforge_test_control_stop")
        assert not ordinary.done()

        release_ordinary.set()
        assert await ordinary == "ordinary"

    try:
        asyncio.run(exercise())
    finally:
        release_ordinary.set()
        client.close()


def test_each_lane_rejects_before_its_bounded_queue_can_grow() -> None:
    client = _client()
    started = threading.Event()
    release = threading.Event()
    rejected_ran = False

    def block() -> None:
        started.set()
        release.wait(timeout=2.0)

    def rejected() -> None:
        nonlocal rejected_ran
        rejected_ran = True

    async def exercise() -> None:
        admitted = asyncio.create_task(client.ordinary(block))
        assert await asyncio.to_thread(started.wait, 1.0)
        with pytest.raises(ExecutionControlBusyError) as error:
            await client.ordinary(rejected)
        assert error.value.lane is ExecutionControlLane.ORDINARY
        assert error.value.maximum_admitted == 1
        release.set()
        await admitted

    try:
        asyncio.run(exercise())
        assert rejected_ran is False
        assert client.status()["ordinary"]["rejected"] == 1
    finally:
        release.set()
        client.close()


def test_cancelled_caller_does_not_release_a_still_running_worker() -> None:
    client = _client()
    started = threading.Event()
    release = threading.Event()
    finished = threading.Event()

    def block() -> None:
        started.set()
        release.wait(timeout=2.0)
        finished.set()

    async def exercise() -> None:
        request = asyncio.create_task(client.ordinary(block))
        assert await asyncio.to_thread(started.wait, 1.0)
        request.cancel()
        with pytest.raises(asyncio.CancelledError):
            await request

        with pytest.raises(ExecutionControlBusyError):
            await client.ordinary(lambda: None)

        release.set()
        assert await asyncio.to_thread(finished.wait, 1.0)
        await asyncio.sleep(0)
        assert client.status()["ordinary"]["admitted"] == 0
        assert await client.ordinary(lambda: "next") == "next"

    try:
        asyncio.run(exercise())
    finally:
        release.set()
        client.close()


def test_lane_spec_rejects_unbounded_or_undersized_configuration() -> None:
    with pytest.raises(ValueError, match="workers"):
        ExecutionControlLaneSpec(0, 1)
    with pytest.raises(ValueError, match="maximum_admitted"):
        ExecutionControlLaneSpec(2, 1)


def test_default_stop_reserve_has_no_waiting_queue() -> None:
    client = ExecutionControlClient()
    try:
        status = client.status()
        assert status["stop"]["workers"] == 2
        assert status["stop"]["maximum_admitted"] == 2
        assert status["ordinary"]["workers"] == 2
        assert status["ordinary"]["maximum_admitted"] == 2
        assert status["stop"]["queued"] == 0
        assert status["ordinary"]["queued"] == 0
    finally:
        client.close()
