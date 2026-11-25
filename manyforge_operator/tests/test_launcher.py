from __future__ import annotations

import errno
import json
import os
from dataclasses import replace
from pathlib import Path
import signal
import socket
import threading
import subprocess
import sys

from fastapi.testclient import TestClient
import pytest

from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.security import SecurityConfig
from manyforge_composer.backend.state import ComposerState
from manyforge_operator import LaunchInstanceState, LaunchInstanceStatus
from manyforge_operator.cell_layout import CellInstanceLayout
from manyforge_operator.orchestrator import (
    CURRENT_DIRECTORIES,
    LauncherError,
    NativeLauncher,
    NativeLauncherConfig,
    _StateStore,
    _proc_observation,
    native_commands,
)
from manyforge_operator.package_aggregate import package_aggregate_sha256
from manyforge_operator.application_processes import MANAGER_STOP_INCOMPLETE
from manyforge_operator import run_session


def _config(tmp_path: Path) -> NativeLauncherConfig:
    source = tmp_path / "source"
    source.mkdir()
    examples = source / "examples"
    examples.mkdir()
    (examples / "fixture.program.yaml").write_text(
        "schema_version: manyforge.program.v2\n",
        encoding="utf-8",
    )
    (examples / "fixture.deployment.yaml").write_text(
        "schema_version: manyforge.deployment.v1\n",
        encoding="utf-8",
    )
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


@pytest.fixture
def journald(monkeypatch: pytest.MonkeyPatch) -> None:
    """Each child keeps its own journald tag (spec 3.3 point 4)."""

    monkeypatch.setattr(
        "manyforge_operator.orchestrator._journal_command",
        lambda instance, component, argv: (
            ["/usr/bin/systemd-cat", "-t", f"manyforge-{instance}-{component}", "--", *argv],
            f"journalctl -t manyforge-{instance}-{component}",
        ),
    )


def _unwrapped(process) -> tuple[str, ...]:
    assert process.argv[:2] == ("/usr/bin/systemd-cat", "-t")
    assert process.argv[2] == f"manyforge-workbench-{process.role}"
    assert process.argv[3] == "--"
    return process.argv[4:]


def test_native_commands_start_only_the_manager_with_the_ordered_process_list(
    tmp_path: Path, journald
):
    config = _config(tmp_path)
    manager, processes = native_commands(config)
    assert manager.component_id == "manager"
    # Headless, then the Program runner, then Composer after the first
    # converged baseline; the Manager stops them in reverse (spec 3.3, 4.7).
    assert [process.role for process in processes] == [
        "headless",
        "program_runner",
        "composer",
    ]
    assert [process.start for process in processes] == [
        "immediate",
        "immediate",
        "after_baseline",
    ]
    headless, runner, composer = (_unwrapped(process) for process in processes)
    command = " ".join(manager.argv)
    assert "--canonical-empty-admission-filename canonical-empty-admission.json" in command
    assert manager.argv[manager.argv.index("--reconciliation-period-ms") + 1] == "500"
    assert manager.argv[manager.argv.index("--application-processes") + 1] == str(
        config.current_root / "application-processes.json"
    )
    assert "--cell-id cell" in " ".join(headless)
    assert "--manager-timeout-s 60.0" in " ".join(headless)
    assert headless[headless.index("--node-environment-manifest-path") + 1] == str(
        config.source_root
        / "manyforge_behavior/resources/node_environment_manifest.yaml"
    )
    assert headless[headless.index("--execution-run-root") + 1] == str(
        config.execution_run_root
    )
    assert manager.argv[manager.argv.index("--execution-state-root") + 1] == str(
        config.execution_state_root
    )
    assert manager.argv[manager.argv.index("--execution-run-root") + 1] == str(
        config.execution_run_root
    )
    assert "--port 9010" in " ".join(headless)
    # The runner serves its command and status sockets in the Manager run
    # root; Headless reaches them there.
    assert "--port" not in runner
    assert runner[runner.index("--program-socket-root") + 1] == str(
        config.manager_run_root
    )
    assert headless[headless.index("--program-runner-socket-root") + 1] == str(
        config.manager_run_root
    )
    assert runner[runner.index("--snapshot-url") + 1] == (
        "http://127.0.0.1:9000/api/scene/snapshot/render"
    )
    expected_liveness = str(config.manager_run_root / "program-runner-liveness.sock")
    assert manager.argv[manager.argv.index("--program-runner-liveness-socket") + 1] == (
        expected_liveness
    )
    assert runner[runner.index("--manager-liveness-socket") + 1] == expected_liveness
    assert runner[runner.index("--cell-id") + 1] == config.cell_id
    # Running state is per start; saved data and History are kept.
    assert runner[runner.index("--cycle-journal-path") + 1] == str(
        config.instance_root / "current/program-cycle.sqlite3"
    )
    assert runner[runner.index("--persistent-key-register-directory") + 1] == str(
        config.instance_root / "data/program-runner"
    )
    assert headless[headless.index("--historian-directory") + 1] == str(
        config.instance_root / "history"
    )
    # Process identities are roles plus the application-start identity, which
    # the Manager appends; no launcher-composed owner ids (spec 12).
    assert "--owner-id" not in headless and "--owner-id" not in runner
    assert "--application-start-id" not in headless + runner + composer
    assert "--port 9000" in " ".join(composer)
    assert "--assistant-bridge-url" not in composer
    every_argv = (manager.argv, headless, runner, composer)
    assert not any("manyforge_assistant_bridge" in part for argv in every_argv for part in argv)
    assert all("--apply" not in argv and "--run" not in argv for argv in every_argv)
    # Children inherit the Manager's environment; Headless and Composer read
    # below the same allowed folders (OD-335).
    assert manager.environment["MANYFORGE_FS_ROOTS"] == str(config.install_root)
    assert processes[0].environment == {
        "MANYFORGE_FS_ROOTS": processes[2].environment["MANYFORGE_FS_ROOTS"]
    }
    assert processes[1].environment == {}
    # The Manager's execution host and its component processes (a point-cloud
    # source, the assistant's host and bridge) inherit both URLs.
    assert manager.environment["MANYFORGE_COMPOSER_URL"] == "http://127.0.0.1:9000"
    assert manager.environment["MANYFORGE_HEADLESS_API_URL"] == "http://127.0.0.1:9010"
    assert processes[2].environment == {
        "MANYFORGE_FS_ROOTS": (
            f"{config.source_root / 'examples'},"
            f"{config.source_root / 'manyforge_example_models'},"
            f"{config.install_root},{config.program_workspace}"
        ),
        # Composer settings are saved under data/composer/ (finding F1).
        "MANYFORGE_STATE_DIR": str(config.instance_root / "data"),
    }


