from __future__ import annotations

import copy
import hashlib
import json
import subprocess
import sys
from pathlib import Path
from typing import Any

import pytest

from verification.composable_cell_i2 import native_build_provenance as provenance
from verification.composable_cell_i2 import verify_qualification_manifest as verifier


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
BUILD_PROVENANCE_CONTRACT = (
    REPOSITORY_ROOT
    / "verification/composable_cell_i2/build_provenance_contract.v1.json"
)
COMPILE_MODES = json.loads(
    BUILD_PROVENANCE_CONTRACT.read_text(encoding="utf-8")
)["toolchain_contract"]["compile_modes"]
BUILD_PROVENANCE_CONTRACT_VALUE = json.loads(
    BUILD_PROVENANCE_CONTRACT.read_text(encoding="utf-8")
)


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _git_blob(path: Path, algorithm: str = "sha1") -> str:
    raw = path.read_bytes()
    return hashlib.new(
        algorithm, f"blob {len(raw)}\0".encode("ascii") + raw
    ).hexdigest()


def _write(path: Path, value: bytes = b"synthetic\n", *, executable: bool = False) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(value)
    if executable:
        path.chmod(0o755)
    return path


def _write_regular_archive(
    path: Path, members: list[tuple[str, bytes]]
) -> Path:
    if not members or len({name for name, _value in members}) != len(members):
        raise ValueError("synthetic archive members must be nonempty and unique")

    def header(name: str, size: int) -> bytes:
        fields = (
            name.ljust(16),
            "0".ljust(12),
            "0".ljust(6),
            "0".ljust(6),
            "100644".ljust(8),
            str(size).ljust(10),
            "`\n",
        )
        raw = "".join(fields).encode("ascii")
        if len(raw) != 60:
            raise ValueError("synthetic archive header is malformed")
        return raw

    long_names = [name for name, _value in members if len(name) > 15]
    offsets: dict[str, int] = {}
    string_table = bytearray()
    for name in long_names:
        offsets[name] = len(string_table)
        string_table.extend(f"{name}/\n".encode("utf-8"))

    raw = bytearray(b"!<arch>\n")
    if string_table:
        raw.extend(header("//", len(string_table)))
        raw.extend(string_table)
        if len(string_table) % 2:
            raw.extend(b"\n")
    for name, value in members:
        encoded_name = f"/{offsets[name]}" if name in offsets else f"{name}/"
        raw.extend(header(encoded_name, len(value)))
        raw.extend(value)
        if len(value) % 2:
            raw.extend(b"\n")
    return _write(path, bytes(raw))


def _roots(tmp_path: Path) -> tuple[Path, Path, provenance._Roots]:
    repository = tmp_path / "repository"
    output = tmp_path / "qualification-output"
    repository.mkdir()
    output.mkdir()
    return repository, output, provenance._Roots(repository.resolve(), output.resolve())


def _git(repository: Path, *args: str) -> None:
    subprocess.run(
        ["/usr/bin/git", "-C", str(repository), *args],
        check=True,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def _git_text(repository: Path, *args: str) -> str:
    return subprocess.run(
        ["/usr/bin/git", "-C", str(repository), *args],
        check=True,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    ).stdout.strip()


def _cmake_files_fixture(
    tmp_path: Path,
) -> tuple[Path, Path, provenance._Roots]:
    repository, output, roots = _roots(tmp_path)
    source = repository / "project"
    build = output / "build"
    source.mkdir()
    build.mkdir()
    _write(source / "CMakeLists.txt", b"cmake_minimum_required(VERSION 3.25)\n")
    _write(repository / "sibling.cmake", b"set(SIBLING true)\n")
    _git(repository, "init", "--quiet")
    _git(repository, "config", "user.name", "I2 Packet Test")
    _git(repository, "config", "user.email", "i2-packet@example.invalid")
    _git(repository, "add", ".")
    _git(repository, "commit", "--quiet", "-m", "fixture")
    return source.resolve(), build.resolve(), roots


def _cmake_files_evidence(
    *,
    source: Path,
    build: Path,
    roots: provenance._Roots,
    inputs: list[dict[str, Any]],
    source_logical: str = "/workspace/project",
    build_logical: str = "/qualification-output/build",
) -> tuple[list[dict[str, Any]], dict[Path, dict[str, Any]]]:
    return provenance._cmake_files_evidence(
        project_id="synthetic",
        cmake_files={
            "kind": "cmakeFiles",
            "version": {"major": 1, "minor": 0},
            "paths": {
                "source": source_logical,
                "build": build_logical,
            },
            "inputs": inputs,
        },
        expected_source_root=source,
        build_root=build,
        roots=roots,
    )


def test_cmake_files_evidence_collapses_only_identical_normalized_repeats(
    tmp_path: Path,
) -> None:
    source, build, roots = _cmake_files_fixture(tmp_path)
    generated = _write(build / "generated.cmake", b"set(SYNTHETIC true)\n")
    cmake_owned = Path(
        "/usr/share/cmake-3.25/Modules/Compiler/Clang-DetermineCompilerInternal.cmake"
    )
    assert cmake_owned.is_file()
    unique_inputs = [
        {"path": "CMakeLists.txt"},
        {"path": "../sibling.cmake"},
        {
            "path": "/qualification-output/build/generated.cmake",
            "isGenerated": True,
        },
        {
            "path": cmake_owned.as_posix(),
            "isExternal": True,
            "isCMake": True,
        },
    ]
    repeated_inputs = [
        *([unique_inputs[0]] * 11),
        *([unique_inputs[1]] * 2),
        *([unique_inputs[2]] * 2),
        *([unique_inputs[3]] * 5),
    ]

    expected_rows, expected_index = _cmake_files_evidence(
        source=source,
        build=build,
        roots=roots,
        inputs=unique_inputs,
    )
    actual_rows, actual_index = _cmake_files_evidence(
        source=source,
        build=build,
        roots=roots,
        inputs=repeated_inputs,
    )

    assert actual_rows == expected_rows
    assert actual_index == expected_index
    assert set(actual_index) == {
        (source / "CMakeLists.txt").resolve(),
        roots.repository / "sibling.cmake",
        generated.resolve(),
        cmake_owned.resolve(),
    }
    rows_by_path = {row["path"]: row for row in actual_rows}
    generated_row = rows_by_path["/qualification-output/build/generated.cmake"]
    assert generated_row["is_generated"] is True
    assert generated_row["is_external"] is False
    assert generated_row["is_cmake"] is False
    assert rows_by_path[cmake_owned.as_posix()]["is_cmake"] is True


def test_cmake_files_evidence_preserves_absolute_same_git_external_form(
    tmp_path: Path,
) -> None:
    source, build, roots = _cmake_files_fixture(tmp_path)

    rows, indexed = _cmake_files_evidence(
        source=source,
        build=build,
        roots=roots,
        inputs=[{"path": "/workspace/sibling.cmake", "isExternal": True}],
    )

    sibling = roots.repository / "sibling.cmake"
    assert set(indexed) == {sibling}
    assert rows[0]["path"] == "/workspace/sibling.cmake"
    assert rows[0]["is_external"] is True
    assert rows[0]["identity"]["kind"] == "git_tracked_source"
    assert rows[0]["identity"]["git_root"] == roots.repository.as_posix()

    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="scope flags disagree",
    ):
        _cmake_files_evidence(
            source=source,
            build=build,
            roots=roots,
            inputs=[{"path": "/workspace/sibling.cmake"}],
        )


def test_cmake_files_evidence_bounds_relative_source_generation_paths(
    tmp_path: Path,
) -> None:
    source, build, roots = _cmake_files_fixture(tmp_path)
    outside = _write(tmp_path / "outside.cmake", b"set(OUTSIDE true)\n")
    outside_directory = tmp_path / "outside-directory"
    outside_directory.mkdir()
    _write(outside_directory / "escaped.cmake", b"set(ESCAPED true)\n")
    (roots.repository / "escape-link.cmake").symlink_to(outside)
    (roots.repository / "escape-directory").symlink_to(
        outside_directory,
        target_is_directory=True,
    )

    for raw_path, flags, pattern in (
        ("../../outside.cmake", {}, "escapes its source Git root"),
        ("//usr/share/cmake-3.25/Modules/CMake.cmake", {}, "not canonical"),
        (
            "nested/../CMakeLists.txt",
            {},
            "not a canonical source-relative path",
        ),
        ("../escape-link.cmake", {}, "is a symlink"),
        ("../escape-directory/escaped.cmake", {}, "escapes its allowed root"),
        ("../sibling.cmake", {"isExternal": True}, "scope flags disagree"),
    ):
        with pytest.raises(provenance.NativeBuildProvenanceError, match=pattern):
            _cmake_files_evidence(
                source=source,
                build=build,
                roots=roots,
                inputs=[{"path": raw_path, **flags}],
            )


def test_cmake_files_evidence_uses_one_source_generation_for_relative_inputs(
    tmp_path: Path,
) -> None:
    repository, output, roots = _roots(tmp_path)
    source_git_root = output / "source-generation"
    source = source_git_root / "project"
    build = output / "build"
    source.mkdir(parents=True)
    build.mkdir()
    _write(source / "CMakeLists.txt", b"cmake_minimum_required(VERSION 3.25)\n")
    sibling = _write(source_git_root / "sibling.cmake", b"set(SIBLING true)\n")
    _git(source_git_root, "init", "--quiet")
    _git(source_git_root, "config", "user.name", "I2 Packet Test")
    _git(source_git_root, "config", "user.email", "i2-packet@example.invalid")
    _git(source_git_root, "add", ".")
    _git(source_git_root, "commit", "--quiet", "-m", "fixture")

    rows, indexed = _cmake_files_evidence(
        source=source.resolve(),
        build=build.resolve(),
        roots=roots,
        inputs=[{"path": "../sibling.cmake"}],
        source_logical="/qualification-output/source-generation/project",
    )

    sibling = sibling.resolve()
    assert set(indexed) == {sibling}
    assert rows[0]["path"] == "/qualification-output/source-generation/sibling.cmake"
    assert rows[0]["identity"]["kind"] == "git_tracked_source"
    assert rows[0]["identity"]["git_root"] == source_git_root.resolve().as_posix()

    nested = repository / "nested-source"
    nested.mkdir()
    _write(nested / "owned.cmake", b"set(NESTED true)\n")
    _git(nested, "init", "--quiet")
    _git(nested, "config", "user.name", "I2 Packet Test")
    _git(nested, "config", "user.email", "i2-packet@example.invalid")
    _git(nested, "add", ".")
    _git(nested, "commit", "--quiet", "-m", "nested")
    root_source = repository / "project"
    root_source.mkdir()
    _write(root_source / "CMakeLists.txt", b"cmake_minimum_required(VERSION 3.25)\n")
    _git(repository, "init", "--quiet")
    _git(repository, "config", "user.name", "I2 Packet Test")
    _git(repository, "config", "user.email", "i2-packet@example.invalid")
    _git(repository, "add", "project/CMakeLists.txt")
    _git(repository, "commit", "--quiet", "-m", "root")

    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="is not tracked by the qualified Git tree",
    ):
        _cmake_files_evidence(
            source=root_source.resolve(),
            build=build.resolve(),
            roots=roots,
            inputs=[{"path": "../nested-source/owned.cmake"}],
        )


def test_cmake_files_evidence_rejects_conflicting_raw_repeats(
    tmp_path: Path,
) -> None:
    source, build, roots = _cmake_files_fixture(tmp_path)
    cmake_owned = Path(
        "/usr/share/cmake-3.25/Modules/Compiler/Clang-DetermineCompilerInternal.cmake"
    )
    assert cmake_owned.is_file()

    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="cmakeFiles has conflicting repeats",
    ):
        _cmake_files_evidence(
            source=source,
            build=build,
            roots=roots,
            inputs=[
                {
                    "path": cmake_owned.as_posix(),
                    "isExternal": True,
                    "isCMake": True,
                },
                {
                    "path": cmake_owned.as_posix(),
                    "isExternal": True,
                },
            ],
        )


@pytest.mark.parametrize("object_format", ["sha1", "sha256"])
def test_git_source_generation_replays_index_bytes_and_untracked_policy(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    object_format: str,
) -> None:
    repository = tmp_path / "source"
    repository.mkdir()
    _git(repository, "init", "--quiet", f"--object-format={object_format}")
    _git(repository, "config", "user.name", "I2 Packet Test")
    _git(repository, "config", "user.email", "i2-packet@example.invalid")
    tracked = _write(repository / "tracked.txt", b"tracked\n")
    _write(repository / "tracked" / "nested.txt", b"nested\n")
    _write(repository / "executable.sh", b"#!/bin/sh\nexit 0\n", executable=True)
    (repository / "tracked-link").symlink_to(tracked.name)
    _git(repository, "add", ".")
    _git(repository, "commit", "--quiet", "-m", "fixture")

    original_run_text = provenance._run_text
    original_run_bytes = provenance._run_bytes

    def check_command(command: list[str]) -> None:
        if command[0] == "/usr/bin/git":
            assert command[1] == "--no-optional-locks"
            assert "write-tree" not in command

    def read_text(command: list[str], *args: Any, **kwargs: Any) -> str:
        check_command(command)
        return original_run_text(command, *args, **kwargs)

    def read_bytes(command: list[str], *args: Any, **kwargs: Any) -> bytes:
        check_command(command)
        return original_run_bytes(command, *args, **kwargs)

    monkeypatch.setattr(provenance, "_run_text", read_text)
    monkeypatch.setattr(provenance, "_run_bytes", read_bytes)

    clean = provenance.replay_git_source_generation(repository)
    assert clean["base_tree"] == clean["patched_index_tree"]
    assert clean["staged_path_count"] == 0
    assert clean["untracked_path_count"] == 0

    tracked.write_bytes(b"staged replacement\n")
    (repository / "tracked-link").unlink()
    _write(repository / "added.txt", b"added\n")
    _git(repository, "add", "-A")
    git_directory = repository / ".git"
    original_mode = git_directory.stat().st_mode & 0o7777
    index_before = (git_directory / "index").read_bytes()
    objects_before = {
        path.relative_to(git_directory / "objects").as_posix(): _sha256(path)
        for path in (git_directory / "objects").rglob("*")
        if path.is_file()
    }
    git_directory.chmod(0o555)
    try:
        staged = provenance.replay_git_source_generation(repository)
    finally:
        git_directory.chmod(original_mode)
    assert (git_directory / "index").read_bytes() == index_before
    assert {
        path.relative_to(git_directory / "objects").as_posix(): _sha256(path)
        for path in (git_directory / "objects").rglob("*")
        if path.is_file()
    } == objects_before
    expected_staged_tree = _git_text(repository, "write-tree")
    assert staged["patched_index_tree"] == expected_staged_tree
    assert staged["staged_path_count"] == 3

    _write(repository / "declared-input.txt", b"declared\n")
    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="undeclared untracked or ignored source inputs",
    ):
        provenance.replay_git_source_generation(repository)

    declared = provenance.replay_git_source_generation(
        repository, ["declared-input.txt"]
    )
    assert declared["allowed_untracked_paths"] == ["declared-input.txt"]


def test_tool_environment_is_closed_for_python_replay_and_loader(
    tmp_path: Path,
) -> None:
    source = tmp_path / "read-only-source"
    source.mkdir()
    probe = _write(
        source / "probe.py",
        b"import json, os\nprint(json.dumps(dict(os.environ), sort_keys=True))\n",
    )
    environment_root = tmp_path / "environment"
    home = environment_root / "home"
    temporary = environment_root / "tmp"
    cache_root = environment_root / "cache"
    for directory in (home, temporary, cache_root):
        directory.mkdir(parents=True, exist_ok=True)
    replay_environment = {
        "HOME": str(home),
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "PATH": "/usr/local/bin:/usr/bin:/bin",
        "PYTHONDONTWRITEBYTECODE": "1",
        "PYTHONHASHSEED": "0",
        "PYTHONNOUSERSITE": "1",
        "TMPDIR": str(temporary),
        "XDG_CACHE_HOME": str(cache_root),
    }
    original_mode = source.stat().st_mode & 0o7777
    source.chmod(0o555)
    try:
        actual_environment = json.loads(
            provenance._run_text(
                [sys.executable, str(probe)],
                source,
                "synthetic Python replay",
                replay_environment,
            )
        )
    finally:
        source.chmod(original_mode)
    assert actual_environment == {
        **replay_environment,
        "TZ": "UTC",
    }
    assert not (source / "__pycache__").exists()

    loader_environment = provenance._run_text(
        ["/usr/bin/env"],
        tmp_path,
        "synthetic loader replay",
        {"LD_LIBRARY_PATH": "/qualification-output/lib"},
    ).splitlines()
    assert "LD_LIBRARY_PATH=/qualification-output/lib" in loader_environment

    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="unsupported tool environment",
    ):
        provenance._run_text(
            ["/usr/bin/env"],
            tmp_path,
            "synthetic forbidden environment",
            {"PYTHONPATH": "/workspace"},
        )


def _configured_target_graph_fixture(
    tmp_path: Path,
) -> tuple[Path, Path, Path, dict[str, Any]]:
    repository = tmp_path / "repository"
    output = tmp_path / "qualification-output"
    build_root = output / "build/module"
    repository.mkdir()
    build_root.mkdir(parents=True)
    owner_path = "/qualification-output/build/core/libmanyforge_core.a"
    graph_lines = [
        'digraph "manyforge_i2" {',
        'node [ fontsize = "12" ];',
        *provenance._GRAPHVIZ_LEGEND,
        '    "i2node0" [ label = "manyforge_core_py", shape = tripleoctagon ];',
        f'    "i2node1" [ label = "{owner_path}", shape = septagon ];',
        '    "i2node0" -> "i2node1"  // '
        f"manyforge_core_py -> {owner_path}",
        "}",
    ]
    graph_path = _write(
        build_root / "manyforge-i2-target-graph.dot",
        ("\n".join(graph_lines) + "\n").encode(),
    )
    _write(
        build_root / "CMakeGraphVizOptions.cmake",
        provenance._GRAPHVIZ_OPTIONS,
    )

    alias = "<owner:manyforge_core/manyforge_core>"
    normalized_projection = {
        "nodes": [
            {"name": alias, "label": alias, "shape": "septagon"},
            {
                "name": "manyforge_core_py",
                "label": "manyforge_core_py",
                "shape": "tripleoctagon",
            },
        ],
        "edges": [
            {
                "source": "manyforge_core_py",
                "target": alias,
                "style": "solid",
            }
        ],
    }
    contract = {
        "projects": {},
        "configured_target_graph_contract": {
            "schema_version": (
                "manyforge.composable_cell_i2.configured_target_graph_contract.v1"
            ),
            "options_sha256": hashlib.sha256(
                provenance._GRAPHVIZ_OPTIONS
            ).hexdigest(),
            "project_node_alias_contracts": {
                "manyforge_core_py": {
                    "absolute_build_sibling_nodes": [
                        {
                            "sibling_build_directory": "core",
                            "artifact_relative_path": "libmanyforge_core.a",
                            "alias": alias,
                            "expected_raw_shape": "septagon",
                        }
                    ],
                    "named_nodes": [],
                }
            },
            "profiles": {
                "manyforge_core_py.testing_off": {
                    "project_id": "manyforge_core_py",
                    "build_testing": False,
                    "projection_sha256": provenance._canonical_sha256(
                        normalized_projection
                    ),
                    "local_target_roles": {
                        "production": [alias, "manyforge_core_py"],
                        "test_only": [],
                        "configured_only": [],
                        "external": [],
                    },
                }
            },
        },
    }
    return repository, output, graph_path, contract


def test_configured_target_graph_replay_normalizes_exact_owner_alias(
    tmp_path: Path,
) -> None:
    repository, output, _, contract = _configured_target_graph_fixture(tmp_path)

    evidence = provenance.replay_configured_target_graph(
        repository,
        output,
        project_id="manyforge_core_py",
        build_root=output / "build/module",
        build_testing=False,
        build_contract=contract,
    )

    assert evidence["local_target_roles"]["production"] == [
        "<owner:manyforge_core/manyforge_core>",
        "manyforge_core_py",
    ]
    assert evidence["owner_node_aliases"] == [
        {
            "source": "/qualification-output/build/core/libmanyforge_core.a",
            "alias": "<owner:manyforge_core/manyforge_core>",
            "expected_raw_shape": "septagon",
        }
    ]
    binding_external = BUILD_PROVENANCE_CONTRACT_VALUE[
        "configured_target_graph_contract"
    ]["profiles"]["manyforge_core_py.testing_off"]["local_target_roles"][
        "external"
    ]
    assert "pybind11::lto" not in binding_external


def test_configured_target_graph_rejects_wrong_owner_shape(tmp_path: Path) -> None:
    repository, output, graph_path, contract = _configured_target_graph_fixture(
        tmp_path
    )
    graph_path.write_text(
        graph_path.read_text(encoding="utf-8").replace(
            (
                'label = "/qualification-output/build/core/'
                'libmanyforge_core.a", shape = septagon'
            ),
            (
                'label = "/qualification-output/build/core/'
                'libmanyforge_core.a", shape = octagon'
            ),
        ),
        encoding="utf-8",
    )

    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="owner-node label",
    ):
        provenance.replay_configured_target_graph(
            repository,
            output,
            project_id="manyforge_core_py",
            build_root=output / "build/module",
            build_testing=False,
            build_contract=contract,
        )


def test_focused_selection_does_not_shrink_configured_file_api_targets() -> None:
    target_graph = {
        "local_target_roles": {
            "production": ["manyforge_deployment"],
            "test_only": ["test_focused", "test_unselected"],
            "configured_only": ["configured_helper"],
            "external": ["GTest::gtest"],
        }
    }

    assert provenance._native_configured_file_api_target_names(
        target_graph,
        "focused",
    ) == {
        "manyforge_deployment",
        "test_focused",
        "test_unselected",
        "configured_helper",
    }

    configured = {
        "manyforge_deployment",
        "test_focused",
        "test_unselected",
        "configured_helper",
    }
    provenance._validate_native_configured_file_api_target_set(
        configured,
        target_graph,
        "focused",
    )
    for drifted in (
        configured - {"test_unselected"},
        configured | {"unexpected"},
    ):
        with pytest.raises(
            provenance.NativeBuildProvenanceError,
            match="configured target set drifted",
        ):
            provenance._validate_native_configured_file_api_target_set(
                drifted,
                target_graph,
                "focused",
            )

    overlapping_roles = copy.deepcopy(target_graph)
    overlapping_roles["local_target_roles"]["test_only"].append(
        "manyforge_deployment"
    )
    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="local roles overlap",
    ):
        provenance._native_configured_file_api_target_names(
            overlapping_roles,
            "focused",
        )

    malformed_roles = copy.deepcopy(target_graph)
    del malformed_roles["local_target_roles"]["external"]
    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="role keys drifted",
    ):
        provenance._native_configured_file_api_target_names(
            malformed_roles,
            "focused",
        )

    selected_material = {"manyforge_deployment", "test_focused"}
    provenance._validate_built_material_target_partition(
        selected_material,
        selected_material,
        "focused",
    )
    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="built material-target partition drifted",
    ):
        provenance._validate_built_material_target_partition(
            selected_material | {"test_unselected"},
            selected_material,
            "focused",
        )


def _configure_fixture_inventory(
    tmp_path: Path,
) -> tuple[Path, str, list[str], dict[str, Any], dict[str, Path]]:
    lane_root = tmp_path / "lane"
    expected_root = "/qualification-output/build/project/generated"
    physical_root = lane_root / "build/project/generated"
    files = {
        "model.srdf": _write(physical_root / "model.srdf", b"<robot/>\n"),
        "model.urdf": _write(physical_root / "model.urdf", b"<robot />\n"),
    }
    entries = [
        {
            "relative_path": relative,
            "kind": "file",
            "size": path.stat().st_size,
            "mode": f"{path.stat().st_mode & 0o777:04o}",
            "sha256": _sha256(path),
        }
        for relative, path in sorted(files.items())
    ]
    inventory = {
        "root": expected_root,
        "entry_count": len(entries),
        "total_file_bytes": sum(entry["size"] for entry in entries),
        "entries_sha256": verifier._canonical_json_sha256(entries),
        "entries": entries,
    }
    return lane_root, expected_root, sorted(files), inventory, files


def test_host_reauthenticates_configure_executed_fixture_inventory(
    tmp_path: Path,
) -> None:
    lane_root, expected_root, expected_paths, inventory, _ = (
        _configure_fixture_inventory(tmp_path)
    )

    entries = verifier._validate_configure_fixture_inventory(
        inventory,
        expected_root=expected_root,
        expected_paths=expected_paths,
        lane_root=lane_root,
        label="synthetic configure-executed fixture",
    )

    assert entries == inventory["entries"]


def test_host_rejects_changed_configure_executed_fixture_bytes(
    tmp_path: Path,
) -> None:
    lane_root, expected_root, expected_paths, inventory, files = (
        _configure_fixture_inventory(tmp_path)
    )
    files["model.urdf"].write_bytes(b"<changed />\n")

    with pytest.raises(verifier.ManifestError, match="retained bytes drifted"):
        verifier._validate_configure_fixture_inventory(
            inventory,
            expected_root=expected_root,
            expected_paths=expected_paths,
            lane_root=lane_root,
            label="synthetic configure-executed fixture",
        )


def _retained_tree_inventory(
    lane_root: Path,
    logical_root: str,
    files: dict[str, bytes],
) -> dict[str, Any]:
    physical_root = lane_root / Path(logical_root).relative_to(
        "/qualification-output"
    )
    entries: list[dict[str, Any]] = []
    for relative, value in sorted(files.items()):
        path = _write(physical_root / relative, value)
        entries.append(
            {
                "relative_path": relative,
                "kind": "file",
                "size": path.stat().st_size,
                "mode": f"{path.stat().st_mode & 0o777:04o}",
                "sha256": _sha256(path),
            }
        )
    return {
        "root": logical_root,
        "entry_count": len(entries),
        "total_file_bytes": sum(entry["size"] for entry in entries),
        "entries_sha256": verifier._canonical_json_sha256(entries),
        "entries": entries,
    }


