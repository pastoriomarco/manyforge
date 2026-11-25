"""History rows of operator commands and manual output writes, and the
caller every record may name (activity_history)."""

from __future__ import annotations

import asyncio
from typing import Any

from fastapi.testclient import TestClient

from manyforge_headless_api.activity_history import (
    COMMAND_REPEAT_WINDOW_NS,
    MAXIMUM_COUNTED_REFUSALS,
    SCHEMA_VERSION,
    CommandHistory,
    bind_request_caller,
    caller,
    command_finished_record,
    component_action_record,
    io_output_record,
    unbind_request_caller,
    with_caller,
)
from manyforge_headless_api.app import create_app
from manyforge_headless_api.control_protocol import (
    ControlFreshnessAuthority,
    ExecutionIngressResult,
)
from manyforge_headless_api.control_session import ExecutionControlSession
from manyforge_headless_api.execution_control import ExecutionControlIngress
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.tests.test_app import (
    FakeCore,
    FakeExecution,
    FakeManager,
    FakeProgramRunner,
    _caller,
    _service,
)
from manyforge_headless_api.tests.test_component_model import _ModelExecution, _reply
from manyforge_headless_api.tests.test_control_session import (
    APPLICATION_START_ID,
    _CommandSocket,
    _frame,
    _Source,
)


class _Core(FakeCore):
    """Keeps every activity record it is handed."""

    def __init__(self) -> None:
        super().__init__()
        self.activity: list[dict[str, Any]] = []

    def record_activity(self, record: dict[str, Any]) -> dict[str, Any]:
        self.activity.append(record)
        return {"recorded": True}


class _Clock:
    def __init__(self) -> None:
        self.now = 1_000

    def __call__(self) -> int:
        return self.now


def _commands(core: _Core) -> list[dict[str, Any]]:
    return [record for record in core.activity if record["kind"] == "command_finished"]


# Records ---------------------------------------------------------------------


def test_a_refused_command_row_carries_its_typed_error_facts_and_line() -> None:
    record = command_finished_record(
        "execute",
        "refused",
        {"code": "control_freshness_expired", "message": "expired",
         "details": {"age_ns": 3, "valid_for_ns": 2}},
        {"repeat_count": 3, "program_run_id": 7, "command_kind": "program_run",
         "operation_id": None},
    )
    assert record == {
        "schema_version": SCHEMA_VERSION,
        "kind": "command_finished",
        "subject": "execute",
        "action": None,
        "outcome": "refused",
        "error": {"code": "control_freshness_expired", "message": "expired",
                  "details": {"age_ns": 3, "valid_for_ns": 2}},
        "components": [],
        "message": "Execute refused: control_freshness_expired: expired · run 7 · 3 times",
        # Unknown facts are left out.
        "command": {"repeat_count": 3, "program_run_id": 7, "command_kind": "program_run"},
    }


def test_an_accepted_command_row_has_no_error() -> None:
    record = command_finished_record(
        "motion_start", "accepted", None, {"repeat_count": 1}
    )
    assert record["error"] is None
    assert record["message"] == "Motion start accepted"


def test_an_output_write_row_names_output_value_origin_and_time() -> None:
    record = io_output_record(
        "robot/DO3", "succeeded", None,
        {"value": True, "origin": "manual", "duration_ms": 4.2},
    )
    assert record == {
        "schema_version": SCHEMA_VERSION,
        "kind": "io_output_written",
        "subject": "robot/DO3",
        "action": None,
        "outcome": "succeeded",
        "error": None,
        "components": [],
        "message": "Output robot/DO3 set to true succeeded · 4 ms",
        "io_output": {"value": True, "origin": "manual", "duration_ms": 4.2},
    }
    refused = io_output_record(
        "robot/DO3", "refused", {"code": "cell_latched", "message": "latched"},
        {"value": 0.5, "origin": "manual", "duration_ms": 1.0},
    )
    assert refused["error"] == {"code": "cell_latched", "message": "latched"}
    assert refused["message"] == (
        "Output robot/DO3 set to 0.5 refused: cell_latched: latched · 1 ms"
    )


