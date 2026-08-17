from __future__ import annotations

from dataclasses import replace
from pathlib import Path

import pytest

from manyforge_operator import (
    FailureRecord,
    LaunchInstanceState,
    LaunchProfile,
    ResourceKind,
)
from manyforge_operator.orchestrator import (
    LauncherError,
    NativeLauncher,
    NativeLauncherConfig,
    ProcessSpec,
)


def _config(tmp_path: Path) -> NativeLauncherConfig:
    source = tmp_path / "source"
    source.mkdir()
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
        package_root=package,
        node_catalog_path=node,
        alarm_catalog_path=alarm,
        publication_root=tmp_path / "publication",
        manager_state_root=tmp_path / "manager-state",
        runtime_state_root=tmp_path / "runtime-state",
        execution_state_root=tmp_path / "execution-state",
        logical_scene_database=tmp_path / "scene.sqlite3",
        historian_database_path=tmp_path / "history.sqlite3",
        state_root=tmp_path / "operator-state",
        package_aggregate_sha256="a" * 64,
        compiler_build_manifest_sha256="b" * 64,
        node_environment_sha256="c" * 64,
    )


def _mechanical_process() -> dict[str, object]:
    return {
        "resource_id": "headless",
        "component_id": "headless",
        "kind": ResourceKind.PROCESS.value,
        "host_id": "runtime",
        "pid": 999999,
        "start_ticks": 1,
        "boot_id": "22222222-2222-2222-2222-222222222222",
        "process_group_id": 999999,
        "log_reference": "headless.log",
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
        {"profile": "native", "processes": {"headless": _mechanical_process()}},
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
    _write_record(launcher, status, {"profile": "native", "processes": {}})

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
        {"profile": "native", "processes": {"headless": _mechanical_process()}},
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
        {"profile": "native", "processes": {"headless": _mechanical_process()}},
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
    _write_record(launcher, starting, {"profile": "native", "processes": {}})
    monkeypatch.setattr(launcher, "_cleanup_authenticated", lambda: False)

    failure = FailureRecord(
        reason_code="readiness_timeout",
        component_id="native",
        detail="owner did not become ready",
        occurred_at_unix_ns=1,
    )
    with pytest.raises(LauncherError) as raised:
        launcher._finish_failed_start(configuration, failure)

    assert raised.value.code == "cleanup_deadline_exceeded"
    assert raised.value.status is not None
    assert raised.value.status.state is LaunchInstanceState.DEGRADED
    assert raised.value.status.last_failure.reason_code == "cleanup_deadline_exceeded"


def test_readiness_timeout_preserves_terminal_failure_without_recleaning(
    tmp_path: Path, configuration_factory, status_factory, monkeypatch
):
    launcher = NativeLauncher(
        replace(_config(tmp_path), readiness_timeout_seconds=0.01)
    )
    configuration = configuration_factory()
    stopped = status_factory(
        configuration, state=LaunchInstanceState.STOPPED, with_resources=False
    )
    calls: list[FailureRecord] = []
    monkeypatch.setattr(
        "manyforge_operator.orchestrator._configuration",
        lambda _config: configuration,
    )
    monkeypatch.setattr(launcher, "_port_available", lambda: None)
    monkeypatch.setattr(
        "manyforge_operator.orchestrator.native_commands", lambda _config: ()
    )
    monkeypatch.setattr(
        launcher, "_readiness", lambda: (False, [], "owner did not become ready")
    )
    monkeypatch.setattr("manyforge_operator.orchestrator.time.sleep", lambda _seconds: None)

    def finish(_configuration, failure, readiness=()):
        calls.append(failure)
        return stopped

    monkeypatch.setattr(launcher, "_finish_failed_start", finish)

    with pytest.raises(LauncherError) as raised:
        launcher.start()

    assert raised.value.code == "readiness_timeout"
    assert raised.value.status is stopped
    assert len(calls) == 1


def test_spawned_component_is_retained_when_next_spawn_raises(
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
        lambda _config: (
            ProcessSpec("manager", ("manager",), {}, tmp_path),
            ProcessSpec("headless", ("headless",), {}, tmp_path),
        ),
    )
    monkeypatch.setattr(
        "manyforge_operator.orchestrator._process_is_authenticated",
        lambda _record: False,
    )
    spawned: list[str] = []

    def spawn(spec):
        spawned.append(spec.component_id)
        if spec.component_id == "headless":
            raise RuntimeError("second component failed")
        launcher._managed[spec.component_id] = type(
            "Managed",
            (),
            {
                "component_id": spec.component_id,
                "pid": 1,
                "start_ticks": 1,
                "boot_id": "22222222-2222-2222-2222-222222222222",
                "process_group_id": 2,
                "argv": spec.argv,
                "log_path": tmp_path / "manager.log",
            },
        )()
        return launcher._managed[spec.component_id]

    monkeypatch.setattr(launcher, "_spawn", spawn)

    with pytest.raises(LauncherError) as raised:
        launcher.start()

    assert raised.value.code == "native_start_failed"
    assert spawned == ["manager", "headless"]
    record = launcher.store.read()
    assert record is not None
    assert "manager" in record[1]["processes"]