def _configure_executed_replay_fixture(
    tmp_path: Path,
) -> tuple[dict[str, Any], dict[str, Any], Path, Path]:
    lane_root = tmp_path / "lane"
    repository_root = tmp_path / "repository"
    repository_root.mkdir()
    build_root = "/qualification-output/build/project"
    output_relative = "generated/model"
    replay_relative = f"i2-configure-fixture-replay/{output_relative}"
    paths = ["model.srdf", "model.urdf", "preparation-manifest.json"]
    bytes_by_path = {
        "model.srdf": b"<robot/>\n",
        "model.urdf": b"<robot />\n",
        "preparation-manifest.json": b'{"schema":"synthetic"}\n',
    }
    actual_inventory = _retained_tree_inventory(
        lane_root,
        f"{build_root}/{output_relative}",
        bytes_by_path,
    )
    replay_inventory = _retained_tree_inventory(
        lane_root,
        f"{build_root}/{replay_relative}",
        bytes_by_path,
    )
    fixture_contract = {
        "owner_project_id": "project",
        "output_root_relative_path": output_relative,
        "exact_output_relative_paths": paths,
        "cmake_repository_path": "project/CMakeLists.txt",
        "tool_repository_path": "project/tools/prepare.py",
        "urdf_repository_path": "project/model/source.urdf",
        "srdf_repository_path": "project/model/source.srdf",
        "resource_repository_paths": ["project/model/mesh.stl"],
    }
    expected_input_paths = sorted(
        f"/workspace/{path}"
        for path in (
            fixture_contract["cmake_repository_path"],
            fixture_contract["tool_repository_path"],
            fixture_contract["urdf_repository_path"],
            fixture_contract["srdf_repository_path"],
            *fixture_contract["resource_repository_paths"],
        )
    )
    required_inputs = [
        {
            "path": path,
            "sha256": hashlib.sha256(path.encode()).hexdigest(),
            "is_generated": False,
            "is_external": False,
            "is_cmake": False,
            "identity": {"kind": "git_tracked_source"},
        }
        for path in expected_input_paths
    ]
    python_contract = {
        "lexical_path": "/usr/bin/python3",
        "resolved_path": "/usr/bin/python3.11",
        "sha256": "c" * 64,
    }
    row = {
        "kind": "cmake_execute_process_replayed_fixture",
        "owner_project_id": "project",
        "output_root": f"{build_root}/{output_relative}",
        "actual_tree_sha256": actual_inventory["entries_sha256"],
        "replay_tree_sha256": replay_inventory["entries_sha256"],
        "required_inputs_sha256": verifier._canonical_json_sha256(
            required_inputs
        ),
        "argv_sha256": "d" * 64,
        "cross_profile_argv_sha256": "e" * 64,
        "cmake": "/workspace/project/CMakeLists.txt",
        "tool": "/workspace/project/tools/prepare.py",
        "interpreter": {
            "path": python_contract["lexical_path"],
            "resolved_path": python_contract["resolved_path"],
            "sha256": python_contract["sha256"],
        },
        "required_inputs": required_inputs,
        "actual_output_inventory": actual_inventory,
        "replay_output_inventory": replay_inventory,
        "replay_stdout_sha256": "f" * 64,
    }
    project = {
        "project_id": "project",
        "build_root": build_root,
        "build_testing": True,
        "configure_executed_fixtures": [row],
    }
    build_contract = {
        "configure_executed_fixture_contracts": [fixture_contract],
        "toolchain_contract": {"python": python_contract},
    }
    return project, build_contract, lane_root, repository_root


def test_host_joins_configure_executed_output_to_fresh_replay(tmp_path: Path) -> None:
    project, build_contract, lane_root, repository_root = (
        _configure_executed_replay_fixture(tmp_path)
    )

    verifier._validate_project_configure_fixtures(
        project,
        build_contract=build_contract,
        lane_root=lane_root,
        repository_root=repository_root,
        label="synthetic project",
    )


def test_host_rejects_self_consistent_but_different_configure_replay(
    tmp_path: Path,
) -> None:
    project, build_contract, lane_root, repository_root = (
        _configure_executed_replay_fixture(tmp_path)
    )
    row = project["configure_executed_fixtures"][0]
    replay_root = row["replay_output_inventory"]["root"]
    row["replay_output_inventory"] = _retained_tree_inventory(
        lane_root,
        replay_root,
        {
            "model.srdf": b"<robot/>\n",
            "model.urdf": b"<different />\n",
            "preparation-manifest.json": b'{"schema":"synthetic"}\n',
        },
    )
    row["replay_tree_sha256"] = row["replay_output_inventory"]["entries_sha256"]

    with pytest.raises(verifier.ManifestError, match="identity/replay join drifted"):
        verifier._validate_project_configure_fixtures(
            project,
            build_contract=build_contract,
            lane_root=lane_root,
            repository_root=repository_root,
            label="synthetic project",
        )


def _configure_executed_provenance_fixture(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    *,
    add_unexpected_replay_entry: bool = False,
) -> tuple[
    dict[str, Any],
    Path,
    Path,
    list[Path],
]:
    repository, output, roots = _roots(tmp_path)
    project_root = repository / "project"
    build_root = output / "build/project"
    resource_root = project_root / "resources"
    build_root.mkdir(parents=True)
    resource_root.mkdir(parents=True)
    cmake = _write(project_root / "CMakeLists.txt")
    tool = _write(project_root / "prepare.py")
    urdf = _write(project_root / "model.urdf", b"<robot />\n")
    srdf = _write(project_root / "model.srdf", b"<robot/>\n")
    resource = _write(resource_root / "mesh.stl", b"solid mesh\nendsolid\n")
    output_root = Path("generated/model")
    output_bytes = {
        "model.srdf": b"<robot/>\n",
        "model.urdf": b"<robot />\n",
        "preparation-manifest.json": b'{"schema":"synthetic"}\n',
    }
    for relative, value in output_bytes.items():
        _write(build_root / output_root / relative, value)
    contract = provenance._ConfigureExecutedFixtureContract(
        owner_project_id="project",
        required_build_testing=True,
        output_root=output_root,
        output_paths=tuple(Path(path) for path in sorted(output_bytes)),
        cmake=cmake.resolve(),
        tool=tool.resolve(),
        urdf=urdf.resolve(),
        srdf=srdf.resolve(),
        resource_root=resource_root.resolve(),
        resources=(resource.resolve(),),
        interpreter_toolchain_key="python",
        model_asset_id="synthetic.model",
        accepted_package_uri_prefix="package://synthetic/",
    )
    monkeypatch.setattr(
        provenance,
        "_configure_executed_fixture_contracts",
        lambda _value, *, repository_root: {
            ("project", output_root): contract
        },
    )
    replay_calls: list[Path] = []

    def replay(
        argv: list[str],
        cwd: Path,
        label: str,
        *,
        env: dict[str, str],
    ) -> str:
        assert cwd == build_root
        assert label == "project configure fixture exact replay"
        assert env["PYTHONDONTWRITEBYTECODE"] == "1"
        output_directory = Path(argv[argv.index("--output-directory") + 1])
        replay_calls.append(output_directory)
        for relative, value in output_bytes.items():
            _write(output_directory / relative, value)
        if add_unexpected_replay_entry:
            _write(output_directory.parents[1] / "unexpected.txt")
        return "prepared synthetic fixture\n"

    monkeypatch.setattr(provenance, "_run_text", replay)
    required = (cmake, tool, urdf, srdf, resource)
    configure_inputs = {
        path.resolve(): {
            "path": roots.logical(path.resolve()),
            "sha256": _sha256(path),
            "is_generated": False,
            "is_external": False,
            "is_cmake": False,
            "identity": {"kind": "git_tracked_source"},
        }
        for path in required
    }
    arguments = {
        "project_id": "project",
        "build_root": build_root.resolve(),
        "build_testing": True,
        "cache": {"_Python3_EXECUTABLE": ("FILEPATH", "/usr/bin/python3")},
        "configure_inputs": configure_inputs,
        "build_contract": {},
        "toolchain_contract": {
            "python": {
                "lexical_path": "/usr/bin/python3",
                "resolved_path": "/usr/bin/python3.11",
                "sha256": "a" * 64,
            }
        },
        "roots": roots,
    }
    return arguments, build_root, build_root / output_root, replay_calls


def test_configure_fixture_replay_is_fresh_twice_and_retained_only_finally(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    arguments, build_root, _actual_root, replay_calls = (
        _configure_executed_provenance_fixture(tmp_path, monkeypatch)
    )
    replay_parent = build_root / "i2-configure-fixture-replay"

    first = provenance._configure_executed_fixture_evidence(
        **arguments,
        retain_replay=False,
    )
    assert replay_calls == [replay_parent / "generated/model"]
    assert not replay_parent.exists()

    second = provenance._configure_executed_fixture_evidence(
        **arguments,
        retain_replay=True,
    )
    assert replay_calls == [
        replay_parent / "generated/model",
        replay_parent / "generated/model",
    ]
    assert first == second
    assert replay_parent.is_dir()


def test_configure_fixture_replay_rejects_preseed_without_deleting_it(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    arguments, build_root, _actual_root, replay_calls = (
        _configure_executed_provenance_fixture(tmp_path, monkeypatch)
    )
    replay_parent = build_root / "i2-configure-fixture-replay"
    marker = _write(replay_parent / "preseeded.txt")

    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="replay root was not fresh",
    ):
        provenance._configure_executed_fixture_evidence(
            **arguments,
            retain_replay=False,
        )

    assert marker.read_bytes() == b"synthetic\n"
    assert replay_calls == []


def test_configure_fixture_replay_rejects_dangling_symlink_preseed(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    arguments, build_root, _actual_root, replay_calls = (
        _configure_executed_provenance_fixture(tmp_path, monkeypatch)
    )
    replay_parent = build_root / "i2-configure-fixture-replay"
    replay_parent.symlink_to(
        build_root / "absent-replay-root",
        target_is_directory=True,
    )

    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="replay root was not fresh",
    ):
        provenance._configure_executed_fixture_evidence(
            **arguments,
            retain_replay=True,
        )

    assert replay_parent.is_symlink()
    assert replay_calls == []


def test_configure_fixture_replay_rejects_extra_state_before_cleanup(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    arguments, build_root, _actual_root, _replay_calls = (
        _configure_executed_provenance_fixture(
            tmp_path,
            monkeypatch,
            add_unexpected_replay_entry=True,
        )
    )
    replay_parent = build_root / "i2-configure-fixture-replay"

    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="replay root contains extra state",
    ):
        provenance._configure_executed_fixture_evidence(
            **arguments,
            retain_replay=False,
        )

    assert (replay_parent / "unexpected.txt").is_file()


def test_configure_fixture_final_replay_does_not_reuse_first_pass_bytes(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    arguments, _build_root, actual_root, replay_calls = (
        _configure_executed_provenance_fixture(tmp_path, monkeypatch)
    )
    provenance._configure_executed_fixture_evidence(
        **arguments,
        retain_replay=False,
    )
    (actual_root / "model.urdf").write_bytes(b"<changed />\n")

    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="replay bytes differ",
    ):
        provenance._configure_executed_fixture_evidence(
            **arguments,
            retain_replay=True,
        )

    assert len(replay_calls) == 2


def test_authenticated_replay_cleanup_refuses_changed_bytes(
    tmp_path: Path,
) -> None:
    _repository, output, roots = _roots(tmp_path)
    replay_parent = output / "build/i2-configure-fixture-replay"
    retained = _write(replay_parent / "generated/model.urdf", b"<robot />\n")
    inventory = provenance._exact_bounded_tree_inventory(
        replay_parent,
        roots=roots,
        label="synthetic replay root",
    )
    retained.write_bytes(b"<changed />\n")

    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="changed before authenticated cleanup",
    ):
        provenance._remove_authenticated_tree(
            replay_parent,
            inventory,
            roots=roots,
            label="synthetic replay root",
        )

    assert retained.read_bytes() == b"<changed />\n"


def test_authenticated_replay_cleanup_does_not_follow_replaced_directory(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    _repository, output, roots = _roots(tmp_path)
    replay_parent = output / "build/i2-configure-fixture-replay"
    _write(replay_parent / "generated/model.urdf", b"<robot />\n")
    external = output / "external-empty-directory"
    external.mkdir()
    inventory = provenance._exact_bounded_tree_inventory(
        replay_parent,
        roots=roots,
        label="synthetic replay root",
    )
    original_rmdir = provenance.os.rmdir
    replaced = False

    def replace_before_rmdir(
        path: str | Path,
        *,
        dir_fd: int | None = None,
    ) -> None:
        nonlocal replaced
        if path == "generated" and dir_fd is not None and not replaced:
            replaced = True
            original_rmdir(replay_parent / "generated")
            (replay_parent / "generated").symlink_to(
                external,
                target_is_directory=True,
            )
        original_rmdir(path, dir_fd=dir_fd)

    monkeypatch.setattr(provenance.os, "rmdir", replace_before_rmdir)
    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="authenticated cleanup failed",
    ):
        provenance._remove_authenticated_tree(
            replay_parent,
            inventory,
            roots=roots,
            label="synthetic replay root",
        )

    assert replaced is True
    assert external.is_dir()
    assert (replay_parent / "generated").is_symlink()


def test_authenticated_replay_cleanup_rejects_fifo_replacement_without_blocking(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    _repository, output, roots = _roots(tmp_path)
    replay_parent = output / "build/i2-configure-fixture-replay"
    retained = _write(replay_parent / "generated/model.urdf", b"<robot />\n")
    inventory = provenance._exact_bounded_tree_inventory(
        replay_parent,
        roots=roots,
        label="synthetic replay root",
    )
    original_open = provenance.os.open
    replaced = False

    def replace_before_open(
        path: str | Path,
        flags: int,
        mode: int = 0o777,
        *,
        dir_fd: int | None = None,
    ) -> int:
        nonlocal replaced
        if path == "model.urdf" and dir_fd is not None and not replaced:
            replaced = True
            retained.unlink()
            provenance.os.mkfifo(retained)
        return original_open(path, flags, mode, dir_fd=dir_fd)

    monkeypatch.setattr(provenance.os, "open", replace_before_open)
    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="file changed before authenticated cleanup",
    ):
        provenance._remove_authenticated_tree(
            replay_parent,
            inventory,
            roots=roots,
            label="synthetic replay root",
        )

    assert replaced is True
    assert retained.is_fifo()


def _bounded_tree_inventory(lane_root: Path, logical_root: str) -> dict[str, Any]:
    physical_root = lane_root / Path(logical_root).relative_to(
        "/qualification-output"
    )
    entries: list[dict[str, Any]] = []
    total_bytes = 0
    for path in sorted(physical_root.rglob("*")):
        relative = path.relative_to(physical_root).as_posix()
        if path.is_dir():
            entries.append({"relative_path": relative, "kind": "directory"})
            continue
        size = path.stat().st_size
        total_bytes += size
        entries.append(
            {
                "relative_path": relative,
                "kind": "file",
                "size": size,
                "mode": f"{path.stat().st_mode & 0o777:04o}",
                "sha256": _sha256(path),
            }
        )
    entries.sort(key=lambda row: (row["relative_path"], row["kind"]))
    return {
        "root": logical_root,
        "entry_count": len(entries),
        "total_file_bytes": total_bytes,
        "entries_sha256": verifier._canonical_json_sha256(entries),
        "entries": entries,
    }


def _synthetic_auxiliary_install_fixture(tmp_path: Path) -> dict[str, Any]:
    lane_root = tmp_path / "lane"
    repository_root = tmp_path / "repository"
    repository_root.mkdir()
    sanitizer_root = "/qualification-output/sanitizer"
    source_git_root = f"{sanitizer_root}/source"
    contract_projects: dict[str, Any] = {}
    projects: list[dict[str, Any]] = []
    qualified_inputs: list[dict[str, Any]] = []
    installed_copies: list[dict[str, Any]] = []
    target_proofs: list[dict[str, Any]] = []
    expected_install_roots: dict[str, str] = {}
    generated_rows: list[dict[str, Any]] = []

    for project_id in ("roboplan", "roboplan_example_models"):
        source_root = f"{source_git_root}/{project_id}"
        build_root = f"{sanitizer_root}/build/{project_id}"
        install_root = f"{sanitizer_root}/install/{project_id}"
        expected_install_roots[project_id] = install_root
        target_name = f"{project_id}_runtime"
        logical_target = f"{project_id}::{project_id}"
        source_relative = f"src/{target_name}.cpp"
        header_relative = f"include/{project_id}/api.hpp"
        library_build_relative = f"lib{project_id}.so"
        library_install_relative = f"lib/lib{project_id}.so"
        main_export_relative = (
            f"lib/cmake/{project_id}/{project_id}Targets.cmake"
        )
        config_export_relative = (
            f"lib/cmake/{project_id}/{project_id}Targets-relwithdebinfo.cmake"
        )

        source = _write(
            lane_root
            / Path(source_root).relative_to("/qualification-output")
            / source_relative,
            f"int {target_name}() {{ return 1; }}\n".encode(),
        )
        header = _write(
            lane_root
            / Path(source_root).relative_to("/qualification-output")
            / header_relative,
            b"#pragma once\n",
        )
        cmake_recipe = _write(
            lane_root
            / Path(source_root).relative_to("/qualification-output")
            / "CMakeLists.txt",
            b"cmake_minimum_required(VERSION 3.25)\n",
        )
        library = _write(
            lane_root
            / Path(build_root).relative_to("/qualification-output")
            / library_build_relative,
            f"synthetic-shared-library:{project_id}\n".encode(),
        )
        main_export = _write(
            lane_root
            / Path(build_root).relative_to("/qualification-output")
            / f"export/{project_id}Targets.cmake",
            f"add_library({logical_target} SHARED IMPORTED)\n".encode(),
        )
        config_export = _write(
            lane_root
            / Path(build_root).relative_to("/qualification-output")
            / f"export/{project_id}Targets-relwithdebinfo.cmake",
            (
                f"set_property(TARGET {logical_target} APPEND PROPERTY "
                "IMPORTED_CONFIGURATIONS RELWITHDEBINFO)\n"
            ).encode(),
        )

        installed_header = _write(
            lane_root
            / Path(install_root).relative_to("/qualification-output")
            / header_relative,
            header.read_bytes(),
        )
        installed_main = _write(
            lane_root
            / Path(install_root).relative_to("/qualification-output")
            / main_export_relative,
            main_export.read_bytes(),
        )
        installed_config = _write(
            lane_root
            / Path(install_root).relative_to("/qualification-output")
            / config_export_relative,
            config_export.read_bytes(),
        )
        installed_library = _write(
            lane_root
            / Path(install_root).relative_to("/qualification-output")
            / library_install_relative,
            library.read_bytes(),
        )

        manifest_rows = [
            header_relative,
            main_export_relative,
            config_export_relative,
            library_install_relative,
            library_install_relative,
        ]
        manifest_path = f"{build_root}/install_manifest.txt"
        physical_manifest = _write(
            lane_root / Path(manifest_path).relative_to("/qualification-output"),
            "".join(f"{install_root}/{path}\n" for path in manifest_rows).encode(),
        )
        repeated_rows = [{"path": library_install_relative, "count": 2}]
        manifest_contract = {
            "row_count": len(manifest_rows),
            "distinct_path_count": len(set(manifest_rows)),
            "relative_rows_sha256": verifier._canonical_json_sha256(
                manifest_rows
            ),
            "repeated_relative_paths": repeated_rows,
        }
        manifest = {
            "path": manifest_path,
            "sha256": _sha256(physical_manifest),
            **manifest_contract,
        }

        generated_contracts = [
            {
                "install_relative_path": main_export_relative,
                "template_source_relative_path": None,
                "producer_kind": "install_export_main",
                "build_relative_path": f"export/{project_id}Targets.cmake",
                "cmake_source_relative_path": "CMakeLists.txt",
                "required_logical_targets": [logical_target],
                "physical": main_export,
                "installed": installed_main,
            },
            {
                "install_relative_path": config_export_relative,
                "template_source_relative_path": None,
                "producer_kind": "install_export_configuration",
                "build_relative_path": (
                    f"export/{project_id}Targets-relwithdebinfo.cmake"
                ),
                "cmake_source_relative_path": "CMakeLists.txt",
                "required_logical_targets": [logical_target],
                "physical": config_export,
                "installed": installed_config,
            },
        ]
        contract_projects[project_id] = {
            "manifest": manifest_contract,
            "source_copy_prefixes": [
                {"install_prefix": "include", "source_prefix": "include"}
            ],
            "expected_source_copy_count": 1,
            "generated_files": [
                {
                    key: value
                    for key, value in row.items()
                    if key not in {"physical", "installed"}
                }
                for row in generated_contracts
            ],
            "library_files": [
                {
                    "target": target_name,
                    "type": "SHARED_LIBRARY",
                    "install_relative_path": library_install_relative,
                    "build_artifact_relative_path": library_build_relative,
                    "required_sources": [source_relative],
                }
            ],
        }

        source_identity = {
            "git_root": source_git_root,
            "path": f"{project_id}/{header_relative}",
            "mode": "100644",
            "blob": _git_blob(header),
        }
        source_owner = f"{source_root}/{header_relative}"
        qualified_inputs.append(
            {
                "path": f"{install_root}/{header_relative}",
                "resolved_path": f"{install_root}/{header_relative}",
                "symlink_chain": [],
                "sha256": _sha256(installed_header),
                "project_id": project_id,
                "install_relative_path": header_relative,
                "install_manifest": manifest,
                "producer": {
                    "kind": "git_tracked_install_source",
                    "path": source_owner,
                    "sha256": _sha256(header),
                    "source_identity": source_identity,
                },
            }
        )
        non_library_projection = [
            {
                "install_relative_path": header_relative,
                "kind": "git_tracked_install_source",
                "owner": source_owner,
            }
        ]
        owner_projection: dict[str, dict[str, str]] = {
            header_relative: {
                "path": header_relative,
                "kind": "git_tracked_install_source",
                "owner": source_owner,
            }
        }
        for generated_contract in generated_contracts:
            generated_path = (
                f"{build_root}/{generated_contract['build_relative_path']}"
            )
            installed_path = (
                f"{install_root}/{generated_contract['install_relative_path']}"
            )
            producer = {
                "kind": "cmake_generated_install_source",
                "producer_kind": generated_contract["producer_kind"],
                "path": generated_path,
                "sha256": _sha256(generated_contract["physical"]),
                "cmake_recipe": {
                    "path": f"{source_root}/CMakeLists.txt",
                    "sha256": _sha256(cmake_recipe),
                    "source_identity": {
                        "git_root": source_git_root,
                        "path": f"{project_id}/CMakeLists.txt",
                        "mode": "100644",
                        "blob": _git_blob(cmake_recipe),
                    },
                },
                "template": None,
                "required_logical_targets": [logical_target],
            }
            qualified_inputs.append(
                {
                    "path": installed_path,
                    "resolved_path": installed_path,
                    "symlink_chain": [],
                    "sha256": _sha256(generated_contract["installed"]),
                    "project_id": project_id,
                    "install_relative_path": generated_contract[
                        "install_relative_path"
                    ],
                    "install_manifest": manifest,
                    "producer": producer,
                }
            )
            projection_row = {
                "install_relative_path": generated_contract[
                    "install_relative_path"
                ],
                "kind": "cmake_generated_install_source",
                "owner": generated_path,
            }
            non_library_projection.append(projection_row)
            owner_projection[generated_contract["install_relative_path"]] = {
                "path": generated_contract["install_relative_path"],
                "kind": "cmake_generated_install_source",
                "owner": generated_path,
            }
            generated_rows.append(
                {
                    "project_id": project_id,
                    "producer_kind": generated_contract["producer_kind"],
                    "build_path": generated_path,
                }
            )

        owner_projection[library_install_relative] = {
            "path": library_install_relative,
            "kind": "target_artifact",
            "owner": target_name,
        }
        inventory = _bounded_tree_inventory(lane_root, install_root)
        projects.append(
            {
                "project_id": project_id,
                "install_root": install_root,
                "manifest": manifest,
                "install_tree_inventory": inventory,
                "owner_counts": {
                    "git_source_copy": 1,
                    "cmake_generated": 2,
                    "target_artifact": 1,
                },
                "non_library_owner_projection_sha256": (
                    verifier._canonical_json_sha256(
                        sorted(
                            non_library_projection,
                            key=lambda row: row["install_relative_path"],
                        )
                    )
                ),
                "owner_partition_sha256": verifier._canonical_json_sha256(
                    [owner_projection[path] for path in sorted(owner_projection)]
                ),
            }
        )
        target_proofs.append(
            {
                "project_id": project_id,
                "target": target_name,
                "target_type": "SHARED_LIBRARY",
                "contract_artifact": library_build_relative,
                "artifact": {
                    "path": f"{build_root}/{library_build_relative}",
                    "resolved_path": f"{build_root}/{library_build_relative}",
                    "sha256": _sha256(library),
                },
                "file_api_sources": [
                    {
                        "path": f"{source_root}/{source_relative}",
                        "sha256": _sha256(source),
                        "cross_profile_sha256": _sha256(source),
                    }
                ],
            }
        )
        installed_copies.append(
            {
                "project_id": project_id,
                "path": f"{install_root}/{library_install_relative}",
                "resolved_path": f"{install_root}/{library_install_relative}",
                "symlink_chain": [],
                "sha256": _sha256(installed_library),
                "install_relative_path": library_install_relative,
                "producer_project": project_id,
                "producer_target": target_name,
                "producer_artifact": f"{build_root}/{library_build_relative}",
                "install_manifest": manifest,
            }
        )

    return {
        "projects": projects,
        "qualified_inputs": sorted(
            qualified_inputs, key=lambda row: (row["project_id"], row["path"])
        ),
        "installed_copies": installed_copies,
        "target_proofs": target_proofs,
        "lane_root": lane_root,
        "repository_root": repository_root,
        "build_contract": {
            "auxiliary_install_contract": {"projects": contract_projects}
        },
        "expected_install_roots": expected_install_roots,
        "generated_rows": generated_rows,
    }


