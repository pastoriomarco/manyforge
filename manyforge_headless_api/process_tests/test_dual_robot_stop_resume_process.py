"""Stop and Resume at chosen moments of a two-robot Mock cycle (release gate).

A short subset of the Stop/Resume matrix: six Stops, each in its own Execute
of the bundled two-robot Mock workbench (``mock_dual_arm_workbench``: a UR5e
and a Lite 6 behind a wall, each robot two joint moves in sequence, the two
robots in parallel). The moment of each Stop is chosen from what the run
shows while it moves: node statuses and whether a move's operation is
executing. Each Stop is followed through settle, the checks, Resume and the
rest of the cycle, the way the operator's screen does it, over the same API.

The bundled workbench puts all four moves in one motion group. A robot can
be stopped alone only when it has its own group, so the test loads a copy
whose only change is the group membership: each robot's two moves form its
own group (``lite6_arm``, ``ur5e_arm``), as in
``mock_dual_arm_separate_groups``. Motions, speeds and scene are as authored.

No fixed wait: every bound is the undisturbed duration of one cycle,
measured first in this test on the same machine (its first, undisturbed
cycle is bounded by the Program's own planning and execution timeouts).
"""

from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass
import json
from pathlib import Path
import time
from typing import Any, Callable, Iterator, Mapping

import pytest

from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient
from manyforge_headless_api.process_tests.host_probe import HostProbe
from manyforge_headless_api.process_tests.test_dual_robot_grouped_execution import (
    REPOSITORY_ROOT,
    _application_start_id,
    _dual_robot_stack,
    _load_scenario,
    _reset_cycle,
    _start_motion,
    _start_run,
    _wait_kernel_attention_reconciled,
)
from manyforge_headless_api.process_tests.test_process_stack import (
    ProcessStack,
    _request_key,
    _required_path,
    _runner_command_end,
)


WORKBENCH_SCENARIO = "mock_dual_arm_workbench"
SOURCE_LABEL = "dual_robot_stop_resume_process_test"
# The bundled workbench's four moves, two per robot, in sequence order.
LITE6_FIRST, LITE6_SECOND = "lite6_joint_motion", "lite6_joint_motion_2"
UR5E_FIRST, UR5E_SECOND = "ur5e_joint_motion", "ur5e_joint_motion_2"
MOVES = (LITE6_FIRST, LITE6_SECOND, UR5E_FIRST, UR5E_SECOND)
# The copy's motion groups: one per robot, as in mock_dual_arm_separate_groups.
GROUP_OF_COMPONENT = {"lite6_robot": "lite6_arm", "ur5e_robot": "ur5e_arm"}
ARM_TARGETS = {("lite6_robot", "arm"), ("ur5e_robot", "arm")}
ENDED_RUN_STATES = frozenset({"succeeded", "failed", "stopped"})
ENDED_NODE_STATUSES = frozenset({"success", "failure", "interrupted"})
# A stopped group is settled for Resume in one of these states
# (program_run_owner._selection_group_state).
STOPPED_GROUP_STATES = frozenset({"held", "mixed"})
# The GUI's "Stopping": the group's Stop is still settling.
STOP_SETTLING_REASON = "selected_motion_stop_settling"
# A failure hold's Retry, in the GUI's order (ProgramRuntimePanel.tsx
# FAILURE_RETRY_ACTION_IDS); an operator Stop's replan stays its Resume.
FAILURE_RETRY_ACTION_IDS = (
    "retry_motion_from_current_state",
    "retry_after_scene_repair",
    "retry_runtime_step",
    "replan_from_stopped_state",
)
# Bound on retries of a refusal whose code names a stale fence.
STALE_ATTEMPTS = 8
# A move's final joint position against its goal, on the Mock lane.
GOAL_TOLERANCE_RAD = 0.01


# --- The cycle's Program -------------------------------------------------


