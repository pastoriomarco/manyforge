"""Demand-scoped, change-driven viewer projection of Manager pose rows.

The scene-runtime payload and the pose sessions come from the native mirror
of the Manager's status stream (OD-216); the stream's follower wakes every
viewer connection when either changes or the Manager starts or stops
answering. Nothing is shown while the Manager does not answer.
"""

from __future__ import annotations

import asyncio
from contextlib import suppress
import json
import time
from typing import Any, Callable

from fastapi import WebSocket, WebSocketDisconnect

from .control_state import _drained_owner_read
from .manager_status_stream import ManagerStatusChanges

_MAX_SAFE_INTEGER = 9_007_199_254_740_991


def _nanoseconds(value: Any) -> int:
    if type(value) is int and 0 <= value <= _MAX_SAFE_INTEGER:
        return value
    if (isinstance(value, str) and value.isascii() and value.isdecimal()
            and (value == "0" or not value.startswith("0")) and len(value) <= 19):
        parsed = int(value)
        if parsed <= 9_223_372_036_854_775_807:
            return parsed
    raise ValueError("invalid viewer pose timestamp")


def _project(payload: dict[str, Any] | None, sessions: dict[str, Any] | None) -> dict[str, Any]:
    if payload is None or sessions is None:
        return {"identity": None, "sources": []}
    state = payload.get("state")
    if not isinstance(state, dict) or state.get("state") != "open":
        return {"identity": None, "sources": []}
    runtime = state.get("runtime_identity")
    rows = state.get("pose_sources")
    if not isinstance(runtime, dict) or not isinstance(rows, list):
        raise ValueError("invalid viewer Scene runtime")
    identity = {
        "cell_id": payload.get("cell_id"),
        "retained_bundle_id": runtime.get("retained_bundle_id"),
        "graph_revision": runtime.get("graph_revision"),
    }
    if any(not isinstance(value, str) or not value for value in identity.values()):
        raise ValueError("invalid viewer Scene runtime identity")
    raw_sessions = sessions.get("sessions")
    if (set(sessions) != {"runtime_identity", "sessions"}
            or sessions["runtime_identity"] != runtime
            or not isinstance(raw_sessions, list) or len(raw_sessions) > 4096):
        raise ValueError("invalid viewer pose sessions")
    active = set()
    source_ids = set()
    for session in raw_sessions:
        if (not isinstance(session, dict)
                or set(session) != {"source_id", "source_epoch", "source_epoch_order"}
                or not isinstance(session["source_id"], str) or not session["source_id"]
                or not isinstance(session["source_epoch"], str) or not session["source_epoch"]
                or type(session["source_epoch_order"]) is not int
                or not 0 < session["source_epoch_order"] <= _MAX_SAFE_INTEGER
                or session["source_id"] in source_ids):
            raise ValueError("invalid viewer pose session")
        source_ids.add(session["source_id"])
        active.add((session["source_id"], session["source_epoch"], session["source_epoch_order"]))
    now = time.monotonic_ns()
    sources = []
    for row in rows:
        if not isinstance(row, dict) or row.get("policy_id") != "viewer.measured_robot":
            continue
        if (row.get("source_id"), row.get("source_epoch"), row.get("source_epoch_order")) not in active:
            continue
        sample = row.get("payload")
        if not isinstance(sample, dict) or sample.get("kind") != "joint_state":
            continue
        observed = _nanoseconds(row.get("observed_at_ns"))
        expires = _nanoseconds(row.get("expires_at_ns"))
        if observed > now or expires < observed:
            raise ValueError("invalid viewer pose freshness")
        sources.append({
            "source": row,
            "age_at_delivery_ns": min(now - observed, _MAX_SAFE_INTEGER),
            "freshness_budget_ns": min(expires - observed, _MAX_SAFE_INTEGER),
        })
    return {"identity": identity, "sources": sources}


async def serve_viewer_pose_stream(
    websocket: WebSocket,
    read_sources: Callable[[], tuple[str, str] | None] | None,
    changes: ManagerStatusChanges | None,
    cell_id: str | None,
    read_owner: Callable[[], dict[str, Any]] | None,
) -> None:
    """One pending credit and one latest source value; no Manager RPC per frame."""
    await websocket.accept()
    changed = asyncio.Event()
    ready = asyncio.Event()
    latest: tuple[dict[str, Any], dict[str, Any]] | None = None
    generation = 0
    verified_owner: str | None = None

    async def capture() -> tuple[dict[str, Any], dict[str, Any]] | None:
        nonlocal verified_owner
        assert read_sources is not None and cell_id is not None and read_owner is not None
        observed = await _drained_owner_read(read_sources)
        if observed is None:
            verified_owner = None
            return None
        payload = json.loads(observed[0])
        sessions = json.loads(observed[1])
        state = payload.get("state")
        if not isinstance(state, dict) or state.get("state") != "open":
            verified_owner = None
            return None
        identity = state.get("runtime_identity")
        if not isinstance(identity, dict):
            raise ValueError("invalid viewer Scene identity")
        if sessions.get("runtime_identity") != identity:
            return None
        identity_key = json.dumps(identity, allow_nan=False, sort_keys=True, separators=(",", ":"))
        candidate = identity_key
        if candidate != verified_owner:
            owner = await _drained_owner_read(read_owner)
            if (owner.get("cell_id") != cell_id
                    or owner.get("initialization_state") != "open"
                    or owner.get("runtime_identity") != identity):
                verified_owner = None
                return None
            verified_owner = candidate
        return payload, sessions

    async def watch() -> None:
        nonlocal latest, generation
        if read_sources is None or changes is None:
            ready.set()
            changed.set()
            await asyncio.Future()
            return
        # Registered before the first capture: a change that races it wakes
        # one more capture.
        source_changed = asyncio.Event()
        unregister = changes.register(asyncio.get_running_loop(), source_changed)
        try:
            while True:
                source_changed.clear()
                latest = await capture()
                generation += 1
                ready.set()
                changed.set()
                await source_changed.wait()
        finally:
            unregister()

    async def send() -> None:
        await ready.wait()
        while True:
            text = await websocket.receive_text()
            if len(text.encode("utf-8")) > 4096:
                raise ValueError("viewer Scene credit is oversized")
            credit = json.loads(text)
            if (not isinstance(credit, dict) or set(credit) != {"last_applied"}
                    or (credit["last_applied"] is not None
                        and (type(credit["last_applied"]) is not int
                             or credit["last_applied"] < 0))):
                raise ValueError("invalid viewer Scene credit")
            while credit["last_applied"] == generation:
                changed.clear()
                if credit["last_applied"] != generation:
                    break
                await changed.wait()
            projected = _project(*(latest or (None, None)))
            projected["generation"] = generation
            await asyncio.wait_for(
                websocket.send_text(json.dumps(projected, allow_nan=False, separators=(",", ":"))),
                timeout=0.25,
            )

    watcher = asyncio.create_task(watch())
    sender = asyncio.create_task(send())
    try:
        done, _ = await asyncio.wait({watcher, sender}, return_when=asyncio.FIRST_COMPLETED)
        for task in done:
            task.result()
    except (WebSocketDisconnect, ValueError, OSError, RuntimeError, asyncio.TimeoutError):
        pass
    finally:
        for task in (watcher, sender):
            task.cancel()
        await asyncio.gather(watcher, sender, return_exceptions=True)
        with suppress(WebSocketDisconnect, RuntimeError):
            await websocket.close()
