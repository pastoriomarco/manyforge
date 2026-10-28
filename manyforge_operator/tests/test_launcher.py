from __future__ import annotations

import os
from dataclasses import replace
from pathlib import Path
import socket

from fastapi.testclient import TestClient
import pytest

from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.security import SecurityConfig
from manyforge_composer.backend.state import ComposerState
from manyforge_operator import LaunchProfile, LaunchInstanceState, ResourcePresence
from manyforge_operator.cell_layout import CellInstanceLayout
from manyforge_operator.orchestrator import (
    LauncherError,
    NativeLauncher,
    NativeLauncherConfig,
    _StateStore,
    _proc_observation,
    native_commands,
)
from manyforge_operator.package_aggregate import package_aggregate_sha256


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


def test_native_commands_retain_five_owner_entrypoints(tmp_path: Path):
    config = _config(tmp_path)
    specs = native_commands(config)
    assert [spec.component_id for spec in specs] == [
        "manager",
        "headless",
        "program_runner",
        "assistant_bridge",
        "composer",
    ]
    commands = [" ".join(spec.argv) for spec in specs]
    assert "--canonical-empty-admission-filename canonical-empty-admission.json" in commands[0]
    assert specs[0].argv[
        specs[0].argv.index("--reconciliation-period-ms") + 1
    ] == "500"
    assert "--cell-id native" in commands[1]
    assert "--manager-timeout-s 60.0" in commands[1]
    assert specs[1].argv[
        specs[1].argv.index("--node-environment-manifest-path") + 1
    ] == str(
        config.source_root
        / "manyforge_behavior/resources/node_environment_manifest.yaml"
    )
    assert "--execution-run-root" in specs[1].argv
    assert specs[1].argv[specs[1].argv.index("--execution-run-root") + 1] == str(
        config.execution_run_root
    )
    assert specs[0].argv[specs[0].argv.index("--execution-state-root") + 1] == str(
        config.execution_state_root
    )
    assert specs[0].argv[specs[0].argv.index("--execution-run-root") + 1] == str(
        config.execution_run_root
    )
    assert "--port 9010" in commands[1]
    assert "--port 9011" in commands[2]
    assert specs[2].argv[
        specs[2].argv.index("--snapshot-url") + 1
    ] == "http://127.0.0.1:9000/api/scene/snapshot/render"
    expected_liveness = str(
        config.manager_run_root / "program-runner-liveness.sock"
    )
    assert (
        specs[0].argv[
            specs[0].argv.index("--program-runner-liveness-socket") + 1
        ]
        == expected_liveness
    )
    assert (
        specs[2].argv[specs[2].argv.index("--manager-liveness-socket") + 1]
        == expected_liveness
    )
    assert specs[2].argv[specs[2].argv.index("--cell-id") + 1] == config.cell_id
    assert specs[3].argv[1:] == ("-m", "manyforge_assistant_bridge.bridge")
    assert specs[3].environment == {
        "MANYFORGE_HEADLESS_API_URL": "http://127.0.0.1:9010",
        "MANYFORGE_COMPOSER_URL": "http://127.0.0.1:9000",
        "MANYFORGE_ASSISTANT_BRIDGE_HOST": "127.0.0.1",
        "MANYFORGE_ASSISTANT_BRIDGE_PORT": "8100",
        "MANYFORGE_ASSISTANT_AUDIT_LOG": str(
            config.instance_root / "state" / "assistant-bridge" / "audit.jsonl"
        ),
    }
    assert "--port 9000" in commands[4]
    assert specs[4].argv[
        specs[4].argv.index("--assistant-bridge-url") + 1
    ] == "http://127.0.0.1:8100"
    assert all("--apply" not in spec.argv and "--run" not in spec.argv for spec in specs)
    assert all(
        spec.environment["MANYFORGE_FS_ROOTS"] == str(config.install_root)
        for spec in specs[:3]
    )
    assert specs[4].environment == {
        "MANYFORGE_FS_ROOTS": (
            f"{config.source_root / 'examples'},"
            f"{config.install_root},{config.program_workspace}"
        ),
        "MANYFORGE_PROGRAM_WRITE_ROOTS": str(config.program_workspace),
        "MANYFORGE_DEPLOYMENT_WRITE_ROOTS": str(config.program_workspace),
    }