def test_native_commands_pass_the_previous_run_report(tmp_path: Path, journald):
    config = _config(tmp_path)
    _manager, processes = native_commands(config, previous_session_id="a" * 32)
    headless = _unwrapped(processes[0])
    assert headless[headless.index("--workbench-previous-session-reports-root") + 1] == str(
        config.instance_root / "session-reports"
    )
    assert headless[headless.index("--workbench-previous-session-id") + 1] == "a" * 32
    assert "MANYFORGE_WORKBENCH_PREVIOUS_SESSION_REPORT" not in processes[2].environment


def test_native_commands_pass_the_disk_floor_margin_and_record_folders(tmp_path: Path, journald):
    config = _config(tmp_path)
    _manager, processes = native_commands(config)
    headless = _unwrapped(processes[0])
    # 10 GB always free plus the machine minimum (0, not determined yet).
    assert headless[headless.index("--disk-free-floor-bytes") + 1] == "10000000000"
    assert headless[headless.index("--disk-free-margin-bytes") + 1] == "1000000000"
    assert headless[headless.index("--historian-directory") + 1] == str(config.instance_root / "history")
    assert headless[headless.index("--execution-run-root") + 1] == str(
        config.instance_root / "current/execution-run")
    raised = replace(config, disk_limits=run_session.DiskLimits(10, 10_000_000_000, 2_000_000_000))
    headless = _unwrapped(native_commands(raised)[1][0])
    assert headless[headless.index("--disk-free-floor-bytes") + 1] == "12000000000"


def test_native_ordered_roots_make_examples_the_default_browse_root(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, journald
):
    config = _config(tmp_path)
    composer_environment = native_commands(config)[1][-1].environment
    for name, value in composer_environment.items():
        monkeypatch.setenv(name, value)

    client = TestClient(
        create_app(ComposerState(application_start_id=0x3F9A0000000000A1), security=SecurityConfig.from_env())
    )
    expected_root = str((config.source_root / "examples").resolve())

    for kind, expected_name in (
        ("program", "fixture.program.yaml"),
        ("deployment", "fixture.deployment.yaml"),
    ):
        response = client.get("/api/files/browse", params={"kind": kind})

        assert response.status_code == 200
        assert response.json()["currentPath"] == expected_root
        assert expected_name in {
            entry["name"] for entry in response.json()["entries"]
        }


@pytest.mark.parametrize(
    "field,value,expected_error",
    [
        ("install_root", "package,extra", "MANYFORGE_FS_ROOTS"),
        ("install_root", "package ", "MANYFORGE_FS_ROOTS"),
        ("state_root", "operator,state", "MANYFORGE_FS_ROOTS"),
        ("instance_id", "workbench,extra", "path-safe"),
    ],
)
def test_native_configuration_rejects_ambiguous_composer_roots_before_effects(
    tmp_path: Path, field: str, value: str, expected_error: str
):
    config = _config(tmp_path)
    replacement = value if field == "instance_id" else tmp_path / value

    with pytest.raises(ValueError, match=expected_error):
        replace(config, **{field: replacement})


@pytest.mark.parametrize("source_name", ["source,ambiguous", "source "])
def test_native_configuration_rejects_ambiguous_examples_root_before_effects(
    tmp_path: Path, source_name: str
):
    source = tmp_path / source_name
    source.mkdir()
    config = _config(tmp_path)

    with pytest.raises(ValueError, match="MANYFORGE_FS_ROOTS"):
        replace(config, source_root=source)

    assert not config.state_root.exists()


@pytest.mark.parametrize("examples_kind", ["missing", "not-a-directory"])
def test_native_configuration_requires_usable_examples_root_before_effects(
    tmp_path: Path, examples_kind: str
):
    config = _config(tmp_path)
    examples = config.source_root / "examples"
    for child in examples.iterdir():
        child.unlink()
    examples.rmdir()
    if examples_kind == "not-a-directory":
        examples.write_text("not a directory\n", encoding="utf-8")

    with pytest.raises(ValueError, match="examples_root"):
        replace(config, source_root=config.source_root)

    assert not config.state_root.exists()


@pytest.mark.parametrize("instance_id", [".", ".."])
def test_native_configuration_rejects_dot_instance_ids(
    tmp_path: Path, instance_id: str
):
    config = _config(tmp_path)

    with pytest.raises(ValueError, match="instance_id"):
        replace(config, instance_id=instance_id)

    assert not config.state_root.exists()


@pytest.mark.parametrize("instance_id", [".", ".."])
def test_from_state_root_rejects_dot_instance_ids_without_mutation(
    tmp_path: Path, instance_id: str
):
    parent = tmp_path / "state-parent"
    parent.mkdir()
    marker = parent / "marker.txt"
    marker.write_text("unchanged\n", encoding="utf-8")
    state_root = parent / "operator-state"
    entries_before = sorted(path.name for path in parent.iterdir())

    with pytest.raises(LauncherError) as raised:
        NativeLauncher.from_state_root(state_root, instance_id)

    assert raised.value.code == "invalid_instance_id"
    assert raised.value.exit_code == 64
    assert not state_root.exists()
    assert sorted(path.name for path in parent.iterdir()) == entries_before
    assert marker.read_text(encoding="utf-8") == "unchanged\n"


@pytest.mark.parametrize(
    "relationship",
    ["workspace-inside-package", "package-inside-workspace", "equal"],
)
def test_native_configuration_rejects_overlapping_composer_roots_without_mutation(
    tmp_path: Path, relationship: str
):
    config = _config(tmp_path)
    state_root = tmp_path / "prospective-state"
    instance_id = "workbench"
    prospective_workspace = state_root / instance_id / "data" / "program-workspace"
    package_root = config.install_root

    if relationship == "workspace-inside-package":
        state_root = package_root / "operator-state"
        prospective_workspace = state_root / instance_id / "data" / "program-workspace"
    else:
        prospective_workspace.mkdir(parents=True)
        if relationship == "package-inside-workspace":
            package_root = prospective_workspace / "immutable-package"
            package_root.mkdir()
        else:
            package_root = prospective_workspace
        (package_root / "immutable.txt").write_text(
            "immutable\n", encoding="utf-8"
        )

    package_before = package_aggregate_sha256(package_root)
    workspace_existed = prospective_workspace.exists()
    workspace_entries = (
        sorted(
            path.relative_to(prospective_workspace)
            for path in prospective_workspace.rglob("*")
        )
        if workspace_existed
        else []
    )

    with pytest.raises(ValueError, match="must be disjoint"):
        replace(
            config,
            install_root=package_root,
            state_root=state_root,
            instance_id=instance_id,
            package_aggregate_sha256=package_before,
        )

    assert prospective_workspace.exists() is workspace_existed
    if workspace_existed:
        assert sorted(
            path.relative_to(prospective_workspace)
            for path in prospective_workspace.rglob("*")
        ) == workspace_entries
    assert package_aggregate_sha256(package_root) == package_before


