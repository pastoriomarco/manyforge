from __future__ import annotations

import asyncio
import json
import queue
import threading
from typing import Any

import pytest

from manyforge_composer.backend import execution_observation_stream as stream


def _status(*, sequence: int = 7, position: float = 0.25) -> dict[str, Any]:
    return {
        "api_version": "manyforge.execution_command_response.v1",
        "command": "observation",
        "ok": True,
        "observation": {
            "schema_version": "manyforge.execution_observation.v1",
            "owner_epoch": "execution-owner.1",
            "cell_id": "cell-1",
            "retained_bundle_id": "bundle-1",
            "graph_revision": "graph-1",
            "controllable_component_id": "robot-1",
            "command_target_id": "arm",
            "source_epoch": "joint-source.1",
            "source_sequence": sequence,
            "source_joint_names": ["joint1", "joint2"],
            "compiled_joint_names": ["robot__joint1", "robot__joint2"],
            "positions_rad": [position, -position],
        },
    }


def test_project_execution_observation_is_exact_and_compact() -> None:
    frame = stream.project_execution_observation(
        _status(), received_at_monotonic_us=123_456
    )
    assert frame == {
        "schema_version": "manyforge.execution_observation_frame.v1",
        "owner_epoch": "execution-owner.1",
        "cell_id": "cell-1",
        "retained_bundle_id": "bundle-1",
        "graph_revision": "graph-1",
        "controllable_component_id": "robot-1",
        "command_target_id": "arm",
        "source_epoch": "joint-source.1",
        "source_sequence": 7,
        "received_at_monotonic_us": 123_456,
        "source_joint_names": ["joint1", "joint2"],
        "compiled_joint_names": ["robot__joint1", "robot__joint2"],
        "positions_rad": [0.25, -0.25],
    }

    crossed = _status()
    crossed["observation"]["compiled_joint_names"] = ["robot__joint1"]
    with pytest.raises(
        stream.ExecutionObservationProjectionError,
        match="joint orders differ",
    ):
        stream.project_execution_observation(
            crossed, received_at_monotonic_us=123_457
        )

    inflated = _status()
    inflated["observation"]["diagnostics"] = {"unexpected": "cold data"}
    with pytest.raises(
        stream.ExecutionObservationProjectionError,
        match="schema is unsupported",
    ):
        stream.project_execution_observation(
            inflated, received_at_monotonic_us=123_458
        )


def test_encode_execution_observation_frame_is_one_canonical_payload() -> None:
    frame = stream.project_execution_observation(
        _status(), received_at_monotonic_us=123_456
    )

    encoded = stream.encode_execution_observation_frame(frame)

    assert json.loads(encoded) == frame
    assert " " not in encoded
    assert encoded.startswith('{"cell_id":"cell-1",')


class _Upstream:
    def __init__(self, owner: "_Client") -> None:
        self.owner = owner

    def __enter__(self) -> "_Upstream":
        self.owner.connections += 1
        return self

    def __exit__(self, *_exc: object) -> None:
        return None

    def next(
        self,
        cursor: dict[str, Any] | None,
        cancelled: threading.Event,
    ) -> dict[str, Any]:
        self.owner.cursors.put(cursor)
        while not cancelled.is_set():
            try:
                return self.owner.responses.get(timeout=0.01)
            except queue.Empty:
                continue
        raise RuntimeError("cancelled")


class _Client:
    def __init__(self) -> None:
        self.connections = 0
        self.cursors: queue.Queue[dict[str, Any] | None] = queue.Queue()
        self.responses: queue.Queue[dict[str, Any]] = queue.Queue()

    def open_execution_observation_stream(self) -> _Upstream:
        return _Upstream(self)


async def _next_cursor(client: _Client) -> dict[str, Any] | None:
    return await asyncio.to_thread(client.cursors.get, True, 1.0)


async def _wait_for_error(source: stream.ExecutionObservationSource) -> None:
    for _ in range(1_000):
        if source.status()["source"]["error_count"] > 0:
            return
        await asyncio.sleep(0)
    raise AssertionError("source error was not published")


