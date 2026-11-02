"""Component model v2 (CONTRACTS §1, §2, §5, §7, §10, §16): mirrors,
readiness, group-action order, the REST surface and the `/ws/control`
events."""

from __future__ import annotations

import threading
from typing import Any

import pytest
from fastapi.testclient import TestClient

from manyforge_headless_api.app import create_app
from manyforge_headless_api.component_model import (
    DEFAULT_LIFECYCLE_TIMEOUT_S,
    ComponentModel,
    action_wait_s,
    enable_all_targets,
    group_action_order,
    latch_summary,
    launch_settle_bound_s,
    lifecycle_timeouts_s,
    manual_operation_resident,
    parse_component_model,
    run_readiness,
    wait_for_action,
)
from manyforge_headless_api.component_overview import build_component_overview
from manyforge_headless_api.control_state import (
    _component_model_events,
    control_shell_fact_key,
)
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.tests.test_app import (
    FakeCore,
    FakeExecution,
    FakeManager,
    FakeProgramRunner,
    _service,
)


def _state(component_id: str, lifecycle: str = "enabled", **extra: Any) -> dict[str, Any]:
    return {
        "component_id": component_id,
        "lifecycle": lifecycle,
        "activity": "idle",
        "streams": [{"stream_id": "joints", "freshness": "fresh", "age_ns": 5}],
        "capabilities": ["reset_position"],
        "observed_at_ns": 10,
        **extra,
    }


_FAULT = {
    "code": "fault_estop",
    "message": "Emergency stop active",
    "clearable": True,
    "operator_action": "enable",
    "since_ns": 3,
}


def _reply(**overrides: Any) -> dict[str, Any]:
    return {
        "ok": True,
        "command": "read_component_model",
        "component_states": [_state("robot"), _state("gripper")],
        "cell_latch": {"latched": False, "causes": []},
        "signal_samples": [
            {"signal_id": "door", "value": True, "source": "device", "age_ns": 7, "fresh": True},
            {"signal_id": "CO0", "value": False, "source": "manyforge_write", "age_ns": 2,
             "fresh": True, "controller_owned": False},
        ],
        "rule_holds": [],
        "required_signal_ids": ["door"],
        **overrides,
    }


def _model(**overrides: Any) -> ComponentModel:
    return parse_component_model(_reply(**overrides))


# Mirrors and parse ---------------------------------------------------------


def test_parse_keeps_the_contract_field_names() -> None:
    model = _model()
    assert [row["component_id"] for row in model.component_states] == ["robot", "gripper"]
    assert model.cell_latch == {"latched": False, "causes": []}
    assert model.signal_samples[1]["controller_owned"] is False
    assert model.required_signal_ids == ["door"]


@pytest.mark.parametrize(
    "override",
    [
        {"cell_latch": {"causes": []}},
        {"cell_latch": {"latched": "no", "causes": []}},
        {"component_states": [{"component_id": "robot", "lifecycle": "active"}]},
        {"component_states": [{"lifecycle": "enabled"}]},
        {"signal_samples": None},
        {"rule_holds": [1]},
        {"execution_blocks": [1]},
        {"required_signal_ids": [3]},
        {"program_motion_component_ids": "robot"},
    ],
)
def test_parse_refuses_a_malformed_reply(override: dict[str, Any]) -> None:
    with pytest.raises(HeadlessApiError) as refused:
        parse_component_model(_reply(**override))
    assert refused.value.code == "execution_command_response_invalid"


def _machine() -> list[dict[str, Any]]:
    """Cell order: an owner-less tool, a member listed before its owner, the
    owner, its second member."""

    return [
        _state("gripper", "prepared"),
        _state("lite6_arm", "prepared", owner_id="lite6", role="arm"),
        _state("lite6", "prepared", capabilities=["power_off", "reboot"]),
        _state("lite6_controller_io", "prepared", owner_id="lite6", role="controller_io"),
    ]


def test_parse_keeps_the_adapters_details_and_refuses_non_text_pairs() -> None:
    details = [{"name": "mode", "value": "position"}, {"name": "state", "value": "ready"}]
    (robot,) = _model(component_states=[_state("robot", details=details)]).component_states
    assert robot["details"] == details
    for bad in ({"name": "mode"}, [{"name": "mode", "value": 1}], ["mode"]):
        with pytest.raises(HeadlessApiError):
            _model(component_states=[_state("robot", details=bad)])


