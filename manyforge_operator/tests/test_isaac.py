from __future__ import annotations

import json
import os
import stat
from dataclasses import replace
from pathlib import Path
import shlex

import pytest

from manyforge_operator import LaunchInstanceState, LaunchProfile
from manyforge_operator.cli import _isaac_config, _parser, main
from manyforge_operator.isaac import (
    IsaacLauncher,
    IsaacLauncherConfig,
    RemoteNativeConfig,
)
from manyforge_operator.orchestrator import LauncherError


def _remote(tmp_path: Path) -> RemoteNativeConfig:
    return RemoteNativeConfig(
        target="operator@runtime-host",
        advertised_host="runtime-host",
        source_root="/srv/manyforge/manyforge",
        manager_executable="/opt/manyforge/bin/manyforge_manager_daemon",
        install_root="/srv/manyforge/package",
        node_catalog_path="/opt/manyforge/share/node_catalog.yaml",
        alarm_catalog_path="/opt/manyforge/share/alarm_catalog.yaml",
        state_root="/srv/manyforge/operator-state",
        simulation_control_config="/srv/manyforge/manyforge/manyforge_ros_bringup/config/isaac_program_simulation_site.json",
        bridge_state_root="/srv/manyforge/bridge-state",
        ros_setup_script="/opt/ros/lyrical/setup.bash",
        ros_environment_script="/srv/manyforge/ros_env.sh",
        package_aggregate_sha256="a" * 64,
        compiler_build_manifest_sha256="b" * 64,
        node_environment_sha256="c" * 64,
    )


def _config(tmp_path: Path) -> IsaacLauncherConfig:
    source = tmp_path / "source"
    source.mkdir()
    ssh = tmp_path / "ssh"
    ssh.write_text("#!/bin/sh\n", encoding="utf-8")
    ssh.chmod(0o755)
    return IsaacLauncherConfig(
        source_root=source,
        state_root=tmp_path / "state",
        remote=_remote(tmp_path),
        simulator_host="isaac-host",
        static_peers=("isaac-host", "runtime-host"),
        ssh_executable=ssh,
    )


def _container_config(tmp_path: Path) -> IsaacLauncherConfig:
    config = _config(tmp_path)
    return replace(config, remote=replace(config.remote,
        container_image_id="sha256:" + "e" * 64, container_name="manyforge-g-fixture",
        cache_root="/srv/manyforge/cache", runtime_root="/srv/manyforge/runtime",
        evidence_root="/srv/manyforge/evidence"))



def _isaac_arguments(config: IsaacLauncherConfig) -> list[str]:
    remote = config.remote
    arguments = [
        "start", "--profile", "isaac", "--source-root", str(config.source_root),
        "--remote-target", remote.target,
        "--advertised-host", remote.advertised_host,
        "--simulator-host", config.simulator_host,
    ]
    for peer in config.static_peers:
        arguments += ["--static-peer", peer]
    for field in ("source_root", "container_image_id", "container_name", "cache_root",
                  "runtime_root", "evidence_root", "manager_executable", "install_root",
                  "node_catalog_path", "alarm_catalog_path", "state_root",
                  "simulation_control_config", "bridge_state_root", "ros_setup_script",
                  "ros_environment_script"):
        arguments += ["--remote-" + field.replace("_", "-"), getattr(remote, field)]
    for field in ("package_aggregate_sha256", "compiler_build_manifest_sha256",
                  "node_environment_sha256"):
        arguments += ["--" + field.replace("_", "-"), getattr(remote, field)]
    return arguments


def test_isaac_hosts_and_paths_are_explicit_inputs(tmp_path, monkeypatch):
    config = _container_config(tmp_path)
    captured = []
    monkeypatch.setattr("manyforge_operator.cli.IsaacLauncherConfig",
                        lambda **kwargs: captured.append(kwargs))
    _isaac_config(_parser().parse_args(_isaac_arguments(config)))
    remote = captured[-1]["remote"]
    assert remote.target == "operator@runtime-host"
    assert remote.advertised_host == "runtime-host"
    assert remote.bridge_state_root == "/srv/manyforge/bridge-state"
    assert remote.ros_setup_script == "/opt/ros/lyrical/setup.bash"
    assert captured[-1]["simulator_host"] == "isaac-host"
    assert captured[-1]["static_peers"] == ("isaac-host", "runtime-host")