def test_a_record_names_the_request_it_was_built_in_and_nothing_outside_one() -> None:
    assert caller() is None
    record = component_action_record("robot", "enable", "succeeded")
    assert "caller" not in with_caller(dict(record))
    token = bind_request_caller("req.1", "mcp")
    try:
        assert caller(source_label="operator_ui", principal_id=None, request_key="") == {
            "request_id": "req.1", "transport": "mcp", "source_label": "operator_ui",
        }
        assert with_caller(dict(record))["caller"] == {
            "request_id": "req.1", "transport": "mcp",
        }
        # Each member is bounded to 256 bytes.
        assert len(caller(source_label="é" * 200)["source_label"].encode()) <= 256
    finally:
        unbind_request_caller(token)
    assert caller() is None


# Coalescing ------------------------------------------------------------------


def _refuse(history: CommandHistory, *, source: str = "operator_ui",
            code: str = "control_freshness_expired") -> bool:
    return history.finished(
        "execute", "refused", {"code": code, "message": "m"}, {},
        {"transport": "http", "source_label": source},
    )


def test_identical_refusals_are_counted_into_the_next_row() -> None:
    core, clock = _Core(), _Clock()
    history = CommandHistory(core, clock_ns=clock)

    assert _refuse(history) is True
    clock.now += 1
    assert _refuse(history) is False
    clock.now += 1
    assert _refuse(history) is False
    # Another source and another code are not identical.
    assert _refuse(history, source="assistant") is True
    assert _refuse(history, code="command_not_allowed") is True
    clock.now += COMMAND_REPEAT_WINDOW_NS
    assert _refuse(history) is True

    rows = _commands(core)
    assert [row["command"]["repeat_count"] for row in rows] == [1, 1, 1, 3]
    assert rows[0]["caller"] == {"transport": "http", "source_label": "operator_ui"}
    assert rows[3]["message"].endswith("· 3 times")


def test_another_answer_ends_a_run_of_identical_refusals() -> None:
    core, clock = _Core(), _Clock()
    history = CommandHistory(core, clock_ns=clock)
    _refuse(history)
    _refuse(history)
    history.finished("execute", "accepted", None, {}, {"transport": "http"})
    # Written at once, with the refusal counted before the acceptance.
    assert _refuse(history) is True
    assert [row["outcome"] for row in _commands(core)] == ["refused", "accepted", "refused"]
    assert [row["command"]["repeat_count"] for row in _commands(core)] == [1, 1, 2]


def test_the_counted_refusal_runs_are_bounded() -> None:
    core, clock = _Core(), _Clock()
    history = CommandHistory(core, clock_ns=clock)
    for index in range(MAXIMUM_COUNTED_REFUSALS + 5):
        _refuse(history, source=f"source.{index}")
    assert len(history._refusals) == MAXIMUM_COUNTED_REFUSALS
    # The oldest run was forgotten: its next refusal is written at once.
    assert _refuse(history, source="source.0") is True


# Headless routes ---------------------------------------------------------------


def _start_body() -> dict[str, Any]:
    return {
        **_caller(),
        "scenario_id": "cell-pick",
        "source_document_digest": "sha256:" + "6" * 64,
        "expected_applied_record_digest": "sha256:" + "5" * 64,
        "draft_program_revision": "sha256:" + "7" * 64,
    }


class _NonExecutableCore(_Core):
    def applied_program_selection(self) -> dict[str, Any]:
        raise HeadlessApiError(
            "program_applied_non_executable",
            "Applied Program is not executable in the active Deployment",
            details={"target_role": "gripper"},
        )


