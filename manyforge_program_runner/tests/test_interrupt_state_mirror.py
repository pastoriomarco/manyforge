"""Global-rule state mirror over the existing events reply."""

from __future__ import annotations

from copy import deepcopy
import threading
from typing import Any

import pytest

from manyforge_behavior.kernel_client import ExecutionCommandError, KernelClient
from manyforge_behavior.orchestration_values import ProgramValueError
from manyforge_headless_api.native import HeadlessApiError
from manyforge_program_runner.bbkey_publication import ProgramBlackboardPublicationScope
from manyforge_program_runner.interrupt_state_mirror import (
    InterruptRuleStateMirror,
    InterruptStateWireError,
    RuleCatalog,
    parse_baseline,
    parse_catalog,
)
from manyforge_program_runner.tests.fake_gateway import RecordingGateway
from manyforge_program_runner.tests.test_bbkey_publication import _store


_CONTEXT = {
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
        "catalog_generation": "4",
        "source_id": "cell.io",
        "record_sequence": "12",
    },
}


def _ready(states: dict[str, str], **context: Any) -> dict[str, Any]:
    return {
        "status": "ready",
        "snapshot": {
            **_CONTEXT,
            **context,
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
        "catalog_generation": snapshot["catalog_generation"],
        "active_publication_generation": snapshot["active_publication_generation"],
        "bundle": snapshot["retained_bundle_id"],
        "graph": snapshot["graph_revision"],
        "rule_ids": sorted(row["rule_id"] for row in snapshot["states"]),
        "referenced_program_blackboard_keys": [],
    }


class _Kernel:
    """The existing events command: the ordered journal. Unless a
    test sets ``catalog``, the reply carries the catalog proving the latest
    ready baseline's context (states need that proof)."""

    def __init__(self) -> None:
        self.events: list[dict[str, Any]] = []
        self.baseline: Any = None
        self.catalog: Any = _AUTO
        self._proving: dict[str, Any] | None = None
        self.reads: list[int] = []
        self.fail: Exception | None = None

    def __call__(self, after: int) -> dict[str, Any]:
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
            "interrupt_rule_state_baseline": deepcopy(self.baseline),
            "interrupt_rule_catalog": deepcopy(catalog),
            "event_page": {
                "events": deepcopy(rows),
                "next_after_sequence": rows[-1]["sequence"] if rows else after,
                "has_more": len(rows) == 16,
                "schema_version": "manyforge.execution_event_page.v3",
            },
        }


def _mirror(kernel: _Kernel) -> tuple[InterruptRuleStateMirror, list[Any]]:
    changes: list[Any] = []
    mirror = InterruptRuleStateMirror(read_events=kernel, on_change=changes.append)
    return mirror, changes


def test_poll_takes_each_ready_baseline_and_never_the_rows() -> None:
    kernel = _Kernel()
    kernel.events = [_event(1, "door", "tripped"), _other(2), _event(3, "door", "clear")]
    kernel.baseline = _ready({"door": "inactive", "zone": "clear"})
    mirror, changes = _mirror(kernel)
    mirror.poll()
    assert mirror.current() == {"door": "inactive", "zone": "clear"}
    # Rows are History only; the next baseline carries the change.
    kernel.events += [_other(4), _event(5, "zone", "tripped")]
    mirror.poll()
    assert mirror.current() == {"door": "inactive", "zone": "clear"}
    kernel.baseline = _ready({"door": "inactive", "zone": "tripped"})
    mirror.poll()
    assert mirror.current() == {"door": "inactive", "zone": "tripped"}
    assert changes == [{"door": "inactive", "zone": "clear"}, {"door": "inactive", "zone": "tripped"}]
    mirror.poll()  # No change, no notification.
    assert len(changes) == 2 and kernel.reads == [0, 3, 5, 5]


def test_pending_is_unavailable_then_the_ready_baseline_installs() -> None:
    kernel = _Kernel()
    kernel.baseline = {"status": "projection_pending"}
    kernel.events = [_event(1, "door", "tripped"), _event(2, "door", "inactive")]
    mirror, changes = _mirror(kernel)
    mirror.poll()
    assert mirror.current() is None and changes == [None]
    kernel.baseline = _ready({"door": "tripped"})
    mirror.poll()
    assert mirror.current() == {"door": "tripped"}
    assert kernel.reads == [0, 2]


def test_null_baseline_means_no_catalog_never_clear() -> None:
    kernel = _Kernel()
    kernel.baseline = _ready({"door": "clear"})
    mirror, changes = _mirror(kernel)
    mirror.poll()
    kernel.baseline = None
    mirror.poll()
    assert mirror.current() is None and changes[-1] is None


def test_context_replacement_recaptures_from_the_new_baseline() -> None:
    kernel = _Kernel()
    kernel.baseline = _ready({"door": "clear"})
    mirror, _changes = _mirror(kernel)
    mirror.poll()
    # A new publication's catalog without its states yet.
    kernel.catalog = {**_proving_catalog(kernel.baseline["snapshot"]),
                      "active_publication_generation": 4}
    kernel.baseline = {"status": "projection_pending"}
    kernel.events = [_event(1, "door", "tripped", active_publication_generation=4)]
    mirror.poll()
    assert mirror.current() is None
    kernel.catalog = _AUTO
    kernel.baseline = _ready({"door": "inactive"}, active_publication_generation=4)
    mirror.poll()
    assert mirror.current() == {"door": "inactive"}


@pytest.mark.parametrize(
    "failure",
    [
        HeadlessApiError("execution_event_cursor_unavailable", "gap"),
        "malformed_baseline",
    ],
)
def test_gap_unreadable_or_malformed_baseline_drops_the_mirror(failure) -> None:
    kernel = _Kernel()
    kernel.baseline = _ready({"door": "clear"})
    mirror, _changes = _mirror(kernel)
    mirror.poll()
    if failure == "malformed_baseline":
        kernel.catalog = _proving_catalog(kernel.baseline["snapshot"])
        kernel.baseline = _ready({"door": "melted"})
    else:
        kernel.fail = failure
    mirror.poll()
    assert mirror.current() is None
    kernel.fail, kernel.catalog = None, _AUTO
    kernel.baseline = _ready({"door": "tripped"})
    mirror.poll()
    assert mirror.current() == {"door": "tripped"}


