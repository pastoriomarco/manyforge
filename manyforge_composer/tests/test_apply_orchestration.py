"""Server-side Apply orchestration order with a fake host."""

from typing import Any

import pytest

from manyforge_composer.backend.apply_orchestration import (
    DEFAULT_LIFECYCLE_TIMEOUT_S,
    HOST_COMPONENT_STOP_GRACE_S,
    ApplyRefused,
    ApplyStepFailed,
    ComponentImpact,
    Preview,
    candidate_preview,
    lifecycle_timeouts,
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
        silent: frozenset[str] = frozenset(),
    ) -> None:
        self.silent = silent  # components whose action result never comes back
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

    def cycle_state(self) -> str:
        return self.cycle

    def run_held(self) -> bool:
        return self.held

    def reset_cycle(self) -> None:
        self.calls.append(("reset",))
        self.cycle = "idle_after_reset"

    def lifecycle_timeout_s(self, component_id: str) -> float:
        return 0.0 if component_id in self.silent else DEFAULT_LIFECYCLE_TIMEOUT_S

    def preview(self) -> Preview:
        self.calls.append(("preview",))
        return Preview(components=tuple(self.impact))

    def components(self) -> list[dict[str, Any]]:
        rows = []
        for component_id, lifecycle in self.lifecycles.items():
            row: dict[str, Any] = {"component_id": component_id, "lifecycle": lifecycle}
            if component_id in self.owners:
                row["owner_id"] = self.owners[component_id]
            if component_id in self._results:
                row["last_action"] = self._results[component_id]
            rows.append(row)
        return rows

    def component_action(self, component_id: str, action: str) -> None:
        self.calls.append((action, component_id))
        if component_id in self.silent:
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
@pytest.mark.parametrize("cycle", ["running", "starting", "stopping", "resetting"])
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


def test_a_step_without_a_result_ends_unknown_after_its_lifecycle_timeout() -> None:
    cell = _FakeCell(
        {"robot": "enabled"}, _impact(robot="replace"), silent=frozenset({"robot"})
    )
    result = run_apply(cell)
    assert cell.calls == [("preview",), ("disable", "robot")]  # never resent
    assert result["outcome"] == "failed"
    assert result["error"]["code"] == "component_action_outcome_unknown"


def test_a_prepare_after_launch_is_bounded_like_the_host(monkeypatch: pytest.MonkeyPatch) -> None:
    # The host's Prepare of a just-launched component first waits for its
    # launch to settle within the launch bound (the lifecycle timeouts of
    # every added, replaced or removed component), then its own timeout; a
    # disable before the Apply keeps its own timeout.
    import manyforge_composer.backend.apply_orchestration as orchestration

    bounds: list[tuple[str, str, float]] = []
    real_wait = orchestration.wait_for_action

    def record(read_states, component_id, action, previous, *, timeout_s):  # type: ignore[no-untyped-def]
        bounds.append((action, component_id, timeout_s))
        return real_wait(read_states, component_id, action, previous, timeout_s=timeout_s)

    monkeypatch.setattr(orchestration, "wait_for_action", record)
    cell = _FakeCell(
        {"arm": "enabled", "camera": "enabled"},
        _impact(arm="replace", camera="retain", gripper="add", old="remove"),
        after_apply={"arm": "not_prepared", "gripper": "not_prepared"},
    )
    assert run_apply(cell)["outcome"] == "succeeded"
    launch = 3 * DEFAULT_LIFECYCLE_TIMEOUT_S  # arm, gripper, old; not camera
    assert bounds == [
        ("disable", "arm", DEFAULT_LIFECYCLE_TIMEOUT_S),
        ("prepare", "arm", launch + DEFAULT_LIFECYCLE_TIMEOUT_S),
        ("prepare", "gripper", launch + DEFAULT_LIFECYCLE_TIMEOUT_S),
    ]


