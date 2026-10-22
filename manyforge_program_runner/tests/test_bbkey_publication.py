"""Change-only BB key publication per captured Run (contract 52)."""

from __future__ import annotations

from collections.abc import Callable
from copy import deepcopy
from pathlib import Path
import threading
from typing import Any

import pytest

from manyforge_behavior.canonical_artifact import compute_content_identity
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.orchestration_values import (
    ProgramValueError,
    ProgramValueStore,
)
from manyforge_behavior.program import Program
from manyforge_headless_api.native import HeadlessApiError
from manyforge_program_runner.bbkey_publication import (
    FAULT_CODE,
    MAXIMUM_PENDING_FRAMES,
    ProgramBlackboardKeyPublisher,
    ProgramBlackboardPublicationScope,
)
from manyforge_program_runner.execution_transport import (
    ProgramRunnerExecutionTransport,
)
from manyforge_program_runner.liveness import ProgramRunnerLivenessError
from manyforge_program_runner.runtime import ProgramRuntime

from manyforge_program_runner.tests.test_liveness import _expected_status_digest, _heartbeat, _receiver

_ROOT = Path(__file__).resolve().parents[2]
_CATALOG = NodeCatalog(_ROOT / "manyforge_behavior/resources/node_catalog.yaml")
_BUDGET_NS = 30_000_000_000
_KEYS = ("count", "door_open", "label", "speed")


def _key(key: str, value_type: str, **extra: Any) -> dict[str, Any]:
    return {
        "key": key,
        "type": value_type,
        "access": "read_write",
        "persistence": "run",
        **extra,
    }


def _program(*extra: dict[str, Any]) -> Program:
    document = Program(
        _ROOT / "examples/pick_and_place_minimal.program.yaml", _CATALOG
    ).to_dict()
    document["blackboard"] = {
        "namespaces": [],
        "keys": sorted(
            [
                _key("count", "int64", initial=0),
                _key("door_open", "bool", initial=False),
                _key("label", "string"),
                _key("speed", "float64", initial=0.5),
                _key("pose", "stored_pose"),
                _key("total", "int64", persistence="persistent", initial=1),
                *extra,
            ],
            key=lambda row: row["key"],
        ),
    }
    document["program_revision"] = compute_content_identity(
        deepcopy(document),
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v2",
    )
    return Program.from_dict(document, _CATALOG)


class _Clock:
    def __init__(self) -> None:
        self.now = 1_000

    def __call__(self) -> int:
        return self.now


class _Queue:
    """A manual stand-in for the tick pool: work runs only when drained."""

    def __init__(self) -> None:
        self.work: list[Callable[[], None]] = []

    def __call__(self, work: Callable[[], None]) -> None:
        self.work.append(work)

    def run(self) -> None:
        while self.work:
            self.work.pop(0)()


class _Host:
    def __init__(self, *, fail_with: Exception | None = None) -> None:
        self.frames: list[dict[str, Any]] = []
        self.fail_with = fail_with
        self.during: Callable[[dict[str, Any]], None] | None = None

    def __call__(self, request: dict[str, Any]) -> dict[str, Any]:
        self.frames.append(deepcopy(dict(request)))
        if self.during is not None:
            self.during(request)
        if self.fail_with is not None:
            raise self.fail_with
        return {
            "api_version": "manyforge.execution_command_response.v1",
            "command": "publish_program_bbkeys",
            "ok": True,
            "status": "accepted",
            "owner_epoch": request["owner_epoch"],
            "program_binding_id": request["program_binding_id"],
            "active_publication_generation": request["active_publication_generation"],
            "cycle_generation": request["cycle_generation"],
            "program_run_id": request["program_run_id"],
            "source_epoch": "source-1",
            "accepted_sequence": request["message_sequence"],
        }


def _publisher(
    host: _Host,
    submit: Callable[[Callable[[], None]], None],
    *,
    clock: _Clock | None = None,
    events: list[Any] | None = None,
    between_runs: bool = False,
) -> ProgramBlackboardKeyPublisher:
    events = [] if events is None else events
    return ProgramBlackboardKeyPublisher(
        ProgramBlackboardPublicationScope(
            owner_epoch="kernel-owner",
            program_runner_owner_epoch="runner-owner",
            claim_generation=None if between_runs else 3,
            program_run_id=None if between_runs else "run-1",
            program_binding_id="sha256:" + "d" * 64,
            active_publication_generation=5,
            cycle_generation=7,
        ),
        publish=host,
        submit=submit,
        stop_run=lambda run_id: events.append(("stop", run_id)),
        on_fault=lambda fault, reference: events.append(("fault", fault, reference)),
        fault_reference=ProgramRuntime._digest,  # noqa: SLF001
        budget_ns=_BUDGET_NS,
        **({} if clock is None else {"monotonic_ns": clock}),
    )


