"""GUI boolean signal read and Mock input simulation: models, wire, routes."""

from copy import deepcopy
from typing import Any

import pytest
from fastapi.testclient import TestClient
from pydantic import ValidationError

from manyforge_headless_api.app import create_app
from manyforge_headless_api.execution import ExecutionCommandClient
from manyforge_headless_api.models import (
    IoSignalObservation,
    IoSignalReadRequest,
    MockIoInputRequest,
    TargetObservationQuery,
)
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.tests.test_app import FakeCore, FakeManager, _service

BINDING = "sha256:" + "a" * 64
# The host's actual incarnation spelling: framed_sha256("manyforge.host-io-source", ...).
HOST_EPOCH = "sha256:" + "5c" * 32


def _read() -> dict[str, Any]:
    return {
        "schema_version": "manyforge.target_observation_query.v1",
        "owner_epoch": "owner.1",
        "program_binding_id": BINDING,
        "target_role_id": "station",
        "kind": "io_signal",
        "resource_role_id": "part_present",
    }


def _mock(value: bool = True) -> dict[str, Any]:
    return {
        "schema_version": "manyforge.mock_io_input_request.v1",
        "owner_epoch": "owner.1",
        "program_binding_id": BINDING,
        "target_role_id": "station",
        "resource_role_id": "part_present",
        "expected_source_epoch": "3",
        "value": value,
    }


def _value(value: bool | None = True, quality: str = "fresh") -> dict[str, Any]:
    fresh = quality == "fresh"
    return {
        "liveness": {"mode": "in_process", "warning": None},
        "signal_name": "part_present",
        "value_type": "bool",
        "value": value,
        "quality": quality,
        "source_id": "station_mock",
        "source_epoch": "3" if fresh else None,
        "source_sequence": "9" if fresh else None,
        "receipt_age_ns": 1200 if fresh else None,
        "continuity_loss_generation": "0",
        "error": None,
    }


def _observation(command: str, value: dict[str, Any] | None = None) -> dict[str, Any]:
    echo = {key: item for key, item in _read().items() if key != "schema_version"}
    return {
        "api_version": "manyforge.execution_command_response.v1",
        "ok": True,
        "command": command,
        "target_observation": {
            **echo,
            "schema_version": "manyforge.target_observation.v1",
            "evaluated_at_ns": 1_000,
            "value": value or _value(),
        },
    }


def _receipt(
    value: dict[str, Any] | None = None,
    *,
    model_value: bool = True,
    cursor: dict[str, Any] | None | bool = True,
    **extra: Any,
) -> dict[str, Any]:
    reply = _observation("set_mock_io_input", value)
    reply["model_value"] = model_value
    reply["published_cursor"] = (
        {
            "owner_epoch": "owner.1",
            "catalog_generation": "1",
            "source_id": "station_mock",
            "record_sequence": "10",
        }
        if cursor is True
        else cursor
    )
    reply.update(extra)
    return reply


def _unavailable() -> dict[str, Any]:
    value = _value(None, "unavailable")
    value["error"] = {"code": "io_source_unavailable", "message": "disabled"}
    return value


def _client(response: dict[str, Any]):
    client = object.__new__(ExecutionCommandClient)
    sent: list[dict[str, Any]] = []

    def exchange(request):
        sent.append(deepcopy(request))
        return deepcopy(response)

    client._exchange = exchange
    return client, sent


def test_read_request_is_exactly_the_io_signal_target_query() -> None:
    request = IoSignalReadRequest.model_validate(_read())
    assert request.target_query() == TargetObservationQuery.model_validate(_read())
    for change in (
        {"kind": "gripper_status"},
        {"program_binding_id": "binding.1"},
        {"frame_role_id": "tcp"},
        {"resource_role_id": ""},
        {"target_role_id": None},
    ):
        with pytest.raises(ValidationError):
            IoSignalReadRequest.model_validate({**_read(), **change})
    without = _read()
    del without["resource_role_id"]
    with pytest.raises(ValidationError):
        IoSignalReadRequest.model_validate(without)


@pytest.mark.parametrize(
    "change",
    [
        {"value": "true"},
        {"value": 1},
        {"value": None},
        {"expected_source_epoch": 3},
        {"expected_source_epoch": ""},
        {"expected_source_epoch": "s" * 257},
        {"expected_source_epoch": "e\u0301"},
        {"schema_version": "manyforge.mock_io_input_request.v2"},
        {"program_binding_id": "binding.1"},
        {"invert": True},
    ],
)
def test_mock_input_request_is_closed_and_strict(change) -> None:
    with pytest.raises(ValidationError):
        MockIoInputRequest.model_validate({**_mock(), **change})


