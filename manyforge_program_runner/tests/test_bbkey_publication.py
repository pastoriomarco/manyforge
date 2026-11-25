"""Change-only BB key publication per captured Run."""

from __future__ import annotations

from collections.abc import Callable
from copy import deepcopy
from pathlib import Path
import itertools
import logging
import threading
from types import SimpleNamespace
from typing import Any

import py_trees
import pytest

from manyforge_behavior.canonical_artifact import compute_content_identity
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.nodes.v2_owner import OwnerInvocationAdapter
from manyforge_behavior.nodes.v2_pure import LogicSetValue
from manyforge_behavior.orchestration_ports import (
    NodeAttemptIdentity,
    OwnerInvocationResult,
)
from manyforge_behavior.orchestration_values import (
    NodeAttemptInputResolver,
    ProgramValueError,
    ProgramValueStore,
)
from manyforge_behavior.program import Program
from manyforge_behavior.program_scheduling import (
    PROGRAM_TICK_WORKER_FAILED_CODE,
    ProgramSchedulingError,
)
from manyforge_behavior.kernel_client import ExecutionCommandError, KernelClient
from manyforge_behavior.run_sender import RunSender
from manyforge_headless_api.client import HeadlessClientError
from manyforge_headless_api.native import HeadlessApiError
from manyforge_program_runner.bbkey_publication import (
    FAULT_CODE,
    ProgramBlackboardKeyPublisher,
    ProgramBlackboardPublicationScope,
)
from manyforge_program_runner.active_tcp_mirror import ActiveTcpMirror
from manyforge_program_runner.liveness import (
    TICK_WORKER_FAULT_CODE,
    ProgramRunnerLivenessError,
)
from manyforge_program_runner.runtime import ProgramRunnerError, ProgramRuntime

from manyforge_program_runner.tests.fake_gateway import RecordingGateway
from manyforge_program_runner.tests.test_liveness import _heartbeat, _receiver

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
        api_version="manyforge.program.v4",
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
            "api_version": "manyforge.execution_command_response.v6",
            "command": "publish_program_bbkeys",
            "ok": True,
            "status": "accepted",
                "program_binding_id": request["program_binding_id"],
            "active_publication_generation": request["active_publication_generation"],
            "cycle_generation": request["cycle_generation"],
            "program_run_id": request["program_run_id"],
            "source_epoch": "source-1",
            "accepted_sequence": request["message_sequence"],
        }


def _inline_sender() -> RunSender:
    """A source sender that drains on the submitting thread (synchronous)."""

    return RunSender(1, spawn=lambda _name, work: work())


