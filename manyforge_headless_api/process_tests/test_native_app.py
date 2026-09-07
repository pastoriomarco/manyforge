"""Native-owner qualification that intentionally requires the built binding."""

from __future__ import annotations

from dataclasses import replace
from pathlib import Path
import os
import subprocess
import sys
import time

from fastapi.testclient import TestClient
import pytest

from manyforge_headless_api.app import create_app
from manyforge_headless_api.native import HeadlessApiConfig, NativeHeadlessCore


CATALOG = Path("/workspace/manyforge_headless/resources/operator-alarm-catalog.v1.yaml")
NODE_CATALOG = Path("/workspace/manyforge_behavior/resources/node_catalog.yaml")
NODE_ENVIRONMENT_MANIFEST = Path(
    "/workspace/manyforge_behavior/resources/node_environment_manifest.yaml"
)


def _config(root: Path, owner_epoch: str) -> HeadlessApiConfig:
    publication_root = root / "publications"
    artifact_root = root / "artifacts"
    program_selection_state_root = root / "program-selection-state"
    manager_run_root = root / "manager-run"
    execution_run_root = root / "execution-run"
    publication_root.mkdir(exist_ok=True)
    artifact_root.mkdir(exist_ok=True)
    program_selection_state_root.mkdir(exist_ok=True)
    manager_run_root.mkdir(exist_ok=True)
    execution_run_root.mkdir(exist_ok=True)
    return HeadlessApiConfig(
        projection_root=publication_root,
        install_root=Path(os.environ["MANYFORGE_CANONICAL_EMPTY_PACKAGE_ROOT"]),
        artifact_root=artifact_root,
        program_selection_state_root=program_selection_state_root,
        node_catalog_path=NODE_CATALOG,
        node_environment_manifest_path=NODE_ENVIRONMENT_MANIFEST,
        alarm_catalog_path=CATALOG,
        historian_database_path=root / "history.sqlite3",
        manager_run_root=manager_run_root,
        execution_run_root=execution_run_root,
        cell_id="c1_build_authority_cell",
        resource_ids=("resource.arm.a",),
        owner_id="headless-api.test",
        owner_epoch=owner_epoch,
        freshness_milliseconds=100,
        historian_queue_capacity=32,
        historian_record_retention=100,
    )