def test_a_refused_execute_is_one_row_with_its_caller_and_typed_details() -> None:
    core = _NonExecutableCore()
    service = _service(core, FakeManager(), FakeExecution(), program_runner=FakeProgramRunner())
    with TestClient(create_app(service=service)) as client:
        refused = client.post(
            "/api/program/runs", json=_start_body(), headers={"X-Request-ID": "composer.7"}
        )
        again = client.post(
            "/api/program/runs", json=_start_body(),
            headers={"X-Request-ID": "assistant.1", "X-ManyForge-Transport": "mcp"},
        )

    assert refused.status_code == 409, refused.text
    assert again.status_code == 409, again.text
    rows = _commands(core)
    assert len(rows) == 2
    row = rows[0]
    assert row["subject"] == "execute"
    assert row["outcome"] == "refused"
    assert row["error"] == {
        "code": "program_applied_non_executable",
        "message": "Applied Program is not executable in the active Deployment",
        "details": {"target_role": "gripper"},
    }
    assert row["command"] == {
        "command_kind": "program_run", "disposition": "rejected", "repeat_count": 1,
    }
    assert row["caller"] == {
        "request_id": "composer.7",
        "request_key": "c0" + _caller()["request_key"][2:],
        "transport": "http",
        "source_label": "operator_ui",
    }
    # The MCP adapter's mark names another transport: not an identical refusal.
    assert rows[1]["caller"]["transport"] == "mcp"
    assert rows[1]["caller"]["request_id"] == "assistant.1"


def test_an_accepted_execute_is_recorded_accepted() -> None:
    core = _Core()
    service = _service(core, FakeManager(), FakeExecution(), program_runner=FakeProgramRunner())
    calls: list[Any] = []

    def program_run_start(request: Any) -> dict[str, Any]:
        calls.append(request)
        return {"schema_version": "manyforge.program_command_reply.v1", "command": {
            "kind": "start", "request_key": request.request_key, "step": None,
            "accepted_at_ns": "1", "end": None}}

    service.program_run_start = program_run_start  # type: ignore[method-assign]
    with TestClient(create_app(service=service)) as client:
        accepted = client.post("/api/program/runs", json=_start_body())
    assert accepted.status_code == 200, accepted.text
    [row] = _commands(core)
    assert (row["subject"], row["outcome"], row["error"]) == ("execute", "accepted", None)
    assert row["command"]["receipt_code"] == "program_run_received"


def _stop_body() -> dict[str, Any]:
    return {
        "application_start_id": APPLICATION_START_ID,
        "request_context": {"request_key": "b3" * 16, "source_label": "operator_ui"},
        "operations": [{"operation_id": 7, "expected_operation_revision": 1}],
        "reason": None,
    }


def test_a_refused_stop_is_recorded_and_an_accepted_one_is_not() -> None:
    class RefusingStop(FakeExecution):
        refuse = False

        def stop(self, request: object, *, trusted_principal_id: str | None) -> dict[str, Any]:
            super().stop(request, trusted_principal_id=trusted_principal_id)
            if self.refuse:
                return {"ok": False, "error": {
                    "code": "execution_operation_revision_stale", "message": "stale"}}
            return {"ok": True, "command": "stop", "accepted": {"event_id": 3}}

    core = _Core()
    execution = RefusingStop()
    with TestClient(create_app(service=_service(core, FakeManager(), execution))) as client:
        accepted = client.post("/api/execution/stops", json=_stop_body())
        execution.refuse = True
        refused = client.post("/api/execution/stops", json=_stop_body())

    assert accepted.status_code == 200, accepted.text
    assert refused.json()["ok"] is False, refused.text
    [row] = _commands(core)
    assert row["subject"] == "stop"
    assert row["outcome"] == "refused"
    assert row["error"] == {"code": "execution_operation_revision_stale", "message": "stale"}
    assert row["caller"]["request_key"] == "c0" + "b3" * 15