@dataclass(frozen=True)
class _Cycle:
    """What the test needs of the loaded copy's Program."""

    # Every authored node instance, and the group of each motion node.
    nodes: tuple[str, ...]
    group_of: Mapping[str, str]
    # Each group's motion nodes, in sequence order.
    group_moves: Mapping[str, tuple[str, ...]]
    # Each robot's last goal: component -> joint name -> position (rad).
    final_goals: Mapping[str, Mapping[str, float]]
    # Bound for the first, undisturbed cycle: the longest robot sequence of
    # authored planning plus execution timeouts.
    timeout_bound_s: float

    def stop_groups(self, scope: str) -> tuple[str, ...]:
        """The motion groups a Stop of ``scope`` addresses."""

        return tuple(self.group_moves) if scope == "all" else (scope,)

    def scope_nodes(self, scope: str) -> tuple[str, ...]:
        return self.nodes if scope == "all" else self.group_moves[scope]


def _authored_nodes(nodes: list[dict[str, Any]]) -> Iterator[dict[str, Any]]:
    """Depth first, in authored order."""

    for node in nodes:
        yield node
        yield from _authored_nodes(node.get("children", []))


def _is_motion(node: dict[str, Any]) -> bool:
    return node["node_id"].startswith("motion.")


def _write_two_group_workbench(stack: ProcessStack) -> tuple[Path, _Cycle]:
    """The bundled workbench with one motion group per robot; nothing else
    changes. Saved as the same Scenario under the stack's root."""

    # Keep Composer out of native-only test collection.
    from manyforge_composer.backend.scenario_artifacts import (
        build_scenario_document,
        load_scenario_artifact,
        save_scenario_artifact,
    )

    examples = _required_path("MANYFORGE_EXAMPLE_SCENARIO_ROOT")
    source = examples / f"{WORKBENCH_SCENARIO}.scenario"
    roots = [str(examples), str(stack.root), str(REPOSITORY_ROOT)]
    document, _ = load_scenario_artifact(str(source), allowed_roots=roots)
    program = deepcopy(document["program_draft"])
    assert set(program["execution_groups"]) == {"robot_group"}, program["execution_groups"]
    program["execution_groups"] = {
        group: {"execution_policy": "independent"} for group in GROUP_OF_COMPONENT.values()
    }
    nodes: list[str] = []
    group_of: dict[str, str] = {}
    group_moves: dict[str, list[str]] = {group: [] for group in GROUP_OF_COMPONENT.values()}
    final_goals: dict[str, dict[str, float]] = {}
    sequence_bounds: dict[str, float] = {}
    for node in _authored_nodes(program["forest"]):
        nodes.append(node["instance_id"])
        if not _is_motion(node):
            continue
        assert node["execution_groups"] == ["robot_group"], node
        parameters = node["parameters"]
        component = parameters["target"]["component_id"]
        group = GROUP_OF_COMPONENT[component]
        node["execution_groups"] = [group]
        group_of[node["instance_id"]] = group
        group_moves[group].append(node["instance_id"])
        final_goals[component] = {
            row["joint_name"]: row["position_rad"] for row in parameters["goal"]
        }
        sequence_bounds[component] = sequence_bounds.get(component, 0.0) + (
            parameters["planning_timeout_s"] + parameters["execution_timeout_s"]
        )
    # The four moves this test names, two per robot.
    assert group_moves == {
        "lite6_arm": [LITE6_FIRST, LITE6_SECOND],
        "ur5e_arm": [UR5E_FIRST, UR5E_SECOND],
    }, group_moves
    target = stack.root / "two-group-workbench" / f"{WORKBENCH_SCENARIO}.scenario"
    target.parent.mkdir()
    candidate = build_scenario_document(
        scenario_id=document["scenario_id"],
        name=document["name"],
        description=(
            "The two-robot Mock workbench with one motion group per robot "
            "(Stop/Resume process test copy)."
        ),
        program_draft=program,
        deployment_draft=document["deployment_draft"],
        manyforge_version=document["saved_status"]["manyforge_version"],
        last_known_status="unknown",
    )
    save_scenario_artifact(
        candidate, str(target), allowed_roots=roots, overwrite=False,
        source_project_path=str(source),
    )
    return target, _Cycle(
        nodes=tuple(nodes),
        group_of=group_of,
        group_moves={group: tuple(moves) for group, moves in group_moves.items()},
        final_goals=final_goals,
        timeout_bound_s=max(sequence_bounds.values()),
    )


