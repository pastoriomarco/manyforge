"""Fail-closed native build provenance for Composable Cell I2 qualification.

This module consumes only packet-owned contracts, CTest's JSON registry, CMake's
File API/cache/compile database, Ninja's own graph/command tools, and the native
artifacts themselves.  It intentionally does not consume a build-authored
summary.  The public validators examine complete native, production-only, and
binding build lanes before any test executable is started and return canonical
JSON-compatible evidence.

The implementation is deliberately strict.  An unfamiliar CMake, Ninja,
response-file, archive, ELF, or dependency shape is a qualification failure,
not a reason to guess.
"""

from __future__ import annotations

import hashlib
import json
import os
import posixpath
import re
import shlex
import stat
import subprocess
from collections import Counter
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath
from typing import Any


SCHEMA_VERSION = "manyforge.composable_cell_i2.native_build_provenance.v1"
PRODUCTION_SCHEMA_VERSION = (
    "manyforge.composable_cell_i2.production_build_provenance.v1"
)
BINDING_SCHEMA_VERSION = "manyforge.composable_cell_i2.binding_build_provenance.v1"
PRODUCTION_TARGET_CONTRACT_SCHEMA = (
    "manyforge.composable_cell_i2.production_target_contract.v1"
)
BINDING_BUILD_CONTRACT_SCHEMA = (
    "manyforge.composable_cell_i2.binding_build_contract.v1"
)
NATIVE_LANE_TARGET_PARTITION_SCHEMA = (
    "manyforge.composable_cell_i2.native_lane_target_partitions.v1"
)
_WORKSPACE = PurePosixPath("/workspace")
_OUTPUT = PurePosixPath("/qualification-output")
_HEX64 = re.compile(r"[0-9a-f]{64}")
_TARGET = re.compile(r"test_[a-z0-9_]+")
_MAX_JSON_BYTES = 128 * 1024 * 1024
_MAX_RESPONSE_BYTES = 16 * 1024 * 1024
_MAX_RESPONSE_DEPTH = 8
_MAX_QUERY_NODES = 20_000
_MAX_COMMAND_LINES = 100_000
_CODE_SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".C"}
_MATERIAL_TARGET_TYPES = {
    "EXECUTABLE",
    "STATIC_LIBRARY",
    "SHARED_LIBRARY",
    "MODULE_LIBRARY",
    "OBJECT_LIBRARY",
}
_PACKAGE_BY_GROUP = {
    "core_release_baseline": "manyforge_core",
    "deployment_release_baseline": "manyforge_deployment",
    "c02_tail_required": "manyforge_deployment",
    "roboplan_release_baseline": "manyforge_roboplan",
    "roboplan_consumer_required": "manyforge_roboplan",
    "roboplan_scene_sanitizer_required": "manyforge_roboplan",
}
_ORDINARY_BY_GROUP = {
    "core_release_baseline": set(),
    "deployment_release_baseline": {
        "BuildManifestAuthorityRejectsMultiConfigGenerator",
        "CanonicalEmptyGeneratorCli",
        "BuildQualifiedAdmissionProfileIsDeterministic",
    },
    "c02_tail_required": set(),
    "roboplan_release_baseline": {
        "retained_compiled_visual_fk_composer_consumer",
        "retained_compiled_model_independent_standard_parser",
    },
    "roboplan_consumer_required": {
        "retained_compiled_visual_fk_composer_consumer",
        "retained_compiled_model_independent_standard_parser",
    },
    "roboplan_scene_sanitizer_required": set(),
}
_HELPERS = {
    "CanonicalEmptyGeneratorCli": (
        "MANYFORGE_GENERATOR",
        "manyforge_generate_canonical_empty_package",
        "manyforge_deployment/tools/generate_canonical_empty_package.cpp",
    ),
    "BuildQualifiedAdmissionProfileIsDeterministic": (
        "MANYFORGE_PROFILE_EMITTER",
        "manyforge_emit_build_qualified_admission_profile",
        "manyforge_deployment/tools/emit_build_qualified_admission_profile.cpp",
    ),
}
_SANITIZER_RUNTIME_NAMES = ("libasan.so", "libubsan.so", "libtsan.so")
_SANITIZER_SYMBOLS = ("__asan_init", "__ubsan_handle_", "__tsan_init")
_PYTHON_EXTENSION_SUFFIX = ".cpython-312-x86_64-linux-gnu.so"
_GRAPHVIZ_OPTIONS = b"""set(GRAPHVIZ_GRAPH_NAME \"manyforge_i2\")
set(GRAPHVIZ_GRAPH_HEADER \"node [ fontsize = \\\"12\\\" ];\")
set(GRAPHVIZ_NODE_PREFIX \"i2node\")
set(GRAPHVIZ_EXECUTABLES TRUE)
set(GRAPHVIZ_STATIC_LIBS TRUE)
set(GRAPHVIZ_SHARED_LIBS TRUE)
set(GRAPHVIZ_MODULE_LIBS TRUE)
set(GRAPHVIZ_INTERFACE_LIBS TRUE)
set(GRAPHVIZ_OBJECT_LIBS TRUE)
set(GRAPHVIZ_UNKNOWN_LIBS TRUE)
set(GRAPHVIZ_EXTERNAL_LIBS TRUE)
set(GRAPHVIZ_CUSTOM_TARGETS TRUE)
set(GRAPHVIZ_IGNORE_TARGETS \"\")
set(GRAPHVIZ_GENERATE_PER_TARGET FALSE)
set(GRAPHVIZ_GENERATE_DEPENDERS FALSE)
"""
_GRAPHVIZ_SHAPES = frozenset(
    {
        "egg",
        "octagon",
        "doubleoctagon",
        "tripleoctagon",
        "pentagon",
        "hexagon",
        "septagon",
        "box",
    }
)


class NativeBuildProvenanceError(RuntimeError):
    """The native lane does not have complete, unambiguous provenance."""


# Short alias for callers that prefer the mechanism-oriented name.
ProvenanceError = NativeBuildProvenanceError


def _fail(message: str) -> None:
    raise NativeBuildProvenanceError(message)


def _text(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value or "\x00" in value:
        _fail(f"{label} must be one non-empty text value")
    return value


def _object(value: Any, label: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping) or not all(isinstance(key, str) for key in value):
        _fail(f"{label} must be one string-keyed object")
    return value


def _list(value: Any, label: str) -> list[Any]:
    if not isinstance(value, list):
        _fail(f"{label} must be one array")
    return value


def _safe_relative(value: Any, label: str) -> Path:
    text = _text(value, label)
    pure = PurePosixPath(text)
    if (
        pure.is_absolute()
        or text != pure.as_posix()
        or text in {".", ".."}
        or any(part in {"", ".", ".."} for part in pure.parts)
        or "\\" in text
    ):
        _fail(f"{label} is not a canonical safe relative path: {text}")
    return Path(*pure.parts)


def _is_within(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
    except ValueError:
        return False
    return True


def _directory(path: Path, label: str, *, reject_symlink: bool = True) -> Path:
    try:
        metadata = path.lstat()
    except OSError as error:
        _fail(f"{label} is absent: {error}")
    if (reject_symlink and stat.S_ISLNK(metadata.st_mode)) or not path.is_dir():
        _fail(f"{label} is not one real directory: {path}")
    try:
        return path.resolve(strict=True)
    except OSError as error:
        _fail(f"{label} cannot be resolved: {error}")


def _regular_file(
    path: Path,
    label: str,
    *,
    allow_symlink: bool = False,
    contained_by: Path | None = None,
    require_single_link: bool = True,
) -> Path:
    try:
        metadata = path.lstat()
    except OSError as error:
        _fail(f"{label} is absent: {error}")
    if stat.S_ISLNK(metadata.st_mode) and not allow_symlink:
        _fail(f"{label} is a symlink: {path}")
    try:
        resolved = path.resolve(strict=True)
        resolved_metadata = resolved.stat()
    except OSError as error:
        _fail(f"{label} cannot be resolved: {error}")
    if not stat.S_ISREG(resolved_metadata.st_mode):
        _fail(f"{label} is not one regular file: {path}")
    if require_single_link and resolved_metadata.st_nlink != 1:
        _fail(f"{label} has an unqualified hardlink count: {resolved_metadata.st_nlink}")
    if contained_by is not None and not _is_within(resolved, contained_by):
        _fail(f"{label} escapes its allowed root: {path}")
    return resolved


def _read_bytes(path: Path, label: str, maximum: int = _MAX_JSON_BYTES) -> bytes:
    file_path = _regular_file(path, label)
    try:
        size = file_path.stat().st_size
        if size > maximum:
            _fail(f"{label} exceeds its {maximum}-byte bound")
        return file_path.read_bytes()
    except OSError as error:
        _fail(f"{label} cannot be read: {error}")


def _read_json(path: Path, label: str) -> Any:
    raw = _read_bytes(path, label)
    try:
        return json.loads(raw.decode("utf-8"), object_pairs_hook=_unique_object)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        _fail(f"{label} is not strict UTF-8 JSON: {error}")


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            _fail(f"JSON object contains duplicate key: {key}")
        result[key] = value
    return result


def _sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            while chunk := stream.read(1024 * 1024):
                digest.update(chunk)
    except OSError as error:
        _fail(f"cannot hash {path}: {error}")
    return digest.hexdigest()


def _canonical_sha256(value: Any) -> str:
    encoded = json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")
    return _sha256_bytes(encoded)


def _verify_framed_content_identity(
    document: Mapping[str, Any],
    *,
    identity_domain: str,
    api_version: str,
    own_field: str,
    label: str,
) -> str:
    if document.get("api_version") != api_version:
        _fail(f"{label} api_version drifted")
    declared = document.get(own_field)
    if (
        not isinstance(declared, str)
        or not declared.startswith("sha256:")
        or not _HEX64.fullmatch(declared[7:])
    ):
        _fail(f"{label} own identity is malformed")
    payload = {key: value for key, value in document.items() if key != own_field}
    try:
        canonical = json.dumps(
            payload,
            ensure_ascii=False,
            allow_nan=False,
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")
    except (TypeError, ValueError) as error:
        _fail(f"{label} cannot be canonically serialized: {error}")
    preimage = (
        b"manyforge.content-identity.v1\0"
        + identity_domain.encode("ascii")
        + b"\0"
        + api_version.encode("ascii")
        + b"\0"
        + canonical
    )
    computed = f"sha256:{_sha256_bytes(preimage)}"
    if declared != computed:
        _fail(f"{label} content identity does not match its exact document")
    return computed


_GIT_INDEX_CACHE: dict[
    tuple[str, str], tuple[str, dict[str, tuple[str, str]]]
] = {}


def _git_index_tree(
    object_format: str,
    entries: Mapping[str, tuple[str, str]],
    label: str,
) -> str:
    """Reconstruct Git's canonical index tree without writing the source repo."""

    if object_format not in {"sha1", "sha256"}:
        _fail(f"{label} uses unsupported Git object format {object_format!r}")
    expected_hex_size = hashlib.new(object_format).digest_size * 2
    directories: set[tuple[str, ...]] = {()}
    child_directories: dict[tuple[str, ...], set[str]] = {(): set()}
    files: dict[tuple[str, ...], dict[str, tuple[str, str]]] = {(): {}}
    for relative, (mode, blob) in entries.items():
        parts = PurePosixPath(relative).parts
        if not parts:
            _fail(f"{label} Git index contains an empty path")
        if mode not in {"100644", "100755", "120000", "160000"} or not re.fullmatch(
            rf"[0-9a-f]{{{expected_hex_size}}}", blob
        ):
            _fail(f"{label} Git index mode or object ID is malformed at {relative!r}")
        for depth, name in enumerate(parts[:-1]):
            parent = tuple(parts[:depth])
            child = tuple(parts[: depth + 1])
            if name in files.setdefault(parent, {}):
                _fail(f"{label} Git index has a file/directory collision at {relative!r}")
            child_directories.setdefault(parent, set()).add(name)
            directories.add(child)
            child_directories.setdefault(child, set())
            files.setdefault(child, {})
        parent = tuple(parts[:-1])
        name = parts[-1]
        if name in child_directories.setdefault(parent, set()) or name in files.setdefault(
            parent, {}
        ):
            _fail(f"{label} Git index has a duplicate or colliding path at {relative!r}")
        files[parent][name] = (mode, blob)

    tree_ids: dict[tuple[str, ...], str] = {}
    for directory in sorted(directories, key=lambda parts: (-len(parts), parts)):
        encoded: list[tuple[bytes, bytes]] = []
        for name, (mode, blob) in files[directory].items():
            raw_name = name.encode("utf-8")
            encoded.append(
                (
                    raw_name + b"\0",
                    mode.encode("ascii")
                    + b" "
                    + raw_name
                    + b"\0"
                    + bytes.fromhex(blob),
                )
            )
        for name in child_directories[directory]:
            raw_name = name.encode("utf-8")
            child_id = tree_ids[directory + (name,)]
            encoded.append(
                (
                    raw_name + b"/",
                    b"40000 "
                    + raw_name
                    + b"\0"
                    + bytes.fromhex(child_id),
                )
            )
        payload = b"".join(value for _key, value in sorted(encoded))
        tree_ids[directory] = hashlib.new(
            object_format,
            f"tree {len(payload)}\0".encode("ascii") + payload,
        ).hexdigest()
    return tree_ids[()]


def _git_index(
    git_root: Path, label: str
) -> tuple[str, str, dict[str, tuple[str, str]]]:
    object_format = _run_text(
        [
            "/usr/bin/git",
            "--no-optional-locks",
            "-C",
            str(git_root),
            "rev-parse",
            "--show-object-format",
        ],
        git_root,
        f"{label} Git object format",
    ).strip()
    if object_format not in {"sha1", "sha256"}:
        _fail(f"{label} uses unsupported Git object format {object_format!r}")
    raw = _run_bytes(
        [
            "/usr/bin/git",
            "--no-optional-locks",
            "-C",
            str(git_root),
            "ls-files",
            "--stage",
            "-z",
        ],
        git_root,
        f"{label} Git index",
    )
    entries: dict[str, tuple[str, str]] = {}
    for record in raw.split(b"\0"):
        if not record:
            continue
        try:
            metadata, path_bytes = record.split(b"\t", 1)
            mode_bytes, blob_bytes, stage_bytes = metadata.split(b" ", 2)
            path = path_bytes.decode("utf-8")
            mode = mode_bytes.decode("ascii")
            blob = blob_bytes.decode("ascii")
            stage = stage_bytes.decode("ascii")
        except (UnicodeDecodeError, ValueError) as error:
            _fail(f"{label} Git index row is malformed: {error}")
        if (
            stage != "0"
            or mode not in {"100644", "100755", "120000", "160000"}
            or not re.fullmatch(r"[0-9a-f]{40,64}", blob)
            or path in entries
            or PurePosixPath(path).is_absolute()
            or path != PurePosixPath(path).as_posix()
            or any(part in {"", ".", ".."} for part in PurePosixPath(path).parts)
        ):
            _fail(f"{label} Git index is conflicted or malformed at {path!r}")
        entries[path] = (mode, blob)
    if not entries:
        _fail(f"{label} Git index is empty")
    index_tree = _git_index_tree(object_format, entries, label)
    cache_key = (git_root.as_posix(), index_tree)
    cached = _GIT_INDEX_CACHE.get(cache_key)
    if cached is not None:
        cached_format, cached_entries = cached
        return index_tree, cached_format, cached_entries
    result = (object_format, entries)
    _GIT_INDEX_CACHE[cache_key] = result
    return index_tree, *result


def _git_tracked_file_identity(path: Path, git_root: Path, label: str) -> dict[str, str]:
    git_root = _directory(git_root, f"{label} Git root")
    try:
        relative = path.relative_to(git_root).as_posix()
    except ValueError:
        _fail(f"{label} escapes its Git root")
    _index_tree, object_format, entries = _git_index(git_root, label)
    entry = entries.get(relative)
    if entry is None:
        _fail(f"{label} is not tracked by the qualified Git tree: {relative}")
    mode, expected_blob = entry
    if mode not in {"100644", "100755"}:
        _fail(f"{label} uses unsupported tracked mode {mode}")
    file_path = _regular_file(path, label, contained_by=git_root)
    raw = _read_bytes(file_path, label)
    prefix = f"blob {len(raw)}\0".encode("ascii")
    digest = hashlib.new(object_format, prefix + raw).hexdigest()
    executable = bool(file_path.stat().st_mode & stat.S_IXUSR)
    if digest != expected_blob or executable != (mode == "100755"):
        _fail(f"{label} working bytes/mode differ from tracked Git blob {expected_blob}")
    return {
        "git_root": git_root.as_posix(),
        "path": relative,
        "mode": mode,
        "blob": expected_blob,
    }


def _validate_git_worktree_against_index(
    git_root: Path, label: str
) -> tuple[int, str]:
    """Hash raw tracked bytes/modes directly, without Git filters or stat hints."""

    _index_tree, object_format, entries = _git_index(git_root, label)
    projection: list[dict[str, str]] = []
    for relative, (mode, expected_blob) in sorted(entries.items()):
        pure = PurePosixPath(relative)
        current = git_root
        for part in pure.parts[:-1]:
            current = current / part
            try:
                metadata = current.lstat()
            except OSError as error:
                _fail(f"{label} tracked parent is absent at {relative}: {error}")
            if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
                _fail(f"{label} tracked path uses a non-directory parent: {relative}")
        path = git_root / Path(*pure.parts)
        try:
            metadata = path.lstat()
        except OSError as error:
            _fail(f"{label} tracked path is absent at {relative}: {error}")
        if mode in {"100644", "100755"}:
            if (
                not stat.S_ISREG(metadata.st_mode)
                or metadata.st_nlink != 1
                or bool(metadata.st_mode & stat.S_IXUSR) != (mode == "100755")
            ):
                _fail(f"{label} tracked file type/mode drifted at {relative}")
            digest = hashlib.new(object_format)
            digest.update(f"blob {metadata.st_size}\0".encode("ascii"))
            try:
                with path.open("rb") as stream:
                    while chunk := stream.read(1024 * 1024):
                        digest.update(chunk)
            except OSError as error:
                _fail(f"{label} tracked file cannot be hashed at {relative}: {error}")
            actual_blob = digest.hexdigest()
        elif mode == "120000":
            if not stat.S_ISLNK(metadata.st_mode):
                _fail(f"{label} tracked symlink type drifted at {relative}")
            try:
                raw_target = os.fsencode(os.readlink(path))
            except OSError as error:
                _fail(f"{label} tracked symlink cannot be read at {relative}: {error}")
            actual_blob = hashlib.new(
                object_format,
                f"blob {len(raw_target)}\0".encode("ascii") + raw_target,
            ).hexdigest()
        else:
            _fail(f"{label} contains unsupported tracked Gitlink {relative}")
        if actual_blob != expected_blob:
            _fail(f"{label} tracked working bytes differ from index at {relative}")
        projection.append({"path": relative, "mode": mode, "blob": expected_blob})
    return len(projection), _canonical_sha256(projection)


def replay_git_source_generation(
    git_root: str | os.PathLike[str],
    allowed_untracked_paths: Sequence[str] = (),
) -> dict[str, Any]:
    """Recompute exact raw worktree/index facts with no Git filter trust."""

    git_root = _directory(Path(git_root), "source Git generation root")
    allowed_values = list(allowed_untracked_paths)
    if allowed_values != sorted(set(allowed_values)):
        _fail("allowed untracked source paths are not exact and sorted")
    for value in allowed_values:
        directory_marker = value.endswith("/")
        normalized = _safe_relative(
            value[:-1] if directory_marker else value,
            "allowed untracked source path",
        ).as_posix()
        if value != normalized + ("/" if directory_marker else ""):
            _fail("allowed untracked source path is not canonical")
    label = "source Git generation"
    git_prefix = [
        "/usr/bin/git",
        "--no-optional-locks",
        "-c",
        "color.ui=false",
        "-c",
        "diff.algorithm=myers",
        "-c",
        "diff.renames=false",
        "-c",
        "core.fileMode=true",
        "-c",
        "core.fsmonitor=false",
        "-c",
        "core.autocrlf=false",
        "-C",
        str(git_root),
        f"--work-tree={git_root}",
    ]
    stable_diff = [
        "--binary",
        "--full-index",
        "--no-color",
        "--no-ext-diff",
        "--no-textconv",
        "--no-renames",
        "--src-prefix=a/",
        "--dst-prefix=b/",
    ]
    head = _run_text(
        [*git_prefix, "rev-parse", "HEAD"],
        git_root,
        f"{label} HEAD",
    ).strip()
    head_tree = _run_text(
        [*git_prefix, "rev-parse", "HEAD^{tree}"],
        git_root,
        f"{label} HEAD tree",
    ).strip()
    index_tree, _object_format, _entries = _git_index(git_root, label)
    staged_diff = _run_bytes(
        [*git_prefix, "diff", "--cached", *stable_diff],
        git_root,
        f"{label} staged diff",
    )
    staged_paths = _run_bytes(
        [*git_prefix, "diff", "--cached", "--name-only", "-z", "--no-renames"],
        git_root,
        f"{label} staged paths",
    )
    working_diff = _run_bytes(
        [*git_prefix, "diff", *stable_diff],
        git_root,
        f"{label} working-tree diff",
    )
    untracked = _run_bytes(
        [*git_prefix, "ls-files", "--others", "-z"],
        git_root,
        f"{label} untracked or ignored paths",
    )
    untracked_records = [record for record in untracked.split(b"\0") if record]
    allowed_snapshot_inputs = [value.encode("utf-8") for value in allowed_values]
    if untracked_records != allowed_snapshot_inputs:
        _fail(
            f"{label} has undeclared untracked or ignored source inputs: "
            f"{[record.decode('utf-8', errors='replace') for record in untracked_records]}"
        )
    unmerged = _run_bytes(
        [*git_prefix, "ls-files", "--unmerged", "-z"],
        git_root,
        f"{label} unmerged paths",
    )
    submodules = _run_bytes(
        [*git_prefix, "submodule", "status", "--recursive"],
        git_root,
        f"{label} submodule state",
    )
    for flag, description in (
        ("-v", "assume-unchanged or skip-worktree"),
        ("-f", "fsmonitor-clean"),
    ):
        tracked_flags = _run_bytes(
            [*git_prefix, "ls-files", flag, "-z"],
            git_root,
            f"{label} tracked-file {description} flags",
        )
        if any(
            record and not record.startswith(b"H ")
            for record in tracked_flags.split(b"\0")
        ):
            _fail(f"{label} uses hidden {description} index flags")
    if working_diff or unmerged or submodules:
        _fail(f"{label} has unstaged, untracked, unmerged, or submodule state")
    staged_path_count = len([path for path in staged_paths.split(b"\0") if path])
    try:
        allowed_untracked_paths = [
            record.decode("utf-8", errors="strict")
            for record in untracked_records
        ]
    except UnicodeDecodeError as error:
        _fail(f"{label} has a non-UTF-8 untracked path: {error}")
    tracked_path_count, tracked_projection_sha256 = (
        _validate_git_worktree_against_index(git_root, label)
    )
    return {
        "base_commit": head,
        "base_tree": head_tree,
        "patched_index_tree": index_tree,
        "staged_diff_sha256": _sha256_bytes(staged_diff),
        "staged_path_count": staged_path_count,
        "staged_paths_sha256": _sha256_bytes(staged_paths),
        "unstaged_path_count": 0,
        "untracked_path_count": len(untracked_records),
        "allowed_untracked_paths": allowed_untracked_paths,
        "undeclared_untracked_path_count": 0,
        "unmerged_path_count": 0,
        "submodule_path_count": 0,
        "tracked_path_count": tracked_path_count,
        "tracked_index_projection_sha256": tracked_projection_sha256,
    }


def _git_source_generation_evidence(
    *,
    git_root: Path,
    project_id: str,
    build_contract: Mapping[str, Any],
    roots: "_Roots",
) -> dict[str, Any]:
    """Capture one exact source generation and join its prebuild contract."""

    label = f"{project_id} source Git generation"
    facts = replay_git_source_generation(
        git_root,
        [".docker/roboplan/"] if git_root == roots.repository else [],
    )
    prebuild_capture: dict[str, Any] | None = None
    if project_id in {"roboplan", "roboplan_example_models"}:
        auxiliary_contract = _object(
            build_contract.get("auxiliary_install_contract"),
            "auxiliary install contract",
        )
        expected = _object(
            auxiliary_contract.get("source_generation"),
            "auxiliary source generation contract",
        )
        if {
            key: facts[key]
            for key in (
                "base_commit",
                "base_tree",
                "patched_index_tree",
                "staged_diff_sha256",
                "staged_path_count",
                "staged_paths_sha256",
            )
        } != expected:
            _fail(f"{label} differs from the frozen patched generation")
        capture_path = _regular_file(
            git_root.parent / "patched-source-generation-pre.json",
            f"{label} prebuild capture",
            contained_by=roots.output,
        )
        capture = _object(
            _read_json(capture_path, f"{label} prebuild capture"),
            f"{label} prebuild capture",
        )
        expected_capture = {
            "schema_version": "manyforge.composable_cell_i2.patched_source_generation.v1",
            **facts,
        }
        if capture != expected_capture:
            _fail(f"{label} changed between prebuild capture and provenance")
        prebuild_capture = {
            "path": roots.logical(capture_path),
            "sha256": _sha256_file(capture_path),
        }
    elif (
        facts["base_tree"] != facts["patched_index_tree"]
        or facts["staged_diff_sha256"] != _sha256_bytes(b"")
        or facts["staged_paths_sha256"] != _sha256_bytes(b"")
        or facts["staged_path_count"] != 0
    ):
        _fail(f"{label} repository source is not one clean committed generation")
    return {
        "git_root": roots.logical(git_root),
        **facts,
        "prebuild_capture": prebuild_capture,
        "prebuild_matches_postbuild": True,
    }


def _containing_git_root(path: Path, boundary: Path, label: str) -> Path:
    current = path if path.is_dir() else path.parent
    while _is_within(current, boundary):
        if (current / ".git").is_dir():
            return current
        if current == boundary:
            break
        current = current.parent
    _fail(f"{label} has no bounded Git root")


@dataclass(frozen=True)
class _Roots:
    repository: Path
    output: Path

    def decode(self, value: str, base: Path, label: str) -> Path:
        text = _text(value, label)
        if "\\" in text:
            _fail(f"{label} uses a non-POSIX path: {text}")
        pure = PurePosixPath(text)
        if not pure.is_absolute():
            if text != pure.as_posix() or any(part == ".." for part in pure.parts):
                _fail(f"{label} is not canonical: {text}")
            return Path(os.path.normpath(base / Path(*pure.parts)))
        if posixpath.normpath(text) != text:
            _fail(f"{label} is not canonical: {text}")
        if pure == _WORKSPACE or pure.is_relative_to(_WORKSPACE):
            return self.repository / Path(*pure.relative_to(_WORKSPACE).parts)
        if pure == _OUTPUT or pure.is_relative_to(_OUTPUT):
            return self.output / Path(*pure.relative_to(_OUTPUT).parts)
        return Path(text)

    def logical(self, path: Path) -> str:
        lexical = Path(os.path.abspath(path))
        if _is_within(lexical, self.repository):
            relative = lexical.relative_to(self.repository).as_posix()
            return "/workspace" if relative == "." else f"/workspace/{relative}"
        if _is_within(lexical, self.output):
            relative = lexical.relative_to(self.output).as_posix()
            return (
                "/qualification-output"
                if relative == "."
                else f"/qualification-output/{relative}"
            )
        return lexical.as_posix()

    def normalize_tool_output(self, text: str) -> str:
        return text.replace(str(self.repository), "/workspace").replace(
            str(self.output), "/qualification-output"
        )


def _run_bytes(argv: Sequence[str], cwd: Path, label: str, env: Mapping[str, str] | None = None) -> bytes:
    if not argv or not all(isinstance(item, str) and item for item in argv):
        _fail(f"{label} has malformed argv")
    environment = {
        "PATH": "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        "LC_ALL": "C.UTF-8",
        "TZ": "UTC",
    }
    if env is not None:
        allowed_environment = {
            "HOME",
            "LANG",
            "LC_ALL",
            "LD_LIBRARY_PATH",
            "PATH",
            "PYTHONDONTWRITEBYTECODE",
            "PYTHONHASHSEED",
            "PYTHONNOUSERSITE",
            "TMPDIR",
            "XDG_CACHE_HOME",
        }
        if not set(env).issubset(allowed_environment) or any(
            not isinstance(value, str) or not value or "\0" in value
            for value in env.values()
        ):
            _fail(f"{label} requested an unsupported tool environment")
        environment.update(env)
    try:
        completed = subprocess.run(
            list(argv),
            cwd=cwd,
            env=environment,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=120,
        )
    except (OSError, subprocess.SubprocessError) as error:
        _fail(f"{label} could not run: {error}")
    if completed.returncode != 0:
        stderr = completed.stderr.decode("utf-8", errors="replace")[-2000:]
        _fail(f"{label} failed with {completed.returncode}: {stderr}")
    if len(completed.stdout) > _MAX_JSON_BYTES:
        _fail(f"{label} output exceeds the {_MAX_JSON_BYTES}-byte bound")
    return completed.stdout


def _run_text(argv: Sequence[str], cwd: Path, label: str, env: Mapping[str, str] | None = None) -> str:
    raw = _run_bytes(argv, cwd, label, env)
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError as error:
        _fail(f"{label} did not return UTF-8: {error}")


def _parse_cache(path: Path, label: str) -> tuple[dict[str, tuple[str, str]], str]:
    raw = _read_bytes(path, label)
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as error:
        _fail(f"{label} is not UTF-8: {error}")
    result: dict[str, tuple[str, str]] = {}
    for number, line in enumerate(text.splitlines(), 1):
        if not line or line.startswith(("#", "//")):
            continue
        left, separator, value = line.partition("=")
        key, type_separator, cache_type = left.partition(":")
        if not separator or not type_separator or not key or key in result:
            _fail(f"{label} has a malformed or duplicate row at line {number}")
        result[key] = (cache_type, value)
    return result, _sha256_bytes(raw)


def _cache_value(cache: Mapping[str, tuple[str, str]], key: str, label: str) -> str:
    if key not in cache:
        _fail(f"{label} lacks {key}")
    return cache[key][1]


@dataclass(frozen=True)
class _VirtualSourceReply:
    path: Path
    kind: str
    owner: Path


def _classify_cmake_virtual_source(
    *,
    target_name: str,
    target_type: str,
    build_root: Path,
    target_build_base: Path,
    path: Path,
    generated: bool,
    compile_group_present: bool,
    source_group: str | None,
) -> _VirtualSourceReply | None:
    """Recognize only the virtual source rows emitted by pinned CMake/Ninja."""

    utility_stamp = target_build_base / "CMakeFiles" / target_name
    if target_type == "UTILITY" and path == utility_stamp:
        if not generated or source_group != "" or compile_group_present:
            _fail(f"target {target_name} has malformed CMake utility stamp source")
        return _VirtualSourceReply(
            path=path,
            kind="cmake_utility_stamp",
            owner=path,
        )

    is_rule_node = path.suffix == ".rule"
    if source_group == "CMake Rules" or is_rule_node:
        if (
            source_group != "CMake Rules"
            or not is_rule_node
            or not generated
            or compile_group_present
            or not _is_within(path, build_root)
        ):
            _fail(f"target {target_name} has malformed CMake virtual rule source")
        return _VirtualSourceReply(
            path=path,
            kind="cmake_rule",
            owner=Path(path.as_posix()[: -len(".rule")]),
        )
    return None


@dataclass(frozen=True)
class _TargetReply:
    name: str
    target_type: str
    artifacts: tuple[Path, ...]
    sources: tuple[Path, ...]
    virtual_sources: tuple[_VirtualSourceReply, ...]
    build_base: Path
    reply_path: Path
    reply_sha256: str


@dataclass(frozen=True)
class _CompileRecord:
    output: Path
    source: Path
    directory: Path
    argv: tuple[str, ...]
    argv_sha256: str


@dataclass
class _Build:
    lane_id: str
    project_id: str
    root: Path
    source_root: Path
    compiler_path: str
    archiver_path: str
    targets: dict[str, _TargetReply]
    compile_commands: dict[Path, _CompileRecord]
    configure_inputs: dict[Path, dict[str, Any]]
    configure_generated_contracts: dict[str, _ConfigureGeneratedInputContract]
    qualified_install_inputs: dict[Path, dict[str, Any]]
    evidence: dict[str, Any]
    configure_executed_inputs: dict[Path, dict[str, Any]] = field(
        default_factory=dict
    )
    parity_path_aliases: tuple[tuple[str, str], ...] = ()
    cross_profile_command_deltas: dict[str, dict[str, int]] = field(
        default_factory=dict
    )
    cross_profile_known_delta_tokens: frozenset[str] = frozenset()


class _ResponseCollector:
    def __init__(self, roots: _Roots) -> None:
        self._roots = roots
        self._rows: dict[str, dict[str, Any]] = {}

    def expand(
        self,
        argv: Sequence[str],
        cwd: Path,
        label: str,
        *,
        depth: int = 0,
        active: frozenset[Path] = frozenset(),
    ) -> tuple[str, ...]:
        if depth > _MAX_RESPONSE_DEPTH:
            _fail(f"{label} exceeds the response-file recursion bound")
        result: list[str] = []
        for token in argv:
            prefix = ""
            response_name: str | None = None
            if token.startswith("@") and len(token) > 1:
                response_name = token[1:]
            elif token.startswith("-Wl,@") and len(token) > 5:
                prefix = "-Wl,"
                response_name = token[5:]
            if response_name is None:
                result.append(token)
                continue
            response = self._roots.decode(response_name, cwd, f"{label} response path")
            response = _regular_file(
                response,
                f"{label} response file",
                contained_by=self._roots.output,
            )
            if response in active:
                _fail(f"{label} contains a response-file cycle")
            raw = _read_bytes(response, f"{label} response file", _MAX_RESPONSE_BYTES)
            try:
                decoded = shlex.split(raw.decode("utf-8"), posix=True)
            except (UnicodeDecodeError, ValueError) as error:
                _fail(f"{label} response file has unsupported syntax: {error}")
            logical = self._roots.logical(response)
            row = {
                "path": logical,
                "sha256": _sha256_bytes(raw),
                "size": len(raw),
                "token_count": len(decoded),
            }
            if logical in self._rows and self._rows[logical] != row:
                _fail(f"{label} response file changed during provenance inspection")
            self._rows[logical] = row
            expanded = self.expand(
                decoded,
                response.parent,
                label,
                depth=depth + 1,
                active=active | {response},
            )
            if prefix:
                result.extend(f"{prefix}{item}" for item in expanded)
            else:
                result.extend(expanded)
        return tuple(result)

    def evidence(self) -> list[dict[str, Any]]:
        return [self._rows[key] for key in sorted(self._rows)]


def _option_output(argv: Sequence[str], roots: _Roots, cwd: Path, label: str) -> Path | None:
    outputs: list[Path] = []
    index = 0
    while index < len(argv):
        token = argv[index]
        if token == "-o":
            if index + 1 >= len(argv):
                _fail(f"{label} ends with -o")
            outputs.append(roots.decode(argv[index + 1], cwd, f"{label} output"))
            index += 2
            continue
        index += 1
    if len(outputs) > 1:
        _fail(f"{label} has multiple -o outputs")
    return outputs[0] if outputs else None


def _compile_source(argv: Sequence[str], roots: _Roots, cwd: Path, label: str) -> Path | None:
    indexes = [index for index, token in enumerate(argv) if token == "-c"]
    if not indexes:
        return None
    if len(indexes) != 1 or indexes[0] + 1 >= len(argv):
        _fail(f"{label} has an ambiguous -c source")
    return roots.decode(argv[indexes[0] + 1], cwd, f"{label} source")


def _compile_system_path(path: Path) -> bool:
    return _is_within(path, Path("/usr")) or _is_within(path, Path("/lib"))


def _validated_compile_directory(
    value: str,
    *,
    cwd: Path,
    build_root: Path,
    roots: _Roots,
    label: str,
) -> Path:
    raw = value[1:] if value.startswith("=") else value
    path = Path(os.path.normpath(roots.decode(raw, cwd, label)))
    directory = _directory(path, label, reject_symlink=False)
    if (
        _is_within(directory, roots.repository)
        or _is_within(directory, roots.output)
        or _compile_system_path(directory)
    ):
        return directory
    _fail(f"{label} escapes qualified source, output, and pinned-system roots")


def _project_source_required_compile_flags(
    compile_modes: Mapping[str, Any],
    *,
    project_id: str,
    project_source_root: Path,
) -> dict[Path, set[str]]:
    """Resolve the frozen source-scoped compiler flags for one project."""

    catalog = _object(
        compile_modes.get("project_source_required_flags"),
        "project source required compile flags",
    )
    if project_id not in catalog:
        return {}
    project = _object(
        catalog[project_id], f"project source required compile flags {project_id}"
    )
    result: dict[Path, set[str]] = {}
    for flag, raw_paths in project.items():
        if flag != "-fopenmp":
            _fail(f"{project_id} has an unsupported contracted compiler flag")
        path_values = _list(
            raw_paths, f"{project_id} required compiler flag {flag} sources"
        )
        if path_values != sorted(path_values) or len(path_values) != len(
            set(path_values)
        ):
            _fail(f"{project_id} required compiler flag source set drifted")
        for index, raw_path in enumerate(path_values):
            relative = _safe_relative(
                raw_path,
                f"{project_id} required compiler flag {flag} source[{index}]",
            )
            source = _regular_file(
                project_source_root / relative,
                f"{project_id} required compiler flag {flag} source {relative}",
                contained_by=project_source_root,
            )
            normalized = Path(os.path.normpath(source))
            result.setdefault(normalized, set()).add(flag)
    return result


def _validate_compile_driver_argv(
    argv: Sequence[str],
    *,
    cwd: Path,
    source: Path,
    output: Path,
    build_root: Path,
    roots: _Roots,
    expected_configuration: str,
    compile_modes: Mapping[str, Any],
    required_driver_flags: frozenset[str],
    label: str,
) -> None:
    """Reject compiler inputs or tool controls outside the proved dependency graph."""

    if not argv:
        _fail(f"{label} has no compiler argv")
    if source.suffix not in _CODE_SOURCE_SUFFIXES:
        _fail(f"{label} compiles an unsupported source-language suffix")
    dangerous_prefixes = (
        "-fplugin",
        "-fprofile",
        "-fauto-profile",
        "-fbranch-probabilities",
        "-fmodule",
        "-fmodules",
        "-fprebuilt-module-path",
        "-fpch",
        "-flto",
        "-fuse-ld",
        "-specs",
        "--specs",
        "-B",
        "--sysroot",
        "-isysroot",
        "-wrapper",
        "-Xclang",
        "-Xassembler",
        "-Xpreprocessor",
        "-Wp,",
        "-Wa,",
        "-Wl,",
        "-mllvm",
        "-save-temps",
        "-frepo",
        "-include",
        "-imacros",
    )
    safe_no_argument = {
        "-MD",
        "-MMD",
        "-MP",
        "-pipe",
        "-pthread",
        "-pedantic",
        "-pedantic-errors",
        "-w",
        "-nostdinc",
        "-nostdinc++",
    }
    safe_flag = re.compile(
        r"(?:"
        r"-O(?:[0-3]|s|g|fast)|"
        r"-g(?:[0-3]|gdb[0-9]?|line-tables-only)?|"
        r"-[DU][A-Za-z_][A-Za-z0-9_]*(?:=.*)?|"
        r"-std=(?:gnu\+\+|c\+\+)[A-Za-z0-9]+|"
        r"-W[A-Za-z0-9_+.,=-]+|"
        r"-m[A-Za-z0-9_+.,=-]+|"
        r"-f(?:PIC|pic|PIE|pie|omit-frame-pointer|no-omit-frame-pointer|"
        r"sanitize=[A-Za-z0-9_,+-]+|no-sanitize=[A-Za-z0-9_,+-]+|"
        r"sanitize-recover=[A-Za-z0-9_,+-]+|"
        r"no-sanitize-recover=[A-Za-z0-9_,+-]+|"
        r"visibility(?:-inlines)?-hidden|no-semantic-interposition|"
        r"function-sections|data-sections|"
        r"stack-protector(?:-strong|-all)?|"
        r"cf-protection(?:=[A-Za-z0-9_-]+)?|"
        r"exceptions|no-exceptions|rtti|no-rtti|"
        r"diagnostics-color(?:=[A-Za-z0-9_-]+)?|"
        r"common|no-common|signed-char|unsigned-char|"
        r"strict-aliasing|no-strict-aliasing|wrapv|"
        r"no-delete-null-pointer-checks)"
        r")"
    )
    seen_source = 0
    seen_output = 0
    index = 1
    while index < len(argv):
        token = argv[index]
        if token == "-c":
            if index + 1 >= len(argv):
                _fail(f"{label} has a dangling -c")
            candidate = Path(
                os.path.normpath(
                    roots.decode(argv[index + 1], cwd, f"{label} -c source")
                )
            )
            if candidate != Path(os.path.normpath(source)):
                _fail(f"{label} -c source differs from the contracted source")
            seen_source += 1
            index += 2
            continue
        if token == "-o":
            if index + 1 >= len(argv):
                _fail(f"{label} has a dangling -o")
            candidate = Path(
                os.path.normpath(
                    roots.decode(argv[index + 1], cwd, f"{label} -o output")
                )
            )
            if candidate != Path(os.path.normpath(output)):
                _fail(f"{label} -o output differs from the contracted object")
            seen_output += 1
            index += 2
            continue
        if token in {"-I", "-isystem", "-iquote", "-idirafter"}:
            if index + 1 >= len(argv):
                _fail(f"{label} has a dangling {token}")
            _validated_compile_directory(
                argv[index + 1],
                cwd=cwd,
                build_root=build_root,
                roots=roots,
                label=f"{label} {token} directory",
            )
            index += 2
            continue
        joined_include = next(
            (
                prefix
                for prefix in ("-isystem", "-iquote", "-idirafter", "-I")
                if token.startswith(prefix) and len(token) > len(prefix)
            ),
            None,
        )
        if joined_include is not None:
            _validated_compile_directory(
                token[len(joined_include) :],
                cwd=cwd,
                build_root=build_root,
                roots=roots,
                label=f"{label} {joined_include} directory",
            )
            index += 1
            continue
        if token == "-MF":
            if index + 1 >= len(argv):
                _fail(f"{label} has a dangling -MF")
            depfile = Path(
                os.path.normpath(
                    roots.decode(argv[index + 1], cwd, f"{label} depfile")
                )
            )
            if not _is_within(depfile, build_root):
                _fail(f"{label} depfile escapes its build")
            index += 2
            continue
        if token.startswith("-MF") and len(token) > 3:
            depfile = Path(
                os.path.normpath(roots.decode(token[3:], cwd, f"{label} depfile"))
            )
            if not _is_within(depfile, build_root):
                _fail(f"{label} depfile escapes its build")
            index += 1
            continue
        if token in {"-MT", "-MQ"}:
            if index + 1 >= len(argv) or not argv[index + 1]:
                _fail(f"{label} has a dangling {token}")
            index += 2
            continue
        if token.startswith(("-MT", "-MQ")) and len(token) > 3:
            index += 1
            continue
        if token == "-x":
            if index + 1 >= len(argv) or argv[index + 1] not in {
                "c++",
                "c++-cpp-output",
            }:
                _fail(f"{label} has an unsupported explicit language")
            index += 2
            continue
        if token == "-ansi" or token.startswith("@") or token.startswith(
            dangerous_prefixes
        ):
            _fail(f"{label} uses an unsupported compiler input/tool control: {token}")
        if (
            token in safe_no_argument
            or token in required_driver_flags
            or safe_flag.fullmatch(token)
        ):
            if token in {"-march=native", "-mtune=native", "-mcpu=native"}:
                _fail(f"{label} depends on ambient host CPU selection")
            index += 1
            continue
        if token.startswith("-"):
            _fail(f"{label} uses an unsupported compiler option: {token}")
        _fail(f"{label} has an unowned positional compiler input: {token}")
    if seen_source != 1 or seen_output != 1:
        _fail(f"{label} does not have exactly one source and object output")
    if any(argv.count(flag) != 1 for flag in required_driver_flags):
        _fail(f"{label} does not contain its exact project/source compiler flags")
    mode = _object(
        compile_modes.get(expected_configuration),
        f"{label} {expected_configuration} compile mode",
    )
    standard_flags = [token for token in argv if token.startswith("-std=")]
    optimization_flags = [
        token for token in argv if re.fullmatch(r"-O(?:[0-3]|s|g|fast)", token)
    ]
    debug_flags = [
        token
        for token in argv
        if re.fullmatch(r"-g(?:[0-3]|gdb[0-9]?|line-tables-only)?", token)
    ]
    expected_debug = mode.get("debug_flag")
    if (
        standard_flags != [compile_modes.get("cxx_standard_flag")]
        or optimization_flags != [mode.get("optimization_flag")]
        or debug_flags != ([] if expected_debug is None else [expected_debug])
        or (argv.count("-DNDEBUG") == 1) != (mode.get("require_ndebug") is True)
        or any(token.startswith(("-UNDEBUG", "-DNDEBUG=")) for token in argv)
    ):
        _fail(
            f"{label} does not implement exact C++20 {expected_configuration} "
            "compile mode"
        )


def _compiler_slice(argv: Sequence[str], label: str) -> tuple[str, ...]:
    result = _maybe_compiler_slice(argv)
    if result is None:
        _fail(f"{label} has no supported compiler driver")
    return result


def _maybe_compiler_slice(argv: Sequence[str]) -> tuple[str, ...] | None:
    compiler = re.compile(r"(?:c\+\+|g\+\+|clang\+\+|gcc|clang|cc)(?:-[0-9.]+)?$")
    for index, token in enumerate(argv):
        if compiler.fullmatch(Path(token).name):
            return tuple(argv[index:])
    return None


def _load_compile_commands(
    project_id: str,
    project_source_root: Path,
    build_root: Path,
    source_roots: Sequence[Path],
    expected_compiler: str,
    roots: _Roots,
    responses: _ResponseCollector,
    expected_configuration: str,
    compile_modes: Mapping[str, Any],
) -> tuple[dict[Path, _CompileRecord], dict[str, Any]]:
    path = build_root / "compile_commands.json"
    raw = _read_bytes(path, f"{build_root.name} compile_commands.json")
    try:
        rows = json.loads(raw.decode("utf-8"), object_pairs_hook=_unique_object)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        _fail(f"{build_root.name} compile_commands.json is malformed: {error}")
    if not isinstance(rows, list) or not rows:
        _fail(f"{build_root.name} compile_commands.json is empty or not an array")
    result: dict[Path, _CompileRecord] = {}
    required_flags_by_source = _project_source_required_compile_flags(
        compile_modes,
        project_id=project_id,
        project_source_root=project_source_root,
    )
    observed_contracted_sources: set[Path] = set()
    for index, raw_row in enumerate(rows):
        row = _object(raw_row, f"compile_commands[{index}]")
        allowed = {"directory", "file", "output", "command", "arguments"}
        if not set(row).issubset(allowed):
            _fail(f"compile_commands[{index}] has unsupported keys")
        directory = roots.decode(
            _text(row.get("directory"), f"compile_commands[{index}].directory"),
            build_root,
            f"compile_commands[{index}].directory",
        )
        if not _is_within(directory, build_root):
            _fail(f"compile_commands[{index}] directory escapes its build")
        has_command = "command" in row
        has_arguments = "arguments" in row
        if has_command == has_arguments:
            _fail(f"compile_commands[{index}] must have exactly one argv representation")
        if has_arguments:
            arguments = _list(row["arguments"], f"compile_commands[{index}].arguments")
            if not arguments or not all(isinstance(item, str) and item for item in arguments):
                _fail(f"compile_commands[{index}].arguments is malformed")
            argv = tuple(arguments)
        else:
            try:
                argv = tuple(shlex.split(_text(row["command"], f"compile_commands[{index}].command")))
            except ValueError as error:
                _fail(f"compile_commands[{index}].command is unsupported: {error}")
        argv = responses.expand(argv, directory, f"compile_commands[{index}]")
        compiler_argv = _compiler_slice(argv, f"compile_commands[{index}]")
        if not argv or argv[0] != expected_compiler or compiler_argv != argv:
            _fail(
                f"compile_commands[{index}] does not invoke the exact cache compiler "
                "without an environment assignment, wrapper, or launcher"
            )
        source = roots.decode(
            _text(row.get("file"), f"compile_commands[{index}].file"),
            directory,
            f"compile_commands[{index}].file",
        )
        parsed_source = _compile_source(compiler_argv, roots, directory, f"compile_commands[{index}]")
        if parsed_source is None or os.path.normpath(parsed_source) != os.path.normpath(source):
            _fail(f"compile_commands[{index}] source disagrees with its argv")
        if not (
            any(_is_within(source, source_root) for source_root in source_roots)
            or _is_within(source, build_root)
        ):
            _fail(f"compile_commands[{index}] source escapes source/build roots")
        source_owner = (
            build_root
            if _is_within(source, build_root)
            else next(
                (root for root in source_roots if _is_within(source, root)),
                None,
            )
        )
        if source_owner is None:
            _fail(f"compile_commands[{index}] source has no containment owner")
        _regular_file(
            source,
            f"compile_commands[{index}] source",
            contained_by=source_owner,
        )
        output_value = row.get("output")
        output = (
            roots.decode(_text(output_value, f"compile_commands[{index}].output"), directory, f"compile_commands[{index}].output")
            if output_value is not None
            else _option_output(compiler_argv, roots, directory, f"compile_commands[{index}]")
        )
        if output is None or not _is_within(output, build_root):
            _fail(f"compile_commands[{index}] output is absent or escapes its build")
        _validate_compile_driver_argv(
            compiler_argv,
            cwd=directory,
            source=source,
            output=output,
            build_root=build_root,
            roots=roots,
            expected_configuration=expected_configuration,
            compile_modes=compile_modes,
            required_driver_flags=frozenset(
                required_flags_by_source.get(
                    Path(os.path.normpath(source)),
                    set(),
                )
            ),
            label=f"compile_commands[{index}]",
        )
        if Path(os.path.normpath(source)) in required_flags_by_source:
            observed_contracted_sources.add(Path(os.path.normpath(source)))
        key = Path(os.path.normpath(output))
        record = _CompileRecord(
            output=key,
            source=Path(os.path.normpath(source)),
            directory=directory,
            argv=compiler_argv,
            argv_sha256=_canonical_sha256(list(compiler_argv)),
        )
        if key in result:
            _fail(f"compile database has duplicate output {roots.logical(key)}")
        result[key] = record
    if observed_contracted_sources != set(required_flags_by_source):
        _fail(
            f"{project_id} compile database omits contracted flag-bearing sources: "
            f"{sorted(path.as_posix() for path in set(required_flags_by_source) - observed_contracted_sources)}"
        )
    return result, {
        "path": roots.logical(path),
        "sha256": _sha256_bytes(raw),
        "entry_count": len(result),
    }


def _resolve_declared_path(
    value: Any,
    declared_base: Path,
    roots: _Roots,
    label: str,
) -> Path:
    return Path(os.path.normpath(roots.decode(_text(value, label), declared_base, label)))


def _file_api_documents(reply_root: Path, index: Path) -> dict[str, tuple[Any, str]]:
    """Load exactly the JSON graph reachable from one File API index."""

    pending = [index.name]
    documents: dict[str, tuple[Any, str]] = {}
    while pending:
        name = pending.pop()
        if name in documents:
            continue
        if Path(name).name != name or not name.endswith(".json"):
            _fail(f"File API references an unsafe JSON file: {name}")
        path = _regular_file(reply_root / name, f"File API reply {name}", contained_by=reply_root)
        raw = _read_bytes(path, f"File API reply {name}")
        try:
            value = json.loads(raw.decode("utf-8"), object_pairs_hook=_unique_object)
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            _fail(f"File API reply {name} is malformed: {error}")
        documents[name] = (value, _sha256_bytes(raw))

        def visit(item: Any) -> None:
            if isinstance(item, Mapping):
                for key, child in item.items():
                    if key == "jsonFile":
                        referenced = _text(child, f"File API {name} jsonFile")
                        if referenced not in documents:
                            pending.append(referenced)
                    else:
                        visit(child)
            elif isinstance(item, list):
                for child in item:
                    visit(child)

        visit(value)
    actual = {path.name for path in reply_root.iterdir() if path.is_file() and path.name.endswith(".json")}
    if actual != set(documents):
        _fail(
            "File API reply directory contains stale or unreferenced JSON: "
            f"expected={sorted(documents)}, actual={sorted(actual)}"
        )
    return documents


def _cmake_files_evidence(
    *,
    project_id: str,
    cmake_files: Mapping[str, Any],
    expected_source_root: Path,
    build_root: Path,
    roots: _Roots,
) -> tuple[list[dict[str, Any]], dict[Path, dict[str, Any]]]:
    """Authenticate every file CMake reports reading during configure/generate."""

    if set(cmake_files) != {"kind", "version", "paths", "inputs"}:
        _fail(f"{project_id} cmakeFiles reply shape drifted")
    version = _object(cmake_files.get("version"), f"{project_id} cmakeFiles version")
    if cmake_files.get("kind") != "cmakeFiles" or version != {
        "major": 1,
        "minor": 0,
    }:
        _fail(f"{project_id} File API cmakeFiles version is not exact v1.0")
    paths = _object(cmake_files.get("paths"), f"{project_id} cmakeFiles paths")
    if set(paths) != {"source", "build"}:
        _fail(f"{project_id} cmakeFiles roots shape drifted")
    source = roots.decode(
        _text(paths.get("source"), f"{project_id} cmakeFiles source"),
        expected_source_root,
        f"{project_id} cmakeFiles source",
    )
    build = roots.decode(
        _text(paths.get("build"), f"{project_id} cmakeFiles build"),
        build_root,
        f"{project_id} cmakeFiles build",
    )
    if source.resolve(strict=True) != expected_source_root or build.resolve(strict=True) != build_root:
        _fail(f"{project_id} cmakeFiles roots disagree with the configured build")
    if _is_within(expected_source_root, roots.repository):
        source_git_boundary = roots.repository
    elif _is_within(expected_source_root, roots.output):
        source_git_boundary = roots.output
    else:
        _fail(f"{project_id} cmakeFiles source is outside the qualified roots")
    source_git_root = _containing_git_root(
        expected_source_root,
        source_git_boundary,
        f"{project_id} cmakeFiles source",
    )

    rows: list[dict[str, Any]] = []
    indexed: dict[Path, dict[str, Any]] = {}
    first_raw_indices: dict[Path, int] = {}
    for index, raw in enumerate(
        _list(cmake_files.get("inputs"), f"{project_id} cmakeFiles inputs")
    ):
        label = f"{project_id} cmakeFiles input[{index}]"
        item = _object(raw, label)
        if not set(item).issubset({"path", "isGenerated", "isExternal", "isCMake"}):
            _fail(f"{label} has unsupported keys")
        for flag in ("isGenerated", "isExternal", "isCMake"):
            if flag in item and item[flag] is not True:
                _fail(f"{label}.{flag} may only be present with value true")
        generated = item.get("isGenerated") is True
        external = item.get("isExternal") is True
        cmake_owned = item.get("isCMake") is True
        raw_path = _text(item.get("path"), f"{label}.path")
        pure = PurePosixPath(raw_path)
        if pure.is_absolute():
            if raw_path.startswith("//") or posixpath.normpath(raw_path) != raw_path:
                _fail(f"{label}.path is not canonical")
            path = roots.decode(raw_path, expected_source_root, f"{label}.path")
        else:
            if (
                "\\" in raw_path
                or raw_path != pure.as_posix()
                or posixpath.normpath(raw_path) != raw_path
                or any(part in {"", "."} for part in pure.parts)
            ):
                _fail(f"{label}.path is not a canonical source-relative path")
            path = Path(os.path.normpath(expected_source_root / Path(*pure.parts)))
            if not _is_within(path, source_git_root):
                _fail(f"{label}.path escapes its source Git root")
        path = _regular_file(
            path,
            label,
            contained_by=source_git_root if not pure.is_absolute() else None,
            require_single_link=False,
        )
        in_source = _is_within(path, expected_source_root)
        in_build = _is_within(path, build_root)
        relative_in_source_generation = (
            not pure.is_absolute() and _is_within(path, source_git_root)
        )
        if (pure.is_absolute() and in_source) or (
            not pure.is_absolute() and not relative_in_source_generation
        ):
            _fail(f"{label} does not use CMake's exact relative/absolute path form")
        expected_external = (
            pure.is_absolute() and not in_source and not in_build
        )
        if generated != in_build or external != expected_external:
            _fail(f"{label} scope flags disagree with its path")
        if cmake_owned and (
            not external
            or not _is_within(path, Path("/usr/share/cmake-3.25"))
        ):
            _fail(f"{label} has a false CMake-installation ownership claim")

        if not pure.is_absolute():
            identity = {
                "kind": "git_tracked_source",
                **_git_tracked_file_identity(path, source_git_root, label),
            }
        elif _is_within(path, roots.repository):
            git_root = _containing_git_root(path, roots.repository, label)
            identity = {
                "kind": "git_tracked_source",
                **_git_tracked_file_identity(path, git_root, label),
            }
        elif in_source:
            boundary = roots.output
            git_root = _containing_git_root(path, boundary, label)
            identity = {
                "kind": "git_tracked_source",
                **_git_tracked_file_identity(path, git_root, label),
            }
        elif in_build:
            identity = {
                "kind": "fresh_cmake_generated_build_input",
                "sha256": _sha256_file(path),
            }
        elif _is_within(path, Path("/usr")):
            identity = {
                "kind": "qualified_image_file",
                "sha256": _sha256_file(path),
            }
        elif _is_within(path, roots.output):
            identity = {
                "kind": "qualification_output_owner_required",
                "sha256": _sha256_file(path),
            }
        else:
            _fail(f"{label} is outside Git, the fresh build, the image, and qualification output")
        logical = roots.logical(path)
        row = {
            "path": logical,
            "sha256": _sha256_file(path),
            "is_generated": generated,
            "is_external": external,
            "is_cmake": cmake_owned,
            "identity": identity,
        }
        if path in indexed:
            if indexed[path] != row:
                _fail(
                    f"{project_id} cmakeFiles has conflicting repeats for "
                    f"{logical} at raw indices {first_raw_indices[path]} and {index}"
                )
            continue
        rows.append(row)
        indexed[path] = row
        first_raw_indices[path] = index
    if not rows:
        _fail(f"{project_id} cmakeFiles input inventory is empty")
    rows.sort(key=lambda row: row["path"])
    return rows, indexed


_GRAPHVIZ_LEGEND = """subgraph clusterLegend {
  label = "Legend";
  color = black;
  edge [ style = invis ];
  legendNode0 [ label = "Executable", shape = egg ];
  legendNode1 [ label = "Static Library", shape = octagon ];
  legendNode2 [ label = "Shared Library", shape = doubleoctagon ];
  legendNode3 [ label = "Module Library", shape = tripleoctagon ];
  legendNode4 [ label = "Interface Library", shape = pentagon ];
  legendNode5 [ label = "Object Library", shape = hexagon ];
  legendNode6 [ label = "Unknown Library", shape = septagon ];
  legendNode7 [ label = "Custom Target", shape = box ];
  legendNode0 -> legendNode1 [ style = solid ];
  legendNode0 -> legendNode2 [ style = solid ];
  legendNode0 -> legendNode3;
  legendNode1 -> legendNode4 [ label = "Interface", style = dashed ];
  legendNode2 -> legendNode5 [ label = "Private", style = dotted ];
  legendNode3 -> legendNode6 [ style = solid ];
  legendNode0 -> legendNode7;
}""".splitlines()


def _parse_target_graph(path: Path, project_id: str) -> dict[str, Any]:
    raw = _read_bytes(path, f"{project_id} configured-target graph")
    try:
        lines = raw.decode("utf-8").splitlines()
    except UnicodeDecodeError as error:
        _fail(f"{project_id} configured-target graph is not UTF-8: {error}")
    prefix = [
        'digraph "manyforge_i2" {',
        'node [ fontsize = "12" ];',
        *_GRAPHVIZ_LEGEND,
    ]
    if lines[: len(prefix)] != prefix or not lines or lines[-1] != "}":
        _fail(f"{project_id} configured-target graph header/legend/footer drifted")
    body = lines[len(prefix) : -1]
    node_pattern = re.compile(
        r'^    "(i2node[0-9]+)" \[ label = "((?:[^"\\]|\\.)*)", '
        r'shape = (egg|octagon|doubleoctagon|tripleoctagon|pentagon|hexagon|septagon|box) \];$'
    )
    edge_pattern = re.compile(
        r'^    "(i2node[0-9]+)" -> "(i2node[0-9]+)" '
        r'(?:\[ style = (dotted|dashed) \])? // (.*)$'
    )
    nodes_by_id: dict[str, dict[str, str]] = {}
    raw_edges: list[tuple[str, str, str, str]] = []
    for line in body:
        node_match = node_pattern.fullmatch(line)
        if node_match is not None:
            node_id, label, shape = node_match.groups()
            if node_id in nodes_by_id or shape not in _GRAPHVIZ_SHAPES:
                _fail(f"{project_id} configured-target graph duplicates a node")
            primary = label.split(r"\n(", 1)[0]
            name = primary.replace(r'\"', '"')
            if not name or any(ord(character) < 32 for character in name):
                _fail(f"{project_id} configured-target graph has a malformed label")
            nodes_by_id[node_id] = {"name": name, "label": label, "shape": shape}
            continue
        edge_match = edge_pattern.fullmatch(line)
        if edge_match is None:
            _fail(f"{project_id} configured-target graph has unsupported DOT syntax")
        source, target, style, comment = edge_match.groups()
        raw_edges.append((source, target, style or "solid", comment))
    if not nodes_by_id:
        _fail(f"{project_id} configured-target graph is empty")
    names = [node["name"] for node in nodes_by_id.values()]
    if len(names) != len(set(names)):
        _fail(f"{project_id} configured-target graph duplicates a target name")
    edges: list[dict[str, str]] = []
    for source_id, target_id, style, comment in raw_edges:
        if source_id not in nodes_by_id or target_id not in nodes_by_id:
            _fail(f"{project_id} configured-target graph has a dangling edge")
        source = nodes_by_id[source_id]["name"]
        target = nodes_by_id[target_id]["name"]
        if comment != f"{source} -> {target}":
            _fail(f"{project_id} configured-target graph edge comment disagrees")
        edges.append({"source": source, "target": target, "style": style})
    edge_identities = [
        (edge["source"], edge["target"], edge["style"]) for edge in edges
    ]
    if len(edge_identities) != len(set(edge_identities)):
        _fail(f"{project_id} configured-target graph duplicates an edge")
    nodes = sorted(nodes_by_id.values(), key=lambda row: row["name"])
    edges.sort(key=lambda row: (row["source"], row["target"], row["style"]))
    projection = {"nodes": nodes, "edges": edges}
    return {
        "path": path.as_posix(),
        "sha256": _sha256_bytes(raw),
        "projection_sha256": _canonical_sha256(projection),
        **projection,
    }


def _normalize_target_graph_owner_nodes(
    *,
    project_id: str,
    build_root: Path,
    roots: "_Roots",
    evidence: dict[str, Any],
    alias_contracts: Mapping[str, Any],
) -> None:
    """Canonicalize exact binding owner nodes without lossy path aliases."""

    raw_projection_sha256 = _text(
        evidence.get("projection_sha256"), f"{project_id} raw graph projection"
    )
    evidence["raw_projection_sha256"] = raw_projection_sha256
    if project_id not in alias_contracts:
        evidence["owner_node_aliases"] = []
        return
    contract = _object(
        alias_contracts[project_id], f"{project_id} graph owner-node aliases"
    )
    if set(contract) != {"absolute_build_sibling_nodes", "named_nodes"}:
        _fail(f"{project_id} graph owner-node alias contract keys drifted")
    aliases: dict[str, str] = {}
    expected_shapes: dict[str, str] = {}
    alias_rows: list[dict[str, str]] = []
    for index, raw in enumerate(
        _list(
            contract.get("absolute_build_sibling_nodes"),
            f"{project_id} absolute graph owner nodes",
        )
    ):
        label = f"{project_id} absolute graph owner node[{index}]"
        row = _object(raw, label)
        if set(row) != {
            "sibling_build_directory",
            "artifact_relative_path",
            "alias",
            "expected_raw_shape",
        }:
            _fail(f"{label} keys drifted")
        sibling = _text(row.get("sibling_build_directory"), f"{label}.sibling")
        if re.fullmatch(r"[a-z][a-z0-9_]*", sibling) is None:
            _fail(f"{label}.sibling is malformed")
        artifact = _safe_relative(
            row.get("artifact_relative_path"), f"{label}.artifact"
        )
        source = roots.logical(build_root.parent / sibling / artifact)
        alias = _text(row.get("alias"), f"{label}.alias")
        expected_shape = _text(
            row.get("expected_raw_shape"), f"{label}.expected_raw_shape"
        )
        if (
            source in aliases
            or not re.fullmatch(r"<owner:[a-z0-9_]+/[a-z0-9_]+>", alias)
            or expected_shape not in _GRAPHVIZ_SHAPES
        ):
            _fail(f"{label} source/alias is malformed or duplicated")
        aliases[source] = alias
        expected_shapes[source] = expected_shape
        alias_rows.append(
            {"source": source, "alias": alias, "expected_raw_shape": expected_shape}
        )
    for index, raw in enumerate(
        _list(contract.get("named_nodes"), f"{project_id} named graph owner nodes")
    ):
        label = f"{project_id} named graph owner node[{index}]"
        row = _object(raw, label)
        if set(row) != {"source", "alias", "expected_raw_shape"}:
            _fail(f"{label} keys drifted")
        source = _text(row.get("source"), f"{label}.source")
        alias = _text(row.get("alias"), f"{label}.alias")
        expected_shape = _text(
            row.get("expected_raw_shape"), f"{label}.expected_raw_shape"
        )
        if (
            source in aliases
            or not re.fullmatch(r"[A-Za-z0-9_:.-]+", source)
            or not re.fullmatch(r"<owner:[a-z0-9_]+/[a-z0-9_]+>", alias)
            or expected_shape not in _GRAPHVIZ_SHAPES
        ):
            _fail(f"{label} source/alias is malformed or duplicated")
        aliases[source] = alias
        expected_shapes[source] = expected_shape
        alias_rows.append(
            {"source": source, "alias": alias, "expected_raw_shape": expected_shape}
        )
    if (
        not aliases
        or len(set(aliases.values())) != len(aliases)
        or alias_rows
        != sorted(alias_rows, key=lambda row: (row["source"], row["alias"]))
    ):
        _fail(f"{project_id} graph owner-node aliases are not exact/sorted")
    raw_nodes = _list(evidence.get("nodes"), f"{project_id} raw graph nodes")
    raw_names = {
        _text(_object(node, f"{project_id} raw graph node").get("name"), "graph node")
        for node in raw_nodes
    }
    if not set(aliases).issubset(raw_names):
        _fail(f"{project_id} graph omits a contracted owner-node alias source")
    unexpected_output_nodes = {
        name
        for name in raw_names
        if name.startswith("/qualification-output/") and name not in aliases
    }
    if unexpected_output_nodes:
        _fail(
            f"{project_id} graph contains uncontracted qualification-output nodes: "
            f"{sorted(unexpected_output_nodes)}"
        )

    normalized_nodes: list[dict[str, str]] = []
    for raw_node in raw_nodes:
        node = dict(_object(raw_node, f"{project_id} raw graph node"))
        source = _text(node.get("name"), f"{project_id} raw graph node name")
        if source in aliases:
            label = _text(node.get("label"), f"{project_id} raw graph node label")
            if (
                not label.startswith(source)
                or node.get("shape") != expected_shapes[source]
            ):
                _fail(f"{project_id} owner-node label does not start with its name")
            node["name"] = aliases[source]
            node["label"] = aliases[source] + label[len(source) :]
        normalized_nodes.append(node)
    normalized_edges: list[dict[str, str]] = []
    for raw_edge in _list(evidence.get("edges"), f"{project_id} raw graph edges"):
        edge = dict(_object(raw_edge, f"{project_id} raw graph edge"))
        edge["source"] = aliases.get(edge["source"], edge["source"])
        edge["target"] = aliases.get(edge["target"], edge["target"])
        normalized_edges.append(edge)
    normalized_nodes.sort(key=lambda row: row["name"])
    normalized_edges.sort(
        key=lambda row: (row["source"], row["target"], row["style"])
    )
    normalized_names = [node["name"] for node in normalized_nodes]
    if len(normalized_names) != len(set(normalized_names)):
        _fail(f"{project_id} normalized graph aliases collide")
    normalized_projection = {
        "nodes": normalized_nodes,
        "edges": normalized_edges,
    }
    evidence.update(normalized_projection)
    evidence["projection_sha256"] = _canonical_sha256(normalized_projection)
    evidence["owner_node_aliases"] = alias_rows


def _target_graph_evidence(
    *,
    project_id: str,
    build_testing: bool,
    build_root: Path,
    roots: _Roots,
    build_contract: Mapping[str, Any],
) -> dict[str, Any]:
    options = _regular_file(
        build_root / "CMakeGraphVizOptions.cmake",
        f"{project_id} Graphviz options",
        contained_by=build_root,
    )
    if _read_bytes(options, f"{project_id} Graphviz options") != _GRAPHVIZ_OPTIONS:
        _fail(f"{project_id} Graphviz options bytes drifted")
    graph_path = _regular_file(
        build_root / "manyforge-i2-target-graph.dot",
        f"{project_id} configured-target graph",
        contained_by=build_root,
    )
    evidence = _parse_target_graph(graph_path, project_id)
    evidence["path"] = roots.logical(graph_path)
    evidence["options"] = {
        "path": roots.logical(options),
        "sha256": _sha256_file(options),
    }
    contract = _object(
        _object(build_contract, "build-provenance contract").get(
            "configured_target_graph_contract"
        ),
        "configured_target_graph_contract",
    )
    if set(contract) != {
        "schema_version",
        "options_sha256",
        "project_node_alias_contracts",
        "profiles",
    } or contract.get(
        "schema_version"
    ) != "manyforge.composable_cell_i2.configured_target_graph_contract.v1":
        _fail("configured-target graph contract shape/schema drifted")
    if contract.get("options_sha256") != _sha256_bytes(_GRAPHVIZ_OPTIONS):
        _fail("configured-target Graphviz options digest drifted")
    alias_contracts = _object(
        contract.get("project_node_alias_contracts"),
        "configured-target graph owner-node aliases",
    )
    if set(alias_contracts) != {"manyforge_core_py"}:
        _fail("configured-target graph owner-node alias project set drifted")
    _normalize_target_graph_owner_nodes(
        project_id=project_id,
        build_root=build_root,
        roots=roots,
        evidence=evidence,
        alias_contracts=alias_contracts,
    )
    profile_id = f"{project_id}.testing_{'on' if build_testing else 'off'}"
    profiles = _object(contract.get("profiles"), "configured-target graph profiles")
    if profile_id not in profiles:
        _fail(f"configured-target graph contract lacks profile {profile_id}")
    profile = _object(profiles[profile_id], f"configured-target graph {profile_id}")
    if set(profile) != {
        "project_id",
        "build_testing",
        "projection_sha256",
        "local_target_roles",
    }:
        _fail(f"configured-target graph profile {profile_id} keys drifted")
    if (
        profile.get("project_id") != project_id
        or profile.get("build_testing") is not build_testing
        or profile.get("projection_sha256") != evidence["projection_sha256"]
    ):
        _fail(f"configured-target graph profile {profile_id} drifted")
    roles = _object(profile.get("local_target_roles"), f"{profile_id} target roles")
    if set(roles) != {"production", "test_only", "configured_only", "external"}:
        _fail(f"configured-target graph profile {profile_id} role keys drifted")
    role_names: list[str] = []
    for role in ("production", "test_only", "configured_only", "external"):
        values = _list(roles.get(role), f"{profile_id} {role} targets")
        if values != sorted(values) or len(values) != len(set(values)):
            _fail(f"configured-target graph profile {profile_id} {role} is not exact/sorted")
        role_names.extend(_text(value, f"{profile_id} {role} target") for value in values)
    graph_names = {node["name"] for node in evidence["nodes"]}
    if len(role_names) != len(set(role_names)) or set(role_names) != graph_names:
        _fail(f"configured-target graph profile {profile_id} local roles drifted")
    production = set(_list(roles["production"], f"{profile_id} production roles"))
    forbidden = set(_list(roles["test_only"], f"{profile_id} test roles")) | set(
        _list(roles["configured_only"], f"{profile_id} configured-only roles")
    )
    projects = _object(
        build_contract.get("projects"), "configured-target graph project catalog"
    )
    if project_id in projects:
        product_contract = _production_contract(
            build_contract,
            expected_project=project_id,
            repository_root=roots.repository,
        )
        if production != set(product_contract.targets):
            _fail(
                f"configured-target graph profile {profile_id} production role set "
                "disagrees with the authoritative product target catalog"
            )
    adjacency: dict[str, set[str]] = {}
    for edge in evidence["edges"]:
        adjacency.setdefault(edge["source"], set()).add(edge["target"])
    if project_id in alias_contracts:
        pending = [project_id]
        reachable: set[str] = set()
        while pending:
            current = pending.pop()
            if current in reachable:
                continue
            reachable.add(current)
            pending.extend(adjacency.get(current, set()) - reachable)
        required_owners = {
            row["alias"]
            for row in _list(
                evidence.get("owner_node_aliases"),
                f"{project_id} normalized owner-node aliases",
            )
        }
        if not required_owners.issubset(reachable):
            _fail(
                f"{project_id} module graph does not reach all authenticated owners: "
                f"{sorted(required_owners - reachable)}"
            )
    for root in production:
        pending = [root]
        visited: set[str] = set()
        while pending:
            current = pending.pop()
            if current in visited:
                continue
            visited.add(current)
            pending.extend(adjacency.get(current, set()) - visited)
        reached = visited.intersection(forbidden)
        if reached:
            _fail(
                f"production graph target {root} reaches test-only targets "
                f"{sorted(reached)}"
            )
    evidence["profile_id"] = profile_id
    evidence["local_target_roles"] = dict(roles)
    return evidence


def _native_configured_file_api_target_names(
    target_graph: Mapping[str, Any],
    label: str,
) -> set[str]:
    roles = _object(
        target_graph.get("local_target_roles"),
        f"{label} configured-target roles",
    )
    if set(roles) != {"production", "test_only", "configured_only", "external"}:
        _fail(f"{label} configured-target role keys drifted")
    names: list[str] = []
    for role in ("production", "test_only", "configured_only"):
        names.extend(
            _text(value, f"{label} configured-target {role} role")
            for value in _list(
                roles.get(role),
                f"{label} configured-target {role} roles",
            )
        )
    if len(names) != len(set(names)):
        _fail(f"{label} configured-target local roles overlap")
    return set(names)


def _validate_native_configured_file_api_target_set(
    configured_targets: set[str],
    target_graph: Mapping[str, Any],
    label: str,
) -> None:
    expected = _native_configured_file_api_target_names(target_graph, label)
    if configured_targets != expected:
        _fail(
            f"{label} configured target set drifted: "
            f"expected={sorted(expected)}, actual={sorted(configured_targets)}"
        )


def _validate_built_material_target_partition(
    existing: set[str],
    required: set[str],
    label: str,
) -> None:
    if existing != required:
        _fail(
            f"{label} built material-target partition drifted: "
            f"expected={sorted(required)}, actual={sorted(existing)}"
        )


def _toolchain_contract(value: Mapping[str, Any]) -> Mapping[str, Any]:
    outer = _object(value, "build-provenance contract")
    if outer.get("schema_version") != "manyforge.composable_cell_i2.build_provenance_contract.v1":
        _fail("toolchain contract requires the whole build-provenance contract")
    raw = _object(outer.get("toolchain_contract"), "toolchain_contract")
    if set(raw) != {
        "schema_version",
        "cmake",
        "git",
        "python",
        "cxx_compiler",
        "archiver",
        "ranlib",
        "ninja",
        "compile_modes",
    } or raw.get("schema_version") != "manyforge.composable_cell_i2.toolchain_contract.v1":
        _fail("toolchain contract shape/schema drifted")
    for name in (
        "cmake",
        "git",
        "python",
        "cxx_compiler",
        "archiver",
        "ranlib",
        "ninja",
    ):
        tool = _object(raw.get(name), f"toolchain {name}")
        expected_keys = {"lexical_path", "resolved_path", "sha256"}
        if name in {"cmake", "git", "python", "cxx_compiler"}:
            expected_keys.add("version_first_line")
        if set(tool) != expected_keys:
            _fail(f"toolchain {name} keys drifted")
        lexical = _text(tool.get("lexical_path"), f"toolchain {name} lexical path")
        resolved = _text(tool.get("resolved_path"), f"toolchain {name} resolved path")
        digest = _text(tool.get("sha256"), f"toolchain {name} digest")
        if (
            not lexical.startswith(
                "/usr/local/bin/" if name == "python" else "/usr/bin/"
            )
            or not resolved.startswith(
                "/usr/local/bin/" if name == "python" else "/usr/bin/"
            )
            or not _HEX64.fullmatch(digest)
        ):
            _fail(f"toolchain {name} identity is malformed")
        if name in {"cmake", "git", "python", "cxx_compiler"}:
            _text(tool.get("version_first_line"), f"toolchain {name} version")
    modes = _object(raw.get("compile_modes"), "toolchain compile modes")
    if set(modes) != {
        "cxx_standard_flag",
        "project_source_required_flags",
        "Release",
        "RelWithDebInfo",
    }:
        _fail("toolchain compile-mode set drifted")
    if modes.get("cxx_standard_flag") != "-std=c++20":
        _fail("toolchain compile standard is not exact C++20")
    required_flags = _object(
        modes.get("project_source_required_flags"),
        "toolchain project source required flags",
    )
    if set(required_flags) != {"manyforge_roboplan", "roboplan"}:
        _fail("toolchain project source compiler-flag project set drifted")
    for project_id, raw_project in required_flags.items():
        project = _object(
            raw_project, f"toolchain project source flags {project_id}"
        )
        if set(project) != {"-fopenmp"}:
            _fail(f"toolchain project source flags {project_id} drifted")
        paths = _list(
            project["-fopenmp"],
            f"toolchain project source flags {project_id} -fopenmp",
        )
        if (
            not paths
            or paths != sorted(paths)
            or len(paths) != len(set(paths))
            or any(
                _safe_relative(path, f"{project_id} -fopenmp source").suffix
                not in _CODE_SOURCE_SUFFIXES
                for path in paths
            )
        ):
            _fail(f"toolchain project source flags {project_id} paths drifted")
    for configuration, expected in {
        "Release": ("-O3", None),
        "RelWithDebInfo": ("-O2", "-g"),
    }.items():
        mode = _object(modes.get(configuration), f"toolchain mode {configuration}")
        if set(mode) != {"optimization_flag", "debug_flag", "require_ndebug"} or mode != {
            "optimization_flag": expected[0],
            "debug_flag": expected[1],
            "require_ndebug": True,
        }:
            _fail(f"toolchain mode {configuration} drifted")
    return raw


_CROSS_PROFILE_COMMAND_KINDS = ("compile", "link", "archive", "generated")


def _command_delta_catalog(value: Any, label: str) -> dict[str, dict[str, int]]:
    raw = _object(value, label)
    if set(raw) != set(_CROSS_PROFILE_COMMAND_KINDS):
        _fail(f"{label} command-kind set drifted")
    result: dict[str, dict[str, int]] = {}
    for kind in _CROSS_PROFILE_COMMAND_KINDS:
        rows = _list(raw.get(kind), f"{label}.{kind}")
        parsed: dict[str, int] = {}
        ordered_tokens: list[str] = []
        for index, raw_row in enumerate(rows):
            row_label = f"{label}.{kind}[{index}]"
            row = _object(raw_row, row_label)
            if set(row) != {"token", "count"}:
                _fail(f"{row_label} keys drifted")
            token = _text(row.get("token"), f"{row_label}.token")
            count = row.get("count")
            if (
                token in parsed
                or not token.startswith("-")
                or not isinstance(count, int)
                or isinstance(count, bool)
                or count < 1
            ):
                _fail(f"{row_label} token/count is malformed or duplicated")
            parsed[token] = count
            ordered_tokens.append(token)
        if ordered_tokens != sorted(ordered_tokens):
            _fail(f"{label}.{kind} token catalog is not exact/sorted")
        result[kind] = parsed
    return result


def _cross_profile_command_contract(
    value: Mapping[str, Any],
    *,
    lane_id: str,
    project_id: str,
    configuration: str,
) -> tuple[dict[str, dict[str, int]], frozenset[str]]:
    """Decode the only command tokens permitted to differ between profiles."""

    outer = _object(value, "build-provenance contract")
    raw = _object(
        outer.get("cross_profile_parity_contract"),
        "cross_profile_parity_contract",
    )
    if set(raw) != {
        "schema_version",
        "path_normalization_policy",
        "configuration_command_deltas",
        "sanitizer_lane_command_deltas",
        "comparisons",
    } or raw.get("schema_version") != (
        "manyforge.composable_cell_i2.cross_profile_parity_contract.v1"
    ):
        _fail("cross-profile parity contract shape/schema drifted")
    if raw.get("path_normalization_policy") != (
        "exact_lane_local_longest_prefix_preserve_suffix_and_reject_"
        "unaliased_qualification_output"
    ):
        _fail("cross-profile path-normalization policy drifted")

    configurations = _object(
        raw.get("configuration_command_deltas"),
        "cross-profile configuration deltas",
    )
    if set(configurations) != {"Release", "RelWithDebInfo"}:
        _fail("cross-profile configuration set drifted")
    configuration_rows = {
        name: _command_delta_catalog(
            configurations[name], f"cross-profile configuration {name}"
        )
        for name in sorted(configurations)
    }
    expected_configurations = {
        "Release": {
            "compile": {"-O3": 1},
            "link": {"-O3": 1},
            "archive": {},
            "generated": {},
        },
        "RelWithDebInfo": {
            "compile": {"-O2": 1, "-g": 1},
            "link": {"-O2": 1, "-g": 1},
            "archive": {},
            "generated": {},
        },
    }
    if configuration_rows != expected_configurations or configuration not in configuration_rows:
        _fail("cross-profile configuration command deltas drifted")

    sanitizer_lanes = _object(
        raw.get("sanitizer_lane_command_deltas"),
        "cross-profile sanitizer lane deltas",
    )
    expected_lane_profiles = {
        "bindings_asan_ubsan": "address+undefined",
        "bindings_tsan": "thread",
        "core_asan_ubsan": "address+undefined",
        "core_tsan": "thread",
        "deployment_asan_ubsan": "address+undefined",
        "roboplan_asan_ubsan": "address+undefined",
        "roboplan_tsan": "thread",
    }
    if set(sanitizer_lanes) != set(expected_lane_profiles):
        _fail("cross-profile sanitizer lane set drifted")

    no_warning_asan = {
        "compile": {
            "-fno-omit-frame-pointer": 1,
            "-fsanitize=address,undefined": 1,
        },
        "link": {
            "-fno-omit-frame-pointer": 1,
            "-fsanitize=address,undefined": 2,
        },
        "archive": {},
        "generated": {},
    }
    no_warning_tsan = {
        "compile": {"-fno-omit-frame-pointer": 1, "-fsanitize=thread": 1},
        "link": {"-fno-omit-frame-pointer": 1, "-fsanitize=thread": 2},
        "archive": {},
        "generated": {},
    }
    core_asan = {
        "compile": {
            "-Wno-error=array-bounds": 1,
            "-Wno-error=maybe-uninitialized": 1,
            **no_warning_asan["compile"],
        },
        "link": {
            "-Wno-error=array-bounds": 1,
            "-Wno-error=maybe-uninitialized": 1,
            **no_warning_asan["link"],
        },
        "archive": {},
        "generated": {},
    }
    core_tsan = {
        "compile": {
            "-Wno-error=array-bounds": 1,
            **no_warning_tsan["compile"],
        },
        "link": {
            "-Wno-error=array-bounds": 1,
            **no_warning_tsan["link"],
        },
        "archive": {},
        "generated": {},
    }
    roboplan_asan = {
        "compile": dict(no_warning_asan["compile"]),
        "link": {
            "-fno-omit-frame-pointer": 2,
            "-fsanitize=address,undefined": 2,
        },
        "archive": {},
        "generated": {},
    }
    roboplan_tsan = {
        "compile": dict(no_warning_tsan["compile"]),
        "link": {
            "-fno-omit-frame-pointer": 2,
            "-fsanitize=thread": 2,
        },
        "archive": {},
        "generated": {},
    }
    expected_common = {
        "bindings_asan_ubsan": no_warning_asan,
        "bindings_tsan": no_warning_tsan,
        "core_asan_ubsan": core_asan,
        "core_tsan": core_tsan,
        "deployment_asan_ubsan": core_asan,
        "roboplan_asan_ubsan": roboplan_asan,
        "roboplan_tsan": roboplan_tsan,
    }
    eigen_delta = {
        "compile": {"-DEIGEN_MALLOC_ALREADY_ALIGNED=1": 1},
        "link": {"-DEIGEN_MALLOC_ALREADY_ALIGNED=1": 2},
        "archive": {},
        "generated": {},
    }
    expected_project_deltas = {
        lane: (
            {
                "manyforge_roboplan": eigen_delta,
                "roboplan": eigen_delta,
                "roboplan_example_models": eigen_delta,
            }
            if lane == "roboplan_asan_ubsan"
            else {}
        )
        for lane in expected_lane_profiles
    }

    decoded_lanes: dict[
        str, tuple[str, dict[str, dict[str, int]], dict[str, dict[str, dict[str, int]]]]
    ] = {}
    known_tokens = {
        token
        for command_rows in configuration_rows.values()
        for row in command_rows.values()
        for token in row
    }
    for selected_lane in sorted(sanitizer_lanes):
        label = f"cross-profile sanitizer lane {selected_lane}"
        row = _object(sanitizer_lanes[selected_lane], label)
        if set(row) != {"sanitizer", "common", "projects"}:
            _fail(f"{label} keys drifted")
        sanitizer = _text(row.get("sanitizer"), f"{label}.sanitizer")
        common = _command_delta_catalog(row.get("common"), f"{label}.common")
        raw_projects = _object(row.get("projects"), f"{label}.projects")
        projects = {
            _text(name, f"{label}.project_id"): _command_delta_catalog(
                raw_project, f"{label}.projects.{name}"
            )
            for name, raw_project in raw_projects.items()
        }
        if (
            sanitizer != expected_lane_profiles[selected_lane]
            or common != expected_common[selected_lane]
            or projects != expected_project_deltas[selected_lane]
        ):
            _fail(f"{label} exact token/multiplicity contract drifted")
        decoded_lanes[selected_lane] = (sanitizer, common, projects)
        known_tokens.update(
            token for command_rows in (common, *projects.values())
            for delta_row in command_rows.values() for token in delta_row
        )

    comparisons = _list(raw.get("comparisons"), "cross-profile comparisons")
    if len(comparisons) != 5:
        _fail("cross-profile comparison catalog must contain exactly five rows")
    comparison_ids: list[str] = []
    for index, raw_comparison in enumerate(comparisons):
        label = f"cross-profile comparisons[{index}]"
        comparison = _object(raw_comparison, label)
        if set(comparison) != {
            "comparison_id",
            "kind",
            "lane_ids",
            "target_owners",
        }:
            _fail(f"{label} keys drifted")
        comparison_id = _text(comparison.get("comparison_id"), f"{label}.id")
        if comparison_id in comparison_ids:
            _fail(f"{label} repeats a comparison id")
        comparison_ids.append(comparison_id)
        if comparison.get("kind") not in {
            "native_cross_profile",
            "binding_cross_profile",
            "native_same_profile_exact",
        }:
            _fail(f"{label} kind drifted")
        lane_values = _list(comparison.get("lane_ids"), f"{label}.lane_ids")
        owner_values = _list(
            comparison.get("target_owners"), f"{label}.target_owners"
        )
        if (
            len(lane_values) < 2
            or len(lane_values) != len(set(lane_values))
            or not owner_values
        ):
            _fail(f"{label} lane/owner catalog is empty or duplicated")
        owners: list[tuple[str, str]] = []
        for owner_index, raw_owner in enumerate(owner_values):
            owner_label = f"{label}.target_owners[{owner_index}]"
            owner = _object(raw_owner, owner_label)
            if set(owner) != {"project_id", "target"}:
                _fail(f"{owner_label} keys drifted")
            owners.append(
                (
                    _text(owner.get("project_id"), f"{owner_label}.project_id"),
                    _text(owner.get("target"), f"{owner_label}.target"),
                )
            )
        if owners != sorted(owners) or len(owners) != len(set(owners)):
            _fail(f"{label} owner catalog is not exact/sorted")
    if comparison_ids != [
        "core_release_asan_tsan",
        "deployment_release_asan",
        "binding_release_asan_tsan",
        "roboplan_scene_asan_tsan",
        "roboplan_release_focused",
    ]:
        _fail("cross-profile comparison ordering/identity drifted")

    selected = {
        kind: dict(configuration_rows[configuration][kind])
        for kind in _CROSS_PROFILE_COMMAND_KINDS
    }
    if lane_id in decoded_lanes:
        _sanitizer, common, projects = decoded_lanes[lane_id]
        for kind in _CROSS_PROFILE_COMMAND_KINDS:
            for token, count in common[kind].items():
                if token in selected[kind]:
                    _fail("cross-profile common/configuration token overlap")
                selected[kind][token] = count
            for token, count in projects.get(project_id, {}).get(kind, {}).items():
                if token in selected[kind]:
                    _fail("cross-profile project/common token overlap")
                selected[kind][token] = count
    return selected, frozenset(known_tokens)


def _file_api_target_sources(
    *,
    project_id: str,
    target_name: str,
    target_type: str,
    target_value: Mapping[str, Any],
    model_source: Path,
    target_build_base: Path,
    allowed_source_roots: Sequence[Path],
    build_root: Path,
    roots: _Roots,
) -> tuple[tuple[Path, ...], tuple[_VirtualSourceReply, ...]]:
    """Decode physical sources and the exact pinned CMake virtual exceptions."""

    raw_sources = _list(
        target_value.get("sources", []), f"{project_id} target {target_name} sources"
    )
    raw_source_groups = _list(
        target_value.get("sourceGroups", []),
        f"{project_id} target {target_name} source groups",
    )
    sources: list[Path] = []
    generated_sources: set[Path] = set()
    virtual_sources: list[_VirtualSourceReply] = []

    for source_index, raw_source in enumerate(raw_sources):
        label = f"{project_id} target {target_name} source[{source_index}]"
        source = _object(raw_source, label)
        generated = source.get("isGenerated", False)
        if not isinstance(generated, bool):
            _fail(f"{project_id} target {target_name} source has malformed isGenerated")

        source_group_index: int | None = None
        source_group: str | None = None
        if "sourceGroupIndex" in source:
            raw_group_index = source.get("sourceGroupIndex")
            if (
                not isinstance(raw_group_index, int)
                or isinstance(raw_group_index, bool)
                or raw_group_index < 0
                or raw_group_index >= len(raw_source_groups)
            ):
                _fail(
                    f"{project_id} target {target_name} source group index is malformed"
                )
            source_group_index = raw_group_index
            group = _object(
                raw_source_groups[source_group_index],
                f"{project_id} target {target_name} source group[{source_group_index}]",
            )
            group_name = group.get("name")
            if not isinstance(group_name, str) or "\x00" in group_name:
                _fail(
                    f"{project_id} target {target_name} source group has malformed name"
                )
            source_group = group_name

        path = _resolve_declared_path(
            source.get("path"),
            build_root if generated else model_source,
            roots,
            f"{project_id} target {target_name} source",
        )
        if not (
            any(_is_within(path, source_root) for source_root in allowed_source_roots)
            or _is_within(path, build_root)
        ):
            _fail(f"{project_id} target {target_name} source escapes source/build roots")

        virtual_source = _classify_cmake_virtual_source(
            target_name=target_name,
            target_type=target_type,
            build_root=build_root,
            target_build_base=target_build_base,
            path=path,
            generated=generated,
            compile_group_present="compileGroupIndex" in source,
            source_group=source_group,
        )
        if virtual_source is not None:
            memberships: list[int] = []
            for group_index, raw_group in enumerate(raw_source_groups):
                group = _object(
                    raw_group,
                    f"{project_id} target {target_name} source group[{group_index}]",
                )
                raw_indexes = _list(
                    group.get("sourceIndexes"),
                    f"{project_id} target {target_name} source group[{group_index}] indexes",
                )
                indexes: list[int] = []
                for raw_index in raw_indexes:
                    if (
                        not isinstance(raw_index, int)
                        or isinstance(raw_index, bool)
                        or raw_index < 0
                        or raw_index >= len(raw_sources)
                    ):
                        _fail(
                            f"{project_id} target {target_name} source group membership is malformed"
                        )
                    indexes.append(raw_index)
                if len(indexes) != len(set(indexes)):
                    _fail(
                        f"{project_id} target {target_name} source group membership is duplicated"
                    )
                if source_index in indexes:
                    memberships.append(group_index)
            if source_group_index is None or memberships != [source_group_index]:
                _fail(
                    f"{project_id} target {target_name} CMake virtual source group membership drifted"
                )
            try:
                path.lstat()
            except FileNotFoundError:
                pass
            except OSError as error:
                _fail(
                    f"{project_id} target {target_name} cannot inspect CMake virtual source: {error}"
                )
            else:
                _fail(
                    f"{project_id} target {target_name} CMake virtual source exists physically"
                )
            virtual_sources.append(virtual_source)
            continue

        compile_group = source.get("compileGroupIndex")
        if path.suffix in _CODE_SOURCE_SUFFIXES and (
            not isinstance(compile_group, int)
            or isinstance(compile_group, bool)
            or compile_group < 0
        ):
            _fail(f"{project_id} target {target_name} code source has no compile group")
        sources.append(path)
        if generated:
            generated_sources.add(path)

    virtual_paths = [source.path for source in virtual_sources]
    if (
        len(sources) != len(set(sources))
        or len(virtual_paths) != len(set(virtual_paths))
        or set(sources).intersection(virtual_paths)
    ):
        _fail(f"{project_id} target {target_name} repeats a source")
    if target_type in _MATERIAL_TARGET_TYPES:
        for virtual_source in virtual_sources:
            if (
                virtual_source.kind != "cmake_rule"
                or virtual_source.owner not in generated_sources
            ):
                _fail(
                    f"{project_id} target {target_name} has an orphan CMake virtual source"
                )
    elif target_type != "UTILITY" and virtual_sources:
        _fail(
            f"{project_id} target {target_name} has unsupported CMake virtual sources"
        )
    return tuple(sources), tuple(virtual_sources)


def _load_build(
    *,
    lane_id: str,
    project_id: str,
    build_root: Path,
    expected_source_root: Path,
    expected_configuration: str,
    expected_build_testing: bool,
    roots: _Roots,
    responses: _ResponseCollector,
    toolchain_contract: Mapping[str, Any],
    build_provenance_contract: Mapping[str, Any],
    configure_generated_contracts: Mapping[
        str, _ConfigureGeneratedInputContract
    ],
    retain_configure_fixture_replay: bool,
    build_testing_cache_key: str = "BUILD_TESTING",
) -> _Build:
    build_root = _directory(build_root, f"{project_id} build root")
    expected_source_root = _directory(expected_source_root, f"{project_id} source root")
    if not _is_within(build_root, roots.output):
        _fail(f"{project_id} build root escapes the qualification output")
    allowed_source_roots = [expected_source_root]
    if _is_within(expected_source_root, roots.repository):
        allowed_source_roots = [roots.repository]

    cache, cache_sha = _parse_cache(build_root / "CMakeCache.txt", f"{project_id} CMake cache")
    if _cache_value(cache, "CMAKE_GENERATOR", project_id) != "Ninja":
        _fail(f"{project_id} was not generated by Ninja")
    if _cache_value(cache, "CMAKE_BUILD_TYPE", project_id) != expected_configuration:
        _fail(f"{project_id} has the wrong single-config build type")
    if _cache_value(cache, "CMAKE_EXPORT_COMPILE_COMMANDS", project_id).upper() not in {
        "1",
        "ON",
        "TRUE",
        "YES",
    }:
        _fail(f"{project_id} did not export compile_commands.json")
    expected_testing = "ON" if expected_build_testing else "OFF"
    if _cache_value(cache, build_testing_cache_key, project_id).upper() != expected_testing:
        _fail(
            f"{project_id} {build_testing_cache_key} differs from {expected_testing}"
        )
    home = roots.decode(
        _cache_value(cache, "CMAKE_HOME_DIRECTORY", project_id),
        build_root,
        f"{project_id} CMAKE_HOME_DIRECTORY",
    )
    if home.resolve(strict=True) != expected_source_root:
        _fail(f"{project_id} CMake source root is not the contracted source")
    toolchain = _object(toolchain_contract, f"{project_id} toolchain contract")

    def exact_tool(cache_value: str, tool_name: str, label: str) -> Path:
        tool = _object(toolchain.get(tool_name), f"{project_id} {label} contract")
        lexical = _text(
            tool.get("lexical_path"), f"{project_id} {label} lexical path"
        )
        if cache_value != lexical:
            _fail(f"{project_id} did not use exact contracted {label} {lexical}")
        lexical_path = roots.decode(cache_value, build_root, f"{project_id} {label}")
        resolved = _regular_file(
            lexical_path,
            f"{project_id} {label}",
            allow_symlink=True,
        )
        if (
            resolved.as_posix()
            != _text(tool.get("resolved_path"), f"{project_id} {label} resolved path")
            or _sha256_file(resolved)
            != _text(tool.get("sha256"), f"{project_id} {label} digest")
        ):
            _fail(f"{project_id} {label} resolved identity/digest drifted")
        return resolved

    cmake_value = _cache_value(cache, "CMAKE_COMMAND", project_id)
    cmake = exact_tool(cmake_value, "cmake", "CMake")
    cmake_version_lines = _run_text(
        [cmake_value, "--version"],
        build_root,
        f"{project_id} CMake version",
    ).splitlines()
    if (
        not cmake_version_lines
        or cmake_version_lines[0]
        != _object(toolchain["cmake"], "CMake contract").get(
            "version_first_line"
        )
    ):
        _fail(f"{project_id} CMake version identity drifted")
    git_value = _text(
        _object(toolchain["git"], "Git contract").get("lexical_path"),
        "Git lexical path",
    )
    git = exact_tool(git_value, "git", "Git")
    git_version_lines = _run_text(
        [git_value, "--version"],
        build_root,
        f"{project_id} Git version",
    ).splitlines()
    if (
        not git_version_lines
        or git_version_lines[0]
        != _object(toolchain["git"], "Git contract").get(
            "version_first_line"
        )
    ):
        _fail(f"{project_id} Git version identity drifted")
    python_value = _text(
        _object(toolchain["python"], "Python contract").get("lexical_path"),
        "Python lexical path",
    )
    python = exact_tool(python_value, "python", "Python")
    python_version_lines = _run_text(
        [python_value, "--version"],
        build_root,
        f"{project_id} Python version",
    ).splitlines()
    if (
        not python_version_lines
        or python_version_lines[0]
        != _object(toolchain["python"], "Python contract").get(
            "version_first_line"
        )
    ):
        _fail(f"{project_id} Python version identity drifted")
    ninja_value = _cache_value(cache, "CMAKE_MAKE_PROGRAM", project_id)
    ninja = exact_tool(ninja_value, "ninja", "Ninja")
    compiler_value = _cache_value(cache, "CMAKE_CXX_COMPILER", project_id)
    compiler = exact_tool(compiler_value, "cxx_compiler", "C++ compiler")
    compiler_version_lines = _run_text(
        [compiler_value, "--version"],
        build_root,
        f"{project_id} C++ compiler version",
    ).splitlines()
    if (
        not compiler_version_lines
        or compiler_version_lines[0]
        != _object(toolchain["cxx_compiler"], "compiler contract").get(
            "version_first_line"
        )
    ):
        _fail(f"{project_id} C++ compiler version identity drifted")
    for launcher in ("CMAKE_CXX_COMPILER_LAUNCHER", "CMAKE_C_COMPILER_LAUNCHER"):
        if launcher in cache and cache[launcher][1]:
            _fail(f"{project_id} uses an uncontracted compiler launcher")
    archiver_value = _cache_value(cache, "CMAKE_AR", project_id)
    archiver = exact_tool(archiver_value, "archiver", "archiver")
    ranlib_value = _cache_value(cache, "CMAKE_RANLIB", project_id)
    ranlib = exact_tool(ranlib_value, "ranlib", "ranlib")
    source_boundary = (
        roots.repository
        if _is_within(expected_source_root, roots.repository)
        else roots.output
    )
    source_git_root = _containing_git_root(
        expected_source_root,
        source_boundary,
        f"{project_id} source",
    )
    source_git_generation = _git_source_generation_evidence(
        git_root=source_git_root,
        project_id=project_id,
        build_contract=build_provenance_contract,
        roots=roots,
    )

    build_ninja = _regular_file(build_root / "build.ninja", f"{project_id} build.ninja")
    rules_ninja = _regular_file(
        build_root / "CMakeFiles/rules.ninja", f"{project_id} rules.ninja"
    )
    query = _regular_file(
        build_root / ".cmake/api/v1/query/codemodel-v2",
        f"{project_id} codemodel File API query",
    )
    cmake_files_query = _regular_file(
        build_root / ".cmake/api/v1/query/cmakeFiles-v1",
        f"{project_id} cmakeFiles File API query",
    )
    if _read_json(query, f"{project_id} codemodel File API query") != {}:
        _fail(f"{project_id} File API query is not the exact empty codemodel-v2 query")
    if _read_json(cmake_files_query, f"{project_id} cmakeFiles File API query") != {}:
        _fail(f"{project_id} File API query is not the exact empty cmakeFiles-v1 query")
    reply_root = _directory(
        build_root / ".cmake/api/v1/reply", f"{project_id} File API reply root"
    )
    indexes = sorted(reply_root.glob("index-*.json"))
    if len(indexes) != 1:
        _fail(f"{project_id} does not have exactly one fresh File API index")
    documents = _file_api_documents(reply_root, indexes[0])
    index_value = _object(documents[indexes[0].name][0], f"{project_id} File API index")
    reply = _object(index_value.get("reply"), f"{project_id} File API index reply")
    codemodel_rows = [
        _object(value, f"{project_id} codemodel index row")
        for key, value in reply.items()
        if key.startswith("codemodel-v2")
    ]
    if len(codemodel_rows) != 1:
        _fail(f"{project_id} File API index does not expose exactly one codemodel-v2")
    cmake_files_rows = [
        _object(value, f"{project_id} cmakeFiles index row")
        for key, value in reply.items()
        if key.startswith("cmakeFiles-v1")
    ]
    if len(cmake_files_rows) != 1:
        _fail(f"{project_id} File API index does not expose exactly one cmakeFiles-v1")
    codemodel_name = _text(codemodel_rows[0].get("jsonFile"), f"{project_id} codemodel jsonFile")
    if codemodel_name not in documents:
        _fail(f"{project_id} codemodel reply is not index-reachable")
    codemodel_version = _object(
        codemodel_rows[0].get("version"), f"{project_id} codemodel index version"
    )
    if codemodel_rows[0].get("kind") != "codemodel" or codemodel_version.get("major") != 2:
        _fail(f"{project_id} File API codemodel version is not v2")
    cmake_files_name = _text(
        cmake_files_rows[0].get("jsonFile"),
        f"{project_id} cmakeFiles jsonFile",
    )
    if cmake_files_name not in documents:
        _fail(f"{project_id} cmakeFiles reply is not index-reachable")
    cmake_files_version = _object(
        cmake_files_rows[0].get("version"),
        f"{project_id} cmakeFiles index version",
    )
    if (
        cmake_files_rows[0].get("kind") != "cmakeFiles"
        or cmake_files_version != {"major": 1, "minor": 0}
    ):
        _fail(f"{project_id} File API cmakeFiles index version is not exact v1.0")
    cmake_files = _object(
        documents[cmake_files_name][0], f"{project_id} cmakeFiles"
    )
    configure_input_rows, configure_inputs = _cmake_files_evidence(
        project_id=project_id,
        cmake_files=cmake_files,
        expected_source_root=expected_source_root,
        build_root=build_root,
        roots=roots,
    )
    codemodel = _object(documents[codemodel_name][0], f"{project_id} codemodel")
    if codemodel.get("kind") != "codemodel":
        _fail(f"{project_id} File API object is not a codemodel")
    paths = _object(codemodel.get("paths"), f"{project_id} codemodel paths")
    model_source = _resolve_declared_path(
        paths.get("source"), expected_source_root, roots, f"{project_id} codemodel source"
    )
    model_build = _resolve_declared_path(
        paths.get("build"), build_root, roots, f"{project_id} codemodel build"
    )
    if model_source.resolve(strict=True) != expected_source_root or model_build.resolve(strict=True) != build_root:
        _fail(f"{project_id} codemodel roots disagree with its cache")
    configurations = _list(codemodel.get("configurations"), f"{project_id} configurations")
    if len(configurations) != 1:
        _fail(f"{project_id} codemodel is not exactly single-config")
    configuration = _object(configurations[0], f"{project_id} configuration")
    if configuration.get("name") != expected_configuration:
        _fail(f"{project_id} codemodel configuration drifted")

    targets: dict[str, _TargetReply] = {}
    for index, raw_entry in enumerate(_list(configuration.get("targets"), f"{project_id} targets")):
        entry = _object(raw_entry, f"{project_id} target[{index}]")
        name = _text(entry.get("name"), f"{project_id} target[{index}].name")
        if name in targets:
            _fail(f"{project_id} codemodel duplicates target {name}")
        reply_name = _text(entry.get("jsonFile"), f"{project_id} target {name} jsonFile")
        if reply_name not in documents:
            _fail(f"{project_id} target {name} reply is not index-reachable")
        target_value = _object(documents[reply_name][0], f"{project_id} target {name}")
        if target_value.get("name") != name:
            _fail(f"{project_id} target entry/reply name mismatch")
        target_type = _text(target_value.get("type"), f"{project_id} target {name} type")
        target_paths = _object(target_value.get("paths"), f"{project_id} target {name} paths")
        target_source_base = _resolve_declared_path(
            target_paths.get("source"), model_source, roots, f"{project_id} target {name} source base"
        )
        target_build_base = _resolve_declared_path(
            target_paths.get("build"), model_build, roots, f"{project_id} target {name} build base"
        )
        if not any(
            _is_within(target_source_base, source_root)
            for source_root in allowed_source_roots
        ) or not _is_within(target_build_base, model_build):
            _fail(f"{project_id} target {name} directory paths escape the codemodel roots")
        artifacts: list[Path] = []
        for artifact_index, raw_artifact in enumerate(target_value.get("artifacts", [])):
            artifact = _object(raw_artifact, f"{project_id} target {name} artifact[{artifact_index}]")
            path = _resolve_declared_path(
                artifact.get("path"), model_build, roots, f"{project_id} target {name} artifact"
            )
            if not _is_within(path, build_root):
                _fail(f"{project_id} target {name} artifact escapes its build")
            artifacts.append(path)
        sources, virtual_sources = _file_api_target_sources(
            project_id=project_id,
            target_name=name,
            target_type=target_type,
            target_value=target_value,
            model_source=model_source,
            target_build_base=target_build_base,
            allowed_source_roots=allowed_source_roots,
            build_root=build_root,
            roots=roots,
        )
        if len(artifacts) != len(set(artifacts)):
            _fail(f"{project_id} target {name} repeats an artifact")
        targets[name] = _TargetReply(
            name=name,
            target_type=target_type,
            artifacts=tuple(artifacts),
            sources=sources,
            virtual_sources=virtual_sources,
            build_base=target_build_base,
            reply_path=reply_root / reply_name,
            reply_sha256=documents[reply_name][1],
        )
    compile_commands, compile_evidence = _load_compile_commands(
        project_id,
        expected_source_root,
        build_root,
        allowed_source_roots,
        compiler_value,
        roots,
        responses,
        expected_configuration,
        _object(toolchain["compile_modes"], f"{project_id} compile modes"),
    )
    target_graph = _target_graph_evidence(
        project_id=project_id,
        build_testing=expected_build_testing,
        build_root=build_root,
        roots=roots,
        build_contract=build_provenance_contract,
    )
    graph_names = {node["name"] for node in target_graph["nodes"]}
    if not set(targets).issubset(graph_names):
        _fail(f"{project_id} File API targets are absent from configured-target graph")
    external_graph_names = set(
        _list(
            _object(
                target_graph.get("local_target_roles"),
                f"{project_id} configured-target roles",
            ).get("external"),
            f"{project_id} external graph nodes",
        )
    )
    if set(targets).intersection(external_graph_names):
        _fail(f"{project_id} File API target is misclassified as external")
    configure_fixture_rows, configure_executed_inputs = (
        _configure_executed_fixture_evidence(
            project_id=project_id,
            build_root=build_root,
            build_testing=expected_build_testing,
            cache=cache,
            configure_inputs=configure_inputs,
            build_contract=build_provenance_contract,
            toolchain_contract=toolchain,
            roots=roots,
            retain_replay=retain_configure_fixture_replay,
        )
    )
    evidence = {
        "project_id": project_id,
        "source_root": roots.logical(expected_source_root),
        "source_git_generation": source_git_generation,
        "build_root": roots.logical(build_root),
        "configuration": expected_configuration,
        "compile_mode_contract": dict(
            _object(toolchain["compile_modes"], f"{project_id} compile modes")
        ),
        "build_testing": expected_build_testing,
        "build_testing_cache_key": build_testing_cache_key,
        "cmake_cache": {
            "path": roots.logical(build_root / "CMakeCache.txt"),
            "sha256": cache_sha,
        },
        "build_ninja": {
            "path": roots.logical(build_ninja),
            "sha256": _sha256_file(build_ninja),
        },
        "rules_ninja": {
            "path": roots.logical(rules_ninja),
            "sha256": _sha256_file(rules_ninja),
        },
        "file_api_query": {
            "path": roots.logical(query),
            "sha256": _sha256_file(query),
        },
        "file_api_cmake_files_query": {
            "path": roots.logical(cmake_files_query),
            "sha256": _sha256_file(cmake_files_query),
        },
        "file_api_index": {
            "path": roots.logical(indexes[0]),
            "sha256": documents[indexes[0].name][1],
        },
        "file_api_codemodel": {
            "path": roots.logical(reply_root / codemodel_name),
            "sha256": documents[codemodel_name][1],
        },
        "file_api_cmake_files": {
            "path": roots.logical(reply_root / cmake_files_name),
            "sha256": documents[cmake_files_name][1],
        },
        "configure_inputs": configure_input_rows,
        "configure_executed_fixtures": configure_fixture_rows,
        "configured_target_graph": target_graph,
        "file_api_document_count": len(documents),
        "file_api_documents": [
            {
                "path": roots.logical(reply_root / name),
                "sha256": documents[name][1],
            }
            for name in sorted(documents)
        ],
        "compile_commands": compile_evidence,
        "cmake": {
            "path": cmake_value,
            "resolved_path": cmake.as_posix(),
            "sha256": _sha256_file(cmake),
            "version_first_line": cmake_version_lines[0],
        },
        "git": {
            "path": git_value,
            "resolved_path": git.as_posix(),
            "sha256": _sha256_file(git),
            "version_first_line": git_version_lines[0],
        },
        "python": {
            "path": python_value,
            "resolved_path": python.as_posix(),
            "sha256": _sha256_file(python),
            "version_first_line": python_version_lines[0],
        },
        "ninja": {
            "path": ninja_value,
            "resolved_path": ninja.as_posix(),
            "sha256": _sha256_file(ninja),
            "version": _run_text(
                [ninja_value, "--version"],
                build_root,
                f"{project_id} Ninja version",
            ).strip(),
        },
        "compiler": {
            "path": compiler_value,
            "resolved_path": compiler.as_posix(),
            "sha256": _sha256_file(compiler),
            "version_first_line": compiler_version_lines[0],
        },
        "archiver": {
            "path": archiver_value,
            "resolved_path": archiver.as_posix(),
            "sha256": _sha256_file(archiver),
        },
        "ranlib": {
            "path": ranlib_value,
            "resolved_path": ranlib.as_posix(),
            "sha256": _sha256_file(ranlib),
        },
    }
    command_deltas, known_delta_tokens = _cross_profile_command_contract(
        build_provenance_contract,
        lane_id=lane_id,
        project_id=project_id,
        configuration=expected_configuration,
    )
    return _Build(
        lane_id=lane_id,
        project_id=project_id,
        root=build_root,
        source_root=expected_source_root,
        compiler_path=compiler_value,
        archiver_path=archiver_value,
        targets=targets,
        compile_commands=compile_commands,
        configure_inputs=configure_inputs,
        configure_generated_contracts=dict(configure_generated_contracts),
        qualified_install_inputs={},
        evidence=evidence,
        configure_executed_inputs=configure_executed_inputs,
        cross_profile_command_deltas=command_deltas,
        cross_profile_known_delta_tokens=known_delta_tokens,
    )


@dataclass(frozen=True)
class _Command:
    kind: str
    output: Path
    inputs: tuple[Path, ...]
    auxiliary_outputs: tuple[Path, ...]
    source: Path | None
    directory: Path
    argv: tuple[str, ...]
    argv_sha256: str


def _artifact_kind(path: Path) -> str | None:
    name = path.name
    if name.endswith((".o", ".obj")):
        return "object"
    if name.endswith(".a"):
        return "archive"
    if re.search(r"\.so(?:\.[0-9]+)*$", name):
        return "shared_library"
    return None


def _tool_index(argv: Sequence[str]) -> int:
    index = 0
    while index < len(argv) and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*=.*", argv[index]):
        index += 1
    return index


_GTEST_DISCOVERY_MODULE = Path(
    "/usr/share/cmake-3.25/Modules/GoogleTestAddTests.cmake"
)
_GTEST_DISCOVERY_ROBOPLAN_LD_LIBRARY_PATH = (
    "/usr/local/lib:"
    "/usr/local/lib/python3.12/site-packages/cmeel.prefix/lib:"
    "/usr/local/lib"
)
_GTEST_DISCOVERY_DEFINITIONS = (
    "TEST_TARGET",
    "TEST_EXECUTABLE",
    "TEST_EXECUTOR",
    "TEST_WORKING_DIR",
    "TEST_EXTRA_ARGS",
    "TEST_PROPERTIES",
    "TEST_PREFIX",
    "TEST_SUFFIX",
    "TEST_FILTER",
    "NO_PRETTY_TYPES",
    "NO_PRETTY_VALUES",
    "TEST_LIST",
    "CTEST_FILE",
    "TEST_DISCOVERY_TIMEOUT",
    "TEST_XML_OUTPUT_DIR",
)


def _gtest_discovery_tokens(line: str, label: str) -> tuple[str, ...]:
    try:
        lexer = shlex.shlex(
            line,
            posix=True,
            punctuation_chars="&|;<>",
        )
        lexer.whitespace_split = True
        lexer.commenters = ""
        tokens = tuple(lexer)
    except ValueError as error:
        _fail(f"{label} has unsupported GoogleTest discovery quoting: {error}")
    shell_controls = {"&", "&&", "|", "||", ";", "<", ">", ">>", "<<"}
    if any(token in shell_controls for token in tokens):
        _fail(f"{label} has shell control in its GoogleTest discovery suffix")
    return tokens


def _strip_authenticated_gtest_discovery_suffix(
    line: str,
    *,
    build: _Build,
    roots: _Roots,
    label: str,
) -> tuple[str, Path | None]:
    """Authenticate and remove CMake 3.25's fixed post-link GTest discovery."""

    module_value = _GTEST_DISCOVERY_MODULE.as_posix()
    if module_value not in line:
        return line, None

    logical_build_root = roots.logical(build.root)
    separator = f" && cd {logical_build_root} && "
    prefix, found, suffix = line.rpartition(separator)
    if not found or not prefix or not suffix.endswith(module_value):
        _fail(f"{label} has a malformed GoogleTest discovery suffix")

    tokens = _gtest_discovery_tokens(suffix, label)
    if not tokens or tokens[0] != "/usr/bin/cmake":
        _fail(f"{label} does not use the pinned CMake discovery tool")
    definitions: dict[str, str] = {}
    index = 1
    for expected_key in _GTEST_DISCOVERY_DEFINITIONS:
        if index + 1 >= len(tokens) or tokens[index] != "-D":
            _fail(f"{label} has a malformed GoogleTest discovery definition list")
        key, separator_value, value = tokens[index + 1].partition("=")
        if not separator_value or key != expected_key:
            _fail(f"{label} changes the GoogleTest discovery definition set")
        definitions[key] = value
        index += 2
    if tokens[index:] != ("-P", module_value):
        _fail(f"{label} changes the pinned GoogleTest discovery module")

    target_name = definitions["TEST_TARGET"]
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", target_name):
        _fail(f"{label} has an invalid GoogleTest discovery target")
    target = build.targets.get(target_name)
    if target is None or target.target_type != "EXECUTABLE":
        _fail(f"{label} GoogleTest discovery target is not a configured executable")
    if (
        len(target.artifacts) != 1
        or definitions["TEST_EXECUTABLE"] != roots.logical(target.artifacts[0])
    ):
        _fail(f"{label} changes the exact GoogleTest executable spelling")

    executable = Path(
        os.path.normpath(
            roots.decode(
                definitions["TEST_EXECUTABLE"],
                build.root,
                f"{label} GoogleTest executable",
            )
        )
    )
    if target.artifacts != (executable,):
        _fail(f"{label} GoogleTest executable differs from its File API target")
    if not os.access(
        _regular_file(
            executable,
            f"{label} GoogleTest executable",
            contained_by=build.root,
        ),
        os.X_OK,
    ):
        _fail(f"{label} GoogleTest discovery input is not executable")

    working_directory = Path(
        os.path.normpath(
            roots.decode(
                definitions["TEST_WORKING_DIR"],
                build.root,
                f"{label} GoogleTest working directory",
            )
        )
    )
    if (
        definitions["TEST_WORKING_DIR"] != roots.logical(build.root)
        or working_directory != build.root
    ):
        _fail(f"{label} GoogleTest discovery working directory differs from its build")

    expected_ctest_file = build.root / f"{target_name}[1]_tests.cmake"
    if definitions["CTEST_FILE"] != roots.logical(expected_ctest_file):
        _fail(f"{label} changes the exact GoogleTest CTest filename spelling")
    ctest_file = Path(
        os.path.normpath(
            roots.decode(
                definitions["CTEST_FILE"],
                build.root,
                f"{label} GoogleTest CTest file",
            )
        )
    )
    if ctest_file != expected_ctest_file:
        _fail(f"{label} changes the fixed GoogleTest CTest filename")
    _regular_file(
        ctest_file,
        f"{label} GoogleTest CTest file",
        contained_by=build.root,
    )
    shell_word_candidate = Path(
        f"CTEST_FILE={definitions['CTEST_FILE'].replace('[1]', '1', 1)}"
    )
    shell_expansion_candidate = build.root / shell_word_candidate
    if os.path.lexists(shell_expansion_candidate):
        _fail(f"{label} GoogleTest CTest filename has a shell glob match")

    if definitions["TEST_LIST"] != f"{target_name}_TESTS":
        _fail(f"{label} changes the GoogleTest discovery list name")
    fixed_values = {
        "TEST_EXECUTOR": "",
        "TEST_PREFIX": "",
        "TEST_SUFFIX": "",
        "TEST_FILTER": "",
        "NO_PRETTY_TYPES": "FALSE",
        "NO_PRETTY_VALUES": "FALSE",
        "TEST_DISCOVERY_TIMEOUT": "5",
        "TEST_XML_OUTPUT_DIR": "",
    }
    if any(definitions[key] != value for key, value in fixed_values.items()):
        _fail(f"{label} changes a fixed GoogleTest discovery definition")
    if definitions["TEST_EXTRA_ARGS"]:
        _fail(f"{label} changes the bounded GoogleTest discovery arguments")
    properties = definitions["TEST_PROPERTIES"]
    roboplan_lanes = {
        "roboplan_release",
        "roboplan_consumer_focused",
        "roboplan_asan_ubsan",
        "roboplan_tsan",
    }
    if build.lane_id in roboplan_lanes and not properties.startswith(
        "ENVIRONMENT;"
    ):
        _fail(f"{label} lacks the pinned GoogleTest discovery environment")
    if properties.startswith("ENVIRONMENT;"):
        if build.lane_id in {"roboplan_release", "roboplan_consumer_focused"}:
            expected_library_path = _GTEST_DISCOVERY_ROBOPLAN_LD_LIBRARY_PATH
        elif build.lane_id in {"roboplan_asan_ubsan", "roboplan_tsan"}:
            logical_build_root = Path(roots.logical(build.root))
            sanitizer_root = logical_build_root.parents[1]
            cmeel_library = (
                "/usr/local/lib/python3.12/site-packages/cmeel.prefix/lib"
            )
            system_library = "/usr/local/lib"
            expected_library_path = ":".join(
                (
                    (sanitizer_root / "install/roboplan/lib").as_posix(),
                    (
                        sanitizer_root
                        / "install/roboplan_example_models/lib"
                    ).as_posix(),
                    cmeel_library,
                    system_library,
                    cmeel_library,
                    system_library,
                )
            )
        else:
            _fail(f"{label} adds an unexpected GoogleTest discovery environment")
        roboplan_prefix = (
            f"ENVIRONMENT;LD_LIBRARY_PATH={expected_library_path};"
        )
        if not properties.startswith(roboplan_prefix):
            _fail(f"{label} changes the pinned GoogleTest discovery environment")
        properties = properties.removeprefix(roboplan_prefix)
    properties_match = re.fullmatch(
        r"TIMEOUT;(?P<timeout>[1-9][0-9]{0,4})",
        properties,
    )
    if properties_match is None or int(properties_match.group("timeout")) > 3600:
        _fail(f"{label} has unsupported GoogleTest discovery properties")

    _regular_file(
        _GTEST_DISCOVERY_MODULE,
        f"{label} GoogleTest discovery module",
        contained_by=_GTEST_DISCOVERY_MODULE.parent,
    )

    serialized: list[str] = ["/usr/bin/cmake"]
    for key in _GTEST_DISCOVERY_DEFINITIONS:
        value = definitions[key]
        assignment = f"{key}={value}"
        if key == "TEST_PROPERTIES":
            assignment = f'"{assignment}"'
        serialized.extend(("-D", assignment))
    serialized.extend(("-P", module_value))
    if suffix != " ".join(serialized):
        _fail(f"{label} changes the exact GoogleTest discovery shell spelling")
    return prefix, executable


def _split_shell_line(line: str, label: str) -> list[tuple[str, ...]]:
    if not line or len(line.encode("utf-8")) > _MAX_RESPONSE_BYTES:
        _fail(f"{label} is empty or too large")
    if (
        "`" in line
        or "$(" in line
        or "${" in line
        or re.search(r"(?<!\\)\$[A-Za-z_0-9@*#?!-]", line)
        or re.search(r"(?<!\\)[*?\[]", line)
        or re.search(r"(?:^|[\s=:,])~", line)
    ):
        _fail(f"{label} uses unsupported shell evaluation")
    controls_removed = line.replace("&&", "")
    if any(value in controls_removed for value in ("&", "|", ";", "<", ">")):
        _fail(f"{label} uses attached or unsupported shell control/redirection")
    try:
        tokens = shlex.split(line, posix=True)
    except ValueError as error:
        _fail(f"{label} has unsupported quoting: {error}")
    forbidden = {"||", "|", ";", "<", ">", ">>", "<<"}
    if forbidden.intersection(tokens):
        _fail(f"{label} uses unsupported shell control or redirection")
    segments: list[tuple[str, ...]] = []
    current: list[str] = []
    for token in tokens:
        if token == "&&":
            if not current:
                _fail(f"{label} contains an empty shell segment")
            segments.append(tuple(current))
            current = []
        else:
            current.append(token)
    if current:
        segments.append(tuple(current))
    return segments


def _non_driver_segment(
    segment: Sequence[str],
    cwd: Path,
    build: _Build,
    roots: _Roots,
    label: str,
) -> _Command | None:
    """Accept only bounded CMake/Ninja housekeeping or a local header generator."""

    if not segment:
        _fail(f"{label} contains an empty command segment")
    tool = segment[0]
    if tool == "/usr/bin/ranlib":
        if len(segment) != 2:
            _fail(f"{label} has an unsupported ranlib command")
        archive = roots.decode(segment[1], cwd, f"{label} ranlib archive")
        if not _is_within(archive, build.root) or archive.suffix != ".a":
            _fail(f"{label} ranlib target escapes its build")
        return None
    if tool == "/usr/bin/cmake" and len(segment) >= 3 and segment[1] == "-E":
        operation = segment[2]
        if operation == "rm" and len(segment) >= 5 and segment[3] == "-f":
            paths = segment[4:]
        elif operation == "rm" and len(segment) == 5 and segment[3] == "-rf":
            paths = segment[4:]
        elif operation == "make_directory" and len(segment) >= 4:
            paths = segment[3:]
        elif operation == "cmake_symlink_library" and len(segment) in {5, 6}:
            paths = segment[3:]
        else:
            _fail(f"{label} uses unsupported CMake -E operation {operation!r}")
        for value in paths:
            path = roots.decode(value, cwd, f"{label} CMake -E path")
            if not _is_within(path, build.root):
                _fail(f"{label} CMake -E path escapes its build")
        return None
    forbidden_tools = {
        "cp",
        "install",
        "objcopy",
        "llvm-objcopy",
        "patchelf",
        "strip",
        "llvm-strip",
        "dd",
    }
    if Path(tool).name in forbidden_tools:
        _fail(f"{label} uses forbidden post-link mutation tool {Path(tool).name}")

    executable = roots.decode(tool, cwd, f"{label} local generator")
    output_values: list[tuple[str, str]] = []
    for index, token in enumerate(segment[1:], 1):
        if token in {"--output", "--output-root"}:
            if index + 1 >= len(segment):
                _fail(f"{label} local generator has a dangling {token}")
            output_values.append((token, segment[index + 1]))
        elif token.startswith(("--output=", "--output-root=")):
            option, _separator, value = token.partition("=")
            output_values.append((option, value))
    if (
        len(output_values) != 1
        or not _is_within(executable, build.root)
        or not os.access(
            _regular_file(
                executable,
                f"{label} local generator",
                contained_by=build.root,
            ),
            os.X_OK,
        )
    ):
        _fail(f"{label} has an unsupported non-driver command segment")
    output = roots.decode(output_values[0][1], cwd, f"{label} generated output")
    if not _is_within(output, build.root) or _artifact_kind(output) is not None:
        _fail(f"{label} local generator may only produce a non-code build input")
    return _Command(
        kind="generated",
        output=Path(os.path.normpath(output)),
        inputs=(Path(os.path.normpath(executable)),),
        auxiliary_outputs=(),
        source=None,
        directory=cwd,
        argv=tuple(segment),
        argv_sha256=_canonical_sha256(list(segment)),
    )


def _archive_command(
    argv: Sequence[str],
    cwd: Path,
    roots: _Roots,
    expected_archiver: str,
    label: str,
) -> tuple[Path, tuple[Path, ...]] | None:
    tool_index = _tool_index(argv)
    if tool_index >= len(argv) or Path(argv[tool_index]).name not in {"ar", "gcc-ar", "llvm-ar", "gcc-ar-12"}:
        return None
    if tool_index != 0 or argv[0] != expected_archiver:
        _fail(f"{label} does not invoke the exact cache archiver without a prefix")
    tail = list(argv[tool_index + 1 :])
    if len(tail) < 2:
        _fail(f"{label} archive command is too short")
    operation = tail.pop(0)
    if operation.startswith("-"):
        operation = operation.lstrip("-")
    if operation not in {"qc", "qcs", "qcD", "qcsD", "rc", "rcs", "rcD", "rcsD"}:
        _fail(f"{label} archive operation is unsupported: {operation}")
    output = roots.decode(tail.pop(0), cwd, f"{label} archive output")
    if not tail or any(
        token.startswith("-")
        or _artifact_kind(Path(token)) != "object"
        for token in tail
    ):
        _fail(
            f"{label} archive command has an option, plugin, or ignored positional input"
        )
    inputs = tuple(
        roots.decode(token, cwd, f"{label} archive input") for token in tail
    )
    return output, inputs


def _link_system_path(path: Path) -> bool:
    return any(
        _is_within(path, root)
        for root in (
            Path("/lib"),
            Path("/lib64"),
            Path("/usr/lib"),
            Path("/usr/lib64"),
            Path("/usr/local/lib"),
            Path("/usr/local/lib64"),
        )
    )


def _require_system_library_chain(
    resolved: Path, chain: Sequence[str], label: str
) -> None:
    if not _link_system_path(resolved) or any(
        not _link_system_path(Path(item)) for item in chain
    ):
        _fail(f"{label} symlink chain escapes pinned system-library roots")


def _authenticate_link_input(
    value: str,
    *,
    cwd: Path,
    build: _Build,
    roots: _Roots,
    label: str,
) -> Path:
    path = Path(os.path.normpath(roots.decode(value, cwd, label)))
    if _is_within(path, roots.output):
        owner = roots.output
    elif _is_within(path, roots.repository):
        owner = roots.repository
    elif _link_system_path(path):
        owner = Path("/")
    else:
        _fail(f"{label} escapes qualified source, output, and system roots: {value}")
    _regular_file(
        path,
        label,
        allow_symlink=True,
        contained_by=owner,
    )
    return path


def _validate_link_input_format(path: Path, label: str) -> None:
    """Reject GNU ld's implicit-script positional-input fallback."""

    resolved = _regular_file(path, label, allow_symlink=True)
    try:
        with resolved.open("rb") as stream:
            magic = stream.read(8)
    except OSError as error:
        _fail(f"{label} cannot be inspected: {error}")
    if path.name.endswith(".a"):
        if magic == b"!<thin>\n":
            _fail(f"{label} is a forbidden thin archive")
        if magic != b"!<arch>\n":
            _fail(f"{label} has an archive suffix without regular archive bytes")
        return
    if not magic.startswith(b"\x7fELF"):
        _fail(
            f"{label} is not an ELF object/DSO/interpreter; implicit linker "
            "scripts and other opaque positional inputs are forbidden"
        )


def _validated_link_directory(
    value: str,
    *,
    cwd: Path,
    build: _Build,
    roots: _Roots,
    label: str,
) -> Path:
    path = Path(os.path.normpath(roots.decode(value, cwd, label)))
    directory = _directory(path, label)
    if _is_within(directory, roots.output):
        return directory
    if _link_system_path(directory):
        return directory
    _fail(f"{label} is outside qualified output/system library roots: {value}")


def _validate_runtime_search_path(
    value: str,
    *,
    cwd: Path,
    build: _Build,
    roots: _Roots,
    label: str,
) -> None:
    if not value:
        _fail(f"{label} is empty")
    for index, item in enumerate(value.split(":")):
        if re.fullmatch(r"\$ORIGIN(?:/[A-Za-z0-9_.+-]+)*", item):
            continue
        _validated_link_directory(
            item,
            cwd=cwd,
            build=build,
            roots=roots,
            label=f"{label}[{index}]",
        )


def _resolve_link_library(
    name: str,
    *,
    static_mode: bool,
    search_directories: Sequence[Path],
    build: _Build,
    roots: _Roots,
    label: str,
) -> Path:
    if name.startswith(":"):
        filename = name[1:]
        if Path(filename).name != filename or not filename:
            _fail(f"{label} has an unsafe -l: filename")
        candidates = [filename]
    else:
        if not re.fullmatch(r"[A-Za-z0-9_+.-]+", name):
            _fail(f"{label} has an unsafe -l library name: {name}")
        candidates = [f"lib{name}.a"] if static_mode else [f"lib{name}.so", f"lib{name}.a"]
    search_flags = [f"-L{path}" for path in search_directories]
    for filename in candidates:
        resolved_value = _run_text(
            [
                build.compiler_path,
                *search_flags,
                f"-print-file-name={filename}",
            ],
            build.root,
            f"{label} exact compiler library resolution",
        ).strip()
        if (
            not resolved_value
            or "\n" in resolved_value
            or resolved_value == filename
        ):
            continue
        normalized = os.path.normpath(resolved_value)
        if not Path(normalized).is_absolute():
            _fail(f"{label} compiler returned a non-absolute library resolution")
        return _authenticate_link_input(
            normalized,
            cwd=build.root,
            build=build,
            roots=roots,
            label=f"{label} resolved -l{name}",
        )
    _fail(f"{label} cannot resolve -l{name} to one exact library file")


def _driver_linker_tokens(argv: Sequence[str], label: str) -> list[str]:
    tokens: list[str] = []
    index = 1
    dangerous_driver_prefixes = (
        "-B",
        "-fuse-ld",
        "-flto",
        "-specs",
        "--sysroot",
        "-wrapper",
        "-static-lib",
    )
    while index < len(argv):
        token = argv[index]
        if token == "-o":
            if index + 1 >= len(argv):
                _fail(f"{label} has a dangling -o")
            index += 2
            continue
        if token.startswith("-Wl,"):
            embedded = token[4:].split(",")
            if not embedded or any(not item for item in embedded):
                _fail(f"{label} has malformed -Wl linker arguments")
            tokens.extend(embedded)
            index += 1
            continue
        if token == "-Xlinker":
            if index + 1 >= len(argv):
                _fail(f"{label} has a dangling -Xlinker")
            tokens.append(argv[index + 1])
            index += 2
            continue
        if token in {
            "-L",
            "-l",
            "-T",
            "--script",
            "--version-script",
        }:
            if index + 1 >= len(argv):
                _fail(f"{label} has a dangling {token}")
            tokens.extend((token, argv[index + 1]))
            index += 2
            continue
        if token.startswith(("-L", "-l", "-T")) and len(token) > 2:
            tokens.append(token)
            index += 1
            continue
        if token.startswith(("--script=", "--version-script=")):
            tokens.append(token)
            index += 1
            continue
        if token == "-static":
            tokens.append("-Bstatic")
            index += 1
            continue
        if token.startswith(dangerous_driver_prefixes):
            _fail(f"{label} uses unsupported implicit linker/tool search option {token}")
        if token in {"-shared", "-pie", "-no-pie", "-rdynamic", "-pthread"}:
            index += 1
            continue
        if re.fullmatch(
            r"-(?:O[0-3sgz]|g(?:[0-9]|gdb)?|D[A-Za-z_][A-Za-z0-9_]*(?:=.*)?|"
            r"m[A-Za-z0-9_+.-]+(?:=.*)?|W(?!l,)[A-Za-z0-9_+.,=-]+|"
            r"f(?:PIC|PIE|pic|pie|no-omit-frame-pointer|omit-frame-pointer|"
            r"sanitize=[A-Za-z0-9_,+-]+|no-sanitize=[A-Za-z0-9_,+-]+|"
            r"sanitize-recover=[A-Za-z0-9_,+-]+|no-sanitize-recover=[A-Za-z0-9_,+-]+|"
            r"visibility(?:-inlines)?-hidden|no-semantic-interposition|"
            r"function-sections|data-sections|stack-protector(?:-strong|-all)?|"
            r"cf-protection(?:=[A-Za-z0-9_-]+)?))",
            token,
        ):
            index += 1
            continue
        if token.startswith("-"):
            _fail(f"{label} uses unsupported driver/linker option {token}")
        tokens.append(token)
        index += 1
    return tokens


def _decode_link_inputs(
    argv: Sequence[str],
    *,
    cwd: Path,
    build: _Build,
    roots: _Roots,
    label: str,
) -> tuple[tuple[Path, ...], tuple[Path, ...]]:
    tokens = _driver_linker_tokens(argv, label)
    search_directories: list[Path] = []
    index = 0
    while index < len(tokens):
        token = tokens[index]
        value: str | None = None
        if token == "-L":
            if index + 1 >= len(tokens):
                _fail(f"{label} has a dangling linker -L")
            value = tokens[index + 1]
            index += 2
        elif token.startswith("-L") and len(token) > 2:
            value = token[2:]
            index += 1
        else:
            index += 1
        if value is not None:
            directory = _validated_link_directory(
                value,
                cwd=cwd,
                build=build,
                roots=roots,
                label=f"{label} library search directory",
            )
            if directory in search_directories:
                _fail(f"{label} repeats a library search directory")
            search_directories.append(directory)

    inputs: list[Path] = []
    auxiliary_outputs: list[Path] = []
    static_mode = False
    state_stack: list[bool] = []
    no_argument = {
        "--as-needed",
        "--no-as-needed",
        "--gc-sections",
        "--no-gc-sections",
        "--no-undefined",
        "--allow-shlib-undefined",
        "--eh-frame-hdr",
        "--enable-new-dtags",
        "--disable-new-dtags",
        "--start-group",
        "--end-group",
        "--whole-archive",
        "--no-whole-archive",
        "--copy-dt-needed-entries",
        "--no-copy-dt-needed-entries",
        "--export-dynamic",
        "--fatal-warnings",
        "--demangle",
        "--no-demangle",
        "-Bsymbolic",
        "-Bsymbolic-functions",
    }
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token == "-L":
            index += 2
            continue
        if token.startswith("-L") and len(token) > 2:
            index += 1
            continue
        if token in {"-Bstatic", "-Bdynamic"}:
            static_mode = token == "-Bstatic"
            index += 1
            continue
        if token == "--push-state":
            state_stack.append(static_mode)
            index += 1
            continue
        if token == "--pop-state":
            if not state_stack:
                _fail(f"{label} pops an empty linker-state stack")
            static_mode = state_stack.pop()
            index += 1
            continue
        if token == "-l":
            if index + 1 >= len(tokens):
                _fail(f"{label} has a dangling linker -l")
            library_name = tokens[index + 1]
            index += 2
        elif token.startswith("-l") and len(token) > 2:
            library_name = token[2:]
            index += 1
        else:
            library_name = None
        if library_name is not None:
            inputs.append(
                _resolve_link_library(
                    library_name,
                    static_mode=static_mode,
                    search_directories=search_directories,
                    build=build,
                    roots=roots,
                    label=label,
                )
            )
            continue

        nested_input_prefixes = (
            "--script=",
            "--version-script=",
            "--dynamic-list=",
            "--retain-symbols-file=",
            "--symbol-ordering-file=",
            "--just-symbols=",
        )
        if token.startswith(nested_input_prefixes) or (
            token.startswith("-T") and len(token) > 2
        ):
            _fail(
                f"{label} uses an unsupported linker script/nested linker input: {token}"
            )
        if token in {
            "-T",
            "--script",
            "--version-script",
            "--dynamic-list",
            "--retain-symbols-file",
            "--symbol-ordering-file",
            "--just-symbols",
        }:
            _fail(
                f"{label} uses an unsupported linker script/nested linker input: {token}"
            )
        if token.startswith("--dynamic-linker="):
            inputs.append(
                _authenticate_link_input(
                    token.removeprefix("--dynamic-linker="),
                    cwd=cwd,
                    build=build,
                    roots=roots,
                    label=f"{label} linker input option",
                )
            )
            index += 1
            continue
        if token == "--dynamic-linker":
            if index + 1 >= len(tokens):
                _fail(f"{label} has a dangling linker input option {token}")
            inputs.append(
                _authenticate_link_input(
                    tokens[index + 1],
                    cwd=cwd,
                    build=build,
                    roots=roots,
                    label=f"{label} {token} input",
                )
            )
            index += 2
            continue

        output_value: str | None = None
        for prefix in ("--dependency-file=", "-Map=", "--Map="):
            if token.startswith(prefix):
                output_value = token[len(prefix) :]
                break
        if output_value is not None:
            output_path = Path(
                os.path.normpath(roots.decode(output_value, cwd, f"{label} auxiliary output"))
            )
            if not _is_within(output_path, build.root):
                _fail(f"{label} linker auxiliary output escapes its build")
            auxiliary_outputs.append(output_path)
            index += 1
            continue
        if token in {"--dependency-file", "-Map", "--Map"}:
            if index + 1 >= len(tokens):
                _fail(f"{label} has a dangling linker auxiliary-output option")
            output_path = Path(
                os.path.normpath(
                    roots.decode(tokens[index + 1], cwd, f"{label} auxiliary output")
                )
            )
            if not _is_within(output_path, build.root):
                _fail(f"{label} linker auxiliary output escapes its build")
            auxiliary_outputs.append(output_path)
            index += 2
            continue

        if token in {"-rpath", "--rpath", "-rpath-link", "--rpath-link"}:
            if index + 1 >= len(tokens):
                _fail(f"{label} has a dangling runtime search-path option")
            _validate_runtime_search_path(
                tokens[index + 1],
                cwd=cwd,
                build=build,
                roots=roots,
                label=f"{label} {token}",
            )
            index += 2
            continue
        if any(
            token.startswith(prefix)
            for prefix in ("-rpath=", "--rpath=", "-rpath-link=", "--rpath-link=")
        ):
            _validate_runtime_search_path(
                token.partition("=")[2],
                cwd=cwd,
                build=build,
                roots=roots,
                label=f"{label} runtime search path",
            )
            index += 1
            continue
        if token in {"-soname", "--soname", "-z", "-u", "--undefined", "-e", "--entry"}:
            if index + 1 >= len(tokens) or not tokens[index + 1]:
                _fail(f"{label} has a dangling non-file linker option {token}")
            index += 2
            continue
        if token in no_argument or re.fullmatch(
            r"(?:--build-id(?:=[A-Za-z0-9_.+-]+)?|--hash-style=[A-Za-z0-9_.+-]+|"
            r"--compress-debug-sections=[A-Za-z0-9_.+-]+|--sort-section=[A-Za-z0-9_.+-]+|"
            r"--icf=[A-Za-z0-9_.+-]+|--wrap=[A-Za-z0-9_.$@+-]+|"
            r"--undefined=[A-Za-z0-9_.$@+-]+|--entry=[A-Za-z0-9_.$@+-]+|"
            r"--export-dynamic-symbol=[A-Za-z0-9_.$@*?+-]+|-O[0-9]+)",
            token,
        ):
            index += 1
            continue
        if token in {"-s", "-S", "--strip-all", "--strip-debug"}:
            _fail(f"{label} attempts link-time output stripping")
        if token.startswith(("--plugin", "-plugin", "--sysroot", "-Y", "-R")):
            _fail(f"{label} uses an unsupported hidden linker input/search option {token}")
        if token.startswith("-"):
            _fail(f"{label} uses unsupported linker option {token}")
        inputs.append(
            _authenticate_link_input(
                token,
                cwd=cwd,
                build=build,
                roots=roots,
                label=f"{label} explicit linker input",
            )
        )
        index += 1

    if len(auxiliary_outputs) != len(set(auxiliary_outputs)):
        _fail(f"{label} repeats a linker auxiliary output")
    if state_stack:
        _fail(f"{label} leaves linker state pushed")
    for input_path in inputs:
        _validate_link_input_format(input_path, f"{label} authenticated linker input")
    return tuple(inputs), tuple(auxiliary_outputs)


def _parse_ninja_commands(
    text: str,
    build: _Build,
    roots: _Roots,
    responses: _ResponseCollector,
    label: str,
) -> dict[Path, _Command]:
    lines = [line for line in text.splitlines() if line]
    if not lines or len(lines) > _MAX_COMMAND_LINES:
        _fail(f"{label} has no commands or exceeds the command bound")
    commands: dict[Path, _Command] = {}
    for line_index, line in enumerate(lines):
        line, discovery_executable = _strip_authenticated_gtest_discovery_suffix(
            line,
            build=build,
            roots=roots,
            label=f"{label} line {line_index + 1}",
        )
        line_commands: list[_Command] = []
        cwd = build.root
        for raw_segment in _split_shell_line(line, f"{label} line {line_index + 1}"):
            if raw_segment == (":",):
                continue
            if raw_segment[0] == "cd":
                if len(raw_segment) != 2:
                    _fail(f"{label} uses an unsupported cd wrapper")
                cwd = roots.decode(raw_segment[1], cwd, f"{label} cd")
                if not _is_within(cwd, roots.output):
                    _fail(f"{label} cd escapes the qualification output")
                continue
            segment = responses.expand(raw_segment, cwd, f"{label} line {line_index + 1}")
            archive = _archive_command(
                segment, cwd, roots, build.archiver_path, label
            )
            if archive is not None:
                output, inputs = archive
                command = _Command(
                    kind="archive",
                    output=Path(os.path.normpath(output)),
                    inputs=tuple(Path(os.path.normpath(item)) for item in inputs),
                    auxiliary_outputs=(),
                    source=None,
                    directory=cwd,
                    argv=tuple(segment),
                    argv_sha256=_canonical_sha256(list(segment)),
                )
            else:
                compiler_argv = _maybe_compiler_slice(segment)
                if compiler_argv is None:
                    generated = _non_driver_segment(
                        segment, cwd, build, roots, label
                    )
                    if generated is None:
                        continue
                    command = generated
                    if command.output in commands and commands[command.output] != command:
                        _fail(
                            f"{label} has ambiguous producers for "
                            f"{roots.logical(command.output)}"
                        )
                    line_commands.append(command)
                    commands[command.output] = command
                    continue
                if compiler_argv != segment or compiler_argv[0] != build.compiler_path:
                    _fail(
                        f"{label} does not invoke the exact cache compiler without a "
                        "prefix assignment, wrapper, or launcher"
                    )
                source = _compile_source(compiler_argv, roots, cwd, label)
                output = _option_output(compiler_argv, roots, cwd, label)
                if source is None and output is None:
                    continue
                if output is None:
                    _fail(f"{label} compile/link segment has no output")
                inputs: tuple[Path, ...] = ()
                auxiliary_outputs: tuple[Path, ...] = ()
                if source is None:
                    inputs, auxiliary_outputs = _decode_link_inputs(
                        compiler_argv,
                        cwd=cwd,
                        build=build,
                        roots=roots,
                        label=f"{label} linker closure",
                    )
                    if Path(os.path.normpath(output)) in inputs:
                        _fail(f"{label} linker output is also an authenticated input")
                command = _Command(
                    kind="compile" if source is not None else "link",
                    output=Path(os.path.normpath(output)),
                    inputs=inputs,
                    auxiliary_outputs=auxiliary_outputs,
                    source=Path(os.path.normpath(source)) if source is not None else None,
                    directory=cwd,
                    argv=compiler_argv,
                    argv_sha256=_canonical_sha256(list(compiler_argv)),
                )
            if command.output in commands and commands[command.output] != command:
                _fail(f"{label} has ambiguous producers for {roots.logical(command.output)}")
            line_commands.append(command)
            commands[command.output] = command
        if discovery_executable is not None and (
            len(line_commands) != 1
            or line_commands[0].kind != "link"
            or line_commands[0].output != discovery_executable
        ):
            _fail(
                f"{label} GoogleTest discovery suffix differs from its one link output"
            )
    return commands


def _sanitizer_state(argv: Sequence[str], label: str) -> dict[str, Any]:
    enabled: set[str] = set()
    disabled_tokens: list[str] = []
    frame_pointer: bool | None = None
    for token in argv:
        if token == "-fsanitize" or token == "-fno-sanitize":
            _fail(f"{label} uses unsupported split sanitizer syntax")
        if token.startswith("-fsanitize="):
            values = token.partition("=")[2].split(",")
            if not values or any(not value for value in values):
                _fail(f"{label} has a malformed sanitizer list")
            enabled.update(values)
        elif token.startswith("-fno-sanitize="):
            values = token.partition("=")[2].split(",")
            if not values or any(not value for value in values):
                _fail(f"{label} has a malformed sanitizer disable list")
            disabled_tokens.append(token)
            if "all" in values:
                enabled.clear()
            else:
                enabled.difference_update(values)
        elif token == "-fno-omit-frame-pointer":
            frame_pointer = True
        elif token == "-fomit-frame-pointer":
            frame_pointer = False
    return {
        "effective_sanitizers": sorted(enabled),
        "effective_frame_pointer": frame_pointer,
        "disable_flags": disabled_tokens,
    }


def _check_sanitizer(argv: Sequence[str], profile: str, label: str) -> dict[str, Any]:
    state = _sanitizer_state(argv, label)
    expected = {
        "none": set(),
        "address+undefined": {"address", "undefined"},
        "thread": {"thread"},
    }
    if profile not in expected:
        _fail(f"unknown sanitizer profile: {profile}")
    if state["disable_flags"]:
        _fail(f"{label} contains sanitizer-disabling flags")
    if set(state["effective_sanitizers"]) != expected[profile]:
        _fail(
            f"{label} effective sanitizer set differs from {sorted(expected[profile])}: "
            f"{state['effective_sanitizers']}"
        )
    if profile != "none" and state["effective_frame_pointer"] is not True:
        _fail(f"{label} does not effectively preserve frame pointers")
    return state


def _compile_semantic_argv(argv: Sequence[str]) -> tuple[str, ...]:
    """Remove only Ninja's depfile bookkeeping from a compiler invocation."""

    result: list[str] = []
    index = 0
    while index < len(argv):
        token = argv[index]
        if token in {"-MD", "-MMD", "-MP"}:
            index += 1
            continue
        if token in {"-MF", "-MT", "-MQ"}:
            if index + 1 >= len(argv):
                _fail("compiler dependency flag lacks its value")
            index += 2
            continue
        result.append(token)
        index += 1
    return tuple(result)


def _reject_untracked_assembler_file_inputs(path: Path, label: str) -> None:
    """Depfiles do not report files read by inline assembler directives."""

    raw = _read_bytes(path, label)
    if re.search(rb"(?<![A-Za-z0-9_])(?:asm|__asm|__asm__)(?![A-Za-z0-9_])", raw):
        _fail(
            f"{label} contains inline assembly, whose external file reads are not "
            "covered by the frozen Ninja dependency evidence"
        )


def _configure_generated_dependency_evidence(
    *,
    dependency: Path,
    build: _Build,
    commands: Mapping[Path, _Command],
    roots: _Roots,
    label: str,
) -> dict[str, Any] | None:
    contract = build.configure_generated_contracts.get(dependency.name)
    if contract is None:
        return None
    try:
        relative = dependency.relative_to(build.root)
    except ValueError:
        return None
    if relative not in contract.allowed_build_relative_paths:
        _fail(
            f"{label} configure-generated header has an uncontracted build path: "
            f"{roots.logical(dependency)}"
        )
    if dependency in commands:
        _fail(f"{label} configure-generated header unexpectedly has a Ninja producer")
    generated = _regular_file(
        dependency,
        f"{label} configure-generated header",
        contained_by=build.root,
    )
    required_inputs = (
        roots.repository / "manyforge_deployment/CMakeLists.txt",
        contract.template,
        *contract.semantic_inputs,
    )
    input_rows: list[dict[str, Any]] = []
    for required in required_inputs:
        normalized = Path(os.path.normpath(required))
        row = build.configure_inputs.get(normalized)
        if row is None:
            _fail(
                f"{label} configure-generated owner lacks authenticated CMake input "
                f"{roots.logical(normalized)}"
            )
        identity = _object(row.get("identity"), f"{label} configure input identity")
        if identity.get("kind") != "git_tracked_source":
            _fail(f"{label} configure-generated semantic input is not Git tracked")
        input_rows.append(dict(row))
    cmake = _object(build.evidence.get("cmake"), f"{label} CMake evidence")
    cache = _object(build.evidence.get("cmake_cache"), f"{label} cache evidence")
    configure_rows = _list(
        build.evidence.get("configure_inputs"), f"{label} configure inputs"
    )
    return {
        "kind": "cmake_configure_file",
        "owner_project_id": contract.owner_project_id,
        "output": roots.logical(generated),
        "output_sha256": _sha256_file(generated),
        "cmake": dict(cmake),
        "cmake_cache": dict(cache),
        "semantic_input_policy": "all_authenticated_cmake_inputs_and_exact_cache",
        "required_inputs": sorted(input_rows, key=lambda row: row["path"]),
        "all_configure_inputs_sha256": _canonical_sha256(configure_rows),
    }


def _configure_header_cross_profile_sha256(path: Path) -> str:
    if path.name != "embedded_build_evidence.hpp":
        return _sha256_file(path)
    raw = _read_bytes(path, "embedded build-evidence header")
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as error:
        _fail(f"embedded build-evidence header is not UTF-8: {error}")
    substitutions = {
        "kBuildProfile": r"[A-Za-z0-9_.+-]+",
        "kBuildConfigurationDigest": r"sha256:[0-9a-f]{64}",
    }
    normalized = text
    for symbol, value_pattern in substitutions.items():
        pattern = re.compile(
            rf'(inline constexpr std::string_view {symbol} =\n    ")'
            rf'({value_pattern})(";\n)'
        )
        normalized, count = pattern.subn(
            rf'\1<I2_ALLOWED_{symbol.upper()}>\3', normalized
        )
        if count != 1:
            _fail(
                f"embedded build-evidence header does not contain one exact {symbol}"
            )
    return _sha256_bytes(normalized.encode("utf-8"))


def _build_qualified_profile_cross_profile_sha256(path: Path) -> str:
    """Normalize only the two qualified compiler-build profile facts.

    The compiler-build digest is a derived, non-circular framed content
    identity, not the raw SHA-256 of JSON bytes that contain that identity.
    Validate the exact identity first, then omit the derived own field from the
    normalized projection so Release and RelWithDebInfo can compare equal when
    their only semantic differences are the two sanctioned profile facts.
    """

    raw = _read_bytes(path, "generated build-qualified admission profile")
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as error:
        _fail(f"generated build-qualified admission profile is not UTF-8: {error}")
    pattern = re.compile(
        r"inline constexpr std::array<std::uint8_t, (?P<size>[0-9]+)U> "
        r"kCompilerBuildManifestBytes\{\{\n(?P<body>.*?)\}\};\n\n"
        r"inline constexpr std::string_view kCompilerBuildManifestDigest = "
        r'"(?P<digest>sha256:[0-9a-f]{64})";',
        re.DOTALL,
    )
    match = pattern.search(text)
    if match is None or pattern.search(text, match.end()) is not None:
        _fail("generated profile does not contain one exact compiler-build block")
    body = match.group("body")
    if re.fullmatch(
        r"\s*0x[0-9a-f]{2}U(?:,\s*0x[0-9a-f]{2}U)*\s*", body
    ) is None:
        _fail("generated compiler-build byte-array syntax drifted")
    manifest_bytes = bytes(
        int(value, 16) for value in re.findall(r"0x([0-9a-f]{2})U", body)
    )
    if len(manifest_bytes) != int(match.group("size")):
        _fail("generated compiler-build byte-array size drifted")
    try:
        manifest = json.loads(
            manifest_bytes.decode("utf-8"),
            object_pairs_hook=_unique_object,
            parse_constant=lambda token: _fail(
                f"generated compiler-build manifest contains {token}"
            ),
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        _fail(f"generated compiler-build manifest is invalid JSON: {error}")
    manifest = _object(manifest, "generated compiler-build manifest")
    try:
        canonical_manifest_bytes = json.dumps(
            manifest,
            ensure_ascii=False,
            allow_nan=False,
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")
    except (TypeError, ValueError) as error:
        _fail(
            "generated compiler-build manifest cannot be canonically "
            f"serialized: {error}"
        )
    if manifest_bytes != canonical_manifest_bytes:
        _fail("generated compiler-build manifest is not exact canonical JSON")
    manifest_digest = _verify_framed_content_identity(
        manifest,
        identity_domain="compiler-build-digest",
        api_version="manyforge.compiler_build_manifest.v1",
        own_field="compiler_build_digest",
        label="generated compiler-build manifest",
    )
    if match.group("digest") != manifest_digest:
        _fail("generated profile compiler-build digest disagrees with its manifest")
    profile = manifest.get("build_profile")
    configuration_digest = manifest.get("build_configuration_digest")
    if profile not in {"Release", "RelWithDebInfo"} or not isinstance(
        configuration_digest, str
    ) or not re.fullmatch(r"sha256:[0-9a-f]{64}", configuration_digest):
        _fail("generated compiler-build profile/configuration facts drifted")
    normalized_manifest = {
        key: value for key, value in manifest.items() if key != "compiler_build_digest"
    }
    normalized_manifest["build_profile"] = "<I2_ALLOWED_BUILD_PROFILE>"
    normalized_manifest["build_configuration_digest"] = (
        "<I2_ALLOWED_BUILD_CONFIGURATION_DIGEST>"
    )
    replacement = (
        "<I2_NORMALIZED_COMPILER_BUILD_MANIFEST:"
        f"{_canonical_sha256(normalized_manifest)}>"
    )
    normalized_text = text[: match.start()] + replacement + text[match.end() :]
    return _sha256_bytes(normalized_text.encode("utf-8"))


def _generated_output_cross_profile_sha256(path: Path) -> str:
    if path.name == "build_qualified_admission_profile_generated.hpp":
        return _build_qualified_profile_cross_profile_sha256(path)
    return _sha256_file(path)


def generated_output_cross_profile_sha256(
    path: str | os.PathLike[str],
) -> str:
    """Recompute the exact cross-profile digest for one physical generated file."""

    physical = _regular_file(Path(path), "cross-profile generated output")
    return _generated_output_cross_profile_sha256(physical)


def compile_dependency_cross_profile_sha256(
    path: str | os.PathLike[str], scope: str
) -> str:
    """Recompute the only profile-normalized compiler-dependency digests."""

    physical = _regular_file(Path(path), "cross-profile compiler dependency")
    if scope == "configure_generated_input":
        return _configure_header_cross_profile_sha256(physical)
    if scope == "generated_build_input":
        return _generated_output_cross_profile_sha256(physical)
    return _sha256_file(physical)


def _file_api_source_evidence(
    source: Path,
    *,
    build: "_Build",
    roots: "_Roots",
    label: str,
) -> dict[str, str]:
    """Retain physical bytes while separately normalizing generated profile bytes."""

    source_file = _regular_file(source, label)
    raw_digest = _sha256_file(source_file)
    return {
        "path": roots.logical(source),
        "sha256": raw_digest,
        "cross_profile_sha256": (
            _generated_output_cross_profile_sha256(source_file)
            if _is_within(source, build.root)
            else raw_digest
        ),
    }


def _compile_dependency_closure(
    *,
    build: _Build,
    record: _CompileRecord,
    commands: Mapping[Path, _Command],
    roots: _Roots,
    label: str,
) -> dict[str, Any]:
    """Authenticate every file the compiler reports reading for one object."""

    try:
        object_node = record.output.relative_to(build.root).as_posix()
    except ValueError:
        _fail(f"{label} object escapes its build")
    text = _run_text(
        ["/usr/bin/ninja", "-C", str(build.root), "-t", "deps", object_node],
        build.root,
        f"{label} Ninja deps",
    )
    lines = text.splitlines()
    if not lines:
        _fail(f"{label} Ninja deps is empty")
    match = re.fullmatch(
        rf"{re.escape(object_node)}: #deps ([0-9]+), deps mtime [0-9]+ \(VALID\)",
        lines[0],
    )
    if match is None:
        _fail(f"{label} Ninja deps has an unsupported header")
    declared_count = int(match.group(1))
    dependencies: list[Path] = []
    for line_index, line in enumerate(lines[1:], 1):
        if not line:
            continue
        if not line.startswith("    ") or not line.strip():
            _fail(f"{label} Ninja deps has a malformed row {line_index}")
        dependencies.append(
            Path(
                os.path.normpath(
                    roots.decode(
                        line.strip(),
                        build.root,
                        f"{label} Ninja dependency {line_index}",
                    )
                )
            )
        )
    if (
        not dependencies
        or declared_count != len(dependencies)
        or len(dependencies) != len(set(dependencies))
        or Path(os.path.normpath(record.source)) not in dependencies
    ):
        _fail(
            f"{label} Ninja deps count/source closure is incomplete or ambiguous"
        )

    rows: list[dict[str, Any]] = []
    for dependency in sorted(dependencies, key=roots.logical):
        producer: dict[str, Any] | None = None
        source_identity: dict[str, str] | None = None
        inspect_assembler_inputs = False
        if _is_within(dependency, roots.repository):
            scope = "repository_source"
            resolved, chain = _symlink_chain(
                dependency,
                roots.repository,
                roots,
                f"{label} repository dependency",
            )
            if dependency != resolved:
                _fail(f"{label} repository compiler dependency is a symlink")
            source_identity = _git_tracked_file_identity(
                resolved,
                roots.repository,
                f"{label} repository compiler dependency",
            )
            inspect_assembler_inputs = True
        elif _is_within(dependency, build.source_root):
            scope = "qualified_external_source"
            resolved, chain = _symlink_chain(
                dependency,
                build.source_root,
                roots,
                f"{label} qualified external-source dependency",
            )
            if dependency != resolved:
                _fail(f"{label} qualified external compiler dependency is a symlink")
            external_git_root = _containing_git_root(
                build.source_root,
                roots.output,
                f"{label} qualified external source",
            )
            source_identity = _git_tracked_file_identity(
                resolved,
                external_git_root,
                f"{label} qualified external compiler dependency",
            )
            inspect_assembler_inputs = True
        elif dependency in build.qualified_install_inputs:
            scope = "qualified_install_input"
            installed_owner = build.qualified_install_inputs[dependency]
            resolved, chain = _symlink_chain(
                dependency,
                roots.output,
                roots,
                f"{label} qualified install dependency",
            )
            producer = {
                "kind": "qualified_install_input",
                "owner": installed_owner,
            }
            inspect_assembler_inputs = True
        elif _is_within(dependency, roots.output):
            configure_producer = _configure_generated_dependency_evidence(
                dependency=dependency,
                build=build,
                commands=commands,
                roots=roots,
                label=label,
            )
            if configure_producer is not None:
                scope = "configure_generated_input"
                resolved, chain = _symlink_chain(
                    dependency,
                    roots.output,
                    roots,
                    f"{label} configure-generated dependency",
                )
                producer = configure_producer
            else:
                scope = "generated_build_input"
                command = commands.get(dependency)
                if command is None or command.kind != "generated" or len(command.inputs) != 1:
                    _fail(
                        f"{label} generated dependency has no unique parsed generator: "
                        f"{roots.logical(dependency)}"
                    )
                producer_artifact = Path(os.path.normpath(command.inputs[0]))
                owners = [
                    owner_name
                    for owner_name, owner_target in build.targets.items()
                    if owner_target.target_type == "EXECUTABLE"
                    and len(owner_target.artifacts) == 1
                    and Path(os.path.normpath(owner_target.artifacts[0]))
                    == producer_artifact
                ]
                if len(owners) != 1:
                    _fail(
                        f"{label} generated dependency has no unique File API "
                        "executable owner"
                    )
                producer_file = _regular_file(
                    producer_artifact,
                    f"{label} generated dependency producer",
                    contained_by=build.root,
                )
                if not os.access(producer_file, os.X_OK):
                    _fail(f"{label} generated dependency producer is not executable")
                resolved, chain = _symlink_chain(
                    dependency,
                    roots.output,
                    roots,
                    f"{label} generated dependency",
                )
                producer = {
                    "kind": "executable_build_generator",
                    "project_id": build.project_id,
                    "target": owners[0],
                    "artifact": roots.logical(producer_artifact),
                    "artifact_sha256": _sha256_file(producer_file),
                    "argv_sha256": command.argv_sha256,
                    "parity_argv_sha256": _parity_argv_sha256(
                        command.argv, build, roots
                    ),
                }
            inspect_assembler_inputs = True
        elif _compile_system_path(dependency):
            scope = "pinned_container"
            resolved = _regular_file(
                dependency,
                f"{label} pinned-system dependency",
                allow_symlink=True,
                contained_by=Path("/"),
                require_single_link=False,
            )
            if not _compile_system_path(resolved):
                _fail(
                    f"{label} pinned-system dependency resolved outside "
                    "pinned-system roots"
                )
            chain = list(
                dict.fromkeys((roots.logical(dependency), roots.logical(resolved)))
            )
        else:
            _fail(
                f"{label} compiler dependency escapes source, output, and "
                f"pinned-system roots: {roots.logical(dependency)}"
            )
        if inspect_assembler_inputs:
            _reject_untracked_assembler_file_inputs(
                resolved, f"{label} non-system compiler dependency"
            )
        rows.append(
            {
                "path": roots.logical(dependency),
                "resolved_path": roots.logical(resolved),
                "symlink_chain": chain,
                "sha256": _sha256_file(resolved),
                "cross_profile_sha256": (
                    compile_dependency_cross_profile_sha256(resolved, scope)
                ),
                "scope": scope,
                "producer": producer,
                "source_identity": source_identity,
            }
        )
    return {
        "object": roots.logical(record.output),
        "declared_dependency_count": declared_count,
        "raw_sha256": _sha256_bytes(
            roots.normalize_tool_output(text).encode("utf-8")
        ),
        "files": rows,
    }


def _parse_query(text: str, requested: str, label: str) -> tuple[str, list[tuple[str, str]]]:
    lines = [line.rstrip() for line in text.splitlines() if line.strip()]
    if not lines or not lines[0].endswith(":"):
        _fail(f"{label} returned an unsupported Ninja query shape")
    node = lines[0][:-1]
    if node != requested:
        _fail(f"{label} queried {requested!r} but Ninja described {node!r}")
    section: str | None = None
    rule: str | None = None
    dependencies: list[tuple[str, str]] = []
    for line in lines[1:]:
        stripped = line.strip()
        if stripped.startswith("input:"):
            section = "input"
            rule = stripped.partition(":")[2].strip()
            if not rule:
                _fail(f"{label} omits its input rule")
            continue
        if stripped == "outputs:":
            section = "outputs"
            continue
        if section != "input":
            continue
        edge_type = "explicit"
        dependency = stripped
        if dependency.startswith("|| "):
            edge_type, dependency = "order_only", dependency[3:]
        elif dependency.startswith("| "):
            edge_type, dependency = "implicit", dependency[2:]
        if not dependency:
            _fail(f"{label} has an empty dependency")
        dependencies.append((edge_type, dependency))
    if rule is None:
        _fail(f"{label} has no input rule")
    return rule, dependencies


def _query_closure(
    build: _Build,
    seed: str,
    roots: _Roots,
) -> tuple[list[dict[str, Any]], set[Path], set[Path]]:
    pending = [seed]
    seen: set[str] = set()
    rows: list[dict[str, Any]] = []
    code_paths: set[Path] = set()
    generated_sources: set[Path] = set()
    while pending:
        if len(seen) >= _MAX_QUERY_NODES:
            _fail(f"{build.project_id} Ninja query closure exceeds {_MAX_QUERY_NODES} nodes")
        node = pending.pop()
        if node in seen:
            continue
        seen.add(node)
        text = _run_text(
            ["/usr/bin/ninja", "-C", str(build.root), "-t", "query", node],
            build.root,
            f"{build.project_id} Ninja query {node}",
        )
        rule, dependencies = _parse_query(text, node, f"{build.project_id} Ninja query {node}")
        normalized = roots.normalize_tool_output(text)
        edges: list[dict[str, str]] = []
        for edge_type, dependency in dependencies:
            path = roots.decode(dependency, build.root, f"{build.project_id} Ninja dependency")
            edges.append({"type": edge_type, "path": roots.logical(path)})
            kind = _artifact_kind(path)
            if kind is not None and edge_type != "order_only" and _is_within(path, build.root):
                code_paths.add(Path(os.path.normpath(path)))
                pending.append(dependency)
            elif (
                path.suffix in _CODE_SOURCE_SUFFIXES
                and edge_type != "order_only"
                and _is_within(path, build.root)
            ):
                generated_sources.add(Path(os.path.normpath(path)))
                pending.append(dependency)
        rows.append(
            {
                "node": node,
                "rule": rule,
                "edges": edges,
                "raw_sha256": _sha256_bytes(normalized.encode("utf-8")),
            }
        )
    rows.sort(key=lambda row: row["node"])
    return rows, code_paths, generated_sources


def _archive_evidence(
    archive: Path,
    producer: _Command,
    roots: _Roots,
) -> dict[str, Any]:
    archive_file = _regular_file(archive, "local static archive", contained_by=roots.output)
    try:
        with archive_file.open("rb") as stream:
            magic = stream.read(8)
    except OSError as error:
        _fail(f"cannot inspect archive {archive}: {error}")
    if magic == b"!<thin>\n":
        _fail(f"thin archives are not qualified: {roots.logical(archive)}")
    if magic != b"!<arch>\n":
        _fail(f"local .a file is not a regular archive: {roots.logical(archive)}")
    member_text = _run_text(
        ["/usr/bin/ar", "t", str(archive_file)],
        archive_file.parent,
        f"archive member inspection for {archive.name}",
    )
    members = [line for line in member_text.splitlines() if line]
    if not members or len(members) != len(set(members)):
        _fail(f"archive {roots.logical(archive)} is empty or has duplicate member names")
    expected = [path.name for path in producer.inputs]
    if members != expected:
        _fail(
            f"archive {roots.logical(archive)} members disagree with its Ninja producer: "
            f"expected={expected}, actual={members}"
        )
    rows: list[dict[str, Any]] = []
    for member, source_object in zip(members, producer.inputs, strict=True):
        if _artifact_kind(source_object) != "object":
            _fail(
                f"archive {roots.logical(archive)} has a non-object producer member"
            )
        source = _regular_file(
            source_object,
            f"producer object for {archive.name}({member})",
            contained_by=roots.output,
        )
        extracted = _run_bytes(
            ["/usr/bin/ar", "p", str(archive_file), member],
            archive_file.parent,
            f"archive member extraction for {archive.name}({member})",
        )
        member_digest = _sha256_bytes(extracted)
        source_digest = _sha256_file(source)
        if member_digest != source_digest:
            _fail(f"archive member {archive.name}({member}) differs from its producer object")
        rows.append(
            {
                "name": member,
                "sha256": member_digest,
                "producer_object": roots.logical(source_object),
            }
        )
    return {
        "path": roots.logical(archive),
        "sha256": _sha256_file(archive_file),
        "members": rows,
    }


def _symlink_chain(path: Path, allowed_root: Path, roots: _Roots, label: str) -> tuple[Path, list[str]]:
    current = path
    seen: set[Path] = set()
    chain: list[str] = []
    for _ in range(32):
        try:
            metadata = current.lstat()
        except OSError as error:
            _fail(f"{label} is absent: {error}")
        chain.append(roots.logical(current))
        if not stat.S_ISLNK(metadata.st_mode):
            if not stat.S_ISREG(metadata.st_mode):
                _fail(f"{label} does not resolve to a regular file")
            if metadata.st_nlink != 1:
                _fail(f"{label} resolves to an unqualified hardlink")
            if not _is_within(current.resolve(strict=True), allowed_root):
                _fail(f"{label} escapes its allowed root")
            return current.resolve(strict=True), chain
        if current in seen:
            _fail(f"{label} contains a symlink cycle")
        seen.add(current)
        try:
            target = os.readlink(current)
        except OSError as error:
            _fail(f"{label} symlink cannot be read: {error}")
        current = Path(target) if Path(target).is_absolute() else current.parent / target
        current = Path(os.path.normpath(current))
        if not _is_within(current, allowed_root):
            _fail(f"{label} symlink escapes its allowed root")
    _fail(f"{label} exceeds the symlink-depth bound")


def _properties(test: Mapping[str, Any], label: str) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for index, raw in enumerate(_list(test.get("properties", []), f"{label}.properties")):
        row = _object(raw, f"{label}.properties[{index}]")
        name = _text(row.get("name"), f"{label}.properties[{index}].name")
        if name in result:
            _fail(f"{label} repeats CTest property {name}")
        result[name] = row.get("value")
    return result


def _environment(value: Any, label: str) -> dict[str, str]:
    if value is None:
        return {}
    if isinstance(value, str):
        entries = value.split(";") if value else []
    elif isinstance(value, list) and all(isinstance(item, str) for item in value):
        entries = value
    else:
        _fail(f"{label} is not a CTest ENVIRONMENT list")
    result: dict[str, str] = {}
    for entry in entries:
        name, separator, item = entry.partition("=")
        if (
            separator != "="
            or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name)
            or name in result
            or "\x00" in item
        ):
            _fail(f"{label} contains a malformed or duplicate environment entry")
        result[name] = item
    if {"LD_PRELOAD", "LD_AUDIT"}.intersection(result):
        _fail(f"{label} attempts loader injection")
    return result


def _name_digest(names: Sequence[str] | set[str]) -> str:
    return _sha256_bytes("".join(f"{name}\n" for name in sorted(names)).encode("utf-8"))


def _target_mapping(value: Mapping[str, Any], group: str) -> dict[str, tuple[int, str]]:
    raw: Mapping[str, Any]
    if group in value and isinstance(value[group], Mapping):
        raw = _object(value[group], f"target group {group}")
    else:
        raw = value
    result: dict[str, tuple[int, str]] = {}
    for target, item in raw.items():
        if not _TARGET.fullmatch(target):
            _fail(f"native target mapping has malformed target {target}")
        if isinstance(item, Mapping):
            row = _object(item, f"target mapping {target}")
            count, digest = row.get("count"), row.get("names_sha256")
        elif isinstance(item, (list, tuple)) and len(item) == 2:
            count, digest = item
        else:
            _fail(f"native target mapping row {target} is malformed")
        if (
            not isinstance(count, int)
            or isinstance(count, bool)
            or count <= 0
            or not isinstance(digest, str)
            or not _HEX64.fullmatch(digest)
        ):
            _fail(f"native target mapping row {target} has invalid count/digest")
        result[target] = (count, digest)
    if not result:
        _fail(f"native target group {group} is empty")
    return result


def _ordinary_mapping(value: Mapping[str, Any]) -> dict[str, tuple[str, str, str]]:
    result: dict[str, tuple[str, str, str]] = {}
    for name, item in value.items():
        if isinstance(item, Mapping):
            row = _object(item, f"ordinary mapping {name}")
            values = (
                row.get("container_path", row.get("source_path")),
                row.get("baseline_sha256"),
                row.get("candidate_sha256"),
            )
        elif isinstance(item, (list, tuple)) and len(item) == 3:
            values = tuple(item)
        else:
            _fail(f"ordinary mapping row {name} is malformed")
        path, baseline, candidate = values
        if (
            not isinstance(path, str)
            or not path.startswith("/workspace/")
            or not isinstance(baseline, str)
            or not _HEX64.fullmatch(baseline)
            or not isinstance(candidate, str)
            or not _HEX64.fullmatch(candidate)
        ):
            _fail(f"ordinary mapping row {name} has invalid path/digests")
        result[name] = (path, baseline, candidate)
    return result


@dataclass(frozen=True)
class _ProductTargetSpec:
    name: str
    target_type: str
    artifact: Path
    required_sources: tuple[Path, ...]
    required_generated_sources: tuple[Path, ...]


@dataclass(frozen=True)
class _ProductContract:
    project_id: str
    source_root: Path
    targets: dict[str, _ProductTargetSpec]
    test_only_targets: dict[str, _ProductTargetSpec]


@dataclass(frozen=True)
class _UtilityTargetContract:
    project_id: str
    target_name: str
    primary_output: Path
    declared_byproducts: tuple[Path, ...]
    producer_target: str
    producer_artifact: Path
    generator_output_option: str
    generator_output: Path
    housekeeping_policy: str
    output_inventory_policy: str
    consumer_relationships: tuple[tuple[str, str], ...]

    @property
    def consumer_targets(self) -> tuple[str, ...]:
        return tuple(target for target, _kind in self.consumer_relationships)


@dataclass(frozen=True)
class _ConfigureGeneratedInputContract:
    owner_project_id: str
    output_name: str
    allowed_build_relative_paths: tuple[Path, ...]
    template: Path
    semantic_inputs: tuple[Path, ...]


@dataclass(frozen=True)
class _ConfigureExecutedFixtureContract:
    owner_project_id: str
    required_build_testing: bool
    output_root: Path
    output_paths: tuple[Path, ...]
    cmake: Path
    tool: Path
    urdf: Path
    srdf: Path
    resource_root: Path
    resources: tuple[Path, ...]
    interpreter_toolchain_key: str
    model_asset_id: str
    accepted_package_uri_prefix: str


@dataclass(frozen=True)
class _AuxiliaryGeneratedInstallFile:
    install_relative: Path
    build_relative: Path
    producer_kind: str
    cmake_source_relative: Path
    template_source_relative: Path | None
    required_logical_targets: tuple[str, ...]


@dataclass(frozen=True)
class _AuxiliaryLibraryInstallFile:
    install_relative: Path
    target: str
    target_type: str
    build_artifact_relative: Path
    required_sources: tuple[Path, ...]


@dataclass(frozen=True)
class _AuxiliaryInstallContract:
    project_id: str
    manifest_row_count: int
    distinct_path_count: int
    manifest_relative_rows_sha256: str
    repeated_relative_paths: tuple[tuple[Path, int], ...]
    source_copy_prefixes: tuple[tuple[Path, Path], ...]
    expected_source_copy_count: int
    generated_files: tuple[_AuxiliaryGeneratedInstallFile, ...]
    library_files: tuple[_AuxiliaryLibraryInstallFile, ...]


@dataclass
class _AuxiliaryInstallState:
    build: "_Build"
    contract: _AuxiliaryInstallContract
    install_root: Path
    manifest_path: Path
    files_by_relative: dict[Path, Path]
    evidence: dict[str, Any]


@dataclass(frozen=True)
class _NativeLanePartition:
    production_targets: frozenset[str]
    test_only_targets: frozenset[str]


@dataclass(frozen=True)
class _BindingDependencyProject:
    project_id: str
    source_root: Path
    build_testing_cache_key: str
    production_targets: dict[str, _ProductTargetSpec]
    module_linked_archive_targets: frozenset[str]


@dataclass(frozen=True)
class _BindingBuildContract:
    source_root: Path
    artifact: Path
    registration_sources: tuple[Path, ...]
    registration_header: Path
    build_contract: Path
    forbidden_modules: frozenset[str]
    forbidden_dependency_tokens: tuple[str, ...]
    lane_configurations: dict[str, tuple[str, str]]
    dependency_projects: dict[str, _BindingDependencyProject]
    module_linked_archive_owners: frozenset[tuple[str, str]]
    pybind11_include_root: Path
    pybind11_required_headers: tuple[tuple[Path, str], ...]


@dataclass(frozen=True)
class _ProductionExclusions:
    target_names: tuple[re.Pattern[str], ...]
    repository_source_paths: tuple[re.Pattern[str], ...]
    dependency_names: tuple[re.Pattern[str], ...]


def _configure_generated_input_contracts(
    value: Mapping[str, Any],
    *,
    repository_root: Path,
) -> dict[str, _ConfigureGeneratedInputContract]:
    outer = _object(value, "build-provenance contract")
    if (
        outer.get("schema_version")
        != "manyforge.composable_cell_i2.build_provenance_contract.v1"
    ):
        _fail("configure-generated contracts require the whole build contract")
    result: dict[str, _ConfigureGeneratedInputContract] = {}
    expected_keys = {
        "owner_project_id",
        "output_name",
        "allowed_build_relative_paths",
        "template_repository_path",
        "semantic_input_policy",
        "required_semantic_repository_paths",
    }
    for index, raw in enumerate(
        _list(
            outer.get("configure_generated_input_contracts"),
            "configure_generated_input_contracts",
        )
    ):
        label = f"configure_generated_input_contracts[{index}]"
        row = _object(raw, label)
        if set(row) != expected_keys:
            _fail(f"{label} keys drifted")
        if (
            row.get("owner_project_id") != "manyforge_deployment"
            or row.get("semantic_input_policy")
            != "all_authenticated_cmake_inputs_and_exact_cache"
        ):
            _fail(f"{label} ownership/policy drifted")
        output_name = _text(row.get("output_name"), f"{label}.output_name")
        if (
            not re.fullmatch(r"embedded_[a-z0-9_]+\.hpp", output_name)
            or output_name in result
        ):
            _fail(f"{label}.output_name is duplicate or malformed")
        relative_values = _list(
            row.get("allowed_build_relative_paths"),
            f"{label}.allowed_build_relative_paths",
        )
        allowed_paths = tuple(
            _safe_relative(item, f"{label}.allowed_build_relative_paths")
            for item in relative_values
        )
        expected_paths = (
            Path("generated") / output_name,
            Path("manyforge_deployment/generated") / output_name,
        )
        if allowed_paths != expected_paths:
            _fail(f"{label} allowed build paths drifted")
        template_relative = _safe_relative(
            row.get("template_repository_path"), f"{label}.template"
        )
        template = _regular_file(
            repository_root / template_relative,
            f"{label} template",
            contained_by=repository_root,
        )
        semantic_values = _list(
            row.get("required_semantic_repository_paths"),
            f"{label}.required_semantic_repository_paths",
        )
        semantic_paths = tuple(
            _regular_file(
                repository_root / _safe_relative(item, f"{label} semantic input"),
                f"{label} semantic input",
                contained_by=repository_root,
            )
            for item in semantic_values
        )
        if not semantic_paths or len(semantic_paths) != len(set(semantic_paths)):
            _fail(f"{label} semantic inputs are empty or duplicate")
        result[output_name] = _ConfigureGeneratedInputContract(
            owner_project_id="manyforge_deployment",
            output_name=output_name,
            allowed_build_relative_paths=allowed_paths,
            template=template,
            semantic_inputs=semantic_paths,
        )
    if len(result) != 13:
        _fail("configure-generated input contract must contain exact 13 headers")
    return result


def _configure_executed_fixture_contracts(
    value: Mapping[str, Any],
    *,
    repository_root: Path,
) -> dict[tuple[str, Path], _ConfigureExecutedFixtureContract]:
    outer = _object(value, "build-provenance contract")
    if (
        outer.get("schema_version")
        != "manyforge.composable_cell_i2.build_provenance_contract.v1"
    ):
        _fail("configure-executed fixtures require the whole build contract")
    rows = _list(
        outer.get("configure_executed_fixture_contracts"),
        "configure_executed_fixture_contracts",
    )
    if len(rows) != 3:
        _fail("configure-executed fixture contract must contain exactly three rows")
    expected_keys = {
        "accepted_package_uri_prefix",
        "cmake_repository_path",
        "exact_output_relative_paths",
        "execution_policy",
        "interpreter_toolchain_key",
        "model_asset_id",
        "output_root_relative_path",
        "owner_project_id",
        "required_build_testing",
        "resource_repository_paths",
        "resource_root_repository_path",
        "srdf_repository_path",
        "tool_repository_path",
        "urdf_repository_path",
    }
    result: dict[tuple[str, Path], _ConfigureExecutedFixtureContract] = {}
    catalog_order: list[tuple[str, str]] = []
    for index, raw_row in enumerate(rows):
        label = f"configure_executed_fixture_contracts[{index}]"
        raw = _object(raw_row, label)
        if set(raw) != expected_keys:
            _fail(f"{label} keys drifted")
        owner = _text(raw.get("owner_project_id"), f"{label}.owner_project_id")
        if (
            owner not in {"manyforge_deployment", "manyforge_roboplan"}
            or raw.get("required_build_testing") is not True
            or raw.get("execution_policy")
            != "exact_cmake_execute_process_and_fresh_replay"
            or raw.get("interpreter_toolchain_key") != "python"
        ):
            _fail(f"{label} ownership/execution policy drifted")

        def repository_file(field_name: str) -> Path:
            return _regular_file(
                repository_root
                / _safe_relative(raw.get(field_name), f"{label}.{field_name}"),
                f"{label}.{field_name}",
                contained_by=repository_root,
            )

        output_root = _safe_relative(
            raw.get("output_root_relative_path"), f"{label}.output_root"
        )
        output_values = _list(
            raw.get("exact_output_relative_paths"), f"{label}.output_paths"
        )
        output_paths = tuple(
            _safe_relative(value, f"{label}.output_path")
            for value in output_values
        )
        if output_values != sorted(output_values) or output_paths != (
            Path("model.srdf"),
            Path("model.urdf"),
            Path("preparation-manifest.json"),
        ):
            _fail(f"{label} output path catalog drifted")
        resource_root = _directory(
            repository_root
            / _safe_relative(
                raw.get("resource_root_repository_path"),
                f"{label}.resource_root",
            ),
            f"{label}.resource_root",
        )
        resource_values = _list(
            raw.get("resource_repository_paths"), f"{label}.resources"
        )
        if resource_values != sorted(resource_values) or len(
            resource_values
        ) != len(set(resource_values)):
            _fail(f"{label} resource catalog is not exact/sorted")
        resources = tuple(
            _regular_file(
                repository_root
                / _safe_relative(value, f"{label}.resource_path"),
                f"{label}.resource",
                contained_by=resource_root,
            )
            for value in resource_values
        )
        mesh_root = _directory(resource_root / "meshes", f"{label}.mesh_root")
        actual_resources: set[Path] = set()
        for path in mesh_root.rglob("*"):
            metadata = path.lstat()
            if stat.S_ISLNK(metadata.st_mode):
                _fail(f"{label} resource tree contains a symlink")
            if stat.S_ISREG(metadata.st_mode):
                actual_resources.add(path.resolve(strict=True))
            elif not stat.S_ISDIR(metadata.st_mode):
                _fail(f"{label} resource tree contains a special file")
        if set(resources) != actual_resources:
            _fail(f"{label} resource filesystem differs from its exact catalog")
        cmake = repository_file("cmake_repository_path")
        expected_cmake = repository_root / (
            "manyforge_deployment/CMakeLists.txt"
            if owner == "manyforge_deployment"
            else "manyforge_roboplan/CMakeLists.txt"
        )
        if cmake != expected_cmake.resolve(strict=True):
            _fail(f"{label} CMake owner drifted")
        tool = repository_file("tool_repository_path")
        if tool != (
            repository_root / "manyforge_deployment/tools/prepare_structural_model.py"
        ).resolve(strict=True):
            _fail(f"{label} preparation tool drifted")
        urdf = repository_file("urdf_repository_path")
        srdf = repository_file("srdf_repository_path")
        if not _is_within(urdf, resource_root) or not _is_within(srdf, resource_root):
            _fail(f"{label} URDF/SRDF escape the resource root")
        key = (owner, output_root)
        if key in result:
            _fail(f"{label} duplicates one owner/output root")
        result[key] = _ConfigureExecutedFixtureContract(
            owner_project_id=owner,
            required_build_testing=True,
            output_root=output_root,
            output_paths=output_paths,
            cmake=cmake,
            tool=tool,
            urdf=urdf,
            srdf=srdf,
            resource_root=resource_root,
            resources=resources,
            interpreter_toolchain_key="python",
            model_asset_id=_text(raw.get("model_asset_id"), f"{label}.model_asset_id"),
            accepted_package_uri_prefix=_text(
                raw.get("accepted_package_uri_prefix"),
                f"{label}.accepted_package_uri_prefix",
            ),
        )
        catalog_order.append((owner, output_root.as_posix()))
    expected_catalog = {
        ("manyforge_deployment", Path("generated/lite6_lite_gripper_structural")),
        ("manyforge_deployment", Path("generated/structural_model_preparation_test")),
        ("manyforge_roboplan", Path("lite6_structural_consumer_fixture")),
    }
    if set(result) != expected_catalog or catalog_order != sorted(catalog_order):
        _fail("configure-executed fixture owner/output catalog drifted")
    return result


def _production_exclusions(value: Mapping[str, Any]) -> _ProductionExclusions:
    outer = _object(value, "build-provenance contract")
    if (
        outer.get("schema_version")
        != "manyforge.composable_cell_i2.build_provenance_contract.v1"
    ):
        _fail("production exclusions require the whole build-provenance contract")
    raw = _object(outer.get("production_exclusions"), "production_exclusions")
    expected = {
        "forbidden_target_name_patterns": [r"^perf_.*$", r"^test_.*$"],
        "forbidden_repository_source_path_patterns": [
            r"(^|/)test(/|$)",
            r"(^|/)tests(/|$)",
        ],
        "forbidden_dependency_name_patterns": [
            r"(^|::)GTest(::|$)",
            r"(^|::)[A-Za-z0-9_.+-]*test_support[A-Za-z0-9_.+-]*(::|$)",
            r"(?i)gmock",
            r"(?i)gtest",
        ],
    }
    if raw != expected:
        _fail("production exclusion regex contract drifted")

    def compile_field(field: str) -> tuple[re.Pattern[str], ...]:
        values = _list(raw.get(field), f"production_exclusions.{field}")
        try:
            return tuple(re.compile(_text(item, field)) for item in values)
        except re.error as error:
            _fail(f"production exclusion regex is malformed: {error}")

    return _ProductionExclusions(
        target_names=compile_field("forbidden_target_name_patterns"),
        repository_source_paths=compile_field(
            "forbidden_repository_source_path_patterns"
        ),
        dependency_names=compile_field("forbidden_dependency_name_patterns"),
    )


def _target_specs(
    value: Any,
    *,
    repository_root: Path,
    label: str,
) -> dict[str, _ProductTargetSpec]:
    rows: list[tuple[str | None, Any]]
    if isinstance(value, Mapping):
        rows = list(_object(value, label).items())
    else:
        rows = [(None, item) for item in _list(value, label)]
    result: dict[str, _ProductTargetSpec] = {}
    for index, (mapping_name, raw) in enumerate(rows):
        row_label = f"{label}[{index}]"
        if isinstance(raw, Mapping):
            item = _object(raw, row_label)
            allowed = {
                "name",
                "type",
                "artifact",
                "required_sources",
                "required_generated_sources",
            }
            if not set(item).issubset(allowed):
                _fail(f"{row_label} has unsupported target-contract keys")
            name = mapping_name or _text(item.get("name"), f"{row_label}.name")
            if mapping_name is not None and item.get("name", mapping_name) != mapping_name:
                _fail(f"{row_label} mapping key/name disagree")
            target_type = _text(item.get("type"), f"{row_label}.type")
            artifact_value = item.get("artifact")
            source_values = item.get("required_sources", [])
            generated_source_values = item.get("required_generated_sources", [])
        elif mapping_name is not None and isinstance(raw, (list, tuple)) and len(raw) == 2:
            name = mapping_name
            target_type, artifact_value = raw
            source_values = []
            generated_source_values = []
        elif mapping_name is None and isinstance(raw, (list, tuple)) and len(raw) in {3, 4}:
            name, target_type, artifact_value = raw[:3]
            source_values = raw[3] if len(raw) == 4 else []
            generated_source_values = []
        else:
            _fail(f"{row_label} is malformed")
        name = _text(name, f"{row_label}.name")
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.+-]*", name) or name in result:
            _fail(f"{row_label} has malformed or duplicate target name")
        if target_type not in _MATERIAL_TARGET_TYPES | {"UTILITY"}:
            _fail(f"{row_label} has unsupported material target type {target_type}")
        artifact = _safe_relative(artifact_value, f"{row_label}.artifact")
        sources: list[Path] = []
        for source_index, source_value in enumerate(_list(source_values, f"{row_label}.required_sources")):
            relative = _safe_relative(
                source_value, f"{row_label}.required_sources[{source_index}]"
            )
            source = _regular_file(
                repository_root / relative,
                f"{row_label} required source",
                contained_by=repository_root,
            )
            sources.append(source)
        if len(sources) != len(set(sources)):
            _fail(f"{row_label} repeats required sources")
        generated_sources = tuple(
            _safe_relative(
                generated_source,
                f"{row_label}.required_generated_sources[{generated_index}]",
            )
            for generated_index, generated_source in enumerate(
                _list(
                    generated_source_values,
                    f"{row_label}.required_generated_sources",
                )
            )
        )
        if len(generated_sources) != len(set(generated_sources)):
            _fail(f"{row_label} repeats required generated sources")
        if target_type == "UTILITY" and (sources or generated_sources):
            _fail(f"{row_label} UTILITY targets may not contract compiled sources")
        result[name] = _ProductTargetSpec(
            name=name,
            target_type=target_type,
            artifact=artifact,
            required_sources=tuple(sources),
            required_generated_sources=generated_sources,
        )
    return result


def _production_contract(
    value: Mapping[str, Any],
    *,
    expected_project: str | None,
    repository_root: Path,
) -> _ProductContract:
    raw = _object(value, "production target contract")
    if "projects" in raw:
        projects = _object(raw["projects"], "production target contract projects")
        if expected_project is None or expected_project not in projects:
            _fail("production target contract does not contain the selected project")
        raw = _object(projects[expected_project], f"production project {expected_project}")
    elif expected_project is not None and expected_project in raw and isinstance(raw[expected_project], Mapping):
        raw = _object(raw[expected_project], f"production project {expected_project}")
    allowed = {
        "schema_version",
        "project_id",
        "source_root",
        "targets",
        "test_only_targets",
        "native_lane_target_partitions",
    }
    if set(raw) - allowed:
        _fail("production target contract has unsupported keys")
    schema = raw.get("schema_version", PRODUCTION_TARGET_CONTRACT_SCHEMA)
    if schema != PRODUCTION_TARGET_CONTRACT_SCHEMA:
        _fail("production target contract schema drifted")
    project_id = _text(raw.get("project_id", expected_project), "production project_id")
    if expected_project is not None and project_id != expected_project:
        _fail("production target contract selected the wrong project")
    source_relative = _safe_relative(
        raw.get("source_root", project_id), "production source_root"
    )
    source_root = _directory(
        repository_root / source_relative, "production contract source root"
    )
    targets = _target_specs(
        raw.get("targets"),
        repository_root=repository_root,
        label=f"production {project_id} targets",
    )
    test_only = _target_specs(
        raw.get("test_only_targets", []),
        repository_root=repository_root,
        label=f"production {project_id} test-only targets",
    )
    if not targets or set(targets).intersection(test_only):
        _fail("production target contract is empty or overlaps test-only targets")
    return _ProductContract(project_id, source_root, targets, test_only)


def _utility_target_contracts(
    value: Mapping[str, Any],
    *,
    repository_root: Path,
) -> dict[tuple[str, str], _UtilityTargetContract]:
    outer = _object(value, "build-provenance contract")
    if outer.get("schema_version") != "manyforge.composable_cell_i2.build_provenance_contract.v1":
        _fail("utility contracts require the whole build-provenance contract")
    projects = _object(outer.get("projects"), "build-provenance projects")
    project_contracts = {
        project_id: _production_contract(
            outer,
            expected_project=project_id,
            repository_root=repository_root,
        )
        for project_id in projects
    }
    rows = _list(
        outer.get("utility_target_contracts"), "utility_target_contracts"
    )
    result: dict[tuple[str, str], _UtilityTargetContract] = {}
    allowed = {
        "project_id",
        "target_name",
        "file_api_type",
        "file_api_artifact_policy",
        "primary_output_relative_path",
        "declared_byproduct_relative_paths",
        "producer_target",
        "producer_executable_artifact_relative_path",
        "generator_output_option",
        "generator_output_relative_path",
        "housekeeping_policy",
        "output_inventory_policy",
        "consumer_relationships",
    }
    for index, raw in enumerate(rows):
        row = _object(raw, f"utility_target_contracts[{index}]")
        if set(row) != allowed:
            _fail(f"utility_target_contracts[{index}] keys drifted")
        project_id = _text(
            row.get("project_id"), f"utility_target_contracts[{index}].project_id"
        )
        target_name = _text(
            row.get("target_name"), f"utility_target_contracts[{index}].target_name"
        )
        if project_id not in project_contracts:
            _fail(f"utility target row references unknown project {project_id}")
        if (
            row.get("file_api_type") != "UTILITY"
            or row.get("file_api_artifact_policy") != "absent_for_utility"
        ):
            _fail(f"utility target row {project_id}/{target_name} policy drifted")
        primary_output = _safe_relative(
            row.get("primary_output_relative_path"),
            f"utility target {project_id}/{target_name} primary output",
        )
        byproduct_values = _list(
            row.get("declared_byproduct_relative_paths"),
            f"utility target {project_id}/{target_name} declared byproducts",
        )
        declared_byproducts = tuple(
            _safe_relative(
                value,
                f"utility target {project_id}/{target_name} declared byproduct",
            )
            for value in byproduct_values
        )
        if len(declared_byproducts) != len(set(declared_byproducts)):
            _fail(f"utility target {project_id}/{target_name} repeats a byproduct")
        producer_target = _text(
            row.get("producer_target"),
            f"utility target {project_id}/{target_name} producer",
        )
        producer_artifact = _safe_relative(
            row.get("producer_executable_artifact_relative_path"),
            f"utility target {project_id}/{target_name} producer artifact",
        )
        generator_output_option = _text(
            row.get("generator_output_option"),
            f"utility target {project_id}/{target_name} generator output option",
        )
        generator_output = _safe_relative(
            row.get("generator_output_relative_path"),
            f"utility target {project_id}/{target_name} generator output",
        )
        housekeeping_policy = _text(
            row.get("housekeeping_policy"),
            f"utility target {project_id}/{target_name} housekeeping policy",
        )
        output_inventory_policy = _text(
            row.get("output_inventory_policy"),
            f"utility target {project_id}/{target_name} output inventory policy",
        )
        if generator_output_option == "--output":
            if (
                generator_output != primary_output
                or declared_byproducts
                or housekeeping_policy != "make_output_parent"
                or output_inventory_policy != "single_file"
            ):
                _fail(
                    f"utility target {project_id}/{target_name} file-output policy drifted"
                )
        elif generator_output_option == "--output-root":
            descendants = (primary_output, *declared_byproducts)
            if (
                not descendants
                or any(
                    path == generator_output
                    or not path.is_relative_to(generator_output)
                    for path in descendants
                )
                or housekeeping_policy != "replace_output_root"
                or output_inventory_policy != "exact_bounded_tree"
            ):
                _fail(
                    f"utility target {project_id}/{target_name} root-output policy drifted"
                )
        else:
            _fail(
                f"utility target {project_id}/{target_name} generator option is unsupported"
            )
        relationship_values = _list(
            row.get("consumer_relationships"),
            f"utility target {project_id}/{target_name} consumer relationships",
        )
        relationships: list[tuple[str, str]] = []
        for relationship_index, raw_relationship in enumerate(relationship_values):
            relationship = _object(
                raw_relationship,
                f"utility target {project_id}/{target_name} consumer relationship "
                f"{relationship_index}",
            )
            if set(relationship) != {"target", "kind"}:
                _fail(
                    f"utility target {project_id}/{target_name} consumer relationship "
                    "keys drifted"
                )
            consumer = _text(
                relationship.get("target"),
                f"utility target {project_id}/{target_name} consumer target",
            )
            relationship_kind = _text(
                relationship.get("kind"),
                f"utility target {project_id}/{target_name} relationship kind",
            )
            if relationship_kind not in {
                "compiled_generated_input",
                "target_dependency",
            }:
                _fail(
                    f"utility target {project_id}/{target_name} relationship kind drifted"
                )
            relationships.append((consumer, relationship_kind))
        if not relationships or len(relationships) != len(set(relationships)):
            _fail(
                f"utility target {project_id}/{target_name} consumers are empty/duplicate"
            )
        key = (project_id, target_name)
        if key in result:
            _fail(f"utility target contract duplicates {project_id}/{target_name}")

        project = project_contracts[project_id]
        all_specs = {**project.targets, **project.test_only_targets}
        utility_spec = all_specs.get(target_name)
        producer_spec = all_specs.get(producer_target)
        if (
            utility_spec is None
            or utility_spec.target_type != "UTILITY"
            or utility_spec.artifact != primary_output
        ):
            _fail(f"utility target row {project_id}/{target_name} disagrees with target specs")
        if (
            producer_spec is None
            or producer_spec.target_type != "EXECUTABLE"
            or producer_spec.artifact != producer_artifact
        ):
            _fail(f"utility target row {project_id}/{target_name} producer spec drifted")
        expected_compiled_consumers = {
            name
            for name, spec in all_specs.items()
            if primary_output in spec.required_generated_sources
        }
        contracted_compiled_consumers = {
            target for target, kind in relationships
            if kind == "compiled_generated_input"
        }
        if contracted_compiled_consumers != expected_compiled_consumers:
            _fail(
                f"utility target row {project_id}/{target_name} compiled consumer set "
                f"drifted: expected={sorted(expected_compiled_consumers)}, "
                f"actual={sorted(contracted_compiled_consumers)}"
            )
        if any(target not in all_specs for target, _kind in relationships):
            _fail(
                f"utility target row {project_id}/{target_name} references an unknown consumer"
            )
        result[key] = _UtilityTargetContract(
            project_id=project_id,
            target_name=target_name,
            primary_output=primary_output,
            declared_byproducts=declared_byproducts,
            producer_target=producer_target,
            producer_artifact=producer_artifact,
            generator_output_option=generator_output_option,
            generator_output=generator_output,
            housekeeping_policy=housekeeping_policy,
            output_inventory_policy=output_inventory_policy,
            consumer_relationships=tuple(relationships),
        )
    expected_keys = {
        (project_id, name)
        for project_id, project in project_contracts.items()
        for name, spec in {**project.targets, **project.test_only_targets}.items()
        if spec.target_type == "UTILITY"
    }
    if set(result) != expected_keys:
        _fail(
            "utility target contract catalog is incomplete or extra: "
            f"expected={sorted(expected_keys)}, actual={sorted(result)}"
        )
    return result


def _auxiliary_install_contracts(
    value: Mapping[str, Any],
) -> dict[str, _AuxiliaryInstallContract]:
    outer = _object(value, "build-provenance contract")
    raw_contract = _object(
        outer.get("auxiliary_install_contract"), "auxiliary_install_contract"
    )
    if set(raw_contract) != {
        "schema_version",
        "source_generation",
        "projects",
    } or raw_contract.get(
        "schema_version"
    ) != "manyforge.composable_cell_i2.auxiliary_install_contract.v1":
        _fail("auxiliary install contract shape/schema drifted")
    source_generation = _object(
        raw_contract.get("source_generation"),
        "auxiliary install source generation",
    )
    expected_generation = {
        "base_commit": "e06d1a2f1b749c38742aee9e401bbe97fa967f7b",
        "base_tree": "d918767326a19d44886c0a4560cabe5a4ebe044d",
        "patched_index_tree": "687405b677d50965c9ce5701016cb67c4579c563",
        "staged_diff_sha256": "3505ad3d5d71bf738cb369658312eba7f1b67eeddce83037dafb0c2bb9ef00bf",
        "staged_path_count": 24,
        "staged_paths_sha256": "2f3cd1f83cd55709b9b8d2c68cd050c02a53e6f4dcd21ecf0d9a33c04b714f93",
    }
    if source_generation != expected_generation:
        _fail("auxiliary install source-generation identity drifted")
    projects = _object(
        raw_contract.get("projects"), "auxiliary install contract projects"
    )
    if set(projects) != {"roboplan", "roboplan_example_models"}:
        _fail("auxiliary install contract project set drifted")
    result: dict[str, _AuxiliaryInstallContract] = {}
    for project_id in sorted(projects):
        label = f"auxiliary install contract {project_id}"
        row = _object(projects[project_id], label)
        if set(row) != {
            "manifest",
            "source_copy_prefixes",
            "expected_source_copy_count",
            "generated_files",
            "library_files",
        }:
            _fail(f"{label} keys drifted")
        manifest = _object(row.get("manifest"), f"{label} manifest")
        if set(manifest) != {
            "row_count",
            "distinct_path_count",
            "relative_rows_sha256",
            "repeated_relative_paths",
        }:
            _fail(f"{label} manifest keys drifted")
        row_count = manifest.get("row_count")
        distinct_count = manifest.get("distinct_path_count")
        source_count = row.get("expected_source_copy_count")
        manifest_digest = _text(
            manifest.get("relative_rows_sha256"), f"{label} manifest digest"
        )
        if (
            not all(
                isinstance(count, int)
                and not isinstance(count, bool)
                and count > 0
                for count in (row_count, distinct_count, source_count)
            )
            or not _HEX64.fullmatch(manifest_digest)
        ):
            _fail(f"{label} manifest/count contract is malformed")
        repeated: list[tuple[Path, int]] = []
        for index, raw_repeat in enumerate(
            _list(
                manifest.get("repeated_relative_paths"),
                f"{label} repeated manifest paths",
            )
        ):
            repeat_label = f"{label} repeated manifest path[{index}]"
            repeat = _object(raw_repeat, repeat_label)
            if set(repeat) != {"path", "count"}:
                _fail(f"{repeat_label} keys drifted")
            path = _safe_relative(repeat.get("path"), f"{repeat_label}.path")
            count = repeat.get("count")
            if not isinstance(count, int) or isinstance(count, bool) or count < 2:
                _fail(f"{repeat_label}.count is malformed")
            repeated.append((path, count))
        if not repeated or len(repeated) != len({path for path, _ in repeated}):
            _fail(f"{label} repeated manifest paths are empty/duplicated")

        prefixes: list[tuple[Path, Path]] = []
        for index, raw_prefix in enumerate(
            _list(row.get("source_copy_prefixes"), f"{label} source prefixes")
        ):
            prefix_label = f"{label} source prefix[{index}]"
            prefix = _object(raw_prefix, prefix_label)
            if set(prefix) != {"install_prefix", "source_prefix"}:
                _fail(f"{prefix_label} keys drifted")
            prefixes.append(
                (
                    _safe_relative(
                        prefix.get("install_prefix"),
                        f"{prefix_label}.install_prefix",
                    ),
                    _safe_relative(
                        prefix.get("source_prefix"),
                        f"{prefix_label}.source_prefix",
                    ),
                )
            )
        if (
            not prefixes
            or prefixes
            != sorted(prefixes, key=lambda item: item[0].as_posix())
            or len({install for install, _source in prefixes}) != len(prefixes)
        ):
            _fail(f"{label} source prefixes are not exact/sorted")

        generated: list[_AuxiliaryGeneratedInstallFile] = []
        for index, raw_generated in enumerate(
            _list(row.get("generated_files"), f"{label} generated files")
        ):
            generated_label = f"{label} generated file[{index}]"
            generated_row = _object(raw_generated, generated_label)
            if set(generated_row) != {
                "install_relative_path",
                "build_relative_path",
                "producer_kind",
                "cmake_source_relative_path",
                "template_source_relative_path",
                "required_logical_targets",
            }:
                _fail(f"{generated_label} keys drifted")
            producer_kind = _text(
                generated_row.get("producer_kind"),
                f"{generated_label}.producer_kind",
            )
            if producer_kind not in {
                "configure_package_config_file_final",
                "write_basic_package_version_file",
                "install_export_main",
                "install_export_configuration",
            }:
                _fail(f"{generated_label} producer kind drifted")
            template_value = generated_row.get("template_source_relative_path")
            template = (
                None
                if template_value is None
                else _safe_relative(
                    template_value, f"{generated_label}.template source"
                )
            )
            if (producer_kind == "configure_package_config_file_final") != (
                template is not None
            ):
                _fail(f"{generated_label} template/final-writer contract drifted")
            logical_targets = tuple(
                _text(target, f"{generated_label} logical target")
                for target in _list(
                    generated_row.get("required_logical_targets"),
                    f"{generated_label} logical targets",
                )
            )
            if list(logical_targets) != sorted(set(logical_targets)):
                _fail(f"{generated_label} logical targets are not exact/sorted")
            if producer_kind.startswith("install_export_") != bool(logical_targets):
                _fail(f"{generated_label} exported-target contract drifted")
            generated.append(
                _AuxiliaryGeneratedInstallFile(
                    install_relative=_safe_relative(
                        generated_row.get("install_relative_path"),
                        f"{generated_label}.install path",
                    ),
                    build_relative=_safe_relative(
                        generated_row.get("build_relative_path"),
                        f"{generated_label}.build path",
                    ),
                    producer_kind=producer_kind,
                    cmake_source_relative=_safe_relative(
                        generated_row.get("cmake_source_relative_path"),
                        f"{generated_label}.CMake source",
                    ),
                    template_source_relative=template,
                    required_logical_targets=logical_targets,
                )
            )
        if not generated or [item.install_relative for item in generated] != sorted(
            {item.install_relative for item in generated}, key=lambda path: path.as_posix()
        ):
            _fail(f"{label} generated file set is not exact/sorted")

        libraries: list[_AuxiliaryLibraryInstallFile] = []
        for index, raw_library in enumerate(
            _list(row.get("library_files"), f"{label} library files")
        ):
            library_label = f"{label} library file[{index}]"
            library = _object(raw_library, library_label)
            if set(library) != {
                "install_relative_path",
                "target",
                "type",
                "build_artifact_relative_path",
                "required_sources",
            }:
                _fail(f"{library_label} keys drifted")
            required_sources = tuple(
                _safe_relative(source, f"{library_label}.required source")
                for source in _list(
                    library.get("required_sources"),
                    f"{library_label}.required sources",
                )
            )
            if (
                not required_sources
                or list(required_sources)
                != sorted(set(required_sources), key=lambda path: path.as_posix())
            ):
                _fail(f"{library_label} required sources are not exact/sorted")
            libraries.append(
                _AuxiliaryLibraryInstallFile(
                    install_relative=_safe_relative(
                        library.get("install_relative_path"),
                        f"{library_label}.install path",
                    ),
                    target=_text(library.get("target"), f"{library_label}.target"),
                    target_type=_text(
                        library.get("type"), f"{library_label}.target type"
                    ),
                    build_artifact_relative=_safe_relative(
                        library.get("build_artifact_relative_path"),
                        f"{library_label}.build artifact",
                    ),
                    required_sources=required_sources,
                )
            )
        if not libraries or [item.install_relative for item in libraries] != sorted(
            {item.install_relative for item in libraries}, key=lambda path: path.as_posix()
        ):
            _fail(f"{label} library file set is not exact/sorted")
        library_paths = {item.install_relative for item in libraries}
        if {path for path, _count in repeated} != library_paths.intersection(
            {path for path, _count in repeated}
        ):
            _fail(f"{label} repeats a non-library manifest path")
        if (
            source_count + len(generated) + len(libraries) != distinct_count
            or row_count
            != distinct_count + sum(count - 1 for _path, count in repeated)
        ):
            _fail(f"{label} owner/multiplicity counts disagree")
        expected_library_contracts = {
            "roboplan": {
                "filters": (
                    "SHARED_LIBRARY",
                    Path("libfilters.so"),
                    (Path("src/filters/se3_low_pass_filter.cpp"),),
                ),
                "roboplan": (
                    "SHARED_LIBRARY",
                    Path("libroboplan.so"),
                    tuple(
                        Path(value)
                        for value in (
                            "src/core/collision_context.cpp",
                            "src/core/path_utils.cpp",
                            "src/core/pose_utils.cpp",
                            "src/core/scene.cpp",
                            "src/core/scene_utils.cpp",
                            "src/core/types.cpp",
                        )
                    ),
                ),
            },
            "roboplan_example_models": {
                "roboplan_example_models": (
                    "SHARED_LIBRARY",
                    Path("libroboplan_example_models.so"),
                    (Path("src/anchor.cpp"), Path("src/resources.cpp")),
                ),
            },
        }
        decoded_library_contracts = {
            item.target: (
                item.target_type,
                item.build_artifact_relative,
                item.required_sources,
            )
            for item in libraries
        }
        if decoded_library_contracts != expected_library_contracts[project_id]:
            _fail(f"{label} target artifact/source contracts drifted")
        result[project_id] = _AuxiliaryInstallContract(
            project_id=project_id,
            manifest_row_count=row_count,
            distinct_path_count=distinct_count,
            manifest_relative_rows_sha256=manifest_digest,
            repeated_relative_paths=tuple(repeated),
            source_copy_prefixes=tuple(prefixes),
            expected_source_copy_count=source_count,
            generated_files=tuple(generated),
            library_files=tuple(libraries),
        )
    return result


def _native_lane_partition(
    value: Mapping[str, Any],
    *,
    lane_id: str,
    contract: _ProductContract,
) -> _NativeLanePartition:
    outer = _object(value, "production target contract")
    raw_partitions = outer.get("native_lane_target_partitions")
    if raw_partitions is None:
        _fail("production target contract lacks native_lane_target_partitions")
    partition_object = _object(
        raw_partitions, "native_lane_target_partitions"
    )
    schema = partition_object.get(
        "schema_version", NATIVE_LANE_TARGET_PARTITION_SCHEMA
    )
    if schema != NATIVE_LANE_TARGET_PARTITION_SCHEMA:
        _fail("native lane target partition schema drifted")
    if set(partition_object) - {"schema_version", "lanes"}:
        _fail("native lane target partitions have unsupported keys")
    lanes = _object(
        partition_object.get("lanes"), "native_lane_target_partitions.lanes"
    )
    if lane_id not in lanes:
        _fail(f"native lane target partitions omit {lane_id}")
    row = _object(lanes[lane_id], f"native lane target partition {lane_id}")
    if set(row) != {"production_targets", "test_only_targets"}:
        _fail(f"native lane target partition {lane_id} keys drifted")

    def names(field: str, known: Mapping[str, _ProductTargetSpec]) -> frozenset[str]:
        values = _list(row[field], f"native lane {lane_id} {field}")
        if not all(isinstance(item, str) and item for item in values):
            _fail(f"native lane {lane_id} {field} is malformed")
        result = frozenset(values)
        if len(result) != len(values) or not result.issubset(known):
            _fail(
                f"native lane {lane_id} {field} is duplicate or references an "
                "uncontracted target"
            )
        return result

    production = names("production_targets", contract.targets)
    test_only = names("test_only_targets", contract.test_only_targets)
    if production.intersection(test_only):
        _fail(f"native lane target partition {lane_id} overlaps roles")
    return _NativeLanePartition(production, test_only)


def _binding_build_contract(
    value: Mapping[str, Any],
    *,
    repository_root: Path,
    reify_external_headers: bool = True,
) -> _BindingBuildContract:
    outer = _object(value, "binding build-provenance contract")
    if outer.get("schema_version") != "manyforge.composable_cell_i2.build_provenance_contract.v1":
        _fail("binding caller did not supply the exact build-provenance contract")
    provenance = _object(
        outer.get("binding_build_contract"), "binding_build_contract"
    )
    constraints = _object(
        outer.get("binding_builder_constraints"),
        "binding_builder_constraints",
    )
    allowed = {
        "schema_version",
        "source_root",
        "artifact",
        "dependency_projects",
    }
    if set(provenance) != allowed:
        _fail("binding build_provenance keys drifted")
    if provenance.get("schema_version") != BINDING_BUILD_CONTRACT_SCHEMA:
        _fail("binding build-provenance schema drifted")
    source_root = _directory(
        repository_root
        / _safe_relative(provenance.get("source_root"), "binding source_root"),
        "binding source root",
    )
    if source_root != (repository_root / "manyforge_core/python").resolve(strict=True):
        _fail("binding source root is not exact manyforge_core/python")
    artifact = _safe_relative(provenance.get("artifact"), "binding artifact")
    if artifact.name != f"manyforge_core_py{_PYTHON_EXTENSION_SUFFIX}":
        _fail("binding artifact does not have the frozen CPython ABI suffix")

    constraint_allowed = {
        "schema_version",
        "build_recipe",
        "framework",
        "pybind11_build_provenance",
        "production_modules",
        "forbidden_production_modules",
        "target",
        "cpython_abi",
        "lanes",
        "forbidden_ros_or_roboplan_dependency_or_export_tokens",
        "forbidden_python_module_exports",
    }
    if set(constraints) != constraint_allowed or constraints.get(
        "schema_version"
    ) != "manyforge.composable_cell_i2.binding_builder_constraints.v1":
        _fail("binding builder constraints keys/schema drifted")
    if (
        constraints.get("framework") != "pybind11"
        or constraints.get("production_modules") != ["manyforge_core_py"]
    ):
        _fail("binding framework or sole production module drifted")
    pybind11 = _object(
        constraints.get("pybind11_build_provenance"),
        "binding pybind11 build provenance",
    )
    if set(pybind11) != {
        "version",
        "include_root",
        "required_public_headers",
    } or pybind11.get("version") != "3.0.3":
        _fail("binding pybind11 provenance keys/version drifted")
    pybind11_include_root = Path(
        _text(
            pybind11.get("include_root"),
            "binding pybind11 include root",
        )
    )
    if reify_external_headers:
        pybind11_include_root = _directory(
            pybind11_include_root, "binding pybind11 include root"
        )
    if (
        pybind11_include_root.as_posix()
        != "/usr/local/lib/python3.12/site-packages/pybind11/include"
    ):
        _fail("binding pybind11 include root drifted")
    pybind11_headers: list[tuple[Path, str]] = []
    for index, raw_header in enumerate(
        _list(
            pybind11.get("required_public_headers"),
            "binding pybind11 required public headers",
        )
    ):
        header = _object(raw_header, f"binding pybind11 header[{index}]")
        if set(header) != {"path", "sha256"}:
            _fail(f"binding pybind11 header[{index}] keys drifted")
        path = Path(
            _text(
                header.get("path"),
                f"binding pybind11 header[{index}] path",
            )
        )
        digest = _text(
            header.get("sha256"), f"binding pybind11 header[{index}] digest"
        )
        if (
            not path.is_absolute()
            or not path.is_relative_to(pybind11_include_root)
            or not _HEX64.fullmatch(digest)
            or (
                reify_external_headers
                and _sha256_file(
                    _regular_file(
                        path,
                        f"binding pybind11 header[{index}]",
                        contained_by=pybind11_include_root,
                        require_single_link=False,
                    )
                )
                != digest
            )
        ):
            _fail(f"binding pybind11 header[{index}] digest drifted")
        pybind11_headers.append((path, digest))
    if pybind11_headers != [
        (
            pybind11_include_root / "pybind11/pybind11.h",
            "8171c8ee463f7f5bdb8655ef6eb6aa8e8c34d18b74bb5e07d3e259149dc526ad",
        )
    ]:
        _fail("binding pybind11 required-header contract drifted")
    forbidden_production_values = _list(
        constraints.get("forbidden_production_modules"),
        "binding forbidden production modules",
    )
    if not all(isinstance(item, str) and item for item in forbidden_production_values):
        _fail("binding forbidden production modules are malformed")
    forbidden = frozenset(forbidden_production_values)
    if len(forbidden) != len(forbidden_production_values) or forbidden != {
        "manyforge_roboplan_py",
        "e0_nanobind",
        "e0_pybind",
    }:
        _fail("binding forbidden production-module set drifted")
    build_contract = _regular_file(
        repository_root
        / _safe_relative(
            constraints.get("build_recipe"), "binding build recipe"
        ),
        "binding build recipe",
        contained_by=repository_root,
    )
    target_constraint = _object(
        constraints.get("target"), "binding builder target"
    )
    if set(target_constraint) != {
        "name",
        "file_api_type",
        "artifact_relative_path",
        "required_translation_units",
        "required_header_dependencies",
        "required_upstream_archive_owners",
    }:
        _fail("binding builder target keys drifted")
    if (
        target_constraint.get("name") != "manyforge_core_py"
        or target_constraint.get("file_api_type") != "MODULE_LIBRARY"
        or target_constraint.get("artifact_relative_path") != artifact.as_posix()
    ):
        _fail("binding builder module target identity drifted")
    source_values = _list(
        target_constraint.get("required_translation_units"),
        "binding required_translation_units",
    )
    header_values = _list(
        target_constraint.get("required_header_dependencies"),
        "binding required_header_dependencies",
    )
    if not all(isinstance(item, str) and item for item in source_values + header_values):
        _fail("binding translation-unit/header contract is malformed")
    sources = tuple(
        _regular_file(
            repository_root
            / _safe_relative(item, f"binding required_translation_units[{index}]"),
            f"binding translation unit {index}",
            contained_by=repository_root,
        )
        for index, item in enumerate(source_values)
    )
    headers = tuple(
        _regular_file(
            repository_root
            / _safe_relative(item, f"binding required_header_dependencies[{index}]"),
            f"binding header dependency {index}",
            contained_by=repository_root,
        )
        for index, item in enumerate(header_values)
    )
    if (
        len(sources) != 7
        or len(sources) != len(set(sources))
        or any(path.suffix not in _CODE_SOURCE_SUFFIXES for path in sources)
        or len(headers) != 1
        or headers[0].name != "binding_registrations.hpp"
    ):
        _fail("binding translation-unit/header membership drifted")
    abi = _object(constraints.get("cpython_abi"), "binding CPython ABI")
    if set(abi) != {
        "implementation",
        "major_minor",
        "soabi",
        "extension_suffix",
        "module_filename",
        "required_exported_pyinit_symbols",
    } or (
        abi.get("implementation") != "CPython"
        or abi.get("major_minor") != "3.12"
        or abi.get("soabi") != "cpython-312-x86_64-linux-gnu"
        or abi.get("extension_suffix") != _PYTHON_EXTENSION_SUFFIX
        or abi.get("module_filename") != artifact.name
        or abi.get("required_exported_pyinit_symbols") != [
            "PyInit_manyforge_core_py"
        ]
    ):
        _fail("binding CPython ABI contract drifted")
    token_values = _list(
        constraints.get("forbidden_ros_or_roboplan_dependency_or_export_tokens"),
        "binding forbidden dependency tokens",
    )
    if not all(
        isinstance(item, str) and re.fullmatch(r"[a-z][a-z0-9_]*", item)
        for item in token_values
    ) or len(token_values) != len(set(token_values)):
        _fail("binding forbidden dependency-token contract is malformed")
    module_values = _list(
        constraints.get("forbidden_python_module_exports"),
        "binding forbidden module exports",
    )
    if not all(isinstance(item, str) and item for item in module_values):
        _fail("binding forbidden module-export contract is malformed")
    if not set(module_values).issubset(forbidden) or "manyforge_roboplan_py" not in module_values:
        _fail("binding forbidden module-export set disagrees with production policy")

    lane_configurations: dict[str, tuple[str, str]] = {}
    for index, raw_lane in enumerate(_list(constraints.get("lanes"), "binding lanes")):
        row = _object(raw_lane, f"binding lane constraint[{index}]")
        if set(row) != {
            "lane_id",
            "configuration",
            "sanitizer_profile",
            "required_sanitizers",
        }:
            _fail(f"binding lane constraint[{index}] keys drifted")
        lane_id = _text(row.get("lane_id"), f"binding lane constraint[{index}] id")
        configuration = _text(
            row.get("configuration"), f"binding lane constraint[{index}] configuration"
        )
        sanitizer_profile = _text(
            row.get("sanitizer_profile"),
            f"binding lane constraint[{index}] sanitizer",
        )
        expected_sanitizers = {
            "none": [],
            "address+undefined": ["address", "undefined"],
            "thread": ["thread"],
        }
        if (
            lane_id in lane_configurations
            or sanitizer_profile not in expected_sanitizers
            or row.get("required_sanitizers")
            != expected_sanitizers[sanitizer_profile]
            or configuration
            != ("Release" if sanitizer_profile == "none" else "RelWithDebInfo")
        ):
            _fail(f"binding lane constraint[{index}] profile drifted")
        lane_configurations[lane_id] = (configuration, sanitizer_profile)
    project_values = _object(
        provenance.get("dependency_projects"),
        "binding dependency_projects",
    )
    required_linked_targets = {
        "manyforge_core": {"manyforge_core", "manyforge_content_identity"},
        "manyforge_deployment": {
            "manyforge_deployment",
            "manyforge_deployment_verification_core",
        },
    }
    if set(project_values) != set(required_linked_targets):
        _fail("binding dependency project set drifted")
    owner_rows = _list(
        target_constraint.get("required_upstream_archive_owners"),
        "binding upstream archive owners",
    )
    owner_values: list[tuple[str, str]] = []
    for index, raw_owner in enumerate(owner_rows):
        owner = _object(raw_owner, f"binding upstream archive owner[{index}]")
        if set(owner) != {"project_id", "target"}:
            _fail(f"binding upstream archive owner[{index}] keys drifted")
        owner_values.append(
            (
                _text(
                    owner.get("project_id"),
                    f"binding upstream archive owner[{index}].project_id",
                ),
                _text(
                    owner.get("target"),
                    f"binding upstream archive owner[{index}].target",
                ),
            )
        )
    required_owner_values = sorted(
        (project_id, target)
        for project_id, targets in required_linked_targets.items()
        for target in targets
    )
    if owner_values != required_owner_values:
        _fail("binding upstream archive owner set drifted")
    dependency_projects: dict[str, _BindingDependencyProject] = {}
    for project_id in sorted(project_values):
        row = _object(
            project_values[project_id], f"binding dependency project {project_id}"
        )
        if set(row) != {
            "source_root",
            "build_testing_cache_key",
            "production_catalog_project_id",
            "production_targets_source",
            "module_linked_archive_targets",
        }:
            _fail(f"binding dependency project {project_id} keys drifted")
        project_contract = _production_contract(
            outer,
            expected_project=project_id,
            repository_root=repository_root,
        )
        project_source = _directory(
            repository_root
            / _safe_relative(
                row.get("source_root"),
                f"binding dependency project {project_id} source_root",
            ),
            f"binding dependency project {project_id} source root",
        )
        if (
            row.get("production_catalog_project_id") != project_id
            or row.get("production_targets_source")
            != "shared_projects_catalog"
            or project_source != project_contract.source_root
        ):
            _fail(
                f"binding dependency project {project_id} does not use its shared "
                "production catalog"
            )
        cache_key = _text(
            row.get("build_testing_cache_key"),
            f"binding dependency project {project_id} build-testing key",
        )
        if not re.fullmatch(r"[A-Z][A-Z0-9_]*BUILD_TESTING|BUILD_TESTING", cache_key):
            _fail(f"binding dependency project {project_id} cache key is malformed")
        linked_values = _list(
            row.get("module_linked_archive_targets"),
            f"binding dependency project {project_id} linked archives",
        )
        if (
            linked_values != sorted(required_linked_targets[project_id])
            or any(
                project_contract.targets[target].target_type
                not in {"STATIC_LIBRARY", "SHARED_LIBRARY"}
                for target in linked_values
            )
        ):
            _fail(
                f"binding dependency project {project_id} linked archive set drifted"
            )
        dependency_projects[project_id] = _BindingDependencyProject(
            project_id=project_id,
            source_root=project_source,
            build_testing_cache_key=cache_key,
            production_targets=project_contract.targets,
            module_linked_archive_targets=frozenset(linked_values),
        )
    return _BindingBuildContract(
        source_root=source_root,
        artifact=artifact,
        registration_sources=sources,
        registration_header=headers[0],
        build_contract=build_contract,
        forbidden_modules=forbidden,
        dependency_projects=dependency_projects,
        module_linked_archive_owners=frozenset(owner_values),
        forbidden_dependency_tokens=tuple(token_values),
        lane_configurations=lane_configurations,
        pybind11_include_root=pybind11_include_root,
        pybind11_required_headers=tuple(pybind11_headers),
    )


def _validate_configured_target_graph_contract(
    value: Mapping[str, Any],
    *,
    projects: Mapping[str, _ProductContract],
    auxiliary: Mapping[str, _AuxiliaryInstallContract],
    binding: _BindingBuildContract,
) -> None:
    outer = _object(value, "build-provenance contract")
    contract = _object(
        outer.get("configured_target_graph_contract"),
        "configured_target_graph_contract",
    )
    if set(contract) != {
        "schema_version",
        "options_sha256",
        "project_node_alias_contracts",
        "profiles",
    } or contract.get("schema_version") != (
        "manyforge.composable_cell_i2.configured_target_graph_contract.v1"
    ):
        _fail("configured-target graph contract shape/schema drifted")
    if contract.get("options_sha256") != _sha256_bytes(_GRAPHVIZ_OPTIONS):
        _fail("configured-target graph options digest drifted")
    aliases = _object(
        contract.get("project_node_alias_contracts"),
        "configured-target graph owner aliases",
    )
    if set(aliases) != {"manyforge_core_py"}:
        _fail("configured-target graph alias project set drifted")
    module_alias = _object(
        aliases["manyforge_core_py"], "manyforge_core_py graph aliases"
    )
    if set(module_alias) != {"absolute_build_sibling_nodes", "named_nodes"}:
        _fail("manyforge_core_py graph alias keys drifted")
    alias_values: list[str] = []
    absolute_rows = _list(
        module_alias.get("absolute_build_sibling_nodes"),
        "manyforge_core_py absolute graph aliases",
    )
    if absolute_rows != [
        {
            "alias": "<owner:manyforge_core/manyforge_core>",
            "artifact_relative_path": "libmanyforge_core.a",
            "expected_raw_shape": "septagon",
            "sibling_build_directory": "core",
        }
    ]:
        _fail("manyforge_core_py absolute graph owner alias drifted")
    alias_values.append(absolute_rows[0]["alias"])
    named_rows = _list(
        module_alias.get("named_nodes"), "manyforge_core_py named graph aliases"
    )
    expected_named_rows = [
        {
            "alias": "<owner:manyforge_core/manyforge_content_identity>",
            "expected_raw_shape": "octagon",
            "source": "manyforge_content_identity_archive",
        },
        {
            "alias": "<owner:manyforge_deployment/manyforge_deployment>",
            "expected_raw_shape": "octagon",
            "source": "manyforge_deployment_archive",
        },
        {
            "alias": (
                "<owner:manyforge_deployment/"
                "manyforge_deployment_verification_core>"
            ),
            "expected_raw_shape": "octagon",
            "source": "manyforge_deployment_verification_core_archive",
        },
    ]
    if named_rows != expected_named_rows:
        _fail("manyforge_core_py named graph owner aliases drifted")
    alias_values.extend(row["alias"] for row in named_rows)
    expected_owner_aliases = {
        f"<owner:{project_id}/{target}>"
        for project_id, target in binding.module_linked_archive_owners
    }
    if set(alias_values) != expected_owner_aliases:
        _fail("configured-target graph aliases disagree with binding owners")

    profiles = _object(contract.get("profiles"), "configured-target graph profiles")
    expected_profiles = {
        "manyforge_core.testing_on": ("manyforge_core", True),
        "manyforge_core.testing_off": ("manyforge_core", False),
        "manyforge_core_py.testing_off": ("manyforge_core_py", False),
        "manyforge_deployment.testing_on": ("manyforge_deployment", True),
        "manyforge_deployment.testing_off": ("manyforge_deployment", False),
        "manyforge_roboplan.testing_on": ("manyforge_roboplan", True),
        "roboplan.testing_off": ("roboplan", False),
        "roboplan_example_models.testing_off": (
            "roboplan_example_models",
            False,
        ),
    }
    if set(profiles) != set(expected_profiles):
        _fail("configured-target graph profile set drifted")
    auxiliary_targets = {
        project_id: {library.target for library in contract.library_files}
        for project_id, contract in auxiliary.items()
    }
    for profile_id in sorted(profiles):
        label = f"configured-target graph profile {profile_id}"
        row = _object(profiles[profile_id], label)
        if set(row) != {
            "project_id",
            "build_testing",
            "projection_sha256",
            "local_target_roles",
        }:
            _fail(f"{label} keys drifted")
        project_id, build_testing = expected_profiles[profile_id]
        if (
            row.get("project_id") != project_id
            or row.get("build_testing") is not build_testing
            or not _HEX64.fullmatch(
                _text(row.get("projection_sha256"), f"{label}.projection")
            )
        ):
            _fail(f"{label} identity drifted")
        roles = _object(row.get("local_target_roles"), f"{label} roles")
        if set(roles) != {"production", "test_only", "configured_only", "external"}:
            _fail(f"{label} role keys drifted")
        decoded_roles: dict[str, list[str]] = {}
        all_names: list[str] = []
        for role in ("production", "test_only", "configured_only", "external"):
            values = _list(roles.get(role), f"{label}.{role}")
            if (
                not all(isinstance(item, str) and item for item in values)
                or values != sorted(set(values))
            ):
                _fail(f"{label}.{role} is not exact/sorted")
            decoded_roles[role] = list(values)
            all_names.extend(values)
        if len(all_names) != len(set(all_names)):
            _fail(f"{label} target roles overlap")
        production = set(decoded_roles["production"])
        if project_id in projects:
            if production != set(projects[project_id].targets):
                _fail(f"{label} production role catalog drifted")
        elif project_id == "manyforge_core_py":
            if production != {"manyforge_core_py", *expected_owner_aliases}:
                _fail("manyforge_core_py graph production owner set drifted")
            if decoded_roles["test_only"] or decoded_roles["configured_only"]:
                _fail("manyforge_core_py graph exposes a non-production local target")
        else:
            if production != auxiliary_targets[project_id]:
                _fail(f"{label} auxiliary production role catalog drifted")
            expected_configured = ["tl_expected"] if project_id == "roboplan" else []
            if decoded_roles["configured_only"] != expected_configured:
                _fail(f"{label} auxiliary configured-only role drifted")


def _validate_product_contract_shape(
    raw: Mapping[str, Any],
    *,
    parsed: _ProductContract,
    expected_target_count: int,
    expected_test_only_count: int,
) -> None:
    label = f"production project {parsed.project_id}"
    if set(raw) != {
        "schema_version",
        "project_id",
        "source_root",
        "targets",
        "test_only_targets",
    } or raw.get("schema_version") != PRODUCTION_TARGET_CONTRACT_SCHEMA:
        _fail(f"{label} shape/schema drifted")
    if (
        raw.get("project_id") != parsed.project_id
        or raw.get("source_root") != parsed.project_id
        or len(parsed.targets) != expected_target_count
        or len(parsed.test_only_targets) != expected_test_only_count
    ):
        _fail(f"{label} identity/count drifted")
    for field, decoded in (
        ("targets", parsed.targets),
        ("test_only_targets", parsed.test_only_targets),
    ):
        rows = _list(raw.get(field), f"{label}.{field}")
        names: list[str] = []
        for index, raw_row in enumerate(rows):
            row_label = f"{label}.{field}[{index}]"
            row = _object(raw_row, row_label)
            if not {"name", "type", "artifact"}.issubset(row) or not set(row).issubset(
                {
                    "name",
                    "type",
                    "artifact",
                    "required_sources",
                    "required_generated_sources",
                }
            ):
                _fail(f"{row_label} keys drifted")
            name = _text(row.get("name"), f"{row_label}.name")
            names.append(name)
            if name not in decoded:
                _fail(f"{row_label} is absent from the decoded catalog")
            for source_field in ("required_sources", "required_generated_sources"):
                values = _list(row.get(source_field, []), f"{row_label}.{source_field}")
                if values != sorted(set(values)):
                    _fail(f"{row_label}.{source_field} is not exact/sorted")
        if names != sorted(set(names)) or set(names) != set(decoded):
            _fail(f"{label}.{field} catalog is not exact/sorted")


def validate_build_provenance_contract(
    value: Mapping[str, Any],
    repository_root: str | os.PathLike[str],
    *,
    reify_external_headers: bool = False,
) -> dict[str, Any]:
    """Purely decode every closed build-contract catalog and cross-section join."""

    root = _directory(Path(repository_root), "build-contract repository root")
    outer = _object(value, "build-provenance contract")
    if set(outer) != {
        "schema_version",
        "implementation_base",
        "closed_shape_policy",
        "toolchain_contract",
        "production_exclusions",
        "configure_generated_input_contracts",
        "configure_executed_fixture_contracts",
        "configured_target_graph_contract",
        "cross_profile_parity_contract",
        "auxiliary_install_contract",
        "projects",
        "native_lane_target_partitions",
        "production_release_parity_lanes",
        "utility_target_contracts",
        "binding_build_contract",
        "binding_builder_constraints",
    } or outer.get("schema_version") != (
        "manyforge.composable_cell_i2.build_provenance_contract.v1"
    ):
        _fail("build-provenance contract top-level shape/schema drifted")
    _toolchain_contract(outer)
    _production_exclusions(outer)
    raw_projects = _object(outer.get("projects"), "build-provenance projects")
    expected_counts = {
        "manyforge_core": (2, 3),
        "manyforge_deployment": (8, 0),
        "manyforge_roboplan": (4, 20),
    }
    if set(raw_projects) != set(expected_counts):
        _fail("build-provenance project set drifted")
    projects = {
        project_id: _production_contract(
            outer, expected_project=project_id, repository_root=root
        )
        for project_id in sorted(raw_projects)
    }
    for project_id in sorted(projects):
        _validate_product_contract_shape(
            _object(raw_projects[project_id], f"production project {project_id}"),
            parsed=projects[project_id],
            expected_target_count=expected_counts[project_id][0],
            expected_test_only_count=expected_counts[project_id][1],
        )
    _configure_generated_input_contracts(outer, repository_root=root)
    _configure_executed_fixture_contracts(outer, repository_root=root)
    _utility_target_contracts(outer, repository_root=root)
    auxiliary = _auxiliary_install_contracts(outer)
    binding = _binding_build_contract(
        outer,
        repository_root=root,
        reify_external_headers=reify_external_headers,
    )
    expected_binding_lanes = {
        "bindings_release": ("Release", "none"),
        "bindings_selector": ("Release", "none"),
        "bindings_asan_ubsan": ("RelWithDebInfo", "address+undefined"),
        "bindings_tsan": ("RelWithDebInfo", "thread"),
    }
    if binding.lane_configurations != expected_binding_lanes:
        _fail("binding lane configuration catalog drifted")
    _validate_configured_target_graph_contract(
        outer, projects=projects, auxiliary=auxiliary, binding=binding
    )
    _cross_profile_command_contract(
        outer,
        lane_id="core_release",
        project_id="manyforge_core",
        configuration="Release",
    )

    raw_partitions = _object(
        outer.get("native_lane_target_partitions"),
        "native_lane_target_partitions",
    )
    if set(raw_partitions) != {"schema_version", "lanes"} or raw_partitions.get(
        "schema_version"
    ) != NATIVE_LANE_TARGET_PARTITION_SCHEMA:
        _fail("native lane target partition shape/schema drifted")
    lanes = _object(raw_partitions.get("lanes"), "native target partition lanes")
    lane_projects = {
        "c02_tail_focused_release": "manyforge_deployment",
        "core_asan_ubsan": "manyforge_core",
        "core_release": "manyforge_core",
        "core_tsan": "manyforge_core",
        "deployment_asan_ubsan": "manyforge_deployment",
        "deployment_release": "manyforge_deployment",
        "roboplan_asan_ubsan": "manyforge_roboplan",
        "roboplan_consumer_focused": "manyforge_roboplan",
        "roboplan_release": "manyforge_roboplan",
        "roboplan_tsan": "manyforge_roboplan",
    }
    if set(lanes) != set(lane_projects):
        _fail("native lane target partition lane set drifted")
    decoded_partitions = {
        lane_id: _native_lane_partition(
            outer, lane_id=lane_id, contract=projects[project_id]
        )
        for lane_id, project_id in lane_projects.items()
    }
    core_production = frozenset(projects["manyforge_core"].targets)
    core_test = frozenset(projects["manyforge_core"].test_only_targets)
    deployment_production = frozenset(projects["manyforge_deployment"].targets)
    roboplan_production = frozenset(projects["manyforge_roboplan"].targets)
    expected_partitions = {
        "core_release": (core_production, core_test),
        "core_asan_ubsan": (core_production, core_test),
        "core_tsan": (core_production, core_test),
        "deployment_release": (deployment_production, frozenset()),
        "deployment_asan_ubsan": (deployment_production, frozenset()),
        "c02_tail_focused_release": (
            frozenset(
                {
                    "manyforge_content_identity",
                    "manyforge_deployment",
                    "manyforge_deployment_verification_core",
                }
            ),
            frozenset(),
        ),
        "roboplan_release": (
            roboplan_production,
            frozenset(
                {
                    "generate_canonical_empty_product_package",
                    "generate_lite6_retained_consumer_fixture",
                    "generate_lite6_retained_consumer_product",
                    "manyforge_build_qualified_admission_profile",
                    "manyforge_deployment",
                    "manyforge_deployment_admission",
                    "manyforge_deployment_canonical_package_generator",
                    "manyforge_deployment_verification_core",
                    "manyforge_emit_build_qualified_admission_profile",
                    "manyforge_generate_canonical_empty_package",
                    "manyforge_roboplan_planning_contracts",
                    "manyforge_roboplan_planning_evidence",
                }
            ),
        ),
        "roboplan_consumer_focused": (
            roboplan_production,
            frozenset(projects["manyforge_roboplan"].test_only_targets)
            - {"test_retained_product_consumer"},
        ),
        "roboplan_asan_ubsan": (frozenset(), frozenset()),
        "roboplan_tsan": (frozenset(), frozenset()),
    }
    for lane_id, partition in decoded_partitions.items():
        expected = expected_partitions[lane_id]
        if (partition.production_targets, partition.test_only_targets) != expected:
            _fail(f"native lane target partition {lane_id} membership drifted")

    parity_rows = _list(
        outer.get("production_release_parity_lanes"),
        "production Release parity lanes",
    )
    if parity_rows != [
        {
            "production_lane_id": "core_production_release",
            "project_id": "manyforge_core",
            "test_enabled_release_lane_id": "core_release",
        },
        {
            "production_lane_id": "deployment_production_release",
            "project_id": "manyforge_deployment",
            "test_enabled_release_lane_id": "deployment_release",
        },
    ]:
        _fail("production Release parity lane contract drifted")

    cross = _object(
        outer.get("cross_profile_parity_contract"), "cross-profile parity contract"
    )
    comparisons = _list(cross.get("comparisons"), "cross-profile comparisons")

    def owner_rows(owners: Iterable[tuple[str, str]]) -> list[dict[str, str]]:
        return [
            {"project_id": project_id, "target": target}
            for project_id, target in sorted(owners)
        ]

    expected_comparisons = [
        {
            "comparison_id": "core_release_asan_tsan",
            "kind": "native_cross_profile",
            "lane_ids": ["core_release", "core_asan_ubsan", "core_tsan"],
            "target_owners": owner_rows(
                ("manyforge_core", target) for target in projects["manyforge_core"].targets
            ),
        },
        {
            "comparison_id": "deployment_release_asan",
            "kind": "native_cross_profile",
            "lane_ids": ["deployment_release", "deployment_asan_ubsan"],
            "target_owners": owner_rows(
                ("manyforge_deployment", target)
                for target in projects["manyforge_deployment"].targets
            ),
        },
        {
            "comparison_id": "binding_release_asan_tsan",
            "kind": "binding_cross_profile",
            "lane_ids": ["bindings_release", "bindings_asan_ubsan", "bindings_tsan"],
            "target_owners": owner_rows(
                {
                    *( ("manyforge_core", target) for target in projects["manyforge_core"].targets ),
                    *( ("manyforge_deployment", target) for target in projects["manyforge_deployment"].targets ),
                    ("manyforge_core_py", "manyforge_core_py"),
                }
            ),
        },
        {
            "comparison_id": "roboplan_scene_asan_tsan",
            "kind": "native_cross_profile",
            "lane_ids": ["roboplan_asan_ubsan", "roboplan_tsan"],
            "target_owners": owner_rows(
                {
                    ("manyforge_roboplan", "test_roboplan_scene_generation"),
                    ("roboplan", "filters"),
                    ("roboplan", "roboplan"),
                    ("roboplan_example_models", "roboplan_example_models"),
                }
            ),
        },
        {
            "comparison_id": "roboplan_release_focused",
            "kind": "native_same_profile_exact",
            "lane_ids": ["roboplan_release", "roboplan_consumer_focused"],
            "target_owners": owner_rows(
                ("manyforge_roboplan", target)
                for target in projects["manyforge_roboplan"].targets
            ),
        },
    ]
    if comparisons != expected_comparisons:
        _fail("cross-profile comparison catalog/owner joins drifted")
    return {
        "project_target_owners": {
            project_id: sorted(project.targets)
            for project_id, project in projects.items()
        },
        "binding_linked_archive_owners": sorted(binding.module_linked_archive_owners),
        "native_lane_ids": sorted(decoded_partitions),
        "cross_profile_comparison_ids": [
            row["comparison_id"] for row in expected_comparisons
        ],
    }


def replay_configured_target_graph(
    repository_root: str | os.PathLike[str],
    output_root: str | os.PathLike[str],
    *,
    project_id: str,
    build_root: str | os.PathLike[str],
    build_testing: bool,
    build_contract: Mapping[str, Any],
) -> dict[str, Any]:
    """Reparse and contract-check one retained CMake configured-target graph."""

    roots = _Roots(
        repository=_directory(Path(repository_root), "graph repository root"),
        output=_directory(Path(output_root), "graph qualification output root"),
    )
    physical_build = _directory(Path(build_root), f"{project_id} graph build root")
    if not _is_within(physical_build, roots.output):
        _fail(f"{project_id} graph build root escapes qualification output")
    if not isinstance(build_testing, bool):
        _fail(f"{project_id} graph build-testing value is not boolean")
    return _target_graph_evidence(
        project_id=_text(project_id, "graph project_id"),
        build_testing=build_testing,
        build_root=physical_build,
        roots=roots,
        build_contract=_object(build_contract, "graph build contract"),
    )


def _material_target_names(build: _Build) -> set[str]:
    return {
        name
        for name, target in build.targets.items()
        if target.target_type in _MATERIAL_TARGET_TYPES and target.artifacts
    }


def _one_query(
    build: _Build,
    node: str,
    roots: _Roots,
    label: str,
) -> tuple[dict[str, Any], list[tuple[str, Path]]]:
    text = _run_text(
        ["/usr/bin/ninja", "-C", str(build.root), "-t", "query", node],
        build.root,
        label,
    )
    rule, raw_dependencies = _parse_query(text, node, label)
    dependencies = [
        (
            edge_type,
            roots.decode(value, build.root, f"{label} dependency"),
        )
        for edge_type, value in raw_dependencies
    ]
    return (
        {
            "node": node,
            "rule": rule,
            "edges": [
                {"type": edge_type, "path": roots.logical(path)}
                for edge_type, path in dependencies
            ],
            "raw_sha256": _sha256_bytes(
                roots.normalize_tool_output(text).encode("utf-8")
            ),
        },
        dependencies,
    )


def _exact_custom_command_output_edge(
    *,
    build: _Build,
    expected_outputs: set[Path],
    roots: _Roots,
    label: str,
) -> dict[str, Any]:
    """Prove that all declared outputs share one exact Ninja CUSTOM_COMMAND edge."""

    build_ninja = _regular_file(
        build.root / "build.ninja", f"{label} build.ninja", contained_by=build.root
    )
    raw = _read_bytes(build_ninja, f"{label} build.ninja")
    try:
        physical_lines = raw.decode("utf-8").splitlines()
    except UnicodeDecodeError as error:
        _fail(f"{label} build.ninja is not UTF-8: {error}")
    logical_lines: list[str] = []
    pending = ""
    for line in physical_lines:
        pending += line
        if pending.endswith("$"):
            pending = pending[:-1]
            continue
        logical_lines.append(pending)
        pending = ""
    if pending:
        _fail(f"{label} build.ninja has a dangling continuation")
    expected = {path.as_posix() for path in expected_outputs}
    matches: list[tuple[str, set[str], set[str], str]] = []
    for line in logical_lines:
        if not line.startswith("build "):
            continue
        head, separator, tail = line.partition(":")
        if not separator:
            _fail(f"{label} build.ninja has a malformed build edge")
        output_spec = head.removeprefix("build ")
        explicit_spec, implicit_separator, implicit_spec = output_spec.partition(" | ")
        if "$" in explicit_spec:
            _fail(f"{label} build.ninja uses unsupported escaped explicit outputs")
        explicit = set(explicit_spec.split())
        if not expected.intersection(explicit):
            continue
        implicit = set(implicit_spec.split()) if implicit_separator else set()
        rule = tail.strip().split(maxsplit=1)[0] if tail.strip() else ""
        matches.append((line, explicit, implicit, rule))
    if len(matches) != 1:
        _fail(f"{label} outputs do not resolve to one Ninja edge")
    line, explicit, implicit, rule = matches[0]
    expected_implicit = {f"${{cmake_ninja_workdir}}{path}" for path in expected}
    if rule != "CUSTOM_COMMAND" or explicit != expected or implicit != expected_implicit:
        _fail(f"{label} CUSTOM_COMMAND output edge membership drifted")
    return {
        "explicit_outputs": sorted(expected),
        "implicit_outputs": sorted(expected_implicit),
        "rule": rule,
        "edge_sha256": _sha256_bytes(
            roots.normalize_tool_output(line).encode("utf-8")
        ),
        "build_ninja": {
            "path": roots.logical(build_ninja),
            "sha256": _sha256_bytes(raw),
        },
    }


def _parity_path_alias_catalog(
    *,
    builds: Sequence[_Build],
    install_roots: Mapping[str, Path],
    roots: _Roots,
) -> tuple[tuple[str, str], ...]:
    """Create exact lane-local aliases while preserving every path suffix."""

    aliases: dict[str, str] = {}

    def add(path: Path, alias: str, label: str) -> None:
        logical = roots.logical(path)
        if not logical.startswith("/qualification-output/"):
            _fail(f"{label} is not lane-local qualification output")
        previous = aliases.setdefault(logical, alias)
        if previous != alias:
            _fail(f"{label} has conflicting parity aliases")

    project_ids = [build.project_id for build in builds]
    if len(project_ids) != len(set(project_ids)):
        _fail("parity path-alias catalog repeats a build project")
    for build in builds:
        add(build.root, f"<build:{build.project_id}>", "parity build root")
        if _is_within(build.source_root, roots.output):
            add(
                build.source_root,
                f"<source:{build.project_id}>",
                "parity source root",
            )
    for project_id in sorted(install_roots):
        add(
            install_roots[project_id],
            f"<install:{project_id}>",
            "parity install root",
        )
    result = tuple(
        sorted(aliases.items(), key=lambda row: (-len(row[0]), row[0], row[1]))
    )
    if not result:
        _fail("parity path-alias catalog is empty")
    return result


def _assign_parity_path_aliases(
    *,
    builds: Sequence[_Build],
    install_roots: Mapping[str, Path],
    roots: _Roots,
) -> tuple[tuple[str, str], ...]:
    aliases = _parity_path_alias_catalog(
        builds=builds, install_roots=install_roots, roots=roots
    )
    for build in builds:
        build.parity_path_aliases = aliases
    return aliases


def _normalize_parity_text(
    value: str,
    *,
    aliases: Sequence[tuple[str, str]],
    roots: _Roots,
    label: str,
) -> str:
    normalized = roots.normalize_tool_output(value)
    for prefix, alias in aliases:
        normalized = re.sub(
            re.escape(prefix) + r"(?=$|[/,:;])",
            alias,
            normalized,
        )
    if "/qualification-output" in normalized:
        _fail(f"{label} retains an unaliased qualification-output path")
    return normalized


def _parity_argv_sha256(
    argv: Sequence[str], build: _Build, roots: _Roots
) -> str:
    if not build.parity_path_aliases:
        _fail(f"{build.project_id} parity command lacks its exact path-alias catalog")
    return _canonical_sha256(
        [
            _normalize_parity_text(
                token,
                aliases=build.parity_path_aliases,
                roots=roots,
                label=f"{build.project_id} parity command",
            )
            for token in argv
        ]
    )


def _cross_profile_argv_sha256(
    argv: Sequence[str],
    build: _Build,
    roots: _Roots,
    profile: str,
    command_kind: str,
) -> str:
    """Remove only exact, occurrence-pinned profile deltas from one command."""

    if profile not in {"none", "address+undefined", "thread"}:
        _fail(f"unknown cross-profile sanitizer {profile}")
    if command_kind not in _CROSS_PROFILE_COMMAND_KINDS:
        _fail(f"unknown cross-profile command kind {command_kind}")
    expected = build.cross_profile_command_deltas.get(command_kind)
    if expected is None:
        _fail(f"{build.project_id} lacks a {command_kind} parity-delta contract")
    observed = Counter(argv)
    for token in sorted(build.cross_profile_known_delta_tokens):
        if observed[token] != expected.get(token, 0):
            _fail(
                f"{build.lane_id} {build.project_id} {command_kind} profile token "
                f"{token} count drifted: expected={expected.get(token, 0)}, "
                f"actual={observed[token]}"
            )
    retained = [token for token in argv if token not in expected]
    return _parity_argv_sha256(retained, build, roots)


def _exact_bounded_tree_inventory(
    root: Path,
    *,
    roots: _Roots,
    label: str,
) -> dict[str, Any]:
    """Hash one complete generated tree while rejecting aliases and special files."""

    root = _directory(root, label)
    rows: list[dict[str, Any]] = []
    pending = [root]
    total_bytes = 0
    while pending:
        directory = pending.pop()
        try:
            with os.scandir(directory) as iterator:
                children = sorted(iterator, key=lambda entry: entry.name)
        except OSError as error:
            _fail(f"{label} cannot be enumerated: {error}")
        for child in children:
            path = Path(child.path)
            try:
                metadata = path.lstat()
            except OSError as error:
                _fail(f"{label} entry cannot be inspected: {error}")
            relative = path.relative_to(root).as_posix()
            if len(rows) >= 20_000 or len(PurePosixPath(relative).parts) > 32:
                _fail(f"{label} exceeds its entry/depth bound")
            if stat.S_ISLNK(metadata.st_mode):
                _fail(f"{label} contains a symlink: {relative}")
            if stat.S_ISDIR(metadata.st_mode):
                rows.append({"relative_path": relative, "kind": "directory"})
                pending.append(path)
                continue
            if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1:
                _fail(f"{label} contains a special or hardlinked entry: {relative}")
            total_bytes += metadata.st_size
            if total_bytes > 256 * 1024 * 1024:
                _fail(f"{label} exceeds its total-byte bound")
            file_path = _regular_file(path, f"{label} file {relative}", contained_by=root)
            rows.append(
                {
                    "relative_path": relative,
                    "kind": "file",
                    "size": metadata.st_size,
                    "mode": f"{stat.S_IMODE(metadata.st_mode):04o}",
                    "sha256": _sha256_file(file_path),
                }
            )
    if not rows:
        _fail(f"{label} is empty")
    rows.sort(key=lambda row: (row["relative_path"], row["kind"]))
    return {
        "root": roots.logical(root),
        "entry_count": len(rows),
        "total_file_bytes": total_bytes,
        "entries_sha256": _canonical_sha256(rows),
        "entries": rows,
    }


def _remove_authenticated_tree(
    root: Path,
    inventory: Mapping[str, Any],
    *,
    roots: _Roots,
    label: str,
) -> None:
    """Remove only an unchanged tree that this verifier just authenticated."""

    expected = _object(inventory, f"{label} inventory")
    directory_flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
    parent_descriptor = -1
    root_descriptor = -1

    def same_identity(left: os.stat_result, right: os.stat_result) -> bool:
        return left.st_dev == right.st_dev and left.st_ino == right.st_ino

    def open_parent(parts: tuple[str, ...]) -> int:
        descriptor = os.dup(root_descriptor)
        try:
            for part in parts:
                child = os.open(
                    part,
                    directory_flags,
                    dir_fd=descriptor,
                )
                os.close(descriptor)
                descriptor = child
            return descriptor
        except OSError:
            os.close(descriptor)
            raise

    try:
        parent_descriptor = os.open(root.parent, directory_flags)
        root_descriptor = os.open(
            root.name,
            directory_flags,
            dir_fd=parent_descriptor,
        )
        root_metadata = os.fstat(root_descriptor)
        observed = _exact_bounded_tree_inventory(root, roots=roots, label=label)
        current_root_metadata = os.stat(
            root.name,
            dir_fd=parent_descriptor,
            follow_symlinks=False,
        )
        if (
            not stat.S_ISDIR(current_root_metadata.st_mode)
            or not same_identity(root_metadata, current_root_metadata)
            or observed != expected
        ):
            _fail(f"{label} changed before authenticated cleanup")
        rows = _list(observed.get("entries"), f"{label} entries")
        ordered = sorted(
            (_object(value, f"{label} entry") for value in rows),
            key=lambda value: (
                len(PurePosixPath(_text(value.get("relative_path"), label)).parts),
                value.get("kind") == "file",
            ),
            reverse=True,
        )
        for row in ordered:
            relative = _safe_relative(row.get("relative_path"), f"{label} path")
            parts = relative.parts
            entry_parent = open_parent(parts[:-1])
            try:
                name = parts[-1]
                metadata = os.stat(
                    name,
                    dir_fd=entry_parent,
                    follow_symlinks=False,
                )
                kind = row.get("kind")
                if kind == "file":
                    if (
                        not stat.S_ISREG(metadata.st_mode)
                        or metadata.st_nlink != 1
                    ):
                        _fail(f"{label} file changed before authenticated cleanup")
                    file_descriptor = os.open(
                        name,
                        os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK,
                        dir_fd=entry_parent,
                    )
                    try:
                        opened_metadata = os.fstat(file_descriptor)
                        if (
                            not stat.S_ISREG(opened_metadata.st_mode)
                            or opened_metadata.st_nlink != 1
                            or not same_identity(metadata, opened_metadata)
                        ):
                            _fail(
                                f"{label} file changed before authenticated cleanup"
                            )
                        digest = hashlib.sha256()
                        for chunk in iter(
                            lambda: os.read(file_descriptor, 1024 * 1024),
                            b"",
                        ):
                            digest.update(chunk)
                    finally:
                        os.close(file_descriptor)
                    current_metadata = os.stat(
                        name,
                        dir_fd=entry_parent,
                        follow_symlinks=False,
                    )
                    if (
                        not same_identity(metadata, opened_metadata)
                        or not same_identity(opened_metadata, current_metadata)
                        or opened_metadata.st_nlink != 1
                        or current_metadata.st_nlink != 1
                        or current_metadata.st_size != row.get("size")
                        or f"{stat.S_IMODE(current_metadata.st_mode):04o}"
                        != row.get("mode")
                        or digest.hexdigest() != row.get("sha256")
                    ):
                        _fail(f"{label} file changed before authenticated cleanup")
                    os.unlink(name, dir_fd=entry_parent)
                elif kind == "directory":
                    if not stat.S_ISDIR(metadata.st_mode):
                        _fail(
                            f"{label} directory changed before authenticated cleanup"
                        )
                    child_descriptor = os.open(
                        name,
                        directory_flags,
                        dir_fd=entry_parent,
                    )
                    try:
                        opened_metadata = os.fstat(child_descriptor)
                        if os.listdir(child_descriptor):
                            _fail(
                                f"{label} directory changed before authenticated cleanup"
                            )
                    finally:
                        os.close(child_descriptor)
                    current_metadata = os.stat(
                        name,
                        dir_fd=entry_parent,
                        follow_symlinks=False,
                    )
                    if (
                        not same_identity(metadata, opened_metadata)
                        or not same_identity(opened_metadata, current_metadata)
                    ):
                        _fail(
                            f"{label} directory changed before authenticated cleanup"
                        )
                    os.rmdir(name, dir_fd=entry_parent)
                else:
                    _fail(f"{label} contains an unsupported cleanup entry")
            finally:
                os.close(entry_parent)
        current_root_metadata = os.stat(
            root.name,
            dir_fd=parent_descriptor,
            follow_symlinks=False,
        )
        if (
            not same_identity(root_metadata, current_root_metadata)
            or os.listdir(root_descriptor)
        ):
            _fail(f"{label} changed before authenticated root cleanup")
        os.rmdir(root.name, dir_fd=parent_descriptor)
    except OSError as error:
        _fail(f"{label} authenticated cleanup failed: {error}")
    finally:
        if root_descriptor >= 0:
            os.close(root_descriptor)
        if parent_descriptor >= 0:
            os.close(parent_descriptor)


def _configure_executed_fixture_evidence(
    *,
    project_id: str,
    build_root: Path,
    build_testing: bool,
    cache: Mapping[str, tuple[str, str]],
    configure_inputs: Mapping[Path, Mapping[str, Any]],
    build_contract: Mapping[str, Any],
    toolchain_contract: Mapping[str, Any],
    roots: _Roots,
    retain_replay: bool,
) -> tuple[list[dict[str, Any]], dict[Path, dict[str, Any]]]:
    contracts = _configure_executed_fixture_contracts(
        build_contract,
        repository_root=roots.repository,
    )
    selected = sorted(
        (
            contract
            for (owner, _output), contract in contracts.items()
            if owner == project_id
        ),
        key=lambda contract: contract.output_root.as_posix(),
    )
    if not selected:
        return [], {}
    if not build_testing:
        return [], {}
    if any(contract.required_build_testing is not True for contract in selected):
        _fail(f"{project_id} configure-executed fixture testing policy drifted")
    if not isinstance(retain_replay, bool):
        _fail(f"{project_id} configure fixture replay-retention policy drifted")
    python_contract = _object(
        toolchain_contract.get("python"), f"{project_id} Python contract"
    )
    python_path = _text(
        python_contract.get("lexical_path"), f"{project_id} Python path"
    )
    if _cache_value(cache, "_Python3_EXECUTABLE", project_id) != python_path:
        _fail(f"{project_id} configure fixture did not use the contracted Python")

    replay_parent = build_root / "i2-configure-fixture-replay"
    if replay_parent.exists() or replay_parent.is_symlink():
        _fail(f"{project_id} configure fixture replay root was not fresh")
    try:
        replay_parent.mkdir(mode=0o755)
    except OSError as error:
        _fail(f"{project_id} configure fixture replay root cannot be created: {error}")

    evidence_rows: list[dict[str, Any]] = []
    output_index: dict[Path, dict[str, Any]] = {}
    expected_parent_entries: dict[str, dict[str, Any]] = {}
    for contract_index, contract in enumerate(selected, start=1):
        actual_root = build_root / contract.output_root
        actual_inventory = _exact_bounded_tree_inventory(
            actual_root,
            roots=roots,
            label=(
                f"{project_id} configure-executed fixture "
                f"{contract.output_root.as_posix()}"
            ),
        )
        actual_files = {
            row["relative_path"]
            for row in actual_inventory["entries"]
            if row["kind"] == "file"
        }
        if actual_files != {path.as_posix() for path in contract.output_paths} or any(
            row["kind"] != "file" for row in actual_inventory["entries"]
        ):
            _fail(
                f"{project_id} configure-executed fixture output tree drifted"
            )

        required_input_rows: list[dict[str, Any]] = []
        for required in (
            contract.cmake,
            contract.tool,
            contract.urdf,
            contract.srdf,
            *contract.resources,
        ):
            row = configure_inputs.get(Path(os.path.normpath(required)))
            if row is None:
                _fail(
                    f"{project_id} configure-executed fixture lacks authenticated input "
                    f"{roots.logical(required)}"
                )
            identity = _object(
                row.get("identity"), f"{project_id} configure fixture input identity"
            )
            if identity.get("kind") != "git_tracked_source":
                _fail(f"{project_id} configure fixture input is not Git tracked")
            required_input_rows.append(dict(row))
        required_input_rows.sort(key=lambda row: row["path"])

        replay_root = replay_parent / contract.output_root
        try:
            replay_root.mkdir(mode=0o755, parents=True)
        except OSError as error:
            _fail(f"{project_id} configure fixture replay cannot be created: {error}")
        environment_root = replay_parent / f"environment-{contract_index}"
        home = environment_root / "home"
        temporary = environment_root / "tmp"
        cache_root = environment_root / "cache"
        try:
            home.mkdir(mode=0o755, parents=True)
            temporary.mkdir(mode=0o755)
            cache_root.mkdir(mode=0o755)
        except OSError as error:
            _fail(
                f"{project_id} configure fixture environment cannot be created: {error}"
            )
        argv = [
            python_path,
            str(contract.tool),
            "--urdf",
            str(contract.urdf),
            "--srdf",
            str(contract.srdf),
            "--model-asset-id",
            contract.model_asset_id,
            "--accepted-package-uri-prefix",
            contract.accepted_package_uri_prefix,
            "--resource-root",
            str(contract.resource_root),
            "--output-directory",
            str(replay_root),
        ]
        replay_stdout = _run_text(
            argv,
            build_root,
            f"{project_id} configure fixture exact replay",
            env={
                "HOME": str(home),
                "LANG": "C.UTF-8",
                "LC_ALL": "C.UTF-8",
                "PATH": "/usr/local/bin:/usr/bin:/bin",
                "PYTHONDONTWRITEBYTECODE": "1",
                "PYTHONHASHSEED": "0",
                "PYTHONNOUSERSITE": "1",
                "TMPDIR": str(temporary),
                "XDG_CACHE_HOME": str(cache_root),
            },
        )
        try:
            environment_entries = [
                child
                for root in (home, temporary, cache_root)
                for child in root.iterdir()
            ]
        except OSError as error:
            _fail(f"{project_id} configure fixture environment cannot be read: {error}")
        if environment_entries:
            _fail(f"{project_id} configure fixture replay wrote ambient environment state")
        try:
            for empty_directory in (cache_root, temporary, home, environment_root):
                empty_directory.rmdir()
        except OSError as error:
            _fail(f"{project_id} configure fixture environment cleanup failed: {error}")
        replay_inventory = _exact_bounded_tree_inventory(
            replay_root,
            roots=roots,
            label=f"{project_id} configure fixture replay output",
        )
        if actual_inventory["entries"] != replay_inventory["entries"]:
            _fail(f"{project_id} configure fixture replay bytes differ")
        parent = Path()
        for part in contract.output_root.parts:
            parent /= part
            expected_parent_entries.setdefault(
                parent.as_posix(),
                {"relative_path": parent.as_posix(), "kind": "directory"},
            )
        for raw_entry in replay_inventory["entries"]:
            entry = dict(_object(raw_entry, f"{project_id} replay entry"))
            relative = contract.output_root / _safe_relative(
                entry.get("relative_path"), f"{project_id} replay entry path"
            )
            entry["relative_path"] = relative.as_posix()
            if relative.as_posix() in expected_parent_entries:
                _fail(f"{project_id} configure fixture replay paths overlap")
            expected_parent_entries[relative.as_posix()] = entry
        stable_argv = [*argv[:-1], "<fresh-replay-output-directory>"]
        fixture_identity = {
            "kind": "cmake_execute_process_replayed_fixture",
            "owner_project_id": project_id,
            "output_root": roots.logical(actual_root),
            "actual_tree_sha256": actual_inventory["entries_sha256"],
            "replay_tree_sha256": replay_inventory["entries_sha256"],
            "required_inputs_sha256": _canonical_sha256(required_input_rows),
            "argv_sha256": _canonical_sha256(argv),
            "cross_profile_argv_sha256": _canonical_sha256(stable_argv),
        }
        for relative in contract.output_paths:
            output = _regular_file(
                actual_root / relative,
                f"{project_id} configure fixture output {relative.as_posix()}",
                contained_by=actual_root,
            )
            output_index[output] = {
                **fixture_identity,
                "path": roots.logical(output),
                "sha256": _sha256_file(output),
            }
        evidence_rows.append(
            {
                **fixture_identity,
                "cmake": roots.logical(contract.cmake),
                "tool": roots.logical(contract.tool),
                "interpreter": {
                    "path": python_path,
                    "resolved_path": _text(
                        python_contract.get("resolved_path"),
                        f"{project_id} Python resolved path",
                    ),
                    "sha256": _text(
                        python_contract.get("sha256"),
                        f"{project_id} Python digest",
                    ),
                },
                "required_inputs": required_input_rows,
                "actual_output_inventory": actual_inventory,
                "replay_output_inventory": replay_inventory,
                "replay_stdout_sha256": _sha256_bytes(replay_stdout.encode("utf-8")),
            }
        )
    replay_parent_inventory = _exact_bounded_tree_inventory(
        replay_parent,
        roots=roots,
        label=f"{project_id} configure fixture replay root",
    )
    expected_entries = sorted(
        expected_parent_entries.values(),
        key=lambda row: (row["relative_path"], row["kind"]),
    )
    if replay_parent_inventory["entries"] != expected_entries:
        _fail(f"{project_id} configure fixture replay root contains extra state")
    if not retain_replay:
        _remove_authenticated_tree(
            replay_parent,
            replay_parent_inventory,
            roots=roots,
            label=f"{project_id} transient configure fixture replay root",
        )
    return evidence_rows, output_index


def _utility_housekeeping_evidence(
    *,
    commands_text: str,
    producer: _Command,
    contract: _UtilityTargetContract,
    build: _Build,
    roots: _Roots,
) -> list[dict[str, Any]]:
    """Bind the generator to its one exact, bounded CMake housekeeping wrapper."""

    matches: list[tuple[Path, list[tuple[str, ...]]]] = []
    for line_index, line in enumerate(
        line for line in commands_text.splitlines() if line
    ):
        cwd = build.root
        executable_segments: list[tuple[str, ...]] = []
        for segment in _split_shell_line(
            line, f"{build.project_id} utility command line {line_index + 1}"
        ):
            if segment and segment[0] == "cd":
                if len(segment) != 2 or executable_segments:
                    _fail(
                        f"{build.project_id} utility generator uses an unsupported cd wrapper"
                    )
                cwd = roots.decode(
                    segment[1], cwd, f"{build.project_id} utility generator cd"
                )
                if not _is_within(cwd, build.root):
                    _fail(f"{build.project_id} utility generator cd escapes its build")
                continue
            executable_segments.append(tuple(segment))
        if tuple(producer.argv) in executable_segments:
            matches.append((cwd, executable_segments))
    if len(matches) != 1:
        _fail(
            f"{build.project_id} utility {contract.target_name} lacks one exact "
            "generator wrapper"
        )
    cwd, segments = matches[0]
    generator_index = segments.index(tuple(producer.argv))
    if generator_index != len(segments) - 1:
        _fail(
            f"{build.project_id} utility {contract.target_name} mutates output after generation"
        )
    output = Path(os.path.normpath(build.root / contract.generator_output))

    def cmake_path(segment: tuple[str, ...], expected_prefix: tuple[str, ...]) -> Path:
        if len(segment) != len(expected_prefix) + 1 or segment[: len(expected_prefix)] != expected_prefix:
            _fail(
                f"{build.project_id} utility {contract.target_name} housekeeping drifted"
            )
        return Path(
            os.path.normpath(
                roots.decode(
                    segment[-1],
                    cwd,
                    f"{build.project_id} utility {contract.target_name} housekeeping path",
                )
            )
        )

    housekeeping = segments[:generator_index]
    if contract.housekeeping_policy == "make_output_parent":
        if len(housekeeping) != 1 or cmake_path(
            housekeeping[0], ("/usr/bin/cmake", "-E", "make_directory")
        ) != output.parent:
            _fail(
                f"{build.project_id} utility {contract.target_name} parent creation drifted"
            )
    elif contract.housekeeping_policy == "replace_output_root":
        if (
            len(housekeeping) != 2
            or cmake_path(
                housekeeping[0], ("/usr/bin/cmake", "-E", "rm", "-rf")
            )
            != output
            or cmake_path(
                housekeeping[1], ("/usr/bin/cmake", "-E", "make_directory")
            )
            != output
        ):
            _fail(
                f"{build.project_id} utility {contract.target_name} root replacement drifted"
            )
    else:
        _fail(
            f"{build.project_id} utility {contract.target_name} housekeeping policy is unknown"
        )
    return [
        {
            "argv_sha256": _canonical_sha256(list(segment)),
            "argv": list(segment),
        }
        for segment in housekeeping
    ]


def _require_cmake_utility_virtual_sources(
    target: _TargetReply,
    *,
    build: _Build,
    primary_output: Path,
) -> None:
    """Bind pinned CMake metadata rows to one contracted UTILITY output."""

    if (
        not _is_within(target.build_base, build.root)
        or not _is_within(primary_output, build.root)
    ):
        _fail(f"{build.project_id} utility {target.name} path escapes its build")
    utility_stamp = target.build_base / "CMakeFiles" / target.name
    expected = (
        _VirtualSourceReply(
            path=utility_stamp,
            kind="cmake_utility_stamp",
            owner=utility_stamp,
        ),
        _VirtualSourceReply(
            path=Path(f"{utility_stamp}.rule"),
            kind="cmake_rule",
            owner=utility_stamp,
        ),
        _VirtualSourceReply(
            path=Path(f"{primary_output}.rule"),
            kind="cmake_rule",
            owner=primary_output,
        ),
    )
    if target.sources or target.virtual_sources != expected:
        _fail(
            f"{build.project_id} utility {target.name} CMake virtual source set drifted"
        )


def _prove_utility_target(
    *,
    build: _Build,
    spec: _ProductTargetSpec,
    contract: _UtilityTargetContract,
    role: str,
    profile: str,
    roots: _Roots,
    responses: _ResponseCollector,
) -> dict[str, Any]:
    """Prove one generated output closure and its build-owned producer target."""

    if (
        contract.project_id != build.project_id
        or contract.target_name != spec.name
        or contract.primary_output != spec.artifact
    ):
        _fail(f"{build.project_id} utility {spec.name} contract/spec identity drifted")

    target = build.targets[spec.name]
    if target.target_type != "UTILITY" or target.artifacts:
        _fail(
            f"{build.project_id} target {spec.name} is not an artifact-free "
            "File API UTILITY target"
        )
    primary_output = Path(os.path.normpath(build.root / contract.primary_output))
    _require_cmake_utility_virtual_sources(
        target,
        build=build,
        primary_output=primary_output,
    )
    primary_file = _regular_file(
        primary_output,
        f"{build.project_id} target {spec.name} generated artifact",
        contained_by=build.root,
    )
    generator_output = Path(
        os.path.normpath(build.root / contract.generator_output)
    )
    target_query, target_dependencies = _one_query(
        build,
        spec.name,
        roots,
        f"{build.project_id} Ninja query {spec.name}",
    )
    if sum(
        Path(os.path.normpath(path)) == primary_output
        for edge_type, path in target_dependencies
        if edge_type != "order_only"
    ) != 1:
        _fail(
            f"{build.project_id} UTILITY {spec.name} does not depend exactly once "
            "on its contracted generated artifact"
        )
    artifact_node = contract.primary_output.as_posix()
    artifact_query, artifact_dependencies = _one_query(
        build,
        artifact_node,
        roots,
        f"{build.project_id} Ninja query {artifact_node}",
    )
    if artifact_query["rule"] != "CUSTOM_COMMAND":
        _fail(
            f"{build.project_id} generated artifact {artifact_node} does not have "
            "one direct CUSTOM_COMMAND producer"
        )

    commands_text = _run_text(
        ["/usr/bin/ninja", "-C", str(build.root), "-t", "commands", spec.name],
        build.root,
        f"{build.project_id} Ninja commands {spec.name}",
    )
    commands = _parse_ninja_commands(
        commands_text,
        build,
        roots,
        responses,
        f"{build.project_id} target {spec.name}",
    )
    if (
        generator_output not in commands
        or commands[generator_output].kind != "generated"
    ):
        _fail(
            f"{build.project_id} UTILITY {spec.name} lacks one parsed generated-output "
            "producer"
        )
    producer = commands[generator_output]
    if (
        len(producer.inputs) != 1
        or not _is_within(producer.directory, build.root)
        or len(producer.argv) != 3
        or producer.argv[1] != contract.generator_output_option
        or Path(
            os.path.normpath(
                roots.decode(
                    producer.argv[2],
                    build.root,
                    f"{build.project_id} UTILITY {spec.name} generator output",
                )
            )
        )
        != generator_output
    ):
        _fail(
            f"{build.project_id} UTILITY {spec.name} generator argv is not exact"
        )
    producer_artifact = producer.inputs[0]
    owners = [
        owner
        for owner, reply in build.targets.items()
        if reply.target_type == "EXECUTABLE"
        and len(reply.artifacts) == 1
        and Path(os.path.normpath(reply.artifacts[0])) == producer_artifact
    ]
    if len(owners) != 1:
        _fail(
            f"{build.project_id} UTILITY {spec.name} generator does not have exactly "
            "one File API executable owner"
        )
    producer_file = _regular_file(
        producer_artifact,
        f"{build.project_id} UTILITY {spec.name} generator",
        contained_by=build.root,
    )
    if not os.access(producer_file, os.X_OK):
        _fail(f"{build.project_id} UTILITY {spec.name} generator is not executable")

    housekeeping = _utility_housekeeping_evidence(
        commands_text=commands_text,
        producer=producer,
        contract=contract,
        build=build,
        roots=roots,
    )

    output_queries = [artifact_query]
    byproduct_files: list[Path] = []
    for relative in contract.declared_byproducts:
        byproduct = Path(os.path.normpath(build.root / relative))
        byproduct_files.append(
            _regular_file(
                byproduct,
                f"{build.project_id} utility {spec.name} declared byproduct",
                contained_by=build.root,
            )
        )
        byproduct_query, byproduct_dependencies = _one_query(
            build,
            relative.as_posix(),
            roots,
            f"{build.project_id} Ninja query {relative.as_posix()}",
        )
        if (
            byproduct_query["rule"] != "CUSTOM_COMMAND"
            or byproduct_dependencies != artifact_dependencies
        ):
            _fail(
                f"{build.project_id} utility {spec.name} byproduct producer edge drifted"
            )
        output_queries.append(byproduct_query)

    custom_command_output_edge = _exact_custom_command_output_edge(
        build=build,
        expected_outputs={contract.primary_output, *contract.declared_byproducts},
        roots=roots,
        label=f"{build.project_id} utility {spec.name}",
    )

    if contract.output_inventory_policy == "single_file":
        output_inventory = {
            "policy": "single_file",
            "root": roots.logical(primary_file.parent),
            "entry_count": 1,
            "total_file_bytes": primary_file.stat().st_size,
            "entries": [
                {
                    "relative_path": primary_file.name,
                    "kind": "file",
                    "size": primary_file.stat().st_size,
                    "mode": f"{stat.S_IMODE(primary_file.stat().st_mode):04o}",
                    "sha256": _sha256_file(primary_file),
                }
            ],
        }
        output_inventory["entries_sha256"] = _canonical_sha256(
            output_inventory["entries"]
        )
    elif contract.output_inventory_policy == "exact_bounded_tree":
        output_inventory = _exact_bounded_tree_inventory(
            generator_output,
            roots=roots,
            label=f"{build.project_id} utility {spec.name} generated output tree",
        )
        output_inventory["policy"] = "exact_bounded_tree"
        inventory_files = {
            Path(os.path.normpath(generator_output / row["relative_path"]))
            for row in output_inventory["entries"]
            if row["kind"] == "file"
        }
        required_files = {primary_file, *byproduct_files}
        if not required_files.issubset(inventory_files):
            _fail(
                f"{build.project_id} utility {spec.name} output tree omits a declared output"
            )
    else:
        _fail(f"{build.project_id} utility {spec.name} output policy is unknown")

    graph = _object(
        build.evidence.get("configured_target_graph"),
        f"{build.project_id} configured target graph",
    )
    graph_edges = _list(graph.get("edges"), f"{build.project_id} target graph edges")
    consumer_relationships: list[dict[str, str]] = []
    for consumer_target, relationship_kind in contract.consumer_relationships:
        row = {"target": consumer_target, "kind": relationship_kind}
        if relationship_kind == "target_dependency":
            matches = [
                edge
                for raw_edge in graph_edges
                if (
                    (edge := _object(raw_edge, "utility graph relationship")).get(
                        "source"
                    )
                    == consumer_target
                    and edge.get("target") == spec.name
                )
            ]
            if len(matches) != 1 or matches[0].get("style") != "solid":
                _fail(
                    f"{build.project_id} utility {spec.name} lacks its exact CMake "
                    f"target dependency from {consumer_target}"
                )
            row["graph_style"] = "solid"
        consumer_relationships.append(row)

    dependency_rows: list[dict[str, Any]] = []
    for edge_type, path in sorted(
        artifact_dependencies,
        key=lambda row: (row[0], roots.logical(row[1])),
    ):
        normalized = Path(os.path.normpath(path))
        if normalized == producer_artifact:
            dependency_rows.append(
                {
                    "type": edge_type,
                    "path": roots.logical(normalized),
                    "sha256": _sha256_file(producer_file),
                    "producer_target": owners[0],
                }
            )
            continue
        configure_owner = build.configure_executed_inputs.get(normalized)
        if configure_owner is not None:
            dependency_file = _regular_file(
                normalized,
                f"{build.project_id} UTILITY {spec.name} configure fixture dependency",
                contained_by=build.root,
            )
            if configure_owner.get("sha256") != _sha256_file(dependency_file):
                _fail(
                    f"{build.project_id} UTILITY {spec.name} configure fixture changed"
                )
            dependency_rows.append(
                {
                    "type": edge_type,
                    "path": roots.logical(normalized),
                    "sha256": _sha256_file(dependency_file),
                    "producer_target": None,
                    "configure_execution_owner": dict(configure_owner),
                }
            )
            continue
        if not normalized.exists():
            _fail(
                f"{build.project_id} UTILITY {spec.name} has an unauthenticated "
                f"generated-input dependency: {roots.logical(normalized)}"
            )
        dependency_file = _regular_file(
            normalized,
            f"{build.project_id} UTILITY {spec.name} dependency",
        )
        if _is_within(dependency_file, roots.repository):
            source_identity: dict[str, Any] | None = _git_tracked_file_identity(
                dependency_file,
                roots.repository,
                f"{build.project_id} UTILITY {spec.name} repository dependency",
            )
        elif _is_within(dependency_file, build.root):
            if normalized not in commands:
                _fail(
                    f"{build.project_id} UTILITY {spec.name} build dependency has no "
                    "authenticated producer"
                )
            source_identity = None
        else:
            _fail(
                f"{build.project_id} UTILITY {spec.name} dependency escapes source/build roots"
            )
        dependency_rows.append(
            {
                "type": edge_type,
                "path": roots.logical(normalized),
                "sha256": _sha256_file(dependency_file),
                "producer_target": None,
                "source_identity": source_identity,
            }
        )

    normalized_commands = roots.normalize_tool_output(commands_text)
    inventory_root = roots.decode(
        _text(output_inventory["root"], "utility output inventory root"),
        build.root,
        "utility output inventory root",
    )
    generated_paths = sorted(
        roots.logical(inventory_root / row["relative_path"])
        for row in output_inventory["entries"]
        if row["kind"] == "file"
    )
    cross_profile_inventory_entries: list[dict[str, Any]] = []
    for raw_entry in output_inventory["entries"]:
        entry = _object(raw_entry, "utility output inventory entry")
        projected = {
            "relative_path": _text(
                entry.get("relative_path"), "utility output relative path"
            ),
            "kind": _text(entry.get("kind"), "utility output kind"),
        }
        if projected["kind"] == "file":
            projected["mode"] = _text(entry.get("mode"), "utility output mode")
            projected["cross_profile_sha256"] = (
                _generated_output_cross_profile_sha256(
                    _regular_file(
                        inventory_root / projected["relative_path"],
                        "utility cross-profile output",
                        contained_by=inventory_root,
                    )
                )
            )
        elif projected["kind"] != "directory":
            _fail("utility output inventory contains an unknown entry kind")
        cross_profile_inventory_entries.append(projected)
    cross_profile_output_inventory = {
        "entries": cross_profile_inventory_entries,
        "entries_sha256": _canonical_sha256(cross_profile_inventory_entries),
    }
    return {
        "project_id": build.project_id,
        "build_root": roots.logical(build.root),
        "target": spec.name,
        "role": role,
        "target_type": "UTILITY",
        "artifact": {
            "path": roots.logical(primary_output),
            "resolved_path": roots.logical(primary_file),
            "sha256": _sha256_file(primary_file),
        },
        "contract_artifact": spec.artifact.as_posix(),
        "file_api_target_reply": {
            "path": roots.logical(target.reply_path),
            "sha256": target.reply_sha256,
        },
        "file_api_sources": [
            _file_api_source_evidence(
                source,
                build=build,
                roots=roots,
                label=f"File API source for {spec.name}",
            )
            for source in sorted(target.sources, key=roots.logical)
        ],
        "ninja_query": [target_query, *output_queries],
        "custom_command_output_edge": custom_command_output_edge,
        "ninja_commands_sha256": _sha256_bytes(
            normalized_commands.encode("utf-8")
        ),
        "generated_source_producers": [],
        "commands": [
            {
                "kind": "generated",
                "output": roots.logical(generator_output),
                "output_inventory_sha256": output_inventory["entries_sha256"],
                "inputs": [roots.logical(producer_artifact)],
                "source": None,
                "argv_sha256": producer.argv_sha256,
                "parity_argv_sha256": _parity_argv_sha256(
                    producer.argv, build, roots
                ),
                "cross_profile_argv_sha256": _cross_profile_argv_sha256(
                    producer.argv, build, roots, profile, "generated"
                ),
                "sanitizer": None,
            }
        ],
        "archives": [],
        "generator": {
            "project_id": build.project_id,
            "target": owners[0],
            "path": roots.logical(producer_artifact),
            "sha256": _sha256_file(producer_file),
            "sanitizer_profile": profile,
        },
        "generator_dependencies": dependency_rows,
        "housekeeping_commands": housekeeping,
        "output_inventory": output_inventory,
        "cross_profile_output_inventory": cross_profile_output_inventory,
        "declared_byproducts": [
            {
                "path": roots.logical(path),
                "sha256": _sha256_file(path),
            }
            for path in byproduct_files
        ],
        "consumer_relationships": consumer_relationships,
        "transitive_local_artifacts": generated_paths,
        "unresolved_output_dependency_claims": [],
    }


def _prove_spec(
    *,
    build: _Build,
    spec: _ProductTargetSpec,
    role: str,
    profile: str,
    roots: _Roots,
    responses: _ResponseCollector,
    utility_contract: _UtilityTargetContract | None = None,
) -> dict[str, Any]:
    if spec.name not in build.targets or build.targets[spec.name].target_type != spec.target_type:
        _fail(f"{build.project_id} target {spec.name} type differs from its contract")
    if spec.target_type == "UTILITY":
        if utility_contract is None:
            _fail(f"{build.project_id} utility {spec.name} lacks its exact contract")
        return _prove_utility_target(
            build=build,
            spec=spec,
            contract=utility_contract,
            role=role,
            profile=profile,
            roots=roots,
            responses=responses,
        )
    artifact = build.root / spec.artifact
    proof = _prove_target(
        build=build,
        target_name=spec.name,
        artifact=artifact,
        expected_sources=set(spec.required_sources),
        role=role,
        profile=profile,
        roots=roots,
        responses=responses,
    )
    proof["contract_artifact"] = spec.artifact.as_posix()
    generated_rows: list[dict[str, Any]] = []
    if spec.required_generated_sources:
        commands_text = _run_text(
            ["/usr/bin/ninja", "-C", str(build.root), "-t", "commands", spec.name],
            build.root,
            f"{build.project_id} generated-input commands {spec.name}",
        )
        commands = _parse_ninja_commands(
            commands_text,
            build,
            roots,
            responses,
            f"{build.project_id} generated-input target {spec.name}",
        )
        target_sources = {
            Path(os.path.normpath(source)) for source in build.targets[spec.name].sources
        }
        proof_commands = [
            _object(command_value, f"generated-input target command[{command_index}]")
            for command_index, command_value in enumerate(
                _list(proof.get("commands"), "generated-input target commands")
            )
        ]
        artifact_path = _text(
            _object(proof.get("artifact"), "generated-input target artifact").get(
                "path"
            ),
            "generated-input target artifact path",
        )
        final_kind = "archive" if spec.target_type == "STATIC_LIBRARY" else "link"
        final_commands = [
            command
            for command in proof_commands
            if command.get("kind") == final_kind
            and command.get("output") == artifact_path
        ]
        if len(final_commands) != 1:
            _fail(
                f"{build.project_id} target {spec.name} generated-input proof has "
                "no unique final artifact command"
            )
        direct_objects: set[str] = set()
        for input_index, input_value in enumerate(
            _list(
                final_commands[0].get("inputs"),
                "generated-input final command inputs",
            )
        ):
            input_path = _text(
                input_value,
                f"generated-input final command input[{input_index}]",
            )
            if _artifact_kind(Path(input_path)) == "object":
                direct_objects.add(input_path)
        direct_compile_dependencies: dict[str, list[dict[str, Any]]] = {}
        for command_index, command in enumerate(proof_commands):
            if (
                command.get("kind") != "compile"
                or command.get("output") not in direct_objects
            ):
                continue
            closure = _object(
                command.get("dependency_closure"),
                f"generated-input target command[{command_index}] dependency closure",
            )
            for dependency_index, dependency_value in enumerate(
                _list(
                    closure.get("files"),
                    f"generated-input target command[{command_index}] dependency files",
                )
            ):
                dependency = _object(
                    dependency_value,
                    (
                        f"generated-input target command[{command_index}] "
                        f"dependency[{dependency_index}]"
                    ),
                )
                dependency_path = _text(
                    dependency.get("path"),
                    (
                        f"generated-input target command[{command_index}] "
                        f"dependency[{dependency_index}].path"
                    ),
                )
                direct_compile_dependencies.setdefault(dependency_path, []).append(
                    dependency
                )
        for relative in spec.required_generated_sources:
            generated = Path(os.path.normpath(build.root / relative))
            generated_file = _regular_file(
                generated,
                f"{build.project_id} target {spec.name} contracted generated input",
                contained_by=build.root,
            )
            if generated not in target_sources:
                _fail(
                    f"{build.project_id} target {spec.name} generated input is absent "
                    "from its File API source set"
                )
            dependency_rows = direct_compile_dependencies.get(
                roots.logical(generated), []
            )
            if not dependency_rows:
                _fail(
                    f"{build.project_id} target {spec.name} generated input is absent "
                    "from its direct-object compiler dependency closure"
                )
            if generated not in commands or commands[generated].kind != "generated":
                _fail(
                    f"{build.project_id} target {spec.name} generated input lacks an "
                    "authenticated generator command"
                )
            producer = commands[generated]
            if len(producer.inputs) != 1:
                _fail(
                    f"{build.project_id} target {spec.name} generated input has an "
                    "ambiguous producer"
                )
            producer_path = producer.inputs[0]
            owners = [
                owner
                for owner, target in build.targets.items()
                if target.target_type == "EXECUTABLE"
                and len(target.artifacts) == 1
                and Path(os.path.normpath(target.artifacts[0])) == producer_path
            ]
            if len(owners) != 1:
                _fail(
                    f"{build.project_id} target {spec.name} generated input producer "
                    "does not have one File API executable owner"
                )
            producer_file = _regular_file(
                producer_path,
                f"{build.project_id} target {spec.name} generated-input producer",
                contained_by=build.root,
            )
            if not os.access(producer_file, os.X_OK):
                _fail(
                    f"{build.project_id} target {spec.name} generated-input producer "
                    "is not executable"
                )
            generated_digest = _sha256_file(generated_file)
            producer_digest = _sha256_file(producer_file)
            expected_dependency_producer = {
                "kind": "executable_build_generator",
                "project_id": build.project_id,
                "target": owners[0],
                "artifact": roots.logical(producer_path),
                "artifact_sha256": producer_digest,
                "argv_sha256": producer.argv_sha256,
                "parity_argv_sha256": _parity_argv_sha256(
                    producer.argv, build, roots
                ),
            }
            for dependency in dependency_rows:
                dependency_producer = _object(
                    dependency.get("producer"),
                    f"{build.project_id} target {spec.name} generated dependency producer",
                )
                if (
                    dependency.get("sha256") != generated_digest
                    or dependency.get("scope") != "generated_build_input"
                    or any(
                        dependency_producer.get(key) != value
                        for key, value in expected_dependency_producer.items()
                    )
                ):
                    _fail(
                        f"{build.project_id} target {spec.name} generated input "
                        "dependency and generator evidence disagree"
                    )
            generated_rows.append(
                {
                    "path": roots.logical(generated),
                    "sha256": generated_digest,
                    "producer_target": owners[0],
                    "producer_artifact": roots.logical(producer_path),
                    "producer_sha256": producer_digest,
                    "argv_sha256": producer.argv_sha256,
                }
            )
    proof["required_generated_inputs"] = generated_rows
    return proof


def _registered_gtest_spec(
    *,
    project_id: str,
    build_root: Path,
    target_name: str,
    registered_artifact: Path,
    contract: _ProductContract,
    roots: _Roots,
) -> _ProductTargetSpec:
    """Derive one frozen GTest target without duplicating its native contract.

    The native target contract owns the exact GTest target names and the
    ``<package>/test/<target>.cpp`` source rule; the validated CTest registry
    owns the exact executable selected for this lane.  A build-provenance
    test-only row is optional because Core and Deployment intentionally do not
    duplicate their registered GTest catalogs there.  When such a row exists
    (as it does for RoboPlan targets reused by focused lanes), it must agree
    exactly with the derived identity.
    """

    if project_id != contract.project_id:
        _fail(
            f"registered GTest {target_name} selected the wrong project contract"
        )
    if target_name in contract.targets:
        _fail(
            f"{project_id} registered GTest {target_name} collides with a "
            "production target contract"
        )
    artifact = Path(os.path.normpath(registered_artifact))
    normalized_build_root = Path(os.path.normpath(build_root))
    if not _is_within(artifact, normalized_build_root):
        _fail(
            f"{project_id} registered GTest {target_name} artifact escapes its build"
        )
    artifact_relative = _safe_relative(
        artifact.relative_to(normalized_build_root).as_posix(),
        f"{project_id} registered GTest {target_name} artifact",
    )
    source_relative = Path(project_id) / "test" / f"{target_name}.cpp"
    source = _regular_file(
        roots.repository / source_relative,
        f"{project_id} registered GTest {target_name} frozen source",
        contained_by=roots.repository,
    )
    derived = _ProductTargetSpec(
        name=target_name,
        target_type="EXECUTABLE",
        artifact=artifact_relative,
        required_sources=(source,),
        required_generated_sources=(),
    )
    explicit = contract.test_only_targets.get(target_name)
    if explicit is not None and explicit != derived:
        _fail(
            f"{project_id} registered GTest {target_name} explicit target contract "
            "differs from its frozen native identity"
        )
    return derived


def _parity_projection(
    rows: Sequence[Mapping[str, Any]],
    *,
    aliases: Sequence[tuple[str, str]],
    roots: _Roots,
) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    for raw in rows:
        row = _object(raw, "production parity target")
        artifact = _object(row.get("artifact"), "production parity artifact")
        def stable_path(value: str) -> str:
            return _normalize_parity_text(
                value,
                aliases=aliases,
                roots=roots,
                label="production parity path",
            )

        closure_sources: dict[str, str] = {}
        compile_dependencies: dict[str, dict[str, Any]] = {}
        command_semantics: list[dict[str, Any]] = []
        for raw_command in _list(row.get("commands"), "production parity commands"):
            command = _object(raw_command, "production parity command")
            source = command.get("source")
            command_semantics.append(
                {
                    "kind": _text(command.get("kind"), "parity command kind"),
                    "output": stable_path(
                        _text(command.get("output"), "parity command output")
                    ),
                    "argv_sha256": _text(
                        command.get("parity_argv_sha256"),
                        "parity command argv digest",
                    ),
                    "cross_profile_argv_sha256": _text(
                        command.get("cross_profile_argv_sha256"),
                        "cross-profile command argv digest",
                    ),
                }
            )
            dependency_closure = command.get("dependency_closure")
            if dependency_closure is not None:
                dependency_row = _object(
                    dependency_closure, "parity compile dependency closure"
                )
                for raw_dependency in _list(
                    dependency_row.get("files"),
                    "parity compile dependency files",
                ):
                    dependency = _object(
                        raw_dependency, "parity compile dependency"
                    )
                    dependency_path = stable_path(
                        _text(
                            dependency.get("path"),
                            "parity compile dependency path",
                        )
                    )
                    producer_value = dependency.get("producer")
                    producer_kind: str | None = None
                    producer_project: str | None = None
                    producer_target: str | None = None
                    producer_inputs_sha256: str | None = None
                    if producer_value is not None:
                        producer = _object(
                            producer_value,
                            "parity compile dependency producer",
                        )
                        producer_kind = _text(
                            producer.get("kind"),
                            "parity dependency producer kind",
                        )
                        if producer_kind == "executable_build_generator":
                            producer_project = _text(
                                producer.get("project_id"),
                                "parity dependency producer project",
                            )
                            producer_target = _text(
                                producer.get("target"),
                                "parity dependency producer target",
                            )
                        elif producer_kind == "cmake_configure_file":
                            producer_project = _text(
                                producer.get("owner_project_id"),
                                "parity configure owner project",
                            )
                            required_inputs = _list(
                                producer.get("required_inputs"),
                                "parity configure required inputs",
                            )
                            producer_inputs_sha256 = _canonical_sha256(
                                [
                                    {
                                        "path": _text(
                                            _object(item, "parity configure input").get(
                                                "path"
                                            ),
                                            "parity configure input path",
                                        ),
                                        "sha256": _text(
                                            _object(item, "parity configure input").get(
                                                "sha256"
                                            ),
                                            "parity configure input digest",
                                        ),
                                        "identity": _object(
                                            _object(item, "parity configure input").get(
                                                "identity"
                                            ),
                                            "parity configure input identity",
                                        ),
                                    }
                                    for item in required_inputs
                                ]
                            )
                        elif producer_kind == "qualified_install_input":
                            owner = _object(
                                producer.get("owner"),
                                "parity qualified install owner",
                            )
                            producer_project = _text(
                                owner.get("project_id"),
                                "parity qualified install project",
                            )
                            installed_producer = _object(
                                owner.get("producer"),
                                "parity qualified install producer",
                            )
                            producer_inputs_sha256 = _canonical_sha256(
                                {
                                    "installed_sha256": _text(
                                        owner.get("sha256"),
                                        "parity installed input digest",
                                    ),
                                    "producer_kind": _text(
                                        installed_producer.get("kind"),
                                        "parity installed producer kind",
                                    ),
                                    "producer_sha256": _text(
                                        installed_producer.get("sha256"),
                                        "parity installed producer digest",
                                    ),
                                }
                            )
                        else:
                            _fail(
                                f"parity compile dependency has unknown producer kind "
                                f"{producer_kind}"
                            )
                    projected_dependency = {
                        "path": dependency_path,
                        "sha256": _text(
                            dependency.get("cross_profile_sha256"),
                            "parity compile dependency cross-profile digest",
                        ),
                        "scope": _text(
                            dependency.get("scope"),
                            "parity compile dependency scope",
                        ),
                        "producer_kind": producer_kind,
                        "producer_project": producer_project,
                        "producer_target": producer_target,
                        "producer_inputs_sha256": producer_inputs_sha256,
                    }
                    if (
                        dependency_path in compile_dependencies
                        and compile_dependencies[dependency_path]
                        != projected_dependency
                    ):
                        _fail(
                            "compile dependency has conflicting parity identities: "
                            f"{dependency_path}"
                        )
                    compile_dependencies[dependency_path] = projected_dependency
            if source is None:
                continue
            source_row = _object(source, "production parity command source")
            closure_sources[stable_path(_text(source_row.get("path"), "parity source path"))] = _text(
                source_row.get("sha256"), "parity source digest"
            )
        direct_sources = []
        for raw_source in _list(row.get("file_api_sources"), "parity direct sources"):
            source = _object(raw_source, "parity direct source")
            direct_sources.append(
                {
                    "path": stable_path(
                        _text(source.get("path"), "parity direct source path")
                    ),
                    "sha256": _text(
                        source.get("cross_profile_sha256"),
                        "parity direct source cross-profile digest",
                    ),
                }
            )
        result.append(
            {
                "project_id": row["project_id"],
                "target": row["target"],
                "target_type": row["target_type"],
                "artifact_relative": row["contract_artifact"],
                "artifact_sha256": artifact["sha256"],
                "output_inventory_cross_profile_sha256": (
                    _text(
                        _object(
                            row.get("cross_profile_output_inventory"),
                            "utility cross-profile output inventory",
                        ).get("entries_sha256"),
                        "utility cross-profile output inventory digest",
                    )
                    if row.get("target_type") == "UTILITY"
                    else None
                ),
                "direct_sources": sorted(
                    direct_sources, key=lambda source: source["path"]
                ),
                "closure_sources": [
                    {"path": path, "sha256": closure_sources[path]}
                    for path in sorted(closure_sources)
                ],
                "compile_dependencies": [
                    compile_dependencies[path]
                    for path in sorted(compile_dependencies)
                ],
                "command_semantics": sorted(
                    command_semantics,
                    key=lambda command: (
                        command["kind"],
                        command["output"],
                        command["argv_sha256"],
                    ),
                ),
            }
        )
    return sorted(result, key=lambda row: (row["project_id"], row["target"]))


def _decode_inventory(
    *,
    lane: Mapping[str, Any],
    inventory: Mapping[str, Any],
    target_contract: Mapping[str, tuple[int, str]],
    ordinary_contract: Mapping[str, tuple[str, str, str]],
    build_root: Path,
    roots: _Roots,
) -> tuple[dict[str, dict[str, Any]], dict[str, dict[str, Any]]]:
    lane_id = _text(lane.get("lane_id"), "lane.lane_id")
    group = _text(lane.get("required_group"), "lane.required_group")
    if lane.get("allow_additional_targets") is not False:
        _fail(f"{lane_id} does not enforce its closed configured-target set")
    if group not in _ORDINARY_BY_GROUP:
        _fail(f"{lane_id} has unknown native group {group}")
    tests = _list(inventory.get("tests"), f"{lane_id} CTest inventory tests")
    targets: dict[str, dict[str, Any]] = {}
    ordinary: dict[str, dict[str, Any]] = {}
    seen: set[str] = set()
    for index, raw in enumerate(tests):
        test = _object(raw, f"{lane_id} CTest test[{index}]")
        name = _text(test.get("name"), f"{lane_id} CTest test[{index}].name")
        if name in seen:
            _fail(f"{lane_id} CTest registry duplicates {name}")
        seen.add(name)
        command = _list(test.get("command"), f"{lane_id} CTest command {name}")
        if not command or not all(isinstance(item, str) and item for item in command):
            _fail(f"{lane_id} CTest command {name} is malformed")
        properties = _properties(test, f"{lane_id} CTest {name}")
        filters = [item[15:] for item in command[1:] if item.startswith("--gtest_filter=")]
        if filters:
            if len(filters) != 1 or filters[0] != name:
                _fail(f"{lane_id} GTest {name} does not have its exact filter")
            binary = roots.decode(command[0], build_root, f"{lane_id} GTest {name} binary")
            if not _is_within(binary, build_root) or binary.name not in target_contract:
                _fail(f"{lane_id} GTest {name} is not owned by a contracted target")
            expected_argv = [command[0], f"--gtest_filter={name}", "--gtest_also_run_disabled_tests"]
            if command != expected_argv:
                _fail(f"{lane_id} GTest {name} argv drifted")
            environment = _environment(properties.get("ENVIRONMENT"), f"{lane_id} {name} environment")
            row = targets.setdefault(
                binary.name,
                {"artifact": binary, "names": [], "environment": environment},
            )
            if row["artifact"] != binary or row["environment"] != environment:
                _fail(f"{lane_id} target {binary.name} registrations disagree")
            row["names"].append(name)
        else:
            if name not in ordinary_contract:
                _fail(f"{lane_id} has an uncontracted ordinary CTest entry: {name}")
            ordinary[name] = {
                "command": list(command),
                "properties": properties,
                "environment": _environment(
                    properties.get("ENVIRONMENT"), f"{lane_id} {name} environment"
                ),
            }
    if set(targets) != set(target_contract):
        _fail(
            f"{lane_id} target registry differs from its contract: "
            f"expected={sorted(target_contract)}, actual={sorted(targets)}"
        )
    allow_additions = lane.get("allow_additional_tests")
    if not isinstance(allow_additions, bool):
        _fail(f"{lane_id} allow_additional_tests is not boolean")
    for target, (count, digest) in target_contract.items():
        names = targets[target]["names"]
        if len(names) != len(set(names)) or len(names) < count:
            _fail(f"{lane_id} CTest names moved to or from target {target}")
        if len(names) == count and _name_digest(names) != digest:
            _fail(f"{lane_id} CTest names moved to or from target {target}")
        if len(names) > count and not allow_additions:
            _fail(f"{lane_id} focused target {target} contains added names")
    expected_ordinary = _ORDINARY_BY_GROUP[group]
    if set(ordinary) != expected_ordinary:
        _fail(
            f"{lane_id} ordinary registry differs from its group contract: "
            f"expected={sorted(expected_ordinary)}, actual={sorted(ordinary)}"
        )
    for name in sorted(expected_ordinary):
        source_value, baseline, candidate = ordinary_contract[name]
        source = roots.decode(source_value, roots.repository, f"ordinary source {name}")
        source_file = _regular_file(source, f"ordinary source {name}", contained_by=roots.repository)
        if _sha256_file(source_file) != candidate:
            _fail(f"ordinary source {name} differs from its candidate digest")
        command_paths = [item.split("=", 1)[-1] for item in ordinary[name]["command"]]
        if source_value not in command_paths:
            _fail(f"ordinary CTest {name} does not execute its contracted source")
        ordinary[name]["source"] = source
        ordinary[name]["baseline_sha256"] = baseline
        ordinary[name]["candidate_sha256"] = candidate
    return targets, ordinary


def _target_reply(
    build: _Build,
    target_name: str,
    expected_artifact: Path,
    expected_sources: set[Path],
    roots: _Roots,
    role: str,
) -> _TargetReply:
    if target_name not in build.targets:
        _fail(f"{build.project_id} File API lacks target {target_name}")
    target = build.targets[target_name]
    wrong_type = (
        target.target_type != "EXECUTABLE"
        if role in {"gtest", "ordinary_helper"}
        else target.target_type not in _MATERIAL_TARGET_TYPES
    )
    if wrong_type:
        _fail(f"{build.project_id} target {target_name} has wrong type {target.target_type}")
    if len(target.artifacts) != 1 or Path(os.path.normpath(target.artifacts[0])) != Path(os.path.normpath(expected_artifact)):
        _fail(f"{build.project_id} target {target_name} File API artifact mapping is not exact")
    artifact = _regular_file(
        expected_artifact,
        f"{build.project_id} target {target_name} artifact",
        allow_symlink=role == "local_library",
        contained_by=build.root,
    )
    if role in {"gtest", "ordinary_helper"} and not os.access(artifact, os.X_OK):
        _fail(f"{build.project_id} target {target_name} artifact is not executable")
    direct_code_sources = {
        Path(os.path.normpath(path))
        for path in target.sources
        if path.suffix in _CODE_SOURCE_SUFFIXES
    }
    normalized_expected = {Path(os.path.normpath(path)) for path in expected_sources}
    require_exact_sources = role in {
        "gtest",
        "ordinary_helper",
        "binding_module",
        "production",
        "test_support",
        "dependency",
        "local_library",
    }
    if (
        require_exact_sources
        and direct_code_sources != normalized_expected
    ) or (
        not require_exact_sources
        and expected_sources
        and not normalized_expected.issubset(direct_code_sources)
    ):
        _fail(
            f"{build.project_id} target {target_name} candidate sources are not exact: "
            f"expected={sorted(map(roots.logical, normalized_expected))}, "
            f"actual={sorted(map(roots.logical, direct_code_sources))}"
        )
    for source in target.sources:
        if _is_within(source, build.root):
            source_owner = build.root
        elif _is_within(source, build.source_root):
            source_owner = build.source_root
        elif _is_within(source, roots.repository):
            source_owner = roots.repository
        else:
            _fail(f"{build.project_id} target {target_name} source has no owner")
        _regular_file(
            source,
            f"{build.project_id} target {target_name} source",
            contained_by=source_owner,
        )
    return target


def _command_input_evidence(
    *,
    path: Path,
    commands: Mapping[Path, _Command],
    build: _Build,
    roots: _Roots,
    label: str,
) -> dict[str, Any]:
    normalized = Path(os.path.normpath(path))
    if _is_within(normalized, build.root):
        scope = "selected_build"
        owner = build.root
        if _artifact_kind(normalized) is None and normalized not in commands:
            _fail(f"{label} local non-code input has no parsed Ninja producer")
    elif _is_within(normalized, roots.output):
        if _artifact_kind(normalized) is None:
            _fail(f"{label} non-code input from another output build is unsupported")
        scope = "qualification_output"
        owner = roots.output
    elif _is_within(normalized, roots.repository):
        if _artifact_kind(normalized) is not None:
            _fail(f"{label} links a code artifact directly from the source tree")
        scope = "repository_input"
        owner = roots.repository
    elif _link_system_path(normalized):
        scope = "pinned_container"
        owner = Path("/")
    else:
        _fail(f"{label} input escapes all qualified roots")
    resolved, chain = _symlink_chain(normalized, owner, roots, label)
    if scope == "pinned_container":
        _require_system_library_chain(resolved, chain, label)
    return {
        "path": roots.logical(normalized),
        "resolved_path": roots.logical(resolved),
        "symlink_chain": chain,
        "sha256": _sha256_file(resolved),
        "scope": scope,
        "kind": _artifact_kind(normalized) or "authenticated_non_code_input",
    }


def _prove_target(
    *,
    build: _Build,
    target_name: str,
    artifact: Path,
    expected_sources: set[Path],
    role: str,
    profile: str,
    roots: _Roots,
    responses: _ResponseCollector,
) -> dict[str, Any]:
    target = _target_reply(build, target_name, artifact, expected_sources, roots, role)
    artifact = Path(os.path.normpath(artifact))
    relative_artifact = artifact.relative_to(build.root).as_posix()
    query_rows, query_code_paths, generated_sources = _query_closure(
        build, relative_artifact, roots
    )
    commands_text = _run_text(
        ["/usr/bin/ninja", "-C", str(build.root), "-t", "commands", target_name],
        build.root,
        f"{build.project_id} Ninja commands {target_name}",
    )
    commands = _parse_ninja_commands(
        commands_text,
        build,
        roots,
        responses,
        f"{build.project_id} target {target_name}",
    )
    if artifact not in commands:
        _fail(f"{build.project_id} target {target_name} has no exact Ninja artifact producer")
    final = commands[artifact]
    expected_final_kind = "archive" if target.target_type == "STATIC_LIBRARY" else "link"
    if final.kind != expected_final_kind:
        _fail(
            f"{build.project_id} target {target_name} final producer is {final.kind}, "
            f"expected {expected_final_kind}"
        )

    command_rows: list[dict[str, Any]] = []
    archives: list[dict[str, Any]] = []
    compiled_sources: set[Path] = set()
    artifact_owners: dict[Path, list[str]] = {}
    for owner_name, owner_target in build.targets.items():
        for owner_artifact in owner_target.artifacts:
            artifact_owners.setdefault(
                Path(os.path.normpath(owner_artifact)), []
            ).append(owner_name)
    for output in sorted(commands, key=lambda path: roots.logical(path)):
        command = commands[output]
        if not _is_within(output, build.root) or not _is_within(
            command.directory, build.root
        ):
            _fail(
                f"{build.project_id} target {target_name} command escapes its build"
            )
        output_kind = _artifact_kind(output)
        if output_kind in {"archive", "shared_library"} and _is_within(
            output, build.root
        ):
            owners = artifact_owners.get(output, [])
            if len(owners) != 1:
                _fail(
                    f"{build.project_id} local {output_kind} {roots.logical(output)} "
                    "does not have exactly one File API target owner"
                )
        if command.kind in {"compile", "link"}:
            sanitizer = _check_sanitizer(
                command.argv,
                profile,
                f"{build.project_id} {target_name} {command.kind} {roots.logical(output)}",
            )
        else:
            sanitizer = None
        dependency_closure: dict[str, Any] | None = None
        if command.kind == "compile":
            if output not in build.compile_commands:
                _fail(
                    f"{build.project_id} object {roots.logical(output)} has no compile database row"
                )
            compile_row = build.compile_commands[output]
            if (
                compile_row.source != command.source
                or compile_row.directory != command.directory
                or _compile_semantic_argv(compile_row.argv)
                != _compile_semantic_argv(command.argv)
            ):
                _fail(
                    f"{build.project_id} compile database and Ninja disagree for "
                    f"{roots.logical(output)}"
                )
            if command.source is None:
                _fail(f"{build.project_id} compile command has no source")
            _validate_compile_driver_argv(
                command.argv,
                cwd=command.directory,
                source=command.source,
                output=output,
                build_root=build.root,
                roots=roots,
                expected_configuration=_text(
                    build.evidence.get("configuration"),
                    f"{build.project_id} compile configuration",
                ),
                compile_modes=_object(
                    build.evidence.get("compile_mode_contract"),
                    f"{build.project_id} compile-mode contract",
                ),
                required_driver_flags=frozenset(
                    _project_source_required_compile_flags(
                        _object(
                            build.evidence.get("compile_mode_contract"),
                            f"{build.project_id} compile-mode contract",
                        ),
                        project_id=build.project_id,
                        project_source_root=build.source_root,
                    ).get(command.source, set())
                ),
                label=(
                    f"{build.project_id} {target_name} Ninja compile "
                    f"{roots.logical(output)}"
                ),
            )
            dependency_closure = _compile_dependency_closure(
                build=build,
                record=compile_row,
                commands=commands,
                roots=roots,
                label=(
                    f"{build.project_id} {target_name} compile "
                    f"{roots.logical(output)}"
                ),
            )
            source_file = _regular_file(command.source, f"compiled source for {target_name}")
            compiled_sources.add(command.source)
            source_evidence: dict[str, Any] | None = {
                "path": roots.logical(command.source),
                "sha256": _sha256_file(source_file),
            }
        else:
            source_evidence = None
        output_file = _regular_file(
            output,
            f"Ninja output for {target_name}",
            allow_symlink=command.kind == "link" and _artifact_kind(output) == "shared_library",
            contained_by=roots.output,
        )
        input_evidence = [
            _command_input_evidence(
                path=input_path,
                commands=commands,
                build=build,
                roots=roots,
                label=(
                    f"{build.project_id} {target_name} input "
                    f"{roots.logical(input_path)}"
                ),
            )
            for input_path in command.inputs
        ]
        auxiliary_evidence: list[dict[str, str]] = []
        for auxiliary in command.auxiliary_outputs:
            auxiliary_file = _regular_file(
                auxiliary,
                f"{build.project_id} {target_name} linker auxiliary output",
                contained_by=build.root,
            )
            auxiliary_evidence.append(
                {
                    "path": roots.logical(auxiliary),
                    "sha256": _sha256_file(auxiliary_file),
                }
            )
        command_rows.append(
            {
                "kind": command.kind,
                "directory": roots.logical(command.directory),
                "output": roots.logical(output),
                "output_sha256": _sha256_file(output_file),
                "inputs": sorted(roots.logical(path) for path in command.inputs),
                "input_evidence": sorted(
                    input_evidence, key=lambda row: (row["path"], row["sha256"])
                ),
                "auxiliary_outputs": sorted(
                    auxiliary_evidence, key=lambda row: row["path"]
                ),
                "source": source_evidence,
                "dependency_closure": dependency_closure,
                "argv_sha256": command.argv_sha256,
                "parity_argv_sha256": _parity_argv_sha256(
                    command.argv, build, roots
                ),
                "cross_profile_argv_sha256": _cross_profile_argv_sha256(
                    command.argv, build, roots, profile, command.kind
                ),
                "sanitizer": sanitizer,
            }
        )
        if command.kind == "archive":
            archives.append(_archive_evidence(output, command, roots))

    expected_normalized = {Path(os.path.normpath(path)) for path in expected_sources}
    if expected_normalized and not expected_normalized.issubset(compiled_sources):
        _fail(f"{build.project_id} target {target_name} did not compile its contracted source")
    target_generated_sources = {
        Path(os.path.normpath(source))
        for source in target.sources
        if source.suffix in _CODE_SOURCE_SUFFIXES and _is_within(source, build.root)
    }
    if not target_generated_sources.issubset(generated_sources):
        _fail(
            f"{build.project_id} target {target_name} has generated code without an exact "
            "Ninja query producer"
        )
    if not generated_sources.issubset(commands):
        _fail(
            f"{build.project_id} target {target_name} has generated code without a "
            "parsed, authenticated producer command"
        )

    # Traverse every code-bearing local input from the final artifact.  The
    # command closure and Ninja query must independently account for it.
    pending = [artifact]
    visited: set[Path] = set()
    outside_build: set[Path] = set()
    external_inputs: dict[str, dict[str, Any]] = {}
    while pending:
        current = pending.pop()
        if current in visited:
            continue
        visited.add(current)
        if current not in commands:
            if _is_within(current, build.root):
                _fail(f"local artifact has no Ninja producer: {roots.logical(current)}")
            outside_build.add(current)
            continue
        for dependency in commands[current].inputs:
            if _artifact_kind(dependency) is None:
                continue
            if _is_within(dependency, build.root):
                pending.append(dependency)
            elif _is_within(dependency, roots.output):
                outside_build.add(dependency)
            else:
                if _is_within(dependency, roots.repository):
                    _fail(
                        f"{build.project_id} target {target_name} links a code artifact "
                        "directly from the readonly source tree"
                    )
                if not any(
                    _is_within(dependency, root)
                    for root in (
                        Path("/lib"),
                        Path("/lib64"),
                        Path("/usr/lib"),
                        Path("/usr/lib64"),
                        Path("/usr/local/lib"),
                        Path("/usr/local/lib64"),
                    )
                ):
                    _fail(
                        f"{build.project_id} target {target_name} links an artifact "
                        f"outside qualified roots: {dependency}"
                    )
                resolved, chain = _symlink_chain(
                    dependency,
                    Path("/"),
                    roots,
                    f"external link input for {target_name}",
                )
                _require_system_library_chain(
                    resolved, chain, f"external link input for {target_name}"
                )
                logical = roots.logical(dependency)
                external_inputs[logical] = {
                    "path": logical,
                    "resolved_path": roots.logical(resolved),
                    "symlink_chain": chain,
                    "sha256": _sha256_file(resolved),
                    "scope": "pinned_container",
                }
    graph_required = {
        path for path in query_code_paths if _is_within(path, build.root)
    }
    if not graph_required.issubset(commands):
        missing = sorted(roots.logical(path) for path in graph_required - set(commands))
        _fail(f"{build.project_id} Ninja graph has code inputs without command producers: {missing}")
    if not visited.intersection({artifact}) or not {
        path for path in visited if _is_within(path, build.root)
    }.issubset(set(commands)):
        _fail(f"{build.project_id} command traversal is incomplete for {target_name}")

    normalized_commands = roots.normalize_tool_output(commands_text)
    artifact_file = _regular_file(
        artifact,
        f"{build.project_id} target {target_name} final artifact",
        allow_symlink=role == "local_library",
        contained_by=build.root,
    )
    local_target_dependencies: list[dict[str, str]] = []
    for path in sorted(visited, key=roots.logical):
        if path == artifact or path not in artifact_owners:
            continue
        owners = artifact_owners[path]
        if len(owners) != 1:
            _fail(
                f"{build.project_id} local target dependency has ambiguous ownership: "
                f"{roots.logical(path)}"
            )
        local_target_dependencies.append(
            {
                "project_id": build.project_id,
                "target": owners[0],
                "artifact": roots.logical(path),
            }
        )
    return {
        "project_id": build.project_id,
        "build_root": roots.logical(build.root),
        "target": target_name,
        "role": role,
        "target_type": target.target_type,
        "artifact": {
            "path": roots.logical(artifact),
            "resolved_path": roots.logical(artifact_file),
            "sha256": _sha256_file(artifact_file),
        },
        "file_api_target_reply": {
            "path": roots.logical(target.reply_path),
            "sha256": target.reply_sha256,
        },
        "file_api_sources": [
            _file_api_source_evidence(
                source,
                build=build,
                roots=roots,
                label=f"File API source for {target_name}",
            )
            for source in sorted(target.sources, key=roots.logical)
        ],
        "ninja_query": query_rows,
        "ninja_commands_sha256": _sha256_bytes(normalized_commands.encode("utf-8")),
        "generated_source_producers": sorted(
            roots.logical(path) for path in target_generated_sources
        ),
        "commands": command_rows,
        "archives": sorted(archives, key=lambda row: row["path"]),
        "transitive_local_artifacts": sorted(
            roots.logical(path) for path in visited if _is_within(path, roots.output)
        ),
        "authenticated_local_target_dependencies": local_target_dependencies,
        "unresolved_output_dependency_claims": sorted(
            roots.logical(path) for path in outside_build
        ),
        "transitive_external_link_inputs": [
            external_inputs[key] for key in sorted(external_inputs)
        ],
    }


def _parse_ldd(text: str, roots: _Roots, label: str) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    seen: set[str] = set()
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("linux-vdso.so"):
            continue
        without_address = re.sub(r"\s+\(0x[0-9a-fA-F]+\)$", "", stripped)
        if "=>" in without_address:
            soname, resolved_value = (part.strip() for part in without_address.split("=>", 1))
            if resolved_value == "not found":
                _fail(f"{label} has unresolved dependency {soname}")
        else:
            soname = Path(without_address.split()[0]).name
            resolved_value = without_address.split()[0]
            if resolved_value in {"statically", "not"}:
                _fail(f"{label} returned unsupported ldd output: {stripped}")
        if not resolved_value.startswith("/"):
            _fail(f"{label} resolved {soname} to a non-absolute path")
        resolved_lexical = roots.decode(resolved_value, Path("/"), f"{label} dependency {soname}")
        allowed_root: Path
        scope: str
        if _is_within(resolved_lexical, roots.output):
            allowed_root, scope = roots.output, "qualification_output"
        elif _is_within(resolved_lexical, roots.repository):
            _fail(f"{label} loads a DSO directly from the readonly source tree")
        elif any(
            _is_within(resolved_lexical, root)
            for root in (
                Path("/lib"),
                Path("/lib64"),
                Path("/usr/lib"),
                Path("/usr/lib64"),
                Path("/usr/local/lib"),
                Path("/usr/local/lib64"),
            )
        ):
            allowed_root, scope = Path("/"), "pinned_container"
        else:
            _fail(f"{label} dependency escapes qualified roots: {resolved_value}")
        resolved, chain = _symlink_chain(
            resolved_lexical, allowed_root, roots, f"{label} dependency {soname}"
        )
        if scope == "pinned_container":
            _require_system_library_chain(
                resolved, chain, f"{label} dependency {soname}"
            )
        if soname in seen:
            _fail(f"{label} resolves dependency {soname} more than once")
        seen.add(soname)
        rows.append(
            {
                "soname": soname,
                "path": roots.logical(resolved_lexical),
                "resolved_path": roots.logical(resolved),
                "symlink_chain": chain,
                "sha256": _sha256_file(resolved),
                "scope": scope,
            }
        )
    if not rows:
        _fail(f"{label} returned no concrete runtime dependencies")
    return sorted(rows, key=lambda row: (row["soname"], row["resolved_path"]))


def _elf_evidence(
    path: Path,
    *,
    profile: str,
    environment: Mapping[str, str],
    roots: _Roots,
    strict_runtime: bool,
    require_executable: bool,
) -> dict[str, Any]:
    file_path = _regular_file(
        path,
        f"ELF artifact {path.name}",
        allow_symlink=True,
        contained_by=roots.output,
    )
    try:
        with file_path.open("rb") as stream:
            magic = stream.read(4)
    except OSError as error:
        _fail(f"cannot inspect ELF magic for {path}: {error}")
    if magic != b"\x7fELF":
        _fail(f"native artifact is not ELF: {roots.logical(path)}")
    if require_executable and not os.access(file_path, os.X_OK):
        _fail(f"native ELF is not executable: {roots.logical(path)}")
    tool_env: dict[str, str] = {}
    if "LD_LIBRARY_PATH" in environment:
        tool_env["LD_LIBRARY_PATH"] = environment["LD_LIBRARY_PATH"]
    if {"LD_PRELOAD", "LD_AUDIT"}.intersection(environment):
        _fail(f"ELF environment for {path.name} contains loader injection")
    header = _run_text(["/usr/bin/readelf", "-hW", str(file_path)], file_path.parent, f"readelf header {path.name}")
    program = _run_text(["/usr/bin/readelf", "-lW", str(file_path)], file_path.parent, f"readelf program headers {path.name}")
    dynamic = _run_text(["/usr/bin/readelf", "-dW", str(file_path)], file_path.parent, f"readelf dynamic section {path.name}")
    symbols = _run_text(["/usr/bin/readelf", "-Ws", str(file_path)], file_path.parent, f"readelf symbols {path.name}")
    if (
        "Class:                             ELF64" not in header
        or "Data:                              2's complement, little endian" not in header
        or "Machine:                           Advanced Micro Devices X86-64" not in header
        or not re.search(r"Type:\s+(?:DYN|EXEC)\b", header)
    ):
        _fail(f"native ELF has the wrong architecture/type: {roots.logical(path)}")
    interpreters = re.findall(r"\[Requesting program interpreter: (.+?)\]", program)
    if require_executable:
        if interpreters != ["/lib64/ld-linux-x86-64.so.2"]:
            _fail(f"native executable has the wrong PT_INTERP: {roots.logical(path)}")
    elif interpreters:
        _fail(f"native DSO unexpectedly has PT_INTERP: {roots.logical(path)}")
    ldd = _run_text(
        ["/usr/bin/ldd", str(file_path)],
        file_path.parent,
        f"ldd {path.name}",
        tool_env,
    )
    dependencies = _parse_ldd(ldd, roots, f"ldd {path.name}")
    needed = sorted(set(re.findall(r"\(NEEDED\).*?\[(.+?)\]", dynamic)))
    python_init_symbols = sorted(set(re.findall(r"\bPyInit_[A-Za-z0-9_]+\b", symbols)))
    resolved_sonames = {row["soname"] for row in dependencies}
    if not set(needed).issubset(resolved_sonames):
        _fail(
            f"ldd did not resolve every DT_NEEDED entry for {path.name}: "
            f"missing={sorted(set(needed) - resolved_sonames)}"
        )
    dependency_text = "\n".join(row["soname"] for row in dependencies)
    if profile == "none":
        if any(name in dependency_text for name in _SANITIZER_RUNTIME_NAMES) or any(
            symbol in symbols for symbol in _SANITIZER_SYMBOLS
        ):
            _fail(f"Release ELF unexpectedly contains sanitizer runtime evidence: {path.name}")
    elif profile == "address+undefined":
        required_dependencies = {"libasan.so", "libubsan.so"} if strict_runtime else {"libasan.so"}
        if (
            any(name not in dependency_text for name in required_dependencies)
            or "libtsan.so" in dependency_text
            or "__asan_init" not in symbols
            or (strict_runtime and "__ubsan_handle_" not in symbols)
            or "__tsan_init" in symbols
        ):
            _fail(f"ASan/UBSan ELF runtime identity is incomplete: {path.name}")
    elif profile == "thread":
        if (
            "libtsan.so" not in dependency_text
            or "libasan.so" in dependency_text
            or "libubsan.so" in dependency_text
            or "__tsan_init" not in symbols
            or "__asan_init" in symbols
        ):
            _fail(f"TSan ELF runtime identity is incomplete: {path.name}")
    else:
        _fail(f"unknown ELF sanitizer profile: {profile}")
    normalized_ldd = json.dumps(dependencies, sort_keys=True, separators=(",", ":"))
    return {
        "path": roots.logical(path),
        "resolved_path": roots.logical(file_path),
        "sha256": _sha256_file(file_path),
        "readelf_header_sha256": _sha256_bytes(header.encode("utf-8")),
        "readelf_program_headers_sha256": _sha256_bytes(program.encode("utf-8")),
        "readelf_dynamic_sha256": _sha256_bytes(dynamic.encode("utf-8")),
        "readelf_symbols_sha256": _sha256_bytes(symbols.encode("utf-8")),
        "ldd_normalized_sha256": _sha256_bytes(normalized_ldd.encode("utf-8")),
        "needed": needed,
        "python_init_symbols": python_init_symbols,
        "dependencies": dependencies,
    }


def _auxiliary_install_states(
    *,
    project_builds: Sequence[_Build],
    install_roots: Mapping[str, Path],
    contracts: Mapping[str, _AuxiliaryInstallContract],
    roots: _Roots,
) -> dict[str, _AuxiliaryInstallState]:
    """Authenticate exact manifest multiplicity and each distinct install tree."""

    builds = {build.project_id: build for build in project_builds}
    if set(builds) != set(install_roots) or set(builds) != set(contracts):
        _fail("auxiliary build/install/contract project sets disagree")
    result: dict[str, _AuxiliaryInstallState] = {}
    for project_id in sorted(builds):
        build = builds[project_id]
        contract = contracts[project_id]
        install_root = _directory(
            install_roots[project_id], f"{project_id} exact install root"
        )
        if not _is_within(install_root, roots.output):
            _fail(f"{project_id} install root escapes qualification output")
        manifest = _regular_file(
            build.root / "install_manifest.txt", f"{project_id} install manifest"
        )
        raw_manifest = _read_bytes(manifest, f"{project_id} install manifest")
        try:
            manifest_text = raw_manifest.decode("utf-8")
        except UnicodeDecodeError as error:
            _fail(f"{project_id} install manifest is not UTF-8: {error}")
        lines = manifest_text.splitlines()
        if (
            not raw_manifest.endswith(b"\n")
            or not lines
            or any(not line or "\r" in line for line in lines)
        ):
            _fail(f"{project_id} install manifest line format drifted")
        relative_rows: list[str] = []
        files_by_relative: dict[Path, Path] = {}
        for index, line in enumerate(lines):
            installed = Path(
                os.path.normpath(
                    roots.decode(
                        line, build.root, f"{project_id} install manifest[{index}]"
                    )
                )
            )
            if not _is_within(installed, install_root):
                _fail(f"{project_id} install manifest escapes its exact prefix")
            relative = installed.relative_to(install_root)
            relative_rows.append(relative.as_posix())
            files_by_relative.setdefault(relative, installed)
        counts = Counter(relative_rows)
        repeated = tuple(
            (Path(path), count)
            for path, count in sorted(counts.items())
            if count > 1
        )
        if (
            len(relative_rows) != contract.manifest_row_count
            or len(counts) != contract.distinct_path_count
            or _canonical_sha256(relative_rows)
            != contract.manifest_relative_rows_sha256
            or repeated != contract.repeated_relative_paths
        ):
            _fail(f"{project_id} exact install-manifest multiplicity/order drifted")

        inventory = _exact_bounded_tree_inventory(
            install_root,
            roots=roots,
            label=f"{project_id} exact installed tree",
        )
        inventory_files = {
            Path(row["relative_path"])
            for row in inventory["entries"]
            if row["kind"] == "file"
        }
        inventory_directories = {
            Path(row["relative_path"])
            for row in inventory["entries"]
            if row["kind"] == "directory"
        }
        required_directories = {
            parent
            for file_path in inventory_files
            for parent in file_path.parents
            if parent != Path(".")
        }
        if (
            inventory_files != set(files_by_relative)
            or inventory_directories != required_directories
        ):
            _fail(
                f"{project_id} installed tree differs from the distinct manifest "
                "or contains an unowned empty directory"
            )
        evidence = {
            "project_id": project_id,
            "install_root": roots.logical(install_root),
            "manifest": {
                "path": roots.logical(manifest),
                "sha256": _sha256_bytes(raw_manifest),
                "row_count": len(relative_rows),
                "distinct_path_count": len(counts),
                "relative_rows_sha256": _canonical_sha256(relative_rows),
                "repeated_relative_paths": [
                    {"path": path.as_posix(), "count": count}
                    for path, count in repeated
                ],
            },
            "install_tree_inventory": inventory,
            "owner_counts": {
                "git_source_copy": contract.expected_source_copy_count,
                "cmake_generated": len(contract.generated_files),
                "target_artifact": len(contract.library_files),
            },
            "owner_partition_sha256": None,
        }
        result[project_id] = _AuxiliaryInstallState(
            build=build,
            contract=contract,
            install_root=install_root,
            manifest_path=manifest,
            files_by_relative=files_by_relative,
            evidence=evidence,
        )
    return result


def _validate_generated_install_owner(
    *,
    state: _AuxiliaryInstallState,
    contract: _AuxiliaryGeneratedInstallFile,
    roots: _Roots,
) -> dict[str, Any]:
    project_id = state.contract.project_id
    source_git_root = _containing_git_root(
        state.build.source_root,
        roots.output,
        f"{project_id} generated install source",
    )
    installed = state.files_by_relative.get(contract.install_relative)
    if installed is None:
        _fail(f"{project_id} generated install file is absent from the manifest")
    build_source = _regular_file(
        state.build.root / contract.build_relative,
        f"{project_id} generated install build source {contract.build_relative}",
        contained_by=state.build.root,
    )
    installed_file = _regular_file(
        installed,
        f"{project_id} generated installed file {contract.install_relative}",
        contained_by=state.install_root,
    )
    digest = _sha256_file(installed_file)
    if digest != _sha256_file(build_source):
        _fail(f"{project_id} generated installed bytes differ from their exact build owner")
    cmake_source = _regular_file(
        state.build.source_root / contract.cmake_source_relative,
        f"{project_id} generated install CMake owner",
        contained_by=state.build.source_root,
    )
    cmake_identity = _git_tracked_file_identity(
        cmake_source,
        source_git_root,
        f"{project_id} generated install CMake owner",
    )
    template_evidence: dict[str, Any] | None = None
    if contract.template_source_relative is not None:
        template = _regular_file(
            state.build.source_root / contract.template_source_relative,
            f"{project_id} generated install template owner",
            contained_by=state.build.source_root,
        )
        template_evidence = {
            "path": roots.logical(template),
            "sha256": _sha256_file(template),
            "source_identity": _git_tracked_file_identity(
                template,
                source_git_root,
                f"{project_id} generated install template owner",
            ),
        }

    try:
        generated_text = _read_bytes(
            build_source, f"{project_id} generated CMake install owner"
        ).decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        _fail(f"{project_id} generated CMake install owner is not UTF-8: {error}")
    if contract.producer_kind == "configure_package_config_file_final":
        try:
            cmake_text = _read_bytes(
                cmake_source, f"{project_id} CMake final-writer recipe"
            ).decode("utf-8", errors="strict")
        except UnicodeDecodeError as error:
            _fail(f"{project_id} CMake final-writer recipe is not UTF-8: {error}")
        output_name = contract.build_relative.name
        writer_kinds = [
            kind
            for kind, body in re.findall(
                r"\b(configure_file|configure_package_config_file)\s*\((.*?)\)",
                cmake_text,
                flags=re.DOTALL,
            )
            if output_name in body
        ]
        if writer_kinds != ["configure_file", "configure_package_config_file"]:
            _fail(f"{project_id} Config.cmake final-writer order drifted")
    elif contract.producer_kind == "write_basic_package_version_file":
        try:
            cmake_text = _read_bytes(
                cmake_source, f"{project_id} CMake version-writer recipe"
            ).decode("utf-8", errors="strict")
        except UnicodeDecodeError as error:
            _fail(f"{project_id} CMake version-writer recipe is not UTF-8: {error}")
        matches = [
            body
            for body in re.findall(
                r"\bwrite_basic_package_version_file\s*\((.*?)\)",
                cmake_text,
                flags=re.DOTALL,
            )
            if contract.build_relative.name in body
        ]
        if len(matches) != 1:
            _fail(f"{project_id} ConfigVersion.cmake writer drifted")
    elif contract.producer_kind == "install_export_main":
        actual_targets = sorted(
            set(
                re.findall(
                    r"\badd_library\(([^ )]+) (?:STATIC|SHARED|MODULE|INTERFACE|UNKNOWN) IMPORTED\)",
                    generated_text,
                )
            )
        )
        if actual_targets != list(contract.required_logical_targets):
            _fail(f"{project_id} installed main export logical-target set drifted")
    elif contract.producer_kind == "install_export_configuration":
        actual_targets = sorted(
            set(
                re.findall(
                    r"\bset_property\(TARGET ([^ )]+) APPEND PROPERTY "
                    r"IMPORTED_CONFIGURATIONS RELWITHDEBINFO\)",
                    generated_text,
                )
            )
        )
        if actual_targets != list(contract.required_logical_targets):
            _fail(f"{project_id} installed configuration export target set drifted")
    else:
        _fail(f"{project_id} generated install producer kind is unsupported")
    return {
        "kind": "cmake_generated_install_source",
        "producer_kind": contract.producer_kind,
        "path": roots.logical(build_source),
        "sha256": digest,
        "cmake_recipe": {
            "path": roots.logical(cmake_source),
            "sha256": _sha256_file(cmake_source),
            "source_identity": cmake_identity,
        },
        "template": template_evidence,
        "required_logical_targets": list(contract.required_logical_targets),
    }


def _qualified_install_input_inventory(
    *,
    states: Mapping[str, _AuxiliaryInstallState],
    roots: _Roots,
) -> dict[Path, dict[str, Any]]:
    """Bind every non-library install file to one exact Git/CMake owner."""

    result: dict[Path, dict[str, Any]] = {}
    for project_id in sorted(states):
        state = states[project_id]
        source_git_root = _containing_git_root(
            state.build.source_root,
            roots.output,
            f"{project_id} exact install source",
        )
        try:
            project_prefix = PurePosixPath(
                state.build.source_root.relative_to(source_git_root).as_posix()
            )
        except ValueError:
            _fail(f"{project_id} source root escapes its Git root")
        _index_tree, _object_format, git_entries = _git_index(
            source_git_root, f"{project_id} exact install source index"
        )
        source_owners: dict[Path, Path] = {}
        for install_prefix, source_prefix in state.contract.source_copy_prefixes:
            pure_source_prefix = PurePosixPath(source_prefix.as_posix())
            for repository_relative, (mode, _blob) in git_entries.items():
                pure_repository = PurePosixPath(repository_relative)
                if not pure_repository.is_relative_to(project_prefix):
                    continue
                pure_source = pure_repository.relative_to(project_prefix)
                if pure_source == pure_source_prefix or not pure_source.is_relative_to(
                    pure_source_prefix
                ):
                    continue
                if mode not in {"100644", "100755"}:
                    _fail(f"{project_id} install source prefix contains a non-file entry")
                suffix = pure_source.relative_to(pure_source_prefix)
                install_relative = install_prefix / Path(*suffix.parts)
                source = source_git_root / Path(*pure_repository.parts)
                if install_relative in source_owners:
                    _fail(f"{project_id} source install prefixes overlap")
                source_owners[install_relative] = source
        if len(source_owners) != state.contract.expected_source_copy_count:
            _fail(f"{project_id} exact Git-source install count drifted")

        owner_rows: list[dict[str, Any]] = []
        manifest_identity = dict(state.evidence["manifest"])
        for install_relative in sorted(source_owners, key=lambda path: path.as_posix()):
            source = _regular_file(
                source_owners[install_relative],
                f"{project_id} exact installed source {install_relative}",
                contained_by=state.build.source_root,
            )
            installed = state.files_by_relative.get(install_relative)
            if installed is None:
                _fail(f"{project_id} exact installed source copy is absent")
            installed_file = _regular_file(
                installed,
                f"{project_id} exact source install copy {install_relative}",
                contained_by=state.install_root,
            )
            digest = _sha256_file(source)
            if _sha256_file(installed_file) != digest:
                _fail(f"{project_id} installed source copy changed bytes")
            producer = {
                "kind": "git_tracked_install_source",
                "path": roots.logical(source),
                "sha256": digest,
                "source_identity": _git_tracked_file_identity(
                    source,
                    source_git_root,
                    f"{project_id} exact installed source {install_relative}",
                ),
            }
            row = {
                "path": roots.logical(installed_file),
                "resolved_path": roots.logical(installed_file),
                "symlink_chain": [],
                "sha256": digest,
                "project_id": project_id,
                "install_relative_path": install_relative.as_posix(),
                "install_manifest": manifest_identity,
                "producer": producer,
            }
            result[installed_file] = row
            owner_rows.append(
                {
                    "install_relative_path": install_relative.as_posix(),
                    "kind": producer["kind"],
                    "owner": roots.logical(source),
                }
            )

        for generated_contract in state.contract.generated_files:
            install_relative = generated_contract.install_relative
            installed = state.files_by_relative.get(install_relative)
            if installed is None:
                _fail(f"{project_id} exact generated install copy is absent")
            installed_file = _regular_file(
                installed,
                f"{project_id} exact generated install copy {install_relative}",
                contained_by=state.install_root,
            )
            producer = _validate_generated_install_owner(
                state=state,
                contract=generated_contract,
                roots=roots,
            )
            row = {
                "path": roots.logical(installed_file),
                "resolved_path": roots.logical(installed_file),
                "symlink_chain": [],
                "sha256": _sha256_file(installed_file),
                "project_id": project_id,
                "install_relative_path": install_relative.as_posix(),
                "install_manifest": manifest_identity,
                "producer": producer,
            }
            if installed_file in result:
                _fail(f"{project_id} generated/source install ownership overlaps")
            result[installed_file] = row
            owner_rows.append(
                {
                    "install_relative_path": install_relative.as_posix(),
                    "kind": producer["kind"],
                    "owner": producer["path"],
                }
            )
        state.evidence["non_library_owner_projection_sha256"] = _canonical_sha256(
            sorted(owner_rows, key=lambda row: row["install_relative_path"])
        )
    return result


def _installed_copies(
    *,
    states: Mapping[str, _AuxiliaryInstallState],
    target_evidence: Sequence[Mapping[str, Any]],
    roots: _Roots,
) -> list[dict[str, Any]]:
    """Join each installed library to its exact project/target/artifact owner."""

    target_index: dict[tuple[str, str], Mapping[str, Any]] = {}
    for target in target_evidence:
        key = (
            _text(target.get("project_id"), "auxiliary library project"),
            _text(target.get("target"), "auxiliary library target"),
        )
        if key in target_index:
            _fail(f"auxiliary target evidence duplicates {key}")
        target_index[key] = target
    expected_targets = {
        (project_id, library.target)
        for project_id, state in states.items()
        for library in state.contract.library_files
    }
    if set(target_index) != expected_targets:
        _fail("auxiliary installed-library target evidence set drifted")
    rows: list[dict[str, Any]] = []
    for project_id in sorted(states):
        state = states[project_id]
        for library in state.contract.library_files:
            producer = target_index[(project_id, library.target)]
            artifact = _object(
                producer.get("artifact"), f"{project_id} {library.target} artifact"
            )
            expected_artifact = state.build.root / library.build_artifact_relative
            artifact_path = _regular_file(
                expected_artifact,
                f"{project_id} {library.target} exact build artifact",
                contained_by=state.build.root,
            )
            installed = state.files_by_relative.get(library.install_relative)
            if installed is None:
                _fail(f"{project_id} installed library is absent from its manifest")
            installed_file = _regular_file(
                installed,
                f"{project_id} exact installed library {library.install_relative}",
                contained_by=state.install_root,
            )
            digest = _sha256_file(artifact_path)
            if (
                producer.get("project_id") != project_id
                or producer.get("target") != library.target
                or producer.get("target_type") != "SHARED_LIBRARY"
                or artifact.get("path") != roots.logical(expected_artifact)
                or artifact.get("sha256") != digest
                or _sha256_file(installed_file) != digest
            ):
                _fail(
                    f"{project_id} installed library {library.install_relative} "
                    "does not match its exact File API/Ninja target owner"
                )
            rows.append(
                {
                    "project_id": project_id,
                    "path": roots.logical(installed_file),
                    "resolved_path": roots.logical(installed_file),
                    "symlink_chain": [],
                    "sha256": digest,
                    "install_relative_path": library.install_relative.as_posix(),
                    "producer_project": project_id,
                    "producer_target": library.target,
                    "producer_artifact": roots.logical(artifact_path),
                    "install_manifest": dict(state.evidence["manifest"]),
                }
            )
    return sorted(rows, key=lambda row: (row["project_id"], row["path"]))


def _finalize_auxiliary_install_owner_partitions(
    *,
    states: Mapping[str, _AuxiliaryInstallState],
    qualified_inputs: Mapping[Path, Mapping[str, Any]],
    installed_copies: Sequence[Mapping[str, Any]],
) -> None:
    owners_by_project: dict[str, dict[str, dict[str, Any]]] = {
        project_id: {} for project_id in states
    }
    for raw in qualified_inputs.values():
        row = _object(raw, "qualified install owner")
        project_id = _text(row.get("project_id"), "qualified install owner project")
        relative = _text(
            row.get("install_relative_path"), "qualified install owner relative path"
        )
        producer = _object(row.get("producer"), "qualified install producer")
        if relative in owners_by_project[project_id]:
            _fail(f"{project_id} repeats a non-library install owner")
        owners_by_project[project_id][relative] = {
            "path": relative,
            "kind": _text(producer.get("kind"), "qualified install producer kind"),
            "owner": _text(producer.get("path"), "qualified install producer path"),
        }
    for raw in installed_copies:
        row = _object(raw, "installed target owner")
        project_id = _text(row.get("project_id"), "installed target owner project")
        relative = _text(
            row.get("install_relative_path"), "installed target owner relative path"
        )
        if relative in owners_by_project[project_id]:
            _fail(f"{project_id} install owner partition overlaps")
        owners_by_project[project_id][relative] = {
            "path": relative,
            "kind": "target_artifact",
            "owner": _text(row.get("producer_target"), "installed target owner"),
        }
    for project_id, state in states.items():
        actual = owners_by_project[project_id]
        expected = {path.as_posix() for path in state.files_by_relative}
        if set(actual) != expected or len(actual) != state.contract.distinct_path_count:
            _fail(
                f"{project_id} install owner partition is not the exact distinct tree"
            )
        state.evidence["owner_partition_sha256"] = _canonical_sha256(
            [actual[path] for path in sorted(actual)]
        )


def _attach_qualified_install_inputs(
    build: _Build,
    inputs: Mapping[Path, dict[str, Any]],
) -> None:
    build.qualified_install_inputs = dict(inputs)
    for path, row in build.configure_inputs.items():
        identity = _object(row.get("identity"), "configure input identity")
        if identity.get("kind") != "qualification_output_owner_required":
            continue
        owner = inputs.get(path)
        if owner is None:
            _fail(
                f"configure input under qualification output lacks an exact install "
                f"owner: {path}"
            )
        row["identity"] = {
            "kind": "qualified_install_input",
            "owner": owner,
        }


def _require_bound_configure_inputs(builds: Sequence[_Build]) -> None:
    for build in builds:
        for row in build.configure_inputs.values():
            identity = _object(row.get("identity"), "configure input identity")
            if identity.get("kind") == "qualification_output_owner_required":
                _fail(
                    f"{build.project_id} retains an unbound qualification-output "
                    f"configure input: {row.get('path')}"
                )


def _bind_output_dependency_owners(
    *,
    proofs: Sequence[dict[str, Any]],
    target_owners: Sequence[Mapping[str, Any]],
    installed_owners: Sequence[Mapping[str, Any]],
    roots: _Roots,
) -> None:
    """Replace every deferred output dependency with one authenticated owner."""

    owner_index: dict[tuple[str, str, str, str, str, str], dict[str, Any]] = {}
    for raw in target_owners:
        row = _object(raw, "output dependency target owner")
        artifact = _object(row.get("artifact"), "output dependency target artifact")
        target_type = _text(row.get("target_type"), "dependency owner target type")
        artifact_path = _text(artifact.get("path"), "dependency owner path")
        artifact_digest = _text(artifact.get("sha256"), "dependency owner digest")
        artifact_kind = _artifact_kind(Path(artifact_path))
        if artifact_kind not in {"archive", "shared_library"}:
            continue
        if artifact_kind == "archive":
            if target_type != "STATIC_LIBRARY":
                _fail("archive output owner is not a STATIC_LIBRARY target")
            archives = [
                _object(item, "output owner archive evidence")
                for item in _list(row.get("archives"), "output owner archives")
            ]
            matching_archives = [
                item
                for item in archives
                if item.get("path") == artifact_path
                and item.get("sha256") == artifact_digest
            ]
            if len(matching_archives) != 1:
                _fail("static output owner lacks exact deep archive evidence")
        else:
            if target_type not in {"SHARED_LIBRARY", "MODULE_LIBRARY"}:
                _fail("DSO output owner is not a shared/module target")
            elf = _object(row.get("elf"), "DSO output owner ELF evidence")
            if (
                elf.get("path") != artifact_path
                or elf.get("sha256") != artifact_digest
            ):
                _fail("DSO output owner lacks exact ELF identity evidence")
        owner = {
                "kind": "file_api_ninja_target",
                "project_id": _text(row.get("project_id"), "dependency owner project"),
                "target": _text(row.get("target"), "dependency owner target"),
                "path": artifact_path,
                "resolved_path": _text(
                    artifact.get("resolved_path"), "dependency owner resolved path"
                ),
                "sha256": artifact_digest,
            }
        owner_index[
            (
                owner["kind"],
                owner["project_id"],
                owner["target"],
                owner["path"],
                owner["resolved_path"],
                owner["sha256"],
            )
        ] = owner
    for raw in installed_owners:
        row = _object(raw, "installed output dependency owner")
        owner = {
                "kind": "authenticated_install_copy",
                "project_id": _text(row.get("producer_project"), "install producer project"),
                "target": _text(row.get("producer_target"), "install producer target"),
                "path": _text(row.get("path"), "installed owner path"),
                "resolved_path": _text(
                    row.get("resolved_path"), "installed owner resolved path"
                ),
                "sha256": _text(row.get("sha256"), "installed owner digest"),
            }
        owner_index[
            (
                owner["kind"],
                owner["project_id"],
                owner["target"],
                owner["path"],
                owner["resolved_path"],
                owner["sha256"],
            )
        ] = owner
    owners = list(owner_index.values())

    for proof in proofs:
        claims = _list(
            proof.pop("unresolved_output_dependency_claims", None),
            f"{proof.get('project_id')} {proof.get('target')} output dependency claims",
        )
        bound: list[dict[str, Any]] = []
        for index, claim in enumerate(claims):
            logical = _text(claim, f"output dependency claim[{index}]")
            lexical = roots.decode(
                logical,
                roots.output,
                f"output dependency claim[{index}]",
            )
            dependency = _regular_file(
                lexical,
                f"output dependency claim[{index}]",
                allow_symlink=True,
                contained_by=roots.output,
            )
            dependency_path = roots.logical(Path(os.path.normpath(lexical)))
            resolved_path = roots.logical(dependency)
            digest = _sha256_file(dependency)
            matches = [
                owner
                for owner in owners
                if owner["sha256"] == digest
                and (
                    owner["path"] == dependency_path
                    or owner["resolved_path"] == resolved_path
                )
            ]
            semantic_matches: dict[
                tuple[str, str, str, str, str], dict[str, Any]
            ] = {}
            for owner in matches:
                identity = (
                    owner["kind"],
                    owner["project_id"],
                    owner["target"],
                    owner["resolved_path"],
                    owner["sha256"],
                )
                selected = semantic_matches.get(identity)
                if selected is None or (
                    owner["path"] == dependency_path
                    and selected["path"] != dependency_path
                ):
                    semantic_matches[identity] = owner
            if len(semantic_matches) != 1:
                _fail(
                    "qualification-output archive/DSO has no unique authenticated "
                    "upstream File API/Ninja or install owner: "
                    f"{dependency_path}"
                )
            owner = next(iter(semantic_matches.values()))
            if (
                owner["project_id"] == proof.get("project_id")
                and owner["target"] == proof.get("target")
            ):
                _fail(f"output dependency self-owns its external claim: {dependency_path}")
            bound.append(
                {
                    "path": dependency_path,
                    "resolved_path": resolved_path,
                    "sha256": digest,
                    "owner": owner,
                }
            )
        proof["authenticated_output_dependencies"] = sorted(
            bound, key=lambda row: (row["path"], row["owner"]["project_id"], row["owner"]["target"])
        )


def _validate_local_runtime_dependencies(
    elf_rows: Sequence[Mapping[str, Any]],
    target_rows: Sequence[Mapping[str, Any]],
    installed_rows: Sequence[Mapping[str, Any]],
) -> None:
    owners: set[tuple[str, str]] = set()
    for target in target_rows:
        artifact = _object(target.get("artifact"), "target artifact ownership row")
        owners.add(
            (
                _text(artifact.get("resolved_path"), "target resolved artifact"),
                _text(artifact.get("sha256"), "target artifact digest"),
            )
        )
    for installed in installed_rows:
        owners.add(
            (
                _text(installed.get("resolved_path"), "installed resolved artifact"),
                _text(installed.get("sha256"), "installed artifact digest"),
            )
        )
    for elf in elf_rows:
        for dependency in _list(elf.get("dependencies"), "ELF dependencies"):
            row = _object(dependency, "ELF dependency")
            if row.get("scope") != "qualification_output":
                continue
            identity = (
                _text(row.get("resolved_path"), "local ELF dependency path"),
                _text(row.get("sha256"), "local ELF dependency digest"),
            )
            if identity not in owners:
                _fail(
                    "runtime dependency has no File API/Ninja or authenticated install owner: "
                    f"{identity[0]}"
                )


def _helper_artifact(
    ordinary: Mapping[str, Any],
    variable: str,
    expected_name: str,
    build_root: Path,
    roots: _Roots,
    label: str,
) -> Path:
    prefix = f"-D{variable}="
    values = [item[len(prefix) :] for item in ordinary["command"] if item.startswith(prefix)]
    if len(values) != 1:
        _fail(f"{label} does not name exactly one {variable} helper")
    artifact = roots.decode(values[0], build_root, f"{label} helper artifact")
    if artifact.name != expected_name or artifact.parent != build_root:
        _fail(f"{label} helper artifact path drifted")
    return Path(os.path.normpath(artifact))


def _inspection_tool_evidence() -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for value in ("/usr/bin/ar", "/usr/bin/ldd", "/usr/bin/readelf"):
        path = _regular_file(Path(value), f"inspection tool {value}", allow_symlink=True)
        rows.append(
            {
                "path": value,
                "resolved_path": path.as_posix(),
                "sha256": _sha256_file(path),
            }
        )
    return rows


def _require_production_test_free(
    rows: Sequence[Mapping[str, Any]],
    exclusions: _ProductionExclusions,
) -> None:
    def matches(patterns: Sequence[re.Pattern[str]], value: str) -> bool:
        return any(pattern.search(value) for pattern in patterns)

    def require_target_name(value: Any, label: str) -> str:
        name = _text(value, label)
        if matches(exclusions.target_names, name) or matches(
            exclusions.dependency_names, name
        ):
            _fail(f"production closure contains forbidden target/dependency {name}")
        return name

    def require_path(value: Any, label: str) -> str:
        path = _text(value, label)
        if path.startswith("/workspace/") and matches(
            exclusions.repository_source_paths, path
        ):
            _fail(f"production closure contains forbidden repository source {path}")
        dependency_candidates = {path, PurePosixPath(path).name}
        dependency_candidates.update(PurePosixPath(path).parts)
        if any(
            matches(exclusions.dependency_names, candidate)
            for candidate in dependency_candidates
        ):
            _fail(f"production closure contains forbidden test dependency {path}")
        return path

    for raw in rows:
        row = _object(raw, "production target proof")
        target = require_target_name(row.get("target"), "production target name")
        for source in _list(row.get("file_api_sources"), f"{target} File API sources"):
            require_path(
                _object(source, "production source").get("path"),
                "production source path",
            )
        for command in _list(row.get("commands"), f"{target} commands"):
            command_row = _object(command, "production command")
            source = command_row.get("source")
            if source is not None:
                require_path(
                    _object(source, "production command source").get("path"),
                    "production command source path",
                )
            for input_path in _list(
                command_row.get("inputs"), "production command inputs"
            ):
                require_path(input_path, "production command input")
            for input_row in _list(
                command_row.get("input_evidence", []),
                "production command input evidence",
            ):
                require_path(
                    _object(input_row, "production input evidence").get("path"),
                    "production input evidence path",
                )
            dependency_closure = command_row.get("dependency_closure")
            if dependency_closure is not None:
                for dependency in _list(
                    _object(
                        dependency_closure, "production compile dependency closure"
                    ).get("files"),
                    "production compiler dependencies",
                ):
                    require_path(
                        _object(
                            dependency, "production compiler dependency"
                        ).get("path"),
                        "production compiler dependency path",
                    )
        for query in _list(row.get("ninja_query"), f"{target} Ninja query"):
            for edge in _list(_object(query, "production query").get("edges"), "production query edges"):
                require_path(
                    _object(edge, "production query edge").get("path"),
                    "production query path",
                )
        for external in _list(
            row.get("transitive_external_link_inputs", []),
            f"{target} external link inputs",
        ):
            require_path(
                _object(external, "production external link input").get("path"),
                "production external link path",
            )
        for dependency in _list(
            row.get("authenticated_output_dependencies", []),
            f"{target} authenticated output dependencies",
        ):
            dependency_row = _object(
                dependency, "production authenticated output dependency"
            )
            require_path(
                dependency_row.get("path"),
                "production authenticated output dependency path",
            )
            owner = _object(
                dependency_row.get("owner"),
                "production authenticated output dependency owner",
            )
            require_target_name(
                owner.get("target"),
                "production authenticated output dependency owner target",
            )
        generator = row.get("generator")
        if generator is not None:
            require_target_name(
                _object(generator, "production generator").get("target"),
                "production generator target",
            )
        elf = row.get("elf")
        if elf is not None:
            elf_row = _object(elf, "production ELF")
            for needed in _list(elf_row.get("needed"), "production ELF NEEDED"):
                name = _text(needed, "production NEEDED entry")
                if matches(exclusions.dependency_names, name):
                    _fail(f"production ELF links a test dependency: {needed}")
            for dependency in _list(
                elf_row.get("dependencies"), "production ELF dependencies"
            ):
                dependency_row = _object(
                    dependency, "production ELF dependency"
                )
                soname = _text(
                    dependency_row.get("soname"), "production ELF dependency soname"
                )
                if matches(exclusions.dependency_names, soname):
                    _fail(f"production ELF links a test dependency: {soname}")
                require_path(
                    dependency_row.get("path"),
                    "production ELF dependency path",
                )


def _production_role_catalog(
    value: Mapping[str, Any],
    *,
    repository_root: Path,
) -> frozenset[tuple[str, str]]:
    outer = _object(value, "build-provenance contract")
    projects = _object(outer.get("projects"), "build-provenance projects")
    result: set[tuple[str, str]] = set()
    for project_id in projects:
        contract = _production_contract(
            outer,
            expected_project=project_id,
            repository_root=repository_root,
        )
        result.update((project_id, target) for target in contract.targets)
    return frozenset(result)


def _require_production_role_closure(
    rows: Sequence[Mapping[str, Any]],
    allowed: frozenset[tuple[str, str]],
) -> None:
    """Require every authenticated local owner in a production closure to be production."""

    def require(project: Any, target: Any, label: str) -> None:
        identity = (_text(project, f"{label} project"), _text(target, f"{label} target"))
        if identity not in allowed:
            _fail(
                f"production closure reaches non-production target "
                f"{identity[0]}/{identity[1]}"
            )

    for raw in rows:
        row = _object(raw, "production role proof")
        require(row.get("project_id"), row.get("target"), "production root")
        for dependency in _list(
            row.get("authenticated_local_target_dependencies", []),
            "production local target dependencies",
        ):
            item = _object(dependency, "production local target dependency")
            require(item.get("project_id"), item.get("target"), "local dependency")
        for dependency in _list(
            row.get("authenticated_output_dependencies", []),
            "production output dependencies",
        ):
            owner = _object(
                _object(dependency, "production output dependency").get("owner"),
                "production output dependency owner",
            )
            require(owner.get("project_id"), owner.get("target"), "output dependency")
        generator = row.get("generator")
        if generator is not None:
            item = _object(generator, "production utility generator")
            require(item.get("project_id"), item.get("target"), "utility generator")
        for command in _list(row.get("commands", []), "production commands"):
            closure = _object(command, "production command").get("dependency_closure")
            if closure is None:
                continue
            for dependency in _list(
                _object(closure, "production dependency closure").get("files"),
                "production dependency files",
            ):
                producer = _object(dependency, "production dependency file").get(
                    "producer"
                )
                if producer is None:
                    continue
                producer_row = _object(producer, "production dependency producer")
                if producer_row.get("kind") == "executable_build_generator":
                    require(
                        producer_row.get("project_id"),
                        producer_row.get("target"),
                        "generated input producer",
                    )


def _require_utility_generator_owners(rows: Sequence[Mapping[str, Any]]) -> None:
    material_owners: dict[tuple[str, str], list[Mapping[str, Any]]] = {}
    for raw in rows:
        row = _object(raw, "production target proof")
        if row.get("target_type") == "UTILITY":
            continue
        artifact = _object(row.get("artifact"), "production target artifact")
        key = (
            _text(row.get("project_id"), "production target project"),
            _text(artifact.get("path"), "production target artifact path"),
        )
        material_owners.setdefault(key, []).append(row)
    for raw in rows:
        row = _object(raw, "production target proof")
        if row.get("target_type") != "UTILITY":
            continue
        generator = _object(row.get("generator"), "UTILITY generator")
        key = (
            _text(generator.get("project_id"), "UTILITY generator project"),
            _text(generator.get("path"), "UTILITY generator path"),
        )
        owners = material_owners.get(key, [])
        if len(owners) != 1:
            _fail(
                f"UTILITY target {row.get('target')} does not have exactly one "
                "separately proved production generator target"
            )
        owner = owners[0]
        if (
            owner.get("target") != generator.get("target")
            or _object(owner.get("artifact"), "UTILITY generator owner artifact").get(
                "sha256"
            )
            != generator.get("sha256")
        ):
            _fail(f"UTILITY target {row.get('target')} generator ownership drifted")


def _join_utility_relationships(
    *,
    project_id: str,
    proofs: Sequence[Mapping[str, Any]],
    contracts: Mapping[tuple[str, str], _UtilityTargetContract],
    require_complete_project: bool,
) -> list[dict[str, Any]]:
    proof_index: dict[str, Mapping[str, Any]] = {}
    for raw in proofs:
        row = _object(raw, "utility relationship target proof")
        if row.get("project_id") != project_id:
            _fail("utility relationship proof belongs to the wrong project")
        target = _text(row.get("target"), "utility relationship proof target")
        if target in proof_index:
            _fail(f"utility relationship proof duplicates target {target}")
        proof_index[target] = row
    project_contracts = {
        target: row
        for (owner_project, target), row in contracts.items()
        if owner_project == project_id
    }
    actual_utility_targets = {
        target
        for target, row in proof_index.items()
        if row.get("target_type") == "UTILITY"
    }
    triggered = {
        target
        for target, contract in project_contracts.items()
        if require_complete_project
        or target in proof_index
        or bool(set(contract.consumer_targets).intersection(proof_index))
    }
    if actual_utility_targets != triggered:
        _fail(
            f"{project_id} utility proof/contract selection drifted: "
            f"expected={sorted(triggered)}, actual={sorted(actual_utility_targets)}"
        )
    rows: list[dict[str, Any]] = []
    for target_name in sorted(triggered):
        contract = project_contracts[target_name]
        required_targets = {
            target_name,
            contract.producer_target,
            *contract.consumer_targets,
        }
        if not required_targets.issubset(proof_index):
            _fail(
                f"{project_id} utility {target_name} proof omits producer/consumer targets: "
                f"{sorted(required_targets - set(proof_index))}"
            )
        utility = proof_index[target_name]
        producer = proof_index[contract.producer_target]
        utility_artifact = _object(utility.get("artifact"), "utility proof artifact")
        producer_artifact = _object(producer.get("artifact"), "utility producer artifact")
        build_root = _text(utility.get("build_root"), "utility proof build root")
        expected_generated_path = f"{build_root}/{contract.primary_output.as_posix()}"
        expected_producer_path = f"{build_root}/{contract.producer_artifact.as_posix()}"
        if (
            utility.get("target_type") != "UTILITY"
            or utility.get("contract_artifact") != contract.primary_output.as_posix()
            or utility_artifact.get("path") != expected_generated_path
            or producer.get("target_type") != "EXECUTABLE"
            or producer.get("contract_artifact") != contract.producer_artifact.as_posix()
            or producer_artifact.get("path") != expected_producer_path
        ):
            _fail(f"{project_id} utility {target_name} proof identity drifted")
        generator = _object(utility.get("generator"), "utility generator proof")
        if (
            generator.get("project_id") != project_id
            or generator.get("target") != contract.producer_target
            or generator.get("path") != expected_producer_path
            or generator.get("sha256") != producer_artifact.get("sha256")
        ):
            _fail(f"{project_id} utility {target_name} generator join drifted")
        generator_dependency_matches = [
            dependency
            for raw_dependency in _list(
                utility.get("generator_dependencies"),
                "utility generator dependencies",
            )
            if (
                (dependency := _object(raw_dependency, "utility generator dependency")).get(
                    "producer_target"
                )
                == contract.producer_target
                and dependency.get("path") == expected_producer_path
                and dependency.get("sha256") == producer_artifact.get("sha256")
            )
        ]
        if not generator_dependency_matches:
            _fail(f"{project_id} utility {target_name} generator dependency is unjoined")

        actual_relationships: dict[tuple[str, str], Mapping[str, Any]] = {}
        for proof_target, proof in proof_index.items():
            matches = [
                generated
                for raw_generated in _list(
                    proof.get("required_generated_inputs", []),
                    f"{proof_target} required generated inputs",
                )
                if (
                    (generated := _object(raw_generated, "required generated input")).get(
                        "path"
                    )
                    == expected_generated_path
                )
            ]
            if matches:
                if len(matches) != 1:
                    _fail(f"{proof_target} repeats utility generated input")
                actual_relationships[(proof_target, "compiled_generated_input")] = matches[0]
        for raw_relationship in _list(
            utility.get("consumer_relationships"),
            "utility target-dependency relationships",
        ):
            relationship = _object(
                raw_relationship, "utility target-dependency relationship"
            )
            if relationship.get("kind") != "target_dependency":
                continue
            consumer_target = _text(
                relationship.get("target"), "utility target-dependency consumer"
            )
            if relationship.get("graph_style") != "solid":
                _fail(
                    f"{project_id} utility {target_name} target dependency is not exact"
                )
            key = (consumer_target, "target_dependency")
            if key in actual_relationships:
                _fail(f"{project_id} utility {target_name} repeats a target dependency")
            actual_relationships[key] = relationship
        if set(actual_relationships) != set(contract.consumer_relationships):
            _fail(
                f"{project_id} utility {target_name} actual consumer relationships "
                f"drifted: expected={sorted(contract.consumer_relationships)}, "
                f"actual={sorted(actual_relationships)}"
            )
        for (consumer_target, relationship_kind), generated in actual_relationships.items():
            if relationship_kind != "compiled_generated_input":
                continue
            if (
                generated.get("sha256") != utility_artifact.get("sha256")
                or generated.get("producer_target") != contract.producer_target
                or generated.get("producer_artifact") != expected_producer_path
                or generated.get("producer_sha256") != producer_artifact.get("sha256")
            ):
                _fail(
                    f"{project_id} utility {target_name} consumer {consumer_target} "
                    "is not byte/owner joined"
                )
        rows.append(
            {
                "project_id": project_id,
                "target": target_name,
                "primary_output_relative_path": contract.primary_output.as_posix(),
                "generated_output_sha256": utility_artifact.get("sha256"),
                "output_inventory_sha256": _text(
                    _object(
                        utility.get("output_inventory"),
                        "utility output inventory",
                    ).get("entries_sha256"),
                    "utility output inventory digest",
                ),
                "producer_target": contract.producer_target,
                "producer_artifact_relative_path": contract.producer_artifact.as_posix(),
                "producer_artifact_sha256": producer_artifact.get("sha256"),
                "consumer_relationships": [
                    {"target": target, "kind": kind}
                    for target, kind in contract.consumer_relationships
                ],
            }
        )
    return rows


def _binding_dependency_evidence(
    *,
    build: _Build,
    module_proof: Mapping[str, Any],
    sources: Sequence[Path],
    required_header: Path,
    pybind11_include_root: Path,
    pybind11_required_headers: Sequence[tuple[Path, str]],
    roots: _Roots,
) -> list[dict[str, Any]]:
    by_source: dict[Path, list[_CompileRecord]] = {}
    for record in build.compile_commands.values():
        by_source.setdefault(record.source, []).append(record)
    command_by_source: dict[str, list[Mapping[str, Any]]] = {}
    for raw_command in _list(
        module_proof.get("commands"), "binding module proof commands"
    ):
        command = _object(raw_command, "binding module proof command")
        source = command.get("source")
        if source is None:
            continue
        source_path = _text(
            _object(source, "binding proof command source").get("path"),
            "binding proof command source path",
        )
        command_by_source.setdefault(source_path, []).append(command)

    def include_directories(record: _CompileRecord) -> list[Path]:
        result: list[Path] = []
        index = 1
        while index < len(record.argv):
            token = record.argv[index]
            if token in {"-I", "-isystem", "-iquote", "-idirafter"}:
                if index + 1 >= len(record.argv):
                    _fail("binding compiler include option is dangling")
                result.append(
                    _validated_compile_directory(
                        record.argv[index + 1],
                        cwd=record.directory,
                        build_root=build.root,
                        roots=roots,
                        label="binding compiler include directory",
                    )
                )
                index += 2
                continue
            joined = next(
                (
                    prefix
                    for prefix in ("-isystem", "-iquote", "-idirafter", "-I")
                    if token.startswith(prefix) and len(token) > len(prefix)
                ),
                None,
            )
            if joined is not None:
                result.append(
                    _validated_compile_directory(
                        token[len(joined) :],
                        cwd=record.directory,
                        build_root=build.root,
                        roots=roots,
                        label="binding compiler joined include directory",
                    )
                )
            index += 1
        return result

    rows: list[dict[str, Any]] = []
    normalized_header = Path(os.path.normpath(required_header))
    header_file = _regular_file(
        required_header,
        "binding registration header",
        contained_by=roots.repository,
    )
    for source in sorted(sources, key=roots.logical):
        records = by_source.get(Path(os.path.normpath(source)), [])
        if len(records) != 1:
            _fail(
                f"binding registration source {roots.logical(source)} does not have "
                "exactly one compile-database object"
            )
        record = records[0]
        includes = include_directories(record)
        if includes.count(pybind11_include_root) != 1:
            _fail(
                f"binding registration source {roots.logical(source)} does not use "
                "the exact pinned pybind11 include root once"
            )
        commands = command_by_source.get(roots.logical(source), [])
        if len(commands) != 1:
            _fail(
                f"binding registration source {roots.logical(source)} does not have "
                "one complete module-proof compile command"
            )
        command = commands[0]
        closure = _object(
            command.get("dependency_closure"),
            "binding registration complete dependency closure",
        )
        files = [
            _object(item, "binding registration dependency")
            for item in _list(
                closure.get("files"), "binding registration dependency files"
            )
        ]
        file_index = {
            _text(item.get("path"), "binding registration dependency path"): item
            for item in files
        }
        if (
            len(file_index) != len(files)
            or closure.get("object") != roots.logical(record.output)
            or closure.get("declared_dependency_count") != len(files)
        ):
            _fail("binding registration dependency closure shape/count drifted")
        registration_row = file_index.get(roots.logical(normalized_header))
        if (
            registration_row is None
            or registration_row.get("sha256") != _sha256_file(header_file)
            or registration_row.get("scope") != "repository_source"
        ):
            _fail(
                f"binding registration object {roots.logical(record.output)} lacks "
                "one authenticated "
                "binding_registrations.hpp dependency"
            )
        public_rows: list[dict[str, str]] = []
        for pybind_header, expected_digest in pybind11_required_headers:
            dependency = file_index.get(roots.logical(pybind_header))
            if (
                dependency is None
                or dependency.get("resolved_path") != roots.logical(pybind_header)
                or dependency.get("sha256") != expected_digest
                or dependency.get("scope") != "pinned_container"
            ):
                _fail(
                    f"binding registration source {roots.logical(source)} does not "
                    "compile through the exact pinned pybind11 public header"
                )
            public_rows.append(
                {"path": roots.logical(pybind_header), "sha256": expected_digest}
            )
        pybind_closure = [
            item
            for item in files
            if Path(_text(item.get("path"), "pybind11 closure path")).is_relative_to(
                pybind11_include_root
            )
        ]
        if not pybind_closure or any(
            item.get("scope") != "pinned_container" for item in pybind_closure
        ):
            _fail("binding pybind11 header closure is empty or not container-pinned")
        rows.append(
            {
                "source": roots.logical(source),
                "object": roots.logical(record.output),
                "dependency_count": len(files),
                "registration_header": {
                    "path": roots.logical(normalized_header),
                    "sha256": _sha256_file(header_file),
                },
                "pybind11": {
                    "version": "3.0.3",
                    "include_root": pybind11_include_root.as_posix(),
                    "required_public_headers": public_rows,
                    "header_closure_count": len(pybind_closure),
                    "header_closure_sha256": _canonical_sha256(pybind_closure),
                },
                "raw_sha256": _text(
                    closure.get("raw_sha256"),
                    "binding registration dependency raw digest",
                ),
            }
        )
    return rows


def _binding_ros_free_evidence(
    *,
    build: _Build,
    target_name: str,
    proof_rows: Sequence[Mapping[str, Any]],
    elf: Mapping[str, Any],
    forbidden_tokens: Sequence[str],
    roots: _Roots,
    responses: _ResponseCollector,
) -> dict[str, Any]:
    scanned: list[str] = []
    for record in build.compile_commands.values():
        if record.source not in build.targets[target_name].sources:
            continue
        scanned.extend(record.argv)
        scanned.append(roots.logical(record.source))
    commands_text = _run_text(
        ["/usr/bin/ninja", "-C", str(build.root), "-t", "commands", target_name],
        build.root,
        f"binding ROS-free Ninja commands {target_name}",
    )
    commands = _parse_ninja_commands(
        commands_text,
        build,
        roots,
        responses,
        f"binding ROS-free target {target_name}",
    )
    for command in commands.values():
        scanned.extend(command.argv)
        scanned.extend(roots.logical(path) for path in command.inputs)
        if command.source is not None:
            scanned.append(roots.logical(command.source))
    for raw in proof_rows:
        row = _object(raw, "binding ROS-free proof")
        for source in _list(row.get("file_api_sources"), "binding File API sources"):
            scanned.append(
                _text(_object(source, "binding File API source").get("path"), "binding source path")
            )
        for query in _list(row.get("ninja_query"), "binding Ninja query"):
            for edge in _list(_object(query, "binding query row").get("edges"), "binding query edges"):
                scanned.append(
                    _text(_object(edge, "binding query edge").get("path"), "binding query path")
                )
    elf_row = _object(elf, "binding ELF evidence")
    scanned.extend(_text(value, "binding ELF NEEDED") for value in _list(elf_row.get("needed"), "binding ELF NEEDED"))
    for dependency in _list(elf_row.get("dependencies"), "binding ELF dependencies"):
        dep = _object(dependency, "binding ELF dependency")
        scanned.extend(
            (
                _text(dep.get("soname"), "binding ELF soname"),
                _text(dep.get("path"), "binding ELF dependency path"),
            )
        )
    elf_path = roots.decode(
        _text(elf_row.get("path"), "binding ELF path"),
        roots.output,
        "binding ELF path",
    )
    symbol_text = _run_text(
        ["/usr/bin/readelf", "-Ws", str(elf_path)],
        elf_path.parent,
        "binding ROS-free ELF symbol scan",
    )
    scanned.extend(line for line in symbol_text.splitlines() if line)
    patterns = []
    for token in forbidden_tokens:
        if token == "ros":
            expression = r"(?<![a-z0-9])ros(?:2)?(?![a-z0-9])"
        elif token == "rcl":
            expression = r"(?<![a-z0-9])rcl(?:cpp)?(?![a-z0-9])"
        else:
            expression = rf"(?<![a-z0-9]){re.escape(token)}(?![a-z0-9])"
        patterns.append(re.compile(expression, re.IGNORECASE))
    matches = sorted(
        {
            value
            for value in scanned
            if any(pattern.search(value) for pattern in patterns)
        }
    )
    if matches:
        _fail(f"manyforge_core_py build closure contains ROS evidence: {matches}")
    return {
        "scanned_value_count": len(scanned),
        "normalized_values_sha256": _canonical_sha256(sorted(scanned)),
        "forbidden_match_count": 0,
    }


def validate_production_build_provenance(
    repository_root: str | os.PathLike[str],
    output_root: str | os.PathLike[str],
    lane: Mapping[str, Any],
    empty_ctest_inventory: Mapping[str, Any],
    target_contract: Mapping[str, Any],
) -> dict[str, Any]:
    """Prove an exact BUILD_TESTING=OFF Release build without executing it."""

    roots = _Roots(
        repository=_directory(Path(repository_root), "repository root"),
        output=_directory(Path(output_root), "qualification output root"),
    )
    lane = _object(lane, "production lane")
    inventory = _object(empty_ctest_inventory, "production CTest inventory")
    tests = _list(inventory.get("tests"), "production CTest inventory tests")
    if tests:
        _fail("BUILD_TESTING=OFF production build registered tests")
    lane_id = _text(lane.get("lane_id"), "production lane_id")
    project_id = _text(lane.get("project_id"), "production project_id")
    build_relative = _safe_relative(lane.get("build_root"), "production build_root")
    provenance_relative = _safe_relative(
        lane.get("provenance_output"), "production provenance_output"
    )
    build_contract = _object(target_contract, "production target contract")
    toolchain = _toolchain_contract(build_contract)
    exclusions = _production_exclusions(build_contract)
    contract = _production_contract(
        build_contract,
        expected_project=project_id,
        repository_root=roots.repository,
    )
    utility_contracts = _utility_target_contracts(
        build_contract,
        repository_root=roots.repository,
    )
    configure_generated_contracts = _configure_generated_input_contracts(
        build_contract,
        repository_root=roots.repository,
    )
    responses = _ResponseCollector(roots)
    build = _load_build(
        lane_id=lane_id,
        project_id=project_id,
        build_root=roots.output / build_relative,
        expected_source_root=contract.source_root,
        expected_configuration="Release",
        expected_build_testing=False,
        roots=roots,
        responses=responses,
        toolchain_contract=toolchain,
        build_provenance_contract=build_contract,
        configure_generated_contracts=configure_generated_contracts,
        retain_configure_fixture_replay=True,
    )
    production_parity_aliases = _assign_parity_path_aliases(
        builds=[build], install_roots={}, roots=roots
    )
    configured_targets = set(build.targets)
    expected_material = {
        name
        for name, spec in contract.targets.items()
        if spec.target_type in _MATERIAL_TARGET_TYPES
    }
    if configured_targets != set(contract.targets):
        _fail(
            f"{lane_id} production configured target set drifted: "
            f"expected={sorted(contract.targets)}, actual={sorted(configured_targets)}"
        )
    if _material_target_names(build) != expected_material:
        _fail(f"{lane_id} production File API material target/artifact set drifted")
    rows: list[dict[str, Any]] = []
    elf_rows: list[dict[str, Any]] = []
    for name in sorted(contract.targets):
        spec = contract.targets[name]
        proof = _prove_spec(
            build=build,
            spec=spec,
            role="production",
            profile="none",
            roots=roots,
            responses=responses,
            utility_contract=utility_contracts.get((project_id, name)),
        )
        if spec.target_type in {"EXECUTABLE", "SHARED_LIBRARY", "MODULE_LIBRARY"}:
            proof["elf"] = _elf_evidence(
                build.root / spec.artifact,
                profile="none",
                environment={},
                roots=roots,
                strict_runtime=spec.target_type == "EXECUTABLE",
                require_executable=spec.target_type == "EXECUTABLE",
            )
            elf_rows.append(proof["elf"])
        rows.append(proof)
    _require_utility_generator_owners(rows)
    utility_relationships = _join_utility_relationships(
        project_id=project_id,
        proofs=rows,
        contracts=utility_contracts,
        require_complete_project=True,
    )
    _bind_output_dependency_owners(
        proofs=rows,
        target_owners=rows,
        installed_owners=[],
        roots=roots,
    )
    _require_production_role_closure(
        rows,
        _production_role_catalog(
            build_contract,
            repository_root=roots.repository,
        ),
    )
    _require_production_test_free(rows, exclusions)
    _require_bound_configure_inputs([build])
    _validate_local_runtime_dependencies(elf_rows, rows, [])
    evidence: dict[str, Any] = {
        "schema_version": PRODUCTION_SCHEMA_VERSION,
        "lane_id": lane_id,
        "project_id": project_id,
        "build_root": roots.logical(build.root),
        "provenance_output": roots.logical(roots.output / provenance_relative),
        "empty_ctest_inventory_sha256": _canonical_sha256(inventory),
        "project": build.evidence,
        "targets": rows,
        "utility_target_relationships": utility_relationships,
        "production_parity": _parity_projection(
            rows, aliases=production_parity_aliases, roots=roots
        ),
        "response_files": responses.evidence(),
        "inspection_tools": _inspection_tool_evidence(),
        "passed": True,
    }
    evidence["evidence_sha256"] = _canonical_sha256(evidence)
    return evidence


def validate_binding_build_provenance(
    repository_root: str | os.PathLike[str],
    output_root: str | os.PathLike[str],
    lane: Mapping[str, Any],
    staged_extension: str | os.PathLike[str],
    binding_contract: Mapping[str, Any],
) -> dict[str, Any]:
    """Prove the exact pybind11 module and every output-owned archive input."""

    roots = _Roots(
        repository=_directory(Path(repository_root), "repository root"),
        output=_directory(Path(output_root), "qualification output root"),
    )
    lane = _object(lane, "binding build lane")
    allowed_lane_keys = {
        "lane_id",
        "build_root",
        "core_build_root",
        "deployment_build_root",
        "module_build_root",
        "stage_root",
        "generated_extension",
        "provenance_output",
        "sanitizer",
    }
    if set(lane) != allowed_lane_keys:
        _fail("binding build lane keys drifted")
    whole_build_contract = _object(
        binding_contract, "binding build-provenance contract"
    )
    toolchain = _toolchain_contract(whole_build_contract)
    exclusions = _production_exclusions(whole_build_contract)
    configure_generated_contracts = _configure_generated_input_contracts(
        whole_build_contract,
        repository_root=roots.repository,
    )
    utility_contracts = _utility_target_contracts(
        whole_build_contract,
        repository_root=roots.repository,
    )
    contract = _binding_build_contract(
        whole_build_contract,
        repository_root=roots.repository,
    )
    lane_id = _text(lane.get("lane_id"), "binding lane_id")
    if lane_id not in contract.lane_configurations:
        _fail(f"binding build contract does not contain lane {lane_id}")
    configuration, profile = contract.lane_configurations[lane_id]
    if lane.get("sanitizer") != profile:
        _fail(f"binding lane {lane_id} sanitizer differs from its build contract")

    relative_paths = {
        key: _safe_relative(lane.get(key), f"binding lane {key}")
        for key in (
            "build_root",
            "core_build_root",
            "deployment_build_root",
            "module_build_root",
            "stage_root",
            "provenance_output",
        )
    }
    overall_build = _directory(
        roots.output / relative_paths["build_root"], "binding overall build root"
    )
    sub_builds = {
        "manyforge_core": roots.output / relative_paths["core_build_root"],
        "manyforge_deployment": roots.output
        / relative_paths["deployment_build_root"],
        "manyforge_core_py": roots.output / relative_paths["module_build_root"],
    }
    if len(set(sub_builds.values())) != len(sub_builds) or any(
        not _is_within(path, overall_build) or path == overall_build
        for path in sub_builds.values()
    ):
        _fail("binding project build roots do not form exact distinct sub-builds")
    responses = _ResponseCollector(roots)

    dependency_builds: dict[str, _Build] = {}
    dependency_rows: list[dict[str, Any]] = []
    dependency_elf_rows: list[dict[str, Any]] = []
    project_rows: list[dict[str, Any]] = []
    for project_id in sorted(contract.dependency_projects):
        project_contract = contract.dependency_projects[project_id]
        build = _load_build(
            lane_id=lane_id,
            project_id=project_id,
            build_root=sub_builds[project_id],
            expected_source_root=project_contract.source_root,
            expected_configuration=configuration,
            expected_build_testing=False,
            roots=roots,
            responses=responses,
            toolchain_contract=toolchain,
            build_provenance_contract=whole_build_contract,
            configure_generated_contracts=configure_generated_contracts,
            retain_configure_fixture_replay=True,
            build_testing_cache_key=project_contract.build_testing_cache_key,
        )
        dependency_builds[project_id] = build
        project_rows.append(build.evidence)
        _assign_parity_path_aliases(builds=[build], install_roots={}, roots=roots)
        configured_targets = set(build.targets)
        if configured_targets != set(project_contract.production_targets):
            _fail(
                f"binding dependency {project_id} configured target set drifted: "
                f"expected={sorted(project_contract.production_targets)}, "
                f"actual={sorted(configured_targets)}"
            )
        expected_material = {
            name
            for name, spec in project_contract.production_targets.items()
            if spec.target_type in _MATERIAL_TARGET_TYPES
        }
        if _material_target_names(build) != expected_material:
            _fail(f"binding dependency {project_id} material target set drifted")
        for target_name in sorted(project_contract.production_targets):
            spec = project_contract.production_targets[target_name]
            proof = _prove_spec(
                build=build,
                spec=spec,
                role="production_dependency",
                profile=profile,
                roots=roots,
                responses=responses,
                utility_contract=utility_contracts.get((project_id, target_name)),
            )
            if spec.target_type in {
                "EXECUTABLE",
                "SHARED_LIBRARY",
                "MODULE_LIBRARY",
            }:
                proof["elf"] = _elf_evidence(
                    build.root / spec.artifact,
                    profile=profile,
                    environment={},
                    roots=roots,
                    strict_runtime=spec.target_type == "EXECUTABLE",
                    require_executable=spec.target_type == "EXECUTABLE",
                )
                dependency_elf_rows.append(proof["elf"])
            dependency_rows.append(proof)

    _require_utility_generator_owners(dependency_rows)
    dependency_utility_relationships: list[dict[str, Any]] = []
    for project_id in sorted(contract.dependency_projects):
        dependency_utility_relationships.extend(
            _join_utility_relationships(
                project_id=project_id,
                proofs=[
                    row
                    for row in dependency_rows
                    if row.get("project_id") == project_id
                ],
                contracts=utility_contracts,
                require_complete_project=True,
            )
        )

    module_build = _load_build(
        lane_id=lane_id,
        project_id="manyforge_core_py",
        build_root=sub_builds["manyforge_core_py"],
        expected_source_root=contract.source_root,
        expected_configuration=configuration,
        expected_build_testing=False,
        roots=roots,
        responses=responses,
        toolchain_contract=toolchain,
        build_provenance_contract=whole_build_contract,
        configure_generated_contracts=configure_generated_contracts,
        retain_configure_fixture_replay=True,
    )
    binding_parity_aliases = _assign_parity_path_aliases(
        builds=[*dependency_builds.values(), module_build],
        install_roots={},
        roots=roots,
    )
    project_rows.append(module_build.evidence)
    configured_module_targets = set(module_build.targets)
    if configured_module_targets != {"manyforge_core_py"} or _material_target_names(
        module_build
    ) != {"manyforge_core_py"}:
        _fail(
            "binding module build configured/material target set drifted: "
            f"actual={sorted(configured_module_targets)}"
        )
    if contract.forbidden_modules.intersection(module_build.targets):
        _fail("binding module build contains a forbidden module/evaluation target")
    module_spec = _ProductTargetSpec(
        name="manyforge_core_py",
        target_type="MODULE_LIBRARY",
        artifact=contract.artifact,
        required_sources=contract.registration_sources,
        required_generated_sources=(),
    )
    module_proof = _prove_spec(
        build=module_build,
        spec=module_spec,
        role="binding_module",
        profile=profile,
        roots=roots,
        responses=responses,
    )
    all_proofs = dependency_rows + [module_proof]
    _bind_output_dependency_owners(
        proofs=all_proofs,
        target_owners=dependency_rows,
        installed_owners=[],
        roots=roots,
    )
    _require_production_role_closure(
        all_proofs,
        _production_role_catalog(
            whole_build_contract,
            repository_root=roots.repository,
        )
        | frozenset({("manyforge_core_py", "manyforge_core_py")}),
    )
    _require_production_test_free(all_proofs, exclusions)
    _require_bound_configure_inputs([*dependency_builds.values(), module_build])
    bound_owner_values = {
        (
            _text(
                _object(
                    _object(item, "binding authenticated dependency").get("owner"),
                    "binding authenticated dependency owner",
                ).get("project_id"),
                "binding authenticated dependency owner project",
            ),
            _text(
                _object(
                    _object(item, "binding authenticated dependency").get("owner"),
                    "binding authenticated dependency owner",
                ).get("target"),
                "binding authenticated dependency owner target",
            ),
        )
        for item in _list(
            module_proof.get("authenticated_output_dependencies"),
            "binding authenticated output dependencies",
        )
    }
    if bound_owner_values != contract.module_linked_archive_owners:
        _fail(
            "manyforge_core_py linked archive owner set drifted: "
            f"expected={sorted(contract.module_linked_archive_owners)}, "
            f"actual={sorted(bound_owner_values)}"
        )
    graph_owner_aliases = {
        _text(
            _object(row, "binding configured graph owner alias").get("alias"),
            "binding configured graph owner alias",
        )
        for row in _list(
            _object(
                module_build.evidence.get("configured_target_graph"),
                "binding configured target graph",
            ).get("owner_node_aliases"),
            "binding configured graph owner aliases",
        )
    }
    expected_graph_owner_aliases = {
        f"<owner:{project_id}/{target}>"
        for project_id, target in contract.module_linked_archive_owners
    }
    if graph_owner_aliases != expected_graph_owner_aliases:
        _fail("manyforge_core_py configured graph owner set drifted")
    module_linked_archive_owners = sorted(
        [
            dict(
                _object(
                    _object(item, "binding authenticated dependency").get("owner"),
                    "binding authenticated dependency owner",
                )
            )
            for item in _list(
                module_proof.get("authenticated_output_dependencies"),
                "binding authenticated output dependencies",
            )
        ],
        key=lambda owner: (owner["project_id"], owner["target"]),
    )

    module_artifact = module_build.root / contract.artifact
    stage_root = _directory(
        roots.output / relative_paths["stage_root"], "binding stage root"
    )
    supplied_text = os.fspath(staged_extension)
    if not isinstance(supplied_text, str) or not supplied_text:
        _fail("staged_extension is not one path")
    supplied_path = roots.decode(
        supplied_text, roots.output, "staged_extension"
    )
    expected_staged = stage_root / contract.artifact.name
    if Path(os.path.normpath(supplied_path)) != Path(os.path.normpath(expected_staged)):
        _fail("staged_extension differs from the binding lane stage contract")
    staged = _regular_file(
        expected_staged,
        "staged manyforge_core_py extension",
        contained_by=stage_root,
    )
    if sorted(stage_root.iterdir(), key=lambda path: path.name) != [expected_staged]:
        _fail("binding stage root is not extension-only")
    built = _regular_file(
        module_artifact,
        "File API manyforge_core_py artifact",
        contained_by=module_build.root,
    )
    built_digest = _sha256_file(built)
    staged_digest = _sha256_file(staged)
    if built_digest != staged_digest:
        _fail("staged manyforge_core_py differs from its File API/Ninja artifact")

    generated_value = lane.get("generated_extension")
    generated_row: dict[str, str] | None = None
    if generated_value is None:
        if profile == "none":
            _fail("unsanitized binding lane lacks its generated extension copy")
    else:
        if profile != "none":
            _fail("sanitized binding lane unexpectedly publishes a generated copy")
        generated_relative = _safe_relative(
            generated_value, "binding generated_extension"
        )
        generated = _regular_file(
            roots.output / generated_relative,
            "generated manyforge_core_py extension",
            contained_by=roots.output,
        )
        generated_digest = _sha256_file(generated)
        if generated.name != contract.artifact.name or generated_digest != built_digest:
            _fail("generated manyforge_core_py is not the byte-identical module artifact")
        generated_row = {
            "path": roots.logical(roots.output / generated_relative),
            "resolved_path": roots.logical(generated),
            "sha256": generated_digest,
        }

    header_evidence = _binding_dependency_evidence(
        build=module_build,
        module_proof=module_proof,
        sources=contract.registration_sources,
        required_header=contract.registration_header,
        pybind11_include_root=contract.pybind11_include_root,
        pybind11_required_headers=contract.pybind11_required_headers,
        roots=roots,
    )
    elf = _elf_evidence(
        expected_staged,
        profile=profile,
        environment={},
        roots=roots,
        strict_runtime=True,
        require_executable=False,
    )
    if elf["python_init_symbols"] != ["PyInit_manyforge_core_py"]:
        _fail("manyforge_core_py exports the wrong PyInit symbol set")
    if any(value.startswith("libpython") for value in elf["needed"]):
        _fail("manyforge_core_py unexpectedly has a libpython DT_NEEDED entry")
    ros_free = _binding_ros_free_evidence(
        build=module_build,
        target_name="manyforge_core_py",
        proof_rows=all_proofs,
        elf=elf,
        forbidden_tokens=contract.forbidden_dependency_tokens,
        roots=roots,
        responses=responses,
    )
    _validate_local_runtime_dependencies(
        [elf, *dependency_elf_rows], dependency_rows, []
    )

    target_projection = _parity_projection(
        dependency_rows + [module_proof],
        aliases=binding_parity_aliases,
        roots=roots,
    )
    deterministic_parity = {
        "configuration": configuration,
        "sanitizer": profile,
        "artifact_sha256": built_digest,
        "build_recipe": {
            "path": roots.logical(contract.build_contract),
            "sha256": _sha256_file(contract.build_contract),
        },
        "target_projection": target_projection,
        "registration_header": {
            "path": roots.logical(contract.registration_header),
            "sha256": _sha256_file(contract.registration_header),
        },
    }
    evidence: dict[str, Any] = {
        "schema_version": BINDING_SCHEMA_VERSION,
        "lane_id": lane_id,
        "configuration": configuration,
        "sanitizer": profile,
        "build_root": roots.logical(overall_build),
        "provenance_output": roots.logical(
            roots.output / relative_paths["provenance_output"]
        ),
        "projects": sorted(project_rows, key=lambda row: row["project_id"]),
        "module_target": module_proof,
        "dependency_targets": sorted(
            dependency_rows, key=lambda row: (row["project_id"], row["target"])
        ),
        "dependency_utility_target_relationships": sorted(
            dependency_utility_relationships,
            key=lambda row: (row["project_id"], row["target"]),
        ),
        "module_linked_archive_owners": module_linked_archive_owners,
        "staged_extension": {
            "path": roots.logical(expected_staged),
            "resolved_path": roots.logical(staged),
            "sha256": staged_digest,
        },
        "generated_extension": generated_row,
        "registration_header_dependencies": header_evidence,
        "elf": elf,
        "ros_free_closure": ros_free,
        "deterministic_parity": deterministic_parity,
        "response_files": responses.evidence(),
        "inspection_tools": _inspection_tool_evidence(),
        "passed": True,
    }
    evidence["evidence_sha256"] = _canonical_sha256(evidence)
    return evidence


def validate_native_build_provenance(
    repository_root: str | os.PathLike[str],
    output_root: str | os.PathLike[str],
    lane: Mapping[str, Any],
    inventory: Mapping[str, Any],
    target_group_mapping: Mapping[str, Any],
    ordinary_mapping: Mapping[str, Any],
    production_target_contract: Mapping[str, Any],
    retain_configure_fixture_replay: bool,
) -> dict[str, Any]:
    """Validate one complete native lane and return deterministic JSON evidence.

    The caller invokes this against one unchanged CTest JSON-v1 inventory both
    before and after the native test processes.  ``target_group_mapping`` may
    be either the selected group's ``target -> (count, names_sha256)`` mapping
    or the outer ``group -> mapping`` object.  ``ordinary_mapping`` is the
    decoded packet contract ``name -> (container_path, baseline_sha256,
    candidate_sha256)``.

    ``production_target_contract`` is the packet-owned exact product/test-only
    target partition for this CMake project.  It is required so a green test
    executable cannot substitute for a missing production artifact.

    Configure-executed fixtures are replayed into one initially absent,
    canonical build-local tree.  A pre-execution caller sets
    ``retain_configure_fixture_replay`` false so the fully authenticated tree
    is removed before native tests; post-execution revalidation sets it true
    so the byte-identical replay remains available to the host verifier.  Any
    incomplete, ambiguous, or unsupported provenance raises
    :class:`NativeBuildProvenanceError`.
    """

    roots = _Roots(
        repository=_directory(Path(repository_root), "repository root"),
        output=_directory(Path(output_root), "qualification output root"),
    )
    lane = _object(lane, "native lane")
    inventory = _object(inventory, "CTest inventory")
    target_group_mapping = _object(target_group_mapping, "native target-group mapping")
    ordinary_mapping = _object(ordinary_mapping, "ordinary source mapping")
    production_target_contract = _object(
        production_target_contract, "production target contract"
    )
    if not isinstance(retain_configure_fixture_replay, bool):
        _fail("native configure fixture replay-retention policy drifted")
    toolchain = _toolchain_contract(production_target_contract)
    exclusions = _production_exclusions(production_target_contract)
    lane_id = _text(lane.get("lane_id"), "lane.lane_id")
    group = _text(lane.get("required_group"), "lane.required_group")
    if group not in _PACKAGE_BY_GROUP:
        _fail(f"{lane_id} has unknown required group {group}")
    profile = _text(lane.get("sanitizer"), "lane.sanitizer")
    if profile not in {"none", "address+undefined", "thread"}:
        _fail(f"{lane_id} has unknown sanitizer profile {profile}")
    build_relative = _safe_relative(lane.get("build_root"), "lane.build_root")
    provenance_relative = _safe_relative(
        lane.get("provenance_output"), "lane.provenance_output"
    )
    build_root = roots.output / build_relative
    package = _PACKAGE_BY_GROUP[group]
    package_root = roots.repository / package
    product_contract = _production_contract(
        production_target_contract,
        expected_project=package,
        repository_root=roots.repository,
    )
    utility_contracts = _utility_target_contracts(
        production_target_contract,
        repository_root=roots.repository,
    )
    auxiliary_install_contracts = _auxiliary_install_contracts(
        production_target_contract
    )
    configure_generated_contracts = _configure_generated_input_contracts(
        production_target_contract,
        repository_root=roots.repository,
    )
    lane_partition = _native_lane_partition(
        production_target_contract,
        lane_id=lane_id,
        contract=product_contract,
    )
    if product_contract.source_root != package_root.resolve(strict=True):
        _fail(f"{lane_id} production target contract has the wrong source root")
    expected_configuration = "Release" if profile == "none" else "RelWithDebInfo"
    responses = _ResponseCollector(roots)
    selected_targets = _target_mapping(target_group_mapping, group)
    ordinary_contract = _ordinary_mapping(ordinary_mapping)
    if not _ORDINARY_BY_GROUP[group].issubset(ordinary_contract):
        _fail(f"ordinary mapping lacks entries required by {group}")

    primary = _load_build(
        lane_id=lane_id,
        project_id=package,
        build_root=build_root,
        expected_source_root=package_root,
        expected_configuration=expected_configuration,
        expected_build_testing=True,
        roots=roots,
        responses=responses,
        toolchain_contract=toolchain,
        build_provenance_contract=production_target_contract,
        configure_generated_contracts=configure_generated_contracts,
        retain_configure_fixture_replay=retain_configure_fixture_replay,
    )
    registry_targets, registry_ordinary = _decode_inventory(
        lane=lane,
        inventory=inventory,
        target_contract=selected_targets,
        ordinary_contract=ordinary_contract,
        build_root=primary.root,
        roots=roots,
    )

    configured_targets = set(primary.targets)
    _validate_native_configured_file_api_target_set(
        configured_targets,
        _object(
            primary.evidence.get("configured_target_graph"),
            f"{lane_id} configured-target graph",
        ),
        lane_id,
    )

    existing_material = {
        name
        for name in _material_target_names(primary)
        if all(path.exists() for path in primary.targets[name].artifacts)
    }
    required_material = {
        name
        for name in lane_partition.production_targets
        if product_contract.targets[name].target_type in _MATERIAL_TARGET_TYPES
    } | {
        name
        for name in lane_partition.test_only_targets
        if product_contract.test_only_targets[name].target_type
        in _MATERIAL_TARGET_TYPES
    } | set(registry_targets)
    _validate_built_material_target_partition(
        existing_material,
        required_material,
        lane_id,
    )
    all_specs = {
        **product_contract.targets,
        **product_contract.test_only_targets,
    }
    existing_utility = {
        name
        for name, target in primary.targets.items()
        if target.target_type == "UTILITY"
        and name in all_specs
        and (primary.root / all_specs[name].artifact).is_file()
    }
    required_utility = {
        name
        for name in lane_partition.production_targets
        if product_contract.targets[name].target_type == "UTILITY"
    } | {
        name
        for name in lane_partition.test_only_targets
        if product_contract.test_only_targets[name].target_type == "UTILITY"
    }
    if existing_utility != required_utility:
        _fail(
            f"{lane_id} built UTILITY-target partition drifted: "
            f"expected={sorted(required_utility)}, actual={sorted(existing_utility)}"
        )
    expected_helpers = {
        _HELPERS[name][1] for name in _ORDINARY_BY_GROUP[group] if name in _HELPERS
    }
    if not expected_helpers.issubset(lane_partition.production_targets):
        _fail(
            f"{lane_id} ordinary helpers are absent from its production target partition"
        )

    project_rows: list[dict[str, Any]] = [primary.evidence]
    auxiliary_builds: list[_Build] = []
    install_roots: dict[str, Path] = {}
    install_states: dict[str, _AuxiliaryInstallState] = {}
    qualified_install_rows: list[dict[str, Any]] = []
    install_inputs: dict[Path, dict[str, Any]] = {}
    if group == "roboplan_scene_sanitizer_required":
        if profile == "none":
            _fail("RoboPlan scene sanitizer group cannot use the Release profile")
        sanitizer_root = roots.output / build_relative.parents[1]
        for project_id in ("roboplan_example_models", "roboplan"):
            auxiliary = _load_build(
                lane_id=lane_id,
                project_id=project_id,
                build_root=sanitizer_root / "build" / project_id,
                expected_source_root=sanitizer_root / "source" / project_id,
                expected_configuration="RelWithDebInfo",
                expected_build_testing=False,
                roots=roots,
                responses=responses,
                toolchain_contract=toolchain,
                build_provenance_contract=production_target_contract,
                configure_generated_contracts=configure_generated_contracts,
                retain_configure_fixture_replay=retain_configure_fixture_replay,
            )
            auxiliary_builds.append(auxiliary)
            project_rows.append(auxiliary.evidence)
            install_roots[project_id] = sanitizer_root / "install" / project_id
        install_states = _auxiliary_install_states(
            project_builds=auxiliary_builds,
            install_roots=install_roots,
            contracts=auxiliary_install_contracts,
            roots=roots,
        )
        install_inputs = _qualified_install_input_inventory(
            states=install_states,
            roots=roots,
        )
        qualified_install_rows = sorted(
            {row["path"]: row for row in install_inputs.values()}.values(),
            key=lambda row: (row["project_id"], row["path"]),
        )
        _attach_qualified_install_inputs(primary, install_inputs)
    native_parity_aliases = _assign_parity_path_aliases(
        builds=[primary, *auxiliary_builds],
        install_roots=install_roots,
        roots=roots,
    )
    target_rows: list[dict[str, Any]] = []
    elf_rows: list[dict[str, Any]] = []
    production_rows: list[dict[str, Any]] = []
    test_only_rows: list[dict[str, Any]] = []
    product_environment = next(iter(registry_targets.values()))["environment"]
    for target_name in sorted(lane_partition.production_targets):
        spec = product_contract.targets[target_name]
        proof = _prove_spec(
            build=primary,
            spec=spec,
            role="production",
            profile=profile,
            roots=roots,
            responses=responses,
            utility_contract=utility_contracts.get((package, target_name)),
        )
        if spec.target_type in {"EXECUTABLE", "SHARED_LIBRARY", "MODULE_LIBRARY"}:
            proof["elf"] = _elf_evidence(
                primary.root / spec.artifact,
                profile=profile,
                environment=product_environment,
                roots=roots,
                strict_runtime=spec.target_type == "EXECUTABLE",
                require_executable=spec.target_type == "EXECUTABLE",
            )
            elf_rows.append(proof["elf"])
        production_rows.append(proof)
    for target_name in sorted(lane_partition.test_only_targets):
        spec = product_contract.test_only_targets[target_name]
        test_only_rows.append(
            _prove_spec(
                build=primary,
                spec=spec,
                role="test_support",
                profile=profile,
                roots=roots,
                responses=responses,
                utility_contract=utility_contracts.get((package, target_name)),
            )
        )
    for target_name in sorted(registry_targets):
        registration = registry_targets[target_name]
        spec = _registered_gtest_spec(
            project_id=package,
            build_root=primary.root,
            target_name=target_name,
            registered_artifact=registration["artifact"],
            contract=product_contract,
            roots=roots,
        )
        proof = _prove_spec(
            build=primary,
            spec=spec,
            role="gtest",
            profile=profile,
            roots=roots,
            responses=responses,
            utility_contract=None,
        )
        proof["registered_test_count"] = len(registration["names"])
        proof["registered_test_names_sha256"] = _name_digest(registration["names"])
        proof["contracted_test_count"] = selected_targets[target_name][0]
        proof["contracted_test_names_sha256"] = selected_targets[target_name][1]
        proof["additional_test_count"] = (
            len(registration["names"]) - selected_targets[target_name][0]
        )
        proof["elf"] = _elf_evidence(
            registration["artifact"],
            profile=profile,
            environment=registration["environment"],
            roots=roots,
            strict_runtime=True,
            require_executable=True,
        )
        elf_rows.append(proof["elf"])
        target_rows.append(proof)

    ordinary_rows: list[dict[str, Any]] = []
    for name in sorted(registry_ordinary):
        row = registry_ordinary[name]
        evidence: dict[str, Any] = {
            "name": name,
            "source_path": roots.logical(row["source"]),
            "baseline_sha256": row["baseline_sha256"],
            "candidate_sha256": row["candidate_sha256"],
            "helper_target": None,
        }
        if name in _HELPERS:
            variable, helper_name, source_relative = _HELPERS[name]
            helper = _helper_artifact(
                row, variable, helper_name, primary.root, roots, f"ordinary test {name}"
            )
            helper_source = _regular_file(
                roots.repository / source_relative,
                f"ordinary helper source {helper_name}",
                contained_by=roots.repository,
            )
            helper_spec = product_contract.targets.get(helper_name)
            if (
                helper_spec is None
                or helper_spec.target_type != "EXECUTABLE"
                or helper_spec.required_sources != (helper_source,)
                or helper_spec.required_generated_sources
                or Path(os.path.normpath(primary.root / helper_spec.artifact))
                != Path(os.path.normpath(helper))
            ):
                _fail(
                    f"ordinary helper {helper_name} differs from its production "
                    "target contract"
                )
            helper_proof = _prove_spec(
                build=primary,
                spec=helper_spec,
                role="ordinary_helper",
                profile=profile,
                roots=roots,
                responses=responses,
                utility_contract=None,
            )
            helper_proof["elf"] = _elf_evidence(
                helper,
                profile=profile,
                environment=row["environment"],
                roots=roots,
                strict_runtime=True,
                require_executable=True,
            )
            elf_rows.append(helper_proof["elf"])
            target_rows.append(helper_proof)
            evidence["helper_target"] = {
                "project_id": primary.project_id,
                "target": helper_name,
                "artifact": roots.logical(helper),
            }
        ordinary_rows.append(evidence)

    auxiliary_target_rows: list[dict[str, Any]] = []
    if group == "roboplan_scene_sanitizer_required":
        for auxiliary in auxiliary_builds:
            auxiliary_contract = auxiliary_install_contracts[auxiliary.project_id]
            library_contracts = {
                item.target: item for item in auxiliary_contract.library_files
            }
            configured_auxiliary = set(auxiliary.targets)
            if configured_auxiliary != set(library_contracts):
                _fail(
                    f"{auxiliary.project_id} configured target set drifted: "
                    f"expected={sorted(library_contracts)}, "
                    f"actual={sorted(configured_auxiliary)}"
                )
            material = {
                name: target
                for name, target in auxiliary.targets.items()
                if target.target_type in _MATERIAL_TARGET_TYPES and target.artifacts
            }
            if set(material) != set(library_contracts):
                _fail(f"{auxiliary.project_id} lacks its required RoboPlan library target")
            for target_name in sorted(material):
                target = material[target_name]
                target_contract = library_contracts[target_name]
                expected_artifact = Path(
                    os.path.normpath(
                        auxiliary.root / target_contract.build_artifact_relative
                    )
                )
                if (
                    len(target.artifacts) != 1
                    or target.target_type != target_contract.target_type
                    or Path(os.path.normpath(target.artifacts[0]))
                    != expected_artifact
                ):
                    _fail(f"auxiliary target {target_name} has ambiguous artifacts")
                proof = _prove_target(
                    build=auxiliary,
                    target_name=target_name,
                    artifact=expected_artifact,
                    expected_sources={
                        auxiliary.source_root / relative
                        for relative in target_contract.required_sources
                    },
                    role="local_library",
                    profile=profile,
                    roots=roots,
                    responses=responses,
                )
                proof["contract_artifact"] = (
                    target_contract.build_artifact_relative.as_posix()
                )
                proof["required_generated_inputs"] = []
                if target.target_type in {"SHARED_LIBRARY", "MODULE_LIBRARY"}:
                    proof["elf"] = _elf_evidence(
                        target.artifacts[0],
                        profile=profile,
                        environment=next(iter(registry_targets.values()))["environment"],
                        roots=roots,
                        strict_runtime=False,
                        require_executable=False,
                    )
                    elf_rows.append(proof["elf"])
                auxiliary_target_rows.append(proof)
                target_rows.append(proof)

    installed_rows = _installed_copies(
        states=install_states,
        target_evidence=auxiliary_target_rows,
        roots=roots,
    ) if install_states else []
    if install_states:
        _finalize_auxiliary_install_owner_partitions(
            states=install_states,
            qualified_inputs=install_inputs,
            installed_copies=installed_rows,
        )
    primary_relationship_proofs = production_rows + test_only_rows
    proved_primary_targets = {
        _text(row.get("target"), "primary relationship target")
        for row in primary_relationship_proofs
    }
    for row in target_rows:
        if row.get("project_id") != package:
            continue
        target_name = _text(row.get("target"), "primary relationship target")
        if target_name not in proved_primary_targets:
            primary_relationship_proofs.append(row)
            proved_primary_targets.add(target_name)
    _require_utility_generator_owners(production_rows + test_only_rows)
    utility_relationships = _join_utility_relationships(
        project_id=package,
        proofs=primary_relationship_proofs,
        contracts=utility_contracts,
        require_complete_project=False,
    )
    all_target_rows = production_rows + test_only_rows + target_rows
    _bind_output_dependency_owners(
        proofs=all_target_rows,
        target_owners=all_target_rows,
        installed_owners=installed_rows,
        roots=roots,
    )
    _require_production_role_closure(
        production_rows,
        _production_role_catalog(
            production_target_contract,
            repository_root=roots.repository,
        ),
    )
    _require_production_test_free(production_rows, exclusions)
    _require_bound_configure_inputs([primary, *auxiliary_builds])
    _validate_local_runtime_dependencies(
        elf_rows, all_target_rows, installed_rows
    )

    external_dependencies: dict[tuple[str, str, str], dict[str, Any]] = {}
    for elf in elf_rows:
        for raw_dependency in elf["dependencies"]:
            dependency = _object(raw_dependency, "ELF dependency evidence")
            if dependency.get("scope") != "pinned_container":
                continue
            key = (
                dependency["soname"],
                dependency["resolved_path"],
                dependency["sha256"],
            )
            external_dependencies[key] = {
                "resolved_path": dependency["resolved_path"],
                "sha256": dependency["sha256"],
                "soname": dependency["soname"],
            }

    cross_profile_rows = (
        target_rows
        if group == "roboplan_scene_sanitizer_required"
        else production_rows
    )
    evidence: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "lane_id": lane_id,
        "required_group": group,
        "sanitizer": profile,
        "build_root": roots.logical(primary.root),
        "provenance_output": roots.logical(roots.output / provenance_relative),
        "projects": sorted(project_rows, key=lambda row: row["project_id"]),
        "targets": sorted(target_rows, key=lambda row: (row["project_id"], row["target"])),
        "production_targets": sorted(
            production_rows, key=lambda row: (row["project_id"], row["target"])
        ),
        "test_only_targets": sorted(
            test_only_rows, key=lambda row: (row["project_id"], row["target"])
        ),
        "production_parity": _parity_projection(
            production_rows, aliases=native_parity_aliases, roots=roots
        ),
        "test_only_parity": _parity_projection(
            test_only_rows, aliases=native_parity_aliases, roots=roots
        ),
        "execution_parity": _parity_projection(
            target_rows, aliases=native_parity_aliases, roots=roots
        ),
        "cross_profile_parity": _parity_projection(
            cross_profile_rows, aliases=native_parity_aliases, roots=roots
        ),
        "utility_target_relationships": utility_relationships,
        "ordinary_tests": ordinary_rows,
        "installed_copies": installed_rows,
        "qualified_install_inputs": qualified_install_rows,
        "install_projects": [
            install_states[project_id].evidence
            for project_id in sorted(install_states)
        ],
        "response_files": responses.evidence(),
        "external_dependencies": [
            external_dependencies[key] for key in sorted(external_dependencies)
        ],
        "inspection_tools": _inspection_tool_evidence(),
        "passed": True,
    }
    evidence["evidence_sha256"] = _canonical_sha256(evidence)
    return evidence


__all__ = [
    "BINDING_BUILD_CONTRACT_SCHEMA",
    "BINDING_SCHEMA_VERSION",
    "NATIVE_LANE_TARGET_PARTITION_SCHEMA",
    "NativeBuildProvenanceError",
    "ProvenanceError",
    "PRODUCTION_SCHEMA_VERSION",
    "PRODUCTION_TARGET_CONTRACT_SCHEMA",
    "SCHEMA_VERSION",
    "compile_dependency_cross_profile_sha256",
    "generated_output_cross_profile_sha256",
    "replay_git_source_generation",
    "replay_configured_target_graph",
    "validate_build_provenance_contract",
    "validate_binding_build_provenance",
    "validate_native_build_provenance",
    "validate_production_build_provenance",
]