def test_a_manual_output_write_is_one_row_with_its_caller() -> None:
    core = _Core()
    execution = _ModelExecution(_reply())
    service = _service(core, FakeManager(), execution,
                       program_runner=FakeProgramRunner(active=False))
    with TestClient(create_app(service=service)) as client:
        client.post("/api/io/robot%2FCO0/set", json={"value": True},
                    headers={"X-Request-ID": "composer.9"})
        execution.refusal = "cell_latched"
        client.post("/api/io/robot/CO0/set", json={"value": False})

    rows = [record for record in core.activity if record["kind"] == "io_output_written"]
    assert [(row["subject"], row["outcome"]) for row in rows] == [
        ("robot/CO0", "succeeded"), ("robot/CO0", "refused"),
    ]
    assert rows[0]["io_output"]["value"] is True
    assert rows[0]["io_output"]["origin"] == "manual"
    assert rows[0]["io_output"]["duration_ms"] >= 0
    assert rows[0]["caller"] == {"request_id": "composer.9", "transport": "http"}
    assert rows[1]["error"] == {"code": "cell_latched", "message": "refused"}
    # An output write has its own row, not a command row.
    assert _commands(core) == []


def test_a_refused_alarm_acknowledgement_is_recorded() -> None:
    core = _Core()

    def reject(**_kwargs: Any) -> dict[str, Any]:
        raise HeadlessApiError("operator_alarm_record_revision_stale", "stale")

    core.acknowledge_alarm = reject  # type: ignore[method-assign]
    service = _service(core, FakeManager())
    condition = "condition-" + "a" * 24
    with TestClient(create_app(service=service)) as client:
        response = client.post(f"/api/execution/alarms/{condition}/acknowledge", json={
            **_caller(), "activation_id": "activation-" + "b" * 24,
            "expected_record_revision": 1, "confirmed": True,
        })
    assert response.status_code >= 400
    [row] = _commands(core)
    assert (row["subject"], row["outcome"]) == ("alarm_acknowledge", "refused")
    assert row["command"]["condition_id"] == condition
    assert row["caller"]["source_label"] == "operator_ui"


# Control session (WebSocket) ---------------------------------------------------


def _ws_ingress(disposition: str, code: str) -> ExecutionControlIngress:
    def dispatch(request, stamp):
        del request, stamp
        return (
            ExecutionIngressResult(
                disposition=disposition, code=code, execution_control_revision=4,
            ),
            {"ok": False, "error": {"code": code, "message": "not now"}}
            if disposition == "rejected" else None,
        )

    return ExecutionControlIngress(
        projection=lambda: 4,
        dispatch=dispatch,
        application_start_id=APPLICATION_START_ID,
        authority=ControlFreshnessAuthority(secret=b"x" * 32),
    )


def _serve_one_command(disposition: str, code: str) -> tuple[_Core, int]:
    core = _Core()
    ingress = _ws_ingress(disposition, code)
    frame = _frame(ingress)
    socket = _CommandSocket(frame)
    try:
        asyncio.run(ExecutionControlSession(
            socket, _Source(frame), ingress,  # type: ignore[arg-type]
            command_history=CommandHistory(core),
        ).serve())
    finally:
        ingress.close()
    assert socket.session_number is not None
    return core, socket.session_number


def test_a_websocket_command_is_recorded_at_its_receipt() -> None:
    core, session = _serve_one_command("received", "program_run_received")
    [row] = _commands(core)
    assert (row["subject"], row["outcome"]) == ("execute", "accepted")
    assert row["command"]["control_session_number"] == session
    assert row["command"]["client_sequence"] == "1"
    assert row["caller"] == {
        # Class 0: the session and its client sequence.
        "request_key": "00" + f"{session:014x}" + f"{1:016x}",
        "transport": "ws",
        "source_label": "test",
    }


def test_a_refused_websocket_command_keeps_the_owner_message() -> None:
    core, _session = _serve_one_command("rejected", "program_not_applied")
    [row] = _commands(core)
    assert row["outcome"] == "refused"
    assert row["error"] == {"code": "program_not_applied", "message": "not now"}


