"""Recover cell composite (recovery design step 0.3): one server-side press."""

from __future__ import annotations

import asyncio
import dataclasses
import os
from pathlib import Path
import socket
import threading
from typing import Any

from fastapi.testclient import TestClient
import pytest
import rfc8785

from manyforge_headless_api.app import create_app
from manyforge_headless_api.cclc import (
    CCLC_OPEN_RESPONSE_VERSION,
    CCLC_SOCKET_FILE,
    CCLCOpenRequestClient,
)
from manyforge_headless_api.cell_recover import (
    CellRecoverLedger,
    CellRecoverPorts,
    derived_request_id,
    recover_cell,
    recover_cell_once,
)
from manyforge_headless_api.models import (
    ComponentLifecycleRequest,
    StopFaultRecoveryIntentRequest,
)
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.tests.test_cclc import _serve_once
from manyforge_headless_api.tests.test_app import (
    FakeCore,
    FakeExecution,
    FakeManager,
    FakeProgramRunner,
    _service,
)

ESTOP = {
    "incident_id": "incident.arm.1",
    "cause_class": "emergency_stop",
    "acknowledged": False,
    "detected_at_ns": 1_000,
    "summary": "E-stop pressed",
    "vendor_codes": ["C2"],
}


def snapshot(component_id: str, incident: dict[str, Any] | None) -> dict[str, Any]:
    return {
        "controllable_component_id": component_id,
        "graph_revision": "graph.1",
        "component_lifecycle_generation": 4,
        "domains": [
            {
                "kind": "controller",
                "active_control_configuration": None,
                "control_domain_id": f"domain.{component_id}",
                "control_domain_revision": "domain.revision.1",
                "controller_topology_id": "topology.1",
                "topology_revision": "topology.revision.1",
                "current_control_bindings": [
                    {
                        "control_binding_id": "binding.1",
                        "control_binding_revision": "binding.revision.1",
                        "control_route_id": "route.normal",
                    }
                ],
                "resources": ["joint.1"],
            }
        ],
        "lifecycle_progress": {"incident": incident, "phase": "blocked"},
    }


def offer(
    status: str = "ready", revision: int = 2, unavailable: str | None = None
) -> dict[str, Any]:
    return {
        "ok": True,
        "command": "get_stop_fault",
        "stop_fault": {
            "recovery_unavailable_reason": unavailable,
            "configuration_expectations": (
                []
                if unavailable
                else [
                    {
                        "control_domain_id": "domain.arm",
                        "active_control_configuration_id": "active.arm",
                        "active_control_configuration_generation": 3,
                    }
                ]
            ),
            "latch": {
                "latch_id": "stop-fault.1.1",
                "latch_generation": 1,
                "recovery_assessment_revision": revision,
                "recovery_status": status,
                "cell_id": "cell.1",
                "retained_bundle_id": "bundle.1",
                "graph_revision": "graph.1",
                "stop_certificate": {
                    "stop_certificate_id": "stop-certificate.1",
                    "stop_certificate_revision": 1,
                    "stop_generation": 1,
                    "stop_id": "stop.1",
                },
            },
        },
    }


NO_LATCH = {"ok": True, "command": "get_stop_fault", "stop_fault": None}


def kernel_result(accepted: bool, code: str | None = None, status: str = "ready") -> dict:
    return {
        "ok": True,
        "command": "recover_stop_fault",
        "result": {
            "accepted": accepted,
            "error": None if code is None else {"code": code, "message": code},
            "latch": offer(status)["stop_fault"]["latch"],
        },
    }


