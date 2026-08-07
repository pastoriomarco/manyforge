"""N3 process test across host, headless, Behavior, and Composer callers."""

from __future__ import annotations

import copy
import json
import os
import shutil
import subprocess
import time
from pathlib import Path
from typing import Any, Mapping

from fastapi import FastAPI
from fastapi.testclient import TestClient
import pytest

from manyforge_behavior.kernel_client import KernelClient
from manyforge_behavior.runner import OrchestratorRunner
from manyforge_composer.backend.routes_headless import create_headless_router
from manyforge_headless_api.app import create_app
from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient
from manyforge_headless_api.execution import ExecutionCommandClient
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.service import HeadlessService


TERMINAL_STATES = {
    "completed",
    "stopped",
    "failed",
    "interrupted",
    "intervention_required",
    "cancelled_pre_dispatch",
}


class UnusedCore:
    pass


class UnusedManager:
    pass


class RecordingTransport:
    def __init__(self, client: HeadlessHttpClient) -> None:
        self.client = client
        self.last_submission: dict[str, Any] | None = None

    def execution_status(self) -> dict[str, Any]:
        return self.client.execution_status()

    def execute(self, body: Mapping[str, Any]) -> dict[str, Any]:
        self.last_submission = copy.deepcopy(dict(body))
        return self.client.execute(body)

    def operation(self, operation_id: str) -> dict[str, Any]:
        return self.client.operation(operation_id)


def _required_path(name: str) -> Path:
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"{name} was not supplied by the N3 build")
    path = Path(value)
    if not path.exists():
        raise RuntimeError(f"{name} does not exist: {path}")
    return path


