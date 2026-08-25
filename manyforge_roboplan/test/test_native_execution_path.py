"""I6 process test across host, headless, Behavior, and Composer callers."""

from __future__ import annotations

import copy
import json
import math
import os
import shutil
import sqlite3
import subprocess
import threading
import time
from pathlib import Path
from typing import Any, Callable, Mapping

from fastapi import FastAPI
from fastapi.testclient import TestClient
import pytest

from manyforge_behavior.kernel_client import KernelClient
from manyforge_behavior.runner import OrchestratorRunner
from manyforge_composer.backend.routes_headless import create_headless_router
from manyforge_composer.backend.state import ComposerState
from manyforge_headless_api.app import TrustedPrincipal, create_app
from manyforge_headless_api.cclc import CCLCOpenRequestClient
from manyforge_headless_api.cell_admission import CellAdmissionClient
from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient
from manyforge_headless_api.execution import ExecutionCommandClient
from manyforge_headless_api.models import (
    OperationControlRequest,
    OperationIntentRequest,
)
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

    def plan_pick(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self.client.plan_pick(body)

    def plan_place(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self.client.plan_place(body)

    def start_pick(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self.client.start_pick(body)

    def start_place(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self.client.start_place(body)

    def pick_place_sequence(self, sequence_id: str) -> dict[str, Any]:
        return self.client.pick_place_sequence(sequence_id)

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


def _wait_for_cclc_terminal(
    read_status: Callable[[str], dict[str, Any] | None],
    transaction_id: str,
    *,
    label: str,
) -> dict[str, Any]:
    deadline = time.monotonic() + 10.0
    last_status: dict[str, Any] | None = None
    while time.monotonic() < deadline:
        status = read_status(transaction_id)
        last_status = status
        if status is not None and status.get("terminal_result") is not None:
            if status["terminal_result"] != "succeeded":
                raise RuntimeError(
                    f"native CCLC {label} failed: {status}"
                )
            return status
        time.sleep(0.01)
    raise RuntimeError(
        f"native CCLC {label} did not become terminal: {last_status}"
    )


def _expected_active_configuration(
    active: Mapping[str, Any] | None,
) -> dict[str, Any]:
    if active is None:
        return {"presence": "absent"}
    return {
        "presence": "present",
        "active_control_configuration_id": active[
            "active_control_configuration_id"
        ],
        "active_control_configuration_generation": active[
            "active_control_configuration_generation"
        ],
    }


def _qualify_native_cclc_product(
    execution_run_root: Path,
) -> None:
    client = CCLCOpenRequestClient(execution_run_root, timeout_seconds=5.0)
    admission = CellAdmissionClient(execution_run_root, timeout_seconds=5.0)

    def context(request_id: str, owner_epoch: str) -> dict[str, Any]:
        return {
            "request_id": request_id,
            "request_owner_epoch": owner_epoch,
            "source_label": "native-process-qualification",
        }

    for intent_kind in ("prepare", "enable"):
        owner_epoch = client.owner_identity()["request_owner_epoch"]
        snapshot = client.component_snapshot("lite6_arm")
        admission_generation = admission.get()["admission_generation"]
        expected_domains = [
            {
                "control_domain_id": domain["control_domain_id"],
                "control_domain_revision": domain[
                    "control_domain_revision"
                ],
                "controller_topology_id": domain[
                    "controller_topology_id"
                ],
                "topology_revision": domain["topology_revision"],
                "expected_active_control_configuration": (
                    _expected_active_configuration(
                        domain["active_control_configuration"]
                    )
                ),
                "expected_control_bindings": domain[
                    "current_control_bindings"
                ],
            }
            for domain in snapshot["domains"]
        ]
        result = client.submit_lifecycle(
            {
                "request_context": context(
                    f"process.lifecycle.{intent_kind}", owner_epoch
                ),
                "controllable_component_id": snapshot[
                    "controllable_component_id"
                ],
                "intent": intent_kind,
                "expected_graph_revision": snapshot["graph_revision"],
                "expected_component_lifecycle_generation": snapshot[
                    "component_lifecycle_generation"
                ],
                "expected_admission_generation": admission_generation,
                "expected_control_domains": expected_domains,
            }
        )
        acceptance = result.get("acceptance")
        if not isinstance(acceptance, dict):
            raise RuntimeError(
                f"native CCLC {intent_kind} was not accepted: {result}"
            )
        _wait_for_cclc_terminal(
            client.lifecycle_status,
            acceptance["lifecycle_transaction_id"],
            label=intent_kind,
        )

    _submit_native_cclc_routes(execution_run_root)


def _submit_native_cclc_routes(
    execution_run_root: Path,
    *,
    request_prefix: str = "process.route",
) -> None:
    client = CCLCOpenRequestClient(execution_run_root, timeout_seconds=5.0)

    def context(request_id: str, owner_epoch: str) -> dict[str, Any]:
        return {
            "request_id": request_id,
            "request_owner_epoch": owner_epoch,
            "source_label": "native-process-qualification",
        }

    template_set = client.route_templates("lite6_arm")
    domain_ids = sorted(
        row["desired_configuration"]["control_domain_id"]
        for row in template_set["templates"]
    )
    if domain_ids != ["lite6_arm_domain", "lite6_gripper_domain"]:
        raise RuntimeError(
            f"native CCLC product has an unexpected route set: {domain_ids}"
        )
    for domain_id in domain_ids:
        owner_epoch = client.owner_identity()["request_owner_epoch"]
        template_set = client.route_templates("lite6_arm")
        template = next(
            row
            for row in template_set["templates"]
            if row["desired_configuration"]["control_domain_id"]
            == domain_id
        )
        result = client.submit_route(
            {
                "request_context": context(
                    f"{request_prefix}.{domain_id}", owner_epoch
                ),
                "expected_retained_bundle_id": template_set[
                    "retained_bundle_id"
                ],
                "expected_graph_revision": template_set["graph_revision"],
                "desired_configuration": template[
                    "desired_configuration"
                ],
                "expected_active_control_configuration": template[
                    "expected_active_control_configuration"
                ],
                "component_expectations": template[
                    "component_expectations"
                ],
            }
        )
        acceptance = result.get("acceptance")
        if not isinstance(acceptance, dict):
            raise RuntimeError(
                f"native CCLC route {domain_id} was not accepted: {result}"
            )
        _wait_for_cclc_terminal(
            client.route_status,
            acceptance["route_selection_transaction_id"],
            label=f"route {domain_id}",
        )

    final_snapshot = client.component_snapshot("lite6_arm")
    if final_snapshot["component_lifecycle_state"] != "active" or any(
        domain["active_control_configuration"] is None
        for domain in final_snapshot["domains"]
    ):
        raise RuntimeError(
            f"native CCLC product did not publish complete readiness: {final_snapshot}"
        )


def _wait_for_cclc_component(execution_run_root: Path) -> dict[str, Any]:
    client = CCLCOpenRequestClient(execution_run_root, timeout_seconds=1.0)
    deadline = time.monotonic() + 10.0
    last_error = ""
    while time.monotonic() < deadline:
        try:
            return client.component_snapshot("lite6_arm")
        except HeadlessApiError as error:
            last_error = f"{error.code}: {error}"
        time.sleep(0.01)
    raise RuntimeError(f"native CCLC socket did not become ready: {last_error}")


def _start_host(
    fixture_root: Path,
    execution_state_root: Path,
    execution_run_root: Path,
    manifest: dict[str, Any],
    *,
    candidate_worker: Path | None = None,
    fake_worker_marker: Path | None = None,
    reconciliation_test_marker: Path | None = None,
    reconciliation_test_gate: Path | None = None,
    reconciliation_test_throw: str | None = None,
    host_environment: str = "MANYFORGE_NATIVE_EXECUTION_HOST",
    qualify_cclc: bool = True,
) -> subprocess.Popen[bytes]:
    environment = os.environ.copy()
    if fake_worker_marker is not None:
        environment["MANYFORGE_FAKE_WORKER_MARKER"] = str(fake_worker_marker)
    if reconciliation_test_marker is not None:
        environment["MANYFORGE_RECONCILIATION_TEST_MARKER"] = str(
            reconciliation_test_marker
        )
    if reconciliation_test_gate is not None:
        environment["MANYFORGE_RECONCILIATION_TEST_GATE"] = str(
            reconciliation_test_gate
        )
    if reconciliation_test_throw is not None:
        environment["MANYFORGE_RECONCILIATION_TEST_THROW"] = (
            reconciliation_test_throw
        )
    process = subprocess.Popen(
        [
            str(_required_path(host_environment)),
            "--package-root",
            str(fixture_root / "retained-package"),
            "--manager-state-root",
            str(fixture_root / "manager-state"),
            "--execution-state-root",
            str(execution_state_root),
            "--execution-run-root",
            str(execution_run_root),
            "--program-binding-id",
            manifest["program_binding_id"],
            "--motion-target-role-id",
            manifest["target_role_id"],
            "--gripper-target-role-id",
            manifest["gripper_target_role_id"],
            "--candidate-worker",
            str(
                candidate_worker
                or _required_path("MANYFORGE_ROBOPLAN_CANDIDATE_WORKER")
            ),
            "--cell-id",
            manifest["cell_id"],
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        env=environment,
    )
    client = ExecutionCommandClient(execution_run_root, timeout_seconds=2.0)
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
                if manifest.get("cclc_product") is True and qualify_cclc:
                    try:
                        _qualify_native_cclc_product(
                            execution_run_root,
                        )
                    except Exception as error:
                        _stop_host(process)
                        detail = (
                            process.stderr.read() if process.stderr else b""
                        ).decode(errors="replace")
                        raise RuntimeError(
                            "native CCLC product qualification failed: "
                            f"{error}; host stderr: {detail}"
                        ) from error
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
            # The production owner drains independent StopControl,
            # participant-attempt, projection, surveillance, and scheduler
            # lanes before writing its clean-epoch marker.  They normally
            # quiesce immediately, but the launcher timeout must cover that
            # complete bounded shutdown rather than killing the process in
            # the middle of durable close.
            process.wait(timeout=15.0)
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


def _gripper_submission(
    runner: OrchestratorRunner,
    *,
    request_id: str,
    position_kind: str,
    position: float,
    command_duration_s: float = 0.05,
) -> dict[str, Any]:
    return runner.submit_gripper_position(
        request_id=request_id,
        program_run_id="n5-program-run-1",
        component_role_id="robot",
        target_role_id="gripper",
        node_instance_id="root/native-gripper[0]",
        node_attempt_id=f"n5-program-run-1/{request_id}",
        position_kind=position_kind,  # type: ignore[arg-type]
        position=position,
        command_duration_s=command_duration_s,
        completion_timeout_s=max(2.0, command_duration_s + 1.0),
        correlation_id="n5-process-test",
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


def _run_native_benchmark_move(
    client: ExecutionCommandClient,
    *,
    request_id: str,
    positions_rad: list[float],
    cache_mode: str,
    move_id: str | None = None,
) -> float:
    """Measure one complete addressed plan/execute/release cycle."""

    status = client.status()["status"]
    operation = _direct_control_joint_operation(
        status,
        request_id=request_id,
        positions_rad=positions_rad,
    )
    payload = operation["participants"][0]["payload"]
    payload["cache_mode"] = cache_mode
    if move_id is not None:
        payload["move_id"] = move_id
    request = OperationIntentRequest.model_validate(operation)

    started_at = time.perf_counter()
    accepted = client.submit(request, trusted_principal_id=None)
    assert accepted["ok"] is True, accepted
    operation_id = accepted["accepted"]["operation_id"]
    deadline = time.monotonic() + 5.0
    terminal: dict[str, Any] = {}
    while time.monotonic() < deadline:
        terminal = client.read(operation_id)
        projection = terminal.get("operation")
        if isinstance(projection, dict) and projection.get("state") in TERMINAL_STATES:
            break
        time.sleep(0.005)
    elapsed_seconds = time.perf_counter() - started_at
    projection = terminal.get("operation")
    assert isinstance(projection, dict), terminal
    assert projection["state"] == "completed", terminal

    acknowledgment = OperationControlRequest.model_validate(
        {
            "request_context": {
                "request_id": f"ack-{request_id}",
                "request_owner_epoch": status["owner_epoch"],
                "source_label": "n7_b_native_benchmark",
                "correlation_id": "n7-b-historical-benchmark",
            },
            "operation_id": operation_id,
            "expected_operation_revision": projection["operation_revision"],
            "reason": "release completed N7 benchmark operation",
        }
    )
    settlement_deadline = time.monotonic() + 5.0
    while True:
        released = client.acknowledge_completed(
            acknowledgment,
            trusted_principal_id=None,
        )
        if released.get("ok") is True:
            break
        error = released.get("error")
        details = error.get("details") if isinstance(error, dict) else None
        if (
            not isinstance(details, dict)
            or details.get("reason") != "operation_effect_in_flight"
            or time.monotonic() >= settlement_deadline
        ):
            break
        time.sleep(0.005)
    assert released["ok"] is True, released

    # Control acceptance is asynchronous. Keep retirement outside the
    # historical timing sample, but do not start another sample until the
    # Kernel's authoritative projection confirms the operation is no longer
    # resident. Admission and recovery remain separate backend facts.
    residency_projection: dict[str, Any] = {}
    residency_deadline = time.monotonic() + 5.0
    while True:
        residency_projection = client.read(operation_id)
        if residency_projection.get("resident") is False:
            break
        if time.monotonic() >= residency_deadline:
            raise AssertionError(
                "native execution remained resident after acknowledgement: "
                f"{residency_projection}"
            )
        time.sleep(0.005)
    return elapsed_seconds


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


def _recover_active_stop_fault(
    client: HeadlessHttpClient,
    *,
    owner_epoch: str,
    request_prefix: str,
    correlation_id: str,
    reason: str,
    timeout_seconds: float = 10.0,
) -> dict[str, Any] | None:
    deadline = time.monotonic() + timeout_seconds
    last_result: dict[str, Any] | None = None
    attempt = 0
    while time.monotonic() < deadline:
        offer = client.stop_fault()["stop_fault"]
        if offer is None:
            return last_result
        latch = offer["latch"]
        certificate = latch["stop_certificate"]
        attempt += 1
        last_result = client.recover_stop_fault(
            {
                "request_context": {
                    "request_id": f"{request_prefix}-{attempt}",
                    "request_owner_epoch": owner_epoch,
                    "source_label": "headless_process_test",
                    "correlation_id": correlation_id,
                },
                "expected_latch_id": latch["latch_id"],
                "expected_latch_generation": latch["latch_generation"],
                "expected_recovery_assessment_revision": latch[
                    "recovery_assessment_revision"
                ],
                "expected_stop_certificate_id": certificate[
                    "stop_certificate_id"
                ],
                "expected_stop_certificate_revision": certificate[
                    "stop_certificate_revision"
                ],
                "expected_cell_id": latch["cell_id"],
                "expected_retained_bundle_id": latch["retained_bundle_id"],
                "expected_graph_revision": latch["graph_revision"],
                "configuration_expectations": offer[
                    "configuration_expectations"
                ],
                "reason": reason,
            }
        )["result"]
        if last_result["accepted"]:
            return last_result
        time.sleep(0.05)
    raise RuntimeError(f"Stop fault did not become recoverable: {last_result}")


_PLANNING_SCENE_REVISION = {
    "scene_id": "sha256:" + "1" * 64,
    "revision_id": 1,
    "content_hash": "sha256:" + "2" * 64,
}


def _pose(x_m: float = 0.0, y_m: float = 0.0, z_m: float = 0.0) -> dict[str, Any]:
    return {
        "position_m": {"x_m": x_m, "y_m": y_m, "z_m": z_m},
        "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
    }


def _translation_error_m(
    left: Mapping[str, Any], right: Mapping[str, Any]
) -> float:
    return math.sqrt(
        sum(
            (float(left[axis]) - float(right[axis])) ** 2
            for axis in ("x_m", "y_m", "z_m")
        )
    )


def _rotation_error_rad(
    left: Mapping[str, Any], right: Mapping[str, Any]
) -> float:
    dot = abs(
        sum(
            float(left[axis]) * float(right[axis])
            for axis in ("x", "y", "z", "w")
        )
    )
    return 2.0 * math.acos(max(-1.0, min(1.0, dot)))


def _planning_motion(
    move_id: str,
    motion: dict[str, Any],
    *,
    contact_policy_id: str | None = None,
    selected_object_id: str | None = None,
) -> dict[str, Any]:
    runtime_objects = (
        [
            {
                "runtime_object_id": "selected_object",
                "scene_resource_id": selected_object_id,
            }
        ]
        if selected_object_id is not None
        else []
    )
    return {
        "kind": "motion",
        "move_id": move_id,
        "base_frame_role_id": "frame-role:base",
        "tcp_frame_role_id": "frame-role:tcp",
        "tolerances": {
            "joint_position_tol_rad": 0.01,
            "translation_tol_m": 0.001,
            "rotation_tol_rad": 0.01,
        },
        "speed_profile": {
            "velocity_scale": 0.5,
            "acceleration_scale": 0.5,
            "max_tcp_linear_speed_mps": 0.1,
        },
        "timeouts": {"planning_timeout_s": 1.0, "execution_timeout_s": 2.0},
        "cache_mode": "default",
        "planning_profile": "first_feasible",
        "target_mode": "absolute",
        "target_reference": None,
        "offset_reference": None,
        "motion": motion,
        "contact_policy_id": contact_policy_id,
        "per_joint_dynamics_limits": {},
        "motion_interrupt_scopes": [],
        "runtime_object_bindings": runtime_objects,
    }


def _planning_operation(
    request_id: str,
    payload: dict[str, Any],
    *,
    tool: bool = False,
) -> dict[str, Any]:
    return {
        "request_context": {
            "request_id": request_id,
            "request_owner_epoch": "kernel.epoch:test",
            "source_label": "n5d-process-test",
            "correlation_id": "n5d-process-correlation",
        },
        "program_id": "program:test",
        "program_revision": "program-revision:test",
        "program_binding_id": "program-binding:test",
        "program_run_id": "run:test",
        "expected_graph_revision": "graph:test",
        "expected_retained_bundle_id": "bundle:test",
        "expected_scene_revision": _PLANNING_SCENE_REVISION,
        "participants": [
            {
                "component_role_id": (
                    "component-role:tool" if tool else "component-role:arm"
                ),
                "target_role_id": (
                    "target-role:tool" if tool else "target-role:test"
                ),
                "required_capability_id": (
                    "tool.gripper.command"
                    if tool
                    else "execution.trajectory.execute"
                ),
                "action_span": None,
                "payload": payload,
            }
        ],
        "coordination_policy": "independent",
    }


def _direct_control_joint_operation(
    status: dict[str, Any],
    *,
    request_id: str,
    positions_rad: list[float],
) -> dict[str, Any]:
    direct = status["direct_control"]
    joint_names = [joint["source_joint_name"] for joint in direct["joints"]]
    assert len(joint_names) == len(positions_rad)
    return {
        "request_context": {
            "request_id": request_id,
            "request_owner_epoch": status["owner_epoch"],
            "source_label": "composer_ui",
            "correlation_id": None,
        },
        "program_id": direct["program_id"],
        "program_revision": direct["program_revision"],
        "program_binding_id": direct["program_binding_id"],
        "program_run_id": direct["program_run_id"],
        "expected_graph_revision": status["graph_revision"],
        "expected_retained_bundle_id": status["retained_bundle_id"],
        "expected_scene_revision": status["scene_revision"],
        "participants": [
            {
                "component_role_id": direct["component_role_id"],
                "target_role_id": direct["target_role_id"],
                "required_capability_id": direct[
                    "operation_capability_id"
                ],
                "action_span": None,
                "payload": _planning_motion(
                    f"robot-control-{request_id}",
                    {
                        "kind": "joint_goal",
                        "joint_names": joint_names,
                        "positions_rad": positions_rad,
                    },
                )
                | {
                    "base_frame_role_id": direct["base_frame_role_id"],
                    "tcp_frame_role_id": direct["tcp_frame_role_id"],
                    "planning_profile": "balanced",
                    "speed_profile": {
                        "velocity_scale": 1.0,
                        "acceleration_scale": 1.0,
                        "max_tcp_linear_speed_mps": None,
                    },
                },
            }
        ],
        "coordination_policy": "independent",
    }


def _direct_control_gripper_operation(
    status: dict[str, Any], *, request_id: str, position: float
) -> dict[str, Any]:
    direct = status["direct_control_gripper"]
    position_value = (
        {"kind": "angular_rad", "value_rad": position}
        if direct["resource"]["coordinate_kind"] == "angular_rad"
        else {"kind": "linear_m", "value_m": position}
    )
    return {
        "request_context": {
            "request_id": request_id,
            "request_owner_epoch": status["owner_epoch"],
            "source_label": "composer_ui",
            "correlation_id": None,
        },
        "program_id": direct["program_id"],
        "program_revision": direct["program_revision"],
        "program_binding_id": direct["program_binding_id"],
        "program_run_id": direct["program_run_id"],
        "expected_graph_revision": status["graph_revision"],
        "expected_retained_bundle_id": status["retained_bundle_id"],
        "expected_scene_revision": status["scene_revision"],
        "participants": [
            {
                "component_role_id": direct["component_role_id"],
                "target_role_id": direct["target_role_id"],
                "required_capability_id": direct[
                    "operation_capability_id"
                ],
                "action_span": None,
                "payload": {
                    "kind": "capability_command",
                    "command_schema_id": direct["command_schema_id"],
                    "command_schema_revision": direct[
                        "command_schema_revision"
                    ],
                    "command_value": {
                        "kind": "gripper_position",
                        "position": position_value,
                        "max_effort": None,
                    },
                    "completion_timeout_s": 2.0,
                    "caller_options": [
                        {
                            "option_id": "command_duration_s",
                            "value_s": 0.5,
                        }
                    ],
                },
            }
        ],
        "coordination_policy": "independent",
    }


def _direct_control_motion_operation(
    status: dict[str, Any], *, request_id: str, motion: dict[str, Any]
) -> dict[str, Any]:
    operation = _direct_control_joint_operation(
        status,
        request_id=request_id,
        positions_rad=[
            float(value)
            for value in status["direct_control"]["observed_state"][
                "positions_rad"
            ]
        ],
    )
    payload = operation["participants"][0]["payload"]
    payload["motion"] = motion
    payload["speed_profile"]["max_tcp_linear_speed_mps"] = 0.1
    return operation


def _tool_payload(position_m: float) -> dict[str, Any]:
    return {
        "kind": "capability_command",
        "command_schema_id": "gripper.command_position",
        "command_schema_revision": "gripper-command-revision:test",
        "command_value": {
            "kind": "gripper_position",
            "position": {"kind": "linear_m", "value_m": position_m},
            "max_effort": {"kind": "linear_newton", "value_n": 8.0},
        },
        "completion_timeout_s": 2.0,
        "caller_options": [],
    }


def _cartesian_path(start_z_m: float, end_z_m: float) -> dict[str, Any]:
    return {
        "kind": "cartesian_path",
        "waypoints": [_pose(z_m=start_z_m), _pose(z_m=end_z_m)],
        "max_translation_step_m": 0.01,
        "max_rotation_step_rad": 0.1,
        "speed_mode": "bounded",
        "max_linear_speed_mps": 0.1,
        "max_angular_speed_rad_s": 0.2,
        "max_linear_acceleration_mps2": 0.3,
        "max_angular_acceleration_rad_s2": 0.4,
        "fallback_policy": "fail_closed",
    }


def _pick_planning_request() -> dict[str, Any]:
    return {
        "sequence_id": "process-pick-1",
        "selected_object_id": "workpiece",
        "selected_runtime_object_id": "selected_object",
        "carrier_frame_role_id": "frame-role:tcp",
        "synthesis": {
            "pick_offset_envelope": {
                "position_min_m": [0.0, 0.0, 0.0],
                "position_max_m": [0.0, 0.0, 0.0],
                "orientation_rpy_min_deg": [0.0, 0.0, 0.0],
                "orientation_rpy_max_deg": [0.0, 0.0, 0.0],
            },
            "approach_offset_envelope": {
                "position_min_m": [0.0, 0.0, 0.05],
                "position_max_m": [0.0, 0.0, 0.05],
                "orientation_rpy_min_deg": [0.0, 0.0, 0.0],
                "orientation_rpy_max_deg": [0.0, 0.0, 0.0],
            },
            "pick_offset_reference": {
                "type": "scene_resource",
                "scene_resource_id": "workpiece",
            },
            "approach_offset_reference": {
                "type": "frame_role",
                "frame_role_id": "frame-role:tcp",
            },
            "base_tool_orientation_rpy_deg": [180.0, 0.0, 0.0],
            "translation_step_m": 0.01,
            "rotation_step_deg": 15.0,
            "force_vertical_pick": False,
            "min_tcp_z_m": None,
            "maximum_candidate_count": 8,
            "search_strategy": "first_feasible",
        },
        "approach_operation": _planning_operation(
            "process-pick-approach",
            _planning_motion(
                "pick.approach", {"kind": "pose_goal", "goal": _pose()}
            ),
        ),
        "descend_operation": _planning_operation(
            "process-pick-descend",
            _planning_motion(
                "pick.descend",
                _cartesian_path(0.05, 0.0),
                contact_policy_id="pick.contact",
                selected_object_id="workpiece",
            ),
        ),
        "tool_operation": _planning_operation(
            "process-pick-tool", _tool_payload(0.0), tool=True
        ),
        "retreat_operation": _planning_operation(
            "process-pick-retreat",
            _planning_motion("pick.retreat", _cartesian_path(0.0, 0.05)),
        ),
    }


def _place_planning_request() -> dict[str, Any]:
    return {
        "sequence_id": "process-place-1",
        "selected_object_id": "carried-workpiece",
        "carrier_frame_role_id": "frame-role:tcp",
        "approach_operation": _planning_operation(
            "process-place-approach",
            _planning_motion(
                "place.approach",
                {"kind": "pose_goal", "goal": _pose(z_m=0.3)},
            ),
        ),
        "descend_operation": _planning_operation(
            "process-place-descend",
            _planning_motion("place.descend", _cartesian_path(0.3, 0.2)),
        ),
        "tool_operation": _planning_operation(
            "process-place-tool", _tool_payload(0.04), tool=True
        ),
        "retreat_operation": _planning_operation(
            "process-place-retreat",
            _planning_motion("place.retreat", _cartesian_path(0.2, 0.3)),
        ),
    }


def test_production_standalone_cclc_remains_closed_without_manager_liveness(
    tmp_path: Path,
) -> None:
    built_fixture = _required_path("MANYFORGE_I6_NATIVE_FIXTURE_ROOT")
    fixture_root = tmp_path / "fixture"
    shutil.copytree(built_fixture, fixture_root)
    manifest = json.loads((fixture_root / "manifest.json").read_text("utf-8"))
    execution_state_root = tmp_path / "execution-state"
    execution_run_root = tmp_path / "execution-run"
    execution_state_root.mkdir()
    execution_run_root.mkdir()

    process = _start_host(
        fixture_root,
        execution_state_root,
        execution_run_root,
        manifest,
        host_environment="MANYFORGE_NATIVE_PRODUCTION_EXECUTION_HOST",
        qualify_cclc=False,
    )
    try:
        admission = CellAdmissionClient(
            execution_run_root, timeout_seconds=5.0
        ).get()
        assert admission["authoring_allowed"] is False
        before = _wait_for_cclc_component(execution_run_root)
        with pytest.raises(HeadlessApiError) as rejected:
            _qualify_native_cclc_product(execution_run_root)
        assert rejected.value.code == "authoring_not_ready"
        assert _wait_for_cclc_component(execution_run_root) == before
    finally:
        _stop_host(process)


def test_reconciliation_gate_does_not_block_ordinary_accept_or_exact_read(
    tmp_path: Path,
) -> None:
    built_fixture = _required_path("MANYFORGE_I6_NATIVE_FIXTURE_ROOT")
    fixture_root = tmp_path / "fixture"
    shutil.copytree(built_fixture, fixture_root)
    manifest = json.loads((fixture_root / "manifest.json").read_text("utf-8"))
    execution_state_root = tmp_path / "execution-state"
    execution_run_root = tmp_path / "execution-run"
    reconciliation_marker = tmp_path / "reconciliation-entered"
    reconciliation_gate = tmp_path / "release-reconciliation"
    execution_state_root.mkdir()
    execution_run_root.mkdir()

    process = _start_host(
        fixture_root,
        execution_state_root,
        execution_run_root,
        manifest,
        host_environment="MANYFORGE_NATIVE_PICK_PLACE_FIXTURE_HOST",
        reconciliation_test_marker=reconciliation_marker,
        reconciliation_test_gate=reconciliation_gate,
    )
    native = ExecutionCommandClient(execution_run_root, timeout_seconds=0.5)
    raw_api = TestClient(
        create_app(
            service=HeadlessService(
                UnusedCore(),  # type: ignore[arg-type]
                UnusedManager(),  # type: ignore[arg-type]
                native,
            ),
            trusted_principal_adapter=FixedTrustedPrincipalAdapter(),
        )
    )
    headless = HeadlessHttpClient("http://headless.test", client=raw_api)
    runner = OrchestratorRunner(KernelClient(RecordingTransport(headless)))

    try:
        deadline = time.monotonic() + 2.0
        while not reconciliation_marker.exists() and time.monotonic() < deadline:
            time.sleep(0.005)
        assert reconciliation_marker.exists()
        assert not reconciliation_gate.exists()

        status = headless.execution_status()
        accepted = _runner_submission(
            runner,
            request_id="accept-while-reconciliation-gated",
            joint_names=("joint1",),
            positions=(0.1,),
        )
        operation_id = accepted["accepted"]["operation_id"]
        observed = runner.operation(operation_id)

        assert status["ok"] is True
        assert observed["operation"]["operation_id"] == operation_id
        assert observed["operation"]["state"] in {"planning", "executing"}
        assert not reconciliation_gate.exists()

        reconciliation_gate.touch()
        terminal = _wait_terminal(runner, operation_id)["operation"]
        assert terminal["state"] == "completed"
    finally:
        _stop_host(process)
        raw_api.close()


def test_reconciliation_exception_uses_caught_host_failure_path(
    tmp_path: Path,
) -> None:
    built_fixture = _required_path("MANYFORGE_I6_NATIVE_FIXTURE_ROOT")
    fixture_root = tmp_path / "fixture"
    shutil.copytree(built_fixture, fixture_root)
    manifest = json.loads((fixture_root / "manifest.json").read_text("utf-8"))
    execution_state_root = tmp_path / "execution-state"
    execution_run_root = tmp_path / "execution-run"
    reconciliation_marker = tmp_path / "reconciliation-entered"
    reconciliation_gate = tmp_path / "release-reconciliation"
    injected_failure = "fixture injected reconciliation failure"
    execution_state_root.mkdir()
    execution_run_root.mkdir()

    process = _start_host(
        fixture_root,
        execution_state_root,
        execution_run_root,
        manifest,
        host_environment="MANYFORGE_NATIVE_PICK_PLACE_FIXTURE_HOST",
        reconciliation_test_marker=reconciliation_marker,
        reconciliation_test_gate=reconciliation_gate,
        reconciliation_test_throw=injected_failure,
    )

    try:
        deadline = time.monotonic() + 2.0
        while not reconciliation_marker.exists() and time.monotonic() < deadline:
            time.sleep(0.005)
        assert reconciliation_marker.exists()

        reconciliation_gate.touch()
        assert process.wait(timeout=5.0) == 70
        stderr = (process.stderr.read() if process.stderr else b"").decode(
            errors="replace"
        )

        assert stderr == f"{injected_failure}\n"
        assert process.poll() == 70
        assert not (execution_run_root / "execution-command.sock").exists()
        assert not (execution_run_root / "execution-control.sock").exists()
    finally:
        _stop_host(process)


def test_pick_place_planning_crosses_real_cpp_owner_and_thin_clients(
    tmp_path: Path,
) -> None:
    built_fixture = _required_path("MANYFORGE_I6_NATIVE_FIXTURE_ROOT")
    fixture_root = tmp_path / "fixture"
    shutil.copytree(built_fixture, fixture_root)
    manifest = json.loads((fixture_root / "manifest.json").read_text("utf-8"))
    execution_state_root = tmp_path / "execution-state"
    execution_run_root = tmp_path / "execution-run"
    execution_state_root.mkdir()
    execution_run_root.mkdir()

    process = _start_host(
        fixture_root,
        execution_state_root,
        execution_run_root,
        manifest,
        host_environment="MANYFORGE_NATIVE_PICK_PLACE_FIXTURE_HOST",
    )
    native = ExecutionCommandClient(execution_run_root, timeout_seconds=10.0)
    raw_api = TestClient(
        create_app(
            service=HeadlessService(
                UnusedCore(),  # type: ignore[arg-type]
                UnusedManager(),  # type: ignore[arg-type]
                native,
            ),
            trusted_principal_adapter=FixedTrustedPrincipalAdapter(),
        )
    )
    headless = HeadlessHttpClient("http://headless.test", client=raw_api)
    behavior = KernelClient(RecordingTransport(headless))
    authoring_root = tmp_path / "composer-authoring"
    authoring_root.mkdir()
    composer_app = FastAPI()
    composer_app.include_router(
        create_headless_router(
            headless,
            ComposerState(
                headless_client=headless,
                authoring_state_root=str(authoring_root),
            ),
        )
    )
    raw_composer = TestClient(composer_app)
    composer = HeadlessHttpClient("http://composer.test", client=raw_composer)

    try:
        owner_epoch = native.status()["status"]["owner_epoch"]
        before = native.events(owner_epoch, 0)["events"]

        pick = behavior.plan_pick(_pick_planning_request())["planning_result"]
        assert pick["schema_version"] == "manyforge.pick_place_planning_result.v1"
        assert pick["sequence_id"] == "process-pick-1"
        assert pick["kind"] == "pick"
        assert pick["selected_object_id"] == "workpiece"
        assert pick["status"] == "planned"
        assert pick["attempted_candidate_count"] == 1
        assert pick["last_attempted_candidate"]["candidate_index"] == 1
        assert pick["last_attempted_candidate"]["pick_cartesian_path"][
            "kind"
        ] == "cartesian_path"
        assert pick["plan"]["approach_motion"]["kind"] == "pose_goal"
        assert pick["plan"]["descend_motion"]["kind"] == "cartesian_path"
        assert pick["plan"]["retreat_motion"]["kind"] == "cartesian_path"

        place = composer.plan_place(_place_planning_request())[
            "planning_result"
        ]
        assert place["sequence_id"] == "process-place-1"
        assert place["kind"] == "place"
        assert place["selected_object_id"] == "carried-workpiece"
        assert place["status"] == "planned"
        assert place["attempted_candidate_count"] == 1
        assert place["last_attempted_candidate"] is None
        assert place["plan"]["approach_motion"]["kind"] == "pose_goal"
        assert place["plan"]["descend_motion"]["kind"] == "cartesian_path"
        assert place["plan"]["retreat_motion"]["kind"] == "cartesian_path"

        infeasible_request = _pick_planning_request()
        infeasible_request["approach_operation"]["participants"][0][
            "payload"
        ]["move_id"] = "pick.fixture_no_feasible.approach"
        infeasible = behavior.start_pick(infeasible_request)
        assert infeasible["planning_result"]["status"] == (
            "no_feasible_candidate"
        )
        assert infeasible["planning_result"]["attempted_candidate_count"] > 0
        assert infeasible["planning_result"]["last_attempted_candidate"]
        assert infeasible["sequence_status"] is None
        assert native.events(owner_epoch, 0)["events"] == before

        execution_request = _pick_planning_request()
        execution_request["retreat_operation"]["participants"][0]["payload"][
            "move_id"
        ] = "pick.fixture_fail.retreat"
        started = behavior.start_pick(execution_request)
        assert started["planning_result"]["status"] == "planned"
        assert started["sequence_status"]["phase"] == "approach"

        deadline = time.monotonic() + 5.0
        observed = started
        while observed["sequence_status"]["phase"] not in {
            "completed",
            "failed",
            "intervention_required",
        }:
            assert time.monotonic() < deadline
            time.sleep(0.01)
            observed = behavior.pick_place_sequence("process-pick-1")
        status = observed["sequence_status"]
        # The retreat operation failed after the attach was durably committed.
        # Scene truth is therefore known (the object remains attached), so this
        # is an ordinary failed sequence rather than an uncertainty that needs
        # intervention.
        assert status["phase"] == "failed"
        assert status["committed_projection_revision"] is not None
        assert status["terminal_error"]["details"]["reason"] == (
            "pick_place_phase_operation_failed"
        )
        assert composer.pick_place_sequence("process-pick-1")[
            "sequence_status"
        ] == status

        assert native.events(owner_epoch, 0)["events"] == before
    finally:
        _stop_host(process)
        raw_composer.close()
        raw_api.close()


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
    execution_state_root = tmp_path / "execution-state"
    execution_run_root = tmp_path / "execution-run"
    execution_state_root.mkdir()
    execution_run_root.mkdir()

    process = _start_host(
        fixture_root, execution_state_root, execution_run_root, manifest
    )
    raw_api = TestClient(
        create_app(
            service=HeadlessService(
                UnusedCore(),  # type: ignore[arg-type]
                UnusedManager(),  # type: ignore[arg-type]
                ExecutionCommandClient(execution_run_root, timeout_seconds=10.0),
            ),
            trusted_principal_adapter=FixedTrustedPrincipalAdapter(),
        )
    )
    headless = HeadlessHttpClient("http://headless.test", client=raw_api)
    recording = RecordingTransport(headless)
    runner = OrchestratorRunner(KernelClient(recording))

    authoring_root = tmp_path / "composer-authoring"
    authoring_root.mkdir()
    composer_app = FastAPI()
    composer_app.include_router(
        create_headless_router(
            headless,
            ComposerState(
                headless_client=headless,
                authoring_state_root=str(authoring_root),
            ),
        )
    )
    raw_composer = TestClient(composer_app)
    composer = HeadlessHttpClient("http://composer.test", client=raw_composer)

    try:
        with pytest.raises(HeadlessClientError) as unavailable_start:
            headless.start_pick(_pick_planning_request())
        assert unavailable_start.value.code == (
            "pick_place_execution_owner_unavailable"
        )
        with pytest.raises(HeadlessClientError) as unavailable_status:
            composer.pick_place_sequence("not-started")
        assert unavailable_status.value.code == (
            "pick_place_execution_owner_unavailable"
        )

        first_status = headless.execution_status()["status"]
        assert first_status["program_binding_id"] == manifest["program_binding_id"]
        assert first_status["retained_bundle_id"] == manifest["retained_bundle_id"]
        assert first_status["gripper_target_role_id"] == "gripper"
        assert first_status["gripper_command_schema_id"] == manifest[
            "gripper_command_schema_id"
        ]
        assert first_status["gripper_command_schema_revision"] == manifest[
            "gripper_command_schema_revision"
        ]
        observed = {
            target["target_role_id"]: target
            for target in first_status["observed_targets"]
        }
        assert set(observed) == {"arm", "gripper"}
        assert all(
            position["kind"] == "angular_rad"
            for position in observed["arm"]["positions"]
        )
        assert observed["gripper"]["positions"] == [
            {"kind": "linear_m", "value_m": 0.0}
        ]
        assert observed["arm"]["resources"] != observed["gripper"]["resources"]

        direct = first_status["direct_control"]
        assert direct["program_id"] == "manyforge.direct_control.motion"
        assert direct["program_binding_id"] != manifest["program_binding_id"]
        assert direct["controllable_component_id"] == observed["arm"][
            "controllable_component_id"
        ]
        assert direct["command_target_id"] == observed["arm"][
            "command_target_id"
        ]
        assert direct["base_frame_role_id"] == "base"
        assert direct["tcp_frame_role_id"] == "tcp"
        assert direct["planning_capability_ids"] == [
            "planning.motion.cartesian_path",
            "planning.motion.joint_goal",
            "planning.motion.pose_goal",
        ]
        assert [joint["source_joint_name"] for joint in direct["joints"]] == (
            manifest["source_joint_names"]
        )
        assert direct["observed_state"]["source_joint_names"] == manifest[
            "source_joint_names"
        ]
        current_orientation = direct["observed_state"]["current_tcp_pose"][
            "orientation"
        ]
        assert sum(value * value for value in current_orientation.values()) == (
            pytest.approx(1.0)
        )
        assert all(
            joint["min_position_rad"] <= joint["max_position_rad"]
            and joint["max_velocity_rad_s"] > 0.0
            for joint in direct["joints"]
        )

        direct_gripper = first_status["direct_control_gripper"]
        assert direct_gripper["program_id"] == (
            "manyforge.direct_control.gripper"
        )
        assert direct_gripper["program_binding_id"] != manifest[
            "program_binding_id"
        ]
        assert direct_gripper["command_schema_id"] == manifest[
            "gripper_command_schema_id"
        ]
        assert direct_gripper["resource"]["coordinate_kind"] == "linear_m"
        assert direct_gripper["resource"]["source_coordinate_name"]
        assert direct_gripper["resource"]["min_position"] <= (
            direct_gripper["resource"]["max_position"]
        )

        native = ExecutionCommandClient(execution_run_root, timeout_seconds=2.0)
        before_preview_events = native.events(first_status["owner_epoch"], 0)[
            "events"
        ]
        before_preview_state = copy.deepcopy(observed["arm"]["positions"])
        before_gripper_preview_state = copy.deepcopy(
            observed["gripper"]["positions"]
        )
        gripper_preview_target = (
            direct_gripper["resource"]["min_position"]
            + direct_gripper["resource"]["max_position"]
        ) / 2.0
        gripper_intent = _direct_control_gripper_operation(
            first_status,
            request_id="composer-direct-gripper-preview",
            position=gripper_preview_target,
        )
        gripper_preview = composer.preview_capability_command(gripper_intent)
        gripper_receipt = gripper_preview["preview_receipt"]
        assert gripper_receipt["commit_eligible"] is True
        assert gripper_receipt["target_position"] == pytest.approx(
            gripper_preview_target
        )
        assert gripper_receipt["coordinate_kind"] == "linear_m"
        assert gripper_receipt["program_binding_id"] == direct_gripper[
            "program_binding_id"
        ]
        assert native.events(first_status["owner_epoch"], 0)["events"] == (
            before_preview_events
        )
        after_gripper_preview = headless.execution_status()["status"]
        after_gripper_observed = next(
            target
            for target in after_gripper_preview["observed_targets"]
            if target["target_role_id"] == "gripper"
        )
        assert after_gripper_observed["positions"] == (
            before_gripper_preview_state
        )
        direct_positions = [
            float(position)
            for position in direct["observed_state"]["positions_rad"]
        ]
        direct_positions[0] = 0.03
        direct_intent = _direct_control_joint_operation(
            first_status,
            request_id="composer-direct-preview",
            positions_rad=direct_positions,
        )
        preview = composer.preview_motion(direct_intent)
        receipt = preview["preview_receipt"]
        assert receipt["commit_eligible"] is True
        assert receipt["source_joint_names"] == manifest["source_joint_names"]
        assert receipt["final_positions_rad"] == pytest.approx(direct_positions)
        assert receipt["program_binding_id"] == direct["program_binding_id"]
        assert "trajectory" not in receipt
        assert "compiled" not in receipt
        assert "providers" not in receipt
        pose_intent = _direct_control_motion_operation(
            first_status,
            request_id="composer-direct-pose-preview",
            motion={
                "kind": "pose_goal",
                "goal": receipt["final_tcp_pose"],
            },
        )
        pose_preview = composer.preview_motion(pose_intent)["preview_receipt"]
        pose_tolerances = pose_intent["participants"][0]["payload"][
            "tolerances"
        ]
        assert _translation_error_m(
            pose_preview["final_tcp_pose"]["position_m"],
            receipt["final_tcp_pose"]["position_m"],
        ) <= pose_tolerances["translation_tol_m"]
        assert _rotation_error_rad(
            pose_preview["final_tcp_pose"]["orientation"],
            receipt["final_tcp_pose"]["orientation"],
        ) <= pose_tolerances["rotation_tol_rad"]
        cartesian_intent = _direct_control_motion_operation(
            first_status,
            request_id="composer-direct-cartesian-preview",
            motion={
                "kind": "cartesian_path",
                "waypoints": [
                    direct["observed_state"]["current_tcp_pose"],
                    receipt["final_tcp_pose"],
                ],
                "max_translation_step_m": 0.005,
                "max_rotation_step_rad": 0.05,
                "speed_mode": "bounded",
                "max_linear_speed_mps": 0.1,
                "max_angular_speed_rad_s": 0.5,
                "max_linear_acceleration_mps2": 0.5,
                "max_angular_acceleration_rad_s2": 2.5,
                "fallback_policy": "fail_closed",
            },
        )
        cartesian_preview = composer.preview_motion(cartesian_intent)[
            "preview_receipt"
        ]
        assert cartesian_preview["commit_eligible"] is True
        assert native.events(first_status["owner_epoch"], 0)["events"] == (
            before_preview_events
        )
        after_preview_status = headless.execution_status()["status"]
        after_preview_arm = next(
            target
            for target in after_preview_status["observed_targets"]
            if target["target_role_id"] == "arm"
        )
        assert after_preview_arm["positions"] == before_preview_state

        stale_scene_intent = copy.deepcopy(direct_intent)
        stale_scene_intent["request_context"]["request_id"] = (
            "composer-direct-stale-scene"
        )
        stale_scene_intent["expected_scene_revision"]["revision_id"] += 1
        with pytest.raises(HeadlessClientError) as stale_scene:
            composer.preview_motion(stale_scene_intent)
        assert stale_scene.value.code == "scene_changed_during_validation"

        direct_accepted = composer.execute(direct_intent)
        direct_operation_id = direct_accepted["accepted"]["operation_id"]
        direct_terminal = _wait_terminal(runner, direct_operation_id)[
            "operation"
        ]
        assert direct_terminal["state"] == "completed", direct_terminal
        assert direct_terminal["program_binding_id"] == direct[
            "program_binding_id"
        ]
        composer.acknowledge_completed_operation(
            direct_operation_id,
            {
                "request_context": {
                    "request_id": "ack-composer-direct-operation",
                    "request_owner_epoch": first_status["owner_epoch"],
                    "source_label": "composer_ui",
                    "correlation_id": None,
                },
                "operation_id": direct_operation_id,
                "expected_operation_revision": direct_terminal[
                    "operation_revision"
                ],
                "reason": "release completed direct-control operation",
            },
        )

        relative_status = headless.execution_status()["status"]
        relative_direct = relative_status["direct_control"]
        relative_start_pose = relative_direct["observed_state"][
            "current_tcp_pose"
        ]
        relative_pose_intent = _direct_control_motion_operation(
            relative_status,
            request_id="composer-direct-relative-pose-execution",
            motion={"kind": "pose_goal", "goal": _pose()},
        )
        relative_payload = relative_pose_intent["participants"][0]["payload"]
        relative_payload["target_mode"] = "relative"
        relative_payload["target_reference"] = {
            "type": "frame_role",
            "frame_role_id": relative_direct["tcp_frame_role_id"],
        }
        relative_payload["offset_reference"] = {
            "type": "frame_role",
            "frame_role_id": relative_direct["tcp_frame_role_id"],
        }
        relative_accepted = composer.execute(relative_pose_intent)
        relative_operation_id = relative_accepted["accepted"]["operation_id"]
        relative_terminal = _wait_terminal(runner, relative_operation_id)[
            "operation"
        ]
        assert relative_terminal["state"] == "completed", relative_terminal
        relative_end_pose = headless.execution_status()["status"][
            "direct_control"
        ]["observed_state"]["current_tcp_pose"]
        assert _translation_error_m(
            relative_end_pose["position_m"], relative_start_pose["position_m"]
        ) <= relative_payload["tolerances"]["translation_tol_m"]
        assert _rotation_error_rad(
            relative_end_pose["orientation"], relative_start_pose["orientation"]
        ) <= relative_payload["tolerances"]["rotation_tol_rad"]
        composer.acknowledge_completed_operation(
            relative_operation_id,
            {
                "request_context": {
                    "request_id": "ack-composer-relative-operation",
                    "request_owner_epoch": relative_status["owner_epoch"],
                    "source_label": "composer_ui",
                    "correlation_id": None,
                },
                "operation_id": relative_operation_id,
                "expected_operation_revision": relative_terminal[
                    "operation_revision"
                ],
                "reason": "release completed relative operation",
            },
        )

        direct_gripper_accepted = composer.execute(gripper_intent)
        direct_gripper_operation_id = direct_gripper_accepted["accepted"][
            "operation_id"
        ]
        direct_gripper_terminal = _wait_terminal(
            runner, direct_gripper_operation_id
        )["operation"]
        assert direct_gripper_terminal["state"] == "completed"
        assert direct_gripper_terminal["program_binding_id"] == (
            direct_gripper["program_binding_id"]
        )
        composer.acknowledge_completed_operation(
            direct_gripper_operation_id,
            {
                "request_context": {
                    "request_id": "ack-composer-direct-gripper-operation",
                    "request_owner_epoch": first_status["owner_epoch"],
                    "source_label": "composer_ui",
                    "correlation_id": None,
                },
                "operation_id": direct_gripper_operation_id,
                "expected_operation_revision": direct_gripper_terminal[
                    "operation_revision"
                ],
                "reason": "release completed direct-control gripper operation",
            },
        )

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

        with pytest.raises(HeadlessClientError) as wrong_unit:
            _gripper_submission(
                runner,
                request_id="reject-angular-gripper-unit",
                position_kind="angular_rad",
                position=0.004,
            )
        assert wrong_unit.value.status_code == 400
        assert recording.last_submission is not None
        raw_wrong_unit = raw_api.post(
            "/api/execution/operations", json=recording.last_submission
        )
        assert raw_wrong_unit.json()["error"]["details"]["reason"] == (
            "runtime_command_resource_unit_mismatch"
        )

        with pytest.raises(HeadlessClientError) as beyond_limit:
            _gripper_submission(
                runner,
                request_id="reject-gripper-limit",
                position_kind="linear_m",
                position=0.02,
            )
        assert beyond_limit.value.status_code == 400
        assert recording.last_submission is not None
        raw_beyond_limit = raw_api.post(
            "/api/execution/operations", json=recording.last_submission
        )
        assert raw_beyond_limit.json()["error"]["code"] == "invalid_request"
        assert raw_beyond_limit.json()["error"]["details"]["reason"] == (
            "gripper_position_limit_violation"
        )

        gripper_accepted = _gripper_submission(
            runner,
            request_id="accepted-linear-gripper",
            position_kind="linear_m",
            position=0.004,
        )
        gripper_id = gripper_accepted["accepted"]["operation_id"]
        gripper_terminal = _wait_terminal(runner, gripper_id)["operation"]
        assert gripper_terminal["state"] == "completed"
        assert gripper_terminal["participants"][0]["payload_kind"] == (
            "capability_command"
        )
        assert composer.operation(gripper_id)["operation"] == gripper_terminal
        gripper_status = headless.execution_status()["status"]
        gripper_observed = next(
            target
            for target in gripper_status["observed_targets"]
            if target["target_role_id"] == "gripper"
        )
        assert gripper_observed["positions"] == [
            {"kind": "linear_m", "value_m": pytest.approx(0.004)}
        ]
        runner.acknowledge_completed_operation(
            request_id="ack-linear-gripper",
            operation_id=gripper_id,
            expected_operation_revision=gripper_terminal[
                "operation_revision"
            ],
            reason="release completed gripper operation",
            correlation_id="n5-process-test",
        )

        cancellable = _gripper_submission(
            runner,
            request_id="cancellable-linear-gripper",
            position_kind="linear_m",
            position=0.008,
            command_duration_s=1.0,
        )
        cancellable_id = cancellable["accepted"]["operation_id"]
        cancellable_state = _wait_state(
            runner, cancellable_id, {"executing"}
        )["operation"]
        gripper_stop = runner.stop_operation(
            request_id="stop-linear-gripper",
            operation_id=cancellable_id,
            expected_operation_revision=cancellable_state[
                "operation_revision"
            ],
            expected_stop_generation=0,
            reason="exercise addressed gripper Stop",
            correlation_id="n5-process-test",
        )
        cancelled = _wait_terminal(runner, cancellable_id)["operation"]
        assert cancelled["state"] == "stopped", json.dumps(
            cancelled, sort_keys=True
        )
        assert cancelled["active_stop_id"] == gripper_stop["accepted"][
            "stop_id"
        ]
        runner.reset_operation(
            request_id="reset-linear-gripper",
            operation_id=cancellable_id,
            expected_operation_revision=cancelled["operation_revision"],
            reason="release stopped gripper operation",
            correlation_id="n5-process-test",
        )
        gripper_recovery = _recover_active_stop_fault(
            headless,
            owner_epoch=first_status["owner_epoch"],
            request_prefix="recover-linear-gripper-stop",
            correlation_id="n5-process-test",
            reason="operator verified post-Stop gripper truth",
        )
        assert gripper_recovery is not None
        assert gripper_recovery["accepted"] is True
        assert gripper_recovery["latch"]["state"] == "cleared"
        assert headless.stop_fault()["stop_fault"] is None

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
        stopped = _wait_terminal(runner, controlled_id)["operation"]
        assert stopped["state"] == "stopped"
        assert stopped["active_stop_id"] == stop_accepted["accepted"]["stop_id"]
        with sqlite3.connect(
            execution_state_root / "operation-journal.sqlite3"
        ) as database:
            durable_stop = database.execute(
                "SELECT sequence,payload_json FROM journal_records "
                "WHERE request_id = ? AND record_kind = "
                "'stop_acceptance_projected' AND stop_id = ?",
                (
                    "stop-controlled-operation",
                    stop_accepted["accepted"]["stop_id"],
                ),
            ).fetchone()
        assert durable_stop is not None
        durable_stop_sequence = int(durable_stop[0])
        durable_stop_payload = json.loads(durable_stop[1])
        assert durable_stop_payload["intent"]["value"][
            "request_attribution"
        ] == {
            "assurance": "externally_authenticated",
            "principal_id": "operator.process-test",
            "transport": "http",
        }
        durable_stop_page = native.events(
            first_status["owner_epoch"], durable_stop_sequence - 1, limit=1
        )
        assert len(durable_stop_page["events"]) == 1
        durable_stop_event = durable_stop_page["events"][0]
        assert durable_stop_event["sequence"] == durable_stop_sequence
        assert durable_stop_event["record_kind"] == (
            "stop_acceptance_projected"
        )
        assert durable_stop_event["operation_id"] is None
        assert durable_stop_event["operation_revision"] is None
        assert durable_stop_event["payload"] == durable_stop_payload

        # Exercise the complete recognized-malformed ingress and durable
        # recovery boundary through the production HTTP/native/SQLite path.
        # The first Stop above is released before starting new work so the
        # malformed request must contain a genuinely live operation rather
        # than an idle snapshot.
        runner.reset_operation(
            request_id="reset-controlled-operation",
            operation_id=controlled_id,
            expected_operation_revision=stopped["operation_revision"],
            reason="release first stopped operation",
            correlation_id="n4-public-control-test",
        )
        controlled_recovery = _recover_active_stop_fault(
            headless,
            owner_epoch=first_status["owner_epoch"],
            request_prefix="recover-controlled-stop",
            correlation_id="n4-public-control-test",
            reason="operator verified post-Stop motion truth",
        )
        assert controlled_recovery is not None
        assert controlled_recovery["accepted"] is True
        assert controlled_recovery["latch"]["state"] == "cleared"
        malformed_operation = _runner_submission(
            runner,
            request_id="native-malformed-stop-operation",
            joint_names=("joint1",),
            positions=(-0.55,),
        )
        malformed_operation_id = malformed_operation["accepted"]["operation_id"]
        _wait_state(runner, malformed_operation_id, {"executing"})
        malformed_response = raw_api.post(
            "/api/execution/stops",
            content=b'{"operations":[',
            headers={
                "Content-Type": "application/json",
                "X-Request-ID": "native-malformed-stop",
            },
        )
        assert malformed_response.status_code == 200, malformed_response.text
        malformed_acceptance = malformed_response.json()["accepted"]
        assert malformed_acceptance["request_id"] == (
            "malformed-stop.native-malformed-stop"
        )
        assert malformed_acceptance["resolution_disposition"] == (
            "promoted_to_full"
        )
        assert malformed_acceptance["resolution_reason"] == (
            "malformed_ordinary_intent"
        )
        malformed_terminal = _wait_terminal(
            runner, malformed_operation_id
        )["operation"]
        assert malformed_terminal["state"] == "stopped"
        assert malformed_terminal["active_stop_id"] == malformed_acceptance[
            "stop_id"
        ]

        with sqlite3.connect(
            execution_state_root / "operation-journal.sqlite3"
        ) as database:
            malformed_row = database.execute(
                "SELECT payload_json FROM journal_records WHERE request_id=? "
                "AND record_kind='stop_acceptance_projected'",
                ("malformed-stop.native-malformed-stop",),
            ).fetchone()
        assert malformed_row is not None
        malformed_payload = json.loads(malformed_row[0])
        assert malformed_payload["acceptance"]["malformed_ingress_reason"] == (
            "recognized_stop_payload_invalid"
        )
        assert malformed_payload["acceptance"]["normalized_request_digest"] is None
        assert malformed_payload["intent"] == {
            "intent_kind": "malformed_ingress",
            "value": None,
        }

        recovered_result = _recover_active_stop_fault(
            headless,
            owner_epoch=first_status["owner_epoch"],
            request_prefix="recover-native-malformed",
            correlation_id="native-malformed-stop-recovery",
            reason="operator verified post-Stop cell truth",
        )
        assert recovered_result is not None
        assert recovered_result["accepted"] is True, recovered_result
        assert recovered_result["latch"]["state"] == "cleared"
        assert headless.stop_fault()["stop_fault"] is None
        with sqlite3.connect(
            execution_state_root / "operation-journal.sqlite3"
        ) as database:
            clear_rows = database.execute(
                "SELECT payload_json FROM journal_records "
                "WHERE record_kind='stop_fault_cleared' AND stop_id=?",
                (malformed_acceptance["stop_id"],),
            ).fetchall()
        assert len(clear_rows) == 1
        durable_clear = json.loads(clear_rows[0][0])
        assert durable_clear[0] == (
            "manyforge.internal.stop-journal-fault-clear-projection.v1"
        )
        assert durable_clear[2][0] == recovered_result["request_id"]

        # N7-b exercises the largest maintained page through the real
        # SOCK_SEQPACKET boundary. The preceding maintained workflow has
        # produced more than one page; this is not a synthetic event source.
        largest_page = native.events(
            first_status["owner_epoch"], 0, limit=16
        )
        assert len(largest_page["events"]) == 16
        assert largest_page["has_more"] is True
        assert largest_page["next_after_sequence"] == largest_page["events"][-1][
            "sequence"
        ]

        durable_journal = execution_state_root / "operation-journal.sqlite3"
        durable_identity = (
            durable_journal.stat().st_dev,
            durable_journal.stat().st_ino,
        )
        run_root_identity = (
            execution_run_root.stat().st_dev,
            execution_run_root.stat().st_ino,
        )
        _stop_host(process)
        assert (
            execution_run_root.stat().st_dev,
            execution_run_root.stat().st_ino,
        ) == run_root_identity
        assert not (execution_run_root / "execution-command.sock").exists()
        process = _start_host(
            fixture_root, execution_state_root, execution_run_root, manifest
        )
        assert (durable_journal.stat().st_dev, durable_journal.stat().st_ino) == (
            durable_identity
        )
        assert (
            execution_run_root.stat().st_dev,
            execution_run_root.stat().st_ino,
        ) == run_root_identity
        assert (execution_run_root / "execution-command.sock").is_socket()
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


def test_cell_admission_crosses_real_execution_seqpacket_surface(
    tmp_path: Path,
) -> None:
    built_fixture = _required_path("MANYFORGE_I6_NATIVE_FIXTURE_ROOT")
    fixture_root = tmp_path / "fixture"
    shutil.copytree(built_fixture, fixture_root)
    manifest = json.loads((fixture_root / "manifest.json").read_text("utf-8"))
    execution_state_root = tmp_path / "execution-state"
    execution_run_root = tmp_path / "execution-run"
    execution_state_root.mkdir()
    execution_run_root.mkdir()
    process = _start_host(fixture_root, execution_state_root, execution_run_root, manifest)
    client = ExecutionCommandClient(execution_run_root, timeout_seconds=5.0)
    try:
        get = {
            "api_version": "manyforge.execution_command.v1",
            "command": "cell_admission_get",
        }
        first = client._exchange(get)
        replay = client._exchange(get)
        assert replay == first
        assert set(first) == {
            "api_version",
            "authoring_admission",
            "command",
            "ok",
        }
        assert first["command"] == "cell_admission_get"

        malformed = client._exchange(
            {
                **get,
                "unexpected": True,
            }
        )
        assert malformed["ok"] is False
        assert malformed["error"]["code"] == "execution_command_invalid"
        assert process.poll() is None

        stop_only = client._exchange_on(
            get, execution_run_root / "execution-control.sock"
        )
        assert stop_only == {
            "api_version": "manyforge.execution_command_response.v1",
            "command": "cell_admission_get",
            "error": {
                "code": "execution_control_command_unsupported",
                "details": {},
                "message": "the independent Stop control socket accepts only Stop",
            },
            "ok": False,
        }
        assert process.poll() is None
    finally:
        _stop_host(process)


def test_addressed_native_benchmark_preserves_historical_measurements(
    tmp_path: Path,
) -> None:
    """Restore the three historical benchmark meanings on the N7 route."""

    built_fixture = _required_path("MANYFORGE_I6_NATIVE_FIXTURE_ROOT")
    fixture_root = tmp_path / "fixture"
    shutil.copytree(built_fixture, fixture_root)
    manifest = json.loads((fixture_root / "manifest.json").read_text("utf-8"))
    execution_state_root = tmp_path / "execution-state"
    execution_run_root = tmp_path / "execution-run"
    execution_state_root.mkdir()
    execution_run_root.mkdir()
    process = _start_host(
        fixture_root, execution_state_root, execution_run_root, manifest
    )
    native = ExecutionCommandClient(execution_run_root, timeout_seconds=5.0)

    goal_a = [0.1, -0.5, 0.5, -0.5, -0.5, 0.0]
    goal_b = [0.3, -0.8, 0.8, -0.3, -0.8, 0.1]
    five_goals = [
        goal_a,
        goal_b,
        [-0.2, -1.0, 1.0, -0.5, -1.0, 0.0],
        [0.5, -0.6, 0.6, -0.6, -0.6, 0.2],
        [0.0, -0.7, 0.7, -0.7, -0.7, 0.0],
    ]

    try:
        initial_status = native.status()["status"]
        origin = [
            float(value)
            for value in initial_status["direct_control"]["observed_state"][
                "positions_rad"
            ]
        ]

        _run_native_benchmark_move(
            native,
            request_id="benchmark-warmup",
            positions_rad=goal_a,
            cache_mode="default",
        )
        _run_native_benchmark_move(
            native,
            request_id="benchmark-warmup-return",
            positions_rad=origin,
            cache_mode="disable",
        )

        single_plan_seconds: list[float] = []
        for index in range(3):
            single_plan_seconds.append(
                _run_native_benchmark_move(
                    native,
                    request_id=f"benchmark-single-{index}",
                    positions_rad=goal_a,
                    cache_mode="disable",
                )
            )
            _run_native_benchmark_move(
                native,
                request_id=f"benchmark-single-return-{index}",
                positions_rad=origin,
                cache_mode="disable",
            )

        shared_cache_move_id = "benchmark-cache-shared-motion"
        uncached_seconds = _run_native_benchmark_move(
            native,
            request_id="benchmark-cache-prime",
            positions_rad=goal_b,
            cache_mode="default",
            move_id=shared_cache_move_id,
        )
        _run_native_benchmark_move(
            native,
            request_id="benchmark-cache-return",
            positions_rad=origin,
            cache_mode="disable",
        )
        cached_seconds = _run_native_benchmark_move(
            native,
            request_id="benchmark-cache-reuse",
            positions_rad=goal_b,
            cache_mode="default",
            move_id=shared_cache_move_id,
        )

        five_started_at = time.perf_counter()
        five_move_seconds = [
            _run_native_benchmark_move(
                native,
                request_id=f"benchmark-five-{index}",
                positions_rad=goal,
                cache_mode="disable",
            )
            for index, goal in enumerate(five_goals)
        ]
        five_total_seconds = time.perf_counter() - five_started_at

        # The historical probe was informational. These broad bounds retain
        # its regression value while the request's own planning/execution
        # deadlines remain the authoritative product limits.
        assert max(single_plan_seconds) < 5.0
        assert uncached_seconds < 5.0
        assert cached_seconds < 5.0
        assert five_total_seconds < 25.0
        measurement = {
            "schema": "manyforge.i6.addressed_native_benchmark.v1",
            "historical_source_commit": (
                "08222188293df8e12f90dc72e204c21f22f92ea1"
            ),
            "single_plan_ms": [
                round(value * 1000.0, 3) for value in single_plan_seconds
            ],
            "cached_vs_uncached_ms": {
                "cached": round(cached_seconds * 1000.0, 3),
                "uncached": round(uncached_seconds * 1000.0, 3),
            },
            "five_moves": {
                "per_move_ms": [
                    round(value * 1000.0, 3) for value in five_move_seconds
                ],
                "total_ms": round(five_total_seconds * 1000.0, 3),
            },
        }
        print(json.dumps(measurement, sort_keys=True))
    finally:
        _stop_host(process)


def test_slow_preview_cannot_delay_the_standard_stop_path(tmp_path: Path) -> None:
    built_fixture = _required_path("MANYFORGE_I6_NATIVE_FIXTURE_ROOT")
    fixture_root = tmp_path / "fixture"
    shutil.copytree(built_fixture, fixture_root)
    manifest = json.loads((fixture_root / "manifest.json").read_text("utf-8"))
    execution_state_root = tmp_path / "execution-state"
    execution_run_root = tmp_path / "execution-run"
    execution_state_root.mkdir()
    execution_run_root.mkdir()
    marker = tmp_path / "blocked-preview-worker"
    process = _start_host(
        fixture_root,
        execution_state_root,
        execution_run_root,
        manifest,
        candidate_worker=_required_path("MANYFORGE_ROBOPLAN_FAKE_WORKER"),
        fake_worker_marker=marker,
    )
    execution = ExecutionCommandClient(execution_run_root, timeout_seconds=3.0)
    app = create_app(
        service=HeadlessService(
            UnusedCore(),  # type: ignore[arg-type]
            UnusedManager(),  # type: ignore[arg-type]
            execution,
        ),
        trusted_principal_adapter=FixedTrustedPrincipalAdapter(),
    )
    raw_api = TestClient(app)
    headless = HeadlessHttpClient("http://headless.test", client=raw_api)
    preview_done = threading.Event()
    preview_response: list[dict[str, Any]] = []
    preview_error: list[Exception] = []
    preview_thread: threading.Thread | None = None

    try:
        status = headless.execution_status()["status"]
        positions = [
            float(position)
            for position in status["direct_control"]["observed_state"][
                "positions_rad"
            ]
        ]
        positions[0] = 0.03
        intent = _direct_control_joint_operation(
            status,
            request_id="blocked-direct-preview",
            positions_rad=positions,
        )

        def run_preview() -> None:
            try:
                preview_response.append(
                    execution.preview_motion(
                        OperationIntentRequest.model_validate(intent),
                        trusted_principal_id=None,
                    )
                )
            except Exception as error:  # surfaced after the causal assertion
                preview_error.append(error)
            finally:
                preview_done.set()

        preview_thread = threading.Thread(target=run_preview, daemon=True)
        preview_thread.start()
        deadline = time.monotonic() + 2.0
        while not marker.exists() and time.monotonic() < deadline:
            time.sleep(0.005)
        assert marker.exists()
        assert not preview_done.is_set()

        stopped = headless.stop_operations(
            {
                "request_context": {
                    "request_id": "stop-while-preview-blocked",
                    "request_owner_epoch": status["owner_epoch"],
                    "source_label": "headless_process_test",
                    "correlation_id": "n6-c1-stop-isolation",
                },
                "operations": [
                    {
                        "operation_id": "non-resident-operation",
                        "expected_operation_revision": 1,
                        "expected_stop_generation": 0,
                    }
                ],
                "reason": "prove Stop transport is independent from preview",
            }
        )
        assert stopped["accepted"]["resolution_disposition"] == (
            "promoted_to_full"
        )
        assert stopped["accepted"]["resolution_reason"] == "unknown_operation"
        assert not preview_done.is_set()

        preview_thread.join(timeout=3.0)
        assert preview_done.is_set()
        assert not preview_error
        assert preview_response[0]["ok"] is False
    finally:
        _stop_host(process)
        if preview_thread is not None:
            preview_thread.join(timeout=3.0)
        raw_api.close()


def test_planning_stop_and_active_host_replacement_are_bounded(
    tmp_path: Path,
) -> None:
    built_fixture = _required_path("MANYFORGE_I6_NATIVE_FIXTURE_ROOT")
    fixture_root = tmp_path / "fixture"
    shutil.copytree(built_fixture, fixture_root)
    manifest = json.loads((fixture_root / "manifest.json").read_text("utf-8"))
    execution_state_root = tmp_path / "execution-state"
    execution_run_root = tmp_path / "execution-run"
    execution_state_root.mkdir()
    execution_run_root.mkdir()
    marker = tmp_path / "fake-worker-generation"
    fake_worker = _required_path("MANYFORGE_ROBOPLAN_FAKE_WORKER")

    process = _start_host(
        fixture_root,
        execution_state_root,
        execution_run_root,
        manifest,
        candidate_worker=fake_worker,
        fake_worker_marker=marker,
    )
    raw_api = TestClient(
        create_app(
            service=HeadlessService(
                UnusedCore(),  # type: ignore[arg-type]
                UnusedManager(),  # type: ignore[arg-type]
                ExecutionCommandClient(execution_run_root, timeout_seconds=10.0),
            ),
            trusted_principal_adapter=FixedTrustedPrincipalAdapter(),
        )
    )
    headless = HeadlessHttpClient("http://headless.test", client=raw_api)
    recording = RecordingTransport(headless)
    runner = OrchestratorRunner(KernelClient(recording))

    try:
        first_epoch = headless.execution_status()["status"]["owner_epoch"]
        cclc_owner_before_loss = CCLCOpenRequestClient(
            execution_run_root, timeout_seconds=5.0
        ).owner_identity()["request_owner_epoch"]
        cclc_before_loss = _wait_for_cclc_component(execution_run_root)
        planning_acceptance = _runner_submission(
            runner,
            request_id="planning-stop-operation",
            joint_names=("joint1",),
            positions=(0.2,),
        )
        planning_id = planning_acceptance["accepted"]["operation_id"]
        planning = _wait_state(runner, planning_id, {"planning"})["operation"]
        assert marker.exists()

        stop = runner.stop_operation(
            request_id="stop-during-planning",
            operation_id=planning_id,
            expected_operation_revision=planning["operation_revision"],
            expected_stop_generation=0,
            reason="prove bounded planning cancellation",
            correlation_id="n4d-process-test",
        )
        stopped = _wait_terminal(runner, planning_id)["operation"]
        assert stopped["state"] == "stopped"
        assert stopped["active_stop_id"] == stop["accepted"]["stop_id"]

        reset = runner.reset_operation(
            request_id="reset-planning-stop",
            operation_id=planning_id,
            expected_operation_revision=stopped["operation_revision"],
            reason="release stopped operation",
            correlation_id="n4d-process-test",
        )
        assert reset["accepted"]["control"] == "reset"
        recovered_acceptance = _runner_submission(
            runner,
            request_id="worker-after-cancel",
            joint_names=("joint1",),
            positions=(0.1,),
        )
        recovered_id = recovered_acceptance["accepted"]["operation_id"]
        recovered = _wait_terminal(runner, recovered_id)["operation"]
        assert recovered["state"] == "completed", recovered
        runner.acknowledge_completed_operation(
            request_id="ack-worker-after-cancel",
            operation_id=recovered_id,
            expected_operation_revision=recovered["operation_revision"],
            reason="release completed operation",
            correlation_id="n4d-process-test",
        )

        marker.unlink()
        orphan_acceptance = _runner_submission(
            runner,
            request_id="active-host-loss",
            joint_names=("joint1",),
            positions=(0.3,),
        )
        orphan_id = orphan_acceptance["accepted"]["operation_id"]
        _wait_state(runner, orphan_id, {"planning"})
        assert marker.exists()
        process.kill()
        process.wait(timeout=5.0)

        process = _start_host(
            fixture_root,
            execution_state_root,
            execution_run_root,
            manifest,
            candidate_worker=fake_worker,
            fake_worker_marker=marker,
            qualify_cclc=False,
        )
        restarted = headless.execution_status()["status"]
        assert restarted["owner_epoch"] != first_epoch
        cclc_after_loss = _wait_for_cclc_component(execution_run_root)
        assert cclc_after_loss["component_lifecycle_state"] == "active"
        assert (
            cclc_after_loss["component_lifecycle_generation"]
            == cclc_before_loss["component_lifecycle_generation"]
        )
        assert [
            (
                domain["control_domain_id"],
                domain["current_control_bindings"],
                domain["active_control_configuration"],
            )
            for domain in cclc_after_loss["domains"]
        ] == [
            (
                domain["control_domain_id"],
                domain["current_control_bindings"],
                domain["active_control_configuration"],
            )
            for domain in cclc_before_loss["domains"]
        ]
        provider_epochs_before = {
            (
                row["control_domain_id"],
                row["provider_instance_id"],
                row["provider_runtime_unit_id"],
            ): row["provider_runtime_epoch"]
            for row in cclc_before_loss["provider_occurrences"]
        }
        provider_epochs_after = {
            (
                row["control_domain_id"],
                row["provider_instance_id"],
                row["provider_runtime_unit_id"],
            ): row["provider_runtime_epoch"]
            for row in cclc_after_loss["provider_occurrences"]
        }
        assert provider_epochs_after.keys() == provider_epochs_before.keys()
        assert all(
            provider_epochs_after[unit] != epoch
            for unit, epoch in provider_epochs_before.items()
        )
        orphan = runner.operation(orphan_id)["operation"]
        assert orphan["state"] == "intervention_required"
        terminal_error = orphan["participants"][0]["terminal_error"]
        assert terminal_error["code"] == "execution_host_replaced"
        assert terminal_error["details"]["reason"] == (
            "unreconciled_external_or_orphan_goal"
        )

        native = ExecutionCommandClient(execution_run_root, timeout_seconds=2.0)
        page = native.events(restarted["owner_epoch"], 0)
        assert [event["record_kind"] for event in page["events"]] == [
            "host_replaced"
        ]
        assert page["events"][0]["operation_id"] == orphan_id
        assert page["events"][0]["occurred_at_wall"].endswith("Z")
        with pytest.raises(HeadlessApiError) as stale_events:
            native.events(first_epoch, 0)
        assert stale_events.value.code == "operation_journal_owner_epoch_stale"

        # Recovery retained lifecycle and route truth without replaying either
        # lifecycle intent. The prior request epoch remains fenced, while one
        # explicit fresh complete-domain route transaction is accepted.
        cclc = CCLCOpenRequestClient(execution_run_root, timeout_seconds=5.0)
        templates = cclc.route_templates("lite6_arm")
        stale_template = templates["templates"][0]
        with pytest.raises(HeadlessApiError):
            cclc.submit_route(
                {
                    "request_context": {
                        "request_id": "process.route.stale-owner",
                        "request_owner_epoch": cclc_owner_before_loss,
                        "source_label": "native-process-qualification",
                    },
                    "expected_retained_bundle_id": templates[
                        "retained_bundle_id"
                    ],
                    "expected_graph_revision": templates["graph_revision"],
                    "desired_configuration": stale_template[
                        "desired_configuration"
                    ],
                    "expected_active_control_configuration": stale_template[
                        "expected_active_control_configuration"
                    ],
                    "component_expectations": stale_template[
                        "component_expectations"
                    ],
                }
            )
        _submit_native_cclc_routes(
            execution_run_root,
            request_prefix="process.route.after-child-sigkill",
        )

        # The new owner adopted the exact durable route, proved current
        # quiescence/no-live-goal without a command, released only those
        # orphan-resource slots, and then republished normal readiness.
        gripper_after_restart = _gripper_submission(
            runner,
            request_id="gripper-after-arm-host-loss",
            position_kind="linear_m",
            position=0.003,
        )
        gripper_after_restart_id = gripper_after_restart["accepted"][
            "operation_id"
        ]
        gripper_terminal = _wait_terminal(
            runner, gripper_after_restart_id
        )["operation"]
        assert gripper_terminal["state"] == "completed"
        runner.acknowledge_completed_operation(
            request_id="ack-gripper-after-arm-host-loss",
            operation_id=gripper_after_restart_id,
            expected_operation_revision=gripper_terminal[
                "operation_revision"
            ],
            reason="release restarted gripper operation",
            correlation_id="n4d-process-test",
        )

        arm_after_restart = _runner_submission(
            runner,
            request_id="arm-after-arm-host-loss",
            joint_names=("joint1",),
            positions=(0.05,),
        )
        arm_after_restart_id = arm_after_restart["accepted"]["operation_id"]
        arm_terminal = _wait_terminal(runner, arm_after_restart_id)[
            "operation"
        ]
        assert arm_terminal["state"] == "completed"
        runner.acknowledge_completed_operation(
            request_id="ack-arm-after-arm-host-loss",
            operation_id=arm_after_restart_id,
            expected_operation_revision=arm_terminal["operation_revision"],
            reason="release restarted arm operation",
            correlation_id="n4d-process-test",
        )
    finally:
        _stop_host(process)
        raw_api.close()


def test_active_gripper_host_replacement_reconciles_and_reopens_inventory(
    tmp_path: Path,
) -> None:
    built_fixture = _required_path("MANYFORGE_I6_NATIVE_FIXTURE_ROOT")
    fixture_root = tmp_path / "fixture"
    shutil.copytree(built_fixture, fixture_root)
    manifest = json.loads((fixture_root / "manifest.json").read_text("utf-8"))
    execution_state_root = tmp_path / "execution-state"
    execution_run_root = tmp_path / "execution-run"
    execution_state_root.mkdir()
    execution_run_root.mkdir()

    process = _start_host(
        fixture_root, execution_state_root, execution_run_root, manifest
    )
    raw_api = TestClient(
        create_app(
            service=HeadlessService(
                UnusedCore(),  # type: ignore[arg-type]
                UnusedManager(),  # type: ignore[arg-type]
                ExecutionCommandClient(execution_run_root, timeout_seconds=10.0),
            ),
            trusted_principal_adapter=FixedTrustedPrincipalAdapter(),
        )
    )
    headless = HeadlessHttpClient("http://headless.test", client=raw_api)
    recording = RecordingTransport(headless)
    runner = OrchestratorRunner(KernelClient(recording))

    try:
        first_epoch = headless.execution_status()["status"]["owner_epoch"]
        accepted = _gripper_submission(
            runner,
            request_id="active-gripper-host-loss",
            position_kind="linear_m",
            position=0.007,
            command_duration_s=5.0,
        )
        operation_id = accepted["accepted"]["operation_id"]
        before_loss = _wait_state(runner, operation_id, {"executing"})[
            "operation"
        ]
        assert before_loss["participants"][0]["resources"]
        _stop_host(process)

        process = _start_host(
            fixture_root,
            execution_state_root,
            execution_run_root,
            manifest,
        )
        restarted = headless.execution_status()["status"]
        assert restarted["owner_epoch"] != first_epoch
        orphan = runner.operation(operation_id)["operation"]
        assert orphan["state"] == "intervention_required"
        terminal_error = orphan["participants"][0]["terminal_error"]
        assert terminal_error["code"] == "execution_host_replaced"
        assert terminal_error["details"]["reason"] == (
            "unreconciled_external_or_orphan_goal"
        )

        arm_after_restart = _runner_submission(
            runner,
            request_id="arm-after-gripper-host-loss",
            joint_names=("joint1",),
            positions=(0.05,),
        )
        arm_after_restart_id = arm_after_restart["accepted"]["operation_id"]
        arm_terminal = _wait_terminal(runner, arm_after_restart_id)[
            "operation"
        ]
        assert arm_terminal["state"] == "completed"
        runner.acknowledge_completed_operation(
            request_id="ack-arm-after-gripper-host-loss",
            operation_id=arm_after_restart_id,
            expected_operation_revision=arm_terminal["operation_revision"],
            reason="release restarted arm operation",
            correlation_id="n4d-process-test",
        )

        gripper_after_restart = _gripper_submission(
            runner,
            request_id="gripper-after-gripper-host-loss",
            position_kind="linear_m",
            position=0.002,
        )
        gripper_after_restart_id = gripper_after_restart["accepted"][
            "operation_id"
        ]
        gripper_terminal = _wait_terminal(
            runner, gripper_after_restart_id
        )["operation"]
        assert gripper_terminal["state"] == "completed"
        runner.acknowledge_completed_operation(
            request_id="ack-gripper-after-gripper-host-loss",
            operation_id=gripper_after_restart_id,
            expected_operation_revision=gripper_terminal[
                "operation_revision"
            ],
            reason="release restarted gripper operation",
            correlation_id="n4d-process-test",
        )
    finally:
        _stop_host(process)
        raw_api.close()