class World:
    """Fake owners: each read returns the next scripted value (the last repeats)."""

    def __init__(
        self,
        *,
        snapshots: list[dict[str, Any]] | None = None,
        offers: list[dict[str, Any]] | None = None,
        kernel: list[dict[str, Any]] | None = None,
        mode: str = "component_lifecycle",
        cclc_result: dict[str, Any] | None = None,
    ) -> None:
        self.snapshots = snapshots or [{"arm": snapshot("arm", None)}]
        self.offers = offers or [NO_LATCH]
        self.kernel = kernel or [kernel_result(True)]
        self.mode = mode
        self.cclc_result = cclc_result
        self.now = 0.0
        self.sleeps = 0
        self.lifecycle: list[ComponentLifecycleRequest] = []
        self.recoveries: list[StopFaultRecoveryIntentRequest] = []
        self.snapshot_reads = 0
        self.acknowledged_incident_id = "incident.arm.1"

    @staticmethod
    def _next(rows: list, index: int):
        return rows[min(index, len(rows) - 1)]

    def ports(self) -> CellRecoverPorts:
        offer_reads = 0

        async def execution_identity() -> dict[str, Any]:
            return {
                "ok": True,
                "identity": {
                    "owner_epoch": "kernel-owner.7",
                    "control_management_mode": self.mode,
                }
            }

        async def execution_targets() -> dict[str, Any]:
            return {
                "ok": True,
                "targets": {
                    "components": [
                        {"controllable_component_id": component_id}
                        for component_id in sorted(self.snapshots[0])
                    ]
                },
            }

        async def cclc_owner_epoch() -> str:
            return "cclc-owner.3"

        async def component_snapshot(component_id: str) -> dict[str, Any]:
            # Before any acknowledgement the first scripted view; afterwards the next.
            view = self._next(self.snapshots, len(self.lifecycle))
            self.snapshot_reads += 1
            return view[component_id]

        async def admission_generation() -> int:
            return 9

        async def submit_lifecycle(payload: dict[str, Any]) -> dict[str, Any]:
            request = ComponentLifecycleRequest.model_validate(payload)
            self.lifecycle.append(request)
            if self.cclc_result is not None:
                return self.cclc_result
            return {
                "acceptance": None,
                "status": None,
                "replay_status": "accepted_new",
                "incident_recovery": {
                    "incident_id": self.acknowledged_incident_id,
                    "controllable_component_id": request.controllable_component_id,
                    "cause_class": "emergency_stop",
                    "acknowledged_by": "composer_ui",
                    "acknowledged_at_ns": 2_000,
                },
            }

        async def stop_fault() -> dict[str, Any]:
            nonlocal offer_reads
            offer_reads += 1
            return self._next(self.offers, offer_reads - 1)

        async def submit_stop_recovery(payload: dict[str, Any]) -> dict[str, Any]:
            self.recoveries.append(StopFaultRecoveryIntentRequest.model_validate(payload))
            return self._next(self.kernel, len(self.recoveries) - 1)

        async def sleep(seconds: float) -> None:
            self.sleeps += 1
            self.now += seconds

        return CellRecoverPorts(
            execution_identity=execution_identity,
            execution_targets=execution_targets,
            cclc_owner_epoch=cclc_owner_epoch,
            component_snapshot=component_snapshot,
            admission_generation=admission_generation,
            submit_lifecycle=submit_lifecycle,
            stop_fault=stop_fault,
            submit_stop_recovery=submit_stop_recovery,
            monotonic=lambda: self.now,
            sleep=sleep,
        )

    def run(self, request_id: str = "press.1") -> dict[str, Any]:
        return asyncio.run(
            recover_cell(self.ports(), request_id=request_id, source_label="composer_ui")
        )


def run(world: World, **overrides: Any) -> dict[str, Any]:
    return asyncio.run(
        recover_cell(
            dataclasses.replace(world.ports(), **overrides),
            request_id="press.1",
            source_label="composer_ui",
        )
    )


def reason_code(result: dict[str, Any]) -> str | None:
    return None if result["reason"] is None else result["reason"]["code"]


def test_incident_only_is_acknowledged_with_server_read_fences() -> None:
    world = World(snapshots=[{"arm": snapshot("arm", ESTOP)}, {"arm": snapshot("arm", None)}])

    result = world.run()

    assert result == {
        "schema_version": "manyforge.cell_recover_result.v1",
        "request_id": "press.1",
        "outcome": "done",
        "acknowledged_incidents": [
            {
                "controllable_component_id": "arm",
                "incident_id": "incident.arm.1",
                "cause_class": "emergency_stop",
            }
        ],
        "latch_cleared": False,
        "recovered_components": [],
        "reason": None,
    }
    [request] = world.lifecycle
    assert request.intent == "recover"
    assert request.request_context.request_owner_epoch == "cclc-owner.3"
    assert request.request_context.request_id == derived_request_id("press.1", "incident.arm")
    assert request.expected_component_lifecycle_generation == 4
    assert request.expected_graph_revision == "graph.1"
    assert request.expected_admission_generation == 9
    assert world.recoveries == []


def test_latch_only_is_recovered_from_the_fresh_offer_and_current_owner() -> None:
    world = World(offers=[offer("ready")])

    result = world.run()

    assert result["outcome"] == "done"
    assert result["latch_cleared"] is True
    assert result["acknowledged_incidents"] == []
    assert world.lifecycle == []
    [recovery] = world.recoveries
    assert recovery.request_context.request_owner_epoch == "kernel-owner.7"
    assert recovery.expected_latch_id == "stop-fault.1.1"
    assert recovery.expected_recovery_assessment_revision == 2
    assert recovery.expected_stop_certificate_id == "stop-certificate.1"
    assert [row.control_domain_id for row in recovery.configuration_expectations] == [
        "domain.arm"
    ]


def test_incident_and_latch_in_one_press_waits_for_the_latch_to_become_ready() -> None:
    # The offer is re-assessed on the host loop after the acknowledgement.
    world = World(
        snapshots=[{"arm": snapshot("arm", ESTOP)}, {"arm": snapshot("arm", None)}],
        offers=[offer("waiting_for_observation", 1), offer("waiting_for_observation", 1), offer("ready", 2)],
    )

    result = world.run()

    assert result["outcome"] == "done"
    assert result["latch_cleared"] is True
    assert len(result["acknowledged_incidents"]) == 1
    assert world.sleeps == 2
    assert len(world.recoveries) == 1


