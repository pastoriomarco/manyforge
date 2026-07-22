#!/usr/bin/env python3
"""Strictly verify the source-bound Step 18 live qualification matrix."""

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
import tempfile
import time
import uuid
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from scripts.qualification.roboplan_05_qualification import (  # noqa: E402
    MANYFORGE_SOURCE_IDENTITY_SCHEMA,
    ValidationError,
    _manyforge_source_fingerprint_sha256,
    validate_capture,
    validate_evidence,
)
from scripts.qualification.source_identity import (  # noqa: E402
    SPECS_CONTRACT_EXCLUDED_PATH,
    SPECS_CONTRACT_SCHEMA,
    SourceIdentityError,
    source_identity as current_source_identity,
    specs_contract_fingerprint_sha256,
    specs_contract_identity as current_specs_contract_identity,
    validate_specs_contract_identity,
)


SCHEMA = "manyforge.step18_live_contact_modes.v3"
MANIFEST_SCHEMA = "manyforge.step18_live_contact_modes_manifest.v3"
PARITY_SCHEMA = "manyforge.step18_live_baseline_retained_output_parity.v1"
PINNED_BASELINE_COMMIT = "2bfe30eeb37952f1ddc36bfdbf89d471a9f6b909"
PINNED_BASELINE_TREE = "c900d971cdcbcfb6ec5d7f9b465ff18befa1dd89"
PINNED_SPECS_COMMIT = "67c24447a117e14c8274b13d05d720766671aa7a"
PINNED_SPECS_TREE = "7529497b02758c14666f85bc9bf98e85abec075a"
BASELINE_MODE = "current_all_active_distance"
CANDIDATE_MODES = (
    "signed_penetration",
    "binary_contact",
    "bounded_penetration",
)
VIEWER_STRATA = ("disconnected", "focused")
ROW_KINDS = (
    "baseline_pristine_timing",
    "baseline_observation",
    "candidate_timing",
    "candidate_observation",
)
OBSERVATION_ROW_KINDS = frozenset(
    {"baseline_observation", "candidate_observation"}
)
CANDIDATE_ROW_KINDS = frozenset(
    {"candidate_timing", "candidate_observation"}
)
SPECS_CONTRACT_IDENTITY_SCHEMA = SPECS_CONTRACT_SCHEMA
_OBSERVATION_PATCH_PATHS = frozenset(
    {
        "manyforge_ros2_control_execution/include/manyforge_ros2_control_execution/ros2_control_execution_backend.hpp",
        "manyforge_ros2_control_execution/src/ros2_control_execution_backend.cpp",
        "scripts/qualification/run_roboplan_05_live_baseline.py",
    }
)
ZERO_DELTA_COUNTERS = (
    "controller_jtc_cancel_reference_rejections_total",
    "runtime_collision_lookahead_jobs_cycle_timeout",
    "runtime_collision_lookahead_jobs_inconclusive",
    "runtime_collision_lookahead_jobs_query_timeout",
    "runtime_collision_snapshot_refresh_failures",
    "runtime_collision_stop_unavailable_ticks",
    "runtime_collision_tick_late_periods",
    "runtime_collision_tick_missed_periods",
    "runtime_collision_ticks_faulted",
    "runtime_collision_unavailable_events_suppressed",
    "scene_projection_completion_callback_exceptions_total",
    "scene_projection_terminal_retry_exhausted_total",
    "validation_collision_jobs_abandoned",
    "validation_collision_jobs_cancelled",
    "validation_collision_jobs_dropped",
)
FRESH_BASELINE_METRICS = frozenset(
    {
        "controller_command_progress_samples_total",
        "controller_last_command_progress_latency_ns",
        "controller_max_command_progress_latency_ns",
        "runtime_collision_tick_max_ns",
        "runtime_collision_ticks_started",
        "runtime_collision_lookahead_jobs_started",
        "runtime_collision_lookahead_max_cycle_duration_ns",
        "runtime_collision_lookahead_max_start_jitter_ns",
        "runtime_collision_lookahead_max_worker_active_duration_ns",
        "validation_collision_jobs_abandoned",
        "validation_collision_jobs_cancelled",
        "validation_collision_jobs_completed",
        "validation_collision_jobs_dropped",
        "validation_collision_jobs_queued",
    }
)
_OVERRUN = re.compile(
    r"Overrun detected!.*?missed\s+cycles\s*:\s*(?P<cycles>[0-9]+)",
    re.IGNORECASE,
)
_SHA256 = re.compile(r"[0-9a-f]{64}")
_GIT_SHA = re.compile(r"[0-9a-f]{40}")
_VERIFIER_RELATIVE_PATH = Path(
    "scripts/qualification/verify_step18_live_contact_modes.py"
)
_INVOCATION_SCHEMA = "manyforge.step18.producer_invocation.v1"
MECHANISM_REPLAY_SCHEMA = "manyforge.step18_isaac_live_mechanism_replay.v1"
MECHANISM_REPLAY_MANIFEST_SCHEMA = (
    "manyforge.step18_live_mechanism_replay_manifest.v1"
)


class _FileTracker:
    """Buffer every input once and reject any later byte change."""

    def __init__(self) -> None:
        self._payloads: dict[Path, bytes] = {}
        self._digests: dict[Path, str] = {}

    def read(self, path: Path, label: str) -> tuple[bytes, str]:
        resolved = path.resolve()
        if resolved in self._payloads:
            return self._payloads[resolved], self._digests[resolved]
        try:
            payload = resolved.read_bytes()
        except OSError as exc:
            raise ValidationError(f"{label} is not readable: {exc}") from exc
        digest = hashlib.sha256(payload).hexdigest()
        self._payloads[resolved] = payload
        self._digests[resolved] = digest
        return payload, digest

    def assert_unchanged(self) -> None:
        for path, expected in self._digests.items():
            try:
                observed = hashlib.sha256(path.read_bytes()).hexdigest()
            except OSError as exc:
                raise ValidationError(
                    f"retained live input disappeared: {path}: {exc}"
                ) from exc
            if observed != expected:
                raise ValidationError(
                    f"retained live input changed during verification: {path}"
                )

    def binding(self, path: Path) -> dict[str, str]:
        resolved = path.resolve()
        digest = self._digests.get(resolved)
        if digest is None:  # pragma: no cover - caller construction invariant
            raise ValidationError(f"live input was not buffered: {resolved}")
        return {"path": str(resolved), "sha256": digest}

    @property
    def paths(self) -> frozenset[Path]:
        return frozenset(self._digests)


def _json_payload(payload: bytes, field: str) -> dict[str, Any]:
    try:
        value = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValidationError(f"{field}: cannot decode JSON: {exc}") from exc
    if not isinstance(value, dict):
        raise ValidationError(f"{field}: top-level value must be an object")
    return value


