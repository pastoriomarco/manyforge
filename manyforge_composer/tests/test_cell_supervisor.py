from __future__ import annotations

import json
import subprocess

from manyforge_supervision.cell_supervisor import CellSupervisor
from manyforge_supervision.cell_supervisor import ComposerSupervisionProbe
from manyforge_supervision.cell_supervisor import ContainerDomain
from manyforge_supervision.cell_supervisor import DomainHealthObservation
from manyforge_supervision.cell_supervisor import DockerCliProcessControl
from manyforge_supervision.cell_supervisor import _heartbeat_payload
from manyforge_supervision.cell_supervisor import run_supervisor


class FakeProcessControl:
    def __init__(self) -> None:
        self.states = {"composer": False, "owner": False}
        self.stop_failures: set[str] = set()
        self.calls: list[tuple[object, ...]] = []

    def running_many(self, names: tuple[str, ...]) -> dict[str, bool]:
        self.calls.append(("running_many", names))
        return {name: self.states[name] for name in names}

    def stop(self, name: str, grace_seconds: int) -> bool:
        self.calls.append(("stop", name, grace_seconds))
        if name in self.stop_failures:
            return False
        self.states[name] = False
        return True

    def force_remove(self, name: str) -> bool:
        self.calls.append(("force_remove", name))
        self.states[name] = False
        return True


def supervisor(control: FakeProcessControl) -> CellSupervisor:
    return CellSupervisor(
        domain=ContainerDomain(
            composer="composer",
            transport_owner="owner",
        ),
        process_control=control,
    )


def test_supervisor_starts_fenced_and_arms_only_complete_domain() -> None:
    control = FakeProcessControl()
    subject = supervisor(control)

    starting = subject.evaluate(
        arm_requested=False,
        shutdown_requested=False,
    )
    assert starting.desired_state == "starting"
    assert not starting.healthy

    incomplete = subject.evaluate(
        arm_requested=True,
        shutdown_requested=False,
    )
    assert incomplete.desired_state == "restart_required"
    assert incomplete.terminal
    assert incomplete.reason_codes == (
        "component_not_running_at_arm:composer",
        "component_not_running_at_arm:owner",
    )

    heartbeat = _heartbeat_payload(
        decision=starting,
        cell_id="lite6-a",
        robot_id="lite6-a",
        component_epoch="cell-supervisor-epoch-1",
        sequence=1,
        critical_progress_sequence=1,
        now_ns=1,
        unclean_terminations=(),
    )
    assert heartbeat["healthState"] == "starting"


def test_unexpected_composer_exit_tears_down_owner_without_restart() -> None:
    control = FakeProcessControl()
    control.states = {"composer": True, "owner": True}
    subject = supervisor(control)
    assert subject.evaluate(
        arm_requested=True,
        shutdown_requested=False,
    ).healthy

    control.calls.clear()
    control.states["composer"] = False
    failed = subject.evaluate(
        arm_requested=True,
        shutdown_requested=False,
    )

    assert failed.desired_state == "restart_required"
    assert failed.reason_codes == (
        "supervised_component_exited:composer",
    )
    assert ("stop", "owner", 20) in control.calls
    assert not control.states["owner"]

    control.states = {"composer": True, "owner": True}
    still_terminal = subject.evaluate(
        arm_requested=True,
        shutdown_requested=False,
    )
    assert still_terminal.desired_state == "restart_required"
    assert not still_terminal.healthy
    assert control.states == {"composer": True, "owner": True}


def test_reduced_domain_health_loss_tears_down_complete_domain() -> None:
    control = FakeProcessControl()
    control.states = {"composer": True, "owner": True}
    subject = supervisor(control)
    assert subject.evaluate(
        arm_requested=True,
        shutdown_requested=False,
        domain_health=DomainHealthObservation(status="healthy"),
    ).healthy

    control.calls.clear()
    failed = subject.evaluate(
        arm_requested=True,
        shutdown_requested=False,
        domain_health=DomainHealthObservation(
            status="lost",
            reason_codes=("composer_supervision_progress_stale",),
        ),
    )

    assert failed.terminal
    assert failed.reason_codes == (
        "composer_supervision_progress_stale",
    )
    assert [call for call in control.calls if call[0] == "stop"] == [
        ("stop", "composer", 10),
        ("stop", "owner", 20),
    ]