def _validate_synthetic_auxiliary_install_fixture(fixture: dict[str, Any]) -> None:
    verifier._validate_auxiliary_install_projects(
        fixture["projects"],
        qualified_inputs_value=fixture["qualified_inputs"],
        installed_copies_value=fixture["installed_copies"],
        target_proofs_value=fixture["target_proofs"],
        lane_root=fixture["lane_root"],
        build_contract=fixture["build_contract"],
        expected_install_roots=fixture["expected_install_roots"],
        label="synthetic auxiliary installs",
    )
    verifier._authenticate_provenance_files(
        fixture,
        lane_root=fixture["lane_root"],
        repository_root=fixture["repository_root"],
        label="synthetic auxiliary install provenance",
    )


def test_host_joins_auxiliary_manifest_tree_sources_exports_and_library_copies(
    tmp_path: Path,
) -> None:
    _validate_synthetic_auxiliary_install_fixture(
        _synthetic_auxiliary_install_fixture(tmp_path)
    )


def test_host_rejects_auxiliary_generated_export_target_drift(
    tmp_path: Path,
) -> None:
    fixture = _synthetic_auxiliary_install_fixture(tmp_path)
    generated = next(
        row
        for row in fixture["generated_rows"]
        if row["producer_kind"] == "install_export_main"
    )
    physical = fixture["lane_root"] / Path(generated["build_path"]).relative_to(
        "/qualification-output"
    )
    physical.write_text(
        "add_library(forged::target SHARED IMPORTED)\n", encoding="utf-8"
    )
    owner = next(
        row
        for row in fixture["qualified_inputs"]
        if row["project_id"] == generated["project_id"]
        and row["producer"].get("producer_kind") == "install_export_main"
    )
    installed = fixture["lane_root"] / Path(owner["path"]).relative_to(
        "/qualification-output"
    )
    installed.write_bytes(physical.read_bytes())
    owner["sha256"] = _sha256(installed)
    owner["producer"]["sha256"] = _sha256(physical)
    project = next(
        row
        for row in fixture["projects"]
        if row["project_id"] == generated["project_id"]
    )
    project["install_tree_inventory"] = _bounded_tree_inventory(
        fixture["lane_root"], project["install_root"]
    )

    verifier._authenticate_provenance_files(
        fixture,
        lane_root=fixture["lane_root"],
        repository_root=fixture["repository_root"],
        label="self-consistent forged auxiliary install provenance",
    )

    with pytest.raises(verifier.ManifestError, match="main-export targets drifted"):
        verifier._validate_auxiliary_install_projects(
            fixture["projects"],
            qualified_inputs_value=fixture["qualified_inputs"],
            installed_copies_value=fixture["installed_copies"],
            target_proofs_value=fixture["target_proofs"],
            lane_root=fixture["lane_root"],
            build_contract=fixture["build_contract"],
            expected_install_roots=fixture["expected_install_roots"],
            label="synthetic auxiliary installs",
        )


def _embedded_build_evidence_header(
    *, profile: str, configuration_digest: str, sentinel: int = 7
) -> bytes:
    return (
        "#pragma once\n"
        "#include <string_view>\n\n"
        "inline constexpr std::string_view kBuildProfile =\n"
        f'    "{profile}";\n'
        "inline constexpr std::string_view kBuildConfigurationDigest =\n"
        f'    "{configuration_digest}";\n'
        f"inline constexpr int kStableSentinel = {sentinel};\n"
    ).encode()


def _build_qualified_profile_header(
    *, profile: str, configuration_digest: str, sentinel: str = "stable"
) -> bytes:
    manifest = json.dumps(
        {
            "build_configuration_digest": configuration_digest,
            "build_profile": profile,
            "sentinel": sentinel,
        },
        sort_keys=True,
        separators=(",", ":"),
    ).encode()
    byte_array = ", ".join(f"0x{value:02x}U" for value in manifest)
    digest = hashlib.sha256(manifest).hexdigest()
    return (
        "#pragma once\n"
        "#include <array>\n"
        "#include <cstdint>\n"
        "#include <string_view>\n\n"
        f"inline constexpr std::array<std::uint8_t, {len(manifest)}U> "
        "kCompilerBuildManifestBytes{{\n"
        f"    {byte_array}\n"
        "}};\n\n"
        "inline constexpr std::string_view kCompilerBuildManifestDigest = "
        f'"sha256:{digest}";\n'
    ).encode()


def test_cross_profile_generated_headers_normalize_only_sanctioned_facts(
    tmp_path: Path,
) -> None:
    release_embedded = _write(
        tmp_path / "release/embedded_build_evidence.hpp",
        _embedded_build_evidence_header(
            profile="Release", configuration_digest=f"sha256:{'1' * 64}"
        ),
    )
    relwithdebinfo_embedded = _write(
        tmp_path / "relwithdebinfo/embedded_build_evidence.hpp",
        _embedded_build_evidence_header(
            profile="RelWithDebInfo", configuration_digest=f"sha256:{'2' * 64}"
        ),
    )
    mutated_embedded = _write(
        tmp_path / "mutated/embedded_build_evidence.hpp",
        _embedded_build_evidence_header(
            profile="RelWithDebInfo",
            configuration_digest=f"sha256:{'2' * 64}",
            sentinel=8,
        ),
    )
    release_profile = _write(
        tmp_path / "release/build_qualified_admission_profile_generated.hpp",
        _build_qualified_profile_header(
            profile="Release", configuration_digest=f"sha256:{'3' * 64}"
        ),
    )
    relwithdebinfo_profile = _write(
        tmp_path
        / "relwithdebinfo/build_qualified_admission_profile_generated.hpp",
        _build_qualified_profile_header(
            profile="RelWithDebInfo", configuration_digest=f"sha256:{'4' * 64}"
        ),
    )
    mutated_profile = _write(
        tmp_path / "mutated/build_qualified_admission_profile_generated.hpp",
        _build_qualified_profile_header(
            profile="RelWithDebInfo",
            configuration_digest=f"sha256:{'4' * 64}",
            sentinel="changed",
        ),
    )

    embedded_release_digest = provenance.compile_dependency_cross_profile_sha256(
        release_embedded, "configure_generated_input"
    )
    assert embedded_release_digest == (
        provenance.compile_dependency_cross_profile_sha256(
            relwithdebinfo_embedded, "configure_generated_input"
        )
    )
    assert embedded_release_digest != (
        provenance.compile_dependency_cross_profile_sha256(
            mutated_embedded, "configure_generated_input"
        )
    )

    profile_release_digest = provenance.compile_dependency_cross_profile_sha256(
        release_profile, "generated_build_input"
    )
    assert profile_release_digest == (
        provenance.compile_dependency_cross_profile_sha256(
            relwithdebinfo_profile, "generated_build_input"
        )
    )
    assert profile_release_digest != (
        provenance.compile_dependency_cross_profile_sha256(
            mutated_profile, "generated_build_input"
        )
    )


@pytest.mark.parametrize(
    "argv",
    [
        ["/usr/bin/clang++", "-c", "source.cpp", "-o", "source.o"],
        ["/usr/bin/ccache", "/usr/bin/c++", "-c", "source.cpp", "-o", "source.o"],
        ["CXXFLAGS=-O2", "/usr/bin/c++", "-c", "source.cpp", "-o", "source.o"],
    ],
    ids=["alternate-compiler", "wrapper", "environment-prefix"],
)
def test_compile_database_rejects_alternate_compiler_wrapper_or_prefix(
    tmp_path: Path,
    argv: list[str],
) -> None:
    repository, output, roots = _roots(tmp_path)
    source_root = repository / "project"
    source = _write(source_root / "source.cpp")
    build = output / "build"
    build.mkdir()
    absolute_argv = [
        str(source)
        if token == "source.cpp"
        else str(build / token)
        if token == "source.o"
        else token
        for token in argv
    ]
    (build / "compile_commands.json").write_text(
        json.dumps(
            [
                {
                    "directory": str(build),
                    "file": str(source),
                    "output": str(build / "source.o"),
                    "arguments": absolute_argv,
                }
            ]
        ),
        encoding="utf-8",
    )

    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="exact cache compiler.*wrapper|exact cache compiler.*launcher",
    ):
        provenance._load_compile_commands(
            "synthetic",
            source_root,
            build,
            [source_root],
            "/usr/bin/c++",
            roots,
            provenance._ResponseCollector(roots),
            "Release",
            COMPILE_MODES,
        )


@pytest.mark.parametrize(
    "command",
    [
        "/usr/bin/c++ -o app app.o; /usr/bin/cp app copy",
        "/usr/bin/c++ -o app app.o | /usr/bin/tee digest",
        "/usr/bin/c++ -o app app.o > app.log",
        "/usr/bin/c++ -o app $(/usr/bin/printf evil)",
        "/usr/bin/c++ -o app `printf evil`",
        "/usr/bin/c++ -I$HOME/ambient-headers -o app app.o",
        "/usr/bin/c++ -o app *.o",
        "/usr/bin/c++ -o app app[0].o",
        "/usr/bin/c++ -o app ~/ambient.o",
    ],
    ids=[
        "semicolon",
        "pipe",
        "redirection",
        "subshell",
        "backticks",
        "ambient-variable",
        "wildcard",
        "bracket-glob",
        "home-expansion",
    ],
)
def test_ninja_shell_control_and_evaluation_are_fail_closed(command: str) -> None:
    with pytest.raises(provenance.NativeBuildProvenanceError, match="unsupported"):
        provenance._split_shell_line(command, "synthetic Ninja command")


def _gtest_discovery_fixture(
    tmp_path: Path,
    *,
    lane_id: str = "synthetic",
    build_relative: Path = Path("build"),
    extra_args: str = "",
    properties: str = "TIMEOUT;180",
) -> tuple[
    str,
    provenance._Build,
    provenance._Roots,
    Path,
]:
    repository, output, roots = _roots(tmp_path)
    build_root = output / build_relative
    build_root.mkdir(parents=True)
    target_name = "test_synthetic"
    executable = _write(
        build_root / target_name,
        b"\x7fELFsynthetic\n",
        executable=True,
    )
    object_path = _write(build_root / "synthetic.o", b"\x7fELFsynthetic\n")
    _write(build_root / f"{target_name}[1]_tests.cmake")
    reply = _write(build_root / "target-reply.json", b"{}\n")
    build = provenance._Build(
        lane_id=lane_id,
        project_id="synthetic",
        root=build_root,
        source_root=repository,
        compiler_path="/usr/bin/c++",
        archiver_path="/usr/bin/ar",
        targets={
            target_name: provenance._TargetReply(
                name=target_name,
                target_type="EXECUTABLE",
                artifacts=(executable,),
                sources=(),
                reply_path=reply,
                reply_sha256=_sha256(reply),
            )
        },
        compile_commands={},
        configure_inputs={},
        configure_generated_contracts={},
        qualified_install_inputs={},
        evidence={},
    )
    definitions = {
        "TEST_TARGET": target_name,
        "TEST_EXECUTABLE": roots.logical(executable),
        "TEST_EXECUTOR": "",
        "TEST_WORKING_DIR": roots.logical(build_root),
        "TEST_EXTRA_ARGS": extra_args,
        "TEST_PROPERTIES": properties,
        "TEST_PREFIX": "",
        "TEST_SUFFIX": "",
        "TEST_FILTER": "",
        "NO_PRETTY_TYPES": "FALSE",
        "NO_PRETTY_VALUES": "FALSE",
        "TEST_LIST": f"{target_name}_TESTS",
        "CTEST_FILE": roots.logical(build_root / f"{target_name}[1]_tests.cmake"),
        "TEST_DISCOVERY_TIMEOUT": "5",
        "TEST_XML_OUTPUT_DIR": "",
    }
    discovery: list[str] = ["/usr/bin/cmake"]
    for key in provenance._GTEST_DISCOVERY_DEFINITIONS:
        assignment = f"{key}={definitions[key]}"
        if key == "TEST_PROPERTIES" or (key == "TEST_EXTRA_ARGS" and extra_args):
            assignment = f'"{assignment}"'
        discovery.extend(("-D", assignment))
    discovery.extend(("-P", provenance._GTEST_DISCOVERY_MODULE.as_posix()))
    line = (
        f"/usr/bin/c++ -o {roots.logical(executable)} {roots.logical(object_path)}"
        f" && cd {roots.logical(build_root)} && {' '.join(discovery)}"
    )
    return line, build, roots, executable


@pytest.mark.parametrize(
    ("lane_id", "build_relative", "properties"),
    [
        (
            "synthetic",
            Path("build"),
            "TIMEOUT;180",
        ),
        (
            "roboplan_release",
            Path("build/roboplan-release/roboplan"),
            "ENVIRONMENT;LD_LIBRARY_PATH="
            f"{provenance._GTEST_DISCOVERY_ROBOPLAN_LD_LIBRARY_PATH};TIMEOUT;180",
        ),
        (
            "roboplan_asan_ubsan",
            Path("build/roboplan-asan/build/manyforge_roboplan"),
            "ENVIRONMENT;LD_LIBRARY_PATH="
            "/qualification-output/build/roboplan-asan/install/roboplan/lib:"
            "/qualification-output/build/roboplan-asan/install/"
            "roboplan_example_models/lib:"
            "/usr/local/lib/python3.12/site-packages/cmeel.prefix/lib:"
            "/usr/local/lib:"
            "/usr/local/lib/python3.12/site-packages/cmeel.prefix/lib:"
            "/usr/local/lib;TIMEOUT;180",
        ),
    ],
    ids=["deployment", "roboplan-release", "roboplan-sanitizer"],
)
def test_ninja_commands_authenticate_bounded_gtest_discovery_suffix(
    tmp_path: Path,
    lane_id: str,
    build_relative: Path,
    properties: str,
) -> None:
    line, build, roots, executable = _gtest_discovery_fixture(
        tmp_path,
        lane_id=lane_id,
        build_relative=build_relative,
        properties=properties,
    )

    commands = provenance._parse_ninja_commands(
        line + "\n",
        build,
        roots,
        provenance._ResponseCollector(roots),
        "synthetic target",
    )

    assert set(commands) == {executable}
    assert commands[executable].kind == "link"


@pytest.mark.parametrize(
    ("old", "new"),
    [
        ("TEST_EXECUTOR=", "TEST_EXECUTOR=/usr/bin/env"),
        ("[1]_tests.cmake", "[2]_tests.cmake"),
        ("TEST_DISCOVERY_TIMEOUT=5", "TEST_DISCOVERY_TIMEOUT=6"),
        ("TEST_EXTRA_ARGS=", '"TEST_EXTRA_ARGS=--gtest_filter=*"'),
        ("GoogleTestAddTests.cmake", "HostileAddTests.cmake"),
        (
            "GoogleTestAddTests.cmake",
            "GoogleTestAddTests.cmake && /usr/bin/true",
        ),
        (
            "GoogleTestAddTests.cmake",
            "GoogleTestAddTests.cmake $(/usr/bin/id)",
        ),
        ("-D TEST_PREFIX=", "-D EXTRA_FIELD=1 -D TEST_PREFIX="),
        ("-D TEST_TARGET=test_synthetic", "-D TEST_LIST=test_synthetic_TESTS"),
        (
            "TEST_EXECUTABLE=/qualification-output/build/test_synthetic",
            "TEST_EXECUTABLE=/qualification-output/test_synthetic",
        ),
        (
            "TEST_EXECUTABLE=/qualification-output/build/test_synthetic",
            "TEST_EXECUTABLE=test_synthetic",
        ),
        (
            "TEST_EXECUTABLE=/qualification-output/build/test_synthetic",
            "TEST_EXECUTABLE=/qualification-output/build/./test_synthetic",
        ),
        (
            "TEST_WORKING_DIR=/qualification-output/build",
            "TEST_WORKING_DIR=/qualification-output",
        ),
        (
            "TEST_WORKING_DIR=/qualification-output/build",
            "TEST_WORKING_DIR=.",
        ),
        (
            "CTEST_FILE=/qualification-output/build/test_synthetic[1]_tests.cmake",
            "CTEST_FILE=/qualification-output/test_synthetic[1]_tests.cmake",
        ),
        (
            "CTEST_FILE=/qualification-output/build/test_synthetic[1]_tests.cmake",
            "CTEST_FILE=test_synthetic[1]_tests.cmake",
        ),
    ],
    ids=[
        "executor",
        "ordinal",
        "discovery-timeout",
        "extra-arguments",
        "module",
        "trailing-control",
        "trailing-substitution",
        "extra-definition",
        "reordered-definition",
        "executable-escape",
        "executable-relative",
        "executable-noncanonical",
        "working-directory-escape",
        "working-directory-relative",
        "ctest-file-escape",
        "ctest-file-relative",
    ],
)
def test_ninja_commands_reject_mutated_gtest_discovery_suffix(
    tmp_path: Path,
    old: str,
    new: str,
) -> None:
    line, build, roots, _executable = _gtest_discovery_fixture(tmp_path)
    assert old in line

    with pytest.raises(provenance.NativeBuildProvenanceError):
        provenance._parse_ninja_commands(
            line.replace(old, new, 1) + "\n",
            build,
            roots,
            provenance._ResponseCollector(roots),
            "synthetic target",
        )


def test_ninja_commands_reject_gtest_discovery_glob_collision(
    tmp_path: Path,
) -> None:
    line, build, roots, _executable = _gtest_discovery_fixture(tmp_path)
    _write(
        build.root
        / "CTEST_FILE=/qualification-output/build/test_synthetic1_tests.cmake"
    )

    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="shell glob match",
    ):
        provenance._parse_ninja_commands(
            line + "\n",
            build,
            roots,
            provenance._ResponseCollector(roots),
            "synthetic target",
        )


def test_ninja_commands_reject_physical_gtest_discovery_path_spelling(
    tmp_path: Path,
) -> None:
    line, build, roots, executable = _gtest_discovery_fixture(tmp_path)
    logical_assignment = f"TEST_EXECUTABLE={roots.logical(executable)}"
    physical_assignment = f"TEST_EXECUTABLE={executable.as_posix()}"
    assert logical_assignment in line

    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="exact GoogleTest executable spelling",
    ):
        provenance._parse_ninja_commands(
            line.replace(logical_assignment, physical_assignment, 1) + "\n",
            build,
            roots,
            provenance._ResponseCollector(roots),
            "synthetic target",
        )


def test_ninja_commands_require_gtest_discovery_metacharacter_quoting(
    tmp_path: Path,
) -> None:
    line, build, roots, _executable = _gtest_discovery_fixture(tmp_path)
    quoted = '"TEST_PROPERTIES=TIMEOUT;180"'
    unquoted = "TEST_PROPERTIES=TIMEOUT;180"
    assert quoted in line

    with pytest.raises(provenance.NativeBuildProvenanceError):
        provenance._parse_ninja_commands(
            line.replace(quoted, unquoted, 1) + "\n",
            build,
            roots,
            provenance._ResponseCollector(roots),
            "synthetic target",
        )


def test_ninja_commands_reject_stale_roboplan_discovery_property(
    tmp_path: Path,
) -> None:
    properties = (
        "ENVIRONMENT;LD_LIBRARY_PATH="
        f"{provenance._GTEST_DISCOVERY_ROBOPLAN_LD_LIBRARY_PATH};"
        "TIMEOUT;180;DISCOVERY_EXTRA_ARGS;--gtest_filter=*"
    )
    line, build, roots, _executable = _gtest_discovery_fixture(
        tmp_path,
        lane_id="roboplan_release",
        build_relative=Path("build/roboplan-release/roboplan"),
        properties=properties,
    )

    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="unsupported GoogleTest discovery properties",
    ):
        provenance._parse_ninja_commands(
            line + "\n",
            build,
            roots,
            provenance._ResponseCollector(roots),
            "synthetic target",
        )


def test_ninja_commands_reject_missing_roboplan_discovery_environment(
    tmp_path: Path,
) -> None:
    line, build, roots, _executable = _gtest_discovery_fixture(
        tmp_path,
        lane_id="roboplan_release",
        build_relative=Path("build/roboplan-release/roboplan"),
    )

    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="lacks the pinned GoogleTest discovery environment",
    ):
        provenance._parse_ninja_commands(
            line + "\n",
            build,
            roots,
            provenance._ResponseCollector(roots),
            "synthetic target",
        )


def test_ninja_commands_join_gtest_discovery_to_same_link_output(
    tmp_path: Path,
) -> None:
    line, build, roots, executable = _gtest_discovery_fixture(tmp_path)
    other_executable = _write(
        build.root / "test_other",
        b"\x7fELFsynthetic\n",
        executable=True,
    )
    other_reply = _write(build.root / "other-target-reply.json", b"{}\n")
    build.targets["test_other"] = provenance._TargetReply(
        name="test_other",
        target_type="EXECUTABLE",
        artifacts=(other_executable,),
        sources=(),
        reply_path=other_reply,
        reply_sha256=_sha256(other_reply),
    )
    link_prefix = f"/usr/bin/c++ -o {roots.logical(executable)}"
    mismatched_prefix = f"/usr/bin/c++ -o {roots.logical(other_executable)}"
    assert link_prefix in line

    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="differs from its one link output",
    ):
        provenance._parse_ninja_commands(
            line.replace(link_prefix, mismatched_prefix, 1) + "\n",
            build,
            roots,
            provenance._ResponseCollector(roots),
            "synthetic target",
        )


@pytest.mark.parametrize(
    "extra_argv",
    [
        ["-fplugin=/qualification-output/plugin.so"],
        ["-flto=auto", "-fno-fat-lto-objects"],
        ["-specs=/qualification-output/compiler.specs"],
        ["-B/qualification-output/alternate-tools"],
        ["--sysroot=/qualification-output/fake-sysroot"],
        ["-wrapper", "/qualification-output/compiler-wrapper"],
        ["-Xclang", "-load", "-Xclang", "/qualification-output/plugin.so"],
    ],
    ids=[
        "plugin",
        "lto",
        "specs",
        "tool-prefix",
        "sysroot",
        "wrapper",
        "clang-plugin",
    ],
)
def test_compile_database_rejects_hidden_tool_or_plugin_controls(
    tmp_path: Path,
    extra_argv: list[str],
) -> None:
    repository, output, roots = _roots(tmp_path)
    source_root = repository / "project"
    source = _write(source_root / "source.cpp")
    build = output / "build"
    build.mkdir()
    object_path = build / "source.o"
    (build / "compile_commands.json").write_text(
        json.dumps(
            [
                {
                    "directory": str(build),
                    "file": str(source),
                    "output": str(object_path),
                    "arguments": [
                        "/usr/bin/c++",
                        *extra_argv,
                        "-o",
                        str(object_path),
                        "-c",
                        str(source),
                    ],
                }
            ]
        ),
        encoding="utf-8",
    )

    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="unsupported|forbidden|unqualified",
    ):
        provenance._load_compile_commands(
            "synthetic",
            source_root,
            build,
            [source_root],
            "/usr/bin/c++",
            roots,
            provenance._ResponseCollector(roots),
            "Release",
            COMPILE_MODES,
        )


def test_linker_script_is_rejected_without_recursive_input_proof(
    tmp_path: Path,
) -> None:
    repository, output, roots = _roots(tmp_path)
    build_root = output / "build"
    build_root.mkdir()
    script = _write(build_root / "linker.ld", b"INPUT(hidden.a)\n")
    object_path = _write(build_root / "source.o", b"object")
    build = provenance._Build(
        lane_id="synthetic",
        project_id="synthetic",
        root=build_root,
        source_root=repository,
        compiler_path="/usr/bin/c++",
        archiver_path="/usr/bin/ar",
        targets={},
        compile_commands={},
        configure_inputs={},
        configure_generated_contracts={},
        qualified_install_inputs={},
        evidence={},
    )

    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="linker script|nested linker input|unsupported",
    ):
        provenance._decode_link_inputs(
            [
                "/usr/bin/c++",
                "-o",
                str(build_root / "app"),
                f"-Wl,-T,{script}",
                str(object_path),
            ],
            cwd=build_root,
            build=build,
            roots=roots,
            label="synthetic link",
        )


@pytest.mark.parametrize("tool", ["cp", "objcopy", "patchelf", "strip"])
def test_ninja_commands_reject_post_link_mutation_tools(
    tmp_path: Path,
    tool: str,
) -> None:
    repository, output, roots = _roots(tmp_path)
    build_root = output / "build"
    build_root.mkdir()
    build = provenance._Build(
        lane_id="synthetic",
        project_id="synthetic",
        root=build_root,
        source_root=repository,
        compiler_path="/usr/bin/c++",
        archiver_path="/usr/bin/ar",
        targets={},
        compile_commands={},
        configure_inputs={},
        configure_generated_contracts={},
        qualified_install_inputs={},
        evidence={},
    )

    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="forbidden post-link mutation tool",
    ):
        provenance._parse_ninja_commands(
            f"/usr/bin/{tool} input output\n",
            build,
            roots,
            provenance._ResponseCollector(roots),
            "synthetic target",
        )


def test_response_file_change_during_provenance_inspection_is_rejected(
    tmp_path: Path,
) -> None:
    _, output, roots = _roots(tmp_path)
    build = output / "build"
    build.mkdir()
    response = _write(build / "compile.rsp", b"-DFROZEN=1\n")
    collector = provenance._ResponseCollector(roots)

    assert collector.expand(["@compile.rsp"], build, "compile") == ("-DFROZEN=1",)
    response.write_bytes(b"-DFROZEN=0\n")

    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="changed during provenance inspection",
    ):
        collector.expand(["@compile.rsp"], build, "compile")


@pytest.mark.parametrize(
    "tail",
    [
        ["qc", "libproduct.a", "--plugin", "evil.so", "product.o"],
        ["qc", "libproduct.a", "ignored.txt", "product.o"],
        ["qcT", "libproduct.a", "product.o"],
    ],
    ids=["plugin", "ignored-positional", "thin-archive"],
)
def test_archive_command_rejects_plugins_ignored_inputs_and_noncreation_modes(
    tmp_path: Path,
    tail: list[str],
) -> None:
    _, output, roots = _roots(tmp_path)
    build = output / "build"
    build.mkdir()

    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="unsupported|plugin|ignored positional",
    ):
        provenance._archive_command(
            ["/usr/bin/ar", *tail],
            build,
            roots,
            "/usr/bin/ar",
            "synthetic archive",
        )


