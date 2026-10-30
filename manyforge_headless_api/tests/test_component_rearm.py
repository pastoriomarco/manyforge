"""m5.5: Recover cell re-arms a moved robot and re-enables it after an incident.

M6 ADAPTERM: a motion command after an idle external move is no longer
refused here; the robot's vendor adapter re-engages it inside the motion."""

from __future__ import annotations

import asyncio
import dataclasses
from typing import Any

import pytest

from manyforge_headless_api.cclc import CCLCOwnerRefusal
from manyforge_headless_api.cell_recover import CellRecoverLedger, recover_cell_once
from manyforge_headless_api.component_rearm import (
    ComponentRearmPorts,
    reenable_components,
)
from manyforge_headless_api.models import ComponentLifecycleRequest
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.tests.test_cell_recover import (
    ESTOP,
    World,
    kernel_result,
    offer,
    snapshot,
)


def component(state: str, phase: str, reason: str | None, incident=None, target=None,
              component_id: str = "arm"):
    row = snapshot(component_id, incident)
    row["component_lifecycle_state"] = state
    row["lifecycle_progress"] = {
        "incident": incident,
        "phase": phase,
        "blocking_reason": reason,
    }
    if target is not None:
        row["lifecycle_target"] = target
    return row


class RearmWorld:
    """Fake CCLC: scripted snapshots and per-intent terminal results."""

    def __init__(self, snapshots: list[dict[str, Any]], terminal: dict[str, Any] | None = None):
        self.snapshots = snapshots
        self.reads = 0
        self.terminal = terminal or {}
        self.submitted: list[ComponentLifecycleRequest] = []
        self.now = 0.0

    def ports(self) -> ComponentRearmPorts:
        async def component_ids() -> list[str]:
            return ["arm"]

        async def component_snapshot(component_id: str) -> dict[str, Any]:
            view = self.snapshots[min(self.reads, len(self.snapshots) - 1)]
            self.reads += 1
            return view

        async def admission_generation() -> int:
            return 9

        async def submit(payload: dict[str, Any]) -> dict[str, Any]:
            request = ComponentLifecycleRequest.model_validate(payload)
            self.submitted.append(request)
            return {
                "acceptance": {"lifecycle_transaction_id": f"lifecycle.{len(self.submitted)}"},
                "replay_status": "accepted_new",
                "status": None,
            }

        async def status(transaction_id: str) -> dict[str, Any]:
            intent = self.submitted[int(transaction_id.split(".")[1]) - 1].intent
            return self.terminal.get(intent, {"terminal_result": "succeeded", "error": None})

        async def sleep(seconds: float) -> None:
            self.now += seconds

        return ComponentRearmPorts(
            component_ids=component_ids,
            component_snapshot=component_snapshot,
            admission_generation=admission_generation,
            submit_lifecycle=submit,
            lifecycle_status=status,
            monotonic=lambda: self.now,
            sleep=sleep,
        )


class GateService:
    """The Headless service as the control dispatch sees it (CCLC-managed cell)."""

    def __init__(self, snapshots: dict[str, dict[str, Any]], *, refuse: set[str] | None = None):
        self.snapshots = snapshots
        self.refuse = refuse or set()
        self.submitted: list[ComponentLifecycleRequest] = []
        self.executed: list[Any] = []
        self.execution_calls: list[str] = []
        self.allowed = True
        cclc = self

        class _CCLC:
            @staticmethod
            def component_snapshot(component_id: str) -> dict[str, Any]:
                return cclc.snapshots[component_id]

            @staticmethod
            def owner_identity() -> dict[str, Any]:
                return {"request_owner_epoch": "cclc-owner.3"}

        self.cclc = _CCLC()

    @staticmethod
    def execution_identity() -> dict[str, Any]:
        return {"ok": True, "identity": {"control_management_mode": "component_lifecycle"}}

    def execution_targets(self) -> dict[str, Any]:
        return {
            "ok": True,
            "targets": {
                "components": [{"controllable_component_id": cid} for cid in self.snapshots]
            },
        }

    @staticmethod
    def authoring_admission() -> dict[str, Any]:
        return {"admission_generation": 9}

    def component_lifecycle_submit(self, request: ComponentLifecycleRequest) -> dict[str, Any]:
        self.submitted.append(request)
        if request.controllable_component_id in self.refuse:
            return {"acceptance": None, "rejection": {"code": "cclc_domain_busy"}}
        return {"acceptance": {"lifecycle_transaction_id": "lifecycle.1"}}

    # The control dispatch's Program and native owner seams.
    def program_cycle_status(self) -> dict[str, Any]:
        return {
            "program_runner_owner_epoch": "runner.1",
            "execution_control_revision": 7,
            "cycle_generation": 3,
            "program_run_id": None,
            "state": "idle_after_reset",
            "allowed_commands": [
                {"command_kind": "operation_start", "allowed": self.allowed,
                 "disabled_reason": None if self.allowed else "command_not_allowed"}
            ],
        }

    def execution_submit(self, request: Any, attribution: Any) -> dict[str, Any]:
        self.execution_calls.append("submit")
        self.executed.append(request)
        return {"ok": True, "accepted": {}}


MOVED = "moved_externally:rearm_at_next_motion"


