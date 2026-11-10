from __future__ import annotations

import asyncio
import json
import os
from pathlib import Path
import threading

import pytest

from manyforge_headless_api import control_state as module
from manyforge_headless_api.tests.source_snapshot_fixture import source_snapshot_text


def test_file_hint_is_bounded_and_rejects_nonregular_or_invalid_inputs(tmp_path):
    path = tmp_path / "manager-source.json"
    payload = {
        "payload": {"health": "ready"},
        "source_owner_epoch": "cost.1",
        "source_owner_id": "manager",
        "source_owner_kind": "component_manager",
    }
    path.write_text(source_snapshot_text(payload), encoding="utf-8")
    key, modified = module._source_file_observation(path)
    assert key is not None and len(key) == 71
    assert modified == path.stat().st_mtime_ns
    with path.open("wb") as stream:
        stream.truncate(16 * 1024 * 1024 + 1)
    assert module._source_file_observation(path) == (None, None)
    path.write_text('{"payload":', encoding="utf-8")
    assert module._source_file_observation(path) == (None, None)
    path.write_text("[" * 2000 + "]" * 2000, encoding="utf-8")
    assert module._source_file_observation(path) == (None, None)
    path.unlink()
    os.mkfifo(path)
    assert module._source_file_observation(path) == (None, None)
    path.unlink()
    path.symlink_to(tmp_path / "missing")
    assert module._source_file_observation(path) == (None, None)
    path.unlink()
    path.mkdir()
    assert module._source_file_observation(path) == (None, None)


@pytest.mark.parametrize("corruption", ("digest", "generation", "duplicate", "owner"))
def test_same_meaning_corrupt_source_wakes_invalidation_and_repair(tmp_path, corruption):
    path = tmp_path / "manager-source.json"
    valid = source_snapshot_text(
        {
            "payload": {"health": "ready"},
            "source_owner_epoch": "cost.1",
            "source_owner_id": "manager",
            "source_owner_kind": "component_manager",
        }
    )
    source = module.HeadlessControlStateSource(
        object(),
        None,
        program_runner_websocket_url=None,
        cold_watch_paths=(path,),
    )
    wakes = []
    source.notify_cold = lambda: wakes.append(True)
    path.write_text(valid, encoding="utf-8")
    asyncio.run(source._refresh_file_watches(notify=False))
    if corruption == "duplicate":
        invalid = valid[:-1] + ',"source_generation":1}'
    else:
        invalid = json.loads(valid)
        if corruption == "digest":
            invalid["record_digest"] = "sha256:" + "0" * 64
        elif corruption == "generation":
            invalid["source_generation"] = 0
        else:
            invalid["source_owner_epoch"] = "cost.\n1"
        # Correct hashes must not make an invalid closed envelope acceptable.
        invalid = (
            json.dumps(invalid, sort_keys=True, separators=(",", ":"))
            if corruption == "digest"
            else source_snapshot_text(invalid)
        )
    path.write_text(invalid, encoding="utf-8")
    asyncio.run(source._refresh_file_watches(notify=True))
    assert source._watch_keys[path] is None
    assert source._watch_modified[path] is None
    assert wakes == [True]
    path.write_text(valid, encoding="utf-8")
    asyncio.run(source._refresh_file_watches(notify=True))
    assert source._watch_keys[path] is not None
    assert wakes == [True, True]


def test_root_only_replacement_releases_and_reregisters_watcher(tmp_path, monkeypatch):
    async def exercise():
        old_closed, recovered = asyncio.Event(), asyncio.Event()
        removed = asyncio.Event()
        captures, registrations = [], []
        path = tmp_path / "program-selection-owner.json"  # No expiry timer.

        def read(*, path):
            captures.append(path)
            return f"capture.{len(captures)}", 1

        async def watches(*roots):
            registrations.append(roots)
            if len(registrations) == 1:
                try:
                    await removed.wait()
                    yield {(3, str(tmp_path))}
                finally:
                    old_closed.set()
            else:
                assert old_closed.is_set()
                recovered.set()
                await asyncio.Event().wait()

        monkeypatch.setattr(module, "_source_file_observation", read)
        monkeypatch.setattr(module, "awatch", watches)
        monkeypatch.setattr(module, "_SOURCE_RETRY_INITIAL_SECONDS", 0)
        source = module.HeadlessControlStateSource(
            object(),
            None,
            program_runner_websocket_url=None,
            cold_watch_paths=(path,),
        )
        source._loop = asyncio.get_running_loop()
        source._source_transition = lambda *_args, **_kwargs: None
        subscription = module.ControlStateSubscription(asyncio.Event())
        source._subscriptions.add(subscription)
        source._watch_task = asyncio.create_task(source._run_file_wakes())
        try:
            removed.set()
            await asyncio.wait_for(recovered.wait(), 1)
            assert len(registrations) == 2
            assert captures == [path, path]
            assert source._watch_keys[path] == "capture.2"
        finally:
            await source.unsubscribe(subscription)

    asyncio.run(exercise())