def test_mock_input_request_has_exactly_seven_fields() -> None:
    assert set(MockIoInputRequest.model_validate(_mock(False)).model_dump()) == set(_mock())


def test_mock_input_wire_uses_internal_attribution_and_checks_the_echo() -> None:
    client, sent = _client(_receipt(model_value=False))
    response = client.set_mock_io_input(MockIoInputRequest.model_validate(_mock(False)))

    assert response["target_observation"]["value"]["value"] is True
    assert response["published_cursor"]["record_sequence"] == "10"
    assert sent == [
        {
            "api_version": "manyforge.execution_command.v1",
            "command": "set_mock_io_input",
            "attribution": {
                "transport": "internal",
                "assurance": "unverified",
                "principal_id": None,
            },
            "request": _mock(False),
        }
    ]


@pytest.mark.parametrize(
    "mutate",
    [
        lambda reply: reply.update(command="query_target"),
        lambda reply: reply["target_observation"].update(source_epoch="3"),
        lambda reply: reply["target_observation"].pop("evaluated_at_ns"),
        lambda reply: reply["target_observation"].update(evaluated_at_ns=-1),
        lambda reply: reply["target_observation"]["value"].update(quality="ok"),
        lambda reply: reply["target_observation"]["value"].update(value=None),
    ],
)
def test_mock_input_mismatched_reply_is_never_shown_as_fresh(mutate) -> None:
    reply = _receipt()
    mutate(reply)
    client, _sent = _client(reply)
    with pytest.raises(HeadlessApiError) as error:
        client.set_mock_io_input(MockIoInputRequest.model_validate(_mock()))
    assert error.value.code == "execution_target_observation_invalid"


def test_disabled_mock_toggle_is_an_accepted_model_without_delivery() -> None:
    client, _sent = _client(_receipt(_unavailable(), cursor=None))
    response = client.set_mock_io_input(MockIoInputRequest.model_validate(_mock()))
    assert response["model_value"] is True
    assert response["published_cursor"] is None
    assert "publication_error" not in response
    assert response["target_observation"]["value"]["quality"] == "unavailable"


def test_mock_ingress_failure_is_a_publication_error_receipt() -> None:
    error = {"code": "io_ingress_failed", "message": "ingress full", "details": {}}
    client, _sent = _client(_receipt(_unavailable(), cursor=None, publication_error=error))
    response = client.set_mock_io_input(MockIoInputRequest.model_validate(_mock()))
    assert response["publication_error"] == error


_CURSOR = {
    "owner_epoch": "owner.1",
    "catalog_generation": "1",
    "source_id": "station_mock",
    "record_sequence": "10",
}


@pytest.mark.parametrize(
    "reply",
    [
        _receipt(model_value=False),
        _receipt(model_value=1),
        {k: v for k, v in _receipt().items() if k != "published_cursor"},
        {k: v for k, v in _receipt().items() if k != "model_value"},
        _receipt(cursor={**_CURSOR, "record_sequence": "0"}),
        _receipt(cursor={**_CURSOR, "record_sequence": "010"}),
        _receipt(cursor={**_CURSOR, "catalog_generation": 1}),
        _receipt(cursor={**_CURSOR, "owner_epoch": "owner.2"}),
        _receipt(cursor={**_CURSOR, "source_id": "other"}),
        _receipt(cursor={**_CURSOR, "source_epoch": "3"}),
        _receipt(publication_error={"code": "x", "message": "y"}),
        _receipt(_unavailable(), cursor=None, publication_error=None),
        _receipt(_unavailable(), cursor=None, publication_error={"code": "", "message": "y"}),
        _receipt(delivered=True),
    ],
)
def test_mock_receipt_is_exact(reply) -> None:
    client, _sent = _client(reply)
    with pytest.raises(HeadlessApiError) as error:
        client.set_mock_io_input(MockIoInputRequest.model_validate(_mock()))
    assert error.value.code == "execution_target_observation_invalid"


def test_mock_input_refusal_keeps_the_host_error_code() -> None:
    client, _sent = _client(
        {
            "api_version": "manyforge.execution_command_response.v1",
            "ok": False,
            "command": "set_mock_io_input",
            "error": {
                "code": "io_mock_input_required",
                "message": "Signal is not an installed Mock input",
                "details": {},
            },
        }
    )
    with pytest.raises(HeadlessApiError) as error:
        client.set_mock_io_input(MockIoInputRequest.model_validate(_mock()))
    assert error.value.code == "io_mock_input_required"


