"""Native-owner qualification that intentionally requires the built binding."""

from __future__ import annotations

from pathlib import Path
import os
import subprocess
import sys
import time

from fastapi.testclient import TestClient

from manyforge_headless_api.app import create_app
from manyforge_headless_api.native import HeadlessApiConfig


CATALOG = Path("/workspace/manyforge_headless/resources/operator-alarm-catalog.v1.yaml")
NODE_CATALOG = Path("/workspace/manyforge_behavior/resources/node_catalog.yaml")


def _config(root: Path, owner_epoch: str) -> HeadlessApiConfig:
    publication_root = root / "publications"
    manager_state_root = root / "manager"
    execution_state_root = root / "execution"
    publication_root.mkdir(exist_ok=True)
    manager_state_root.mkdir(exist_ok=True)
    execution_state_root.mkdir(exist_ok=True)
    return HeadlessApiConfig(
        publication_root=publication_root,
        package_root=Path(os.environ["MANYFORGE_CANONICAL_EMPTY_PACKAGE_ROOT"]),
        node_catalog_path=NODE_CATALOG,
        alarm_catalog_path=CATALOG,
        historian_database_path=root / "history.sqlite3",
        manager_state_root=manager_state_root,
        execution_state_root=execution_state_root,
        cell_id="c1_build_authority_cell",
        resource_ids=("resource.arm.a",),
        owner_id="headless-api.test",
        owner_epoch=owner_epoch,
        freshness_milliseconds=100,
        historian_queue_capacity=32,
        historian_record_retention=100,
        mutation_replay_capacity=32,
    )


def _acknowledgement(alarm: dict, owner_epoch: str, request_id: str) -> dict:
    return {
        "request_id": request_id,
        "request_owner_epoch": owner_epoch,
        "source_label": "native_http_test",
        "correlation_id": "correlation.1",
        "activation_id": alarm["activation_id"],
        "expected_record_revision": alarm["record_revision"],
        "reason": "observed",
        "confirmed": True,
    }