@pytest.mark.parametrize(
    "flag",
    ["--remote-target", "--advertised-host", "--simulator-host", "--static-peer",
     "--remote-source-root", "--remote-bridge-state-root", "--remote-ros-setup-script"],
)
def test_isaac_start_without_host_or_path_refuses_with_typed_error(tmp_path, capsys, flag):
    arguments = _isaac_arguments(_container_config(tmp_path))
    while flag in arguments:
        index = arguments.index(flag)
        del arguments[index:index + 2]
    exit_code = main(["--state-root", str(tmp_path / "state"), "--json", *arguments])
    result = json.loads(capsys.readouterr().out)
    assert exit_code == 64
    assert result["outcome"] == "failed"
    assert result["error"]["code"] == "operator_configuration_invalid"
    assert flag.removeprefix("--") in result["error"]["message"]
    assert not (tmp_path / "state" / "workbench").exists()


def test_remote_native_command_keeps_optional_source_identity_absent(tmp_path: Path):
    launcher = IsaacLauncher(_config(tmp_path))

    command = launcher._remote_native_shell("start")

    assert "--profile standard" in command
    assert "--simulation-control-config" in command
    assert "--expected-source-fingerprint" not in command
    assert "--expected-head-commit" not in command
    assert "--expected-head-tree" not in command


def test_explicit_arm_topic_remains_an_input_identity(tmp_path: Path):
    original = _config(tmp_path)
    configured = replace(original, joint_commands_topic="/cell_a/arm/joint_commands")
    assert configured.input_digest() != original.input_digest()
    assert configured.record()["joint_commands_topic"] == configured.joint_commands_topic


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
                }
            }
        ),
        encoding="utf-8",
    )

    restarted = IsaacLauncher.from_state_root(config.state_root, config.instance_id)

    mechanical = restarted._mechanical()
    assert mechanical["config"]["remote"]["source_root"] == config.remote.source_root
    assert mechanical["launch_input_digest"] == persisted_digest


def test_retired_simulator_ownership_record_is_refused_without_cleanup(
    tmp_path: Path,
):
    config = _config(tmp_path)
    launcher = IsaacLauncher(config)
    retired_config = config.record()
    retired_config["isaac_executable"] = "/opt/isaac-sim/isaac-sim.sh"
    launcher.store.record_path.parent.mkdir(parents=True, exist_ok=True)
    launcher.store.record_path.write_text(
        json.dumps(
            {
                "mechanical": {
                    "config": retired_config,
                    "processes": {"isaac_simulator": {"pid": 123}},
                }
            }
        ),
        encoding="utf-8",
    )

    with pytest.raises(
        LauncherError,
        match="recreate the operator instance after reconciling the existing remote runtime",
    ):
        IsaacLauncher.from_state_root(config.state_root, config.instance_id)


def test_degraded_start_recovers_before_attempting_fresh_launch(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    configuration_factory,
):
    launcher = IsaacLauncher(_container_config(tmp_path))
    monkeypatch.setattr(launcher, "_verify_remote_source", lambda expected: None)
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
        "_remote_native_start",
        lambda: (_ for _ in ()).throw(
            LauncherError("runtime_command_failed", "remote start test failure", exit_code=73)
        ),
    )

    with pytest.raises(LauncherError, match="remote start test failure"):
        launcher.start()

    assert launcher.last_start_idempotent is False
    assert len(cleanup_calls) == 2
    stored = launcher.store.read()
    assert stored is not None
    assert stored[0].state is LaunchInstanceState.STOPPED


def test_cli_unknown_stop_is_a_failure(tmp_path: Path):
    assert main(["--state-root", str(tmp_path / "state"), "stop"]) == 66


