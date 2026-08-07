"""I6 process test across host, headless, Behavior, and Composer callers."""

from __future__ import annotations

import copy
import json
import os
import shutil
import sqlite3
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
from manyforge_headless_api.app import TrustedPrincipal, create_app
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


class FixedTrustedPrincipalAdapter:
    def resolve(self, _request: object) -> TrustedPrincipal:
        return TrustedPrincipal("operator.process-test", "process-test")


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

    def pause_operation(
        self, operation_id: str, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self.client.pause_operation(operation_id, body)

    def resume_operation(
        self, operation_id: str, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self.client.resume_operation(operation_id, body)

    def reset_operation(
        self, operation_id: str, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self.client.reset_operation(operation_id, body)

    def acknowledge_completed_operation(
        self, operation_id: str, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self.client.acknowledge_completed_operation(operation_id, body)

    def stop_operations(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self.client.stop_operations(body)

    def set_operation_runtime_scale(
        self, operation_id: str, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self.client.set_operation_runtime_scale(operation_id, body)

    def operation_runtime_scale(self, operation_id: str) -> dict[str, Any]:
        return self.client.operation_runtime_scale(operation_id)


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


def _wait_state(
    runner: OrchestratorRunner,
    operation_id: str,
    expected: set[str],
    *,
    timeout_seconds: float = 20.0,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_seconds
    last: dict[str, Any] = {}
    while time.monotonic() < deadline:
        last = runner.operation(operation_id)
        operation = last.get("operation")
        if isinstance(operation, dict) and operation.get("state") in expected:
            return last
        time.sleep(0.005)
    raise RuntimeError(
        f"operation did not reach one of {sorted(expected)}: {last}"
    )


def _wait_runtime_scale_applied(
    client: HeadlessHttpClient,
    operation_id: str,
    generation: int,
    scale: float,
) -> dict[str, Any]:
    deadline = time.monotonic() + 5.0
    last: dict[str, Any] = {}
    while time.monotonic() < deadline:
        last = client.operation_runtime_scale(operation_id)
        status = last.get("runtime_scale")
        if (
            isinstance(status, dict)
            and status.get("runtime_scale_generation") == generation
            and status.get("state") == "applied"
            and abs(float(status.get("effective_scale", -1.0)) - scale) <= 1e-9
        ):
            return last
        time.sleep(0.005)
    raise RuntimeError(f"runtime scale was not applied: {last}")


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
            ),
            trusted_principal_adapter=FixedTrustedPrincipalAdapter(),
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

        terminal_revision = terminal["operation"]["operation_revision"]
        acknowledged = composer.acknowledge_completed_operation(
            operation_id,
            {
                "request_context": {
                    "request_id": "acknowledge-first-operation",
                    "request_owner_epoch": first_status["owner_epoch"],
                    "source_label": "composer_ui",
                    "correlation_id": "n4-public-control-test",
                },
                "operation_id": operation_id,
                "expected_operation_revision": terminal_revision,
                "reason": "release completed test operation",
            },
        )
        assert acknowledged["accepted"]["control"] == "acknowledge_completed"
        acknowledged_projection = composer.operation(operation_id)
        assert acknowledged_projection["operation"]["operation_revision"] == (
            acknowledged["accepted"]["operation_revision_after_acceptance"]
        )

        controlled = _runner_submission(
            runner,
            request_id="n4-controlled-operation",
            joint_names=("joint1",),
            positions=(0.75,),
        )
        controlled_id = controlled["accepted"]["operation_id"]
        executing = _wait_state(runner, controlled_id, {"executing"})[
            "operation"
        ]
        scaled = runner.set_operation_runtime_scale(
            request_id="scale-controlled-operation",
            operation_id=controlled_id,
            expected_operation_revision=executing["operation_revision"],
            expected_runtime_scale_generation=(
                executing["runtime_scale_generation"] or 0
            ),
            scale=0.5,
            correlation_id="n4-public-control-test",
        )
        assert scaled["accepted"]["runtime_scale_generation"] == 1
        scale_status = _wait_runtime_scale_applied(
            composer,
            controlled_id,
            scaled["accepted"]["runtime_scale_generation"],
            0.5,
        )
        assert scale_status["runtime_scale"]["requested_scale"] == 0.5
        assert scale_status["runtime_scale"]["effective_scale"] == 0.5
        scale_participant = scale_status["runtime_scale"]["participants"][0]
        assert scale_participant["commanded_scale"] == 0.5
        assert scale_participant["applied_scale"] == 0.5
        assert scale_participant["controller_goal_id"] == executing[
            "participants"
        ][0]["controller_goal_id"]

        paused = composer.pause_operation(
            controlled_id,
            {
                "request_context": {
                    "request_id": "pause-controlled-operation",
                    "request_owner_epoch": first_status["owner_epoch"],
                    "source_label": "composer_ui",
                    "correlation_id": "n4-public-control-test",
                },
                "operation_id": controlled_id,
                "expected_operation_revision": executing["operation_revision"],
                "reason": "exercise public Pause",
            },
        )
        paused_state = _wait_state(runner, controlled_id, {"paused"})[
            "operation"
        ]
        assert paused_state["operation_revision"] == paused["accepted"][
            "operation_revision_after_acceptance"
        ]

        resumed = headless.resume_operation(
            controlled_id,
            {
                "request_context": {
                    "request_id": "resume-controlled-operation",
                    "request_owner_epoch": first_status["owner_epoch"],
                    "source_label": "headless_process_test",
                    "correlation_id": "n4-public-control-test",
                },
                "operation_id": controlled_id,
                "expected_operation_revision": paused_state[
                    "operation_revision"
                ],
                "reason": "exercise public Resume",
            },
        )
        resumed_state = _wait_state(runner, controlled_id, {"executing"})[
            "operation"
        ]
        assert resumed_state["operation_revision"] == resumed["accepted"][
            "operation_revision_after_acceptance"
        ]

        stop_arguments = {
            "request_id": "stop-controlled-operation",
            "operation_id": controlled_id,
            "expected_operation_revision": resumed_state[
                "operation_revision"
            ],
            "expected_stop_generation": 0,
            "reason": "exercise historical operator Stop",
            "correlation_id": "n4-public-control-test",
        }
        stop_accepted = runner.stop_operation(**stop_arguments)
        stop_replayed = runner.stop_operation(**stop_arguments)
        assert stop_accepted["accepted"]["stop_id"] == stop_replayed[
            "accepted"
        ]["stop_id"]
        assert stop_replayed["accepted"]["replay_status"] == "replayed_exact"
        stopped = _wait_terminal(runner, controlled_id)["operation"]
        assert stopped["state"] == "stopped"
        assert stopped["active_stop_id"] == stop_accepted["accepted"]["stop_id"]
        with sqlite3.connect(
            execution_root / "operation-journal.sqlite3"
        ) as database:
            durable_stop = database.execute(
                "SELECT payload_json FROM journal_records WHERE request_id = ?",
                ("stop-controlled-operation",),
            ).fetchone()
        assert durable_stop is not None
        durable_stop_payload = json.loads(durable_stop[0])
        assert durable_stop_payload["intent"]["request_attribution"] == {
            "assurance": "externally_authenticated",
            "principal_id": "operator.process-test",
            "transport": "http",
        }

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
        retained = composer.operation(operation_id)
        assert retained["operation"] == acknowledged_projection["operation"]
    finally:
        _stop_host(process)
        raw_composer.close()
        raw_api.close()
