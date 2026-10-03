"""Whether a held Program run leaves the Draft editable.

The Draft is editable while a Program run exists only when every owner
confirms that all of the run's motion is stopped: a failure pause whose Stop
of the Program's other motion is confirmed, an operator Stop that holds the
whole Program, a settled Halt, or a run that was Executed but never Run (every
execution group still ``ready``), and every manual operation affirmatively
settled.
The facts are the Program runner's and Kernel's existing projections; an
unavailable, unknown or settling fact means "not editable". A Draft edit has
no external effect, so this grants nothing beyond the edit itself: Apply,
Execute and every other command keep their own admission.
"""

from __future__ import annotations

from typing import Any, Mapping

# Manual-operation results that affirmatively settle motion. The Kernel's
# control-state row carries only the operation state and ``resident`` (True
# while the Kernel retains any authority over it), so settlement is proven
# only by a released (non-resident) terminal result, or by the owner's own
# never-dispatched proof (``cancelled_pre_dispatch``). ``interrupted``,
# ``intervention_required``, any in-progress, missing or unknown state is not
# settled: its effect is not proven absent.
_MANUAL_SETTLED_RELEASED = frozenset({"completed", "stopped", "failed"})
_MANUAL_NEVER_DISPATCHED = "cancelled_pre_dispatch"


def manual_operation_settled(row: Any) -> bool:
    """One ``control_state.manual_operations`` row affirmatively settled."""

    operation = row.get("operation") if isinstance(row, Mapping) else None
    if not isinstance(operation, Mapping):
        return False
    state = operation.get("state")
    if state == _MANUAL_NEVER_DISPATCHED:
        return isinstance(row.get("resident"), bool)
    return row.get("resident") is False and state in _MANUAL_SETTLED_RELEASED


def _all_motion_held(controls: Any) -> bool:
    """The whole-Program selection is held by a confirmed Stop."""

    selections = (
        controls.get("execution_selections") if isinstance(controls, Mapping) else None
    )
    groups = selections.get("groups") if isinstance(selections, Mapping) else None
    if not isinstance(groups, list):
        return False
    whole = [
        group
        for group in groups
        if isinstance(group, Mapping) and group.get("group_id") == "all"
    ]
    if len(whole) != 1:
        return False
    group = whole[0]
    state = group.get("state")
    node_count = group.get("node_count")
    return state == "held" or (
        state == "mixed"
        and type(node_count) is int
        and node_count > 0
        and group.get("held_node_count") == node_count
    )


def _never_run(cycle: Mapping[str, Any]) -> bool:
    """The run was Executed but no motion was ever started (maintainer,
    2026-10-02).

    The Program runner publishes a group as ``ready`` only while the Kernel
    still holds every effectful node of the group with its initial
    never-released cause and no node of it was ever stopped. With every group
    ``ready`` no effectful node was ever released, so the Kernel accepted no
    operation for the run. A published recovery offer means some node already
    failed: not this state.
    """

    if cycle.get("recovery_offer") is not None:
        return False
    controls = cycle.get("runtime_controls")
    selections = (
        controls.get("execution_selections") if isinstance(controls, Mapping) else None
    )
    groups = selections.get("groups") if isinstance(selections, Mapping) else None
    return (
        isinstance(groups, list)
        and len(groups) > 0
        and all(
            isinstance(group, Mapping) and group.get("state") == "ready"
            for group in groups
        )
    )


def _containment_unconfirmed(offer: Any) -> bool:
    actions = offer.get("actions") if isinstance(offer, Mapping) else None
    return isinstance(actions, list) and any(
        isinstance(action, Mapping)
        and action.get("disabled_reason_code") == "failure_pause_containment_unconfirmed"
        for action in actions
    )


def held_run_motion_stopped(cycle: Any, control_state: Any) -> bool:
    """All motion of the existing run is confirmed stopped (see module doc)."""

    if not isinstance(cycle, Mapping) or cycle.get("state") != "running":
        return False
    run = cycle.get("run")
    capture = run.get("capture") if isinstance(run, Mapping) else None
    if (
        not isinstance(capture, Mapping)
        or cycle.get("program_run_id") is None
        or capture.get("program_run_id") != cycle.get("program_run_id")
    ):
        return False
    control = (
        control_state.get("control_state")
        if isinstance(control_state, Mapping)
        else None
    )
    manual = control.get("manual_operations") if isinstance(control, Mapping) else None
    if not isinstance(manual, list):
        return False
    if not all(manual_operation_settled(row) for row in manual):
        return False
    state = run.get("state")
    if state == "full_stopped":
        return run.get("active_operation_count") == 0
    if state not in {"running", "intervention_required"}:
        return False
    if state == "running" and _never_run(cycle):
        return True
    offer = cycle.get("recovery_offer")
    if state == "intervention_required" and not (
        isinstance(offer, Mapping) and isinstance(offer.get("actions"), list)
    ):
        # A failure pause proves its containment only through its published
        # recovery offer; a missing offer is unknown, not stopped.
        return False
    return _all_motion_held(cycle.get("runtime_controls")) and not (
        _containment_unconfirmed(offer)
    )


def held_run_draft_editable(client: Any) -> bool:
    """Read the owners' current facts once; any failure is "not editable"."""

    try:
        return held_run_motion_stopped(
            client.program_cycle_status(), client.execution_control_state()
        )
    except Exception:
        return False


__all__ = [
    "held_run_draft_editable",
    "held_run_motion_stopped",
    "manual_operation_settled",
]
