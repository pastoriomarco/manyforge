"""Short real-native History/ASGI contention probe, with no robot processes.

Uses the current app, native recorder, bounded writer, SQLite durability and
retention. The in-process HTTP transport excludes network/browser costs. The
small retention capacity deliberately exercises pruning, not the maximum-row
store qualification. No motion or lifecycle command is issued.
"""

from __future__ import annotations

import argparse
import asyncio
from collections import defaultdict
import json
from pathlib import Path
import resource
from tempfile import TemporaryDirectory
import time

import httpx

from manyforge_headless_api.app import create_app
from manyforge_headless_api.control_state import HeadlessControlStateSource
from manyforge_headless_api.manager import ManagerCommandClient
from manyforge_headless_api.native import HeadlessApiConfig, NativeHeadlessCore
from manyforge_headless_api.service import HeadlessService
from scripts.qualification.ui6h_source_cost import distribution


async def qualify(core, config):
    app = create_app(service=HeadlessService(core, ManagerCommandClient(config.manager_run_root)))
    samples = defaultdict(list)
    loop = asyncio.get_running_loop()
    started, cpu = loop.time(), time.process_time()
    expected = loop.time() + 0.005
    due = None

    def tick():
        nonlocal expected, due
        samples["loop_lag"].append(max(0, loop.time() - expected) * 1000)
        expected = loop.time() + 0.005
        due = loop.call_at(expected, tick)

    def record(**record):
        before = loop.time()
        result = core.record_client_incident(**record)
        samples["scheduled_incident_enqueue"].append((loop.time() - before) * 1000)
        assert result["recorded"] is True
        return result

    source = HeadlessControlStateSource(
        object(),
        None,
        program_runner_websocket_url=None,
        incident_recorder=record,
    )
    source._loop = loop
    record_value = {
        "schema_version": "manyforge.program_diagnostic.v2",
        "owner_epoch": "cost.runner",
        **{
            key: "sha256:" + value * 64
            for key, value in (
                ("program_revision", "a"),
                ("program_binding_id", "b"),
                ("retained_bundle_id", "c"),
                ("graph_revision", "d"),
            )
        },
        "state": "failed",
        "reason_code": "qualification",
        "message": "History cost probe",
        "occurred_at_wall": "2026-09-08T10:00:00Z",
        "blackboard": {f"item{index}": "x" * 2048 for index in range(7)},
        "runtime_overrides": {},
        "omitted_blackboard_count": 0,
        "omitted_override_count": 0,
        "delivery_loss_count": 0,
        "details": None,
        "origin": {
            "kind": "program_runner",
            "node_instance_id": None,
            "owner_api": None,
        },
    }
    try:
        async with app.router.lifespan_context(app), httpx.AsyncClient(
            transport=httpx.ASGITransport(app=app), base_url="http://qualification"
        ) as client:

            async def request(method, path, **kwargs):
                before = loop.time()
                response = await client.request(method, path, **kwargs)
                samples[path].append((loop.time() - before) * 1000)
                if response.status_code != 200:
                    raise RuntimeError(f"{path}: {response.status_code} {response.text[:1000]}")
                return response

            async def write():
                for index in range(256):
                    response = await request(
                        "POST",
                        "/api/program/diagnostics",
                        json={
                            **record_value,
                            "program_run_id": f"run.cost.{index}",
                            "occurred_at_ns": str(index + 1),
                        },
                    )
                    assert response.json()["recorded"] is True
                    source.record_incident(
                        incident_id=f"cost.{index}",
                        incident_kind="control_delayed",
                        phase="activated" if index % 2 == 0 else "resolved",
                        control_session_id="cost.session",
                        client_incarnation="cost.client",
                        source_label="qualification",
                        reason_code="injected_delay",
                        occurrence_count=1,
                        maximum_delay_ns=600_000_000,
                        first_observed_ns=index,
                        last_observed_ns=index,
                        causal_context={},
                    )

            async def read(number):
                path = "/api/execution/history" + ("/export" if number % 2 else "")
                for _ in range(64):
                    await request("GET", path, params={"limit": 32})
                    await request("GET", "/api/control/diagnostics")

            due = loop.call_at(expected, tick)
            await asyncio.wait_for(asyncio.gather(write(), *(read(n) for n in range(4))), 30)
            deadline = loop.time() + 5
            while True:
                health = await asyncio.to_thread(core.historian_health)
                if health["delivered_count"] == 512:
                    break
                if loop.time() > deadline:
                    raise RuntimeError(f"writer failed to drain: {health}")
                await asyncio.sleep(0.005)
            page = (await request("GET", "/api/execution/history")).json()
            assert health["available"] is True, health
            assert health["queue_depth"] == 0, health
            return {
                "schema_version": "manyforge.history_asgi_cost.v1",
                "measurements": {name: distribution(values) for name, values in samples.items()},
                "health": health,
                "retention": page["retention"],
                "cpu_seconds": time.process_time() - cpu,
                "elapsed_seconds": loop.time() - started,
                "max_rss_kib": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
                "scope": "real native History via in-process ASGI, four readers/one writer; no motion/network/browser",
            }
    finally:
        if due is not None:
            due.cancel()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--install-root", type=Path, required=True)
    args = parser.parse_args()
    with TemporaryDirectory(prefix="ui6h-history-asgi-") as temporary:
        root = Path(temporary)
        paths = {
            name: root / name
            for name in (
                "projections",
                "artifacts",
                "selection",
                "manager",
                "execution",
            )
        }
        for path in paths.values():
            path.mkdir()
        config = HeadlessApiConfig(
            projection_root=paths["projections"],
            install_root=args.install_root,
            artifact_root=paths["artifacts"],
            node_catalog_path=Path("/workspace/manyforge_behavior/resources/node_catalog.yaml"),
            node_environment_manifest_path=Path(
                "/workspace/manyforge_behavior/resources/node_environment_manifest.yaml"
            ),
            alarm_catalog_path=Path(
                "/workspace/manyforge_headless/resources/operator-alarm-catalog.v1.yaml"
            ),
            historian_database_path=root / "history.sqlite3",
            manager_run_root=paths["manager"],
            execution_run_root=paths["execution"],
            cell_id="lite6_pick_place_cell",
            owner_epoch="history.cost.1",
            historian_record_retention=64,
        )
        core = NativeHeadlessCore(config)
        try:
            print(json.dumps(asyncio.run(qualify(core, config)), sort_keys=True))
        finally:
            core.shutdown()


if __name__ == "__main__":
    main()