def test_config_rejects_nested_run_and_durable_capabilities(
    tmp_path: Path,
) -> None:
    config = _config(tmp_path, "headless-config-test.epoch")
    nested_execution_run = config.manager_run_root / "execution"
    nested_execution_run.mkdir()
    with pytest.raises(ValueError, match="disjoint trees"):
        replace(config, execution_run_root=nested_execution_run)

    with pytest.raises(ValueError, match="durable Headless paths"):
        replace(
            config,
            historian_database_path=config.execution_run_root / "history.sqlite3",
        )

    with pytest.raises(ValueError, match="durable Headless paths"):
        replace(config, projection_root=tmp_path)


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
    manager_run_root = root / "manager-run"
    execution_state_root = root / "execution-state"
    execution_run_root = root / "execution-run"
    for path in (
        publication_root,
        manager_state_root,
        manager_run_root,
        execution_state_root,
        execution_run_root,
    ):
        path.mkdir(exist_ok=True)
    process = subprocess.Popen(
        [
            os.environ["MANYFORGE_MANAGER_DAEMON"],
            "--install-root",
            os.environ["MANYFORGE_CANONICAL_EMPTY_PACKAGE_ROOT"],
            "--manager-state-root",
            str(manager_state_root),
            "--manager-run-root",
            str(manager_run_root),
            "--execution-state-root",
            str(execution_state_root),
            "--execution-run-root",
            str(execution_run_root),
            "--logical-scene-database",
            str(root / "logical-scene.sqlite3"),
            "--program-cycle-journal",
            str(root / "program-cycle.sqlite3"),
            "--authoring-transaction-journal",
            str(root / "authoring-transactions.sqlite3"),
            "--projection-root",
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
        manager_run_root / "manager-command.sock",
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
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
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
            if (
                alarm.get("definition_id")
                == "manyforge.runtime.provider_unavailable"
                and alarm.get("details", {}).get("unavailable_sections")
                == ["manager", "logical_scene", "scene_runtime"]
            ):
                return snapshot, alarm
        time.sleep(0.01)
    raise RuntimeError(
        "provider alarm did not settle on the complete stopped-Manager "
        f"source set: {snapshot}"
    )


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


def test_native_history_discovery_exports_and_exact_context_survive_restart(tmp_path: Path) -> None:
    config = replace(_config(tmp_path, "history.epoch.1"), historian_queue_capacity=64)
    core = NativeHeadlessCore(config)
    origin = 31 * 365 * 86400 * 1_000_000_000
    try:
        for index in range(35):
            result = core.record_client_incident(
                incident_id=f"incident.{index}", incident_kind="control_delayed",
                phase="activated", control_session_id="session.1", client_incarnation="client.1",
                source_label="operator_ui", reason_code="heartbeat_delayed", occurrence_count=1,
                maximum_delay_ns=600_000_000,
                first_observed_ns=origin + index, last_observed_ns=origin + index,
                causal_context={"program_run_id": "run.old" if index == 0 else "run.new",
                                "scene_id": "scene.at.incident"},
            )
            assert result["recorded"] is True
    finally:
        core.shutdown()  # real bounded writer drain; reopen through the public API

    with TestClient(create_app(replace(config, owner_epoch="history.epoch.2"))) as client:
        page = client.get("/api/execution/history", params={"program_run_id": "run.old", "severities": "WARN"})
        assert page.status_code == 200, page.text
        row, = page.json()["events"]
        assert row["sequence"] == 1  # selection is before the 32-row page, not a browser scan
        detail = client.get("/api/execution/history/1")
        assert detail.status_code == 200, detail.text
        channels = client.get("/api/execution/history/discovery", params={"kind": "channel"})
        assert channels.status_code == 200, channels.text
        assert channels.json()["values"] == ["manyforge.composer"]
        runs = client.get("/api/execution/history/discovery", params={"kind": "program_run_id", "limit": 1})
        assert runs.json()["values"] == ["run.new"]
        assert runs.json()["next_after"] == "run.new"
        assert client.get("/api/execution/history/discovery", params={"kind": "program_run_id", "after": "run.new"}).json()["values"] == ["run.old"]
        for path in ("/api/execution/history/export", "/api/debug/export", "/api/debug/export/ros2-medkit"):
            response = client.get(path, params={"program_run_id": "run.old"})
            assert response.status_code == 200, response.text
            assert response.headers["content-disposition"].startswith("attachment;")
            assert "scene.at.incident" in response.text
            if path.endswith("/ros2-medkit"):
                assert response.json()["schema_version"] == "manyforge.ros2_medkit_trace.v2"
                assert response.json()["records"][0]["program_run_id"] == "run.old"
        exported = client.get("/api/execution/history/export", params={"program_run_id": "run.old"}).json()
        assert exported["events"][0]["event"]["payload"]["first_observed_ns"] == str(origin)
        assert client.get("/api/debug/export?run_id=a&program_run_id=b").status_code == 422
        assert client.get("/api/execution/history/discovery?kind=payload_json").status_code == 422
        assert client.get("/api/execution/history/export?limit=33").status_code == 422


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
        unknown = replacement.get("/api/execution/alarms").json()
        assert unknown["alarms"] == []
        assert unknown["reconciled"] is False

        # A replacement owner must not manufacture a new acknowledgement
        # obligation from retained sources that were already stale when it
        # started. Reannunciation becomes authoritative only after this owner
        # observes one fresh complete source set and then observes its loss.
        replacement_manager = _start_manager(tmp_path)
        try:
            assert _wait_runtime(replacement, complete=True)["complete"] is True
            assert replacement.get("/api/execution/alarms").json()["alarms"] == []
            _stop_manager(replacement_manager)
            assert _wait_runtime(replacement, complete=False)["complete"] is False
            snapshot, replacement_alarm = _wait_provider_alarm(replacement)
        finally:
            _stop_manager(replacement_manager)
        assert snapshot["owner_epoch"] == "api.epoch.2"
        assert replacement_alarm["activation_id"] != first_alarm["activation_id"]
        assert replacement_alarm["lifecycle_state"] == "active_unacknowledged"
        retained_history = replacement.get("/api/execution/history").json()
        assert any(
            row.get("event", {}).get("event_type")
            == "operator_alarm_acknowledged"
            for row in retained_history["events"]
        )