def _inline(work: Callable[[], None]) -> None:
    work()


def test_baseline_then_ordered_typed_updates_cleared_by_exact_ack() -> None:
    store = _store()
    host, queue = _Host(), _Queue()
    publisher = _publisher(host, queue)
    publisher.start(store, _KEYS)
    store.write("count", 7)
    store.write("label", "café")
    store.write("count", 8)
    assert publisher.pending_sequences() == (1, 2, 3)

    queue.run()

    assert publisher.pending_sequences() == ()
    assert [
        (f["kind"], f["message_sequence"], f["previous_sequence"]) for f in host.frames
    ] == [("baseline", "1", "0"), ("update", "2", "1"), ("update", "3", "2")]
    # The baseline carries the whole union; an unset key is null.
    assert host.frames[0]["values"] == [
        {"name": "count", "value_type": "int64", "value": "0"},
        {"name": "door_open", "value_type": "bool", "value": False},
        {"name": "label", "value_type": "string", "value": None},
        {"name": "speed", "value_type": "float64", "value": 0.5},
    ]
    # Updates carry only changed keys (bb-wire-shapes03). Different keys
    # share an unsent frame; the same key starts the next one.
    assert host.frames[1]["values"] == [
        {"name": "count", "value_type": "int64", "value": "7"},
        {"name": "label", "value_type": "string", "value": "café"},
    ]
    assert host.frames[2]["values"] == [
        {"name": "count", "value_type": "int64", "value": "8"},
    ]
    assert {
        key: host.frames[0][key]
        for key in ("claim_generation", "kind", "active_publication_generation")
    } == {"claim_generation": 3, "kind": "baseline", "active_publication_generation": 5}
    assert all(len(frame) == 12 for frame in host.frames)
    assert publisher.fault is None


def test_unchanged_and_unsubscribed_writes_create_no_obligation() -> None:
    store = _store()
    host, queue = _Host(), _Queue()
    publisher = _publisher(host, queue)
    publisher.start(store, ("count", "door_open"))
    store.write("count", 0)
    store.write("speed", 2.0)
    assert publisher.pending_sequences() == (1,)


def test_one_request_in_flight_never_absorbs_later_writes() -> None:
    store = _store()
    host, queue = _Host(), _Queue()
    publisher = _publisher(host, queue)
    publisher.start(store, _KEYS)
    queue.run()
    store.write("door_open", True)

    def write_while_in_flight(request: dict[str, Any]) -> None:
        if request["message_sequence"] == "2":
            store.write("speed", 1.5)
            assert publisher.pending_sequences() == (2, 3)

    host.during = write_while_in_flight
    queue.run()
    rows = [
        {row["name"]: row["value"] for row in frame["values"]}
        for frame in host.frames[1:]
    ]
    assert rows == [{"door_open": True}, {"speed": 1.5}]


def test_baseline_capture_is_atomic_with_concurrent_writes() -> None:
    store = _store()
    host = _Host()
    publisher = _publisher(host, _inline)
    started = threading.Event()

    def writer() -> None:
        for value in range(1, 2_001):
            store.write("count", value)
            if value == 200:
                started.set()

    thread = threading.Thread(target=writer)
    thread.start()
    started.wait(timeout=5)
    publisher.start(store, ("count",))
    thread.join(timeout=10)

    published = [int(frame["values"][0]["value"]) for frame in host.frames]
    assert host.frames[0]["kind"] == "baseline"
    # Every change after the baseline arrives once, in order, ending last.
    assert published == list(range(published[0], 2_001))
    assert publisher.fault is None