def test_estop_still_pressed_reports_the_reopened_incident_without_waiting() -> None:
    reopened = {**ESTOP, "incident_id": "incident.arm.2"}
    world = World(
        snapshots=[{"arm": snapshot("arm", ESTOP)}, {"arm": snapshot("arm", reopened)}],
        offers=[offer("waiting_for_observation", 1)],
    )

    result = world.run()

    assert result["outcome"] == "not_recovered"
    assert result["reason"]["code"] == "incident_open"
    assert result["reason"]["details"] == {
        "controllable_component_id": "arm",
        "incident_id": "incident.arm.2",
        "cause_class": "emergency_stop",
    }
    assert len(result["acknowledged_incidents"]) == 1
    assert world.sleeps == 0
    assert world.recoveries == []


def test_closing_offer_is_a_confirmed_refusal_without_a_latch_submission() -> None:
    # D6 (SX-366): while Close runs the Kernel offer says recovery is
    # unavailable because Close keeps the latch; no recovery is submitted.
    for reason in ("stop_fault_recovery_closing", "stop_fault_recovery_closing_not_ready"):
        world = World(offers=[offer("ready", unavailable=reason)])

        result = world.run()

        assert reason_code(result) == "stop_fault_recovery_closing"
        assert result["latch_cleared"] is False
        assert world.recoveries == []


def test_closing_refusal_is_a_confirmed_no_effect_refusal() -> None:
    # D6: while Scenario Close runs the Kernel refuses before any write, so the
    # press reports that refusal (not an unknown outcome) and the latch stays.
    world = World(
        offers=[offer("ready")],
        kernel=[{"ok": False, "error": {"code": "stop_fault_recovery_closing",
                                        "message": "Scenario Close keeps the Stop fault"}}],
    )

    result = world.run()

    assert result["outcome"] == "not_recovered"
    assert reason_code(result) == "stop_fault_recovery_closing"
    assert result["latch_cleared"] is False


def test_kernel_error_reply_on_fresh_facts_is_unknown_with_its_code() -> None:
    # A stale ready offer; the Kernel's fresh facts refuse with an error reply.
    # Only `accepted: false` proves no effect, so this is unknown (SX-315).
    world = World(
        offers=[offer("ready")],
        kernel=[{"ok": False, "error": {"code": "incident_open", "message": "open"}}],
    )

    result = world.run()

    assert reason_code(result) == "cell_recover_outcome_unknown"
    assert result["reason"]["details"] == {
        "step": "latch",
        "latch_id": "stop-fault.1.1",
        "code": "incident_open",
        "message": "open",
    }
    assert len(world.recoveries) == 1


def test_arm_at_rest_unproven_ends_when_the_bounded_wait_expires() -> None:
    world = World(offers=[offer("waiting_for_quiescence", 1)])

    result = world.run()

    assert result["outcome"] == "not_recovered"
    assert result["reason"]["code"] == "stop_fault_recovery_waiting_for_quiescence"
    assert result["reason"]["details"] == {
        "latch_id": "stop-fault.1.1",
        "recovery_status": "waiting_for_quiescence",
    }
    assert world.now >= 5.0
    assert world.sleeps == 25
    assert world.recoveries == []


def test_uncertain_operation_found_by_the_kernel_is_waited_on_then_reported() -> None:
    world = World(
        offers=[offer("ready", 2), offer("waiting_for_observation", 3)],
        kernel=[kernel_result(False, "stop_fault_recovery_not_ready", "waiting_for_observation")],
    )

    result = world.run()

    assert reason_code(result) == "stop_fault_recovery_waiting_for_observation"
    assert len(world.recoveries) == 1
    assert world.now >= 5.0


def test_a_moved_assessment_is_retried_once_from_the_fresh_offer() -> None:
    world = World(
        offers=[offer("ready", 2), offer("ready", 3)],
        kernel=[
            kernel_result(False, "stop_fault_recovery_identity_mismatch"),
            kernel_result(True),
        ],
    )

    result = world.run()

    assert result["outcome"] == "done"
    assert result["latch_cleared"] is True
    assert [row.expected_recovery_assessment_revision for row in world.recoveries] == [2, 3]
    assert len({row.request_context.request_id for row in world.recoveries}) == 2
    assert world.sleeps == 0


def test_a_second_identity_mismatch_is_reported_not_retried_again() -> None:
    world = World(
        offers=[offer("ready", 2), offer("ready", 3), offer("ready", 4)],
        kernel=[kernel_result(False, "stop_fault_recovery_identity_mismatch")],
    )

    result = world.run()

    assert reason_code(result) == "stop_fault_recovery_identity_mismatch"
    assert len(world.recoveries) == 2


