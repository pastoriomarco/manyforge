#!/usr/bin/env python3
"""Execute one exact I2 CTest registry and emit checkable packet evidence.

This file is part of the Composable Cell I2 qualification packet.  It
groups discovered GoogleTest cases by their registered executable, executes
ordinary CTest entries individually, and records the exact argv, result, log,
and executable identity for every process.  It never falls back to a different
execution strategy and never accepts disabled or skipped tests.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import os
import re
import signal
import stat
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


CTEST = Path("/usr/bin/ctest")
PYTHON = Path("/usr/local/bin/python3")
PACKET_VERIFIER = Path(
    "/workspace/verification/composable_cell_i2/verify_qualification_manifest.py"
)
PACKET_MANIFEST = Path(
    "/workspace/verification/composable_cell_i2/qualification_manifest.v1.json"
)
SCHEMA = "manyforge.native_direct_execution.v1"
PREEXECUTION_RECEIPT_SCHEMA = (
    "manyforge.composable_cell_i2.native_preexecution_receipt.v1"
)
SANITIZER_ENVIRONMENT = {
    "none": {},
    "address+undefined": {
        "ASAN_OPTIONS": "halt_on_error=1:detect_leaks=1:exitcode=66",
        "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1:exitcode=66",
    },
    "thread": {
        "TSAN_OPTIONS": (
            "halt_on_error=1 second_deadlock_stack=1 history_size=7 "
            "exitcode=66 suppressions=/workspace/scripts/ci/tsan.supp"
        ),
    },
}
SANITIZER_SIGNATURES = (
    "ERROR: AddressSanitizer",
    "ERROR: LeakSanitizer",
    "SUMMARY: AddressSanitizer",
    "SUMMARY: UndefinedBehaviorSanitizer",
    "WARNING: ThreadSanitizer",
    "SUMMARY: ThreadSanitizer",
    "FATAL: ThreadSanitizer",
    "runtime error:",
)


@dataclass
class GTestGroup:
    binary: Path
    working_directory: Path
    tests: list[str] = field(default_factory=list)
    filters: list[str] = field(default_factory=list)
    environment: dict[str, str] = field(default_factory=dict)
    timeout_seconds: float = 300.0


@dataclass(frozen=True)
class OrdinaryCTest:
    name: str
    command: tuple[str, ...]
    working_directory: Path
    environment: dict[str, str]
    timeout_seconds: float


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lane-id", required=True)
    parser.add_argument(
        "--sanitizer", choices=sorted(SANITIZER_ENVIRONMENT), required=True
    )
    parser.add_argument("--artifact-root", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--inventory-json", type=Path, required=True)
    parser.add_argument("--execution-index-json", type=Path, required=True)
    parser.add_argument("--result-root", type=Path, required=True)
    parser.add_argument("--test-regex")
    parser.add_argument("--exclude-name", action="append", default=[])
    parser.add_argument("--timeout-seconds", type=float, default=300.0)
    parser.add_argument("--require-zero-disabled", action="store_true")
    parser.add_argument("--require-zero-skips", action="store_true")
    parser.add_argument("--forbid-fallback", action="store_true")
    return parser.parse_args()


def fail(message: str) -> None:
    raise RuntimeError(message)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def property_map(test: dict[str, Any]) -> dict[str, Any]:
    raw = test.get("properties", [])
    if not isinstance(raw, list):
        fail(f"CTest properties are not an array: {test.get('name')!r}")
    properties: dict[str, Any] = {}
    for entry in raw:
        if not isinstance(entry, dict) or not isinstance(entry.get("name"), str):
            fail(f"CTest property is malformed: {test.get('name')!r}")
        name = entry["name"]
        if name in properties:
            fail(f"CTest property is duplicated for {test.get('name')!r}: {name}")
        properties[name] = entry.get("value")
    return properties


def ctest_skip_marker_is_exact(value: Any) -> bool:
    """Match CTest 3.25's JSON representation of the exact GTest skip marker."""

    return value == [r"\[  SKIPPED \]"]


