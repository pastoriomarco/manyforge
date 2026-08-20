from __future__ import annotations

import json
import os
from pathlib import Path
import shutil

import pytest
import manyforge_operator.package_aggregate as package_aggregate_module

from manyforge_operator import orchestrator
from manyforge_operator.cli import main as operator_main
from manyforge_operator.orchestrator import (
    LauncherError,
    NativeLauncher,
    NativeLauncherConfig,
)
from manyforge_operator.package_aggregate import (
    PackageAggregateUnavailable,
    main,
    package_aggregate_sha256,
)


def _root(tmp_path: Path) -> Path:
    root = tmp_path / "package"
    root.mkdir()
    (root / "a").write_bytes(b"")
    return root


def _config(tmp_path: Path, package_root: Path) -> NativeLauncherConfig:
    source = tmp_path / "source"
    source.mkdir()
    manager = tmp_path / "manager"
    manager.write_text("#!/bin/sh\n", encoding="utf-8")
    manager.chmod(0o755)
    catalog = package_root / "a"
    return NativeLauncherConfig(
        source_root=source,
        manager_executable=manager,
        package_root=package_root,
        node_catalog_path=catalog,
        alarm_catalog_path=catalog,
        publication_root=tmp_path / "publication",
        manager_state_root=tmp_path / "manager-state",
        runtime_state_root=tmp_path / "runtime-state",
        execution_state_root=tmp_path / "execution-state",
        logical_scene_database=tmp_path / "scene.sqlite3",
        historian_database_path=tmp_path / "history.sqlite3",
        state_root=tmp_path / "operator-state",
        package_aggregate_sha256=package_aggregate_sha256(package_root),
        compiler_build_manifest_sha256="b" * 64,
        node_environment_sha256="c" * 64,
    )


def test_fixed_independent_vector_and_copied_root(tmp_path: Path):
    root = _root(tmp_path)
    expected = "624603957c2bca2c300246ff93369f7e7874d067655d6129adfc89e7e3fb5952"

    assert package_aggregate_sha256(root) == expected
    copied = tmp_path / "copied"
    shutil.copytree(root, copied)
    assert package_aggregate_sha256(copied) == expected


def test_content_path_add_delete_rename_and_lock_files_change_digest(
    tmp_path: Path,
):
    root = _root(tmp_path)
    baseline = package_aggregate_sha256(root)
    (root / "a").write_bytes(b"x")
    content = package_aggregate_sha256(root)
    assert content != baseline
    (root / "a").rename(root / "b")
    renamed = package_aggregate_sha256(root)
    assert renamed != content
    (root / "nested").mkdir()
    (root / "nested" / "added").write_bytes(b"")
    added = package_aggregate_sha256(root)
    assert added != renamed
    (root / "nested" / "added").unlink()
    deleted = package_aggregate_sha256(root)
    assert deleted != added
    (root / ".repository.lock").write_bytes(b"lock")
    repository_lock = package_aggregate_sha256(root)
    assert repository_lock != deleted
    (root / "catalog.lock").write_bytes(b"catalog")
    assert package_aggregate_sha256(root) != repository_lock


def test_permissions_and_timestamps_do_not_change_digest(tmp_path: Path):
    root = _root(tmp_path)
    baseline = package_aggregate_sha256(root)

    (root / "a").chmod(0o400)
    os.utime(root / "a", ns=(1_000_000_000, 2_000_000_000))

    assert package_aggregate_sha256(root) == baseline


def test_empty_symlink_fifo_and_nonregular_descendants_are_unavailable(
    tmp_path: Path,
):
    empty = tmp_path / "empty"
    empty.mkdir()
    with pytest.raises(PackageAggregateUnavailable):
        package_aggregate_sha256(empty)

    root = _root(tmp_path)
    link_root = tmp_path / "root-link"
    link_root.symlink_to(root, target_is_directory=True)
    with pytest.raises(PackageAggregateUnavailable):
        package_aggregate_sha256(link_root)

    (root / "link").symlink_to(root / "a")
    with pytest.raises(PackageAggregateUnavailable):
        package_aggregate_sha256(root)
    (root / "link").unlink()
    os.mkfifo(root / "fifo")
    with pytest.raises(PackageAggregateUnavailable):
        package_aggregate_sha256(root)


def test_non_nfc_and_invalid_utf8_paths_are_unavailable(tmp_path: Path):
    root = _root(tmp_path)
    (root / "e\u0301").write_bytes(b"x")
    with pytest.raises(PackageAggregateUnavailable):
        package_aggregate_sha256(root)
    (root / "e\u0301").unlink()

    invalid = os.fsencode(root) + b"/invalid-\xff"
    descriptor = os.open(invalid, os.O_WRONLY | os.O_CREAT, 0o600)
    os.close(descriptor)
    try:
        with pytest.raises(PackageAggregateUnavailable):
            package_aggregate_sha256(root)
    finally:
        os.unlink(invalid)