def test_docker_process_control_observes_domain_in_one_bounded_call(
    monkeypatch,
) -> None:
    calls: list[tuple[list[str], float]] = []

    def run(
        arguments: list[str],
        *,
        timeout_s: float = 30.0,
    ) -> subprocess.CompletedProcess[str]:
        calls.append((arguments, timeout_s))
        return subprocess.CompletedProcess(
            arguments,
            1,
            stdout='"/composer" true\n',
            stderr="No such object: owner",
        )

    monkeypatch.setattr(DockerCliProcessControl, "_run", staticmethod(run))
    observed = DockerCliProcessControl().running_many(
        ("composer", "owner")
    )

    assert observed == {"composer": True, "owner": False}
    assert len(calls) == 1
    assert calls[0][1] == 1.0


class FakeClock:
    def __init__(self) -> None:
        self.now_ns = 1_000_000_000

    def __call__(self) -> int:
        return self.now_ns

    def advance(self, delta_ns: int) -> None:
        self.now_ns += delta_ns


class FakeHttpResponse:
    def __init__(self, payload: dict[str, object]) -> None:
        self._payload = json.dumps(payload).encode("utf-8")

    def __enter__(self) -> "FakeHttpResponse":
        return self

    def __exit__(self, *_args: object) -> None:
        return None

    def read(self, _maximum: int) -> bytes:
        return self._payload


class FakeUrlOpen:
    def __init__(self) -> None:
        self.payload: dict[str, object] = {}
        self.fail = False

    def __call__(self, _request: object, *, timeout: float) -> FakeHttpResponse:
        assert timeout == 0.2
        if self.fail:
            raise OSError("composer unavailable")
        return FakeHttpResponse(self.payload)


class RaisingHealthProbe:
    def __init__(self) -> None:
        self.bound_epoch = ""

    def bind_supervisor_epoch(self, component_epoch: str) -> None:
        self.bound_epoch = component_epoch

    def observe(self) -> DomainHealthObservation:
        raise RuntimeError("probe crashed")


def supervision_payload(
    *,
    progress_ns: int,
    admitted: bool = True,
    epoch: str = "composer-epoch-1",
    supervisor_epoch: str = "cell-supervisor-epoch-1",
) -> dict[str, object]:
    return {
        "schemaVersion": "manyforge.runtime_status.v1",
        "cellSupervision": {
            "cellId": "lite6-a",
            "authorityComponentEpoch": epoch,
            "evaluatedAtSteadyTimeNs": progress_ns,
            "dispatchAdmitted": admitted,
            "reasonCodes": (
                [] if admitted else ["lifecycle-provider:heartbeat-stale"]
            ),
            "components": [
                {
                    "role": "cell_supervisor",
                    "componentEpoch": supervisor_epoch,
                }
            ],
        },
    }


def test_composer_probe_requires_progress_and_pins_epoch() -> None:
    clock = FakeClock()
    urlopen = FakeUrlOpen()
    probe = ComposerSupervisionProbe(
        url="http://127.0.0.1:9000/api/runtime/status",
        expected_cell_id="lite6-a",
        startup_grace_ns=5_000_000_000,
        maximum_progress_age_ns=2_000_000_000,
        maximum_unhealthy_age_ns=2_000_000_000,
        request_timeout_s=0.2,
        clock_ns=clock,
        urlopen=urlopen,
    )
    probe.bind_supervisor_epoch("cell-supervisor-epoch-1")

    urlopen.payload = {"schemaVersion": "manyforge.runtime_status.v1"}
    assert probe.observe().status == "starting"

    urlopen.payload = supervision_payload(progress_ns=10)
    assert probe.observe().status == "healthy"

    # Exact HTTP replay does not renew receiver-local critical progress.
    clock.advance(1_500_000_000)
    assert probe.observe().status == "healthy"
    clock.advance(600_000_000)
    assert probe.observe().status == "lost"

    urlopen.payload = supervision_payload(
        progress_ns=11,
        epoch="composer-epoch-2",
    )
    assert probe.observe().reason_codes == (
        "composer_component_epoch_changed",
    )


