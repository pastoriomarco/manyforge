#!/usr/bin/env python3
"""Run the bounded C++ I5/H0 alarm-owner performance probe.

The historical Composer/Python benchmark remains pinned at commit
08222188293df8e12f90dc72e204c21f22f92ea1, blob
099112f29b7b7a9b799d765904d440fec83ff7c3. Its severity-derived inbox and
raw-byte catalog are not a valid performance baseline for the condition-backed
C++ owner. This entrypoint instead records the exact probe/catalog identities
and enforces deliberately generous non-real-time bounds on the new owner.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import tempfile
from typing import Any, Final, Sequence


SCHEMA_VERSION: Final = "manyforge.operator_alarm_performance_evidence.v2"
PROBE_SCHEMA_VERSION: Final = "manyforge.operator_alarm_performance.v2"
MAXIMUM_CAPTURE_BYTES: Final = 1024 * 1024
MAXIMUM_PROBE_BYTES: Final = 256 * 1024 * 1024
MAXIMUM_CATALOG_BYTES: Final = 1024 * 1024
PROBE_TIMEOUT_SECONDS: Final = 60


def _regular_file(path: Path, *, label: str, maximum_bytes: int) -> os.stat_result:
    metadata = path.lstat()
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
        raise ValueError(f"{label} must be a regular non-symlink file")
    if metadata.st_nlink != 1:
        raise ValueError(f"{label} must have exactly one hard link")
    if metadata.st_size <= 0 or metadata.st_size > maximum_bytes:
        raise ValueError(f"{label} has an invalid byte size")
    return metadata


def _identity(path: Path, *, label: str, maximum_bytes: int) -> dict[str, Any]:
    before = _regular_file(path, label=label, maximum_bytes=maximum_bytes)
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(64 * 1024):
            digest.update(chunk)
    after = _regular_file(path, label=label, maximum_bytes=maximum_bytes)
    if (
        before.st_dev,
        before.st_ino,
        before.st_size,
        before.st_mtime_ns,
        before.st_ctime_ns,
    ) != (
        after.st_dev,
        after.st_ino,
        after.st_size,
        after.st_mtime_ns,
        after.st_ctime_ns,
    ):
        raise ValueError(f"{label} changed while being hashed")
    return {"path": str(path), "sha256": digest.hexdigest(), "size": before.st_size}


def _no_duplicate_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"probe output contains duplicate key {key!r}")
        result[key] = value
    return result


def _reject_constant(value: str) -> None:
    raise ValueError(f"probe output contains non-finite number {value}")


def _run_probe(probe: Path, catalog: Path, iterations: int) -> dict[str, Any]:
    with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
        process = subprocess.run(
            [
                str(probe),
                "--catalog",
                str(catalog),
                "--iterations",
                str(iterations),
            ],
            stdin=subprocess.DEVNULL,
            stdout=stdout,
            stderr=stderr,
            check=False,
            timeout=PROBE_TIMEOUT_SECONDS,
            close_fds=True,
        )
        if stdout.tell() > MAXIMUM_CAPTURE_BYTES or stderr.tell() > MAXIMUM_CAPTURE_BYTES:
            raise ValueError("performance probe output exceeds its byte bound")
        stdout.seek(0)
        stderr.seek(0)
        output = stdout.read().decode("utf-8", errors="strict")
        diagnostic = stderr.read().decode("utf-8", errors="strict").strip()
    if process.returncode != 0:
        raise RuntimeError(
            f"performance probe exited {process.returncode}: {diagnostic or 'no diagnostic'}"
        )
    try:
        value = json.loads(
            output,
            parse_constant=_reject_constant,
            object_pairs_hook=_no_duplicate_object,
        )
    except (TypeError, ValueError) as error:
        raise ValueError(f"performance probe output is not strict JSON: {error}") from error
    if (
        not isinstance(value, dict)
        or value.get("schema_version") != PROBE_SCHEMA_VERSION
        or value.get("passed") is not True
    ):
        raise ValueError("performance probe returned an unsupported or failed result")
    return value


def main(argv: Sequence[str] | None = None) -> int:
    repository_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument(
        "--catalog",
        type=Path,
        default=repository_root
        / "manyforge_headless"
        / "resources"
        / "operator-alarm-catalog.v1.yaml",
    )
    parser.add_argument("--iterations", type=int, default=20_000)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    if not 100 <= args.iterations <= 100_000:
        parser.error("--iterations must be between 100 and 100000")
    _regular_file(
        args.probe,
        label="performance probe argument",
        maximum_bytes=MAXIMUM_PROBE_BYTES,
    )
    _regular_file(
        args.catalog,
        label="alarm catalog argument",
        maximum_bytes=MAXIMUM_CATALOG_BYTES,
    )
    probe = args.probe.resolve(strict=True)
    catalog = args.catalog.resolve(strict=True)
    probe_identity = _identity(
        probe,
        label="performance probe",
        maximum_bytes=MAXIMUM_PROBE_BYTES,
    )
    if not os.access(probe, os.X_OK):
        raise ValueError("performance probe is not executable")
    catalog_identity = _identity(
        catalog,
        label="alarm catalog",
        maximum_bytes=MAXIMUM_CATALOG_BYTES,
    )
    result = _run_probe(probe, catalog, args.iterations)
    payload = {
        "schema_version": SCHEMA_VERSION,
        "historical_composer_reference": {
            "commit": "08222188293df8e12f90dc72e204c21f22f92ea1",
            "benchmark_blob": "099112f29b7b7a9b799d765904d440fec83ff7c3",
            "disposition": "semantic_reference_not_comparable_runtime_baseline",
        },
        "probe_identity": probe_identity,
        "catalog_identity": catalog_identity,
        "result": result,
        "passed": True,
    }
    encoded = json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
