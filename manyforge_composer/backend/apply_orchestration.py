"""Server-side Apply orchestration: the one path for Apply,
Load, New and Close, used alike by the GUI, REST and MCP.

For each ``replace``/``remove`` component: ``disable`` → Apply → (``replace``
and ``add``) ``prepare``. Retained components are untouched. A component in
``fault`` or ``offline`` (faulted, or its process is gone) is not disabled:
the host removes it by stopping its processes. Afterwards the affected
components are ``prepared``, never enabled. Machines: owners are
prepared before their members, and members are disabled first. Each step is
bounded by the component's lifecycle timeout; a ``prepare`` of an arriving
component is bounded as the host bounds it, by the launch bound
(:func:`launch_settle_bound_s`) plus its lifecycle timeout. One Apply at a time. Refused
while a Program run's motion is executing or stopping; a finished run awaiting
Reset, or a run with all its motion held, is reset first; allowed
during a cell latch. Every
step is tried and each failure is reported on its row; the Apply then
ends ``failed``: no retries, no rollback.
"""

from __future__ import annotations

from collections.abc import Callable, Iterator
from contextlib import contextmanager
from copy import deepcopy
from dataclasses import dataclass, field
import json
import logging
import threading
from typing import Any, Protocol, TypeVar

from manyforge_headless_api.component_model import (
    DEFAULT_LIFECYCLE_TIMEOUT_S,
    HOST_COMPONENT_STOP_GRACE_S,
    ActionResult,
    ComponentState,
    action_wait_s,
    launch_settle_bound_s as host_launch_settle_bound_s,
    wait_for_action,
)

# The Runner cycle state in which no Program run is in progress.
_IDLE_CYCLE_STATE = "idle_after_reset"
# A finished run awaiting Reset: the Apply resets it itself.
_FINISHED_CYCLE_STATE = "terminal_awaiting_reset"
# A run whose motion is all held (``run_held``) is reset alike.
_RUNNING_CYCLE_STATE = "running"
_APPLY_SLOT = threading.Lock()


class ApplyRefused(Exception):
    """Refused before any effect (HTTP 409)."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


class ApplyStepFailed(Exception):
    """A step failed after the Apply started; the Apply ends `failed`."""

    def __init__(self, code: str, message: str, component_id: str | None = None) -> None:
        super().__init__(message)
        self.code = code
        # The component the failure names, if any: reported on its row.
        self.component_id = component_id


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
            "program_id": None,
            "unresolved_references": [],
            "affected_nodes": [],
            "affected_execution_groups": [],
        }
    )
    attachment_changed_components: tuple[str, ...] = ()
    detached_components: tuple[str, ...] = ()


class ApplyPorts(Protocol):
    def cycle_state(self) -> str | None: ...

    def run_held(self) -> bool:
        """The running run has all its motion stopped and held, nothing can
        start (the published held-run rule)."""

    def reset_cycle(self) -> None:
        """Reset the finished or held run and return once the cycle is idle
        (raise :class:`ApplyStepFailed` otherwise)."""

    def preview(self) -> Preview: ...

    def components(self) -> list[ComponentState]: ...

    def component_action(self, component_id: str, action: str) -> None: ...

    def lifecycle_timeout_s(self, component_id: str) -> float: ...

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
            "program_id": program.get("program_id"),
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
    }


def lifecycle_timeouts(deployment: dict[str, Any] | None) -> dict[str, float]:
    """Each Deployment component's authored lifecycle timeout, in seconds."""

    timeouts: dict[str, float] = {}
    for row in (deployment or {}).get("components") or ():
        configuration = row.get("configuration") if isinstance(row, dict) else None
        value = (
            configuration.get("lifecycle_command_timeout_s")
            if isinstance(configuration, dict)
            else None
        )
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            timeouts[str(row.get("component_id"))] = float(value)
    return timeouts


def launch_settle_bound_s(ports: ApplyPorts, rows: tuple[ComponentImpact, ...]) -> float:
    """The host's launch bound for this Apply (the bring-up bound,
    `LaunchSettleBound`): the sum of the lifecycle timeouts of the components
    it adds, replaces or removes, one default when it changes none. A
    just-launched component's ``prepare`` first waits up to this for its
    launch to settle, then up to its own lifecycle timeout."""

    return host_launch_settle_bound_s(
        ports.lifecycle_timeout_s(row.component_id)
        for row in rows
        if row.disposition != "retain"
    )


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


def _run_action(
    ports: ApplyPorts,
    states: list[ComponentState],
    component_id: str,
    action: str,
    timeout_s: float,
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
        timeout_s=timeout_s,
    )
    if result["outcome"] != "succeeded":
        error = result.get("error")
        raise ApplyStepFailed(
            error["code"] if error else "component_action_outcome_unknown",
            error["message"]
            if error
            else f"{action} of {component_id} ended {result['outcome']}",
        )


def _members_last(states: list[ComponentState]) -> list[ComponentState]:
    """Cell order with every machine member (`owner_id`) after the rest."""

    return sorted(states, key=lambda state: bool(state.get("owner_id")))


def run_apply(
    ports: ApplyPorts,
    *,
    error_code: Callable[[Exception], tuple[str, str] | None] = lambda _e: None,
) -> dict[str, Any]:
    """Run one orchestrated Apply and return its ApplyResult.

    Every step is tried and each failure is reported on its component's row:
    no rollback, no retry. A component that must leave but is still
    ``enabled`` after the disable phase without a failed Disable in this Apply
    would be refused by the host, so the Apply isn't submitted; one whose
    Disable failed is stopped by the host's Apply (the failure stays on
    its row). Once installed, the Apply stays; every arriving
    component is prepared, owners before members, and a member whose owner's
    prepare failed is reported ``owner_not_prepared`` without being tried.
    ``error`` is the first failure. ``error_code`` maps a port's own exception
    to ``(code, message)``; an exception it doesn't map propagates. Removed
    components carry no ``lifecycle_after``. A finished or held run is Reset
    before the Preview; a failed Reset ends the Apply with no component rows.
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
        if require_no_run(ports):
            ports.reset_cycle()
    except ApplyRefused:
        raise
    except Exception as failure:  # noqa: BLE001 - the Apply's result
        return {"outcome": "failed", "components": [], "error": failed(failure)}
    rows = ports.preview().components
    launch_bound_s = launch_settle_bound_s(ports, rows)
    disposition = {row.component_id: row.disposition for row in rows}
    errors: dict[str, dict[str, str]] = {}
    first: list[dict[str, str]] = []

    def record(component_id: str | None, error: dict[str, str]) -> None:
        if component_id is not None:
            errors[component_id] = error
        if not first:
            first.append(error)

    def step(states: list[ComponentState], component_id: str, action: str) -> bool:
        # Arriving components are prepared right after their launch.
        timeout_s = action_wait_s(
            action,
            ports.lifecycle_timeout_s(component_id),
            launch_bound_s,
            launch_settled=False,
        )
        try:
            _run_action(ports, states, component_id, action, timeout_s)
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
            ports.apply()
            states = ports.components()
            arriving = {
                row.component_id for row in rows if row.disposition in ("replace", "add")
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


def _record_apply(client: Any, apply_kind: str, terminal: dict[str, Any]) -> None:
    if client is None:
        return
    try:
        client.record_apply_outcome({"apply_kind": apply_kind, **terminal})
    except Exception:  # noqa: BLE001 - a lost History line never fails the Apply
        _LOGGER.warning(
            "apply_outcome_record_failed", extra={"apply_kind": apply_kind}, exc_info=True
        )