# --- Observation ---------------------------------------------------------


@dataclass(frozen=True)
class _Observed:
    at: float
    run: dict[str, Any]
    statuses: Mapping[str, str]
    # Moves whose latest operation is executing (mid-move).
    executing: frozenset[str]


class _Watch:
    """Reads one run as the operator's screen does, plus the moves'
    operations from the execution host's event journal (followed
    incrementally across runs)."""

    def __init__(self, stack: ProcessStack, client: HeadlessHttpClient) -> None:
        self.native = HostProbe(stack.execution_run_root, timeout_seconds=5.0)
        self._client = client
        self._start = _application_start_id(client)
        self._after = 0
        # program_run_id -> accepted operation events, in journal order.
        self._accepted: dict[int, list[dict[str, Any]]] = {}

    def _follow_events(self) -> None:
        while True:
            page = self.native.events(self._after, limit=16)
            for event in page["events"]:
                if event["record_kind"] == "operation_accepted":
                    run_id = event["payload"]["intent"]["program_run_id"]
                    self._accepted.setdefault(run_id, []).append(event)
            self._after = page["next_after_sequence"]
            if not page["has_more"]:
                return

    def accepted(self, program_run_id: int) -> list[dict[str, Any]]:
        self._follow_events()
        return list(self._accepted.get(program_run_id, ()))

    def operations_by_node(self, program_run_id: int) -> dict[str, list[int]]:
        nodes: dict[str, list[int]] = {}
        for event in self.accepted(program_run_id):
            for participant in event["payload"]["intent"]["participants"]:
                span = participant.get("action_span") or {}
                node = span.get("node_instance_id")
                if node is not None:
                    nodes.setdefault(node, []).append(event["operation_id"])
        return nodes

    def run(self, program_run_id: int) -> dict[str, Any]:
        return self._client.program_run(program_run_id, application_start_id=self._start)

    def controls(self, program_run_id: int) -> dict[str, Any]:
        return self._client.program_runtime_controls(
            program_run_id, application_start_id=self._start
        )

    def observe(self, program_run_id: int) -> _Observed:
        run = self.run(program_run_id)
        statuses = dict(run["node_statuses"])
        executing = set()
        for node, operation_ids in self.operations_by_node(program_run_id).items():
            if node in MOVES and statuses.get(node) == "running":
                latest = self.native.read(max(operation_ids))["operation"]
                if latest["state"] == "executing":
                    executing.add(node)
        return _Observed(time.monotonic(), run, statuses, frozenset(executing))

    def joint_positions(self, component_id: str, joint_names: list[str]) -> dict[str, float]:
        response = self.native.observation()
        assert response.get("ok") is True, response
        for row in response["observation"]["targets"]:
            names = row["source_joint_names"]
            if row["controllable_component_id"] == component_id and set(joint_names) <= set(names):
                return {name: row["positions_si"][names.index(name)] for name in joint_names}
        raise AssertionError(
            f"no observed joint row of {component_id} names {joint_names}: {response!r}"
        )


def _group_rows(controls: dict[str, Any]) -> dict[str, dict[str, Any]]:
    selections = controls.get("execution_selections") or {}
    return {row["group_id"]: row for row in selections.get("groups", ())}


def _until(
    read: Callable[[], Any], done: Callable[[Any], bool], *, deadline: float
) -> tuple[bool, Any]:
    """Poll ``read`` until ``done`` holds or the deadline passes: whether it
    held, and the last read."""

    while True:
        value = read()
        if done(value):
            return True, value
        if time.monotonic() >= deadline:
            return False, value
        time.sleep(0.02)