def test_native_ordered_roots_make_examples_the_default_browse_root(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
):
    config = _config(tmp_path)
    composer_environment = native_commands(config)[-1].environment
    for name, value in composer_environment.items():
        monkeypatch.setenv(name, value)

    client = TestClient(
        create_app(ComposerState(), security=SecurityConfig.from_env())
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
        ("instance_id", "native,extra", "path-safe"),
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
    instance_id = "native"
    prospective_workspace = (
        state_root / instance_id / "state" / "composer-authoring" /
        "program-workspace"
    )
    package_root = config.install_root

    if relationship == "workspace-inside-package":
        state_root = package_root / "operator-state"
        prospective_workspace = (
            state_root / instance_id / "state" / "composer-authoring" /
            "program-workspace"
        )
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


def test_start_records_and_preserves_the_instance_program_workspace(
    tmp_path: Path, configuration_factory, monkeypatch
):
    from manyforge_operator.tests.conftest import make_readiness

    config = _config(tmp_path)
    launcher = NativeLauncher(config)
    configuration = configuration_factory()
    active_pids: set[int] = set()
    spawn_count = 0

    monkeypatch.setattr(
        "manyforge_operator.orchestrator._configuration",
        lambda _config: configuration,
    )
    monkeypatch.setattr(launcher, "_port_available", lambda: None)
    monkeypatch.setattr(
        "manyforge_operator.orchestrator._process_is_authenticated",
        lambda record: int(record["pid"]) in active_pids,
    )
    monkeypatch.setattr(
        launcher,
        "_readiness",
        lambda _configuration=None: (True, [make_readiness()], None),
    )

    def spawn(spec):
        nonlocal spawn_count
        spawn_count += 1
        assert config.program_workspace.is_dir()
        pid = 1000 + spawn_count
        active_pids.add(pid)
        return type(
            "Managed",
            (),
            {
                "component_id": spec.component_id,
                "pid": pid,
                "start_ticks": 1,
                "boot_id": "22222222-2222-2222-2222-222222222222",
                "process_group_id": pid,
                "argv": spec.argv,
                "log_path": config.instance_root / f"{spec.component_id}.log",
            },
        )()

    def cleanup(process_records=None, *, manager_run_root=None):
        assert process_records is not None
        assert manager_run_root == config.manager_run_root
        assert {int(item["pid"]) for item in process_records.values()} == active_pids
        active_pids.clear()
        return True

    monkeypatch.setattr(launcher, "_spawn", spawn)
    monkeypatch.setattr(launcher, "_cleanup_authenticated", cleanup)
    package_before = package_aggregate_sha256(config.install_root)

    first = launcher.start()
    assert first.state is LaunchInstanceState.READY
    assert all(
        observation.presence is ResourcePresence.PRESENT
        for observation in first.resource_observations
    )
    assert spawn_count == 5
    saved = config.program_workspace / "saved.program.yaml"
    saved.write_text("schema_version: manyforge.program.v2\n", encoding="utf-8")
    record = launcher.store.read()
    assert record is not None
    assert record[1]["paths"]["program_workspace"] == str(
        config.program_workspace
    )

    idempotent = launcher.start()
    assert launcher.last_start_idempotent is True
    assert idempotent.state is LaunchInstanceState.READY
    assert spawn_count == 5
    assert saved.is_file()

    stopped = launcher.stop()
    assert stopped is not None
    assert stopped.state is LaunchInstanceState.STOPPED
    assert all(
        observation.presence is ResourcePresence.ABSENT
        for observation in stopped.resource_observations
    )
    assert not active_pids
    assert saved.is_file()
    restarted = launcher.start()
    assert restarted.state is LaunchInstanceState.READY
    assert all(
        observation.presence is ResourcePresence.PRESENT
        for observation in restarted.resource_observations
    )
    assert spawn_count == 10
    assert saved.read_text(encoding="utf-8") == (
        "schema_version: manyforge.program.v2\n"
    )
    assert package_aggregate_sha256(config.install_root) == package_before


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


def test_native_commands_bind_explicit_cell_admission_and_static_inputs(tmp_path: Path):
    static_dir = tmp_path / "static"
    static_dir.mkdir()
    (static_dir / "index.html").write_text("<html></html>\n", encoding="utf-8")
    config = replace(
        _config(tmp_path),
        canonical_empty_admission_filename="canonical-empty-admission.lite6.json",
        cell_id="lite6_pick_place_cell",
        composer_static_dir=static_dir,
    )

    specs = native_commands(config)

    assert "canonical-empty-admission.lite6.json" in specs[0].argv
    assert "lite6_pick_place_cell" in specs[1].argv
    assert specs[-1].argv[-2:] == ("--static-dir", str(static_dir))


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


def test_native_commands_pass_explicit_composer_static_dir(tmp_path: Path):
    static_dir = tmp_path / "static"
    static_dir.mkdir()
    (static_dir / "index.html").write_text("<html></html>\n", encoding="utf-8")
    config = replace(_config(tmp_path), composer_static_dir=static_dir)
    composer = native_commands(config)[-1]
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
        root=config.instance_root / "state" / "operator",
        lock_root=config.instance_root / "run" / "operator",
    )
    from manyforge_operator.tests.conftest import make_status

    status = make_status(configuration_factory(), state=LaunchInstanceState.STARTING, with_resources=False)
    with store.lock():
        store.write(status, {"profile": "native", "processes": {}})
        loaded = store.read()
    assert loaded is not None
    assert loaded[0] == status
    assert loaded[1]["profile"] == "native"
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
    operator_state = config.instance_root / "state" / "operator"
    operator_state.rmdir()

    with pytest.raises(LauncherError, match="operator state root is unavailable"):
        _StateStore(
            root=operator_state,
            lock_root=config.instance_root / "run" / "operator",
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
    operator_state = config.instance_root / "state" / "operator"
    store = _StateStore(
        root=operator_state,
        lock_root=config.instance_root / "run" / "operator",
    )
    detached = config.instance_root / "state" / "operator.detached"
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
        root=config.instance_root / "state" / "operator",
        lock_root=config.instance_root / "run" / "operator",
    )
    outside = tmp_path / "outside.json"
    outside.write_text("unchanged\n", encoding="utf-8")
    store.record_path.symlink_to(outside)

    with pytest.raises(LauncherError) as raised:
        store.read()

    assert raised.value.code == "status_record_invalid"
    assert outside.read_text(encoding="utf-8") == "unchanged\n"