class _IoExecution:
    def __init__(self) -> None:
        self.calls: list[tuple[str, object]] = []
        self.mock_reply: dict[str, Any] = _receipt()

    def query_target(self, request):
        self.calls.append(("query_target", request))
        return _observation("query_target", _value(None, "stale"))

    def set_mock_io_input(self, request):
        self.calls.append(("set_mock_io_input", request))
        return self.mock_reply


def test_routes_forward_one_typed_request_each() -> None:
    execution = _IoExecution()
    app = create_app(service=_service(FakeCore(), FakeManager(), execution))
    with TestClient(app) as client:
        read = client.post("/api/execution/io-signals/read", json=_read())
        wrong_kind = client.post(
            "/api/execution/io-signals/read", json={**_read(), "kind": "frame_pose"}
        )
        mock = client.post("/api/execution/io-signals/mock-input", json=_mock())
        malformed = client.post(
            "/api/execution/io-signals/mock-input", json={**_mock(), "value": "on"}
        )

    assert read.status_code == 200, read.text
    assert read.json()["target_observation"]["value"]["quality"] == "stale"
    assert wrong_kind.status_code == 422
    assert mock.status_code == 200, mock.text
    assert mock.json()["command"] == "set_mock_io_input"
    assert malformed.status_code == 422
    assert [kind for kind, _ in execution.calls] == ["query_target", "set_mock_io_input"]
    assert execution.calls[0][1] == TargetObservationQuery.model_validate(_read())
    assert execution.calls[1][1] == MockIoInputRequest.model_validate(_mock())


def test_mock_route_refusal_is_the_existing_error_envelope() -> None:
    execution = _IoExecution()
    execution.mock_reply = {
        "api_version": "manyforge.execution_command_response.v1",
        "ok": False,
        "command": "set_mock_io_input",
        "error": {"code": "io_signal_catalog_stale", "message": "stale", "details": {}},
    }
    app = create_app(service=_service(FakeCore(), FakeManager(), execution))
    with TestClient(app) as client:
        response = client.post("/api/execution/io-signals/mock-input", json=_mock())
    assert response.status_code == 409
    assert response.json()["error"]["code"] == "io_signal_catalog_stale"


def test_observation_liveness_is_the_closed_sx2194_projection() -> None:
    IoSignalObservation.model_validate(
        {**_value(), "liveness": {"mode": "change_only", "warning": "liveness_unproven"}}
    )
    IoSignalObservation.model_validate({**_value(), "liveness": None})  # an output
    for liveness in (
        {"mode": "change_only", "warning": None},
        {"mode": "periodic", "warning": "liveness_unproven"},
        {"mode": "sampled", "warning": None},
        {"mode": "periodic"},
    ):
        with pytest.raises(ValidationError):
            IoSignalObservation.model_validate({**_value(), "liveness": liveness})
    missing = _value()
    del missing["liveness"]
    with pytest.raises(ValidationError):
        IoSignalObservation.model_validate(missing)


def test_real_host_source_epoch_is_opaque_and_the_sequence_stays_a_counter() -> None:
    """SX-2518: the host's `source_epoch` is an opaque incarnation (spec 260
    §2.3), here its real `sha256:` spelling; only the sequence is uint64."""

    value = {**_value(False), "source_epoch": HOST_EPOCH}
    client, _ = _client(_observation("query_target", value))
    answer = client.query_target(TargetObservationQuery.model_validate(_read()))
    assert answer["target_observation"]["value"]["source_epoch"] == HOST_EPOCH
    MockIoInputRequest.model_validate({**_mock(), "expected_source_epoch": HOST_EPOCH})
    for change in (
        {"source_epoch": ""},
        {"source_epoch": "s" * 257},
        {"source_epoch": 3},
        {"source_sequence": HOST_EPOCH},
        {"source_sequence": "0"},
        {"source_sequence": "09"},
        {"source_sequence": "18446744073709551616"},
    ):
        with pytest.raises(ValidationError):
            IoSignalObservation.model_validate({**value, **change})
        client, _ = _client(_observation("query_target", {**value, **change}))
        with pytest.raises(HeadlessApiError):
            client.query_target(TargetObservationQuery.model_validate(_read()))
