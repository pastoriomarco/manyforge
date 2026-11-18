"""Server-side Apply orchestration order with a fake host."""

from typing import Any

import pytest

from manyforge_composer.backend.apply_orchestration import (
    ApplyRefused,
    ApplyStepFailed,
    ComponentImpact,
    Preview,
    candidate_preview,
    one_apply_at_a_time,
    preview_view,
    run_apply,
)


class _FakeCell:
    """Components that finish each action at once, plus a recorded Apply."""

    def __init__(
        self,
        lifecycles: dict[str, str],
        impact: list[ComponentImpact],
        *,
        after_apply: dict[str, str] | None = None,
        cycle_state: str = "idle_after_reset",
        held: bool = False,
        fail: dict[tuple[str, str], str] | None = None,
        latched: bool = False,
        owners: dict[str, str] | None = None,
        stalled: frozenset[str] = frozenset(),
        faults: dict[str, str] | None = None,
        faults_after_apply: dict[str, str] | None = None,
    ) -> None:
        # Components whose action never finishes: their owner stalls, so
        # they report the owner-stall fault with the action still running.
        self.stalled = stalled
        # component -> fault code before the Apply, and after it (a component
        # the Apply touches loses its fault unless listed after).
        self.faults = dict(faults or {})
        self.faults_after_apply = faults_after_apply or {}
        self.owners = owners or {}  # member -> owner (ComponentState.owner_id)
        self.lifecycles = dict(lifecycles)
        self.impact = impact
        self.after_apply = after_apply or {}
        self.cycle = cycle_state
        self.held = held  # a running run with all its motion held
        self.fail = fail or {}
        self.latched = latched  # never consulted: Apply is allowed during a latch
        self.calls: list[tuple[str, ...]] = []
        self._results: dict[str, dict[str, Any]] = {}
        self._running: dict[str, str] = {}

    def cycle_state(self) -> str:
        return self.cycle

    def run_held(self) -> bool:
        return self.held

    def reset_cycle(self) -> None:
        self.calls.append(("reset",))
        self.cycle = "idle_after_reset"

    def preview(self) -> Preview:
        self.calls.append(("preview",))
        return Preview(components=tuple(self.impact))

    def components(self) -> list[dict[str, Any]]:
        rows = []
        for component_id, lifecycle in self.lifecycles.items():
            row: dict[str, Any] = {"component_id": component_id, "lifecycle": lifecycle}
            if component_id in self.owners:
                row["owner_id"] = self.owners[component_id]
            if component_id in self.faults:
                row["fault"] = {
                    "code": self.faults[component_id], "message": "exited",
                    "clearable": True, "operator_action": "recover", "since_ns": 1,
                }
            if component_id in self._results:
                row["last_action"] = self._results[component_id]
            if component_id in self._running:
                row["action"] = {"action": self._running[component_id], "started_at_ns": 1}
            rows.append(row)
        return rows

    def component_action(self, component_id: str, action: str) -> None:
        self.calls.append((action, component_id))
        if component_id in self.stalled:
            self._running[component_id] = action
            self.faults[component_id] = "component_owner_stalled"
            return
        code = self.fail.get((component_id, action))
        result: dict[str, Any] = {
            "action": action,
            "outcome": "failed" if code else "succeeded",
            "started_at_ns": len(self.calls),
            "finished_at_ns": len(self.calls),
        }
        if code:
            result["error"] = {"code": code, "message": f"{action} failed"}
        else:
            self.lifecycles[component_id] = {"disable": "prepared", "prepare": "prepared"}[action]
        self._results[component_id] = result

    def apply(self) -> None:
        self.calls.append(("apply",))
        for component_id in self.after_apply:
            self.faults.pop(component_id, None)
        self.faults.update(self.faults_after_apply)
        for component_id, lifecycle in self.after_apply.items():
            if lifecycle is None:
                self.lifecycles.pop(component_id, None)
            else:
                self.lifecycles[component_id] = lifecycle


def _impact(**rows: str) -> list[ComponentImpact]:
    return [ComponentImpact(component_id, disposition, ()) for component_id, disposition in rows.items()]


def test_disable_only_enabled_leaving_components_then_apply_then_prepare() -> None:
    cell = _FakeCell(
        {"robot": "enabled", "gripper": "enabled", "camera": "enabled", "plc": "fault", "old": "prepared"},
        _impact(robot="replace", gripper="replace", camera="retain", plc="remove", old="remove", tool="add"),
        after_apply={"robot": "not_prepared", "gripper": "not_prepared", "plc": None, "old": None, "tool": "not_prepared"},
    )
    result = run_apply(cell)
    assert cell.calls == [
        ("preview",),
        ("disable", "gripper"),  # reverse cell order
        ("disable", "robot"),
        ("apply",),
        ("prepare", "robot"),  # cell order; never enable
        ("prepare", "gripper"),
        ("prepare", "tool"),
    ]
    assert result["outcome"] == "succeeded" and "error" not in result
    assert {row["component_id"]: row.get("lifecycle_after") for row in result["components"]} == {
        "robot": "prepared",
        "gripper": "prepared",
        "camera": "enabled",
        "plc": None,
        "old": None,
        "tool": "prepared",
    }
    # Removed components carry no `lifecycle_after` at all.
    removed = {row["component_id"]: row for row in result["components"]}["plc"]
    assert removed == {"component_id": "plc", "disposition": "remove"}


def test_dead_or_faulted_components_are_removed_without_disable() -> None:
    cell = _FakeCell(
        {"robot": "fault", "io": "offline"},
        _impact(robot="remove", io="remove"),
        after_apply={"robot": None, "io": None},
    )
    run_apply(cell)
    assert cell.calls == [("preview",), ("apply",)]


# `running` here is a run whose motion is executing (not held).
@pytest.mark.parametrize("cycle", ["running", "starting", "resetting"])
def test_refused_while_a_run_is_in_progress_before_any_effect(cycle: str) -> None:
    cell = _FakeCell({"robot": "enabled"}, _impact(robot="replace"), cycle_state=cycle)
    with pytest.raises(ApplyRefused) as refused:
        run_apply(cell)
    assert refused.value.code == "apply_refused_run_in_progress"
    assert cell.calls == []


def test_a_finished_run_is_reset_by_the_apply_itself() -> None:
    cell = _FakeCell(
        {"robot": "enabled"},
        _impact(robot="replace"),
        after_apply={"robot": "not_prepared"},
        cycle_state="terminal_awaiting_reset",
    )
    assert run_apply(cell)["outcome"] == "succeeded"
    assert cell.calls == [
        ("reset",),  # first: the Preview's preparation needs an idle cycle
        ("preview",),
        ("disable", "robot"),
        ("apply",),
        ("prepare", "robot"),
    ]