def test_one_apply_at_a_time() -> None:
    with one_apply_at_a_time():
        with pytest.raises(ApplyRefused) as refused:
            with one_apply_at_a_time():
                pass
    assert refused.value.code == "apply_in_progress"
    with one_apply_at_a_time():  # released again
        pass


def test_lifecycle_timeouts_read_the_authored_component_budget() -> None:
    deployment = {
        "components": [
            {"component_id": "plc", "configuration": {"lifecycle_command_timeout_s": 30}},
            {"component_id": "robot", "configuration": {}},
        ]
    }
    assert lifecycle_timeouts(deployment) == {"plc": 30.0}
    assert lifecycle_timeouts(None) == {}


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


def test_members_of_an_owner_whose_prepare_failed_are_not_attempted() -> None:
    cell = _FakeCell(
        {},
        _impact(machine="add", arm="add", io="add", camera="add"),
        after_apply={
            "machine": "not_prepared", "arm": "not_prepared",
            "io": "not_prepared", "camera": "not_prepared",
        },
        owners={"arm": "machine", "io": "machine"},
        fail={("machine", "prepare"): "fault_estop"},
    )
    result = run_apply(cell)
    assert cell.calls == [
        ("preview",), ("apply",), ("prepare", "machine"), ("prepare", "camera"),
    ]
    rows = {row["component_id"]: row for row in result["components"]}
    assert rows["machine"]["error"]["code"] == "fault_estop"
    assert rows["arm"]["error"]["code"] == "owner_not_prepared"
    assert rows["io"]["error"]["code"] == "owner_not_prepared"
    assert "error" not in rows["camera"]
    assert result["error"]["code"] == "fault_estop"


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


class _CycleClient:
    """A Runner that finishes a Reset after one `resetting` read."""

    def components(self) -> list[dict[str, Any]]:
        return []  # an empty installed cell

    def __init__(self, after_reset: dict[str, Any], *, first: dict[str, Any] | None = None) -> None:
        self.reads = [
            first or {"state": "terminal_awaiting_reset", "cycle_generation": 4,
                      "program_run_id": "run.1", "program_runner_owner_epoch": "runner.1",
                      "cycle_claim_generation": 2,
                      "terminal_or_settlement_reference": "sha256:" + "a" * 64},
            {"state": "resetting", "cycle_generation": 4},
            after_reset,
        ]
        self.resets: list[dict[str, Any]] = []

    def program_cycle_status(self) -> dict[str, Any]:
        return self.reads.pop(0)

    def program_cycle_reset(self, body: dict[str, Any]) -> dict[str, Any]:
        self.resets.append(body)
        return {}

    def authoring_admission(self) -> dict[str, Any]:
        return {"authoring_allowed": True, "admission_generation": 7}


def _ports(client: Any, deployment: dict[str, Any] | None = None):
    from types import SimpleNamespace

    from manyforge_composer.backend.routes_deployment_draft import HeadlessApplyPorts

    ports = HeadlessApplyPorts(
        SimpleNamespace(headless_client=client, applied_deployment_document=None),  # type: ignore[arg-type]
        deployment,
    )
    ports._POLL_SECONDS = 0.0
    return ports