class _Manager:
    """A Manager that runs for ``polls`` waits, then exits with ``status``."""

    def __init__(self, status: int, polls: int = 1) -> None:
        self.status = status
        self.polls = polls
        self.signals: list[int] = []
        self.on_exit = lambda: None

    def wait(self, timeout=None):
        if self.polls > 0:
            self.polls -= 1
            raise subprocess.TimeoutExpired("manager", timeout)
        self.on_exit()
        return self.status

    def send_signal(self, number: int) -> None:
        self.signals.append(number)


def _resident_launcher(tmp_path, configuration, monkeypatch, managers):
    from manyforge_operator.tests.conftest import make_readiness

    config = _config(tmp_path)
    launcher = NativeLauncher(config)
    monkeypatch.setattr(
        "manyforge_operator.orchestrator._configuration",
        lambda _config: configuration,
    )
    monkeypatch.setattr(launcher, "_port_available", lambda: None)
    alive: set[int] = set()
    monkeypatch.setattr(
        "manyforge_operator.orchestrator._process_is_authenticated",
        lambda record: int(record["pid"]) in alive,
    )
    monkeypatch.setattr(
        launcher,
        "_readiness",
        lambda _configuration=None: (True, [make_readiness()], None),
    )
    spawned = []

    def spawn(spec):
        assert config.program_workspace.is_dir()
        spawned.append(spec)
        pid = 1000 + len(spawned)
        alive.add(pid)
        manager = managers.pop(0)
        manager.on_exit = lambda: alive.discard(pid)
        return type(
            "Managed",
            (),
            {
                "component_id": spec.component_id,
                "process": manager,
                "pid": 1000 + len(spawned),
                "start_ticks": 1,
                "boot_id": "22222222-2222-2222-2222-222222222222",
                "process_group_id": 1000 + len(spawned),
                "argv": spec.argv,
                "log_path": "journalctl -t manyforge-workbench-manager",
            },
        )()

    monkeypatch.setattr(launcher, "_spawn", spawn)
    return config, launcher, spawned


def test_start_waits_on_the_manager_and_keeps_saved_data_across_clean_starts(
    tmp_path: Path, configuration_factory, monkeypatch, journald
):
    configuration = configuration_factory()
    config, launcher, spawned = _resident_launcher(
        tmp_path, configuration, monkeypatch, [_Manager(0), _Manager(0)]
    )
    package_before = package_aggregate_sha256(config.install_root)
    ready: list[LaunchInstanceStatus] = []

    first = launcher.start(on_ready=ready.append)

    # Only the Manager is started; READY is reported once, and the start
    # returns when the Manager has ended the application.
    assert [spec.component_id for spec in spawned] == ["manager"]
    assert [status.state for status in ready] == [LaunchInstanceState.READY]
    assert first.state is LaunchInstanceState.STOPPED
    assert first.last_failure is None
    document = json.loads(config.application_processes_document.read_text(encoding="utf-8"))
    assert document["api_version"] == "manyforge.application_processes.v1"
    assert [row["role"] for row in document["processes"]] == [
        "headless", "program_runner", "composer"]
    assert document["processes"][2]["start"] == "after_baseline"
    for name in CURRENT_DIRECTORIES:
        assert (config.current_root / name).is_dir()
    # OD-216: the Manager's state reaches the Headless API over its status
    # socket in the Manager run root, which both receive; no status folder.
    assert not (config.current_root / "publications").exists()
    assert (config.instance_root / "data/program-runner").is_dir()
    assert (config.instance_root / "data/composer").is_dir()
    record = launcher.store.read()
    assert record is not None
    assert record[1]["paths"]["program_workspace"] == str(config.program_workspace)
    assert list(record[1]["processes"]) == ["manager"]

    saved = config.program_workspace / "saved.program.yaml"
    saved.write_text("schema_version: manyforge.program.v2\n", encoding="utf-8")
    keys = config.persistent_key_register_directory / "persistent-keys.v4.json"
    keys.write_text("{}\n", encoding="utf-8")
    first_run = json.loads((config.current_root / "session.json").read_text(encoding="utf-8"))
    assert first_run["disposition"] == "stopped" and first_run["shutdown"] == "clean"
    (config.current_root / "program-cycle.sqlite3").write_bytes(b"previous run")

    second = launcher.start()

    # Every start begins from a fresh, empty current/ (B1, spec 4.2); the
    # previous run is archived and reported, saved data is untouched.
    assert second.state is LaunchInstanceState.STOPPED
    assert not (config.current_root / "program-cycle.sqlite3").exists()
    archived = config.instance_root / "previous-runs" / first_run["session_id"]
    assert (archived / "program-cycle.sqlite3").read_bytes() == b"previous run"
    previous = json.loads(config.application_processes_document.read_text(encoding="utf-8"))
    headless = previous["processes"][0]["argv"]
    assert headless[headless.index("--workbench-previous-session-id") + 1] == (
        first_run["session_id"]
    )
    assert saved.is_file() and keys.read_text(encoding="utf-8") == "{}\n"
    assert package_aggregate_sha256(config.install_root) == package_before


def test_an_application_process_exit_ends_the_start_with_a_typed_failure(
    tmp_path: Path, configuration_factory, monkeypatch, journald
):
    _config_value, launcher, _spawned = _resident_launcher(
        tmp_path, configuration_factory(), monkeypatch, [_Manager(72)]
    )

    with pytest.raises(LauncherError) as raised:
        launcher.start()

    assert raised.value.code == "application_exited"
    assert raised.value.exit_code == 73
    assert raised.value.status is not None
    assert raised.value.status.state is LaunchInstanceState.STOPPED
    assert raised.value.status.last_failure.reason_code == "application_exited"
    assert "status 72" in raised.value.status.last_failure.detail


