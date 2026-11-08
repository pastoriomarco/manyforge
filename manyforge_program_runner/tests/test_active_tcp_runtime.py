"""Run and idle Active TCP store wiring over the existing stream."""

from threading import Event, Lock, Thread
from types import SimpleNamespace

import pytest

from manyforge_program_runner.active_tcp_mirror import ActiveTcpMirror, ActiveTcpWireError
from manyforge_program_runner.models import (
    ProgramFullStopResumeRequest,
    ProgramSelectionResumeRequest,
    ProgramSelectionStartRequest,
)
from manyforge_program_runner.runtime import ProgramRunnerError, ProgramRuntime


class _Store:
    def __init__(self) -> None:
        self.values = {}

    def write_active_tcp(self, component_id, command_target_id, entry):
        self.values[(component_id, command_target_id)] = entry


def _entry(frame_id="flange", *, target_component_id="robot", command_target_id="arm",
           frame_component_id=None):
    return {
        "target": {
            "component_id": target_component_id,
            "command_target_id": command_target_id,
        },
        "frame": {
            "component_id": frame_component_id or target_component_id,
            "frame_id": frame_id,
        },
    }


def _binding(*, include_io=False):
    rows = [{
        "target": {"component_id": "robot", "command_target_id": "arm"},
        "robot_tcps": [],
    }]
    if include_io:
        rows.append({"target": {"component_id": "io", "command_target_id": "output"}})
    return {"target_bindings": rows}


def _runtime():
    runtime = object.__new__(ProgramRuntime)
    runtime._liveness = object()  # noqa: SLF001
    runtime._execution_transport = SimpleNamespace(  # noqa: SLF001
        subscribe_events=lambda *_args: None,
    )
    runtime._interrupt_state_follower = SimpleNamespace(is_alive=lambda: True)  # noqa: SLF001
    runtime._active_tcp_stores_lock = Lock()  # noqa: SLF001
    runtime._active_tcp_stores = {}  # noqa: SLF001
    runtime._active_tcp_mirror = ActiveTcpMirror(on_change=runtime._write_active_tcp)  # noqa: SLF001
    runtime._kernel = SimpleNamespace(read_active_tcp=lambda **_kwargs: {"robots": []})  # noqa: SLF001
    runtime._current_kernel_owner_epoch = lambda: "owner-1"  # type: ignore[method-assign]  # noqa: SLF001
    return runtime


def _row(sequence, entry, *, owner="owner-1"):
    return {
        "component_id": "robot", "owner_epoch": owner,
        "registration_generation": 1, "sequence": sequence, "entry": entry,
    }


def test_run_and_idle_stores_receive_stream_then_result_with_newer_winner() -> None:
    runtime = _runtime()
    binding = _binding(include_io=True)
    run, idle = _Store(), _Store()
    runtime._register_active_tcp_store(None, idle, binding, "owner-1")  # noqa: SLF001
    runtime._register_active_tcp_store("run", run, binding, "owner-1")  # noqa: SLF001
    first = _entry("tcp", target_component_id="tool", command_target_id="gripper")
    later = _entry("tip", target_component_id="tool", command_target_id="gripper")
    runtime._on_active_tcp_message({  # noqa: SLF001
        "type": "snapshot", "owner_epoch": "owner-1",
        "state": {"active_tcp_baseline": [_row(2, later)]},
    })
    runtime._apply_active_tcp_result("run", "robot", [_row(1, first)])  # noqa: SLF001
    assert run.values == idle.values == {("robot", "arm"): later}
    runtime._apply_active_tcp_result("run", "robot", [_row(3, first, owner="old")])  # noqa: SLF001
    assert run.values == idle.values == {("robot", "arm"): later}
    with pytest.raises(ActiveTcpWireError):
        runtime._apply_active_tcp_result("run", "robot", [])  # noqa: SLF001


def test_store_registration_seeds_known_state_without_a_bb_publisher() -> None:
    runtime = _runtime()
    entry = _entry()
    runtime._active_tcp_mirror.set_owner("owner-1")  # noqa: SLF001
    runtime._active_tcp_mirror.apply_rows([_row(0, entry)])  # noqa: SLF001
    runtime._kernel = SimpleNamespace(read_active_tcp=lambda **_kwargs: {"robots": [_row(0, entry)]})  # noqa: SLF001
    store = _Store()
    binding = _binding()
    runtime._register_active_tcp_store(None, store, binding, "owner-1")  # noqa: SLF001
    assert store.values == {("robot", "arm"): entry}