def test_budget_expiry_latches_fault_stop_first_and_fences_liveness(
    tmp_path: Path,
) -> None:
    receiver, _rclock, _losses, _reconciliations = _receiver(tmp_path)
    receiver._handle(_heartbeat())  # noqa: SLF001
    events: list[Any] = []
    clock = _Clock()
    store = _store()
    host, queue = _Host(), _Queue()
    publisher = ProgramBlackboardKeyPublisher(
        ProgramBlackboardPublicationScope(
            "kernel-owner", "runner-owner", 3, "run-1", "sha256:" + "d" * 64, 5, 7
        ),
        publish=host,
        submit=queue,
        stop_run=lambda run_id: events.append(("stop", run_id)),
        on_fault=lambda fault, reference: (
            events.append(("fault", fault["code"])),
            receiver.latch_publisher_fault(fault, reference),
        ),
        fault_reference=ProgramRuntime._digest,  # noqa: SLF001
        budget_ns=_BUDGET_NS,
        monotonic_ns=clock,
    )
    receiver._publication_check = publisher.check_budget  # noqa: SLF001
    publisher.start(store, _KEYS)
    clock.now += _BUDGET_NS
    receiver.check_publication()
    assert publisher.fault is None
    clock.now += 1
    receiver.check_publication()

    assert events == [("stop", "run-1"), ("fault", FAULT_CODE)]
    assert publisher.pending_sequences() == ()
    acknowledgement = receiver._handle(_heartbeat(sequence=2))  # noqa: SLF001
    assert len(acknowledgement) == 24
    assert acknowledgement["publisher_fault"]["code"] == FAULT_CODE
    assert acknowledgement["publisher_fault"]["details"] == {}
    assert acknowledgement["state"] == "current"
    assert acknowledgement["command_fenced"] is True
    assert acknowledgement["fault_reference"] == ProgramRuntime._digest(  # noqa: SLF001
        acknowledgement["publisher_fault"]
    )
    assert acknowledgement["last_accepted_heartbeat_sequence"] == 2
    assert acknowledgement["receiver_status_digest"] == _expected_status_digest(
        acknowledgement
    )
    assert receiver.manager_fault_reference is None
    with pytest.raises(ProgramRunnerLivenessError, match="publication fault"):
        receiver.require_current()
    # No frame is sent after the fault, even if the tick pool runs.
    queue.run()
    store.write("count", 5)
    queue.run()
    assert host.frames == []


def test_reconciliation_and_later_heartbeats_never_clear_the_latch(
    tmp_path: Path,
) -> None:
    receiver, _clock, _losses, _reconciliations = _receiver(tmp_path)
    receiver._handle(_heartbeat())  # noqa: SLF001
    probe = receiver._handle(  # noqa: SLF001
        {"api_version": _heartbeat()["api_version"], "cell_id": "cell", "kind": "probe"}
    )
    assert len(probe) == 10
    healthy = receiver._handle(_heartbeat(sequence=2))  # noqa: SLF001
    assert healthy["publisher_fault"] is None and len(healthy) == 24
    receiver.latch_publisher_fault(
        {"code": FAULT_CODE, "message": "lost", "details": {}}, "sha256:" + "e" * 64
    )
    for sequence in range(3, 6):
        result = receiver._handle(_heartbeat(sequence=sequence))  # noqa: SLF001
        assert result["publisher_fault"] == {
            "code": FAULT_CODE,
            "message": "lost",
            "details": {},
        }
        assert result["command_fenced"] is True
        assert result["fault_reference"] == "sha256:" + "e" * 64
        assert result["last_accepted_heartbeat_sequence"] == sequence
    with pytest.raises(ProgramRunnerLivenessError):
        receiver.require_current()
    with pytest.raises(ProgramRunnerLivenessError):
        receiver.latch_publisher_fault(
            {"code": "other", "message": "x", "details": {}}, "sha256:" + "e" * 64
        )


def test_unknown_reply_latches_once_and_never_resends() -> None:
    events: list[Any] = []
    store = _store()
    host, queue = _Host(fail_with=HeadlessApiError("execution_owner_unavailable", "lost")), _Queue()
    publisher = _publisher(host, queue, events=events)
    publisher.start(store, _KEYS)
    store.write("count", 1)
    queue.run()
    store.write("count", 2)
    queue.run()

    assert [frame["message_sequence"] for frame in host.frames] == ["1"]
    assert [event[0] for event in events] == ["stop", "fault"]
    assert events[1][1]["code"] == FAULT_CODE
    assert publisher.pending_sequences() == ()


def test_wrong_ack_sequence_is_a_fault() -> None:
    events: list[Any] = []
    store = _store()
    host, queue = _Host(), _Queue()

    def wrong(request: dict[str, Any]) -> dict[str, Any]:
        return {**host(request), "accepted_sequence": "9"}

    publisher = _publisher(wrong, queue, events=events)  # type: ignore[arg-type]
    publisher.start(store, _KEYS)
    queue.run()
    assert [event[0] for event in events] == ["stop", "fault"]


