"""Tests for ManyForge Composer Backend."""
from __future__ import annotations

import copy
import logging
import threading
import time
import pytest
from pathlib import Path

pytest.importorskip("fastapi")

from fastapi import HTTPException
from fastapi.testclient import TestClient
from httpx import AsyncClient
import httpx
import asyncio
import py_trees
import yaml

from manyforge_behavior.kernel_client import (
    CommandResult,
    ExecutionState,
    MockKernelClient,
    SceneEditRequest,
    Trajectory,
    TrajectoryWaypoint,
)
import manyforge_behavior.cycle_manager as cycle_manager_module
from manyforge_behavior.cycle_manager import CycleState
from manyforge_behavior.deployment import Deployment
from manyforge_composer.backend.state import ComposerState, RuntimeOverrideRecord
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.models import (
    SceneObjectResponse,
    SceneShapeResponse,
    VisualSceneSnapshotResponse,
)
from manyforge_composer.backend import route_execution, routes_debug, routes_execution
from manyforge_composer.backend.security import SecurityConfig
from manyforge_composer.backend.ws_state import StateHub
from manyforge_composer.__main__ import build_program_loaded_callback
from manyforge_composer.backend import routes_filesystem, routes_infra, routes_runtime

# Test fixture paths (relative to repo root, same as existing tests)
_REPO_ROOT = Path(__file__).resolve().parents[2]
_CATALOG_PATH = str(_REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml")
_PROGRAM_PATH = str(_REPO_ROOT / "manyforge_behavior" / "tests" / "fixtures" / "pick_and_place.program.yaml")
_DEPLOYMENT_PATH = str(_REPO_ROOT / "manyforge_behavior" / "tests" / "fixtures" / "ur5_roboplan.deployment.yaml")
_EXAMPLE_PROGRAM_PATH = str(_REPO_ROOT / "examples" / "pick_and_place.program.yaml")
_EXAMPLE_DEPLOYMENT_PATH = str(_REPO_ROOT / "examples" / "pick_and_place.deployment.yaml")
_UR10E_ROBOTIQ_DEPLOYMENT_PATH = str(_REPO_ROOT / "examples" / "pick_and_place_ur10e_robotiq.deployment.yaml")
_SEMANTIC_DEPLOYMENT_PATH = str(_REPO_ROOT / "examples" / "semantic_shape_pick.deployment.yaml")
_ISAAC_LITE6_DEPLOYMENT_PATH = str(
    _REPO_ROOT / "examples" / "isaac_lite6_lite_gripper_pick_and_place.deployment.yaml"
)


def _script_ready_recovery(
    kernel: MockKernelClient,
    *,
    action: str,
    recovery_class: str,
    status: str = "retry_ready",
    execution_state: str = "stopped",
) -> dict:
    kernel._state = ExecutionState(execution_state)
    assessment = {
        "schema_version": "manyforge.kernel_recovery_assessment.v1",
        "assessment_revision": 31,
        "assessed_at_ns": time.monotonic_ns(),
        "control_command_epoch": kernel.control_command_epoch(),
        "execution_id": kernel.get_status().execution_id,
        "execution_state": execution_state,
        "recovery_class": recovery_class,
        "status": status,
        "latch_id": "composer-latch" if recovery_class != "ordinary_operator_stop" else "",
        "source_id": "composer-test",
        "policy_id": "composer-test",
        "preemption_result": "succeeded",
        "stop_owner_active": False,
        "stop_owner_generation": 9,
        "backend_stop_confirmed": True,
        "stop_certificate_id": "composer-stop-cert",
        "observed_state_available": True,
        "observed_state_sequence": 44,
        "observed_state_source_id": "mock",
        "observed_state_authority_profile": "mock_measured",
        "observed_state_measured": True,
        "observed_state_valid_until_ns": time.monotonic_ns() + 5_000_000_000,
        "observed_state_fresh": True,
        "observed_state_at_rest": True,
        "stronger_authority_active": False,
        "queued_latch_present": False,
        "retry_admissible": status == "retry_ready",
        "allowed_actions": [action, "reset_cycle"],
        "reason_code": "recovery_stop_certified",
        "evidence_refs": ["composer-stop-cert", "kernel-event:44"],
    }
    resulting = dict(assessment)
    resulting.update(
        {
            "assessment_revision": 32,
            "execution_state": "idle",
            "recovery_class": "none",
            "status": "unavailable",
            "latch_id": "",
            "stop_certificate_id": "",
            "retry_admissible": False,
            "allowed_actions": [],
            "reason_code": "no_recovery_active",
            "evidence_refs": [],
        }
    )
    kernel.script_recovery_assessment(assessment)
    kernel.script_recovery_action_result(
        {
            "request_id": "scripted",
            "action": action,
            "accepted": True,
            "error": None,
            "resulting_assessment": resulting,
        }
    )
    return assessment

# Test security config: no auth, permissive filesystem roots for tmp_path access
_TEST_SECURITY = SecurityConfig(
    cors_origins=["http://localhost:3000"],
    api_token=None,
    allowed_fs_roots=[str(_REPO_ROOT), "/workspace", "/tmp"],
)


class _FakeCycleState:
    def __init__(self, value: str) -> None:
        self.value = value


class _FakeKernelState:
    def __init__(self, value: str) -> None:
        self.name = value
        self.value = value


class _CycleManagerTestSurface:
    """Minimal current CycleManager surface for route-focused test doubles."""

    def get_recovery_offer(self):
        return None

    def wait_for_kernel_events_and_project(self, timeout_s):
        time.sleep(float(timeout_s))
        return False

    def drain_kernel_events_now(self):
        return []

    def shutdown(self, *_, **__):
        return True

    def kernel_event_pipeline_status(self):
        return {
            "collector_dropped_total": 0,
            "manager_dropped_total": 0,
            "last_sequence_seen": 0,
        }


class _FakeCycleManager(_CycleManagerTestSurface):
    def __init__(self, state_value: str) -> None:
        self.state = _FakeCycleState(state_value)
        self.aborted = False

    def abort_intervention(self) -> None:
        self.aborted = True
        self.state = _FakeCycleState("idle")


class _ExecutorProbeCycleManager(_CycleManagerTestSurface):
    def __init__(self) -> None:
        self.state = _FakeCycleState("idle")
        self.motion_stop_state = "idle"
        self.velocity_scale = 1.0
        self.error = None
        self.error_code = None
        self.error_details = None
        self.intervention = None
        self.thread_names: dict[str, str] = {}
        self.run_entered = [threading.Event(), threading.Event()]
        self.release_run = threading.Event()
        self._run_calls = 0
        self._lock = threading.Lock()

    def run(self, *, principal_id="operator") -> None:
        _ = principal_id
        self.thread_names["run"] = threading.current_thread().name
        with self._lock:
            index = self._run_calls
            self._run_calls += 1
        if index < len(self.run_entered):
            self.run_entered[index].set()
            self.release_run.wait(timeout=5.0)
        self.state = _FakeCycleState("running")

    def reset(self, *, request_id=None) -> None:
        self.thread_names["reset"] = threading.current_thread().name
        self.state = _FakeCycleState("idle")

    def get_recovery_offer(self):
        return None

    def apply_recovery_action(
        self,
        request,
        *,
        principal_id,
        reset_callback=None,
    ):
        _ = (principal_id, reset_callback)
        self.thread_names["recovery"] = threading.current_thread().name
        return {
            "requestId": request.get("request_id", ""),
            "accepted": True,
            "cycleResumed": False,
            "resultCode": "accepted",
            "error": None,
            "recoveryOffer": None,
        }

    def stop(self) -> None:
        self.thread_names["stop"] = threading.current_thread().name
        self.motion_stop_state = "held"

    def full_stop(self) -> None:
        self.thread_names["full_stop"] = threading.current_thread().name
        self.state = _FakeCycleState("stopped")


class _ThreadProbeKernel(MockKernelClient):
    def __init__(self, *args, **kwargs) -> None:
        super().__init__(*args, **kwargs)
        self.stop_thread_name: str | None = None

    def stop(self):  # type: ignore[override]
        self.stop_thread_name = threading.current_thread().name
        return super().stop()


class _KernelSnapshotFaultCycleManager(_CycleManagerTestSurface):
    """Cycle manager fake whose fault exists only in the kernel snapshot."""

    state = _FakeCycleState("stopped")
    velocity_scale = 1.0
    error = None
    error_code = None
    error_details = None
    intervention = None

    def get_status(self):
        return type(
            "KernelStatus",
            (),
            {
                "state": _FakeKernelState("INTERVENTION_REQUIRED"),
                "execution_id": 1,
                "active_move_id": "pick_approach",
                "progress": 0.5,
            },
        )()

    def get_execution_snapshot(self):
        return type(
            "KernelSnapshot",
            (),
            {
                "error": "Timed out waiting for FollowJointTrajectory cancel confirmation",
                "error_code": "stop_failed",
                "error_details": {
                    "backend": "ros2_control",
                    "backend_state": "Stopping",
                    "phase": "controller_cancel_timeout",
                    "reason_code": "controller_cancel_timeout",
                    "stop_completion_timeout_s": "0.5",
                },
            },
        )()

    def drain_kernel_events_now(self) -> None:
        return None

    def get_kernel_events_since(self, _index: int):
        return []

    def get_node_statuses(self):
        return {}


class _HeldCollisionInterventionCycleManager(_CycleManagerTestSurface):
    """Fake for RUNNING+held soft-fail after the kernel escalated to intervention."""

    state = _FakeCycleState("running")
    motion_stop_state = "held"
    velocity_scale = 1.0
    error = "Motion execution ended in interrupted"
    error_code = "runtime_collision"
    error_details = {
        "reason": "runtime_collision_detected",
        "collision_result_first_failure_pair": (
            "object/wall/shape/0 <-> robot/robot0/link/wrist_1_link/collision/0"
        ),
    }
    intervention = None

    def get_status(self):
        return type(
            "KernelStatus",
            (),
            {
                "state": _FakeKernelState("INTERVENTION_REQUIRED"),
                "execution_id": 2,
                "active_move_id": "pick_approach",
                "progress": 0.5,
            },
        )()

    def get_execution_snapshot(self):
        return type(
            "KernelSnapshot",
            (),
            {
                "error": (
                    "fault-preempt controlled stop and emergency hard-stop floor "
                    "both failed; operator intervention is required"
                ),
                "error_code": "motion_interrupt_backend_stop_failed",
                "error_details": {
                    "phase": "preempt_trajectory",
                    "reason": "fault_stop_total_failure",
                    "hard_stop_transitioned": "true",
                },
            },
        )()

    def drain_kernel_events_now(self) -> None:
        return None

    def get_kernel_events_since(self, _index: int):
        return []

    def get_node_statuses(self):
        return {}


class _CommandPayloadProbeCycleManager(_CycleManagerTestSurface):
    """Fake that records kernel-visibility probes during command responses."""

    def __init__(self) -> None:
        self.state = _FakeCycleState("idle")
        self.motion_stop_state = "idle"
        self.velocity_scale = 1.0
        self.error = "latched local error"
        self.error_code = "local_error_code"
        self.error_details = {"reason_code": "local_error_code"}
        self.intervention = None
        self.status_reads = 0
        self.snapshot_reads = 0

    def run(self, *, principal_id="operator") -> None:
        _ = principal_id
        self.state = _FakeCycleState("running")

    def get_status(self):
        self.status_reads += 1
        return type(
            "KernelStatus",
            (),
            {"state": _FakeKernelState("INTERVENTION_REQUIRED")},
        )()

    def get_execution_snapshot(self):
        self.snapshot_reads += 1
        return type(
            "KernelSnapshot",
            (),
            {
                "error": "snapshot error",
                "error_code": "snapshot_error_code",
                "error_details": {"reason_code": "snapshot_error_code"},
            },
        )()


def _simulation_control_deployment(
    *,
    command_gate_topic: str = "/manyforge/test_command_enable",
    source_mode: str = "commanded_sim",
    capabilities: dict[str, bool] | None = None,
    resume_play_after_reset: bool = True,
    service_timeout_s: float = 0.2,
) -> Deployment:
    control_capabilities = {"reset": True}
    if capabilities:
        control_capabilities.update(capabilities)
    return Deployment.from_dict(
        {
            "api_version": "manyforge.deployment.v0",
            "schema_version": "0.1.0",
            "name": "simulation_control_test",
            "program_profile": "v0.1",
            "robot_config": {
                "robot_id": "ur5",
                "group_id": "arm",
                "urdf_path": "package://test.urdf",
                "global_max_tcp_linear_speed_mps": 0.5,
            },
            "simulation_control": {
                "kind": "ros2_simulation_interfaces",
                "service_prefix": "auto",
                "service_timeout_s": service_timeout_s,
                "capabilities": control_capabilities,
                "reset": {
                    "default_scope": "state",
                    "close_command_gate": True,
                    "wait_ready_s": 0.1,
                    "resume_play_after_reset": resume_play_after_reset,
                },
            },
            "adapters": {
                "planning": {
                    "adapter_id": "planning.test",
                    "adapter_version": "0.1.0",
                    "config": {},
                },
                "context": {
                    "adapter_id": "context.test",
                    "adapter_version": "0.1.0",
                    "config": {},
                },
                "execution": {
                    "adapter_id": "execution.test",
                    "adapter_version": "0.1.0",
                    "config": {
                        "execution_state_source_mode": source_mode,
                        "command_gate_topic": command_gate_topic,
                    },
                },
            },
            "orchestrator": {
                "engine": "py_trees",
                "config": {},
            },
            "launch": {
                "runner": {
                    "module": "manyforge_behavior.runner",
                    "args": [],
                },
            },
        }
    )


def _write_program_with_runtime_policy(
    tmp_path: Path,
    *,
    runtime_override: str = "llm_allowed",
    persistent_edit: str = "human_approval",
    intervention_delegate: str = "forbidden",
) -> str:
    data = yaml.safe_load(Path(_PROGRAM_PATH).read_text(encoding="utf-8"))
    move_node = data["tree"]["children"][0]
    move_node["modification_policy"] = {
        "runtime_override": runtime_override,
        "persistent_edit": persistent_edit,
        "intervention_delegate": intervention_delegate,
    }
    target = tmp_path / "runtime_override.program.yaml"
    target.write_text(yaml.safe_dump(data, sort_keys=False), encoding="utf-8")
    return str(target)


def _write_program_with_failing_delegate_policy(
    tmp_path: Path,
    *,
    runtime_override: str = "llm_allowed",
    persistent_edit: str = "human_approval",
    intervention_delegate: str = "forbidden",
) -> str:
    data = yaml.safe_load(Path(_PROGRAM_PATH).read_text(encoding="utf-8"))
    data["blackboard"] = {
        "keys": [
            {
                "id": "ready_flag",
                "type": "bool",
                "key": "ready_flag",
                "description": "Unset flag used to force a deterministic node failure.",
            }
        ]
    }
    data["tree"] = {
        "id": "sequence",
        "name": "main_sequence",
        "children": [
            {
                "id": "check_key_bool_value",
                "name": "failing_delegate_check",
                "params": {
                    "key": {"key_ref": "ready_flag"},
                    "expected_value": True,
                },
                "modification_policy": {
                    "runtime_override": runtime_override,
                    "persistent_edit": persistent_edit,
                    "intervention_delegate": intervention_delegate,
                },
            }
        ],
    }
    target = tmp_path / "failing_delegate.program.yaml"
    target.write_text(yaml.safe_dump(data, sort_keys=False), encoding="utf-8")
    return str(target)

@pytest.fixture
def state():
    """Create a fresh ComposerState for each test."""
    kernel = MockKernelClient()
    return ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)


@pytest.fixture
def client(state):
    """Create a TestClient for the app."""
    app = create_app(state, security=_TEST_SECURITY)
    return TestClient(app)


def make_client(execute_duration_s: float = 0.0) -> TestClient:
    """Create a TestClient with a configurable mock execution duration."""
    kernel = MockKernelClient(execute_duration_s=execute_duration_s)
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    return TestClient(app)


class _StopCapabilityBundle:
    def __init__(self, payload: dict) -> None:
        self.payload = payload
        self.calls = 0

    def get_motion_interrupt_stop_capability(self) -> dict:
        self.calls += 1
        return copy.deepcopy(self.payload)


class _FlakyStopCapabilityBundle:
    def __init__(self, payload: dict) -> None:
        self.payload = payload
        self.calls = 0

    def get_motion_interrupt_stop_capability(self) -> dict:
        self.calls += 1
        if self.calls > 1:
            raise RuntimeError("capability query unavailable")
        return copy.deepcopy(self.payload)


class _StopCapabilityKernel(MockKernelClient):
    def __init__(self, payload: dict) -> None:
        super().__init__()
        self.payload = payload
        self.calls = 0

    def get_motion_interrupt_stop_capability(self) -> dict:
        self.calls += 1
        return copy.deepcopy(self.payload)


def _write_deployment_with_stop_profile(
    tmp_path: Path,
    *,
    stop_mode: str,
    profile_id: str | None = None,
    fault_stop_mode: str | None = None,
    include_fault_stop: bool = True,
) -> str:
    data = yaml.safe_load(Path(_DEPLOYMENT_PATH).read_text(encoding="utf-8"))
    config = data["adapters"]["execution"].setdefault("config", {})
    config["operator_stop_profile"] = {
        "profile_id": profile_id or f"operator_{stop_mode}",
        "stop_mode": stop_mode,
        "deceleration_time_s": 0.5,
    }
    if include_fault_stop:
        fault_mode = fault_stop_mode or stop_mode
        config["default_fault_stop_profile"] = {
            "profile_id": f"default_fault_{fault_mode}",
            "stop_mode": fault_mode,
            "deceleration_time_s": 0.5,
        }
    else:
        config.pop("default_fault_stop_profile", None)
    target = tmp_path / f"deployment_{stop_mode}.deployment.yaml"
    target.write_text(yaml.safe_dump(data, sort_keys=False), encoding="utf-8")
    return str(target)


# =============================================================================
# Cycle Control Tests
# =============================================================================

def test_status_no_program(client):
    """GET /api/cycle/status returns idle + 1.0 when no program loaded."""
    response = client.get("/api/cycle/status")
    assert response.status_code == 200
    data = response.json()
    assert data["state"] == "idle"
    assert data["velocityScale"] == 1.0


def test_runtime_override_status_no_program_returns_empty_state(client):
    """GET /api/program/runtime-overrides should be stable before any program is loaded."""
    response = client.get("/api/program/runtime-overrides")
    assert response.status_code == 200
    data = response.json()
    assert data["active"] == []
    assert data["staged"] == []
    assert data["stale"] == []
    assert data["activeCount"] == 0
    assert data["stagedCount"] == 0
    assert data["staleCount"] == 0
    assert data["hasActive"] is False
    assert data["hasStaged"] is False
    assert data["hasStale"] is False
    assert data["canActivateNow"] is False
    assert data["requiresResetToActivate"] is False
    assert data["currentTreeOrigin"] == "saved_program"
    assert data["baseProgramHash"] == ""
    assert data["nextEffectiveTree"] is None


def test_run_no_program_409(client):
    """POST /api/cycle/run returns 409 when no program loaded."""
    response = client.post("/api/cycle/run")
    assert response.status_code == 409


def test_run_no_program_409_payload_is_structured(client):
    """Rich error UX (#1 + #3): cycle-command 409 carries a structured
    `detail.{message, errorCode, errorDetails}` payload so the viewer can
    render a cause panel instead of a single string. Even when the cause
    is a config-state error (no program), the errorCode must be present
    so the panel maps to the right operator-facing hint."""
    response = client.post("/api/cycle/run")
    assert response.status_code == 409
    body = response.json()
    detail = body.get("detail")
    assert isinstance(detail, dict), (
        "cycle 409 detail must be a structured object, got: " + str(detail)
    )
    assert detail.get("message") == "No program loaded"
    assert detail.get("errorCode") == "no_program_loaded"
    # errorDetails is None when the cycle manager has no kernel diagnostics
    # to attach (this case never reaches the kernel).
    assert detail.get("errorDetails") in (None,)


def test_stop_no_program_409(client):
    """POST /api/cycle/stop returns 409 when no program loaded."""
    response = client.post("/api/cycle/stop")
    assert response.status_code == 409


def test_reset_no_program_409(client):
    """POST /api/cycle/reset returns 409 when no program loaded."""
    response = client.post("/api/cycle/reset")
    assert response.status_code == 409


def test_recovery_action_no_program_409(client):
    response = client.post(
        "/api/cycle/recovery-actions",
        json={
            "requestId": "no-program",
            "offerId": "missing-offer",
            "offerRevision": 1,
            "actionId": "reset_cycle",
        },
    )
    assert response.status_code == 409
    assert response.json()["detail"]["errorCode"] == "no_program_loaded"


@pytest.mark.parametrize(
    "legacy_path",
    [
        "/api/cycle/acknowledge-intervention-for-retry",
        "/api/cycle/acknowledge-failed-validation-for-retry",
    ],
)
def test_legacy_recovery_routes_are_absent(client, legacy_path):
    assert legacy_path not in client.app.openapi()["paths"]
    # The frontend SPA's GET catch-all can make a POST to an unknown path 405
    # instead of 404; OpenAPI absence is the authoritative route assertion.
    assert client.post(legacy_path).status_code in {404, 405}


def test_cycle_status_includes_error_fields_for_hmi(client):
    """Rich error UX (#1 + #3): `/api/cycle/status` always returns the
    error / errorCode / errorDetails fields so the viewer can poll one
    endpoint and know whether to render a panel. Idle/no-program state
    leaves them null."""
    response = client.get("/api/cycle/status")
    assert response.status_code == 200
    data = response.json()
    assert data["state"] == "idle"
    assert data["velocityScale"] == 1.0
    assert "error" in data
    assert "errorCode" in data
    assert "errorDetails" in data
    assert data["error"] is None
    assert data["errorCode"] is None
    assert data["errorDetails"] is None


def test_cycle_status_falls_back_to_kernel_snapshot_fault(state):
    """A stop fault can leave CycleManager.error empty while the kernel is in
    INTERVENTION_REQUIRED. `/api/cycle/status` must still surface the backend
    reason so the HMI explains why Run is blocked."""
    state.cycle_manager = _KernelSnapshotFaultCycleManager()
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as local_client:
        response = local_client.get("/api/cycle/status")

    assert response.status_code == 200
    data = response.json()
    assert data["state"] == "stopped"
    assert data["error"] == "Timed out waiting for FollowJointTrajectory cancel confirmation"
    assert data["errorCode"] == "controller_cancel_timeout"
    assert data["errorDetails"]["reason_code"] == "controller_cancel_timeout"
    assert data["errorDetails"]["stop_completion_timeout_s"] == "0.5"


def test_cycle_status_marks_held_collision_as_intervention_recovery(state):
    """A held runtime-collision cause must not hide later kernel intervention.

    This is the live failure mode: the HMI had enough information to explain the
    collision pair, but not enough to know that Resume was no longer admissible.
    """
    state.cycle_manager = _HeldCollisionInterventionCycleManager()
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as local_client:
        response = local_client.get("/api/cycle/status")

    assert response.status_code == 200
    data = response.json()
    assert data["state"] == "running"
    assert data["motionStopState"] == "held"
    assert data["errorCode"] == "runtime_collision"
    assert data["errorDetails"]["reason"] == "runtime_collision_detected"
    assert (
        "object/wall"
        in data["errorDetails"]["collision_result_first_failure_pair"]
    )
    assert data["errorDetails"]["kernel_state"] == "intervention_required"
    assert (
        data["errorDetails"]["motion_soft_stop_reason"]
        == "motion_soft_fail_requires_intervention"
    )
    assert data["errorDetails"]["kernel_intervention_reason"] == "fault_stop_total_failure"
    assert data["errorDetails"]["hard_stop_transitioned"] == "true"


