"""Private Program-runner HTTP boundary and lifecycle tests."""

from __future__ import annotations

import asyncio
import json
import threading
from typing import Any

import pytest
from fastapi.testclient import TestClient

from manyforge_headless_api.models import (
    ProgramFullStopResumeRequest,
    ProgramFullStopResetRequest,
    ProgramRunStartRequest,
    ProgramRunStopRequest,
    ProgramScenePreparationRequest,
)
from manyforge_program_runner.app import (
    ProgramControlStateSource,
    _serve_program_control_state,
    create_app,
)
from manyforge_program_runner.models import (
    ProgramCycleResetRequest,
    ProgramNodeAvailabilityRequest,
)
from manyforge_program_runner.runtime import ProgramRunnerError


_DIGEST_A = "sha256:" + "a" * 64
_DIGEST_B = "sha256:" + "b" * 64
_DIGEST_C = "sha256:" + "c" * 64


def _snapshot(*, state: str = "running") -> dict[str, Any]:
    return {
        "schema_version": "manyforge.program_run_status.v3",
        "owner_id": "program-runner.cell-a",
        "owner_epoch": "program-runner.epoch.1",
        "run_revision": 1,
        "state": state,
        "capture": {
            "schema_version": "manyforge.program_run_capture.v2",
            "program_run_id": "program-run.1",
            "applied_record_digest": _DIGEST_C,
            "scenario_id": "cell-pick",
            "source_document_digest": _DIGEST_A,
            "program_id": "pick",
            "program_revision": _DIGEST_B,
            "program_binding_id": _DIGEST_C,
            "retained_bundle_id": _DIGEST_A,
            "graph_revision": _DIGEST_B,
            "node_environment_digest": _DIGEST_C,
            "node_policy_digest": _DIGEST_A,
            "node_policies": [],
            "owner_identities": [
                {
                    "owner_role": "program_runner",
                    "owner_id": "program-runner.cell-a",
                    "owner_epoch": "program-runner.epoch.1",
                }
            ],
        },
        "tree_status": "running",
        "node_status_revision": 1,
        "node_statuses": {"root": "running"},
        "active_operation_count": 1,
        "reason_code": None,
        "message": "",
        "details": None,
        "origin": None,
    }


