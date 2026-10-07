"""The one held-run Draft-edit rule (spec 450 §5.1.1)."""

from __future__ import annotations

from typing import Any

import pytest

from manyforge_headless_api.held_run_authoring import held_run_draft_edit_allowed


def _cycle(
    *,
    run_state: str = "intervention_required",
    motion_held: bool | None = True,
    active_operation_count: int | None = None,
    containment_pending: bool = False,
    offer: bool = True,
) -> dict[str, Any]:
    selections: dict[str, Any] = {
        "selection_revision": 2,
        "groups": [{"group_id": "all", "state": "held", "node_count": 3, "held_node_count": 3}],
    }
    if motion_held is not None:
        selections["motion_held"] = motion_held
    return {
        "state": "running",
        "program_run_id": "run.1",
        "run": {
            "state": run_state,
            "capture": {"program_run_id": "run.1"},
            "active_operation_count": active_operation_count,
        },
        "recovery_offer": {"actions": [{
            "action_id": "retry_motion_from_current_state",
            "enabled": not containment_pending,
            "disabled_reason_code": (
                "failure_pause_containment_unconfirmed" if containment_pending else None
            ),
        }]} if offer else None,
        "runtime_controls": {"execution_selections": selections},
    }


# The live case after an operator Stop: `all` is not `held` (control flow
# keeps running and two motion nodes never started), yet the runner proves
# every effectful node held, so nothing moves and nothing can start.
LIVE_STOPPED_RUN = {
    "state": "running",
    "program_run_id": "run.1",
    "run": {
        "state": "running",
        "capture": {"program_run_id": "run.1"},
        "active_operation_count": None,
        "node_statuses": {
            "parallel": "running", "sequence_a": "running", "sequence_b": "running",
            "move_a_1": "held", "move_b_1": "held", "move_a_2": "idle", "move_b_2": "idle",
        },
    },
    "recovery_offer": None,
    "runtime_controls": {"execution_selections": {
        "selection_revision": 3,
        "motion_held": True,
        "groups": [{"group_id": "all", "state": "ready", "node_count": 7, "held_node_count": 4}],
    }},
}


def _control(manual: list[Any] | None = None) -> dict[str, Any]:
    return {"ok": True, "control_state": {"manual_operations": [] if manual is None else manual}}


def test_live_operator_stop_with_unstarted_motion_and_running_control_flow_is_editable() -> None:
    assert held_run_draft_edit_allowed(LIVE_STOPPED_RUN, _control()) is True


@pytest.mark.parametrize(
    "cycle",
    [
        _cycle(),  # failure pause, Stop of other motion confirmed
        _cycle(run_state="running"),  # operator Stop, or Executed but never Run
        _cycle(run_state="running", offer=False),
        _cycle(run_state="full_stopped", motion_held=None, active_operation_count=0),
    ],
)
@pytest.mark.parametrize(
    "manual",
    [
        [],
        # Released terminal results and the owner's never-dispatched proof.
        [{"resident": False, "operation": {"state": "completed"}}],
        [{"resident": False, "operation": {"state": "stopped"}}],
        [{"resident": False, "operation": {"state": "failed"}}],
        [{"resident": True, "operation": {"state": "cancelled_pre_dispatch"}}],
        [{"resident": False, "operation": {"state": "cancelled_pre_dispatch"}}],
    ],
)
def test_held_run_with_all_motion_stopped_is_editable(cycle, manual) -> None:
    assert held_run_draft_edit_allowed(cycle, _control(manual)) is True


@pytest.mark.parametrize(
    "cycle, manual",
    [
        # Some effectful node is released, stopping, faulted or unknown.
        (_cycle(motion_held=False), []),
        (_cycle(run_state="running", motion_held=False), []),
        (_cycle(motion_held=None), []),
        ({**_cycle(), "runtime_controls": None}, []),
        (_cycle(containment_pending=True), []),
        (_cycle(run_state="full_stopping", active_operation_count=1), []),
        (_cycle(run_state="full_stopped", active_operation_count=None), []),
        (_cycle(run_state="stopping"), []),
        ({**_cycle(), "state": "stopping"}, []),
        ({**_cycle(), "program_run_id": "run.2"}, []),
        (_cycle(), [{"resident": True, "operation": {"state": "executing"}}]),
        # Not affirmatively settled: effect unproven, authority retained,
        # or a state the owner did not (recognizably) publish.
        (_cycle(), [{"resident": True, "operation": {"state": "intervention_required"}}]),
        (_cycle(), [{"resident": True, "operation": {"state": "interrupted"}}]),
        (_cycle(), [{"resident": False, "operation": {"state": "interrupted"}}]),
        (_cycle(), [{"resident": False, "operation": {"state": "intervention_required"}}]),
        (_cycle(), [{"resident": True, "operation": {"state": "completed"}}]),
        (_cycle(), [{"resident": False, "operation": {"state": "mystery"}}]),
        (_cycle(), [{"resident": False, "operation": {}}]),
        (_cycle(), [{"operation": {"state": "completed"}}]),
        (_cycle(), [{"resident": False}]),
        (_cycle(), [{"operation": {"state": "cancelled_pre_dispatch"}}]),
        (_cycle(offer=False), []),  # failure pause unproven
    ],
)
def test_moving_settling_or_unknown_run_is_not_editable(cycle, manual) -> None:
    assert held_run_draft_edit_allowed(cycle, _control(manual)) is False


def test_missing_owner_facts_are_not_editable() -> None:
    assert held_run_draft_edit_allowed(None, _control()) is False
    assert held_run_draft_edit_allowed(_cycle(), None) is False
    assert held_run_draft_edit_allowed(_cycle(), {"ok": True, "control_state": {}}) is False