@pytest.mark.parametrize(
    "baseline",
    [
        {"status": "projection_pending", "snapshot": None},
        {"status": "ready"},
        {"status": "stale", "snapshot": _ready({})["snapshot"]},
        {"status": "ready", "snapshot": {**_ready({})["snapshot"], "extra": 1}},
        {"status": "ready", "snapshot": {**_ready({})["snapshot"], "through_sequence": 0}},
        {"status": "ready", "snapshot": {**_ready({})["snapshot"], "catalog_generation": 4}},
        {"status": "ready", "snapshot": {**_ready({})["snapshot"], "catalog_generation": "04"}},
        {"status": "ready",
         "snapshot": {**_ready({})["snapshot"], "active_publication_generation": 0}},
        {"status": "ready", "snapshot": {**_ready({})["snapshot"], "retained_bundle_id": ""}},
        _ready({"door": "unknown"}),
    ],
)
def test_malformed_baselines_are_refused(baseline) -> None:
    with pytest.raises(InterruptStateWireError):
        parse_baseline(baseline)


def test_baseline_rows_are_sorted_unique_and_acquisitions_exact() -> None:
    ready = _ready({"a": "tripped", "b": "clear"})
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


def test_kernel_client_reads_the_existing_events_command_whole() -> None:
    reply = _Kernel()(0)
    gateway = RecordingGateway({
        **{command: {"ok": True} for command in
           ("identity", "submit", "read", "reset", "acknowledge_completed", "stop")},
        "events": dict(reply),
    })
    client = KernelClient(gateway)
    assert client.read_interrupt_rule_state_events(7) == reply
    # The core adds the envelope; the page is the fixed 16-event page.
    assert gateway.sent == [("events", {"after_sequence": 7, "limit": 16})]
    gateway.replies["events"] = {
        "ok": False, "error": {"code": "execution_event_cursor_unavailable",
                               "message": "gap", "details": {}},
    }
    with pytest.raises(ExecutionCommandError) as error:
        client.read_interrupt_rule_state_events(7)
    assert error.value.code == "execution_event_cursor_unavailable"


_CATALOG = {
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
        "18446744073709551615", 3, "bundle-1", "graph-1",
        ("door", "zone"), ("door_open", "zone_clear"),
    )
    assert mirror.current() is None and kernel.reads == [0]
    assert mirror.catalog() is catalog and kernel.reads == [0]  # No extra read.
    kernel.catalog = None  # Uninstalled.
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
            claim_generation=None,
            program_run_id=None,
            program_binding_id="sha256:" + "d" * 64,
            active_publication_generation=generation,
            cycle_generation=7,
        )
        self.started: tuple[str, ...] | None = None
        self.retired = False
        self.failed: str | None = None
        # Not started yet: nothing in flight, nothing settled.
        self.baseline_in_flight = False
        self.settled = False

    def start(self, store, keys) -> bool:
        self.started = keys
        self.settled = True
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


_CONTEXT_W = (3, "bundle-1", "graph-1")
_VERIFIED_EMPTY = {
    "status": "verified_empty",
    "active_publication_generation": 3, "bundle": "bundle-1", "graph": "graph-1",
}


def test_verified_empty_catalog_parses_strictly() -> None:
    empty = parse_catalog(_VERIFIED_EMPTY)
    assert empty == RuleCatalog(None, 3, "bundle-1", "graph-1", (), ())
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
    runtime.application_start_id = "runner-1"
    assert runtime._start_bbkey_publication(  # noqa: SLF001
        "run", claim_generation=1, program_binding_id="binding",
        active_publication_generation=3, cycle_generation=1, keys=(),
        retained_bundle_id="bundle-1", graph_revision="graph-1",
    ) is None
    assert created == [] and runtime._bbkey_pending == {}  # noqa: SLF001
    # The same with an unknown catalog: a sender waits for its context.
    runtime._interrupt_state_mirror = _CatalogMirror(None)  # noqa: SLF001
    runtime._new_bbkey_publisher = lambda scope: _Publisher(scope.active_publication_generation)  # type: ignore[method-assign]  # noqa: SLF001
    waiting = runtime._start_bbkey_publication(  # noqa: SLF001
        "run", claim_generation=1, program_binding_id="binding",
        active_publication_generation=3, cycle_generation=1, keys=(),
        retained_bundle_id="bundle-1", graph_revision="graph-1",
    )
    assert waiting is not None and waiting.started is None and "run" in runtime._bbkey_pending  # noqa: SLF001


_CATALOG_A = {**_CATALOG, "catalog_generation": "4"}  # Names _CONTEXT exactly.


def test_context_change_proven_by_the_catalog_invalidates_until_its_ready_cut() -> None:
    # Catalog B with pending never keeps A's states current.
    kernel = _Kernel()
    kernel.catalog = _CATALOG_A
    kernel.baseline = _ready({"door": "inactive"})
    mirror, changes = _mirror(kernel)
    mirror.poll()
    assert mirror.current() == {"door": "inactive"}
    kernel.catalog = {**_CATALOG_A, "catalog_generation": "5"}
    kernel.baseline = {"status": "projection_pending"}
    mirror.poll()
    assert mirror.current() is None and changes[-1] is None
    # A cut of A (not what the catalog proves) is not installed either.
    kernel.baseline = _ready({"door": "inactive"})
    mirror.poll()
    assert mirror.current() is None
    kernel.baseline = _ready({"door": "clear"}, catalog_generation="5")
    mirror.poll()
    assert mirror.current() == {"door": "clear"} and changes[-1] == {"door": "clear"}


def test_current_is_context_checked() -> None:
    kernel = _Kernel()
    kernel.catalog = _CATALOG_A
    kernel.baseline = _ready({"door": "inactive"})
    mirror, _changes = _mirror(kernel)
    mirror.poll()
    assert mirror.current(_CONTEXT_W) == {"door": "inactive"}
    assert mirror.current((4, "bundle-1", "graph-1")) is None
    assert mirror.current((3, "bundle-2", "graph-1")) is None