@pytest.mark.parametrize(
    ("index", "extra", "message"),
    [
        (
            {"reply": {"codemodel-v2": {"jsonFile": "../escape.json"}}},
            {},
            "unsafe JSON file",
        ),
        (
            {"reply": {"codemodel-v2": {"jsonFile": "codemodel.json"}}},
            {"codemodel.json": {}, "stale.json": {}},
            "stale or unreferenced JSON",
        ),
    ],
    ids=["unsafe-reference", "unreferenced-reply"],
)
def test_file_api_graph_rejects_unsafe_or_unowned_documents(
    tmp_path: Path,
    index: dict[str, Any],
    extra: dict[str, Any],
    message: str,
) -> None:
    reply = tmp_path / "reply"
    reply.mkdir()
    index_path = reply / "index-1.json"
    index_path.write_text(json.dumps(index), encoding="utf-8")
    for name, value in extra.items():
        (reply / name).write_text(json.dumps(value), encoding="utf-8")

    with pytest.raises(provenance.NativeBuildProvenanceError, match=message):
        provenance._file_api_documents(reply, index_path)


def _native_host_fixture(
    tmp_path: Path,
) -> tuple[Path, Path, Path, dict[str, Any], dict[str, Path]]:
    repository = tmp_path / "repository"
    lane_root = tmp_path / "lane"
    repository.mkdir()
    lane_root.mkdir()
    files = {
        "binary": _write(lane_root / "build/test_synthetic", b"\x7fELFbinary"),
        "file_api": _write(lane_root / "build/file-api-target.json", b"{}\n"),
        "response": _write(lane_root / "build/link.rsp", b"-lqualified\n"),
        "archive": _write(lane_root / "build/libproduct.a", b"!<arch>\narchive"),
        "archive_member": _write(lane_root / "build/product.o", b"object"),
    }

    def logical(path: Path) -> str:
        return f"/qualification-output/{path.relative_to(lane_root).as_posix()}"

    row = {
        "lane_id": "synthetic_native",
        "required_group": "synthetic_group",
        "sanitizer": "none",
        "build_root": "build",
        "provenance_output": "native-build-provenance.json",
    }
    evidence: dict[str, Any] = {
        "schema_version": "manyforge.composable_cell_i2.native_build_provenance.v1",
        "lane_id": row["lane_id"],
        "required_group": row["required_group"],
        "sanitizer": row["sanitizer"],
        "build_root": "/qualification-output/build",
        "provenance_output": "/qualification-output/native-build-provenance.json",
        "projects": [
            {
                "file_api_target_reply": {
                    "path": logical(files["file_api"]),
                    "sha256": _sha256(files["file_api"]),
                }
            }
        ],
        "targets": [
            {
                "role": "gtest",
                "artifact": {
                    "path": logical(files["binary"]),
                    "resolved_path": logical(files["binary"]),
                    "sha256": _sha256(files["binary"]),
                },
            }
        ],
        "production_targets": [
            {
                "project_id": "synthetic",
                "target": "product",
                "target_type": "STATIC_LIBRARY",
                "artifact": {
                    "path": logical(files["archive"]),
                    "resolved_path": logical(files["archive"]),
                    "sha256": _sha256(files["archive"]),
                },
                "archives": [
                    {
                        "members": [
                            {
                                "producer_object": logical(files["archive_member"]),
                                "sha256": _sha256(files["archive_member"]),
                            }
                        ]
                    }
                ],
            }
        ],
        "test_only_targets": [],
        "utility_target_relationships": [],
        "production_parity": [],
        "test_only_parity": [],
        "execution_parity": [],
        "cross_profile_parity": [],
        "ordinary_tests": [],
        "installed_copies": [],
        "qualified_install_inputs": [],
        "install_projects": [],
        "response_files": [
            {
                "path": logical(files["response"]),
                "sha256": _sha256(files["response"]),
                "size": files["response"].stat().st_size,
                "token_count": 1,
            }
        ],
        "external_dependencies": [],
        "inspection_tools": [],
        "passed": True,
    }
    evidence["evidence_sha256"] = verifier._canonical_json_sha256(evidence)
    provenance_path = lane_root / row["provenance_output"]
    provenance_path.write_text(json.dumps(evidence), encoding="utf-8")
    return repository, lane_root, provenance_path, row, files


@pytest.mark.parametrize(
    "file_role",
    ["binary", "file_api", "response", "archive", "archive_member"],
)
def test_host_reauthenticates_every_packet_named_build_evidence_class(
    tmp_path: Path,
    file_role: str,
) -> None:
    repository, lane_root, provenance_path, row, files = _native_host_fixture(
        tmp_path
    )
    evidence = json.loads(provenance_path.read_text(encoding="utf-8"))
    verifier._authenticate_provenance_files(
        evidence,
        lane_root=lane_root,
        repository_root=repository,
        label="synthetic native provenance",
    )

    files[file_role].write_bytes(files[file_role].read_bytes() + b"mutated")

    with pytest.raises(verifier.ManifestError, match="changed after"):
        verifier._authenticate_provenance_files(
            evidence,
            lane_root=lane_root,
            repository_root=repository,
            label="synthetic native provenance",
        )


def test_host_rehash_skips_alias_normalized_parity_projections(
    tmp_path: Path,
) -> None:
    repository, lane_root, provenance_path, _row, files = _native_host_fixture(
        tmp_path
    )
    evidence = json.loads(provenance_path.read_text(encoding="utf-8"))
    alias_projection = [
        {
            "compile_dependencies": [
                {
                    "path": "<build:synthetic>/generated/evidence.hpp",
                    "sha256": "a" * 64,
                }
            ]
        }
    ]
    for field in (
        "production_parity",
        "test_only_parity",
        "execution_parity",
        "cross_profile_parity",
        "deterministic_parity",
    ):
        evidence[field] = alias_projection

    authenticated = verifier._authenticate_provenance_files(
        evidence,
        lane_root=lane_root,
        repository_root=repository,
        label="synthetic native provenance",
    )
    assert {row["path"] for row in authenticated} == {
        f"/qualification-output/{path.relative_to(lane_root).as_posix()}"
        for path in files.values()
    }

    evidence["execution_parity_alias"] = alias_projection
    with pytest.raises(verifier.ManifestError, match="path/digest is not canonical"):
        verifier._authenticate_provenance_files(
            evidence,
            lane_root=lane_root,
            repository_root=repository,
            label="synthetic native provenance",
        )


def test_host_rejects_conflicting_digests_inside_self_consistent_provenance(
    tmp_path: Path,
) -> None:
    repository, lane_root, provenance_path, row, files = _native_host_fixture(
        tmp_path
    )
    evidence = json.loads(provenance_path.read_text(encoding="utf-8"))
    evidence["projects"].append(
        {
            "conflicting_claim": {
                "path": f"/qualification-output/{files['file_api'].relative_to(lane_root)}",
                "sha256": "0" * 64,
            }
        }
    )
    evidence.pop("evidence_sha256")
    evidence["evidence_sha256"] = verifier._canonical_json_sha256(evidence)
    provenance_path.write_text(json.dumps(evidence), encoding="utf-8")

    with pytest.raises(verifier.ManifestError, match="changed after|conflicting digests"):
        verifier._authenticate_provenance_files(
            evidence,
            lane_root=lane_root,
            repository_root=repository,
            label="synthetic native provenance",
        )


def _host_target_projection_fixture(
    tmp_path: Path, *, utility: bool = False
) -> dict[str, Any]:
    lane_root = tmp_path / "lane"
    repository = tmp_path / "repository"
    lane_root.mkdir()
    source = _write(repository / "project/source.cpp")
    source_digest = _sha256(source)
    build_root = lane_root / "build/project"
    if utility:
        artifact_relative = "generated/generated.hpp"
        artifact = _write(
            build_root / artifact_relative,
            b"#define GENERATED_PROFILE \"<profile>\"\n",
        )
        target_name = "generate_product"
        target_type = "UTILITY"
        direct_sources: list[dict[str, str]] = []
        file_api_sources: list[dict[str, str]] = []
        inventory_root = "/qualification-output/build/project/generated"
        mode = f"{artifact.stat().st_mode & 0o777:04o}"
        cross_digest = provenance.generated_output_cross_profile_sha256(artifact)
        output_inventory = {
            "policy": "single_file",
            "root": inventory_root,
            "entry_count": 1,
            "total_file_bytes": artifact.stat().st_size,
            "entries_sha256": verifier._canonical_json_sha256(
                [
                    {
                        "relative_path": "generated.hpp",
                        "kind": "file",
                        "size": artifact.stat().st_size,
                        "mode": mode,
                        "sha256": _sha256(artifact),
                    }
                ]
            ),
            "entries": [
                {
                    "relative_path": "generated.hpp",
                    "kind": "file",
                    "size": artifact.stat().st_size,
                    "mode": mode,
                    "sha256": _sha256(artifact),
                }
            ],
        }
        cross_entries = [
            {
                "relative_path": "generated.hpp",
                "kind": "file",
                "mode": mode,
                "cross_profile_sha256": cross_digest,
            }
        ]
        cross_inventory: dict[str, Any] | None = {
            "entries": cross_entries,
            "entries_sha256": verifier._canonical_json_sha256(cross_entries),
        }
    else:
        artifact_relative = "libproduct.a"
        artifact = _write(build_root / artifact_relative, b"!<arch>\nproduct")
        target_name = "product"
        target_type = "STATIC_LIBRARY"
        direct_sources = [
            {"path": "/workspace/project/source.cpp", "sha256": source_digest}
        ]
        file_api_sources = [
            {
                "path": "/workspace/project/source.cpp",
                "sha256": source_digest,
                "cross_profile_sha256": source_digest,
            }
        ]
        output_inventory = None
        cross_inventory = None
    artifact_logical = f"/qualification-output/build/project/{artifact_relative}"
    parity_digest = "a" * 64
    cross_command_digest = "b" * 64
    command_output = artifact_logical
    command = {
        "kind": "generated" if utility else "archive",
        "output": command_output,
        "argv_sha256": "c" * 64,
        "parity_argv_sha256": parity_digest,
        "cross_profile_argv_sha256": cross_command_digest,
        "dependency_closure": None,
        "source": None,
    }
    proof: dict[str, Any] = {
        "project_id": "project",
        "target": target_name,
        "target_type": target_type,
        "contract_artifact": artifact_relative,
        "artifact": {
            "path": artifact_logical,
            "resolved_path": artifact_logical,
            "sha256": _sha256(artifact),
        },
        "file_api_sources": file_api_sources,
        "commands": [command],
    }
    if utility:
        proof["output_inventory"] = output_inventory
        proof["cross_profile_output_inventory"] = cross_inventory
    projected_command = {
        "kind": command["kind"],
        "output": (
            "<build:project>/generated/generated.hpp"
            if utility
            else "<build:project>/libproduct.a"
        ),
        "argv_sha256": parity_digest,
        "cross_profile_argv_sha256": cross_command_digest,
    }
    projection = {
        "project_id": "project",
        "target": target_name,
        "target_type": target_type,
        "artifact_relative": artifact_relative,
        "artifact_sha256": _sha256(artifact),
        "output_inventory_cross_profile_sha256": (
            cross_inventory["entries_sha256"] if cross_inventory is not None else None
        ),
        "direct_sources": direct_sources,
        "closure_sources": [],
        "compile_dependencies": [],
        "command_semantics": [projected_command],
    }
    return {
        "lane_root": lane_root,
        "repository_root": repository,
        "proofs": [proof],
        "projection": [projection],
        "configure_generated_specs": {},
        "qualified_install_owners": {},
        "install_roots": {},
        "source_roots": {"project": "/workspace/project"},
        "project_provenance": {},
        "expected": {
            ("project", target_name): {
                "target_type": target_type,
                "artifact_relative": artifact_relative,
                "direct_source_paths": [row["path"] for row in direct_sources],
            }
        },
    }


def _validate_host_target_projection_fixture(fixture: dict[str, Any]) -> None:
    verifier._validate_target_proofs_against_projection(
        fixture["proofs"],
        fixture["projection"],
        expected=fixture["expected"],
        build_roots={"project": "/qualification-output/build/project"},
        source_roots=fixture["source_roots"],
        install_roots=fixture["install_roots"],
        configure_generated_specs=fixture["configure_generated_specs"],
        qualified_install_owners=fixture["qualified_install_owners"],
        project_provenance=fixture["project_provenance"],
        repository_root=fixture["repository_root"],
        lane_root=fixture["lane_root"],
        allow_unexpected_proofs=False,
        label="synthetic target proof/parity join",
    )


def _add_compile_dependency_fixture(
    fixture: dict[str, Any],
) -> tuple[dict[str, Any], dict[str, Any], Path]:
    proof = fixture["proofs"][0]
    projection = fixture["projection"][0]
    lane_root = fixture["lane_root"]
    generated = _write(
        lane_root / "build/project/generated/embedded_build_evidence.hpp",
        _embedded_build_evidence_header(
            profile="Release", configuration_digest=f"sha256:{'9' * 64}"
        ),
    )
    generated_path = (
        "/qualification-output/build/project/generated/embedded_build_evidence.hpp"
    )
    generated_digest = _sha256(generated)
    generated_cross_digest = provenance.compile_dependency_cross_profile_sha256(
        generated, "configure_generated_input"
    )
    cmake_input = _write(
        fixture["repository_root"] / "manyforge_deployment/CMakeLists.txt",
        b"cmake_minimum_required(VERSION 3.25)\n",
    )
    external_input = _write(
        fixture["repository_root"] / "shared/config.yaml",
        b"schema: synthetic\n",
    )
    cmake_input_digest = _sha256(cmake_input)
    required_inputs: list[dict[str, Any]] = [
        {
            "path": "/workspace/manyforge_deployment/CMakeLists.txt",
            "sha256": cmake_input_digest,
            "is_generated": False,
            "is_external": True,
            "is_cmake": False,
            "identity": {
                "kind": "git_tracked_source",
                "git_root": "/workspace",
                "path": "manyforge_deployment/CMakeLists.txt",
                "mode": "100644",
                "blob": _git_blob(cmake_input),
            },
        },
        {
            # Host evidence retains the normalized path, while the producer
            # derived this false flag from canonical raw ../shared/config.yaml.
            "path": "/workspace/shared/config.yaml",
            "sha256": _sha256(external_input),
            "is_generated": False,
            "is_external": False,
            "is_cmake": False,
            "identity": {
                "kind": "git_tracked_source",
                "git_root": "/workspace",
                "path": "shared/config.yaml",
                "mode": "100644",
                "blob": _git_blob(external_input),
            },
        },
    ]
    configure_owner = "manyforge_deployment"
    fixture["configure_generated_specs"] = {
        "embedded_build_evidence.hpp": {
            "owner_project_id": configure_owner,
            "allowed_build_relative_paths": [
                "generated/embedded_build_evidence.hpp"
            ],
            "required_input_paths": [
                "/workspace/manyforge_deployment/CMakeLists.txt",
                "/workspace/shared/config.yaml",
            ],
        }
    }
    fixture["project_provenance"] = {
        "project": {
            "project_id": "project",
            "source_git_generation": {"git_root": "/workspace"},
            "cmake": {"path": "/usr/bin/cmake", "sha256": "a" * 64},
            "cmake_cache": {"configuration": "Release"},
            "configure_inputs": required_inputs,
        }
    }
    dependency = {
        "path": generated_path,
        "resolved_path": generated_path,
        "symlink_chain": [generated_path],
        "sha256": generated_digest,
        "cross_profile_sha256": generated_cross_digest,
        "scope": "configure_generated_input",
        "producer": {
            "kind": "cmake_configure_file",
            "owner_project_id": configure_owner,
            "output": generated_path,
            "output_sha256": generated_digest,
            "cmake": fixture["project_provenance"]["project"]["cmake"],
            "cmake_cache": fixture["project_provenance"]["project"][
                "cmake_cache"
            ],
            "semantic_input_policy": (
                "all_authenticated_cmake_inputs_and_exact_cache"
            ),
            "required_inputs": required_inputs,
            "all_configure_inputs_sha256": verifier._canonical_json_sha256(
                required_inputs
            ),
        },
        "source_identity": None,
    }
    source_path = "/workspace/project/source.cpp"
    source_digest = proof["file_api_sources"][0]["sha256"]
    source_dependency = {
        "path": source_path,
        "resolved_path": source_path,
        "symlink_chain": [source_path],
        "sha256": source_digest,
        "cross_profile_sha256": source_digest,
        "scope": "repository_source",
        "producer": None,
        "source_identity": {
            "git_root": "/workspace",
            "path": "project/source.cpp",
            "mode": "100644",
            "blob": _git_blob(
                fixture["repository_root"] / "project/source.cpp"
            ),
        },
    }
    compile_command = {
        "kind": "compile",
        "output": (
            "/qualification-output/build/project/"
            "CMakeFiles/product.dir/source.cpp.o"
        ),
        "argv_sha256": "d" * 64,
        "parity_argv_sha256": "e" * 64,
        "cross_profile_argv_sha256": "f" * 64,
        "dependency_closure": {
            "object": (
                "/qualification-output/build/project/"
                "CMakeFiles/product.dir/source.cpp.o"
            ),
            "declared_dependency_count": 2,
            "raw_sha256": "1" * 64,
            "files": [dependency, source_dependency],
        },
        "source": {
            "path": source_path,
            "sha256": source_digest,
        },
    }
    proof["commands"].append(compile_command)
    projection["closure_sources"] = [
        {
            "path": "/workspace/project/source.cpp",
            "sha256": source_digest,
        }
    ]
    projection["compile_dependencies"] = sorted(
        [
            {
                "path": "<build:project>/generated/embedded_build_evidence.hpp",
                "sha256": generated_cross_digest,
                "scope": "configure_generated_input",
                "producer_kind": "cmake_configure_file",
                "producer_project": configure_owner,
                "producer_target": None,
                "producer_inputs_sha256": verifier._canonical_json_sha256(
                    [
                        {
                            "path": row["path"],
                            "sha256": row["sha256"],
                            "identity": row["identity"],
                        }
                        for row in required_inputs
                    ]
                ),
            },
            {
                "path": source_path,
                "sha256": source_digest,
                "scope": "repository_source",
                "producer_kind": None,
                "producer_project": None,
                "producer_target": None,
                "producer_inputs_sha256": None,
            },
        ],
        key=lambda row: row["path"],
    )
    projection["command_semantics"].append(
        {
            "kind": "compile",
            "output": "<build:project>/CMakeFiles/product.dir/source.cpp.o",
            "argv_sha256": "e" * 64,
            "cross_profile_argv_sha256": "f" * 64,
        }
    )
    projection["command_semantics"].sort(
        key=lambda row: (row["kind"], row["output"], row["argv_sha256"])
    )
    fixture["expected"][("project", "product")].update(
        {
            "compiled_source_paths": [source_path],
            "dependency_only_source_paths": [],
        }
    )
    return dependency, compile_command, generated


def _add_executable_generator_fixture(
    fixture: dict[str, Any],
    dependency: dict[str, Any],
) -> dict[str, Any]:
    lane_root = fixture["lane_root"]
    original_generated = (
        lane_root / Path(dependency["path"]).relative_to("/qualification-output")
    )
    generated_path = (
        "/qualification-output/build/project/generated/"
        "executable_generated.hpp"
    )
    generated = _write(
        lane_root / Path(generated_path).relative_to("/qualification-output"),
        original_generated.read_bytes(),
    )
    generator = _write(
        lane_root / "build/project/bin/generator",
        b"\x7fELFsynthetic-generator",
        executable=True,
    )
    artifact_path = "/qualification-output/build/project/bin/generator"
    generated_command = {
        "kind": "generated",
        "output": generated_path,
        "inputs": [artifact_path],
        "input_evidence": [
            {
                "path": artifact_path,
                "resolved_path": artifact_path,
                "symlink_chain": [artifact_path],
                "sha256": _sha256(generator),
                "scope": "selected_build",
                "kind": "authenticated_non_code_input",
            }
        ],
        "argv_sha256": "2" * 64,
        "parity_argv_sha256": "3" * 64,
        "cross_profile_argv_sha256": "4" * 64,
        "dependency_closure": None,
        "source": None,
    }
    fixture["proofs"][0]["commands"].append(generated_command)
    dependency.update(
        {
            "path": generated_path,
            "resolved_path": generated_path,
            "symlink_chain": [generated_path],
            "sha256": _sha256(generated),
            "scope": "generated_build_input",
        }
    )
    dependency["cross_profile_sha256"] = (
        provenance.compile_dependency_cross_profile_sha256(
            generated,
            "generated_build_input",
        )
    )
    dependency["producer"] = {
        "kind": "executable_build_generator",
        "project_id": "project",
        "target": "generator",
        "artifact": artifact_path,
        "artifact_sha256": _sha256(generator),
        "argv_sha256": generated_command["argv_sha256"],
        "parity_argv_sha256": generated_command["parity_argv_sha256"],
    }
    generated_projection = next(
        row
        for row in fixture["projection"][0]["compile_dependencies"]
        if row["path"].startswith("<build:project>/")
    )
    generated_projection.update(
        {
            "path": "<build:project>/generated/executable_generated.hpp",
            "sha256": dependency["cross_profile_sha256"],
            "scope": "generated_build_input",
            "producer_kind": "executable_build_generator",
            "producer_project": "project",
            "producer_target": "generator",
            "producer_inputs_sha256": None,
        }
    )
    fixture["projection"][0]["command_semantics"].append(
        {
            "kind": "generated",
            "output": "<build:project>/generated/executable_generated.hpp",
            "argv_sha256": generated_command["parity_argv_sha256"],
            "cross_profile_argv_sha256": generated_command[
                "cross_profile_argv_sha256"
            ],
        }
    )
    fixture["projection"][0]["command_semantics"].sort(
        key=lambda row: (row["kind"], row["output"], row["argv_sha256"])
    )

    generator_proof = {
        "project_id": "project",
        "target": "generator",
        "target_type": "EXECUTABLE",
        "contract_artifact": "bin/generator",
        "artifact": {
            "path": artifact_path,
            "resolved_path": artifact_path,
            "sha256": _sha256(generator),
        },
        "file_api_sources": [],
        "commands": [
            {
                "kind": "link",
                "output": artifact_path,
                "argv_sha256": "5" * 64,
                "parity_argv_sha256": "6" * 64,
                "cross_profile_argv_sha256": "7" * 64,
                "dependency_closure": None,
                "source": None,
            }
        ],
    }
    generator_projection = {
        "project_id": "project",
        "target": "generator",
        "target_type": "EXECUTABLE",
        "artifact_relative": "bin/generator",
        "artifact_sha256": _sha256(generator),
        "output_inventory_cross_profile_sha256": None,
        "direct_sources": [],
        "closure_sources": [],
        "compile_dependencies": [],
        "command_semantics": [
            {
                "kind": "link",
                "output": "<build:project>/bin/generator",
                "argv_sha256": "6" * 64,
                "cross_profile_argv_sha256": "7" * 64,
            }
        ],
    }
    fixture["proofs"].append(generator_proof)
    fixture["proofs"].sort(key=lambda row: (row["project_id"], row["target"]))
    fixture["projection"].append(generator_projection)
    fixture["projection"].sort(key=lambda row: (row["project_id"], row["target"]))
    fixture["expected"][("project", "generator")] = {
        "target_type": "EXECUTABLE",
        "artifact_relative": "bin/generator",
        "direct_source_paths": [],
    }
    return generated_command


def _add_qualified_install_dependency_fixture(
    fixture: dict[str, Any],
    compile_command: dict[str, Any],
) -> tuple[dict[str, Any], dict[str, Any]]:
    lane_root = fixture["lane_root"]
    install_root = "/qualification-output/install/dependency"
    install_relative = "include/dependency/header.hpp"
    installed_path = f"{install_root}/{install_relative}"
    installed = _write(
        lane_root / Path(installed_path).relative_to("/qualification-output"),
        b"#pragma once\n#define DEPENDENCY_HEADER 1\n",
    )
    source_path = "/qualification-output/source/dependency/include/header.hpp"
    source = _write(
        lane_root / Path(source_path).relative_to("/qualification-output"),
        installed.read_bytes(),
    )
    digest = _sha256(installed)
    producer = {
        "kind": "git_tracked_install_source",
        "path": source_path,
        "sha256": digest,
        "source_identity": {
            "git_root": "/qualification-output/source",
            "path": "dependency/include/header.hpp",
            "mode": "100644",
            "blob": _git_blob(source),
        },
    }
    manifest = {
        "path": "/qualification-output/build/dependency/install_manifest.txt",
        "sha256": "0" * 64,
        "row_count": 1,
        "distinct_path_count": 1,
        "relative_rows_sha256": "1" * 64,
        "repeated_relative_paths": [],
    }
    owner = {
        "path": installed_path,
        "resolved_path": installed_path,
        "symlink_chain": [],
        "sha256": digest,
        "project_id": "dependency",
        "install_relative_path": install_relative,
        "install_manifest": manifest,
        "producer": producer,
    }
    dependency = {
        "path": installed_path,
        "resolved_path": installed_path,
        "symlink_chain": [installed_path],
        "sha256": digest,
        "cross_profile_sha256": digest,
        "scope": "qualified_install_input",
        "producer": {"kind": "qualified_install_input", "owner": owner},
        "source_identity": None,
    }
    closure = compile_command["dependency_closure"]
    closure["files"].append(dependency)
    closure["files"].sort(key=lambda row: row["path"])
    closure["declared_dependency_count"] = len(closure["files"])
    projected = {
        "path": "<install:dependency>/include/dependency/header.hpp",
        "sha256": digest,
        "scope": "qualified_install_input",
        "producer_kind": "qualified_install_input",
        "producer_project": "dependency",
        "producer_target": None,
        "producer_inputs_sha256": verifier._canonical_json_sha256(
            {
                "installed_sha256": digest,
                "producer_kind": producer["kind"],
                "producer_sha256": producer["sha256"],
            }
        ),
    }
    fixture["projection"][0]["compile_dependencies"].append(projected)
    fixture["projection"][0]["compile_dependencies"].sort(
        key=lambda row: row["path"]
    )
    fixture["install_roots"] = {"dependency": install_root}
    fixture["qualified_install_owners"] = {installed_path: owner}
    return dependency, owner