# --- Commands ------------------------------------------------------------


def _runner_command(
    client: HeadlessHttpClient,
    send: Callable[[str], dict[str, Any]],
    *,
    bound_s: float,
) -> dict[str, Any]:
    """Send one runner command (answered at acceptance, OD-214) and follow
    its row to its end. ``send`` builds the body from current reads, so a
    refusal naming a stale fence is retried with a fresh one."""

    refusal: dict[str, Any] = {}
    for attempt in range(1, STALE_ATTEMPTS + 1):
        try:
            reply = send(_request_key())
        except HeadlessClientError as error:
            refusal = {"outcome": "refused", "stage": "before_acceptance",
                       "code": error.code, "details": error.details,
                       "message": str(error), "status_code": error.status_code}
        else:
            row, _cycle = _runner_command_end(client, reply, timeout_seconds=bound_s)
            end = row["end"]
            if end["outcome"] == "succeeded":
                return {"outcome": "succeeded", "row": row, "attempts": attempt}
            refusal = {"outcome": end["outcome"], "stage": "row_end",
                       "code": end["code"], "details": end["details"]}
        if "stale" not in str(refusal.get("code")):
            break
    return {**refusal, "attempts": attempt}


def _stop(
    client: HeadlessHttpClient, program_run_id: int, scope: str, reason: str
) -> dict[str, Any]:
    """The Stop lane answers at once with the Kernel's acceptance."""

    try:
        return client.program_run_motion_stop(program_run_id, {
            "request_key": _request_key(),
            "application_start_id": _application_start_id(client),
            "source_label": SOURCE_LABEL,
            "program_run_id": program_run_id,
            "group_ids": [scope],
            "reason": reason,
        })
    except HeadlessClientError as error:
        return {"ok": False, "code": error.code, "details": error.details,
                "message": str(error)}


def _resume_group(
    client: HeadlessHttpClient, watch: _Watch, program_run_id: int, scope: str,
    *, bound_s: float,
) -> dict[str, Any]:
    """The GUI's Resume of one group, fenced on the current hold revision."""

    def send(request_key: str) -> dict[str, Any]:
        controls = watch.controls(program_run_id)
        return client.program_run_motion_resume(program_run_id, {
            "request_key": request_key,
            "application_start_id": _application_start_id(client),
            "source_label": SOURCE_LABEL,
            "program_run_id": program_run_id,
            "group_ids": [scope],
            "expected_hold_revision": controls["execution_selections"]["hold_revision"],
            "reason": f"resume {scope} after the Stop",
        })

    return _runner_command(client, send, bound_s=bound_s)


def _retry_failure_hold(
    client: HeadlessHttpClient, watch: _Watch, program_run_id: int, node: str,
    *, bound_s: float,
) -> dict[str, Any]:
    """A failure hold's Retry, as the GUI chooses it: the first enabled
    action of the hold's current offer."""

    chosen: dict[str, Any] = {}

    def send(request_key: str) -> dict[str, Any]:
        controls = watch.controls(program_run_id)
        offers = [row["offer"] for row in controls["recovery_offers"]
                  if row["offer"]["node_instance_id"] == node]
        assert len(offers) == 1, f"(d) one failure-hold offer of {node} expected: {controls!r}"
        (offer,) = offers
        actions = [action["action_id"] for action in offer["actions"] if action["enabled"]]
        action_id = next((action for action in FAILURE_RETRY_ACTION_IDS if action in actions), None)
        assert action_id is not None, (
            f"(d) the failure hold of {node} offers no enabled Retry "
            f"({list(FAILURE_RETRY_ACTION_IDS)}): {offer!r}"
        )
        chosen["action_id"] = action_id
        return client.cycle_recovery_action({
            "request_key": request_key,
            "application_start_id": _application_start_id(client),
            "source_label": SOURCE_LABEL,
            "offer_id": offer["offer_id"],
            "offer_revision": offer["offer_revision"],
            "action_id": action_id,
            "reason": f"retry {node} after the Stop",
        })

    result = _runner_command(client, send, bound_s=bound_s)
    return {**result, "action_id": chosen.get("action_id"), "node": node}


