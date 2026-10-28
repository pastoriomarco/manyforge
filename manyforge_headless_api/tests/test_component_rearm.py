"""m5.5: re-arm before motion after an external move; re-enable after an incident."""

from __future__ import annotations

import asyncio
import dataclasses
from typing import Any

import pytest

from manyforge_headless_api.cell_recover import CellRecoverLedger, recover_cell_once
from manyforge_headless_api.component_rearm import (
    REARM_BEFORE_COMMAND_KINDS,
    ComponentRearmPorts,
    reenable_components,
    require_no_external_move,
)
from manyforge_headless_api.models import ComponentLifecycleRequest
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.tests.test_cell_recover import ESTOP, World, snapshot


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

        async def owner_epoch() -> str:
            return "cclc-owner.3"

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
            cclc_owner_epoch=owner_epoch,
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
        self.executed.append(request)
        return {"ok": True, "accepted": {}}


MOVED = "moved_externally:rearm_at_next_motion"


def gate(service: GateService) -> None:
    require_no_external_move(service)


def test_motion_commands_that_need_rearm_first() -> None:
    assert REARM_BEFORE_COMMAND_KINDS == {"operation_start", "program_run"}


def test_moved_robot_refuses_motion_typed_with_no_effect() -> None:
    service = GateService(
        {
            "arm": component("active", "satisfied", MOVED),
            "other": component(
                "active", "satisfied", "moved_externally:in_motion", component_id="other"
            ),
            "ok": component("active", "satisfied", None, component_id="ok"),
        }
    )
    with pytest.raises(HeadlessApiError) as raised:
        gate(service)
    assert raised.value.code == "component_rearm_required"
    assert raised.value.details == {"components": "arm,other", "in_motion": "other"}
    assert service.submitted == []
    only_moved = GateService({"arm": component("active", "satisfied", MOVED)})
    with pytest.raises(HeadlessApiError) as raised:
        gate(only_moved)
    assert raised.value.details == {"components": "arm"}
    assert "Recover cell" in str(raised.value)
    assert only_moved.submitted == []


def test_needs_rearm_is_derived_from_the_robot_status_on_every_command() -> None:
    """No Headless memory: every command reads the driver's warning, derived
    from the adapter's `device.moved_externally` (held until ManyForge's arming
    completes); while the re-arm runs the warning is gone and the owners
    refuse motion for a component that is not active."""

    gate(GateService({"arm": component("active", "satisfied", None)}))
    gate(GateService({"arm": component("disabled", "transition_in_progress", None)}))

    class Native:
        pass

    require_no_external_move(Native())


def _dispatch(service: GateService, payload_update: dict[str, Any] | None = None):
    from manyforge_headless_api.control_dispatch import HeadlessExecutionControlDispatch
    from manyforge_headless_api.control_protocol import (
        ExecutionCommandRequestDTO,
        ExecutionIngressStamp,
    )
    from manyforge_headless_api.tests.test_control_ingress import _Clock, _ingress, _request
    from manyforge_headless_api.tests.test_execution_events import _direct_command_request

    payload = _direct_command_request().model_dump(mode="json")
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


def test_refuse_only_gate_covers_every_transport_with_no_effect() -> None:
    """FIX5: REST named routes, /api/control/commands and /ws/control reach
    the owners only through this dispatch. The gate runs after the ingress's
    freshness and busy checks, the Program availability check and the
    command's validation, and only refuses: no Recover, no owner call."""

    moved = GateService({"arm": component("active", "satisfied", MOVED)})
    result, response = _dispatch(moved)
    assert result.disposition == "rejected"
    assert result.code == "component_rearm_required"
    assert response["error"]["details"]["components"] == "arm"
    assert moved.submitted == []
    assert moved.executed == []

    refused = GateService({"arm": component("active", "satisfied", MOVED)})
    refused.allowed = False
    result, _ = _dispatch(refused)
    assert result.code == "command_not_allowed"

    ready = GateService({"arm": component("active", "satisfied", None)})
    result, _ = _dispatch(ready)
    assert result.disposition == "received"
    assert len(ready.executed) == 1


def test_malformed_motion_command_reaches_neither_gate_nor_owner() -> None:
    moved = GateService({"arm": component("active", "satisfied", MOVED)})
    with pytest.raises(HeadlessApiError) as raised:
        _dispatch(moved, {"unexpected_field": True})
    assert raised.value.code == "execution_control_payload_invalid"
    assert moved.submitted == [] and moved.executed == []


