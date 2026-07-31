#!/usr/bin/env python3
"""Run a CTest registry efficiently and completely under a sanitizer.

CTest normally registers one process per GTest case. That is useful for
ordinary CI, but sanitizer process startup/teardown (especially LeakSanitizer
on ARM) can dominate a full suite. This runner consumes CTest's JSON registry,
groups the exact enabled filters by executable, and verifies that each grouped
process ran the same number of cases that CTest registered. Non-GTest CTest
entries remain owned by CTest and are executed individually with their exact
registered name; they are never mistaken for malformed GTest filters or
silently omitted.
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


@dataclass
class BinaryGroup:
    binary: str
    working_directory: str
    enabled_filters: list[str] = field(default_factory=list)
    registered_count: int = 0
    disabled_count: int = 0
    timeout_seconds: float = 300.0


@dataclass(frozen=True)
class OrdinaryCTest:
    name: str
    disabled: bool
    timeout_seconds: float


def properties(test: dict[str, Any]) -> dict[str, Any]:
    return {
        entry["name"]: entry.get("value")
        for entry in test.get("properties", [])
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--test-regex")
    parser.add_argument("--exclude-test-regex")
    parser.add_argument("--summary-prefix", default="GROUPED_GTEST")
    parser.add_argument(
        "--timeout-seconds",
        type=float,
        default=float(
            os.environ.get("MANYFORGE_GROUPED_GTEST_TIMEOUT_SECONDS", "300")
        ),
        help="Per-binary timeout ceiling; CTest's per-case timeout may lower it.",
    )
    return parser.parse_args()


def load_registry(
    build_dir: Path,
    test_regex: str | None,
    exclude_test_regex: str | None,
) -> list[dict[str, Any]]:
    command = [
        "ctest",
        "--test-dir",
        str(build_dir),
        "--show-only=json-v1",
    ]
    if test_regex:
        command.extend(["-R", test_regex])
    if exclude_test_regex:
        command.extend(["-E", exclude_test_regex])
    completed = subprocess.run(
        command,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return json.loads(completed.stdout)["tests"]


def group_registry(
    tests: list[dict[str, Any]], timeout_ceiling: float
) -> tuple[
    collections.OrderedDict[str, BinaryGroup],
    list[OrdinaryCTest],
]:
    groups: collections.OrderedDict[str, BinaryGroup] = collections.OrderedDict()
    ordinary_tests: list[OrdinaryCTest] = []
    allowed_extra_arguments = {"--gtest_also_run_disabled_tests"}

    for test in tests:
        command = test.get("command", [])
        if not command:
            raise RuntimeError(f"CTest entry has no command: {test.get('name')}")
        test_name = test.get("name")
        if not isinstance(test_name, str) or not test_name:
            raise RuntimeError(f"CTest entry has no valid name: {test_name!r}")
        props = properties(test)
        filters = [
            argument.removeprefix("--gtest_filter=")
            for argument in command[1:]
            if argument.startswith("--gtest_filter=")
        ]
        extras = {
            argument
            for argument in command[1:]
            if not argument.startswith("--gtest_filter=")
        }
        has_gtest_argument = any(
            argument.startswith("--gtest_") for argument in command[1:]
        )
        if not filters and not has_gtest_argument:
            timeout_seconds = float(props.get("TIMEOUT", timeout_ceiling))
            ordinary_tests.append(
                OrdinaryCTest(
                    name=test_name,
                    disabled=props.get("DISABLED") is True,
                    timeout_seconds=min(timeout_ceiling, timeout_seconds),
                )
            )
            continue
        if len(filters) != 1 or not extras.issubset(allowed_extra_arguments):
            raise RuntimeError(
                f"unsupported CTest GTest command for {test_name}: {command}"
            )

        working_directory = str(props.get("WORKING_DIRECTORY", Path(command[0]).parent))
        timeout_seconds = float(props.get("TIMEOUT", timeout_ceiling))
        binary = command[0]
        group = groups.get(binary)
        if group is None:
            group = BinaryGroup(
                binary=binary,
                working_directory=working_directory,
                timeout_seconds=min(timeout_ceiling, timeout_seconds),
            )
            groups[binary] = group
        elif group.working_directory != working_directory:
            raise RuntimeError(
                f"one binary has multiple CTest working directories: {binary}"
            )
        group.timeout_seconds = min(
            group.timeout_seconds, timeout_ceiling, timeout_seconds
        )
        group.registered_count += 1
        if props.get("DISABLED") is True:
            group.disabled_count += 1
        else:
            group.enabled_filters.append(filters[0])

    return groups, ordinary_tests


def summary_count(pattern: str, output: str) -> int:
    matches = re.findall(pattern, output, flags=re.MULTILINE)
    return int(matches[-1]) if matches else 0


def has_sanitizer_finding(returncode: int, output: str) -> bool:
    if returncode == 66:
        return True
    signatures = (
        "ERROR: AddressSanitizer",
        "ERROR: LeakSanitizer",
        "SUMMARY: AddressSanitizer",
        "SUMMARY: UndefinedBehaviorSanitizer",
        "WARNING: ThreadSanitizer",
        "SUMMARY: ThreadSanitizer",
        "FATAL: ThreadSanitizer",
    )
    return any(signature in output for signature in signatures)


def execute(
    command: list[str], working_directory: str, timeout_seconds: float
) -> tuple[int, str, bool]:
    try:
        completed = subprocess.run(
            command,
            cwd=working_directory,
            env=os.environ.copy(),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout_seconds,
        )
        return completed.returncode, completed.stdout, False
    except subprocess.TimeoutExpired as error:
        output = error.stdout or ""
        if isinstance(output, bytes):
            output = output.decode(errors="replace")
        return 124, output, True


def output_counts(output: str) -> tuple[int, int, int]:
    ran = summary_count(
        r"^\[==========\] ([0-9]+) tests? from .* ran\.", output
    )
    passed = summary_count(r"^\[  PASSED  \] ([0-9]+) tests?\.", output)
    skipped = summary_count(
        r"^\[  SKIPPED \] ([0-9]+) tests?(?:,|\.)", output
    )
    return ran, passed, skipped


def run_isolated_fallback(
    index: int, group: BinaryGroup
) -> tuple[int, int, int]:
    print(
        f"GROUP_FALLBACK_START index={index} tests={len(group.enabled_filters)} "
        f"binary={group.binary}",
        flush=True,
    )
    total_ran = 0
    total_passed = 0
    total_skipped = 0
    for isolated_index, test_filter in enumerate(group.enabled_filters, start=1):
        command = [
            group.binary,
            f"--gtest_filter={test_filter}",
            "--gtest_also_run_disabled_tests",
            "--gtest_color=no",
        ]
        status, output, timed_out = execute(
            command, group.working_directory, group.timeout_seconds
        )
        print(output, end="", flush=True)
        ran, passed, skipped = output_counts(output)
        print(
            f"ISOLATED_RESULT group_index={index} "
            f"test_index={isolated_index} status={status} timed_out={int(timed_out)} "
            f"ran={ran} passed={passed} skipped={skipped} filter={test_filter}",
            flush=True,
        )
        if has_sanitizer_finding(status, output):
            raise RuntimeError(
                f"sanitizer finding in isolated GTest: {test_filter}"
            )
        if status != 0 or ran != 1 or passed + skipped != 1:
            raise RuntimeError(f"isolated GTest execution failed: {test_filter}")
        total_ran += ran
        total_passed += passed
        total_skipped += skipped
    print(
        f"GROUP_FALLBACK_RESULT index={index} status=0 ran={total_ran} "
        f"passed={total_passed} skipped={total_skipped} binary={group.binary}",
        flush=True,
    )
    return total_ran, total_passed, total_skipped


def run_group(index: int, group: BinaryGroup) -> tuple[int, int, int, bool]:
    expected = len(group.enabled_filters)
    if expected == 0:
        print(
            f"GROUP_DISABLED index={index} registered={group.registered_count} "
            f"binary={group.binary}",
            flush=True,
        )
        return 0, 0, 0, False

    print(
        f"GROUP_START index={index} expected={expected} "
        f"disabled={group.disabled_count} timeout_seconds={group.timeout_seconds:g} "
        f"binary={group.binary}",
        flush=True,
    )
    command = [
        group.binary,
        f"--gtest_filter={':'.join(group.enabled_filters)}",
        "--gtest_color=no",
    ]
    status, output, timed_out = execute(
        command, group.working_directory, group.timeout_seconds
    )
    print(output, end="", flush=True)
    ran, passed, skipped = output_counts(output)
    print(
        f"GROUP_RESULT index={index} status={status} timed_out={int(timed_out)} "
        f"expected={expected} ran={ran} passed={passed} skipped={skipped} "
        f"binary={group.binary}",
        flush=True,
    )
    if status == 0 and ran == expected and passed + skipped == expected:
        return ran, passed, skipped, False
    if has_sanitizer_finding(status, output):
        raise RuntimeError(f"sanitizer finding in grouped GTest: {group.binary}")
    ran, passed, skipped = run_isolated_fallback(index, group)
    return ran, passed, skipped, True


def run_ordinary_test(
    index: int,
    test: OrdinaryCTest,
    build_dir: Path,
) -> tuple[int, int, int]:
    if test.disabled:
        print(
            f"CTEST_DISABLED index={index} name={test.name}",
            flush=True,
        )
        return 0, 0, 0

    print(
        f"CTEST_START index={index} timeout_seconds={test.timeout_seconds:g} "
        f"name={test.name}",
        flush=True,
    )
    command = [
        "ctest",
        "--test-dir",
        str(build_dir),
        "--output-on-failure",
        "--no-tests=error",
        "-R",
        f"^{re.escape(test.name)}$",
    ]
    status, output, timed_out = execute(
        command,
        str(build_dir),
        test.timeout_seconds + 30.0,
    )
    print(output, end="", flush=True)
    print(
        f"CTEST_RESULT index={index} status={status} "
        f"timed_out={int(timed_out)} name={test.name}",
        flush=True,
    )
    if has_sanitizer_finding(status, output):
        raise RuntimeError(
            f"sanitizer finding in ordinary CTest: {test.name}"
        )
    if status != 0 or timed_out:
        raise RuntimeError(f"ordinary CTest execution failed: {test.name}")
    return 1, 1, 0


def main() -> int:
    args = parse_args()
    build_dir = Path(args.build_dir).resolve()
    tests = load_registry(
        build_dir, args.test_regex, args.exclude_test_regex
    )
    groups, ordinary_tests = group_registry(tests, args.timeout_seconds)
    disabled = (
        sum(group.disabled_count for group in groups.values())
        + sum(test.disabled for test in ordinary_tests)
    )
    enabled = (
        sum(len(group.enabled_filters) for group in groups.values())
        + sum(not test.disabled for test in ordinary_tests)
    )
    print(
        f"{args.summary_prefix}_REGISTRY registered_tests={len(tests)} "
        f"enabled_tests={enabled} disabled_tests={disabled} "
        f"unique_binaries={len(groups)} ordinary_tests={len(ordinary_tests)}",
        flush=True,
    )

    total_ran = 0
    total_passed = 0
    total_skipped = 0
    executed_binaries = 0
    fallback_binaries = 0
    for index, group in enumerate(groups.values(), start=1):
        ran, passed, skipped, used_fallback = run_group(index, group)
        if group.enabled_filters:
            executed_binaries += 1
        if used_fallback:
            fallback_binaries += 1
        total_ran += ran
        total_passed += passed
        total_skipped += skipped

    for index, test in enumerate(ordinary_tests, start=1):
        ran, passed, skipped = run_ordinary_test(index, test, build_dir)
        if not test.disabled:
            executed_binaries += 1
        total_ran += ran
        total_passed += passed
        total_skipped += skipped

    print(
        f"{args.summary_prefix}_GROUP_SUMMARY binaries={executed_binaries} "
        f"registered_tests={len(tests)} enabled_tests={enabled} "
        f"disabled_tests={disabled} ran={total_ran} passed={total_passed} "
        f"skipped={total_skipped} fallback_binaries={fallback_binaries}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.SubprocessError, ValueError) as error:
        print(f"grouped GTest runner failed: {error}", file=sys.stderr)
        raise SystemExit(1)