def test_the_run_end_records_the_managers_application_end_and_signal(
    tmp_path: Path, configuration_factory, monkeypatch, journald
):
    config, launcher, _spawned = _resident_launcher(
        tmp_path, configuration_factory(), monkeypatch, [_Manager(-6), _Manager(0)]
    )
    end = {
        "schema_version": "manyforge.application_end.v1",
        "ended_by": "process_exit",
        "process": {"role": "headless", "pid": 77, "exit_status": None, "signal": 11,
                    "core_dumped": False},
        "ended_at_wall": "2026-11-24T10:15:30.123Z",
    }
    original_wait = launcher._wait_on_manager

    def wait_and_record(*args, **kwargs):
        # The Manager writes its record before it exits.
        (config.manager_run_root / "application-end.json").write_text(json.dumps(end))
        return original_wait(*args, **kwargs)

    monkeypatch.setattr(launcher, "_wait_on_manager", wait_and_record)
    with pytest.raises(LauncherError):
        launcher.start()
    first_run = json.loads((config.current_root / "session.json").read_text(encoding="utf-8"))
    assert first_run["schema_version"] == "manyforge.workbench_session.v3"
    assert first_run["application_end"] == end
    assert first_run["manager_exit"] == {"exit_status": None, "signal": 6}
    monkeypatch.setattr(launcher, "_wait_on_manager", original_wait)
    launcher.start()
    report = json.loads((config.current_root / "session.json").read_text(encoding="utf-8"))["previous_session"]
    assert report["schema_version"] == "manyforge.workbench_previous_session.v4"
    assert report["application_end"] == end
    assert report["started_at_wall"] == first_run["started_at_wall"]


def test_a_session_of_an_older_build_is_reported_as_archived_at_the_upgrade(
    tmp_path: Path, configuration_factory, monkeypatch, journald
):
    config, launcher, _spawned = _resident_launcher(
        tmp_path, configuration_factory(), monkeypatch, [_Manager(0), _Manager(0)]
    )
    launcher.start()
    path = config.current_root / "session.json"
    first_run = json.loads(path.read_text(encoding="utf-8"))
    del first_run["application_end"], first_run["manager_exit"]
    path.write_text(json.dumps(dict(first_run, schema_version="manyforge.workbench_session.v2")))
    launcher.start()
    report = json.loads(path.read_text(encoding="utf-8"))["previous_session"]
    assert report["archive_relative_path"] == "previous-runs/" + report["session_id"]
    assert report["archived_older_marker"] == "manyforge.workbench_session.v2"
    assert report["shutdown"] == "unknown"
    assert (" It ran an older build (marker manyforge.workbench_session.v2) and was archived to "
            f"previous-runs/{report['session_id']} at the upgrade.") in report["message"]


def test_native_python_processes_start_with_faulthandler_and_motion_traces_below_the_run(
    tmp_path: Path, journald, monkeypatch
):
    monkeypatch.delenv("MANYFORGE_MOTION_TRACE_DIR", raising=False)
    config = _config(tmp_path)
    manager, processes = native_commands(config)
    for process in processes:
        argv = _unwrapped(process)
        assert argv[1:5] == ("-X", "faulthandler", "-m", f"manyforge_{'headless_api' if process.role == 'headless' else process.role}")
    # The execution host writes the traces and Headless reads them: both
    # inherit the Manager's environment.
    assert manager.environment["MANYFORGE_MOTION_TRACE_DIR"] == str(
        config.current_root / "motion-traces")
    monkeypatch.setenv("MANYFORGE_MOTION_TRACE_DIR", "")
    assert native_commands(config)[0].environment["MANYFORGE_MOTION_TRACE_DIR"] == ""


def test_the_launcher_starts_the_manager_with_core_dumps_off(tmp_path: Path):
    from manyforge_operator.application_processes import parent_death_preexec

    completed = subprocess.run(
        ["/bin/sh", "-c", "ulimit -c"], capture_output=True, text=True, check=True,
        preexec_fn=parent_death_preexec())
    assert completed.stdout == "0\n"


def test_the_launcher_starts_the_manager_with_the_site_core_dump_limit(tmp_path: Path):
    import resource
    from manyforge_operator.application_processes import parent_death_preexec

    limit = 1_048_576
    _soft, hard = resource.getrlimit(resource.RLIMIT_CORE)
    completed = subprocess.run(
        [sys.executable, "-c", "import resource; print(*resource.getrlimit(resource.RLIMIT_CORE))"],
        capture_output=True, text=True, check=True, preexec_fn=parent_death_preexec(limit))
    # Only the soft value changes, and never above the hard limit.
    expected = limit if hard == resource.RLIM_INFINITY else min(limit, hard)
    assert completed.stdout == f"{expected} {hard}\n"


def test_a_signal_to_the_resident_launcher_asks_the_manager_to_stop(
    tmp_path: Path, configuration_factory, monkeypatch, journald
):
    manager = _Manager(0, polls=2)
    _config_value, launcher, _spawned = _resident_launcher(
        tmp_path, configuration_factory(), monkeypatch, [manager]
    )
    original = manager.wait

    def wait(timeout=None):
        if manager.polls == 1:
            os.kill(os.getpid(), signal.SIGTERM)
        return original(timeout)

    manager.wait = wait

    assert launcher.start().state is LaunchInstanceState.STOPPED
    assert manager.signals == [signal.SIGTERM]


def test_a_second_stop_request_is_passed_on_to_force_the_managers_stop(
    tmp_path: Path, configuration_factory, monkeypatch, journald
):
    # OD-187 (1): the Manager's stop has no time limit; every stop request to
    # the launcher (a second Ctrl-C) reaches the Manager as one SIGTERM, and
    # the second forces its stop.
    manager = _Manager(0, polls=3)
    _config_value, launcher, _spawned = _resident_launcher(
        tmp_path, configuration_factory(), monkeypatch, [manager]
    )
    original = manager.wait

    def wait(timeout=None):
        if manager.polls == 2:
            os.kill(os.getpid(), signal.SIGTERM)
        elif manager.polls == 1:
            os.kill(os.getpid(), signal.SIGINT)
        return original(timeout)

    manager.wait = wait

    assert launcher.start().state is LaunchInstanceState.STOPPED
    assert manager.signals == [signal.SIGTERM, signal.SIGTERM]


