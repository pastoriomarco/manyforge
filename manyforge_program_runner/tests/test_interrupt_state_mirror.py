"""Global-rule state mirror over the existing events reply (contract 64)."""

from __future__ import annotations

from copy import deepcopy
import threading
from typing import Any

import pytest

from manyforge_behavior.orchestration_values import ProgramValueError
from manyforge_headless_api.native import HeadlessApiError
from manyforge_program_runner.bbkey_publication import ProgramBlackboardPublicationScope
from manyforge_program_runner.execution_transport import ProgramRunnerExecutionTransport
from manyforge_program_runner.interrupt_state_mirror import (
    InterruptRuleStateMirror,
    InterruptStateWireError,
    RuleCatalog,
    parse_baseline,
    parse_catalog,
    parse_state_event,
)
from manyforge_program_runner.tests.test_bbkey_publication import _store


_CONTEXT = {
    "owner_epoch": "kernel-1",
    "catalog_generation": "4",
    "active_publication_generation": 3,
    "retained_bundle_id": "bundle-1",
    "graph_revision": "graph-1",
}
_ACQUISITION = {
    "source_id": "cell.io",
    "source_epoch": "io-epoch",
    "source_sequence": "18446744073709551615",
    "source_cursor": {
        "owner_epoch": "kernel-1",
        "catalog_generation": "4",
        "source_id": "cell.io",
        "record_sequence": "12",
    },
}


def _ready(through: int, states: dict[str, str], **context: Any) -> dict[str, Any]:
    return {
        "status": "ready",
        "snapshot": {
            **_CONTEXT,
            **context,
            "through_sequence": through,
            "states": [
                {
                    "rule_id": rule_id,
                    "state": state,
                    "triggering_acquisition": None if state == "clear" else _ACQUISITION,
                }
                for rule_id, state in sorted(states.items())
            ],
        },
    }


def _event(sequence: int, rule_id: str, state: str, **context: Any) -> dict[str, Any]:
    return {
        "sequence": sequence,
        "record_kind": "interrupt_rule_state_changed",
        "payload": {
            **_CONTEXT,
            **context,
            "rule_id": rule_id,
            "state": state,
            "triggering_acquisition": None,
        },
    }


def _other(sequence: int) -> dict[str, Any]:
    return {"sequence": sequence, "record_kind": "operation_terminal", "payload": {}}


_AUTO = object()


def _proving_catalog(snapshot: dict[str, Any]) -> dict[str, Any]:
    return {
        "owner_epoch": snapshot["owner_epoch"],
        "catalog_generation": snapshot["catalog_generation"],
        "active_publication_generation": snapshot["active_publication_generation"],
        "bundle": snapshot["retained_bundle_id"],
        "graph": snapshot["graph_revision"],
        "rule_ids": sorted(row["rule_id"] for row in snapshot["states"]),
        "referenced_program_blackboard_keys": [],
    }


class _Kernel:
    """The existing events command: one owner's ordered journal. Unless a
    test sets ``catalog``, the reply carries the catalog proving the latest
    ready baseline's context (SX-2216: states need that proof)."""

    def __init__(self) -> None:
        self.owner = "kernel-1"
        self.events: list[dict[str, Any]] = []
        self.baseline: Any = None
        self.catalog: Any = _AUTO
        self._proving: dict[str, Any] | None = None
        self.reads: list[int] = []
        self.fail: Exception | None = None

    def __call__(self, owner_epoch: str, after: int) -> dict[str, Any]:
        self.reads.append(after)
        if self.fail is not None:
            raise self.fail
        rows = [event for event in self.events if event["sequence"] > after][:16]
        if isinstance(self.baseline, dict) and self.baseline.get("status") == "ready":
            self._proving = _proving_catalog(self.baseline["snapshot"])
        catalog = self._proving if self.catalog is _AUTO else self.catalog
        return {
            "command": "events",
            "ok": True,
            "owner_epoch": owner_epoch,
            "interrupt_rule_state_baseline": deepcopy(self.baseline),
            "interrupt_rule_catalog": deepcopy(catalog),
            "event_page": {
                "owner_epoch": owner_epoch,
                "events": deepcopy(rows),
                "next_after_sequence": rows[-1]["sequence"] if rows else after,
                "has_more": len(rows) == 16,
            },
        }


def _mirror(kernel: _Kernel) -> tuple[InterruptRuleStateMirror, list[Any]]:
    changes: list[Any] = []
    mirror = InterruptRuleStateMirror(
        owner_epoch=lambda: kernel.owner, read_events=kernel, on_change=changes.append
    )
    return mirror, changes


def test_ready_baseline_then_only_same_context_events_after_the_cut() -> None:
    kernel = _Kernel()
    kernel.events = [_event(1, "door", "tripped"), _other(2), _event(3, "door", "clear")]
    kernel.baseline = _ready(3, {"door": "inactive", "zone": "clear"})
    mirror, changes = _mirror(kernel)
    mirror.poll()
    # Rows at/before through_sequence never replay over the baseline.
    assert mirror.current() == {"door": "inactive", "zone": "clear"}
    kernel.events += [_other(4), _event(5, "zone", "tripped")]
    mirror.poll()
    assert mirror.current() == {"door": "inactive", "zone": "tripped"}
    assert changes == [{"door": "inactive", "zone": "clear"}, {"door": "inactive", "zone": "tripped"}]
    mirror.poll()  # No change, no notification.
    assert len(changes) == 2 and kernel.reads[-1] == 5


def test_pending_keeps_a_qualified_mirror_and_follows_its_events() -> None:
    kernel = _Kernel()
    kernel.baseline = _ready(0, {"door": "clear"})
    mirror, _changes = _mirror(kernel)
    mirror.poll()
    kernel.baseline = {"status": "projection_pending"}
    kernel.events = [_event(1, "door", "tripped")]
    mirror.poll()
    assert mirror.current() == {"door": "tripped"}


def test_pending_without_a_mirror_is_unavailable_then_installs_the_later_cut() -> None:
    kernel = _Kernel()
    kernel.baseline = {"status": "projection_pending"}
    kernel.events = [_event(1, "door", "tripped"), _event(2, "door", "inactive")]
    mirror, changes = _mirror(kernel)
    mirror.poll()
    assert mirror.current() is None and changes == [None]
    # The cut lies before rows already passed: they are reread after it.
    kernel.baseline = _ready(1, {"door": "tripped"})
    mirror.poll()
    assert mirror.current() == {"door": "inactive"}
    assert kernel.reads == [0, 2, 1]


def test_null_baseline_means_no_catalog_never_clear() -> None:
    kernel = _Kernel()
    kernel.baseline = _ready(0, {"door": "clear"})
    mirror, changes = _mirror(kernel)
    mirror.poll()
    kernel.baseline = None
    mirror.poll()
    assert mirror.current() is None and changes[-1] is None


def test_context_replacement_recaptures_from_the_new_baseline() -> None:
    kernel = _Kernel()
    kernel.baseline = _ready(0, {"door": "clear"})
    mirror, _changes = _mirror(kernel)
    mirror.poll()
    # A new publication's event arrives before its baseline is ready.
    kernel.baseline = {"status": "projection_pending"}
    kernel.events = [_event(1, "door", "tripped", active_publication_generation=4)]
    mirror.poll()
    assert mirror.current() is None
    kernel.baseline = _ready(1, {"door": "tripped"}, active_publication_generation=4)
    kernel.events.append(_event(2, "door", "inactive", active_publication_generation=4))
    mirror.poll()
    assert mirror.current() == {"door": "inactive"}


def test_owner_change_restarts_the_cursor_and_recaptures() -> None:
    kernel = _Kernel()
    kernel.events = [_event(1, "door", "tripped")]
    kernel.baseline = _ready(1, {"door": "tripped"})
    mirror, _changes = _mirror(kernel)
    mirror.poll()
    kernel.owner = "kernel-2"
    kernel.events = []
    kernel.baseline = _ready(0, {"door": "clear"}, owner_epoch="kernel-2")
    mirror.poll()
    assert mirror.current() == {"door": "clear"} and kernel.reads[-1] == 0


@pytest.mark.parametrize(
    "failure",
    [
        HeadlessApiError("execution_event_cursor_unavailable", "gap"),
        "malformed_event",
        "foreign_baseline",
    ],
)
def test_gap_unreadable_or_malformed_rows_drop_the_mirror(failure) -> None:
    kernel = _Kernel()
    kernel.baseline = _ready(0, {"door": "clear"})
    mirror, _changes = _mirror(kernel)
    mirror.poll()
    if failure == "malformed_event":
        kernel.events = [{**_event(1, "door", "clear"), "payload": {"rule_id": "door"}}]
    elif failure == "foreign_baseline":
        kernel.baseline = _ready(0, {"door": "clear"}, owner_epoch="kernel-9")
    else:
        kernel.fail = failure
    mirror.poll()
    assert mirror.current() is None
    kernel.fail, kernel.events = None, []
    kernel.baseline = _ready(0, {"door": "tripped"})
    mirror.poll()
    assert mirror.current() == {"door": "tripped"}