def test_unknown_catalog_proves_no_retained_states() -> None:
    # A null catalog with a ready cut cannot install it.
    kernel = _Kernel()
    kernel.catalog = None
    kernel.baseline = _ready({"door": "inactive"})
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
    kernel.baseline = _ready({"door": "inactive"})
    mirror, _changes = _mirror(kernel)
    mirror.poll()
    program = _store()._program.with_interrupt_state_keys(("door",))  # noqa: SLF001
    runtime = _state_runtime(mirror, None)
    same, other = _store(program), _store(program)
    runtime._seed_interrupt_states(same, _CONTEXT_W)  # noqa: SLF001
    runtime._seed_interrupt_states(other, (4, "bundle-1", "graph-1"))  # noqa: SLF001
    assert same.read("interrupt_state.door") == "inactive"
    with pytest.raises(KeyError):
        other.read("interrupt_state.door")


def test_known_to_unknown_catalog_makes_declared_keys_unavailable() -> None:
    # An unreadable reply drops the catalog; the store's declared
    # keys become unavailable (never clear, never another id declared).
    kernel = _Kernel()
    kernel.catalog = _CATALOG_A
    kernel.baseline = _ready({"door": "inactive", "zone": "clear"})
    program = _store()._program.with_interrupt_state_keys(("door", "zone"))  # noqa: SLF001
    store = _store(program)
    runtime: Any = None
    mirror = InterruptRuleStateMirror(
        read_events=kernel,
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


# Event subscription ----------------------------------------------------------


def _stream(kind: str, **fields: Any) -> dict[str, Any]:
    return {"api_version": "manyforge.execution_event_stream.v4", "type": kind, **fields}


def _state(baseline: Any, catalog: Any, rows: list[Any] | None = None,
           last_losses: list[Any] | None = None,
           program_run_hold: dict[str, Any] | None = None,
           last_stop_event_id: int | None = None,
           stop_progress_sequence: int = 0) -> dict[str, Any]:
    return {"interrupt_rule_state_baseline": baseline, "interrupt_rule_catalog": catalog,
            "carried_object_lost_signals": rows or [],
            "carried_object_last_losses": last_losses or [],
            "program_run_hold": program_run_hold,
            "last_stop_event_id": last_stop_event_id,
            "stop_progress_sequence": stop_progress_sequence}


def _snapshot(cursor: int, baseline: Any, catalog: Any, **fields: Any) -> dict[str, Any]:
    return _stream("snapshot", cursor=cursor, reason="subscribed",
                   state=_state(baseline, catalog, **fields))


def _streamed(event: dict[str, Any]) -> dict[str, Any]:
    return _stream("event", event=event)


class _StreamHost:
    """A scripted core event subscription (``next``): one script per
    connection; the end of a script, or an error in it, is the core's
    ``disconnected`` message before the next script's snapshot. ``stop`` is
    set once the scripts run out, which ends the follow loop. ``resync``
    drops the rest of the current script: ``disconnected``, then the next
    script (its fresh snapshot)."""

    def __init__(self, *scripts: list[Any]) -> None:
        self.scripts = [list(script) for script in scripts]
        self.stop = threading.Event()
        self.resyncs = 0
        self._resynced = False

    def resync(self) -> None:
        self.resyncs += 1
        self._resynced = True

    def next(self, timeout_s: float) -> Any:
        if self._resynced:
            self._resynced = False
            self.scripts.pop(0)
            return {"type": "disconnected", "code": "execution_event_stream_invalid",
                    "message": "resync"}
        while self.scripts:
            script = self.scripts[0]
            if script:
                item = script.pop(0)
                if not isinstance(item, HeadlessApiError):
                    return item
                self.scripts.pop(0)
                return {"type": "disconnected", "code": item.code, "message": str(item)}
            self.scripts.pop(0)
            if self.scripts:
                return {"type": "disconnected", "code": "execution_owner_unavailable",
                        "message": "event stream closed"}
        self.stop.set()
        return None


def test_notify_rule_state_stream_updates_reserved_key_only() -> None:
    # Consecutive state messages carry the ordinary clear/tripped/clear
    # values; the journal rows change nothing. Core tests own the motion
    # effects.
    baseline = _ready({"advisory": "clear"})
    catalog = _proving_catalog(baseline["snapshot"])
    kernel = _Kernel()
    kernel.catalog = catalog
    kernel.baseline = baseline
    host = _StreamHost([
        _snapshot(0, baseline, catalog),
        _stream("state", state=_state(_ready({"advisory": "tripped"}), catalog)),
        _streamed(_event(1, "advisory", "tripped")),
        _stream("state", state=_state(_ready({"advisory": "clear"}), catalog)),
        _streamed(_event(2, "advisory", "clear")),
    ])
    runtime = _hook_runtime(None)
    runtime._interrupt_state_stores = lambda: []  # type: ignore[method-assign]  # noqa: SLF001
    mirror = InterruptRuleStateMirror(
        read_events=kernel, subscription=host,
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
        host.stop,
        on_message=lambda _message: observed.append(store.read("interrupt_state.advisory")),
    )
    assert observed == ["clear", "tripped", "tripped", "clear", "clear"]
    assert changes == [
        ("interrupt_state.advisory", "tripped"),
        ("interrupt_state.advisory", "clear"),
    ]
    assert store.read("count") == 0 and store.read("door_open") is False
    with pytest.raises(ProgramValueError):
        store.write("interrupt_state.advisory", "tripped")


def test_a_trip_inside_one_state_interval_leaves_the_key_unchanged() -> None:
    # Trip and clear both fell between two state reads: the rows reach the
    # mirror, the next state still says clear, and the key is never written.
    program = _store()._program.with_interrupt_state_keys(("door",))  # noqa: SLF001
    store = _store(program)
    runtime: Any = None
    mirror = InterruptRuleStateMirror(
        read_events=_Kernel(),
        on_change=lambda states: runtime._apply_interrupt_states(states),  # noqa: SLF001
    )
    runtime = _state_runtime(mirror, store)
    mirror.apply_stream_message(_snapshot(0, _ready({"door": "clear"}), _CATALOG_A))
    assert store.read("interrupt_state.door") == "clear"
    changes = []
    store.subscribe_published_keys(
        ("interrupt_state.door",),
        baseline=lambda _rows: None,
        on_change=lambda change: changes.append((change.key, change.value)),
    )
    mirror.apply_stream_message(_streamed(_event(1, "door", "tripped")))
    mirror.apply_stream_message(_streamed(_event(2, "door", "clear")))
    mirror.apply_stream_message(_stream("state", state=_state(
        _ready({"door": "clear"}), _CATALOG_A, rows=[_LOSS_ROW])))
    assert store.read("interrupt_state.door") == "clear"
    assert changes == []


def test_a_resubscribe_starts_from_its_snapshot_baseline() -> None:
    host = _StreamHost(
        [_snapshot(0, _ready({"door": "clear"}), _CATALOG_A),
         HeadlessApiError("execution_event_stream_dead", "no heartbeat")],
        [_snapshot(0, _ready({"door": "tripped"}), _CATALOG_A)],
    )
    changes: list[Any] = []
    mirror = InterruptRuleStateMirror(
        read_events=_Kernel(), on_change=changes.append, subscription=host,
    )
    mirror.follow(host.stop)
    assert changes == [{"door": "clear"}, None, {"door": "tripped"}]


def test_shared_stream_callback_sees_each_message_after_interrupt_state() -> None:
    first = _snapshot(0, _ready({"door": "clear"}), _CATALOG_A)
    second = _stream("state", state=_state(_ready({"door": "tripped"}), _CATALOG_A))
    host = _StreamHost([first, second])
    seen = []
    mirror = InterruptRuleStateMirror(
        read_events=_Kernel(),
        subscription=host, on_change=lambda _states: None,
    )
    mirror.follow(
        host.stop,
        on_message=lambda message: seen.append((message["type"], mirror.current())),
    )
    assert seen == [("snapshot", {"door": "clear"}), ("state", {"door": "tripped"})]


def test_shared_stream_disconnect_keeps_existing_unavailable_path() -> None:
    first = _snapshot(0, _ready({"door": "clear"}), _CATALOG_A)
    host = _StreamHost([first, HeadlessApiError("execution_owner_unavailable", "closed")], [])
    lost = []
    mirror = InterruptRuleStateMirror(
        read_events=_Kernel(),
        subscription=host, on_change=lambda _states: None,
    )
    mirror.follow(host.stop, on_disconnect=lost.append)
    assert len(lost) == 1
    assert mirror.current() is None


def test_stream_applies_snapshot_then_each_state_and_never_the_rows() -> None:
    rows = [_LOSS_ROW]
    host = _StreamHost([
        _snapshot(0, _ready({"door": "clear", "zone": "clear"}), _CATALOG_A),
        _streamed(_event(1, "door", "inactive")),
        _streamed(_event(3, "door", "tripped")),
        _stream("state", state=_state(_ready({"door": "tripped", "zone": "clear"}),
                                      _CATALOG_A, rows)),
        _stream("heartbeat", cursor=3),
    ])
    changes: list[Any] = []
    delivered: list[Any] = []
    mirror = InterruptRuleStateMirror(
        read_events=_Kernel(), on_change=changes.append,
        subscription=host, on_carried_object_lost=delivered.append,
    )
    mirror.follow(host.stop)
    assert changes == [{"door": "clear", "zone": "clear"}, {"door": "tripped", "zone": "clear"}]
    # Rows on snapshot, state and heartbeat; unchanged rows are re-offered.
    assert delivered == [(), tuple(rows), tuple(rows)]
    assert mirror.carried_object_lost_signals() == tuple(rows)


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
        read_events=_Kernel(), on_change=lambda _s: None,
        on_carried_object_lost=delivered.append,
    )
    mirror.apply_stream_message(_snapshot(0, _ready({"door": "clear"}), _CATALOG_A,
                                          rows=[_LOSS_ROW, bad]))
    assert mirror.carried_object_lost_signals() is None and delivered == []
    assert mirror.current() == {"door": "clear"}  # The rule states are unaffected.