def test_an_incomplete_stop_is_unknown_and_reported_at_the_next_start(
    tmp_path: Path, configuration_factory, monkeypatch, journald
):
    # OD-187 (2): a stop in which a step did not complete (status 74) is not
    # "stopped normally": the shutdown is recorded unknown, the start ends
    # with its own typed failure (not application_exited), and the next start
    # reports it.
    config, launcher, _spawned = _resident_launcher(
        tmp_path, configuration_factory(), monkeypatch,
        [_Manager(MANAGER_STOP_INCOMPLETE), _Manager(0)],
    )

    with pytest.raises(LauncherError) as raised:
        launcher.start()

    assert raised.value.code == "application_stop_incomplete"
    assert raised.value.exit_code == 73
    assert raised.value.status.last_failure.reason_code == "application_stop_incomplete"
    assert raised.value.status.last_failure.detail == (
        "the Manager's stop of the application did not complete (status 74); "
        "its log names the step")
    first_run = json.loads((config.current_root / "session.json").read_text(encoding="utf-8"))
    assert first_run["disposition"] == "stopped" and first_run["shutdown"] == "unknown"
    assert first_run["manager_exit"] == {"exit_status": MANAGER_STOP_INCOMPLETE, "signal": None}

    assert launcher.start().last_failure is None
    second_run = json.loads((config.current_root / "session.json").read_text(encoding="utf-8"))
    report = second_run["previous_session"]
    assert report["session_id"] == first_run["session_id"]
    assert report["shutdown"] == "unknown"
    assert "; shutdown was unclean or could not be verified." in report["message"]


def test_a_run_end_that_cannot_be_written_has_its_own_code(
    tmp_path: Path, configuration_factory, monkeypatch, journald
):
    # OD-229: the session file's "stopped" write fails at the end of a run
    # (full or failing disk): the launcher ends with run_end_not_recorded and
    # exit status 71, not operator_configuration_invalid; the run stays
    # "started", so the next start treats it as ended uncleanly, as before.
    config, launcher, _spawned = _resident_launcher(
        tmp_path, configuration_factory(), monkeypatch, [_Manager(0), _Manager(0)]
    )
    record_stopped = run_session.record_stopped
    failures = [OSError(28, "No space left on device")]

    def failing_record_stopped(*args, **kwargs):
        if failures:
            raise failures.pop()
        return record_stopped(*args, **kwargs)

    monkeypatch.setattr(run_session, "record_stopped", failing_record_stopped)

    with pytest.raises(LauncherError) as raised:
        launcher.start()

    assert raised.value.code == "run_end_not_recorded"
    assert raised.value.code != "operator_configuration_invalid"
    assert raised.value.exit_code == 71
    assert isinstance(raised.value.__cause__, OSError)
    first_run = json.loads((config.current_root / "session.json").read_text(encoding="utf-8"))
    assert first_run["disposition"] == "started"

    assert launcher.start().last_failure is None
    report = json.loads(
        (config.current_root / "session.json").read_text(encoding="utf-8"))["previous_session"]
    assert report["session_id"] == first_run["session_id"]
    assert report["shutdown"] == "unknown"


def test_start_meeting_a_resident_launcher_reports_it_without_starting(
    tmp_path: Path, configuration_factory, status_factory, monkeypatch
):
    from manyforge_operator.tests.conftest import make_readiness

    configuration = configuration_factory()
    config = _config(tmp_path)
    launcher = NativeLauncher(config)
    monkeypatch.setattr(
        "manyforge_operator.orchestrator._configuration",
        lambda _config: configuration,
    )
    monkeypatch.setattr(
        launcher, "_readiness", lambda _configuration=None: (True, [make_readiness()], None)
    )
    monkeypatch.setattr(
        "manyforge_operator.orchestrator._process_is_authenticated", lambda _record: True
    )
    monkeypatch.setattr(launcher, "_spawn", lambda _spec: pytest.fail("no second Manager"))
    ready = status_factory(configuration, state=LaunchInstanceState.READY)
    resident = {
        "kind": "process", "resource_id": "manager", "component_id": "manager",
        "host_id": "runtime", "pid": 1, "start_ticks": 1,
        "boot_id": "22222222-2222-2222-2222-222222222222", "process_group_id": 1,
        "log_reference": "journalctl", "argv": [],
    }
    with launcher.store.lock():
        launcher.store.write(ready, {"profile": "standard", "processes": {"manager": resident}})
        observed = launcher.start()
        assert launcher.last_start_idempotent is True
        assert observed.state is LaunchInstanceState.READY
        # status observes the resident record without taking the lock.
        assert launcher.status().state is LaunchInstanceState.READY


@pytest.mark.parametrize("conflict", ["file", "symlink"])
def test_start_rejects_an_unsafe_program_workspace_before_child_effects(
    tmp_path: Path, configuration_factory, monkeypatch, conflict: str
):
    config = _config(tmp_path)
    launcher = NativeLauncher(config)
    if conflict == "file":
        config.program_workspace.write_text("not a directory\n", encoding="utf-8")
    else:
        outside = tmp_path / "outside"
        outside.mkdir()
        config.program_workspace.symlink_to(outside, target_is_directory=True)

    monkeypatch.setattr(
        "manyforge_operator.orchestrator._configuration",
        lambda _config: configuration_factory(),
    )

    def unexpected_effect():
        raise AssertionError("port or child effects must not occur")

    monkeypatch.setattr(launcher, "_port_available", unexpected_effect)
    monkeypatch.setattr(launcher, "_spawn", lambda _spec: unexpected_effect())

    with pytest.raises(LauncherError) as raised:
        launcher.start()

    assert raised.value.code == "program_workspace_invalid"


def test_native_commands_bind_explicit_cell_admission_and_static_inputs(tmp_path: Path, journald):
    static_dir = tmp_path / "static"
    static_dir.mkdir()
    (static_dir / "index.html").write_text("<html></html>\n", encoding="utf-8")
    config = replace(
        _config(tmp_path),
        canonical_empty_admission_filename="canonical-empty-admission.lite6.json",
        cell_id="lite6_pick_place_cell",
        composer_static_dir=static_dir,
    )

    manager, processes = native_commands(config)

    assert "canonical-empty-admission.lite6.json" in manager.argv
    assert "lite6_pick_place_cell" in processes[0].argv
    assert processes[-1].argv[-2:] == ("--static-dir", str(static_dir))


