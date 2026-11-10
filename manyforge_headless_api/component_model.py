"""Component model v2 mirrors and the one Execute/Resume readiness owner.

Plain Python mirrors of the core types with
the contract's field names, the parse of the execution host's
``read_component_model`` reply, and :func:`run_readiness`, the only place that
computes ``RunReadiness`` (from component states, the cell latch, rule holds,
required inputs and the Program cycle). Everything here is derived from the
host's published state; nothing is stored.
"""

from __future__ import annotations

from collections.abc import Callable, Iterable, Mapping
from dataclasses import dataclass, field
import time
from typing import Any, Literal, NotRequired, TypedDict

from .native import HeadlessApiError

Lifecycle = Literal["offline", "not_prepared", "prepared", "enabled", "fault"]
Activity = Literal["idle", "busy", "unknown"]
Action = Literal[
    "prepare", "enable", "disable", "recover", "reset_position", "power_off", "reboot", "sim_reset"
]
Outcome = Literal["succeeded", "failed", "unknown"]

COMPONENT_ACTIONS: frozenset[str] = frozenset(
    {"prepare", "enable", "disable", "recover", "reset_position", "power_off", "reboot", "sim_reset"}
)
LIFECYCLE_ACTIONS: frozenset[str] = frozenset({"prepare", "enable", "disable", "recover"})
# Group actions that stop or remove run members before
# their owner; `prepare` runs each owner before its members.
STOPPING_ACTIONS: frozenset[str] = frozenset({"disable", "power_off", "reboot"})
# A component's lifecycle timeout when its Deployment row sets none
# (`configuration.lifecycle_command_timeout_s`, default 10 s).
DEFAULT_LIFECYCLE_TIMEOUT_S = 10.0
# The host's stop grace of a component's controller launch group before its
# SIGKILL (C++ `manyforge::core::kControllerLaunchStopGrace`, the one source;
# a test keeps them equal). A host Apply may retire one per component it
# changes: its bound counts it (the Manager's `HostApplyTimeout`, F8).
HOST_COMPONENT_STOP_GRACE_S = 10.0


class ErrorText(TypedDict):
    code: str
    message: str


class Fault(TypedDict):
    code: str
    message: str
    clearable: bool
    operator_action: Literal["enable", "recover", "none"]
    vendor_code: NotRequired[str]
    vendor_text: NotRequired[str]
    # On an owner's fault, the members it concerns.
    concerns: NotRequired[list[str]]
    since_ns: int


class ComponentStop(TypedDict):
    code: Literal["estop"]
    message: str
    since_ns: int
    vendor_code: NotRequired[str]
    vendor_text: NotRequired[str]


class ComponentStream(TypedDict):
    stream_id: str
    freshness: Literal["fresh", "stale", "none"]
    age_ns: NotRequired[int]


class ActionError(TypedDict):
    code: str
    message: str
    vendor_code: NotRequired[str]
    vendor_text: NotRequired[str]


class ActionPhase(TypedDict):
    component_id: str
    action: Literal["recover", "enable"]
    outcome: Outcome
    error: NotRequired[ActionError]


class ActionResult(TypedDict):
    action: Action
    outcome: Outcome
    error: NotRequired[ActionError]
    started_at_ns: int
    finished_at_ns: int
    phases: NotRequired[list[ActionPhase]]


class ActionInProgress(TypedDict):
    action: Action
    started_at_ns: int


class ComponentDetail(TypedDict):
    """One read-only diagnostic row (e.g. xArm mode, state, error code)."""

    name: str
    value: str


class ComponentEndpoint(TypedDict):
    """One endpoint the component's adapter publishes (e.g. `assistant_request`)."""

    role_id: str
    url: str


class ComponentState(TypedDict):
    component_id: str
    # The machine this component is a member of, and its member slot.
    owner_id: NotRequired[str]
    role: NotRequired[str]
    lifecycle: Lifecycle
    fault: NotRequired[Fault]
    stop: NotRequired[ComponentStop]
    # Why it's offline / not_prepared; a member shown offline through its
    # owner carries `owner_unavailable`.
    reason: NotRequired[ErrorText]
    activity: Activity
    streams: list[ComponentStream]
    capabilities: list[str]
    action: NotRequired[ActionInProgress]
    last_action: NotRequired[ActionResult]
    # The adapter's read-only diagnostics, display only.
    details: NotRequired[list[ComponentDetail]]
    # Adapter-published endpoints, present only while enabled; unlike
    # `details`, logic may read it.
    endpoints: NotRequired[list[ComponentEndpoint]]
    observed_at_ns: int