def test_a_failed_reset_ends_the_apply_before_its_preview() -> None:
    cell = _FakeCell(
        {"robot": "enabled"}, _impact(robot="replace"), cycle_state="terminal_awaiting_reset"
    )

    def reset_fails() -> None:
        cell.calls.append(("reset",))
        raise ApplyStepFailed("program_cycle_reset_failed", "Reset failed")

    cell.reset_cycle = reset_fails  # type: ignore[method-assign]
    assert run_apply(cell) == {
        "outcome": "failed",
        "components": [],
        "error": {"code": "program_cycle_reset_failed", "message": "Reset failed"},
    }
    assert cell.calls == [("reset",)]


def test_a_held_run_is_reset_by_the_apply_itself() -> None:
    # As Close does: a run with all its motion stopped and held is Reset
    # first, like a finished one.
    cell = _FakeCell(
        {"robot": "enabled"},
        _impact(robot="replace"),
        after_apply={"robot": "not_prepared"},
        cycle_state="running",
        held=True,
    )
    assert run_apply(cell)["outcome"] == "succeeded"
    assert cell.calls == [
        ("reset",),
        ("preview",),
        ("disable", "robot"),
        ("apply",),
        ("prepare", "robot"),
    ]


def test_a_step_ends_on_the_owner_stall_fault_with_its_code() -> None:
    # OD-214 (T34): a step has no limit of its own; an action whose owner
    # stalled never finishes, and the step ends unknown with the stall's code.
    cell = _FakeCell(
        {"robot": "enabled"}, _impact(robot="replace"), stalled=frozenset({"robot"})
    )
    result = run_apply(cell)
    assert cell.calls == [("preview",), ("disable", "robot")]  # never resent
    assert result["outcome"] == "failed"
    assert result["error"] == {"code": "component_owner_stalled", "message": "exited"}
    assert result["components"][0]["error"]["code"] == "component_owner_stalled"


def test_a_step_ends_when_the_component_read_fails() -> None:
    # OD-214 (T34): the host lost after the dispatch ends the step with the
    # read's own error, mapped like every port error.
    class _LostHost(Exception):
        pass

    cell = _FakeCell({"robot": "enabled"}, _impact(robot="replace"))
    dispatch = cell.component_action

    def dispatch_then_lose(component_id: str, action: str) -> None:
        dispatch(component_id, action)

        def lost() -> list[dict[str, Any]]:
            raise _LostHost("the host is gone")

        cell.components = lost  # type: ignore[method-assign]

    cell.component_action = dispatch_then_lose  # type: ignore[method-assign]
    with pytest.raises(_LostHost):
        run_apply(cell)  # an unmapped port error propagates
    assert cell.calls == [("preview",), ("disable", "robot")]


def test_one_apply_at_a_time() -> None:
    with one_apply_at_a_time():
        with pytest.raises(ApplyRefused) as refused:
            with one_apply_at_a_time():
                pass
    assert refused.value.code == "apply_in_progress"
    with one_apply_at_a_time():  # released again
        pass


def test_allowed_during_a_cell_latch() -> None:
    cell = _FakeCell({"robot": "fault"}, _impact(robot="remove"), after_apply={"robot": None}, latched=True)
    assert run_apply(cell)["outcome"] == "succeeded"


def test_a_failed_disable_is_reported_and_the_apply_still_goes_ahead() -> None:
    # The gripper stays enabled because its Disable failed; the host
    # stops its process like a dead one, so the Apply is submitted.
    cell = _FakeCell(
        {"robot": "enabled", "gripper": "enabled", "camera": "enabled"},
        _impact(robot="replace", gripper="replace", camera="remove"),
        after_apply={"robot": "not_prepared", "gripper": "not_prepared", "camera": None},
        fail={("gripper", "disable"): "controller_refused"},
    )
    result = run_apply(cell)
    assert cell.calls == [
        ("preview",), ("disable", "camera"), ("disable", "gripper"), ("disable", "robot"),
        ("apply",), ("prepare", "robot"), ("prepare", "gripper"),
    ]
    assert result["outcome"] == "failed"
    assert result["error"] == {"code": "controller_refused", "message": "disable failed"}
    rows = {row["component_id"]: row for row in result["components"]}
    assert rows["gripper"]["error"] == {"code": "controller_refused", "message": "disable failed"}
    assert rows["gripper"]["lifecycle_after"] == "prepared"
    assert "error" not in rows["robot"] and "error" not in rows["camera"]


def test_a_failed_disable_of_a_removed_component_does_not_block_new() -> None:
    # New empties the cell: the robot's Disable fails and it is still removed.
    cell = _FakeCell(
        {"robot": "enabled"},
        _impact(robot="remove"),
        after_apply={"robot": None},
        fail={("robot", "disable"): "controller_refused"},
    )
    result = run_apply(cell)
    assert cell.calls == [("preview",), ("disable", "robot"), ("apply",)]
    assert result["components"] == [{
        "component_id": "robot",
        "disposition": "remove",
        "error": {"code": "controller_refused", "message": "disable failed"},
    }]


def test_an_enabled_component_without_a_disable_attempt_is_still_refused() -> None:
    # The camera is enabled (e.g. by an operator) while the robot is being
    # disabled: its Disable was never tried, so nothing is submitted.
    class _Race(_FakeCell):
        def component_action(self, component_id: str, action: str) -> None:
            super().component_action(component_id, action)
            self.lifecycles["camera"] = "enabled"

    cell = _Race(
        {"robot": "enabled", "camera": "prepared"}, _impact(robot="remove", camera="remove")
    )
    result = run_apply(cell)
    assert cell.calls == [("preview",), ("disable", "robot")]
    assert result["error"] == {
        "code": "apply_requires_inactive_component",
        "message": "camera is still enabled",
    }


def test_one_failing_prepare_among_three_the_others_are_still_prepared() -> None:
    cell = _FakeCell(
        {},
        _impact(robot="add", gripper="add", camera="add"),
        after_apply={"robot": "not_prepared", "gripper": "not_prepared", "camera": "not_prepared"},
        fail={("gripper", "prepare"): "fault_communication"},
    )
    result = run_apply(cell)
    assert cell.calls == [
        ("preview",), ("apply",),
        ("prepare", "robot"), ("prepare", "gripper"), ("prepare", "camera"),
    ]
    assert result["outcome"] == "failed"
    assert result["error"]["code"] == "fault_communication"
    assert result["components"] == [
        {"component_id": "robot", "disposition": "add", "lifecycle_after": "prepared"},
        {"component_id": "gripper", "disposition": "add", "lifecycle_after": "not_prepared",
         "error": {"code": "fault_communication", "message": "prepare failed"}},
        {"component_id": "camera", "disposition": "add", "lifecycle_after": "prepared"},
    ]