# --- One cycle -----------------------------------------------------------


def _execute(
    stack: ProcessStack, client: HeadlessHttpClient, applied: dict[str, Any], label: str
) -> tuple[int, float]:
    """Execute (the run starts held), then Run every group: the run id and
    when Run was accepted."""

    started = _start_run(
        stack, scenario_id=applied["scenario_id"],
        source_document_digest=applied["source_document_digest"], applied=applied,
    )
    _wait_kernel_attention_reconciled(stack)
    _start_motion(client, started, request_id=f"stop-resume.{label}.run")
    return started["capture"]["program_run_id"], time.monotonic()


def _assert_cycle_completed(
    watch: _Watch, cycle: _Cycle, program_run_id: int, terminal: dict[str, Any], label: str
) -> None:
    """The run succeeded with every move at its goal, its motions planned and
    run per robot as in the grouped-execution qualification."""

    assert terminal["state"] == "succeeded", f"(e) {label}: the run did not succeed: {terminal!r}"
    statuses = terminal["node_statuses"]
    assert all(statuses.get(node) == "success" for node in MOVES), (
        f"(e) {label}: not every move reached its goal: {statuses!r}"
    )
    accepted = watch.accepted(program_run_id)
    intents = [event["payload"]["intent"] for event in accepted]
    targets = {
        (participant["target"]["component_id"], participant["target"]["command_target_id"])
        for intent in intents for participant in intent["participants"]
    }
    assert (
        all(intent["coordination_policy"] == "independent" for intent in intents)
        and all(len(intent["participants"]) == 1 for intent in intents)
        and targets == ARM_TARGETS
        and set(watch.operations_by_node(program_run_id)) == set(MOVES)
    ), (
        f"(e) {label}: each move is one independent single-robot operation per "
        f"attempt on its own arm: {intents!r}"
    )
    for component, goal in cycle.final_goals.items():
        observed = watch.joint_positions(component, list(goal))
        assert all(abs(observed[name] - goal[name]) <= GOAL_TOLERANCE_RAD for name in goal), (
            f"(e) {label}: {component} is not at its last goal "
            f"(±{GOAL_TOLERANCE_RAD} rad): observed {observed!r}, goal {dict(goal)!r}"
        )


def _undisturbed_cycle(
    stack: ProcessStack, client: HeadlessHttpClient, watch: _Watch,
    applied: dict[str, Any], cycle: _Cycle,
) -> float:
    """The cycle's normal duration from Run to its end, measured once."""

    program_run_id, run_at = _execute(stack, client, applied, "undisturbed")
    ended, terminal = _until(
        lambda: watch.run(program_run_id),
        lambda run: run["state"] in ENDED_RUN_STATES,
        deadline=run_at + cycle.timeout_bound_s,
    )
    duration = time.monotonic() - run_at
    assert ended, (
        f"the undisturbed cycle did not end within the Program's own timeouts "
        f"({cycle.timeout_bound_s:.0f} s): {terminal!r}"
    )
    _assert_cycle_completed(watch, cycle, program_run_id, terminal, "undisturbed")
    # Undisturbed: exactly one operation per move, no automatic retry.
    operations = watch.operations_by_node(program_run_id)
    assert all(len(operations[node]) == 1 for node in MOVES), operations
    _reset_cycle(client, program_run_id)
    return duration


# --- Stop moments --------------------------------------------------------


@dataclass(frozen=True)
class _Moment:
    name: str
    scope: str
    what: str
    ready: Callable[[_Observed], bool]