def test_pending_frame_overflow_fails_closed() -> None:
    events: list[Any] = []
    store = _store()
    queue = _Queue()
    publisher = _publisher(_Host(), queue, events=events)
    publisher.start(store, ("count",))
    for value in range(1, MAXIMUM_PENDING_FRAMES + 1):
        store.write("count", value)
    queue.work.pop(0)  # The sender never ran: frames stay unsent.
    queue.run()
    assert [event[0] for event in events] == ["stop", "fault"]


def _store(program: Program | None = None) -> ProgramValueStore:
    return ProgramValueStore(
        program or _program(),
        persistent_commit=lambda _declaration, _value, _metadata: None,
    )


def test_reset_retires_the_run_source_locally_without_a_clearing_frame() -> None:
    events: list[Any] = []
    store = _store()
    host, queue = _Host(), _Queue()
    publisher = _publisher(host, queue, events=events)
    publisher.start(store, ("count", "label", "total"))
    store.write("label", "held")
    queue.run()
    # Held/finished: the values stay and nothing is sent or retired.
    assert publisher.pending_sequences() == ()
    store.write("total", 9)
    publisher.retire()  # The Runner's own Reset transition (DX-1281).
    store.clear_run_values()
    queue.run()
    assert [frame["message_sequence"] for frame in host.frames] == ["1", "2"]
    assert events == [] and publisher.pending_sequences() == ()


@pytest.mark.parametrize(
    "reply",
    [
        HeadlessApiError("program_bbkey_publication_stale", "retired"),
        HeadlessApiError("execution_owner_unavailable", "lost"),
        "superseded",
    ],
)
def test_a_late_reply_for_a_source_the_runner_retired_is_recorded_only(reply) -> None:
    events: list[Any] = []
    store = _store()
    host, queue = _Host(), _Queue()
    publisher = _publisher(host, queue, events=events)
    publisher.start(store, ("count",))

    def retire_while_in_flight(request: dict[str, Any]) -> dict[str, Any]:
        host(request)
        publisher.retire()  # Reset won while this frame was in flight.
        if reply == "superseded":
            return _superseded(request)
        raise reply

    publisher._publish = retire_while_in_flight  # noqa: SLF001
    queue.run()
    assert events == [] and publisher.fault is None


def test_unknown_or_stale_reply_for_the_current_source_still_latches() -> None:
    events: list[Any] = []
    store = _store()
    host = _Host(fail_with=HeadlessApiError("program_bbkey_publication_stale", "stale"))
    queue = _Queue()
    publisher = _publisher(host, queue, events=events)
    publisher.start(store, ("count",))
    queue.run()
    assert [event[0] for event in events] == ["stop", "fault"]


def test_source_not_ready_is_retried_within_the_budget_then_latches() -> None:
    events: list[Any] = []
    clock = _Clock()
    store = _store()
    host = _Host(fail_with=HeadlessApiError("bb_source_not_ready", "Apply installing"))
    queue = _Queue()
    publisher = _publisher(host, queue, clock=clock, events=events, between_runs=True)
    publisher.start(store, ("count",))
    queue.run()
    # Kept, not resent until the next observation, no fault.
    assert publisher.pending_sequences() == (1,) and events == []
    store.write("count", 4)
    queue.run()
    assert len(host.frames) == 1
    clock.now += _BUDGET_NS // 2
    assert publisher.check_budget() is False
    host.fail_with = None
    queue.run()
    assert [frame["message_sequence"] for frame in host.frames] == ["1", "1", "2"]
    assert publisher.pending_sequences() == () and events == []
    # Still not ready when the overall budget is spent: the fault latches.
    host.fail_with = HeadlessApiError("bb_source_not_ready", "Apply installing")
    store.write("count", 5)
    queue.run()
    clock.now += _BUDGET_NS + 1
    assert publisher.check_budget() is True
    assert [event[0] for event in events] == ["fault"]
    assert "source not ready" in events[0][1]["message"]


def test_computed_key_is_published_only_when_its_value_changes() -> None:
    pytest.importorskip("manyforge_headless_py")
    store = _store(_program(_key("double", "int64", access="read", expression="key.count * 2")))
    host, queue = _Host(), _Queue()
    publisher = _publisher(host, queue)
    publisher.start(store, ("door_open", "double"))
    store.write("count", 3)
    store.write("speed", 1.0)
    store.write("count", 3)
    queue.run()
    assert [
        {row["name"]: row["value"] for row in frame["values"]} for frame in host.frames
    ] == [
        {"door_open": False, "double": "0"},
        {"double": "6"},
    ]