def test_unavailable_recovery_and_owner_refusals_are_typed() -> None:
    unavailable = World(offers=[offer("ready", unavailable="no_configuration_proof")]).run()
    assert reason_code(unavailable) == "stop_fault_recovery_unavailable"
    assert unavailable["reason"]["details"]["recovery_unavailable_reason"] == (
        "no_configuration_proof"
    )

    stale = World(
        offers=[offer("ready")],
        kernel=[{"ok": False, "error": {"code": "stale_request_owner_epoch"}}],
    ).run()
    assert reason_code(stale) == "cell_recover_outcome_unknown"
    assert stale["reason"]["details"]["code"] == "stale_request_owner_epoch"

    refused = World(
        snapshots=[{"arm": snapshot("arm", ESTOP)}],
        offers=[offer("ready")],
        cclc_result={"acceptance": None, "status": None, "replay_status": "stale_request_owner_epoch"},
    )
    result = refused.run()
    assert reason_code(result) == "incident_acknowledgement_refused"
    assert result["reason"]["details"]["replay_status"] == "stale_request_owner_epoch"
    assert result["acknowledged_incidents"] == []
    assert refused.recoveries == []


def test_owner_errors_after_a_done_step_keep_that_step_in_the_result() -> None:
    world = World(snapshots=[{"arm": snapshot("arm", ESTOP)}, {"arm": snapshot("arm", None)}])

    async def stop_fault() -> dict[str, Any]:
        raise HeadlessApiError("execution_owner_unavailable", "owner gone")

    result = run(world, stop_fault=stop_fault)

    assert reason_code(result) == "execution_owner_unavailable"
    assert len(result["acknowledged_incidents"]) == 1


def test_direct_control_reads_no_component_incidents() -> None:
    world = World(mode="direct", snapshots=[{"arm": snapshot("arm", ESTOP)}], offers=[offer("ready")])

    result = world.run()

    assert result["outcome"] == "done"
    assert world.snapshot_reads == 0
    assert world.lifecycle == []


def offer_for(latch_id: str, status: str = "ready", revision: int = 2) -> dict[str, Any]:
    value = offer(status, revision)
    value["stop_fault"]["latch"]["latch_id"] = latch_id
    return value


def test_sx294_retry_never_switches_to_a_replacing_latch() -> None:
    # Latch A's recovery meets a mismatch; meanwhile ready latch B replaced A.
    world = World(
        offers=[offer_for("stop-fault.1.1", revision=2), offer_for("stop-fault.1.2", revision=1)],
        kernel=[kernel_result(False, "stop_fault_recovery_identity_mismatch")],
    )

    result = world.run()

    assert reason_code(result) == "stop_fault_latch_replaced"
    assert result["reason"]["details"] == {
        "latch_id": "stop-fault.1.1",
        "latch_generation": "1",
        "current_latch_id": "stop-fault.1.2",
        "current_latch_generation": "1",
    }
    assert [row.expected_latch_id for row in world.recoveries] == ["stop-fault.1.1"]


def test_sx294_a_latch_replaced_during_the_wait_is_not_recovered() -> None:
    world = World(
        offers=[offer_for("stop-fault.1.1", "waiting_for_observation"), offer_for("stop-fault.1.2")],
    )

    result = world.run()

    assert reason_code(result) == "stop_fault_latch_replaced"
    assert world.recoveries == []


def test_sx294_reports_the_incident_cclc_actually_acknowledged() -> None:
    # Snapshot shows incident A; by commit CCLC had replaced it with B.
    world = World(snapshots=[{"arm": snapshot("arm", ESTOP)}, {"arm": snapshot("arm", None)}])
    world.acknowledged_incident_id = "incident.arm.2"

    result = world.run()

    assert result["acknowledged_incidents"] == [
        {
            "controllable_component_id": "arm",
            "incident_id": "incident.arm.2",
            "cause_class": "emergency_stop",
        }
    ]


def test_sx294_ledger_replays_without_reading_state() -> None:
    ledger = CellRecoverLedger(maximum=2)
    assert ledger.begin("press.1") is None
    assert reason_code(ledger.begin("press.1")) == "cell_recover_in_progress"
    ledger.finish("press.1", {"outcome": "done", "reason": None})
    assert ledger.begin("press.1") == {"outcome": "done", "reason": None}
    # An error or cancellation is remembered as a typed unknown outcome.
    assert ledger.begin("press.2") is None
    ledger.finish("press.2", None)
    assert reason_code(ledger.begin("press.2")) == "cell_recover_outcome_unknown"
    ledger.begin("press.3")
    ledger.finish("press.3", {"outcome": "done", "reason": None})
    # Bounded: the oldest completed result is dropped.
    assert ledger.begin("press.1") is None


