"""Private Program-runner HTTP boundary and lifecycle tests."""

from __future__ import annotations

from typing import Any

import pytest
from fastapi.testclient import TestClient

from manyforge_headless_api.models import (
    ProgramRunStartRequest,
    ProgramRunStopRequest,
)
from manyforge_program_runner.app import create_app
from manyforge_program_runner.models import ProgramCycleResetRequest
from manyforge_program_runner.runtime import ProgramRunnerError


_DIGEST_A = "sha256:" + "a" * 64
_DIGEST_B = "sha256:" + "b" * 64
_DIGEST_C = "sha256:" + "c" * 64


def _snapshot(*, state: str = "running") -> dict[str, Any]:
    return {
        "schema_version": "manyforge.program_run_status.v1",
        "owner_id": "program-runner.cell-a",
        "owner_epoch": "program-runner.epoch.1",
        "run_revision": 1,
        "state": state,
        "capture": {
            "schema_version": "manyforge.program_run_capture.v1",
            "program_run_id": "program-run.1",
            "scenario_id": "cell-pick",
            "scenario_catalog_revision": _DIGEST_A,
            "program_id": "pick",
            "program_revision": _DIGEST_B,
            "program_binding_id": _DIGEST_C,
            "retained_bundle_id": _DIGEST_A,
            "graph_revision": _DIGEST_B,
            "node_environment_digest": _DIGEST_C,
            "owner_identities": [
                {
                    "owner_role": "program_runner",
                    "owner_id": "program-runner.cell-a",
                    "owner_epoch": "program-runner.epoch.1",
                }
            ],
        },
        "tree_status": "running",
        "active_operation_count": 1,
        "reason_code": None,
        "message": "",
    }


class FakeRuntime:
    def __init__(self) -> None:
        self.started = False
        self.closed = False
        self.calls: list[tuple[str, Any]] = []

    def start(self) -> None:
        self.started = True

    def shutdown(self) -> None:
        self.closed = True

    def health(self) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.program_runner_health.v1",
            "owner_id": "program-runner.cell-a",
            "owner_epoch": "program-runner.epoch.1",
            "started": self.started,
            "closed": self.closed,
            "assistant_model_transport_available": False,
            "assistant_snapshot_source_available": False,
            "simulation_control_available": False,
        }

    def runs(self, *, active_only: bool, limit: int) -> dict[str, Any]:
        self.calls.append(("runs", (active_only, limit)))
        return {
            "schema_version": "manyforge.program_run_index.v1",
            "owner_id": "program-runner.cell-a",
            "owner_epoch": "program-runner.epoch.1",
            "active_only": active_only,
            "truncated": False,
            "runs": [_snapshot()],
        }

    def start_run(self, request: ProgramRunStartRequest) -> dict[str, Any]:
        self.calls.append(("start", request))
        return _snapshot()

    def read_run(self, program_run_id: str) -> dict[str, Any]:
        self.calls.append(("read", program_run_id))
        return _snapshot()

    def stop_run(
        self,
        program_run_id: str,
        request: ProgramRunStopRequest,
    ) -> dict[str, Any]:
        self.calls.append(("stop", (program_run_id, request)))
        return _snapshot(state="stopping")

    def reset_cycle(self, request: ProgramCycleResetRequest) -> dict[str, Any]:
        self.calls.append(("reset", request))
        return {
            "schema_version": "manyforge.program_cycle_reset_result.v1",
            "program_run_id": request.program_run_id,
            "reset_cycle_generation": request.expected_cycle_generation,
            "idle_cycle_generation": request.expected_cycle_generation + 1,
            "settlement_reference": request.settlement_evidence_digest,
            "admission": {"admission_generation": 8},
        }

    def cycle_status(self) -> dict[str, Any]:
        return {
            "program_runner_owner_epoch": "program-runner.epoch.1",
            "cycle_generation": 1,
            "state": "idle_after_reset",
            "program_run_id": None,
            "cycle_claim_generation": None,
            "terminal_or_settlement_reference": None,
        }


def _caller() -> dict[str, Any]:
    return {
        "request_id": "request.1",
        "request_owner_epoch": "composer.epoch.1",
        "source_label": "operator_ui",
        "correlation_id": None,
    }


