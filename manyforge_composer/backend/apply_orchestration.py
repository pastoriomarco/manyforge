"""Server-side Apply orchestration: the one path for Apply,
Load, New and Close, used alike by the GUI, REST and MCP.

For each ``replace``/``remove`` component: ``disable`` → Apply → (``replace``
and ``add``) ``prepare``. Retained components are untouched, except one whose
process had exited: the host starts it again in the Apply, and it is
``prepared`` like a replaced one. A component in
``fault`` or ``offline`` (faulted, or its process is gone) is not disabled:
the host removes it by stopping its processes. Afterwards the affected
components are ``prepared``, never enabled. Machines: owners are
prepared before their members, and members are disabled first. Each step
waits for the component's ``last_action`` (its adapter bounds the action; the
wait has no limit of its own) or its owner-stall fault, which ends the step
``component_owner_stalled``. One Apply at a time. Refused
while a Program run's motion is executing or stopping; a finished run awaiting
Reset, or a run with all its motion held, is reset first; allowed
during a cell latch. Every
step is tried and each failure is reported on its row; the Apply then
ends ``failed``: no retries, no rollback.
"""

from __future__ import annotations

from collections.abc import Callable, Iterator
from contextlib import contextmanager
from contextvars import ContextVar
from copy import deepcopy
from dataclasses import dataclass, field
import json
import logging
import threading
from typing import Any, Protocol, TypeVar

from manyforge_headless_api.component_model import (
    ActionResult,
    ComponentState,
    wait_for_action,
)
from manyforge_headless_api.command_rows import CommandRowTable

# The Runner cycle state in which no Program run is in progress.
_IDLE_CYCLE_STATE = "idle_after_reset"
# A finished run awaiting Reset: the Apply resets it itself.
_FINISHED_CYCLE_STATE = "terminal_awaiting_reset"
# A run whose motion is all held (``run_held``) is reset alike.
_RUNNING_CYCLE_STATE = "running"
_APPLY_SLOT = threading.Lock()


class ApplyRefused(Exception):
    """Refused before any effect (HTTP 409)."""

    def __init__(
        self, code: str, message: str, details: dict[str, str] | None = None
    ) -> None:
        super().__init__(message)
        self.code = code
        # The refusal's structured details (e.g. the busy component), if any.
        self.details = details


class ApplyStepFailed(Exception):
    """A step failed after the Apply started; the Apply ends `failed`."""

    def __init__(
        self,
        code: str,
        message: str,
        component_id: str | None = None,
        *,
        before_effect: bool = False,
    ) -> None:
        super().__init__(message)
        self.code = code
        # The component the failure names, if any: reported on its row.
        self.component_id = component_id
        # The owner stated that this failure made no configuration, runtime,
        # component or Scene change (a Manager ``failed`` terminal whose
        # failure has ``effect_boundary: before_effect``).
        self.before_effect = before_effect


@contextmanager
def one_apply_at_a_time() -> Iterator[None]:
    """Hold the one Apply slot (Apply, Revert, Load, New, Close)."""

    if not _APPLY_SLOT.acquire(blocking=False):
        raise ApplyRefused(
            "apply_in_progress", "Another Apply, Load or New is in progress"
        )
    try:
        yield
    finally:
        _APPLY_SLOT.release()


@dataclass(frozen=True, slots=True)
class ComponentImpact:
    component_id: str
    disposition: str  # add | remove | replace | retain
    reasons: tuple[str, ...]


@dataclass(frozen=True, slots=True)
class Preview:
    """The Manager's impact of one candidate, as the Preview shows it."""

    components: tuple[ComponentImpact, ...]
    program: dict[str, Any] = field(
        default_factory=lambda: {
            "executable": True,
            "unresolved_references": [],
            "affected_nodes": [],
            "affected_execution_groups": [],
        }
    )
    attachment_changed_components: tuple[str, ...] = ()
    detached_components: tuple[str, ...] = ()
    # The cell collision rules the Apply adds, removes or changes (their
    # authored row or their resolved members), as the Manager lists them:
    # ``{rule_id, change, now, was}``.
    collision_rules: tuple[dict[str, Any], ...] = ()
    # Depth rules and trip depths that select a mesh that is not closed
    # (OD-363), as the Manager reports them; a report, not a confirmation:
    # ``{code, collision_rule_id | signal_id, member, mesh_uri}``.
    mesh_depth_reports: tuple[dict[str, Any], ...] = ()

    def collision_rule_ids(self) -> list[str]:
        """The ids of the rules the Apply changes, which it must confirm."""

        return [str(row["rule_id"]) for row in self.collision_rules]


