"""Native-owner qualification that intentionally requires the built binding."""

from __future__ import annotations

from dataclasses import replace
from pathlib import Path
import json
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


# One ``A`` per application start (contract 1.1).
FIRST_START = "3f9a0000000000a1"
SECOND_START = "5c2b0000000000b2"


def _config(root: Path, application_start_id: str) -> HeadlessApiConfig:
    artifact_root = root / "artifacts"
    manager_run_root = root / "manager-run"
    execution_run_root = root / "execution-run"
    artifact_root.mkdir(exist_ok=True)
    manager_run_root.mkdir(exist_ok=True)
    execution_run_root.mkdir(exist_ok=True)
    history_directory = root / "history"
    history_directory.mkdir(exist_ok=True)
    return HeadlessApiConfig(
        install_root=Path(os.environ["MANYFORGE_CANONICAL_EMPTY_PACKAGE_ROOT"]),
        artifact_root=artifact_root,
        node_catalog_path=NODE_CATALOG,
        node_environment_manifest_path=NODE_ENVIRONMENT_MANIFEST,
        alarm_catalog_path=CATALOG,
        historian_directory=history_directory,
        manager_run_root=manager_run_root,
        execution_run_root=execution_run_root,
        cell_id="c1_build_authority_cell",
        application_start_id=application_start_id,
        freshness_milliseconds=100,
        historian_queue_capacity=32,
        historian_record_retention=100,
    )


def test_config_rejects_nested_run_and_durable_capabilities(
    tmp_path: Path,
) -> None:
    config = _config(tmp_path, FIRST_START)
    nested_execution_run = config.manager_run_root / "execution"
    nested_execution_run.mkdir()
    with pytest.raises(ValueError, match="disjoint trees"):
        replace(config, execution_run_root=nested_execution_run)

    with pytest.raises(ValueError, match="durable Headless paths"):
        replace(
            config,
            historian_directory=config.execution_run_root / "history",
        )


def _acknowledgement(alarm: dict, application_start_id: str, request_key: str) -> dict:
    return {
        "request_key": request_key,
        "application_start_id": application_start_id,
        "source_label": "native_http_test",
        "activation_id": alarm["activation_id"],
        "expected_record_revision": alarm["record_revision"],
        "reason": "observed",
        "confirmed": True,
    }


