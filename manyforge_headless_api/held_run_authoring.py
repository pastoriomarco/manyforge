"""Whether a held Program run leaves the Draft editable.

The one rule, computed here and nowhere else: Headless publishes it in the
control-state shell (``held_run_draft_edit_allowed``) and Composer applies the
same function to every Draft edit it admits.

While a Program run exists the Draft is editable only when nothing is moving
and nothing can start. The Program runner's ``motion_held`` fact proves both
for a running or failure-paused run: every effectful node is held by the
Kernel, either never released (Executed but not Run) or stopped with its Stop
certified, and the Kernel admits a Program operation only for a released node.
Control-flow nodes never move, so they do not count. A failure pause also
needs its published recovery offer with confirmed containment; a settled Halt
(Full Stopped) needs no active operation. Every manual operation must be
affirmatively settled. An unavailable, unknown or settling fact means "not
editable". A Draft edit has no external effect, so this grants nothing beyond
the edit itself: Apply, Execute and every other command keep their own
admission.
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


def _program_motion_held(controls: Any) -> bool:
    """The runner's fact: every effectful node is held in a settled phase."""

    selections = (
        controls.get("execution_selections") if isinstance(controls, Mapping) else None
    )
    return isinstance(selections, Mapping) and selections.get("motion_held") is True


def _containment_unconfirmed(offer: Any) -> bool:
    actions = offer.get("actions") if isinstance(offer, Mapping) else None
    return isinstance(actions, list) and any(
        isinstance(action, Mapping)
        and action.get("disabled_reason_code") == "failure_pause_containment_unconfirmed"
        for action in actions
    )


def held_run_draft_edit_allowed(cycle: Any, control_response: Any) -> bool:
    """All motion of the existing run is stopped and none can start."""

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
        control_response.get("control_state")
        if isinstance(control_response, Mapping)
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
    offer = cycle.get("recovery_offer")
    if state == "intervention_required" and not (
        isinstance(offer, Mapping) and isinstance(offer.get("actions"), list)
    ):
        # A failure pause proves its containment only through its published
        # recovery offer; a missing offer is unknown, not stopped.
        return False
    return _program_motion_held(cycle.get("runtime_controls")) and not (
        _containment_unconfirmed(offer)
    )


__all__ = ["held_run_draft_edit_allowed", "manual_operation_settled"]