class FakeRuntime:
    def __init__(self) -> None:
        self.started = False
        self.closed = False
        self.calls: list[tuple[str, Any]] = []
        self.control_state_observers: set[Any] = set()
        self.control_revision = 1
        self.node_status_revision = 1

    def add_control_state_observer(self, observer) -> None:
        self.control_state_observers.add(observer)

    def remove_control_state_observer(self, observer) -> None:
        self.control_state_observers.discard(observer)

    def publish_control_state(self, *, node_only: bool = False) -> None:
        self.node_status_revision += 1
        if not node_only:
            self.control_revision += 1
        for observer in tuple(self.control_state_observers):
            observer()

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

    def node_availability(
        self, request: ProgramNodeAvailabilityRequest
    ) -> dict[str, Any]:
        self.calls.append(("node_availability", request))
        return {
            "schema_version": "manyforge.program_node_availability.v1",
            "source": {},
            "availability": {},
        }

    def diagnostic_health(self) -> dict[str, Any]:
        return self.health()

    def resume_motion(self, program_run_id: str, request) -> dict[str, Any]:
        self.calls.append(("resume_motion", (program_run_id, request)))
        return _snapshot()

    def start_motion(self, program_run_id: str, request) -> dict[str, Any]:
        self.calls.append(("start_motion", (program_run_id, request)))
        return _snapshot()

    def prepare_manual_handover(self, program_run_id: str, request) -> dict[str, Any]:
        raise AssertionError("Manual handover is outside this fixture's scenarios")

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

    def prepare_applied_scene(
        self, request: ProgramScenePreparationRequest
    ) -> dict[str, Any]:
        self.calls.append(("prepare_scene", request))
        return {
            "schema_version": "manyforge.program_scene_preparation.v1",
            "status": "prepared",
            "applied_record_digest": request.expected_applied_record_digest,
            "program_revision": _DIGEST_B,
            "admission_generation": 9,
            "prepared_scene": {"schema_version": "manyforge.prepared_program_scene.v1"},
        }

    def read_run(self, program_run_id: str) -> dict[str, Any]:
        self.calls.append(("read", program_run_id))
        return _snapshot()

    def preview_node_correction(self, program_run_id: str, request) -> dict[str, Any]:
        self.calls.append(("node_preview", (program_run_id, request)))
        return {"schema_version": "manyforge.program_node_correction_preview_intent.v1"}

    def stop_run(
        self,
        program_run_id: str,
        request: ProgramRunStopRequest,
    ) -> dict[str, Any]:
        self.calls.append(("stop", (program_run_id, request)))
        return _snapshot(state="stopping")

    def stop_motion(
        self,
        program_run_id: str,
        request: ProgramRunStopRequest,
    ) -> dict[str, Any]:
        self.calls.append(("motion_stop", (program_run_id, request)))
        return _snapshot(state="running")

    def full_stop(
        self,
        program_run_id: str,
        request: ProgramRunStopRequest,
    ) -> dict[str, Any]:
        self.calls.append(("full_stop", (program_run_id, request)))
        return _snapshot(state="full_stopping")

    def resume_full_stop(
        self,
        program_run_id: str,
        request: ProgramFullStopResumeRequest,
    ) -> dict[str, Any]:
        self.calls.append(("resume_full_stop", (program_run_id, request)))
        return _snapshot(state="running")

    def discard_full_stop(
        self,
        program_run_id: str,
        request: ProgramFullStopResetRequest,
    ) -> dict[str, Any]:
        self.calls.append(("discard_full_stop", (program_run_id, request)))
        return _snapshot(state="stopped")

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
            "execution_control_revision": self.control_revision,
            "causal_correlation_id": None,
            "causal_session_id": None,
            "causal_client_sequence": None,
            "current_command_kind": None,
            "failure": None,
            "reset_phase": None,
        "reset_options": None,
        "reset_robot_position": {"available": False, "reason": "unsupported"},
            "allowed_commands": [],
            "run_revision": None,
            "node_status_revision": self.node_status_revision,
            "runtime_control_revision": None,
            "recovery_offer": None,
            "run": None,
            "runtime_controls": None,
        }

    def read_runtime_controls(self, program_run_id: str) -> dict[str, Any]:
        self.calls.append(("runtime_controls", program_run_id))
        return {"program_run_id": program_run_id}

    def write_runtime_blackboard(self, program_run_id: str, request) -> dict[str, Any]:
        self.calls.append(("write_runtime_blackboard", (program_run_id, request)))
        return {"accepted": True}

    def stage_manual_result(self, program_run_id: str, request) -> dict[str, Any]:
        self.calls.append(("stage_manual_result", (program_run_id, request)))
        return {"accepted": True}

    def apply_recovery_action(self, request) -> dict[str, Any]:
        self.calls.append(("apply_recovery_action", request))
        return {"accepted": True}

    def read_persistent_values(self, scenario_id: str) -> dict[str, Any]:
        self.calls.append(("read_persistent_values", scenario_id))
        return {"scenario_id": scenario_id}

    def set_persistent_value(self, scenario_id: str, request) -> dict[str, Any]:
        self.calls.append(("set_persistent_value", (scenario_id, request)))
        return {"action": "set"}

    def reset_persistent_value(self, scenario_id: str, request) -> dict[str, Any]:
        self.calls.append(("reset_persistent_value", (scenario_id, request)))
        return {"action": "reset"}

    def clear_persistent_values(self, scenario_id: str, request) -> dict[str, Any]:
        self.calls.append(("clear_persistent_values", (scenario_id, request)))
        return {"action": "clear"}

    def reconcile_persistent_values(self) -> dict[str, Any]:
        self.calls.append(("reconcile_persistent_values", None))
        return {"outcome": "duplicate"}


def _caller() -> dict[str, Any]:
    return {
        "request_id": "request.1",
        "request_owner_epoch": "composer.epoch.1",
        "source_label": "operator_ui",
        "correlation_id": None,
    }


def test_node_availability_is_a_read_only_runner_projection() -> None:
    runtime = FakeRuntime()
    body = {
        "applied": {"program_id": "program_a"},
    }
    with TestClient(create_app(runtime)) as client:
        response = client.post("/api/program/node-availability", json=body)

    assert response.status_code == 200
    assert response.json()["schema_version"] == (
        "manyforge.program_node_availability.v1"
    )
    assert runtime.calls[0][0] == "node_availability"
    assert runtime.calls[0][1].model_dump() == body


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
    assert (
        client.post(
            "/api/program/cycle/reset", json={**body, "unexpected": True}
        ).status_code
        == 422
    )


