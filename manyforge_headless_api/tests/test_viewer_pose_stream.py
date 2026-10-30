from __future__ import annotations

import asyncio
import json
from pathlib import Path

import pytest

from manyforge_headless_api import viewer_pose_stream as viewer


def payload() -> dict:
    return {
        "cell_id": "cell-a",
        "state": {
            "state": "open",
            "runtime_identity": {"retained_bundle_id": "bundle", "graph_revision": "graph"},
            "pose_sources": [{
                "policy_id": "viewer.measured_robot", "source_id": "observer",
                "source_epoch": "source-epoch", "source_epoch_order": 1,
                "observed_at_ns": 1_000, "expires_at_ns": 2_000,
                "payload": {"kind": "joint_state"}, "fresh": True,
            }],
        },
    }


def sessions(epoch="source-epoch") -> dict:
    return {"runtime_identity": payload()["state"]["runtime_identity"], "sessions": [
        {"source_id": "observer", "source_epoch": epoch, "source_epoch_order": 1}
    ]}


def test_projection_requires_current_viewer_session(monkeypatch):
    monkeypatch.setattr(viewer.time, "monotonic_ns", lambda: 1_250)
    result = viewer._project(payload(), sessions())
    assert result["sources"][0]["age_at_delivery_ns"] == 250
    assert result["sources"][0]["freshness_budget_ns"] == 1_000
    assert result["sources"][0]["source"] == payload()["state"]["pose_sources"][0]
    assert viewer._project(payload(), sessions("different"))["sources"] == []
    assert viewer._project(payload(), None)["sources"] == []


class Socket:
    def __init__(self) -> None:
        self.incoming: asyncio.Queue[str] = asyncio.Queue()
        self.outgoing: asyncio.Queue[dict] = asyncio.Queue()

    async def accept(self):
        pass

    async def receive_text(self):
        return await self.incoming.get()

    async def send_text(self, text):
        await self.outgoing.put(json.loads(text))

    async def close(self):
        pass


@pytest.mark.parametrize("epoch", ["epoch-a", "different-epoch"])
def test_registered_watch_precedes_capture_and_manager_epoch_is_checked(monkeypatch, epoch):
    async def exercise():
        registered, release = asyncio.Event(), asyncio.Event()
        reads = []
        owner_reads = []

        async def fake_watch(*_paths, **kwargs):
            assert kwargs["yield_on_timeout"] is True
            registered.set()
            await release.wait()
            yield set()  # first timeout acknowledges a registered watcher
            await asyncio.Future()

        def fake_read(*, path, cell_id, freshness_budget_ns):
            reads.append(path)
            assert cell_id == "cell-a"
            return payload(), "epoch-a"

        def fake_sessions(*, path, cell_id):
            assert path.name == "viewer-pose-sessions.json"
            assert cell_id == "cell-a"
            return sessions(), "epoch-a"

        def owner():
            owner_reads.append(True)
            return {"cell_id": "cell-a", "scene_runtime_owner_epoch": epoch,
                    "initialization_state": "open", "runtime_identity": payload()["state"]["runtime_identity"]}

        monkeypatch.setattr(viewer, "awatch", fake_watch)
        monkeypatch.setattr(viewer, "_read_scene", fake_read)
        monkeypatch.setattr(viewer, "_read_sessions", fake_sessions)
        monkeypatch.setattr(viewer.time, "monotonic_ns", lambda: 1_250)
        socket = Socket()
        task = asyncio.create_task(viewer.serve_viewer_pose_stream(
            socket, Path("/tmp/scene-runtime-source.json"), "cell-a", 2_000_000_000, owner
        ))
        try:
            await registered.wait()
            assert reads == []
            await socket.incoming.put(json.dumps({"last_applied": None}))
            assert socket.outgoing.empty()
            release.set()
            result = await asyncio.wait_for(socket.outgoing.get(), 1)
            assert reads and owner_reads == [True]
            assert len(result["sources"]) == (1 if epoch == "epoch-a" else 0)
        finally:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)

    asyncio.run(exercise())


def test_session_replacement_wakes_quiet_scene_and_filters_old_epoch(monkeypatch):
    async def exercise():
        events: asyncio.Queue = asyncio.Queue()
        registered = asyncio.Event()
        current_pose = payload()
        current_sessions = sessions()
        owner_reads = []

        async def fake_watch(*_paths, **_kwargs):
            registered.set()
            while True:
                yield await events.get()

        def fake_read(*, path, cell_id, freshness_budget_ns):
            return current_pose, "epoch-a"

        def fake_sessions(*, path, cell_id):
            return current_sessions, "epoch-a"

        def owner():
            owner_reads.append(True)
            return {"cell_id": "cell-a", "scene_runtime_owner_epoch": "epoch-a",
                    "initialization_state": "open", "runtime_identity": payload()["state"]["runtime_identity"]}

        monkeypatch.setattr(viewer, "awatch", fake_watch)
        monkeypatch.setattr(viewer, "_read_scene", fake_read)
        monkeypatch.setattr(viewer, "_read_sessions", fake_sessions)
        monkeypatch.setattr(viewer.time, "monotonic_ns", lambda: 1_250)
        socket = Socket()
        task = asyncio.create_task(viewer.serve_viewer_pose_stream(
            socket, Path("/tmp/scene-runtime-source.json"), "cell-a", 2_000_000_000, owner
        ))
        try:
            await registered.wait()
            await events.put(set())
            await socket.incoming.put(json.dumps({"last_applied": None}))
            first = await asyncio.wait_for(socket.outgoing.get(), 1)
            assert len(first["sources"]) == 1
            current_sessions["sessions"][0]["source_epoch"] = "source-epoch-2"
            await events.put({(1, "/tmp/viewer-pose-sessions.json")})
            await socket.incoming.put(json.dumps({"last_applied": first["generation"]}))
            replaced = await asyncio.wait_for(socket.outgoing.get(), 1)
            assert replaced["sources"] == []
            current_pose["state"]["pose_sources"][0]["source_epoch"] = "source-epoch-2"
            await events.put({(1, "/tmp/scene-runtime-source.json")})
            await socket.incoming.put(json.dumps({"last_applied": replaced["generation"]}))
            latest = await asyncio.wait_for(socket.outgoing.get(), 1)
            assert len(latest["sources"]) == 1
            assert owner_reads == [True]  # pose/session changes do not query Manager
        finally:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)

    asyncio.run(exercise())