@pytest.mark.parametrize(
    "baseline",
    [
        {"status": "projection_pending", "snapshot": None},
        {"status": "ready"},
        {"status": "stale", "snapshot": _ready(0, {})["snapshot"]},
        {"status": "ready", "snapshot": {**_ready(0, {})["snapshot"], "extra": 1}},
        {"status": "ready", "snapshot": {**_ready(0, {})["snapshot"], "through_sequence": "0"}},
        {"status": "ready", "snapshot": {**_ready(0, {})["snapshot"], "through_sequence": -1}},
        {"status": "ready", "snapshot": {**_ready(0, {})["snapshot"], "catalog_generation": 4}},
        {"status": "ready", "snapshot": {**_ready(0, {})["snapshot"], "catalog_generation": "04"}},
        {"status": "ready",
         "snapshot": {**_ready(0, {})["snapshot"], "active_publication_generation": 0}},
        {"status": "ready", "snapshot": {**_ready(0, {})["snapshot"], "retained_bundle_id": ""}},
        _ready(0, {"door": "unknown"}),
    ],
)
def test_malformed_baselines_are_refused(baseline) -> None:
    with pytest.raises(InterruptStateWireError):
        parse_baseline(baseline)


def test_baseline_rows_are_sorted_unique_and_acquisitions_exact() -> None:
    ready = _ready(2, {"a": "tripped", "b": "clear"})
    assert parse_baseline(ready).states == {"a": "tripped", "b": "clear"}  # type: ignore[union-attr]
    unsorted = deepcopy(ready)
    unsorted["snapshot"]["states"].reverse()
    duplicate = deepcopy(ready)
    duplicate["snapshot"]["states"][1]["rule_id"] = "a"
    bad_acquisition = deepcopy(ready)
    bad_acquisition["snapshot"]["states"][0]["triggering_acquisition"] = {
        **_ACQUISITION, "source_sequence": 5,
    }
    for value in (unsorted, duplicate, bad_acquisition):
        with pytest.raises(InterruptStateWireError):
            parse_baseline(value)
    assert parse_state_event(_event(3, "a", "inactive")["payload"])[1:] == ("a", "inactive")
    with pytest.raises(InterruptStateWireError):
        parse_state_event({**_event(3, "a", "inactive")["payload"], "extra": 1})


def test_reserved_interrupt_state_keys_are_system_written_and_survive_reset() -> None:
    store = _store()
    program = store._program.with_interrupt_state_keys(("door",))  # noqa: SLF001
    store = _store(program)
    assert program.blackboard_keys["interrupt_state.door"].access == "read"
    assert store.write_interrupt_state("zone", "clear") is False  # Not an Applied rule.
    assert store.write_interrupt_state("door", "tripped") is True
    assert store.read("interrupt_state.door") == "tripped"
    with pytest.raises(ProgramValueError):
        store.write("interrupt_state.door", "clear")
    with pytest.raises(ProgramValueError):
        store.write_interrupt_state("door", "open")
    store.clear_run_values()  # Always current: Reset never clears it.
    assert store.read("interrupt_state.door") == "tripped"
    store.write_interrupt_state("door", None)  # Unavailable, not clear.
    with pytest.raises(KeyError):
        store.read("interrupt_state.door")


def test_mirror_changes_reach_the_published_key_stream() -> None:
    program = _store()._program.with_interrupt_state_keys(("door",))  # noqa: SLF001
    store = _store(program)
    published: list[Any] = []
    store.subscribe_published_keys(
        ("interrupt_state.door",),
        baseline=lambda rows: published.append([(r.key, r.present) for r in rows]),
        on_change=lambda change: published.append((change.value, change.present)),
    )
    store.write_interrupt_state("door", "inactive")
    store.write_interrupt_state("door", "inactive")
    store.write_interrupt_state("door", None)
    assert published == [[("interrupt_state.door", False)], ("inactive", True), (None, False)]


def test_transport_reads_the_existing_events_command_whole() -> None:
    transport = object.__new__(ProgramRunnerExecutionTransport)
    sent: list[dict[str, Any]] = []
    reply = _Kernel()("kernel-1", 0)
    transport._exchange = lambda command: sent.append(command) or dict(reply)  # type: ignore[method-assign]  # noqa: SLF001
    assert transport.read_interrupt_rule_state_events("kernel-1", 7) == reply
    assert sent == [{
        "api_version": "manyforge.execution_command.v1",
        "after_sequence": 7,
        "command": "events",
        "limit": 16,
        "owner_epoch": "kernel-1",
    }]
    transport._exchange = lambda command: {  # type: ignore[method-assign]  # noqa: SLF001
        "ok": False, "error": {"code": "execution_event_cursor_unavailable",
                               "message": "gap", "details": {}},
    }
    with pytest.raises(HeadlessApiError) as error:
        transport.read_interrupt_rule_state_events("kernel-1", 7)
    assert error.value.code == "execution_event_cursor_unavailable"


_CATALOG = {
    "owner_epoch": "kernel-1",
    "catalog_generation": "18446744073709551615",
    "active_publication_generation": 3,
    "bundle": "bundle-1",
    "graph": "graph-1",
    "rule_ids": ["door", "zone"],
    "referenced_program_blackboard_keys": ["door_open", "zone_clear"],
}


def test_catalog_is_read_independently_of_the_state_baseline() -> None:
    kernel = _Kernel()
    kernel.baseline = {"status": "projection_pending"}
    kernel.catalog = _CATALOG
    mirror, _changes = _mirror(kernel)
    # The first read polls once; states stay unavailable while pending.
    catalog = mirror.catalog()
    assert catalog == RuleCatalog(
        "kernel-1", "18446744073709551615", 3, "bundle-1", "graph-1",
        ("door", "zone"), ("door_open", "zone_clear"),
    )
    assert mirror.current() is None and kernel.reads == [0]
    assert mirror.catalog() is catalog and kernel.reads == [0]  # No extra read.
    kernel.catalog = None  # Uninstalled.
    mirror.poll()
    assert mirror.catalog() is None


def test_catalog_of_another_owner_or_malformed_drops_it() -> None:
    kernel = _Kernel()
    kernel.catalog = _CATALOG
    mirror, _changes = _mirror(kernel)
    mirror.poll()
    kernel.catalog = {**_CATALOG, "owner_epoch": "kernel-2"}
    mirror.poll()
    assert mirror.catalog() is None


@pytest.mark.parametrize(
    "catalog",
    [
        {**_CATALOG, "extra": 1},
        {key: value for key, value in _CATALOG.items() if key != "graph"},
        {**_CATALOG, "rule_ids": ["zone", "door"]},
        {**_CATALOG, "rule_ids": ["door", "door"]},
        {**_CATALOG, "referenced_program_blackboard_keys": [""]},
        {**_CATALOG, "catalog_generation": "0"},
        {**_CATALOG, "catalog_generation": 4},
        {**_CATALOG, "active_publication_generation": 0},
        {**_CATALOG, "active_publication_generation": True},
        {**_CATALOG, "bundle": ""},
    ],
)
def test_malformed_catalogs_are_refused(catalog) -> None:
    with pytest.raises(InterruptStateWireError):
        parse_catalog(catalog)
    assert parse_catalog(None) is None


class _CatalogMirror:
    def __init__(self, catalog: RuleCatalog | None) -> None:
        self.value = catalog
        self.polls = 0
        self.arrive: RuleCatalog | None = None  # Delivered by the next poll.

    def catalog(self) -> RuleCatalog | None:
        return self.value

    def carried_object_last_losses(self) -> None:
        return None

    def poll(self) -> None:
        self.polls += 1
        if self.arrive is not None:
            self.value, self.arrive = self.arrive, None


def _hook_runtime(catalog: RuleCatalog | None):
    from types import SimpleNamespace
    import threading

    from manyforge_program_runner.runtime import ProgramRuntime

    runtime = object.__new__(ProgramRuntime)
    runtime._interrupt_state_mirror = _CatalogMirror(catalog)  # noqa: SLF001
    runtime._grasp_lost_mirror_lock = threading.Lock()  # noqa: SLF001
    runtime._carried_object_lost_run_id = None  # noqa: SLF001
    runtime._carried_object_lost_run_active = False  # noqa: SLF001
    runtime._carried_object_lost_seen_event_ids = set()  # noqa: SLF001
    runtime._applied_condition_keys = ("count", "zone_clear")  # noqa: SLF001
    runtime._bbkey_publishers = {}  # noqa: SLF001
    runtime._bbkey_pending = {}  # noqa: SLF001
    runtime._bbkey_publishers_lock = threading.RLock()  # noqa: SLF001
    runtime._bbkey_source_lock = threading.RLock()  # noqa: SLF001
    runtime.config = SimpleNamespace(headless_timeout_seconds=5.0)
    return runtime


class _Publisher:
    def __init__(self, generation: int) -> None:
        # The real scope type: every scope carries all of its identities.
        self.scope = ProgramBlackboardPublicationScope(
            owner_epoch="kernel-owner",
            program_runner_owner_epoch="runner-owner",
            claim_generation=None,
            program_run_id=None,
            program_binding_id="sha256:" + "d" * 64,
            active_publication_generation=generation,
            cycle_generation=7,
        )
        self.started: tuple[str, ...] | None = None
        self.retired = False
        self.failed: str | None = None

    def start(self, store, keys) -> bool:
        self.started = keys
        return True

    def retire(self) -> None:
        self.retired = True

    def fail(self, message: str) -> None:
        self.failed = message