def test_cycle_status_exposes_only_program_runner_owned_facts() -> None:
    runtime = FakeRuntime()
    client = TestClient(create_app(runtime))
    with client:
        response = client.get("/api/cycle/status")
    assert response.status_code == 200
    assert set(response.json()) == {
        "program_runner_owner_epoch",
        "cycle_generation",
        "state",
        "program_run_id",
        "cycle_claim_generation",
        "terminal_or_settlement_reference",
        "execution_control_revision",
        "causal_correlation_id",
        "causal_session_id",
        "causal_client_sequence",
        "current_command_kind",
        "failure",
        "reset_phase",
        "reset_options",
        "reset_robot_position",
        "allowed_commands",
        "run_revision",
        "node_status_revision",
        "runtime_control_revision",
        "recovery_offer",
        "run",
        "runtime_controls",
    }
    assert client.get("/api/program/cycle/status").status_code == 404
    assert runtime.control_state_observers == set()


def test_control_state_websocket_is_change_driven_and_demand_scoped() -> None:
    class GroupRuntime(FakeRuntime):
        group_state = "running"

        def cycle_status(self) -> dict[str, Any]:
            cycle = super().cycle_status()
            cycle["runtime_controls"] = {
                "execution_selections": {
                    "groups": [{"group_id": "arm", "state": self.group_state}]
                }
            }
            return cycle

    runtime = GroupRuntime()
    client = TestClient(create_app(runtime))

    with client:
        assert runtime.control_state_observers == set()
        with client.websocket_connect("/ws/program/control-state") as websocket:
            first = websocket.receive_json()
            assert first["schema_version"] == "manyforge.program_control_state.v2"
            assert first["event_class"] == "critical"
            assert first["server_sequence"] == "1"
            assert first["program_cycle"]["execution_control_revision"] == 1
            assert len(runtime.control_state_observers) == 1

            runtime.publish_control_state()
            second = websocket.receive_json()
            assert second["server_sequence"] == "2"
            assert second["program_cycle"]["execution_control_revision"] == 2
            assert second["program_cycle"]["node_status_revision"] == 2
            assert second["event_class"] == "critical"
            runtime.publish_control_state(node_only=True)
            progress = websocket.receive_json()
            assert progress["event_class"] == "node_progress"
            for state in ("stopping", "held", "running"):
                runtime.group_state = state
                runtime.publish_control_state(node_only=True)
                change = websocket.receive_json()
                assert change["event_class"] == "critical"
                assert change["program_cycle"]["execution_control_revision"] == 2

        assert runtime.control_state_observers == set()