def test_unavailable_one_shot_read_leaves_new_store_unavailable() -> None:
    runtime = _runtime()
    entry = _entry()
    runtime._active_tcp_mirror.set_owner("owner-1")  # noqa: SLF001
    runtime._active_tcp_mirror.apply_rows([_row(1, entry)])  # noqa: SLF001

    def unavailable(**_kwargs):
        raise RuntimeError("unavailable")

    runtime._kernel = SimpleNamespace(read_active_tcp=unavailable)  # noqa: SLF001
    store = _Store()
    binding = _binding()
    runtime._register_active_tcp_store("run", store, binding, "owner-1")  # noqa: SLF001
    assert store.values == {}
    # A same-value Set TCP has no change event, but its successful receipt
    # still writes the previously unavailable Run key before it completes.
    runtime._apply_active_tcp_result("run", "robot", [_row(1, entry)])  # noqa: SLF001
    assert store.values == {("robot", "arm"): entry}


def test_late_old_registration_and_event_cannot_replace_stream_owner() -> None:
    runtime = _runtime()
    runtime._current_kernel_owner_epoch = lambda: "owner-2"  # type: ignore[method-assign]  # noqa: SLF001
    entry = _entry()
    runtime._on_active_tcp_message({  # noqa: SLF001
        "type": "snapshot", "owner_epoch": "owner-2",
        "state": {"active_tcp_baseline": [_row(0, entry, owner="owner-2")]},
    })
    runtime._kernel = SimpleNamespace(read_active_tcp=lambda **_kwargs: {"robots": [_row(5, entry)]})  # noqa: SLF001
    store = _Store()
    binding = _binding()
    runtime._register_active_tcp_store("old-run", store, binding, "owner-1")  # noqa: SLF001
    runtime._on_active_tcp_message({  # noqa: SLF001
        "type": "event", "owner_epoch": "owner-1",
        "event": {"record_kind": "active_tcp_changed", "payload": {
            **_row(8, entry), "api_version": "manyforge.active_tcp_change.v2",
        }},
    })
    assert runtime._active_tcp_mirror.owner_epoch == "owner-2"  # noqa: SLF001
    assert store.values == {}


def test_run_registration_uses_newer_stream_fence_not_start_capture() -> None:
    runtime = _runtime()
    runtime._active_tcp_mirror.set_owner("owner-2")  # noqa: SLF001
    entry = _entry()
    runtime._kernel = SimpleNamespace(
        read_active_tcp=lambda **_kwargs: {"robots": [_row(1, entry, owner="owner-2")]}
    )  # noqa: SLF001
    store = _Store()
    binding = _binding()
    runtime._register_active_tcp_store("run", store, binding, "owner-1")  # noqa: SLF001
    assert runtime._active_tcp_mirror.owner_epoch == "owner-2"  # noqa: SLF001
    assert store.values == {("robot", "arm"): entry}


def test_resume_refresh_failed_read_leaves_key_unavailable() -> None:
    runtime = _runtime()
    runtime._interrupt_state_follower = None  # noqa: SLF001
    entry = _entry()
    store = _Store()
    runtime._active_tcp_stores["run"] = (store, {"robot": ("arm",)})  # noqa: SLF001
    runtime._active_tcp_mirror.set_owner("owner-1")  # noqa: SLF001
    runtime._active_tcp_mirror.apply_rows([_row(1, entry)])  # noqa: SLF001
    def unavailable(*, ordered=False):
        assert ordered
        raise RuntimeError("offline")
    runtime._kernel = SimpleNamespace(read_active_tcp=unavailable)  # noqa: SLF001
    runtime._refresh_active_tcp()  # noqa: SLF001
    assert store.values[("robot", "arm")] is None