def test_parse_keeps_owner_role_concerns_and_owner_unavailable() -> None:
    owner_fault = {**_FAULT, "concerns": ["lite6_arm", "lite6_controller_io"]}
    member = _state(
        "lite6_arm", "offline", owner_id="lite6", role="arm",
        reason={"code": "owner_unavailable", "message": "through lite6: rebooting"},
    )
    model = _model(component_states=[_state("lite6", "fault", fault=owner_fault), member,
                                     _state("gripper")])
    owner, arm, gripper = model.component_states
    assert owner["fault"]["concerns"] == ["lite6_arm", "lite6_controller_io"]
    assert "owner_id" not in owner and "role" not in owner
    assert (arm["owner_id"], arm["role"]) == ("lite6", "arm")
    assert arm["reason"] == {"code": "owner_unavailable", "message": "through lite6: rebooting"}
    assert "owner_id" not in gripper
    # The member's own reason travels into Execute's blocked reasons.
    blocked = _ready(model)["blocked_reasons"]
    assert {"code": "component_unhealthy", "component_id": "lite6_arm",
            "message": "lite6_arm is offline: through lite6: rebooting"} in blocked


@pytest.mark.parametrize("extra", [{"owner_id": 3}, {"role": None}])
def test_parse_refuses_owner_fields_that_are_not_text(extra: dict[str, Any]) -> None:
    with pytest.raises(HeadlessApiError) as refused:
        _model(component_states=[_state("lite6_arm", **extra)])
    assert refused.value.code == "execution_command_response_invalid"


def test_details_pass_through_as_read_only_text_rows() -> None:
    details = [{"name": "mode", "value": "1"}, {"name": "error code", "value": "C0"}]
    model = _model(component_states=[_state("lite6", "prepared", details=details)])
    assert model.component_states[0]["details"] == details
    events = _component_model_events(_Facts(model))  # type: ignore[arg-type]
    assert events["component_state"][0]["details"] == details


@pytest.mark.parametrize("details", [{"mode": "1"}, [{"name": "mode", "value": 1}]])
def test_parse_refuses_details_that_are_not_text_rows(details: Any) -> None:
    with pytest.raises(HeadlessApiError) as refused:
        _model(component_states=[_state("lite6", details=details)])
    assert refused.value.code == "execution_command_response_invalid"


def test_required_signal_ids_default_to_none() -> None:
    reply = _reply()
    del reply["required_signal_ids"]
    assert parse_component_model(reply).required_signal_ids == []


# Readiness (CONTRACTS §10) -------------------------------------------------


def _codes(readiness: dict[str, Any]) -> list[str]:
    return [reason["code"] for reason in readiness["blocked_reasons"]]


def _cycle(state: str = "idle_after_reset", *, resume: bool = False) -> dict[str, Any]:
    return {"state": state, "allowed_commands": [
        {"command_kind": "program_resume_full_stop", "allowed": resume}]}


def _ready(model: ComponentModel, state: str = "idle_after_reset", *,
           resume: bool = False, manual: bool = False) -> dict[str, Any]:
    return run_readiness(model, _cycle(state, resume=resume),
                         manual_operation_in_progress=manual)


def test_all_enabled_and_idle_cycle_can_execute() -> None:
    readiness = _ready(_model())
    assert readiness == {"can_execute": True, "can_resume": False, "motion_clear": True, "blocked_reasons": []}


def test_a_latched_cell_blocks_with_its_summary() -> None:
    latch = {"latched": True, "causes": [{"component_id": "robot", "kind": "fault", "fault": _FAULT}]}
    readiness = _ready(_model(cell_latch=latch))
    assert _codes(readiness) == ["cell_latched"]
    assert readiness["blocked_reasons"][0]["message"] == "robot: Emergency stop active"


def test_a_prepared_component_is_not_enabled_with_its_id() -> None:
    readiness = _ready(
        _model(component_states=[_state("robot", "prepared"), _state("gripper")])
    )
    assert readiness["can_execute"] is False
    assert readiness["blocked_reasons"] == [
        {"code": "component_not_enabled", "message": "robot is not enabled", "component_id": "robot"}
    ]


def test_a_simulation_that_is_not_playing_never_blocks_execute() -> None:
    """SIM-1: a simulated robot stays enabled with the reason, and the
    simulator itself (`prepared`, paused or stopped) doesn't block, so the
    Program's own nodes can play it."""

    not_playing = {"code": "simulation_not_playing", "message": "The simulation is not playing"}
    readiness = _ready(
        _model(
            component_states=[
                _state("isaac_robot", reason=not_playing,
                       streams=[{"stream_id": "measured_joints", "freshness": "stale"}]),
                _state("isaac_sim", "prepared",
                       reason={"code": "simulation_paused", "message": "The simulation is paused."}),
                _state("isaac_sim_2", "prepared",
                       reason={"code": "simulation_stopped", "message": "The simulation is stopped."}),
            ]
        )
    )
    assert readiness["can_execute"] is True
    assert readiness["blocked_reasons"] == []
    # Another prepared component still needs Enable.
    readiness = _ready(_model(component_states=[_state("robot", "prepared", reason=not_playing)]))
    assert _codes(readiness) == ["component_not_enabled"]


