"""Change-only BB key publication per captured Run (contract 52)."""

from __future__ import annotations

from collections.abc import Callable
from copy import deepcopy
from pathlib import Path
import itertools
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
    ProgramBlackboardKeyPublisher,
    ProgramBlackboardPublicationScope,
)
from manyforge_program_runner.active_tcp_mirror import ActiveTcpMirror
from manyforge_program_runner.execution_transport import (
    ProgramRunnerExecutionTransport,
)
from manyforge_program_runner.liveness import ProgramRunnerLivenessError
from manyforge_program_runner.runtime import ProgramRuntime

from manyforge_program_runner.tests.test_liveness import _expected_status_digest, _heartbeat, _receiver

_ROOT = Path(__file__).resolve().parents[2]
_CATALOG = NodeCatalog(_ROOT / "manyforge_behavior/resources/node_catalog.yaml")
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
    )


def _inline(work: Callable[[], None]) -> None:
    work()


def _rows(frame: dict[str, Any]) -> dict[str, Any]:
    return {row["name"]: row["value"] for row in frame["values"]}


_LOSS_SEQUENCE = str(2**64 - 1)


def test_baseline_then_each_write_forwarded_before_it_returns() -> None:
    # BBK-1: no pool, no queue: a write returns after its frame's ACK, so
    # the writer's next step (a node's motion) follows the Kernel's
    # evaluation of it. One frame per write, never merged.
    store = _store()
    host, queue = _Host(), _Queue()
    publisher = _publisher(host, queue)
    assert publisher.start(store, _KEYS) is True
    store.write("count", 7)
    assert [frame["message_sequence"] for frame in host.frames] == ["1", "2"]
    store.write("label", "café")
    store.write("count", 8)
    assert queue.work == []

    assert [
        (f["kind"], f["message_sequence"], f["previous_sequence"]) for f in host.frames
    ] == [
        ("baseline", "1", "0"), ("update", "2", "1"), ("update", "3", "2"),
        ("update", "4", "3"),
    ]
    # The baseline carries the whole union; an unset key is null.
    assert host.frames[0]["values"] == [
        {"name": "count", "value_type": "int64", "value": "0"},
        {"name": "door_open", "value_type": "bool", "value": False},
        {"name": "label", "value_type": "string", "value": None},
        {"name": "speed", "value_type": "float64", "value": 0.5},
    ]
    # An update carries only its written key (bb-wire-shapes03).
    assert [_rows(frame) for frame in host.frames[1:]] == [
        {"count": "7"}, {"label": "café"}, {"count": "8"},
    ]
    assert {
        key: host.frames[0][key]
        for key in ("claim_generation", "kind", "active_publication_generation")
    } == {"claim_generation": 3, "kind": "baseline", "active_publication_generation": 5}
    assert all(len(frame) == 12 for frame in host.frames)
    assert publisher.fault is None


def test_a_pulse_reaches_the_kernel_as_two_frames() -> None:
    # BBK-1: true then false at once is two evaluations, never the latest.
    store = _store()
    host = _Host()
    publisher = _publisher(host, _Queue())
    publisher.start(store, ("door_open",))
    store.write("door_open", True)
    store.write("door_open", False)
    assert [_rows(frame) for frame in host.frames] == [
        {"door_open": False}, {"door_open": True}, {"door_open": False},
    ]


def test_operator_write_then_tree_write_are_both_forwarded_in_order() -> None:
    # BBK-1: every writer commits through the one store. An operator write
    # (runtime control) still in flight holds the tree's write until its
    # frame is ACKed: the Kernel sees true, then false.
    store = _store()
    host = _Host()
    publisher = _publisher(host, _Queue())
    publisher.start(store, ("door_open",))
    in_flight, release = threading.Event(), threading.Event()

    def hold_operator_frame(request: dict[str, Any]) -> None:
        if _rows(request) == {"door_open": True}:
            in_flight.set()
            assert release.wait(5.0)

    host.during = hold_operator_frame
    operator = threading.Thread(target=lambda: store.write(
        "door_open", True,
        metadata={"program_run_id": "run-1", "request_id": "r-1",
                  "value_source": "runtime_control"},
    ))
    operator.start()
    assert in_flight.wait(5.0)
    tree = threading.Thread(target=lambda: store.write("door_open", False))
    tree.start()
    tree.join(0.2)
    assert tree.is_alive()  # The tree's write waits for the operator's ACK.
    release.set()
    operator.join(5.0)
    tree.join(5.0)
    assert [_rows(frame) for frame in host.frames[1:]] == [
        {"door_open": True}, {"door_open": False},
    ]
    assert publisher.fault is None