def _dispatch(service: GateService, payload_update: dict[str, Any] | None = None):
    from manyforge_headless_api.control_dispatch import HeadlessExecutionControlDispatch
    from manyforge_headless_api.control_protocol import (
        ExecutionCommandRequestDTO,
        ExecutionIngressStamp,
    )
    from manyforge_headless_api.tests.test_control_ingress import _Clock, _ingress, _request
    from manyforge_headless_api.tests.test_execution_events import _direct_motion_request

    payload = _direct_motion_request().model_dump(mode="json")
    payload["expected_execution_owner_epoch"] = "kernel.1"
    payload.update(payload_update or {})
    ingress, _ = _ingress(_Clock())
    try:
        request: ExecutionCommandRequestDTO = _request(ingress, kind="operation_start").model_copy(
            update={"payload": payload}
        )
    finally:
        ingress.close()
    return HeadlessExecutionControlDispatch(service)(  # type: ignore[arg-type]
        request,
        ExecutionIngressStamp(
            transport_kind="websocket",
            receiver_incarnation="receiver.1",
            received_server_sequence=1,
            received_monotonic_ns=1,
        ),
    )


def test_a_moved_robots_motion_command_reaches_the_owner_without_a_recover() -> None:
    """M6 ADAPTERM: the control dispatch no longer refuses a motion command
    while a robot reports a move from its own interface. The fake native owner
    records the motion; no CCLC Recover is submitted.
    Adapter behavior is outside this dispatch fixture."""

    moved = GateService({"arm": component("active", "satisfied", MOVED)})
    result, _ = _dispatch(moved)
    assert result.disposition == "received"
    assert len(moved.executed) == 1
    assert moved.executed[0].participants[0].payload.kind == "motion"
    assert moved.execution_calls == ["submit"]
    assert moved.submitted == []

    still_moving = GateService(
        {"arm": component("active", "satisfied", "moved_externally:in_motion")}
    )
    result, _ = _dispatch(still_moving)
    # The owner decides (the adapter refuses a robot still moving, typed).
    assert result.disposition == "received"
    assert len(still_moving.executed) == 1
    assert still_moving.executed[0].participants[0].payload.kind == "motion"
    assert still_moving.execution_calls == ["submit"]
    assert still_moving.submitted == []


def test_malformed_motion_command_reaches_no_owner() -> None:
    moved = GateService({"arm": component("active", "satisfied", MOVED)})
    with pytest.raises(HeadlessApiError) as raised:
        _dispatch(moved, {"unexpected_field": True})
    assert raised.value.code == "execution_control_payload_invalid"
    assert moved.submitted == [] and moved.executed == []
    assert moved.execution_calls == []


@pytest.mark.parametrize("program_id", ["manyforge.direct_control.motion", "ordinary.program"])
@pytest.mark.parametrize("standalone", [False, True])
def test_public_motion_syncs_before_submit_for_any_program_id(program_id, standalone) -> None:
    service = GateService({"arm": component("active", "satisfied", MOVED)})
    if standalone:
        service.program_runner = None
    result, _ = _dispatch(service, {"program_id": program_id})
    assert result.disposition == "received"
    assert service.executed[0].program_id == program_id
    assert service.executed[0].participants[0].payload.kind == "motion"
    assert service.execution_calls == ["submit"]
    assert service.submitted == []


def test_recover_cell_rearms_a_moved_component_with_recover_only() -> None:
    """FIX5: Recover cell is the re-arm. A moved robot (no incident, target
    kept) gets the operator's Recover; the driver re-drives Prepare/Enable."""

    target = {"origin": "operator", "requested_state": "active", "target_generation": 2}
    moved = component("active", "satisfied", MOVED, target=target)
    cell = World(snapshots=[{"arm": moved}])
    rearm_world = RearmWorld([moved])
    result = press(cell, rearm_world, CellRecoverLedger(), "press.1")
    assert result["outcome"] == "done"
    assert [request.intent for request in rearm_world.submitted] == ["recover"]
    assert result["recovered_components"] == ["arm"]


def test_recover_cell_never_rearms_a_robot_still_moving() -> None:
    target = {"origin": "operator", "requested_state": "active", "target_generation": 2}
    moving = component("active", "satisfied", "moved_externally:in_motion", target=target)
    cell = World(snapshots=[{"arm": moving}])
    rearm_world = RearmWorld([moving])
    result = press(cell, rearm_world, CellRecoverLedger(), "press.1")
    assert result["outcome"] == "not_recovered"
    assert result["reason"]["code"] == "component_rearm_required"
    assert result["reason"]["details"]["in_motion"] == "arm"
    assert rearm_world.submitted == []


def test_lost_reenable_reply_is_cell_recover_outcome_unknown() -> None:
    """Known/unknown outcome reporting in Recover cell: a transport failure
    after send is `cell_recover_outcome_unknown`, never a known refusal."""

    target = {"origin": "operator", "requested_state": "active", "target_generation": 2}
    moved = component("active", "satisfied", MOVED, target=target)
    cell = World(snapshots=[{"arm": moved}])
    rearm_world = RearmWorld([moved])
    ports = rearm_world.ports()

    async def lost(payload: dict[str, Any]) -> dict[str, Any]:
        raise HeadlessApiError("cclc_owner_unavailable", "timed out")

    lossy = dataclasses.replace(ports, submit_lifecycle=lost)
    cell_ports = dataclasses.replace(
        cell.ports(),
        reenable=lambda ids, moved_ids, owner_epoch, acknowledge: reenable_components(
            lossy, ids, request_id="press.1",
            owner_epoch=owner_epoch, moved=moved_ids, acknowledge_incident=acknowledge,
        ),
    )
    result, _ = asyncio.run(
        recover_cell_once(CellRecoverLedger(), cell_ports, request_id="press.1",
                          source_label="composer_ui")
    )
    assert result["reason"]["code"] == "cell_recover_outcome_unknown"
    assert result["reason"]["details"]["code"] == "cclc_owner_unavailable"


