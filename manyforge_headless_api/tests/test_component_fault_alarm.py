"""The event pump passes component faults to the alarm observer (OD-247)."""

from __future__ import annotations

import time
from typing import Any

from manyforge_headless_api.execution import ExecutionEventPump
from manyforge_headless_api.tests.test_component_model import _state
from manyforge_headless_api.tests.test_execution_event_stream import (
    _SubscribedExecution,
    _host_snapshot,
    _host_state,
    _message,
)
from manyforge_headless_api.tests.test_execution_events import _RecordingCore

_APPLICATION_START_ID = "3f9a0000000000a1"

_STALL = {
    "code": "component_owner_stalled",
    "message": "The component's owner stopped answering",
    "clearable": True,
    "operator_action": "none",
    "since_ns": 11,
}
_DRIVE = {
    "code": "fault_drive",
    "message": "Joint 2 drive fault",
    "clearable": False,
    "operator_action": "none",
    "vendor_code": "C12",
    "vendor_text": "Joint 2 overcurrent",
    "concerns": ["lite6_arm", "lite6_io"],
    "since_ns": 5,
    "since_utc_ns": "1795122990477663500",
    "details": {"output_tail": "driver: joint 2 overcurrent"},
}


class _ChangedCore(_RecordingCore):
    def observe_component_faults(self, faults_json: str) -> dict[str, Any]:
        super().observe_component_faults(faults_json)
        return {"status": "observed", "changed": True}


def _pump(core: _RecordingCore) -> ExecutionEventPump:
    return ExecutionEventPump(  # type: ignore[arg-type]
        core, _SubscribedExecution(), application_start_id=_APPLICATION_START_ID
    )


def test_faults_are_passed_without_their_times_details_and_simulators() -> None:
    core = _ChangedCore()
    pump = _pump(core)
    changes: list[None] = []
    pump.set_change_observer(lambda: changes.append(None))
    pump._observe_component_faults({"component_states": [  # noqa: SLF001
        _state("lite6", "fault", fault=_DRIVE),
        _state("gripper"),
        _state("isaac", "fault", fault=_STALL,
               capabilities=["sim_reset", "reset_position"]),
        _state("io", "fault", fault=_STALL),
    ]})
    assert core.component_faults == [[
        {"component_id": "lite6", "fault": {
            "code": "fault_drive",
            "message": "Joint 2 drive fault",
            "clearable": False,
            "operator_action": "none",
            "vendor_code": "C12",
            "vendor_text": "Joint 2 overcurrent",
            "concerns": ["lite6_arm", "lite6_io"],
        }},
        {"component_id": "io", "fault": {
            "code": "component_owner_stalled",
            "message": "The component's owner stopped answering",
            "clearable": True,
            "operator_action": "none",
        }},
    ]]
    assert changes == [None]  # A changed alarm wakes the control state.


def test_a_model_without_faults_is_passed_as_an_empty_list() -> None:
    core = _RecordingCore()
    pump = _pump(core)
    changes: list[None] = []
    pump.set_change_observer(lambda: changes.append(None))
    pump._observe_component_faults({"component_states": [_state("lite6")]})  # noqa: SLF001
    assert core.component_faults == [[]]
    assert changes == []  # Nothing changed: no wake-up.


def test_a_model_without_component_states_is_not_observed() -> None:
    # No host answered: nothing is known, so no alarm returns to normal.
    core = _RecordingCore()
    _pump(core)._observe_component_faults({"component_states": None})  # noqa: SLF001
    assert core.component_faults == []


def test_the_running_pump_observes_the_first_model_and_each_change() -> None:
    core = _RecordingCore()
    faulted = {**_host_state(), "component_states": [_state("lite6", "fault", fault=_STALL)]}
    execution = _SubscribedExecution(
        _message("snapshot", cursor=0, reason="subscribed", state=faulted)
    )
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, application_start_id=_APPLICATION_START_ID, poll_seconds=0.01
    )
    pump.start()
    try:
        deadline = time.monotonic() + 3.0
        while not core.component_faults and time.monotonic() < deadline:
            time.sleep(0.01)
        # The first model is observed too: a fault present at start is alarmed.
        assert [entry["component_id"] for entry in core.component_faults[0]] == ["lite6"]
        execution.script.append(_message("state", state={
            **_host_state(), "component_states": [_state("lite6")]}))
        deadline = time.monotonic() + 3.0
        while len(core.component_faults) < 2 and time.monotonic() < deadline:
            time.sleep(0.01)
    finally:
        pump.shutdown()
    assert pump.last_error is None
    assert core.component_faults == [
        [{"component_id": "lite6", "fault": {
            key: value for key, value in _STALL.items() if key != "since_ns"}}],
        [],
    ]


def test_the_pump_without_component_states_never_calls_the_observer() -> None:
    core = _RecordingCore()
    execution = _SubscribedExecution(_host_snapshot())
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, application_start_id=_APPLICATION_START_ID, poll_seconds=0.01
    )
    pump.start()
    try:
        assert core.completed.wait(3.0)
    finally:
        pump.shutdown()
    assert pump.last_error is None
    assert core.component_faults == []