MOMENTS = (
    _Moment("all_mid_first_moves", "all",
            "Stop all while both robots are mid their first move",
            lambda seen: {LITE6_FIRST, UR5E_FIRST} <= seen.executing),
    _Moment("lite6_only_mid_move", "lite6_arm",
            "Stop the Lite 6 alone mid-move; the UR5e finishes its move",
            lambda seen: {LITE6_FIRST, UR5E_FIRST} <= seen.executing),
    _Moment("ur5e_only_mid_move", "ur5e_arm",
            "Stop the UR5e alone mid-move; the Lite 6 finishes its move",
            lambda seen: {LITE6_FIRST, UR5E_FIRST} <= seen.executing),
    _Moment("all_just_after_a_move", "all",
            "Stop all just after the Lite 6 completes its first move",
            lambda seen: seen.statuses.get(LITE6_FIRST) == "success"),
    _Moment("group_just_after_its_move", "ur5e_arm",
            "Stop the UR5e alone just after its first move completes; the "
            "Lite 6 is not stopped",
            lambda seen: seen.statuses.get(UR5E_FIRST) == "success"),
    _Moment("all_at_second_moves", "all",
            "Stop all while a second move is executing",
            lambda seen: bool({LITE6_SECOND, UR5E_SECOND} & seen.executing)),
)