@pytest.mark.parametrize("lifecycle", ["fault", "offline"])
def test_an_unhealthy_component_names_itself(lifecycle: str) -> None:
    extra = {"fault": _FAULT} if lifecycle == "fault" else {
        "reason": {"code": "shutdown_by_operator", "message": "shut down by the operator"}}
    readiness = _ready(_model(component_states=[_state("robot", lifecycle, **extra)]))
    (reason,) = readiness["blocked_reasons"]
    assert reason["code"] == "component_unhealthy"
    assert reason["component_id"] == "robot"


def test_a_rule_hold_names_its_rule() -> None:
    hold = {"code": "rule_holds", "message": "door_open holds", "details": {"rule_id": "door_open"}}
    readiness = _ready(_model(rule_holds=[hold]))
    assert readiness["blocked_reasons"] == [
        {"code": "rule_holds", "message": "door_open holds", "rule_id": "door_open",
         "scope": "everything"}
    ]


def test_a_rule_hold_leaves_motion_clear_to_the_kernels_scoped_refusal() -> None:
    # §8: a hold gates only its own scope, so other groups may still Run/Resume.
    hold = {"code": "rule_holds", "message": "door_open holds", "details": {"rule_id": "door_open"}}
    readiness = _ready(_model(rule_holds=[hold]), "running", resume=True)
    assert readiness["can_execute"] is False
    assert readiness["can_resume"] is False
    assert readiness["motion_clear"] is True
    latch = {"latched": True, "causes": [{"component_id": "robot", "kind": "fault", "fault": _FAULT}]}
    assert _ready(_model(rule_holds=[hold], cell_latch=latch))["motion_clear"] is False


@pytest.mark.parametrize(
    ("details", "motion", "blocks"),
    [
        ({"scope": "everything"}, [], True),
        ({"scope": "all_motion"}, ["robot"], True),
        ({"scope": "all_motion"}, [], False),
        ({"scope": "execution_groups", "component_ids": "other,robot"}, ["robot"], True),
        ({"scope": "execution_groups", "component_ids": "other"}, ["robot"], False),
    ],
)
def test_a_rule_hold_blocks_only_when_its_scope_meets_the_program(
    details: dict[str, str], motion: list[str], blocks: bool
) -> None:
    hold = {"code": "rule_holds", "message": "held", "details": {"rule_id": "door", **details}}
    model = _model(rule_holds=[hold], program_motion_component_ids=motion)
    for cycle_state, key in (("idle_after_reset", "can_execute"), ("running", "can_resume")):
        readiness = _ready(model, cycle_state, resume=True)
        assert readiness[key] is not blocks
    # Every hold is shown (the GUI disables the groups it meets), with its scope.
    (reason,) = _ready(model)["blocked_reasons"]
    assert reason["code"] == "rule_holds"
    assert reason["scope"] == details["scope"]
    if details["scope"] == "execution_groups":
        assert reason["component_ids"] == details["component_ids"].split(",")
    else:
        assert "component_ids" not in reason


@pytest.mark.parametrize("samples", [[], [{"signal_id": "door", "value": None,
                                          "source": "device", "age_ns": 9, "fresh": False}]])
def test_a_stale_or_missing_required_input_names_its_signal(samples: list[dict[str, Any]]) -> None:
    readiness = _ready(_model(signal_samples=samples))
    assert readiness["blocked_reasons"] == [
        {"code": "input_not_fresh", "message": "door has no fresh value", "signal_id": "door"}
    ]


@pytest.mark.parametrize("cycle", ["starting", "stopping", "resetting",
                                   "terminal_awaiting_reset"])
def test_any_run_blocks_execute(cycle: str) -> None:
    readiness = _ready(_model(), cycle)
    assert _codes(readiness) == ["run_in_progress"]
    assert readiness["can_execute"] is False


@pytest.mark.parametrize("code", ["execution_host_internal_fault", "manager_unavailable"])
def test_an_execution_block_blocks_execute_and_resume_with_its_code(code: str) -> None:
    block = {"code": code, "message": "blocked", "details": {}}
    model = _model(execution_blocks=[block])
    assert model.execution_blocks == [block]
    assert _codes(_ready(model)) == [code]
    assert _ready(model, "running", resume=True)["can_resume"] is False
    # Lifts by itself once the host no longer reports it.
    assert _ready(_model(execution_blocks=[]))["can_execute"] is True