def test_resume_refresh_does_not_blank_key_while_ordered_read_waits() -> None:
    runtime = _runtime()
    runtime._interrupt_state_follower = None  # noqa: SLF001
    entry = _entry()
    store = _Store()
    runtime._active_tcp_stores["run"] = (store, {"robot": ("arm",)})  # noqa: SLF001
    runtime._active_tcp_mirror.set_owner("owner-1")  # noqa: SLF001
    runtime._active_tcp_mirror.apply_rows([_row(1, entry)])  # noqa: SLF001
    entered, release = Event(), Event()

    def read(*, ordered=False):
        assert ordered
        entered.set()
        assert release.wait(2)
        return {"robots": [_row(1, entry)]}

    runtime._kernel = SimpleNamespace(read_active_tcp=read)  # noqa: SLF001
    worker = Thread(target=runtime._refresh_active_tcp)  # noqa: SLF001
    worker.start()
    try:
        assert entered.wait(2)
        assert store.values[("robot", "arm")] == entry
    finally:
        release.set()
        worker.join(2)
    assert not worker.is_alive()
    assert store.values[("robot", "arm")] == entry


def test_resume_failed_read_keeps_stream_authority() -> None:
    runtime = _runtime()
    entry = _entry()
    store = _Store()
    runtime._active_tcp_stores["run"] = (store, {"robot": ("arm",)})  # noqa: SLF001
    runtime._active_tcp_mirror.set_owner("owner-1")  # noqa: SLF001
    runtime._active_tcp_mirror.apply_rows([_row(1, entry)])  # noqa: SLF001

    def unavailable(*, ordered=False):
        assert ordered
        raise RuntimeError("offline")

    runtime._kernel = SimpleNamespace(read_active_tcp=unavailable)  # noqa: SLF001
    runtime._refresh_active_tcp()  # noqa: SLF001
    assert store.values[("robot", "arm")] == entry


def test_resume_refresh_keeps_newer_stream_value_than_ordered_read() -> None:
    runtime = _runtime()
    old = _entry()
    new = _entry("tip")
    store = _Store()
    runtime._active_tcp_stores["run"] = (store, {"robot": ("arm",)})  # noqa: SLF001
    runtime._active_tcp_mirror.set_owner("owner-1")  # noqa: SLF001
    runtime._active_tcp_mirror.apply_rows([_row(1, old)])  # noqa: SLF001

    def read(*, ordered=False):
        assert ordered
        runtime._active_tcp_mirror.apply_rows([_row(3, new)])  # noqa: SLF001
        return {"robots": [_row(2, old)]}

    runtime._kernel = SimpleNamespace(read_active_tcp=read)  # noqa: SLF001
    runtime._refresh_active_tcp()  # noqa: SLF001
    assert store.values[("robot", "arm")] == new


def test_program_scene_reset_failure_preserves_typed_tcp_cause() -> None:
    runtime = _runtime()
    runtime._reset_persistent_keys_to_initial = lambda _request: None  # type: ignore[method-assign]  # noqa: SLF001
    runtime._prepare_scene_after_reset = lambda _request, _authorization: None  # type: ignore[method-assign]  # noqa: SLF001

    def fail_reset():
        raise ProgramRunnerError("vendor_failed", "unconfirmed transport", details={
            "reason": "transport_unavailable", "unregistered": "not surfaced",
        })

    runtime._reset_active_tcp = fail_reset  # type: ignore[method-assign]  # noqa: SLF001
    cycle = SimpleNamespace(reset_phase="restoring_scene", cycle_generation=1)
    journal = SimpleNamespace(read=lambda: cycle)
    request = SimpleNamespace(reload_initial_scene=True)
    with pytest.raises(ProgramRunnerError) as captured:
        runtime._advance_reset_options(request, None, journal)  # noqa: SLF001
    assert captured.value.code == "program_cycle_reset_scene_preparation_failed"
    assert captured.value.details == {
        "cause_code": "active_tcp_reset_failed", "reason": "transport_unavailable",
    }


def test_reset_requires_every_registered_robot_from_current_owner() -> None:
    runtime = _runtime()
    entry = _entry()
    store = _Store()
    runtime._active_tcp_stores["run"] = (store, {"robot": ("arm",)})  # noqa: SLF001
    runtime._active_tcp_mirror.set_owner("owner-1")  # noqa: SLF001
    for rows in ([], [_row(1, entry, owner="old")]):
        runtime._kernel = SimpleNamespace(reset_active_tcp=lambda: {"robots": rows})  # noqa: SLF001
        with pytest.raises(ActiveTcpWireError):
            runtime._reset_active_tcp()  # noqa: SLF001
    runtime._kernel = SimpleNamespace(reset_active_tcp=lambda: {"robots": [_row(1, entry)]})  # noqa: SLF001
    runtime._reset_active_tcp()  # noqa: SLF001
    assert store.values[("robot", "arm")] == entry