def test_unchanged_and_unsubscribed_writes_send_nothing() -> None:
    store = _store()
    host = _Host()
    publisher = _publisher(host, _Queue())
    publisher.start(store, ("count", "door_open"))
    store.write("count", 0)
    store.write("speed", 2.0)
    assert [frame["kind"] for frame in host.frames] == ["baseline"]


def test_baseline_capture_is_atomic_with_concurrent_writes() -> None:
    store = _store()
    host = _Host()
    publisher = _publisher(host, _inline)
    started = threading.Event()

    def writer() -> None:
        for value in range(1, 501):
            store.write("count", value)
            if value == 50:
                started.set()

    thread = threading.Thread(target=writer)
    thread.start()
    started.wait(timeout=5)
    publisher.start(store, ("count",))
    thread.join(timeout=10)

    published = [int(frame["values"][0]["value"]) for frame in host.frames]
    assert host.frames[0]["kind"] == "baseline"
    # Every change after the baseline arrives once, in order, ending last.
    assert published == list(range(published[0], 501))
    assert publisher.fault is None


def test_a_failed_forward_is_source_loss_before_the_write_returns(
    tmp_path: Path,
) -> None:
    # BBK-1 (a): after the source exists, a refused or unanswered forward
    # makes the Kernel's source unavailable (exact-sender gap) before the
    # write returns, so its rules can't watch and hold their scope; then the
    # Run's Program Stop, then the fault fences liveness. No resend.
    receiver, _rclock, _losses, _reconciliations = _receiver(tmp_path)
    receiver._handle(_heartbeat())  # noqa: SLF001
    events: list[Any] = []
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
    )
    publisher.start(store, _KEYS)
    host.fail_with = HeadlessApiError("execution_owner_unavailable", "lost")
    store.write("count", 1)
    # The write has returned: the loss frame was already sent.
    assert [(f["message_sequence"], f["previous_sequence"]) for f in host.frames] == [
        ("1", "0"), ("2", "1"), (_LOSS_SEQUENCE, str(2**64 - 2)),
    ]
    assert publisher.fault is not None and not publisher.loss_confirmed
    store.write("count", 2)
    assert len(host.frames) == 3  # Nothing is sent after the fault.
    queue.run()  # Containment, off the store lock.
    assert events == [("stop", "run-1"), ("fault", FAULT_CODE)]
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


def test_a_sequence_gap_reply_confirms_the_loss() -> None:
    store = _store()
    host = _Host()
    publisher = _publisher(host, _inline)
    publisher.start(store, ("count",))

    def refuse_then_confirm(request: dict[str, Any]) -> None:
        host.fail_with = (
            _SEQUENCE_GAP if request["message_sequence"] == _LOSS_SEQUENCE else _SCOPE_STALE
        )

    host.during = refuse_then_confirm
    store.write("count", 1)
    assert publisher.fault is not None and publisher.loss_confirmed


def test_writes_before_the_source_exists_are_carried_by_its_baseline() -> None:
    # BBK-1 (a): while the Kernel answers the baseline `bb_source_not_ready`
    # (Apply or Reset still installing), a write only commits; the owner's
    # next start sends a baseline built from the store then. No fault.
    events: list[Any] = []
    store = _store()
    host = _Host(fail_with=HeadlessApiError("bb_source_not_ready", "Apply installing"))
    publisher = _publisher(host, _inline, events=events, between_runs=True)
    assert publisher.start(store, ("count", "door_open")) is False
    store.write("count", 4)
    store.write("door_open", True)
    assert len(host.frames) == 1 and events == [] and publisher.fault is None
    host.fail_with = None
    assert publisher.start(store, ("count", "door_open")) is True
    baseline = host.frames[1]
    assert (baseline["kind"], baseline["message_sequence"]) == ("baseline", "1")
    assert _rows(baseline) == {"count": "4", "door_open": True}
    store.write("count", 5)
    assert (host.frames[2]["message_sequence"], _rows(host.frames[2])) == ("2", {"count": "5"})
    assert events == [] and publisher.baseline_accepted


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


def test_wrong_reply_kind_is_a_fault() -> None:
    events: list[Any] = []
    store = _store()
    host = _Host()

    def wrong(request: dict[str, Any]) -> dict[str, Any]:
        return {**host(request), "command": "events"}

    publisher = _publisher(wrong, _inline, events=events)  # type: ignore[arg-type]
    publisher.start(store, _KEYS)
    assert [event[0] for event in events] == ["stop", "fault"]


def test_ack_echo_is_not_compared() -> None:
    """Each exchange is its own connection: the echoed sequence is not compared."""

    events: list[Any] = []
    store = _store()
    host = _Host()

    def echo(request: dict[str, Any]) -> dict[str, Any]:
        return {**host(request), "accepted_sequence": "9"}

    publisher = _publisher(echo, _inline, events=events)  # type: ignore[arg-type]
    publisher.start(store, _KEYS)
    store.write("count", 1)
    assert events == [] and publisher.fault is None