def truthy_ctest(value: Any) -> bool:
    if value is True:
        return True
    if isinstance(value, int) and not isinstance(value, bool):
        return value != 0
    return isinstance(value, str) and value.upper() in {"1", "ON", "TRUE", "YES"}


def require_absolute_under(path: Path, root: Path, label: str) -> Path:
    if not path.is_absolute():
        fail(f"{label} must be absolute: {path}")
    resolved_root = root.resolve(strict=True)
    try:
        resolved = path.resolve(strict=False)
    except OSError as error:
        fail(f"cannot resolve {label}: {error}")
    if not resolved.is_relative_to(resolved_root):
        fail(f"{label} escapes its root: {path}")
    current = resolved_root
    relative = resolved.relative_to(resolved_root)
    for part in relative.parts[:-1]:
        current = current / part
        if current.exists() and current.is_symlink():
            fail(f"{label} contains a symlinked directory: {current}")
    return resolved


def prepare_output_file(path: Path, root: Path, label: str) -> Path:
    path = require_absolute_under(path, root, label)
    path.parent.mkdir(parents=True, exist_ok=True)
    current = root.resolve(strict=True)
    for part in path.parent.relative_to(current).parts:
        current = current / part
        metadata = current.lstat()
        if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
            fail(f"{label} parent is unsafe: {current}")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(path, flags, 0o600)
    except OSError as error:
        fail(f"cannot exclusively create {label}: {error}")
    metadata = os.fstat(descriptor)
    os.close(descriptor)
    if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1:
        fail(f"{label} is not one regular file")
    return path


def verify_output_file(path: Path, root: Path, label: str) -> None:
    path = require_absolute_under(path, root, label)
    metadata = path.lstat()
    if (
        stat.S_ISLNK(metadata.st_mode)
        or not stat.S_ISREG(metadata.st_mode)
        or metadata.st_nlink != 1
        or metadata.st_size == 0
    ):
        fail(f"{label} is not one nonempty regular file")


def write_exclusive_text(path: Path, root: Path, content: str, label: str) -> None:
    path = prepare_output_file(path, root, label)
    with path.open("w", encoding="utf-8") as destination:
        destination.write(content)
    verify_output_file(path, root, label)


def write_exclusive_json(path: Path, root: Path, value: Any, label: str) -> None:
    write_exclusive_text(
        path,
        root,
        json.dumps(value, indent=2, sort_keys=True) + "\n",
        label,
    )


def run_process(
    argv: list[str],
    cwd: Path,
    timeout_seconds: float,
    base_environment: dict[str, str],
    environment: dict[str, str],
) -> tuple[int, str, bool]:
    process_environment = dict(base_environment)
    process_environment.update(environment)
    process = subprocess.Popen(
        argv,
        cwd=cwd,
        env=process_environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        start_new_session=True,
    )
    try:
        output, _ = process.communicate(timeout=timeout_seconds)
        return process.returncode, output, False
    except subprocess.TimeoutExpired as error:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        final_output, _ = process.communicate()
        output = error.stdout or ""
        if isinstance(output, bytes):
            output = output.decode(errors="replace")
        if final_output and final_output not in output:
            output += final_output
        return 124, output, True


def sanitizer_clean(output: str) -> bool:
    return not any(signature in output for signature in SANITIZER_SIGNATURES)


def ctest_environment(value: Any, name: str) -> dict[str, str]:
    if value is None:
        return {}
    if isinstance(value, str):
        entries = value.split(";") if value else []
    elif isinstance(value, list) and all(isinstance(entry, str) for entry in value):
        entries = value
    else:
        fail(f"CTest ENVIRONMENT property is malformed: {name}")
    result: dict[str, str] = {}
    for entry in entries:
        key, separator, item = entry.partition("=")
        if (
            separator != "="
            or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", key)
            or key in result
            or "\x00" in item
        ):
            fail(f"CTest ENVIRONMENT entry is malformed or duplicated: {name}")
        result[key] = item
    return result