def test_source_is_change_driven_credited_and_independent_per_browser() -> None:
    async def exercise() -> None:
        client = _Client()
        source = stream.ExecutionObservationSource(client)  # type: ignore[arg-type]
        subscription = source.subscribe()
        assert client.connections == 0
        source.grant_credit(subscription, None)
        assert await _next_cursor(client) is None
        client.responses.put(_status(sequence=1, position=0.1))
        await asyncio.wait_for(subscription.wake.wait(), timeout=1.0)
        first = json.loads(source.take_payload(subscription) or "null")
        assert first["source_sequence"] == 1

        # No acknowledgement means no second upstream request or frame.
        await asyncio.sleep(0)
        assert client.cursors.empty()

        source.grant_credit(subscription, ("execution-owner.1", "joint-source.1", 1))
        assert await _next_cursor(client) == {
            "owner_epoch": "execution-owner.1",
            "source_epoch": "joint-source.1",
            "source_sequence": 1,
        }
        client.responses.put(_status(sequence=2, position=0.2))
        await asyncio.wait_for(subscription.wake.wait(), timeout=1.0)
        second_frame = json.loads(source.take_payload(subscription) or "null")
        assert second_frame["source_sequence"] == 2

        # A new browser receives the shared current frame without another
        # owner request. Its missing acknowledgement cannot block the first.
        second = source.subscribe()
        source.grant_credit(second, None)
        await asyncio.wait_for(second.wake.wait(), timeout=1.0)
        assert json.loads(source.take_payload(second) or "null")["source_sequence"] == 2
        source.grant_credit(
            subscription,
            ("execution-owner.1", "joint-source.1", 2),
        )
        assert (await _next_cursor(client))["source_sequence"] == 2  # type: ignore[index]
        client.responses.put(_status(sequence=3, position=0.3))
        await asyncio.wait_for(subscription.wake.wait(), timeout=1.0)
        assert json.loads(source.take_payload(subscription) or "null")["source_sequence"] == 3
        assert not second.wake.is_set()

        status = source.status()
        assert status["source"]["mode"] == "change_driven"
        assert status["source"]["periodic_http_requests"] == 0
        assert status["source"]["wake_count"] == 3
        assert status["fanout"]["capacity_per_client"] == 1
        source.unsubscribe(second)
        source.unsubscribe(subscription)
        source.close()
        status = source.status()
        assert status["fanout"]["client_count"] == 0
        assert status["source"]["active"] is False

    asyncio.run(exercise())


def test_source_coalesces_visual_heartbeats_but_retains_the_latest_cursor() -> None:
    async def exercise() -> None:
        client = _Client()
        source = stream.ExecutionObservationSource(
            client,  # type: ignore[arg-type]
            monotonic=lambda: 12.5,
        )
        fast = source.subscribe()
        source.grant_credit(fast, None)
        assert await _next_cursor(client) is None
        client.responses.put(_status(sequence=1, position=0.1))
        await asyncio.wait_for(fast.wake.wait(), timeout=1.0)
        first = json.loads(source.take_payload(fast) or "null")
        assert first["received_at_monotonic_us"] == 12_500_000

        source.grant_credit(
            fast,
            ("execution-owner.1", "joint-source.1", 1),
        )
        assert (await _next_cursor(client)) == {
            "owner_epoch": "execution-owner.1",
            "source_epoch": "joint-source.1",
            "source_sequence": 1,
        }
        client.responses.put(_status(sequence=2, position=0.1))
        next_cursor = await _next_cursor(client)
        assert source.status()["fanout"]["coalesced_frames_total"] == 1
        assert not fast.wake.is_set()

        # First demand from another browser receives the newest complete
        # identity even though the pose itself did not need another render.
        slow = source.subscribe()
        source.grant_credit(slow, None)
        await asyncio.wait_for(slow.wake.wait(), timeout=1.0)
        retained = json.loads(source.take_payload(slow) or "null")
        assert retained["source_sequence"] == 2

        # The original credit stays live and the next visual change advances
        # immediately; the unacknowledged second browser is independent.
        assert next_cursor["source_sequence"] == 2  # type: ignore[index]
        client.responses.put(_status(sequence=3, position=0.3))
        await asyncio.wait_for(fast.wake.wait(), timeout=1.0)
        assert json.loads(source.take_payload(fast) or "null")["source_sequence"] == 3
        assert not slow.wake.is_set()

        source.unsubscribe(slow)
        source.unsubscribe(fast)
        source.close()

    asyncio.run(exercise())


