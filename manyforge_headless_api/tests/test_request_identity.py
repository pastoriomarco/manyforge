"""Application-start identity ``A`` and request keys at the Headless edge.

Contracts 1.4, 1.6, 1.8 and 1.10 of bulk change 1: only Headless checks ``A``;
the request key is formed here once; a Stop is never refused and never goes
through the Program runner.
"""

from __future__ import annotations

import asyncio
import threading
from typing import Any

import pytest

from manyforge_headless_api.control_dispatch import HeadlessExecutionControlDispatch
from manyforge_headless_api.control_protocol import (
    CONTROL_PROTOCOL_VERSION,
    EXECUTION_COMMAND_SCHEMA,
    ControlFreshnessAuthority,
    ControlServerHelloDTO,
    ExecutionCommandRequestDTO,
    ExecutionIngressStamp,
)
from manyforge_headless_api.execution_control import (
    ExecutionControlBusyError,
    ExecutionControlClient,
    ExecutionControlLane,
    ExecutionControlLaneSpec,
)
from manyforge_headless_api.identity import (
    HeadlessKeyMinter,
    RequestAdmission,
    RequestKeyInvalid,
    RequestKeyRequired,
    StartIdentityMismatch,
    display_request_key,
    format_request_key,
    http_request_key,
    identified,
    parse_application_start_id,
    parse_id_filter,
    websocket_request_key,
    with_part,
)
from manyforge_headless_api.native import HeadlessApiError


A = "3f9a00000000beef"
OTHER_A = "3f9a00000000feed"
CLIENT_KEY = "0123456789ABCDEF0123456789abcdef"
_GROUPS = {"group_ids": ["all"]}


def test_application_start_id_has_exactly_one_text_form() -> None:
    assert parse_application_start_id(A) == 0x3F9A00000000BEEF
    for invalid in ("", "0" * 16, A.upper(), A[:-1], A + "0", "3f9a-0000000beef"):
        with pytest.raises(ValueError):
            parse_application_start_id(invalid)


def test_request_key_layout_and_classes() -> None:
    websocket = websocket_request_key(4, 118)
    assert websocket[0] == 0x00
    assert int.from_bytes(websocket[1:8], "big") == 4
    assert int.from_bytes(websocket[8:16], "big") == 118
    assert display_request_key(A, websocket) == "3f9a/s4.118"

    http = http_request_key(CLIENT_KEY)
    assert http[0] == 0xC0 and http[1:] == bytes.fromhex(CLIENT_KEY)[1:]
    # Upper or lower case is accepted; everything emitted is lower case.
    assert format_request_key(http) == "c0" + CLIENT_KEY[2:].lower()
    assert with_part(http, 2)[0] == 0xC2 and with_part(http, 2)[1:] == http[1:]
    for invalid in ("", CLIENT_KEY[:-1], CLIENT_KEY + "0", "z" * 32):
        with pytest.raises(RequestKeyInvalid):
            http_request_key(invalid)

    minter = HeadlessKeyMinter()
    first, second = minter.mint(), minter.mint()
    assert first[0] == 0x40 and int.from_bytes(first[1:8], "big") == 5
    assert int.from_bytes(first[8:], "big") + 1 == int.from_bytes(second[8:], "big")


def test_http_retry_forms_the_same_key_and_a_stop_without_a_key_acts() -> None:
    admission = RequestAdmission(A)
    # A resend with the same client key reaches the Kernel unchanged, so its
    # replay window answers it exactly.
    assert admission.http_key(CLIENT_KEY, stop=False) == admission.http_key(
        CLIENT_KEY.lower(), stop=False
    )
    with pytest.raises(RequestKeyRequired):
        admission.http_key(None, stop=False)
    with pytest.raises(RequestKeyInvalid):
        admission.http_key("short", stop=False)
    stop_without = admission.http_key(None, stop=True)
    stop_invalid = admission.http_key("short", stop=True)
    assert stop_without != stop_invalid
    assert bytes.fromhex(stop_without)[0] == 0x40


