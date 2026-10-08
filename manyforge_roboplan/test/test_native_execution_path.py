"""I6 process test across host, headless, Behavior, and Composer callers."""

from __future__ import annotations

import copy
import json
import math
import os
import shutil
import socket
import sqlite3
import subprocess
import threading
import time
from pathlib import Path
from typing import Any, Callable, Mapping

from fastapi import FastAPI
from fastapi.testclient import TestClient
import pytest
import rfc8785

from manyforge_behavior.kernel_client import (
    KernelClient,
    KernelOperationAdmissionRejectedError,
)
from manyforge_behavior.runner import OrchestratorRunner
from manyforge_composer.backend.routes_headless import create_headless_router
from manyforge_composer.backend.state import ComposerState
from manyforge_headless_api.app import TrustedPrincipal, create_app
from manyforge_headless_api.cclc import CCLCOpenRequestClient
from manyforge_headless_api.cell_admission import (
    CellAdmissionClient,
    CellAdmissionError,
)
from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient
from manyforge_headless_api.execution import (
    EXECUTION_COMMAND_VERSION,
    ExecutionCommandClient,
)
from manyforge_headless_api.models import (
    FullStopIntentRequest,
    OperationControlRequest,
    OperationIntentRequest,
    PickSequencePlanningRequest,
    ProgramRunStopIntentRequest,
    StopIntentRequest,
    TargetObservationQuery,
)
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.private_transport import PinnedRunRoot
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
        # KernelClient retains its semantic method name, but the transport uses
        # the explicit cold-path aggregate rather than the deprecated alias.
        return self.client.execution_diagnostics()

    def identity(self) -> dict[str, Any]:
        return self.client.execution_identity()

    def submit(self, body: Mapping[str, Any]) -> dict[str, Any]:
        self.last_submission = copy.deepcopy(dict(body))
        return self.client.execute(body)

    def plan_pick(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self.client.plan_pick(body)

    def plan_place(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self.client.plan_place(body)

    def read(self, operation_id: str) -> dict[str, Any]:
        return self.client.operation(operation_id)

    def pause(self, operation_id: str, body: Mapping[str, Any]) -> dict[str, Any]:
        return self.client.pause_operation(operation_id, body)

    def resume(self, operation_id: str, body: Mapping[str, Any]) -> dict[str, Any]:
        return self.client.resume_operation(operation_id, body)

    def reset(self, operation_id: str, body: Mapping[str, Any]) -> dict[str, Any]:
        return self.client.reset_operation(operation_id, body)

    def acknowledge_completed(
        self, operation_id: str, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        return self.client.acknowledge_completed_operation(operation_id, body)

    def stop(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self.client.stop_operations(body)


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
                raise RuntimeError(f"native CCLC {label} failed: {status}")
            return status
        time.sleep(0.01)
    raise RuntimeError(f"native CCLC {label} did not become terminal: {last_status}")


def _expected_active_configuration(
    active: Mapping[str, Any] | None,
) -> dict[str, Any]:
    if active is None:
        return {"presence": "absent"}
    return {
        "presence": "present",
        "active_control_configuration_id": active["active_control_configuration_id"],
        "active_control_configuration_generation": active[
            "active_control_configuration_generation"
        ],
    }


def _stable_active_configuration(
    active: Mapping[str, Any],
) -> dict[str, Any]:
    return {
        key: value
        for key, value in active.items()
        if key
        not in {
            "active_control_configuration_generation",
            "active_control_configuration_id",
            "source_runtime_epoch",
            "source_sequence",
        }
    }


def _submit_native_cclc_lifecycle(
    execution_run_root: Path, *, intent_kind: str, request_id: str
) -> dict[str, Any]:
    client = CCLCOpenRequestClient(execution_run_root, timeout_seconds=5.0)
    admission = CellAdmissionClient(execution_run_root, timeout_seconds=5.0)
    owner_epoch = client.owner_identity()["request_owner_epoch"]
    snapshot = client.component_snapshot("lite6_arm")
    expected_domains = [
        {
            "control_domain_id": domain["control_domain_id"],
            "control_domain_revision": domain["control_domain_revision"],
            "controller_topology_id": domain["controller_topology_id"],
            "topology_revision": domain["topology_revision"],
            "expected_active_control_configuration": _expected_active_configuration(
                domain["active_control_configuration"]
            ),
            "expected_control_bindings": domain["current_control_bindings"],
        }
        for domain in snapshot["domains"]
    ]
    result = client.submit_lifecycle(
        {
            "request_context": {
                "request_id": request_id,
                "request_owner_epoch": owner_epoch,
                "source_label": "native-process-qualification",
            },
            "controllable_component_id": snapshot["controllable_component_id"],
            "intent": intent_kind,
            "expected_graph_revision": snapshot["graph_revision"],
            "expected_component_lifecycle_generation": snapshot[
                "component_lifecycle_generation"
            ],
            "expected_admission_generation": admission.get()["admission_generation"],
            "expected_control_domains": expected_domains,
        }
    )
    acceptance = result.get("acceptance")
    if not isinstance(acceptance, dict):
        raise RuntimeError(f"native CCLC {intent_kind} was not accepted: {result}")
    return _wait_for_cclc_terminal(
        client.lifecycle_status,
        acceptance["lifecycle_transaction_id"],
        label=intent_kind,
    )


def _qualify_native_cclc_product(
    execution_run_root: Path, *, gripper_only: bool = False
) -> None:
    _submit_native_cclc_lifecycle(
        execution_run_root,
        intent_kind="prepare",
        request_id="process.lifecycle.prepare",
    )
    # This fixture was published without Manager Apply. Establish its routes
    # through ordinary CCLC before Enable opens command admission.
    _submit_native_cclc_routes(
        execution_run_root,
        expected_domain_ids=(
            ["lite6_gripper_domain"]
            if gripper_only
            else ["lite6_arm_domain", "lite6_gripper_domain"]
        ),
    )
    _submit_native_cclc_lifecycle(
        execution_run_root,
        intent_kind="enable",
        request_id="process.lifecycle.enable",
    )

    client = CCLCOpenRequestClient(execution_run_root, timeout_seconds=5.0)
    final_snapshot = client.component_snapshot("lite6_arm")
    if final_snapshot["component_lifecycle_state"] != "active":
        raise RuntimeError(
            f"native CCLC product did not become active: {final_snapshot}"
        )


def _submit_native_cclc_routes(
    execution_run_root: Path,
    *,
    request_prefix: str = "process.route",
    expected_domain_ids: list[str] | None = None,
) -> None:
    client = CCLCOpenRequestClient(execution_run_root, timeout_seconds=5.0)
    admission = CellAdmissionClient(execution_run_root, timeout_seconds=5.0)

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
    if expected_domain_ids is None:
        expected_domain_ids = ["lite6_arm_domain", "lite6_gripper_domain"]
    if domain_ids != expected_domain_ids:
        raise RuntimeError(
            f"native CCLC product has an unexpected route set: {domain_ids}"
        )
    for domain_id in domain_ids:
        owner_epoch = client.owner_identity()["request_owner_epoch"]
        template_set = client.route_templates("lite6_arm")
        template = next(
            row
            for row in template_set["templates"]
            if row["desired_configuration"]["control_domain_id"] == domain_id
        )
        result = client.submit_route(
            {
                "request_context": context(
                    f"{request_prefix}.{domain_id}", owner_epoch
                ),
                "expected_retained_bundle_id": template_set["retained_bundle_id"],
                "expected_admission_generation": admission.get()[
                    "admission_generation"
                ],
                "expected_graph_revision": template_set["graph_revision"],
                "desired_configuration": template["desired_configuration"],
                "expected_active_control_configuration": template[
                    "expected_active_control_configuration"
                ],
                "component_expectations": template["component_expectations"],
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
    if any(
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


def _cclc_routes_are_current(
    snapshot: Mapping[str, Any], *, expected_state: str = "active"
) -> bool:
    if snapshot.get("component_lifecycle_state") != expected_state:
        return False
    provider_epochs = {
        (
            row.get("control_domain_id"),
            row.get("provider_runtime_unit_id"),
        ): row.get("provider_runtime_epoch")
        for row in snapshot.get("provider_occurrences", [])
        if isinstance(row, Mapping)
    }
    domains = snapshot.get("domains", [])
    return bool(domains) and all(
        isinstance(domain, Mapping)
        and isinstance(active := domain.get("active_control_configuration"), Mapping)
        and active.get("state") == "reconciled"
        and provider_epochs.get(
            (
                domain.get("control_domain_id"),
                active.get("source_runtime_unit_id"),
            )
        )
        == active.get("source_runtime_epoch")
        for domain in domains
    )


def _wait_for_component_lifecycle_management(execution_run_root: Path) -> None:
    """Wait until owner route recovery settled, whatever the lifecycle state."""

    execution = ExecutionCommandClient(execution_run_root, timeout_seconds=1.0)
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        try:
            if (
                execution.identity()["identity"]["control_management_mode"]
                == "component_lifecycle"
            ):
                return
        except HeadlessApiError:
            pass
        time.sleep(0.01)
    raise RuntimeError("native CCLC route recovery did not settle")


def _wait_for_current_cclc_routes(
    execution_run_root: Path,
    *,
    expected_state: str = "active",
) -> dict[str, Any]:
    client = CCLCOpenRequestClient(execution_run_root, timeout_seconds=1.0)
    execution = ExecutionCommandClient(execution_run_root, timeout_seconds=1.0)
    deadline = time.monotonic() + 10.0
    last_snapshot: dict[str, Any] | None = None
    while time.monotonic() < deadline:
        try:
            last_snapshot = client.component_snapshot("lite6_arm")
            identity = execution.identity()["identity"]
            if (
                _cclc_routes_are_current(last_snapshot, expected_state=expected_state)
                and identity["control_management_mode"] == "component_lifecycle"
            ):
                return last_snapshot
        except HeadlessApiError:
            pass
        time.sleep(0.01)
    raise RuntimeError(
        "native CCLC routes did not reconcile to the current provider: "
        f"{last_snapshot}"
    )


def _start_host(
    fixture_root: Path,
    execution_state_root: Path,
    execution_run_root: Path,
    manifest: dict[str, Any],
    *,
    candidate_worker: Path | None = None,
    fake_worker_marker: Path | None = None,
    fake_worker_failure_gate: Path | None = None,
    reconciliation_test_marker: Path | None = None,
    reconciliation_test_gate: Path | None = None,
    reconciliation_test_throw: str | None = None,
    host_environment: str = "MANYFORGE_NATIVE_EXECUTION_HOST",
    qualify_cclc: bool = True,
    include_gripper_target: bool = True,
) -> subprocess.Popen[bytes]:
    environment = os.environ.copy()
    if fake_worker_marker is not None:
        environment["MANYFORGE_FAKE_WORKER_MARKER"] = str(fake_worker_marker)
    if fake_worker_failure_gate is not None:
        environment["MANYFORGE_FAKE_WORKER_FAILURE_GATE"] = str(
            fake_worker_failure_gate
        )
    if reconciliation_test_marker is not None:
        environment["MANYFORGE_RECONCILIATION_TEST_MARKER"] = str(
            reconciliation_test_marker
        )
    if reconciliation_test_gate is not None:
        environment["MANYFORGE_RECONCILIATION_TEST_GATE"] = str(
            reconciliation_test_gate
        )
    if reconciliation_test_throw is not None:
        environment["MANYFORGE_RECONCILIATION_TEST_THROW"] = reconciliation_test_throw
    command = [
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
        "--candidate-worker",
        str(
            candidate_worker
            or _required_path("MANYFORGE_ROBOPLAN_CANDIDATE_WORKER")
        ),
        "--cell-id",
        manifest["cell_id"],
    ]
    motion_target_role_id = manifest.get("motion_target_role_id")
    if motion_target_role_id is None:
        motion_target_role_id = manifest.get("target_role_id")
    if motion_target_role_id is not None:
        command.extend(["--motion-target-role-id", motion_target_role_id])
    gripper_target_role_id = manifest.get("gripper_target_role_id")
    if include_gripper_target and gripper_target_role_id is not None:
        command.extend(
            ["--gripper-target-role-id", gripper_target_role_id]
        )
    process = subprocess.Popen(
        command,
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
                "native execution host exited during startup "
                f"(status {process.returncode}): {detail}"
            )
        try:
            response = client.identity()
            if response.get("ok") is True:
                if manifest.get("cclc_product") is True and qualify_cclc:
                    try:
                        snapshot = _wait_for_cclc_component(execution_run_root)
                        if snapshot.get("component_lifecycle_state") == "active":
                            _wait_for_current_cclc_routes(execution_run_root)
                        else:
                            while (
                                client.identity()["identity"][
                                    "control_management_mode"
                                ]
                                == "pending"
                            ):
                                assert time.monotonic() < deadline
                                time.sleep(0.01)
                            _qualify_native_cclc_product(
                                execution_run_root,
                                gripper_only=manifest.get("gripper_only") is True,
                            )
                            _wait_for_current_cclc_routes(execution_run_root)
                    except Exception as error:
                        _stop_host(process)
                        detail = (
                            process.stderr.read() if process.stderr else b""
                        ).decode(errors="replace")
                        structured_error = (
                            f" [{error.code}: {error.details}]"
                            if isinstance(error, HeadlessApiError)
                            else ""
                        )
                        raise RuntimeError(
                            "native CCLC product qualification failed: "
                            f"{error}{structured_error}; host stderr: {detail}"
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


def _unclaimed_program_submission(
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


def _manual_joint_submission(
    client: HeadlessHttpClient,
    *,
    request_id: str,
    joint_names: tuple[str, ...],
    positions: tuple[float, ...],
) -> dict[str, Any]:
    return client.execute(
        _direct_control_motion_operation(
            client.execution_diagnostics()["status"],
            request_id=request_id,
            motion={
                "kind": "joint_goal",
                "joint_names": list(joint_names),
                "positions_rad": list(positions),
            },
        )
    )


def _manual_gripper_submission(
    client: HeadlessHttpClient,
    *,
    request_id: str,
    position_kind: str,
    position: float,
    command_duration_s: float = 0.05,
) -> dict[str, Any]:
    operation = _direct_control_gripper_operation(
        client.execution_diagnostics()["status"],
        request_id=request_id,
        position=position,
    )
    payload = operation["participants"][0]["payload"]
    assert payload["command_value"]["position"]["kind"] == position_kind
    payload["caller_options"][0]["value_s"] = command_duration_s
    payload["completion_timeout_s"] = max(2.0, command_duration_s + 1.0)
    return client.execute(operation)


def _wait_terminal(
    runner: OrchestratorRunner | HeadlessHttpClient, operation_id: str
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
    admission: CellAdmissionClient,
    *,
    request_id: str,
    positions_rad: list[float],
    cache_mode: str,
    move_id: str | None = None,
) -> float:
    """Measure one complete addressed plan/execute/release cycle."""

    identity = client.identity()["identity"]
    targets = client.targets()
    assert targets["identity"] == identity
    _, direct = _execution_target(
        targets["targets"], presentation="motion"
    )
    operation = _direct_control_joint_operation(
        identity,
        direct,
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
    assert projection["state"] == "completed", json.dumps(
        terminal, indent=2, sort_keys=True
    )

    # Native manual-control settlement is backend-owned. Keep retirement
    # outside the historical timing sample without a redundant client Ack.
    _wait_retired(client, operation_id)
    # Non-residency is published only after the manual admission claim is
    # released, so admission is already reopened here.
    assert admission.get()["authoring_allowed"] is True
    return elapsed_seconds


def _wait_retired(client: ExecutionCommandClient, operation_id: str) -> None:
    """Observe exact automatic settlement; never perform client-side cleanup."""

    residency_projection: dict[str, Any] = {}
    residency_deadline = time.monotonic() + 5.0
    while True:
        residency_projection = client.read(operation_id)
        if residency_projection.get("resident") is False:
            break
        if time.monotonic() >= residency_deadline:
            raise AssertionError(
                "native execution remained resident after terminal settlement: "
                f"{residency_projection}"
            )
        time.sleep(0.005)


def _wait_state(
    runner: OrchestratorRunner | HeadlessHttpClient,
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
    raise RuntimeError(f"operation did not reach one of {sorted(expected)}: {last}")


def _wait_runtime_scale_control_applied(
    client: HeadlessHttpClient,
    selector: Mapping[str, str],
    generation: int,
    scale: float,
    *,
    active_operation_id: str | None,
) -> dict[str, Any]:
    deadline = time.monotonic() + 5.0
    last: dict[str, Any] = {}
    while time.monotonic() < deadline:
        last = client.runtime_scale_control(selector)
        status = last.get("runtime_scale_control")
        if (
            isinstance(status, dict)
            and status.get("control_generation") == generation
            and status.get("application_state") == "applied"
            and status.get("active_operation_id") == active_operation_id
            and abs(float(status.get("effective_scale", -1.0)) - scale) <= 1e-9
        ):
            return last
        time.sleep(0.005)
    raise RuntimeError(f"runtime-scale control was not applied: {last}")


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
                "expected_stop_certificate_id": certificate["stop_certificate_id"],
                "expected_stop_certificate_revision": certificate[
                    "stop_certificate_revision"
                ],
                "expected_cell_id": latch["cell_id"],
                "expected_retained_bundle_id": latch["retained_bundle_id"],
                "expected_graph_revision": latch["graph_revision"],
                "configuration_expectations": offer["configuration_expectations"],
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


def _translation_error_m(left: Mapping[str, Any], right: Mapping[str, Any]) -> float:
    return math.sqrt(
        sum(
            (float(left[axis]) - float(right[axis])) ** 2
            for axis in ("x_m", "y_m", "z_m")
        )
    )


def _rotation_error_rad(left: Mapping[str, Any], right: Mapping[str, Any]) -> float:
    dot = abs(
        sum(float(left[axis]) * float(right[axis]) for axis in ("x", "y", "z", "w"))
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
                "target_role_id": ("target-role:tool" if tool else "target-role:test"),
                "required_capability_id": (
                    "tool.gripper.command" if tool else "execution.trajectory.execute"
                ),
                "action_span": None,
                "payload": payload,
            }
        ],
        "coordination_policy": "independent",
    }


def _execution_target(
    targets: Mapping[str, Any], *, presentation: str
) -> tuple[dict[str, Any], dict[str, Any]]:
    matches = [
        (component, target)
        for component in targets["components"]
        for target in component["targets"]
        if target["presentation"] == presentation
    ]
    assert len(matches) == 1, (presentation, matches)
    component, target = matches[0]
    assert target["command"] is not None
    return component, target


def _status_target(
    status: Mapping[str, Any], *, presentation: str
) -> tuple[dict[str, Any], dict[str, Any]]:
    return _execution_target(status["targets"], presentation=presentation)


def _status_observation(
    status: Mapping[str, Any], *, presentation: str
) -> dict[str, Any]:
    component, target = _status_target(status, presentation=presentation)
    target_key = (
        component["controllable_component_id"],
        target["command_target_id"],
        target["operation_capability_id"],
    )
    matching = [
        row["observed"]
        for row in status["control_state"]["observed_targets"]
        if (
            row["controllable_component_id"],
            row["command_target_id"],
            row["operation_capability_id"],
        )
        == target_key
    ]
    assert len(matching) == 1, (target_key, matching)
    observed = matching[0]
    assert observed is not None
    return observed


def _direct_control_joint_operation(
    identity: Mapping[str, Any],
    target: dict[str, Any],
    *,
    request_id: str,
    positions_rad: list[float],
) -> dict[str, Any]:
    command = target["command"]
    manual_control = command["manual_control"]
    joint_names = [joint["source_joint_name"] for joint in command["joints"]]
    assert len(joint_names) == len(positions_rad)
    return {
        "request_context": {
            "request_id": request_id,
            "request_owner_epoch": identity["owner_epoch"],
            "source_label": "composer_ui",
            "correlation_id": None,
        },
        "program_id": manual_control["program_id"],
        "program_revision": manual_control["program_revision"],
        "program_binding_id": manual_control["program_binding_id"],
        "program_run_id": manual_control["program_run_id"],
        "expected_graph_revision": identity["graph_revision"],
        "expected_retained_bundle_id": identity["retained_bundle_id"],
        "expected_scene_revision": identity["scene_revision"],
        "participants": [
            {
                "component_role_id": command["component_role_id"],
                "target_role_id": command["target_role_id"],
                "required_capability_id": target["operation_capability_id"],
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
                    "base_frame_role_id": command["base_frame_role_id"],
                    "tcp_frame_role_id": command["tcp_frame_role_id"],
                    "planning_profile": "balanced",
                    "speed_profile": {
                        "velocity_scale": 1.0,
                        "acceleration_scale": 1.0,
                        "max_tcp_linear_speed_mps": None,
                    },
                    "timeouts": {
                        "planning_timeout_s": 2.0,
                        "execution_timeout_s": 10.0,
                    },
                },
            }
        ],
        "coordination_policy": "independent",
    }


def _direct_control_gripper_operation(
    status: dict[str, Any], *, request_id: str, position: float
) -> dict[str, Any]:
    identity = status["identity"]
    _, direct_target = _status_target(status, presentation="gripper")
    direct = direct_target["command"]
    manual_control = direct["manual_control"]
    position_value = (
        {"kind": "angular_rad", "value_rad": position}
        if direct["resource"]["coordinate_kind"] == "angular_rad"
        else {"kind": "linear_m", "value_m": position}
    )
    return {
        "request_context": {
            "request_id": request_id,
            "request_owner_epoch": identity["owner_epoch"],
            "source_label": "composer_ui",
            "correlation_id": None,
        },
        "program_id": manual_control["program_id"],
        "program_revision": manual_control["program_revision"],
        "program_binding_id": manual_control["program_binding_id"],
        "program_run_id": manual_control["program_run_id"],
        "expected_graph_revision": identity["graph_revision"],
        "expected_retained_bundle_id": identity["retained_bundle_id"],
        "expected_scene_revision": identity["scene_revision"],
        "participants": [
            {
                "component_role_id": direct["component_role_id"],
                "target_role_id": direct["target_role_id"],
                "required_capability_id": direct_target[
                    "operation_capability_id"
                ],
                "action_span": None,
                "payload": {
                    "kind": "capability_command",
                    "command_schema_id": direct["command_schema_id"],
                    "command_schema_revision": direct["command_schema_revision"],
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
    identity = status["identity"]
    _, target = _status_target(status, presentation="motion")
    observed = _status_observation(status, presentation="motion")
    operation = _direct_control_joint_operation(
        identity,
        target,
        request_id=request_id,
        positions_rad=[
            float(value) for value in observed["positions_rad"]
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
            _planning_motion("pick.approach", {"kind": "pose_goal", "goal": _pose()}),
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
        admission = CellAdmissionClient(execution_run_root, timeout_seconds=5.0).get()
        assert admission["authoring_allowed"] is False
        before = _wait_for_cclc_component(execution_run_root)
        with pytest.raises(HeadlessApiError) as rejected:
            _qualify_native_cclc_product(execution_run_root)
        assert rejected.value.code == "authoring_not_ready"
        assert _wait_for_cclc_component(execution_run_root) == before
    finally:
        _stop_host(process)


def test_arm_only_native_process_publishes_component_based_status(
    tmp_path: Path,
) -> None:
    # CMake generates this sibling fixture specifically without the independent
    # gripper domain. It is already published and does not require managed Apply.
    built_fixture = _required_path("MANYFORGE_I6_NATIVE_FIXTURE_ROOT")
    arm_only_fixture = built_fixture.parent / "lite6_native_cclc_readiness_fixture"
    assert arm_only_fixture.is_dir(), (
        "the native process test requires CMake's generated arm-only CCLC "
        f"readiness fixture beside {built_fixture}"
    )
    fixture_root = tmp_path / "fixture"
    shutil.copytree(arm_only_fixture, fixture_root)
    manifest = json.loads((fixture_root / "manifest.json").read_text("utf-8"))
    state_root, run_root = tmp_path / "state", tmp_path / "run"
    state_root.mkdir()
    run_root.mkdir()
    process = _start_host(
        fixture_root,
        state_root,
        run_root,
        manifest,
        include_gripper_target=False,
        qualify_cclc=False,
    )
    client = ExecutionCommandClient(run_root, timeout_seconds=2.0)
    try:
        identity = client.identity()["identity"]
        targets = client.targets()["targets"]
        control_state = client.control_state()["control_state"]
        observation = client.observation()["observation"]

        assert identity["schema_version"] == "manyforge.execution_identity.v2"
        assert set(identity) == {
            "active_publication_generation",
            "cell_id",
            "control_management_mode",
            "graph_revision",
            "owner_epoch",
            "retained_bundle_id",
            "scene_revision",
            "schema_version",
        }
        assert targets["schema_version"] == "manyforge.execution_targets.v3"
        supported = [
            (component, target)
            for component in targets["components"]
            for target in component["targets"]
            if target["presentation"] != "unsupported"
        ]
        assert len(supported) == 1
        component, target = supported[0]
        assert target["presentation"] == "motion"
        assert target["command"] is not None
        target_key = (
            component["controllable_component_id"],
            target["command_target_id"],
            target["operation_capability_id"],
        )

        assert control_state["schema_version"] == (
            "manyforge.execution_control_state.v6"
        )
        assert set(control_state) == {
            "cell_stop_action",
            "manual_operations",
            "observed_targets",
            "runtime_scale_control",
            "schema_version",
        }
        assert control_state["runtime_scale_control"] is not None
        control_rows = control_state["observed_targets"]
        assert len(control_rows) == 1
        control_row = control_rows[0]
        assert (
            control_row["controllable_component_id"],
            control_row["command_target_id"],
            control_row["operation_capability_id"],
        ) == target_key
        assert control_row["observed"]["kind"] == "joint_positions"

        assert observation["schema_version"] == "manyforge.execution_observation.v3"
        observation_rows = observation["targets"]
        assert len(observation_rows) == 1
        observation_row = observation_rows[0]
        assert (
            observation_row["controllable_component_id"],
            observation_row["command_target_id"],
            observation_row["operation_capability_id"],
        ) == target_key
        assert len(observation_row["positions_si"]) == 6
    finally:
        _stop_host(process)


def test_native_disable_closes_arm_and_gripper_dispatch(
    tmp_path: Path,
) -> None:
    fixture_root = tmp_path / "fixture"
    shutil.copytree(_required_path("MANYFORGE_I6_NATIVE_FIXTURE_ROOT"), fixture_root)
    manifest = json.loads((fixture_root / "manifest.json").read_text("utf-8"))
    state_root, run_root = tmp_path / "state", tmp_path / "run"
    state_root.mkdir()
    run_root.mkdir()
    process = _start_host(fixture_root, state_root, run_root, manifest)
    raw_api = TestClient(
        create_app(
            service=HeadlessService(
                UnusedCore(),  # type: ignore[arg-type]
                UnusedManager(),  # type: ignore[arg-type]
                ExecutionCommandClient(run_root, timeout_seconds=5.0),
            ),
            trusted_principal_adapter=FixedTrustedPrincipalAdapter(),
        )
    )
    headless = HeadlessHttpClient("http://headless.test", client=raw_api)
    admission = CellAdmissionClient(run_root, timeout_seconds=5.0)
    try:
        before = headless.execution_diagnostics()["status"]
        assert admission.get()["authoring_allowed"] is True
        disabled = _submit_native_cclc_lifecycle(
            run_root, intent_kind="disable", request_id="process.lifecycle.disable"
        )
        assert disabled["current_state"] == "disabled"
        snapshot = CCLCOpenRequestClient(run_root).component_snapshot("lite6_arm")
        assert snapshot["component_lifecycle_state"] == "disabled"
        assert all(
            domain["active_control_configuration"] is not None
            for domain in snapshot["domains"]
        )

        before_positions = list(
            _status_observation(before, presentation="motion")["positions_rad"]
        )
        arm_positions = list(before_positions)
        arm_positions[0] += 0.05
        for operation in (
            _direct_control_joint_operation(
                before["identity"],
                _status_target(before, presentation="motion")[1],
                request_id="disabled.arm",
                positions_rad=arm_positions,
            ),
            _direct_control_gripper_operation(
                before, request_id="disabled.gripper", position=0.02
            ),
        ):
            with pytest.raises(HeadlessClientError) as refused:
                headless.execute(operation)
            # The exact Kernel eligibility refusal, not any other failure.
            assert refused.value.details.get("reason") == (
                "component_command_not_eligible"
            ), (refused.value.code, refused.value.details)
        # No dispatch or motion: the observed arm is exactly where it was.
        after_refusal = headless.execution_diagnostics()["status"]
        assert (
            _status_observation(after_refusal, presentation="motion")["positions_rad"]
            == before_positions
        )

        # Retained Disable survives an owner restart: route recovery settles,
        # routes stay installed, and nothing re-enables the component.
        _stop_host(process)
        process = _start_host(
            fixture_root, state_root, run_root, manifest, qualify_cclc=False
        )
        _wait_for_component_lifecycle_management(run_root)
        _wait_for_current_cclc_routes(run_root, expected_state="disabled")
        restarted_snapshot = CCLCOpenRequestClient(run_root).component_snapshot(
            "lite6_arm"
        )
        assert restarted_snapshot["component_lifecycle_state"] == "disabled"
        assert restarted_snapshot["component_lifecycle_generation"] == (
            snapshot["component_lifecycle_generation"]
        )
        assert all(
            domain["active_control_configuration"] is not None
            for domain in restarted_snapshot["domains"]
        )
        restarted = headless.execution_diagnostics()["status"]
        with pytest.raises(HeadlessClientError) as refused_after_restart:
            headless.execute(
                _direct_control_joint_operation(
                    restarted["identity"],
                    _status_target(restarted, presentation="motion")[1],
                    request_id="disabled.arm.after-restart",
                    positions_rad=arm_positions,
                )
            )
        # Route verification did not grant command permission or Enable it.
        assert refused_after_restart.value.details.get("reason") == (
            "component_command_not_eligible"
        ), refused_after_restart.value.details
        with pytest.raises(HeadlessClientError) as gripper_refused_after_restart:
            headless.execute(_direct_control_gripper_operation(
                restarted, request_id="disabled.gripper.after-restart", position=0.02
            ))
        assert gripper_refused_after_restart.value.details.get("reason") == (
            "component_command_not_eligible"
        ), gripper_refused_after_restart.value.details

        _submit_native_cclc_lifecycle(
            run_root, intent_kind="prepare", request_id="process.lifecycle.reprepare"
        )
        # No manual route selection is needed after verified reconstruction.
        _submit_native_cclc_lifecycle(
            run_root, intent_kind="enable", request_id="process.lifecycle.reenable"
        )
        assert admission.get()["authoring_allowed"] is True

        # Equivalent commands with fresh identities and current expectations
        # now execute to completion on both arm and gripper.
        current = headless.execution_diagnostics()["status"]
        reenabled_positions = list(
            _status_observation(current, presentation="motion")["positions_rad"]
        )
        reenabled_positions[0] += 0.05
        arm = headless.execute(
            _direct_control_joint_operation(
                current["identity"],
                _status_target(current, presentation="motion")[1],
                request_id="reenabled.arm",
                positions_rad=reenabled_positions,
            )
        )
        arm_terminal = _wait_terminal(headless, arm["accepted"]["operation_id"])
        assert arm_terminal["operation"]["state"] == "completed", arm_terminal
        gripper = _manual_gripper_submission(
            headless,
            request_id="reenabled.gripper",
            position_kind=_status_target(
                headless.execution_diagnostics()["status"], presentation="gripper"
            )[1]["command"]["resource"]["coordinate_kind"],
            position=0.007,
        )
        gripper_terminal = _wait_terminal(headless, gripper["accepted"]["operation_id"])
        assert gripper_terminal["operation"]["state"] == "completed", gripper_terminal

        # An Active component is reconstructed command-eligible after restart.
        _stop_host(process)
        process = _start_host(
            fixture_root, state_root, run_root, manifest, qualify_cclc=False
        )
        _wait_for_current_cclc_routes(run_root)
        after_restart = headless.execution_diagnostics()["status"]
        after_restart_positions = list(
            _status_observation(after_restart, presentation="motion")["positions_rad"]
        )
        after_restart_positions[0] -= 0.05
        restarted_arm = headless.execute(
            _direct_control_joint_operation(
                after_restart["identity"],
                _status_target(after_restart, presentation="motion")[1],
                request_id="reenabled.arm.after-restart",
                positions_rad=after_restart_positions,
            )
        )
        restarted_terminal = _wait_terminal(
            headless, restarted_arm["accepted"]["operation_id"]
        )
        assert restarted_terminal["operation"]["state"] == "completed", (
            restarted_terminal
        )
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
    # Let startup establish CCLC readiness, then gate an ordinary reconciliation
    # iteration. The gate is not allowed to redefine initial route readiness.
    reconciliation_gate.touch()
    execution_state_root.mkdir()
    execution_run_root.mkdir()

    process = _start_host(
        fixture_root,
        execution_state_root,
        execution_run_root,
        manifest,
        host_environment="MANYFORGE_NATIVE_PICK_PLACE_PLANNING_FIXTURE_HOST",
        reconciliation_test_marker=reconciliation_marker,
        reconciliation_test_gate=reconciliation_gate,
    )
    reconciliation_marker.unlink(missing_ok=True)
    reconciliation_gate.unlink()
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
    runner = OrchestratorRunner(
        KernelClient(RecordingTransport(headless)),
        json.loads((fixture_root / "program_binding.json").read_text("utf-8")),
    )

    try:
        deadline = time.monotonic() + 2.0
        while not reconciliation_marker.exists() and time.monotonic() < deadline:
            time.sleep(0.005)
        assert reconciliation_marker.exists()
        time.sleep(0.02)
        assert not reconciliation_gate.exists()

        status = headless.execution_diagnostics()
        identity = headless.execution_identity()
        targets = headless.execution_targets()
        control_state = headless.execution_control_state()
        with raw_api.websocket_connect("/ws/execution/observations") as viewer:
            viewer.send_json(
                {
                    "last_applied": None,
                    "schema_version": ("manyforge.execution_observation_credit.v1"),
                }
            )
            observation = viewer.receive_json()
        accepted = _manual_joint_submission(
            headless,
            request_id="accept-while-reconciliation-gated",
            joint_names=("joint1",),
            positions=(0.1,),
        )
        operation_id = accepted["accepted"]["operation_id"]
        resident_control_state = headless.execution_control_state()
        observed = runner.operation(operation_id)

        assert status["ok"] is True
        assert identity["command"] == "identity"
        assert identity["identity"]["schema_version"] == (
            "manyforge.execution_identity.v2"
        )
        assert set(identity["identity"]) == {
            "active_publication_generation",
            "cell_id",
            "control_management_mode",
            "graph_revision",
            "owner_epoch",
            "retained_bundle_id",
            "scene_revision",
            "schema_version",
        }
        assert identity["identity"]["control_management_mode"] == (
            "component_lifecycle"
        )
        assert targets["command"] == "targets"
        assert targets["targets"]["schema_version"] == (
            "manyforge.execution_targets.v3"
        )
        target_components = targets["targets"]["components"]
        target_component_ids = [
            component["controllable_component_id"]
            for component in target_components
        ]
        assert target_component_ids == sorted(set(target_component_ids))
        target_keys: set[tuple[str, str, str]] = set()
        for component in target_components:
            assert set(component) == {
                "availability",
                "controllable_component_id",
                "targets",
            }
            availability = component["availability"]
            assert set(availability) == {"command_eligible", "reason"}
            assert availability["command_eligible"] is (
                availability["reason"] is None
            )
            target_pairs = [
                (target["command_target_id"], target["operation_capability_id"])
                for target in component["targets"]
            ]
            assert target_pairs == sorted(set(target_pairs))
            for target in component["targets"]:
                assert set(target) == {
                    "command",
                    "command_target_id",
                    "operation_capability_id",
                    "presentation",
                }
                if target["presentation"] == "unsupported":
                    assert target["command"] is None
                    continue
                command = target["command"]
                target_keys.add(
                    (
                        component["controllable_component_id"],
                        target["command_target_id"],
                        target["operation_capability_id"],
                    )
                )
                assert "observed_state" not in command
                assert "controllable_component_id" not in command
                assert "command_target_id" not in command
                assert "operation_capability_id" not in command
                assert set(command["manual_control"]) == {
                    "program_binding_id",
                    "program_id",
                    "program_revision",
                    "program_run_id",
                }
        assert control_state["command"] == "control_state"
        assert control_state["control_state"]["schema_version"] == (
            "manyforge.execution_control_state.v6"
        )
        observed_targets = control_state["control_state"]["observed_targets"]
        observed_target_keys = [
            (
                row["controllable_component_id"],
                row["command_target_id"],
                row["operation_capability_id"],
            )
            for row in observed_targets
        ]
        assert sorted(observed_target_keys) == sorted(target_keys)
        assert [row["observed"]["kind"] for row in observed_targets] == [
            "joint_positions",
            "coordinate",
        ]
        assert control_state["control_state"]["manual_operations"] == []
        manual_operations = resident_control_state["control_state"]["manual_operations"]
        # This is a real manual operation, not a fabricated captured Program.
        assert [row["operation"]["operation_id"] for row in manual_operations] == [
            operation_id
        ]
        assert targets["identity"] == control_state["identity"] == identity["identity"]
        assert observation["ok"] is True, observation
        assert observation["command"] == "observation"
        assert observation["observation"]["schema_version"] == (
            "manyforge.execution_observation.v3"
        )
        observation_targets = observation["observation"]["targets"]
        observation_target_keys = [
            (
                row["controllable_component_id"],
                row["command_target_id"],
                row["operation_capability_id"],
            )
            for row in observation_targets
        ]
        assert sorted(observation_target_keys) == sorted(observed_target_keys)
        assert observation["observation"]["targets"][0]["source_joint_names"] == [
            "joint1",
            "joint2",
            "joint3",
            "joint4",
            "joint5",
            "joint6",
        ]
        assert len(observation["observation"]["targets"][0]["positions_si"]) == 6
        encoded_sizes = {
            name: len(
                json.dumps(
                    projection,
                    allow_nan=False,
                    separators=(",", ":"),
                    sort_keys=True,
                ).encode("utf-8")
            )
            for name, projection in {
                "identity": identity,
                "targets": targets,
                "control_state": control_state,
                "observation": observation,
                "diagnostics": status,
            }.items()
        }
        assert encoded_sizes["identity"] < 2 * 1024
        # v2 carries the complete addressed target inventory alongside the
        # compatibility primary target. Keep the projection bounded while the
        # singular compatibility fields remain supported.
        assert encoded_sizes["targets"] < 32 * 1024
        assert encoded_sizes["control_state"] < 4 * 1024
        assert encoded_sizes["observation"] < 2 * 1024
        assert encoded_sizes["diagnostics"] < 32 * 1024
        assert encoded_sizes["identity"] < encoded_sizes["diagnostics"]
        assert encoded_sizes["targets"] < encoded_sizes["diagnostics"]
        assert encoded_sizes["control_state"] < encoded_sizes["diagnostics"]
        assert encoded_sizes["observation"] < encoded_sizes["control_state"]
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
        host_environment="MANYFORGE_NATIVE_PICK_PLACE_PLANNING_FIXTURE_HOST",
        reconciliation_test_marker=reconciliation_marker,
        reconciliation_test_gate=reconciliation_gate,
        reconciliation_test_throw=injected_failure,
        qualify_cclc=False,
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

        assert stderr == (
            "native command-free orphan reconciliation: resources=0\n"
            f"{injected_failure}\n"
        )
        assert process.poll() == 70
        assert not (execution_run_root / "execution-command.sock").exists()
        assert not (execution_run_root / "execution-control.sock").exists()
        assert not (execution_run_root / "execution-observation.sock").exists()
    finally:
        _stop_host(process)


def test_private_program_capture_is_claim_bound_and_cannot_mint_node_effects(
    tmp_path: Path,
) -> None:
    from manyforge_program_runner.execution_transport import (
        ProgramRunnerExecutionTransport,
    )

    fixture_root = tmp_path / "fixture"
    shutil.copytree(_required_path("MANYFORGE_I6_NATIVE_FIXTURE_ROOT"), fixture_root)
    manifest = json.loads((fixture_root / "manifest.json").read_text("utf-8"))
    state_root, run_root = tmp_path / "state", tmp_path / "run"
    state_root.mkdir()
    run_root.mkdir()
    process = _start_host(fixture_root, state_root, run_root, manifest)
    client = ProgramRunnerExecutionTransport(run_root, timeout_seconds=2.0)
    admission = CellAdmissionClient(run_root, timeout_seconds=2.0)
    try:
        identity = client.identity()["identity"]
        capture = {
            "schema_version": "manyforge.program_run_execution_capture.v1",
            "request_owner_epoch": identity["owner_epoch"],
            "program_runner_owner_epoch": "standalone-admission-fixture.program-runner",
            "claim_generation": 1,
            "program_run_id": "program-capture.1",
            **{
                key: manifest[key]
                for key in (
                    "program_id",
                    "program_revision",
                    "program_binding_id",
                    "retained_bundle_id",
                    "graph_revision",
                )
            },
        }
        with pytest.raises(HeadlessApiError) as unclaimed:
            client.register_program_run_capture(capture)
        assert unclaimed.value.details["reason"] == "program_execution_capture_stale"
        current = admission.get()
        assert current["authoring_allowed"] is True
        claim = admission.acquire(
            {
                "claim_kind": "program_cycle",
                "owner_role": "program_runner",
                "owner_epoch": capture["program_runner_owner_epoch"],
                "owner_transaction_id": capture["program_run_id"],
                "request_id": "claim.program-capture.1",
                "request_payload_digest": "sha256:" + "a" * 64,
                "expected_admission_generation": current["admission_generation"],
                "scope": {"resources": [], "configurations": []},
                "program_cycle_evidence": {
                    "draft_program_revision": capture["program_revision"],
                    "applied_program_revision": capture["program_revision"],
                },
                "intervention_evidence": None,
            }
        )
        capture["claim_generation"] = claim["claim_generation"]
        accepted = client.register_program_run_capture(capture)
        assert client.register_program_run_capture(capture) == accepted
        for changed in (
            {"table": {"nodes": []}},
            {"claim_generation": capture["claim_generation"] + 1},
            {"program_runner_owner_epoch": "another-runner"},
            {"program_binding_id": "sha256:" + "0" * 64},
        ):
            with pytest.raises(HeadlessApiError):
                client.register_program_run_capture({**capture, **changed})

        # Both authored groups contain the same logical root. Exercise the
        # real control socket: one union acceptance, exact retry, one release.
        kernel = KernelClient(client)
        before = kernel.read_program_execution_scope(capture["program_run_id"])
        selection = {
            "schema_version": "manyforge.program_execution_selection.v2",
            **{
                key: capture[key]
                for key in (
                    "program_runner_owner_epoch",
                    "claim_generation",
                    "program_run_id",
                )
            },
            "group_ids": ["selection.a", "selection.b", "selection.a"],
            "request_context": {
                "request_id": "selection.stop.1",
                "request_owner_epoch": identity["owner_epoch"],
                "source_label": "native-capture-test",
                "correlation_id": None,
            },
        }
        stopped = kernel.stop_program_selection(selection)
        assert stopped["group_ids"] == selection["group_ids"]
        assert stopped["selection_error"] is None
        assert stopped["widened_node_indices"] == []
        assert stopped["state"]["held_node_indices"] == [0]
        assert stopped["state"]["pending_effects"] == []
        revision = stopped["state"]["selection_revision"]
        assert revision == before["selection_revision"] + 1
        replay = kernel.stop_program_selection(selection)
        assert replay["replay_status"] == "replayed_exact"
        assert replay["state"]["selection_revision"] == revision
        released = kernel.release_program_selection(
            {
                **selection,
                "request_context": {
                    **selection["request_context"],
                    "request_id": "selection.resume.1",
                },
            },
            revision,
        )
        assert released["held_node_indices"] == []

        _, direct = _execution_target(
            client.targets()["targets"], presentation="motion"
        )
        observation = client.observation()["observation"]["targets"][0]
        operation = _direct_control_joint_operation(
            identity,
            direct,
            request_id="forged-node-effect",
            positions_rad=list(observation["positions_si"]),
        )
        operation.update(
            {
                key: capture[key]
                for key in (
                    "program_id",
                    "program_revision",
                    "program_binding_id",
                    "program_run_id",
                )
            }
        )
        operation["participants"][0]["execution_scope"] = {
            "node_instance_id": "root",
            "node_attempt_id": "root.1",
        }
        # The sealed fixture's root is logical. A valid claim and a
        # physically valid target cannot turn that node into an effect owner.
        with pytest.raises(HeadlessApiError) as logical:
            client.submit(operation)
        assert logical.value.details["reason"] == "program_execution_scope_unknown"
        raw = ExecutionCommandClient(run_root, timeout_seconds=2.0)
        denied = raw._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "submit",
                "operation": operation,
                "attribution": {
                    "transport": "http",
                    "assurance": "unverified",
                    "principal_id": None,
                },
            }
        )
        assert denied["ok"] is False
        assert denied["error"]["code"] == "execution_command_invalid"
        assert (
            "Program origin requires the private owner-client path"
            in denied["error"]["message"]
        )
        without_origin = copy.deepcopy(operation)
        without_origin["participants"][0].pop("execution_scope")
        missing = raw._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "submit",
                "operation": without_origin,
                "attribution": {
                    "transport": "http",
                    "assurance": "unverified",
                    "principal_id": None,
                },
            }
        )
        assert missing["ok"] is False
        assert (
            missing["error"]["details"]["reason"] == "program_execution_scope_missing"
        )
        # A different run cannot evade the capture by dropping its origin.
        without_origin["program_run_id"] = "unclaimed-foreign-run"
        foreign = raw.submit(
            OperationIntentRequest.model_validate(without_origin),
            trusted_principal_id=None,
        )
        assert foreign["ok"] is False
        assert (
            foreign["error"]["details"]["reason"] == "program_execution_scope_missing"
        )
        forged_direct = _direct_control_joint_operation(
            identity,
            direct,
            request_id="forged-direct-product",
            positions_rad=list(observation["positions_si"]),
        )
        forged_direct["program_id"] = "forged-direct-program"
        forged = raw.submit(
            OperationIntentRequest.model_validate(forged_direct),
            trusted_principal_id=None,
        )
        assert forged["ok"] is False
        assert forged["error"]["details"]["reason"] == "direct_control_product_mismatch"
        scoped_direct = _direct_control_joint_operation(
            identity,
            direct,
            request_id="scoped-direct-product",
            positions_rad=list(observation["positions_si"]),
        )
        scoped_direct["participants"][0]["execution_scope"] = {
            "node_instance_id": "root",
            "node_attempt_id": "root.1",
        }
        with pytest.raises(HeadlessApiError) as scoped:
            client.submit(scoped_direct)
        assert scoped.value.details["reason"] == "direct_control_product_mismatch"
        mixed = copy.deepcopy(operation)
        mixed["participants"].append(without_origin["participants"][0])
        mixed["coordination_policy"] = "coordinated_all"
        mixed_result = raw._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "submit",
                "operation": mixed,
                "attribution": {
                    "transport": "internal",
                    "assurance": "unverified",
                    "principal_id": None,
                },
            }
        )
        assert mixed_result["ok"] is False
        assert (
            mixed_result["error"]["details"]["reason"]
            == "program_execution_scope_missing"
        )
        stopped = client.stop_program_run(
            {
                "request_context": {
                    "request_id": "stop.program-capture.1",
                    "request_owner_epoch": identity["owner_epoch"],
                    "source_label": "native-capture-test",
                    "correlation_id": None,
                },
                "program_run_id": capture["program_run_id"],
                "reason": "Stop before any node effect",
            }
        )
        assert stopped["ok"] is True
        assert client.register_program_run_capture(capture) == accepted
        with pytest.raises(HeadlessApiError) as held:
            client.submit(operation)
        assert held.value.details["reason"] == "program_run_stop_fence_active"
        with sqlite3.connect(state_root / "operation-journal.sqlite3") as database:
            assert (
                database.execute("SELECT COUNT(*) FROM operation_snapshots").fetchone()[
                    0
                ]
                == 0
            )
    finally:
        _stop_host(process)


def test_internal_program_run_stop_uses_one_exact_priority_request(
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
    )
    client = ExecutionCommandClient(
        execution_run_root,
        timeout_seconds=5.0,
        attribution_transport="internal",
    )

    try:
        identity = client.identity()["identity"]
        _, direct = _execution_target(
            client.targets()["targets"], presentation="motion"
        )
        current = [
            float(value)
            for value in client.observation()["observation"]["targets"][0][
                "positions_si"
            ]
        ]
        goal = list(current)
        first_joint = direct["command"]["joints"][0]
        goal[0] = min(
            float(first_joint["max_position_rad"]),
            max(
                float(first_joint["min_position_rad"]),
                goal[0] + 0.2,
            ),
        )
        operation = _direct_control_joint_operation(
            identity,
            direct,
            request_id="program-run-stop.operation",
            positions_rad=goal,
        )
        operation["program_run_id"] = "program-run.stop-process-test"
        accepted = client.submit(
            OperationIntentRequest.model_validate(operation),
            trusted_principal_id=None,
        )["accepted"]
        request = ProgramRunStopIntentRequest.model_validate(
            {
                "request_context": {
                    "request_id": "program-run-stop.request",
                    "request_owner_epoch": identity["owner_epoch"],
                    "source_label": "manyforge_behavior",
                    "correlation_id": "program-run.stop-process-test",
                },
                "program_run_id": "program-run.stop-process-test",
                "reason": "process-test Program scope Stop",
            }
        )

        stopped = client.stop_program_run(request)
        replay = client.stop_program_run(request)
        assert stopped["owner_epoch"] == identity["owner_epoch"]
        assert stopped["accepted"]["request_id"] == (request.request_context.request_id)
        assert replay["accepted"]["stop_id"] == stopped["accepted"]["stop_id"]
        assert replay["accepted"]["replay_status"] == "replayed_exact"

        deadline = time.monotonic() + 10.0
        stop_status: dict[str, Any] = {}
        while time.monotonic() < deadline:
            stop_status = client.stop_status(stopped["accepted"]["stop_id"])
            if stop_status["stop_status"]["state"] in {
                "succeeded",
                "partial",
                "failed",
            }:
                break
            time.sleep(0.01)
        assert stop_status["stop_status"]["state"] == "succeeded"
        observed = client.read(accepted["operation_id"])
        assert observed["operation"]["state"] == "stopped"

        # Cross-lane fence: Stop and its status travel the control socket,
        # the release travels the ordinary one and acts only on the exact
        # Stop identity and generation it names, never on arrival order.
        stale_release = client.release_program_run_stop_fence(
            "program-run.stop-process-test",
            stopped["accepted"]["stop_id"],
            stopped["accepted"]["stop_generation"] + 1,
        )
        assert stale_release["ok"] is False, stale_release
        released = client.release_program_run_stop_fence(
            "program-run.stop-process-test",
            stopped["accepted"]["stop_id"],
            stopped["accepted"]["stop_generation"],
        )
        replayed_release = client.release_program_run_stop_fence(
            "program-run.stop-process-test",
            stopped["accepted"]["stop_id"],
            stopped["accepted"]["stop_generation"],
        )
        assert released["ok"] is True
        assert replayed_release["ok"] is True

        event_deadline = time.monotonic() + 5.0
        stop_events: list[dict[str, Any]] = []
        while time.monotonic() < event_deadline:
            stop_events = [
                event
                for event in client.events(identity["owner_epoch"], 0)["events"]
                if event.get("request_id") == request.request_context.request_id
            ]
            if stop_events:
                break
            time.sleep(0.01)
        assert stop_events

        # Releasing the Program-run hold lets the Kernel retire this clean,
        # already-settled manual operation. Do not invent a second Reset.
        _wait_retired(client, accepted["operation_id"])
        admission = CellAdmissionClient(execution_run_root)
        assert admission.get()["authoring_allowed"] is True

        next_operation = _direct_control_joint_operation(
            identity,
            direct,
            request_id="program-run-stop.next-operation",
            positions_rad=current,
        )
        next_operation["program_run_id"] = "program-run.stop-process-test.next"
        next_accepted = client.submit(
            OperationIntentRequest.model_validate(next_operation),
            trusted_principal_id=None,
        )["accepted"]
        deadline = time.monotonic() + 20.0
        next_terminal: dict[str, Any] = {}
        while time.monotonic() < deadline:
            next_terminal = client.read(next_accepted["operation_id"])["operation"]
            if next_terminal["state"] in TERMINAL_STATES:
                break
            time.sleep(0.01)
        assert next_terminal["state"] == "completed", next_terminal
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
        host_environment="MANYFORGE_NATIVE_PICK_PLACE_PLANNING_FIXTURE_HOST",
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
    composer_app = FastAPI()
    composer_app.include_router(
        create_headless_router(
            headless,
            ComposerState(headless_client=headless),
        )
    )
    raw_composer = TestClient(composer_app)
    composer = HeadlessHttpClient("http://composer.test", client=raw_composer)

    try:
        owner_epoch = native.identity()["identity"]["owner_epoch"]
        before = native.events(owner_epoch, 0)["events"]

        pick = behavior.plan_pick(_pick_planning_request())["planning_result"]
        assert pick["schema_version"] == "manyforge.pick_place_planning_result.v1"
        assert pick["sequence_id"] == "process-pick-1"
        assert pick["kind"] == "pick"
        assert pick["selected_object_id"] == "workpiece"
        assert pick["status"] == "planned"
        assert pick["attempted_candidate_count"] == 1
        assert pick["last_attempted_candidate"]["candidate_index"] == 1
        assert (
            pick["last_attempted_candidate"]["pick_cartesian_path"]["kind"]
            == "cartesian_path"
        )
        assert pick["plan"]["approach_motion"]["kind"] == "pose_goal"
        assert pick["plan"]["descend_motion"]["kind"] == "cartesian_path"
        assert pick["plan"]["retreat_motion"]["kind"] == "cartesian_path"

        place = composer.plan_place(_place_planning_request())["planning_result"]
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
        infeasible_request["approach_operation"]["participants"][0]["payload"][
            "move_id"
        ] = "pick.fixture_no_feasible.approach"
        infeasible = behavior.plan_pick(infeasible_request)
        assert infeasible["planning_result"]["status"] == ("no_feasible_candidate")
        assert infeasible["planning_result"]["attempted_candidate_count"] > 0
        assert infeasible["planning_result"]["last_attempted_candidate"]

        # A pick offset may name another robot's frame, but that robot must be
        # bound in the Program Binding, exactly like a motion reference; the
        # refusal names the failed reference.
        unbound_request = _pick_planning_request()
        unbound_request["sequence_id"] = "process-pick-unbound-offset"
        unbound_request["synthesis"]["approach_offset_reference"] = {
            "type": "frame_role",
            "frame_role_id": "frame-role:tcp",
            "target": {
                "target_role_id": "unbound-robot",
                "controllable_component_id": "unbound.component",
                "command_target_id": "unbound.target",
            },
        }
        unbound = native.plan_pick(
            PickSequencePlanningRequest.model_validate(unbound_request),
            trusted_principal_id=None,
        )
        assert unbound["ok"] is False
        assert unbound["error"]["code"] == "missing_reference"
        assert unbound["error"]["details"] == {
            "reason": "program_reference_target_role_unbound",
            "reference_type": "frame_role",
            "reference_id": "frame-role:tcp",
            "target_role_id": "unbound-robot",
        }
        assert native.events(owner_epoch, 0)["events"] == before
    finally:
        _stop_host(process)
        raw_composer.close()
        raw_api.close()


def _check_malformed_stop_recovery(
    raw_api: TestClient,
    headless: HeadlessHttpClient,
    execution: ExecutionCommandClient,
    execution_state_root: Path,
    owner_epoch: str,
    malformed_operation_id: str,
    before_recovery: Callable[[], str] | None = None,
) -> None:
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
    assert malformed_acceptance["resolution_disposition"] == ("promoted_to_full")
    assert malformed_acceptance["resolution_reason"] == ("malformed_ordinary_intent")
    malformed_terminal = _wait_terminal(headless, malformed_operation_id)["operation"]
    assert malformed_terminal["state"] == "stopped"
    assert malformed_terminal["active_stop_id"] == malformed_acceptance["stop_id"]

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

    if before_recovery is not None:
        owner_epoch = before_recovery()
    # Assessment belongs to the backend, not to a series of rejected clear
    # requests. Reading the offer must eventually show ready while the fault
    # remains latched; only the explicit request below may clear it.
    assessment_deadline = time.monotonic() + 2.0
    while True:
        offer = headless.stop_fault()["stop_fault"]
        assert offer is not None
        assert offer["latch"]["state"] == "latched"
        if offer["latch"]["recovery_status"] == "ready":
            break
        assert time.monotonic() < assessment_deadline, {
            key: offer["latch"][key]
            for key in (
                "recovery_status",
                "reason_code",
                "recovery_assessment_revision",
            )
        }
        time.sleep(0.05)
    recovered_result = _recover_active_stop_fault(
        headless,
        owner_epoch=owner_epoch,
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
            "SELECT sequence,payload_json FROM journal_records "
            "WHERE record_kind='stop_fault_cleared' AND stop_id=?",
            (malformed_acceptance["stop_id"],),
        ).fetchall()
    assert len(clear_rows) == 1
    durable_clear = json.loads(clear_rows[0][1])
    assert durable_clear[0] == (
        "manyforge.internal.stop-journal-fault-clear-projection.v2"
    )
    assert durable_clear[2][0] == recovered_result["request_id"]
    clear_page = execution.events(owner_epoch, int(clear_rows[0][0]) - 1, limit=1)
    assert len(clear_page["events"]) == 1
    clear_event = clear_page["events"][0]
    assert clear_event["record_kind"] == "stop_fault_cleared"
    assert clear_event["request_id"] == recovered_result["request_id"]
    assert clear_event["operation_id"] is None
    assert clear_event["operation_revision"] is None
    clear_history = clear_event["payload"]
    assert clear_history["api_version"] == ("manyforge.stop_fault_clear_projection.v1")
    assert clear_history["fault_latch"]["latch_id"] == (
        recovered_result["latch"]["latch_id"]
    )
    assert clear_history["fault_latch"]["state"] == "cleared"
    assert clear_history["fault_latch"]["recovery_status"] == "cleared"
    assert clear_history["result"]["accepted"] is True


def test_events_sample_live_progress_without_durable_progress_records(tmp_path: Path) -> None:
    fixture_root = tmp_path / "fixture"
    shutil.copytree(_required_path("MANYFORGE_I6_NATIVE_FIXTURE_ROOT"), fixture_root)
    manifest = json.loads((fixture_root / "manifest.json").read_text("utf-8"))
    state_root, run_root = tmp_path / "state", tmp_path / "run"
    state_root.mkdir()
    run_root.mkdir()
    process = _start_host(fixture_root, state_root, run_root, manifest)
    execution = ExecutionCommandClient(run_root, timeout_seconds=3.0)
    try:
        status = execution.diagnostics()["status"]
        request = _direct_control_gripper_operation(status, request_id="progress-only", position=0.004)
        payload = request["participants"][0]["payload"]
        payload["caller_options"][0]["value_s"] = 3.0
        payload["completion_timeout_s"] = 5.0
        accepted = execution.submit(OperationIntentRequest.model_validate(request), trusted_principal_id="test")
        assert accepted.get("ok") is True, accepted
        operation_id = accepted["accepted"]["operation_id"]
        epoch = status["identity"]["owner_epoch"]
        cursor = 0
        previous = None
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            page = execution.events(epoch, cursor)
            cursor = page["next_after_sequence"]
            sample = next((row for row in page["operation_progress"]
                           if row["operation_id"] == operation_id), None)
            if sample is not None:
                assert 0 <= sample["progress"] <= 1
                if (previous is not None and not page["events"]
                        and sample["operation_revision"] == previous["operation_revision"]
                        and sample["progress"] > previous["progress"]):
                    break
                previous = sample
            time.sleep(0.02)
        else:
            pytest.fail("native progress did not advance independently of journal events")
    finally:
        _stop_host(process)


@pytest.mark.parametrize("restart_with_fault", [False, True])
def test_manual_motion_malformed_stop_recovers_with_durable_evidence(
    tmp_path: Path,
    restart_with_fault: bool,
) -> None:
    fixture_root = tmp_path / "fixture"
    shutil.copytree(_required_path("MANYFORGE_I6_NATIVE_FIXTURE_ROOT"), fixture_root)
    manifest = json.loads((fixture_root / "manifest.json").read_text("utf-8"))
    state_root, run_root = tmp_path / "state", tmp_path / "run"
    state_root.mkdir()
    run_root.mkdir()
    process = _start_host(fixture_root, state_root, run_root, manifest)
    execution = ExecutionCommandClient(run_root, timeout_seconds=3.0)
    raw_api = TestClient(
        create_app(
            service=HeadlessService(UnusedCore(), UnusedManager(), execution),
            trusted_principal_adapter=FixedTrustedPrincipalAdapter(),
        )
    )
    headless = HeadlessHttpClient("http://headless.test", client=raw_api)
    try:
        status = headless.execution_diagnostics()["status"]
        positions = list(_status_observation(status, presentation="motion")["positions_rad"])
        positions[0] = 0.55
        request = _direct_control_joint_operation(
            status["identity"],
            _status_target(status, presentation="motion")[1],
            request_id="manual-malformed-stop-operation",
            positions_rad=positions,
        )
        accepted = headless.execute(request)
        operation_id = accepted["accepted"]["operation_id"]
        _wait_state(headless, operation_id, {"executing"})

        def cclc_recovery_footprint() -> dict[str, list[tuple[Any, ...]]]:
            with sqlite3.connect(state_root / "cclc-journal.sqlite3") as database:
                return {
                    "owners": database.execute(
                        "SELECT owner_epoch FROM cclc_owner_epochs ORDER BY epoch_sequence"
                    ).fetchall(),
                    "lifecycle_effects": database.execute(
                        "SELECT attempt_sequence, effect_kind, "
                        "completed_attempt_blob IS NOT NULL "
                        "FROM cclc_effect_attempts ORDER BY attempt_sequence"
                    ).fetchall(),
                    "route_effects": database.execute(
                        "SELECT attempt_sequence, effect_kind, "
                        "completed_attempt_blob IS NOT NULL "
                        "FROM cclc_route_effect_attempts ORDER BY attempt_sequence"
                    ).fetchall(),
                }

        retained_cclc_footprint = None

        def restart_before_recovery() -> str:
            nonlocal process, retained_cclc_footprint
            fault = headless.stop_fault()["stop_fault"]
            assert fault is not None
            _stop_host(process)
            predecessor_footprint = cclc_recovery_footprint()
            process = _start_host(
                fixture_root, state_root, run_root, manifest, qualify_cclc=False
            )
            restored = headless.stop_fault()["stop_fault"]
            assert restored is not None
            assert restored["latch"]["latch_id"] == fault["latch"]["latch_id"]
            assert restored["latch"]["state"] != "cleared"
            assert CellAdmissionClient(run_root).get()["authoring_allowed"] is False
            assert _wait_for_cclc_component(run_root) is not None
            _wait_for_current_cclc_routes(run_root)
            # Route readiness arrives asynchronously; it cannot clear the fault.
            assert CellAdmissionClient(run_root).get()["authoring_allowed"] is False
            retained_cclc_footprint = cclc_recovery_footprint()
            assert len(retained_cclc_footprint["owners"]) == len(
                predecessor_footprint["owners"]
            ) + 1
            assert (
                retained_cclc_footprint["lifecycle_effects"]
                == predecessor_footprint["lifecycle_effects"]
            )
            predecessor_route_sequence = max(
                (row[0] for row in predecessor_footprint["route_effects"]),
                default=0,
            )
            recovery_effects = [
                row
                for row in retained_cclc_footprint["route_effects"]
                if row[0] > predecessor_route_sequence
            ]
            assert recovery_effects
            assert all(
                row[1]
                in {
                    "kernel_route_configuration_fence_acquire",
                    "kernel_route_configuration_fence_release",
                }
                and row[2] == 1
                for row in recovery_effects
            )
            return headless.execution_identity()["identity"]["owner_epoch"]

        _check_malformed_stop_recovery(
            raw_api,
            headless,
            execution,
            state_root,
            status["identity"]["owner_epoch"],
            operation_id,
            restart_before_recovery if restart_with_fault else None,
        )
        if restart_with_fault:
            assert cclc_recovery_footprint() == retained_cclc_footprint
        assert CellAdmissionClient(run_root).get()["authoring_allowed"] is True
        assert _wait_for_cclc_component(run_root) is not None
        retained = headless.operation(operation_id)["operation"]
        journal_path = state_root / "operation-journal.sqlite3"
        journal_identity = (journal_path.stat().st_dev, journal_path.stat().st_ino)
        run_identity = (run_root.stat().st_dev, run_root.stat().st_ino)
        _stop_host(process)
        assert (run_root.stat().st_dev, run_root.stat().st_ino) == run_identity
        assert not (run_root / "execution-command.sock").exists()
        assert not (run_root / "execution-observation.sock").exists()
        process = _start_host(fixture_root, state_root, run_root, manifest)
        assert (run_root.stat().st_dev, run_root.stat().st_ino) == run_identity
        assert (run_root / "execution-command.sock").is_socket()
        assert (run_root / "execution-observation.sock").is_socket()
        assert (
            journal_path.stat().st_dev,
            journal_path.stat().st_ino,
        ) == journal_identity
        restarted = headless.execution_diagnostics()["status"]
        assert (
            restarted["identity"]["owner_epoch"]
            != status["identity"]["owner_epoch"]
        )
        with pytest.raises(HeadlessClientError) as stale:
            headless.execute(request)
        assert stale.value.code == "stale_request_owner_epoch"
        assert stale.value.status_code == 409
        assert headless.operation(operation_id)["operation"] == retained
        assert headless.stop_fault()["stop_fault"] is None
    finally:
        _stop_host(process)
        raw_api.close()


def test_native_manual_preview_control_and_restart_contracts(tmp_path: Path) -> None:
    built_fixture = _required_path("MANYFORGE_I6_NATIVE_FIXTURE_ROOT")
    fixture_root = tmp_path / "fixture"
    shutil.copytree(built_fixture, fixture_root)
    manifest = json.loads((fixture_root / "manifest.json").read_text("utf-8"))
    assert manifest["api_version"] == ("manyforge.i6.native-execution-fixture.v1")
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
    runner = OrchestratorRunner(
        KernelClient(recording),
        json.loads((fixture_root / "program_binding.json").read_text("utf-8")),
    )

    composer_app = FastAPI()
    composer_app.include_router(
        create_headless_router(
            headless,
            ComposerState(headless_client=headless),
        )
    )
    raw_composer = TestClient(composer_app)
    composer = HeadlessHttpClient("http://composer.test", client=raw_composer)

    try:
        first_status = headless.execution_diagnostics()["status"]
        assert first_status["schema_version"] == "manyforge.execution_status.v2"
        identity = first_status["identity"]
        assert identity["retained_bundle_id"] == manifest["retained_bundle_id"]
        assert first_status["control_state"]["cell_stop_action"] == {
            "available": True,
            "expected_stop_generation": 0,
            "fault": None,
            "latest_terminal": None,
            "unavailable_reason": None,
        }
        motion_component, motion_target = _status_target(
            first_status, presentation="motion"
        )
        direct = motion_target["command"]
        direct_manual_control = direct["manual_control"]
        motion_observed = _status_observation(
            first_status, presentation="motion"
        )
        gripper_component, gripper_target = _status_target(
            first_status, presentation="gripper"
        )
        direct_gripper = gripper_target["command"]
        gripper_observed = _status_observation(
            first_status, presentation="gripper"
        )
        assert motion_observed["kind"] == "joint_positions"
        assert len(motion_observed["positions_rad"]) == len(
            motion_observed["source_joint_names"]
        )
        assert gripper_observed["position"] == {
            "kind": "linear_m",
            "value_m": pytest.approx(0.0),
        }
        # This fixture models one Lite6 component with an integrated gripper;
        # independent arm/gripper ownership is covered by the component-set
        # transition tests and the gripper-only managed-process fixture.
        assert (
            motion_component["controllable_component_id"]
            == gripper_component["controllable_component_id"]
        )
        assert direct_manual_control["program_id"] == "manyforge.direct_control.motion"
        assert direct_manual_control["program_binding_id"] != manifest["program_binding_id"]
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
        assert motion_observed["source_joint_names"] == manifest["source_joint_names"]
        current_orientation = motion_observed["current_tcp_pose"]["orientation"]
        assert sum(value * value for value in current_orientation.values()) == (
            pytest.approx(1.0)
        )
        assert all(
            joint["min_position_rad"] <= joint["max_position_rad"]
            and joint["max_velocity_rad_s"] == pytest.approx(2.14)
            and joint["max_acceleration_rad_s2"] == pytest.approx(10.0)
            for joint in direct["joints"]
        )
        motion_descriptor = direct["target_descriptor"]
        assert motion_descriptor["schema_version"] == (
            "manyforge.manual_control_motion_descriptor.v5"
        )
        assert (
            motion_descriptor["controllable_component_id"]
            == motion_component["controllable_component_id"]
        )
        assert motion_descriptor["command_target_id"] == motion_target["command_target_id"]
        assert [
            joint["source_joint_name"] for joint in motion_descriptor["joints"]
        ] == manifest["source_joint_names"]
        assert all(
            joint["max_velocity_rad_s"] == pytest.approx(2.14)
            and joint["max_acceleration_rad_s2"] == pytest.approx(10.0)
            for joint in motion_descriptor["joints"]
        )
        assert [
            limit["source_joint_name"]
            for limit in motion_descriptor["operational_joint_limits"]
        ] == manifest["source_joint_names"]
        operational_limits = motion_descriptor["operational_joint_limits"]
        assert all(
            limit["model_max_velocity_rad_s"] == pytest.approx(2.14)
            and limit["model_max_acceleration_rad_s2"] == pytest.approx(10.0)
            for limit in operational_limits
        )
        assert operational_limits[0]["source_joint_name"] == "joint1"
        assert operational_limits[0]["deployment_max_velocity_rad_s"] == (
            pytest.approx(1.5)
        )
        assert operational_limits[0]["deployment_max_acceleration_rad_s2"] == (
            pytest.approx(2.5)
        )
        assert operational_limits[0]["deployment_max_jerk_rad_s3"] is None
        assert operational_limits[0]["effective_max_velocity_rad_s"] == (
            pytest.approx(1.5)
        )
        assert operational_limits[0]["effective_max_acceleration_rad_s2"] == (
            pytest.approx(2.5)
        )
        assert operational_limits[0]["effective_max_jerk_rad_s3"] is None
        assert all(
            limit["deployment_max_velocity_rad_s"] is None
            and limit["deployment_max_acceleration_rad_s2"] is None
            and limit["deployment_max_jerk_rad_s3"] is None
            and limit["effective_max_velocity_rad_s"] == pytest.approx(2.14)
            and limit["effective_max_acceleration_rad_s2"] == pytest.approx(10.0)
            and limit["effective_max_jerk_rad_s3"] is None
            for limit in operational_limits[1:]
        )
        assert {frame["frame_role_id"] for frame in motion_descriptor["frames"]} >= {
            "base",
            "tcp",
        }
        assert motion_descriptor["contracts"]["operation_capability_id"] == (
            motion_target["operation_capability_id"]
        )
        assert motion_descriptor["contracts"]["supporting_providers"]
        assert (
            motion_descriptor["contracts"]["control_path"]["stop_evidence_policy_id"]
            == "lite6_motion_controlled_stop"
        )

        assert direct_gripper["manual_control"]["program_id"] == (
            "manyforge.direct_control.gripper"
        )
        assert direct_gripper["manual_control"]["program_binding_id"] != (
            manifest["program_binding_id"]
        )
        assert (
            direct_gripper["command_schema_id"] == manifest["gripper_command_schema_id"]
        )
        assert direct_gripper["resource"]["coordinate_kind"] == "linear_m"
        assert direct_gripper["resource"]["source_coordinate_name"]
        assert direct_gripper["resource"]["min_position"] <= (
            direct_gripper["resource"]["max_position"]
        )
        gripper_descriptor = direct_gripper["target_descriptor"]
        assert gripper_descriptor["schema_version"] == (
            "manyforge.manual_control_gripper_descriptor.v2"
        )
        assert gripper_descriptor["description"] == (
            "Native simulated UFACTORY lite pneumatic gripper."
        )
        assert gripper_descriptor["command_kind"] == "gripper_position"
        assert gripper_descriptor["actuation_kind"] == "position_commanded"
        assert gripper_descriptor["transport_kind"] == "resolved_binding"
        assert gripper_descriptor["state_authority_class"] == "predicted"
        assert gripper_descriptor["state_authority_id"]
        assert gripper_descriptor["completion_policy"] == ("goal_fenced_target_reached")
        assert gripper_descriptor["grasp_confirmation_policy"] == (
            "binding_feedback_required"
        )
        assert gripper_descriptor["source_group_name"] == "lite_gripper"
        assert gripper_descriptor["command_duration_s"] == pytest.approx(0.8)
        assert gripper_descriptor["open_state_ref"] == "open"
        assert gripper_descriptor["closed_state_ref"] == "closed"
        assert gripper_descriptor["touch_links"] == [
            "lite_gripper_base_link",
            "lite_gripper_left_finger",
            "lite_gripper_right_finger",
        ]
        assert gripper_descriptor["mount_frame"] == "link_eef"
        assert gripper_descriptor["attach_frame"] == "link_tcp"
        assert gripper_descriptor["tcp_frame"] == "link_tcp"
        assert gripper_descriptor["auxiliary_frames"] == ["lite_gripper_base_link"]
        assert gripper_descriptor["source_coordinate_name"] == (
            direct_gripper["resource"]["source_coordinate_name"]
        )
        assert gripper_descriptor["coordinate_kind"] == "linear_m"
        assert gripper_descriptor["unit"] == "m"
        assert (
            gripper_descriptor["min_position"]
            == direct_gripper["resource"]["min_position"]
        )
        assert (
            gripper_descriptor["max_position"]
            == direct_gripper["resource"]["max_position"]
        )
        assert [preset["name"] for preset in gripper_descriptor["named_presets"]] == [
            "close",
            "closed",
            "open",
        ]
        assert len(gripper_descriptor["mimic_followers"]) == 1
        follower = gripper_descriptor["mimic_followers"][0]
        assert follower["source_coordinate_name"] == "left_finger_joint"
        assert follower["compiled_coordinate_name"].startswith("mf2_joint_")
        assert follower["multiplier"] == pytest.approx(1.0)
        assert follower["offset"] == pytest.approx(0.0)
        assert gripper_descriptor["contracts"]["command_schema"] == {
            "command_schema_id": direct_gripper["command_schema_id"],
            "command_schema_revision": direct_gripper["command_schema_revision"],
        }
        assert (
            gripper_descriptor["contracts"]["control_path"]["stop_evidence_policy_id"]
            == "lite6_controller_goal_terminal_stop"
        )

        native = ExecutionCommandClient(execution_run_root, timeout_seconds=2.0)
        before_preview_events = native.events(identity["owner_epoch"], 0)["events"]
        before_preview_state = copy.deepcopy(motion_observed["positions_rad"])
        before_gripper_preview_state = copy.deepcopy(gripper_observed["position"])
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
        assert (
            gripper_receipt["program_binding_id"]
            == direct_gripper["manual_control"]["program_binding_id"]
        )
        assert native.events(identity["owner_epoch"], 0)["events"] == (
            before_preview_events
        )
        after_gripper_preview = headless.execution_diagnostics()["status"]
        after_gripper_observed = _status_observation(
            after_gripper_preview, presentation="gripper"
        )
        assert after_gripper_observed["position"] == before_gripper_preview_state
        direct_positions = [
            float(position) for position in motion_observed["positions_rad"]
        ]
        direct_positions[0] = 0.03
        direct_intent = _direct_control_joint_operation(
            identity,
            motion_target,
            request_id="composer-direct-preview",
            positions_rad=direct_positions,
        )
        preview = composer.preview_motion(direct_intent)
        receipt = preview["preview_receipt"]
        assert receipt["commit_eligible"] is True
        assert receipt["source_joint_names"] == manifest["source_joint_names"]
        assert receipt["final_positions_rad"] == pytest.approx(direct_positions)
        assert receipt["program_binding_id"] == direct_manual_control["program_binding_id"]
        assert "trajectory" not in receipt
        assert "compiled" not in receipt
        assert "providers" not in receipt
        # A settled preview must release its exact collision-projection lease.
        # The retained publisher has a fixed capacity of 64; 65 distinct
        # requests therefore prove the host does not leak one lease per
        # preview before Robot Control can be used again.
        for preview_index in range(65):
            repeated_intent = copy.deepcopy(direct_intent)
            repeated_intent["request_context"][
                "request_id"
            ] = f"composer-direct-preview-retention-{preview_index}"
            repeated_receipt = composer.preview_motion(repeated_intent)[
                "preview_receipt"
            ]
            assert repeated_receipt["commit_eligible"] is True
            assert repeated_receipt["final_positions_rad"] == pytest.approx(
                direct_positions
            )
        pose_intent = _direct_control_motion_operation(
            first_status,
            request_id="composer-direct-pose-preview",
            motion={
                "kind": "pose_goal",
                "goal": receipt["final_tcp_pose"],
            },
        )
        pose_preview = composer.preview_motion(pose_intent)["preview_receipt"]
        pose_tolerances = pose_intent["participants"][0]["payload"]["tolerances"]
        assert (
            _translation_error_m(
                pose_preview["final_tcp_pose"]["position_m"],
                receipt["final_tcp_pose"]["position_m"],
            )
            <= pose_tolerances["translation_tol_m"]
        )
        assert (
            _rotation_error_rad(
                pose_preview["final_tcp_pose"]["orientation"],
                receipt["final_tcp_pose"]["orientation"],
            )
            <= pose_tolerances["rotation_tol_rad"]
        )
        cartesian_intent = _direct_control_motion_operation(
            first_status,
            request_id="composer-direct-cartesian-preview",
            motion={
                "kind": "cartesian_path",
                "waypoints": [
                    motion_observed["current_tcp_pose"],
                    receipt["final_tcp_pose"],
                ],
                "max_translation_step_m": 0.005,
                "max_rotation_step_rad": 0.05,
                "speed_mode": "bounded",
                "max_angular_speed_rad_s": 0.5,
                "max_linear_acceleration_mps2": 0.5,
                "max_angular_acceleration_rad_s2": 2.5,
                "fallback_policy": "fail_closed",
            },
        )
        cartesian_preview = composer.preview_motion(cartesian_intent)["preview_receipt"]
        assert cartesian_preview["commit_eligible"] is True
        assert native.events(identity["owner_epoch"], 0)["events"] == (
            before_preview_events
        )
        after_preview_status = headless.execution_diagnostics()["status"]
        after_preview_arm = _status_observation(
            after_preview_status, presentation="motion"
        )
        assert after_preview_arm["positions_rad"] == before_preview_state

        stale_scene_intent = copy.deepcopy(direct_intent)
        stale_scene_intent["request_context"][
            "request_id"
        ] = "composer-direct-stale-scene"
        stale_scene_intent["expected_scene_revision"]["revision_id"] += 1
        with pytest.raises(HeadlessClientError) as stale_scene:
            composer.preview_motion(stale_scene_intent)
        assert stale_scene.value.code == "scene_changed_during_validation"
        # The host accepts an earlier revision of the same Scene (a status read
        # that lags a scene edit or a before-planning latch) but never another
        # Scene lineage. This standalone fixture has no Scene owner, so the
        # earlier-revision acceptance is covered where a Scene can advance.
        other_scene_intent = copy.deepcopy(direct_intent)
        other_scene_intent["request_context"][
            "request_id"
        ] = "composer-direct-other-scene"
        other_scene_intent["expected_scene_revision"]["scene_id"] = (
            "sha256:" + "0" * 64
        )
        with pytest.raises(HeadlessClientError) as other_scene:
            composer.preview_motion(other_scene_intent)
        assert other_scene.value.code == "scene_changed_during_validation"

        direct_accepted = composer.execute(direct_intent)
        direct_operation_id = direct_accepted["accepted"]["operation_id"]
        direct_terminal = _wait_terminal(runner, direct_operation_id)["operation"]
        assert direct_terminal["state"] == "completed", direct_terminal
        assert direct_terminal["program_binding_id"] == direct_manual_control[
            "program_binding_id"
        ]
        _wait_retired(native, direct_operation_id)

        relative_status = headless.execution_diagnostics()["status"]
        _, relative_target = _status_target(relative_status, presentation="motion")
        relative_direct = relative_target["command"]
        relative_start_pose = _status_observation(
            relative_status, presentation="motion"
        )["current_tcp_pose"]
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
        relative_terminal = _wait_terminal(runner, relative_operation_id)["operation"]
        assert relative_terminal["state"] == "completed", relative_terminal
        relative_end_pose = _status_observation(
            headless.execution_diagnostics()["status"], presentation="motion"
        )["current_tcp_pose"]
        assert (
            _translation_error_m(
                relative_end_pose["position_m"], relative_start_pose["position_m"]
            )
            <= relative_payload["tolerances"]["translation_tol_m"]
        )
        assert (
            _rotation_error_rad(
                relative_end_pose["orientation"], relative_start_pose["orientation"]
            )
            <= relative_payload["tolerances"]["rotation_tol_rad"]
        )
        _wait_retired(native, relative_operation_id)

        # An absolute pose expressed in a named frame is resolved by the
        # native cell resolver from the admission-time state. The TCP frame
        # with an identity goal therefore holds the current TCP pose.
        framed_status = headless.execution_diagnostics()["status"]
        framed_start_pose = _status_observation(
            framed_status, presentation="motion"
        )["current_tcp_pose"]
        framed_operation = OperationIntentRequest.model_validate(
            _direct_control_motion_operation(
                framed_status,
                request_id="native-direct-framed-absolute-pose",
                motion={"kind": "pose_goal", "goal": _pose()},
            )
        ).model_dump(mode="json")
        framed_payload = framed_operation["participants"][0]["payload"]
        assert framed_payload["target_mode"] == "absolute"
        framed_payload["goal_frame_reference"] = {
            "type": "frame_role",
            "frame_role_id": relative_direct["tcp_frame_role_id"],
        }
        native_commands = ExecutionCommandClient(
            execution_run_root, timeout_seconds=10.0
        )
        framed_submit = {
            "api_version": EXECUTION_COMMAND_VERSION,
            "attribution": native_commands._request_attribution(
                "operator.process-test"
            ),
            "command": "submit",
        }
        relative_framed = copy.deepcopy(framed_operation)
        relative_framed["request_context"]["request_id"] = "native-direct-framed-relative"
        relative_framed["participants"][0]["payload"] |= {
            "target_mode": "relative",
            "target_reference": framed_payload["goal_frame_reference"],
        }
        malformed = native_commands._exchange(
            {**framed_submit, "operation": relative_framed}
        )
        assert malformed["ok"] is False, malformed
        framed_response = native_commands._exchange(
            {**framed_submit, "operation": framed_operation}
        )
        assert framed_response["ok"] is True, framed_response
        framed_operation_id = framed_response["accepted"]["operation_id"]
        framed_terminal = _wait_terminal(runner, framed_operation_id)["operation"]
        assert framed_terminal["state"] == "completed", framed_terminal
        framed_end_pose = _status_observation(
            headless.execution_diagnostics()["status"], presentation="motion"
        )["current_tcp_pose"]
        assert (
            _translation_error_m(
                framed_end_pose["position_m"], framed_start_pose["position_m"]
            )
            <= framed_payload["tolerances"]["translation_tol_m"]
        )
        assert (
            _rotation_error_rad(
                framed_end_pose["orientation"], framed_start_pose["orientation"]
            )
            <= framed_payload["tolerances"]["rotation_tol_rad"]
        )
        _wait_retired(native, framed_operation_id)

        direct_gripper_accepted = composer.execute(gripper_intent)
        direct_gripper_operation_id = direct_gripper_accepted["accepted"][
            "operation_id"
        ]
        direct_gripper_terminal = _wait_terminal(runner, direct_gripper_operation_id)[
            "operation"
        ]
        assert direct_gripper_terminal["state"] == "completed"
        assert direct_gripper_terminal["program_binding_id"] == (
            direct_gripper["manual_control"]["program_binding_id"]
        )
        _wait_retired(native, direct_gripper_operation_id)

        # Public callers cannot impersonate a captured Program effect. Keep
        # that boundary check separate from payload validation below.
        with pytest.raises(KernelOperationAdmissionRejectedError) as forged:
            _unclaimed_program_submission(
                runner,
                request_id="reject-public-program-origin",
                joint_names=("arm_joints",),
                positions=(0.05,),
            )
        assert forged.value.code == "invalid_request"
        assert recording.last_submission is not None
        raw_forged = raw_api.post(
            "/api/execution/operations", json=recording.last_submission
        )
        assert raw_forged.status_code == 400
        assert raw_forged.json()["error"]["details"]["reason"] == (
            "program_execution_scope_missing"
        )

        invalid_joint_intent = _direct_control_motion_operation(
            headless.execution_diagnostics()["status"],
            request_id="reject-unknown-source-joint",
            motion={
                "kind": "joint_goal",
                "joint_names": ["arm_joints"],
                "positions_rad": [0.05],
            },
        )
        with pytest.raises(HeadlessClientError) as rejected:
            composer.execute(invalid_joint_intent)
        assert rejected.value.code == "execution_rejected", vars(rejected.value)
        assert rejected.value.status_code == 400
        invalid_joint_intent["request_context"]["request_id"] += "-raw"
        raw_rejection = raw_api.post(
            "/api/execution/operations", json=invalid_joint_intent
        )
        assert raw_rejection.status_code == 400, raw_rejection.json()
        assert raw_rejection.json()["error"]["details"]["reason"] == (
            "joint_goal_source_joint_unresolved"
        )

        gripper_status = headless.execution_diagnostics()["status"]
        wrong_unit_intent = _direct_control_gripper_operation(
            gripper_status, request_id="reject-angular-gripper-unit", position=0.004
        )
        wrong_unit_intent["participants"][0]["payload"]["command_value"]["position"] = {
            "kind": "angular_rad",
            "value_rad": 0.004,
        }
        assert wrong_unit_intent["program_binding_id"] == (
            _status_target(gripper_status, presentation="gripper")[1]["command"][
                "manual_control"
            ]["program_binding_id"]
        )
        with pytest.raises(HeadlessClientError) as wrong_unit:
            composer.execute(wrong_unit_intent)
        assert wrong_unit.value.status_code == 400
        wrong_unit_intent["request_context"]["request_id"] += "-raw"
        raw_wrong_unit = raw_api.post(
            "/api/execution/operations", json=wrong_unit_intent
        )
        assert raw_wrong_unit.status_code == 400, raw_wrong_unit.json()
        assert raw_wrong_unit.json()["error"]["details"]["reason"] == (
            "runtime_command_resource_unit_mismatch"
        )

        beyond_limit_intent = _direct_control_gripper_operation(
            gripper_status, request_id="reject-gripper-limit", position=0.02
        )
        assert beyond_limit_intent["program_binding_id"] == (
            _status_target(gripper_status, presentation="gripper")[1]["command"][
                "manual_control"
            ]["program_binding_id"]
        )
        with pytest.raises(HeadlessClientError) as beyond_limit:
            composer.execute(beyond_limit_intent)
        assert beyond_limit.value.status_code == 400
        beyond_limit_intent["request_context"]["request_id"] += "-raw"
        raw_beyond_limit = raw_api.post(
            "/api/execution/operations", json=beyond_limit_intent
        )
        assert raw_beyond_limit.status_code == 400, raw_beyond_limit.json()
        assert raw_beyond_limit.json()["error"]["code"] == "invalid_request"
        assert raw_beyond_limit.json()["error"]["details"]["reason"] == (
            "gripper_position_limit_violation"
        )

        subset_intent = _direct_control_motion_operation(
            headless.execution_diagnostics()["status"],
            request_id="accepted-source-joint-subset",
            motion={
                "kind": "joint_goal",
                "joint_names": ["joint1"],
                "positions_rad": [0.05],
            },
        )
        accepted = composer.execute(subset_intent)
        stale_submission = copy.deepcopy(subset_intent)
        serialized = json.dumps(stale_submission, sort_keys=True)
        assert "robot_id" not in serialized
        assert "group_id" not in serialized
        operation_id = accepted["accepted"]["operation_id"]
        terminal = _wait_terminal(runner, operation_id)
        assert terminal["operation"]["state"] == "completed", terminal
        # Compare one settled projection; residency is transient until the
        # Kernel retires the operation and releases its admission claim.
        _wait_retired(native, operation_id)

        composer_projection = composer.operation(operation_id)
        current_projection = headless.operation(operation_id)
        assert composer_projection == current_projection
        assert composer_projection["operation"]["state"] == "completed"
        assert composer.execution_diagnostics()["status"]["identity"]["owner_epoch"] == (
            identity["owner_epoch"]
        )

        _wait_retired(native, operation_id)
        acknowledged_projection = composer.operation(operation_id)
        with sqlite3.connect(
            execution_state_root / "operation-journal.sqlite3"
        ) as database:
            acknowledgements = [
                json.loads(row[0])
                for row in database.execute(
                    "SELECT payload_json FROM journal_records "
                    "WHERE operation_id=? AND record_kind='control_accepted'",
                    (operation_id,),
                )
            ]
        assert len(acknowledgements) == 1
        acknowledged = acknowledgements[0]["result"]
        assert acknowledged["control"] == "acknowledge_completed"
        assert acknowledged_projection["operation"]["operation_revision"] == (
            acknowledged["operation_revision_after_acceptance"]
        )

        completed_gripper_intent = _direct_control_gripper_operation(
            headless.execution_diagnostics()["status"],
            request_id="accepted-linear-gripper",
            position=0.004,
        )
        completed_gripper_intent["participants"][0]["payload"]["caller_options"][0][
            "value_s"
        ] = 0.05
        gripper_accepted = composer.execute(completed_gripper_intent)
        gripper_id = gripper_accepted["accepted"]["operation_id"]
        gripper_terminal = _wait_terminal(runner, gripper_id)["operation"]
        assert gripper_terminal["state"] == "completed"
        assert gripper_terminal["participants"][0]["payload_kind"] == (
            "capability_command"
        )
        durable_gripper = composer.operation(gripper_id)["operation"]
        for bookkeeping_field in ("operation_revision", "updated_at_ns"):
            durable_gripper.pop(bookkeeping_field)
            gripper_terminal.pop(bookkeeping_field)
        assert durable_gripper == gripper_terminal
        gripper_status = headless.execution_diagnostics()["status"]
        gripper_observed = _status_observation(
            gripper_status, presentation="gripper"
        )
        assert gripper_observed["position"] == {
            "kind": "linear_m",
            "value_m": pytest.approx(0.004),
        }
        _wait_retired(native, gripper_id)

        cancellable_intent = _direct_control_gripper_operation(
            headless.execution_diagnostics()["status"],
            request_id="cancellable-linear-gripper",
            position=0.008,
        )
        cancellable_intent["participants"][0]["payload"]["caller_options"][0][
            "value_s"
        ] = 1.0
        cancellable = composer.execute(cancellable_intent)
        cancellable_id = cancellable["accepted"]["operation_id"]
        cancellable_state = _wait_state(runner, cancellable_id, {"executing"})[
            "operation"
        ]
        gripper_stop = runner.stop_operation(
            request_id="stop-linear-gripper",
            operation_id=cancellable_id,
            expected_operation_revision=cancellable_state["operation_revision"],
            expected_stop_generation=0,
            reason="exercise addressed gripper Stop",
            correlation_id="n5-process-test",
        )
        cancelled = _wait_terminal(runner, cancellable_id)["operation"]
        assert cancelled["state"] == "stopped", json.dumps(cancelled, sort_keys=True)
        assert cancelled["active_stop_id"] == gripper_stop["accepted"]["stop_id"]
        _wait_retired(native, cancellable_id)
        # A certified nominal Stop followed by exact operation Reset is the
        # complete normal cycle. It must not fabricate an intervention latch
        # or require a second recovery command for the same physical Stop.
        gripper_recovery = _recover_active_stop_fault(
            headless,
            owner_epoch=identity["owner_epoch"],
            request_prefix="recover-linear-gripper-stop",
            correlation_id="n5-process-test",
            reason="operator verified post-Stop gripper truth",
        )
        assert gripper_recovery is None
        assert headless.stop_fault()["stop_fault"] is None

        controlled_intent = _direct_control_motion_operation(
            headless.execution_diagnostics()["status"],
            request_id="n4-controlled-operation",
            # Keep enough path after Pause/Resume for this test to exercise the
            # one-second replacement Stop trajectory rather than the separately
            # qualified short-path natural-completion policy.
            motion={
                "kind": "joint_goal",
                "joint_names": ["joint1"],
                "positions_rad": [1.5],
            },
        )
        controlled = composer.execute(controlled_intent)
        controlled_id = controlled["accepted"]["operation_id"]
        executing = _wait_state(runner, controlled_id, {"executing"})["operation"]
        projected_scale_control = composer.execution_control_state()["control_state"][
            "runtime_scale_control"
        ]
        scale_selector = projected_scale_control["selector"]
        assert scale_selector == {
            "program_binding_id": manifest["program_binding_id"],
            "component_role_id": "robot",
            "target_role_id": "arm",
        }
        initial_scale_control = composer.runtime_scale_control(scale_selector)[
            "runtime_scale_control"
        ]
        assert initial_scale_control["supported"] is True
        scaled = composer.set_runtime_scale_control(
            {
                "request_context": {
                    "request_id": "scale-controlled-scope",
                    "request_owner_epoch": identity["owner_epoch"],
                    "source_label": "composer_ui",
                    "correlation_id": "n4-public-control-test",
                },
                "selector": scale_selector,
                "expected_control_generation": initial_scale_control[
                    "control_generation"
                ],
                "scale": 0.5,
            }
        )
        assert scaled["accepted"]["control_generation"] == (
            initial_scale_control["control_generation"] + 1
        )
        assert scaled["accepted"]["active_operation_id"] == controlled_id
        scale_status = _wait_runtime_scale_control_applied(
            composer,
            scale_selector,
            scaled["accepted"]["control_generation"],
            0.5,
            active_operation_id=controlled_id,
        )
        assert scale_status["runtime_scale_control"]["requested_scale"] == 0.5
        assert scale_status["runtime_scale_control"]["effective_scale"] == 0.5
        assert (
            scale_status["runtime_scale_control"]["active_operation_scale_generation"]
            == 1
        )

        paused = composer.pause_operation(
            controlled_id,
            {
                "request_context": {
                    "request_id": "pause-controlled-operation",
                    "request_owner_epoch": identity["owner_epoch"],
                    "source_label": "composer_ui",
                    "correlation_id": "n4-public-control-test",
                },
                "operation_id": controlled_id,
                "expected_operation_revision": executing["operation_revision"],
                "reason": "exercise public Pause",
            },
        )
        paused_state = _wait_state(runner, controlled_id, {"paused"})["operation"]
        assert (
            paused_state["operation_revision"]
            == paused["accepted"]["operation_revision_after_acceptance"]
        )

        resumed = headless.resume_operation(
            controlled_id,
            {
                "request_context": {
                    "request_id": "resume-controlled-operation",
                    "request_owner_epoch": identity["owner_epoch"],
                    "source_label": "headless_process_test",
                    "correlation_id": "n4-public-control-test",
                },
                "operation_id": controlled_id,
                "expected_operation_revision": paused_state["operation_revision"],
                "reason": "exercise public Resume",
            },
        )
        resumed_state = _wait_state(runner, controlled_id, {"executing"})["operation"]
        assert (
            resumed_state["operation_revision"]
            == resumed["accepted"]["operation_revision_after_acceptance"]
        )
        before_controlled_stop = {
            "operation": resumed_state,
        }

        stop_arguments = {
            "request_id": "stop-controlled-operation",
            "operation_id": controlled_id,
            "expected_operation_revision": resumed_state["operation_revision"],
            "expected_stop_generation": 0,
            "reason": "exercise historical operator Stop",
            "correlation_id": "n4-public-control-test",
        }
        stop_requested_at = time.monotonic()
        stop_accepted = runner.stop_operation(**stop_arguments)
        stop_acceptance_elapsed_s = time.monotonic() - stop_requested_at
        stop_replay = runner.stop_operation(**stop_arguments)
        assert (
            stop_replay["accepted"]["stop_id"] == stop_accepted["accepted"]["stop_id"]
        )
        assert (
            stop_replay["accepted"]["stop_generation"]
            == stop_accepted["accepted"]["stop_generation"]
        )
        assert stop_replay["accepted"]["replay_status"] == "replayed_exact"
        stopped = _wait_terminal(runner, controlled_id)["operation"]
        stop_settlement_elapsed_s = time.monotonic() - stop_requested_at
        assert stopped["state"] == "stopped", json.dumps(stopped, sort_keys=True)
        assert stopped["active_stop_id"] == stop_accepted["accepted"]["stop_id"]
        assert stop_acceptance_elapsed_s < 0.5
        projected_stop = headless.execution_diagnostics()["status"]["control_state"][
            "cell_stop_action"
        ]
        assert 0.9 <= stop_settlement_elapsed_s < 1.75, json.dumps(
            {
                "before_stop": before_controlled_stop,
                "stop": projected_stop,
            },
            sort_keys=True,
        )
        assert projected_stop["expected_stop_generation"] == (
            stop_accepted["accepted"]["stop_generation"]
        )
        assert projected_stop["latest_terminal"]["certificate"]["stop_id"] == (
            stop_accepted["accepted"]["stop_id"]
        )
        terminal = projected_stop["latest_terminal"]
        assert terminal["result"] == "succeeded_nominal"
        assert terminal["escalated"] is False
        assert terminal["hard_stop_floor_used"] is False
        assert len(terminal["executed_stages"]) == 1
        controlled_stage = terminal["executed_stages"][0]
        assert controlled_stage["action_class"] == "controlled"
        assert controlled_stage["executed_mode"] == "soft_on_path"
        assert controlled_stage["stage_id"] == "nominal"
        assert controlled_stage["stage_index"] == 0
        assert controlled_stage["outcome"] == "succeeded"
        assert controlled_stage["substitution_reason"] is None
        durable_stop = None
        durable_terminal = None
        projection_deadline = time.monotonic() + 5.0
        while time.monotonic() < projection_deadline:
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
                durable_terminal = database.execute(
                    "SELECT sequence,payload_digest FROM journal_records "
                    "WHERE record_kind='stop_terminal_projected' AND stop_id=? "
                    "ORDER BY sequence DESC LIMIT 1",
                    (stop_accepted["accepted"]["stop_id"],),
                ).fetchone()
            if durable_stop is not None and durable_terminal is not None:
                break
            time.sleep(0.01)
        assert durable_stop is not None
        durable_stop_sequence = int(durable_stop[0])
        durable_stop_payload = json.loads(durable_stop[1])
        assert durable_stop_payload["intent"]["value"]["request_attribution"] == {
            "assurance": "externally_authenticated",
            "principal_id": "operator.process-test",
            "transport": "http",
        }
        durable_stop_page = native.events(
            identity["owner_epoch"], durable_stop_sequence - 1, limit=1
        )
        assert len(durable_stop_page["events"]) == 1
        durable_stop_event = durable_stop_page["events"][0]
        assert durable_stop_event["sequence"] == durable_stop_sequence
        assert durable_stop_event["record_kind"] == ("stop_acceptance_projected")
        assert durable_stop_event["operation_id"] is None
        assert durable_stop_event["operation_revision"] is None
        assert durable_stop_event["payload"] == durable_stop_payload
        assert durable_terminal is not None
        terminal_page = native.events(
            identity["owner_epoch"], int(durable_terminal[0]) - 1, limit=1
        )
        assert len(terminal_page["events"]) == 1
        terminal_event = terminal_page["events"][0]
        assert terminal_event["record_kind"] == "stop_terminal_projected"
        assert terminal_event["request_id"] is None
        assert terminal_event["operation_id"] is None
        assert terminal_event["operation_revision"] is None
        terminal_history = terminal_event["payload"]
        assert terminal_history["api_version"] == (
            "manyforge.stop_terminal_projection.v1"
        )
        assert terminal_history["durable_projection_digest"] == (durable_terminal[1])
        assert terminal_history["details_truncated"] is False
        assert terminal_history["request_id"] == "stop-controlled-operation"
        assert terminal_history["result"] == "succeeded_nominal"
        assert terminal_history["state"] == "succeeded"
        assert (
            900_000_000
            <= terminal_history["acceptance_to_terminal_latency_ns"]
            < 1_750_000_000
        )
        assert terminal_history["attempted_stage_count"] == 1
        terminal_attempt = terminal_history["attempted_stages"][0]
        assert terminal_attempt["stage"]["command_kind"] == ("motion_controlled_stop")
        assert terminal_attempt["profile"] is not None
        assert terminal_attempt["profile"]["profile_id"] == ("operator_soft_on_path")
        assert terminal_attempt["profile"]["stop_mode"] == "soft_on_path"
        assert terminal_attempt["actual_stop_mode"] == "soft_on_path"
        # The deployment owns the retained profile, while the exact prepared
        # participant command records the participant-specific mapping that
        # produced its executable parameters.
        assert terminal_attempt["profile_source"] == ("participant_profile_mapping")
        assert terminal_attempt["outcome"] == "succeeded"
        assert any(
            item["evidence"] is not None
            and item["evidence"].get("requested_stop_mode") == "soft_on_path"
            and item["evidence"].get("actual_stop_mode") == "soft_on_path"
            and item["evidence"].get("backend_stop_confirmed") is True
            for item in terminal_attempt["evidence_items"]
        )
        assert terminal_history["participant_settlement_count"] == 1
        assert (
            terminal_history["participant_settlement"][0]["facts"][
                "terminal_settlement"
            ]
            == "settled"
        )

        # The manual owner retires nominal Stop without clearing an invented fault.
        _wait_retired(native, controlled_id)
        persisted_scale_control = composer.runtime_scale_control(scale_selector)[
            "runtime_scale_control"
        ]
        assert (
            persisted_scale_control["control_generation"]
            == scaled["accepted"]["control_generation"]
        )
        assert persisted_scale_control["requested_scale"] == 0.5
        # Exact nominal motion containment settles directly to reset-ready
        # Stopped. It must not fabricate an intervention latch or require a
        # second recovery acknowledgement for the same physical Stop.
        controlled_recovery = _recover_active_stop_fault(
            headless,
            owner_epoch=identity["owner_epoch"],
            request_prefix="recover-controlled-stop",
            correlation_id="n4-public-control-test",
            reason="operator verified post-Stop motion truth",
        )
        assert controlled_recovery is None
        assert headless.stop_fault()["stop_fault"] is None
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
        assert not (execution_run_root / "execution-observation.sock").exists()
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
        assert (execution_run_root / "execution-observation.sock").is_socket()
        restarted = headless.execution_diagnostics()["status"]
        assert restarted["identity"]["owner_epoch"] != identity["owner_epoch"]
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


def test_target_queries_cannot_borrow_undeclared_capabilities(tmp_path: Path) -> None:
    from manyforge_program_runner.execution_transport import (
        ProgramRunnerExecutionTransport,
    )

    fixture_root = tmp_path / "fixture"
    shutil.copytree(_required_path("MANYFORGE_I6_NATIVE_FIXTURE_ROOT"), fixture_root)
    manifest = json.loads((fixture_root / "manifest.json").read_text("utf-8"))
    state_root, run_root = tmp_path / "state", tmp_path / "run"
    state_root.mkdir()
    run_root.mkdir()
    process = _start_host(fixture_root, state_root, run_root, manifest)
    client = ProgramRunnerExecutionTransport(run_root, timeout_seconds=2.0)
    try:
        identity = client.identity()["identity"]
        query = {
            "schema_version": "manyforge.target_observation_query.v1",
            "owner_epoch": identity["owner_epoch"],
            "program_binding_id": manifest["program_binding_id"],
            "target_role_id": manifest["target_role_id"],
            "kind": "frame_pose",
            "frame_role_id": "base",
        }
        before = client.events(identity["owner_epoch"], 0)
        # This fixture declares command/planning capabilities only. Merely
        # possessing its exact binding must not grant model/feedback access.
        for change, code in [
            ({}, "execution_target_observation_unavailable"),
            ({"owner_epoch": "old.owner"}, "execution_target_owner_stale"),
            (
                {"program_binding_id": "sha256:" + "0" * 64},
                "execution_target_observation_unavailable",
            ),
            (
                {"target_role_id": "unknown"},
                "execution_target_observation_unavailable",
            ),
        ]:
            with pytest.raises(HeadlessApiError) as caught:
                client.query_target({**query, **change})
            assert caught.value.code == code
        gripper_query = {
            key: value for key, value in query.items() if key != "frame_role_id"
        }
        gripper_query.update(
            kind="gripper_status", target_role_id=manifest["gripper_target_role_id"]
        )
        with pytest.raises(HeadlessApiError) as caught:
            client.query_target(gripper_query)
        assert caught.value.code == "execution_target_observation_unavailable"
        assert client.events(identity["owner_epoch"], 0) == before
    finally:
        _stop_host(process)


def test_frame_transform_query_uses_cell_resolver_and_verified_target_roles(
    tmp_path: Path,
) -> None:
    fixture_root = tmp_path / "fixture"
    shutil.copytree(_required_path("MANYFORGE_I6_NATIVE_FIXTURE_ROOT"), fixture_root)
    manifest = json.loads((fixture_root / "manifest.json").read_text("utf-8"))
    binding = json.loads((fixture_root / "program_binding.json").read_text("utf-8"))
    arm = next(
        row
        for row in binding["target_role_bindings"]
        if row["target_role_id"] == manifest["target_role_id"]
    )
    frame_roles = sorted(row["frame_role_id"] for row in arm["frame_role_bindings"])
    assert len(frame_roles) >= 2
    state_root, run_root = tmp_path / "state", tmp_path / "run"
    state_root.mkdir()
    run_root.mkdir()
    process = _start_host(fixture_root, state_root, run_root, manifest)
    client = ExecutionCommandClient(run_root, timeout_seconds=10.0)
    try:
        identity = client.identity()["identity"]
        robot = {
            "target_role_id": arm["target_role_id"],
            "controllable_component_id": arm["controllable_component_id"],
            "command_target_id": arm["command_target_id"],
        }
        probe = _pose(0.05, -0.02, 0.03)
        query = {
            "schema_version": "manyforge.target_observation_query.v1",
            "owner_epoch": identity["owner_epoch"],
            "program_binding_id": manifest["program_binding_id"],
            "kind": "frame_transform",
            "pose": probe,
            "source_frame": {"type": "cell_frame"},
            "target_frame": {"type": "cell_frame"},
        }

        def exchange(change: dict[str, Any]) -> dict[str, Any]:
            return client._exchange(
                {
                    "api_version": EXECUTION_COMMAND_VERSION,
                    "command": "query_target",
                    "query": {**query, **change},
                }
            )

        before = client.events(identity["owner_epoch"], 0)
        identity_transform = exchange({})
        assert identity_transform["ok"] is True, identity_transform
        # The typed client accepts the transform answer shape.
        typed = client.query_target(TargetObservationQuery.model_validate(query))
        assert typed["target_observation"]["value"] == (
            identity_transform["target_observation"]["value"]
        )
        observation = identity_transform["target_observation"]
        assert observation["schema_version"] == "manyforge.target_observation.v1"
        for key, value in query.items():
            if key != "schema_version":
                assert observation[key] == value
        assert observation["resolution_evidence_digests"] == []
        assert observation["scene_revision"]["scene_id"]
        assert _translation_error_m(
            observation["value"]["position_m"], probe["position_m"]
        ) < 1e-12

        source = {"type": "frame_role", "frame_role_id": frame_roles[0], "target": robot}
        target = {"type": "frame_role", "frame_role_id": frame_roles[1], "target": robot}
        forward = exchange({"source_frame": source, "target_frame": target})
        assert forward["ok"] is True, forward
        assert len(forward["target_observation"]["resolution_evidence_digests"]) == 2
        backward = exchange(
            {
                "pose": forward["target_observation"]["value"],
                "source_frame": target,
                "target_frame": source,
            }
        )
        assert backward["ok"] is True, backward
        assert _translation_error_m(
            backward["target_observation"]["value"]["position_m"],
            probe["position_m"],
        ) < 1e-9

        # Every refusal names the failed reference (spec 210 §5.3); the host
        # passes the owner's details through unchanged.
        for change, reason, named in [
            (
                {"source_frame": {**source, "target": {**robot, "command_target_id": "other"}}},
                "program_reference_target_role_mismatch",
                {
                    "reference_type": "frame_role",
                    "reference_id": frame_roles[0],
                    "target_role_id": robot["target_role_id"],
                },
            ),
            (
                {"source_frame": {**source, "target": {**robot, "target_role_id": "absent"}}},
                "program_reference_target_role_unbound",
                {
                    "reference_type": "frame_role",
                    "reference_id": frame_roles[0],
                    "target_role_id": "absent",
                },
            ),
            (
                {"source_frame": {"type": "scene_resource", "scene_resource_id": "absent"}},
                "program_scene_resource_missing",
                {"reference_type": "scene_resource", "reference_id": "absent"},
            ),
            (
                {"source_frame": {"type": "frame_role", "frame_role_id": frame_roles[0]}},
                "frame_transform_reference_unsupported",
                {"reference_type": "frame_role", "reference_id": frame_roles[0]},
            ),
        ]:
            rejected = exchange(change)
            assert rejected["ok"] is False, rejected
            assert rejected["error"]["details"] == {"reason": reason, **named}
        stale_owner = exchange({"owner_epoch": "old.owner"})
        assert stale_owner["error"]["code"] == "execution_target_owner_stale"
        assert client.events(identity["owner_epoch"], 0) == before
    finally:
        _stop_host(process)


def test_program_operation_query_rejects_wrong_or_missing_native_context(
    tmp_path: Path,
) -> None:
    from manyforge_program_runner.execution_transport import (
        ProgramRunnerExecutionTransport,
    )

    fixture_root = tmp_path / "fixture"
    shutil.copytree(_required_path("MANYFORGE_I6_NATIVE_FIXTURE_ROOT"), fixture_root)
    manifest = json.loads((fixture_root / "manifest.json").read_text("utf-8"))
    state_root, run_root = tmp_path / "state", tmp_path / "run"
    state_root.mkdir()
    run_root.mkdir()
    process = _start_host(fixture_root, state_root, run_root, manifest)
    client = ProgramRunnerExecutionTransport(run_root, timeout_seconds=2.0)
    try:
        identity = client.identity()["identity"]
        query = {
            "schema_version": "manyforge.program_operation_query.v1",
            "owner_epoch": identity["owner_epoch"],
            "program_binding_id": manifest["program_binding_id"],
            "program_run_id": "n3-program-run-1",
            "target_role_id": manifest["target_role_id"],
        }
        before = client.events(identity["owner_epoch"], 0)
        for change, code in [
            ({}, "execution_operation_context_unavailable"),
            ({"owner_epoch": "old"}, "stale_request_owner_epoch"),
            ({"target_role_id": "foreign"}, "execution_operation_target_unavailable"),
        ]:
            with pytest.raises(HeadlessApiError) as error:
                client.query_operation({**query, **change})
            assert error.value.code == code
        assert client.events(identity["owner_epoch"], 0) == before
        # Positive lookup, paused scale, Resume and unchanged persistent scale
        # are checked by ui6h_target_queries.py --controls through an actual
        # claimed/captured Program. The old synthetic submission here had no
        # authored origin and no longer represents the Runner's contract.
    finally:
        _stop_host(process)


def test_gripper_only_motion_updates_the_shared_native_observation_stream(
    tmp_path: Path,
) -> None:
    fixture_root = tmp_path / "fixture"
    shutil.copytree(_required_path("MANYFORGE_I6_NATIVE_FIXTURE_ROOT"), fixture_root)
    manifest = json.loads((fixture_root / "manifest.json").read_text("utf-8"))
    state_root, run_root = tmp_path / "state", tmp_path / "run"
    state_root.mkdir()
    run_root.mkdir()
    process = _start_host(fixture_root, state_root, run_root, manifest)
    client = ExecutionCommandClient(run_root, timeout_seconds=2.0)
    cancelled = threading.Event()
    timeout = threading.Timer(10.0, cancelled.set)
    try:
        status = client.diagnostics()["status"]
        _, gripper_target = _status_target(status, presentation="gripper")
        direct = gripper_target["command"]
        resource = direct["resource"]
        observed_targets = client.control_state()["control_state"][
            "observed_targets"
        ]
        observed_target_keys = sorted(
            (
                row["controllable_component_id"],
                row["command_target_id"],
                row["operation_capability_id"],
            )
            for row in observed_targets
        )
        timeout.start()
        # No Composer/browser owns these commands or performs terminal release.
        # Repeat both ends of travel to prove the next operation stays usable.
        for attempt, goal in enumerate(
            [resource["max_position"], resource["min_position"]] * 2
        ):
            with client.open_observation_session() as observation_session:
                first = observation_session.next(None, cancelled)["observation"]
                assert first["schema_version"] == (
                    "manyforge.execution_observation.v3"
                )
                assert sorted(
                    (
                        row["controllable_component_id"],
                        row["command_target_id"],
                        row["operation_capability_id"],
                    )
                    for row in first["targets"]
                ) == observed_target_keys
                motion, gripper = first["targets"]
                assert motion["coordinate_kinds"] == ["angular"] * 6
                assert gripper["coordinate_kinds"] == ["linear"]
                assert gripper["source_joint_names"] == [
                    resource["source_coordinate_name"]
                ]
                operation = _direct_control_gripper_operation(
                    status,
                    request_id=f"request.gripper.observation.{attempt}",
                    position=goal,
                )
                if attempt == 0:
                    operation["participants"][0]["payload"]["caller_options"][0][
                        "value_s"
                    ] = 1.0
                request = OperationIntentRequest.model_validate(operation)
                preview = client.preview_capability_command(
                    request, trusted_principal_id=None
                )
                assert preview["ok"] is True, preview
                accepted = client.submit(request, trusted_principal_id=None)
                assert accepted["ok"] is True, accepted
                if attempt == 0:
                    assert (
                        client.read(accepted["accepted"]["operation_id"])["resident"]
                        is True
                    )
                    arm_preview = OperationIntentRequest.model_validate(
                        _direct_control_joint_operation(
                            status["identity"],
                            _status_target(status, presentation="motion")[1],
                            request_id="preview.independent.arm",
                            positions_rad=list(motion["positions_si"]),
                        )
                    )
                    # Planning for the idle arm must not be blocked by the
                    # independent gripper command. This never approves motion.
                    planned = client.preview_motion(
                        arm_preview, trusted_principal_id=None
                    )
                    assert planned["ok"] is True, planned
                latest = first
                while latest["targets"][1]["positions_si"] != pytest.approx([goal]):
                    cursor = {
                        key: latest[key]
                        for key in ("owner_epoch", "source_epoch", "source_sequence")
                    }
                    response = observation_session.next(cursor, cancelled)
                    assert response["ok"] is True, response
                    current = response["observation"]
                    assert current["source_epoch"] == first["source_epoch"]
                    assert current["source_sequence"] > latest["source_sequence"]
                    assert (
                        current["targets"][0]["positions_si"] == motion["positions_si"]
                    )
                    assert (
                        current["targets"][1]["source_sequence"]
                        >= latest["targets"][1]["source_sequence"]
                    )
                    latest = current
                if gripper["positions_si"] != pytest.approx([goal]):
                    assert latest["source_sequence"] > first["source_sequence"]
                assert len(json.dumps(latest).encode("utf-8")) <= 4096
            operation_id = accepted["accepted"]["operation_id"]
            deadline = time.monotonic() + 2.0
            while time.monotonic() < deadline:
                terminal = client.read(operation_id)
                if terminal.get("resident") is False:
                    break
                cancelled.wait(0.005)
            assert terminal["operation"]["state"] == "completed", terminal
            assert terminal["resident"] is False, terminal
            control = client.control_state()["control_state"]
            assert control["schema_version"] == "manyforge.execution_control_state.v6"
            assert sorted(
                (
                    row["controllable_component_id"],
                    row["command_target_id"],
                    row["operation_capability_id"],
                )
                for row in control["observed_targets"]
            ) == observed_target_keys
            results = control["manual_operations"]
            assert len(results) == 1
            assert results[0]["resident"] is False
            assert results[0]["operation"]["operation_id"] == operation_id
            assert results[0]["operation"]["state"] == "completed"
    finally:
        cancelled.set()
        timeout.cancel()
        _stop_host(process)


def test_managed_gripper_only_cell_has_no_motion_target_and_refuses_disabled_command(
    tmp_path: Path,
) -> None:
    fixture_root = tmp_path / "fixture"
    shutil.copytree(
        _required_path("MANYFORGE_I6_NATIVE_GRIPPER_ONLY_FIXTURE_ROOT"),
        fixture_root,
    )
    manifest = json.loads((fixture_root / "manifest.json").read_text("utf-8"))
    assert manifest["gripper_only"] is True
    state_root, run_root = tmp_path / "state", tmp_path / "run"
    state_root.mkdir()
    run_root.mkdir()
    process = _start_host(fixture_root, state_root, run_root, manifest)
    client = ExecutionCommandClient(run_root, timeout_seconds=2.0)
    try:
        status = client.diagnostics()["status"]
        assert status["control_state"]["runtime_scale_control"] is None
        assert [
            target["presentation"]
            for component in status["targets"]["components"]
            for target in component["targets"]
        ] == ["gripper"]
        _, target = _status_target(status, presentation="gripper")
        goal = target["command"]["resource"]["max_position"]
        accepted = client.submit(
            OperationIntentRequest.model_validate(
                _direct_control_gripper_operation(
                    status, request_id="gripper-only.command", position=goal
                )
            ),
            trusted_principal_id=None,
        )
        assert accepted["ok"] is True, accepted
        operation_id = accepted["accepted"]["operation_id"]
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            terminal = client.read(operation_id)
            if terminal["resident"] is False:
                break
            time.sleep(0.005)
        assert terminal["operation"]["state"] == "completed", terminal

        # A motion-less cell still has a real Stop path for its gripper.
        stop_intent = _direct_control_gripper_operation(
            status,
            request_id="gripper-only.stop-command",
            position=target["command"]["resource"]["min_position"],
        )
        stop_payload = stop_intent["participants"][0]["payload"]
        stop_payload["completion_timeout_s"] = 3.0
        stop_payload["caller_options"][0]["value_s"] = 2.0
        started = client.submit(
            OperationIntentRequest.model_validate(stop_intent),
            trusted_principal_id=None,
        )
        assert started["ok"] is True, started
        stopped_operation_id = started["accepted"]["operation_id"]
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            in_flight = client.read(stopped_operation_id)
            if in_flight["operation"]["state"] == "executing":
                break
            time.sleep(0.005)
        assert in_flight["operation"]["state"] == "executing", in_flight
        stopped = client.stop(
            StopIntentRequest.model_validate(
                {
                    "request_context": {
                        "request_id": "gripper-only.stop",
                        "request_owner_epoch": status["identity"]["owner_epoch"],
                        "source_label": "native-gripper-only-test",
                    },
                    "operations": [
                        {
                            "operation_id": stopped_operation_id,
                            "expected_operation_revision": in_flight["operation"][
                                "operation_revision"
                            ],
                            "expected_stop_generation": 0,
                        }
                    ],
                }
            ),
            trusted_principal_id=None,
        )
        assert stopped["ok"] is True, stopped
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            stopped_status = client.read(stopped_operation_id)
            if (
                stopped_status["operation"]["state"] == "stopped"
                and stopped_status["resident"] is False
            ):
                break
            time.sleep(0.005)
        assert stopped_status["operation"]["state"] == "stopped", stopped_status
        assert stopped_status["resident"] is False, stopped_status

        disabled = _submit_native_cclc_lifecycle(
            run_root, intent_kind="disable", request_id="gripper-only.disable"
        )
        assert disabled["current_state"] == "disabled", disabled
        after = client.diagnostics()["status"]
        assert [
            target["presentation"]
            for component in after["targets"]["components"]
            for target in component["targets"]
        ] == ["gripper"]
        operations_before_refusal = after["control_state"]["manual_operations"]
        refused = client.submit(
            OperationIntentRequest.model_validate(
                _direct_control_gripper_operation(
                    after,
                    request_id="gripper-only.disabled-command",
                    position=target["command"]["resource"]["min_position"],
                )
            ),
            trusted_principal_id=None,
        )
        assert refused["ok"] is False, refused
        assert refused["error"]["details"]["reason"] == (
            "component_command_not_eligible"
        ), refused
        assert client.diagnostics()["status"]["control_state"][
            "manual_operations"
        ] == operations_before_refusal
        _submit_native_cclc_lifecycle(
            run_root, intent_kind="prepare", request_id="gripper-only.prepare-again"
        )
        _submit_native_cclc_lifecycle(
            run_root, intent_kind="enable", request_id="gripper-only.enable-again"
        )
        enabled = client.diagnostics()["status"]
        retried = client.submit(
            OperationIntentRequest.model_validate(
                _direct_control_gripper_operation(
                    enabled,
                    request_id="gripper-only.command-after-enable",
                    position=target["command"]["resource"]["max_position"],
                )
            ),
            trusted_principal_id=None,
        )
        assert retried["ok"] is True, retried
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            retried_status = client.read(retried["accepted"]["operation_id"])
            if retried_status["operation"]["state"] == "completed":
                break
            time.sleep(0.005)
        assert retried_status["operation"]["state"] == "completed", retried_status
    finally:
        _stop_host(process)


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
    process = _start_host(
        fixture_root, execution_state_root, execution_run_root, manifest
    )
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

        status = client.diagnostics()["status"]
        motion_component, direct = _status_target(status, presentation="motion")
        observed = _status_observation(status, presentation="motion")
        operation = _direct_control_joint_operation(
            status["identity"],
            direct,
            request_id="handover.query",
            positions_rad=list(observed["positions_rad"]),
        )
        proof = client.read_manual_handover(
            OperationIntentRequest.model_validate(operation),
            trusted_principal_id=None,
        )
        assert proof["ok"] is True and proof["handover"] is None
        assert proof["availability"] == {
            "available": True,
            "cleanup_required": False,
            "unavailable_reason": None,
        }
        # A supplied context is only a precondition. Reading cannot acquire a
        # claim or invent a held parent, including the valid initial revision 0.
        operation["manual_handover"] = {
            "program_runner_owner_epoch": "absent.runner",
            "program_run_id": "absent.parent",
            "claim_generation": 1,
            "selection_revision": 0,
        }
        proof = client.read_manual_handover(
            OperationIntentRequest.model_validate(operation),
            trusted_principal_id=None,
        )
        assert proof["ok"] is True and proof["handover"] is None
        for field, value in (
            ("selection_revision", True),
            ("selection_revision", -1),
            ("selection_revision", float(2**53)),
            ("extra", 0),
        ):
            malformed_operation = copy.deepcopy(operation)
            malformed_operation["manual_handover"][field] = value
            invalid = client._exchange(
                {
                    "api_version": EXECUTION_COMMAND_VERSION,
                    "command": "read_manual_handover",
                    "operation": malformed_operation,
                    "attribution": {
                        "transport": "http",
                        "assurance": "unverified",
                        "principal_id": None,
                    },
                }
            )
            assert invalid["ok"] is False
        assert client._exchange(get) == first

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
                "message": (
                    "the independent Stop control socket accepts only Stop, "
                    "Stop status and operator-alarm attention"
                ),
            },
            "ok": False,
        }
        observation_only = client._exchange_on(
            get, execution_run_root / "execution-observation.sock"
        )
        assert observation_only == {
            "api_version": "manyforge.execution_command_response.v1",
            "command": "cell_admission_get",
            "error": {
                "code": "execution_observation_command_unsupported",
                "details": {},
                "message": (
                    "the independent observation socket accepts only observation"
                ),
            },
            "ok": False,
        }
        wrong_observation_command = client._exchange_on(
            {
                "api_version": "manyforge.execution_command.v1",
                "command": "identity",
            },
            execution_run_root / "execution-observation.sock",
        )
        assert wrong_observation_command["ok"] is False
        assert wrong_observation_command["error"]["code"] == (
            "execution_observation_command_unsupported"
        )
        observation = client.observation()
        assert observation.get("ok") is True, observation
        assert observation["command"] == "observation"

        # UI6 uses one persistent private session. A second credited request
        # must cross the same accepted socket instead of reconnecting or
        # falling back to the legacy one-request observation exchange.
        motion_target_key = (
            motion_component["controllable_component_id"],
            direct["command_target_id"],
            direct["operation_capability_id"],
        )

        def observed_motion_state() -> dict[str, Any]:
            control = client.control_state()["control_state"]
            assert control["schema_version"] == (
                "manyforge.execution_control_state.v6"
            )
            matching = [
                row["observed"]
                for row in control["observed_targets"]
                if (
                    row["controllable_component_id"],
                    row["command_target_id"],
                    row["operation_capability_id"],
                )
                == motion_target_key
            ]
            assert len(matching) == 1
            assert matching[0]["kind"] == "joint_positions"
            return matching[0]

        before_motion = observed_motion_state()
        with client.open_observation_session() as observation_session:
            first_observation = observation_session.next(
                None,
                threading.Event(),
            )
            first_frame = first_observation["observation"]
            repeated_current = observation_session.next(
                {
                    "owner_epoch": "retired-owner-epoch",
                    "source_epoch": first_frame["source_epoch"],
                    "source_sequence": first_frame["source_sequence"],
                },
                threading.Event(),
            )
        repeated_frame = repeated_current["observation"]
        after_motion = observed_motion_state()
        # Idle render identity is unchanged, while actual source sequences are
        # bracketed by exact owner observations, not ignored in this assertion.
        assert repeated_frame["source_sequence"] == first_frame["source_sequence"]
        for frame in (first_frame, repeated_frame):
            motion = frame["targets"][0]
            assert (
                motion["source_epoch"]
                == before_motion["source_epoch"]
                == after_motion["source_epoch"]
            )
            assert (
                before_motion["sequence"]
                <= motion["source_sequence"]
                <= after_motion["sequence"]
            )
            assert (
                motion["positions_si"]
                == before_motion["positions_rad"]
                == after_motion["positions_rad"]
            )
        assert {
            key: value for key, value in repeated_frame.items() if key != "targets"
        } == {key: value for key, value in first_frame.items() if key != "targets"}
        assert process.poll() is None
    finally:
        _stop_host(process)


def test_native_runtime_scale_selector_codec_preserves_singular_scope(
    tmp_path: Path,
) -> None:
    """Exercise the native decoder with the runtime-scale selector variants."""

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
    client = ExecutionCommandClient(execution_run_root, timeout_seconds=5.0)
    try:
        api_version = EXECUTION_COMMAND_VERSION
        binding = manifest["program_binding_id"]
        singular_selector = {
            "program_binding_id": binding,
            "component_role_id": "robot",
            "target_role_id": manifest["target_role_id"],
        }

        singular_before = client._exchange(
            {
                "api_version": api_version,
                "command": "get_runtime_scale_control",
                "selector": singular_selector,
            }
        )
        assert singular_before["ok"] is True, singular_before
        assert singular_before["runtime_scale_control"]["selector"] == (
            singular_selector
        )

        synchronized = client._exchange(
            {
                "api_version": api_version,
                "command": "get_runtime_scale_control",
                "selector": {
                    "program_binding_id": binding,
                    "group_targets": [
                        {
                            "component_role_id": "robot",
                            "target_role_id": manifest["target_role_id"],
                        },
                        {
                            "component_role_id": "robot",
                            "target_role_id": manifest["gripper_target_role_id"],
                        },
                    ],
                },
            }
        )
        if synchronized["ok"] is False:
            assert synchronized["error"]["code"] in {
                "runtime_scale_control_unsupported",
                "runtime_scale_control_target_missing",
            }, synchronized
        else:
            assert synchronized["command"] == "get_runtime_scale_control"

        malformed_selectors = [
            {
                **singular_selector,
                "group_targets": [
                    {
                        "component_role_id": "robot",
                        "target_role_id": manifest["target_role_id"],
                    },
                    {
                        "component_role_id": "robot",
                        "target_role_id": manifest["gripper_target_role_id"],
                    },
                ],
            },
            {
                "program_binding_id": binding,
                "group_targets": [
                    {
                        "component_role_id": "robot",
                        "target_role_id": manifest["target_role_id"],
                    },
                    {
                        "component_role_id": "robot",
                        "target_role_id": manifest["target_role_id"],
                    },
                ],
            },
        ]
        for selector in malformed_selectors:
            rejected = client._exchange(
                {
                    "api_version": api_version,
                    "command": "get_runtime_scale_control",
                    "selector": selector,
                }
            )
            assert rejected["ok"] is False
            assert rejected["error"]["code"] == "execution_command_invalid"

        singular_after = client._exchange(
            {
                "api_version": api_version,
                "command": "get_runtime_scale_control",
                "selector": singular_selector,
            }
        )
        assert singular_after == singular_before
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
    admission = CellAdmissionClient(execution_run_root, timeout_seconds=5.0)

    def run_move(**kwargs: Any) -> float:
        return _run_native_benchmark_move(native, admission, **kwargs)

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
        initial_observation = native.observation()
        assert initial_observation.get("ok") is True, initial_observation
        origin = [
            float(value)
            for value in initial_observation["observation"]["targets"][0][
                "positions_si"
            ]
        ]

        run_move(
            request_id="benchmark-warmup",
            positions_rad=goal_a,
            cache_mode="default",
        )
        run_move(
            request_id="benchmark-warmup-return",
            positions_rad=origin,
            cache_mode="disable",
        )

        single_plan_seconds: list[float] = []
        for index in range(3):
            single_plan_seconds.append(
                run_move(
                    request_id=f"benchmark-single-{index}",
                    positions_rad=goal_a,
                    cache_mode="disable",
                )
            )
            run_move(
                request_id=f"benchmark-single-return-{index}",
                positions_rad=origin,
                cache_mode="disable",
            )

        shared_cache_move_id = "benchmark-cache-shared-motion"
        uncached_seconds = run_move(
            request_id="benchmark-cache-prime",
            positions_rad=goal_b,
            cache_mode="default",
            move_id=shared_cache_move_id,
        )
        run_move(
            request_id="benchmark-cache-return",
            positions_rad=origin,
            cache_mode="disable",
        )
        cached_seconds = run_move(
            request_id="benchmark-cache-reuse",
            positions_rad=goal_b,
            cache_mode="default",
            move_id=shared_cache_move_id,
        )

        five_started_at = time.perf_counter()
        five_move_seconds = [
            run_move(
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
            "historical_source_commit": ("08222188293df8e12f90dc72e204c21f22f92ea1"),
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
        status = headless.execution_diagnostics()["status"]
        positions = [
            float(position)
            for position in _status_observation(
                status, presentation="motion"
            )["positions_rad"]
        ]
        positions[0] = 0.03
        intent = _direct_control_joint_operation(
            status["identity"],
            _status_target(status, presentation="motion")[1],
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

        stop_request = {
            "request_context": {
                "request_id": "stop-while-preview-blocked",
                "request_owner_epoch": status["identity"]["owner_epoch"],
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
        stopped = headless.stop_operations(stop_request)
        assert stopped["accepted"]["resolution_disposition"] == ("promoted_to_full")
        assert stopped["accepted"]["resolution_reason"] == "unknown_operation"

        # Stop preempted the blocked preview instead of waiting for it.
        preview_thread.join(timeout=3.0)
        assert preview_done.is_set()
        assert not preview_error
        assert preview_response[0]["ok"] is False
        replay = headless.stop_operations(stop_request)["accepted"]
        assert replay["replay_status"] == "replayed_exact", replay
        for field in ("stop_id", "stop_generation"):
            assert replay[field] == stopped["accepted"][field]
        # The Stop-control worker projects the acceptance after the
        # acknowledgement; the cancelled preview no longer delays this check.
        deadline = time.monotonic() + 5.0
        records: list[Any] = []
        while not records and time.monotonic() < deadline:
            with sqlite3.connect(
                f"file:{execution_state_root / 'operation-journal.sqlite3'}?mode=ro",
                uri=True,
            ) as journal:
                records = journal.execute(
                    "SELECT sequence,payload_json FROM journal_records "
                    "WHERE record_kind='stop_acceptance_projected' "
                    "AND request_id=?",
                    (stop_request["request_context"]["request_id"],),
                ).fetchall()
            if not records:
                time.sleep(0.01)
        assert len(records) == 1, records
        sequence, payload_json = records[0]
        payload = json.loads(payload_json)
        assert payload["intent"]["value"]["request_attribution"] == {
            "assurance": "externally_authenticated",
            "principal_id": "operator.process-test",
            "transport": "http",
        }
        events = execution.events(
            status["identity"]["owner_epoch"], sequence - 1, limit=1
        )["events"]
        assert len(events) == 1
        assert events[0]["sequence"] == sequence
        assert events[0]["record_kind"] == "stop_acceptance_projected"
        assert events[0]["operation_id"] is None
        assert events[0]["operation_revision"] is None
        assert events[0]["payload"] == payload
    finally:
        _stop_host(process)
        if preview_thread is not None:
            preview_thread.join(timeout=3.0)
        raw_api.close()


def _preview_packet(intent: Mapping[str, Any]) -> bytes:
    return rfc8785.dumps(
        {
            "api_version": EXECUTION_COMMAND_VERSION,
            "attribution": {
                "assurance": "unverified",
                "principal_id": None,
                "transport": "http",
            },
            "command": "preview_motion",
            "operation": OperationIntentRequest.model_validate(intent).model_dump(
                mode="json"
            ),
        }
    )


def test_blocked_preview_leaves_control_traffic_served_and_program_stop_preempts_it(
    tmp_path: Path,
) -> None:
    """A planning preview never occupies the lane of control-relevant traffic.

    The fake candidate worker blocks the first planning request until it is
    cancelled, so every assertion below is causal rather than timed: before
    the submission-ordered lane existed, each ordinary request here waited for
    the blocked preview and failed at its transport deadline.
    """

    fixture_root = tmp_path / "fixture"
    shutil.copytree(_required_path("MANYFORGE_I6_NATIVE_FIXTURE_ROOT"), fixture_root)
    manifest = json.loads((fixture_root / "manifest.json").read_text("utf-8"))
    state_root, run_root = tmp_path / "state", tmp_path / "run"
    state_root.mkdir()
    run_root.mkdir()
    marker = tmp_path / "blocked-preview-worker"
    process = _start_host(
        fixture_root,
        state_root,
        run_root,
        manifest,
        candidate_worker=_required_path("MANYFORGE_ROBOPLAN_FAKE_WORKER"),
        fake_worker_marker=marker,
    )
    # Ordinary requests get a short transport deadline; the blocked preview
    # itself must outlive every step before its preemption.
    execution = ExecutionCommandClient(run_root, timeout_seconds=2.0)
    planning = ExecutionCommandClient(run_root, timeout_seconds=30.0)
    internal = ExecutionCommandClient(
        run_root, timeout_seconds=2.0, attribution_transport="internal"
    )
    admission = CellAdmissionClient(run_root, timeout_seconds=2.0)
    pinned = PinnedRunRoot(run_root)
    preview_done = threading.Event()
    preview_response: list[dict[str, Any]] = []
    preview_error: list[Exception] = []
    preview_thread: threading.Thread | None = None
    queued: list[socket.socket] = []
    try:
        status = execution.diagnostics()["status"]
        identity = status["identity"]
        positions = [
            float(position)
            for position in _status_observation(status, presentation="motion")[
                "positions_rad"
            ]
        ]
        positions[0] = 0.03
        target = _status_target(status, presentation="motion")[1]

        def intent(request_id: str) -> dict[str, Any]:
            return _direct_control_joint_operation(
                identity, target, request_id=request_id, positions_rad=positions
            )

        def run_preview() -> None:
            try:
                preview_response.append(
                    planning.preview_motion(
                        OperationIntentRequest.model_validate(
                            intent("blocked-lane-preview")
                        ),
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

        # Stop status, events, reads, cell admission and operator-alarm
        # attention are all served while the preview is still planning.
        assert execution.identity()["ok"] is True
        assert "events" in execution.events(identity["owner_epoch"], 0, limit=1)
        assert internal.stop_status("no-such-stop")["ok"] is False
        assert "authoring_allowed" in admission.get()
        attention = {
            "active_configuration": None,
            "cell_id": manifest["cell_id"],
            "generation": 1,
            "members": [],
            "owner_epoch": "alarm.owner.lane-test",
            "owner_host_boot_id": Path("/proc/sys/kernel/random/boot_id")
            .read_text("ascii")
            .strip(),
            "owner_incarnation_us": 1,
            "reconciled": True,
            "schema_version": "manyforge.operator_alarm_motion_attention.v3",
        }
        assert admission.publish_operator_alarm_attention(
            attention, deadline=time.monotonic() + 2.0
        ) == {"generation": 1, "owner_epoch": "alarm.owner.lane-test"}
        # Those Stop-lane requests have no second path on the ordinary socket.
        for moved in (
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "attribution": {
                    "assurance": "unverified",
                    "principal_id": None,
                    "transport": "internal",
                },
                "command": "read_stop",
                "stop_id": "no-such-stop",
            },
            {"api_version": EXECUTION_COMMAND_VERSION, "command": "get_stop_fault"},
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "cell_admission_publish_operator_alarm_attention",
                "request": attention,
            },
        ):
            refused_ordinary = execution._exchange(moved)
            assert refused_ordinary["ok"] is False, refused_ordinary
            assert refused_ordinary["error"]["code"] == (
                "execution_ordinary_command_unsupported"
            ), refused_ordinary
        assert not preview_done.is_set()

        # Submission-ordered requests queue FIFO behind the blocked preview up
        # to the host bound. The ordinary lane accepts connections in order and
        # enqueues each before accepting the next, so the request beyond the
        # bound is refused deterministically instead of occupying the lane.
        for index in range(7):
            client = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            client.settimeout(5.0)
            with pinned.socket_address("execution-command.sock") as address:
                client.connect(address)
            client.send(_preview_packet(intent(f"queued-lane-preview-{index}")))
            queued.append(client)
        overflow = execution.preview_motion(
            OperationIntentRequest.model_validate(intent("overflow-lane-preview")),
            trusted_principal_id=None,
        )
        assert overflow["ok"] is False
        assert overflow["error"]["code"] == "execution_submission_lane_busy"
        assert not preview_done.is_set()

        def reset(operation_id: str, revision: int, request_id: str) -> dict[str, Any]:
            return execution.reset(
                OperationControlRequest.model_validate(
                    {
                        "request_context": {
                            "request_id": request_id,
                            "request_owner_epoch": identity["owner_epoch"],
                            "source_label": "native-lane-test",
                        },
                        "operation_id": operation_id,
                        "expected_operation_revision": revision,
                    }
                ),
                trusted_principal_id=None,
            )

        # A refused Reset is served at once and changes nothing a preview
        # depends on, so the preview keeps planning.
        assert reset("non-resident-operation", 1, "lane-test-refused")["ok"] is False
        assert not preview_done.is_set()

        # Program Stop preempts planning before it is dispatched and never
        # waits for it.
        preempting = internal.stop_program_run(
            ProgramRunStopIntentRequest.model_validate(
                {
                    "request_context": {
                        "request_id": "lane-test-program-stop",
                        "request_owner_epoch": identity["owner_epoch"],
                        "source_label": "manyforge_behavior",
                        "correlation_id": "program-run.lane-test",
                    },
                    "program_run_id": "program-run.lane-test",
                    "reason": "preempt the blocked preview",
                }
            )
        )
        assert preempting["ok"] is True, preempting

        # The preempting command did not wait for planning; the preview and
        # every queued one now end cancelled.
        preview_thread.join(timeout=5.0)
        assert preview_done.is_set()
        assert not preview_error, preview_error
        assert preview_response[0]["ok"] is False
        assert preview_response[0]["error"]["details"]["reason"] == (
            "motion_preview_cancelled"
        ), preview_response[0]
        # A queued preview reaches the Kernel already cancelled; after a
        # Program Stop the Kernel may refuse it earlier at its closed motion
        # admission. Neither plans.
        not_planned = {"motion_preview_cancelled", "motion_admission_closed"}
        for client in queued:
            payload = client.recv(4 * 1024 * 1024)
            response = json.loads(payload)
            assert response["ok"] is False, response
            assert response["error"]["details"]["reason"] in not_planned, response
    finally:
        for client in queued:
            client.close()
        _stop_host(process)
        if preview_thread is not None:
            preview_thread.join(timeout=3.0)


def test_component_lifecycle_claim_preempts_blocked_preview(tmp_path: Path) -> None:
    """A component-lifecycle claim never waits behind a planning preview.

    The Kernel serializes the claim with the submission lock the preview
    retains while it plans. The fake candidate worker blocks planning until it
    is cancelled, so the claim completes only because it preempted the
    preview, like Stop: the in-flight and queued previews end cancelled.
    """

    fixture_root = tmp_path / "fixture"
    shutil.copytree(_required_path("MANYFORGE_I6_NATIVE_FIXTURE_ROOT"), fixture_root)
    manifest = json.loads((fixture_root / "manifest.json").read_text("utf-8"))
    state_root, run_root = tmp_path / "state", tmp_path / "run"
    state_root.mkdir()
    run_root.mkdir()
    marker = tmp_path / "blocked-preview-worker"
    process = _start_host(
        fixture_root,
        state_root,
        run_root,
        manifest,
        candidate_worker=_required_path("MANYFORGE_ROBOPLAN_FAKE_WORKER"),
        fake_worker_marker=marker,
    )
    execution = ExecutionCommandClient(run_root, timeout_seconds=2.0)
    planning = ExecutionCommandClient(run_root, timeout_seconds=30.0)
    admission = CellAdmissionClient(run_root, timeout_seconds=2.0)
    pinned = PinnedRunRoot(run_root)
    preview_done = threading.Event()
    preview_response: list[dict[str, Any]] = []
    preview_error: list[Exception] = []
    preview_thread: threading.Thread | None = None
    queued: socket.socket | None = None
    try:
        status = execution.diagnostics()["status"]
        identity = status["identity"]
        positions = [
            float(position)
            for position in _status_observation(status, presentation="motion")[
                "positions_rad"
            ]
        ]
        positions[0] = 0.03
        target = _status_target(status, presentation="motion")[1]

        def intent(request_id: str) -> dict[str, Any]:
            return _direct_control_joint_operation(
                identity, target, request_id=request_id, positions_rad=positions
            )

        def run_preview() -> None:
            try:
                preview_response.append(
                    planning.preview_motion(
                        OperationIntentRequest.model_validate(
                            intent("lifecycle-blocked-preview")
                        ),
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
        queued = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        queued.settimeout(5.0)
        with pinned.socket_address("execution-command.sock") as address:
            queued.connect(address)
        queued.send(_preview_packet(intent("lifecycle-queued-preview")))
        # Other cell-admission commands do not preempt planning.
        current = admission.get()
        lifecycle_owner_epoch = CCLCOpenRequestClient(
            run_root, timeout_seconds=2.0
        ).owner_identity()["request_owner_epoch"]
        assert not preview_done.is_set()

        lifecycle_intent = {
            "claim_kind": "component_lifecycle",
            "owner_role": "component_lifecycle_owner",
            "owner_epoch": lifecycle_owner_epoch,
            "owner_transaction_id": "lifecycle-preempt.transaction",
            "request_id": "claim.lifecycle-preempt",
            "request_payload_digest": "sha256:" + "b" * 64,
            "expected_admission_generation": current["admission_generation"],
            "scope": {"resources": [], "configurations": []},
            "program_cycle_evidence": None,
            "intervention_evidence": None,
        }
        # Accepted, not refused at a transport deadline: before preemption
        # the claim waited for the blocked preview's submission lock.
        claim = admission.acquire(lifecycle_intent)
        assert claim["intent"] == lifecycle_intent

        preview_thread.join(timeout=5.0)
        assert preview_done.is_set()
        assert not preview_error, preview_error
        assert preview_response[0]["ok"] is False
        assert preview_response[0]["error"]["details"]["reason"] == (
            "motion_preview_cancelled"
        ), preview_response[0]
        response = json.loads(queued.recv(4 * 1024 * 1024))
        assert response["ok"] is False, response
        assert response["error"]["details"]["reason"] == (
            "motion_preview_cancelled"
        ), response
    finally:
        if queued is not None:
            queued.close()
        _stop_host(process)
        if preview_thread is not None:
            preview_thread.join(timeout=3.0)


@pytest.mark.parametrize("active_target", ["arm", "gripper"])
def test_native_manual_arm_and_gripper_are_exclusive_and_recover_independently(
    tmp_path: Path,
    active_target: str,
) -> None:
    fixture_root = tmp_path / "fixture"
    shutil.copytree(_required_path("MANYFORGE_I6_NATIVE_FIXTURE_ROOT"), fixture_root)
    manifest = json.loads((fixture_root / "manifest.json").read_text("utf-8"))
    state_root, run_root = tmp_path / "state", tmp_path / "run"
    state_root.mkdir()
    run_root.mkdir()
    process = _start_host(fixture_root, state_root, run_root, manifest)
    client = ExecutionCommandClient(run_root, timeout_seconds=2.0)
    try:
        status = client.diagnostics()["status"]
        positions = list(
            _status_observation(status, presentation="motion")["positions_rad"]
        )
        positions[0] += 0.4
        intents = {
            "arm": _direct_control_joint_operation(
                status["identity"],
                _status_target(status, presentation="motion")[1],
                request_id="independent-arm",
                positions_rad=positions,
            ),
            "gripper": _direct_control_gripper_operation(
                status,
                request_id="independent-gripper",
                position=_status_target(status, presentation="gripper")[1][
                    "command"
                ]["resource"]["max_position"],
            ),
        }
        intents["arm"]["participants"][0]["payload"]["speed_profile"][
            "velocity_scale"
        ] = 0.5
        tool_payload = intents["gripper"]["participants"][0]["payload"]
        tool_payload["completion_timeout_s"] = 3.0
        tool_payload["caller_options"][0]["value_s"] = 2.0

        def wait_state(operation_id, states, timeout=6.0):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                row = client.read(operation_id)
                if row["operation"]["state"] in states:
                    return row
                time.sleep(0.005)
            pytest.fail(f"native operation did not reach {states}: {row}")

        other = "gripper" if active_target == "arm" else "arm"
        response = client.submit(
            OperationIntentRequest.model_validate(intents[active_target]),
            trusted_principal_id=None,
        )
        assert response["ok"] is True, json.dumps(response, sort_keys=True)
        operation_id = response["accepted"]["operation_id"]
        selected = wait_state(operation_id, {"executing"})["operation"]

        conflicting = client.submit(
            OperationIntentRequest.model_validate(intents[other]),
            trusted_principal_id=None,
        )
        assert conflicting["ok"] is False
        assert conflicting["error"]["code"] == "authoring_not_ready"
        assert conflicting["error"]["details"]["reason"] == (
            "positive_admission_required"
        )

        context = {
            "request_id": f"stop-manual-{active_target}",
            "request_owner_epoch": status["identity"]["owner_epoch"],
            "source_label": "native-manual-exclusion-test",
        }
        stopped = client.stop(
            StopIntentRequest.model_validate(
                {
                    "request_context": context,
                    "operations": [
                        {
                            "operation_id": operation_id,
                            "expected_operation_revision": selected[
                                "operation_revision"
                            ],
                            "expected_stop_generation": 0,
                        }
                    ],
                }
            ),
            trusted_principal_id=None,
        )
        assert stopped["ok"] is True, stopped
        wait_state(operation_id, {"stopped"})
        deadline = time.monotonic() + 2.0
        while not CellAdmissionClient(run_root).get()["authoring_allowed"]:
            assert time.monotonic() < deadline
            time.sleep(0.005)

        resumed_other = client.submit(
            OperationIntentRequest.model_validate(intents[other]),
            trusted_principal_id=None,
        )
        assert resumed_other["ok"] is True, json.dumps(
            resumed_other, sort_keys=True
        )
        completed = wait_state(
            resumed_other["accepted"]["operation_id"], {"completed"}
        )["operation"]
        assert all(row["terminal_error"] is None for row in completed["participants"])
        assert completed["active_stop_id"] is None
        with client.open_observation_session() as stream:
            observed_frame = stream.next(None, threading.Event())
            assert observed_frame.get("ok") is True, observed_frame
            observed = observed_frame["observation"]
        if other == "arm":
            assert observed["targets"][0]["positions_si"] == pytest.approx(positions)
        else:
            assert observed["targets"][1]["positions_si"] == pytest.approx(
                [
                    _status_target(status, presentation="gripper")[1]["command"][
                        "resource"
                    ]["max_position"]
                ]
            )
    finally:
        _stop_host(process)


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
    runner = OrchestratorRunner(
        KernelClient(recording),
        json.loads((fixture_root / "program_binding.json").read_text("utf-8")),
    )

    try:
        first_epoch = headless.execution_diagnostics()["status"]["identity"][
            "owner_epoch"
        ]
        cclc_owner_before_loss = CCLCOpenRequestClient(
            execution_run_root, timeout_seconds=5.0
        ).owner_identity()["request_owner_epoch"]
        cclc_before_loss = _wait_for_cclc_component(execution_run_root)
        planning_acceptance = _manual_joint_submission(
            headless,
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

        native = ExecutionCommandClient(execution_run_root, timeout_seconds=2.0)
        _wait_retired(native, planning_id)
        recovered_acceptance = _manual_joint_submission(
            headless,
            request_id="worker-after-cancel",
            joint_names=("joint1",),
            positions=(0.1,),
        )
        recovered_id = recovered_acceptance["accepted"]["operation_id"]
        recovered = _wait_terminal(runner, recovered_id)["operation"]
        assert recovered["state"] == "completed", recovered
        _wait_retired(native, recovered_id)

        marker.unlink()
        orphan_acceptance = _manual_joint_submission(
            headless,
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
        restarted = headless.execution_diagnostics()["status"]
        assert restarted["identity"]["owner_epoch"] != first_epoch
        cclc_after_loss = _wait_for_current_cclc_routes(execution_run_root)
        assert cclc_after_loss["component_lifecycle_state"] == "active"
        assert (
            cclc_after_loss["component_lifecycle_generation"]
            == cclc_before_loss["component_lifecycle_generation"]
        )
        before_domains = {
            domain["control_domain_id"]: domain
            for domain in cclc_before_loss["domains"]
        }
        after_domains = {
            domain["control_domain_id"]: domain for domain in cclc_after_loss["domains"]
        }
        assert after_domains.keys() == before_domains.keys()
        for domain_id, before_domain in before_domains.items():
            after_domain = after_domains[domain_id]
            assert after_domain["current_control_bindings"] == (
                before_domain["current_control_bindings"]
            )
            before_active = before_domain["active_control_configuration"]
            after_active = after_domain["active_control_configuration"]
            assert _stable_active_configuration(after_active) == (
                _stable_active_configuration(before_active)
            )
            assert (
                after_active["active_control_configuration_generation"]
                > before_active["active_control_configuration_generation"]
            )
            assert after_active["active_control_configuration_id"] != (
                before_active["active_control_configuration_id"]
            )
            assert after_active["source_runtime_epoch"] != (
                before_active["source_runtime_epoch"]
            )
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
        page = native.events(restarted["identity"]["owner_epoch"], 0)
        assert [event["record_kind"] for event in page["events"]] == ["host_replaced"]
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
                    "expected_retained_bundle_id": templates["retained_bundle_id"],
                    "expected_graph_revision": templates["graph_revision"],
                    "desired_configuration": stale_template["desired_configuration"],
                    "expected_active_control_configuration": stale_template[
                        "expected_active_control_configuration"
                    ],
                    "component_expectations": stale_template["component_expectations"],
                }
            )
        _submit_native_cclc_routes(
            execution_run_root,
            request_prefix="process.route.after-child-sigkill",
        )

        # The new owner adopted the exact durable route, proved current
        # quiescence/no-live-goal without a command, released only those
        # orphan-resource slots, and then republished normal readiness.
        gripper_after_restart = _manual_gripper_submission(
            headless,
            request_id="gripper-after-arm-host-loss",
            position_kind="linear_m",
            position=0.003,
        )
        gripper_after_restart_id = gripper_after_restart["accepted"]["operation_id"]
        gripper_terminal = _wait_terminal(runner, gripper_after_restart_id)["operation"]
        assert gripper_terminal["state"] == "completed"
        _wait_retired(native, gripper_after_restart_id)

        arm_after_restart = _manual_joint_submission(
            headless,
            request_id="arm-after-arm-host-loss",
            joint_names=("joint1",),
            positions=(0.05,),
        )
        arm_after_restart_id = arm_after_restart["accepted"]["operation_id"]
        arm_terminal = _wait_terminal(runner, arm_after_restart_id)["operation"]
        assert arm_terminal["state"] == "completed"
        _wait_retired(native, arm_after_restart_id)
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
    runner = OrchestratorRunner(
        KernelClient(recording),
        json.loads((fixture_root / "program_binding.json").read_text("utf-8")),
    )

    try:
        first_epoch = headless.execution_diagnostics()["status"]["identity"][
            "owner_epoch"
        ]
        native = ExecutionCommandClient(execution_run_root, timeout_seconds=2.0)
        accepted = _manual_gripper_submission(
            headless,
            request_id="active-gripper-host-loss",
            position_kind="linear_m",
            position=0.007,
            command_duration_s=5.0,
        )
        operation_id = accepted["accepted"]["operation_id"]
        before_loss = _wait_state(runner, operation_id, {"executing"})["operation"]
        assert before_loss["participants"][0]["resources"]
        _stop_host(process)

        process = _start_host(
            fixture_root,
            execution_state_root,
            execution_run_root,
            manifest,
        )
        restarted = headless.execution_diagnostics()["status"]
        assert restarted["identity"]["owner_epoch"] != first_epoch
        orphan = runner.operation(operation_id)["operation"]
        assert orphan["state"] == "intervention_required"
        terminal_error = orphan["participants"][0]["terminal_error"]
        assert terminal_error["code"] == "execution_host_replaced"
        assert terminal_error["details"]["reason"] == (
            "unreconciled_external_or_orphan_goal"
        )

        arm_after_restart = _manual_joint_submission(
            headless,
            request_id="arm-after-gripper-host-loss",
            joint_names=("joint1",),
            positions=(0.05,),
        )
        arm_after_restart_id = arm_after_restart["accepted"]["operation_id"]
        arm_terminal = _wait_terminal(runner, arm_after_restart_id)["operation"]
        assert arm_terminal["state"] == "completed"
        _wait_retired(native, arm_after_restart_id)

        gripper_after_restart = _manual_gripper_submission(
            headless,
            request_id="gripper-after-gripper-host-loss",
            position_kind="linear_m",
            position=0.002,
        )
        gripper_after_restart_id = gripper_after_restart["accepted"]["operation_id"]
        gripper_terminal = _wait_terminal(runner, gripper_after_restart_id)["operation"]
        assert gripper_terminal["state"] == "completed"
        _wait_retired(native, gripper_after_restart_id)
    finally:
        _stop_host(process)
        raw_api.close()
