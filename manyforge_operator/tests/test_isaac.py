from __future__ import annotations

import json
from pathlib import Path

import pytest

from manyforge_operator import LaunchInstanceState, LaunchProfile
from manyforge_operator.cli import main
from manyforge_operator.isaac import (
    IsaacLauncher,
    IsaacLauncherConfig,
    RemoteNativeConfig,
)
from manyforge_operator.orchestrator import LauncherError


def _remote(tmp_path: Path) -> RemoteNativeConfig:
    return RemoteNativeConfig(
        source_root="/mnt/nova_ssd/codex-manyforge/runners/manyforge",
        manager_executable="/opt/manyforge/bin/manyforge_manager_daemon",
        package_root="/mnt/nova_ssd/codex-manyforge/builds/current/package",
        node_catalog_path="/opt/manyforge/share/node_catalog.yaml",
        alarm_catalog_path="/opt/manyforge/share/alarm_catalog.yaml",
        publication_root="/mnt/nova_ssd/codex-manyforge/runners/state/publication",
        manager_state_root="/mnt/nova_ssd/codex-manyforge/runners/state/manager",
        runtime_state_root="/mnt/nova_ssd/codex-manyforge/runners/state/runtime",
        execution_state_root="/mnt/nova_ssd/codex-manyforge/runners/state/execution",
        logical_scene_database="/mnt/nova_ssd/codex-manyforge/runners/state/scene.sqlite3",
        historian_database_path="/mnt/nova_ssd/codex-manyforge/runners/state/history.sqlite3",
        state_root="/mnt/nova_ssd/codex-manyforge/runners/state/operator",
        simulation_control_config="/mnt/nova_ssd/codex-manyforge/runners/manyforge/manyforge_ros_bringup/config/isaac_program_simulation_site.json",
        ros_environment_script="/mnt/nova_ssd/codex-manyforge/runners/manyforge/config/ros/isaac_i9.env.sh",
        package_aggregate_sha256="a" * 64,
        compiler_build_manifest_sha256="b" * 64,
        node_environment_sha256="c" * 64,
    )


def _config(tmp_path: Path) -> IsaacLauncherConfig:
    source = tmp_path / "source"
    source.mkdir()
    simulator = tmp_path / "isaac-sim.sh"
    simulator.write_text("#!/bin/sh\n", encoding="utf-8")
    simulator.chmod(0o755)
    (tmp_path / "setup_ros_env.sh").write_text("# Isaac ROS setup\n", encoding="utf-8")
    world = tmp_path / "scene.usda"
    world.write_text("#usda 1.0\n", encoding="utf-8")
    ros_setup = tmp_path / "setup.bash"
    ros_setup.write_text("# setup\n", encoding="utf-8")
    ros_environment = tmp_path / "isaac.env.sh"
    ros_environment.write_text("# env\n", encoding="utf-8")
    ssh = tmp_path / "ssh"
    ssh.write_text("#!/bin/sh\n", encoding="utf-8")
    ssh.chmod(0o755)
    return IsaacLauncherConfig(
        source_root=source,
        isaac_executable=simulator,
        world_path=world,
        state_root=tmp_path / "state",
        remote=_remote(tmp_path),
        local_ros_setup_script=ros_setup,
        local_ros_environment_script=ros_environment,
        ssh_executable=ssh,
    )


def test_remote_native_command_keeps_optional_source_identity_absent(tmp_path: Path):
    launcher = IsaacLauncher(_config(tmp_path))

    command = launcher._remote_native_shell("start")

    assert "--profile native" in command
    assert "--simulation-control-config" in command
    assert "--expected-source-fingerprint" not in command
    assert "--expected-head-commit" not in command
    assert "--expected-head-tree" not in command


def test_persisted_isaac_config_can_be_serialized_after_restart(tmp_path: Path):
    config = _config(tmp_path)
    launcher = IsaacLauncher(config)
    persisted_digest = "d" * 64
    launcher.store.record_path.parent.mkdir(parents=True, exist_ok=True)
    launcher.store.record_path.write_text(
        json.dumps(
            {
                "mechanical": {
                    "launch_input_digest": persisted_digest,
                    "config": config.record(),
                    "processes": {},
                    "native_status": None,
                }
            }
        ),
        encoding="utf-8",
    )

    restarted = IsaacLauncher.from_state_root(config.state_root, config.instance_id)

    mechanical = restarted._mechanical()
    assert mechanical["config"]["remote"]["source_root"] == config.remote.source_root
    assert mechanical["launch_input_digest"] == persisted_digest