def test_start_identity_check() -> None:
    admission = RequestAdmission(A)
    admission.start_identity(A)
    with pytest.raises(StartIdentityMismatch) as missing:
        admission.start_identity(None)
    assert missing.value.code == "start_identity_required"
    with pytest.raises(StartIdentityMismatch) as wrong:
        admission.start_identity(OTHER_A)
    assert wrong.value.code == "stale_application_start_id"


def test_server_hello_carries_application_start_id() -> None:
    hello = ControlServerHelloDTO(
        schema_version=CONTROL_PROTOCOL_VERSION,
        message_kind="server_hello",
        application_start_id=A,
        control_session_number=1,
        heartbeat_period_ns=250_000_000,
        heartbeat_delayed_ns=500_000_000,
        heartbeat_stale_ns=1_000_000_000,
        maximum_message_bytes=65_536,
        maximum_messages_per_second=64,
    )
    value = hello.model_dump(mode="json")
    assert value["application_start_id"] == A
    assert "receiver_incarnation" not in value


def _command(
    kind: str,
    payload: dict[str, Any],
    *,
    session: int | None = None,
    sequence: int | None = None,
) -> ExecutionCommandRequestDTO:
    authority = ControlFreshnessAuthority(
        clock_ns=lambda: 1_000_000_000, secret=b"x" * 32
    )
    return ExecutionCommandRequestDTO.model_validate(
        {
            "command_envelope": {
                "schema_version": EXECUTION_COMMAND_SCHEMA,
                "command_kind": kind,
                "source_label": "test",
                "control_session_number": session,
                "client_sequence": None if sequence is None else str(sequence),
                "freshness": authority.issue(
                    execution_control_revision=7
                ).model_dump(mode="json"),
                "expected_execution_control_revision": 7,
            },
            "payload": payload,
        }
    )


_STAMP = ExecutionIngressStamp(
    transport_kind="rest",
    received_server_sequence=1,
    received_monotonic_ns=1,
)


class _HostService:
    """Records each host call; the Program runner is silent (never called)."""

    def __init__(self) -> None:
        self.calls: list[tuple[str, Any]] = []
        self.cycle_reads = 0

    def program_cycle_status(self, **_kwargs: Any) -> dict[str, Any]:
        self.cycle_reads += 1
        return {
            "execution_control_revision": 7,
            "cycle_generation": 1,
            "program_run_id": None,
            "state": "idle_after_reset",
            "allowed_commands": [
                {"command_kind": "program_run", "allowed": True, "disabled_reason": None},
                {"command_kind": "runtime_scale", "allowed": True, "disabled_reason": None},
            ],
        }

    def _record(name: str):  # type: ignore[no-redef]
        def call(self: "_HostService", request: Any, *args: Any) -> dict[str, Any]:
            self.calls.append((name, request))
            return {"ok": True, "command": name, "accepted": {}}

        return call

    program_run_motion_stop = _record("stop_program_selection")
    execution_stop = _record("stop")
    execution_stop_all = _record("stop_all")
    execution_halt = _record("halt")
    program_run_start = _record("program_run")

    def execution_recognized_stop(self, **call: Any) -> dict[str, Any]:
        self.calls.append(("recognized_stop", call))
        return {"ok": True, "command": "recognized_stop", "accepted": {}}


def _dispatch() -> tuple[HeadlessExecutionControlDispatch, _HostService]:
    service = _HostService()
    return HeadlessExecutionControlDispatch(service, RequestAdmission(A)), service  # type: ignore[arg-type]