def test_before_the_first_component_report_after_a_restart_readiness_names_it() -> None:
    block = {"code": "execution_host_restart_awaiting_components", "message": "waits",
             "details": {}}
    readiness = _ready(_model(component_states=[], execution_blocks=[block]))
    assert readiness["can_execute"] is False
    assert readiness["blocked_reasons"] == [
        {"code": "execution_host_restart_awaiting_components", "message": "waits"}]


def test_a_restart_settling_block_names_its_busy_component() -> None:
    block = {"code": "execution_host_restart_settling", "message": "waits",
             "details": {"component_id": "robot"}}
    reasons = _ready(_model(execution_blocks=[block]))["blocked_reasons"]
    assert reasons == [{"code": "execution_host_restart_settling", "message": "waits",
                        "component_id": "robot"}]


def test_resume_needs_the_runners_resume_and_no_other_reason() -> None:
    assert _ready(_model(), "running", resume=True)["can_resume"] is True
    blocked = _ready(_model(component_states=[_state("robot", "prepared")]), "running",
                     resume=True)
    assert blocked["can_resume"] is False
    assert _ready(_model(), "terminal_awaiting_reset")["can_resume"] is False


def test_program_motion_running_blocks_resume_with_its_reason() -> None:
    readiness = _ready(_model(), "running")
    assert readiness["can_resume"] is False
    assert _codes(readiness) == ["run_in_progress", "program_motion_running"]


def test_an_unavailable_runner_blocks_execute_and_resume() -> None:
    readiness = run_readiness(_model(), None, manual_operation_in_progress=False)
    assert readiness["can_execute"] is False and readiness["can_resume"] is False
    assert _codes(readiness) == ["program_runner_unavailable"]


def test_a_resident_manual_operation_blocks_execute_and_resume() -> None:
    assert _codes(_ready(_model(), manual=True)) == ["manual_operation_in_progress"]
    assert _ready(_model(), "running", resume=True, manual=True)["can_resume"] is False


def test_manual_operation_resident_reads_the_control_state_rows() -> None:
    def control(*rows: dict[str, Any]) -> dict[str, Any]:
        return {"control_state": {"manual_operations": list(rows)}}

    assert manual_operation_resident(control({"resident": True})) is True
    assert manual_operation_resident(control({"resident": False})) is False
    assert manual_operation_resident(None) is False


def test_a_program_bound_to_a_component_outside_the_cell_blocks_execute() -> None:
    status = {
        "program_id": "pick", "executable": False,
        "message": "The Program can't run in this Deployment",
        "non_executable": {"unresolved_roles": [
            {"component_role_id": "picker", "controllable_component_id": "ur5e_robot"}]},
    }
    readiness = run_readiness(_model(), _cycle(), manual_operation_in_progress=False,
                              program_status=status)
    assert readiness["can_execute"] is False
    assert readiness["blocked_reasons"] == [{
        "code": "program_not_executable",
        "message": "The Program can't run in this Deployment (picker → ur5e_robot)"}]
    executable = {"program_id": "pick", "executable": True}
    assert _ready(_model())["can_execute"] is True
    assert run_readiness(_model(), _cycle(), manual_operation_in_progress=False,
                         program_status=executable)["can_execute"] is True


def test_latch_summary_counts_every_cause() -> None:
    latch = {"latched": True, "causes": [
        {"component_id": "robot", "kind": "fault", "fault": _FAULT},
        {"component_id": "io", "kind": "offline"},
    ]}
    assert latch_summary(latch) == "2 errors require attention · robot: Emergency stop active"
    assert latch_summary({"latched": True, "causes": [
        {"component_id": "io", "kind": "activity_unknown"}]}) == "io: activity unknown"


def test_enable_all_targets_prepared_components_in_cell_order() -> None:
    states = [_state("a", "prepared"), _state("b"), _state("c", "prepared"), _state("d", "fault")]
    assert enable_all_targets(states) == ["a", "c"]


def test_enable_all_skips_owners_which_have_no_enable() -> None:
    assert enable_all_targets(_machine()) == ["gripper", "lite6_arm", "lite6_controller_io"]


@pytest.mark.parametrize("action,expected", [
    ("prepare", ["gripper", "lite6", "lite6_arm", "lite6_controller_io"]),
    ("disable", ["gripper", "lite6_arm", "lite6_controller_io", "lite6"]),
    ("power_off", ["gripper", "lite6_arm", "lite6_controller_io", "lite6"]),
    ("reboot", ["gripper", "lite6_arm", "lite6_controller_io", "lite6"]),
    ("recover", ["gripper", "lite6_arm", "lite6", "lite6_controller_io"]),
    ("enable", ["gripper", "lite6_arm", "lite6", "lite6_controller_io"]),
])
def test_group_action_order_follows_owners_and_members(action: str, expected: list[str]) -> None:
    assert group_action_order(_machine(), action) == expected