def test_reset_installs_ordered_reply_owner_without_follower() -> None:
    runtime = _runtime()
    runtime._interrupt_state_follower = None  # noqa: SLF001
    entry = _entry()
    store = _Store()
    runtime._active_tcp_stores["run"] = (store, {"robot": ("arm",)})  # noqa: SLF001
    runtime._active_tcp_mirror.set_owner("owner-1")  # noqa: SLF001
    runtime._active_tcp_mirror.apply_rows([_row(5, entry)])  # noqa: SLF001
    new = _entry("tip")
    runtime._kernel = SimpleNamespace(  # noqa: SLF001
        reset_active_tcp=lambda: {"robots": [_row(1, new, owner="owner-2")]},
    )
    runtime._reset_active_tcp()  # noqa: SLF001
    assert runtime._active_tcp_mirror.owner_epoch == "owner-2"  # noqa: SLF001
    assert store.values[("robot", "arm")] == new


def test_invalid_reset_reply_cannot_install_a_new_owner() -> None:
    runtime = _runtime()
    runtime._interrupt_state_follower = None  # noqa: SLF001
    entry = _entry()
    runtime._active_tcp_stores["run"] = (_Store(), {"robot": ("arm",)})  # noqa: SLF001
    runtime._active_tcp_mirror.set_owner("owner-1")  # noqa: SLF001
    other = {**_row(1, entry, owner="owner-3"), "component_id": "other"}
    for rows in ([], [_row(1, entry, owner="owner-2"), other]):
        runtime._kernel = SimpleNamespace(reset_active_tcp=lambda: {"robots": rows})  # noqa: SLF001
        with pytest.raises(ActiveTcpWireError):
            runtime._reset_active_tcp()  # noqa: SLF001
        assert runtime._active_tcp_mirror.owner_epoch == "owner-1"  # noqa: SLF001


@pytest.mark.parametrize("no_subscription", [False, True])
def test_ordered_read_uses_owner_without_running_follower_even_with_liveness(no_subscription) -> None:
    runtime = _runtime()
    if no_subscription:
        runtime._execution_transport = SimpleNamespace()  # noqa: SLF001
    else:
        runtime._interrupt_state_follower = SimpleNamespace(is_alive=lambda: False)  # noqa: SLF001
    entry = _entry()
    runtime._active_tcp_mirror.set_owner("owner-1")  # noqa: SLF001
    runtime._kernel = SimpleNamespace(  # noqa: SLF001
        read_active_tcp=lambda **_kwargs: {"robots": [_row(1, entry, owner="owner-2")]},
    )
    runtime._refresh_active_tcp()  # noqa: SLF001
    assert runtime._active_tcp_mirror.owner_epoch == "owner-2"  # noqa: SLF001
    assert runtime._active_tcp_mirror.current()["robot"][2] == entry  # noqa: SLF001


@pytest.mark.parametrize(
    ("method", "request_type"),
    [
        ("start_motion", ProgramSelectionStartRequest),
        ("resume_motion", ProgramSelectionResumeRequest),
        ("resume_full_stop", ProgramFullStopResumeRequest),
    ],
)
def test_motion_start_and_resume_refresh_tcp_before_host_action(method, request_type) -> None:
    runtime = _runtime()
    calls = []

    def host_action(*_args, **_kwargs):
        calls.append("host")
        return SimpleNamespace(state=SimpleNamespace(value="running"), run_revision=2)

    runtime._refresh_active_tcp = lambda: calls.append("tcp")  # noqa: SLF001
    runtime._require_run_readiness = lambda *_args: None  # noqa: SLF001
    runtime._host = SimpleNamespace(**{method: host_action})  # noqa: SLF001
    runtime._control_effect = lambda _request, *, kind, effect: {"run": effect()}  # noqa: SLF001
    runtime._project_run = lambda run: run  # noqa: SLF001
    request = request_type.model_construct(
        program_run_id="run", request_id="request", reason="operator",
        group_ids=["arm"], expected_run_revision=1, expected_selection_revision=1,
    )
    getattr(runtime, method)("run", request)
    assert calls == ["tcp", "host"]


