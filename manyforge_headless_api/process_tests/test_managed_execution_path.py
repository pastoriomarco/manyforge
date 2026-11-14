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
import hashlib
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
    _request_key as _process_request_key,
    _required_path,
    _runner_command_end,
    _runner_command_succeeded,
    _started_run,
    keep_failure_evidence,
)
from manyforge_headless_api.process_tests.test_dual_robot_grouped_execution import (
    _native_events,
    _native_events_when,
    _start_motion,
)
from manyforge_behavior.request_keys import Part, format_request_key, with_part
from manyforge_headless_api.service import HeadlessService


def _request_key(label: str) -> str:
    """One request key per test label: 32 hex (contract 1.4)."""

    return hashlib.sha256(label.encode("utf-8")).hexdigest()[:32]


def _context(label: str, source_label: str) -> dict[str, Any]:
    return {"request_key": _request_key(label), "source_label": source_label}


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
    monkeypatch: pytest.MonkeyPatch,
    *,
    fake_worker_marker: Path | None = None,
    metadata_environment: str = "MANYFORGE_ASSISTANT_NATIVE_METADATA",
    package_environment: str = "MANYFORGE_ASSISTANT_NATIVE_PACKAGE_ROOT",
) -> Iterator[ProcessStack]:
    """The Mock Lite 6 cell under Manager, every component enabled.

    With `fake_worker_marker`, Manager's host plans with the fake candidate
    worker, which blocks its first planning request until it is cancelled.
    The environment names select the generated fixture package.
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
            with _assistant_stack(
                root,
                metadata_environment=metadata_environment,
                package_environment=package_environment,
            ) as stack:
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
                application_start_id=stack.application_start_id,
            ),
            trusted_principal_adapter=FixedTrustedPrincipalAdapter(),
        )
    )


def _wait_terminal(
    headless: HeadlessHttpClient, operation_id: int, application_start_id: str
) -> dict[str, Any]:
    deadline = time.monotonic() + 20.0
    last: dict[str, Any] = {}
    while time.monotonic() < deadline:
        last = headless.operation(operation_id, application_start_id=application_start_id)
        operation = last.get("operation")
        if isinstance(operation, dict) and operation.get("state") in TERMINAL_STATES:
            return last
        time.sleep(0.01)
    raise RuntimeError(f"operation did not reach a terminal state: {last}")


def _wait_state(
    headless: HeadlessHttpClient,
    operation_id: int,
    expected: set[str],
    application_start_id: str,
) -> dict[str, Any]:
    deadline = time.monotonic() + 20.0
    last: dict[str, Any] = {}
    while time.monotonic() < deadline:
        last = headless.operation(operation_id, application_start_id=application_start_id)
        operation = last.get("operation")
        if isinstance(operation, dict) and operation.get("state") in expected:
            return last
        time.sleep(0.005)
    raise RuntimeError(f"operation did not reach one of {sorted(expected)}: {last}")


def _wait_retired(client: ExecutionCommandClient, operation_id: int) -> None:
    """Observe exact automatic settlement; never perform client-side cleanup."""

    deadline = time.monotonic() + 5.0
    while True:
        projection = client.read(operation_id)
        if projection.get("resident") is False:
            return
        assert time.monotonic() < deadline, projection
        time.sleep(0.005)


def _wait_events_settled(client: ExecutionCommandClient) -> dict[str, Any]:
    """The event page once the Kernel has journaled its interrupt-rule states.

    Enabling the cell queues each global rule's first state; the Kernel's
    projection worker journals it asynchronously, while the baseline already
    carries it from memory: wait until each rule's latest row matches it.
    """

    def journaled(page: dict[str, Any]) -> bool:
        baseline = page.get("interrupt_rule_state_baseline") or {}
        if baseline.get("status") != "ready":
            return False
        latest: dict[str, str] = {}
        after, more = 0, True
        while more:
            rows = client.events(after)
            for event in rows["events"]:
                if event["record_kind"] == "interrupt_rule_state_changed":
                    latest[event["payload"]["rule_id"]] = event["payload"]["state"]
            after, more = rows["next_after_sequence"], rows["has_more"]
        return all(latest.get(row["rule_id"]) == row["state"]
                   for row in baseline["snapshot"]["states"])

    deadline = time.monotonic() + 10.0
    page = client.events(0)
    while not journaled(page):
        assert time.monotonic() < deadline, page
        time.sleep(0.01)
        page = client.events(0)
    return page


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
    label: str,
    positions_rad: list[float],
) -> dict[str, Any]:
    command = target["command"]
    manual_control = command["manual_control"]
    joint_names = [joint["source_joint_name"] for joint in command["joints"]]
    assert len(joint_names) == len(positions_rad)
    return {
        "request_context": _context(label, "composer_ui"),
        "program_id": manual_control["program_id"],
        "program_revision": manual_control["program_revision"],
        "program_binding_id": manual_control["program_binding_id"],
        "program_run_id": None,
        "expected_graph_revision": identity["graph_revision"],
        "expected_retained_bundle_id": identity["retained_bundle_id"],
        "expected_scene_revision": None,
        "participants": [
            {
                "target": command["target"],
                "required_capability_id": target["operation_capability_id"],
                "action_span": None,
                "payload": _planning_motion(
                    f"robot-control-{label}",
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
    status: dict[str, Any], *, label: str, position: float
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
        "request_context": _context(label, "composer_ui"),
        "program_id": manual_control["program_id"],
        "program_revision": manual_control["program_revision"],
        "program_binding_id": manual_control["program_binding_id"],
        "program_run_id": None,
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
    # A connected private client may be descheduled before its request. It
    # holds up no other connection of the lane, and its request is served
    # whenever it arrives: there is no first-packet time limit.
    pinned = PinnedRunRoot(execution_run_root)
    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as delayed:
        delayed.settimeout(5.0)
        with pinned.socket_address("execution-command.sock") as address:
            delayed.connect(address)
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as queued:
            queued.settimeout(5.0)
            with pinned.socket_address("execution-command.sock") as address:
                queued.connect(address)
            queued.sendall(rfc8785.dumps(get))
            queued_response = json.loads(queued.recv(4 * 1024 * 1024))
        # The silent connection is still open and unanswered (a closed one
        # would read end-of-file).
        delayed.setblocking(False)
        with pytest.raises(BlockingIOError):
            delayed.recv(4 * 1024 * 1024)
        delayed.settimeout(5.0)
        # A connection closed without a request is dropped; the lane goes on.
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as closed:
            with pinned.socket_address("execution-command.sock") as address:
                closed.connect(address)
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as after_close:
            after_close.settimeout(5.0)
            with pinned.socket_address("execution-command.sock") as address:
                after_close.connect(address)
            after_close.sendall(rfc8785.dumps(get))
            after_close_response = json.loads(after_close.recv(4 * 1024 * 1024))
        # The lateness under test: well beyond the former 1 s limit.
        time.sleep(1.5)
        delayed.sendall(rfc8785.dumps(get))
        delayed_response = json.loads(delayed.recv(4 * 1024 * 1024))
    assert queued_response == first
    assert after_close_response == first
    assert delayed_response == first


def _stoppable_motion_fixture() -> tuple[str, str]:
    """Select the assistant-native cell whose one Program is a slow joint move.

    The headless build generates it (manyforge_headless/CMakeLists.txt) and
    scripts/test-all.sh exports its own environment names, so
    ``_assistant_stack`` keeps its one-Program rule.
    """

    return ("MANYFORGE_ASSISTANT_STOPPABLE_MOTION_METADATA",
            "MANYFORGE_ASSISTANT_STOPPABLE_MOTION_PACKAGE_ROOT")


def test_internal_program_run_stop_uses_one_exact_priority_request(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    # The Forced Program End of a Kernel-held run, sent by the Program runner
    # as the first step of a Reset of the running cycle.
    metadata_environment, package_environment = _stoppable_motion_fixture()
    with _managed_cell(
        monkeypatch,
        metadata_environment=metadata_environment,
        package_environment=package_environment,
    ) as stack, HeadlessHttpClient(stack.base_url, timeout_seconds=20.0) as headless:
        run_root = stack.execution_run_root
        client = ExecutionCommandClient(
            run_root, timeout_seconds=5.0, attribution_transport="internal"
        )
        applied = headless.program_applied()
        stack.wait_authoring_admission(expected_allowed=True)
        reply = headless.program_run_start({
            "request_key": _process_request_key(),
            "source_label": "managed_execution_process_test",
            "scenario_id": applied["scenario_id"],
            "source_document_digest": applied["source_document_digest"],
            "expected_applied_record_digest": applied["source"]["record_digest"],
            "draft_program_revision": applied["program_revision"],
        })
        started = _started_run(*_runner_command_end(headless, reply, timeout_seconds=20.0))
        program_run_id = started["capture"]["program_run_id"]
        _start_motion(headless, started, request_id="program-run-stop.motion-start")

        # The run's one motion operation, executing.
        deadline = time.monotonic() + 30.0
        operation_id: int | None = None
        while operation_id is None or client.read(operation_id)["operation"]["state"] != "executing":
            assert time.monotonic() < deadline, (operation_id, _native_events(stack))
            if operation_id is None:
                accepted = [event["operation_id"] for event in _native_events(stack)
                            if event["record_kind"] == "operation_accepted"
                            and event["payload"]["intent"]["program_run_id"] == program_run_id]
                assert len(accepted) <= 1, accepted
                operation_id = accepted[0] if accepted else None
            time.sleep(0.01)

        # Reset the running cycle: the runner's Forced Program End, keyed as
        # part 1 of the Reset's request key, is its first step.
        cycle = headless.program_cycle_status()
        assert cycle["state"] == "running" and cycle["program_run_id"] == program_run_id, cycle
        reset_key = _process_request_key()
        reset = headless.program_cycle_reset({
            "request_key": reset_key,
            "application_start_id": stack.application_start_id,
            "source_label": "managed_execution_process_test",
            "program_run_id": program_run_id,
            "expected_cycle_generation": cycle["cycle_generation"],
            "expected_claim_generation": cycle["cycle_claim_generation"],
        })
        assert reset["command"]["request_key"] == reset_key, reset
        reset_row, _ = _runner_command_succeeded(headless, reset, timeout_seconds=60.0)
        assert reset_row["kind"] == "reset", reset_row
        assert headless.program_cycle_status()["state"] == "idle_after_reset"
        stop_key = format_request_key(
            with_part(bytes.fromhex(reset_key), Part.RESET_FORCED_END)
        )

        # Exactly one priority-lane request: one Program-end Stop event naming
        # the run, with the exact key and the runner's internal attribution.
        def program_end_events(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
            return [
                event["payload"] for event in rows
                if event["record_kind"] == "stop_event_accepted"
                and event["payload"]["requested"] == "program_end"
                and event["payload"]["program_run_id"] == program_run_id
            ]

        def stopped_states(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
            return [
                event["payload"]["operation_state"] for event in rows
                if event["record_kind"] == "state_observed"
                and event["operation_id"] == operation_id
                and event["payload"]["operation_state"]["state"] == "stopped"
            ]

        # The journal writer writes the rows shortly after the actions.
        events = _native_events_when(
            stack, lambda rows: bool(program_end_events(rows)) and bool(stopped_states(rows))
        )
        (stop_event,) = program_end_events(events)
        assert stop_event["request_key"] == stop_key, stop_event
        assert stop_event["attribution"]["transport"] == "internal", stop_event
        assert stop_event["source_label"] == "manyforge_behavior", stop_event
        assert operation_id in [row["operation_id"] for row in stop_event["covered"]], stop_event
        event_id = stop_event["event_id"]
        assert type(event_id) is int

        # Replaying the same request returns the kept answer.
        request = ProgramRunStopIntentRequest.model_validate({
            "request_context": {"request_key": stop_key, "source_label": "manyforge_behavior"},
            "program_run_id": program_run_id,
        })
        replay = client.stop_program_run(request)["accepted"]
        assert replay["request_key"] == stop_key, replay
        assert replay["event_id"] == event_id, replay
        assert replay["replay_status"] == "replayed_exact", replay
        assert replay["hold_revision"] == stop_event["hold_revision_after"], (replay, stop_event)

        deadline = time.monotonic() + 10.0
        stop_status: dict[str, Any] = {}
        while time.monotonic() < deadline:
            stop_status = client.read_stop(event_id)
            if stop_status["stop_status"]["state"] == "complete":
                break
            time.sleep(0.01)
        assert stop_status["stop_status"]["state"] == "complete"
        assert stop_status["stop_status"]["result"] == "stopped"
        # The operation ended stopped by that event.
        stopped_rows = stopped_states(events)
        assert stopped_rows, [event for event in events if event["operation_id"] == operation_id]
        assert stopped_rows[-1]["stop_event_id"] == event_id, stopped_rows[-1]

        # The Reset retired the stopped operation and the run's claim, and
        # with it the Program-run hold; authoring is open again.
        _wait_retired(client, operation_id)
        # The events reply's Kernel Stop facts (the page keeps only some).
        facts = client._exchange({
            "api_version": EXECUTION_COMMAND_VERSION,
            "after_sequence": 0,
            "command": "events",
            "limit": 1,
        })
        assert facts["ok"] is True and facts["program_run_hold"] is None, facts
        stack.wait_authoring_admission(expected_allowed=True)
        assert CellAdmissionClient(run_root).get()["authoring_allowed"] is True


def test_events_sample_live_progress_without_durable_progress_records(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    with _managed_cell(monkeypatch) as stack:
        execution = ExecutionCommandClient(stack.execution_run_root, timeout_seconds=3.0)
        status = execution.diagnostics()["status"]
        request = _direct_control_gripper_operation(status, label="progress-only",
                                                    position=0.004)
        payload = request["participants"][0]["payload"]
        payload["caller_options"][0]["value_s"] = 3.0
        payload["completion_timeout_s"] = 5.0
        accepted = execution.submit(OperationIntentRequest.model_validate(request),
                                    trusted_principal_id="test")
        assert accepted.get("ok") is True, accepted
        operation_id = accepted["accepted"]["operation_id"]
        cursor = 0
        previous = None
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            page = execution.events(cursor)
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
                label="manual-malformed-stop-operation", positions_rad=positions)
            operation_id = headless.execute(
                {**request, "application_start_id": stack.application_start_id}
            )["accepted"]["operation_id"]
            _wait_state(headless, operation_id, {"executing"}, stack.application_start_id)

            malformed_response = raw_api.post(
                "/api/execution/stops",
                content=b'{"operations":[',
                headers={"Content-Type": "application/json",
                         "X-Request-ID": "managed-malformed-stop"},
            )
            assert malformed_response.status_code == 200, malformed_response.text
            acceptance = malformed_response.json()["accepted"]
            # It acts with a fresh Headless key: class 1, minter 5.
            assert acceptance["request_key"][:16] == "40" + "00000000000005"
            # PD-1: an invalid operation Stop becomes Stop all.
            assert acceptance["requested"] == "operation"
            assert acceptance["applied"] == "stop_all"
            assert acceptance["widen_reason"] == "malformed"
            terminal = _wait_terminal(
                headless, operation_id, stack.application_start_id
            )["operation"]
            assert terminal["state"] == "stopped"
            assert terminal["stop_event_id"] == acceptance["event_id"]

            # The journal writer writes the row shortly after the Stop.
            deadline = time.monotonic() + 5.0
            row = None
            while row is None and time.monotonic() < deadline:
                with sqlite3.connect(stack.execution_state_root / "operation-journal.sqlite3") as db:
                    row = db.execute(
                        "SELECT payload_json FROM journal_records WHERE stop_event_id=? "
                        "AND record_kind='stop_event_accepted'",
                        (acceptance["event_id"],),
                    ).fetchone()
                if row is None:
                    time.sleep(0.01)
            assert row is not None
            projected = json.loads(row[0])
            assert projected["request_key"] == acceptance["request_key"]
            assert projected["reason"] == "recognized_stop_payload_invalid"
            assert projected["widen_reason"] == "malformed"

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
        client.identity()  # Ready before the read-only queries.
        probe = _pose(0.05, -0.02, 0.03)
        query = {
            "schema_version": "manyforge.target_observation_query.v3",
            "program_binding_id": binding["program_binding_id"],
            "kind": "frame_transform",
            "pose": probe,
            "source_frame": {"type": "cell_frame"},
            "target_frame": {"type": "cell_frame"},
        }

        def exchange(change: dict[str, Any]) -> dict[str, Any]:
            return client._exchange({"api_version": EXECUTION_COMMAND_VERSION,
                                     "command": "query_target", "query": {**query, **change}})

        # Read-only queries append nothing; the rule states enabling queued
        # are journaled first.
        before = _wait_events_settled(client)
        identity_transform = exchange({})
        assert identity_transform["ok"] is True, identity_transform
        typed = client.query_target(TargetObservationQuery.model_validate(query))
        assert typed["target_observation"]["value"] == (
            identity_transform["target_observation"]["value"])
        observation = identity_transform["target_observation"]
        assert observation["schema_version"] == "manyforge.target_observation.v3"
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
        assert client.events(0) == before


def test_program_operation_query_rejects_wrong_or_missing_context(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from manyforge_program_runner.execution_transport import ProgramRunnerExecutionTransport

    with _managed_cell(monkeypatch) as stack:
        binding = _program_binding(stack)
        client = ProgramRunnerExecutionTransport(stack.execution_run_root, timeout_seconds=2.0)
        client.identity()  # Ready before the read-only queries.
        query = {
            "schema_version": "manyforge.program_operation_query.v3",
            "program_binding_id": binding["program_binding_id"],
            "program_run_id": 1,
            "target": _arm_binding(binding)["target"],
        }
        before = client.events(0)
        for change, code in [
            ({}, "execution_operation_context_unavailable"),
            ({"target": {**query["target"], "command_target_id": "foreign"}},
             "execution_operation_target_unavailable"),
        ]:
            with pytest.raises(HeadlessApiError) as error:
                client.query_operation({**query, **change})
            assert error.value.code == code
        assert client.events(0) == before


def test_cell_admission_crosses_real_execution_seqpacket_surface(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    with _managed_cell(monkeypatch) as stack:
        run_root = stack.execution_run_root
        client = ExecutionCommandClient(run_root, timeout_seconds=5.0)
        get = {"api_version": "manyforge.execution_command.v4", "command": "cell_admission_get"}
        first = client._exchange(get)
        assert client._exchange(get) == first
        _assert_delayed_first_packet(run_root, get, first)
        assert set(first) == {"api_version", "authoring_admission", "command", "ok"}
        assert first["command"] == "cell_admission_get"

        status = client.diagnostics()["status"]
        motion_component, direct = _status_target(status, presentation="motion")
        observed = _status_observation(status, presentation="motion")
        operation = _direct_control_joint_operation(
            status["identity"], direct, label="handover.query",
            positions_rad=list(observed["positions_rad"]))
        proof = client.read_manual_handover(OperationIntentRequest.model_validate(operation),
                                            trusted_principal_id=None)
        assert proof["ok"] is True and proof["handover"] is None
        assert proof["availability"] == {
            "available": True, "cleanup_required": False, "unavailable_reason": None}
        # A supplied context is only a precondition. Reading cannot acquire a
        # claim or invent a held parent, including the valid initial revision 0.
        operation["manual_handover"] = {
            "program_run_id": 9_000,
            "claim_generation": 1,
            "hold_revision": 0,
        }
        proof = client.read_manual_handover(OperationIntentRequest.model_validate(operation),
                                            trusted_principal_id=None)
        assert proof["ok"] is True and proof["handover"] is None
        for field, value in (("hold_revision", True), ("hold_revision", -1),
                             ("hold_revision", float(2**53)), ("extra", 0)):
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
            "api_version": "manyforge.execution_command_response.v6",
            "command": "cell_admission_get",
            "error": {
                "code": "execution_control_command_unsupported",
                "details": {},
                "message": ("the independent Stop control socket accepts only Stop, "
                            "Stop status, carried-object-lost and speed changes; "
                            "component actions, the component model and "
                            "operator-alarm attention go to the independent "
                            "component socket"),
                "producer": "kernel",
            },
            "ok": False,
        }
        assert client._exchange_on(get, run_root / "execution-component.sock") == {
            "api_version": "manyforge.execution_command_response.v6",
            "command": "cell_admission_get",
            "error": {
                "code": "execution_component_command_unsupported",
                "details": {},
                "message": ("the independent component socket accepts only "
                            "component actions, the component model and "
                            "operator-alarm attention"),
                "producer": "kernel",
            },
            "ok": False,
        }
        assert client._exchange_on(get, run_root / "execution-observation.sock") == {
            "api_version": "manyforge.execution_command_response.v6",
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
            {"api_version": "manyforge.execution_command.v4", "command": "identity"},
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
            assert control["schema_version"] == "manyforge.execution_control_state.v8"
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
                # A retired application start: answered at once, never held.
                {"application_start_id": format(
                    int(stack.application_start_id, 16) % 0xFFFF_FFFF_FFFF_FFFF + 1, "016x"),
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
                label="blocked-direct-preview", positions_rad=positions)

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
                "application_start_id": stack.application_start_id,
                "request_context": _context(
                    "stop-while-preview-blocked", "headless_process_test"
                ),
                "operations": [{
                    "operation_id": 9_000_000,
                    "expected_operation_revision": 1,
                }],
                "reason": "prove Stop transport is independent from preview",
            }
            stopped = headless.stop_operations(stop_request)
            # PD-1: an operation Stop naming an unknown target is Stop all.
            assert stopped["accepted"]["applied"] == "stop_all"
            assert stopped["accepted"]["widen_reason"] == "unknown_target"

            # Stop preempted the blocked preview instead of waiting for it.
            preview_thread.join(timeout=3.0)
            assert preview_done.is_set()
            assert not preview_error
            assert preview_response[0]["ok"] is False
            replay = headless.stop_operations(stop_request)["accepted"]
            assert replay["replay_status"] == "replayed_exact", replay
            assert replay["event_id"] == stopped["accepted"]["event_id"]
            deadline = time.monotonic() + 5.0
            records: list[Any] = []
            while not records and time.monotonic() < deadline:
                with sqlite3.connect(
                    f"file:{stack.execution_state_root / 'operation-journal.sqlite3'}?mode=ro",
                    uri=True,
                ) as journal:
                    records = journal.execute(
                        "SELECT sequence,payload_json FROM journal_records "
                        "WHERE record_kind='stop_event_accepted' AND stop_event_id=?",
                        (stopped["accepted"]["event_id"],),
                    ).fetchall()
                if not records:
                    time.sleep(0.01)
            assert len(records) == 1, records
            sequence, payload_json = records[0]
            payload = json.loads(payload_json)
            # Headless forms the HTTP key: class 3, part 0 (contract 1.4).
            assert payload["request_key"] == (
                "c0" + stop_request["request_context"]["request_key"][2:]
            )
            assert payload["attribution"] == {
                "assurance": "externally_authenticated",
                "principal_id": "operator.process-test",
                "transport": "http",
            }
            events = execution.events(sequence - 1, limit=1)["events"]
            assert len(events) == 1
            assert events[0]["sequence"] == sequence
            assert events[0]["record_kind"] == "stop_event_accepted"
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
                                                     label="scale-lane-preview",
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

            def request(label: str, scale: float) -> dict[str, Any]:
                return {
                    "request_context": _context(label, "test"),
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
                "request_context": _context("scale-lane-stop", "test"),
                "program_run_id": 43,
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
                                                     label="tcp-barrier-preview",
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
                                                      label="tcp-barrier-set",
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
                "request_context": _context("tcp-barrier-stop", "test"),
                "program_run_id": 44,
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