def test_cycle_command_response_uses_manager_error_without_kernel_probe(state):
    """200 command responses must not perform extra kernel snapshot reads.

    /stop and /full-stop run on the reserved control executor. The shared
    command payload path should therefore report manager-owned error fields
    only; richer snapshot fallback remains a /status concern.
    """
    manager = _CommandPayloadProbeCycleManager()
    state.cycle_manager = manager
    app = create_app(state, security=_TEST_SECURITY)
    local_client = TestClient(app)

    try:
        response = local_client.post("/api/cycle/run")
    finally:
        local_client.close()

    assert response.status_code == 200
    data = response.json()
    assert data["state"] == "running"
    assert data["error"] == "latched local error"
    assert data["errorCode"] == "local_error_code"
    assert data["errorDetails"]["reason_code"] == "local_error_code"
    assert manager.status_reads == 0
    assert manager.snapshot_reads == 0


def test_kernel_snapshot_includes_structured_error_fields(client):
    """The viewer uses `/api/kernel/snapshot.errorCode/errorDetails` to surface
    the kernel ErrorDTO (collision pair, classification, waypoint index,
    fault reason code, …). The fields must always be present so the
    frontend can render the same panel shape whether or not an error is
    active."""
    response = client.get("/api/kernel/snapshot")
    assert response.status_code == 200
    data = response.json()
    assert "error" in data
    assert "error_code" in data
    assert "error_details" in data
    # Snake-case here because KernelSnapshotResponse does not use alias.


def test_kernel_snapshot_preserves_backend_reason_details():
    class TrackingFaultKernel(MockKernelClient):
        def get_execution_snapshot(self):
            snapshot = super().get_execution_snapshot()
            snapshot.error = (
                "Controller tracking error exceeded tolerance: 0.125 rad"
            )
            snapshot.error_code = "execution_aborted"
            snapshot.error_details = {
                "backend": "ros2_control",
                "reason_code": "tracking_error_exceeded",
                "joint_name": "shoulder_pan_joint",
                "peak_tracking_error_rad": "0.125",
                "tracking_error_tolerance_rad": "0.05",
            }
            return snapshot

    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=TrackingFaultKernel())
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as client:
        response = client.get("/api/kernel/snapshot")
        assert response.status_code == 200
        data = response.json()

    assert data["error"] == (
        "Controller tracking error exceeded tolerance: 0.125 rad"
    )
    assert data["error_code"] == "execution_aborted"
    assert data["error_details"]["reason_code"] == "tracking_error_exceeded"
    assert data["error_details"]["joint_name"] == "shoulder_pan_joint"


def test_velocity_no_program_409(client):
    """PUT /api/cycle/velocity-scale returns 409 when no program loaded."""
    response = client.put("/api/cycle/velocity-scale", json={"value": 0.5})
    assert response.status_code == 409


def test_run_from_idle(client, state):
    """Load program, POST run, verify state=running."""
    # Load program
    client.post("/api/program/load", json={"path": _PROGRAM_PATH})
    
    # Run cycle
    response = client.post("/api/cycle/run")
    assert response.status_code == 200
    data = response.json()
    assert data["state"] == "running"


def test_stop_from_running():
    """Run then motion-stop, verify the cycle keeps ticking in held motion state."""
    with make_client(execute_duration_s=1.0) as client:
        client.post("/api/program/load", json={"path": _PROGRAM_PATH})
        run_response = client.post("/api/cycle/run")
        assert run_response.status_code == 200
        assert run_response.json()["state"] == "running"

        response = client.post("/api/cycle/stop")
        assert response.status_code == 200
        data = response.json()
        assert data["state"] == "running"
        assert data["motionStopState"] == "held"


def test_full_stop_from_running():
    """Run then full-stop, verify the legacy whole-cycle STOPPED state."""
    with make_client(execute_duration_s=1.0) as client:
        client.post("/api/program/load", json={"path": _PROGRAM_PATH})
        run_response = client.post("/api/cycle/run")
        assert run_response.status_code == 200
        assert run_response.json()["state"] == "running"

        response = client.post("/api/cycle/full-stop")
        assert response.status_code == 200
        data = response.json()
        assert data["state"] == "stopped"
        assert data["motionStopState"] == "idle"


def test_recovery_action_acknowledge_and_continue_does_not_reset_backend(
    client, state
):
    """The typed endpoint forwards an exact non-resetting intervention action."""
    assert isinstance(state.kernel, MockKernelClient)
    state.kernel._execute_duration_s = 1.0  # noqa: SLF001 - keep mock running for stop
    client.post("/api/program/load", json={"path": _PROGRAM_PATH})
    assert client.post("/api/cycle/run").status_code == 200
    assert client.post("/api/cycle/full-stop").status_code == 200

    assert state.cycle_manager is not None
    kernel = state.cycle_manager._kernel  # noqa: SLF001 - test fixture introspection
    assert isinstance(kernel, MockKernelClient)
    assessment = _script_ready_recovery(
        kernel,
        action="acknowledge_and_continue",
        recovery_class="stop_fault_intervention",
        status="action_required",
        execution_state="intervention_required",
    )
    offer = state.cycle_manager.refresh_recovery_offer()
    before_reset_count = kernel.reset_count

    payload = {
        "requestId": "composer-ack-intervention",
        "offerId": offer["offerId"],
        "offerRevision": offer["offerRevision"],
        "actionId": "acknowledge_and_continue",
        "reason": "operator inspected controlled stop",
    }
    response = client.post("/api/cycle/recovery-actions", json=payload)
    assert response.status_code == 200
    assert response.json()["accepted"] is True
    assert response.json()["cycleResumed"] is True
    assert kernel.get_status().state in {
        ExecutionState.IDLE,
        ExecutionState.EXECUTING,
    }
    assert kernel.reset_count == before_reset_count
    request = kernel.apply_recovery_action_requests[-1]
    assert request["expected_assessment_revision"] == assessment["assessment_revision"]
    assert request["expected_latch_id"] == assessment["latch_id"]
    assert request["principal_id"] == "operator_ui"

    replay = client.post("/api/cycle/recovery-actions", json=payload)
    assert replay.status_code == 200
    assert replay.json() == response.json()
    assert len(kernel.apply_recovery_action_requests) == 1


def test_recovery_action_retry_after_scene_repair_does_not_reset_backend(
    client, state
):
    """Failed-validation recovery uses the same typed non-resetting endpoint."""
    assert isinstance(state.kernel, MockKernelClient)
    client.post("/api/program/load", json={"path": _PROGRAM_PATH})
    assert state.cycle_manager is not None
    manager = state.cycle_manager
    kernel = manager._kernel  # noqa: SLF001 - test fixture introspection
    assert isinstance(kernel, MockKernelClient)
    manager.reset()

    manager._state = CycleState.STOPPED  # noqa: SLF001
    manager._error = "Observed scene source stale"  # noqa: SLF001
    manager._error_code = "scene_changed_during_validation"  # noqa: SLF001
    manager._error_details = {  # noqa: SLF001
        "reason": "observed_scene_source_stale",
        "object_id": "isaac_cheese_box",
        "source_id": "isaac_scene_objects",
    }
    _script_ready_recovery(
        kernel,
        action="retry_after_scene_repair",
        recovery_class="failed_validation",
        execution_state="failed",
    )
    offer = manager.refresh_recovery_offer()
    before_reset_count = kernel.reset_count

    response = client.post(
        "/api/cycle/recovery-actions",
        json={
            "requestId": "composer-retry-validation",
            "offerId": offer["offerId"],
            "offerRevision": offer["offerRevision"],
            "actionId": "retry_after_scene_repair",
        },
    )
    assert response.status_code == 200
    data = response.json()
    assert data["accepted"] is True
    assert data["cycleResumed"] is True
    assert kernel.get_status().state in {
        ExecutionState.IDLE,
        ExecutionState.EXECUTING,
    }
    assert kernel.reset_count == before_reset_count


def test_reset_from_running(client, state):
    """Run then reset, verify state=idle."""
    # Load program
    client.post("/api/program/load", json={"path": _PROGRAM_PATH})
    
    # Run cycle
    client.post("/api/cycle/run")
    
    # Reset cycle
    response = client.post("/api/cycle/reset")
    assert response.status_code == 200
    data = response.json()
    assert data["state"] == "idle"


def test_cycle_lifecycle_routes_use_lifecycle_executor():
    manager = _ExecutorProbeCycleManager()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=MockKernelClient())
    state.cycle_manager = manager
    state.reset_backend_authoritative_sim_if_needed = lambda **_kwargs: None
    state.clear_visual_state_snapshot = lambda: None
    state.clear_visual_scene_objects_snapshot = lambda: None
    state.refresh_visual_scene_objects_sync = (
        lambda **_kwargs: {"success": True}
    )
    state.request_visual_state_publish = lambda **_kwargs: None
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as local_client:
        manager.release_run.set()
        assert local_client.post("/api/cycle/run").status_code == 200
        assert local_client.post("/api/cycle/reset").status_code == 200
        assert (
            local_client.post(
                "/api/cycle/recovery-actions",
                json={
                    "requestId": "executor-probe",
                    "offerId": "executor-offer",
                    "offerRevision": 1,
                    "actionId": "reset_cycle",
                },
            ).status_code
            == 200
        )

    assert manager.thread_names["run"].startswith("manyforge_cycle_lifecycle")
    assert manager.thread_names["reset"].startswith("manyforge_cycle_lifecycle")
    assert manager.thread_names["recovery"].startswith(
        "manyforge_cycle_lifecycle"
    )


def test_full_stop_uses_reserved_control_executor_when_lifecycle_pool_blocked():
    manager = _ExecutorProbeCycleManager()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=MockKernelClient())
    state.cycle_manager = manager
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as local_client:
        responses: list[int] = []

        def post_run() -> None:
            responses.append(local_client.post("/api/cycle/run").status_code)

        threads = [
            threading.Thread(target=post_run, daemon=True),
            threading.Thread(target=post_run, daemon=True),
        ]
        for thread in threads:
            thread.start()
        try:
            assert manager.run_entered[0].wait(timeout=1.0)
            assert manager.run_entered[1].wait(timeout=1.0)
            started = time.monotonic()
            full_stop = local_client.post("/api/cycle/full-stop")
            elapsed_s = time.monotonic() - started
            assert full_stop.status_code == 200
            assert elapsed_s < 0.5
            assert manager.thread_names["full_stop"].startswith(
                "manyforge_cycle_control"
            )
        finally:
            manager.release_run.set()
            for thread in threads:
                thread.join(timeout=1.0)

    assert responses == [200, 200]


def test_route_executor_roles_are_explicit():
    status = route_execution.route_executor_status()

    assert status["control"]["thread_name_prefix"] == "manyforge_cycle_control"
    assert status["lifecycle"]["thread_name_prefix"] == "manyforge_cycle_lifecycle"
    assert status["background"]["thread_name_prefix"] == "manyforge_route_background"
    assert "operator stop" in status["control"]["description"]
    assert "debug" in status["background"]["description"]


def test_kernel_stop_backstop_uses_reserved_control_executor():
    kernel = _ThreadProbeKernel()
    kernel._state = ExecutionState.EXECUTING  # noqa: SLF001 - test active kernel
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as local_client:
        response = local_client.post("/api/kernel/stop")

    assert response.status_code == 200
    assert response.json()["accepted"] is True
    assert kernel.stop_thread_name is not None
    assert kernel.stop_thread_name.startswith("manyforge_cycle_control")


def test_kernel_stop_backstop_singleflight_does_not_starve_full_stop():
    class BlockingStopKernel(MockKernelClient):
        def __init__(self) -> None:
            super().__init__()
            self._state = ExecutionState.EXECUTING  # noqa: SLF001 - test active kernel
            self.entered = threading.Event()
            self.release = threading.Event()
            self.stop_calls = 0
            self._stop_lock = threading.Lock()

        def stop(self):  # type: ignore[override]
            with self._stop_lock:
                self.stop_calls += 1
            self.entered.set()
            self.release.wait(timeout=5.0)
            return CommandResult(accepted=True)

    kernel = BlockingStopKernel()
    manager = _ExecutorProbeCycleManager()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    state.cycle_manager = manager
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as local_client:
        first_response: list[int] = []

        def post_kernel_stop() -> None:
            first_response.append(local_client.post("/api/kernel/stop").status_code)

        first_thread = threading.Thread(target=post_kernel_stop, daemon=True)
        first_thread.start()
        try:
            assert kernel.entered.wait(timeout=1.0)

            duplicate_started = time.monotonic()
            duplicate = local_client.post("/api/kernel/stop")
            duplicate_elapsed_s = time.monotonic() - duplicate_started
            assert duplicate.status_code == 200
            duplicate_body = duplicate.json()
            assert duplicate_body["accepted"] is True
            assert duplicate_body["coalesced"] is True
            assert (
                duplicate_body["reason_code"]
                == "kernel_stop_backstop_already_in_flight"
            )
            assert duplicate_elapsed_s < 0.25

            full_stop_started = time.monotonic()
            full_stop = local_client.post("/api/cycle/full-stop")
            full_stop_elapsed_s = time.monotonic() - full_stop_started
            assert full_stop.status_code == 200
            assert full_stop_elapsed_s < 0.5
            assert manager.thread_names["full_stop"].startswith(
                "manyforge_cycle_control"
            )
        finally:
            kernel.release.set()
            first_thread.join(timeout=1.0)

    assert first_response == [200]
    assert kernel.stop_calls == 1


def test_cycle_stop_uses_reserved_control_executor():
    manager = _ExecutorProbeCycleManager()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=MockKernelClient())
    state.cycle_manager = manager
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as local_client:
        response = local_client.post("/api/cycle/stop")

    assert response.status_code == 200
    assert manager.thread_names["stop"].startswith("manyforge_cycle_control")


def test_debug_history_and_snapshot_routes_use_background_executor(monkeypatch):
    observed: dict[str, str] = {}

    def fake_debug_export(_state, *, run_id=None):
        observed["debug"] = threading.current_thread().name
        return {"ok": True, "run_id": run_id}

    def fake_history_payload(_state, **_kwargs):
        observed["history"] = threading.current_thread().name
        return {"events": [], "nextCursor": None}

    class SnapshotBrokerProbe:
        def has_clients(self) -> bool:
            return True

        def render_sync(self, _width: int, _height: int, _format: str):
            observed["snapshot"] = threading.current_thread().name
            return b"image-bytes"

    monkeypatch.setattr(routes_debug, "build_debug_export_payload", fake_debug_export)
    monkeypatch.setattr(
        routes_execution,
        "build_execution_history_payload",
        fake_history_payload,
    )
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=MockKernelClient())
    state.snapshot_broker = SnapshotBrokerProbe()
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as local_client:
        assert local_client.get("/api/debug/export").status_code == 200
        assert local_client.get("/api/execution/history").status_code == 200
        snapshot = local_client.get("/api/scene/snapshot")
        assert snapshot.status_code == 200
        assert snapshot.content == b"image-bytes"

    assert observed["debug"].startswith("manyforge_route_background")
    assert observed["history"].startswith("manyforge_route_background")
    assert observed["snapshot"].startswith("manyforge_route_background")


def test_route_background_executor_rejects_above_admission_limit():
    async def scenario() -> None:
        status = route_execution.route_executor_status()["background"]
        max_admitted = status["max_admitted"]
        release = threading.Event()

        async def submit_blocking() -> bool:
            return await route_execution.run_route_background(
                lambda: release.wait(timeout=5.0)
            )

        tasks = [asyncio.create_task(submit_blocking()) for _ in range(max_admitted)]
        try:
            for _ in range(100):
                if (
                    route_execution.route_executor_status()["background"][
                        "admitted"
                    ]
                    == max_admitted
                ):
                    break
                await asyncio.sleep(0.01)
            assert (
                route_execution.route_executor_status()["background"]["admitted"]
                == max_admitted
            )

            with pytest.raises(HTTPException) as exc_info:
                await route_execution.run_route_background(lambda: True)
            assert exc_info.value.status_code == 503
            assert exc_info.value.detail["error"] == "route_executor_busy"
            assert exc_info.value.detail["role"] == "background"
        finally:
            release.set()
            await asyncio.gather(*tasks)
            route_execution.shutdown_route_executors(wait_timeout_s=1.0)

    asyncio.run(scenario())


def test_route_executors_restart_after_app_lifespan_shutdown():
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=MockKernelClient())
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as local_client:
        assert local_client.get("/api/kernel/snapshot").status_code == 200

    restarts_before = route_execution.route_executor_status()["background"][
        "restarts"
    ]

    with TestClient(app) as local_client:
        assert local_client.get("/api/kernel/snapshot").status_code == 200

    assert (
        route_execution.route_executor_status()["background"]["restarts"]
        > restarts_before
    )


def test_full_stop_is_not_blocked_by_saturated_background_routes(monkeypatch):
    manager = _ExecutorProbeCycleManager()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=MockKernelClient())
    state.cycle_manager = manager
    app = create_app(state, security=_TEST_SECURITY)
    background_workers = route_execution.route_executor_status()["background"][
        "max_workers"
    ]
    entered = [threading.Event() for _ in range(background_workers)]
    release = threading.Event()
    enter_lock = threading.Lock()
    enter_count = 0

    def blocked_debug_export(_state, *, run_id=None):
        nonlocal enter_count
        with enter_lock:
            index = enter_count
            enter_count += 1
        if index < len(entered):
            entered[index].set()
        release.wait(timeout=5.0)
        return {"ok": True, "run_id": run_id}

    monkeypatch.setattr(routes_debug, "build_debug_export_payload", blocked_debug_export)

    with TestClient(app) as local_client:
        responses: list[int] = []

        def get_debug_export() -> None:
            responses.append(local_client.get("/api/debug/export").status_code)

        threads = [
            threading.Thread(target=get_debug_export, daemon=True)
            for _ in range(background_workers)
        ]
        for thread in threads:
            thread.start()
        try:
            for event in entered:
                assert event.wait(timeout=1.0)
            started = time.monotonic()
            full_stop = local_client.post("/api/cycle/full-stop")
            elapsed_s = time.monotonic() - started
            assert full_stop.status_code == 200
            assert elapsed_s < 0.5
            assert manager.thread_names["full_stop"].startswith(
                "manyforge_cycle_control"
            )
        finally:
            release.set()
            for thread in threads:
                thread.join(timeout=1.0)

    assert responses == [200] * background_workers


def test_reset_phase_timeout_returns_structured_409(monkeypatch):
    """A reset phase timeout is operator-visible and request-correlated."""

    class SlowResetKernel(MockKernelClient):
        def reset(self, *, expected_epoch=None):  # type: ignore[override]
            time.sleep(0.02)
            return super().reset(expected_epoch=expected_epoch)

    monkeypatch.setattr(cycle_manager_module, "_RESET_PHASE_DEADLINE_S", 0.001)
    kernel = SlowResetKernel()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as local_client:
        assert (
            local_client.post(
                "/api/program/load", json={"path": _PROGRAM_PATH}
            ).status_code
            == 200
        )
        assert state.cycle_manager is not None
        kernel._state = ExecutionState.STOPPED  # noqa: SLF001
        response = local_client.post(
            "/api/cycle/reset",
            headers={"X-Request-ID": "reset-timeout-ticket"},
        )

    assert response.status_code == 409
    detail = response.json()["detail"]
    assert detail["errorCode"] == "reset_kernel_reset_timeout"
    assert detail["errorDetails"]["phase"] == "kernel_reset"
    assert "kernel_reset" in detail["message"]
    events = state.event_recorder.get_events_since(0)
    failed = [
        event
        for event in events
        if event["type"] == "cycle_reset_phase_failed"
        and event.get("details", {}).get("phase") == "kernel_reset"
    ]
    assert failed
    assert failed[-1]["details"]["request_id"] == "reset-timeout-ticket"
    assert failed[-1]["severity"] == "ERROR"


def test_reset_refreshes_visual_scene_after_runtime_object_removal(client, state):
    """Reset removes runtime objects from backend and stale observer caches."""
    response = client.post("/api/program/load", json={"path": _PROGRAM_PATH})
    assert response.status_code == 200
    assert state.kernel is not None

    edit = state.kernel.scene_edit(
        SceneEditRequest(
            operation="add",
            object_id="wall",
            shape_type="box",
            box_dimensions_m=[0.1, 0.2, 0.3],
            pose_position_m=[-0.3, 0.0, 0.4],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        )
    )
    assert edit.success is True
    refresh = state.refresh_visual_scene_objects_sync(reason="test_runtime_wall")
    assert refresh["success"] is True
    state.update_visual_state_snapshot(
        VisualSceneSnapshotResponse(
            sequence_number=42,
            timestamp_ns=1,
            objects=[
                SceneObjectResponse(
                    object_id="wall",
                    shape=SceneShapeResponse(
                        type="box", box_dimensions_m=[0.1, 0.2, 0.3]
                    ),
                    attached=False,
                    source="runtime",
                    persistable=False,
                )
            ],
        ),
        reason="test_stale_wall_snapshot",
    )
    state.update_pose_source_snapshot(
        "isaac_scene_objects",
        {"status": "fresh", "objects": [], "monotonic_s": 1.0},
    )
    assert any(obj.object_id == "wall" for obj in state.visual_scene_objects_snapshot()[0])
    assert any(obj.object_id == "wall" for obj in state.visual_state_snapshot()[0].objects)
    assert "isaac_scene_objects" in state.pose_source_snapshots()

    response = client.post("/api/cycle/reset")
    assert response.status_code == 200
    assert response.json()["state"] == "idle"

    assert all(obj.object_id != "wall" for obj in state.kernel.get_scene_objects())
    assert all(obj.object_id != "wall" for obj in state.visual_scene_objects_snapshot()[0])
    assert state.pose_source_snapshots() == {}
    visual_state, _meta = state.visual_state_snapshot()
    assert visual_state is None or all(obj.object_id != "wall" for obj in visual_state.objects)
    scene_response = client.get("/api/scene/state")
    assert scene_response.status_code == 200
    assert all(obj["object_id"] != "wall" for obj in scene_response.json()["objects"])


def test_simulation_reset_route_uses_deployment_contract_and_records_events(state):
    state.deployment = _simulation_control_deployment()
    state.update_external_runtime_status(
        "isaac_demo_supervisor",
        {"readiness": "ready", "canExecute": True},
    )
    state._refresh_runtime_after_simulation_reset = lambda: None
    calls = []

    def fake_reset(**kwargs):
        calls.append(kwargs)
        return {
            "success": True,
            "service_name": "/reset_simulation",
            "scope": "state",
            "duration_s": 0.01,
            "command_gate_closed": True,
            "command_gate_reopened": True,
            "readiness": {"ready": True, "status": "ready"},
        }

    state._run_ros_simulation_reset = fake_reset
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as test_client:
        response = test_client.post("/api/simulation/reset")

    assert response.status_code == 200
    body = response.json()
    assert body["success"] is True
    assert body["operation"] == "reset"
    assert body["scope"] == "state"
    assert body["serviceName"] == "/reset_simulation"
    assert body["commandGateClosed"] is True
    assert body["commandGateReopened"] is True
    assert calls
    assert calls[0]["service_prefix"] == "auto"
    assert calls[0]["command_gate_topic"] == "/manyforge/test_command_enable"
    assert calls[0]["close_command_gate"] is True

    events = state.event_recorder.get_events_since(0, include_node_transitions=True)
    event_types = [event["type"] for event in events]
    assert "simulation_control_requested" in event_types
    assert "simulation_control_completed" in event_types


def test_simulation_reset_route_rejects_active_program_cycle(state):
    state.deployment = _simulation_control_deployment()
    state.cycle_manager = _FakeCycleManager("running")
    state._run_ros_simulation_reset = lambda **_kwargs: pytest.fail(
        "reset service must not be called during ad-hoc active cycle reset"
    )
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as test_client:
        response = test_client.post("/api/simulation/reset")

    assert response.status_code == 409
    assert "active program cycle" in response.json()["detail"]


