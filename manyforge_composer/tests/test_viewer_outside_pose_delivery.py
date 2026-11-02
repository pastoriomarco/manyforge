from __future__ import annotations

import asyncio
import json
import queue
import threading
from typing import Any

from manyforge_composer.backend.execution_observation_stream import ExecutionObservationSource


class Stream:
    def __init__(self) -> None:
        self.cursors: queue.Queue[Any] = queue.Queue()
        self.responses: queue.Queue[dict[str, Any]] = queue.Queue()

    def __enter__(self) -> Stream:
        return self

    def __exit__(self, *_exc: object) -> None:
        pass

    def next(self, cursor: Any, cancelled: threading.Event) -> dict[str, Any]:
        self.cursors.put(cursor)
        while not cancelled.is_set():
            try:
                return self.responses.get(timeout=0.01)
            except queue.Empty:
                pass
        raise RuntimeError('cancelled')


class Client:
    def __init__(self) -> None:
        self.execution = Stream()
        self.poses = Stream()

    def open_execution_observation_stream(self) -> Stream:
        return self.execution

    def open_viewer_pose_stream(self) -> Stream:
        return self.poses


def execution() -> dict[str, Any]:
    return {'api_version': 'manyforge.execution_command_response.v1', 'command': 'observation',
            'ok': True, 'observation': {
                'schema_version': 'manyforge.execution_observation.v4',
                'owner_epoch': 'owner', 'cell_id': 'cell', 'retained_bundle_id': 'bundle',
                'graph_revision': 'graph', 'source_epoch': 'execution', 'source_sequence': 1,
                'targets': [], 'measured_targets': [],
            }}


def pose(generation: int, position: float, *, graph: str = 'graph') -> dict[str, Any]:
    return {'generation': generation,
            'identity': {'cell_id': 'cell', 'retained_bundle_id': 'bundle', 'graph_revision': graph},
            'sources': [{'source': {'source_id': 'arm', 'position': position},
                         'age_at_delivery_ns': 200_000_000, 'freshness_budget_ns': 1_000_000_000}]}


def cursor(frame: dict[str, Any]) -> tuple[str, str, int, int]:
    return (frame['owner_epoch'], frame['source_epoch'], frame['source_sequence'], frame['delivery_sequence'])


def test_pose_only_updates_wake_credited_viewers_and_coalesce_per_browser() -> None:
    async def exercise() -> None:
        now = 10.0
        client = Client()
        source = ExecutionObservationSource(client, monotonic=lambda: now)
        fast, slow = source.subscribe(), source.subscribe()
        try:
            source.grant_credit(fast, None)
            await asyncio.to_thread(client.execution.cursors.get, True, 1)
            await asyncio.to_thread(client.poses.cursors.get, True, 1)
            client.poses.responses.put(pose(1, 0.1))
            assert await asyncio.to_thread(client.poses.cursors.get, True, 1) == 1
            client.execution.responses.put(execution())
            await asyncio.wait_for(fast.wake.wait(), 1)
            first = json.loads(source.take_payload(fast))
            assert first['outside_pose_sources'][0]['source']['position'] == 0.1
            source.grant_credit(slow, None)
            await asyncio.wait_for(slow.wake.wait(), 1)
            slow_first = json.loads(source.take_payload(slow))
            source.grant_credit(fast, cursor(first))
            # No new execution frame: the pose stream alone wakes the fast browser.
            client.poses.responses.put(pose(2, 0.2))
            assert await asyncio.to_thread(client.poses.cursors.get, True, 1) == 2
            await asyncio.wait_for(fast.wake.wait(), 1)
            second = json.loads(source.take_payload(fast))
            assert second['source_sequence'] == first['source_sequence']
            assert second['delivery_sequence'] > first['delivery_sequence']
            assert second['outside_pose_sources'][0]['source']['position'] == 0.2
            # Slow has an outstanding credit; intermediate updates are not queued.
            client.poses.responses.put(pose(3, 0.3))
            assert await asyncio.to_thread(client.poses.cursors.get, True, 1) == 3
            client.poses.responses.put(pose(4, 0.4))
            assert await asyncio.to_thread(client.poses.cursors.get, True, 1) == 4
            assert not slow.wake.is_set()
            now = 10.5
            source.grant_credit(slow, cursor(slow_first))
            await asyncio.wait_for(slow.wake.wait(), 1)
            latest = json.loads(source.take_payload(slow))
            assert latest['outside_pose_sources'][0]['source']['position'] == 0.4
            assert latest['outside_pose_sources'][0]['age_at_delivery_ns'] == 700_000_000
            source.grant_credit(fast, cursor(second))
            await asyncio.wait_for(fast.wake.wait(), 1)
            fourth = json.loads(source.take_payload(fast))
            assert fourth['outside_pose_sources'][0]['source']['position'] == 0.4
            source.grant_credit(fast, cursor(fourth))
            # A source from another applied model cannot be paired with this frame.
            client.poses.responses.put(pose(5, 0.5, graph='other'))
            assert await asyncio.to_thread(client.poses.cursors.get, True, 1) == 5
            await asyncio.wait_for(fast.wake.wait(), 1)
            changed = json.loads(source.take_payload(fast))
            assert changed['outside_pose_sources'] == []
            source.grant_credit(fast, cursor(changed))
            retired = pose(6, 0.6)
            retired['sources'] = []
            client.poses.responses.put(retired)
            assert await asyncio.to_thread(client.poses.cursors.get, True, 1) == 6
            await asyncio.wait_for(fast.wake.wait(), 1)
            assert json.loads(source.take_payload(fast))['outside_pose_sources'] == []
        finally:
            source.unsubscribe(fast)
            source.unsubscribe(slow)
            source.close()

    asyncio.run(exercise())
