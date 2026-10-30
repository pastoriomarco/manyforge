"""I/O read and watch commands on the ordinary execution-command socket."""

from __future__ import annotations

from copy import deepcopy
from typing import Any

import pytest

from manyforge_headless_api.execution import ExecutionCommandClient
from manyforge_headless_api.models import IoWatchResult, TargetObservationQuery
from manyforge_headless_api.native import HeadlessApiError
from manyforge_program_runner.execution_transport import (
    ProgramRunnerExecutionTransport,
)

_A = "sha256:" + "a" * 64


def _query() -> dict[str, Any]:
    return {
        "schema_version": "manyforge.target_observation_query.v1",
        "owner_epoch": "kernel.epoch.7",
        "program_binding_id": _A,
        "target_role_id": "cell_io",
        "kind": "io_signal",
        "resource_role_id": "part_present",
    }


def _value(**overrides: Any) -> dict[str, Any]:
    return {
        "liveness": {"mode": "in_process", "warning": None},
        "signal_name": "di_0",
        "value_type": "bool",
        "value": False,
        "quality": "fresh",
        "source_id": "mock.io",
        "source_epoch": "1",
        "source_sequence": "18446744073709551615",
        "receipt_age_ns": "9223372036854775807",
        "continuity_loss_generation": "0",
        "error": None,
        **overrides,
    }


def _request() -> dict[str, Any]:
    return {
        "schema_version": "manyforge.io_watch_request.v1",
        "owner_epoch": "kernel.epoch.7",
        "program_binding_id": _A,
        "program_runner_owner_epoch": "runner.epoch.3",
        "claim_generation": 5,
        "program_run_id": "run.1",
        "node_instance_id": "wait",
        "node_attempt_id": "attempt.1",
        "target_role_id": "cell_io",
        "resource_role_id": "part_present",
        "condition": {"kind": "level", "expected_value": True},
    }


def _registration() -> dict[str, Any]:
    return {
        "owner_epoch": "kernel.epoch.7",
        "catalog_generation": "1",
        "registration_id": "7",
        "scope": {
            "program_runner_owner_epoch": "runner.epoch.3",
            "program_run_id": "run.1",
            "node": {"node_instance_id": "wait", "node_attempt_id": "attempt.1"},
        },
    }


def _result(state: str = "pending") -> dict[str, Any]:
    return {"state": state, "observation": _value(quality="stale"), "matched_sample": None}


def test_query_model_names_a_resource_role_only_for_io_signal() -> None:
    TargetObservationQuery.model_validate(_query())
    with pytest.raises(ValueError):
        TargetObservationQuery.model_validate({**_query(), "resource_role_id": None})
    with pytest.raises(ValueError):
        TargetObservationQuery.model_validate({**_query(), "kind": "gripper_status"})


@pytest.mark.parametrize(
    "bad",
    [
        {"extra_outer": True},
        {"value": _value(quality="fresh", value=None)},
        {"value": _value(source_sequence="07")},
        {"value": _value(quality="unknown")},
        {"value": _value(value_type="int")},
        {"evaluated_at_ns": None},
        {"evaluated_at_ns": "07"},
        {"evaluated_at_ns": -1},
        {"evaluated_at_ns": "9223372036854775808"},
        {"drop_evaluated_at_ns": True},
    ],
)
def test_io_signal_answers_are_exact(monkeypatch, tmp_path, bad) -> None:
    transport = ProgramRunnerExecutionTransport(tmp_path, timeout_seconds=1.0)
    observation = {
        **_query(),
        "schema_version": "manyforge.target_observation.v1",
        # The actual host projection names its outer evaluation time.
        "evaluated_at_ns": "9223372036854775807",
        "value": _value(),
    }
    response = {"ok": True, "command": "query_target", "target_observation": observation}
    monkeypatch.setattr(ExecutionCommandClient, "_exchange", lambda *_: deepcopy(response))
    assert transport.query_target(_query())["target_observation"]["value"]["value"] is False
    stale = deepcopy(response)
    stale["target_observation"]["value"] = _value(
        quality="stale", value=True, source_epoch=None, source_sequence=None,
        receipt_age_ns=None,
    )
    monkeypatch.setattr(ExecutionCommandClient, "_exchange", lambda *_: deepcopy(stale))
    assert transport.query_target(_query())["target_observation"]["value"]["quality"] == "stale"
    broken = deepcopy(response)
    if "extra_outer" in bad:
        broken["target_observation"]["source_sequence"] = 1
    elif "drop_evaluated_at_ns" in bad:
        del broken["target_observation"]["evaluated_at_ns"]
    else:
        broken["target_observation"].update(bad)
    monkeypatch.setattr(ExecutionCommandClient, "_exchange", lambda *_: deepcopy(broken))
    with pytest.raises(HeadlessApiError) as invalid:
        transport.query_target(_query())
    assert invalid.value.code == "execution_target_observation_invalid"