def test_each_stop_kind_goes_to_its_host_command_with_the_runner_silent() -> None:
    dispatch, service = _dispatch()
    dispatch(
        _command(
            "program_motion_stop",
            {"program_run_id": 3, "application_start_id": A, **_GROUPS},
        ),
        _STAMP,
    )
    dispatch(
        _command(
            "operation_stop",
            {
                "application_start_id": A,
                "request_context": {"source_label": "test"},
                "operations": [
                    {
                        "operation_id": 7,
                        "expected_operation_revision": 1,
                    }
                ],
            },
        ),
        _STAMP,
    )
    for kind in ("stop_all", "halt"):
        dispatch(
            _command(kind, {"request_context": {"source_label": "test"}}),
            _STAMP,
        )
    assert [name for name, _ in service.calls] == [
        "stop_program_selection",
        "stop",
        "stop_all",
        "halt",
    ]
    assert service.cycle_reads == 0
    selection = service.calls[0][1]
    assert selection.program_run_id == 3 and selection.group_ids == ["all"]
    # A Stop without a key acts, with a fresh Headless key.
    assert bytes.fromhex(selection.request_key)[0] == 0x40


def test_a_websocket_stop_is_keyed_by_its_session_and_sequence() -> None:
    dispatch, service = _dispatch()
    dispatch(
        _command("program_motion_stop", {"program_run_id": 3, **_GROUPS}, session=4, sequence=118),
        _STAMP,
    )
    (_name, selection), = service.calls
    assert selection.request_key == format_request_key(websocket_request_key(4, 118))


def _keyed_stop_payload(kind: str) -> dict[str, Any]:
    """A valid session-less Stop of ``kind`` carrying the client's key."""

    if kind == "program_motion_stop":
        return {"request_key": CLIENT_KEY, "program_run_id": 3, **_GROUPS}
    return {
        "request_context": {"request_key": CLIENT_KEY, "source_label": "test"},
        "operations": [{"operation_id": 7, "expected_operation_revision": 1}],
    }


@pytest.mark.parametrize("kind", ["program_motion_stop", "operation_stop"])
@pytest.mark.parametrize("start_identity", [None, OTHER_A])
def test_a_stop_naming_another_or_no_start_acts_as_stop_all_recorded_stale_start(
    kind: str, start_identity: str | None,
) -> None:
    """Never refused: it goes as a recognized Stop on the operation-Stop route
    with the caller's formed key, so the Kernel widens it to Stop all and
    records `stale_start` (OD-244)."""

    dispatch, service = _dispatch()
    payload = _keyed_stop_payload(kind)
    if start_identity is not None:
        payload["application_start_id"] = start_identity
    receipt, reply = dispatch(_command(kind, payload), _STAMP)
    formed = format_request_key(http_request_key(CLIENT_KEY))
    assert receipt.disposition == "received"
    assert receipt.code == f"{kind}_received"
    assert receipt.request_key == formed
    assert reply == {"ok": True, "command": "recognized_stop", "accepted": {}}
    assert service.calls == [
        (
            "recognized_stop",
            {
                "request_key": formed,
                "recognized_route": "/api/execution/stops",
                "reason": "recognized_stop_start_not_current",
                # The caller's label (the command envelope's).
                "source_label": "test",
                "attribution": service.calls[0][1]["attribution"],
            },
        )
    ]
    assert service.calls[0][1]["attribution"].principal_id is None
    assert service.cycle_reads == 0


@pytest.mark.parametrize(
    ("kind", "host_call"),
    [("program_motion_stop", "stop_program_selection"), ("operation_stop", "stop")],
)
def test_a_stop_naming_the_current_start_takes_its_ordinary_path(
    kind: str, host_call: str
) -> None:
    dispatch, service = _dispatch()
    payload = {**_keyed_stop_payload(kind), "application_start_id": A}
    receipt, _ = dispatch(_command(kind, payload), _STAMP)
    assert receipt.disposition == "received"
    ((name, request),) = service.calls
    assert name == host_call
    key = (
        request.request_key
        if kind == "program_motion_stop"
        else request.request_context.request_key
    )
    assert key == format_request_key(http_request_key(CLIENT_KEY))


