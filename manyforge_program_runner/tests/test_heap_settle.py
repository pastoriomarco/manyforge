"""Full collection and freeze at the runner's pause-tolerant points (OD-234)."""

from __future__ import annotations

import gc
import logging
import re
import weakref

import pytest

from manyforge_program_runner import heap_settle
from manyforge_program_runner.heap_settle import LOG_CODE, freeze_heap, settle_heap


class _Node:
    def __init__(self) -> None:
        self.peer: _Node | None = None


@pytest.fixture(autouse=True)
def _restore_collector():
    yield
    gc.unfreeze()
    while heap_settle._observe_collection in gc.callbacks:  # noqa: SLF001
        gc.callbacks.remove(heap_settle._observe_collection)  # noqa: SLF001
    heap_settle._full_collections[:] = [0, 0, 0, 0]  # noqa: SLF001


def _lines(caplog: pytest.LogCaptureFixture) -> list[dict[str, str]]:
    return [
        dict(re.findall(r"(\w+)=(\S+)", record.getMessage()))
        for record in caplog.records
        if record.getMessage().startswith(LOG_CODE + " ")
    ]


def test_unfreeze_then_collect_then_freeze() -> None:
    settle_heap("start")
    assert gc.get_freeze_count() > 0

    # A cycle that died while set aside is freed: unfreeze precedes collect.
    dead = _Node()
    dead.peer = _Node()
    dead.peer.peer = dead
    dead_ref = weakref.ref(dead)
    gc.freeze()
    del dead
    # A live object created since is set aside: freeze follows collect.
    live = _Node()

    settle_heap("load")

    assert dead_ref() is None
    assert all(item is not live for item in gc.get_objects())
    assert gc.get_count()[0] < 100


@pytest.mark.parametrize(
    "point",
    [
        ("start", settle_heap),
        ("load", settle_heap),
        ("reset", settle_heap),
        ("run_start", freeze_heap),
    ],
    ids=lambda point: point[0],
)
def test_a_line_only_at_a_point_after_an_automatic_full_collection(
    caplog: pytest.LogCaptureFixture, point
) -> None:
    name, at_point = point
    caplog.set_level(logging.WARNING, logger=heap_settle.__name__)

    settle_heap("start")
    # The points' own collections are not counted as automatic ones.
    at_point(name)
    settle_heap("reset")
    assert _lines(caplog) == []
    # Stands in for an automatic generation-2 collection between two points.
    gc.collect()
    at_point(name)
    # The counters restart at every point, written or not.
    at_point(name)

    lines = _lines(caplog)
    assert [line["point"] for line in lines] == [name]
    assert lines[0]["automatic_full_collections"] == "1"
    assert float(lines[0]["longest_automatic_full_collection_ms"]) >= 0.0
    assert float(lines[0]["collection_ms"]) >= 0.0
    if name == "run_start":
        assert lines[0]["collection_ms"] == "0.000"
        assert "frozen_objects" not in lines[0]
    else:
        assert int(lines[0]["frozen_objects"]) > 0
    assert gc.callbacks.count(heap_settle._observe_collection) == 1  # noqa: SLF001


def test_collection_observer_allocates_no_tracked_object() -> None:
    settle_heap("start")
    start = {"generation": 2, "collected": 0, "uncollectable": 0}
    young = {"generation": 0, "collected": 0, "uncollectable": 0}
    observe = heap_settle._observe_collection  # noqa: SLF001

    def tracked_allocations(rounds: int) -> int:
        calls = range(rounds)
        gc.disable()
        try:
            before = gc.get_count()[0]
            for _ in calls:
                observe("start", start)
                observe("stop", start)
                observe("start", young)
                observe("stop", young)
            return gc.get_count()[0] - before
        finally:
            gc.enable()

    # The same measurement without calls is the baseline (the first run of
    # the measuring code itself may allocate once).
    tracked_allocations(0)
    baseline = tracked_allocations(0)
    heap_settle._full_collections[0] = 0  # noqa: SLF001

    assert tracked_allocations(1_000) == baseline
    assert heap_settle._full_collections[0] == 1_000  # noqa: SLF001


def test_run_start_freezes_without_collecting_and_the_next_settle_frees(
    caplog: pytest.LogCaptureFixture,
) -> None:
    caplog.set_level(logging.WARNING, logger=heap_settle.__name__)
    settle_heap("load")
    collections: list[int] = []

    def observe(phase: str, info: dict) -> None:
        if phase == "start":
            collections.append(info["generation"])

    tree = _Node()
    tree.peer = _Node()
    tree.peer.peer = tree
    tree_ref = weakref.ref(tree)
    before = gc.get_freeze_count()
    gc.callbacks.append(observe)
    try:
        freeze_heap("run_start")
    finally:
        gc.callbacks.remove(observe)

    assert collections == []
    assert gc.get_freeze_count() > before
    assert all(item is not tree for item in gc.get_objects())
    del tree
    assert tree_ref() is not None
    settle_heap("reset")
    assert tree_ref() is None

    # No automatic full collection ran: no line at any of the three points.
    assert _lines(caplog) == []