def test_runner_hooks_use_the_installed_catalog() -> None:
    catalog = parse_catalog(_CATALOG)
    runtime = _hook_runtime(catalog)
    # The learned Program part; the global keys join per publication.
    assert runtime._cell_bbkey_union() == ("count", "zone_clear")  # noqa: SLF001
    assert runtime._applied_global_rule_ids() == ("door", "zone")  # noqa: SLF001
    assert runtime._applied_global_rule_ids(  # noqa: SLF001
        retained_bundle_id="bundle-1", graph_revision="graph-1") == ("door", "zone")
    # A catalog of another publication declares no key for this Program.
    assert runtime._applied_global_rule_ids(  # noqa: SLF001
        retained_bundle_id="bundle-2", graph_revision="graph-1") == ()
    program = _store()._program  # noqa: SLF001
    declared = runtime._with_global_rule_keys(  # noqa: SLF001
        program, retained_bundle_id="bundle-1", graph_revision="graph-1")
    assert {"interrupt_state.door", "interrupt_state.zone"} <= set(declared.blackboard_keys)
    assert runtime._with_global_rule_keys(  # noqa: SLF001
        program, retained_bundle_id="bundle-1", graph_revision="graph-2") is program
    runtime._interrupt_state_mirror = _CatalogMirror(None)  # noqa: SLF001
    assert runtime._applied_global_rule_ids() == ()  # noqa: SLF001


_CONTEXT_W = ("kernel-1", 3, "bundle-1", "graph-1")
_VERIFIED_EMPTY = {
    "status": "verified_empty", "owner_epoch": "kernel-1",
    "active_publication_generation": 3, "bundle": "bundle-1", "graph": "graph-1",
}


def test_verified_empty_catalog_parses_strictly() -> None:
    empty = parse_catalog(_VERIFIED_EMPTY)
    assert empty == RuleCatalog("kernel-1", None, 3, "bundle-1", "graph-1", (), ())
    assert empty.context == _CONTEXT_W
    for malformed in (
        {**_VERIFIED_EMPTY, "status": "empty"},
        {**_VERIFIED_EMPTY, "rule_ids": []},
        {key: value for key, value in _VERIFIED_EMPTY.items() if key != "graph"},
        {**_VERIFIED_EMPTY, "active_publication_generation": 0},
    ):
        with pytest.raises(InterruptStateWireError):
            parse_catalog(malformed)


def test_baseline_sends_the_union_of_the_same_context_catalog() -> None:
    runtime = _hook_runtime(parse_catalog(_CATALOG))
    publisher = _Publisher(3)
    runtime._bbkey_publishers["run"] = publisher  # noqa: SLF001
    runtime._start_or_defer_bbkey_source(  # noqa: SLF001
        "run", publisher, object(), ("count", "zone_clear"), _CONTEXT_W)
    assert publisher.started == ("count", "door_open", "zone_clear")
    assert runtime._bbkey_pending == {}  # noqa: SLF001


@pytest.mark.parametrize(
    "catalog",
    [
        None,
        {**_CATALOG, "active_publication_generation": 2},
        {**_CATALOG, "bundle": "bundle-0"},
        {**_CATALOG, "graph": "graph-0"},
        {**_VERIFIED_EMPTY, "active_publication_generation": 2},
    ],
)
def test_null_or_other_context_catalog_is_not_known_and_waits_then_latches(catalog, monkeypatch) -> None:
    import manyforge_program_runner.runtime as runtime_module

    runtime = _hook_runtime(None if catalog is None else parse_catalog(catalog))
    publisher = _Publisher(3)
    runtime._bbkey_publishers["run"] = publisher  # noqa: SLF001
    now = [1_000]
    monkeypatch.setattr(runtime_module.time, "monotonic_ns", lambda: now[0])
    runtime._start_or_defer_bbkey_source("run", publisher, object(), ("count",), _CONTEXT_W)  # noqa: SLF001
    # Not an empty policy: no baseline yet, one fresh read tried.
    assert publisher.started is None and not publisher.retired
    assert "run" in runtime._bbkey_pending  # noqa: SLF001
    assert runtime._interrupt_state_mirror.polls == 1  # noqa: SLF001
    runtime._retry_pending_bbkey_sources()  # noqa: SLF001  # Within the budget: still waiting.
    assert publisher.started is None and publisher.failed is None
    now[0] += 5_000_000_001  # The budget is spent: the existing publisher fault.
    runtime._retry_pending_bbkey_sources()  # noqa: SLF001
    assert publisher.started is None
    assert "publication 3 was not installed" in publisher.failed
    assert runtime._bbkey_pending == {}  # noqa: SLF001


def test_catalog_arriving_after_the_publication_starts_the_baseline() -> None:
    runtime = _hook_runtime(None)
    publisher = _Publisher(3)
    runtime._bbkey_publishers[None] = publisher  # noqa: SLF001
    runtime._start_or_defer_bbkey_source(None, publisher, object(), (), _CONTEXT_W)  # noqa: SLF001
    assert publisher.started is None
    runtime._interrupt_state_mirror.arrive = parse_catalog(_CATALOG)  # noqa: SLF001
    runtime._retry_pending_bbkey_sources()  # noqa: SLF001
    assert publisher.started == ("door_open", "zone_clear")
    assert runtime._bbkey_pending == {}  # noqa: SLF001
    # A source retired meanwhile (Reset, new Run) is never started.
    stale = _Publisher(3)
    runtime._interrupt_state_mirror.value = None  # noqa: SLF001
    runtime._start_or_defer_bbkey_source("old", stale, object(), ("count",), _CONTEXT_W)  # noqa: SLF001
    runtime._interrupt_state_mirror.arrive = parse_catalog(_CATALOG)  # noqa: SLF001
    runtime._retry_pending_bbkey_sources()  # noqa: SLF001
    assert stale.started is None and stale.failed is None


def test_verified_empty_with_program_keys_is_a_normal_sender() -> None:
    runtime = _hook_runtime(parse_catalog(_VERIFIED_EMPTY))
    publisher = _Publisher(3)
    runtime._bbkey_publishers["run"] = publisher  # noqa: SLF001
    runtime._start_or_defer_bbkey_source("run", publisher, object(), ("count",), _CONTEXT_W)  # noqa: SLF001
    assert publisher.started == ("count",)
    assert runtime._applied_global_rule_ids() == ()  # noqa: SLF001


def test_verified_empty_with_no_program_key_is_w0_no_sender_no_baseline() -> None:
    from types import SimpleNamespace

    runtime = _hook_runtime(parse_catalog(_VERIFIED_EMPTY))
    runtime._owner = SimpleNamespace(value_store=lambda run_id: SimpleNamespace())  # noqa: SLF001
    created: list[Any] = []
    runtime._new_bbkey_publisher = lambda scope: created.append(scope)  # type: ignore[method-assign]  # noqa: SLF001
    runtime._retire_bbkey_publishers = lambda **_kwargs: None  # type: ignore[method-assign]  # noqa: SLF001
    runtime._interrupt_state_run_id = None  # noqa: SLF001
    runtime.owner_epoch = "runner-1"
    assert runtime._start_bbkey_publication(  # noqa: SLF001
        "run", owner_epoch="kernel-1", claim_generation=1, program_binding_id="binding",
        active_publication_generation=3, cycle_generation=1, keys=(),
        retained_bundle_id="bundle-1", graph_revision="graph-1",
    ) is None
    assert created == [] and runtime._bbkey_pending == {}  # noqa: SLF001
    # The same with an unknown catalog: a sender waits for its context.
    runtime._interrupt_state_mirror = _CatalogMirror(None)  # noqa: SLF001
    runtime._new_bbkey_publisher = lambda scope: _Publisher(scope.active_publication_generation)  # type: ignore[method-assign]  # noqa: SLF001
    waiting = runtime._start_bbkey_publication(  # noqa: SLF001
        "run", owner_epoch="kernel-1", claim_generation=1, program_binding_id="binding",
        active_publication_generation=3, cycle_generation=1, keys=(),
        retained_bundle_id="bundle-1", graph_revision="graph-1",
    )
    assert waiting is not None and waiting.started is None and "run" in runtime._bbkey_pending  # noqa: SLF001


_CATALOG_A = {**_CATALOG, "catalog_generation": "4"}  # Names _CONTEXT exactly.


def test_context_change_proven_by_the_catalog_invalidates_until_its_ready_cut() -> None:
    # SX-2207: catalog B with pending never keeps A's states current.
    kernel = _Kernel()
    kernel.catalog = _CATALOG_A
    kernel.baseline = _ready(0, {"door": "inactive"})
    mirror, changes = _mirror(kernel)
    mirror.poll()
    assert mirror.current() == {"door": "inactive"}
    kernel.catalog = {**_CATALOG_A, "catalog_generation": "5"}
    kernel.baseline = {"status": "projection_pending"}
    mirror.poll()
    assert mirror.current() is None and changes[-1] is None
    # A cut of A (not what the catalog proves) is not installed either.
    kernel.baseline = _ready(0, {"door": "inactive"})
    mirror.poll()
    assert mirror.current() is None
    kernel.baseline = _ready(0, {"door": "clear"}, catalog_generation="5")
    mirror.poll()
    assert mirror.current() == {"door": "clear"} and changes[-1] == {"door": "clear"}