def _stop_and_resume(
    stack: ProcessStack, client: HeadlessHttpClient, watch: _Watch,
    applied: dict[str, Any], cycle: _Cycle, moment: _Moment, bound_s: float,
) -> dict[str, Any]:
    label = moment.name
    program_run_id, run_at = _execute(stack, client, applied, label)
    observed, seen = _until(
        lambda: watch.observe(program_run_id),
        lambda seen: moment.ready(seen) or seen.run["state"] in ENDED_RUN_STATES,
        deadline=run_at + bound_s,
    )
    assert observed and moment.ready(seen), (
        f"{label}: the moment ({moment.what}) was not observed within the "
        f"undisturbed cycle duration {bound_s:.1f} s: statuses {dict(seen.statuses)!r}, "
        f"executing {sorted(seen.executing)!r}, run state {seen.run['state']!r}"
    )
    groups = cycle.stop_groups(moment.scope)
    # Under Stop all, a robot that has already finished both moves has
    # nothing left to stop.
    stopped_groups = [moment.scope] + [
        group for group in groups if moment.scope == "all"
        and any(seen.statuses.get(node) != "success" for node in cycle.group_moves[group])
    ]
    peer_moving = (
        sorted(seen.executing - set().union(*(cycle.group_moves[group] for group in groups)))
        if moment.scope != "all" else []
    )

    stop_at = time.monotonic()
    stop = _stop(client, program_run_id, moment.scope, f"Stop/Resume matrix: {moment.what}")
    assert stop.get("ok") is True, f"{label}: the Stop of {moment.scope} was not accepted: {stop!r}"

    # (a) Every stopped group reaches held (or mixed).
    def stopped_groups_held(controls: dict[str, Any]) -> bool:
        rows = _group_rows(controls)
        return all(rows.get(group, {}).get("state") in STOPPED_GROUP_STATES
                   for group in stopped_groups)

    held, controls = _until(
        lambda: watch.controls(program_run_id), stopped_groups_held, deadline=stop_at + bound_s)
    held_s = time.monotonic() - stop_at
    assert held, (
        f"(a) {label}: stopped group(s) {stopped_groups} did not reach held or mixed "
        f"within the undisturbed cycle duration {bound_s:.1f} s after the Stop: "
        f"{_group_rows(controls)!r}"
    )

    # (b) No group stays "Stopping".
    def settled(controls: dict[str, Any]) -> bool:
        rows = _group_rows(controls)
        return stopped_groups_held(controls) and all(
            rows[group]["resume_unavailable_reason"] != STOP_SETTLING_REASON
            for group in stopped_groups
        )

    was_settled, controls = _until(
        lambda: watch.controls(program_run_id), settled, deadline=stop_at + bound_s)
    settle_s = time.monotonic() - stop_at
    assert was_settled, (
        f"(b) {label}: group(s) {stopped_groups} stayed Stopping "
        f"(resume_unavailable_reason {STOP_SETTLING_REASON!r}) for the undisturbed "
        f"cycle duration {bound_s:.1f} s after the Stop: {_group_rows(controls)!r}"
    )

    # (c) Every stopped node shows a recovery.
    statuses = dict(watch.run(program_run_id)["node_statuses"])
    controls = watch.controls(program_run_id)
    rows = _group_rows(controls)
    stopped_nodes = sorted(node for node in cycle.scope_nodes(moment.scope)
                           if statuses.get(node) in {"running", "held"})
    offered = {row["offer"]["node_instance_id"] for row in controls["recovery_offers"]}
    focused = controls.get("recovery_offer")
    if focused is not None:
        offered.add(focused["node_instance_id"])
    failure_holds = sorted(
        {row["offer"]["node_instance_id"] for row in controls["recovery_offers"]}
        & set(stopped_nodes)
    )

    def recoverable(node: str) -> bool:
        if node not in cycle.group_of:
            return True  # a non-motion node: no recovery of its own expected
        return (node in offered
                or rows[moment.scope]["resume_allowed"] is True
                or rows[cycle.group_of[node]]["resume_allowed"] is True)

    unrecoverable = [node for node in stopped_nodes if not recoverable(node)]
    assert not unrecoverable, (
        f"(c) {label}: stopped node(s) {unrecoverable} show no recovery: no offer names "
        f"them and their group's Resume is not allowed: groups {rows!r}, "
        f"recovery_offer {focused!r}, recovery_offers {controls['recovery_offers']!r}"
    )

    # Peer isolation: a robot outside the stopped group keeps moving and
    # finishes the move it was making, while the stopped group stays held.
    peer: dict[str, Any] = {}
    if peer_moving:
        peer_groups = sorted({cycle.group_of[node] for node in peer_moving})

        def peer_read() -> tuple[dict[str, Any], dict[str, Any]]:
            return watch.run(program_run_id), _group_rows(watch.controls(program_run_id))

        violations: list[dict[str, Any]] = []

        def peer_done(value: tuple[dict[str, Any], dict[str, Any]]) -> bool:
            run, rows = value
            peer_stopped = any(rows[group]["state"] in {"held", "stopping", "faulted"}
                               for group in peer_groups)
            if peer_stopped or rows[moment.scope]["state"] not in STOPPED_GROUP_STATES:
                violations.append({"statuses": run["node_statuses"], "groups": rows})
                return True
            return all(run["node_statuses"].get(node) in ENDED_NODE_STATUSES
                       for node in peer_moving)

        finished, (peer_run, peer_rows) = _until(
            peer_read, peer_done, deadline=stop_at + bound_s)
        peer = {"groups": peer_groups, "moves": peer_moving,
                "finished_after_stop_s": round(time.monotonic() - stop_at, 3)}
        assert (finished and not violations
                and all(peer_run["node_statuses"][node] == "success" for node in peer_moving)), (
            f"{label}: the robot outside {moment.scope} must not be held and must finish "
            f"its move(s) {peer_moving} while {moment.scope} stays held, within "
            f"{bound_s:.1f} s: violations {violations!r}, statuses "
            f"{peer_run['node_statuses']!r}, groups {peer_rows!r}"
        )

    # (d) Resume: a failure hold's Retry where one is offered, then the
    # group's Resume while the group is still held.
    unfinished = [node for node in cycle.scope_nodes(moment.scope)
                  if node in cycle.group_of and statuses.get(node) != "success"]
    assert unfinished, f"{label}: the Stop left no move of {moment.scope} to resume: {statuses!r}"
    resume_at = time.monotonic()
    commands = [_retry_failure_hold(client, watch, program_run_id, node, bound_s=bound_s)
                for node in failure_holds]
    rows = _group_rows(watch.controls(program_run_id))
    if not failure_holds or rows[moment.scope]["state"] in STOPPED_GROUP_STATES:
        commands.append({**_resume_group(client, watch, program_run_id, moment.scope,
                                         bound_s=bound_s), "action_id": "resume"})
    for command in commands:
        if command["outcome"] != "succeeded":
            print(json.dumps({"moment": label, "refusal": command}, sort_keys=True, default=str))
            assert isinstance(command.get("code"), str) and command["code"] and isinstance(
                command.get("details"), dict) and command["details"], (
                f"(d) {label}: the {command['action_id']} refusal carries no typed code "
                f"and details: {command!r}"
            )
    assert all(command["outcome"] == "succeeded" for command in commands), (
        f"(d) {label}: the recovery of {moment.scope} was refused: "
        + "; ".join(f"{command['action_id']}: {command.get('code')} {command.get('details')!r}"
                    for command in commands if command["outcome"] != "succeeded")
    )

    # (e) The run continues: a node of the stopped group ends after Resume,
    # and the cycle completes.
    node_ended, after_resume = _until(
        lambda: watch.run(program_run_id),
        lambda run: any(run["node_statuses"].get(node) in ENDED_NODE_STATUSES
                        for node in unfinished) or run["state"] in ENDED_RUN_STATES,
        deadline=resume_at + bound_s,
    )
    node_end_s = time.monotonic() - resume_at
    assert node_ended and any(after_resume["node_statuses"].get(node) == "success"
                              for node in unfinished), (
        f"(e) {label}: no node of {moment.scope} {unfinished} ended with success within "
        f"the undisturbed cycle duration {bound_s:.1f} s after Resume: {after_resume!r}"
    )
    run_ended, terminal = _until(
        lambda: watch.run(program_run_id),
        lambda run: run["state"] in ENDED_RUN_STATES,
        deadline=resume_at + bound_s,
    )
    run_end_s = time.monotonic() - resume_at
    assert run_ended, (
        f"(e) {label}: the run did not end within the undisturbed cycle duration "
        f"{bound_s:.1f} s after Resume: {terminal!r}"
    )
    _assert_cycle_completed(watch, cycle, program_run_id, terminal, label)
    _reset_cycle(client, program_run_id)
    return {
        "moment": label,
        "scope": moment.scope,
        "executing_at_stop": sorted(seen.executing),
        "statuses_at_stop": dict(seen.statuses),
        "stopped_groups": stopped_groups,
        "stopped_nodes": stopped_nodes,
        "held_after_stop_s": round(held_s, 3),
        "settled_after_stop_s": round(settle_s, 3),
        "recovery": [command["action_id"] for command in commands],
        "stale_retries": sum(command["attempts"] - 1 for command in commands),
        "peer": peer,
        "node_end_after_resume_s": round(node_end_s, 3),
        "run_end_after_resume_s": round(run_end_s, 3),
    }


def test_dual_robot_stop_resume_short_matrix(capsys: pytest.CaptureFixture[str]) -> None:
    with _dual_robot_stack() as stack:
        project, cycle = _write_two_group_workbench(stack)
        applied, _digest = _load_scenario(stack, WORKBENCH_SCENARIO, scenario_path=project)
        with HeadlessHttpClient(stack.composer_url, timeout_seconds=30.0) as client:
            watch = _Watch(stack, client)
            bound_s = _undisturbed_cycle(stack, client, watch, applied, cycle)
            results = [
                _stop_and_resume(stack, client, watch, applied, cycle, moment, bound_s)
                for moment in MOMENTS
            ]
        with capsys.disabled():
            print(f"\nStop/Resume matrix (short): undisturbed cycle {bound_s:.2f} s")
            for result in results:
                print("  " + json.dumps(result, sort_keys=True))