def test_next_run_or_shutdown_retires_the_sender() -> None:
    events: list[Any] = []
    clock = _Clock()
    store = _store()
    host, queue = _Host(), _Queue()
    publisher = _publisher(host, queue, clock=clock, events=events)
    publisher.start(store, _KEYS)
    publisher.retire()
    store.write("count", 4)
    queue.run()
    clock.now += _BUDGET_NS * 2
    assert publisher.check_budget() is False
    assert host.frames == [] and events == []
    assert publisher.pending_sequences() == ()


def test_subscription_is_fixed_once_and_scalar_only() -> None:
    store = _store()
    publisher = _publisher(_Host(), _Queue())
    for keys in (("pose",), ("undeclared",), ("count", "count"), ("speed", "count"), ()):
        with pytest.raises(ProgramValueError):
            store.subscribe_published_keys(
                keys, baseline=lambda _rows: None, on_change=lambda _change: None
            )
    publisher.start(store, ("count",))
    with pytest.raises(ProgramValueError, match="already fixed"):
        store.subscribe_published_keys(
            ("speed",), baseline=lambda _rows: None, on_change=lambda _change: None
        )


def test_transport_sends_closed_frame_and_validates_the_ack() -> None:
    transport = object.__new__(ProgramRunnerExecutionTransport)
    transport._attribution_transport = "internal"  # noqa: SLF001
    sent: list[dict[str, Any]] = []
    frame = {
        "schema_version": "manyforge.program_bbkey_publication.v1",
        "owner_epoch": "kernel-owner",
        "program_runner_owner_epoch": "runner-owner",
        "claim_generation": 3,
        "program_run_id": "run-1",
        "program_binding_id": "sha256:" + "d" * 64,
        "active_publication_generation": 5,
        "cycle_generation": 7,
        "kind": "update",
        "message_sequence": "2",
        "previous_sequence": "1",
        "values": [{"name": "count", "value_type": "int64", "value": "-4"}],
    }
    reply = {
        "api_version": "manyforge.execution_command_response.v1",
        "command": "publish_program_bbkeys",
        "ok": True,
        "status": "accepted",
        "owner_epoch": "kernel-owner",
        "program_binding_id": "sha256:" + "d" * 64,
        "active_publication_generation": 5,
        "cycle_generation": 7,
        "program_run_id": "run-1",
        "source_epoch": "source-1",
        "accepted_sequence": "2",
    }
    transport._exchange = lambda command: sent.append(command) or dict(reply)  # type: ignore[method-assign]  # noqa: SLF001

    assert transport.publish_program_bbkeys(frame)["accepted_sequence"] == "2"
    assert sent[0]["command"] == "publish_program_bbkeys"
    assert sent[0]["request"] == frame
    assert len(sent[0]["request"]) == 12  # The closed bb-wire-shapes03 request.
    for field, wrong in (
        ("program_binding_id", "sha256:" + "e" * 64),
        ("active_publication_generation", 6),
        ("cycle_generation", 8),
        ("owner_epoch", "other-owner"),
    ):
        with pytest.raises(HeadlessApiError):  # Not this frame's occurrence.
            transport._exchange = lambda command, f=field, w=wrong: {**reply, f: w}  # type: ignore[method-assign]  # noqa: SLF001
            transport.publish_program_bbkeys(frame)
    for bad_reply in (
        {k: v for k, v in reply.items() if k != "status"},
        {k: v for k, v in reply.items() if k != "cycle_generation"},
        {k: v for k, v in reply.items() if k != "active_publication_generation"},
        {**reply, "active_publication_generation": "5"},
        {**reply, "status": "queued"},
        {**reply, "accepted_sequence": None},
        {**reply, "extra": 1},
    ):
        transport._exchange = lambda command, r=bad_reply: dict(r)  # type: ignore[method-assign]  # noqa: SLF001
        with pytest.raises(HeadlessApiError):
            transport.publish_program_bbkeys(frame)
    # The owner's ErrorDTO keeps its code (SX-2143 transient retry).
    transport._exchange = lambda command: {  # type: ignore[method-assign]  # noqa: SLF001
        "api_version": "manyforge.execution_command_response.v1",
        "command": "publish_program_bbkeys",
        "ok": False,
        "error": {"code": "bb_source_not_ready", "message": "installing", "details": {}},
    }
    with pytest.raises(HeadlessApiError) as refused:
        transport.publish_program_bbkeys(frame)
    assert refused.value.code == "bb_source_not_ready"
    # Superseded names the installed replacement, never this frame.
    superseded = {
        **reply, "status": "superseded", "accepted_sequence": None,
        "cycle_generation": 8, "program_run_id": None, "source_epoch": "source-2",
    }
    transport._exchange = lambda command: dict(superseded)  # type: ignore[method-assign]  # noqa: SLF001
    assert transport.publish_program_bbkeys(frame)["status"] == "superseded"
    for bad_reply in (
        {**superseded, "accepted_sequence": "2"},
        {**reply, "status": "superseded", "accepted_sequence": None},
    ):
        transport._exchange = lambda command, r=bad_reply: dict(r)  # type: ignore[method-assign]  # noqa: SLF001
        with pytest.raises(HeadlessApiError):
            transport.publish_program_bbkeys(frame)
    transport._exchange = lambda command: sent.append(command) or dict(reply)  # type: ignore[method-assign]  # noqa: SLF001
    unset = {**frame, "values": [{"name": "label", "value_type": "string", "value": None}]}
    assert transport.publish_program_bbkeys(unset)["accepted_sequence"] == "2"
    for bad in (
        {"previous_sequence": "0"},
        {"kind": "renewal"},
        {"values": [{"name": "count", "value_type": "int64", "value": 4}]},
        {"values": []},
        {"values": [{"name": "count", "value_type": "bool", "value": "4"}]},
        {"active_publication_generation": 0},
        {"active_publication_generation": None},
    ):
        with pytest.raises(HeadlessApiError):
            transport.publish_program_bbkeys({**frame, **bad})
    between_runs = {**frame, "claim_generation": None, "program_run_id": None}
    with pytest.raises(HeadlessApiError):  # The reply names a Run.
        transport.publish_program_bbkeys(between_runs)
    reply["program_run_id"] = None
    assert transport.publish_program_bbkeys(between_runs)["program_run_id"] is None
    with pytest.raises(HeadlessApiError):  # Claim and Run go together.
        transport.publish_program_bbkeys({**frame, "claim_generation": None})
    reply["program_run_id"] = "run-1"
    reply["accepted_sequence"] = "3"
    with pytest.raises(HeadlessApiError):
        transport.publish_program_bbkeys(frame)