def _host_qualified_external_source_fixture(
    tmp_path: Path,
) -> tuple[dict[str, Any], dict[str, Any]]:
    fixture = _host_target_projection_fixture(tmp_path)
    source_root = "/qualification-output/source/project"
    raw_source_path = f"{source_root}/source.cpp"
    source = _write(
        fixture["lane_root"]
        / Path(raw_source_path).relative_to("/qualification-output"),
        b"int external_source() { return 7; }\n",
    )
    digest = _sha256(source)
    proof = fixture["proofs"][0]
    proof["file_api_sources"] = [
        {
            "path": raw_source_path,
            "sha256": digest,
            "cross_profile_sha256": digest,
        }
    ]
    dependency = {
        "path": raw_source_path,
        "resolved_path": raw_source_path,
        "symlink_chain": [raw_source_path],
        "sha256": digest,
        "cross_profile_sha256": digest,
        "scope": "qualified_external_source",
        "producer": None,
        "source_identity": {
            "git_root": "/qualification-output/source",
            "path": "project/source.cpp",
            "mode": "100644",
            "blob": _git_blob(source),
        },
    }
    compile_output = (
        "/qualification-output/build/project/CMakeFiles/product.dir/source.cpp.o"
    )
    proof["commands"].append(
        {
            "kind": "compile",
            "output": compile_output,
            "argv_sha256": "2" * 64,
            "parity_argv_sha256": "3" * 64,
            "cross_profile_argv_sha256": "4" * 64,
            "dependency_closure": {
                "object": compile_output,
                "declared_dependency_count": 1,
                "raw_sha256": "5" * 64,
                "files": [dependency],
            },
            "source": {"path": raw_source_path, "sha256": digest},
        }
    )
    normalized_source = "<source:project>/source.cpp"
    projection = fixture["projection"][0]
    projection["direct_sources"] = [
        {"path": normalized_source, "sha256": digest}
    ]
    projection["closure_sources"] = [
        {"path": normalized_source, "sha256": digest}
    ]
    projection["compile_dependencies"] = [
        {
            "path": normalized_source,
            "sha256": digest,
            "scope": "qualified_external_source",
            "producer_kind": None,
            "producer_project": None,
            "producer_target": None,
            "producer_inputs_sha256": None,
        }
    ]
    projection["command_semantics"].append(
        {
            "kind": "compile",
            "output": "<build:project>/CMakeFiles/product.dir/source.cpp.o",
            "argv_sha256": "3" * 64,
            "cross_profile_argv_sha256": "4" * 64,
        }
    )
    projection["command_semantics"].sort(
        key=lambda row: (row["kind"], row["output"], row["argv_sha256"])
    )
    fixture["expected"][("project", "product")]["direct_source_paths"] = [
        normalized_source
    ]
    fixture["source_roots"] = {"project": source_root}
    fixture["project_provenance"] = {
        "project": {
            "project_id": "project",
            "source_git_generation": {
                "git_root": "/qualification-output/source"
            },
        }
    }
    return fixture, dependency


def test_host_derives_target_projection_from_current_raw_proof(tmp_path: Path) -> None:
    _validate_host_target_projection_fixture(_host_target_projection_fixture(tmp_path))


def test_host_derives_compile_dependency_projection_from_retained_bytes(
    tmp_path: Path,
) -> None:
    fixture = _host_target_projection_fixture(tmp_path)
    _add_compile_dependency_fixture(fixture)

    _validate_host_target_projection_fixture(fixture)


def test_host_rejects_non_boolean_configure_input_scope_flag(
    tmp_path: Path,
) -> None:
    fixture = _host_target_projection_fixture(tmp_path)
    dependency, _, _ = _add_compile_dependency_fixture(fixture)
    producer = dependency["producer"]
    producer["required_inputs"][1]["is_external"] = "false"
    producer["all_configure_inputs_sha256"] = verifier._canonical_json_sha256(
        producer["required_inputs"]
    )

    with pytest.raises(verifier.ManifestError, match="tracked-input shape drifted"):
        _validate_host_target_projection_fixture(fixture)


def test_host_rejects_direct_source_without_compile_command_owner(
    tmp_path: Path,
) -> None:
    fixture = _host_target_projection_fixture(tmp_path)
    _dependency, compile_command, _generated = _add_compile_dependency_fixture(
        fixture
    )
    fixture["proofs"][0]["commands"].remove(compile_command)
    projection = fixture["projection"][0]
    projection["closure_sources"] = []
    projection["compile_dependencies"] = []
    projection["command_semantics"] = [
        row for row in projection["command_semantics"] if row["kind"] != "compile"
    ]

    with pytest.raises(verifier.ManifestError, match="no exact compile-command owner"):
        _validate_host_target_projection_fixture(fixture)


def test_host_rejects_direct_source_with_duplicate_compile_command_owner(
    tmp_path: Path,
) -> None:
    fixture = _host_target_projection_fixture(tmp_path)
    _dependency, compile_command, _generated = _add_compile_dependency_fixture(
        fixture
    )
    duplicate = copy.deepcopy(compile_command)
    duplicate_output = (
        "/qualification-output/build/project/"
        "CMakeFiles/product.dir/duplicate-source.cpp.o"
    )
    duplicate["output"] = duplicate_output
    duplicate["dependency_closure"]["object"] = duplicate_output
    duplicate["argv_sha256"] = "6" * 64
    duplicate["parity_argv_sha256"] = "7" * 64
    duplicate["cross_profile_argv_sha256"] = "8" * 64
    fixture["proofs"][0]["commands"].append(duplicate)
    projection = fixture["projection"][0]
    projection["command_semantics"].append(
        {
            "kind": "compile",
            "output": (
                "<build:project>/CMakeFiles/product.dir/"
                "duplicate-source.cpp.o"
            ),
            "argv_sha256": duplicate["parity_argv_sha256"],
            "cross_profile_argv_sha256": duplicate[
                "cross_profile_argv_sha256"
            ],
        }
    )
    projection["command_semantics"].sort(
        key=lambda row: (row["kind"], row["output"], row["argv_sha256"])
    )

    with pytest.raises(verifier.ManifestError, match="no exact compile-command owner"):
        _validate_host_target_projection_fixture(fixture)


@pytest.mark.parametrize(
    "mutation",
    ["wrong-object", "wrong-count", "omitted-source"],
)
def test_host_rejects_compile_dependency_closure_envelope_or_source_drift(
    tmp_path: Path,
    mutation: str,
) -> None:
    fixture = _host_target_projection_fixture(tmp_path)
    _, compile_command, _ = _add_compile_dependency_fixture(fixture)
    closure = compile_command["dependency_closure"]
    expected_message = "dependency-closure envelope drifted"
    if mutation == "wrong-object":
        closure["object"] = "/qualification-output/build/project/wrong.o"
    elif mutation == "wrong-count":
        closure["declared_dependency_count"] += 1
    else:
        closure["files"] = [
            row for row in closure["files"] if row["scope"] != "repository_source"
        ]
        closure["declared_dependency_count"] = len(closure["files"])
        product_projection = fixture["projection"][0]
        product_projection["compile_dependencies"] = [
            row
            for row in product_projection["compile_dependencies"]
            if row["scope"] != "repository_source"
        ]
        expected_message = "source is absent from its dependency closure"

    with pytest.raises(verifier.ManifestError, match=expected_message):
        _validate_host_target_projection_fixture(fixture)


def test_host_rejects_forged_generated_dependency_normalization(
    tmp_path: Path,
) -> None:
    fixture = _host_target_projection_fixture(tmp_path)
    dependency, _, _ = _add_compile_dependency_fixture(fixture)
    forged = "8" * 64
    dependency["cross_profile_sha256"] = forged
    generated_projection = next(
        row
        for row in fixture["projection"][0]["compile_dependencies"]
        if row["path"].startswith("<build:project>/")
    )
    generated_projection["sha256"] = forged

    with pytest.raises(verifier.ManifestError, match="normalization drifted"):
        _validate_host_target_projection_fixture(fixture)


def test_host_rejects_relabelled_generated_dependency_resolved_path(
    tmp_path: Path,
) -> None:
    fixture = _host_target_projection_fixture(tmp_path)
    dependency, _, generated = _add_compile_dependency_fixture(fixture)
    alternate = _write(
        fixture["lane_root"] / "build/project/generated/alternate.hpp",
        generated.read_bytes(),
    )
    dependency["resolved_path"] = (
        "/qualification-output/build/project/generated/alternate.hpp"
    )
    dependency["sha256"] = _sha256(alternate)

    with pytest.raises(verifier.ManifestError, match="raw/resolved.*identity drifted"):
        _validate_host_target_projection_fixture(fixture)


@pytest.mark.parametrize(
    "mutation",
    ["missing-producer", "unknown-scope"],
)
def test_host_rejects_dependency_scope_ownership_drift(
    tmp_path: Path,
    mutation: str,
) -> None:
    fixture = _host_target_projection_fixture(tmp_path)
    dependency, _, _ = _add_compile_dependency_fixture(fixture)
    if mutation == "missing-producer":
        dependency["producer"] = None
    else:
        dependency["scope"] = "invented_scope"

    with pytest.raises(
        verifier.ManifestError,
        match="scope/producer ownership drifted|unknown dependency scope",
    ):
        _validate_host_target_projection_fixture(fixture)


@pytest.mark.parametrize(
    ("path", "resolved_path", "symlink_chain"),
    [
        (
            "/lib64/synthetic.hpp",
            "/lib64/synthetic.hpp",
            ["/lib64/synthetic.hpp"],
        ),
        (
            "/usr/include/synthetic.hpp",
            "/opt/synthetic.hpp",
            ["/usr/include/synthetic.hpp", "/opt/synthetic.hpp"],
        ),
    ],
)
def test_host_rejects_pinned_compile_dependency_outside_native_system_roots(
    tmp_path: Path,
    path: str,
    resolved_path: str,
    symlink_chain: list[str],
) -> None:
    fixture = _host_target_projection_fixture(tmp_path)
    dependency, _, _ = _add_compile_dependency_fixture(fixture)
    digest = "8" * 64
    dependency.update(
        {
            "path": path,
            "resolved_path": resolved_path,
            "symlink_chain": symlink_chain,
            "sha256": digest,
            "cross_profile_sha256": digest,
            "scope": "pinned_container",
            "producer": None,
            "source_identity": None,
        }
    )
    projected = next(
        row
        for row in fixture["projection"][0]["compile_dependencies"]
        if row["producer_kind"] == "cmake_configure_file"
    )
    projected.update(
        {
            "path": path,
            "sha256": digest,
            "scope": "pinned_container",
            "producer_kind": None,
            "producer_project": None,
            "producer_target": None,
            "producer_inputs_sha256": None,
        }
    )
    fixture["projection"][0]["compile_dependencies"].sort(
        key=lambda row: row["path"]
    )

    with pytest.raises(verifier.ManifestError, match="pinned scope escaped"):
        _validate_host_target_projection_fixture(fixture)


@pytest.mark.parametrize(
    "mutation",
    ["cmake-cache", "all-input-digest", "required-input-blob"],
)
def test_host_rejects_self_consistent_forged_configure_producer(
    tmp_path: Path,
    mutation: str,
) -> None:
    fixture = _host_target_projection_fixture(tmp_path)
    dependency, _, _ = _add_compile_dependency_fixture(fixture)
    producer = dependency["producer"]
    if mutation == "cmake-cache":
        producer["cmake_cache"] = {"configuration": "Forged"}
    elif mutation == "all-input-digest":
        producer["all_configure_inputs_sha256"] = "0" * 64
    else:
        # The producer and project rows intentionally share this current-schema
        # row, making the mutation self-consistent across both raw claims.
        producer["required_inputs"][0]["identity"]["blob"] = "0" * 40
        generated_projection = next(
            row
            for row in fixture["projection"][0]["compile_dependencies"]
            if row["producer_kind"] == "cmake_configure_file"
        )
        generated_projection["producer_inputs_sha256"] = (
            verifier._canonical_json_sha256(
                [
                    {
                        "path": row["path"],
                        "sha256": row["sha256"],
                        "identity": row["identity"],
                    }
                    for row in producer["required_inputs"]
                ]
            )
        )

    with pytest.raises(
        verifier.ManifestError,
        match="configure producer identity drifted|tracked-input shape drifted",
    ):
        _validate_host_target_projection_fixture(fixture)


def test_host_rejects_self_consistent_configure_input_git_root_relabel(
    tmp_path: Path,
) -> None:
    fixture = _host_target_projection_fixture(tmp_path)
    dependency, _, _ = _add_compile_dependency_fixture(fixture)
    producer = dependency["producer"]
    external_input = producer["required_inputs"][1]
    external_input["identity"]["git_root"] = "/workspace/shared"
    external_input["identity"]["path"] = "config.yaml"
    producer["all_configure_inputs_sha256"] = verifier._canonical_json_sha256(
        producer["required_inputs"]
    )
    generated_projection = next(
        row
        for row in fixture["projection"][0]["compile_dependencies"]
        if row["producer_kind"] == "cmake_configure_file"
    )
    generated_projection["producer_inputs_sha256"] = (
        verifier._canonical_json_sha256(
            [
                {
                    "path": row["path"],
                    "sha256": row["sha256"],
                    "identity": row["identity"],
                }
                for row in producer["required_inputs"]
            ]
        )
    )

    with pytest.raises(verifier.ManifestError, match="tracked-input shape drifted"):
        _validate_host_target_projection_fixture(fixture)


def test_host_rejects_self_consistent_configure_input_object_format_relabel(
    tmp_path: Path,
) -> None:
    fixture = _host_target_projection_fixture(tmp_path)
    dependency, _, _ = _add_compile_dependency_fixture(fixture)
    producer = dependency["producer"]
    external_input = producer["required_inputs"][1]
    raw = (fixture["repository_root"] / "shared/config.yaml").read_bytes()
    external_input["identity"]["blob"] = hashlib.sha256(
        f"blob {len(raw)}\0".encode("ascii") + raw
    ).hexdigest()
    producer["all_configure_inputs_sha256"] = verifier._canonical_json_sha256(
        producer["required_inputs"]
    )
    generated_projection = next(
        row
        for row in fixture["projection"][0]["compile_dependencies"]
        if row["producer_kind"] == "cmake_configure_file"
    )
    generated_projection["producer_inputs_sha256"] = (
        verifier._canonical_json_sha256(
            [
                {
                    "path": row["path"],
                    "sha256": row["sha256"],
                    "identity": row["identity"],
                }
                for row in producer["required_inputs"]
            ]
        )
    )

    with pytest.raises(verifier.ManifestError, match="tracked-input shape drifted"):
        _validate_host_target_projection_fixture(fixture)


def test_host_joins_repository_source_to_exact_tracked_identity(
    tmp_path: Path,
) -> None:
    fixture = _host_target_projection_fixture(tmp_path)
    _, compile_command, _ = _add_compile_dependency_fixture(fixture)
    source_dependency = next(
        row
        for row in compile_command["dependency_closure"]["files"]
        if row["scope"] == "repository_source"
    )
    source_dependency["source_identity"]["path"] = "project/other.cpp"

    with pytest.raises(verifier.ManifestError, match="tracked-source identity drifted"):
        _validate_host_target_projection_fixture(fixture)


@pytest.mark.parametrize("mutation", ["git-root", "blob", "mode"])
def test_host_rejects_repository_source_identity_root_blob_or_mode_drift(
    tmp_path: Path,
    mutation: str,
) -> None:
    fixture = _host_target_projection_fixture(tmp_path)
    _, compile_command, _ = _add_compile_dependency_fixture(fixture)
    source_dependency = next(
        row
        for row in compile_command["dependency_closure"]["files"]
        if row["scope"] == "repository_source"
    )
    identity = source_dependency["source_identity"]
    if mutation == "git-root":
        identity["git_root"] = "/workspace/project"
        identity["path"] = "source.cpp"
    elif mutation == "blob":
        identity["blob"] = "f" * 40
    else:
        identity["mode"] = "100755"

    with pytest.raises(
        verifier.ManifestError,
        match="tracked-source identity drifted|tracked-source bytes/mode drifted",
    ):
        _validate_host_target_projection_fixture(fixture)


def test_host_joins_qualified_install_dependency_to_authenticated_owner(
    tmp_path: Path,
) -> None:
    fixture = _host_target_projection_fixture(tmp_path)
    _, compile_command, _ = _add_compile_dependency_fixture(fixture)
    _add_qualified_install_dependency_fixture(fixture, compile_command)

    _validate_host_target_projection_fixture(fixture)


def test_host_joins_qualified_external_source_identity_and_bytes(
    tmp_path: Path,
) -> None:
    fixture, _ = _host_qualified_external_source_fixture(tmp_path)

    _validate_host_target_projection_fixture(fixture)


def test_host_rejects_qualified_external_source_git_root_relabel(
    tmp_path: Path,
) -> None:
    fixture, dependency = _host_qualified_external_source_fixture(tmp_path)
    dependency["source_identity"]["git_root"] = (
        "/qualification-output/source/project"
    )
    dependency["source_identity"]["path"] = "source.cpp"

    with pytest.raises(verifier.ManifestError, match="tracked-source identity drifted"):
        _validate_host_target_projection_fixture(fixture)


def test_host_rejects_forged_embedded_qualified_install_owner(
    tmp_path: Path,
) -> None:
    fixture = _host_target_projection_fixture(tmp_path)
    _, compile_command, _ = _add_compile_dependency_fixture(fixture)
    dependency, _ = _add_qualified_install_dependency_fixture(
        fixture, compile_command
    )
    forged_owner = copy.deepcopy(dependency["producer"]["owner"])
    forged_owner["install_manifest"] = {
        **forged_owner["install_manifest"],
        "sha256": "f" * 64,
    }
    dependency["producer"]["owner"] = forged_owner

    with pytest.raises(
        verifier.ManifestError,
        match="embedded install owner differs from the authenticated inventory",
    ):
        _validate_host_target_projection_fixture(fixture)


def test_host_joins_generated_dependency_to_executable_owner(tmp_path: Path) -> None:
    fixture = _host_target_projection_fixture(tmp_path)
    dependency, _, _ = _add_compile_dependency_fixture(fixture)
    _add_executable_generator_fixture(fixture, dependency)

    _validate_host_target_projection_fixture(fixture)


def test_host_rejects_reserved_configure_output_as_executable_generated(
    tmp_path: Path,
) -> None:
    fixture = _host_target_projection_fixture(tmp_path)
    dependency, _, _ = _add_compile_dependency_fixture(fixture)
    generated_command = _add_executable_generator_fixture(fixture, dependency)
    reserved_path = (
        "/qualification-output/build/project/generated/"
        "embedded_build_evidence.hpp"
    )
    reserved = fixture["lane_root"] / Path(reserved_path).relative_to(
        "/qualification-output"
    )
    dependency.update(
        {
            "path": reserved_path,
            "resolved_path": reserved_path,
            "symlink_chain": [reserved_path],
            "sha256": _sha256(reserved),
            "cross_profile_sha256": (
                provenance.compile_dependency_cross_profile_sha256(
                    reserved, "generated_build_input"
                )
            ),
        }
    )
    generated_command["output"] = reserved_path
    product_projection = next(
        row for row in fixture["projection"] if row["target"] == "product"
    )
    projected_dependency = next(
        row
        for row in product_projection["compile_dependencies"]
        if row["producer_kind"] == "executable_build_generator"
    )
    projected_dependency.update(
        {
            "path": "<build:project>/generated/embedded_build_evidence.hpp",
            "sha256": dependency["cross_profile_sha256"],
        }
    )
    projected_command = next(
        row
        for row in product_projection["command_semantics"]
        if row["kind"] == "generated"
    )
    projected_command["output"] = (
        "<build:project>/generated/embedded_build_evidence.hpp"
    )

    with pytest.raises(
        verifier.ManifestError,
        match="reserved configure-generated output",
    ):
        _validate_host_target_projection_fixture(fixture)


def test_host_rejects_generated_dependency_with_nonexecutable_owner(
    tmp_path: Path,
) -> None:
    fixture = _host_target_projection_fixture(tmp_path)
    dependency, _, _ = _add_compile_dependency_fixture(fixture)
    _add_executable_generator_fixture(fixture, dependency)
    dependency["producer"]["target"] = "product"
    product_projection = next(
        row for row in fixture["projection"] if row["target"] == "product"
    )
    generated_projection = next(
        row
        for row in product_projection["compile_dependencies"]
        if row["path"].startswith("<build:project>/")
    )
    generated_projection["producer_target"] = "product"

    with pytest.raises(verifier.ManifestError, match="no proved executable owner"):
        _validate_host_target_projection_fixture(fixture)


def test_host_rejects_generated_dependency_with_nonexecutable_artifact(
    tmp_path: Path,
) -> None:
    fixture = _host_target_projection_fixture(tmp_path)
    dependency, _, _ = _add_compile_dependency_fixture(fixture)
    _add_executable_generator_fixture(fixture, dependency)
    generator = fixture["lane_root"] / "build/project/bin/generator"
    generator.chmod(0o644)

    with pytest.raises(verifier.ManifestError, match="artifact is not executable"):
        _validate_host_target_projection_fixture(fixture)


def test_host_rejects_generated_command_executable_artifact_drift(
    tmp_path: Path,
) -> None:
    fixture = _host_target_projection_fixture(tmp_path)
    dependency, _, _ = _add_compile_dependency_fixture(fixture)
    _add_executable_generator_fixture(fixture, dependency)
    dependency["producer"]["artifact"] = (
        "/qualification-output/build/project/libproduct.a"
    )

    with pytest.raises(
        verifier.ManifestError,
        match="generated-command/executable owner join drifted",
    ):
        _validate_host_target_projection_fixture(fixture)


def test_host_rejects_generated_command_input_owner_drift(tmp_path: Path) -> None:
    fixture = _host_target_projection_fixture(tmp_path)
    dependency, _, _ = _add_compile_dependency_fixture(fixture)
    generated_command = _add_executable_generator_fixture(fixture, dependency)
    generated_command["inputs"] = [
        "/qualification-output/build/project/libproduct.a"
    ]

    with pytest.raises(
        verifier.ManifestError,
        match="generated command input owner join drifted",
    ):
        _validate_host_target_projection_fixture(fixture)


def test_host_joins_mixed_repository_and_generated_sources_across_sort_domains(
    tmp_path: Path,
) -> None:
    fixture = _host_target_projection_fixture(tmp_path)
    dependency, _compile_command, generated = _add_compile_dependency_fixture(
        fixture
    )
    raw_path = dependency["path"]
    direct_source_cross_digest = provenance.generated_output_cross_profile_sha256(
        generated
    )
    normalized_path = (
        "<build:project>/generated/embedded_build_evidence.hpp"
    )
    fixture["proofs"][0]["file_api_sources"].insert(
        0,
        {
            "path": raw_path,
            "sha256": dependency["sha256"],
            "cross_profile_sha256": direct_source_cross_digest,
        },
    )
    fixture["projection"][0]["direct_sources"] = sorted(
        [
            *fixture["projection"][0]["direct_sources"],
            {
                "path": normalized_path,
                "sha256": direct_source_cross_digest,
            },
        ],
        key=lambda row: row["path"],
    )
    fixture["expected"][("project", "product")]["direct_source_paths"] = sorted(
        [
            *fixture["expected"][("project", "product")]["direct_source_paths"],
            normalized_path,
        ]
    )
    fixture["expected"][("project", "product")][
        "dependency_only_source_paths"
    ] = [normalized_path]

    _validate_host_target_projection_fixture(fixture)


def test_host_rejects_raw_command_digest_projection_drift(tmp_path: Path) -> None:
    fixture = _host_target_projection_fixture(tmp_path)
    fixture["projection"][0]["command_semantics"][0]["argv_sha256"] = "d" * 64

    with pytest.raises(verifier.ManifestError, match="raw command projection"):
        _validate_host_target_projection_fixture(fixture)


def test_host_rejects_relabelled_contracted_target_artifact(tmp_path: Path) -> None:
    fixture = _host_target_projection_fixture(tmp_path)
    alternate = _write(fixture["lane_root"] / "build/project/alternate.a")
    fixture["proofs"][0]["artifact"]["resolved_path"] = (
        "/qualification-output/build/project/alternate.a"
    )
    fixture["proofs"][0]["artifact"]["sha256"] = _sha256(alternate)
    fixture["projection"][0]["artifact_sha256"] = _sha256(alternate)

    with pytest.raises(verifier.ManifestError, match="artifact/type identity"):
        _validate_host_target_projection_fixture(fixture)


def test_host_recomputes_utility_cross_profile_inventory(tmp_path: Path) -> None:
    fixture = _host_target_projection_fixture(tmp_path, utility=True)
    _validate_host_target_projection_fixture(fixture)
    forged_digest = "e" * 64
    fixture["proofs"][0]["cross_profile_output_inventory"][
        "entries_sha256"
    ] = forged_digest
    fixture["projection"][0][
        "output_inventory_cross_profile_sha256"
    ] = forged_digest

    with pytest.raises(verifier.ManifestError, match="retained bytes"):
        _validate_host_target_projection_fixture(fixture)