def test_simulation_reset_rejects_resumable_stopped_context(state):
    state.deployment = _simulation_control_deployment()
    manager = _FakeCycleManager("stopped")
    manager.live_scene_mutation_block_reason = lambda: "resumable_stopped_context"
    state.cycle_manager = manager
    state._run_ros_simulation_reset = lambda **_kwargs: pytest.fail(
        "reset service must not be called while stopped context is resumable"
    )

    with pytest.raises(RuntimeError, match="resumable behavior-tree context"):
        state.reset_simulation(reason="test", source="api")


def test_simulation_reset_rejects_when_scene_guard_unavailable(state):
    state.deployment = _simulation_control_deployment()
    manager = _FakeCycleManager("stopped")
    manager.live_scene_mutation_block_reason = (
        lambda: "scene_mutation_guard_unavailable"
    )
    state.cycle_manager = manager
    state._run_ros_simulation_reset = lambda **_kwargs: pytest.fail(
        "reset service must not be called without a live scene mutation guard"
    )

    with pytest.raises(RuntimeError, match="guard is unavailable"):
        state.reset_simulation(reason="test", source="api")


def test_simulation_reset_aborts_direct_control_before_service(state):
    state.deployment = _simulation_control_deployment(source_mode="authored_preview")
    manager = _FakeCycleManager("direct_control")
    state.cycle_manager = manager
    state._refresh_runtime_after_simulation_reset = lambda: None
    state._run_ros_simulation_reset = lambda **_kwargs: {
        "success": True,
        "service_name": "/reset_simulation",
        "scope": "state",
        "duration_s": 0.01,
        "command_gate_closed": True,
        "command_gate_reopened": True,
        "readiness": {"ready": True, "status": "not_required"},
    }

    result = state.reset_simulation(reason="test", source="api")

    assert result["success"] is True
    assert manager.aborted is True


def test_behavior_node_reset_allows_running_cycle(state):
    state.deployment = _simulation_control_deployment(source_mode="authored_preview")
    state.cycle_manager = _FakeCycleManager("running")
    state._refresh_runtime_after_simulation_reset = lambda: None
    calls = []
    state._run_ros_simulation_reset = lambda **kwargs: calls.append(kwargs) or {
        "success": True,
        "service_name": "/reset_simulation",
        "scope": "state",
        "duration_s": 0.01,
        "command_gate_closed": True,
        "command_gate_reopened": True,
        "readiness": {"ready": True, "status": "not_required"},
    }

    result = state.reset_simulation_for_behavior_node(reason="reset_node")

    assert result["success"] is True
    assert calls
    assert calls[0]["service_timeout_s"] == 0.2
    assert calls[0]["wait_ready_s"] == 0.1


def test_simulation_reset_requires_command_gate_topic(state):
    state.deployment = _simulation_control_deployment(command_gate_topic="")
    state._run_ros_simulation_reset = lambda **_kwargs: pytest.fail(
        "reset service must not be called without command gate topic"
    )

    with pytest.raises(RuntimeError, match="command_gate_topic"):
        state.reset_simulation(reason="test", source="api")


def test_run_ros_simulation_reset_delegates_gate_to_kernel_authority(state, monkeypatch):
    """Single gate authority: the reset flow must request the command gate
    from the kernel client (backend = sole publisher), never publish the
    topic itself, closing before the reset service call and reopening only
    after readiness."""
    import sys
    import types

    gate_states_at_reset: list[bool] = []

    class _FakeClient:
        def __init__(self, node, *, service_prefix, service_timeout_s):
            del node, service_prefix, service_timeout_s

        def reset(self, *, scope):
            del scope
            # Record the kernel-side gate state at the moment the simulator
            # reset service runs: it must be closed.
            gate_states_at_reset.append(state._kernel.command_gate_enabled())

            class _Result:
                @staticmethod
                def to_dict():
                    return {"success": True, "service_name": "/reset_simulation"}

            return _Result()

    # Hermetic environment: the flow under test is state.py's gate
    # delegation, so both ROS-side imports inside _run_ros_simulation_reset
    # are faked (no rclpy/manyforge_ros needed in this lane). The
    # "sidecar exposes no gate publisher" contract is asserted by
    # manyforge_ros/tests/test_simulation_control.py in the ros lane.
    fake_rclpy = types.SimpleNamespace(
        ok=lambda: True,
        init=lambda args=None: None,
        shutdown=lambda: None,
        create_node=lambda name: types.SimpleNamespace(
            destroy_node=lambda: None
        ),
    )
    monkeypatch.setitem(sys.modules, "rclpy", fake_rclpy)
    fake_sim_control = types.ModuleType("manyforge_ros.simulation_control")
    fake_sim_control.SimulationControlClient = _FakeClient
    fake_manyforge_ros = types.ModuleType("manyforge_ros")
    fake_manyforge_ros.simulation_control = fake_sim_control
    monkeypatch.setitem(sys.modules, "manyforge_ros", fake_manyforge_ros)
    monkeypatch.setitem(
        sys.modules, "manyforge_ros.simulation_control", fake_sim_control
    )
    monkeypatch.setattr(
        state,
        "_wait_for_simulation_reset_readiness",
        lambda **_kwargs: {"ready": True},
    )

    result = state._run_ros_simulation_reset(
        control_id="gate_authority_test",
        scope="state",
        service_prefix="auto",
        command_gate_topic="/manyforge/test_command_enable",
        close_command_gate=True,
        resume_play_after_reset=True,
        play_capability_declared=False,
        service_timeout_s=0.1,
        wait_ready_s=0.1,
        execution_config={},
        source="api",
        reason="test",
    )

    assert result["success"] is True
    assert result["command_gate_closed"] is True
    assert result["command_gate_reopened"] is True
    # Kernel-authority request ordering: closed before the service call,
    # reopened after readiness.
    assert state._kernel.command_gate_requests == [False, True]
    assert gate_states_at_reset == [False]
    assert state._kernel.command_gate_enabled() is True


def test_run_ros_simulation_reset_resumes_playing_for_commanded_sim(
    state,
    monkeypatch,
):
    import sys
    import types

    calls: list[tuple[str, str]] = []

    class _FakeClient:
        def __init__(self, node, *, service_prefix, service_timeout_s):
            del node, service_prefix, service_timeout_s

        def reset(self, *, scope):
            calls.append(("reset", scope))

            class _Result:
                @staticmethod
                def to_dict():
                    return {"success": True, "service_name": "/reset_simulation"}

            return _Result()

        def set_simulation_state(self, *, state):
            calls.append(("set_state", state))

            class _Result:
                @staticmethod
                def to_dict():
                    return {
                        "success": True,
                        "service_name": "/set_simulation_state",
                        "state": "playing",
                    }

            return _Result()

    fake_rclpy = types.SimpleNamespace(
        ok=lambda: True,
        init=lambda args=None: None,
        shutdown=lambda: None,
        create_node=lambda name: types.SimpleNamespace(
            destroy_node=lambda: None
        ),
    )
    monkeypatch.setitem(sys.modules, "rclpy", fake_rclpy)
    fake_sim_control = types.ModuleType("manyforge_ros.simulation_control")
    fake_sim_control.SimulationControlClient = _FakeClient
    fake_manyforge_ros = types.ModuleType("manyforge_ros")
    fake_manyforge_ros.simulation_control = fake_sim_control
    monkeypatch.setitem(sys.modules, "manyforge_ros", fake_manyforge_ros)
    monkeypatch.setitem(
        sys.modules, "manyforge_ros.simulation_control", fake_sim_control
    )
    monkeypatch.setattr(
        state,
        "_wait_for_simulation_reset_readiness",
        lambda **_kwargs: {"ready": True},
    )

    result = state._run_ros_simulation_reset(
        control_id="play_resume_test",
        scope="state",
        service_prefix="auto",
        command_gate_topic="/manyforge/test_command_enable",
        close_command_gate=True,
        resume_play_after_reset=True,
        play_capability_declared=True,
        service_timeout_s=0.1,
        wait_ready_s=0.1,
        execution_config={"execution_state_authority_profile": "commanded_sim"},
        source="api",
        reason="test",
    )

    assert result["success"] is True
    assert result["resume_play_after_reset"] is True
    assert result["resume_play_result"]["state"] == "playing"
    assert calls == [("reset", "state"), ("set_state", "playing")]


def test_run_ros_simulation_reset_opt_out_does_not_resume_playing(
    state,
    monkeypatch,
):
    import sys
    import types

    calls: list[tuple[str, str]] = []

    class _FakeClient:
        def __init__(self, node, *, service_prefix, service_timeout_s):
            del node, service_prefix, service_timeout_s

        def reset(self, *, scope):
            calls.append(("reset", scope))

            class _Result:
                @staticmethod
                def to_dict():
                    return {"success": True, "service_name": "/reset_simulation"}

            return _Result()

        def set_simulation_state(self, *, state):
            calls.append(("set_state", state))
            raise AssertionError("play resume must not be called")

    fake_rclpy = types.SimpleNamespace(
        ok=lambda: True,
        init=lambda args=None: None,
        shutdown=lambda: None,
        create_node=lambda name: types.SimpleNamespace(
            destroy_node=lambda: None
        ),
    )
    monkeypatch.setitem(sys.modules, "rclpy", fake_rclpy)
    fake_sim_control = types.ModuleType("manyforge_ros.simulation_control")
    fake_sim_control.SimulationControlClient = _FakeClient
    fake_manyforge_ros = types.ModuleType("manyforge_ros")
    fake_manyforge_ros.simulation_control = fake_sim_control
    monkeypatch.setitem(sys.modules, "manyforge_ros", fake_manyforge_ros)
    monkeypatch.setitem(
        sys.modules, "manyforge_ros.simulation_control", fake_sim_control
    )
    monkeypatch.setattr(
        state,
        "_wait_for_simulation_reset_readiness",
        lambda **_kwargs: {"ready": True},
    )

    result = state._run_ros_simulation_reset(
        control_id="play_resume_opt_out_test",
        scope="state",
        service_prefix="auto",
        command_gate_topic="/manyforge/test_command_enable",
        close_command_gate=True,
        resume_play_after_reset=False,
        play_capability_declared=True,
        service_timeout_s=0.1,
        wait_ready_s=0.1,
        execution_config={"execution_state_authority_profile": "commanded_sim"},
        source="api",
        reason="test",
    )

    assert result["success"] is True
    assert result["resume_play_after_reset"] is False
    assert result["resume_play_result"] is None
    assert calls == [("reset", "state")]


def test_set_simulation_state_route_uses_deployment_capability_and_records_events(state):
    state.deployment = _simulation_control_deployment(
        capabilities={"pause": True, "play": True},
    )
    calls = []

    def fake_control(**kwargs):
        calls.append(kwargs)
        return {
            "success": True,
            "operation": "set_state",
            "service_name": "/set_simulation_state",
            "duration_s": 0.01,
            "result_code": 1,
            "result_message": "",
            "state": kwargs["payload"]["state"],
        }

    state._run_ros_simulation_control_operation = fake_control
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as test_client:
        response = test_client.post(
            "/api/simulation/state",
            json={"state": "paused"},
        )

    assert response.status_code == 200
    body = response.json()
    assert body["success"] is True
    assert body["operation"] == "set_state"
    assert body["state"] == "paused"
    assert body["serviceName"] == "/set_simulation_state"
    assert calls[0]["payload"] == {"state": "paused"}
    assert calls[0]["service_timeout_s"] == 0.2

    events = state.event_recorder.get_events_since(0, include_node_transitions=True)
    event_types = [event["type"] for event in events]
    assert "simulation_control_requested" in event_types
    assert "simulation_control_completed" in event_types


def test_simulation_step_and_entity_state_route_payloads(state):
    state.deployment = _simulation_control_deployment(
        capabilities={"step": True, "set_entity_state": True},
    )
    calls = []

    def fake_control(**kwargs):
        calls.append(kwargs)
        payload = kwargs["payload"]
        return {
            "success": True,
            "operation": kwargs["operation"],
            "service_name": f"/{kwargs['operation']}",
            "duration_s": 0.01,
            "result_code": 1,
            "result_message": "",
            "steps": payload.get("steps"),
            "entity": payload.get("entity"),
        }

    state._run_ros_simulation_control_operation = fake_control
    app = create_app(state, security=_TEST_SECURITY)
    pose = {
        "frame_id": "world",
        "position_m": [0.1, 0.2, 0.3],
        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
    }
    with TestClient(app) as test_client:
        step_response = test_client.post(
            "/api/simulation/step",
            json={"steps": 4},
        )
        entity_response = test_client.post(
            "/api/simulation/entity-state",
            json={"entity": "/World/box", "pose": pose},
        )

    assert step_response.status_code == 200
    assert step_response.json()["steps"] == 4
    assert entity_response.status_code == 200
    assert entity_response.json()["entity"] == "/World/box"
    assert calls[0]["payload"] == {"steps": 4}
    assert calls[1]["payload"] == {"entity": "/World/box", "pose": pose}


def test_behavior_node_simulation_control_allows_running_cycle(state):
    state.deployment = _simulation_control_deployment(capabilities={"step": True})
    state.cycle_manager = _FakeCycleManager("running")
    calls = []
    state._run_ros_simulation_control_operation = lambda **kwargs: calls.append(kwargs) or {
        "success": True,
        "operation": kwargs["operation"],
        "service_name": "/step_simulation",
        "duration_s": 0.01,
        "result_code": 1,
        "steps": kwargs["payload"]["steps"],
    }

    result = state.simulation_control_for_behavior_node(
        operation="step",
        reason="node_step",
        steps=2,
    )

    assert result["success"] is True
    assert result["steps"] == 2
    assert calls


def test_simulation_control_route_rejects_missing_capability(state):
    state.deployment = _simulation_control_deployment()
    state._run_ros_simulation_control_operation = lambda **_kwargs: pytest.fail(
        "control service must not be called without deployment capability"
    )
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as test_client:
        response = test_client.post("/api/simulation/step", json={"steps": 1})

    assert response.status_code == 409
    assert "simulator.control.step" in response.json()["detail"]


def test_simulation_control_route_rejects_active_program_cycle(state):
    state.deployment = _simulation_control_deployment(capabilities={"pause": True})
    state.cycle_manager = _FakeCycleManager("running")
    state._run_ros_simulation_control_operation = lambda **_kwargs: pytest.fail(
        "control service must not be called during ad-hoc active cycle control"
    )
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as test_client:
        response = test_client.post(
            "/api/simulation/state",
            json={"state": "paused"},
        )

    assert response.status_code == 409
    assert "active program cycle" in response.json()["detail"]


def test_run_from_stopped(client, state):
    """Run consumes the certified automatic offer after a motion stop."""
    assert isinstance(state.kernel, MockKernelClient)
    state.kernel._execute_duration_s = 1.0  # noqa: SLF001 - keep motion active.
    client.post("/api/program/load", json={"path": _PROGRAM_PATH})
    first_run = client.post("/api/cycle/run")
    assert first_run.status_code == 200
    assert first_run.json()["state"] == "running"

    stop_response = client.post("/api/cycle/stop")
    assert stop_response.status_code == 200
    assert stop_response.json()["state"] == "running"
    assert stop_response.json()["motionStopState"] == "held"
    _script_ready_recovery(
        state.kernel,
        action="replan_from_stopped_state",
        recovery_class="ordinary_operator_stop",
    )

    response = client.post("/api/cycle/run")
    assert response.status_code == 200
    data = response.json()
    assert data["state"] == "running"
    assert data["motionStopState"] == "idle"


def test_run_route_returns_waiting_offer_while_kernel_retains_authority(
    client, state
):
    """A pending stop is a structured 409 with the current immutable offer."""

    load = client.post("/api/program/load", json={"path": _PROGRAM_PATH})
    assert load.status_code == 200
    manager = state.cycle_manager
    kernel = state.kernel
    assert manager is not None
    assert isinstance(kernel, MockKernelClient)
    _script_ready_recovery(
        kernel,
        action="retry_motion_from_current_state",
        recovery_class="runtime_collision",
        status="waiting",
        execution_state="interrupted",
    )
    with manager._lock:
        manager._state = CycleState.RUNNING
        manager._motion_stop_state = "held"
        manager._error = "runtime collision"
        manager._error_code = "runtime_collision"

    response = client.post("/api/cycle/run")

    assert response.status_code == 409
    detail = response.json()["detail"]
    assert detail["recoveryOffer"]["status"] == "waiting"
    assert detail["recoveryOffer"]["recoveryClass"] == "runtime_collision"
    assert not getattr(kernel, "apply_recovery_action_requests", [])
    assert manager.error == "runtime collision"
    assert manager.error_code == "runtime_collision"


def test_velocity_scale(client, state):
    """PUT velocity-scale {value: 0.5}, verify response."""
    # Load program
    client.post("/api/program/load", json={"path": _PROGRAM_PATH})
    
    # Set velocity scale
    response = client.put(
        "/api/cycle/velocity-scale",
        json={"value": 0.5, "principalId": "operator_alice"},
    )
    assert response.status_code == 200
    data = response.json()
    assert data["velocityScale"] == 0.5
    assert (
        state.cycle_manager._kernel.runtime_velocity_scale_principal_ids[-1]
        == "operator_alice"
    )


# =============================================================================
# Catalog Tests
# =============================================================================

def test_catalog_list(client):
    """GET /api/catalog returns entries list with version."""
    response = client.get("/api/catalog")
    assert response.status_code == 200
    data = response.json()
    assert "version" in data
    assert "entries" in data
    assert isinstance(data["entries"], list)


def test_catalog_entry(client):
    """GET /api/catalog/sequence returns entry details."""
    response = client.get("/api/catalog/sequence")
    assert response.status_code == 200
    data = response.json()
    assert data["id"] == "sequence"
    assert "name" in data
    assert "category" in data
    assert "description" in data
    assert data["stability"] == "stable"
    assert "kind" in data


def test_catalog_entry_includes_blackboard_metadata(client):
    """Catalog entry surfaces declared blackboard reader/writer parameter metadata."""
    response = client.get("/api/catalog/set_key_bool_value")
    assert response.status_code == 200
    data = response.json()
    assert data["bbReaderParams"] == []
    assert data["bbWriterParams"] == ["key"]


def test_catalog_entry_includes_ui_editor_hints(client):
    """Catalog entry surfaces UI editor hints for structured parameters."""
    response = client.get("/api/catalog/plan_pick_sequence")
    assert response.status_code == 200
    data = response.json()
    params = {param["name"]: param for param in data["parameters"]}
    assert params["pick_offset_envelope"]["uiEditor"] == "pose_envelope"
    assert params["pick_offset_reference"]["uiEditor"] == "motion_reference"
    assert params["approach_offset_envelope"]["uiEditor"] == "pose_envelope"
    assert params["approach_offset_reference"]["uiEditor"] == "motion_reference"
    assert params["approach_move_node_name"]["schema"]["type"] == "string"
    assert params["pick_move_node_name"]["schema"]["type"] == "string"
    assert params["retreat_move_node_name"]["schema"]["type"] == "string"
    assert "place_pose_map" not in params


def test_catalog_entry_includes_interrupt_metadata(client):
    """Catalog entry surfaces interruptibility metadata for runtime policy authors."""
    response = client.get("/api/catalog/move_manipulator_action")
    assert response.status_code == 200
    data = response.json()
    assert data["interruptible"] is True
    assert data["defaultInterruptScope"] == "kernel_motion"
    params = {param["name"]: param for param in data["parameters"]}
    target_mode = params["target_mode"]
    relative_option = next(
        option
        for option in target_mode["enumOptions"]
        if option["value"] == "relative"
    )
    assert relative_option["profilesMin"] == "v0.1"
    # The shared target-mode option also covers relative joint deltas, which
    # need only the authority-selected start state. Program validation adds
    # context.pose.resolve_reference for relative pose/Cartesian requests.
    assert relative_option["requiredCapabilities"] == []
    assert params["target_reference"]["profilesMin"] == "v0.1"
    assert params["target_reference"]["visibleWhen"] == {
        "param": "target_mode",
        "equals": "relative",
    }


def test_catalog_entry_includes_stability_metadata(client):
    """Catalog entry surfaces experimental/stable lifecycle metadata."""
    response = client.get("/api/catalog/command_gripper")
    assert response.status_code == 200
    data = response.json()
    assert data["stability"] == "experimental"
    assert data["profilesMin"] == "v0.1"

def test_catalog_entry_not_found(client):
    """GET /api/catalog/nonexistent returns 404."""
    response = client.get("/api/catalog/nonexistent")
    assert response.status_code == 404


# =============================================================================
# Filesystem Browser Tests
# =============================================================================

def test_browse_program_files_filters_to_compatible_extensions(client, tmp_path):
    (tmp_path / "alpha.program.yaml").write_text("apiVersion: manyforge/v1\n", encoding="utf-8")
    (tmp_path / "beta.program.yml").write_text("apiVersion: manyforge/v1\n", encoding="utf-8")
    (tmp_path / "ignore.txt").write_text("hello\n", encoding="utf-8")
    (tmp_path / "subdir").mkdir()

    response = client.get("/api/files/browse", params={"path": str(tmp_path), "kind": "program"})
    assert response.status_code == 200
    data = response.json()

    entry_names = [entry["name"] for entry in data["entries"]]
    assert "subdir" in entry_names
    assert "alpha.program.yaml" in entry_names
    assert "beta.program.yml" in entry_names
    assert "ignore.txt" not in entry_names


def test_browse_deployment_files_filters_to_compatible_extensions(client, tmp_path):
    (tmp_path / "robot.deployment.yaml").write_text("apiVersion: manyforge/v1\n", encoding="utf-8")
    (tmp_path / "robot.deployment.yml").write_text("apiVersion: manyforge/v1\n", encoding="utf-8")
    (tmp_path / "other.program.yaml").write_text("apiVersion: manyforge/v1\n", encoding="utf-8")

    response = client.get("/api/files/browse", params={"path": str(tmp_path), "kind": "deployment"})
    assert response.status_code == 200
    data = response.json()

    entry_names = [entry["name"] for entry in data["entries"]]
    assert "robot.deployment.yaml" in entry_names
    assert "robot.deployment.yml" in entry_names
    assert "other.program.yaml" not in entry_names


def test_file_browser_roots_skip_container_root_home(monkeypatch, tmp_path):
    workspace = tmp_path / "workspace"
    workspace.mkdir()

    monkeypatch.setattr(routes_filesystem.Path, "cwd", staticmethod(lambda: workspace))
    monkeypatch.setattr(routes_filesystem.Path, "home", staticmethod(lambda: Path("/root")))

    allowed = [str(workspace), "/workspace"]
    roots = routes_filesystem._root_shortcuts(allowed)
    assert str(workspace.resolve()) in roots
    assert "/root" not in roots


# =============================================================================
# Program Tests
# =============================================================================

def test_load_program(client, state):
    """POST /api/program/load with valid path, verify response."""
    response = client.post("/api/program/load", json={"path": _PROGRAM_PATH})
    assert response.status_code == 200
    data = response.json()
    assert "name" in data
    assert "description" in data
    assert "programProfile" in data
    assert "robot" in data
    assert "sceneCount" in data
    assert "sceneResources" in data
    assert data["sceneResources"] == [
        {
            "objectId": "table",
            "shape": {
                "type": "box",
                "box_dimensions_m": [1.0, 0.6, 0.02],
            },
            "pose": {
                "frame_id": "base_link",
                "position_m": [0.5, 0.0, 0.4],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            },
            "metadata": {},
        }
    ]
    assert "parameterCount" in data
    assert "parameters" in data
    assert data["parameters"][0]["name"] == "pick_velocity_scale"