class RecoverExecution(FakeExecution):
    def identity(self) -> dict[str, Any]:
        return {
            "ok": True,
            "command": "identity",
            "identity": {
                "owner_epoch": "kernel-owner.7",
                "control_management_mode": "component_lifecycle",
            },
        }

    def targets(self) -> dict[str, Any]:
        return {
            "ok": True,
            "command": "targets",
            "targets": {"components": [{"controllable_component_id": "arm"}]},
        }

    def stop_fault(self) -> dict[str, Any]:
        self.requests.append(("stop_fault", None))
        return offer("ready")


class RecoverCCLC:
    def __init__(self) -> None:
        self.submitted: list[dict[str, Any]] = []

    def owner_identity(self) -> dict[str, Any]:
        return {"request_owner_epoch": "cclc-owner.3"}

    def component_snapshot(self, component_id: str) -> dict[str, Any]:
        return snapshot(component_id, None if self.submitted else ESTOP)

    def submit_lifecycle(self, intent: dict[str, Any]) -> dict[str, Any]:
        self.submitted.append(intent)
        if len(self.submitted) > 1:
            # m5.5: the re-enable after the incident (Recover, Prepare, Enable).
            return {
                "acceptance": {"lifecycle_transaction_id": f"lifecycle.{len(self.submitted)}"},
                "status": None,
                "replay_status": "accepted_new",
            }
        return {
            "acceptance": None,
            "status": None,
            "replay_status": "accepted_new",
            "incident_recovery": {
                "incident_id": "incident.arm.1",
                "controllable_component_id": "arm",
                "cause_class": "emergency_stop",
                "acknowledged_by": "composer_ui",
                "acknowledged_at_ns": 2_000,
            },
        }


    def lifecycle_status(self, transaction_id: str) -> dict[str, Any]:
        return {"terminal_result": "succeeded", "error": None}


def test_route_takes_only_a_request_identity_and_reads_every_fence() -> None:
    execution = RecoverExecution()
    cclc = RecoverCCLC()
    app = create_app(
        service=_service(
            FakeCore(),
            FakeManager(),
            execution,
            program_runner=FakeProgramRunner(active=False),
            cclc=cclc,
        )
    )
    context = {"request_id": "press.1", "source_label": "composer_ui", "correlation_id": None}
    with TestClient(app) as client:
        echoed = client.post(
            "/api/execution/cell-recover",
            json={"request_context": {**context, "request_owner_epoch": "client.epoch"}},
        )
        response = client.post("/api/execution/cell-recover", json={"request_context": context})
        paths = client.get("/openapi.json").json()["paths"]

    assert echoed.status_code == 422
    assert response.status_code == 200
    assert response.json()["outcome"] == "done"
    assert response.json()["latch_cleared"] is True
    assert "/api/execution/cell-recover" in paths
    # The incident acknowledgement, then (m5.5) the re-enable of the component
    # whose Active target the incident retired.
    lifecycle, *reenable = cclc.submitted
    assert [row["intent"] for row in reenable] == ["recover", "prepare", "enable"]
    assert response.json()["recovered_components"] == ["arm"]
    assert lifecycle["request_context"]["request_owner_epoch"] == "cclc-owner.3"
    assert lifecycle["expected_admission_generation"] >= 1
    [(kind, recovery)] = [row for row in execution.requests if row[0] == "recover_stop_fault"]
    assert recovery.request_context.request_owner_epoch == "kernel-owner.7"
    assert recovery.expected_latch_id == "stop-fault.1.1"


def test_sx294_replayed_noop_press_does_not_touch_a_newer_latch() -> None:
    execution = RecoverExecution()
    cclc = RecoverCCLC()
    cclc.submitted.append({})  # no open incident
    latch: list[dict[str, Any]] = [NO_LATCH]
    execution.stop_fault = lambda: latch[0]  # type: ignore[method-assign]
    app = create_app(
        service=_service(
            FakeCore(),
            FakeManager(),
            execution,
            program_runner=FakeProgramRunner(active=False),
            cclc=cclc,
        )
    )
    body = {"request_context": {"request_id": "press.r", "source_label": "composer_ui"}}
    with TestClient(app) as client:
        first = client.post("/api/execution/cell-recover", json=body).json()
        latch[0] = offer("ready")  # a newer latch opens after press R
        replay = client.post("/api/execution/cell-recover", json=body).json()
        fresh = client.post(
            "/api/execution/cell-recover",
            json={"request_context": {**body["request_context"], "request_id": "press.s"}},
        ).json()

    assert first["outcome"] == "done" and first["latch_cleared"] is False
    assert replay == first
    assert fresh["latch_cleared"] is True
    recoveries = [row for row in execution.requests if row[0] == "recover_stop_fault"]
    assert len(recoveries) == 1