def _store(program: Program | None = None) -> ProgramValueStore:
    return ProgramValueStore(
        program or _program(),
        persistent_commit=lambda _declaration, _value, _metadata: None,
    )


def test_reset_retires_the_run_source_locally_without_a_clearing_frame() -> None:
    events: list[Any] = []
    store = _store()
    host = _Host()
    publisher = _publisher(host, _inline, events=events)
    publisher.start(store, ("count", "label", "total"))
    store.write("label", "held")
    # Held/finished: the values stay and nothing is retired.
    store.write("total", 9)
    publisher.retire()  # The Runner's own Reset transition (DX-1281).
    store.clear_run_values()
    assert [frame["message_sequence"] for frame in host.frames] == ["1", "2", "3"]
    assert events == []


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
    host = _Host()
    publisher = _publisher(host, _inline, events=events)
    publisher.start(store, ("count",))

    def retire_while_in_flight(request: dict[str, Any]) -> dict[str, Any]:
        host(request)
        publisher.retire()  # Reset won while this frame was in flight.
        if reply == "superseded":
            return _superseded(request)
        raise reply

    publisher._publish = retire_while_in_flight  # noqa: SLF001
    store.write("count", 1)
    assert events == [] and publisher.fault is None


def test_unknown_or_stale_reply_for_the_current_source_still_latches() -> None:
    events: list[Any] = []
    store = _store()
    host = _Host(fail_with=HeadlessApiError(
        "stale_generation", "stale", details={"reason": "program_bb_scope_stale"}
    ))
    publisher = _publisher(host, _inline, events=events)
    publisher.start(store, ("count",))
    assert [event[0] for event in events] == ["stop", "fault"]
    # The owner's typed code and sub-reason stay visible in the fault.
    assert "code=stale_generation reason=program_bb_scope_stale" in events[1][1]["message"]


def test_source_not_ready_after_the_baseline_is_source_loss() -> None:
    # BBK-1 (a): only the baseline may wait for the Kernel's source; an
    # update refused not-ready is lost like any other refusal (no retry).
    events: list[Any] = []
    store = _store()
    host = _Host()
    publisher = _publisher(host, _inline, events=events)
    publisher.start(store, ("count",))
    host.fail_with = HeadlessApiError("bb_source_not_ready", "transition")
    store.write("count", 4)
    assert [event[0] for event in events] == ["stop", "fault"]
    assert host.frames[-1]["message_sequence"] == _LOSS_SEQUENCE


def test_computed_key_is_published_only_when_its_value_changes() -> None:
    pytest.importorskip("manyforge_headless_py")
    store = _store(_program(_key("double", "int64", access="read", expression="key.count * 2")))
    host = _Host()
    publisher = _publisher(host, _inline)
    publisher.start(store, ("door_open", "double"))
    store.write("count", 3)
    store.write("speed", 1.0)
    store.write("count", 3)
    assert [_rows(frame) for frame in host.frames] == [
        {"door_open": False, "double": "0"},
        {"double": "6"},
    ]


def test_next_run_or_shutdown_retires_the_sender() -> None:
    events: list[Any] = []
    store = _store()
    host = _Host()
    publisher = _publisher(host, _inline, events=events)
    publisher.start(store, _KEYS)
    publisher.retire()
    store.write("count", 4)
    assert [frame["kind"] for frame in host.frames] == ["baseline"] and events == []


def test_subscription_is_fixed_once_and_scalar_only() -> None:
    store = _store()
    publisher = _publisher(_Host(), _inline)
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
    transport._exchange = lambda command: {**superseded, "accepted_sequence": "2"}  # type: ignore[method-assign]  # noqa: SLF001
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
    reply["program_run_id"] = None
    assert transport.publish_program_bbkeys(between_runs)["program_run_id"] is None
    with pytest.raises(HeadlessApiError):  # Claim and Run go together.
        transport.publish_program_bbkeys({**frame, "claim_generation": None})
    transport._exchange = lambda command: {**reply, "command": "events"}  # type: ignore[method-assign]  # noqa: SLF001
    with pytest.raises(HeadlessApiError) as invalid:  # Wrong reply kind.
        transport.publish_program_bbkeys(frame)
    assert invalid.value.code == "execution_command_response_invalid"


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