def ensure_ignored_qualification_output(path: Path, repo_root: Path) -> Path:
    """Require CLI output below the candidate's ignored evidence directory."""

    resolved_root = repo_root.resolve()
    resolved = path.resolve()
    evidence_root = (resolved_root / ".manyforge-var/qualification").resolve()
    try:
        relative = resolved.relative_to(evidence_root)
    except ValueError as exc:
        raise ValidationError(
            f"CLI output must be below ignored evidence root {evidence_root}"
        ) from exc
    if not relative.parts:
        raise ValidationError("CLI output must name a file below the evidence root")
    try:
        repository_relative = resolved.relative_to(resolved_root)
    except ValueError as exc:  # pragma: no cover - implied by evidence-root check
        raise ValidationError("CLI output escapes the candidate repository") from exc
    ignored = subprocess.run(
        [
            "git",
            "-C",
            str(resolved_root),
            "check-ignore",
            "-q",
            "--",
            str(repository_relative),
        ],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if ignored.returncode != 0:
        raise ValidationError("CLI output is not ignored by the candidate repository")
    return resolved


def _validate_evidence_payload(payload: bytes, field: str) -> dict[str, Any]:
    """Apply the shared evidence validator to the already hash-bound bytes."""

    evidence = _json_payload(payload, field)
    with tempfile.TemporaryDirectory(prefix="step18-live-evidence-") as raw_root:
        buffered_path = Path(raw_root) / "evidence.json"
        buffered_path.write_bytes(payload)
        validate_evidence(buffered_path)
    return evidence


def _canonical_sha256(value: Any) -> str:
    return hashlib.sha256(
        json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()


def _require_sha256(value: Any, field: str) -> str:
    if not isinstance(value, str) or _SHA256.fullmatch(value) is None:
        raise ValidationError(f"{field} must be a lowercase SHA-256")
    return value


def _resolve_path(manifest_path: Path, value: Any, field: str) -> Path:
    if not isinstance(value, str) or not value:
        raise ValidationError(f"{field} must be a non-empty path")
    path = Path(value)
    if not path.is_absolute():
        path = manifest_path.parent / path
    path = path.resolve()
    try:
        path.relative_to(manifest_path.parent.resolve())
    except ValueError as exc:
        raise ValidationError(f"{field} escapes the manifest evidence root") from exc
    if not path.is_file():
        raise ValidationError(f"{field} does not exist: {path}")
    return path


def _finite_nonnegative(value: Any, field: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValidationError(f"{field} must be numeric")
    result = float(value)
    if not math.isfinite(result) or result < 0.0:
        raise ValidationError(f"{field} must be finite and non-negative")
    return result


def _parse_time(value: Any, field: str) -> dt.datetime:
    if not isinstance(value, str) or not value.endswith("Z"):
        raise ValidationError(f"{field} must be an RFC3339 UTC timestamp")
    try:
        parsed = dt.datetime.fromisoformat(value[:-1] + "+00:00")
    except ValueError as exc:
        raise ValidationError(f"{field} is not a valid timestamp") from exc
    return parsed


def _delta_sha256(tracked_diff: bytes) -> str:
    digest = hashlib.sha256()
    digest.update(b"tracked-diff\0")
    digest.update(len(tracked_diff).to_bytes(8, "big"))
    digest.update(tracked_diff)
    digest.update(b"untracked-files\0")
    digest.update((0).to_bytes(8, "big"))
    return digest.hexdigest()


def _source_identity(
    *, head_commit: str, head_tree: str, dirty: bool, delta_sha256: str
) -> dict[str, Any]:
    return {
        "schema_version": MANYFORGE_SOURCE_IDENTITY_SCHEMA,
        "head_commit": head_commit,
        "head_tree": head_tree,
        "dirty": dirty,
        "delta_sha256": delta_sha256,
        "fingerprint_sha256": _manyforge_source_fingerprint_sha256(
            head_commit=head_commit,
            head_tree=head_tree,
            dirty=dirty,
            delta_sha256=delta_sha256,
        ),
    }


def _validated_source_identity(value: Any, field: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValidationError(f"{field} must be an object")
    expected_fields = {
        "schema_version",
        "head_commit",
        "head_tree",
        "dirty",
        "delta_sha256",
        "fingerprint_sha256",
    }
    if set(value) != expected_fields:
        raise ValidationError(f"{field} has missing or unknown fields")
    if value.get("schema_version") != MANYFORGE_SOURCE_IDENTITY_SCHEMA:
        raise ValidationError(f"{field}.schema_version is invalid")
    for name in ("head_commit", "head_tree"):
        if not isinstance(value.get(name), str) or _GIT_SHA.fullmatch(value[name]) is None:
            raise ValidationError(f"{field}.{name} must be a full Git SHA")
    if not isinstance(value.get("dirty"), bool):
        raise ValidationError(f"{field}.dirty must be boolean")
    _require_sha256(value.get("delta_sha256"), f"{field}.delta_sha256")
    _require_sha256(value.get("fingerprint_sha256"), f"{field}.fingerprint_sha256")
    expected = _source_identity(
        head_commit=value["head_commit"],
        head_tree=value["head_tree"],
        dirty=value["dirty"],
        delta_sha256=value["delta_sha256"],
    )
    if value != expected:
        raise ValidationError(f"{field}.fingerprint_sha256 is inconsistent")
    return value


def _specs_contract_identity(
    *,
    head_commit: str,
    head_tree: str,
    contract_dirty: bool,
    contract_delta_sha256: str,
) -> dict[str, Any]:
    excluded_paths = [SPECS_CONTRACT_EXCLUDED_PATH]
    return {
        "schema_version": SPECS_CONTRACT_IDENTITY_SCHEMA,
        "head_commit": head_commit,
        "head_tree": head_tree,
        "excluded_paths": excluded_paths,
        "contract_dirty": contract_dirty,
        "contract_delta_sha256": contract_delta_sha256,
        "contract_fingerprint_sha256": specs_contract_fingerprint_sha256(
            head_commit=head_commit,
            head_tree=head_tree,
            excluded_paths=excluded_paths,
            contract_dirty=contract_dirty,
            contract_delta_sha256=contract_delta_sha256,
        ),
    }


def _validated_specs_contract_identity(value: Any, field: str) -> dict[str, Any]:
    try:
        return validate_specs_contract_identity(value)
    except SourceIdentityError as exc:
        raise ValidationError(f"{field}: {exc}") from exc


def _observation_patch_identity(
    payload: bytes, observed_sha256: str, expected_sha256: Any
) -> dict[str, Any]:
    digest = _require_sha256(expected_sha256, "baseline_observation_patch_sha256")
    if observed_sha256 != digest:
        raise ValidationError("baseline observation patch SHA-256 changed")
    text = payload.decode("utf-8", errors="strict")
    paths = {
        match.group(1)
        for match in re.finditer(r"^diff --git a/(.+?) b/(.+?)$", text, re.MULTILINE)
        if match.group(1) == match.group(2)
    }
    if paths != _OBSERVATION_PATCH_PATHS:
        raise ValidationError("baseline observation patch scope changed")
    for match in re.finditer(
        r"^diff --git a/(.+?) b/.+?\n(?:.*\n)*?index ([0-9a-f]{40})\.\.([0-9a-f]{40})",
        text,
        re.MULTILINE,
    ):
        relative, old_blob = match.group(1), match.group(2)
        if relative not in paths:
            continue
        actual = subprocess.check_output(
            ["git", "-C", str(REPO_ROOT), "rev-parse", f"{PINNED_BASELINE_COMMIT}:{relative}"],
            text=True,
        ).strip()
        if actual != old_blob:
            raise ValidationError(
                f"baseline observation patch preimage changed for {relative}"
            )
    if text.count("\nindex ") != len(paths):
        raise ValidationError("baseline observation patch lacks full-index preimages")
    return _source_identity(
        head_commit=PINNED_BASELINE_COMMIT,
        head_tree=PINNED_BASELINE_TREE,
        dirty=True,
        delta_sha256=_delta_sha256(payload),
    )


def _deadline_log(path: Path, payload: bytes, digest: str) -> dict[str, Any]:
    text = payload.decode("utf-8", errors="replace")
    event_lines = [line for line in text.splitlines() if "Overrun detected!" in line]
    cycles = [int(match.group("cycles")) for match in _OVERRUN.finditer(text)]
    return {
        "path": str(path),
        "sha256": digest,
        "bytes": len(payload),
        "overrun_events": len(event_lines),
        "parsed_events": len(cycles),
        "missed_cycles": sum(cycles),
        "all_events_parsed": len(event_lines) == len(cycles),
    }


def _validate_process_identity(value: Any, field: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValidationError(f"{field} must be an object")
    fingerprint = _require_sha256(value.get("fingerprint_sha256"), f"{field}.fingerprint_sha256")
    payload = {key: item for key, item in value.items() if key != "fingerprint_sha256"}
    if _canonical_sha256(payload) != fingerprint:
        raise ValidationError(f"{field}.fingerprint_sha256 is inconsistent")
    processes = value.get("processes")
    artifacts = value.get("mapped_native_artifacts")
    if not isinstance(processes, list) or not processes:
        raise ValidationError(f"{field}.processes is empty")
    if not isinstance(artifacts, list) or not artifacts:
        raise ValidationError(f"{field}.mapped_native_artifacts is empty")
    for index, artifact in enumerate(artifacts):
        if not isinstance(artifact, dict) or not Path(str(artifact.get("path") or "")).is_absolute():
            raise ValidationError(f"{field}.mapped_native_artifacts[{index}] is invalid")
        _require_sha256(artifact.get("sha256"), f"{field}.mapped_native_artifacts[{index}].sha256")
    for index, process in enumerate(processes):
        if not isinstance(process, dict):
            raise ValidationError(f"{field}.processes[{index}] is invalid")
        if isinstance(process.get("pid"), bool) or not isinstance(process.get("pid"), int) or process["pid"] <= 0:
            raise ValidationError(f"{field}.processes[{index}].pid is invalid")
        if not Path(str(process.get("exe") or "")).is_absolute():
            raise ValidationError(f"{field}.processes[{index}].exe is invalid")
        _require_sha256(process.get("exe_sha256"), f"{field}.processes[{index}].exe_sha256")
        if not isinstance(process.get("cmdline"), str) or not process["cmdline"]:
            raise ValidationError(f"{field}.processes[{index}].cmdline is missing")
    return value


def _validate_runtime_identity(value: Any, field: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValidationError(f"{field} must be an object")
    fingerprint = _require_sha256(value.get("fingerprint_sha256"), f"{field}.fingerprint_sha256")
    payload = {key: item for key, item in value.items() if key != "fingerprint_sha256"}
    if _canonical_sha256(payload) != fingerprint:
        raise ValidationError(f"{field}.fingerprint_sha256 is inconsistent")
    for name in ("name", "id", "image_id", "image_name", "started_at"):
        if not isinstance(value.get(name), str) or not value[name]:
            raise ValidationError(f"{field}.{name} is missing")
    if not str(value["image_id"]).startswith("sha256:"):
        raise ValidationError(f"{field}.image_id is not content-addressed")
    mount = value.get("workspace_mount")
    if (
        not isinstance(mount, dict)
        or mount.get("type") != "bind"
        or mount.get("destination") != "/workspace"
        or mount.get("rw") is not True
        or not Path(str(mount.get("source") or "")).is_absolute()
    ):
        raise ValidationError(f"{field}.workspace_mount is not the source bind")
    log_config = value.get("log_config")
    if not isinstance(log_config, dict) or log_config.get("Type") in {None, "", "none"}:
        raise ValidationError(f"{field}.log_config cannot retain controller evidence")
    _validate_process_identity(value.get("process"), f"{field}.process")
    return value


def _runtime_profile(value: Any, field: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValidationError(f"{field} must be an object")
    expected = {
        "host_repo_root",
        "composer_runtime_fingerprint_sha256",
        "controller_runtime_fingerprint_sha256",
        "composer_image_id",
        "controller_image_id",
        "composer_native_artifacts_sha256",
        "controller_native_artifacts_sha256",
    }
    if set(value) != expected:
        raise ValidationError(f"{field} has missing or unknown fields")
    if not Path(str(value.get("host_repo_root") or "")).is_absolute():
        raise ValidationError(f"{field}.host_repo_root must be absolute")
    for name in expected - {"host_repo_root", "composer_image_id", "controller_image_id"}:
        _require_sha256(value.get(name), f"{field}.{name}")
    for name in ("composer_image_id", "controller_image_id"):
        if not isinstance(value.get(name), str) or not value[name].startswith("sha256:"):
            raise ValidationError(f"{field}.{name} is invalid")
    return value


def _thor_host_identity(value: Any, field: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValidationError(f"{field} must be an object")
    expected = {
        "schema_version",
        "hostname",
        "architecture",
        "kernel_release",
        "device_tree_model",
        "gpus",
        "fingerprint_sha256",
    }
    if set(value) != expected:
        raise ValidationError(f"{field} has missing or unknown fields")
    if value.get("schema_version") != "manyforge.step18_thor_host_identity.v1":
        raise ValidationError(f"{field}.schema_version is invalid")
    fingerprint = _require_sha256(
        value.get("fingerprint_sha256"), f"{field}.fingerprint_sha256"
    )
    payload = {name: item for name, item in value.items() if name != "fingerprint_sha256"}
    if _canonical_sha256(payload) != fingerprint:
        raise ValidationError(f"{field}.fingerprint_sha256 is inconsistent")
    if value.get("architecture") != "aarch64":
        raise ValidationError(f"{field} is not an aarch64 Thor host")
    if value.get("device_tree_model") != "NVIDIA Jetson AGX Thor Developer Kit":
        raise ValidationError(f"{field} is not the declared Jetson AGX Thor target")
    if not isinstance(value.get("kernel_release"), str) or "tegra" not in value["kernel_release"]:
        raise ValidationError(f"{field}.kernel_release is not a Tegra kernel")
    if not isinstance(value.get("hostname"), str) or not value["hostname"]:
        raise ValidationError(f"{field}.hostname is missing")
    gpus = value.get("gpus")
    if not isinstance(gpus, list) or not gpus:
        raise ValidationError(f"{field}.gpus is empty")
    for index, gpu in enumerate(gpus):
        if not isinstance(gpu, dict) or set(gpu) != {
            "name",
            "uuid",
            "driver_version",
        }:
            raise ValidationError(f"{field}.gpus[{index}] is invalid")
        if gpu.get("name") != "NVIDIA Thor":
            raise ValidationError(f"{field}.gpus[{index}] is not NVIDIA Thor")
        if any(not isinstance(gpu.get(name), str) or not gpu[name] for name in gpu):
            raise ValidationError(f"{field}.gpus[{index}] identity is incomplete")
    return value


def _one_live_record(evidence: dict[str, Any], field: str) -> dict[str, Any]:
    records = evidence.get("records")
    if not isinstance(records, list) or len(records) != 1:
        raise ValidationError(f"{field} must contain exactly one live record")
    record = records[0]
    if (
        not isinstance(record, dict)
        or record.get("record_type") != "live_planning_stage"
        or record.get("fixture_id") != "isaac_light_drop_approach"
    ):
        raise ValidationError(f"{field} does not contain a live planning record")
    if record.get("passed") is not True:
        raise ValidationError(f"{field} live record did not pass")
    sample_count = record.get("sample_count")
    if isinstance(sample_count, bool) or not isinstance(sample_count, int) or sample_count < 30:
        raise ValidationError(f"{field} requires at least 30 successful observations")
    details = record.get("details")
    if not isinstance(details, dict) or details.get("measurement_mode") != "primary":
        raise ValidationError(f"{field} must use uncontaminated primary measurement")
    return record


def _trajectory_fingerprint(details: dict[str, Any], field: str) -> str:
    samples = details.get("samples")
    if not isinstance(samples, list) or not samples:
        raise ValidationError(f"{field}.samples is empty")
    retained: list[dict[str, Any]] = []
    for index, sample in enumerate(samples):
        result = sample.get("result") if isinstance(sample, dict) else None
        metrics = sample.get("planner_metrics") if isinstance(sample, dict) else None
        if not isinstance(result, dict) or not isinstance(metrics, dict):
            raise ValidationError(f"{field}.samples[{index}] is incomplete")
        retained.append(
            {
                "trajectory_sha256": _require_sha256(
                    result.get("trajectory_sha256"),
                    f"{field}.samples[{index}].result.trajectory_sha256",
                ),
                "waypoint_count": result.get("waypoint_count"),
                "trajectory_joint_names": result.get("trajectory_joint_names"),
                "rng_base_seed": metrics.get("rng_base_seed"),
                "rng_seed": metrics.get("rng_seed"),
                "shortcut_rng_seed_argument": metrics.get("shortcut_rng_seed_argument"),
            }
        )
    return _canonical_sha256(retained)


def retained_live_output_sha256(evidence: dict[str, Any], field: str) -> str:
    """Hash only logical outputs retained by the live evidence contract.

    Normalized collision-pair outputs are intentionally not claimed here: the
    live collector does not retain them on successful validation.  That parity
    belongs to the offline baseline mechanism lane.
    """

    records = evidence.get("records")
    if not isinstance(records, list) or len(records) != 1:
        raise ValidationError(f"{field} must contain exactly one record")
    details = records[0].get("details") if isinstance(records[0], dict) else None
    samples = details.get("samples") if isinstance(details, dict) else None
    if not isinstance(samples, list) or not samples:
        raise ValidationError(f"{field}.samples is empty")
    retained: list[dict[str, Any]] = []
    for index, sample in enumerate(samples):
        request = sample.get("request") if isinstance(sample, dict) else None
        result = sample.get("result") if isinstance(sample, dict) else None
        metrics = sample.get("planner_metrics") if isinstance(sample, dict) else None
        if not all(isinstance(value, dict) for value in (request, result, metrics)):
            raise ValidationError(f"{field}.samples[{index}] is incomplete")
        validation = result.get("authoritative_validation")
        if not isinstance(validation, dict):
            raise ValidationError(
                f"{field}.samples[{index}].authoritative_validation is missing"
            )
        retained.append(
            {
                "request": {
                    "contact_policy_id": request.get("contact_policy_id"),
                    "contact_policy_resolution": request.get(
                        "contact_policy_resolution"
                    ),
                    "scene_binding": request.get("scene_binding"),
                    "configured_policy_revision": request.get(
                        "configured_policy_revision"
                    ),
                },
                "planner": {
                    "measurement_complete": metrics.get("measurement_complete"),
                    "succeeded": metrics.get("succeeded"),
                    "error_code": metrics.get("error_code"),
                    "waypoint_count": metrics.get("waypoint_count"),
                    "total_collision_query_count": metrics.get(
                        "total_collision_query_count"
                    ),
                    "rng_base_seed": metrics.get("rng_base_seed"),
                    "rng_seed": metrics.get("rng_seed"),
                    "shortcut_rng_seed_argument": metrics.get(
                        "shortcut_rng_seed_argument"
                    ),
                },
                "result": {
                    "succeeded": result.get("succeeded"),
                    "waypoint_count": result.get("waypoint_count"),
                    "trajectory_joint_names": result.get(
                        "trajectory_joint_names"
                    ),
                    "trajectory_sha256": result.get("trajectory_sha256"),
                    "scene_revision": result.get("scene_revision"),
                    "authoritative_validation_decision": validation.get("decision"),
                },
            }
        )
    return _canonical_sha256(retained)


def _mechanism_capture_identity(
    value: Any, *, details: dict[str, Any], field: str
) -> dict[str, Any]:
    """Validate and project the exact live replay input without weakening it."""

    if not isinstance(value, dict):
        raise ValidationError(f"{field} must be an object")
    payload = (
        json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)
        + "\n"
    ).encode("utf-8")
    with tempfile.NamedTemporaryFile(suffix=".json") as stream:
        stream.write(payload)
        stream.flush()
        validate_capture(Path(stream.name), required_attachment="isaac_cheese_box")

    source = value.get("source")
    planning = value.get("planning_result")
    scene = value.get("scene")
    robot = scene.get("robot") if isinstance(scene, dict) else None
    authority = scene.get("authority") if isinstance(scene, dict) else None
    if not all(isinstance(item, dict) for item in (source, planning, scene, robot, authority)):
        raise ValidationError(f"{field} is missing source/planning/scene authority")
    if source.get("move_id") != "drop_approach":
        raise ValidationError(f"{field} is not the drop-approach fixture")
    if robot.get("robot_id") != "ur10e":
        raise ValidationError(f"{field} is not the UR10e live fixture")

    trajectory_sha256 = _require_sha256(
        planning.get("trajectory_sha256"), f"{field}.planning_result.trajectory_sha256"
    )
    seed = robot.get("collision_projection_seed")
    if not isinstance(seed, dict):
        raise ValidationError(f"{field}.scene.robot.collision_projection_seed is missing")
    projection_seed_sha256 = _require_sha256(
        seed.get("sha256"), f"{field}.scene.robot.collision_projection_seed.sha256"
    )
    revision = authority.get("revision")
    if not isinstance(revision, dict) or not isinstance(
        revision.get("content_hash"), str
    ) or not revision["content_hash"]:
        raise ValidationError(f"{field}.scene.authority.revision is incomplete")

    samples = details.get("samples")
    if not isinstance(samples, list):
        raise ValidationError(f"{field}: live samples are missing")
    execution_id = source.get("planning_execution_id")
    selected = [
        sample
        for sample in samples
        if isinstance(sample, dict) and sample.get("execution_id") == execution_id
    ]
    if len(selected) != 1:
        raise ValidationError(
            f"{field}: captured planning execution is not uniquely selected"
        )
    result = selected[0].get("result")
    if not isinstance(result, dict) or result.get("trajectory_sha256") != trajectory_sha256:
        raise ValidationError(f"{field}: captured trajectory differs from live sample")

    geometry_objects: list[dict[str, Any]] = []
    for item in scene.get("objects") or []:
        if not isinstance(item, dict):
            raise ValidationError(f"{field}: scene object is invalid")
        geometry_objects.append(
            {
                key: child
                for key, child in item.items()
                if key != "support_contacts"
            }
        )
    geometry = {
        "world_frame": scene.get("world_frame"),
        "robot": robot,
        "objects": geometry_objects,
    }
    policy = {
        "configured_collision_policy": authority.get(
            "configured_collision_policy"
        ),
        "object_support_contacts": [
            {
                "object_id": item.get("object_id"),
                "support_contacts": item.get("support_contacts"),
            }
            for item in scene.get("objects") or []
        ],
    }
    return {
        "capture_id": value.get("capture_id"),
        "capture_payload_sha256": hashlib.sha256(payload).hexdigest(),
        "capture_semantic_sha256": _require_sha256(
            value.get("semantic_sha256"), f"{field}.semantic_sha256"
        ),
        "trajectory_sha256": trajectory_sha256,
        "projection_seed_sha256": projection_seed_sha256,
        "captured_scene_content_hash": revision["content_hash"],
        "geometry_sha256": _canonical_sha256(geometry),
        "captured_policy_sha256": _canonical_sha256(policy),
        "planning_execution_id": execution_id,
    }


def _command_value(command: list[str], option: str) -> str | None:
    try:
        index = command.index(option)
    except ValueError:
        return None
    return command[index + 1] if index + 1 < len(command) else None


def _row_payload(
    manifest_path: Path,
    row: dict[str, Any],
    *,
    tracker: _FileTracker,
    candidate_identity: dict[str, Any],
    pristine_identity: dict[str, Any],
    observation_identity: dict[str, Any],
    baseline_specs_identity: dict[str, Any],
    candidate_specs_identity: dict[str, Any],
    baseline_specs_contract_identity: dict[str, Any],
    candidate_specs_contract_identity: dict[str, Any],
    runtime_profiles: dict[str, dict[str, Any]],
    expected_runner_sha256: str,
    expected_tool_bundle_sha256: str,
    expected_thor_host_identity: dict[str, Any],
    bounded_threshold_m: float,
) -> dict[str, Any]:
    row_id = row.get("id")
    row_kind = row.get("row_kind")
    mode = row.get("mode")
    stratum = row.get("viewer_stratum")
    if not isinstance(row_id, str) or not row_id:
        raise ValidationError("manifest row id must be non-empty")
    if row_kind not in ROW_KINDS:
        raise ValidationError(f"row {row_id}: row_kind is invalid")
    if stratum not in VIEWER_STRATA:
        raise ValidationError(f"row {row_id}: viewer_stratum is invalid")
    if row_kind in CANDIDATE_ROW_KINDS:
        if mode not in CANDIDATE_MODES:
            raise ValidationError(f"row {row_id}: candidate mode is invalid")
    elif mode != BASELINE_MODE:
        raise ValidationError(f"row {row_id}: baseline mode is invalid")
    row_uuid = row.get("row_uuid")
    try:
        uuid.UUID(str(row_uuid))
    except ValueError as exc:
        raise ValidationError(f"row {row_id}: row_uuid is invalid") from exc

    evidence_path = _resolve_path(manifest_path, row.get("evidence"), f"row {row_id}.evidence")
    deadline_path = _resolve_path(manifest_path, row.get("controller_log"), f"row {row_id}.controller_log")
    metadata_path = _resolve_path(manifest_path, row.get("run_metadata"), f"row {row_id}.run_metadata")
    expected_evidence_sha = _require_sha256(row.get("evidence_sha256"), f"row {row_id}.evidence_sha256")
    expected_log_sha = _require_sha256(row.get("controller_log_sha256"), f"row {row_id}.controller_log_sha256")
    expected_metadata_sha = _require_sha256(row.get("run_metadata_sha256"), f"row {row_id}.run_metadata_sha256")
    evidence_payload, evidence_digest = tracker.read(
        evidence_path, f"row {row_id} evidence"
    )
    deadline_payload, deadline_digest = tracker.read(
        deadline_path, f"row {row_id} controller log"
    )
    metadata_payload, metadata_digest = tracker.read(
        metadata_path, f"row {row_id} run metadata"
    )
    if evidence_digest != expected_evidence_sha:
        raise ValidationError(f"row {row_id}: evidence SHA-256 changed")
    if deadline_digest != expected_log_sha:
        raise ValidationError(f"row {row_id}: controller log SHA-256 changed")
    if metadata_digest != expected_metadata_sha:
        raise ValidationError(f"row {row_id}: run metadata SHA-256 changed")

    metadata = _json_payload(metadata_payload, f"row {row_id} run metadata")
    if metadata.get("schema_version") != "manyforge.step18_live_row_run.v2":
        raise ValidationError(f"row {row_id}: run metadata schema is invalid")
    if metadata.get("row_uuid") != row_uuid or metadata.get("row_kind") != row_kind:
        raise ValidationError(f"row {row_id}: run metadata row binding changed")
    if metadata.get("collector_exit_code") != 0:
        raise ValidationError(f"row {row_id}: collector did not exit successfully")
    if metadata.get("viewer_stratum") != stratum or metadata.get("support_contact_mode") != mode:
        raise ValidationError(f"row {row_id}: run metadata labels changed")
    if metadata.get("runtime_identity_verified_after_run") is not True:
        raise ValidationError(f"row {row_id}: runtime identity was not rechecked")
    if Path(str(metadata.get("evidence_path") or "")).resolve() != evidence_path:
        raise ValidationError(f"row {row_id}: metadata evidence path changed")
    if metadata.get("evidence_sha256") != expected_evidence_sha:
        raise ValidationError(f"row {row_id}: metadata evidence hash changed")
    if Path(str(metadata.get("controller_log_path") or "")).resolve() != deadline_path:
        raise ValidationError(f"row {row_id}: metadata log path changed")
    if metadata.get("controller_log_sha256") != expected_log_sha:
        raise ValidationError(f"row {row_id}: metadata log hash changed")
    started = _parse_time(metadata.get("started_at_utc"), f"row {row_id}.started_at_utc")
    ended = _parse_time(metadata.get("ended_at_utc"), f"row {row_id}.ended_at_utc")
    if ended <= started:
        raise ValidationError(f"row {row_id}: run interval is not positive")
    if not math.isclose(float(metadata.get("duration_s", -1.0)), (ended - started).total_seconds(), rel_tol=0.0, abs_tol=1e-6):
        raise ValidationError(f"row {row_id}: run duration is inconsistent")

    host_runner = metadata.get("host_runner")
    expected_runner = REPO_ROOT / "scripts/qualification/run_step18_live_contact_mode.py"
    _, observed_runner_sha256 = tracker.read(
        expected_runner, "Step 18 live row runner"
    )
    if (
        not isinstance(host_runner, dict)
        or Path(str(host_runner.get("path") or "")).resolve() != expected_runner.resolve()
        or host_runner.get("sha256") != expected_runner_sha256
        or observed_runner_sha256 != expected_runner_sha256
    ):
        raise ValidationError(f"row {row_id}: host runner provenance changed")
    bundle = metadata.get("qualification_tool_bundle")
    if (
        not isinstance(bundle, dict)
        or bundle.get("schema_version") != "manyforge.step18_qualification_tool_bundle.v1"
        or bundle.get("fingerprint_sha256") != expected_tool_bundle_sha256
        or _canonical_sha256(bundle.get("files")) != expected_tool_bundle_sha256
    ):
        raise ValidationError(f"row {row_id}: qualification tool bundle changed")

    expected_identity = {
        "baseline_pristine_timing": pristine_identity,
        "baseline_observation": observation_identity,
        "candidate_timing": candidate_identity,
        "candidate_observation": candidate_identity,
    }[row_kind]
    if metadata.get("observed_source_identity") != expected_identity:
        raise ValidationError(f"row {row_id}: runtime source identity changed")
    if metadata.get("expected_source_fingerprint_sha256") != expected_identity["fingerprint_sha256"]:
        raise ValidationError(f"row {row_id}: expected source fingerprint changed")
    expected_specs = (
        candidate_specs_identity
        if row_kind in CANDIDATE_ROW_KINDS
        else baseline_specs_identity
    )
    if metadata.get("specs_source_identity") != expected_specs:
        raise ValidationError(f"row {row_id}: Specs source identity changed")
    expected_specs_contract = (
        candidate_specs_contract_identity
        if row_kind in CANDIDATE_ROW_KINDS
        else baseline_specs_contract_identity
    )
    if metadata.get("specs_contract_identity") != expected_specs_contract:
        raise ValidationError(f"row {row_id}: Specs contract identity changed")
    if metadata.get("thor_host_identity") != expected_thor_host_identity:
        raise ValidationError(f"row {row_id}: Thor host identity changed")

    composer_runtime = _validate_runtime_identity(metadata.get("composer_runtime"), f"row {row_id}.composer_runtime")
    controller_runtime = _validate_runtime_identity(metadata.get("controller_runtime"), f"row {row_id}.controller_runtime")
    if composer_runtime["name"] != "manyforge-composer-ros-1":
        raise ValidationError(f"row {row_id}: Composer runtime is not the live lane")
    if controller_runtime["name"] != "manyforge-isaac-topic-control-1":
        raise ValidationError(f"row {row_id}: controller runtime is not the Isaac lane")
    controller_commands = " ".join(
        process.get("cmdline", "")
        for process in controller_runtime["process"]["processes"]
    ).lower()
    if "isaac" not in controller_commands:
        raise ValidationError(f"row {row_id}: controller process does not identify Isaac")
    profile = runtime_profiles[row_kind]
    if metadata.get("host_repo_root") != profile["host_repo_root"]:
        raise ValidationError(f"row {row_id}: host source root changed")
    for runtime_name, runtime in (("composer", composer_runtime), ("controller", controller_runtime)):
        if runtime["workspace_mount"]["source"] != profile["host_repo_root"]:
            raise ValidationError(f"row {row_id}: {runtime_name} mount source changed")
        if runtime["fingerprint_sha256"] != profile[f"{runtime_name}_runtime_fingerprint_sha256"]:
            raise ValidationError(f"row {row_id}: {runtime_name} runtime fingerprint changed")
        if runtime["image_id"] != profile[f"{runtime_name}_image_id"]:
            raise ValidationError(f"row {row_id}: {runtime_name} image changed")
        artifacts_sha = _canonical_sha256(runtime["process"]["mapped_native_artifacts"])
        if artifacts_sha != profile[f"{runtime_name}_native_artifacts_sha256"]:
            raise ValidationError(f"row {row_id}: {runtime_name} loaded native artifacts changed")
        if _parse_time(runtime["started_at"], f"row {row_id}.{runtime_name}.started_at") > started:
            raise ValidationError(f"row {row_id}: {runtime_name} started after row start")

    log_command = metadata.get("controller_log_command")
    if not isinstance(log_command, list) or log_command != [
        "docker", "logs", "--timestamps", "--since", metadata["started_at_utc"],
        "--until", metadata["ended_at_utc"], controller_runtime["name"],
    ]:
        raise ValidationError(f"row {row_id}: controller log interval is not bound")
    expected_markers = {
        "start": f"MANYFORGE_STEP18_ROW {row_uuid} start",
        "end": f"MANYFORGE_STEP18_ROW {row_uuid} end",
    }
    if metadata.get("controller_log_markers") != expected_markers:
        raise ValidationError(f"row {row_id}: controller log markers are not bound")
    controller_log_text = deadline_payload.decode("utf-8", errors="replace")
    if any(marker not in controller_log_text for marker in expected_markers.values()):
        raise ValidationError(f"row {row_id}: controller log marker capture is incomplete")
    command_text = metadata.get("command")
    if not isinstance(command_text, str) or not command_text:
        raise ValidationError(f"row {row_id}: collector command is missing")
    command = shlex.split(command_text)
    required_command_values = {
        "--run-id": row_uuid,
        "--viewer-stratum": stratum,
        "--support-contact-mode": mode,
        "--measurement-mode": "primary",
        "--successful-observations": str(metadata.get("successful_observations")),
        "--poll-period-s": str(metadata.get("poll_period_s")),
        "--phase": str(metadata.get("phase")),
        "--max-contact-penetration-m": str(
            metadata.get("max_contact_penetration_m")
        ),
        "--expected-source-fingerprint-sha256": expected_identity["fingerprint_sha256"],
        "--controller-progress-observer-mode": (
            "quiescent_repeated" if row_kind == "baseline_pristine_timing" else "coherent_native"
        ),
    }
    for option, expected_value in required_command_values.items():
        if _command_value(command, option) != expected_value:
            raise ValidationError(f"row {row_id}: command {option} changed")

    evidence = _validate_evidence_payload(
        evidence_payload, f"row {row_id} evidence"
    )
    if evidence.get("run_id") != row_uuid or evidence.get("phase") != metadata.get("phase"):
        raise ValidationError(f"row {row_id}: evidence run binding changed")
    record = _one_live_record(evidence, f"row {row_id}")
    details = record["details"]
    if details.get("viewer_stratum") != stratum:
        raise ValidationError(f"row {row_id}: artifact stratum is mislabeled")
    viewer_clients = details.get("viewer_state_websocket_clients")
    if isinstance(viewer_clients, bool) or not isinstance(viewer_clients, int):
        raise ValidationError(f"row {row_id}: viewer client count is unavailable")
    expected_focus_evidence = (
        "operator_attested_and_state_client_connected"
        if stratum == "focused"
        else "server_verified_zero_state_clients"
    )
    if details.get("viewer_focus_evidence") != expected_focus_evidence:
        raise ValidationError(f"row {row_id}: viewer load evidence changed")
    if (stratum == "focused" and viewer_clients < 1) or (
        stratum == "disconnected" and viewer_clients != 0
    ):
        raise ValidationError(f"row {row_id}: viewer stratum is not physically active")
    source = evidence.get("source")
    environment = evidence.get("environment")
    if not isinstance(source, dict) or not isinstance(environment, dict):
        raise ValidationError(f"row {row_id}: source/environment is incomplete")
    if environment.get("lane") != "ros_isaac_container":
        raise ValidationError(f"row {row_id}: evidence is not the ROS/Isaac lane")
    if environment.get("architecture") != expected_thor_host_identity["architecture"]:
        raise ValidationError(f"row {row_id}: container architecture differs from Thor")
    if metadata.get("successful_observations") != record["sample_count"]:
        raise ValidationError(f"row {row_id}: configured observation count changed")
    if metadata.get("controller_progress_observer_mode") != required_command_values[
        "--controller-progress-observer-mode"
    ]:
        raise ValidationError(f"row {row_id}: controller observer mode changed")
    poll_period = _finite_nonnegative(
        metadata.get("poll_period_s"), f"row {row_id}.poll_period_s"
    )
    if poll_period <= 0.0:
        raise ValidationError(f"row {row_id}: poll_period_s must be positive")
    if source.get("manyforge_source_identity") != expected_identity:
        raise ValidationError(f"row {row_id}: evidence source identity changed")
    if source.get("manyforge_source_fingerprint_sha256") != expected_identity["fingerprint_sha256"]:
        raise ValidationError(f"row {row_id}: evidence source fingerprint changed")
    expected_mode = "clean_git_commit" if row_kind == "baseline_pristine_timing" else "frozen_source_fingerprint_sha256"
    if source.get("manyforge_source_identity_mode") != expected_mode:
        raise ValidationError(f"row {row_id}: evidence source identity mode changed")
    if source.get("manyforge_expected_source_fingerprint_sha256") != expected_identity["fingerprint_sha256"]:
        raise ValidationError(f"row {row_id}: evidence frozen-source declaration changed")
    if environment.get("source_verified_after_run") is not True:
        raise ValidationError(f"row {row_id}: source was not verified after the run")
    matched_input = _require_sha256(source.get("matched_input_sha256"), f"row {row_id}.matched_input_sha256")
    artifacts: dict[str, dict[str, Any]] = {}
    for artifact_name in ("program", "deployment"):
        artifact = source.get(artifact_name)
        if not isinstance(artifact, dict):
            raise ValidationError(f"row {row_id}: {artifact_name} identity is missing")
        _require_sha256(artifact.get("sha256"), f"row {row_id}.{artifact_name}.sha256")
        _require_sha256(artifact.get("loaded_payload_sha256"), f"row {row_id}.{artifact_name}.loaded_payload_sha256")
        if not isinstance(artifact.get("path"), str) or not artifact["path"]:
            raise ValidationError(f"row {row_id}: {artifact_name} path is missing")
        artifacts[artifact_name] = artifact
    if artifacts["program"]["path"] != "examples/isaac_manipulator_scene_light.program.yaml":
        raise ValidationError(f"row {row_id}: live program fixture changed")
    if artifacts["deployment"]["path"] != (
        "examples/isaac_manipulator_scene_light_ros2_control.deployment.yaml"
    ):
        raise ValidationError(f"row {row_id}: live deployment fixture changed")
    reload = source.get("artifact_reload")
    if (
        not isinstance(reload, dict)
        or reload.get("schema_version") != "manyforge.step18_artifact_reload.v1"
        or reload.get("endpoint") != "/api/program/load"
        or reload.get("force_discard_overrides") is not True
        or reload.get("program_path") != artifacts["program"]["path"]
        or reload.get("deployment_path") != artifacts["deployment"]["path"]
        or reload.get("response_payload_sha256")
        != artifacts["program"]["loaded_payload_sha256"]
    ):
        raise ValidationError(
            f"row {row_id}: disk-backed program/deployment reload is not bound"
        )

    evidence_command_text = environment.get("command")
    if not isinstance(evidence_command_text, str) or not evidence_command_text:
        raise ValidationError(f"row {row_id}: evidence command is missing")
    evidence_command = shlex.split(evidence_command_text)
    for option, expected_value in required_command_values.items():
        if _command_value(evidence_command, option) != expected_value:
            raise ValidationError(f"row {row_id}: evidence command {option} changed")
    capture_flag = "--capture-mechanism-replay-input"
    if (capture_flag in command) != (row_kind in OBSERVATION_ROW_KINDS):
        raise ValidationError(f"row {row_id}: host command replay-capture scope changed")
    if (capture_flag in evidence_command) != (row_kind in OBSERVATION_ROW_KINDS):
        raise ValidationError(
            f"row {row_id}: evidence command replay-capture scope changed"
        )

    contact = details.get("step18_support_contact")
    if not isinstance(contact, dict) or contact.get("mode") != mode:
        raise ValidationError(f"row {row_id}: support-contact mode is mislabeled")
    if mode == "bounded_penetration":
        if _finite_nonnegative(contact.get("bounded_penetration_m"), f"row {row_id}.bounded_penetration_m") != bounded_threshold_m:
            raise ValidationError(f"row {row_id}: bounded threshold changed")
    elif contact.get("bounded_penetration_m") is not None:
        raise ValidationError(f"row {row_id}: non-bounded row carries a threshold")
    progress = details.get("controller_progress_latency")
    summary = progress.get("latency_ns") if isinstance(progress, dict) else None
    if not isinstance(summary, dict):
        raise ValidationError(f"row {row_id}: controller-progress evidence is missing")
    expected_progress_source = (
        "collector_quiescent_repeated_counter_transition"
        if row_kind == "baseline_pristine_timing"
        else "coherent_native_counter_transition"
    )
    if progress.get("source") != expected_progress_source:
        raise ValidationError(f"row {row_id}: controller-progress source changed")
    progress_p95 = _finite_nonnegative(summary.get("p95"), f"row {row_id}.controller_progress.p95")
    progress_max = _finite_nonnegative(summary.get("max"), f"row {row_id}.controller_progress.max")
    runtime = details.get("kernel_runtime_metrics")
    deltas = runtime.get("counter_deltas") if isinstance(runtime, dict) else None
    before = runtime.get("before") if isinstance(runtime, dict) else None
    after = runtime.get("after") if isinstance(runtime, dict) else None
    if not all(isinstance(value, dict) for value in (deltas, before, after)):
        raise ValidationError(f"row {row_id}: kernel counter deltas are missing")
    for boundary_name, boundary in (("before", before), ("after", after)):
        if boundary.get("runtime_collision_lookahead_max_samples") != 32:
            raise ValidationError(
                f"row {row_id}: {boundary_name} runtime lookahead is not exactly 32 samples"
            )
        workers = boundary.get("runtime_collision_lookahead_workers_effective")
        if isinstance(workers, bool) or not isinstance(workers, int) or workers < 2:
            raise ValidationError(
                f"row {row_id}: {boundary_name} runtime lookahead is not concurrent"
            )
    dirty_baseline = {
        name: before.get(name)
        for name in sorted(FRESH_BASELINE_METRICS)
        if before.get(name) != 0
    }
    if dirty_baseline:
        raise ValidationError(
            f"row {row_id}: program reload did not produce a fresh metric baseline"
        )
    zero_values: dict[str, int] = {}
    for name in ZERO_DELTA_COUNTERS:
        value = deltas.get(name)
        if isinstance(value, bool) or not isinstance(value, int) or value < 0:
            raise ValidationError(f"row {row_id}: counter {name} is unavailable")
        zero_values[name] = value

    positive_counters = {
        "controller_command_progress_samples_total": record["sample_count"],
        "runtime_collision_ticks_started": 1,
        "runtime_collision_lookahead_jobs_started": 1,
        "validation_collision_jobs_completed": record["sample_count"],
    }
    for name, minimum in positive_counters.items():
        value = deltas.get(name)
        if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
            raise ValidationError(
                f"row {row_id}: loaded runtime counter {name} did not advance"
            )

    samples = details.get("samples")
    if not isinstance(samples, list) or len(samples) != record["sample_count"]:
        raise ValidationError(f"row {row_id}: retained live samples are incomplete")
    for index, sample in enumerate(samples):
        result = sample.get("result") if isinstance(sample, dict) else None
        validation = result.get("authoritative_validation") if isinstance(result, dict) else None
        if not isinstance(validation, dict):
            raise ValidationError(f"row {row_id}: sample {index} validation is missing")
        try:
            workers_requested = int(validation.get("collision_workers_requested"))
            workers_effective = int(validation.get("collision_workers_effective"))
            workers_used = int(validation.get("collision_workers_used"))
            validation_wall_ns = int(validation.get("collision_validation_wall_ns"))
        except (TypeError, ValueError) as exc:
            raise ValidationError(
                f"row {row_id}: sample {index} validation telemetry is invalid"
            ) from exc
        if (
            min(workers_requested, workers_effective, workers_used) < 2
            or validation_wall_ns <= 0
            or validation.get("collision_parallel_fallback_reason") != ""
            or validation.get("collision_ordered_contact_summary_used") != "true"
        ):
            raise ValidationError(
                f"row {row_id}: sample {index} did not retain concurrent no-fallback validation"
            )

    mechanism_input = details.get("mechanism_replay_input")
    if row_kind in OBSERVATION_ROW_KINDS:
        mechanism_input_identity = _mechanism_capture_identity(
            mechanism_input,
            details=details,
            field=f"row {row_id}.details.mechanism_replay_input",
        )
    else:
        if mechanism_input is not None:
            raise ValidationError(
                f"row {row_id}: timing row retained perturbing mechanism input"
            )
        mechanism_input_identity = None

    return {
        "id": row_id,
        "row_uuid": row_uuid,
        "row_kind": row_kind,
        "mode": mode,
        "viewer_stratum": stratum,
        "evidence": {"path": str(evidence_path), "sha256": expected_evidence_sha},
        "controller_log": {
            "path": str(deadline_path),
            "sha256": expected_log_sha,
        },
        "run_metadata": {"path": str(metadata_path), "sha256": expected_metadata_sha},
        "source": source,
        "environment": environment,
        "sample_count": record["sample_count"],
        "matched_input_sha256": matched_input,
        "trajectory_fingerprint_sha256": _trajectory_fingerprint(details, f"row {row_id}.details"),
        "retained_live_output_sha256": retained_live_output_sha256(
            evidence, f"row {row_id}.details"
        ),
        "saved_scene_sha256": contact.get("saved_scene_sha256"),
        "effective_scene_sha256": contact.get("effective_scene_sha256"),
        "controller_progress_p95_ns": progress_p95,
        "controller_progress_max_ns": progress_max,
        "counter_zero_gates": zero_values,
        "loaded_runtime_positive_counters": {
            name: deltas[name] for name in sorted(positive_counters)
        },
        "thor_host_identity": expected_thor_host_identity,
        "controller_deadline_log": _deadline_log(
            deadline_path, deadline_payload, deadline_digest
        ),
        "mechanism_replay_input": mechanism_input_identity,
        "runtime_profile": profile,
        "metadata": metadata,
        "specs_contract_identity": expected_specs_contract,
        "matched_configuration_sha256": _canonical_sha256(
            {
                "phase": metadata.get("phase"),
                "successful_observations": metadata.get(
                    "successful_observations"
                ),
                "poll_period_s": poll_period,
                "architecture": environment.get("architecture"),
                "cpu_count": environment.get("cpu_count"),
                "cpu_set": environment.get("cpu_set"),
                "cpu_governor": environment.get("cpu_governor"),
                "composer_image_id": composer_runtime["image_id"],
                "composer_image_name": composer_runtime["image_name"],
                "controller_image_id": controller_runtime["image_id"],
                "controller_image_name": controller_runtime["image_name"],
            }
        ),
    }


def _verify_observation_parity(
    path: Path,
    expected_sha256: Any,
    rows: list[dict[str, Any]],
    pristine_identity: dict[str, Any],
    observation_identity: dict[str, Any],
    tracker: _FileTracker,
) -> dict[str, Any]:
    digest = _require_sha256(expected_sha256, "baseline_observation_parity_sha256")
    payload, observed_digest = tracker.read(
        path, "baseline retained-output parity artifact"
    )
    if observed_digest != digest:
        raise ValidationError("baseline observation parity SHA-256 changed")
    value = _json_payload(payload, "baseline retained-output parity artifact")
    if value.get("schema_version") != PARITY_SCHEMA or value.get("passed") is not True:
        raise ValidationError("baseline retained-output parity did not pass")
    if value.get("scope") != "retained_live_decision_trajectory_outputs":
        raise ValidationError("baseline retained-output parity scope is ambiguous")
    if value.get("pristine_source_fingerprint_sha256") != pristine_identity["fingerprint_sha256"]:
        raise ValidationError("baseline parity pristine source changed")
    if value.get("observation_source_fingerprint_sha256") != observation_identity["fingerprint_sha256"]:
        raise ValidationError("baseline parity observation source changed")
    bindings = value.get("rows")
    if not isinstance(bindings, list) or len(bindings) != len(VIEWER_STRATA):
        raise ValidationError("baseline parity must bind both viewer strata")
    for stratum in VIEWER_STRATA:
        binding = next((item for item in bindings if isinstance(item, dict) and item.get("viewer_stratum") == stratum), None)
        pristine = next(row for row in rows if row["row_kind"] == "baseline_pristine_timing" and row["viewer_stratum"] == stratum)
        observation = next(row for row in rows if row["row_kind"] == "baseline_observation" and row["viewer_stratum"] == stratum)
        if not isinstance(binding, dict):
            raise ValidationError(f"baseline parity omits {stratum}")
        if binding.get("pristine_evidence_sha256") != pristine["evidence"]["sha256"] or binding.get("observation_evidence_sha256") != observation["evidence"]["sha256"]:
            raise ValidationError(f"baseline parity {stratum} evidence binding changed")
        pristine_output = _require_sha256(
            binding.get("pristine_retained_live_output_sha256"),
            f"baseline parity {stratum}.pristine_retained_live_output_sha256",
        )
        observation_output = _require_sha256(
            binding.get("observation_retained_live_output_sha256"),
            f"baseline parity {stratum}.observation_retained_live_output_sha256",
        )
        if (
            pristine_output != pristine["retained_live_output_sha256"]
            or observation_output != observation["retained_live_output_sha256"]
        ):
            raise ValidationError(
                f"baseline parity {stratum} retained-output digest changed"
            )
        if pristine_output != observation_output:
            raise ValidationError(f"baseline parity {stratum} retained outputs differ")
        if pristine["trajectory_fingerprint_sha256"] != observation["trajectory_fingerprint_sha256"]:
            raise ValidationError(f"baseline parity {stratum} trajectories differ")
    return {
        "path": str(path),
        "sha256": digest,
        "scope": "retained_live_decision_trajectory_outputs",
    }


def _nonnegative_int(value: Any, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ValidationError(f"{field} must be a non-negative integer")
    return value


def _physical_pair_transcript(value: Any, field: str) -> list[dict[str, Any]]:
    if not isinstance(value, list) or not value:
        raise ValidationError(f"{field} must be a non-empty list")
    projected: list[dict[str, Any]] = []
    for index, record in enumerate(value):
        if not isinstance(record, dict):
            raise ValidationError(f"{field}[{index}] must be an object")
        waypoint = record.get("waypoint_index")
        if waypoint != index:
            raise ValidationError(f"{field}[{index}] is not in waypoint order")
        if not isinstance(record.get("in_collision"), bool):
            raise ValidationError(f"{field}[{index}].in_collision is invalid")
        engine_status = _nonnegative_int(
            record.get("engine_status"), f"{field}[{index}].engine_status"
        )
        _nonnegative_int(record.get("decision"), f"{field}[{index}].decision")
        pairs = record.get("normalized_pairs")
        if not isinstance(pairs, list):
            raise ValidationError(f"{field}[{index}].normalized_pairs is invalid")
        canonical: list[dict[str, str]] = []
        for pair_index, pair in enumerate(pairs):
            if not isinstance(pair, dict) or set(pair) != {"body_a", "body_b"}:
                raise ValidationError(
                    f"{field}[{index}].normalized_pairs[{pair_index}] is invalid"
                )
            body_a = pair.get("body_a")
            body_b = pair.get("body_b")
            if (
                not isinstance(body_a, str)
                or not body_a
                or not isinstance(body_b, str)
                or not body_b
                or body_a >= body_b
            ):
                raise ValidationError(
                    f"{field}[{index}].normalized_pairs[{pair_index}] is not canonical"
                )
            canonical.append({"body_a": body_a, "body_b": body_b})
        if canonical != sorted(canonical, key=lambda pair: (pair["body_a"], pair["body_b"])):
            raise ValidationError(f"{field}[{index}].normalized_pairs is not sorted")
        if len(canonical) != len({(pair["body_a"], pair["body_b"]) for pair in canonical}):
            raise ValidationError(f"{field}[{index}].normalized_pairs has duplicates")
        if bool(canonical) != record["in_collision"]:
            raise ValidationError(
                f"{field}[{index}] collision flag disagrees with pair output"
            )
        projected.append(
            {
                "waypoint_index": waypoint,
                "in_collision": record["in_collision"],
                "engine_status": engine_status,
                "normalized_pairs": canonical,
            }
        )
    return projected


def _policy_contacts(value: Any, *, arm: str, threshold: float, field: str) -> list[dict[str, Any]]:
    contacts = value.get("support_contacts") if isinstance(value, dict) else None
    if not isinstance(contacts, list) or not contacts:
        raise ValidationError(f"{field}.support_contacts is empty")
    pairs: list[tuple[str, str]] = []
    for index, contact in enumerate(contacts):
        item = f"{field}.support_contacts[{index}]"
        if not isinstance(contact, dict):
            raise ValidationError(f"{item} must be an object")
        pair = contact.get("pair")
        if not isinstance(pair, dict):
            raise ValidationError(f"{item}.pair is missing")
        body_a = pair.get("body_a")
        body_b = pair.get("body_b")
        if not isinstance(body_a, str) or not isinstance(body_b, str) or body_a >= body_b:
            raise ValidationError(f"{item}.pair is not canonical")
        pairs.append((body_a, body_b))
        numeric = {
            name: _finite_nonnegative(contact.get(name), f"{item}.{name}")
            for name in (
                "max_start_penetration_m",
                "release_tolerance_m",
                "min_release_progress_m",
                "max_contact_penetration_m",
            )
        }
        booleans = {
            name: contact.get(name)
            for name in (
                "enforce_max_start_penetration",
                "require_separation_by_end",
                "require_strictly_decreasing_penetration",
                "allow_recontact",
            )
        }
        if any(not isinstance(item_value, bool) for item_value in booleans.values()):
            raise ValidationError(f"{item} has a non-boolean policy field")
        mode = _nonnegative_int(contact.get("mode"), f"{item}.mode")
        evidence = _nonnegative_int(
            contact.get("release_evidence_mode"),
            f"{item}.release_evidence_mode",
        )
        if arm == "binary_contact":
            if (
                mode != 0
                or evidence != 1
                or any(numeric.values())
                or booleans
                != {
                    "enforce_max_start_penetration": False,
                    "require_separation_by_end": True,
                    "require_strictly_decreasing_penetration": False,
                    "allow_recontact": False,
                }
            ):
                raise ValidationError(f"{item} is not canonical binary contact")
        elif arm == "bounded_penetration":
            if (
                mode != 1
                or evidence != 0
                or numeric["max_contact_penetration_m"] != threshold
                or any(
                    numeric[name]
                    for name in (
                        "max_start_penetration_m",
                        "release_tolerance_m",
                        "min_release_progress_m",
                    )
                )
                or any(booleans.values())
            ):
                raise ValidationError(f"{item} is not canonical bounded penetration")
        else:
            if (
                mode != 0
                or evidence != 0
                or numeric["max_contact_penetration_m"] != 0.0
            ):
                raise ValidationError(f"{item} is not signed contact release")
    if pairs != sorted(set(pairs)):
        raise ValidationError(f"{field}.support_contacts is not canonical")
    return contacts


def _validate_replay_artifact(
    value: dict[str, Any],
    *,
    execution: dict[str, Any],
    capture_identity: dict[str, Any],
    threshold: float,
    field: str,
) -> dict[str, Any]:
    if value.get("schema_version") != MECHANISM_REPLAY_SCHEMA:
        raise ValidationError(f"{field}: replay schema is invalid")
    for name in ("run_id", "arm", "source_role"):
        if value.get(name) != execution.get(name):
            raise ValidationError(f"{field}.{name} changed")
    expected_measurement = (
        "observation" if execution["role"].endswith("observation") else "decision"
    )
    if value.get("measurement_kind") != expected_measurement:
        raise ValidationError(f"{field}.measurement_kind changed")
    replay_input = value.get("input")
    expected_input = {
        "capture_id": capture_identity["capture_id"],
        "semantic_sha256": capture_identity["capture_semantic_sha256"],
        "trajectory_sha256": capture_identity["trajectory_sha256"],
        "projection_seed_sha256": capture_identity["projection_seed_sha256"],
        "captured_scene_content_hash": capture_identity[
            "captured_scene_content_hash"
        ],
    }
    if replay_input != expected_input:
        raise ValidationError(f"{field}.input differs from exact live capture")
    replay = value.get("replay")
    if not isinstance(replay, dict):
        raise ValidationError(f"{field}.replay is missing")
    input_waypoints = _nonnegative_int(
        replay.get("input_waypoint_count"), f"{field}.replay.input_waypoint_count"
    )
    expanded_waypoints = _nonnegative_int(
        replay.get("expanded_waypoint_count"),
        f"{field}.replay.expanded_waypoint_count",
    )
    expanded_joints = _nonnegative_int(
        replay.get("expanded_joint_count"), f"{field}.replay.expanded_joint_count"
    )
    if input_waypoints < 2 or expanded_waypoints < input_waypoints or expanded_joints == 0:
        raise ValidationError(f"{field}.replay trajectory dimensions are invalid")
    for name in ("world_content_hash", "policy_content_hash"):
        if not isinstance(replay.get(name), str) or not replay[name]:
            raise ValidationError(f"{field}.replay.{name} is missing")

    decision = value.get("decision")
    if (
        not isinstance(decision, dict)
        or decision.get("accepted") is not True
        or decision.get("first_failing_waypoint") is not None
    ):
        raise ValidationError(f"{field}: replay did not accept the complete trajectory")
    _nonnegative_int(decision.get("action"), f"{field}.decision.action")
    transcript = _physical_pair_transcript(
        value.get("pair_transcript"), f"{field}.pair_transcript"
    )
    if len(transcript) != expanded_waypoints:
        raise ValidationError(f"{field}: pair transcript is incomplete")
    fingerprint = value.get("pair_transcript_fingerprint")
    if not isinstance(fingerprint, str) or not re.fullmatch(r"[0-9a-f]+", fingerprint):
        raise ValidationError(f"{field}.pair_transcript_fingerprint is invalid")

    _policy_contacts(
        value.get("policy_contract"),
        arm=execution["arm"],
        threshold=threshold,
        field=f"{field}.policy_contract",
    )
    mechanism = value.get("mechanism")
    counter_names = (
        "query_count",
        "collision_api_calls",
        "active_collision_pairs",
        "colliding_geometry_pairs",
        "colliding_logical_pairs",
        "distance_api_calls",
        "distance_geometry_pairs",
        "distance_logical_pairs",
        "fk_placement_invocations",
        "undeclared_colliding_logical_pairs",
        "distance_mode_mask",
        "collision_completion_ordinal_failures",
    )
    if not isinstance(mechanism, dict) or set(mechanism) != set(counter_names):
        raise ValidationError(f"{field}.mechanism counter set changed")
    counters = {
        name: _nonnegative_int(mechanism.get(name), f"{field}.mechanism.{name}")
        for name in counter_names
    }
    if expected_measurement == "decision":
        if any(counters.values()):
            raise ValidationError(f"{field}: decision binary contains observation counters")
    else:
        if (
            counters["query_count"] != expanded_waypoints
            or counters["collision_api_calls"] != expanded_waypoints
            or counters["collision_completion_ordinal_failures"] != 0
            or counters["undeclared_colliding_logical_pairs"] != 0
        ):
            raise ValidationError(f"{field}: collision enumeration is incomplete")
        if execution["role"] == "baseline_observation":
            if counters["distance_mode_mask"] != 8 or counters["distance_api_calls"] == 0:
                raise ValidationError(f"{field}: baseline did not measure all active pairs")
        elif execution["arm"] == "binary_contact":
            if (
                counters["distance_mode_mask"] != 1
                or counters["distance_api_calls"] != 0
                or counters["distance_geometry_pairs"] != 0
                or counters["distance_logical_pairs"] != 0
            ):
                raise ValidationError(f"{field}: binary mode performed distance work")
        else:
            if (
                counters["distance_mode_mask"] != 2
                or counters["colliding_logical_pairs"] == 0
                or counters["distance_api_calls"] == 0
                or counters["distance_logical_pairs"]
                > counters["colliding_logical_pairs"]
                or counters["distance_api_calls"] > counters["collision_api_calls"]
            ):
                raise ValidationError(
                    f"{field}: metric mode measured outside colliding configured pairs"
                )
    return {
        "decision": decision,
        "replay": replay,
        "physical_pair_transcript": transcript,
        "pair_transcript_fingerprint": fingerprint,
        "mechanism": counters,
        "policy_contract_sha256": _canonical_sha256(value["policy_contract"]),
    }


def _verify_mechanism_replay_manifest(
    path: Path,
    expected_sha256: Any,
    *,
    rows: list[dict[str, Any]],
    candidate_identity: dict[str, Any],
    candidate_specs_contract_identity: dict[str, Any],
    runtime_profiles: dict[str, dict[str, Any]],
    threshold: float,
    tracker: _FileTracker,
) -> dict[str, Any]:
    expected_digest = _require_sha256(
        expected_sha256, "mechanism_replay_manifest_sha256"
    )
    payload, digest = tracker.read(path, "live mechanism replay manifest")
    if digest != expected_digest:
        raise ValidationError("mechanism replay manifest SHA-256 changed")
    manifest = _json_payload(payload, "live mechanism replay manifest")
    if (
        manifest.get("schema_version") != MECHANISM_REPLAY_MANIFEST_SCHEMA
        or manifest.get("scope")
        != "exact_ur10e_isaac_live_validation_mechanism_owner"
    ):
        raise ValidationError("mechanism replay manifest scope is invalid")
    if manifest.get("candidate_source_identity") != candidate_identity:
        raise ValidationError("mechanism replay candidate source changed")
    if manifest.get("candidate_specs_contract_identity") != candidate_specs_contract_identity:
        raise ValidationError("mechanism replay Specs contract changed")
    if _finite_nonnegative(
        manifest.get("bounded_penetration_m"),
        "mechanism_replay_manifest.bounded_penetration_m",
    ) != threshold:
        raise ValidationError("mechanism replay bounded threshold changed")

    runner_path = REPO_ROOT / "scripts/qualification/run_step18_live_mechanism_replay.py"
    runner = manifest.get("runner")
    if not isinstance(runner, dict) or set(runner) != {"path", "sha256"}:
        raise ValidationError("mechanism replay runner binding is invalid")
    _, runner_digest = tracker.read(runner_path, "live mechanism replay runner")
    if (
        Path(str(runner.get("path") or "")).resolve() != runner_path.resolve()
        or runner.get("sha256") != runner_digest
    ):
        raise ValidationError("mechanism replay runner substitution detected")

    image = manifest.get("dev_image")
    if (
        not isinstance(image, dict)
        or not isinstance(image.get("image_id"), str)
        or not image["image_id"].startswith("sha256:")
        or _SHA256.fullmatch(image["image_id"][7:]) is None
        or not isinstance(image.get("reference"), str)
        or not image["reference"]
    ):
        raise ValidationError("mechanism replay image identity is invalid")

    candidate_binaries = manifest.get("candidate_binaries")
    if not isinstance(candidate_binaries, dict) or set(candidate_binaries) != {
        "timing",
        "observation",
    }:
        raise ValidationError("mechanism replay candidate binary matrix is invalid")
    for name, binding in candidate_binaries.items():
        if not isinstance(binding, dict) or set(binding) != {"path", "sha256"}:
            raise ValidationError(
                f"mechanism replay candidate {name} binary binding is invalid"
            )
        binary_path = Path(str(binding.get("path") or "")).resolve()
        _, binary_digest = tracker.read(
            binary_path, f"mechanism replay candidate {name} binary"
        )
        if binary_digest != binding.get("sha256"):
            raise ValidationError(
                f"mechanism replay candidate {name} binary changed"
            )

    build_commands = manifest.get("candidate_build_commands_argv")
    if (
        not isinstance(build_commands, list)
        or len(build_commands) != 2
        or any(
            not isinstance(command, list)
            or not command
            or any(not isinstance(argument, str) for argument in command)
            for command in build_commands
        )
    ):
        raise ValidationError("mechanism replay candidate build argv is invalid")

    baseline_metadata_binding = manifest.get("baseline_lane_metadata")
    if (
        not isinstance(baseline_metadata_binding, dict)
        or set(baseline_metadata_binding) != {"path", "sha256"}
    ):
        raise ValidationError("mechanism replay baseline lane binding is invalid")
    baseline_metadata_path = Path(
        str(baseline_metadata_binding.get("path") or "")
    ).resolve()
    baseline_payload, baseline_digest = tracker.read(
        baseline_metadata_path, "mechanism replay baseline lane metadata"
    )
    if baseline_digest != baseline_metadata_binding.get("sha256"):
        raise ValidationError("mechanism replay baseline lane metadata changed")
    baseline_metadata = _json_payload(
        baseline_payload, "mechanism replay baseline lane metadata"
    )
    if baseline_metadata.get("schema_version") != "manyforge.runtime_collision_baseline_lane.v2":
        raise ValidationError("mechanism replay baseline lane schema changed")
    if (
        manifest.get("baseline_timing_source_identity")
        != baseline_metadata.get("timing_source_identity")
        or manifest.get("baseline_observation_source_identity")
        != baseline_metadata.get("observation_source_identity")
    ):
        raise ValidationError("mechanism replay baseline source identity changed")
    for name in ("timing_source_identity", "observation_source_identity"):
        identity = baseline_metadata.get(name)
        if (
            not isinstance(identity, dict)
            or identity.get("head_commit") != PINNED_BASELINE_COMMIT
            or identity.get("dirty") is not True
        ):
            raise ValidationError(f"mechanism replay baseline {name} is not pinned")

    observation_rows = {
        row["id"]: row for row in rows if row["row_kind"] in OBSERVATION_ROW_KINDS
    }
    if sorted(manifest.get("observation_row_ids") or []) != sorted(observation_rows):
        raise ValidationError("mechanism replay does not own every observation row")
    records = manifest.get("records")
    if not isinstance(records, list) or len(records) != len(observation_rows):
        raise ValidationError("mechanism replay record matrix is incomplete")

    retained_artifacts: list[dict[str, str]] = [
        tracker.binding(path),
        tracker.binding(runner_path),
        tracker.binding(baseline_metadata_path),
        tracker.binding(Path(candidate_binaries["timing"]["path"])),
        tracker.binding(Path(candidate_binaries["observation"]["path"])),
    ]
    summaries: list[dict[str, Any]] = []
    for record in records:
        if not isinstance(record, dict):
            raise ValidationError("mechanism replay record must be an object")
        row_id = record.get("observation_row_id")
        row = observation_rows.get(row_id)
        if row is None:
            raise ValidationError("mechanism replay record owns an unknown row")
        if (
            record.get("row_uuid") != row["row_uuid"]
            or record.get("row_kind") != row["row_kind"]
            or record.get("mode") != row["mode"]
            or record.get("viewer_stratum") != row["viewer_stratum"]
        ):
            raise ValidationError(f"{row_id}: mechanism replay row labels changed")
        for name, row_binding_name in (
            ("row_metadata", "run_metadata"),
            ("row_evidence", "evidence"),
        ):
            binding = record.get(name)
            if binding != row[row_binding_name]:
                raise ValidationError(f"{row_id}: mechanism replay {name} changed")

        capture_binding = record.get("capture")
        if not isinstance(capture_binding, dict) or set(capture_binding) != {"path", "sha256"}:
            raise ValidationError(f"{row_id}: mechanism capture binding is invalid")
        capture_path = Path(str(capture_binding.get("path") or "")).resolve()
        capture_payload, capture_digest = tracker.read(
            capture_path, f"{row_id} mechanism capture"
        )
        if capture_digest != capture_binding.get("sha256"):
            raise ValidationError(f"{row_id}: mechanism capture changed")
        capture = _json_payload(capture_payload, f"{row_id} mechanism capture")
        evidence_payload, _ = tracker.read(
            Path(row["evidence"]["path"]), f"{row_id} live evidence"
        )
        evidence = _json_payload(evidence_payload, f"{row_id} live evidence")
        details = evidence["records"][0]["details"]
        if capture != details.get("mechanism_replay_input"):
            raise ValidationError(f"{row_id}: replay input differs from live evidence")
        identity = _mechanism_capture_identity(
            capture, details=details, field=f"{row_id}.mechanism_capture"
        )
        if record.get("capture_identity") != identity or row["mechanism_replay_input"] != identity:
            raise ValidationError(f"{row_id}: mechanism capture identity changed")
        retained_artifacts.append(tracker.binding(capture_path))

        expected_roles = (
            {"baseline_decision", "baseline_observation"}
            if row["row_kind"] == "baseline_observation"
            else {
                "baseline_decision",
                "baseline_observation",
                "candidate_decision",
                "candidate_observation",
            }
        )
        executions = record.get("executions")
        if not isinstance(executions, list):
            raise ValidationError(f"{row_id}: replay executions are missing")
        by_role = {
            item.get("role"): item for item in executions if isinstance(item, dict)
        }
        if set(by_role) != expected_roles or len(by_role) != len(executions):
            raise ValidationError(f"{row_id}: replay execution roles are incomplete")
        parsed_results: dict[str, dict[str, Any]] = {}
        for role, execution in by_role.items():
            expected_binary_role = {
                "baseline_decision": "baseline_timing",
                "baseline_observation": "baseline_observation",
                "candidate_decision": "candidate_timing",
                "candidate_observation": "candidate_observation",
            }[role]
            expected_arm = (
                "signed_penetration_selective"
                if role.startswith("baseline_")
                else {
                    "signed_penetration": "signed_penetration_selective",
                    "binary_contact": "binary_contact",
                    "bounded_penetration": "bounded_penetration",
                }[row["mode"]]
            )
            expected_source_role = {
                "baseline_decision": "baseline_pristine",
                "baseline_observation": "baseline_observation",
                "candidate_decision": "current_candidate",
                "candidate_observation": "current_candidate",
            }[role]
            if (
                execution.get("binary_role") != expected_binary_role
                or execution.get("arm") != expected_arm
                or execution.get("source_role") != expected_source_role
                or execution.get("run_id") != f"{row['row_uuid']}--{role}"
            ):
                raise ValidationError(f"{row_id}/{role}: replay declaration changed")
            binary = execution.get("binary")
            artifact = execution.get("artifact")
            if (
                not isinstance(binary, dict)
                or set(binary) != {"path", "sha256"}
                or not isinstance(artifact, dict)
                or set(artifact) != {"path", "sha256"}
            ):
                raise ValidationError(f"{row_id}/{role}: artifact binding is invalid")
            binary_path = Path(str(binary.get("path") or "")).resolve()
            artifact_path = Path(str(artifact.get("path") or "")).resolve()
            _, binary_digest = tracker.read(binary_path, f"{row_id}/{role} binary")
            artifact_payload, artifact_digest = tracker.read(
                artifact_path, f"{row_id}/{role} replay artifact"
            )
            if binary_digest != binary.get("sha256") or artifact_digest != artifact.get("sha256"):
                raise ValidationError(f"{row_id}/{role}: retained artifact changed")
            expected_binary_hash = (
                baseline_metadata[
                    "timing_binary_sha256"
                    if expected_binary_role == "baseline_timing"
                    else "observation_binary_sha256"
                ]
                if expected_binary_role.startswith("baseline_")
                else manifest["candidate_binaries"][
                    "timing" if expected_binary_role == "candidate_timing" else "observation"
                ]["sha256"]
            )
            if binary_digest != expected_binary_hash:
                raise ValidationError(f"{row_id}/{role}: replay binary provenance changed")
            if expected_binary_role.startswith("candidate_"):
                candidate_binary_name = (
                    "timing"
                    if expected_binary_role == "candidate_timing"
                    else "observation"
                )
                if binary != candidate_binaries[candidate_binary_name]:
                    raise ValidationError(
                        f"{row_id}/{role}: candidate replay binary binding changed"
                    )
            command = execution.get("command_argv")
            if not isinstance(command, list) or any(not isinstance(item, str) for item in command):
                raise ValidationError(f"{row_id}/{role}: replay argv is invalid")
            capture_container_path = execution.get("capture_container_path")
            if (
                not isinstance(capture_container_path, str)
                or not capture_container_path.startswith("/workspace/")
                or Path(capture_container_path).name != capture_path.name
            ):
                raise ValidationError(
                    f"{row_id}/{role}: replay capture container path changed"
                )
            required_argv = {
                "--mechanism-replay-input": capture_container_path,
                "--repo-root": "/workspace",
                "--run-id": execution["run_id"],
                "--source-role": expected_source_role,
                "--arm": expected_arm,
                "--bounded-penetration-m": str(threshold),
            }
            for option, expected_value in required_argv.items():
                if _command_value(command, option) != expected_value:
                    raise ValidationError(f"{row_id}/{role}: replay argv {option} changed")
            value = _json_payload(artifact_payload, f"{row_id}/{role} replay artifact")
            parsed = _validate_replay_artifact(
                value,
                execution={
                    "role": role,
                    "run_id": execution["run_id"],
                    "arm": expected_arm,
                    "source_role": expected_source_role,
                },
                capture_identity=identity,
                threshold=threshold,
                field=f"{row_id}/{role}",
            )
            summary = execution.get("result_summary")
            if (
                not isinstance(summary, dict)
                or summary.get("measurement_kind") != value.get("measurement_kind")
                or summary.get("pair_transcript_fingerprint")
                != value.get("pair_transcript_fingerprint")
            ):
                raise ValidationError(f"{row_id}/{role}: replay summary changed")
            parsed_results[role] = parsed
            retained_artifacts.extend(
                [tracker.binding(binary_path), tracker.binding(artifact_path)]
            )

        baseline_decision = parsed_results["baseline_decision"]
        baseline_observation = parsed_results["baseline_observation"]
        if (
            baseline_decision["physical_pair_transcript"]
            != baseline_observation["physical_pair_transcript"]
            or baseline_decision["decision"] != baseline_observation["decision"]
        ):
            raise ValidationError(f"{row_id}: baseline observation perturbed replay")
        if row["row_kind"] == "candidate_observation":
            candidate_decision = parsed_results["candidate_decision"]
            candidate_observation = parsed_results["candidate_observation"]
            if (
                candidate_decision["physical_pair_transcript"]
                != candidate_observation["physical_pair_transcript"]
                or candidate_decision["decision"] != candidate_observation["decision"]
            ):
                raise ValidationError(f"{row_id}: candidate observation perturbed replay")
            if (
                baseline_decision["physical_pair_transcript"]
                != candidate_decision["physical_pair_transcript"]
            ):
                raise ValidationError(f"{row_id}: baseline/candidate collision pairs differ")
        summaries.append(
            {
                "observation_row_id": row_id,
                "capture_identity": identity,
                "execution_artifacts": {
                    role: by_role[role]["artifact"] for role in sorted(by_role)
                },
                "direct_mechanism": {
                    role: parsed_results[role]["mechanism"]
                    for role in sorted(parsed_results)
                    if role.endswith("observation")
                },
                "physical_pair_parity_passed": True,
                "observation_nonperturbation_passed": True,
            }
        )

    if len({summary["capture_identity"]["geometry_sha256"] for summary in summaries}) != 1:
        raise ValidationError("mechanism replay rows do not share exact live geometry")
    if len({summary["capture_identity"]["trajectory_sha256"] for summary in summaries}) != 1:
        raise ValidationError("mechanism replay rows do not share one trajectory")
    return {
        "manifest": tracker.binding(path),
        "runner": tracker.binding(runner_path),
        "baseline_lane_metadata": tracker.binding(baseline_metadata_path),
        "owner_scope": "exact_ur10e_isaac_live_validation_mechanism_owner",
        "observation_row_count": len(summaries),
        "records": summaries,
        "retained_artifacts": retained_artifacts,
        "all_direct_mechanism_checks_passed": True,
        "all_physical_pair_parity_checks_passed": True,
    }


def verify(
    manifest_path: Path, *, output_path: Path | None = None
) -> dict[str, Any]:
    manifest_path = manifest_path.resolve()
    tracker = _FileTracker()
    manifest_payload, manifest_digest = tracker.read(
        manifest_path, "Step 18 live manifest"
    )
    manifest = _json_payload(manifest_payload, "Step 18 live manifest")
    if manifest.get("schema_version") != MANIFEST_SCHEMA:
        raise ValidationError(f"manifest schema_version must be {MANIFEST_SCHEMA!r}")
    if manifest.get("expected_baseline_commit") != PINNED_BASELINE_COMMIT:
        raise ValidationError("expected_baseline_commit is not pinned 2bfe30e")
    if manifest.get("expected_specs_commit") != PINNED_SPECS_COMMIT:
        raise ValidationError("expected_specs_commit is not pinned 67c2444")

    candidate_root_raw = manifest.get("current_candidate_root")
    specs_root_raw = manifest.get("current_specs_root")
    if not isinstance(candidate_root_raw, str) or not candidate_root_raw:
        raise ValidationError("current_candidate_root must be an absolute path")
    if not isinstance(specs_root_raw, str) or not specs_root_raw:
        raise ValidationError("current_specs_root must be an absolute path")
    candidate_root = Path(candidate_root_raw).resolve()
    specs_root = Path(specs_root_raw).resolve()
    if not Path(candidate_root_raw).is_absolute() or not Path(specs_root_raw).is_absolute():
        raise ValidationError("current candidate/Specs roots must be absolute")
    if candidate_root != REPO_ROOT.resolve():
        raise ValidationError("current_candidate_root is not the verifier checkout")
    if output_path is not None:
        ensure_ignored_qualification_output(output_path, candidate_root)

    verifier_raw = manifest.get("expected_verifier")
    if not isinstance(verifier_raw, dict) or set(verifier_raw) != {"path", "sha256"}:
        raise ValidationError("expected_verifier must be an exact path/SHA-256 binding")
    canonical_verifier = (candidate_root / _VERIFIER_RELATIVE_PATH).resolve()
    running_verifier = Path(__file__).resolve()
    if (
        Path(str(verifier_raw.get("path") or "")).resolve() != canonical_verifier
        or running_verifier != canonical_verifier
    ):
        raise ValidationError("live verifier path substitution detected")
    expected_verifier_sha = _require_sha256(
        verifier_raw.get("sha256"), "expected_verifier.sha256"
    )
    _, verifier_digest = tracker.read(running_verifier, "Step 18 live verifier")
    if verifier_digest != expected_verifier_sha:
        raise ValidationError("live verifier hash substitution detected")

    pristine_identity = _source_identity(
        head_commit=PINNED_BASELINE_COMMIT,
        head_tree=PINNED_BASELINE_TREE,
        dirty=False,
        delta_sha256=_delta_sha256(b""),
    )
    patch_path = _resolve_path(manifest_path, manifest.get("baseline_observation_patch"), "baseline_observation_patch")
    patch_payload, patch_digest = tracker.read(
        patch_path, "baseline observation patch"
    )
    observation_identity = _observation_patch_identity(
        patch_payload,
        patch_digest,
        manifest.get("baseline_observation_patch_sha256"),
    )
    candidate_identity = _validated_source_identity(
        manifest.get("expected_candidate_source_identity"),
        "expected_candidate_source_identity",
    )
    if (
        candidate_identity["head_commit"] != PINNED_BASELINE_COMMIT
        or candidate_identity["head_tree"] != PINNED_BASELINE_TREE
        or candidate_identity["dirty"] is not True
    ):
        raise ValidationError("candidate source is not the frozen 2bfe30e-based delta")
    baseline_specs_identity = _validated_source_identity(
        manifest.get("expected_baseline_specs_source_identity"),
        "expected_baseline_specs_source_identity",
    )
    candidate_specs_identity = _validated_source_identity(
        manifest.get("expected_candidate_specs_source_identity"),
        "expected_candidate_specs_source_identity",
    )
    clean_specs_identity = _source_identity(
        head_commit=PINNED_SPECS_COMMIT,
        head_tree=PINNED_SPECS_TREE,
        dirty=False,
        delta_sha256=_delta_sha256(b""),
    )
    if baseline_specs_identity != clean_specs_identity:
        raise ValidationError("baseline Specs identity is not pristine pinned 67c2444")
    if (
        candidate_specs_identity["head_commit"] != PINNED_SPECS_COMMIT
        or candidate_specs_identity["head_tree"] != PINNED_SPECS_TREE
        or candidate_specs_identity["dirty"] is not True
        or candidate_specs_identity["delta_sha256"] == _delta_sha256(b"")
    ):
        raise ValidationError("candidate Specs identity is not the frozen 67c2444 delta")
    baseline_specs_contract_identity = _validated_specs_contract_identity(
        manifest.get("expected_baseline_specs_contract_identity"),
        "expected_baseline_specs_contract_identity",
    )
    candidate_specs_contract_identity = _validated_specs_contract_identity(
        manifest.get("expected_candidate_specs_contract_identity"),
        "expected_candidate_specs_contract_identity",
    )
    clean_specs_contract_identity = _specs_contract_identity(
        head_commit=PINNED_SPECS_COMMIT,
        head_tree=PINNED_SPECS_TREE,
        contract_dirty=False,
        contract_delta_sha256=_delta_sha256(b""),
    )
    if baseline_specs_contract_identity != clean_specs_contract_identity:
        raise ValidationError(
            "baseline Specs contract identity is not pristine pinned 67c2444"
        )
    if (
        candidate_specs_contract_identity["head_commit"] != PINNED_SPECS_COMMIT
        or candidate_specs_contract_identity["head_tree"] != PINNED_SPECS_TREE
        or candidate_specs_contract_identity["contract_dirty"] is not True
        or candidate_specs_contract_identity["contract_delta_sha256"]
        == _delta_sha256(b"")
    ):
        raise ValidationError(
            "candidate Specs contract identity is not the frozen Step 18 contract"
        )
    try:
        initial_candidate_identity = current_source_identity(candidate_root)
        initial_specs_contract_identity = current_specs_contract_identity(specs_root)
    except SourceIdentityError as exc:
        raise ValidationError(f"current source identity cannot be read: {exc}") from exc
    if initial_candidate_identity != candidate_identity:
        raise ValidationError(
            "current ManyForge candidate differs from the live campaign identity"
        )
    if initial_specs_contract_identity != candidate_specs_contract_identity:
        raise ValidationError(
            "current Specs contract differs from the live campaign identity"
        )

    expected_runner_sha = _require_sha256(manifest.get("expected_host_runner_sha256"), "expected_host_runner_sha256")
    expected_tool_sha = _require_sha256(manifest.get("expected_qualification_tool_bundle_fingerprint_sha256"), "expected_qualification_tool_bundle_fingerprint_sha256")
    expected_thor_host_identity = _thor_host_identity(
        manifest.get("expected_thor_host_identity"),
        "expected_thor_host_identity",
    )
    threshold = _finite_nonnegative(manifest.get("bounded_penetration_m"), "bounded_penetration_m")
    maximum_ratio = _finite_nonnegative(manifest.get("maximum_controller_progress_regression_ratio", 1.10), "maximum_controller_progress_regression_ratio")
    if maximum_ratio < 1.0:
        raise ValidationError("maximum controller-progress ratio must be at least 1")
    profiles_raw = manifest.get("runtime_profiles")
    if not isinstance(profiles_raw, dict) or set(profiles_raw) != set(ROW_KINDS):
        raise ValidationError("runtime_profiles must pin all four row roles")
    runtime_profiles = {
        name: _runtime_profile(profiles_raw[name], f"runtime_profiles.{name}")
        for name in ROW_KINDS
    }
    if len({profile["composer_image_id"] for profile in runtime_profiles.values()}) != 1 or len({profile["controller_image_id"] for profile in runtime_profiles.values()}) != 1:
        raise ValidationError("matched rows did not use one Composer/controller image")

    rows_raw = manifest.get("rows")
    if not isinstance(rows_raw, list) or any(not isinstance(row, dict) for row in rows_raw):
        raise ValidationError("manifest rows must be a list of objects")
    parsed = [
        _row_payload(
            manifest_path,
            row,
            tracker=tracker,
            candidate_identity=candidate_identity,
            pristine_identity=pristine_identity,
            observation_identity=observation_identity,
            baseline_specs_identity=baseline_specs_identity,
            candidate_specs_identity=candidate_specs_identity,
            baseline_specs_contract_identity=baseline_specs_contract_identity,
            candidate_specs_contract_identity=candidate_specs_contract_identity,
            runtime_profiles=runtime_profiles,
            expected_runner_sha256=expected_runner_sha,
            expected_tool_bundle_sha256=expected_tool_sha,
            expected_thor_host_identity=expected_thor_host_identity,
            bounded_threshold_m=threshold,
        )
        for row in rows_raw
    ]
    expected_rows = {
        (kind, BASELINE_MODE, stratum)
        for kind in ("baseline_pristine_timing", "baseline_observation")
        for stratum in VIEWER_STRATA
    } | {
        (kind, mode, stratum)
        for kind in ("candidate_timing", "candidate_observation")
        for mode in CANDIDATE_MODES
        for stratum in VIEWER_STRATA
    }
    actual_rows = {(row["row_kind"], row["mode"], row["viewer_stratum"]) for row in parsed}
    if len(actual_rows) != len(parsed) or actual_rows != expected_rows:
        raise ValidationError(
            "manifest must contain each of the sixteen matched rows exactly once"
        )

    parity_path = _resolve_path(manifest_path, manifest.get("baseline_observation_parity"), "baseline_observation_parity")
    parity = _verify_observation_parity(
        parity_path,
        manifest.get("baseline_observation_parity_sha256"),
        parsed,
        pristine_identity,
        observation_identity,
        tracker,
    )
    mechanism_path = _resolve_path(
        manifest_path,
        manifest.get("mechanism_replay_manifest"),
        "mechanism_replay_manifest",
    )
    mechanism_replay = _verify_mechanism_replay_manifest(
        mechanism_path,
        manifest.get("mechanism_replay_manifest_sha256"),
        rows=parsed,
        candidate_identity=candidate_identity,
        candidate_specs_contract_identity=candidate_specs_contract_identity,
        runtime_profiles=runtime_profiles,
        threshold=threshold,
        tracker=tracker,
    )

    failures: list[str] = []
    if len({row["matched_input_sha256"] for row in parsed}) != 1:
        failures.append("canonical program/deployment input differs across matched rows")
    if len({row["matched_configuration_sha256"] for row in parsed}) != 1:
        failures.append("canonical runtime configuration differs across matched rows")
    if len({row["trajectory_fingerprint_sha256"] for row in parsed}) != 1:
        failures.append("trajectory/RNG fingerprint differs across matched rows")
    roboplan_identities = {
        (
            row["source"].get("roboplan_commit"),
            row["source"].get("roboplan_version"),
            row["source"].get("roboplan_patch_manifest_sha256"),
        )
        for row in parsed
    }
    if len(roboplan_identities) != 1:
        failures.append("RoboPlan dependency identity differs across rows")
    for row_kind in ROW_KINDS:
        for mode in (
            (BASELINE_MODE,)
            if row_kind not in CANDIDATE_ROW_KINDS
            else CANDIDATE_MODES
        ):
            group = [row for row in parsed if row["row_kind"] == row_kind and row["mode"] == mode]
            if len({(row["saved_scene_sha256"], row["effective_scene_sha256"]) for row in group}) != 1:
                failures.append(f"{row_kind}/{mode}: saved/effective scene differs by viewer stratum")

    for stratum in VIEWER_STRATA:
        for mode in CANDIDATE_MODES:
            timing = next(
                row
                for row in parsed
                if row["row_kind"] == "candidate_timing"
                and row["viewer_stratum"] == stratum
                and row["mode"] == mode
            )
            observation = next(
                row
                for row in parsed
                if row["row_kind"] == "candidate_observation"
                and row["viewer_stratum"] == stratum
                and row["mode"] == mode
            )
            if (
                timing["retained_live_output_sha256"]
                != observation["retained_live_output_sha256"]
                or timing["trajectory_fingerprint_sha256"]
                != observation["trajectory_fingerprint_sha256"]
            ):
                failures.append(
                    f"{mode}/{stratum}: candidate observation perturbed live output"
                )

    for row in parsed:
        if row["row_kind"] != "baseline_pristine_timing":
            for name, value in row["counter_zero_gates"].items():
                if value != 0:
                    failures.append(f"{row['id']}: {name} advanced by {value}")
        deadline = row["controller_deadline_log"]
        if deadline["bytes"] == 0:
            failures.append(f"{row['id']}: controller log is empty")
        if not deadline["all_events_parsed"]:
            failures.append(f"{row['id']}: controller overrun log was not fully parsed")
        if deadline["overrun_events"] != 0 or deadline["missed_cycles"] != 0:
            failures.append(
                f"{row['id']}: controller deadline misses={deadline['missed_cycles']} "
                f"in {deadline['overrun_events']} events"
            )

    comparisons: list[dict[str, Any]] = []
    for stratum in VIEWER_STRATA:
        baseline = next(row for row in parsed if row["row_kind"] == "baseline_pristine_timing" and row["viewer_stratum"] == stratum)
        for mode in CANDIDATE_MODES:
            candidate = next(row for row in parsed if row["row_kind"] == "candidate_timing" and row["viewer_stratum"] == stratum and row["mode"] == mode)
            if baseline["controller_progress_p95_ns"] <= 0.0 or baseline["controller_progress_max_ns"] <= 0.0:
                raise ValidationError(f"pristine baseline {stratum} controller latency is zero")
            p95_ratio = candidate["controller_progress_p95_ns"] / baseline["controller_progress_p95_ns"]
            max_ratio = candidate["controller_progress_max_ns"] / baseline["controller_progress_max_ns"]
            passed = p95_ratio <= maximum_ratio and max_ratio <= maximum_ratio
            comparisons.append(
                {
                    "viewer_stratum": stratum,
                    "mode": mode,
                    "timing_denominator_row_id": baseline["id"],
                    "timing_denominator_source": "pristine_2bfe30e_unpatched",
                    "p95_ratio": p95_ratio,
                    "max_ratio": max_ratio,
                    "maximum_allowed_ratio": maximum_ratio,
                    "passed": passed,
                }
            )
            if not passed:
                failures.append(
                    f"{mode}/{stratum}: controller progress ratio "
                    f"p95={p95_ratio:.6f}, max={max_ratio:.6f}"
                )

    verifier_binding = tracker.binding(running_verifier)
    manifest_binding = tracker.binding(manifest_path)
    artifact_manifest: list[dict[str, Any]] = [
        {"role": "live_manifest", **manifest_binding},
        {"role": "live_verifier", **verifier_binding},
        {
            "role": "baseline_observation_patch",
            **tracker.binding(patch_path),
        },
        {
            "role": "baseline_observation_parity",
            **tracker.binding(parity_path),
        },
        {
            "role": "live_row_runner",
            **tracker.binding(
                REPO_ROOT / "scripts/qualification/run_step18_live_contact_mode.py"
            ),
        },
    ]
    for row in parsed:
        for field, role in (
            ("evidence", "live_row_evidence"),
            ("controller_log", "live_row_controller_log"),
            ("run_metadata", "live_row_metadata"),
        ):
            artifact_manifest.append(
                {"role": role, "row_id": row["id"], **row[field]}
            )
    existing_artifact_paths = {
        Path(item["path"]).resolve() for item in artifact_manifest
    }
    for binding in mechanism_replay["retained_artifacts"]:
        path = Path(binding["path"]).resolve()
        if path in existing_artifact_paths:
            continue
        artifact_manifest.append(
            {"role": "live_mechanism_replay_input", **binding}
        )
        existing_artifact_paths.add(path)
    declared_paths = {Path(item["path"]).resolve() for item in artifact_manifest}
    if len(declared_paths) != len(artifact_manifest) or declared_paths != tracker.paths:
        raise ValidationError("live artifact manifest does not exactly cover retained inputs")

    producer_invocation = {
        "schema_version": _INVOCATION_SCHEMA,
        "verifier": verifier_binding,
        "arguments": ["--manifest", str(manifest_path)],
        "inputs": [{"role": "manifest", **manifest_binding}],
        "result_transport": "output_json",
    }
    # Evidence and run metadata remain available through their exact retained
    # file bindings.  Do not duplicate their container-only semantic paths in
    # the host closure summary: a `/workspace/...` program identity or a
    # `/tmp/manyforge-build/...` loaded-object identity is content provenance,
    # not a host file the closure process should try to open.  The compact row
    # projection retains every value used by this verifier and one path-free
    # digest of the common RoboPlan dependency identity.
    summary_rows: list[dict[str, Any]] = []
    for row in parsed:
        projected = {
            key: value
            for key, value in row.items()
            if key not in {"source", "environment", "metadata"}
        }
        projected["roboplan_dependency_identity_sha256"] = _canonical_sha256(
            {
                "commit": row["source"].get("roboplan_commit"),
                "version": row["source"].get("roboplan_version"),
                "patch_manifest_sha256": row["source"].get(
                    "roboplan_patch_manifest_sha256"
                ),
            }
        )
        summary_rows.append(projected)

    # Final campaign binding: every retained byte, the current candidate, and
    # the Specs contract are re-observed only after all semantic checks finish.
    tracker.assert_unchanged()
    try:
        final_candidate_identity = current_source_identity(candidate_root)
        final_specs_contract_identity = current_specs_contract_identity(specs_root)
    except SourceIdentityError as exc:
        raise ValidationError(f"final source identity cannot be read: {exc}") from exc
    if final_candidate_identity != candidate_identity:
        raise ValidationError("current ManyForge candidate changed during verification")
    if final_specs_contract_identity != candidate_specs_contract_identity:
        raise ValidationError("current Specs contract changed during verification")

    return {
        "schema_version": SCHEMA,
        "scope": "step18_live_validation_nonregression_subverification",
        "overall_step18_closure_claimed": False,
        "generated_at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "manifest": manifest_binding,
        "verifier": verifier_binding,
        "producer_invocation": producer_invocation,
        "artifact_manifest": artifact_manifest,
        "current_candidate_root": str(candidate_root),
        "current_specs_root": str(specs_root),
        "pinned_baseline_commit": PINNED_BASELINE_COMMIT,
        "pinned_specs_commit": PINNED_SPECS_COMMIT,
        "pristine_source_identity": pristine_identity,
        "observation_source_identity": observation_identity,
        "candidate_source_identity": candidate_identity,
        "baseline_specs_source_identity": baseline_specs_identity,
        "candidate_specs_source_identity": candidate_specs_identity,
        "baseline_specs_contract_identity": baseline_specs_contract_identity,
        "candidate_specs_contract_identity": candidate_specs_contract_identity,
        "thor_host_identity": expected_thor_host_identity,
        "baseline_observation_patch": tracker.binding(patch_path),
        "baseline_observation_parity": parity,
        "live_mechanism_replay": mechanism_replay,
        "bounded_penetration_m": threshold,
        "rows": summary_rows,
        "comparisons": comparisons,
        "failures": failures,
        "live_lane_passed": not failures,
    }


def _assert_live_result_bindings_current(result: dict[str, Any]) -> None:
    artifacts = result.get("artifact_manifest")
    # Five live-control inputs, three files for each of sixteen rows, and
    # forty-three unique exact-replay inputs/artifacts.
    if not isinstance(artifacts, list) or len(artifacts) != 96:
        raise ValidationError("published live artifact manifest is not exact")
    paths: list[Path] = []
    for index, item in enumerate(artifacts):
        if not isinstance(item, dict):
            raise ValidationError(f"published live artifact {index} is malformed")
        raw_path = item.get("path")
        expected = _require_sha256(
            item.get("sha256"), f"published live artifact {index}.sha256"
        )
        if not isinstance(raw_path, str) or not raw_path:
            raise ValidationError(f"published live artifact {index} has no path")
        path = Path(raw_path).resolve()
        try:
            observed = hashlib.sha256(path.read_bytes()).hexdigest()
        except OSError as exc:
            raise ValidationError(
                f"published live artifact disappeared: {path}: {exc}"
            ) from exc
        if observed != expected:
            raise ValidationError(f"published live artifact changed: {path}")
        paths.append(path)
    if len(set(paths)) != len(paths):
        raise ValidationError("published live artifact paths are not globally unique")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    temporary: Path | None = None
    try:
        result = verify(args.manifest, output_path=args.output)
        output = ensure_ignored_qualification_output(
            args.output, Path(result["current_candidate_root"])
        )
        if output.exists():
            raise ValidationError(f"refusing to overwrite live summary: {output}")
        output.parent.mkdir(parents=True, exist_ok=True)
        temporary = output.with_name(f".{output.name}.tmp-{os.getpid()}")
        if temporary.exists():
            raise ValidationError(
                f"live summary temporary output already exists: {temporary}"
            )
        with temporary.open("x", encoding="utf-8") as stream:
            json.dump(result, stream, indent=2, sort_keys=True, allow_nan=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        _assert_live_result_bindings_current(result)
        if current_source_identity(
            Path(result["current_candidate_root"])
        ) != result["candidate_source_identity"]:
            raise ValidationError("candidate source changed before live summary publication")
        publication_specs = current_source_identity(
            Path(result["current_specs_root"])
        )
        campaign_specs = result["candidate_specs_source_identity"]
        if (
            publication_specs["head_commit"] != campaign_specs["head_commit"]
            or publication_specs["head_tree"] != campaign_specs["head_tree"]
        ):
            raise ValidationError(
                "full Specs Git base changed before live summary publication"
            )
        if current_specs_contract_identity(
            Path(result["current_specs_root"])
        ) != result["candidate_specs_contract_identity"]:
            raise ValidationError("Specs contract changed before live summary publication")
        os.link(temporary, output)
        try:
            temporary.unlink()
        except OSError:
            pass
        temporary = None
    except (OSError, ValidationError, ValueError, subprocess.CalledProcessError) as exc:
        if temporary is not None:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass
        print(f"Step 18 live verification failed: {exc}", file=sys.stderr)
        return 2
    if not result["live_lane_passed"]:
        for failure in result["failures"]:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print(json.dumps({"live_lane_passed": True, "rows": len(result["rows"])}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