def test_start_invokes_remote_manager_only_without_owned_simulator_or_process(
    tmp_path, monkeypatch, configuration_factory
):
    launcher = IsaacLauncher(_container_config(tmp_path))
    monkeypatch.setattr(launcher, "_verify_remote_source", lambda expected: None)
    configuration = configuration_factory(profile=LaunchProfile.ISAAC)
    calls = []
    monkeypatch.setattr(launcher, "_remote_boot_id", lambda: "boot")
    monkeypatch.setattr(launcher, "_configuration", lambda boot: configuration)
    native = {"state": "ready", "configuration": configuration.model_dump(mode="json"),
              "owned_resources": [{"resource_id": "manager", "component_id": "manager", "kind": "process",
                                   "host_id": "runtime", "pid": 442, "start_ticks": 123,
                                   "boot_id": "22222222-2222-2222-2222-222222222222", "process_group_id": 442}],
              "resource_observations": [{"resource_id": "manager", "presence": "present", "observed_at_unix_ns": 1}],
              "readiness_observations": [{"component_id": "manager", "authority": "owner_issued", "state": "ready",
                                          "source_schema": "manyforge.manager.health.v1", "owner_id": "manager",
                                          "payload_sha256": "a" * 64, "observed_at_unix_ns": 1}]}
    monkeypatch.setattr(launcher, "_remote_native_start", lambda: (calls.append("manager") or native))
    observed = launcher.start()
    assert calls == ["manager"]
    assert observed.state is LaunchInstanceState.READY
    assert [row.component_id for row in observed.readiness_observations] == ["manager"]
    assert {row.resource_id: row.presence.value for row in observed.resource_observations} == {
        "manager": "present"}
    assert "isaac_simulator" not in {row.resource_id for row in observed.owned_resources}
    assert launcher._mechanical()["processes"] == {}


def _container_readback(launcher):
    return {
        "container_id": "d" * 64,
        "image_id": launcher.config.remote.container_image_id,
        "labels": {"manyforge.launch-instance": launcher.config.instance_id,
                   "manyforge.launch-input-digest": launcher.config.input_digest()},
        "running": True,
        "workbench": {
            "manager": {"source": {"available": True, "freshness": "fresh", "known_gaps": []},
                        "status": {"manager_ready": True, "components": [{"lifecycle_state": "inactive"}]}},
            "headless": {"status": "ready", "headless_projection_ready": True},
            "program_runner": {"started": True, "closed": False},
            "composer": {"status": "ready"},
            "timing": {"build_elapsed_seconds": 120, "readiness_elapsed_seconds": 1},
        },
    }


def test_g_container_uses_existing_workbench_and_exact_inputs_without_host_ros(tmp_path):
    launcher = IsaacLauncher(_container_config(tmp_path))
    command = shlex.split(launcher._container_shell("start"))
    assert command[-2:] == [launcher.config.remote.source_root + "/scripts/compose-dev.sh", "start"]
    assert "--project" not in command and "--bootstrap-project" not in command
    environment = dict(word.split("=", 1) for word in command[1:-3])
    assert environment["MANYFORGE_DEV_IMAGE"] == launcher.config.remote.container_image_id
    assert environment["MANYFORGE_DEV_LAUNCH_INPUT_DIGEST"] == launcher.config.input_digest()
    assert environment["MANYFORGE_DEV_RUNTIME_STATE_ROOT"] == launcher.config.remote.runtime_root
    assert environment["MANYFORGE_DEV_EVIDENCE_ROOT"] == launcher.config.remote.evidence_root
    assert "MANYFORGE_DEV_ISAAC_SITE_SETTINGS" not in environment
    assert environment["ROS_DOMAIN_ID"] == str(launcher.config.ros_domain)
    assert environment["ROS_STATIC_PEERS"] == ";".join(launcher.config.static_peers)
    assert "source " not in launcher._container_shell("start")
    assert launcher.config.remote.ros_setup_script not in command


