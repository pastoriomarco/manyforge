"""The Program runner's command and status sockets: dispatch and lifecycle."""

from __future__ import annotations

from collections.abc import Iterator
from contextlib import contextmanager
import hashlib
from pathlib import Path
import queue
import threading
from typing import Any

import pytest

import manyforge_headless_py as core

from manyforge_headless_api.models import (
    ProgramHaltResumeRequest,
    ProgramRunStartRequest,
    ProgramScenePreparationRequest,
)
from manyforge_program_runner.app import (
    PROGRAM_RUNNER_COMMANDS,
    ProgramRunnerApplication,
    _ProgramStatusPump,
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
        "schema_version": "manyforge.program_run_status.v6",
        "owner_id": "program-runner.cell-a",
        "run_revision": 1,
        "state": state,
        "capture": {
            "schema_version": "manyforge.program_run_capture.v3",
            "program_run_id": 1,
            "applied_record_digest": _DIGEST_C,
            "scenario_id": "cell-pick",
            "source_document_digest": _DIGEST_A,
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
        "node_running_since_utc_ns": {"root": 1_795_000_000_000_000_000},
        "node_wait_values": {},
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

    def read_runtime_blackboard(self, program_run_id: int) -> dict[str, Any]:
        self.calls.append(("read_runtime_blackboard", program_run_id))
        return {"program_run_id": program_run_id, "keys": []}

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


# Headless's code for a request the runner refuses as malformed.
_MALFORMED = {
    "code": "program_runner_http_error",
    "details": {},
    "message": "Program runner returned HTTP 422",
    "producer": "program_runner",
}


class _Client:
    """Headless's side of the sockets, through the core's link."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self.link = core.ProgramRunnerLink(str(root), 5.0)

    def call(self, command: str, request: dict[str, Any] | None = None) -> dict[str, Any]:
        return self.link.call(command, {} if request is None else request)

    def value(self, command: str, request: dict[str, Any] | None = None) -> Any:
        reply = self.call(command, request)
        assert reply["ok"] is True, reply
        return reply["value"]

    def error(self, command: str, request: dict[str, Any] | None = None) -> dict[str, Any]:
        reply = self.call(command, request)
        assert reply["ok"] is False, reply
        return reply["error"]


@contextmanager
def _served(runtime: Any, tmp_path: Path) -> Iterator[_Client]:
    root = tmp_path / "sockets"
    root.mkdir(mode=0o700)
    application = ProgramRunnerApplication(runtime, root)
    application.start()
    try:
        yield _Client(root)
    finally:
        application.close()


def _run(program_run_id: int, body: dict[str, Any]) -> dict[str, Any]:
    return {"program_run_id": program_run_id, "body": body}


_START_BODY = {
    "scenario_id": "cell-pick",
    "source_document_digest": _DIGEST_A,
    "expected_applied_record_digest": _DIGEST_B,
    "draft_program_revision": _DIGEST_B,
}


def test_node_availability_is_a_read_only_runner_projection(tmp_path) -> None:
    runtime = FakeRuntime()
    body = {
        "applied": {"program_revision": "sha256:" + "a" * 64},
    }
    with _served(runtime, tmp_path) as client:
        value = client.value("node_availability", body)

    assert value["schema_version"] == "manyforge.program_node_availability.v2"
    assert runtime.calls[0][0] == "node_availability"
    assert runtime.calls[0][1].model_dump() == body


def test_reset_is_an_explicit_strict_cycle_transition(tmp_path) -> None:
    runtime = FakeRuntime()
    body = {
        **_caller(),
        "program_run_id": 1,
        "expected_cycle_generation": 4,
        "expected_claim_generation": 7,
        "terminal_reference": _DIGEST_A,
        "settlement_evidence_digest": _DIGEST_B,
    }
    with _served(runtime, tmp_path) as client:
        # Answered at acceptance (OD-214): the reply is the command's row.
        assert client.value("reset_cycle", body) == _reply("reset")
        assert client.error("reset_cycle", {**body, "unexpected": True}) == _MALFORMED
    assert runtime.calls[0][0] == "reset"
    assert len(runtime.calls) == 1


def test_cycle_status_exposes_only_program_runner_owned_facts(tmp_path) -> None:
    runtime = FakeRuntime()
    with _served(runtime, tmp_path) as client:
        value = client.value("cycle_status")
        unknown = client.error("program_cycle_status")
        members = client.error("cycle_status", {"extra": 1})
    assert set(value) == {
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
    assert unknown == members == _MALFORMED
    # The status publisher's observer ends with the application.
    assert runtime.control_state_observers == set()


def test_control_state_stream_is_change_driven(tmp_path) -> None:
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
    with _served(runtime, tmp_path) as client:
        subscription = client.link.subscribe_status()
        first = subscription.next(5.0)
        assert first["schema_version"] == "manyforge.program_control_state.v4"
        assert first["event_class"] == "critical"
        assert first["server_sequence"] == "1"
        assert first["program_cycle"]["execution_control_revision"] == 1
        assert len(runtime.control_state_observers) == 1

        runtime.publish_control_state()
        second = subscription.next(5.0)
        assert second["server_sequence"] == "2"
        assert second["program_cycle"]["execution_control_revision"] == 2
        assert second["program_cycle"]["node_status_revision"] == 2
        assert second["event_class"] == "critical"
        runtime.publish_control_state(node_only=True)
        progress = subscription.next(5.0)
        assert progress["event_class"] == "node_progress"
        for state in ("stopping", "held", "running"):
            runtime.group_state = state
            runtime.publish_control_state(node_only=True)
            change = subscription.next(5.0)
            assert change["event_class"] == "critical"
            assert change["program_cycle"]["execution_control_revision"] == 2
        subscription.close()
    assert runtime.control_state_observers == set()


class _Publisher:
    """The core publisher's Python surface: values out, read requests in."""

    def __init__(self) -> None:
        self.published: queue.Queue[tuple[dict[str, Any], bool]] = queue.Queue()
        self.withdrawn: queue.Queue[tuple[str, str]] = queue.Queue()
        self._requested = threading.Event()

    def publish(self, cycle: dict[str, Any], critical: bool) -> None:
        self.published.put((cycle, critical))

    def withdraw(self, code: str, message: str) -> None:
        self.withdrawn.put((code, message))

    def request_read(self) -> None:
        self._requested.set()

    def wait_read_request(self, timeout_s: float | None = None) -> bool:
        if not self._requested.wait(timeout_s):
            return False
        self._requested.clear()
        return True


def test_control_state_frame_reads_reset_position_once_at_publication() -> None:
    """Classification snapshots skip the host Reset-position read; each
    published cycle reads it once and equals the complete cycle status."""

    runtime = FakeRuntime()
    publisher = _Publisher()
    # A still clock keeps the node-progress window open, so the publisher
    # always holds the node-only wake for its whole period.
    pump = _ProgramStatusPump(runtime, publisher, clock_ns=lambda: 1)
    pump.start()
    try:
        first, critical = publisher.published.get(timeout=5.0)
        assert critical is True
        assert (runtime.projections, runtime.reset_reads) == (1, 1)
        runtime.publish_control_state(node_only=True)
        progress, critical = publisher.published.get(timeout=5.0)
        assert critical is False
        # Before: one full status per snapshot (here 2 Reset reads per frame).
        assert (runtime.projections, runtime.reset_reads) == (3, 2)
        for cycle in (first, progress):
            assert cycle["reset_robot_position"] == {
                "available": False, "reason": "unsupported"}
        assert progress == runtime.cycle_status()
    finally:
        pump.close()
    assert runtime.control_state_observers == set()


def test_unreadable_control_state_withdraws_the_frame_until_readable() -> None:
    class FailingRuntime(FakeRuntime):
        fail = True

        def cycle_projection(self) -> dict[str, Any]:
            if self.fail:
                raise ProgramRunnerError("program_cycle_journal_invalid", "unreadable")
            return super().cycle_projection()

    runtime = FailingRuntime()
    publisher = _Publisher()
    pump = _ProgramStatusPump(runtime, publisher)
    pump.start()
    try:
        assert publisher.withdrawn.get(timeout=5.0) == (
            "program_cycle_journal_invalid", "unreadable")
        runtime.fail = False
        runtime.publish_control_state()
        _cycle, critical = publisher.published.get(timeout=5.0)
        assert critical is True
    finally:
        pump.close()


def test_a_reconnecting_subscriber_reads_a_withdrawn_state_again(tmp_path) -> None:
    """A withdrawn state is not kept for later subscribers: each new one
    makes the runner read the state again (as each new connection did)."""

    class RecoveringRuntime(FakeRuntime):
        fail = True

        def cycle_projection(self) -> dict[str, Any]:
            if self.fail:
                raise ProgramRunnerError("program_cycle_journal_invalid", "unreadable")
            return super().cycle_projection()

    runtime = RecoveringRuntime()
    with _served(runtime, tmp_path) as client:
        first = client.link.subscribe_status()
        with pytest.raises(core.GatewayError) as withdrawn:
            first.next(10.0)
        assert withdrawn.value.code == "program_cycle_journal_invalid"
        first.close()
        # Readable again without any runtime wake: only the reconnect reads it.
        runtime.fail = False
        second = client.link.subscribe_status()
        frame = second.next(10.0)
        assert frame["event_class"] == "critical"
        assert frame["server_sequence"] == "1"
        assert frame["program_cycle"] == runtime.cycle_status()
        second.close()


def test_a_second_runner_on_the_same_sockets_is_refused_by_code(tmp_path) -> None:
    """Each socket has one server; a second runner's start is refused with a
    typed error, and the first keeps serving."""

    runtime = FakeRuntime()
    with _served(runtime, tmp_path) as client:
        second = ProgramRunnerApplication(FakeRuntime(), client.root)
        with pytest.raises(ProgramRunnerError) as refused:
            second.start()
        second.close()
        assert refused.value.code == "program_runner_socket_unavailable"
        assert "another Program runner" in str(refused.value)
        assert client.value("health")["started"] is True


def test_private_surface_projects_owner_facts_and_manages_lifecycle(tmp_path) -> None:
    runtime = FakeRuntime()
    with _served(runtime, tmp_path) as client:
        health = client.value("health")
        index = client.value("runs", {"active_only": True, "limit": 7})
        started = client.value("start_run", {**_caller(), **_START_BODY})
        prepared = client.value(
            "prepare_applied_scene",
            {
                **_caller(),
                "request_key": _key("request.prepare.1"),
                "expected_applied_record_digest": _DIGEST_B,
                "expected_admission_generation": 7,
            },
        )
        read = client.value("read_run", {"program_run_id": 1})
        motion_started = client.value(
            "start_motion",
            _run(1, {
                **_caller(),
                "request_key": _key("request.motion-start.1"),
                "program_run_id": 1,
                "expected_hold_revision": 0,
                "group_ids": ["robot_group"],
                "reason": "initial operator motion Run",
            }),
        )
        resumed = client.value(
            "resume_halt",
            _run(1, {
                **_caller(),
                "request_key": _key("request.resume-halt.1"),
                "program_run_id": 1,
                "expected_run_revision": 1,
                "reason": "operator Execute after Halt",
            }),
        )
        # Kernel-first: the runner has no Stop command; every client Stop
        # goes Headless -> Kernel.
        removed_stops = [
            client.error(command, _run(1, {}))
            for command in (
                "stop", "motion_stop", "full_stop", "discard_full_stop",
                "resume_full_stop",
            )
        ]

    assert prepared["status"] == "prepared"
    assert health["started"] is True
    assert index["runs"][0]["capture"]["program_run_id"] == 1
    assert read["capture"]["program_run_id"] == 1
    assert started == _reply("start")
    assert motion_started == _reply("start_motion", _key("request.motion-start.1"))
    assert resumed == _reply("resume_halt", _key("request.resume-halt.1"))
    assert removed_stops == [_MALFORMED] * 5
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
    # The sockets go with the application.
    assert not (client.root / "program-command.sock").exists()
    assert not (client.root / "program-status.sock").exists()


def test_private_surface_rejects_unknown_fields_and_path_body_mismatch(tmp_path) -> None:
    runtime = FakeRuntime()
    body = {
        **_caller(),
        "program_run_id": 2,
        "expected_hold_revision": 0,
        "group_ids": ["robot_group"],
        "reason": None,
    }
    with _served(runtime, tmp_path) as client:
        mismatch = client.error("start_motion", _run(1, body))
        # Run numbers are integers >= 1 (1.15.3).
        textual = client.error("read_run", {"program_run_id": "program-run.1"})
        zero = client.error("read_run", {"program_run_id": 0})
        unknown = client.error(
            "start_run", {**_caller(), **_START_BODY, "invented": True}
        )
        limit = client.error("runs", {"limit": 4_097})

    assert mismatch["code"] == "program_run_path_invalid"
    assert mismatch["message"] == "Program-run path and body must match"
    assert textual == zero == unknown == limit == _MALFORMED
    assert runtime.calls == []


def test_private_surface_preserves_stable_owner_error_codes(tmp_path) -> None:
    class FailingRuntime(FakeRuntime):
        def read_run(self, program_run_id: int) -> dict[str, Any]:
            raise ProgramRunnerError(
                "program_run_not_found",
                f"unknown Program run {program_run_id}",
                producer="kernel",
            )

    with _served(FailingRuntime(), tmp_path) as client:
        error = client.error("read_run", {"program_run_id": 7})

    assert error == {
        "code": "program_run_not_found",
        "details": {},
        "message": "unknown Program run 7",
        "producer": "kernel",
    }


def test_an_unexpected_runner_failure_is_an_invalid_answer(tmp_path) -> None:
    class BrokenRuntime(FakeRuntime):
        def read_run(self, program_run_id: int) -> dict[str, Any]:
            raise KeyError("bug")

        def read_runtime_controls(self, program_run_id: int) -> dict[str, Any]:
            return {"not_json": object()}

    with _served(BrokenRuntime(), tmp_path) as client:
        errors = [
            client.error("read_run", {"program_run_id": 7}),
            client.error("read_runtime_controls", {"program_run_id": 7}),
        ]
    assert errors == [{
        "code": "program_runner_response_invalid",
        "details": {},
        "message": "Program runner returned an invalid response",
        "producer": "program_runner",
    }] * 2


def test_a_full_ordinary_lane_refuses_at_once(tmp_path) -> None:
    release = threading.Event()
    entered = threading.Semaphore(0)

    class SlowRuntime(FakeRuntime):
        def reconcile_persistent_values(self) -> dict[str, Any]:
            entered.release()
            release.wait()
            return {"outcome": "duplicate"}

    with _served(SlowRuntime(), tmp_path) as client:
        replies: list[dict[str, Any]] = []
        callers = [
            threading.Thread(
                target=lambda: replies.append(client.call("reconcile_persistent_values"))
            )
            for _ in range(2)
        ]
        for caller in callers:
            caller.start()
        assert entered.acquire(timeout=5.0) and entered.acquire(timeout=5.0)
        busy = client.error("reconcile_persistent_values")
        # Reads keep being served while the lane is full.
        assert client.value("health")["started"] is True
        release.set()
        for caller in callers:
            caller.join()
    assert busy == {
        "code": "execution_control_ingress_busy",
        "details": {"lane": "ordinary", "maximum_admitted": 2},
        "message": "execution-control ordinary lane is at its admission limit (2)",
        "producer": "program_runner",
    }
    assert replies == [{"ok": True, "value": {"outcome": "duplicate"}}] * 2


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
def test_program_start_preconditions_keep_their_codes(tmp_path, code: str) -> None:
    class RejectingRuntime(FakeRuntime):
        def start_run(self, request: ProgramRunStartRequest) -> dict[str, Any]:
            raise ProgramRunnerError(code, "start precondition rejected")

    with _served(RejectingRuntime(), tmp_path) as client:
        error = client.error("start_run", {**_caller(), **_START_BODY})
    assert error["code"] == code


def test_persistent_value_routes_are_applied_project_strict_requests(tmp_path) -> None:
    runtime = FakeRuntime()
    project = "/projects/cell-pick.scenario"
    edit = {
        **_caller(),
        "project_path": project,
        "key": "layer",
        "expected_program_revision": _DIGEST_A,
    }
    with _served(runtime, tmp_path) as client:
        assert client.value("read_persistent_values") == {"project_path": project}
        assert client.value("set_persistent_value", {**edit, "value": 3}) == _reply(
            "persistent_value_set"
        )
        assert client.value("reset_persistent_value", edit) == _reply(
            "persistent_value_reset"
        )
        cleared = client.value(
            "clear_persistent_values", {**_caller(), "project_path": project}
        )
        assert cleared == _reply("persistent_values_clear")
        # The project folder is absolute and lexically normalized.
        for path in (
            "projects/cell-pick.scenario",
            "/projects/../cell-pick.scenario",
            "/projects/cell-pick.scenario/",
            "//projects/cell-pick.scenario",
            "/projects/\x00cell-pick",
            "/" + "p" * 4096,
        ):
            assert client.error(
                "clear_persistent_values", {**_caller(), "project_path": path}
            ) == _MALFORMED
        assert client.error("clear_persistent_values", _caller()) == _MALFORMED
        # A value is required to set, and nothing else is accepted.
        assert client.error("set_persistent_value", edit) == _MALFORMED
        assert client.error("reset_persistent_value", {**edit, "value": 3}) == _MALFORMED
        # A live run is named with its revision; the register sequence is no
        # longer a fence (without a run the last admitted edit wins).
        assert client.error(
            "reset_persistent_value", {**edit, "program_run_id": 1}
        ) == _MALFORMED
        assert client.error(
            "reset_persistent_value", {**edit, "expected_sequence": 4}
        ) == _MALFORMED
        assert client.value(
            "reset_persistent_value",
            {**edit, "program_run_id": 1, "expected_run_revision": 2},
        ) == _reply("persistent_value_reset")
        assert client.error(
            "clear_persistent_values",
            {**_caller(), "project_path": project, "expected_sequence": 4},
        ) == _MALFORMED
        # Headless's wake after a committed Apply carries nothing to trust.
        assert client.value("reconcile_persistent_values") == {"outcome": "duplicate"}

    assert [name for name, _values in runtime.calls] == [
        "read_persistent_values",
        "set_persistent_value",
        "reset_persistent_value",
        "clear_persistent_values",
        "reset_persistent_value",
        "reconcile_persistent_values",
    ]
    assert runtime.calls[1][1].value == 3


def test_runtime_override_commands_are_removed_and_key_writes_remain() -> None:
    assert not any("override" in command for command in PROGRAM_RUNNER_COMMANDS)
    assert {
        "read_runtime_blackboard",
        "write_runtime_blackboard",
        "read_runtime_controls",
    } <= PROGRAM_RUNNER_COMMANDS


def test_key_values_read_is_a_plain_runner_query(tmp_path) -> None:
    runtime = FakeRuntime()
    with _served(runtime, tmp_path) as client:
        value = client.value("read_runtime_blackboard", {"program_run_id": 3})
        invalid = client.error("read_runtime_blackboard", {"program_run_id": 0})

    assert value == {"program_run_id": 3, "keys": []}
    assert invalid == _MALFORMED
    assert ("read_runtime_blackboard", 3) in runtime.calls


def test_node_correction_preview_is_a_plain_runner_query(tmp_path) -> None:
    runtime = FakeRuntime()
    body = {
        "schema_version": "manyforge.program_node_correction_preview_request.v2",
        "request_key": _key("node-preview.1"),
        "program_run_id": 1,
        "program_revision": "sha256:" + "a" * 64,
        "program_binding_id": "sha256:" + "a" * 64,
        "node_instance_id": "move",
        "node_attempt_id": 1,
        "proposed_inputs": [{"path": "goal", "value": []}],
    }

    with _served(runtime, tmp_path) as client:
        client.value("preview_node_correction", _run(1, body))
        foreign = client.error(
            "preview_node_correction", _run(1, {**body, "target_role_id": "other"})
        )

    assert foreign == _MALFORMED
    assert [kind for kind, _value in runtime.calls] == ["node_preview"]
    assert runtime.calls[0][1][1].model_dump(mode="json") == body


def test_run_contract_removes_client_generation_and_rejects_old_wire_field(tmp_path):
    runtime = FakeRuntime()
    body = {**_caller(), **_START_BODY}
    with _served(runtime, tmp_path) as client:
        client.value("start_run", body)
        legacy = client.error("start_run", {**body, "expected_admission_generation": 36})
        assert legacy == _MALFORMED
    schema = ProgramRunStartRequest.model_json_schema()
    assert "expected_admission_generation" not in schema["properties"]
    assert "expected_admission_generation" not in schema["required"]
    assert len([call for call in runtime.calls if call[0] == "start"]) == 1


@pytest.mark.parametrize("code", [
    "program_run_started_then_failed",
    "authoring_not_ready",
    "program_cycle_reconciliation_required",
    "program_runner_internal_error",
])
def test_started_then_failed_response_preserves_actual_identity_and_native_details(
    tmp_path, code: str,
) -> None:
    details = {"program_run_id": 2, "claim_generation": 9, "needs_reset": True,
        "cause_code": "component_lifecycle_facts_stale", "admission_changed": "true",
        "admission_generation_before": "36", "admission_generation_after": "37"}
    class FailedStart(FakeRuntime):
        def start_run(self, _request):
            raise ProgramRunnerError(code, "Owner outcome", details=details)
    with _served(FailedStart(), tmp_path) as client:
        error = client.error("start_run", {**_caller(), **_START_BODY,
            "request_key": _key("run.failed.1"), "draft_program_revision": _DIGEST_A})
    assert error == {"code": code, "message": "Owner outcome", "details": details,
        "producer": "program_runner"}


def test_a_forwarded_request_without_its_key_keeps_its_code(tmp_path) -> None:
    """1.15.1: the runner refuses a request Headless forwarded without its key
    with ``request_key_required``, an internal contract violation."""

    class KeylessRuntime(FakeRuntime):
        def start_run(self, request):
            raise ProgramRunnerError(
                "request_key_required", "the forwarded request carries no request key"
            )

    with _served(KeylessRuntime(), tmp_path) as client:
        error = client.error(
            "start_run", {"source_label": "operator_ui", **_START_BODY}
        )
    assert error["code"] == "request_key_required"


def test_startup_collects_once_after_start_and_before_serving(monkeypatch, tmp_path) -> None:
    runtime = FakeRuntime()
    settles: list[tuple[str, bool, int, bool]] = []
    monkeypatch.setattr(
        "manyforge_program_runner.app.settle_heap",
        lambda point: settles.append((
            point, runtime.started, len(runtime.calls),
            (tmp_path / "sockets" / "program-command.sock").exists(),
        )),
    )
    with _served(runtime, tmp_path) as client:
        assert settles == [("start", True, 0, False)]
        assert client.value("health")["started"] is True
    assert settles == [("start", True, 0, False)]