_LAST_LOSS = {"controllable_component_id": "robot", "command_target_id": "gripper",
              "set_key": "part_lost", "loss_event_id": "loss.1"}


def test_last_losses_come_with_every_snapshot_and_state() -> None:
    # Coalescing: a loss cleared before any read (no row) still reaches
    # the Runner through its last loss; a reconnect's snapshot carries it.
    mirror, _changes = _mirror(_Kernel())
    assert mirror.carried_object_last_losses() is None  # Unknown before a snapshot.
    mirror.apply_stream_message(_snapshot(0, _ready({}), _CATALOG_A, last_losses=[_LAST_LOSS]))
    assert mirror.carried_object_lost_signals() == ()
    assert mirror.carried_object_last_losses() == (_LAST_LOSS,)
    later = {**_LAST_LOSS, "loss_event_id": "loss.2"}
    other_key = {**_LAST_LOSS, "set_key": "other_lost", "loss_event_id": "loss.3"}
    mirror.apply_stream_message(_stream("state", state=_state(_ready({}), _CATALOG_A,
                                                              last_losses=[later, other_key])))
    assert mirror.carried_object_last_losses() == (later, other_key)


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
        read_events=_Kernel(), on_change=lambda _s: None,
        on_carried_object_lost=delivered.append,
    )
    state = _state(_ready({"door": "clear"}), _CATALOG_A, rows=[_LOSS_ROW])
    state["carried_object_last_losses"] = bad
    mirror.apply_stream_message(_stream("snapshot", cursor=0, reason="subscribed", state=state))
    assert mirror.carried_object_last_losses() is None
    assert mirror.carried_object_lost_signals() is None and delivered == []
    assert mirror.current() == {"door": "clear"}