def test_runner_less_headless_is_gated_too() -> None:
    moved = GateService({"arm": component("active", "satisfied", MOVED)})
    moved.program_runner = None  # type: ignore[attr-defined]
    result, _ = _dispatch(moved)
    assert result.code == "component_rearm_required"
    assert moved.executed == []


def test_replay_of_an_old_command_after_a_later_move_is_refused_without_effect() -> None:
    """SX-476 under FIX5: an exact replay of an OLD Execute after a LATER
    manual move is refused `component_rearm_required` before the owner, with
    no effect (no Recover); once re-armed it reaches the owner as before."""

    class ReplayingOwner(GateService):
        def __init__(self, *args: Any, **kwargs: Any) -> None:
            super().__init__(*args, **kwargs)
            self.seen: set[str] = set()

        def execution_submit(self, request: Any, attribution: Any) -> dict[str, Any]:
            self.executed.append(request)
            request_id = request.request_context.request_id
            if request_id in self.seen:
                return {"ok": False, "error": {"code": "request_replay_refused"}}
            self.seen.add(request_id)
            return {"ok": True, "accepted": {}}

    owner = ReplayingOwner({"arm": component("active", "satisfied", None)})
    assert _dispatch(owner)[0].disposition == "received"
    owner.snapshots["arm"] = component("active", "satisfied", MOVED)
    replay, _ = _dispatch(owner)
    assert replay.code == "component_rearm_required"
    assert owner.submitted == [] and len(owner.executed) == 1
    owner.snapshots["arm"] = component("active", "satisfied", None)
    again, _ = _dispatch(owner)
    assert again.code == "request_replay_refused"


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
        reenable=lambda ids, moved_ids: reenable_components(
            lossy, ids, request_id="press.1", source_label="composer_ui", moved=moved_ids
        ),
    )
    result, _ = asyncio.run(
        recover_cell_once(CellRecoverLedger(), cell_ports, request_id="press.1",
                          source_label="composer_ui")
    )
    assert result["reason"]["code"] == "cell_recover_outcome_unknown"
    assert result["reason"]["details"]["code"] == "cclc_owner_unavailable"


def estop_reenable_world(terminal: dict[str, Any]) -> tuple[World, RearmWorld]:
    """An e-stop during activity: an incident retired the Active target."""

    incident = component("active", "blocked", "incident_open", incident=ESTOP)
    acknowledged = component("active", "blocked", None)
    cell = World(snapshots=[{"arm": incident}, {"arm": acknowledged}])
    return cell, RearmWorld([acknowledged], terminal)


def press(cell: World, rearm_world: RearmWorld, ledger: CellRecoverLedger, request_id: str):
    ports = dataclasses.replace(
        cell.ports(),
        reenable=lambda ids, moved: reenable_components(
            rearm_world.ports(), ids, request_id=request_id, source_label="composer_ui",
            moved=moved,
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
                "details": {"reason": "xarm_error_persists_after_clear", "vendor_code": "C2"},
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
    assert [entry["incident_id"] for entry in first["acknowledged_incidents"]] == [
        "incident.arm.1"
    ]
    assert [request.intent for request in rearm_world.submitted] == ["recover"]
    assert first["recovered_components"] == []

    # Released: the next press (no incident any more) tries again and enables.
    after = component("disabled", "waiting_for_prerequisites", None)
    cell = World(snapshots=[{"arm": after}])
    retry = RearmWorld([after])
    second = press(cell, retry, ledger, "press.2")
    assert second["outcome"] == "done"
    assert [request.intent for request in retry.submitted] == ["recover", "prepare", "enable"]
    assert second["recovered_components"] == ["arm"]
    # Once enabled it is not re-enabled again.
    third = press(World(snapshots=[{"arm": after}]), RearmWorld([after]), ledger, "press.3")
    assert third["recovered_components"] == []


def test_component_with_a_present_target_is_left_to_the_target_driver() -> None:
    target = {"origin": "operator", "requested_state": "prepared", "target_generation": 2}
    incident = component("declared", "blocked", "incident_open", incident=ESTOP, target=target)
    cell = World(snapshots=[{"arm": incident}])
    rearm_world = RearmWorld([incident])
    result = press(cell, rearm_world, CellRecoverLedger(), "press.1")
    assert result["outcome"] == "done"
    assert rearm_world.submitted == []