def test_a_stale_websocket_command_answers_its_typed_refusal_details() -> None:
    core = _Core()
    revision = [4]
    ingress = ExecutionControlIngress(
        projection=lambda: revision[0],
        dispatch=lambda request, stamp: (_ for _ in ()).throw(AssertionError("dispatched")),
        application_start_id=APPLICATION_START_ID,
        authority=ControlFreshnessAuthority(secret=b"x" * 32),
    )
    frame = _frame(ingress)
    revision[0] = 5
    socket = _CommandSocket(frame)
    try:
        asyncio.run(ExecutionControlSession(
            socket, _Source(frame), ingress,  # type: ignore[arg-type]
            command_history=CommandHistory(core),
        ).serve())
    finally:
        ingress.close()
    [receipt] = [m for m in socket.sent if m["message_kind"] == "command_receipt"]
    details = {
        "expected_execution_control_revision": 4,
        "current_execution_control_revision": 5,
    }
    assert receipt["receipt"]["code"] == "control_projection_stale"
    assert receipt["result"] == {
        "code": "control_projection_stale",
        "message": "control revision changed",
        "details": details,
    }
    [row] = _commands(core)
    assert row["error"]["details"] == details


# Runtime scale -----------------------------------------------------------------


def _scale(history: CommandHistory, scale: float, *, source: str = "operator_ui",
           transport: str = "ws") -> bool:
    return history.finished(
        "runtime_scale", "accepted", None,
        {"command_kind": "runtime_scale", "scale": scale},
        {"transport": transport, "source_label": source},
    )


def test_accepted_runtime_scale_changes_are_counted_per_source_and_transport() -> None:
    core, clock = _Core(), _Clock()
    history = CommandHistory(core, clock_ns=clock)

    assert _scale(history, 0.5) is True
    clock.now += 1
    assert _scale(history, 0.4) is False
    clock.now += 1
    assert _scale(history, 0.3) is False
    # Another source or transport is its own run.
    assert _scale(history, 0.9, source="assistant") is True
    assert _scale(history, 0.8, transport="http") is True
    # A refused change neither ends nor joins the run.
    assert history.finished(
        "runtime_scale", "refused", {"code": "control_freshness_expired"},
        {"command_kind": "runtime_scale", "scale": 0.1},
        {"transport": "ws", "source_label": "operator_ui"},
    ) is True
    clock.now += COMMAND_REPEAT_WINDOW_NS
    assert _scale(history, 0.2) is True

    rows = [row for row in _commands(core) if row["outcome"] == "accepted"]
    assert len(rows) == 4
    assert [(row["command"]["repeat_count"], row["command"]["scale"]) for row in rows] == [
        (1, 0.5), (1, 0.9), (1, 0.8), (3, 0.2),
    ]
    assert rows[3]["error"] is None
    assert rows[3]["message"] == "Runtime scale accepted · scale 0.2 · 3 times"