def estop_reenable_world(terminal: dict[str, Any]) -> tuple[World, RearmWorld]:
    """An e-stop during activity: an incident retired the Active target (CCLC's
    mark, set when the incident opened)."""

    incident = retired_by_incident(
        component("active", "blocked", "incident_open", incident=ESTOP)
    )
    acknowledged = retired_by_incident(component("active", "blocked", None))
    cell = World(snapshots=[{"arm": incident}, {"arm": acknowledged}])
    return cell, RearmWorld([acknowledged], terminal)


def press(cell: World, rearm_world: RearmWorld, ledger: CellRecoverLedger, request_id: str):
    ports = dataclasses.replace(
        cell.ports(),
        reenable=lambda ids, moved, owner_epoch, acknowledge: reenable_components(
            rearm_world.ports(), ids, request_id=request_id,
            owner_epoch=owner_epoch, moved=moved, acknowledge_incident=acknowledge,
        ),
    )
    result, _ = asyncio.run(
        recover_cell_once(ledger, ports, request_id=request_id, source_label="composer_ui")
    )
    return result


def test_estop_still_pressed_reenable_fails_typed_without_escalation_then_retry_succeeds() -> None:
    persists = {
        "recover": {
            "terminal_result": "failed_known_no_effect",
            "error": {
                "code": "execution_rejected",
                "details": {
                    "reason": "xarm_error_persists_after_clear",
                    "vendor_code": "C2",
                    "vendor_status": "C2 mode=0 state=4",
                    "ros2_control_status": "hardware=active controller=inactive "
                    "broadcaster=inactive",
                },
            },
        }
    }
    cell, rearm_world = estop_reenable_world(persists)
    ledger = CellRecoverLedger()
    first = press(cell, rearm_world, ledger, "press.1")
    # The incident is acknowledged (CCLC needs it before any lifecycle step);
    # the re-enable fails typed with the vendor code. Nothing else is
    # submitted: no Prepare, no Enable, no new incident or latch.
    assert first["outcome"] == "not_recovered"
    assert first["reason"]["code"] == "component_rearm_failed"
    assert first["reason"]["details"]["reason"] == "xarm_error_persists_after_clear"
    assert first["reason"]["details"]["vendor_code"] == "C2"
    # M6 XADAPT: with the robot's current status; the Recover changed nothing.
    assert first["reason"]["details"]["vendor_status"] == "C2 mode=0 state=4"
    assert first["reason"]["details"]["ros2_control_status"] == (
        "hardware=active controller=inactive broadcaster=inactive"
    )
    assert first["reason"]["details"]["terminal_result"] == "failed_known_no_effect"
    assert [entry["incident_id"] for entry in first["acknowledged_incidents"]] == [
        "incident.arm.1"
    ]
    assert [request.intent for request in rearm_world.submitted] == ["recover"]
    assert first["recovered_components"] == []

    # Released: the next press (no incident any more) tries again and enables,
    # from CCLC's fact that the incident retired the Active target.
    after = retired_by_incident(component("disabled", "waiting_for_prerequisites", None))
    cell = World(snapshots=[{"arm": after}])
    retry = RearmWorld([after])
    second = press(cell, retry, ledger, "press.2")
    assert second["outcome"] == "done"
    assert [request.intent for request in retry.submitted] == ["recover", "prepare", "enable"]
    assert second["recovered_components"] == ["arm"]
    # Once enabled (an Active target again, so no such fact) it is not
    # re-enabled again.
    enabled = component("active", "satisfied", None, target=ACTIVE_TARGET)
    third = press(World(snapshots=[{"arm": enabled}]), RearmWorld([enabled]), ledger, "press.3")
    assert third["recovered_components"] == []


ACTIVE_TARGET = {"origin": "operator", "requested_state": "active", "target_generation": 3}


def retired_by_incident(row: dict[str, Any]) -> dict[str, Any]:
    """CCLC's fact: an incident retired the Active target, Active not restored."""

    return {**row, "active_target_retired_by_incident": True}


def test_a_press_stopped_after_the_acknowledgement_leaves_the_reenable_to_the_next_press() -> None:
    """M55 live (19:45Z): press 1 acknowledged the incident but stopped at a
    refusal before the re-enable step; press 2 (no open incident) re-enables
    from CCLC's durable fact, not from Headless memory (fresh ledger: as after
    a Headless restart)."""

    incident = retired_by_incident(
        component("active", "blocked", "incident_open", incident=ESTOP)
    )
    acknowledged = retired_by_incident(component("active", "blocked", None))
    cell = World(
        snapshots=[{"arm": incident}, {"arm": acknowledged}],
        offers=[offer()],
        kernel=[kernel_result(False, "program_cycle_state_running")],
    )
    first_rearm = RearmWorld([acknowledged])
    first = press(cell, first_rearm, CellRecoverLedger(), "press.1")
    assert first["outcome"] == "not_recovered"
    assert first["reason"]["code"] == "program_cycle_state_running"
    assert [entry["incident_id"] for entry in first["acknowledged_incidents"]] == [
        "incident.arm.1"
    ]
    assert first_rearm.submitted == []
    assert first["recovered_components"] == []

    # After the Program Reset: no open incident, the latch recovers, and the
    # retired component is enabled again.
    second_rearm = RearmWorld([acknowledged])
    second = press(
        World(snapshots=[{"arm": acknowledged}], offers=[offer()]),
        second_rearm, CellRecoverLedger(), "press.2",
    )
    assert second["outcome"] == "done"
    assert second["acknowledged_incidents"] == []
    assert second["latch_cleared"] is True
    assert [request.intent for request in second_rearm.submitted] == [
        "recover", "prepare", "enable"
    ]
    assert second["recovered_components"] == ["arm"]