def test_a_prepared_owner_does_not_block_execute_but_its_prepared_member_does() -> None:
    states = _machine()
    for row in states:
        row["lifecycle"] = "enabled" if row["component_id"] != "lite6" else "prepared"
    assert _ready(_model(component_states=states))["can_execute"]
    states[1]["lifecycle"] = "prepared"
    readiness = _ready(_model(component_states=states))
    assert [(r["code"], r.get("component_id")) for r in readiness["blocked_reasons"]] == [
        ("component_not_enabled", "lite6_arm")]


def test_wait_for_action_returns_the_new_last_action() -> None:
    old = {"action": "enable", "outcome": "succeeded", "started_at_ns": 1, "finished_at_ns": 2}
    new = {"action": "disable", "outcome": "succeeded", "started_at_ns": 3, "finished_at_ns": 4}
    reads = iter([
        [_state("robot", last_action=old)],
        [_state("robot", action={"action": "disable", "started_at_ns": 3}, last_action=old)],
        [_state("robot", "prepared", last_action=new)],
    ])
    assert (
        wait_for_action(
            lambda: next(reads), "robot", "disable", old, timeout_s=10.0, poll_seconds=0.0
        )
        == new
    )


def test_wait_for_action_ends_unknown_after_the_lifecycle_timeout() -> None:
    busy = [_state("robot", action={"action": "prepare", "started_at_ns": 3})]
    result = wait_for_action(
        lambda: busy, "robot", "prepare", None, timeout_s=0.0, poll_seconds=0.0
    )
    assert result["outcome"] == "unknown"
    assert result["action"] == "prepare"
    assert result["error"]["code"] == "component_action_outcome_unknown"


def test_wait_for_action_refuses_a_component_that_left_the_cell() -> None:
    with pytest.raises(HeadlessApiError) as missing:
        wait_for_action(lambda: [], "robot", "enable", None, timeout_s=1.0, poll_seconds=0.0)
    assert missing.value.code == "component_not_found"


# REST surface --------------------------------------------------------------


class _ModelExecution(FakeExecution):
    def __init__(self, reply: dict[str, Any]) -> None:
        super().__init__()
        self.reply = reply
        self.actions: list[tuple[str, str]] = []
        self.writes: list[tuple[str, Any]] = []
        self.refusal: str | None = None
        self.done = threading.Event()
        self.outcome: dict[str, str] = {}
        self.stop_after_first = False

    def control_state(self) -> dict[str, Any]:
        generation = 2 if self.stop_after_first and self.actions else 1
        return {"ok": True, "command": "control_state",
                "control_state": {"cell_stop_action": {"expected_stop_generation": generation}}}

    def component_model(self) -> dict[str, Any]:
        return self.reply

    def component_action(self, component_id: str, action: str) -> dict[str, Any]:
        if self.refusal is not None:
            return {"ok": False, "error": {"code": self.refusal, "message": "refused"}}
        self.actions.append((component_id, action))
        # The coordinator finishes at once: the state carries the result.
        for row in self.reply["component_states"]:
            if row["component_id"] == component_id:
                row["last_action"] = {"action": action,
                                      "outcome": self.outcome.get(component_id, "succeeded"),
                                      "started_at_ns": len(self.actions), "finished_at_ns": 99}
                row["lifecycle"] = "enabled" if action == "enable" else row["lifecycle"]
        if len(self.actions) == 2:
            self.done.set()
        return {"ok": True, "command": "component_action",
                "accepted": {"component_id": component_id, "action": action}}

    def set_io_output(self, signal_id: str, value: Any, *, origin: str) -> dict[str, Any]:
        assert origin == "manual"
        if self.refusal is not None:
            return {"ok": False, "error": {"code": self.refusal, "message": "refused"}}
        self.writes.append((signal_id, value))
        return {"ok": True, "command": "set_io_output", "result": {"outcome": "succeeded"}}


def _rest(reply: dict[str, Any] | None = None) -> tuple[TestClient, _ModelExecution]:
    execution = _ModelExecution(reply or _reply())
    service = _service(FakeCore(), FakeManager(), execution,
                       program_runner=FakeProgramRunner(active=False))
    return TestClient(create_app(service=service)), execution