def test_a_row_without_a_key_is_valid() -> None:
    delivered: list[Any] = []
    mirror = InterruptRuleStateMirror(
        read_events=_Kernel(), on_change=lambda _s: None,
        on_carried_object_lost=delivered.append,
    )
    row = {**_LOSS_ROW, "set_key": None}
    mirror.apply_stream_message(_snapshot(0, _ready({}), _CATALOG_A, rows=[row]))
    assert delivered == [(row,)]


def test_rows_are_unknown_after_a_lost_stream_until_the_next_snapshot() -> None:
    delivered: list[Any] = []
    mirror = InterruptRuleStateMirror(
        read_events=_Kernel(), on_change=lambda _s: None,
        on_carried_object_lost=delivered.append,
    )
    mirror.apply_stream_message(_snapshot(0, _ready({}), _CATALOG_A, rows=[_LOSS_ROW]))
    assert delivered == [(_LOSS_ROW,)]
    mirror._stream_lost(RuntimeError("dead link"))  # noqa: SLF001
    assert mirror.carried_object_lost_signals() is None
    mirror.apply_stream_message(_stream("heartbeat", cursor=0))
    assert delivered == [(_LOSS_ROW,)]  # Still nothing until a snapshot.
    mirror.apply_stream_message(_stream("snapshot", cursor=0, reason="subscribed",
                                        state=_state(None, None)))
    assert delivered[-1] == () and mirror.carried_object_lost_signals() == ()


def test_no_lock_is_held_while_the_stream_waits() -> None:
    # A quiet stream (no rows, no heartbeat yet) blocks only its own
    # receive; the mirror's readers and Stop settlement stay responsive.
    import time

    receiving = threading.Event()

    stop = threading.Event()

    class _Quiet:
        def __init__(self) -> None:
            self.script = [_snapshot(0, _ready({"door": "clear"}), _CATALOG_A)]

        def next(self, timeout_s: float) -> Any:
            if self.script:
                return self.script.pop(0)
            receiving.set()
            stop.wait()
            return None

    mirror = InterruptRuleStateMirror(
        read_events=_Kernel(), on_change=lambda _s: None,
        subscription=_Quiet(),
    )
    follower = threading.Thread(target=mirror.follow, args=(stop,))
    follower.start()
    try:
        assert receiving.wait(2.0)
        started = time.monotonic()
        assert mirror.current() == {"door": "clear"}
        assert mirror.catalog() is not None
        assert mirror.carried_object_lost_signals() == ()
        assert time.monotonic() - started < 0.1
    finally:
        stop.set()
        follower.join(1.0)
    assert not follower.is_alive()


def test_dead_link_is_unavailable_until_the_reconnect_snapshot() -> None:
    host = _StreamHost(
        [_snapshot(0, _ready({"door": "inactive"}), _CATALOG_A), _streamed(_other(4)),
         HeadlessApiError("execution_event_stream_dead", "no heartbeat")],
        [_snapshot(4, _ready({"door": "inactive"}), _CATALOG_A)],
    )
    changes: list[Any] = []
    mirror = InterruptRuleStateMirror(
        read_events=_Kernel(), on_change=changes.append,
        subscription=host,
    )
    mirror.follow(host.stop)
    # The core resumes from the cursor (its own tests); the mirror sees the
    # loss and the reconnect's snapshot.
    assert changes == [{"door": "inactive"}, None, {"door": "inactive"}]


def test_with_a_subscription_a_poll_installs_only_a_context_it_lacks() -> None:
    # The stream's states are ordered; a poll read in between must not leave
    # a state the stream never corrects.
    kernel = _Kernel()
    kernel.catalog = _CATALOG_A
    kernel.baseline = _ready({"door": "tripped"})
    changes: list[Any] = []
    mirror = InterruptRuleStateMirror(
        read_events=kernel, on_change=changes.append, subscription=_StreamHost(),
    )
    mirror.apply_stream_message(_snapshot(0, _ready({"door": "clear"}), _CATALOG_A))
    mirror.apply_stream_message(_streamed(_other(7)))
    mirror.poll()
    assert mirror.current() == {"door": "clear"} and kernel.reads == [7]
    mirror.apply_stream_message(_stream("state", state=_state(_ready({"door": "tripped"}),
                                                              _CATALOG_A)))
    assert mirror.current() == {"door": "tripped"}
    # A context the mirror lacks installs from the poll at once.
    kernel.catalog = _AUTO
    kernel.baseline = _ready({"door": "inactive"}, active_publication_generation=4)
    mirror.poll()
    assert mirror.current() == {"door": "inactive"}
    assert changes == [{"door": "clear"}, {"door": "tripped"}, {"door": "inactive"}]


def test_malformed_stream_state_recaptures_and_raises() -> None:
    mirror, changes = _mirror(_Kernel())
    mirror.apply_stream_message(_snapshot(0, _ready({"door": "inactive"}), _CATALOG_A))
    # A malformed row is History's concern only.
    bad = _event(1, "door", "inactive")
    bad["payload"]["state"] = "melted"
    mirror.apply_stream_message(_streamed(bad))
    assert changes == [{"door": "inactive"}]
    with pytest.raises(InterruptStateWireError):
        mirror.apply_stream_message(_stream("state", state=_state(
            _ready({"door": "melted"}), _CATALOG_A)))
    assert changes == [{"door": "inactive"}, None]


def test_a_message_the_mirror_cannot_apply_asks_for_a_fresh_snapshot() -> None:
    # As a lost link: the states are unavailable until the subscription's
    # fresh snapshot; the rest of the old feed is never applied.
    host = _StreamHost(
        [_snapshot(0, _ready({"door": "inactive"}), _CATALOG_A),
         _stream("state", state=_state(_ready({"door": "melted"}), _CATALOG_A)),
         _stream("state", state=_state(_ready({"door": "clear"}), _CATALOG_A))],
        [_snapshot(0, _ready({"door": "tripped"}), _CATALOG_A)],
    )
    changes: list[Any] = []
    lost: list[BaseException] = []
    mirror = InterruptRuleStateMirror(
        read_events=_Kernel(), on_change=changes.append, subscription=host,
    )
    mirror.follow(host.stop, on_disconnect=lost.append)
    assert host.resyncs == 1
    assert changes == [{"door": "inactive"}, None, {"door": "tripped"}]
    assert [getattr(error, "code", None) for error in lost] == [
        "execution_event_stream_invalid"]