def test_superseded_reply_retires_the_source_without_a_fault() -> None:
    events: list[Any] = []
    store = _store()
    host = _Host()
    publisher = _publisher(host, _inline, events=events)
    publisher.start(store, ("count", "label"))
    publisher._publish = lambda request: host(request) and _superseded(request)  # noqa: SLF001
    store.write("label", "held")
    store.write("count", 2)  # The occurrence was replaced: nothing more is sent.
    assert [frame["message_sequence"] for frame in host.frames] == ["1", "2"]
    assert events == []  # No Program Stop, no publisher fault.
    assert publisher.fault is None


def test_between_run_baseline_start_failure_keeps_the_source_without_fence(
    tmp_path: Path,
) -> None:
    # F-2: a between-Runs start failure changes no source and never fences.
    receiver, _clock, _losses, _reconciliations = _receiver(tmp_path)
    receiver._handle(_heartbeat())  # noqa: SLF001
    runtime = object.__new__(ProgramRuntime)
    runtime._liveness = receiver  # noqa: SLF001
    runtime._bbkey_publishers = {}  # noqa: SLF001
    runtime._bbkey_idle_store = None  # noqa: SLF001
    runtime._bbkey_publishers_lock = threading.RLock()  # noqa: SLF001
    runtime._bbkey_source_lock = threading.RLock()  # noqa: SLF001
    runtime._interrupt_state_mirror = None  # noqa: SLF001
    previous = _publisher(_Host(), _inline, between_runs=True)
    runtime._bbkey_publishers[None] = previous  # noqa: SLF001

    def fail_start(*, keys: tuple[str, ...]) -> None:
        raise RuntimeError("Applied Program unreadable")

    runtime._start_idle_bbkey_publication = fail_start  # type: ignore[method-assign]  # noqa: SLF001
    runtime._cell_bbkey_union = lambda: ()  # type: ignore[method-assign]  # noqa: SLF001
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    assert receiver._handle(_heartbeat(sequence=2))["publisher_fault"] is None  # noqa: SLF001
    assert runtime._bbkey_publishers[None] is previous  # noqa: SLF001
    runtime._cell_bbkey_union = lambda: ("count",)  # type: ignore[method-assign]  # noqa: SLF001
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    acknowledgement = receiver._handle(_heartbeat(sequence=3))  # noqa: SLF001
    assert acknowledgement["publisher_fault"] is None
    assert acknowledgement["command_fenced"] is False
    assert runtime._bbkey_publishers[None] is previous  # noqa: SLF001
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
            "program_binding": {"target_role_bindings": [{
                "controllable_component_id": "robot", "target_role_id": "arm", "robot_tcps": [],
            }]},
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
    runtime._active_tcp_stores_lock = threading.Lock()  # noqa: SLF001
    runtime._active_tcp_stores = {}  # noqa: SLF001
    runtime._active_tcp_mirror = ActiveTcpMirror(on_change=runtime._write_active_tcp)  # noqa: SLF001
    runtime._bbkey_source_lock = threading.RLock()  # noqa: SLF001
    runtime._bbkey_fence_lock = threading.Lock()  # noqa: SLF001
    runtime._bbkey_open_losses = {}  # noqa: SLF001
    runtime._bbkey_fence_reference = None  # noqa: SLF001
    runtime._bbkey_fence_permanent = False  # noqa: SLF001
    runtime._bbkey_source_ordinals = itertools.count(1)  # noqa: SLF001
    runtime._interrupt_state_mirror = None  # noqa: SLF001
    runtime._interrupt_state_run_id = None  # noqa: SLF001
    runtime._grasp_lost_mirror_lock = threading.Lock()  # noqa: SLF001
    runtime._carried_object_lost_run_id = None  # noqa: SLF001
    runtime._carried_object_lost_run_active = False  # noqa: SLF001
    runtime._carried_object_lost_seen_event_ids = set()  # noqa: SLF001
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
    )


def test_every_new_applied_publication_is_a_new_between_run_source() -> None:
    applied, journal = _Applied(), _Journal()
    runtime, hosts = _runtime(applied, journal)
    runtime._applied_condition_keys = ("door", "total")  # noqa: SLF001
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    (first,) = hosts
    assert first.frames[0]["active_publication_generation"] == 5
    assert {row["name"]: row["value"] for row in first.frames[0]["values"]} == {
        # Run-scoped key: its declared initial between Runs (D-M4-2).
        "door": False, "total": "4",
    }
    runtime._follow_cell_bbkey_source()  # The same publication notice: no-op.
    assert len(hosts) == 1
    applied.generation = 6  # A new Apply of the same binding and cycle.
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    assert len(hosts) == 2 and hosts[1].frames[0]["active_publication_generation"] == 6
    assert runtime._bbkey_publishers[None].scope.active_publication_generation == 6  # noqa: SLF001