COLLISION_RULE_CONFIRMATION_REQUIRED = "collision_rule_confirmation_required"


def unconfirmed_collision_rules(
    preview: Preview, confirmed: list[str] | None
) -> list[str]:
    """The changed rules of ``preview`` that ``confirmed`` leaves out.

    ``None`` is a caller that is itself the confirmation (a Scenario Load):
    nothing is left out.
    """

    if confirmed is None:
        return []
    return [rule_id for rule_id in preview.collision_rule_ids() if rule_id not in confirmed]


class ApplyPorts(Protocol):
    def cycle_state(self) -> str | None: ...

    def run_held(self) -> bool:
        """The running run has all its motion stopped and held, nothing can
        start (the published held-run rule)."""

    def manual_operations_settled(self) -> bool:
        """Every manual operation is affirmatively settled (the held-run
        rule's manual part): no manual move executes or is being stopped."""

    def reset_cycle(self) -> None:
        """Reset the finished or held run and return once the cycle is idle
        (raise :class:`ApplyStepFailed` otherwise)."""

    def preview(self) -> Preview: ...

    def components(self) -> list[ComponentState]: ...

    def component_action(self, component_id: str, action: str) -> None: ...

    def apply(self) -> None:
        """Submit the Manager Apply and return once it is terminal and
        succeeded (raise :class:`ApplyStepFailed` otherwise)."""


def candidate_preview(response: dict[str, Any] | None) -> Preview:
    """The Preview of a Manager candidate preview (its ``impact``)."""

    impact = response.get("impact") if isinstance(response, dict) else None
    impact = impact if isinstance(impact, dict) else {}
    rows = impact.get("components")
    program = impact.get("program") if isinstance(impact.get("program"), dict) else {}
    outcome = program.get("non_executable")
    outcome = outcome if isinstance(outcome, dict) else {}
    return Preview(
        components=tuple(
            ComponentImpact(
                component_id=str(row["controllable_component_id"]),
                disposition=str(row["disposition"]),
                reasons=tuple(str(reason) for reason in row.get("replacement_reasons") or ()),
            )
            for row in rows or ()
            if isinstance(row, dict)
        ),
        program={
            "executable": program.get("executable") is not False,
            "unresolved_references": [
                deepcopy(row)
                for row in outcome.get("unresolved_targets") or ()
                if isinstance(row, dict)
            ],
            "affected_nodes": list(outcome.get("affected_nodes") or ()),
            "affected_execution_groups": list(
                outcome.get("affected_execution_groups") or ()
            ),
        },
        attachment_changed_components=tuple(
            impact.get("attachment_changed_components") or ()
        ),
        detached_components=tuple(impact.get("detached_components") or ()),
        collision_rules=tuple(
            deepcopy(row)
            for row in impact.get("collision_rules") or ()
            if isinstance(row, dict)
        ),
        mesh_depth_reports=tuple(
            deepcopy(row)
            for row in impact.get("mesh_depth_reports") or ()
            if isinstance(row, dict)
        ),
    )


def preview_view(preview: Preview, scope: dict[str, Any]) -> dict[str, Any]:
    """`POST /api/deployment/preview` response.

    ``scope`` is the activation input the Preview was computed from; the GUI
    sends it back with the Apply, which is refused when the input changed.
    """

    return {
        "components": [
            {
                "component_id": row.component_id,
                "disposition": row.disposition,
                "reasons": list(row.reasons),
                "restarts": row.disposition == "replace",
            }
            for row in preview.components
        ],
        "scope": scope,
        "program": dict(preview.program),
        "attachment_changed_components": list(preview.attachment_changed_components),
        "detached_components": list(preview.detached_components),
        # The Apply sends these rule ids back in ``confirmed_collision_rule_ids``.
        "collision_rules": [deepcopy(row) for row in preview.collision_rules],
        "mesh_depth_reports": [deepcopy(row) for row in preview.mesh_depth_reports],
    }


