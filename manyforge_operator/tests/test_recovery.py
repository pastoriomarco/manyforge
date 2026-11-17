from __future__ import annotations

from pathlib import Path

import pytest

from manyforge_operator import (
    FailureRecord,
    LaunchInstanceState,
    LaunchProfile,
    ResourceKind,
)
from manyforge_operator.application_processes import ApplicationProcess
from manyforge_operator.orchestrator import (
    LauncherError,
    NativeLauncher,
    NativeLauncherConfig,
    ProcessSpec,
)
from manyforge_operator.package_aggregate import package_aggregate_sha256


def _config(tmp_path: Path) -> NativeLauncherConfig:
    source = tmp_path / "source"
    source.mkdir()
    (source / "examples").mkdir()
    manager = tmp_path / "manager-daemon"
    manager.write_text("#!/bin/sh\n", encoding="utf-8")
    manager.chmod(0o755)
    package = tmp_path / "package"
    package.mkdir()
    node = package / "node.yaml"
    alarm = package / "alarm.yaml"
    node.write_text("nodes: []\n", encoding="utf-8")
    alarm.write_text("alarms: []\n", encoding="utf-8")
    return NativeLauncherConfig(
        source_root=source,
        manager_executable=manager,
        install_root=package,
        node_catalog_path=node,
        alarm_catalog_path=alarm,
        state_root=tmp_path / "operator-state",
        package_aggregate_sha256=package_aggregate_sha256(package),
        compiler_build_manifest_sha256="b" * 64,
        node_environment_sha256="c" * 64,
    )


def _mechanical_process() -> dict[str, object]:
    # The launcher records only the Manager it started (spec 3.3, S3).
    return {
        "resource_id": "manager",
        "component_id": "manager",
        "kind": ResourceKind.PROCESS.value,
        "host_id": "runtime",
        "pid": 999999,
        "start_ticks": 1,
        "boot_id": "22222222-2222-2222-2222-222222222222",
        "process_group_id": 999999,
        "log_reference": "manager.log",
        "argv": [],
    }


def _write_record(launcher, status, mechanical) -> None:
    with launcher.store.lock():
        launcher.store.write(status, mechanical)


def test_status_reconciles_a_dead_recorded_child_to_degraded(
    tmp_path: Path, configuration_factory, status_factory
):
    launcher = NativeLauncher(_config(tmp_path))
    status = status_factory(configuration_factory(), state=LaunchInstanceState.READY)
    _write_record(
        launcher,
        status,
        {"profile": "standard", "processes": {"manager": _mechanical_process()}},
    )

    observed = launcher.status()

    assert observed is not None
    assert observed.state is LaunchInstanceState.DEGRADED
    assert observed.last_failure is not None
    assert observed.last_failure.reason_code == "component_unavailable"


def test_repeated_status_and_stop_preserve_a_proven_stopped_record(
    tmp_path: Path, configuration_factory, status_factory
):
    launcher = NativeLauncher(_config(tmp_path))
    status = status_factory(
        configuration_factory(), state=LaunchInstanceState.STOPPED, with_resources=False
    )
    _write_record(launcher, status, {"profile": "standard", "processes": {}})

    assert launcher.status().state is LaunchInstanceState.STOPPED
    assert launcher.status().state is LaunchInstanceState.STOPPED
    assert launcher.stop().state is LaunchInstanceState.STOPPED
    assert launcher.last_stop_already_stopped is True


def test_status_rejects_a_present_process_over_a_stopped_record(
    tmp_path: Path, configuration_factory, status_factory, monkeypatch
):
    launcher = NativeLauncher(_config(tmp_path))
    status = status_factory(
        configuration_factory(), state=LaunchInstanceState.STOPPED, with_resources=False
    )
    _write_record(
        launcher,
        status,
        {"profile": "standard", "processes": {"manager": _mechanical_process()}},
    )
    monkeypatch.setattr(
        "manyforge_operator.orchestrator._process_is_authenticated",
        lambda _record: True,
    )

    with pytest.raises(LauncherError) as raised:
        launcher.status()

    assert raised.value.code == "stale_launch_record"


def test_start_conflicts_with_a_present_process_over_stopped_record(
    tmp_path: Path, configuration_factory, status_factory, monkeypatch
):
    launcher = NativeLauncher(_config(tmp_path))
    old_configuration = configuration_factory()
    requested_configuration = configuration_factory(profile=LaunchProfile.ISAAC)
    status = status_factory(
        old_configuration, state=LaunchInstanceState.STOPPED, with_resources=False
    )
    _write_record(
        launcher,
        status,
        {"profile": "standard", "processes": {"manager": _mechanical_process()}},
    )
    monkeypatch.setattr(
        "manyforge_operator.orchestrator._configuration",
        lambda _config: requested_configuration,
    )
    monkeypatch.setattr(
        "manyforge_operator.orchestrator._process_is_authenticated",
        lambda _record: True,
    )

    with pytest.raises(LauncherError) as raised:
        launcher.start()

    assert raised.value.code == "incompatible_owner"


