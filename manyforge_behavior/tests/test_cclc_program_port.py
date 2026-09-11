"""Program lifecycle/route nodes remain thin clients of the sole CCLC owner."""

from __future__ import annotations

from copy import deepcopy
from typing import Any

import pytest

from manyforge_behavior.cclc_program_port import CCLCProgramTaskPort
from manyforge_behavior.orchestration_ports import (
    NodeAttemptIdentity,
    OwnerNodeInputs,
    OwnerTaskState,
    OwnerInvocationHeldError,
    OwnerTaskObservationUnavailableError,
    ProgramRunPortContext,
)


_A = "sha256:" + "a" * 64
_B = "sha256:" + "b" * 64
_C = "sha256:" + "c" * 64
_D = "sha256:" + "d" * 64


def _context() -> ProgramRunPortContext:
    return ProgramRunPortContext(
        program_run_id="run.1",
        applied_record_digest="sha256:" + "9" * 64,
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


def _evidence(identity: NodeAttemptIdentity, owner_api: str) -> dict[str, Any]:
    return {
        "program_runner_owner_epoch": "runner.1",
        "program_run_id": identity.program_run_id,
        "parent_program_cycle_claim_generation": 7,
        "node_instance_id": identity.node_instance_id,
        "node_attempt_id": identity.node_attempt_id,
        "owner_api": owner_api,
    }


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

    def authoring_admission(self) -> dict[str, Any]:
        return {"admission_generation": 9, "authoring_allowed": False}

    def component_lifecycle_request_result(self, body):
        return {"result": None}

    def route_selection_request_result(self, body):
        return {"result": None}

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

    def submit_component_lifecycle(self, body: dict[str, Any]) -> dict[str, Any]:
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

    def component_lifecycle_status(self, transaction_id: str) -> dict[str, Any]:
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
    port = CCLCProgramTaskPort(client, program_node_evidence=_evidence)
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
        "expected_admission_generation": 9,
        "program_node_evidence": _evidence(identity, "cclc.lifecycle"),
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


@pytest.mark.parametrize("missing", ["authoring_admission", "component_lifecycle_request_result", "route_selection_request_result"])
def test_runtime_client_requires_both_exact_result_paths(missing):
    client = _Client()
    setattr(client, missing, None)
    with pytest.raises(TypeError, match="exact request-result"):
        CCLCProgramTaskPort(client, program_node_evidence=_evidence)


def test_route_request_copies_one_exact_retained_current_template() -> None:
    client = _Client()
    port = CCLCProgramTaskPort(client, program_node_evidence=_evidence)
    port.bind(_context())
    identity = _identity("route")
    started = port.start(
        "controller.select_route",
        identity,
        OwnerNodeInputs({"target_role": "arm", "route_role": "route.fast"}),
    )
    assert started.state is OwnerTaskState.ACTIVE
    request = client.route_requests[0]
    assert request["program_node_evidence"] == _evidence(identity, "cclc.control_route")
    assert request["expected_admission_generation"] == 9
    assert (
        request["desired_configuration"]
        == client.cclc_route_templates("component.arm")["templates"][0][
            "desired_configuration"
        ]
    )
    assert (
        request["component_expectations"][0]["expected_component_lifecycle_generation"]
        == 4
    )
    assert request["confirmed"] is True
    client.route_statuses["route.1"] = {
        "acceptance": {"route_selection_transaction_id": "route.1"},
        "phase": "succeeded",
        "reconciliation_required": False,
        "terminal_result": "succeeded",
    }
    assert port.observe(started.task_id).state is OwnerTaskState.SUCCEEDED


@pytest.mark.parametrize("route", [False, True])
def test_lost_reply_and_status_error_remain_active_without_resubmission(route):
    client = _Client()
    submit_name = "submit_route_selection" if route else "submit_component_lifecycle"
    result_name = "route_selection_request_result" if route else "component_lifecycle_request_result"
    original_submit = getattr(client, submit_name)
    retained = {}
    queries = []

    def lost(body):
        retained.update(original_submit(body))
        raise TimeoutError("lost response")

    def read_result(body):
        queries.append(deepcopy(body))
        return {"result": retained}

    setattr(client, submit_name, lost)
    setattr(client, result_name, read_result)
    port = CCLCProgramTaskPort(client, program_node_evidence=_evidence)
    port.bind(_context())
    identity = _identity("lost")
    started = port.start(
        "controller.select_route" if route else "lifecycle.enable_component",
        identity,
        OwnerNodeInputs({"target_role": "arm", "route_role": "route.fast"} if route else {"component_role": "robot"}),
    )
    requests = client.route_requests if route else client.lifecycle_requests
    statuses = client.route_statuses if route else client.lifecycle_statuses
    transaction_id = "route.1" if route else "lifecycle.1"
    assert started.state is OwnerTaskState.ACTIVE
    assert started.metadata["outcome_unconfirmed"]
    assert port.observe(started.task_id).state is OwnerTaskState.ACTIVE
    assert queries == requests
    with pytest.raises(OwnerTaskObservationUnavailableError):
        port.observe(started.task_id)  # Missing status is not terminal Failure.
    statuses[transaction_id] = {
        "acceptance": {"route_selection_transaction_id" if route else "lifecycle_transaction_id": transaction_id},
        "phase": "succeeded",
        "reconciliation_required": False,
        "terminal_result": "succeeded",
    }
    assert port.observe(started.task_id).state is OwnerTaskState.SUCCEEDED
    assert len(requests) == 1
    port.acknowledge(started.task_id, identity)


@pytest.mark.parametrize("proven", [True, False])
def test_namespace_refresh_requires_explicit_no_effect_and_rebinds_once(proven):
    client = _Client()
    original = client.submit_component_lifecycle
    attempted = []

    def submit(body):
        attempted.append(deepcopy(body))
        if len(attempted) == 1:
            error = RuntimeError("receiver namespace changed")
            error.code = "cclc_request_namespace_refreshed"
            error.details = {"reason": "settled_request_namespace_checkpoint"}
            if proven:
                error.details["effect"] = "not_started"
            client.cclc_owner_identity = lambda: {"request_owner_epoch": "successor.2"}
            client.authoring_admission = lambda: {"admission_generation": 11}
            raise error
        return original(body)

    client.submit_component_lifecycle = submit
    port = CCLCProgramTaskPort(client, program_node_evidence=_evidence)
    port.bind(_context())
    started = port.start(
        "lifecycle.enable_component",
        _identity("checkpoint"),
        OwnerNodeInputs({"component_role": "robot"}),
    )
    assert started.state is OwnerTaskState.ACTIVE
    if proven:
        assert len(attempted) == 2
        assert attempted[1]["request_context"]["request_owner_epoch"] == "successor.2"
        assert attempted[1]["expected_admission_generation"] == 11
        assert (
            attempted[1]["request_context"]["request_id"]
            == attempted[0]["request_context"]["request_id"]
        )
        assert len(client.lifecycle_requests) == 1
    else:
        assert len(attempted) == 1
        assert started.metadata["outcome_unconfirmed"]
        with pytest.raises(OwnerTaskObservationUnavailableError):
            port.observe(started.task_id)
        assert len(attempted) == 1


def test_repeated_namespace_change_does_not_create_an_unbounded_retry_loop():
    client = _Client()
    attempted = []

    def submit(body):
        attempted.append(deepcopy(body))
        error = RuntimeError("receiver namespace changed")
        error.code = "cclc_request_namespace_refreshed"
        error.details = {
            "reason": "settled_request_namespace_checkpoint",
            "effect": "not_started",
        }
        raise error

    client.submit_component_lifecycle = submit
    port = CCLCProgramTaskPort(client, program_node_evidence=_evidence)
    port.bind(_context())
    started = port.start(
        "lifecycle.enable_component",
        _identity("churn"),
        OwnerNodeInputs({"component_role": "robot"}),
    )
    assert len(attempted) == 2
    assert started.state is OwnerTaskState.ACTIVE
    assert started.metadata["admission_deferred"]
    assert port.observe(started.task_id).metadata["admission_deferred"]
    assert (
        len(attempted) == 4
    )  # Bounded work on an existing wake, no internal retry loop.


@pytest.mark.parametrize("lost_reply", [False, True])
@pytest.mark.parametrize("stop_before_retry", [False, True])
def test_full_namespace_defers_on_existing_wake_and_stop_still_wins(
    lost_reply, stop_before_retry
):
    client = _Client()
    original = client.submit_component_lifecycle
    attempts = []

    def no_admission():
        error = RuntimeError("full namespace is finishing accepted work")
        error.code = "cclc_request_admission_deferred"
        error.details = {"effect": "not_started", "reason": "request_namespace_busy"}
        return error

    def submit(body):
        attempts.append(deepcopy(body))
        if len(attempts) == 1:
            raise (
                TimeoutError("deferral response lost") if lost_reply else no_admission()
            )
        if stop_before_retry:
            error = RuntimeError("Stop won before admission")
            error.code = "invalid_state_transition"
            error.details = {
                "effect": "not_started",
                "reason": "program_node_stop_fence_active",
                "selection_revision": "4",
            }
            raise error
        return original(body)

    def read_result(body):
        assert body == attempts[0]
        raise no_admission()

    client.submit_component_lifecycle = submit
    client.component_lifecycle_request_result = read_result
    port = CCLCProgramTaskPort(client, program_node_evidence=_evidence)
    port.bind(_context())
    started = port.start(
        "lifecycle.enable_component",
        _identity("backpressure"),
        OwnerNodeInputs({"component_role": "robot"}),
    )
    assert started.state is OwnerTaskState.ACTIVE
    if lost_reply:
        assert started.metadata["outcome_unconfirmed"]
        assert port.observe(started.task_id).metadata["admission_deferred"]
    else:
        assert started.metadata["admission_deferred"]
    assert len(attempts) == 1  # The result lookup was observation only.
    client.cclc_owner_identity = lambda: {"request_owner_epoch": "successor.2"}
    if stop_before_retry:
        with pytest.raises(OwnerInvocationHeldError):
            port.observe(started.task_id)
        assert not client.lifecycle_requests
    else:
        assert port.observe(started.task_id).state is OwnerTaskState.ACTIVE
        assert len(client.lifecycle_requests) == 1
    assert len(attempts) == 2
    assert attempts[-1]["request_context"]["request_owner_epoch"] == "successor.2"


@pytest.mark.parametrize(
    "disposition",
    ["request_result_expired", "request_id_conflict", "stale_request_owner_epoch"],
)
def test_missing_original_result_is_not_proof_of_failed_or_unstarted_work(disposition):
    client = _Client()
    original_submit = client.submit_component_lifecycle

    def lost(body):
        original_submit(body)
        raise TimeoutError("accepted reply lost")

    client.submit_component_lifecycle = lost
    client.component_lifecycle_request_result = lambda body: {
        "result": {"replay_status": disposition, "acceptance": None, "status": None}
    }
    port = CCLCProgramTaskPort(client, program_node_evidence=_evidence)
    port.bind(_context())
    started = port.start(
        "lifecycle.enable_component",
        _identity("lost-history"),
        OwnerNodeInputs({"component_role": "robot"}),
    )
    with pytest.raises(OwnerTaskObservationUnavailableError):
        port.observe(started.task_id)
    assert port._tasks[started.task_id].snapshot.state is OwnerTaskState.ACTIVE
    assert len(client.lifecycle_requests) == 1


def test_stop_before_lifecycle_admission_is_unstarted_not_failed_or_unknown() -> None:
    class Held(RuntimeError):
        code = "invalid_state_transition"
        details = {
            "reason": "program_node_stop_fence_active",
            "selection_revision": "0",
            "effect": "not_started",
        }

    client = _Client()

    def held(body):
        raise Held()

    client.submit_component_lifecycle = held
    port = CCLCProgramTaskPort(client, program_node_evidence=_evidence)
    port.bind(_context())
    with pytest.raises(OwnerInvocationHeldError) as error:
        port.start(
            "lifecycle.enable_component",
            _identity("held"),
            OwnerNodeInputs({"component_role": "robot"}),
        )
    assert error.value.selection_revision == 0
    assert not port._tasks and not port._reserved_tasks


@pytest.mark.parametrize("mixed", [False, True])
def test_lost_stop_rejection_is_recovered_as_unstarted_not_resubmitted(mixed):
    client = _Client()

    def lost(body):
        raise TimeoutError("rejection reply was lost")

    client.submit_component_lifecycle = lost
    client.component_lifecycle_request_result = lambda body: {
        "result": {
            "replay_status": "replayed_exact",
            "acceptance": (
                {"lifecycle_transaction_id": "lifecycle.1"} if mixed else None
            ),
            "status": None,
            "rejection": {
                "code": "invalid_state_transition",
                "message": "Held",
                "details": {
                    "effect": "not_started",
                    "reason": "program_node_stop_fence_active",
                    "selection_revision": "12",
                },
            },
        }
    }
    port = CCLCProgramTaskPort(client, program_node_evidence=_evidence)
    port.bind(_context())
    started = port.start(
        "lifecycle.enable_component",
        _identity("lost-held"),
        OwnerNodeInputs({"component_role": "robot"}),
    )
    assert started.state is OwnerTaskState.ACTIVE
    if mixed:
        with pytest.raises(OwnerTaskObservationUnavailableError):
            port.observe(started.task_id)
        assert started.task_id in port._tasks  # Conflicting evidence stays unknown.
        return
    with pytest.raises(OwnerInvocationHeldError) as held:
        port.observe(started.task_id)
    assert held.value.selection_revision == 12
    assert not port._tasks


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