def _utility_executable_owner_fixture(
    tmp_path: Path,
) -> tuple[list[dict[str, Any]], list[dict[str, Any]], Path, Path]:
    lane_root = tmp_path / "lane"
    build_root = "/qualification-output/build/project"
    producer_relative = "bin/generator"
    generated_relative = "generated/output.hpp"
    producer_path = f"{build_root}/{producer_relative}"
    generated_path = f"{build_root}/{generated_relative}"
    producer = _write(
        lane_root / Path(producer_path).relative_to("/qualification-output"),
        b"\x7fELFsynthetic-generator\n",
        executable=True,
    )
    generated = _write(
        lane_root / Path(generated_path).relative_to("/qualification-output"),
        b"#pragma once\n#define GENERATED_VALUE 1\n",
    )
    producer_digest = _sha256(producer)
    generated_digest = _sha256(generated)
    inventory_root = f"{build_root}/generated"
    inventory_entry = {
        "relative_path": "output.hpp",
        "kind": "file",
        "size": generated.stat().st_size,
        "mode": f"{generated.stat().st_mode & 0o777:04o}",
        "sha256": generated_digest,
    }
    output_inventory = {
        "policy": "single_file",
        "root": inventory_root,
        "entry_count": 1,
        "total_file_bytes": generated.stat().st_size,
        "entries_sha256": verifier._canonical_json_sha256([inventory_entry]),
        "entries": [inventory_entry],
    }
    consumer_relationship = {
        "target": "consumer",
        "kind": "compiled_generated_input",
    }
    utility = {
        "project_id": "project",
        "target": "generate_output",
        "role": "production",
        "target_type": "UTILITY",
        "contract_artifact": generated_relative,
        "build_root": build_root,
        "artifact": {
            "path": generated_path,
            "resolved_path": generated_path,
            "sha256": generated_digest,
        },
        "file_api_target_reply": {
            "path": f"{build_root}/.cmake/api/v1/reply/target-generate_output.json",
            "sha256": "0" * 64,
        },
        "file_api_sources": [],
        "ninja_query": [
            {
                "node": "generate_output",
                "rule": "phony",
                "edges": [
                    {"type": "explicit", "path": generated_path}
                ],
                "raw_sha256": "1" * 64,
            }
        ],
        "ninja_commands_sha256": "2" * 64,
        "generated_source_producers": [],
        "generator": {
            "project_id": "project",
            "target": "generator",
            "path": producer_path,
            "sha256": producer_digest,
            "sanitizer_profile": "none",
        },
        "commands": [
            {
                "kind": "generated",
                "output": generated_path,
                "output_inventory_sha256": output_inventory["entries_sha256"],
                "inputs": [producer_path],
                "source": None,
                "argv_sha256": "3" * 64,
                "parity_argv_sha256": "4" * 64,
                "cross_profile_argv_sha256": "5" * 64,
                "sanitizer": None,
            }
        ],
        "archives": [],
        "output_inventory": output_inventory,
        "cross_profile_output_inventory": {
            "entries": [
                {
                    "relative_path": "output.hpp",
                    "kind": "file",
                    "mode": inventory_entry["mode"],
                    "cross_profile_sha256": generated_digest,
                }
            ],
            "entries_sha256": verifier._canonical_json_sha256(
                [
                    {
                        "relative_path": "output.hpp",
                        "kind": "file",
                        "mode": inventory_entry["mode"],
                        "cross_profile_sha256": generated_digest,
                    }
                ]
            ),
        },
        "generator_dependencies": [],
        "housekeeping_commands": [],
        "declared_byproducts": [],
        "custom_command_output_edge": {
            "explicit_outputs": [generated_relative],
            "implicit_outputs": [
                f"${{cmake_ninja_workdir}}{generated_relative}"
            ],
            "rule": "CUSTOM_COMMAND",
            "edge_sha256": "1" * 64,
            "build_ninja": {
                "path": f"{build_root}/build.ninja",
                "sha256": "2" * 64,
            },
        },
        "consumer_relationships": [consumer_relationship],
        "transitive_local_artifacts": [generated_path],
        "authenticated_output_dependencies": [],
    }
    producer_target = {
        "project_id": "project",
        "target": "generator",
        "target_type": "EXECUTABLE",
        "contract_artifact": producer_relative,
        "artifact": {
            "path": producer_path,
            "resolved_path": producer_path,
            "sha256": producer_digest,
        },
    }
    consumer = {
        "project_id": "project",
        "target": "consumer",
        "target_type": "STATIC_LIBRARY",
        "required_generated_inputs": [
            {
                "path": generated_path,
                "sha256": generated_digest,
                "producer_target": "generator",
                "producer_artifact": producer_path,
                "producer_sha256": producer_digest,
            }
        ],
    }
    relationship = {
        "project_id": "project",
        "target": "generate_output",
        "primary_output_relative_path": generated_relative,
        "generated_output_sha256": generated_digest,
        "output_inventory_sha256": output_inventory["entries_sha256"],
        "producer_target": "generator",
        "producer_artifact_relative_path": producer_relative,
        "producer_artifact_sha256": producer_digest,
        "consumer_relationships": [consumer_relationship],
    }
    return [relationship], [utility, producer_target, consumer], lane_root, producer


def test_utility_target_rejects_chmod_only_nonexecutable_producer(
    tmp_path: Path,
) -> None:
    relationships, targets, lane_root, producer = (
        _utility_executable_owner_fixture(tmp_path)
    )
    claimed_digest = _sha256(producer)
    producer.chmod(0o644)

    assert _sha256(producer) == claimed_digest
    assert producer.stat().st_mode & 0o111 == 0
    with pytest.raises(verifier.ManifestError, match="executable"):
        verifier._validate_utility_target_relationships(
            relationships,
            targets,
            "synthetic utility owner",
            lane_root=lane_root,
        )


def _validate_utility_command_fixture(
    targets: list[dict[str, Any]], lane_root: Path, repository_root: Path
) -> None:
    verifier._validate_target_command_file_evidence(
        [targets[0]],
        project_build_roots={
            "project": "/qualification-output/build/project"
        },
        target_output_owners=[targets[0]],
        installed_output_owners=[],
        expected_roles=frozenset({"production"}),
        sanitizer_profile="none",
        lane_root=lane_root,
        repository_root=repository_root,
        label="synthetic utility target command evidence",
    )


def test_host_validates_exact_utility_command_evidence(tmp_path: Path) -> None:
    _relationships, targets, lane_root, _producer = (
        _utility_executable_owner_fixture(tmp_path)
    )
    repository_root = tmp_path / "repository"
    repository_root.mkdir()

    _validate_utility_command_fixture(targets, lane_root, repository_root)


@pytest.mark.parametrize(
    "field",
    [
        "output_inventory_sha256",
        "inputs",
        "parity_argv_sha256",
        "sanitizer",
    ],
)
def test_host_rejects_incomplete_utility_command_envelope(
    tmp_path: Path, field: str
) -> None:
    _relationships, targets, lane_root, _producer = (
        _utility_executable_owner_fixture(tmp_path)
    )
    repository_root = tmp_path / "repository"
    repository_root.mkdir()
    del targets[0]["commands"][0][field]

    with pytest.raises(verifier.ManifestError, match="keys differ"):
        _validate_utility_command_fixture(targets, lane_root, repository_root)


def test_host_rejects_utility_command_inventory_relabel(tmp_path: Path) -> None:
    _relationships, targets, lane_root, _producer = (
        _utility_executable_owner_fixture(tmp_path)
    )
    repository_root = tmp_path / "repository"
    repository_root.mkdir()
    targets[0]["commands"][0]["output_inventory_sha256"] = "f" * 64

    with pytest.raises(verifier.ManifestError, match="generator join"):
        _validate_utility_command_fixture(targets, lane_root, repository_root)


def _target_spec(name: str) -> provenance._ProductTargetSpec:
    return provenance._ProductTargetSpec(
        name=name,
        target_type="STATIC_LIBRARY",
        artifact=Path(f"lib{name}.a"),
        required_sources=(),
        required_generated_sources=(),
    )


@pytest.mark.parametrize("explicit_contract", [False, True])
def test_registered_gtest_spec_uses_frozen_native_identity_without_duplication(
    tmp_path: Path, explicit_contract: bool
) -> None:
    repository, output, roots = _roots(tmp_path)
    project_id = "project"
    target_name = "test_synthetic"
    source = _write(repository / project_id / "test" / f"{target_name}.cpp")
    build_root = output / "build"
    build_root.mkdir()
    derived = provenance._ProductTargetSpec(
        name=target_name,
        target_type="EXECUTABLE",
        artifact=Path(target_name),
        required_sources=(source.resolve(),),
        required_generated_sources=(),
    )
    contract = provenance._ProductContract(
        project_id=project_id,
        source_root=(repository / project_id).resolve(),
        targets={},
        test_only_targets={target_name: derived} if explicit_contract else {},
    )

    assert provenance._registered_gtest_spec(
        project_id=project_id,
        build_root=build_root,
        target_name=target_name,
        registered_artifact=build_root / target_name,
        contract=contract,
        roots=roots,
    ) == derived


def test_registered_gtest_spec_rejects_conflicting_optional_explicit_contract(
    tmp_path: Path,
) -> None:
    repository, output, roots = _roots(tmp_path)
    project_id = "project"
    target_name = "test_synthetic"
    source = _write(repository / project_id / "test" / f"{target_name}.cpp")
    build_root = output / "build"
    build_root.mkdir()
    conflicting = provenance._ProductTargetSpec(
        name=target_name,
        target_type="EXECUTABLE",
        artifact=Path("relocated/test_synthetic"),
        required_sources=(source.resolve(),),
        required_generated_sources=(),
    )
    contract = provenance._ProductContract(
        project_id=project_id,
        source_root=(repository / project_id).resolve(),
        targets={},
        test_only_targets={target_name: conflicting},
    )

    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="explicit target contract differs",
    ):
        provenance._registered_gtest_spec(
            project_id=project_id,
            build_root=build_root,
            target_name=target_name,
            registered_artifact=build_root / target_name,
            contract=contract,
            roots=roots,
        )


@pytest.mark.parametrize(
    ("field", "value", "message"),
    [
        (
            "producer_target",
            "manyforge_deployment",
            "producer spec drifted",
        ),
        (
            "consumer_relationships",
            [
                {
                    "kind": "compiled_generated_input",
                    "target": "manyforge_deployment",
                }
            ],
            "consumer set drifted",
        ),
    ],
    ids=["producer-owner", "consumer-owner"],
)
def test_utility_target_contract_is_semantically_joined_to_target_specs(
    field: str,
    value: Any,
    message: str,
) -> None:
    contract = json.loads(BUILD_PROVENANCE_CONTRACT.read_text(encoding="utf-8"))
    contract["utility_target_contracts"][0][field] = value

    with pytest.raises(provenance.NativeBuildProvenanceError, match=message):
        provenance._utility_target_contracts(
            contract,
            repository_root=REPOSITORY_ROOT,
        )


def test_native_lane_partition_is_role_exact_and_rejects_substitution() -> None:
    contract = provenance._ProductContract(
        project_id="project",
        source_root=Path("/workspace/project"),
        targets={"runtime": _target_spec("runtime")},
        test_only_targets={"test_support": _target_spec("test_support")},
    )
    exact = {
        "native_lane_target_partitions": {
            "schema_version": provenance.NATIVE_LANE_TARGET_PARTITION_SCHEMA,
            "lanes": {
                "lane": {
                    "production_targets": ["runtime"],
                    "test_only_targets": ["test_support"],
                }
            },
        }
    }
    assert provenance._native_lane_partition(
        exact, lane_id="lane", contract=contract
    ) == provenance._NativeLanePartition(
        frozenset({"runtime"}), frozenset({"test_support"})
    )

    substituted = copy.deepcopy(exact)
    substituted["native_lane_target_partitions"]["lanes"]["lane"] = {
        "production_targets": ["test_support"],
        "test_only_targets": ["runtime"],
    }
    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="uncontracted target",
    ):
        provenance._native_lane_partition(
            substituted, lane_id="lane", contract=contract
        )


def test_production_target_rejects_one_extra_generated_code_source(
    tmp_path: Path,
) -> None:
    repository, output, roots = _roots(tmp_path)
    source_root = repository / "project"
    expected = _write(source_root / "expected.cpp")
    build_root = output / "build"
    extra = _write(build_root / "generated/evil.cpp")
    artifact = _write(build_root / "libproject.a", b"!<arch>\n")
    reply = provenance._TargetReply(
        name="project",
        target_type="STATIC_LIBRARY",
        artifacts=(artifact,),
        sources=(expected, extra),
        reply_path=build_root / "target-project.json",
        reply_sha256="0" * 64,
    )
    build = provenance._Build(
        lane_id="synthetic",
        project_id="project",
        root=build_root,
        source_root=source_root,
        compiler_path="/usr/bin/c++",
        archiver_path="/usr/bin/ar",
        targets={"project": reply},
        compile_commands={},
        configure_inputs={},
        configure_generated_contracts={},
        qualified_install_inputs={},
        evidence={},
    )

    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="candidate sources are not exact",
    ):
        provenance._target_reply(
            build,
            "project",
            artifact,
            {expected},
            roots,
            "production",
        )


@pytest.mark.parametrize(
    "proof",
    [
        {"target": "test_runtime"},
        {
            "target": "runtime",
            "file_api_sources": [{"path": "/workspace/project/tests/evil.cpp"}],
        },
    ],
    ids=["test-target", "test-source"],
)
def test_production_closure_rejects_test_only_target_or_source(
    proof: dict[str, Any],
) -> None:
    proof.setdefault("file_api_sources", [])
    proof.setdefault("commands", [])
    proof.setdefault("ninja_query", [])
    proof.setdefault("transitive_external_link_inputs", [])

    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="forbidden target/dependency|forbidden repository source",
    ):
        provenance._require_production_test_free(
            [proof],
            provenance._production_exclusions(BUILD_PROVENANCE_CONTRACT_VALUE),
        )


def test_production_validator_rejects_nonempty_ctest_inventory_before_build(
    tmp_path: Path,
) -> None:
    repository, output, _ = _roots(tmp_path)

    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="BUILD_TESTING=OFF production build registered tests",
    ):
        provenance.validate_production_build_provenance(
            repository,
            output,
            {},
            {"tests": [{"name": "unexpected"}]},
            {},
        )


def test_host_rejects_production_release_parity_drift(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    artifact_root = tmp_path / "artifacts"
    production_root = artifact_root / "lanes/core_production_release"
    native_root = artifact_root / "lanes/core_release"
    production_root.mkdir(parents=True)
    native_root.mkdir(parents=True)
    row = {
        "lane_id": "core_production_release",
        "project_id": "manyforge_core",
        "source_root": "manyforge_core",
        "build_root": "build/core-production-release",
        "inventory_output": "core-production/inventory.json",
        "provenance_output": "core-production/provenance.json",
        "execution_index_output": "core-production/index.json",
        "test_enabled_release_lane_id": "core_release",
        "configure_argv": ["configure"],
        "build_argv": ["build"],
        "inventory_argv": ["inventory"],
    }
    native_row = {
        "lane_id": "core_release",
        "provenance_output": "core-release/provenance.json",
    }
    inventory = {"tests": []}
    inventory_path = production_root / row["inventory_output"]
    inventory_path.parent.mkdir(parents=True)
    inventory_path.write_text(json.dumps(inventory), encoding="utf-8")
    parity = [{"target": "manyforge_core", "artifact_sha256": "1" * 64}]
    evidence: dict[str, Any] = {
        "schema_version": "manyforge.composable_cell_i2.production_build_provenance.v1",
        "lane_id": row["lane_id"],
        "project_id": row["project_id"],
        "build_root": f"/qualification-output/{row['build_root']}",
        "provenance_output": f"/qualification-output/{row['provenance_output']}",
        "empty_ctest_inventory_sha256": verifier._canonical_json_sha256(inventory),
        "project": {},
        "targets": [
            {
                "project_id": "manyforge_core",
                "target": "manyforge_core",
                "target_type": "STATIC_LIBRARY",
            }
        ],
        "utility_target_relationships": [],
        "production_parity": parity,
        "response_files": [],
        "inspection_tools": [],
        "passed": True,
    }
    evidence["evidence_sha256"] = verifier._canonical_json_sha256(evidence)
    provenance_path = production_root / row["provenance_output"]
    provenance_path.write_text(json.dumps(evidence), encoding="utf-8")
    index = {
        "schema_version": "manyforge.composable_cell_i2.production_build_execution.v1",
        "lane_id": row["lane_id"],
        "project_id": row["project_id"],
        "environment": verifier._production_build_environment(row["lane_id"]),
        "configure_argv": row["configure_argv"],
        "configure_exit_status": 0,
        "build_argv": row["build_argv"],
        "build_exit_status": 0,
        "inventory_argv": row["inventory_argv"],
        "inventory_exit_status": 0,
        "inventory_path": row["inventory_output"],
        "inventory_sha256": _sha256(inventory_path),
        "provenance_path": row["provenance_output"],
        "provenance_sha256": _sha256(provenance_path),
        "provenance_evidence_sha256": evidence["evidence_sha256"],
        "passed": True,
    }
    index_path = production_root / row["execution_index_output"]
    index_path.write_text(json.dumps(index), encoding="utf-8")
    native_path = native_root / native_row["provenance_output"]
    native_path.parent.mkdir(parents=True)
    native_path.write_text(
        json.dumps(
            {
                "production_parity": [
                    {"target": "manyforge_core", "artifact_sha256": "2" * 64}
                ]
            }
        ),
        encoding="utf-8",
    )
    manifest = {
        "test_inventory_contract": {
            "production_build_lanes": [row],
            "native_execution_lanes": [native_row],
        }
    }
    monkeypatch.setattr(verifier, "_authenticate_provenance_files", lambda *args, **kwargs: [])
    monkeypatch.setattr(
        verifier,
        "_load_build_provenance_contract",
        lambda *args, **kwargs: BUILD_PROVENANCE_CONTRACT_VALUE,
    )
    monkeypatch.setattr(
        verifier, "_validate_project_source_generations", lambda *args, **kwargs: None
    )
    monkeypatch.setattr(
        verifier,
        "_validate_target_proofs_against_projection",
        lambda *args, **kwargs: {},
    )
    monkeypatch.setattr(
        verifier,
        "_validate_target_command_file_evidence",
        lambda *args, **kwargs: None,
    )

    with pytest.raises(verifier.ManifestError, match="products differ"):
        verifier._validate_production_builds(
            manifest,
            artifact_root,
            {"core_production_release", "core_release"},
            tmp_path,
            {},
        )


def test_malformed_static_archive_is_rejected_without_member_inference(
    tmp_path: Path,
) -> None:
    _, output, roots = _roots(tmp_path)
    archive = _write(output / "build/libcopied.a", b"not-an-archive")
    source = _write(output / "build/member.o", b"object")
    producer = provenance._Command(
        kind="archive",
        output=archive,
        inputs=(source,),
        auxiliary_outputs=(),
        source=None,
        directory=archive.parent,
        argv=("/usr/bin/ar", "qc", str(archive), str(source)),
        argv_sha256="0" * 64,
    )

    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="not a regular archive",
    ):
        provenance._archive_evidence(archive, producer, roots)


def test_sanitized_lane_rejects_unsanitized_compile_or_link_command() -> None:
    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="effective sanitizer set differs",
    ):
        provenance._check_sanitizer(
            ["/usr/bin/c++", "-c", "source.cpp", "-o", "source.o"],
            "address+undefined",
            "binding compile",
        )


@pytest.mark.parametrize(
    ("lane_id", "project_id", "kind", "changed_token"),
    [
        (
            "core_asan_ubsan",
            "manyforge_core",
            "compile",
            "-fsanitize=address,undefined",
        ),
        (
            "core_tsan",
            "manyforge_core",
            "link",
            "-Wno-error=array-bounds",
        ),
        (
            "roboplan_asan_ubsan",
            "roboplan",
            "link",
            "-DEIGEN_MALLOC_ALREADY_ALIGNED=1",
        ),
    ],
    ids=["asan-duplicate", "tsan-warning-duplicate", "roboplan-eigen-duplicate"],
)
def test_cross_profile_command_delta_requires_exact_token_multiplicity(
    tmp_path: Path,
    lane_id: str,
    project_id: str,
    kind: str,
    changed_token: str,
) -> None:
    repository, output, roots = _roots(tmp_path)
    deltas, known_tokens = provenance._cross_profile_command_contract(
        BUILD_PROVENANCE_CONTRACT_VALUE,
        lane_id=lane_id,
        project_id=project_id,
        configuration="RelWithDebInfo",
    )
    build_root = output / "build"
    build_root.mkdir()
    build = provenance._Build(
        lane_id=lane_id,
        project_id=project_id,
        root=build_root,
        source_root=repository,
        compiler_path="/usr/bin/c++",
        archiver_path="/usr/bin/ar",
        targets={},
        compile_commands={},
        configure_inputs={},
        configure_generated_contracts={},
        qualified_install_inputs={},
        evidence={},
        parity_path_aliases=((build_root.as_posix(), f"<build:{project_id}>"),),
        cross_profile_command_deltas=deltas,
        cross_profile_known_delta_tokens=known_tokens,
    )
    argv = ["/usr/bin/c++"]
    for token, count in sorted(deltas[kind].items()):
        argv.extend([token] * count)
    profile = "thread" if "tsan" in lane_id else "address+undefined"
    provenance._cross_profile_argv_sha256(argv, build, roots, profile, kind)
    argv.append(changed_token)

    with pytest.raises(
        provenance.NativeBuildProvenanceError, match="profile token.*count drifted"
    ):
        provenance._cross_profile_argv_sha256(argv, build, roots, profile, kind)


def test_copied_archive_cannot_impersonate_an_upstream_target_owner(
    tmp_path: Path,
) -> None:
    repository, output, roots = _roots(tmp_path)
    owner = _write(output / "core/libowner.a", b"same-bytes")
    copied = _write(output / "binding/librelabeled.a", owner.read_bytes())

    proof = {
        "project_id": "manyforge_core_py",
        "target": "manyforge_core_py",
        "unresolved_output_dependency_claims": [roots.logical(copied)],
    }
    owner_proof = {
        "project_id": "manyforge_core",
        "target": "manyforge_core",
        "target_type": "STATIC_LIBRARY",
        "artifact": {
            "path": roots.logical(owner),
            "resolved_path": roots.logical(owner),
            "sha256": _sha256(owner),
        },
        "archives": [
            {
                "path": roots.logical(owner),
                "sha256": _sha256(owner),
                "members": [{"name": "owner.o", "sha256": "0" * 64}],
            }
        ],
    }

    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="no unique authenticated upstream",
    ):
        provenance._bind_output_dependency_owners(
            proofs=[proof],
            target_owners=[owner_proof],
            installed_owners=[],
            roots=roots,
        )

    assert repository.is_dir()


def test_transitive_output_dso_requires_a_proven_target_or_install_owner() -> None:
    elf = {
        "dependencies": [
            {
                "scope": "qualification_output",
                "resolved_path": "/qualification-output/unowned/libforeign.so",
                "sha256": "1" * 64,
            }
        ]
    }
    different_owner = {
        "artifact": {
            "resolved_path": "/qualification-output/owned/libqualified.so",
            "sha256": "1" * 64,
        }
    }

    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="runtime dependency has no File API/Ninja",
    ):
        provenance._validate_local_runtime_dependencies(
            [elf], [different_owner], []
        )


@pytest.mark.parametrize(
    ("field", "missing"),
    [
        ("required_translation_units", "manyforge_core/python/missing_registration.cpp"),
        ("required_header_dependencies", "manyforge_core/python/missing_registration.hpp"),
    ],
    ids=["translation-unit", "registration-header"],
)
def test_binding_contract_authenticates_every_registration_source_and_header(
    field: str,
    missing: str,
) -> None:
    contract = json.loads(BUILD_PROVENANCE_CONTRACT.read_text(encoding="utf-8"))
    contract = copy.deepcopy(contract)
    contract["binding_builder_constraints"]["target"][field][0] = missing

    with pytest.raises(provenance.NativeBuildProvenanceError, match="is absent"):
        provenance._binding_build_contract(
            contract,
            repository_root=REPOSITORY_ROOT,
        )


_FAKE_BINDING_FULL_PRODUCTION_TARGETS = {
    "manyforge_core": (
        "manyforge_content_identity",
        "manyforge_core",
    ),
    "manyforge_deployment": (
        "manyforge_deployment",
        "manyforge_deployment_admission",
        "manyforge_deployment_verification_core",
    ),
}
_FAKE_BINDING_SELECTED_ARCHIVE_TARGETS = {
    "manyforge_core": frozenset(
        {
            "manyforge_content_identity",
            "manyforge_core",
        }
    ),
    "manyforge_deployment": frozenset(
        {
            "manyforge_deployment",
            "manyforge_deployment_verification_core",
        }
    ),
}


def _fake_binding_contract(
    repository: Path,
) -> provenance._BindingBuildContract:
    source_root = repository / "manyforge_core/python"
    source_root.mkdir(parents=True)
    registration_sources = tuple(
        _write(source_root / f"registration_{index}.cpp") for index in range(7)
    )
    registration_header = _write(source_root / "binding_registrations.hpp")
    recipe = _write(source_root / "CMakeLists.txt")
    pybind11_include_root = repository / "pinned-pybind11/include"
    pybind11_header = _write(
        pybind11_include_root / "pybind11/pybind11.h", b"// pinned pybind11\n"
    )

    def dependency(project_id: str) -> provenance._BindingDependencyProject:
        project_root = repository / project_id
        project_root.mkdir(exist_ok=True)
        return provenance._BindingDependencyProject(
            project_id=project_id,
            source_root=project_root,
            build_testing_cache_key="BUILD_TESTING",
            production_targets={
                name: _target_spec(name)
                for name in _FAKE_BINDING_FULL_PRODUCTION_TARGETS[project_id]
            },
            module_linked_archive_targets=(
                _FAKE_BINDING_SELECTED_ARCHIVE_TARGETS[project_id]
            ),
        )

    dependency_projects = {
        project_id: dependency(project_id)
        for project_id in sorted(_FAKE_BINDING_FULL_PRODUCTION_TARGETS)
    }
    selected_archive_owners = frozenset(
        (project_id, target)
        for project_id, targets in _FAKE_BINDING_SELECTED_ARCHIVE_TARGETS.items()
        for target in targets
    )

    return provenance._BindingBuildContract(
        source_root=source_root,
        artifact=Path("manyforge_core_py.cpython-312-x86_64-linux-gnu.so"),
        registration_sources=registration_sources,
        registration_header=registration_header,
        build_contract=recipe,
        forbidden_modules=frozenset(
            {"manyforge_roboplan_py", "e0_nanobind", "e0_pybind"}
        ),
        forbidden_dependency_tokens=("ros", "roboplan"),
        lane_configurations={"bindings_release": ("Release", "none")},
        dependency_projects=dependency_projects,
        module_linked_archive_owners=selected_archive_owners,
        pybind11_include_root=pybind11_include_root,
        pybind11_required_headers=((pybind11_header, _sha256(pybind11_header)),),
    )