def test_an_accepted_websocket_runtime_scale_is_recorded_with_its_scale() -> None:
    from manyforge_headless_api.tests.test_control_session import _HandshakeSocket

    core = _Core()
    ingress = ExecutionControlIngress(
        projection=lambda: 4,
        dispatch=lambda request, stamp: (ExecutionIngressResult(
            disposition="received", code="runtime_scale_received",
            execution_control_revision=4,
        ), {"accepted": True}),
        application_start_id=APPLICATION_START_ID,
        authority=ControlFreshnessAuthority(secret=b"x" * 32),
    )
    frame = _frame(ingress)
    session = ExecutionControlSession(  # type: ignore[arg-type]
        _HandshakeSocket(), _Source(frame), ingress, command_history=CommandHistory(core)
    )
    control = frame.control

    def command(sequence: int, scale: float) -> Any:
        from manyforge_headless_api.control_protocol import (
            CONTROL_PROTOCOL_VERSION, EXECUTION_COMMAND_SCHEMA, ExecutionControlCommandDTO,
        )
        return ExecutionControlCommandDTO.model_validate({
            "schema_version": CONTROL_PROTOCOL_VERSION,
            "message_kind": "command",
            "control_session_number": session._session_number,  # noqa: SLF001
            "client_sequence": str(sequence),
            "command": {
                "command_envelope": {
                    "schema_version": EXECUTION_COMMAND_SCHEMA,
                    "command_kind": "runtime_scale",
                    "source_label": "operator_ui",
                    "control_session_number": session._session_number,  # noqa: SLF001
                    "client_sequence": str(sequence),
                    "freshness": control.freshness.model_dump(mode="json"),
                    "expected_execution_control_revision": control.execution_control_revision,
                },
                "payload": {"scale": scale},
            },
        })

    async def exercise() -> None:
        for sequence, scale in ((1, 0.5), (2, 0.25)):
            session._receive_command(command(sequence, scale))  # noqa: SLF001
            pending = session._runtime_scale_pending  # noqa: SLF001
            assert pending is not None
            await asyncio.wait_for(asyncio.shield(pending), 5)
            await asyncio.sleep(0)

    try:
        asyncio.run(exercise())
    finally:
        session._closed = True  # noqa: SLF001
        ingress.close()
    # The second change, within the window of the first row, is counted.
    [row] = _commands(core)
    assert (row["subject"], row["outcome"], row["error"]) == ("runtime_scale", "accepted", None)
    assert row["command"]["scale"] == 0.5
    assert row["command"]["repeat_count"] == 1
    assert row["caller"]["transport"] == "ws"


# Log lines -------------------------------------------------------------------


def test_a_repeated_reconciliation_failure_is_logged_once_per_run(caplog) -> None:
    from manyforge_headless_api.program_runner_client import ProgramRunnerClientError

    class Runner(FakeProgramRunner):
        failing = True

        def reconcile_persistent_values(self) -> dict[str, Any]:
            if self.failing:
                raise ProgramRunnerClientError(
                    "program_runner_unavailable", "independent Program runner is unavailable"
                )
            return {}

    runner = Runner()
    service = _service(_Core(), FakeManager(), program_runner=runner)

    def lines() -> int:
        return sum("did not reconcile persistent values" in r.getMessage() for r in caplog.records)

    with caplog.at_level("WARNING", logger="manyforge_headless_api.service"):
        service.notify_committed_apply()
        service.notify_committed_apply()
        assert lines() == 1
        runner.failing = False
        service.notify_committed_apply()
        runner.failing = True
        service.notify_committed_apply()
        assert lines() == 2


def test_a_repeated_scene_preparation_failure_is_logged_once_per_run(caplog) -> None:
    service = _service(_Core(), FakeManager(), program_runner=FakeProgramRunner())
    service.program_runner.reconcile_persistent_values = lambda: {}  # type: ignore[method-assign]
    failing = [True]

    def committed() -> dict[str, Any]:
        if failing[0]:
            raise HeadlessApiError("manager_unavailable", "Manager is unavailable")
        return {"apply_id": 1, "apply_generation": 1, "record_digest": "sha256:" + "1" * 64,
                "scene_initialization": "none"}

    service.committed_scenario_activation = committed  # type: ignore[method-assign]
    service._end_unstarted_empty_applies = lambda *args: None  # type: ignore[method-assign]  # noqa: SLF001

    def lines() -> int:
        return sum("Program scene preparation after committed Apply failed" in r.getMessage()
                   for r in caplog.records)

    with caplog.at_level("WARNING", logger="manyforge_headless_api.service"):
        service.notify_committed_apply()
        service.notify_committed_apply()
        assert lines() == 1
        failing[0] = False
        service.notify_committed_apply()
        failing[0] = True
        service.notify_committed_apply()
        assert lines() == 2


def test_every_control_command_kind_has_a_history_name() -> None:
    from typing import get_args

    from manyforge_headless_api.activity_history import CONTROL_COMMAND_NAMES
    from manyforge_headless_api.control_protocol import ExecutionCommandKind

    assert set(CONTROL_COMMAND_NAMES) == set(get_args(ExecutionCommandKind))