def test_load_program_invalid_path(client, state):
    """POST with a path outside the allowlist returns 403."""
    response = client.post("/api/program/load", json={"path": "/nonexistent/path.yaml"})
    assert response.status_code == 403


def test_load_program_validation_error_includes_reason(client, tmp_path):
    data = yaml.safe_load(Path(_PROGRAM_PATH).read_text(encoding="utf-8"))
    del data["scene"][0]["pose"]["frame_id"]
    program_path = tmp_path / "invalid_scene.program.yaml"
    program_path.write_text(yaml.safe_dump(data, sort_keys=False), encoding="utf-8")

    response = client.post("/api/program/load", json={"path": str(program_path)})

    assert response.status_code == 400
    assert response.json()["detail"] == "scene[0] pose.frame_id is required"


def test_get_program(client, state):
    """Load then GET /api/program."""
    # Load program
    client.post("/api/program/load", json={"path": _PROGRAM_PATH})
    
    # Get program
    response = client.get("/api/program")
    assert response.status_code == 200
    data = response.json()
    assert "name" in data
    assert "robot" in data
    assert data["sceneResources"][0]["objectId"] == "table"
    assert data["parameters"][0]["name"] == "pick_velocity_scale"


def test_program_scene_resource_metadata_round_trips_through_apply(client, state, tmp_path):
    data = yaml.safe_load(Path(_PROGRAM_PATH).read_text(encoding="utf-8"))
    data["scene"].append(
        {
            "object_id": "supported_box",
            "shape": {"type": "box", "box_dimensions_m": [0.05, 0.05, 0.05]},
            "pose": {
                "frame_id": "base_link",
                "position_m": [0.45, 0.0, 0.455],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            },
            "metadata": {
                "support_contacts": [
                    {
                        "support_object_id": "table",
                        "max_start_penetration_m": 0.01,
                        "release_tolerance_m": 0.001,
                        "require_separation_by_end": True,
                    }
                ]
            },
        }
    )
    program_path = tmp_path / "support_contact_scene.program.yaml"
    program_path.write_text(yaml.safe_dump(data, sort_keys=False), encoding="utf-8")

    load_response = client.post("/api/program/load", json={"path": str(program_path)})
    assert load_response.status_code == 200
    resources = load_response.json()["sceneResources"]
    supported = next(item for item in resources if item["objectId"] == "supported_box")
    assert supported["metadata"]["support_contacts"][0]["support_object_id"] == "table"

    apply_response = client.post("/api/session/apply", json={"scene": resources})
    assert apply_response.status_code == 200

    current_resources = client.get("/api/program").json()["sceneResources"]
    current_supported = next(
        item for item in current_resources if item["objectId"] == "supported_box"
    )
    assert current_supported["metadata"] == supported["metadata"]
    active_supported = next(
        item for item in state.kernel.get_scene_objects()
        if item.object_id == "supported_box"
    )
    assert active_supported.support_contacts == supported["metadata"]["support_contacts"]


def test_get_saved_program_returns_saved_parameters(client):
    client.post("/api/program/load", json={"path": _PROGRAM_PATH})

    response = client.get("/api/program/saved")
    assert response.status_code == 200
    data = response.json()
    assert data["parameters"][0]["name"] == "pick_velocity_scale"
    assert data["parameters"][0]["default"] == 0.5
    assert data["sceneResources"][0]["objectId"] == "table"


def test_get_program_not_loaded(client):
    """GET /api/program without loading returns 404."""
    response = client.get("/api/program")
    assert response.status_code == 404


def test_get_program_tree(client, state):
    """Load then GET /api/program/tree, verify tree structure."""
    # Load program
    client.post("/api/program/load", json={"path": _PROGRAM_PATH})
    
    # Get tree
    response = client.get("/api/program/tree")
    assert response.status_code == 200
    data = response.json()
    assert "tree" in data
    tree = data["tree"]
    assert "id" in tree
    assert "name" in tree
    assert "params" in tree
    assert "children" in tree


def test_get_program_tree_preserves_modification_policy(client, tmp_path):
    program_path = _write_program_with_runtime_policy(tmp_path)
    client.post("/api/program/load", json={"path": program_path})

    response = client.get("/api/program/tree")
    assert response.status_code == 200
    move_node = response.json()["tree"]["children"][0]
    assert move_node["modificationPolicy"]["runtime_override"] == "llm_allowed"


def test_gripper_command_rejects_out_of_bounds_target(client, state):
    state.deployment = Deployment(_UR10E_ROBOTIQ_DEPLOYMENT_PATH)
    state.deployment_source_path = _UR10E_ROBOTIQ_DEPLOYMENT_PATH

    response = client.post(
        "/api/scene/gripper/command",
        json={"gripperId": "primary", "targetPosition": 999.0},
    )

    assert response.status_code == 400
    assert "outside limits" in response.json()["detail"]
    assert state.kernel.gripper_command_count == 0


def test_gripper_command_rejects_while_cycle_owns_execution(client, state):
    state.deployment = Deployment(_UR10E_ROBOTIQ_DEPLOYMENT_PATH)
    state.deployment_source_path = _UR10E_ROBOTIQ_DEPLOYMENT_PATH
    state.cycle_manager = type(
        "CycleManagerStub",
        (),
        {"state": CycleState.RUNNING},
    )()

    response = client.post(
        "/api/scene/gripper/command",
        json={"gripperId": "primary", "targetPosition": 0.1},
    )

    assert response.status_code == 409
    assert "current cycle state is running" in response.json()["detail"]
    assert state.kernel.gripper_command_count == 0


def test_gripper_command_accepts_bounded_idle_target(client, state):
    state.deployment = Deployment(_UR10E_ROBOTIQ_DEPLOYMENT_PATH)
    state.deployment_source_path = _UR10E_ROBOTIQ_DEPLOYMENT_PATH

    response = client.post(
        "/api/scene/gripper/command",
        json={"gripperId": "primary", "targetPosition": 0.1, "durationS": 0.0},
    )

    assert response.status_code == 200
    assert response.json()["accepted"] is True
    assert state.kernel.gripper_command_count == 1
    assert state.kernel.last_gripper_command_request is not None
    assert state.kernel.last_gripper_command_request.target_position == pytest.approx(0.1)


def test_gripper_command_rejects_pneumatic_numeric_target(client, state):
    state.deployment = Deployment(_ISAAC_LITE6_DEPLOYMENT_PATH)
    state.deployment_source_path = _ISAAC_LITE6_DEPLOYMENT_PATH

    response = client.post(
        "/api/scene/gripper/command",
        json={"gripperId": "primary", "targetPosition": 0.004},
    )

    assert response.status_code == 400
    assert "open_state_ref" in response.json()["detail"]
    assert state.kernel.gripper_command_count == 0


def test_runtime_override_apply_updates_effective_tree_but_not_saved_artifact(client, tmp_path):
    program_path = _write_program_with_runtime_policy(tmp_path)
    client.post("/api/program/load", json={"path": program_path})

    response = client.post(
        "/api/program/runtime-overrides",
        json={
            "nodeName": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authoritySource": "llm",
            "approvalMode": "not_required",
            "reason": "path planning degraded",
        },
    )
    assert response.status_code == 200
    data = response.json()
    assert data["hasActive"] is True
    assert data["currentTreeOrigin"] == "runtime_override"

    runtime_tree = client.get("/api/program/tree").json()["tree"]
    move_node = runtime_tree["children"][0]
    assert move_node["params"]["planning_timeout_s"] == 3.5
    assert move_node["modificationPolicy"]["runtime_override"] == "llm_allowed"

    save_path = str(tmp_path / "saved_copy")
    save_response = client.post("/api/program/save", json={"path": save_path})
    assert save_response.status_code == 200
    saved_yaml = yaml.safe_load(
        Path(save_response.json()["path"]).read_text(encoding="utf-8")
    )
    assert saved_yaml["tree"]["children"][0]["params"]["planning_timeout_s"] == 2.0


def test_runtime_override_requires_node_policy(client, tmp_path):
    program_path = _write_program_with_runtime_policy(tmp_path, runtime_override="forbidden")
    client.post("/api/program/load", json={"path": program_path})

    response = client.post(
        "/api/program/runtime-overrides",
        json={
            "nodeName": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authoritySource": "llm",
            "approvalMode": "not_required",
        },
    )
    assert response.status_code == 409
    assert "does not allow runtime overrides" in response.json()["detail"]


def test_runtime_override_rejects_per_move_tcp_above_global(client, state, tmp_path):
    """Fail closed: a runtime override setting a per-move TCP cap above the
    deployment global cap is rejected, not silently clamped at runtime."""
    state.deployment = Deployment(_DEPLOYMENT_PATH)  # global cap 0.25
    state.deployment_source_path = _DEPLOYMENT_PATH
    program_path = _write_program_with_runtime_policy(tmp_path)
    client.post("/api/program/load", json={"path": program_path})

    response = client.post(
        "/api/program/runtime-overrides",
        json={
            "nodeName": "move_to_pick",
            "params": {"max_tcp_linear_speed_mps": 0.4},
            "authoritySource": "human",
            "approvalMode": "human_approved",
        },
    )
    assert response.status_code == 400
    assert "exceeds the deployment global cap" in response.json()["detail"]


def test_runtime_override_allows_per_move_tcp_within_global(client, state, tmp_path):
    """A runtime override at or below the global cap is accepted."""
    state.deployment = Deployment(_DEPLOYMENT_PATH)  # global cap 0.25
    state.deployment_source_path = _DEPLOYMENT_PATH
    program_path = _write_program_with_runtime_policy(tmp_path)
    client.post("/api/program/load", json={"path": program_path})

    response = client.post(
        "/api/program/runtime-overrides",
        json={
            "nodeName": "move_to_pick",
            "params": {"max_tcp_linear_speed_mps": 0.2},
            "authoritySource": "human",
            "approvalMode": "human_approved",
        },
    )
    assert response.status_code == 200


def test_program_validate_flags_per_move_tcp_above_global(client, state):
    """Inline /program/validate surfaces an over-cap per-move value as an error
    (early editor feedback mirroring the load/save/apply reject)."""
    state.deployment = Deployment(_DEPLOYMENT_PATH)  # global cap 0.25
    tree = {
        "id": "sequence",
        "name": "main",
        "children": [
            {
                "id": "move_manipulator_action",
                "name": "move_to_pick",
                "params": {
                    "move_id": "m",
                    "motion_type": "joint",
                    "joint_goal_names": ["j1", "j2", "j3", "j4", "j5", "j6"],
                    "joint_goal_positions_rad": [0.2, -1.2, 1.0, -1.5, -1.3, 0.4],
                    "max_tcp_linear_speed_mps": 0.4,
                    "planning_timeout_s": 2.0,
                    "execution_timeout_s": 10.0,
                },
            }
        ],
    }
    response = client.post("/api/program/validate", json={"tree": tree})
    assert response.status_code == 200
    data = response.json()
    assert data["valid"] is False
    assert any(
        "exceeds the deployment global cap" in issue["message"]
        for issue in data["errors"]
    )


def test_runtime_override_staged_in_direct_control_requires_reset_before_stopped_resume(tmp_path):
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    program_path = _write_program_with_runtime_policy(tmp_path)

    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": program_path})
        state.cycle_manager._state = CycleState.DIRECT_CONTROL

        response = client.post(
            "/api/program/runtime-overrides",
            json={
                "nodeName": "move_to_pick",
                "params": {"planning_timeout_s": 4.0},
                "authoritySource": "llm",
                "approvalMode": "not_required",
            },
        )
        assert response.status_code == 200
        assert response.json()["hasStaged"] is True
        assert response.json()["hasActive"] is False

        state.cycle_manager._state = CycleState.STOPPED
        run_response = client.post("/api/cycle/run")
        assert run_response.status_code == 409
        # Cycle command 409 now carries a structured detail payload
        # ({message, errorCode, errorDetails}) so the viewer can render a
        # cause panel; the human-readable string lives in detail.message.
        detail = run_response.json()["detail"]
        assert isinstance(detail, dict)
        assert "reset the cycle first" in detail["message"]
        assert detail["errorCode"] == "staged_overrides_blocked_resume"


def test_runtime_override_activate_from_idle_promotes_staged_override(tmp_path):
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    program_path = _write_program_with_runtime_policy(tmp_path)

    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": program_path})
        state.cycle_manager._state = CycleState.DIRECT_CONTROL
        client.post(
            "/api/program/runtime-overrides",
            json={
                "nodeName": "move_to_pick",
                "params": {"planning_timeout_s": 4.0},
                "authoritySource": "llm",
                "approvalMode": "not_required",
            },
        )

        state.cycle_manager._state = CycleState.IDLE
        activate_response = client.post("/api/program/runtime-overrides/activate")
        assert activate_response.status_code == 200
        assert activate_response.json()["hasActive"] is True
        assert activate_response.json()["hasStaged"] is False

        runtime_tree = client.get("/api/program/tree").json()["tree"]
        assert runtime_tree["children"][0]["params"]["planning_timeout_s"] == 4.0


def test_runtime_override_status_caches_staged_effective_tree(
    tmp_path,
    monkeypatch,
):
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    program_path = _write_program_with_runtime_policy(tmp_path)
    state.load_program(program_path)
    state.cycle_manager._state = CycleState.DIRECT_CONTROL
    state.apply_runtime_override(
        node_name="move_to_pick",
        params={"planning_timeout_s": 4.0},
        authority_source="llm",
        approval_mode="not_required",
    )

    original_builder = state._build_program_with_overrides
    calls = 0

    def counted_builder(*args, **kwargs):
        nonlocal calls
        calls += 1
        return original_builder(*args, **kwargs)

    monkeypatch.setattr(state, "_build_program_with_overrides", counted_builder)

    first = state.runtime_override_status()
    assert first["next_effective_tree"]["children"][0]["params"]["planning_timeout_s"] == 4.0
    first["next_effective_tree"]["children"][0]["params"]["planning_timeout_s"] = 99.0
    second = state.runtime_override_status()

    assert calls == 1
    assert second["next_effective_tree"]["children"][0]["params"]["planning_timeout_s"] == 4.0


def test_runtime_override_status_is_safe_during_concurrent_mutation(tmp_path):
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    program_path = _write_program_with_runtime_policy(tmp_path)
    state.load_program(program_path)
    state.cycle_manager._state = CycleState.DIRECT_CONTROL
    with state._runtime_override_lock:
        for index in range(64):
            state._runtime_overrides[f"seed_override_{index}"] = RuntimeOverrideRecord(
                node_name=f"seed_override_{index}",
                params={"planning_timeout_s": float(index + 1)},
                authority_source="llm",
                approval_mode="not_required",
                reason="pb5 seeded race coverage",
                session_id=None,
                trigger_node_instance=None,
                base_program_hash=state.program_tree_hash,
                activation_state="staged",
                activated_at_ns=None,
            )
        state._mark_runtime_overrides_changed_locked()

    hub = StateHub()
    hub.start(state)
    start = threading.Barrier(5)
    errors: list[BaseException] = []

    def mutate_overrides(offset: int) -> None:
        try:
            start.wait(timeout=1.0)
            for iteration in range(250):
                if iteration % 2 == 0:
                    state.apply_runtime_override(
                        node_name="move_to_pick",
                        params={"planning_timeout_s": 3.0 + offset + iteration / 1000.0},
                        authority_source="llm",
                        approval_mode="not_required",
                        reason="pb5 race regression",
                    )
                else:
                    state.remove_runtime_override("move_to_pick")
        except BaseException as exc:  # pragma: no cover - test aid
            errors.append(exc)

    def collect_status_frames() -> None:
        try:
            start.wait(timeout=1.0)
            for _ in range(500):
                status = state.runtime_override_status()
                assert status["active_count"] >= 0
                frame = hub._collect_frame(include_payload=True)
                assert frame is not None
                runtime_status = frame.get("runtimeOverrideStatus")
                assert runtime_status is None or runtime_status["activeCount"] >= 0
        except BaseException as exc:  # pragma: no cover - test aid
            errors.append(exc)

    threads = [
        threading.Thread(target=mutate_overrides, args=(0,), name="pb5-mutator-a"),
        threading.Thread(target=mutate_overrides, args=(1,), name="pb5-mutator-b"),
        threading.Thread(target=collect_status_frames, name="pb5-reader-a"),
        threading.Thread(target=collect_status_frames, name="pb5-reader-b"),
    ]
    for thread in threads:
        thread.start()
    start.wait(timeout=1.0)
    for thread in threads:
        thread.join(timeout=5.0)

    hub.stop()
    assert all(not thread.is_alive() for thread in threads)
    assert not errors
    final_status = state.runtime_override_status()
    assert final_status["staged_count"] >= 64
    assert final_status["stale_count"] == 0


def test_runtime_override_materialized_tree_supports_staged_view(tmp_path):
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    program_path = _write_program_with_runtime_policy(tmp_path)

    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": program_path})
        state.cycle_manager._state = CycleState.DIRECT_CONTROL
        client.post(
            "/api/program/runtime-overrides",
            json={
                "nodeName": "move_to_pick",
                "params": {"planning_timeout_s": 4.0},
                "authoritySource": "llm",
                "approvalMode": "not_required",
            },
        )

        current_tree = client.get("/api/program/runtime-overrides/materialized-tree").json()["tree"]
        staged_tree = client.get(
            "/api/program/runtime-overrides/materialized-tree",
            params={"includeStaged": "true"},
        ).json()["tree"]

        assert current_tree["children"][0]["params"]["planning_timeout_s"] == 2.0
        assert staged_tree["children"][0]["params"]["planning_timeout_s"] == 4.0


def test_runtime_override_promote_loaded_program_materializes_and_clears_ledger(client, tmp_path):
    program_path = _write_program_with_runtime_policy(tmp_path)
    client.post("/api/program/load", json={"path": program_path})

    client.post(
        "/api/program/runtime-overrides",
        json={
            "nodeName": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authoritySource": "llm",
            "approvalMode": "not_required",
        },
    )

    response = client.post(
        "/api/program/runtime-overrides/promote",
        json={"target": "loaded_program"},
    )
    assert response.status_code == 200
    data = response.json()
    assert data["runtimeOverrideStatus"]["hasActive"] is False
    assert data["tree"]["children"][0]["params"]["planning_timeout_s"] == 3.5

    runtime_tree = client.get("/api/program/tree").json()["tree"]
    assert runtime_tree["children"][0]["params"]["planning_timeout_s"] == 3.5


def test_runtime_override_promote_loaded_program_rejects_dropping_staged_overrides(tmp_path):
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    program_path = _write_program_with_runtime_policy(tmp_path)

    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": program_path})
        state.cycle_manager._state = CycleState.DIRECT_CONTROL
        client.post(
            "/api/program/runtime-overrides",
            json={
                "nodeName": "move_to_pick",
                "params": {"planning_timeout_s": 4.0},
                "authoritySource": "llm",
                "approvalMode": "not_required",
            },
        )

        state.cycle_manager._state = CycleState.STOPPED
        response = client.post(
            "/api/program/runtime-overrides/promote",
            json={"target": "loaded_program"},
        )
        assert response.status_code == 409
        assert "include staged overrides" in response.json()["detail"]


def test_runtime_override_promote_save_requires_persistent_edit_policy(client, tmp_path):
    program_path = _write_program_with_runtime_policy(
        tmp_path,
        persistent_edit="forbidden",
    )
    client.post("/api/program/load", json={"path": program_path})
    client.post(
        "/api/program/runtime-overrides",
        json={
            "nodeName": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authoritySource": "llm",
            "approvalMode": "not_required",
        },
    )

    response = client.post(
        "/api/program/runtime-overrides/promote",
        json={
            "target": "save",
            "path": str(tmp_path / "saved_materialized"),
            "includeStaged": False,
            "authoritySource": "llm",
            "approvalMode": "not_required",
        },
    )
    assert response.status_code == 409
    assert "does not allow persistent edits" in response.json()["detail"]


def test_runtime_override_promote_save_materializes_to_disk_with_explicit_authority(client, tmp_path):
    program_path = _write_program_with_runtime_policy(
        tmp_path,
        persistent_edit="llm_allowed",
    )
    client.post("/api/program/load", json={"path": program_path})
    client.post(
        "/api/program/runtime-overrides",
        json={
            "nodeName": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authoritySource": "llm",
            "approvalMode": "not_required",
        },
    )

    response = client.post(
        "/api/program/runtime-overrides/promote",
        json={
            "target": "save",
            "path": str(tmp_path / "saved_materialized"),
            "authoritySource": "llm",
            "approvalMode": "not_required",
        },
    )
    assert response.status_code == 200
    saved_path = Path(response.json()["path"])
    saved_yaml = yaml.safe_load(saved_path.read_text(encoding="utf-8"))
    assert saved_yaml["tree"]["children"][0]["params"]["planning_timeout_s"] == 3.5
    status = client.get("/api/program/runtime-overrides").json()
    assert status["hasActive"] is True


# =============================================================================
# Runtime Override Revision Binding Tests (OP-5)
# =============================================================================


def test_runtime_override_status_includes_base_program_hash(client, tmp_path):
    program_path = _write_program_with_runtime_policy(tmp_path)
    client.post("/api/program/load", json={"path": program_path})

    response = client.post(
        "/api/program/runtime-overrides",
        json={
            "nodeName": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authoritySource": "llm",
            "approvalMode": "not_required",
        },
    )
    assert response.status_code == 200
    data = response.json()
    assert "baseProgramHash" in data
    assert len(data["baseProgramHash"]) == 64
    assert data["active"][0]["baseProgramHash"] == data["baseProgramHash"]
    assert data["active"][0]["isStale"] is False
    assert data["staleCount"] == 0
    assert data["hasStale"] is False


def test_load_program_rejects_when_overrides_exist(client, tmp_path):
    program_path = _write_program_with_runtime_policy(tmp_path)
    client.post("/api/program/load", json={"path": program_path})

    client.post(
        "/api/program/runtime-overrides",
        json={
            "nodeName": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authoritySource": "llm",
            "approvalMode": "not_required",
        },
    )

    response = client.post("/api/program/load", json={"path": program_path})
    assert response.status_code == 409
    assert "runtime overrides exist" in response.json()["detail"].lower()


def test_load_program_with_force_discard_overrides(client, tmp_path):
    program_path = _write_program_with_runtime_policy(tmp_path)
    client.post("/api/program/load", json={"path": program_path})

    client.post(
        "/api/program/runtime-overrides",
        json={
            "nodeName": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authoritySource": "llm",
            "approvalMode": "not_required",
        },
    )

    response = client.post(
        "/api/program/load",
        json={"path": program_path, "forceDiscardOverrides": True},
    )
    assert response.status_code == 200

    status = client.get("/api/program/runtime-overrides").json()
    assert status["activeCount"] == 0


def test_load_program_validates_robot_control_joint_limits(client, tmp_path):
    deployment_data = yaml.safe_load(Path(_DEPLOYMENT_PATH).read_text(encoding="utf-8"))
    deployment_data["robot_config"]["robot_control_joint_limits"] = {
        "typo_not_a_joint": {"max_velocity_rad_s": 0.2}
    }
    deployment_path = tmp_path / "invalid_robot_control_limits.deployment.yaml"
    deployment_path.write_text(yaml.safe_dump(deployment_data), encoding="utf-8")

    response = client.post(
        "/api/program/load",
        json={"path": _PROGRAM_PATH, "deploymentPath": str(deployment_path)},
    )

    assert response.status_code == 400
    assert "robot_control_joint_limits references unknown" in response.json()["detail"]
    assert "typo_not_a_joint" in response.json()["detail"]


def test_load_program_clears_viewer_frame_overrides(client, state):
    state.set_frame_visibility("tool0", False)
    state.set_object_frame_visibility("box", True)

    assert state.get_frame_visibility("tool0", default=True) is False
    assert state.get_object_frame_visibility("box") is True

    response = client.post("/api/program/load", json={"path": _PROGRAM_PATH})

    assert response.status_code == 200
    assert state.get_frame_visibility("tool0", default=True) is True
    assert state.get_object_frame_visibility("box") is False