class CellLatchCause(TypedDict):
    component_id: str
    kind: Literal["fault", "offline", "activity_unknown", "stop"]
    fault: NotRequired[Fault]
    stop: NotRequired[ComponentStop]


class CellLatch(TypedDict):
    latched: bool
    causes: list[CellLatchCause]


class SignalSample(TypedDict):
    signal_id: str
    value: bool | float | int | None
    source: Literal["device", "manyforge_write", "outside", "not_readable"]
    age_ns: int
    fresh: bool
    # Present on outputs only.
    controller_owned: NotRequired[bool]


class SetOutputResult(TypedDict):
    outcome: Outcome
    error: NotRequired[ErrorText]


class BlockedReason(TypedDict):
    code: str
    message: str
    component_id: NotRequired[str]
    rule_id: NotRequired[str]
    # `rule_holds` only: the hold's scope and, for execution groups, the
    # components it holds; the GUI disables the groups it meets.
    scope: NotRequired[str]
    component_ids: NotRequired[list[str]]
    signal_id: NotRequired[str]


class RunReadiness(TypedDict):
    can_execute: bool
    can_resume: bool
    # Motion may start or continue (Motion panel Run/Resume of a group): no
    # latch, component or input reason, whatever the run state; a rule hold
    # is refused by the kernel in its own scope only.
    motion_clear: bool
    blocked_reasons: list[BlockedReason]


@dataclass(frozen=True, slots=True)
class ComponentModel:
    """One read of the execution host's published component model."""

    component_states: list[ComponentState]
    cell_latch: CellLatch
    signal_samples: list[SignalSample]
    # The kernel's `ReadRuleHolds()` rows: `{code: rule_holds, message,
    # details: {rule_id, scope: everything | all_motion | execution_groups,
    # component_ids?}}` (`component_ids` comma-separated, groups only).
    rule_holds: list[dict[str, Any]]
    # Signals the applied Program and the active rules read (the host knows
    # its installed bindings); a stale or missing one blocks Execute.
    required_signal_ids: list[str]
    # The applied Program's motion components; a rule hold blocks
    # Execute/Resume only when its scope meets them.
    program_motion_component_ids: list[str]
    # The kernel's `ReadExecutionBlocks()` rows `{code, message, details}`:
    # blocks of no component (`execution_host_internal_fault`,
    # `manager_unavailable`); each blocks Execute and Resume.
    execution_blocks: list[dict[str, Any]] = field(default_factory=list)


def _invalid(message: str) -> HeadlessApiError:
    return HeadlessApiError("execution_command_response_invalid", message)


def _rows(value: object, label: str) -> list[dict[str, Any]]:
    if not isinstance(value, list) or not all(isinstance(row, dict) for row in value):
        raise _invalid(f"component model {label} must be a list of objects")
    return value


def _validate_stop(value: object) -> None:
    if (
        not isinstance(value, dict)
        or not {"code", "message", "since_ns"} <= value.keys()
        or not value.keys() <= {"code", "message", "since_ns", "vendor_code", "vendor_text"}
        or value["code"] != "estop"
        or not isinstance(value["message"], str)
        or not value["message"]
        or type(value["since_ns"]) is not int
        or value["since_ns"] < 0
        or any(key in value and not isinstance(value[key], str) for key in ("vendor_code", "vendor_text"))
    ):
        raise _invalid("component stop is invalid")