def test_same_context_pending_keeps_valid_facts() -> None:
    kernel = _Kernel()
    kernel.catalog = _CATALOG_A
    kernel.baseline = _ready(0, {"door": "clear"})
    mirror, _changes = _mirror(kernel)
    mirror.poll()
    kernel.baseline = {"status": "projection_pending"}
    kernel.events = [_event(1, "door", "tripped")]
    mirror.poll()
    assert mirror.current() == {"door": "tripped"}


def test_current_is_context_checked() -> None:
    kernel = _Kernel()
    kernel.catalog = _CATALOG_A
    kernel.baseline = _ready(0, {"door": "inactive"})
    mirror, _changes = _mirror(kernel)
    mirror.poll()
    assert mirror.current(_CONTEXT_W) == {"door": "inactive"}
    assert mirror.current(("kernel-1", 4, "bundle-1", "graph-1")) is None
    assert mirror.current(("kernel-1", 3, "bundle-2", "graph-1")) is None


def test_unknown_catalog_proves_no_retained_states() -> None:
    # SX-2216: a null catalog with a ready cut cannot install it.
    kernel = _Kernel()
    kernel.catalog = None
    kernel.baseline = _ready(0, {"door": "inactive"})
    mirror, changes = _mirror(kernel)
    mirror.poll()
    assert mirror.current() is None and mirror.current(_CONTEXT_W) is None
    assert changes == [None]
    kernel.catalog = _CATALOG_A
    mirror.poll()
    assert mirror.current() == {"door": "inactive"}
    # A valid cut, then a null catalog with pending: unavailable, and the
    # store's declared keys too.
    program = _store()._program.with_interrupt_state_keys(("door",))  # noqa: SLF001
    store = _store(program)
    _state_runtime(mirror, store)._apply_interrupt_states(mirror.current())  # noqa: SLF001
    assert store.read("interrupt_state.door") == "inactive"
    kernel.catalog = None
    kernel.baseline = {"status": "projection_pending"}
    mirror.poll()
    assert mirror.current() is None and changes[-1] is None
    _state_runtime(mirror, store)._apply_interrupt_states(changes[-1])  # noqa: SLF001
    with pytest.raises(KeyError):
        store.read("interrupt_state.door")
    # The proven same-context catalog with pending keeps nothing stale: it
    # waits for the next ready cut.
    kernel.catalog = _CATALOG_A
    mirror.poll()
    assert mirror.current() is None


def _state_runtime(mirror: InterruptRuleStateMirror, store: Any):
    runtime = _hook_runtime(None)
    runtime._interrupt_state_mirror = mirror  # noqa: SLF001
    runtime._interrupt_state_stores = lambda: [store]  # type: ignore[method-assign]  # noqa: SLF001
    return runtime


def test_seeding_is_context_checked() -> None:
    kernel = _Kernel()
    kernel.catalog = _CATALOG_A
    kernel.baseline = _ready(0, {"door": "inactive"})
    mirror, _changes = _mirror(kernel)
    mirror.poll()
    program = _store()._program.with_interrupt_state_keys(("door",))  # noqa: SLF001
    runtime = _state_runtime(mirror, None)
    same, other = _store(program), _store(program)
    runtime._seed_interrupt_states(same, _CONTEXT_W)  # noqa: SLF001
    runtime._seed_interrupt_states(other, ("kernel-1", 4, "bundle-1", "graph-1"))  # noqa: SLF001
    assert same.read("interrupt_state.door") == "inactive"
    with pytest.raises(KeyError):
        other.read("interrupt_state.door")


def test_known_to_unknown_catalog_makes_declared_keys_unavailable() -> None:
    # SX-2208: an unreadable reply drops the catalog; the store's declared
    # keys become unavailable (never clear, never another id declared).
    kernel = _Kernel()
    kernel.catalog = _CATALOG_A
    kernel.baseline = _ready(0, {"door": "inactive", "zone": "clear"})
    program = _store()._program.with_interrupt_state_keys(("door", "zone"))  # noqa: SLF001
    store = _store(program)
    runtime: Any = None
    mirror = InterruptRuleStateMirror(
        owner_epoch=lambda: kernel.owner, read_events=kernel,
        on_change=lambda states: runtime._apply_interrupt_states(states),  # noqa: SLF001
    )
    runtime = _state_runtime(mirror, store)
    mirror.poll()
    assert store.read("interrupt_state.door") == "inactive"
    assert store.read("interrupt_state.zone") == "clear"
    kernel.fail = HeadlessApiError("execution_event_cursor_unavailable", "gap")
    mirror.poll()
    assert mirror.catalog() is None
    for key in ("interrupt_state.door", "interrupt_state.zone"):
        with pytest.raises(KeyError):
            store.read(key)


# Event subscription (contract §8.1) ------------------------------------------


def _stream(kind: str, owner: str = "kernel-1", **fields: Any) -> dict[str, Any]:
    return {"api_version": "manyforge.execution_event_stream.v1", "owner_epoch": owner,
            "type": kind, **fields}


def _state(baseline: Any, catalog: Any, rows: list[Any] | None = None,
           last_losses: list[Any] | None = None) -> dict[str, Any]:
    return {"interrupt_rule_state_baseline": baseline, "interrupt_rule_catalog": catalog,
            "carried_object_lost_signals": rows or [],
            "carried_object_last_losses": last_losses or []}


def _snapshot(cursor: int, baseline: Any, catalog: Any, **fields: Any) -> dict[str, Any]:
    return _stream("snapshot", cursor=cursor, reason="subscribed",
                   state=_state(baseline, catalog, **fields))


def _streamed(event: dict[str, Any]) -> dict[str, Any]:
    return _stream("event", event=event)


class _StreamHost:
    """Scripted subscriptions (one script per connection; an exception in a
    script drops the link); the follow loop ends when the scripts run out."""

    def __init__(self, *scripts: list[Any]) -> None:
        self.scripts = list(scripts)
        self.subscriptions: list[tuple[int, str | None]] = []

    def subscribe(self, after_cursor: int, owner_epoch: str | None):
        from contextlib import contextmanager

        @contextmanager
        def opened():
            self.subscriptions.append((after_cursor, owner_epoch))
            script = self.scripts.pop(0)

            class _Subscription:
                def receive(self, stop: Any = None) -> Any:
                    if not script:
                        if not host.scripts:
                            stop.set()
                            return None
                        raise HeadlessApiError("execution_owner_unavailable", "closed")
                    item = script.pop(0)
                    if isinstance(item, Exception):
                        raise item
                    return item

            yield _Subscription()

        host = self
        return opened()


def test_notify_rule_state_stream_updates_reserved_key_only() -> None:
    # The catalog identifies the notify rule; its state stream carries the
    # ordinary clear/tripped/clear values. Core tests own the motion effects.
    baseline = _ready(0, {"advisory": "clear"})
    catalog = _proving_catalog(baseline["snapshot"])
    catalog["notify_rule_ids"] = ["advisory"]
    kernel = _Kernel()
    kernel.catalog = catalog
    kernel.baseline = baseline
    host = _StreamHost([
        _snapshot(0, baseline, catalog),
        _streamed(_event(1, "advisory", "tripped")),
        _streamed(_event(2, "advisory", "clear")),
    ])
    runtime = _hook_runtime(None)
    runtime._interrupt_state_stores = lambda: []  # type: ignore[method-assign]  # noqa: SLF001
    mirror = InterruptRuleStateMirror(
        owner_epoch=lambda: kernel.owner, read_events=kernel, subscribe=host.subscribe,
        on_change=runtime._apply_interrupt_states,  # noqa: SLF001
    )
    runtime._interrupt_state_mirror = mirror  # noqa: SLF001
    mirror.poll()  # The existing catalog declares the Applied rule's key.
    program = runtime._with_global_rule_keys(  # noqa: SLF001
        _store().program, retained_bundle_id="bundle-1", graph_revision="graph-1",
    )
    store = _store(program)
    runtime._interrupt_state_stores = lambda: [store]  # type: ignore[method-assign]  # noqa: SLF001
    runtime._seed_interrupt_states(store, _CONTEXT_W)  # noqa: SLF001
    assert store.read("interrupt_state.advisory") == "clear"
    assert store.read("count") == 0 and store.read("door_open") is False

    changes = []
    store.subscribe_published_keys(
        ("count", "door_open", "interrupt_state.advisory"),
        baseline=lambda _rows: None,
        on_change=lambda change: changes.append((change.key, change.value)),
    )
    observed = []
    mirror.follow(
        threading.Event(),
        on_message=lambda _message: observed.append(store.read("interrupt_state.advisory")),
    )
    assert observed == ["clear", "tripped", "clear"]
    assert changes == [
        ("interrupt_state.advisory", "tripped"),
        ("interrupt_state.advisory", "clear"),
    ]
    assert store.read("count") == 0 and store.read("door_open") is False
    with pytest.raises(ProgramValueError):
        store.write("interrupt_state.advisory", "tripped")


def test_shared_stream_callback_sees_each_message_after_interrupt_state() -> None:
    first = _snapshot(0, _ready(0, {"door": "clear"}), _CATALOG_A)
    second = _streamed(_event(1, "door", "tripped"))
    host = _StreamHost([first, second])
    seen = []
    mirror = InterruptRuleStateMirror(
        owner_epoch=lambda: "kernel-1", read_events=_Kernel(),
        subscribe=host.subscribe, on_change=lambda _states: None,
    )
    mirror.follow(
        threading.Event(),
        on_message=lambda message: seen.append((message["type"], mirror.current())),
    )
    assert seen == [("snapshot", {"door": "clear"}), ("event", {"door": "tripped"})]