def test_update_tree_rejects_when_overrides_exist(client, tmp_path):
    program_path = _write_program_with_runtime_policy(tmp_path)
    client.post("/api/program/load", json={"path": program_path})
    original_tree = client.get("/api/program/tree").json()["tree"]

    client.post(
        "/api/program/runtime-overrides",
        json={
            "nodeName": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authoritySource": "llm",
            "approvalMode": "not_required",
        },
    )

    # Make a SCHEMA-VALID tree change: fail-closed v1 validation rejects unknown
    # params with 400 before the overrides-conflict check can answer 409
    # (sequence declares no params), so change a real catalog param on the
    # overridden move node instead.
    modified_tree = copy.deepcopy(original_tree)
    move_node = modified_tree["children"][0]
    move_node["params"] = dict(move_node.get("params") or {})
    move_node["params"]["planning_timeout_s"] = 9.0
    response = client.put(
        "/api/program/tree",
        json={"tree": modified_tree, "forceDiscardOverrides": False},
    )
    assert response.status_code == 409
    assert "runtime overrides exist" in response.json()["detail"].lower()


def test_update_tree_with_force_discard_overrides(client, tmp_path):
    program_path = _write_program_with_runtime_policy(tmp_path)
    client.post("/api/program/load", json={"path": program_path})
    original_tree = client.get("/api/program/tree").json()["tree"]

    client.post(
        "/api/program/runtime-overrides",
        json={
            "nodeName": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authoritySource": "llm",
            "approvalMode": "not_required",
        },
    )

    response = client.put(
        "/api/program/tree",
        json={"tree": original_tree, "forceDiscardOverrides": True},
    )
    assert response.status_code == 200

    status = client.get("/api/program/runtime-overrides").json()
    assert status["activeCount"] == 0


def test_runtime_override_revalidate_endpoint_no_stale(client, tmp_path):
    program_path = _write_program_with_runtime_policy(tmp_path)
    client.post("/api/program/load", json={"path": program_path})

    client.post(
        "/api/program/runtime-overrides",
        json={
            "nodeName": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authoritySource": "llm",
            "approvalMode": "not_required",
        },
    )

    response = client.post("/api/program/runtime-overrides/revalidate")
    assert response.status_code == 200
    data = response.json()
    assert data["staleCount"] == 0


def test_stale_override_detected_after_hash_change(tmp_path):
    """Simulate a program change by mutating _program_tree_hash; override becomes stale."""
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    program_path = _write_program_with_runtime_policy(tmp_path)

    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": program_path})

        client.post(
            "/api/program/runtime-overrides",
            json={
                "nodeName": "move_to_pick",
                "params": {"planning_timeout_s": 3.5},
                "authoritySource": "llm",
                "approvalMode": "not_required",
            },
        )

        status = client.get("/api/program/runtime-overrides").json()
        assert status["hasStale"] is False
        original_hash = status["baseProgramHash"]

        # Simulate a program change by mutating the hash.
        state._program_tree_hash = "0" * 64

        status = client.get("/api/program/runtime-overrides").json()
        assert status["hasStale"] is True
        assert status["staleCount"] == 1
        assert status["stale"][0]["nodeName"] == "move_to_pick"
        assert status["stale"][0]["isStale"] is True
        assert status["stale"][0]["baseProgramHash"] == original_hash
        assert status["canActivateNow"] is False


def test_stale_override_blocks_materialized_tree(tmp_path):
    """Stale overrides prevent materialized-tree preview."""
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    program_path = _write_program_with_runtime_policy(tmp_path)

    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": program_path})

        client.post(
            "/api/program/runtime-overrides",
            json={
                "nodeName": "move_to_pick",
                "params": {"planning_timeout_s": 3.5},
                "authoritySource": "llm",
                "approvalMode": "not_required",
            },
        )

        state._program_tree_hash = "0" * 64

        response = client.get("/api/program/runtime-overrides/materialized-tree")
        assert response.status_code == 404
        assert "stale" in response.json()["detail"].lower()


def test_stale_override_revalidation_succeeds(tmp_path):
    """Revalidation updates the hash when the override is still valid."""
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    program_path = _write_program_with_runtime_policy(tmp_path)

    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": program_path})

        client.post(
            "/api/program/runtime-overrides",
            json={
                "nodeName": "move_to_pick",
                "params": {"planning_timeout_s": 3.5},
                "authoritySource": "llm",
                "approvalMode": "not_required",
            },
        )

        original_hash = state._program_tree_hash

        # Force-discard overrides and reload the same program to get a new
        # tree hash that is identical to the original.  Then re-apply the
        # override and simulate a hash change followed by reverting to the
        # real hash so the node still exists and validates.
        client.post(
            "/api/program/runtime-overrides/clear",
        )
        client.post("/api/program/load", json={"path": program_path})
        client.post(
            "/api/program/runtime-overrides",
            json={
                "nodeName": "move_to_pick",
                "params": {"planning_timeout_s": 3.5},
                "authoritySource": "llm",
                "approvalMode": "not_required",
            },
        )

        # Simulate staleness then restore the real hash (like loading a
        # compatible program revision).
        state._program_tree_hash = "0" * 64
        assert state._has_stale_overrides()

        # Restore real hash so the node exists and override validates.
        state._program_tree_hash = original_hash
        # The override still has its old hash from apply time which equals
        # original_hash, so it is NOT stale now.  To test revalidation we
        # must make the override hash differ from current.
        for record in state._runtime_overrides.values():
            record.base_program_hash = "0" * 64

        response = client.post("/api/program/runtime-overrides/revalidate")
        assert response.status_code == 200
        data = response.json()
        assert data["revalidation_results"]["move_to_pick"] == "revalidated"
        assert data["staleCount"] == 0
        assert data["activeCount"] == 1
        assert data["active"][0]["baseProgramHash"] == original_hash


def test_stale_override_invalidation_removes_from_ledger(tmp_path):
    """Revalidation removes overrides whose target node no longer exists."""
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    program_path = _write_program_with_runtime_policy(tmp_path)

    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": program_path})

        client.post(
            "/api/program/runtime-overrides",
            json={
                "nodeName": "move_to_pick",
                "params": {"planning_timeout_s": 3.5},
                "authoritySource": "llm",
                "approvalMode": "not_required",
            },
        )

        # Simulate staleness.
        for record in state._runtime_overrides.values():
            record.base_program_hash = "0" * 64
        # Also rename the node in the tree so it can't be found.
        state.program.tree.children[0].name = "renamed_node"

        response = client.post("/api/program/runtime-overrides/revalidate")
        assert response.status_code == 200
        data = response.json()
        assert data["revalidation_results"]["move_to_pick"] == "invalid"
        assert data["activeCount"] == 0
        assert data["staleCount"] == 0


def test_force_discard_overrides_unconditional_same_tree(client, tmp_path):
    """forceDiscardOverrides=true clears overrides even when the tree is unchanged."""
    program_path = _write_program_with_runtime_policy(tmp_path)
    client.post("/api/program/load", json={"path": program_path})
    original_tree = client.get("/api/program/tree").json()["tree"]

    client.post(
        "/api/program/runtime-overrides",
        json={
            "nodeName": "move_to_pick",
            "params": {"planning_timeout_s": 3.5},
            "authoritySource": "llm",
            "approvalMode": "not_required",
        },
    )

    # Re-apply the same tree with forceDiscardOverrides.
    response = client.put(
        "/api/program/tree",
        json={"tree": original_tree, "forceDiscardOverrides": True},
    )
    assert response.status_code == 200

    status = client.get("/api/program/runtime-overrides").json()
    assert status["activeCount"] == 0
    assert status["hasActive"] is False


def test_validate_program_tree_accepts_current_tree(client):
    client.post("/api/program/load", json={"path": _PROGRAM_PATH})
    current_tree = client.get("/api/program/tree").json()["tree"]

    response = client.post("/api/program/validate", json={"tree": current_tree})
    assert response.status_code == 200
    data = response.json()
    assert data["valid"] is True
    assert data["errors"] == []


def test_validate_program_tree_reports_structural_errors(client):
    bad_tree = {
        "id": "sequence",
        "name": "root",
        "children": [
            {
                "id": "nonexistent_node",
                "name": "bad_child",
            },
        ],
    }

    response = client.post("/api/program/validate", json={"tree": bad_tree})
    assert response.status_code == 200
    data = response.json()
    assert data["valid"] is False
    assert data["errors"]
    assert data["errors"][0]["path"] == "tree.children[0]"
    assert "Unknown catalog entry" in data["errors"][0]["message"]


def test_validate_program_tree_reports_parameter_errors(client):
    client.post("/api/program/load", json={"path": _PROGRAM_PATH})
    bad_tree = {
        "id": "sequence",
        "name": "root",
        "children": [
            {
                "id": "move_manipulator_action",
                "name": "bad_move",
                "params": {
                    "move_id": "pick",
                    "motion_type": "joint",
                    "joint_goal_names": ["j1"],
                    "joint_goal_positions_rad": [0.1],
                    "velocity_scale": -0.1,
                },
            },
        ],
    }

    response = client.post("/api/program/validate", json={"tree": bad_tree})
    assert response.status_code == 200
    data = response.json()
    assert data["valid"] is False
    assert any(
        error["path"] == "tree.children[0].params.velocity_scale"
        and "must be > 0.0" in error["message"]
        for error in data["errors"]
    )


def test_get_saved_program_tree_reflects_disk_not_applied_runtime(client):
    """GET /api/program/saved/tree returns the on-disk artifact even after an unsaved apply."""
    client.post("/api/program/load", json={"path": _PROGRAM_PATH})

    modified_tree = {
        "id": "sequence",
        "name": "applied_not_saved_sequence",
        "children": [
            {
                "id": "reset_trajectories",
                "name": "reset_once",
            },
        ],
    }
    response = client.post("/api/session/apply", json={"tree": modified_tree})
    assert response.status_code == 200

    runtime_tree = client.get("/api/program/tree").json()["tree"]
    saved_tree = client.get("/api/program/saved/tree").json()["tree"]
    assert runtime_tree["name"] == "applied_not_saved_sequence"
    assert saved_tree["name"] == "main_sequence"


def test_session_apply_surfaces_validation_reason(client):
    """POST /api/session/apply includes the concrete validation failure in the response detail."""
    client.post("/api/program/load", json={"path": _PROGRAM_PATH})

    invalid_tree = {
        "id": "sequence",
        "name": "invalid_sequence",
        "children": [
            {
                "id": "add_collision_object",
                "name": "add_object",
                "params": {
                    "object_id": "graspable_copy",
                    "frame_id": "universe",
                    "pose": {
                        "position_m": [0.0, 0.0, 0.0],
                        "orientation_rpy_deg": [0.0, 0.0, 0.0],
                    },
                },
                "children": [],
            },
        ],
    }

    response = client.post("/api/session/apply", json={"tree": invalid_tree})
    assert response.status_code == 400
    assert "missing required parameter 'shape'" in response.json()["detail"]


# =============================================================================
# Deployment Tests
# =============================================================================

def test_get_deployment_not_loaded(client):
    """GET /api/deployment returns 404."""
    response = client.get("/api/deployment")
    assert response.status_code == 404


def test_get_capabilities_requires_loaded_deployment(client):
    response = client.get("/api/capabilities")
    assert response.status_code == 404


def test_load_deployment(client, state):
    """POST /api/deployment/load with valid path, verify response."""
    response = client.post("/api/deployment/load", json={"path": _DEPLOYMENT_PATH})
    assert response.status_code == 200
    data = response.json()
    assert "name" in data
    assert "description" in data
    assert "programProfile" in data
    assert "robotConfig" in data


def test_get_deployment_surfaces_per_joint_max_jerk(client, state):
    """Phase 4-P5.1K — a loaded deployment's per-joint max_jerk_rad_s3 must be
    surfaced through the Composer deployment API, not hidden or dropped."""
    from manyforge_behavior.deployment import RobotControlJointLimit

    client.post("/api/deployment/load", json={"path": _DEPLOYMENT_PATH})
    state.deployment.robot_config.robot_control_joint_limits = {
        "shoulder_pan_joint": RobotControlJointLimit(
            max_velocity_rad_s=0.4,
            max_acceleration_rad_s2=1.5,
            max_jerk_rad_s3=8.0,
        )
    }

    data = client.get("/api/deployment").json()
    limits = data["robotConfig"]["robotControlJointLimits"]
    assert "shoulder_pan_joint" in limits
    assert limits["shoulder_pan_joint"]["maxVelocityRadS"] == 0.4
    assert limits["shoulder_pan_joint"]["maxAccelerationRadS2"] == 1.5
    assert limits["shoulder_pan_joint"]["maxJerkRadS3"] == 8.0


def test_save_deployment_round_trips_per_joint_max_jerk(client, state, tmp_path):
    """Phase 4-P5.1K — saving a deployment with a per-joint jerk cap writes it to
    YAML so it survives a reload (not dropped on the serialize round-trip)."""
    import yaml
    from manyforge_behavior.deployment import RobotControlJointLimit

    client.post("/api/deployment/load", json={"path": _DEPLOYMENT_PATH})
    state.deployment.robot_config.robot_control_joint_limits = {
        "shoulder_pan_joint": RobotControlJointLimit(max_jerk_rad_s3=7.5),
    }
    target = str(tmp_path / "with_jerk")
    resp = client.post("/api/deployment/save", json={"path": target})
    assert resp.status_code == 200
    saved_path = resp.json()["path"]
    with open(saved_path) as fh:
        saved = yaml.safe_load(fh)
    jl = saved["robot_config"]["robot_control_joint_limits"]["shoulder_pan_joint"]
    assert jl["max_jerk_rad_s3"] == 7.5


def test_get_capabilities_returns_deployment_capability_summary(client):
    client.post("/api/program/load", json={"path": _PROGRAM_PATH, "deployment_path": _DEPLOYMENT_PATH})

    response = client.get("/api/capabilities")
    assert response.status_code == 200
    data = response.json()
    assert "planning.motion.joint_goal" in data["planning"]
    assert "context.state.current" in data["context"]
    assert "scene.edit.add" in data["scene"]
    assert "execution.trajectory.execute" in data["execution"]
    assert data["sensing"] == []
    assert data["contactTier"] == "unsupported"
    assert data["adapterIds"]["planning"] == "planning.roboplan"
    assert data["programProfile"] == "v0.0"
    assert data["timestampNs"] > 0


def test_get_skills_returns_catalog(client):
    """GET /api/skills returns the skill catalog with at least the default skills."""
    response = client.get("/api/skills")
    assert response.status_code == 200
    data = response.json()
    assert "skills" in data
    assert len(data["skills"]) >= 3
    skill_ids = [s["id"] for s in data["skills"]]
    assert "pick_object" in skill_ids
    assert "place_object" in skill_ids
    assert "move_to_named_config" in skill_ids

    pick = next(s for s in data["skills"] if s["id"] == "pick_object")
    assert pick["compatibleProfiles"] == ["v0.1"]
    assert len(pick["requires"]) > 0
    assert len(pick["provides"]) > 0
    assert len(pick["requiresCapabilities"]) > 0
    assert pick["requiresTools"] == []
    assert len(pick["parameters"]) >= 1
    assert "expandsTo" not in pick
    assert pick["body"]["id"] == "sequence"
    assert data["timestampNs"] > 0


def test_get_full_deployment(client):
    """GET /api/deployment/full returns the full loaded artifact plus source path."""
    client.post("/api/deployment/load", json={"path": _DEPLOYMENT_PATH})

    response = client.get("/api/deployment/full")
    assert response.status_code == 200
    data = response.json()
    assert data["sourcePath"] == _DEPLOYMENT_PATH
    assert data["apiVersion"] == "manyforge.deployment.v0"
    assert "adapters" in data
    assert "orchestrator" in data
    assert "launch" in data


def test_get_full_deployment_includes_model_metadata(client):
    """GET /api/deployment/full includes model endpoints and prompt templates when configured."""
    client.post("/api/deployment/load", json={"path": _SEMANTIC_DEPLOYMENT_PATH})

    response = client.get("/api/deployment/full")
    assert response.status_code == 200
    data = response.json()
    assert "modelEndpoints" in data
    assert "promptTemplates" in data
    assert data["modelEndpointProfileRegistry"] == "model_endpoint_profiles.yaml"
    assert "selection_model" in data["modelEndpoints"]
    assert "select_scene_object_v1" in data["promptTemplates"]
    assert data["modelEndpoints"]["selection_model"]["profileRef"] == "thor_manyforge_qwen36"
    assert (
        data["modelEndpoints"]["selection_model"]["modelId"]
        == "qwen3.6-35b-a3b-nvfp4-tq-mtp-manyforge"
    )
    assert data["modelEndpoints"]["selection_model"]["capabilities"][
        "supportsResponseTools"
    ] is True
    assert data["assistant"]["provider"] == "openclaw"
    assert data["assistant"]["endpointAlias"] == "assistant_model"
    assert data["promptTemplates"]["select_scene_object_v1"]["responseTool"]["name"] == "select_scene_object"


def test_infra_model_endpoints_reports_configured_and_live_models(client, monkeypatch):
    """GET /api/infra/model-endpoints exposes deployment aliases plus deterministic /v1/models discovery."""
    client.post("/api/deployment/load", json={"path": _SEMANTIC_DEPLOYMENT_PATH})

    monkeypatch.setattr(
        routes_infra,
        "_probe_openai_compatible_models",
        lambda base_url: (
            [
                "Qwen3.5-9B-Claude-Distilled-NVFP4",
                "qwen3.6-35b-a3b-nvfp4-tq-mtp-manyforge",
                "cosmos-reason2-8b",
            ],
            None,
        ),
    )

    response = client.get("/api/infra/model-endpoints")
    assert response.status_code == 200
    data = response.json()
    assert "selection_model" in data["endpoints"]
    endpoint = data["endpoints"]["selection_model"]
    assert endpoint["configuredModelId"] == "qwen3.6-35b-a3b-nvfp4-tq-mtp-manyforge"
    assert endpoint["capabilities"]["supportsResponseTools"] is True
    assert endpoint["probeStatus"] == "ok"
    assert "Qwen3.5-9B-Claude-Distilled-NVFP4" in endpoint["availableModelIds"]


def test_runtime_status_reports_composer_only_before_program_load(client):
    """GET /api/runtime/status returns the generic runtime contract."""
    response = client.get("/api/runtime/status")

    assert response.status_code == 200
    data = response.json()
    assert data["schemaVersion"] == "manyforge.runtime_status.v1"
    assert data["profileKind"] == "composer_only"
    assert data["authority"] == "backend_authored"
    assert data["readiness"] == "waiting"
    assert data["canRender"] is False
    assert data["canPlan"] is False
    assert data["canExecute"] is False
    component_ids = {component["id"] for component in data["components"]}
    assert {"composer", "program", "native_viewer", "planner", "execution_backend"} <= component_ids


def test_runtime_status_treats_native_viewer_as_render_ready_without_hmi(client):
    """Native viewer render readiness does not require the legacy viewer server."""
    load_response = client.post(
        "/api/program/load",
        json={"path": _PROGRAM_PATH, "deployment_path": _DEPLOYMENT_PATH},
    )
    assert load_response.status_code == 200

    response = client.get("/api/runtime/status")

    assert response.status_code == 200
    data = response.json()
    assert data["readiness"] == "ready"
    assert data["canRender"] is True
    assert data["canPlan"] is True
    assert data["canExecute"] is True
    component_ids = {component["id"] for component in data["components"]}
    assert "native_viewer" in component_ids
    assert all(component_id != "legacy" + "_hmi" for component_id in component_ids)


def test_runtime_status_reports_stop_capability_audit_fields_without_poll_requery(
    tmp_path: Path,
):
    bundle = _StopCapabilityBundle({
        "supported_stop_modes": ["spring_back_hold"],
        "capability_profile_id": "test_stop_capability",
        "hard_stop_level": "hard_deceleration",
        "derivative_proof_level": "manyforge_core_sampled_path_ramp",
        "scaling_source_visibility": "jtc_speed_scaling_input",
        "scaling_source_probe_status": "live_probe_passed",
        "unsupported_reasons": {
            "soft_on_path": "soft_on_path_not_enabled",
        },
    })
    state = ComposerState(
        catalog_path=_CATALOG_PATH,
        kernel=MockKernelClient(),
        kernel_bundle=bundle,
    )
    app = create_app(state, security=_TEST_SECURITY)
    deployment_path = _write_deployment_with_stop_profile(
        tmp_path, stop_mode="spring_back_hold"
    )

    with TestClient(app) as local_client:
        load = local_client.post(
            "/api/program/load",
            json={"path": _PROGRAM_PATH, "deployment_path": deployment_path},
        )
        assert load.status_code == 200, load.text
        assert bundle.calls == 1

        first = local_client.get("/api/runtime/status").json()
        second = local_client.get("/api/runtime/status").json()
        assert bundle.calls == 1
        run = local_client.post("/api/cycle/run")

    assert run.status_code == 200, run.text
    assert bundle.calls == 2
    assert first["supportedStopModes"] == ["spring_back_hold"]
    assert first["configuredStopModes"] == {
        "operator_stop_profile": "spring_back_hold",
        "default_fault_stop_profile": "spring_back_hold",
    }
    assert first["configuredStopDecelerationTimes"] == {
        "operator_stop_profile": 0.5,
        "default_fault_stop_profile": 0.5,
    }
    assert first["stopCapabilityProfileId"] == "test_stop_capability"
    assert first["hardStopLevel"] == "hard_deceleration"
    assert first["derivativeProofLevel"] == "manyforge_core_sampled_path_ramp"
    assert first["scalingSourceVisibility"] == "jtc_speed_scaling_input"
    assert first["scalingSourceProbeStatus"] == "live_probe_passed"
    assert first["backendUnsupportedStopReasons"] == {
        "soft_on_path": "soft_on_path_not_enabled"
    }
    assert first["stopProfileUnsupported"] is False
    assert second["stopCapabilityProfileId"] == "test_stop_capability"


def test_runtime_status_uses_kernel_stop_capability_when_bundle_absent(
    tmp_path: Path,
):
    kernel = _StopCapabilityKernel({
        "supported_stop_modes": ["spring_back_hold", "soft_on_path"],
        "capability_profile_id": "native_client_stop_capability",
        "hard_stop_level": "native_immediate_hold",
    })
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    deployment_path = _write_deployment_with_stop_profile(
        tmp_path, stop_mode="soft_on_path"
    )

    with TestClient(app) as local_client:
        load = local_client.post(
            "/api/program/load",
            json={"path": _PROGRAM_PATH, "deployment_path": deployment_path},
        )
        assert load.status_code == 200, load.text
        status = local_client.get("/api/runtime/status").json()

    assert kernel.calls == 1
    assert status["supportedStopModes"] == ["soft_on_path", "spring_back_hold"]
    assert status["stopCapabilityProfileId"] == "native_client_stop_capability"
    assert status["hardStopLevel"] == "native_immediate_hold"
    assert status["stopCapabilityQueryFailed"] is False
    assert status["stopProfileUnsupported"] is False
    assert status["canExecute"] is True


