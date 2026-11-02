from __future__ import annotations

import asyncio
import json
from types import SimpleNamespace

import pytest

from manyforge_composer.backend.execution_observation_stream import OBSERVATION_CREDIT_SCHEMA
from manyforge_composer.backend.routes_viewer import _credit_identity, create_viewer_router
from manyforge_composer.backend.security import SecurityConfig


def test_credit_requires_the_configured_pose_rate() -> None:
    base = {"schema_version": OBSERVATION_CREDIT_SCHEMA, "last_applied": None}
    assert _credit_identity(json.dumps({**base, "pose_rate_hz": 5})) == (None, 5)
    assert _credit_identity(json.dumps({**base, "pose_rate_hz": 60})) == (None, 60)
    with pytest.raises(ValueError):
        _credit_identity(json.dumps(base))
    for value in (None, True, 4, 61, 30.5):
        with pytest.raises(ValueError):
            _credit_identity(json.dumps({**base, "pose_rate_hz": value}))


def test_credit_rate_paces_and_coalesces_the_next_viewer_frame() -> None:
    class Subscription:
        def __init__(self):
            self.wake = asyncio.Event()
            self.payload = None

    class Source:
        def __init__(self):
            self.subscription = Subscription()
            self.latest = "A"
            self.credits = []

        def subscribe(self):
            return self.subscription

        def unsubscribe(self, _subscription):
            pass

        def grant_credit(self, subscription, identity):
            self.credits.append(identity)
            subscription.payload = self.latest
            subscription.wake.set()

        def take_payload(self, subscription):
            subscription.wake.clear()
            payload, subscription.payload = subscription.payload, None
            return payload

    class Socket:
        def __init__(self):
            self.incoming = asyncio.Queue()
            self.outgoing = asyncio.Queue()

        async def accept(self):
            pass

        async def receive_text(self):
            return await self.incoming.get()

        async def send_text(self, payload):
            await self.outgoing.put((asyncio.get_running_loop().time(), payload))

        async def close(self):
            pass

    async def exercise():
        source, socket = Source(), Socket()
        router = create_viewer_router(SimpleNamespace(execution_observation_source=source), SecurityConfig())
        endpoint = next(route.endpoint for route in router.routes if getattr(route, "path", None) == "/ws/scene/viewer")
        task = asyncio.create_task(endpoint(socket))
        initial = {"schema_version": OBSERVATION_CREDIT_SCHEMA, "last_applied": None, "pose_rate_hz": 5}
        try:
            await socket.incoming.put(json.dumps(initial))
            first_time, first = await asyncio.wait_for(socket.outgoing.get(), 1)
            assert first == "A"
            cursor = {"owner_epoch": "owner", "source_epoch": "source", "source_sequence": 1, "delivery_sequence": 1}
            await socket.incoming.put(json.dumps({**initial, "last_applied": cursor}))
            source.latest = "B"
            await asyncio.sleep(0.05)
            assert source.credits == [None]
            source.latest = "C"
            second_time, second = await asyncio.wait_for(socket.outgoing.get(), 1)
            assert second == "C"
            assert second_time - first_time >= 0.19
        finally:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)

    asyncio.run(exercise())