PREPARED_TARGET = {"origin": "operator", "requested_state": "prepared", "target_generation": 5}


@pytest.mark.parametrize("headless_restart", [False, True])
def test_a_partial_reenable_left_prepared_is_enabled_by_a_later_press(headless_restart) -> None:
    """SX-647: the re-enable's own Prepare writes a Prepared target, then its
    Enable fails. CCLC keeps the fact (Active not restored), so a later press,
    on the same ledger or after a Headless restart (fresh ledger), enables it
    again instead of reporting done."""

    retired = retired_by_incident(component("disabled", "waiting_for_prerequisites", None))
    refused = {
        "enable": {
            "terminal_result": "failed_known_no_effect",
            "error": {"code": "execution_rejected", "details": {"reason": "estop_engaged"}},
        }
    }
    ledger = CellRecoverLedger()
    first_rearm = RearmWorld([retired], refused)
    first = press(World(snapshots=[{"arm": retired}]), first_rearm, ledger, "press.1")
    assert first["outcome"] == "not_recovered"
    assert first["reason"]["code"] == "component_rearm_failed"
    assert first["reason"]["details"]["step"] == "enable"
    assert [request.intent for request in first_rearm.submitted] == [
        "recover", "prepare", "enable"
    ]
    assert first["recovered_components"] == []

    # CCLC now reports the re-enable's Prepared target, and still the fact.
    prepared = retired_by_incident(
        component("prepared", "satisfied", None, target=PREPARED_TARGET)
    )
    second_rearm = RearmWorld([prepared])
    second = press(
        World(snapshots=[{"arm": prepared}]),
        second_rearm,
        CellRecoverLedger() if headless_restart else ledger,
        "press.2",
    )
    assert second["outcome"] == "done"
    assert [request.intent for request in second_rearm.submitted] == [
        "recover", "prepare", "enable"
    ]
    assert second["recovered_components"] == ["arm"]
    # SX-653: the re-enable's own requests carry its label, so CCLC keeps the
    # mark through its Prepared target.
    assert {
        request.request_context.source_label
        for request in first_rearm.submitted + second_rearm.submitted
    } == {"cell_recover_reenable"}


@pytest.mark.parametrize("operator_target", ["disabled", "prepared"])
def test_an_operator_target_between_reenable_steps_stops_that_reenable(operator_target) -> None:
    """SX-666: CCLC fences each step on the lifecycle generation only, so the
    re-enable re-checks the mark before each step. The operator's own Disable
    (or Prepare-only) accepted after the re-enable's Prepare ends the mark;
    the press then sends no Enable, ends cleanly and does not list it."""

    retired = retired_by_incident(component("disabled", "waiting_for_prerequisites", None))
    newer = component(
        operator_target, "satisfied", None,
        target={"origin": "operator", "requested_state": operator_target,
                "target_generation": 6},
    )
    rearm_world = RearmWorld([retired])
    base = rearm_world.ports()

    async def snapshot(component_id: str) -> dict[str, Any]:
        # The operator's target is accepted right after the re-enable's Prepare.
        if any(request.intent == "prepare" for request in rearm_world.submitted):
            return newer
        return await base.component_snapshot(component_id)

    ports = dataclasses.replace(
        World(snapshots=[{"arm": retired}]).ports(),
        reenable=lambda ids, moved, owner_epoch, acknowledge: reenable_components(
            dataclasses.replace(base, component_snapshot=snapshot), ids,
            request_id="press.1",
            owner_epoch=owner_epoch, moved=moved, acknowledge_incident=acknowledge,
        ),
    )
    result, _ = asyncio.run(
        recover_cell_once(CellRecoverLedger(), ports, request_id="press.1",
                          source_label="composer_ui")
    )
    assert result["outcome"] == "done"
    assert [request.intent for request in rearm_world.submitted] == ["recover", "prepare"]
    assert result["recovered_components"] == []


def test_a_mark_gone_before_the_first_step_sends_nothing() -> None:
    """SX-673: selection was by the mark; if it is already gone when the
    re-enable reads its first snapshot, nothing is submitted, cleanly."""

    rearm_world = RearmWorld([component("prepared", "satisfied", None, target=PREPARED_TARGET)])
    reenabled = asyncio.run(
        reenable_components(
            rearm_world.ports(), ["arm"], request_id="press.1",
            owner_epoch="cclc-owner.3",
        )
    )
    assert reenabled == []
    assert rearm_world.submitted == []