def test_watch_commands_use_the_ordinary_socket_and_exact_token(monkeypatch, tmp_path) -> None:
    transport = ProgramRunnerExecutionTransport(tmp_path, timeout_seconds=1.0)
    sent: list[tuple[dict[str, Any], str]] = []
    replies = {
        "register_io_watch": {
            "ok": True, "command": "register_io_watch",
            "registration": _registration(), "result": _result(),
        },
        "read_io_watch": {"ok": True, "command": "read_io_watch", "result": _result()},
        "release_io_watch": {"ok": True, "command": "release_io_watch"},
    }

    def exchange_on(_self, command, socket_path):
        sent.append((deepcopy(command), socket_path.name))
        return deepcopy(replies[command["command"]])

    monkeypatch.setattr(ExecutionCommandClient, "_exchange_on", exchange_on)
    registered = transport.register_io_watch(_request())
    assert registered["registration"] == _registration()
    assert transport.read_io_watch(registered["registration"])["state"] == "pending"
    transport.release_io_watch(registered["registration"])
    assert [command["command"] for command, _ in sent] == [
        "register_io_watch", "read_io_watch", "release_io_watch"
    ]
    assert {socket for _, socket in sent} == {transport._socket_path.name}
    assert sent[0][0]["request"] == _request()
    assert sent[1][0]["registration"] == sent[2][0]["registration"] == _registration()
    assert all(command["attribution"]["transport"] == "internal" for command, _ in sent)


def test_registration_reply_must_carry_a_valid_result(monkeypatch, tmp_path) -> None:
    transport = ProgramRunnerExecutionTransport(tmp_path, timeout_seconds=1.0)
    result = {"state": "matched", "observation": _value(), "matched_sample": None}
    monkeypatch.setattr(
        ExecutionCommandClient,
        "_exchange_on",
        lambda *_: {
            "ok": True, "command": "register_io_watch",
            "registration": _registration(), "result": result,
        },
    )
    with pytest.raises(HeadlessApiError) as invalid:
        transport.register_io_watch(_request())
    assert invalid.value.code == "execution_command_response_invalid"


def test_registration_echo_is_not_compared(monkeypatch, tmp_path) -> None:
    """Each exchange is its own connection: the echoed scope is not compared."""

    transport = ProgramRunnerExecutionTransport(tmp_path, timeout_seconds=1.0)
    registration = _registration()
    registration["scope"]["node"]["node_attempt_id"] = "attempt.2"
    monkeypatch.setattr(
        ExecutionCommandClient,
        "_exchange_on",
        lambda *_: {
            "ok": True, "command": "register_io_watch",
            "registration": registration, "result": _result(),
        },
    )
    assert transport.register_io_watch(_request())["registration"] == registration


def test_edge_conditions_never_reach_the_socket(monkeypatch, tmp_path) -> None:
    transport = ProgramRunnerExecutionTransport(tmp_path, timeout_seconds=1.0)
    monkeypatch.setattr(
        ExecutionCommandClient, "_exchange_on",
        lambda *_: pytest.fail("an edge watch must not be sent"),
    )
    request = _request()
    request["condition"] = {"kind": "rising", "expected_value": True}
    with pytest.raises(HeadlessApiError) as refused:
        transport.register_io_watch(request)
    assert refused.value.code == "execution_command_invalid"


_API = {"api_version": "manyforge.execution_command_response.v1"}


def test_actual_released_reply_carries_neither_observation_nor_sample(
    monkeypatch, tmp_path
) -> None:
    transport = ProgramRunnerExecutionTransport(tmp_path, timeout_seconds=1.0)
    released = {"state": "released", "observation": None, "matched_sample": None}
    monkeypatch.setattr(
        ExecutionCommandClient, "_exchange_on",
        lambda *_: {**_API, "ok": True, "command": "read_io_watch", "result": released},
    )
    assert transport.read_io_watch(_registration()) == released


@pytest.mark.parametrize(
    "result",
    [
        {"state": "released", "observation": _value(), "matched_sample": None},
        {"state": "pending", "observation": None, "matched_sample": None},
        {"state": "matched", "observation": None, "matched_sample": None},
    ],
)
def test_only_released_omits_the_observation(result) -> None:
    with pytest.raises(ValueError):
        IoWatchResult.model_validate(result)


def _matched_sample(**identity) -> dict:
    return {**identity, "signal_name": "di_0", "source_id": "mock.io", "source_epoch": "1",
            "source_sequence": "10", "value": True, "received_at_ns": 55}


def test_matched_sample_names_the_controllable_component() -> None:
    result = {"state": "matched", "observation": _value(),
              "matched_sample": _matched_sample(controllable_component_id="io.1")}
    IoWatchResult.model_validate(result)
    # Hard switch: the superseded `component_id` key is refused.
    result["matched_sample"] = _matched_sample(component_id="io.1")
    with pytest.raises(ValueError):
        IoWatchResult.model_validate(result)