def test_reset_is_an_explicit_strict_cycle_transition() -> None:
    runtime = FakeRuntime()
    client = TestClient(create_app(runtime))
    body = {
        **_caller(),
        "program_run_id": "program-run.1",
        "expected_owner_epoch": "program-runner.epoch.1",
        "expected_cycle_generation": 4,
        "expected_claim_generation": 7,
        "terminal_reference": _DIGEST_A,
        "settlement_evidence_digest": _DIGEST_B,
    }
    with client:
        response = client.post("/api/program/cycle/reset", json=body)
    assert response.status_code == 200
    assert response.json()["idle_cycle_generation"] == 5
    assert runtime.calls[0][0] == "reset"
    assert client.post(
        "/api/program/cycle/reset", json={**body, "unexpected": True}
    ).status_code == 422


def test_cycle_status_exposes_only_program_runner_owned_facts() -> None:
    client = TestClient(create_app(FakeRuntime()))
    with client:
        response = client.get("/api/program/cycle/status")
    assert response.status_code == 200
    assert set(response.json()) == {
        "program_runner_owner_epoch",
        "cycle_generation",
        "state",
        "program_run_id",
        "cycle_claim_generation",
        "terminal_or_settlement_reference",
    }


def test_private_surface_projects_owner_facts_and_manages_lifecycle() -> None:
    runtime = FakeRuntime()
    client = TestClient(create_app(runtime))
    with client:
        health = client.get("/api/program/health")
        index = client.get(
            "/api/program/runs",
            params={"active_only": "true", "limit": 7},
        )
        started = client.post(
            "/api/program/runs",
            json={
                **_caller(),
                "scenario_id": "cell-pick",
                "scenario_catalog_revision": _DIGEST_A,
                "expected_applied_record_digest": _DIGEST_B,
            },
        )
        read = client.get("/api/program/runs/program-run.1")
        stopped = client.post(
            "/api/program/runs/program-run.1/stop",
            json={
                **_caller(),
                "request_id": "request.2",
                "program_run_id": "program-run.1",
                "expected_owner_epoch": "program-runner.epoch.1",
                "reason": "operator stop",
            },
        )

    assert health.status_code == 200
    assert health.json()["started"] is True
    assert index.json()["runs"][0]["capture"]["program_run_id"] == "program-run.1"
    assert started.status_code == 200
    assert read.status_code == 200
    assert stopped.json()["state"] == "stopping"
    assert runtime.calls[0] == ("runs", (True, 7))
    assert runtime.calls[1][0] == "start"
    assert runtime.calls[2] == ("read", "program-run.1")
    assert runtime.calls[3][0] == "stop"
    assert runtime.closed is True


def test_private_surface_rejects_unknown_fields_and_path_body_mismatch() -> None:
    runtime = FakeRuntime()
    client = TestClient(create_app(runtime))
    body = {
        **_caller(),
        "program_run_id": "program-run.2",
        "expected_owner_epoch": "program-runner.epoch.1",
        "reason": None,
    }
    with client:
        mismatch = client.post(
            "/api/program/runs/program-run.1/stop",
            json=body,
        )
        unknown = client.post(
            "/api/program/runs",
            json={
                **_caller(),
                "scenario_id": "cell-pick",
                "scenario_catalog_revision": _DIGEST_A,
                "expected_applied_record_digest": _DIGEST_B,
                "invented": True,
            },
        )

    assert mismatch.status_code == 400
    assert mismatch.json()["detail"]["code"] == "program_run_path_invalid"
    assert unknown.status_code == 422
    assert runtime.calls == []


def test_private_surface_preserves_stable_owner_error_codes() -> None:
    class FailingRuntime(FakeRuntime):
        def read_run(self, program_run_id: str) -> dict[str, Any]:
            raise ProgramRunnerError(
                "program_run_not_found",
                f"unknown Program run {program_run_id}",
            )

    client = TestClient(create_app(FailingRuntime()))
    with client:
        response = client.get("/api/program/runs/missing")

    assert response.status_code == 404
    assert response.json() == {
        "detail": {
            "code": "program_run_not_found",
            "message": "unknown Program run missing",
        }
    }


@pytest.mark.parametrize(
    "code",
    [
        "authoring_not_ready",
        "program_cycle_reset_required",
        "program_run_draft_applied_mismatch",
        "stale_generation",
    ],
)
def test_program_start_preconditions_map_to_client_conflict(code: str) -> None:
    class RejectingRuntime(FakeRuntime):
        def start_run(self, request: ProgramRunStartRequest) -> dict[str, Any]:
            raise ProgramRunnerError(code, "start precondition rejected")

    client = TestClient(create_app(RejectingRuntime()))
    with client:
        response = client.post(
            "/api/program/runs",
            json={
                **_caller(),
                "scenario_id": "cell-pick",
                "scenario_catalog_revision": _DIGEST_A,
                "expected_applied_record_digest": _DIGEST_B,
            },
        )
    assert response.status_code == 409
    assert response.json()["detail"]["code"] == code