def test_failed_cleanup_after_start_failure_is_degraded(
    tmp_path: Path, configuration_factory, status_factory, monkeypatch
):
    launcher = NativeLauncher(_config(tmp_path))
    configuration = configuration_factory()
    starting = status_factory(
        configuration, state=LaunchInstanceState.STARTING, with_resources=False
    )
    _write_record(launcher, starting, {"profile": "standard", "processes": {}})
    monkeypatch.setattr(launcher, "_cleanup_authenticated", lambda: False)

    failure = FailureRecord(
        reason_code="readiness_timeout",
        component_id="launcher",
        detail="owner did not become ready",
        occurred_at_unix_ns=1,
    )
    with pytest.raises(LauncherError) as raised:
        launcher._finish_failed_start(configuration, failure)

    assert raised.value.code == "cleanup_deadline_exceeded"
    assert raised.value.status is not None
    assert raised.value.status.state is LaunchInstanceState.DEGRADED
    assert raised.value.status.last_failure.reason_code == "cleanup_deadline_exceeded"


def test_manager_spawn_failure_ends_the_start_with_its_failure_recorded(
    tmp_path: Path, configuration_factory, monkeypatch
):
    launcher = NativeLauncher(_config(tmp_path))
    configuration = configuration_factory()
    monkeypatch.setattr(
        "manyforge_operator.orchestrator._configuration",
        lambda _config: configuration,
    )
    monkeypatch.setattr(launcher, "_port_available", lambda: None)
    monkeypatch.setattr(
        "manyforge_operator.orchestrator.native_commands",
        lambda _config, previous_session_id=None: (
            ProcessSpec("manager", ("manager",), {}, tmp_path),
            (ApplicationProcess("headless", ("/bin/true",)),),
        ),
    )
    monkeypatch.setattr(
        "manyforge_operator.orchestrator._process_is_authenticated",
        lambda _record: False,
    )
    spawned: list[str] = []

    def spawn(spec):
        spawned.append(spec.component_id)
        raise RuntimeError("Manager could not start")

    monkeypatch.setattr(launcher, "_spawn", spawn)

    with pytest.raises(LauncherError) as raised:
        launcher.start()

    assert raised.value.code == "native_start_failed"
    assert spawned == ["manager"]
    assert raised.value.status is not None
    assert raised.value.status.state is LaunchInstanceState.STOPPED
    assert raised.value.status.last_failure.reason_code == "native_start_failed"


def _live_manager(monkeypatch, *, ends_on_request: bool):
    """A recorded, authenticated Manager; `os.kill` is observed, not sent."""

    import signal as signal_module

    alive = {999999}
    signals: list[tuple[int, int]] = []
    monkeypatch.setattr(
        "manyforge_operator.orchestrator._process_is_authenticated",
        lambda record: int(record["pid"]) in alive,
    )

    def kill(pid: int, number: int) -> None:
        signals.append((pid, number))
        if ends_on_request:
            alive.discard(pid)

    monkeypatch.setattr("manyforge_operator.orchestrator.os.kill", kill)

    def no_group_signal(*_args):
        raise AssertionError("a live Manager is not signalled as a group")

    monkeypatch.setattr("manyforge_operator.orchestrator.os.killpg", no_group_signal)
    return signals, signal_module.SIGTERM


def test_recovery_cleanup_is_one_stop_request_followed_to_the_manager_end(
    tmp_path: Path, monkeypatch
):
    # Start-failure and recovery cleanup: one SIGTERM (the Manager would count
    # a second request as a forced stop), then wait for its end, no time limit.
    launcher = NativeLauncher(_config(tmp_path))
    signals, sigterm = _live_manager(monkeypatch, ends_on_request=True)

    assert launcher._cleanup_authenticated({"manager": _mechanical_process()})
    assert signals == [(999999, sigterm)]


def test_stop_sends_one_sigterm_to_a_live_manager_and_follows_it_to_its_end(
    tmp_path: Path, configuration_factory, status_factory, monkeypatch, capsys
):
    # OD-187: each `stop` is one stop request, one SIGTERM to the recorded
    # Manager (its launcher has ended here); `stop` then follows the recorded
    # process to its end with no time limit of its own.
    launcher = NativeLauncher(_config(tmp_path))
    status = status_factory(configuration_factory(), state=LaunchInstanceState.READY)
    _write_record(launcher, status, {
        "profile": "standard",
        "processes": {"manager": _mechanical_process()},
        "paths": {"manager_run_root": str(tmp_path / "manager-run")},
    })
    signals, sigterm = _live_manager(monkeypatch, ends_on_request=True)

    stopped = launcher.stop()

    assert stopped.state is LaunchInstanceState.STOPPED
    assert signals == [(999999, sigterm)]
    assert ("stop: asked the Manager (pid 999999) to stop the application; waiting "
            "for it to end. Its log shows each step; a second stop request forces "
            "the stop.") in capsys.readouterr().err


def test_a_stop_while_the_instance_is_owned_is_one_more_request(
    tmp_path: Path, configuration_factory, status_factory, monkeypatch
):
    # A running start (or another `stop`) holds the instance: this `stop` is
    # one more SIGTERM to its Manager, then returns the owner's final record.
    from manyforge_operator.orchestrator import _InstanceBusy

    launcher = NativeLauncher(_config(tmp_path))
    status = status_factory(configuration_factory(), state=LaunchInstanceState.READY)
    _write_record(launcher, status, {
        "profile": "standard",
        "processes": {"manager": _mechanical_process()},
        "paths": {"manager_run_root": str(tmp_path / "manager-run")},
    })
    signals, sigterm = _live_manager(monkeypatch, ends_on_request=False)

    def busy():
        raise _InstanceBusy()

    monkeypatch.setattr(launcher, "_stop_locked", busy)

    assert launcher.stop().state is LaunchInstanceState.READY
    assert launcher.stop().state is LaunchInstanceState.READY
    assert signals == [(999999, sigterm), (999999, sigterm)]