def test_shared_stream_disconnect_keeps_existing_unavailable_path() -> None:
    first = _snapshot(0, _ready(0, {"door": "clear"}), _CATALOG_A)
    host = _StreamHost([first, HeadlessApiError("execution_owner_unavailable", "closed")], [])
    lost = []
    mirror = InterruptRuleStateMirror(
        owner_epoch=lambda: "kernel-1", read_events=_Kernel(),
        subscribe=host.subscribe, on_change=lambda _states: None,
    )
    mirror.follow(threading.Event(), on_disconnect=lost.append)
    assert len(lost) == 1
    assert mirror.current() is None


def test_stream_applies_snapshot_then_rows_as_they_arrive() -> None:
    rows = [_LOSS_ROW]
    host = _StreamHost([
        _snapshot(0, _ready(2, {"door": "clear", "zone": "clear"}), _CATALOG_A),
        _streamed(_event(1, "door", "inactive")),  # Covered by the cut (2).
        _streamed(_event(3, "door", "tripped")),
        _stream("state", state=_state(_ready(3, {"door": "tripped", "zone": "clear"}),
                                      _CATALOG_A, rows)),
        _stream("heartbeat", cursor=3),
    ])
    changes: list[Any] = []
    delivered: list[Any] = []
    mirror = InterruptRuleStateMirror(
        owner_epoch=lambda: "kernel-1", read_events=_Kernel(), on_change=changes.append,
        subscribe=host.subscribe, on_carried_object_lost=delivered.append,
    )
    mirror.follow(threading.Event())
    assert changes == [{"door": "clear", "zone": "clear"}, {"door": "tripped", "zone": "clear"}]
    # Rows on snapshot, state and heartbeat; unchanged rows are re-offered.
    assert delivered == [(), tuple(rows), tuple(rows)]
    assert mirror.carried_object_lost_signals() == tuple(rows)
    assert host.subscriptions == [(0, None)]


_LOSS_ROW = {
    "carried_object_attachment_id": "a-1", "command_target_id": "gripper",
    "controllable_component_id": "robot", "lost": True, "loss_event_id": "loss.1",
    "scene_resource_id": "box", "set_key": "part_lost",
    "signal_name": "carried_object_lost.gripper",
}


@pytest.mark.parametrize("bad", [
    {**_LOSS_ROW, "lost": False},
    {**_LOSS_ROW, "signal_name": "carried_object_lost.other"},
    {**_LOSS_ROW, "extra": 1},
    {key: value for key, value in _LOSS_ROW.items() if key != "scene_resource_id"},
    {**_LOSS_ROW, "command_target_id": ""},
    {**_LOSS_ROW, "loss_event_id": ""},
    {**_LOSS_ROW, "set_key": ""},
    {**_LOSS_ROW, "set_key": 3},
    {key: value for key, value in _LOSS_ROW.items() if key != "loss_event_id"},
    "row",
])
def test_invalid_loss_rows_are_never_offered(bad: Any) -> None:
    delivered: list[Any] = []
    mirror = InterruptRuleStateMirror(
        owner_epoch=lambda: "kernel-1", read_events=_Kernel(), on_change=lambda _s: None,
        on_carried_object_lost=delivered.append,
    )
    mirror.apply_stream_message(_snapshot(0, _ready(0, {"door": "clear"}), _CATALOG_A,
                                          rows=[_LOSS_ROW, bad]))
    assert mirror.carried_object_lost_signals() is None and delivered == []
    assert mirror.current() == {"door": "clear"}  # The rule states are unaffected.


_LAST_LOSS = {"controllable_component_id": "robot", "command_target_id": "gripper",
              "set_key": "part_lost", "loss_event_id": "loss.1"}


def test_last_losses_come_with_every_snapshot_and_state() -> None:
    # §10 "Coalescing": a loss cleared before any read (no row) still reaches
    # the Runner through its last loss; a reconnect's snapshot carries it.
    mirror, _changes = _mirror(_Kernel())
    assert mirror.carried_object_last_losses() is None  # Unknown before a snapshot.
    mirror.apply_stream_message(_snapshot(0, _ready(0, {}), _CATALOG_A, last_losses=[_LAST_LOSS]))
    assert mirror.carried_object_lost_signals() == ()
    assert mirror.carried_object_last_losses() == (_LAST_LOSS,)
    later = {**_LAST_LOSS, "loss_event_id": "loss.2"}
    other_key = {**_LAST_LOSS, "set_key": "other_lost", "loss_event_id": "loss.3"}
    mirror.apply_stream_message(_stream("state", state=_state(_ready(0, {}), _CATALOG_A,
                                                              last_losses=[later, other_key])))
    assert mirror.carried_object_last_losses() == (later, other_key)
    mirror.apply_stream_message(_stream("gap", "kernel-2", reason="owner_epoch_changed"))
    assert mirror.carried_object_last_losses() is None


@pytest.mark.parametrize("bad", [
    [{**_LAST_LOSS, "set_key": None}],
    [{**_LAST_LOSS, "loss_event_id": ""}],
    [{**_LAST_LOSS, "extra": 1}],
    [_LAST_LOSS, {**_LAST_LOSS, "loss_event_id": "loss.9"}],  # Same gripper and key twice.
    "entries",
])
def test_invalid_last_losses_make_the_loss_state_unknown(bad: Any) -> None:
    delivered: list[Any] = []
    mirror = InterruptRuleStateMirror(
        owner_epoch=lambda: "kernel-1", read_events=_Kernel(), on_change=lambda _s: None,
        on_carried_object_lost=delivered.append,
    )
    state = _state(_ready(0, {"door": "clear"}), _CATALOG_A, rows=[_LOSS_ROW])
    state["carried_object_last_losses"] = bad
    mirror.apply_stream_message(_stream("snapshot", cursor=0, reason="subscribed", state=state))
    assert mirror.carried_object_last_losses() is None
    assert mirror.carried_object_lost_signals() is None and delivered == []
    assert mirror.current() == {"door": "clear"}


def test_a_row_without_a_key_is_valid() -> None:
    delivered: list[Any] = []
    mirror = InterruptRuleStateMirror(
        owner_epoch=lambda: "kernel-1", read_events=_Kernel(), on_change=lambda _s: None,
        on_carried_object_lost=delivered.append,
    )
    row = {**_LOSS_ROW, "set_key": None}
    mirror.apply_stream_message(_snapshot(0, _ready(0, {}), _CATALOG_A, rows=[row]))
    assert delivered == [(row,)]


def test_rows_are_unknown_after_a_gap_or_a_lost_stream_until_the_next_snapshot() -> None:
    delivered: list[Any] = []
    mirror = InterruptRuleStateMirror(
        owner_epoch=lambda: "kernel-1", read_events=_Kernel(), on_change=lambda _s: None,
        on_carried_object_lost=delivered.append,
    )
    mirror.apply_stream_message(_snapshot(0, _ready(0, {}), _CATALOG_A, rows=[_LOSS_ROW]))
    assert delivered == [(_LOSS_ROW,)]
    mirror.apply_stream_message(_stream("gap", "kernel-2", reason="owner_epoch_changed"))
    assert mirror.carried_object_lost_signals() is None
    assert delivered == [(_LOSS_ROW,)]  # Nothing offered on the gap.
    mirror.apply_stream_message(_stream("snapshot", "kernel-2", cursor=0, reason="owner_epoch_changed",
                                        state=_state(None, None)))
    assert delivered[-1] == () and mirror.carried_object_lost_signals() == ()
    mirror._stream_lost(RuntimeError("dead link"))  # noqa: SLF001
    assert mirror.carried_object_lost_signals() is None
    mirror.apply_stream_message(_stream("heartbeat", "kernel-2", cursor=0))
    assert delivered[-1] == ()  and len(delivered) == 2  # Still nothing until a snapshot.


def test_no_lock_is_held_while_the_stream_waits() -> None:
    # SX-191: a quiet stream (no rows, no heartbeat yet) blocks only its own
    # receive; the mirror's readers and Stop settlement stay responsive.
    import time

    receiving = threading.Event()

    class _Quiet:
        def __init__(self) -> None:
            self.script = [_snapshot(0, _ready(0, {"door": "clear"}), _CATALOG_A)]

        def receive(self, stop: Any = None) -> Any:
            if self.script:
                return self.script.pop(0)
            receiving.set()
            stop.wait()
            return None

    from contextlib import contextmanager

    @contextmanager
    def subscribe(after_cursor: int, owner_epoch: str | None):
        yield _Quiet()

    mirror = InterruptRuleStateMirror(
        owner_epoch=lambda: "kernel-1", read_events=_Kernel(), on_change=lambda _s: None,
        subscribe=subscribe,
    )
    stop = threading.Event()
    follower = threading.Thread(target=mirror.follow, args=(stop,))
    follower.start()
    try:
        assert receiving.wait(2.0)
        started = time.monotonic()
        assert mirror.current() == {"door": "clear"}
        assert mirror.catalog() is not None
        assert mirror.stop_terminal("stop-1", 1) is None
        assert mirror.carried_object_lost_signals() == ()
        assert time.monotonic() - started < 0.1
    finally:
        stop.set()
        follower.join(1.0)
    assert not follower.is_alive()


