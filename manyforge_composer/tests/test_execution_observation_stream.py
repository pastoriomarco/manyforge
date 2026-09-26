from __future__ import annotations

import asyncio
import json
import queue
import threading
import time
from types import SimpleNamespace
from typing import Any

import pytest
from fastapi import FastAPI
from fastapi.testclient import TestClient
from starlette.websockets import WebSocketDisconnect

from manyforge_composer.backend import execution_observation_stream as stream
from manyforge_composer.backend.routes_viewer import create_viewer_router
from manyforge_composer.backend.security import SecurityConfig


def _status(*, sequence: int = 7, position: float = 0.25) -> dict[str, Any]:
    return {
        "api_version": "manyforge.execution_command_response.v1",
        "command": "observation",
        "ok": True,
        "observation": {
            "schema_version": "manyforge.execution_observation.v3",
            "owner_epoch": "execution-owner.1",
            "cell_id": "cell-1",
            "retained_bundle_id": "bundle-1",
            "graph_revision": "graph-1",
            "source_epoch": "joint-source.1",
            "source_sequence": sequence,
            "targets": [
                {
                    "controllable_component_id": "robot-1",
                    "command_target_id": "arm",
                    "operation_capability_id": "execution.trajectory.execute",
                    "source_epoch": "arm-source.1",
                    "source_sequence": sequence,
                    "source_joint_names": ["joint1", "joint2"],
                    "compiled_joint_names": ["robot__joint1", "robot__joint2"],
                    "coordinate_kinds": ["angular", "angular"],
                    "positions_si": [position, -position],
                }
            ],
        },
    }


def test_project_execution_observation_is_exact_and_compact() -> None:
    frame = stream.project_execution_observation(
        _status(), received_at_monotonic_us=123_456
    )
    assert frame == {
        "schema_version": "manyforge.execution_observation_frame.v3",
        "owner_epoch": "execution-owner.1",
        "cell_id": "cell-1",
        "retained_bundle_id": "bundle-1",
        "graph_revision": "graph-1",
        "source_epoch": "joint-source.1",
        "source_sequence": 7,
        "received_at_monotonic_us": 123_456,
        "targets": [
            {
                "controllable_component_id": "robot-1",
                "command_target_id": "arm",
                "operation_capability_id": "execution.trajectory.execute",
                "source_epoch": "arm-source.1",
                "source_sequence": 7,
                "source_joint_names": ["joint1", "joint2"],
                "compiled_joint_names": ["robot__joint1", "robot__joint2"],
                "coordinate_kinds": ["angular", "angular"],
                "positions_si": [0.25, -0.25],
            }
        ],
    }

    crossed = _status()
    crossed["observation"]["targets"][0]["compiled_joint_names"] = ["robot__joint1"]
    with pytest.raises(
        stream.ExecutionObservationProjectionError,
        match="joint orders differ",
    ):
        stream.project_execution_observation(crossed, received_at_monotonic_us=123_457)

    inflated = _status()
    inflated["observation"]["diagnostics"] = {"unexpected": "cold data"}
    with pytest.raises(
        stream.ExecutionObservationProjectionError,
        match="schema is unsupported",
    ):
        stream.project_execution_observation(inflated, received_at_monotonic_us=123_458)


def test_gripper_only_changes_are_typed_and_not_deduplicated_as_idle_arm() -> None:
    response = _status()
    target = {
        "controllable_component_id": "robot-1",
        "command_target_id": "gripper",
        "operation_capability_id": "tool.gripper.command",
        "source_epoch": "gripper-source.1",
        "source_sequence": 1,
        "source_joint_names": ["finger"],
        "compiled_joint_names": ["tool__finger"],
        "coordinate_kinds": ["linear"],
        "positions_si": [0.025],
    }
    response["observation"]["targets"].append(target)
    before = stream.project_execution_observation(response, received_at_monotonic_us=1)
    target["positions_si"] = [0.005]
    target["source_sequence"] = 2
    response["observation"]["source_sequence"] += 1
    after = stream.project_execution_observation(response, received_at_monotonic_us=2)
    assert before["targets"][0] == after["targets"][0]
    assert after["targets"][1]["positions_si"] == [0.005]
    assert stream._render_signature(before) != stream._render_signature(after)
    target["coordinate_kinds"] = ["metres_as_radians"]
    with pytest.raises(stream.ExecutionObservationProjectionError):
        stream.project_execution_observation(response, received_at_monotonic_us=3)
    target["coordinate_kinds"] = ["linear"]
    target["compiled_joint_names"] = ["robot__joint1"]
    with pytest.raises(stream.ExecutionObservationProjectionError, match="overlap"):
        stream.project_execution_observation(response, received_at_monotonic_us=3)


def test_encode_execution_observation_frame_is_one_canonical_payload() -> None:
    frame = stream.project_execution_observation(
        _status(), received_at_monotonic_us=123_456
    )

    encoded = stream.encode_execution_observation_frame(frame)

    assert json.loads(encoded) == frame
    assert " " not in encoded
    assert encoded.startswith('{"cell_id":"cell-1",')


