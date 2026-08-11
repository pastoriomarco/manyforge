#!/usr/bin/env python3
"""Generate exact target-scoped native Stop build and runner closures.

This tool reads CMake's File API after both material targets exist. It hashes
only the selected target closures, explicit generated semantic inputs, their
effective build descriptions, and the resulting executables. It deliberately
does not consume Git state, timestamps, hostnames, or whole-repository state.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
from pathlib import Path
from typing import Any


IMPLEMENTATION_API = "manyforge.stop_implementation_build_closure.v1"
RUNNER_API = "manyforge.stop_qualification_runner_contract.v1"
CASE_API = "manyforge.native_stop_qualification_case_contract.v1"
CASE_RUNNER_ID = "manyforge.native_stop_qualification_runner.v1"
EXPECTED_CASE_CONTRACT = {
    "controlled_substitution_cases": [
        "selected_before_physical_dispatch",
        "nominal_scope_preserved",
        "nominal_stage_budget_preserved",
        "requested_executed_reason_retained",
    ],
    "implementation_fault_cases": [
        "nominal_provider_call_blocked",
        "nominal_adapter_lock_blocked",
        "nominal_result_wait_blocked",
    ],
}
MAX_INPUTS = 4096


class ClosureError(RuntimeError):
    pass


def fail(message: str) -> None:
    raise ClosureError(message)


def strict_json(path: Path, label: str) -> Any:
    def unique(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                fail(f"{label} contains duplicate key {key}")
            result[key] = value
        return result

    try:
        return json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=unique,
            parse_constant=lambda token: fail(f"{label} contains {token}"),
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(f"{label} is not strict JSON: {error}")


def canonical_bytes(value: Any) -> bytes:
    try:
        return json.dumps(
            value,
            ensure_ascii=False,
            allow_nan=False,
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")
    except (TypeError, ValueError) as error:
        fail(f"value cannot be canonically encoded: {error}")


def digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def digest_file(path: Path, label: str) -> str:
    try:
        if not path.is_file() or path.is_symlink():
            fail(f"{label} is not one real regular file: {path}")
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            while chunk := stream.read(1024 * 1024):
                digest.update(chunk)
        return "sha256:" + digest.hexdigest()
    except OSError as error:
        fail(f"cannot hash {label}: {error}")


def framed_identity(document: dict[str, Any], domain: str, own_field: str) -> str:
    payload = {key: value for key, value in document.items() if key != own_field}
    preimage = (
        b"manyforge.content-identity.v1\0"
        + domain.encode("ascii")
        + b"\0"
        + str(document["api_version"]).encode("ascii")
        + b"\0"
        + canonical_bytes(payload)
    )
    return digest_bytes(preimage)


def reject_symlink_components(path: Path, label: str) -> Path:
    """Return one lexical absolute path after rejecting every symlink edge."""
    absolute = Path(os.path.abspath(path))
    current = Path(absolute.anchor)
    for component in absolute.parts[1:]:
        current /= component
        try:
            if current.is_symlink():
                fail(f"{label} traverses a symlink: {current}")
        except OSError as error:
            fail(f"cannot inspect {label}: {error}")
    return absolute


def safe_root(path: Path, label: str) -> Path:
    lexical = reject_symlink_components(path, label)
    try:
        resolved = lexical.resolve(strict=True)
    except OSError as error:
        fail(f"{label} is unavailable: {error}")
    if not resolved.is_dir():
        fail(f"{label} is not one real directory")
    return resolved


def safe_file(path: Path, label: str) -> Path:
    lexical = reject_symlink_components(path, label)
    try:
        resolved = lexical.resolve(strict=True)
    except OSError as error:
        fail(f"{label} is unavailable: {error}")
    if not resolved.is_file():
        fail(f"{label} is not one real regular file")
    return resolved


def compiler_file(path: Path) -> Path:
    """Resolve the CMake-selected compiler, whose launcher may be a symlink."""
    try:
        resolved = path.resolve(strict=True)
    except OSError as error:
        fail(f"CXX compiler is unavailable: {error}")
    if not resolved.is_file():
        fail("CXX compiler does not resolve to one regular file")
    return resolved


def normalize_text(value: str, source_root: Path, build_root: Path) -> str:
    return value.replace(str(build_root), "/build").replace(
        str(source_root), "/source"
    )


def normalize_value(value: Any, source_root: Path, build_root: Path) -> Any:
    if isinstance(value, str):
        return normalize_text(value, source_root, build_root)
    if isinstance(value, list):
        return [normalize_value(item, source_root, build_root) for item in value]
    if isinstance(value, dict):
        return {
            key: normalize_value(item, source_root, build_root)
            for key, item in sorted(value.items())
        }
    return value


def effective_cmake_value(value: Any) -> Any:
    """Drop File API provenance indexes while retaining effective settings."""
    if isinstance(value, list):
        return [effective_cmake_value(item) for item in value]
    if isinstance(value, dict):
        return {
            key: effective_cmake_value(item)
            for key, item in sorted(value.items())
            if key not in {"backtrace", "backtraceGraph"}
        }
    return value


def logical_id(prefix: str, value: str) -> str:
    slug = re.sub(r"[^a-z0-9._-]+", ".", value.lower()).strip(".")
    if not slug or not ("a" <= slug[0] <= "z"):
        slug = "input." + slug
    suffix = hashlib.sha256(value.encode("utf-8")).hexdigest()[:16]
    return f"{prefix}:{slug[:160]}:{suffix}"


def logical_input_name(path: Path, source_root: Path, build_root: Path) -> str:
    try:
        return "/build/" + path.relative_to(build_root).as_posix()
    except ValueError:
        pass
    try:
        return "/source/" + path.relative_to(source_root).as_posix()
    except ValueError:
        fail(
            "qualification input is outside the declared source/build roots: "
            f"{path}"
        )


def input_rows(
    paths: list[Path],
    prefix: str,
    source_root: Path,
    build_root: Path,
) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    observed: set[Path] = set()
    for path in paths:
        resolved = safe_file(path, f"{prefix} input")
        if resolved in observed:
            continue
        observed.add(resolved)
        name = logical_input_name(resolved, source_root, build_root)
        rows.append(
            {
                "input_id": logical_id(prefix, name),
                "content_digest": digest_file(resolved, f"{prefix} input"),
            }
        )
    rows.sort(key=lambda row: row["input_id"])
    if not rows or len(rows) > MAX_INPUTS:
        fail(f"{prefix} input closure has invalid cardinality")
    return rows


def cmake_reply(build_root: Path) -> tuple[dict[str, Any], dict[str, Any], Path]:
    reply_root = build_root / ".cmake/api/v1/reply"
    indexes = sorted(reply_root.glob("index-*.json"))
    if not indexes:
        fail("CMake File API reply is absent; configure once with codemodel-v2")
    index_path = indexes[-1]
    index = strict_json(index_path, "CMake File API index")
    objects = index.get("objects")
    if not isinstance(objects, list):
        fail("CMake File API index has no object list")
    def object_ref(kind: str) -> dict[str, Any]:
        found = next(
            (
                row
                for row in objects
                if isinstance(row, dict)
                and row.get("kind") == kind
                and isinstance(row.get("jsonFile"), str)
            ),
            None,
        )
        if found is None:
            fail(f"CMake File API index has no {kind} reply")
        return found

    codemodel_ref = object_ref("codemodel")
    toolchains_ref = object_ref("toolchains")
    return (
        strict_json(reply_root / codemodel_ref["jsonFile"], "CMake codemodel"),
        strict_json(reply_root / toolchains_ref["jsonFile"], "CMake toolchains"),
        reply_root,
    )


def target_index(
    codemodel: dict[str, Any], reply_root: Path
) -> tuple[dict[str, dict[str, Any]], dict[str, dict[str, Any]]]:
    configurations = codemodel.get("configurations")
    if not isinstance(configurations, list) or len(configurations) != 1:
        fail("qualification requires one unambiguous CMake configuration")
    rows = configurations[0].get("targets")
    if not isinstance(rows, list):
        fail("CMake codemodel target list is missing")
    by_id: dict[str, dict[str, Any]] = {}
    by_name: dict[str, dict[str, Any]] = {}
    for row in rows:
        if not isinstance(row, dict):
            fail("CMake codemodel target row is malformed")
        target_id = row.get("id")
        name = row.get("name")
        json_file = row.get("jsonFile")
        if not all(isinstance(item, str) and item for item in (target_id, name, json_file)):
            fail("CMake codemodel target identity is malformed")
        value = strict_json(reply_root / json_file, f"CMake target {name}")
        if not isinstance(value, dict):
            fail(f"CMake target {name} reply is malformed")
        if value.get("id", target_id) != target_id or value.get("name", name) != name:
            fail(f"CMake target {name} reply identity disagrees with codemodel")
        value["id"] = target_id
        value["name"] = name
        by_id[target_id] = value
        if name in by_name:
            fail(f"CMake target name is ambiguous: {name}")
        by_name[name] = value
    return by_id, by_name


def target_closure(
    target_name: str,
    by_id: dict[str, dict[str, Any]],
    by_name: dict[str, dict[str, Any]],
) -> list[dict[str, Any]]:
    root = by_name.get(target_name)
    if root is None:
        fail(f"CMake target is absent: {target_name}")
    pending = [root]
    result: dict[str, dict[str, Any]] = {}
    while pending:
        target = pending.pop()
        target_id = target.get("id")
        name = target.get("name")
        if not isinstance(target_id, str) or not isinstance(name, str):
            fail("CMake target reply lacks identity")
        if target_id in result:
            continue
        result[target_id] = target
        dependencies = target.get("dependencies", [])
        if not isinstance(dependencies, list):
            fail(f"CMake target {name} dependencies are malformed")
        for dependency in dependencies:
            dependency_id = (
                dependency.get("id") if isinstance(dependency, dict) else None
            )
            if not isinstance(dependency_id, str):
                fail(f"CMake target {name} has malformed dependency")
            if dependency_id in by_id:
                pending.append(by_id[dependency_id])
    return sorted(result.values(), key=lambda value: str(value.get("name")))


def declared_source_path(
    row: dict[str, Any], cmake_source_root: Path, cmake_build_root: Path
) -> tuple[Path, bool]:
    """Resolve one File API source path against its top-level model root.

    Codemodel-v2 source rows are relative to the top-level source directory,
    or to the top-level build directory when `isGenerated` is true. CMake can
    also report non-material rule markers. Those stay in the build projection
    but are not pretended to be hashable source files.
    """
    if not isinstance(row, dict) or not isinstance(row.get("path"), str):
        fail("CMake target source row is malformed")
    generated = row.get("isGenerated", False)
    if not isinstance(generated, bool):
        fail("CMake target source generated marker is malformed")
    path = Path(row["path"])
    if not path.is_absolute():
        path = (cmake_build_root if generated else cmake_source_root) / path
    lexical = reject_symlink_components(path, "CMake target source")
    exists = lexical.exists()
    if exists:
        return safe_file(lexical, "CMake target source"), True
    return lexical, False


def closure_sources(
    closure: list[dict[str, Any]],
    cmake_source_root: Path,
    cmake_build_root: Path,
) -> list[Path]:
    paths: list[Path] = []
    for target in closure:
        rows = target.get("sources", [])
        if not isinstance(rows, list):
            fail("CMake target sources are malformed")
        for row in rows:
            path, exists = declared_source_path(
                row, cmake_source_root, cmake_build_root
            )
            if exists:
                paths.append(path)
    return paths


def source_build_projection(
    target: dict[str, Any],
    source_root: Path,
    build_root: Path,
    cmake_source_root: Path,
    cmake_build_root: Path,
) -> list[dict[str, Any]]:
    projection = []
    rows = target.get("sources", [])
    if not isinstance(rows, list):
        fail("CMake target sources are malformed")
    for row in rows:
        path, exists = declared_source_path(
            row, cmake_source_root, cmake_build_root
        )
        projection.append(
            {
                "path": logical_input_name(path, source_root, build_root),
                "isGenerated": row.get("isGenerated", False),
                "exists": exists,
                "compileGroupIndex": row.get("compileGroupIndex"),
            }
        )
    return sorted(projection, key=lambda row: str(row["path"]))


def cxx_toolchain_projection(toolchains: dict[str, Any]) -> dict[str, Any]:
    rows = toolchains.get("toolchains")
    if not isinstance(rows, list):
        fail("CMake toolchains reply is malformed")
    selected = [
        row
        for row in rows
        if isinstance(row, dict) and row.get("language") == "CXX"
    ]
    if len(selected) != 1:
        fail("qualification requires one unambiguous CXX toolchain")
    value = selected[0]
    compiler = value.get("compiler")
    if not isinstance(compiler, dict) or not isinstance(compiler.get("path"), str):
        fail("CMake CXX toolchain omits its compiler path")
    reported_path = compiler["path"]
    resolved_compiler = compiler_file(Path(reported_path))
    return {
        "language": "CXX",
        "sourceFileExtensions": value.get("sourceFileExtensions", []),
        "compiler": {
            "reported_path_digest": digest_bytes(reported_path.encode("utf-8")),
            "executable_content_digest": digest_file(
                resolved_compiler, "resolved CXX compiler"
            ),
            "id": compiler.get("id"),
            "version": compiler.get("version"),
            "target": compiler.get("target"),
            "frontendVariant": compiler.get("frontendVariant"),
            "implicit": compiler.get("implicit", {}),
        },
    }


def cmake_generated_metadata(build_root: Path) -> dict[str, Any]:
    compiler_files = sorted(
        (build_root / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake")
    )
    system_files = sorted((build_root / "CMakeFiles").glob("*/CMakeSystem.cmake"))
    if len(compiler_files) != 1 or len(system_files) != 1:
        fail("CMake generated compiler/system metadata is absent or ambiguous")

    selected_names = {
        "CMAKE_CXX_COMPILER_ID",
        "CMAKE_CXX_COMPILER_VERSION",
        "CMAKE_CXX_COMPILER_VERSION_INTERNAL",
        "CMAKE_CXX_COMPILER_TARGET",
        "CMAKE_CXX_COMPILER_FRONTEND_VARIANT",
        "CMAKE_CXX_STANDARD_COMPUTED_DEFAULT",
        "CMAKE_CXX_EXTENSIONS_COMPUTED_DEFAULT",
        "CMAKE_CXX_PLATFORM_ID",
        "CMAKE_CXX_SIZEOF_DATA_PTR",
        "CMAKE_CXX_BYTE_ORDER",
        "CMAKE_SYSTEM",
        "CMAKE_SYSTEM_NAME",
        "CMAKE_SYSTEM_VERSION",
        "CMAKE_SYSTEM_PROCESSOR",
    }
    pattern = re.compile(r'^set\(([A-Za-z0-9_]+)\s+"?([^"\)]*)"?\)')
    values: dict[str, str] = {}
    for path in (compiler_files[0], system_files[0]):
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except (OSError, UnicodeError) as error:
            fail(f"cannot read CMake generated metadata: {error}")
        for line in lines:
            match = pattern.match(line.strip())
            if match and match.group(1) in selected_names:
                values[match.group(1)] = match.group(2)
    required = {
        "CMAKE_CXX_COMPILER_ID",
        "CMAKE_CXX_COMPILER_VERSION",
        "CMAKE_CXX_SIZEOF_DATA_PTR",
        "CMAKE_SYSTEM_NAME",
        "CMAKE_SYSTEM_PROCESSOR",
    }
    if not required.issubset(values):
        fail("CMake generated metadata omits required compiler/ABI identity")
    return dict(sorted(values.items()))


def build_rows(
    closure: list[dict[str, Any]],
    source_root: Path,
    build_root: Path,
    cmake_source_root: Path,
    cmake_build_root: Path,
    toolchains: dict[str, Any],
) -> list[dict[str, str]]:
    compile_projection = []
    link_projection = []
    dependency_projection = []
    for target in closure:
        name = target.get("name")
        compile_projection.append(
            {
                "target": name,
                "sources": source_build_projection(
                    target,
                    source_root,
                    build_root,
                    cmake_source_root,
                    cmake_build_root,
                ),
                "compileGroups": effective_cmake_value(
                    target.get("compileGroups", [])
                ),
            }
        )
        link_projection.append(
            {
                "target": name,
                "link": effective_cmake_value(target.get("link")),
                "archive": effective_cmake_value(target.get("archive")),
            }
        )
        dependency_projection.append({
            "target": name,
            "dependencies": sorted(
                dependency["id"]
                for dependency in target.get("dependencies", [])
                if isinstance(dependency, dict)
                and isinstance(dependency.get("id"), str)
            ),
        })
    toolchain_projection = cxx_toolchain_projection(toolchains)
    generated_metadata = cmake_generated_metadata(build_root)
    rows = [
        {
            "input_id": "compile:target-closure",
            "content_digest": digest_bytes(
                canonical_bytes(normalize_value(compile_projection, source_root, build_root))
            ),
        },
        {
            "input_id": "link:target-closure",
            "content_digest": digest_bytes(
                canonical_bytes(
                    normalize_value(
                        {"links": link_projection, "dependencies": dependency_projection},
                        source_root,
                        build_root,
                    )
                )
            ),
        },
        {
            "input_id": "toolchain:cxx",
            "content_digest": digest_bytes(
                canonical_bytes(
                    normalize_value(
                        toolchain_projection, source_root, build_root
                    )
                )
            ),
        },
        {
            "input_id": "abi:target",
            "content_digest": digest_bytes(
                canonical_bytes(
                    normalize_value(
                        {
                            "cmake": codemodel_version(closure),
                            "generated": generated_metadata,
                            "toolchain_target": toolchain_projection[
                                "compiler"
                            ].get("target"),
                        },
                        source_root,
                        build_root,
                    )
                )
            ),
        },
    ]
    return sorted(rows, key=lambda row: row["input_id"])


def codemodel_version(closure: list[dict[str, Any]]) -> list[Any]:
    return sorted(
        {
            (str(target.get("type", "")), str(target.get("name", "")))
            for target in closure
        }
    )


def atomic_write(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + f".tmp.{os.getpid()}")
    try:
        temporary.write_bytes(canonical_bytes(value))
        os.replace(temporary, path)
    except OSError as error:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
        fail(f"cannot write {path}: {error}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--implementation-target", required=True)
    parser.add_argument("--implementation-id", required=True)
    parser.add_argument("--installed-target-id", required=True)
    parser.add_argument("--installed-target", type=Path, required=True)
    parser.add_argument("--runner-target", required=True)
    parser.add_argument("--runner-executable", type=Path, required=True)
    parser.add_argument("--case-contract", type=Path, required=True)
    parser.add_argument("--semantic-input", type=Path, action="append", default=[])
    parser.add_argument(
        "--runner-source-input", type=Path, action="append", default=[]
    )
    parser.add_argument("--implementation-output", type=Path, required=True)
    parser.add_argument("--runner-output", type=Path, required=True)
    args = parser.parse_args()

    source_root = safe_root(args.source_root, "source root")
    build_root = safe_root(args.build_root, "build root")
    installed_target = safe_file(args.installed_target, "installed target")
    runner_executable = safe_file(args.runner_executable, "runner executable")
    case_contract_path = safe_file(args.case_contract, "case contract")
    case_contract = strict_json(case_contract_path, "case contract")
    if case_contract != {
        "api_version": CASE_API,
        "runner_id": CASE_RUNNER_ID,
        "case_contract": EXPECTED_CASE_CONTRACT,
    }:
        fail("case contract has an unsupported closed shape")

    codemodel, toolchains, reply_root = cmake_reply(build_root)
    codemodel_paths = codemodel.get("paths")
    if not isinstance(codemodel_paths, dict):
        fail("CMake codemodel omits its source/build roots")
    cmake_source_root = safe_root(
        Path(str(codemodel_paths.get("source", ""))),
        "CMake codemodel source root",
    )
    cmake_build_root = safe_root(
        Path(str(codemodel_paths.get("build", ""))),
        "CMake codemodel build root",
    )
    if cmake_build_root != build_root:
        fail("CMake codemodel belongs to a different build root")
    try:
        cmake_source_root.relative_to(source_root)
    except ValueError:
        fail("CMake codemodel source root is outside the declared source root")
    by_id, by_name = target_index(codemodel, reply_root)
    implementation_closure = target_closure(
        args.implementation_target, by_id, by_name
    )
    runner_closure = target_closure(args.runner_target, by_id, by_name)

    implementation = {
        "api_version": IMPLEMENTATION_API,
        "implementation_id": args.implementation_id,
        "implementation_source_build_fingerprint": "sha256:" + "0" * 64,
        "source_inputs": input_rows(
            closure_sources(
                implementation_closure, cmake_source_root, cmake_build_root
            ),
            "source",
            source_root,
            build_root,
        ),
        "generated_semantic_inputs": input_rows(
            args.semantic_input,
            "semantic",
            source_root,
            build_root,
        ),
        "build_inputs": build_rows(
            implementation_closure,
            source_root,
            build_root,
            cmake_source_root,
            cmake_build_root,
            toolchains,
        ),
        "installed_target": {
            "installed_target_id": args.installed_target_id,
            "byte_size": installed_target.stat().st_size,
            "content_digest": digest_file(installed_target, "installed target"),
        },
    }
    implementation["implementation_source_build_fingerprint"] = framed_identity(
        implementation,
        "stop-implementation-source-build-fingerprint",
        "implementation_source_build_fingerprint",
    )

    runner_sources = closure_sources(
        runner_closure, cmake_source_root, cmake_build_root
    )
    runner_sources.extend(
        [*args.runner_source_input, case_contract_path, Path(__file__)]
    )
    runner_contract = {
        "api_version": RUNNER_API,
        "qualification_runner_contract_digest": "sha256:" + "0" * 64,
        "runner_id": case_contract["runner_id"],
        "source_inputs": input_rows(
            runner_sources, "source", source_root, build_root
        ),
        "build_inputs": build_rows(
            runner_closure,
            source_root,
            build_root,
            cmake_source_root,
            cmake_build_root,
            toolchains,
        ),
        "executable": {
            "byte_size": runner_executable.stat().st_size,
            "content_digest": digest_file(runner_executable, "runner executable"),
        },
        "case_contract": case_contract["case_contract"],
    }
    runner_contract["qualification_runner_contract_digest"] = framed_identity(
        runner_contract,
        "stop-qualification-runner-contract-digest",
        "qualification_runner_contract_digest",
    )

    atomic_write(args.implementation_output, implementation)
    atomic_write(args.runner_output, runner_contract)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ClosureError as error:
        raise SystemExit(f"native Stop build-closure generation failed: {error}")