def test_static_visible_credit_over_24_hours_creates_no_periodic_source_work() -> None:
    async def exercise() -> None:
        now = 0.0
        client = _Client()
        source = stream.ExecutionObservationSource(
            client,  # type: ignore[arg-type]
            monotonic=lambda: now,
        )
        subscription = source.subscribe()
        source.grant_credit(subscription, None)
        assert await _next_cursor(client) is None
        client.responses.put(_status(sequence=1, position=0.1))
        await asyncio.wait_for(subscription.wake.wait(), timeout=1.0)
        source.take_payload(subscription)
        applied = ("execution-owner.1", "joint-source.1", 1)
        source.grant_credit(subscription, applied)
        assert await _next_cursor(client) == {
            "owner_epoch": "execution-owner.1",
            "source_epoch": "joint-source.1",
            "source_sequence": 1,
        }

        # Advance a manual clock through the same coarse 15-second renewals as
        # the browser. One long-lived owner wait stays in flight; passage of
        # time alone cannot create another read, frame, encode, or send.
        for _ in range((25 * 60 * 60) // 15):
            now += 15.0
            source.grant_credit(subscription, applied)

        assert now > 24 * 60 * 60
        assert client.cursors.empty()
        status = source.status()
        assert status["source"]["wake_count"] == 1
        assert status["source"]["published_count"] == 1
        assert status["fanout"]["sent_count"] == 1
        assert status["fanout"]["credit_count"] == 6_002
        source.unsubscribe(subscription)
        source.close()

    asyncio.run(exercise())


def test_source_bounds_clients_and_rejects_stale_acknowledgements() -> None:
    async def exercise() -> None:
        client = _Client()
        source = stream.ExecutionObservationSource(client)  # type: ignore[arg-type]
        subscriptions = [
            source.subscribe()
            for _ in range(stream.MAXIMUM_OBSERVATION_CLIENTS)
        ]
        with pytest.raises(RuntimeError, match="client limit"):
            source.subscribe()

        first = subscriptions[0]
        source.grant_credit(first, None)
        await _next_cursor(client)
        client.responses.put(_status(sequence=1, position=0.1))
        await asyncio.wait_for(first.wake.wait(), timeout=1.0)
        source.take_payload(first)
        with pytest.raises(ValueError, match="stale"):
            source.grant_credit(
                first,
                ("execution-owner.1", "joint-source.1", 99),
            )

        for subscription in subscriptions:
            source.unsubscribe(subscription)
        source.close()

    asyncio.run(exercise())


def test_source_rejects_changed_state_at_the_same_source_sequence() -> None:
    async def exercise() -> None:
        client = _Client()
        source = stream.ExecutionObservationSource(client)  # type: ignore[arg-type]
        subscription = source.subscribe()
        source.grant_credit(subscription, None)
        assert await _next_cursor(client) is None
        client.responses.put(_status(sequence=7, position=1.0))
        await asyncio.wait_for(subscription.wake.wait(), timeout=1.0)
        first = json.loads(source.take_payload(subscription) or "null")
        assert first["positions_rad"] == [1.0, -1.0]
        source.grant_credit(
            subscription,
            ("execution-owner.1", "joint-source.1", 7),
        )
        await _next_cursor(client)
        client.responses.put(_status(sequence=7, position=2.0))
        await _wait_for_error(source)
        status = source.status()["source"]
        assert status["published_count"] == 1
        assert status["error_count"] == 1
        assert status["last_error"]["message"] == (
            "execution observation changed without advancing its source sequence"
        )
        source.unsubscribe(subscription)
        source.close()

    asyncio.run(exercise())


def test_source_accepts_a_fresh_restart_epoch_with_a_lower_sequence() -> None:
    async def exercise() -> None:
        client = _Client()
        source = stream.ExecutionObservationSource(client)  # type: ignore[arg-type]
        subscription = source.subscribe()
        source.grant_credit(subscription, None)
        await _next_cursor(client)
        client.responses.put(_status(sequence=50, position=0.1))
        await asyncio.wait_for(subscription.wake.wait(), timeout=1.0)
        source.take_payload(subscription)
        source.grant_credit(
            subscription,
            ("execution-owner.1", "joint-source.1", 50),
        )
        await _next_cursor(client)
        restarted = _status(sequence=1, position=0.2)
        restarted["observation"]["owner_epoch"] = "execution-owner.2"
        restarted["observation"]["source_epoch"] = "joint-source.2"
        client.responses.put(restarted)
        await asyncio.wait_for(subscription.wake.wait(), timeout=1.0)
        status = source.status()["source"]
        assert status["published_count"] == 2
        assert status["error_count"] == 0
        latest = json.loads(source.take_payload(subscription) or "null")
        assert latest["owner_epoch"] == "execution-owner.2"
        assert latest["source_epoch"] == "joint-source.2"
        assert latest["source_sequence"] == 1
        assert latest["positions_rad"] == [0.2, -0.2]
        source.unsubscribe(subscription)
        source.close()

    asyncio.run(exercise())
