#!/usr/bin/env python3
"""Measure the recorded I9 laptop-to-Orin clock envelope over SSH."""

from __future__ import annotations

import argparse
from collections.abc import Sequence
from contextlib import suppress
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
from typing import Final

from run_i9_isaac_matrix import (
    CLOCK_EVIDENCE_SCHEMA,
    CLOCK_SAMPLE_COUNT,
    QualificationError,
    validate_clock_evidence,
)


_MAX_SSH_OUTPUT_BYTES: Final = 64 * 1024
_SSH_TIMEOUT_S: Final = 10.0
_REMOTE_STATUS_COMMAND: Final = (
    "hostname; "
    "timedatectl show -p NTPSynchronized --value; "
    "if test -e /run/systemd/timesync/synchronized; "
    "then printf 'yes\\n'; else printf 'no\\n'; fi"
)


def _ssh_command(
    *,
    ssh_config: Path,
    known_hosts: Path,
    target: str,
    remote_command: str,
) -> list[str]:
    return [
        "ssh",
        "-F",
        str(ssh_config),
        "-o",
        f"UserKnownHostsFile={known_hosts}",
        "-o",
        "BatchMode=yes",
        "-o",
        "ConnectTimeout=5",
        "-o",
        "ConnectionAttempts=1",
        "-o",
        "StrictHostKeyChecking=yes",
        target,
        remote_command,
    ]


def _run_ssh(command: Sequence[str]) -> str:
    process = subprocess.Popen(
        list(command),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        start_new_session=True,
    )
    try:
        stdout, stderr = process.communicate(timeout=_SSH_TIMEOUT_S)
    except subprocess.TimeoutExpired as error:
        with suppress(ProcessLookupError):
            os.killpg(process.pid, signal.SIGTERM)
        try:
            process.communicate(timeout=2.0)
        except subprocess.TimeoutExpired:
            with suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGKILL)
            process.communicate(timeout=2.0)
        raise QualificationError("clock-probe SSH command timed out") from error
    if len(stdout) + len(stderr) > _MAX_SSH_OUTPUT_BYTES:
        raise QualificationError("clock-probe SSH output exceeded its byte bound")
    if process.returncode != 0:
        detail = stderr.decode("utf-8", errors="replace").strip()[:512]
        raise QualificationError(f"clock-probe SSH command failed: {detail}")
    return stdout.decode("utf-8", errors="strict").strip()


def build_evidence(
    *,
    ssh_config: Path,
    known_hosts: Path,
    target: str,
    local_hostname_file: Path,
    local_sync_marker: Path,
) -> dict[str, object]:
    for label, path in (
        ("SSH config", ssh_config),
        ("SSH known-hosts", known_hosts),
        ("local hostname", local_hostname_file),
    ):
        if not path.is_file() or path.is_symlink():
            raise QualificationError(f"{label} input is missing or not exact: {path}")
    local_hostname = local_hostname_file.read_text(encoding="utf-8").strip()
    local_synchronized = local_sync_marker.is_file()

    status = _run_ssh(
        _ssh_command(
            ssh_config=ssh_config,
            known_hosts=known_hosts,
            target=target,
            remote_command=_REMOTE_STATUS_COMMAND,
        )
    ).splitlines()
    if len(status) != 3:
        raise QualificationError("Orin clock status response was not exact")
    remote_hostname, remote_synchronized, remote_marker = status

    samples: list[dict[str, int]] = []
    for _index in range(CLOCK_SAMPLE_COUNT):
        sent = time.time_ns()
        raw_remote = _run_ssh(
            _ssh_command(
                ssh_config=ssh_config,
                known_hosts=known_hosts,
                target=target,
                remote_command="date +%s%N",
            )
        )
        received = time.time_ns()
        if not raw_remote.isdigit():
            raise QualificationError("Orin clock sample was not integer nanoseconds")
        samples.append(
            {
                "local_receive_ns": received,
                "local_send_ns": sent,
                "remote_ns": int(raw_remote),
            }
        )
    return {
        "schema_version": CLOCK_EVIDENCE_SCHEMA,
        "hosts": {
            "laptop": {
                "address": "192.168.1.132",
                "clock_synchronized": local_synchronized,
                "hostname": local_hostname,
                "synchronization_marker": local_synchronized,
            },
            "orin": {
                "address": "192.168.1.68",
                "clock_synchronized": remote_synchronized == "yes",
                "hostname": remote_hostname,
                "synchronization_marker": remote_marker == "yes",
            },
        },
        "samples": samples,
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ssh-config", type=Path, required=True)
    parser.add_argument("--known-hosts", type=Path, required=True)
    parser.add_argument("--target", default="orin")
    parser.add_argument("--local-hostname-file", type=Path, required=True)
    parser.add_argument("--local-sync-marker", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        evidence = build_evidence(
            ssh_config=args.ssh_config,
            known_hosts=args.known_hosts,
            target=args.target,
            local_hostname_file=args.local_hostname_file,
            local_sync_marker=args.local_sync_marker,
        )
        _normalized, passed = validate_clock_evidence(evidence)
        print(json.dumps(evidence, separators=(",", ":"), sort_keys=True))
        return 0 if passed else 2
    except (OSError, UnicodeError, QualificationError) as error:
        print(f"I9 clock probe rejected: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
