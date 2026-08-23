"""Program lifecycle/route nodes remain thin clients of the sole CCLC owner."""

from __future__ import annotations

from copy import deepcopy
from typing import Any

from manyforge_behavior.cclc_program_port import CCLCProgramTaskPort
from manyforge_behavior.orchestration_ports import (
    NodeAttemptIdentity,
    OwnerNodeInputs,
    OwnerTaskState,
    ProgramRunPortContext,
)


_A = "sha256:" + "a" * 64
_B = "sha256:" + "b" * 64
_C = "sha256:" + "c" * 64
_D = "sha256:" + "d" * 64


def _context() -> ProgramRunPortContext:
    return ProgramRunPortContext(
        program_run_id="run.1",
        scenario_id="scenario.1",
        scenario_catalog_revision=_A,
        program_id="program.1",
        program_revision=_A,
        program_binding_id=_B,
        retained_bundle_id=_C,
        graph_revision=_D,
        node_environment_digest=_A,
        program_binding={
            "program_binding_id": _B,
            "program_id": "program.1",
            "program_revision": _A,
            "retained_bundle_id": _C,
            "graph_revision": _D,
            "component_role_bindings": [
                {
                    "component_role_id": "robot",
                    "controllable_component_id": "component.arm",
                }
            ],
            "target_role_bindings": [
                {
                    "target_role_id": "arm",
                    "component_role_id": "robot",
                    "controllable_component_id": "component.arm",
                    "capability_bindings": [
                        {
                            "capability_id": "execution.trajectory.execute",
                            "control_binding": {
                                "control_binding_id": "binding.arm",
                                "control_binding_revision": "binding.revision.1",
                            },
                        }
                    ],
                }
            ],
        },
    )


def _identity(attempt: str) -> NodeAttemptIdentity:
    return NodeAttemptIdentity("run.1", attempt, f"attempt:{attempt}")


def _active_configuration() -> dict[str, Any]:
    return {
        "active_control_configuration_id": "active.1",
        "active_control_configuration_generation": 8,
    }


class _Client:
    def __init__(self) -> None:
        self.lifecycle_requests: list[dict[str, Any]] = []
        self.route_requests: list[dict[str, Any]] = []
        self.lifecycle_statuses: dict[str, dict[str, Any]] = {}
        self.route_statuses: dict[str, dict[str, Any]] = {}

    def cclc_owner_identity(self) -> dict[str, Any]:
        return {"request_owner_epoch": "cclc-owner.epoch.1"}

    def cclc_component(self, component_id: str) -> dict[str, Any]:
        return {
            "controllable_component_id": component_id,
            "retained_bundle_id": _C,
            "graph_revision": _D,
            "component_lifecycle_generation": 4,
            "component_lifecycle_state": "prepared",
            "domains": [
                {
                    "control_domain_id": "domain.arm",
                    "control_domain_revision": "domain.revision.1",
                    "controller_topology_id": "topology.arm",
                    "topology_revision": "topology.revision.1",
                    "active_control_configuration": _active_configuration(),
                    "current_control_bindings": [
                        {
                            "control_binding_id": "binding.arm",
                            "control_binding_revision": "binding.revision.1",
                            "control_route_id": "route.normal",
                        }
                    ],
                }
            ],
        }

    def cclc_route_templates(self, component_id: str) -> dict[str, Any]:
        return {
            "controllable_component_id": component_id,
            "retained_bundle_id": _C,
            "graph_revision": _D,
            "templates": [
                {
                    "desired_configuration": {
                        "control_domain_id": "domain.arm",
                        "control_domain_revision": "domain.revision.1",
                        "controller_topology_id": "topology.arm",
                        "topology_revision": "topology.revision.1",
                        "activation_set_id": "activation.arm",
                        "binding_routes": [
                            {
                                "control_binding_id": "binding.arm",
                                "control_binding_revision": "binding.revision.1",
                                "control_route_id": "route.fast",
                            }
                        ],
                    },
                    "expected_active_control_configuration": {
                        "presence": "present",
                        **_active_configuration(),
                    },
                    "component_expectations": [
                        {
                            "controllable_component_id": "component.arm",
                            "expected_component_lifecycle_generation": 4,
                            "expected_component_lifecycle_state": "prepared",
                        }
                    ],
                }
            ],
        }

    def submit_component_lifecycle(
        self, body: dict[str, Any]
    ) -> dict[str, Any]:
        self.lifecycle_requests.append(deepcopy(body))
        request_id = body["request_context"]["request_id"]
        transaction_id = "lifecycle.1"
        return {
            "replay_status": "accepted_new",
            "acceptance": {
                "request_id": request_id,
                "lifecycle_transaction_id": transaction_id,
            },
            "status": None,
        }

    def component_lifecycle_status(
        self, transaction_id: str
    ) -> dict[str, Any]:
        return {"status": self.lifecycle_statuses.get(transaction_id)}

    def submit_route_selection(self, body: dict[str, Any]) -> dict[str, Any]:
        self.route_requests.append(deepcopy(body))
        request_id = body["request_context"]["request_id"]
        transaction_id = "route.1"
        return {
            "replay_status": "accepted_new",
            "acceptance": {
                "request_id": request_id,
                "route_selection_transaction_id": transaction_id,
            },
            "status": None,
        }

    def route_selection_status(self, transaction_id: str) -> dict[str, Any]:
        return {"status": self.route_statuses.get(transaction_id)}