def test_the_apply_bound_counts_every_component_like_the_manager() -> None:
    # New after a Scenario: the empty candidate removes every Applied
    # component; each one without an authored timeout takes the default (the
    # Manager's `HostApplyTimeout`, the host's stop grace per component).
    from types import SimpleNamespace

    from manyforge_composer.backend.routes_deployment_draft import HeadlessApplyPorts

    class _Installed:
        def __init__(self, ids: list[str]) -> None:
            self.ids = ids

        def components(self) -> list[dict[str, Any]]:
            return [{"component_id": component_id} for component_id in self.ids]

    applied = {"components": [
        {"component_id": "plc", "configuration": {"lifecycle_command_timeout_s": 4.0}},
    ]}
    ports = HeadlessApplyPorts(
        SimpleNamespace(headless_client=_Installed(["arm", "gripper", "plc"]),  # type: ignore[arg-type]
                        applied_deployment_document=applied),
        {"components": []},
    )
    # Each component also gets the host's stop grace (a hung launch).
    grace = HOST_COMPONENT_STOP_GRACE_S
    assert ports._apply_bound_s() == 2 * (DEFAULT_LIFECYCLE_TIMEOUT_S + grace) + 4.0 + grace
    # The Applied document not verified yet: the installed cell still counts.
    unverified = HeadlessApplyPorts(
        SimpleNamespace(headless_client=_Installed(["arm", "gripper"]),  # type: ignore[arg-type]
                        applied_deployment_document=None),
        {"components": []},
    )
    assert unverified._apply_bound_s() == 2 * (DEFAULT_LIFECYCLE_TIMEOUT_S + grace)
    assert _ports(_Installed([]))._apply_bound_s() == DEFAULT_LIFECYCLE_TIMEOUT_S


def test_the_apply_leaves_the_admission_wait_to_the_preparation() -> None:
    # One wait in one place. An idle cycle goes straight to the Preview;
    # Headless's activation preparation waits for a momentarily closed
    # admission, the Composer reads none.
    from manyforge_composer.backend.apply_orchestration import ApplyPorts

    assert not hasattr(ApplyPorts, "await_authoring")
    cell = _FakeCell({"arm": "enabled"}, _impact(arm="retain"))
    run_apply(cell)
    assert cell.calls[0] == ("preview",)


def test_a_lone_hung_component_fits_the_apply_bound() -> None:
    # New after a one-arm cell whose arm node hangs: the host waits its
    # stop grace, then kills and reaps the group; the wait must cover it.
    from types import SimpleNamespace

    from manyforge_composer.backend.routes_deployment_draft import HeadlessApplyPorts

    class _Installed:
        def components(self) -> list[dict[str, Any]]:
            return [{"component_id": "arm"}]

    ports = HeadlessApplyPorts(
        SimpleNamespace(headless_client=_Installed(), applied_deployment_document=None),  # type: ignore[arg-type]
        {"components": []},
    )
    assert ports._apply_bound_s() > HOST_COMPONENT_STOP_GRACE_S + 1.0


def test_the_stop_grace_mirrors_the_hosts_constant() -> None:
    # One source: manyforge::core::kControllerLaunchStopGrace.
    import re
    from pathlib import Path

    header = Path(__file__).resolve().parents[2] / (
        "manyforge_core/include/manyforge_core/types/controller_launch_group_drain.hpp")
    match = re.search(r"kControllerLaunchStopGrace\{([0-9']+)\}", header.read_text())
    assert match is not None
    assert int(match.group(1).replace("'", "")) / 1000.0 == HOST_COMPONENT_STOP_GRACE_S


def test_the_server_resets_a_finished_run_like_the_program_tab() -> None:
    client = _CycleClient({"state": "idle_after_reset", "cycle_generation": 5})
    _ports(client).reset_cycle()
    [reset] = client.resets
    assert reset["program_run_id"] == "run.1"
    assert reset["expected_owner_epoch"] == "runner.1"
    assert reset["expected_cycle_generation"] == 4
    assert reset["expected_claim_generation"] == 2
    assert reset["terminal_reference"] == "sha256:" + "a" * 64
    assert (reset["reload_initial_scene"], reset["reset_robot_position"]) == (False, False)


def test_the_server_resets_a_held_run_like_the_program_tab() -> None:
    client = _CycleClient(
        {"state": "idle_after_reset", "cycle_generation": 5},
        first={"state": "running", "cycle_generation": 4, "program_run_id": "run.1",
               "program_runner_owner_epoch": "runner.1", "cycle_claim_generation": 2,
               "terminal_or_settlement_reference": None},
    )
    # The Reset may still read `running` before `resetting`.
    client.reads.insert(1, {"state": "running", "cycle_generation": 4})
    _ports(client).reset_cycle()
    [reset] = client.resets
    assert reset["program_run_id"] == "run.1"
    assert reset["expected_claim_generation"] == 2
    assert reset["terminal_reference"] is None


