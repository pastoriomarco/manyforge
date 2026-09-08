"""Short isolated file-watcher cost probe; never starts runtime owners.

The envelope profiles exercise the source codec's byte/node ceilings, not a
qualified Manager/Scene payload or a whole-browser performance claim. Optional
captured files measure real payloads without modifying those files.
"""

from __future__ import annotations

import argparse
import asyncio
import inspect
import json
from pathlib import Path
import resource
from tempfile import TemporaryDirectory
import time

from manyforge_headless_api.control_state import HeadlessControlStateSource
from scripts.qualification.source_snapshot_fixture import source_snapshot_text


def distribution(samples):
    values = sorted(samples)
    return {
        "samples": len(values),
        "p95_ms": values[(len(values) * 95 - 1) // 100],
        "p99_ms": values[(len(values) * 99 - 1) // 100],
        "max_ms": values[-1],
    }


async def measure(paths, iterations):
    source = HeadlessControlStateSource(
        object(),
        None,
        program_runner_websocket_url=None,
        cold_watch_paths=paths,
    )
    durations, lag = [], []
    loop = asyncio.get_running_loop()
    cpu_start = time.process_time()
    started = loop.time()
    for _ in range(iterations):
        ready = loop.create_future()
        due = loop.time() + 0.005

        def tick():
            nonlocal due, handle
            lag.append(max(0, loop.time() - due) * 1000)
            if not ready.done():
                ready.set_result(None)
            due = loop.time() + 0.005
            handle = loop.call_at(due, tick)

        handle = loop.call_at(due, tick)
        try:
            before = loop.time()
            result = source._refresh_file_watches(notify=False)
            if inspect.isawaitable(result):
                await result
            durations.append((loop.time() - before) * 1000)
            if any(key is None for key in source._watch_keys.values()):
                raise RuntimeError("fixture did not pass the current source reader")
            await ready
        finally:
            handle.cancel()
    return {
        "source_bytes": [path.stat().st_size for path in paths],
        "batch": distribution(durations),
        "loop_lag": distribution(lag),
        "cpu_seconds": time.process_time() - cpu_start,
        "elapsed_seconds": loop.time() - started,
        "max_rss_kib": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("text", "nodes", "captured"), required=True)
    parser.add_argument("--path", type=Path, action="append", default=[])
    parser.add_argument("--iterations", type=int, default=20)
    arguments = parser.parse_args()
    if not 1 <= arguments.iterations <= 50:
        parser.error("iterations must be in [1, 50]")
    with TemporaryDirectory(prefix="ui6h-source-cost-") as temporary:
        if arguments.profile == "captured":
            if not 1 <= len(arguments.path) <= 4:
                parser.error("captured requires one to four --path arguments")
            paths = arguments.path
        else:
            # SourceSnapshotCodec currently uses the artifact decoder's
            # 250,000-node / 1-MiB-scalar ceilings as well as 16 MiB total.
            value = (
                ["x" * (1024**2 - 100)] * 16
                if arguments.profile == "text"
                else [1234567890] * (250_000 - 64)
            )
            encoded = source_snapshot_text(
                {
                    "api_version": "manyforge.source_snapshot.v1",
                    "payload": {"value": value},
                    "source_owner_epoch": "cost.1",
                    "source_owner_id": "cost",
                    "source_owner_kind": "qualification",
                }
            )
            paths = [
                Path(temporary) / name
                for name in (
                    "manager-source.json",
                    "logical-scene-source.json",
                    "scene-runtime-source.json",
                    "program-selection-owner.json",
                )
            ]
            for path in paths:
                path.write_text(encoded, encoding="utf-8")
            del value, encoded
        result = asyncio.run(measure(paths, arguments.iterations))
        print(
            json.dumps(
                {
                    "schema_version": "manyforge.source_cost.v1",
                    "profile": arguments.profile,
                    **result,
                },
                sort_keys=True,
            )
        )


if __name__ == "__main__":
    main()