# Decided (OD-211, 1): a retained component whose process had exited is
# rebuilt by the host's Apply with a new process; it is prepared afterwards,
# as a replaced one is. Its prior state decides nothing.
def test_a_retained_component_whose_process_the_apply_started_again_is_prepared() -> None:
    cell = _FakeCell(
        {"robot": "enabled", "plc": "fault", "camera": "prepared"},
        _impact(robot="replace", plc="retain", camera="retain"),
        after_apply={"robot": "not_prepared", "plc": "not_prepared"},
        faults={"plc": "fault_process_exited"},
    )
    result = run_apply(cell)
    assert cell.calls == [
        ("preview",), ("disable", "robot"), ("apply",),
        ("prepare", "robot"), ("prepare", "plc"),
    ]
    assert result["outcome"] == "succeeded"
    rows = {row["component_id"]: row for row in result["components"]}
    assert rows["plc"] == {"component_id": "plc", "disposition": "retain", "lifecycle_after": "prepared"}
    assert rows["camera"]["lifecycle_after"] == "prepared"


def test_a_retained_component_with_a_live_process_is_not_touched() -> None:
    cell = _FakeCell(
        {"plc": "not_prepared", "sensor": "fault", "camera": "offline"},
        _impact(plc="retain", sensor="retain", camera="retain", tool="add"),
        after_apply={"tool": "not_prepared"},
        # A kept component still shows its exit (e.g. one whose adapter runs
        # its own process): the Apply didn't start it again.
        faults={"sensor": "fault_process_exited", "camera": "fault_communication"},
    )
    run_apply(cell)
    assert cell.calls == [("preview",), ("apply",), ("prepare", "tool")]


def test_a_failed_prepare_of_a_restarted_component_is_reported_as_for_a_replaced_one() -> None:
    cell = _FakeCell(
        {"plc": "fault", "robot": "prepared"},
        _impact(plc="retain", robot="replace"),
        after_apply={"plc": "not_prepared", "robot": "not_prepared"},
        faults={"plc": "fault_process_exited"},
        fail={("plc", "prepare"): "simulated_plc_not_ready", ("robot", "prepare"): "fault_communication"},
    )
    result = run_apply(cell)
    assert result["outcome"] == "failed"
    assert result["error"]["code"] == "simulated_plc_not_ready"
    rows = {row["component_id"]: row for row in result["components"]}
    assert rows["plc"] == {
        "component_id": "plc", "disposition": "retain", "lifecycle_after": "not_prepared",
        "error": {"code": "simulated_plc_not_ready", "message": "prepare failed"},
    }
    assert rows["robot"]["error"] == {"code": "fault_communication", "message": "prepare failed"}


def test_a_restarted_component_whose_exit_is_shown_again_is_not_prepared() -> None:
    cell = _FakeCell(
        {"plc": "fault"},
        _impact(plc="retain"),
        after_apply={"plc": "fault"},
        faults={"plc": "fault_process_exited"},
        faults_after_apply={"plc": "fault_process_exited"},
    )
    result = run_apply(cell)
    assert cell.calls == [("preview",), ("apply",)]
    assert result["outcome"] == "succeeded"


def test_members_of_an_owner_whose_prepare_failed_are_not_attempted() -> None:
    cell = _FakeCell(
        {},
        _impact(machine="add", arm="add", io="add", camera="add"),
        after_apply={
            "machine": "not_prepared", "arm": "not_prepared",
            "io": "not_prepared", "camera": "not_prepared",
        },
        owners={"arm": "machine", "io": "machine"},
        fail={("machine", "prepare"): "fault_communication"},
    )
    result = run_apply(cell)
    assert cell.calls == [
        ("preview",), ("apply",), ("prepare", "machine"), ("prepare", "camera"),
    ]
    rows = {row["component_id"]: row for row in result["components"]}
    assert rows["machine"]["error"]["code"] == "fault_communication"
    assert rows["arm"]["error"]["code"] == "owner_not_prepared"
    assert rows["io"]["error"]["code"] == "owner_not_prepared"
    assert "error" not in rows["camera"]
    assert result["error"]["code"] == "fault_communication"


def test_a_manager_failure_is_mapped_by_the_route() -> None:
    cell = _FakeCell({"robot": "prepared"}, _impact(robot="replace"))

    def refuse() -> None:
        raise ApplyStepFailed("manager_apply_rejected", "Manager rejected the candidate")

    cell.apply = refuse  # type: ignore[method-assign]
    result = run_apply(cell)
    assert result["error"]["code"] == "manager_apply_rejected"
    assert result["components"] == [
        {"component_id": "robot", "disposition": "replace", "lifecycle_after": "prepared"}
    ]


def test_preview_view_carries_the_impact_program_attachments_and_scope() -> None:
    reference = {
        "target": {"component_id": "robot", "command_target_id": "arm"},
        "detail": "removed", "reason": "component_missing",
    }
    manager_preview = {
        "impact": {
            "components": [
                {"controllable_component_id": "robot", "disposition": "replace",
                 "replacement_reasons": ["description"]},
                {"controllable_component_id": "camera", "disposition": "retain"},
            ],
            "attachment_changed_components": ["gripper"],
            "detached_components": ["tray"],
            "program": {
                "executable": False,
                "program_id": "pick",
                "program_revision": "sha256:" + "1" * 64,
                "non_executable": {
                    "unresolved_targets": [reference],
                    "affected_nodes": ["move_1"],
                    "affected_execution_groups": ["main"],
                },
            },
        }
    }
    scope = {"deployment_draft_id": "d", "deployment_draft_revision": 3}
    assert preview_view(candidate_preview(manager_preview), scope) == {
        "components": [
            {"component_id": "robot", "disposition": "replace",
             "reasons": ["description"], "restarts": True},
            {"component_id": "camera", "disposition": "retain",
             "reasons": [], "restarts": False},
        ],
        "scope": scope,
        "program": {
            "executable": False,
            "program_id": "pick",
            "unresolved_references": [reference],
            "affected_nodes": ["move_1"],
            "affected_execution_groups": ["main"],
        },
        "attachment_changed_components": ["gripper"],
        "detached_components": ["tray"],
    }


def test_an_executable_program_has_no_unresolved_references() -> None:
    view = preview_view(
        candidate_preview({"impact": {"components": [], "program": {
            "executable": True, "program_id": "pick", "non_executable": None,
        }}}),
        {},
    )
    assert view["program"] == {
        "executable": True, "program_id": "pick", "unresolved_references": [],
        "affected_nodes": [], "affected_execution_groups": [],
    }


def test_owners_are_prepared_before_members_and_members_disabled_first() -> None:
    # Cell order puts the members (lite6_robot*) after their owner (lite6)
    # only by chance of the ids; here the members sort first.
    cell = _FakeCell(
        {"arm": "enabled", "io": "enabled", "machine": "prepared"},
        _impact(arm="replace", io="replace", machine="replace"),
        after_apply={"arm": "not_prepared", "io": "not_prepared", "machine": "not_prepared"},
        owners={"arm": "machine", "io": "machine"},
    )
    run_apply(cell)
    assert cell.calls == [
        ("preview",),
        ("disable", "io"),  # members first, reverse cell order; the owner has no disable
        ("disable", "arm"),
        ("apply",),
        ("prepare", "machine"),  # the owner before its members
        ("prepare", "arm"),
        ("prepare", "io"),
    ]