def require_no_run(ports: ApplyPorts) -> bool:
    """Refuse while a run's motion is executing or stopping; True when a
    finished or held run is to be Reset first."""

    state = ports.cycle_state()
    if state == _FINISHED_CYCLE_STATE or (
        state == _RUNNING_CYCLE_STATE and ports.run_held()
    ):
        return True
    if state != _IDLE_CYCLE_STATE:
        raise ApplyRefused(
            "apply_refused_run_in_progress",
            "A Program run is in progress; Reset it before Apply",
        )
    return False


def require_no_motion(ports: ApplyPorts) -> None:
    """New's own check, before it releases or changes anything: refused while
    a Program run's motion is executing or stopping (:func:`require_no_run`)
    or a manual move is executing or being stopped. The operator stops motion
    first (maintainer, 2026-11-13). Then no component action may be running
    (:func:`require_no_component_action`)."""

    require_no_run(ports)
    if not ports.manual_operations_settled():
        raise ApplyRefused(
            "authoring_not_ready",
            "A manual move is in progress; stop motion first",
        )
    require_no_component_action(ports)


def require_no_component_action(ports: ApplyPorts) -> None:
    """New (and Load's New step) is refused, before it releases or disables
    anything, while any component has an action running (OD-194): with the
    host's own code and details, naming the first busy component."""

    for row in ports.components():
        running = row.get("action")
        if running is not None:
            raise ApplyRefused(
                "component_action_in_progress",
                "Another action of this component is still running",
                {"component_id": row["component_id"], "action": running["action"]},
            )


def _run_action(
    ports: ApplyPorts,
    states: list[ComponentState],
    component_id: str,
    action: str,
) -> None:
    previous: ActionResult | None = next(
        (row.get("last_action") for row in states if row["component_id"] == component_id),
        None,
    )
    ports.component_action(component_id, action)
    result = wait_for_action(
        ports.components,
        component_id,
        action,  # type: ignore[arg-type]
        previous,
    )
    if result["outcome"] != "succeeded":
        error = result.get("error")
        raise ApplyStepFailed(
            error["code"] if error else "component_action_outcome_unknown",
            error["message"]
            if error
            else f"{action} of {component_id} ended {result['outcome']}",
        )


def _process_exited(state: ComponentState) -> bool:
    """The component's own process ended by itself (its fault says so)."""

    fault = state.get("fault")
    return fault is not None and fault["code"] == "fault_process_exited"


def _members_last(states: list[ComponentState]) -> list[ComponentState]:
    """Cell order with every machine member (`owner_id`) after the rest."""

    return sorted(states, key=lambda state: bool(state.get("owner_id")))