def load_registry(
    build_dir: Path,
    inventory_path: Path,
    artifact_root: Path,
    test_regex: str | None,
    environment: dict[str, str],
) -> tuple[dict[str, Any], list[str]]:
    argv = [str(CTEST), "--test-dir", str(build_dir), "--show-only=json-v1"]
    if test_regex is not None:
        argv.extend(["-R", test_regex])
    completed = subprocess.run(
        argv,
        cwd=build_dir,
        env=dict(environment),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        fail(
            f"CTest registry failed ({completed.returncode}): "
            f"{completed.stderr.strip() or completed.stdout.strip()}"
        )
    try:
        registry = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        fail(f"CTest registry is malformed JSON: {error}")
    if not isinstance(registry, dict) or not isinstance(registry.get("tests"), list):
        fail("CTest registry has no test array")
    write_exclusive_text(
        inventory_path,
        artifact_root,
        completed.stdout,
        "CTest inventory",
    )
    return registry, argv


def validate_frozen_registry(
    lane_id: str,
    inventory_path: Path,
    artifact_root: Path,
    validator_environment: dict[str, str],
    *,
    revalidate_provenance: bool = False,
) -> dict[str, Any]:
    for path, label in (
        (PYTHON, "packet Python"),
        (PACKET_VERIFIER, "packet verifier"),
        (PACKET_MANIFEST, "packet manifest"),
    ):
        if not path.is_file() or not os.access(path, os.R_OK):
            fail(f"{label} is unavailable: {path}")
    completed = subprocess.run(
        [
            str(PYTHON),
            str(PACKET_VERIFIER),
            "--manifest",
            str(PACKET_MANIFEST),
            (
                "--revalidate-native-provenance"
                if revalidate_provenance
                else "--validate-native-registry"
            ),
            lane_id,
            "--registry-inventory",
            str(inventory_path),
        ],
        cwd=Path("/workspace"),
        env=dict(validator_environment),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        fail(
            "frozen registry/provenance validation failed: "
            f"{completed.stderr.strip() or completed.stdout.strip()}"
        )
    try:
        receipt = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        fail(f"native pre-execution receipt is malformed JSON: {error}")
    required_keys = {
        "schema_version",
        "lane_id",
        "inventory_sha256",
        "provenance_path",
        "provenance_sha256",
        "provenance_evidence_sha256",
        "native_target_mapping_sha256",
    }
    if not isinstance(receipt, dict) or set(receipt) != required_keys:
        fail("native pre-execution receipt has the wrong shape")
    digest_fields = (
        "inventory_sha256",
        "provenance_sha256",
        "provenance_evidence_sha256",
        "native_target_mapping_sha256",
    )
    if (
        receipt["schema_version"] != PREEXECUTION_RECEIPT_SCHEMA
        or receipt["lane_id"] != lane_id
        or receipt["inventory_sha256"] != sha256_file(inventory_path)
        or any(
            not isinstance(receipt[field], str)
            or re.fullmatch(r"[0-9a-f]{64}", receipt[field]) is None
            for field in digest_fields
        )
    ):
        fail("native pre-execution receipt identity drifted")
    raw_path = receipt["provenance_path"]
    if not isinstance(raw_path, str) or not raw_path:
        fail("native provenance receipt path is malformed")
    relative = Path(raw_path)
    if (
        relative.is_absolute()
        or relative.as_posix() != raw_path
        or any(part in {"", ".", ".."} for part in relative.parts)
    ):
        fail("native provenance receipt path is not canonical and relative")
    provenance_path = artifact_root / relative
    verify_output_file(provenance_path, artifact_root, "native build provenance")
    if sha256_file(provenance_path) != receipt["provenance_sha256"]:
        fail("native build provenance bytes differ from the pre-execution receipt")
    return receipt


def classify_registry(
    registry: dict[str, Any],
    build_dir: Path,
    exclusions: set[str],
    timeout_ceiling: float,
    require_zero_disabled: bool,
) -> tuple[list[GTestGroup], list[OrdinaryCTest], set[str]]:
    groups: "collections.OrderedDict[tuple[Path, Path, tuple[tuple[str, str], ...]], GTestGroup]" = (
        collections.OrderedDict()
    )
    ordinary: list[OrdinaryCTest] = []
    names: set[str] = set()
    found_exclusions: set[str] = set()
    allowed_gtest_arguments = {"--gtest_also_run_disabled_tests"}
    forbidden_on_presence = {
        "PASS_REGULAR_EXPRESSION",
        "SKIP_RETURN_CODE",
        "ENVIRONMENT_MODIFICATION",
    }

    for raw in registry["tests"]:
        if not isinstance(raw, dict):
            fail("CTest registry contains a non-object test")
        name = raw.get("name")
        command = raw.get("command")
        if not isinstance(name, str) or not name or name in names:
            fail(f"CTest test name is empty or duplicated: {name!r}")
        if not isinstance(command, list) or not command or not all(
            isinstance(value, str) and value for value in command
        ):
            fail(f"CTest command is malformed: {name}")
        names.add(name)
        properties = property_map(raw)
        if require_zero_disabled and truthy_ctest(properties.get("DISABLED")):
            fail(f"CTest registry contains a disabled test: {name}")
        excluded = name in exclusions
        if excluded:
            found_exclusions.add(name)

        filters = [
            value.removeprefix("--gtest_filter=")
            for value in command[1:]
            if value.startswith("--gtest_filter=")
        ]
        gtest_arguments = {
            value for value in command[1:] if value.startswith("--gtest_")
        }
        for key in forbidden_on_presence:
            if key in properties:
                fail(
                    "CTest registry contains an unsupported result/environment "
                    f"property {key}: {name}"
                )
        if "WILL_FAIL" in properties and truthy_ctest(properties["WILL_FAIL"]):
            fail(f"CTest registry inverts failure into success: {name}")
        environment = ctest_environment(properties.get("ENVIRONMENT"), name)
        if not filters and not gtest_arguments:
            if "SKIP_REGULAR_EXPRESSION" in properties:
                fail(f"ordinary CTest entry may not convert output into a skip: {name}")
            if excluded:
                continue
            timeout = min(float(properties.get("TIMEOUT", timeout_ceiling)), timeout_ceiling)
            working_directory = Path(
                str(properties.get("WORKING_DIRECTORY", build_dir))
            ).resolve(strict=True)
            if not working_directory.is_relative_to(build_dir):
                fail(
                    "ordinary CTest working directory is outside the exact build: "
                    f"{working_directory}"
                )
            ordinary.append(
                OrdinaryCTest(
                    name=name,
                    command=tuple(command),
                    working_directory=working_directory,
                    environment=environment,
                    timeout_seconds=timeout,
                )
            )
            continue
        if not ctest_skip_marker_is_exact(
            properties.get("SKIP_REGULAR_EXPRESSION")
        ):
            fail(
                "registered GoogleTest must retain the exact CMake skip marker; "
                f"direct JSON execution remains authoritative: {name}"
            )
        if excluded:
            continue
        extras = set(command[1:]) - {f"--gtest_filter={value}" for value in filters}
        if (
            len(filters) != 1
            or not filters[0]
            or ":" in filters[0]
            or not extras.issubset(allowed_gtest_arguments)
        ):
            fail(f"unsupported registered GoogleTest command for {name}: {command}")
        binary = Path(command[0]).resolve(strict=True)
        working_directory = Path(
            str(properties.get("WORKING_DIRECTORY", binary.parent))
        ).resolve(strict=True)
        if not binary.is_relative_to(build_dir) or not binary.is_file():
            fail(f"registered GoogleTest binary is outside the exact build: {binary}")
        if not working_directory.is_relative_to(build_dir):
            fail(
                "registered GoogleTest working directory is outside the exact build: "
                f"{working_directory}"
            )
        if not os.access(binary, os.X_OK):
            fail(f"registered GoogleTest binary is not executable: {binary}")
        timeout = min(float(properties.get("TIMEOUT", timeout_ceiling)), timeout_ceiling)
        key = (binary, working_directory, tuple(sorted(environment.items())))
        group = groups.setdefault(
            key,
            GTestGroup(
                binary=binary,
                working_directory=working_directory,
                environment=environment,
                timeout_seconds=timeout,
            ),
        )
        group.timeout_seconds = min(group.timeout_seconds, timeout)
        group.tests.append(name)
        group.filters.append(filters[0])

    if not names:
        fail("CTest registry contains zero tests")
    if found_exclusions != exclusions:
        fail(f"declared exclusions are absent: {sorted(exclusions - found_exclusions)}")
    return list(groups.values()), ordinary, names


def gtest_cases(path: Path) -> dict[str, str]:
    try:
        decoded = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(f"GoogleTest result is unreadable: {error}")
    suites = decoded.get("testsuites") if isinstance(decoded, dict) else None
    if not isinstance(suites, list):
        fail("GoogleTest result has no suites")
    cases: dict[str, str] = {}
    for suite in suites:
        if not isinstance(suite, dict) or not isinstance(suite.get("name"), str):
            fail("GoogleTest result contains a malformed suite")
        tests = suite.get("testsuite")
        if not isinstance(tests, list):
            fail("GoogleTest result contains a malformed case array")
        for case in tests:
            if not isinstance(case, dict) or not isinstance(case.get("name"), str):
                fail("GoogleTest result contains a malformed case")
            name = f"{suite['name']}.{case['name']}"
            if name in cases:
                fail(f"GoogleTest result duplicates a case: {name}")
            failures = case.get("failures", [])
            if not isinstance(failures, list):
                fail(f"GoogleTest failure list is malformed: {name}")
            if failures or case.get("result") != "COMPLETED":
                status = "failed"
            elif case.get("status") not in {None, "RUN"}:
                status = "skipped"
            else:
                status = "passed"
            cases[name] = status
    if not cases:
        fail("GoogleTest result contains zero cases")
    return cases


def process_record(
    *,
    kind: str,
    argv: list[str],
    cwd: Path,
    timeout_seconds: float,
    result_path: Path,
    log_path: Path,
    artifact_root: Path,
    registered_tests: list[str],
    binary: Path | None,
    base_environment: dict[str, str],
    environment: dict[str, str],
) -> dict[str, Any]:
    status, output, timed_out = run_process(
        argv, cwd, timeout_seconds, base_environment, environment
    )
    write_exclusive_text(log_path, artifact_root, output or "<no output>\n", "process log")
    if timed_out or status != 0:
        fail(f"native test process failed: status={status}, timed_out={timed_out}")
    if not sanitizer_clean(output):
        fail("native test process reported a sanitizer finding")
    if kind == "gtest_json":
        verify_output_file(result_path, artifact_root, "process result")
        cases = gtest_cases(result_path)
    elif kind == "ordinary_direct":
        if len(registered_tests) != 1:
            fail("one ordinary process must represent exactly one registered test")
        if re.search(
            r"(?im)^\s*(?:\[\s+SKIPPED\s+\]|SKIP(?:PED)?\s*:|"
            r"dependency[-_ ]unavailable\b)",
            output,
        ):
            fail("ordinary native process reported a skipped or unavailable result")
        write_exclusive_json(
            result_path,
            artifact_root,
            {
                "schema_version": "manyforge.native_ordinary_result.v1",
                "name": registered_tests[0],
                "argv": argv,
                "cwd": cwd.relative_to(artifact_root).as_posix(),
                "environment": environment,
                "exit_status": status,
                "timed_out": timed_out,
                "log_sha256": sha256_file(log_path),
                "passed": True,
            },
            "ordinary direct result",
        )
        cases = {registered_tests[0]: "passed"}
    else:
        fail(f"native process kind is unsupported: {kind}")
    if set(cases) != set(registered_tests):
        fail(
            "native result set differs from its exact registered tests: "
            f"expected={sorted(registered_tests)}, actual={sorted(cases)}"
        )
    if any(value != "passed" for value in cases.values()):
        fail(f"native result contains a failed or skipped test: {cases}")
    return {
        "kind": kind,
        "cwd": cwd.relative_to(artifact_root).as_posix(),
        "argv": argv,
        "exit_status": status,
        "timed_out": timed_out,
        "timeout_seconds": timeout_seconds,
        "environment": environment,
        "result_path": result_path.relative_to(artifact_root).as_posix(),
        "log_path": log_path.relative_to(artifact_root).as_posix(),
        "binary_path": (
            binary.relative_to(artifact_root).as_posix() if binary is not None else None
        ),
        "binary_sha256": sha256_file(binary) if binary is not None else None,
        "registered_tests": registered_tests,
    }


def main() -> int:
    args = parse_args()
    if not re.fullmatch(r"[a-z][a-z0-9_]*", args.lane_id):
        fail("lane ID is not canonical")
    if args.timeout_seconds <= 0 or args.timeout_seconds > 3600:
        fail("timeout ceiling is outside the qualified range")
    if not args.require_zero_disabled or not args.require_zero_skips:
        fail("qualification requires zero disabled and zero skipped tests")
    if not args.forbid_fallback:
        fail("qualification must explicitly forbid fallback execution")
    if len(args.exclude_name) != len(set(args.exclude_name)):
        fail("native exclusions are duplicated")
    forbidden_outer = {
        "CC",
        "CXX",
        "CFLAGS",
        "CXXFLAGS",
        "CPPFLAGS",
        "LDFLAGS",
        "LD_LIBRARY_PATH",
        "LD_PRELOAD",
        "LD_AUDIT",
        "PYTHONPATH",
        "GTEST_FILTER",
        "GTEST_OUTPUT",
    }
    present_forbidden = sorted(forbidden_outer.intersection(os.environ))
    present_forbidden.extend(
        sorted(name for name in os.environ if name.startswith("CTEST_"))
    )
    if present_forbidden:
        fail(
            "native runner received forbidden ambient overrides: "
            f"{present_forbidden}"
        )
    if not CTEST.is_file() or not os.access(CTEST, os.X_OK):
        fail("exact /usr/bin/ctest is unavailable")

    artifact_root = args.artifact_root.resolve(strict=True)
    build_dir = require_absolute_under(args.build_dir, artifact_root, "build directory")
    if not build_dir.is_dir() or build_dir.is_symlink():
        fail("build directory is absent or unsafe")
    inventory_path = require_absolute_under(
        args.inventory_json, artifact_root, "CTest inventory"
    )
    index_path = require_absolute_under(
        args.execution_index_json, artifact_root, "execution index"
    )
    result_root = require_absolute_under(args.result_root, artifact_root, "result root")
    result_root.mkdir(parents=True, exist_ok=False)

    expected_environment = SANITIZER_ENVIRONMENT[args.sanitizer]
    actual_environment = {
        key: os.environ[key]
        for key in ("ASAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS")
        if key in os.environ
    }
    if actual_environment != expected_environment:
        fail(
            "sanitizer environment differs from the frozen contract: "
            f"expected={expected_environment}, actual={actual_environment}"
        )
    fixed_environment = {
        "PATH": "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "TZ": "UTC",
        "PYTHONNOUSERSITE": "1",
        "TMPDIR": str(artifact_root / "tmp" / args.lane_id),
        "HOME": str(artifact_root / "tmp" / args.lane_id),
        "XDG_CACHE_HOME": str(
            artifact_root / "tmp" / args.lane_id / "xdg-cache"
        ),
        **expected_environment,
    }
    if any(os.environ.get(name) != value for name, value in fixed_environment.items()):
        fail("native process base environment differs from the frozen contract")

    registry, inventory_argv = load_registry(
        build_dir,
        inventory_path,
        artifact_root,
        args.test_regex,
        fixed_environment,
    )
    frozen_inventory_sha256 = sha256_file(inventory_path)
    preexecution_receipt = validate_frozen_registry(
        args.lane_id,
        inventory_path,
        artifact_root,
        fixed_environment,
    )
    if sha256_file(inventory_path) != frozen_inventory_sha256:
        fail("CTest inventory changed during frozen pre-execution validation")
    groups, ordinary, registered = classify_registry(
        registry,
        build_dir,
        set(args.exclude_name),
        args.timeout_seconds,
        args.require_zero_disabled,
    )
    processes: list[dict[str, Any]] = []
    process_ordinal = 0
    for group in groups:
        process_ordinal += 1
        result_path = prepare_output_file(
            result_root / f"{process_ordinal:03d}-gtest.json",
            artifact_root,
            "GoogleTest JSON result",
        )
        log_path = result_root / f"{process_ordinal:03d}-gtest.log"
        argv = [
            str(group.binary),
            f"--gtest_filter={':'.join(group.filters)}",
            "--gtest_color=no",
            f"--gtest_output=json:{result_path}",
        ]
        processes.append(
            process_record(
                kind="gtest_json",
                argv=argv,
                cwd=group.working_directory,
                timeout_seconds=group.timeout_seconds,
                result_path=result_path,
                log_path=log_path,
                artifact_root=artifact_root,
                registered_tests=group.tests,
                binary=group.binary,
                base_environment=fixed_environment,
                environment=group.environment,
            )
        )
    for test in ordinary:
        process_ordinal += 1
        result_path = result_root / f"{process_ordinal:03d}-ordinary.json"
        log_path = result_root / f"{process_ordinal:03d}-ordinary.log"
        argv = list(test.command)
        processes.append(
            process_record(
                kind="ordinary_direct",
                argv=argv,
                cwd=test.working_directory,
                timeout_seconds=test.timeout_seconds,
                result_path=result_path,
                log_path=log_path,
                artifact_root=artifact_root,
                registered_tests=[test.name],
                binary=None,
                base_environment=fixed_environment,
                environment=test.environment,
            )
        )
    if not processes:
        fail("native registry produced zero executable processes")
    if sha256_file(inventory_path) != frozen_inventory_sha256:
        fail("CTest inventory changed during direct native execution")
    final_receipt = validate_frozen_registry(
        args.lane_id,
        inventory_path,
        artifact_root,
        fixed_environment,
        revalidate_provenance=True,
    )
    if final_receipt != preexecution_receipt:
        fail("native provenance receipt changed after direct native execution")
    if sha256_file(inventory_path) != frozen_inventory_sha256:
        fail("CTest inventory changed during final provenance recomputation")

    index = {
        "schema_version": SCHEMA,
        "lane_id": args.lane_id,
        "sanitizer": args.sanitizer,
        "artifact_root": str(artifact_root),
        "build_dir": build_dir.relative_to(artifact_root).as_posix(),
        "inventory_argv": inventory_argv,
        "inventory_sha256": frozen_inventory_sha256,
        "provenance_path": preexecution_receipt["provenance_path"],
        "provenance_sha256": preexecution_receipt["provenance_sha256"],
        "provenance_evidence_sha256": preexecution_receipt[
            "provenance_evidence_sha256"
        ],
        "native_target_mapping_sha256": preexecution_receipt[
            "native_target_mapping_sha256"
        ],
        "test_regex": args.test_regex,
        "excluded_tests": sorted(args.exclude_name),
        "process_base_environment": fixed_environment,
        "fallback_processes": 0,
        "registered_test_count": len(registered),
        "processes": processes,
    }
    write_exclusive_json(index_path, artifact_root, index, "execution index")
    print(
        f"native direct evidence: lane={args.lane_id} "
        f"registered={len(registered)} executed={len(registered) - len(args.exclude_name)} "
        f"processes={len(processes)}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, ValueError) as error:
        print(f"grouped native sanitizer runner failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