def test_w0_keeps_an_idle_active_tcp_store_without_a_bb_publisher() -> None:
    applied, journal = _Applied(), _Journal()
    runtime, hosts = _runtime(applied, journal)
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    assert hosts == []
    assert None in runtime._active_tcp_stores  # noqa: SLF001
    store, roles = runtime._active_tcp_stores[None]  # noqa: SLF001
    assert roles == {"robot": ("arm",)}
    assert "active_tcp.arm" in store.program.blackboard_keys


def test_unchanged_identity_keeps_its_source_and_never_resends_its_baseline() -> None:
    # F-2 root: a repeated notice whose catalog read transiently proves no
    # key (W0) must not retire a source of an unchanged identity; otherwise
    # the next notice sends the same scope's baseline again.
    applied, journal = _Applied(), _Journal()
    runtime, hosts = _runtime(applied, journal)
    runtime._applied_condition_keys = ("door",)  # noqa: SLF001
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    (first,) = hosts
    source = runtime._bbkey_publishers[None]  # noqa: SLF001
    runtime._applied_condition_keys = ()  # W0: no Program key, no rule key.  # noqa: SLF001
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    runtime._applied_condition_keys = ("door",)  # noqa: SLF001
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    assert hosts == [first] and runtime._bbkey_publishers[None] is source  # noqa: SLF001
    assert [frame["message_sequence"] for frame in first.frames] == ["1"]
    journal.cycle_generation = 8  # A Reset: a new identity, a new source.
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    assert len(hosts) == 2 and hosts[1].frames[0]["cycle_generation"] == 8


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


# F-2: a refused between-Runs publication never fences the Runner -----------


_SEQUENCE_STALE = HeadlessApiError(
    "stale_generation",
    "I/O signal identity is no longer current",
    details={"reason": "program_bb_sequence_stale"},
)


def _fencing_runtime(
    tmp_path: Path, host: _Host
) -> tuple[ProgramRuntime, _Applied, _Journal, Any, list[str]]:
    """The real publisher wiring (`_new_bbkey_publisher`) on a live receiver."""

    receiver, _clock, _losses, _reconciliations = _receiver(tmp_path)
    receiver._handle(_heartbeat())  # noqa: SLF001
    applied, journal = _Applied(), _Journal()
    runtime, _hosts = _runtime(applied, journal)
    del runtime._new_bbkey_publisher  # noqa: SLF001
    stops: list[str] = []
    runtime._liveness = receiver  # noqa: SLF001
    runtime._bbkey_pending = {}  # noqa: SLF001
    runtime.config = type("Config", (), {"headless_timeout_seconds": 30.0})()  # type: ignore[assignment]
    runtime._stop_for_bbkey_publication_fault = stops.append  # type: ignore[method-assign]  # noqa: SLF001
    runtime._execution_transport = type(  # type: ignore[assignment]  # noqa: SLF001
        "Transport", (), {"publish_program_bbkeys": staticmethod(host)}
    )()
    return runtime, applied, journal, receiver, stops


_SEQUENCE_GAP = HeadlessApiError(
    "stale_generation",
    "I/O signal identity is no longer current",
    details={"reason": "program_bb_sequence_gap"},
)
_SCOPE_STALE = HeadlessApiError(
    "stale_generation",
    "I/O signal identity is no longer current",
    details={"reason": "program_bb_scope_stale"},
)


class _ScriptedHost(_Host):
    """Answers each frame with the next scripted error, then accepts."""

    def __init__(self, *errors: Exception) -> None:
        super().__init__()
        self.errors = list(errors)

    def __call__(self, request: dict[str, Any]) -> dict[str, Any]:
        self.fail_with = self.errors.pop(0) if self.errors else None
        return super().__call__(request)