@pytest.mark.parametrize("mark_kept", [False, True])
def test_an_operator_target_between_read_and_submit_is_refused_stale(mark_kept) -> None:
    """SX-673/676: the request is fenced on the lifecycle generation of the
    very snapshot that carried the mark. An admitted operator request advances
    the component watermark in the same journal commit that records its
    target (`sqlite_cclc_journal.cpp` AcceptOrReplayLifecycle: the exact
    watermark check, the immediate-successor rule, the watermark write and
    RecordAcceptedOperatorRequestLocked), so our Prepare built on the prior
    generation is refused (`component_lifecycle_current_snapshot_mismatch`,
    `component_lifecycle_admission.cpp`), nothing done; with the mark gone
    the re-enable stops cleanly. A refusal while the mark is still there
    stays the typed `component_rearm_refused`."""

    retired = retired_by_incident(component("disabled", "waiting_for_prerequisites", None))
    newer = component(
        "disabled", "satisfied", None,
        target={"origin": "operator", "requested_state": "disabled", "target_generation": 6},
    )
    rearm_world = RearmWorld([retired])
    base = rearm_world.ports()
    current = {"row": retired}

    async def snapshot(_component_id: str) -> dict[str, Any]:
        return current["row"]

    async def submit(payload: dict[str, Any]) -> dict[str, Any]:
        request = ComponentLifecycleRequest.model_validate(payload)
        if request.intent != "prepare":
            return await base.submit_lifecycle(payload)
        rearm_world.submitted.append(request)
        # The operator's Disable was accepted after the Prepare's read.
        current["row"] = retired if mark_kept else newer
        return {
            "acceptance": None,
            "rejection": {"code": "component_lifecycle_current_snapshot_mismatch"},
        }

    ports = dataclasses.replace(base, component_snapshot=snapshot, submit_lifecycle=submit)
    if mark_kept:
        with pytest.raises(HeadlessApiError) as refused:
            asyncio.run(
                reenable_components(
                    ports, ["arm"], request_id="press.1",
                    owner_epoch="cclc-owner.3",
                )
            )
        assert refused.value.code == "component_rearm_refused"
        assert refused.value.details["rejection_code"] == (
            "component_lifecycle_current_snapshot_mismatch"
        )
    else:
        reenabled = asyncio.run(
            reenable_components(
                ports, ["arm"], request_id="press.1",
                owner_epoch="cclc-owner.3",
            )
        )
        assert reenabled == []
    assert [request.intent for request in rearm_world.submitted] == ["recover", "prepare"]


def test_a_prepared_target_without_the_fact_is_left_alone() -> None:
    """SX-653: the operator's own Prepare after the incident ends CCLC's mark
    (it is not the re-enable's label), so a later Recover cell leaves the
    component Prepared."""

    prepared = component("prepared", "satisfied", None, target=PREPARED_TARGET)
    rearm_world = RearmWorld([prepared])
    result = press(World(snapshots=[{"arm": prepared}]), rearm_world, CellRecoverLedger(), "press.1")
    assert result["outcome"] == "done"
    assert rearm_world.submitted == []
    assert result["recovered_components"] == []


def test_a_component_enabled_again_manually_is_not_reenabled() -> None:
    """Retired by an incident, then Prepare + Enable by hand: an Active target
    again, so CCLC reports no retirement and the press leaves it alone."""

    enabled = component("active", "satisfied", None, target=ACTIVE_TARGET)
    rearm_world = RearmWorld([enabled])
    result = press(World(snapshots=[{"arm": enabled}]), rearm_world, CellRecoverLedger(), "press.1")
    assert result["outcome"] == "done"
    assert rearm_world.submitted == []
    assert result["recovered_components"] == []


def test_a_component_not_retired_by_an_incident_is_never_reenabled() -> None:
    """The operator's Disable (a Disabled target) and a component without a
    target that no incident retired (for example after a CCLC restart) are
    left alone."""

    disabled_target = {"origin": "operator", "requested_state": "disabled", "target_generation": 4}
    for row in (
        component("disabled", "satisfied", None, target=disabled_target),
        component("active", "waiting_for_prerequisites", None),
    ):
        rearm_world = RearmWorld([row])
        result = press(World(snapshots=[{"arm": row}]), rearm_world, CellRecoverLedger(), "press.1")
        assert result["outcome"] == "done"
        assert rearm_world.submitted == []
        assert result["recovered_components"] == []


def test_item35_the_cclc_owner_epoch_is_read_once_and_pinned_for_the_whole_press() -> None:
    """M6 (item 35): one press reads the CCLC owner epoch once; the incident
    acknowledgement and every re-arm step (Recover, Prepare, Enable) name it,
    even though a re-read would now see another owner."""

    cell, rearm_world = estop_reenable_world({})
    reads: list[str] = []

    async def rotating_epoch() -> str:
        reads.append("read")
        return "cclc-owner.3" if len(reads) == 1 else "cclc-owner.4"

    ports = dataclasses.replace(
        cell.ports(),
        cclc_owner_epoch=rotating_epoch,
        reenable=lambda ids, moved, owner_epoch, acknowledge: reenable_components(
            rearm_world.ports(), ids, request_id="press.1",
            owner_epoch=owner_epoch, moved=moved, acknowledge_incident=acknowledge,
        ),
    )
    result, _ = asyncio.run(
        recover_cell_once(CellRecoverLedger(), ports, request_id="press.1",
                          source_label="composer_ui")
    )

    assert result["outcome"] == "done"
    assert result["recovered_components"] == ["arm"]
    assert reads == ["read"]
    assert [request.request_context.request_owner_epoch for request in cell.lifecycle] == [
        "cclc-owner.3"
    ]
    assert [request.intent for request in rearm_world.submitted] == [
        "recover", "prepare", "enable"
    ]
    assert {request.request_context.request_owner_epoch for request in rearm_world.submitted} == {
        "cclc-owner.3"
    }


