"""Short real-ENOSPC qualification on an explicitly isolated Docker tmpfs.

Run in the pinned development image with --tmpfs /fault-volume:size=8m,mode=1777.
Refuses ordinary directories and larger volumes; never fills a host filesystem.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
from tempfile import TemporaryDirectory
import time

from manyforge_headless_api.native import HeadlessApiConfig, NativeHeadlessCore


def wait_for(core: NativeHeadlessCore, predicate) -> dict:
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        health = core.historian_health()
        if predicate(health):
            return health
        time.sleep(0.005)
    raise AssertionError(f"bounded historian transition did not occur: {health}")


def qualify(install_root: Path, volume: Path) -> dict:
    volume = volume.resolve(strict=True)
    capacity = os.statvfs(volume)
    if (
        volume != Path("/fault-volume")
        or not os.path.ismount(volume)
        or not 0 < capacity.f_blocks * capacity.f_frsize <= 16 * 1024**2
    ):
        raise ValueError("requires a private /fault-volume mount of at most 16 MiB")
    if list(volume.iterdir()):
        raise ValueError("fault volume must be empty")
    with TemporaryDirectory(prefix="manyforge-ui6h-storage-") as temporary:
        root = Path(temporary)
        paths = {
            name: root / name
            for name in (
                "projections",
                "artifacts",
                "selection",
                "manager-run",
                "execution-run",
            )
        }
        for path in paths.values():
            path.mkdir()
        config = HeadlessApiConfig(
            projection_root=paths["projections"],
            install_root=install_root,
            artifact_root=paths["artifacts"],
            program_selection_state_root=paths["selection"],
            node_catalog_path=Path(
                "/workspace/manyforge_behavior/resources/node_catalog.yaml"
            ),
            node_environment_manifest_path=Path(
                "/workspace/manyforge_behavior/resources/node_environment_manifest.yaml"
            ),
            alarm_catalog_path=Path(
                "/workspace/manyforge_headless/resources/operator-alarm-catalog.v1.yaml"
            ),
            historian_database_path=volume / "history.sqlite3",
            manager_run_root=paths["manager-run"],
            execution_run_root=paths["execution-run"],
            cell_id="c1_build_authority_cell",
            resource_ids=("resource.arm.a",),
            owner_id="storage-qualification",
            owner_epoch="storage.1",
            historian_queue_capacity=8,
            historian_record_retention=100,
        )
        core = NativeHeadlessCore(config)
        padding = volume / "qualification-padding"

        def record(number: int):
            result = core.record_client_incident(
                incident_id=f"incident.{number}",
                incident_kind="control_delayed",
                phase="activated",
                control_session_id="session.1",
                client_incarnation="client.1",
                source_label="qualification",
                reason_code="heartbeat_delayed",
                occurrence_count=1,
                maximum_delay_ns=600_000_000,
                first_observed_ns=number,
                last_observed_ns=number,
                causal_context={"program_run_id": "run.storage"},
            )
            assert result["recorded"] is True

        try:
            record(1)
            wait_for(core, lambda health: health["delivered_count"] == 1)
            with padding.open("xb") as output:
                capacity = os.statvfs(volume)
                os.posix_fallocate(
                    output.fileno(), 0, capacity.f_bavail * capacity.f_frsize
                )
            assert os.statvfs(volume).f_bavail == 0
            record(2)
            failed = wait_for(core, lambda health: health["delivery_failure_count"] > 0)
            assert failed["available"] is False and failed["delivered_count"] == 1
            assert "full" in failed["error_message"].lower(), failed
            assert len(core.history()["events"]) == 1
            padding.unlink()
            recovered = wait_for(
                core,
                lambda health: health["delivered_count"] == 2
                and health["recovery_ready"],
            )
            assert (
                recovered["available"] is False
            )  # explicit reconciliation remains required
            assert recovered["health_generation"] >= failed["health_generation"]
            assert len(core.history()["events"]) == 2
            return {
                "schema_version": "manyforge.ui6h_storage_qualification.v1",
                "actual_enospc": True,
                "failed_health": failed,
                "drained_health": recovered,
                "original_and_pending_records_retained": True,
            }
        finally:
            padding.unlink(missing_ok=True)
            core.shutdown()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--install-root", type=Path, required=True)
    parser.add_argument("--fault-volume", type=Path, required=True)
    arguments = parser.parse_args()
    print(
        json.dumps(
            qualify(arguments.install_root, arguments.fault_volume), sort_keys=True
        )
    )