def test_container_ready_reads_owner_facts_and_keeps_exact_birth_identity(tmp_path):
    launcher = IsaacLauncher(_container_config(tmp_path))
    readback = _container_readback(launcher)
    status = launcher._container_status(readback)
    assert status["state"] == "ready"  # Inactive component peers do not change G readiness.
    assert len(status["readiness_observations"]) == 4
    resource = status["owned_resources"][0]
    assert resource["container_id"] == "d" * 64
    assert resource["log_reference"] == "docker logs " + "d" * 64
    launcher._remote_status = status
    assert launcher._container_environment()["MANYFORGE_DEV_EXPECTED_CONTAINER_ID"] == "d" * 64
    absent = launcher._container_status({"container_id": None, "running": False, "workbench": None})
    assert absent["state"] == "stopped"
    assert absent["owned_resources"] == status["owned_resources"]
    assert absent["resource_observations"][0]["presence"] == "absent"


@pytest.mark.parametrize("changed", ["container_id", "image_id", "instance", "digest"])
def test_container_readback_rejects_foreign_or_replaced_occurrence(tmp_path, changed):
    launcher = IsaacLauncher(_container_config(tmp_path))
    readback = _container_readback(launcher)
    launcher._remote_status = launcher._container_status(readback)
    if changed in {"container_id", "image_id"}:
        readback[changed] = ("sha256:" if changed == "image_id" else "") + "f" * 64
    else:
        readback["labels"]["manyforge.launch-" + ("instance" if changed == "instance" else "input-digest")] = "foreign"
    with pytest.raises(LauncherError, match="exact launch identity"):
        launcher._container_status(readback)


@pytest.mark.parametrize("change", ["stale", "manager_false", "missing_runner", "no_owners", "malformed_source"])
def test_container_liveness_and_timing_cannot_substitute_for_ready_owners(tmp_path, change):
    launcher = IsaacLauncher(_container_config(tmp_path))
    readback = _container_readback(launcher)
    owners = readback["workbench"]
    if change == "stale":
        owners["manager"]["source"]["freshness"] = "stale"
    elif change == "manager_false":
        owners["manager"]["status"]["manager_ready"] = False
    elif change == "missing_runner":
        del owners["program_runner"]
    elif change == "malformed_source":
        owners["manager"]["source"] = None
    else:
        readback["workbench"] = None
    assert launcher._container_status(readback)["state"] == "degraded"


def test_failed_container_start_recovers_identity_then_remote_stop_is_attempted(tmp_path, monkeypatch):
    launcher = IsaacLauncher(_container_config(tmp_path))
    readback = _container_readback(launcher)
    readback["workbench"] = None
    calls = []

    def operation(name):
        calls.append(name)
        if name == "start":
            raise LauncherError("runtime_command_failed", "partial startup", exit_code=73)
        return launcher._container_status(readback)

    monkeypatch.setattr(launcher, "_remote_container_operation", operation)
    monkeypatch.setattr(launcher, "_remote_native_build", lambda: calls.append("build-native"))
    with pytest.raises(LauncherError, match="partial startup"):
        launcher._remote_native_start()
    assert launcher._remote_status["owned_resources"][0]["container_id"] == "d" * 64
    def stop():
        calls.append("export_stop")
        raise LauncherError("runtime_command_failed", "export failed; retained", exit_code=73)
    monkeypatch.setattr(launcher, "_remote_native_stop", stop)
    assert launcher._cleanup({"native_status": launcher._remote_status}) is False
    assert calls == ["build-native", "start", "status", "status", "export_stop"]