def test_item35_a_cclc_restart_mid_press_is_a_typed_stale_epoch_refusal() -> None:
    """The press pinned the old epoch; the restarted CCLC refuses the next
    step `stale_request_owner_epoch` before anything (as CCLC sends it, with
    no `effect` detail). The press reports that confirmed refusal, keeps the
    confirmed acknowledgement, and nothing goes to the new owner under a
    re-read epoch."""

    cell, rearm_world = estop_reenable_world({})

    async def restarted(payload: dict[str, Any]) -> dict[str, Any]:
        rearm_world.submitted.append(ComponentLifecycleRequest.model_validate(payload))
        raise CCLCOwnerRefusal(
            "stale_request_owner_epoch",
            "the lifecycle request targets a stale CCLC owner epoch",
            details={"reason": "request_owner_epoch_stale"},
        )

    stale = dataclasses.replace(rearm_world.ports(), submit_lifecycle=restarted)
    ports = dataclasses.replace(
        cell.ports(),
        reenable=lambda ids, moved, owner_epoch, acknowledge: reenable_components(
            stale, ids, request_id="press.1",
            owner_epoch=owner_epoch, moved=moved, acknowledge_incident=acknowledge,
        ),
    )
    result, _ = asyncio.run(
        recover_cell_once(CellRecoverLedger(), ports, request_id="press.1",
                          source_label="composer_ui")
    )

    assert result["outcome"] == "not_recovered"
    assert result["reason"]["code"] == "stale_request_owner_epoch"
    assert [entry["incident_id"] for entry in result["acknowledged_incidents"]] == [
        "incident.arm.1"
    ]
    assert result["recovered_components"] == []
    [request] = rearm_world.submitted
    assert request.intent == "recover"
    assert request.request_context.request_owner_epoch == "cclc-owner.3"


def test_item36_the_rearm_recover_of_a_pinned_press_acknowledges_no_incident() -> None:
    """S3: the press acknowledges only the incident the operator was shown;
    its re-arm Recover then acknowledges none (an incident opened meanwhile
    refuses it `cclc_incident_open`). Prepare and Enable are unchanged."""

    for shown in (frozenset({"incident.arm.1"}), None):
        cell, rearm_world = estop_reenable_world({})
        ports = dataclasses.replace(
            cell.ports(),
            reenable=lambda ids, moved, owner_epoch, acknowledge: reenable_components(
                rearm_world.ports(), ids, request_id="press.1",
                owner_epoch=owner_epoch, moved=moved, acknowledge_incident=acknowledge,
            ),
        )
        result, _ = asyncio.run(
            recover_cell_once(CellRecoverLedger(), ports, request_id="press.1",
                              source_label="composer_ui", expected_incident_ids=shown)
        )

        assert result["outcome"] == "done"
        [acknowledgement] = cell.lifecycle
        assert acknowledgement.expected_incident_id == (
            None if shown is None else "incident.arm.1"
        )
        assert [request.intent for request in rearm_world.submitted] == [
            "recover", "prepare", "enable"
        ]
        assert [request.acknowledge_incident for request in rearm_world.submitted] == [
            None if shown is None else False, None, None
        ]


def test_an_unmarked_incident_without_a_target_is_only_acknowledged_and_recovered() -> None:
    """Maintainer (SX-668): an open incident on a component with no target and
    no retired-Active mark (for example after an interrupted Close) is
    acknowledged and the latch recovered, as in old main; nothing enables it."""

    incident = component("prepared", "blocked", "incident_open", incident=ESTOP)
    acknowledged = component("prepared", "blocked", None)
    cell = World(snapshots=[{"arm": incident}, {"arm": acknowledged}], offers=[offer()])
    rearm_world = RearmWorld([acknowledged])
    result = press(cell, rearm_world, CellRecoverLedger(), "press.1")
    assert result["outcome"] == "done"
    assert [entry["incident_id"] for entry in result["acknowledged_incidents"]] == [
        "incident.arm.1"
    ]
    assert result["latch_cleared"] is True
    assert rearm_world.submitted == []
    assert result["recovered_components"] == []


def test_component_with_a_present_target_is_left_to_the_target_driver() -> None:
    target = {"origin": "operator", "requested_state": "prepared", "target_generation": 2}
    incident = component("declared", "blocked", "incident_open", incident=ESTOP, target=target)
    cell = World(snapshots=[{"arm": incident}])
    rearm_world = RearmWorld([incident])
    result = press(cell, rearm_world, CellRecoverLedger(), "press.1")
    assert result["outcome"] == "done"
    assert rearm_world.submitted == []


def _slow_world(allowance_ns: int | None, finishes_at: float) -> RearmWorld:
    """A component whose Recover runs for `finishes_at` seconds; the CCLC
    acceptance carries its lifecycle allowance (absent when None)."""

    # SX-666/673: re-enable runs only while CCLC's retired-Active mark holds.
    world = RearmWorld([retired_by_incident(component("disabled", "waiting_for_prerequisites", None))])
    ports = world.ports()
    submit = ports.submit_lifecycle
    status = ports.lifecycle_status

    async def submit_with_allowance(payload: dict[str, Any]) -> dict[str, Any]:
        result = await submit(payload)
        if allowance_ns is not None:
            result["acceptance"]["timeout_duration_ns"] = allowance_ns
        return result

    async def slow_status(transaction_id: str) -> dict[str, Any] | None:
        if world.now < finishes_at:
            return {"terminal_result": None, "error": None}
        return await status(transaction_id)

    world.ports = lambda: dataclasses.replace(  # type: ignore[method-assign]
        ports, submit_lifecycle=submit_with_allowance, lifecycle_status=slow_status
    )
    return world