def run_apply(
    ports: ApplyPorts,
    *,
    error_code: Callable[[Exception], tuple[str, str] | None] = lambda _e: None,
    confirmed_collision_rule_ids: list[str] | None = None,
) -> dict[str, Any]:
    """Run one orchestrated Apply and return its ApplyResult.

    Every step is tried and each failure is reported on its component's row:
    no rollback, no retry. A component that must leave but is still
    ``enabled`` after the disable phase without a failed Disable in this Apply
    would be refused by the host, so the Apply isn't submitted; one whose
    Disable failed is stopped by the host's Apply (the failure stays on
    its row). Once installed, the Apply stays; every arriving
    component (added, replaced, or retained whose exited process the Apply
    started again) is prepared, owners before members, and a member whose owner's
    prepare failed is reported ``owner_not_prepared`` without being tried.
    ``error`` is the first failure. ``error_code`` maps a port's own exception
    to ``(code, message)``; an exception it doesn't map propagates. Removed
    components carry no ``lifecycle_after``. A finished or held run is Reset
    before the Preview; a failed Reset ends the Apply with no component rows.
    A Preview that changes collision rules ``confirmed_collision_rule_ids``
    does not name ends the Apply ``collision_rule_confirmation_required``
    before any component step (``None``: the caller is the confirmation).
    """

    def failed(failure: Exception) -> dict[str, str]:
        if isinstance(failure, ApplyStepFailed):
            return {"code": failure.code, "message": str(failure)}
        mapped = error_code(failure)
        if mapped is None:
            raise failure
        return {"code": mapped[0], "message": mapped[1]}

    # The Preview prepares the candidate, which the authoring admission
    # allows only once the cycle is idle again: a finished or held run is
    # Reset first. A momentarily closed admission (it reopens a moment after
    # a Reset) is waited for by the preparation itself (Headless).
    try:
        reset_first = require_no_run(ports)
        # Every check that refuses before any effect has passed (OD-214).
        apply_accepted()
        if reset_first:
            apply_step("resetting_cycle")
            ports.reset_cycle()
    except ApplyRefused:
        raise
    except Exception as failure:  # noqa: BLE001 - the Apply's result
        return {"outcome": "failed", "components": [], "error": failed(failure)}
    apply_step("previewing")
    preview = ports.preview()
    unconfirmed = unconfirmed_collision_rules(preview, confirmed_collision_rule_ids)
    if unconfirmed:
        return {
            "outcome": "failed",
            "components": [],
            "error": {
                "code": COLLISION_RULE_CONFIRMATION_REQUIRED,
                "message": "Confirm the collision rule changes of the Preview first: "
                + ", ".join(unconfirmed),
            },
        }
    rows = preview.components
    disposition = {row.component_id: row.disposition for row in rows}
    errors: dict[str, dict[str, str]] = {}
    first: list[dict[str, str]] = []

    def record(component_id: str | None, error: dict[str, str]) -> None:
        if component_id is not None:
            errors[component_id] = error
        if not first:
            first.append(error)

    def step(states: list[ComponentState], component_id: str, action: str) -> bool:
        apply_step(
            "disabling" if action == "disable" else "preparing_components",
            {"status": "component_action", "component_id": component_id},
        )
        try:
            _run_action(ports, states, component_id, action)
            return True
        except Exception as failure:  # noqa: BLE001 - reported on the row
            record(component_id, failed(failure))
            return False

    try:
        states = ports.components()
        leaving = {
            row.component_id for row in rows if row.disposition in ("replace", "remove")
        }
        attempted: set[str] = set()
        for state in reversed(_members_last(states)):
            if state["component_id"] in leaving and state["lifecycle"] == "enabled":
                attempted.add(state["component_id"])
                step(states, state["component_id"], "disable")
                states = ports.components()

        def disable_failed(state: ComponentState) -> bool:
            # This Apply's Disable ended `failed`, nothing runs and it isn't
            # moving: the host stops that component like a dead one
            # (host_detail::LeavingComponentRefusesApply).
            last = state.get("last_action") or {}
            return (
                state["component_id"] in attempted
                and not state.get("action")
                and state.get("activity") != "busy"
                and last.get("action") == "disable"
                and last.get("outcome") == "failed"
            )

        still_enabled = [
            state["component_id"]
            for state in states
            if state["component_id"] in leaving
            and state["lifecycle"] == "enabled"
            and not disable_failed(state)
        ]
        if still_enabled:
            for component_id in still_enabled:
                if component_id not in errors:
                    record(component_id, {
                        "code": "apply_requires_inactive_component",
                        "message": f"{component_id} is still enabled",
                    })
        else:
            # A retained component whose process had exited is rebuilt by
            # the host's Apply with a new process: the exit fault is gone
            # afterwards. It is prepared like a replaced component.
            exited = {
                state["component_id"]
                for state in states
                if disposition.get(state["component_id"]) == "retain" and _process_exited(state)
            }
            apply_step("applying")
            ports.apply()
            states = ports.components()
            arriving = {
                row.component_id for row in rows if row.disposition in ("replace", "add")
            } | {
                state["component_id"]
                for state in states
                if state["component_id"] in exited and not _process_exited(state)
            }
            unprepared_owners: set[str] = set()
            for state in _members_last(states):
                component_id = state["component_id"]
                if component_id not in arriving or state["lifecycle"] in (
                    "prepared",
                    "enabled",
                ):
                    continue
                owner = state.get("owner_id")
                if owner in unprepared_owners:
                    record(component_id, {
                        "code": "owner_not_prepared",
                        "message": f"{component_id} wasn't prepared: its machine "
                        f"{owner} didn't prepare",
                    })
                    continue
                if not step(states, component_id, "prepare"):
                    unprepared_owners.add(component_id)
                states = ports.components()
    except Exception as failure:  # noqa: BLE001 - Manager step
        named = getattr(failure, "component_id", None)
        record(named if named in disposition else None, failed(failure))
    lifecycle = {state["component_id"]: state["lifecycle"] for state in ports.components()}
    components: list[dict[str, Any]] = []
    for component_id, kind in disposition.items():
        row: dict[str, Any] = {"component_id": component_id, "disposition": kind}
        if kind != "remove" and component_id in lifecycle:
            row["lifecycle_after"] = lifecycle[component_id]
        if component_id in errors:
            row["error"] = errors[component_id]
        components.append(row)
    result: dict[str, Any] = {
        "outcome": "failed" if first else "succeeded",
        "components": components,
    }
    if first:
        result["error"] = first[0]
    return result