def test_control_state_websocket_releases_disconnected_clients_without_owner_wake() -> (
    None
):
    class FakeWebSocket:
        def __init__(self) -> None:
            self.inbound: asyncio.Queue[dict[str, Any]] = asyncio.Queue()
            self.outbound: asyncio.Queue[str] = asyncio.Queue()

        async def accept(self) -> None:
            return None

        async def receive(self) -> dict[str, Any]:
            return await self.inbound.get()

        async def send_text(self, payload: str) -> None:
            await self.outbound.put(payload)

        async def close(self, *, code: int, reason: str) -> None:
            del code, reason

    async def exercise() -> None:
        runtime = FakeRuntime()
        source = ProgramControlStateSource(runtime)
        # More sequential clients than the concurrent capacity proves that a
        # quiet owner cannot retain dead subscriptions indefinitely.
        for _ in range(6):
            websocket = FakeWebSocket()
            serving = asyncio.create_task(
                _serve_program_control_state(websocket, source)  # type: ignore[arg-type]
            )
            first = json.loads(
                await asyncio.wait_for(websocket.outbound.get(), timeout=1.0)
            )
            assert first["server_sequence"] == "1"
            assert len(runtime.control_state_observers) == 1
            await websocket.inbound.put({"type": "websocket.disconnect"})
            await asyncio.wait_for(serving, timeout=1.0)
            assert runtime.control_state_observers == set()
        source.close()

    asyncio.run(exercise())


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
                "source_document_digest": _DIGEST_A,
                "expected_applied_record_digest": _DIGEST_B,
                "draft_program_revision": _DIGEST_B,
            },
        )
        prepared = client.post(
            "/api/program/scene-preparations",
            json={
                **_caller(),
                "request_id": "request.prepare.1",
                "expected_applied_record_digest": _DIGEST_B,
                "expected_admission_generation": 7,
            },
        )
        read = client.get("/api/program/runs/program-run.1")
        motion_stopped = client.post(
            "/api/program/runs/program-run.1/motion-stop",
            json={
                **_caller(),
                "request_id": "request.motion-stop.1",
                "program_run_id": "program-run.1",
                "expected_owner_epoch": "program-runner.epoch.1",
                "reason": "ordinary operator motion Stop",
            },
        )
        motion_started = client.post(
            "/api/program/runs/program-run.1/motion-start",
            json={
                **_caller(),
                "request_id": "request.motion-start.1",
                "program_run_id": "program-run.1",
                "expected_owner_epoch": "program-runner.epoch.1",
                "expected_run_revision": 1,
                "expected_selection_revision": 0,
                "group_ids": ["robot_group"],
                "reason": "initial operator motion Run",
            },
        )
        full_stopped = client.post(
            "/api/program/runs/program-run.1/full-stop",
            json={
                **_caller(),
                "request_id": "request.full-stop.1",
                "program_run_id": "program-run.1",
                "expected_owner_epoch": "program-runner.epoch.1",
                "reason": "ordinary operator Full Stop",
            },
        )
        resumed = client.post(
            "/api/program/runs/program-run.1/resume-full-stop",
            json={
                **_caller(),
                "request_id": "request.resume-full-stop.1",
                "program_run_id": "program-run.1",
                "expected_owner_epoch": "program-runner.epoch.1",
                "expected_run_revision": 1,
                "reason": "operator Run after Full Stop",
            },
        )
        discarded = client.post(
            "/api/program/runs/program-run.1/discard-full-stop",
            json={
                **_caller(),
                "request_id": "request.discard-full-stop.1",
                "program_run_id": "program-run.1",
                "expected_owner_epoch": "program-runner.epoch.1",
                "expected_run_revision": 1,
                "expected_cycle_generation": 4,
                "expected_claim_generation": 7,
                "reason": "operator Reset discarded Full Stop",
            },
        )
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
    assert prepared.status_code == 200
    assert prepared.json()["status"] == "prepared"
    assert health.json()["started"] is True
    assert index.json()["runs"][0]["capture"]["program_run_id"] == "program-run.1"
    assert started.status_code == 200
    assert read.status_code == 200
    assert motion_stopped.json()["state"] == "running"
    assert motion_started.json()["state"] == "running"
    assert full_stopped.json()["state"] == "full_stopping"
    assert resumed.json()["state"] == "running"
    assert discarded.json()["state"] == "stopped"
    assert stopped.json()["state"] == "stopping"
    assert runtime.calls[0] == ("runs", (True, 7))
    assert runtime.calls[1][0] == "start"
    assert runtime.calls[2][0] == "prepare_scene"
    assert runtime.calls[3] == ("read", "program-run.1")
    assert runtime.calls[4][0] == "motion_stop"
    assert runtime.calls[5][0] == "start_motion"
    assert runtime.calls[6][0] == "full_stop"
    assert runtime.calls[7][0] == "resume_full_stop"
    assert runtime.calls[8][0] == "discard_full_stop"
    assert runtime.calls[9][0] == "stop"
    assert runtime.closed is True