def parse_component_model(response: Mapping[str, Any]) -> ComponentModel:
    """Parse the host's ``read_component_model`` reply (shape checks only)."""

    latch = response.get("cell_latch")
    if not isinstance(latch, dict) or type(latch.get("latched")) is not bool:
        raise _invalid("component model cell_latch is invalid")
    causes = _rows(latch.get("causes"), "cell_latch.causes")
    for cause in causes:
        if cause.get("kind") == "stop":
            _validate_stop(cause.get("stop"))
    states = _rows(response.get("component_states"), "component_states")
    for state in states:
        if "stop" in state:
            _validate_stop(state["stop"])
        if not isinstance(state.get("component_id"), str) or state.get(
            "lifecycle"
        ) not in ("offline", "not_prepared", "prepared", "enabled", "fault"):
            raise _invalid("component state has no id or an unknown lifecycle")
        if any(
            key in state and not isinstance(state[key], str) for key in ("owner_id", "role")
        ):
            raise _invalid("component state owner_id or role is not text")
        details = state.get("details", [])
        if not isinstance(details, list) or not all(
            isinstance(row, dict)
            and isinstance(row.get("name"), str)
            and isinstance(row.get("value"), str)
            for row in details
        ):
            raise _invalid("component state details must be name/value text pairs")
        endpoints = state.get("endpoints", [])
        if not isinstance(endpoints, list) or not all(
            isinstance(row, dict)
            and isinstance(row.get("role_id"), str)
            and isinstance(row.get("url"), str)
            for row in endpoints
        ):
            raise _invalid("component state endpoints must be role_id/url text pairs")
    required = response.get("required_signal_ids", [])
    if not isinstance(required, list) or not all(isinstance(v, str) for v in required):
        raise _invalid("component model required_signal_ids is invalid")
    motion = response.get("program_motion_component_ids", [])
    if not isinstance(motion, list) or not all(isinstance(v, str) for v in motion):
        raise _invalid("component model program_motion_component_ids is invalid")
    return ComponentModel(
        component_states=states,  # type: ignore[arg-type]
        cell_latch=latch,  # type: ignore[arg-type]
        signal_samples=_rows(response.get("signal_samples"), "signal_samples"),  # type: ignore[arg-type]
        rule_holds=_rows(response.get("rule_holds"), "rule_holds"),
        required_signal_ids=list(required),
        program_motion_component_ids=list(motion),
        execution_blocks=_rows(response.get("execution_blocks", []), "execution_blocks"),
    )


def _reason(code: str, message: str, **ids: str) -> BlockedReason:
    reason: BlockedReason = {"code": code, "message": message}
    reason.update(ids)  # type: ignore[typeddict-item]
    return reason


def latch_summary(latch: CellLatch) -> str:
    """"N errors require attention · <leading cause>" (or the single cause)."""

    causes = latch["causes"]
    if not causes:
        return "Cell latched"
    first = causes[0]
    lead = f"{first['component_id']}: " + (
        first["fault"]["message"] if "fault" in first
        else first["stop"]["message"] if "stop" in first
        else {"offline": "offline", "activity_unknown": "activity unknown"}.get(
            first["kind"], first["kind"]
        )
    )
    return lead if len(causes) == 1 else f"{len(causes)} errors require attention · {lead}"


# Typed refusals of Execute, Resume and a group's Run/Resume before any
# effect: every RunReadiness reason and execution block, and the
# runner's own run refusals. Headless and runner map them to 409, never 5xx.
RUN_REFUSAL_CODES: frozenset[str] = frozenset(
    {
        "cell_latched",
        "component_not_enabled",
        "component_unhealthy",
        "rule_holds",
        "rule_unknown",
        "input_not_fresh",
        "run_in_progress",
        "program_motion_running",
        "manual_operation_in_progress",
        "program_not_executable",
        "execution_host_internal_fault",
        "execution_host_restart_awaiting_components",
        "execution_host_restart_settling",
        "manager_unavailable",
        "program_scene_preparation_failed",
        "program_cycle_reset_required",
        "program_run_history_full",
        "program_end_effector_tcp_unavailable",
        "program_motion_resume_incomplete",
        "io_manual_write_motion_executing",
    }
)

# Existing nonblocking reasons on simulated robot/tool states while they are
# prepared. The simulator itself is identified by `sim_reset` and skipped
# separately, including when its lifecycle is offline or faulted.
SIMULATION_NOT_PLAYING_REASONS: frozenset[str] = frozenset(
    {"simulation_paused", "simulation_stopped"}
)

# Runner cycle states in which no run is in progress: Execute needs this one.
_IDLE_CYCLE_STATE = "idle_after_reset"
# The runner's Resume command: allowed only while the run is held (stopped by
# Stop/Halt or a rule), never while its motion runs.
_RESUME_COMMAND = "program_resume_full_stop"