def test_unknown_manager_counters_do_not_hide_meaningful_health_changes(tmp_path):
    path = tmp_path / "manager-source.json"
    hints = []
    for health in ("starting", "unavailable", "ready"):
        path.write_text(
            source_snapshot_text(
                {
                    "payload": {
                        "runtime": {
                            "observed_units": [
                                {
                                    "process_observation_sequence": None,
                                    "source_report_sequence": None,
                                    "source_health": health,
                                }
                            ]
                        }
                    },
                    "source_owner_epoch": "manager.1",
                    "source_owner_id": "manager",
                    "source_owner_kind": "component_manager",
                }
            ),
            encoding="utf-8",
        )
        hint, modified = module._source_file_observation(path)
        assert hint is not None and modified is not None
        hints.append(hint)
    assert len(set(hints)) == 3


@pytest.mark.parametrize("invalid", (None, {}, "missing"))
def test_invalid_scene_pose_shape_cannot_be_coalesced_as_a_valid_pulse(tmp_path, invalid):
    path = tmp_path / "scene-runtime-source.json"
    document = {
        "payload": {"state": {"state": "open", "pose_sources": []}},
        "source_owner_epoch": "scene.1",
        "source_owner_id": "scene",
        "source_owner_kind": "scene_runtime",
    }
    path.write_text(source_snapshot_text(document), encoding="utf-8")
    assert module._source_file_observation(path)[0] is not None
    if invalid == "missing":
        document["payload"]["state"].pop("pose_sources")
    else:
        document["payload"]["state"]["pose_sources"] = invalid
    path.write_text(source_snapshot_text(document), encoding="utf-8")
    assert module._source_file_observation(path) == (None, None)


def test_expiry_uses_cached_inode_time_and_emits_one_transition(tmp_path, monkeypatch):
    now = [10_000]
    monkeypatch.setattr(module.time, "time_ns", lambda: now[0])
    path = tmp_path / "manager-source.json"
    monkeypatch.setattr(module, "_source_file_observation", lambda **_: ("one", now[0]))
    source = module.HeadlessControlStateSource(
        object(),
        None,
        program_runner_websocket_url=None,
        cold_watch_paths=(path,),
        cold_freshness_paths=(path,),
        cold_freshness_budget_ns=100,
    )
    wakes = []
    source.notify_cold = lambda: wakes.append(True)
    asyncio.run(source._refresh_file_watches(notify=False))
    monkeypatch.setattr(Path, "stat", lambda *_: pytest.fail("expiry must not inspect files"))
    monkeypatch.setattr(
        module,
        "_source_file_observation",
        lambda **_: pytest.fail("expiry must not read"),
    )
    now[0] += 101
    source._refresh_file_freshness(notify=True)
    source._refresh_file_freshness(notify=True)
    assert wakes == [True]
    assert source._watch_fresh == {path: False}
    source._watch_modified[path] = now[0] + 1_000_000
    source._refresh_file_freshness(notify=True)
    assert source._watch_fresh == {path: False}  # Future mtime is not fresh.


def test_watcher_reads_only_changed_paths_off_loop_and_drains_on_teardown(tmp_path, monkeypatch):
    async def exercise():
        loop = asyncio.get_running_loop()
        loop_thread = threading.get_ident()
        paths = (
            tmp_path / "manager-source.json",
            tmp_path / "logical-scene-source.json",
        )
        observed, entered = asyncio.Event(), asyncio.Event()
        release = threading.Event()
        next_change = asyncio.Event()
        calls = []

        def read(*, path):
            assert threading.get_ident() != loop_thread
            calls.append(path)
            if len(calls) == 3:
                loop.call_soon_threadsafe(entered.set)
                assert release.wait(2)
            return "hint", 1

        async def watches(*_roots):
            observed.set()
            await next_change.wait()
            yield {(1, str(paths[0])), (1, str(tmp_path / "unrelated.tmp"))}
            await asyncio.Event().wait()

        monkeypatch.setattr(module, "_source_file_observation", read)
        monkeypatch.setattr(module, "awatch", watches)
        source = module.HeadlessControlStateSource(
            object(),
            None,
            program_runner_websocket_url=None,
            cold_watch_paths=paths,
        )
        source._loop = loop
        subscription = module.ControlStateSubscription(asyncio.Event())
        source._subscriptions.add(subscription)
        task = source._watch_task = asyncio.create_task(source._run_file_wakes())
        close = None
        try:
            await asyncio.wait_for(observed.wait(), 1)
            assert calls == list(paths)
            next_change.set()
            await asyncio.wait_for(entered.wait(), 1)
            # The loop can service this event while the physical read is blocked.
            assert calls == [*paths, paths[0]]
            close = asyncio.create_task(source.unsubscribe(subscription))
            await asyncio.sleep(0)
            await asyncio.sleep(0)
            assert not close.done()
            assert not task.done()
            release.set()
            await asyncio.wait_for(close, 1)
            assert source._watch_task is None
            assert source._watch_modified == {}
            assert source._watch_keys == {}
        finally:
            release.set()
            if close is not None:
                await close
            else:
                await source.unsubscribe(subscription)

    asyncio.run(exercise())