def test_fake_binding_contract_separates_full_catalog_from_selected_archives(
    tmp_path: Path,
) -> None:
    contract = _fake_binding_contract(tmp_path / "repository")

    for project_id, project in contract.dependency_projects.items():
        full_catalog = set(project.production_targets)
        selected_archives = set(project.module_linked_archive_targets)
        assert full_catalog == set(
            _FAKE_BINDING_FULL_PRODUCTION_TARGETS[project_id]
        )
        assert selected_archives == set(
            _FAKE_BINDING_SELECTED_ARCHIVE_TARGETS[project_id]
        )
        assert selected_archives.issubset(full_catalog)

    assert (
        set(contract.dependency_projects["manyforge_deployment"].production_targets)
        - set(
            contract.dependency_projects[
                "manyforge_deployment"
            ].module_linked_archive_targets
        )
        == {"manyforge_deployment_admission"}
    )
    assert contract.module_linked_archive_owners == frozenset(
        (project_id, target)
        for project_id, project in contract.dependency_projects.items()
        for target in project.module_linked_archive_targets
    )


def _run_binding_until_copy_validation(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    *,
    staged_bytes: bytes,
    generated_name: str,
    generated_bytes: bytes,
    supplied_stage: str | None = None,
) -> None:
    repository, output, roots = _roots(tmp_path)
    contract = _fake_binding_contract(repository)
    overall = output / "build/bindings-release"
    build_roots = {
        "manyforge_core": overall / "core",
        "manyforge_deployment": overall / "deployment",
        "manyforge_core_py": overall / "module",
    }
    for root in build_roots.values():
        root.mkdir(parents=True)
    built_bytes = b"\x7fELFqualified-module"
    built = _write(build_roots["manyforge_core_py"] / contract.artifact, built_bytes)
    stage = output / "bindings-release/staged"
    staged = _write(stage / contract.artifact.name, staged_bytes)
    _write(output / "generated" / generated_name, generated_bytes)

    builds: dict[str, provenance._Build] = {}
    for project_id, root in build_roots.items():
        targets: dict[str, provenance._TargetReply] = {}
        if project_id == "manyforge_core_py":
            targets["manyforge_core_py"] = provenance._TargetReply(
                name="manyforge_core_py",
                target_type="MODULE_LIBRARY",
                artifacts=(built,),
                sources=contract.registration_sources,
                reply_path=root / "target.json",
                reply_sha256="0" * 64,
            )
            evidence: dict[str, Any] = {
                "project_id": project_id,
                "configured_target_graph": {
                    "owner_node_aliases": [
                        {
                            "alias": f"<owner:{owner_project}/{owner_target}>"
                        }
                        for owner_project, owner_target in sorted(
                            contract.module_linked_archive_owners
                        )
                    ]
                },
            }
        else:
            project = contract.dependency_projects[project_id]
            for target_name, spec in project.production_targets.items():
                artifact = _write(
                    root / spec.artifact,
                    f"!<arch>\n{project_id}/{target_name}\n".encode(),
                )
                targets[target_name] = provenance._TargetReply(
                    name=target_name,
                    target_type=spec.target_type,
                    artifacts=(artifact,),
                    sources=(),
                    reply_path=root / f"target-{target_name}.json",
                    reply_sha256="0" * 64,
                )
            evidence = {"project_id": project_id}
        builds[project_id] = provenance._Build(
            lane_id="bindings_release",
            project_id=project_id,
            root=root,
            source_root=(
                contract.source_root
                if project_id == "manyforge_core_py"
                else contract.dependency_projects[project_id].source_root
            ),
            compiler_path="/usr/bin/c++",
            archiver_path="/usr/bin/ar",
            targets=targets,
            compile_commands={},
            configure_inputs={},
            configure_generated_contracts={},
            qualified_install_inputs={},
            evidence=evidence,
        )

    monkeypatch.setattr(
        provenance,
        "_binding_build_contract",
        lambda *args, **kwargs: contract,
    )
    monkeypatch.setattr(
        provenance,
        "_load_build",
        lambda *, project_id, **kwargs: builds[project_id],
    )
    monkeypatch.setattr(
        provenance,
        "_configure_generated_input_contracts",
        lambda *args, **kwargs: {},
    )
    monkeypatch.setattr(
        provenance,
        "_utility_target_contracts",
        lambda *args, **kwargs: {},
    )
    monkeypatch.setattr(
        provenance,
        "_production_role_catalog",
        lambda *args, **kwargs: frozenset(
            (project_id, target)
            for project_id, project in contract.dependency_projects.items()
            for target in project.production_targets
        ),
    )

    def prove_spec(
        *,
        build: provenance._Build,
        spec: provenance._ProductTargetSpec,
        **kwargs: Any,
    ) -> dict[str, Any]:
        artifact = build.root / spec.artifact
        return {
            "project_id": build.project_id,
            "target": spec.name,
            "target_type": spec.target_type,
            "artifact": {
                "path": roots.logical(artifact),
                "resolved_path": roots.logical(artifact),
                "sha256": _sha256(artifact),
            },
            "authenticated_output_dependencies": [],
        }

    monkeypatch.setattr(
        provenance,
        "_prove_spec",
        prove_spec,
    )
    monkeypatch.setattr(
        provenance,
        "_require_production_test_free",
        lambda rows, exclusions: None,
    )

    def bind_owners(*, proofs: list[dict[str, Any]], **kwargs: Any) -> None:
        proofs[-1]["authenticated_output_dependencies"] = [
            {
                "path": roots.logical(artifact),
                "resolved_path": roots.logical(artifact),
                "sha256": _sha256(artifact),
                "owner": {
                    "kind": "file_api_ninja_target",
                    "project_id": project_id,
                    "target": target,
                    "path": roots.logical(artifact),
                    "resolved_path": roots.logical(artifact),
                    "sha256": _sha256(artifact),
                },
            }
            for project_id, target in sorted(contract.module_linked_archive_owners)
            for artifact in [
                builds[project_id].root
                / contract.dependency_projects[project_id].production_targets[
                    target
                ].artifact
            ]
        ]

    monkeypatch.setattr(provenance, "_bind_output_dependency_owners", bind_owners)
    lane = {
        "lane_id": "bindings_release",
        "build_root": "build/bindings-release",
        "core_build_root": "build/bindings-release/core",
        "deployment_build_root": "build/bindings-release/deployment",
        "module_build_root": "build/bindings-release/module",
        "stage_root": "bindings-release/staged",
        "generated_extension": f"generated/{generated_name}",
        "provenance_output": "bindings-release/build-provenance.json",
        "sanitizer": "none",
    }
    provenance.validate_binding_build_provenance(
        repository,
        output,
        lane,
        supplied_stage or staged.as_posix(),
        BUILD_PROVENANCE_CONTRACT_VALUE,
    )


@pytest.mark.parametrize(
    ("staged_bytes", "generated_name", "generated_bytes", "message"),
    [
        (
            b"\x7fELFstale-stage",
            "manyforge_core_py.cpython-312-x86_64-linux-gnu.so",
            b"\x7fELFqualified-module",
            "staged manyforge_core_py differs",
        ),
        (
            b"\x7fELFqualified-module",
            "relabeled_module.so",
            b"\x7fELFqualified-module",
            "generated manyforge_core_py is not the byte-identical",
        ),
        (
            b"\x7fELFqualified-module",
            "manyforge_core_py.cpython-312-x86_64-linux-gnu.so",
            b"\x7fELFstale-generated",
            "generated manyforge_core_py is not the byte-identical",
        ),
    ],
    ids=["stage-mismatch", "relabeled-generated-elf", "generated-byte-mismatch"],
)
def test_binding_stage_and_generated_extension_must_be_exact_build_copies(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    staged_bytes: bytes,
    generated_name: str,
    generated_bytes: bytes,
    message: str,
) -> None:
    with pytest.raises(provenance.NativeBuildProvenanceError, match=message):
        _run_binding_until_copy_validation(
            tmp_path,
            monkeypatch,
            staged_bytes=staged_bytes,
            generated_name=generated_name,
            generated_bytes=generated_bytes,
        )


def test_binding_stage_argument_must_name_the_frozen_lane_stage(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="staged_extension differs from the binding lane stage contract",
    ):
        _run_binding_until_copy_validation(
            tmp_path,
            monkeypatch,
            staged_bytes=b"\x7fELFqualified-module",
            generated_name="manyforge_core_py.cpython-312-x86_64-linux-gnu.so",
            generated_bytes=b"\x7fELFqualified-module",
            supplied_stage=(tmp_path / "wrong-stage/module.so").as_posix(),
        )


def test_binding_module_file_api_artifact_cannot_be_relabelled(
    tmp_path: Path,
) -> None:
    repository, output, roots = _roots(tmp_path)
    source_root = repository / "manyforge_core/python"
    source = _write(source_root / "bindings.cpp")
    build_root = output / "build/module"
    expected = _write(
        build_root / "manyforge_core_py.cpython-312-x86_64-linux-gnu.so",
        b"\x7fELFsame",
    )
    relabelled = _write(build_root / "copied_module.so", expected.read_bytes())
    target = provenance._TargetReply(
        name="manyforge_core_py",
        target_type="MODULE_LIBRARY",
        artifacts=(relabelled,),
        sources=(source,),
        reply_path=build_root / "target.json",
        reply_sha256="0" * 64,
    )
    build = provenance._Build(
        lane_id="bindings_release",
        project_id="manyforge_core_py",
        root=build_root,
        source_root=source_root,
        compiler_path="/usr/bin/c++",
        archiver_path="/usr/bin/ar",
        targets={"manyforge_core_py": target},
        compile_commands={},
        configure_inputs={},
        configure_generated_contracts={},
        qualified_install_inputs={},
        evidence={},
    )

    with pytest.raises(
        provenance.NativeBuildProvenanceError,
        match="artifact mapping is not exact",
    ):
        provenance._target_reply(
            build,
            "manyforge_core_py",
            expected,
            {source},
            roots,
            "binding_module",
        )


def _binding_host_reduced_contract() -> tuple[dict[str, Any], dict[str, Any]]:
    module_name = verifier.BINDING_EXTENSION_NAME
    core_targets = [
        {
            "name": "core_catalog_only",
            "type": "STATIC_LIBRARY",
            "artifact": "libcore_catalog_only.a",
            "required_sources": ["manyforge_core/src/core_catalog_only.cpp"],
            "required_generated_sources": [],
        },
        {
            "name": "core_selected",
            "type": "STATIC_LIBRARY",
            "artifact": "libcore_selected.a",
            "required_sources": ["manyforge_core/src/core_selected.cpp"],
            "required_generated_sources": [],
        },
    ]
    deployment_targets = [
        {
            "name": "deployment_selected",
            "type": "STATIC_LIBRARY",
            "artifact": "libdeployment_selected.a",
            "required_sources": [
                "manyforge_deployment/src/deployment_selected.cpp"
            ],
            "required_generated_sources": [],
        }
    ]
    selected_owners = [
        {"project_id": "manyforge_core", "target": "core_selected"},
        {
            "project_id": "manyforge_deployment",
            "target": "deployment_selected",
        },
    ]
    pybind_root = "/usr/local/lib/python3.12/site-packages/pybind11/include"
    contract = {
        "projects": {
            "manyforge_core": {"targets": core_targets},
            "manyforge_deployment": {"targets": deployment_targets},
        },
        "configure_generated_input_contracts": [
            {
                "owner_project_id": "manyforge_deployment",
                "output_name": "unused_binding_fixture.hpp",
                "allowed_build_relative_paths": [
                    "generated/unused_binding_fixture.hpp"
                ],
                "template_repository_path": (
                    "manyforge_deployment/src/unused_binding_fixture.hpp.in"
                ),
                "semantic_input_policy": (
                    "all_authenticated_cmake_inputs_and_exact_cache"
                ),
                "required_semantic_repository_paths": [],
            }
        ],
        "binding_builder_constraints": {
            "build_recipe": "manyforge_core/python/CMakeLists.txt",
            "target": {
                "file_api_type": "MODULE_LIBRARY",
                "artifact_relative_path": module_name,
                "required_translation_units": [
                    "manyforge_core/python/bindings.cpp"
                ],
                "required_header_dependencies": [
                    "manyforge_core/python/binding_registrations.hpp"
                ],
                "required_upstream_archive_owners": selected_owners,
            },
            "pybind11_build_provenance": {
                "version": "3.0.3",
                "include_root": pybind_root,
                "required_public_headers": [
                    {
                        "path": f"{pybind_root}/pybind11/pybind11.h",
                        "sha256": "9" * 64,
                    }
                ],
            },
        },
        "binding_build_contract": {
            "dependency_projects": {
                "manyforge_core": {
                    "module_linked_archive_targets": ["core_selected"]
                },
                "manyforge_deployment": {
                    "module_linked_archive_targets": [
                        "deployment_selected"
                    ]
                },
            }
        },
    }
    row = {
        "lane_id": "bindings_host_fixture",
        "sanitizer": "none",
        "build_root": "binding-host/build",
        "provenance_output": "binding-host/build-provenance.json",
        "stage_root": "binding-host/staged",
        "core_build_root": "binding-host/core",
        "deployment_build_root": "binding-host/deployment",
        "module_build_root": "binding-host/module",
        "generated_extension": f"binding-host/generated/{module_name}",
    }
    return contract, row


def _binding_host_lane_file(
    lane_root: Path, logical_path: str, value: bytes
) -> Path:
    return _write(
        lane_root / Path(logical_path).relative_to("/qualification-output"),
        value,
    )


def _binding_host_repository_dependency(
    repository_root: Path,
    repository_relative: str,
) -> tuple[dict[str, Any], dict[str, Any]]:
    physical = repository_root / repository_relative
    logical = f"/workspace/{repository_relative}"
    digest = _sha256(physical)
    raw = {
        "path": logical,
        "resolved_path": logical,
        "symlink_chain": [logical],
        "sha256": digest,
        "cross_profile_sha256": digest,
        "scope": "repository_source",
        "producer": None,
        "source_identity": {
            "git_root": "/workspace",
            "path": repository_relative,
            "mode": "100644",
            "blob": _git_blob(physical),
        },
    }
    projected = {
        "path": logical,
        "sha256": digest,
        "scope": "repository_source",
        "producer_kind": None,
        "producer_project": None,
        "producer_target": None,
        "producer_inputs_sha256": None,
    }
    return raw, projected


def _binding_host_target_fixture(
    *,
    lane_root: Path,
    repository_root: Path,
    project_id: str,
    target_name: str,
    role: str,
    target_type: str,
    source_relative: str,
    build_root: str,
    artifact_relative: str,
    final_command_kind: str,
    artifact_bytes: bytes,
    extra_dependencies: list[tuple[dict[str, Any], dict[str, Any]]] | None = None,
    authenticated_output_dependencies: list[dict[str, Any]] | None = None,
) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any]]:
    source = _write(
        repository_root / source_relative,
        f"// {project_id}:{target_name}\n".encode(),
    )
    source_raw, source_projected = _binding_host_repository_dependency(
        repository_root, source_relative
    )
    object_relative = f"CMakeFiles/{target_name}.dir/{source.name}.o"
    object_logical = f"{build_root}/{object_relative}"
    object_file = _binding_host_lane_file(
        lane_root,
        object_logical,
        f"\x7fELFobject:{project_id}:{target_name}\n".encode(),
    )
    artifact_logical = f"{build_root}/{artifact_relative}"
    if final_command_kind == "archive":
        artifact = _write_regular_archive(
            lane_root / Path(artifact_logical).relative_to("/qualification-output"),
            [(object_file.name, object_file.read_bytes())],
        )
    else:
        artifact = _binding_host_lane_file(
            lane_root, artifact_logical, artifact_bytes
        )
    reply_logical = (
        f"{build_root}/.cmake/api/v1/reply/target-{target_name}.json"
    )
    reply = _binding_host_lane_file(
        lane_root,
        reply_logical,
        f'{{"name":"{target_name}"}}\n'.encode(),
    )
    closure_pairs = [(source_raw, source_projected), *(extra_dependencies or [])]
    closure_files = sorted(
        (raw for raw, _projected in closure_pairs),
        key=lambda dependency: dependency["path"],
    )
    closure_raw_digest = hashlib.sha256(
        f"closure:{project_id}:{target_name}".encode()
    ).hexdigest()
    release_sanitizer = {
        "effective_sanitizers": [],
        "effective_frame_pointer": None,
        "disable_flags": [],
    }
    object_input_evidence = {
        "path": object_logical,
        "resolved_path": object_logical,
        "symlink_chain": [object_logical],
        "sha256": _sha256(object_file),
        "scope": "selected_build",
        "kind": "object",
    }
    compile_command = {
        "kind": "compile",
        "directory": build_root,
        "output": object_logical,
        "output_sha256": _sha256(object_file),
        "inputs": [],
        "input_evidence": [],
        "auxiliary_outputs": [],
        "source": {
            "path": source_raw["path"],
            "sha256": source_raw["sha256"],
        },
        "dependency_closure": {
            "object": object_logical,
            "declared_dependency_count": len(closure_files),
            "raw_sha256": closure_raw_digest,
            "files": closure_files,
        },
        "argv_sha256": hashlib.sha256(
            f"compile-raw:{project_id}:{target_name}".encode()
        ).hexdigest(),
        "parity_argv_sha256": hashlib.sha256(
            f"compile-parity:{project_id}:{target_name}".encode()
        ).hexdigest(),
        "cross_profile_argv_sha256": hashlib.sha256(
            f"compile-cross:{project_id}:{target_name}".encode()
        ).hexdigest(),
        "sanitizer": release_sanitizer,
    }
    final_input_evidence = [object_input_evidence]
    for dependency in authenticated_output_dependencies or []:
        final_input_evidence.append(
            {
                "path": dependency["path"],
                "resolved_path": dependency["resolved_path"],
                "symlink_chain": [dependency["path"]],
                "sha256": dependency["sha256"],
                "scope": "qualification_output",
                "kind": "archive",
            }
        )
    final_input_evidence.sort(
        key=lambda row: (row["path"], row["sha256"])
    )
    final_auxiliary_outputs: list[dict[str, str]] = []
    if final_command_kind == "link":
        map_logical = f"{build_root}/CMakeFiles/{target_name}.map"
        map_file = _binding_host_lane_file(
            lane_root,
            map_logical,
            f"map:{project_id}:{target_name}\n".encode(),
        )
        final_auxiliary_outputs.append(
            {"path": map_logical, "sha256": _sha256(map_file)}
        )
    final_command = {
        "kind": final_command_kind,
        "directory": build_root,
        "output": artifact_logical,
        "output_sha256": _sha256(artifact),
        "inputs": sorted(row["path"] for row in final_input_evidence),
        "input_evidence": final_input_evidence,
        "auxiliary_outputs": final_auxiliary_outputs,
        "source": None,
        "dependency_closure": None,
        "argv_sha256": hashlib.sha256(
            f"{final_command_kind}-raw:{project_id}:{target_name}".encode()
        ).hexdigest(),
        "parity_argv_sha256": hashlib.sha256(
            f"{final_command_kind}-parity:{project_id}:{target_name}".encode()
        ).hexdigest(),
        "cross_profile_argv_sha256": hashlib.sha256(
            f"{final_command_kind}-cross:{project_id}:{target_name}".encode()
        ).hexdigest(),
        "sanitizer": (
            release_sanitizer if final_command_kind == "link" else None
        ),
    }
    proof: dict[str, Any] = {
        "project_id": project_id,
        "build_root": build_root,
        "target": target_name,
        "role": role,
        "target_type": target_type,
        "contract_artifact": artifact_relative,
        "artifact": {
            "path": artifact_logical,
            "resolved_path": artifact_logical,
            "sha256": _sha256(artifact),
        },
        "file_api_target_reply": {
            "path": reply_logical,
            "sha256": _sha256(reply),
        },
        "file_api_sources": [
            {
                "path": source_raw["path"],
                "sha256": source_raw["sha256"],
                "cross_profile_sha256": source_raw["cross_profile_sha256"],
            }
        ],
        "ninja_query": [
            {
                "node": artifact_relative,
                "rule": final_command_kind.upper(),
                "edges": [
                    {"type": "explicit", "path": object_logical}
                ],
                "raw_sha256": hashlib.sha256(
                    f"query:{project_id}:{target_name}".encode()
                ).hexdigest(),
            }
        ],
        "ninja_commands_sha256": hashlib.sha256(
            f"commands:{project_id}:{target_name}".encode()
        ).hexdigest(),
        "generated_source_producers": [],
        "commands": sorted(
            [compile_command, final_command], key=lambda command: command["output"]
        ),
        "archives": (
            [
                {
                    "path": artifact_logical,
                    "sha256": _sha256(artifact),
                    "members": [
                        {
                            "name": object_file.name,
                            "sha256": _sha256(object_file),
                            "producer_object": object_logical,
                        }
                    ],
                }
            ]
            if final_command_kind == "archive"
            else []
        ),
        "transitive_local_artifacts": sorted(
            [artifact_logical, object_logical]
        ),
        "authenticated_local_target_dependencies": [],
        "transitive_external_link_inputs": [],
        "required_generated_inputs": [],
        "authenticated_output_dependencies": (
            authenticated_output_dependencies or []
        ),
    }
    normalized_object = f"<build:{project_id}>/{object_relative}"
    normalized_artifact = f"<build:{project_id}>/{artifact_relative}"
    command_semantics = sorted(
        [
            {
                "kind": "compile",
                "output": normalized_object,
                "argv_sha256": compile_command["parity_argv_sha256"],
                "cross_profile_argv_sha256": compile_command[
                    "cross_profile_argv_sha256"
                ],
            },
            {
                "kind": final_command_kind,
                "output": normalized_artifact,
                "argv_sha256": final_command["parity_argv_sha256"],
                "cross_profile_argv_sha256": final_command[
                    "cross_profile_argv_sha256"
                ],
            },
        ],
        key=lambda command: (
            command["kind"],
            command["output"],
            command["argv_sha256"],
        ),
    )
    projection = {
        "project_id": project_id,
        "target": target_name,
        "target_type": target_type,
        "artifact_relative": artifact_relative,
        "artifact_sha256": _sha256(artifact),
        "output_inventory_cross_profile_sha256": None,
        "direct_sources": [
            {"path": source_raw["path"], "sha256": source_raw["sha256"]}
        ],
        "closure_sources": [
            {"path": source_raw["path"], "sha256": source_raw["sha256"]}
        ],
        "compile_dependencies": sorted(
            (projected for _raw, projected in closure_pairs),
            key=lambda dependency: dependency["path"],
        ),
        "command_semantics": command_semantics,
    }
    return proof, projection, compile_command