def test_stop_route_progresses_while_both_ordinary_workers_are_occupied() -> None:
    class PressuredRuntime(FakeRuntime):
        def __init__(self) -> None:
            super().__init__()
            self._lock = threading.Lock()
            self._ordinary_started_count = 0
            self.ordinary_workers_started = threading.Event()
            self.release_ordinary = threading.Event()
            self.stop_called = threading.Event()

        def start_run(self, request: ProgramRunStartRequest) -> dict[str, Any]:
            with self._lock:
                self._ordinary_started_count += 1
                if self._ordinary_started_count == 2:
                    self.ordinary_workers_started.set()
            self.release_ordinary.wait(timeout=2.0)
            return super().start_run(request)

        def stop_motion(
            self,
            program_run_id: str,
            request: ProgramRunStopRequest,
        ) -> dict[str, Any]:
            self.stop_called.set()
            return super().stop_motion(program_run_id, request)

    runtime = PressuredRuntime()
    app = create_app(runtime)
    start_endpoint = next(
        route.endpoint
        for route in app.routes
        if getattr(route, "path", None) == "/api/program/runs"
        and "POST" in (getattr(route, "methods", set()) or set())
    )
    stop_endpoint = next(
        route.endpoint
        for route in app.routes
        if getattr(route, "path", None)
        == "/api/program/runs/{program_run_id}/motion-stop"
    )
    start_request = ProgramRunStartRequest.model_validate(
        {
            **_caller(),
            "scenario_id": "cell-pick",
            "source_document_digest": _DIGEST_A,
            "expected_applied_record_digest": _DIGEST_B,
            "draft_program_revision": _DIGEST_B,
        }
    )
    stop_request = ProgramRunStopRequest.model_validate(
        {
            **_caller(),
            "request_id": "request.stop.pressure",
            "program_run_id": "program-run.1",
            "expected_owner_epoch": "program-runner.epoch.1",
            "reason": "priority qualification",
        }
    )

    async def exercise() -> None:
        async with app.router.lifespan_context(app):
            ordinary = [
                asyncio.create_task(start_endpoint(start_request)) for _ in range(2)
            ]
            try:
                assert await asyncio.to_thread(
                    runtime.ordinary_workers_started.wait,
                    1.0,
                )
                stopped = await asyncio.wait_for(
                    stop_endpoint("program-run.1", stop_request),
                    timeout=0.5,
                )
                assert json.loads(stopped.body)["state"] == "running"
                assert runtime.stop_called.is_set()
                assert not runtime.release_ordinary.is_set()
            finally:
                runtime.release_ordinary.set()
                await asyncio.gather(*ordinary)

    asyncio.run(exercise())
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
                "source_document_digest": _DIGEST_A,
                "expected_applied_record_digest": _DIGEST_B,
                "draft_program_revision": _DIGEST_B,
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
            "details": {},
            "message": "unknown Program run missing",
        }
    }


@pytest.mark.parametrize(
    "code",
    [
        "authoring_not_ready",
        "program_cycle_reset_required",
        "program_run_draft_applied_mismatch",
        "program_scene_preparation_failed",
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
                "source_document_digest": _DIGEST_A,
                "expected_applied_record_digest": _DIGEST_B,
                "draft_program_revision": _DIGEST_B,
            },
        )
    assert response.status_code == 409
    assert response.json()["detail"]["code"] == code


def test_persistent_value_routes_are_scenario_scoped_strict_requests() -> None:
    runtime = FakeRuntime()
    base = "/api/program/scenarios/cell-pick/persistent-values"
    edit = {
        **_caller(),
        "scenario_id": "cell-pick",
        "key": "layer",
        "expected_program_revision": _DIGEST_A,
    }
    with TestClient(create_app(runtime)) as client:
        assert client.get(base).json() == {"scenario_id": "cell-pick"}
        assert client.post(base, json={**edit, "value": 3}).json() == {
            "action": "set"
        }
        assert client.post(f"{base}/reset", json=edit).json() == {"action": "reset"}
        cleared = client.post(
            f"{base}/clear",
            json={**_caller(), "scenario_id": "cell-pick"},
        )
        assert cleared.json() == {"action": "clear"}
        # A value is required to set, and nothing else is accepted.
        assert client.post(base, json=edit).status_code == 422
        assert client.post(f"{base}/reset", json={**edit, "value": 3}).status_code == 422
        # A live run is named with its revision; the register sequence is no
        # longer a fence (without a run the last admitted edit wins).
        assert client.post(
            f"{base}/reset", json={**edit, "program_run_id": "run.1"}
        ).status_code == 422
        assert client.post(
            f"{base}/reset", json={**edit, "expected_sequence": 4}
        ).status_code == 422
        assert client.post(
            f"{base}/reset",
            json={**edit, "program_run_id": "run.1", "expected_run_revision": 2},
        ).json() == {"action": "reset"}
        assert client.post(
            f"{base}/clear",
            json={**_caller(), "scenario_id": "cell-pick", "expected_sequence": 4},
        ).status_code == 422
        assert client.get("/api/program/scenarios/Bad!/persistent-values").status_code == 422
        # Headless's wake after a committed Apply carries nothing to trust.
        assert client.post("/api/program/persistent-values/reconcile").json() == {
            "outcome": "duplicate"
        }

    assert [name for name, _values in runtime.calls] == [
        "read_persistent_values",
        "set_persistent_value",
        "reset_persistent_value",
        "clear_persistent_values",
        "reset_persistent_value",
        "reconcile_persistent_values",
    ]
    assert runtime.calls[1][1][1].value == 3