def test_sx295_reads_fail_closed_never_as_direct_or_empty() -> None:
    async def refused() -> dict[str, Any]:
        return {"ok": False, "error": {"code": "execution_owner_not_ready"}}

    async def pending_mode() -> dict[str, Any]:
        return {"ok": True, "identity": {"owner_epoch": "k.1", "control_management_mode": "pending"}}

    async def malformed_row() -> dict[str, Any]:
        return {"ok": True, "targets": {"components": [{"controllable_component_id": ""}]}}

    world = World(offers=[NO_LATCH])
    for overrides, source in (
        ({"execution_identity": refused}, "execution_identity"),
        ({"execution_identity": pending_mode}, "execution_identity"),
        ({"execution_targets": refused}, "execution_targets"),
        ({"execution_targets": malformed_row}, "execution_targets"),
        ({"stop_fault": refused}, "stop_fault"),
    ):
        result = run(world, **overrides)
        assert result["outcome"] == "not_recovered", source
        assert result["reason"]["code"] == "cell_state_unavailable"
        assert result["reason"]["details"]["source"] == source
    assert world.recoveries == [] and world.lifecycle == []


def persisting(component_id: str) -> dict[str, Any]:
    value = snapshot(component_id, None)
    value["lifecycle_progress"] = {
        "incident": None,
        "phase": "blocked",
        "next_intent": "recover",
        "blocking_cause_class": "emergency_stop",
        "blocking_reason": (
            "recovered_cause_persists:emergency_stop:step_failed:"
            "component_lifecycle_postcondition_not_satisfied"
        ),
    }
    return value


def test_sx296_two_press_estop_sequence_submits_the_component_recover_on_press_two() -> None:
    # Press 1, e-stop still engaged: the incident is acknowledged and the latch
    # cleared; the driver's own Recover then fails and leaves the cause persisting.
    world = World(
        snapshots=[{"arm": snapshot("arm", ESTOP)}, {"arm": persisting("arm")}],
        offers=[offer("ready")],
    )
    first = world.run("press.1")
    assert first["outcome"] == "done" and first["latch_cleared"] is True
    assert first["recovered_components"] == []  # no second Recover on press 1
    assert len(world.lifecycle) == 1

    # Released; press 2: no incident, no latch, the cause blocked on recover.
    world.offers = [NO_LATCH]
    world.cclc_result = {
        "acceptance": {"request_id": derived_request_id("press.2", "recover.arm")},
        "status": None,
        "replay_status": "accepted_new",
    }
    second = world.run("press.2")

    assert second["outcome"] == "done"
    assert second["recovered_components"] == ["arm"]
    assert second["acknowledged_incidents"] == []
    recover = world.lifecycle[-1]
    assert recover.intent == "recover"
    assert recover.request_context.request_id == derived_request_id("press.2", "recover.arm")
    assert recover.expected_component_lifecycle_generation == 4
    assert len(world.lifecycle) == 2


def test_sx303_cancel_after_dispatch_then_replay_dispatches_nothing_new() -> None:
    world = World(snapshots=[{"arm": snapshot("arm", ESTOP)}], offers=[offer("ready")])
    ports = world.ports()
    dispatched: list[dict[str, Any]] = []

    async def paused_submit(payload: dict[str, Any]) -> dict[str, Any]:
        dispatched.append(payload)  # the owner request is issued ...
        await asyncio.Event().wait()  # ... and its reply never arrives
        raise AssertionError("unreachable")

    ports = dataclasses.replace(ports, submit_lifecycle=paused_submit)
    ledger = CellRecoverLedger()

    async def scenario() -> tuple[dict[str, Any], bool]:
        press = asyncio.create_task(
            recover_cell_once(ledger, ports, request_id="press.x", source_label="composer_ui")
        )
        while not dispatched:
            await asyncio.sleep(0)
        # Replay while running: in progress, nothing new dispatched.
        running, _ = await recover_cell_once(
            ledger, ports, request_id="press.x", source_label="composer_ui"
        )
        assert reason_code(running) == "cell_recover_in_progress"
        press.cancel()  # the HTTP request is cancelled mid-submission
        try:
            await press
        except asyncio.CancelledError:
            pass
        return await recover_cell_once(
            ledger, ports, request_id="press.x", source_label="composer_ui"
        )

    replay, replayed = asyncio.run(scenario())

    assert replayed is True
    assert reason_code(replay) == "cell_recover_outcome_unknown"
    assert len(dispatched) == 1
    assert world.recoveries == []