def test_cell_level_source_between_runs_has_no_run_and_never_stops_one() -> None:
    events: list[Any] = []
    store = _store()
    store.clear_run_values()
    host = _Host(fail_with=HeadlessApiError("execution_owner_unavailable", "lost"))
    queue = _Queue()
    publisher = _publisher(host, queue, events=events, between_runs=True)
    publisher.start(store, ("count", "total"))
    queue.run()
    frame = host.frames[0]
    assert (frame["claim_generation"], frame["program_run_id"], frame["cycle_generation"]) == (
        None,
        None,
        7,
    )
    assert {row["name"]: row["value"] for row in frame["values"]} == {
        "count": None,
        "total": "1",
    }
    assert [event[0] for event in events] == ["fault"]


def _superseded(request: dict[str, Any]) -> dict[str, Any]:
    """The actual SX-2105 reply: the installed replacement, no sequence."""

    return {
        "api_version": "manyforge.execution_command_response.v1",
        "command": "publish_program_bbkeys",
        "ok": True,
        "status": "superseded",
        "owner_epoch": request["owner_epoch"],
        "program_binding_id": request["program_binding_id"],
        "active_publication_generation": request["active_publication_generation"],
        "cycle_generation": request["cycle_generation"] + 1,
        "program_run_id": None,
        "source_epoch": "source-2",
        "accepted_sequence": None,
    }


def test_superseded_reply_retires_only_that_obligation() -> None:
    events: list[Any] = []
    store = _store()
    host, queue = _Host(), _Queue()
    publisher = _publisher(host, queue, events=events)
    publisher.start(store, ("count", "label"))
    queue.run()
    store.write("label", "held")
    store.write("count", 2)
    store.write("count", 3)
    publisher._publish = lambda request: host(request) and _superseded(request)  # noqa: SLF001
    queue.run()
    assert [frame["message_sequence"] for frame in host.frames] == ["1", "2", "3"]
    assert events == []  # No Program Stop, no publisher fault.
    assert publisher.fault is None and publisher.pending_sequences() == ()