def test_an_arm_launch_edit_prepares_the_rebuilt_owner_then_its_members() -> None:
    # An arm-only edit (e.g. its initial joint positions): the host rebuilds
    # the machine with its arm, so the delta replaces the owner and, through
    # it, every member; an unrelated component is retained and left alone.
    cell = _FakeCell(
        {"lite6": "prepared", "lite6_robot": "enabled", "lite6_controller_io": "enabled",
         "other": "enabled"},
        _impact(lite6="replace", lite6_robot="replace", lite6_controller_io="replace",
                other="retain"),
        after_apply={"lite6": "not_prepared", "lite6_robot": "not_prepared",
                     "lite6_controller_io": "not_prepared"},
        owners={"lite6_robot": "lite6", "lite6_controller_io": "lite6"},
    )
    result = run_apply(cell)
    assert cell.calls == [
        ("preview",),
        ("disable", "lite6_controller_io"),
        ("disable", "lite6_robot"),
        ("apply",),
        ("prepare", "lite6"),
        ("prepare", "lite6_robot"),
        ("prepare", "lite6_controller_io"),
    ]
    assert result["outcome"] == "succeeded"


def test_a_second_apply_route_is_refused_before_any_effect() -> None:
    from fastapi import HTTPException

    from manyforge_composer.backend.routes_deployment_draft import apply_slot

    with apply_slot():
        with pytest.raises(HTTPException) as refused:
            with apply_slot():
                pass
    assert refused.value.status_code == 409
    assert refused.value.detail == {
        "code": "apply_in_progress",
        "message": "Another Apply, Load or New is in progress",
        "effect": "none",
    }


_RESET_KEY = "c" * 32
_SUCCEEDED = {"outcome": "succeeded", "code": None, "details": None, "finished_at_ns": "2"}


def _reset_row(step: str | None = "admitting", end: dict[str, Any] | None = None) -> dict[str, Any]:
    # The runner's row of the Reset (OD-214); its key is the one Headless formed.
    return {"kind": "reset", "request_key": _RESET_KEY, "step": step,
            "accepted_at_ns": "1", "end": end}


class _CycleClient:
    """A Runner that accepts the Reset, shows it in progress once, then ended."""

    def components(self) -> list[dict[str, Any]]:
        return []  # an empty installed cell

    def __init__(self, end: dict[str, Any], *, first: dict[str, Any] | None = None) -> None:
        self.reads = [
            first or {"state": "terminal_awaiting_reset", "cycle_generation": 4,
                      "program_run_id": "run.1", "cycle_claim_generation": 2,
                      "terminal_or_settlement_reference": "sha256:" + "a" * 64},
            {"state": "resetting", "cycle_generation": 4,
             "runner_commands": {"in_progress": _reset_row("stopping"), "ended": []}},
            {"state": "idle_after_reset", "cycle_generation": 5,
             "runner_commands": {"in_progress": None,
                                 "ended": [_reset_row("releasing", end)]}},
        ]
        self.resets: list[dict[str, Any]] = []

    def program_cycle_status(self) -> dict[str, Any]:
        return self.reads.pop(0)

    def program_cycle_reset(self, body: dict[str, Any]) -> dict[str, Any]:
        self.resets.append(body)
        return {"schema_version": "manyforge.program_command_reply.v1",
                "command": _reset_row()}

    def authoring_admission(self) -> dict[str, Any]:
        return {"authoring_allowed": True, "admission_generation": 7}


def _ports(client: Any):
    from types import SimpleNamespace

    from manyforge_composer.backend.routes_deployment_draft import HeadlessApplyPorts

    ports = HeadlessApplyPorts(
        SimpleNamespace(
            headless_client=client,
            applied_deployment_document=None,
            application_start_id=0x3F9A0000000000A1,
        ),  # type: ignore[arg-type]
    )
    ports._POLL_SECONDS = 0.0
    return ports


def test_the_apply_leaves_the_admission_wait_to_the_preparation() -> None:
    # One wait in one place. An idle cycle goes straight to the Preview;
    # Headless's activation preparation waits for a momentarily closed
    # admission, the Composer reads none.
    from manyforge_composer.backend.apply_orchestration import ApplyPorts

    assert not hasattr(ApplyPorts, "await_authoring")
    cell = _FakeCell({"arm": "enabled"}, _impact(arm="retain"))
    run_apply(cell)
    assert cell.calls[0] == ("preview",)


def test_the_default_lifecycle_timeout_mirrors_the_generated_default() -> None:
    # The Manager's component budgets use `kDefaultLifecycleBudgetNs`,
    # generated from robot_defaults.tsv like this Composer table.
    from manyforge_composer.backend.robot_defaults import COMPONENT_DEFAULTS
    from manyforge_headless_api.component_model import DEFAULT_LIFECYCLE_TIMEOUT_S

    assert DEFAULT_LIFECYCLE_TIMEOUT_S == COMPONENT_DEFAULTS["lifecycle_command_timeout_s"]


def test_the_server_resets_a_finished_run_like_the_program_tab() -> None:
    client = _CycleClient(_SUCCEEDED)
    _ports(client).reset_cycle()
    [reset] = client.resets
    assert reset["program_run_id"] == "run.1"
    # A server-originated Reset: Composer's own `A` and a fresh key, no
    # runner-epoch fence.
    assert reset["application_start_id"] == "3f9a0000000000a1"
    assert len(reset["request_key"]) == 32
    assert "expected_owner_epoch" not in reset and "request_id" not in reset
    assert reset["expected_cycle_generation"] == 4
    assert reset["expected_claim_generation"] == 2
    assert reset["terminal_reference"] == "sha256:" + "a" * 64
    assert (reset["reload_initial_scene"], reset["reset_robot_position"]) == (False, False)


def test_the_server_resets_a_held_run_like_the_program_tab() -> None:
    client = _CycleClient(
        _SUCCEEDED,
        first={"state": "running", "cycle_generation": 4, "program_run_id": "run.1",
               "cycle_claim_generation": 2,
               "terminal_or_settlement_reference": None},
    )
    # The cycle may still read `running` while the Reset row is admitted.
    client.reads.insert(1, {"state": "running", "cycle_generation": 4,
                            "runner_commands": {"in_progress": _reset_row(), "ended": []}})
    _ports(client).reset_cycle()
    [reset] = client.resets
    assert reset["program_run_id"] == "run.1"
    assert reset["expected_claim_generation"] == 2
    assert reset["terminal_reference"] is None


