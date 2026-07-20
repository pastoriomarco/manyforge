#!/usr/bin/env python3
"""Verify the Step 18 execution-kernel repeat-under-load qualification.

The gate deliberately treats the shell-produced manifest as untrusted input:
every retained log and ledger is re-hashed, the complete 30-by-3 liveness
matrix is rechecked, and the native binary/gate script are bound to the
container workspace recorded by the outer run manifest.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import os
import re
import stat
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from source_identity import (
    SourceIdentityError,
    source_identity,
    validate_identity,
)


RUN_SCHEMA = "manyforge.step18.loaded_kernel_run.v1"
RAW_SCHEMA = "manyforge.loaded_kernel_stability_raw.v1"
SUMMARY_SCHEMA = "manyforge.step18.loaded_kernel_verification.v1"
EXPECTED_ITERATIONS = 30
EXPECTED_LOAD_WORKERS = 3
EXPECTED_TIMEOUT_SECONDS = 30
MINIMUM_TESTS_PER_ITERATION = 291
REQUIRED_LOADED_PREDICATE_TESTS = frozenset(
    {
        "ExecutionKernelTest.Cancel_StopDuringValidationRequestsValidationCancel",
        "ExecutionKernelTest.Cancel_ResetDuringValidationRequestsValidationCancel",
        "ExecutionKernelTest.Stop_OperatorProfileRefused_EmergencyLegacyFallback",
        "ExecutionKernelTest.Stop_OperatorDeadlineWatchdogHardStopsBeforeBackendReturns",
    }
)
EXPECTED_INNER_SCRIPT = "\n".join(
    (
        "set -euo pipefail",
        'python3 /workspace/scripts/qualification/source_identity.py --repo-root /workspace --expect-fingerprint-sha256 "${MANYFORGE_EXPECTED_SOURCE_FINGERPRINT_SHA256}" >/dev/null',
        'if [[ -e "${MANYFORGE_CORE_BUILD_DIR}" ]]; then',
        '  echo "loaded-kernel build directory was not fresh: ${MANYFORGE_CORE_BUILD_DIR}" >&2',
        "  exit 2",
        "fi",
        'cmake -S /workspace/manyforge_core -B "${MANYFORGE_STABILITY_TEST_BINARY%/test_execution_kernel}" -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-Werror',
        'cmake --build "${MANYFORGE_STABILITY_TEST_BINARY%/test_execution_kernel}" --target test_execution_kernel --parallel "${MANYFORGE_BUILD_JOBS}"',
        "/workspace/manyforge_core/tools/stability_gate.sh",
        'python3 /workspace/scripts/qualification/source_identity.py --repo-root /workspace --expect-fingerprint-sha256 "${MANYFORGE_EXPECTED_SOURCE_FINGERPRINT_SHA256}" >/dev/null',
    )
)
EXPECTED_CONTAINER_ENVIRONMENT_KEYS = {
    "LD_PRELOAD",
    "MANYFORGE_BUILD_JOBS",
    "MANYFORGE_CORE_BUILD_DIR",
    "MANYFORGE_EXPECTED_SOURCE_FINGERPRINT_SHA256",
    "MANYFORGE_EXPECTED_SPECS_CONTRACT_FINGERPRINT_SHA256",
    "MANYFORGE_STABILITY_ITERATIONS",
    "MANYFORGE_STABILITY_LOAD_WORKERS",
    "MANYFORGE_STABILITY_LOG_DIR",
    "MANYFORGE_STABILITY_MANIFEST",
    "MANYFORGE_STABILITY_PROCESS_INSTANCE_ID",
    "MANYFORGE_STABILITY_RUN_ID",
    "MANYFORGE_STABILITY_TEST_BINARY",
    "MANYFORGE_STABILITY_TIMEOUT_SECONDS",
    "SEGFAULT_SIGNALS",
}
UUID = re.compile(
    r"[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-"
    r"[89ab][0-9a-f]{3}-[0-9a-f]{12}"
)
SHA256 = re.compile(r"[0-9a-f]{64}")
CONTAINER_ID = re.compile(r"[0-9a-f]{64}")
RUN_ID = re.compile(
    r"[0-9]{8}T[0-9]{15}Z-step18-loaded-kernel-(?P<uuid>"
    + UUID.pattern
    + r")"
)
COMPOSE_PROJECT = re.compile(r"[a-z0-9][a-z0-9_-]*")
EXPECTED_LOAD_WORKER_COMMAND = ["bash", "-c", "while :; do :; done"]
ALLOWED_LOAD_WORKER_STATES = {"R", "S", "D", "I"}


class ValidationError(RuntimeError):
    """One retained qualification claim is incomplete or inconsistent."""


@dataclass(frozen=True)
class FileSnapshot:
    """One regular file read once into the buffer used for all validation."""

    path: Path
    label: str
    data: bytes
    sha256: str

    @property
    def size(self) -> int:
        return len(self.data)


def _snapshot_file(
    path: Path,
    label: str,
    snapshots: list[FileSnapshot],
    expected_sha256: Any | None = None,
) -> FileSnapshot:
    path = path.resolve()
    try:
        with path.open("rb") as stream:
            metadata = os.fstat(stream.fileno())
            if not stat.S_ISREG(metadata.st_mode):
                raise ValidationError(f"{label} is not a regular file: {path}")
            data = stream.read()
    except OSError as error:
        raise ValidationError(f"cannot read {label} at {path}: {error}") from error
    digest = hashlib.sha256(data).hexdigest()
    if expected_sha256 is not None and digest != _sha256_value(
        expected_sha256, f"{label} hash"
    ):
        raise ValidationError(f"{label} hash changed")
    snapshot = FileSnapshot(path=path, label=label, data=data, sha256=digest)
    snapshots.append(snapshot)
    return snapshot


def _load_json(snapshot: FileSnapshot) -> dict[str, Any]:
    try:
        value = json.loads(snapshot.data)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValidationError(
            f"{snapshot.label} is not readable JSON: {error}"
        ) from error
    if not isinstance(value, dict):
        raise ValidationError(f"{snapshot.label} must contain one JSON object")
    return value


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as error:
        raise ValidationError(f"cannot hash {path}: {error}") from error
    return digest.hexdigest()


def _sha256_value(value: Any, label: str) -> str:
    if not isinstance(value, str) or SHA256.fullmatch(value) is None:
        raise ValidationError(f"{label} must be a lowercase SHA-256")
    return value


def _exact_keys(value: dict[str, Any], expected: set[str], label: str) -> None:
    actual = set(value)
    if actual != expected:
        raise ValidationError(
            f"{label} fields differ: missing={sorted(expected - actual)} "
            f"unexpected={sorted(actual - expected)}"
        )


def _resolve_file(
    manifest_path: Path,
    value: Any,
    expected_sha256: Any,
    label: str,
    snapshots: list[FileSnapshot],
) -> FileSnapshot:
    if not isinstance(value, str) or not value:
        raise ValidationError(f"{label} path must be a non-empty string")
    candidate = Path(value)
    path = candidate if candidate.is_absolute() else manifest_path.parent / candidate
    path = path.resolve()
    return _snapshot_file(path, label, snapshots, expected_sha256)


def _identity(value: Any, label: str) -> dict[str, Any]:
    try:
        return validate_identity(value)
    except SourceIdentityError as error:
        raise ValidationError(f"{label} is invalid: {error}") from error


def _specs_contract_identity(value: Any, label: str) -> dict[str, Any]:
    """Call the shared helper once its exact contract validator is present."""

    try:
        from source_identity import validate_specs_contract_identity
    except ImportError as error:  # pragma: no cover - transitional tree guard
        raise ValidationError(
            "source_identity helper lacks Specs contract identity support"
        ) from error
    try:
        return validate_specs_contract_identity(value)
    except SourceIdentityError as error:
        raise ValidationError(f"{label} is invalid: {error}") from error


def _current_specs_contract_identity(repo_root: Path) -> dict[str, Any]:
    try:
        from source_identity import specs_contract_identity
    except ImportError as error:  # pragma: no cover - transitional tree guard
        raise ValidationError(
            "source_identity helper lacks Specs contract identity support"
        ) from error
    try:
        return specs_contract_identity(repo_root)
    except SourceIdentityError as error:
        raise ValidationError(f"cannot collect current Specs contract identity: {error}") from error


def _workspace_file(
    container_path_value: Any,
    workspace_host_root: Path,
    expected_sha256: Any,
    label: str,
    snapshots: list[FileSnapshot],
) -> FileSnapshot:
    if not isinstance(container_path_value, str):
        raise ValidationError(f"{label} container path must be a string")
    container_path = Path(container_path_value)
    try:
        relative = container_path.relative_to("/workspace")
    except ValueError as error:
        raise ValidationError(f"{label} is outside /workspace") from error
    host_path = (workspace_host_root / relative).resolve()
    try:
        host_path.relative_to(workspace_host_root)
    except ValueError as error:
        raise ValidationError(f"{label} escapes the bound workspace") from error
    return _snapshot_file(host_path, label, snapshots, expected_sha256)


def _gtest_counts(log: FileSnapshot, iteration: int) -> tuple[int, int]:
    try:
        output = log.data.decode("utf-8", errors="replace")
    except (AttributeError, UnicodeError) as error:
        raise ValidationError(
            f"cannot read iteration {iteration} GTest log: {error}"
        ) from error
    ran_matches = re.findall(
        r"^\[==========\] ([0-9]+) tests? from .* ran\.",
        output,
        flags=re.MULTILINE,
    )
    passed_matches = re.findall(
        r"^\[  PASSED  \] ([0-9]+) tests?\.",
        output,
        flags=re.MULTILINE,
    )
    if (
        len(ran_matches) != 1
        or len(passed_matches) != 1
        or int(ran_matches[0]) <= 0
        or ran_matches[0] != passed_matches[0]
        or re.search(r"^\[  FAILED  \]", output, flags=re.MULTILINE)
    ):
        raise ValidationError(
            f"iteration {iteration} does not prove one complete passing GTest run"
        )
    return int(ran_matches[0]), int(passed_matches[0])


def _gtest_inventory(snapshot: FileSnapshot) -> list[str]:
    try:
        output = snapshot.data.decode("utf-8", errors="strict")
    except UnicodeError as error:
        raise ValidationError(f"cannot decode GTest inventory: {error}") from error
    suite: str | None = None
    tests: list[str] = []
    for line in output.splitlines():
        if not line.strip():
            continue
        if not line[:1].isspace():
            suite_name = line.split("#", 1)[0].strip()
            if not suite_name.endswith("."):
                raise ValidationError("GTest inventory contains a malformed suite")
            suite = suite_name
            continue
        test_name = line.split("#", 1)[0].strip()
        if suite is None or not test_name:
            raise ValidationError("GTest inventory contains an orphan test")
        tests.append(suite + test_name)
    if not tests or len(tests) != len(set(tests)):
        raise ValidationError("GTest inventory is empty or contains duplicates")
    missing = REQUIRED_LOADED_PREDICATE_TESTS - set(tests)
    if missing:
        raise ValidationError(
            "loaded-kernel inventory omits required cancellation/fallback predicates: "
            + ", ".join(sorted(missing))
        )
    return tests


def _validate_worker_snapshot(
    value: Any, label: str, expected_ordinals: list[int]
) -> list[dict[str, Any]]:
    if not isinstance(value, list) or len(value) != EXPECTED_LOAD_WORKERS:
        raise ValidationError(f"{label} must retain exactly three workers")
    expected_fields = {
        "ordinal",
        "pid",
        "proc_starttime_ticks",
        "state",
        "state_observations",
        "command_argv",
    }
    normalized: list[dict[str, Any]] = []
    for expected_ordinal, record in zip(expected_ordinals, value, strict=True):
        if not isinstance(record, dict):
            raise ValidationError(f"{label} worker {expected_ordinal} is not an object")
        _exact_keys(record, expected_fields, f"{label} worker {expected_ordinal}")
        pid = record.get("pid")
        starttime = record.get("proc_starttime_ticks")
        state = record.get("state")
        observations = record.get("state_observations")
        command = record.get("command_argv")
        if (
            record.get("ordinal") != expected_ordinal
            or not isinstance(pid, int)
            or isinstance(pid, bool)
            or pid <= 0
            or not isinstance(starttime, int)
            or isinstance(starttime, bool)
            or starttime <= 0
            or state not in ALLOWED_LOAD_WORKER_STATES
            or not isinstance(observations, list)
            or len(observations) != 2
            or any(value not in ALLOWED_LOAD_WORKER_STATES for value in observations)
            or command != EXPECTED_LOAD_WORKER_COMMAND
        ):
            raise ValidationError(
                f"{label} worker {expected_ordinal} PID/starttime/state/command is invalid"
            )
        normalized.append(record)
    if len({record["pid"] for record in normalized}) != EXPECTED_LOAD_WORKERS:
        raise ValidationError(f"{label} contains duplicate worker PIDs")
    if (
        len(
            {
                (record["pid"], record["proc_starttime_ticks"])
                for record in normalized
            }
        )
        != EXPECTED_LOAD_WORKERS
    ):
        raise ValidationError(f"{label} contains reused worker process identities")
    return normalized


def _worker_identities(
    records: list[dict[str, Any]],
) -> list[tuple[int, int, tuple[str, ...]]]:
    return [
        (
            record["pid"],
            record["proc_starttime_ticks"],
            tuple(record["command_argv"]),
        )
        for record in records
    ]


def _verify_raw_manifest(
    raw_snapshot: FileSnapshot,
    workspace_host_root: Path,
    expected_cpuset: str,
    expected_run_id: str,
    expected_process_instance_id: str,
    expected_gate_sha256: str,
    snapshots: list[FileSnapshot],
) -> dict[str, Any]:
    raw_path = raw_snapshot.path
    raw = _load_json(raw_snapshot)
    _exact_keys(
        raw,
        {
            "schema_version",
            "generated_at_utc",
            "run_id",
            "process_instance_id",
            "configuration",
            "command_argv",
            "test_binary",
            "gate_script",
            "test_inventory",
            "environment",
            "ledger",
            "iterations",
            "passed",
        },
        "raw stability manifest",
    )
    if raw["schema_version"] != RAW_SCHEMA or raw["passed"] is not True:
        raise ValidationError("raw stability manifest is not a passing v1 artifact")
    if raw["run_id"] != expected_run_id:
        raise ValidationError("raw stability run id differs from the outer run")
    if raw["process_instance_id"] != expected_process_instance_id:
        raise ValidationError("raw stability process UUID differs from the outer run")

    config = raw.get("configuration")
    if not isinstance(config, dict):
        raise ValidationError("raw stability configuration must be an object")
    _exact_keys(
        config,
        {"iterations", "load_workers", "timeout_seconds", "cpus_allowed_list"},
        "raw stability configuration",
    )
    if (
        config["iterations"] != EXPECTED_ITERATIONS
        or config["load_workers"] != EXPECTED_LOAD_WORKERS
        or config["timeout_seconds"] != EXPECTED_TIMEOUT_SECONDS
        or config["cpus_allowed_list"] != expected_cpuset
    ):
        raise ValidationError("raw stability execution profile is not exactly 30x3/30s/cpuset")

    binary = raw.get("test_binary")
    gate = raw.get("gate_script")
    environment = raw.get("environment")
    ledger = raw.get("ledger")
    inventory = raw.get("test_inventory")
    for value, label, fields in (
        (binary, "test_binary", {"path", "sha256"}),
        (gate, "gate_script", {"path", "sha256"}),
        (
            environment,
            "environment",
            {"ld_preload", "segfault_signals", "gtest_overrides_present"},
        ),
        (ledger, "ledger", {"name", "sha256", "bytes"}),
        (inventory, "test_inventory", {"name", "sha256", "bytes"}),
    ):
        if not isinstance(value, dict):
            raise ValidationError(f"raw {label} must be an object")
        _exact_keys(value, fields, f"raw {label}")
    binary_snapshot = _workspace_file(
        binary["path"],
        workspace_host_root,
        binary["sha256"],
        "test binary",
        snapshots,
    )
    if binary_snapshot.path.name != "test_execution_kernel":
        raise ValidationError("loaded-kernel binary is not test_execution_kernel")
    gate_snapshot = _workspace_file(
        gate["path"],
        workspace_host_root,
        gate["sha256"],
        "gate script",
        snapshots,
    )
    if gate_snapshot.sha256 != expected_gate_sha256:
        raise ValidationError("raw manifest used a different gate script")
    if raw.get("command_argv") != [
        str(binary["path"]),
        "--gtest_filter=*",
        "--gtest_repeat=1",
        "--gtest_brief=1",
    ]:
        raise ValidationError("loaded-kernel command argv changed")
    if environment["gtest_overrides_present"] != []:
        raise ValidationError("loaded-kernel GTest environment was not neutralized")
    if environment["ld_preload"] != "" or environment["segfault_signals"] != "":
        raise ValidationError("loaded-kernel preload environment was not neutralized")

    inventory_name = inventory["name"]
    if (
        not isinstance(inventory_name, str)
        or Path(inventory_name).name != inventory_name
        or inventory_name != "test_execution_kernel.inventory.txt"
    ):
        raise ValidationError("raw test inventory name is not canonical")
    inventory_path = raw_path.parent / inventory_name
    inventory_snapshot = _snapshot_file(
        inventory_path, "execution-kernel test inventory", snapshots, inventory["sha256"]
    )
    if (
        inventory_snapshot.size != inventory["bytes"]
        or isinstance(inventory["bytes"], bool)
        or not isinstance(inventory["bytes"], int)
        or inventory["bytes"] <= 0
    ):
        raise ValidationError("raw test inventory bytes or hash changed")
    inventory_tests = _gtest_inventory(inventory_snapshot)

    ledger_name = ledger["name"]
    if (
        not isinstance(ledger_name, str)
        or Path(ledger_name).name != ledger_name
        or ledger_name != "stability-gate-ledger.tsv"
    ):
        raise ValidationError("raw ledger name is not canonical")
    ledger_path = raw_path.parent / ledger_name
    ledger_snapshot = _snapshot_file(
        ledger_path, "raw stability ledger", snapshots, ledger["sha256"]
    )
    if (
        ledger_snapshot.size != ledger["bytes"]
        or isinstance(ledger["bytes"], bool)
        or not isinstance(ledger["bytes"], int)
        or ledger["bytes"] <= 0
    ):
        raise ValidationError("raw ledger bytes or hash changed")

    records = raw.get("iterations")
    if not isinstance(records, list) or len(records) != EXPECTED_ITERATIONS:
        raise ValidationError("loaded-kernel manifest must retain exactly 30 iterations")
    expected_ordinals = list(range(1, EXPECTED_LOAD_WORKERS + 1))
    retained_logs: list[dict[str, Any]] = []
    tests_per_iteration: set[int] = set()
    expected_record_fields = {
        "iteration",
        "exit_status",
        "duration_ns",
        "log_name",
        "log_sha256",
        "log_bytes",
        "load_worker_evidence_before",
        "load_worker_evidence_after",
    }
    try:
        ledger_text = ledger_snapshot.data.decode("utf-8", errors="strict")
        ledger_reader = csv.DictReader(io.StringIO(ledger_text, newline=""), delimiter="\t")
        if ledger_reader.fieldnames != [
            "iteration",
            "status",
            "duration_ns",
            "log_name",
            "log_sha256",
            "log_bytes",
            "before_workers_json",
            "after_workers_json",
        ]:
            raise ValidationError("raw ledger header changed")
        ledger_rows = list(ledger_reader)
    except UnicodeError as error:
        raise ValidationError(f"cannot read raw ledger: {error}") from error
    if len(ledger_rows) != EXPECTED_ITERATIONS:
        raise ValidationError("raw ledger must retain exactly 30 iterations")
    for index, record in enumerate(records, start=1):
        if not isinstance(record, dict):
            raise ValidationError(f"iteration {index} is not an object")
        _exact_keys(record, expected_record_fields, f"iteration {index}")
        expected_log_name = f"test_execution_kernel.{index}.log"
        if (
            record["iteration"] != index
            or record["exit_status"] != 0
            or not isinstance(record["duration_ns"], int)
            or isinstance(record["duration_ns"], bool)
            or record["duration_ns"] <= 0
            or record["log_name"] != expected_log_name
        ):
            raise ValidationError(f"iteration {index} status/order is invalid")
        before_workers = _validate_worker_snapshot(
            record["load_worker_evidence_before"],
            f"iteration {index} before-worker evidence",
            expected_ordinals,
        )
        after_workers = _validate_worker_snapshot(
            record["load_worker_evidence_after"],
            f"iteration {index} after-worker evidence",
            expected_ordinals,
        )
        before_identities = _worker_identities(before_workers)
        after_identities = _worker_identities(after_workers)
        if before_identities != after_identities:
            raise ValidationError(
                f"iteration {index} load-worker PID/starttime/command changed"
            )
        if index == 1:
            reference_worker_identities = before_identities
        elif before_identities != reference_worker_identities:
            raise ValidationError(
                f"iteration {index} load-worker identity was reused or replaced"
            )
        ledger_row = ledger_rows[index - 1]
        expected_ledger_row = {
            "iteration": str(record["iteration"]),
            "status": str(record["exit_status"]),
            "duration_ns": str(record["duration_ns"]),
            "log_name": record["log_name"],
            "log_sha256": record["log_sha256"],
            "log_bytes": str(record["log_bytes"]),
            "before_workers_json": json.dumps(
                before_workers, sort_keys=True, separators=(",", ":")
            ),
            "after_workers_json": json.dumps(
                after_workers, sort_keys=True, separators=(",", ":")
            ),
        }
        if ledger_row != expected_ledger_row:
            raise ValidationError(f"iteration {index} differs from the retained ledger")
        log_path = raw_path.parent / expected_log_name
        log_snapshot = _snapshot_file(
            log_path,
            f"iteration {index} GTest log",
            snapshots,
            record["log_sha256"],
        )
        if (
            log_snapshot.size != record["log_bytes"]
            or isinstance(record["log_bytes"], bool)
            or not isinstance(record["log_bytes"], int)
            or record["log_bytes"] <= 0
        ):
            raise ValidationError(f"iteration {index} log bytes/hash are invalid")
        tests_ran, tests_passed = _gtest_counts(log_snapshot, index)
        tests_per_iteration.add(tests_ran)
        retained_logs.append(
            {
                "iteration": index,
                "path": str(log_path),
                "sha256": log_snapshot.sha256,
                "bytes": record["log_bytes"],
                "duration_ns": record["duration_ns"],
                "tests_ran": tests_ran,
                "tests_passed": tests_passed,
                "load_workers_alive_before": EXPECTED_LOAD_WORKERS,
                "load_workers_alive_after": EXPECTED_LOAD_WORKERS,
                "load_worker_evidence_before": before_workers,
                "load_worker_evidence_after": after_workers,
            }
        )
    if len(tests_per_iteration) != 1:
        raise ValidationError("loaded-kernel GTest count changed across iterations")
    tests_ran = next(iter(tests_per_iteration))
    if tests_ran < MINIMUM_TESTS_PER_ITERATION:
        raise ValidationError(
            "loaded-kernel run did not execute the complete execution-kernel suite"
        )
    if tests_ran != len(inventory_tests):
        raise ValidationError(
            "loaded-kernel executed test count differs from its retained inventory"
        )
    return {
        "raw": raw,
        "binary_path": str(binary_snapshot.path),
        "binary_sha256": binary_snapshot.sha256,
        "gate_path": str(gate_snapshot.path),
        "gate_sha256": gate_snapshot.sha256,
        "ledger_path": str(ledger_path),
        "ledger_sha256": ledger_snapshot.sha256,
        "inventory_path": str(inventory_path),
        "inventory_sha256": inventory_snapshot.sha256,
        "required_loaded_predicate_tests": sorted(REQUIRED_LOADED_PREDICATE_TESTS),
        "logs": retained_logs,
        "tests_per_iteration": tests_ran,
    }


def _container_path_for_host(path: Path, workspace_host_root: Path, label: str) -> str:
    try:
        relative = path.resolve().relative_to(workspace_host_root)
    except ValueError as error:
        raise ValidationError(f"{label} is outside the bound workspace") from error
    return str(Path("/workspace") / relative)


def _canonical_cpuset(value: Any) -> str:
    if not isinstance(value, str) or not re.fullmatch(
        r"[0-9]+(?:-[0-9]+)?(?:,[0-9]+(?:-[0-9]+)?)*", value
    ):
        raise ValidationError("requested CPU affinity is not canonical")
    cpus: set[int] = set()
    for part in value.split(","):
        if "-" in part:
            first_raw, last_raw = part.split("-", 1)
            first, last = int(first_raw), int(last_raw)
            if first > last:
                raise ValidationError("requested CPU affinity has a descending range")
            cpus.update(range(first, last + 1))
        else:
            cpus.add(int(part))
    if len(cpus) != 4:
        raise ValidationError("loaded-kernel affinity must contain exactly four CPUs")
    ordered = sorted(cpus)
    ranges: list[str] = []
    start = previous = ordered[0]
    for cpu in ordered[1:]:
        if cpu == previous + 1:
            previous = cpu
            continue
        ranges.append(str(start) if start == previous else f"{start}-{previous}")
        start = previous = cpu
    ranges.append(str(start) if start == previous else f"{start}-{previous}")
    canonical = ",".join(ranges)
    if value != canonical:
        raise ValidationError("requested CPU affinity is not minimally canonical")
    return canonical


def _validate_expected_mounts(
    value: Any, workspace_host_root: Path, compose_project_name: str
) -> list[dict[str, Any]]:
    if not isinstance(value, list) or len(value) != 2:
        raise ValidationError("expected mount inventory must contain exactly two mounts")
    normalized: list[dict[str, Any]] = []
    for index, mount in enumerate(value, start=1):
        if not isinstance(mount, dict):
            raise ValidationError(f"expected mount {index} is not an object")
        _exact_keys(
            mount,
            {"type", "source", "destination", "rw"},
            f"expected mount {index}",
        )
        kind = mount.get("type")
        source = mount.get("source")
        destination = mount.get("destination")
        rw = mount.get("rw")
        if (
            kind not in {"bind", "volume"}
            or not isinstance(source, str)
            or not source
            or not isinstance(destination, str)
            or not destination.startswith("/")
            or str(Path(destination)) != destination
            or rw is not True
        ):
            raise ValidationError(f"expected mount {index} is not canonical read-write provenance")
        normalized.append(
            {
                "type": kind,
                "source": (
                    str(Path(source).resolve()) if kind == "bind" else source
                ),
                "destination": destination,
                "rw": rw,
            }
        )
    normalized.sort(
        key=lambda mount: (mount["destination"], mount["type"], mount["source"])
    )
    if value != normalized:
        raise ValidationError("expected mount inventory is not canonically ordered")
    expected = [
        {
            "type": "volume",
            "source": f"{compose_project_name}_build-cache",
            "destination": "/tmp/manyforge-build",
            "rw": True,
        },
        {
            "type": "bind",
            "source": str(workspace_host_root),
            "destination": "/workspace",
            "rw": True,
        },
    ]
    if normalized != expected:
        raise ValidationError("expected mount inventory differs from the exact dev lane")
    return normalized


def _inspect_container(
    inspect_snapshot: FileSnapshot,
    expected_container_id: str,
    expected_container_name: str,
    expected_image_id: str,
    expected_image_reference: str,
    expected_compose_project: str,
    workspace_host_root: Path,
    expected_environment: dict[str, str],
    expected_command_argv: list[str],
    expected_labels: dict[str, str],
    expected_mounts: list[dict[str, Any]],
) -> dict[str, Any]:
    try:
        value = json.loads(inspect_snapshot.data)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValidationError(f"container inspect is invalid: {error}") from error
    if not isinstance(value, list) or len(value) != 1 or not isinstance(value[0], dict):
        raise ValidationError("container inspect must contain exactly one object")
    record = value[0]
    image = str(record.get("Image", "")).removeprefix("sha256:")
    name = str(record.get("Name", "")).removeprefix("/")
    state = record.get("State")
    config = record.get("Config")
    mounts = record.get("Mounts")
    if (
        record.get("Id") != expected_container_id
        or image != expected_image_id
        or name != expected_container_name
        or not isinstance(state, dict)
        or state.get("ExitCode") != 0
        or state.get("OOMKilled") is not False
        or state.get("Error") not in ("", None)
    ):
        raise ValidationError("container identity or successful terminal state differs")
    if (
        not isinstance(config, dict)
        or config.get("Image") != expected_image_reference
        or not isinstance(config.get("Env"), list)
        or not isinstance(config.get("Labels"), dict)
    ):
        raise ValidationError("container inspect omits its configured environment")
    configured_command = [record.get("Path"), *record.get("Args", [])]
    if configured_command != expected_command_argv:
        raise ValidationError("container command argv differs from the frozen run manifest")
    actual_environment: dict[str, str] = {}
    for entry in config["Env"]:
        if isinstance(entry, str) and "=" in entry:
            key, value = entry.split("=", 1)
            if key in actual_environment:
                raise ValidationError(f"container environment duplicates {key}")
            actual_environment[key] = value
    for key, expected in expected_environment.items():
        if actual_environment.get(key) != expected:
            raise ValidationError(f"container environment {key} differs")
    actual_labels = config["Labels"]
    for key, expected in expected_labels.items():
        if actual_labels.get(key) != expected:
            raise ValidationError(f"container qualification label {key} differs")
    unexpected_qualification_labels = {
        key
        for key in actual_labels
        if key.startswith("io.manyforge.qualification.") and key not in expected_labels
    }
    if unexpected_qualification_labels:
        raise ValidationError(
            "container has unexpected qualification labels: "
            f"{sorted(unexpected_qualification_labels)}"
        )
    for key, expected in {
        "com.docker.compose.project": expected_compose_project,
        "com.docker.compose.service": "dev",
        "com.docker.compose.oneoff": "True",
    }.items():
        if actual_labels.get(key) != expected:
            raise ValidationError(f"container Compose label {key} differs")

    if not isinstance(mounts, list):
        raise ValidationError("container inspect omits mounts")
    normalized_mounts: list[dict[str, Any]] = []
    destinations: set[str] = set()
    for index, mount in enumerate(mounts, start=1):
        if not isinstance(mount, dict):
            raise ValidationError(f"container mount {index} is malformed")
        kind = mount.get("Type")
        runtime_source = mount.get("Source")
        destination = mount.get("Destination")
        rw = mount.get("RW")
        if (
            kind not in {"bind", "volume"}
            or not isinstance(runtime_source, str)
            or not runtime_source
            or not isinstance(destination, str)
            or not destination.startswith("/")
            or str(Path(destination)) != destination
            or not isinstance(rw, bool)
        ):
            raise ValidationError(f"container mount {index} is not normalized provenance")
        if destination in destinations:
            raise ValidationError(f"container mount destination is duplicated: {destination}")
        destinations.add(destination)
        if destination.startswith("/workspace/"):
            raise ValidationError(
                f"container has a nested mount shadowing the workspace: {destination}"
            )
        if kind == "volume":
            source = mount.get("Name")
            if (
                not isinstance(source, str)
                or not source
                or not runtime_source.startswith("/")
                or str(Path(runtime_source)) != runtime_source
            ):
                raise ValidationError(
                    f"container volume mount {index} omits its logical name/backing source"
                )
        else:
            source = str(Path(runtime_source).resolve())
        normalized_mounts.append(
            {
                "type": kind,
                "source": source,
                "destination": destination,
                "rw": rw,
            }
        )
    normalized_mounts.sort(
        key=lambda mount: (mount["destination"], mount["type"], mount["source"])
    )
    if normalized_mounts != expected_mounts:
        raise ValidationError("container exact mount inventory/RW differs")
    return {
        "id": expected_container_id,
        "name": expected_container_name,
        "image_id": expected_image_id,
        "image_reference": expected_image_reference,
        "compose_project_name": expected_compose_project,
        "compose_service": "dev",
        "qualification_labels": expected_labels,
        "mounts": normalized_mounts,
        "workspace_source": str(workspace_host_root),
        "exit_code": 0,
        "oom_killed": False,
    }


def _validate_cli_output_path(output_path: Path, manyforge_root: Path) -> Path:
    resolved = output_path.resolve()
    qualification_root = (
        manyforge_root / ".manyforge-var" / "qualification"
    ).resolve()
    try:
        relative = resolved.relative_to(qualification_root)
    except ValueError as error:
        raise ValidationError(
            "CLI output must be below .manyforge-var/qualification"
        ) from error
    if not relative.parts:
        raise ValidationError("CLI output must name a file below the qualification root")
    ignored = subprocess.run(
        ["git", "-C", str(manyforge_root), "check-ignore", "--quiet", "--", str(resolved)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if ignored.returncode != 0:
        raise ValidationError(
            "CLI output is not covered by the candidate checkout's Git ignore rules"
        )
    return resolved


def _assert_snapshots_unchanged(snapshots: list[FileSnapshot]) -> None:
    expected_by_path: dict[Path, tuple[str, str]] = {}
    for snapshot in snapshots:
        previous = expected_by_path.get(snapshot.path)
        if previous is not None and previous[0] != snapshot.sha256:
            raise ValidationError(
                f"{snapshot.label} changed between buffered verifier reads"
            )
        expected_by_path[snapshot.path] = (snapshot.sha256, snapshot.label)
    for path, (expected_digest, label) in expected_by_path.items():
        if _sha256(path) != expected_digest:
            raise ValidationError(f"{label} changed during verification")


def _assert_retained_artifact_manifest_current(value: Any) -> None:
    if not isinstance(value, list) or len(value) != 39:
        raise ValidationError(
            "loaded-kernel retained artifact manifest must contain exactly 39 files"
        )
    paths: list[Path] = []
    roles: list[str] = []
    for index, item in enumerate(value):
        if not isinstance(item, dict) or set(item) not in (
            {"role", "path", "sha256"},
            {"role", "iteration", "path", "sha256"},
        ):
            raise ValidationError(
                f"loaded-kernel retained artifact {index} has unexpected fields"
            )
        role = item.get("role")
        if not isinstance(role, str) or not role:
            raise ValidationError(
                f"loaded-kernel retained artifact {index} has no role"
            )
        path = Path(str(item.get("path", ""))).resolve()
        if not path.is_file():
            raise ValidationError(f"loaded-kernel retained artifact disappeared: {path}")
        if _sha256(path) != _sha256_value(
            item.get("sha256"), f"loaded-kernel retained artifact {index}"
        ):
            raise ValidationError(f"loaded-kernel retained artifact changed: {path}")
        paths.append(path)
        roles.append(role)
    if len(set(paths)) != len(paths):
        raise ValidationError(
            "loaded-kernel retained artifact paths are not globally unique"
        )
    expected_singletons = {
        "loaded_run_manifest",
        "loaded_raw_manifest",
        "loaded_container_inspect",
        "loaded_container_log",
        "loaded_test_binary",
        "loaded_gate_script",
        "loaded_verifier_script",
        "loaded_ledger",
        "loaded_test_inventory",
    }
    if (
        {role for role in roles if role != "loaded_iteration_log"}
        != expected_singletons
        or any(roles.count(role) != 1 for role in expected_singletons)
        or roles.count("loaded_iteration_log") != EXPECTED_ITERATIONS
    ):
        raise ValidationError("loaded-kernel retained artifact roles are not exact")
    iterations = [
        item.get("iteration")
        for item in value
        if item.get("role") == "loaded_iteration_log"
    ]
    if iterations != list(range(1, EXPECTED_ITERATIONS + 1)):
        raise ValidationError(
            "loaded-kernel retained iteration-log inventory is not ordered and complete"
        )


def verify(
    manifest_path: Path, *, cli_output_path: Path | None = None
) -> dict[str, Any]:
    snapshots: list[FileSnapshot] = []
    manifest_path = manifest_path.resolve()
    manifest_snapshot = _snapshot_file(
        manifest_path, "loaded-kernel run manifest", snapshots
    )
    manifest = _load_json(manifest_snapshot)
    _exact_keys(
        manifest,
        {
            "schema_version",
            "run_id",
            "process_instance_id",
            "paths",
            "identities",
            "execution",
            "hashes",
        },
        "loaded-kernel run manifest",
    )
    if manifest["schema_version"] != RUN_SCHEMA:
        raise ValidationError(f"run manifest schema must be {RUN_SCHEMA}")
    run_id = manifest.get("run_id")
    process_instance_id = manifest.get("process_instance_id")
    run_id_match = RUN_ID.fullmatch(run_id) if isinstance(run_id, str) else None
    if (
        not isinstance(process_instance_id, str)
        or UUID.fullmatch(process_instance_id) is None
        or run_id_match is None
        or run_id_match.group("uuid") != process_instance_id
    ):
        raise ValidationError(
            "run id must contain the exact process instance UUID"
        )

    paths = manifest.get("paths")
    identities = manifest.get("identities")
    execution = manifest.get("execution")
    hashes = manifest.get("hashes")
    for value, label in (
        (paths, "paths"),
        (identities, "identities"),
        (execution, "execution"),
        (hashes, "hashes"),
    ):
        if not isinstance(value, dict):
            raise ValidationError(f"run manifest {label} must be an object")
    _exact_keys(
        paths,
        {
            "manyforge_root",
            "specs_root",
            "raw_manifest",
            "container_inspect",
            "container_log",
        },
        "run manifest paths",
    )
    _exact_keys(
        identities,
        {
            "manyforge_before",
            "manyforge_after",
            "specs_before",
            "specs_after",
            "specs_contract_before",
            "specs_contract_after",
        },
        "run manifest identities",
    )
    _exact_keys(
        execution,
        {
            "requested_cpu_affinity",
            "host_kernel_id",
            "cpu_governor_set",
            "cpu_frequency_driver_set",
            "compose_service",
            "container_id",
            "container_name",
            "image_id",
            "image_reference",
            "compose_project_name",
            "container_labels",
            "expected_mounts",
            "outer_command_argv",
            "container_command_argv",
            "container_environment",
        },
        "run manifest execution",
    )
    _exact_keys(
        hashes,
        {
            "runner_script_sha256",
            "source_identity_helper_sha256",
            "verifier_script_sha256",
            "gate_script_sha256",
            "compose_file_sha256",
            "raw_manifest_sha256",
            "container_inspect_sha256",
            "container_log_sha256",
        },
        "run manifest hashes",
    )

    if not isinstance(paths["manyforge_root"], str) or not isinstance(
        paths["specs_root"], str
    ):
        raise ValidationError("run manifest source roots must be strings")
    manyforge_root = Path(paths["manyforge_root"]).resolve()
    specs_root = Path(paths["specs_root"]).resolve()
    if not manyforge_root.is_dir() or not specs_root.is_dir():
        raise ValidationError("run manifest source roots are unavailable")
    if cli_output_path is not None:
        _validate_cli_output_path(cli_output_path, manyforge_root)
    manyforge_before = _identity(identities["manyforge_before"], "manyforge_before")
    manyforge_after = _identity(identities["manyforge_after"], "manyforge_after")
    specs_before = _identity(identities["specs_before"], "specs_before")
    specs_after = _identity(identities["specs_after"], "specs_after")
    specs_contract_before = _specs_contract_identity(
        identities["specs_contract_before"], "specs_contract_before"
    )
    specs_contract_after = _specs_contract_identity(
        identities["specs_contract_after"], "specs_contract_after"
    )
    if manyforge_before != manyforge_after:
        raise ValidationError("ManyForge source changed during the loaded-kernel run")
    if specs_before != specs_after:
        raise ValidationError("Specs source changed during the loaded-kernel run")
    if specs_contract_before != specs_contract_after:
        raise ValidationError("Specs contract changed during the loaded-kernel run")
    try:
        current_manyforge = source_identity(manyforge_root)
    except SourceIdentityError as error:
        raise ValidationError(f"cannot collect current ManyForge identity: {error}") from error
    if current_manyforge != manyforge_after:
        raise ValidationError("current ManyForge source differs from the loaded-kernel candidate")
    current_specs_contract = _current_specs_contract_identity(specs_root)
    if current_specs_contract != specs_contract_after:
        raise ValidationError("current Specs contract differs from the loaded-kernel candidate")

    cpuset = _canonical_cpuset(execution.get("requested_cpu_affinity"))
    for field in ("host_kernel_id", "cpu_governor_set", "cpu_frequency_driver_set"):
        if not isinstance(execution.get(field), str) or not execution[field]:
            raise ValidationError(f"execution {field} is missing")
    if execution.get("compose_service") != "dev":
        raise ValidationError("loaded-kernel gate did not use the dev container lane")
    container_id = execution.get("container_id")
    container_name = execution.get("container_name")
    image_id = str(execution.get("image_id", "")).removeprefix("sha256:")
    image_reference = execution.get("image_reference")
    compose_project_name = execution.get("compose_project_name")
    if (
        not isinstance(container_id, str)
        or CONTAINER_ID.fullmatch(container_id) is None
        or not isinstance(container_name, str)
        or container_name != f"manyforge-step18-loaded-{process_instance_id}"
        or SHA256.fullmatch(image_id) is None
        or not isinstance(compose_project_name, str)
        or COMPOSE_PROJECT.fullmatch(compose_project_name) is None
        or image_reference != f"{compose_project_name}-dev"
    ):
        raise ValidationError(
            "container id/name/image/project identity is invalid or unrelated to the run UUID"
        )
    container_labels = execution.get("container_labels")
    expected_labels = {
        "io.manyforge.qualification.kind": "step18-loaded-kernel",
        "io.manyforge.qualification.run_id": run_id,
        "io.manyforge.qualification.process_instance_id": process_instance_id,
        "io.manyforge.qualification.source_fingerprint_sha256": manyforge_after[
            "fingerprint_sha256"
        ],
        "io.manyforge.qualification.specs_contract_fingerprint_sha256": specs_contract_after[
            "contract_fingerprint_sha256"
        ],
        "io.manyforge.qualification.compose_project": compose_project_name,
    }
    if container_labels != expected_labels:
        raise ValidationError("container qualification labels differ from the exact run")
    expected_mounts = _validate_expected_mounts(
        execution.get("expected_mounts"), manyforge_root, compose_project_name
    )
    if not isinstance(execution.get("outer_command_argv"), list) or not all(
        isinstance(value, str) and value for value in execution["outer_command_argv"]
    ):
        raise ValidationError("outer command argv is not an exact non-empty argv")
    if not isinstance(execution.get("container_command_argv"), list) or not all(
        isinstance(value, str) and value
        for value in execution["container_command_argv"]
    ):
        raise ValidationError("container command argv is not an exact non-empty argv")
    expected_container_command = [
        "taskset",
        "-c",
        cpuset,
        "bash",
        "-lc",
        EXPECTED_INNER_SCRIPT,
    ]
    if execution["container_command_argv"] != expected_container_command:
        raise ValidationError("container command is not the exact fresh-build gate")
    container_environment = execution.get("container_environment")
    if not isinstance(container_environment, dict) or not container_environment or not all(
        isinstance(key, str)
        and key
        and isinstance(value, str)
        for key, value in container_environment.items()
    ):
        raise ValidationError("container environment provenance is invalid")
    _exact_keys(
        container_environment,
        EXPECTED_CONTAINER_ENVIRONMENT_KEYS,
        "container qualification environment",
    )

    raw_snapshot = _resolve_file(
        manifest_path,
        paths["raw_manifest"],
        hashes["raw_manifest_sha256"],
        "raw stability manifest",
        snapshots,
    )
    inspect_snapshot = _resolve_file(
        manifest_path,
        paths["container_inspect"],
        hashes["container_inspect_sha256"],
        "container inspect",
        snapshots,
    )
    log_snapshot = _resolve_file(
        manifest_path,
        paths["container_log"],
        hashes["container_log_sha256"],
        "container log",
        snapshots,
    )
    if log_snapshot.size <= 0:
        raise ValidationError("container log is empty")

    gate_sha256 = _sha256_value(hashes["gate_script_sha256"], "gate script hash")
    raw_result = _verify_raw_manifest(
        raw_snapshot,
        manyforge_root,
        cpuset,
        run_id,
        process_instance_id,
        gate_sha256,
        snapshots,
    )

    raw_binary_container_path = raw_result["raw"]["test_binary"]["path"]
    expected_log_dir = _container_path_for_host(
        raw_snapshot.path.parent, manyforge_root, "raw stability directory"
    )
    expected_raw_manifest = _container_path_for_host(
        raw_snapshot.path, manyforge_root, "raw stability manifest"
    )
    build_jobs = container_environment["MANYFORGE_BUILD_JOBS"]
    if not re.fullmatch(r"[1-9][0-9]*", build_jobs):
        raise ValidationError("container build-job count is not a positive integer")
    expected_environment = {
        "LD_PRELOAD": "",
        "MANYFORGE_BUILD_JOBS": build_jobs,
        "MANYFORGE_CORE_BUILD_DIR": str(Path(raw_binary_container_path).parent),
        "MANYFORGE_EXPECTED_SOURCE_FINGERPRINT_SHA256": manyforge_after[
            "fingerprint_sha256"
        ],
        "MANYFORGE_EXPECTED_SPECS_CONTRACT_FINGERPRINT_SHA256": specs_contract_after[
            "contract_fingerprint_sha256"
        ],
        "MANYFORGE_STABILITY_ITERATIONS": str(EXPECTED_ITERATIONS),
        "MANYFORGE_STABILITY_LOAD_WORKERS": str(EXPECTED_LOAD_WORKERS),
        "MANYFORGE_STABILITY_LOG_DIR": expected_log_dir,
        "MANYFORGE_STABILITY_MANIFEST": expected_raw_manifest,
        "MANYFORGE_STABILITY_PROCESS_INSTANCE_ID": process_instance_id,
        "MANYFORGE_STABILITY_RUN_ID": run_id,
        "MANYFORGE_STABILITY_TEST_BINARY": raw_binary_container_path,
        "MANYFORGE_STABILITY_TIMEOUT_SECONDS": str(EXPECTED_TIMEOUT_SECONDS),
        "SEGFAULT_SIGNALS": "",
    }
    if container_environment != expected_environment:
        raise ValidationError("container qualification environment values differ")

    runner_path = manyforge_root / "scripts/run-step18-loaded-kernel-qualification.sh"
    helper_path = manyforge_root / "scripts/qualification/source_identity.py"
    verifier_path = manyforge_root / "scripts/qualification/verify_step18_loaded_kernel.py"
    gate_path = manyforge_root / "manyforge_core/tools/stability_gate.sh"
    compose_path = manyforge_root / "docker-compose.yaml"
    expected_outer_command = [
        "docker",
        "compose",
        "--project-name",
        compose_project_name,
        "-f",
        str(compose_path),
        "run",
        "--detach",
        "--no-deps",
        "-T",
        "--name",
        container_name,
    ]
    for key, value in sorted(expected_environment.items()):
        expected_outer_command.extend(("-e", f"{key}={value}"))
    for key, value in sorted(expected_labels.items()):
        expected_outer_command.extend(("--label", f"{key}={value}"))
    expected_outer_command.extend(("dev", *expected_container_command))
    if execution["outer_command_argv"] != expected_outer_command:
        raise ValidationError("outer Docker command argv differs from the exact gate")

    container = _inspect_container(
        inspect_snapshot,
        container_id,
        container_name,
        image_id,
        image_reference,
        compose_project_name,
        manyforge_root,
        container_environment,
        execution["container_command_argv"],
        expected_labels,
        expected_mounts,
    )

    tool_snapshots: dict[str, FileSnapshot] = {}
    for path, field, label in (
        (runner_path, "runner_script_sha256", "loaded-kernel runner"),
        (helper_path, "source_identity_helper_sha256", "source identity helper"),
        (verifier_path, "verifier_script_sha256", "loaded-kernel verifier"),
        (gate_path, "gate_script_sha256", "loaded-kernel gate script"),
        (compose_path, "compose_file_sha256", "compose file"),
    ):
        tool_snapshots[field] = _snapshot_file(
            path, label, snapshots, hashes[field]
        )
    if Path(raw_result["gate_path"]) != gate_path.resolve():
        raise ValidationError("raw manifest gate path is not the qualification gate")

    # One final sweep detects any mutation after a file's single buffered read;
    # recomputing both current identities closes the corresponding source-tree
    # window before a passing summary is returned.
    _assert_snapshots_unchanged(snapshots)
    try:
        final_manyforge = source_identity(manyforge_root)
    except SourceIdentityError as error:
        raise ValidationError(
            f"cannot collect final ManyForge identity: {error}"
        ) from error
    final_specs_contract = _current_specs_contract_identity(specs_root)
    if final_manyforge != manyforge_after:
        raise ValidationError(
            "current ManyForge source changed during loaded-kernel verification"
        )
    if final_specs_contract != specs_contract_after:
        raise ValidationError(
            "current Specs contract changed during loaded-kernel verification"
        )

    retained_artifact_manifest: list[dict[str, Any]] = [
        {
            "role": "loaded_run_manifest",
            "path": str(manifest_path),
            "sha256": manifest_snapshot.sha256,
        },
        {
            "role": "loaded_raw_manifest",
            "path": str(raw_snapshot.path),
            "sha256": raw_snapshot.sha256,
        },
        {
            "role": "loaded_container_inspect",
            "path": str(inspect_snapshot.path),
            "sha256": inspect_snapshot.sha256,
        },
        {
            "role": "loaded_container_log",
            "path": str(log_snapshot.path),
            "sha256": log_snapshot.sha256,
        },
        {
            "role": "loaded_test_binary",
            "path": raw_result["binary_path"],
            "sha256": raw_result["binary_sha256"],
        },
        {
            "role": "loaded_gate_script",
            "path": raw_result["gate_path"],
            "sha256": raw_result["gate_sha256"],
        },
        {
            "role": "loaded_verifier_script",
            "path": str(verifier_path.resolve()),
            "sha256": tool_snapshots["verifier_script_sha256"].sha256,
        },
        {
            "role": "loaded_ledger",
            "path": raw_result["ledger_path"],
            "sha256": raw_result["ledger_sha256"],
        },
        {
            "role": "loaded_test_inventory",
            "path": raw_result["inventory_path"],
            "sha256": raw_result["inventory_sha256"],
        },
        *(
            {
                "role": "loaded_iteration_log",
                "iteration": log["iteration"],
                "path": log["path"],
                "sha256": log["sha256"],
            }
            for log in raw_result["logs"]
        ),
    ]
    _assert_retained_artifact_manifest_current(retained_artifact_manifest)

    return {
        "schema_version": SUMMARY_SCHEMA,
        "run_id": run_id,
        "process_instance_id": process_instance_id,
        "source_identity": manyforge_after,
        "specs_source_identity_at_run": specs_after,
        "specs_contract_identity": specs_contract_after,
        "run_manifest": {
            "path": str(manifest_path),
            "sha256": manifest_snapshot.sha256,
        },
        "raw_manifest": {
            "path": str(raw_snapshot.path),
            "sha256": raw_snapshot.sha256,
        },
        "container_inspect": {
            "path": str(inspect_snapshot.path),
            "sha256": inspect_snapshot.sha256,
        },
        "container_log": {
            "path": str(log_snapshot.path),
            "sha256": log_snapshot.sha256,
            "bytes": log_snapshot.size,
        },
        "container": container,
        "configuration": {
            "iterations": EXPECTED_ITERATIONS,
            "load_workers": EXPECTED_LOAD_WORKERS,
            "timeout_seconds": EXPECTED_TIMEOUT_SECONDS,
            "requested_cpu_affinity": cpuset,
            "tests_per_iteration": raw_result["tests_per_iteration"],
        },
        "test_binary": {
            "path": raw_result["binary_path"],
            "sha256": raw_result["binary_sha256"],
        },
        "gate_script": {
            "path": raw_result["gate_path"],
            "sha256": raw_result["gate_sha256"],
        },
        "verifier_script": {
            "path": str(verifier_path.resolve()),
            "sha256": tool_snapshots["verifier_script_sha256"].sha256,
        },
        "ledger": {
            "path": raw_result["ledger_path"],
            "sha256": raw_result["ledger_sha256"],
        },
        "test_inventory": {
            "path": raw_result["inventory_path"],
            "sha256": raw_result["inventory_sha256"],
            "required_predicates": raw_result[
                "required_loaded_predicate_tests"
            ],
        },
        "iteration_logs": raw_result["logs"],
        "retained_artifact_manifest": retained_artifact_manifest,
        "failure_count": 0,
        "loaded_kernel_gate_passed": True,
        "step18_closed": False,
        "closure_note": (
            "This verifier owns only the 30-iteration/three-load-worker "
            "execution-kernel gate; Step 18 closure is decided by the "
            "cross-evidence verifier."
        ),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    temporary: Path | None = None
    try:
        result = verify(args.manifest, cli_output_path=args.output)
        output_path = args.output.resolve()
        if output_path.exists():
            raise ValidationError(
                f"refusing to overwrite loaded-kernel summary: {output_path}"
            )
        output_path.parent.mkdir(parents=True, exist_ok=True)
        temporary = output_path.with_name(f".{output_path.name}.tmp-{os.getpid()}")
        if temporary.exists():
            raise ValidationError(
                f"loaded-kernel temporary output already exists: {temporary}"
            )
        with temporary.open("x", encoding="utf-8") as stream:
            json.dump(result, stream, indent=2, sort_keys=True, allow_nan=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        _assert_retained_artifact_manifest_current(
            result["retained_artifact_manifest"]
        )
        run_manifest = _load_json(
            _snapshot_file(
                Path(result["run_manifest"]["path"]),
                "loaded-kernel publication run manifest",
                [],
                result["run_manifest"]["sha256"],
            )
        )
        manyforge_root = Path(run_manifest["paths"]["manyforge_root"]).resolve()
        specs_root = Path(run_manifest["paths"]["specs_root"]).resolve()
        if source_identity(manyforge_root) != result["source_identity"]:
            raise ValidationError(
                "ManyForge source changed before loaded-kernel summary publication"
            )
        publication_specs = source_identity(specs_root)
        campaign_specs = result["specs_source_identity_at_run"]
        if (
            publication_specs["head_commit"] != campaign_specs["head_commit"]
            or publication_specs["head_tree"] != campaign_specs["head_tree"]
        ):
            raise ValidationError(
                "full Specs Git base changed before loaded-kernel summary publication"
            )
        if _current_specs_contract_identity(specs_root) != result[
            "specs_contract_identity"
        ]:
            raise ValidationError(
                "Specs contract changed before loaded-kernel summary publication"
            )
        os.link(temporary, output_path)
        try:
            temporary.unlink()
        except OSError:
            pass
        temporary = None
    except (OSError, SourceIdentityError, ValidationError, ValueError) as error:
        if temporary is not None:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass
        print(f"Step 18 loaded-kernel verification failed: {error}", file=sys.stderr)
        return 2
    print(json.dumps({"passed": True, "iterations": EXPECTED_ITERATIONS}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