def test_state_store_lock_never_recreates_a_missing_typed_run_root(
    tmp_path: Path,
) -> None:
    config = _config(tmp_path)
    CellInstanceLayout.open_or_create(
        cell_id=config.cell_id,
        cell_root=config.instance_root,
        install_root=config.install_root,
    )
    run_root = config.instance_root / "run" / "operator"
    store = _StateStore(
        root=config.instance_root / "state" / "operator",
        lock_root=run_root,
    )
    run_root.rmdir()

    with pytest.raises(LauncherError) as raised:
        with store.lock():
            pass

    assert raised.value.code == "cell_layout_invalid"
    assert not run_root.exists()


def test_state_store_log_creation_rejects_a_symlink_directory(
    tmp_path: Path,
) -> None:
    config = _config(tmp_path)
    CellInstanceLayout.open_or_create(
        cell_id=config.cell_id,
        cell_root=config.instance_root,
        install_root=config.install_root,
    )
    run_root = config.instance_root / "run" / "operator"
    store = _StateStore(
        root=config.instance_root / "state" / "operator",
        lock_root=run_root,
    )
    outside = tmp_path / "outside-logs"
    outside.mkdir()
    (run_root / "logs").symlink_to(outside, target_is_directory=True)

    with pytest.raises(LauncherError) as raised:
        store.open_log("manager.log")

    assert raised.value.code == "log_path_invalid"
    assert list(outside.iterdir()) == []


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
        if ":9011/" in url:
            return {"started": True, "closed": False, "owner_id": "program"}
        return {"status": "ready", "headless_projection_ready": True, "owner_id": "owner"}

    launcher._response = response
    ready, rows, reason = launcher._readiness(configuration_factory())

    assert ready is True
    assert reason is None
    assert [row.state.value for row in rows] == ["ready", "ready", "ready", "ready"]
    assert urls == [
        "http://127.0.0.1:9010/api/health",
        "http://127.0.0.1:9011/api/program/health",
        "http://127.0.0.1:8100/health",
        "http://127.0.0.1:9000/api/health",
    ]


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
    assert captured["argv"] == ["/usr/bin/systemd-cat", "-t", "manyforge-native-manager", "--", *spec.argv]
    assert captured["kwargs"]["stdout"] == -3
    assert captured["kwargs"]["env"]["CUSTOM"] == "value"
    assert managed.argv == spec.argv
    assert managed.log_path == "journalctl -t manyforge-native-manager"
    assert not (launcher.store.root / "logs").exists()