def test_sx303_unexpected_error_is_stored_as_unknown_not_released() -> None:
    world = World(snapshots=[{"arm": snapshot("arm", ESTOP)}, {"arm": snapshot("arm", None)}],
                  offers=[offer("ready")])

    async def broken(payload: dict[str, Any]) -> dict[str, Any]:
        world.recoveries.append(payload)  # dispatched, then the reply path fails
        raise RuntimeError("transport broke after dispatch")

    ports = dataclasses.replace(world.ports(), submit_stop_recovery=broken)
    ledger = CellRecoverLedger()

    async def scenario() -> tuple[dict[str, Any], dict[str, Any]]:
        first, _ = await recover_cell_once(ledger, ports, request_id="press.y", source_label="ui")
        replay, _ = await recover_cell_once(ledger, ports, request_id="press.y", source_label="ui")
        return first, replay

    first, replay = asyncio.run(scenario())

    # The original request gets the typed unknown too, with its confirmed step.
    assert reason_code(first) == "cell_recover_outcome_unknown"
    assert len(first["acknowledged_incidents"]) == 1
    assert replay == first
    assert len(world.recoveries) == 1


def test_sx305_owner_accepts_then_the_reply_fails_is_unknown_not_refused() -> None:
    # CCLC accepts the acknowledgement; its reply then fails validation.
    world = World(snapshots=[{"arm": snapshot("arm", ESTOP)}], offers=[offer("ready")])

    async def accepted_then_invalid(payload: dict[str, Any]) -> dict[str, Any]:
        world.lifecycle.append(ComponentLifecycleRequest.model_validate(payload))
        raise HeadlessApiError("cclc_open_response_identity_mismatch", "reply invalid")

    result = run(world, submit_lifecycle=accepted_then_invalid)

    assert reason_code(result) == "cell_recover_outcome_unknown"
    assert result["reason"]["details"] == {
        "step": "incident",
        "controllable_component_id": "arm",
        "code": "cclc_open_response_identity_mismatch",
    }
    assert result["acknowledged_incidents"] == []  # not confirmed, not proven undone
    assert len(world.lifecycle) == 1
    assert world.recoveries == []


def test_sx305_kernel_timeout_after_send_keeps_confirmed_steps() -> None:
    world = World(
        snapshots=[{"arm": snapshot("arm", ESTOP)}, {"arm": snapshot("arm", None)}],
        offers=[offer("ready")],
    )

    async def sent_then_timeout(payload: dict[str, Any]) -> dict[str, Any]:
        world.recoveries.append(StopFaultRecoveryIntentRequest.model_validate(payload))
        raise HeadlessApiError("execution_command_timeout", "no reply")

    result = run(world, submit_stop_recovery=sent_then_timeout)

    assert reason_code(result) == "cell_recover_outcome_unknown"
    assert result["reason"]["details"]["step"] == "latch"
    assert result["reason"]["details"]["code"] == "execution_command_timeout"
    assert len(result["acknowledged_incidents"]) == 1
    assert result["latch_cleared"] is False
    assert len(world.recoveries) == 1


def test_sx305_a_confirmed_ingress_refusal_keeps_its_typed_code() -> None:
    world = World(
        offers=[offer("ready")],
        kernel=[
            {"ok": False, "error": {"code": "command_not_allowed"}, "ingress_rejected": True}
        ],
    )

    assert reason_code(world.run()) == "command_not_allowed"


def test_sx315_kernel_postcommit_conflict_is_unknown_but_accepted_false_is_refused() -> None:
    conflict = World(
        offers=[offer("ready")],
        kernel=[
            {
                "ok": False,
                "error": {
                    "code": "stop_fault_recovery_postcommit_conflict",
                    "message": "Stop fault truth changed after durable clear",
                },
            }
        ],
    )
    unknown = conflict.run()
    assert reason_code(unknown) == "cell_recover_outcome_unknown"
    assert unknown["reason"]["details"]["code"] == "stop_fault_recovery_postcommit_conflict"
    assert unknown["reason"]["details"]["message"] == (
        "Stop fault truth changed after durable clear"
    )
    assert unknown["latch_cleared"] is False  # not confirmed, not proven undone

    refused = World(
        offers=[offer("ready")],
        kernel=[kernel_result(False, "stop_fault_recovery_configuration_mismatch")],
    ).run()
    assert reason_code(refused) == "stop_fault_recovery_configuration_mismatch"


def test_sx309_cancellation_stores_the_steps_confirmed_so_far() -> None:
    world = World(
        snapshots=[{"arm": snapshot("arm", ESTOP)}, {"arm": snapshot("arm", None)}],
        offers=[offer("ready")],
    )
    sent: list[dict[str, Any]] = []

    async def paused_latch(payload: dict[str, Any]) -> dict[str, Any]:
        sent.append(payload)
        await asyncio.Event().wait()
        raise AssertionError("unreachable")

    ports = dataclasses.replace(world.ports(), submit_stop_recovery=paused_latch)
    ledger = CellRecoverLedger()

    async def scenario() -> dict[str, Any]:
        press = asyncio.create_task(
            recover_cell_once(ledger, ports, request_id="press.z", source_label="ui")
        )
        while not sent:
            await asyncio.sleep(0)
        press.cancel()
        try:
            await press
        except asyncio.CancelledError:
            pass
        replay, _ = await recover_cell_once(ledger, ports, request_id="press.z", source_label="ui")
        return replay

    replay = asyncio.run(scenario())

    assert reason_code(replay) == "cell_recover_outcome_unknown"
    assert replay["reason"]["details"] == {"error": "cancelled"}
    assert [row["incident_id"] for row in replay["acknowledged_incidents"]] == ["incident.arm.1"]
    assert len(sent) == 1 and len(world.lifecycle) == 1