def test_component_reads_have_the_contract_shapes() -> None:
    client, _execution = _rest()
    with client:
        components = client.get("/api/components").json()
        latch = client.get("/api/cell/latch").json()
        signals = client.get("/api/io").json()
        readiness = client.get("/api/program/readiness").json()
    assert [row["component_id"] for row in components] == ["robot", "gripper"]
    assert latch == {"latched": False, "causes": []}
    assert [row["signal_id"] for row in signals] == ["door", "CO0"]
    # The shape; can_resume follows the fake runner's allowed commands.
    assert set(readiness) == {"can_execute", "can_resume", "motion_clear", "blocked_reasons"}
    assert readiness["can_execute"] is True and isinstance(readiness["can_resume"], bool)
    assert readiness["blocked_reasons"] == []


def test_one_action_is_accepted_with_202() -> None:
    client, execution = _rest()
    with client:
        response = client.post("/api/components/robot/actions", json={"action": "disable"})
    assert response.status_code == 202
    assert response.json() == {"action": "disable"}
    assert execution.actions == [("robot", "disable")]


@pytest.mark.parametrize("code,status", [
    ("component_action_in_progress", 409),
    ("component_action_unsupported", 409),
    ("simulation_not_playing", 409),
    ("component_not_found", 404),
])
def test_action_refusals_keep_their_codes(code: str, status: int) -> None:
    client, execution = _rest()
    execution.refusal = code
    with client:
        response = client.post("/api/components/robot/actions", json={"action": "reboot"})
    assert response.status_code == status
    assert response.json()["detail"]["code"] == code


def test_an_unknown_action_is_refused_before_dispatch() -> None:
    client, execution = _rest()
    with client:
        response = client.post("/api/components/robot/actions", json={"action": "home"})
    assert response.status_code == 422
    assert execution.actions == []


def test_a_group_action_runs_the_named_components_one_at_a_time_in_cell_order() -> None:
    reply = _reply(component_states=[_state("robot", "prepared"), _state("io"),
                                     _state("gripper", "prepared")])
    client, execution = _rest(reply)
    with client:
        response = client.post("/api/components/actions",
                               json={"action": "enable", "component_ids": ["gripper", "robot"]})
        assert execution.done.wait(5.0)
    assert response.status_code == 202
    assert response.json() == {"action": "enable"}
    assert execution.actions == [("robot", "enable"), ("gripper", "enable")]


@pytest.mark.parametrize("body", [{"action": "enable"},
                                  {"action": "enable", "component_ids": []}])
def test_a_group_action_requires_component_ids(body: dict[str, Any]) -> None:
    client, execution = _rest()
    with client:
        response = client.post("/api/components/actions", json=body)
    assert response.status_code == 422
    assert execution.actions == []


@pytest.mark.parametrize("action,expected", [
    ("prepare", [("lite6", "prepare"), ("lite6_arm", "prepare")]),
    ("disable", [("lite6_arm", "disable"), ("lite6", "disable")]),
])
def test_group_actions_run_owners_and_members_in_contract_order(action, expected) -> None:
    reply = _reply(component_states=[_state("lite6_arm", "prepared", owner_id="lite6", role="arm"),
                                     _state("lite6", "prepared")])
    client, execution = _rest(reply)
    with client:
        response = client.post("/api/components/actions",
                               json={"action": action, "component_ids": ["lite6_arm", "lite6"]})
        assert execution.done.wait(5.0)
    assert response.status_code == 202
    assert execution.actions == expected


def _run_group(execution: _ModelExecution, action: str, component_ids: list[str]) -> None:
    service = _service(FakeCore(), FakeManager(), execution,
                       program_runner=FakeProgramRunner(active=False))
    service.component_actions(action, component_ids)
    for thread in threading.enumerate():
        if thread.name == "component-group-action":
            thread.join(5.0)


def test_a_group_prepare_skips_the_members_of_an_owner_whose_prepare_failed() -> None:
    reply = _reply(component_states=[
        _state("lite6", "not_prepared"),
        _state("lite6_arm", "not_prepared", owner_id="lite6", role="arm"),
        _state("gripper", "not_prepared"),
    ])
    execution = _ModelExecution(reply)
    execution.outcome = {"lite6": "failed"}
    _run_group(execution, "prepare", ["lite6", "lite6_arm", "gripper"])
    assert execution.actions == [("lite6", "prepare"), ("gripper", "prepare")]


def _graph(**timeouts: float) -> dict[str, Any]:
    """An active cell graph authoring `timeouts`; `io` is an I/O device."""

    return {"runtime_units": [{"configuration": {
        "lifecycle_command_timeouts": [
            {"controllable_component_id": component_id, "lifecycle_command_timeout_s": value}
            for component_id, value in timeouts.items() if component_id != "io"],
        "io_provider_devices": [
            {"controllable_component_id": "io", "lifecycle_command_timeout_s": timeouts["io"]}
        ] if "io" in timeouts else [],
    }}]}