def test_refused_between_run_baseline_is_declared_lost_without_fence(
    tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    # F-2 / SX-527: the refused sender is dropped and its Kernel source is
    # made unavailable (exact-sender gap), so BB-backed gates fail closed;
    # the Runner itself is not fenced.
    host = _ScriptedHost(_SEQUENCE_STALE, _SEQUENCE_GAP)
    runtime, _applied, journal, receiver, stops = _fencing_runtime(tmp_path, host)
    runtime._applied_condition_keys = ("door",)  # noqa: SLF001
    with caplog.at_level("WARNING"):
        runtime._follow_cell_bbkey_source()  # noqa: SLF001
    baseline, loss = host.frames
    assert baseline["message_sequence"] == "1"
    assert (loss["kind"], loss["message_sequence"], loss["previous_sequence"]) == (
        "update", str(2**64 - 1), str(2**64 - 2),
    )
    assert {key: loss[key] for key in ("program_run_id", "cycle_generation")} == {
        "program_run_id": None, "cycle_generation": 7,
    }
    assert loss["program_binding_id"] == baseline["program_binding_id"]
    assert loss["active_publication_generation"] == baseline["active_publication_generation"]
    assert None not in runtime._bbkey_publishers  # noqa: SLF001
    assert runtime._bbkey_idle_store is None  # No edit lands in a dead store.  # noqa: SLF001
    assert stops == []
    acknowledgement = receiver._handle(_heartbeat(sequence=2))  # noqa: SLF001
    assert acknowledgement["publisher_fault"] is None
    assert acknowledgement["command_fenced"] is False
    receiver.require_current()
    senders = [r.getMessage() for r in caplog.records if "BB baseline sender" in r.getMessage()]
    assert len(senders) == 1 and "run=None" in senders[0] and "cycle=7" in senders[0]
    errors = [r.getMessage() for r in caplog.records if r.levelname == "ERROR"]
    assert len(errors) == 1 and "program_bb_sequence_stale" in errors[0]
    assert any("declared lost" in r.getMessage() for r in caplog.records)

    # SX-536: a repeated notice of the same scope is not a recovery; it is
    # refused and declared lost again, still without a fence.
    host.errors = [_SEQUENCE_STALE, _SEQUENCE_GAP]
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    assert None not in runtime._bbkey_publishers  # noqa: SLF001
    assert receiver._handle(_heartbeat(sequence=3))["publisher_fault"] is None  # noqa: SLF001

    # The next Reset (a new Idle cycle) establishes a fresh, accepted source.
    journal.cycle_generation = 8
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    fresh = runtime._bbkey_publishers[None]  # noqa: SLF001
    assert fresh.scope.cycle_generation == 8 and fresh.fault is None
    assert fresh.baseline_accepted
    assert host.frames[-1]["cycle_generation"] == 8
    assert host.frames[-1]["message_sequence"] == "1"
    assert receiver._handle(_heartbeat(sequence=4))["publisher_fault"] is None  # noqa: SLF001


def test_between_run_loss_not_confirmed_by_the_kernel_fences_as_before(
    tmp_path: Path,
) -> None:
    # Fail closed: without the Kernel's typed loss the source may still be
    # usable, so the Runner fences itself as before F-2.
    host = _ScriptedHost(_SEQUENCE_STALE, _SCOPE_STALE)
    runtime, _applied, _journal, receiver, stops = _fencing_runtime(tmp_path, host)
    runtime._applied_condition_keys = ("door",)  # noqa: SLF001
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    assert len(host.frames) == 2 and stops == []
    acknowledgement = receiver._handle(_heartbeat(sequence=2))  # noqa: SLF001
    assert acknowledgement["publisher_fault"]["code"] == FAULT_CODE
    assert "program_bb_sequence_stale" in acknowledgement["publisher_fault"]["message"]
    assert acknowledgement["command_fenced"] is True
    with pytest.raises(ProgramRunnerLivenessError, match="publication fault"):
        receiver.require_current()


class _QueuedPool:
    """Run work only when drained (never inline), like the Runner pool."""

    def __init__(self) -> None:
        self.queue = _Queue()

    def submit_run_work(self, work: Callable[[], None]) -> None:
        self.queue(work)


def _queued_fencing_runtime(
    tmp_path: Path, host: _Host
) -> tuple[ProgramRuntime, _Journal, Any, _Queue]:
    runtime, _applied, journal, receiver, _stops = _fencing_runtime(tmp_path, host)
    pool = _QueuedPool()
    runtime._host = pool  # type: ignore[assignment]  # noqa: SLF001
    runtime._applied_condition_keys = ("door",)  # noqa: SLF001
    return runtime, journal, receiver, pool.queue


def test_between_run_fault_is_lost_in_the_kernel_before_any_containment(
    tmp_path: Path,
) -> None:
    # SX-543 (1) / BBK-1 (a): the refused sender declares its loss in the
    # Kernel before its containment runs (queued, not inline), so nothing
    # gated by the previous value can be admitted meanwhile.
    host = _ScriptedHost(_SEQUENCE_STALE, _SEQUENCE_GAP)
    runtime, _journal, receiver, queue = _queued_fencing_runtime(tmp_path, host)
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    assert [frame["message_sequence"] for frame in host.frames] == ["1", _LOSS_SEQUENCE]
    assert runtime._bbkey_publishers[None].loss_confirmed  # noqa: SLF001
    assert len(queue.work) == 1  # Containment, off the store lock.
    queue.run()
    released = receiver._handle(_heartbeat(sequence=2))  # noqa: SLF001
    assert released["publisher_fault"] is None and released["command_fenced"] is False
    receiver.require_current()


def test_late_reply_for_a_superseded_between_run_source_releases_the_fence(
    tmp_path: Path,
) -> None:
    # SX-543 (2) / SX-547 (1): the old loss is unconfirmed (scope stale); the
    # Kernel's ACK of the newer baseline releases the fence.
    host = _ScriptedHost(_SEQUENCE_STALE, _SCOPE_STALE)
    runtime, journal, receiver, queue = _queued_fencing_runtime(tmp_path, host)
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    journal.cycle_generation = 8  # Reset: a new Idle cycle.
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    newer = runtime._bbkey_publishers[None]  # noqa: SLF001
    assert newer.scope.cycle_generation == 8 and newer.baseline_accepted
    queue.work.pop(0)()  # The old containment: the fence latches.
    held = receiver._handle(_heartbeat(sequence=2))  # noqa: SLF001
    assert held["publisher_fault"]["code"] == FAULT_CODE and held["command_fenced"] is True
    queue.run()  # The newer baseline's ACK settles the old loss.
    assert [(f["cycle_generation"], f["message_sequence"]) for f in host.frames] == [
        (7, "1"), (7, _LOSS_SEQUENCE), (8, "1"),
    ]
    assert newer.fault is None
    released = receiver._handle(_heartbeat(sequence=3))  # noqa: SLF001
    assert released["publisher_fault"] is None and released["command_fenced"] is False
    receiver.require_current()


def test_unacked_successor_does_not_release_the_fence(tmp_path: Path) -> None:
    # SX-547 (1): a successor whose baseline is refused too (and an old gap
    # reply that never confirms) is no proof: fence kept.
    host = _ScriptedHost(_SEQUENCE_STALE, _SCOPE_STALE, _SEQUENCE_STALE, _SCOPE_STALE)
    runtime, journal, receiver, queue = _queued_fencing_runtime(tmp_path, host)
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    journal.cycle_generation = 8
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    successor = runtime._bbkey_publishers[None]  # noqa: SLF001
    assert not successor.baseline_accepted
    queue.work.pop(0)()  # The old containment: fenced.
    with pytest.raises(ProgramRunnerLivenessError, match="publication fault"):
        receiver.require_current()
    queue.run()
    assert not successor.baseline_accepted
    held = receiver._handle(_heartbeat(sequence=2))  # noqa: SLF001
    assert held["publisher_fault"]["code"] == FAULT_CODE and held["command_fenced"] is True
    assert len(runtime._bbkey_open_losses) == 2  # noqa: SLF001


def test_fence_waits_for_every_unconfirmed_loss(tmp_path: Path) -> None:
    # SX-547 (2): A and B both fault; A's confirmed loss alone does not lift
    # the fence while B's loss is unconfirmed.
    host = _ScriptedHost(_SEQUENCE_STALE, _SEQUENCE_GAP, _SEQUENCE_STALE, _SCOPE_STALE)
    runtime, journal, receiver, queue = _queued_fencing_runtime(tmp_path, host)
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    journal.cycle_generation = 8
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    assert len(queue.work) == 2
    queue.run()
    assert [frame["cycle_generation"] for frame in host.frames] == [7, 7, 8, 8]
    held = receiver._handle(_heartbeat(sequence=2))  # noqa: SLF001
    assert held["publisher_fault"]["code"] == FAULT_CODE and held["command_fenced"] is True
    assert len(runtime._bbkey_open_losses) == 1  # noqa: SLF001
    with pytest.raises(ProgramRunnerLivenessError, match="publication fault"):
        receiver.require_current()


def test_late_ack_of_a_replaced_sender_leaves_the_newer_loss_open(tmp_path: Path) -> None:
    # SX-554: A's baseline ACK is delayed; meanwhile a Reset installs B, whose
    # baseline is refused. A's late ACK callback must not settle B's loss.
    frames: list[dict[str, Any]] = []
    holder: dict[str, Any] = {}

    def host(request: dict[str, Any]) -> dict[str, Any]:
        frames.append(deepcopy(dict(request)))
        if request["cycle_generation"] == 8:
            raise _SEQUENCE_STALE if request["message_sequence"] == "1" else _SCOPE_STALE
        # A's baseline is in flight: Reset installs B and B faults first.
        runtime, journal, queue = holder["runtime"], holder["journal"], holder["queue"]
        journal.cycle_generation = 8
        runtime._follow_cell_bbkey_source()  # noqa: SLF001
        queue.work.pop(0)()  # B's baseline: refused; B's containment queued.
        return _Host()(request)  # Only now is A's baseline ACKed.

    runtime, journal, receiver, queue = _queued_fencing_runtime(tmp_path, host)  # type: ignore[arg-type]
    holder.update(runtime=runtime, journal=journal, queue=queue)
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    queue.work.pop(0)()  # B's queued loss settlement; A's ACK callback follows.
    assert [(f["cycle_generation"], f["message_sequence"]) for f in frames] == [
        (7, "1"), (8, "1"), (8, _LOSS_SEQUENCE),
    ]
    assert len(runtime._bbkey_open_losses) == 1  # B's loss stays open.  # noqa: SLF001
    held = receiver._handle(_heartbeat(sequence=2))  # noqa: SLF001
    assert held["publisher_fault"]["code"] == FAULT_CODE and held["command_fenced"] is True
    queue.run()  # B's own gap reply is unavailable: the fence stays.
    with pytest.raises(ProgramRunnerLivenessError, match="publication fault"):
        receiver.require_current()


def test_concurrent_followers_of_one_scope_create_one_sender() -> None:
    # SX-529: A and B both find no current source before either installs;
    # serialized creation leaves one sender and one baseline.
    applied, journal = _Applied(), _Journal()
    runtime, hosts = _runtime(applied, journal)
    runtime._applied_condition_keys = ("door",)  # noqa: SLF001
    create = runtime._new_bbkey_publisher  # noqa: SLF001
    first_inside = threading.Event()
    release_first = threading.Event()
    calls: list[str] = []

    def held_create(scope: ProgramBlackboardPublicationScope) -> ProgramBlackboardKeyPublisher:
        calls.append(threading.current_thread().name)
        if len(calls) == 1:
            first_inside.set()
            assert release_first.wait(5.0)
        return create(scope)

    runtime._new_bbkey_publisher = held_create  # type: ignore[method-assign]  # noqa: SLF001
    first = threading.Thread(target=runtime._follow_cell_bbkey_source, name="A")  # noqa: SLF001
    second = threading.Thread(target=runtime._follow_cell_bbkey_source, name="B")  # noqa: SLF001
    first.start()
    assert first_inside.wait(5.0)
    second.start()
    second.join(0.2)  # B now waits for A's creation, before its own check.
    assert second.is_alive() and calls == ["A"]
    release_first.set()
    first.join(5.0)
    second.join(5.0)
    assert not first.is_alive() and not second.is_alive()
    assert calls == ["A"]
    (host,) = hosts
    assert [frame["message_sequence"] for frame in host.frames] == ["1"]
    assert runtime._bbkey_publishers[None].fault is None  # noqa: SLF001


def test_refused_run_publication_still_stops_the_run_and_fences(tmp_path: Path) -> None:
    host = _Host(fail_with=_SEQUENCE_STALE)
    runtime, _applied, _journal, receiver, stops = _fencing_runtime(tmp_path, host)
    publisher = runtime._new_bbkey_publisher(  # noqa: SLF001
        ProgramBlackboardPublicationScope(
            "kernel-owner", "runner-owner", 3, "run-1", "sha256:" + "d" * 64, 5, 7
        )
    )
    runtime._bbkey_publishers["run-1"] = publisher  # noqa: SLF001
    publisher.start(_store(), ("count",))
    assert stops == ["run-1"]
    assert publisher.fault is not None
    acknowledgement = receiver._handle(_heartbeat(sequence=2))  # noqa: SLF001
    assert acknowledgement["publisher_fault"]["code"] == FAULT_CODE
    assert "program_bb_sequence_stale" in acknowledgement["publisher_fault"]["message"]
    assert acknowledgement["command_fenced"] is True
    with pytest.raises(ProgramRunnerLivenessError, match="publication fault"):
        receiver.require_current()


def test_a_source_not_ready_starts_later_from_the_values_committed_meanwhile(
    tmp_path: Path,
) -> None:
    # BBK-1 (a): the Kernel has not installed the source yet (Reset just
    # journaled): writes only commit, and the next liveness observation sends
    # the baseline built from the store then. No fault, no fence.
    host = _ScriptedHost(HeadlessApiError("bb_source_not_ready", "cycle not observed"))
    runtime, _applied, _journal, receiver, stops = _fencing_runtime(tmp_path, host)
    runtime._applied_condition_keys = ("door",)  # noqa: SLF001
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    assert None in runtime._bbkey_pending  # noqa: SLF001
    runtime._bbkey_idle_store.write("door", True)  # noqa: SLF001
    assert len(host.frames) == 1
    runtime._retry_pending_bbkey_sources()  # The liveness observation.  # noqa: SLF001
    assert [(f["kind"], f["message_sequence"]) for f in host.frames] == [
        ("baseline", "1"), ("baseline", "1"),
    ]
    assert _rows(host.frames[1]) == {"door": True}
    assert runtime._bbkey_pending == {} and stops == []  # noqa: SLF001
    assert runtime._bbkey_publishers[None].baseline_accepted  # noqa: SLF001
    assert receiver._handle(_heartbeat(sequence=2))["publisher_fault"] is None  # noqa: SLF001