_T = TypeVar("_T")
_LOGGER = logging.getLogger(__name__)


def _apply_terminal(result: Any) -> dict[str, Any]:
    """The History view of a route's result: an ApplyResult, or a Program
    Apply/Revert response (``applied`` now, or ``accepted`` by the Manager)."""

    body = result
    if hasattr(result, "body") and hasattr(result, "status_code"):
        body = json.loads(bytes(result.body))
    body = body if isinstance(body, dict) else {}
    if "outcome" in body:
        return {key: body[key] for key in ("outcome", "error", "components") if key in body}
    status = body.get("status")
    return {"outcome": "succeeded" if status == "applied" else "accepted"}


def _apply_failure(error: Exception) -> dict[str, Any]:
    detail = getattr(error, "detail", None)
    detail = detail if isinstance(detail, dict) else {}
    code = detail.get("code") or getattr(error, "code", None) or "composer_internal_failure"
    message = detail.get("message") or str(error) or code
    status = getattr(error, "status_code", 500)
    return {
        # Refused before any effect (4xx) or failed while running (5xx).
        "outcome": "refused" if isinstance(status, int) and status < 500 else "failed",
        "error": {"code": str(code), "message": str(message)},
    }


def recorded_apply(client: Any, apply_kind: str, run: Callable[[], _T]) -> _T:
    """Run one Load/New/Apply/Revert route and record its terminal in
    History once (Headless ``apply_finished``). Recording never changes the
    route's result."""

    try:
        result = run()
    except Exception as error:
        terminal = _apply_failure(error)
        _record_apply(client, apply_kind, terminal)
        raise
    _record_apply(client, apply_kind, _apply_terminal(result))
    return result


# OD-214: Apply, Load, New and Revert answer at acceptance; their row
# carries the step and the end.
APPLY_COMMAND_REPLY_SCHEMA = "manyforge.apply_command_reply.v1"
# Bound of the component rows an ended row's details carry (as History).
_MAXIMUM_ROW_COMPONENTS = 256
_INTERRUPTED_CODE = "component_action_interrupted_by_stop"
_UNKNOWN_CODE = "manager_apply_unknown_effect"


class ApplyCommand:
    """The progress of the one Apply command run in this thread.

    It is accepted once its before-effect checks passed (:func:`apply_accepted`,
    called where the first effect follows); until then it has no row in
    progress. Steps and their sources are published on its row.
    """

    def __init__(self, rows: CommandRowTable, kind: str, request_key: str) -> None:
        self._rows = rows
        self.kind = kind
        self.request_key = request_key
        self.accepted = False
        # Set at acceptance, or at the end of a command never accepted.
        self.decided = threading.Event()
        # The step its row takes at acceptance (Load: ``emptying``).
        self.accept_step: str | None = None
        self.hidden = 0
        self.effect_unknown = False
        self._step: str | None = None

    def accept(self) -> None:
        if self.accepted:
            return
        self._rows.accept(
            self.kind, self.request_key, step=self.accept_step, step_source=None
        )
        self._step = self.accept_step
        self.accepted = True
        self.decided.set()

    def step(self, token: str, source: dict[str, Any] | None = None) -> None:
        if not self.accepted or self.hidden:
            return
        self._step = token
        self._rows.step(self.request_key, token, step_source=deepcopy(source))

    def step_source(self, token: str, source: dict[str, Any]) -> None:
        if self.accepted and not self.hidden and self._step == token:
            self._rows.update(self.request_key, step_source=deepcopy(source))