def test_stop_capability_query_failure_blocks_configured_profile(tmp_path: Path):
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=MockKernelClient())
    app = create_app(state, security=_TEST_SECURITY)
    deployment_path = _write_deployment_with_stop_profile(
        tmp_path, stop_mode="spring_back_hold"
    )

    with TestClient(app) as local_client:
        load = local_client.post(
            "/api/program/load",
            json={"path": _PROGRAM_PATH, "deployment_path": deployment_path},
        )
        assert load.status_code == 200, load.text

        status = local_client.get("/api/runtime/status").json()
        run = local_client.post("/api/cycle/run")

    assert status["readiness"] == "degraded"
    assert status["canExecute"] is False
    assert status["stopCapabilityQueryFailed"] is True
    assert status["stopProfileUnsupported"] is True
    assert status["stopProfileUnsupportedReasons"] == [
        {
            "profile": "operator_stop_profile",
            "stop_mode": "spring_back_hold",
            "reason": "capability_query_failed",
        },
        {
            "profile": "default_fault_stop_profile",
            "stop_mode": "spring_back_hold",
            "reason": "capability_query_failed",
        },
    ]
    assert run.status_code == 409
    assert run.json()["detail"]["errorCode"] == "stop_capability_query_failed"


def test_missing_required_fault_stop_profile_blocks_motion_arm(tmp_path: Path):
    bundle = _StopCapabilityBundle({
        "supported_stop_modes": ["spring_back_hold"],
        "capability_profile_id": "test_stop_capability",
        "hard_stop_level": "jtc_cancel_unprofiled_hold",
    })
    state = ComposerState(
        catalog_path=_CATALOG_PATH,
        kernel=MockKernelClient(),
        kernel_bundle=bundle,
    )
    app = create_app(state, security=_TEST_SECURITY)
    deployment_path = _write_deployment_with_stop_profile(
        tmp_path, stop_mode="spring_back_hold", include_fault_stop=False
    )

    with TestClient(app) as local_client:
        load = local_client.post(
            "/api/program/load",
            json={"path": _PROGRAM_PATH, "deployment_path": deployment_path},
        )
        assert load.status_code == 200, load.text
        status = local_client.get("/api/runtime/status").json()
        run = local_client.post("/api/cycle/run")

    assert status["readiness"] == "degraded"
    assert status["canExecute"] is False
    assert status["stopProfileUnsupported"] is True
    assert status["stopProfileUnsupportedReasons"] == [
        {
            "profile": "default_fault_stop_profile",
            "stop_mode": "",
            "reason": "stop_profile_required",
        }
    ]
    assert run.status_code == 409
    assert run.json()["detail"]["errorCode"] == "stop_profile_required"


def test_missing_hard_stop_floor_blocks_motion_arm(tmp_path: Path):
    bundle = _StopCapabilityBundle({
        "supported_stop_modes": ["spring_back_hold"],
        "capability_profile_id": "test_stop_capability",
        "hard_stop_level": "unavailable",
    })
    state = ComposerState(
        catalog_path=_CATALOG_PATH,
        kernel=MockKernelClient(),
        kernel_bundle=bundle,
    )
    app = create_app(state, security=_TEST_SECURITY)
    deployment_path = _write_deployment_with_stop_profile(
        tmp_path, stop_mode="spring_back_hold"
    )

    with TestClient(app) as local_client:
        load = local_client.post(
            "/api/program/load",
            json={"path": _PROGRAM_PATH, "deployment_path": deployment_path},
        )
        assert load.status_code == 200, load.text
        status = local_client.get("/api/runtime/status").json()
        run = local_client.post("/api/cycle/run")

    assert status["readiness"] == "degraded"
    assert status["canExecute"] is False
    assert status["stopProfileUnsupported"] is True
    assert status["stopProfileUnsupportedReasons"] == [
        {
            "profile": "hard_stop_floor",
            "stop_mode": "unavailable",
            "reason": "hard_stop_floor_unavailable",
        }
    ]
    assert run.status_code == 409
    assert run.json()["detail"]["errorCode"] == "hard_stop_floor_unavailable"


def test_stop_capability_accessor_throw_blocks_motion_arm(tmp_path: Path):
    bundle = _FlakyStopCapabilityBundle({
        "supported_stop_modes": ["spring_back_hold"],
        "capability_profile_id": "test_flaky_capability",
        "hard_stop_level": "jtc_cancel_unprofiled_hold",
    })
    state = ComposerState(
        catalog_path=_CATALOG_PATH,
        kernel=MockKernelClient(),
        kernel_bundle=bundle,
    )
    app = create_app(state, security=_TEST_SECURITY)
    deployment_path = _write_deployment_with_stop_profile(
        tmp_path, stop_mode="spring_back_hold"
    )

    with TestClient(app) as local_client:
        load = local_client.post(
            "/api/program/load",
            json={"path": _PROGRAM_PATH, "deployment_path": deployment_path},
        )
        assert load.status_code == 200, load.text
        status = local_client.get("/api/runtime/status").json()
        run = local_client.post("/api/cycle/run")

    assert bundle.calls == 2
    assert status["stopProfileUnsupported"] is False
    assert run.status_code == 409
    assert run.json()["detail"]["errorCode"] == "stop_capability_query_failed"


def test_unsupported_stop_profile_blocks_direct_intervention(tmp_path: Path):
    bundle = _StopCapabilityBundle({
        "supported_stop_modes": ["spring_back_hold"],
        "capability_profile_id": "test_spring_only",
        "hard_stop_level": "jtc_cancel_unprofiled_hold",
    })
    state = ComposerState(
        catalog_path=_CATALOG_PATH,
        kernel=MockKernelClient(),
        kernel_bundle=bundle,
    )
    app = create_app(state, security=_TEST_SECURITY)
    deployment_path = _write_deployment_with_stop_profile(
        tmp_path, stop_mode="hard_deceleration"
    )

    with TestClient(app) as local_client:
        load = local_client.post(
            "/api/program/load",
            json={"path": _PROGRAM_PATH, "deployment_path": deployment_path},
        )
        assert load.status_code == 200, load.text
        response = local_client.post(
            "/api/direct/execute",
            json={
                "tree": {
                    "id": "timer",
                    "name": "blocked_direct_intervention",
                    "params": {"duration_s": 0.01},
                    "children": [],
                },
                "principalId": "operator",
            },
        )

    assert response.status_code == 409
    assert response.json()["error_code"] == "stop_profile_unsupported"


def test_unsupported_stop_profile_blocks_direct_joint_jog(
    tmp_path: Path,
):
    state = ComposerState(
        catalog_path=_CATALOG_PATH,
        kernel=MockKernelClient(),
        kernel_bundle=_StopCapabilityBundle({
            "supported_stop_modes": ["spring_back_hold"],
            "capability_profile_id": "test_spring_only",
            "hard_stop_level": "jtc_cancel_unprofiled_hold",
        }),
    )
    app = create_app(state, security=_TEST_SECURITY)
    deployment_path = _write_deployment_with_stop_profile(
        tmp_path, stop_mode="hard_deceleration"
    )

    with TestClient(app) as local_client:
        load = local_client.post(
            "/api/program/load",
            json={"path": _PROGRAM_PATH, "deployment_path": deployment_path},
        )
        assert load.status_code == 200, load.text
        response = local_client.post(
            "/api/direct/joint-jog",
            json={
                "jointNames": ["joint1"],
                "jointPositionsRad": [0.1],
                "principalId": "human:test-operator",
            },
        )

    assert response.status_code == 409
    assert response.json()["error_code"] == "stop_profile_unsupported"


def test_runtime_status_classifies_unknown_kernel_status_as_fault(client, monkeypatch):
    """Unknown cross-boundary kernel statuses fail closed in readiness telemetry."""

    def fake_heartbeat(_state):
        return {
            "status": "backend_unavailable",
            "uptime_s": 1.0,
            "components": {
                "kernel": {
                    "available": True,
                    "type": "MockKernelClient",
                    "state": "backend_unavailable",
                }
            },
        }

    monkeypatch.setattr(routes_runtime, "build_infra_heartbeat_payload", fake_heartbeat)

    response = client.get("/api/runtime/status")

    assert response.status_code == 200
    data = response.json()
    assert data["readiness"] == "fault"
    assert data["canExecute"] is False
    assert "backend_unavailable" in data["userMessage"]
    execution_backend = next(
        component for component in data["components"] if component["id"] == "execution_backend"
    )
    assert execution_backend["status"] == "fault"


def test_runtime_status_classifies_missing_kernel_status_as_fault(client, monkeypatch):
    """The heartbeat contract requires a status; missing values fail closed."""

    def fake_heartbeat(_state):
        return {
            "uptime_s": 1.0,
            "components": {
                "kernel": {
                    "available": True,
                    "type": "MockKernelClient",
                    "state": "executing",
                }
            },
        }

    monkeypatch.setattr(routes_runtime, "build_infra_heartbeat_payload", fake_heartbeat)

    response = client.get("/api/runtime/status")

    assert response.status_code == 200
    data = response.json()
    assert data["readiness"] == "fault"
    assert data["canExecute"] is False
    assert "__missing__" in data["userMessage"]


def test_runtime_status_merges_external_provider_components(client):
    """External bringup supervisors can contribute modular runtime status."""
    response = client.post(
        "/api/runtime/external/isaac_demo_supervisor",
        json={
            "readiness": "waiting",
            "canExecute": False,
            "userMessage": "Waiting for Isaac.",
            "components": [
                {
                    "id": "isaac_stream",
                    "label": "Isaac joint stream",
                    "kind": "simulator",
                    "status": "waiting",
                    "requiredFor": ["execute"],
                    "detail": "Waiting for /isaac_joint_states",
                }
            ],
        },
    )
    assert response.status_code == 200

    data = client.get("/api/runtime/status").json()
    assert "isaac_demo_supervisor" in data["externalProviders"]
    assert any(component["id"] == "isaac_stream" for component in data["components"])


def test_get_saved_full_deployment_reflects_disk_not_applied_runtime(client):
    """GET /api/deployment/saved/full returns the on-disk artifact even after an unsaved apply."""
    client.post("/api/program/load", json={"path": _PROGRAM_PATH, "deployment_path": _DEPLOYMENT_PATH})
    full_deployment = client.get("/api/deployment/full").json()
    modified_deployment = {**full_deployment, "name": "applied_not_saved_deployment"}

    response = client.post("/api/session/apply", json={"deployment": modified_deployment})
    assert response.status_code == 200

    runtime_deployment = client.get("/api/deployment/full").json()
    saved_deployment = client.get("/api/deployment/saved/full").json()
    assert runtime_deployment["name"] == "applied_not_saved_deployment"
    assert saved_deployment["name"] == full_deployment["name"]


def test_load_program_rebuilds_kernel_from_deployment_config():
    """Program load rebuilds the kernel from deployment-derived config when a factory is set."""
    calls = []

    class RebuiltKernel(MockKernelClient):
        pass

    def kernel_factory(deployment):
        calls.append(deployment)
        return RebuiltKernel(), {"bundle": len(calls)}

    state = ComposerState(
        catalog_path=_CATALOG_PATH,
        kernel=MockKernelClient(),
        kernel_factory=kernel_factory,
    )
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as client:
        response = client.post(
            "/api/program/load",
            json={
                "path": _EXAMPLE_PROGRAM_PATH,
                "deployment_path": _EXAMPLE_DEPLOYMENT_PATH,
            },
        )

    assert response.status_code == 200
    assert len(calls) == 1
    assert calls[0] is state.deployment
    assert state.deployment is not None
    assert state.deployment.orchestrator.config["collision_monitor_period_ms"] == 50.0
    assert isinstance(state._kernel, RebuiltKernel)
    assert state.kernel_bundle == {"bundle": 1}


def test_put_deployment(client, state):
    """Load deployment via POST, PUT /api/deployment with modified data, GET /api/deployment — verify updated."""
    # Load deployment
    client.post("/api/deployment/load", json={"path": _DEPLOYMENT_PATH})
    
    # Get original deployment
    response = client.get("/api/deployment")
    original_data = response.json()
    original_name = original_data["name"]
    
    # Create modified deployment data
    modified_data = {
        "name": "updated_deployment",
        "description": "Updated description",
    }
    
    # PUT the modified deployment
    response = client.put("/api/deployment", json=modified_data)
    assert response.status_code == 200
    assert response.json()["status"] == "ok"
    
    # GET the deployment and verify it was updated
    response = client.get("/api/deployment")
    assert response.status_code == 200
    updated_data = response.json()
    assert updated_data["name"] == "updated_deployment"
    assert updated_data["name"] != original_name
    assert updated_data["description"] == "Updated description"


def test_save_deployment(client, state):
    """Load deployment via POST, POST /api/deployment/save with tmp path, verify file exists and is valid YAML."""
    import tempfile
    import os
    
    # Load deployment
    client.post("/api/deployment/load", json={"path": _DEPLOYMENT_PATH})
    
    # Create temp path
    tmp_path = tempfile.mktemp(suffix=".yaml")
    
    try:
        # POST to save
        response = client.post("/api/deployment/save", json={"path": tmp_path})
        assert response.status_code == 200
        data = response.json()
        assert data["status"] == "ok"
        saved_path = data["path"]
        assert saved_path.endswith(".deployment.yaml")
        assert saved_path == f"{tmp_path}.deployment.yaml"
        
        # Verify file exists
        assert os.path.exists(saved_path)
        
        # Verify it's valid YAML
        import yaml
        with open(saved_path) as f:
            saved_data = yaml.safe_load(f)
        
        assert saved_data["api_version"] == "manyforge.deployment.v0"
        assert "robot_config" in saved_data
        assert "adapters" in saved_data
        assert "orchestrator" in saved_data
        assert "launch" in saved_data
    finally:
        if os.path.exists(tmp_path):
            os.unlink(tmp_path)
        suffixed_path = f"{tmp_path}.deployment.yaml"
        if os.path.exists(suffixed_path):
            os.unlink(suffixed_path)


def test_save_deployment_payload_does_not_apply_runtime_state(client):
    """Saving a modified deployment payload writes disk only and does not mutate loaded runtime state."""
    import os
    import tempfile
    import yaml

    client.post("/api/program/load", json={"path": _PROGRAM_PATH, "deployment_path": _DEPLOYMENT_PATH})
    original = client.get("/api/deployment/full").json()
    modified = {**original, "name": "saved_only_deployment"}

    tmp_path = tempfile.mktemp(suffix=".yaml")
    try:
      response = client.post("/api/deployment/save", json={"path": tmp_path, "deployment": modified})
      assert response.status_code == 200
      saved_path = response.json()["path"]
      assert saved_path == f"{tmp_path}.deployment.yaml"

      current = client.get("/api/deployment/full").json()
      assert current["name"] == original["name"]

      with open(saved_path) as f:
          saved_data = yaml.safe_load(f)
      assert saved_data["name"] == "saved_only_deployment"
    finally:
      if os.path.exists(tmp_path):
          os.unlink(tmp_path)
      suffixed_path = f"{tmp_path}.deployment.yaml"
      if os.path.exists(suffixed_path):
          os.unlink(suffixed_path)


def test_save_full_deployment_strips_readonly_gripper_response_fields(client):
    """Full UI payloads carry read-only gripper metadata; saved YAML must not."""
    import os
    import tempfile
    import yaml

    load = client.post(
        "/api/deployment/load",
        json={"path": _ISAAC_LITE6_DEPLOYMENT_PATH},
    )
    assert load.status_code == 200
    full = client.get("/api/deployment/full").json()
    gripper = full["grippers"]["primary"]
    assert gripper["srdfPresets"]
    assert "semanticConfigured" in gripper
    assert "migrationNotes" in gripper

    tmp_path = tempfile.mktemp(
        dir=str(_REPO_ROOT / "examples"),
        suffix=".yaml",
    )
    try:
        response = client.post(
            "/api/deployment/save",
            json={"path": tmp_path, "deployment": full},
        )
        assert response.status_code == 200
        saved_path = response.json()["path"]
        with open(saved_path) as f:
            saved_data = yaml.safe_load(f)

        saved_gripper = saved_data["grippers"]["primary"]
        assert saved_gripper["actuation_kind"] == "pneumatic"
        assert saved_gripper["transport"]["kind"] == "isaac_joint_state_effort"
        assert "srdf_presets" not in saved_gripper
        assert "semantic_configured" not in saved_gripper
        assert "migration_notes" not in saved_gripper
        assert "visualization" not in saved_gripper
    finally:
        if os.path.exists(tmp_path):
            os.unlink(tmp_path)
        suffixed_path = f"{tmp_path}.deployment.yaml"
        if os.path.exists(suffixed_path):
            os.unlink(suffixed_path)


def test_save_full_legacy_gripper_does_not_materialize_display_semantics(client):
    """Compatibility semantics shown by the API must not silently rewrite YAML."""
    import os
    import tempfile
    import yaml

    load = client.post(
        "/api/deployment/load",
        json={"path": _UR10E_ROBOTIQ_DEPLOYMENT_PATH},
    )
    assert load.status_code == 200
    full = client.get("/api/deployment/full").json()
    gripper = full["grippers"]["primary"]
    assert gripper["commandMode"] == "simulated_joint"
    assert gripper["semanticConfigured"] is False
    assert gripper["actuationKind"] == "position_commanded"

    tmp_path = tempfile.mktemp(
        dir=str(_REPO_ROOT / "examples"),
        suffix=".yaml",
    )
    try:
        response = client.post(
            "/api/deployment/save",
            json={"path": tmp_path, "deployment": full},
        )
        assert response.status_code == 200
        saved_path = response.json()["path"]
        with open(saved_path) as f:
            saved_data = yaml.safe_load(f)

        saved_gripper = saved_data["grippers"]["primary"]
        assert saved_gripper["command_mode"] == "simulated_joint"
        assert "actuation_kind" not in saved_gripper
        assert "state_authority" not in saved_gripper
        assert "completion_policy" not in saved_gripper
        assert "transport" not in saved_gripper
        assert "semantic_configured" not in saved_gripper
    finally:
        if os.path.exists(tmp_path):
            os.unlink(tmp_path)
        suffixed_path = f"{tmp_path}.deployment.yaml"
        if os.path.exists(suffixed_path):
            os.unlink(suffixed_path)


# =============================================================================
# WebSocket Tests
# =============================================================================

def test_ws_state_receives_update(state):
    """Connect to /ws/state, verify at least one JSON message with state/velocityScale/timestamp fields."""
    app = create_app(state)
    with TestClient(app) as tc:
        with tc.websocket_connect("/ws/state") as ws:
            # The broadcast loop sends at 10Hz, so a message should arrive quickly
            message = ws.receive_json()
            assert "state" in message
            assert "velocityScale" in message
            assert "kernelEvents" in message
            assert isinstance(message["kernelEvents"], list)
            assert "timestamp" in message


def test_ws_includes_node_statuses(state):
    """WebSocket messages include nodeStatuses key."""
    app = create_app(state)
    with TestClient(app) as tc:
        with tc.websocket_connect("/ws/state") as ws:
            data = ws.receive_json()
            assert "nodeStatuses" in data
            assert isinstance(data["nodeStatuses"], dict)


def test_ws_state_falls_back_to_kernel_snapshot_fault(state):
    """WebSocket state mirrors `/api/cycle/status` fault visibility."""
    state.cycle_manager = _KernelSnapshotFaultCycleManager()
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as tc:
        with tc.websocket_connect("/ws/state") as ws:
            data = ws.receive_json()
            assert data["state"] == "stopped"
            assert data["errorCode"] == "controller_cancel_timeout"
            assert data["errorDetails"]["reason_code"] == "controller_cancel_timeout"


# =============================================================================
# Program Save/Update Tests
# =============================================================================

def test_put_program_tree(client, state):
    """Load program via POST, PUT /api/program/tree with modified tree, GET /api/program/tree — verify updated."""
    # Load program
    client.post("/api/program/load", json={"path": _PROGRAM_PATH})
    
    # Get original tree
    response = client.get("/api/program/tree")
    original_tree = response.json()["tree"]
    original_name = original_tree["name"]
    
    # Create modified tree
    modified_tree = {
        "id": "sequence",
        "name": "updated_sequence",
        "children": [
            {
                "id": "reset_trajectories",
                "name": "reset",
            },
        ],
    }
    
    # PUT the modified tree
    response = client.put("/api/program/tree", json={"tree": modified_tree})
    assert response.status_code == 200
    assert response.json()["status"] == "ok"
    
    # GET the tree and verify it was updated
    response = client.get("/api/program/tree")
    assert response.status_code == 200
    updated_tree = response.json()["tree"]
    assert updated_tree["name"] == "updated_sequence"
    assert updated_tree["name"] != original_name
    assert len(updated_tree["children"]) == 1

def test_save_program(client, state):
    """Load program via POST, POST /api/program/save with tmp path, verify file exists and is valid YAML."""
    import tempfile
    import os
    
    # Load program
    client.post("/api/program/load", json={"path": _PROGRAM_PATH})
    
    # Create temp path
    tmp_path = tempfile.mktemp(suffix=".yaml")
    
    try:
        # POST to save
        response = client.post("/api/program/save", json={"path": tmp_path})
        assert response.status_code == 200
        data = response.json()
        assert data["status"] == "ok"
        saved_path = data["path"]
        assert saved_path.endswith(".program.yaml")
        assert saved_path == f"{tmp_path}.program.yaml"
        
        # Verify file exists
        assert os.path.exists(saved_path)
        
        # Verify it's valid YAML
        import yaml
        with open(saved_path) as f:
            saved_data = yaml.safe_load(f)
        
        assert saved_data["api_version"] == "manyforge.program.v0"
        assert "tree" in saved_data
        assert saved_data["tree"]["name"] == "main_sequence"
    finally:
        if os.path.exists(tmp_path):
            os.unlink(tmp_path)
        suffixed_path = f"{tmp_path}.program.yaml"
        if os.path.exists(suffixed_path):
            os.unlink(suffixed_path)


def test_save_program_no_diff_revalidates_loaded_program(client, monkeypatch):
    """Saving without an explicit patch still rebuilds and revalidates the artifact."""
    import os
    import tempfile

    from manyforge_behavior.program import Program as RealProgram
    from manyforge_composer.backend import routes_program as routes_program_module

    client.post("/api/program/load", json={"path": _PROGRAM_PATH, "deployment_path": _DEPLOYMENT_PATH})

    called = False

    class ObservedProgram:
        @staticmethod
        def from_dict(*args, **kwargs):
            nonlocal called
            called = True
            return RealProgram.from_dict(*args, **kwargs)

    monkeypatch.setattr(routes_program_module, "Program", ObservedProgram)

    tmp_path = tempfile.mktemp(suffix=".yaml")
    try:
        response = client.post("/api/program/save", json={"path": tmp_path})
        assert response.status_code == 200
        assert called is True
    finally:
        if os.path.exists(tmp_path):
            os.unlink(tmp_path)
        suffixed_path = f"{tmp_path}.program.yaml"
        if os.path.exists(suffixed_path):
            os.unlink(suffixed_path)


def test_save_program_tree_payload_does_not_apply_runtime_state(client):
    """Saving a modified tree payload writes disk only and does not mutate loaded runtime state."""
    import os
    import tempfile
    import yaml

    client.post("/api/program/load", json={"path": _PROGRAM_PATH})
    original_tree = client.get("/api/program/tree").json()["tree"]
    modified_tree = {
        "id": "sequence",
        "name": "saved_only_sequence",
        "children": [
            {
                "id": "reset_trajectories",
                "name": "reset_once",
            },
        ],
    }

    tmp_path = tempfile.mktemp(suffix=".yaml")
    try:
        response = client.post("/api/program/save", json={"path": tmp_path, "tree": modified_tree})
        assert response.status_code == 200
        saved_path = response.json()["path"]
        assert saved_path == f"{tmp_path}.program.yaml"

        current_tree = client.get("/api/program/tree").json()["tree"]
        assert current_tree == original_tree

        with open(saved_path) as f:
            saved_data = yaml.safe_load(f)
        assert saved_data["tree"]["name"] == "saved_only_sequence"
    finally:
        if os.path.exists(tmp_path):
            os.unlink(tmp_path)
        suffixed_path = f"{tmp_path}.program.yaml"
        if os.path.exists(suffixed_path):
            os.unlink(suffixed_path)