@pytest.mark.parametrize("outcome,code", [
    ("failed", "program_cycle_recovery_required"),
    ("refused", "execution_control_ingress_busy"),
    ("unknown", "program_cycle_reconciliation_required"),
])
def test_a_reset_that_did_not_succeed_ends_the_apply_with_its_rows_code(
    outcome: str, code: str
) -> None:
    client = _CycleClient({"outcome": outcome, "code": code, "details": None,
                           "finished_at_ns": "2"})
    with pytest.raises(ApplyStepFailed) as failed:
        _ports(client).reset_cycle()
    assert failed.value.code == code
    assert client.reads == []


def test_a_reset_that_ended_in_its_reply_reads_no_status() -> None:
    # An idle Reset without effects ends before the reply is built.
    class _EndedAtOnce(_CycleClient):
        def program_cycle_reset(self, body: dict[str, Any]) -> dict[str, Any]:
            self.resets.append(body)
            return {"schema_version": "manyforge.program_command_reply.v1",
                    "command": _reset_row("admitting", _SUCCEEDED)}

    client = _EndedAtOnce(_SUCCEEDED)
    _ports(client).reset_cycle()
    assert len(client.reads) == 2  # only the cycle read before the Reset


def test_the_reset_follows_the_row_the_headless_key_names() -> None:
    # Another command's row (another key) is not this Reset's end.
    client = _CycleClient(_SUCCEEDED)
    client.reads.insert(1, {"state": "resetting", "cycle_generation": 4,
                            "runner_commands": {"in_progress": _reset_row("stopping"),
                                                "ended": [{**_reset_row("releasing", {
                                                    "outcome": "failed",
                                                    "code": "program_cycle_recovery_required",
                                                    "details": None, "finished_at_ns": "1"}),
                                                    "request_key": "d" * 32}]}})
    _ports(client).reset_cycle()
    assert client.reads == []


def test_an_invalid_reset_reply_fails_the_step() -> None:
    class _OldReply(_CycleClient):
        def program_cycle_reset(self, body: dict[str, Any]) -> dict[str, Any]:
            return {"schema_version": "manyforge.program_cycle_reset_result.v1"}

    with pytest.raises(ApplyStepFailed) as failed:
        _ports(_OldReply(_SUCCEEDED)).reset_cycle()
    assert failed.value.code == "program_runner_response_invalid"


def test_the_reset_returns_once_the_cycle_is_idle() -> None:
    # The Runner's Idle reaches the Kernel admission a moment later; the
    # Apply's preparation waits for it (Headless), the Reset doesn't.
    class _NoAdmissionRead(_CycleClient):
        def authoring_admission(self) -> dict[str, Any]:
            raise AssertionError("the Reset reads no admission")

    client = _NoAdmissionRead(_SUCCEEDED)
    _ports(client).reset_cycle()
    assert client.reads == []


def test_the_reset_wait_has_no_bound() -> None:
    # OD-214 (T16): the Reset row is followed to its true end, however long
    # its phases take; no Apply bound decides it.
    client = _CycleClient(_SUCCEEDED)
    for _ in range(50):
        client.reads.insert(1, {"state": "resetting", "cycle_generation": 4,
                                "runner_commands": {"in_progress": _reset_row("settling"),
                                                    "ended": []}})
    _ports(client).reset_cycle()
    assert client.reads == []
    assert len(client.resets) == 1


def test_the_manager_apply_wait_has_no_bound() -> None:
    # OD-214 (T32): a pending Manager Apply is followed to its terminal
    # status, however long it takes; no Apply bound decides it.
    import manyforge_composer.backend.routes_deployment_draft as routes

    class _SlowManager:
        reads = 0

        def components(self) -> list[dict[str, Any]]:
            raise AssertionError("the wait reads no Apply bound")

        def manager_apply_status(
            self, _apply_id: int, _generation: int, *, application_start_id: str
        ) -> dict[str, Any]:
            self.reads += 1
            if self.reads < 50:
                return {"ok": True, "state": "pending"}
            return {"ok": True, "state": "succeeded"}

    assert not hasattr(routes.HeadlessApplyPorts, "MANAGER_APPLY_WAIT_MARGIN_S")
    manager = _SlowManager()
    _ports(manager).wait_for_manager_apply(
        {"status": "accepted", "apply_id": 1, "apply_generation": 1}
    )
    assert manager.reads == 50


def test_a_failed_manager_apply_status_read_ends_the_wait() -> None:
    # Peer loss: the status read's own error ends the wait, typed.
    from manyforge_headless_api.client import HeadlessClientError

    class _Lost:
        reads = 0

        def manager_apply_status(
            self, _apply_id: int, _generation: int, *, application_start_id: str
        ) -> dict[str, Any]:
            self.reads += 1
            if self.reads < 3:
                return {"ok": True, "state": "pending"}
            raise HeadlessClientError("manager_unavailable", "Manager is not reachable")

    lost = _Lost()
    with pytest.raises(HeadlessClientError) as error:
        _ports(lost).wait_for_manager_apply(
            {"status": "accepted", "apply_id": 1, "apply_generation": 1}
        )
    assert error.value.code == "manager_unavailable"
    assert lost.reads == 3


def test_a_terminal_manager_apply_reports_its_retained_failure() -> None:
    class _Failed:
        def components(self) -> list[dict[str, Any]]:
            return []  # an empty installed cell

        def manager_apply_status(
            self, _apply_id: int, _generation: int, *, application_start_id: str
        ) -> dict[str, Any]:
            return {"ok": True, "state": "unknown_effect", "error": None,
                    "failure": {"code": "manager_apply_fence_failed", "message": "fence"}}

    with pytest.raises(ApplyStepFailed) as failed:
        _ports(_Failed()).wait_for_manager_apply(
            {"status": "accepted", "apply_id": 1, "apply_generation": 1}
        )
    assert failed.value.code == "manager_apply_fence_failed"


@pytest.mark.parametrize("code", ["apply_progression_failed"])
def test_a_manager_apply_ended_by_the_manager_is_an_unknown_effect_failure(code: str) -> None:
    # OD-188 P1: the existing terminal state, the Manager's own typed code
    # (the phase code and, without a deeper cause, the cause code too).
    class _Ended:
        def components(self) -> list[dict[str, Any]]:
            return []

        def manager_apply_status(
            self, _apply_id: int, _generation: int, *, application_start_id: str
        ) -> dict[str, Any]:
            return {"ok": True, "state": "unknown_effect", "error": None,
                    "failure": {"code": code, "cause_code": code, "cause_details": {},
                                "effect_disposition": "unknown",
                                "effect_boundary": "unknown",
                                "message": "the Apply ended"}}

    with pytest.raises(ApplyStepFailed) as failed:
        _ports(_Ended()).wait_for_manager_apply(
            {"status": "accepted", "apply_id": 1, "apply_generation": 1}
        )
    assert failed.value.code == code
    assert failed.value.before_effect is False