@pytest.mark.parametrize(
    ("start_identity", "code"),
    [(None, "start_identity_required"), (OTHER_A, "stale_application_start_id")],
)
def test_an_ordinary_command_naming_another_or_no_start_is_refused(
    start_identity: str | None, code: str
) -> None:
    dispatch, service = _dispatch()
    payload: dict[str, Any] = {
        "request_key": CLIENT_KEY,
        "request_context": {"source_label": "test"},
        "selector": {"program_binding_id": "binding"},
    }
    if start_identity is not None:
        payload["application_start_id"] = start_identity
    with pytest.raises(HeadlessApiError) as refused:
        dispatch(_command("runtime_scale", payload), _STAMP)
    assert refused.value.code == code
    assert service.calls == [] and service.cycle_reads == 0


@pytest.mark.parametrize(
    ("key", "code"),
    [(None, "request_key_required"), ("not-a-key", "request_key_invalid")],
)
def test_an_execute_without_a_valid_key_is_refused(key: str | None, code: str) -> None:
    dispatch, service = _dispatch()
    payload: dict[str, Any] = {
        "scenario_id": "lite6",
        "source_document_digest": "sha256:" + "0" * 64,
        "expected_applied_record_digest": "sha256:" + "1" * 64,
        "draft_program_revision": "sha256:" + "2" * 64,
    }
    if key is not None:
        payload["request_key"] = key
    with pytest.raises(HeadlessApiError) as refused:
        dispatch(_command("program_run", payload), _STAMP)
    assert refused.value.code == code
    assert service.calls == []


def test_an_invalid_stop_body_is_a_malformed_stop_with_a_headless_key() -> None:
    dispatch, service = _dispatch()
    dispatch(
        _command(
            "program_motion_stop",
            {"program_run_id": "not-a-number", "application_start_id": A},
        ),
        _STAMP,
    )
    ((name, call),) = service.calls
    assert name == "recognized_stop"
    assert call["recognized_route"] == "/api/execution/stops"
    assert call["source_label"] == "test"
    assert bytes.fromhex(call["request_key"])[0] == 0x40


def test_the_stop_lane_never_refuses_and_keeps_order() -> None:
    """A burst of Stops above the worker count is all accepted, in order."""

    async def scenario() -> list[int]:
        lanes = ExecutionControlClient(
            stop=ExecutionControlLaneSpec(1, None),
            ordinary=ExecutionControlLaneSpec(1, 1),
        )
        release = threading.Event()
        done: list[int] = []

        def stop(number: int) -> int:
            release.wait(5.0)
            done.append(number)
            return number

        try:
            futures = [
                lanes.start(ExecutionControlLane.STOP, lambda n=n: stop(n))
                for n in range(16)
            ]
            blocked = lanes.start(ExecutionControlLane.ORDINARY, release.wait)
            with pytest.raises(ExecutionControlBusyError):
                lanes.start(ExecutionControlLane.ORDINARY, lambda: None)
            assert lanes.status()["stop"]["rejected"] == 0
            release.set()
            await asyncio.gather(*futures, blocked)
            return done
        finally:
            lanes.close()

    assert asyncio.run(scenario()) == list(range(16))


def test_each_scene_preparation_attempt_gets_a_new_key() -> None:
    admission = RequestAdmission(A)
    keys = {admission.mint_key() for _ in range(8)}
    assert len(keys) == 8


def test_every_forwarded_receipt_names_its_formed_key() -> None:
    dispatch, service = _dispatch()
    http, _ = dispatch(
        _command(
            "program_motion_stop",
            {"program_run_id": 3, "application_start_id": A, "request_key": CLIENT_KEY, **_GROUPS},
        ),
        _STAMP,
    )
    websocket, _ = dispatch(
        _command("program_motion_stop", {"program_run_id": 3, **_GROUPS}, session=4, sequence=118),
        _STAMP,
    )
    assert http.request_key == service.calls[0][1].request_key
    assert http.request_key == format_request_key(http_request_key(CLIENT_KEY))
    # OD-214: a WebSocket command's receipt names its class 0 key too, so
    # the client finds the command's row by it.
    assert websocket.request_key == service.calls[1][1].request_key
    assert websocket.request_key == format_request_key(websocket_request_key(4, 118))