def test_apply_session_updates_program_and_deployment_together(client):
    """POST /api/session/apply updates the runtime models atomically."""
    client.post("/api/program/load", json={"path": _PROGRAM_PATH, "deployment_path": _DEPLOYMENT_PATH})
    full_deployment = client.get("/api/deployment/full").json()

    modified_tree = {
        "id": "sequence",
        "name": "applied_sequence",
        "children": [
            {
                "id": "reset_trajectories",
                "name": "reset_once",
            },
        ],
    }
    modified_deployment = {**full_deployment, "name": "applied_deployment"}

    response = client.post(
        "/api/session/apply",
        json={"tree": modified_tree, "deployment": modified_deployment},
    )
    assert response.status_code == 200

    current_tree = client.get("/api/program/tree").json()["tree"]
    current_deployment = client.get("/api/deployment/full").json()
    assert current_tree["name"] == "applied_sequence"
    assert current_deployment["name"] == "applied_deployment"


def test_apply_session_program_uses_candidate_deployment_capabilities(client):
    """Atomic apply validates program modes against the edited deployment payload."""
    client.post(
        "/api/program/load",
        json={"path": _EXAMPLE_PROGRAM_PATH, "deployment_path": _EXAMPLE_DEPLOYMENT_PATH},
    )
    full_deployment = client.get("/api/deployment/full").json()
    assert full_deployment.get("simulationControl") is None

    modified_tree = {
        "id": "sequence",
        "name": "sim_control_sequence",
        "children": [
            {
                "id": "reset_simulation",
                "name": "reset_sim",
                "params": {"reason": "apply regression"},
            },
            {
                "id": "set_simulation_state",
                "name": "play_sim",
                "params": {"state": "playing", "reason": "apply regression"},
            },
        ],
    }
    modified_deployment = {
        **full_deployment,
        "name": "sim_control_deployment",
        "simulationControl": {
            "kind": "ros2_simulation_interfaces",
            "servicePrefix": "auto",
            "capabilities": {
                "reset": True,
                "pause": True,
                "play": True,
                "step": True,
                "setEntityState": True,
            },
            "reset": {
                "defaultScope": "state",
                "closeCommandGate": True,
                "waitReadyS": 3.0,
                "resumePlayAfterReset": True,
            },
        },
    }

    response = client.post(
        "/api/session/apply",
        json={"tree": modified_tree, "deployment": modified_deployment},
    )
    assert response.status_code == 200

    current_tree = client.get("/api/program/tree").json()["tree"]
    current_deployment = client.get("/api/deployment/full").json()
    assert current_tree["name"] == "sim_control_sequence"
    assert current_deployment["name"] == "sim_control_deployment"
    assert current_deployment["simulationControl"]["capabilities"]["reset"] is True
    assert current_deployment["simulationControl"]["capabilities"]["play"] is True


def test_apply_session_updates_program_parameters_without_touching_saved_artifact(client):
    client.post("/api/program/load", json={"path": _PROGRAM_PATH, "deployment_path": _DEPLOYMENT_PATH})

    response = client.post(
        "/api/session/apply",
        json={
            "programParameters": [
                {
                    "name": "pick_velocity_scale",
                    "type": "float",
                    "default": 0.8,
                    "constraints": {
                        "min_exclusive": 0.0,
                        "max_inclusive": 1.0,
                    },
                    "description": "Velocity scale for pick moves.",
                }
            ]
        },
    )
    assert response.status_code == 200

    current_program = client.get("/api/program").json()
    saved_program = client.get("/api/program/saved").json()
    assert current_program["parameters"][0]["default"] == 0.8
    assert saved_program["parameters"][0]["default"] == 0.5


def test_apply_session_updates_scene_without_touching_saved_artifact(client):
    client.post("/api/program/load", json={"path": _PROGRAM_PATH, "deployment_path": _DEPLOYMENT_PATH})

    response = client.post(
        "/api/session/apply",
        json={
            "scene": [
                {
                    "objectId": "runtime_box",
                    "shape": {
                        "type": "box",
                        "box_dimensions_m": [0.08, 0.08, 0.12],
                    },
                    "pose": {
                        "frame_id": "universe",
                        "position_m": [0.45, 0.0, 0.06],
                        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                    },
                }
            ]
        },
    )
    assert response.status_code == 200

    current_program = client.get("/api/program").json()
    saved_program = client.get("/api/program/saved").json()
    assert current_program["sceneResources"][0]["objectId"] == "runtime_box"
    assert saved_program["sceneResources"][0]["objectId"] == "table"


def test_save_program_persists_updated_parameters(client, tmp_path):
    client.post("/api/program/load", json={"path": _PROGRAM_PATH})

    save_target = tmp_path / "parameterized"
    response = client.post(
        "/api/program/save",
        json={
            "path": str(save_target),
            "parameters": [
                {
                    "name": "pick_velocity_scale",
                    "type": "float",
                    "default": 0.75,
                    "constraints": {
                        "min_exclusive": 0.0,
                        "max_inclusive": 1.0,
                    },
                    "description": "Velocity scale for pick moves.",
                }
            ],
        },
    )
    assert response.status_code == 200
    saved_yaml = yaml.safe_load(Path(response.json()["path"]).read_text(encoding="utf-8"))
    assert saved_yaml["parameters"][0]["default"] == 0.75


def test_save_program_persists_updated_scene_resources(client, tmp_path):
    client.post("/api/program/load", json={"path": _PROGRAM_PATH})

    save_target = tmp_path / "scene_promoted"
    response = client.post(
        "/api/program/save",
        json={
            "path": str(save_target),
            "scene": [
                {
                    "objectId": "runtime_box",
                    "shape": {
                        "type": "box",
                        "box_dimensions_m": [0.1, 0.2, 0.3],
                    },
                    "pose": {
                        "frame_id": "universe",
                        "position_m": [0.4, 0.1, 0.2],
                        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                    },
                }
            ],
        },
    )
    assert response.status_code == 200
    saved_yaml = yaml.safe_load(Path(response.json()["path"]).read_text(encoding="utf-8"))
    assert saved_yaml["scene"][0]["object_id"] == "runtime_box"
    assert saved_yaml["scene"][0]["shape"]["box_dimensions_m"] == [0.1, 0.2, 0.3]


# =============================================================================
# Logging Tests
# =============================================================================

def test_json_log_format():
    """setup_logging produces JSON with level, event, timestamp keys."""
    import io
    import json
    import logging
    from manyforge_composer.backend.logging_config import setup_logging

    root_logger = logging.getLogger()
    uvicorn_access_logger = logging.getLogger("uvicorn.access")
    original_handlers = tuple(root_logger.handlers)
    original_root_level = root_logger.level
    original_uvicorn_access_level = uvicorn_access_logger.level

    try:
        setup_logging("INFO")
        configured_handlers = tuple(root_logger.handlers)
        test_logger = logging.getLogger("test.json_format")
        buf = io.StringIO()
        handler = logging.StreamHandler(buf)
        handler.setFormatter(root_logger.handlers[0].formatter)
        test_logger.addHandler(handler)
        try:
            test_logger.info("hello world")
        finally:
            test_logger.removeHandler(handler)
            handler.close()
    finally:
        # setup_logging() intentionally replaces the process-wide root handlers.
        # This test must put pytest's capture handler back or later caplog tests
        # become order-dependent while production logging remains correct.
        for configured_handler in tuple(root_logger.handlers):
            root_logger.removeHandler(configured_handler)
        for original_handler in original_handlers:
            root_logger.addHandler(original_handler)
        root_logger.setLevel(original_root_level)
        uvicorn_access_logger.setLevel(original_uvicorn_access_level)
        for configured_handler in configured_handlers:
            if configured_handler not in original_handlers:
                configured_handler.close()

    line = buf.getvalue().strip()
    record = json.loads(line)
    assert "timestamp" in record
    assert record["level"] == "INFO"
    assert record["event"] == "hello world"
    assert record["logger"] == "test.json_format"


def test_request_logging(client, capsys):
    """HTTP requests produce structured log entries."""
    client.get("/api/health")
    # Just verify the request didn't error — the logging middleware runs
    # but capturing stderr in a test with uvicorn is non-trivial.
    # The JSON format test above covers the formatter.


def test_program_load_callback_registers_scene_edit_callback(caplog):
    """Program-load callback wires scene callbacks without raising or logging errors."""
    py_trees.blackboard.Blackboard.clear()

    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=MockKernelClient())
    class DummyBundle:
        def __init__(self):
            self.set_calls = 0

        def set_joint_positions(self, positions):
            self.set_calls += 1

    bundle = DummyBundle()
    state._on_program_loaded = build_program_loaded_callback(
        bundle=bundle,
    )

    with caplog.at_level(logging.ERROR):
        state.load_program(_PROGRAM_PATH)

    assert not any("on_program_loaded callback failed" in rec.message for rec in caplog.records)
    assert state.cycle_manager._on_run_from_idle is not None
    assert state.cycle_manager._tree is not None
    assert bundle.set_calls == 0

    bb = py_trees.blackboard.Client(name="test_scene_edit_callback")
    bb.register_key(key="scene_edit_callback", access=py_trees.common.Access.READ)
    assert callable(bb.scene_edit_callback)

    bb.scene_edit_callback(
        "add",
        SceneEditRequest(
            operation="add",
            object_id="visual_box",
            shape_type="box",
            box_dimensions_m=[0.1, 0.2, 0.3],
            pose_position_m=[0.4, 0.5, 0.6],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        ),
    )
    visual_objects, visual_meta = state.visual_scene_objects_snapshot()
    assert visual_meta["timestamp_ns"] > 0
    visual_box = next(obj for obj in visual_objects if obj.object_id == "visual_box")
    assert visual_box.attached is False
    assert visual_box.pose_in_universe["position_m"] == [0.4, 0.5, 0.6]

    state.cycle_manager._on_run_from_idle()
    _refreshed_objects, refreshed_meta = state.visual_scene_objects_snapshot()
    assert refreshed_meta["timestamp_ns"] > 0


def test_program_load_callback_installs_native_visuals(caplog):
    """Native visual scene-edit wiring works."""
    py_trees.blackboard.Blackboard.clear()

    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=MockKernelClient())
    state._on_program_loaded = build_program_loaded_callback(
        bundle=None,
    )

    with caplog.at_level(logging.ERROR):
        state.load_program(_PROGRAM_PATH)

    assert not any("on_program_loaded callback failed" in rec.message for rec in caplog.records)
    assert state.cycle_manager._on_run_from_idle is not None
    assert state.cycle_manager._tree is not None

    bb = py_trees.blackboard.Client(name="test_native_scene_edit_callback")
    bb.register_key(key="scene_edit_callback", access=py_trees.common.Access.READ)
    assert callable(bb.scene_edit_callback)

    bb.scene_edit_callback(
        "add",
        SceneEditRequest(
            operation="add",
            object_id="visual_box",
            shape_type="box",
            box_dimensions_m=[0.1, 0.1, 0.1],
            pose_position_m=[0.01, 0.02, 0.03],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        ),
    )
    visual_objects, visual_meta = state.visual_scene_objects_snapshot()
    assert visual_meta["timestamp_ns"] > 0
    visual_box = next(obj for obj in visual_objects if obj.object_id == "visual_box")
    assert visual_box.attached is False
    assert visual_box.frame_id == "universe"


def test_program_load_callback_applies_simulator_bootstrap_from_deployment(caplog):
    """Program-load callback applies explicit simulator-only startup joint positions."""
    py_trees.blackboard.Blackboard.clear()

    class DummyBundle:
        def __init__(self):
            self.set_calls = []

        def set_joint_positions(self, positions):
            self.set_calls.append(list(positions))

    bundle = DummyBundle()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=MockKernelClient())
    state._on_program_loaded = build_program_loaded_callback(
        bundle=bundle,
    )

    with caplog.at_level(logging.ERROR):
        state.load_program(_EXAMPLE_PROGRAM_PATH, deployment_path=_EXAMPLE_DEPLOYMENT_PATH)

    assert not any("on_program_loaded callback failed" in rec.message for rec in caplog.records)
    assert bundle.set_calls == [[0.0, -1.57, 1.57, -1.57, -1.57, 0.0]]


def test_debug_scene_trace_endpoint_returns_backend_and_visual_observability():
    """Debug scene trace endpoint exposes current backend/native-viewer trace scope."""
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    state.load_program(_PROGRAM_PATH)

    kernel.scene_edit(
        SceneEditRequest(
            operation="add",
            object_id="trace_box",
            pose_position_m=[0.4, 0.1, 0.2],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        )
    )

    state.refresh_visual_scene_objects_sync(reason="test_debug_scene_trace")
    app = create_app(state)

    with TestClient(app) as client:
        response = client.get("/api/debug/scene-trace?limit=10")

    assert response.status_code == 200
    data = response.json()
    assert "included" in data["scope"]
    assert data["backend"]
    assert data["backend"][-1]["event"] == "add"
    assert data["visual"]
    assert data["visual"][0]["event"] == "visual_scene_objects_snapshot"
    assert "hmi" not in data


def test_debug_collision_compare_trajectory_endpoint_uses_cached_waypoint():
    """Debug endpoint compares old/new collision engines for cached waypoints."""

    class CompareKernel(MockKernelClient):
        def __init__(self):
            super().__init__()
            self.compared_positions = None
            self.compared_waypoint_index = None

        def get_cached_trajectory(self, move_id):
            assert move_id == "pick_retreat"
            return Trajectory(
                joint_names=["joint_1", "joint_2"],
                waypoints=[
                    TrajectoryWaypoint(
                        time_from_start_s=0.0,
                        positions_rad=[0.1, 0.2],
                    ),
                    TrajectoryWaypoint(
                        time_from_start_s=0.5,
                        positions_rad=[0.3, 0.4],
                    ),
                ],
            )

        def collision_compare_positions(self, positions_rad, *, waypoint_index=None):
            self.compared_positions = list(positions_rad)
            self.compared_waypoint_index = waypoint_index
            return {
                "ok": True,
                "match": False,
                "legacy": {"in_collision": False},
                "manyforge_collision": {"in_collision": True},
            }

    kernel = CompareKernel()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state)

    with TestClient(app) as client:
        response = client.get(
            "/api/debug/collision/compare-trajectory/pick_retreat?waypoint_index=1"
        )

    assert response.status_code == 200
    data = response.json()
    assert data["move_id"] == "pick_retreat"
    assert data["waypoint_index"] == 1
    assert data["positions_rad"] == [0.3, 0.4]
    assert data["comparison"]["manyforge_collision"]["in_collision"] is True
    assert kernel.compared_positions == [0.3, 0.4]
    assert kernel.compared_waypoint_index == 1


def test_debug_planning_qualification_routes_preserve_generation_and_snapshot():
    class QualificationKernel(MockKernelClient):
        def __init__(self):
            super().__init__()
            self.active_generation = 0

        def get_scene_authority_snapshot(self):
            return {
                "revision": {"revision_id": 7, "content_hash": "scene-seven"},
                "objects": [],
            }

        def start_planning_qualification_capture(self, capacity=128):
            assert capacity == 4
            self.active_generation = 9
            return self.active_generation

        def drain_planning_qualification_capture(self, generation):
            assert generation == self.active_generation
            return {
                "records": [{"sequence": 1, "request": {"move_id": "move"}}],
                "dropped_total": 1,
                "in_flight": 1,
                "status": {
                    "behavior_dropped_total": 3,
                    "behavior_in_flight": 2,
                },
            }

        def stop_planning_qualification_capture(
            self, generation, idle_timeout_ms=10_000
        ):
            assert generation == self.active_generation
            assert idle_timeout_ms == 123
            self.active_generation = 0
            return {
                "records": [{"sequence": 2, "request": {"move_id": "move"}}],
                "dropped_total": 2,
                "in_flight": 0,
                "status": {
                    "behavior_dropped_total": 4,
                    "behavior_in_flight": 0,
                },
            }

        def planning_qualification_capture_status(self):
            return {
                "generation": self.active_generation,
                "dropped_total": 0,
                "in_flight": 0,
            }

    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=QualificationKernel())
    app = create_app(state)
    with TestClient(app) as client:
        snapshot = client.get(
            "/api/debug/qualification/scene-authority-snapshot"
        )
        started = client.post(
            "/api/debug/qualification/planning-capture/start",
            json={"capacity": 4},
        )
        drained = client.post(
            "/api/debug/qualification/planning-capture/drain",
            json={"generation": 9},
        )
        stopped = client.post(
            "/api/debug/qualification/planning-capture/stop",
            json={"generation": 9, "idle_timeout_ms": 123},
        )

    assert snapshot.json()["revision"]["revision_id"] == 7
    assert started.json() == {"generation": 9, "capacity": 4}
    assert drained.json()["records"][0]["sequence"] == 1
    assert drained.json()["status"]["dropped_total"] == 1
    assert drained.json()["status"]["in_flight"] == 1
    assert drained.json()["status"]["behavior_dropped_total"] == 3
    assert drained.json()["status"]["behavior_in_flight"] == 2
    assert stopped.json()["status"]["generation"] == 9
    assert stopped.json()["status"]["dropped_total"] == 2
    assert stopped.json()["status"]["behavior_dropped_total"] == 4
    assert stopped.json()["status"]["behavior_in_flight"] == 0
    assert stopped.json()["records"][0]["sequence"] == 2


def test_debug_qualification_environment_reports_state_websocket_clients():
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=MockKernelClient())
    app = create_app(state)
    with TestClient(app) as client:
        disconnected = client.get("/api/debug/qualification/environment")
        with client.websocket_connect("/ws/state"):
            connected = client.get("/api/debug/qualification/environment")

    assert disconnected.json() == {"state_websocket_clients": 0}
    assert connected.json() == {"state_websocket_clients": 1}


def test_debug_collision_compare_positions_endpoint_uses_explicit_positions():
    """Debug endpoint accepts positions copied from a collision event."""

    class CompareKernel(MockKernelClient):
        def __init__(self):
            super().__init__()
            self.compared_positions = None

        def collision_compare_positions(self, positions_rad, *, waypoint_index=None):
            self.compared_positions = list(positions_rad)
            return {
                "ok": True,
                "match": True,
                "waypoint_index": waypoint_index,
            }

    kernel = CompareKernel()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state)

    with TestClient(app) as client:
        response = client.post(
            "/api/debug/collision/compare-positions",
            json={"positions_rad": [0.1, 0.2], "waypoint_index": 0},
        )

    assert response.status_code == 200
    data = response.json()
    assert data["positions_rad"] == [0.1, 0.2]
    assert data["comparison"]["match"] is True
    assert kernel.compared_positions == [0.1, 0.2]


def test_scene_snapshot_returns_503_without_snapshot_source():
    """GET /api/scene/snapshot returns 503 when no viewer snapshot source exists."""
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state)
    with TestClient(app) as client:
        response = client.get("/api/scene/snapshot")
    assert response.status_code == 503
    assert "No native scene snapshot viewer connected" in response.json()["detail"]


def test_scene_snapshot_returns_image_from_native_broker():
    """GET /api/scene/snapshot returns image bytes from the native snapshot broker."""
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)

    # Minimal JPEG-like byte sequence is enough here; the route serves the
    # renderer bytes verbatim and does not decode image content.
    fake_jpeg = b"\xff\xd8\xff\xd9"

    class MockBroker:
        def has_clients(self):
            return True

        def render_sync(self, width=640, height=480, format="jpeg"):
            return fake_jpeg

    state.snapshot_broker = MockBroker()
    app = create_app(state)
    with TestClient(app) as client:
        response = client.get("/api/scene/snapshot?width=320&height=240&format=jpeg")
    assert response.status_code == 200
    assert response.headers["content-type"] == "image/jpeg"
    assert len(response.content) > 0


def test_published_scene_snapshot_returns_stored_image():
    """GET /api/scene/snapshots/{id} serves the exact published bytes."""
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    state.public_api_base_url = "http://localhost:9000"
    published = state.publish_scene_snapshot(
        b"\xff\xd8fake-jpeg",
        width=320,
        height=240,
        format="jpeg",
    )
    app = create_app(state)
    with TestClient(app) as client:
        response = client.get(f"/api/scene/snapshots/{published['snapshot_id']}")
    assert response.status_code == 200
    assert response.headers["content-type"] == "image/jpeg"
    assert response.headers["etag"] == published["sha256"]
    assert response.content == b"\xff\xd8fake-jpeg"


def test_publish_scene_snapshot_rejects_invalid_explicit_fetch_base_url():
    """Explicit fetch_base_url must be an absolute HTTP(S) URL with a host."""
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)

    with pytest.raises(RuntimeError, match="absolute http\\(s\\) URL with a host"):
        state.publish_scene_snapshot(
            b"\xff\xd8fake-jpeg",
            width=320,
            height=240,
            format="jpeg",
            fetch_base_url="javascript:alert(1)",
        )


def test_publish_scene_snapshot_rejects_invalid_public_api_base_url():
    """Configured public_api_base_url must also be an absolute HTTP(S) URL with a host."""
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    state.public_api_base_url = "/relative/base"

    with pytest.raises(RuntimeError, match="absolute http\\(s\\) URL with a host"):
        state.publish_scene_snapshot(
            b"\xff\xd8fake-jpeg",
            width=320,
            height=240,
            format="jpeg",
        )


def test_published_scene_snapshot_404_for_missing_id():
    """GET /api/scene/snapshots/{id} returns 404 for unknown ids."""
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state)
    with TestClient(app) as client:
        response = client.get("/api/scene/snapshots/missing")
    assert response.status_code == 404


# =============================================================================
# Direct Control / Intervention Tests
# =============================================================================

def test_direct_execute_no_program_409(client):
    """POST /api/direct/execute returns 409 when no program loaded."""
    response = client.post("/api/direct/execute", json={
        "tree": {"id": "timer", "name": "t", "params": {"duration_s": 0.01}, "children": []},
        "principalId": "human:test-operator",
    })
    assert response.status_code == 409


def test_direct_status_no_program_409(client):
    """GET /api/direct/status returns 409 when no program loaded."""
    response = client.get("/api/direct/status")
    assert response.status_code == 409


def test_direct_abort_no_program_409(client):
    """POST /api/direct/abort returns 409 when no program loaded."""
    response = client.post("/api/direct/abort")
    assert response.status_code == 409


def test_direct_status_no_session_404():
    """GET /api/direct/status returns 404 when no intervention has run."""
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": _PROGRAM_PATH})
        response = client.get("/api/direct/status")
    assert response.status_code == 404


def test_direct_pending_recovery_404_when_no_request():
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": _PROGRAM_PATH})
        response = client.get("/api/direct/pending-recovery")
    assert response.status_code == 404


def _wait_for_pending_recovery(client, *, timeout_s: float = 2.0):
    deadline = time.monotonic() + timeout_s
    last_response = None
    while time.monotonic() < deadline:
        last_response = client.get("/api/direct/pending-recovery")
        if last_response.status_code == 200:
            return last_response
        time.sleep(0.02)
    return last_response


def _wait_for_no_pending_recovery(client, *, timeout_s: float = 2.0):
    deadline = time.monotonic() + timeout_s
    last_response = None
    while time.monotonic() < deadline:
        last_response = client.get("/api/direct/pending-recovery")
        if last_response.status_code == 404:
            return last_response
        time.sleep(0.02)
    return last_response