def test_a_failed_reset_ends_the_apply_with_its_code() -> None:
    client = _CycleClient({
        "state": "recovery_required", "cycle_generation": 4,
        "failure": {"code": "program_cycle_reset_failed", "message": "Reset failed"},
    })
    with pytest.raises(ApplyStepFailed) as failed:
        _ports(client).reset_cycle()
    assert failed.value.code == "program_cycle_reset_failed"


def test_the_reset_returns_once_the_cycle_is_idle() -> None:
    # The Runner's Idle reaches the Kernel admission a moment later; the
    # Apply's preparation waits for it (Headless), the Reset doesn't.
    class _NoAdmissionRead(_CycleClient):
        def authoring_admission(self) -> dict[str, Any]:
            raise AssertionError("the Reset reads no admission")

    client = _NoAdmissionRead({"state": "idle_after_reset", "cycle_generation": 5})
    _ports(client).reset_cycle()
    assert client.reads == []


def test_the_reset_wait_is_bounded(monkeypatch: pytest.MonkeyPatch) -> None:
    import manyforge_composer.backend.routes_deployment_draft as routes

    class _NeverResets(_CycleClient):
        def program_cycle_status(self) -> dict[str, Any]:
            return dict(self.reads[0])  # the Reset is accepted, the cycle never moves

    monkeypatch.setattr(routes, "DEFAULT_LIFECYCLE_TIMEOUT_S", 0.0)
    client = _NeverResets({})
    with pytest.raises(ApplyStepFailed) as failed:
        _ports(client).reset_cycle()
    assert failed.value.code == "program_cycle_reset_failed"
    assert len(client.resets) == 1


def test_the_manager_apply_wait_is_bounded(monkeypatch: pytest.MonkeyPatch) -> None:
    import manyforge_composer.backend.routes_deployment_draft as routes

    class _Pending:
        def components(self) -> list[dict[str, Any]]:
            return []  # an empty installed cell

        def manager_apply_status(self, _apply_id: str, _generation: int) -> dict[str, Any]:
            return {"ok": True, "state": "running"}

    monkeypatch.setattr(routes, "DEFAULT_LIFECYCLE_TIMEOUT_S", 0.0)
    with pytest.raises(ApplyStepFailed) as unknown:
        _ports(_Pending()).wait_for_manager_apply(
            {"status": "accepted", "apply_id": "a", "apply_generation": 1}
        )
    assert unknown.value.code == "manager_apply_unknown_effect"


