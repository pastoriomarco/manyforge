"""Follower of the Manager's status stream (OD-216), reader side.

A fake Manager serves ``manager-status.sock`` in a real run root; a fake core
records what the follower applies. Real sockets, short real bounds.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import socket
import threading
import time
from typing import Any

import pytest

from manyforge_headless_api.manager_status_stream import (
    MANAGER_STATUS_SOCKET_FILE,
    ManagerStatusFollower,
)
from manyforge_headless_api.native import HeadlessApiError


def _frame(kind: str, tick: int, **members: Any) -> bytes:
    return json.dumps(
        {
            "api_version": "manyforge.manager_status_stream.v1",
            "application_start_id": "0123456789abcdef",
            "tick": tick,
            "type": kind,
            **members,
        },
        sort_keys=True,
        separators=(",", ":"),
    ).encode()


class FakeCore:
    """Applies frames as the native mirror reports them."""

    def __init__(self) -> None:
        self.frames: list[dict[str, Any]] = []
        self.disconnects = 0
        self._tick = -1
        self._connected = False

    def apply_manager_status_frame(self, frame: bytes) -> dict[str, Any]:
        value = json.loads(frame)
        if value["type"] == "error":
            raise HeadlessApiError(value["error"]["code"], value["error"]["message"])
        self.frames.append(value)
        if value["type"] == "snapshot":
            self._connected = True
            self._tick = value["tick"]
            return {"control_changed": True, "tick_advanced": True, "viewer_changed": True}
        advanced = value["tick"] > self._tick
        self._tick = max(self._tick, value["tick"])
        section = value.get("name")
        return {
            "control_changed": section in ("manager", "logical_scene", "scene_runtime"),
            "tick_advanced": advanced,
            "viewer_changed": section in ("scene_runtime", "viewer_pose_sessions"),
        }

    def mark_manager_status_disconnected(self) -> bool:
        was = self._connected
        self._connected = False
        self.disconnects += 1
        return was


class FakeManager:
    """Accepts status subscribers on a real socket; the test sends frames."""

    def __init__(self, run_root: Path) -> None:
        self.path = run_root / MANAGER_STATUS_SOCKET_FILE
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.listener.bind(str(self.path))
        os.chmod(self.path, 0o600)
        self.listener.listen(4)
        self.listener.settimeout(5.0)

    def accept(self) -> socket.socket:
        client, _ = self.listener.accept()
        return client

    def close(self) -> None:
        self.listener.close()
        self.path.unlink(missing_ok=True)


class Wakes:
    def __init__(self) -> None:
        self.control = 0
        self.viewer = 0
        self.changed = threading.Condition()

    def on_control(self) -> None:
        with self.changed:
            self.control += 1
            self.changed.notify_all()

    def on_viewer(self) -> None:
        with self.changed:
            self.viewer += 1
            self.changed.notify_all()

    def wait(self, predicate, timeout: float = 3.0) -> bool:
        with self.changed:
            return self.changed.wait_for(predicate, timeout)


def _until(predicate, timeout: float = 3.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.005)
    return predicate()


@pytest.fixture
def run_root(tmp_path: Path) -> Path:
    root = tmp_path / "manager-run"
    root.mkdir(mode=0o700)
    return root


def _follower(core: FakeCore, run_root: Path, wakes: Wakes, silence: float = 0.4):
    return ManagerStatusFollower(
        core,
        run_root,
        silence_seconds=silence,
        on_control_change=wakes.on_control,
        on_viewer_change=wakes.on_viewer,
        backoff_seconds=(0.02, 0.05),
    )


def test_full_state_at_connect_and_each_change_reaches_the_mirror(run_root: Path) -> None:
    manager = FakeManager(run_root)
    core, wakes = FakeCore(), Wakes()
    follower = _follower(core, run_root, wakes)
    follower.start()
    try:
        connection = manager.accept()
        connection.send(_frame("snapshot", 1, generations={
            "logical_scene": 1, "manager": 1, "scene_runtime": 0, "viewer_pose_sessions": 0}))
        connection.send(_frame("section", 1, name="manager", section={
            "generation": 1, "observed_at_ns": 1, "oversized": False, "payload": {}}))
        assert _until(lambda: len(core.frames) == 2)
        assert [frame["type"] for frame in core.frames] == ["snapshot", "section"]
        assert wakes.wait(lambda: wakes.control >= 2 and wakes.viewer >= 1)
        viewer_before = wakes.viewer
        # A pose-only change wakes the viewer, not the control state.
        control_before = wakes.control
        connection.send(_frame("section", 1, name="viewer_pose_sessions", section={
            "generation": 1, "observed_at_ns": 1, "oversized": False, "payload": {}}))
        assert wakes.wait(lambda: wakes.viewer > viewer_before)
        assert wakes.control == control_before
        connection.close()
    finally:
        follower.shutdown()
        manager.close()


def test_silence_marks_not_answering_once_and_a_new_tick_restores_it(run_root: Path) -> None:
    manager = FakeManager(run_root)
    core, wakes = FakeCore(), Wakes()
    follower = _follower(core, run_root, wakes, silence=0.3)
    follower.start()
    try:
        connection = manager.accept()
        connection.send(_frame("snapshot", 1, generations={
            "logical_scene": 0, "manager": 0, "scene_runtime": 0, "viewer_pose_sessions": 0}))
        assert wakes.wait(lambda: wakes.control == 1)
        started = time.monotonic()
        # A frame with the same tick (a lane publishing while the loop is
        # held) does not extend the deadline.
        connection.send(_frame("section", 1, name="viewer_pose_sessions", section={
            "generation": 1, "observed_at_ns": 1, "oversized": False, "payload": {}}))
        assert wakes.wait(lambda: wakes.control == 2, timeout=2.0)
        silent_after = time.monotonic() - started
        assert 0.25 <= silent_after < 1.0
        # One wake for the silence; the connection is kept (no reconnect).
        time.sleep(0.4)
        assert wakes.control == 2 and core.disconnects == 0
        connection.send(_frame("keepalive", 2))
        assert wakes.wait(lambda: wakes.control == 3)
        assert core.disconnects == 0
        connection.close()
    finally:
        follower.shutdown()
        manager.close()


def test_the_connection_end_is_lost_at_once_and_the_follower_resubscribes(run_root: Path) -> None:
    manager = FakeManager(run_root)
    core, wakes = FakeCore(), Wakes()
    follower = _follower(core, run_root, wakes, silence=10.0)
    follower.start()
    try:
        first = manager.accept()
        first.send(_frame("snapshot", 5, generations={
            "logical_scene": 0, "manager": 1, "scene_runtime": 0, "viewer_pose_sessions": 0}))
        assert wakes.wait(lambda: wakes.control == 1)
        closed = time.monotonic()
        first.close()  # The Manager ended (or restarted).
        assert _until(lambda: core.disconnects == 1, timeout=1.0)
        assert time.monotonic() - closed < 0.5  # Not the 10 s silence bound.
        assert wakes.wait(lambda: wakes.control == 2)
        # The restarted Manager: a new subscription and its new state.
        second = manager.accept()
        second.send(_frame("snapshot", 1, generations={
            "logical_scene": 0, "manager": 1, "scene_runtime": 0, "viewer_pose_sessions": 0}))
        second.send(_frame("section", 1, name="manager", section={
            "generation": 1, "observed_at_ns": 9, "oversized": False, "payload": {"new": True}}))
        assert _until(lambda: core.frames[-1].get("name") == "manager")
        assert core.frames[-1]["section"]["payload"] == {"new": True}
        second.close()
    finally:
        follower.shutdown()
        manager.close()


def test_an_error_frame_reconnects_and_an_absent_socket_is_retried(run_root: Path) -> None:
    core, wakes = FakeCore(), Wakes()
    follower = _follower(core, run_root, wakes)
    follower.start()
    try:
        time.sleep(0.2)  # No socket yet: the Manager has not created it.
        assert core.frames == []
        manager = FakeManager(run_root)
        refused = manager.accept()
        refused.send(json.dumps({
            "api_version": "manyforge.manager_status_stream.v1",
            "error": {"code": "manager_status_subscribers_full", "details": {}, "message": "full"},
            "type": "error",
        }).encode())
        refused.close()
        served = manager.accept()  # The follower retried.
        served.send(_frame("snapshot", 1, generations={
            "logical_scene": 0, "manager": 0, "scene_runtime": 0, "viewer_pose_sessions": 0}))
        assert _until(lambda: core.frames and core.frames[-1]["type"] == "snapshot")
        served.close()
        manager.close()
    finally:
        follower.shutdown()


def test_shutdown_ends_the_follower_within_one_receive_slice(run_root: Path) -> None:
    manager = FakeManager(run_root)
    core, wakes = FakeCore(), Wakes()
    follower = _follower(core, run_root, wakes, silence=10.0)
    follower.start()
    try:
        connection = manager.accept()
        connection.send(_frame("snapshot", 1, generations={
            "logical_scene": 0, "manager": 0, "scene_runtime": 0, "viewer_pose_sessions": 0}))
        assert wakes.wait(lambda: wakes.control == 1)
        started = time.monotonic()
        follower.shutdown()
        assert time.monotonic() - started < 0.6
        connection.close()
    finally:
        manager.close()