def test_a_host_refusal_reports_its_typed_cause_on_the_component() -> None:
    # The Manager's phase code wraps the host's composition refusal, which
    # wraps the typed cause (failure record).
    failure = {
        "code": "configuration_preservation_failed",
        "cause_code": "scenario_composition_refused",
        "message": "Scenario refused: component lite6_robot: no site settings file "
                   "is configured for this machine",
        "cause_details": {
            "cause": "component lite6_robot: no site settings file is configured "
                     "for this machine",
            "cause_code": "site_settings_unavailable",
            "component_id": "lite6_robot",
        },
    }

    class _Refused:
        def components(self) -> list[dict[str, Any]]:
            return []  # an empty installed cell

        def manager_apply_status(
            self, _apply_id: int, _generation: int, *, application_start_id: str
        ) -> dict[str, Any]:
            return {"ok": True, "state": "failed", "error": None, "failure": failure}

    with pytest.raises(ApplyStepFailed) as failed:
        _ports(_Refused()).wait_for_manager_apply(
            {"status": "accepted", "apply_id": 1, "apply_generation": 1}
        )
    assert failed.value.code == "site_settings_unavailable"
    assert failed.value.component_id == "lite6_robot"
    assert str(failed.value) == (
        "component lite6_robot: no site settings file is configured for this machine"
    )

    # run_apply reports it on the named component's row and as the Apply's error.
    cell = _FakeCell({"lite6": "prepared"}, _impact(lite6="remove", lite6_robot="add"))

    def refuse() -> None:
        raise failed.value

    cell.apply = refuse  # type: ignore[method-assign]
    result = run_apply(cell)
    rows = {row["component_id"]: row for row in result["components"]}
    assert rows["lite6_robot"]["error"]["code"] == "site_settings_unavailable"
    assert result["error"] == rows["lite6_robot"]["error"]
    assert "error" not in rows["lite6"]


def test_a_typed_manager_cause_wins_over_its_phase_code() -> None:
    class _Refused:
        def components(self) -> list[dict[str, Any]]:
            return []  # an empty installed cell

        def manager_apply_status(
            self, _apply_id: int, _generation: int, *, application_start_id: str
        ) -> dict[str, Any]:
            return {"ok": True, "state": "failed", "error": None, "failure": {
                "code": "configuration_preservation_failed",
                "cause_code": "apply_requires_inactive_component",
                "message": "robot is enabled", "cause_details": {}}}

    with pytest.raises(ApplyStepFailed) as failed:
        _ports(_Refused()).wait_for_manager_apply(
            {"status": "accepted", "apply_id": 1, "apply_generation": 1}
        )
    assert failed.value.code == "apply_requires_inactive_component"
    assert failed.value.component_id is None


def test_the_apply_ports_carry_no_step_limit() -> None:
    # OD-214 (T34): each step waits for its result, not a lifecycle timeout.
    from manyforge_composer.backend.apply_orchestration import ApplyPorts

    assert not hasattr(_ports(None), "lifecycle_timeout_s")
    assert not hasattr(ApplyPorts, "lifecycle_timeout_s")


class _SceneClearClient:
    """Cycle status reads, each with the Headless Scene clear it shows."""

    def components(self) -> list[dict[str, Any]]:
        return []  # an empty installed cell

    def __init__(self, *clears: dict[str, Any] | None) -> None:
        self.reads = [{"state": "idle_after_reset", "scene_clear": clear} for clear in clears]

    def program_cycle_status(self) -> dict[str, Any]:
        return self.reads.pop(0)


_ACCEPTED_EMPTY = {
    "status": "accepted", "apply_id": 1, "apply_generation": 2,
    "manager_apply": {"ok": True, "state": "pending", "replay_status": "accepted_new"},
}


def test_an_empty_cell_apply_returns_once_its_scene_clear_ended() -> None:
    # New (and the New inside Load): Headless clears the Scene after the
    # commit, holding the authoring admission; the Apply waits for this
    # Apply's clear to end, so the next Load's preparation is admitted.
    client = _SceneClearClient(
        {"apply_id": 1, "apply_generation": 1, "state": "succeeded"},  # earlier Apply
        None,
        {"apply_id": 1, "apply_generation": 2, "state": "pending"},
        {"apply_id": 1, "apply_generation": 2, "state": "partial"},
        {"apply_id": 1, "apply_generation": 2, "state": "succeeded"},
    )
    _ports(client).wait_for_scene_clear(_ACCEPTED_EMPTY)
    assert len(client.reads) == 1  # returned at the first ended clear


def test_the_scene_clear_wait_ends_when_a_later_apply_replaced_its_status() -> None:
    # OD-239: `scene_clear` holds one status; the Manager numbers Applies in
    # increasing order, so a status naming a later Apply means this one's
    # clear has ended (its own end may have been replaced unread).
    client = _SceneClearClient(
        {"apply_id": 1, "apply_generation": 2, "state": "pending"},
        {"apply_id": 2, "apply_generation": 1, "state": "pending"},
        {"apply_id": 2, "apply_generation": 1, "state": "succeeded"},
    )
    _ports(client).wait_for_scene_clear(_ACCEPTED_EMPTY)
    assert len(client.reads) == 1


@pytest.mark.parametrize("state,details,logged", [
    ("unknown", {"cause_code": "manager_command_unavailable", "cause_details": {"reason": "timeout"}}, True),
    ("unknown", {}, False),
    ("succeeded", {"cause_code": "manager_command_unavailable"}, False),
])
def test_scene_clear_cause_warning_does_not_change_wait(
    state: str, details: dict[str, Any], logged: bool,
    caplog: pytest.LogCaptureFixture,
) -> None:
    clear = {"apply_id": 1, "apply_generation": 2, "state": state,
             "reason": {"details": details}}
    client = _SceneClearClient(clear, {"apply_id": 1, "apply_generation": 2, "state": "pending"})
    assert _ports(client).wait_for_scene_clear(_ACCEPTED_EMPTY) is None
    assert len(client.reads) == 1
    messages = [record.getMessage() for record in caplog.records
                if record.name == "manyforge_composer.backend.routes_deployment_draft"]
    if logged:
        assert messages == [
            "Scene clear after Apply 1/2 outcome unknown: "
            "cause_code=manager_command_unavailable cause_details={'reason': 'timeout'}"
        ]
    else:
        assert messages == []


@pytest.mark.parametrize("result", [
    {"status": "applied", "apply_id": None},  # nothing new was committed
    {**_ACCEPTED_EMPTY, "manager_apply": {"replay_status": "replayed_exact"}},
])
def test_no_scene_clear_wait_without_a_newly_accepted_apply(result: dict[str, Any]) -> None:
    client = _SceneClearClient()
    _ports(client).wait_for_scene_clear(result)  # no read at all