def _start_manager(root: Path) -> subprocess.Popen[bytes]:
    manager_state_root = root / "manager"
    manager_run_root = root / "manager-run"
    execution_state_root = root / "execution-state"
    execution_run_root = root / "execution-run"
    for path in (
        manager_state_root,
        manager_run_root,
        execution_state_root,
        execution_run_root,
    ):
        path.mkdir(exist_ok=True)
    # The launcher's process list: this test starts Headless itself.
    application_processes = root / "application-processes.json"
    application_processes.write_text(
        json.dumps({"api_version": "manyforge.application_processes.v1", "processes": []}),
        encoding="utf-8",
    )
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
            "--application-processes",
            str(application_processes),
            "--reconciliation-period-ms",
            "20",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )
    required = {
        manager_run_root / "manager-command.sock",
        manager_run_root / "manager-status.sock",
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
    raise RuntimeError("Manager did not open its command and status sockets")


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
    # Other process tests in this session may load the Composer; this owner
    # must not load any of it.
    loaded_before = {name for name in sys.modules if name.startswith("manyforge_composer")}
    app = create_app(_config(tmp_path, FIRST_START))
    with TestClient(app) as client:
        runtime = client.get("/api/runtime")
        assert runtime.status_code == 200
        assert runtime.json()["complete"] is False
        assert runtime.json()["application_start_id"] == FIRST_START
        assert client.get("/api/manager/status").status_code == 503

        snapshot = client.get("/api/execution/alarms").json()
        assert "owner_epoch" not in snapshot
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
    loaded_after = {name for name in sys.modules if name.startswith("manyforge_composer")}
    assert loaded_after == loaded_before


def test_native_failure_capture_protects_context_and_reports_its_limits(tmp_path: Path) -> None:
    config = replace(_config(tmp_path, FIRST_START), historian_queue_capacity=64)
    core = NativeHeadlessCore(config)
    origin = 31 * 365 * 86400 * 1_000_000_000

    def incident(index: int, kind: str) -> None:
        result = core.record_client_incident(
            incident_id=f"incident.{index}", incident_kind=kind, phase="activated",
            control_session_number=1, client_incarnation="client.1",
            source_label="operator_ui", reason_code=kind, occurrence_count=1,
            maximum_delay_ns=None, first_observed_ns=origin + index,
            last_observed_ns=origin + index,
        )
        assert result["recorded"] is True

    try:
        for index in range(3):
            incident(index, "control_delayed")  # routine WARN context
        incident(3, "viewer_model_unavailable")  # ERROR opens one capture
        incident(4, "control_delayed")  # enters its tail
    finally:
        core.shutdown()

    # The writer restarts inside the tail; the History is the same file.
    with TestClient(create_app(config)) as client:
        page = client.get("/api/execution/history/captures")
        assert page.status_code == 200, page.text
        # The restarted app (no Program runner here) may open its own capture
        # for its unavailable Program control source; this one is the first.
        capture, = [row for row in page.json()["captures"] if row["capture_id"] == "1"]
        assert capture["first_sequence"] == 1
        assert capture["trigger_sequence"] == 4
        assert capture["last_sequence"] == 5
        # The writer restarted inside the tail: the capture says so.
        assert capture["state"] == "closed_by_restart"
        assert capture["coverage"] == "uncertain"
        assert "tail_interrupted_by_restart" in capture["uncertainty"]
        assert capture["trigger"]["event_type"] == "client_incident_activated"
        assert page.json()["retention"]["schema_version"] == "manyforge.historian_retention.v3"
        rows = client.get("/api/execution/history", params={"capture_id": 1})
        assert rows.status_code == 200, rows.text
        assert [row["sequence"] for row in rows.json()["events"]] == [5, 4, 3, 2, 1]
        exported = client.get("/api/execution/history/export", params={"capture_id": 1})
        assert exported.status_code == 200, exported.text
        assert exported.json()["capture"]["contents"] == "retained_event_context"
        missing = client.get("/api/execution/history", params={"capture_id": 99})
        assert missing.status_code == 404, missing.text
        assert client.get("/api/execution/history/captures?limit=33").status_code == 422


def test_native_history_discovery_exports_and_exact_context_survive_restart(tmp_path: Path) -> None:
    config = replace(_config(tmp_path, FIRST_START), historian_queue_capacity=64)
    core = NativeHeadlessCore(config)
    origin = 31 * 365 * 86400 * 1_000_000_000
    try:
        for index in range(35):
            result = core.record_client_incident(
                incident_id=f"incident.{index}", incident_kind="control_delayed",
                phase="activated", control_session_number=1, client_incarnation="client.1",
                source_label="operator_ui", reason_code="heartbeat_delayed", occurrence_count=1,
                maximum_delay_ns=600_000_000,
                first_observed_ns=origin + index, last_observed_ns=origin + index,
                # Run numbers are integers, recorded as decimal text (1.5).
                causal_context={"program_run_id": "8" if index == 0 else "7",
                                "scene_id": "scene.at.incident"},
            )
            assert result["recorded"] is True
    finally:
        core.shutdown()  # real bounded writer drain; reopen through the public API

    # Reopened in the same start: an id filter names this start's ``A``.
    start = {"application_start_id": FIRST_START}
    with TestClient(create_app(config)) as client:
        page = client.get(
            "/api/execution/history",
            params={"program_run_id": "8", "severities": "WARN", **start},
        )
        assert page.status_code == 200, page.text
        row, = page.json()["events"]
        assert row["sequence"] == 1  # selection is before the 32-row page, not a browser scan
        assert row["application_start_id"] == FIRST_START  # each row names its start
        detail = client.get("/api/execution/history/1")
        assert detail.status_code == 200, detail.text
        channels = client.get("/api/execution/history/discovery", params={"kind": "channel"})
        assert channels.status_code == 200, channels.text
        assert channels.json()["values"] == ["manyforge.composer"]
        runs = client.get("/api/execution/history/discovery", params={"kind": "program_run_id", "limit": 1})
        assert runs.json()["values"] == ["7"]
        assert runs.json()["next_after"] == "7"
        assert client.get("/api/execution/history/discovery", params={"kind": "program_run_id", "after": "7"}).json()["values"] == ["8"]
        # The display form names the same run (BS 2.9).
        for path in ("/api/execution/history/export", "/api/debug/export", "/api/debug/export/ros2-medkit"):
            response = client.get(path, params={"program_run_id": f"{FIRST_START[:4]}/run8", **start})
            assert response.status_code == 200, response.text
            assert response.headers["content-disposition"].startswith("attachment;")
            assert "scene.at.incident" in response.text
            if path.endswith("/ros2-medkit"):
                assert response.json()["schema_version"] == "manyforge.ros2_medkit_trace.v2"
                assert response.json()["records"][0]["program_run_id"] == "8"
        # The copy form carries ``A`` itself.
        exported = client.get(
            "/api/execution/history/export", params={"program_run_id": f"{FIRST_START}/run8"}
        ).json()
        assert exported["events"][0]["event"]["payload"]["first_observed_ns"] == str(origin)
        assert client.get("/api/debug/export?run_id=a&program_run_id=b").status_code == 422
        assert client.get("/api/execution/history/discovery?kind=payload_json").status_code == 422
        assert client.get("/api/execution/history/export?limit=33").status_code == 422


def test_manager_loss_raises_an_alarm_acknowledged_once_for_the_current_start(
    tmp_path: Path,
) -> None:
    manager = _start_manager(tmp_path)
    try:
        with TestClient(create_app(_config(tmp_path, FIRST_START))) as first:
            assert _wait_runtime(first, complete=True)["complete"] is True
            assert first.get("/api/execution/alarms").json()["alarms"] == []

            _stop_manager(manager)
            assert _wait_runtime(first, complete=False)["complete"] is False
            snapshot, first_alarm = _wait_provider_alarm(first)

            stale = _acknowledgement(first_alarm, SECOND_START, "aa" * 16)
            path = (
                f"/api/execution/alarms/{first_alarm['condition_id']}"
                "/acknowledge"
            )
            response = first.post(path, json=stale)
            assert response.status_code == 409
            assert response.json()["detail"]["code"] == (
                "stale_application_start_id"
            )

            body = _acknowledgement(first_alarm, FIRST_START, "01" * 16)
            acknowledged = first.post(path, json=body)
            assert acknowledged.status_code == 200
            replayed = first.post(path, json=body).json()
            assert replayed["status"] == "already_acknowledged"
            assert replayed["changed"] is False
            assert replayed["audit_event_ids"] == []
            replayed_alarm = next(
                alarm
                for alarm in replayed["snapshot"]["alarms"]
                if alarm["condition_id"] == first_alarm["condition_id"]
            )
            assert replayed_alarm["acknowledged"] is True
            # Headless forms the HTTP key: class 3, part 0 (contract 1.4).
            assert replayed_alarm["acknowledgement_attribution"]["request_key"] == (
                "c0" + "01" * 15
            )
            conflict = first.post(path, json={**body, "reason": "changed"})
            assert conflict.status_code == 200
            repeated = conflict.json()
            assert repeated["status"] == "already_acknowledged"
            assert repeated["changed"] is False
            assert repeated["audit_event_ids"] == []

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