def test_a_failing_message_handler_reads_the_message_again() -> None:
    # The core re-reads from before the message not applied (its own tests);
    # here the handler sees it again after the fresh snapshot.
    host = _StreamHost(
        [_snapshot(0, _ready({"door": "clear"}), _CATALOG_A), _streamed(_other(1))],
        [_snapshot(0, _ready({"door": "clear"}), _CATALOG_A), _streamed(_other(1))],
    )
    seen: list[Any] = []

    def on_message(message: Any) -> None:
        seen.append(message["type"])
        if message["type"] == "event" and seen.count("event") == 1:
            raise ValueError("not applied")

    mirror = InterruptRuleStateMirror(
        read_events=_Kernel(), on_change=lambda _states: None, subscription=host,
    )
    mirror.follow(host.stop, on_message=on_message)
    assert host.resyncs == 1
    assert seen == ["snapshot", "event", "snapshot", "event"]


def test_a_disconnected_message_carries_its_typed_code() -> None:
    host = _StreamHost(
        [_snapshot(0, _ready({"door": "clear"}), _CATALOG_A),
         HeadlessApiError("execution_event_stream_dead", "no heartbeat")],
    )
    lost: list[Any] = []
    mirror = InterruptRuleStateMirror(
        read_events=_Kernel(), on_change=lambda _s: None, subscription=host,
    )
    mirror.follow(host.stop, on_disconnect=lost.append)
    assert [(error.code, str(error)) for error in lost] == [
        ("execution_event_stream_dead", "no heartbeat")
    ]
    assert mirror.current() is None


def test_poll_sends_its_request_without_the_lock_and_drops_a_stale_reply() -> None:
    # The events request runs outside the mirror's lock; a stream state
    # applied meanwhile wins over the reply read before it.
    entered, release = threading.Event(), threading.Event()

    class _Blocking(_Kernel):
        def __call__(self, after: int) -> dict[str, Any]:
            entered.set()
            assert release.wait(5.0)
            return super().__call__(after)

    kernel = _Blocking()
    kernel.baseline = _ready({"door": "inactive"}, active_publication_generation=4)
    changes: list[Any] = []
    mirror = InterruptRuleStateMirror(
        read_events=kernel, on_change=changes.append, subscription=_StreamHost(),
    )
    mirror.apply_stream_message(_snapshot(0, _ready({"door": "clear"}), _CATALOG_A))
    poller = threading.Thread(target=mirror.poll)
    poller.start()
    try:
        assert entered.wait(5.0)
        mirror.poll()  # Never concurrent: returns at once.
        assert mirror.current() == {"door": "clear"}  # The lock is free.
        mirror.apply_stream_message(_stream("state", state=_state(
            _ready({"door": "tripped"}), _CATALOG_A)))
    finally:
        release.set()
        poller.join(5.0)
    assert not poller.is_alive()
    assert kernel.reads == [0]
    assert mirror.current() == {"door": "tripped"}
    # Nothing changed during the next poll: its new context installs.
    mirror.poll()
    assert mirror.current() == {"door": "inactive"}
    assert changes == [{"door": "clear"}, {"door": "tripped"}, {"door": "inactive"}]


def test_pending_source_keeps_its_original_deadline(monkeypatch) -> None:
    # Watchdog retries never extend the same source's budget.
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


# Stop event rows on the events page --------------------------------------


def _stop_event_row(sequence: int, row: str) -> dict[str, Any]:
    return {
        "sequence": sequence,
        "record_kind": f"stop_event_{row}",
        "payload": {"schema_version": "manyforge.stop_event.v1", "row": row, "event_id": 7},
    }


def test_stop_event_rows_are_not_read_by_the_mirror() -> None:
    """The interrupt node reads its containing stage from memory (the
    operation's participant Stop state), so the mirror keeps no Stop rows:
    accepted and completed rows on a page or in the stream change nothing,
    nor do rule rows."""

    kernel = _Kernel()
    kernel.baseline = _ready({"door": "clear"})
    kernel.events = [
        _stop_event_row(1, "accepted"),
        _event(2, "door", "tripped"),
        _stop_event_row(3, "completed"),
    ]
    mirror, changes = _mirror(kernel)
    mirror.poll()
    assert mirror.current() == {"door": "clear"}
    assert changes == [{"door": "clear"}]
    assert not hasattr(mirror, "stop_terminal")
    mirror.apply_stream_message(_streamed(_stop_event_row(4, "completed")))
    assert mirror.current() == {"door": "clear"} and len(changes) == 1
    mirror.poll()
    assert kernel.reads[-1] == 4  # Read past, never retained.


_WAITING = {
    "status": "waiting_for_program",
    "active_publication_generation": 3, "bundle": "bundle-1", "graph": "graph-1",
    "rule_ids": ["door", "zone"],
    "waiting_for_program": [
        {"rule_id": "door", "keys": ["door_open"]},
        {"rule_id": "zone", "keys": ["count", "zone_clear"]},
    ],
}


