"""Operator confirmation uses the existing strict control ingress."""

from copy import deepcopy
import asyncio
import hashlib

import pytest
import rfc8785
from pydantic import ValidationError

from manyforge_headless_api.execution import ExecutionCommandClient
from manyforge_headless_api.models import (
    GripperConfirmationRequest,
    TargetObservationQuery,
)
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.control_dispatch import HeadlessExecutionControlDispatch
from manyforge_headless_api.control_protocol import (
    ControlFreshnessAuthority,
    ExecutionCommandRequestDTO,
    EXECUTION_COMMAND_SCHEMA,
)
from manyforge_headless_api.execution_control import ExecutionControlIngress
from manyforge_headless_api.service import HeadlessService, TrustedAttribution


def _request():
    return {
        "schema_version": "manyforge.gripper_confirmation_request.v1",
        "program_binding_id": "sha256:" + "a" * 64,
        "target_role_id": "gripper",
        "command_id": 7,
        "generation": 2,
        "expected_status_revision": 4,
        "confirmed": True,
        "request_context": {
            "request_id": "press.1",
            "request_owner_epoch": "native.1",
            "source_label": "station-a",
            "correlation_id": None,
        },
    }


@pytest.mark.parametrize(
    "change",
    [
        {"confirmed": 1},
        {"command_id": True},
        {"generation": 0},
        {"expected_status_revision": 0},
        {"observed_at_ns": 100},
        {"valid_until_monotonic_ns": 100},
        {"source_id": "unqualified"},
        {"source_kind": "perception"},
        {"validity_ns": 100},
        {"target_role_id": ""},
        {"program_binding_id": "latest"},
    ],
)
def test_operator_cannot_choose_owner_time_or_source_policy(change):
    with pytest.raises(ValidationError):
        GripperConfirmationRequest.model_validate({**_request(), **change})


def _client():
    client = object.__new__(ExecutionCommandClient)
    client._attribution_transport = "http"
    received = []
    time_ns = 31 * 365 * 24 * 60 * 60 * 1_000_000_000
    response = {
        "api_version": "manyforge.execution_command_response.v1",
        "command": "gripper_confirm",
        "ok": True,
        "owner_epoch": "native.1",
        "program_binding_id": _request()["program_binding_id"],
        "target_role_id": "gripper",
        "confirmation": {
            "source_kind": "operator",
            "source_id": "confirmation.source",
            "source_revision": "sha256:" + "b" * 64,
            "source_epoch": "provider.1",
            "observation_id": "sha256:"
            + hashlib.sha256(rfc8785.dumps(["control-session.1", "press.1"])).hexdigest(),
            "attribution": "station-a",
            "command_id": 7,
            "generation": 2,
            "confirmed": True,
            "observed_at_ns": str(time_ns),
            "expires_at_ns": str(time_ns + 1_000_000_000),
        },
    }

    def exchange(command):
        received.append(deepcopy(command))
        return deepcopy(response)

    client._exchange = exchange
    return client, received, response, time_ns


def _send(client, time_ns):
    return client.confirm_grasp(
        GripperConfirmationRequest.model_validate(_request()),
        trusted_principal_id=None,
        origin_epoch="control-session.1",
        received_monotonic_ns=time_ns,
        valid_until_monotonic_ns=time_ns + 500_000_000,
    )


def test_private_hop_preserves_exact_time_origin_and_operator_not_sensor_proof():
    client, received, response, time_ns = _client()
    assert _send(client, time_ns) == response
    assert len(received) == 1
    assert received[0]["origin_epoch"] == "control-session.1"
    assert received[0]["request"] == _request()
    assert received[0]["received_monotonic_ns"] == str(time_ns)
    assert received[0]["valid_until_monotonic_ns"] == str(time_ns + 500_000_000)
    assert received[0]["attribution"]["assurance"] == "unverified"
    assert "validity_ns" not in received[0]


@pytest.mark.parametrize(
    "change",
    [
        {"source_kind": "perception"},
        {"source_revision": "current"},
        {"command_id": 8},
        {"generation": 3},
        {"confirmed": False},
        {"observed_at_ns": True},
        {"observed_at_ns": 2**53},
        {"expires_at_ns": "01"},
        {"expires_at_ns": "1"},
        {"observation_id": "sha256:" + "c" * 64},
        {"attribution": "station-b"},
        {"extra": "unqualified"},
    ],
)
def test_invalid_or_mismatched_evidence_is_never_repaired(change):
    client, _, response, time_ns = _client()
    response["confirmation"].update(change)
    with pytest.raises(HeadlessApiError) as error:
        _send(client, time_ns)
    assert error.value.code == "grasp_confirmation_response_invalid"