def test_failed_source_invalidates_every_subscriber_and_never_replays_cache() -> None:
    async def exercise() -> None:
        client = _Client()
        source = stream.ExecutionObservationSource(client)
        first, slow = source.subscribe(), source.subscribe()
        try:
            source.grant_credit(first, None)
            await _next_cursor(client)
            client.responses.put(_status(sequence=1))
            await asyncio.wait_for(first.wake.wait(), 1.0)
            source.take_payload(first)
            source.grant_credit(slow, None)
            # Slow browser has not taken its cached frame yet.
            source.grant_credit(first, ("execution-owner.1", "joint-source.1", 1))
            await _next_cursor(client)
            client.responses.put({"ok": False, "error": {"code": "source_lost"}})
            await asyncio.wait_for(first.wake.wait(), 1.0)
            for subscriber in (first, slow):
                with pytest.raises(stream.ExecutionObservationSourceUnavailable):
                    source.take_payload(subscriber)
                with pytest.raises(ValueError, match="closed"):
                    source.grant_credit(subscriber, None)
                source.unsubscribe(subscriber)
            replacement = source.subscribe()
            source.grant_credit(replacement, None)
            assert await _next_cursor(client) is None
            assert not replacement.wake.is_set()
            client.responses.put(_status(sequence=9))
            await asyncio.wait_for(replacement.wake.wait(), 1.0)
            assert json.loads(source.take_payload(replacement))["source_sequence"] == 9
            source.unsubscribe(replacement)
        finally:
            source.close()

    asyncio.run(exercise())


@pytest.mark.parametrize("fail_projection", [False, True])
def test_last_client_cancellation_fences_response_during_projection(
    monkeypatch, fail_projection
) -> None:
    entered, release = threading.Event(), threading.Event()
    original = stream.project_execution_observation

    def paused_projection(response, **kwargs):
        frame = original(response, **kwargs)
        if frame["source_sequence"] == 1:
            entered.set()
            assert release.wait(2.0)
            if fail_projection:
                raise stream.ExecutionObservationProjectionError(
                    "retired interval failed"
                )
        return frame

    monkeypatch.setattr(stream, "project_execution_observation", paused_projection)

    async def exercise() -> None:
        client = _Client()
        source = stream.ExecutionObservationSource(client)
        first = source.subscribe()
        try:
            source.grant_credit(first, None)
            await _next_cursor(client)
            client.responses.put(_status(sequence=1))
            assert await asyncio.to_thread(entered.wait, 1.0)
            source.unsubscribe(first)
            replacement = source.subscribe()
            source.grant_credit(replacement, None)
            release.set()
            assert await _next_cursor(client) is None
            assert not replacement.wake.is_set()
            client.responses.put(_status(sequence=2))
            await asyncio.wait_for(replacement.wake.wait(), 1.0)
            assert json.loads(source.take_payload(replacement))["source_sequence"] == 2
            assert source.status()["source"]["error_count"] == 0
            source.unsubscribe(replacement)
        finally:
            release.set()
            source.close()

    asyncio.run(exercise())


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


def test_websocket_closes_failed_source_and_reconnect_captures_current_only():
    upstream = _Client()
    source = stream.ExecutionObservationSource(upstream)
    app = FastAPI()
    app.include_router(
        create_viewer_router(
            SimpleNamespace(execution_observation_source=source), SecurityConfig()
        )
    )
    credit = {"schema_version": stream.OBSERVATION_CREDIT_SCHEMA, "last_applied": None}
    try:
        with TestClient(app) as client:
            with client.websocket_connect("/ws/scene/viewer") as socket:
                socket.send_json(credit)
                assert upstream.cursors.get(timeout=1) is None
                upstream.responses.put(_status(sequence=1))
                frame = socket.receive_json()
                assert frame["source_sequence"] == 1
                socket.send_json(
                    {
                        **credit,
                        "last_applied": {
                            key: frame[key]
                            for key in (
                                "owner_epoch",
                                "source_epoch",
                                "source_sequence",
                            )
                        },
                    }
                )
                assert upstream.cursors.get(timeout=1)["source_sequence"] == 1
                upstream.responses.put({"ok": False, "error": {"code": "source_lost"}})
                with pytest.raises(WebSocketDisconnect) as disconnected:
                    socket.receive_json()
                assert disconnected.value.code == 1013
                assert (
                    disconnected.value.reason
                    == "execution observation source unavailable"
                )
            with client.websocket_connect("/ws/scene/viewer") as replacement:
                replacement.send_json(credit)
                assert upstream.cursors.get(timeout=1) is None
                upstream.responses.put(_status(sequence=9))
                assert replacement.receive_json()["source_sequence"] == 9
    finally:
        source.close()


async def _wait_for_error(source: stream.ExecutionObservationSource) -> None:
    def wait_for_worker() -> bool:
        deadline = time.monotonic() + 1.0
        while time.monotonic() < deadline:
            if source.status()["source"]["error_count"] > 0:
                return True
            # This is cross-thread test synchronization, not product retry or
            # scheduling. Yield the GIL so the observation worker can publish
            # its terminal diagnostic even on a saturated qualification host.
            time.sleep(0.001)
        return False

    if not await asyncio.to_thread(wait_for_worker):
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
        assert (
            json.loads(source.take_payload(subscription) or "null")["source_sequence"]
            == 3
        )
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
            source.subscribe() for _ in range(stream.MAXIMUM_OBSERVATION_CLIENTS)
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
        assert first["targets"][0]["positions_si"] == [1.0, -1.0]
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
        assert latest["targets"][0]["positions_si"] == [0.2, -0.2]
        source.unsubscribe(subscription)
        source.close()

    asyncio.run(exercise())