def test_dead_link_is_unavailable_until_the_reconnect_snapshot() -> None:
    host = _StreamHost(
        [_snapshot(0, _ready(0, {"door": "inactive"}), _CATALOG_A), _streamed(_other(4)),
         HeadlessApiError("execution_event_stream_dead", "no heartbeat")],
        [_snapshot(4, _ready(0, {"door": "inactive"}), _CATALOG_A)],
    )
    changes: list[Any] = []
    mirror = InterruptRuleStateMirror(
        owner_epoch=lambda: "kernel-1", read_events=_Kernel(), on_change=changes.append,
        subscribe=host.subscribe,
    )
    mirror.follow(threading.Event())
    assert changes == [{"door": "inactive"}, None, {"door": "inactive"}]
    assert host.subscriptions == [(0, None), (4, "kernel-1")]  # Resumed.


def test_streamed_row_is_applied_once_also_after_a_poll_read_it() -> None:
    kernel = _Kernel()
    kernel.catalog = _CATALOG_A
    kernel.baseline = _ready(0, {"door": "clear"})
    kernel.events = [_event(1, "door", "tripped")]
    changes: list[Any] = []
    mirror = InterruptRuleStateMirror(
        owner_epoch=lambda: kernel.owner, read_events=kernel, on_change=changes.append,
    )
    mirror.poll()  # A Run start's one-shot read applies row 1.
    assert mirror.current() == {"door": "tripped"}
    mirror.apply_stream_message(_stream("state", state=_state(_ready(0, {"door": "clear"}),
                                                              _CATALOG_A)))
    mirror.apply_stream_message(_streamed(_event(1, "door", "inactive")))  # Same sequence.
    assert mirror.current() == {"door": "tripped"}
    mirror.apply_stream_message(_streamed(_event(2, "door", "clear")))
    assert mirror.current() == {"door": "clear"}


def test_owner_change_snapshot_recaptures_and_a_gap_is_no_error() -> None:
    mirror, changes = _mirror(_Kernel())
    mirror.apply_stream_message(_snapshot(0, _ready(0, {"door": "inactive"}), _CATALOG_A))
    mirror.apply_stream_message(_stream("gap", "kernel-2", reason="owner_epoch_changed"))
    catalog_b = {**_CATALOG_A, "owner_epoch": "kernel-2"}
    mirror.apply_stream_message(_stream(
        "snapshot", "kernel-2", cursor=0, reason="owner_epoch_changed",
        state=_state(_ready(0, {"door": "clear"}, owner_epoch="kernel-2"), catalog_b),
    ))
    assert changes == [{"door": "inactive"}, {"door": "clear"}]


def test_malformed_stream_row_recaptures_and_raises() -> None:
    mirror, changes = _mirror(_Kernel())
    mirror.apply_stream_message(_snapshot(0, _ready(0, {"door": "inactive"}), _CATALOG_A))
    bad = _event(1, "door", "inactive")
    bad["payload"]["state"] = "melted"
    with pytest.raises(Exception):
        mirror.apply_stream_message(_streamed(bad))
    assert changes == [{"door": "inactive"}, None]


def test_pending_source_keeps_its_original_deadline(monkeypatch) -> None:
    # SX-2209: watchdog retries never extend the same source's budget.
    import manyforge_program_runner.runtime as runtime_module

    runtime = _hook_runtime(None)
    publisher = _Publisher(3)
    runtime._bbkey_publishers["run"] = publisher  # noqa: SLF001
    now = [1_000]
    monkeypatch.setattr(runtime_module.time, "monotonic_ns", lambda: now[0])
    runtime._start_or_defer_bbkey_source("run", publisher, object(), ("count",), _CONTEXT_W)  # noqa: SLF001
    deadline = 1_000 + runtime._bbkey_budget_ns()  # noqa: SLF001
    for step in range(1, 5):
        now[0] = 1_000 + step * 1_000_000_000
        runtime._retry_pending_bbkey_sources()  # noqa: SLF001
        assert publisher.failed is None
        assert runtime._bbkey_pending["run"][4] == deadline  # noqa: SLF001
    now[0] = deadline
    runtime._retry_pending_bbkey_sources()  # noqa: SLF001
    assert "publication 3 was not installed" in (publisher.failed or "")
    assert runtime._bbkey_pending == {}  # noqa: SLF001


# Stop terminal detail through the existing events consumption (SX-2215) ----


def _attempt(stage_id: str, index: int) -> dict[str, Any]:
    subject = {"kind": "participant", "operation_id": "op", "participant_id": "arm"}
    return {
        "authority": "authoritative", "error": None, "operation_id": "op",
        "outcome": "succeeded", "participant_id": "arm",
        "participant_stop_invocation_plan_digest": "plan-arm",
        "scope_key": {"origin_stop_generation": 1, "origin_stop_id": "stop", "subject": subject},
        "stage": {"stage_id": stage_id, "stage_index": index},
    }


def _stop_terminal_event(sequence: int, **patch: Any) -> dict[str, Any]:
    payload = {
        "api_version": "manyforge.stop_terminal_projection.v1",
        "owner_epoch": "kernel-1",
        "details_truncated": False,
        "stop_id": "stop",
        "stop_generation": 1,
        "stop_certificate_id": "cert",
        "stop_certificate_revision": 1,
        "attempted_stages": [_attempt("controlled_stop", 0), _attempt("controller_goal_cancel", 1)],
        "participant_settlement": [{
            "scope_key": {"subject": {"kind": "participant", "operation_id": "op",
                                      "participant_id": "arm"}},
            "participant_stop_invocation_plan_digest": "plan-arm",
        }],
        **patch,
    }
    return {"sequence": sequence, "record_kind": "stop_terminal_projected", "payload": payload}


def _escalated_operation(**row_patch: Any) -> dict[str, Any]:
    row = {
        "registration": {"owner_epoch": "kernel-1", "policy_id": "door_rule",
                         "scope": {"kind": "operation", "operation_id": "op"}},
        "evidence": {"state": "retired", "trip": {"triggering_acquisition": None},
                     "source_losses": [], "evaluation_error": None},
        "stop_id": "stop", "stop_generation": 1,
        "stop_certificate_id": "cert", "stop_certificate_revision": 1,
        "stop_result": "succeeded_escalated", "terminal_cause": "signal_trip", "error": None,
        **row_patch,
    }
    return {"operation_id": "op", "participants": [{"participant_id": "arm",
                                                     "io_rule_outcomes": [row]}]}


def _map_with_events(
    events: list[dict[str, Any]],
    operation: dict[str, Any],
    projection: Any = "recorded",
    *,
    late: list[dict[str, Any]] = (),
):
    """The port over the mirror: the events seen before the terminal
    observation, then (``late``) rows committed before read_stop said
    ``projection``."""

    from types import SimpleNamespace

    from manyforge_behavior.interrupt_outcome import (
        contained_stages_from_stop_terminal,
        map_interrupt_result,
    )
    from manyforge_behavior.production_ports import NativeKernelProgramPort

    kernel = _Kernel()
    kernel.events = list(events)
    mirror, _changes = _mirror(kernel)
    mirror.poll()  # The existing cadence read before the terminal observation.
    status: dict[str, Any] = {"stop_id": "stop", "stop_generation": 1, "state": "succeeded"}
    if projection is not _ABSENT:
        status["terminal_projection"] = projection
    reads: list[int] = []

    def stop_status(stop_id: str) -> dict[str, Any]:
        reads.append(len(kernel.reads))
        kernel.events.extend(late)
        return {"ok": True, "stop_status": dict(status)}

    port = object.__new__(NativeKernelProgramPort)
    port._stop_terminal = mirror.stop_terminal  # noqa: SLF001
    port._kernel = SimpleNamespace(stop_status=stop_status)  # noqa: SLF001
    terminal, pending = port._interrupt_stop_terminal(operation)  # noqa: SLF001
    row = operation["participants"][0]["io_rule_outcomes"][0]
    contained = (
        None if terminal is None
        else contained_stages_from_stop_terminal(row, terminal, operation_id="op")
    )
    result = map_interrupt_result(
        "motion.move_until_signal", {"success_through_stage_id": "controller_goal_cancel"},
        operation, stop_chain=("controlled_stop", "controller_goal_cancel", "software_hard_stop"),
        contained_stage_id=contained,
    )
    return result, terminal, pending, reads, kernel


_ABSENT = object()


def test_interrupt_reads_the_matching_stop_terminal_from_the_events_stream() -> None:
    # Already read on the existing cadence: no read_stop.
    result, terminal, pending, reads, _kernel = _map_with_events(
        [_other(1), _stop_terminal_event(2)], _escalated_operation())
    assert terminal is not None and terminal["stop_id"] == "stop" and not pending
    assert (result.success, result.node_outcome) == (True, "signal_trip")
    assert reads == []


def test_recorded_reads_the_events_after_read_stop_to_the_record() -> None:
    # SX-2226: the row committed after the last cadence read, more than one
    # page later; it is read after observing recorded, to the record.
    late = [_other(sequence) for sequence in range(2, 40)] + [_stop_terminal_event(40)]
    result, terminal, pending, reads, kernel = _map_with_events(
        [_other(1)], _escalated_operation(), late=late)
    assert terminal is not None and not pending
    assert (result.success, result.node_outcome) == (True, "signal_trip")
    assert reads == [1] and len(kernel.reads) > 2  # Read only after read_stop.