def test_between_run_baseline_start_failure_with_keys_latches_the_fault(
    tmp_path: Path,
) -> None:
    receiver, _clock, _losses, _reconciliations = _receiver(tmp_path)
    receiver._handle(_heartbeat())  # noqa: SLF001
    runtime = object.__new__(ProgramRuntime)
    runtime._liveness = receiver  # noqa: SLF001
    runtime._bbkey_publishers = {}  # noqa: SLF001
    runtime._bbkey_idle_store = None  # noqa: SLF001
    runtime._bbkey_publishers_lock = threading.RLock()  # noqa: SLF001
    runtime._interrupt_state_mirror = None  # noqa: SLF001

    def fail_start(*, keys: tuple[str, ...]) -> None:
        raise RuntimeError("Applied Program unreadable")

    runtime._start_idle_bbkey_publication = fail_start  # type: ignore[method-assign]  # noqa: SLF001
    runtime._cell_bbkey_union = lambda: ()  # type: ignore[method-assign]  # noqa: SLF001
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    assert receiver._handle(_heartbeat(sequence=2))["publisher_fault"] is None  # noqa: SLF001
    runtime._cell_bbkey_union = lambda: ("count",)  # type: ignore[method-assign]  # noqa: SLF001
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    acknowledgement = receiver._handle(_heartbeat(sequence=3))  # noqa: SLF001
    assert acknowledgement["publisher_fault"]["code"] == FAULT_CODE
    assert "Applied Program unreadable" in acknowledgement["publisher_fault"]["message"]
    assert acknowledgement["command_fenced"] is True
    with pytest.raises(ProgramRunnerLivenessError):
        receiver.require_current()


# Runtime wiring: between-Runs source identity (bb-wire-shapes03) -----------


class _Journal:
    def __init__(self, state: str = "idle_after_reset", cycle_generation: int = 7) -> None:
        self.state = state
        self.cycle_generation = cycle_generation

    def read(self) -> "_Journal":
        return self


class _Applied:
    def __init__(self) -> None:
        self.document = _program(_key("door", "bool", initial=False)).to_dict()
        self.binding = "sha256:" + "d" * 64
        self.bundle = "bundle-1"
        self.generation = 5
        self.reads = 0

    def program_applied(self) -> dict[str, Any]:
        self.reads += 1
        return {
            "program": deepcopy(self.document),
            "program_binding_id": self.binding,
            "scenario_id": "scenario-1",
            "project_path": "/projects/scenario-1.scenario",
            "retained_bundle_id": "bundle-1",
            "graph_revision": "graph-1",
        }

    def identity(self) -> dict[str, Any]:
        return {
            "ok": True,
            "identity": {
                "active_publication_generation": self.generation,
                "retained_bundle_id": self.bundle,
                "graph_revision": "graph-1",
            },
        }


class _Register:
    def restore(self, _project_path: str, _persistent: Any) -> dict[str, Any]:
        return {"total": 4}


class _Pool:
    def submit_run_work(self, work: Callable[[], None]) -> None:
        work()


def _runtime(applied: _Applied, journal: _Journal) -> tuple[ProgramRuntime, list[Any]]:
    runtime = object.__new__(ProgramRuntime)
    hosts: list[Any] = []
    runtime._liveness = None  # noqa: SLF001
    runtime._bbkey_publishers = {}  # noqa: SLF001
    runtime._bbkey_idle_store = None  # noqa: SLF001
    runtime._bbkey_publishers_lock = threading.RLock()  # noqa: SLF001
    runtime._interrupt_state_mirror = None  # noqa: SLF001
    runtime._interrupt_state_run_id = None  # noqa: SLF001
    runtime._applied_condition_keys = ()  # noqa: SLF001
    runtime._journal = journal  # type: ignore[assignment]  # noqa: SLF001
    runtime._persistent_keys = _Register()  # type: ignore[assignment]  # noqa: SLF001
    runtime._headless = applied  # type: ignore[assignment]  # noqa: SLF001
    runtime._kernel = applied  # type: ignore[assignment]  # noqa: SLF001
    runtime._host = _Pool()  # type: ignore[assignment]  # noqa: SLF001
    runtime._environment = type("Env", (), {"catalog": _CATALOG})()  # type: ignore[assignment]  # noqa: SLF001
    runtime.owner_epoch = "runner-owner"
    runtime._current_kernel_owner_epoch = lambda: "kernel-owner"  # type: ignore[method-assign]  # noqa: SLF001

    def new_publisher(scope: ProgramBlackboardPublicationScope) -> ProgramBlackboardKeyPublisher:
        host = _Host()
        hosts.append(host)
        return _publisher_for(scope, host)

    runtime._new_bbkey_publisher = new_publisher  # type: ignore[method-assign]  # noqa: SLF001
    runtime._execution_transport = type(  # type: ignore[assignment]  # noqa: SLF001
        "Transport", (), {"publish_program_bbkeys": lambda self, body: {}}
    )()
    return runtime, hosts