def _cclc_submit_port(root: Path):
    client = CCLCOpenRequestClient(root)

    async def submit(payload: dict[str, Any]) -> dict[str, Any]:
        intent = {key: value for key, value in payload.items() if key != "confirmed"}
        return await asyncio.to_thread(client.submit_lifecycle, intent)

    return submit


def _error_envelope(details: dict[str, str]):
    return lambda request: rfc8785.dumps(
        {
            "api_version": CCLC_OPEN_RESPONSE_VERSION,
            "command": request["command"],
            "error": {"code": "stale_request_owner_epoch" if details else
                      "component_lifecycle_journal_result_unknown",
                      "details": details, "message": "owner error"},
            "ok": False,
            "result": None,
        }
    )


@pytest.mark.parametrize(
    ("reply", "expected"),
    [
        # CCLC's known-unstarted refusal (effect=not_started): a confirmed refusal.
        (_error_envelope({"effect": "not_started"}), "stale_request_owner_epoch"),
        # A valid error envelope after a claim or journal step: no proof of no effect.
        (_error_envelope({}), "cell_recover_outcome_unknown"),
        # A reply that fails validation after the send.
        (lambda _request: b'{"ok": true, "api_version":"invalid"}', "cell_recover_outcome_unknown"),
    ],
)
def test_sx312_only_a_known_unstarted_cclc_refusal_is_typed(
    tmp_path: Path, reply, expected: str
) -> None:
    thread = _serve_once(tmp_path, reply)
    world = World(snapshots=[{"arm": snapshot("arm", ESTOP)}], offers=[NO_LATCH])

    result = run(world, submit_lifecycle=_cclc_submit_port(tmp_path))
    thread.join(timeout=2)

    assert not thread.is_alive()
    assert reason_code(result) == expected
    if expected == "cell_recover_outcome_unknown":
        assert result["reason"]["details"]["step"] == "incident"
    assert result["acknowledged_incidents"] == []
    assert world.recoveries == []


def test_sx312_cclc_transport_timeout_after_send_is_unknown(tmp_path: Path) -> None:
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    listener.bind(str(tmp_path / CCLC_SOCKET_FILE))
    os.chmod(tmp_path / CCLC_SOCKET_FILE, 0o600)
    listener.listen(1)
    received: list[bytes] = []
    release = threading.Event()

    def silent_owner() -> None:
        with listener:
            connection, _address = listener.accept()
            with connection:
                received.append(connection.recv(4 * 1024 * 1024))
                release.wait(timeout=5)  # receives, never answers

    thread = threading.Thread(target=silent_owner)
    thread.start()
    world = World(snapshots=[{"arm": snapshot("arm", ESTOP)}], offers=[NO_LATCH])
    client = CCLCOpenRequestClient(tmp_path, timeout_seconds=0.2)

    async def submit(payload: dict[str, Any]) -> dict[str, Any]:
        intent = {key: value for key, value in payload.items() if key != "confirmed"}
        return await asyncio.to_thread(client.submit_lifecycle, intent)

    try:
        result = run(world, submit_lifecycle=submit)
    finally:
        release.set()
        thread.join(timeout=5)

    assert len(received) == 1
    assert reason_code(result) == "cell_recover_outcome_unknown"
    assert result["reason"]["details"]["code"] == "cclc_owner_unavailable"
    assert result["acknowledged_incidents"] == []


class StopRecoveryRefusingRunner(FakeProgramRunner):
    def cycle_status(self) -> dict[str, Any]:
        status = super().cycle_status()
        for row in status["allowed_commands"]:
            if row["command_kind"] == "stop_recovery":
                row.update(allowed=False, disabled_reason="command_not_allowed")
        return status


def test_sx315_route_ingress_refusal_is_typed_and_never_reaches_the_kernel() -> None:
    execution = RecoverExecution()
    cclc = RecoverCCLC()
    cclc.submitted.append({})  # no open incident
    app = create_app(
        service=_service(
            FakeCore(),
            FakeManager(),
            execution,
            program_runner=StopRecoveryRefusingRunner(active=False),
            cclc=cclc,
        )
    )
    body = {"request_context": {"request_id": "press.i", "source_label": "composer_ui"}}
    with TestClient(app) as client:
        result = client.post("/api/execution/cell-recover", json=body).json()

    assert reason_code(result) == "command_not_allowed"
    assert [row for row in execution.requests if row[0] == "recover_stop_fault"] == []