def test_pending_projection_waits_without_reading_events() -> None:
    result, terminal, pending, reads, kernel = _map_with_events(
        [_other(1)], _escalated_operation(), "pending")
    assert terminal is None and pending and reads == [1] and len(kernel.reads) == 1


def test_nominal_or_ordinary_results_never_wait_for_the_stop_terminal() -> None:
    for patch in ({"stop_result": "succeeded_nominal"}, {"terminal_cause": "stopped"}):
        _result, terminal, pending, reads, _kernel = _map_with_events(
            [_other(1)], _escalated_operation(**patch), "pending")
        assert (terminal, pending, reads) == (None, False, [])


@pytest.mark.parametrize(
    "events, row_patch, projection",
    [
        # Recorded but no longer retained / never read: unproven.
        ([_other(1)], {}, "recorded"),
        # Failed is never proof of absence nor of containment; missing or
        # unknown projection values are not pending.
        ([_other(1)], {}, "failed"),
        ([_other(1)], {}, _ABSENT),
        ([_other(1)], {}, "not_applicable"),
        # Another Stop identity or generation.
        ([_stop_terminal_event(1, stop_id="stop-2")], {}, "recorded"),
        ([_stop_terminal_event(1, stop_generation=2)], {}, "recorded"),
        ([_stop_terminal_event(1)], {"stop_generation": 2}, "recorded"),
        # Another Stop certificate revision or another owner's row.
        ([_stop_terminal_event(1, stop_certificate_revision=2)], {}, "recorded"),
        ([_stop_terminal_event(1, owner_epoch="kernel-9")], {}, "recorded"),
        # Over the byte bound: details truncated, attempts omitted.
        ([_stop_terminal_event(1, details_truncated=True, attempted_stages=[])], {}, "recorded"),
    ],
)
def test_unresolved_stop_terminal_keeps_the_containing_stage_unproven(
    events, row_patch, projection
) -> None:
    result, _terminal, pending, _reads, _kernel = _map_with_events(
        events, _escalated_operation(**row_patch), projection)
    assert not pending
    assert (result.success, result.reason_code) == (
        False, "motion_interrupt_containing_stage_unproven"
    )


def test_stop_terminals_are_bounded_and_cleared_with_the_owner() -> None:
    from manyforge_program_runner.interrupt_state_mirror import MAXIMUM_STOP_TERMINALS

    kernel = _Kernel()
    kernel.events = [
        _stop_terminal_event(index + 1, stop_generation=index + 1)
        for index in range(MAXIMUM_STOP_TERMINALS + 1)
    ]
    mirror, _changes = _mirror(kernel)
    mirror.poll()
    reads = len(kernel.reads)
    assert mirror.stop_terminal("stop", MAXIMUM_STOP_TERMINALS + 1) is not None
    assert len(kernel.reads) == reads  # Retained: no extra read.
    assert mirror.stop_terminal("stop", 1) is None  # Oldest dropped.
    kernel.owner, kernel.events = "kernel-2", []
    mirror.poll()
    assert mirror.stop_terminal("stop", MAXIMUM_STOP_TERMINALS + 1) is None


_WAITING = {
    "status": "waiting_for_program", "owner_epoch": "kernel-1",
    "active_publication_generation": 3, "bundle": "bundle-1", "graph": "graph-1",
    "rule_ids": ["door", "zone"],
    "waiting_for_program": [
        {"rule_id": "door", "keys": ["door_open"]},
        {"rule_id": "zone", "keys": ["count", "zone_clear"]},
    ],
}


def test_waiting_for_program_catalog_parses_strictly() -> None:
    # Decision 110: no catalog_generation, binding or source; baseline pending.
    waiting = parse_catalog(_WAITING)
    assert waiting == RuleCatalog(
        "kernel-1", None, 3, "bundle-1", "graph-1", ("door", "zone"), (),
        (("door", ("door_open",)), ("zone", ("count", "zone_clear"))),
    )
    assert waiting.context == _CONTEXT_W and waiting.waiting_rule_ids == {"door", "zone"}
    rows = _WAITING["waiting_for_program"]
    for malformed in (
        {**_WAITING, "catalog_generation": "4"},
        {**_WAITING, "referenced_program_blackboard_keys": []},
        {key: value for key, value in _WAITING.items() if key != "waiting_for_program"},
        {**_WAITING, "rule_ids": ["door", "door"]},
        {**_WAITING, "rule_ids": []},
        {**_WAITING, "waiting_for_program": []},
        {**_WAITING, "waiting_for_program": [{"rule_id": "gate", "keys": ["k"]}]},
        {**_WAITING, "waiting_for_program": rows + rows},
        {**_WAITING, "waiting_for_program": [{"rule_id": "zone", "keys": []}]},
        {**_WAITING, "waiting_for_program": [{"rule_id": "zone", "keys": ["k", "k"]}]},
        {**_WAITING, "waiting_for_program": [{"rule_id": "zone", "keys": [""]}]},
        {**_WAITING, "waiting_for_program": [{"rule_id": "zone", "keys": ["k"], "x": 1}]},
        {**_WAITING, "active_publication_generation": 0},
        {**_WAITING, "bundle": ""},
    ):
        with pytest.raises(InterruptStateWireError):
            parse_catalog(malformed)


def test_waiting_catalog_requires_every_rule_waiting() -> None:
    # Decision 110: Core waits the whole catalog; a subset is not this product.
    rows = _WAITING["waiting_for_program"]
    for subset in ([rows[0]], [rows[1]]):
        with pytest.raises(InterruptStateWireError, match="every rule"):
            parse_catalog({**_WAITING, "waiting_for_program": subset})
    single = {**_WAITING, "rule_ids": ["zone"], "waiting_for_program": [rows[1]]}
    assert parse_catalog(single).waiting_rule_ids == {"zone"}


def test_waiting_rows_and_rule_ids_are_canonical_sorted() -> None:
    # Like the ready catalog's rule_ids/keys and the baseline states rows.
    rows = _WAITING["waiting_for_program"]
    for unsorted in (
        {**_WAITING, "rule_ids": ["zone", "door"]},
        {**_WAITING, "waiting_for_program": [rows[1], rows[0]]},
        {**_WAITING, "waiting_for_program": [
            rows[0], {"rule_id": "zone", "keys": ["zone_clear", "count"]},
        ]},
        {**_CATALOG, "waiting_for_program": [
            {"rule_id": "zone", "keys": ["zone_clear", "count"]},
        ]},
        {**_CATALOG, "waiting_for_program": [
            {"rule_id": "zone", "keys": ["k"]}, {"rule_id": "door", "keys": ["k"]},
        ]},
    ):
        with pytest.raises(InterruptStateWireError, match="sorted"):
            parse_catalog(unsorted)


def test_ready_catalog_accepts_the_optional_waiting_annotation() -> None:
    assert parse_catalog(_CATALOG).waiting_for_program == ()
    annotated = parse_catalog(
        {**_CATALOG, "waiting_for_program": [{"rule_id": "zone", "keys": ["zone_clear"]}]}
    )
    assert annotated.waiting_for_program == (("zone", ("zone_clear",)),)
    # A ready catalog's annotation may name only some of its rules.
    assert annotated.rule_ids == ("door", "zone") and annotated.waiting_rule_ids == {"zone"}
    assert parse_catalog({**_CATALOG, "waiting_for_program": []}).waiting_for_program == ()
    for rows in (
        [{"rule_id": "gate", "keys": ["k"]}],
        [{"rule_id": "zone", "keys": []}],
        [{"rule_id": "zone"}],
        {"zone": ["k"]},
    ):
        with pytest.raises(InterruptStateWireError):
            parse_catalog({**_CATALOG, "waiting_for_program": rows})


def test_waiting_catalog_keeps_no_states_and_declares_unavailable_keys() -> None:
    kernel = _Kernel()
    kernel.catalog = _CATALOG_A
    kernel.baseline = _ready(0, {"door": "inactive", "zone": "clear"})
    program = _store()._program.with_interrupt_state_keys(("door", "zone"))  # noqa: SLF001
    store = _store(program)
    runtime: Any = None
    mirror = InterruptRuleStateMirror(
        owner_epoch=lambda: kernel.owner, read_events=kernel,
        on_change=lambda states: runtime._apply_interrupt_states(states),  # noqa: SLF001
    )
    runtime = _state_runtime(mirror, store)
    mirror.poll()
    assert store.read("interrupt_state.zone") == "clear"
    # The Program goes away: the whole catalog waits, no state retained.
    kernel.catalog = _WAITING
    kernel.baseline = {"status": "projection_pending"}
    mirror.poll()
    assert mirror.catalog() == parse_catalog(_WAITING)
    assert mirror.current() is None and mirror.current(_CONTEXT_W) is None
    for key in ("interrupt_state.door", "interrupt_state.zone"):
        with pytest.raises(KeyError):
            store.read(key)
    # Its rule ids stay declared for the same context; the source sends none.
    hooks = _hook_runtime(parse_catalog(_WAITING))
    assert hooks._applied_global_rule_ids(  # noqa: SLF001
        retained_bundle_id="bundle-1", graph_revision="graph-1") == ("door", "zone")
    assert hooks._catalog_for_context(_CONTEXT_W).referenced_program_blackboard_keys == ()  # noqa: SLF001
    # Another owner's waiting catalog is refused like any other.
    kernel.catalog = {**_WAITING, "owner_epoch": "kernel-2"}
    mirror.poll()
    assert mirror.catalog() is None