def test_lifecycle_timeouts_are_read_from_the_active_cell_graph_as_the_host_reads_them() -> None:
    assert lifecycle_timeouts_s(_graph(robot=25.0, io=3)) == {"robot": 25.0, "io": 3.0}
    assert lifecycle_timeouts_s({"runtime_units": [{"configuration": {}}]}) == {}


def test_the_launch_bound_sums_lifecycle_timeouts_and_a_prepare_awaits_an_unsettled_launch() -> None:
    assert launch_settle_bound_s([25.0, 10.0]) == 35.0
    assert launch_settle_bound_s([]) == DEFAULT_LIFECYCLE_TIMEOUT_S
    assert action_wait_s("prepare", 25.0, 35.0, launch_settled=False) == 60.0
    assert action_wait_s("prepare", 25.0, 35.0, launch_settled=True) == 25.0
    assert action_wait_s("enable", 25.0, 35.0, launch_settled=False) == 25.0


def test_a_group_action_waits_for_each_result_within_its_component_bound(monkeypatch) -> None:
    import manyforge_headless_api.service as service_module

    bounds: list[tuple[str, float]] = []
    real_wait = service_module.wait_for_action

    def record(read_states, component_id, action, previous, *, timeout_s):  # type: ignore[no-untyped-def]
        bounds.append((component_id, timeout_s))
        return real_wait(read_states, component_id, action, previous, timeout_s=timeout_s)

    monkeypatch.setattr(service_module, "wait_for_action", record)
    settled = {"action": "disable", "outcome": "succeeded", "started_at_ns": 0, "finished_at_ns": 1}
    reply = _reply(component_states=[
        _state("robot", "not_prepared"),  # just launched: no action yet
        _state("gripper", "not_prepared", last_action=settled),
        _state("io", "not_prepared", last_action=settled),
    ])
    execution = _ModelExecution(reply)
    core = FakeCore()
    core.manager_payload["active"]["cell_graph"] = _graph(robot=25.0, io=3.0)
    service = _service(core, FakeManager(), execution,
                       program_runner=FakeProgramRunner(active=False))
    service.component_actions("prepare", ["robot", "gripper", "io"])
    for thread in threading.enumerate():
        if thread.name == "component-group-action":
            thread.join(5.0)
    launch = 25.0 + DEFAULT_LIFECYCLE_TIMEOUT_S + 3.0  # every component of the cell
    assert bounds == [
        ("robot", launch + 25.0),
        ("gripper", DEFAULT_LIFECYCLE_TIMEOUT_S),
        ("io", 3.0),
    ]


def test_a_stop_ends_the_group_action() -> None:
    reply = _reply(component_states=[_state("robot", "prepared"), _state("gripper", "prepared")])
    execution = _ModelExecution(reply)
    execution.stop_after_first = True
    _run_group(execution, "enable", ["robot", "gripper"])
    assert execution.actions == [("robot", "enable")]


def test_a_second_group_action_while_one_runs_is_refused() -> None:
    reply = _reply(component_states=[_state("robot", "prepared"), _state("gripper", "prepared")])
    execution = _ModelExecution(reply)
    release = threading.Event()
    dispatch = execution.component_action

    def held(component_id: str, action: str) -> dict[str, Any]:
        release.wait(5.0)
        return dispatch(component_id, action)

    execution.component_action = held  # type: ignore[method-assign]
    service = _service(FakeCore(), FakeManager(), execution,
                       program_runner=FakeProgramRunner(active=False))
    client = TestClient(create_app(service=service))
    with client:
        first = client.post("/api/components/actions",
                            json={"action": "enable", "component_ids": ["robot", "gripper"]})
        second = client.post("/api/components/actions",
                             json={"action": "enable", "component_ids": ["robot"]})
        release.set()
        assert execution.done.wait(5.0)
    assert first.status_code == 202
    assert second.status_code == 409
    assert second.json()["detail"]["code"] == "component_action_in_progress"
    assert execution.actions == [("robot", "enable"), ("gripper", "enable")]


def test_owner_not_prepared_refusal_passes_through() -> None:
    client, execution = _rest()
    execution.refusal = "owner_not_prepared"
    with client:
        response = client.post("/api/components/robot/actions", json={"action": "enable"})
    assert response.status_code == 409
    assert response.json()["detail"]["code"] == "owner_not_prepared"


def test_a_group_naming_an_unknown_component_is_refused() -> None:
    client, execution = _rest()
    with client:
        response = client.post(
            "/api/components/actions", json={"action": "disable", "component_ids": ["ghost"]})
    assert response.status_code == 404
    assert response.json()["detail"]["code"] == "component_not_found"
    assert execution.actions == []


