from __future__ import annotations

import json
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


@pytest.fixture(autouse=True)
def detached_journal(monkeypatch):
    # Unit tests inspect command routing without starting a simulator or journald.
    def command(instance, component, argv):
        identifier = f"manyforge-{instance}-{component}"
        return ["/usr/bin/systemd-cat", "-t", identifier, "--", *argv], f"journalctl -t {identifier}"
    monkeypatch.setattr("manyforge_operator.isaac._journal_command", command)


def _remote(tmp_path: Path) -> RemoteNativeConfig:
    return RemoteNativeConfig(
        source_root="/mnt/nova_ssd/codex-manyforge/runners/manyforge",
        manager_executable="/opt/manyforge/bin/manyforge_manager_daemon",
        install_root="/mnt/nova_ssd/codex-manyforge/builds/current/package",
        node_catalog_path="/opt/manyforge/share/node_catalog.yaml",
        alarm_catalog_path="/opt/manyforge/share/alarm_catalog.yaml",
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


def _container_config(tmp_path: Path) -> IsaacLauncherConfig:
    config = _config(tmp_path)
    return replace(config, remote=replace(config.remote,
        container_image_id="sha256:" + "e" * 64, container_name="manyforge-g-fixture",
        cache_root="/srv/manyforge/cache", runtime_root="/srv/manyforge/runtime",
        evidence_root="/srv/manyforge/evidence", isaac_site_settings="/srv/manyforge/site.json"))



def test_held_isaac_setup_defaults_are_jazzy_and_explicit_inputs_win(tmp_path, monkeypatch):
    config = _container_config(tmp_path)
    assert config.remote.ros_setup_script == "/opt/ros/jazzy/setup.bash"
    assert IsaacLauncherConfig.__dataclass_fields__["local_ros_setup_script"].default == Path(
        "/opt/ros/jazzy/setup.bash"
    )
    arguments = _parser().parse_args(
        ["start", "--profile", "isaac", "--source-root", str(config.source_root)]
    )
    arguments.source_root = config.source_root
    arguments.isaac_executable = config.isaac_executable
    arguments.world_path = config.world_path
    for field in ("source_root", "container_image_id", "container_name", "cache_root",
                  "runtime_root", "evidence_root", "manager_executable", "install_root",
                  "node_catalog_path", "alarm_catalog_path", "state_root",
                  "simulation_control_config", "ros_environment_script"):
        setattr(arguments, "remote_" + field, getattr(config.remote, field))
    for field in ("package_aggregate_sha256", "compiler_build_manifest_sha256",
                  "node_environment_sha256"):
        setattr(arguments, field, getattr(config.remote, field))
    captured = []
    monkeypatch.setattr("manyforge_operator.cli.IsaacLauncherConfig",
                        lambda **kwargs: captured.append(kwargs))
    _isaac_config(arguments)
    assert captured[-1]["local_ros_setup_script"] == Path("/opt/ros/jazzy/setup.bash")
    assert captured[-1]["remote"].ros_setup_script == "/opt/ros/jazzy/setup.bash"
    arguments.local_ros_setup_script = config.local_ros_setup_script
    arguments.remote_ros_setup_script = "/custom/qualified/setup.bash"
    _isaac_config(arguments)
    assert captured[-1]["local_ros_setup_script"] == config.local_ros_setup_script
    assert captured[-1]["remote"].ros_setup_script == "/custom/qualified/setup.bash"


def test_remote_native_command_keeps_optional_source_identity_absent(tmp_path: Path):
    launcher = IsaacLauncher(_config(tmp_path))

    command = launcher._remote_native_shell("start")

    assert "--profile native" in command
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
    assert observed["log_reference"] == "journalctl -t manyforge-isaac-isaac_simulator"
    assert not (config.state_root / config.instance_id / "logs").exists()
    assert captured["kwargs"]["stdout"] == -3
    assert captured["args"][0][:5] == ["/usr/bin/systemd-cat", "-t", "manyforge-isaac-isaac_simulator", "--", "bash"]
    argv = captured["args"][0]
    assert "--enable_extension isaacsim.ros2.bridge" in argv[-1]
    assert "--enable_extension isaacsim.ros2.sim_control" in argv[-1]
    assert "--merge-config" not in argv[-1]
    assert "--enable_extension manyforge.isaac.command_guard" not in argv[-1]


def test_explicit_guard_config_uses_native_merge_without_parsing_or_scene_guessing(tmp_path, monkeypatch):
    config = _config(tmp_path)
    startup = tmp_path / "scene guard.toml"
    startup.write_text('[exts."manyforge.isaac.command_guard"]\nenabled = false\n')
    ext_config = config.source_root / "exts/manyforge.isaac.command_guard/config/extension.toml"
    ext_config.parent.mkdir(parents=True)
    ext_config.write_text('[settings]\n')
    configured = replace(config, guard_startup_config=startup)
    captured = []
    monkeypatch.setattr("manyforge_operator.isaac.subprocess.Popen",
                        lambda argv, **kwargs: (captured.append(argv) or type("Process", (), {"pid": 321})()))
    monkeypatch.setattr("manyforge_operator.isaac._proc_observation", lambda pid: (123, "S"))
    monkeypatch.setattr("manyforge_operator.isaac._command_line", lambda pid: ("kit",))
    monkeypatch.setattr("manyforge_operator.isaac._boot_id", lambda: "boot")
    monkeypatch.setattr("manyforge_operator.isaac.os.getpgid", lambda pid: 321)
    IsaacLauncher(configured)._spawn_local_simulator()
    words = shlex.split(captured[0][-1])
    assert f"--merge-config={startup}" in words
    assert words[words.index("--ext-folder") + 1] == str(config.source_root / "exts")
    assert words[words.index("manyforge.isaac.command_guard") - 1] == "--enable_extension"
    assert json.loads(json.dumps(configured.record()))["guard_startup_config"] == str(startup)
    assert configured.input_digest() != config.input_digest()
    before = configured.input_digest()
    startup.write_text('enabled = false\n# changed explicit inputs\n')
    assert configured.input_digest() != before
    arguments = ["start", "--profile", "isaac", "--source-root", str(config.source_root),
                 "--joint-commands-topic", "/cell_a/arm/joint_commands",
                 "--guard-startup-config", str(startup), "--isaac-executable", str(config.isaac_executable),
                 "--world-path", str(config.world_path), "--local-ros-setup-script", str(config.local_ros_setup_script),
                 "--local-ros-environment-script", str(config.local_ros_environment_script)]
    remote_fields = ("source_root", "manager_executable", "install_root", "node_catalog_path",
                     "alarm_catalog_path", "state_root", "simulation_control_config", "ros_environment_script")
    for field in remote_fields:
        arguments.extend(("--remote-" + field.replace("_", "-"), str(getattr(config.remote, field))))
    for field in ("package_aggregate_sha256", "compiler_build_manifest_sha256", "node_environment_sha256"):
        arguments.extend(("--" + field.replace("_", "-"), getattr(config.remote, field)))
    arguments.extend(("--remote-container-image-id", "sha256:" + "e" * 64,
                      "--remote-container-name", "manyforge-g-fixture", "--remote-cache-root", "/srv/cache",
                      "--remote-runtime-root", "/srv/runtime", "--remote-evidence-root", "/srv/evidence"))
    # The no-process fixture supplies SSH instead of requiring it in the image.
    monkeypatch.setattr("manyforge_operator.cli.IsaacLauncherConfig",
                        lambda **kwargs: IsaacLauncherConfig(ssh_executable=config.ssh_executable, **kwargs))
    cli_config = _isaac_config(_parser().parse_args(arguments))
    assert cli_config.guard_startup_config == startup
    assert cli_config.joint_commands_topic == "/cell_a/arm/joint_commands"


def test_guard_config_requires_exact_existing_inputs_and_has_no_fallback(tmp_path):
    config = _config(tmp_path)
    for path in (Path("relative.toml"), tmp_path / "missing.toml"):
        with pytest.raises(ValueError, match="existing absolute scene config"):
            replace(config, guard_startup_config=path)
    startup = tmp_path / "guard.toml"
    startup.write_text('enabled = false\n')
    with pytest.raises(ValueError, match="selected guard extension"):
        replace(config, guard_startup_config=startup)


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
    assert main(["--state-root", str(tmp_path / "state"), "stop"]) == 66


def _simulator_meta():
    return {"resource_id": "isaac_simulator", "component_id": "isaac_simulator",
            "kind": "process", "host_id": "simulator", "pid": 321, "start_ticks": 123,
            "boot_id": "33333333-3333-3333-3333-333333333333", "process_group_id": 321, "argv": ["kit"]}


def test_start_simulator_then_manager_without_bridge_or_barrier(tmp_path, monkeypatch, configuration_factory):
    launcher = IsaacLauncher(_container_config(tmp_path))
    monkeypatch.setattr(launcher, "_verify_remote_source", lambda expected: None)
    configuration = configuration_factory(profile=LaunchProfile.ISAAC)
    order = []
    monkeypatch.setattr(launcher, "_remote_boot_id", lambda: "boot")
    monkeypatch.setattr(launcher, "_configuration", lambda boot: configuration)
    monkeypatch.setattr(launcher, "_local_process_authenticated", lambda meta: True)
    monkeypatch.setattr(launcher, "_spawn_local_simulator", lambda: (order.append("simulator") or _simulator_meta()))
    native = {"state": "ready", "configuration": configuration.model_dump(mode="json"),
              "owned_resources": [{"resource_id": "manager", "component_id": "manager", "kind": "process",
                                   "host_id": "runtime", "pid": 442, "start_ticks": 123,
                                   "boot_id": "22222222-2222-2222-2222-222222222222", "process_group_id": 442}],
              "resource_observations": [{"resource_id": "manager", "presence": "present", "observed_at_unix_ns": 1}],
              "readiness_observations": [{"component_id": "manager", "authority": "owner_issued", "state": "ready",
                                          "source_schema": "manyforge.manager.health.v1", "owner_id": "manager",
                                          "payload_sha256": "a" * 64, "observed_at_unix_ns": 1}]}
    monkeypatch.setattr(launcher, "_remote_native_start", lambda: (order.append("manager") or native))
    observed = launcher.start()
    assert order == ["simulator", "manager"]
    assert observed.state is LaunchInstanceState.READY
    assert [row.component_id for row in observed.readiness_observations] == ["isaac_simulator", "manager"]
    assert {row.resource_id: row.presence.value for row in observed.resource_observations} == {
        "isaac_simulator": "present", "manager": "present"}
    assert set(launcher._mechanical()["processes"]) == {"isaac_simulator"}
    assert "barrier" not in launcher._mechanical()


@pytest.mark.parametrize("sim_present,manager_state,expected", [
    (True, "ready", "ready"), (False, "ready", "degraded"), (True, "degraded", "degraded"),
])
def test_readiness_requires_simulator_and_manager(tmp_path, monkeypatch, configuration_factory,
                                                 sim_present, manager_state, expected):
    launcher = IsaacLauncher(_config(tmp_path))
    launcher._processes = {"isaac_simulator": _simulator_meta()}
    monkeypatch.setattr(launcher, "_local_process_authenticated", lambda meta: sim_present)
    native = {"state": manager_state, "owned_resources": [],
              "resource_observations": [], "readiness_observations": []}
    observed = launcher._snapshot(configuration_factory(profile=LaunchProfile.ISAAC), None,
                                  launcher._mechanical(), remote_status=native)
    assert observed.state.value == expected


@pytest.mark.parametrize("owned", [True, False])
def test_cleanup_stops_only_owned_simulator_once_without_bridge(tmp_path, monkeypatch, owned):
    launcher = IsaacLauncher(_config(tmp_path))
    order = []
    monkeypatch.setattr(launcher, "_local_process_authenticated", lambda meta: True)
    monkeypatch.setattr(launcher, "_remote_stop_simulation", lambda: (order.append("stop_simulation") or True))
    native = {"state": "ready", "owned_resources": []}
    monkeypatch.setattr(launcher, "_remote_native_stop", lambda: (order.append("stop_manager") or {"state": "stopped"}))
    def terminate(meta):
        if meta:
            order.append("terminate_simulator")
            monkeypatch.setattr(launcher, "_local_process_authenticated", lambda meta: False)
    monkeypatch.setattr(launcher, "_terminate_local", terminate)
    mechanical = {"processes": {"isaac_simulator": _simulator_meta()} if owned else {}, "native_status": native}
    assert launcher._cleanup(mechanical) is True
    assert order == (["stop_simulation", "stop_manager", "terminate_simulator"] if owned else ["stop_manager"])


def test_simulator_records_child_after_journal_wrapper_exec(tmp_path, monkeypatch):
    launcher = IsaacLauncher(_config(tmp_path))
    observations = iter([("/usr/bin/systemd-cat", "-t", "tag"), ("kit", "--no-window")])
    monkeypatch.setattr("manyforge_operator.isaac.subprocess.Popen", lambda *args, **kwargs: type("Process", (), {"pid": 321})())
    monkeypatch.setattr("manyforge_operator.isaac._command_line", lambda pid: next(observations))
    monkeypatch.setattr("manyforge_operator.isaac._proc_observation", lambda pid: (123, "S"))
    monkeypatch.setattr("manyforge_operator.isaac._boot_id", lambda: "boot")
    monkeypatch.setattr("manyforge_operator.isaac.os.getpgid", lambda pid: pid)
    monkeypatch.setattr("manyforge_operator.isaac.time.sleep", lambda seconds: None)
    assert launcher._spawn_local_simulator()["argv"] == ("kit", "--no-window")


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
    assert command[-3:] == [launcher.config.remote.source_root + "/scripts/compose-dev.sh", "start", "--ros2-control"]
    assert "--project" not in command and "--bootstrap-project" not in command
    environment = dict(word.split("=", 1) for word in command[1:-4])
    assert environment["MANYFORGE_DEV_IMAGE"] == launcher.config.remote.container_image_id
    assert environment["MANYFORGE_DEV_LAUNCH_INPUT_DIGEST"] == launcher.config.input_digest()
    assert environment["MANYFORGE_DEV_RUNTIME_STATE_ROOT"] == launcher.config.remote.runtime_root
    assert environment["MANYFORGE_DEV_EVIDENCE_ROOT"] == launcher.config.remote.evidence_root
    assert environment["MANYFORGE_DEV_ISAAC_SITE_SETTINGS"] == launcher.config.remote.isaac_site_settings
    assert environment["ROS_DOMAIN_ID"] == str(launcher.config.ros_domain)
    assert environment["ROS_STATIC_PEERS"] == ";".join(launcher.config.static_peers)
    assert "source " not in launcher._container_shell("start")
    assert launcher.config.remote.ros_setup_script not in command


def test_container_ready_reads_owner_facts_and_keeps_exact_birth_identity(tmp_path):
    launcher = IsaacLauncher(_container_config(tmp_path))
    readback = _container_readback(launcher)
    status = launcher._container_status(readback)
    assert status["state"] == "ready"  # Inactive CCLC peers do not change G readiness.
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


def test_failed_container_start_recovers_identity_then_sim_stop_precedes_export(tmp_path, monkeypatch):
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
    with pytest.raises(LauncherError, match="partial startup"):
        launcher._remote_native_start()
    assert launcher._remote_status["owned_resources"][0]["container_id"] == "d" * 64
    alive = True
    monkeypatch.setattr(launcher, "_local_process_authenticated", lambda meta: alive)
    monkeypatch.setattr(launcher, "_remote_stop_simulation", lambda: (calls.append("sim_stop") or True))
    def stop():
        calls.append("export_stop")
        raise LauncherError("runtime_command_failed", "export failed; retained", exit_code=73)
    def terminate(meta):
        nonlocal alive
        calls.append("local_cleanup")
        alive = False
    monkeypatch.setattr(launcher, "_remote_native_stop", stop)
    monkeypatch.setattr(launcher, "_terminate_local", terminate)
    assert launcher._cleanup({"processes": {"isaac_simulator": _simulator_meta()}}) is False
    assert calls == ["start", "status", "status", "sim_stop", "export_stop", "local_cleanup"]


def test_remote_source_check_preserves_existing_g_clean_exact_source_rule(tmp_path, monkeypatch, source_factory):
    launcher = IsaacLauncher(_container_config(tmp_path))
    actual = source_factory()
    commands = []
    monkeypatch.setattr(launcher, "_ssh_json", lambda command, **kwargs: (commands.append(command) or actual.model_dump(mode="json")))
    launcher._verify_remote_source(actual)
    assert "scripts/qualification/source_identity.py --repo-root" in commands[0]
    assert "source " not in commands[0] and "docker" not in commands[0]
    actual = source_factory(dirty=True)
    with pytest.raises(LauncherError, match="dirty"):
        launcher._verify_remote_source(source_factory())


def test_g_never_sends_sim_stop_through_exited_container(tmp_path, monkeypatch):
    launcher = IsaacLauncher(_container_config(tmp_path))
    readback = _container_readback(launcher)
    readback["running"] = False
    launcher._remote_status = launcher._container_status(readback)
    monkeypatch.setattr(launcher, "_remote_native_status", lambda: launcher._remote_status)
    monkeypatch.setattr(launcher, "_ssh_json", lambda *args, **kwargs: pytest.fail("dead container cannot be a ROS client"))
    assert launcher._remote_stop_simulation() is None
