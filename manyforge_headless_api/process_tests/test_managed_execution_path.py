"""The execution host's command surface, through the Manager-managed host.

Manager starts the execution host as its child for the maintained Mock Lite 6
arm and gripper cell (the assistant-native fixture). These checks drive the
host's private sockets directly: Stop priority, the cell-admission seqpacket
surface, manual motion Stop recovery, preview versus Stop, ordered reads and
the target queries.
"""

from __future__ import annotations

import copy
from contextlib import contextmanager
import json
import math
import socket
import sqlite3
import threading
import time
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import Any, Iterator, Mapping

from fastapi.testclient import TestClient
import pytest
import rfc8785

from manyforge_headless_api.app import TrustedPrincipal, create_app
from manyforge_headless_api.cell_admission import CellAdmissionClient
from manyforge_headless_api.client import HeadlessHttpClient
from manyforge_headless_api.execution import (
    EXECUTION_COMMAND_VERSION,
    ExecutionCommandClient,
)
from manyforge_headless_api.models import (
    OperationIntentRequest,
    ProgramRunStopIntentRequest,
    RuntimeScaleControlRequest,
    TargetObservationQuery,
)
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.private_transport import PinnedRunRoot
from manyforge_headless_api.program_runner_client import ProgramRunnerHttpClient
from manyforge_headless_api.process_tests.test_process_stack import (
    ProcessStack,
    _assistant_stack,
    _required_path,
    keep_failure_evidence,
)
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


def _component_states(run_root: Path) -> list[dict[str, Any]]:
    return ExecutionCommandClient(run_root, timeout_seconds=2.0).component_model()[
        "component_states"
    ]


def _run_component_action(run_root: Path, component_id: str, action: str) -> dict[str, Any]:
    """One component action; returns the state carrying its result."""

    def state() -> dict[str, Any]:
        (row,) = [row for row in _component_states(run_root) if row["component_id"] == component_id]
        return row

    previous = state().get("last_action")
    accepted = ExecutionCommandClient(run_root).component_action(component_id, action)
    assert accepted["ok"] is True, accepted
    deadline = time.monotonic() + 10.0
    while True:
        current = state()
        last = current.get("last_action")
        if "action" not in current and last is not None and last != previous:
            assert last["action"] == action and last["outcome"] == "succeeded", current
            return current
        assert time.monotonic() < deadline, current
        time.sleep(0.01)


def _prepare_and_enable_all(run_root: Path) -> None:
    """Prepare owners before members, then enable everything but owners."""

    states = _component_states(run_root)
    owners = {row["owner_id"] for row in states if row.get("owner_id")}
    ordered = sorted(states, key=lambda row: row.get("owner_id") is not None)
    for row in ordered:
        _run_component_action(run_root, row["component_id"], "prepare")
    for row in ordered:
        if row["component_id"] not in owners:
            enabled = _run_component_action(run_root, row["component_id"], "enable")
            assert enabled["lifecycle"] == "enabled", enabled


@contextmanager
def _managed_cell(
    monkeypatch: pytest.MonkeyPatch, *, fake_worker_marker: Path | None = None
) -> Iterator[ProcessStack]:
    """The Mock Lite 6 cell under Manager, every component enabled.

    With `fake_worker_marker`, Manager's host plans with the fake candidate
    worker, which blocks its first planning request until it is cancelled.
    """

    if fake_worker_marker is not None:
        monkeypatch.setenv(
            "MANYFORGE_ROBOPLAN_CANDIDATE_WORKER",
            str(_required_path("MANYFORGE_ROBOPLAN_FAKE_WORKER")),
        )
        monkeypatch.setenv("MANYFORGE_FAKE_WORKER_MARKER", str(fake_worker_marker))
    # POSIX socket paths are bounded: keep the stack root short.
    with TemporaryDirectory(prefix="mf-managed-") as temporary:
        root = Path(temporary)
        try:
            with _assistant_stack(root) as stack:
                _prepare_and_enable_all(stack.execution_run_root)
                yield stack
        except BaseException:
            keep_failure_evidence(root)
            raise


def _program_binding(stack: ProcessStack) -> dict[str, Any]:
    assert stack.fixture_metadata is not None
    binding_id = stack.fixture_metadata["program_binding_id"]
    path = (stack.package_root / "program-bindings" / "sha256" /
            (binding_id.removeprefix("sha256:") + ".json"))
    binding = json.loads(path.read_text("utf-8"))
    assert binding["program_binding_id"] == binding_id
    return binding


def _arm_binding(binding: Mapping[str, Any]) -> dict[str, Any]:
    # The motion target: the one that carries the robot's TCPs.
    (arm,) = [row for row in binding["target_bindings"] if "robot_tcps" in row]
    return arm


def _in_process_api(stack: ProcessStack, execution: ExecutionCommandClient) -> TestClient:
    """Headless in process over the managed cell's execution host and Program
    runner (ordinary commands read the runner's cycle before dispatch)."""

    return TestClient(
        create_app(
            service=HeadlessService(
                UnusedCore(),  # type: ignore[arg-type]
                UnusedManager(),  # type: ignore[arg-type]
                execution,
                ProgramRunnerHttpClient(stack.program_runner_url, timeout_seconds=8.0),
            ),
            trusted_principal_adapter=FixedTrustedPrincipalAdapter(),
        )
    )


