#!/usr/bin/env python3
"""Strict offline verifier for Step 18 scheduled-runtime JSON artifacts.

The default gate requires three pristine-timing and three observation records
from the genuine 2bfe30e baseline plus three timing and three observation
records for every updated arm. ``--allow-missing-baseline`` is a development
aid: it verifies current-source invariants but never reports the matched
before/after gate as closed.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
import os
import statistics
import subprocess
import sys
import tempfile
import uuid
from collections import defaultdict
from pathlib import Path
from typing import Any, Iterable


SCHEMA = "manyforge.runtime_collision_schedule_qualification.v2"
SUMMARY_SCHEMA = "manyforge.runtime_collision_schedule_verification.v2"
CLEAN_CHECKOUT_SCHEMA = (
    "manyforge.runtime_collision_baseline_clean_checkout.v2"
)
BASELINE_PARITY_SCHEMA = (
    "manyforge.runtime_collision_baseline_decision_parity.v2"
)
BASELINE_ARM = "current_all_active_distance"
BASELINE_SOURCE = "2bfe30eeb37952f1ddc36bfdbf89d471a9f6b909"
BASELINE_SPECS = "67c24447a117e14c8274b13d05d720766671aa7a"
UPDATED_ARMS = (
    "signed_penetration_selective",
    "binary_contact",
    "bounded_penetration",
)
MEASUREMENT_KINDS = ("timing", "observation")
EXPECTED_DISTANCE_MASK = {
    BASELINE_ARM: 1 << 3,  # all_active_pairs
    "signed_penetration_selective": 1 << 1,
    "binary_contact": 1 << 0,
    "bounded_penetration": 1 << 1,
}
CPU_REDUCTION_GATES = {
    "signed_penetration_selective": 0.20,
    "binary_contact": 0.40,
    "bounded_penetration": 0.20,
}
EXTERNAL_NULL_FIELDS = (
    "controller_progress_latency",
    "controller_deadline_misses",
    "focused_browser_viewer",
    "isaac_ros_process_tree_cpu",
    "mesh_heavy_ur10_drop_approach",
    "projection_retry_exhaustion",
)
FORMAL_MINIMUM_RUNS = 3
FORMAL_PROFILE = {
    "monitor_period_ns": 50_000_000,
    "immediate_query_budget_ns": 50_000_000,
    "lookahead_cycle_budget_ns": 200_000_000,
    "lookahead_horizon_ns": 2_000_000_000,
    "lookahead_query_budget_ns": 75_000_000,
    "lookahead_max_samples": 32,
    "lookahead_workers_requested": 2,
    "lookahead_workers_effective": 2,
    "warmup_ticks_requested": 40,
    "warmup_lookahead_cycles_requested": 5,
    "measured_ticks_requested": 200,
    "measured_lookahead_cycles_requested": 20,
    "max_extra_ticks": 200,
    "campaign_timeout_s": 180,
}
FORMAL_BOUNDED_THRESHOLD_M = 2.0


_SOURCE_IDENTITY_PATH = Path(__file__).with_name("source_identity.py")
_SOURCE_IDENTITY_SPEC = importlib.util.spec_from_file_location(
    "manyforge_step18_source_identity", _SOURCE_IDENTITY_PATH
)
if _SOURCE_IDENTITY_SPEC is None or _SOURCE_IDENTITY_SPEC.loader is None:
    raise RuntimeError("unable to load source identity authority")
_SOURCE_IDENTITY_MODULE = importlib.util.module_from_spec(_SOURCE_IDENTITY_SPEC)
_SOURCE_IDENTITY_SPEC.loader.exec_module(_SOURCE_IDENTITY_MODULE)


class Checks:
    def __init__(self) -> None:
        self.items: list[dict[str, Any]] = []

    def require(self, name: str, condition: bool, detail: str = "") -> None:
        self.items.append(
            {"name": name, "passed": bool(condition), "detail": detail}
        )

    @property
    def failures(self) -> list[dict[str, Any]]:
        return [item for item in self.items if not item["passed"]]


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_file_snapshot(path: Path) -> tuple[bytes, str]:
    payload = path.read_bytes()
    return payload, hashlib.sha256(payload).hexdigest()


def load_artifact(path: Path) -> list[dict[str, Any]]:
    payload, digest = read_file_snapshot(path)
    text = payload.decode("utf-8").strip()
    if not text:
        raise ValueError(f"empty artifact: {path}")
    try:
        parsed = json.loads(text)
    except json.JSONDecodeError:
        parsed = [json.loads(line) for line in text.splitlines() if line.strip()]
    records = parsed if isinstance(parsed, list) else [parsed]
    if not records or not all(isinstance(record, dict) for record in records):
        raise ValueError(f"artifact must contain JSON object records: {path}")
    for index, record in enumerate(records):
        record["_artifact"] = {
            "path": str(path),
            "sha256": digest,
            "record_index": index,
        }
    return records


def patch_paths_from_bytes(payload: bytes, path: Path) -> set[str]:
    paths: set[str] = set()
    for line in payload.decode("utf-8").splitlines():
        if not line.startswith("diff --git "):
            continue
        fields = line.split()
        if (
            len(fields) != 4
            or not fields[2].startswith("a/")
            or not fields[3].startswith("b/")
        ):
            raise ValueError(f"unparseable diff header in {path}: {line}")
        left = fields[2][2:]
        right = fields[3][2:]
        if left != right:
            raise ValueError(f"rename/copy is not allowed in {path}: {line}")
        paths.add(left)
    if not paths:
        raise ValueError(f"patch contains no diff paths: {path}")
    return paths


def require_artifacts_unchanged(
    artifact_manifest: list[dict[str, Any]],
    checks: Checks,
    label_prefix: str = "retained_input_unchanged",
) -> None:
    for index, artifact in enumerate(artifact_manifest):
        path = Path(artifact["path"])
        try:
            observed = sha256_file(path)
        except OSError as error:
            checks.require(
                f"{label_prefix}:{index}", False, f"{path}: {error}"
            )
            continue
        checks.require(
            f"{label_prefix}:{index}",
            observed == artifact["sha256"],
            f"path={path} expected={artifact['sha256']} observed={observed}",
        )


def path_within(path: Path, root: Path) -> bool:
    return path == root or root in path.parents


def git_ignored(path: Path, repository_root: Path) -> bool:
    repository_root = repository_root.resolve()
    try:
        relative = path.resolve(strict=False).relative_to(repository_root)
    except ValueError:
        return True
    result = subprocess.run(
        [
            "git",
            "-C",
            str(repository_root),
            "check-ignore",
            "-q",
            "--",
            str(relative),
        ],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return result.returncode == 0


def checkout_roots_from_records(
    records: Iterable[dict[str, Any]], field: str
) -> set[Path]:
    roots: set[Path] = set()
    for record in records:
        value = nested(record, "provenance", "runner_environment", field)
        if isinstance(value, str) and value:
            roots.add(Path(value).expanduser().resolve(strict=False))
    return roots


def require_current_candidate_checkout(
    records: list[dict[str, Any]], checks: Checks, repository_root: Path
) -> None:
    current_records = [
        record
        for record in records
        if nested(record, "provenance", "source_role") == "current_candidate"
    ]
    source_roots = checkout_roots_from_records(current_records, "source_root")
    specs_roots = checkout_roots_from_records(current_records, "specs_root")
    source_root = next(iter(source_roots)) if len(source_roots) == 1 else None
    specs_root = next(iter(specs_roots)) if len(specs_roots) == 1 else None
    checks.require(
        "current_candidate_checkout_roots",
        source_root == repository_root.resolve() and specs_root is not None,
        f"source_roots={sorted(map(str, source_roots))} "
        f"specs_roots={sorted(map(str, specs_roots))}",
    )
    if source_root is None or specs_root is None or not current_records:
        return
    expected_source = nested(
        current_records[0], "identity", "source_identity", default={}
    )
    expected_specs = nested(
        current_records[0], "identity", "specs_source_identity", default={}
    )
    expected_contract = nested(
        current_records[0], "identity", "specs_contract_identity", default={}
    )
    try:
        observed_source = _SOURCE_IDENTITY_MODULE.source_identity(source_root)
        observed_specs = _SOURCE_IDENTITY_MODULE.source_identity(specs_root)
        observed_contract = _SOURCE_IDENTITY_MODULE.specs_contract_identity(
            specs_root
        )
        detail = ""
    except (OSError, _SOURCE_IDENTITY_MODULE.SourceIdentityError) as error:
        observed_source = observed_specs = observed_contract = None
        detail = str(error)
    checks.require(
        "current_candidate_source_and_specs_contract_unchanged_at_verifier_end",
        observed_source == expected_source
        and observed_contract == expected_contract
        and isinstance(observed_specs, dict)
        and observed_specs.get("head_commit") == expected_specs.get("head_commit")
        and observed_specs.get("head_tree") == expected_specs.get("head_tree"),
        detail,
    )


def validated_evidence_output(
    output: Path,
    records: list[dict[str, Any]],
    repository_root: Path,
) -> Path:
    destination = output.expanduser().resolve(strict=False)
    if destination.exists():
        raise ValueError(f"refusing to overwrite verification summary: {destination}")
    current_records = [
        record
        for record in records
        if nested(record, "provenance", "source_role") == "current_candidate"
    ]
    current_roots = checkout_roots_from_records(current_records, "source_root")
    current_root = next(iter(current_roots)) if len(current_roots) == 1 else None
    protected = checkout_roots_from_records(records, "source_root")
    protected.update(checkout_roots_from_records(records, "specs_root"))
    for root in protected:
        if current_root is not None and root == current_root:
            continue
        if path_within(destination, root):
            raise ValueError(
                f"verification output must not be inside checkout {root}"
            )
    repository_root = repository_root.resolve()
    if path_within(destination, repository_root):
        allowed = (
            repository_root / "qualification",
            repository_root / ".manyforge-var" / "qualification",
            repository_root / "build",
        )
        if not any(path_within(destination, root) for root in allowed):
            raise ValueError(
                "verification output inside ManyForge must be below an ignored "
                "qualification or build evidence root"
            )
        if not git_ignored(destination, repository_root):
            raise ValueError(
                "verification output inside ManyForge must be actually ignored "
                "by git"
            )
    return destination


def stage_output(path: Path, payload: bytes) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.pending-", suffix=".tmp", dir=path.parent
    )
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
    except BaseException:
        Path(temporary_name).unlink(missing_ok=True)
        raise
    return Path(temporary_name)


def publish_staged_exclusive(staged: Path, destination: Path) -> None:
    try:
        os.link(staged, destination)
    except FileExistsError as error:
        raise ValueError(
            f"refusing to overwrite verification summary: {destination}"
        ) from error
    finally:
        try:
            staged.unlink(missing_ok=True)
        except OSError:
            pass


def require_manifest_snapshot(
    manifests: Iterable[dict[str, Any]], label: str
) -> None:
    for index, entry in enumerate(manifests):
        path = Path(entry["path"])
        if sha256_file(path) != entry["sha256"]:
            raise ValueError(
                f"{label} changed during summary publication: "
                f"index={index} path={path}"
            )


def load_one_probe(payload: bytes, path: Path) -> dict[str, Any]:
    parsed = json.loads(payload)
    if isinstance(parsed, list):
        if len(parsed) != 1:
            raise ValueError(f"expected one parity probe record in {path}")
        parsed = parsed[0]
    if not isinstance(parsed, dict):
        raise ValueError(f"parity probe must be one JSON object: {path}")
    return parsed


def nested(record: dict[str, Any], *path: str, default: Any = None) -> Any:
    value: Any = record
    for key in path:
        if not isinstance(value, dict) or key not in value:
            return default
        value = value[key]
    return value


def publication_checkout_snapshot(
    records: list[dict[str, Any]], repository_root: Path
) -> dict[str, Any]:
    current_records = [
        record
        for record in records
        if nested(record, "provenance", "source_role") == "current_candidate"
    ]
    source_roots = checkout_roots_from_records(current_records, "source_root")
    specs_roots = checkout_roots_from_records(current_records, "specs_root")
    result: dict[str, Any] = {
        "source_roots": sorted(map(str, source_roots)),
        "specs_roots": sorted(map(str, specs_roots)),
    }
    if len(source_roots) != 1 or len(specs_roots) != 1:
        return result
    source_root = next(iter(source_roots))
    specs_root = next(iter(specs_roots))
    try:
        result.update(
            {
                "source_identity": _SOURCE_IDENTITY_MODULE.source_identity(
                    source_root
                ),
                "specs_source_identity": _SOURCE_IDENTITY_MODULE.source_identity(
                    specs_root
                ),
                "specs_contract_identity": (
                    _SOURCE_IDENTITY_MODULE.specs_contract_identity(specs_root)
                ),
                "repository_root_matches": source_root == repository_root.resolve(),
            }
        )
    except (OSError, _SOURCE_IDENTITY_MODULE.SourceIdentityError) as error:
        result["error"] = f"{type(error).__name__}: {error}"
    return result


def publish_summary(
    destination: Path,
    payload: bytes,
    records: list[dict[str, Any]],
    repository_root: Path,
    artifact_manifest: list[dict[str, Any]],
    parity_probe_manifest: list[dict[str, Any]],
    tool_manifest: list[dict[str, Any]],
) -> None:
    checkout_before = publication_checkout_snapshot(records, repository_root)
    staged = stage_output(destination, payload)
    try:
        if hashlib.sha256(staged.read_bytes()).digest() != hashlib.sha256(
            payload
        ).digest():
            raise ValueError("staged runtime summary changed before publication")
        require_manifest_snapshot(artifact_manifest, "retained artifact")
        require_manifest_snapshot(parity_probe_manifest, "retained parity probe")
        require_manifest_snapshot(tool_manifest, "retained tool")
        if publication_checkout_snapshot(records, repository_root) != checkout_before:
            raise ValueError(
                "current candidate or Specs identity changed during summary publication"
            )
        if validated_evidence_output(
            destination, records, repository_root
        ) != destination:
            raise ValueError("runtime summary output destination changed")
        publish_staged_exclusive(staged, destination)
    finally:
        staged.unlink(missing_ok=True)


def finite_nonnegative(value: Any) -> bool:
    return (
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(value)
        and value >= 0
    )


def sha256_string(value: Any) -> bool:
    return (
        isinstance(value, str)
        and len(value) == 64
        and all(character in "0123456789abcdef" for character in value)
    )


def git_commit_string(value: Any) -> bool:
    return (
        isinstance(value, str)
        and len(value) == 40
        and all(character in "0123456789abcdef" for character in value)
    )


def source_identity_object(value: Any) -> bool:
    try:
        _SOURCE_IDENTITY_MODULE.validate_identity(value)
    except (RuntimeError, TypeError, ValueError):
        return False
    return True


def specs_contract_identity_object(value: Any) -> bool:
    try:
        _SOURCE_IDENTITY_MODULE.validate_specs_contract_identity(value)
    except (RuntimeError, TypeError, ValueError):
        return False
    return True


def cpu_affinity_set(value: Any) -> frozenset[int] | None:
    """Normalize taskset range and expanded-list spellings to one CPU set."""
    if not isinstance(value, str) or not value or value == "unbound":
        return None
    cpus: set[int] = set()
    try:
        for component in value.split(","):
            if not component:
                return None
            if "-" in component:
                if component.count("-") != 1:
                    return None
                lower_text, upper_text = component.split("-", maxsplit=1)
                lower = int(lower_text)
                upper = int(upper_text)
                if lower < 0 or upper < lower:
                    return None
                cpus.update(range(lower, upper + 1))
            else:
                cpu = int(component)
                if cpu < 0:
                    return None
                cpus.add(cpu)
    except ValueError:
        return None
    return frozenset(cpus) if cpus else None


def record_label(record: dict[str, Any]) -> str:
    artifact = record.get("_artifact", {})
    return f"{artifact.get('path', '?')}#{artifact.get('record_index', '?')}"


def independent_process_group(
    group: list[dict[str, Any]], minimum_runs: int
) -> tuple[bool, str]:
    run_ids = {nested(record, "identity", "run_id") for record in group}
    process_ids = {
        nested(record, "identity", "process_instance_id") for record in group
    }
    paths = {nested(record, "_artifact", "path") for record in group}
    artifact_hashes = {
        nested(record, "_artifact", "sha256") for record in group
    }
    passed = (
        len(group) == minimum_runs
        and len(run_ids) == len(group)
        and len(process_ids) == len(group)
        and len(paths) == len(group)
        and len(artifact_hashes) == len(group)
    )
    detail = (
        f"records={len(group)} runs={len(run_ids)} "
        f"processes={len(process_ids)} paths={len(paths)} "
        f"hashes={len(artifact_hashes)} required={minimum_runs}"
    )
    return passed, detail


def fixture_signature(record: dict[str, Any]) -> tuple[Any, ...]:
    inputs = record.get("inputs", {})
    config = record.get("config", {})
    return (
        inputs.get("fixture_id"),
        inputs.get("robot_model"),
        inputs.get("group"),
        inputs.get("geometry_fingerprint_fnv1a64"),
        inputs.get("pair_fingerprint_fnv1a64"),
        inputs.get("trajectory_fingerprint_fnv1a64"),
        inputs.get("trajectory_waypoint_count"),
        inputs.get("bounded_threshold_m"),
        config.get("monitor_period_ns"),
        config.get("immediate_query_budget_ns"),
        config.get("lookahead_cycle_budget_ns"),
        config.get("lookahead_horizon_ns"),
        config.get("lookahead_query_budget_ns"),
        config.get("lookahead_max_samples"),
        config.get("lookahead_workers_requested"),
        config.get("lookahead_workers_effective"),
        config.get("warmup_ticks_requested"),
        config.get("warmup_lookahead_cycles_requested"),
        config.get("measured_ticks_requested"),
        config.get("measured_lookahead_cycles_requested"),
        config.get("requested_cpu_affinity"),
        record.get("promotion_scope"),
    )


def baseline_fixture_identity(record: dict[str, Any]) -> dict[str, Any]:
    inputs = record.get("inputs", {})
    return {
        field: inputs.get(field)
        for field in (
            "fixture_id",
            "robot_model",
            "group",
            "geometry_fingerprint_fnv1a64",
            "pair_fingerprint_fnv1a64",
            "policy_fingerprint_fnv1a64",
            "trajectory_fingerprint_fnv1a64",
            "world_content_hash",
            "allowed_contact_pair_count",
            "trajectory_waypoint_count",
        )
    }


def normalized_pair_output(record: dict[str, Any]) -> dict[str, Any] | None:
    probe = record.get("parity_probe", {})
    pairs = probe.get("normalized_pairs") if isinstance(probe, dict) else None
    if (
        not isinstance(probe, dict)
        or probe.get("evidence_kind") != "unmeasured_output"
        or not isinstance(pairs, list)
        or not pairs
        or not all(
            isinstance(pair, dict)
            and isinstance(pair.get("body_a"), str)
            and isinstance(pair.get("body_b"), str)
            and pair["body_a"] < pair["body_b"]
            for pair in pairs
        )
    ):
        return None
    canonical_pairs = [
        {"body_a": pair["body_a"], "body_b": pair["body_b"]}
        for pair in pairs
    ]
    if canonical_pairs != sorted(
        canonical_pairs, key=lambda pair: (pair["body_a"], pair["body_b"])
    ) or len(
        {(pair["body_a"], pair["body_b"]) for pair in canonical_pairs}
    ) != len(canonical_pairs):
        return None
    rendered = json.dumps(
        canonical_pairs, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    fingerprint = probe.get("normalized_pair_output_fingerprint_fnv1a64")
    if not isinstance(fingerprint, str) or not fingerprint:
        return None
    return {
        "engine_status_code": probe.get("engine_status_code"),
        "decision_code": probe.get("decision_code"),
        "normalized_pair_output_fingerprint_fnv1a64": fingerprint,
        "normalized_pairs": canonical_pairs,
        "normalized_pairs_sha256": hashlib.sha256(rendered).hexdigest(),
    }


def baseline_decision_profile(record: dict[str, Any]) -> dict[str, Any]:
    return {
        "normalized_pair_output": normalized_pair_output(record),
        "immediate": [
            (sample.get("status"), sample.get("reason_code"))
            for sample in nested(
                record, "raw", "immediate_samples", default=[]
            )
        ],
        "lookahead": [
            (
                sample.get("status"),
                sample.get("checked_samples"),
                sample.get("cycle_deadline_exceeded"),
            )
            for sample in nested(
                record, "raw", "lookahead_cycles", default=[]
            )
        ],
    }


def validate_record(record: dict[str, Any], checks: Checks) -> None:
    label = record_label(record)
    identity = record.get("identity", {})
    inputs = record.get("inputs", {})
    config = record.get("config", {})
    immediate = nested(record, "immediate", "summary", default={})
    lookahead = nested(record, "lookahead", "summary", default={})
    raw_immediate = nested(record, "raw", "immediate_samples", default=[])
    raw_lookahead = nested(record, "raw", "lookahead_cycles", default=[])
    external = record.get("external_required", {})
    cross = record.get("cross_checks", {})
    provenance = record.get("provenance", {})

    arm = inputs.get("arm")
    kind = identity.get("measurement_kind")
    role = provenance.get("source_role")
    expected_evidence = {
        ("current_candidate", "timing"): "candidate_timing",
        ("current_candidate", "observation"): "candidate_observation",
        ("pristine_baseline_timing", "timing"): "pristine_timing",
        (
            "pristine_baseline_observation",
            "observation",
        ): "baseline_observation_only",
    }.get((role, kind))
    checks.require(f"{label}:schema", record.get("schema_version") == SCHEMA)
    checks.require(f"{label}:known_arm", arm in (BASELINE_ARM, *UPDATED_ARMS))
    checks.require(f"{label}:measurement_kind", kind in MEASUREMENT_KINDS)
    checks.require(
        f"{label}:evidence_label",
        expected_evidence is not None
        and identity.get("evidence_kind") == expected_evidence,
    )
    expected_measurement_label = (
        "observation_only" if kind == "observation" else "timing_measurement"
    )
    checks.require(
        f"{label}:surface_evidence_labels",
        immediate.get("evidence_kind") == expected_measurement_label
        and lookahead.get("evidence_kind") == expected_measurement_label,
    )
    checks.require(
        f"{label}:identity:source_identity",
        source_identity_object(identity.get("source_identity")),
    )
    checks.require(
        f"{label}:identity:specs_source_identity",
        source_identity_object(identity.get("specs_source_identity")),
    )
    checks.require(
        f"{label}:identity:specs_contract_identity",
        specs_contract_identity_object(identity.get("specs_contract_identity"))
        and nested(identity, "specs_contract_identity", "head_commit")
        == nested(identity, "specs_source_identity", "head_commit")
        and nested(identity, "specs_contract_identity", "head_tree")
        == nested(identity, "specs_source_identity", "head_tree"),
    )
    for field in ("image_id", "binary_sha256", "compiler"):
        checks.require(
            f"{label}:identity:{field}",
            isinstance(identity.get(field), str)
            and bool(identity[field])
            and identity[field] != "unknown",
        )
    for field in ("image_id", "binary_sha256"):
        checks.require(
            f"{label}:identity:{field}:sha256",
            sha256_string(identity.get(field)),
        )
    process_instance_id = identity.get("process_instance_id")
    try:
        process_uuid_valid = str(uuid.UUID(str(process_instance_id))) == process_instance_id
    except (ValueError, AttributeError):
        process_uuid_valid = False
    for field in ("run_id", "process_instance_id"):
        checks.require(
            f"{label}:identity:{field}",
            isinstance(identity.get(field), str) and bool(identity.get(field)),
        )
    checks.require(f"{label}:identity:process_uuid", process_uuid_valid)
    checks.require(
        f"{label}:promotion_scope",
        record.get("promotion_scope") == "local_runtime_only",
    )
    checks.require(
        f"{label}:source_role_present",
        provenance.get("source_role")
        in {
            "current_candidate",
            "pristine_baseline_timing",
            "pristine_baseline_observation",
        }
        and isinstance(provenance.get("checkout_base_clean"), bool),
    )
    for field in (
        "clean_checkout_manifest_sha256",
        "qualification_patch_sha256",
        "observation_patch_sha256",
        "pristine_decision_parity_sha256",
    ):
        value = provenance.get(field)
        checks.require(
            f"{label}:provenance:{field}:nullable_sha256",
            value is None or sha256_string(value),
        )
    command = provenance.get("command_argv")
    runner_command = provenance.get("runner_command_argv")
    build_commands = provenance.get("build_commands_argv")
    outer_command = provenance.get("outer_command_argv")
    runner_environment = provenance.get("runner_environment")
    checks.require(
        f"{label}:exact_command_provenance",
        isinstance(command, list)
        and len(command) >= 4
        and all(isinstance(item, str) and item for item in command)
        and isinstance(runner_command, list)
        and len(runner_command) >= 2
        and all(isinstance(item, str) and item for item in runner_command)
        and isinstance(build_commands, list)
        and bool(build_commands)
        and all(
            isinstance(item, list)
            and bool(item)
            and all(isinstance(argument, str) and argument for argument in item)
            for item in build_commands
        )
        and isinstance(outer_command, list)
        and len(outer_command) >= 3
        and outer_command[:2] == ["docker", "compose"]
        and all(isinstance(item, str) and item for item in outer_command)
        and isinstance(runner_environment, dict)
        and bool(runner_environment)
        and sha256_string(provenance.get("runner_script_sha256"))
        and sha256_string(provenance.get("wrapper_script_sha256"))
        and sha256_string(provenance.get("source_identity_script_sha256"))
        and sha256_string(provenance.get("raw_record_sha256"))
        and (
            provenance.get("prepare_script_sha256") is None
            or sha256_string(provenance.get("prepare_script_sha256"))
        ),
    )
    checks.require(
        f"{label}:external_not_measured",
        external.get("evidence_kind") == "not_measured"
        and all(external.get(field, "missing") is None for field in EXTERNAL_NULL_FIELDS),
    )
    checks.require(
        f"{label}:exact_formal_sampling_profile",
        all(config.get(field) == expected for field, expected in FORMAL_PROFILE.items())
        and inputs.get("bounded_threshold_m") == FORMAL_BOUNDED_THRESHOLD_M,
    )
    checks.require(
        f"{label}:controlled_cpu_affinity",
        cpu_affinity_set(config.get("requested_cpu_affinity")) is not None
        and cpu_affinity_set(config.get("requested_cpu_affinity"))
        == cpu_affinity_set(config.get("effective_cpu_affinity")),
    )
    requested_cpus = cpu_affinity_set(config.get("requested_cpu_affinity"))
    checks.require(
        f"{label}:formal_four_cpu_affinity",
        requested_cpus is not None and len(requested_cpus) == 4,
    )
    checks.require(
        f"{label}:execution_environment_provenance",
        all(
            isinstance(config.get(field), str)
            and bool(config.get(field))
            and config.get(field) != "unknown"
            for field in (
                "cpu_governor_set",
                "cpu_frequency_driver_set",
                "kernel_id",
                "compiler_flags",
                "build_type",
            )
        )
        and config.get("build_type") == "Release"
        and nested(provenance, "runner_environment", "requested_cpu_affinity")
        == config.get("requested_cpu_affinity")
        and nested(provenance, "runner_environment", "image_id")
        == identity.get("image_id")
        and nested(provenance, "runner_environment", "compiler_flags")
        == config.get("compiler_flags")
        and nested(provenance, "runner_environment", "build_type")
        == config.get("build_type")
        and nested(provenance, "runner_environment", "build_jobs") == "1"
        and nested(provenance, "runner_environment", "cpu_governor_set")
        == config.get("cpu_governor_set")
        and nested(provenance, "runner_environment", "cpu_frequency_driver_set")
        == config.get("cpu_frequency_driver_set")
        and nested(provenance, "runner_environment", "kernel_id")
        == config.get("kernel_id")
        and nested(provenance, "runner_environment", "source_fingerprint_sha256")
        == nested(identity, "source_identity", "fingerprint_sha256")
        and nested(provenance, "runner_environment", "specs_fingerprint_sha256")
        == nested(identity, "specs_source_identity", "fingerprint_sha256")
        and nested(
            provenance,
            "runner_environment",
            "specs_contract_fingerprint_sha256",
        )
        == nested(
            identity,
            "specs_contract_identity",
            "contract_fingerprint_sha256",
        )
        and isinstance(
            nested(provenance, "runner_environment", "source_root"), str
        )
        and bool(nested(provenance, "runner_environment", "source_root"))
        and isinstance(
            nested(provenance, "runner_environment", "specs_root"), str
        )
        and bool(nested(provenance, "runner_environment", "specs_root")),
    )
    checks.require(
        f"{label}:fixture_contact_active",
        inputs.get("allowed_contact_pair_count", 0) > 0
        and inputs.get("maximum_observed_penetration_m", 0) > 0,
    )
    pair_output = normalized_pair_output(record)
    checks.require(
        f"{label}:unmeasured_normalized_pair_output",
        pair_output is not None
        and pair_output.get("engine_status_code") == 0
        and pair_output.get("decision_code") == 1,
    )

    measured_ticks = config.get("measured_ticks_requested", 0)
    measured_cycles = config.get("measured_lookahead_cycles_requested", 0)
    immediate_count = immediate.get("sample_count", -1)
    lookahead_count = lookahead.get("sample_count", -1)
    checks.require(
        f"{label}:immediate_samples",
        isinstance(immediate_count, int)
        and immediate_count >= measured_ticks
        and len(raw_immediate) == immediate_count,
    )
    checks.require(
        f"{label}:immediate_clear",
        immediate.get("clear") == immediate_count
        and immediate.get("collision") == 0
        and immediate.get("inconclusive") == 0
        and immediate.get("faults") == 0,
    )
    query_budget = config.get("immediate_query_budget_ns", -1)
    checks.require(
        f"{label}:immediate_query_budget",
        all(
            finite_nonnegative(sample.get("engine_query_ns"))
            and sample["engine_query_ns"] <= query_budget
            and finite_nonnegative(sample.get("tick_wall_ns"))
            and finite_nonnegative(sample.get("callback_wall_ns"))
            and finite_nonnegative(sample.get("process_cpu_ns"))
            and finite_nonnegative(sample.get("thread_cpu_ns"))
            and sample.get("callback_started_at_ns", 1)
            <= sample.get("started_at_ns", 0)
            <= sample.get("finished_at_ns", -1)
            <= sample.get("callback_finished_at_ns", -2)
            for sample in raw_immediate
        ),
    )
    scheduler = immediate.get("scheduler", {})
    checks.require(
        f"{label}:monitor_no_misses",
        scheduler.get("missed_periods") == 0
        and scheduler.get("callbacks_started") == immediate_count,
    )

    checks.require(
        f"{label}:lookahead_cycles",
        isinstance(lookahead_count, int)
        and lookahead_count == measured_cycles
        and len(raw_lookahead) == lookahead_count,
    )
    checks.require(
        f"{label}:lookahead_clear",
        lookahead.get("clear") == lookahead_count
        and lookahead.get("collision") == 0
        and lookahead.get("inconclusive_or_other") == 0
        and lookahead.get("faults") == 0,
    )
    checks.require(
        f"{label}:lookahead_exact_32",
        lookahead.get("checked_samples_min") == 32
        and lookahead.get("checked_samples_max") == 32
        and lookahead.get("checked_samples_total") == 32 * lookahead_count
        and lookahead.get("exact_32_failures") == 0
        and cross.get("exact_32_lookahead") is True,
    )
    checks.require(
        f"{label}:lookahead_request_callback_accounting",
        cross.get("measured_lookahead_requests_issued")
        == lookahead_count,
    )
    cycle_budget = config.get("lookahead_cycle_budget_ns", -1)
    checks.require(
        f"{label}:lookahead_cycle_budget",
        all(
            finite_nonnegative(cycle.get("cycle_wall_ns"))
            and cycle["cycle_wall_ns"] <= cycle_budget
            and cycle.get("cycle_deadline_exceeded") is False
            and cycle.get("checked_samples") == 32
            and finite_nonnegative(cycle.get("process_cpu_ns"))
            for cycle in raw_lookahead
        ),
    )
    status = lookahead.get("status_counters", {})
    checks.require(
        f"{label}:lookahead_no_timeouts",
        status.get("jobs_query_timeout_total") == 0
        and status.get("jobs_cycle_timeout_total") == 0
        and status.get("jobs_inconclusive_total") == 0
        and status.get("stale_generation_results_total") == 0
        and lookahead.get("process_stamp_missing") == 0,
    )
    checks.require(
        f"{label}:lookahead_no_unreported_or_coalesced_cycles",
        status.get("jobs_started_total")
        == config.get("warmup_lookahead_cycles_requested", -1)
        + measured_cycles
        and status.get("reruns_coalesced_total") == 0
        and status.get("clear_permit_try_lock_misses_total") == 0,
    )
    derived_overlap = sum(
        1
        for immediate_sample in raw_immediate
        if any(
            immediate_sample.get("callback_started_at_ns", 1)
            < lookahead_cycle.get("requested_at_ns", 0)
            + lookahead_cycle.get("start_jitter_ns", 0)
            + lookahead_cycle.get("worker_active_wall_ns", 0)
            and immediate_sample.get("callback_finished_at_ns", -1)
            > lookahead_cycle.get("requested_at_ns", 0)
            + lookahead_cycle.get("start_jitter_ns", 0)
            for lookahead_cycle in raw_lookahead
        )
    )
    checks.require(
        f"{label}:worker_active_overlap_exact_derivation",
        cross.get("immediate_lookahead_worker_active_overlap_ticks")
        == derived_overlap,
    )
    checks.require(
        f"{label}:cross_check_evidence_labels",
        cross.get("semantic_evidence_kind") == "exact_derivation"
        and cross.get("mechanism_evidence_kind")
        == ("direct_counter" if kind == "observation" else "not_measured"),
    )
    bounded_helper_key = (
        "bounded_transition_helper_any_allowed_pair_clear_late_appearance_"
        "disappearance_reappearance_final_contact_accepted"
    )
    if arm == BASELINE_ARM:
        checks.require(
            f"{label}:bounded_semantics_not_backported_to_baseline",
            cross.get("bounded_semantic_evidence_kind") == "not_applicable"
            and cross.get(bounded_helper_key) is None
            and cross.get("bounded_over_cap_transition_rejected") is None
            and cross.get("bounded_expected_contacts_under_cap") is None,
        )
    else:
        checks.require(
            f"{label}:bounded_transition_helper_semantic_preflight",
            cross.get("bounded_semantic_evidence_kind")
            == "exact_derivation"
            and cross.get(bounded_helper_key) is True
            and cross.get("bounded_over_cap_transition_rejected") is True
            and cross.get("bounded_expected_contacts_under_cap") is True,
        )

    for context_name in ("immediate_context", "lookahead_shared_context"):
        cold = nested(record, "cold_snapshot_sync", context_name, default={})
        warm = nested(record, "warm_snapshot_reuse", context_name, default={})
        checks.require(
            f"{label}:{context_name}:cold_ready",
            cold.get("evidence_kind") == expected_measurement_label
            and cold.get("prepare_status") == "ready"
            and finite_nonnegative(cold.get("wall_ns"))
            and finite_nonnegative(cold.get("process_cpu_ns")),
        )
        checks.require(
            f"{label}:{context_name}:warm_reuse",
            warm.get("evidence_kind") == expected_measurement_label
            and warm.get("prepare_status") == "ready"
            and finite_nonnegative(warm.get("wall_ns"))
            and finite_nonnegative(warm.get("process_cpu_ns")),
        )

    if kind != "observation":
        checks.require(
            f"{label}:timing_instrumentation_absent",
            immediate.get("framework_owned_allocations") is None
            and immediate.get("engine_boundary_allocations") is None
            and cross.get("all_colliding_pairs_declared") is None
            and immediate.get("mechanism", {}).get("evidence_kind")
            == "not_measured"
            and lookahead.get("mechanism", {}).get("evidence_kind")
            == "not_measured",
        )
        return
    checks.require(
        f"{label}:all_collisions_declared",
        cross.get("all_colliding_pairs_declared") is True,
    )
    for context_name in ("immediate_context", "lookahead_shared_context"):
        cold = nested(record, "cold_snapshot_sync", context_name, default={})
        warm = nested(record, "warm_snapshot_reuse", context_name, default={})
        checks.require(
            f"{label}:{context_name}:cold_build_once",
            cold.get("scene_build_count_after")
            == cold.get("scene_build_count_before", -2) + 1,
        )
        checks.require(
            f"{label}:{context_name}:warm_build_reused",
            warm.get("scene_build_count_after")
            == warm.get("scene_build_count_before"),
        )
    checks.require(
        f"{label}:zero_framework_warm_allocations",
        immediate.get("framework_owned_allocations") == 0
        and all(sample.get("framework_allocations") == 0 for sample in raw_immediate),
    )
    checks.require(
        f"{label}:allocation_totals_attributed",
        immediate.get("framework_owned_allocations")
        == sum(sample.get("framework_allocations", -1) for sample in raw_immediate)
        and immediate.get("engine_boundary_allocations")
        == sum(sample.get("engine_allocations", -1) for sample in raw_immediate),
    )
    immediate_cold = nested(
        record, "cold_snapshot_sync", "immediate_context", default={}
    )
    lookahead_cold = nested(
        record, "cold_snapshot_sync", "lookahead_shared_context", default={}
    )
    checks.require(
        f"{label}:cold_allocation_owner_attribution",
        immediate_cold.get("lookahead_engine_allocations") == 0
        and lookahead_cold.get("immediate_engine_allocations") == 0
        and finite_nonnegative(immediate_cold.get("immediate_engine_allocations"))
        and finite_nonnegative(lookahead_cold.get("lookahead_engine_allocations")),
    )
    immediate_mechanism = immediate.get("mechanism", {})
    lookahead_mechanism = lookahead.get("mechanism", {})
    expected_queries = (immediate_count, 32 * lookahead_count)
    for surface, mechanism, query_count in (
        ("immediate", immediate_mechanism, expected_queries[0]),
        ("lookahead", lookahead_mechanism, expected_queries[1]),
    ):
        checks.require(
            f"{label}:{surface}:complete_collision_first",
            mechanism.get("evidence_kind") == "direct_counter"
            and mechanism.get("query_count") == query_count
            and mechanism.get("collision_api_calls") == query_count
            and mechanism.get("collision_completion_ordinal_failures") == 0
            and mechanism.get("active_collision_pairs", 0) > 0
            and mechanism.get("active_collision_pairs", 0)
            > mechanism.get("colliding_geometry_pairs", 0)
            and mechanism.get("colliding_geometry_pairs", 0)
            >= mechanism.get("colliding_logical_pairs", 0)
            and mechanism.get("colliding_logical_pairs", 0) > 0
            and (
                mechanism.get("fk_placement_invocations", 0) >= query_count
                if arm == BASELINE_ARM
                else mechanism.get("fk_placement_invocations")
                == query_count
            ),
        )
        checks.require(
            f"{label}:{surface}:declared_pairs_only",
            mechanism.get("undeclared_colliding_logical_pairs") == 0,
        )
        checks.require(
            f"{label}:{surface}:distance_mode",
            mechanism.get("distance_mode_mask") == EXPECTED_DISTANCE_MASK.get(arm),
        )
        if arm == BASELINE_ARM:
            # The genuine before source intentionally measures all active
            # pairs. It is evidence for the cost being removed, never a
            # production fallback in the updated candidate.
            distance_ok = (
                mechanism.get("distance_api_calls", 0) > 0
                and mechanism.get("distance_geometry_pairs", 0) > 0
                and mechanism.get("distance_logical_pairs", 0) > 0
            )
        elif arm == "binary_contact":
            distance_ok = (
                mechanism.get("distance_api_calls") == 0
                and mechanism.get("distance_geometry_pairs") == 0
                and mechanism.get("distance_logical_pairs") == 0
            )
        else:
            distance_ok = (
                mechanism.get("distance_api_calls", 0) > 0
                and mechanism.get("distance_geometry_pairs", 0) > 0
                and mechanism.get("distance_logical_pairs", 0) > 0
                and mechanism.get("distance_geometry_pairs", math.inf)
                <= mechanism.get("colliding_geometry_pairs", -1)
                and mechanism.get("distance_logical_pairs", math.inf)
                <= mechanism.get("colliding_logical_pairs", -1)
            )
        checks.require(f"{label}:{surface}:distance_work", distance_ok)


def median_p95(records: Iterable[dict[str, Any]], surface: str) -> float:
    metric = "process_cpu_window"
    values = [
        nested(record, surface, "summary", metric, "p95_ns")
        for record in records
    ]
    if not values or not all(finite_nonnegative(value) and value > 0 for value in values):
        return math.nan
    return float(statistics.median(values))


def finite_json_number(value: float) -> float | None:
    return value if math.isfinite(value) else None


def normalized_mechanism(record: dict[str, Any], surface: str, field: str) -> tuple[int, int]:
    summary = nested(record, surface, "summary", default={})
    mechanism = summary.get("mechanism", {})
    denominator = mechanism.get("query_count", 0)
    return int(mechanism.get(field, -1)), int(denominator)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifacts", nargs="+", type=Path)
    parser.add_argument("--minimum-runs", type=int, default=3)
    parser.add_argument(
        "--allow-missing-baseline",
        action="store_true",
        help="verify current artifacts but keep the before/after gate open",
    )
    parser.add_argument("--summary-out", type=Path)
    parser.add_argument(
        "--baseline-clean-checkout-manifest",
        type=Path,
        help=(
            "archived pre-patch manifest proving clean manyforge/specs "
            "checkouts at the exact baseline commits"
        ),
    )
    parser.add_argument(
        "--baseline-harness-patch",
        type=Path,
        help="archived harness-only patch applied to clean 2bfe30e",
    )
    parser.add_argument(
        "--baseline-observation-patch",
        type=Path,
        help="archived observation-only instrumentation patch for 2bfe30e",
    )
    parser.add_argument(
        "--baseline-decision-parity-artifact",
        type=Path,
        help="artifact proving instrumented and pristine baseline decisions/pairs match",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.minimum_runs != FORMAL_MINIMUM_RUNS:
        raise SystemExit(
            f"formal --minimum-runs must be exactly {FORMAL_MINIMUM_RUNS}"
        )
    checks = Checks()
    records: list[dict[str, Any]] = []
    artifact_manifest: list[dict[str, Any]] = []
    try:
        for path in args.artifacts:
            resolved = path.resolve()
            loaded_records = load_artifact(resolved)
            if len(loaded_records) != 1:
                raise ValueError(
                    f"formal runtime input must contain exactly one record: {resolved}"
                )
            record = loaded_records[0]
            artifact_manifest.append(
                {
                    "role": "runtime_schedule_artifact",
                    "path": str(resolved),
                    "sha256": record["_artifact"]["sha256"],
                    "arm": nested(record, "inputs", "arm"),
                    "measurement_kind": nested(
                        record, "identity", "measurement_kind"
                    ),
                    "run_id": nested(record, "identity", "run_id"),
                    "process_instance_id": nested(
                        record, "identity", "process_instance_id"
                    ),
                }
            )
            records.extend(loaded_records)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"artifact load failed: {error}", file=sys.stderr)
        return 2

    verifier_path = Path(__file__).resolve()
    scripts_root = verifier_path.parents[1]
    repository_root = verifier_path.parents[2]
    tool_paths = {
        "runtime_schedule_verifier": verifier_path,
        "runtime_artifact_wrapper": verifier_path.with_name(
            "wrap_runtime_collision_schedule_artifact.py"
        ),
        "source_identity_helper": verifier_path.with_name("source_identity.py"),
        "current_runtime_runner": scripts_root
        / "run-runtime-collision-schedule-qualification.sh",
        "baseline_runtime_runner": scripts_root
        / "run-runtime-collision-schedule-baseline.sh",
        "baseline_runtime_prepare": scripts_root
        / "prepare-runtime-collision-schedule-baseline.sh",
        "baseline_parity_generator": verifier_path.with_name(
            "generate_runtime_collision_baseline_parity.py"
        ),
        "baseline_observation_patch": verifier_path.parent
        / "baseline"
        / "runtime_collision_schedule_observation.patch",
        "runtime_harness_source": repository_root
        / "tests/cpp/perf/perf_runtime_collision_schedule_qualification.cpp",
        "baseline_runtime_cmake_patch": verifier_path.parent
        / "baseline"
        / "runtime_collision_schedule_cmake.patch",
        "baseline_runtime_compat_patch": verifier_path.parent
        / "baseline"
        / "runtime_collision_schedule_baseline_compat.patch",
    }
    try:
        tool_manifest = [
            {
                "role": role,
                "path": str(path.resolve()),
                "sha256": sha256_file(path.resolve()),
            }
            for role, path in tool_paths.items()
        ]
        summary_destination = (
            validated_evidence_output(args.summary_out, records, repository_root)
            if args.summary_out is not None
            else None
        )
    except (OSError, ValueError) as error:
        print(f"runtime verifier setup failed: {error}", file=sys.stderr)
        return 2
    tool_hashes = {entry["role"]: entry["sha256"] for entry in tool_manifest}

    auxiliary_paths = {
        "baseline_clean_checkout_manifest": (
            args.baseline_clean_checkout_manifest
        ),
        "baseline_harness_patch": args.baseline_harness_patch,
        "baseline_observation_patch": args.baseline_observation_patch,
        "baseline_decision_parity_artifact": args.baseline_decision_parity_artifact,
    }
    auxiliary_hashes: dict[str, str | None] = {}
    auxiliary_payloads: dict[str, bytes] = {}
    try:
        for role, path in auxiliary_paths.items():
            if path is None:
                auxiliary_hashes[role] = None
                continue
            resolved = path.resolve()
            payload, digest = read_file_snapshot(resolved)
            auxiliary_hashes[role] = digest
            auxiliary_payloads[role] = payload
            artifact_manifest.append(
                {"path": str(resolved), "sha256": digest, "role": role}
            )
    except OSError as error:
        print(f"baseline provenance input failed: {error}", file=sys.stderr)
        return 2


    harness_patch_paths: set[str] | None = None
    observation_patch_paths: set[str] | None = None
    try:
        if args.baseline_harness_patch is not None:
            harness_patch_paths = patch_paths_from_bytes(
                auxiliary_payloads["baseline_harness_patch"],
                args.baseline_harness_patch,
            )
        if args.baseline_observation_patch is not None:
            observation_patch_paths = patch_paths_from_bytes(
                auxiliary_payloads["baseline_observation_patch"],
                args.baseline_observation_patch,
            )
    except (OSError, ValueError) as error:
        print(f"baseline patch scope failed: {error}", file=sys.stderr)
        return 2

    if harness_patch_paths is not None:
        checks.require(
            "baseline_harness_patch:harness_only_scope",
            harness_patch_paths
            <= {
                "tests/cpp/CMakeLists.txt",
                "tests/cpp/perf/perf_runtime_collision_schedule_qualification.cpp",
            }
            and "tests/cpp/CMakeLists.txt" in harness_patch_paths
            and "tests/cpp/perf/perf_runtime_collision_schedule_qualification.cpp"
            in harness_patch_paths,
            f"paths={sorted(harness_patch_paths)}",
        )
    if observation_patch_paths is not None:
        checks.require(
            "baseline_observation_patch:instrumentation_scope",
            observation_patch_paths
            <= {
                "manyforge_collision/include/manyforge_collision/manyforge_collision_engine.hpp",
                "manyforge_collision/src/manyforge_collision_engine.cpp",
            }
            and bool(observation_patch_paths),
            f"paths={sorted(observation_patch_paths)}",
        )

    clean_checkout_manifest: dict[str, Any] | None = None
    if args.baseline_clean_checkout_manifest is not None:
        try:
            parsed_manifest = json.loads(
                auxiliary_payloads["baseline_clean_checkout_manifest"]
            )
            if not isinstance(parsed_manifest, dict):
                raise ValueError("clean-checkout manifest must be a JSON object")
            clean_checkout_manifest = parsed_manifest
        except (OSError, ValueError, json.JSONDecodeError) as error:
            print(
                f"baseline clean-checkout manifest failed: {error}",
                file=sys.stderr,
            )
            return 2

    parity_artifact: dict[str, Any] | None = None
    parity_probe_manifest: list[dict[str, Any]] = []
    parity_probe_records: dict[str, dict[str, Any]] = {}
    if args.baseline_decision_parity_artifact is not None:
        try:
            parsed_parity = json.loads(
                auxiliary_payloads["baseline_decision_parity_artifact"]
            )
            if not isinstance(parsed_parity, dict):
                raise ValueError("decision parity artifact must be a JSON object")
            parity_artifact = parsed_parity
        except (OSError, ValueError, json.JSONDecodeError) as error:
            print(f"baseline decision parity failed: {error}", file=sys.stderr)
            return 2
        try:
            supplied_manifest = parity_artifact.get("input_manifest")
            if not (
                isinstance(supplied_manifest, list)
                and len(supplied_manifest) == 2
                and all(
                    isinstance(entry, dict)
                    and set(entry) == {"role", "path", "sha256"}
                    and isinstance(entry.get("path"), str)
                    and bool(entry.get("path"))
                    and sha256_string(entry.get("sha256"))
                    for entry in supplied_manifest
                )
            ):
                raise ValueError("parity input_manifest must contain two typed inputs")
            by_role = {entry["role"]: entry for entry in supplied_manifest}
            expected_roles = {
                "baseline_timing_parity_probe",
                "baseline_observation_parity_probe",
            }
            if set(by_role) != expected_roles:
                raise ValueError("parity input_manifest roles are not exact")
            resolved_paths: set[Path] = set()
            for role in sorted(expected_roles):
                entry = by_role[role]
                path = Path(entry["path"]).expanduser().resolve()
                if path in resolved_paths:
                    raise ValueError("parity probe paths must be distinct")
                resolved_paths.add(path)
                payload, digest = read_file_snapshot(path)
                if digest != entry["sha256"]:
                    raise ValueError(f"retained parity probe hash differs: {path}")
                parity_probe_manifest.append(
                    {"role": role, "path": str(path), "sha256": digest}
                )
                parity_probe_records[role] = load_one_probe(payload, path)
        except (OSError, ValueError, json.JSONDecodeError) as error:
            print(f"baseline parity replay input failed: {error}", file=sys.stderr)
            return 2

    if clean_checkout_manifest is not None:
        repositories = clean_checkout_manifest.get("repositories", {})
        timing_checkout = (
            repositories.get("manyforge_timing", {})
            if isinstance(repositories, dict)
            else {}
        )
        specs_checkout = (
            repositories.get("specs", {})
            if isinstance(repositories, dict)
            else {}
        )
        specs_contract_checkout = clean_checkout_manifest.get(
            "specs_contract_identity", {}
        )
        observation_checkout = (
            repositories.get("manyforge_observation", {})
            if isinstance(repositories, dict)
            else {}
        )
        checks.require(
            "baseline_clean_checkout_manifest:schema",
            clean_checkout_manifest.get("schema_version")
            == CLEAN_CHECKOUT_SCHEMA,
        )
        checks.require(
            "baseline_clean_checkout_manifest:captured_before_patch",
            clean_checkout_manifest.get("captured_before_patch") is True,
        )
        checks.require(
            "baseline_clean_checkout_manifest:timing_base_clean",
            source_identity_object(timing_checkout)
            and timing_checkout.get("head_commit") == BASELINE_SOURCE
            and timing_checkout.get("dirty") is False,
        )
        checks.require(
            "baseline_clean_checkout_manifest:observation_base_clean",
            source_identity_object(observation_checkout)
            and observation_checkout.get("head_commit") == BASELINE_SOURCE
            and observation_checkout.get("dirty") is False,
        )
        checks.require(
            "baseline_clean_checkout_manifest:specs_clean",
            source_identity_object(specs_checkout)
            and specs_checkout.get("head_commit") == BASELINE_SPECS
            and specs_checkout.get("dirty") is False,
        )
        checks.require(
            "baseline_clean_checkout_manifest:specs_contract_clean",
            specs_contract_identity_object(specs_contract_checkout)
            and specs_contract_checkout.get("head_commit") == BASELINE_SPECS
            and specs_contract_checkout.get("contract_dirty") is False
            and specs_contract_checkout.get("head_commit")
            == specs_checkout.get("head_commit")
            and specs_contract_checkout.get("head_tree")
            == specs_checkout.get("head_tree"),
        )

    groups: dict[tuple[str, str], list[dict[str, Any]]] = defaultdict(list)
    for record in records:
        validate_record(record, checks)
        groups[(nested(record, "inputs", "arm"), nested(record, "identity", "measurement_kind"))].append(record)

    required_groups = [
        (arm, kind) for arm in UPDATED_ARMS for kind in MEASUREMENT_KINDS
    ]
    baseline_present = all(
        len(groups[(BASELINE_ARM, kind)]) >= args.minimum_runs
        for kind in MEASUREMENT_KINDS
    )
    if baseline_present or not args.allow_missing_baseline:
        for role, digest in auxiliary_hashes.items():
            checks.require(
                f"baseline_provenance:{role}:supplied",
                sha256_string(digest),
            )
    if not args.allow_missing_baseline:
        required_groups.extend(
            (BASELINE_ARM, kind) for kind in MEASUREMENT_KINDS
        )
    for arm, kind in required_groups:
        group = groups[(arm, kind)]
        independent, detail = independent_process_group(
            group, args.minimum_runs
        )
        checks.require(
            f"group:{arm}:{kind}:independent_processes",
            independent,
            detail,
        )
    expected_record_count = (
        len(UPDATED_ARMS) * len(MEASUREMENT_KINDS) * FORMAL_MINIMUM_RUNS
        if args.allow_missing_baseline
        else (len(UPDATED_ARMS) + 1)
        * len(MEASUREMENT_KINDS)
        * FORMAL_MINIMUM_RUNS
    )
    checks.require(
        "exact_formal_record_population",
        len(records) == expected_record_count
        and len(args.artifacts) == expected_record_count
        and (
            args.allow_missing_baseline
            or len(artifact_manifest) == expected_record_count + 4
        ),
        f"records={len(records)} inputs={len(args.artifacts)} "
        f"manifest_entries={len(artifact_manifest)} expected={expected_record_count}",
    )

    all_run_ids = [nested(record, "identity", "run_id") for record in records]
    all_process_ids = [
        nested(record, "identity", "process_instance_id") for record in records
    ]
    all_paths = [nested(record, "_artifact", "path") for record in records]
    all_hashes = [nested(record, "_artifact", "sha256") for record in records]
    checks.require("all_run_ids_unique", len(all_run_ids) == len(set(all_run_ids)))
    checks.require(
        "all_process_instance_ids_unique",
        len(all_process_ids) == len(set(all_process_ids)),
    )
    checks.require("all_artifact_paths_unique", len(all_paths) == len(set(all_paths)))
    checks.require(
        "all_artifact_hashes_unique", len(all_hashes) == len(set(all_hashes))
    )
    if parity_artifact is not None:
        probe_run_ids = {
            parity_artifact.get("timing_probe_run_id"),
            parity_artifact.get("observation_probe_run_id"),
        }
        probe_process_ids = {
            parity_artifact.get("timing_probe_process_instance_id"),
            parity_artifact.get("observation_probe_process_instance_id"),
        }
        probe_hashes = {
            parity_artifact.get("timing_probe_sha256"),
            parity_artifact.get("observation_probe_sha256"),
        }
        checks.require(
            "baseline_decision_parity:probes_disjoint_from_formal_records",
            probe_run_ids.isdisjoint(all_run_ids)
            and probe_process_ids.isdisjoint(all_process_ids)
            and probe_hashes.isdisjoint(all_hashes),
        )

    for kind in MEASUREMENT_KINDS:
        for record in groups[(BASELINE_ARM, kind)]:
            label = record_label(record)
            provenance = record.get("provenance", {})
            checks.require(
                f"{label}:genuine_baseline_source",
                nested(record, "identity", "source_identity", "head_commit")
                == BASELINE_SOURCE
                and nested(record, "identity", "source_identity", "dirty") is True
                and nested(
                    record, "identity", "specs_source_identity", "head_commit"
                )
                == BASELINE_SPECS
                and nested(record, "identity", "specs_source_identity", "dirty")
                is False
                and nested(
                    record,
                    "identity",
                    "specs_contract_identity",
                    "contract_dirty",
                )
                is False
                and provenance.get("checkout_base_clean") is True,
            )
            checks.require(
                f"{label}:formal_record_not_parity_probe",
                nested(record, "provenance", "runner_environment", "purpose")
                == "formal",
            )
            checks.require(
                f"{label}:baseline_harness_patch_archived",
                provenance.get("qualification_patch_sha256")
                == auxiliary_hashes["baseline_harness_patch"],
            )
            checks.require(
                f"{label}:clean_checkout_manifest_archived",
                provenance.get("clean_checkout_manifest_sha256")
                == auxiliary_hashes["baseline_clean_checkout_manifest"],
            )
            if kind == "timing":
                checks.require(
                    f"{label}:pristine_timing_provenance",
                    provenance.get("source_role") == "pristine_baseline_timing"
                    and provenance.get("observation_patch_sha256") is None
                    and provenance.get("pristine_decision_parity_sha256") is None,
                )
            else:
                checks.require(
                    f"{label}:observation_patch_provenance",
                    provenance.get("source_role")
                    == "pristine_baseline_observation"
                    and provenance.get("observation_patch_sha256")
                    == auxiliary_hashes["baseline_observation_patch"]
                    and provenance.get("pristine_decision_parity_sha256")
                    == auxiliary_hashes["baseline_decision_parity_artifact"],
                )

    if parity_artifact is not None:
        parity_comparisons = parity_artifact.get("comparisons", {})
        expected_parity_generator_hash = tool_hashes[
            "baseline_parity_generator"
        ]
        checks.require(
            "baseline_decision_parity:schema_and_source",
            parity_artifact.get("schema_version")
            == BASELINE_PARITY_SCHEMA
            and parity_artifact.get("generator_script_sha256")
            == expected_parity_generator_hash
            and source_identity_object(
                parity_artifact.get("timing_source_identity")
            )
            and source_identity_object(
                parity_artifact.get("observation_source_identity")
            )
            and source_identity_object(
                parity_artifact.get("specs_source_identity")
            )
            and specs_contract_identity_object(
                parity_artifact.get("specs_contract_identity")
            )
            and nested(parity_artifact, "timing_source_identity", "head_commit")
            == BASELINE_SOURCE
            and nested(
                parity_artifact, "observation_source_identity", "head_commit"
            )
            == BASELINE_SOURCE
            and nested(parity_artifact, "specs_source_identity", "head_commit")
            == BASELINE_SPECS
            and nested(
                parity_artifact, "specs_contract_identity", "head_commit"
            )
            == BASELINE_SPECS
            and nested(
                parity_artifact, "specs_contract_identity", "contract_dirty"
            )
            is False,
        )
        checks.require(
            "baseline_decision_parity:canonical_observation_patch",
            auxiliary_hashes.get("baseline_observation_patch")
            == tool_hashes["baseline_observation_patch"],
        )
        checks.require(
            "baseline_decision_parity:typed_claims",
            parity_artifact.get("parity_passed") is True
            and isinstance(parity_comparisons, dict)
            and parity_comparisons.get("fixture_identity_equal") is True
            and parity_comparisons.get(
                "normalized_logical_pairs_equal"
            )
            is True
            and parity_comparisons.get("immediate_status_equal") is True
            and parity_comparisons.get("immediate_reason_equal") is True
            and parity_comparisons.get("lookahead_status_equal") is True
            and parity_comparisons.get("lookahead_exact_32_equal") is True
            and isinstance(parity_artifact.get("timing_output"), dict)
            and isinstance(parity_artifact.get("observation_output"), dict)
            and parity_artifact.get("timing_output")
            == parity_artifact.get("observation_output"),
        )
        checks.require(
            "baseline_decision_parity:binary_hash_shape",
            sha256_string(parity_artifact.get("timing_binary_sha256"))
            and sha256_string(
                parity_artifact.get("observation_binary_sha256")
            )
            and sha256_string(parity_artifact.get("timing_probe_sha256"))
            and sha256_string(
                parity_artifact.get("observation_probe_sha256")
            ),
        )
        timing_probe_process_id = parity_artifact.get(
            "timing_probe_process_instance_id"
        )
        observation_probe_process_id = parity_artifact.get(
            "observation_probe_process_instance_id"
        )
        try:
            timing_probe_uuid_valid = (
                str(uuid.UUID(str(timing_probe_process_id)))
                == timing_probe_process_id
            )
            observation_probe_uuid_valid = (
                str(uuid.UUID(str(observation_probe_process_id)))
                == observation_probe_process_id
            )
        except (AttributeError, ValueError):
            timing_probe_uuid_valid = False
            observation_probe_uuid_valid = False
        timing_probe_run_id = parity_artifact.get("timing_probe_run_id")
        observation_probe_run_id = parity_artifact.get(
            "observation_probe_run_id"
        )
        checks.require(
            "baseline_decision_parity:independent_probe_identities",
            isinstance(timing_probe_run_id, str)
            and bool(timing_probe_run_id)
            and isinstance(observation_probe_run_id, str)
            and bool(observation_probe_run_id)
            and timing_probe_run_id != observation_probe_run_id
            and timing_probe_uuid_valid
            and observation_probe_uuid_valid
            and timing_probe_process_id != observation_probe_process_id,
        )
        checks.require(
            "baseline_decision_parity:probe_artifacts_distinct",
            parity_artifact.get("timing_probe_sha256")
            != parity_artifact.get("observation_probe_sha256"),
        )
        timing_probe = parity_probe_records.get(
            "baseline_timing_parity_probe", {}
        )
        observation_probe = parity_probe_records.get(
            "baseline_observation_parity_probe", {}
        )
        if timing_probe:
            validate_record(timing_probe, checks)
        if observation_probe:
            validate_record(observation_probe, checks)
        checks.require(
            "baseline_decision_parity:retained_probe_records_replay_claim",
            bool(timing_probe)
            and bool(observation_probe)
            and nested(timing_probe, "identity", "measurement_kind") == "timing"
            and nested(observation_probe, "identity", "measurement_kind")
            == "observation"
            and nested(timing_probe, "provenance", "source_role")
            == "pristine_baseline_timing"
            and nested(observation_probe, "provenance", "source_role")
            == "pristine_baseline_observation"
            and nested(
                timing_probe, "provenance", "runner_environment", "purpose"
            )
            == "parity-probe"
            and nested(
                observation_probe,
                "provenance",
                "runner_environment",
                "purpose",
            )
            == "parity-probe"
            and nested(timing_probe, "identity", "run_id")
            == parity_artifact.get("timing_probe_run_id")
            and nested(observation_probe, "identity", "run_id")
            == parity_artifact.get("observation_probe_run_id")
            and nested(timing_probe, "identity", "process_instance_id")
            == parity_artifact.get("timing_probe_process_instance_id")
            and nested(observation_probe, "identity", "process_instance_id")
            == parity_artifact.get("observation_probe_process_instance_id")
            and nested(timing_probe, "identity", "binary_sha256")
            == parity_artifact.get("timing_binary_sha256")
            and nested(observation_probe, "identity", "binary_sha256")
            == parity_artifact.get("observation_binary_sha256")
            and baseline_fixture_identity(timing_probe)
            == parity_artifact.get("fixture_identity")
            and baseline_fixture_identity(observation_probe)
            == parity_artifact.get("fixture_identity")
            and normalized_pair_output(timing_probe)
            == parity_artifact.get("timing_output")
            and normalized_pair_output(observation_probe)
            == parity_artifact.get("observation_output")
            and baseline_decision_profile(timing_probe)
            == baseline_decision_profile(observation_probe),
        )

    if baseline_present:
        baseline_timing_records = groups[(BASELINE_ARM, "timing")]
        baseline_observation_records = groups[(BASELINE_ARM, "observation")]
        timing_binary_hashes = {
            nested(record, "identity", "binary_sha256")
            for record in baseline_timing_records
        }
        observation_binary_hashes = {
            nested(record, "identity", "binary_sha256")
            for record in baseline_observation_records
        }
        timing_source_identities = {
            json.dumps(
                nested(record, "identity", "source_identity"),
                sort_keys=True,
                separators=(",", ":"),
            )
            for record in baseline_timing_records
        }
        observation_source_identities = {
            json.dumps(
                nested(record, "identity", "source_identity"),
                sort_keys=True,
                separators=(",", ":"),
            )
            for record in baseline_observation_records
        }
        baseline_specs_identities = {
            json.dumps(
                nested(record, "identity", "specs_source_identity"),
                sort_keys=True,
                separators=(",", ":"),
            )
            for record in (*baseline_timing_records, *baseline_observation_records)
        }
        baseline_specs_contract_identities = {
            json.dumps(
                nested(record, "identity", "specs_contract_identity"),
                sort_keys=True,
                separators=(",", ":"),
            )
            for record in (*baseline_timing_records, *baseline_observation_records)
        }
        baseline_fixture_identities = {
            json.dumps(
                baseline_fixture_identity(record),
                sort_keys=True,
                separators=(",", ":"),
            )
            for record in (
                *baseline_timing_records,
                *baseline_observation_records,
            )
        }
        direct_decision_parity = all(
            baseline_decision_profile(timing)
            == baseline_decision_profile(observation)
            for timing in baseline_timing_records
            for observation in baseline_observation_records
        )
        baseline_pair_outputs = {
            json.dumps(
                normalized_pair_output(record),
                sort_keys=True,
                separators=(",", ":"),
            )
            for record in (
                *baseline_timing_records,
                *baseline_observation_records,
            )
        }
        checks.require(
            "baseline_decision_parity:single_binary_per_kind",
            len(timing_binary_hashes) == 1
            and len(observation_binary_hashes) == 1,
        )
        checks.require(
            "baseline_decision_parity:artifact_binds_supplied_binaries",
            parity_artifact is not None
            and parity_artifact.get("timing_binary_sha256")
            in timing_binary_hashes
            and parity_artifact.get("observation_binary_sha256")
            in observation_binary_hashes,
        )
        checks.require(
            "baseline_decision_parity:artifact_binds_source_identities",
            parity_artifact is not None
            and len(timing_source_identities) == 1
            and len(observation_source_identities) == 1
            and len(baseline_specs_identities) == 1
            and len(baseline_specs_contract_identities) == 1
            and json.dumps(
                parity_artifact.get("timing_source_identity"),
                sort_keys=True,
                separators=(",", ":"),
            )
            in timing_source_identities
            and json.dumps(
                parity_artifact.get("observation_source_identity"),
                sort_keys=True,
                separators=(",", ":"),
            )
            in observation_source_identities
            and json.dumps(
                parity_artifact.get("specs_source_identity"),
                sort_keys=True,
                separators=(",", ":"),
            )
            in baseline_specs_identities
            and json.dumps(
                parity_artifact.get("specs_contract_identity"),
                sort_keys=True,
                separators=(",", ":"),
            )
            in baseline_specs_contract_identities,
        )
        checks.require(
            "baseline_decision_parity:artifact_binds_fixture_inputs",
            parity_artifact is not None
            and len(baseline_fixture_identities) == 1
            and json.dumps(
                parity_artifact.get("fixture_identity"),
                sort_keys=True,
                separators=(",", ":"),
            )
            in baseline_fixture_identities,
        )
        checks.require(
            "baseline_decision_parity:artifact_binds_normalized_pair_outputs",
            parity_artifact is not None
            and len(baseline_pair_outputs) == 1
            and json.dumps(
                parity_artifact.get("timing_output"),
                sort_keys=True,
                separators=(",", ":"),
            )
            in baseline_pair_outputs
            and json.dumps(
                parity_artifact.get("observation_output"),
                sort_keys=True,
                separators=(",", ":"),
            )
            in baseline_pair_outputs,
        )
        checks.require(
            "baseline_decision_parity:recomputed_status_reason_exact32",
            direct_decision_parity,
        )

    updated_records = [
        record
        for arm in UPDATED_ARMS
        for kind in MEASUREMENT_KINDS
        for record in groups[(arm, kind)]
    ]
    if updated_records:
        for record in updated_records:
            checks.require(
                f"{record_label(record)}:updated_role_not_baseline",
                nested(record, "provenance", "source_role")
                == "current_candidate"
                and nested(record, "provenance", "qualification_patch_sha256")
                is None
                and nested(
                    record,
                    "provenance",
                    "clean_checkout_manifest_sha256",
                )
                is None
                and nested(record, "provenance", "observation_patch_sha256")
                is None
                and nested(
                    record, "provenance", "pristine_decision_parity_sha256"
                )
                is None,
            )
        candidate_ids = {
            (
                json.dumps(
                    nested(record, "identity", "source_identity"),
                    sort_keys=True,
                    separators=(",", ":"),
                ),
                json.dumps(
                    nested(record, "identity", "specs_source_identity"),
                    sort_keys=True,
                    separators=(",", ":"),
                ),
                json.dumps(
                    nested(record, "identity", "specs_contract_identity"),
                    sort_keys=True,
                    separators=(",", ":"),
                ),
                nested(record, "identity", "image_id"),
                nested(record, "identity", "compiler"),
            )
            for record in updated_records
        }
        checks.require("updated:single_candidate_identity", len(candidate_ids) == 1)

    expected_wrapper_hash = tool_hashes["runtime_artifact_wrapper"]
    expected_identity_hash = tool_hashes["source_identity_helper"]
    expected_runner_hashes = {
        "current_candidate": tool_hashes["current_runtime_runner"],
        "pristine_baseline_timing": tool_hashes["baseline_runtime_runner"],
        "pristine_baseline_observation": tool_hashes[
            "baseline_runtime_runner"
        ],
    }
    expected_prepare_hash = tool_hashes["baseline_runtime_prepare"]
    for record in (*records, *parity_probe_records.values()):
        role = nested(record, "provenance", "source_role")
        checks.require(
            f"{record_label(record)}:qualification_tools_pinned",
            nested(record, "provenance", "wrapper_script_sha256")
            == expected_wrapper_hash
            and nested(record, "provenance", "source_identity_script_sha256")
            == expected_identity_hash
            and nested(record, "provenance", "runner_script_sha256")
            == expected_runner_hashes.get(role)
            and (
                nested(record, "provenance", "prepare_script_sha256")
                == expected_prepare_hash
                if role
                in {
                    "pristine_baseline_timing",
                    "pristine_baseline_observation",
                }
                else nested(record, "provenance", "prepare_script_sha256")
                is None
            )
            and (
                role == "current_candidate"
                or (
                    nested(
                        record,
                        "provenance",
                        "runner_environment",
                        "harness_source_sha256",
                    )
                    == tool_hashes["runtime_harness_source"]
                    and nested(
                        record,
                        "provenance",
                        "runner_environment",
                        "cmake_patch_sha256",
                    )
                    == tool_hashes["baseline_runtime_cmake_patch"]
                    and nested(
                        record,
                        "provenance",
                        "runner_environment",
                        "baseline_compat_patch_sha256",
                    )
                    == tool_hashes["baseline_runtime_compat_patch"]
                )
            ),
        )

    comparable_records = list(updated_records)
    if baseline_present:
        comparable_records.extend(
            record
            for kind in MEASUREMENT_KINDS
            for record in groups[(BASELINE_ARM, kind)]
        )
    signatures = {fixture_signature(record) for record in comparable_records}
    checks.require(
        "matched_fixture_and_schedule_identity",
        bool(comparable_records) and len(signatures) == 1,
        f"distinct_signatures={len(signatures)}",
    )
    normalized_outputs = {
        json.dumps(
            normalized_pair_output(record),
            sort_keys=True,
            separators=(",", ":"),
        )
        for record in comparable_records
    }
    checks.require(
        "matched_baseline_current_normalized_collision_output",
        bool(comparable_records)
        and "null" not in normalized_outputs
        and len(normalized_outputs) == 1,
        f"distinct_outputs={len(normalized_outputs)}",
    )
    execution_environments = {
        (
            nested(record, "identity", "image_id"),
            nested(record, "identity", "compiler"),
            nested(record, "config", "requested_cpu_affinity"),
            nested(record, "config", "effective_cpu_affinity"),
            nested(record, "config", "process_scheduler_policy"),
            nested(record, "config", "cpu_governor_set"),
            nested(record, "config", "cpu_frequency_driver_set"),
            nested(record, "config", "kernel_id"),
            nested(record, "config", "compiler_flags"),
            nested(record, "config", "build_type"),
        )
        for record in comparable_records
    }
    checks.require(
        "matched_container_compiler_affinity_scheduler_and_host",
        bool(comparable_records) and len(execution_environments) == 1,
        f"distinct_environments={len(execution_environments)}",
    )

    for arm in (BASELINE_ARM, *UPDATED_ARMS):
        timing = groups[(arm, "timing")]
        observation = groups[(arm, "observation")]
        if not timing or not observation:
            continue
        timing_binaries = {
            nested(record, "identity", "binary_sha256") for record in timing
        }
        observation_binaries = {
            nested(record, "identity", "binary_sha256")
            for record in observation
        }
        checks.require(
            f"{arm}:separate_timing_observation_binaries",
            bool(timing_binaries)
            and bool(observation_binaries)
            and timing_binaries.isdisjoint(observation_binaries),
        )

    if baseline_present:
        baseline_observation = groups[(BASELINE_ARM, "observation")]
        for arm in UPDATED_ARMS:
            current_observation = groups[(arm, "observation")]
            for surface in ("immediate", "lookahead"):
                for field in (
                    "collision_api_calls",
                    "active_collision_pairs",
                    "colliding_geometry_pairs",
                    "colliding_logical_pairs",
                ):
                    baseline_values = {
                        normalized_mechanism(record, surface, field)
                        for record in baseline_observation
                    }
                    current_values = {
                        normalized_mechanism(record, surface, field)
                        for record in current_observation
                    }
                    normalized_equal = all(
                        left_num * right_den == right_num * left_den
                        for left_num, left_den in baseline_values
                        for right_num, right_den in current_values
                        if left_den > 0 and right_den > 0
                    )
                    checks.require(
                        f"{arm}:{surface}:baseline_full_enumeration:{field}",
                        bool(baseline_values)
                        and bool(current_values)
                        and normalized_equal,
                    )

    performance: dict[str, Any] = {}
    before_after_measured = baseline_present
    if baseline_present:
        baseline_timing = groups[(BASELINE_ARM, "timing")]
        baseline_metrics = {
            surface: median_p95(baseline_timing, surface)
            for surface in ("immediate", "lookahead")
        }
        performance[BASELINE_ARM] = {
            surface: finite_json_number(value)
            for surface, value in baseline_metrics.items()
        }
        for arm in UPDATED_ARMS:
            arm_metrics: dict[str, Any] = {}
            for surface in ("immediate", "lookahead"):
                current = median_p95(groups[(arm, "timing")], surface)
                baseline = baseline_metrics[surface]
                reduction = (
                    1.0 - current / baseline
                    if math.isfinite(current)
                    and math.isfinite(baseline)
                    and baseline > 0
                    else math.nan
                )
                arm_metrics[surface] = {
                    "median_process_p95_ns": finite_json_number(current),
                    "reduction_fraction_vs_pristine": finite_json_number(reduction),
                    "required_reduction_fraction": CPU_REDUCTION_GATES[arm],
                }
                checks.require(
                    f"{arm}:{surface}:cpu_p95_reduction",
                    math.isfinite(reduction)
                    and reduction >= CPU_REDUCTION_GATES[arm],
                )
            performance[arm] = arm_metrics

    require_current_candidate_checkout(records, checks, repository_root)
    require_artifacts_unchanged(artifact_manifest, checks)
    require_artifacts_unchanged(
        parity_probe_manifest, checks, "retained_parity_probe_unchanged"
    )
    require_artifacts_unchanged(
        tool_manifest, checks, "retained_tool_unchanged"
    )
    available_checks_passed = not checks.failures
    runtime_schedule_subgate_passed = (
        available_checks_passed and before_after_measured
    )
    candidate_source_identities = {
        json.dumps(
            nested(record, "identity", "source_identity"),
            sort_keys=True,
            separators=(",", ":"),
        )
        for record in updated_records
    }
    candidate_specs_identities = {
        json.dumps(
            nested(record, "identity", "specs_source_identity"),
            sort_keys=True,
            separators=(",", ":"),
        )
        for record in updated_records
    }
    candidate_specs_contract_identities = {
        json.dumps(
            nested(record, "identity", "specs_contract_identity"),
            sort_keys=True,
            separators=(",", ":"),
        )
        for record in updated_records
    }
    summary = {
        "schema_version": SUMMARY_SCHEMA,
        "artifact_manifest": artifact_manifest,
        "parity_probe_manifest": parity_probe_manifest,
        "tool_manifest": tool_manifest,
        "baseline_provenance_hashes": auxiliary_hashes,
        "record_count": len(records),
        "minimum_runs": args.minimum_runs,
        "formal_profile": FORMAL_PROFILE,
        "formal_bounded_threshold_m": FORMAL_BOUNDED_THRESHOLD_M,
        "baseline_required": not args.allow_missing_baseline,
        "genuine_pristine_baseline_present": baseline_present,
        "before_after_measured": before_after_measured,
        "performance": performance,
        "checks": checks.items,
        "failure_count": len(checks.failures),
        "available_checks_passed": available_checks_passed,
        "runtime_schedule_subgate_passed": runtime_schedule_subgate_passed,
        "candidate_source_identity": (
            json.loads(next(iter(candidate_source_identities)))
            if len(candidate_source_identities) == 1
            else None
        ),
        "candidate_specs_source_identity": (
            json.loads(next(iter(candidate_specs_identities)))
            if len(candidate_specs_identities) == 1
            else None
        ),
        "candidate_specs_contract_identity": (
            json.loads(next(iter(candidate_specs_contract_identities)))
            if len(candidate_specs_contract_identities) == 1
            else None
        ),
        "verifier_script_sha256": tool_hashes["runtime_schedule_verifier"],
        "overall_step18_closure_claimed": False,
        "requires_common_candidate_aggregator": True,
        "closure_note": (
            "This verifier covers only the isolated scheduled-runtime row; "
            "a common-candidate aggregator plus external controller/viewer/"
            "Isaac/live gates is required for Step 18 closure."
        ),
    }
    rendered = json.dumps(summary, indent=2, sort_keys=True, allow_nan=False)
    try:
        print(rendered)
    except BrokenPipeError:
        return 2
    if summary_destination is not None:
        try:
            publish_summary(
                summary_destination,
                (rendered + "\n").encode("utf-8"),
                records,
                repository_root,
                artifact_manifest,
                parity_probe_manifest,
                tool_manifest,
            )
        except (OSError, ValueError) as error:
            print(f"runtime summary write failed: {error}", file=sys.stderr)
            return 2
    if checks.failures:
        return 1
    if not runtime_schedule_subgate_passed and not args.allow_missing_baseline:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
