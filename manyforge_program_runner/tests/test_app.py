"""Private Program-runner HTTP boundary and lifecycle tests."""

from __future__ import annotations

import hashlib

import asyncio
import json
import threading
from typing import Any

import pytest
from fastapi.testclient import TestClient

from manyforge_headless_api.models import (
    ProgramHaltResumeRequest,
    ProgramRunStartRequest,
    ProgramScenePreparationRequest,
)
from manyforge_headless_api.component_model import RUN_REFUSAL_CODES
from manyforge_program_runner.app import (
    ProgramControlStateSource,
    _serve_program_control_state,
    _status,
    create_app,
)
from manyforge_program_runner.models import (
    ProgramCycleResetRequest,
    ProgramNodeAvailabilityRequest,
)
from manyforge_program_runner.runtime import ProgramRunnerError


def _kb(*parts) -> bytes:
    """A deterministic 16-byte request key named by text, for readable tests."""

    return hashlib.md5("/".join(map(str, parts)).encode()).digest()


def _key(*parts) -> str:
    """The wire form (32 lowercase hex) of ``_kb``."""

    return _kb(*parts).hex()


_DIGEST_A = "sha256:" + "a" * 64
_DIGEST_B = "sha256:" + "b" * 64
_DIGEST_C = "sha256:" + "c" * 64