def test_the_scene_clear_wait_has_no_time_limit() -> None:
    # OD-239: Headless publishes an end for every accepted empty Apply; the
    # wait follows it however long the clear takes.
    client = _SceneClearClient(
        *([{"apply_id": 1, "apply_generation": 2, "state": "pending"}] * 500),
        {"apply_id": 1, "apply_generation": 2, "state": "failed"},
        None,
    )
    _ports(client).wait_for_scene_clear(_ACCEPTED_EMPTY)
    assert len(client.reads) == 1


def test_the_scene_clear_wait_ends_when_a_direct_clear_replaced_its_status() -> None:
    # A direct clear's status names no Apply: this Apply's status was
    # replaced (or its clear has not begun; accepted, OD-239).
    client = _SceneClearClient(
        None,
        {"apply_id": 1, "apply_generation": 2, "state": "pending"},
        {"request_key": "c3" * 16, "state": "succeeded", "remaining": [], "reason": None},
        {"apply_id": 1, "apply_generation": 2, "state": "succeeded"},
    )
    _ports(client).wait_for_scene_clear(_ACCEPTED_EMPTY)
    assert len(client.reads) == 1


# History of Apply terminals (Headless `apply_finished`) ------------------------


class _HistoryClient:
    def __init__(self, failure: Exception | None = None) -> None:
        self.recorded: list[dict[str, Any]] = []
        self.failure = failure

    def record_apply_outcome(self, body: dict[str, Any]) -> dict[str, Any]:
        self.recorded.append(body)
        if self.failure is not None:
            raise self.failure
        return {"recorded": True}


def test_an_apply_result_is_recorded_once_with_its_rows() -> None:
    from manyforge_composer.backend.apply_orchestration import recorded_apply

    client = _HistoryClient()
    result = {
        "outcome": "failed",
        "components": [{"component_id": "lite6", "disposition": "add",
                        "error": {"code": "component_launch_not_settled", "message": "m"}}],
        "error": {"code": "component_launch_not_settled", "message": "m"},
    }
    assert recorded_apply(client, "scenario_load", lambda: result) is result
    assert client.recorded == [{"apply_kind": "scenario_load", **result}]


def test_a_refused_or_failed_route_is_recorded_and_still_raised() -> None:
    from fastapi import HTTPException

    from manyforge_composer.backend.apply_orchestration import recorded_apply

    client = _HistoryClient()

    def refused() -> dict[str, Any]:
        raise HTTPException(409, {"code": "apply_in_progress", "message": "busy"})

    def unavailable() -> dict[str, Any]:
        raise HTTPException(503, {"code": "execution_owner_unavailable", "message": "gone"})

    for run in (refused, unavailable):
        with pytest.raises(HTTPException):
            recorded_apply(client, "deployment_apply", run)
    assert [(r["outcome"], r["error"]["code"]) for r in client.recorded] == [
        ("refused", "apply_in_progress"), ("failed", "execution_owner_unavailable")]


def test_a_program_apply_response_records_applied_or_accepted() -> None:
    from fastapi.responses import JSONResponse

    from manyforge_composer.backend.apply_orchestration import recorded_apply

    client = _HistoryClient()
    for status in ("applied", "accepted"):
        recorded_apply(client, "program_apply",
                       lambda: JSONResponse(status_code=200, content={"status": status}))
    assert [r["outcome"] for r in client.recorded] == ["succeeded", "accepted"]


def test_a_lost_history_record_never_changes_the_apply_result() -> None:
    from manyforge_composer.backend.apply_orchestration import recorded_apply

    client = _HistoryClient(failure=RuntimeError("headless gone"))
    assert recorded_apply(client, "scenario_new", lambda: {"outcome": "succeeded",
                                                          "components": []}) == {
        "outcome": "succeeded", "components": []}
    assert recorded_apply(None, "scenario_new", lambda: {"outcome": "succeeded"}) == {
        "outcome": "succeeded"}


# Apply-level rows (OD-214): answered at acceptance, the end in the row ------


_KEY = "a" * 32


def _commands():
    from manyforge_composer.backend.apply_orchestration import ApplyCommands

    return ApplyCommands()


def test_an_apply_answers_at_acceptance_and_its_row_carries_the_steps() -> None:
    import threading

    from manyforge_composer.backend.apply_orchestration import APPLY_COMMAND_REPLY_SCHEMA

    commands = _commands()
    release = threading.Event()
    cell = _FakeCell({"arm": "enabled"}, _impact(arm="replace"),
                     after_apply={"arm": "unprepared"})
    original_apply = cell.apply

    def held_apply() -> None:
        release.wait()  # the Manager Apply is still running when the reply comes
        original_apply()

    cell.apply = held_apply  # type: ignore[method-assign]
    history = _HistoryClient()
    results: list[dict[str, Any]] = []

    def run() -> dict[str, Any]:
        results.append(run_apply(cell))
        return results[-1]

    reply = commands.run(history, "deployment_apply", _KEY, run)
    assert reply["schema_version"] == APPLY_COMMAND_REPLY_SCHEMA
    row = reply["command"]
    assert (row["kind"], row["request_key"], row["end"]) == ("deployment_apply", _KEY, None)
    assert row["accepted_at_ns"] is not None
    assert commands.snapshot()["in_progress"]["request_key"] == _KEY
    assert history.recorded == []  # History is written at the end, once
    release.set()
    ended = commands.ended_row(_KEY)
    assert ended["end"]["outcome"] == "succeeded"
    # A succeeded row keeps its component rows (the panel's success text).
    assert ended["end"]["code"] is None
    assert ended["end"]["details"] == {"components": results[0]["components"]}
    assert results[0]["components"]
    assert ended["step"] == "preparing_components"
    assert ended["step_source"] == {"status": "component_action", "component_id": "arm"}
    assert commands.snapshot() == {"in_progress": None, "ended": [ended]}
    assert [r["outcome"] for r in history.recorded] == ["succeeded"]


def test_the_same_key_again_returns_its_row_and_runs_nothing() -> None:
    commands = _commands()
    runs: list[int] = []

    def run() -> dict[str, Any]:
        runs.append(1)
        return {"outcome": "succeeded", "components": []}

    commands.run(_HistoryClient(), "scenario_new", _KEY, run)
    commands.ended_row(_KEY)
    replay = commands.run(_HistoryClient(), "scenario_new", _KEY, run)
    assert runs == [1]
    assert replay["command"]["end"]["outcome"] == "succeeded"