@pytest.mark.parametrize(
    "field,value",
    [
        ("canonical_empty_admission_filename", "../admission.json"),
        ("canonical_empty_admission_filename", "/tmp/admission.json"),
        ("cell_id", "cell with spaces"),
    ],
)
def test_native_configuration_rejects_unsafe_cell_admission_inputs(
    tmp_path: Path, field: str, value: str
):
    with pytest.raises(ValueError, match=field if field != "cell_id" else "cell_id"):
        replace(_config(tmp_path), **{field: value})


def test_native_commands_pass_explicit_composer_static_dir(tmp_path: Path, journald):
    static_dir = tmp_path / "static"
    static_dir.mkdir()
    (static_dir / "index.html").write_text("<html></html>\n", encoding="utf-8")
    config = replace(_config(tmp_path), composer_static_dir=static_dir)
    composer = native_commands(config)[1][-1]
    assert composer.argv[-2:] == ("--static-dir", str(static_dir))


def test_native_configuration_requires_explicit_artifact_digests(tmp_path: Path):
    valid = _config(tmp_path)
    assert valid.package_aggregate_sha256 == package_aggregate_sha256(
        valid.install_root
    )
    with pytest.raises(ValueError, match="package_aggregate_sha256"):
        NativeLauncherConfig(
            source_root=valid.source_root,
            manager_executable=valid.manager_executable,
            install_root=valid.install_root,
            node_catalog_path=valid.node_catalog_path,
            alarm_catalog_path=valid.alarm_catalog_path,
            state_root=valid.state_root,
            compiler_build_manifest_sha256=valid.compiler_build_manifest_sha256,
            node_environment_sha256=valid.node_environment_sha256,
        )


def test_atomic_state_store_round_trip(tmp_path: Path, configuration_factory):
    config = _config(tmp_path)
    CellInstanceLayout.open_or_create(
        cell_id=config.cell_id,
        cell_root=config.instance_root,
        install_root=config.install_root,
    )
    store = _StateStore(
        root=config.instance_root / "operator",
        lock_root=config.instance_root / "operator",
    )
    from manyforge_operator.tests.conftest import make_status

    status = make_status(configuration_factory(), state=LaunchInstanceState.STARTING, with_resources=False)
    with store.lock():
        store.write(status, {"profile": "standard", "processes": {}})
        loaded = store.read()
    assert loaded is not None
    assert loaded[0] == status
    assert loaded[1]["profile"] == "standard"
    store.record_path.write_text("{\"partial\":", encoding="utf-8")
    with store.lock(), pytest.raises(Exception):
        store.read()


def test_state_store_never_repairs_a_missing_marked_layout_child(
    tmp_path: Path,
) -> None:
    config = _config(tmp_path)
    CellInstanceLayout.open_or_create(
        cell_id=config.cell_id,
        cell_root=config.instance_root,
        install_root=config.install_root,
    )
    operator_state = config.instance_root / "operator"
    operator_state.rmdir()

    with pytest.raises(LauncherError, match="operator state root is unavailable"):
        _StateStore(
            root=operator_state,
            lock_root=config.instance_root / "operator",
        )

    assert not operator_state.exists()


def test_state_store_rejects_a_replaced_root_without_using_it(
    tmp_path: Path,
) -> None:
    config = _config(tmp_path)
    CellInstanceLayout.open_or_create(
        cell_id=config.cell_id,
        cell_root=config.instance_root,
        install_root=config.install_root,
    )
    operator_state = config.instance_root / "operator"
    store = _StateStore(
        root=operator_state,
        lock_root=config.instance_root / "operator",
    )
    detached = config.instance_root / "operator.detached"
    operator_state.rename(detached)
    operator_state.mkdir(mode=0o700)

    with pytest.raises(LauncherError) as raised:
        store.read()

    assert raised.value.code == "cell_layout_invalid"
    assert not (operator_state / "launch_instance.json").exists()


def test_state_store_rejects_a_symlink_status_record_without_following_it(
    tmp_path: Path,
) -> None:
    config = _config(tmp_path)
    CellInstanceLayout.open_or_create(
        cell_id=config.cell_id,
        cell_root=config.instance_root,
        install_root=config.install_root,
    )
    store = _StateStore(
        root=config.instance_root / "operator",
        lock_root=config.instance_root / "operator",
    )
    outside = tmp_path / "outside.json"
    outside.write_text("unchanged\n", encoding="utf-8")
    store.record_path.symlink_to(outside)

    with pytest.raises(LauncherError) as raised:
        store.read()

    assert raised.value.code == "status_record_invalid"
    assert outside.read_text(encoding="utf-8") == "unchanged\n"


def test_state_store_refuses_an_earlier_record_format_with_guidance(
    tmp_path: Path,
) -> None:
    config = _config(tmp_path)
    CellInstanceLayout.open_or_create(
        cell_id=config.cell_id,
        cell_root=config.instance_root,
        install_root=config.install_root,
    )
    store = _StateStore(
        root=config.instance_root / "operator",
        lock_root=config.instance_root / "operator",
    )
    earlier = '{"schema_version":"manyforge.native_operator_instance.v2"}\n'
    store.record_path.write_text(earlier, encoding="utf-8")
    store.record_path.chmod(0o600)

    with pytest.raises(LauncherError) as raised:
        store.read()

    assert raised.value.code == "status_record_invalid"
    message = str(raised.value)
    assert "manyforge.native_operator_instance.v2" in message
    assert "not read or converted" in message
    assert "new instance" in message
    assert store.record_path.read_text(encoding="utf-8") == earlier


def test_state_store_lock_never_recreates_a_missing_typed_run_root(
    tmp_path: Path,
) -> None:
    config = _config(tmp_path)
    CellInstanceLayout.open_or_create(
        cell_id=config.cell_id,
        cell_root=config.instance_root,
        install_root=config.install_root,
    )
    run_root = config.instance_root / "operator"
    store = _StateStore(
        root=config.instance_root / "operator",
        lock_root=run_root,
    )
    run_root.rmdir()

    with pytest.raises(LauncherError) as raised:
        with store.lock():
            pass

    assert raised.value.code == "cell_layout_invalid"
    assert not run_root.exists()


def test_proc_observation_authenticates_current_process():
    observation = _proc_observation(os.getpid())
    assert observation is not None
    assert observation[0] > 0