def test_container_start_runs_the_native_build_as_its_own_call_first(tmp_path, monkeypatch):
    launcher = IsaacLauncher(_container_config(tmp_path))
    readback = _container_readback(launcher)
    calls = []
    build_fails = False

    def ssh(command, *, timeout=None):
        operation = shlex.split(command)[-1]
        calls.append((operation, timeout))
        if operation == "build-native" and build_fails:
            raise LauncherError("runtime_host_unavailable", "Native build failed", exit_code=75)
        return json.dumps(readback) if operation == "start" else "Native build complete.\n"

    monkeypatch.setattr(launcher, "_ssh", ssh)
    assert launcher._remote_native_start()["state"] == "ready"
    # The build has its own bound (cold build: about 30-40 min); start's
    # 930 s then bounds readiness only.
    assert calls == [("build-native", 2700), ("start", 930)]
    command = shlex.split(launcher._container_shell("build-native"))
    assert command[-2:] == [launcher.config.remote.source_root + "/scripts/compose-dev.sh", "build-native"]
    calls.clear()
    build_fails = True
    with pytest.raises(LauncherError, match="Native build failed"):
        launcher._remote_native_start()
    assert calls == [("build-native", 2700)]  # A failed build starts nothing.


def test_remote_source_check_preserves_existing_g_clean_exact_source_rule(tmp_path, monkeypatch, source_factory):
    launcher = IsaacLauncher(_container_config(tmp_path))
    actual = source_factory()
    commands = []
    monkeypatch.setattr(launcher, "_ssh_json", lambda command, **kwargs: (commands.append(command) or actual.model_dump(mode="json")))
    launcher._verify_remote_source(actual)
    assert "scripts/lib/source_identity.py --repo-root" in commands[0]
    assert "source " not in commands[0] and "docker" not in commands[0]
    actual = source_factory(dirty=True)
    with pytest.raises(LauncherError, match="dirty"):
        launcher._verify_remote_source(source_factory())


def test_stop_calls_remote_manager_without_owned_simulator_or_process(
    tmp_path, monkeypatch, configuration_factory
):
    launcher = IsaacLauncher(_container_config(tmp_path))
    configuration = configuration_factory(profile=LaunchProfile.ISAAC)
    ready = {
        "state": "ready",
        "owned_resources": [{
            "resource_id": "manager", "component_id": "manager", "kind": "process",
            "host_id": "runtime", "pid": 442, "start_ticks": 123,
            "boot_id": "22222222-2222-2222-2222-222222222222", "process_group_id": 442,
        }],
        "resource_observations": [{
            "resource_id": "manager", "presence": "present", "observed_at_unix_ns": 1,
        }],
        "readiness_observations": [{
            "component_id": "manager", "authority": "owner_issued", "state": "ready",
            "source_schema": "manyforge.manager.health.v1", "owner_id": "manager",
            "payload_sha256": "a" * 64, "observed_at_unix_ns": 1,
        }],
    }
    stopped = {
        "state": "stopped",
        "owned_resources": ready["owned_resources"],
        "resource_observations": [{
            "resource_id": "manager", "presence": "absent", "observed_at_unix_ns": 2,
        }],
        "readiness_observations": [],
    }
    launcher._remote_status = ready
    launch_status = launcher._snapshot(
        configuration, None, launcher._mechanical(), remote_status=ready
    )
    launcher._write(launch_status, launcher._mechanical())
    calls = []
    remote_statuses = iter((ready, stopped))
    monkeypatch.setattr(
        launcher, "_remote_native_status", lambda: (calls.append("status") or next(remote_statuses))
    )
    monkeypatch.setattr(
        launcher, "_remote_native_stop", lambda: (calls.append("manager_stop") or stopped)
    )

    observed = launcher.stop()

    assert observed is not None
    assert observed.state is LaunchInstanceState.STOPPED
    assert calls == ["status", "manager_stop", "status"]
    assert "isaac_simulator" not in {row.resource_id for row in observed.owned_resources}
    assert launcher._mechanical()["processes"] == {}


def test_state_directories_it_creates_are_owner_only_under_a_group_umask(tmp_path: Path):
    # Ubuntu's user-private-group default (0002) must not make the launcher
    # refuse a state directory it created itself.
    previous = os.umask(0o002)
    try:
        config = _config(tmp_path)
        launcher = IsaacLauncher(config)
        restarted = IsaacLauncher.from_state_root(config.state_root, config.instance_id)
    finally:
        os.umask(previous)
    for directory in (config.state_root, config.instance_root):
        assert stat.S_IMODE(directory.stat().st_mode) == 0o700
    assert launcher.store.root == restarted.store.root