def rule_hold_meets_program(hold: Mapping[str, Any], program_motion: Iterable[str]) -> bool:
    """A hold's scope meets the Program: `everything` always; `all_motion`
    when the Program moves anything; execution groups when one of their
    components is a Program motion component."""

    details = hold.get("details", {})
    scope = details.get("scope")
    motion = set(program_motion)
    if scope == "all_motion":
        return bool(motion)
    if scope == "execution_groups":
        return bool(motion & set(filter(None, str(details.get("component_ids", "")).split(","))))
    return True


def owner_ids(states: Iterable[ComponentState]) -> set[str]:
    """Owners: every component that is some state's `owner_id`."""

    return {state["owner_id"] for state in states if "owner_id" in state}


def manual_operation_resident(execution_control: Mapping[str, Any] | None) -> bool:
    """A manual operation the execution host still holds (``resident``)."""

    control = execution_control.get("control_state") if execution_control else None
    operations = control.get("manual_operations") if isinstance(control, Mapping) else None
    return any(
        isinstance(row, Mapping) and row.get("resident") is True
        for row in operations or ()
    )


def _runner_allows(cycle: Mapping[str, Any], command_kind: str) -> bool:
    return any(
        isinstance(command, Mapping)
        and command.get("command_kind") == command_kind
        and command.get("allowed") is True
        for command in cycle.get("allowed_commands") or ()
    )


def program_not_executable(status: Mapping[str, Any] | None) -> BlockedReason | None:
    """The applied Program's binding problem, from applied-Program status."""

    if not status or status.get("program_id") is None or status.get("executable"):
        return None
    outcome = status.get("non_executable") or {}
    targets = ", ".join(
        f"{row['target'].get('component_id')} / {row['target'].get('command_target_id')}"
        for row in outcome.get("unresolved_targets") or ()
        if isinstance(row, Mapping) and isinstance(row.get("target"), Mapping)
    )
    message = status.get("message") or "The applied Program can't run in this Deployment"
    return _reason("program_not_executable", f"{message} ({targets})" if targets else message)


