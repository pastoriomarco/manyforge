#!/usr/bin/env python3
"""Run the exact non-simulator I9 controller and ownership probes."""

from __future__ import annotations

import argparse
from collections.abc import Mapping, Sequence
from contextlib import suppress
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
from typing import Any, Final

from run_i9_isaac_matrix import (
    PROBE_CONTRACTS,
    STATIC_PROBES_SCHEMA,
    QualificationError,
    validate_static_probes,
)


_MAX_OUTPUT_BYTES: Final = 8 * 1024 * 1024
_PROBE_TIMEOUT_S: Final = 300.0


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _run_bounded(
    command: Sequence[str],
    *,
    cwd: Path,
    environment: Mapping[str, str],
    timeout_s: float,
) -> dict[str, Any]:
    started = time.monotonic()
    process = subprocess.Popen(
        list(command),
        cwd=cwd,
        env=dict(environment),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    timed_out = False
    try:
        output, _stderr = process.communicate(timeout=timeout_s)
    except subprocess.TimeoutExpired:
        timed_out = True
        with suppress(ProcessLookupError):
            os.killpg(process.pid, signal.SIGTERM)
        try:
            output, _stderr = process.communicate(timeout=2.0)
        except subprocess.TimeoutExpired:
            with suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGKILL)
            output, _stderr = process.communicate(timeout=2.0)
    if len(output) > _MAX_OUTPUT_BYTES:
        raise QualificationError("static probe output exceeded its byte bound")
    return {
        "elapsed_s": round(time.monotonic() - started, 3),
        "output": output,
        "returncode": 124 if timed_out else int(process.returncode),
    }


def _git_text(source_root: Path, *arguments: str) -> str:
    result = _run_bounded(
        ["git", "-C", str(source_root), *arguments],
        cwd=source_root,
        environment=os.environ,
        timeout_s=10.0,
    )
    output = result["output"].decode("utf-8", errors="replace").strip()
    if result["returncode"] != 0:
        raise QualificationError(f"source identity command failed: {output[:512]}")
    return output


def _source_identity(source_root: Path) -> dict[str, Any]:
    commit = _git_text(source_root, "rev-parse", "HEAD")
    tree = _git_text(source_root, "rev-parse", "HEAD^{tree}")
    dirty = bool(
        _git_text(
            source_root,
            "status",
            "--porcelain=v1",
            "--untracked-files=all",
        )
    )
    return {"commit": commit, "dirty": dirty, "tree": tree}


def build_evidence(
    *,
    source_root: Path,
    roboplan_test: Path,
    deployment_test: Path,
    environment: Mapping[str, str],
) -> dict[str, Any]:
    for label, path in (
        ("source root", source_root),
        ("RoboPlan test", roboplan_test),
        ("deployment test", deployment_test),
    ):
        if label == "source root":
            valid = path.is_dir() and not path.is_symlink()
        else:
            valid = path.is_file() and not path.is_symlink()
        if not valid:
            raise QualificationError(f"{label} is missing or not exact: {path}")

    rows: list[dict[str, Any]] = []
    for probe_id in sorted(PROBE_CONTRACTS):
        kind, selector = PROBE_CONTRACTS[probe_id]
        if probe_id in {
            "direct_controller_route",
            "two_same_type_instances_isolated",
        }:
            subject = roboplan_test
            command = [
                str(roboplan_test),
                "--gtest_color=no",
                f"--gtest_filter={selector}",
            ]
        elif probe_id == "representative_chained_controller_route":
            subject = deployment_test
            command = [
                str(deployment_test),
                "--gtest_color=no",
                f"--gtest_filter={selector}",
            ]
        else:
            subject = source_root / selector.split("::", 1)[0]
            if not subject.is_file() or subject.is_symlink():
                raise QualificationError(
                    f"DemoSupervisor probe source is missing: {subject}"
                )
            command = [
                sys.executable,
                "-m",
                "pytest",
                "-c",
                "/dev/null",
                "--import-mode=importlib",
                "-p",
                "no:cacheprovider",
                "-p",
                "no:launch_testing",
                "-p",
                "no:launch_ros",
                selector,
                "-q",
            ]
        result = _run_bounded(
            command,
            cwd=source_root,
            environment=environment,
            timeout_s=_PROBE_TIMEOUT_S,
        )
        output = result["output"]
        rows.append(
            {
                "elapsed_s": result["elapsed_s"],
                "kind": kind,
                "output_bytes": len(output),
                "output_sha256": hashlib.sha256(output).hexdigest(),
                "probe_id": probe_id,
                "returncode": result["returncode"],
                "selector": selector,
                "subject_sha256": _sha256_file(subject),
            }
        )
    return {
        "schema_version": STATIC_PROBES_SCHEMA,
        "source": _source_identity(source_root),
        "probes": rows,
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--roboplan-test", type=Path, required=True)
    parser.add_argument("--deployment-test", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        evidence = build_evidence(
            source_root=args.source_root.resolve(),
            roboplan_test=args.roboplan_test.resolve(),
            deployment_test=args.deployment_test.resolve(),
            environment=os.environ,
        )
        _normalized, checks = validate_static_probes(evidence)
        print(json.dumps(evidence, separators=(",", ":"), sort_keys=True))
        return 0 if all(checks.values()) else 2
    except (OSError, QualificationError) as error:
        print(f"I9 static probes rejected: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