@pytest.mark.parametrize(
    ("command", "call"),
    [
        ("register_io_watch", lambda transport: transport.register_io_watch(_request())),
        ("read_io_watch", lambda transport: transport.read_io_watch(_registration())),
        ("release_io_watch", lambda transport: transport.release_io_watch(_registration())),
    ],
)
def test_host_error_envelopes_keep_their_code_and_reason(
    monkeypatch, tmp_path, command, call
) -> None:
    transport = ProgramRunnerExecutionTransport(tmp_path, timeout_seconds=1.0)
    # The actual host ErrorResponse: no `command`, the Core ErrorDTO verbatim.
    refusal = {
        **_API,
        "ok": False,
        "error": {
            "code": "stale_generation",
            "message": "I/O signal identity is no longer current",
            "details": {"reason": "io_watch_generation_stale"},
        },
    }
    monkeypatch.setattr(ExecutionCommandClient, "_exchange_on", lambda *_: deepcopy(refusal))
    with pytest.raises(HeadlessApiError) as refused:
        call(transport)
    assert refused.value.code == "stale_generation"
    assert refused.value.details == {"reason": "io_watch_generation_stale"}


def _scope_request() -> dict[str, Any]:
    return {
        "schema_version": "manyforge.io_watch_scope_release.v1",
        "owner_epoch": "kernel.epoch.7",
        "program_binding_id": _A,
        "program_runner_owner_epoch": "runner.epoch.3",
        "claim_generation": 5,
        "program_run_id": "run.1",
        "node_instance_id": "wait",
        "node_attempt_id": "attempt.1",
    }


def test_scope_release_sends_the_closed_eight_field_scope_request(
    monkeypatch, tmp_path
) -> None:
    transport = ProgramRunnerExecutionTransport(tmp_path, timeout_seconds=1.0)
    sent: list[dict[str, Any]] = []

    def exchange_on(_self, command, socket_path):
        sent.append(deepcopy(command))
        assert socket_path.name == transport._socket_path.name
        return {**_API, "ok": True, "command": "release_io_watch"}

    monkeypatch.setattr(ExecutionCommandClient, "_exchange_on", exchange_on)
    assert transport.release_io_watch_scope(_scope_request()) is None
    assert sent == [{
        "api_version": "manyforge.execution_command.v1",
        "command": "release_io_watch",
        "attribution": sent[0]["attribution"],
        "scope_request": _scope_request(),
    }]
    assert sent[0]["attribution"]["transport"] == "internal"
    assert "registration" not in sent[0]


@pytest.mark.parametrize(
    "bad",
    [
        {"extra": 1},
        {"schema_version": "manyforge.io_watch_request.v1"},
        {"claim_generation": 0},
        {"claim_generation": 2**53},
        {"program_binding_id": "sha256:short"},
        {"drop": "node_attempt_id"},
    ],
)
def test_scope_release_request_is_closed(monkeypatch, tmp_path, bad) -> None:
    transport = ProgramRunnerExecutionTransport(tmp_path, timeout_seconds=1.0)
    monkeypatch.setattr(
        ExecutionCommandClient, "_exchange_on",
        lambda *_: pytest.fail("an invalid scope release must not be sent"),
    )
    request = _scope_request()
    if "drop" in bad:
        del request[bad["drop"]]
    else:
        request.update(bad)
    with pytest.raises(HeadlessApiError) as refused:
        transport.release_io_watch_scope(request)
    assert refused.value.code == "execution_command_invalid"


def test_scope_release_reply_must_name_its_command(monkeypatch, tmp_path) -> None:
    transport = ProgramRunnerExecutionTransport(tmp_path, timeout_seconds=1.0)
    monkeypatch.setattr(
        ExecutionCommandClient, "_exchange_on",
        lambda *_: {**_API, "ok": True, "command": "read_io_watch"},
    )
    with pytest.raises(HeadlessApiError) as invalid:
        transport.release_io_watch_scope(_scope_request())
    assert invalid.value.code == "execution_command_response_invalid"


def test_matched_sample_source_epoch_is_opaque_and_its_sequence_a_counter() -> None:
    """SX-2518: the host mints `sha256:` incarnations (spec 260 §2.3)."""

    epoch = "sha256:" + "5c" * 32
    sample = {**_matched_sample(controllable_component_id="io.1"), "source_epoch": epoch}
    IoWatchResult.model_validate(
        {"state": "matched", "observation": _value(), "matched_sample": sample})
    for change in ({"source_epoch": ""}, {"source_epoch": "s" * 257},
                   {"source_sequence": epoch}, {"source_sequence": "0"}):
        with pytest.raises(ValueError):
            IoWatchResult.model_validate({"state": "matched", "observation": _value(),
                                          "matched_sample": {**sample, **change}})