def _publisher_for(
    scope: ProgramBlackboardPublicationScope, host: _Host
) -> ProgramBlackboardKeyPublisher:
    return ProgramBlackboardKeyPublisher(
        scope,
        publish=host,
        submit=_inline,
        stop_run=lambda _run_id: None,
        on_fault=lambda _fault, _reference: None,
        fault_reference=ProgramRuntime._digest,  # noqa: SLF001
        budget_ns=_BUDGET_NS,
    )


def test_every_new_applied_publication_is_a_new_between_run_source() -> None:
    applied, journal = _Applied(), _Journal()
    runtime, hosts = _runtime(applied, journal)
    runtime._applied_condition_keys = ("door", "total")  # noqa: SLF001
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    (first,) = hosts
    assert first.frames[0]["active_publication_generation"] == 5
    assert {row["name"]: row["value"] for row in first.frames[0]["values"]} == {
        "door": None, "total": "4",
    }
    runtime._follow_cell_bbkey_source()  # The same publication notice: no-op.
    assert len(hosts) == 1
    applied.generation = 6  # A new Apply of the same binding and cycle.
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    assert len(hosts) == 2 and hosts[1].frames[0]["active_publication_generation"] == 6
    assert runtime._bbkey_publishers[None].scope.active_publication_generation == 6  # noqa: SLF001


def test_host_status_for_another_applied_occurrence_waits_for_its_notice() -> None:
    applied, journal = _Applied(), _Journal()
    applied.bundle = "bundle-0"
    runtime, hosts = _runtime(applied, journal)
    runtime._applied_condition_keys = ("door",)  # noqa: SLF001
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    assert hosts == [] and None not in runtime._bbkey_publishers  # noqa: SLF001


@pytest.mark.parametrize("state, expected", [("idle_after_reset", 1), ("running", 0)])
def test_runner_start_establishes_the_between_run_source(state: str, expected: int) -> None:
    applied, journal = _Applied(), _Journal(state)
    applied.document = _program(_key("door", "bool", initial=False)).to_dict()
    runtime, hosts = _runtime(applied, journal)
    runtime._learn_condition_keys = lambda program: ("door",)  # type: ignore[method-assign]  # noqa: SLF001
    runtime._cell_bbkey_union = lambda: ("door",)  # type: ignore[method-assign]  # noqa: SLF001
    runtime._establish_startup_bbkey_source()  # noqa: SLF001
    assert len(hosts) == expected


def test_runtime_reset_retires_the_run_sender_without_a_frame() -> None:
    applied, journal = _Applied(), _Journal()
    runtime, _hosts = _runtime(applied, journal)
    host = _Host()
    publisher = _publisher_for(
        ProgramBlackboardPublicationScope(
            "kernel-owner", "runner-owner", 3, "run-1", "sha256:" + "d" * 64, 5, 7
        ),
        host,
    )
    store = _store()
    publisher.start(store, ("count",))
    runtime._bbkey_publishers["run-1"] = publisher  # noqa: SLF001
    runtime._reset_bbkey_publication("run-1")  # noqa: SLF001
    store.write("count", 3)
    assert "run-1" not in runtime._bbkey_publishers  # noqa: SLF001
    assert [frame["kind"] for frame in host.frames] == ["baseline"]


def test_manager_loss_and_publisher_fault_keep_each_cause(tmp_path: Path) -> None:
    # SX-2210a: with both faults the emitted fault_reference is the digest of
    # the emitted publisher ErrorDTO; the Manager loss keeps its own field.
    from manyforge_program_runner.liveness import EXPIRY_TIMEOUT_NS

    receiver, clock, losses, _reconciliations = _receiver(tmp_path)
    receiver._handle({**_heartbeat(), "reconciliation_digest": None})  # noqa: SLF001
    clock.now += EXPIRY_TIMEOUT_NS + 1
    assert receiver.check_expiry()
    fault = {"code": FAULT_CODE, "message": "lost", "details": {}}
    receiver.latch_publisher_fault(fault, ProgramRuntime._digest(fault))  # noqa: SLF001
    status = receiver.status()
    assert status["publisher_fault"] == fault
    assert status["fault_reference"] == ProgramRuntime._digest(  # noqa: SLF001
        status["publisher_fault"]
    )
    assert status["command_fenced"] is True
    assert receiver.manager_fault_reference == losses[0]
    assert losses[0] != status["fault_reference"]