def _wait_terminal(headless: HeadlessHttpClient, operation_id: str) -> dict[str, Any]:
    deadline = time.monotonic() + 20.0
    last: dict[str, Any] = {}
    while time.monotonic() < deadline:
        last = headless.operation(operation_id)
        operation = last.get("operation")
        if isinstance(operation, dict) and operation.get("state") in TERMINAL_STATES:
            return last
        time.sleep(0.01)
    raise RuntimeError(f"operation did not reach a terminal state: {last}")


def _wait_state(
    headless: HeadlessHttpClient, operation_id: str, expected: set[str]
) -> dict[str, Any]:
    deadline = time.monotonic() + 20.0
    last: dict[str, Any] = {}
    while time.monotonic() < deadline:
        last = headless.operation(operation_id)
        operation = last.get("operation")
        if isinstance(operation, dict) and operation.get("state") in expected:
            return last
        time.sleep(0.005)
    raise RuntimeError(f"operation did not reach one of {sorted(expected)}: {last}")


def _wait_retired(client: ExecutionCommandClient, operation_id: str) -> None:
    """Observe exact automatic settlement; never perform client-side cleanup."""

    deadline = time.monotonic() + 5.0
    while True:
        projection = client.read(operation_id)
        if projection.get("resident") is False:
            return
        assert time.monotonic() < deadline, projection
        time.sleep(0.005)


def _wait_cell_latch_cleared(headless: HeadlessHttpClient) -> dict[str, Any]:
    """The latch is derived; it clears by itself once every
    component is healthy and idle. Nothing recovers or acknowledges it."""

    deadline = time.monotonic() + 10.0
    latch = headless.cell_latch()
    while latch.get("latched"):
        assert time.monotonic() < deadline, latch
        time.sleep(0.05)
        latch = headless.cell_latch()
    return latch


def _pose(x_m: float = 0.0, y_m: float = 0.0, z_m: float = 0.0) -> dict[str, Any]:
    return {
        "position_m": {"x_m": x_m, "y_m": y_m, "z_m": z_m},
        "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
    }


def _translation_error_m(left: Mapping[str, Any], right: Mapping[str, Any]) -> float:
    return math.sqrt(
        sum((float(left[axis]) - float(right[axis])) ** 2 for axis in ("x_m", "y_m", "z_m"))
    )