def test_idle_no_liveness_seeds_own_ordered_read_not_previous_owner() -> None:
    runtime = _runtime()
    runtime._liveness = None  # noqa: SLF001
    runtime._interrupt_state_follower = None  # noqa: SLF001
    old = _entry()
    new = _entry("tip")
    runtime._active_tcp_mirror.set_owner("owner-1")  # noqa: SLF001
    runtime._active_tcp_mirror.apply_rows([_row(1, old)])  # noqa: SLF001
    def read(*, ordered=False):
        assert ordered
        return {"robots": [_row(1, new, owner="owner-2")]}

    runtime._kernel = SimpleNamespace(read_active_tcp=read)  # noqa: SLF001
    store = _Store()
    binding = _binding()
    runtime._register_active_tcp_store(None, store, binding, "owner-1")  # noqa: SLF001
    assert store.values == {("robot", "arm"): new}
    assert runtime._active_tcp_mirror.owner_epoch == "owner-2"  # noqa: SLF001


def test_idle_no_liveness_failed_ordered_read_does_not_seed_cache() -> None:
    runtime = _runtime()
    runtime._liveness = None  # noqa: SLF001
    runtime._interrupt_state_follower = None  # noqa: SLF001
    old = _entry()
    runtime._active_tcp_mirror.set_owner("owner-1")  # noqa: SLF001
    runtime._active_tcp_mirror.apply_rows([_row(1, old)])  # noqa: SLF001

    def read(*, ordered=False):
        assert ordered
        raise RuntimeError("read unavailable")

    runtime._kernel = SimpleNamespace(read_active_tcp=read)  # noqa: SLF001
    store = _Store()
    binding = _binding()
    runtime._register_active_tcp_store(None, store, binding, "owner-1")  # noqa: SLF001
    assert store.values == {}


def test_idle_registration_excludes_inflight_updates_until_own_read_fails() -> None:
    runtime = _runtime()
    runtime._liveness = None  # noqa: SLF001
    runtime._interrupt_state_follower = None  # noqa: SLF001
    runtime._active_tcp_mirror.set_owner("owner-1")  # noqa: SLF001
    entry = _entry()
    entered, release = Event(), Event()

    def read(*, ordered=False):
        assert ordered
        entered.set()
        assert release.wait(2)
        raise RuntimeError("unavailable")

    runtime._kernel = SimpleNamespace(read_active_tcp=read)  # noqa: SLF001
    store = _Store()
    binding = _binding()
    worker = Thread(target=runtime._register_active_tcp_store, args=(None, store, binding, "owner-1"))  # noqa: SLF001
    worker.start()
    assert entered.wait(2)
    runtime._active_tcp_mirror.apply_rows([_row(1, entry)])  # noqa: SLF001
    assert store.values == {}
    release.set()
    worker.join(2)
    assert not worker.is_alive()
    assert store.values == {}
    newer = _entry("tip")
    runtime._active_tcp_mirror.apply_rows([_row(2, newer)])  # noqa: SLF001
    assert store.values == {("robot", "arm"): newer}


def test_idle_registration_seeds_latest_update_after_own_read() -> None:
    runtime = _runtime()
    runtime._liveness = None  # noqa: SLF001
    runtime._interrupt_state_follower = None  # noqa: SLF001
    runtime._active_tcp_mirror.set_owner("owner-1")  # noqa: SLF001
    old = _entry()
    new = _entry("tip")

    def read(*, ordered=False):
        assert ordered
        runtime._active_tcp_mirror.apply_rows([_row(2, new)])  # noqa: SLF001
        return {"robots": [_row(1, old)]}

    runtime._kernel = SimpleNamespace(read_active_tcp=read)  # noqa: SLF001
    store = _Store()
    binding = _binding()
    runtime._register_active_tcp_store(None, store, binding, "owner-1")  # noqa: SLF001
    assert store.values == {("robot", "arm"): new}