def test_local_simulator_command_enables_both_required_ros_extensions(
    tmp_path: Path, monkeypatch
):
    config = _config(tmp_path)
    launcher = IsaacLauncher(config)
    captured: dict[str, object] = {}

    def fake_popen(*args, **kwargs):
        captured["args"] = args
        captured["kwargs"] = kwargs
        return type("Process", (), {"pid": 321})()

    monkeypatch.setattr(
        "manyforge_operator.isaac.subprocess.Popen",
        fake_popen,
    )
    monkeypatch.setattr(
        "manyforge_operator.isaac._proc_observation",
        lambda _pid: (123, "S"),
    )
    monkeypatch.setattr(
        "manyforge_operator.isaac._command_line",
        lambda _pid: ("kit", "--no-window"),
    )
    monkeypatch.setattr("manyforge_operator.isaac._boot_id", lambda: "boot")
    monkeypatch.setattr("manyforge_operator.isaac.os.getpgid", lambda _pid: 321)

    observed = launcher._spawn_local_simulator()

    assert observed["argv"] == ("kit", "--no-window")
    log_text = (config.state_root / config.instance_id / "logs" / "isaac_simulator.log")
    assert log_text.is_file()
    argv = captured["args"][0]
    assert "--enable_extension isaacsim.ros2.bridge" in argv[-1]
    assert "--enable_extension isaacsim.ros2.sim_control" in argv[-1]


def test_degraded_start_recovers_before_attempting_fresh_launch(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    configuration_factory,
):
    launcher = IsaacLauncher(_config(tmp_path))
    configuration = configuration_factory(profile=LaunchProfile.ISAAC)
    mechanical = launcher._mechanical()
    degraded = launcher._snapshot(
        configuration,
        None,
        mechanical,
        remote_status={
            "state": LaunchInstanceState.STOPPED.value,
            "owned_resources": [],
            "resource_observations": [],
            "readiness_observations": [],
        },
        barrier=None,
        state_override=LaunchInstanceState.DEGRADED,
    )
    launcher._write(degraded, mechanical)
    cleanup_calls: list[dict[str, object]] = []
    monkeypatch.setattr(
        launcher,
        "_cleanup",
        lambda value: cleanup_calls.append(dict(value)) or True,
    )
    monkeypatch.setattr(launcher, "_remote_boot_id", lambda: "boot")
    monkeypatch.setattr(launcher, "_configuration", lambda _boot: configuration)
    monkeypatch.setattr(
        launcher,
        "_spawn_local_simulator",
        lambda: (_ for _ in ()).throw(
            LauncherError("simulator_start_failed", "test failure", exit_code=73)
        ),
    )

    with pytest.raises(LauncherError, match="test failure"):
        launcher.start()

    assert launcher.last_start_idempotent is False
    assert len(cleanup_calls) == 2
    stored = launcher.store.read()
    assert stored is not None
    assert stored[0].state is LaunchInstanceState.STOPPED


def test_barrier_failure_preserves_owner_error(tmp_path: Path, monkeypatch):
    launcher = IsaacLauncher(_config(tmp_path))
    monkeypatch.setattr(launcher, "_remote_probe", lambda _meta: True)
    monkeypatch.setattr(
        launcher,
        "_read_remote_barrier",
        lambda _meta: {
            "schema_version": "manyforge.isaac_scene_load_barrier.v1",
            "success": False,
            "error": "GetCurrentWorld failed: timeout",
        },
    )

    with pytest.raises(LauncherError, match="GetCurrentWorld failed: timeout") as raised:
        launcher._wait_for_barrier({"resource_id": "isaac_bridge"})

    assert raised.value.code == "isaac_scene_barrier_failed"


def test_cleanup_accepts_dead_simulator_and_records_unverified_stop(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
):
    launcher = IsaacLauncher(_config(tmp_path))
    monkeypatch.setattr(launcher, "_local_process_authenticated", lambda _meta: False)
    monkeypatch.setattr(launcher, "_remote_stop_process", lambda _meta: True)

    clean = launcher._cleanup(
        {
            "processes": {
                "isaac_simulator": {"resource_id": "isaac_simulator"},
                "isaac_bridge": {"resource_id": "isaac_bridge"},
            },
            "native_status": None,
        }
    )

    assert clean is True
    assert launcher._sim_stop_unverified is True


def test_cli_unknown_stop_is_a_failure(tmp_path: Path):
    assert main(["--state-root", str(tmp_path / "state"), "stop"]) == 70