def test_composer_probe_requires_persistent_reduced_unhealthy_state() -> None:
    clock = FakeClock()
    urlopen = FakeUrlOpen()
    probe = ComposerSupervisionProbe(
        url="http://127.0.0.1:9000/api/runtime/status",
        expected_cell_id="lite6-a",
        startup_grace_ns=5_000_000_000,
        maximum_progress_age_ns=2_000_000_000,
        maximum_unhealthy_age_ns=2_000_000_000,
        request_timeout_s=0.2,
        clock_ns=clock,
        urlopen=urlopen,
    )
    probe.bind_supervisor_epoch("cell-supervisor-epoch-1")
    urlopen.payload = supervision_payload(progress_ns=10)
    assert probe.observe().status == "healthy"

    urlopen.payload = supervision_payload(
        progress_ns=11,
        admitted=False,
    )
    transient = probe.observe()
    assert transient.status == "starting"
    assert transient.reason_codes == (
        "lifecycle-provider:heartbeat-stale",
    )

    # Local dispatch remains fenced, but a short unhealthy sample does not
    # tear down the owner process domain.
    clock.advance(1_500_000_000)
    urlopen.payload = supervision_payload(
        progress_ns=12,
        admitted=False,
    )
    assert probe.observe().status == "starting"

    # A healthy advancing sample clears the persistence timer.
    clock.advance(100_000_000)
    urlopen.payload = supervision_payload(progress_ns=13)
    assert probe.observe().status == "healthy"

    urlopen.payload = supervision_payload(
        progress_ns=14,
        admitted=False,
    )
    assert probe.observe().status == "starting"
    clock.advance(2_100_000_000)
    urlopen.payload = supervision_payload(
        progress_ns=15,
        admitted=False,
    )
    failed = probe.observe()
    assert failed.status == "lost"
    assert failed.reason_codes == (
        "lifecycle-provider:heartbeat-stale",
    )


def test_explicit_shutdown_is_dependency_ordered() -> None:
    control = FakeProcessControl()
    control.states = {"composer": True, "owner": True}
    subject = supervisor(control)

    stopped = subject.evaluate(
        arm_requested=False,
        shutdown_requested=True,
    )

    assert stopped.desired_state == "stopped"
    stop_calls = [call for call in control.calls if call[0] == "stop"]
    assert stop_calls == [
        ("stop", "composer", 10),
        ("stop", "owner", 20),
    ]


def test_force_remove_is_recorded_as_unclean_not_physical_stop() -> None:
    control = FakeProcessControl()
    control.states = {"composer": True, "owner": True}
    control.stop_failures.add("owner")
    subject = supervisor(control)

    subject.evaluate(arm_requested=False, shutdown_requested=True)

    assert subject.unclean_terminations == ("owner",)
    assert ("force_remove", "owner") in control.calls


def test_runner_persists_one_terminal_epoch_and_restores_signals(
    tmp_path,
) -> None:
    control = FakeProcessControl()
    control_root = tmp_path / "host-only-control"
    control_root.mkdir()
    (control_root / "cell-supervisor-arm").touch()

    result = run_supervisor(
        runtime_root=tmp_path,
        control_root=control_root,
        cell_id="lite6-a",
        robot_id="lite6-a",
        domain=ContainerDomain(
            composer="composer",
            transport_owner="owner",
        ),
        process_control=control,
        period_seconds=0.25,
        clock_ns=lambda: 123,
        sleep=lambda _seconds: None,
    )

    assert result == 3
    heartbeat = json.loads(
        (tmp_path / "cell-supervisor-heartbeat.json").read_text(
            encoding="utf-8"
        )
    )
    state = json.loads(
        (tmp_path / "cell-supervisor-state.json").read_text(
            encoding="utf-8"
        )
    )
    assert heartbeat["healthState"] == "lost"
    assert heartbeat["desiredState"] == "restart_required"
    assert state["terminalState"] == "restart_required"
    assert state["componentEpoch"] == heartbeat["componentEpoch"]
    assert state["requestedContainment"] == (
        "fence_and_teardown_restart_domain"
    )


def test_runner_contains_domain_if_health_adapter_raises(tmp_path) -> None:
    control = FakeProcessControl()
    control.states = {"composer": True, "owner": True}
    control_root = tmp_path / "host-only-control"
    control_root.mkdir()
    (control_root / "cell-supervisor-arm").touch()
    probe = RaisingHealthProbe()

    result = run_supervisor(
        runtime_root=tmp_path,
        control_root=control_root,
        cell_id="lite6-a",
        robot_id="lite6-a",
        domain=ContainerDomain(
            composer="composer",
            transport_owner="owner",
        ),
        process_control=control,
        period_seconds=0.25,
        domain_health_probe=probe,
        clock_ns=lambda: 123,
        sleep=lambda _seconds: None,
    )

    assert result == 3
    assert probe.bound_epoch.startswith("cell-supervisor-")
    assert [call for call in control.calls if call[0] == "stop"] == [
        ("stop", "composer", 10),
        ("stop", "owner", 20),
    ]
    state = json.loads(
        (tmp_path / "cell-supervisor-state.json").read_text(
            encoding="utf-8"
        )
    )
    assert state["reasonCodes"] == [
        "domain_health_probe_failed:probe crashed"
    ]