def test_ready_catalog_waiting_rule_is_unavailable_never_clear() -> None:
    kernel = _Kernel()
    kernel.catalog = {
        **_CATALOG_A, "waiting_for_program": [{"rule_id": "zone", "keys": ["zone_clear"]}],
    }
    kernel.baseline = _ready(0, {"door": "inactive", "zone": "clear"})
    mirror, changes = _mirror(kernel)
    mirror.poll()
    assert mirror.current() == {"door": "inactive"} and changes == [{"door": "inactive"}]
    program = _store()._program.with_interrupt_state_keys(("door", "zone"))  # noqa: SLF001
    store = _store(program)
    _state_runtime(mirror, store)._seed_interrupt_states(store, _CONTEXT_W)  # noqa: SLF001
    assert store.read("interrupt_state.door") == "inactive"
    with pytest.raises(KeyError):
        store.read("interrupt_state.zone")
    # Once it no longer waits, its current state is available again.
    kernel.catalog = _CATALOG_A
    mirror.poll()
    assert mirror.current() == {"door": "inactive", "zone": "clear"}


def test_notify_pending_and_unknown_invalidate_real_store_then_recover() -> None:
    kernel = _Kernel()
    catalog = {**_CATALOG_A, "notify_rule_ids": ["door"]}
    kernel.catalog = catalog
    kernel.baseline = _ready(0, {"door": "tripped", "zone": "inactive"})
    program = _store()._program.with_interrupt_state_keys(("door", "zone"))  # noqa: SLF001
    store = _store(program)
    runtime: Any = None
    mirror = InterruptRuleStateMirror(
        owner_epoch=lambda: kernel.owner, read_events=kernel,
        on_change=lambda states: runtime._apply_interrupt_states(states),  # noqa: SLF001
    )
    runtime = _state_runtime(mirror, store)
    mirror.poll()
    assert store.read("interrupt_state.door") == "tripped"
    kernel.baseline = {"status": "projection_pending"}
    kernel.events = [_event(1, "door", "tripped"), _event(2, "zone", "clear")]
    mirror.poll()
    with pytest.raises(KeyError):
        store.read("interrupt_state.door")
    assert store.read("interrupt_state.zone") == "clear"
    unknown = _ready(2, {"door": "clear", "zone": "clear"})
    unknown["snapshot"]["states"][0].update(
        state="unknown", triggering_acquisition=None,
        unavailable_reason={"code": "io_rule_source_unavailable", "message": "lost", "details": {}},
    )
    kernel.baseline = unknown
    mirror.poll()
    with pytest.raises(KeyError):
        store.read("interrupt_state.door")
    assert store.read("interrupt_state.zone") == "clear"
    kernel.baseline = {"status": "projection_pending"}
    kernel.events.append(_event(3, "door", "tripped"))
    mirror.poll()
    with pytest.raises(KeyError):
        store.read("interrupt_state.door")
    kernel.baseline = _ready(3, {"door": "tripped", "zone": "clear"})
    mirror.poll()
    assert store.read("interrupt_state.door") == "tripped"
    with pytest.raises(ProgramValueError):
        store.write("interrupt_state.door", "clear")


def test_notify_stream_invalidates_pending_and_restores_same_context_cut() -> None:
    catalog = {**_CATALOG_A, "notify_rule_ids": ["door"]}
    program = _store()._program.with_interrupt_state_keys(("door", "zone"))  # noqa: SLF001
    store = _store(program)
    runtime: Any = None
    mirror = InterruptRuleStateMirror(
        owner_epoch=lambda: "kernel-1", read_events=_Kernel(),
        on_change=lambda states: runtime._apply_interrupt_states(states),  # noqa: SLF001
    )
    runtime = _state_runtime(mirror, store)
    mirror.apply_stream_message(_snapshot(0, _ready(0, {"door": "tripped", "zone": "inactive"}), catalog))
    mirror.apply_stream_message(_stream("state", state=_state({"status": "projection_pending"}, catalog)))
    mirror.apply_stream_message(_streamed(_event(1, "door", "tripped")))
    mirror.apply_stream_message(_streamed(_event(2, "zone", "clear")))
    with pytest.raises(KeyError):
        store.read("interrupt_state.door")
    assert store.read("interrupt_state.zone") == "clear"
    mirror.apply_stream_message(_stream("state", state=_state(_ready(2, {"door": "clear", "zone": "clear"}), catalog)))
    assert store.read("interrupt_state.door") == "clear"
    mirror.apply_stream_message(_streamed(_event(3, "door", "tripped")))
    assert store.read("interrupt_state.door") == "tripped"


def test_unknown_wire_requires_typed_reason_and_no_acquisition() -> None:
    payload = _event(1, "door", "unknown")["payload"]
    with pytest.raises(InterruptStateWireError):
        parse_state_event(payload)
    payload["unavailable_reason"] = {"code": "io_watch_bound_exceeded", "message": "", "details": {"limit": "4"}}
    assert parse_state_event(payload)[1:] == ("door", "unknown")
    for change in ({"triggering_acquisition": _ACQUISITION},
                   {"unavailable_reason": {"code": "", "message": "", "details": {}}}):
        with pytest.raises(InterruptStateWireError):
            parse_state_event({**payload, **change})
    with pytest.raises(InterruptStateWireError):
        parse_catalog({**_CATALOG_A, "notify_rule_ids": ["foreign"]})


def test_notify_ready_cut_recovers_when_pending_and_journal_row_were_not_seen() -> None:
    # A bounded journal projection queue may omit a transition. The next ready
    # cut is authoritative even when the stream coalesced the pending state.
    catalog = {**_CATALOG_A, "notify_rule_ids": ["door"]}
    program = _store()._program.with_interrupt_state_keys(("door", "zone"))  # noqa: SLF001
    store = _store(program)
    runtime: Any = None
    mirror = InterruptRuleStateMirror(
        owner_epoch=lambda: "kernel-1", read_events=_Kernel(),
        on_change=lambda states: runtime._apply_interrupt_states(states),  # noqa: SLF001
    )
    runtime = _state_runtime(mirror, store)
    mirror.apply_stream_message(_snapshot(0, _ready(0, {"door": "tripped", "zone": "inactive"}), catalog))
    unknown = _ready(0, {"door": "clear", "zone": "inactive"})
    unknown["snapshot"]["states"][0].update(
        state="unknown", triggering_acquisition=None,
        unavailable_reason={"code": "io_rule_source_unavailable", "message": "lost", "details": {}},
    )
    mirror.apply_stream_message(_stream("state", state=_state(unknown, catalog)))
    with pytest.raises(KeyError):
        store.read("interrupt_state.door")
    assert store.read("interrupt_state.zone") == "inactive"
    mirror.apply_stream_message(_stream("state", state=_state(_ready(0, {"door": "clear", "zone": "inactive"}), catalog)))
    assert store.read("interrupt_state.door") == "clear"


def test_notify_cut_does_not_hide_legacy_rows_still_in_stream() -> None:
    catalog = {**_CATALOG_A, "notify_rule_ids": ["door"]}
    mirror, _ = _mirror(_Kernel())
    mirror.apply_stream_message(_snapshot(0, _ready(0, {"door": "tripped", "zone": "inactive"}), catalog))
    mirror.apply_stream_message(_stream("state", state=_state({"status": "projection_pending"}, catalog)))
    mirror.apply_stream_message(_stream("state", state=_state(_ready(2, {"door": "clear", "zone": "clear"}), catalog)))
    mirror.apply_stream_message(_streamed(_event(1, "door", "tripped")))
    mirror.apply_stream_message(_streamed(_event(2, "zone", "clear")))
    assert mirror.current() == {"door": "clear", "zone": "clear"}


def test_unchanged_notify_ready_cut_covers_delayed_notify_rows() -> None:
    catalog = {**_CATALOG_A, "notify_rule_ids": ["door"]}
    mirror, _ = _mirror(_Kernel())
    mirror.apply_stream_message(_snapshot(0, _ready(0, {"door": "tripped", "zone": "inactive"}), catalog))
    mirror.apply_stream_message(_stream("state", state=_state(_ready(2, {"door": "tripped", "zone": "inactive"}), catalog)))
    mirror.apply_stream_message(_streamed(_event(1, "door", "clear")))
    mirror.apply_stream_message(_streamed(_event(2, "zone", "clear")))
    assert mirror.current() == {"door": "tripped", "zone": "clear"}


def test_older_stream_cut_cannot_undo_later_polled_notify_event() -> None:
    catalog = {**_CATALOG_A, "notify_rule_ids": ["door"]}
    kernel = _Kernel()
    kernel.catalog = catalog
    kernel.baseline = _ready(0, {"door": "clear", "zone": "clear"})
    mirror, _ = _mirror(kernel)
    mirror.apply_stream_message(_snapshot(0, kernel.baseline, catalog))
    kernel.events = [_event(2, "door", "tripped")]
    mirror.poll()
    assert mirror.current()["door"] == "tripped"  # type: ignore[index]
    # The subscription's older ready fact arrived after the poll.
    mirror.apply_stream_message(_stream("state", state=_state(_ready(1, {"door": "clear", "zone": "clear"}), catalog)))
    assert mirror.current()["door"] == "tripped"  # type: ignore[index]