def _reenable(world: RearmWorld) -> list[str]:
    return asyncio.run(
        reenable_components(
            world.ports(), ["arm"], request_id="press.1",
            owner_epoch="cclc-owner.3",
        )
    )


def test_each_step_waits_within_its_components_own_lifecycle_allowance() -> None:
    """M6 (maintainer, Recover cell timeout): a component that takes 60 s
    to recover, with a 90 s allowance, is not refused at the former fixed
    30 s; Prepare and Enable follow."""

    world = _slow_world(90_000_000_000, finishes_at=60.0)
    assert _reenable(world) == ["arm"]
    assert [request.intent for request in world.submitted] == ["recover", "prepare", "enable"]
    assert world.now >= 60.0


def test_a_step_past_its_allowance_is_a_typed_rearm_timeout_naming_it() -> None:
    world = _slow_world(10_000_000_000, finishes_at=60.0)
    with pytest.raises(HeadlessApiError) as raised:
        _reenable(world)
    assert raised.value.code == "component_rearm_timeout"
    assert raised.value.details["step"] == "recover"
    assert raised.value.details["allowance_s"] == "10.0"
    # The allowance plus the grace, not the former 30 s.
    assert 10.0 <= world.now < 12.0
    assert [request.intent for request in world.submitted] == ["recover"]


def test_an_acceptance_without_an_allowance_keeps_the_10_s_default() -> None:
    world = _slow_world(None, finishes_at=60.0)
    with pytest.raises(HeadlessApiError) as raised:
        _reenable(world)
    assert raised.value.code == "component_rearm_timeout"
    assert raised.value.details["allowance_s"] == "10.0"
    assert 10.0 <= world.now < 12.0


# --- SX-679: abort a running Recover cell ------------------------------------


def _aborting_press(cell: World, rearm_world: RearmWorld, ledger: CellRecoverLedger,
                    request_id: str = "press.1"):
    ports = dataclasses.replace(
        cell.ports(),
        reenable=lambda ids, moved, owner_epoch, acknowledge: reenable_components(
            rearm_world.ports(), ids, request_id=request_id,
            owner_epoch=owner_epoch, moved=moved, acknowledge_incident=acknowledge,
            aborted=lambda: ledger.aborted(request_id),
        ),
    )
    result, _ = asyncio.run(
        recover_cell_once(ledger, ports, request_id=request_id, source_label="composer_ui")
    )
    return result


def _abort_after(rearm_world: RearmWorld, ledger: CellRecoverLedger, *, intent: str,
                 during: bool = False) -> None:
    """Abort when the re-enable submits `intent` (after it settles, or while
    its status is still read: `during`)."""

    base = rearm_world.ports()

    async def submit(payload: dict[str, Any]) -> dict[str, Any]:
        result = await base.submit_lifecycle(payload)
        if payload["intent"] == intent and not during:
            assert ledger.abort_running() == ["press.1"]
        return result

    async def status(transaction_id: str) -> dict[str, Any]:
        if during and rearm_world.submitted[-1].intent == intent:
            ledger.abort_running()
        return await base.lifecycle_status(transaction_id)

    rearm_world.ports = lambda: dataclasses.replace(  # type: ignore[method-assign]
        base, submit_lifecycle=submit, lifecycle_status=status
    )


def test_abort_between_reenable_steps_starts_no_later_step_and_lists_what_ran() -> None:
    cell, rearm_world = estop_reenable_world({})
    ledger = CellRecoverLedger()
    _abort_after(rearm_world, ledger, intent="recover")
    result = _aborting_press(cell, rearm_world, ledger)

    assert result["outcome"] == "not_recovered"
    assert result["reason"]["code"] == "cell_recover_aborted"
    assert [entry["incident_id"] for entry in result["acknowledged_incidents"]] == [
        "incident.arm.1"
    ]
    assert [request.intent for request in rearm_world.submitted] == ["recover"]
    assert result["reason"]["details"] == {
        "before_step": "reenable",
        "reenable_steps": "arm:recover",
        "not_reenabled": "arm",
    }
    assert result["recovered_components"] == []
    assert not ledger.running() and not ledger.aborted("press.1")


def test_abort_during_a_step_lets_it_settle_then_stops() -> None:
    """The step in flight (Prepare) is not cancelled: it settles within its own
    limit and counts as done; Enable is never sent."""

    cell, rearm_world = estop_reenable_world({})
    ledger = CellRecoverLedger()
    _abort_after(rearm_world, ledger, intent="prepare", during=True)
    result = _aborting_press(cell, rearm_world, ledger)

    assert result["reason"]["code"] == "cell_recover_aborted"
    assert [request.intent for request in rearm_world.submitted] == ["recover", "prepare"]
    assert result["reason"]["details"]["reenable_steps"] == "arm:recover+prepare"
    assert result["reason"]["details"]["not_reenabled"] == "arm"