def run_readiness(
    model: ComponentModel,
    cycle: Mapping[str, Any] | None,
    *,
    manual_operation_in_progress: bool,
    program_status: Mapping[str, Any] | None = None,
) -> RunReadiness:
    """The one computation of Execute/Resume readiness.

    ``cycle`` is the Program runner's cycle status (None: the runner can't be
    read); ``program_status`` the applied-Program status (None: unknown).
    Every component in the cell must be enabled (deactivated ones never
    reach it); an owner has no Enable: `prepared` is its usable state.
    A component declaring `sim_reset` is the simulator itself: its state does
    not block Execute, so a Program's own nodes can operate it. Simulated
    robots and tools keep their own lifecycle and health rules."""

    reasons: list[BlockedReason] = []
    if cycle is None:
        reasons.append(
            _reason("program_runner_unavailable", "The Program runner is not available")
        )
    if manual_operation_in_progress:
        reasons.append(
            _reason(
                "manual_operation_in_progress",
                "A manual operation is in progress; wait for it or Stop it",
            )
        )
    simulator_ids = {
        state["component_id"]
        for state in model.component_states
        if "sim_reset" in state["capabilities"]
    }
    blocking_latch_causes = [
        cause
        for cause in model.cell_latch["causes"]
        if cause["component_id"] not in simulator_ids
    ]
    # Core excludes a simulator's own state from the canonical cell latch.
    # Ignore a simulator-only cause from a mixed-version runtime too, while
    # keeping every ordinary component cause (and an unexplained latch).
    if model.cell_latch["latched"] and (
        blocking_latch_causes or not model.cell_latch["causes"]
    ):
        reasons.append(
            _reason(
                "cell_latched",
                latch_summary({"latched": True, "causes": blocking_latch_causes}),
            )
        )
    for block in model.execution_blocks:
        component_id = block.get("details", {}).get("component_id")
        ids = {"component_id": component_id} if isinstance(component_id, str) else {}
        reasons.append(_reason(str(block.get("code")), str(block.get("message", "")), **ids))
    binding_problem = program_not_executable(program_status)
    if binding_problem is not None:
        reasons.append(binding_problem)
    owners = owner_ids(model.component_states)
    for state in model.component_states:
        # `sim_reset` is the simulator discriminator. Its own offline/fault
        # state is visible but does not block Execute; Core applies the same
        # identity contract when deriving the cell latch.
        if "sim_reset" in state.get("capabilities", []):
            continue
        component_id = state["component_id"]
        lifecycle = state["lifecycle"]
        if component_id in simulator_ids:
            continue
        if "stop" in state or lifecycle in ("fault", "offline"):
            detail = (
                state["stop"]["message"] if "stop" in state
                else state["fault"]["message"] if "fault" in state
                else state.get("reason", {}).get("message", lifecycle)
            )
            reasons.append(
                _reason(
                    "component_unhealthy",
                    f"{component_id} is {lifecycle}: {detail}",
                    component_id=component_id,
                )
            )
        elif lifecycle != "enabled" and not (
            lifecycle == "prepared"
            and (
                component_id in owners
                or state.get("reason", {}).get("code") in SIMULATION_NOT_PLAYING_REASONS
            )
        ):
            reasons.append(
                _reason(
                    "component_not_enabled",
                    f"{component_id} is not enabled",
                    component_id=component_id,
                )
            )
    # Every hold is shown with its scope; only one meeting the Program's
    # motion blocks Execute/Resume.
    other_holds: list[BlockedReason] = []
    for hold in model.rule_holds:
        details = hold.get("details", {})
        rule_id = str(details.get("rule_id", ""))
        reason = _reason(
            "rule_holds",
            str(hold.get("message") or f"Rule {rule_id} holds its scope"),
            rule_id=rule_id,
            scope=str(details.get("scope", "everything")),
        )
        if details.get("scope") == "execution_groups":
            reason["component_ids"] = list(
                filter(None, str(details.get("component_ids", "")).split(","))
            )
        if rule_hold_meets_program(hold, model.program_motion_component_ids):
            reasons.append(reason)
        else:
            other_holds.append(reason)
    samples = {sample["signal_id"]: sample for sample in model.signal_samples}
    for signal_id in model.required_signal_ids:
        sample = samples.get(signal_id)
        if sample is None or sample.get("fresh") is not True:
            reasons.append(
                _reason(
                    "input_not_fresh",
                    f"{signal_id} has no fresh value",
                    signal_id=signal_id,
                )
            )
    cycle_state = cycle.get("state") if cycle is not None else None
    run_reasons = list(reasons)
    if cycle is not None and cycle_state != _IDLE_CYCLE_STATE:
        run_reasons.append(
            _reason(
                "run_in_progress",
                "A Program run is in progress; Reset it first",
            )
        )
        if cycle_state == "running" and not _runner_allows(cycle, _RESUME_COMMAND):
            run_reasons.append(
                _reason(
                    "program_motion_running",
                    "The Program run isn't stopped; Resume needs a stopped run",
                )
            )
    return {
        "can_execute": not run_reasons,
        "can_resume": cycle is not None
        and _runner_allows(cycle, _RESUME_COMMAND)
        and not reasons,
        # A rule hold gates only its own scope; the kernel refuses motion
        # in that scope (`rule_holds`), so it doesn't hold every group here.
        "motion_clear": all(reason["code"] == "rule_holds" for reason in reasons),
        "blocked_reasons": run_reasons + other_holds,
    }


def enable_all_targets(states: Iterable[ComponentState]) -> list[str]:
    """Enable every prepared non-simulator member and owner-less component.

    Simulators expose `sim_reset` and are controlled explicitly, never by
    Activate-and-execute.
    """

    states = list(states)
    owners = owner_ids(states)
    return [
        state["component_id"]
        for state in states
        if state["lifecycle"] == "prepared"
        and state["component_id"] not in owners
        and "sim_reset" not in state["capabilities"]
    ]


def prepare_all_targets(states: Iterable[ComponentState]) -> list[str]:
    """Prepare all not-prepared or offline non-simulator components.

    Simulators expose `sim_reset` and are controlled explicitly, never by
    Activate-and-execute.
    """

    return [
        state["component_id"]
        for state in states
        if state["lifecycle"] in ("not_prepared", "offline")
        and "sim_reset" not in state["capabilities"]
    ]