_COMMAND: ContextVar[ApplyCommand | None] = ContextVar(
    "manyforge_apply_command", default=None
)


def apply_command() -> ApplyCommand | None:
    """The Apply command of this thread, if one runs (none in a Preview)."""

    return _COMMAND.get()


def apply_accepted() -> None:
    """The command's before-effect checks passed: answer its caller now."""

    command = _COMMAND.get()
    if command is not None:
        command.accept()


def apply_step(token: str, source: dict[str, Any] | None = None) -> None:
    command = _COMMAND.get()
    if command is not None:
        command.step(token, source)


def apply_step_source(token: str, source: dict[str, Any]) -> None:
    """Name the status of the current step ``token`` once it is known."""

    command = _COMMAND.get()
    if command is not None:
        command.step_source(token, source)


def apply_effect_unknown() -> None:
    """The Manager's Apply ended ``unknown_effect``: a failed end is unknown."""

    command = _COMMAND.get()
    if command is not None:
        command.effect_unknown = True


@contextmanager
def apply_steps_hidden() -> Iterator[None]:
    """A nested Apply (Load's New) keeps the outer step on the row."""

    command = _COMMAND.get()
    if command is not None:
        command.hidden += 1
    try:
        yield
    finally:
        if command is not None:
            command.hidden -= 1


def apply_command_reply(row: dict[str, Any]) -> dict[str, Any]:
    return {"schema_version": APPLY_COMMAND_REPLY_SCHEMA, "command": row}


def _row_end(
    terminal: dict[str, Any], error_details: Any, effect_unknown: bool
) -> tuple[str, str | None, dict[str, Any] | None]:
    """A History terminal as a row's ``(outcome, code, details)`` (§3.1)."""

    outcome = terminal.get("outcome")
    details: dict[str, Any] = {}
    components = terminal.get("components")
    if isinstance(components, list):
        details["components"] = deepcopy(components[:_MAXIMUM_ROW_COMPONENTS])
    if outcome in ("succeeded", "accepted"):
        # A Program Apply/Revert ends once the Manager accepted its Apply
        # (History: `accepted`); the ended row's `step_source` names that
        # Manager Apply. A succeeded row keeps its component rows.
        return "succeeded", None, details or None
    error = terminal.get("error") if isinstance(terminal.get("error"), dict) else {}
    code = str(error.get("code") or "composer_internal_failure")
    if isinstance(error_details, dict):
        details = {**deepcopy(error_details), **details}
    if outcome == "refused":
        return "refused", code, details or None
    if code == _INTERRUPTED_CODE:
        return "interrupted", code, details or None
    if code == _UNKNOWN_CODE or effect_unknown:
        return "unknown", code, details or None
    return "failed", code, details or None


_UNTYPED_CODE = "program_runtime_control_outcome_unknown"


def _typed(error: BaseException) -> bool:
    """Whether a failure carries its typed code (else its effect is unknown)."""

    detail = getattr(error, "detail", None)
    code = detail.get("code") if isinstance(detail, dict) else getattr(error, "code", None)
    return isinstance(code, str) and bool(code)


def _untyped_details(kind: str, error: BaseException) -> dict[str, Any]:
    return {"command_kind": kind, "cause_code": type(error).__name__}


def _error_details(error: BaseException) -> Any:
    detail = getattr(error, "detail", None)
    if isinstance(detail, dict):
        return detail.get("details")
    return getattr(error, "details", None)