def test_an_output_write_returns_its_result_and_refusals_keep_their_code() -> None:
    client, execution = _rest()
    with client:
        written = client.post("/api/io/robot%2FCO0/set", json={"value": True})
        execution.refusal = "cell_latched"
        refused = client.post("/api/io/robot/CO0/set", json={"value": False})
    assert written.json() == {"outcome": "succeeded"}
    assert execution.writes == [("robot/CO0", True)]
    assert refused.status_code == 409
    assert refused.json()["detail"]["code"] == "cell_latched"


# /ws/control events and Overview tiles --------------------------------------


class _Facts:
    def __init__(self, model: ComponentModel | None, cycle_state: str = "idle_after_reset") -> None:
        self.component_model = model
        self.program_cycle = {"state": cycle_state}
        self.execution_control_response = None
        self.applied_program_status = None


def test_shell_events_carry_the_current_values() -> None:
    events = _component_model_events(_Facts(_model()))  # type: ignore[arg-type]
    assert set(events) == {"component_state", "cell_latch", "io_signals", "run_readiness"}
    assert [row["component_id"] for row in events["component_state"]] == ["robot", "gripper"]
    assert events["run_readiness"]["can_execute"] is True



def test_shell_events_are_null_while_the_model_cannot_be_read() -> None:
    # The GUI shows "unavailable", never the last values.
    assert _component_model_events(_Facts(None)) == {  # type: ignore[arg-type]
        "component_state": None, "cell_latch": None, "io_signals": None, "run_readiness": None}


def test_age_only_changes_publish_no_event() -> None:
    first = _component_model_events(_Facts(_model()))  # type: ignore[arg-type]
    aged = _reply()
    aged["component_states"][0]["observed_at_ns"] = 999
    aged["signal_samples"][0]["age_ns"] = 999
    second = _component_model_events(_Facts(parse_component_model(aged)))  # type: ignore[arg-type]
    for name in ("component_state", "io_signals"):
        assert control_shell_fact_key(first, name) == control_shell_fact_key(second, name)
    changed = _reply()
    changed["component_states"][0]["lifecycle"] = "prepared"
    third = _component_model_events(_Facts(parse_component_model(changed)))  # type: ignore[arg-type]
    assert control_shell_fact_key(first, "component_state") != control_shell_fact_key(
        third, "component_state")


def _overview(model: ComponentModel | None, errors: dict[str, Any] | None = None) -> dict[str, Any]:
    return build_component_overview(
        runtime=None, alarms=None, authoring_admission=None, program_cycle=None,
        applied_program=None, source_model=None, execution_identity_response=None,
        execution_targets_response=None, execution_control_response=None,
        component_model=model, errors=errors or {},
    )


def test_overview_has_one_tile_per_component_from_its_state() -> None:
    model = _model(component_states=[
        _state("robot"),
        _state("gripper", "prepared"),
        _state("io", "fault", fault=_FAULT),
        _state("plc", "offline", reason={"code": "process_exited", "message": "process exited"}),
    ])
    tiles = {row["id"]: row for row in _overview(model)["components"]}
    assert tiles["component:robot"]["status"] == "healthy"
    assert tiles["component:gripper"]["status"] == "waiting"
    assert tiles["component:io"]["status"] == "fault"
    assert tiles["component:io"]["reason_code"] == "fault_estop"
    assert tiles["component:plc"]["status"] == "offline"
    assert tiles["component:plc"]["reason_code"] == "process_exited"
    assert not any(tile_id.startswith("runtime_unit:") for tile_id in tiles)
    assert "component:io" in tiles["execute_control"]["contributor_ids"]


def test_overview_without_the_model_shows_one_offline_components_tile() -> None:
    tiles = {row["id"]: row for row in _overview(
        None, {"component_model": {"code": "execution_owner_unavailable", "message": "x"}}
    )["components"]}
    assert tiles["components"]["status"] == "offline"
    assert tiles["components"]["reason_code"] == "execution_owner_unavailable"


def test_run_end_shape_is_the_contract_one() -> None:
    from manyforge_headless_api.service import _valid_run_end

    assert _valid_run_end(None)
    assert _valid_run_end({"end_reason": "completed", "details": {}})
    assert _valid_run_end(
        {"end_reason": "stopped", "details": {"stopped_by": "rule", "rule_id": "door"}}
    )
    assert _valid_run_end(
        {"end_reason": "cell_fault", "details": {"component_id": "robot",
                                                  "error": {"code": "fault_estop", "message": "e-stop"}}}
    )
    assert not _valid_run_end({"end_reason": "finished", "details": {}})
    assert not _valid_run_end({"end_reason": "stopped", "details": {"stopped_by": "nobody"}})
    assert not _valid_run_end({"end_reason": "failed", "details": {"operation_id": "x"}})