def _binding_host_artifact_fixture(tmp_path: Path) -> dict[str, Any]:
    contract, row = _binding_host_reduced_contract()
    repository_root = tmp_path / "repository"
    lane_root = tmp_path / "lane"
    repository_root.mkdir()
    lane_root.mkdir()
    recipe_relative = contract["binding_builder_constraints"]["build_recipe"]
    recipe = _write(
        repository_root / recipe_relative,
        b"cmake_minimum_required(VERSION 3.25)\n",
    )
    target_contract = contract["binding_builder_constraints"]["target"]
    header_relative = target_contract["required_header_dependencies"][0]
    header = _write(repository_root / header_relative, b"#pragma once\n")

    dependency_definitions = [
        (
            "manyforge_core",
            contract["projects"]["manyforge_core"]["targets"][0],
            f"/qualification-output/{row['core_build_root']}",
        ),
        (
            "manyforge_core",
            contract["projects"]["manyforge_core"]["targets"][1],
            f"/qualification-output/{row['core_build_root']}",
        ),
        (
            "manyforge_deployment",
            contract["projects"]["manyforge_deployment"]["targets"][0],
            f"/qualification-output/{row['deployment_build_root']}",
        ),
    ]
    dependency_proofs: list[dict[str, Any]] = []
    dependency_projections: list[dict[str, Any]] = []
    for project_id, spec, build_root in dependency_definitions:
        proof, projection, _compile = _binding_host_target_fixture(
            lane_root=lane_root,
            repository_root=repository_root,
            project_id=project_id,
            target_name=spec["name"],
            role="production_dependency",
            target_type=spec["type"],
            source_relative=spec["required_sources"][0],
            build_root=build_root,
            artifact_relative=spec["artifact"],
            final_command_kind="archive",
            artifact_bytes=f"!<arch>\n{project_id}:{spec['name']}\n".encode(),
        )
        dependency_proofs.append(proof)
        dependency_projections.append(projection)
    dependency_proofs.sort(
        key=lambda proof: (proof["project_id"], proof["target"])
    )
    dependency_projections.sort(
        key=lambda projection: (
            projection["project_id"],
            projection["target"],
        )
    )
    proof_index = {
        (proof["project_id"], proof["target"]): proof
        for proof in dependency_proofs
    }
    selected_keys = [
        ("manyforge_core", "core_selected"),
        ("manyforge_deployment", "deployment_selected"),
    ]
    selected_owners: list[dict[str, Any]] = []
    for project_id, target_name in selected_keys:
        artifact = proof_index[(project_id, target_name)]["artifact"]
        selected_owners.append(
            {
                "kind": "file_api_ninja_target",
                "project_id": project_id,
                "target": target_name,
                "path": artifact["path"],
                "resolved_path": artifact["resolved_path"],
                "sha256": artifact["sha256"],
            }
        )
    selected_owners.sort(
        key=lambda owner: (owner["project_id"], owner["target"])
    )
    authenticated_output_dependencies = [
        {
            "path": owner["path"],
            "resolved_path": owner["resolved_path"],
            "sha256": owner["sha256"],
            "owner": owner,
        }
        for owner in selected_owners
    ]

    header_raw, header_projected = _binding_host_repository_dependency(
        repository_root, header_relative
    )
    pybind_contract = contract["binding_builder_constraints"][
        "pybind11_build_provenance"
    ]
    public_header = pybind_contract["required_public_headers"][0]
    pybind_raw = {
        "path": public_header["path"],
        "resolved_path": public_header["path"],
        "symlink_chain": [public_header["path"]],
        "sha256": public_header["sha256"],
        "cross_profile_sha256": public_header["sha256"],
        "scope": "pinned_container",
        "producer": None,
        "source_identity": None,
    }
    pybind_projected = {
        "path": public_header["path"],
        "sha256": public_header["sha256"],
        "scope": "pinned_container",
        "producer_kind": None,
        "producer_project": None,
        "producer_target": None,
        "producer_inputs_sha256": None,
    }
    module_bytes = b"\x7fELFbinding-host-module\n"
    module_proof, module_projection, module_compile = (
        _binding_host_target_fixture(
            lane_root=lane_root,
            repository_root=repository_root,
            project_id="manyforge_core_py",
            target_name="manyforge_core_py",
            role="binding_module",
            target_type=target_contract["file_api_type"],
            source_relative=target_contract["required_translation_units"][0],
            build_root=f"/qualification-output/{row['module_build_root']}",
            artifact_relative=target_contract["artifact_relative_path"],
            final_command_kind="link",
            artifact_bytes=module_bytes,
            extra_dependencies=[
                (header_raw, header_projected),
                (pybind_raw, pybind_projected),
            ],
            authenticated_output_dependencies=authenticated_output_dependencies,
        )
    )
    module_artifact = module_proof["artifact"]
    staged_path = (
        f"/qualification-output/{row['stage_root']}/"
        f"{verifier.BINDING_EXTENSION_NAME}"
    )
    generated_path = f"/qualification-output/{row['generated_extension']}"
    staged = _binding_host_lane_file(lane_root, staged_path, module_bytes)
    generated = _binding_host_lane_file(lane_root, generated_path, module_bytes)
    expected_header = {
        "path": f"/workspace/{header_relative}",
        "sha256": _sha256(header),
    }
    pybind_closure = [pybind_raw]
    closure = module_compile["dependency_closure"]
    registration_header_dependencies = [
        {
            "source": module_compile["source"]["path"],
            "object": closure["object"],
            "dependency_count": len(closure["files"]),
            "registration_header": expected_header,
            "pybind11": {
                "version": pybind_contract["version"],
                "include_root": pybind_contract["include_root"],
                "required_public_headers": pybind_contract[
                    "required_public_headers"
                ],
                "header_closure_count": len(pybind_closure),
                "header_closure_sha256": verifier._canonical_json_sha256(
                    pybind_closure
                ),
            },
            "raw_sha256": closure["raw_sha256"],
        }
    ]
    target_projection = sorted(
        [*dependency_projections, module_projection],
        key=lambda projection: (
            projection["project_id"],
            projection["target"],
        ),
    )
    evidence: dict[str, Any] = {
        "schema_version": (
            "manyforge.composable_cell_i2.binding_build_provenance.v1"
        ),
        "lane_id": row["lane_id"],
        "configuration": "Release",
        "sanitizer": "none",
        "build_root": f"/qualification-output/{row['build_root']}",
        "provenance_output": (
            f"/qualification-output/{row['provenance_output']}"
        ),
        "projects": [
            {
                "project_id": "manyforge_core",
                "source_git_generation": {"git_root": "/workspace"},
            },
            {
                "project_id": "manyforge_core_py",
                "source_git_generation": {"git_root": "/workspace"},
            },
            {
                "project_id": "manyforge_deployment",
                "source_git_generation": {"git_root": "/workspace"},
            },
        ],
        "module_target": module_proof,
        "dependency_targets": dependency_proofs,
        "dependency_utility_target_relationships": [],
        "module_linked_archive_owners": selected_owners,
        "staged_extension": {
            "path": staged_path,
            "resolved_path": staged_path,
            "sha256": _sha256(staged),
        },
        "generated_extension": {
            "path": generated_path,
            "resolved_path": generated_path,
            "sha256": _sha256(generated),
        },
        "registration_header_dependencies": (
            registration_header_dependencies
        ),
        "elf": {},
        "ros_free_closure": {},
        "deterministic_parity": {
            "configuration": "Release",
            "sanitizer": "none",
            "artifact_sha256": module_artifact["sha256"],
            "build_recipe": {
                "path": f"/workspace/{recipe_relative}",
                "sha256": _sha256(recipe),
            },
            "target_projection": target_projection,
            "registration_header": expected_header,
        },
        "response_files": [],
        "inspection_tools": [],
        "passed": True,
    }
    evidence["evidence_sha256"] = verifier._canonical_json_sha256(evidence)
    evidence_path = _write(
        lane_root / row["provenance_output"],
        json.dumps(evidence, sort_keys=True).encode(),
    )
    return {
        "path": evidence_path,
        "lane_root": lane_root,
        "repository_root": repository_root,
        "row": row,
        "build_contract": contract,
        "evidence": evidence,
        "critical_paths": {
            module_artifact["path"],
            staged_path,
            generated_path,
            *(proof["artifact"]["path"] for proof in dependency_proofs),
            *(
                source["path"]
                for proof in [*dependency_proofs, module_proof]
                for source in proof["file_api_sources"]
            ),
            *(
                command["output"]
                for proof in [*dependency_proofs, module_proof]
                for command in proof["commands"]
                if command["kind"] == "compile"
            ),
            *(
                auxiliary["path"]
                for proof in [*dependency_proofs, module_proof]
                for command in proof["commands"]
                for auxiliary in command["auxiliary_outputs"]
            ),
            f"/workspace/{header_relative}",
            module_compile["source"]["path"],
        },
    }


def _validate_binding_target_command_fixture(
    fixture: dict[str, Any],
    *,
    extra_output_owners: tuple[dict[str, Any], ...] = (),
) -> None:
    evidence = fixture["evidence"]
    row = fixture["row"]
    build_roots = {
        "manyforge_core": (
            f"/qualification-output/{row['core_build_root']}"
        ),
        "manyforge_deployment": (
            f"/qualification-output/{row['deployment_build_root']}"
        ),
        "manyforge_core_py": (
            f"/qualification-output/{row['module_build_root']}"
        ),
    }
    output_owners = [
        *evidence["dependency_targets"],
        *extra_output_owners,
        evidence["module_target"],
    ]
    verifier._validate_target_command_file_evidence(
        evidence["dependency_targets"],
        project_build_roots=build_roots,
        target_output_owners=output_owners,
        installed_output_owners=[],
        expected_roles=frozenset({"production_dependency"}),
        sanitizer_profile="none",
        lane_root=fixture["lane_root"],
        repository_root=fixture["repository_root"],
        label="synthetic binding dependency command evidence",
    )
    verifier._validate_target_command_file_evidence(
        [evidence["module_target"]],
        project_build_roots=build_roots,
        target_output_owners=output_owners,
        installed_output_owners=[],
        expected_roles=frozenset({"binding_module"}),
        sanitizer_profile="none",
        lane_root=fixture["lane_root"],
        repository_root=fixture["repository_root"],
        label="synthetic binding module command evidence",
    )


def test_host_accepts_exact_local_library_material_envelope(
    tmp_path: Path,
) -> None:
    repository_root = tmp_path / "repository"
    lane_root = tmp_path / "lane"
    repository_root.mkdir()
    lane_root.mkdir()
    build_root = "/qualification-output/native/build/roboplan"
    proof, _projection, _compile = _binding_host_target_fixture(
        lane_root=lane_root,
        repository_root=repository_root,
        project_id="roboplan",
        target_name="roboplan",
        role="local_library",
        target_type="SHARED_LIBRARY",
        source_relative="roboplan/src/roboplan.cpp",
        build_root=build_root,
        artifact_relative="libroboplan.so",
        final_command_kind="link",
        artifact_bytes=b"\x7fELFsynthetic local roboplan library\n",
    )
    proof["elf"] = {
        "path": proof["artifact"]["path"],
        "resolved_path": proof["artifact"]["resolved_path"],
        "sha256": proof["artifact"]["sha256"],
        "readelf_header_sha256": "1" * 64,
        "readelf_program_headers_sha256": "2" * 64,
        "readelf_dynamic_sha256": "3" * 64,
        "readelf_symbols_sha256": "4" * 64,
        "ldd_normalized_sha256": "5" * 64,
        "needed": [],
        "python_init_symbols": [],
        "dependencies": [],
    }

    assert proof["required_generated_inputs"] == []
    verifier._validate_target_command_file_evidence(
        [proof],
        project_build_roots={"roboplan": build_root},
        target_output_owners=[proof],
        installed_output_owners=[],
        expected_roles=frozenset({"local_library"}),
        sanitizer_profile="none",
        lane_root=lane_root,
        repository_root=repository_root,
        label="synthetic local-library command evidence",
    )


def test_host_accepts_exact_gtest_material_envelope(tmp_path: Path) -> None:
    repository_root = tmp_path / "repository"
    lane_root = tmp_path / "lane"
    repository_root.mkdir()
    lane_root.mkdir()
    build_root = "/qualification-output/native/build/manyforge_deployment"
    proof, _projection, _compile = _binding_host_target_fixture(
        lane_root=lane_root,
        repository_root=repository_root,
        project_id="manyforge_deployment",
        target_name="test_compiler",
        role="gtest",
        target_type="EXECUTABLE",
        source_relative="manyforge_deployment/test/test_compiler.cpp",
        build_root=build_root,
        artifact_relative="test_compiler",
        final_command_kind="link",
        artifact_bytes=b"\x7fELFsynthetic compiler GTest\n",
    )
    artifact_path = lane_root / Path(proof["artifact"]["path"]).relative_to(
        "/qualification-output"
    )
    artifact_path.chmod(0o755)
    proof.update(
        {
            "registered_test_count": 1,
            "registered_test_names_sha256": "6" * 64,
            "contracted_test_count": 1,
            "contracted_test_names_sha256": "6" * 64,
            "additional_test_count": 0,
            "elf": {
                "path": proof["artifact"]["path"],
                "resolved_path": proof["artifact"]["resolved_path"],
                "sha256": proof["artifact"]["sha256"],
                "readelf_header_sha256": "1" * 64,
                "readelf_program_headers_sha256": "2" * 64,
                "readelf_dynamic_sha256": "3" * 64,
                "readelf_symbols_sha256": "4" * 64,
                "ldd_normalized_sha256": "5" * 64,
                "needed": [],
                "python_init_symbols": [],
                "dependencies": [],
            },
        }
    )

    verifier._validate_target_command_file_evidence(
        [proof],
        project_build_roots={"manyforge_deployment": build_root},
        target_output_owners=[proof],
        installed_output_owners=[],
        expected_roles=frozenset({"gtest"}),
        sanitizer_profile="none",
        lane_root=lane_root,
        repository_root=repository_root,
        label="synthetic GTest command evidence",
    )


@pytest.mark.parametrize(
    "field",
    [
        "directory",
        "output_sha256",
        "inputs",
        "input_evidence",
        "auxiliary_outputs",
        "sanitizer",
    ],
)
def test_host_rejects_incomplete_material_command_envelope(
    tmp_path: Path, field: str
) -> None:
    fixture = _binding_host_artifact_fixture(tmp_path)
    module_link = next(
        command
        for command in fixture["evidence"]["module_target"]["commands"]
        if command["kind"] == "link"
    )
    del module_link[field]

    with pytest.raises(verifier.ManifestError, match="keys differ"):
        _validate_binding_target_command_fixture(fixture)


def test_host_rejects_command_output_byte_drift(tmp_path: Path) -> None:
    fixture = _binding_host_artifact_fixture(tmp_path)
    compile_command = next(
        command
        for command in fixture["evidence"]["module_target"]["commands"]
        if command["kind"] == "compile"
    )
    physical = fixture["lane_root"] / Path(
        compile_command["output"]
    ).relative_to("/qualification-output")
    physical.write_bytes(b"changed object bytes\n")

    with pytest.raises(verifier.ManifestError, match="retained output identity"):
        _validate_binding_target_command_fixture(fixture)


def test_host_rejects_one_sided_command_input_scope_relabel(
    tmp_path: Path,
) -> None:
    fixture = _binding_host_artifact_fixture(tmp_path)
    module_link = next(
        command
        for command in fixture["evidence"]["module_target"]["commands"]
        if command["kind"] == "link"
    )
    external_archive = next(
        row
        for row in module_link["input_evidence"]
        if row["scope"] == "qualification_output"
    )
    external_archive["scope"] = "selected_build"

    with pytest.raises(verifier.ManifestError, match="scope/kind identity"):
        _validate_binding_target_command_fixture(fixture)


def test_host_rejects_selected_build_input_without_command_owner(
    tmp_path: Path,
) -> None:
    fixture = _binding_host_artifact_fixture(tmp_path)
    module_target = fixture["evidence"]["module_target"]
    module_link = next(
        command
        for command in module_target["commands"]
        if command["kind"] == "link"
    )
    selected_object = next(
        row
        for row in module_link["input_evidence"]
        if row["scope"] == "selected_build"
    )
    build_root = module_target["build_root"]
    unowned_logical = f"{build_root}/CMakeFiles/unowned-input.o"
    selected_physical = fixture["lane_root"] / Path(
        selected_object["path"]
    ).relative_to("/qualification-output")
    unowned = _binding_host_lane_file(
        fixture["lane_root"], unowned_logical, selected_physical.read_bytes()
    )
    selected_object.update(
        {
            "path": unowned_logical,
            "resolved_path": unowned_logical,
            "symlink_chain": [unowned_logical],
            "sha256": _sha256(unowned),
        }
    )
    module_link["input_evidence"].sort(
        key=lambda row: (row["path"], row["sha256"])
    )
    module_link["inputs"] = sorted(
        row["path"] for row in module_link["input_evidence"]
    )

    with pytest.raises(verifier.ManifestError, match="no command owner"):
        _validate_binding_target_command_fixture(fixture)


def test_host_rejects_command_directory_outside_project_build(
    tmp_path: Path,
) -> None:
    fixture = _binding_host_artifact_fixture(tmp_path)
    module_compile = next(
        command
        for command in fixture["evidence"]["module_target"]["commands"]
        if command["kind"] == "compile"
    )
    module_compile["directory"] = (
        f"/qualification-output/{fixture['row']['core_build_root']}"
    )

    with pytest.raises(
        verifier.ManifestError, match="output/directory identity"
    ):
        _validate_binding_target_command_fixture(fixture)


def test_host_rejects_command_auxiliary_output_digest_relabel(
    tmp_path: Path,
) -> None:
    fixture = _binding_host_artifact_fixture(tmp_path)
    module_link = next(
        command
        for command in fixture["evidence"]["module_target"]["commands"]
        if command["kind"] == "link"
    )
    module_link["auxiliary_outputs"][0]["sha256"] = "e" * 64

    with pytest.raises(verifier.ManifestError, match="retained bytes"):
        _validate_binding_target_command_fixture(fixture)


def test_host_rejects_ambiguous_cross_build_command_owner(
    tmp_path: Path,
) -> None:
    fixture = _binding_host_artifact_fixture(tmp_path)
    dependency_targets = fixture["evidence"]["dependency_targets"]
    selected = next(
        proof for proof in dependency_targets if proof["target"] == "core_selected"
    )
    alternate_owner = {
        "project_id": "manyforge_core",
        "target": "core_catalog_only",
        "target_type": "STATIC_LIBRARY",
        "artifact": dict(selected["artifact"]),
    }

    with pytest.raises(
        verifier.ManifestError, match="output dependency owner join"
    ):
        _validate_binding_target_command_fixture(
            fixture,
            extra_output_owners=(alternate_owner,),
        )


def test_host_rejects_missing_final_target_command_owner(tmp_path: Path) -> None:
    fixture = _binding_host_artifact_fixture(tmp_path)
    module_target = fixture["evidence"]["module_target"]
    commands = module_target["commands"]
    commands[:] = [command for command in commands if command["kind"] != "link"]
    module_target["authenticated_output_dependencies"] = []

    with pytest.raises(
        verifier.ManifestError, match="final artifact ownership"
    ):
        _validate_binding_target_command_fixture(fixture)


def test_binding_host_validates_complete_raw_artifact_without_mocking_joins(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    fixture = _binding_host_artifact_fixture(tmp_path)
    source_generation_calls: list[dict[str, Any]] = []

    def validate_synthetic_project_generations(
        value: Any, **kwargs: Any
    ) -> None:
        projects = [
            verifier._require_object(raw, "synthetic binding project")
            for raw in verifier._require_list(value, "synthetic binding projects")
        ]
        assert [project["project_id"] for project in projects] == [
            "manyforge_core",
            "manyforge_core_py",
            "manyforge_deployment",
        ]
        row = fixture["row"]
        assert kwargs["expected_projects"] == {
            "manyforge_core": (
                "/workspace/manyforge_core",
                f"/qualification-output/{row['core_build_root']}",
            ),
            "manyforge_deployment": (
                "/workspace/manyforge_deployment",
                f"/qualification-output/{row['deployment_build_root']}",
            ),
            "manyforge_core_py": (
                "/workspace/manyforge_core/python",
                f"/qualification-output/{row['module_build_root']}",
            ),
        }
        assert kwargs["expected_configuration"] == "Release"
        assert kwargs["expected_build_testing"] == {
            "manyforge_core": False,
            "manyforge_deployment": False,
            "manyforge_core_py": False,
        }
        assert kwargs["build_contract"] is fixture["build_contract"]
        assert kwargs["lane_root"] == fixture["lane_root"]
        assert kwargs["repository_root"] == fixture["repository_root"]
        assert kwargs["expected_manyforge_generation"] == {}
        assert kwargs["expected_auxiliary_git_root"] is None
        assert kwargs["expected_auxiliary_capture_path"] is None
        source_generation_calls.append(kwargs)

    monkeypatch.setattr(
        verifier,
        "_validate_project_source_generations",
        validate_synthetic_project_generations,
    )

    evidence, authenticated_files = (
        verifier._validate_binding_build_provenance_artifact(
            fixture["path"],
            lane_root=fixture["lane_root"],
            repository_root=fixture["repository_root"],
            row=fixture["row"],
            build_contract=fixture["build_contract"],
            expected_manyforge_generation={},
        )
    )

    assert evidence == fixture["evidence"]
    assert len(source_generation_calls) == 1
    authenticated_paths = {row["path"] for row in authenticated_files}
    assert fixture["critical_paths"].issubset(authenticated_paths)


def test_binding_host_rejects_output_dependency_relabelled_from_its_owner(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    fixture = _binding_host_artifact_fixture(tmp_path)
    evidence = fixture["evidence"]
    dependency = evidence["module_target"][
        "authenticated_output_dependencies"
    ][0]
    original_dependency_path = dependency["path"]
    alternate = next(
        proof["artifact"]
        for proof in evidence["dependency_targets"]
        if proof["target"] == "core_catalog_only"
    )
    dependency.update(
        {
            "path": alternate["path"],
            "resolved_path": alternate["resolved_path"],
            "sha256": alternate["sha256"],
        }
    )
    module_link = next(
        command
        for command in evidence["module_target"]["commands"]
        if command["kind"] == "link"
    )
    command_dependency = next(
        row
        for row in module_link["input_evidence"]
        if row["path"] == original_dependency_path
    )
    command_dependency.update(
        {
            "path": alternate["path"],
            "resolved_path": alternate["resolved_path"],
            "symlink_chain": [alternate["path"]],
            "sha256": alternate["sha256"],
        }
    )
    module_link["input_evidence"].sort(
        key=lambda row: (row["path"], row["sha256"])
    )
    module_link["inputs"] = sorted(
        row["path"] for row in module_link["input_evidence"]
    )
    evidence.pop("evidence_sha256")
    evidence["evidence_sha256"] = verifier._canonical_json_sha256(evidence)
    fixture["path"].write_text(
        json.dumps(evidence, sort_keys=True), encoding="utf-8"
    )
    monkeypatch.setattr(
        verifier,
        "_validate_project_source_generations",
        lambda _value, **_kwargs: None,
    )

    with pytest.raises(
        verifier.ManifestError,
        match="output dependency owner join",
    ):
        verifier._validate_binding_build_provenance_artifact(
            fixture["path"],
            lane_root=fixture["lane_root"],
            repository_root=fixture["repository_root"],
            row=fixture["row"],
            build_contract=fixture["build_contract"],
            expected_manyforge_generation={},
        )


def test_binding_consumer_witness_cannot_name_a_different_producer(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    artifact_root = tmp_path / "artifacts"
    lane_root = artifact_root / "lanes/bindings_selector"
    lane_root.mkdir(parents=True)
    result_row = {
        "lane_id": "bindings_selector",
        "collection_output": "collection.txt",
        "result_output": "junit.xml",
        "witness_output": "witness.json",
        "producer_lane_id": "bindings_selector",
        "required_case_ids": ["ros_free_import_and_sole_module_inventory"],
        "require_python_baseline": False,
    }
    build_row = {
        "lane_id": "bindings_selector",
        "provenance_output": "build-provenance.json",
    }
    collection = _write(lane_root / result_row["collection_output"])
    junit = _write(lane_root / result_row["result_output"])
    build_provenance_path = _write(lane_root / build_row["provenance_output"], b"{}\n")
    extension = _write(
        lane_root / "bindings-selector/staged" / verifier.BINDING_EXTENSION_NAME,
        b"\x7fELFqualified",
    )
    node = (
        f"{verifier.BINDING_NODE_PREFIX}"
        "ros_free_import_and_sole_module_inventory"
    )
    build_evidence = {
        "evidence_sha256": "1" * 64,
        "staged_extension": {"sha256": _sha256(extension)},
    }
    witness = {
        "schema_version": "manyforge.composable_cell_i2.binding_witness.v1",
        "lane_id": "bindings_selector",
        "framework": "packet-owned-pytest-junit",
        "preparation_argv": verifier.BINDING_LANE_PREPARATION["bindings_selector"],
        "preparation_exit_status": 0,
        "collection_argv": verifier._binding_collection_argv(result_row),
        "collection_sha256": _sha256(collection),
        "junit_sha256": _sha256(junit),
        "child_argv": verifier._binding_run_argv(result_row),
        "child_exit_status": 0,
        "extension_path": (
            "/qualification-output/bindings-selector/staged/"
            f"{verifier.BINDING_EXTENSION_NAME}"
        ),
        "extension_sha256": _sha256(extension),
        "producer_lane_id": "bindings_release",
        "build_provenance_path": build_row["provenance_output"],
        "build_provenance_sha256": _sha256(build_provenance_path),
        "build_provenance_evidence_sha256": build_evidence["evidence_sha256"],
        "case_witnesses": [],
    }
    (lane_root / result_row["witness_output"]).write_text(
        json.dumps(witness), encoding="utf-8"
    )
    manifest = {
        "test_inventory_contract": {
            "binding_result_lanes": [result_row],
            "binding_build_lanes": [build_row],
        }
    }
    monkeypatch.setattr(
        verifier,
        "_load_baseline_test_inventory",
        lambda *args, **kwargs: ({}, {}),
    )
    monkeypatch.setattr(
        verifier,
        "_load_build_provenance_contract",
        lambda *args, **kwargs: BUILD_PROVENANCE_CONTRACT_VALUE,
    )
    monkeypatch.setattr(
        verifier,
        "_validate_binding_build_provenance_artifact",
        lambda *args, **kwargs: (build_evidence, []),
    )
    monkeypatch.setattr(
        verifier,
        "_pytest_collection_nodes",
        lambda *args, **kwargs: {node},
    )
    monkeypatch.setattr(
        verifier,
        "_pytest_junit_nodes",
        lambda *args, **kwargs: {node: "passed"},
    )

    with pytest.raises(verifier.ManifestError, match="witness identity drifted"):
        verifier._validate_binding_results(
            manifest,
            artifact_root,
            {"bindings_selector"},
            tmp_path,
            {},
        )


def _synthetic_binding_parity(
    *,
    configuration: str,
    sanitizer: str,
    argv_sha256: str,
) -> dict[str, Any]:
    return {
        "configuration": configuration,
        "sanitizer": sanitizer,
        "artifact_sha256": "1" * 64,
        "build_recipe": {
            "path": "/workspace/manyforge_core/python/CMakeLists.txt",
            "sha256": "2" * 64,
        },
        "target_projection": [
            {
                "project_id": "manyforge_core_py",
                "target": "manyforge_core_py",
                "target_type": "MODULE_LIBRARY",
                "artifact_relative": "manyforge_core_py.so",
                "artifact_sha256": "3" * 64,
                "output_inventory_cross_profile_sha256": None,
                "direct_sources": [
                    {
                        "path": "/workspace/manyforge_core/python/bindings.cpp",
                        "sha256": "4" * 64,
                    }
                ],
                "closure_sources": [
                    {
                        "path": "/workspace/manyforge_core/python/bindings.cpp",
                        "sha256": "4" * 64,
                    },
                    {
                        "path": "<build:manyforge_core_py>/manyforge_core_py.so",
                        "sha256": "3" * 64,
                    },
                ],
                "compile_dependencies": [
                    {
                        "path": "/workspace/manyforge_core/python/bindings.cpp",
                        "sha256": "4" * 64,
                        "scope": "repository_source",
                        "producer_kind": None,
                        "producer_project": None,
                        "producer_target": None,
                        "producer_inputs_sha256": None,
                    }
                ],
                "command_semantics": [
                    {
                        "kind": "link",
                        "output": "<build:manyforge_core_py>/manyforge_core_py.so",
                        "argv_sha256": argv_sha256,
                        "cross_profile_argv_sha256": "a" * 64,
                    }
                ],
            }
        ],
        "registration_header": {
            "path": "/workspace/manyforge_core/python/binding_registration.hpp",
            "sha256": "5" * 64,
        },
    }


def test_binding_sanitizer_parity_compares_exact_source_and_owner_closure() -> None:
    release = _synthetic_binding_parity(
        configuration="Release",
        sanitizer="none",
        argv_sha256="6" * 64,
    )
    asan = _synthetic_binding_parity(
        configuration="RelWithDebInfo",
        sanitizer="address+undefined",
        argv_sha256="7" * 64,
    )
    asan["artifact_sha256"] = "8" * 64
    asan["target_projection"][0]["artifact_sha256"] = "9" * 64

    release_projection = verifier._binding_source_parity(release, "release")
    asan_projection = verifier._binding_source_parity(asan, "asan")
    assert asan_projection == release_projection

    owner_drift = copy.deepcopy(asan)
    owner_drift["target_projection"][0]["project_id"] = "relabeled_owner"
    assert (
        verifier._binding_source_parity(owner_drift, "owner drift")
        != release_projection
    )

    source_drift = copy.deepcopy(asan)
    source_drift["target_projection"][0]["closure_sources"][0]["sha256"] = (
        "a" * 64
    )
    assert (
        verifier._binding_source_parity(source_drift, "source drift")
        != release_projection
    )

    semantic_drift = copy.deepcopy(asan)
    semantic_drift["target_projection"][0]["command_semantics"][0][
        "cross_profile_argv_sha256"
    ] = "b" * 64
    assert (
        verifier._binding_source_parity(semantic_drift, "semantic drift")
        != release_projection
    )