@pytest.mark.parametrize(
    "field,value",
    [
        ("owner_epoch", "native.old"),
        ("program_binding_id", "sha256:" + "b" * 64),
        ("target_role_id", "other-gripper"),
    ],
)
def test_same_command_counters_do_not_identify_a_different_owner_or_target(field, value):
    client, _, response, time_ns = _client()
    response[field] = value
    with pytest.raises(HeadlessApiError):
        _send(client, time_ns)


def test_confirmation_cannot_be_observed_after_its_receiver_admission_time():
    client, _, response, time_ns = _client()
    response["confirmation"]["observed_at_ns"] = str(time_ns + 1)
    with pytest.raises(HeadlessApiError):
        _send(client, time_ns)


def test_existing_ingress_preserves_caller_namespace_while_fencing_native_owner():
    class Service:
        program_runner = object()

        def __init__(self):
            self.calls = []

        def program_cycle_status(self):
            return {
                "program_runner_owner_epoch": "runner.1",
                "execution_control_revision": 7,
                "state": "running",
                "allowed_commands": [
                    {
                        "command_kind": "gripper_confirm",
                        "allowed": True,
                        "disabled_reason": None,
                    }
                ],
            }

        def execution_confirm_grasp(self, request, attribution, **stamp):
            self.calls.append((request, attribution, stamp))
            return {"ok": True}

    now = 1_000_000_000
    service = Service()
    dispatch = HeadlessExecutionControlDispatch(service)
    ingress = ExecutionControlIngress(
        projection=dispatch.owner_projection,
        dispatch=dispatch,
        authority=ControlFreshnessAuthority(clock_ns=lambda: now),
        clock_ns=lambda: now,
    )
    freshness = ingress.current_freshness()
    requests = []
    for session in ("control.a", "control.b"):
        payload = _request()
        payload["expected_execution_owner_epoch"] = "native.1"
        requests.append(
            ExecutionCommandRequestDTO.model_validate(
                {
                    "command_envelope": {
                        "schema_version": EXECUTION_COMMAND_SCHEMA,
                        "command_kind": "gripper_confirm",
                        "correlation_id": "press.1",
                        "source_label": "station-a",
                        "control_session_id": session,
                        "client_sequence": "1",
                        "freshness": freshness.model_dump(),
                        "expected_semantic_owner_epoch": "runner.1",
                        "expected_execution_control_revision": 7,
                    },
                    "payload": payload,
                }
            )
        )
    now += 200_000_000
    try:
        for request in requests:
            result = asyncio.run(ingress.submit(request, transport_kind="websocket"))
            assert result.receipt.disposition == "received"
    finally:
        ingress.close()
    assert [row[2]["origin_epoch"] for row in service.calls] == [
        "control.a",
        "control.b",
    ]
    for request, _, stamp in service.calls:
        assert request.request_context.request_owner_epoch == "native.1"
        assert request.request_context.request_id == "press.1"
        assert stamp["received_monotonic_ns"] == now
        assert stamp["valid_until_monotonic_ns"] == 1_000_000_000 + freshness.valid_for_ns


@pytest.mark.parametrize(
    "outcome",
    [
        "accepted",
        "negative",
        "lost",
        "malformed",
        "history_full",
        "attempt_trace_lost",
        "outcome_trace_lost",
        "fallback_trace_lost",
    ],
)
def test_history_records_intent_and_truth_without_retry_or_changing_the_owner_result(
    outcome,
    monkeypatch,
):
    client, sends, response, now = _client()
    events = []

    class Core:
        def record_gripper_confirmation(self, record):
            events.append(deepcopy(record))
            if (len(events) == 1 and outcome == "attempt_trace_lost") or (
                len(events) == 2 and outcome == "outcome_trace_lost"
            ):
                raise HeadlessApiError("headless_api_closed", "owner closed")
            return {"recorded": outcome not in {"history_full", "fallback_trace_lost"}}

    if outcome == "fallback_trace_lost":
        import logging

        def broken_sink(*args, **kwargs):
            raise RuntimeError("configured fallback sink failed")

        monkeypatch.setattr(
            logging.getLogger("manyforge_headless_api.service"), "warning", broken_sink
        )

    if outcome == "lost":

        def lost(command):
            sends.append(command)
            raise HeadlessApiError("execution_command_unavailable", "connection lost")

        client._exchange = lost
    if outcome == "malformed":
        response["confirmation"]["command_id"] = 8
    if outcome == "negative":
        response["confirmation"]["confirmed"] = False
    service = HeadlessService(Core(), None, execution=client)

    def send():
        return service.execution_confirm_grasp(
            GripperConfirmationRequest.model_validate(
                {**_request(), "confirmed": outcome != "negative"}
            ),
            TrustedAttribution(),
            origin_epoch="control-session.1",
            received_monotonic_ns=now,
            valid_until_monotonic_ns=now + 500_000_000,
        )

    if outcome in {"lost", "malformed"}:
        with pytest.raises(HeadlessApiError):
            send()
    else:
        result = send()
        assert result["ok"] is True
        assert result["history_recorded"] is (outcome in {"accepted", "negative"})
        assert result["confirmation"]["confirmed"] is (outcome != "negative")
    assert len(sends) == 1
    assert events[0]["phase"] == "attempted"
    assert "observed_at_ns" not in events[0]
    assert events[0]["details"] == {"principal_id": None, "assurance": "unverified"}
    if outcome in {"lost", "malformed"}:
        assert events[1]["phase"] == "unconfirmed"
        assert "evidence" not in events[1]["details"]
    else:
        assert events[1]["phase"] == "accepted"
        assert events[1]["details"]["evidence"]["source_kind"] == "operator"


