"""Activation-path CCLC helpers mirror useCCLC.ts: torn-read re-read, shared
deadline, and provider/controller expected-domain echo."""

from copy import deepcopy
import json
from types import SimpleNamespace

import pytest

from manyforge_headless_api.cclc import _validate_component_readiness
from manyforge_headless_api.client import HeadlessClientError
from manyforge_headless_api.models import ComponentLifecycleRequest
from manyforge_headless_api.process_tests import test_ui6i_grouped_execution as ui6i
from manyforge_headless_api.tests import test_cclc as cclc_samples


def _transient() -> HeadlessClientError:
    return HeadlessClientError(
        "component_lifecycle_facts_stale",
        "the current component context changed during its read",
        409,
        details={"reason": "component_read_changed"},
    )


def _snapshot(state: str) -> dict:
    return {"controllable_component_id": "arm", "component_lifecycle_state": state,
            "graph_revision": "graph", "component_lifecycle_generation": 3, "domains": []}


class Clock:
    def __init__(self) -> None:
        self.now = 0.0
        self.sleeps: list[float] = []

    def monotonic(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.sleeps.append(seconds)
        self.now += seconds


@pytest.fixture
def clock(monkeypatch) -> Clock:
    clock = Clock()
    monkeypatch.setattr(ui6i, "time", SimpleNamespace(
        monotonic=clock.monotonic, sleep=clock.sleep, time=lambda: 0.0))
    return clock


class FakeHeadless:
    def __init__(self, reads: list, clock: Clock | None = None, component_id: str = "arm") -> None:
        self.reads = list(reads)
        self.read_count = 0
        self.read_times: list[float] = []
        self.submitted: list[dict] = []
        self.clock = clock
        self.component_id = component_id

    def __enter__(self):
        return self

    def __exit__(self, *_):
        return False

    def cclc_owner_identity(self):
        return {"request_owner_epoch": "owner"}

    def cclc_component(self, component_id):
        assert component_id == self.component_id
        self.read_count += 1
        if self.clock is not None:
            self.read_times.append(self.clock.now)
        value = self.reads.pop(0)
        if isinstance(value, Exception):
            raise value
        return value

    def submit_component_lifecycle(self, request):
        self.submitted.append(request)
        return {"acceptance": {"lifecycle_transaction_id": "tx"}}

    def component_lifecycle_status(self, transaction_id):
        achieved = {"prepare": "prepared", "enable": "active", "disable": "disabled"}[
            self.submitted[-1]["intent"]]
        return {"status": {"status_owner_epoch": "owner",
                           "acceptance": {"lifecycle_transaction_id": transaction_id},
                           "terminal_result": "succeeded", "current_state": achieved,
                           "domains": [{"control_domain_id": row["control_domain_id"],
                                        "current_state": achieved}
                                       for row in self.submitted[-1]["expected_control_domains"]]}}


def _stack(fake, monkeypatch, **extra):
    monkeypatch.setattr(ui6i, "HeadlessHttpClient", lambda *_args, **_kwargs: fake)
    return SimpleNamespace(base_url="http://fake",
                           wait_authoring_admission=lambda *_, **__: {"admission_generation": 1},
                           **extra)


def test_transient_reads_are_reread_and_the_command_is_sent_once(monkeypatch, clock):
    fake = FakeHeadless([_transient(), _transient(), _snapshot("prepared"), _snapshot("active")])
    status = ui6i._component_lifecycle(_stack(fake, monkeypatch), "arm", "enable", "owner")
    assert status["terminal_result"] == "succeeded"
    assert len(fake.submitted) == 1
    assert fake.read_count == 4  # 2 transient + pre-submit + post-terminal


def test_other_errors_fail_immediately_without_reread(clock):
    for error in (
        HeadlessClientError("component_lifecycle_facts_stale", "other", 409,
                            details={"reason": "different_reason"}),
        HeadlessClientError("other_code", "other", 409, details={"reason": "component_read_changed"}),
        HeadlessClientError("component_lifecycle_facts_stale", "other", 500,
                            details={"reason": "component_read_changed"}),
    ):
        fake = FakeHeadless([error, _snapshot("active")])
        with pytest.raises(HeadlessClientError) as raised:
            ui6i.read_activation_component(fake, "arm", deadline=20.0)
        assert raised.value is error
        assert fake.read_count == 1
    assert clock.sleeps == []


def test_transient_until_the_deadline_fails_and_never_reads_past_it(clock):
    fake = FakeHeadless([_transient() for _ in range(100)], clock)
    with pytest.raises(AssertionError, match="shared activation deadline.*component_read_changed"):
        ui6i.read_activation_component(fake, "arm", deadline=2.0)
    assert fake.read_count == 8 and max(fake.read_times) < 2.0
    assert set(clock.sleeps) == {0.25}


def test_an_expired_shared_deadline_starts_no_read(clock):
    clock.now = 5.0
    fake = FakeHeadless([_snapshot("active")], clock)
    with pytest.raises(AssertionError, match="shared activation deadline"):
        ui6i.read_activation_component(fake, "arm", deadline=5.0)
    assert fake.read_count == 0


def _provider_snapshot(*, ready: bool) -> dict:
    snapshot = cclc_samples._provider_domain_snapshot()
    snapshot["domains"][0]["resources"] = ["station_io.clamp", "station_io.lamp"]
    readiness = snapshot["readiness"]
    readiness["request_owner_epoch"] = "owner"
    if ready:
        readiness["ready"], readiness["error"] = True, None
        readiness["domains"][0].update(error=None, decision_kind="dispatch_primitive")
    _validate_component_readiness(snapshot)
    assert readiness["measured_state"] == {"required": False, "available": None, "error": None}
    return snapshot


def _activate_one_provider(monkeypatch, reads, clock, tmp_path):
    component_id = reads[0]["controllable_component_id"]
    fake = FakeHeadless(reads, clock, component_id)
    stack = _stack(fake, monkeypatch, root=tmp_path)
    monkeypatch.setattr(ui6i, "_applied_activation_order", lambda _stack: [component_id])
    return fake, stack


def test_wait_ready_transients_near_the_deadline_share_its_budget(monkeypatch, clock, tmp_path):
    # Not ready until just before the 20 s deadline, then only transients and
    # a final ready read: the read must not start a fresh budget (old ~40 s)
    # nor return ready after the shared deadline.
    not_ready = _provider_snapshot(ready=False)
    reads = [not_ready] + [deepcopy(not_ready) for _ in range(79)] + [
        _transient() for _ in range(200)] + [_provider_snapshot(ready=True)]
    fake, stack = _activate_one_provider(monkeypatch, reads, clock, tmp_path)
    with pytest.raises(AssertionError, match="shared activation deadline"):
        ui6i._activate_loaded_components(stack)
    assert max(fake.read_times) < 20.0 and clock.now <= 20.0
    assert fake.submitted == []


def test_wait_ready_timeout_evidence_names_provider_runtime_epochs(monkeypatch, clock, tmp_path):
    not_ready = _provider_snapshot(ready=False)
    reads = [deepcopy(not_ready) for _ in range(200)]
    fake, stack = _activate_one_provider(monkeypatch, reads, clock, tmp_path)
    with pytest.raises(AssertionError, match="readiness-last-snapshot"):
        ui6i._activate_loaded_components(stack)
    assert max(fake.read_times) < 20.0 and fake.submitted == []
    evidence = json.loads((tmp_path / "readiness-last-snapshot.json").read_text())
    assert evidence["provider_runtime_epochs"] == ["epoch.1"]


def test_provider_domain_builds_the_composer_lifecycle_request(monkeypatch, clock):
    current = _provider_snapshot(ready=True)
    domain = current["domains"][0]
    fake = FakeHeadless([current, {**deepcopy(current), "component_lifecycle_state": "active"}],
                        component_id="component.arm")
    ui6i._component_lifecycle(_stack(fake, monkeypatch), "component.arm", "enable", "owner")
    (request,) = fake.submitted
    # Exactly useCCLC.ts lifecycleRequestFromSnapshot for kind == "io_provider".
    assert request["expected_control_domains"] == [{
        "kind": "io_provider",
        "control_domain_id": domain["control_domain_id"],
        "control_domain_revision": domain["control_domain_revision"],
        "provider": domain["provider"],
        "expected_active_control_configuration": {
            "presence": "present",
            "active_control_configuration_id": "provider.configuration",
            "active_control_configuration_generation": 9007199254740991,
        },
    }]
    ComponentLifecycleRequest.model_validate(request)

    current["domains"][0]["active_control_configuration"] = None
    _validate_component_readiness(current)
    fake = FakeHeadless([current, {**deepcopy(current), "component_lifecycle_state": "active"}],
                        component_id="component.arm")
    ui6i._component_lifecycle(_stack(fake, monkeypatch), "component.arm", "enable", "owner")
    (request,) = fake.submitted
    assert request["expected_control_domains"][0]["expected_active_control_configuration"] == {
        "presence": "absent"}
    ComponentLifecycleRequest.model_validate(request)


def test_controller_domain_request_carries_its_kind(monkeypatch, clock):
    current = cclc_samples._target_snapshot()
    domain = current["domains"][0]
    fake = FakeHeadless([current, {**deepcopy(current), "component_lifecycle_state": "active"}],
                        component_id="component.arm")
    ui6i._component_lifecycle(_stack(fake, monkeypatch), "component.arm", "enable", "owner")
    (request,) = fake.submitted
    assert request["expected_control_domains"] == [{
        "kind": "controller",
        "control_domain_id": "domain.arm",
        "control_domain_revision": "domain.revision.1",
        "controller_topology_id": "topology.arm",
        "topology_revision": "topology.revision.1",
        "expected_active_control_configuration": {"presence": "absent"},
        "expected_control_bindings": domain["current_control_bindings"],
    }]
    ComponentLifecycleRequest.model_validate(request)


def test_late_admission_past_the_shared_deadline_sends_no_command(monkeypatch, clock):
    # Valid read at t=19.75 s, admission returns at t=21 s: never dispatch.
    fake = FakeHeadless([_snapshot("prepared"), _snapshot("active")], clock)
    stack = _stack(fake, monkeypatch)
    budgets: list[float] = []

    def late_admission(timeout_seconds, *, expected_allowed):
        assert expected_allowed is True
        budgets.append(timeout_seconds)
        clock.now = 21.0
        return {"admission_generation": 1}

    stack.wait_authoring_admission = late_admission
    original_read = fake.cclc_component

    def slow_read(component_id):
        clock.now = 19.75
        return original_read(component_id)

    fake.cclc_component = slow_read
    with pytest.raises(AssertionError,
                       match=r"enable of 'arm' reached the shared activation deadline before "
                             r"command dispatch \(1\.000 s late\); no command was sent"):
        ui6i._component_lifecycle(stack, "arm", "enable", "owner")
    assert budgets == [pytest.approx(0.25)]  # remaining shared budget, not 8 s
    assert fake.submitted == []


def test_terminal_status_poll_never_starts_after_the_shared_deadline(monkeypatch, clock):
    poll_times: list[float] = []

    class PendingHeadless(FakeHeadless):
        def component_lifecycle_status(self, transaction_id):
            poll_times.append(clock.now)
            status = super().component_lifecycle_status(transaction_id)
            status["status"]["terminal_result"] = None
            return status

    fake = PendingHeadless([_snapshot("prepared")], clock)
    with pytest.raises(AssertionError, match="before terminal status poll.*unobserved.*acceptance="):
        ui6i._component_lifecycle(_stack(fake, monkeypatch), "arm", "enable", "owner")
    assert len(fake.submitted) == 1  # one command, never resent
    assert poll_times and max(poll_times) < 20.0 and clock.now >= 20.0


def test_activation_timing_evidence_rows_use_the_monotonic_clock(monkeypatch, clock, tmp_path):
    # Contract 122 evidence only: one bounded JSON file per activation span.
    evidence_root = tmp_path / "evidence"
    monkeypatch.setenv("MANYFORGE_PROCESS_TEST_EVIDENCE_ROOT", str(evidence_root))
    monkeypatch.setenv("PYTEST_CURRENT_TEST", "a/b.py::t (call)")
    not_ready, ready = _provider_snapshot(ready=False), _provider_snapshot(ready=True)
    active = {**deepcopy(ready), "component_lifecycle_state": "active"}
    reads = [_transient(), deepcopy(not_ready),  # before: t=0 -> 0.25
             deepcopy(not_ready), _transient(), deepcopy(ready),  # wait_ready: 0.25 -> 0.75
             _transient(), deepcopy(ready),  # pre-submit read: -> 1.0
             _transient(), active]  # installed read: 2.25 -> 2.5

    class SlowStatus(FakeHeadless):
        def component_lifecycle_status(self, transaction_id):
            clock.now += 0.75
            return super().component_lifecycle_status(transaction_id)

    component_id = ready["controllable_component_id"]
    fake = SlowStatus(reads, clock, component_id)
    monkeypatch.setattr(ui6i, "HeadlessHttpClient", lambda *_args, **_kwargs: fake)
    monkeypatch.setattr(ui6i, "_applied_activation_order", lambda _stack: [component_id])

    def admission(timeout_seconds, *, expected_allowed):
        clock.now += 0.5
        return {"admission_generation": 1}

    stack = SimpleNamespace(base_url="http://fake", root=tmp_path / "stack",
                            wait_authoring_admission=admission)
    assert ui6i._activate_loaded_components(stack) == [component_id]
    assert len(fake.submitted) == 1 and fake.reads == []

    directory = evidence_root / "a_b.py__t"
    assert sorted(path.name for path in directory.iterdir()) == ["activation-timing.00.json"]
    assert not (tmp_path / "stack").exists()
    document = json.loads((directory / "activation-timing.00.json").read_text())
    assert document["schema"] == ui6i.ACTIVATION_TIMING_SCHEMA
    assert document["span"] == "activate_loaded_components" and document["outcome"] == "ok"
    assert document["span_ms"] == 2500.0 and document["rows_dropped"] == 0
    assert document["deadline_seconds"] == 20.0
    (row,) = document["rows"]
    assert row == {
        "component_id": component_id, "kind": "io_provider", "intent": "enable",
        "deadline_seconds": 20.0,
        "readiness_wait_ms": 500.0, "readiness_wait_iterations": 2,
        "admission_wait_ms": 500.0,
        "submit_to_terminal_ms": 750.0, "submit_to_ready_ms": 1000.0,
        "component_read_changed_count": 4,
        "terminal_result": "succeeded", "terminal_state": "active",
        "target_state": "active", "final_state": "active", "outcome": "ok",
    }


@pytest.mark.parametrize(("kinds", "label"), [
    (["controller"], "controller"), (["io_provider"], "io_provider"),
    (["controller", "io_provider"], "mixed"), (["provider"], "unknown"), ([None], "unknown"),
    (["io_provider", "provider"], "mixed"),
])
def test_timing_row_labels_exact_domain_kinds(kinds, label):
    current = {"domains": [{} if kind is None else {"kind": kind} for kind in kinds]}
    assert ui6i._timing_row("c", current, "enable")["kind"] == label


def test_activation_timing_evidence_is_bounded_and_written_on_failure(clock, tmp_path):
    stack = SimpleNamespace(root=tmp_path)
    rows = [ui6i._timing_row(f"c{index}", None, "prepare") for index in range(40)]
    ui6i._write_activation_timing(stack, "activate_loaded_components", 0.0, rows,
                                  AssertionError("x" * 4096))
    document = json.loads((tmp_path / "activation-timing.00.json").read_text())
    assert len(document["rows"]) == ui6i.ACTIVATION_TIMING_MAX_ROWS
    assert document["rows_dropped"] == 8
    assert document["outcome"] == "error" and len(document["error"]) == 512
    for _ in range(ui6i.ACTIVATION_TIMING_MAX_FILES + 5):
        ui6i._write_activation_timing(stack, "disable_for_reload", 0.0, [], None)
    assert len(list(tmp_path.glob("activation-timing.*.json"))) == ui6i.ACTIVATION_TIMING_MAX_FILES


def test_failed_evidence_diagnostic_never_masks_the_original_exception(monkeypatch, clock, tmp_path):
    class BrokenStderr:
        def write(self, _text):
            raise OSError("stderr closed")

        def flush(self):
            raise OSError("stderr closed")

    def broken_write_text(*_args, **_kwargs):
        raise OSError("disk full")

    monkeypatch.delenv("MANYFORGE_PROCESS_TEST_EVIDENCE_ROOT", raising=False)
    monkeypatch.setattr(ui6i.Path, "write_text", broken_write_text)
    monkeypatch.setattr(ui6i.sys, "stderr", BrokenStderr())
    fake = FakeHeadless([_transient() for _ in range(100)], clock)
    stack = _stack(fake, monkeypatch, root=tmp_path)
    monkeypatch.setattr(ui6i, "_applied_activation_order", lambda _stack: ["arm"])
    with pytest.raises(AssertionError, match="shared activation deadline") as raised:
        ui6i._activate_loaded_components(stack)
    assert "component_read_changed" in str(raised.value)
    assert list(tmp_path.iterdir()) == []


def test_initial_transient_until_the_deadline_keeps_one_row_with_the_real_count(
        monkeypatch, clock, tmp_path):
    monkeypatch.delenv("MANYFORGE_PROCESS_TEST_EVIDENCE_ROOT", raising=False)
    fake = FakeHeadless([_transient() for _ in range(100)], clock)
    stack = _stack(fake, monkeypatch, root=tmp_path)
    monkeypatch.setattr(ui6i, "_applied_activation_order", lambda _stack: ["arm"])
    with pytest.raises(AssertionError, match="shared activation deadline"):
        ui6i._activate_loaded_components(stack)
    assert fake.read_count == 80 and fake.submitted == []  # 20 s / 0.25 s, unchanged
    (path,) = tmp_path.glob("activation-timing.*.json")
    document = json.loads(path.read_text())
    assert document["outcome"] == "error" and document["span_ms"] == 20000.0
    (row,) = document["rows"]
    assert row["component_id"] == "arm" and row["kind"] == "unknown"
    assert row["component_read_changed_count"] == 80 and row["outcome"] == "error"
    assert row["intent"] is None and row["target_state"] is None
    assert row["terminal_result"] is None and row["terminal_state"] is None
    assert row["final_state"] is None and row["submit_to_terminal_ms"] is None


def _requested_target_snapshot(state: str, phase: str) -> dict:
    snapshot = _snapshot(state)
    snapshot.update(lifecycle_target={"requested_state": "active"},
                    lifecycle_progress={"phase": phase})
    return snapshot


def test_target_wait_rereads_only_transient_reads_without_resending_commands(monkeypatch, clock):
    fake = FakeHeadless([
        _transient(), _requested_target_snapshot("prepared", "in_progress"),
        _transient(), _requested_target_snapshot("active", "satisfied"),
    ], clock)
    ui6i._wait_components_reached_requested_target(
        _stack(fake, monkeypatch), ["arm"], timeout_seconds=2.0)
    assert fake.read_count == 4
    assert clock.sleeps == [0.25, 0.1, 0.25]
    assert fake.submitted == []


@pytest.mark.parametrize("error", [
    HeadlessClientError("component_lifecycle_facts_stale", "other", 409,
                        details={"reason": "different_reason"}),
    HeadlessClientError("other_code", "other", 409,
                        details={"reason": "component_read_changed"}),
    HeadlessClientError("component_lifecycle_facts_stale", "other", 500,
                        details={"reason": "component_read_changed"}),
])
def test_target_wait_other_read_errors_propagate_immediately(monkeypatch, clock, error):
    fake = FakeHeadless([error, _requested_target_snapshot("active", "satisfied")], clock)
    with pytest.raises(HeadlessClientError) as raised:
        ui6i._wait_components_reached_requested_target(
            _stack(fake, monkeypatch), ["arm"], timeout_seconds=2.0)
    assert raised.value is error
    assert fake.read_count == 1 and fake.submitted == []
    assert clock.sleeps == []


def test_target_wait_transient_reads_keep_original_qualification_deadline(monkeypatch, clock):
    fake = FakeHeadless([_transient() for _ in range(8)], clock)
    with pytest.raises(AssertionError, match="shared activation deadline.*component_read_changed"):
        ui6i._wait_components_reached_requested_target(
            _stack(fake, monkeypatch), ["arm"], timeout_seconds=0.5)
    assert fake.read_count == 2 and max(fake.read_times) < 0.5
    assert clock.sleeps == [0.25, 0.25]
    assert fake.submitted == []