def test_a_refusal_at_once_keeps_its_reply_and_is_a_refused_row_replayed_as_is() -> None:
    from fastapi import HTTPException

    commands = _commands()
    history = _HistoryClient()
    refusal = HTTPException(409, {"code": "scenario_discard_confirmation_required",
                                  "message": "unsaved"})

    def run() -> dict[str, Any]:
        raise refusal

    with pytest.raises(HTTPException) as first:
        commands.run(history, "scenario_load", _KEY, run)
    assert first.value is refusal
    [row] = commands.snapshot()["ended"]
    assert (row["accepted_at_ns"], row["step"]) == (None, None)
    assert (row["end"]["outcome"], row["end"]["code"]) == (
        "refused", "scenario_discard_confirmation_required")
    with pytest.raises(HTTPException) as again:
        commands.run(history, "scenario_load", _KEY, lambda: pytest.fail("ran again"))
    assert again.value is refusal
    assert [r["outcome"] for r in history.recorded] == ["refused"]


def test_a_run_in_progress_refuses_a_second_apply_at_once() -> None:
    from manyforge_composer.backend.apply_orchestration import ApplyRefused as Refused

    commands = _commands()
    with one_apply_at_a_time():
        def run() -> dict[str, Any]:
            with one_apply_at_a_time():
                return {"outcome": "succeeded", "components": []}

        with pytest.raises(Refused):
            commands.run(_HistoryClient(), "deployment_apply", _KEY, run)
    assert commands.snapshot()["ended"][0]["end"]["code"] == "apply_in_progress"


@pytest.mark.parametrize("code,outcome", [
    ("component_launch_not_settled", "failed"),
    ("component_action_interrupted_by_stop", "interrupted"),
    ("manager_apply_unknown_effect", "unknown"),
])
def test_a_failed_apply_result_ends_its_row(code: str, outcome: str) -> None:
    commands = _commands()
    components = [{"component_id": "arm", "disposition": "add",
                   "error": {"code": code, "message": "m"}}]
    commands.run(_HistoryClient(), "deployment_apply", _KEY, lambda: {
        "outcome": "failed", "components": components,
        "error": {"code": code, "message": "m"}})
    end = commands.ended_row(_KEY)["end"]
    assert (end["outcome"], end["code"]) == (outcome, code)
    assert end["details"] == {"components": components}


def test_a_manager_apply_ended_unknown_effect_ends_the_row_unknown() -> None:
    from manyforge_composer.backend.apply_orchestration import apply_effect_unknown

    commands = _commands()

    def run() -> dict[str, Any]:
        cell = _FakeCell({"arm": "enabled"}, _impact(arm="retain"))
        run_apply(cell)  # accepts the command
        apply_effect_unknown()
        return {"outcome": "failed", "components": [],
                "error": {"code": "host_apply_failed", "message": "m"}}

    commands.run(_HistoryClient(), "scenario_new", _KEY, run)
    end = commands.ended_row(_KEY)["end"]
    assert (end["outcome"], end["code"]) == ("unknown", "host_apply_failed")


def test_a_failure_after_acceptance_ends_the_row_and_is_not_raised_to_the_caller() -> None:
    from fastapi import HTTPException

    commands = _commands()

    def run() -> dict[str, Any]:
        run_apply(_FakeCell({"arm": "enabled"}, _impact(arm="retain")))
        raise HTTPException(503, {"code": "execution_owner_unavailable", "message": "gone",
                                  "details": {"owner": "host"}})

    reply = commands.run(_HistoryClient(), "deployment_revert", _KEY, run)
    assert reply["command"]["accepted_at_ns"] is not None
    end = commands.ended_row(_KEY)["end"]
    assert (end["outcome"], end["code"], end["details"]) == (
        "failed", "execution_owner_unavailable", {"owner": "host"})


def test_a_load_shows_emptying_until_its_new_ended() -> None:
    from manyforge_composer.backend.apply_orchestration import (
        apply_command,
        apply_steps_hidden,
    )

    commands = _commands()
    seen: list[Any] = []

    def run() -> dict[str, Any]:
        apply_command().accept_step = "emptying"  # type: ignore[union-attr]
        with apply_steps_hidden():
            run_apply(_FakeCell({"arm": "enabled"}, _impact(arm="remove"),
                                after_apply={"arm": None}))
            seen.append(commands.snapshot()["in_progress"]["step"])
        run_apply(_FakeCell({}, _impact(arm="add"), after_apply={"arm": "unprepared"}))
        return {"outcome": "succeeded", "components": []}

    commands.run(_HistoryClient(), "scenario_load", _KEY, run)
    ended = commands.ended_row(_KEY)
    assert seen == ["emptying"]
    assert ended["step"] == "preparing_components"


def test_an_untyped_failure_after_acceptance_ends_the_row_unknown() -> None:
    commands = _commands()

    def run() -> dict[str, Any]:
        run_apply(_FakeCell({"arm": "enabled"}, _impact(arm="retain")))
        raise KeyError("lost")

    reply = commands.run(_HistoryClient(), "deployment_apply", _KEY, run)
    assert reply["command"]["accepted_at_ns"] is not None
    end = commands.ended_row(_KEY)["end"]
    assert (end["outcome"], end["code"], end["details"]) == (
        "unknown", "program_runtime_control_outcome_unknown",
        {"command_kind": "deployment_apply", "cause_code": "KeyError"})


def test_an_untyped_failure_before_acceptance_is_raised_and_ends_unknown() -> None:
    commands = _commands()
    failure = KeyError("lost")

    def run() -> dict[str, Any]:
        raise failure

    with pytest.raises(KeyError) as raised:
        commands.run(_HistoryClient(), "scenario_new", _KEY, run)
    assert raised.value is failure
    [row] = commands.snapshot()["ended"]
    assert (row["accepted_at_ns"], row["step"]) == (None, None)
    assert (row["end"]["outcome"], row["end"]["code"]) == (
        "unknown", "program_runtime_control_outcome_unknown")


@pytest.mark.parametrize("status", ["accepted", "applied"])
def test_a_program_apply_ends_succeeded_once_the_manager_accepted(status: str) -> None:
    """Program Apply/Revert reply as before only to the row: the row ends
    when the Manager accepted (History `accepted`) or it was already applied;
    the ended row names the Manager Apply it started."""

    from manyforge_composer.backend.apply_orchestration import (
        apply_accepted,
        apply_step,
        apply_step_source,
    )

    commands = _commands()
    history = _HistoryClient()

    def run() -> dict[str, Any]:
        apply_accepted()
        apply_step("previewing")
        apply_step("applying")
        apply_step_source("applying", {"status": "manager_apply", "apply_id": 4,
                                       "apply_generation": 2})
        return {"status": status, "headless": {}}

    commands.run(history, "program_apply", _KEY, run)
    ended = commands.ended_row(_KEY)
    assert (ended["end"]["outcome"], ended["end"]["code"], ended["end"]["details"]) == (
        "succeeded", None, None)
    assert ended["step"] == "applying"
    assert ended["step_source"] == {"status": "manager_apply", "apply_id": 4,
                                    "apply_generation": 2}
    assert [r["outcome"] for r in history.recorded] == [
        "accepted" if status == "accepted" else "succeeded"]