def _planning_motion(
    move_id: str, motion: dict[str, Any], base_frame: dict[str, str]
) -> dict[str, Any]:
    return {
        "kind": "motion",
        "move_id": move_id,
        "base_frame": base_frame,
        "tcp_endpoint": {"kind": "current"},
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
        "target_mode": "absolute",
        "target_reference": None,
        "offset_reference": None,
        "motion": motion,
        "contact_policy_id": None,
        "per_joint_dynamics_limits": {},
        "motion_interrupt_scopes": [],
        "runtime_object_bindings": [],
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


def _status_observation(status: Mapping[str, Any], *, presentation: str) -> dict[str, Any]:
    component, target = _status_target(status, presentation=presentation)
    target_key = (
        component["controllable_component_id"],
        target["command_target_id"],
        target["operation_capability_id"],
    )
    matching = [
        row["observed"]
        for row in status["control_state"]["observed_targets"]
        if (row["controllable_component_id"], row["command_target_id"],
            row["operation_capability_id"]) == target_key
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
        "expected_scene_revision": None,
        "participants": [
            {
                "target": command["target"],
                "required_capability_id": target["operation_capability_id"],
                "action_span": None,
                "payload": _planning_motion(
                    f"robot-control-{request_id}",
                    {
                        "kind": "joint_goal",
                        "joint_names": joint_names,
                        "positions_rad": positions_rad,
                    },
                    command["base_frame"],
                )
                | {
                    "tcp_endpoint": {"kind": "current"},
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
        "expected_scene_revision": None,
        "participants": [
            {
                "target": direct["target"],
                "required_capability_id": direct_target["operation_capability_id"],
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
                    "caller_options": [{"option_id": "command_duration_s", "value_s": 0.5}],
                },
            }
        ],
        "coordination_policy": "independent",
    }


def _assert_delayed_first_packet(
    execution_run_root: Path, get: dict[str, Any], first: dict[str, Any],
) -> None:
    # A connected private client may be descheduled before its first
    # packet. A second request stays queued during the 300 ms delay.
    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as delayed:
        delayed.settimeout(5.0)
        pinned = PinnedRunRoot(execution_run_root)
        with pinned.socket_address("execution-command.sock") as address:
            delayed.connect(address)
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as queued:
            with pinned.socket_address("execution-command.sock") as address:
                queued.connect(address)
            queued.sendall(rfc8785.dumps(get))
            queued.settimeout(0.3)
            with pytest.raises(socket.timeout):
                queued.recv(4 * 1024 * 1024)
            delayed.sendall(rfc8785.dumps(get))
            delayed_response = json.loads(delayed.recv(4 * 1024 * 1024))
            queued.settimeout(5.0)
            queued_response = json.loads(queued.recv(4 * 1024 * 1024))
    assert delayed_response == first
    assert queued_response == first


def test_internal_program_run_stop_uses_one_exact_priority_request(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    with _managed_cell(monkeypatch) as stack:
        run_root = stack.execution_run_root
        client = ExecutionCommandClient(
            run_root, timeout_seconds=5.0, attribution_transport="internal"
        )
        identity = client.identity()["identity"]
        component, direct = _execution_target(client.targets()["targets"], presentation="motion")
        (observed,) = [
            row for row in client.observation()["observation"]["targets"]
            if (row["controllable_component_id"], row["command_target_id"])
            == (component["controllable_component_id"], direct["command_target_id"])]
        current = [float(value) for value in observed["positions_si"]]
        goal = list(current)
        first_joint = direct["command"]["joints"][0]
        goal[0] = min(float(first_joint["max_position_rad"]),
                      max(float(first_joint["min_position_rad"]), goal[0] + 0.2))
        operation = _direct_control_joint_operation(
            identity, direct, request_id="program-run-stop.operation", positions_rad=goal)
        operation["program_run_id"] = "program-run.stop-process-test"
        accepted = client.submit(
            OperationIntentRequest.model_validate(operation), trusted_principal_id=None
        )["accepted"]
        request = ProgramRunStopIntentRequest.model_validate({
            "request_context": {
                "request_id": "program-run-stop.request",
                "request_owner_epoch": identity["owner_epoch"],
                "source_label": "manyforge_behavior",
                "correlation_id": "program-run.stop-process-test",
            },
            "program_run_id": "program-run.stop-process-test",
            "reason": "process-test Program scope Stop",
        })

        stopped = client.stop_program_run(request)
        replay = client.stop_program_run(request)
        assert stopped["owner_epoch"] == identity["owner_epoch"]
        assert stopped["accepted"]["request_id"] == request.request_context.request_id
        assert replay["accepted"]["stop_id"] == stopped["accepted"]["stop_id"]
        assert replay["accepted"]["replay_status"] == "replayed_exact"

        deadline = time.monotonic() + 10.0
        stop_status: dict[str, Any] = {}
        while time.monotonic() < deadline:
            stop_status = client.stop_status(stopped["accepted"]["stop_id"])
            if stop_status["stop_status"]["state"] in {"succeeded", "partial", "failed"}:
                break
            time.sleep(0.01)
        assert stop_status["stop_status"]["state"] == "succeeded"
        assert client.read(accepted["operation_id"])["operation"]["state"] == "stopped"

        # Cross-lane fence: Stop and its status travel the control socket,
        # the release travels the ordinary one and acts only on the exact
        # Stop identity and generation it names, never on arrival order.
        stale_release = client.release_program_run_stop_fence(
            "program-run.stop-process-test", stopped["accepted"]["stop_id"],
            stopped["accepted"]["stop_generation"] + 1)
        assert stale_release["ok"] is False, stale_release
        for _ in range(2):
            released = client.release_program_run_stop_fence(
                "program-run.stop-process-test", stopped["accepted"]["stop_id"],
                stopped["accepted"]["stop_generation"])
            assert released["ok"] is True, released

        event_deadline = time.monotonic() + 5.0
        stop_events: list[dict[str, Any]] = []
        while not stop_events and time.monotonic() < event_deadline:
            stop_events = [event for event in client.events(identity["owner_epoch"], 0)["events"]
                           if event.get("request_id") == request.request_context.request_id]
            time.sleep(0.01)
        assert stop_events

        # Releasing the Program-run hold lets the Kernel retire this clean,
        # already-settled manual operation. Do not invent a second Reset.
        _wait_retired(client, accepted["operation_id"])
        stack.wait_authoring_admission(expected_allowed=True)
        assert CellAdmissionClient(run_root).get()["authoring_allowed"] is True

        next_operation = _direct_control_joint_operation(
            identity, direct, request_id="program-run-stop.next-operation", positions_rad=current)
        next_operation["program_run_id"] = "program-run.stop-process-test.next"
        next_accepted = client.submit(
            OperationIntentRequest.model_validate(next_operation), trusted_principal_id=None
        )["accepted"]
        deadline = time.monotonic() + 20.0
        next_terminal: dict[str, Any] = {}
        while time.monotonic() < deadline:
            next_terminal = client.read(next_accepted["operation_id"])["operation"]
            if next_terminal["state"] in TERMINAL_STATES:
                break
            time.sleep(0.01)
        assert next_terminal["state"] == "completed", next_terminal


def test_events_sample_live_progress_without_durable_progress_records(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    with _managed_cell(monkeypatch) as stack:
        execution = ExecutionCommandClient(stack.execution_run_root, timeout_seconds=3.0)
        status = execution.diagnostics()["status"]
        request = _direct_control_gripper_operation(status, request_id="progress-only",
                                                    position=0.004)
        payload = request["participants"][0]["payload"]
        payload["caller_options"][0]["value_s"] = 3.0
        payload["completion_timeout_s"] = 5.0
        accepted = execution.submit(OperationIntentRequest.model_validate(request),
                                    trusted_principal_id="test")
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
            pytest.fail("progress did not advance independently of journal events")


def test_manual_motion_malformed_stop_recovers_with_durable_evidence(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    with _managed_cell(monkeypatch) as stack:
        run_root = stack.execution_run_root
        execution = ExecutionCommandClient(run_root, timeout_seconds=3.0)
        raw_api = _in_process_api(stack, execution)
        headless = HeadlessHttpClient("http://headless.test", client=raw_api)
        try:
            status = headless.execution_diagnostics()["status"]
            positions = list(_status_observation(status, presentation="motion")["positions_rad"])
            positions[0] = 0.55
            request = _direct_control_joint_operation(
                status["identity"], _status_target(status, presentation="motion")[1],
                request_id="manual-malformed-stop-operation", positions_rad=positions)
            operation_id = headless.execute(request)["accepted"]["operation_id"]
            _wait_state(headless, operation_id, {"executing"})

            malformed_response = raw_api.post(
                "/api/execution/stops",
                content=b'{"operations":[',
                headers={"Content-Type": "application/json",
                         "X-Request-ID": "managed-malformed-stop"},
            )
            assert malformed_response.status_code == 200, malformed_response.text
            acceptance = malformed_response.json()["accepted"]
            assert acceptance["request_id"] == "malformed-stop.managed-malformed-stop"
            assert acceptance["resolution_disposition"] == "promoted_to_full"
            assert acceptance["resolution_reason"] == "malformed_ordinary_intent"
            terminal = _wait_terminal(headless, operation_id)["operation"]
            assert terminal["state"] == "stopped"
            assert terminal["active_stop_id"] == acceptance["stop_id"]

            with sqlite3.connect(stack.execution_state_root / "operation-journal.sqlite3") as db:
                row = db.execute(
                    "SELECT payload_json FROM journal_records WHERE request_id=? "
                    "AND record_kind='stop_acceptance_projected'",
                    ("malformed-stop.managed-malformed-stop",),
                ).fetchone()
            assert row is not None
            projected = json.loads(row[0])
            assert projected["acceptance"]["malformed_ingress_reason"] == (
                "recognized_stop_payload_invalid")
            assert projected["acceptance"]["normalized_request_digest"] is None
            assert projected["intent"] == {"intent_kind": "malformed_ingress", "value": None}

            # No recovery request exists: the latch clears by itself once every
            # component is healthy and idle again.
            assert _wait_cell_latch_cleared(headless)["latched"] is False
            # Authoring admission is the Kernel's own fact: it reopens once the
            # Kernel retires the stopped operation and then releases its
            # claim, independently of the component-derived latch.
            stack.wait_authoring_admission(expected_allowed=True)
            assert CellAdmissionClient(run_root).get()["authoring_allowed"] is True
        finally:
            raw_api.close()


def test_frame_transform_query_uses_cell_resolver_and_direct_frames(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    with _managed_cell(monkeypatch) as stack:
        binding = _program_binding(stack)
        arm = _arm_binding(binding)
        # Direct references to two of the arm's compiled frames.
        frames = sorted((row["reference"] for row in binding["frame_bindings"]
                         if row["reference"]["component_id"] == arm["target"]["component_id"]),
                        key=lambda frame: frame["frame_id"])
        assert len(frames) >= 2, frames
        client = ExecutionCommandClient(stack.execution_run_root, timeout_seconds=10.0)
        identity = client.identity()["identity"]
        probe = _pose(0.05, -0.02, 0.03)
        query = {
            "schema_version": "manyforge.target_observation_query.v2",
            "owner_epoch": identity["owner_epoch"],
            "program_binding_id": binding["program_binding_id"],
            "kind": "frame_transform",
            "pose": probe,
            "source_frame": {"type": "cell_frame"},
            "target_frame": {"type": "cell_frame"},
        }

        def exchange(change: dict[str, Any]) -> dict[str, Any]:
            return client._exchange({"api_version": EXECUTION_COMMAND_VERSION,
                                     "command": "query_target", "query": {**query, **change}})

        before = client.events(identity["owner_epoch"], 0)
        identity_transform = exchange({})
        assert identity_transform["ok"] is True, identity_transform
        typed = client.query_target(TargetObservationQuery.model_validate(query))
        assert typed["target_observation"]["value"] == (
            identity_transform["target_observation"]["value"])
        observation = identity_transform["target_observation"]
        assert observation["schema_version"] == "manyforge.target_observation.v2"
        for key, value in query.items():
            if key != "schema_version":
                assert observation[key] == value
        assert observation["resolution_evidence_digests"] == []
        assert observation["scene_revision"]["scene_id"]
        assert _translation_error_m(observation["value"]["position_m"],
                                    probe["position_m"]) < 1e-12

        source = {"type": "component_frame", "frame": frames[0]}
        target = {"type": "component_frame", "frame": frames[1]}
        forward = exchange({"source_frame": source, "target_frame": target})
        assert forward["ok"] is True, forward
        assert len(forward["target_observation"]["resolution_evidence_digests"]) == 2
        backward = exchange({"pose": forward["target_observation"]["value"],
                             "source_frame": target, "target_frame": source})
        assert backward["ok"] is True, backward
        assert _translation_error_m(backward["target_observation"]["value"]["position_m"],
                                    probe["position_m"]) < 1e-9

        # Both a missing component and a missing frame identify the exact
        # direct reference that failed resolution.
        for frame in ({**frames[0], "component_id": "absent"},
                      {**frames[0], "frame_id": "absent"}):
            rejected = exchange({"source_frame": {"type": "component_frame", "frame": frame}})
            assert rejected["ok"] is False, rejected
            assert rejected["error"]["code"] == "frame_missing"
            assert rejected["error"]["details"] == {
                "reference_type": "component_frame", "reference_id": frame["frame_id"],
                "component_id": frame["component_id"],
            }
        rejected = exchange({"source_frame": {"type": "scene_resource", "scene_resource_id": "absent"}})
        assert rejected["ok"] is False, rejected
        assert rejected["error"]["details"] == {
            "reason": "program_scene_resource_missing", "reference_type": "scene_resource",
            "reference_id": "absent",
        }
        malformed = exchange({"source_frame": {"type": "component_frame", "frame": {"frame_id": frames[0]["frame_id"]}}})
        assert malformed["ok"] is False
        assert malformed["error"]["code"] == "execution_command_invalid"
        assert exchange({"owner_epoch": "old.owner"})["error"]["code"] == (
            "execution_target_owner_stale")
        assert client.events(identity["owner_epoch"], 0) == before


def test_program_operation_query_rejects_wrong_or_missing_context(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from manyforge_program_runner.execution_transport import ProgramRunnerExecutionTransport

    with _managed_cell(monkeypatch) as stack:
        binding = _program_binding(stack)
        client = ProgramRunnerExecutionTransport(stack.execution_run_root, timeout_seconds=2.0)
        identity = client.identity()["identity"]
        query = {
            "schema_version": "manyforge.program_operation_query.v2",
            "owner_epoch": identity["owner_epoch"],
            "program_binding_id": binding["program_binding_id"],
            "program_run_id": "n3-program-run-1",
            "target": _arm_binding(binding)["target"],
        }
        before = client.events(identity["owner_epoch"], 0)
        for change, code in [
            ({}, "execution_operation_context_unavailable"),
            ({"owner_epoch": "old"}, "stale_request_owner_epoch"),
            ({"target": {**query["target"], "command_target_id": "foreign"}},
             "execution_operation_target_unavailable"),
        ]:
            with pytest.raises(HeadlessApiError) as error:
                client.query_operation({**query, **change})
            assert error.value.code == code
        assert client.events(identity["owner_epoch"], 0) == before


def test_cell_admission_crosses_real_execution_seqpacket_surface(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    with _managed_cell(monkeypatch) as stack:
        run_root = stack.execution_run_root
        client = ExecutionCommandClient(run_root, timeout_seconds=5.0)
        get = {"api_version": "manyforge.execution_command.v3", "command": "cell_admission_get"}
        first = client._exchange(get)
        assert client._exchange(get) == first
        _assert_delayed_first_packet(run_root, get, first)
        assert set(first) == {"api_version", "authoring_admission", "command", "ok"}
        assert first["command"] == "cell_admission_get"

        status = client.diagnostics()["status"]
        motion_component, direct = _status_target(status, presentation="motion")
        observed = _status_observation(status, presentation="motion")
        operation = _direct_control_joint_operation(
            status["identity"], direct, request_id="handover.query",
            positions_rad=list(observed["positions_rad"]))
        proof = client.read_manual_handover(OperationIntentRequest.model_validate(operation),
                                            trusted_principal_id=None)
        assert proof["ok"] is True and proof["handover"] is None
        assert proof["availability"] == {
            "available": True, "cleanup_required": False, "unavailable_reason": None}
        # A supplied context is only a precondition. Reading cannot acquire a
        # claim or invent a held parent, including the valid initial revision 0.
        operation["manual_handover"] = {
            "program_runner_owner_epoch": "absent.runner",
            "program_run_id": "absent.parent",
            "claim_generation": 1,
            "selection_revision": 0,
        }
        proof = client.read_manual_handover(OperationIntentRequest.model_validate(operation),
                                            trusted_principal_id=None)
        assert proof["ok"] is True and proof["handover"] is None
        for field, value in (("selection_revision", True), ("selection_revision", -1),
                             ("selection_revision", float(2**53)), ("extra", 0)):
            malformed_operation = copy.deepcopy(operation)
            malformed_operation["manual_handover"][field] = value
            invalid = client._exchange({
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "read_manual_handover",
                "operation": malformed_operation,
                "attribution": {"transport": "http", "assurance": "unverified",
                                "principal_id": None},
            })
            assert invalid["ok"] is False
        assert client._exchange(get) == first

        malformed = client._exchange({**get, "unexpected": True})
        assert malformed["ok"] is False
        assert malformed["error"]["code"] == "execution_command_invalid"

        assert client._exchange_on(get, run_root / "execution-control.sock") == {
            "api_version": "manyforge.execution_command_response.v4",
            "command": "cell_admission_get",
            "error": {
                "code": "execution_control_command_unsupported",
                "details": {},
                "message": ("the independent Stop control socket accepts only Stop, "
                            "Stop status and operator-alarm attention"),
                "producer": "kernel",
            },
            "ok": False,
        }
        assert client._exchange_on(get, run_root / "execution-observation.sock") == {
            "api_version": "manyforge.execution_command_response.v4",
            "command": "cell_admission_get",
            "error": {
                "code": "execution_observation_command_unsupported",
                "details": {},
                "message": "the independent observation socket accepts only observation",
                "producer": "kernel",
            },
            "ok": False,
        }
        wrong_observation_command = client._exchange_on(
            {"api_version": "manyforge.execution_command.v3", "command": "identity"},
            run_root / "execution-observation.sock")
        assert wrong_observation_command["ok"] is False
        assert wrong_observation_command["error"]["code"] == (
            "execution_observation_command_unsupported")
        observation = client.observation()
        assert observation.get("ok") is True, observation
        assert observation["command"] == "observation"

        # One persistent private session: a second credited request crosses
        # the same accepted socket.
        motion_target_key = (motion_component["controllable_component_id"],
                             direct["command_target_id"], direct["operation_capability_id"])

        def observed_motion_state() -> dict[str, Any]:
            control = client.control_state()["control_state"]
            assert control["schema_version"] == "manyforge.execution_control_state.v7"
            matching = [row["observed"] for row in control["observed_targets"]
                        if (row["controllable_component_id"], row["command_target_id"],
                            row["operation_capability_id"]) == motion_target_key]
            assert len(matching) == 1
            assert matching[0]["kind"] == "joint_positions"
            return matching[0]

        before_motion = observed_motion_state()
        with client.open_observation_session() as observation_session:
            first_frame = observation_session.next(None, threading.Event())["observation"]
            repeated_frame = observation_session.next(
                {"owner_epoch": "retired-owner-epoch",
                 "source_epoch": first_frame["source_epoch"],
                 "source_sequence": first_frame["source_sequence"]},
                threading.Event(),
            )["observation"]
        after_motion = observed_motion_state()
        assert repeated_frame["source_sequence"] == first_frame["source_sequence"]
        for frame in (first_frame, repeated_frame):
            (motion,) = [row for row in frame["targets"]
                         if (row["controllable_component_id"], row["command_target_id"],
                             row["operation_capability_id"]) == motion_target_key]
            assert (motion["source_epoch"] == before_motion["source_epoch"]
                    == after_motion["source_epoch"])
            assert (before_motion["sequence"] <= motion["source_sequence"]
                    <= after_motion["sequence"])
            assert (motion["positions_si"] == before_motion["positions_rad"]
                    == after_motion["positions_rad"])
        # Per-target rows are live snapshots (commanded and measured alike);
        # the frame identity around them repeats exactly.
        per_target = {"targets", "measured_targets"}
        assert ({key: value for key, value in repeated_frame.items() if key not in per_target}
                == {key: value for key, value in first_frame.items() if key not in per_target})


def test_execution_host_scale_selector_codec_preserves_singular_scope(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """The host's decoder with the runtime-scale selector variants."""

    with _managed_cell(monkeypatch) as stack:
        binding = _program_binding(stack)
        target = _arm_binding(binding)["target"]
        client = ExecutionCommandClient(stack.execution_run_root, timeout_seconds=5.0)
        singular_selector = {
            "program_binding_id": binding["program_binding_id"],
            **target,
        }

        def get(selector: dict[str, Any]) -> dict[str, Any]:
            return client._exchange({"api_version": EXECUTION_COMMAND_VERSION,
                                     "command": "get_runtime_scale_control",
                                     "selector": selector})

        singular_before = get(singular_selector)
        assert singular_before["ok"] is True, singular_before
        assert singular_before["runtime_scale_control"]["selector"] == singular_selector
        group = [target] * 2
        for selector in (
            {**singular_selector, "group_targets": group},
            {"program_binding_id": binding["program_binding_id"], "group_targets": group},
        ):
            rejected = get(selector)
            assert rejected["ok"] is False
            assert rejected["error"]["code"] == "execution_command_invalid"
        assert get(singular_selector) == singular_before


def test_slow_preview_cannot_delay_the_standard_stop_path(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    marker = tmp_path / "blocked-preview-worker"
    with _managed_cell(monkeypatch, fake_worker_marker=marker) as stack:
        execution = ExecutionCommandClient(stack.execution_run_root, timeout_seconds=3.0)
        raw_api = _in_process_api(stack, execution)
        headless = HeadlessHttpClient("http://headless.test", client=raw_api)
        preview_done = threading.Event()
        preview_response: list[dict[str, Any]] = []
        preview_error: list[Exception] = []
        preview_thread: threading.Thread | None = None
        try:
            status = headless.execution_diagnostics()["status"]
            positions = [float(position) for position in
                         _status_observation(status, presentation="motion")["positions_rad"]]
            positions[0] = 0.03
            intent = _direct_control_joint_operation(
                status["identity"], _status_target(status, presentation="motion")[1],
                request_id="blocked-direct-preview", positions_rad=positions)

            def run_preview() -> None:
                try:
                    preview_response.append(execution.preview_motion(
                        OperationIntentRequest.model_validate(intent), trusted_principal_id=None))
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
                "operations": [{
                    "operation_id": "non-resident-operation",
                    "expected_operation_revision": 1,
                    "expected_stop_generation": 0,
                }],
                "reason": "prove Stop transport is independent from preview",
            }
            stopped = headless.stop_operations(stop_request)
            assert stopped["accepted"]["resolution_disposition"] == "promoted_to_full"
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
            deadline = time.monotonic() + 5.0
            records: list[Any] = []
            while not records and time.monotonic() < deadline:
                with sqlite3.connect(
                    f"file:{stack.execution_state_root / 'operation-journal.sqlite3'}?mode=ro",
                    uri=True,
                ) as journal:
                    records = journal.execute(
                        "SELECT sequence,payload_json FROM journal_records "
                        "WHERE record_kind='stop_acceptance_projected' AND request_id=?",
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
            events = execution.events(status["identity"]["owner_epoch"], sequence - 1,
                                      limit=1)["events"]
            assert len(events) == 1
            assert events[0]["sequence"] == sequence
            assert events[0]["record_kind"] == "stop_acceptance_projected"
            assert events[0]["operation_id"] is None
            assert events[0]["operation_revision"] is None
            assert events[0]["payload"] == payload
        finally:
            if preview_thread is not None:
                preview_thread.join(timeout=3.0)
            raw_api.close()


def test_runtime_scale_is_answered_on_its_own_lane_while_a_preview_blocks_submission(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """Speed changes from both sockets reach the scale lane, never the
    submission lane that a blocked preview holds.

    The full-lane refusal itself (`execution_runtime_scale_lane_busy` for the
    ninth waiting request) needs the lane worker held, which only an Apply's
    exclusive composition section does; a process test cannot hold it, so this
    checks the routing.
    """

    marker = tmp_path / "blocked-preview-worker"
    with _managed_cell(monkeypatch, fake_worker_marker=marker) as stack:
        run_root = stack.execution_run_root
        execution = ExecutionCommandClient(run_root, timeout_seconds=2.0)
        internal = ExecutionCommandClient(run_root, timeout_seconds=2.0,
                                          attribution_transport="internal")
        planning = ExecutionCommandClient(run_root, timeout_seconds=30.0)
        preview_thread: threading.Thread | None = None
        try:
            status = execution.diagnostics()["status"]
            identity = status["identity"]
            _, target = _status_target(status, presentation="motion")
            positions = list(_status_observation(status, presentation="motion")["positions_rad"])
            positions[0] = 0.03
            motion = _direct_control_joint_operation(identity, target,
                                                     request_id="scale-lane-preview",
                                                     positions_rad=positions)
            preview_thread = threading.Thread(
                target=lambda: planning.preview_motion(
                    OperationIntentRequest.model_validate(motion), trusted_principal_id=None),
                daemon=True)
            preview_thread.start()
            deadline = time.monotonic() + 2.0
            while not marker.exists() and time.monotonic() < deadline:
                time.sleep(0.005)
            assert marker.exists()

            binding = _program_binding(stack)
            selector = {"program_binding_id": binding["program_binding_id"],
                        **_arm_binding(binding)["target"]}

            def request(request_id: str, scale: float) -> dict[str, Any]:
                return {
                    "request_context": {"request_id": request_id,
                                        "request_owner_epoch": identity["owner_epoch"],
                                        "source_label": "test"},
                    "selector": selector,
                    "expected_control_generation": 0,
                    "scale": scale,
                }

            def reached_the_kernel(response: dict[str, Any]) -> bool:
                # Accepted, or refused by the Kernel itself: never a lane or
                # socket refusal, never a timeout behind the blocked preview.
                return response["ok"] is True or response["error"]["code"] not in {
                    "execution_submission_lane_busy",
                    "execution_runtime_scale_lane_busy",
                    "execution_control_command_unsupported",
                    "execution_ordinary_command_unsupported",
                }

            # Control socket (the Headless route).
            control = execution.set_runtime_scale_control(
                RuntimeScaleControlRequest.model_validate(request("scale-lane-control", 0.5)),
                trusted_principal_id=None)
            assert reached_the_kernel(control), control
            if control["ok"]:
                assert control["accepted"]["active_operation_id"] is None
            # Ordinary socket: the same lane, no new refusal.
            ordinary = execution._exchange({
                "api_version": EXECUTION_COMMAND_VERSION,
                "attribution": {"transport": "http", "assurance": "unverified",
                                "principal_id": None},
                "command": "set_runtime_scale_control",
                "request": request("scale-lane-ordinary", 0.4),
            })
            assert reached_the_kernel(ordinary), ordinary
            assert preview_thread.is_alive()
            assert internal.stop_program_run(ProgramRunStopIntentRequest.model_validate({
                "request_context": {"request_id": "scale-lane-stop",
                                    "request_owner_epoch": identity["owner_epoch"],
                                    "source_label": "test"},
                "program_run_id": "program-run.scale-lane",
                "reason": "release blocked preview",
            }))["ok"] is True
        finally:
            if preview_thread is not None:
                preview_thread.join(timeout=3.0)


def test_ordered_active_tcp_read_waits_for_queued_set_tcp(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """The read is behind an already-enqueued Set TCP, not a send receipt."""

    marker = tmp_path / "blocked-preview-worker"
    with _managed_cell(monkeypatch, fake_worker_marker=marker) as stack:
        run_root = stack.execution_run_root
        execution = ExecutionCommandClient(run_root, timeout_seconds=2.0)
        internal = ExecutionCommandClient(run_root, timeout_seconds=2.0,
                                          attribution_transport="internal")
        planning = ExecutionCommandClient(run_root, timeout_seconds=30.0)
        pinned = PinnedRunRoot(run_root)
        clients: list[socket.socket] = []
        preview_thread: threading.Thread | None = None
        preview_errors: list[Exception] = []
        try:
            status = execution.diagnostics()["status"]
            identity = status["identity"]
            component, target = _status_target(status, presentation="motion")
            positions = list(_status_observation(status, presentation="motion")["positions_rad"])
            positions[0] = 0.03
            motion = _direct_control_joint_operation(identity, target,
                                                     request_id="tcp-barrier-preview",
                                                     positions_rad=positions)
            baseline = execution._exchange({"api_version": EXECUTION_COMMAND_VERSION,
                                            "command": "read_active_tcp"})
            assert baseline["ok"] is True
            row = next(row for row in baseline["robots"]
                       if row["component_id"] == component["controllable_component_id"]
                       and row["entry"] is not None)
            chosen_entry = next(
                tcp["entry"] for tcp in target["command"]["target_descriptor"]["tcps"]
                if tcp["mounted_here"] and tcp["entry"] != row["entry"])

            def preview() -> None:
                try:
                    planning.preview_motion(OperationIntentRequest.model_validate(motion),
                                            trusted_principal_id=None)
                except Exception as error:
                    preview_errors.append(error)

            preview_thread = threading.Thread(target=preview, daemon=True)
            preview_thread.start()
            deadline = time.monotonic() + 2.0
            while not marker.exists() and time.monotonic() < deadline:
                time.sleep(0.005)
            assert marker.exists()
            set_tcp = _direct_control_joint_operation(identity, target,
                                                      request_id="tcp-barrier-set",
                                                      positions_rad=positions)
            set_tcp["participants"][0]["payload"] = {
                "kind": "set_tcp", "tcp": {"kind": "entry", **chosen_entry}}
            for packet in (
                {"api_version": EXECUTION_COMMAND_VERSION, "command": "submit",
                 "attribution": {"transport": "http", "assurance": "unverified",
                                 "principal_id": None},
                 "operation": set_tcp},
                {"api_version": EXECUTION_COMMAND_VERSION, "command": "read_active_tcp",
                 "ordered": True},
            ):
                client = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
                client.settimeout(5.0)
                with pinned.socket_address("execution-command.sock") as address:
                    client.connect(address)
                client.send(rfc8785.dumps(packet))
                clients.append(client)
            # The ordinary request has been handled after both connections,
            # while the preview holds the submission lane.
            assert execution._exchange({"api_version": EXECUTION_COMMAND_VERSION,
                                        "command": "read_active_tcp"})["ok"] is True
            clients[1].settimeout(0.05)
            with pytest.raises(TimeoutError):
                clients[1].recv(4 * 1024 * 1024)
            clients[1].settimeout(5.0)
            assert internal.stop_program_run(ProgramRunStopIntentRequest.model_validate({
                "request_context": {"request_id": "tcp-barrier-stop",
                                    "request_owner_epoch": identity["owner_epoch"],
                                    "source_label": "test"},
                "program_run_id": "program-run.tcp-barrier",
                "reason": "release blocked preview",
            }))["ok"] is True
            set_result = json.loads(clients[0].recv(4 * 1024 * 1024))
            read_result = json.loads(clients[1].recv(4 * 1024 * 1024))
            assert set_result["ok"] is True, set_result
            assert read_result["ok"] is True, read_result
            actual = next(item for item in read_result["robots"]
                          if item["component_id"] == row["component_id"])
            assert actual["entry"] == chosen_entry
            assert actual["sequence"] > row["sequence"]
        finally:
            for client in clients:
                client.close()
            if preview_thread is not None:
                preview_thread.join(timeout=3.0)