def test_a_terminal_manager_apply_reports_its_retained_failure() -> None:
    class _Failed:
        def components(self) -> list[dict[str, Any]]:
            return []  # an empty installed cell

        def manager_apply_status(self, _apply_id: str, _generation: int) -> dict[str, Any]:
            return {"ok": True, "state": "unknown_effect", "error": None,
                    "failure": {"code": "manager_apply_fence_failed", "message": "fence"}}

    with pytest.raises(ApplyStepFailed) as failed:
        _ports(_Failed()).wait_for_manager_apply(
            {"status": "accepted", "apply_id": "a", "apply_generation": 1}
        )
    assert failed.value.code == "manager_apply_fence_failed"


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

        def manager_apply_status(self, _apply_id: str, _generation: int) -> dict[str, Any]:
            return {"ok": True, "state": "failed", "error": None, "failure": failure}

    with pytest.raises(ApplyStepFailed) as failed:
        _ports(_Refused()).wait_for_manager_apply(
            {"status": "accepted", "apply_id": "a", "apply_generation": 1}
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

        def manager_apply_status(self, _apply_id: str, _generation: int) -> dict[str, Any]:
            return {"ok": True, "state": "failed", "error": None, "failure": {
                "code": "configuration_preservation_failed",
                "cause_code": "apply_requires_inactive_component",
                "message": "robot is enabled", "cause_details": {}}}

    with pytest.raises(ApplyStepFailed) as failed:
        _ports(_Refused()).wait_for_manager_apply(
            {"status": "accepted", "apply_id": "a", "apply_generation": 1}
        )
    assert failed.value.code == "apply_requires_inactive_component"
    assert failed.value.component_id is None


def test_each_step_uses_the_component_lifecycle_timeout() -> None:
    ports = _ports(None, {"components": [
        {"component_id": "plc", "configuration": {"lifecycle_command_timeout_s": 30}},
    ]})
    assert ports.lifecycle_timeout_s("plc") == 30.0
    assert ports.lifecycle_timeout_s("robot") == DEFAULT_LIFECYCLE_TIMEOUT_S


class _SceneClearClient:
    """Cycle status reads, each with the Headless Scene clear it shows."""

    def components(self) -> list[dict[str, Any]]:
        return []  # an empty installed cell

    def __init__(self, *clears: dict[str, Any] | None) -> None:
        self.reads = [{"state": "idle_after_reset", "scene_clear": clear} for clear in clears]

    def program_cycle_status(self) -> dict[str, Any]:
        return self.reads.pop(0)


_ACCEPTED_EMPTY = {
    "status": "accepted", "apply_id": "a", "apply_generation": 2,
    "manager_apply": {"ok": True, "state": "pending", "replay_status": "accepted_new"},
}


def test_an_empty_cell_apply_returns_once_its_scene_clear_ended() -> None:
    # New (and the New inside Load): Headless clears the Scene after the
    # commit, holding the authoring admission; the Apply waits for this
    # Apply's clear to end, so the next Load's preparation is admitted.
    client = _SceneClearClient(
        {"apply_id": "a", "apply_generation": 1, "state": "succeeded"},  # earlier Apply
        None,
        {"apply_id": "a", "apply_generation": 2, "state": "pending"},
        {"apply_id": "a", "apply_generation": 2, "state": "partial"},
        {"apply_id": "a", "apply_generation": 2, "state": "succeeded"},
    )
    _ports(client).wait_for_scene_clear(_ACCEPTED_EMPTY)
    assert len(client.reads) == 1  # returned at the first ended clear


@pytest.mark.parametrize("state,details,logged", [
    ("unknown", {"cause_code": "manager_command_unavailable", "cause_details": {"reason": "timeout"}}, True),
    ("unknown", {}, False),
    ("succeeded", {"cause_code": "manager_command_unavailable"}, False),
])
def test_scene_clear_cause_warning_does_not_change_wait(
    state: str, details: dict[str, Any], logged: bool,
    caplog: pytest.LogCaptureFixture,
) -> None:
    clear = {"apply_id": "a", "apply_generation": 2, "state": state,
             "reason": {"details": details}}
    client = _SceneClearClient(clear, {"apply_id": "a", "apply_generation": 2, "state": "pending"})
    assert _ports(client).wait_for_scene_clear(_ACCEPTED_EMPTY) is None
    assert len(client.reads) == 1
    messages = [record.getMessage() for record in caplog.records
                if record.name == "manyforge_composer.backend.routes_deployment_draft"]
    if logged:
        assert messages == [
            "Scene clear after Apply a/2 outcome unknown: "
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


def test_the_scene_clear_wait_is_bounded(monkeypatch: pytest.MonkeyPatch) -> None:
    import manyforge_composer.backend.routes_deployment_draft as routes

    class _Pending:
        def components(self) -> list[dict[str, Any]]:
            return []  # an empty installed cell

        def program_cycle_status(self) -> dict[str, Any]:
            return {"scene_clear": {"apply_id": "a", "apply_generation": 2, "state": "pending"}}

    monkeypatch.setattr(routes, "DEFAULT_LIFECYCLE_TIMEOUT_S", 0.0)
    _ports(_Pending()).wait_for_scene_clear(_ACCEPTED_EMPTY)  # returns as it is


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
