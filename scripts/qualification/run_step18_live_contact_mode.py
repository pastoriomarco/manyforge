#!/usr/bin/env python3
"""Run one source-bound Step 18 live row and retain immutable row evidence."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import math
import os
import re
import shlex
import subprocess
import sys
import uuid
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from scripts.qualification.source_identity import (  # noqa: E402
    source_identity as _manyforge_source_identity,
    specs_contract_identity as _specs_contract_identity,
)


CONTAINER_REPO_ROOT = Path("/workspace")
PINNED_BASELINE_COMMIT = "643fe9c399182419d38d4478750d7e6f692dc9ad"
PINNED_SPECS_COMMIT = "5e50a4872bbb9a2167c6bc14b9b5963061f2f09e"
ROW_KINDS = (
    "baseline_pristine_timing",
    "baseline_observation",
    "candidate_timing",
    "candidate_observation",
)
_SHA256 = re.compile(r"[0-9a-f]{64}")


def _run(command: list[str], *, capture: bool = False) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        check=False,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.STDOUT if capture else None,
    )


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _canonical_sha256(value: Any) -> str:
    return hashlib.sha256(
        json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()


def _thor_host_identity() -> dict[str, Any]:
    """Capture the physical Thor host independently of container metadata."""

    model_path = Path("/proc/device-tree/model")
    try:
        device_tree_model = model_path.read_bytes().rstrip(b"\0").decode("utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        raise RuntimeError(f"cannot identify the Step 18 Thor host: {exc}") from exc
    uname = os.uname()
    gpu_result = _run(
        [
            "nvidia-smi",
            "--query-gpu=name,uuid,driver_version",
            "--format=csv,noheader,nounits",
        ],
        capture=True,
    )
    if gpu_result.returncode != 0:
        raise RuntimeError(
            "cannot identify the Step 18 Thor GPU: " + (gpu_result.stdout or "")
        )
    gpus: list[dict[str, str]] = []
    for line in (gpu_result.stdout or "").splitlines():
        parts = [part.strip() for part in line.split(",")]
        if len(parts) != 3 or any(not part for part in parts):
            raise RuntimeError("nvidia-smi returned an incomplete Thor GPU identity")
        gpus.append({"name": parts[0], "uuid": parts[1], "driver_version": parts[2]})
    payload = {
        "schema_version": "manyforge.step18_thor_host_identity.v1",
        "hostname": uname.nodename,
        "architecture": uname.machine,
        "kernel_release": uname.release,
        "device_tree_model": device_tree_model,
        "gpus": gpus,
    }
    return {**payload, "fingerprint_sha256": _canonical_sha256(payload)}


def _qualification_tool_bundle(root: Path) -> dict[str, Any]:
    qualification = root.resolve() / "scripts/qualification"
    files = sorted(path for path in qualification.glob("*.py") if path.is_file())
    if not files:
        raise RuntimeError(f"qualification tool root is incomplete: {root}")
    entries = [
        {
            "path": str(path.relative_to(root.resolve())),
            "sha256": _sha256(path),
        }
        for path in files
    ]
    return {
        "schema_version": "manyforge.step18_qualification_tool_bundle.v1",
        "files": entries,
        "fingerprint_sha256": _canonical_sha256(entries),
    }


def _runtime_process_identity(name: str) -> dict[str, Any]:
    probe = r'''
import hashlib, json, os

def digest(path):
    value = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()

processes = []
all_artifacts = {}
for name in sorted((entry for entry in os.listdir("/proc") if entry.isdigit()), key=int):
    pid = int(name)
    root = f"/proc/{pid}"
    try:
        exe = os.path.realpath(f"{root}/exe")
        cmdline = open(f"{root}/cmdline", "rb").read().replace(b"\0", b" ").decode("utf-8", "replace").strip()
        paths = set()
        with open(f"{root}/maps", encoding="utf-8", errors="replace") as stream:
            for line in stream:
                parts = line.rstrip().split(None, 5)
                if len(parts) != 6:
                    continue
                path = parts[5]
                if path.endswith(" (deleted)"):
                    path = path[:-10]
                if not path.startswith("/") or not os.path.isfile(path):
                    continue
                if (
                    "/tmp/manyforge-build/" in path
                    or "/workspace/build/" in path
                    or path.startswith(
                        "/opt/manyforge/topic_based_ros2_control/"
                    )
                ):
                    paths.add(path)
        if pid != 1 and not paths:
            continue
        artifacts = [{"path": path, "sha256": digest(path)} for path in sorted(paths)]
        for artifact in artifacts:
            all_artifacts[artifact["path"]] = artifact["sha256"]
        processes.append({
            "pid": pid,
            "exe": exe,
            "exe_sha256": digest(exe),
            "cmdline": cmdline,
            "mapped_native_artifacts": artifacts,
        })
    except (FileNotFoundError, PermissionError, ProcessLookupError):
        continue
value = {
    "processes": processes,
    "mapped_native_artifacts": [
        {"path": path, "sha256": all_artifacts[path]}
        for path in sorted(all_artifacts)
    ],
}
payload = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
value["fingerprint_sha256"] = hashlib.sha256(payload).hexdigest()
print(json.dumps(value, sort_keys=True))
'''
    result = _run(["docker", "exec", name, "python3", "-c", probe], capture=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"cannot inspect loaded process artifacts in {name!r}: {result.stdout}"
        )
    value = json.loads(result.stdout)
    if not value.get("mapped_native_artifacts"):
        raise RuntimeError(f"container {name!r} has no mapped ManyForge native artifact")
    return value


def _container_identity(name: str, host_repo_root: Path) -> dict[str, Any]:
    result = _run(
        ["docker", "inspect", "--format", "{{json .}}", name], capture=True
    )
    if result.returncode != 0:
        raise RuntimeError(f"cannot inspect container {name!r}: {result.stdout}")
    value = json.loads(result.stdout)
    if not value.get("State", {}).get("Running"):
        raise RuntimeError(f"container {name!r} is not running")
    mounts = [
        mount
        for mount in value.get("Mounts") or []
        if mount.get("Destination") == str(CONTAINER_REPO_ROOT)
    ]
    expected_source = host_repo_root.resolve()
    if len(mounts) != 1:
        raise RuntimeError(f"container {name!r} has no unique /workspace mount")
    mount = mounts[0]
    if (
        mount.get("Type") != "bind"
        or Path(str(mount.get("Source") or "")).resolve() != expected_source
        or mount.get("RW") is not True
    ):
        raise RuntimeError(
            f"container {name!r} /workspace is not the expected writable host bind"
        )
    config = value.get("Config") or {}
    identity = {
        "name": name,
        "id": value.get("Id"),
        "image_id": value.get("Image"),
        "image_name": config.get("Image"),
        "started_at": (value.get("State") or {}).get("StartedAt"),
        "log_config": dict((value.get("HostConfig") or {}).get("LogConfig") or {}),
        "workspace_mount": {
            "type": mount.get("Type"),
            "source": str(expected_source),
            "destination": str(CONTAINER_REPO_ROOT),
            "rw": mount.get("RW"),
        },
        "process": _runtime_process_identity(name),
    }
    identity["fingerprint_sha256"] = _canonical_sha256(identity)
    return identity


def _require_same_runtime(before: dict[str, Any], after: dict[str, Any], label: str) -> None:
    if before != after:
        raise RuntimeError(f"{label} container/runtime identity changed during the row")


def _emit_controller_log_marker(container: str, row_uuid: str, phase: str) -> str:
    marker = f"MANYFORGE_STEP18_ROW {row_uuid} {phase}"
    script = (
        "import os; "
        "fd=os.open('/proc/1/fd/1', os.O_WRONLY); "
        f"os.write(fd, {json.dumps((marker + chr(10)).encode('utf-8').decode('utf-8'))}.encode()); "
        "os.close(fd)"
    )
    result = _run(["docker", "exec", container, "python3", "-c", script], capture=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"cannot emit controller qualification log marker: {result.stdout}"
        )
    return marker


def _utc_now() -> dt.datetime:
    return dt.datetime.now(dt.timezone.utc)


def _rfc3339(value: dt.datetime) -> str:
    return value.isoformat(timespec="microseconds").replace("+00:00", "Z")


def _exclusive_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")


def _copy_qualification_tools(
    *, container: str, tool_root: Path, container_tool_root: Path
) -> None:
    mkdir = _run(
        [
            "docker",
            "exec",
            container,
            "mkdir",
            "-p",
            str(container_tool_root / "scripts"),
        ],
        capture=True,
    )
    if mkdir.returncode != 0:
        raise RuntimeError(f"cannot prepare qualification tool directory: {mkdir.stdout}")
    copied = _run(
        [
            "docker",
            "cp",
            str(tool_root.resolve() / "scripts/qualification"),
            f"{container}:{container_tool_root}/scripts/",
        ],
        capture=True,
    )
    if copied.returncode != 0:
        raise RuntimeError(f"cannot copy qualification tool bundle: {copied.stdout}")


def _copy_from_container(container: str, source: Path, destination: Path) -> None:
    copied = _run(
        ["docker", "cp", f"{container}:{source}", str(destination)], capture=True
    )
    if copied.returncode != 0:
        raise RuntimeError(f"cannot copy live evidence from container: {copied.stdout}")


def _validate_row_contract(args: argparse.Namespace) -> str:
    if args.row_kind in {"baseline_pristine_timing", "baseline_observation"}:
        if args.support_contact_mode != "current_all_active_distance":
            raise RuntimeError("baseline rows require current_all_active_distance")
    elif args.support_contact_mode == "current_all_active_distance":
        raise RuntimeError("candidate rows cannot claim the historical baseline mode")
    return (
        "quiescent_repeated"
        if args.row_kind == "baseline_pristine_timing"
        else "coherent_native"
    )


def run(args: argparse.Namespace) -> int:
    for path in (args.output, args.controller_log, args.run_metadata):
        if path.exists():
            raise RuntimeError(f"refusing to overwrite live evidence: {path}")
        path.parent.mkdir(parents=True, exist_ok=True)
    observer_mode = _validate_row_contract(args)
    if _SHA256.fullmatch(args.expected_source_fingerprint_sha256) is None:
        raise RuntimeError("expected source fingerprint is not a SHA-256")

    host_repo_root = args.host_repo_root.resolve()
    observed_source = _manyforge_source_identity(host_repo_root)
    if observed_source["head_commit"] != PINNED_BASELINE_COMMIT:
        raise RuntimeError("Step 18 source must remain based on pinned 643fe9c")
    if (
        observed_source["fingerprint_sha256"]
        != args.expected_source_fingerprint_sha256
    ):
        raise RuntimeError("host source fingerprint differs from the frozen row")
    if args.row_kind == "baseline_pristine_timing" and observed_source["dirty"]:
        raise RuntimeError("pristine baseline timing requires a clean source checkout")
    if args.row_kind != "baseline_pristine_timing" and not observed_source["dirty"]:
        raise RuntimeError("observation/candidate rows require an explicit frozen delta")

    specs_root = args.host_specs_root.resolve()
    specs_identity = _manyforge_source_identity(specs_root)
    specs_contract_identity = _specs_contract_identity(specs_root)
    if specs_identity["head_commit"] != PINNED_SPECS_COMMIT:
        raise RuntimeError("Step 18 Specs source must remain based on pinned 5e50a48")
    if args.row_kind in {"candidate_timing", "candidate_observation"} and not specs_identity["dirty"]:
        raise RuntimeError("candidate rows require the frozen Step 18 Specs delta")
    if args.row_kind not in {"candidate_timing", "candidate_observation"} and specs_identity["dirty"]:
        raise RuntimeError("baseline rows require pristine pinned Step 17 Specs")

    tool_root = args.qualification_tool_root.resolve()
    tool_bundle = _qualification_tool_bundle(tool_root)
    host_identity_before = _thor_host_identity()
    row_uuid = args.row_uuid or str(uuid.uuid4())
    try:
        uuid.UUID(row_uuid)
    except ValueError as exc:
        raise RuntimeError("row UUID is invalid") from exc
    container_tool_root = Path(f"/tmp/manyforge-step18-tools-{row_uuid}")
    container_output = container_tool_root / "evidence.json"
    _copy_qualification_tools(
        container=args.composer_container,
        tool_root=tool_root,
        container_tool_root=container_tool_root,
    )

    composer_before = _container_identity(args.composer_container, host_repo_root)
    controller_before = _container_identity(args.controller_container, host_repo_root)
    collector = container_tool_root / "scripts/qualification/run_roboplan_05_live_baseline.py"
    command = [
        "docker",
        "exec",
        args.composer_container,
        "env",
        f"PYTHONPATH={container_tool_root}",
        "python3",
        str(collector),
        "--repo-root",
        str(CONTAINER_REPO_ROOT),
        "--artifact-root",
        str(CONTAINER_REPO_ROOT),
        "--composer-url",
        args.composer_url,
        "--viewer-stratum",
        args.viewer_stratum,
        "--measurement-mode",
        "primary",
        "--support-contact-mode",
        args.support_contact_mode,
        "--max-contact-penetration-m",
        str(args.max_contact_penetration_m),
        "--successful-observations",
        str(args.successful_observations),
        "--poll-period-s",
        str(args.poll_period_s),
        "--controller-progress-observer-mode",
        observer_mode,
        "--phase",
        args.phase,
        "--run-id",
        row_uuid,
        "--expected-source-fingerprint-sha256",
        args.expected_source_fingerprint_sha256,
        "--output",
        str(container_output),
    ]
    if args.operator_attests_viewer_focused:
        command.append("--operator-attests-viewer-focused")
    if args.row_kind in {"baseline_observation", "candidate_observation"}:
        command.append("--capture-mechanism-replay-input")

    started = _utc_now()
    start_marker = _emit_controller_log_marker(
        args.controller_container, row_uuid, "start"
    )
    result = _run(command, capture=True)
    end_marker = _emit_controller_log_marker(
        args.controller_container, row_uuid, "end"
    )
    ended = _utc_now()
    controller_log_command = [
            "docker",
            "logs",
            "--timestamps",
            "--since",
            _rfc3339(started),
            "--until",
            _rfc3339(ended),
            args.controller_container,
        ]
    log_result = _run(
        controller_log_command,
        capture=True,
    )
    if log_result.returncode != 0:
        raise RuntimeError(
            "failed to capture the bounded controller log interval: "
            f"{log_result.stdout}"
        )
    with args.controller_log.open("x", encoding="utf-8") as stream:
        stream.write(log_result.stdout or "")
    if result.returncode == 0:
        _copy_from_container(args.composer_container, container_output, args.output)

    composer_after = _container_identity(args.composer_container, host_repo_root)
    controller_after = _container_identity(args.controller_container, host_repo_root)
    _require_same_runtime(composer_before, composer_after, "Composer")
    _require_same_runtime(controller_before, controller_after, "controller")
    if _manyforge_source_identity(host_repo_root) != observed_source:
        raise RuntimeError("host source identity changed during the row")
    if _manyforge_source_identity(specs_root) != specs_identity:
        raise RuntimeError("Specs source identity changed during the row")
    if _specs_contract_identity(specs_root) != specs_contract_identity:
        raise RuntimeError("Specs contract identity changed during the row")
    if _thor_host_identity() != host_identity_before:
        raise RuntimeError("Thor host identity changed during the row")

    evidence_sha256 = _sha256(args.output) if args.output.is_file() else None
    metadata = {
        "schema_version": "manyforge.step18_live_row_run.v2",
        "row_uuid": row_uuid,
        "row_kind": args.row_kind,
        "started_at_utc": _rfc3339(started),
        "ended_at_utc": _rfc3339(ended),
        "duration_s": (ended - started).total_seconds(),
        "command": shlex.join(command),
        "collector_stdout": result.stdout or "",
        "collector_exit_code": result.returncode,
        "viewer_stratum": args.viewer_stratum,
        "support_contact_mode": args.support_contact_mode,
        "max_contact_penetration_m": args.max_contact_penetration_m,
        "successful_observations": args.successful_observations,
        "poll_period_s": args.poll_period_s,
        "phase": args.phase,
        "controller_progress_observer_mode": observer_mode,
        "expected_source_fingerprint_sha256": (
            args.expected_source_fingerprint_sha256
        ),
        "observed_source_identity": observed_source,
        "specs_source_identity": specs_identity,
        "specs_contract_identity": specs_contract_identity,
        "composer_runtime": composer_before,
        "controller_runtime": controller_before,
        "runtime_identity_verified_after_run": True,
        "evidence_path": str(args.output.resolve()),
        "evidence_sha256": evidence_sha256,
        "controller_log_path": str(args.controller_log.resolve()),
        "controller_log_sha256": _sha256(args.controller_log),
        "controller_log_command": controller_log_command,
        "controller_log_markers": {
            "start": start_marker,
            "end": end_marker,
        },
        "host_repo_root": str(host_repo_root),
        "thor_host_identity": host_identity_before,
        "qualification_tool_bundle": tool_bundle,
        "host_runner": {
            "path": str(Path(__file__).resolve()),
            "sha256": _sha256(Path(__file__)),
        },
    }
    _exclusive_json(args.run_metadata, metadata)
    return result.returncode


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--composer-container", default="manyforge-composer-ros-1")
    parser.add_argument(
        "--controller-container", default="manyforge-isaac-topic-control-1"
    )
    parser.add_argument("--composer-url", default="http://127.0.0.1:9000")
    parser.add_argument("--host-repo-root", type=Path, required=True)
    parser.add_argument(
        "--host-specs-root", type=Path, default=REPO_ROOT.parent / "manyforge_specs"
    )
    parser.add_argument(
        "--qualification-tool-root", type=Path, default=REPO_ROOT
    )
    parser.add_argument("--row-kind", choices=ROW_KINDS, required=True)
    parser.add_argument("--row-uuid")
    parser.add_argument(
        "--viewer-stratum", choices=("disconnected", "focused"), required=True
    )
    parser.add_argument(
        "--support-contact-mode",
        choices=(
            "current_all_active_distance",
            "signed_penetration",
            "binary_contact",
            "bounded_penetration",
        ),
        required=True,
    )
    parser.add_argument("--max-contact-penetration-m", type=float, default=0.03)
    parser.add_argument("--successful-observations", type=int, default=30)
    parser.add_argument("--poll-period-s", type=float, default=0.2)
    parser.add_argument("--phase", choices=("rp5_0", "rp5_3", "rp5_7"), default="rp5_7")
    parser.add_argument("--expected-source-fingerprint-sha256", required=True)
    parser.add_argument("--operator-attests-viewer-focused", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--controller-log", type=Path, required=True)
    parser.add_argument("--run-metadata", type=Path, required=True)
    args = parser.parse_args(argv)
    if args.successful_observations < 30:
        parser.error("formal Step 18 live rows require at least 30 observations")
    if (
        not math.isfinite(args.max_contact_penetration_m)
        or args.max_contact_penetration_m < 0.0
    ):
        parser.error("--max-contact-penetration-m must be finite and non-negative")
    if args.viewer_stratum == "focused" and not args.operator_attests_viewer_focused:
        parser.error("focused rows require --operator-attests-viewer-focused")
    try:
        return run(args)
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as exc:
        print(f"Step 18 live row failed: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
