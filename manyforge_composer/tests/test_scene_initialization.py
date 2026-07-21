from __future__ import annotations

from collections import deque

import pytest

from manyforge_composer.backend.scene_initialization import (
    SceneInitializationBarrier,
)


def _barrier() -> SceneInitializationBarrier:
    times = iter(range(100, 1000))
    tokens = deque(["scene-init-one", "scene-init-two", "scene-init-three"])
    return SceneInitializationBarrier(
        now_ns=lambda: next(times),
        token_factory=tokens.popleft,
    )


def _snapshot(position: float) -> dict:
    return {
        "source": {
            "status": "ready",
            "received_at_ns": int(position * 10),
            "objects": [
                {
                    "object_id": "box",
                    "apply_policy": "continuous",
                    "position": position,
                }
            ],
        }
    }


def test_latest_eligible_object_replaces_older_sample_and_opens_once() -> None:
    barrier = _barrier()
    closed = barrier.close(deferred=True, reason="load")
    token = closed["token"]

    first = barrier.buffer_apply(
        source_id="source",
        policy="continuous",
        object_ids=["box"],
        require_idle=False,
        reason="first",
        snapshots=_snapshot(1.0),
    )
    second = barrier.buffer_apply(
        source_id="source",
        policy="continuous",
        object_ids=["box"],
        require_idle=False,
        reason="second",
        snapshots=_snapshot(2.0),
    )

    assert first["buffered_count"] == 1
    assert second["replacement_count"] == 1
    batch = barrier.take_replay_batch(token)
    assert batch.objects[("source", "box")]["object"]["position"] == 2.0
    barrier.record_replay(token, applied_count=1)
    opened = barrier.open_if_no_buffer(token, reason="complete")
    assert opened["state"] == "open"
    assert opened["counters"]["opened"] == 1
    assert opened["counters"]["replayed"] == 1


def test_new_generation_rejects_stale_reset_and_completion() -> None:
    barrier = _barrier()
    first = barrier.close(deferred=True, reason="first")
    second = barrier.close(deferred=True, reason="second")

    with pytest.raises(RuntimeError, match="stale scene initialization token"):
        barrier.prepare_reset(first["token"], reason="stale")
    with pytest.raises(RuntimeError, match="stale scene initialization token"):
        barrier.take_replay_batch(first["token"])

    assert barrier.prepare_reset(second["token"], reason="current") == second["token"]
    assert barrier.mark_seeded(second["token"])["state"] == "seeding"


def test_failed_replay_restores_batch_for_same_token_retry() -> None:
    barrier = _barrier()
    token = barrier.close(deferred=True, reason="load")["token"]
    barrier.buffer_apply(
        source_id="source",
        policy="continuous",
        object_ids=["box"],
        require_idle=False,
        reason="sample",
        snapshots=_snapshot(1.0),
    )
    batch = barrier.take_replay_batch(token)

    failed = barrier.restore_failed_batch(token, batch, error="seed failed")
    assert failed["state"] == "failed"
    assert failed["bufferedObjectCount"] == 1
    assert barrier.prepare_reset(token, reason="retry") == token
    retried = barrier.take_replay_batch(token)
    assert retried.requests["continuous"]["object_ids"] == {"box"}
