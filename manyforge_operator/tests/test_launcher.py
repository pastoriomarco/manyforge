from __future__ import annotations

import os
from dataclasses import replace
from pathlib import Path

import pytest

from manyforge_operator import LaunchProfile, LaunchInstanceState
from manyforge_operator.orchestrator import (
    NativeLauncher,
    NativeLauncherConfig,
    _StateStore,
    _proc_observation,
    native_commands,
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


def test_native_commands_retain_four_owner_entrypoints(tmp_path: Path):
    config = _config(tmp_path)
    specs = native_commands(config)
    assert [spec.component_id for spec in specs] == [
        "manager",
        "headless",
        "program_runner",
        "composer",
    ]
    commands = [" ".join(spec.argv) for spec in specs]
    assert "--canonical-empty-admission-filename canonical-empty-admission.json" in commands[0]
    assert "--cell-id native" in commands[1]
    assert "--manager-timeout-s 60.0" in commands[1]
    assert "--execution-state-root" in specs[1].argv
    assert specs[1].argv[specs[1].argv.index("--execution-state-root") + 1] == str(
        config.runtime_state_root
    )
    assert "--port 9010" in commands[1]
    assert "--port 9011" in commands[2]
    assert "--port 9000" in commands[3]
    assert all("--apply" not in spec.argv and "--run" not in spec.argv for spec in specs)
    assert all(spec.environment["MANYFORGE_FS_ROOTS"] == str(config.package_root) for spec in specs)


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
    assert valid.package_aggregate_sha256 == "a" * 64
    with pytest.raises(ValueError, match="package_aggregate_sha256"):
        NativeLauncherConfig(
            source_root=valid.source_root,
            manager_executable=valid.manager_executable,
            package_root=valid.package_root,
            node_catalog_path=valid.node_catalog_path,
            alarm_catalog_path=valid.alarm_catalog_path,
            publication_root=valid.publication_root,
            manager_state_root=valid.manager_state_root,
            runtime_state_root=valid.runtime_state_root,
            execution_state_root=valid.execution_state_root,
            logical_scene_database=valid.logical_scene_database,
            historian_database_path=valid.historian_database_path,
            state_root=valid.state_root,
            compiler_build_manifest_sha256=valid.compiler_build_manifest_sha256,
            node_environment_sha256=valid.node_environment_sha256,
        )


def test_atomic_state_store_round_trip(tmp_path: Path, configuration_factory):
    config = _config(tmp_path)
    store = _StateStore(config)
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


def test_proc_observation_authenticates_current_process():
    observation = _proc_observation(os.getpid())
    assert observation is not None
    assert observation[0] > 0


def test_launcher_start_outcome_flag_defaults_to_non_idempotent(tmp_path: Path):
    launcher = NativeLauncher(_config(tmp_path))
    assert launcher.last_start_idempotent is False
    assert launcher.last_stop_already_stopped is False


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
    assert [row.state.value for row in rows] == ["ready", "ready", "ready"]
    assert urls == [
        "http://127.0.0.1:9010/api/health",
        "http://127.0.0.1:9011/api/program/health",
        "http://127.0.0.1:9000/api/health",
    ]