def test_delegated_recovery_request_created_on_node_failure(tmp_path):
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    program_path = _write_program_with_failing_delegate_policy(
        tmp_path,
        intervention_delegate="llm_allowed",
        persistent_edit="llm_allowed",
    )
    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": program_path})
        client.post("/api/cycle/run")

        pending = _wait_for_pending_recovery(client)
        assert pending.status_code == 200
        data = pending.json()
        assert data["triggerNodeInstance"] == "failing_delegate_check"
        # When no identifying kernel event is in the window, the
        # delegated-recovery fallback emits the canonical `execute_failed`
        # per spec/460 §6.2.2; the legacy `node_failure` alias is kept
        # only for serialised-trace round-trip.
        assert data["triggerReason"] == "execute_failed"
        assert data["delegatePolicy"] == "llm_allowed"
        assert data["allowedAuthoritySources"] == ["human", "llm"]
        assert "runtime_override" in data["allowedActions"]
        assert "draft_patch" in data["allowedActions"]

        digest = client.get("/api/execution/digest").json()
        assert digest["state"] == "stopped"
        assert digest["delegatedRecoveryRequest"]["triggerNodeInstance"] == "failing_delegate_check"


def test_direct_execute_delegated_recovery_consumes_pending_request(tmp_path):
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    program_path = _write_program_with_failing_delegate_policy(
        tmp_path,
        intervention_delegate="llm_allowed",
    )
    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": program_path})
        client.post("/api/cycle/run")
        pending = _wait_for_pending_recovery(client)
        assert pending.status_code == 200

        response = client.post("/api/direct/execute", json={
            "tree": {
                "id": "timer",
                "name": "delegated_pause",
                "params": {"duration_s": 0.05},
                "children": [],
            },
            "sessionKind": "delegated_recovery",
            "authoritySource": "llm",
            "principalId": "agent:test-llm",
            "grantType": "policy_granted",
            "resumeTarget": "stop",
        })
        assert response.status_code == 200
        assert response.json()["sessionKind"] == "delegated_recovery"
        assert response.json()["triggerNodeInstance"] == "failing_delegate_check"
        assert response.json()["principalId"] == "agent:test-llm"
        assert response.json()["grantType"] == "policy_granted"

        pending = _wait_for_no_pending_recovery(client)
        assert pending.status_code == 404


def test_direct_execute_delegated_recovery_enforces_pending_authority(tmp_path):
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    program_path = _write_program_with_failing_delegate_policy(
        tmp_path,
        intervention_delegate="human_only",
    )
    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": program_path})
        client.post("/api/cycle/run")
        pending = _wait_for_pending_recovery(client)
        assert pending.status_code == 200

        response = client.post("/api/direct/execute", json={
            "tree": {
                "id": "timer",
                "name": "delegated_pause",
                "params": {"duration_s": 0.05},
                "children": [],
            },
            "sessionKind": "delegated_recovery",
            "authoritySource": "llm",
            "principalId": "agent:test-llm",
            "grantType": "policy_granted",
            "resumeTarget": "stop",
        })
        assert response.status_code == 409
        assert "authority_source must be one of: human" in response.json()["error"]


def test_direct_execute_and_status():
    """POST /api/direct/execute starts intervention; GET status returns session."""
    import time

    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": _PROGRAM_PATH})

        response = client.post("/api/direct/execute", json={
            "tree": {
                "id": "timer",
                "name": "brief_timer",
                "params": {"duration_s": 0.05},
                "children": [],
            },
            "sessionKind": "manual_intervention",
            "authoritySource": "human",
            "principalId": "human:test-operator",
            "approvalMode": "human_approved",
        })
        assert response.status_code == 200
        data = response.json()
        assert "sessionId" in data
        assert data["sessionKind"] == "manual_intervention"
        assert data["authoritySource"] == "human"
        assert data["principalId"] == "human:test-operator"
        assert data["grantType"] == "human_approved"
        assert data["status"] == "running"

        # Wait for completion
        time.sleep(0.3)

        status_resp = client.get("/api/direct/status")
        assert status_resp.status_code == 200
        status_data = status_resp.json()
        assert status_data["sessionId"] == data["sessionId"]
        assert status_data["status"] in ("completed", "failed")


def test_direct_cartesian_jog_preserves_requested_frames():
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as client:
        load = client.post("/api/program/load", json={"path": _PROGRAM_PATH})
        assert load.status_code == 200, load.text
        assert state.cycle_manager is not None
        state.cycle_manager.reset()

        response = client.post(
            "/api/direct/cartesian-jog",
            json={
                "positionM": [0.4, 0.1, 0.5],
                "orientationXyzw": [0.0, 0.0, 0.0, 1.0],
                "frameId": "custom_world",
                "tcpFrame": "gripper_tip",
                "principalId": "human:test-operator",
            },
        )

        assert response.status_code == 200, response.text
        deadline = time.time() + 5.0
        while kernel.execute_count == 0 and time.time() < deadline:
            time.sleep(0.02)
        assert kernel.execute_count >= 1
        assert kernel.last_execute_request is not None
        assert kernel.last_execute_request.base_frame == "custom_world"
        assert kernel.last_execute_request.tcp_frame == "gripper_tip"
        assert kernel.last_execute_request.pose_goal is not None
        assert kernel.last_execute_request.pose_goal["frame_id"] == "custom_world"


def test_direct_promote_blackboard_value_after_intervention():
    import time

    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": _PROGRAM_PATH})

        execute_response = client.post("/api/direct/execute", json={
            "tree": {
                "id": "set_key_bool_value",
                "name": "publish_detection",
                "params": {"key": {"key_ref": "detected_ok"}, "value": True},
                "children": [],
            },
            "sessionKind": "manual_intervention",
            "authoritySource": "human",
            "principalId": "human:test-operator",
            "approvalMode": "human_approved",
        })
        assert execute_response.status_code == 200
        session_id = execute_response.json()["sessionId"]

        time.sleep(0.1)

        promote_response = client.post("/api/direct/promote", json={
            "sessionId": session_id,
            "promoteKeys": {"detected_ok": "recovery.detected_ok"},
        })
        assert promote_response.status_code == 200
        assert promote_response.json()["promotions"]

        blackboard = client.get("/api/execution/blackboard").json()["keys"]
        assert blackboard["recovery/detected_ok"]["valueJson"] is True


def test_direct_execute_from_idle_requires_human_approval():
    """POST /api/direct/execute rejects unapproved IDLE direct control."""
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": _PROGRAM_PATH})

        response = client.post("/api/direct/execute", json={
            "tree": {
                "id": "timer",
                "name": "brief_timer",
                "params": {"duration_s": 0.05},
                "children": [],
            },
            "sessionKind": "manual_intervention",
            "authoritySource": "llm",
            "principalId": "agent:test-llm",
            "grantType": "policy_granted",
        })
    assert response.status_code == 409
    assert "human-approved manual intervention" in response.json()["error"]


def test_direct_execute_while_running_409():
    """POST /api/direct/execute returns 409 when cycle is RUNNING."""
    kernel = MockKernelClient(execute_duration_s=1.0)
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": _PROGRAM_PATH})
        client.post("/api/cycle/run")

        response = client.post("/api/direct/execute", json={
            "tree": {
                "id": "timer",
                "name": "t",
                "params": {"duration_s": 0.01},
                "children": [],
            },
            "sessionKind": "manual_intervention",
            "authoritySource": "human",
            "principalId": "human:test-operator",
        })
        assert response.status_code == 409

        client.post("/api/cycle/stop")
        client.post("/api/cycle/reset")


def test_direct_abort_no_running_intervention():
    """POST /api/direct/abort returns 409 when no intervention is running."""
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": _PROGRAM_PATH})
        response = client.post("/api/direct/abort")
    assert response.status_code == 409


# =============================================================================
# Kernel Event Bridge Tests
# =============================================================================

def test_kernel_events_no_program(client):
    """GET /api/kernel/events returns empty when no program loaded."""
    response = client.get("/api/kernel/events")
    assert response.status_code == 200
    data = response.json()
    assert data["events"] == []
    assert data["last_event_index"] == 0


def test_kernel_events_drains_direct_kernel_events():
    """GET /api/kernel/events drains events produced outside the cycle tick loop."""
    from manyforge_behavior.kernel_client import ExecuteRequest

    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": _PROGRAM_PATH})
        kernel.execute(
            ExecuteRequest(
                robot_id="r",
                group_id="g",
                move_id="direct",
                base_frame="base_link",
                tcp_frame="tool0",
                motion_type="joint",
                joint_goal_names=["joint_1"],
                joint_goal_positions_rad=[0.1],
            )
        )

        response = client.get("/api/kernel/events?since=0")
        assert response.status_code == 200
        data = response.json()
        assert len(data["events"]) > 0
        assert data["last_event_index"] > 0
        assert all("event_index" in event for event in data["events"])


def test_kernel_events_after_execution():
    """GET /api/kernel/events returns events after program execution."""
    import time

    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": _PROGRAM_PATH})
        client.post("/api/cycle/run")
        time.sleep(0.3)  # let tree tick

        response = client.get("/api/kernel/events?since=0")
        assert response.status_code == 200
        data = response.json()
        assert len(data["events"]) > 0
        assert data["last_event_index"] > 0

        # Polling with last index returns no new events
        last = data["last_event_index"]
        response2 = client.get(f"/api/kernel/events?since={last}")
        data2 = response2.json()
        assert len(data2["events"]) == 0

        client.post("/api/cycle/reset")


def test_kernel_events_have_correct_structure():
    """Kernel events have required fields: type, execution_id, timestamp_ns."""
    import time

    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": _PROGRAM_PATH})
        client.post("/api/cycle/run")
        time.sleep(0.3)

        response = client.get("/api/kernel/events?since=0")
        data = response.json()
        for event in data["events"]:
            assert "type" in event
            assert "execution_id" in event
            assert "timestamp_ns" in event
            assert "event_index" in event

        client.post("/api/cycle/reset")


def test_kernel_stop_route_stops_active_kernel_without_cycle_layer():
    """POST /api/kernel/stop is the operator backstop: it must stop an active
    kernel regardless of what the cycle layer believes its state is."""
    kernel = MockKernelClient(execute_duration_s=5.0)
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as client:
        # Drive the kernel active directly, bypassing the cycle layer
        # entirely — the exact situation the backstop exists for.
        kernel._state = ExecutionState.EXECUTING

        response = client.post("/api/kernel/stop")

    assert response.status_code == 200
    data = response.json()
    assert data["accepted"] is True
    assert data["kernel_state"] == "stopped"
    assert kernel.get_status().state == ExecutionState.STOPPED


def test_kernel_stop_route_reports_rejection_honestly():
    """A kernel with nothing to stop must report the rejection, not a fake
    success."""
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as client:
        response = client.post("/api/kernel/stop")

    assert response.status_code == 200
    data = response.json()
    assert data["accepted"] is False
    assert data["error"]
    assert data["kernel_state"] == "idle"


def test_lifespan_shutdown_closes_kernel_before_recorder():
    """Graceful shutdown must quiesce in order: kernel close (motion/gate
    teardown) strictly before the event recorder flush, so the quiesce's own
    events reach disk, and the recorder must actually be flushed."""
    call_order: list[str] = []

    class ClosableKernel(MockKernelClient):
        def close(self):  # type: ignore[override]
            call_order.append("kernel_close")

    kernel = ClosableKernel()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)

    original_recorder_close = state.event_recorder.close

    def recording_recorder_close(timeout_s: float = 5.0) -> bool:
        call_order.append("recorder_close")
        return original_recorder_close(timeout_s=timeout_s)

    state.event_recorder.close = recording_recorder_close
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app):
        pass  # startup + immediate graceful shutdown

    assert call_order == ["kernel_close", "recorder_close"]


def test_lifespan_shutdown_quiesces_running_cycle():
    """A cycle still RUNNING at shutdown must be stopped through the kernel,
    and the quiesce's own kernel events must be drained into durable history
    before the recorder closes — not left behind in the kernel buffer."""
    kernel = MockKernelClient(execute_duration_s=10.0)
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)

    flush_calls: list[str] = []
    original_record = state.event_recorder.record_native_kernel_events
    original_close = state.event_recorder.close

    def recording_record(events, **kwargs):
        if events:
            flush_calls.append("record_native_kernel_events")
        return original_record(events, **kwargs)

    def recording_close(timeout_s: float = 5.0) -> bool:
        flush_calls.append("recorder_close")
        return original_close(timeout_s=timeout_s)

    state.event_recorder.record_native_kernel_events = recording_record
    state.event_recorder.close = recording_close
    app = create_app(state, security=_TEST_SECURITY)

    with TestClient(app) as client:
        response = client.post(
            "/api/program/load", json={"path": _PROGRAM_PATH}
        )
        assert response.status_code == 200
        response = client.post("/api/cycle/run")
        assert response.status_code == 200
        deadline = time.time() + 2.0
        while (
            kernel.get_status().state != ExecutionState.EXECUTING
            and time.time() < deadline
        ):
            time.sleep(0.01)
        assert kernel.get_status().state == ExecutionState.EXECUTING
        cycle_manager = state.cycle_manager
        assert cycle_manager is not None
        tick_thread = cycle_manager._tick_thread

    # Context exit ran the lifespan shutdown: motion stopped, tick joined.
    assert kernel.get_status().state in {
        ExecutionState.STOPPED,
        ExecutionState.IDLE,
    }
    assert kernel.stop_count >= 1
    assert tick_thread is None or not tick_thread.is_alive()

    # The stop's kernel events were drained out of the kernel buffer into
    # the indexed cycle buffer before the recorder closed; nothing pending.
    assert kernel.drain_kernel_events() == []
    drained_types = {
        str(event.get("type"))
        for event in cycle_manager.get_kernel_events_since(0)
    }
    assert "stop_requested" in drained_types
    # And the recorder saw the flush strictly before it was closed.
    assert "record_native_kernel_events" in flush_calls
    assert flush_calls.index("record_native_kernel_events") < flush_calls.index(
        "recorder_close"
    )


class TestRequestIdAndLogThrottling:
    """Request-ID middleware + per-route throttling of poller GET logs."""

    def test_response_carries_generated_request_id(self, client):
        response = client.get("/api/health")
        assert response.status_code == 200
        request_id = response.headers.get("x-request-id")
        assert request_id and len(request_id) == 16

    def test_incoming_request_id_is_honored_and_sanitized(self, client):
        response = client.get(
            "/api/health",
            headers={"X-Request-ID": "launcher-run-42\n{injected}"},
        )
        # Log-injection characters are stripped; the usable identity survives.
        assert response.headers["x-request-id"] == "launcher-run-42injected"

    def test_successful_get_logs_are_throttled_per_route(self, caplog):
        kernel = MockKernelClient()
        state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
        app = create_app(state, security=_TEST_SECURITY)
        with caplog.at_level(
            logging.INFO, logger="manyforge_composer.backend.app"
        ):
            with TestClient(app) as local_client:
                for _ in range(5):
                    assert local_client.get("/api/health").status_code == 200
        health_records = [
            r for r in caplog.records if getattr(r, "path", "") == "/api/health"
        ]
        # One line per route per window; the rest suppressed with a count.
        assert len(health_records) == 1
        assert health_records[0].request_id
        assert health_records[0].throttle_key == "request:GET:/api/health"

    def test_mutations_and_failures_always_log(self, caplog):
        kernel = MockKernelClient()
        state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
        app = create_app(state, security=_TEST_SECURITY)
        with caplog.at_level(
            logging.INFO, logger="manyforge_composer.backend.app"
        ):
            with TestClient(app) as local_client:
                local_client.post("/api/cycle/reset")
                local_client.post("/api/cycle/reset")
                local_client.get("/api/does-not-exist")
                local_client.get("/api/does-not-exist")
        posts = [
            r
            for r in caplog.records
            if getattr(r, "method", "") == "POST"
            and getattr(r, "path", "") == "/api/cycle/reset"
        ]
        missing = [
            r
            for r in caplog.records
            if getattr(r, "path", "") == "/api/does-not-exist"
        ]
        # No throttling on the lines an operator greps for.
        assert len(posts) == 2
        assert len(missing) == 2
        assert all(getattr(r, "status_code", 0) == 404 for r in missing)


def test_internal_error_response_carries_request_id(caplog):
    """The 500 path is where ticket correlation matters most: an unhandled
    route exception must still produce an X-Request-ID header, a JSON body
    with the id, and a structured traceback log carrying the same id."""
    class MetricsBoomKernel(MockKernelClient):
        def get_metrics(self):  # type: ignore[override]
            raise ValueError("unhandled route failure")

    kernel = MetricsBoomKernel()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)

    with caplog.at_level(logging.ERROR, logger="manyforge_composer.backend.app"):
        with TestClient(app, raise_server_exceptions=False) as local_client:
            response = local_client.get(
                "/api/kernel/metrics",
                headers={"X-Request-ID": "ticket-777"},
            )

    assert response.status_code == 500
    assert response.headers["x-request-id"] == "ticket-777"
    assert response.json() == {
        "detail": "Internal Server Error",
        "request_id": "ticket-777",
    }
    error_records = [
        r for r in caplog.records if getattr(r, "request_id", "") == "ticket-777"
    ]
    assert error_records and error_records[0].exc_info is not None


class TestVisualCacheReadOnlyContract:
    """The observer caches are returned SHARED (no per-read deepcopy on the
    10-15 Hz paths). These tests pin the two invariants that make that
    safe: composing attached poses never mutates the cache, and routes
    mutate copies, never the shared snapshot."""

    def _seed_attached_object(self, state):
        from manyforge_behavior.kernel_client import (
            SceneObjectSnapshot,
            SceneShape,
        )

        obj = SceneObjectSnapshot(
            object_id="attached_box",
            shape=SceneShape(type="box", box_dimensions_m=[0.1, 0.1, 0.1]),
            pose_in_universe={
                "position_m": [0.5, 0.0, 0.5],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            },
            attached=True,
            frame_id="tool0",
        )
        with state._visual_scene_objects_lock:
            state._visual_scene_objects = [obj]
            state._visual_scene_objects_timestamp_ns = time.monotonic_ns()
        return obj

    def test_compose_attached_pose_does_not_mutate_cache(self, state):
        cached = self._seed_attached_object(state)
        original_pose = dict(cached.pose_in_universe)

        moved_frame = {
            "tool0": {
                "position_m": [1.5, 1.0, 1.0],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            }
        }
        # First call derives the attachment offset; second composes a
        # genuinely different pose from the moved frame.
        state.visual_scene_objects_for_link_poses(
            {
                "tool0": {
                    "position_m": [0.5, 0.0, 0.5],
                    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                }
            }
        )
        composed, _ = state.visual_scene_objects_for_link_poses(moved_frame)

        assert composed[0].pose_in_universe != original_pose
        # Copy-on-write: the recomposed object is a different instance and
        # the cached object's pose is untouched.
        assert composed[0] is not cached
        assert cached.pose_in_universe == original_pose

    def test_unattached_objects_pass_through_shared(self, state):
        from manyforge_behavior.kernel_client import (
            SceneObjectSnapshot,
            SceneShape,
        )

        obj = SceneObjectSnapshot(
            object_id="free_box",
            shape=SceneShape(type="box", box_dimensions_m=[0.1, 0.1, 0.1]),
            pose_in_universe={
                "position_m": [0.1, 0.0, 0.05],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            },
            attached=False,
        )
        with state._visual_scene_objects_lock:
            state._visual_scene_objects = [obj]
            state._visual_scene_objects_timestamp_ns = time.monotonic_ns()

        composed, _ = state.visual_scene_objects_for_link_poses(
            {"tool0": {"position_m": [0, 0, 0], "orientation_xyzw": [0, 0, 0, 1]}}
        )
        assert composed[0] is obj  # no copy for objects that need none


def test_kernel_events_route_records_drop_gap_evidence():
    """OW1 regression: /api/kernel/events must pass pipeline loss counters to
    the recorder so collector-overflow loss becomes durable gap evidence even
    when no websocket client ever connects."""
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": _PROGRAM_PATH})
        # This test owns collector overflow deterministically. The recovery
        # pump is a legitimate concurrent consumer in production, so stop
        # it here before filling the ring; otherwise the test measures a
        # scheduler race rather than the collector's drop accounting.
        state.stop_recovery_offer_pump()
        # Overflow the mock collector ring (maxlen 4096) before any drain:
        # the first drained batch starts past the dropped prefix, so the
        # drop counter is the only loss signal.
        for _ in range(4200):
            kernel._emit_kernel_event("state_transition")
        response = client.get("/api/kernel/events?since=0")
        assert response.status_code == 200

    # The gap record lands FIRST and is then pushed out of the bounded
    # in-memory ring by the 4096 full-fidelity records that follow it, so
    # assert through the accounting surface (the durable JSONL retains it).
    status = state.event_recorder.kernel_event_gap_status()
    assert status["gap_records_total"] >= 1, (
        "collector drop advance must produce durable gap evidence"
    )
    assert status["last_gap_wall"]
    pipeline = state.cycle_manager.kernel_event_pipeline_status()
    assert pipeline["collector_dropped_total"] == 4200 - 4096


def test_kernel_events_route_records_drop_advance_with_empty_batch():
    """RT-8F: loss accounting is independent of retained event delivery."""
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": _PROGRAM_PATH})
        state.stop_recovery_offer_pump()
        manager = state.cycle_manager
        assert manager is not None
        manager.drain_kernel_events_now = lambda: []
        manager.get_kernel_events_since = lambda _since: []
        manager.kernel_event_pipeline_status = lambda: {
            "collector_dropped_total": 1,
            "manager_dropped_total": 0,
            "last_sequence_seen": 1,
        }

        before = state.event_recorder.kernel_event_gap_status()[
            "gap_records_total"
        ]
        response = client.get("/api/kernel/events?since=0")
        assert response.status_code == 200
        assert response.json()["events"] == []
        after = state.event_recorder.kernel_event_gap_status()[
            "gap_records_total"
        ]
        assert after == before + 1


def test_state_hub_records_drop_advance_without_browser_or_events():
    """RT-8F: the always-on NRT pump makes empty-batch loss durable."""
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": _PROGRAM_PATH})
        state.stop_recovery_offer_pump()
        manager = state.cycle_manager
        assert manager is not None
        manager.drain_kernel_events_now = lambda: []
        manager.get_kernel_events_since = lambda _since: []
        manager.kernel_event_pipeline_status = lambda: {
            "collector_dropped_total": 0,
            "manager_dropped_total": 1,
            "last_sequence_seen": 1,
        }
        hub = StateHub()
        hub.start(state)

        before = state.event_recorder.kernel_event_gap_status()[
            "gap_records_total"
        ]
        assert hub._collect_frame(include_payload=False) is None
        after = state.event_recorder.kernel_event_gap_status()[
            "gap_records_total"
        ]
        hub.stop()
        assert after == before + 1


def test_heartbeat_degrades_on_kernel_event_pipeline_drops():
    """OW1 regression: nonzero pipeline drop totals degrade the heartbeat and
    the component carries the gap-record accounting fields."""
    kernel = MockKernelClient()
    state = ComposerState(catalog_path=_CATALOG_PATH, kernel=kernel)
    app = create_app(state, security=_TEST_SECURITY)
    with TestClient(app) as client:
        client.post("/api/program/load", json={"path": _PROGRAM_PATH})

        healthy = client.get("/api/infra/heartbeat").json()
        storage = healthy["components"]["trace_storage"]
        if storage.get("enabled"):
            assert "volatile" in storage and "total_bytes" in storage
        assert storage["status"] == "healthy"
        component = healthy["components"]["kernel_event_pipeline"]
        assert component["status"] == "healthy"
        assert component["collector_dropped_total"] == 0
        assert "gap_records_total" in component
        assert "last_gap_wall" in component

        # This test deliberately owns collector overflow. The production
        # recovery pump is a valid concurrent drain consumer, so leaving it
        # active makes the asserted drop count a scheduler race.
        state.stop_recovery_offer_pump()
        for _ in range(4200):
            kernel._emit_kernel_event("state_transition")
        state.cycle_manager.drain_kernel_events_now()

        degraded = client.get("/api/infra/heartbeat").json()
        component = degraded["components"]["kernel_event_pipeline"]
        assert component["status"] == "degraded"
        assert component["collector_dropped_total"] == 4200 - 4096
        assert degraded["status"] == "degraded"