def group_action_order(states: Iterable[ComponentState], action: str) -> list[str]:
    """The order of a group action over the cell.

    `prepare` runs each owner before its members, and anything that stops or
    removes (`STOPPING_ACTIONS`) runs the members before their owner; a
    machine takes its owner's place in cell order. Other actions, and
    owner-less components, keep cell order.
    """

    states = list(states)
    order = [state["component_id"] for state in states]
    if action != "prepare" and action not in STOPPING_ACTIONS:
        return order
    present = set(order)
    members: dict[str, list[str]] = {}
    for state in states:
        if state.get("owner_id") in present:
            members.setdefault(state["owner_id"], []).append(state["component_id"])
    result: list[str] = []
    for state in states:
        component_id = state["component_id"]
        if state.get("owner_id") in present:
            continue  # placed with its owner
        machine = members.get(component_id, [])
        result.extend(
            [component_id, *machine] if action == "prepare" else [*machine, component_id]
        )
    return result


def lifecycle_timeouts_s(cell_graph: Mapping[str, Any]) -> dict[str, float]:
    """Each component's authored lifecycle timeout in the active cell graph,
    in seconds, as the host reads it (`component_lifecycle_budgets_ns`): the
    runtime units' ``lifecycle_command_timeouts`` and ``io_provider_devices``
    rows. A component without one uses ``DEFAULT_LIFECYCLE_TIMEOUT_S``."""

    timeouts: dict[str, float] = {}
    for unit in cell_graph.get("runtime_units") or ():
        configuration = unit.get("configuration") if isinstance(unit, Mapping) else None
        if not isinstance(configuration, Mapping):
            continue
        for field_name in ("lifecycle_command_timeouts", "io_provider_devices"):
            for row in configuration.get(field_name) or ():
                value = row.get("lifecycle_command_timeout_s") if isinstance(row, Mapping) else None
                if isinstance(value, (int, float)) and not isinstance(value, bool):
                    timeouts[str(row.get("controllable_component_id"))] = float(value)
    return timeouts


def launch_settle_bound_s(timeouts: Iterable[float]) -> float:
    """The host's launch bound (the bring-up bound, `LaunchSettleBound`): the
    sum of the lifecycle timeouts of the components a composition launches or
    removes, one default when that is none."""

    total = sum(timeouts)
    return total if total > 0 else DEFAULT_LIFECYCLE_TIMEOUT_S


def action_wait_s(
    action: str, lifecycle_timeout_s: float, launch_bound_s: float, *, launch_settled: bool
) -> float:
    """How long a caller waits for an action's result, as the host bounds it:
    the component's lifecycle timeout; a ``prepare`` of a component whose
    launch may not have settled first waits up to the launch bound."""

    if action == "prepare" and not launch_settled:
        return launch_bound_s + lifecycle_timeout_s
    return lifecycle_timeout_s


def wait_for_action(
    read_states: Callable[[], list[ComponentState]],
    component_id: str,
    action: Action,
    previous: ActionResult | None,
    *,
    timeout_s: float,
    poll_seconds: float = 0.1,
) -> ActionResult:
    """Wait until the component's action started after ``previous`` finished.

    The coordinator publishes the finished action as ``last_action``. The wait
    is bounded by the component's lifecycle timeout (``timeout_s``): a result
    that hasn't come back by then is ``unknown`` and nothing resends it.
    """

    started_ns = time.time_ns()
    deadline = time.monotonic() + timeout_s
    while True:
        state = next(
            (row for row in read_states() if row["component_id"] == component_id), None
        )
        if state is None:
            raise HeadlessApiError(
                "component_not_found", f"component {component_id} is not in the cell"
            )
        last = state.get("last_action")
        if "action" not in state and last is not None and last != previous:
            return last
        if time.monotonic() >= deadline:
            return {
                "action": action,
                "outcome": "unknown",
                "error": {
                    "code": "component_action_outcome_unknown",
                    "message": (
                        f"{action} of {component_id} didn't finish within "
                        f"{timeout_s:g} s"
                    ),
                },
                "started_at_ns": started_ns,
                "finished_at_ns": time.time_ns(),
            }
        time.sleep(poll_seconds)