def test_lifecycle_request_uses_fresh_snapshot_and_owner_terminal_truth() -> None:
    client = _Client()
    port = CCLCProgramTaskPort(client)
    port.bind(_context())
    identity = _identity("prepare")
    started = port.start(
        "lifecycle.prepare_component",
        identity,
        OwnerNodeInputs({"component_role": "robot"}),
    )
    assert started.state is OwnerTaskState.ACTIVE
    assert client.lifecycle_requests[0] == {
        "request_context": {
            "request_id": started.task_id,
            "request_owner_epoch": "cclc-owner.epoch.1",
            "source_label": "manyforge_behavior.program",
            "correlation_id": "attempt:prepare",
        },
        "controllable_component_id": "component.arm",
        "intent": "prepare",
        "expected_graph_revision": _D,
        "expected_component_lifecycle_generation": 4,
        "expected_control_domains": [
            {
                "control_domain_id": "domain.arm",
                "control_domain_revision": "domain.revision.1",
                "controller_topology_id": "topology.arm",
                "topology_revision": "topology.revision.1",
                "expected_active_control_configuration": {
                    "presence": "present",
                    **_active_configuration(),
                },
                "expected_control_bindings": [
                    {
                        "control_binding_id": "binding.arm",
                        "control_binding_revision": "binding.revision.1",
                        "control_route_id": "route.normal",
                    }
                ],
            }
        ],
        "reason": "applied-program-node",
        "confirmed": True,
    }
    client.lifecycle_statuses["lifecycle.1"] = {
        "acceptance": {"lifecycle_transaction_id": "lifecycle.1"},
        "phase": "succeeded",
        "reconciliation_required": False,
        "terminal_result": "succeeded",
    }
    terminal = port.observe(started.task_id)
    assert terminal.state is OwnerTaskState.SUCCEEDED
    port.acknowledge(started.task_id, identity)


def test_route_request_copies_one_exact_retained_current_template() -> None:
    client = _Client()
    port = CCLCProgramTaskPort(client)
    port.bind(_context())
    identity = _identity("route")
    started = port.start(
        "controller.select_route",
        identity,
        OwnerNodeInputs({"target_role": "arm", "route_role": "route.fast"}),
    )
    assert started.state is OwnerTaskState.ACTIVE
    request = client.route_requests[0]
    assert request["desired_configuration"] == client.cclc_route_templates(
        "component.arm"
    )["templates"][0]["desired_configuration"]
    assert request["component_expectations"][0][
        "expected_component_lifecycle_generation"
    ] == 4
    assert request["confirmed"] is True
    client.route_statuses["route.1"] = {
        "acceptance": {"route_selection_transaction_id": "route.1"},
        "phase": "succeeded",
        "reconciliation_required": False,
        "terminal_result": "succeeded",
    }
    assert port.observe(started.task_id).state is OwnerTaskState.SUCCEEDED


def test_stale_context_and_unknown_route_fail_without_owner_submission() -> None:
    client = _Client()
    port = CCLCProgramTaskPort(client)
    port.bind(_context())
    original = client.cclc_component
    client.cclc_component = lambda component_id: {  # type: ignore[method-assign]
        **original(component_id),
        "graph_revision": _A,
    }
    stale = port.start(
        "lifecycle.enable_component",
        _identity("stale"),
        OwnerNodeInputs({"component_role": "robot"}),
    )
    assert stale.state is OwnerTaskState.FAILED
    assert stale.reason_code == "cclc_program_context_stale"
    assert client.lifecycle_requests == []

    missing = port.start(
        "controller.select_route",
        _identity("missing-route"),
        OwnerNodeInputs({"target_role": "arm", "route_role": "route.unknown"}),
    )
    assert missing.state is OwnerTaskState.FAILED
    assert missing.reason_code == "cclc_route_template_ambiguous"
    assert client.route_requests == []
