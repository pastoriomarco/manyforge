"""The Program runner's I/O read and watch commands through its gateway."""

from __future__ import annotations

from typing import Any

import pytest

from manyforge_behavior.kernel_client import ExecutionCommandError, KernelClient
from manyforge_headless_api.models import IoWatchResult, TargetObservationQuery

from .fake_gateway import RecordingGateway

_A = "sha256:" + "a" * 64
_API = {"api_version": "manyforge.execution_command_response.v6"}


def _query() -> dict[str, Any]:
    return {
        "schema_version": "manyforge.target_observation_query.v3",
        "program_binding_id": _A,
        "kind": "io_signal",
        "signal": {"component_id": "cell_io", "signal_name": "part_present"},
    }


def _value(**overrides: Any) -> dict[str, Any]:
    return {
        "liveness": {"mode": "in_process", "warning": None},
        "signal_name": "part_present",
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
        "schema_version": "manyforge.io_watch_request.v3",
        "program_binding_id": _A,
        "claim_generation": 5,
        "program_run_id": 1,
        "node_instance_id": "wait",
        "node_attempt_id": 1,
        "signal": {"component_id": "cell_io", "signal_name": "part_present"},
        "condition": {"kind": "level", "expected_value": True},
    }


def _registration() -> dict[str, Any]:
    return {
        "catalog_generation": "1",
        "registration_id": "7",
        "scope": {
            "program_run_id": 1,
            "node": {"node_instance_id": "wait", "node_attempt_id": 1},
        },
    }


def _client(replies: dict[str, Any]) -> tuple[KernelClient, RecordingGateway]:
    gateway = RecordingGateway({
        **{command: {**_API, "ok": True} for command in
           ("identity", "submit", "read", "reset", "acknowledge_completed", "stop")},
        **replies,
    })
    return KernelClient(gateway), gateway


def _result(state: str = "pending") -> dict[str, Any]:
    return {"state": state, "observation": _value(quality="stale"), "matched_sample": None}


def test_query_requires_a_signal_only_for_io_signal() -> None:
    TargetObservationQuery.model_validate(_query())
    with pytest.raises(ValueError):
        TargetObservationQuery.model_validate({**_query(), "signal": None})
    with pytest.raises(ValueError):
        TargetObservationQuery.model_validate({**_query(), "kind": "gripper_status"})


def test_watch_commands_send_the_request_and_the_exact_token() -> None:
    client, gateway = _client({
        "register_io_watch": {
            **_API, "ok": True, "command": "register_io_watch",
            "registration": _registration(), "result": _result(),
        },
        "read_io_watch": {**_API, "ok": True, "command": "read_io_watch", "result": _result()},
        "release_io_watch": {**_API, "ok": True, "command": "release_io_watch"},
    })
    registered = client.register_io_watch(_request())
    assert registered == {"registration": _registration(), "result": _result()}
    assert client.read_io_watch(registered["registration"])["state"] == "pending"
    client.release_io_watch(registered["registration"])
    # The core adds the envelope, the internal attribution and the socket.
    assert gateway.sent == [
        ("register_io_watch", {"request": _request()}),
        ("read_io_watch", {"registration": _registration()}),
        ("release_io_watch", {"registration": _registration()}),
    ]


def test_registration_echo_is_not_compared() -> None:
    """Each exchange is its own connection: the echoed scope is not compared."""

    registration = _registration()
    registration["scope"]["node"]["node_attempt_id"] = 2
    client, _gateway = _client({"register_io_watch": {
        **_API, "ok": True, "command": "register_io_watch",
        "registration": registration, "result": _result(),
    }})
    assert client.register_io_watch(_request())["registration"] == registration


def test_actual_released_reply_carries_neither_observation_nor_sample() -> None:
    released = {"state": "released", "observation": None, "matched_sample": None}
    client, _gateway = _client({"read_io_watch": {
        **_API, "ok": True, "command": "read_io_watch", "result": released}})
    assert client.read_io_watch(_registration()) == released


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
    return {**identity, "signal_name": "part_present", "source_id": "mock.io", "source_epoch": "1",
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
        ("register_io_watch", lambda client: client.register_io_watch(_request())),
        ("read_io_watch", lambda client: client.read_io_watch(_registration())),
        ("release_io_watch", lambda client: client.release_io_watch(_registration())),
    ],
)
def test_host_error_envelopes_keep_their_code_and_reason(command, call) -> None:
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
    client, _gateway = _client({command: refusal})
    with pytest.raises(ExecutionCommandError) as refused:
        call(client)
    assert refused.value.code == "stale_generation"
    assert refused.value.details == {"reason": "io_watch_generation_stale"}
    assert refused.value.producer == "kernel"


def _scope_request() -> dict[str, Any]:
    return {
        "schema_version": "manyforge.io_watch_scope_release.v2",
        "program_binding_id": _A,
        "claim_generation": 5,
        "program_run_id": 1,
        "node_instance_id": "wait",
        "node_attempt_id": 1,
    }


def test_scope_release_sends_the_six_field_scope_request() -> None:
    client, gateway = _client(
        {"release_io_watch": {**_API, "ok": True, "command": "release_io_watch"}}
    )
    assert client.release_io_watch_scope(_scope_request()) is None
    assert gateway.sent == [("release_io_watch", {"scope_request": _scope_request()})]


def test_matched_sample_source_epoch_is_opaque_and_its_sequence_a_counter() -> None:
    """The host mints `sha256:` incarnations."""

    epoch = "sha256:" + "5c" * 32
    sample = {**_matched_sample(controllable_component_id="io.1"), "source_epoch": epoch}
    IoWatchResult.model_validate(
        {"state": "matched", "observation": _value(), "matched_sample": sample})
    for change in ({"source_epoch": ""}, {"source_epoch": "s" * 257},
                   {"source_sequence": epoch}, {"source_sequence": "0"}):
        with pytest.raises(ValueError):
            IoWatchResult.model_validate({"state": "matched", "observation": _value(),
                                          "matched_sample": {**sample, **change}})