class ApplyCommands:
    """The Apply-level rows (OD-214) of the one Apply slot, in memory.

    Each Apply, Revert, Load and New is run by :meth:`run` in a thread of its
    own; its caller is answered once it is accepted (or refused) with
    ``manyforge.apply_command_reply.v1``. The row ends when the run ends; the
    History record is written then, as before.
    """

    def __init__(self) -> None:
        self.rows = CommandRowTable()
        self._lock = threading.Lock()
        # The refusal replied for each kept refused row (replayed as is).
        self._refusals: dict[str, BaseException] = {}
        # The thread of the command run last (one Apply slot).
        self._worker: tuple[str, threading.Thread] | None = None

    def set_observer(self, observer: Callable[[], None] | None) -> None:
        self.rows.set_observer(observer)

    def snapshot(self) -> dict[str, Any]:
        return self.rows.snapshot()

    def _refused(self, command: ApplyCommand, error: BaseException) -> None:
        if _typed(error):
            code = _apply_failure(error)["error"]["code"]
            details = _error_details(error)
            self.rows.refuse(
                command.kind,
                command.request_key,
                code,
                details if isinstance(details, dict) else None,
                step_source=None,
            )
        else:
            # An untyped failure before acceptance: unknown, with no step.
            self.rows.refuse(
                command.kind,
                command.request_key,
                _UNTYPED_CODE,
                _untyped_details(command.kind, error),
                outcome="unknown",
                step_source=None,
            )
        kept = {
            row["request_key"] for row in self.rows.snapshot()["ended"]
        }
        with self._lock:
            self._refusals[command.request_key] = error
            for key in tuple(self._refusals):
                if key not in kept:
                    del self._refusals[key]

    def run(
        self, client: Any, apply_kind: str, request_key: str, run: Callable[[], Any]
    ) -> dict[str, Any]:
        kept = self.rows.find(request_key)
        if kept is not None:
            # The same request again: its row as it now stands, nothing runs;
            # a request refused at once gets its refusal again.
            with self._lock:
                refusal = self._refusals.get(request_key)
            if kept["accepted_at_ns"] is None and refusal is not None:
                raise refusal
            return apply_command_reply(kept)
        command = ApplyCommand(self.rows, apply_kind, request_key)
        failure: list[BaseException] = []

        def worker() -> None:
            _COMMAND.set(command)
            try:
                result = recorded_apply(client, apply_kind, run)
            except BaseException as error:  # noqa: BLE001 - the row's end
                if command.accepted and not _typed(error):
                    self.rows.end(
                        request_key, "unknown", _UNTYPED_CODE,
                        _untyped_details(apply_kind, error),
                    )
                elif command.accepted:
                    outcome, code, details = _row_end(
                        _apply_failure(error), _error_details(error),
                        command.effect_unknown,
                    )
                    self.rows.end(request_key, outcome, code, details)  # type: ignore[arg-type]
                else:
                    failure.append(error)
                    self._refused(command, error)
                if not isinstance(error, Exception):
                    raise
            else:
                command.accept()
                outcome, code, details = _row_end(
                    _apply_terminal(result), None, command.effect_unknown
                )
                self.rows.end(request_key, outcome, code, details)  # type: ignore[arg-type]
            finally:
                command.decided.set()

        thread = threading.Thread(
            target=worker, name=f"manyforge_{apply_kind}", daemon=True
        )
        with self._lock:
            self._worker = (request_key, thread)
        thread.start()
        command.decided.wait()
        if failure:
            raise failure[0]
        row = self.rows.find(request_key)
        assert row is not None
        return apply_command_reply(row)

    def ended_row(self, request_key: str) -> dict[str, Any] | None:
        """The row of this key once its command ended (an in-process caller
        that acts on the end, the startup Load)."""

        with self._lock:
            worker = self._worker
        if worker is not None and worker[0] == request_key:
            worker[1].join()
        return self.rows.find(request_key)


def _record_apply(client: Any, apply_kind: str, terminal: dict[str, Any]) -> None:
    if client is None:
        return
    try:
        client.record_apply_outcome({"apply_kind": apply_kind, **terminal})
    except Exception:  # noqa: BLE001 - a lost History line never fails the Apply
        _LOGGER.warning(
            "apply_outcome_record_failed", extra={"apply_kind": apply_kind}, exc_info=True
        )