def test_launcher_start_outcome_flag_defaults_to_non_idempotent(tmp_path: Path):
    launcher = NativeLauncher(_config(tmp_path))
    assert launcher.last_start_idempotent is False
    assert launcher.last_stop_already_stopped is False


def _server_port_in_time_wait() -> int:
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", 0))
    server.listen()
    server.settimeout(1.0)
    port = server.getsockname()[1]
    client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    client.settimeout(1.0)
    accepted = None
    try:
        client.connect(("127.0.0.1", port))
        accepted, _ = server.accept()
        server.close()
        server = None
        accepted.close()
        accepted = None
        assert client.recv(1) == b""
        client.close()
        client = None
    finally:
        if accepted is not None:
            accepted.close()
        if client is not None:
            client.close()
        if server is not None:
            server.close()
    return port


def test_port_preflight_allows_reuse_enabled_time_wait(tmp_path: Path):
    launcher = NativeLauncher(_config(tmp_path))
    port = _server_port_in_time_wait()

    launcher._port_available(probes=(("127.0.0.1", port),))


def test_port_preflight_rejects_a_current_listener(tmp_path: Path):
    launcher = NativeLauncher(_config(tmp_path))
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen()
    try:
        with pytest.raises(LauncherError) as raised:
            launcher._port_available(
                probes=(("127.0.0.1", listener.getsockname()[1]),)
            )
        assert raised.value.code == "port_occupied"
        assert raised.value.exit_code == 72
    finally:
        listener.close()


def test_readiness_uses_local_bind_addresses_for_publicly_advertised_stack(
    tmp_path: Path, configuration_factory
):
    launcher = NativeLauncher(_config(tmp_path))
    urls: list[str] = []

    def response(url: str):
        urls.append(url)
        return {"status": "ready", "headless_projection_ready": True, "owner_id": "owner"}

    launcher._response = response
    launcher._program_runner_health = lambda: {
        "started": True, "closed": False, "owner_id": "program"
    }
    ready, rows, reason = launcher._readiness(configuration_factory())

    assert ready is True
    assert reason is None
    assert [row.state.value for row in rows] == ["ready", "ready", "ready"]
    assert [row.component_id for row in rows] == ["headless", "program_runner", "composer"]
    assert urls == [
        "http://127.0.0.1:9010/api/health",
        "http://127.0.0.1:9000/api/health",
    ]


def test_readiness_reads_the_program_runner_over_its_command_socket(tmp_path: Path):
    import manyforge_headless_py as core

    launcher = NativeLauncher(_config(tmp_path))
    root = launcher.config.manager_run_root
    root.mkdir(parents=True, mode=0o700)
    with pytest.raises(ValueError, match="program_runner_unavailable"):
        launcher._program_runner_health()
    server = core.ProgramCommandServer(str(root))
    server.start()
    try:
        answer = threading.Thread(
            target=lambda: server.reply(
                *(lambda token, request: (
                    token,
                    {"ok": True, "value": {"started": True, "closed": False,
                                           "seen": request}},
                ))(*server.next(10.0))
            )
        )
        answer.start()
        assert launcher._program_runner_health() == {
            "started": True,
            "closed": False,
            "seen": {"command": "health", "request": {}},
        }
        answer.join()
    finally:
        server.close()


def test_the_program_runner_probe_ends_within_its_two_seconds(tmp_path: Path):
    """As the former HTTP probe (2 s): a runner that never answers is
    unavailable, not a readiness read that waits on."""

    import time

    import manyforge_headless_py as core

    launcher = NativeLauncher(_config(tmp_path))
    root = launcher.config.manager_run_root
    root.mkdir(parents=True, mode=0o700)
    server = core.ProgramCommandServer(str(root))
    server.start()
    try:
        started = time.monotonic()
        with pytest.raises(ValueError, match="program_runner_unavailable"):
            launcher._program_runner_health()
        assert 2.0 <= time.monotonic() - started < 10.0
        assert server.next(10.0) is not None
    finally:
        server.close()


@pytest.mark.parametrize("cat_present,socket_present", [(False, True), (True, False)])
def test_detached_start_refuses_missing_journald_before_spawn(tmp_path, monkeypatch,
                                                           cat_present, socket_present):
    from manyforge_operator.orchestrator import _journal_command
    monkeypatch.setattr("manyforge_operator.orchestrator.shutil.which", lambda name: "/usr/bin/systemd-cat" if cat_present else None)
    monkeypatch.setattr(Path, "is_socket", lambda path: socket_present)
    with pytest.raises(LauncherError, match="requires systemd-cat and the journald stream socket") as raised:
        _journal_command("cell-a", "manager", ["/bin/true"])
    assert raised.value.code == "journald_unavailable"


def test_detached_spawn_routes_to_journal_and_keeps_target_identity(tmp_path, monkeypatch):
    from manyforge_operator.orchestrator import ProcessSpec
    config = _config(tmp_path)
    launcher = NativeLauncher(config)
    captured = {}
    monkeypatch.setattr("manyforge_operator.orchestrator.shutil.which", lambda name: "/usr/bin/systemd-cat")
    monkeypatch.setattr(Path, "is_socket", lambda path: True)
    def popen(argv, **kwargs):
        captured["argv"] = argv
        captured["kwargs"] = kwargs
        return type("Process", (), {"pid": 123})()
    monkeypatch.setattr("manyforge_operator.orchestrator.subprocess.Popen", popen)
    monkeypatch.setattr("manyforge_operator.orchestrator._proc_observation", lambda pid: (456, "S"))
    monkeypatch.setattr("manyforge_operator.orchestrator._boot_id", lambda: "boot")
    monkeypatch.setattr("manyforge_operator.orchestrator.os.getpgid", lambda pid: pid)
    spec = ProcessSpec("manager", ("/bin/echo", "some value"), {"CUSTOM": "value"}, tmp_path)
    managed = launcher._spawn(spec)
    assert captured["argv"] == ["/usr/bin/systemd-cat", "-t", "manyforge-workbench-manager", "--", *spec.argv]
    assert captured["kwargs"]["stdout"] == -3
    # The Manager ends with this launcher (PR_SET_PDEATHSIG, spec 3.3).
    assert captured["kwargs"]["start_new_session"] is True
    assert callable(captured["kwargs"]["preexec_fn"])
    assert captured["kwargs"]["env"]["CUSTOM"] == "value"
    assert managed.argv == spec.argv
    assert managed.log_path == "journalctl -t manyforge-workbench-manager"
    assert not (launcher.store.root / "logs").exists()