def test_a_reply_naming_an_id_carries_a_and_its_display() -> None:
    shown = identified(
        {"ok": True, "event_id": 12, "operation_id": 7, "program_run_id": 3}, A
    )
    assert shown["application_start_id"] == A
    assert shown["display"] == "3f9a/stop12"
    assert identified({"operation_id": 7, "program_run_id": 3}, A)["display"] == "3f9a/op7"
    assert identified({"program_run_id": 3}, A)["display"] == "3f9a/run3"
    key = format_request_key(websocket_request_key(4, 118))
    assert identified({"request_key": key}, A)["display"] == "3f9a/s4.118"
    http_key = format_request_key(http_request_key(CLIENT_KEY))
    assert identified({"request_key": http_key}, A)["display"] == "3f9a/k" + http_key[2:10]
    # The id of the accepted or read record when the top level names none.
    assert identified({"ok": True, "accepted": {"event_id": 5}}, A)["display"] == "3f9a/stop5"
    assert identified({"ok": True, "operation": {"operation_id": 9}}, A)["display"] == "3f9a/op9"
    # A top-level id outranks a nested one.
    assert identified(
        {"program_run_id": 3, "accepted": {"event_id": 5}}, A
    )["display"] == "3f9a/run3"
    plain = {"ok": True, "state": "idle"}
    assert identified(plain, A) is plain


@pytest.mark.parametrize(
    ("name", "text", "parsed"),
    [
        ("operation_id", "7", (7, None, None)),
        ("operation_id", "3f9a/op7", (7, None, "3f9a")),
        ("operation_id", f"{A}/op7", (7, A, None)),
        ("stop_event_id", "3f9a/stop12", (12, None, "3f9a")),
        ("program_run_id", f"{A}/run3", (3, A, None)),
        ("node_attempt_id", "4", (4, None, None)),
    ],
)
def test_a_history_id_filter_accepts_the_number_display_and_copy_forms(
    name: str, text: str, parsed: tuple[int, str | None, str | None]
) -> None:
    assert parse_id_filter(name, text) == parsed


@pytest.mark.parametrize(
    ("name", "text"),
    [
        ("operation_id", "3f9a/stop7"),
        ("stop_event_id", "3f9a/op7"),
        ("operation_id", "07"),
        ("operation_id", "0"),
        ("operation_id", "op.7"),
        ("operation_id", "3F9A/op7"),
        ("node_attempt_id", "3f9a/op4"),
        ("program_run_id", "3f9/run3"),
    ],
)
def test_a_history_id_filter_refuses_other_forms(name: str, text: str) -> None:
    with pytest.raises(ValueError):
        parse_id_filter(name, text)


def test_a_group_stop_reply_is_displayed_by_its_stop_event() -> None:
    """A group Stop's acceptance holds the Stop event in ``accepted.stop``."""

    reply = {
        "ok": True,
        "command": "stop_program_selection",
        "accepted": {
            "schema_version": "manyforge.program_execution_selection.v3",
            "state": None,
            "group_ids": ["all"],
            "selected_node_indices": [],
            "widened_node_indices": [],
            "stop": {"event_id": 6, "request_key": "c0" + "ab" * 15},
        },
    }
    assert identified(reply, A)["display"] == "3f9a/stop6"


def test_runner_and_headless_form_the_same_http_key() -> None:
    """The Program Runner predicts the key Headless forms for its HTTP
    requests (its Scene-clear receipt check); both packages keep the rule."""

    from manyforge_behavior.request_keys import RUNNER_KEYS, format_request_key, http_client_key
    from manyforge_headless_api.identity import http_request_key

    for _ in range(3):
        sent = RUNNER_KEYS.next_key()
        assert http_client_key(sent) == http_request_key(format_request_key(sent))
        assert http_client_key(sent) != sent