def test_abort_after_the_acknowledgement_sends_no_later_write() -> None:
    """Aborted right after the incident acknowledgement: no latch recovery and
    no re-enable request is sent (the checks sit right before each write)."""

    cell, rearm_world = estop_reenable_world({})
    ledger = CellRecoverLedger()
    acknowledge = cell.ports().submit_lifecycle

    async def acknowledged_then_abort(payload: dict[str, Any]) -> dict[str, Any]:
        result = await acknowledge(payload)
        ledger.abort_running()
        return result

    ports = dataclasses.replace(
        cell.ports(),
        submit_lifecycle=acknowledged_then_abort,
        reenable=lambda ids, moved, owner_epoch, acknowledge: reenable_components(
            rearm_world.ports(), ids, request_id="press.1",
            owner_epoch=owner_epoch, moved=moved, acknowledge_incident=acknowledge,
            aborted=lambda: ledger.aborted("press.1"),
        ),
    )
    result, _ = asyncio.run(
        recover_cell_once(ledger, ports, request_id="press.1", source_label="composer_ui")
    )
    assert result["reason"]["code"] == "cell_recover_aborted"
    assert result["reason"]["details"]["before_step"] == "reenable"
    assert result["reason"]["details"]["not_reenabled"] == "arm"
    assert len(result["acknowledged_incidents"]) == 1
    assert result["latch_cleared"] is False
    assert cell.recoveries == []
    assert rearm_world.submitted == []


def test_the_ledger_knows_a_running_press_and_aborts_only_that() -> None:
    """Close is allowed again after a press finishes or is aborted."""

    ledger = CellRecoverLedger()
    assert not ledger.running() and ledger.abort_running() == []
    assert ledger.begin("press.1") is None
    assert ledger.running()
    assert ledger.abort_running() == ["press.1"] and ledger.aborted("press.1")
    ledger.finish("press.1", {"outcome": "not_recovered"})
    assert not ledger.running() and not ledger.aborted("press.1")


def test_abort_during_a_read_sends_nothing_after_it() -> None:
    """SX-688: the abort lands while the next step reads its fences (here the
    admission generation of the re-enable's Prepare); its write is not sent."""

    cell, rearm_world = estop_reenable_world({})
    ledger = CellRecoverLedger()
    base = rearm_world.ports()

    async def admission_generation() -> int:
        if [request.intent for request in rearm_world.submitted] == ["recover"]:
            ledger.abort_running()
        return await base.admission_generation()

    rearm_world.ports = lambda: dataclasses.replace(  # type: ignore[method-assign]
        base, admission_generation=admission_generation
    )
    result = _aborting_press(cell, rearm_world, ledger)
    assert result["reason"]["code"] == "cell_recover_aborted"
    assert [request.intent for request in rearm_world.submitted] == ["recover"]
    assert result["reason"]["details"]["reenable_steps"] == "arm:recover"


def test_abort_during_the_latch_offer_read_sends_no_latch_recovery() -> None:
    """SX-688: aborted while the press reads the Stop-fault offer."""

    ledger = CellRecoverLedger()
    cell = World(offers=[offer()])
    read = cell.ports().stop_fault

    async def stop_fault() -> dict[str, Any]:
        ledger.abort_running()
        return await read()

    ports = dataclasses.replace(cell.ports(), stop_fault=stop_fault)
    result, _ = asyncio.run(
        recover_cell_once(ledger, ports, request_id="press.1", source_label="composer_ui")
    )
    assert result["reason"]["code"] == "cell_recover_aborted"
    assert result["reason"]["details"]["before_step"] == "latch"
    assert cell.recoveries == []
    assert result["latch_cleared"] is False


def test_abort_during_a_final_step_that_succeeds_reports_the_abort_with_every_step_done() -> None:
    """SX-689: the Enable in flight succeeds; the press still reports the
    accepted abort, with nothing left to do."""

    cell, rearm_world = estop_reenable_world({})
    ledger = CellRecoverLedger()
    _abort_after(rearm_world, ledger, intent="enable", during=True)
    result = _aborting_press(cell, rearm_world, ledger)

    assert result["outcome"] == "not_recovered"
    assert result["reason"]["code"] == "cell_recover_aborted"
    assert result["reason"]["details"] == {
        "before_step": "none",
        "reenable_steps": "arm:recover+prepare+enable",
        "not_reenabled": "",
    }
    assert result["recovered_components"] == ["arm"]
    assert [request.intent for request in rearm_world.submitted] == [
        "recover", "prepare", "enable"
    ]


def test_a_step_failing_after_the_abort_keeps_its_error_and_reports_the_abort() -> None:
    """SX-689: the Prepare in flight fails; its own failure is the reason, with
    `aborted` and the steps done before it."""

    failed = {
        "prepare": {
            "terminal_result": "failed_known_no_effect",
            "error": {"code": "execution_rejected", "details": {"reason": "estop_engaged"}},
        }
    }
    cell, rearm_world = estop_reenable_world(failed)
    ledger = CellRecoverLedger()
    _abort_after(rearm_world, ledger, intent="prepare", during=True)
    result = _aborting_press(cell, rearm_world, ledger)

    assert result["reason"]["code"] == "component_rearm_failed"
    details = result["reason"]["details"]
    assert details["step"] == "prepare" and details["reason"] == "estop_engaged"
    assert details["aborted"] == "true"
    assert details["reenable_steps"] == "arm:recover"
    assert details["not_reenabled"] == "arm"
    assert [request.intent for request in rearm_world.submitted] == ["recover", "prepare"]