def test_runtime_override_routes_are_removed_and_key_writes_remain() -> None:
    paths = {
        getattr(route, "path", None) for route in create_app(FakeRuntime()).routes
    }
    assert not any("runtime-overrides" in str(path) for path in paths)
    assert "/api/program/runs/{program_run_id}/blackboard-values" in paths
    assert "/api/program/runs/{program_run_id}/runtime-controls" in paths


def test_node_correction_preview_is_a_plain_runner_query() -> None:
    runtime = FakeRuntime()
    body = {
        "schema_version": "manyforge.program_node_correction_preview_request.v1",
        "request_id": "node-preview.1",
        "program_run_id": "program-run.1",
        "expected_owner_epoch": "program-runner.epoch.1",
        "program_id": "lite6_program",
        "program_revision": "sha256:" + "a" * 64,
        "program_binding_id": "sha256:" + "a" * 64,
        "node_instance_id": "move",
        "node_attempt_id": "attempt:1",
        "proposed_inputs": [{"path": "goal", "value": []}],
    }

    with TestClient(create_app(runtime)) as client:
        response = client.post(
            "/api/program/runs/program-run.1/node-correction-previews", json=body
        )
        foreign = client.post(
            "/api/program/runs/program-run.1/node-correction-previews",
            json={**body, "target_role_id": "other"},
        )

    assert response.status_code == 200, response.text
    assert foreign.status_code == 422
    assert [kind for kind, _value in runtime.calls] == ["node_preview"]
    assert runtime.calls[0][1][1].model_dump(mode="json") == body


def test_run_contract_removes_client_generation_and_rejects_old_wire_field():
    runtime = FakeRuntime()
    body = {**_caller(), "scenario_id": "cell-pick",
            "source_document_digest": _DIGEST_A,
            "expected_applied_record_digest": _DIGEST_B,
            "draft_program_revision": _DIGEST_B}
    with TestClient(create_app(runtime)) as client:
        assert client.post("/api/program/runs", json=body).status_code == 200
        legacy = client.post("/api/program/runs", json={**body, "expected_admission_generation": 36})
        assert legacy.status_code == 422
        schema = client.get("/openapi.json").json()["components"]["schemas"]["ProgramRunStartRequest"]
        assert "expected_admission_generation" not in schema["properties"]
        assert "expected_admission_generation" not in schema["required"]
    assert len([call for call in runtime.calls if call[0] == "start"]) == 1


@pytest.mark.parametrize("code,status", [
    ("program_run_started_then_failed", 422),
    ("program_run_superseded_by_stop", 422),
    ("program_run_stopped_before_start", 409),
    ("authoring_not_ready", 409),
    ("program_cycle_reconciliation_required", 503),
    ("program_runner_internal_error", 500),
])
def test_started_then_failed_response_preserves_actual_identity_and_native_details(
    code: str, status: int,
) -> None:
    details = {"program_run_id": "program-cycle.2", "claim_generation": 9, "needs_reset": True,
        "cause_code": "component_lifecycle_facts_stale", "admission_changed": "true",
        "admission_generation_before": "36", "admission_generation_after": "37"}
    class FailedStart(FakeRuntime):
        def start_run(self, _request):
            raise ProgramRunnerError(code, "Owner outcome", details=details)
    with TestClient(create_app(FailedStart())) as client:
        response = client.post("/api/program/runs", json={**_caller(), "request_id": "run.failed.1",
            "scenario_id": "cell-pick", "source_document_digest": _DIGEST_A,
            "expected_applied_record_digest": _DIGEST_B, "draft_program_revision": _DIGEST_A})
    assert response.status_code == status
    assert response.json() == {"detail": {"code": code,
        "message": "Owner outcome", "details": details}}