def _start_manager(root: Path) -> subprocess.Popen[bytes]:
    publication_root = root / "publications"
    manager_state_root = root / "manager"
    runtime_state_root = root / "runtime"
    for path in (publication_root, manager_state_root, runtime_state_root):
        path.mkdir(exist_ok=True)
    process = subprocess.Popen(
        [
            os.environ["MANYFORGE_MANAGER_DAEMON"],
            "--package-root",
            os.environ["MANYFORGE_CANONICAL_EMPTY_PACKAGE_ROOT"],
            "--manager-state-root",
            str(manager_state_root),
            "--runtime-state-root",
            str(runtime_state_root),
            "--logical-scene-database",
            str(root / "logical-scene.sqlite3"),
            "--publication-root",
            str(publication_root),
            "--reconciliation-period-ms",
            "20",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )
    required = {
        publication_root / "manager-source.json",
        publication_root / "logical-scene-source.json",
        publication_root / "scene-runtime-source.json",
        manager_state_root / "manager-command.sock",
    }
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        if process.poll() is not None:
            detail = (process.stderr.read() if process.stderr else b"").decode(
                errors="replace"
            )
            raise RuntimeError(f"Manager exited during startup: {detail}")
        if all(path.exists() for path in required):
            return process
        time.sleep(0.01)
    process.terminate()
    process.wait(timeout=5)
    raise RuntimeError("Manager did not publish its complete H0 source set")


def _stop_manager(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is None:
        process.terminate()
        process.wait(timeout=5)


def _wait_runtime(client: TestClient, *, complete: bool) -> dict:
    deadline = time.monotonic() + 5.0
    last: dict = {}
    while time.monotonic() < deadline:
        last = client.get("/api/runtime").json()
        if last.get("complete") is complete:
            return last
        time.sleep(0.01)
    raise RuntimeError(f"runtime did not reach complete={complete}: {last}")


def _wait_provider_alarm(client: TestClient) -> tuple[dict, dict]:
    deadline = time.monotonic() + 5.0
    snapshot: dict = {}
    while time.monotonic() < deadline:
        snapshot = client.get("/api/execution/alarms").json()
        for alarm in snapshot.get("alarms", []):
            if alarm.get("definition_id") == (
                "manyforge.runtime.provider_unavailable"
            ):
                return snapshot, alarm
        time.sleep(0.01)
    raise RuntimeError(f"provider alarm did not activate: {snapshot}")


def test_native_http_owner_works_without_composer_and_fences_replay(
    tmp_path: Path,
) -> None:
    assert "manyforge_composer" not in sys.modules
    app = create_app(_config(tmp_path, "api.epoch.1"))
    with TestClient(app) as client:
        runtime = client.get("/api/runtime")
        assert runtime.status_code == 200
        assert runtime.json()["complete"] is False
        assert client.get("/api/manager/status").status_code == 503

        snapshot = client.get("/api/execution/alarms").json()
        assert snapshot["owner_epoch"] == "api.epoch.1"
        assert snapshot["reconciled"] is False
        assert snapshot["alarms"] == []

        invalid_filter = client.get(
            "/api/execution/alarms",
            params={"states": "invented"},
        )
        assert invalid_filter.status_code == 400
        assert (
            invalid_filter.json()["detail"]["code"]
            == "operator_alarm_state_filter_invalid"
        )

        assert client.get("/api/health").json()["status"] == "degraded"
    assert "manyforge_composer" not in sys.modules


def test_http_owner_restart_reannunciates_from_fresh_source_observation(
    tmp_path: Path,
) -> None:
    manager = _start_manager(tmp_path)
    try:
        with TestClient(create_app(_config(tmp_path, "api.epoch.1"))) as first:
            assert _wait_runtime(first, complete=True)["complete"] is True
            assert first.get("/api/execution/alarms").json()["alarms"] == []

            _stop_manager(manager)
            assert _wait_runtime(first, complete=False)["complete"] is False
            snapshot, first_alarm = _wait_provider_alarm(first)

            stale = _acknowledgement(
                first_alarm,
                "api.epoch.stale",
                "request.stale",
            )
            path = (
                f"/api/execution/alarms/{first_alarm['condition_id']}"
                "/acknowledge"
            )
            response = first.post(path, json=stale)
            assert response.status_code == 409
            assert response.json()["detail"]["code"] == (
                "stale_request_owner_epoch"
            )

            body = _acknowledgement(
                first_alarm,
                snapshot["owner_epoch"],
                "request.1",
            )
            acknowledged = first.post(path, json=body)
            assert acknowledged.status_code == 200
            assert first.post(path, json=body).json() == acknowledged.json()
            conflict = first.post(path, json={**body, "reason": "changed"})
            assert conflict.status_code == 409
            assert conflict.json()["detail"]["code"] == (
                "headless_request_identity_conflict"
            )

            deadline = time.monotonic() + 2.0
            history = {"events": []}
            while time.monotonic() < deadline:
                history = first.get("/api/execution/history").json()
                if any(
                    row.get("event", {}).get("event_type")
                    == "operator_alarm_acknowledged"
                    for row in history["events"]
                ):
                    break
                time.sleep(0.005)
            assert any(
                row.get("event", {}).get("event_type")
                == "operator_alarm_acknowledged"
                for row in history["events"]
            )
    finally:
        _stop_manager(manager)

    with TestClient(create_app(_config(tmp_path, "api.epoch.2"))) as replacement:
        _wait_runtime(replacement, complete=False)
        snapshot, replacement_alarm = _wait_provider_alarm(replacement)
        assert snapshot["owner_epoch"] == "api.epoch.2"
        assert replacement_alarm["activation_id"] != first_alarm["activation_id"]
        assert replacement_alarm["lifecycle_state"] == "active_unacknowledged"
        retained_history = replacement.get("/api/execution/history").json()
        assert any(
            row.get("event", {}).get("event_type")
            == "operator_alarm_acknowledged"
            for row in retained_history["events"]
        )