def _publisher(
    host: _Host,
    submit: Callable[[Callable[[], None]], None],
    *,
    events: list[Any] | None = None,
    between_runs: bool = False,
    source_sender: RunSender | None = None,
    run_sender: RunSender | None = None,
) -> ProgramBlackboardKeyPublisher:
    events = [] if events is None else events
    return ProgramBlackboardKeyPublisher(
        ProgramBlackboardPublicationScope(
            claim_generation=None if between_runs else 3,
            program_run_id=None if between_runs else 1,
            program_binding_id="sha256:" + "d" * 64,
            active_publication_generation=5,
            cycle_generation=7,
        ),
        publish=host,
        sender=run_sender or source_sender or _inline_sender(),
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


def test_baseline_then_each_write_forwarded_in_commit_order() -> None:
    # A sender that drains on the writing thread: one frame per write,
    # never merged, in commit order, sequence numbers without a gap.
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
    # An update carries only its written key.
    assert [_rows(frame) for frame in host.frames[1:]] == [
        {"count": "7"}, {"label": "café"}, {"count": "8"},
    ]
    assert {
        key: host.frames[0][key]
        for key in ("claim_generation", "kind", "active_publication_generation")
    } == {"claim_generation": 3, "kind": "baseline", "active_publication_generation": 5}
    assert all(len(frame) == 10 for frame in host.frames)
    assert publisher.fault is None


def test_a_pulse_reaches_the_kernel_as_two_frames() -> None:
    # true then false at once is two evaluations, never the latest.
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
    # every writer commits through the one store. An operator write
    # (runtime control) whose frame is still in flight never holds the
    # tree's write: it commits at once, and its frame follows in commit
    # order. The Kernel sees true, then false.
    store = _store()
    host = _Host()
    publisher = _publisher(
        host, _Queue(), source_sender=RunSender(1, name="test-bbkey-sender")
    )
    in_flight, release, done = threading.Event(), threading.Event(), threading.Event()

    def hold_operator_frame(request: dict[str, Any]) -> None:
        if _rows(request) == {"door_open": True}:
            in_flight.set()
            assert release.wait(5.0)
        if _rows(request) == {"door_open": False} and request["kind"] == "update":
            done.set()

    host.during = hold_operator_frame
    publisher.start(store, ("door_open",))
    store.write(
        "door_open", True,
        metadata={"program_run_id": 1, "request_key": "r-1",
                  "value_source": "runtime_control"},
    )
    assert in_flight.wait(5.0)
    tree = threading.Thread(target=lambda: store.write("door_open", False))
    tree.start()
    tree.join(5.0)
    assert not tree.is_alive()  # The tree's write never waits for a frame.
    assert store.read("door_open") is False
    release.set()
    assert done.wait(5.0)
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


def test_a_failed_forward_is_source_loss_sent_next_on_the_same_drain(
    tmp_path: Path,
) -> None:
    # after the source exists, a refused or unanswered forward
    # makes the Kernel's source unavailable (exact-sender gap) as the next
    # frame of the same drain, so its rules can't watch and hold their scope;
    # then the Run's Program Stop, then the fault fences liveness. No resend.
    receiver = _receiver(tmp_path)
    receiver._handle(_heartbeat())  # noqa: SLF001
    events: list[Any] = []
    store = _store()
    host, queue = _Host(), _Queue()
    publisher = ProgramBlackboardKeyPublisher(
        ProgramBlackboardPublicationScope(
            3, 1, "sha256:" + "d" * 64, 5, 7
        ),
        publish=host,
        sender=_inline_sender(),
        submit=queue,
        stop_run=lambda run_id: events.append(("stop", run_id)),
        on_fault=lambda fault, reference: (
            events.append(("fault", fault["code"])),
            receiver.latch_runner_fault(fault, reference),
        ),
        fault_reference=ProgramRuntime._digest,  # noqa: SLF001
    )
    publisher.start(store, _KEYS)
    host.fail_with = HeadlessApiError("execution_owner_unavailable", "lost")
    store.write("count", 1)
    # The inline drain has ended: the loss frame was sent right after.
    assert [(f["message_sequence"], f["previous_sequence"]) for f in host.frames] == [
        ("1", "0"), ("2", "1"), (_LOSS_SEQUENCE, str(2**64 - 2)),
    ]
    assert publisher.fault is not None and not publisher.loss_confirmed
    store.write("count", 2)
    assert len(host.frames) == 3  # Nothing is sent after the fault.
    queue.run()  # Containment, off the store lock.
    assert events == [("stop", 1), ("fault", FAULT_CODE)]
    acknowledgement = receiver._handle(_heartbeat(sequence=2))  # noqa: SLF001
    # Only the fields the Manager reads.
    assert set(acknowledgement) == {
        "api_version", "kind", "schema_version", "receiver_status_sequence",
        "state", "command_fenced", "fault_reference", "runner_fault",
        "program_cycle_status",
    }
    assert acknowledgement["runner_fault"]["code"] == FAULT_CODE
    assert acknowledgement["runner_fault"]["details"] == {}
    assert acknowledgement["state"] == "current"
    assert acknowledgement["command_fenced"] is True
    assert acknowledgement["fault_reference"] == ProgramRuntime._digest(  # noqa: SLF001
        acknowledgement["runner_fault"]
    )
    with pytest.raises(ProgramRunnerLivenessError, match="Runner fault: program_bbkey_publication_failed"):
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
    # while the Kernel answers the baseline `bb_source_not_ready`
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
    receiver = _receiver(tmp_path)
    receiver._handle(_heartbeat())  # noqa: SLF001
    probe = receiver._handle(  # noqa: SLF001
        {"api_version": _heartbeat()["api_version"], "cell_id": "cell", "kind": "probe"}
    )
    assert len(probe) == 8
    healthy = receiver._handle(_heartbeat(sequence=2))  # noqa: SLF001
    # The nine fields the Manager reads (it requires exactly these).
    assert healthy["runner_fault"] is None and len(healthy) == 9
    receiver.latch_runner_fault(
        {"code": FAULT_CODE, "message": "lost", "details": {}}, "sha256:" + "e" * 64
    )
    for sequence in range(3, 6):
        result = receiver._handle(_heartbeat(sequence=sequence))  # noqa: SLF001
        assert result["runner_fault"] == {
            "code": FAULT_CODE,
            "message": "lost",
            "details": {},
        }
        assert result["command_fenced"] is True
        assert result["fault_reference"] == "sha256:" + "e" * 64
    with pytest.raises(ProgramRunnerLivenessError):
        receiver.require_current()
    with pytest.raises(ProgramRunnerLivenessError):
        receiver.latch_runner_fault(
            {"code": "other", "message": "x", "details": {}}, "sha256:" + "e" * 64
        )


def test_a_tick_worker_fault_fences_with_its_code_and_never_clears(
    tmp_path: Path,
) -> None:
    # OD-267: the Runner fault's second code. It replaces a latched BB
    # publication fault, no clear releases it, and no BB fault replaces it.
    receiver = _receiver(tmp_path)
    receiver._handle(_heartbeat())  # noqa: SLF001
    bb_fault = {"code": FAULT_CODE, "message": "lost", "details": {}}
    bb_reference = ProgramRuntime._digest(bb_fault)  # noqa: SLF001
    tick_fault = {
        "code": TICK_WORKER_FAULT_CODE,
        "message": "RuntimeError: boom",
        "details": {},
    }
    tick_reference = ProgramRuntime._digest(tick_fault)  # noqa: SLF001
    assert receiver.latch_runner_fault(bb_fault, bb_reference) is True
    with pytest.raises(ProgramRunnerLivenessError) as fenced:
        receiver.require_current()
    assert fenced.value.code is None
    assert receiver.latch_runner_fault(tick_fault, tick_reference) is True
    assert receiver.clear_runner_fault(bb_reference) is False
    assert receiver.clear_runner_fault(tick_reference) is False
    assert receiver.latch_runner_fault(bb_fault, bb_reference) is False
    assert receiver.latch_runner_fault(tick_fault, tick_reference) is False
    acknowledgement = receiver._handle(_heartbeat(sequence=2))  # noqa: SLF001
    assert acknowledgement["runner_fault"] == tick_fault
    assert acknowledgement["fault_reference"] == tick_reference
    assert acknowledgement["command_fenced"] is True
    with pytest.raises(ProgramRunnerLivenessError) as fenced:
        receiver.require_current()
    assert fenced.value.code == TICK_WORKER_FAULT_CODE


def test_runtime_fences_a_failed_tick_worker_and_refuses_with_its_code(
    tmp_path: Path,
) -> None:
    # OD-267: the tick host's failure observer latches the Runner fault;
    # every later command refuses with the typed code, not a generic one.
    receiver = _receiver(tmp_path)
    receiver._handle(_heartbeat())  # noqa: SLF001
    runtime, _hosts = _runtime(_Applied(), _Journal())
    runtime._liveness = receiver  # noqa: SLF001
    runtime._fence_tick_worker_failure(RuntimeError("boom"))  # noqa: SLF001
    acknowledgement = receiver._handle(_heartbeat(sequence=2))  # noqa: SLF001
    fault = {
        "code": TICK_WORKER_FAULT_CODE,
        "message": "RuntimeError: boom",
        "details": {},
    }
    assert acknowledgement["runner_fault"] == fault
    assert acknowledgement["fault_reference"] == ProgramRuntime._digest(fault)  # noqa: SLF001
    assert acknowledgement["command_fenced"] is True
    with pytest.raises(ProgramRunnerError) as refused:
        runtime._require_manager_current()  # noqa: SLF001
    assert refused.value.code == TICK_WORKER_FAULT_CODE
    scheduling = ProgramSchedulingError(
        "Program tick worker failed", code=PROGRAM_TICK_WORKER_FAILED_CODE
    )
    assert ProgramRuntime._runner_error(scheduling).code == TICK_WORKER_FAULT_CODE  # noqa: SLF001
    assert (
        ProgramRuntime._runner_error(ProgramSchedulingError("full")).code  # noqa: SLF001
        == "program_run_scheduling_failed"
    )


def test_a_tick_worker_fault_names_the_last_frame_of_its_traceback(
    tmp_path: Path,
) -> None:
    # OD-382 F6: the latched fault keeps its message and names where the
    # error was raised (file:line function) in its details.
    receiver = _receiver(tmp_path)
    receiver._handle(_heartbeat())  # noqa: SLF001
    runtime, _hosts = _runtime(_Applied(), _Journal())
    runtime._liveness = receiver  # noqa: SLF001

    def failing_tick() -> None:
        raise RuntimeError("boom")

    try:
        failing_tick()
    except RuntimeError as error:
        runtime._fence_tick_worker_failure(error)  # noqa: SLF001
    fault = receiver._handle(_heartbeat(sequence=2))["runner_fault"]  # noqa: SLF001
    assert fault["message"] == "RuntimeError: boom"
    frame = fault["details"]["last_frame"]
    assert frame.startswith(__file__ + ":") and frame.endswith(" failing_tick")
    with pytest.raises(ProgramRunnerLivenessError):
        receiver.latch_runner_fault(
            {"code": FAULT_CODE, "message": "lost", "details": {"last_frame": "x:1 f"}},
            "sha256:" + "e" * 64,
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
    publisher.retire()  # The Runner's own Reset transition.
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
    # only the baseline may wait for the Kernel's source; an
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


def test_kernel_client_sends_the_frame_and_checks_the_ack_sequence() -> None:
    frame = {
        "schema_version": "manyforge.program_bbkey_publication.v2",
        "claim_generation": 3,
        "program_run_id": 1,
        "program_binding_id": "sha256:" + "d" * 64,
        "active_publication_generation": 5,
        "cycle_generation": 7,
        "kind": "update",
        "message_sequence": "2",
        "previous_sequence": "1",
        "values": [{"name": "count", "value_type": "int64", "value": "-4"}],
    }
    reply = {
        "api_version": "manyforge.execution_command_response.v6",
        "command": "publish_program_bbkeys",
        "ok": True,
        "status": "accepted",
        "program_binding_id": "sha256:" + "d" * 64,
        "active_publication_generation": 5,
        "cycle_generation": 7,
        "program_run_id": 1,
        "source_epoch": "source-1",
        "accepted_sequence": "2",
    }
    gateway = RecordingGateway({
        **{command: {"ok": True} for command in
           ("identity", "submit", "read", "reset", "acknowledge_completed", "stop")},
        "publish_program_bbkeys": reply,
    })
    client = KernelClient(gateway)

    def answer(value: Any) -> None:
        gateway.replies["publish_program_bbkeys"] = value

    assert client.publish_program_bbkeys(frame)["accepted_sequence"] == "2"
    # The frame as given; the core adds the envelope and the attribution.
    assert gateway.sent == [("publish_program_bbkeys", {"request": frame})]
    # Only a sequence matching the status is an ACK.
    for bad_reply in (
        {k: v for k, v in reply.items() if k != "status"},
        {**reply, "status": "queued"},
        {**reply, "accepted_sequence": None},
    ):
        answer(bad_reply)
        with pytest.raises(ExecutionCommandError) as invalid:
            client.publish_program_bbkeys(frame)
        assert invalid.value.code == "execution_command_response_invalid"
    # The owner's ErrorDTO keeps its code (a transient retry).
    answer({
        "api_version": "manyforge.execution_command_response.v6",
        "command": "publish_program_bbkeys",
        "ok": False,
        "error": {"code": "bb_source_not_ready", "message": "installing", "details": {}},
    })
    with pytest.raises(ExecutionCommandError) as refused:
        client.publish_program_bbkeys(frame)
    assert refused.value.code == "bb_source_not_ready"
    # Superseded names the installed replacement, never this frame.
    superseded = {
        **reply, "status": "superseded", "accepted_sequence": None,
        "cycle_generation": 8, "program_run_id": None, "source_epoch": "source-2",
    }
    answer(superseded)
    assert client.publish_program_bbkeys(frame)["status"] == "superseded"
    answer({**superseded, "accepted_sequence": "2"})
    with pytest.raises(ExecutionCommandError):
        client.publish_program_bbkeys(frame)


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
    """The actual reply: the installed replacement, no sequence."""

    return {
        "api_version": "manyforge.execution_command_response.v6",
        "command": "publish_program_bbkeys",
        "ok": True,
        "status": "superseded",
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
    # A between-Runs start failure changes no source and never fences.
    receiver = _receiver(tmp_path)
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
    assert receiver._handle(_heartbeat(sequence=2))["runner_fault"] is None  # noqa: SLF001
    assert runtime._bbkey_publishers[None] is previous  # noqa: SLF001
    runtime._cell_bbkey_union = lambda: ("count",)  # type: ignore[method-assign]  # noqa: SLF001
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    acknowledgement = receiver._handle(_heartbeat(sequence=3))  # noqa: SLF001
    assert acknowledgement["runner_fault"] is None
    assert acknowledgement["command_fenced"] is False
    assert runtime._bbkey_publishers[None] is previous  # noqa: SLF001
    receiver.require_current()


# Runtime wiring: between-Runs source identity -----------------------


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
        record_digest = "sha256:" + "a" * 64
        return {
            "program": deepcopy(self.document),
            "program_binding": {"target_bindings": [{
                "target": {"component_id": "robot", "command_target_id": "arm"}, "robot_tcps": [],
            }]},
            "program_binding_id": self.binding,
            "program_revision": self.document["program_revision"],
            "program_frame_facts": {
                "applied_record_digest": record_digest,
                "program_binding_id": self.binding,
                "program_revision": self.document["program_revision"],
                "retained_bundle_id": "bundle-1",
                "graph_revision": "graph-1",
                "components": [],
                "frames": [],
            },
            "source": {"record_digest": record_digest},
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
    runtime._bbkey_source_ordinals = itertools.count(1)  # noqa: SLF001
    runtime._bbkey_idle_installed = (0, 0)  # noqa: SLF001
    runtime._new_bbkey_source_sender = lambda _name: _inline_sender()  # type: ignore[method-assign]  # noqa: SLF001
    runtime._owner = SimpleNamespace(run_sender=lambda _run_id: None)  # type: ignore[assignment]  # noqa: SLF001
    runtime._interrupt_state_follower = None  # noqa: SLF001
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
    runtime.application_start_id = "runner-owner"

    def new_publisher(scope: ProgramBlackboardPublicationScope) -> ProgramBlackboardKeyPublisher:
        host = _Host()
        hosts.append(host)
        return _publisher_for(scope, host)

    runtime._new_bbkey_publisher = new_publisher  # type: ignore[method-assign]  # noqa: SLF001
    runtime._gateway = type(  # type: ignore[assignment]  # noqa: SLF001
        "Gateway", (), {"publish_program_bbkeys": lambda self, members: {}}
    )()
    return runtime, hosts


def _publisher_for(
    scope: ProgramBlackboardPublicationScope, host: _Host
) -> ProgramBlackboardKeyPublisher:
    return ProgramBlackboardKeyPublisher(
        scope,
        publish=host,
        sender=_inline_sender(),
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
        # Run-scoped key: its declared initial between Runs.
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
    assert "active_tcp/robot/arm" in store.program.blackboard_keys


def test_unchanged_identity_keeps_its_source_and_never_resends_its_baseline() -> None:
    # A repeated notice whose catalog read transiently proves no
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


def test_runner_start_before_any_scenario_logs_one_line_without_a_trace(
    caplog: pytest.LogCaptureFixture,
) -> None:
    # OD-305: no Scenario is loaded yet, an expected state: one line, no traceback.
    applied, journal = _Applied(), _Journal("idle_after_reset")

    def not_loaded() -> dict[str, Any]:
        raise HeadlessClientError(
            "program_applied_selection_not_found",
            "the active configuration was not installed from a Scenario",
        )

    applied.program_applied = not_loaded  # type: ignore[method-assign]
    runtime, hosts = _runtime(applied, journal)
    with caplog.at_level(logging.INFO):
        runtime._establish_startup_bbkey_source()  # noqa: SLF001
    assert hosts == []
    rows = [row for row in caplog.records if "between-Run BB key source" in row.getMessage()]
    assert len(rows) == 1 and rows[0].levelno == logging.INFO and rows[0].exc_info is None


def test_runtime_reset_retires_the_run_sender_without_a_frame() -> None:
    applied, journal = _Applied(), _Journal()
    runtime, _hosts = _runtime(applied, journal)
    host = _Host()
    publisher = _publisher_for(
        ProgramBlackboardPublicationScope(
            3, 1, "sha256:" + "d" * 64, 5, 7
        ),
        host,
    )
    store = _store()
    publisher.start(store, ("count",))
    runtime._bbkey_publishers[1] = publisher  # noqa: SLF001
    runtime._reset_bbkey_publication(1)  # noqa: SLF001
    store.write("count", 3)
    assert 1 not in runtime._bbkey_publishers  # noqa: SLF001
    assert [frame["kind"] for frame in host.frames] == ["baseline"]


# A refused between-Runs publication never fences the Runner -----------


_SEQUENCE_STALE = HeadlessApiError(
    "stale_generation",
    "I/O signal identity is no longer current",
    details={"reason": "program_bb_sequence_stale"},
)


def _fencing_runtime(
    tmp_path: Path, host: _Host
) -> tuple[ProgramRuntime, _Applied, _Journal, Any, list[str]]:
    """The real publisher wiring (`_new_bbkey_publisher`) on a live receiver."""

    receiver = _receiver(tmp_path)
    receiver._handle(_heartbeat())  # noqa: SLF001
    applied, journal = _Applied(), _Journal()
    runtime, _hosts = _runtime(applied, journal)
    del runtime._new_bbkey_publisher  # noqa: SLF001
    stops: list[str] = []
    runtime._liveness = receiver  # noqa: SLF001
    runtime._bbkey_pending = {}  # noqa: SLF001
    runtime.config = type("Config", (), {"headless_timeout_seconds": 30.0})()  # type: ignore[assignment]
    runtime._stop_for_bbkey_publication_fault = stops.append  # type: ignore[method-assign]  # noqa: SLF001
    # The Kernel client's publication (its reply check is the client's own).
    applied.publish_program_bbkeys = host  # type: ignore[attr-defined]
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
    # The refused sender is dropped and its Kernel source is
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
    assert acknowledgement["runner_fault"] is None
    assert acknowledgement["command_fenced"] is False
    receiver.require_current()
    senders = [r.getMessage() for r in caplog.records if "BB baseline sender" in r.getMessage()]
    assert len(senders) == 1 and "run=None" in senders[0] and "cycle=7" in senders[0]
    errors = [r.getMessage() for r in caplog.records if r.levelname == "ERROR"]
    assert len(errors) == 1 and "program_bb_sequence_stale" in errors[0]
    assert any("declared lost" in r.getMessage() for r in caplog.records)

    # A repeated notice of the same scope is not a recovery; it is
    # refused and declared lost again, still without a fence.
    host.errors = [_SEQUENCE_STALE, _SEQUENCE_GAP]
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    assert None not in runtime._bbkey_publishers  # noqa: SLF001
    assert receiver._handle(_heartbeat(sequence=3))["runner_fault"] is None  # noqa: SLF001

    # The next Reset (a new Idle cycle) establishes a fresh, accepted source.
    journal.cycle_generation = 8
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    fresh = runtime._bbkey_publishers[None]  # noqa: SLF001
    assert fresh.scope.cycle_generation == 8 and fresh.fault is None
    assert fresh.baseline_accepted
    assert host.frames[-1]["cycle_generation"] == 8
    assert host.frames[-1]["message_sequence"] == "1"
    assert receiver._handle(_heartbeat(sequence=4))["runner_fault"] is None  # noqa: SLF001


def test_between_run_loss_not_confirmed_by_the_kernel_fences_as_before(
    tmp_path: Path,
) -> None:
    # Fail closed: without the Kernel's typed loss the source may still be
    # usable, so the Runner fences itself.
    host = _ScriptedHost(_SEQUENCE_STALE, _SCOPE_STALE)
    runtime, _applied, _journal, receiver, stops = _fencing_runtime(tmp_path, host)
    runtime._applied_condition_keys = ("door",)  # noqa: SLF001
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    assert len(host.frames) == 2 and stops == []
    acknowledgement = receiver._handle(_heartbeat(sequence=2))  # noqa: SLF001
    assert acknowledgement["runner_fault"]["code"] == FAULT_CODE
    assert "program_bb_sequence_stale" in acknowledgement["runner_fault"]["message"]
    assert acknowledgement["command_fenced"] is True
    with pytest.raises(ProgramRunnerLivenessError, match="Runner fault: program_bbkey_publication_failed"):
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
    # The refused sender declares its loss in the
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
    assert released["runner_fault"] is None and released["command_fenced"] is False
    receiver.require_current()


def test_late_reply_for_a_superseded_between_run_source_releases_the_fence(
    tmp_path: Path,
) -> None:
    # The old loss is unconfirmed (scope stale); the
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
    assert held["runner_fault"]["code"] == FAULT_CODE and held["command_fenced"] is True
    queue.run()  # The newer baseline's ACK settles the old loss.
    assert [(f["cycle_generation"], f["message_sequence"]) for f in host.frames] == [
        (7, "1"), (7, _LOSS_SEQUENCE), (8, "1"),
    ]
    assert newer.fault is None
    released = receiver._handle(_heartbeat(sequence=3))  # noqa: SLF001
    assert released["runner_fault"] is None and released["command_fenced"] is False
    receiver.require_current()


def test_unacked_successor_does_not_release_the_fence(tmp_path: Path) -> None:
    # A successor whose baseline is refused too (and an old gap
    # reply that never confirms) is no proof: fence kept.
    host = _ScriptedHost(_SEQUENCE_STALE, _SCOPE_STALE, _SEQUENCE_STALE, _SCOPE_STALE)
    runtime, journal, receiver, queue = _queued_fencing_runtime(tmp_path, host)
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    journal.cycle_generation = 8
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    successor = runtime._bbkey_publishers[None]  # noqa: SLF001
    assert not successor.baseline_accepted
    queue.work.pop(0)()  # The old containment: fenced.
    with pytest.raises(ProgramRunnerLivenessError, match="Runner fault: program_bbkey_publication_failed"):
        receiver.require_current()
    queue.run()
    assert not successor.baseline_accepted
    held = receiver._handle(_heartbeat(sequence=2))  # noqa: SLF001
    assert held["runner_fault"]["code"] == FAULT_CODE and held["command_fenced"] is True
    assert len(runtime._bbkey_open_losses) == 2  # noqa: SLF001


def test_fence_waits_for_every_unconfirmed_loss(tmp_path: Path) -> None:
    # A and B both fault; A's confirmed loss alone does not lift
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
    assert held["runner_fault"]["code"] == FAULT_CODE and held["command_fenced"] is True
    assert len(runtime._bbkey_open_losses) == 1  # noqa: SLF001
    with pytest.raises(ProgramRunnerLivenessError, match="Runner fault: program_bbkey_publication_failed"):
        receiver.require_current()


def test_late_ack_of_a_replaced_sender_leaves_the_newer_loss_open(tmp_path: Path) -> None:
    # A's baseline ACK is delayed; meanwhile a Reset installs B, whose
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
    assert held["runner_fault"]["code"] == FAULT_CODE and held["command_fenced"] is True
    queue.run()  # B's own gap reply is unavailable: the fence stays.
    with pytest.raises(ProgramRunnerLivenessError, match="Runner fault: program_bbkey_publication_failed"):
        receiver.require_current()


def test_concurrent_followers_of_one_scope_create_one_sender() -> None:
    # A and B both find no current source before either installs;
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
            3, 1, "sha256:" + "d" * 64, 5, 7
        )
    )
    runtime._bbkey_publishers[1] = publisher  # noqa: SLF001
    publisher.start(_store(), ("count",))
    assert stops == [1]
    assert publisher.fault is not None
    acknowledgement = receiver._handle(_heartbeat(sequence=2))  # noqa: SLF001
    assert acknowledgement["runner_fault"]["code"] == FAULT_CODE
    assert "program_bb_sequence_stale" in acknowledgement["runner_fault"]["message"]
    assert acknowledgement["command_fenced"] is True
    with pytest.raises(ProgramRunnerLivenessError, match="Runner fault: program_bbkey_publication_failed"):
        receiver.require_current()


def _run_publisher(
    runtime: ProgramRuntime, store: ProgramValueStore
) -> ProgramBlackboardKeyPublisher:
    publisher = runtime._new_bbkey_publisher(  # noqa: SLF001
        ProgramBlackboardPublicationScope(3, 1, "sha256:" + "d" * 64, 5, 7)
    )
    runtime._bbkey_publishers[1] = publisher  # noqa: SLF001
    publisher.start(store, ("count",))
    return publisher


def test_a_run_source_fault_ends_once_the_kernel_confirms_its_loss(tmp_path: Path) -> None:
    # A write the Kernel refuses during a Runner loss: the Kernel answers the
    # write and the declaration with its typed loss, the Run is stopped, the
    # fault ends with the Run, and Reset's Runner gate and source handoff work.
    host = _Host()
    runtime, _applied, journal, receiver, stops = _fencing_runtime(tmp_path, host)
    store = _store()
    publisher = _run_publisher(runtime, store)
    assert publisher.baseline_accepted
    host.fail_with = _SEQUENCE_GAP
    store.write("count", 3)
    assert [(f["kind"], f["message_sequence"]) for f in host.frames] == [
        ("baseline", "1"), ("update", "2"), ("update", _LOSS_SEQUENCE),
    ]
    assert stops == [1]
    assert publisher.fault is not None and publisher.loss_confirmed
    assert runtime._bbkey_open_losses == {}  # noqa: SLF001
    acknowledgement = receiver._handle(_heartbeat(sequence=2))  # noqa: SLF001
    assert acknowledgement["runner_fault"] is None
    assert acknowledgement["command_fenced"] is False
    receiver.require_current()

    # Reset: the Run's sender retires and the next cycle's source installs.
    host.fail_with = None
    runtime._reset_bbkey_publication(1)  # noqa: SLF001
    runtime._applied_condition_keys = ("door",)  # noqa: SLF001
    journal.cycle_generation = 8
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    fresh = runtime._bbkey_publishers[None]  # noqa: SLF001
    assert fresh.baseline_accepted and fresh.fault is None
    assert host.frames[-1]["cycle_generation"] == 8
    assert receiver._handle(_heartbeat(sequence=3))["runner_fault"] is None  # noqa: SLF001
    receiver.require_current()


def test_a_contact_loss_without_a_refused_value_leaves_the_run_publishing(
    tmp_path: Path,
) -> None:
    # The Manager's missed exchanges change nothing in the Runner: when it is
    # heard again no fault is latched and the Run's writes are still forwarded.
    host = _Host()
    runtime, _applied, _journal, receiver, stops = _fencing_runtime(tmp_path, host)
    store = _store()
    publisher = _run_publisher(runtime, store)
    acknowledgement = receiver._handle(_heartbeat(sequence=2))  # noqa: SLF001
    assert acknowledgement["runner_fault"] is None
    assert acknowledgement["command_fenced"] is False
    receiver.require_current()
    store.write("count", 4)
    assert [(f["kind"], f["message_sequence"]) for f in host.frames] == [
        ("baseline", "1"), ("update", "2"),
    ]
    assert publisher.fault is None and stops == []


def test_a_source_not_ready_starts_later_from_the_values_committed_meanwhile(
    tmp_path: Path,
) -> None:
    # the Kernel has not installed the source yet (Reset just
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
    assert receiver._handle(_heartbeat(sequence=2))["runner_fault"] is None  # noqa: SLF001


# Captured under the store lock, sent off it, on one ordered sender -------


class _ManualSpawn:
    """A sender's thread start that runs nothing until the test drains it."""

    def __init__(self) -> None:
        self.pending: list[Callable[[], None]] = []

    def __call__(self, _name: str, work: Callable[[], None]) -> None:
        self.pending.append(work)

    def run(self) -> None:
        while self.pending:
            self.pending.pop(0)()


def _all_sent(host: _Host, count: int) -> threading.Event:
    sent = threading.Event()
    during = host.during

    def observe(request: dict[str, Any]) -> None:
        if during is not None:
            during(request)
        if len(host.frames) >= count:
            sent.set()

    host.during = observe
    return sent


def test_concurrent_writers_reach_the_wire_in_strictly_increasing_sequence() -> None:
    store = _store()
    host = _Host()
    writes = {
        "count": list(range(1, 101)),
        "speed": [float(value) for value in range(1, 101)],
        "label": [f"label-{value}" for value in range(100)],
        "door_open": [value % 2 == 0 for value in range(100)],
    }
    sent = _all_sent(host, 1 + sum(len(values) for values in writes.values()))
    publisher = _publisher(
        host, _Queue(), source_sender=RunSender(1, name="test-bbkey-sender")
    )
    publisher.start(store, _KEYS)
    writers = [
        threading.Thread(
            target=lambda key=key, values=values: [store.write(key, v) for v in values]
        )
        for key, values in writes.items()
    ]
    for writer in writers:
        writer.start()
    for writer in writers:
        writer.join(10.0)
    assert sent.wait(10.0)
    sequences = [int(frame["message_sequence"]) for frame in host.frames]
    assert sequences == list(range(1, len(host.frames) + 1))
    assert all(
        int(frame["previous_sequence"]) == int(frame["message_sequence"]) - 1
        for frame in host.frames
    )
    # Each key's frames carry its writer's values in commit order.
    for key, values in writes.items():
        rows = [_rows(frame)[key] for frame in host.frames[1:] if key in _rows(frame)]
        assert rows == [str(value) if key == "count" else value for value in values]
    assert publisher.fault is None


def test_no_lock_is_held_while_a_frame_is_sent() -> None:
    store = _store()
    host = _Host()
    in_flight, release = threading.Event(), threading.Event()
    store_lock_owned: list[bool] = []

    def hold_update(request: dict[str, Any]) -> None:
        # The sending thread holds no store lock, and the frame does not
        # hold any writer or reader while it is in flight.
        store_lock_owned.append(store._lock._is_owned())  # noqa: SLF001
        if request["kind"] == "update" and request["message_sequence"] == "2":
            in_flight.set()
            assert release.wait(5.0)

    host.during = hold_update
    sent = _all_sent(host, 3)
    publisher = _publisher(
        host, _Queue(), source_sender=RunSender(1, name="test-bbkey-sender")
    )
    publisher.start(store, ("count",))
    store.write("count", 1)
    assert in_flight.wait(5.0)
    writer = threading.Thread(target=lambda: store.write("count", 2))
    writer.start()
    writer.join(5.0)
    assert not writer.is_alive() and store.read("count") == 2
    release.set()
    assert sent.wait(5.0)
    assert store_lock_owned == [False, False, False]
    assert [_rows(frame) for frame in host.frames[1:]] == [{"count": "1"}, {"count": "2"}]


def test_a_refused_frame_drops_the_later_ones_and_declares_the_loss_next() -> None:
    # The writer observes what it did before: the write commits and returns;
    # the fault latches, the loss is the source's next frame, then Program
    # Stop and the fault report run on the owner's pool.
    events: list[Any] = []
    store = _store()
    host = _Host()
    spawn, queue = _ManualSpawn(), _Queue()

    def refuse_two(request: dict[str, Any]) -> None:
        host.fail_with = _SCOPE_STALE if request["message_sequence"] == "2" else None

    host.during = refuse_two
    publisher = _publisher(
        host, queue, events=events, source_sender=RunSender(1, spawn=spawn)
    )
    publisher.start(store, ("count",))
    store.write("count", 1)
    store.write("count", 2)  # Captured behind the refused frame.
    assert host.frames == [] and store.read("count") == 2
    spawn.run()
    assert [frame["message_sequence"] for frame in host.frames] == ["1", "2", _LOSS_SEQUENCE]
    assert publisher.fault is not None and "program_bb_scope_stale" in publisher.fault["message"]
    store.write("count", 3)  # After the fault a write only commits.
    spawn.run()
    assert len(host.frames) == 3
    queue.run()
    assert [event[0] for event in events] == ["stop", "fault"]


def test_writes_behind_an_unready_baseline_are_carried_by_the_next_one() -> None:
    store = _store()
    host = _ScriptedHost(HeadlessApiError("bb_source_not_ready", "installing"))
    spawn = _ManualSpawn()
    publisher = _publisher(
        host, _Queue(), between_runs=True, source_sender=RunSender(1, spawn=spawn)
    )
    assert publisher.start(store, ("count",)) is False
    assert publisher.baseline_in_flight
    store.write("count", 4)  # Follows the baseline as update 2 ...
    spawn.run()
    # ... and is dropped with it: the source did not exist.
    assert [frame["message_sequence"] for frame in host.frames] == ["1"]
    assert not publisher.baseline_in_flight and not publisher.settled
    assert publisher.start(store, ("count",)) is False
    spawn.run()
    assert [(f["kind"], f["message_sequence"], _rows(f)) for f in host.frames[1:]] == [
        ("baseline", "1", {"count": "4"}),
    ]
    assert publisher.settled and publisher.fault is None


def test_a_run_source_frame_reaches_the_host_before_the_next_run_request() -> None:
    # A value a node writes before the next node's request is sent first:
    # both leave on the Run's one sender, in that order.
    store = _store()
    wire: list[Any] = []
    host = _Host()
    host.during = lambda request: wire.append(request["message_sequence"])
    run_spawn = _ManualSpawn()
    run_sender = RunSender(4, spawn=run_spawn)
    publisher = _publisher(host, _Queue(), run_sender=run_sender)
    publisher.start(store, ("count",))
    store.write("count", 1)
    run_sender.submit(lambda: wire.append("motion start"))
    run_spawn.run()
    assert wire == ["1", "2", "motion start"]


def test_halt_never_withdraws_a_publication_and_a_sent_one_completes() -> None:
    # Values are state: a Halt or the Run's end withdraws the Run's queued
    # requests but never a captured frame (a gap would lose the source). A
    # frame already sent completes with its real reply. After the Run's
    # sender closed, the source still drains on it (not withdrawable).
    store = _store()
    wire: list[Any] = []
    host = _Host()
    run_spawn = _ManualSpawn()
    run_sender = RunSender(4, spawn=run_spawn)
    publisher = _publisher(host, _Queue(), run_sender=run_sender)
    publisher.start(store, ("count",))
    run_spawn.run()
    store.write("count", 1)  # Queued on the Run's sender ...
    motion = run_sender.submit(lambda: wire.append("motion start"))
    run_sender.withdraw_queued()  # ... when the Halt arrives.
    assert motion.withdrawn
    run_spawn.run()
    assert [frame["message_sequence"] for frame in host.frames] == ["1", "2"]

    def halt_and_end_while_sent(request: dict[str, Any]) -> None:
        if request["message_sequence"] == "3":
            run_sender.withdraw_queued()
            run_sender.close()

    host.during = halt_and_end_while_sent
    store.write("count", 2)
    run_spawn.run()
    assert [frame["message_sequence"] for frame in host.frames] == ["1", "2", "3"]
    assert publisher.fault is None
    store.write("count", 3)  # The Run's sender is closed: still sent on it.
    run_spawn.run()
    assert [frame["message_sequence"] for frame in host.frames] == ["1", "2", "3", "4"]
    assert publisher.fault is None and wire == []


def test_source_creation_holds_no_lock_across_host_reads() -> None:
    applied, journal = _Applied(), _Journal()
    runtime, hosts = _runtime(applied, journal)
    runtime._applied_condition_keys = ("door",)  # noqa: SLF001
    held: list[tuple[str, bool]] = []
    program_applied, identity = applied.program_applied, applied.identity

    def owned(name: str) -> None:
        held.append((name, runtime._bbkey_source_lock._is_owned()))  # noqa: SLF001

    applied.program_applied = lambda: (owned("applied"), program_applied())[1]  # type: ignore[method-assign]
    applied.identity = lambda: (owned("identity"), identity())[1]  # type: ignore[method-assign]
    applied.read_active_tcp = lambda *, ordered: (  # type: ignore[attr-defined]
        owned("read_active_tcp"), {"robots": []}
    )[1]
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    assert held == [("applied", False), ("identity", False), ("read_active_tcp", False)]
    (between_runs,) = hosts
    assert [frame["kind"] for frame in between_runs.frames] == ["baseline"]

    held.clear()
    store = _store()
    runtime._owner = SimpleNamespace(  # type: ignore[assignment]  # noqa: SLF001
        run_sender=lambda _run_id: None, value_store=lambda _run_id: store
    )
    runtime._start_bbkey_publication(  # noqa: SLF001
        1, claim_generation=3, program_binding_id="sha256:" + "d" * 64,
        active_publication_generation=5, cycle_generation=7, keys=("count",),
        retained_bundle_id="bundle-1", graph_revision="graph-1",
        binding={"target_bindings": []},
    )
    assert held == [("read_active_tcp", False)]
    assert [frame["kind"] for frame in hosts[1].frames] == ["baseline"]
    assert None not in runtime._bbkey_publishers  # The Run retired it.  # noqa: SLF001


def test_a_creation_from_older_host_reads_drops() -> None:
    # Follower A read publication 5, then B read publication 6 and installed
    # it: A's later install would bring back the older source, so it drops.
    applied, journal = _Applied(), _Journal()
    runtime, hosts = _runtime(applied, journal)
    runtime._applied_condition_keys = ("door",)  # noqa: SLF001
    a_reading, release_a = threading.Event(), threading.Event()

    def read_active_tcp(*, ordered: bool) -> dict[str, Any]:
        if threading.current_thread().name == "A":
            a_reading.set()
            assert release_a.wait(5.0)
        return {"robots": []}

    applied.read_active_tcp = read_active_tcp  # type: ignore[attr-defined]
    first = threading.Thread(target=runtime._follow_cell_bbkey_source, name="A")  # noqa: SLF001
    first.start()
    assert a_reading.wait(5.0)
    applied.generation = 6
    runtime._follow_cell_bbkey_source()  # noqa: SLF001
    release_a.set()
    first.join(5.0)
    assert not first.is_alive()
    (newer,) = hosts
    assert newer.frames[0]["active_publication_generation"] == 6
    assert runtime._bbkey_publishers[None].scope.active_publication_generation == 6  # noqa: SLF001


# --- A write returns its result after the host's answer (OD-213 part 3) ------
#
# Base sent each frame inside the store write, so a write (and the node or
# command that made it) returned only after the Kernel answered it; a refused
# frame was declared lost and its containment queued before the write
# returned, and the write itself never raised. The send now leaves the tick;
# the writer still returns that result only after the answer.

_RUNNING = py_trees.common.Status.RUNNING
_SUCCESS = py_trees.common.Status.SUCCESS
_INVALID = py_trees.common.Status.INVALID


class _ThreadedSpawn(_ManualSpawn):
    """A run's sender whose queue is emptied on a thread of its own."""

    def run(self) -> None:
        while self.pending:
            worker = threading.Thread(target=self.pending.pop(0))
            worker.start()
            worker.join(5.0)


def _run_source(
    host: _Host, *, events: list[Any] | None = None, queue: _Queue | None = None,
) -> tuple[ProgramValueStore, RunSender, _ThreadedSpawn, ProgramBlackboardKeyPublisher]:
    store = _store()
    spawn = _ThreadedSpawn()
    sender = RunSender(4, spawn=spawn)
    publisher = _publisher(host, queue or _Queue(), events=events, run_sender=sender)
    publisher.start(store, ("count", "label"))
    spawn.run()
    assert publisher.baseline_accepted
    return store, sender, spawn, publisher


def _set_value(store: ProgramValueStore, sender: RunSender, value: Any = 7) -> LogicSetValue:
    node = LogicSetValue(
        name="set_count",
        parameters={"key": "count", "value": value},
        value_sources={},
        input_resolver=NodeAttemptInputResolver(
            program=store.program, catalog=_CATALOG, value_store=store
        ),
        attempt_source=SimpleNamespace(
            begin=lambda instance: NodeAttemptIdentity(1, instance, 1)
        ),
        run_sender=sender,
    )
    node.setup_with_descendants()
    return node


def test_a_write_is_answered_once_the_host_answered_its_frame() -> None:
    host = _Host()
    store, _sender, spawn, _publisher_ = _run_source(host)
    receipt = store.write("count", 1)
    assert receipt is not None and not receipt.answered
    assert store.read("count") == 1 and len(host.frames) == 1  # Committed, not sent.
    spawn.run()
    assert receipt.answered and _rows(host.frames[-1]) == {"count": "1"}
    # Nothing is sent: nothing to answer.
    assert store.write("speed", 2.0) is None
    assert store.write("count", 1) is None


def test_a_refused_write_is_answered_after_the_loss_and_its_containment() -> None:
    events: list[Any] = []
    queue = _Queue()
    host = _Host()
    store, _sender, spawn, publisher = _run_source(host, events=events, queue=queue)
    seen: list[tuple[bool, bool]] = []
    receipts: list[Any] = []

    def refuse_two(request: dict[str, Any]) -> None:
        host.fail_with = _SCOPE_STALE if request["message_sequence"] == "2" else None
        if request["message_sequence"] == _LOSS_SEQUENCE:
            seen.append((receipts[0].answered, receipts[1].answered))

    host.during = refuse_two
    receipts.append(store.write("count", 1))
    receipts.append(store.write("label", "later"))  # Dropped behind the refusal.
    spawn.run()
    # Both writers are answered only once the loss was declared and the
    # containment handed over (queued, not run), as base's write returned.
    assert seen == [(False, False)]
    assert all(receipt.answered for receipt in receipts)
    assert publisher.fault is not None and len(queue.work) == 1 and events == []
    queue.run()
    assert [event[0] for event in events] == ["stop", "fault"]
    assert store.write("count", 3) is None  # After the fault a write only commits.


def test_dropped_frames_are_answered_without_a_send() -> None:
    # A source not installed yet, or retired by the Runner: the frame is
    # never sent, and its writer is answered at once (base: only committed).
    host = _ScriptedHost(HeadlessApiError("bb_source_not_ready", "installing"))
    spawn = _ManualSpawn()
    store = _store()
    publisher = _publisher(
        host, _Queue(), between_runs=True, source_sender=RunSender(1, spawn=spawn)
    )
    publisher.start(store, ("count",))
    behind_baseline = store.write("count", 4)
    assert behind_baseline is not None and not behind_baseline.answered
    spawn.run()
    assert behind_baseline.answered and len(host.frames) == 1
    host2 = _Host()
    store2, _sender2, spawn2, publisher2 = _run_source(host2)
    queued = store2.write("count", 5)
    publisher2.retire()
    assert queued is not None and queued.answered
    spawn2.run()
    assert len(host2.frames) == 1


def test_a_command_thread_write_replies_after_the_host_answer() -> None:
    host = _Host()
    store, _sender, spawn, _publisher_ = _run_source(host)
    replied = threading.Event()

    def command() -> None:
        receipt = store.write("count", 9, metadata={"value_source": "runtime_control"})
        assert receipt is not None
        receipt.wait()
        replied.set()

    thread = threading.Thread(target=command)
    thread.start()
    assert not replied.wait(0.2)  # Committed; the reply waits for the host.
    assert store.read("count") == 9
    spawn.run()
    assert replied.wait(5.0)
    thread.join(5.0)


def test_a_set_value_node_is_running_until_the_host_answered_its_value() -> None:
    host = _Host()
    send_threads: list[int] = []
    host.during = lambda _request: send_threads.append(threading.get_ident())
    store, sender, spawn, _publisher_ = _run_source(host)
    node = _set_value(store, sender)
    node.tick_once()
    assert node.status is _RUNNING and store.read("count") == 7
    node.tick_once()
    assert node.status is _RUNNING and len(host.frames) == 1  # Nothing sent in the tick.
    later = sender.submit(lambda: "next node's request")
    spawn.run()
    node.tick_once()
    assert node.status is _SUCCESS
    # The frame left before the later request, never on the tick's thread.
    assert _rows(host.frames[-1]) == {"count": "7"} and later.answered
    assert threading.get_ident() not in send_threads


def test_a_refused_value_still_ends_the_node_as_base_did() -> None:
    # Base: the write never raised; the node succeeded after the loss was
    # declared, and the Run's Program Stop followed from the containment.
    events: list[Any] = []
    queue = _Queue()
    host = _Host()
    store, sender, spawn, publisher = _run_source(host, events=events, queue=queue)
    host.fail_with = _SCOPE_STALE
    node = _set_value(store, sender)
    node.tick_once()
    assert node.status is _RUNNING
    spawn.run()
    node.tick_once()
    assert node.status is _SUCCESS and publisher.fault is not None
    assert [frame["message_sequence"] for frame in host.frames][-1] == _LOSS_SEQUENCE
    queue.run()
    assert [event[0] for event in events] == ["stop", "fault"]


def test_a_stop_after_the_write_ends_the_node_with_its_real_result() -> None:
    host = _Host()
    store, sender, spawn, _publisher_ = _run_source(host)
    node = _set_value(store, sender)
    node.tick_once()
    assert node.status is _RUNNING
    node.stop(_INVALID)  # Forced Program End: the committed value still goes.
    assert node.settle_abandoned_request() is False
    sender.close()
    spawn.run()
    assert _rows(host.frames[-1]) == {"count": "7"}
    assert node.settle_abandoned_request() is True
    assert node.status is _SUCCESS


def test_an_invocation_stopped_after_its_send_writes_its_output() -> None:
    # Base made the read and wrote its output inside one tick: a Stop could
    # not fall between them. Now the stopped node's answered read still
    # writes the output, and ends once the host answered that value.
    host = _Host()
    store, sender, spawn, _publisher_ = _run_source(host)

    entered, release = threading.Event(), threading.Event()

    class _Port:
        def invoke(self, node_id, identity, inputs):
            entered.set()
            assert release.wait(5.0)
            return OwnerInvocationResult(True, output="tripped")

    node = OwnerInvocationAdapter(
        name="read", node_id="io.get_input", node_instance_id="read",
        port=_Port(), parameters={"output_key": "label"}, value_sources={},
        output_key_parameter="output_key",
        attempt_source=SimpleNamespace(begin=lambda instance: NodeAttemptIdentity(1, instance, 1)),
        input_resolver=SimpleNamespace(
            snapshot=lambda **kwargs: SimpleNamespace(
                parameters=kwargs["parameters"], blackboard={}
            ),
            value_store=store,
        ),
        run_sender=sender,
    )
    node.setup_with_descendants()
    node.tick_once()
    assert node.status is _RUNNING
    worker = threading.Thread(target=spawn.run)
    worker.start()
    assert entered.wait(5.0)  # The read is sent ...
    node.stop(_INVALID)  # ... when the Forced Program End arrives.
    sender.close()
    assert node.settle_abandoned_request() is False
    release.set()
    worker.join(5.0)  # The read is answered.
    assert node.settle_abandoned_request() is False  # Its output is on its way.
    assert store.read("label") == "tripped"
    spawn.run()
    assert _rows(host.frames[-1]) == {"label": "tripped"}
    assert node.settle_abandoned_request() is True and node.status is _SUCCESS