def _start_host(
    fixture_root: Path,
    execution_root: Path,
    staging_root: Path,
    manifest: dict[str, Any],
) -> subprocess.Popen[bytes]:
    process = subprocess.Popen(
        [
            str(_required_path("MANYFORGE_NATIVE_EXECUTION_HOST")),
            "--package-root",
            str(fixture_root / "retained-package"),
            "--manager-state-root",
            str(fixture_root / "manager-state"),
            "--execution-state-root",
            str(execution_root),
            "--staging-root",
            str(staging_root),
            "--program",
            str(fixture_root / "program.json"),
            "--program-binding-id",
            manifest["program_binding_id"],
            "--motion-target-role-id",
            manifest["target_role_id"],
            "--candidate-worker",
            str(_required_path("MANYFORGE_ROBOPLAN_CANDIDATE_WORKER")),
            "--cell-id",
            manifest["cell_id"],
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )
    client = ExecutionCommandClient(execution_root, timeout_seconds=2.0)
    deadline = time.monotonic() + 10.0
    last_error = ""
    while time.monotonic() < deadline:
        if process.poll() is not None:
            detail = (process.stderr.read() if process.stderr else b"").decode(
                errors="replace"
            )
            raise RuntimeError(
                f"native execution host exited during startup: {detail}"
            )
        try:
            response = client.status()
            if response.get("ok") is True:
                return process
        except HeadlessApiError as error:
            last_error = f"{error.code}: {error}"
        time.sleep(0.01)
    _stop_host(process)
    raise RuntimeError(f"native execution host did not become ready: {last_error}")


def _stop_host(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5.0)


def _runner_submission(
    runner: OrchestratorRunner,
    *,
    request_id: str,
    joint_names: tuple[str, ...],
    positions: tuple[float, ...],
) -> dict[str, Any]:
    return runner.submit_joint_goal(
        request_id=request_id,
        program_run_id="n3-program-run-1",
        component_role_id="robot",
        target_role_id="arm",
        node_instance_id="root/native-move[0]",
        node_attempt_id=f"n3-program-run-1/{request_id}",
        move_id=f"move-{request_id}",
        joint_names=joint_names,
        positions_rad=positions,
        correlation_id="n3-process-test",
    )


def _wait_terminal(
    runner: OrchestratorRunner, operation_id: str
) -> dict[str, Any]:
    deadline = time.monotonic() + 20.0
    last: dict[str, Any] = {}
    while time.monotonic() < deadline:
        last = runner.operation(operation_id)
        operation = last.get("operation")
        if isinstance(operation, dict) and operation.get("state") in TERMINAL_STATES:
            return last
        time.sleep(0.01)
    raise RuntimeError(f"operation did not reach a terminal state: {last}")


def test_first_complete_addressed_native_motion_path(tmp_path: Path) -> None:
    built_fixture = _required_path("MANYFORGE_I6_NATIVE_FIXTURE_ROOT")
    fixture_root = tmp_path / "fixture"
    shutil.copytree(built_fixture, fixture_root)
    manifest = json.loads((fixture_root / "manifest.json").read_text("utf-8"))
    assert manifest["api_version"] == (
        "manyforge.i6.native-execution-fixture.v1"
    )
    assert manifest["source_joint_names"] == [
        "joint1",
        "joint2",
        "joint3",
        "joint4",
        "joint5",
        "joint6",
    ]
    execution_root = tmp_path / "execution"
    staging_root = tmp_path / "staging"
    execution_root.mkdir()
    staging_root.mkdir()

    process = _start_host(
        fixture_root, execution_root, staging_root, manifest
    )
    raw_api = TestClient(
        create_app(
            service=HeadlessService(
                UnusedCore(),  # type: ignore[arg-type]
                UnusedManager(),  # type: ignore[arg-type]
                ExecutionCommandClient(execution_root, timeout_seconds=10.0),
            )
        )
    )
    headless = HeadlessHttpClient("http://headless.test", client=raw_api)
    recording = RecordingTransport(headless)
    runner = OrchestratorRunner(KernelClient(recording))

    composer_app = FastAPI()
    composer_app.include_router(create_headless_router(headless))
    raw_composer = TestClient(composer_app)
    composer = HeadlessHttpClient("http://composer.test", client=raw_composer)

    try:
        first_status = headless.execution_status()["status"]
        assert first_status["program_binding_id"] == manifest["program_binding_id"]
        assert first_status["retained_bundle_id"] == manifest["retained_bundle_id"]

        with pytest.raises(HeadlessClientError) as rejected:
            _runner_submission(
                runner,
                request_id="reject-unknown-source-joint",
                joint_names=("arm_joints",),
                positions=(0.05,),
            )
        assert rejected.value.code == "execution_rejected"
        assert rejected.value.status_code == 400
        assert recording.last_submission is not None
        raw_rejection = raw_api.post(
            "/api/execution/operations", json=recording.last_submission
        )
        assert raw_rejection.status_code == 400
        assert raw_rejection.json()["error"]["details"]["reason"] == (
            "joint_goal_source_joint_unresolved"
        )

        accepted = _runner_submission(
            runner,
            request_id="accepted-source-joint-subset",
            joint_names=("joint1",),
            positions=(0.05,),
        )
        stale_submission = copy.deepcopy(recording.last_submission)
        assert stale_submission is not None
        serialized = json.dumps(stale_submission, sort_keys=True)
        assert "robot_id" not in serialized
        assert "group_id" not in serialized
        operation_id = accepted["accepted"]["operation_id"]
        terminal = _wait_terminal(runner, operation_id)
        assert terminal["operation"]["state"] == "completed", terminal

        composer_projection = composer.operation(operation_id)
        assert composer_projection["operation"] == terminal["operation"]
        assert composer.execution_status()["status"]["owner_epoch"] == (
            first_status["owner_epoch"]
        )

        _stop_host(process)
        process = _start_host(
            fixture_root, execution_root, staging_root, manifest
        )
        restarted = headless.execution_status()["status"]
        assert restarted["owner_epoch"] != first_status["owner_epoch"]
        with pytest.raises(HeadlessClientError) as stale:
            headless.execute(stale_submission)
        assert stale.value.code == "stale_request_owner_epoch"
        assert stale.value.status_code == 409
        with pytest.raises(HeadlessClientError) as retired:
            composer.operation(operation_id)
        assert retired.value.status_code == 404
    finally:
        _stop_host(process)
        raw_composer.close()
        raw_api.close()
