"""Exact private Program-operation scope and one-shot control wire."""

from copy import deepcopy

import pytest
from pydantic import ValidationError

from manyforge_headless_api.execution import ExecutionCommandClient
from manyforge_headless_api.models import (
    ProgramOperationQuery,
    OperationRuntimeScaleRequest,
)
from manyforge_headless_api.native import HeadlessApiError


QUERY = {
    "schema_version": "manyforge.program_operation_query.v2",
    "owner_epoch": "owner.1",
    "program_binding_id": "sha256:" + "a" * 64,
    "program_run_id": "run.1",
    "target": {"component_id": "robot", "command_target_id": "arm"},
}


def _client(response):
    client = object.__new__(ExecutionCommandClient)
    requests = []

    def exchange(request):
        requests.append(deepcopy(request))
        return deepcopy(response)

    client._exchange = exchange
    client._request_attribution = lambda _principal: {"transport": "internal"}
    return client, requests


def _response():
    return {
        "ok": True,
        "command": "query_operation",
        "owner_epoch": "owner.1",
        "target": {"component_id": "robot", "command_target_id": "arm"},
        "operation": {
            "operation_id": "operation.1",
            "operation_revision": 2,
            "program_run_id": "run.1",
            "program_binding_id": QUERY["program_binding_id"],
            "participants": [{"command_target_id": "arm"}],
        },
    }


def test_operation_query_is_one_read_without_command_or_retry_identity():
    response = _response()
    client, requests = _client(response)
    assert (
        client.query_operation(ProgramOperationQuery.model_validate(QUERY)) == response
    )
    assert requests == [
        {
            "api_version": "manyforge.execution_command.v3",
            "command": "query_operation",
            "query": QUERY,
        }
    ]


@pytest.mark.parametrize(
    "change",
    [
        {"program_run_id": ""},
        {"target": {"component_id": "robot", "command_target_id": 1}},
        {"owner_epoch": ""},
        {"program_binding_id": "latest"},
        {"operation_id": "caller-selected"},
    ],
)
def test_operation_query_rejects_unbounded_or_caller_invented_scope(change):
    with pytest.raises(ValidationError):
        ProgramOperationQuery.model_validate({**QUERY, **change})


@pytest.mark.parametrize(
    "field,value",
    [
        ("operation_id", ""),
        ("operation_revision", True),
        ("operation_revision", 0),
        ("operation_revision", 2**53),
        ("participants", []),
        ("participants", [{}, {}]),
        ("participants", ["wrong"]),
    ],
)
def test_crossed_operation_response_is_rejected_not_retried(field, value):
    response = _response()
    response["operation"][field] = value
    client, requests = _client(response)
    with pytest.raises(HeadlessApiError) as error:
        client.query_operation(ProgramOperationQuery.model_validate(QUERY))
    assert error.value.code == "execution_operation_context_invalid"
    assert len(requests) == 1


def test_operation_response_must_have_its_command_kind():
    client, requests = _client({**_response(), "command": "read"})
    with pytest.raises(HeadlessApiError) as error:
        client.query_operation(ProgramOperationQuery.model_validate(QUERY))
    assert error.value.code == "execution_operation_context_invalid"
    assert len(requests) == 1


def test_operation_response_echo_is_not_compared():
    # Each exchange is its own connection: the echoed identity is not compared.
    response = _response()
    response.update(owner_epoch="foreign", target={"component_id": "robot", "command_target_id": "gripper"})
    response["operation"]["program_run_id"] = "foreign"
    client, requests = _client(response)
    assert client.query_operation(ProgramOperationQuery.model_validate(QUERY)) == response
    assert len(requests) == 1


def test_operation_scale_wire_keeps_generation_and_never_changes_persistent_selector():
    request = OperationRuntimeScaleRequest.model_validate(
        {
            "request_context": {
                "request_id": "scale.1",
                "request_owner_epoch": "owner.1",
                "source_label": "program_runner",
            },
            "operation_id": "operation.1",
            "expected_operation_revision": 2,
            "expected_runtime_scale_generation": 3,
            "scale": 0.4,
        }
    )
    response = {
        "ok": True,
        "command": "set_operation_runtime_scale",
        "accepted": {
            "request_id": "scale.1",
            "operation_id": "operation.1",
            "operation_revision": 2,
            "runtime_scale_generation": 4,
            "requested_scale": 0.4,
        },
    }
    client, requests = _client(response)
    assert (
        client.set_operation_runtime_scale(request, trusted_principal_id=None)
        == response
    )
    assert len(requests) == 1
    assert requests[0]["request"] == request.model_dump(mode="json")
    assert "selector" not in requests[0]["request"]
    # Like Stop, the Kernel resolves scaling to the current operation revision.
    response["accepted"]["operation_revision"] = 3
    client, requests = _client(response)
    assert (
        client.set_operation_runtime_scale(request, trusted_principal_id=None)
        == response
    )
    response["command"] = "query_operation"
    client, requests = _client(response)
    with pytest.raises(HeadlessApiError) as error:
        client.set_operation_runtime_scale(request, trusted_principal_id=None)
    assert error.value.code == "execution_command_response_invalid"
    assert len(requests) == 1