def test_waiting_for_program_catalog_parses_strictly() -> None:
    # No catalog_generation, binding or source; baseline pending.
    waiting = parse_catalog(_WAITING)
    assert waiting == RuleCatalog(
        None, 3, "bundle-1", "graph-1", ("door", "zone"), (),
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
    # Core waits the whole catalog; a subset is not this product.
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
    kernel.baseline = _ready({"door": "inactive", "zone": "clear"})
    program = _store()._program.with_interrupt_state_keys(("door", "zone"))  # noqa: SLF001
    store = _store(program)
    runtime: Any = None
    mirror = InterruptRuleStateMirror(
        read_events=kernel,
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


def test_ready_catalog_waiting_rule_is_unavailable_never_clear() -> None:
    kernel = _Kernel()
    kernel.catalog = {
        **_CATALOG_A, "waiting_for_program": [{"rule_id": "zone", "keys": ["zone_clear"]}],
    }
    kernel.baseline = _ready({"door": "inactive", "zone": "clear"})
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


def test_notify_and_stop_keys_follow_the_baseline_through_unknown() -> None:
    kernel = _Kernel()
    catalog = {**_CATALOG_A}
    kernel.catalog = catalog
    kernel.baseline = _ready({"door": "tripped", "zone": "inactive"})
    program = _store()._program.with_interrupt_state_keys(("door", "zone"))  # noqa: SLF001
    store = _store(program)
    runtime: Any = None
    mirror = InterruptRuleStateMirror(
        read_events=kernel,
        on_change=lambda states: runtime._apply_interrupt_states(states),  # noqa: SLF001
    )
    runtime = _state_runtime(mirror, store)
    mirror.poll()
    assert store.read("interrupt_state.door") == "tripped"
    assert store.read("interrupt_state.zone") == "inactive"
    unknown = _ready({"door": "clear", "zone": "clear"})
    unknown["snapshot"]["states"][0].update(
        state="unknown", triggering_acquisition=None,
        unavailable_reason={"code": "io_rule_source_unavailable", "message": "lost", "details": {}},
    )
    kernel.baseline = unknown
    mirror.poll()
    with pytest.raises(KeyError):
        store.read("interrupt_state.door")
    assert store.read("interrupt_state.zone") == "clear"
    kernel.baseline = _ready({"door": "tripped", "zone": "clear"})
    mirror.poll()
    assert store.read("interrupt_state.door") == "tripped"
    with pytest.raises(ProgramValueError):
        store.write("interrupt_state.door", "clear")


def test_unknown_wire_requires_typed_reason_and_no_acquisition() -> None:
    baseline = _ready({"door": "clear"})
    row = baseline["snapshot"]["states"][0]
    row["state"] = "unknown"
    with pytest.raises(InterruptStateWireError):
        parse_baseline(baseline)
    row["unavailable_reason"] = {"code": "io_watch_bound_exceeded", "message": "", "details": {"limit": "4"}}
    assert parse_baseline(baseline).states == {"door": "unknown"}  # type: ignore[union-attr]
    for change in ({"triggering_acquisition": _ACQUISITION},
                   {"unavailable_reason": {"code": "", "message": "", "details": {}}}):
        broken = deepcopy(baseline)
        broken["snapshot"]["states"][0].update(change)
        with pytest.raises(InterruptStateWireError):
            parse_baseline(broken)


def test_notify_state_stream_recovers_from_unknown() -> None:
    # The next state is authoritative; no journal row is needed.
    catalog = {**_CATALOG_A}
    program = _store()._program.with_interrupt_state_keys(("door", "zone"))  # noqa: SLF001
    store = _store(program)
    runtime: Any = None
    mirror = InterruptRuleStateMirror(
        read_events=_Kernel(),
        on_change=lambda states: runtime._apply_interrupt_states(states),  # noqa: SLF001
    )
    runtime = _state_runtime(mirror, store)
    mirror.apply_stream_message(_snapshot(0, _ready({"door": "tripped", "zone": "inactive"}), catalog))
    unknown = _ready({"door": "clear", "zone": "inactive"})
    unknown["snapshot"]["states"][0].update(
        state="unknown", triggering_acquisition=None,
        unavailable_reason={"code": "io_rule_source_unavailable", "message": "lost", "details": {}},
    )
    mirror.apply_stream_message(_stream("state", state=_state(unknown, catalog)))
    with pytest.raises(KeyError):
        store.read("interrupt_state.door")
    assert store.read("interrupt_state.zone") == "inactive"
    mirror.apply_stream_message(_stream("state", state=_state(_ready({"door": "clear", "zone": "inactive"}), catalog)))
    assert store.read("interrupt_state.door") == "clear"


# Kernel hold facts (§6.3.2): the reader ---------------------------------------


def _hold(**patch: Any) -> dict[str, Any]:
    return {
        "program_run_id": 3, "hold_revision": 9, "frozen": "halt", "frozen_by": 12,
        "all_groups_held": False, "held_by_rule": None, "frozen_kind": "halt",
        "frozen_source_label": "operator.halt", **patch,
    }


def _hold_state(hold: dict[str, Any] | None = None, *, last_stop_event_id: int | None = 12,
                stop_progress_sequence: int = 4) -> dict[str, Any]:
    return _stream("state", state=_state(
        None, None, program_run_hold=hold, last_stop_event_id=last_stop_event_id,
        stop_progress_sequence=stop_progress_sequence))


def _hold_mirror() -> tuple[InterruptRuleStateMirror, list[Any], list[bool]]:
    facts: list[Any] = []
    lock_free: list[bool] = []
    mirror: InterruptRuleStateMirror

    def on_hold(value: Any) -> None:
        # Handed out after the mirror's lock is released.
        lock_free.append(mirror._lock.acquire(blocking=False))  # noqa: SLF001
        mirror._lock.release()  # noqa: SLF001
        facts.append(value)

    kernel = _Kernel()
    mirror = InterruptRuleStateMirror(
        read_events=kernel, on_change=lambda _s: None,
        on_program_run_hold=on_hold,
    )
    # The subscription's first snapshot: no claimed run yet.
    mirror.apply_stream_message(_snapshot(0, None, None, program_run_hold=None))
    assert facts == []  # A null hold produces no call.
    return mirror, facts, lock_free


def test_hold_facts_are_parsed_with_the_markers_and_handed_out_once_per_change() -> None:
    mirror, facts, lock_free = _hold_mirror()
    mirror.apply_stream_message(_snapshot(0, None, None, program_run_hold=None))
    assert facts == []  # Unchanged: nothing handed out again.
    mirror.apply_stream_message(_hold_state(_hold()))
    mirror.apply_stream_message(_hold_state(_hold()))
    assert len(facts) == 1 and lock_free == [True]
    first = facts[0]
    assert (first.program_run_id, first.hold_revision, first.frozen, first.frozen_by) == (
        3, 9, "halt", 12)
    assert (first.all_groups_held, first.held_by_rule) == (False, None)
    assert (first.last_stop_event_id, first.stop_progress_sequence) == (12, 4)
    assert (first.frozen_kind, first.frozen_source_label) == ("halt", "operator.halt")
    assert first.halt_cause() == {"kind": "halt", "source_label": "operator.halt"}
    # Only the progress marker moves (a participant went from stopping to
    # faulted: no hold revision is raised): still a change.
    mirror.apply_stream_message(_hold_state(_hold(), stop_progress_sequence=5))
    # Execute after Halt released the freeze at a higher revision.
    mirror.apply_stream_message(
        _hold_state(_hold(hold_revision=10, frozen="none", frozen_by=None), stop_progress_sequence=5))
    mirror.apply_stream_message(_hold_state(
        _hold(hold_revision=11, frozen="none", frozen_by=None, held_by_rule="guard",
              all_groups_held=True), stop_progress_sequence=5))
    assert [(f.hold_revision, f.frozen, f.stop_progress_sequence) for f in facts] == [
        (9, "halt", 4), (9, "halt", 5), (10, "none", 5), (11, "none", 5)]
    assert facts[-1].held_by_rule == "guard" and facts[-1].all_groups_held is True
    # The claim ends: null is not handed out.
    mirror.apply_stream_message(_hold_state(None))
    assert len(facts) == 4


@pytest.mark.parametrize(("patch", "markers"), [
    ({"program_run_id": "program-cycle.3"}, {}),
    ({"program_run_id": 0}, {}),
    ({"hold_revision": -1}, {}),
    ({"hold_revision": True}, {}),
    ({"frozen": "full_stop"}, {}),
    ({"frozen_by": 0}, {}),
    ({"all_groups_held": 0}, {}),
    ({"held_by_rule": ""}, {}),
    ({"frozen_kind": "stop_all"}, {}),
    ({"frozen_source_label": 7}, {}),
    ({"selection_revision": 9}, {}),
    ({}, {"stop_progress_sequence": -1}),
    ({}, {"last_stop_event_id": "12"}),
])
def test_a_malformed_hold_is_refused_and_never_handed_out(
    patch: dict[str, Any], markers: dict[str, Any]
) -> None:
    mirror, facts, _lock_free = _hold_mirror()
    with pytest.raises(InterruptStateWireError):
        mirror.apply_stream_message(_hold_state(_hold(**patch), **markers))
    assert facts == []
    # The next valid state is handed out.
    mirror.apply_stream_message(_hold_state(_hold()))
    assert len(facts) == 1


def test_a_state_without_the_hold_member_is_malformed() -> None:
    mirror, facts, _lock_free = _hold_mirror()
    state = _state(None, None)
    del state["program_run_hold"]
    with pytest.raises(KeyError):
        mirror.apply_stream_message(_stream("state", state=state))
    assert facts == []


def test_the_old_fence_member_is_not_read() -> None:
    mirror, facts, _lock_free = _hold_mirror()
    state = _state(None, None)
    del state["program_run_hold"]
    state["program_run_fence"] = {"program_run_id": 3}
    with pytest.raises(KeyError):
        mirror.apply_stream_message(_stream("state", state=state))
    assert facts == []


def test_the_core_follower_feeds_the_mirror_its_table_and_the_wake_set(tmp_path) -> None:
    # The runner's wiring over the real core: one C++ follower against a fake
    # host socket, the mirror applying its subscription on a Python thread.
    import json
    import socket
    import time

    import manyforge_headless_py as core
    import rfc8785

    tmp_path.chmod(0o700)
    endpoint = tmp_path / "execution-events.sock"
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    listener.bind(str(endpoint))
    endpoint.chmod(0o600)
    listener.listen(1)
    wake = core.WakeSet()
    wake.watch_operation(1, 4)
    events = core.ExecutionEvents(str(tmp_path), wake_set=wake)
    applied, lost = threading.Event(), threading.Event()
    changes: list[Any] = []
    mirror = InterruptRuleStateMirror(
        read_events=_Kernel(), on_change=changes.append, subscription=events.subscribe(),
    )
    stop = threading.Event()
    follower = threading.Thread(
        target=mirror.follow, args=(stop,),
        kwargs={"on_message": lambda _message: applied.set(),
                "on_disconnect": lambda _error: lost.set()},
    )
    events.start()
    follower.start()
    connection, _address = listener.accept()
    try:
        assert json.loads(connection.recv(4096))["after_cursor"] == 0
        entry = {"control_pending": False, "last_control": None, "operation_id": 4,
                 "operation": {"operation_id": 4, "state": "executing"}}
        state = {
            **_state(_ready({"door": "clear"}), _CATALOG_A),
            "active_tcp_baseline": [],
            "operations": {"change": 3, "faulted": [], "faulted_total": 0,
                           "resident": [entry], "resident_total": 1},
            "planning_jobs": [{"job_id": "job", "kind": "pick", "program_run_id": 1,
                               "state": "planning"}],
        }
        connection.send(rfc8785.dumps(_stream("snapshot", cursor=0, reason="subscribed",
                                              state=state)))
        assert applied.wait(30.0)
        assert wake.wait(time.monotonic() + 30.0) == [1]
        assert events.connected() and mirror.current() == {"door": "clear"}
        assert events.operation_envelope(4, 3)["operation"] == entry["operation"]
        assert events.planning_job_state("job") == "planning"
        with pytest.raises(core.GatewayError) as behind:
            events.operation_envelope(4, 4)
        assert behind.value.code == "execution_event_mirror_behind"
        connection.close()
        assert lost.wait(30.0)
        assert mirror.current() is None and changes == [{"door": "clear"}, None]
        with pytest.raises(core.GatewayError) as unavailable:
            events.planning_job_state("job")
        assert unavailable.value.code == "execution_event_mirror_unavailable"
    finally:
        stop.set()
        events.close()
        follower.join(5.0)
        listener.close()
    assert not follower.is_alive()