@pytest.mark.parametrize(
    "change",
    [
        {},
        {"evidence": None},
        {"command_id": 8},
        {"grasp_confirmed": False},
        {"active_control_configuration_generation": 2**53},
        {"source_id": "other-source"},
        {"extra": 1},
    ],
)
def test_confirmation_reads_are_closed_and_bound_to_the_current_command(change, monkeypatch):
    client, _, result, now = _client()
    monkeypatch.setattr("manyforge_headless_api.execution.time.monotonic_ns", lambda: now)
    evidence = result["confirmation"]
    query = TargetObservationQuery.model_validate(
        {
            "schema_version": "manyforge.target_observation_query.v1",
            "owner_epoch": "native.1",
            "program_binding_id": _request()["program_binding_id"],
            "target_role_id": "gripper",
            "kind": "gripper_confirmation",
        }
    )
    value = {
        "state": "completed",
        "command_id": 7,
        "generation": 2,
        "revision": 5,
        "source_id": evidence["source_id"],
        "source_revision": evidence["source_revision"],
        "source_epoch": evidence["source_epoch"],
        "active_control_configuration_id": "route.1",
        "active_control_configuration_generation": 3,
        "grasp_confirmed": True,
        "evidence": evidence,
        **change,
    }
    response = {
        "api_version": "manyforge.execution_command_response.v1",
        "command": "query_target",
        "ok": True,
        "target_observation": {
            **query.model_dump(exclude_none=True),
            "schema_version": "manyforge.target_observation.v1",
            "source_epoch": evidence["source_epoch"],
            "source_sequence": 5,
            "value": value,
        },
    }
    client._exchange = lambda _: response
    if change:
        with pytest.raises(HeadlessApiError):
            client.query_target(query)
    else:
        assert client.query_target(query) == response


@pytest.mark.parametrize(
    "elapsed_ns,confirmed",
    [
        (-1, True),
        (999_999_999, True),
        (1_000_000_000, True),
        (1_000_000_001, False),
        (1_000_000_001, None),
    ],
)
def test_confirmation_receipt_checks_host_freshness_but_retains_expired_trace(
    elapsed_ns, confirmed, monkeypatch
):
    client, _, result, now = _client()
    evidence = result["confirmation"]
    if confirmed is False:
        evidence["confirmed"] = False
    query = TargetObservationQuery.model_validate(
        {
            "schema_version": "manyforge.target_observation_query.v1",
            "owner_epoch": "native.1",
            "program_binding_id": _request()["program_binding_id"],
            "target_role_id": "gripper",
            "kind": "gripper_confirmation",
        }
    )
    response = {
        "command": "query_target",
        "ok": True,
        "target_observation": {
            **query.model_dump(exclude_none=True),
            "schema_version": "manyforge.target_observation.v1",
            "source_epoch": evidence["source_epoch"],
            "source_sequence": 5,
            "value": {
                "state": "completed",
                "command_id": 7,
                "generation": 2,
                "revision": 5,
                "source_id": evidence["source_id"],
                "source_revision": evidence["source_revision"],
                "source_epoch": evidence["source_epoch"],
                "active_control_configuration_id": "route.1",
                "active_control_configuration_generation": 3,
                "grasp_confirmed": confirmed,
                "evidence": evidence,
            },
        },
    }
    client._exchange = lambda _: response
    monkeypatch.setattr(
        "manyforge_headless_api.execution.time.monotonic_ns", lambda: now + elapsed_ns
    )
    if confirmed is None or 0 <= elapsed_ns < 1_000_000_000:
        assert client.query_target(query) == response
    else:
        with pytest.raises(HeadlessApiError, match="invalid typed evidence"):
            client.query_target(query)
