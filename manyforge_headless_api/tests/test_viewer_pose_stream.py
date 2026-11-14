from __future__ import annotations

import asyncio
import json

from manyforge_headless_api import viewer_pose_stream as viewer
from manyforge_headless_api.manager_status_stream import ManagerStatusChanges


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


class Sources:
    """The mirror's ``viewer_pose_sources``: JSON texts, or None while the
    Manager does not answer. Called off the event loop."""

    def __init__(self) -> None:
        self.pose: dict | None = payload()
        self.sessions: dict = sessions()
        self.reads = 0

    def __call__(self) -> tuple[str, str] | None:
        self.reads += 1
        if self.pose is None:
            return None
        return json.dumps(self.pose), json.dumps(self.sessions)


def owner_for(identity_of, owner_reads):
    def owner():
        owner_reads.append(True)
        return {"cell_id": "cell-a", "initialization_state": "open",
                "runtime_identity": identity_of()}

    return owner


async def _next(socket: Socket, last_applied) -> dict:
    await socket.incoming.put(json.dumps({"last_applied": last_applied}))
    return await asyncio.wait_for(socket.outgoing.get(), 1)


def test_change_registration_precedes_capture_and_scene_owner_is_checked_on_identity_change(
    monkeypatch,
):
    async def exercise():
        changes = ManagerStatusChanges()
        sources = Sources()
        registered_at_read = []
        owner_reads = []

        def read_sources():
            registered_at_read.append(len(changes._waiters))  # noqa: SLF001
            return sources()

        monkeypatch.setattr(viewer.time, "monotonic_ns", lambda: 1_250)
        socket = Socket()
        task = asyncio.create_task(viewer.serve_viewer_pose_stream(
            socket, read_sources, changes, "cell-a",
            owner_for(lambda: sources.pose["state"]["runtime_identity"], owner_reads),
        ))
        try:
            first = await _next(socket, None)
            assert registered_at_read == [1]
            assert owner_reads == [True]
            assert len(first["sources"]) == 1
            assert first["identity"] == {
                "cell_id": "cell-a", "retained_bundle_id": "bundle", "graph_revision": "graph",
            }

            # A change of the same Scene identity does not ask the owner again.
            sources.pose["state"]["pose_sources"][0]["observed_at_ns"] = 1_100
            changes.notify()
            same = await _next(socket, first["generation"])
            assert same["sources"][0]["age_at_delivery_ns"] == 150
            assert owner_reads == [True]

            # A new Scene identity is checked with the owner once.
            identity = {"retained_bundle_id": "bundle", "graph_revision": "graph-2"}
            sources.pose["state"]["runtime_identity"] = identity
            sources.sessions["runtime_identity"] = identity
            changes.notify()
            replaced = await _next(socket, same["generation"])
            assert replaced["identity"]["graph_revision"] == "graph-2"
            assert owner_reads == [True, True]
        finally:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
        assert changes._waiters == set()  # noqa: SLF001

    asyncio.run(exercise())


def test_session_replacement_wakes_quiet_scene_and_filters_old_epoch(monkeypatch):
    async def exercise():
        changes = ManagerStatusChanges()
        sources = Sources()
        owner_reads = []
        monkeypatch.setattr(viewer.time, "monotonic_ns", lambda: 1_250)
        socket = Socket()
        task = asyncio.create_task(viewer.serve_viewer_pose_stream(
            socket, sources, changes, "cell-a",
            owner_for(lambda: payload()["state"]["runtime_identity"], owner_reads),
        ))
        try:
            first = await _next(socket, None)
            assert len(first["sources"]) == 1

            # Quiet viewer: the credit waits until the follower reports a change.
            await socket.incoming.put(json.dumps({"last_applied": first["generation"]}))
            await asyncio.sleep(0.05)
            assert socket.outgoing.empty()
            sources.sessions["sessions"][0]["source_epoch"] = "source-epoch-2"
            changes.notify()
            replaced = await asyncio.wait_for(socket.outgoing.get(), 1)
            assert replaced["generation"] > first["generation"]
            assert replaced["sources"] == []

            sources.pose["state"]["pose_sources"][0]["source_epoch"] = "source-epoch-2"
            changes.notify()
            latest = await _next(socket, replaced["generation"])
            assert len(latest["sources"]) == 1
            assert owner_reads == [True]  # pose/session changes do not query the owner
        finally:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)

    asyncio.run(exercise())


def test_manager_not_answering_clears_the_viewer(monkeypatch):
    async def exercise():
        changes = ManagerStatusChanges()
        sources = Sources()
        owner_reads = []
        monkeypatch.setattr(viewer.time, "monotonic_ns", lambda: 1_250)
        socket = Socket()
        task = asyncio.create_task(viewer.serve_viewer_pose_stream(
            socket, sources, changes, "cell-a",
            owner_for(lambda: payload()["state"]["runtime_identity"], owner_reads),
        ))
        try:
            first = await _next(socket, None)
            assert len(first["sources"]) == 1

            sources.pose = None  # the Manager stopped answering
            changes.notify()
            cleared = await _next(socket, first["generation"])
            assert cleared["generation"] > first["generation"]
            assert {key: cleared[key] for key in ("identity", "sources")} == {
                "identity": None, "sources": [],
            }

            # Answering again re-checks the Scene owner before showing poses.
            sources.pose = payload()
            changes.notify()
            restored = await _next(socket, cleared["generation"])
            assert len(restored["sources"]) == 1
            assert owner_reads == [True, True]
        finally:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)

    asyncio.run(exercise())