def _snapshot(*, state: str = "running") -> dict[str, Any]:
    return {
        "schema_version": "manyforge.program_run_status.v5",
        "owner_id": "program-runner.cell-a",
        "run_revision": 1,
        "state": state,
        "capture": {
            "schema_version": "manyforge.program_run_capture.v3",
            "program_run_id": 1,
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
                    "role": "program_runner",
                    "application_start_id": "3f9a00000000002a",
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
        self.projections = 0
        self.reset_reads = 0

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
            "schema_version": "manyforge.program_runner_health.v2",
            "owner_id": "program-runner.cell-a",
            "application_start_id": "3f9a00000000002a",
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
            "schema_version": "manyforge.program_node_availability.v2",
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
            "schema_version": "manyforge.program_run_index.v3",
            "owner_id": "program-runner.cell-a",
            "application_start_id": "3f9a00000000002a",
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
        return {"schema_version": "manyforge.program_node_correction_preview_intent.v3"}

    def resume_halt(
        self,
        program_run_id: str,
        request: ProgramHaltResumeRequest,
    ) -> dict[str, Any]:
        self.calls.append(("resume_halt", (program_run_id, request)))
        return _snapshot(state="running")

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
        return self.add_reset_robot_position(self.cycle_projection())

    def add_reset_robot_position(self, cycle: dict[str, Any]) -> dict[str, Any]:
        self.reset_reads += 1
        cycle["reset_robot_position"] = {"available": False, "reason": "unsupported"}
        return cycle

    def cycle_projection(self) -> dict[str, Any]:
        self.projections += 1
        return {
            "cycle_generation": 1,
            "state": "idle_after_reset",
            "program_run_id": None,
            "cycle_claim_generation": None,
            "terminal_or_settlement_reference": None,
            "execution_control_revision": self.control_revision,
            "causal_request_key": None,
            "current_command_kind": None,
            "runner_commands": {"in_progress": None, "ended": []},
            "failure": None,
            "reset_phase": None,
        "reset_options": None,
        "reset_robot_position": {"available": False, "reason": "unsampled"},
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

    def read_persistent_values(self) -> dict[str, Any]:
        self.calls.append(("read_persistent_values", None))
        return {"project_path": "/projects/cell-pick.scenario"}

    def set_persistent_value(self, request) -> dict[str, Any]:
        self.calls.append(("set_persistent_value", request))
        return {"action": "set"}

    def reset_persistent_value(self, request) -> dict[str, Any]:
        self.calls.append(("reset_persistent_value", request))
        return {"action": "reset"}

    def clear_persistent_values(self, request) -> dict[str, Any]:
        self.calls.append(("clear_persistent_values", request))
        return {"action": "clear"}

    def reconcile_persistent_values(self) -> dict[str, Any]:
        self.calls.append(("reconcile_persistent_values", None))
        return {"outcome": "duplicate"}

    def run_command(self, request_key, command) -> dict[str, Any]:
        # The runtime answers at acceptance (OD-214); this fake runs the
        # command to its end and answers with its accepted row.
        command()
        return _reply(_KIND_OF_CALL.get(self.calls[-1][0], self.calls[-1][0]),
                      request_key)


_KIND_OF_CALL = {
    "set_persistent_value": "persistent_value_set",
    "reset_persistent_value": "persistent_value_reset",
    "clear_persistent_values": "persistent_values_clear",
    "write_runtime_blackboard": "runtime_blackboard_write",
    "stage_manual_result": "manual_result_stage",
    "apply_recovery_action": "recovery_action",
}


def _reply(kind: str, request_key: str | None = None) -> dict[str, Any]:
    return {
        "schema_version": "manyforge.program_command_reply.v1",
        "command": {"kind": kind, "request_key": request_key or _key("request.1"),
                    "step": "admitting", "accepted_at_ns": "1", "end": None},
    }


def _caller() -> dict[str, Any]:
    return {
        "request_key": _key("request.1"),
        "source_label": "operator_ui",
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
        "manyforge.program_node_availability.v2"
    )
    assert runtime.calls[0][0] == "node_availability"
    assert runtime.calls[0][1].model_dump() == body


def test_reset_is_an_explicit_strict_cycle_transition() -> None:
    runtime = FakeRuntime()
    client = TestClient(create_app(runtime))
    body = {
        **_caller(),
        "program_run_id": 1,
        "expected_cycle_generation": 4,
        "expected_claim_generation": 7,
        "terminal_reference": _DIGEST_A,
        "settlement_evidence_digest": _DIGEST_B,
    }
    with client:
        response = client.post("/api/program/cycle/reset", json=body)
    assert response.status_code == 200
    # Answered at acceptance (OD-214): the reply is the command's row.
    assert response.json() == _reply("reset")
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
        "cycle_generation",
        "state",
        "program_run_id",
        "cycle_claim_generation",
        "terminal_or_settlement_reference",
        "execution_control_revision",
        "causal_request_key",
        "current_command_kind",
        "runner_commands",
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

        def cycle_projection(self) -> dict[str, Any]:
            cycle = super().cycle_projection()
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
            assert first["schema_version"] == "manyforge.program_control_state.v4"
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


def test_control_state_frame_reads_reset_position_once_at_send() -> None:
    """Classification snapshots skip the host Reset-position read; each sent
    frame reads it once and equals the complete cycle status."""

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
        # A still clock keeps the node-progress window open, so the publisher
        # always coalesces the node-only wake.
        source = ProgramControlStateSource(runtime, clock_ns=lambda: 1)
        websocket = FakeWebSocket()
        serving = asyncio.create_task(
            _serve_program_control_state(websocket, source)  # type: ignore[arg-type]
        )
        first = json.loads(await asyncio.wait_for(websocket.outbound.get(), timeout=1.0))
        assert (runtime.projections, runtime.reset_reads) == (1, 1)
        runtime.publish_control_state(node_only=True)
        progress = json.loads(await asyncio.wait_for(websocket.outbound.get(), timeout=1.0))
        assert progress["event_class"] == "node_progress"
        # Before: one full status per snapshot (here 2 Reset reads per frame).
        assert (runtime.projections, runtime.reset_reads) == (3, 2)
        for frame in (first, progress):
            assert frame["program_cycle"]["reset_robot_position"] == {
                "available": False, "reason": "unsupported"}
        assert progress["program_cycle"] == runtime.cycle_status()
        await websocket.inbound.put({"type": "websocket.disconnect"})
        await asyncio.wait_for(serving, timeout=1.0)
        source.close()

    asyncio.run(exercise())


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
                "request_key": _key("request.prepare.1"),
                "expected_applied_record_digest": _DIGEST_B,
                "expected_admission_generation": 7,
            },
        )
        read = client.get("/api/program/runs/1")
        motion_started = client.post(
            "/api/program/runs/1/motion-start",
            json={
                **_caller(),
                "request_key": _key("request.motion-start.1"),
                "program_run_id": 1,
                "expected_hold_revision": 0,
                "group_ids": ["robot_group"],
                "reason": "initial operator motion Run",
            },
        )
        resumed = client.post(
            "/api/program/runs/1/resume-halt",
            json={
                **_caller(),
                "request_key": _key("request.resume-halt.1"),
                "program_run_id": 1,
                "expected_run_revision": 1,
                "reason": "operator Execute after Halt",
            },
        )
        # Kernel-first: the runner has no Stop route; every client Stop goes
        # Headless -> Kernel.
        removed_stop_routes = [
            client.post(f"/api/program/runs/1/{route}", json={})
            for route in (
                "stop", "motion-stop", "full-stop", "discard-full-stop",
                "resume-full-stop",
            )
        ]

    assert health.status_code == 200
    assert prepared.status_code == 200
    assert prepared.json()["status"] == "prepared"
    assert health.json()["started"] is True
    assert index.json()["runs"][0]["capture"]["program_run_id"] == 1
    assert started.status_code == 200
    assert read.status_code == 200
    assert started.json() == _reply("start")
    assert motion_started.json() == _reply("start_motion", _key("request.motion-start.1"))
    assert resumed.json() == _reply("resume_halt", _key("request.resume-halt.1"))
    assert [response.status_code for response in removed_stop_routes] == [404] * 5
    assert runtime.calls[0] == ("runs", (True, 7))
    assert runtime.calls[1][0] == "start"
    assert runtime.calls[2][0] == "prepare_scene"
    assert runtime.calls[3] == ("read", 1)
    assert runtime.calls[4][0] == "start_motion"
    assert runtime.calls[5][0] == "resume_halt"
    assert runtime.calls[5][1][0] == 1
    assert isinstance(runtime.calls[5][1][1], ProgramHaltResumeRequest)
    assert len(runtime.calls) == 6
    assert runtime.closed is True


def test_private_surface_rejects_unknown_fields_and_path_body_mismatch() -> None:
    runtime = FakeRuntime()
    client = TestClient(create_app(runtime))
    body = {
        **_caller(),
        "program_run_id": 2,
        "expected_hold_revision": 0,
        "group_ids": ["robot_group"],
        "reason": None,
    }
    with client:
        mismatch = client.post(
            "/api/program/runs/1/motion-start",
            json=body,
        )
        # Run numbers are decimal integers >= 1 (1.15.3).
        textual = client.get("/api/program/runs/program-run.1")
        zero = client.get("/api/program/runs/0")
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
    assert textual.status_code == 422
    assert zero.status_code == 422
    assert unknown.status_code == 422
    assert runtime.calls == []


def test_private_surface_preserves_stable_owner_error_codes() -> None:
    class FailingRuntime(FakeRuntime):
        def read_run(self, program_run_id: int) -> dict[str, Any]:
            raise ProgramRunnerError(
                "program_run_not_found",
                f"unknown Program run {program_run_id}",
                producer="kernel",
            )

    client = TestClient(create_app(FailingRuntime()))
    with client:
        response = client.get("/api/program/runs/7")

    assert response.status_code == 404
    assert response.json() == {
        "detail": {
            "code": "program_run_not_found",
            "details": {},
            "message": "unknown Program run 7",
            "producer": "kernel",
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


def test_persistent_value_routes_are_applied_project_strict_requests() -> None:
    runtime = FakeRuntime()
    base = "/api/program/persistent-values"
    project = "/projects/cell-pick.scenario"
    edit = {
        **_caller(),
        "project_path": project,
        "key": "layer",
        "expected_program_revision": _DIGEST_A,
    }
    with TestClient(create_app(runtime)) as client:
        assert client.get(base).json() == {"project_path": project}
        assert client.post(base, json={**edit, "value": 3}).json() == _reply(
            "persistent_value_set"
        )
        assert client.post(f"{base}/reset", json=edit).json() == _reply(
            "persistent_value_reset"
        )
        cleared = client.post(
            f"{base}/clear",
            json={**_caller(), "project_path": project},
        )
        assert cleared.json() == _reply("persistent_values_clear")
        # The project folder is absolute and lexically normalized.
        for path in (
            "projects/cell-pick.scenario",
            "/projects/../cell-pick.scenario",
            "/projects/cell-pick.scenario/",
            "//projects/cell-pick.scenario",
            "/projects/\x00cell-pick",
            "/" + "p" * 4096,
        ):
            assert client.post(
                f"{base}/clear", json={**_caller(), "project_path": path}
            ).status_code == 422
        assert client.post(f"{base}/clear", json=_caller()).status_code == 422
        # The former Scenario-id addressed routes are gone.
        assert client.get(
            "/api/program/scenarios/cell-pick/persistent-values"
        ).status_code == 404
        # A value is required to set, and nothing else is accepted.
        assert client.post(base, json=edit).status_code == 422
        assert client.post(f"{base}/reset", json={**edit, "value": 3}).status_code == 422
        # A live run is named with its revision; the register sequence is no
        # longer a fence (without a run the last admitted edit wins).
        assert client.post(
            f"{base}/reset", json={**edit, "program_run_id": 1}
        ).status_code == 422
        assert client.post(
            f"{base}/reset", json={**edit, "expected_sequence": 4}
        ).status_code == 422
        assert client.post(
            f"{base}/reset",
            json={**edit, "program_run_id": 1, "expected_run_revision": 2},
        ).json() == _reply("persistent_value_reset")
        assert client.post(
            f"{base}/clear",
            json={**_caller(), "project_path": project, "expected_sequence": 4},
        ).status_code == 422
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
    assert runtime.calls[1][1].value == 3


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
        "schema_version": "manyforge.program_node_correction_preview_request.v2",
        "request_key": _key("node-preview.1"),
        "program_run_id": 1,
        "program_id": "lite6_program",
        "program_revision": "sha256:" + "a" * 64,
        "program_binding_id": "sha256:" + "a" * 64,
        "node_instance_id": "move",
        "node_attempt_id": 1,
        "proposed_inputs": [{"path": "goal", "value": []}],
    }

    with TestClient(create_app(runtime)) as client:
        response = client.post(
            "/api/program/runs/1/node-correction-previews", json=body
        )
        foreign = client.post(
            "/api/program/runs/1/node-correction-previews",
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
    ("authoring_not_ready", 409),
    ("program_cycle_reconciliation_required", 503),
    ("program_runner_internal_error", 500),
])
def test_started_then_failed_response_preserves_actual_identity_and_native_details(
    code: str, status: int,
) -> None:
    details = {"program_run_id": 2, "claim_generation": 9, "needs_reset": True,
        "cause_code": "component_lifecycle_facts_stale", "admission_changed": "true",
        "admission_generation_before": "36", "admission_generation_after": "37"}
    class FailedStart(FakeRuntime):
        def start_run(self, _request):
            raise ProgramRunnerError(code, "Owner outcome", details=details)
    with TestClient(create_app(FailedStart())) as client:
        response = client.post("/api/program/runs", json={**_caller(), "request_key": _key("run.failed.1"),
            "scenario_id": "cell-pick", "source_document_digest": _DIGEST_A,
            "expected_applied_record_digest": _DIGEST_B, "draft_program_revision": _DIGEST_A})
    assert response.status_code == status
    assert response.json() == {"detail": {"code": code,
        "message": "Owner outcome", "details": details,
        "producer": "program_runner"}}


def test_run_refusals_are_409_not_5xx() -> None:
    # A readiness or run refusal is known before any effect: never 5xx.
    for code in RUN_REFUSAL_CODES:
        assert _status(code) == 409, code
    assert _status("program_runner_unavailable") == 503


def test_a_forwarded_request_without_its_key_is_a_bad_request() -> None:
    """1.15.1: the runner refuses a request Headless forwarded without its key
    with ``request_key_required``, an internal contract violation (400)."""

    class KeylessRuntime(FakeRuntime):
        def start_run(self, request):
            raise ProgramRunnerError(
                "request_key_required", "the forwarded request carries no request key"
            )

    client = TestClient(create_app(KeylessRuntime()))
    with client:
        response = client.post(
            "/api/program/runs",
            json={
                "source_label": "operator_ui",
                "scenario_id": "cell-pick",
                "source_document_digest": _DIGEST_A,
                "expected_applied_record_digest": _DIGEST_B,
                "draft_program_revision": _DIGEST_B,
            },
        )
    assert response.status_code == 400
    assert response.json()["detail"]["code"] == "request_key_required"


def test_startup_collects_once_after_start_and_before_serving(monkeypatch) -> None:
    runtime = FakeRuntime()
    settles: list[tuple[str, bool, int]] = []
    monkeypatch.setattr(
        "manyforge_program_runner.app.settle_heap",
        lambda point: settles.append((point, runtime.started, len(runtime.calls))),
    )
    with TestClient(create_app(runtime)) as client:
        assert settles == [("start", True, 0)]
        assert client.get("/api/program/health").status_code == 200
    assert settles == [("start", True, 0)]