def test_detected_file_and_enumeration_change_are_unavailable(
    tmp_path: Path, monkeypatch
):
    root = _root(tmp_path)
    (root / "a").write_bytes(b"content")
    real_read = os.read
    changed = False

    def changing_read(file_descriptor: int, count: int) -> bytes:
        nonlocal changed
        value = real_read(file_descriptor, count)
        if value and not changed:
            changed = True
            (root / "a").write_bytes(b"different")
        return value

    monkeypatch.setattr(os, "read", changing_read)
    with pytest.raises(PackageAggregateUnavailable):
        package_aggregate_sha256(root)
    monkeypatch.setattr(os, "read", real_read)

    real_enumerate = package_aggregate_module._enumerate
    calls = 0

    def changing_enumeration(root_fd: int):
        nonlocal calls
        calls += 1
        value = real_enumerate(root_fd)
        if calls == 1:
            (root / "added").write_bytes(b"x")
        return value

    monkeypatch.setattr(
        package_aggregate_module, "_enumerate", changing_enumeration
    )
    with pytest.raises(PackageAggregateUnavailable):
        package_aggregate_sha256(root)


def test_file_replaced_by_fifo_before_open_is_unavailable_without_blocking(
    tmp_path: Path, monkeypatch
):
    root = _root(tmp_path)
    real_open = os.open
    replaced = False

    def replacing_open(path, flags, *args, **kwargs):
        nonlocal replaced
        if path == "a" and not flags & os.O_DIRECTORY:
            assert flags & os.O_NONBLOCK
            (root / "a").unlink()
            os.mkfifo(root / "a")
            replaced = True
        return real_open(path, flags, *args, **kwargs)

    monkeypatch.setattr(os, "open", replacing_open)

    with pytest.raises(PackageAggregateUnavailable):
        package_aggregate_sha256(root)

    assert replaced is True


def test_module_cli_success_and_error_shapes(tmp_path: Path, capsys):
    root = _root(tmp_path)
    digest = package_aggregate_sha256(root)

    assert main(["--package-root", str(root)]) == 0
    captured = capsys.readouterr()
    assert captured.out == f"{digest}\n"
    assert captured.err == ""

    assert main(["--package-root", str(tmp_path / "missing")]) == 70
    captured = capsys.readouterr()
    assert captured.out == ""
    assert captured.err == "package_aggregate_unavailable\n"


def test_operator_cli_missing_package_precedes_state_creation_with_valid_inputs(
    tmp_path: Path, capsys
):
    source = tmp_path / "source"
    source.mkdir()
    manager = tmp_path / "manager"
    manager.write_text("#!/bin/sh\n", encoding="utf-8")
    manager.chmod(0o755)
    node_catalog = tmp_path / "node_catalog.yaml"
    node_catalog.write_text("nodes: []\n", encoding="utf-8")
    alarm_catalog = tmp_path / "alarm_catalog.yaml"
    alarm_catalog.write_text("alarms: []\n", encoding="utf-8")
    missing_package = tmp_path / "missing-package"
    state_root = tmp_path / "operator-state"

    result = operator_main(
        [
            "--json",
            "--state-root",
            str(state_root),
            "start",
            "--source-root",
            str(source),
            "--manager-executable",
            str(manager),
            "--package-root",
            str(missing_package),
            "--node-catalog-path",
            str(node_catalog),
            "--alarm-catalog-path",
            str(alarm_catalog),
            "--publication-root",
            str(tmp_path / "publication"),
            "--manager-state-root",
            str(tmp_path / "manager-state"),
            "--runtime-state-root",
            str(tmp_path / "runtime-state"),
            "--execution-state-root",
            str(tmp_path / "execution-state"),
            "--logical-scene-database",
            str(tmp_path / "scene.sqlite3"),
            "--historian-database-path",
            str(tmp_path / "history.sqlite3"),
            "--package-aggregate-sha256",
            "a" * 64,
            "--compiler-build-manifest-sha256",
            "b" * 64,
            "--node-environment-sha256",
            "c" * 64,
        ]
    )

    captured = capsys.readouterr()
    assert result == 71
    assert json.loads(captured.out)["error"]["code"] == (
        "package_aggregate_unavailable"
    )
    assert captured.err == ""
    assert not state_root.exists()


@pytest.mark.parametrize("unavailable", [False, True])
def test_native_verifies_before_state_store_or_other_prestart_effects(
    tmp_path: Path, monkeypatch, unavailable: bool
):
    root = _root(tmp_path)
    config = _config(tmp_path, root)
    called = False

    def state_store(_config):
        nonlocal called
        called = True
        raise AssertionError("state store must not be created")

    monkeypatch.setattr(orchestrator, "_StateStore", state_store)
    if unavailable:
        def observe(_root):
            raise PackageAggregateUnavailable("changed")
        expected_code = "package_aggregate_unavailable"
    else:
        def observe(_root):
            return "d" * 64
        expected_code = "package_aggregate_mismatch"
    monkeypatch.setattr(orchestrator, "package_aggregate_sha256", observe)

    with pytest.raises(LauncherError) as raised:
        NativeLauncher(config)

    assert raised.value.code == expected_code
    assert called is False
    assert not config.instance_root.exists()


def test_native_matching_observation_is_persisted_as_artifact_identity(
    tmp_path: Path, monkeypatch, source_factory
):
    root = _root(tmp_path)
    config = _config(tmp_path, root)
    observed = config.package_aggregate_sha256
    launcher = NativeLauncher(config)
    monkeypatch.setattr(orchestrator, "_source_identity", lambda _root: source_factory())
    monkeypatch.setattr(
        orchestrator,
        "_boot_id",
        lambda: "22222222-2222-2222-2222-222222222222",
    )

    configuration = orchestrator._configuration(launcher.config)

    assert configuration.artifacts[0].package_aggregate_sha256 == observed