def test_start_below_the_disk_floor_is_refused_before_anything_is_written(
    tmp_path: Path, configuration_factory, monkeypatch, journald
):
    config, launcher, spawned = _resident_launcher(
        tmp_path, configuration_factory(), monkeypatch, [_Manager(0)]
    )
    monkeypatch.setattr(
        run_session, "read_disk", lambda _fd: run_session.DiskReading(10**15, 2_500_000_000, 10**6)
    )
    with pytest.raises(LauncherError) as refused:
        launcher.start()
    assert refused.value.code == "disk_space_below_floor"
    assert refused.value.exit_code == 71
    assert str(refused.value) == (
        f"Not started: the disk that holds {config.instance_root} has 2.5 GB free. ManyForge needs at "
        "least 10 GB free (10 GB plus 0 GB for the machine). Free 7.5 GB and start again."
    )
    assert spawned == []
    assert launcher.store.read() is None
    assert not (config.instance_root / "current").exists()
    assert not (config.instance_root / "session-reports").exists()


def test_no_free_inode_refuses_the_start(tmp_path: Path, configuration_factory, monkeypatch, journald):
    _config_, launcher, spawned = _resident_launcher(
        tmp_path, configuration_factory(), monkeypatch, [_Manager(0)]
    )
    monkeypatch.setattr(run_session, "read_disk", lambda _fd: run_session.DiskReading(10**15, 10**15, 0))
    with pytest.raises(LauncherError) as refused:
        launcher.start()
    assert refused.value.code == "disk_space_below_floor" and spawned == []


def test_a_selection_that_meets_a_full_disk_names_the_instance_folder(
    tmp_path: Path, configuration_factory, monkeypatch, journald
):
    config, launcher, spawned = _resident_launcher(
        tmp_path, configuration_factory(), monkeypatch, [_Manager(0)]
    )

    def full_disk(*_args, **_kwargs):
        # A failed write of an open file: the OS text names no path.
        raise OSError(errno.ENOSPC, "No space left on device")

    monkeypatch.setattr(run_session, "prepare_session", full_disk)
    with pytest.raises(LauncherError) as refused:
        launcher.start()
    assert refused.value.code == "session_selection_unavailable"
    assert refused.value.exit_code == 71
    assert str(refused.value) == f"{config.instance_root}: [Errno 28] No space left on device"
    assert spawned == []


def test_start_prunes_previous_runs_before_the_selection(
    tmp_path: Path, configuration_factory, monkeypatch, journald
):
    configuration = configuration_factory()
    config, launcher, _spawned = _resident_launcher(
        tmp_path, configuration, monkeypatch, [_Manager(0), _Manager(0)]
    )
    launcher.start()
    archives = []
    for index in range(7):
        folder = config.instance_root / "previous-runs" / f"{index + 16:032x}"
        folder.mkdir(parents=True)
        os.utime(folder, ns=(index + 1, index + 1))
        archives.append(folder.name)
    seen = []
    original = run_session.prepare_session

    def selection(*args, **kwargs):
        seen.append(sorted(path.name for path in (config.instance_root / "previous-runs").iterdir()))
        return original(*args, **kwargs)

    monkeypatch.setattr(run_session, "prepare_session", selection)
    launcher.start()
    # Pruned first: four previous runs and current/ remain when the
    # selection archives current/.
    assert seen == [archives[3:]]


def test_the_wait_loop_prunes_only_through_the_bounded_running_pass(
    tmp_path: Path, configuration_factory, monkeypatch, journald
):
    config, launcher, _spawned = _resident_launcher(
        tmp_path, configuration_factory(), monkeypatch, [_Manager(0, polls=3)]
    )
    monkeypatch.setattr(run_session, "DISK_PRUNE_PERIOD_SECONDS", 0.0)
    calls = []
    monkeypatch.setattr(
        run_session,
        "prune_while_running",
        lambda root, fd, limits, **_options: calls.append((root, limits)),
    )
    launcher.start()
    assert calls and all(call == (config.instance_root, config.disk_limits) for call in calls)


def test_the_site_settings_option_sets_only_the_disk_limits(tmp_path: Path, journald, capsys):
    from manyforge_operator import cli

    config = _config(tmp_path)
    arguments = [
        "--state-root", str(config.state_root), "start",
        "--source-root", str(config.source_root),
        "--manager-executable", str(config.manager_executable),
        "--install-root", str(config.install_root),
        "--node-catalog-path", str(config.node_catalog_path),
        "--alarm-catalog-path", str(config.alarm_catalog_path),
        "--package-aggregate-sha256", config.package_aggregate_sha256,
        "--compiler-build-manifest-sha256", config.compiler_build_manifest_sha256,
        "--node-environment-sha256", config.node_environment_sha256,
    ]
    # Without the option, the defaults.
    assert cli._config(cli._parser().parse_args(arguments)).disk_limits == run_session.DiskLimits()
    site = tmp_path / "manyforge.site.json"
    site.write_text(
        '{"schema":"manyforge.site.v2","device_addresses":{"lite6_main":"192.168.1.31"},'
        '"disk_max_percent":20,"disk_always_free_bytes":5000000000}', encoding="utf-8")
    configured = cli._config(cli._parser().parse_args([*arguments, "--site-settings", str(site)]))
    assert configured.disk_limits == run_session.DiskLimits(20, 5_000_000_000, 0)
    # The site file is not passed on to the Manager.
    manager, _processes = native_commands(configured)
    assert str(site) not in manager.argv and "--site-settings" not in manager.argv
    # An out-of-range value refuses the start, typed.
    site.write_text('{"schema":"manyforge.site.v2","disk_max_percent":0}', encoding="utf-8")
    assert cli.main([*arguments, "--site-settings", str(site)]) == 64
    error = capsys.readouterr().err
    assert "operator_configuration_invalid: site settings disk_max_percent must be an integer from 1 to 100" in error


def test_the_site_settings_option_is_refused_with_the_isaac_profile(tmp_path: Path, capsys):
    from manyforge_operator import cli

    site = tmp_path / "manyforge.site.json"
    site.write_text('{"schema":"manyforge.site.v2"}', encoding="utf-8")
    assert cli.main([
        "--state-root", str(tmp_path / "state"), "start", "--profile", "isaac",
        "--source-root", str(tmp_path), "--site-settings", str(site),
    ]) == 64
    assert "--site-settings applies to the standard profile only" in capsys.readouterr().err
